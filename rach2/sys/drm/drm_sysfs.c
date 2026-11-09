/*
 * drm_sysfs.c: a DRM device's entries in /sys, by which libdrm, Mesa's
 * loader and libudev find it, and its uevents, as Linux's drm_sysfs.c makes
 * them: the card and render nodes and the connectors below the device's
 * own directory, and their links in /sys/class/drm and /sys/dev/char.
 */

#include "drm.h"

#include "../fs/synthfs.h"
#include "../fs/sysfs.h"
#include "../klog.h"
#include "../lib/kerrno.h"
#include "../lib/kstring.h"

/*
 * The entries of one registered device, by its card index. Each is a
 * handle of sysfs.h, 0 for one not made. `platform` is the device's own
 * directory under /sys/devices/platform when this file made it, and
 * `drm` its `drm` directory, below the PCI function or that directory.
 */
struct drm_sysfs_dev {
    struct drm_device *dev;
    int platform, platform_link;
    int drm;
    int card, render;
    int class_card, class_render;
    int char_card, char_render;
    int conn[DRM_MAX_CONNECTORS];
    int class_conn[DRM_MAX_CONNECTORS];
};

static struct drm_sysfs_dev sysdevs[DRM_MAX_DEVICES];

/* What a show writes: the bytes so far, which may pass `cap`, so that the
 * show returns how many it needs. */
struct emit {
    char *buf;
    uint32_t cap, len;
};

/** Append the `n` bytes at `s` to `e`, as far as its room goes. */
static void emit_bytes(struct emit *e, const char *s, uint32_t n)
{
    uint32_t i;

    for (i = 0; i < n; i++, e->len++) {
        if (e->len < e->cap) {
            e->buf[e->len] = s[i];
        }
    }
}

/** Append the string `s` to `e`. */
static void emit_str(struct emit *e, const char *s)
{
    emit_bytes(e, s, (uint32_t)k_strlen(s));
}

/** Append the decimal `v` to `e`. */
static void emit_u32(struct emit *e, uint32_t v)
{
    char digits[11];
    int n = 0;

    do {
        digits[n++] = (char)('0' + v % 10u);
        v /= 10u;
    } while (v);
    while (n > 0) {
        emit_bytes(e, &digits[--n], 1);
    }
}

/** Start `e` on the show's buffer. */
static void emit_init(struct emit *e, char *buf, uint32_t cap)
{
    e->buf = buf;
    e->cap = cap;
    e->len = 0;
}

/* The `arg` of a node's attributes: which node of the device. */
#define NODE_CARD   0
#define NODE_RENDER 1

/** The minor of node `which` of `dev`. */
static uint32_t node_minor(const struct drm_device *dev, int which)
{
    return (uint32_t)dev->index + (which == NODE_RENDER ? DRM_MINOR_RENDER_BASE : 0u);
}

/** A node's `dev`: its major and minor, "226:0". */
static int32_t show_node_dev(void *obj, int arg, char *buf, uint32_t cap)
{
    struct emit e;

    emit_init(&e, buf, cap);
    emit_u32(&e, DRM_MAJOR);
    emit_str(&e, ":");
    emit_u32(&e, node_minor((struct drm_device *)obj, arg));
    emit_str(&e, "\n");
    return (int32_t)e.len;
}

/** A node's `uevent`: its numbers, its name in /dev and its type, as Linux's
 * device core and drm_sysfs.c give them. */
static int32_t show_node_uevent(void *obj, int arg, char *buf, uint32_t cap)
{
    struct drm_device *dev = (struct drm_device *)obj;
    struct emit e;

    emit_init(&e, buf, cap);
    emit_str(&e, "MAJOR=");
    emit_u32(&e, DRM_MAJOR);
    emit_str(&e, "\nMINOR=");
    emit_u32(&e, node_minor(dev, arg));
    emit_str(&e, arg == NODE_RENDER ? "\nDEVNAME=dri/renderD" : "\nDEVNAME=dri/card");
    emit_u32(&e, node_minor(dev, arg));
    emit_str(&e, "\nDEVTYPE=drm_minor\n");
    return (int32_t)e.len;
}

/** A platform device's `uevent`: its driver, the device's name less a
 * trailing ".N", and its module alias. */
static int32_t show_platform_uevent(void *obj, int arg, char *buf, uint32_t cap)
{
    const char *name = ((struct drm_device *)obj)->unique;
    uint32_t len = (uint32_t)k_strlen(name), i;
    struct emit e;

    (void)arg;
    for (i = len; i > 0 && name[i - 1] >= '0' && name[i - 1] <= '9'; i--) {
    }
    if (i > 0 && i < len && name[i - 1] == '.') {
        len = i - 1;
    }
    emit_init(&e, buf, cap);
    emit_str(&e, "DRIVER=");
    emit_bytes(&e, name, len);
    emit_str(&e, "\nMODALIAS=platform:");
    emit_bytes(&e, name, len);
    emit_str(&e, "\n");
    return (int32_t)e.len;
}

/* A connector's attributes, by their `arg`. */
#define CONN_STATUS  0
#define CONN_ENABLED 1
#define CONN_DPMS    2
#define CONN_MODES   3
#define CONN_EDID    4
#define CONN_ID      5
#define CONN_UEVENT  6

/** Whether the connector feeds an active CRTC, which its `dpms` says. */
static int connector_on(const struct drm_connector *c)
{
    const struct drm_crtc *crtc = c->state ? c->state->crtc : NULL;

    return crtc && crtc->state && crtc->state->active;
}

/** A connector's attribute `arg`, as Linux's drm_sysfs.c writes it:
 * `status` connected, disconnected or unknown; `enabled` while an encoder
 * drives it; `dpms` On or Off; `modes` one name to a line; `edid` the
 * sink's bytes; `connector_id` its object's ID; `uevent` its type. */
static int32_t show_connector(void *obj, int arg, char *buf, uint32_t cap)
{
    struct drm_connector *c = (struct drm_connector *)obj;
    const struct drm_display_mode *m;
    struct emit e;
    uint32_t n;

    emit_init(&e, buf, cap);
    switch (arg) {
    case CONN_STATUS:
        emit_str(&e, c->status == DRM_CONNECTOR_CONNECTED      ? "connected\n"
                     : c->status == DRM_CONNECTOR_DISCONNECTED ? "disconnected\n"
                                                               : "unknown\n");
        break;
    case CONN_ENABLED:
        emit_str(&e, c->encoder ? "enabled\n" : "disabled\n");
        break;
    case CONN_DPMS:
        emit_str(&e, connector_on(c) ? "On\n" : "Off\n");
        break;
    case CONN_MODES:
        for (m = c->modes; m; m = m->next) {
            for (n = 0; n < DRM_DISPLAY_MODE_LEN && m->info.name[n]; n++) {
            }
            emit_bytes(&e, m->info.name, n);
            emit_str(&e, "\n");
        }
        break;
    case CONN_EDID:
        if (c->edid && c->edid->data) {
            emit_bytes(&e, (const char *)c->edid->data, c->edid->length);
        }
        break;
    case CONN_ID:
        emit_u32(&e, c->base.id);
        emit_str(&e, "\n");
        break;
    default:
        emit_str(&e, "DEVTYPE=drm_connector\n");
        break;
    }
    return (int32_t)e.len;
}

/** Whether the `len` bytes at `s`, less a trailing newline, are `w`. */
static int word_is(const char *s, uint32_t len, const char *w)
{
    uint32_t i;

    if (len > 0 && s[len - 1] == '\n') {
        len--;
    }
    for (i = 0; i < len && w[i] == s[i]; i++) {
    }
    return i == len && w[i] == '\0';
}

/**
 * Take a write of a connector's `status`, as Linux's status_store: detect,
 * on, on-digital or off sets what the connector is forced to, which the
 * driver's `force` hook hears, and a change is told as a hotplug. -EINVAL
 * for any other word.
 */
static int32_t store_connector_status(void *obj, int arg, const char *buf, uint32_t len)
{
    struct drm_connector *c = (struct drm_connector *)obj;
    int force;

    (void)arg;
    if (word_is(buf, len, "detect")) {
        force = DRM_FORCE_UNSPECIFIED;
    } else if (word_is(buf, len, "on")) {
        force = DRM_FORCE_ON;
    } else if (word_is(buf, len, "on-digital")) {
        force = DRM_FORCE_ON_DIGITAL;
    } else if (word_is(buf, len, "off")) {
        force = DRM_FORCE_OFF;
    } else {
        return -EINVAL;
    }
    if (force != c->force) {
        c->force = force;
        if (c->funcs && c->funcs->force) {
            c->funcs->force(c);
        }
        drm_kms_hotplug_event(c->dev);
    }
    return (int32_t)len;
}

/** The record of `dev`, or NULL for an index out of range. */
static struct drm_sysfs_dev *sysdev_of(const struct drm_device *dev)
{
    return dev && dev->index >= 0 && dev->index < DRM_MAX_DEVICES ? &sysdevs[dev->index] : NULL;
}

/** Write "PREFIXN" into `out` of `cap` bytes. */
static void name_of(char *out, uint32_t cap, const char *prefix, uint32_t n)
{
    char digits[11];
    uint32_t len = 0;
    int d = 0;

    while (prefix[len] && len < cap - 1) {
        out[len] = prefix[len];
        len++;
    }
    do {
        digits[d++] = (char)('0' + n % 10u);
        n /= 10u;
    } while (n);
    while (d > 0 && len < cap - 1) {
        out[len++] = digits[--d];
    }
    out[len] = '\0';
}

static const struct sysfs_attr node_dev_attr = {"dev", 0444u, show_node_dev, NULL};

/** Add the node `which` of `dev` below the device's `drm` directory, with
 * its attributes and links, its link in /sys/class/drm and in /sys/dev/char:
 * the node's directory, or -errno. */
static int add_node(struct drm_sysfs_dev *sd, int parent, int which, int *class_link,
                    int *char_link)
{
    char name[24], devno[24];
    int dir, h;

    name_of(name, sizeof(name), which == NODE_RENDER ? "renderD" : "card",
            node_minor(sd->dev, which));
    dir = sysfs_add_dir(sd->drm, name);
    if (dir < 0) {
        return dir;
    }
    if ((h = sysfs_add_attr(dir, &node_dev_attr, sd->dev, which)) < 0 ||
        (h = sysfs_add_uevent(dir, show_node_uevent, sd->dev, which)) < 0 ||
        (h = sysfs_add_link(dir, "subsystem", sysfs_anchor(SYSFS_CLASS_DRM, 0))) < 0 ||
        (h = sysfs_add_link(dir, "device", parent)) < 0) {
        sysfs_remove(dir);
        return h;
    }
    *class_link = sysfs_add_link(sysfs_anchor(SYSFS_CLASS_DRM, 0), name, dir);
    name_of(devno, sizeof(devno), "226:", node_minor(sd->dev, which));
    *char_link = sysfs_add_link(sysfs_anchor(SYSFS_DEV_CHAR, 0), devno, dir);
    if (*class_link < 0 || *char_link < 0) {
        h = *class_link < 0 ? *class_link : *char_link;
        sysfs_remove(*class_link);
        sysfs_remove(*char_link);
        sysfs_remove(dir);
        *class_link = 0;
        *char_link = 0;
        return h;
    }
    return dir;
}

/** The directory of the device `dev` sits below: its PCI function's, or a
 * platform device's under /sys/devices/platform, made here when absent with
 * its uevent, its `subsystem` link and its link in /sys/bus/platform. The
 * handle, or -errno. */
static int device_parent(struct drm_sysfs_dev *sd)
{
    const struct drm_device *dev = sd->dev;
    const char *name = dev->unique[0] ? dev->unique : dev->driver->ident.name;
    int platform = sysfs_anchor(SYSFS_PLATFORM, 0), dir, h;

    if (dev->pci) {
        return sysfs_anchor(SYSFS_PCI_DEVICE,
                            dev->pci->bus << 8 | dev->pci->device << 3 | dev->pci->function);
    }
    if (platform < 0) {
        return platform;
    }
    dir = sysfs_node_child(platform, name, (int)k_strlen(name));
    if (dir > 0) {
        return dir;
    }
    dir = sysfs_add_dir(platform, name);
    if (dir < 0) {
        return dir;
    }
    if ((h = sysfs_add_uevent(dir, show_platform_uevent, sd->dev, 0)) < 0 ||
        (h = sysfs_add_link(dir, "subsystem", sysfs_anchor(SYSFS_BUS_PLATFORM, 0))) < 0) {
        sysfs_remove(dir);
        return h;
    }
    sd->platform = dir;
    h = sysfs_add_link(sysfs_anchor(SYSFS_PLATFORM_LINKS, 0), name, dir);
    sd->platform_link = h > 0 ? h : 0;
    return dir;
}

int32_t drm_sysfs_register(struct drm_device *dev)
{
    struct drm_sysfs_dev *sd = sysdev_of(dev);
    int parent, h;
    uint32_t i;

    if (!sd || sd->dev) {
        return -EINVAL;
    }
    k_memset(sd, 0, sizeof(*sd));
    sd->dev = dev;
    parent = device_parent(sd);
    if (parent < 0) {
        sd->dev = NULL;
        return parent;
    }
    sd->drm = sysfs_node_child(parent, "drm", 3);
    if (sd->drm <= 0) {
        sd->drm = sysfs_add_dir(parent, "drm");
    }
    h = sd->drm > 0 ? add_node(sd, parent, NODE_CARD, &sd->class_card, &sd->char_card) : sd->drm;
    if (h < 0) {
        drm_sysfs_unregister(dev);
        return h;
    }
    sd->card = h;
    if (drm_core_check_feature(dev, DRIVER_RENDER)) {
        h = add_node(sd, parent, NODE_RENDER, &sd->class_render, &sd->char_render);
        if (h < 0) {
            drm_sysfs_unregister(dev);
            return h;
        }
        sd->render = h;
    }
    if (sd->platform) {
        sysfs_uevent(sd->platform, "add", NULL);
    }
    sysfs_uevent(sd->card, "add", NULL);
    if (sd->render) {
        sysfs_uevent(sd->render, "add", NULL);
    }
    for (i = 0; i < dev->mode_config.num_connector && i < DRM_MAX_CONNECTORS; i++) {
        h = drm_sysfs_connector_add(dev->mode_config.connectors[i]);
        if (h < 0) {
            klog(KLOG_WARN, "drm", "sysfs: %s: error %d", dev->mode_config.connectors[i]->name,
                 (int)h);
        }
    }
    return 0;
}

int32_t drm_sysfs_connector_add(struct drm_connector *connector)
{
    static const struct sysfs_attr attrs[] = {
        {"status", 0644u, show_connector, store_connector_status},
        {"enabled", 0444u, show_connector, NULL},
        {"dpms", 0444u, show_connector, NULL},
        {"modes", 0444u, show_connector, NULL},
        {"edid", 0444u, show_connector, NULL},
        {"connector_id", 0444u, show_connector, NULL}};
    struct drm_sysfs_dev *sd = connector ? sysdev_of(connector->dev) : NULL;
    char name[SYSFS_NAME_MAX];
    uint32_t i, at = connector ? connector->index : DRM_MAX_CONNECTORS;
    int dir, h = 0, class_drm = sysfs_anchor(SYSFS_CLASS_DRM, 0);

    if (!sd || sd->dev != connector->dev || !sd->card || at >= DRM_MAX_CONNECTORS) {
        return -EINVAL;
    }
    if (sd->conn[at]) {
        return 0;
    }
    name_of(name, sizeof(name), "card", (uint32_t)sd->dev->index);
    i = (uint32_t)k_strlen(name);
    if (i + 1 + (uint32_t)k_strlen(connector->name) >= sizeof(name)) {
        return -ENAMETOOLONG;
    }
    name[i++] = '-';
    k_strncpy(name + i, connector->name, sizeof(name) - 1 - i);
    name[sizeof(name) - 1] = '\0';
    dir = sysfs_add_dir(sd->card, name);
    if (dir < 0) {
        return dir;
    }
    for (i = 0; i < sizeof(attrs) / sizeof(attrs[0]) && h >= 0; i++) {
        h = sysfs_add_attr(dir, &attrs[i], connector, (int)i);
    }
    if (h >= 0) {
        h = sysfs_add_uevent(dir, show_connector, connector, CONN_UEVENT);
    }
    if (h >= 0) {
        h = sysfs_add_link(dir, "subsystem", class_drm);
    }
    if (h >= 0) {
        h = sysfs_add_link(dir, "device", sd->card);
    }
    if (h >= 0) {
        h = sysfs_add_link(class_drm, name, dir);
    }
    if (h < 0) {
        sysfs_remove(dir);
        return h;
    }
    sd->conn[at] = dir;
    sd->class_conn[at] = h;
    sysfs_uevent(dir, "add", NULL);
    return 0;
}

void drm_sysfs_connector_remove(struct drm_connector *connector)
{
    struct drm_sysfs_dev *sd = connector ? sysdev_of(connector->dev) : NULL;
    uint32_t at = connector ? connector->index : DRM_MAX_CONNECTORS;

    if (!sd || sd->dev != connector->dev || at >= DRM_MAX_CONNECTORS || !sd->conn[at]) {
        return;
    }
    sysfs_uevent(sd->conn[at], "remove", NULL);
    sysfs_remove(sd->class_conn[at]);
    sysfs_remove(sd->conn[at]);
    sd->conn[at] = 0;
    sd->class_conn[at] = 0;
}

void drm_sysfs_unregister(struct drm_device *dev)
{
    struct drm_sysfs_dev *sd = sysdev_of(dev);
    uint32_t i;

    if (!sd || sd->dev != dev) {
        return;
    }
    for (i = 0; i < DRM_MAX_CONNECTORS; i++) {
        if (sd->conn[i]) {
            sysfs_uevent(sd->conn[i], "remove", NULL);
            sysfs_remove(sd->class_conn[i]);
            sysfs_remove(sd->conn[i]);
        }
    }
    if (sd->render) {
        sysfs_uevent(sd->render, "remove", NULL);
    }
    if (sd->card) {
        sysfs_uevent(sd->card, "remove", NULL);
    }
    sysfs_remove(sd->class_render);
    sysfs_remove(sd->char_render);
    sysfs_remove(sd->render);
    sysfs_remove(sd->class_card);
    sysfs_remove(sd->char_card);
    sysfs_remove(sd->card);
    if (sd->drm > 0 && sysfs_node_count(sd->drm) == 0) {
        sysfs_remove(sd->drm);
    }
    if (sd->platform) {
        sysfs_uevent(sd->platform, "remove", NULL);
        sysfs_remove(sd->platform_link);
        sysfs_remove(sd->platform);
    }
    k_memset(sd, 0, sizeof(*sd));
}

int drm_sysfs_card_dir(const struct drm_device *dev)
{
    struct drm_sysfs_dev *sd = sysdev_of(dev);

    return sd && sd->dev == dev ? sd->card : 0;
}

void drm_kms_hotplug_event(struct drm_device *dev)
{
    struct drm_sysfs_dev *sd = sysdev_of(dev);

    if (sd && sd->dev == dev && sd->card) {
        sysfs_uevent(sd->card, "change", "HOTPLUG=1\n");
    }
}

void drm_sysfs_connector_hotplug_event(struct drm_connector *connector)
{
    struct drm_sysfs_dev *sd = connector ? sysdev_of(connector->dev) : NULL;
    char env[40];

    if (!sd || sd->dev != connector->dev || !sd->card) {
        return;
    }
    name_of(env, sizeof(env), "HOTPLUG=1\nCONNECTOR=", connector->base.id);
    k_strncpy(env + k_strlen(env), "\n", 2);
    sysfs_uevent(sd->card, "change", env);
}
