//
// rdma_server.cpp — RDMA server using rdma_cm connection management
//
// Flow:
//   rdma_bind_addr → rdma_listen
//   → CONNECT_REQUEST event  → setup resources → rdma_accept (sends MR info)
//   → ESTABLISHED event
//   → poll for client SEND("DONE")   (RDMA WRITE/READ arrive silently)
//   → DISCONNECTED event → cleanup
//
// Usage:
//   ./rdma_server [bind-addr]        # bind-addr defaults to 0.0.0.0
//

#include "common.hpp"


int main(int argc, char *argv[]) {
    const char *bind_ip = (argc > 1) ? argv[1] : "0.0.0.0";

    printf("╔══════════════════════════════════════════════╗\n");
    printf("║        RDMA Server  (rdma_cm + libibverbs)   ║\n");
    printf("╚══════════════════════════════════════════════╝\n\n");

    // ── 1. Create event channel + passive listening ID ──────────────────
    rdma_event_channel *ec = rdma_create_event_channel();
    if (!ec) throw std::runtime_error("rdma_create_event_channel");

    rdma_cm_id *listen_id = nullptr;
    if (rdma_create_id(ec, &listen_id, nullptr, RDMA_PS_TCP))
        throw std::runtime_error("rdma_create_id");

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(DEFAULT_PORT);
    if (inet_pton(AF_INET, bind_ip, &addr.sin_addr) != 1)
        throw std::runtime_error("inet_pton: invalid bind address");

    // create bind address
    if (rdma_bind_addr(listen_id, reinterpret_cast<sockaddr *>(&addr)))
        throw std::runtime_error("rdma_bind_addr");

    // listen
    if (rdma_listen(listen_id, 1))
        throw std::runtime_error("rdma_listen");
    
    printf("[SERVER] Listening on %s:%u\n\n", bind_ip, DEFAULT_PORT);

    // ── 2. Wait for a connection request ────────────────────────────────
    rdma_cm_event *ev = nullptr;
    if (rdma_get_cm_event(ec, &ev)) throw std::runtime_error(std::string("rdma_get_cm_event: ") + strerror(errno));
    if (ev->event != RDMA_CM_EVENT_CONNECT_REQUEST) throw std::runtime_error("Unexpected CM event");

    rdma_cm_id *cm_id = ev->id;  // new per-connection ID
    // Copy client MR info out of private_data before acking the event
    mr_info_t client_mr{};
    if (ev->param.conn.private_data && ev->param.conn.private_data_len >= sizeof(mr_info_t))
       memcpy(&client_mr, )
    if (rdma_ack_cm_event(ev)) throw std::runtime_error("rdma_ack_cm_event"); // ack after reading

    printf("[SERVER] Connection request received\n");
    printf("[SERVER] Client MR: addr=%#lx  rkey=%#010x\n\n",
           (unsigned long)client_mr.addr, client_mr.rkey);

    // ── 3. Allocate buffer + register RDMA resources ────────────────────
    rdma_ctx_t ctx{};
    ctx.cm_id  = cm_id;
    ctx.data_size = BUF_SIZE;
    ctx.data    = static_cast<char *>(aligned_alloc(4096, BUF_SIZE));
    if (!ctx.data) throw std::runtime_error("aligned_alloc failed");
    const char *msg =
        "Hello from RDMA Read! "
        "This landed in server memory without server CPU involvement.\n";
    static constexpr size_t READ_OFF = 1024;
    memcpy(ctx.data + READ_OFF, msg, strlen(msg) + 1);

    // ── PD, MR, CQ, QP ──────────────────────────────────────────────────
     // Protection Domain: all resources (MR, QP) must belong to the same PD
    ctx.pd = ibv_alloc_pd(ctx.cm_id->verbs);  // device context: ctx.cm_id->verbs
    if (!ctx.pd) throw std::runtime_error("ibv_alloc_pd failed");

    // Memory Registration: pin buffer, give NIC DMA access
    // LOCAL_WRITE  – NIC writes here on RECV / RDMA READ response
    // REMOTE_WRITE – peer can issue RDMA WRITE into this buffer
    ctx.mr = ibv_reg_mr(ctx.pd, ctx.data, ctx.data_size,
                        IBV_ACCESS_LOCAL_WRITE  | IBV_ACCESS_REMOTE_WRITE);
    if (!ctx.mr) throw std::runtime_error("ibv_reg_mr failed");

    // Completion Queue: send and recv completions land here
    ctx.cq = ibv_create_cq(ctx.cm_id->verbs, CQ_DEPTH, nullptr, nullptr, 0);
    if (!ctx.cq) throw std::runtime_error("ibv_create_cq failed");

    // Queue Pair via rdma_cm: sets cm_id->qp and transitions it to INIT
    // (raw ibv_create_qp leaves the QP in RESET and requires manual state transitions)
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

    printf("[SERVER] Memory region registered:\n");
    printf("         addr = %p\n",     ctx.data);
    printf("         size = %.1f MB\n", BUF_SIZE / 1e6);
    printf("         lkey = 0x%08x\n",  ctx.mr->lkey);
    printf("         rkey = 0x%08x  (sent to client via private_data)\n\n",
           ctx.mr->rkey);

    // ── 4. Pre-post one RECV before rdma_accept ─────────────────────────────
    // Must happen before accept so we cannot miss the client's SEND("DONE")
    ibv_sge sge{};
    sge.addr   = (uintptr_t)(ctx.data);
    sge.length = BUF_SIZE;
    sge.lkey   = ctx.mr->lkey;

    ibv_recv_wr wr{};
    wr.wr_id   = 42;
    wr.sg_list = &sge;
    wr.num_sge = 1;

    ibv_recv_wr *bad = nullptr;
    if (ibv_post_recv(ctx.cm_id->qp, &wr, &bad)) throw std::runtime_error("ibv_post_recv");


    // ── 5. Accept — send our MR info back in private_data ───────────────
    mr_info_t my_mr{ reinterpret_cast<uint64_t>(ctx.data), ctx.mr->rkey };

    rdma_conn_param cp{};
    cp.private_data        = &my_mr;
    cp.private_data_len    = sizeof(my_mr);
    cp.initiator_depth     = 1;  // allow us to issue RDMA READs if needed
    cp.responder_resources = 1;  // accept RDMA READs from client
    cp.rnr_retry_count     = 7;


    // ── 6. Wait for ESTABLISHED ──────────────────────────────────────────
    if (rdma_accept(cm_id, &cp)) throw std::runtime_error("rdma_accept");
    if (rdma_get_cm_event(ec, &ev)) throw std::runtime_error(std::string("rdma_get_cm_event: ") + strerror(errno));
    if (ev->event != RDMA_CM_EVENT_ESTABLISHED) throw std::runtime_error("Unexpected CM event");
    if (rdma_ack_cm_event(ev)) throw std::runtime_error("rdma_ack_cm_event");

    printf("[SERVER] Connection established — QP is RTS\n");
    printf("[SERVER] Waiting for client to finish RDMA transfers...\n\n");

    // ── 7. Poll for client SEND("DONE") ──────────────────────────────────
    // RDMA WRITE and RDMA READ produce NO CQE on the target side.
    // SEND is two-sided: we get a RECV completion when it arrives.
    bool done = false;
    while (!done) {
        ibv_wc wc[8];
        int n = ibv_poll_cq(ctx.cq, 1, wc);
        for (int i = 0; i < n; ++i) {
            if (wc[i].status != IBV_WC_SUCCESS) {
                fprintf(stderr, "WC error: %s\n", ibv_wc_status_str(wc[i].status));
                continue;
            }
            if (wc[i].opcode == IBV_WC_RECV) {
                printf("[SERVER] RECV completion\n");
                done = true;
            }
        }
    }

    // ── 8. Inspect what RDMA WRITE delivered ────────────────────────────
    printf("\n[SERVER] buf[0..127] after client RDMA WRITE:\n");
    printf("──────────────────────────────────────────────\n");
    printf("%.128s", ctx.data);
    printf("\n──────────────────────────────────────────────\n");

    // ── 9. Graceful disconnect ───────────────────────────────────────────
    rdma_disconnect(cm_id);

    // ── 10. Cleanup ──────────────────────────────────────────────────────
    rdma_destroy_qp(cm_id);      // must be before ibv_destroy_cq
    ibv_destroy_cq(ctx.cq);
    ibv_dereg_mr(ctx.mr);
    ibv_dealloc_pd(ctx.pd);
    free(ctx.data);
    rdma_destroy_id(cm_id);
    rdma_destroy_id(listen_id);
    rdma_destroy_event_channel(ec);

    printf("\n[SERVER] Clean shutdown.\n");
    return 0;
}
