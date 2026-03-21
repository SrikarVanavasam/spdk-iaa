#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include "../snic_client.h"
#include "idxd.h"

static void die(const char *reason) {
    perror(reason);
    exit(EXIT_FAILURE);
}

static void print_buf_hex(const void *buf, size_t len)
{
    const uint8_t *p = (const uint8_t *)buf;
    for (size_t i = 0; i < len; i++) {
        if (i % 16 == 0) {
            printf("%04zx: ", i);
        }
        printf("%02x ", p[i]);
        if (i % 16 == 15 || i == len - 1) {
            printf("\n");
        }
    }
}

static int wait_for_req(struct snic_client_ctx *ctx, uint32_t expect_req)
{
    while (1) {
        uint32_t cid;
        int status;
        if (snic_client_poll(ctx, &cid, &status)) {
            printf("Completion Received! ID: %u, Status: %d\n", cid, status);
            if (cid == expect_req) {
                return status;
            }
        }
    }
}

static int compare_bytes(const void *a, const void *b, size_t len)
{
    const uint8_t *pa = (const uint8_t *)a;
    const uint8_t *pb = (const uint8_t *)b;

    for (size_t i = 0; i < len; i++) {
        if (pa[i] != pb[i]) {
            printf("Mismatch at byte %zu: 0x%02x vs 0x%02x\n",
                   i, pa[i], pb[i]);
            return -1;
        }
    }
    return 0;
}

int main(int argc, char *argv[])
{
    if (argc < 4) {
        fprintf(stderr, "Usage: %s <snic_ip> <target_ip> <target_port> [iax_wq_path]\n", argv[0]);
        return 1;
    }

    struct snic_client_ctx *ctx =
        snic_client_init(argv[1], argv[2], atoi(argv[3]),
                         (argc > 4) ? argv[4] : "/dev/iax/wq1.0");

    printf("Client Initialized. Mode: Async Ring Buffer.\n");

    printf("Sleeping....\n");
    sleep(5);

    size_t orig_len = 2 * 1024;
    void *buf = snic_client_alloc_buffer(ctx, orig_len);
    if (!buf) die("alloc buffer");

    uint8_t *scratch_base = (uint8_t *)snic_client_get_scratch_base(ctx);
    if (!scratch_base) die("scratch base");

    struct iax_completion_record *comp_base =
        (struct iax_completion_record *)snic_client_get_comp_base(ctx);
    if (!comp_base) die("comp base");

    uint8_t *scratch_slot0 = scratch_base + 0 * MAX_DATA_SIZE;
    uint8_t *scratch_slot1 = scratch_base + 1 * MAX_DATA_SIZE;

    /* clean up */
    memset(scratch_slot0, 0, 256);
    memset(scratch_slot1, 0, 256);

    /* 1. WRITE: init 0xAA */
    memset(buf, 0xAA, orig_len);
    uint32_t req1 = 101;

    snic_client_write(ctx, buf, 2, orig_len, req1);
    printf("Submitted Req %u (Write). Waiting for completion...\n", req1);

    if (wait_for_req(ctx, req1) < 0) {
        fprintf(stderr, "Write failed\n");
        return 1;
    }

    /*
     * slot0
     * client: comp_buf[0] records IAA completion record。
     */
    uint32_t write_comp_len = comp_base[0].output_size;
    printf("\n[VERIFY] write_comp_len (slot0 CR.output_size) = %u\n", write_comp_len);

    if (write_comp_len == 0 || write_comp_len > MAX_DATA_SIZE) {
        fprintf(stderr, "Invalid write_comp_len = %u\n", write_comp_len);
        return 1;
    }

    printf("\n[VERIFY] scratch slot0 first %uB after WRITE:\n", write_comp_len);
    print_buf_hex(scratch_slot0, write_comp_len);

    /* 2. READ: without decomp, just read and verify */
    memset(buf, 0x00, orig_len);
    uint32_t req2 = 102;

    snic_client_read(ctx, buf, 2, orig_len, req2);
    printf("Submitted Req %u (Read). Waiting for completion...\n", req2);

    if (wait_for_req(ctx, req2) < 0) {
        fprintf(stderr, "Read failed\n");
        return 1;
    }

    /*
     * The second request uses slot1.
    * At the current stage, read should no longer trigger IAA, so comp_base[1] is not used as the length reference.
    * We use the write_comp_len recorded during write for comparison.
     */
    printf("\n[VERIFY] scratch slot1 first %uB after READ:\n", write_comp_len);
    print_buf_hex(scratch_slot1, write_comp_len);

    printf("\n[VERIFY] Compare scratch slot0 and slot1 first %uB...\n", write_comp_len);
    if (compare_bytes(scratch_slot0, scratch_slot1, write_comp_len) == 0) {
        printf("SUCCESS: compressed payload matches (slot0 == slot1)\n");
    } else {
        printf("FAILURE: compressed payload mismatch (slot0 != slot1)\n");
    }

    return 0;
}