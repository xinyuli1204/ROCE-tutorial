//
// rdma_client.cpp — RDMA client using rdma_cm connection management
//
// Flow:
//   rdma_resolve_addr → ADDR_RESOLVED
//   → rdma_resolve_route → ROUTE_RESOLVED
//   → setup resources → rdma_connect (sends client MR info)
//   → ESTABLISHED event  (receives server MR info)
//   → Demo 1: RDMA WRITE
//   → Demo 2: RDMA READ
//   → Demo 3: SEND
//   → rdma_disconnect → DISCONNECTED → cleanup
//
// Usage:
//   ./rdma_client <server-ip>
//

#include "common.hpp"
#include <chrono>
using clk = std::chrono::steady_clock;

// Poll CQ for one completion; throws on error status.
static void cq_poll(ibv_cq *cq) {
    ibv_wc wc{};
    while (ibv_poll_cq(cq, 1, &wc) == 0) {}
    if (wc.status != IBV_WC_SUCCESS)
        throw std::runtime_error(ibv_wc_status_str(wc.status));
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <server-ip> <client-ip> [optional]\n", argv[0]);
        return 1;
    }
    const char *server_ip = argv[1];
    const char *client_ip = nullptr;
    if (argc >=3){
        client_ip = argv[2];
    }

    printf("╔══════════════════════════════════════════════╗\n");
    printf("║        RDMA Client  (rdma_cm + libibverbs)   ║\n");
    printf("╚══════════════════════════════════════════════╝\n\n");

    // ── 1. Create event channel + active ID ─────────────────────────────
    rdma_event_channel *ec = rdma_create_event_channel();
    if (!ec) throw std::runtime_error("rdma_create_event_channel");

    rdma_cm_id *cm_id = nullptr;
    if (rdma_create_id(ec, &cm_id, nullptr, RDMA_PS_TCP))
        throw std::runtime_error("rdma_create_id");

    // ── 2. Resolve server address ────────────────────────────────────────

    printf("[CLIENT] Resolving address %s:%u...\n", server_ip, DEFAULT_PORT);
    sockaddr_in server_addr{};
    server_addr.sin_family = AF_INET;
    server_addr.sin_port   = htons(DEFAULT_PORT);
    if (inet_pton(AF_INET, server_ip, &server_addr.sin_addr) != 1) {
        fprintf(stderr, "Invalid server IP: %s\n", server_ip);
        return 1;
    }
   
    if (client_ip !=nullptr){
        sockaddr_in client_addr{};
        client_addr.sin_family = AF_INET;
        client_addr.sin_port   = htons(DEFAULT_PORT);
        if (inet_pton(AF_INET, client_ip, &client_addr.sin_addr) != 1) {
            fprintf(stderr, "Invalid client IP: %s\n", client_ip);
            return 1;
        }
        if (rdma_resolve_addr(cm_id, reinterpret_cast<sockaddr *>(&client_addr),
                          reinterpret_cast<sockaddr *>(&server_addr),
                          /*timeout_ms=*/2000))
        throw std::runtime_error("rdma_resolve_addr");

    }else{
         if (rdma_resolve_addr(cm_id, nullptr,
                          reinterpret_cast<sockaddr *>(&server_addr),
                          /*timeout_ms=*/2000))
        throw std::runtime_error("rdma_resolve_addr");

    }

    rdma_cm_event *ev = nullptr;
    if (rdma_get_cm_event(ec, &ev))
        throw std::runtime_error("rdma_get_cm_event ADDR_RESOLVED");
    if (ev->event != RDMA_CM_EVENT_ADDR_RESOLVED)
        throw std::runtime_error("expected ADDR_RESOLVED");
    rdma_ack_cm_event(ev);

    // ── 3. Resolve route ─────────────────────────────────────────────────
    if (rdma_resolve_route(cm_id, /*timeout_ms=*/2000))
        throw std::runtime_error("rdma_resolve_route");

    if (rdma_get_cm_event(ec, &ev))
        throw std::runtime_error("rdma_get_cm_event ROUTE_RESOLVED");
    if (ev->event != RDMA_CM_EVENT_ROUTE_RESOLVED)
        throw std::runtime_error("expected ROUTE_RESOLVED");
    rdma_ack_cm_event(ev);
    printf("[CLIENT] Route resolved\n\n");

    // ── 4. Allocate buffer + register resources ──────────────────────────
    rdma_ctx_t ctx{};
    ctx.cm_id     = cm_id;
    ctx.data_size = BUF_SIZE;
    ctx.data      = static_cast<char *>(aligned_alloc(4096, BUF_SIZE));
    if (!ctx.data) throw std::runtime_error("aligned_alloc failed");
    memset(ctx.data, 0, BUF_SIZE);

    // Protection Domain
    ctx.pd = ibv_alloc_pd(ctx.cm_id->verbs);
    if (!ctx.pd) throw std::runtime_error("ibv_alloc_pd failed");

    // Memory Registration
    ctx.mr = ibv_reg_mr(ctx.pd, ctx.data, ctx.data_size,
                        IBV_ACCESS_LOCAL_WRITE  |
                        IBV_ACCESS_REMOTE_READ  |
                        IBV_ACCESS_REMOTE_WRITE);
    if (!ctx.mr) throw std::runtime_error("ibv_reg_mr failed");

    // Completion Queue
    ctx.cq = ibv_create_cq(ctx.cm_id->verbs, CQ_DEPTH, nullptr, nullptr, 0);
    if (!ctx.cq) throw std::runtime_error("ibv_create_cq failed");

    // Queue Pair — rdma_create_qp transitions QP to INIT automatically
    ibv_qp_init_attr qp_attr{};
    qp_attr.send_cq          = ctx.cq;
    qp_attr.recv_cq          = ctx.cq;
    qp_attr.qp_type          = IBV_QPT_RC;
    qp_attr.cap.max_send_wr  = QP_MAX_WR;
    qp_attr.cap.max_recv_wr  = QP_MAX_WR;
    qp_attr.cap.max_send_sge = QP_MAX_SGE;
    qp_attr.cap.max_recv_sge = QP_MAX_SGE;
    qp_attr.sq_sig_all       = 0;
    if (rdma_create_qp(ctx.cm_id, ctx.pd, &qp_attr))
        throw std::runtime_error("rdma_create_qp failed");

    // ── 5. Connect — send our MR info as private_data ────────────────────
    mr_info_t my_mr{ reinterpret_cast<uint64_t>(ctx.data), ctx.mr->rkey };

    rdma_conn_param cp{};
    cp.private_data        = &my_mr;
    cp.private_data_len    = sizeof(my_mr);
    cp.initiator_depth     = 1;
    cp.responder_resources = 1;
    cp.retry_count         = 7;
    cp.rnr_retry_count     = 7;

    if (rdma_connect(cm_id, &cp))
        throw std::runtime_error("rdma_connect");

    // ── 6. ESTABLISHED — extract server MR info from private_data ────────
    // rdma_cm drives QP through INIT → RTR → RTS automatically
    if (rdma_get_cm_event(ec, &ev))
        throw std::runtime_error("rdma_get_cm_event ESTABLISHED");
    if (ev->event != RDMA_CM_EVENT_ESTABLISHED)
        throw std::runtime_error("expected ESTABLISHED");

    mr_info_t server_mr{};
    if (ev->param.conn.private_data &&
        ev->param.conn.private_data_len >= sizeof(mr_info_t))
        memcpy(&server_mr, ev->param.conn.private_data, sizeof(mr_info_t));
    rdma_ack_cm_event(ev);  // ack after reading private_data

    printf("[CLIENT] Connection established — QP is RTS\n");
    printf("[CLIENT] Server MR: addr=%#lx  rkey=%#010x  size=%.1f MB\n\n",
           (unsigned long)server_mr.addr, server_mr.rkey, BUF_SIZE / 1e6);

    // ════════════════════════════════════════════════════════════════════
    //  Demo 1 — RDMA WRITE
    //  Push data directly into server memory. Server CPU is NOT woken up.
    // ════════════════════════════════════════════════════════════════════
    printf("━━━  Demo 1: RDMA WRITE  ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n");

    const char *wmsg =
        "Hello from RDMA WRITE! "
        "This landed in server memory without server CPU involvement.\n";
    size_t wlen = strlen(wmsg) + 1;
    memcpy(ctx.data, wmsg, wlen);

    {
        ibv_sge sge{};
        sge.addr   = reinterpret_cast<uint64_t>(ctx.data);
        sge.length = (uint32_t)wlen;
        sge.lkey   = ctx.mr->lkey;

        ibv_send_wr wr{};
        wr.wr_id               = 1;
        wr.opcode              = IBV_WR_RDMA_WRITE;
        wr.sg_list             = &sge;
        wr.num_sge             = 1;
        wr.send_flags          = IBV_SEND_SIGNALED;
        wr.wr.rdma.remote_addr = server_mr.addr;
        wr.wr.rdma.rkey        = server_mr.rkey;

        ibv_send_wr *bad = nullptr;
        if (ibv_post_send(cm_id->qp, &wr, &bad))
            throw std::runtime_error("ibv_post_send RDMA_WRITE");
    }
    cq_poll(ctx.cq);
    printf("[CLIENT] WRITE complete  (%zu bytes → server offset 0)\n\n", wlen);

    // ════════════════════════════════════════════════════════════════════
    //  Demo 2 — RDMA READ
    //  Pull data from server memory. Server CPU is NOT involved.
    // ════════════════════════════════════════════════════════════════════
    printf("━━━  Demo 2: RDMA READ   ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n");

    static constexpr size_t READ_OFF = 1024;
    static constexpr size_t READ_LEN = 256;
    memset(ctx.data + READ_OFF, 0, READ_LEN);

    {
        ibv_sge sge{};
        sge.addr   = reinterpret_cast<uint64_t>(ctx.data) + READ_OFF;
        sge.length = (uint32_t)READ_LEN;
        sge.lkey   = ctx.mr->lkey;

        ibv_send_wr wr{};
        wr.wr_id               = 2;
        wr.opcode              = IBV_WR_RDMA_READ;
        wr.sg_list             = &sge;
        wr.num_sge             = 1;
        wr.send_flags          = IBV_SEND_SIGNALED;
        wr.wr.rdma.remote_addr = server_mr.addr + READ_OFF;
        wr.wr.rdma.rkey        = server_mr.rkey;

        ibv_send_wr *bad = nullptr;
        if (ibv_post_send(cm_id->qp, &wr, &bad))
            throw std::runtime_error("ibv_post_send RDMA_READ");
    }
    cq_poll(ctx.cq);
    printf("[CLIENT] READ complete  (%zu bytes from server offset %zu)\n",
           READ_LEN, READ_OFF);
    printf("[CLIENT] Data: \"%.80s\"\n\n", ctx.data + READ_OFF);

    // ════════════════════════════════════════════════════════════════════
    //  Demo 3 — SEND
    //  Two-sided verb. Server pre-posted a RECV; this wakes it up.
    // ════════════════════════════════════════════════════════════════════
    printf("━━━  Demo 3: SEND        ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n");

    static constexpr size_t SEND_OFF = BUF_SIZE - 256;
    memcpy(ctx.data + SEND_OFF, "DONE", 5);

    {
        ibv_sge sge{};
        sge.addr   = reinterpret_cast<uint64_t>(ctx.data) + SEND_OFF;
        sge.length = 5;
        sge.lkey   = ctx.mr->lkey;

        ibv_send_wr wr{};
        wr.wr_id      = 3;
        wr.opcode     = IBV_WR_SEND;
        wr.sg_list    = &sge;
        wr.num_sge    = 1;
        wr.send_flags = IBV_SEND_SIGNALED;

        ibv_send_wr *bad = nullptr;
        if (ibv_post_send(cm_id->qp, &wr, &bad))
            throw std::runtime_error("ibv_post_send SEND");
    }
    cq_poll(ctx.cq);
    printf("[CLIENT] SEND complete\n\n");


    // ── Graceful disconnect ───────────────────────────────────────────────
    rdma_disconnect(cm_id);
    if (rdma_get_cm_event(ec, &ev))
        throw std::runtime_error("rdma_get_cm_event DISCONNECTED");
    if (ev->event != RDMA_CM_EVENT_DISCONNECTED)
        throw std::runtime_error("expected DISCONNECTED");
    rdma_ack_cm_event(ev);

    // ── Cleanup ───────────────────────────────────────────────────────────
    rdma_destroy_qp(cm_id);
    ibv_destroy_cq(ctx.cq);
    ibv_dereg_mr(ctx.mr);
    ibv_dealloc_pd(ctx.pd);
    free(ctx.data);
    rdma_destroy_id(cm_id);
    rdma_destroy_event_channel(ec);

    printf("[CLIENT] Done.\n");
    return 0;
}
