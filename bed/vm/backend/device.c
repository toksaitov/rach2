/*
 * device.c: the device side of the shared-memory GPU transport: the
 * handshake of each epoch, the two virtqueues, and virtio-gpu's commands
 * checked and handed to the renderer and display operations of vgpudev.h.
 * It calls no library but libc, so that the fuzzer links it alone.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#define VGS_IMPLEMENTATION
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

/* Copy n bytes at offset off of the region, checked before, to private dst. */
static void region_get(struct vd *d, uint64_t off, void *dst, uint64_t n)
{
    volatile const uint8_t *s = d->base + off;
    uint8_t *p = dst;
    uint64_t i;

    for (i = 0; i < n; i++) {
        p[i] = s[i];
    }
}

/* Copy n private bytes to offset off of the region, checked before. */
static void region_put(struct vd *d, uint64_t off, const void *src, uint64_t n)
{
    volatile uint8_t *s = d->base + off;
    const uint8_t *p = src;
    uint64_t i;

    for (i = 0; i < n; i++) {
        s[i] = p[i];
    }
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
    d->n.notified++;
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

/**
 * End every object of the old epoch, in the renderer and the table, drop
 * the fenced commands of its queues, and write the offer for the driver's
 * new epoch, every field before the state.
 */
static void offer(struct vd *d, const struct vgs_driver *drv)
{
    volatile struct vgs_device *v = VGS_DEVICE(d->base);
    uint32_t i;

    d->ops->reset(d->cookie);
    for (i = 0; i < VD_OBJS; i++) {
        res_clear(&d->res[i]);
    }
    vgs_table_epoch(&d->t, drv->epoch);
    memset(d->last, 0, sizeof(d->last));
    memset(d->used, 0, sizeof(d->used));
    memset(d->scanout, 0, sizeof(d->scanout));
    memset(d->pend, 0, sizeof(d->pend));
    d->drv = *drv;
    d->npend = 0;
    d->kick = 0;
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
    region_put(d, VGS_DEVICE_OFF + 8, (const uint8_t *)&d->me + 8, sizeof(d->me) - 8);
    VGS_MB();
    v->epoch = drv->epoch;
    VGS_MB();
    v->state = VGS_DEV_READY;
    VGS_MB();
    d->n.epochs++;
    notify(d, VGS_VEC_CONTROL, 0);
}

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
    d->cmd = malloc(off->limits.max_cmd);
    if (d->cmd == 0) {
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
    free(d->cmd);
    d->cmd = 0;
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
    if (r->iov != 0 || r->blob) {
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

/** RESOURCE_DETACH_BACKING. */
static int cmd_detach(struct vd *d, const uint8_t *body)
{
    struct c_resource c;
    struct vgs_obj *o;
    struct vd_res *r;

    memcpy(&c, body, sizeof(c));
    if ((r = find_res(d, c.resource_id, &o)) == 0) {
        return VD_RESP_ERR_RESOURCE_ID;
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

/** CTX_DESTROY. */
static int cmd_ctx_destroy(struct vd *d, const struct vd_hdr *h)
{
    struct vgs_obj *o = vgs_table_find(&d->t, VGS_OBJ_CONTEXT, h->ctx_id);

    if (o == 0) {
        return VD_RESP_ERR_CONTEXT_ID;
    }
    d->ops->ctx_destroy(d->cookie, h->ctx_id);
    vgs_table_del(&d->t, o);
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

/** SUBMIT_3D: whole dwords that follow the body, in the private copy. */
static int cmd_submit(struct vd *d, const struct vd_hdr *h, uint8_t *body, uint32_t blen)
{
    struct c_submit c;

    memcpy(&c, body, sizeof(c));
    if (!ctx_live(d, h->ctx_id)) {
        return VD_RESP_ERR_CONTEXT_ID;
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

/**
 * RESOURCE_MAP_BLOB under VGS_TF_PAGES: the page list of a mappable HOST3D
 * blob from the page `offset` names, as many runs as the chain's writable
 * bytes hold after the header and the `struct vgs_pages`, written to resp.
 * The renderer pins the blob at its first answer. Returns the answer's
 * length; *type is RESP_OK_MAP_INFO or the error.
 */
static uint32_t cmd_map_blob(struct vd *d, const uint8_t *body, uint8_t *resp, uint32_t *type)
{
    uint32_t room = d->room < VD_RESP_MAX ? d->room : VD_RESP_MAX;
    struct vgs_pages p;
    struct c_map_blob c;
    struct vd_pages q;
    struct vd_res *r;
    int err;

    memcpy(&c, body, sizeof(c));
    *type = VD_RESP_ERR_PARAMETER;
    if ((d->drv.transport_features & VGS_TF_PAGES) == 0 || d->ops->pages == 0) {
        *type = VD_RESP_ERR_UNSPEC;
        return sizeof(struct vd_hdr);
    }
    if ((r = find_res(d, c.resource_id, 0)) == 0) {
        *type = VD_RESP_ERR_RESOURCE_ID;
        return sizeof(struct vd_hdr);
    }
    memset(&p, 0, sizeof(p));
    p.total = (r->size + VGS_PAGE - 1) / VGS_PAGE;
    p.first = c.offset;
    if (!r->blob || r->mem != BLOB_MEM_HOST3D || (r->flags & BLOB_FLAG_USE_MAPPABLE) == 0 ||
        p.first >= p.total || room < VGS_PAGES_OFF + sizeof(struct vgs_run)) {
        return sizeof(struct vd_hdr);
    }
    memset(&q, 0, sizeof(q));
    q.id = c.resource_id;
    q.epoch = d->drv.epoch;
    q.size = r->size;
    q.first = p.first;
    q.max = (room - VGS_PAGES_OFF) / sizeof(struct vgs_run);
    if ((q.runs = calloc(q.max, sizeof(*q.runs))) == 0) {
        *type = VD_RESP_ERR_OUT_OF_MEMORY;
        return sizeof(struct vd_hdr);
    }
    err = d->ops->pages(d->cookie, &q);
    if (err == 0) {
        r->pinned = 1;
        err = runs_fit(&q, p.total - p.first);
    }
    if (err == 0) {
        p.map_info = q.map_info;
        p.count = q.count;
        memcpy(resp + sizeof(struct vd_hdr), &p, sizeof(p));
        memcpy(resp + VGS_PAGES_OFF, q.runs, (size_t)q.count * sizeof(*q.runs));
        d->n.maps++;
        if (d->verbose) {
            fprintf(stderr, "backend: map: resource %u, pages %llu of %llu, %u runs, cache %u\n",
                    (unsigned)q.id, (unsigned long long)p.first, (unsigned long long)p.total,
                    (unsigned)q.count, (unsigned)q.map_info);
        }
    }
    free(q.runs);
    *type = err == 0 ? VD_RESP_OK_MAP_INFO : (uint32_t)err;
    return err == 0 ? VGS_PAGES_OFF + q.count * (uint32_t)sizeof(struct vgs_run)
                    : (uint32_t)sizeof(struct vd_hdr);
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
        return cmd_map_blob(d, body, d->resp, type);
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

/** Publish a chain's completion in its queue's used ring. */
static void complete(struct vd *d, uint32_t qi, uint32_t head, uint32_t put)
{
    vgs_used_push(d->base, &d->drv.queue[qi], &d->used[qi], head, put);
    d->kick = 1;
    d->n.completed++;
}

/**
 * Hold a fenced command until its fence retires. Returns 0, or -1 when
 * every pending slot is taken, which the caller answers at once.
 */
static int hold(struct vd *d, uint32_t qi, uint32_t head, const struct vgs_chain *c,
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
        p->rlen = sizeof(struct vd_hdr);
        memcpy(p->resp, resp, sizeof(struct vd_hdr));
        p->chain = *c;
        d->npend++;
        return 0;
    }
    return -1;
}

/**
 * Answer a command: at once, or, for a fenced command that succeeded,
 * once the renderer retires its fence on the timeline the header names.
 */
static void answer(struct vd *d, uint32_t qi, uint32_t head, const struct vgs_chain *c,
                   uint32_t type, uint32_t len)
{
    struct vd_hdr h, *o = (struct vd_hdr *)d->resp;
    int per_ctx;

    memcpy(&h, d->cmd, sizeof(h));
    o->type = type;
    if (type >= VD_RESP_ERR_UNSPEC) {
        d->n.errors++;
        if (d->verbose) {
            fprintf(stderr, "backend: command 0x%x: response 0x%x\n", (unsigned)h.type,
                    (unsigned)type);
        }
    }
    if (h.flags & VD_FLAG_FENCE) {
        o->flags = h.flags & (VD_FLAG_FENCE | VD_FLAG_INFO_RING_IDX);
        o->fence_id = h.fence_id;
        o->ctx_id = h.ctx_id;
        o->ring_idx = h.ring_idx;
        per_ctx = (h.flags & VD_FLAG_INFO_RING_IDX) != 0;
        if (type == VD_RESP_OK_NODATA && (!per_ctx || ctx_live(d, h.ctx_id)) &&
            hold(d, qi, head, c, &h, d->resp) == 0) {
            d->n.fenced++;
            if (d->ops->fence(d->cookie, per_ctx ? h.ctx_id : 0, per_ctx, h.ring_idx, h.fence_id) ==
                0) {
                return;
            }
            vd_fence_retired(d, per_ctx ? h.ctx_id : 0, per_ctx, h.ring_idx, h.fence_id);
            return;
        }
    }
    complete(d, qi, head, put_response(d, c, d->resp, len));
}

/**
 * Complete every fenced command on a timeline whose fence is id or older:
 * the global one, whose ids the renderer keeps in 32 bits, or a context's
 * ring. The renderer calls this through its fence callbacks.
 */
void vd_fence_retired(struct vd *d, uint32_t ctx, int per_ctx, uint32_t ring, uint64_t id)
{
    uint32_t i;

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
        complete(d, p->queue, p->head, put_response(d, &p->chain, p->resp, p->rlen));
        p->used = 0;
        d->npend--;
    }
}

/** Ring the driver's queue vector once for the completions since the last. */
void vd_kick(struct vd *d)
{
    uint32_t qi;

    if (!d->kick) {
        return;
    }
    d->kick = 0;
    for (qi = 0; qi < VGS_QUEUES; qi++) {
        const struct vgs_queue *q = &d->drv.queue[qi];

        if (q->size != 0 &&
            (*(volatile uint16_t *)(d->base + q->avail) & VGS_AVAIL_F_NO_INTERRUPT) == 0) {
            notify(d, VGS_VEC_QUEUE, 1);
            return;
        }
    }
}

/**
 * Serve one queue's available chains. Returns VGS_OK, or the transport
 * fault that puts the device in NEEDS_RESET.
 */
static int serve(struct vd *d, uint32_t qi)
{
    const struct vgs_queue *q = &d->drv.queue[qi];
    uint16_t idx;
    int err;

    if (q->size == 0) {
        return VGS_OK;
    }
    if ((err = vgs_avail_read(d->base, q, d->last[qi], &idx)) != VGS_OK) {
        return err;
    }
    while (d->last[qi] != idx) {
        uint32_t head = vgs_avail_head(d->base, q, d->last[qi]), i, off = 0, type, len;
        struct vgs_chain c;

        if ((err = vgs_chain_read(d->base, &d->l, q, &d->me.limits, head, &c)) != VGS_OK) {
            return err;
        }
        for (i = 0; i < c.nread; i++) {
            region_get(d, c.seg[i].addr, d->cmd + off, c.seg[i].len);
            off += c.seg[i].len;
        }
        d->room = c.wbytes;
        d->last[qi]++;
        d->n.commands++;
        if (qi != VGS_QUEUE_CONTROL) {
            /* The cursor queue: a cursor is not drawn; its chains return. */
            complete(d, qi, head, 0);
            continue;
        }
        if (off < sizeof(struct vd_hdr)) {
            memset(d->resp, 0, sizeof(struct vd_hdr));
            ((struct vd_hdr *)d->resp)->type = VD_RESP_ERR_PARAMETER;
            complete(d, qi, head, put_response(d, &c, d->resp, sizeof(struct vd_hdr)));
            d->n.errors++;
            continue;
        }
        len = execute(d, off, &type);
        answer(d, qi, head, &c, type, len);
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
        d->drv = *drv;
        publish(d, VGS_DEV_FAILED, (uint32_t)err);
        d->n.faults++;
        return VD_WORKED;
    }
    d->drv = *drv;
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
