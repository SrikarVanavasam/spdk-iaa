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

// Async Request State Machine
enum snic_req_state {
    REQ_FREE = 0,
    REQ_IAA_SUBMITTED,      // Portal Write Sent
    REQ_IAA_POLLING,        // Ready to Poll
    REQ_IAA_READ_PENDING,   // RDMA Read Status Sent
    REQ_NVME_PENDING,       // NVMe Command Submitted
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
    
    // Buffer for RDMA Read of Completion Record
    uint8_t *status_buf;
    struct ibv_mr *mr_status;
};

struct snic_context {
    struct rdma_cm_id *listen_id;
    struct rdma_cm_id *cm_id;
    struct ibv_pd *pd;
    struct ibv_mr *mr_req;
    struct snic_request *req; // Recv buffer
    
    // IAA Resources
    struct iax_hw_desc *desc;
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

    // Async Request Pool
    struct snic_async_req active_reqs[CQ_SIZE];
};

static struct snic_context g_ctx = {0};
static char *g_trid_str = NULL;

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
    
    // SPDK_NOTICELOG("NVMe Complete for Req ID %u. Status: %s\n", areq->req_id, spdk_nvme_cpl_is_error(cpl) ? "FAIL" : "OK");

    if (spdk_nvme_cpl_is_error(cpl)) {
        SPDK_ERRLOG("NVMe Command Failed!\n");
        submit_completion(-1, areq->req_id);
    } else {
        if (areq->op == 1) {
            // WRITE: Done
            submit_completion(0, areq->req_id);
            areq->state = REQ_FREE;
        } else {
            // READ: NVMe Done -> Decompress (submit_iaa_async)
            submit_iaa_async(areq->slot_idx);
        }
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
    uint32_t nlb = (areq->len + sector_size - 1) / sector_size;
    cmd.cdw12 = nlb - 1;

    // STAGING OFFSET: Scratch Base + (SlotIdx * MAX_DATA_SIZE)
    uint64_t scratch_offset = (uint64_t)areq->slot_idx * MAX_DATA_SIZE;

    cmd.dptr.sgl1.address = g_ctx.setup_info.scratch_base_addr + scratch_offset;
    cmd.dptr.sgl1.keyed.length = areq->len;
    cmd.dptr.sgl1.keyed.key = g_ctx.setup_info.scratch_rkey;
    cmd.dptr.sgl1.keyed.type = SPDK_NVME_SGL_TYPE_KEYED_DATA_BLOCK;
    cmd.dptr.sgl1.keyed.subtype = SPDK_NVME_SGL_SUBTYPE_ADDRESS;
    cmd.cdw15 = g_ctx.setup_info.client_cntlid;
    
    rc = spdk_nvme_ctrlr_cmd_io_raw_with_md(g_ctx.ctrlr, g_ctx.qpair, &cmd, NULL, 0, NULL, nvme_complete, areq);
    if (rc) SPDK_ERRLOG("Failed to submit NVMe cmd: %d\n", rc);
}

static void
check_async_completions(void) {
    // Iterate all slots to check for progress conditions
    for(int i=0; i<CQ_SIZE; i++) {
        struct snic_async_req *areq = &g_ctx.active_reqs[i];
        
        if (areq->state == REQ_IAA_POLLING) {
            // Issue RDMA Read to check status
            struct ibv_sge sge = {};
            struct ibv_send_wr wr = {}, *bad_wr;

            sge.addr = (uintptr_t)areq->status_buf;
            sge.length = 1;
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

    // Status: REQ_IAA_SUBMITTED
    areq->state = REQ_IAA_SUBMITTED;
    
    // Clear Status Buffer for this slot
    memset(areq->status_buf, 0, 64);

    // Populate Descriptor 
    memset(g_ctx.desc, 0, sizeof(*g_ctx.desc));
    g_ctx.desc->opcode = IAX_OPCODE_MEMMOVE;
    g_ctx.desc->flags = IDXD_OP_FLAG_RCR | IDXD_OP_FLAG_CRAV;
    
    uint64_t comp_offset = (uint64_t)slot_idx * sizeof(struct iax_completion_record);
    g_ctx.desc->completion_addr = g_ctx.setup_info.comp_base_addr + comp_offset; 
    
    // Staging Addr
    uint64_t scratch_addr = g_ctx.setup_info.scratch_base_addr + ((uint64_t)slot_idx * MAX_DATA_SIZE);

    if (areq->op == 1) {
        g_ctx.desc->src1_addr = areq->src_addr;
        g_ctx.desc->dst_addr = scratch_addr;
    } else {
        g_ctx.desc->src1_addr = scratch_addr;
        g_ctx.desc->dst_addr = areq->dst_addr;
    }

    g_ctx.desc->src1_size = areq->len;
    
    // Send Descriptor via RDMA Write (INLINE)
    sge.addr = (uintptr_t)g_ctx.desc;
    sge.length = sizeof(*g_ctx.desc);
   
    wr.wr_id = 1000 + slot_idx; // WR_ID used to track completion (1000 base)
    wr.opcode = IBV_WR_RDMA_WRITE;
    wr.sg_list = &sge;
    wr.num_sge = 1;
    wr.send_flags = IBV_SEND_SIGNALED | IBV_SEND_INLINE;
    wr.wr.rdma.remote_addr = g_ctx.setup_info.portal_addr;
    wr.wr.rdma.rkey = g_ctx.setup_info.portal_rkey;

    int rc = ibv_post_send(g_ctx.cm_id->qp, &wr, &bad_wr);
    if (rc) {
        SPDK_ERRLOG("Failed to post Async IAA Write: %d\n", rc);
        areq->state = REQ_FREE;
        return;
    }
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

    // Alloc PD
    g_ctx.pd = ibv_alloc_pd(id->verbs);

    // Alloc Request Recv Buffer
    g_ctx.req = spdk_dma_zmalloc(sizeof(*g_ctx.req), 64, NULL);
    g_ctx.mr_req = ibv_reg_mr(g_ctx.pd, g_ctx.req, sizeof(*g_ctx.req), IBV_ACCESS_LOCAL_WRITE);

    // Alloc Descriptor Buffer
    g_ctx.desc = spdk_dma_zmalloc(sizeof(*g_ctx.desc), 64, NULL);
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
    qp_attr.cap.max_send_wr = 32; // Increase Depth for Async!
    qp_attr.cap.max_recv_wr = 32;
    qp_attr.cap.max_send_sge = 1;
    qp_attr.cap.max_recv_sge = 1;
    qp_attr.cap.max_inline_data = 64; 
    qp_attr.qp_type = IBV_QPT_RC;
    
    rc = rdma_create_qp(id, g_ctx.pd, &qp_attr);
    if (rc) return -1;

    // Post Recv for the Control Message
    struct ibv_sge sge = {
        .addr = (uintptr_t)g_ctx.req,
        .length = sizeof(*g_ctx.req),
        .lkey = g_ctx.mr_req->lkey
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
    
    return 0;
}

static void
process_request(void) {
    uint32_t slot = g_ctx.req->slot_idx;
    struct snic_async_req *areq = &g_ctx.active_reqs[slot];
    
    // Populate Async Req from Global Recv Buffer
    areq->req_id = g_ctx.req->req_id;
    areq->slot_idx = slot;
    areq->op = g_ctx.req->op;
    areq->len = g_ctx.req->len;
    areq->lba = g_ctx.req->lba;
    areq->src_addr = g_ctx.req->src_addr;
    areq->dst_addr = g_ctx.req->dst_addr;

    if (areq->op == 1) {
        // Async Submit
        submit_iaa_async(slot);
    } else if (areq->op == 2) {
        // NVMe Read (Async)
        submit_nvme_io(areq, 0); // 0 = Read 
    } else {
        SPDK_ERRLOG("Unknown Opcode: %d\n", areq->op);
    }
}

static int
on_connection(struct rdma_cm_id *id) {
    SPDK_NOTICELOG("Connection Established with Host.\n");
    return 0;
}

static int
on_disconnect(struct rdma_cm_id *id) {
    SPDK_NOTICELOG("Host Disconnected.\n");
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
    // POLL RECV CQ (Client Messages)
    if (g_ctx.cm_id && g_ctx.cm_id->qp) {
        // 1. Check Messages
        if (ibv_poll_cq(g_ctx.cm_id->qp->recv_cq, 1, &wc) > 0) {
            rc = 1; // Busy
            if (wc.status == IBV_WC_SUCCESS) {
                // Process Message
                if (!g_ctx.setup_done) {
                    // Expect SETUP Message
                    if (wc.byte_len == sizeof(struct snic_setup_msg)) {
                         memcpy(&g_ctx.setup_info, g_ctx.req, sizeof(struct snic_setup_msg));
                         g_ctx.setup_done = true;
                         SPDK_NOTICELOG("Received SETUP Message. Client QID: %d\n", g_ctx.setup_info.client_cntlid);
                    } else {
                         SPDK_ERRLOG("Expected SETUP msg (%lu bytes), got %d\n", sizeof(struct snic_setup_msg), wc.byte_len);
                    }
                } else {
                    // Expect REQUEST (Compact)
                    SPDK_NOTICELOG("Got Request Op: %d, ID: %lu, Slot: %u\n", g_ctx.req->op, g_ctx.req->req_id, g_ctx.req->slot_idx);
                    process_request();
                }

                // REPOST Recv WQE for next message
                struct ibv_sge sge = {
                    .addr = (uintptr_t)g_ctx.req,
                    .length = sizeof(*g_ctx.req),
                    .lkey = g_ctx.mr_req->lkey
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
            // SPDK_NOTICELOG("Send CQ Completion: WR_ID %lu\n", wc.wr_id);
            if (wc.wr_id >= 1000 && wc.wr_id < 2000) {
                // IAA Write Header Completed
                uint32_t slot = wc.wr_id - 1000;
                if (g_ctx.active_reqs[slot].state == REQ_IAA_SUBMITTED) {
                     g_ctx.active_reqs[slot].state = REQ_IAA_POLLING;
                }
            } else if (wc.wr_id >= 2000 && wc.wr_id < 3000) {
                // Status Read Completed
                uint32_t slot = wc.wr_id - 2000;
                struct snic_async_req *areq = &g_ctx.active_reqs[slot];
                
                if (areq->state == REQ_IAA_READ_PENDING) {
                    if (*(volatile uint8_t*)areq->status_buf != 0) {
                        // DONE!
                        // Handle Completion
                        if (areq->op == 1) {
                            // Write: IAA Done -> Submit NVMe
                             submit_nvme_io(areq, 1); // 1 = Write
                        } else {
                            // Read: IAA (Decomp) Done -> Complete
                            submit_completion(0, areq->req_id);
                            areq->state = REQ_FREE;
                        }
                    } else {
                        // Not done, retry
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
