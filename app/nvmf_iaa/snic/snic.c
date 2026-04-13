#include "spdk/stdinc.h"
#include "spdk/thread.h"
#include "spdk/bdev.h"
#include "spdk/env.h"
#include "spdk/event.h"
#include "spdk/log.h"
#include "spdk/string.h"
#include "spdk/nvme.h"
#include "spdk/vmd.h"
#include "spdk/util.h"

#include <rdma/rdma_cma.h>
#include <infiniband/verbs.h>
#include "idxd.h"
#include "../nvmf_iaa.h"

#include <stdio.h>
#include <stdint.h>

/* Missing in some idxd.h copies */
#define IDXD_OP_FLAG_AECS_RW_TOGGLE      0x400000

/* Compression flags */
#define IAX_COMP_FLAG_STATS_MODE         0x0001
#define IAX_COMP_FLAG_FLUSH_OUTPUT       0x0002
#define IAX_COMP_FLAG_END_PROCESSING     0x0004
#define IAX_COMP_FLAG_HDR_GEN(x)         ((uint16_t)(((x) & 0x7) << 12))

/* Compression2 flags */
#define IAX_COMP2_FLAG_MAKE_COMPLETE_TABLES      0x00000001
#define IAX_COMP2_FLAG_WRITE_AECS_HUFFMAN_TABLES 0x00000002

/* Decompression flags */
#define IAX_DECOMP_FLAG_ENABLE_DECOMP      0x0001
#define IAX_DECOMP_FLAG_FLUSH_OUTPUT       0x0002
#define IAX_DECOMP_FLAG_STOP_ON_EOB        0x0004
#define IAX_DECOMP_FLAG_CHECK_FOR_EOB      0x0008
#define IAX_DECOMP_FLAG_SELECT_BFINAL_EOB  0x0010
#define IAX_DECOMP_FLAG_DECOMP_BIT_ORDER   0x0020
#define IAX_DECOMP_FLAG_SUPPRESS_OUTPUT    0x0200
#define IAX_DECOMP_FLAG_LOAD_PARTIAL       0x2000
#define AECS_SLOT_BYTES(size)              (2ULL * (uint64_t)(size))
#define COMP_STATS_PRINT_EVERY             128

static inline void dump_desc64(const void *desc, const char *tag)
{
    const uint64_t *w = (const uint64_t *)desc;
    fprintf(stderr, "[%s] desc @%p\n", tag, desc);
    for (int i = 0; i < 8; i++) {
        fprintf(stderr, "  +%02d: 0x%016llx\n",
                i * 8, (unsigned long long)w[i]);
    }
}

// Async Request State Machine
enum snic_req_state {
    REQ_FREE = 0,
    REQ_IAA_SUBMITTED,      // Portal Write Sent
    REQ_IAA_POLLING,        // Ready to Poll
    REQ_IAA_READ_PENDING,   // RDMA Read Status Sent
    REQ_NVME_PENDING,       // NVMe Command Submitted
};

enum snic_iaa_phase {
    IAA_PHASE_NONE = 0,
    IAA_PHASE_COMPRESS,
    IAA_PHASE_DECOMPRESS,
};

struct snic_async_req {
    enum snic_req_state state;
    uint32_t slot_idx;
    uint32_t req_id;
    int op;
    // Persist Req Data
    uint64_t len;
    uint64_t lba;
    uint64_t src_addr;
    uint64_t dst_addr;

    // [IAA_COMP_UPDATE]
    uint64_t comp_aecs_addr;
    uint32_t comp_aecs_size;

    uint64_t decomp_aecs_addr;
    uint32_t decomp_aecs_size;

    uint32_t orig_len;
    uint32_t xfer_len;
    uint32_t comp_len;

    int iaa_phase;
    bool status_read_outstanding;
    
    // Buffer for RDMA Read of Completion Record
    uint8_t *status_buf;
    struct ibv_mr *mr_status;
};

struct snic_context {
    struct rdma_cm_id *listen_id;
    struct rdma_cm_id *cm_id;
    struct ibv_pd *pd;
    // struct ibv_mr *mr_req;
    // struct snic_request *req; // Recv buffer
    
    // IAA Resources
    struct iax_hw_desc *desc_pool;
    struct ibv_mr *mr_desc;
    
    // NVMe Resources
    struct spdk_nvme_ctrlr *ctrlr;
    struct spdk_nvme_qpair *qpair;
    struct spdk_nvme_ns *ns;

    // CQ Management
    uint64_t cq_tail; 
    
    // Client Setup Info
    bool setup_done;
    struct snic_setup_msg setup_info;

    struct ibv_mr *mr_msg;
    void *msg_buf;
    size_t msg_buf_sz;

    // Async Request Pool
    struct snic_async_req active_reqs[CQ_SIZE];
};

static struct snic_context g_ctx = {0};
static char *g_trid_str = NULL;
static uint32_t g_lba_comp_len[4096] = {0};
static uint32_t g_lba_orig_len[4096] = {0};
static uint64_t g_comp_stats_reqs = 0;
static uint64_t g_comp_stats_orig_bytes = 0;
static uint64_t g_comp_stats_comp_bytes = 0;

static void
reset_connection_state(void)
{
    g_ctx.setup_done = false;
    g_ctx.cq_tail = 0;
    memset(&g_ctx.setup_info, 0, sizeof(g_ctx.setup_info));
    g_comp_stats_reqs = 0;
    g_comp_stats_orig_bytes = 0;
    g_comp_stats_comp_bytes = 0;

    if (g_ctx.msg_buf && g_ctx.msg_buf_sz) {
        memset(g_ctx.msg_buf, 0, g_ctx.msg_buf_sz);
    }

    for (int i = 0; i < CQ_SIZE; i++) {
        g_ctx.active_reqs[i].state = REQ_FREE;
        g_ctx.active_reqs[i].slot_idx = i;
        g_ctx.active_reqs[i].req_id = 0;
        g_ctx.active_reqs[i].op = 0;
        g_ctx.active_reqs[i].len = 0;
        g_ctx.active_reqs[i].lba = 0;
        g_ctx.active_reqs[i].src_addr = 0;
        g_ctx.active_reqs[i].dst_addr = 0;
        g_ctx.active_reqs[i].comp_aecs_addr = 0;
        g_ctx.active_reqs[i].comp_aecs_size = 0;
        g_ctx.active_reqs[i].decomp_aecs_addr = 0;
        g_ctx.active_reqs[i].decomp_aecs_size = 0;
        g_ctx.active_reqs[i].orig_len = 0;
        g_ctx.active_reqs[i].xfer_len = 0;
        g_ctx.active_reqs[i].comp_len = 0;
        g_ctx.active_reqs[i].iaa_phase = IAA_PHASE_NONE;
        g_ctx.active_reqs[i].status_read_outstanding = false;

        if (g_ctx.active_reqs[i].status_buf) {
            memset(g_ctx.active_reqs[i].status_buf, 0, 64);
        }
    }
}

// -----------------------------------------------------------------------------
// Completion Logic
// -----------------------------------------------------------------------------

static void
submit_completion(int status, uint32_t req_id) {
    struct ibv_sge sge = {};
    struct ibv_send_wr wr = {}, *bad_wr;
    struct snic_completion comp_pkt = {
        .req_id = req_id,
        .status = (status == 0) ? 1 : -1
    };

    // Use INLINE send 
    sge.addr = (uintptr_t)&comp_pkt;
    sge.length = sizeof(comp_pkt);
    
    wr.wr_id = 999;
    wr.opcode = IBV_WR_RDMA_WRITE;
    wr.sg_list = &sge;
    wr.num_sge = 1;
    wr.send_flags = IBV_SEND_SIGNALED | IBV_SEND_INLINE;
    
    wr.wr.rdma.remote_addr = g_ctx.setup_info.cq_base_addr + ((g_ctx.cq_tail % CQ_SIZE) * sizeof(struct snic_completion));
    wr.wr.rdma.rkey = g_ctx.setup_info.cq_rkey;

    g_ctx.cq_tail++;

    int rc = ibv_post_send(g_ctx.cm_id->qp, &wr, &bad_wr);
    if (rc) SPDK_ERRLOG("Failed to post completion: %d\n", rc);
}

static void submit_nvme_io(struct snic_async_req *areq, int r_w); // Forward Decl
static void submit_iaa_async(int slot_idx); // Forward Decl

// -----------------------------------------------------------------------------
// NVMe Logic
// -----------------------------------------------------------------------------

static void
nvme_complete(void *arg, const struct spdk_nvme_cpl *cpl) {
    struct snic_async_req *areq = (struct snic_async_req *)arg;

    if (areq->op == SNIC_OP_READ) {
        SPDK_NOTICELOG("READ nvme_complete: req_id=%u slot=%u phase=%d cdw0=0x%x status_type=0x%x status_code=0x%x\n",
                       areq->req_id, areq->slot_idx, areq->iaa_phase,
                       cpl->cdw0, cpl->status.sct, cpl->status.sc);
    }

    if (spdk_nvme_cpl_is_error(cpl)) {
        SPDK_ERRLOG("NVMe Command Failed!\n");
        submit_completion(-1, areq->req_id);
        areq->state = REQ_FREE;
        return;
    }

    if (areq->op == SNIC_OP_WRITE) {
        /*
         * WRITE path already finished the NVMe write after compression.
         */
        submit_completion(0, areq->req_id);
        areq->state = REQ_FREE;
    } else if (areq->op == SNIC_OP_READ) {
        /*
         * READ path:
         *   NVMe read has placed compressed data into scratch.
         *   Now launch IAA decompression to copy/expand into user buffer.
         */
        areq->iaa_phase = IAA_PHASE_DECOMPRESS;
        submit_iaa_async(areq->slot_idx);
    } else {
        submit_completion(-1, areq->req_id);
        areq->state = REQ_FREE;
    }
}

static void
submit_nvme_io(struct snic_async_req *areq, int r_w) {
    struct spdk_nvme_cmd cmd = {};
    int rc;

    cmd.opc = (r_w == 1) ? SPDK_NVME_OPC_WRITE : SPDK_NVME_OPC_READ;
    cmd.nsid = spdk_nvme_ns_get_id(g_ctx.ns);
    
    cmd.cdw10 = areq->lba & 0xFFFFFFFF; // SLBA Low
    cmd.cdw11 = areq->lba >> 32;        // SLBA High
    
    uint32_t sector_size = spdk_nvme_ns_get_sector_size(g_ctx.ns);
    // uint32_t nlb = (areq->len + sector_size - 1) / sector_size;
    // cmd.cdw12 = nlb - 1;

    // STAGING OFFSET: Scratch Base + (SlotIdx * MAX_DATA_SIZE)
    uint64_t scratch_offset = (uint64_t)areq->slot_idx * MAX_DATA_SIZE;

    uint32_t io_len = areq->xfer_len;
    uint32_t nlb = (io_len + sector_size - 1) / sector_size;
    cmd.cdw12 = nlb - 1;
    cmd.dptr.sgl1.keyed.length = io_len;

    cmd.dptr.sgl1.address = g_ctx.setup_info.scratch_base_addr + scratch_offset;
    // cmd.dptr.sgl1.keyed.length = areq->len;
    cmd.dptr.sgl1.keyed.key = g_ctx.setup_info.scratch_rkey;
    cmd.dptr.sgl1.keyed.type = SPDK_NVME_SGL_TYPE_KEYED_DATA_BLOCK;
    cmd.dptr.sgl1.keyed.subtype = SPDK_NVME_SGL_SUBTYPE_ADDRESS;
    cmd.cdw15 = g_ctx.setup_info.client_cntlid;

    if (r_w == 0) {
        SPDK_NOTICELOG("READ submit_nvme_io: req_id=%u slot=%u lba=%lu comp_len=%u orig_len=%u xfer_len=%u scratch=0x%lx nlb=%u\n",
                       areq->req_id, areq->slot_idx, areq->lba,
                       areq->comp_len, areq->orig_len, areq->xfer_len,
                       cmd.dptr.sgl1.address, nlb);
    }
    
    rc = spdk_nvme_ctrlr_cmd_io_raw_with_md(g_ctx.ctrlr, g_ctx.qpair, &cmd, NULL, 0, NULL, nvme_complete, areq);
    if (rc) SPDK_ERRLOG("Failed to submit NVMe cmd: %d\n", rc);
}

static void
check_async_completions(void) {
    // Iterate all slots to check for progress conditions
    for(int i=0; i<CQ_SIZE; i++) {
        struct snic_async_req *areq = &g_ctx.active_reqs[i];
        
        if (areq->state == REQ_IAA_POLLING) {
            if (areq->status_read_outstanding) {
                continue;
            }

            // Issue RDMA Read to check status
            struct ibv_sge sge = {};
            struct ibv_send_wr wr = {}, *bad_wr;

            sge.addr = (uintptr_t)areq->status_buf;
            sge.length = sizeof(struct iax_completion_record);
            sge.lkey = areq->mr_status->lkey;

            wr.wr_id = 2000 + i; // 2000 base for Read Status
            wr.opcode = IBV_WR_RDMA_READ;
            wr.sg_list = &sge;
            wr.num_sge = 1;
            wr.send_flags = IBV_SEND_SIGNALED;
            
            uint64_t comp_offset = (uint64_t)i * sizeof(struct iax_completion_record);
            wr.wr.rdma.remote_addr = g_ctx.setup_info.comp_base_addr + comp_offset; 
            wr.wr.rdma.rkey = g_ctx.setup_info.comp_rkey;

            if (ibv_post_send(g_ctx.cm_id->qp, &wr, &bad_wr) == 0) {
                 // SPDK_NOTICELOG("Posted status RDMA_READ for slot=%d req_id=%u\n",
                 //                i, areq->req_id);
                 areq->status_read_outstanding = true;
                 areq->state = REQ_IAA_READ_PENDING;
            } else {
                 SPDK_ERRLOG("Failed to post Status Read for slot %d\n", i);
            }
        }
    }
}

static void
submit_iaa_async(int slot_idx) {
    struct ibv_sge sge = {};
    struct ibv_send_wr wr = {}, *bad_wr;
    struct snic_async_req *areq = &g_ctx.active_reqs[slot_idx];
    struct iax_hw_desc *desc = &g_ctx.desc_pool[slot_idx];

    uint64_t comp_offset = (uint64_t)slot_idx * sizeof(struct iax_completion_record);
    uint64_t scratch_addr = g_ctx.setup_info.scratch_base_addr +
                            ((uint64_t)slot_idx * MAX_DATA_SIZE);

    areq->state = REQ_IAA_SUBMITTED;
    areq->status_read_outstanding = false;
    memset(areq->status_buf, 0, 64);
    memset(desc, 0, sizeof(*desc));

    desc->flags = IDXD_OP_FLAG_RCR |
                  IDXD_OP_FLAG_CRAV |
                  IDXD_OP_FLAG_RD_SRC2_AECS;

    desc->completion_addr = g_ctx.setup_info.comp_base_addr + comp_offset;
    desc->int_handle = 0;
    desc->filter_flags = 0;
    desc->num_inputs = 0;

    if (areq->iaa_phase == IAA_PHASE_COMPRESS) {
        /*
         * WRITE path:
         * user buffer -> IAA compress -> scratch
         */
        desc->opcode = IAX_OPCODE_COMPRESS;

        desc->src1_addr = areq->src_addr;
        desc->src1_size = areq->orig_len;

        desc->dst_addr = scratch_addr;
        desc->max_dst_size = MAX_DATA_SIZE;

        desc->compr_flags = IAX_COMP_FLAG_FLUSH_OUTPUT |
                            IAX_COMP_FLAG_END_PROCESSING;

        desc->src2_addr = areq->comp_aecs_addr;
        desc->src2_size = areq->comp_aecs_size;

        // dump_desc64(desc, "COMP_DESC");
    } else if (areq->iaa_phase == IAA_PHASE_DECOMPRESS) {
        /*
         * READ path:
         * scratch (compressed payload) -> IAA decompress -> user buffer
         *
         * Important:
         *   - src1 is scratch
         *   - src1_size is comp_len
         *   - dst is the original user buffer
         *   - max_dst_size is orig_len
         *   - use DECOMPRESS opcode and decomp AECS
         */
        desc->opcode = IAX_OPCODE_DECOMPRESS;

        desc->src1_addr = scratch_addr;
        desc->src1_size = areq->comp_len;

        desc->dst_addr = areq->dst_addr;
        desc->max_dst_size = areq->orig_len;

        /*
         * The 16-bit field is named compr_flags in the struct,
         * but for DECOMPRESS it carries decompression flags.
         */
        desc->compr_flags =
            IAX_DECOMP_FLAG_ENABLE_DECOMP |
            IAX_DECOMP_FLAG_FLUSH_OUTPUT  |
            IAX_DECOMP_FLAG_STOP_ON_EOB   |
            IAX_DECOMP_FLAG_CHECK_FOR_EOB |
            IAX_DECOMP_FLAG_SELECT_BFINAL_EOB;

        desc->src2_addr = areq->decomp_aecs_addr;
        desc->src2_size = areq->decomp_aecs_size;

        // dump_desc64(desc, "DECOMP_DESC");
    } else {
        SPDK_ERRLOG("submit_iaa_async called with invalid iaa_phase=%d\n", areq->iaa_phase);
        submit_completion(-1, areq->req_id);
        areq->state = REQ_FREE;
        return;
    }

    sge.addr = (uintptr_t)desc;
    sge.length = sizeof(*desc);
    sge.lkey = 0;

    wr.wr_id = 1000 + slot_idx;
    wr.opcode = IBV_WR_RDMA_WRITE;
    wr.sg_list = &sge;
    wr.num_sge = 1;
    wr.send_flags = IBV_SEND_SIGNALED | IBV_SEND_INLINE;
    wr.wr.rdma.remote_addr = g_ctx.setup_info.portal_addr;
    wr.wr.rdma.rkey = g_ctx.setup_info.portal_rkey;

    if (ibv_post_send(g_ctx.cm_id->qp, &wr, &bad_wr)) {
        SPDK_ERRLOG("Failed to post IAA descriptor for slot %d\n", slot_idx);
        submit_completion(-1, areq->req_id);
        areq->state = REQ_FREE;
        return;
    }

    // SPDK_NOTICELOG("Posted IAA descriptor slot=%d req_id=%u wr_id=%lu phase=%d\n",
    //                slot_idx, areq->req_id, wr.wr_id, areq->iaa_phase);

    areq->state = REQ_IAA_POLLING;
}


// -----------------------------------------------------------------------------
// RDMA Listen Logic
// -----------------------------------------------------------------------------

static int
on_connect_request(struct rdma_cm_id *id) {
    struct ibv_qp_init_attr qp_attr = {};
    struct rdma_conn_param cm_params = {};
    int rc;

    SPDK_NOTICELOG("Received Connection Request from Host.\n");

    g_ctx.cm_id = id;
    reset_connection_state();

    // Alloc PD
    g_ctx.pd = ibv_alloc_pd(id->verbs);

    // Alloc Message Recv Buffer (must hold either SETUP or REQUEST)
    g_ctx.msg_buf_sz = sizeof(struct snic_setup_msg) > sizeof(struct snic_request) ?
                    sizeof(struct snic_setup_msg) : sizeof(struct snic_request);

    g_ctx.msg_buf = spdk_dma_zmalloc(g_ctx.msg_buf_sz, 64, NULL);
    if (!g_ctx.msg_buf) {
        SPDK_ERRLOG("Failed to alloc msg_buf\n");
        return -1;
    }

    g_ctx.mr_msg = ibv_reg_mr(g_ctx.pd, g_ctx.msg_buf, g_ctx.msg_buf_sz,
                            IBV_ACCESS_LOCAL_WRITE);
    if (!g_ctx.mr_msg) {
        SPDK_ERRLOG("Failed to reg mr_msg\n");
        return -1;
    }

    // Alloc Descriptor Buffer
    g_ctx.desc_pool = spdk_dma_zmalloc(sizeof(*g_ctx.desc_pool) * CQ_SIZE, 64, NULL);
    // Inline send avoids MR registration
    g_ctx.mr_desc = NULL;

    // Alloc Async Resources
    for(int i=0; i<CQ_SIZE; i++) {
        g_ctx.active_reqs[i].state = REQ_FREE;
        // Allocate 4 bytes for status (completion record status field is byte 0)
        // 1 byte is enough but alloc 64 for alignment
        g_ctx.active_reqs[i].status_buf = spdk_dma_zmalloc(64, 64, NULL);
        g_ctx.active_reqs[i].mr_status = ibv_reg_mr(g_ctx.pd, g_ctx.active_reqs[i].status_buf, 64, IBV_ACCESS_LOCAL_WRITE);
    }

    // Create QP
    qp_attr.cap.max_send_wr = 256;
    qp_attr.cap.max_recv_wr = 128;
    qp_attr.cap.max_send_sge = 1;
    qp_attr.cap.max_recv_sge = 1;
    qp_attr.cap.max_inline_data = 64; 
    qp_attr.qp_type = IBV_QPT_RC;
    
    rc = rdma_create_qp(id, g_ctx.pd, &qp_attr);
    if (rc) return -1;

    // Post Recv for the Control Message
    struct ibv_sge sge = {
        .addr = (uintptr_t)g_ctx.msg_buf,
        .length = g_ctx.msg_buf_sz,
        .lkey = g_ctx.mr_msg->lkey
    };
    struct ibv_recv_wr wr = {
        .wr_id = 1,
        .sg_list = &sge,
        .num_sge = 1,
    };
    struct ibv_recv_wr *bad_wr;
    ibv_post_recv(g_ctx.cm_id->qp, &wr, &bad_wr);

    // Accept
    cm_params.initiator_depth = 1;
    cm_params.responder_resources = 1;
    rdma_accept(id, &cm_params);
    // printf("sizeof(struct snic_request)   = %zu\n", sizeof(struct snic_request));
    // printf("sizeof(struct snic_setup_msg) = %zu\n", sizeof(struct snic_setup_msg));
    // printf("msg_buf_sz = %zu\n", g_ctx.msg_buf_sz);
    return 0;
}

static void
process_request(void) {
    struct snic_request *req = (struct snic_request *)g_ctx.msg_buf;

    uint32_t slot = req->slot_idx;
    if (slot >= CQ_SIZE) {
        SPDK_ERRLOG("Invalid slot_idx %u (CQ_SIZE=%u)\n", slot, CQ_SIZE);
        return;
    }

    struct snic_async_req *areq = &g_ctx.active_reqs[slot];

    if (areq->state != REQ_FREE) {
        SPDK_ERRLOG("Slot %u busy, state=%d, old req_id=%u, new req_id=%lu\n",
                    slot, areq->state, areq->req_id, req->req_id);
        submit_completion(-1, req->req_id);
        return;
    }

    areq->req_id   = req->req_id;
    areq->slot_idx = slot;
    areq->op       = req->op;
    areq->len      = req->len;
    areq->lba      = req->lba;
    areq->src_addr = req->src_addr;
    areq->dst_addr = req->dst_addr;

    areq->orig_len = req->len;
    areq->xfer_len = req->len;
    areq->comp_len = 0;
    areq->iaa_phase = IAA_PHASE_NONE;
    areq->status_read_outstanding = false;

    /* AECS buffers come from setup, not from the request itself */
    areq->comp_aecs_addr   = g_ctx.setup_info.comp_aecs_addr +
                             (slot * AECS_SLOT_BYTES(g_ctx.setup_info.comp_aecs_size));
    areq->comp_aecs_size   = g_ctx.setup_info.comp_aecs_size;
    areq->decomp_aecs_addr = g_ctx.setup_info.decomp_aecs_addr +
                             (slot * AECS_SLOT_BYTES(g_ctx.setup_info.decomp_aecs_size));
    areq->decomp_aecs_size = g_ctx.setup_info.decomp_aecs_size;

    memset(areq->status_buf, 0, 64);

    if (req->op == SNIC_OP_WRITE) {
        areq->iaa_phase = IAA_PHASE_COMPRESS;
        submit_iaa_async(slot);
    } else if (req->op == SNIC_OP_READ) {
        if (req->lba >= 4096 || g_lba_comp_len[req->lba] == 0 || g_lba_orig_len[req->lba] == 0) {
            SPDK_ERRLOG("Missing metadata for LBA=%lu\n", req->lba);
            submit_completion(-1, req->req_id);
            areq->state = REQ_FREE;
            return;
        }

        /*
         * comp_len is the valid compressed payload length.
         * orig_len is the expected decompressed size.
         *
         * For now, NVMe I/O still uses block-sized transfer behavior.
         * Keep xfer_len as orig_len, because that is what already works in your current path.
         */
        areq->comp_len = g_lba_comp_len[req->lba];
        areq->orig_len = g_lba_orig_len[req->lba];
        areq->xfer_len = areq->orig_len;

        SPDK_NOTICELOG("READ process_request: req_id=%lu slot=%u lba=%lu comp_len=%u orig_len=%u xfer_len=%u\n",
                       req->req_id, slot, req->lba,
                       areq->comp_len, areq->orig_len, areq->xfer_len);

        submit_nvme_io(areq, 0);
    } else {
        SPDK_ERRLOG("Unknown op %d\n", req->op);
        submit_completion(-1, req->req_id);
        areq->state = REQ_FREE;
    }
}

static int
on_connection(struct rdma_cm_id *id) {
    SPDK_NOTICELOG("Connection Established with Host.\n");
    return 0;
}

// static int
// on_disconnect(struct rdma_cm_id *id) {
//     SPDK_NOTICELOG("Host Disconnected.\n");
//     return 0;
// }

static int
on_disconnect(struct rdma_cm_id *id) {
    SPDK_NOTICELOG("Host Disconnected.\n");

    if (g_ctx.cm_id == id) {
        g_ctx.cm_id = NULL;
    }

    reset_connection_state();

    return 0;
}

static void *
rdma_listener_thread(void *arg) {
    struct rdma_event_channel *ec;
    struct rdma_cm_event *event;
    struct sockaddr_in addr = {};

    ec = rdma_create_event_channel();
    rdma_create_id(ec, &g_ctx.listen_id, NULL, RDMA_PS_TCP);

    addr.sin_family = AF_INET;
    addr.sin_port = htons(SNIC_PORT);
    addr.sin_addr.s_addr = INADDR_ANY;

    rdma_bind_addr(g_ctx.listen_id, (struct sockaddr *)&addr);
    rdma_listen(g_ctx.listen_id, 1);
    
    SPDK_NOTICELOG("SNIC App Listening on port %d...\n", SNIC_PORT);

    while (rdma_get_cm_event(ec, &event) == 0) {
        struct rdma_cm_event event_copy = *event;
        rdma_ack_cm_event(event);

        switch (event_copy.event) {
            case RDMA_CM_EVENT_CONNECT_REQUEST:
                on_connect_request(event_copy.id);
                break;
            case RDMA_CM_EVENT_ESTABLISHED:
                on_connection(event_copy.id);
                break;
            case RDMA_CM_EVENT_DISCONNECTED:
                on_disconnect(event_copy.id);
                break;
            default:
                break;
        }
    }
    return NULL;
}

static int
check_messages(void *arg) {
    struct ibv_wc wc;
    int rc = 0;
    struct snic_request *req = (struct snic_request *)g_ctx.msg_buf;
    // POLL RECV CQ (Client Messages)
    if (g_ctx.cm_id && g_ctx.cm_id->qp) {
        // 1. Check Messages
        if (ibv_poll_cq(g_ctx.cm_id->qp->recv_cq, 1, &wc) > 0) {
            rc = 1; // Busy
            if (wc.status == IBV_WC_SUCCESS) {
                // SPDK_NOTICELOG("Recv CQE: byte_len=%u setup_done=%d\n",
                //                 wc.byte_len, g_ctx.setup_done ? 1 : 0);
                // Process Message
                if (!g_ctx.setup_done) {
                    if (wc.byte_len == sizeof(struct snic_setup_msg)) {
                        memcpy(&g_ctx.setup_info, g_ctx.msg_buf, sizeof(struct snic_setup_msg));
                        g_ctx.setup_done = true;
                        SPDK_NOTICELOG("Received SETUP Message. Client QID: %d\n",
                                    g_ctx.setup_info.client_cntlid);
                    } else {
                        SPDK_ERRLOG("Expected SETUP msg (%lu bytes), got %d\n",
                                    sizeof(struct snic_setup_msg), wc.byte_len);
                    }
                } else {
                    // SPDK_NOTICELOG("Got Request Op: %d, ID: %lu, Slot: %u\n",
                    //             req->op, req->req_id, req->slot_idx);
                    process_request();
                }

                // REPOST Recv WQE for next message
                struct ibv_sge sge = {
                    .addr = (uintptr_t)g_ctx.msg_buf,
                    .length = g_ctx.msg_buf_sz,
                    .lkey = g_ctx.mr_msg->lkey
                };
                struct ibv_recv_wr wr = {
                    .wr_id = 1,
                    .sg_list = &sge,
                    .num_sge = 1,
                };
                struct ibv_recv_wr *bad_wr;
                if (ibv_post_recv(g_ctx.cm_id->qp, &wr, &bad_wr)) {
                    SPDK_ERRLOG("Failed to repost recv WQE\n");
                }
            }
        }
        
        // 2. Poll SEND CQ (Async Completions: WR_ID 1000+, 2000+)
        while (ibv_poll_cq(g_ctx.cm_id->qp->send_cq, 1, &wc) > 0) {
            // SPDK_NOTICELOG("Send CQ completion wr_id=%lu status=%d\n",
            //                wc.wr_id, wc.status);
            if (wc.wr_id >= 1000 && wc.wr_id < 2000) {
                // IAA Write Header Completed
                uint32_t slot = wc.wr_id - 1000;
                if (g_ctx.active_reqs[slot].state == REQ_IAA_SUBMITTED) {
                     // SPDK_NOTICELOG("IAA descriptor send completed slot=%u req_id=%u\n",
                     //                slot, g_ctx.active_reqs[slot].req_id);
                     g_ctx.active_reqs[slot].state = REQ_IAA_POLLING;
                }
            } else if (wc.wr_id >= 2000 && wc.wr_id < 3000) {
                // Status Read Completed
                uint32_t slot = wc.wr_id - 2000;
                struct snic_async_req *areq = &g_ctx.active_reqs[slot];
                areq->status_read_outstanding = false;
                
                if (areq->state == REQ_IAA_READ_PENDING) {
                    struct iax_completion_record *cr =
                        (struct iax_completion_record *)areq->status_buf;

                    // SPDK_NOTICELOG("Status RDMA_READ completed slot=%u req_id=%u cr->status=0x%x output_size=%u phase=%d\n",
                    //                slot, areq->req_id, cr->status, cr->output_size,
                    //                areq->iaa_phase);

                    if (cr->status != 0) {
                        if (areq->iaa_phase == IAA_PHASE_COMPRESS) {
                            /*
                            * Compression finished.
                            * Save metadata for later reads, then write scratch to NVMe.
                            */
                            areq->comp_len = cr->output_size;

                            g_comp_stats_reqs++;
                            g_comp_stats_orig_bytes += areq->orig_len;
                            g_comp_stats_comp_bytes += areq->comp_len;

                            if ((g_comp_stats_reqs % COMP_STATS_PRINT_EVERY) == 0) {
                                SPDK_NOTICELOG("WRITE compress cumulative: reqs=%lu orig_bytes=%lu comp_bytes=%lu ratio=%.3f avg_comp_bytes=%.1f\n",
                                               g_comp_stats_reqs,
                                               g_comp_stats_orig_bytes,
                                               g_comp_stats_comp_bytes,
                                               g_comp_stats_orig_bytes ?
                                                   ((double)g_comp_stats_comp_bytes / (double)g_comp_stats_orig_bytes) : 0.0,
                                               g_comp_stats_reqs ?
                                                   ((double)g_comp_stats_comp_bytes / (double)g_comp_stats_reqs) : 0.0);
                            }

                            if (areq->lba < 4096) {
                                g_lba_comp_len[areq->lba] = areq->comp_len;
                                g_lba_orig_len[areq->lba] = areq->orig_len;
                            }

                            /*
                            * Keep the currently working block-I/O behavior.
                            * The effective compressed payload length is comp_len,
                            * but NVMe transfer length stays aligned to your current working path.
                            */
                            areq->xfer_len = areq->orig_len;

                            submit_nvme_io(areq, 1);
                        } else if (areq->iaa_phase == IAA_PHASE_DECOMPRESS) {
                            /*
                            * Decompression finished.
                            * The user buffer should now contain the original data.
                            */
                            submit_completion(0, areq->req_id);
                            areq->state = REQ_FREE;
                        } else {
                            SPDK_ERRLOG("IAA completion with invalid phase=%d\n", areq->iaa_phase);
                            submit_completion(-1, areq->req_id);
                            areq->state = REQ_FREE;
                        }
                    } else {
                        /*
                        * Hardware has not completed yet. Go back to polling.
                        */
                        areq->state = REQ_IAA_POLLING;
                    }
                }
            }
        }
        
        // 3. Drive Async State Machine
        check_async_completions();
    }

    // POLL NVMe Completions (Crucial for Keep Alives!)
    if (g_ctx.ctrlr) {
        spdk_nvme_ctrlr_process_admin_completions(g_ctx.ctrlr);
        if (g_ctx.qpair) {
            spdk_nvme_qpair_process_completions(g_ctx.qpair, 0);
        }
    }
    return rc;
}

// -----------------------------------------------------------------------------
// SPDK Init
// -----------------------------------------------------------------------------

static bool
probe_cb(void *cb_ctx, const struct spdk_nvme_transport_id *trid,
         struct spdk_nvme_ctrlr_opts *opts) {
    SPDK_NOTICELOG("Attaching to %s\n", trid->traddr);
    return true;
}

static void
attach_cb(void *cb_ctx, const struct spdk_nvme_transport_id *trid,
          struct spdk_nvme_ctrlr *ctrlr, const struct spdk_nvme_ctrlr_opts *opts) {
    int nsid;
    struct spdk_nvme_ns *ns;
    
    SPDK_NOTICELOG("Attached to NVMe Controller\n");
    g_ctx.ctrlr = ctrlr;

    // Use first namespace
    for (nsid = spdk_nvme_ctrlr_get_first_active_ns(ctrlr); nsid != 0;
         nsid = spdk_nvme_ctrlr_get_next_active_ns(ctrlr, nsid)) {
        ns = spdk_nvme_ctrlr_get_ns(ctrlr, nsid);
        if (ns) {
            g_ctx.ns = ns;
            SPDK_NOTICELOG("Using Namespace ID %d\n", nsid);
            break;
        }
    }

    struct spdk_nvme_io_qpair_opts qp_opts;
    spdk_nvme_ctrlr_get_default_io_qpair_opts(ctrlr, &qp_opts, sizeof(qp_opts));
    g_ctx.qpair = spdk_nvme_ctrlr_alloc_io_qpair(ctrlr, &qp_opts, sizeof(qp_opts));
}

static void
snic_start(void *arg1) {
    struct spdk_nvme_transport_id trid = {};
    pthread_t tid;

    SPDK_NOTICELOG("Starting SNIC App... Connecting to Storage Target: %s\n", g_trid_str);

    // Parse TRID
    if (spdk_nvme_transport_id_parse(&trid, g_trid_str) != 0) {
        SPDK_ERRLOG("Invalid TRID\n");
        spdk_app_stop(-1);
        return;
    }

    // Connect to NVMe Target
    if (spdk_nvme_probe(&trid, NULL, probe_cb, attach_cb, NULL) != 0) {
        SPDK_ERRLOG("nvme_probe failed\n");
        spdk_app_stop(-1);
        return;
    }

    // Start RDMA Listener Thread
    pthread_create(&tid, NULL, rdma_listener_thread, NULL);
    
    // Busy poll (0) to ensure we don't miss RDMA Recv postings/completions causing RNR
    spdk_poller_register(check_messages, NULL, 0); 
}

int
main(int argc, char *argv[]) {
    struct spdk_app_opts opts = {};
    int rc;

    spdk_app_opts_init(&opts, sizeof(opts));
    opts.name = "snic_app";
    
    if (argc < 3 || strcmp(argv[1], "-r") != 0) {
        fprintf(stderr, "Usage: %s -r <trid>\n", argv[0]);
        fprintf(stderr, "Example: trtype:RDMA adrfam:IPv4 traddr:192.168.1.100 trsvcid:4420 subnqn:nqn.2016-06.io.spdk:cnode1\n");
        return 1;
    }
    g_trid_str = argv[2];

    rc = spdk_app_start(&opts, snic_start, NULL);
    
    return rc;
}
