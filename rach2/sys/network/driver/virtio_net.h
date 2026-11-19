/*
 * virtio_net.h: the virtio network device (virtio 1.2, 5.1), QEMU's
 * `virtio-net-pci`, as Linux's drivers/net/virtio_net.c drives it: its
 * feature bits, its configuration, the header before each frame, the
 * control queue's commands, and the driver's entry in the table.
 *
 * The device has no registers of its own: frames go through two
 * virtqueues, and what would be a register access under a hypervisor, an
 * exit served by QEMU, is a write into memory the device reads when it is
 * told. The transport, the queues and the bus are virtio/'s, which the
 * virtio GPU driver uses too; this driver reaches them through virtio.h
 * alone.
 */

#ifndef _VIRTIO_NET_H_
#define _VIRTIO_NET_H_

#include "../../lib/kint.h"
#include "netdev.h"

/* The cards this driver drives at once. */
#define VNET_MAX NETDEV_MAX

/* Feature bits (virtio 1.2, 5.1.3). */
#define VIRTIO_NET_F_CSUM           0 /* the device checksums a frame sent */
#define VIRTIO_NET_F_GUEST_CSUM     1 /* the driver takes partial checksums */
#define VIRTIO_NET_F_MTU            3 /* the configuration has the largest MTU */
#define VIRTIO_NET_F_MAC            5 /* the configuration has the address */
#define VIRTIO_NET_F_GUEST_TSO4     7 /* the driver takes TCP segments unsplit */
#define VIRTIO_NET_F_GUEST_TSO6     8
#define VIRTIO_NET_F_GUEST_UFO      10
#define VIRTIO_NET_F_HOST_TSO4      11 /* the device splits TCP segments */
#define VIRTIO_NET_F_HOST_TSO6      12
#define VIRTIO_NET_F_HOST_UFO       14
#define VIRTIO_NET_F_MRG_RXBUF      15 /* a frame may span receive buffers */
#define VIRTIO_NET_F_STATUS         16 /* the configuration has the link */
#define VIRTIO_NET_F_CTRL_VQ        17 /* the control queue */
#define VIRTIO_NET_F_CTRL_RX        18 /* its receive mode commands */
#define VIRTIO_NET_F_CTRL_VLAN      19
#define VIRTIO_NET_F_GUEST_ANNOUNCE 21
#define VIRTIO_NET_F_MQ             22
#define VIRTIO_NET_F_CTRL_MAC_ADDR  23 /* its command that sets the address */
#define VIRTIO_NET_F_SPEED_DUPLEX   63 /* the configuration has both */

/*
 * What the driver asks for. Linux asks for the checksum and segmentation
 * offloads as well, since its stack can leave a checksum to the device and
 * take a segment of 64 KiB; Rach2's stack builds each frame whole, at most
 * 1514 bytes, with every checksum computed, and checks every checksum it
 * receives, so it could use none of them and the driver asks for none, as
 * Linux's asks only for what its stack has. The device then gives and takes
 * finished frames. The ring's own bits, VIRTIO_F_EVENT_IDX and
 * VIRTIO_F_INDIRECT_DESC, are the core's to take.
 */
#define VNET_FEATURES                                                      \
    (VIRTIO_BIT(VIRTIO_NET_F_MAC) | VIRTIO_BIT(VIRTIO_NET_F_STATUS) |      \
     VIRTIO_BIT(VIRTIO_NET_F_MTU) | VIRTIO_BIT(VIRTIO_NET_F_MRG_RXBUF) |   \
     VIRTIO_BIT(VIRTIO_NET_F_CTRL_VQ) | VIRTIO_BIT(VIRTIO_NET_F_CTRL_RX) | \
     VIRTIO_BIT(VIRTIO_NET_F_CTRL_MAC_ADDR) | VIRTIO_BIT(VIRTIO_NET_F_SPEED_DUPLEX))

/* The device configuration (virtio 1.2, 5.1.4), by offset: the station
 * address, the status word, the largest MTU, the speed in Mb/s and the
 * duplex, each there only with its feature. */
#define VNET_CFG_MAC    0
#define VNET_CFG_STATUS 6
#define VNET_CFG_MTU    10
#define VNET_CFG_SPEED  12
#define VNET_CFG_DUPLEX 16

#define VIRTIO_NET_S_LINK_UP 1u

/* The speeds Linux takes from the configuration: 0 to INT_MAX, as its
 * ethtool_validate_speed; 0xFFFFFFFF is unknown. A duplex of 0 is half, 1
 * full and 0xFF unknown. */
#define VNET_SPEED_MAX 0x7FFFFFFFu

/*
 * The header before each frame, both ways (virtio 1.2, 5.1.6), 12 bytes
 * with VIRTIO_F_VERSION_1 whatever else was taken. A frame sent has it all
 * zeros: no checksum left to the device, no segmentation. A frame received
 * has `num_buffers`, the receive buffers it spans, 1 without
 * VIRTIO_NET_F_MRG_RXBUF.
 */
struct virtio_net_hdr {
    uint8_t flags;
    uint8_t gso_type;
    uint16_t hdr_len;
    uint16_t gso_size;
    uint16_t csum_start;
    uint16_t csum_offset;
    uint16_t num_buffers;
};

#define VNET_HDR 12u

/* The control queue (virtio 1.2, 5.1.6.5): a command is a class and a
 * command byte, the command's data, and a byte the device answers in. */
#define VIRTIO_NET_CTRL_RX           0
#define VIRTIO_NET_CTRL_RX_PROMISC   0
#define VIRTIO_NET_CTRL_RX_ALLMULTI  1
#define VIRTIO_NET_CTRL_MAC          1
#define VIRTIO_NET_CTRL_MAC_ADDR_SET 1
#define VIRTIO_NET_OK                0

/* The queues, with one pair of receive and transmit queues. */
#define VNET_Q_RX   0
#define VNET_Q_TX   1
#define VNET_Q_CTRL 2

/*
 * Buffers. Each is half a page of the transport's memory, enough for the
 * header and the largest frame, 1526 bytes, so a frame fits one buffer with
 * VIRTIO_NET_F_MRG_RXBUF or without it. The receive queue is kept full, at
 * most VNET_RX_BUFS buffers; transmit has a pool of at most VNET_TX_BUFS.
 */
#define VNET_BUF       2048u
#define VNET_RX_BUFS   128
#define VNET_TX_BUFS   64
#define VNET_FRAME_MAX 1514u

/* A full transmit pool is waited on for VNET_TX_WAIT_STEPS intervals of
 * VNET_TX_WAIT_USEC before the frame is dropped, as the e1000 waits on its
 * ring; `stop` waits VNET_DRAIN_STEPS of them for the frames given to the
 * device; and a control command's answer VNET_CTRL_STEPS, 100 ms. */
#define VNET_TX_WAIT_STEPS 10
#define VNET_TX_WAIT_USEC  100
#define VNET_DRAIN_STEPS   100
#define VNET_CTRL_STEPS    1000

/* A device that cannot interrupt has its link read once in this many
 * polls, a fifth of a second of the tick's 250 Hz. */
#define VNET_LINK_POLLS 50

struct virtio_dev;

/**
 * The probe of a virtio network device the transport takes after boot, as
 * a write of /sys/bus/pci/drivers/virtio-pci/bind or a function put in the
 * machine does, Linux's virtnet_probe: the device is brought up on a new
 * interface, `ethN` for the lowest N free and a new index, registered with
 * the stack (`netdev_register`) and down, as Linux leaves a new interface
 * until it is set up. 0; -ENOSPC with VNET_MAX devices driven, the
 * registration's error, or the bring-up's, each with the device given back
 * and no interface left.
 */
int32_t virtio_net_bind(struct virtio_dev *vdev);

/** This driver's entry in the driver table (netdev.c). Its `poll` is NULL:
 * the device interrupts. */
extern const struct netdev_ops virtio_net_netdev;

#endif /* _VIRTIO_NET_H_ */
