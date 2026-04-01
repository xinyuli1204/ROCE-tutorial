//
// bench_server.cpp — Multi-connection benchmark server (ConnectX-7)
//
// Each accepted connection is migrated to its own event channel and
// handled in a dedicated thread — allows concurrent background flows
// to run alongside the measured flow simultaneously.
//
// Supports GPUDirect RDMA: pass --gpu <device-id> to allocate the
// receive buffer in GPU memory so the NIC DMAs directly into the GPU.
//
// Usage:
//   ./bench_server [--gpu <device-id>] [bind-addr]
//
// Examples:
//   ./bench_server                   # CPU memory (default)
//   ./bench_server --gpu 0           # GPU0, bind 0.0.0.0
//   ./bench_server --gpu 0 10.0.2.2  # GPU0, bind specific IP
//

#include "common.hpp"
#include <signal.h>
#include <thread>
#include <atomic>

#ifdef HAVE_CUDA
#include <cuda_runtime.h>
#define CUDA_CHECK(call)                                                    \
    do {                                                                    \
        cudaError_t _e = (call);                                            \
        if (_e != cudaSuccess)                                              \
            throw std::runtime_error(std::string("CUDA: ") +               \
                                     cudaGetErrorString(_e));               \
    } while (0)
#endif

static constexpr size_t BENCH_BUF = 4 * 1024 * 1024;  // 4 MB

static volatile bool     g_running     = true;
static std::atomic<int>  g_active_conn {0};
static std::atomic<int>  g_total_conn  {0};
static int               g_gpu_id      = -1;   // -1 = CPU mode

static void sig_handler(int) { g_running = false; }

// ── Per-connection handler (runs in its own thread) ──────────────────────────
static void handle_connection(rdma_cm_id *cm_id,
                               rdma_event_channel *conn_ec,
                               int conn_id) {
    g_active_conn++;
    fprintf(stderr, "[SERVER] conn#%d started  (active=%d, mode=%s)\n",
            conn_id, g_active_conn.load(),
            g_gpu_id >= 0 ? "GPUDirect" : "CPU");

    void   *raw_buf  = nullptr;
    bool    gpu_buf  = false;
    ibv_pd  *pd      = nullptr;
    ibv_mr  *mr      = nullptr;
    ibv_mr  *recv_mr = nullptr;
    ibv_cq  *cq      = nullptr;
    char     recv_buf[16]{};

    try {
        // ── Allocate buffer ──────────────────────────────────────────────
#ifdef HAVE_CUDA
        if (g_gpu_id >= 0) {
            CUDA_CHECK(cudaSetDevice(g_gpu_id));
            CUDA_CHECK(cudaMalloc(&raw_buf, BENCH_BUF));
            CUDA_CHECK(cudaMemset(raw_buf, 0, BENCH_BUF));
            gpu_buf = true;
        } else
#endif
        {
            raw_buf = aligned_alloc(4096, BENCH_BUF);
            if (!raw_buf) throw std::runtime_error("aligned_alloc");
            memset(raw_buf, 0, BENCH_BUF);
        }

        char *buf = static_cast<char *>(raw_buf);

        pd = ibv_alloc_pd(cm_id->verbs);
        if (!pd) throw std::runtime_error("ibv_alloc_pd");

        // ibv_reg_mr works on GPU pointers when nvidia_peermem is loaded
        mr = ibv_reg_mr(pd, buf, BENCH_BUF,
                        IBV_ACCESS_LOCAL_WRITE  |
                        IBV_ACCESS_REMOTE_WRITE |
                        IBV_ACCESS_REMOTE_READ);
        if (!mr) throw std::runtime_error(
            g_gpu_id >= 0
            ? "ibv_reg_mr failed — check: sudo modprobe nvidia_peermem"
            : "ibv_reg_mr failed");

        cq = ibv_create_cq(cm_id->verbs, CQ_DEPTH, nullptr, nullptr, 0);
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

        // Pre-post RECV for client DONE signal (always CPU memory — small control msg)
        recv_mr = ibv_reg_mr(pd, recv_buf, sizeof(recv_buf), IBV_ACCESS_LOCAL_WRITE);
        if (!recv_mr) throw std::runtime_error("ibv_reg_mr recv");

        ibv_sge sge{};
        sge.addr   = reinterpret_cast<uintptr_t>(recv_buf);
        sge.length = sizeof(recv_buf);
        sge.lkey   = recv_mr->lkey;

        ibv_recv_wr rwr{};
        rwr.wr_id   = 99;
        rwr.sg_list = &sge;
        rwr.num_sge = 1;
        ibv_recv_wr *bad = nullptr;
        if (ibv_post_recv(cm_id->qp, &rwr, &bad))
            throw std::runtime_error("ibv_post_recv");

        // ── Accept — send MR info in private_data ────────────────────────
        mr_info_t my_mr{ reinterpret_cast<uint64_t>(buf), mr->rkey };
        rdma_conn_param cp{};
        cp.private_data        = &my_mr;
        cp.private_data_len    = sizeof(my_mr);
        cp.initiator_depth     = 1;
        cp.responder_resources = 1;
        cp.rnr_retry_count     = 7;
        if (rdma_accept(cm_id, &cp))
            throw std::runtime_error("rdma_accept");

        // ── Wait for ESTABLISHED ─────────────────────────────────────────
        rdma_cm_event *ev = nullptr;
        if (rdma_get_cm_event(conn_ec, &ev))
            throw std::runtime_error("get ESTABLISHED");
        rdma_ack_cm_event(ev);

        // ── Poll for client DONE (IBV_WC_RECV) ──────────────────────────
        bool done = false;
        while (!done) {
            ibv_wc wc[16];
            int n = ibv_poll_cq(cq, 16, wc);
            for (int i = 0; i < n; ++i) {
                if (wc[i].status != IBV_WC_SUCCESS)
                    fprintf(stderr, "[SERVER] conn#%d WC error: %s\n",
                            conn_id, ibv_wc_status_str(wc[i].status));
                if (wc[i].opcode == IBV_WC_RECV) done = true;
            }
        }

        // ── Graceful disconnect ──────────────────────────────────────────
        rdma_disconnect(cm_id);
        if (rdma_get_cm_event(conn_ec, &ev))
            throw std::runtime_error("get DISCONNECTED");
        rdma_ack_cm_event(ev);

    } catch (const std::exception &e) {
        fprintf(stderr, "[SERVER] conn#%d error: %s\n", conn_id, e.what());
    }

    // ── Cleanup ───────────────────────────────────────────────────────────
    if (cm_id->qp) rdma_destroy_qp(cm_id);
    if (cq)        ibv_destroy_cq(cq);
    if (recv_mr)   ibv_dereg_mr(recv_mr);
    if (mr)        ibv_dereg_mr(mr);
    if (pd)        ibv_dealloc_pd(pd);

    if (raw_buf) {
#ifdef HAVE_CUDA
        if (gpu_buf) cudaFree(raw_buf);
        else
#endif
        free(raw_buf);
    }

    rdma_destroy_id(cm_id);
    rdma_destroy_event_channel(conn_ec);

    int active = --g_active_conn;
    fprintf(stderr, "[SERVER] conn#%d done  (active=%d)\n", conn_id, active);
}

// ── Main ─────────────────────────────────────────────────────────────────────
int main(int argc, char *argv[]) {
    signal(SIGINT, sig_handler);

    const char *bind_ip = "0.0.0.0";

    // Parse: [--gpu <id>] [bind-addr]
    for (int i = 1; i < argc; i++) {
        if (std::string(argv[i]) == "--gpu" && i + 1 < argc) {
            g_gpu_id = std::stoi(argv[++i]);
        } else {
            bind_ip = argv[i];
        }
    }

#ifdef HAVE_CUDA
    if (g_gpu_id >= 0) {
        int dev_count = 0;
        cudaGetDeviceCount(&dev_count);
        if (g_gpu_id >= dev_count) {
            fprintf(stderr, "ERROR: GPU %d not found (%d GPUs available)\n",
                    g_gpu_id, dev_count);
            return 1;
        }
        cudaDeviceProp prop{};
        cudaGetDeviceProperties(&prop, g_gpu_id);
        printf("[SERVER] GPUDirect mode: GPU%d = %s (%zu MB)\n",
               g_gpu_id, prop.name, prop.totalGlobalMem / 1024 / 1024);
    }
#else
    if (g_gpu_id >= 0) {
        fprintf(stderr, "ERROR: --gpu requires CUDA build. Compile with -DHAVE_CUDA\n");
        return 1;
    }
#endif

    printf("╔══════════════════════════════════════════════════╗\n");
    printf("║  RDMA Benchmark Server  (multi-connection)       ║\n");
    printf("║  Buffer: %-38s  ║\n",
           g_gpu_id >= 0 ? "GPU memory (GPUDirect RDMA)" : "CPU memory");
    printf("╚══════════════════════════════════════════════════╝\n\n");

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

    if (rdma_listen(listen_id, 64))
        throw std::runtime_error("rdma_listen");

    printf("[SERVER] Listening on %s:%u\n", bind_ip, DEFAULT_PORT);
    printf("[SERVER] Ctrl+C to stop\n\n");

    while (g_running) {
        rdma_cm_event *ev = nullptr;
        if (rdma_get_cm_event(ec, &ev)) break;

        if (ev->event != RDMA_CM_EVENT_CONNECT_REQUEST) {
            rdma_ack_cm_event(ev);
            continue;
        }

        rdma_cm_id *cm_id = ev->id;
        rdma_ack_cm_event(ev);

        // Migrate to per-connection event channel so main thread stays free
        rdma_event_channel *conn_ec = rdma_create_event_channel();
        if (!conn_ec) {
            fprintf(stderr, "[SERVER] rdma_create_event_channel failed\n");
            rdma_destroy_id(cm_id);
            continue;
        }
        if (rdma_migrate_id(cm_id, conn_ec)) {
            fprintf(stderr, "[SERVER] rdma_migrate_id failed\n");
            rdma_destroy_event_channel(conn_ec);
            rdma_destroy_id(cm_id);
            continue;
        }

        int conn_id = ++g_total_conn;
        std::thread(handle_connection, cm_id, conn_ec, conn_id).detach();
    }

    rdma_destroy_id(listen_id);
    rdma_destroy_event_channel(ec);
    printf("[SERVER] Shutdown. Total connections served: %d\n",
           g_total_conn.load());
    return 0;
}
