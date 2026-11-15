/*
 * drm_lease.c: display leases, as Linux 6.12's drm_lease.c: a card's master
 * lends CRTCs, connectors and planes to a new file of the card, the lessee,
 * which holds the master role with its lessor and sees and changes only
 * what it leases, until the lessor revokes the lease or either file closes.
 */

#include "drm.h"

#include "../fs/devfs.h"
#include "../lib/kerrno.h"
#include "../lib/kstring.h"
#include "../mm/kheap.h"
#include "../mm/usercopy.h"
#include "../proc/percpu.h"
#include "../proc/process.h"

/* Linux's O_CLOEXEC and O_NONBLOCK, the flags CREATE_LEASE takes. */
#define LEASE_O_CLOEXEC  02000000u
#define LEASE_O_NONBLOCK 04000u

/* The largest object list CREATE_LEASE copies, Linux's KMALLOC_MAX_SIZE on
 * x86, past which its memdup_array_user answers ENOMEM. */
#define LEASE_COPY_MAX (4u << 20)

/** Whether objects of `type` may be leased: connectors, CRTCs and planes,
 * as Linux's drm_mode_object_lease_required. */
static int leasable(uint32_t type)
{
    return type == DRM_MODE_OBJECT_CONNECTOR || type == DRM_MODE_OBJECT_CRTC ||
           type == DRM_MODE_OBJECT_PLANE;
}

/** The lessee `lessee_id` of the master record `lessor_id` of `dev`, or
 * NULL, as Linux's _drm_find_lessee. Under the big lock, which keeps the
 * device's files. */
static struct drm_file *find_lessee(struct drm_device *dev, uint32_t lessor_id, uint32_t lessee_id)
{
    struct drm_file *f;

    for (f = dev->files; f; f = f->dev_next) {
        if (f->lessor_id == lessor_id && f->lessee_id == lessee_id) {
            return f;
        }
    }
    return NULL;
}

/** Whether a lessee of the record `lessor_id` leases `id`, as Linux's
 * _drm_has_leased. */
static int has_leased(struct drm_device *dev, uint32_t lessor_id, uint32_t id)
{
    struct drm_file *f;

    for (f = dev->files; f; f = f->dev_next) {
        if (f->lessor_id == lessor_id && drm_lease_held(f, id)) {
            return 1;
        }
    }
    return 0;
}

/** Add `id` to the ascending set `set` of `*n`: 0, or -ENOSPC for an ID it
 * holds, Linux's answer of an idr slot taken. `set` has room. */
static int32_t lease_add(uint32_t *set, uint32_t *n, uint32_t id)
{
    uint32_t i = *n;

    while (i > 0 && set[i - 1u] > id) {
        i--;
    }
    if (i > 0 && set[i - 1u] == id) {
        return -ENOSPC;
    }
    k_memmove(set + i + 1u, set + i, (*n - i) * sizeof(*set));
    set[i] = id;
    (*n)++;
    return 0;
}

/**
 * Make the ascending set of the `count` objects `ids` that `lessor` leases,
 * as Linux's fill_object_idr and validate_lease: each must be a connector,
 * CRTC or plane the lessor sees (ENOENT for none, EINVAL for another kind),
 * with at least one CRTC and one connector, and a plane when the lessor has
 * universal planes (EINVAL); without them, each CRTC brings its primary and
 * cursor planes. An ID given twice, or a plane a CRTC brought, is ENOSPC.
 * The set, from the heap, goes to `*out` and its size to `*n`.
 */
static int32_t lease_fill(struct drm_device *dev, struct drm_file *lessor, const uint32_t *ids,
                          uint32_t count, uint32_t **out, uint32_t *n)
{
    int universal = lessor->universal_planes;
    int crtcs = 0, connectors = 0, planes = 0;
    struct drm_mode_object *obj;
    uint32_t *set, i, type;
    int32_t rc = 0;

    for (i = 0; i < count; i++) {
        obj = drm_mode_object_find(dev, lessor, ids[i], DRM_MODE_OBJECT_ANY);
        if (!obj) {
            return -ENOENT;
        }
        type = obj->type;
        drm_mode_object_put(obj);
        if (!leasable(type)) {
            return -EINVAL;
        }
        crtcs += type == DRM_MODE_OBJECT_CRTC;
        connectors += type == DRM_MODE_OBJECT_CONNECTOR;
        planes += type == DRM_MODE_OBJECT_PLANE;
    }
    if (!crtcs || !connectors || (universal && !planes)) {
        return -EINVAL;
    }
    set = (uint32_t *)kmalloc((count + 2u * (uint32_t)crtcs) * sizeof(*set));
    if (!set) {
        return -ENOMEM;
    }
    *n = 0;
    for (i = 0; i < count && rc == 0; i++) {
        struct drm_crtc *crtc;

        rc = lease_add(set, n, ids[i]);
        crtc = rc == 0 && !universal ? drm_crtc_find(dev, lessor, ids[i]) : NULL;
        if (crtc && crtc->primary) {
            rc = lease_add(set, n, crtc->primary->base.id);
        }
        if (rc == 0 && crtc && crtc->cursor) {
            rc = lease_add(set, n, crtc->cursor->base.id);
        }
    }
    if (rc != 0) {
        kfree(set);
        return rc;
    }
    *out = set;
    return 0;
}

/**
 * CREATE_LEASE, Linux's drm_mode_create_lease_ioctl: lease the objects the
 * caller names to a new file of its card, opened as the caller's was, and
 * give its descriptor and lessee ID. EINVAL for flags but O_CLOEXEC and
 * O_NONBLOCK, for a lessee's call (no sub-leases) and for a list
 * `lease_fill` refuses; EFAULT, ENOMEM or EOVERFLOW for a list that cannot
 * be copied; EMFILE; EBUSY for an object another lessee of the caller holds.
 * An empty list makes a lessee of nothing. Runs under the big lock and the
 * commit lock, which keep the device's files and the leases.
 */
int32_t drm_mode_create_lease_ioctl(struct drm_device *dev, void *data, struct drm_file *file)
{
    struct drm_mode_create_lease *cl = (struct drm_mode_create_lease *)data;
    struct drm_lease_open lo;
    uint32_t *ids, *leases = NULL, n = 0, i, id;
    uint64_t bytes;
    char name[24];
    int32_t rc, fd;

    if (!drm_core_check_feature(dev, DRIVER_MODESET)) {
        return -EOPNOTSUPP;
    }
    if (cl->flags & ~(LEASE_O_CLOEXEC | LEASE_O_NONBLOCK)) {
        return -EINVAL;
    }
    if (file->lessor_id || !current_proc) {
        return -EINVAL;
    }
    if (cl->object_count) {
        bytes = (uint64_t)cl->object_count * sizeof(*ids);
        if (sizeof(size_t) < 8 && bytes > 0xffffffffu) {
            return -EOVERFLOW;
        }
        if (bytes > LEASE_COPY_MAX) {
            return -ENOMEM;
        }
        ids = (uint32_t *)kmalloc((size_t)bytes);
        if (!ids) {
            return -ENOMEM;
        }
        if (copy_from_user(ids, (const void __user *)(uintptr_t)cl->object_ids, (size_t)bytes) !=
            0) {
            kfree(ids);
            return -EFAULT;
        }
        rc = lease_fill(dev, file, ids, cl->object_count, &leases, &n);
        kfree(ids);
        if (rc != 0) {
            return rc;
        }
    }
    if (!fd_can_alloc(current_proc->fds)) {
        kfree(leases);
        return -EMFILE;
    }
    for (i = 0; i < n; i++) {
        if (has_leased(dev, file->master_id, leases[i])) {
            kfree(leases);
            return -EBUSY;
        }
    }
    /* The lowest lessee ID free in the caller's record, as Linux's idr. */
    for (id = 1; find_lessee(dev, file->master_id, id); id++) {
    }
    lo.opener = current_proc;
    lo.lessor_id = file->master_id;
    lo.lessee_id = id;
    lo.leases = leases;
    lo.lease_count = n;
    lo.file = NULL;
    k_strncpy(name, "dri/card", sizeof(name)); /* minor_fill's name of the card */
    i = (uint32_t)k_strlen(name);
    if (dev->index >= 10) {
        name[i++] = (char)('0' + dev->index / 10);
    }
    name[i++] = (char)('0' + dev->index % 10);
    name[i] = '\0';
    dev->lease_open = &lo;
    fd = devfs_open(
        name, (int)(file->open_flags | ((cl->flags & LEASE_O_CLOEXEC) ? LEASE_O_CLOEXEC : 0u)));
    dev->lease_open = NULL;
    /* The open took the set when it made the lessee. */
    kfree(lo.leases);
    if (fd < 0) {
        return fd;
    }
    cl->fd = (uint32_t)fd;
    cl->lessee_id = id;
    return 0;
}

/**
 * LIST_LESSEES, Linux's drm_mode_list_lessees_ioctl: the IDs of the
 * caller's lessees whose leases are not revoked, in the order they were
 * made, as many as the caller's array holds, and their number. EINVAL for a
 * nonzero pad, EFAULT, ENOMEM.
 */
int32_t drm_mode_list_lessees_ioctl(struct drm_device *dev, void *data, struct drm_file *file)
{
    struct drm_mode_list_lessees *arg = (struct drm_mode_list_lessees *)data;
    struct drm_file *f;
    uint32_t *ids, count = 0, n, at;
    int32_t rc = 0;

    if (arg->pad) {
        return -EINVAL;
    }
    if (!drm_core_check_feature(dev, DRIVER_MODESET)) {
        return -EOPNOTSUPP;
    }
    for (f = dev->files; f; f = f->dev_next) {
        count += f->lessor_id == file->master_id && f->lease_count != 0;
    }
    n = count < arg->count_lessees ? count : arg->count_lessees;
    if (n) {
        ids = (uint32_t *)kmalloc(n * sizeof(*ids));
        if (!ids) {
            return -ENOMEM;
        }
        /* The device's list has the newest open first, and the oldest
         * lessee goes first, as on Linux's list of lessees. */
        at = count;
        for (f = dev->files; f; f = f->dev_next) {
            if (f->lessor_id == file->master_id && f->lease_count != 0 && --at < n) {
                ids[at] = f->lessee_id;
            }
        }
        if (copy_to_user((void __user *)(uintptr_t)arg->lessees_ptr, ids, n * sizeof(*ids)) != 0) {
            rc = -EFAULT;
        }
        kfree(ids);
    }
    if (rc == 0) {
        arg->count_lessees = count;
    }
    return rc;
}

/** Every mode object ID of `dev`, ascending, in a set from the heap at
 * `*out`, and their number: Linux's object_idr, which GET_LEASE gives its
 * owner. -ENOMEM. */
static int32_t all_objects(struct drm_device *dev, uint32_t **out, uint32_t *n)
{
    struct drm_mode_config *mc = &dev->mode_config;
    struct drm_property *prop;
    struct drm_property_blob *blob;
    struct drm_framebuffer *fb;
    uint32_t count, i, *set;

    count = mc->num_crtc + mc->num_plane + mc->num_encoder + mc->num_connector;
    for (prop = mc->properties; prop; prop = prop->next) {
        count++;
    }
    for (blob = mc->blobs; blob; blob = blob->next) {
        count++;
    }
    for (fb = mc->fbs; fb; fb = fb->next) {
        count++;
    }
    set = (uint32_t *)kmalloc((count ? count : 1u) * sizeof(*set));
    if (!set) {
        return -ENOMEM;
    }
    *n = 0;
    for (i = 0; i < mc->num_crtc; i++) {
        (void)lease_add(set, n, mc->crtcs[i]->base.id);
    }
    for (i = 0; i < mc->num_plane; i++) {
        (void)lease_add(set, n, mc->planes[i]->base.id);
    }
    for (i = 0; i < mc->num_encoder; i++) {
        (void)lease_add(set, n, mc->encoders[i]->base.id);
    }
    for (i = 0; i < mc->num_connector; i++) {
        (void)lease_add(set, n, mc->connectors[i]->base.id);
    }
    for (prop = mc->properties; prop; prop = prop->next) {
        (void)lease_add(set, n, prop->base.id);
    }
    for (blob = mc->blobs; blob; blob = blob->next) {
        (void)lease_add(set, n, blob->base.id);
    }
    for (fb = mc->fbs; fb; fb = fb->next) {
        (void)lease_add(set, n, fb->base.id);
    }
    *out = set;
    return 0;
}

/**
 * GET_LEASE, Linux's drm_mode_get_lease_ioctl: the IDs of the objects the
 * caller may use, ascending, as many as its array holds, and their number:
 * a lessee's leases, none once revoked, or every mode object for its owner.
 * EINVAL for a nonzero pad, EFAULT, ENOMEM.
 */
int32_t drm_mode_get_lease_ioctl(struct drm_device *dev, void *data, struct drm_file *file)
{
    struct drm_mode_get_lease *arg = (struct drm_mode_get_lease *)data;
    uint32_t *set = NULL, count, n;
    int32_t rc = 0;

    if (arg->pad) {
        return -EINVAL;
    }
    if (!drm_core_check_feature(dev, DRIVER_MODESET)) {
        return -EOPNOTSUPP;
    }
    if (file->lessor_id) {
        count = file->lease_count;
    } else {
        rc = all_objects(dev, &set, &count);
        if (rc != 0) {
            return rc;
        }
    }
    n = count < arg->count_objects ? count : arg->count_objects;
    if (n && copy_to_user((void __user *)(uintptr_t)arg->objects_ptr, set ? set : file->leases,
                          n * sizeof(*set)) != 0) {
        rc = -EFAULT;
    }
    kfree(set);
    if (rc == 0) {
        arg->count_objects = count;
    }
    return rc;
}

/**
 * REVOKE_LEASE, Linux's drm_mode_revoke_lease_ioctl: take every object
 * from the caller's lessee `lessee_id`, which stays, holding nothing, until
 * it closes. ENOENT for no lessee of that ID in the caller's record or its
 * lessor's, EACCES when the caller is not that lessee's lessor.
 */
int32_t drm_mode_revoke_lease_ioctl(struct drm_device *dev, void *data, struct drm_file *file)
{
    struct drm_mode_revoke_lease *arg = (struct drm_mode_revoke_lease *)data;
    struct drm_file *lessee;

    if (!drm_core_check_feature(dev, DRIVER_MODESET)) {
        return -EOPNOTSUPP;
    }
    lessee = find_lessee(dev, file->lessor_id ? file->lessor_id : file->master_id, arg->lessee_id);
    if (!lessee) {
        return -ENOENT;
    }
    if (lessee->lessor_id != file->master_id) {
        return -EACCES;
    }
    lessee->lease_count = 0;
    return 0;
}

void drm_lease_file_release(struct drm_file *file)
{
    struct drm_device *dev = file->dev;
    struct drm_file *f;

    if (!file->is_master) {
        return;
    }
    if (!file->lessor_id) {
        for (f = dev->files; f && f->lessor_id != file->master_id; f = f->dev_next) {
        }
        if (!f) {
            return;
        }
    }
    drm_modeset_lock_nointr(dev);
    if (file->lessor_id) {
        file->lessor_id = 0;
        file->lessee_id = 0;
        file->lease_count = 0;
        kfree(file->leases);
        file->leases = NULL;
        drm_modeset_unlock(dev);
        drm_sysfs_lease_event(dev);
        return;
    }
    /* The lessees of a record whose maker goes keep nothing, as Linux's
     * drm_lease_revoke of the owner. */
    for (f = dev->files; f; f = f->dev_next) {
        if (f->lessor_id == file->master_id) {
            f->lease_count = 0;
        }
    }
    drm_modeset_unlock(dev);
}
