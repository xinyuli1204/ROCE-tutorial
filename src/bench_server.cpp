//
// bench_server.cpp — Benchmark server for PFC-free RoCE study (ConnectX-7)
//
// Loops accepting connections so run_experiment.sh can run bench_client
// repeatedly without restarting the server.
//
// Usage:
//   ./bench_server [bind-addr]        # defaults to 0.0.0.0
//

#include "common.hpp"
#include <signal.h>

static constexpr size_t BENCH_BUF = 4 * 1024 * 1024;  // 4 MB — covers all msg sizes

static volatile bool g_running = true;
static void sig_handler(int) { g_running = false; }

// ── helper: setup PD / MR / CQ / QP on a connected cm_id ───────────────────
struct bench_res_t {
    ibv_pd  *pd       = nullptr;
    ibv_mr  *mr       = nullptr;
    ibv_mr  *recv_mr  = nullptr;
    ibv_cq  *cq       = nullptr;
    char    *buf      = nullptr;
    char     recv_buf[16]{};
};

static bench_res_t setup_resources(rdma_cm_id *cm_id) {
    bench_res_t r;

    r.buf = static_cast<char *>(aligned_alloc(4096, BENCH_BUF));
    if (!r.buf) throw std::runtime_error("aligned_alloc");
    memset(r.buf, 0, BENCH_BUF);

    r.pd = ibv_alloc_pd(cm_id->verbs);
    if (!r.pd) throw std::runtime_error("ibv_alloc_pd");

    r.mr = ibv_reg_mr(r.pd, r.buf, BENCH_BUF,
                      IBV_ACCESS_LOCAL_WRITE  |
                      IBV_ACCESS_REMOTE_WRITE |
                      IBV_ACCESS_REMOTE_READ);
    if (!r.mr) throw std::runtime_error("ibv_reg_mr");

    r.cq = ibv_create_cq(cm_id->verbs, CQ_DEPTH, nullptr, nullptr, 0);
    if (!r.cq) throw std::runtime_error("ibv_create_cq");

    ibv_qp_init_attr qp_attr{};
    qp_attr.send_cq          = r.cq;
    qp_attr.recv_cq          = r.cq;
    qp_attr.qp_type          = IBV_QPT_RC;
    qp_attr.cap.max_send_wr  = QP_MAX_WR;
    qp_attr.cap.max_recv_wr  = QP_MAX_WR;
    qp_attr.cap.max_send_sge = QP_MAX_SGE;
    qp_attr.cap.max_recv_sge = QP_MAX_SGE;
    if (rdma_create_qp(cm_id, r.pd, &qp_attr))
        throw std::runtime_error("rdma_create_qp");

    // Register small buffer for the client SEND("DONE") signal
    r.recv_mr = ibv_reg_mr(r.pd, r.recv_buf, sizeof(r.recv_buf),
                           IBV_ACCESS_LOCAL_WRITE);
    if (!r.recv_mr) throw std::runtime_error("ibv_reg_mr recv");

    // Pre-post RECV before accept so we never miss the signal
    ibv_sge sge{};
    sge.addr   = reinterpret_cast<uintptr_t>(r.recv_buf);
    sge.length = sizeof(r.recv_buf);
    sge.lkey   = r.recv_mr->lkey;

    ibv_recv_wr wr{};
    wr.wr_id   = 99;
    wr.sg_list = &sge;
    wr.num_sge = 1;
    ibv_recv_wr *bad = nullptr;
    if (ibv_post_recv(cm_id->qp, &wr, &bad))
        throw std::runtime_error("ibv_post_recv");

    return r;
}

static void teardown_resources(rdma_cm_id *cm_id, bench_res_t &r) {
    rdma_destroy_qp(cm_id);
    ibv_destroy_cq(r.cq);
    ibv_dereg_mr(r.recv_mr);
    ibv_dereg_mr(r.mr);
    ibv_dealloc_pd(r.pd);
    free(r.buf);
}

int main(int argc, char *argv[]) {
    signal(SIGINT, sig_handler);

    const char *bind_ip = (argc > 1) ? argv[1] : "0.0.0.0";

    printf("╔══════════════════════════════════════════════╗\n");
    printf("║     RDMA Benchmark Server  (ConnectX-7)      ║\n");
    printf("╚══════════════════════════════════════════════╝\n\n");

    // ── Create event channel + passive listening ID ──────────────────────
    rdma_event_channel *ec = rdma_create_event_channel();
    if (!ec) throw std::runtime_error("rdma_create_event_channel");

    rdma_cm_id *listen_id = nullptr;
    if (rdma_create_id(ec, &listen_id, nullptr, RDMA_PS_TCP))
        throw std::runtime_error("rdma_create_id");

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(DEFAULT_PORT);
    if (inet_pton(AF_INET, bind_ip, &addr.sin_addr) != 1)
        throw std::runtime_error("inet_pton");

    if (rdma_bind_addr(listen_id, reinterpret_cast<sockaddr *>(&addr)))
        throw std::runtime_error("rdma_bind_addr");

    if (rdma_listen(listen_id, 4))
        throw std::runtime_error("rdma_listen");

    printf("[SERVER] Listening on %s:%u\n", bind_ip, DEFAULT_PORT);
    printf("[SERVER] Ctrl+C to stop\n\n");

    int run = 0;

    // ── Main loop: one iteration = one client connection / test run ───────
    while (g_running) {
        rdma_cm_event *ev = nullptr;
        if (rdma_get_cm_event(ec, &ev)) break;

        if (ev->event != RDMA_CM_EVENT_CONNECT_REQUEST) {
            rdma_ack_cm_event(ev);
            continue;
        }

        rdma_cm_id *cm_id = ev->id;
        rdma_ack_cm_event(ev);

        printf("[SERVER] Run #%d — client connected\n", ++run);

        bench_res_t r = setup_resources(cm_id);

        // Accept — send our MR info in private_data
        mr_info_t my_mr{ reinterpret_cast<uint64_t>(r.buf), r.mr->rkey };
        rdma_conn_param cp{};
        cp.private_data        = &my_mr;
        cp.private_data_len    = sizeof(my_mr);
        cp.initiator_depth     = 1;
        cp.responder_resources = 1;
        cp.rnr_retry_count     = 7;
        if (rdma_accept(cm_id, &cp)) throw std::runtime_error("rdma_accept");

        // Wait for ESTABLISHED
        if (rdma_get_cm_event(ec, &ev)) break;
        rdma_ack_cm_event(ev);

        // Poll until client sends DONE signal (IBV_WC_RECV)
        bool done = false;
        while (!done) {
            ibv_wc wc[16];
            int n = ibv_poll_cq(r.cq, 16, wc);
            for (int i = 0; i < n; ++i) {
                if (wc[i].status != IBV_WC_SUCCESS) {
                    fprintf(stderr, "[SERVER] WC error: %s\n",
                            ibv_wc_status_str(wc[i].status));
                }
                if (wc[i].opcode == IBV_WC_RECV) done = true;
            }
        }

        printf("[SERVER] Run #%d done\n\n", run);

        rdma_disconnect(cm_id);
        if (rdma_get_cm_event(ec, &ev)) break;
        rdma_ack_cm_event(ev);

        teardown_resources(cm_id, r);
        rdma_destroy_id(cm_id);
    }

    rdma_destroy_id(listen_id);
    rdma_destroy_event_channel(ec);
    printf("[SERVER] Shutdown after %d runs.\n", run);
    return 0;
}
