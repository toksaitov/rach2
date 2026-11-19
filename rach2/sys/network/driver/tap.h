/* tap.h: /dev/net/tun, Linux's TAP interface: an Ethernet interface whose
 * other end is a descriptor a program reads and writes frames on. */
#ifndef _TAP_H_
#define _TAP_H_

#include "../../lib/kint.h"

/*
 * A program opens /dev/net/tun and names an interface with TUNSETIFF; the
 * kernel makes the Ethernet interface `tapN`, or the name asked for, and
 * from then on each frame the stack sends on the interface is one read of
 * the descriptor, and each write of the descriptor is a frame the
 * interface receives. QEMU's `-netdev tap` is the user this was written
 * for: it joins a guest's network card to an interface of the host, so
 * that the host routes through the guest or the guest through the host.
 *
 * Only the TAP form is offered, Ethernet frames with no packet information
 * header (IFF_TAP | IFF_NO_PI), and no virtio header (IFF_VNET_HDR), so a
 * frame is whole on each side: no checksum is left to the other side and no
 * frame is larger than the interface's MTU allows. QEMU then offers its
 * guest's virtio network device none of the offloads.
 */

/* The interfaces at once. They take slots of netdev.h's NETDEV_MAX. */
#define TAP_MAX 2

/* The frames an interface holds for its reader; one more is dropped and
 * counted, as a card's full ring drops. Linux's tun queues 1000 by default
 * (its tx_queue_len); with 256, a gateway guest that took 50000 frames a
 * second lost one in two of Rach2's acknowledgements and a few percent of
 * its data. */
#define TAP_RING 1024

/* The ioctls of the descriptor, Linux's numbers (linux/if_tun.h). */
#define TUNSETNOCSUM    0x400454c8u
#define TUNSETDEBUG     0x400454c9u
#define TUNSETIFF       0x400454cau
#define TUNSETPERSIST   0x400454cbu
#define TUNSETOWNER     0x400454ccu
#define TUNSETLINK      0x400454cdu
#define TUNSETGROUP     0x400454ceu
#define TUNGETFEATURES  0x800454cfu
#define TUNSETOFFLOAD   0x400454d0u
#define TUNGETIFF       0x800454d2u
#define TUNGETSNDBUF    0x800454d3u
#define TUNSETSNDBUF    0x400454d4u
#define TUNGETVNETHDRSZ 0x800454d7u
#define TUNSETVNETHDRSZ 0x400454d8u
#define TUNSETQUEUE     0x400454d9u
#define TUNSETVNETLE    0x400454dcu
#define TUNGETVNETLE    0x800454ddu
#define TUNSETVNETBE    0x400454deu
#define TUNGETVNETBE    0x800454dfu

/* The flags of TUNSETIFF's `ifr_flags`. */
#define TAP_IFF_TUN         0x0001
#define TAP_IFF_TAP         0x0002
#define TAP_IFF_MULTI_QUEUE 0x0100
#define TAP_IFF_PERSIST     0x0800
#define TAP_IFF_NO_PI       0x1000
#define TAP_IFF_ONE_QUEUE   0x2000
#define TAP_IFF_VNET_HDR    0x4000
#define TAP_IFF_TUN_EXCL    0x8000

/** Register /dev/net/tun. Called once at boot, after devfs is up; a
 * failure is logged and leaves the machine without the device. */
void tap_start(void);

struct fd_entry;

/** Whether the descriptor `e` is an open of /dev/net/tun. */
int tap_is(const struct fd_entry *e);

/**
 * Give the interface of the descriptor `e` the frame of `len` bytes at
 * `frame`, in kernel memory, as one write of the descriptor does; writev
 * gathers its vector and calls this, since a frame is one write however
 * many elements carry it. `len`, with the frame received, or counted as
 * dropped when it is longer than an interface carries, in which case
 * `frame` is not read; -EBADFD before TUNSETIFF; -EINVAL for less than an
 * Ethernet header. Called under the big kernel lock.
 */
int32_t tap_write_frame(struct fd_entry *e, const uint8_t *frame, uint32_t len);

struct tap;

/*
 * A backend that takes the frames the interface sends in place of its
 * reader, as /dev/vhost-net does. Both calls are made under the big lock.
 */
struct tap_backend {
    /* Take the frame of `len` bytes: 1 when taken, 0 when there is no room,
     * and the frame waits in the ring until `tap_backend_drain`. Called
     * with the ring's lock held and interrupts off; it does not sleep. */
    int (*rx)(struct tap_backend *b, const uint8_t *frame, uint16_t len);
    /* The interface's descriptor closed; the backend has been let go. */
    void (*gone)(struct tap_backend *b);
};

/** The interface of the descriptor `e`, or NULL when `e` is no open of
 * /dev/net/tun or has no interface yet. */
struct tap *tap_of(const struct fd_entry *e);

/** Give the interface `t` the backend `b`, or take it back with NULL: 0, or
 * -EBUSY when another backend has it. */
int tap_backend_set(struct tap *t, struct tap_backend *b);

/** Offer the frames waiting in the ring of `t` to its backend, in order,
 * until it takes no more. */
void tap_backend_drain(struct tap *t);

/** Give the interface `t` the frame of `len` bytes, as a write of its
 * descriptor does (`tap_write_frame`). */
int32_t tap_inject(struct tap *t, const uint8_t *frame, uint32_t len);

/** The frames the interfaces' readers took, the frames written to them,
 * the frames dropped for a full ring and the bare acknowledgements a newer
 * one replaced in the ring, for /proc/stat. */
void tap_counts(uint64_t *read, uint64_t *written, uint64_t *dropped, uint64_t *thinned);

#endif /* _TAP_H_ */
