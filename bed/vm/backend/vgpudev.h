/*
 * vgpudev.h: the device side of the shared-memory GPU transport, as the
 * VM's backend runs it: the transport and virtio-gpu's command parser in
 * device.c, and the renderer and display operations it calls, which
 * virgl.c and kms.c implement and the fuzzer stubs.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef VGPUDEV_H
#define VGPUDEV_H

#include <stdint.h>
#include <sys/uio.h>

#include "vgpushm.h"

/* virtio-gpu's feature bits, as virtio_gpu.h numbers them. */
#define VD_F_VIRGL        0
#define VD_F_EDID         1
#define VD_F_BLOB         3
#define VD_F_CONTEXT_INIT 4

/* Header flags. */
#define VD_FLAG_FENCE         1u
#define VD_FLAG_INFO_RING_IDX 2u

/* Responses. */
#define VD_RESP_OK_NODATA         0x1100u
#define VD_RESP_OK_DISPLAY_INFO   0x1101u
#define VD_RESP_OK_CAPSET_INFO    0x1102u
#define VD_RESP_OK_CAPSET         0x1103u
#define VD_RESP_OK_EDID           0x1104u
#define VD_RESP_OK_MAP_INFO       0x1106u
#define VD_RESP_ERR_UNSPEC        0x1200u
#define VD_RESP_ERR_OUT_OF_MEMORY 0x1201u
#define VD_RESP_ERR_SCANOUT_ID    0x1202u
#define VD_RESP_ERR_RESOURCE_ID   0x1203u
#define VD_RESP_ERR_CONTEXT_ID    0x1204u
#define VD_RESP_ERR_PARAMETER     0x1205u

#define VD_SCANOUTS 16
#define VD_EDID_MAX 1024
/* Objects of one epoch; a device's limits stay below. */
#define VD_OBJS     1024
/* Bytes of the largest response: a page list of up to 4092 runs, what 64 KiB
 * holds after VGS_PAGES_OFF and below the 4096 VGPUSHM_IOC_PAGES gives at
 * once, or a capset, after its 24-byte header. */
#define VD_RESP_MAX 65536
/* Fenced commands awaiting their fence at once. */
#define VD_PENDING  256

/* virtio-gpu's command header. */
struct vd_hdr {
    uint32_t type;
    uint32_t flags;
    uint64_t fence_id;
    uint32_t ctx_id;
    uint8_t ring_idx;
    uint8_t padding[3];
};

struct vd_rect {
    uint32_t x, y, width, height;
};

/* A 3D transfer's box, as virglrenderer's struct virgl_box has it. */
struct vd_box {
    uint32_t x, y, z, w, h, d;
};

/* A resource's creation, 2D or 3D, as virglrenderer takes it. */
struct vd_create {
    uint32_t id, target, format, bind, width, height, depth, array_size, last_level, nr_samples,
        flags;
};

/* A transfer between a resource and its backing, in context ctx. */
struct vd_transfer {
    uint32_t id, ctx, to_host, level, stride, layer_stride;
    uint64_t offset;
    struct vd_box box;
};

/* A blob resource's creation; iov is its guest memory, if any. */
struct vd_blob {
    uint32_t id, ctx, mem, flags;
    uint64_t blob_id, size;
};

/*
 * A page list asked of a mappable blob under VGS_TF_PAGES (vgpushm.h): the
 * runs of the VM-physical pages behind blob `id` from page `first` on, at
 * most `max` into `runs`, its memory pinned for the driver's `epoch` at the
 * first ask and held until the device lets it go (`unpin`).
 */
struct vd_pages {
    uint32_t id, epoch;
    uint64_t size;  /* the blob's bytes, as created */
    uint64_t first; /* below the blob's pages */
    struct vgs_run *runs;
    uint32_t max;      /* at least 1 */
    uint32_t count;    /* out: runs written, at least 1 */
    uint32_t map_info; /* out: VIRTIO_GPU_MAP_CACHE_* of the VM's own mapping */
};

/* One display's mode as the device reports it. */
struct vd_mode {
    uint32_t width, height, enabled;
};

/*
 * What the device asks of the renderer and the display. Each returns 0 or
 * a VD_RESP_ERR_* that the device sends back; the device has checked every
 * id against its table and every range against the region first.
 */
struct vd_ops {
    int (*create)(void *c, const struct vd_create *a);
    void (*unref)(void *c, uint32_t id);
    int (*attach)(void *c, uint32_t id, struct iovec *iov, uint32_t n);
    void (*detach)(void *c, uint32_t id);
    int (*transfer)(void *c, const struct vd_transfer *t);
    int (*blob)(void *c, const struct vd_blob *a, struct iovec *iov, uint32_t n);
    int (*ctx_create)(void *c, uint32_t ctx, uint32_t capset, uint32_t nlen, const char *name);
    void (*ctx_destroy)(void *c, uint32_t ctx);
    void (*ctx_resource)(void *c, uint32_t ctx, uint32_t id, int attach);
    int (*submit)(void *c, uint32_t ctx, void *buf, uint32_t bytes);
    /* Fence the work so far; ring is used only when per_ctx is set. */
    int (*fence)(void *c, uint32_t ctx, int per_ctx, uint32_t ring, uint64_t id);
    /* The capset at index, below the count the device offered. */
    void (*capset_info)(void *c, uint32_t index, uint32_t *id, uint32_t *max_ver,
                        uint32_t *max_size);
    int (*capset)(void *c, uint32_t id, uint32_t ver, void *out, uint32_t size);
    int (*display)(void *c, uint32_t scanout, struct vd_mode *m);
    /* Copy the scanout's EDID to out; *size is its length. */
    int (*edid)(void *c, uint32_t scanout, uint8_t *out, uint32_t *size);
    /* Show resource id's rectangle r; id 0, with r 0, takes the scanout off. */
    int (*scanout)(void *c, uint32_t scanout, uint32_t id, const struct vd_rect *r);
    int (*flush)(void *c, uint32_t scanout, uint32_t id, const struct vd_rect *r);
    /* End every resource and context, and every pin: a new epoch begins. */
    void (*reset)(void *c);
    /* Pin a mappable blob, at its first ask, and give its pages' runs. */
    int (*pages)(void *c, struct vd_pages *q);
    /* Let go of a blob's pin, if it has one. */
    void (*unpin)(void *c, uint32_t id);
};

/* What the backend offers, fixed before the first epoch. */
struct vd_offer {
    uint64_t features; /* virtio-gpu's bits, VERSION_1 added */
    uint64_t transport_features;
    struct vgs_limits limits;
    uint32_t peer;
    uint32_t num_scanouts;
    uint32_t num_capsets;
};

/* Counters a run reports. */
struct vd_counts {
    uint64_t epochs, commands, errors, fenced, completed, notified, faults;
    uint64_t maps, unmaps; /* page lists answered, pins let go of at UNMAP_BLOB */
};

/* A device resource beside its table entry. */
struct vd_res {
    struct iovec *iov; /* its backing in the region, or 0 */
    uint32_t niov;
    uint32_t blob;   /* 1 for a blob resource */
    uint32_t mem;    /* a blob's BLOB_MEM_* */
    uint32_t flags;  /* a blob's BLOB_FLAG_* */
    uint32_t pinned; /* 1 once a page list pinned it, until it is let go */
    uint64_t size;   /* a blob's bytes */
};

/* A fenced command whose completion waits on its fence. */
struct vd_pending {
    uint32_t used;    /* 1 while the slot holds a command */
    uint32_t queue;   /* the queue the chain came on */
    uint32_t head;    /* its chain's head */
    uint32_t ctx;     /* the fence's context, 0 for the global timeline */
    uint32_t per_ctx; /* 1 for a context fence on ring */
    uint32_t ring;
    uint64_t fence_id; /* the command's fence */
    uint32_t rlen;     /* response bytes in resp */
    uint8_t resp[24];  /* a fenced command's response is a bare header */
    struct vgs_chain chain;
};

/* The device: its region, its copy of each block and its tables. */
struct vd {
    volatile uint8_t *base;
    uint64_t size;
    struct vgs_layout l;
    struct vgs_device me; /* the offer, as written */
    struct vgs_driver drv;
    struct vd_offer offer;
    struct vgs_table t;
    struct vgs_obj obj[VD_OBJS];
    struct vd_res res[VD_OBJS];
    uint16_t last[VGS_QUEUES];     /* avail entries consumed */
    uint16_t used[VGS_QUEUES];     /* used entries published */
    uint32_t scanout[VD_SCANOUTS]; /* each scanout's resource, or 0 */
    struct vd_pending pend[VD_PENDING];
    uint32_t npend;
    int kick;      /* used entries published since the last notify */
    uint8_t *cmd;  /* offer.limits.max_cmd bytes: a chain's readable part */
    uint32_t room; /* the writable bytes of the chain being served */
    uint8_t resp[VD_RESP_MAX];
    const struct vd_ops *ops;
    void *cookie;
    /* Ring the driver's vector; 0 when the transport has no doorbells. */
    void (*ring)(void *cookie, uint32_t peer, uint32_t vector);
    void *ring_cookie;
    int verbose;
    struct vd_counts n;
};

int vd_init(struct vd *d, volatile void *base, uint64_t size, const struct vd_offer *offer,
            const struct vd_ops *ops, void *cookie);
void vd_fini(struct vd *d);
int vd_step(struct vd *d);
void vd_fence_retired(struct vd *d, uint32_t ctx, int per_ctx, uint32_t ring, uint64_t id);
void vd_kick(struct vd *d);

/* vd_step's results. */
#define VD_IDLE   0 /* nothing changed */
#define VD_WORKED 1 /* a state moved or a command was served */
#define VD_GONE   2 /* the driver detached in order */

#endif /* VGPUDEV_H */
