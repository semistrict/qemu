/*
 * Pure in-process virtio-vsock device (no vhost dependency)
 *
 * Copyright 2025 Ramon Nogueira
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef QEMU_VIRTIO_VSOCK_H
#define QEMU_VIRTIO_VSOCK_H

#include "hw/virtio/virtio.h"
#include "qom/object.h"
#include "standard-headers/linux/virtio_vsock.h"

#define TYPE_VIRTIO_VSOCK "virtio-vsock-device"
OBJECT_DECLARE_SIMPLE_TYPE(VirtIOVSock, VIRTIO_VSOCK)

#define VIRTIO_VSOCK_QUEUE_SIZE 128
#define VIRTIO_VSOCK_DEFAULT_BUF_ALLOC (256 * 1024)

/* Well-known CIDs */
#define VSOCK_CID_HOST 2

typedef enum {
    VSOCK_CONN_STATE_IDLE = 0,
    VSOCK_CONN_STATE_CONNECTING,
    VSOCK_CONN_STATE_ESTABLISHED,
    VSOCK_CONN_STATE_SHUTDOWN,
    VSOCK_CONN_STATE_CLOSED,
} VsockConnState;

/* Key for connection hash table lookup */
typedef struct VsockConnKey {
    uint64_t src_cid;
    uint32_t src_port;
    uint64_t dst_cid;
    uint32_t dst_port;
} VsockConnKey;

/* Pending packet queued for delivery to guest via rx vq */
typedef struct VsockPkt {
    struct virtio_vsock_hdr hdr;
    uint8_t *payload;
    uint32_t payload_len;
    QTAILQ_ENTRY(VsockPkt) entry;
} VsockPkt;

/* Per-connection state */
typedef struct VsockConn {
    VsockConnKey key;
    VsockConnState state;
    int fd;

    /* Flow control - local (what we advertise to guest) */
    uint32_t buf_alloc;
    uint32_t fwd_cnt;
    uint32_t rx_cnt;          /* bytes received from guest */

    /* Flow control - peer (what guest advertises to us) */
    uint32_t peer_buf_alloc;
    uint32_t peer_fwd_cnt;
    uint32_t tx_cnt;          /* bytes we've sent to guest */

    uint32_t last_fwd_cnt;    /* fwd_cnt at last CREDIT_UPDATE we sent */

    uint32_t shutdown_flags;

    /* For host-to-guest: CONNECT header parsing */
    bool connect_header_parsed;
    char connect_header_buf[32];
    int connect_header_len;

    struct VirtIOVSock *vsock;
} VsockConn;

struct VirtIOVSock {
    VirtIODevice parent_obj;

    VirtQueue *rx_vq;     /* host -> guest */
    VirtQueue *tx_vq;     /* guest -> host */
    VirtQueue *event_vq;

    uint64_t guest_cid;
    char *socket_path;
    uint32_t buf_alloc;

    /* Connection tracking */
    GHashTable *conns;

    /* Pending packets for rx vq */
    QTAILQ_HEAD(, VsockPkt) rx_queue;

    /* Listener for host-to-guest connections */
    int listener_fd;
    uint32_t next_host_port; /* ephemeral port counter for host-initiated connections */

    /* BH for deferred rx pumping */
    QEMUBH *rx_bh;

    bool started;
};

#endif /* QEMU_VIRTIO_VSOCK_H */
