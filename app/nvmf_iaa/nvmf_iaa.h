#ifndef NVMF_IAA_H
#define NVMF_IAA_H

#include <stdint.h>

#define SNIC_PORT 18515
#define CQ_SIZE 16
#define MAX_DATA_SIZE (2 * 1024 * 1024)

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
    // 8-byte aligned members (32 bytes)
    uint64_t scratch_base_addr;
    uint64_t portal_addr;
    uint64_t cq_base_addr;
    uint64_t comp_base_addr;
    
    // 4-byte aligned members (20 bytes)
    uint32_t scratch_rkey;       // RKey for SNIC access
    uint32_t scratch_target_rkey; // RKey for Target access (NVMe-oF)
    uint32_t portal_rkey;
    uint32_t cq_rkey;
    uint32_t comp_rkey;
    
    // 2-byte aligned members (4 bytes)
    uint16_t client_cntlid;
    
    // Padding to 64 bytes (10 bytes)
    uint8_t reserved[10]; 
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
