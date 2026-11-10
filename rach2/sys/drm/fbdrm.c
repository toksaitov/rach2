/*
 * fbdrm.c: the framebuffer the firmware or the loader set up, as one fixed
 * mode, Linux's simpledrm: a CRTC, a primary plane and a connector on the
 * screen the boot record describes, whose dumb buffers are kept in memory
 * and copied to the screen, converted to its format, where they changed.
 */

#include "drm.h"

#include "../boot.h"
#include "../display/fbcon.h"
#include "../klog.h"
#include "../lib/kerrno.h"
#include "../lib/kstring.h"
#include "../mm/kheap.h"
#include "../mm/paging.h"

/*
 * The device. `screen` is the kernel's mapping of the framebuffer, the
 * console's (`fbcon_window`), `pitch` bytes a row and `mode` its one mode.
 * `format` is the screen's pixel format and `formats` the plane's, the
 * screen's then XRGB8888, which is converted. `line` holds one row of the
 * screen's pixels while a row is converted.
 */
struct fbdrm_device {
    struct drm_device drm;
    volatile uint8_t *screen;
    uint64_t base;
    uint32_t pitch;
    uint32_t format;
    const struct drm_format_info *info;
    struct drm_mode_modeinfo mode;
    uint32_t mm_width, mm_height;
    uint32_t formats[2];
    uint32_t nformats;
    uint8_t *line;
    struct drm_plane plane;
    struct drm_crtc crtc;
    struct drm_encoder encoder;
    struct drm_connector connector;
};

#define to_fbdrm(dev) ((struct fbdrm_device *)(dev)->driver_priv)

/* simpledrm's millimetres of a screen whose size the firmware does not
 * give: 96 dots an inch, Linux's DRM_MODE_RES_MM. */
#define FBDRM_RES_MM(res) ((uint32_t)(res) * 254u / 960u)

/* simpledrm's shadow plane's largest framebuffer, DRM_SHADOW_PLANE_MAX_*. */
#define FBDRM_SHADOW_MAX 4096u

/* A screen's layout of colours and the DRM format it is: its bits a pixel
 * (15 or 16 both for 1555), and each colour's lowest bit and width. */
struct fbdrm_layout {
    uint8_t bpp, alt_bpp;
    uint8_t red_shift, red_bits, green_shift, green_bits, blue_shift, blue_bits;
    uint32_t format;
};

/* The direct-colour formats simplefb knows (Linux's SIMPLEFB_FORMATS) as
 * sysfb reads them from the firmware, each alpha format by its opaque form,
 * as drm_fb_build_fourcc_list makes it. */
static const struct fbdrm_layout fbdrm_layouts[] = {
    {16, 16, 11, 5, 5, 6, 0, 5, DRM_FORMAT_RGB565},
    {15, 16, 10, 5, 5, 5, 0, 5, DRM_FORMAT_XRGB1555},
    {24, 24, 16, 8, 8, 8, 0, 8, DRM_FORMAT_RGB888},
    {32, 32, 16, 8, 8, 8, 0, 8, DRM_FORMAT_XRGB8888},
    {32, 32, 0, 8, 8, 8, 16, 8, DRM_FORMAT_XBGR8888},
    {32, 32, 20, 10, 10, 10, 0, 10, DRM_FORMAT_XRGB2101010},
};

/** The DRM format of the screen `fb`, or DRM_FORMAT_INVALID for a layout
 * simplefb does not know. */
static uint32_t fbdrm_format(const struct boot_framebuffer *fb)
{
    const struct fbdrm_layout *l;
    uint32_t i;

    for (i = 0; i < sizeof(fbdrm_layouts) / sizeof(fbdrm_layouts[0]); i++) {
        l = &fbdrm_layouts[i];
        if ((fb->bpp == l->bpp || fb->bpp == l->alt_bpp) && fb->red_shift == l->red_shift &&
            fb->red_bits == l->red_bits && fb->green_shift == l->green_shift &&
            fb->green_bits == l->green_bits && fb->blue_shift == l->blue_shift &&
            fb->blue_bits == l->blue_bits) {
            return l->format;
        }
    }
    return DRM_FORMAT_INVALID;
}

/**
 * Convert `n` pixels of XRGB8888 at `src` to the screen's `format` at
 * `dst`, as Linux's drm_fb_blit converts them: each colour's top bits, and
 * for 10 bits a colour its 8 bits with the top two repeated below.
 */
static void fbdrm_convert(uint8_t *dst, const uint8_t *src, uint32_t n, uint32_t format)
{
    uint32_t i, p, r, g, b, v;

    for (i = 0; i < n; i++) {
        p = (uint32_t)src[4 * i] | ((uint32_t)src[4 * i + 1] << 8) |
            ((uint32_t)src[4 * i + 2] << 16) | ((uint32_t)src[4 * i + 3] << 24);
        r = (p >> 16) & 0xffu;
        g = (p >> 8) & 0xffu;
        b = p & 0xffu;
        switch (format) {
        case DRM_FORMAT_RGB565:
            v = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);
            dst[2 * i] = (uint8_t)v;
            dst[2 * i + 1] = (uint8_t)(v >> 8);
            break;
        case DRM_FORMAT_XRGB1555:
            v = ((r >> 3) << 10) | ((g >> 3) << 5) | (b >> 3);
            dst[2 * i] = (uint8_t)v;
            dst[2 * i + 1] = (uint8_t)(v >> 8);
            break;
        case DRM_FORMAT_RGB888:
            dst[3 * i] = (uint8_t)b;
            dst[3 * i + 1] = (uint8_t)g;
            dst[3 * i + 2] = (uint8_t)r;
            break;
        case DRM_FORMAT_XBGR8888:
            v = (b << 16) | (g << 8) | r;
            dst[4 * i] = (uint8_t)v;
            dst[4 * i + 1] = (uint8_t)(v >> 8);
            dst[4 * i + 2] = (uint8_t)(v >> 16);
            dst[4 * i + 3] = 0;
            break;
        case DRM_FORMAT_XRGB2101010:
            v = (((r << 2) | (r >> 6)) << 20) | (((g << 2) | (g >> 6)) << 10) |
                ((b << 2) | (b >> 6));
            dst[4 * i] = (uint8_t)v;
            dst[4 * i + 1] = (uint8_t)(v >> 8);
            dst[4 * i + 2] = (uint8_t)(v >> 16);
            dst[4 * i + 3] = (uint8_t)(v >> 24);
            break;
        default:
            dst[4 * i] = (uint8_t)b;
            dst[4 * i + 1] = (uint8_t)g;
            dst[4 * i + 2] = (uint8_t)r;
            dst[4 * i + 3] = 0;
            break;
        }
    }
}

/**
 * Read `len` bytes of the buffer object `obj`, a shmem object as every
 * object of this driver is, from byte `off` into `dst`, a page at a time
 * through a temporary mapping. A page that cannot be had or mapped reads
 * as zeros. May sleep.
 */
static void fbdrm_read(struct drm_gem_object *obj, uint64_t off, uint8_t *dst, uint32_t len)
{
    struct drm_gem_shmem_object *s = (struct drm_gem_shmem_object *)obj;
    uint32_t n, in;
    uintptr_t phys;
    uint8_t *va;

    while (len > 0) {
        in = (uint32_t)(off & (PAGE_SIZE - 1));
        n = PAGE_SIZE - in < len ? PAGE_SIZE - in : len;
        phys = off < obj->size ? drm_gem_shmem_page(s, (uint32_t)(off >> PAGE_SHIFT)) : 0;
        va = phys ? (uint8_t *)kmap_temp(phys) : NULL;
        if (va) {
            k_memcpy(dst, va + in, n);
            kunmap_temp(va);
        } else {
            k_memset(dst, 0, n);
        }
        dst += n;
        off += n;
        len -= n;
    }
}

/**
 * Copy the rectangle `r` of the framebuffer `fb`, in its pixels, to the
 * screen at (`dx`, `dy`), converted to the screen's format. The caller has
 * clipped both to the screen.
 */
static void fbdrm_blit(struct fbdrm_device *f, const struct drm_framebuffer *fb,
                       const struct drm_rect *r, int32_t dx, int32_t dy)
{
    uint32_t cpp = fb->info->cpp[0], w = (uint32_t)(r->x2 - r->x1), out, row_y;
    uint32_t screen_cpp = f->info->cpp[0];
    uint8_t *row = f->line + f->pitch;
    volatile uint8_t *to;
    uint32_t i;

    out = w * screen_cpp;
    for (row_y = (uint32_t)r->y1; row_y < (uint32_t)r->y2; row_y++) {
        fbdrm_read(fb->obj[0],
                   (uint64_t)fb->offsets[0] + (uint64_t)row_y * fb->pitches[0] +
                       (uint64_t)(uint32_t)r->x1 * cpp,
                   row, w * cpp);
        if (fb->format != f->format) {
            fbdrm_convert(f->line, row, w, f->format);
        } else {
            k_memcpy(f->line, row, out);
        }
        to = f->screen + (uint32_t)(dy + (int32_t)(row_y - (uint32_t)r->y1)) * f->pitch +
             (uint32_t)dx * screen_cpp;
        for (i = 0; i < out; i++) {
            to[i] = f->line[i];
        }
    }
}

/** Clip `r` to `clip`; whether anything is left. */
static int fbdrm_intersect(struct drm_rect *r, const struct drm_rect *clip)
{
    if (r->x1 < clip->x1) {
        r->x1 = clip->x1;
    }
    if (r->y1 < clip->y1) {
        r->y1 = clip->y1;
    }
    if (r->x2 > clip->x2) {
        r->x2 = clip->x2;
    }
    if (r->y2 > clip->y2) {
        r->y2 = clip->y2;
    }
    return r->x1 < r->x2 && r->y1 < r->y2;
}

/**
 * Copy one rectangle of damage `d`, in the framebuffer's pixels, to the
 * screen: clipped to the plane's source `src`, which the plane shows at
 * `dst` on the screen, without scaling.
 */
static void fbdrm_damage(struct fbdrm_device *f, const struct drm_plane_state *ps,
                         const struct drm_rect *src, struct drm_rect d)
{
    struct drm_rect screen, to;

    screen.x1 = screen.y1 = 0;
    screen.x2 = f->mode.hdisplay;
    screen.y2 = f->mode.vdisplay;

    if (!fbdrm_intersect(&d, src)) {
        return;
    }
    to.x1 = ps->dst.x1 + (d.x1 - src->x1);
    to.y1 = ps->dst.y1 + (d.y1 - src->y1);
    to.x2 = to.x1 + (d.x2 - d.x1);
    to.y2 = to.y1 + (d.y2 - d.y1);
    if (!fbdrm_intersect(&to, &screen)) {
        return;
    }
    d.x1 = src->x1 + (to.x1 - ps->dst.x1);
    d.y1 = src->y1 + (to.y1 - ps->dst.y1);
    d.x2 = d.x1 + (to.x2 - to.x1);
    d.y2 = d.y1 + (to.y2 - to.y1);
    fbdrm_blit(f, ps->fb, &d, to.x1, to.y1);
}

/**
 * The plane's update: copy what changed, as Linux's damage iterator gives
 * it. The plane's source in whole pixels, rounded out, is copied whole when
 * the commit gave no FB_DAMAGE_CLIPS, when the source moved, or when the
 * plane was not shown before; otherwise each clip within it.
 */
static void fbdrm_plane_update(struct drm_plane *plane, struct drm_atomic_state *state)
{
    struct fbdrm_device *f = to_fbdrm(plane->dev);
    const struct drm_plane_state *ps = drm_atomic_get_new_plane_state(state, plane);
    const struct drm_plane_state *old = drm_atomic_get_old_plane_state(state, plane);
    const struct drm_mode_rect *clips;
    struct drm_rect src, d;
    uint32_t n;

    if (plane->dev->unplugged || !ps->fb || !ps->crtc || !ps->visible) {
        return;
    }
    src.x1 = ps->src.x1 >> 16;
    src.y1 = ps->src.y1 >> 16;
    src.x2 = (ps->src.x2 >> 16) + ((ps->src.x2 & 0xffff) != 0);
    src.y2 = (ps->src.y2 >> 16) + ((ps->src.y2 & 0xffff) != 0);
    if (!ps->fb_damage_clips || !old->visible || old->src.x1 != ps->src.x1 ||
        old->src.y1 != ps->src.y1 || old->src.x2 != ps->src.x2 || old->src.y2 != ps->src.y2) {
        fbdrm_damage(f, ps, &src, src);
        return;
    }
    clips = (const struct drm_mode_rect *)ps->fb_damage_clips->data;
    for (n = ps->fb_damage_clips->length / (uint32_t)sizeof(*clips); n > 0; n--, clips++) {
        d.x1 = clips->x1;
        d.y1 = clips->y1;
        d.x2 = clips->x2;
        d.y2 = clips->y2;
        fbdrm_damage(f, ps, &src, d);
    }
}

/** The plane's disable: the screen painted black, as simpledrm clears it. */
static void fbdrm_plane_disable(struct drm_plane *plane, struct drm_atomic_state *state)
{
    struct fbdrm_device *f = to_fbdrm(plane->dev);
    uint32_t i, size = f->pitch * f->mode.vdisplay;

    (void)state;
    if (plane->dev->unplugged) {
        return;
    }
    for (i = 0; i < size; i++) {
        f->screen[i] = 0;
    }
}

/** The plane's check, simpledrm's: the plane covers the CRTC unscaled. */
static int32_t fbdrm_plane_check(struct drm_plane *plane, struct drm_atomic_state *state)
{
    struct drm_plane_state *ps = drm_atomic_get_new_plane_state(state, plane);
    const struct drm_crtc_state *cs = NULL;

    if (ps->crtc) {
        cs = drm_atomic_get_new_crtc_state(state, ps->crtc)
                 ? drm_atomic_get_new_crtc_state(state, ps->crtc)
                 : ps->crtc->state;
    }
    return drm_atomic_helper_check_plane_state(ps, cs, DRM_PLANE_NO_SCALING, DRM_PLANE_NO_SCALING,
                                               0, 0);
}

/** The CRTC's check, Linux's drm_crtc_helper_atomic_check: a CRTC that is
 * on shows its primary plane. */
static int32_t fbdrm_crtc_check(struct drm_crtc *crtc, struct drm_atomic_state *state)
{
    const struct drm_crtc_state *cs = drm_atomic_get_new_crtc_state(state, crtc);

    return cs->enable && !(cs->plane_mask & drm_plane_mask(crtc->primary)) ? -EINVAL : 0;
}

/** The CRTC drives the screen's size alone, Linux's
 * drm_crtc_helper_mode_valid_fixed. */
static int32_t fbdrm_crtc_mode_valid(struct drm_crtc *crtc, const struct drm_mode_modeinfo *mode)
{
    struct fbdrm_device *f = to_fbdrm(crtc->dev);

    return mode->hdisplay == f->mode.hdisplay && mode->vdisplay == f->mode.vdisplay ? 0 : -EINVAL;
}

/** The connector's one mode, preferred, and the screen's size, Linux's
 * drm_connector_helper_get_modes_fixed. */
static int fbdrm_get_modes(struct drm_connector *connector)
{
    struct fbdrm_device *f = to_fbdrm(connector->dev);
    struct drm_mode_modeinfo m = f->mode;

    m.type |= DRM_MODE_TYPE_PREFERRED;
    if (drm_mode_probed_add(connector, &m) != 0) {
        return 0;
    }
    connector->mm_width = f->mm_width;
    connector->mm_height = f->mm_height;
    return 1;
}

static const struct drm_plane_funcs fbdrm_plane_funcs = {fbdrm_plane_check, fbdrm_plane_update,
                                                         fbdrm_plane_disable};
static const struct drm_crtc_funcs fbdrm_crtc_funcs = {fbdrm_crtc_check,
                                                       NULL,
                                                       NULL,
                                                       NULL,
                                                       NULL,
                                                       fbdrm_crtc_mode_valid,
                                                       NULL,
                                                       NULL,
                                                       NULL,
                                                       NULL,
                                                       NULL,
                                                       NULL,
                                                       NULL};
static const struct drm_connector_funcs fbdrm_connector_funcs = {NULL, fbdrm_get_modes, NULL, NULL};
static const uint64_t fbdrm_modifiers[] = {DRM_FORMAT_MOD_LINEAR, DRM_FORMAT_MOD_INVALID};

/** The driver's `release`, after the last reference: the display's
 * objects and the record. */
static void fbdrm_release(struct drm_device *dev)
{
    struct fbdrm_device *f = to_fbdrm(dev);

    drm_mode_config_cleanup(dev);
    kfree(f->line);
    kfree(f);
}

static const struct drm_driver fbdrm_driver = {
    DRM_IDENT_SIMPLEDRM, NULL, NULL, drm_gem_shmem_dumb_create, NULL, NULL, NULL, 0, fbdrm_release};

/** The aperture's hook: a native driver takes the framebuffer, and the
 * device goes as a device lost does. */
static void fbdrm_remove(struct drm_device *dev)
{
    klog(KLOG_INFO, "fbdrm", "removed: card %d", dev->index);
    drm_dev_unplug(dev);
}

/**
 * Make the display, in simpledrm's order: the plane of the screen's format
 * and XRGB8888, the CRTC, an encoder of no type and a connector of unknown
 * type, "Unknown-1". 0 or -errno.
 */
static int32_t fbdrm_kms_init(struct fbdrm_device *f)
{
    struct drm_device *dev = &f->drm;
    struct drm_mode_config *mc = &dev->mode_config;
    int32_t err;

    err = drm_mode_config_init(dev);
    if (err != 0) {
        return err;
    }
    mc->min_width = f->mode.hdisplay;
    mc->max_width = f->mode.hdisplay > FBDRM_SHADOW_MAX ? f->mode.hdisplay : FBDRM_SHADOW_MAX;
    mc->min_height = f->mode.vdisplay;
    mc->max_height = f->mode.vdisplay > FBDRM_SHADOW_MAX ? f->mode.vdisplay : FBDRM_SHADOW_MAX;
    mc->preferred_depth = f->info->depth;
    mc->fb_dirty = 1;
    f->formats[0] = f->format;
    f->nformats = 1;
    if (f->format != DRM_FORMAT_XRGB8888) {
        f->formats[f->nformats++] = DRM_FORMAT_XRGB8888;
    }
    if ((err = drm_universal_plane_init(dev, &f->plane, 0, &fbdrm_plane_funcs, f->formats,
                                        f->nformats, fbdrm_modifiers, DRM_PLANE_TYPE_PRIMARY)) !=
            0 ||
        (err = drm_crtc_init_with_planes(dev, &f->crtc, &f->plane, NULL, &fbdrm_crtc_funcs)) != 0 ||
        (err = drm_encoder_init(dev, &f->encoder, DRM_MODE_ENCODER_NONE)) != 0) {
        return err;
    }
    drm_plane_enable_fb_damage_clips(&f->plane);
    f->encoder.possible_crtcs = drm_crtc_mask(&f->crtc);
    err =
        drm_connector_init(dev, &f->connector, &fbdrm_connector_funcs, DRM_MODE_CONNECTOR_Unknown);
    if (err != 0) {
        return err;
    }
    return drm_connector_attach_encoder(&f->connector, &f->encoder);
}

/**
 * Bind the loader's framebuffer: a direct-colour screen in one of the
 * formats simplefb knows, which the kernel reaches, and whose range no
 * native driver has taken. 1, or -ENODEV for none, or -errno.
 */
int32_t fbdrm_probe(void)
{
    const struct boot_framebuffer *fb = fbcon_boot_screen();
    struct fbdrm_device *f;
    volatile uint8_t *screen;
    uint32_t format, size;
    int32_t err;

    if (fb->type != BOOT_FB_RGB || fb->width == 0 || fb->height == 0 || fb->width > 0x7fffu ||
        fb->height > 0x7fffu) {
        return -ENODEV;
    }
    format = fbdrm_format(fb);
    if (format == DRM_FORMAT_INVALID) {
        klog(KLOG_INFO, "fbdrm",
             "framebuffer of %u bits, red %u:%u, green %u:%u, blue %u:%u: no format",
             (unsigned)fb->bpp, (unsigned)fb->red_shift, (unsigned)fb->red_bits,
             (unsigned)fb->green_shift, (unsigned)fb->green_bits, (unsigned)fb->blue_shift,
             (unsigned)fb->blue_bits);
        return -ENODEV;
    }
    if (fb->pitch < fb->width * drm_format_info(format)->cpp[0]) {
        return -ENODEV;
    }
    screen = fbcon_window();
    if (!screen) {
        klog(KLOG_WARN, "fbdrm", "framebuffer at 0x%x not reachable", (uint32_t)fb->addr);
        return -ENODEV;
    }
    f = (struct fbdrm_device *)kzalloc(sizeof(*f));
    if (!f) {
        return -ENOMEM;
    }
    f->screen = screen;
    f->base = fb->addr;
    f->pitch = fb->pitch;
    f->format = format;
    f->info = drm_format_info(format);
    /* simpledrm's mode, DRM_MODE_INIT: 60 Hz with no blanking. */
    f->mode.clock = fb->width * fb->height * 60u / 1000u;
    f->mode.hdisplay = f->mode.hsync_start = f->mode.hsync_end = f->mode.htotal =
        (uint16_t)fb->width;
    f->mode.vdisplay = f->mode.vsync_start = f->mode.vsync_end = f->mode.vtotal =
        (uint16_t)fb->height;
    f->mode.type = DRM_MODE_TYPE_DRIVER;
    drm_mode_set_name(&f->mode);
    f->mode.vrefresh = drm_mode_vrefresh(&f->mode);
    f->mm_width = FBDRM_RES_MM(fb->width);
    f->mm_height = FBDRM_RES_MM(fb->height);
    /* A row of the screen and a row of a framebuffer's pixels beside it,
     * XRGB8888 at most the plane's widest. */
    size = f->pitch + 4u * (fb->width > FBDRM_SHADOW_MAX ? fb->width : FBDRM_SHADOW_MAX);
    f->line = (uint8_t *)kmalloc(size);
    if (!f->line) {
        kfree(f);
        return -ENOMEM;
    }
    drm_dev_init(&f->drm, &fbdrm_driver);
    f->drm.driver_priv = f;
    k_strncpy(f->drm.unique, "simple-framebuffer.0", sizeof(f->drm.unique) - 1);
    err = drm_aperture_acquire_firmware(&f->drm, f->base, (uint64_t)f->pitch * fb->height,
                                        fbdrm_remove);
    if (err == -EBUSY) {
        klog(KLOG_INFO, "fbdrm", "framebuffer at 0x%x is a native driver's", (uint32_t)f->base);
        drm_dev_put(&f->drm);
        return -ENODEV;
    }
    if (err == 0 && (err = fbdrm_kms_init(f)) == 0) {
        err = drm_dev_register(&f->drm);
    }
    if (err != 0) {
        klog(KLOG_WARN, "fbdrm", "register: error %d", (int)err);
        drm_aperture_release_firmware(&f->drm);
        drm_dev_put(&f->drm);
        return err;
    }
    klog(KLOG_INFO, "fbdrm", "registered: simpledrm %ux%u, %c%c%c%c, pitch %u, at 0x%x, card %d",
         (unsigned)fb->width, (unsigned)fb->height, (int)(format & 0xff),
         (int)((format >> 8) & 0xff), (int)((format >> 16) & 0xff), (int)((format >> 24) & 0xff),
         (unsigned)f->pitch, (uint32_t)f->base, f->drm.index);
    return 1;
}
