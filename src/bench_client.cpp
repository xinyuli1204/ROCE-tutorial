//
// bench_client.cpp — Benchmark client for PFC-free RoCE study (ConnectX-7)
//
// Measures RDMA WRITE latency and throughput under configurable message sizes.
// Designed to be called by run_experiment.sh in a sweep over loss rates.
//
// Usage:
//   ./bench_client <server-ip> <msg-size-bytes> <iterations> [client-ip]
//
// Output (CSV line to stdout — one line per run, captured by the script):
//   throughput_gbps,p50_us,p99_us,p999_us,retries
//

#include "common.hpp"
#include <chrono>
#include <vector>
#include <algorithm>
#include <numeric>
#include <fstream>
#include <sstream>

using clk    = std::chrono::steady_clock;
using ns     = std::chrono::nanoseconds;
using micros = std::chrono::microseconds;

static constexpr int WARMUP_ITERS = 1000;
static constexpr size_t BENCH_BUF = 4 * 1024 * 1024;  // 4 MB

// ── Poll CQ for exactly one send completion ──────────────────────────────────
static void poll_one(ibv_cq *cq) {
    ibv_wc wc{};
    while (ibv_poll_cq(cq, 1, &wc) == 0) {}
    if (wc.status != IBV_WC_SUCCESS)
        throw std::runtime_error(std::string("WC error: ") +
                                 ibv_wc_status_str(wc.status));
}

// ── Post one RDMA WRITE and return immediately ───────────────────────────────
static void post_write(ibv_qp *qp, uint64_t local_addr, uint32_t lkey,
                       uint64_t remote_addr, uint32_t rkey,
                       uint32_t length, bool signaled, uint64_t wr_id) {
    ibv_sge sge{};
    sge.addr   = local_addr;
    sge.length = length;
    sge.lkey   = lkey;

    ibv_send_wr wr{};
    wr.wr_id               = wr_id;
    wr.opcode              = IBV_WR_RDMA_WRITE;
    wr.sg_list             = &sge;
    wr.num_sge             = 1;
    wr.send_flags          = signaled ? IBV_SEND_SIGNALED : 0;
    wr.wr.rdma.remote_addr = remote_addr;
    wr.wr.rdma.rkey        = rkey;

    ibv_send_wr *bad = nullptr;
    if (ibv_post_send(qp, &wr, &bad))
        throw std::runtime_error("ibv_post_send");
}

// ── Read a sysfs counter ─────────────────────────────────────────────────────
static uint64_t read_counter(const std::string &path) {
    std::ifstream f(path);
    if (!f.is_open()) return 0;
    uint64_t val = 0;
    f >> val;
    return val;
}

// ── Detect IB device from cm_id and return sysfs hw_counters base path ───────
static std::string hw_counters_path(rdma_cm_id *cm_id) {
    if (!cm_id->verbs) return "";
    std::string dev_name = cm_id->verbs->device->name;
    return "/sys/class/infiniband/" + dev_name + "/ports/1/hw_counters";
}

static std::string port_counters_path(rdma_cm_id *cm_id) {
    if (!cm_id->verbs) return "";
    std::string dev_name = cm_id->verbs->device->name;
    return "/sys/class/infiniband/" + dev_name + "/ports/1/counters";
}

int main(int argc, char *argv[]) {
    if (argc < 4) {
        fprintf(stderr,
                "Usage: %s <server-ip> <msg-size-bytes> <iterations> [client-ip]\n",
                argv[0]);
        return 1;
    }

    const char   *server_ip  = argv[1];
    const size_t  msg_size   = static_cast<size_t>(std::stoul(argv[2]));
    const int     iterations = std::stoi(argv[3]);
    const char   *client_ip  = (argc >= 5) ? argv[4] : nullptr;

    if (msg_size > BENCH_BUF) {
        fprintf(stderr, "msg_size %zu exceeds BENCH_BUF %zu\n", msg_size, BENCH_BUF);
        return 1;
    }
    if (iterations < 1) {
        fprintf(stderr, "iterations must be >= 1\n");
        return 1;
    }

    fprintf(stderr, "[CLIENT] msg_size=%zu bytes  iterations=%d\n",
            msg_size, iterations);

    // ── 1. Create event channel + active ID ─────────────────────────────────
    rdma_event_channel *ec = rdma_create_event_channel();
    if (!ec) throw std::runtime_error("rdma_create_event_channel");

    rdma_cm_id *cm_id = nullptr;
    if (rdma_create_id(ec, &cm_id, nullptr, RDMA_PS_TCP))
        throw std::runtime_error("rdma_create_id");

    // ── 2. Resolve server address ────────────────────────────────────────────
    sockaddr_in server_addr{};
    server_addr.sin_family = AF_INET;
    server_addr.sin_port   = htons(DEFAULT_PORT);
    if (inet_pton(AF_INET, server_ip, &server_addr.sin_addr) != 1)
        throw std::runtime_error("invalid server IP");

    if (client_ip) {
        sockaddr_in client_addr{};
        client_addr.sin_family      = AF_INET;
        client_addr.sin_port        = 0;
        if (inet_pton(AF_INET, client_ip, &client_addr.sin_addr) != 1)
            throw std::runtime_error("invalid client IP");
        if (rdma_resolve_addr(cm_id,
                              reinterpret_cast<sockaddr *>(&client_addr),
                              reinterpret_cast<sockaddr *>(&server_addr), 2000))
            throw std::runtime_error("rdma_resolve_addr");
    } else {
        if (rdma_resolve_addr(cm_id, nullptr,
                              reinterpret_cast<sockaddr *>(&server_addr), 2000))
            throw std::runtime_error("rdma_resolve_addr");
    }

    rdma_cm_event *ev = nullptr;
    if (rdma_get_cm_event(ec, &ev)) throw std::runtime_error("get ADDR_RESOLVED");
    if (ev->event != RDMA_CM_EVENT_ADDR_RESOLVED) throw std::runtime_error("expected ADDR_RESOLVED");
    rdma_ack_cm_event(ev);

    // ── 3. Resolve route ─────────────────────────────────────────────────────
    if (rdma_resolve_route(cm_id, 2000)) throw std::runtime_error("rdma_resolve_route");
    if (rdma_get_cm_event(ec, &ev)) throw std::runtime_error("get ROUTE_RESOLVED");
    if (ev->event != RDMA_CM_EVENT_ROUTE_RESOLVED) throw std::runtime_error("expected ROUTE_RESOLVED");
    rdma_ack_cm_event(ev);

    // ── 4. Allocate + register resources ─────────────────────────────────────
    char *buf = static_cast<char *>(aligned_alloc(4096, BENCH_BUF));
    if (!buf) throw std::runtime_error("aligned_alloc");
    memset(buf, 0xAB, BENCH_BUF);  // fill with pattern

    ibv_pd *pd = ibv_alloc_pd(cm_id->verbs);
    if (!pd) throw std::runtime_error("ibv_alloc_pd");

    ibv_mr *mr = ibv_reg_mr(pd, buf, BENCH_BUF,
                            IBV_ACCESS_LOCAL_WRITE  |
                            IBV_ACCESS_REMOTE_READ  |
                            IBV_ACCESS_REMOTE_WRITE);
    if (!mr) throw std::runtime_error("ibv_reg_mr");

    ibv_cq *cq = ibv_create_cq(cm_id->verbs, CQ_DEPTH, nullptr, nullptr, 0);
    if (!cq) throw std::runtime_error("ibv_create_cq");

    ibv_qp_init_attr qp_attr{};
    qp_attr.send_cq          = cq;
    qp_attr.recv_cq          = cq;
    qp_attr.qp_type          = IBV_QPT_RC;
    qp_attr.cap.max_send_wr  = QP_MAX_WR;
    qp_attr.cap.max_recv_wr  = QP_MAX_WR;
    qp_attr.cap.max_send_sge = QP_MAX_SGE;
    qp_attr.cap.max_recv_sge = QP_MAX_SGE;
    if (rdma_create_qp(cm_id, pd, &qp_attr))
        throw std::runtime_error("rdma_create_qp");

    // ── 5. Connect — send our MR in private_data ─────────────────────────────
    mr_info_t my_mr{ reinterpret_cast<uint64_t>(buf), mr->rkey };
    rdma_conn_param cp{};
    cp.private_data        = &my_mr;
    cp.private_data_len    = sizeof(my_mr);
    cp.initiator_depth     = 1;
    cp.responder_resources = 1;
    cp.retry_count         = 7;
    cp.rnr_retry_count     = 7;
    if (rdma_connect(cm_id, &cp)) throw std::runtime_error("rdma_connect");

    // ── 6. ESTABLISHED — extract server MR ───────────────────────────────────
    if (rdma_get_cm_event(ec, &ev)) throw std::runtime_error("get ESTABLISHED");
    if (ev->event != RDMA_CM_EVENT_ESTABLISHED) throw std::runtime_error("expected ESTABLISHED");
    mr_info_t server_mr{};
    if (ev->param.conn.private_data &&
        ev->param.conn.private_data_len >= sizeof(mr_info_t))
        memcpy(&server_mr, ev->param.conn.private_data, sizeof(mr_info_t));
    rdma_ack_cm_event(ev);

    fprintf(stderr, "[CLIENT] Connected. Warming up...\n");

    // ── 7. Read NIC counters BEFORE benchmark ─────────────────────────────────
    std::string hw_path   = hw_counters_path(cm_id);
    std::string port_path = port_counters_path(cm_id);

    uint64_t cnp_before   = read_counter(hw_path + "/rp_cnp_handled");
    uint64_t retry_before = read_counter(port_path + "/local_ack_timeout_err");
    uint64_t err_before   = read_counter(port_path + "/port_rcv_errors");

    // ── 8. Warmup ─────────────────────────────────────────────────────────────
    for (int i = 0; i < WARMUP_ITERS; ++i) {
        post_write(cm_id->qp,
                   reinterpret_cast<uint64_t>(buf), mr->lkey,
                   server_mr.addr, server_mr.rkey,
                   static_cast<uint32_t>(msg_size), true, 0);
        poll_one(cq);
    }

    fprintf(stderr, "[CLIENT] Warmup done. Measuring %d iterations...\n", iterations);

    // ── 9. Measurement — one WRITE at a time, record each latency ────────────
    std::vector<double> latencies_us;
    latencies_us.reserve(iterations);

    auto total_start = clk::now();

    for (int i = 0; i < iterations; ++i) {
        auto t0 = clk::now();
        post_write(cm_id->qp,
                   reinterpret_cast<uint64_t>(buf), mr->lkey,
                   server_mr.addr, server_mr.rkey,
                   static_cast<uint32_t>(msg_size), true, static_cast<uint64_t>(i));
        poll_one(cq);
        auto t1 = clk::now();

        double us = std::chrono::duration<double, std::micro>(t1 - t0).count();
        latencies_us.push_back(us);
    }

    auto total_end = clk::now();
    double total_sec = std::chrono::duration<double>(total_end - total_start).count();

    // ── 10. Read NIC counters AFTER benchmark ─────────────────────────────────
    uint64_t cnp_after   = read_counter(hw_path + "/rp_cnp_handled");
    uint64_t retry_after = read_counter(port_path + "/local_ack_timeout_err");
    uint64_t err_after   = read_counter(port_path + "/port_rcv_errors");

    uint64_t cnp_count   = cnp_after   - cnp_before;
    uint64_t retry_count = retry_after - retry_before;
    uint64_t err_count   = err_after   - err_before;

    // ── 11. Compute statistics ────────────────────────────────────────────────
    std::sort(latencies_us.begin(), latencies_us.end());

    auto percentile = [&](double p) -> double {
        size_t idx = static_cast<size_t>(p / 100.0 * latencies_us.size());
        if (idx >= latencies_us.size()) idx = latencies_us.size() - 1;
        return latencies_us[idx];
    };

    double p50   = percentile(50.0);
    double p99   = percentile(99.0);
    double p999  = percentile(99.9);
    double p9999 = percentile(99.99);

    double total_bytes   = static_cast<double>(msg_size) * iterations;
    double throughput_gbps = (total_bytes * 8.0) / (total_sec * 1e9);

    // ── 12. Send DONE signal to server ────────────────────────────────────────
    static constexpr size_t SEND_OFF = BENCH_BUF - 16;
    memcpy(buf + SEND_OFF, "DONE", 5);

    ibv_sge done_sge{};
    done_sge.addr   = reinterpret_cast<uint64_t>(buf) + SEND_OFF;
    done_sge.length = 5;
    done_sge.lkey   = mr->lkey;

    ibv_send_wr done_wr{};
    done_wr.wr_id      = 9999;
    done_wr.opcode     = IBV_WR_SEND;
    done_wr.sg_list    = &done_sge;
    done_wr.num_sge    = 1;
    done_wr.send_flags = IBV_SEND_SIGNALED;
    ibv_send_wr *bad   = nullptr;
    if (ibv_post_send(cm_id->qp, &done_wr, &bad))
        throw std::runtime_error("ibv_post_send DONE");
    poll_one(cq);

    // ── 13. Disconnect + cleanup ──────────────────────────────────────────────
    rdma_disconnect(cm_id);
    if (rdma_get_cm_event(ec, &ev)) throw std::runtime_error("get DISCONNECTED");
    rdma_ack_cm_event(ev);

    rdma_destroy_qp(cm_id);
    ibv_destroy_cq(cq);
    ibv_dereg_mr(mr);
    ibv_dealloc_pd(pd);
    free(buf);
    rdma_destroy_id(cm_id);
    rdma_destroy_event_channel(ec);

    // ── 14. Print results — CSV line to stdout ────────────────────────────────
    // Format: throughput_gbps,p50_us,p99_us,p999_us,p9999_us,cnp,retries,errors
    printf("%.6f,%.3f,%.3f,%.3f,%.3f,%lu,%lu,%lu\n",
           throughput_gbps, p50, p99, p999, p9999,
           cnp_count, retry_count, err_count);

    // Human-readable summary to stderr (not captured by script)
    fprintf(stderr, "\n[RESULTS]\n");
    fprintf(stderr, "  Throughput : %.4f Gbps\n", throughput_gbps);
    fprintf(stderr, "  Latency p50: %.3f us\n", p50);
    fprintf(stderr, "  Latency p99: %.3f us\n", p99);
    fprintf(stderr, "  Latency p999: %.3f us\n", p999);
    fprintf(stderr, "  CNP handled: %lu\n", cnp_count);
    fprintf(stderr, "  ACK timeouts: %lu\n", retry_count);
    fprintf(stderr, "  RX errors:   %lu\n", err_count);

    return 0;
}
