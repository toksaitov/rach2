/*
 * virtio_net.c: the driver of up to VNET_MAX virtio network devices, QEMU's
 * `virtio-net-pci`, as Linux's drivers/net/virtio_net.c drives them.
 *
 * A frame is a buffer on a virtqueue: 12 bytes of header and the frame.
 * The driver keeps the receive queue full of empty buffers; the device
 * fills one, returns it and interrupts, and the driver gives the frame to
 * `ethernet_rx` and the buffer back to the queue. To send, it copies the
 * frame into a buffer of its transmit pool, adds it to the transmit queue
 * and tells the device, which returns the buffer once the frame has gone.
 * Nothing here is a register: under a hypervisor a frame costs at most one
 * exit, the notification, and with VIRTIO_F_EVENT_IDX the device asks for
 * that only when it has caught up, where each port write of the RTL8139
 * is an exit.
 *
 * Interrupts are kept down as Linux's driver keeps them. The receive
 * queue's reap points the device's next interrupt past all it takes, and
 * gives the buffers back in one notification (the `batch` of virtio.h),
 * which is the batching NAPI gives Linux. The transmit queue asks for no
 * interrupt at all (`virtqueue_irq`): the buffers the device has returned
 * are taken at the next send, as Linux's free_old_xmit takes them in
 * start_xmit. The control queue is polled, as virtnet_send_command polls.
 *
 * The transport, virtio-pci, and the queues are virtio/'s, reached through
 * virtio.h alone, so the host test runs the driver against a model device.
 * Every path runs under the big kernel lock: a system call's send, the
 * probe, and the device's interrupt, whose handlers virtio-pci registers
 * under it for a driver that does not set `irq_unlocked`.
 */

#include "virtio_net.h"

#include "../../klog.h"
#include "../../lib/kerrno.h"
#include "../../lib/krandom.h"
#include "../../lib/kstring.h"
#include "../../udelay.h"
#include "../../virtio/virtio.h"
#include "../ethernet/ethernet.h"
#include "../net.h"

/* A buffer: half a page of the transport's memory. `next` links the free
 * transmit buffers. */
struct vnet_buf {
    uint8_t *data;
    uint64_t addr; /* as the device reads it */
    struct vnet_buf *next;
};

/* A device. `vdev` is NULL once the device was unbound, and `nd` once the
 * probe took the interface back (`vnet_stop`). */
struct vnet {
    struct virtio_dev *vdev;
    struct netdev *nd;
    struct virtqueue *rx, *tx, *ctrl;
    int up;           /* DRIVER_OK is set and the queues carry frames */
    int key;          /* the PCI function, as `nd->pci_key`, or -1 */
    int mrg;          /* VIRTIO_NET_F_MRG_RXBUF was taken */
    int polled;       /* no interrupt: the tick reaps the queues */
    uint8_t mac[6];   /* the station address */
    int link_up;      /* the link, as the configuration last said */
    uint32_t speed;   /* Mb/s, or 0 when the device does not say */
    int duplex;       /* 1 full, 0 half, -1 when the device does not say */
    uint32_t mtu_max; /* the device's largest MTU, or 0 for none given */
    void *page[(VNET_RX_BUFS + VNET_TX_BUFS) / 2];
    int npages;
    struct vnet_buf rxb[VNET_RX_BUFS], txb[VNET_TX_BUFS];
    struct vnet_buf *tx_free;
    int tx_out; /* transmit buffers the device has */
    /* A frame that spans receive buffers is put together here: the buffers
     * still to come, the bytes so far, and whether it outgrew the frame. */
    uint8_t frame[VNET_FRAME_MAX];
    uint16_t rx_left;
    uint32_t rx_len;
    int rx_over;
    /* The control queue's one command at a time, in a page of its own. */
    uint8_t *cmd;
    uint64_t cmd_addr;
    int cmd_done;
    uint32_t rx_packets, rx_errors, tx_packets, tx_stalls;
};

static struct vnet vnets[VNET_MAX];
static const char *vnet_err = "no virtio network device";
static int vnet_registered;

static int vnet_init(struct netdev *nd, int nth);

/* Where a command's parts lie in its page: the class and command, the data
 * and the device's answer, each in a descriptor of its own. */
#define VNET_CMD_HDR  0u
#define VNET_CMD_DATA 16u
#define VNET_CMD_ACK  64u

/** Give `b` to the receive queue, empty. */
static int vnet_rx_give(struct vnet *v, struct vnet_buf *b)
{
    struct virtio_sg sg;

    sg.addr = b->addr;
    sg.len = VNET_BUF;
    return virtqueue_add(v->rx, &sg, 0, 1, b);
}

/** Give the frame of `len` bytes at `data` to the stack, or count one that
 * is shorter than an Ethernet header or longer than a frame, as Linux's
 * receive_buf counts rx_length_errors. */
static void vnet_deliver(struct vnet *v, const uint8_t *data, uint32_t len)
{
    if (len < ETH_HDR_SIZE || len > VNET_FRAME_MAX) {
        v->rx_errors++;
        ethernet_note_rx_error(v->nd);
        return;
    }
    v->rx_packets++;
    ethernet_rx(v->nd, data, (uint16_t)len);
}

/**
 * The first buffer of a frame, `len` bytes the device wrote: the header,
 * then the frame or its start. A frame in one buffer is delivered from the
 * buffer; one that spans more, as `num_buffers` says with
 * VIRTIO_NET_F_MRG_RXBUF, begins the frame put together in `v->frame`.
 */
static void vnet_rx_first(struct vnet *v, const uint8_t *data, uint32_t len)
{
    struct virtio_net_hdr hdr;
    uint16_t bufs;

    if (len < VNET_HDR) {
        v->rx_errors++;
        ethernet_note_rx_error(v->nd);
        return;
    }
    k_memcpy(&hdr, data, VNET_HDR);
    bufs = v->mrg ? hdr.num_buffers : 1;
    if (bufs <= 1) {
        vnet_deliver(v, data + VNET_HDR, len - VNET_HDR);
        return;
    }
    v->rx_left = (uint16_t)(bufs - 1u);
    v->rx_len = 0;
    v->rx_over = len - VNET_HDR > VNET_FRAME_MAX;
    if (!v->rx_over) {
        v->rx_len = len - VNET_HDR;
        k_memcpy(v->frame, data + VNET_HDR, v->rx_len);
    }
}

/** A later buffer of a frame that spans buffers, which has no header: add
 * its `len` bytes, and deliver the frame at its last buffer, or count it
 * when it outgrew a frame. */
static void vnet_rx_more(struct vnet *v, const uint8_t *data, uint32_t len)
{
    if (!v->rx_over && len <= VNET_FRAME_MAX - v->rx_len) {
        k_memcpy(v->frame + v->rx_len, data, len);
        v->rx_len += len;
    } else {
        v->rx_over = 1;
    }
    if (--v->rx_left != 0) {
        return;
    }
    if (v->rx_over) {
        v->rx_errors++;
        ethernet_note_rx_error(v->nd);
    } else {
        vnet_deliver(v, v->frame, v->rx_len);
    }
}

/**
 * The receive queue's `done`: the device has filled the buffer `token` with
 * `len` bytes. The frame goes to the stack and the buffer back to the
 * queue; `vnet_rx_batch` tells the device once the reap is over. Called
 * from the interrupt handler, or from the tick on a polled device, with
 * interrupts off, as `ethernet_rx` asks.
 */
static void vnet_rx_done(struct virtqueue *vq, void *token, uint32_t len)
{
    struct vnet *v = (struct vnet *)vq->priv;
    struct vnet_buf *b = (struct vnet_buf *)token;

    if (v->up && v->nd) {
        if (v->rx_left == 0) {
            vnet_rx_first(v, b->data, len);
        } else {
            vnet_rx_more(v, b->data, len);
        }
    }
    (void)vnet_rx_give(v, b);
}

/** The receive queue's `batch`: tell the device of the buffers a reap gave
 * back, once, as Linux's try_fill_recv kicks after it has filled. */
static void vnet_rx_batch(struct virtqueue *vq)
{
    virtqueue_kick(vq);
}

/** The transmit queue's `done`: the frame in `token` has gone, and its
 * buffer is free again. */
static void vnet_tx_done(struct virtqueue *vq, void *token, uint32_t len)
{
    struct vnet *v = (struct vnet *)vq->priv;
    struct vnet_buf *b = (struct vnet_buf *)token;

    (void)len;
    b->next = v->tx_free;
    v->tx_free = b;
    v->tx_out--;
}

/** The control queue's `done`: the device has answered the command. */
static void vnet_ctrl_done(struct virtqueue *vq, void *token, uint32_t len)
{
    struct vnet *v = (struct vnet *)vq->priv;

    (void)token;
    (void)len;
    v->cmd_done = 1;
}

/**
 * Send the control command `cmd` of `class` with `len` bytes of `data`, at
 * most 48, and wait for the device's answer, polling the queue as Linux's
 * virtnet_send_command does, for at most 100 ms where Linux waits without
 * bound. Process context.
 *
 * @return 0 when the device answered VIRTIO_NET_OK; -EIO when it refused
 *         the command; -ETIMEDOUT when it did not answer, after which the
 *         control queue is left alone, since the device may still write
 *         the answer; -ENODEV without a control queue.
 */
static int vnet_command(struct vnet *v, uint8_t class, uint8_t cmd, const void *data, uint32_t len)
{
    struct virtio_sg sg[3];
    int i;

    if (!v->ctrl || !v->cmd) {
        return -ENODEV;
    }
    v->cmd[VNET_CMD_HDR] = class;
    v->cmd[VNET_CMD_HDR + 1u] = cmd;
    k_memcpy(v->cmd + VNET_CMD_DATA, data, len);
    v->cmd[VNET_CMD_ACK] = 0xFF;
    sg[0].addr = v->cmd_addr + VNET_CMD_HDR;
    sg[0].len = 2;
    sg[1].addr = v->cmd_addr + VNET_CMD_DATA;
    sg[1].len = len;
    sg[2].addr = v->cmd_addr + VNET_CMD_ACK;
    sg[2].len = 1;
    v->cmd_done = 0;
    if (virtqueue_add(v->ctrl, sg, 2, 1, v->cmd) != 0) {
        return -EIO;
    }
    virtqueue_kick(v->ctrl);
    for (i = 0; i < VNET_CTRL_STEPS && !v->cmd_done; i++) {
        (void)virtqueue_reap(v->ctrl);
        if (!v->cmd_done) {
            k_udelay(100);
        }
    }
    if (!v->cmd_done) {
        klog(KLOG_WARN, "virtio_net", "control: %s: command %d of class %d was not answered",
             v->vdev->name, (int)cmd, (int)class);
        v->ctrl = 0;
        return -ETIMEDOUT;
    }
    return v->cmd[VNET_CMD_ACK] == VIRTIO_NET_OK ? 0 : -EIO;
}

/**
 * Set the device's receive filter as the e1000's is set: frames for the
 * station address and the broadcast address, and every multicast frame,
 * since IPv6 finds its neighbours by groups the stack gives no driver a
 * list of. A device resets with every frame accepted, which stands when it
 * has no receive mode commands or when it does not know the address: one
 * the driver made up for a device without VIRTIO_NET_F_MAC is given to it
 * first (VIRTIO_NET_CTRL_MAC_ADDR_SET), as Linux's virtnet_probe gives it.
 */
static void vnet_rx_mode(struct vnet *v)
{
    uint8_t on = 1, off = 0;
    int known = virtio_has(v->vdev, VIRTIO_NET_F_MAC);

    if (!known && virtio_has(v->vdev, VIRTIO_NET_F_CTRL_MAC_ADDR)) {
        known = vnet_command(v, VIRTIO_NET_CTRL_MAC, VIRTIO_NET_CTRL_MAC_ADDR_SET, v->mac, 6) == 0;
    }
    if (!known || !virtio_has(v->vdev, VIRTIO_NET_F_CTRL_RX)) {
        return;
    }
    if (vnet_command(v, VIRTIO_NET_CTRL_RX, VIRTIO_NET_CTRL_RX_ALLMULTI, &on, 1) == 0) {
        (void)vnet_command(v, VIRTIO_NET_CTRL_RX, VIRTIO_NET_CTRL_RX_PROMISC, &off, 1);
    }
}

/**
 * Read the link from the configuration, as Linux's
 * virtnet_config_changed_work and virtnet_update_settings do: up or down
 * from the status word, and up always without VIRTIO_NET_F_STATUS; the
 * speed and duplex with VIRTIO_NET_F_SPEED_DUPLEX, each kept only when
 * valid. Called at bring-up and from the configuration's interrupt, which
 * virtio-pci allows (`config_atomic`). Returns whether the link changed.
 */
static int vnet_link_read(struct vnet *v)
{
    uint16_t status = VIRTIO_NET_S_LINK_UP;
    uint32_t speed = 0xFFFFFFFFu;
    uint8_t duplex = 0xFF;
    int was = v->link_up;

    if (virtio_has(v->vdev, VIRTIO_NET_F_STATUS)) {
        virtio_config_read(v->vdev, VNET_CFG_STATUS, &status, sizeof(status));
    }
    if (virtio_has(v->vdev, VIRTIO_NET_F_SPEED_DUPLEX)) {
        virtio_config_read(v->vdev, VNET_CFG_SPEED, &speed, sizeof(speed));
        virtio_config_read(v->vdev, VNET_CFG_DUPLEX, &duplex, sizeof(duplex));
    }
    v->link_up = (status & VIRTIO_NET_S_LINK_UP) != 0;
    v->speed = speed <= VNET_SPEED_MAX ? speed : 0;
    v->duplex = duplex <= 1 ? (int)duplex : -1;
    return was != v->link_up;
}

/** The device's `config_changed`: the link may have changed. The interface
 * is told to ask again at its next look. Handler context. */
static void vnet_config_changed(struct virtio_dev *d)
{
    struct vnet *v = (struct vnet *)d->priv;

    if (v && v->up && d->config_atomic && vnet_link_read(v)) {
        netdev_carrier_changed(v->nd);
        klog(KLOG_INFO, "virtio_net", "link: %s: %s", d->name, v->link_up ? "up" : "down");
    }
}

/** Give back the buffers' pages and the command page, once the device can
 * no longer write them: after `virtio_teardown`. */
static void vnet_free(struct vnet *v)
{
    int i;

    for (i = 0; i < v->npages; i++) {
        virtio_dma_free(v->vdev, v->page[i], 2u * VNET_BUF);
    }
    v->npages = 0;
    virtio_dma_free(v->vdev, v->cmd, 2u * VNET_BUF);
    v->cmd = 0;
    v->tx_free = 0;
    v->tx_out = 0;
}

/** Take a page for the two buffers `a` and `b`; `b` is NULL for an odd
 * buffer's page. 0, or -ENOMEM. */
static int vnet_page(struct vnet *v, struct vnet_buf *a, struct vnet_buf *b)
{
    uint64_t addr;
    uint8_t *p = (uint8_t *)virtio_dma_alloc(v->vdev, 2u * VNET_BUF, &addr);

    if (!p) {
        return -ENOMEM;
    }
    v->page[v->npages++] = p;
    a->data = p;
    a->addr = addr;
    if (b) {
        b->data = p + VNET_BUF;
        b->addr = addr + VNET_BUF;
    }
    return 0;
}

/**
 * Take the buffers and fill the receive queue: a buffer for each of its
 * entries, at most VNET_RX_BUFS, and a transmit pool of at most
 * VNET_TX_BUFS, no more than the transmit queue holds, so that a pool
 * buffer always has a descriptor. 0, or -ENOMEM with what was taken left
 * for `vnet_free`.
 */
static int vnet_buffers(struct vnet *v)
{
    int nrx = v->rx->size < VNET_RX_BUFS ? v->rx->size : VNET_RX_BUFS;
    int ntx = v->tx->size < VNET_TX_BUFS ? v->tx->size : VNET_TX_BUFS;
    int i;

    for (i = 0; i < nrx; i += 2) {
        if (vnet_page(v, &v->rxb[i], i + 1 < nrx ? &v->rxb[i + 1] : 0) != 0) {
            return -ENOMEM;
        }
    }
    for (i = 0; i < ntx; i += 2) {
        if (vnet_page(v, &v->txb[i], i + 1 < ntx ? &v->txb[i + 1] : 0) != 0) {
            return -ENOMEM;
        }
    }
    for (i = 0; i < ntx; i++) {
        v->txb[i].next = v->tx_free;
        v->tx_free = &v->txb[i];
    }
    for (i = 0; i < nrx; i++) {
        if (vnet_rx_give(v, &v->rxb[i]) != 0) {
            return -ENOMEM;
        }
    }
    return 0;
}

/**
 * Negotiate the features. A device whose largest MTU is below the least an
 * interface may have is taken without VIRTIO_NET_F_MTU, as Linux's
 * virtnet_validate clears the bit before the features are settled: the
 * device is reset and the negotiation run again without it. 0 with
 * `v->mtu_max` set, or `virtio_init`'s error.
 */
static int vnet_negotiate(struct vnet *v)
{
    uint16_t mtu = 0;
    int rc = virtio_init(v->vdev, VNET_FEATURES, 3);

    v->mtu_max = 0;
    if (rc != 0 || !virtio_has(v->vdev, VIRTIO_NET_F_MTU)) {
        return rc;
    }
    virtio_config_read(v->vdev, VNET_CFG_MTU, &mtu, sizeof(mtu));
    if (mtu >= NETDEV_ETH_MTU_MIN) {
        v->mtu_max = mtu;
        return 0;
    }
    klog(KLOG_WARN, "virtio_net", "mtu: %s: the device's %d is too small, ignored", v->vdev->name,
         (int)mtu);
    virtio_teardown(v->vdev);
    return virtio_init(v->vdev, VNET_FEATURES & ~VIRTIO_BIT(VIRTIO_NET_F_MTU), 3);
}

/** The station address: the configuration's with VIRTIO_NET_F_MAC, else a
 * random one, locally administered, as Linux's eth_hw_addr_random makes. */
static void vnet_mac_read(struct vnet *v)
{
    if (virtio_has(v->vdev, VIRTIO_NET_F_MAC)) {
        virtio_config_read(v->vdev, VNET_CFG_MAC, v->mac, 6);
        return;
    }
    krandom_bytes(v->mac, 6);
    v->mac[0] = (uint8_t)((v->mac[0] & 0xFEu) | 0x02u);
}

/**
 * Set up the queues: receive and transmit, and the control queue when the
 * device has one, with a page for its commands. The transmit and control
 * queues ask for no interrupt, and on a device without interrupts the
 * receive queue needs none either. 0 or -errno.
 */
static int vnet_queues(struct vnet *v)
{
    int rc = virtio_queue_setup(v->vdev, VNET_Q_RX, vnet_rx_done, v, &v->rx);

    if (rc == 0) {
        rc = virtio_queue_setup(v->vdev, VNET_Q_TX, vnet_tx_done, v, &v->tx);
    }
    if (rc != 0) {
        return rc;
    }
    v->rx->batch = vnet_rx_batch;
    (void)virtqueue_irq(v->tx, 0);
    if (!virtio_has(v->vdev, VIRTIO_NET_F_CTRL_VQ)) {
        return 0;
    }
    v->cmd = (uint8_t *)virtio_dma_alloc(v->vdev, 2u * VNET_BUF, &v->cmd_addr);
    if (v->cmd && virtio_queue_setup(v->vdev, VNET_Q_CTRL, vnet_ctrl_done, v, &v->ctrl) == 0) {
        (void)virtqueue_irq(v->ctrl, 0);
    } else {
        v->ctrl = 0;
    }
    return 0;
}

/**
 * Bring `v->vdev` up, Linux's virtnet_probe and virtnet_open: the device
 * shown bound to virtio_net, the features negotiated, the address read,
 * the queues set up and the receive queue filled, DRIVER_OK, the device
 * told of its buffers, the receive filter set and the link read. On an
 * error the device is reset, its memory given back and the binding taken
 * away; the caller gives the device back. 0 or -errno, with the reason in
 * `vnet_err`.
 */
static int vnet_start(struct vnet *v)
{
    struct virtio_dev *d = v->vdev;
    int rc;

    d->priv = v;
    d->irq_unlocked = 0;
    d->config_changed = 0;
    v->rx = v->tx = v->ctrl = 0;
    v->rx_left = 0;
    vnet_err = "the virtio network device could not be shown bound";
    rc = virtio_driver_bind(d, "virtio_net");
    if (rc != 0) {
        return rc;
    }
    vnet_err = "the virtio network device refused the features";
    rc = vnet_negotiate(v);
    if (rc == 0) {
        v->mrg = virtio_has(d, VIRTIO_NET_F_MRG_RXBUF);
        v->polled = d->polled;
        vnet_mac_read(v);
        vnet_err = "the virtio network device's queues could not be set up";
        rc = vnet_queues(v);
    }
    if (rc == 0) {
        vnet_err = "no memory for the virtio network device's buffers";
        rc = vnet_buffers(v);
    }
    if (rc == 0) {
        vnet_err = "the virtio network device did not take DRIVER_OK";
        rc = virtio_ready(d);
    }
    if (rc != 0) {
        virtio_teardown(d);
        vnet_free(v);
        virtio_driver_unbind(d);
        return rc;
    }
    v->up = 1;
    virtqueue_kick(v->rx);
    vnet_rx_mode(v);
    d->config_changed = vnet_config_changed;
    (void)vnet_link_read(v);
    virtio_driver_bound(d);
    return 0;
}

/** Stop the device: no frame is taken or sent from here on, the device is
 * reset, which ends its use of memory, and its buffers are given back. */
static void vnet_down(struct vnet *v)
{
    uint32_t flags = net_lock();

    v->up = 0;
    v->link_up = 0;
    net_unlock(flags);
    v->vdev->config_changed = 0;
    virtio_teardown(v->vdev);
    v->rx = v->tx = v->ctrl = 0;
    vnet_free(v);
}

/**
 * The device's `removed`: its function is being unbound from virtio-pci,
 * Linux's virtnet_remove. The device is stopped and given back, and its
 * interface, which the probe's table keeps for good, is left without a
 * carrier until the function is bound again (`virtio_net_bind`). Process
 * context, under the big lock.
 */
static void vnet_removed(struct virtio_dev *d)
{
    struct vnet *v = (struct vnet *)d->priv;

    if (!v || v->vdev != d) {
        return;
    }
    if (v->up) {
        vnet_down(v);
        if (v->nd) {
            netdev_carrier_changed(v->nd);
        }
    }
    klog(KLOG_INFO, "virtio_net", "removed: %s", d->name);
    d->removed = 0;
    d->priv = 0;
    virtio_driver_unbind(d);
    v->vdev = 0;
    virtio_put(d);
}

/** Log the device's detail lines: its virtio device, queues and what was
 * negotiated, then its link. */
static void vnet_print_info(void *dev)
{
    struct vnet *v = (struct vnet *)dev;

    if (!v || !v->up) {
        return;
    }
    klog(KLOG_INFO, "virtio_net", "%s: rx %d, tx %d buffers%s%s%s%s", v->vdev->name,
         (int)v->rx->size, (int)v->tx->size, v->mrg ? ", mergeable" : "",
         virtio_has(v->vdev, VIRTIO_F_EVENT_IDX) ? ", event index" : "",
         v->ctrl ? ", control queue" : "", v->polled ? ", polled" : "");
    if (!v->link_up) {
        klog(KLOG_INFO, "virtio_net", "no link yet");
    } else if (v->speed) {
        klog(KLOG_INFO, "virtio_net", "link %u Mb/s, %s duplex", (unsigned)v->speed,
             v->duplex < 0 ? "unknown"
             : v->duplex   ? "full"
                           : "half");
    } else {
        klog(KLOG_INFO, "virtio_net", "link up");
    }
    if (v->rx_packets || v->rx_errors || v->tx_packets || v->tx_stalls) {
        klog(KLOG_INFO, "virtio_net", "rx %u (%u error), tx %u (%u stalled)",
             (unsigned)v->rx_packets, (unsigned)v->rx_errors, (unsigned)v->tx_packets,
             (unsigned)v->tx_stalls);
    }
}

/** The driver table's `error`: the reason the last bring-up that failed
 * gave, a string constant. */
static const char *vnet_error(void)
{
    return vnet_err;
}

/** Take back the transmit buffers the device has returned. */
static void vnet_tx_reclaim(struct vnet *v)
{
    (void)virtqueue_reap(v->tx);
}

/**
 * The driver table's `tx`: copy the frame after a header of zeros into a
 * buffer of the pool, add it to the transmit queue and tell the device.
 * The buffers the device has returned are taken first. With the whole pool
 * at the device the send waits VNET_TX_WAIT_STEPS intervals for one, then
 * drops the frame and counts a stall, where Linux stops the queue; the
 * stack sends a dropped segment again. The frame is not padded to 60
 * bytes, as Linux's driver does not pad: no wire is behind the device.
 * Runs under `net_lock`, which nests, since a sender in a handler shares
 * the pool.
 */
static int vnet_tx(void *dev, const uint8_t *data, uint16_t len)
{
    struct vnet *v = (struct vnet *)dev;
    struct vnet_buf *b;
    struct virtio_sg sg;
    uint32_t flags;
    int i;

    if (!v || !v->up || len == 0 || len > VNET_FRAME_MAX) {
        if (len > VNET_FRAME_MAX) {
            klog(KLOG_WARN, "virtio_net", "tx: frame of %d bytes is too large, dropped", (int)len);
        }
        return NETDEV_TX_DROPPED;
    }
    flags = net_lock();
    vnet_tx_reclaim(v);
    for (i = 0; !v->tx_free && i < VNET_TX_WAIT_STEPS; i++) {
        k_udelay(VNET_TX_WAIT_USEC);
        vnet_tx_reclaim(v);
    }
    b = v->tx_free;
    if (!b) {
        v->tx_stalls++;
        net_unlock(flags);
        return NETDEV_TX_DROPPED;
    }
    k_memset(b->data, 0, VNET_HDR);
    k_memcpy(b->data + VNET_HDR, data, len);
    sg.addr = b->addr;
    sg.len = VNET_HDR + len;
    if (virtqueue_add(v->tx, &sg, 1, 0, b) != 0) {
        v->tx_stalls++;
        net_unlock(flags);
        return NETDEV_TX_DROPPED;
    }
    v->tx_free = b->next;
    v->tx_out++;
    v->tx_packets++;
    virtqueue_kick(v->tx);
    net_unlock(flags);
    return NETDEV_TX_OK;
}

/** The polled table's `poll`: reap both queues, for a device that cannot
 * interrupt. The tick calls it, with interrupts off. */
static void vnet_poll(void *dev)
{
    struct vnet *v = (struct vnet *)dev;

    if (v && v->up) {
        (void)virtqueue_reap(v->rx);
        vnet_tx_reclaim(v);
    }
}

/**
 * The driver table's `stop`: wait up to 10 ms for the frames given to the
 * device, so that the last log line before a shutdown leaves, then reset
 * the device and give its memory back. The reset ends the device's use of
 * memory as clearing bus mastering ends a card's, and the transport clears
 * bus mastering itself when the reset does not finish. The interface is
 * let go, since the probe gives a refused card's record to the next card.
 * Safe on a device that never started, and twice.
 */
static void vnet_stop(void *dev)
{
    struct vnet *v = (struct vnet *)dev;
    int i;

    if (!v) {
        return;
    }
    if (v->up) {
        for (i = 0; i < VNET_DRAIN_STEPS && v->tx_out > 0; i++) {
            vnet_tx_reclaim(v);
            if (v->tx_out > 0) {
                k_udelay(VNET_TX_WAIT_USEC);
            }
        }
        vnet_down(v);
    }
    v->nd = 0;
}

/**
 * The driver table's `link`: the link as the configuration last said,
 * which the configuration's interrupt keeps current, so that no look reads
 * the device, as Linux's netif_carrier_ok reads the state its driver
 * keeps. A speed of 0 and a duplex of -1 mean the device does not say.
 * Down for a device that was stopped or unbound.
 */
static int vnet_link(void *dev, struct netdev_link *out)
{
    struct vnet *v = (struct vnet *)dev;

    if (!v) {
        return 0;
    }
    out->up = v->up && v->link_up;
    out->mbps = v->speed;
    out->full_duplex = v->duplex;
    return 1;
}

/* The device's entry in the driver table, and the table of an interface
 * whose device cannot interrupt, which differs in its `poll` alone. */
const struct netdev_ops virtio_net_netdev = {
    "virtio_net", vnet_init, vnet_tx, 0, vnet_stop, vnet_link, vnet_error, vnet_print_info, 0, 0};
static const struct netdev_ops vnet_polled_netdev = {
    "virtio_net", vnet_init,  vnet_tx,         vnet_poll, vnet_stop,
    vnet_link,    vnet_error, vnet_print_info, 0,         0};

/**
 * The driver table's `init` (netdev.h): bring up the `nth` virtio network
 * device the transport took as the interface `nd`, with the device's
 * address and, with VIRTIO_NET_F_MTU, its largest MTU as the interface's
 * limit. NETDEV_ABSENT when there is no such device; NETDEV_FAILED, with
 * `vnet_error` giving the reason and the device given back, when it did
 * not come up; NETDEV_UP otherwise. On a device without interrupts the
 * interface takes the table whose `poll` the tick calls.
 */
static int vnet_init(struct netdev *nd, int nth)
{
    struct virtio_dev *d;
    const pci_device_t *pci;
    struct vnet *v;

    nd->dev = 0;
    if (!vnet_registered) {
        vnet_registered = virtio_driver_register(VIRTIO_ID_NET, virtio_net_bind) == 0;
    }
    if (nth < 0 || nth >= VNET_MAX || virtio_pci_find(VIRTIO_ID_NET, nth, &d) != 0) {
        return NETDEV_ABSENT;
    }
    v = &vnets[nth];
    k_memset(v, 0, sizeof(*v));
    pci = virtio_pci_function(d);
    v->key = pci ? pci->bus << 8 | pci->device << 3 | pci->function : -1;
    v->vdev = d;
    if (vnet_start(v) != 0) {
        v->vdev = 0;
        virtio_put(d);
        return NETDEV_FAILED;
    }
    d->removed = vnet_removed;
    v->nd = nd;
    nd->dev = v;
    nd->pci_key = v->key;
    if (v->polled) {
        nd->ops = &vnet_polled_netdev;
    }
    k_memcpy(nd->mac, v->mac, 6);
    if (v->mtu_max != 0 && v->mtu_max < NETDEV_ETH_MTU_MAX) {
        nd->mtu_max = v->mtu_max;
        nd->mtu = v->mtu_max;
    }
    return NETDEV_UP;
}

int32_t virtio_net_bind(struct virtio_dev *vdev)
{
    const pci_device_t *pci = virtio_pci_function(vdev);
    int key = pci ? pci->bus << 8 | pci->device << 3 | pci->function : -1;
    struct vnet *v = 0;
    int i, rc;

    for (i = 0; i < VNET_MAX && !v; i++) {
        if (!vnets[i].vdev && vnets[i].nd && vnets[i].key == key && key >= 0) {
            v = &vnets[i];
        }
    }
    if (!v) {
        klog(KLOG_WARN, "virtio_net", "refused: %s: it had no interface at boot", vdev->name);
        virtio_put(vdev);
        return -ENODEV;
    }
    v->vdev = vdev;
    rc = vnet_start(v);
    if (rc != 0) {
        v->vdev = 0;
        virtio_put(vdev);
        return rc;
    }
    vdev->removed = vnet_removed;
    v->nd->ops = v->polled ? &vnet_polled_netdev : &virtio_net_netdev;
    k_memcpy(v->nd->mac, v->mac, 6);
    netdev_carrier_changed(v->nd);
    return 0;
}
