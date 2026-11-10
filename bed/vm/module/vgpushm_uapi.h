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

/* Interrupts taken and doorbells rung since the module bound the device,
 * and the pins of VGPUSHM_IOC_PIN: those held now and their pages, among
 * them those kept after their file closed, and those made and refused. */
struct vgpushm_counts {
    __u64 irqs[VGPUSHM_VECTORS];    /* each vector's interrupts */
    __u64 signals[VGPUSHM_VECTORS]; /* of them, those that found an eventfd */
    __u64 rings;                    /* doorbell writes */
    __u64 pins;                     /* pins held */
    __u64 pinned;                   /* pages they hold */
    __u64 kept;                     /* of the pins, those kept for the driver's epoch */
    __u64 made;                     /* pins made since the bind */
    __u64 refused;                  /* pins refused since the bind */
};

/*
 * VGPUSHM_IOC_PIN: pin `size` bytes from `offset` of the buffer behind the
 * descriptor `fd`, a memfd or another file of shmem, or a dma-buf whose
 * pages are RAM, for the epoch `epoch` of the driver's (vgpushm.h), and get
 * a handle to it and its pages' count. Both are whole pages.
 */
struct vgpushm_pin {
    __s32 fd;
    __u32 epoch;
    __u64 offset;
    __u64 size;
    __u32 handle; /* out */
    __u32 kind;   /* out: VGPUSHM_PIN_SHMEM or VGPUSHM_PIN_DMABUF */
    __u64 pages;  /* out */
};

#define VGPUSHM_PIN_SHMEM  1
#define VGPUSHM_PIN_DMABUF 2

/* A run of the VM's physical pages: the first's address and the count. */
struct vgpushm_run {
    __u64 addr;
    __u64 pages;
};

/*
 * VGPUSHM_IOC_PAGES: the runs of a pin's pages from its page `first`, at
 * most `max` of them, into the array at `runs`, each run as long as the
 * pages follow each other; `count` gets the runs written, and `next` the
 * page after the last one's end.
 */
struct vgpushm_pages {
    __u32 handle;
    __u32 max;
    __u64 first;
    __u64 runs;  /* a user pointer to struct vgpushm_run[max] */
    __u32 count; /* out */
    __u32 reserved;
    __u64 next; /* out */
};

#define VGPUSHM_IOC_MAGIC   'V'
#define VGPUSHM_IOC_INFO    _IOR(VGPUSHM_IOC_MAGIC, 0, struct vgpushm_info)
#define VGPUSHM_IOC_EVENTFD _IOW(VGPUSHM_IOC_MAGIC, 1, struct vgpushm_eventfd)
/* The argument is the doorbell value, VGS_DOORBELL(peer, vector). */
#define VGPUSHM_IOC_RING    _IOW(VGPUSHM_IOC_MAGIC, 2, __u32)
#define VGPUSHM_IOC_COUNTS  _IOR(VGPUSHM_IOC_MAGIC, 3, struct vgpushm_counts)
#define VGPUSHM_IOC_PIN     _IOWR(VGPUSHM_IOC_MAGIC, 4, struct vgpushm_pin)
#define VGPUSHM_IOC_PAGES   _IOWR(VGPUSHM_IOC_MAGIC, 5, struct vgpushm_pages)
/* The argument is a pin's handle. */
#define VGPUSHM_IOC_UNPIN   _IOW(VGPUSHM_IOC_MAGIC, 6, __u32)

#endif /* VGPUSHM_UAPI_H */
