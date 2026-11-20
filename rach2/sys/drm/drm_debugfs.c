/*
 * drm_debugfs.c: a DRM device's files in debugfs, as Linux 6.12's
 * drm_debugfs.c and drm_debugfs_crc.c make them, by which IGT reads CRCs
 * and overrides a connector's EDID.
 */

#include "drm.h"

#include "../fs/debugfs.h"
#include "../io/poll.h"
#include "../klog.h"
#include "../lib/kerrno.h"
#include "../lib/kstring.h"
#include "../mm/kheap.h"
#include "../mm/usercopy.h"
#include "../proc/process.h"
#include "../timer.h"

/* The longest debugfs path of a device's file: "dri/", the directory's
 * name, a connector's name and "edid_override", with slashes and a NUL. */
#define DRM_DEBUGFS_PATH 128

/* The longest line of crc/data, ten values, Linux's LINE_LEN(10). */
#define DRM_CRC_LINE_MAX (10u + 11u * DRM_MAX_CRC_NR + 1u + 1u)

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

/** Append the string `s` right-aligned in `width` columns, as "%*s". */
static void emit_pad(struct emit *e, const char *s, uint32_t width)
{
    uint32_t n = (uint32_t)k_strlen(s);

    while (n < width) {
        emit_bytes(e, " ", 1);
        width--;
    }
    emit_str(e, s);
}

/**
 * Append `v` in decimal, negative when `neg`, right-aligned in `width`
 * columns, padded with zeros when `zero`, and with a sign when `plus`, as
 * "%*d", "%0*u" and "%+d" write it.
 */
static void emit_num(struct emit *e, uint64_t v, int neg, uint32_t width, int zero, int plus)
{
    char digits[24];
    char out[28];
    uint32_t n = 0, k = 0, rem;

    do {
        v = tick_div(v, 10, &rem);
        digits[n++] = (char)('0' + rem);
    } while (v);
    if (neg || plus) {
        out[k++] = neg ? '-' : '+';
    }
    while (zero && k + n < width) {
        out[k++] = '0';
    }
    while (n > 0) {
        out[k++] = digits[--n];
    }
    out[k] = '\0';
    emit_pad(e, out, width);
}

/** Append the unsigned `v` in decimal, as "%*u". */
static void emit_u(struct emit *e, uint64_t v, uint32_t width)
{
    emit_num(e, v, 0, width, 0, 0);
}

/** Append the signed `v` in decimal, as "%*d", or "%+d" when `plus`. */
static void emit_d(struct emit *e, int32_t v, uint32_t width, int plus)
{
    emit_num(e, v < 0 ? (uint64_t)(-(int64_t)v) : (uint64_t)v, v < 0, width, 0, plus);
}

/** Append `v` in lowercase hex, at least `digits` of them, as "%0*x". */
static void emit_hex(struct emit *e, uint64_t v, uint32_t digits)
{
    char out[17];
    int n = 0, i;

    do {
        out[n++] = "0123456789abcdef"[v & 0xf];
        v >>= 4;
    } while (v);
    while ((uint32_t)n < digits && n < 16) {
        out[n++] = '0';
    }
    for (i = n - 1; i >= 0; i--) {
        emit_bytes(e, &out[i], 1);
    }
}

/** Append `indent` tabs, Linux's drm_printf_indent. */
static void emit_indent(struct emit *e, uint32_t indent)
{
    while (indent--) {
        emit_bytes(e, "\t", 1);
    }
}

/** Whether the strings `a` and `b` are the same. */
static int same(const char *a, const char *b)
{
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

/** The length `e` needs, as a show returns it. */
static int32_t emit_end(const struct emit *e)
{
    return (int32_t)e->len;
}

/* ------------------------------------------------------------------------
 * The device's files
 */

/** The name of `dev`'s directory, Linux's dev_name of its parent: the PCI
 * function as "0000:BB:DD.F", or the platform device's name. */
static const char *dir_name(const struct drm_device *dev)
{
    const char *u = dev->unique;

    return dev->pci && u[0] == 'p' && u[1] == 'c' && u[2] == 'i' && u[3] == ':' ? u + 4 : u;
}

/** Write "dri/DIR", "/" and `rest`, if any, into `path`. */
static void dev_path(char *path, const struct drm_device *dev, const char *rest)
{
    uint32_t at = 0;

    k_strncpy(path, "dri/", DRM_DEBUGFS_PATH);
    at = (uint32_t)k_strlen(path);
    k_strncpy(path + at, dir_name(dev), DRM_DEBUGFS_PATH - at);
    at = (uint32_t)k_strlen(path);
    if (rest && at + 1u + k_strlen(rest) < DRM_DEBUGFS_PATH) {
        path[at++] = '/';
        k_strncpy(path + at, rest, DRM_DEBUGFS_PATH - at);
    }
    path[DRM_DEBUGFS_PATH - 1] = '\0';
}

/** Whether the master record of `dev`'s current master has its bus name,
 * which a file under that record set with SET_VERSION. */
static int master_has_busid(const struct drm_device *dev)
{
    return dev->master && dev->master->set_busid;
}

/** `name`, Linux's drm_name_info: the driver, the parent device, the
 * master's bus name once SET_VERSION gave it, and the unique name. */
static int32_t name_show(void *data, char *buf, uint32_t cap)
{
    const struct drm_device *dev = (const struct drm_device *)data;
    struct emit e;

    e.buf = buf;
    e.cap = cap;
    e.len = 0;
    emit_str(&e, dev->driver->ident.name);
    emit_str(&e, " dev=");
    emit_str(&e, dir_name(dev));
    if (master_has_busid(dev)) {
        emit_str(&e, " master=");
        emit_str(&e, dev->unique);
    }
    emit_str(&e, " unique=");
    emit_str(&e, dir_name(dev));
    emit_str(&e, "\n");
    return emit_end(&e);
}

/** `clients`, Linux's drm_clients_info: one line for each open file,
 * oldest first, with its process, node, roles, uid and magic. */
static int32_t clients_show(void *data, char *buf, uint32_t cap)
{
    const struct drm_device *dev = (const struct drm_device *)data;
    const struct drm_file *f;
    struct emit e;
    uint32_t count = 0, i, k;

    e.buf = buf;
    e.cap = cap;
    e.len = 0;
    emit_pad(&e, "command", 20);
    emit_str(&e, " ");
    emit_pad(&e, "tgid", 5);
    emit_str(&e, " ");
    emit_pad(&e, "dev", 3);
    emit_str(&e, " master a ");
    emit_pad(&e, "uid", 5);
    emit_str(&e, " ");
    emit_pad(&e, "magic", 10);
    emit_str(&e, "\n");
    for (f = dev->files; f; f = f->dev_next) {
        count++;
    }
    /* The list is newest first, and Linux shows the oldest first. */
    for (i = count; i > 0; i--) {
        const process_t *p;
        char comm[16];
        int minor;

        f = dev->files;
        for (k = 1; k < i; k++) {
            f = f->dev_next;
        }
        p = get_process(f->pid);
        if (p) {
            k_memcpy(comm, p->comm, sizeof(comm) - 1u);
            comm[sizeof(comm) - 1u] = '\0';
        } else {
            k_strncpy(comm, "<unknown>", sizeof(comm));
        }
        minor = f->minor_type == DRM_MINOR_RENDER ? DRM_MINOR_RENDER_BASE + dev->index : dev->index;
        emit_pad(&e, comm, 20);
        emit_str(&e, " ");
        emit_d(&e, (int32_t)f->pid, 5, 0);
        emit_str(&e, " ");
        emit_d(&e, minor, 3, 0);
        emit_str(&e, drm_is_current_master(f) ? "   y" : "   n");
        emit_str(&e, f->authenticated ? "    y " : "    n ");
        emit_d(&e, p ? (int32_t)p->euid : 0, 5, 0);
        emit_str(&e, " ");
        emit_u(&e, f->magic, 10);
        emit_str(&e, "\n");
    }
    return emit_end(&e);
}

/** `gem_names`, Linux's drm_gem_name_info: each buffer object with a flink
 * name, by name, with its size, handles and references, read under the
 * device's `object_lock` as Linux reads them under `object_name_lock`. */
static int32_t gem_names_show(void *data, char *buf, uint32_t cap)
{
    struct drm_device *dev = (struct drm_device *)data;
    const struct drm_gem_object *obj, *next;
    struct emit e;
    uint32_t last = 0, flags;

    e.buf = buf;
    e.cap = cap;
    e.len = 0;
    emit_str(&e, "  name     size handles refcount\n");
    flags = spin_lock_irqsave(&dev->object_lock);
    for (;;) {
        next = NULL;
        for (obj = dev->objects; obj; obj = obj->dev_next) {
            if (obj->name > last && (!next || obj->name < next->name)) {
                next = obj;
            }
        }
        if (!next) {
            break;
        }
        emit_u(&e, next->name, 6);
        emit_str(&e, " ");
        emit_u(&e, next->size, 8);
        emit_str(&e, " ");
        emit_u(&e, next->handle_count, 7);
        emit_str(&e, " ");
        emit_u(&e, next->refs, 8);
        emit_str(&e, "\n");
        last = next->name;
    }
    spin_unlock_irqrestore(&dev->object_lock, flags);
    return emit_end(&e);
}

/** Append a buffer object at `indent`, Linux's drm_gem_print_info. */
static void gem_info(struct emit *e, uint32_t indent, const struct drm_gem_object *obj)
{
    emit_indent(e, indent);
    emit_str(e, "name=");
    emit_u(e, obj->name, 0);
    emit_str(e, "\n");
    emit_indent(e, indent);
    emit_str(e, "refcount=");
    emit_u(e, obj->refs, 0);
    emit_str(e, "\n");
    emit_indent(e, indent);
    emit_str(e, "start=");
    emit_hex(e, obj->mmap_offset >> PAGE_SHIFT, 8);
    emit_str(e, "\n");
    emit_indent(e, indent);
    emit_str(e, "size=");
    emit_u(e, obj->size, 0);
    emit_str(e, "\n");
    emit_indent(e, indent);
    emit_str(e, obj->import ? "imported=yes\n" : "imported=no\n");
}

/** Append a fourcc as Linux's %p4cc: its four characters, a dot for one
 * that does not print, its byte order and its number. */
static void fourcc(struct emit *e, uint32_t format)
{
    uint32_t i;
    char c;

    for (i = 0; i < 4; i++) {
        c = (char)((format & 0x7fffffffu) >> (8 * i));
        emit_bytes(e, c >= 0x20 && c < 0x7f ? &c : ".", 1);
    }
    emit_str(e, format & 0x80000000u ? " big-endian (0x" : " little-endian (0x");
    emit_hex(e, format, 8);
    emit_str(e, ")");
}

/** Append the "%s=%u\n" line `key` `v` at `indent`. */
static void key_u(struct emit *e, uint32_t indent, const char *key, uint32_t index, uint32_t v)
{
    emit_indent(e, indent);
    emit_str(e, key);
    emit_str(e, "[");
    emit_u(e, index, 0);
    emit_str(e, "]=");
    emit_u(e, v, 0);
    emit_str(e, "\n");
}

/** Append a framebuffer at `indent`, Linux's drm_framebuffer_print_info. */
static void fb_info(struct emit *e, uint32_t indent, const struct drm_framebuffer *fb)
{
    uint32_t i, planes = fb->info ? fb->info->num_planes : 1;

    emit_indent(e, indent);
    emit_str(e, "allocated by = ");
    emit_str(e, fb->comm);
    emit_str(e, "\n");
    emit_indent(e, indent);
    emit_str(e, "refcount=");
    emit_u(e, fb->base.refs, 0);
    emit_str(e, "\n");
    emit_indent(e, indent);
    emit_str(e, "format=");
    fourcc(e, fb->format);
    emit_str(e, "\n");
    emit_indent(e, indent);
    emit_str(e, "modifier=0x");
    emit_hex(e, fb->modifier, 0);
    emit_str(e, "\n");
    emit_indent(e, indent);
    emit_str(e, "size=");
    emit_u(e, fb->width, 0);
    emit_str(e, "x");
    emit_u(e, fb->height, 0);
    emit_str(e, "\n");
    emit_indent(e, indent);
    emit_str(e, "layers:\n");
    for (i = 0; i < planes && i < 4; i++) {
        uint32_t hsub = i && fb->info && fb->info->hsub ? fb->info->hsub : 1;
        uint32_t vsub = i && fb->info && fb->info->vsub ? fb->info->vsub : 1;

        emit_indent(e, indent + 1);
        emit_str(e, "size[");
        emit_u(e, i, 0);
        emit_str(e, "]=");
        emit_u(e, (fb->width + hsub - 1) / hsub, 0);
        emit_str(e, "x");
        emit_u(e, (fb->height + vsub - 1) / vsub, 0);
        emit_str(e, "\n");
        key_u(e, indent + 1, "pitch", i, fb->pitches[i]);
        key_u(e, indent + 1, "offset", i, fb->offsets[i]);
        emit_indent(e, indent + 1);
        emit_str(e, "obj[");
        emit_u(e, i, 0);
        emit_str(e, fb->obj[i] ? "]:\n" : "]:(null)\n");
        if (fb->obj[i]) {
            gem_info(e, indent + 2, fb->obj[i]);
        }
    }
}

/** `framebuffer`, Linux's drm_framebuffer_info: every framebuffer of the
 * device, newest first, as Linux's list keeps them. */
static int32_t framebuffer_show(void *data, char *buf, uint32_t cap)
{
    struct drm_device *dev = (struct drm_device *)data;
    const struct drm_framebuffer *fb;
    struct emit e;
    uint32_t count = 0, i, k;

    e.buf = buf;
    e.cap = cap;
    e.len = 0;
    /* The list is the mode lock's, as Linux's is fb_lock's. */
    drm_modeset_lock_nointr(dev);
    for (fb = dev->mode_config.fbs; fb; fb = fb->next) {
        count++;
    }
    for (i = count; i > 0; i--) {
        fb = dev->mode_config.fbs;
        for (k = 1; k < i; k++) {
            fb = fb->next;
        }
        emit_str(&e, "framebuffer[");
        emit_u(&e, fb->base.id, 0);
        emit_str(&e, "]:\n");
        fb_info(&e, 1, fb);
    }
    drm_modeset_unlock(dev);
    return emit_end(&e);
}

/** Append the 16.16 fixed-point `v` as Linux's DRM_RECT_FP_FMT does one
 * value: whole part, a dot and six digits of the fraction. */
static void fixed(struct emit *e, int32_t v, int plus)
{
    emit_d(e, v >> 16, 0, plus);
    emit_str(e, ".");
    emit_num(e, (((uint32_t)v & 0xffffu) * 15625u) >> 10, 0, 6, 1, 0);
}

/** Append "\tKEY=V\n" for the signed `v`. */
static void line_d(struct emit *e, const char *key, int32_t v)
{
    emit_str(e, "\t");
    emit_str(e, key);
    emit_str(e, "=");
    emit_d(e, v, 0, 0);
    emit_str(e, "\n");
}

/** Append "\tKEY=V\n" for `v` in hex. */
static void line_x(struct emit *e, const char *key, uint32_t v)
{
    emit_str(e, "\t");
    emit_str(e, key);
    emit_str(e, "=");
    emit_hex(e, v, 0);
    emit_str(e, "\n");
}

/** Append a plane's state, Linux's drm_atomic_plane_print_state. */
static void plane_state(struct emit *e, const struct drm_plane *plane)
{
    const struct drm_plane_state *s = plane->state;

    emit_str(e, "plane[");
    emit_u(e, plane->base.id, 0);
    emit_str(e, "]: plane-");
    emit_u(e, plane->index, 0);
    emit_str(e, "\n\tcrtc=");
    emit_str(e, s->crtc ? s->crtc->name : "(null)");
    emit_str(e, "\n\tfb=");
    emit_u(e, s->fb ? s->fb->base.id : 0, 0);
    emit_str(e, "\n");
    if (s->fb) {
        fb_info(e, 2, s->fb);
    }
    emit_str(e, "\tcrtc-pos=");
    emit_d(e, (int32_t)s->crtc_w, 0, 0);
    emit_str(e, "x");
    emit_d(e, (int32_t)s->crtc_h, 0, 0);
    emit_d(e, s->crtc_x, 0, 1);
    emit_d(e, s->crtc_y, 0, 1);
    emit_str(e, "\n\tsrc-pos=");
    fixed(e, (int32_t)s->src_w, 0);
    emit_str(e, "x");
    fixed(e, (int32_t)s->src_h, 0);
    fixed(e, (int32_t)s->src_x, 1);
    fixed(e, (int32_t)s->src_y, 1);
    emit_str(e, "\n");
    line_x(e, "rotation", s->rotation);
    line_x(e, "normalized-zpos", s->zpos);
    emit_str(e, "\tcolor-encoding=ITU-R BT.601 YCbCr\n");
    emit_str(e, "\tcolor-range=YCbCr limited range\n");
    emit_str(e, "\tcolor_mgmt_changed=0\n");
}

/** Append a CRTC's state, Linux's drm_atomic_crtc_print_state; its
 * encoders are those of the connectors routed to it. */
static void crtc_state(struct emit *e, const struct drm_device *dev, const struct drm_crtc *crtc)
{
    const struct drm_crtc_state *s = crtc->state;
    const struct drm_mode_modeinfo *m = &s->mode;
    uint32_t encoders = 0, i;
    char name[sizeof(m->name) + 1];

    for (i = 0; i < dev->mode_config.num_connector && i < DRM_MAX_CONNECTORS; i++) {
        const struct drm_connector_state *cs = dev->mode_config.connectors[i]->state;

        if (cs && cs->crtc == crtc && cs->best_encoder) {
            encoders |= 1u << cs->best_encoder->index;
        }
    }
    emit_str(e, "crtc[");
    emit_u(e, crtc->base.id, 0);
    emit_str(e, "]: ");
    emit_str(e, crtc->name);
    emit_str(e, "\n");
    line_d(e, "enable", s->enable);
    line_d(e, "active", s->active);
    line_d(e, "self_refresh_active", 0);
    line_d(e, "planes_changed", s->planes_changed);
    line_d(e, "mode_changed", s->mode_changed);
    line_d(e, "active_changed", s->active_changed);
    line_d(e, "connectors_changed", s->connectors_changed);
    line_d(e, "color_mgmt_changed", s->color_mgmt_changed);
    line_x(e, "plane_mask", s->plane_mask);
    line_x(e, "connector_mask", s->connector_mask);
    line_x(e, "encoder_mask", encoders);
    k_memcpy(name, m->name, sizeof(m->name));
    name[sizeof(m->name)] = '\0';
    emit_str(e, "\tmode: \"");
    emit_str(e, name);
    emit_str(e, "\": ");
    emit_u(e, drm_mode_vrefresh(m), 0);
    emit_str(e, " ");
    emit_u(e, m->clock, 0);
    emit_str(e, " ");
    emit_u(e, m->hdisplay, 0);
    emit_str(e, " ");
    emit_u(e, m->hsync_start, 0);
    emit_str(e, " ");
    emit_u(e, m->hsync_end, 0);
    emit_str(e, " ");
    emit_u(e, m->htotal, 0);
    emit_str(e, " ");
    emit_u(e, m->vdisplay, 0);
    emit_str(e, " ");
    emit_u(e, m->vsync_start, 0);
    emit_str(e, " ");
    emit_u(e, m->vsync_end, 0);
    emit_str(e, " ");
    emit_u(e, m->vtotal, 0);
    emit_str(e, " 0x");
    emit_hex(e, m->type, 0);
    emit_str(e, " 0x");
    emit_hex(e, m->flags, 0);
    emit_str(e, "\n");
}

/** Append a connector's state, Linux's drm_atomic_connector_print_state,
 * with the HDMI connector's defaults, which no driver here changes. */
static void connector_state(struct emit *e, const struct drm_connector *connector)
{
    const struct drm_connector_state *s = connector->state;

    emit_str(e, "connector[");
    emit_u(e, connector->base.id, 0);
    emit_str(e, "]: ");
    emit_str(e, connector->name);
    emit_str(e, "\n\tcrtc=");
    emit_str(e, s && s->crtc ? s->crtc->name : "(null)");
    emit_str(e, "\n\tself_refresh_aware=0\n\tmax_requested_bpc=0\n\tcolorspace=Default\n");
    if (connector->connector_type == DRM_MODE_CONNECTOR_HDMIA ||
        connector->connector_type == DRM_MODE_CONNECTOR_HDMIB) {
        emit_str(e, "\tbroadcast_rgb=Automatic\n\tis_limited_range=n\n\toutput_bpc=0\n"
                    "\toutput_format=RGB\n\ttmds_char_rate=0\n");
    }
}

/** `state`, Linux's drm_state_info for an atomic driver: every plane's,
 * CRTC's and connector's state, in the order of their indexes. */
static int32_t state_show(void *data, char *buf, uint32_t cap)
{
    struct drm_device *dev = (struct drm_device *)data;
    const struct drm_mode_config *mc = &dev->mode_config;
    struct emit e;
    uint32_t i;

    e.buf = buf;
    e.cap = cap;
    e.len = 0;
    /* The committed states, under the mode lock, as Linux's
     * __drm_state_dump takes each object's modeset lock (drm_atomic.c:
     * 1785-1815). */
    drm_modeset_lock_nointr(dev);
    for (i = 0; i < mc->num_plane && i < DRM_MAX_PLANES; i++) {
        if (mc->planes[i]->state) {
            plane_state(&e, mc->planes[i]);
        }
    }
    for (i = 0; i < mc->num_crtc && i < DRM_MAX_CRTCS; i++) {
        if (mc->crtcs[i]->state) {
            crtc_state(&e, dev, mc->crtcs[i]);
        }
    }
    for (i = 0; i < mc->num_connector && i < DRM_MAX_CONNECTORS; i++) {
        connector_state(&e, mc->connectors[i]);
    }
    drm_modeset_unlock(dev);
    return emit_end(&e);
}

/** An encoder's `bridges`: the bridges in its chain, which no driver here
 * has, so nothing. */
static int32_t bridges_show(void *data, char *buf, uint32_t cap)
{
    (void)data;
    (void)buf;
    (void)cap;
    return 0;
}

static const struct debugfs_text_ops name_ops = {name_show, NULL};
static const struct debugfs_text_ops clients_ops = {clients_show, NULL};
static const struct debugfs_text_ops gem_names_ops = {gem_names_show, NULL};
static const struct debugfs_text_ops framebuffer_ops = {framebuffer_show, NULL};
static const struct debugfs_text_ops state_ops = {state_show, NULL};
static const struct debugfs_text_ops bridges_ops = {bridges_show, NULL};

/* ------------------------------------------------------------------------
 * The CRC files, Linux's drm_debugfs_crc.c, on drm_vblank.c's ring
 */

/** crc/control's text, Linux's crc_control_show: each source the driver
 * names and takes, the chosen one starred, or the chosen one alone when it
 * names none; nothing for a driver that cannot list them. */
static int32_t crc_control_show(void *data, char *buf, uint32_t cap)
{
    struct drm_crtc *crtc = (struct drm_crtc *)data;
    const char *const *sources;
    uint32_t count = 0, values, i;
    struct emit e;

    e.buf = buf;
    e.cap = cap;
    e.len = 0;
    if (!crtc->funcs || !crtc->funcs->get_crc_sources) {
        return 0;
    }
    sources = crtc->funcs->get_crc_sources(crtc, &count);
    if (count == 0 || !sources) {
        emit_str(&e, crtc->crc.source);
        emit_str(&e, "*\n");
        return emit_end(&e);
    }
    for (i = 0; i < count; i++) {
        if (crtc->funcs->verify_crc_source &&
            crtc->funcs->verify_crc_source(crtc, sources[i], &values) == 0) {
            emit_str(&e, sources[i]);
            emit_str(&e, same(sources[i], crtc->crc.source) ? "*\n" : "\n");
        }
    }
    return emit_end(&e);
}

/** crc/control's write, Linux's crc_control_write: the source for the
 * next open of crc/data (`drm_crtc_crc_set_source`). */
static int32_t crc_control_store(void *data, const char __user *buf, uint32_t len)
{
    struct drm_crtc *crtc = (struct drm_crtc *)data;
    char *copy;
    int32_t rc;

    if (len == 0) {
        return 0;
    }
    if (len > PAGE_SIZE - 1) {
        return -E2BIG;
    }
    copy = kmalloc(len);
    if (!copy) {
        return -ENOMEM;
    }
    rc = copy_from_user(copy, buf, len) != 0 ? -EFAULT : drm_crtc_crc_set_source(crtc, copy, len);
    kfree(copy);
    return rc;
}

static const struct debugfs_text_ops crc_control_ops = {crc_control_show, crc_control_store};

/** `vblank_backwards`'s text: how many times the CRTC's vblanks were stamped
 * before a vblank stamped earlier, `drm_vblank`'s `backwards`, which is 0
 * unless an event or a WAIT_VBLANK reply went back in time. */
static int32_t vblank_backwards_show(void *data, char *buf, uint32_t cap)
{
    const struct drm_crtc *crtc = (const struct drm_crtc *)data;
    struct emit e;

    e.buf = buf;
    e.cap = cap;
    e.len = 0;
    emit_u(&e, crtc->vblank.backwards, 0);
    emit_str(&e, "\n");
    return emit_end(&e);
}

static const struct debugfs_text_ops vblank_backwards_ops = {vblank_backwards_show, NULL};

/** crc/data's open: start the CRTC's source (`drm_crtc_crc_open`), which
 * may sleep for the commit lock. */
static int32_t crc_data_open(fd_entry_t *e, int flags)
{
    (void)flags;
    return drm_crtc_crc_open((struct drm_crtc *)e->priv);
}

/** crc/data's read: one entry as a line with its NUL, waiting for one
 * unless the description is non-blocking (`drm_crtc_crc_read`). */
static int32_t crc_data_read(fd_entry_t *e, char __user *buf, uint32_t count)
{
    struct drm_crtc *crtc = (struct drm_crtc *)e->priv;
    int nonblock = (e->ofd->status_flags & OFD_NONBLOCK) != 0;
    char line[DRM_CRC_LINE_MAX];
    int32_t n;

    n = drm_crtc_crc_read(crtc, line, count < sizeof(line) ? count : sizeof(line), nonblock);
    if (n > 0 && copy_to_user(buf, line, (uint32_t)n) != 0) {
        return -EFAULT;
    }
    return n;
}

/** crc/data's poll, Linux's crtc_crc_poll: POLLIN and POLLRDNORM while an
 * entry waits. It runs without the big lock (FILE_OPS_POLL_UNLOCKED), as
 * Linux's does, so that a reader's poll does not wait for a holder whose
 * virtual processor the host has stopped. */
static short crc_data_poll(fd_entry_t *e, short events, struct wait_queue **wq)
{
    struct drm_crtc *crtc = (struct drm_crtc *)e->priv;

    *wq = &crtc->crc.wait;
    return (short)(events & (drm_crtc_crc_ready(crtc) ? POLLIN | POLLRDNORM : 0));
}

/** crc/data's last close: stop the source. */
static void crc_data_close(fd_entry_t *e)
{
    drm_crtc_crc_release((struct drm_crtc *)e->priv);
}

/** crc/data has no position, as on Linux, where it has no llseek. */
static int64_t crc_data_llseek(fd_entry_t *e, int64_t offset, int whence)
{
    (void)e;
    (void)offset;
    (void)whence;
    return -ESPIPE;
}

static const struct file_ops crc_data_ops = {
    crc_data_read,  NULL, crc_data_poll,   NULL,
    crc_data_close, NULL, crc_data_llseek, FILE_OPS_POLL_UNLOCKED};

/* ------------------------------------------------------------------------
 * A connector's files, Linux's drm_debugfs_connector_add
 */

/** `force`'s text, Linux's connector_show: the force as a word. */
static int32_t force_show(void *data, char *buf, uint32_t cap)
{
    const struct drm_connector *connector = (const struct drm_connector *)data;
    static const char *const names[] = {"unspecified", "off", "on", "digital"};
    struct emit e;

    e.buf = buf;
    e.cap = cap;
    e.len = 0;
    emit_str(&e, connector->force >= 0 && connector->force <= DRM_FORCE_ON_DIGITAL
                     ? names[connector->force]
                     : "unspecified");
    emit_str(&e, "\n");
    return emit_end(&e);
}

/** `force`'s write, Linux's connector_write: EINVAL past eleven bytes,
 * before the copy, then the word (`drm_connector_force_write`). */
static int32_t force_store(void *data, const char __user *buf, uint32_t len)
{
    char copy[12];
    struct drm_connector *connector = (struct drm_connector *)data;
    int32_t rc;

    if (len > sizeof(copy) - 1u) {
        return -EINVAL;
    }
    if (copy_from_user(copy, buf, len) != 0) {
        return -EFAULT;
    }
    /* Under the mode lock, which the connector's probe holds while it
     * reads the force; Linux's connector_write stores it with no lock
     * (drm_debugfs.c:406-433). */
    drm_modeset_lock_nointr(connector->dev);
    rc = drm_connector_force_write(connector, copy, len);
    drm_modeset_unlock(connector->dev);
    return rc;
}

/** `edid_override`'s text, Linux's drm_edid_override_show: the override's
 * bytes, or nothing. */
static int32_t edid_show(void *data, char *buf, uint32_t cap)
{
    const struct drm_connector *connector = (const struct drm_connector *)data;
    struct emit e;

    e.buf = buf;
    e.cap = cap;
    e.len = 0;
    if (connector->edid_override) {
        emit_bytes(&e, (const char *)connector->edid_override, connector->edid_override_len);
    }
    return emit_end(&e);
}

/** `edid_override`'s write, Linux's edid_write: "reset" drops the
 * override, anything else is an EDID of whole blocks for the next probe. */
static int32_t edid_store(void *data, const char __user *buf, uint32_t len)
{
    struct drm_connector *connector = (struct drm_connector *)data;
    char *copy = kmalloc(len ? len : 1u);
    int32_t rc;

    if (!copy) {
        return -ENOMEM;
    }
    if (copy_from_user(copy, buf, len) != 0) {
        kfree(copy);
        return -EFAULT;
    }
    /* Under the mode lock, which the probe holds while it reads the
     * override, as Linux's drm_edid_override_set and _reset take the
     * connector's edid_override_mutex (drm_edid.c:2259-2280). */
    drm_modeset_lock_nointr(connector->dev);
    if (len == 5 && copy[0] == 'r' && copy[1] == 'e' && copy[2] == 's' && copy[3] == 'e' &&
        copy[4] == 't') {
        drm_edid_override_reset(connector);
        rc = 0;
    } else {
        rc = drm_edid_override_set(connector, copy, len);
    }
    drm_modeset_unlock(connector->dev);
    kfree(copy);
    return rc != 0 ? rc : (int32_t)len;
}

/** `vrr_range`'s text, Linux's vrr_range_show: the refresh range of the
 * sink's EDID; ENODEV while it is not connected. */
static int32_t vrr_range_show(void *data, char *buf, uint32_t cap)
{
    const struct drm_connector *connector = (const struct drm_connector *)data;
    struct emit e;

    if (connector->status != DRM_CONNECTOR_CONNECTED) {
        return -ENODEV;
    }
    e.buf = buf;
    e.cap = cap;
    e.len = 0;
    emit_str(&e, "Min: ");
    emit_u(&e, connector->min_vfreq, 0);
    emit_str(&e, "\nMax: ");
    emit_u(&e, connector->max_vfreq, 0);
    emit_str(&e, "\n");
    return emit_end(&e);
}

/** `output_bpc`'s text, Linux's output_bpc_show: the bits a colour the
 * sink's EDID names, which IGT reads as the connector's most; ENODEV while
 * it is not connected. */
static int32_t output_bpc_show(void *data, char *buf, uint32_t cap)
{
    const struct drm_connector *connector = (const struct drm_connector *)data;
    struct emit e;

    if (connector->status != DRM_CONNECTOR_CONNECTED) {
        return -ENODEV;
    }
    e.buf = buf;
    e.cap = cap;
    e.len = 0;
    emit_str(&e, "Maximum: ");
    emit_u(&e, connector->bpc, 0);
    emit_str(&e, "\n");
    return emit_end(&e);
}

static const struct debugfs_text_ops force_ops = {force_show, force_store};
static const struct debugfs_text_ops edid_ops = {edid_show, edid_store};
static const struct debugfs_text_ops vrr_range_ops = {vrr_range_show, NULL};
static const struct debugfs_text_ops output_bpc_ops = {output_bpc_show, NULL};

/* ------------------------------------------------------------------------
 * Registration
 */

/** A node below a device's directory holds the device. */
static void owner_get(void *owner)
{
    drm_dev_get((struct drm_device *)owner);
}

/** The node is freed: drop its hold, which may release the device. */
static void owner_put(void *owner)
{
    drm_dev_put((struct drm_device *)owner);
}

static const struct debugfs_owner drm_owner = {owner_get, owner_put};

void drm_debugfs_init(void)
{
    int32_t rc = debugfs_create_dir("dri", NULL, NULL);

    if (rc != 0 && rc != -EEXIST) {
        klog(KLOG_WARN, "drm", "debugfs: error %d", (int)rc);
    }
}

/** Write "dri/DIR/PREFIX-N" and `rest`, if any, into `path`. */
static void indexed_path(char *path, const struct drm_device *dev, const char *prefix,
                         uint32_t index, const char *rest)
{
    char name[48];
    uint32_t at;

    k_strncpy(name, prefix, sizeof(name));
    at = (uint32_t)k_strlen(name);
    name[at++] = '-';
    if (index >= 10) {
        name[at++] = (char)('0' + index / 10 % 10);
    }
    name[at++] = (char)('0' + index % 10);
    name[at] = '\0';
    if (rest) {
        name[at++] = '/';
        k_strncpy(name + at, rest, sizeof(name) - at);
    }
    name[sizeof(name) - 1] = '\0';
    dev_path(path, dev, name);
}

/** Write "dri/DIR/CONNECTOR/" and `file` into `path`. */
static void connector_path(char *path, const struct drm_connector *connector, const char *file)
{
    char name[DRM_CONNECTOR_NAME_LEN + 16];
    uint32_t at;

    k_strncpy(name, connector->name, DRM_CONNECTOR_NAME_LEN);
    name[DRM_CONNECTOR_NAME_LEN - 1] = '\0';
    at = (uint32_t)k_strlen(name);
    name[at++] = '/';
    k_strncpy(name + at, file, sizeof(name) - at);
    name[sizeof(name) - 1] = '\0';
    dev_path(path, connector->dev, name);
}

/** Write "dri/N", the link of the node of minor N, into `path`. */
static void minor_link(char *path, int minor)
{
    uint32_t at = 4;

    k_strncpy(path, "dri/", DRM_DEBUGFS_PATH);
    if (minor >= 100) {
        path[at++] = (char)('0' + minor / 100);
    }
    if (minor >= 10) {
        path[at++] = (char)('0' + minor / 10 % 10);
    }
    path[at++] = (char)('0' + minor % 10);
    path[at] = '\0';
}

/** Make `connector`'s files below its device's directory, Linux's
 * drm_debugfs_connector_add. 0, or the first error. */
static int32_t connector_files(struct drm_connector *connector)
{
    static const struct {
        const char *name;
        uint32_t mode;
        const struct debugfs_text_ops *ops;
    } files[] = {{"force", 0644u, &force_ops},
                 {"edid_override", 0644u, &edid_ops},
                 {"vrr_range", 0444u, &vrr_range_ops},
                 {"output_bpc", 0444u, &output_bpc_ops}};
    char path[DRM_DEBUGFS_PATH];
    int32_t rc = 0;
    uint32_t i;

    for (i = 0; rc == 0 && i < sizeof(files) / sizeof(files[0]); i++) {
        connector_path(path, connector, files[i].name);
        rc = debugfs_create_text(path, files[i].mode, files[i].ops, connector);
    }
    return rc;
}

/** Make `dev`'s files below its directory, which is there. 0, or the
 * first error. */
static int32_t make_files(struct drm_device *dev)
{
    const struct drm_mode_config *mc = &dev->mode_config;
    char path[DRM_DEBUGFS_PATH];
    int32_t rc;
    uint32_t i;

    dev_path(path, dev, "name");
    rc = debugfs_create_text(path, 0444u, &name_ops, dev);
    dev_path(path, dev, "clients");
    rc = rc ? rc : debugfs_create_text(path, 0444u, &clients_ops, dev);
    if (drm_core_check_feature(dev, DRIVER_GEM)) {
        dev_path(path, dev, "gem_names");
        rc = rc ? rc : debugfs_create_text(path, 0444u, &gem_names_ops, dev);
    }
    if (!drm_core_check_feature(dev, DRIVER_MODESET)) {
        return rc;
    }
    dev_path(path, dev, "framebuffer");
    rc = rc ? rc : debugfs_create_text(path, 0444u, &framebuffer_ops, dev);
    if (drm_core_check_feature(dev, DRIVER_ATOMIC)) {
        dev_path(path, dev, "state");
        rc = rc ? rc : debugfs_create_text(path, 0444u, &state_ops, dev);
    }
    for (i = 0; rc == 0 && i < mc->num_crtc && i < DRM_MAX_CRTCS; i++) {
        indexed_path(path, dev, "crtc", i, NULL);
        rc = debugfs_create_dir(path, NULL, NULL);
        if (rc == 0 && drm_crtc_has_vblank(mc->crtcs[i])) {
            indexed_path(path, dev, "crtc", i, "vblank_backwards");
            rc = debugfs_create_text(path, 0444u, &vblank_backwards_ops, mc->crtcs[i]);
        }
        /* Linux's drm_debugfs_crtc_crc_add: CRC files only with CRCs. */
        if (rc != 0 || !mc->crtcs[i]->funcs->set_crc_source ||
            !mc->crtcs[i]->funcs->verify_crc_source) {
            continue;
        }
        indexed_path(path, dev, "crtc", i, "crc/control");
        rc = debugfs_create_text(path, 0644u, &crc_control_ops, mc->crtcs[i]);
        indexed_path(path, dev, "crtc", i, "crc/data");
        rc = rc ? rc : debugfs_create_file(path, 0444u, &crc_data_ops, crc_data_open, mc->crtcs[i]);
    }
    for (i = 0; rc == 0 && i < mc->num_encoder && i < DRM_MAX_CONNECTORS; i++) {
        indexed_path(path, dev, "encoder", i, "bridges");
        rc = debugfs_create_text(path, 0444u, &bridges_ops, mc->encoders[i]);
    }
    for (i = 0; rc == 0 && i < mc->num_connector && i < DRM_MAX_CONNECTORS; i++) {
        rc = connector_files(mc->connectors[i]);
    }
    return rc;
}

void drm_debugfs_register(struct drm_device *dev)
{
    char path[DRM_DEBUGFS_PATH], link[DRM_DEBUGFS_PATH];
    int32_t rc;

    dev_path(path, dev, NULL);
    rc = debugfs_create_dir(path, &drm_owner, dev);
    if (rc != 0) {
        klog(KLOG_WARN, "drm", "debugfs: %s: error %d", path, (int)rc);
        return;
    }
    dev->debugfs = 1;
    rc = make_files(dev);
    minor_link(link, dev->index);
    rc = rc ? rc : debugfs_create_symlink(link, dir_name(dev));
    if (rc == 0 && drm_core_check_feature(dev, DRIVER_RENDER)) {
        minor_link(link, DRM_MINOR_RENDER_BASE + dev->index);
        rc = debugfs_create_symlink(link, dir_name(dev));
    }
    if (rc != 0) {
        klog(KLOG_WARN, "drm", "debugfs: %s: error %d", path, (int)rc);
        drm_debugfs_unregister(dev);
    }
}

void drm_debugfs_path(const struct drm_device *dev, const char *name, char *path, uint32_t cap)
{
    char full[DRM_DEBUGFS_PATH];

    if (cap == 0) {
        return;
    }
    dev_path(full, dev, name);
    k_strncpy(path, full, cap);
    path[cap - 1u] = '\0';
}

void drm_debugfs_unregister(struct drm_device *dev)
{
    char path[DRM_DEBUGFS_PATH];

    if (!dev->debugfs) {
        return;
    }
    dev->debugfs = 0;
    minor_link(path, dev->index);
    debugfs_remove(path);
    minor_link(path, DRM_MINOR_RENDER_BASE + dev->index);
    debugfs_remove(path);
    dev_path(path, dev, NULL);
    debugfs_remove(path);
}
