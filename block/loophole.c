/*
 * Loophole block driver
 *
 * Delegates read/write/flush to a loophole volume via C FFI into a
 * statically-linked Go archive (libloophole.a).
 *
 * Since the Go library may block (CGo), all I/O is offloaded to the
 * QEMU thread pool via thread_pool_submit_co().
 *
 * Usage:
 *   -drive driver=loophole,volume=VOLNAME
 *   -drive driver=loophole,volume=VOLNAME,config-dir=/path,profile=PROF
 *
 * Copyright 2025 Semistrict LLC
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qobject/qdict.h"
#include "qemu/module.h"
#include "qemu/option.h"
#include "block/block-io.h"
#include "block/block_int.h"
#include "block/thread-pool.h"

/* FFI declarations — symbols come from libloophole.a (Go c-archive). */
extern int64_t loophole_open(const char *config_dir, const char *profile,
                             const char *name, uint32_t name_len);
extern int32_t loophole_read(int64_t handle, void *buf,
                             uint64_t offset, uint32_t count);
extern int32_t loophole_write(int64_t handle, const void *buf,
                              uint64_t offset, uint32_t count);
extern int32_t loophole_flush(int64_t handle);
extern uint64_t loophole_size(int64_t handle);
extern int32_t loophole_close(int64_t handle);

#define OPT_VOLUME     "volume"
#define OPT_CONFIG_DIR "config-dir"
#define OPT_PROFILE    "profile"

typedef struct BDRVLoopholeState {
    int64_t handle;
    uint64_t size;
} BDRVLoopholeState;

static QemuOptsList runtime_opts = {
    .name = "loophole",
    .head = QTAILQ_HEAD_INITIALIZER(runtime_opts.head),
    .desc = {
        {
            .name = OPT_VOLUME,
            .type = QEMU_OPT_STRING,
            .help = "loophole volume name",
        },
        {
            .name = OPT_CONFIG_DIR,
            .type = QEMU_OPT_STRING,
            .help = "path to loophole config directory",
        },
        {
            .name = OPT_PROFILE,
            .type = QEMU_OPT_STRING,
            .help = "loophole profile name",
        },
        { /* end of list */ }
    },
};

static int loophole_open_fn(BlockDriverState *bs, QDict *options, int flags,
                            Error **errp)
{
    BDRVLoopholeState *s = bs->opaque;
    QemuOpts *opts;
    const char *volume;
    const char *config_dir;
    const char *profile;
    int64_t h;

    opts = qemu_opts_create(&runtime_opts, NULL, 0, &error_abort);
    qemu_opts_absorb_qdict(opts, options, &error_abort);

    volume = qemu_opt_get(opts, OPT_VOLUME);
    if (!volume || !volume[0]) {
        error_setg(errp, "loophole: 'volume' option is required");
        qemu_opts_del(opts);
        return -EINVAL;
    }

    config_dir = qemu_opt_get(opts, OPT_CONFIG_DIR);
    profile = qemu_opt_get(opts, OPT_PROFILE);

    h = loophole_open(config_dir, profile, volume, strlen(volume));
    if (h < 0) {
        error_setg(errp, "loophole_open failed with code %" PRId64, h);
        qemu_opts_del(opts);
        return -EIO;
    }

    s->handle = h;
    s->size = loophole_size(h);

    qemu_opts_del(opts);
    return 0;
}

static void loophole_close_fn(BlockDriverState *bs)
{
    BDRVLoopholeState *s = bs->opaque;
    if (s->handle > 0) {
        loophole_close(s->handle);
        s->handle = 0;
    }
}

static int64_t coroutine_fn loophole_co_getlength(BlockDriverState *bs)
{
    BDRVLoopholeState *s = bs->opaque;
    return (int64_t)s->size;
}

/* ---- Thread-pool worker structs and functions ---- */

typedef struct LoopholeReadReq {
    int64_t handle;
    void *buf;
    uint64_t offset;
    uint32_t count;
} LoopholeReadReq;

static int loophole_read_worker(void *opaque)
{
    LoopholeReadReq *req = opaque;
    int32_t rc = loophole_read(req->handle, req->buf, req->offset, req->count);
    if (rc < 0) {
        return -EIO;
    }
    return 0;
}

typedef struct LoopholeWriteReq {
    int64_t handle;
    const void *buf;
    uint64_t offset;
    uint32_t count;
} LoopholeWriteReq;

static int loophole_write_worker(void *opaque)
{
    LoopholeWriteReq *req = opaque;
    int32_t rc = loophole_write(req->handle, req->buf, req->offset, req->count);
    if (rc < 0) {
        return -EIO;
    }
    return 0;
}

static int loophole_flush_worker(void *opaque)
{
    int64_t handle = *(int64_t *)opaque;
    int32_t rc = loophole_flush(handle);
    if (rc < 0) {
        return -EIO;
    }
    return 0;
}

/* ---- Coroutine I/O callbacks ---- */

static int coroutine_fn loophole_co_preadv(BlockDriverState *bs,
                                           int64_t offset, int64_t bytes,
                                           QEMUIOVector *qiov,
                                           BdrvRequestFlags flags)
{
    BDRVLoopholeState *s = bs->opaque;

    /*
     * For simplicity, linearize into a bounce buffer. The loophole C API
     * takes a flat pointer, not an iovec.
     */
    uint64_t remaining = bytes;
    uint64_t pos = offset;
    int iov_idx = 0;
    size_t iov_off = 0;

    while (remaining > 0) {
        struct iovec *iov = &qiov->iov[iov_idx];
        size_t chunk = MIN(remaining, iov->iov_len - iov_off);

        LoopholeReadReq req = {
            .handle = s->handle,
            .buf = (uint8_t *)iov->iov_base + iov_off,
            .offset = pos,
            .count = (uint32_t)chunk,
        };

        int ret = thread_pool_submit_co(loophole_read_worker, &req);
        if (ret < 0) {
            return ret;
        }

        pos += chunk;
        remaining -= chunk;
        iov_off += chunk;
        if (iov_off >= iov->iov_len) {
            iov_idx++;
            iov_off = 0;
        }
    }

    return 0;
}

static int coroutine_fn loophole_co_pwritev(BlockDriverState *bs,
                                            int64_t offset, int64_t bytes,
                                            QEMUIOVector *qiov,
                                            BdrvRequestFlags flags)
{
    BDRVLoopholeState *s = bs->opaque;

    uint64_t remaining = bytes;
    uint64_t pos = offset;
    int iov_idx = 0;
    size_t iov_off = 0;

    while (remaining > 0) {
        struct iovec *iov = &qiov->iov[iov_idx];
        size_t chunk = MIN(remaining, iov->iov_len - iov_off);

        LoopholeWriteReq req = {
            .handle = s->handle,
            .buf = (const uint8_t *)iov->iov_base + iov_off,
            .offset = pos,
            .count = (uint32_t)chunk,
        };

        int ret = thread_pool_submit_co(loophole_write_worker, &req);
        if (ret < 0) {
            return ret;
        }

        pos += chunk;
        remaining -= chunk;
        iov_off += chunk;
        if (iov_off >= iov->iov_len) {
            iov_idx++;
            iov_off = 0;
        }
    }

    return 0;
}

static int coroutine_fn loophole_co_flush(BlockDriverState *bs)
{
    BDRVLoopholeState *s = bs->opaque;
    int64_t handle = s->handle;
    return thread_pool_submit_co(loophole_flush_worker, &handle);
}

static int loophole_reopen_prepare(BDRVReopenState *reopen_state,
                                   BlockReopenQueue *queue, Error **errp)
{
    return 0;
}

static const char *const loophole_strong_runtime_opts[] = {
    OPT_VOLUME,
    OPT_CONFIG_DIR,
    OPT_PROFILE,
    NULL,
};

static BlockDriver bdrv_loophole = {
    .format_name            = "loophole",
    .protocol_name          = "loophole",
    .instance_size          = sizeof(BDRVLoopholeState),

    .bdrv_open              = loophole_open_fn,
    .bdrv_close             = loophole_close_fn,
    .bdrv_co_getlength      = loophole_co_getlength,

    .bdrv_co_preadv         = loophole_co_preadv,
    .bdrv_co_pwritev        = loophole_co_pwritev,
    .bdrv_co_flush_to_disk  = loophole_co_flush,
    .bdrv_reopen_prepare    = loophole_reopen_prepare,

    .strong_runtime_opts    = loophole_strong_runtime_opts,
};

static void bdrv_loophole_init(void)
{
    bdrv_register(&bdrv_loophole);
}

block_init(bdrv_loophole_init);
