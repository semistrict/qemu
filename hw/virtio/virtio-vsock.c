/*
 * Pure in-process virtio-vsock device (no vhost dependency)
 *
 * Implements the virtio-vsock transport entirely within the QEMU process,
 * exposing guest vsock connections via Unix domain sockets on the host.
 * This enables vsock on platforms without vhost support (e.g. macOS).
 *
 * Host-side interface (Firecracker-style):
 *   Guest->Host: guest connects to host CID port X =>
 *                QEMU connects to {socket-path}_{X}
 *   Host->Guest: host connects to {socket-path}, sends "CONNECT {port}\n" =>
 *                QEMU sends REQUEST to guest
 *
 * Copyright 2025 Ramon Nogueira
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/iov.h"
#include "qemu/error-report.h"
#include "qemu/main-loop.h"
#include "qemu/module.h"
#include "qapi/error.h"
#include "hw/core/qdev-properties.h"
#include "hw/virtio/virtio.h"
#include "hw/virtio/virtio-vsock.h"
#include "trace.h"

#include <sys/socket.h>
#include <sys/un.h>

/* ----------------------------------------------------------------
 * Connection hash table helpers
 * ---------------------------------------------------------------- */

static guint vsock_conn_key_hash(gconstpointer v)
{
    const VsockConnKey *k = v;
    return g_int64_hash(&k->src_cid) ^
           g_int_hash(&k->src_port) ^
           g_int64_hash(&k->dst_cid) ^
           g_int_hash(&k->dst_port);
}

static gboolean vsock_conn_key_equal(gconstpointer a, gconstpointer b)
{
    const VsockConnKey *ka = a;
    const VsockConnKey *kb = b;
    return ka->src_cid == kb->src_cid &&
           ka->src_port == kb->src_port &&
           ka->dst_cid == kb->dst_cid &&
           ka->dst_port == kb->dst_port;
}

static VsockConn *vsock_conn_find(VirtIOVSock *s, const VsockConnKey *key)
{
    return g_hash_table_lookup(s->conns, key);
}

static void vsock_unix_read(void *opaque);

static VsockConn *vsock_conn_new(VirtIOVSock *s, const VsockConnKey *key,
                                 int fd)
{
    VsockConn *conn = g_new0(VsockConn, 1);
    conn->key = *key;
    conn->fd = fd;
    conn->buf_alloc = s->buf_alloc;
    conn->vsock = s;
    g_hash_table_insert(s->conns, &conn->key, conn);
    return conn;
}

static void vsock_conn_close(VirtIOVSock *s, VsockConn *conn)
{
    if (conn->fd >= 0) {
        qemu_set_fd_handler(conn->fd, NULL, NULL, NULL);
        close(conn->fd);
        conn->fd = -1;
    }
    conn->state = VSOCK_CONN_STATE_CLOSED;
    g_hash_table_remove(s->conns, &conn->key);
}

static void vsock_conn_close_all(VirtIOVSock *s)
{
    GHashTableIter iter;
    gpointer value;

    g_hash_table_iter_init(&iter, s->conns);
    while (g_hash_table_iter_next(&iter, NULL, &value)) {
        VsockConn *conn = value;
        if (conn->fd >= 0) {
            qemu_set_fd_handler(conn->fd, NULL, NULL, NULL);
            close(conn->fd);
            conn->fd = -1;
        }
        conn->state = VSOCK_CONN_STATE_CLOSED;
        g_hash_table_iter_remove(&iter);
    }
}

/* Free a connection (GDestroyNotify for hash table) */
static void vsock_conn_free(gpointer data)
{
    g_free(data);
}

/* ----------------------------------------------------------------
 * Flow control helpers
 * ---------------------------------------------------------------- */

/* How many bytes the peer (guest) can still accept from us */
static uint32_t vsock_conn_peer_credit(VsockConn *conn)
{
    if (conn->peer_buf_alloc == 0) {
        return 0;
    }
    uint32_t used = conn->tx_cnt - conn->peer_fwd_cnt;
    if (used >= conn->peer_buf_alloc) {
        return 0;
    }
    return conn->peer_buf_alloc - used;
}

/* ----------------------------------------------------------------
 * RX queue: host -> guest packet delivery
 * ---------------------------------------------------------------- */

static void vsock_rx_pkt_queue(VirtIOVSock *s, struct virtio_vsock_hdr *hdr,
                               const uint8_t *payload, uint32_t payload_len)
{
    VsockPkt *pkt = g_new0(VsockPkt, 1);
    pkt->hdr = *hdr;
    if (payload && payload_len > 0) {
        pkt->payload = g_memdup2(payload, payload_len);
        pkt->payload_len = payload_len;
    }
    QTAILQ_INSERT_TAIL(&s->rx_queue, pkt, entry);
    qemu_bh_schedule(s->rx_bh);
}

static void vsock_rx_pump(void *opaque)
{
    VirtIOVSock *s = opaque;
    VirtIODevice *vdev = VIRTIO_DEVICE(s);

    if (!s->started || !virtio_queue_ready(s->rx_vq)) {
        return;
    }

    while (!QTAILQ_EMPTY(&s->rx_queue)) {
        VsockPkt *pkt = QTAILQ_FIRST(&s->rx_queue);
        VirtQueueElement *elem;

        elem = virtqueue_pop(s->rx_vq, sizeof(VirtQueueElement));
        if (!elem) {
            /* No available rx buffers; will retry when guest posts more */
            break;
        }

        size_t copied = iov_from_buf(elem->in_sg, elem->in_num, 0,
                                     &pkt->hdr, sizeof(pkt->hdr));
        if (pkt->payload && pkt->payload_len > 0) {
            copied += iov_from_buf(elem->in_sg, elem->in_num,
                                   sizeof(pkt->hdr),
                                   pkt->payload, pkt->payload_len);
        }

        virtqueue_push(s->rx_vq, elem, copied);
        g_free(elem);

        QTAILQ_REMOVE(&s->rx_queue, pkt, entry);
        g_free(pkt->payload);
        g_free(pkt);
    }

    virtio_notify(vdev, s->rx_vq);
}

/* ----------------------------------------------------------------
 * Packet construction helpers
 * ---------------------------------------------------------------- */

static void vsock_init_hdr(struct virtio_vsock_hdr *hdr,
                           uint64_t src_cid, uint32_t src_port,
                           uint64_t dst_cid, uint32_t dst_port,
                           uint16_t op, uint16_t type,
                           uint32_t len, VsockConn *conn)
{
    memset(hdr, 0, sizeof(*hdr));
    hdr->src_cid = cpu_to_le64(src_cid);
    hdr->dst_cid = cpu_to_le64(dst_cid);
    hdr->src_port = cpu_to_le32(src_port);
    hdr->dst_port = cpu_to_le32(dst_port);
    hdr->len = cpu_to_le32(len);
    hdr->type = cpu_to_le16(type);
    hdr->op = cpu_to_le16(op);
    if (conn) {
        hdr->buf_alloc = cpu_to_le32(conn->buf_alloc);
        hdr->fwd_cnt = cpu_to_le32(conn->fwd_cnt);
    }
}

static void vsock_send_response(VirtIOVSock *s, VsockConn *conn)
{
    struct virtio_vsock_hdr hdr;
    vsock_init_hdr(&hdr, VSOCK_CID_HOST, conn->key.dst_port,
                   s->guest_cid, conn->key.src_port,
                   VIRTIO_VSOCK_OP_RESPONSE, VIRTIO_VSOCK_TYPE_STREAM,
                   0, conn);
    vsock_rx_pkt_queue(s, &hdr, NULL, 0);
}

static void vsock_send_rst_to_guest(VirtIOVSock *s,
                                    uint64_t src_cid, uint32_t src_port,
                                    uint64_t dst_cid, uint32_t dst_port)
{
    struct virtio_vsock_hdr hdr;
    vsock_init_hdr(&hdr, src_cid, src_port, dst_cid, dst_port,
                   VIRTIO_VSOCK_OP_RST, VIRTIO_VSOCK_TYPE_STREAM,
                   0, NULL);
    vsock_rx_pkt_queue(s, &hdr, NULL, 0);
}

static void vsock_send_credit_update(VirtIOVSock *s, VsockConn *conn)
{
    struct virtio_vsock_hdr hdr;
    vsock_init_hdr(&hdr, VSOCK_CID_HOST, conn->key.dst_port,
                   s->guest_cid, conn->key.src_port,
                   VIRTIO_VSOCK_OP_CREDIT_UPDATE, VIRTIO_VSOCK_TYPE_STREAM,
                   0, conn);
    vsock_rx_pkt_queue(s, &hdr, NULL, 0);
}

static void vsock_send_shutdown(VirtIOVSock *s, VsockConn *conn,
                                uint32_t flags)
{
    struct virtio_vsock_hdr hdr;
    vsock_init_hdr(&hdr, VSOCK_CID_HOST, conn->key.dst_port,
                   s->guest_cid, conn->key.src_port,
                   VIRTIO_VSOCK_OP_SHUTDOWN, VIRTIO_VSOCK_TYPE_STREAM,
                   0, conn);
    hdr.flags = cpu_to_le32(flags);
    vsock_rx_pkt_queue(s, &hdr, NULL, 0);
}

static void vsock_send_request(VirtIOVSock *s, VsockConn *conn)
{
    struct virtio_vsock_hdr hdr;
    vsock_init_hdr(&hdr, VSOCK_CID_HOST, conn->key.dst_port,
                   s->guest_cid, conn->key.src_port,
                   VIRTIO_VSOCK_OP_REQUEST, VIRTIO_VSOCK_TYPE_STREAM,
                   0, conn);
    vsock_rx_pkt_queue(s, &hdr, NULL, 0);
}

/* ----------------------------------------------------------------
 * Unix socket I/O: data from host-side sockets into guest
 * ---------------------------------------------------------------- */

static void vsock_unix_read(void *opaque)
{
    VsockConn *conn = opaque;
    VirtIOVSock *s = conn->vsock;
    uint8_t buf[65536];
    ssize_t n;
    uint32_t credit;

    if (conn->state != VSOCK_CONN_STATE_ESTABLISHED) {
        return;
    }

    credit = vsock_conn_peer_credit(conn);
    if (credit == 0) {
        /* No credit; stop reading until guest sends CREDIT_UPDATE */
        qemu_set_fd_handler(conn->fd, NULL, NULL, NULL);
        return;
    }

    size_t to_read = MIN(sizeof(buf), credit);
    n = read(conn->fd, buf, to_read);
    if (n <= 0) {
        if (n < 0 && (errno == EAGAIN || errno == EINTR)) {
            return;
        }
        /* EOF or error: send SHUTDOWN+RST to guest */
        vsock_send_shutdown(s, conn,
                            VIRTIO_VSOCK_SHUTDOWN_RCV | VIRTIO_VSOCK_SHUTDOWN_SEND);
        qemu_set_fd_handler(conn->fd, NULL, NULL, NULL);
        return;
    }

    /* Queue data packet for guest */
    struct virtio_vsock_hdr hdr;
    vsock_init_hdr(&hdr, VSOCK_CID_HOST, conn->key.dst_port,
                   s->guest_cid, conn->key.src_port,
                   VIRTIO_VSOCK_OP_RW, VIRTIO_VSOCK_TYPE_STREAM,
                   n, conn);
    vsock_rx_pkt_queue(s, &hdr, buf, n);
    conn->tx_cnt += n;
}

/* ----------------------------------------------------------------
 * Guest-to-host connection: connect to {socket-path}_{port}
 * ---------------------------------------------------------------- */

static int vsock_connect_host_socket(VirtIOVSock *s, uint32_t port)
{
    char *path = g_strdup_printf("%s_%u", s->socket_path, port);
    struct sockaddr_un addr;
    int fd;

    fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        g_free(path);
        return -1;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    if (strlen(path) >= sizeof(addr.sun_path)) {
        close(fd);
        g_free(path);
        return -1;
    }
    strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1);
    g_free(path);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(fd);
        return -1;
    }

    /* Set non-blocking */
    int flags = fcntl(fd, F_GETFL);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);

    return fd;
}

/* ----------------------------------------------------------------
 * TX virtqueue handler: guest -> host packets
 * ---------------------------------------------------------------- */

static void virtio_vsock_handle_tx(VirtIODevice *vdev, VirtQueue *vq)
{
    VirtIOVSock *s = VIRTIO_VSOCK(vdev);
    VirtQueueElement *elem;

    if (!s->started) {
        return;
    }

    while ((elem = virtqueue_pop(vq, sizeof(VirtQueueElement))) != NULL) {
        struct virtio_vsock_hdr hdr;
        size_t hdr_len;

        hdr_len = iov_to_buf(elem->out_sg, elem->out_num, 0,
                             &hdr, sizeof(hdr));
        if (hdr_len < sizeof(hdr)) {
            virtqueue_push(vq, elem, 0);
            g_free(elem);
            continue;
        }

        uint64_t src_cid = le64_to_cpu(hdr.src_cid);
        uint32_t src_port = le32_to_cpu(hdr.src_port);
        uint64_t dst_cid = le64_to_cpu(hdr.dst_cid);
        uint32_t dst_port = le32_to_cpu(hdr.dst_port);
        uint16_t op = le16_to_cpu(hdr.op);
        uint32_t len = le32_to_cpu(hdr.len);

        /* Validate: guest src_cid must match configured CID */
        if (src_cid != s->guest_cid) {
            virtqueue_push(vq, elem, 0);
            g_free(elem);
            continue;
        }

        VsockConnKey key = {
            .src_cid = src_cid,
            .src_port = src_port,
            .dst_cid = dst_cid,
            .dst_port = dst_port,
        };
        VsockConn *conn;

        switch (op) {
        case VIRTIO_VSOCK_OP_REQUEST: {
            /* Guest initiating connection to host */
            if (dst_cid != VSOCK_CID_HOST) {
                vsock_send_rst_to_guest(s, dst_cid, dst_port,
                                        src_cid, src_port);
                break;
            }
            conn = vsock_conn_find(s, &key);
            if (conn) {
                /* Duplicate request; RST */
                vsock_send_rst_to_guest(s, VSOCK_CID_HOST, dst_port,
                                        src_cid, src_port);
                break;
            }
            int fd = vsock_connect_host_socket(s, dst_port);
            if (fd < 0) {
                vsock_send_rst_to_guest(s, VSOCK_CID_HOST, dst_port,
                                        src_cid, src_port);
                break;
            }
            conn = vsock_conn_new(s, &key, fd);
            conn->state = VSOCK_CONN_STATE_ESTABLISHED;
            conn->peer_buf_alloc = le32_to_cpu(hdr.buf_alloc);
            conn->peer_fwd_cnt = le32_to_cpu(hdr.fwd_cnt);
            qemu_set_fd_handler(fd, vsock_unix_read, NULL, conn);
            vsock_send_response(s, conn);
            break;
        }

        case VIRTIO_VSOCK_OP_RESPONSE: {
            /* Guest accepting a host-initiated connection */
            conn = vsock_conn_find(s, &key);
            if (!conn || conn->state != VSOCK_CONN_STATE_CONNECTING) {
                vsock_send_rst_to_guest(s, VSOCK_CID_HOST, dst_port,
                                        src_cid, src_port);
                break;
            }
            conn->state = VSOCK_CONN_STATE_ESTABLISHED;
            conn->peer_buf_alloc = le32_to_cpu(hdr.buf_alloc);
            conn->peer_fwd_cnt = le32_to_cpu(hdr.fwd_cnt);

            /* Tell the host the connection is ready (Firecracker compat). */
            {
                char ok_buf[32];
                int ok_len = snprintf(ok_buf, sizeof(ok_buf),
                                      "OK %u\n", conn->key.src_port);
                write(conn->fd, ok_buf, ok_len);
            }

            qemu_set_fd_handler(conn->fd, vsock_unix_read, NULL, conn);
            break;
        }

        case VIRTIO_VSOCK_OP_RW: {
            /* Guest sending data to host */
            conn = vsock_conn_find(s, &key);
            if (!conn || conn->state != VSOCK_CONN_STATE_ESTABLISHED) {
                vsock_send_rst_to_guest(s, VSOCK_CID_HOST, dst_port,
                                        src_cid, src_port);
                break;
            }
            conn->peer_buf_alloc = le32_to_cpu(hdr.buf_alloc);
            conn->peer_fwd_cnt = le32_to_cpu(hdr.fwd_cnt);

            if (len > 0 && conn->fd >= 0) {
                uint8_t *data = g_malloc(len);
                size_t data_len = iov_to_buf(elem->out_sg, elem->out_num,
                                             sizeof(hdr), data, len);
                if (data_len > 0) {
                    /* Write to host Unix socket */
                    ssize_t written = 0;
                    while ((size_t)written < data_len) {
                        ssize_t n = write(conn->fd, data + written,
                                          data_len - written);
                        if (n <= 0) {
                            if (n < 0 && (errno == EAGAIN || errno == EINTR)) {
                                continue;
                            }
                            /* Write error: close connection */
                            vsock_send_rst_to_guest(s, VSOCK_CID_HOST, dst_port,
                                                    src_cid, src_port);
                            vsock_conn_close(s, conn);
                            conn = NULL;
                            break;
                        }
                        written += n;
                    }
                    if (conn) {
                        conn->rx_cnt += data_len;
                        conn->fwd_cnt += data_len;
                        /* Send credit update when we've consumed >= 25% of
                         * buf_alloc since the last update we sent. */
                        uint32_t delta = conn->fwd_cnt - conn->last_fwd_cnt;
                        if (delta >= conn->buf_alloc / 4) {
                            conn->last_fwd_cnt = conn->fwd_cnt;
                            vsock_send_credit_update(s, conn);
                        }
                    }
                }
                g_free(data);
            }
            break;
        }

        case VIRTIO_VSOCK_OP_SHUTDOWN: {
            conn = vsock_conn_find(s, &key);
            if (!conn) {
                break;
            }
            conn->shutdown_flags |= le32_to_cpu(hdr.flags);
            if ((conn->shutdown_flags &
                 (VIRTIO_VSOCK_SHUTDOWN_RCV | VIRTIO_VSOCK_SHUTDOWN_SEND)) ==
                (VIRTIO_VSOCK_SHUTDOWN_RCV | VIRTIO_VSOCK_SHUTDOWN_SEND)) {
                vsock_send_rst_to_guest(s, VSOCK_CID_HOST, dst_port,
                                        src_cid, src_port);
                vsock_conn_close(s, conn);
            } else if (conn->shutdown_flags & VIRTIO_VSOCK_SHUTDOWN_SEND) {
                /* Guest won't send more; shutdown our read side */
                if (conn->fd >= 0) {
                    shutdown(conn->fd, SHUT_WR);
                }
            }
            break;
        }

        case VIRTIO_VSOCK_OP_RST: {
            conn = vsock_conn_find(s, &key);
            if (conn) {
                vsock_conn_close(s, conn);
            }
            break;
        }

        case VIRTIO_VSOCK_OP_CREDIT_UPDATE: {
            conn = vsock_conn_find(s, &key);
            if (conn) {
                conn->peer_buf_alloc = le32_to_cpu(hdr.buf_alloc);
                conn->peer_fwd_cnt = le32_to_cpu(hdr.fwd_cnt);
                /* If we stopped reading due to no credit, resume */
                if (conn->state == VSOCK_CONN_STATE_ESTABLISHED &&
                    conn->fd >= 0 && vsock_conn_peer_credit(conn) > 0) {
                    qemu_set_fd_handler(conn->fd, vsock_unix_read, NULL, conn);
                }
            }
            break;
        }

        case VIRTIO_VSOCK_OP_CREDIT_REQUEST: {
            conn = vsock_conn_find(s, &key);
            if (conn) {
                vsock_send_credit_update(s, conn);
            }
            break;
        }

        default:
            break;
        }

        virtqueue_push(vq, elem, 0);
        g_free(elem);
    }

    virtio_notify(vdev, vq);
}

/* ----------------------------------------------------------------
 * Listener socket: host -> guest connections
 * ---------------------------------------------------------------- */

/* Parse "CONNECT {port}\n" header from a newly accepted connection */
static void vsock_listener_conn_read(void *opaque)
{
    VsockConn *conn = opaque;
    VirtIOVSock *s = conn->vsock;
    char *end;
    ssize_t n;

    n = read(conn->fd, conn->connect_header_buf + conn->connect_header_len,
             sizeof(conn->connect_header_buf) - 1 - conn->connect_header_len);
    if (n <= 0) {
        if (n < 0 && (errno == EAGAIN || errno == EINTR)) {
            return;
        }
        /* Connection closed before header complete */
        vsock_conn_close(s, conn);
        return;
    }

    conn->connect_header_len += n;
    conn->connect_header_buf[conn->connect_header_len] = '\0';

    end = strchr(conn->connect_header_buf, '\n');
    if (!end) {
        if (conn->connect_header_len >= (int)sizeof(conn->connect_header_buf) - 1) {
            /* Header too long */
            vsock_conn_close(s, conn);
        }
        return;
    }

    *end = '\0';

    /* Parse "CONNECT {port}" */
    uint32_t port;
    if (sscanf(conn->connect_header_buf, "CONNECT %u", &port) != 1) {
        vsock_conn_close(s, conn);
        return;
    }

    conn->connect_header_parsed = true;

    /* Update connection key: we're connecting TO the guest port.
     * Each connection gets a unique ephemeral host port so multiple
     * connections to the same guest port don't collide in the hash table.
     * Use steal (not remove) to avoid triggering the destroy func. */
    g_hash_table_steal(s->conns, &conn->key);
    uint32_t host_port = s->next_host_port++;
    conn->key.src_cid = s->guest_cid;
    conn->key.src_port = port;
    conn->key.dst_cid = VSOCK_CID_HOST;
    conn->key.dst_port = host_port;
    g_hash_table_insert(s->conns, &conn->key, conn);

    conn->state = VSOCK_CONN_STATE_CONNECTING;

    /* Replace the header-reading handler with nothing until guest responds */
    qemu_set_fd_handler(conn->fd, NULL, NULL, NULL);

    /* Send REQUEST to guest */
    vsock_send_request(s, conn);
}

static void vsock_listener_accept(void *opaque)
{
    VirtIOVSock *s = opaque;
    struct sockaddr_un addr;
    socklen_t addrlen = sizeof(addr);
    int fd;

    fd = accept(s->listener_fd, (struct sockaddr *)&addr, &addrlen);
    if (fd < 0) {
        if (errno != EAGAIN && errno != EINTR) {
            error_report("virtio-vsock: accept failed: %s", strerror(errno));
        }
        return;
    }
    /* Set non-blocking */
    int flags = fcntl(fd, F_GETFL);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);

    /* Create a temporary connection with a placeholder key.
     * The real key will be set once we parse the CONNECT header. */
    static uint32_t next_ephemeral_port = 0x80000000;
    VsockConnKey key = {
        .src_cid = 0,
        .src_port = next_ephemeral_port++,
        .dst_cid = 0,
        .dst_port = 0,
    };

    VsockConn *conn = vsock_conn_new(s, &key, fd);
    conn->state = VSOCK_CONN_STATE_IDLE;

    /* Read the CONNECT header */
    qemu_set_fd_handler(fd, vsock_listener_conn_read, NULL, conn);
}

static int vsock_listener_setup(VirtIOVSock *s)
{
    struct sockaddr_un addr;
    int fd;

    if (s->listener_fd >= 0) {
        return 0; /* already listening */
    }

    if (!s->socket_path) {
        return -1;
    }

    fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        error_report("virtio-vsock: socket() failed: %s", strerror(errno));
        return -1;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    if (strlen(s->socket_path) >= sizeof(addr.sun_path)) {
        error_report("virtio-vsock: socket path too long");
        close(fd);
        return -1;
    }
    strncpy(addr.sun_path, s->socket_path, sizeof(addr.sun_path) - 1);

    unlink(s->socket_path);
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        error_report("virtio-vsock: bind(%s) failed: %s",
                     s->socket_path, strerror(errno));
        close(fd);
        return -1;
    }

    if (listen(fd, 16) < 0) {
        error_report("virtio-vsock: listen failed: %s", strerror(errno));
        close(fd);
        unlink(s->socket_path);
        return -1;
    }

    int flags = fcntl(fd, F_GETFL);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);

    s->listener_fd = fd;
    qemu_set_fd_handler(fd, vsock_listener_accept, NULL, s);
    return 0;
}

static void vsock_listener_teardown(VirtIOVSock *s)
{
    if (s->listener_fd >= 0) {
        qemu_set_fd_handler(s->listener_fd, NULL, NULL, NULL);
        close(s->listener_fd);
        s->listener_fd = -1;
        if (s->socket_path) {
            unlink(s->socket_path);
        }
    }
}

/* ----------------------------------------------------------------
 * RX virtqueue handler: guest posts buffers for host->guest data
 * ---------------------------------------------------------------- */

static void virtio_vsock_handle_rx(VirtIODevice *vdev, VirtQueue *vq)
{
    VirtIOVSock *s = VIRTIO_VSOCK(vdev);

    /* Guest posted new rx buffers; try to deliver pending packets */
    if (!QTAILQ_EMPTY(&s->rx_queue)) {
        qemu_bh_schedule(s->rx_bh);
    }
}

static void virtio_vsock_handle_event(VirtIODevice *vdev, VirtQueue *vq)
{
    /* Event queue: used for transport reset. We handle this in reset(). */
}

/* ----------------------------------------------------------------
 * VirtIO device lifecycle
 * ---------------------------------------------------------------- */

static void vsock_send_transport_reset(VirtIOVSock *s)
{
    VirtIODevice *vdev = VIRTIO_DEVICE(s);
    VirtQueueElement *elem;
    struct virtio_vsock_event event = {
        .id = cpu_to_le32(VIRTIO_VSOCK_EVENT_TRANSPORT_RESET),
    };

    if (!virtio_queue_ready(s->event_vq)) {
        return;
    }

    elem = virtqueue_pop(s->event_vq, sizeof(VirtQueueElement));
    if (!elem) {
        return;
    }

    if (iov_from_buf(elem->in_sg, elem->in_num, 0,
                     &event, sizeof(event)) != sizeof(event)) {
        virtqueue_detach_element(s->event_vq, elem, 0);
        g_free(elem);
        return;
    }

    virtqueue_push(s->event_vq, elem, sizeof(event));
    virtio_notify(vdev, s->event_vq);
    g_free(elem);
}

static uint64_t virtio_vsock_get_features(VirtIODevice *vdev,
                                          uint64_t features,
                                          Error **errp)
{
    virtio_add_feature(&features, VIRTIO_F_VERSION_1);
    return features;
}

static void virtio_vsock_get_config(VirtIODevice *vdev, uint8_t *config)
{
    VirtIOVSock *s = VIRTIO_VSOCK(vdev);
    struct virtio_vsock_config cfg = {
        .guest_cid = cpu_to_le64(s->guest_cid),
    };
    memcpy(config, &cfg, sizeof(cfg));
}

static int virtio_vsock_set_status(VirtIODevice *vdev, uint8_t status)
{
    VirtIOVSock *s = VIRTIO_VSOCK(vdev);
    bool should_start = virtio_device_should_start(vdev, status);

    if (s->started == should_start) {
        /* After migration restore, listener_fd is -1 but started is true.
         * Re-establish the listener in that case. */
        if (should_start && s->listener_fd < 0) {
            vsock_listener_setup(s);
        }
        return 0;
    }

    if (should_start) {
        s->started = true;
        vsock_listener_setup(s);
    } else {
        vsock_conn_close_all(s);
        vsock_listener_teardown(s);
        s->started = false;
    }
    return 0;
}

static void virtio_vsock_reset(VirtIODevice *vdev)
{
    VirtIOVSock *s = VIRTIO_VSOCK(vdev);

    vsock_conn_close_all(s);
    vsock_listener_teardown(s);
    s->started = false;

    /* Drain pending rx packets */
    while (!QTAILQ_EMPTY(&s->rx_queue)) {
        VsockPkt *pkt = QTAILQ_FIRST(&s->rx_queue);
        QTAILQ_REMOVE(&s->rx_queue, pkt, entry);
        g_free(pkt->payload);
        g_free(pkt);
    }

    vsock_send_transport_reset(s);
}

static void virtio_vsock_device_realize(DeviceState *dev, Error **errp)
{
    VirtIODevice *vdev = VIRTIO_DEVICE(dev);
    VirtIOVSock *s = VIRTIO_VSOCK(dev);

    if (s->guest_cid <= 2) {
        error_setg(errp, "guest-cid must be > 2 (got %" PRIu64 ")",
                   s->guest_cid);
        return;
    }

    if (!s->socket_path || strlen(s->socket_path) == 0) {
        error_setg(errp, "socket-path must be specified");
        return;
    }

    virtio_init(vdev, VIRTIO_ID_VSOCK, sizeof(struct virtio_vsock_config));

    s->rx_vq = virtio_add_queue(vdev, VIRTIO_VSOCK_QUEUE_SIZE,
                                virtio_vsock_handle_rx);
    s->tx_vq = virtio_add_queue(vdev, VIRTIO_VSOCK_QUEUE_SIZE,
                                virtio_vsock_handle_tx);
    s->event_vq = virtio_add_queue(vdev, VIRTIO_VSOCK_QUEUE_SIZE,
                                   virtio_vsock_handle_event);

    s->conns = g_hash_table_new_full(vsock_conn_key_hash,
                                     vsock_conn_key_equal,
                                     NULL, vsock_conn_free);
    QTAILQ_INIT(&s->rx_queue);
    s->listener_fd = -1;
    s->next_host_port = 0x80000000;
    s->rx_bh = qemu_bh_new(vsock_rx_pump, s);

    if (s->buf_alloc == 0) {
        s->buf_alloc = VIRTIO_VSOCK_DEFAULT_BUF_ALLOC;
    }
}

static void virtio_vsock_device_unrealize(DeviceState *dev)
{
    VirtIODevice *vdev = VIRTIO_DEVICE(dev);
    VirtIOVSock *s = VIRTIO_VSOCK(dev);

    vsock_conn_close_all(s);
    vsock_listener_teardown(s);

    /* Drain pending rx packets */
    while (!QTAILQ_EMPTY(&s->rx_queue)) {
        VsockPkt *pkt = QTAILQ_FIRST(&s->rx_queue);
        QTAILQ_REMOVE(&s->rx_queue, pkt, entry);
        g_free(pkt->payload);
        g_free(pkt);
    }

    qemu_bh_delete(s->rx_bh);
    g_hash_table_destroy(s->conns);

    virtio_delete_queue(s->rx_vq);
    virtio_delete_queue(s->tx_vq);
    virtio_delete_queue(s->event_vq);
    virtio_cleanup(vdev);
}

/* ----------------------------------------------------------------
 * QOM registration
 * ---------------------------------------------------------------- */

static int virtio_vsock_post_load(void *opaque, int version_id)
{
    VirtIOVSock *s = VIRTIO_VSOCK(opaque);

    /* After migration, listener_fd is -1 (not migratable).
     * Re-establish the host-side listener. */
    if (s->listener_fd < 0 && s->socket_path) {
        vsock_listener_setup(s);
    }

    /* Reset the guest's vsock transport so it tears down stale connections
     * and reinitializes.  Without this, userspace Accept() calls won't
     * fire for new host-initiated connections. */
    vsock_send_transport_reset(s);

    return 0;
}

static const VMStateDescription vmstate_virtio_vsock = {
    .name = "virtio-vsock",
    .version_id = 1,
    .minimum_version_id = 1,
    .post_load = virtio_vsock_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_VIRTIO_DEVICE,
        VMSTATE_END_OF_LIST()
    },
};

static const Property virtio_vsock_properties[] = {
    DEFINE_PROP_UINT64("guest-cid", VirtIOVSock, guest_cid, 3),
    DEFINE_PROP_STRING("socket-path", VirtIOVSock, socket_path),
    DEFINE_PROP_UINT32("buf-alloc", VirtIOVSock, buf_alloc,
                       VIRTIO_VSOCK_DEFAULT_BUF_ALLOC),
};

static void virtio_vsock_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    VirtioDeviceClass *vdc = VIRTIO_DEVICE_CLASS(klass);

    device_class_set_props(dc, virtio_vsock_properties);
    dc->vmsd = &vmstate_virtio_vsock;
    set_bit(DEVICE_CATEGORY_MISC, dc->categories);

    vdc->realize = virtio_vsock_device_realize;
    vdc->unrealize = virtio_vsock_device_unrealize;
    vdc->get_features = virtio_vsock_get_features;
    vdc->get_config = virtio_vsock_get_config;
    vdc->set_status = virtio_vsock_set_status;
    vdc->reset = virtio_vsock_reset;
}

static const TypeInfo virtio_vsock_info = {
    .name = TYPE_VIRTIO_VSOCK,
    .parent = TYPE_VIRTIO_DEVICE,
    .instance_size = sizeof(VirtIOVSock),
    .class_init = virtio_vsock_class_init,
};

static void virtio_vsock_register_types(void)
{
    type_register_static(&virtio_vsock_info);
}

type_init(virtio_vsock_register_types)
