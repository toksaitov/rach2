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

/* The ioctls, Linux's numbers (linux/vhost.h). */
#define VHOST_GET_FEATURES         0x8008af00u
#define VHOST_SET_FEATURES         0x4008af00u
#define VHOST_SET_OWNER            0x0000af01u
#define VHOST_RESET_OWNER          0x0000af02u
#define VHOST_SET_MEM_TABLE        0x4008af03u
#define VHOST_SET_VRING_NUM        0x4008af10u
#define VHOST_SET_VRING_ADDR       0x4028af11u
#define VHOST_SET_VRING_BASE       0x4008af12u
#define VHOST_GET_VRING_BASE       0xc008af12u
#define VHOST_SET_VRING_KICK       0x4008af20u
#define VHOST_SET_VRING_CALL       0x4008af21u
#define VHOST_SET_VRING_ERR        0x4008af22u
#define VHOST_SET_BACKEND_FEATURES 0x4008af25u
#define VHOST_GET_BACKEND_FEATURES 0x8008af26u
#define VHOST_NET_SET_BACKEND      0x4008af30u

/* The features offered: the backend adds and strips the virtio header (bit
 * 27, VHOST_NET_F_VIRTIO_NET_HDR, which the guest sees as
 * VIRTIO_F_ANY_LAYOUT), an interrupt when the available ring runs empty
 * (VIRTIO_F_NOTIFY_ON_EMPTY) and the 1.0 layout (VIRTIO_F_VERSION_1). */
#define VHOST_NET_F_VIRTIO_NET_HDR (1ull << 27)
#define VHOST_F_NOTIFY_ON_EMPTY    (1ull << 24)
#define VHOST_F_MRG_RXBUF          (1ull << 15)
#define VHOST_F_VERSION_1          (1ull << 32)
#define VHOST_NET_FEATURES \
    (VHOST_NET_F_VIRTIO_NET_HDR | VHOST_F_NOTIFY_ON_EMPTY | VHOST_F_VERSION_1)

/* The most regions of VHOST_SET_MEM_TABLE, Linux's max_mem_regions. */
#define VHOST_NET_REGIONS 64

/** Register /dev/vhost-net and start the kernel thread. Called once at
 * boot, after devfs is up; a failure is logged and leaves the machine
 * without the device. */
void vhost_net_start(void);

#endif /* _VHOST_NET_H_ */
