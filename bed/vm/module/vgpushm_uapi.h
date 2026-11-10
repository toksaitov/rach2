/*
 * vgpushm_uapi.h: the interface of the VM's vgpushm module, which hands the
 * shared region of QEMU's ivshmem device and its doorbell vectors to the
 * device backend in bed/vm/backend.
 * SPDX-License-Identifier: GPL-2.0-only WITH Linux-syscall-note
 */
#ifndef VGPUSHM_UAPI_H
#define VGPUSHM_UAPI_H

#include <linux/ioctl.h>
#include <linux/types.h>

/* The vectors an ivshmem-doorbell device of the transport has. */
#define VGPUSHM_VECTORS 2

/* The device as the module found it, from VGPUSHM_IOC_INFO. */
struct vgpushm_info {
    __u64 size;    /* bytes of the shared region, BAR 2; mmap offset 0 */
    __u32 peer;    /* the VM's ivshmem peer id, IVPosition; ~0 for ivshmem-plain */
    __u32 vectors; /* MSI-X vectors enabled; 0 when the device has none */
};

/* An eventfd that a vector signals, for VGPUSHM_IOC_EVENTFD; fd -1 unbinds. */
struct vgpushm_eventfd {
    __u32 vector;
    __s32 fd;
};

/* Interrupts taken and doorbells rung since the module bound the device. */
struct vgpushm_counts {
    __u64 irqs[VGPUSHM_VECTORS];    /* each vector's interrupts */
    __u64 signals[VGPUSHM_VECTORS]; /* of them, those that found an eventfd */
    __u64 rings;                    /* doorbell writes */
};

#define VGPUSHM_IOC_MAGIC   'V'
#define VGPUSHM_IOC_INFO    _IOR(VGPUSHM_IOC_MAGIC, 0, struct vgpushm_info)
#define VGPUSHM_IOC_EVENTFD _IOW(VGPUSHM_IOC_MAGIC, 1, struct vgpushm_eventfd)
/* The argument is the doorbell value, VGS_DOORBELL(peer, vector). */
#define VGPUSHM_IOC_RING    _IOW(VGPUSHM_IOC_MAGIC, 2, __u32)
#define VGPUSHM_IOC_COUNTS  _IOR(VGPUSHM_IOC_MAGIC, 3, struct vgpushm_counts)

#endif /* VGPUSHM_UAPI_H */
