/*
 * device.c: the device side of the shared-memory GPU transport: the
 * handshake of each epoch, the two virtqueues, and virtio-gpu's commands
 * checked and handed to the renderer and display operations of vgpudev.h.
 * It calls no library but libc, so that the fuzzer links it alone.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#define _POSIX_C_SOURCE 200809L
#define VGS_IMPLEMENTATION
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "vgpudev.h"

/* virtio-gpu's commands. */
#define CMD_GET_DISPLAY_INFO        0x0100u
#define CMD_RESOURCE_CREATE_2D      0x0101u
#define CMD_RESOURCE_UNREF          0x0102u
#define CMD_SET_SCANOUT             0x0103u
#define CMD_RESOURCE_FLUSH          0x0104u
#define CMD_TRANSFER_TO_HOST_2D     0x0105u
#define CMD_RESOURCE_ATTACH_BACKING 0x0106u
#define CMD_RESOURCE_DETACH_BACKING 0x0107u
#define CMD_GET_CAPSET_INFO         0x0108u
#define CMD_GET_CAPSET              0x0109u
#define CMD_GET_EDID                0x010au
#define CMD_RESOURCE_CREATE_BLOB    0x010cu
#define CMD_CTX_CREATE              0x0200u
#define CMD_CTX_DESTROY             0x0201u
#define CMD_CTX_ATTACH_RESOURCE     0x0202u
#define CMD_CTX_DETACH_RESOURCE     0x0203u
#define CMD_RESOURCE_CREATE_3D      0x0204u
#define CMD_TRANSFER_TO_HOST_3D     0x0205u
#define CMD_TRANSFER_FROM_HOST_3D   0x0206u
#define CMD_SUBMIT_3D               0x0207u
#define CMD_RESOURCE_MAP_BLOB       0x0208u
#define CMD_RESOURCE_UNMAP_BLOB     0x0209u

#define BLOB_MEM_GUEST  1u
#define BLOB_MEM_HOST3D 2u

#define BLOB_FLAG_USE_MAPPABLE 1u

/* The commands' bodies after their header, as virtio_gpu.h lays them out. */
struct c_create_2d {
    uint32_t resource_id, format, width, height;
};
struct c_resource {
    uint32_t resource_id, padding;
};
struct c_set_scanout {
    struct vd_rect r;
    uint32_t scanout_id, resource_id;
};
struct c_flush {
    struct vd_rect r;
    uint32_t resource_id, padding;
};
struct c_transfer_2d {
    struct vd_rect r;
    uint64_t offset;
    uint32_t resource_id, padding;
};
struct c_attach {
    uint32_t resource_id, nr_entries;
};
struct c_mem_entry {
    uint64_t addr;
    uint32_t length, padding;
};
struct c_capset_info {
    uint32_t capset_index, padding;
};
struct c_capset {
    uint32_t capset_id, capset_version;
};
struct c_edid {
    uint32_t scanout, padding;
};
struct c_ctx_create {
    uint32_t nlen, context_init;
    char debug_name[64];
};
struct c_create_3d {
    uint32_t resource_id, target, format, bind, width, height, depth, array_size, last_level,
        nr_samples, flags, padding;
};
struct c_transfer_3d {
    struct vd_box box;
    uint64_t offset;
    uint32_t resource_id, level, stride, layer_stride;
};
struct c_submit {
    uint32_t size, padding;
};
struct c_blob {
    uint32_t resource_id, blob_mem, blob_flags, nr_entries;
    uint64_t blob_id, size;
};
struct c_map_blob {
    uint32_t resource_id, padding;
    uint64_t offset;
};

/*
 * Copy n bytes at offset off of the region, checked before, to private dst.
 * The module maps the region write-back, so memcpy reads it at the speed of
 * RAM; only dst is parsed after, so bytes the driver changes meanwhile
 * change nothing that was checked.
 */
static void region_get(struct vd *d, uint64_t off, void *dst, uint64_t n)
{
    memcpy(dst, (const void *)(d->base + off), (size_t)n);
}

/* Copy n private bytes to offset off of the region, checked before. */
static void region_put(struct vd *d, uint64_t off, const void *src, uint64_t n)
{
    memcpy((void *)(d->base + off), src, (size_t)n);
}

/** Ring the driver's vector when the transport has doorbells. */
static void notify(struct vd *d, uint32_t vector, int accepted_only)
{
    if (d->ring == 0) {
        return;
    }
    if (accepted_only && (d->drv.transport_features & VGS_TF_DOORBELL) == 0) {
        return;
    }
    if ((d->offer.transport_features & VGS_TF_DOORBELL) == 0 || d->drv.peer > 0xffffu) {
        return;
    }
    d->ring(d->ring_cookie, d->drv.peer, vector);
    __atomic_fetch_add(&d->n.notified, 1, __ATOMIC_RELAXED);
}

/** Publish the device's state and error after its other writes. */
static void publish(struct vd *d, uint32_t state, uint32_t err)
{
    volatile struct vgs_device *v = VGS_DEVICE(d->base);

    d->me.state = state;
    d->me.error = err;
    v->error = err;
    VGS_MB();
    v->state = state;
    VGS_MB();
    notify(d, VGS_VEC_CONTROL, 0);
}

/** Free a resource's backing list, which the renderer has let go. */
static void res_clear(struct vd_res *r)
{
    free(r->iov);
    memset(r, 0, sizeof(*r));
}

/** Drop the stream being joined from parts, if any. */
static void join_drop(struct vd *d)
{
    free(d->join);
    d->join = 0;
    d->join_len = d->join_total = 0;
    d->join_err = d->join_ctx = 0;
    d->join_open = 0;
}

/**
 * End every object of the old epoch, in the renderer and the table, drop
 * the fenced commands of its queues and a stream half joined, and write the
 * offer for the driver's new epoch, every field before the state.
 */
static void offer(struct vd *d, const struct vgs_driver *drv)
{
    volatile struct vgs_device *v = VGS_DEVICE(d->base);
    uint32_t i;

    d->ops->reset(d->cookie);
    join_drop(d);
    for (i = 0; i < VD_OBJS; i++) {
        res_clear(&d->res[i]);
    }
    vgs_table_epoch(&d->t, drv->epoch);
    memset(d->last, 0, sizeof(d->last));
    memset(d->scanout, 0, sizeof(d->scanout));
    memset(d->ahead, 0, sizeof(d->ahead));
    d->seen = 0;
    pthread_mutex_lock(&d->lock);
    memset(d->used, 0, sizeof(d->used));
    memset(d->pend, 0, sizeof(d->pend));
    d->drv = *drv;
    d->npend = 0;
    d->nunfenced = 0;
    d->kick = 0;
    pthread_mutex_unlock(&d->lock);
    memset(&d->me, 0, sizeof(d->me));
    d->me.epoch = drv->epoch;
    d->me.state = VGS_DEV_READY;
    d->me.peer = d->offer.peer;
    d->me.transport_features = d->offer.transport_features;
    d->me.features = d->offer.features | VGS_F_VERSION_1;
    d->me.limits = d->offer.limits;
    d->me.num_scanouts = d->offer.num_scanouts;
    d->me.num_capsets = d->offer.num_capsets;
    v->state = VGS_DEV_NONE;
    VGS_MB();
    /* The heartbeat, the block's last word, is vd_beat's alone. */
    region_put(d, VGS_DEVICE_OFF + 8, (const uint8_t *)&d->me + 8,
               offsetof(struct vgs_device, heartbeat) - 8);
    VGS_MB();
    v->epoch = drv->epoch;
    VGS_MB();
    v->state = VGS_DEV_READY;
    VGS_MB();
    d->n.epochs++;
    notify(d, VGS_VEC_CONTROL, 0);
}

/* The command buffer's first size; a longer chain grows it (cmd_grow). */
#define VD_CMD_FIRST (1u << 20)

/**
 * Prepare a device for the region at base: its tables, its command buffer
 * and its offer, fixed for every epoch. Returns 0, or -1 when the offer's
 * limits break the transport's rules or the buffer cannot be allocated.
 */
int vd_init(struct vd *d, volatile void *base, uint64_t size, const struct vd_offer *off,
            const struct vd_ops *ops, void *cookie)
{
    memset(d, 0, sizeof(*d));
    if (vgs_check_limits(&off->limits) != VGS_OK ||
        off->limits.max_resources + off->limits.max_contexts > VD_OBJS || off->num_scanouts == 0 ||
        off->num_scanouts > VD_SCANOUTS) {
        return -1;
    }
    d->cmd_cap = off->limits.max_cmd < VD_CMD_FIRST ? off->limits.max_cmd : VD_CMD_FIRST;
    d->cmd = malloc(d->cmd_cap);
    if (d->cmd == 0) {
        return -1;
    }
    if (pthread_mutex_init(&d->lock, 0) != 0) {
        free(d->cmd);
        d->cmd = 0;
        return -1;
    }
    d->base = base;
    d->size = size;
    d->offer = *off;
    d->ops = ops;
    d->cookie = cookie;
    vgs_table_init(&d->t, d->obj, VD_OBJS, &off->limits, 0);
    return 0;
}

/** Free what vd_init and the epochs allocated; the renderer is the caller's. */
void vd_fini(struct vd *d)
{
    uint32_t i;

    for (i = 0; i < VD_OBJS; i++) {
        res_clear(&d->res[i]);
    }
    join_drop(d);
    free(d->cmd);
    d->cmd = 0;
    pthread_mutex_destroy(&d->lock);
}

/** Find a resource of the current epoch, or return 0. */
static struct vd_res *find_res(struct vd *d, uint32_t id, struct vgs_obj **o)
{
    struct vgs_obj *x = vgs_table_find(&d->t, VGS_OBJ_RESOURCE, id);

    if (o) {
        *o = x;
    }
    return x ? &d->res[x - d->obj] : 0;
}

static void release(struct vd *d, struct vd_pending *p);

/** Return whether context id is live in the current epoch. */
static int ctx_live(struct vd *d, uint32_t id)
{
    return vgs_table_find(&d->t, VGS_OBJ_CONTEXT, id) != 0;
}

/**
 * Turn n memory entries at ent into a backing list in the region: each
 * entry inside the heap by vgs_check_range, the total charged to o.
 * Returns 0 with *iov allocated, or a VD_RESP_ERR_*.
 */
static int backing(struct vd *d, struct vgs_obj *o, const uint8_t *ent, uint32_t n,
                   struct iovec **iov)
{
    struct c_mem_entry e;
    uint64_t total = 0;
    uint32_t i;

    if (n == 0 || n > d->me.limits.max_entries) {
        return VD_RESP_ERR_PARAMETER;
    }
    *iov = calloc(n, sizeof(**iov));
    if (*iov == 0) {
        return VD_RESP_ERR_OUT_OF_MEMORY;
    }
    for (i = 0; i < n; i++) {
        memcpy(&e, ent + (uint64_t)i * sizeof(e), sizeof(e));
        if (vgs_check_range(&d->l, e.addr, e.length) != VGS_OK) {
            free(*iov);
            *iov = 0;
            return VD_RESP_ERR_PARAMETER;
        }
        (*iov)[i].iov_base = (void *)(d->base + e.addr);
        (*iov)[i].iov_len = e.length;
        total += e.length;
    }
    if (vgs_table_charge(&d->t, o, total) != VGS_OK) {
        free(*iov);
        *iov = 0;
        return VD_RESP_ERR_OUT_OF_MEMORY;
    }
    return 0;
}

/** Add a resource to the table: VD_RESP_ERR_* for a bad or a live id. */
static int res_add(struct vd *d, uint32_t id)
{
    int err = vgs_table_add(&d->t, VGS_OBJ_RESOURCE, id);

    if (err == VGS_E_QUOTA) {
        return VD_RESP_ERR_OUT_OF_MEMORY;
    }
    return err == VGS_OK ? 0 : VD_RESP_ERR_RESOURCE_ID;
}

/** Remove a resource the renderer refused to make. */
static void res_drop(struct vd *d, uint32_t id)
{
    struct vgs_obj *o;
    struct vd_res *r = find_res(d, id, &o);

    if (r != 0) {
        res_clear(r);
        vgs_table_del(&d->t, o);
    }
}

/** RESOURCE_CREATE_2D, as QEMU makes one: a render target, y 0 at top. */
static int cmd_create_2d(struct vd *d, const uint8_t *body)
{
    struct c_create_2d c;
    struct vd_create a;
    int err;

    memcpy(&c, body, sizeof(c));
    if (c.width == 0 || c.height == 0) {
        return VD_RESP_ERR_PARAMETER;
    }
    if ((err = res_add(d, c.resource_id)) != 0) {
        return err;
    }
    memset(&a, 0, sizeof(a));
    a.id = c.resource_id;
    a.target = 2;
    a.format = c.format;
    a.bind = 1u << 1;
    a.width = c.width;
    a.height = c.height;
    a.depth = 1;
    a.array_size = 1;
    a.flags = 1;
    if ((err = d->ops->create(d->cookie, &a)) != 0) {
        res_drop(d, c.resource_id);
    }
    return err;
}

/** RESOURCE_CREATE_3D: the driver's fields, checked by the renderer. */
static int cmd_create_3d(struct vd *d, const uint8_t *body)
{
    struct c_create_3d c;
    struct vd_create a;
    int err;

    memcpy(&c, body, sizeof(c));
    if ((err = res_add(d, c.resource_id)) != 0) {
        return err;
    }
    a.id = c.resource_id;
    a.target = c.target;
    a.format = c.format;
    a.bind = c.bind;
    a.width = c.width;
    a.height = c.height;
    a.depth = c.depth;
    a.array_size = c.array_size;
    a.last_level = c.last_level;
    a.nr_samples = c.nr_samples;
    a.flags = c.flags;
    if ((err = d->ops->create(d->cookie, &a)) != 0) {
        res_drop(d, c.resource_id);
    }
    return err;
}

/** RESOURCE_UNREF: off every scanout, its backing dropped, then gone. */
static int cmd_unref(struct vd *d, const uint8_t *body)
{
    struct c_resource c;
    struct vgs_obj *o;
    struct vd_res *r;
    uint32_t i;

    memcpy(&c, body, sizeof(c));
    if ((r = find_res(d, c.resource_id, &o)) == 0) {
        return VD_RESP_ERR_RESOURCE_ID;
    }
    for (i = 0; i < VD_SCANOUTS; i++) {
        if (d->scanout[i] == c.resource_id) {
            d->ops->scanout(d->cookie, i, 0, 0);
            d->scanout[i] = 0;
        }
    }
    if (r->iov != 0 && !r->blob) {
        d->ops->detach(d->cookie, c.resource_id);
    }
    /* A backing of the VM's memory is a pin, which its unpin detaches. */
    if (r->pinned) {
        d->ops->unpin(d->cookie, c.resource_id);
    }
    d->ops->unref(d->cookie, c.resource_id);
    res_clear(r);
    vgs_table_del(&d->t, o);
    return 0;
}

/** RESOURCE_ATTACH_BACKING: entries follow the body in the command. */
static int cmd_attach(struct vd *d, const uint8_t *body, uint32_t blen)
{
    struct c_attach c;
    struct vgs_obj *o;
    struct vd_res *r;
    struct iovec *iov;
    int err;

    memcpy(&c, body, sizeof(c));
    if ((r = find_res(d, c.resource_id, &o)) == 0) {
        return VD_RESP_ERR_RESOURCE_ID;
    }
    if (r->iov != 0 || r->blob || r->backed) {
        return VD_RESP_ERR_UNSPEC;
    }
    if (c.nr_entries > (blen - sizeof(c)) / sizeof(struct c_mem_entry)) {
        return VD_RESP_ERR_PARAMETER;
    }
    if ((err = backing(d, o, body + sizeof(c), c.nr_entries, &iov)) != 0) {
        return err;
    }
    if ((err = d->ops->attach(d->cookie, c.resource_id, iov, c.nr_entries)) != 0) {
        vgs_table_charge(&d->t, o, 0);
        free(iov);
        return err;
    }
    r->iov = iov;
    r->niov = c.nr_entries;
    return 0;
}

/** RESOURCE_DETACH_BACKING: the region's entries, or the VM's memory. */
static int cmd_detach(struct vd *d, const uint8_t *body)
{
    struct c_resource c;
    struct vgs_obj *o;
    struct vd_res *r;

    memcpy(&c, body, sizeof(c));
    if ((r = find_res(d, c.resource_id, &o)) == 0) {
        return VD_RESP_ERR_RESOURCE_ID;
    }
    if (r->backed) {
        d->ops->unpin(d->cookie, c.resource_id);
        r->backed = 0;
        r->pinned = 0;
        r->size = 0;
        return 0;
    }
    if (r->iov == 0 || r->blob) {
        return VD_RESP_ERR_UNSPEC;
    }
    d->ops->detach(d->cookie, c.resource_id);
    res_clear(r);
    vgs_table_charge(&d->t, o, 0);
    return 0;
}

/** TRANSFER_TO_HOST_2D: the rectangle from the backing at offset. */
static int cmd_transfer_2d(struct vd *d, const uint8_t *body)
{
    struct c_transfer_2d c;
    struct vd_transfer t;

    memcpy(&c, body, sizeof(c));
    if (find_res(d, c.resource_id, 0) == 0) {
        return VD_RESP_ERR_RESOURCE_ID;
    }
    memset(&t, 0, sizeof(t));
    t.id = c.resource_id;
    t.to_host = 1;
    t.offset = c.offset;
    t.box.x = c.r.x;
    t.box.y = c.r.y;
    t.box.w = c.r.width;
    t.box.h = c.r.height;
    t.box.d = 1;
    return d->ops->transfer(d->cookie, &t);
}

/** TRANSFER_TO_HOST_3D and TRANSFER_FROM_HOST_3D, in the header's context. */
static int cmd_transfer_3d(struct vd *d, const struct vd_hdr *h, const uint8_t *body)
{
    struct c_transfer_3d c;
    struct vd_transfer t;

    memcpy(&c, body, sizeof(c));
    if (find_res(d, c.resource_id, 0) == 0) {
        return VD_RESP_ERR_RESOURCE_ID;
    }
    if (h->ctx_id != 0 && !ctx_live(d, h->ctx_id)) {
        return VD_RESP_ERR_CONTEXT_ID;
    }
    t.id = c.resource_id;
    t.ctx = h->ctx_id;
    t.to_host = h->type == CMD_TRANSFER_TO_HOST_3D;
    t.level = c.level;
    t.stride = c.stride;
    t.layer_stride = c.layer_stride;
    t.offset = c.offset;
    t.box = c.box;
    return d->ops->transfer(d->cookie, &t);
}

/** SET_SCANOUT: resource 0 turns the scanout off. */
static int cmd_set_scanout(struct vd *d, const uint8_t *body)
{
    struct c_set_scanout c;
    int err;

    memcpy(&c, body, sizeof(c));
    if (c.scanout_id >= d->me.num_scanouts) {
        return VD_RESP_ERR_SCANOUT_ID;
    }
    if (c.resource_id == 0) {
        d->scanout[c.scanout_id] = 0;
        return d->ops->scanout(d->cookie, c.scanout_id, 0, 0);
    }
    if (find_res(d, c.resource_id, 0) == 0) {
        return VD_RESP_ERR_RESOURCE_ID;
    }
    if (c.r.width == 0 || c.r.height == 0) {
        return VD_RESP_ERR_PARAMETER;
    }
    if ((err = d->ops->scanout(d->cookie, c.scanout_id, c.resource_id, &c.r)) == 0) {
        d->scanout[c.scanout_id] = c.resource_id;
    }
    return err;
}

/** RESOURCE_FLUSH: every scanout showing the resource is updated. */
static int cmd_flush(struct vd *d, const uint8_t *body)
{
    struct c_flush c;
    uint32_t i;
    int err = 0;

    memcpy(&c, body, sizeof(c));
    if (find_res(d, c.resource_id, 0) == 0) {
        return VD_RESP_ERR_RESOURCE_ID;
    }
    for (i = 0; i < d->me.num_scanouts && err == 0; i++) {
        if (d->scanout[i] == c.resource_id) {
            err = d->ops->flush(d->cookie, i, c.resource_id, &c.r);
        }
    }
    return err;
}

/** GET_DISPLAY_INFO into resp; returns the response's length. */
static uint32_t cmd_display_info(struct vd *d, uint8_t *resp)
{
    uint32_t i, rec[6];
    struct vd_mode m;

    for (i = 0; i < d->me.num_scanouts; i++) {
        memset(rec, 0, sizeof(rec));
        if (d->ops->display(d->cookie, i, &m) == 0) {
            rec[2] = m.width;
            rec[3] = m.height;
            rec[4] = m.enabled;
        }
        memcpy(resp + sizeof(struct vd_hdr) + i * sizeof(rec), rec, sizeof(rec));
    }
    return sizeof(struct vd_hdr) + VD_SCANOUTS * sizeof(rec);
}

/** GET_CAPSET_INFO into resp; returns the response's length or 0. */
static uint32_t cmd_capset_info(struct vd *d, const uint8_t *body, uint8_t *resp)
{
    struct c_capset_info c;
    uint32_t out[4] = {0, 0, 0, 0};

    memcpy(&c, body, sizeof(c));
    if (c.capset_index >= d->me.num_capsets) {
        return 0;
    }
    d->ops->capset_info(d->cookie, c.capset_index, &out[0], &out[1], &out[2]);
    memcpy(resp + sizeof(struct vd_hdr), out, sizeof(out));
    return sizeof(struct vd_hdr) + sizeof(out);
}

/** GET_CAPSET into resp; returns the response's length or 0. */
static uint32_t cmd_capset(struct vd *d, const uint8_t *body, uint8_t *resp)
{
    struct c_capset c;
    uint32_t i, id, ver, size;

    memcpy(&c, body, sizeof(c));
    for (i = 0; i < d->me.num_capsets; i++) {
        d->ops->capset_info(d->cookie, i, &id, &ver, &size);
        if (id == c.capset_id) {
            break;
        }
    }
    if (i == d->me.num_capsets || c.capset_version > ver ||
        size > VD_RESP_MAX - sizeof(struct vd_hdr) || size == 0) {
        return 0;
    }
    if (d->ops->capset(d->cookie, id, c.capset_version, resp + sizeof(struct vd_hdr), size) != 0) {
        return 0;
    }
    return sizeof(struct vd_hdr) + size;
}

/** GET_EDID into resp; returns the response's length or 0. */
static uint32_t cmd_edid(struct vd *d, const uint8_t *body, uint8_t *resp)
{
    struct c_edid c;
    uint32_t size = 0, pad = 0;

    memcpy(&c, body, sizeof(c));
    if (c.scanout >= d->me.num_scanouts || (d->me.features & (1ULL << VD_F_EDID)) == 0) {
        return 0;
    }
    memset(resp + sizeof(struct vd_hdr), 0, 8 + VD_EDID_MAX);
    if (d->ops->edid(d->cookie, c.scanout, resp + sizeof(struct vd_hdr) + 8, &size) != 0 ||
        size > VD_EDID_MAX) {
        return 0;
    }
    memcpy(resp + sizeof(struct vd_hdr), &size, 4);
    memcpy(resp + sizeof(struct vd_hdr) + 4, &pad, 4);
    return sizeof(struct vd_hdr) + 8 + VD_EDID_MAX;
}

/** CTX_CREATE: the capset of context_init when the driver accepted it. */
static int cmd_ctx_create(struct vd *d, const struct vd_hdr *h, const uint8_t *body)
{
    struct c_ctx_create c;
    uint32_t capset = 0;
    int err;

    memcpy(&c, body, sizeof(c));
    if (c.nlen > sizeof(c.debug_name)) {
        return VD_RESP_ERR_PARAMETER;
    }
    if (d->drv.features & (1ULL << VD_F_CONTEXT_INIT)) {
        capset = c.context_init & 0xffu;
    } else if (c.context_init != 0) {
        return VD_RESP_ERR_PARAMETER;
    }
    err = vgs_table_add(&d->t, VGS_OBJ_CONTEXT, h->ctx_id);
    if (err != VGS_OK) {
        return err == VGS_E_QUOTA ? VD_RESP_ERR_OUT_OF_MEMORY : VD_RESP_ERR_CONTEXT_ID;
    }
    if ((err = d->ops->ctx_create(d->cookie, h->ctx_id, capset, c.nlen, c.debug_name)) != 0) {
        vgs_table_del(&d->t, vgs_table_find(&d->t, VGS_OBJ_CONTEXT, h->ctx_id));
    }
    return err;
}

/**
 * CTX_DESTROY, which drops a stream the context was sending in parts and
 * answers each fenced command still held on the context's rings: the
 * renderer frees the context's fences with it without retiring them, and
 * can make none for a command whose fence it refused once the context is
 * gone.
 */
static int cmd_ctx_destroy(struct vd *d, const struct vd_hdr *h)
{
    struct vgs_obj *o = vgs_table_find(&d->t, VGS_OBJ_CONTEXT, h->ctx_id);
    uint32_t i;

    if (o == 0) {
        return VD_RESP_ERR_CONTEXT_ID;
    }
    if (d->join_open && d->join_ctx == h->ctx_id) {
        join_drop(d);
    }
    d->ops->ctx_destroy(d->cookie, h->ctx_id);
    vgs_table_del(&d->t, o);
    pthread_mutex_lock(&d->lock);
    for (i = 0; i < VD_PENDING && d->npend != 0; i++) {
        struct vd_pending *p = &d->pend[i];

        if (p->used && p->per_ctx && p->ctx == h->ctx_id) {
            release(d, p);
        }
    }
    pthread_mutex_unlock(&d->lock);
    return 0;
}

/** CTX_ATTACH_RESOURCE and CTX_DETACH_RESOURCE. */
static int cmd_ctx_resource(struct vd *d, const struct vd_hdr *h, const uint8_t *body)
{
    struct c_resource c;

    memcpy(&c, body, sizeof(c));
    if (!ctx_live(d, h->ctx_id)) {
        return VD_RESP_ERR_CONTEXT_ID;
    }
    if (find_res(d, c.resource_id, 0) == 0) {
        return VD_RESP_ERR_RESOURCE_ID;
    }
    d->ops->ctx_resource(d->cookie, h->ctx_id, c.resource_id, h->type == CMD_CTX_ATTACH_RESOURCE);
    return 0;
}

/**
 * VGS_CMD_PART: the next bytes of the stream a SUBMIT_3D takes whole, under
 * VGS_TF_PARTS (vgpushm.h), copied after those before it. A part at offset
 * 0 begins a stream, dropping one not submitted, and allocates the whole
 * stream; a later part must follow the one before it. A part refused
 * breaks the stream: the rest are refused alike, and so is its SUBMIT_3D.
 * Returns 0 or the refusal: ERR_OUT_OF_MEMORY when the VM has no memory for
 * the stream, ERR_PARAMETER for a part out of order or no stream begun,
 * ERR_CONTEXT_ID for a context not live, whose part allocates nothing.
 */
static int cmd_part(struct vd *d, const struct vd_hdr *h, const uint8_t *body, uint32_t blen)
{
    struct vgs_part p;
    uint64_t n = blen - sizeof(p);

    if ((d->drv.transport_features & VGS_TF_PARTS) == 0) {
        return VD_RESP_ERR_UNSPEC;
    }
    if (!ctx_live(d, h->ctx_id)) {
        return VD_RESP_ERR_CONTEXT_ID;
    }
    memcpy(&p, body, sizeof(p));
    if (p.offset == 0) {
        join_drop(d);
        d->join_open = 1;
        d->join_total = p.total;
        d->join_ctx = h->ctx_id;
        if (p.total == 0 || p.total > UINT32_MAX) {
            d->join_err = VD_RESP_ERR_PARAMETER;
        } else if ((d->join = malloc((size_t)p.total)) == 0) {
            fprintf(stderr, "backend: part: refused: %llu bytes: no memory for the stream\n",
                    (unsigned long long)p.total);
            d->join_err = VD_RESP_ERR_OUT_OF_MEMORY;
        }
    }
    if (!d->join_open) {
        return VD_RESP_ERR_PARAMETER;
    }
    if (d->join_err != 0) {
        return (int)d->join_err;
    }
    if (p.offset != d->join_len || p.total != d->join_total || h->ctx_id != d->join_ctx ||
        n > p.total - p.offset) {
        d->join_err = VD_RESP_ERR_PARAMETER;
        return VD_RESP_ERR_PARAMETER;
    }
    memcpy(d->join + d->join_len, body + sizeof(p), (size_t)n);
    d->join_len += n;
    return 0;
}

/**
 * SUBMIT_3D with VGS_FLAG_JOINED: the stream its parts carried, whole, in
 * its context, and nothing after its body; the stream is dropped either
 * way. Returns the renderer's answer, or the refusal of a broken stream,
 * ERR_PARAMETER for one never begun, short or of another size or context.
 */
static int submit_joined(struct vd *d, const struct vd_hdr *h, const struct c_submit *c,
                         uint32_t blen)
{
    int err;

    if ((d->drv.transport_features & VGS_TF_PARTS) == 0 || !d->join_open) {
        err = VD_RESP_ERR_PARAMETER;
    } else if (d->join_err != 0) {
        err = (int)d->join_err;
    } else if (blen != sizeof(*c) || c->size % 4 != 0 || c->size != d->join_total ||
               d->join_len != d->join_total || h->ctx_id != d->join_ctx) {
        err = VD_RESP_ERR_PARAMETER;
    } else {
        err = d->ops->submit(d->cookie, h->ctx_id, d->join, c->size);
    }
    join_drop(d);
    return err;
}

/** SUBMIT_3D: whole dwords that follow the body, in the private copy, or
 * the stream joined from parts. */
static int cmd_submit(struct vd *d, const struct vd_hdr *h, uint8_t *body, uint32_t blen)
{
    struct c_submit c;

    memcpy(&c, body, sizeof(c));
    if (!ctx_live(d, h->ctx_id)) {
        if (h->flags & VGS_FLAG_JOINED) {
            join_drop(d);
        }
        return VD_RESP_ERR_CONTEXT_ID;
    }
    if (h->flags & VGS_FLAG_JOINED) {
        return submit_joined(d, h, &c, blen);
    }
    if (c.size > blen - sizeof(c) || c.size % 4 != 0) {
        return VD_RESP_ERR_PARAMETER;
    }
    return d->ops->submit(d->cookie, h->ctx_id, body + sizeof(c), c.size);
}

/** RESOURCE_CREATE_BLOB: guest memory from entries, or the context's. */
static int cmd_create_blob(struct vd *d, const struct vd_hdr *h, const uint8_t *body, uint32_t blen)
{
    struct c_blob c;
    struct vd_blob a;
    struct vgs_obj *o;
    struct iovec *iov = 0;
    int err;

    memcpy(&c, body, sizeof(c));
    if (c.blob_mem != BLOB_MEM_GUEST && c.blob_mem != BLOB_MEM_HOST3D) {
        return VD_RESP_ERR_PARAMETER;
    }
    if (c.blob_mem == BLOB_MEM_HOST3D && !ctx_live(d, h->ctx_id)) {
        return VD_RESP_ERR_CONTEXT_ID;
    }
    /* Host memory has no entries; guest memory has at least one. */
    if ((c.blob_mem == BLOB_MEM_HOST3D) != (c.nr_entries == 0)) {
        return VD_RESP_ERR_PARAMETER;
    }
    if (c.nr_entries > (blen - sizeof(c)) / sizeof(struct c_mem_entry) || c.size == 0) {
        return VD_RESP_ERR_PARAMETER;
    }
    if ((err = res_add(d, c.resource_id)) != 0) {
        return err;
    }
    find_res(d, c.resource_id, &o);
    if (c.nr_entries != 0 && (err = backing(d, o, body + sizeof(c), c.nr_entries, &iov)) != 0) {
        res_drop(d, c.resource_id);
        return err;
    }
    a.id = c.resource_id;
    a.ctx = h->ctx_id;
    a.mem = c.blob_mem;
    a.flags = c.blob_flags;
    a.blob_id = c.blob_id;
    a.size = c.size;
    if ((err = d->ops->blob(d->cookie, &a, iov, c.nr_entries)) != 0) {
        free(iov);
        res_drop(d, c.resource_id);
        return err;
    }
    d->res[o - d->obj].iov = iov;
    d->res[o - d->obj].niov = c.nr_entries;
    d->res[o - d->obj].blob = 1;
    d->res[o - d->obj].mem = c.blob_mem;
    d->res[o - d->obj].flags = c.blob_flags;
    d->res[o - d->obj].size = c.size;
    return 0;
}

/** Check that the renderer's runs of a page list lie within the `left`
 * pages asked for; 0 or VD_RESP_ERR_UNSPEC. */
static int runs_fit(const struct vd_pages *q, uint64_t left)
{
    uint32_t i;

    if (q->count == 0 || q->count > q->max) {
        return VD_RESP_ERR_UNSPEC;
    }
    for (i = 0; i < q->count; i++) {
        if (vgs_check_run(&q->runs[i], left) != VGS_OK) {
            return VD_RESP_ERR_UNSPEC;
        }
        left -= q->runs[i].pages;
    }
    return 0;
}

/** The bytes of a refused page list's answer, its bare header, with the
 * refusal in *type. */
static uint32_t refuse(uint32_t *type, uint32_t err)
{
    *type = err;
    return sizeof(struct vd_hdr);
}

/**
 * Answer a page list of resource `id` from p->first of p->total pages,
 * through the renderer's `op`, `pages` or `back`, which pins the memory at
 * the first ask: as many runs as the chain's writable bytes hold after the
 * header and the `struct vgs_pages`, written to d->resp. A refusal lets go of
 * the pin, which the renderer may have made before the list failed, since
 * the driver sends no UNMAP_BLOB for a map it was refused, and a backing
 * refused is no backing. Returns the answer's length; *type is
 * RESP_OK_MAP_INFO or the error.
 */
static uint32_t page_answer(struct vd *d, struct vd_res *r, uint32_t id, struct vgs_pages *p,
                            int (*op)(void *c, struct vd_pages *q), uint32_t *type)
{
    uint32_t room = d->room < VD_RESP_MAX ? d->room : VD_RESP_MAX;
    struct vd_pages q;
    int err;

    if (p->first >= p->total || room < VGS_PAGES_OFF + sizeof(struct vgs_run)) {
        return refuse(type, VD_RESP_ERR_PARAMETER);
    }
    memset(&q, 0, sizeof(q));
    q.id = id;
    q.epoch = d->drv.epoch;
    q.size = r->size;
    q.first = p->first;
    q.max = (room - VGS_PAGES_OFF) / sizeof(struct vgs_run);
    if ((q.runs = calloc(q.max, sizeof(*q.runs))) == 0) {
        return refuse(type, VD_RESP_ERR_OUT_OF_MEMORY);
    }
    err = op(d->cookie, &q);
    if (err == 0) {
        r->pinned = 1;
        err = runs_fit(&q, p->total - p->first);
    }
    if (err != 0 && d->ops->unpin != 0) {
        d->ops->unpin(d->cookie, id);
        r->pinned = 0;
    }
    if (err == 0) {
        p->map_info = q.map_info;
        p->count = q.count;
        memcpy(d->resp + sizeof(struct vd_hdr), p, sizeof(*p));
        memcpy(d->resp + VGS_PAGES_OFF, q.runs, (size_t)q.count * sizeof(*q.runs));
    }
    free(q.runs);
    *type = err == 0 ? VD_RESP_OK_MAP_INFO : (uint32_t)err;
    return err == 0 ? VGS_PAGES_OFF + q.count * (uint32_t)sizeof(struct vgs_run)
                    : (uint32_t)sizeof(struct vd_hdr);
}

/**
 * RESOURCE_MAP_BLOB under VGS_TF_PAGES: the page list of a mappable HOST3D
 * blob from the page `offset` names (`page_answer`). Returns the answer's
 * length; *type is RESP_OK_MAP_INFO or the error.
 */
static uint32_t cmd_map_blob(struct vd *d, const uint8_t *body, uint32_t *type)
{
    struct vgs_pages p;
    struct c_map_blob c;
    struct vd_res *r;
    uint32_t len;

    memcpy(&c, body, sizeof(c));
    if ((d->drv.transport_features & VGS_TF_PAGES) == 0 || d->ops->pages == 0) {
        return refuse(type, VD_RESP_ERR_UNSPEC);
    }
    if ((r = find_res(d, c.resource_id, 0)) == 0) {
        return refuse(type, VD_RESP_ERR_RESOURCE_ID);
    }
    if (!r->blob || r->mem != BLOB_MEM_HOST3D || (r->flags & BLOB_FLAG_USE_MAPPABLE) == 0) {
        return refuse(type, VD_RESP_ERR_PARAMETER);
    }
    memset(&p, 0, sizeof(p));
    p.total = (r->size + VGS_PAGE - 1) / VGS_PAGE;
    p.first = c.offset;
    len = page_answer(d, r, c.resource_id, &p, d->ops->pages, type);
    d->n.maps += *type == VD_RESP_OK_MAP_INFO;
    return len;
}

/**
 * VGS_CMD_BACKING under VGS_TF_BACKING: back a resource of CREATE_2D or
 * CREATE_3D that has no backing with `size` bytes of the VM's memory at the
 * first ask, `first` 0, and answer the backing's page list from `first`
 * (`page_answer`). A later ask names the same size. The backing goes at
 * DETACH_BACKING, UNREF, a refused ask or the epoch's end. Returns the
 * answer's length; *type is RESP_OK_MAP_INFO or the error.
 */
static uint32_t cmd_backing(struct vd *d, const uint8_t *body, uint32_t *type)
{
    struct vgs_backing c;
    struct vgs_pages p;
    struct vd_res *r;
    uint32_t len;

    memcpy(&c, body, sizeof(c));
    if ((d->drv.transport_features & VGS_TF_BACKING) == 0 || d->ops->back == 0) {
        return refuse(type, VD_RESP_ERR_UNSPEC);
    }
    if ((r = find_res(d, c.resource_id, 0)) == 0) {
        return refuse(type, VD_RESP_ERR_RESOURCE_ID);
    }
    if (r->blob || r->iov != 0) {
        return refuse(type, VD_RESP_ERR_UNSPEC);
    }
    if (c.size == 0 || c.size % VGS_PAGE != 0 || c.size > VD_BACKING_MAX ||
        (r->backed ? c.size != r->size : c.first != 0)) {
        return refuse(type, VD_RESP_ERR_PARAMETER);
    }
    memset(&p, 0, sizeof(p));
    p.total = c.size / VGS_PAGE;
    p.first = c.first;
    r->size = c.size;
    len = page_answer(d, r, c.resource_id, &p, d->ops->back, type);
    r->backed = *type == VD_RESP_OK_MAP_INFO;
    if (!r->backed) {
        r->size = 0;
    }
    d->n.backs += r->backed;
    return len;
}

/** RESOURCE_UNMAP_BLOB under VGS_TF_PAGES: let go of the blob's pin. */
static int cmd_unmap_blob(struct vd *d, const uint8_t *body)
{
    struct c_resource c;
    struct vd_res *r;

    memcpy(&c, body, sizeof(c));
    if ((d->drv.transport_features & VGS_TF_PAGES) == 0 || d->ops->unpin == 0) {
        return VD_RESP_ERR_UNSPEC;
    }
    if ((r = find_res(d, c.resource_id, 0)) == 0) {
        return VD_RESP_ERR_RESOURCE_ID;
    }
    if (!r->blob) {
        return VD_RESP_ERR_PARAMETER;
    }
    if (r->pinned) {
        d->ops->unpin(d->cookie, c.resource_id);
        r->pinned = 0;
        d->n.unmaps++;
    }
    return 0;
}

/* Each command's body size after the header, or -1 for one not served. */
static long body_size(uint32_t type)
{
    switch (type) {
    case CMD_GET_DISPLAY_INFO:
    case CMD_CTX_DESTROY:
        return 0;
    case CMD_RESOURCE_CREATE_2D:
        return sizeof(struct c_create_2d);
    case CMD_RESOURCE_UNREF:
    case CMD_RESOURCE_DETACH_BACKING:
    case CMD_CTX_ATTACH_RESOURCE:
    case CMD_CTX_DETACH_RESOURCE:
    case CMD_RESOURCE_UNMAP_BLOB:
        return sizeof(struct c_resource);
    case CMD_SET_SCANOUT:
        return sizeof(struct c_set_scanout);
    case CMD_RESOURCE_FLUSH:
        return sizeof(struct c_flush);
    case CMD_TRANSFER_TO_HOST_2D:
        return sizeof(struct c_transfer_2d);
    case CMD_RESOURCE_ATTACH_BACKING:
        return sizeof(struct c_attach);
    case CMD_GET_CAPSET_INFO:
        return sizeof(struct c_capset_info);
    case CMD_GET_CAPSET:
        return sizeof(struct c_capset);
    case CMD_GET_EDID:
        return sizeof(struct c_edid);
    case CMD_CTX_CREATE:
        return sizeof(struct c_ctx_create);
    case CMD_RESOURCE_CREATE_3D:
        return sizeof(struct c_create_3d);
    case CMD_TRANSFER_TO_HOST_3D:
    case CMD_TRANSFER_FROM_HOST_3D:
        return sizeof(struct c_transfer_3d);
    case CMD_SUBMIT_3D:
        return sizeof(struct c_submit);
    case CMD_RESOURCE_CREATE_BLOB:
        return sizeof(struct c_blob);
    case CMD_RESOURCE_MAP_BLOB:
        return sizeof(struct c_map_blob);
    case VGS_CMD_BACKING:
        return sizeof(struct vgs_backing);
    case VGS_CMD_PART:
        return sizeof(struct vgs_part);
    default:
        return -1;
    }
}

/** Run a command whose answer is a bare header; return its response type. */
static uint32_t run_nodata(struct vd *d, const struct vd_hdr *h, uint8_t *body, uint32_t blen)
{
    int err;

    switch (h->type) {
    case CMD_RESOURCE_CREATE_2D:
        err = cmd_create_2d(d, body);
        break;
    case CMD_RESOURCE_CREATE_3D:
        err = cmd_create_3d(d, body);
        break;
    case CMD_RESOURCE_UNREF:
        err = cmd_unref(d, body);
        break;
    case CMD_RESOURCE_ATTACH_BACKING:
        err = cmd_attach(d, body, blen);
        break;
    case CMD_RESOURCE_DETACH_BACKING:
        err = cmd_detach(d, body);
        break;
    case CMD_TRANSFER_TO_HOST_2D:
        err = cmd_transfer_2d(d, body);
        break;
    case CMD_TRANSFER_TO_HOST_3D:
    case CMD_TRANSFER_FROM_HOST_3D:
        err = cmd_transfer_3d(d, h, body);
        break;
    case CMD_SET_SCANOUT:
        err = cmd_set_scanout(d, body);
        break;
    case CMD_RESOURCE_FLUSH:
        err = cmd_flush(d, body);
        break;
    case CMD_CTX_CREATE:
        err = cmd_ctx_create(d, h, body);
        break;
    case CMD_CTX_DESTROY:
        err = cmd_ctx_destroy(d, h);
        break;
    case CMD_CTX_ATTACH_RESOURCE:
    case CMD_CTX_DETACH_RESOURCE:
        err = cmd_ctx_resource(d, h, body);
        break;
    case CMD_SUBMIT_3D:
        err = cmd_submit(d, h, body, blen);
        break;
    case CMD_RESOURCE_CREATE_BLOB:
        err = cmd_create_blob(d, h, body, blen);
        break;
    case CMD_RESOURCE_UNMAP_BLOB:
        err = cmd_unmap_blob(d, body);
        break;
    case VGS_CMD_PART:
        err = cmd_part(d, h, body, blen);
        break;
    default:
        err = VD_RESP_ERR_UNSPEC;
        break;
    }
    return err == 0 ? VD_RESP_OK_NODATA : (uint32_t)err;
}

/**
 * Execute the command of rlen bytes in d->cmd and write its response to
 * d->resp. Returns the response's length; *type is its type.
 */
static uint32_t execute(struct vd *d, uint32_t rlen, uint32_t *type)
{
    uint8_t *body = d->cmd + sizeof(struct vd_hdr);
    uint32_t blen, len = 0;
    struct vd_hdr h;
    long need;

    memset(d->resp, 0, sizeof(struct vd_hdr) + VD_SCANOUTS * 24);
    memcpy(&h, d->cmd, sizeof(h));
    blen = rlen - sizeof(h);
    need = body_size(h.type);
    if (need < 0 || blen < (uint32_t)need) {
        *type = need < 0 ? VD_RESP_ERR_UNSPEC : VD_RESP_ERR_PARAMETER;
        return sizeof(struct vd_hdr);
    }
    switch (h.type) {
    case CMD_GET_DISPLAY_INFO:
        len = cmd_display_info(d, d->resp);
        *type = VD_RESP_OK_DISPLAY_INFO;
        return len;
    case CMD_GET_CAPSET_INFO:
        len = cmd_capset_info(d, body, d->resp);
        *type = VD_RESP_OK_CAPSET_INFO;
        break;
    case CMD_GET_CAPSET:
        len = cmd_capset(d, body, d->resp);
        *type = VD_RESP_OK_CAPSET;
        break;
    case CMD_GET_EDID:
        len = cmd_edid(d, body, d->resp);
        *type = VD_RESP_OK_EDID;
        break;
    case CMD_RESOURCE_MAP_BLOB:
        return cmd_map_blob(d, body, type);
    case VGS_CMD_BACKING:
        return cmd_backing(d, body, type);
    default:
        *type = run_nodata(d, &h, body, blen);
        return sizeof(struct vd_hdr);
    }
    if (len == 0) {
        *type = VD_RESP_ERR_PARAMETER;
        return sizeof(struct vd_hdr);
    }
    return len;
}

/** Write len bytes of resp into a chain's writable segments; return bytes put. */
static uint32_t put_response(struct vd *d, const struct vgs_chain *c, const uint8_t *resp,
                             uint32_t len)
{
    uint32_t i, put = 0;

    for (i = c->nread; i < c->n && put < len; i++) {
        uint32_t k = c->seg[i].len < len - put ? c->seg[i].len : len - put;

        region_put(d, c->seg[i].addr, resp + put, k);
        put += k;
    }
    return put;
}

/** Publish a chain's completion in its queue's used ring; d->lock held. */
static void complete(struct vd *d, uint32_t qi, uint32_t head, uint32_t put)
{
    vgs_used_push(d->base, &d->drv.queue[qi], &d->used[qi], head, put);
    d->kick = 1;
    d->n.completed++;
}

static struct vd_profile_row *profile_row(struct vd *d);

/** Count a held command's time from its fence call to its completion in its
 * profile row, under -T. d->lock held. */
static void profile_held(struct vd *d, const struct vd_pending *p)
{
    if (d->profile && p->held_ns) {
        d->profile->row[p->row].held++;
        d->profile->row[p->row].fence_ns += vd_profile_now() - p->held_ns;
    }
}

/**
 * Hold a fenced command until its fence retires. Returns its slot, or 0
 * when every pending slot is taken, which the caller answers at once.
 * d->lock held.
 */
static struct vd_pending *hold(struct vd *d, uint32_t qi, uint32_t head, const struct vgs_chain *c,
                               const struct vd_hdr *h, const uint8_t *resp)
{
    uint32_t i;

    for (i = 0; i < VD_PENDING; i++) {
        struct vd_pending *p = &d->pend[i];

        if (p->used) {
            continue;
        }
        p->used = 1;
        p->queue = qi;
        p->head = head;
        p->per_ctx = (h->flags & VD_FLAG_INFO_RING_IDX) != 0;
        p->ctx = p->per_ctx ? h->ctx_id : 0;
        p->ring = p->per_ctx ? h->ring_idx : 0;
        p->fence_id = h->fence_id;
        p->unfenced = 0;
        p->rlen = sizeof(struct vd_hdr);
        p->held_ns = 0;
        memcpy(p->resp, resp, sizeof(struct vd_hdr));
        p->chain = *c;
        d->npend++;
        return p;
    }
    return 0;
}

/** Whether fence a comes after fence b on pending command p's timeline:
 * a context ring's 64-bit ids, or the global timeline's 32 bits, which the
 * renderer keeps, compared across their wrap as vd_fence_retired does. */
static int fence_after(const struct vd_pending *p, uint64_t a, uint64_t b)
{
    return p->per_ctx ? a > b : (int32_t)((uint32_t)a - (uint32_t)b) > 0;
}

/**
 * Ask the renderer again for the fence of each held command it refused
 * one, when that command is still the newest held on its timeline. A
 * newer held command's fence covers the work before it and retires it
 * with its own (vd_fence_retired), so only the newest asks; asking for an
 * older fence after a newer one would break the order of the timeline's
 * fences, which the renderer may report as one. CTX_DESTROY answers a
 * context's held commands, so none of a dead context is left to ask. This
 * runs on the serving thread, the only one that holds commands, so the
 * newest stays the newest while the fence call runs without d->lock, which
 * the renderer's threads take to retire fences; a retire may complete the
 * command meanwhile, which then has nothing left to mark.
 */
static void fence_retry(struct vd *d)
{
    uint32_t i, j, queue, head, ctx, ring, per_ctx;
    uint64_t id;
    int err;

    pthread_mutex_lock(&d->lock);
    for (i = 0; i < VD_PENDING && d->nunfenced != 0; i++) {
        struct vd_pending *p = &d->pend[i];
        int newest = 1;

        if (!p->used || !p->unfenced) {
            continue;
        }
        for (j = 0; j < VD_PENDING && newest; j++) {
            const struct vd_pending *q = &d->pend[j];

            newest = !q->used || q == p || q->per_ctx != p->per_ctx || q->ctx != p->ctx ||
                     q->ring != p->ring || !fence_after(p, q->fence_id, p->fence_id);
        }
        if (!newest) {
            continue;
        }
        queue = p->queue;
        head = p->head;
        ctx = p->ctx;
        ring = p->ring;
        per_ctx = p->per_ctx;
        id = p->fence_id;
        pthread_mutex_unlock(&d->lock);
        err = d->ops->fence(d->cookie, ctx, (int)per_ctx, ring, id);
        pthread_mutex_lock(&d->lock);
        if (err != 0 || !p->used || !p->unfenced || p->queue != queue || p->head != head) {
            continue;
        }
        p->unfenced = 0;
        d->nunfenced--;
        if (d->verbose) {
            fprintf(stderr, "backend: fence: taken again: id %llu of context %u, ring %u\n",
                    (unsigned long long)id, (unsigned)ctx, (unsigned)ring);
        }
    }
    pthread_mutex_unlock(&d->lock);
}

/**
 * Answer a command: at once, or, for a fenced command answered with a bare
 * header, once the renderer retires its fence on the timeline the header
 * names, which it may do on a thread of its own before its fence call
 * returns. A refusal waits so too: the driver retires every earlier fence
 * of the timeline with the answer, as Linux's virtio_gpu_fence_event_process
 * does, and the renderer's work behind them may still be pending. A fence
 * the renderer refuses leaves its command held, with no fence of its own,
 * until a later fence on its timeline retires or fence_retry has the
 * renderer take it, for the same reason. An answer given at once with data
 * or an error rings the driver at once, since the driver waits for those;
 * a bare OK waits for the end of the pass.
 */
static void answer(struct vd *d, uint32_t qi, uint32_t head, const struct vgs_chain *c,
                   uint32_t type, uint32_t len)
{
    struct vd_pending *p = 0;
    struct vd_hdr h, o;
    int per_ctx, err;

    /* resp is a byte array in struct vd, so its header goes by memcpy. */
    memcpy(&h, d->cmd, sizeof(h));
    memcpy(&o, d->resp, sizeof(o));
    o.type = type;
    if (type >= VD_RESP_ERR_UNSPEC) {
        d->n.errors++;
        if (d->verbose) {
            fprintf(stderr, "backend: command 0x%x: response 0x%x\n", (unsigned)h.type,
                    (unsigned)type);
        }
    }
    if (h.flags & VD_FLAG_FENCE) {
        o.flags = h.flags & (VD_FLAG_FENCE | VD_FLAG_INFO_RING_IDX);
        o.fence_id = h.fence_id;
        o.ctx_id = h.ctx_id;
        o.ring_idx = h.ring_idx;
    }
    memcpy(d->resp, &o, sizeof(o));
    if (h.flags & VD_FLAG_FENCE) {
        per_ctx = (h.flags & VD_FLAG_INFO_RING_IDX) != 0;
        if ((type == VD_RESP_OK_NODATA || type >= VD_RESP_ERR_UNSPEC) &&
            (!per_ctx || ctx_live(d, h.ctx_id))) {
            pthread_mutex_lock(&d->lock);
            p = hold(d, qi, head, c, &h, d->resp);
            if (p && d->profile) {
                p->row = (uint32_t)(profile_row(d) - d->profile->row);
                p->held_ns = vd_profile_now();
            }
            pthread_mutex_unlock(&d->lock);
        }
        if (p) {
            uint32_t ctx = p->ctx, ring = p->ring;
            uint64_t id = p->fence_id;

            d->n.fenced++;
            err = d->ops->fence(d->cookie, ctx, per_ctx, ring, id);
            if (err == 0) {
                return;
            }
            fprintf(stderr,
                    "backend: fence: refused: id %llu of context %u, ring %u: error %d: "
                    "held until a later fence retires or the renderer takes it\n",
                    (unsigned long long)id, (unsigned)ctx, (unsigned)ring, err);
            /* Only the serving thread holds, so a used slot is still this
               command's; a later retire may have completed it meanwhile. */
            pthread_mutex_lock(&d->lock);
            if (p->used && p->queue == qi && p->head == head) {
                p->unfenced = 1;
                d->nunfenced++;
            }
            pthread_mutex_unlock(&d->lock);
            return;
        }
    }
    pthread_mutex_lock(&d->lock);
    complete(d, qi, head, put_response(d, c, d->resp, len));
    pthread_mutex_unlock(&d->lock);
    if (type != VD_RESP_OK_NODATA) {
        vd_kick(d);
    }
}

/** Answer a held command with the response it keeps, count its time under
 * -T and free its slot. d->lock held. */
static void release(struct vd *d, struct vd_pending *p)
{
    profile_held(d, p);
    complete(d, p->queue, p->head, put_response(d, &p->chain, p->resp, p->rlen));
    if (p->unfenced) {
        p->unfenced = 0;
        d->nunfenced--;
    }
    p->used = 0;
    d->npend--;
}

/**
 * Complete every fenced command on a timeline whose fence is id or older:
 * the global one, whose ids the renderer keeps in 32 bits, or a context's
 * ring. The renderer calls this through its fence callbacks, from any
 * thread; it takes d->lock, so the caller holds none of the device's.
 */
void vd_fence_retired(struct vd *d, uint32_t ctx, int per_ctx, uint32_t ring, uint64_t id)
{
    uint32_t i;

    pthread_mutex_lock(&d->lock);
    for (i = 0; i < VD_PENDING && d->npend != 0; i++) {
        struct vd_pending *p = &d->pend[i];
        int done;

        if (!p->used || p->per_ctx != (uint32_t)(per_ctx != 0)) {
            continue;
        }
        if (per_ctx) {
            done = p->ctx == ctx && p->ring == ring && p->fence_id <= id;
        } else {
            done = (int32_t)((uint32_t)p->fence_id - (uint32_t)id) <= 0;
        }
        if (!done) {
            continue;
        }
        release(d, p);
    }
    pthread_mutex_unlock(&d->lock);
}

/** Ring the driver's queue vector once for the completions since the last,
 * from any thread, under d->lock. */
void vd_kick(struct vd *d)
{
    uint32_t qi;

    pthread_mutex_lock(&d->lock);
    if (d->kick) {
        d->kick = 0;
        for (qi = 0; qi < VGS_QUEUES; qi++) {
            const struct vgs_queue *q = &d->drv.queue[qi];

            if (q->size != 0 &&
                (*(volatile uint16_t *)(d->base + q->avail) & VGS_AVAIL_F_NO_INTERRUPT) == 0) {
                notify(d, VGS_VEC_QUEUE, 1);
                break;
            }
        }
    }
    pthread_mutex_unlock(&d->lock);
}

/**
 * Return how many fenced commands wait for their fences, copy the one of
 * the oldest fence to *oldest when there is one and give in *unfenced how
 * many of them have no renderer fence, each pointer when not 0, under
 * d->lock.
 */
uint32_t vd_pending(struct vd *d, struct vd_pending *oldest, uint32_t *unfenced)
{
    const struct vd_pending *old = 0;
    uint32_t i, n;

    pthread_mutex_lock(&d->lock);
    n = d->npend;
    if (unfenced) {
        *unfenced = d->nunfenced;
    }
    for (i = 0; i < VD_PENDING && n; i++) {
        const struct vd_pending *p = &d->pend[i];

        if (p->used && (!old || p->fence_id < old->fence_id)) {
            old = p;
        }
    }
    if (old && oldest) {
        *oldest = *old;
    }
    pthread_mutex_unlock(&d->lock);
    return n;
}

/** Read the VM's monotonic clock in nanoseconds. */
uint64_t vd_profile_now(void)
{
    struct timespec t;

    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000 + (uint64_t)t.tv_nsec;
}

/**
 * Add 1 to the device block's heartbeat, under VGS_TF_BEAT offered
 * (vgpushm.h). The backend calls it every VGS_BEAT_MS from a thread of its
 * own, so that the count stops only when the whole process stops or dies,
 * however long the serving thread spends in one command. That thread is
 * the word's only writer, by one atomic store, since an epoch's offer
 * leaves the word out of what it writes.
 */
void vd_beat(struct vd *d)
{
    if ((d->offer.transport_features & VGS_TF_BEAT) != 0) {
        __atomic_store_n(&VGS_DEVICE(d->base)->heartbeat, ++d->beat, __ATOMIC_RELAXED);
    }
}

/** Return the profiling row for the type of the control command in d->cmd. */
static struct vd_profile_row *profile_row(struct vd *d)
{
    uint32_t type, i = VD_PROFILE_OTHER;

    memcpy(&type, d->cmd, sizeof(type));
    if (type >= 0x100 && type < 0x110) {
        i = type - 0x100;
    } else if (type >= 0x200 && type < 0x210) {
        i = 16 + type - 0x200;
    } else if (type >= 0x300 && type < 0x310) {
        i = 32 + type - 0x300;
    } else if (type == VGS_CMD_BACKING) {
        i = 48;
    }
    d->profile->row[i].type = i == VD_PROFILE_OTHER ? 0 : type;
    return &d->profile->row[i];
}

/**
 * Grow the command buffer to hold a chain's `need` readable bytes, which
 * vgs_chain_read has held to the offer's max_cmd. Returns 0, or -1 when
 * the VM has no memory for it, which leaves the buffer as it was.
 */
static int cmd_grow(struct vd *d, uint32_t need)
{
    uint8_t *p = realloc(d->cmd, need);

    if (p == 0) {
        return -1;
    }
    d->cmd = p;
    d->cmd_cap = need;
    return 0;
}

/**
 * Read, execute and answer the chain at `head` of queue qi, counting it in
 * the profile, with its wait since the look-ahead read it from d->read_ns,
 * which the call clears. Returns VGS_OK, or the transport fault of a chain
 * that breaks the transport's rules.
 */
static int serve_chain(struct vd *d, uint32_t qi, uint32_t head)
{
    const struct vgs_queue *q = &d->drv.queue[qi];
    struct vd_profile_row *row = NULL;
    uint64_t start = 0, copied = 0, executed = 0, read = d->read_ns;
    uint32_t i, off = 0, type, len, want;
    struct vgs_chain c;
    int err, short_of_memory;

    d->read_ns = 0;
    if ((err = vgs_chain_read(d->base, &d->l, q, &d->me.limits, head, &c)) != VGS_OK) {
        return err;
    }
    if (d->profile) {
        start = vd_profile_now();
    }
    /* With no room for the whole chain, its header alone is read. */
    short_of_memory = c.rbytes > d->cmd_cap && cmd_grow(d, c.rbytes) != 0;
    want = short_of_memory ? sizeof(struct vd_hdr) : c.rbytes;
    for (i = 0; i < c.nread && off < want; i++) {
        len = c.seg[i].len < want - off ? c.seg[i].len : want - off;
        region_get(d, c.seg[i].addr, d->cmd + off, len);
        off += len;
    }
    d->room = c.wbytes;
    d->n.commands++;
    if (qi != VGS_QUEUE_CONTROL) {
        /* The cursor queue: a cursor is not drawn; its chains return. */
        pthread_mutex_lock(&d->lock);
        complete(d, qi, head, 0);
        pthread_mutex_unlock(&d->lock);
        return VGS_OK;
    }
    if (off < sizeof(struct vd_hdr)) {
        struct vd_hdr e;

        memset(&e, 0, sizeof(e));
        e.type = VD_RESP_ERR_PARAMETER;
        memcpy(d->resp, &e, sizeof(e));
        pthread_mutex_lock(&d->lock);
        complete(d, qi, head, put_response(d, &c, d->resp, sizeof(struct vd_hdr)));
        pthread_mutex_unlock(&d->lock);
        d->n.errors++;
        return VGS_OK;
    }
    if (short_of_memory) {
        memcpy(&type, d->cmd, sizeof(type));
        fprintf(stderr, "backend: command 0x%x: refused: %u bytes: no memory for its copy\n",
                (unsigned)type, (unsigned)c.rbytes);
        if (type == VGS_CMD_PART) {
            /* The stream lacks this part: its SUBMIT_3D is refused. */
            join_drop(d);
            d->join_open = 1;
            d->join_err = VD_RESP_ERR_OUT_OF_MEMORY;
        }
        memset(d->resp, 0, sizeof(struct vd_hdr));
        answer(d, qi, head, &c, VD_RESP_ERR_OUT_OF_MEMORY, sizeof(struct vd_hdr));
        return VGS_OK;
    }
    if (d->profile) {
        copied = vd_profile_now();
        row = profile_row(d);
    }
    len = execute(d, off, &type);
    if (row) {
        executed = vd_profile_now();
    }
    answer(d, qi, head, &c, type, len);
    if (row) {
        row->count++;
        row->bytes += off;
        row->copy_ns += copied - start;
        row->execute_ns += executed - copied;
        row->answer_ns += vd_profile_now() - executed;
        if (read != 0 && start > read) {
            row->queue_ns += start - read;
        }
        if (executed - copied >= VD_PROFILE_SLOW_NS) {
            row->slow++;
            row->slow_ns += executed - copied;
        }
        if (executed - copied > row->max_ns) {
            row->max_ns = executed - copied;
        }
    }
    return VGS_OK;
}

/** Whether a command of `type` names a resource in its body's first word,
 * which the look-ahead reads. */
static int names_resource(uint32_t type)
{
    switch (type) {
    case CMD_RESOURCE_CREATE_2D:
    case CMD_RESOURCE_CREATE_3D:
    case CMD_RESOURCE_UNREF:
    case CMD_RESOURCE_ATTACH_BACKING:
    case CMD_RESOURCE_DETACH_BACKING:
    case CMD_RESOURCE_CREATE_BLOB:
    case CMD_RESOURCE_MAP_BLOB:
    case CMD_RESOURCE_UNMAP_BLOB:
    case CMD_CTX_ATTACH_RESOURCE:
    case CMD_CTX_DETACH_RESOURCE:
    case VGS_CMD_BACKING:
        return 1;
    default:
        return 0;
    }
}

/** Whether a control command of `type` adds a resource to the table. */
static int is_create(uint32_t type)
{
    return type == CMD_RESOURCE_CREATE_2D || type == CMD_RESOURCE_CREATE_3D ||
           type == CMD_RESOURCE_CREATE_BLOB;
}

/** Read the header and first body word of the control queue's chain at
 * `head` into `a`, which keeps the head; a chain that breaks the rules is
 * marked `bad`, for the in-order pass to judge. */
static void ahead_read(struct vd *d, uint32_t head, struct vd_ahead *a)
{
    const struct vgs_queue *q = &d->drv.queue[VGS_QUEUE_CONTROL];
    uint8_t buf[sizeof(struct vd_hdr) + 4];
    uint32_t i, off = 0, k;
    struct vd_hdr h;
    struct vgs_chain c;

    memset(a, 0, sizeof(*a));
    a->head = head;
    if (vgs_chain_read(d->base, &d->l, q, &d->me.limits, head, &c) != VGS_OK) {
        a->bad = 1;
        return;
    }
    for (i = 0; i < c.nread && off < sizeof(buf); i++) {
        k = c.seg[i].len < sizeof(buf) - off ? c.seg[i].len : (uint32_t)sizeof(buf) - off;
        region_get(d, c.seg[i].addr, buf + off, k);
        off += k;
    }
    if (off < sizeof(h)) {
        return;
    }
    memcpy(&h, buf, sizeof(h));
    a->type = h.type;
    a->flags = h.flags;
    if (off == sizeof(buf) && names_resource(h.type)) {
        memcpy(&a->id, buf + sizeof(h), sizeof(a->id));
    }
}

/** Count what the look-ahead did with a backing ask, under -T. */
static void ahead_count(struct vd *d, unsigned what)
{
    if (d->profile) {
        d->profile->ahead[what]++;
    }
}

/**
 * Serve the backing ask at avail place `pos` ahead of its turn, with the
 * create it follows, when that cannot change what an earlier command sees:
 * the ask is the first, unfenced, of a resource whose id the table does
 * not hold, and the only earlier entry not yet served that names the id is
 * its RESOURCE_CREATE_2D or _3D, unfenced. Those earlier entries, made
 * before the resource, cannot name it, so the create and the ask run
 * before them and the ask's answer goes out at once, as Linux's guest has
 * its pages at the create; a fenced create's answer would signal every
 * earlier fence of its timeline. An id still in the table, as through an
 * UNREF not yet served, keeps its order. The create goes ahead only while
 * the table has room for it after every earlier create not yet served, and
 * without the UNREFs queued before it, so that neither it nor an earlier
 * create fails where both would succeed in order; and nothing goes ahead
 * of a chain that breaks the rules, which faults the queue. Returns VGS_OK
 * or a transport fault.
 */
static int ahead_backing(struct vd *d, uint16_t pos)
{
    const struct vgs_queue *q = &d->drv.queue[VGS_QUEUE_CONTROL];
    uint32_t mask = q->size - 1, id = d->ahead[pos & mask].id;
    uint32_t used = d->t.count[VGS_OBJ_RESOURCE];
    uint16_t at, create = 0;
    int found = 0, err;

    if (id == 0 || (d->ahead[pos & mask].flags & VD_FLAG_FENCE) != 0 || find_res(d, id, 0)) {
        ahead_count(d, VD_AHEAD_KNOWN);
        return VGS_OK;
    }
    for (at = d->last[VGS_QUEUE_CONTROL]; at != pos; at++) {
        const struct vd_ahead *a = &d->ahead[at & mask];

        if (a->bad) {
            ahead_count(d, VD_AHEAD_BAD);
            return VGS_OK;
        }
        if (a->done) {
            continue;
        }
        if (a->id != id && is_create(a->type)) {
            used++;
        }
        if (a->id != id || !names_resource(a->type)) {
            continue;
        }
        if (found || (a->type != CMD_RESOURCE_CREATE_2D && a->type != CMD_RESOURCE_CREATE_3D) ||
            (a->flags & VD_FLAG_FENCE) != 0) {
            ahead_count(d, VD_AHEAD_NAMED);
            return VGS_OK;
        }
        found = 1;
        create = at;
    }
    if (!found || used >= d->t.limits.max_resources) {
        ahead_count(d, found ? VD_AHEAD_ROOM : VD_AHEAD_NAMED);
        return VGS_OK;
    }
    ahead_count(d, VD_AHEAD_SERVED);
    d->read_ns = d->ahead[create & mask].read_ns;
    if ((err = serve_chain(d, VGS_QUEUE_CONTROL, d->ahead[create & mask].head)) != VGS_OK) {
        return err;
    }
    d->ahead[create & mask].done = 1;
    d->read_ns = d->ahead[pos & mask].read_ns;
    if ((err = serve_chain(d, VGS_QUEUE_CONTROL, d->ahead[pos & mask].head)) != VGS_OK) {
        return err;
    }
    d->ahead[pos & mask].done = 1;
    return VGS_OK;
}

/**
 * Consume the control queue's avail entries up to `idx`, in order, as far
 * as a queue's length past the in-order pass, and serve each first backing
 * ask ahead of the entries before it where `ahead_backing` allows, so that
 * its answer does not wait for the submissions queued before it. An entry
 * is consumed, its head kept in `ahead`, before any later one is served,
 * so that the avail entries not consumed are those of chains the guest has
 * no answer for: a guest that reuses the descriptors and ring places of
 * the entries served ahead cannot pass the ring's length
 * (`vgs_avail_read`). Returns VGS_OK or a transport fault.
 */
static int ahead(struct vd *d, uint16_t idx)
{
    const struct vgs_queue *q = &d->drv.queue[VGS_QUEUE_CONTROL];
    uint16_t last = d->last[VGS_QUEUE_CONTROL];
    struct vd_ahead *a;
    int err;

    for (; d->seen != idx && (uint16_t)(d->seen - last) < q->size; d->seen++) {
        a = &d->ahead[d->seen & (q->size - 1)];
        ahead_read(d, vgs_avail_head(d->base, q, d->seen), a);
        a->read_ns = d->profile ? vd_profile_now() : 0;
        if (a->type == VGS_CMD_BACKING && d->seen == last) {
            ahead_count(d, VD_AHEAD_TURN);
        } else if (a->type == VGS_CMD_BACKING && (err = ahead_backing(d, d->seen)) != VGS_OK) {
            return err;
        }
    }
    return VGS_OK;
}

/**
 * Serve the control queue's chains consumed at the start (`ahead`), in
 * order but for those served ahead, with the backing asks that come
 * meanwhile looked ahead at after each command, and let the renderer poll
 * between two commands (`poll`). Returns VGS_OK, or the transport fault
 * that puts the device in NEEDS_RESET.
 */
static int serve_control(struct vd *d)
{
    const struct vgs_queue *q = &d->drv.queue[VGS_QUEUE_CONTROL];
    uint16_t idx, end, pos;
    struct vd_ahead *a;
    uint32_t head;
    int err, done;

    if ((err = vgs_avail_read(d->base, q, d->seen, &idx)) != VGS_OK ||
        (err = ahead(d, idx)) != VGS_OK) {
        return err;
    }
    for (end = d->seen; d->last[VGS_QUEUE_CONTROL] != end;) {
        pos = d->last[VGS_QUEUE_CONTROL]++;
        a = &d->ahead[pos & (q->size - 1)];
        done = a->done;
        head = a->head;
        d->read_ns = a->read_ns;
        memset(a, 0, sizeof(*a));
        if (done) {
            continue;
        }
        if ((err = serve_chain(d, VGS_QUEUE_CONTROL, head)) != VGS_OK) {
            return err;
        }
        if (d->ops->poll) {
            d->ops->poll(d->cookie);
        }
        /* Asks that came during the pass go before its remaining entries. */
        if ((err = vgs_avail_read(d->base, q, d->seen, &idx)) != VGS_OK ||
            (err = ahead(d, idx)) != VGS_OK) {
            return err;
        }
    }
    return VGS_OK;
}

/**
 * Serve one queue's chains available at the start: the control queue's as
 * `serve_control` says, the cursor queue's in order. Returns VGS_OK, or the
 * transport fault that puts the device in NEEDS_RESET.
 */
static int serve(struct vd *d, uint32_t qi)
{
    const struct vgs_queue *q = &d->drv.queue[qi];
    uint16_t idx;
    int err;

    if (q->size == 0) {
        return VGS_OK;
    }
    if (qi == VGS_QUEUE_CONTROL) {
        return serve_control(d);
    }
    if ((err = vgs_avail_read(d->base, q, d->last[qi], &idx)) != VGS_OK) {
        return err;
    }
    while (d->last[qi] != idx) {
        if ((err = serve_chain(d, qi, vgs_avail_head(d->base, q, d->last[qi]++))) != VGS_OK) {
            return err;
        }
    }
    return VGS_OK;
}

/** Take the driver's setup for the offered epoch, or refuse it. */
static int accept_setup(struct vd *d, const struct vgs_driver *drv)
{
    int err = vgs_check_accept(&d->l, drv, &d->me);

    if (err != VGS_OK) {
        fprintf(stderr, "backend: refused: epoch %u: %s\n", (unsigned)drv->epoch,
                vgs_strerror(err));
        pthread_mutex_lock(&d->lock);
        d->drv = *drv;
        pthread_mutex_unlock(&d->lock);
        publish(d, VGS_DEV_FAILED, (uint32_t)err);
        d->n.faults++;
        return VD_WORKED;
    }
    pthread_mutex_lock(&d->lock);
    d->drv = *drv;
    pthread_mutex_unlock(&d->lock);
    publish(d, VGS_DEV_RUNNING, VGS_OK);
    if (d->verbose) {
        fprintf(stderr, "backend: running: epoch %u, features 0x%llx, transport 0x%llx\n",
                (unsigned)drv->epoch, (unsigned long long)drv->features,
                (unsigned long long)drv->transport_features);
    }
    return VD_WORKED;
}

/**
 * Look at the region once: answer a new epoch with an offer, take the
 * driver's setup, serve the queues while running, and mark a transport
 * fault. Returns VD_IDLE, VD_WORKED or VD_GONE.
 */
int vd_step(struct vd *d)
{
    struct vgs_driver drv;
    uint64_t before = d->n.commands;
    int err;

    if (vgs_snap_driver(d->base, &drv) != VGS_OK || drv.state == VGS_DRV_NONE) {
        return VD_IDLE;
    }
    if (drv.state == VGS_DRV_GONE) {
        if (d->me.state == VGS_DEV_GONE) {
            return VD_GONE;
        }
        d->ops->reset(d->cookie);
        publish(d, VGS_DEV_GONE, VGS_OK);
        return VD_GONE;
    }
    if (drv.epoch != d->me.epoch || d->me.state == VGS_DEV_GONE) {
        if (vgs_snap_layout(d->base, &d->l) != VGS_OK ||
            vgs_check_layout(&d->l, d->size) != VGS_OK) {
            return VD_IDLE;
        }
        offer(d, &drv);
        if (d->verbose) {
            fprintf(stderr, "backend: offer: epoch %u\n", (unsigned)drv.epoch);
        }
        return VD_WORKED;
    }
    if (d->me.state == VGS_DEV_READY && drv.state == VGS_DRV_OK) {
        return accept_setup(d, &drv);
    }
    if (d->me.state != VGS_DEV_RUNNING) {
        return VD_IDLE;
    }
    fence_retry(d);
    if ((err = serve(d, VGS_QUEUE_CONTROL)) == VGS_OK) {
        err = serve(d, VGS_QUEUE_CURSOR);
    }
    if (err != VGS_OK) {
        fprintf(stderr, "backend: fault: epoch %u: %s\n", (unsigned)d->me.epoch, vgs_strerror(err));
        publish(d, VGS_DEV_NEEDS_RESET, (uint32_t)err);
        d->n.faults++;
        return VD_WORKED;
    }
    vd_kick(d);
    return d->n.commands != before ? VD_WORKED : VD_IDLE;
}
