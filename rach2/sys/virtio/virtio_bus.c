/*
 * virtio_bus.c: the virtio bus in /sys, as Linux 6.12's drivers/virtio/
 * virtio.c shows it: a device virtioN below its transport's device, on
 * /sys/bus/virtio, bound to the driver that takes it.
 */

#include "virtio.h"

#include "../fs/synthfs.h"
#include "../fs/sysfs.h"
#include "../lib/kerrno.h"
#include "../lib/kstring.h"

/* The `arg` of a device's attributes: which file it is. */
#define VB_DEVICE   0
#define VB_VENDOR   1
#define VB_STATUS   2
#define VB_MODALIAS 3
#define VB_FEATURES 4

/* What a show writes: the bytes so far, which may pass `cap`, so that the
 * show returns how many it needs. */
struct vb_emit {
    char *buf;
    uint32_t cap, len;
};

/** Append the string `s` to `e`, as far as its room goes. */
static void vb_str(struct vb_emit *e, const char *s)
{
    for (; *s != '\0'; s++, e->len++) {
        if (e->len < e->cap) {
            e->buf[e->len] = *s;
        }
    }
}

/** Append `v` to `e` as `digits` hexadecimal digits, the letters in `upper`
 * case or not, as printf's "%0*x" and "%0*X". */
static void vb_hex(struct vb_emit *e, uint32_t v, int digits, int upper)
{
    const char *set = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    char one[2];

    one[1] = '\0';
    while (digits-- > 0) {
        one[0] = set[(v >> (digits * 4)) & 15u];
        vb_str(e, one);
    }
}

/** Append the modalias of `d`, "virtio:dNNNNNNNNvNNNNNNNN", as Linux's
 * modalias_show and virtio_uevent write it. */
static void vb_modalias(struct vb_emit *e, const struct virtio_dev *d)
{
    vb_str(e, "virtio:d");
    vb_hex(e, d->id, 8, 1);
    vb_str(e, "v");
    vb_hex(e, d->vendor, 8, 1);
}

/** A device's attribute `arg`, VB_*, as Linux 6.12's virtio.c shows it:
 * `device` and `vendor` as "0x%04x", `status` as "0x%08x" read from the
 * device, `modalias`, and `features` as a '0' or '1' for each of the 64
 * bits the driver took. */
static int32_t vb_show(void *obj, int arg, char *buf, uint32_t cap)
{
    struct virtio_dev *d = (struct virtio_dev *)obj;
    struct vb_emit e;
    int i;

    e.buf = buf;
    e.cap = cap;
    e.len = 0;
    switch (arg) {
    case VB_DEVICE:
        vb_str(&e, "0x");
        vb_hex(&e, d->id, 4, 0);
        break;
    case VB_VENDOR:
        vb_str(&e, "0x");
        vb_hex(&e, d->vendor, 4, 0);
        break;
    case VB_STATUS:
        vb_str(&e, "0x");
        vb_hex(&e, d->ops->get_status(d), 8, 0);
        break;
    case VB_MODALIAS:
        vb_modalias(&e, d);
        break;
    default:
        for (i = 0; i < 64; i++) {
            vb_str(&e, (d->features >> i) & 1u ? "1" : "0");
        }
        break;
    }
    vb_str(&e, "\n");
    return (int32_t)e.len;
}

/** A device's `uevent`: DRIVER while it is bound, as Linux's dev_uevent
 * writes it, and MODALIAS, as virtio_uevent adds it. */
static int32_t vb_show_uevent(void *obj, int arg, char *buf, uint32_t cap)
{
    struct virtio_dev *d = (struct virtio_dev *)obj;
    struct vb_emit e;

    (void)arg;
    e.buf = buf;
    e.cap = cap;
    e.len = 0;
    if (d->sys_driver) {
        vb_str(&e, "DRIVER=");
        vb_str(&e, d->sys_driver);
        vb_str(&e, "\n");
    }
    vb_str(&e, "MODALIAS=");
    vb_modalias(&e, d);
    vb_str(&e, "\n");
    return (int32_t)e.len;
}

/** The directory `name` below `parent`, made when it is not there: its
 * handle, or -errno as `sysfs_add_dir`. */
static int vb_dir_at(int parent, const char *name)
{
    int h;

    if (parent < 0) {
        return parent;
    }
    h = sysfs_node_child(parent, name, (int)k_strlen(name));
    return h > 0 ? h : sysfs_add_dir(parent, name);
}

/** /sys/bus/virtio, or its directory `sub` for a name, each made on its
 * first use: the handle, or -errno. */
static int vb_bus(const char *sub)
{
    int bus = vb_dir_at(sysfs_anchor(SYSFS_BUS, 0), "virtio");

    return sub ? vb_dir_at(bus, sub) : bus;
}

int virtio_bus_add(struct virtio_dev *d, int parent, uint16_t vendor)
{
    static const struct sysfs_attr attrs[] = {{"device", 0444u, vb_show, NULL},
                                              {"vendor", 0444u, vb_show, NULL},
                                              {"status", 0444u, vb_show, NULL},
                                              {"modalias", 0444u, vb_show, NULL},
                                              {"features", 0444u, vb_show, NULL}};
    int dir, bus, devices, drivers, link, h = 0, i;

    if (parent < 0) {
        return parent;
    }
    bus = vb_bus(NULL);
    devices = vb_bus("devices");
    drivers = vb_bus("drivers");
    if (bus < 0 || devices < 0 || drivers < 0) {
        return bus < 0 ? bus : devices < 0 ? devices : drivers;
    }
    d->vendor = vendor;
    dir = sysfs_add_dir(parent, d->name);
    if (dir < 0) {
        return dir;
    }
    for (i = 0; i < (int)(sizeof(attrs) / sizeof(attrs[0])) && h >= 0; i++) {
        h = sysfs_add_attr(dir, &attrs[i], d, i);
    }
    if (h >= 0) {
        h = sysfs_add_uevent(dir, vb_show_uevent, d, 0);
    }
    if (h >= 0) {
        h = sysfs_add_link(dir, "subsystem", bus);
    }
    link = h >= 0 ? sysfs_add_link(devices, d->name, dir) : h;
    if (link < 0) {
        sysfs_remove(dir);
        return link;
    }
    d->sys_dir = dir;
    d->sys_bus_link = link;
    sysfs_uevent(dir, "add", NULL);
    return 0;
}

void virtio_bus_remove(struct virtio_dev *d)
{
    if (!d->sys_dir) {
        return;
    }
    virtio_driver_unbind(d);
    sysfs_uevent(d->sys_dir, "remove", NULL);
    sysfs_remove(d->sys_bus_link);
    sysfs_remove(d->sys_dir);
    d->sys_dir = 0;
    d->sys_bus_link = 0;
}

int virtio_bus_dir(const struct virtio_dev *d)
{
    return d ? d->sys_dir : 0;
}

int virtio_driver_bind(struct virtio_dev *d, const char *driver)
{
    int dir, link, back;

    if (!d->sys_dir) {
        return 0;
    }
    if (d->sys_driver) {
        return -EBUSY;
    }
    dir = vb_dir_at(vb_bus("drivers"), driver);
    if (dir < 0) {
        return dir;
    }
    link = sysfs_add_link(d->sys_dir, "driver", dir);
    if (link < 0) {
        return link;
    }
    back = sysfs_add_link(dir, d->name, d->sys_dir);
    if (back < 0) {
        sysfs_remove(link);
        return back;
    }
    d->sys_driver = driver;
    d->sys_driver_link = link;
    d->sys_driver_back = back;
    return 0;
}

void virtio_driver_bound(struct virtio_dev *d)
{
    if (d->sys_dir && d->sys_driver && !d->sys_bound) {
        d->sys_bound = 1;
        sysfs_uevent(d->sys_dir, "bind", NULL);
    }
}

void virtio_driver_unbind(struct virtio_dev *d)
{
    if (!d->sys_dir || !d->sys_driver) {
        return;
    }
    sysfs_remove(d->sys_driver_link);
    sysfs_remove(d->sys_driver_back);
    d->sys_driver = NULL;
    d->sys_driver_link = 0;
    d->sys_driver_back = 0;
    if (d->sys_bound) {
        d->sys_bound = 0;
        sysfs_uevent(d->sys_dir, "unbind", NULL);
    }
}
