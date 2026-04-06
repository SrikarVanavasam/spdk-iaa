#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>

#include "fio.h"
#include "optgroup.h"

#include "../snic_client.h"
#include "../nvmf_iaa.h"

struct fio_snic_options {
    void *pad;

    char *snic_ip;
    char *target_ip;
    int   target_port;
    char *wq_path;

    unsigned int lba_shift;
    unsigned int max_xfer_size;
    unsigned int poll_usleep;
    unsigned int verbose;
};

struct fio_snic_data {
    struct snic_client_ctx *ctx;
    uint8_t *bounce;
    size_t bounce_size;
    uint32_t next_req_id;
};

static struct fio_option options[] = {
    {
        .name     = "snic_ip",
        .lname    = "SNIC IP",
        .type     = FIO_OPT_STR_STORE,
        .off1     = offsetof(struct fio_snic_options, snic_ip),
        .help     = "SNIC server IP address",
        .category = FIO_OPT_C_ENGINE,
        .group    = FIO_OPT_G_INVALID,
    },
    {
        .name     = "target_ip",
        .lname    = "Target IP",
        .type     = FIO_OPT_STR_STORE,
        .off1     = offsetof(struct fio_snic_options, target_ip),
        .help     = "SPDK target IP address",
        .category = FIO_OPT_C_ENGINE,
        .group    = FIO_OPT_G_INVALID,
    },
    {
        .name     = "target_port",
        .lname    = "Target Port",
        .type     = FIO_OPT_INT,
        .off1     = offsetof(struct fio_snic_options, target_port),
        .help     = "SPDK target port",
        .def      = "4420",
        .category = FIO_OPT_C_ENGINE,
        .group    = FIO_OPT_G_INVALID,
    },
    {
        .name     = "wq_path",
        .lname    = "IAA work queue path",
        .type     = FIO_OPT_STR_STORE,
        .off1     = offsetof(struct fio_snic_options, wq_path),
        .help     = "IAA work queue path",
        .def      = "/dev/iax/wq1.0",
        .category = FIO_OPT_C_ENGINE,
        .group    = FIO_OPT_G_INVALID,
    },
    {
        .name     = "lba_shift",
        .lname    = "LBA shift",
        .type     = FIO_OPT_INT,
        .off1     = offsetof(struct fio_snic_options, lba_shift),
        .help     = "LBA size in log2(bytes), default 9 => 512B",
        .def      = "9",
        .category = FIO_OPT_C_ENGINE,
        .group    = FIO_OPT_G_INVALID,
    },
    {
        .name     = "max_xfer_size",
        .lname    = "Max transfer size",
        .type     = FIO_OPT_INT,
        .off1     = offsetof(struct fio_snic_options, max_xfer_size),
        .help     = "Max single I/O size in bytes",
        .def      = "2097152",
        .category = FIO_OPT_C_ENGINE,
        .group    = FIO_OPT_G_INVALID,
    },
    {
        .name     = "poll_usleep",
        .lname    = "Poll sleep (us)",
        .type     = FIO_OPT_INT,
        .off1     = offsetof(struct fio_snic_options, poll_usleep),
        .help     = "Sleep interval during synchronous completion polling",
        .def      = "1000",
        .category = FIO_OPT_C_ENGINE,
        .group    = FIO_OPT_G_INVALID,
    },
    {
        .name     = "verbose",
        .lname    = "Verbose",
        .type     = FIO_OPT_BOOL,
        .off1     = offsetof(struct fio_snic_options, verbose),
        .help     = "Enable engine debug prints",
        .def      = "0",
        .category = FIO_OPT_C_ENGINE,
        .group    = FIO_OPT_G_INVALID,
    },
    {
        .name = NULL,
    },
};

static inline struct fio_snic_options *get_o(struct thread_data *td)
{
    return td->eo;
}

static inline struct fio_snic_data *get_d(struct thread_data *td)
{
    return td->io_ops_data;
}

static int fio_snic_open_file(struct thread_data fio_unused *td,
                              struct fio_file fio_unused *f)
{
    return 0;
}

static int fio_snic_close_file(struct thread_data fio_unused *td,
                               struct fio_file fio_unused *f)
{
    return 0;
}

static int fio_snic_prep(struct thread_data *td, struct io_u *io_u)
{
    struct fio_snic_options *o = get_o(td);
    uint64_t lba_size = 1ULL << o->lba_shift;

    if (io_u->ddir != DDIR_READ && io_u->ddir != DDIR_WRITE) {
        io_u->error = EOPNOTSUPP;
        return 1;
    }

    if (io_u->xfer_buflen == 0 || io_u->xfer_buflen > o->max_xfer_size) {
        io_u->error = EINVAL;
        return 1;
    }

    if (io_u->offset & (lba_size - 1)) {
        io_u->error = EINVAL;
        return 1;
    }

    if (io_u->xfer_buflen & (lba_size - 1)) {
        io_u->error = EINVAL;
        return 1;
    }

    return 0;
}

static int fio_snic_wait_req(struct thread_data *td, uint32_t expect_req_id,
                             int *out_status)
{
    struct fio_snic_options *o = get_o(td);
    struct fio_snic_data *sd = get_d(td);

    int spins = 0;

    while (1) {
        uint32_t req_id = 0;
        int status = 0;

        if (snic_client_poll(sd->ctx, &req_id, &status)) {
            if (req_id == expect_req_id) {
                *out_status = status;
                return 0;
            }

            fprintf(stderr, "unexpected completion req_id=%u expected=%u\n",
                    req_id, expect_req_id);
            *out_status = -1;
            return -1;
        }

        spins++;
        if ((spins % 5000) == 0) {
            fprintf(stderr, "waiting for req_id=%u\n", expect_req_id);
        }

        if (o->poll_usleep)
            usleep(o->poll_usleep);
    }
}

static enum fio_q_status fio_snic_queue(struct thread_data *td, struct io_u *io_u)
{
    struct fio_snic_options *o = get_o(td);
    struct fio_snic_data *sd = get_d(td);

    uint64_t lba = io_u->offset >> o->lba_shift;
    uint64_t len = io_u->xfer_buflen;
    uint32_t req_id;
    int rc, status;

    if (io_u->xfer_buflen > sd->bounce_size) {
        io_u->error = EINVAL;
        return FIO_Q_COMPLETED;
    }

    req_id = ++sd->next_req_id;

    switch (io_u->ddir) {
    case DDIR_WRITE:
        memcpy(sd->bounce, io_u->xfer_buf, io_u->xfer_buflen);

        rc = snic_client_write(sd->ctx, sd->bounce, lba, len, req_id);
        if (rc < 0) {
            io_u->error = EIO;
            return FIO_Q_COMPLETED;
        }

        rc = fio_snic_wait_req(td, req_id, &status);
        if (rc < 0 || status < 0) {
            io_u->error = EIO;
            return FIO_Q_COMPLETED;
        }

        io_u->resid = 0;
        return FIO_Q_COMPLETED;

    case DDIR_READ:
        memset(sd->bounce, 0, io_u->xfer_buflen);

        rc = snic_client_read(sd->ctx, sd->bounce, lba, len, req_id);
        if (rc < 0) {
            io_u->error = EIO;
            return FIO_Q_COMPLETED;
        }

        rc = fio_snic_wait_req(td, req_id, &status);
        if (rc < 0 || status < 0) {
            io_u->error = EIO;
            return FIO_Q_COMPLETED;
        }

        memcpy(io_u->xfer_buf, sd->bounce, io_u->xfer_buflen);
        io_u->resid = 0;
        return FIO_Q_COMPLETED;

    default:
        io_u->error = EOPNOTSUPP;
        return FIO_Q_COMPLETED;
    }
}

static int fio_snic_init(struct thread_data *td)
{
    struct fio_snic_options *o = get_o(td);
    struct fio_snic_data *sd;

    if (!o || !o->snic_ip || !o->target_ip) {
        fprintf(stderr, "fio_snic_phase1: snic_ip and target_ip are required\n");
        return 1;
    }

    if (td->o.iodepth != 1) {
        fprintf(stderr, "fio_snic_phase1: Phase 1 is synchronous; use iodepth=1\n");
    }

    sd = calloc(1, sizeof(*sd));
    if (!sd) {
        fprintf(stderr, "fio_snic_phase1: calloc failed\n");
        return 1;
    }

    sd->ctx = snic_client_init(o->snic_ip, o->target_ip,
                               o->target_port,
                               o->wq_path ? o->wq_path : "/dev/iax/wq1.0");
    if (!sd->ctx) {
        fprintf(stderr, "fio_snic_phase1: snic_client_init failed\n");
        free(sd);
        return 1;
    }

    sd->bounce_size = o->max_xfer_size ? o->max_xfer_size : MAX_DATA_SIZE;
    sd->bounce = snic_client_alloc_buffer(sd->ctx, sd->bounce_size);
    if (!sd->bounce) {
        fprintf(stderr, "fio_snic_phase1: bounce buffer allocation failed\n");
        // snic_client_fini(sd->ctx);
        free(sd);
        return 1;
    }

    sd->next_req_id = 1000;
    td->io_ops_data = sd;
    return 0;
}

static void fio_snic_cleanup(struct thread_data *td)
{
    struct fio_snic_data *sd = get_d(td);

    if (!sd)
        return;

    if (sd->bounce)
        free(sd->bounce);

    // if (sd->ctx)
    //     snic_client_fini(sd->ctx);

    free(sd);
    td->io_ops_data = NULL;
}

struct ioengine_ops ioengine = {
    .name               = "snic_phase1",
    .version            = FIO_IOOPS_VERSION,
    .flags              = FIO_SYNCIO,

    .init               = fio_snic_init,
    .prep               = fio_snic_prep,
    .queue              = fio_snic_queue,
    .cleanup            = fio_snic_cleanup,
    .open_file          = fio_snic_open_file,
    .close_file         = fio_snic_close_file,

    .options            = options,
    .option_struct_size = sizeof(struct fio_snic_options),
};