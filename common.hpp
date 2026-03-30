#pragma once
#include <rdma/rdma_cma.h>
#include <infiniband/verbs.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <stdexcept>
#include <string>

// ─────────────────────────────────────────────
//  Configuration
// ─────────────────────────────────────────────
static constexpr uint16_t DEFAULT_PORT = 18515;
static constexpr int      CQ_DEPTH     = 128;
static constexpr int      QP_MAX_WR    = 128;
static constexpr int      QP_MAX_SGE   = 1;
static constexpr size_t   BUF_SIZE     = 4096; 

// ─────────────────────────────────────────────
//  MR info exchanged via rdma_cm private_data
//  (fits in the 56-byte private_data limit)
// ─────────────────────────────────────────────
struct mr_info_t {
    uint64_t addr;  // registered buffer virtual address
    uint32_t rkey;  // remote key for RDMA READ / WRITE
};
static_assert(sizeof(mr_info_t) <= 56, "private_data too large");

// ─────────────────────────────────────────────
//  Per-connection RDMA context
// ─────────────────────────────────────────────
struct rdma_ctx_t {
    rdma_cm_id *cm_id     = nullptr; // rdma_cm owns the QP (cm_id->qp)
    ibv_pd     *pd        = nullptr;
    ibv_mr     *mr        = nullptr;
    ibv_cq     *cq        = nullptr;
    char        *data      = nullptr;
    size_t      data_size = 0;
};

