#ifndef SNIC_CLIENT_H
#define SNIC_CLIENT_H

#include <stddef.h>
#include <stdint.h>
#include "nvmf_iaa.h"

// Forward declaration
struct snic_client_ctx;

// Initialize connection to SNIC and Storage Target
// wq_path: Path to IAA Work Queue (e.g., /dev/iax/wq1.0)
struct snic_client_ctx *snic_client_init(const char *snic_ip, const char *target_ip, int target_port, const char *wq_path);

// Teardown
void snic_client_fini(struct snic_client_ctx *ctx);

// Allocate DMA-capable memory (Hugepages) registered with the NIC
// Returns pointer to buffer, sets handle internally
void *snic_client_alloc_buffer(struct snic_client_ctx *ctx, size_t size);

// 3. Submit Write Request
// Returns 0 on success, -1 on failure.
int snic_client_write(struct snic_client_ctx *ctx, void *src_buf, uint64_t lba, uint64_t len, uint32_t req_id);

// 4. Submit Read Request
// Returns 0 on success, -1 on failure.
int snic_client_read(struct snic_client_ctx *ctx, void *dst_buf, uint64_t lba, uint64_t len, uint32_t req_id);

// 5. Poll for Completion (Non-blocking)
// Returns 1 if completion found (populating req_id and status), 0 if empty.
int snic_client_poll(struct snic_client_ctx *ctx, uint32_t *req_id, int *status);

#endif // SNIC_CLIENT_H
