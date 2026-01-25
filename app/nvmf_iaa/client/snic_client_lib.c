#include "../snic_client.h"
#include "idxd.h"
#include <arpa/inet.h>
#include <fcntl.h>
#include <infiniband/verbs.h>
#include <rdma/rdma_cma.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "spdk/stdinc.h"
#include "spdk/env.h"
#include "spdk/nvme.h"
#include "spdk/vmd.h"

#define DATA_SIZE (1024 * 1024)
#define PORTAL_SIZE 4096

// Global arg holding for callback access (Prototype simplicity)
static char *g_snic_ip = NULL;

// Context Structure (Opaque to user)
struct snic_client_ctx {
    // SNIC Connection (Raw RDMA)
    struct rdma_cm_id *id;
    struct ibv_pd *pd;
    struct ibv_qp *qp;

    // Target Connection (SPDK NVMe)
    struct spdk_nvme_ctrlr *ctrlr;
    struct spdk_nvme_qpair *nvme_qpair;
    uint16_t nvme_qid;

    // Infrastructure Memory
    void *scratch_buf;
    struct ibv_mr *mr_scratch;
    
    void *portal_buf;
    struct ibv_mr *mr_portal;

    struct snic_completion *cq_buf;
    struct ibv_mr *mr_cq;

    struct iax_completion_record *comp_buf;
    struct ibv_mr *mr_comp;

    // Ring Buffer Management
    uint64_t submission_idx; // Internal monotonic counter for Ring Slots
    uint32_t cq_head;
};

// Helper: Die
static void die(const char *reason) {
  perror(reason);
  exit(EXIT_FAILURE);
}

// -----------------------------------------------------------------------------
// SNIC Connection (Raw RDMA)
// -----------------------------------------------------------------------------

static int rdma_connect_snic(const char *ip, int port, struct snic_client_ctx *ctx) {
    struct rdma_event_channel *ec;
    struct rdma_cm_event *event;
    struct sockaddr_in addr = {};
    struct rdma_conn_param cm_params = {};
    struct spdk_nvmf_rdma_request_private_data {
        uint16_t recfmt; uint16_t qid; uint16_t hrqsize; uint16_t hsqsize; uint16_t cntlid; uint8_t rsvd[22];
    } pdata = {};

    printf("[RDMA] Resolving SNIC address %s:%d...\n", ip, port);
    ec = rdma_create_event_channel();
    rdma_create_id(ec, &ctx->id, NULL, RDMA_PS_TCP);

    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    inet_pton(AF_INET, ip, &addr.sin_addr);

    rdma_resolve_addr(ctx->id, NULL, (struct sockaddr *)&addr, 2000);
    rdma_get_cm_event(ec, &event); rdma_ack_cm_event(event);

    printf("[RDMA] Resolving route...\n");
    rdma_resolve_route(ctx->id, 2000);
    rdma_get_cm_event(ec, &event); rdma_ack_cm_event(event);

    ctx->pd = ibv_alloc_pd(ctx->id->verbs);

    struct ibv_qp_init_attr qp_attr = {};
    qp_attr.cap.max_send_wr = 16;
    qp_attr.cap.max_recv_wr = 16;
    qp_attr.cap.max_send_sge = 16;
    qp_attr.cap.max_recv_sge = 16;
    qp_attr.cap.max_inline_data = 64;
    qp_attr.qp_type = IBV_QPT_RC;
    qp_attr.send_cq = ibv_create_cq(ctx->id->verbs, 16, NULL, NULL, 0);
    qp_attr.recv_cq = ibv_create_cq(ctx->id->verbs, 16, NULL, NULL, 0);

    rdma_create_qp(ctx->id, ctx->pd, &qp_attr);

    // SNIC Connection Parameters (Side Channel)
    cm_params.initiator_depth = 16;
    cm_params.responder_resources = 16;
    cm_params.retry_count = 7;
    
    pdata.qid = 0; 
    cm_params.private_data = &pdata;
    cm_params.private_data_len = sizeof(pdata);

    printf("[RDMA] Connecting to SNIC...\n");
    rdma_connect(ctx->id, &cm_params);
    rdma_get_cm_event(ec, &event);
    if (event->event != RDMA_CM_EVENT_ESTABLISHED) {
        printf("[RDMA] Connection FAILED (Event: %d)\n", event->event);
        return -1;
    }
    rdma_ack_cm_event(event);
    printf("[RDMA] SNIC Connection established!\n");
    
    ctx->qp = ctx->id->qp;
    return 0;
}

static int snic_send_setup(struct snic_client_ctx *ctx) {
    struct snic_setup_msg msg = {};
    struct ibv_sge sge;
    struct ibv_send_wr wr = {}, *bad_wr;

    printf("[Init] Sending SETUP Message to SNIC...\n");

    msg.scratch_base_addr = (uintptr_t)ctx->scratch_buf;
    msg.scratch_rkey = ctx->mr_scratch->rkey;
    msg.portal_addr = (uintptr_t)ctx->portal_buf;
    msg.portal_rkey = ctx->mr_portal->rkey;
    msg.cq_base_addr = (uintptr_t)ctx->cq_buf;
    msg.cq_rkey = ctx->mr_cq->rkey;
    msg.comp_base_addr = (uintptr_t)ctx->comp_buf;
    msg.comp_rkey = ctx->mr_comp->rkey;
    
    // Extract CNTLID
    const struct spdk_nvme_ctrlr_data *cdata = spdk_nvme_ctrlr_get_data(ctx->ctrlr);
    msg.client_cntlid = cdata->cntlid;

    sge.addr = (uintptr_t)&msg;
    sge.length = sizeof(msg);
    sge.lkey = 0;

    wr.wr_id = 9999;
    wr.opcode = IBV_WR_SEND;
    wr.sg_list = &sge;
    wr.num_sge = 1;
    wr.send_flags = IBV_SEND_SIGNALED | IBV_SEND_INLINE;

    if (ibv_post_send(ctx->qp, &wr, &bad_wr)) return -1;

    // Poll for completion
    struct ibv_wc wc;
    while(ibv_poll_cq(ctx->qp->send_cq, 1, &wc) == 0);
    
    if (wc.status != IBV_WC_SUCCESS) {
        printf("Setup Send Failed: %d\n", wc.status);
        return -1;
    }
    printf("[Init] SETUP Message Sent successfully.\n");
    return 0;
}

// -----------------------------------------------------------------------------
// SPDK Probe Logic
// -----------------------------------------------------------------------------

static bool
probe_cb(void *cb_ctx, const struct spdk_nvme_transport_id *trid,
         struct spdk_nvme_ctrlr_opts *opts) {
    printf("[Init] Attaching to %s\n", trid->traddr);
    return true;
}

static void
attach_cb(void *cb_ctx, const struct spdk_nvme_transport_id *trid,
          struct spdk_nvme_ctrlr *ctrlr, const struct spdk_nvme_ctrlr_opts *opts) {
    struct snic_client_ctx *ctx = cb_ctx;
    
    printf("[Init] Attached to NVMe Controller\n");
    ctx->ctrlr = ctrlr;

    struct spdk_nvme_io_qpair_opts qp_opts;
    spdk_nvme_ctrlr_get_default_io_qpair_opts(ctrlr, &qp_opts, sizeof(qp_opts));
    
    ctx->nvme_qpair = spdk_nvme_ctrlr_alloc_io_qpair(ctrlr, &qp_opts, sizeof(qp_opts));
    if (!ctx->nvme_qpair) {
        printf("[Init] Failed to allocate IO QPair\n");
        return;
    }

    ctx->nvme_qid = spdk_nvme_qpair_get_id(ctx->nvme_qpair);
    printf("[Init] Allocated IO QPair. Negotiated QID: %u\n", ctx->nvme_qid);

    // Connect to SNIC 
    if (rdma_connect_snic(g_snic_ip, 18515, ctx)) {
        printf("[Init] SNIC Connection Failed inside Callback\n");
        exit(1);
    }
}

// -----------------------------------------------------------------------------
// Initialization
// -----------------------------------------------------------------------------

struct snic_client_ctx *snic_client_init(const char *snic_ip, const char *target_ip, int target_port, const char *wq_path) {
    struct snic_client_ctx *ctx = calloc(1, sizeof(*ctx));
    struct spdk_env_opts opts;
    g_snic_ip = (char*)snic_ip; // Save for callback
    
    spdk_env_opts_init(&opts);
    opts.name = "snic_client";
    opts.shm_id = 0;
    if (spdk_env_init(&opts) < 0) {
        fprintf(stderr, "Unable to initialize SPDK env\n");
        exit(1);
    }

    // Connect to Target (SPDK)
    struct spdk_nvme_transport_id trid = {};
    trid.trtype = SPDK_NVME_TRANSPORT_RDMA;
    trid.adrfam = SPDK_NVMF_ADRFAM_IPV4;
    snprintf(trid.traddr, sizeof(trid.traddr), "%s", target_ip);
    snprintf(trid.trsvcid, sizeof(trid.trsvcid), "%d", target_port);
    snprintf(trid.subnqn, sizeof(trid.subnqn), "%s", "nqn.2016-06.io.spdk:cnode1");

    printf("[Init] Probing Target %s:%d...\n", target_ip, target_port);
    if (spdk_nvme_probe(&trid, ctx, probe_cb, attach_cb, NULL) != 0) {
        fprintf(stderr, "spdk_nvme_probe failed\n");
        exit(1);
    }

    if (!ctx->ctrlr || !ctx->nvme_qpair) die("Target Connection Failed / Callback not finished");

    printf("[Init] Registering Memory...\n");
    // HUGEPAGES: Allocate Staging Buffer for ALL slots (CQ_SIZE * MAX_DATA_SIZE)
    uint64_t total_scratch_size = CQ_SIZE * MAX_DATA_SIZE;
    ctx->scratch_buf = spdk_dma_zmalloc(total_scratch_size, 2 * 1024 * 1024, NULL); // 2MB Align
    if (!ctx->scratch_buf) die("scratch hugepage alloc");
    
    // Extract Target PD (using new SPDK API)
    struct ibv_pd *target_pd = spdk_nvme_rdma_get_ibv_pd(ctx->nvme_qpair);
    if (!target_pd) die("failed to get target pd");

    // Register Scratch on Target PD
    ctx->mr_scratch = ibv_reg_mr(target_pd, ctx->scratch_buf, total_scratch_size, IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_REMOTE_READ);
    
    if (posix_memalign((void**)&ctx->cq_buf, 4096, sizeof(struct snic_completion) * CQ_SIZE)) die("cq");
    memset(ctx->cq_buf, 0, sizeof(struct snic_completion) * CQ_SIZE);
    ctx->mr_cq = ibv_reg_mr(ctx->pd, ctx->cq_buf, sizeof(struct snic_completion) * CQ_SIZE, IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE);

    // Parallel IAA Completion Buffers (one per slot)
    if (posix_memalign((void**)&ctx->comp_buf, 64, sizeof(struct iax_completion_record) * CQ_SIZE)) die("comp");
    ctx->mr_comp = ibv_reg_mr(ctx->pd, ctx->comp_buf, sizeof(struct iax_completion_record) * CQ_SIZE, IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_REMOTE_READ);

    printf("[Init] Mapping IAA Portal at %s...\n", wq_path);
    int fd = open(wq_path, O_RDWR);
    if (fd < 0) die("open wq");
    ctx->portal_buf = mmap(NULL, PORTAL_SIZE, PROT_WRITE, MAP_SHARED, fd, 0);
    ctx->mr_portal = ibv_reg_mr(ctx->pd, ctx->portal_buf, PORTAL_SIZE, IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE);

    ctx->submission_idx = 0;
    ctx->cq_head = 0;

    printf("[Init] All systems OK. Staging Buffer: %lu MB (Hugepages)\n", total_scratch_size / 1024 / 1024);

    if (snic_send_setup(ctx)) {
        fprintf(stderr, "[Init] Failed to send SETUP message\n");
        exit(1);
    }

    return ctx;
}

// API: Alloc Buffer
void *snic_client_alloc_buffer(struct snic_client_ctx *ctx, size_t size) {
    void *buf;
    if (posix_memalign(&buf, 4096, size)) return NULL;
    return buf; 
}

// Internal Submit
static int submit_req(struct snic_client_ctx *ctx, int op, void *buf, uint64_t lba, uint64_t len, uint32_t req_id) {
    struct snic_request req = {};
    
    req.req_id = req_id;
    req.op = op;
    req.len = len;
    req.lba = lba;
    
    // User Buffer VA only
    req.src_addr = (uintptr_t)buf;
    req.dst_addr = (uintptr_t)buf;

    // Slot Index for this request (Round Robin)
    uint32_t current_slot = (uint32_t)(ctx->submission_idx++ % CQ_SIZE);
    req.slot_idx = current_slot;

    struct ibv_sge sge = { .addr = (uint64_t)&req, .length = sizeof(req), .lkey = 0 };
    struct ibv_send_wr wr = {
        .wr_id = req.req_id,
        .opcode = IBV_WR_SEND,
        .sg_list = &sge,
        .num_sge = 1,
        .send_flags = IBV_SEND_SIGNALED | IBV_SEND_INLINE,
    };
    struct ibv_send_wr *bad_wr;

    ibv_post_send(ctx->qp, &wr, &bad_wr);
    
    return req_id;
}

// API: Write
int snic_client_write(struct snic_client_ctx *ctx, void *src_buf, uint64_t lba, uint64_t len, uint32_t req_id) {
    return submit_req(ctx, SNIC_OP_WRITE, src_buf, lba, len, req_id);

}

// API: Read
int snic_client_read(struct snic_client_ctx *ctx, void *dst_buf, uint64_t lba, uint64_t len, uint32_t req_id) {
    return submit_req(ctx, SNIC_OP_READ, dst_buf, lba, len, req_id);
}

// API: Poll
int snic_client_poll(struct snic_client_ctx *ctx, uint32_t *req_id, int *status) {
    uint32_t idx = ctx->cq_head % CQ_SIZE;
    volatile struct snic_completion *cump = &ctx->cq_buf[idx];

    // Check Status in Ring Buffer (Status 0 = Empty, 1/-1 = Valid)
    if (cump->status != 0) {
        *req_id = cump->req_id;
        *status = cump->status;
        
        // Clear slot for next wrap
        cump->status = 0;
        
        // Advance Head
        ctx->cq_head++;
        
        // Optional: Trigger SPDK completions if needed to keep connection alive/heartbeats?
        // spdk_nvme_qpair_process_completions(ctx->nvme_qpair, 0); 

        return 1; // Completed
    }
    
    // Pump NVMe connection just in case (e.g. Keepalives)
    spdk_nvme_qpair_process_completions(ctx->nvme_qpair, 0);
    
    return 0;
}