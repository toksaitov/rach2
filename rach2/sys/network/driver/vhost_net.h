/* vhost_net.h: /dev/vhost-net, Linux's in-kernel virtio-net backend, which
 * moves a guest's frames between its virtqueues and a tap interface. */
#ifndef _VHOST_NET_H_
#define _VHOST_NET_H_

/*
 * QEMU's `-netdev tap,...,vhost=on` opens /dev/vhost-net, gives it the
 * guest's memory and the two rings of a virtio-net queue pair, an eventfd
 * the guest's doorbell signals for each ring (the kick, a KVM ioeventfd) and
 * an eventfd that raises the ring's interrupt (the call, a KVM irqfd), and
 * then the tap descriptor (VHOST_NET_SET_BACKEND). From then on QEMU reads
 * and writes no frame: a frame the stack sends on the tap goes straight into
 * the guest's receive ring, in the sender's context, and a frame the guest
 * puts on its transmit ring is taken by the kernel thread `vhost-net`, woken
 * by the kick, and given to the stack as a write of the tap would give it.
 *
 * The ioctls are Linux's (linux/vhost.h), with the split ring alone, no
 * indirect descriptors, no event index, no dirty log and no IOTLB. The tap
 * carries no virtio header, so the backend adds and strips the guest's
 * (VHOST_NET_F_VIRTIO_NET_HDR): no offload either way, and num_buffers 1.
 */

/** Register /dev/vhost-net and start the kernel thread. Called once at
 * boot, after devfs is up; a failure is logged and leaves the machine
 * without the device. */
void vhost_net_start(void);

#include "../../lib/kint.h"

/* The counts `vhost_net_counts` gives, by index: frames put on the guests'
 * receive rings, frames that found no buffer there and waited, interrupts
 * raised, doorbells taken, frames taken off the transmit rings; the
 * thread's wakes by a kick, the cycles from those kicks to its running, the
 * wakes on the kicking processor and on another thread of its core, and the
 * cycles it waited for the big lock; then doorbells by processor and the
 * thread's runs by processor, for the first VN_CPUS processors; then the
 * request-response breakdown `vn_trace` gives: requests, acknowledgements
 * and the cycles from the request to them, replies and the cycles from the
 * request to them, and Rach2's turns and the cycles from reply to request;
 * then requests by sending processor, and those sent from the processor the
 * guest last rang its doorbell from; for the acknowledgement and then the
 * reply, the count that had a device interrupt between the request and
 * them, the cycles from the request to the last such interrupt and from it
 * to the frame (`vn_msi_split`); device interrupts during exchanges; and
 * device interrupts by processor. */
#define VN_WAKES            5
#define VN_WAKE_CYCLES      6
#define VN_WAKE_SAME        7
#define VN_WAKE_SIBLING     8
#define VN_BKL_CYCLES       9
#define VN_CPUS             4
#define VN_KICK_CPU0        10
#define VN_RUN_CPU0         (VN_KICK_CPU0 + VN_CPUS)
#define VN_TR_REQS          (VN_RUN_CPU0 + VN_CPUS)
#define VN_TR_ACKS          (VN_TR_REQS + 1)
#define VN_TR_ACK_CYCLES    (VN_TR_REQS + 2)
#define VN_TR_REPLIES       (VN_TR_REQS + 3)
#define VN_TR_REPLY_CYCLES  (VN_TR_REQS + 4)
#define VN_TR_TURNS         (VN_TR_REQS + 5)
#define VN_TR_TURN_CYCLES   (VN_TR_REQS + 6)
#define VN_REQ_CPU0         (VN_TR_REQS + 7)
#define VN_REQ_ON_GUEST     (VN_REQ_CPU0 + VN_CPUS)
#define VN_A_MSIS           (VN_REQ_ON_GUEST + 1)
#define VN_B_MSIS           (VN_A_MSIS + 3)
#define VN_MSIS_IN_EXCHANGE (VN_B_MSIS + 3)
#define VN_MSI_CPU0         (VN_MSIS_IN_EXCHANGE + 1)
/* The first device interrupt after each request: count, and cycles from
 * the request to it. */
#define VN_C_MSIS           (VN_MSI_CPU0 + VN_CPUS)
#define VHOST_NET_COUNTS    (VN_C_MSIS + 2)

/** Copy the counts since boot into `c`, which holds VHOST_NET_COUNTS, for
 * /proc/stat. */
void vhost_net_counts(uint64_t *c);

/** Note an interrupt of a device passed to a guest, for the request-response
 * breakdown. Called from VFIO's message handler, with interrupts off. */
void vhost_net_note_msi(void);

#endif /* _VHOST_NET_H_ */
