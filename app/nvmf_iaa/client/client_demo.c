#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include "../snic_client.h"
#include "idxd.h" // [IAA_COMP_UPDATE]

// Helper: Die
static void die(const char *reason) {
  perror(reason);
  exit(EXIT_FAILURE);
}

int main(int argc, char *argv[]) {
    if (argc < 4) {
        fprintf(stderr, "Usage: %s <snic_ip> <target_ip> <target_port> [iax_wq_path]\n", argv[0]);
        return 1;
    }

    struct snic_client_ctx *ctx = snic_client_init(argv[1], argv[2], atoi(argv[3]), (argc > 4) ? argv[4] : "/dev/iax/wq1.0");
    
    printf("Client Initialized. Mode: Async Ring Buffer.\n");

    // Applicaton Logic
    size_t size = 65536;
    void *buf = snic_client_alloc_buffer(ctx, size);
    if (!buf) die("alloc buffer");
    
    // 1. Write
    memset(buf, 0xAA, size);
    int req1 = 101; 
    snic_client_write(ctx, buf, 2, size, req1);
    printf("Submitted Req %d (Write). Waiting for completion...\n", req1);

    // Poll for Write
    // while(1) {
        //     int cid;
        //     int status;
        //     if (snic_client_poll(ctx, &cid, &status)) {
            //         printf("Completion Received! ID: %u, Status: %d\n", cid, status);
            //         if (cid == req1) break;
            
            //         // dump compressed data
            //         uint8_t *comp_ptr =
            //             (uint8_t *)((uint8_t*)ctx->scratch_buf + (uint64_t)slot * MAX_DATA_SIZE);
            //         uint32_t comp_len = cr->output_size;
            
            //         printf("Compressed output: first 16 bytes:\n");
            //         for (int i = 0; i < 16 && i < (int)comp_len; i++) printf("%02x ", comp_ptr[i]);
            //         printf("\n");
            
            //         // [UPDATE_END]
            //     }
            // }
            
    int slot = 2;   // [IAA_COMP_UPDATE] Both write/read have slot_idx=2
    while (1) {
        int cid;
        int status;
        if (snic_client_poll(ctx, &cid, &status)) {
            printf("Completion Received! ID: %u, Status: %d\n", cid, status);

            if (cid == req1) {
                struct iax_completion_record *cr =
                    (struct iax_completion_record *)((uint8_t *)snic_client_get_comp_base(ctx)
                        + (size_t)slot * sizeof(struct iax_completion_record));

                printf("IAA CR: status=0x%02x error=0x%02x output_size=%u bytes_completed=%u invalid_flags=0x%08x\n",
                    cr->status, cr->error_code, cr->output_size, cr->bytes_completed, cr->invalid_flags);

                uint8_t *comp_ptr =
                    (uint8_t *)((uint8_t *)snic_client_get_scratch_base(ctx)
                        + (size_t)slot * (size_t)MAX_DATA_SIZE);

                uint32_t comp_len = cr->output_size;

                printf("Compressed output: first 16 bytes:\n");
                for (int i = 0; i < 16 && i < (int)comp_len; i++) {
                    printf("%02x ", comp_ptr[i]);
                }
                printf("\n");

                break;
            }
        }
    }

    // [IAA_COMP_UPDATE] ignore the reading part for now, since it is compress-only workflow now, haven't considered the reading part yet
    // // 2. Read
    // memset(buf, 0x00, size);
    // int req2 = 102;
    // snic_client_read(ctx, buf, 2, size, req2);
    // printf("Submitted Req %d (Read). Waiting for completion...\n", req2);

    // // Poll for Read
    // while(1) {
    //     int cid;
    //     int status;
    //     if (snic_client_poll(ctx, &cid, &status)) {
    //         printf("Completion Received! ID: %u, Status: %d\n", cid, status);
    //         if (cid == req2) break;
    //     }
    // }

    // // Verify
    // if (((uint8_t*)buf)[0] == 0xAA) {
    //     printf("SUCCESS: Read Data Verified (0xAA)\n");
    // } else {
    //     printf("FAILURE: Data Mismatch. Expected 0xAA, Got 0x%02x\n", ((uint8_t*)buf)[0]);
    // }

    return 0;
}
