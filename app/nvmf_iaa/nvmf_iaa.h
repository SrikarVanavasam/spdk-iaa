#ifndef NVMF_IAA_H
#define NVMF_IAA_H

#include <stdint.h>

#define SNIC_PORT 18515
#define CQ_SIZE 32
#define MAX_DATA_SIZE (2 * 1024 * 1024)
#define AECS_SIZE 1568

// Operation Codes
#define SNIC_OP_WRITE 1
#define SNIC_OP_READ  2

// Completion Structure
struct snic_completion {
    uint64_t req_id;
    int status;
};

// Setup/Config Message 
struct snic_setup_msg {
    uint64_t scratch_base_addr;
    uint64_t portal_addr;
    uint64_t cq_base_addr;
    uint64_t comp_base_addr;

    uint64_t comp_aecs_addr;
    uint64_t decomp_aecs_addr;

    uint32_t scratch_rkey;
    uint32_t scratch_target_rkey;
    uint32_t portal_rkey;
    uint32_t cq_rkey;
    uint32_t comp_rkey;

    uint32_t comp_aecs_size;
    uint32_t decomp_aecs_size;

    uint16_t client_cntlid;
    uint8_t reserved[2];
};

// Compact Request Structure (<= 64 bytes)
struct snic_request {
    uint64_t req_id;
    uint64_t lba; // NVMe LBA
    
    uint64_t src_addr; // User VA
    uint64_t dst_addr; // User VA (for Read)
    
    uint32_t len;
    uint32_t slot_idx; // Slot Index (0..CQ_SIZE-1) to derive Scratch/Comp offsets
    
    int op;
    uint8_t pad[16]; // Pad to 64 bytes (Total 48 bytes data + 16 pad = 64)
};

#endif // NVMF_IAA_H
