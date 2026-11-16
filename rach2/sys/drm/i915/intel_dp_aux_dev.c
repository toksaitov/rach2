/*
 * intel_dp_aux_dev.c: /dev/drm_dp_auxN, by which a program reads and
 * writes a DisplayPort sink's DPCD, one for the AUX channel of each eDP
 * and DisplayPort connector, with its entry below the connector's
 * directory in /sys, as Linux's drm_dp_aux_dev.c makes them.
 */

#include "i915.h"
#include "intel_dp.h"

#include "../../fs/devfs.h"
#include "../../fs/file.h"
#include "../../fs/fd.h"
#include "../../fs/synthfs.h"
#include "../../fs/sysfs.h"
#include "../../klog.h"
#include "../../lib/kerrno.h"
#include "../../lib/kstd.h"
#include "../../lib/kstring.h"
#include "../../mm/usercopy.h"
#include "../../proc/process.h"

/* The devices' major; Linux's is dynamic, from the top of 234 to 254
 * down, and 240, 242, 245 and 254 are taken here. */
#define AUX_DEV_MAJOR 239

/* The channels: the eDP panel's and those of DDI B to D. */
#define AUX_DEV_MAX 4

/* The DPCD's size, Linux's AUX_MAX_OFFSET: its addresses are 20 bits. */
#define AUX_MAX_OFFSET (1u << 20)

/*
 * One device, by its minor: `dp` the channel's port, NULL while the slot is
 * free; `devname` "drm_dp_auxN" and `name` Linux's aux->name of the channel,
 * "AUX A/DDI A/PHY A"; `dir`, `class_link` and `char_link` its handles in
 * /sys (fs/sysfs.h), 0 for one not made.
 */
struct aux_dev {
    struct intel_dp *dp;
    struct devfs_node node;
    char devname[16];
    char name[24];
    int dir, class_link, char_link;
};

static struct aux_dev aux_devs[AUX_DEV_MAX];

/* What a show writes: the bytes so far, which may pass `cap`, so that the
 * show returns how many it needs. */
struct emit {
    char *buf;
    uint32_t cap, len;
};

/** Append the string `s` to `e`, as far as its room goes. */
static void emit_str(struct emit *e, const char *s)
{
    for (; *s; s++, e->len++) {
        if (e->len < e->cap) {
            e->buf[e->len] = *s;
        }
    }
}

/** Append `v` in decimal to `e`. */
static void emit_u(struct emit *e, uint32_t v)
{
    char num[12];

    (void)k_itoa((int)v, num, sizeof(num), 10, false);
    emit_str(e, num);
}

/* The attributes of a device's directory, by their `arg`. */
#define AUX_ATTR_DEV    0
#define AUX_ATTR_NAME   1
#define AUX_ATTR_UEVENT 2

/** A device's `dev`, `name` or `uevent`, as Linux's device core and
 * drm_dp_aux_dev.c's name_show write them. */
static int32_t aux_show(void *obj, int arg, char *buf, uint32_t cap)
{
    const struct aux_dev *a = (const struct aux_dev *)obj;
    uint32_t minor = (uint32_t)(a - aux_devs);
    struct emit e;

    e.buf = buf;
    e.cap = cap;
    e.len = 0;
    if (arg == AUX_ATTR_NAME) {
        emit_str(&e, a->name);
    } else if (arg == AUX_ATTR_DEV) {
        emit_u(&e, AUX_DEV_MAJOR);
        emit_str(&e, ":");
        emit_u(&e, minor);
    } else {
        emit_str(&e, "MAJOR=");
        emit_u(&e, AUX_DEV_MAJOR);
        emit_str(&e, "\nMINOR=");
        emit_u(&e, minor);
        emit_str(&e, "\nDEVNAME=");
        emit_str(&e, a->devname);
    }
    emit_str(&e, "\n");
    return (int32_t)e.len;
}

/** Bind an open to the device's port and hold its DRM device, Linux's
 * auxdev_open: -ENODEV for a minor no channel holds. */
static int32_t aux_open(struct devfs_node *node, fd_entry_t *e, int flags)
{
    struct aux_dev *a = (struct aux_dev *)node->priv;

    (void)flags;
    if (!a->dp) {
        return -ENODEV;
    }
    drm_dev_get(&a->dp->i915->drm);
    e->priv = a->dp;
    return 0;
}

/** Let the device go at the last close. */
static void aux_close(fd_entry_t *e)
{
    struct intel_dp *dp = (struct intel_dp *)e->priv;

    if (dp) {
        drm_dev_put(&dp->i915->drm);
    }
    e->priv = NULL;
}

/**
 * Read or write up to `count` bytes of the DPCD at the description's offset,
 * Linux's auxdev_read_iter and auxdev_write_iter: 16 bytes a transfer under
 * the device's mode lock, as every AUX transfer runs, the user's buffer
 * copied outside it. The transfer stops at the DPCD's end, at a signal, or
 * at the first error, which it returns when nothing moved; otherwise the
 * bytes moved, which advance the offset. -ENODEV once the device is gone.
 */
static int32_t aux_rw(fd_entry_t *e, char __user *ubuf, uint32_t count, int write)
{
    struct intel_dp *dp = (struct intel_dp *)e->priv;
    struct drm_device *dev = &dp->i915->drm;
    uint8_t chunk[DP_AUX_MAX_PAYLOAD_BYTES];
    uint64_t start = e->ofd->offset, pos = start;
    int32_t res = 0;
    uint32_t todo;

    if (pos >= AUX_MAX_OFFSET) {
        return 0;
    }
    if (count > AUX_MAX_OFFSET - (uint32_t)pos) {
        count = AUX_MAX_OFFSET - (uint32_t)pos;
    }
    while (count != 0) {
        todo = count < sizeof(chunk) ? count : (uint32_t)sizeof(chunk);
        if (signal_pending()) {
            res = -ERESTARTSYS;
            break;
        }
        if (write && copy_from_user(chunk, ubuf, todo) != 0) {
            res = -EFAULT;
            break;
        }
        if ((res = drm_modeset_lock(dev)) != 0) {
            break;
        }
        if (dev->unplugged) {
            res = -ENODEV;
        } else if (write) {
            res = intel_dp_dpcd_write(dp, (uint32_t)pos, chunk, todo);
        } else {
            res = intel_dp_dpcd_read(dp, (uint32_t)pos, chunk, todo);
        }
        drm_modeset_unlock(dev);
        if (res <= 0) {
            break;
        }
        if (!write && copy_to_user(ubuf, chunk, (uint32_t)res) != 0) {
            res = -EFAULT;
            break;
        }
        ubuf += res;
        pos += (uint32_t)res;
        count -= (uint32_t)res;
    }
    if (pos != start) {
        res = (int32_t)(pos - start);
    }
    e->ofd->offset = pos;
    return res;
}

/** Read the DPCD at the offset (`aux_rw`). */
static int32_t aux_read(fd_entry_t *e, char __user *buf, uint32_t count)
{
    return aux_rw(e, buf, count, 0);
}

/** Write the DPCD at the offset (`aux_rw`). */
static int32_t aux_write(fd_entry_t *e, const char __user *buf, uint32_t count)
{
    return aux_rw(e, (char __force __user *)(uintptr_t)buf, count, 1);
}

/** Seek in the DPCD, Linux's fixed_size_llseek of AUX_MAX_OFFSET bytes:
 * SEEK_DATA and SEEK_HOLE as on a file of that size, -EINVAL for an offset
 * outside it. */
static int64_t aux_llseek(fd_entry_t *e, int64_t offset, int whence)
{
    const int64_t size = AUX_MAX_OFFSET;
    int64_t at;

    switch (whence) {
    case 0:
        at = offset;
        break;
    case 1:
        at = (int64_t)e->ofd->offset + offset;
        break;
    case 2:
        at = size + offset;
        break;
    case 3: /* SEEK_DATA */
    case 4: /* SEEK_HOLE */
        if (offset < 0 || offset >= size) {
            return -ENXIO;
        }
        at = whence == 3 ? offset : size;
        break;
    default:
        return -EINVAL;
    }
    return at < 0 || at > size ? -EINVAL : at;
}

static const struct file_ops aux_fops = {aux_read,  aux_write, NULL,       NULL,
                                         aux_close, NULL,      aux_llseek, FILE_OPS_POSITIONED};

/** /sys/class/drm_dp_aux_dev, made on its first use and kept, as Linux's
 * class lives while the DRM helpers do: its handle, or -errno. */
static int aux_class_dir(void)
{
    static const char name[] = "drm_dp_aux_dev";
    int cls = sysfs_anchor(SYSFS_CLASS, 0);
    int h;

    if (cls < 0) {
        return cls;
    }
    h = sysfs_node_child(cls, name, (int)sizeof(name) - 1);
    return h > 0 ? h : sysfs_add_dir(cls, name);
}

/** Remove `a`'s entries and node and free its slot, after its uevent
 * `remove` when `announced` says its `add` went out. */
static void aux_dev_remove(struct aux_dev *a, int announced)
{
    if (announced) {
        sysfs_uevent(a->dir, "remove", NULL);
    }
    sysfs_remove(a->char_link);
    sysfs_remove(a->class_link);
    sysfs_remove(a->dir);
    devfs_node_unregister(&a->node);
    a->dir = a->class_link = a->char_link = 0;
    a->dp = NULL;
}

/** Write "MAJOR:MINOR" of minor `minor` into `out`, `cap` bytes. */
static void aux_devno(char *out, uint32_t cap, uint32_t minor)
{
    uint32_t at;

    (void)k_itoa(AUX_DEV_MAJOR, out, cap, 10, false);
    at = (uint32_t)k_strlen(out);
    out[at++] = ':';
    (void)k_itoa((int)minor, out + at, cap - at, 10, false);
}

/**
 * Make the device of `dp`'s channel at the lowest free minor, as Linux's
 * drm_dp_aux_register_devnode: its node in /dev, its directory below its
 * connector's with `dev`, `name`, `uevent`, `subsystem` and `device`, and
 * its links in /sys/class/drm_dp_aux_dev and /sys/dev/char, then its
 * uevent `add`. 0, or -errno with nothing left made.
 */
static int32_t aux_dev_add(struct intel_dp *dp)
{
    static const struct sysfs_attr attrs[] = {{"dev", 0444u, aux_show, NULL},
                                              {"name", 0444u, aux_show, NULL}};
    int parent = drm_sysfs_connector_dir(dp->connector);
    struct aux_dev *a = NULL;
    uint32_t i, minor;
    char devno[16];
    int cls, h;

    for (i = 0; i < AUX_DEV_MAX && !a; i++) {
        a = aux_devs[i].dp ? NULL : &aux_devs[i];
    }
    if (!a) {
        return -ENOSPC;
    }
    if (parent <= 0) {
        return -ENOENT;
    }
    if ((cls = aux_class_dir()) < 0) {
        return cls;
    }
    minor = (uint32_t)(a - aux_devs);
    k_memcpy(a->devname, "drm_dp_aux", 10);
    (void)k_itoa((int)minor, a->devname + 10, sizeof(a->devname) - 10, 10, false);
    k_memcpy(a->name, "AUX ?/DDI ?/PHY ?", 18);
    a->name[4] = (char)('A' + dp->aux_port);
    a->name[10] = a->name[16] = (char)('A' + dp->port);
    k_memset(&a->node, 0, sizeof(a->node));
    a->node.name = a->devname;
    a->node.major = AUX_DEV_MAJOR;
    a->node.minor = (uint16_t)minor;
    a->node.mode = 0600;
    a->node.ops = &aux_fops;
    a->node.open = aux_open;
    a->node.priv = a;
    if ((h = devfs_node_register(&a->node)) != 0) {
        return h;
    }
    a->dp = dp;
    if ((h = sysfs_add_dir(parent, a->devname)) > 0) {
        a->dir = h;
    }
    for (i = 0; h >= 0 && i < (uint32_t)(sizeof(attrs) / sizeof(attrs[0])); i++) {
        h = sysfs_add_attr(a->dir, &attrs[i], a, (int)i);
    }
    if (h >= 0) {
        h = sysfs_add_uevent(a->dir, aux_show, a, AUX_ATTR_UEVENT);
    }
    if (h >= 0) {
        h = sysfs_add_link(a->dir, "subsystem", cls);
    }
    if (h >= 0) {
        h = sysfs_add_link(a->dir, "device", parent);
    }
    if (h >= 0 && (h = sysfs_add_link(cls, a->devname, a->dir)) > 0) {
        a->class_link = h;
    }
    if (h >= 0) {
        aux_devno(devno, sizeof(devno), minor);
        if ((h = sysfs_add_link(sysfs_anchor(SYSFS_DEV_CHAR, 0), devno, a->dir)) > 0) {
            a->char_link = h;
        }
    }
    if (h < 0) {
        aux_dev_remove(a, 0);
        return h;
    }
    sysfs_uevent(a->dir, "add", NULL);
    return 0;
}

/** Whether `dp` is a channel whose connector the device registered. */
static int aux_of(struct drm_i915_private *i915, struct intel_dp *dp)
{
    return dp->present && dp->connector && dp->connector->dev == &i915->drm;
}

void intel_dp_aux_dev_register(struct drm_i915_private *i915)
{
    struct intel_display *d = &i915->display;
    struct intel_dp *dps[1 + sizeof(d->ddi) / sizeof(d->ddi[0])];
    int32_t rc;
    uint32_t i, n = 0;

    if (!(i915->drm.features & DRIVER_MODESET)) {
        return;
    }
    dps[n++] = &d->dp;
    for (i = 0; i < sizeof(d->ddi) / sizeof(d->ddi[0]); i++) {
        if (d->ddi[i].present && d->ddi[i].has_dp) {
            dps[n++] = &d->ddi[i].dp;
        }
    }
    for (i = 0; i < n; i++) {
        if (aux_of(i915, dps[i]) && (rc = aux_dev_add(dps[i])) != 0) {
            klog(KLOG_WARN, "i915", "aux: %s: no device: error %d", dps[i]->connector->name,
                 (int)rc);
        }
    }
}

void intel_dp_aux_dev_unregister(struct drm_i915_private *i915)
{
    uint32_t i;

    for (i = 0; i < AUX_DEV_MAX; i++) {
        if (aux_devs[i].dp && aux_devs[i].dp->i915 == i915) {
            aux_dev_remove(&aux_devs[i], 1);
        }
    }
}
