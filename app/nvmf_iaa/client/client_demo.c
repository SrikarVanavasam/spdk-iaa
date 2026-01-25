#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include "../snic_client.h"

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
    while(1) {
        int cid;
        int status;
        if (snic_client_poll(ctx, &cid, &status)) {
            printf("Completion Received! ID: %u, Status: %d\n", cid, status);
            if (cid == req1) break;
        }
    }

    // 2. Read
    memset(buf, 0x00, size);
    int req2 = 102;
    snic_client_read(ctx, buf, 2, size, req2);
    printf("Submitted Req %d (Read). Waiting for completion...\n", req2);

    // Poll for Read
    while(1) {
        int cid;
        int status;
        if (snic_client_poll(ctx, &cid, &status)) {
            printf("Completion Received! ID: %u, Status: %d\n", cid, status);
            if (cid == req2) break;
        }
    }

    // Verify
    if (((uint8_t*)buf)[0] == 0xAA) {
        printf("SUCCESS: Read Data Verified (0xAA)\n");
    } else {
        printf("FAILURE: Data Mismatch. Expected 0xAA, Got 0x%02x\n", ((uint8_t*)buf)[0]);
    }

    return 0;
}
