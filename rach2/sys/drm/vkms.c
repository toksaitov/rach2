/*
 * vkms.c: Linux's vkms, a display in software for IGT's tests: one CRTC
 * with a primary plane, a cursor plane and, when asked, eight overlay
 * planes, a virtual connector, vblanks at the mode's rate from a fine
 * service deadline, each stamped with the time its frame ends, and a CRC of
 * each frame composed in memory. `rach2.vkms` starts it.
 */

#include "../idle_timer.h"
#include "drm.h"

#include "../cmdline.h"
#include "../klog.h"
#include "../lib/kerrno.h"
#include "../lib/crc32.h"
#include "../lib/kirq.h"
#include "../lib/kstring.h"
#include "../mm/kheap.h"
#include "../mm/paging.h"
#include "../rtc.h"

#ifndef RACH2_HOSTTEST
#include "../arch/arch.h"
#include "../proc/process.h"
#include "../proc/sched.h"
#include "../proc/smp.h"
#endif

/* Linux's limits (vkms_drv.h): the modes' sizes, the one preferred, the
 * gamma table's entries and the overlay planes `vkms.enable_overlay`
 * adds. */
#define XRES_MIN           10
#define YRES_MIN           10
#define XRES_DEF           1024
#define YRES_DEF           768
#define XRES_MAX           8192
#define YRES_MAX           8192
#define VKMS_LUT_SIZE      256
#define NUM_OVERLAY_PLANES 8
#define VKMS_MAX_PLANES    (2 + NUM_OVERLAY_PLANES)

/* What a plane shows as the last commit programmed it, Linux's
 * vkms_frame_info: its framebuffer, held; the corner of the part shown, in
 * the framebuffer's pixels; where on the CRTC, clipped to it; and its
 * rotation. The part shown is the destination's size, turned by a quarter
 * for a rotation of 90 or 270 degrees, since the plane does not scale. */
struct vkms_frame {
    struct drm_framebuffer *fb;
    int32_t sx, sy;
    struct drm_rect dst;
    uint32_t rotation;
};

/* What the composer composes: whether the CRTC is on, its mode's size, the
 * visible planes bottom first, which is the planes' order, and the gamma
 * table's blob, held, or NULL. */
struct vkms_scene {
    int active;
    uint32_t width, height;
    uint32_t nframes;
    struct vkms_frame frames[VKMS_MAX_PLANES];
    struct drm_property_blob *gamma;
};

/*
 * The device, Linux's vkms_device and vkms_output. The planes are the
 * primary, the overlays and the cursor, in the order Linux makes them,
 * which is their order from the bottom up. `timer_on`, `period_ns` and
 * `next_ns` are the vblank timer: on while the core holds vblanks, a frame
 * of the mode long, firing at `next_ns` of CLOCK_MONOTONIC, which the tick
 * moves on past the clock by whole periods before it handles the vblank,
 * as Linux's hrtimer_forward_now does, so that the latest vblank ended at
 * `next_ns - period_ns`; `overruns` counts the passes that found more than
 * one period gone, and `skipped` the periods past the first they found.
 * `scene` is what
 * the last commit programmed. While `composer_enabled`, the CRC source is
 * on: each vblank asks for the frames from `frame_start` to `frame_end`
 * (`crc_pending`), which the composer thread composes and enters.
 * `crc_next` is the first frame not yet asked: the one after the vblank
 * count when the source goes on, then the one after the last frame asked;
 * 0 while the source is off and from a commit that needs a modeset of the
 * CRTC to its next request, so that the count the core adds when the
 * vblanks go on again is not taken for frames left out; `crc_missed`
 * counts the frames from `crc_next` that a later vblank, finding no request
 * pending, left out of its own; `catchups` counts the composer's passes
 * that entered more than one frame's CRC. `composing` is set while the
 * composer thread or a commit composes, so that one composes at a time and
 * the CRCs are entered in frame order, as Linux's ordered composer workqueue
 * runs one work at a time; a commit waits for it to fall (`flush_wait`), and
 * the composer thread for it to fall with a request pending
 * (`composer_wait`).
 *
 * The tick runs without the big lock, as Linux's hrtimer callback runs in
 * an interrupt, so two spinlocks guard what it shares. `timer_lock` guards
 * the timer, `timer_on`, `period_ns`, `next_ns`, `overruns` and `skipped`,
 * as an hrtimer's base lock does, and is taken last, since the core calls
 * `enable_vblank`, `disable_vblank` and `get_vblank_timestamp` under its
 * `vbl_lock`. `lock`, Linux's vkms_output.lock, guards `scene`,
 * `composer_enabled`, the request, `crc_next`, `crc_missed`, `catchups`
 * and `composing`. Neither is held across the core's vblank handling,
 * which may turn the timer off.
 */
struct vkms_device {
    struct drm_device drm;
    struct drm_plane planes[VKMS_MAX_PLANES];
    uint32_t nplanes;
    struct drm_crtc crtc;
    struct drm_encoder encoder;
    struct drm_connector connector;
    struct spinlock timer_lock, lock;
    int timer_on;
    int64_t period_ns, next_ns;
    uint32_t overruns, skipped, crc_missed, catchups;
    struct vkms_scene scene;
    int composer_enabled, composer_hold;
    int crc_pending;
    uint64_t frame_start, frame_end, crc_next;
    int composing;
    wait_queue_t composer_wait, flush_wait;
    int composer_started;
};

#define to_vkms(dev) ((struct vkms_device *)(dev)->driver_priv)

/* The device, once registered, for the tick. */
static struct vkms_device *vkms_dev;

/* The formats of every plane, Linux 6.12's vkms_formats. */
static const uint32_t vkms_formats[] = {DRM_FORMAT_ARGB8888, DRM_FORMAT_XRGB8888,
                                        DRM_FORMAT_XRGB16161616, DRM_FORMAT_ARGB16161616,
                                        DRM_FORMAT_RGB565};

/** CLOCK_MONOTONIC in nanoseconds. */
static int64_t vkms_now(void)
{
    int64_t sec, nsec;

    rtc_monotonic(&sec, &nsec);
    return sec * 1000000000ll + nsec;
}

/* ------------------------------------------------------------------------
 * Composing a frame
 */

/* The frames of the pages of a plane's buffer object that its framebuffer
 * covers, which a composition looks up under the big lock before it lets
 * the lock go: `n` pages from page `first` of `obj`, each page's frame in
 * `phys`, 0 for one that reads as zeros, with bit 0 set where the
 * composition took a pin on it. `live` is set when a frame could not be
 * pinned, a device's, which a reset of its device may take back: such a
 * plane's frames are looked up again at each read, and its composition
 * keeps the lock. */
struct vkms_pages {
    struct drm_gem_object *obj;
    uintptr_t *phys;
    uint32_t first, n;
    int live;
};

/**
 * Read `len` bytes from byte `off` of the buffer object whose frames `pg`
 * holds into `dst`, a page at a time through a temporary mapping; a page
 * outside them or without a frame reads as zeros. Needs no big lock unless
 * `pg->live`, when a frame without a pin is looked up again.
 */
static void vkms_read(const struct vkms_pages *pg, uint64_t off, uint8_t *dst, uint32_t len)
{
    uint32_t n, in;
    uint64_t at;
    uintptr_t phys;
    uint8_t *va;

    while (len > 0) {
        in = (uint32_t)(off & (PAGE_SIZE - 1));
        n = PAGE_SIZE - in < len ? PAGE_SIZE - in : len;
        at = (off >> PAGE_SHIFT) - pg->first;
        phys = (off >> PAGE_SHIFT) >= pg->first && at < pg->n ? pg->phys[at] : 0;
        if (phys & 1u) {
            phys &= ~(uintptr_t)1u;
        } else if (pg->live && phys) {
            phys = drm_gem_shmem_page((struct drm_gem_shmem_object *)pg->obj,
                                      pg->first + (uint32_t)at);
        }
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
 * Convert the pixel at `p` of `format` to Linux's pixel_argb_u16, alpha,
 * red, green and blue of 16 bits, into `out`, as vkms_formats.c does: a
 * channel of 8 bits times 257, one of 5 or 6 rounded to the nearest of 16,
 * and opaque for a format without alpha.
 */
static void to_argb16(uint32_t format, const uint8_t *p, uint16_t *out)
{
    uint32_t v;

    switch (format) {
    case DRM_FORMAT_ARGB8888:
    case DRM_FORMAT_XRGB8888:
        out[0] = format == DRM_FORMAT_ARGB8888 ? (uint16_t)(p[3] * 257u) : 0xffffu;
        out[1] = (uint16_t)(p[2] * 257u);
        out[2] = (uint16_t)(p[1] * 257u);
        out[3] = (uint16_t)(p[0] * 257u);
        break;
    case DRM_FORMAT_ARGB16161616:
    case DRM_FORMAT_XRGB16161616:
        out[0] = format == DRM_FORMAT_ARGB16161616 ? (uint16_t)(p[6] | p[7] << 8) : 0xffffu;
        out[1] = (uint16_t)(p[4] | p[5] << 8);
        out[2] = (uint16_t)(p[2] | p[3] << 8);
        out[3] = (uint16_t)(p[0] | p[1] << 8);
        break;
    case DRM_FORMAT_RGB565:
        v = (uint32_t)p[0] | (uint32_t)p[1] << 8;
        out[0] = 0xffffu;
        out[1] = (uint16_t)((((v >> 11) & 0x1fu) * 65535u + 15u) / 31u);
        out[2] = (uint16_t)((((v >> 5) & 0x3fu) * 65535u + 31u) / 63u);
        out[3] = (uint16_t)(((v & 0x1fu) * 65535u + 15u) / 31u);
        break;
    default:
        out[0] = out[1] = out[2] = out[3] = 0;
        break;
    }
}

/**
 * Fill `stage` with row `v` of the frame `f`'s destination, `w` pixels,
 * reading the framebuffer's frames `pg` through `row`, room for a row of the
 * part shown.
 * A destination pixel (u, v) shows the pixel of the part shown that the
 * rotation, undone, and then the reflection, undone, name, as Linux's
 * drm_rect_rotate turns a rectangle: reflect first, then rotate
 * counterclockwise.
 */
static void compose_row(const struct vkms_frame *f, const struct vkms_pages *pg, uint32_t v,
                        uint32_t w, uint16_t *stage, uint8_t *row)
{
    const struct drm_framebuffer *fb = f->fb;
    uint32_t cpp = fb->info->cpp[0], rot = f->rotation & DRM_MODE_ROTATE_MASK;
    uint32_t h = (uint32_t)(f->dst.y2 - f->dst.y1), sw, sh, u, fx, fy;
    uint64_t base = fb->offsets[0];
    uint8_t px[8];

    if (rot == DRM_MODE_ROTATE_90 || rot == DRM_MODE_ROTATE_270) {
        sw = h;
        sh = w;
        fx = rot == DRM_MODE_ROTATE_90 ? sw - 1u - v : v;
        if (f->rotation & DRM_MODE_REFLECT_X) {
            fx = sw - 1u - fx;
        }
        for (u = 0; u < w; u++) {
            fy = rot == DRM_MODE_ROTATE_90 ? u : sh - 1u - u;
            if (f->rotation & DRM_MODE_REFLECT_Y) {
                fy = sh - 1u - fy;
            }
            vkms_read(pg,
                      base + (uint64_t)((uint32_t)f->sy + fy) * fb->pitches[0] +
                          (uint64_t)((uint32_t)f->sx + fx) * cpp,
                      px, cpp);
            to_argb16(fb->format, px, stage + 4u * u);
        }
        return;
    }
    sw = w;
    sh = h;
    fy = rot == DRM_MODE_ROTATE_180 ? sh - 1u - v : v;
    if (f->rotation & DRM_MODE_REFLECT_Y) {
        fy = sh - 1u - fy;
    }
    vkms_read(pg,
              base + (uint64_t)((uint32_t)f->sy + fy) * fb->pitches[0] +
                  (uint64_t)(uint32_t)f->sx * cpp,
              row, sw * cpp);
    for (u = 0; u < w; u++) {
        fx = rot == DRM_MODE_ROTATE_180 ? sw - 1u - u : u;
        if (f->rotation & DRM_MODE_REFLECT_X) {
            fx = sw - 1u - fx;
        }
        to_argb16(fb->format, row + fx * cpp, stage + 4u * u);
    }
}

/** One channel blended, Linux's pre_mul_blend_channel: the source taken as
 * premultiplied by `alpha`, in Linux's 32-bit arithmetic. */
static uint16_t blend_channel(uint32_t src, uint32_t dst, uint32_t alpha)
{
    uint32_t c = src * 0xffffu + dst * (0xffffu - alpha);

    return (uint16_t)((c + 0x7fffu) / 0xffffu);
}

/**
 * Apply the gamma table `lut` of `n` entries to the row `out` of `w`
 * pixels, as Linux's apply_lut: each channel's value placed on the table in
 * 32.32 fixed point by `ratio`, (n - 1) / 0xffff, and taken between the
 * two entries about it.
 */
static void apply_lut(uint16_t *out, uint32_t w, const struct drm_color_lut *lut, uint32_t n,
                      uint64_t ratio)
{
    uint32_t i, c, fl, frac;
    uint64_t at;
    int32_t lo, hi;
    const uint16_t *e0, *e1;

    for (i = 0; i < w; i++) {
        for (c = 1; c <= 3; c++) {
            at = (uint64_t)out[4u * i + c] * ratio;
            fl = (uint32_t)(at >> 32);
            frac = (uint32_t)at;
            e0 = (const uint16_t *)&lut[fl];
            e1 = fl + 1u < n ? (const uint16_t *)&lut[fl + 1u] : e0;
            lo = e0[c - 1u];
            hi = e1[c - 1u];
            out[4u * i + c] = (uint16_t)((((int64_t)lo << 32) + (int64_t)(hi - lo) * frac) >> 32);
        }
    }
}

#ifndef RACH2_HOSTTEST
/** Give way between two rows of a frame, which hold no big lock: this
 * processor to a thread queued on it. */
static void compose_give_way(void)
{
    sched_service_point();
}

/** Pin the frame at `phys` for the rows of a frame: 1 when it took a pin,
 * 0 for a frame the pool does not keep, such as a device's. */
static int compose_pin(uintptr_t phys)
{
    return frame_pin(phys) == 0;
}

/** Drop a pin `compose_pin` took. */
static void compose_unpin(uintptr_t phys)
{
    frame_unpin(phys);
}

/** Let go of the big lock for the rows of a frame; the depth for
 * `compose_relock`. */
static int compose_unlock(void)
{
    uint32_t flags = kirq_save();
    int depth = bkl_drop();

    kirq_restore(flags);
    return depth;
}

/** Take the big lock again at the `depth` `compose_unlock` let go of. */
static void compose_relock(int depth)
{
    uint32_t flags;

    if (depth) {
        flags = kirq_save();
        bkl_take(depth);
        kirq_restore(flags);
    }
}
#else
/* The host tests have no big lock and no other thread; a test may run here
 * what another thread would run while the composer gives way, and read the
 * pins the composition holds and whether its rows run without the lock. */
static void (*compose_give_way_hook)(void);
static int compose_pins, compose_unlocked, compose_pin_refuse;

/** Run the test's `compose_give_way_hook`, if any. */
static void compose_give_way(void)
{
    if (compose_give_way_hook) {
        compose_give_way_hook();
    }
}

/** Count a pin, or refuse it as a device's frame would be while the test
 * asks (`compose_pin_refuse`); the model's frames are host memory. */
static int compose_pin(uintptr_t phys)
{
    (void)phys;
    if (compose_pin_refuse) {
        return 0;
    }
    compose_pins++;
    return 1;
}

/** Count a pin dropped. */
static void compose_unpin(uintptr_t phys)
{
    (void)phys;
    compose_pins--;
}

/** Mark the rows as run without the lock. */
static int compose_unlock(void)
{
    compose_unlocked = 1;
    return 1;
}

/** Mark the lock taken again. */
static void compose_relock(int depth)
{
    (void)depth;
    compose_unlocked = 0;
}
#endif

/**
 * Look up into `pg`, one for each frame of the scene `s`, the frames of the
 * pages of its buffer object that its framebuffer covers, from its offset
 * for its height of rows, and pin each frame the pool keeps, so that the
 * rows may read them without the big lock: 1 when every frame found took a
 * pin, 0 when one did not (`live`), or -ENOMEM with what was taken still in
 * `pg` for `compose_pages_put`. Under the big lock, since a page may be
 * given on its first touch (`drm_gem_shmem_page`).
 */
static int32_t compose_pages_get(const struct vkms_scene *s, struct vkms_pages *pg)
{
    const struct drm_framebuffer *fb;
    uint64_t end, first;
    uintptr_t phys;
    uint32_t i, k;
    int32_t all = 1;

    k_memset(pg, 0, VKMS_MAX_PLANES * sizeof(*pg));
    for (i = 0; i < s->nframes; i++) {
        fb = s->frames[i].fb;
        pg[i].obj = fb->obj[0];
        first = fb->offsets[0] >> PAGE_SHIFT;
        end = fb->offsets[0] + (uint64_t)fb->pitches[0] * fb->height;
        if (end > pg[i].obj->size) {
            end = pg[i].obj->size;
        }
        end = (end + PAGE_SIZE - 1u) >> PAGE_SHIFT;
        if (end <= first) {
            continue;
        }
        if (end - first > 0xffffffffu / (uint32_t)sizeof(uintptr_t)) {
            return -ENOMEM;
        }
        pg[i].phys = (uintptr_t *)kmalloc((size_t)(end - first) * sizeof(uintptr_t));
        if (!pg[i].phys) {
            return -ENOMEM;
        }
        pg[i].first = (uint32_t)first;
        pg[i].n = (uint32_t)(end - first);
        for (k = 0; k < pg[i].n; k++) {
            phys = drm_gem_shmem_page((struct drm_gem_shmem_object *)pg[i].obj, pg[i].first + k);
            pg[i].phys[k] = phys && compose_pin(phys) ? phys | 1u : phys;
            if (phys && !(pg[i].phys[k] & 1u)) {
                pg[i].live = 1;
                all = 0;
            }
        }
    }
    return all;
}

/** Unpin and free what `compose_pages_get` took into `pg` for `s`. Under the
 * big lock. */
static void compose_pages_put(const struct vkms_scene *s, struct vkms_pages *pg)
{
    uint32_t i, k;

    for (i = 0; i < s->nframes; i++) {
        for (k = 0; k < pg[i].n; k++) {
            if (pg[i].phys[k] & 1u) {
                compose_unpin(pg[i].phys[k] & ~(uintptr_t)1u);
            }
        }
        kfree(pg[i].phys);
    }
}

/**
 * Compose the scene `s` row by row into Linux's pixel_argb_u16 on an opaque
 * black background, each plane blended over those below as premultiplied,
 * the gamma table applied, and return the CRC of the rows in `*crc`, as
 * Linux 6.12's blend and compose_active_planes do. 0, or -ENOMEM. May
 * sleep. Called with the big lock held, which it holds again on return.
 *
 * A frame of 1024x768 takes some 11 ms of the testing PC's processor, and in
 * a guest under load more than a frame of 60 Hz. Linux composes under no
 * global lock (vkms_composer_worker), and so does this composer: under the
 * big lock it allocates its rows and looks up and pins the frames each
 * plane's framebuffer covers, as Linux's prepare_fb maps them before the
 * composer runs; then it lets the lock go for the rows, so that neither
 * waits for the other's system calls, and takes it again to unpin. What it
 * reads stays held by `s` and by the pins; it gives way between rows to a
 * thread queued on its processor. A frame that takes no pin is a device's,
 * which a reset of the device may take back; a scene with one keeps the
 * lock for the whole composition and looks such frames up at each read.
 */
static int32_t vkms_compose(const struct vkms_scene *s, uint32_t *crc)
{
    uint32_t w = s->width, ry, i, col, maxrow = 1, lut_n = 0, fw;
    uint16_t *out, *stage;
    uint8_t *row;
    uint64_t ratio = 0;
    const struct drm_color_lut *lut = NULL;
    const struct vkms_frame *f;
    struct vkms_pages pg[VKMS_MAX_PLANES];
    int32_t x0, rc = 0, pinned;
    int depth;

    for (i = 0; i < s->nframes; i++) {
        f = &s->frames[i];
        fw = (uint32_t)(f->dst.x2 - f->dst.x1);
        if (fw * f->fb->info->cpp[0] > maxrow) {
            maxrow = fw * f->fb->info->cpp[0];
        }
    }
    out = (uint16_t *)kmalloc(w * 8u);
    stage = (uint16_t *)kmalloc(w * 8u);
    row = (uint8_t *)kmalloc(maxrow);
    pinned = compose_pages_get(s, pg);
    if (!out || !stage || !row || pinned < 0) {
        rc = -ENOMEM;
        goto out;
    }
    if (s->gamma && s->gamma->length >= sizeof(struct drm_color_lut)) {
        lut = (const struct drm_color_lut *)s->gamma->data;
        lut_n = s->gamma->length / (uint32_t)sizeof(struct drm_color_lut);
        ratio = drm_div64((uint64_t)(lut_n - 1u) << 32, 0xffffu);
    }
    *crc = 0;
    depth = pinned ? compose_unlock() : 0;
    for (ry = 0; ry < s->height; ry++) {
        if (ry > 0) {
            compose_give_way();
        }
        for (col = 0; col < w; col++) {
            out[4u * col] = 0xffffu;
            out[4u * col + 1u] = out[4u * col + 2u] = out[4u * col + 3u] = 0;
        }
        for (i = 0; i < s->nframes; i++) {
            f = &s->frames[i];
            if ((int32_t)ry < f->dst.y1 || (int32_t)ry >= f->dst.y2) {
                continue;
            }
            fw = (uint32_t)(f->dst.x2 - f->dst.x1);
            compose_row(f, &pg[i], ry - (uint32_t)f->dst.y1, fw, stage, row);
            for (col = 0; col < fw; col++) {
                uint16_t *o, *in = stage + 4u * col;

                x0 = f->dst.x1 + (int32_t)col;
                if (x0 < 0 || x0 >= (int32_t)w) {
                    continue;
                }
                o = out + 4u * (uint32_t)x0;
                o[0] = 0xffffu;
                o[1] = blend_channel(in[1], o[1], in[0]);
                o[2] = blend_channel(in[2], o[2], in[0]);
                o[3] = blend_channel(in[3], o[3], in[0]);
            }
        }
        if (lut) {
            apply_lut(out, w, lut, lut_n, ratio);
        }
        *crc = crc32_le(*crc, (const uint8_t *)out, w * 8u);
    }
    compose_relock(depth);
out:
    compose_pages_put(s, pg);
    kfree(out);
    kfree(stage);
    kfree(row);
    return rc;
}

/** Drop what a scene holds. */
static void scene_put(struct vkms_scene *s)
{
    uint32_t i;

    for (i = 0; i < s->nframes; i++) {
        drm_framebuffer_put(s->frames[i].fb);
    }
    drm_property_blob_put(s->gamma);
    s->nframes = 0;
    s->gamma = NULL;
}

/** Take a reference on everything `s` names. */
static void scene_get(struct vkms_scene *s)
{
    uint32_t i;

    for (i = 0; i < s->nframes; i++) {
        drm_framebuffer_get(s->frames[i].fb);
    }
    drm_property_blob_get(s->gamma);
}

/* A composition the composer thread took: whether it took one, and then
 * the scene, held, and the frames from `start` to `end` it is for. */
struct vkms_job {
    struct vkms_scene scene;
    uint64_t start, end;
    int pending;
};

/**
 * Compose the scene `s` once and enter its CRC for each frame from `start`
 * to `end`, as Linux's vkms_composer_worker does, so that a composer that
 * fell behind catches up; `catchups` counts a pass that entered more than
 * one. Nothing for a scene that shows nothing. May sleep.
 */
static void compose_enter(struct vkms_device *v, const struct vkms_scene *s, uint64_t start,
                          uint64_t end)
{
    uint32_t flags, crc, entered = 0;

    if (s->active && s->width && s->height && vkms_compose(s, &crc) == 0) {
        while (start <= end) {
            flags = kirq_save();
            if (drm_crtc_add_crc_entry(&v->crtc, 1, (uint32_t)start, &crc) == 0) {
                entered++;
            }
            kirq_restore(flags);
            start++;
        }
    }
    if (entered > 1) {
        flags = spin_lock_irqsave(&v->lock);
        v->catchups++;
        spin_unlock_irqrestore(&v->lock, flags);
    }
}

/** Take the frames the vblanks asked for into `j`, with a hold on the scene
 * they show, and mark the device `composing`; nothing (`j->pending` 0) when
 * no frame is asked for or a commit composes. */
static void vkms_composer_take(struct vkms_device *v, struct vkms_job *j)
{
    uint32_t flags = spin_lock_irqsave(&v->lock);

    j->pending = v->crc_pending && !v->composing;
    if (j->pending) {
        j->start = v->frame_start;
        j->end = v->frame_end;
        v->crc_pending = 0;
        v->composing = 1;
        j->scene = v->scene;
        scene_get(&j->scene);
    }
    spin_unlock_irqrestore(&v->lock, flags);
}

/** Compose what `j` took, end the device's `composing` for a commit that
 * waits on it (`vkms_crtc_flush`), and let go of the scene. May sleep. */
static void vkms_composer_finish(struct vkms_device *v, struct vkms_job *j)
{
    uint32_t flags;

    if (!j->pending) {
        return;
    }
    compose_enter(v, &j->scene, j->start, j->end);
    flags = spin_lock_irqsave(&v->lock);
    v->composing = 0;
    spin_unlock_irqrestore(&v->lock, flags);
    wq_wake_all(&v->flush_wait);
    scene_put(&j->scene);
}

/**
 * Compose the frames the vblanks asked for, Linux's vkms_composer_worker:
 * the request and a hold on the scene taken with interrupts off, the scene
 * composed, and its CRC entered for each frame from `frame_start` to
 * `frame_end`. Nothing when no frame is asked for. The composer thread's
 * work, and the host tests'. May sleep.
 */
static void vkms_composer_run(struct vkms_device *v)
{
    struct vkms_job j;

    vkms_composer_take(v, &j);
    vkms_composer_finish(v, &j);
}

#ifndef RACH2_HOSTTEST
/** Whether a frame is asked for and no commit composes. */
static int composer_ready(void *arg)
{
    const struct vkms_device *v = (const struct vkms_device *)arg;

    return v->crc_pending && !v->composing;
}

/** The composer thread, kvkms: sleep in an idle wait until a vblank asks for a
 * frame, then compose it, holding the big lock as kernel code does but for
 * the rows of the frame, which hold none (`vkms_compose`), as Linux's
 * composer holds no global lock. Like every kernel thread, it is not
 * switched by the tick (`no_tick_switch`, which `setup_process_stack` sets),
 * since a frame's last put of a framebuffer may free an imported buffer
 * through its exporter, virtio-gpu's among them; it gives way where it
 * sleeps and between frames, as Linux's vkms_composer_worker runs on a
 * workqueue whose kworker gives way between works (kernel/workqueue.c:
 * 3298-3299 in 6.12.115), and between the rows of a frame. */
static void composer_main(void)
{
    struct vkms_device *v = vkms_dev;

    arch_irq_enable();
    for (;;) {
        sched_service_point();
        (void)wq_wait_event_idle(&v->composer_wait, composer_ready, v);
        vkms_composer_run(v);
    }
}

/** Start the composer thread on the first CRC source that needs it: 0, or
 * -ENOMEM. */
static int32_t composer_start(struct vkms_device *v)
{
    process_t *p;
    uintptr_t base;
    uint32_t flags;

    if (v->composer_started) {
        return 0;
    }
    flags = kirq_save();
    base = kstack_alloc();
    p = base ? alloc_process() : NULL;
    if (!p) {
        if (base) {
            kstack_free(base + KERNEL_STACK_SIZE);
        }
        kirq_restore(flags);
        klog(KLOG_ERR, "vkms", "composer: refused: no record or stack for it");
        return -ENOMEM;
    }
    p->kernel_stack = base + KERNEL_STACK_SIZE;
    p->space_root = 0;
    p->page_dir = NULL;
    p->parent_pid = 0;
    proc_set_comm(p, "kvkms");
    setup_process_stack(p, composer_main);
    v->composer_started = 1;
    sched_make_ready(p);
    kirq_restore(flags);
    klog(KLOG_DEBUG, "vkms", "composer: kvkms is pid %d", (int)p->pid);
    return 0;
}
#else
/** The host tests run the composer themselves (`vkms_composer_run`). */
static int32_t composer_start(struct vkms_device *v)
{
    v->composer_started = 1;
    return 0;
}
#endif

/* ------------------------------------------------------------------------
 * The vblank timer and the CRC source
 */

/**
 * One vblank, Linux's vkms_vblank_simulate: counted by the core, then,
 * while the CRC source is on and the CRTC shows a scene, a frame asked of
 * the composer under `lock`: from this one, or from the first the composer
 * has yet to take, to this one. Frames the count passed since the last one
 * asked, as a late pass of the timer leaves, get no CRC unless a request
 * was still pending to cover them, as on Linux; `crc_missed` counts them.
 * Neither of the device's locks is held.
 */
static void vkms_vblank(struct vkms_device *v)
{
    uint64_t frame;
    uint32_t flags;
    int asked = 0;

    (void)drm_crtc_handle_vblank(&v->crtc);
    flags = spin_lock_irqsave(&v->lock);
    if (!v->composer_enabled || !v->scene.active) {
        v->crc_next = 0;
    } else {
        frame = drm_crtc_vblank_count_and_time(&v->crtc, NULL);
        if (!v->crc_pending) {
            if (v->crc_next != 0 && frame > v->crc_next) {
                v->crc_missed += (uint32_t)(frame - v->crc_next);
            }
            v->frame_start = frame;
        }
        v->frame_end = frame;
        v->crc_next = frame + 1u;
        v->crc_pending = 1;
        asked = 1;
    }
    spin_unlock_irqrestore(&v->lock, flags);
    if (asked) {
        wq_wake_all(&v->composer_wait);
    }
}

void vkms_tick(void)
{
    struct vkms_device *v = vkms_dev;
    int64_t now, next;
    uint64_t n;
    uint32_t flags;
    int due = 0;

    if (!v) {
        return;
    }
    now = vkms_now();
    flags = spin_lock_irqsave(&v->timer_lock);
    if (v->timer_on && now >= v->next_ns) {
        n = drm_div64((uint64_t)(now - v->next_ns), (uint64_t)v->period_ns) + 1u;
        if (n != 1u) {
            v->overruns++;
            v->skipped += (uint32_t)(n - 1u);
        }
        v->next_ns += (int64_t)n * v->period_ns;
        due = 1;
    }
    spin_unlock_irqrestore(&v->timer_lock, flags);
    if (due) {
        vkms_vblank(v);
    }
    flags = spin_lock_irqsave(&v->timer_lock);
    next = v->timer_on ? v->next_ns : 0;
    spin_unlock_irqrestore(&v->timer_lock, flags);
    timer_service_wake_at(next);
}

/**
 * The time of the latest vblank, Linux's vkms_get_vblank_timestamp: while
 * the vblanks are on, the timer's expiry less a frame, since the tick moves
 * the expiry on before it handles the vblank, so that each vblank is
 * stamped with the end of its period and not with the time the tick ran,
 * which a tick of 4 ms puts 16 or 20 ms apart for a frame of 16.67 ms, and
 * the core counts the frames a late pass skipped from the stamps; otherwise
 * the clock now. Under the device's `vbl_lock`. Answers 1.
 */
static int vkms_get_vblank_timestamp(struct drm_crtc *crtc, int64_t *time_ns, int in_vblank_irq)
{
    struct vkms_device *v = to_vkms(crtc->dev);
    uint32_t flags = spin_lock_irqsave(&v->timer_lock);
    int on = crtc->vblank.enabled && v->timer_on;
    int64_t t = v->next_ns - v->period_ns;

    spin_unlock_irqrestore(&v->timer_lock, flags);
    (void)in_vblank_irq;
    *time_ns = on ? t : vkms_now();
    return 1;
}

/** Start the timer, Linux's vkms_enable_vblank: the first vblank a frame of
 * the CRTC's mode from now, the core's `framedur_ns`, or of 60 Hz for a
 * mode with no clock. 0. */
static int32_t vkms_enable_vblank(struct drm_crtc *crtc)
{
    struct vkms_device *v = to_vkms(crtc->dev);
    int64_t period = crtc->vblank.framedur_ns ? crtc->vblank.framedur_ns : 16666667;
    int64_t next = vkms_now() + period;
    uint32_t flags;

    flags = spin_lock_irqsave(&v->timer_lock);
    v->period_ns = period;
    v->next_ns = next;
    v->timer_on = 1;
    spin_unlock_irqrestore(&v->timer_lock, flags);
    timer_service_wake_at(next);
    return 0;
}

/** Stop the timer; from a commit or from inside the tick's vblank. */
static void vkms_disable_vblank(struct drm_crtc *crtc)
{
    struct vkms_device *v = to_vkms(crtc->dev);
    uint32_t flags;

    flags = spin_lock_irqsave(&v->timer_lock);
    v->timer_on = 0;
    spin_unlock_irqrestore(&v->timer_lock, flags);
}

/* The CRC sources, Linux's pipe_crc_sources. */
static const char *const crc_sources[] = {"auto"};

/** Whether `name` is a source: 0 with `*enabled` set for "auto", 0 with it
 * clear for NULL, -EINVAL otherwise, Linux's vkms_crc_parse_source. */
static int32_t crc_parse_source(const char *name, int *enabled)
{
    *enabled = 0;
    if (!name) {
        return 0;
    }
    if (name[0] == 'a' && name[1] == 'u' && name[2] == 't' && name[3] == 'o' && name[4] == '\0') {
        *enabled = 1;
        return 0;
    }
    return -EINVAL;
}

/** The CRTC's `verify_crc_source`: one value a frame. */
static int32_t vkms_verify_crc_source(struct drm_crtc *crtc, const char *name, uint32_t *values)
{
    int enabled;

    (void)crtc;
    if (crc_parse_source(name, &enabled) != 0) {
        return -EINVAL;
    }
    *values = 1;
    return 0;
}

/**
 * The CRTC's `set_crc_source`, Linux's vkms_set_crc_source: the composer
 * on for "auto", with a hold on the vblanks that keeps the timer running
 * and the composer thread started, and off for NULL or a source that is
 * not one, which is -EINVAL. 0, or -ENOMEM when the thread cannot start.
 */
static int32_t vkms_set_crc_source(struct drm_crtc *crtc, const char *name)
{
    struct vkms_device *v = to_vkms(crtc->dev);
    int32_t rc;
    uint32_t flags;
    int enabled;

    rc = crc_parse_source(name, &enabled);
    if (rc == 0 && enabled) {
        rc = composer_start(v);
        enabled = rc == 0;
    }
    if (enabled && !v->composer_hold && drm_crtc_vblank_get(crtc) == 0) {
        v->composer_hold = 1;
    }
    flags = spin_lock_irqsave(&v->lock);
    v->composer_enabled = enabled;
    if (!enabled) {
        v->crc_pending = 0;
    }
    v->crc_next = enabled && v->scene.active ? drm_crtc_vblank_count_and_time(crtc, NULL) + 1u : 0;
    spin_unlock_irqrestore(&v->lock, flags);
    if (!enabled && v->composer_hold) {
        v->composer_hold = 0;
        drm_crtc_vblank_put(crtc);
    }
    return rc;
}

/** The CRTC's `get_crc_sources`. */
static const char *const *vkms_get_crc_sources(struct drm_crtc *crtc, uint32_t *count)
{
    (void)crtc;
    *count = (uint32_t)(sizeof(crc_sources) / sizeof(crc_sources[0]));
    return crc_sources;
}

/* ------------------------------------------------------------------------
 * The display
 */

/** A plane's check, Linux's vkms_plane_atomic_check: on its CRTC,
 * unscaled, anywhere, the CRTC's state taken into the commit. */
static int32_t vkms_plane_check(struct drm_plane *plane, struct drm_atomic_state *state)
{
    struct drm_plane_state *ps = drm_atomic_get_new_plane_state(state, plane);
    struct drm_crtc_state *cs = NULL;

    if (ps->fb && ps->crtc) {
        cs = drm_atomic_get_crtc_state(state, ps->crtc);
        if (!cs) {
            return -ENOMEM;
        }
    }
    return drm_atomic_helper_check_plane_state(ps, cs, DRM_PLANE_NO_SCALING, DRM_PLANE_NO_SCALING,
                                               1, 1);
}

/** Whether nobody composes for the device at `arg`. */
static int composer_idle(void *arg)
{
    return !((const struct vkms_device *)arg)->composing;
}

/**
 * The CRTC's flush, Linux's vkms_crtc_atomic_flush: the new states become
 * the scene the composer composes, each visible plane of the CRTC with its
 * framebuffer held, bottom first, under the device's `lock`. As Linux's
 * vkms_atomic_commit_tail flushes the replaced state's composer work, the
 * commit waits out a composition the composer thread began, then marks the
 * device `composing` while it swaps the scene and composes the frames asked
 * for before it from the scene it replaces, so that the CRCs keep frame
 * order and none of the old scene is entered after the commit returns. The
 * replaced scene is let go after that. May sleep.
 */
static void vkms_crtc_flush(struct drm_crtc *crtc, struct drm_atomic_state *state)
{
    struct vkms_device *v = to_vkms(crtc->dev);
    struct vkms_scene s, old;
    const struct drm_plane_state *ps;
    struct vkms_frame *f;
    uint64_t start, end;
    uint32_t i, flags;
    int pending;

    (void)state;
    k_memset(&s, 0, sizeof(s));
    s.active = crtc->state->active;
    s.width = crtc->state->mode.hdisplay;
    s.height = crtc->state->mode.vdisplay;
    s.gamma = crtc->state->gamma_lut;
    for (i = 0; i < v->nplanes; i++) {
        ps = v->planes[i].state;
        if (!ps || ps->crtc != crtc || !ps->fb || !ps->visible) {
            continue;
        }
        f = &s.frames[s.nframes++];
        f->fb = ps->fb;
        f->sx = ps->src.x1 >> 16;
        f->sy = ps->src.y1 >> 16;
        f->dst = ps->dst;
        f->rotation = ps->rotation;
    }
    scene_get(&s);
    for (;;) {
        (void)wq_wait_event_nointr(&v->flush_wait, composer_idle, v);
        flags = spin_lock_irqsave(&v->lock);
        if (!v->composing) {
            break;
        }
        spin_unlock_irqrestore(&v->lock, flags);
    }
    v->composing = 1;
    old = v->scene;
    v->scene = s;
    if (old.active != s.active || drm_atomic_crtc_needs_modeset(crtc->state)) {
        v->crc_next = 0;
    }
    pending = v->crc_pending;
    start = v->frame_start;
    end = v->frame_end;
    v->crc_pending = 0;
    spin_unlock_irqrestore(&v->lock, flags);
    if (pending) {
        compose_enter(v, &old, start, end);
    }
    flags = spin_lock_irqsave(&v->lock);
    v->composing = 0;
    spin_unlock_irqrestore(&v->lock, flags);
    wq_wake_all(&v->composer_wait);
    scene_put(&old);
}

/** The device's check, Linux's vkms_atomic_check: a gamma table of more
 * than VKMS_LUT_SIZE entries is EINVAL, then the core's check. */
static int32_t vkms_atomic_check(struct drm_device *dev, struct drm_atomic_state *state)
{
    const struct drm_crtc_state *cs;
    int i;

    for (i = 0; i < DRM_MAX_CRTCS; i++) {
        cs = state->crtcs_new[i];
        if (!cs || !cs->gamma_lut || !cs->color_mgmt_changed) {
            continue;
        }
        if (cs->gamma_lut->length / sizeof(struct drm_color_lut) > VKMS_LUT_SIZE) {
            return -EINVAL;
        }
    }
    return drm_atomic_helper_check(dev, state);
}

/** The connector's modes, Linux's vkms_conn_get_modes: the DMT modes up
 * to 8192x8192, with 1024x768 preferred. */
static int vkms_get_modes(struct drm_connector *connector)
{
    struct drm_display_mode *m;
    int count = drm_add_modes_noedid(connector, XRES_MAX, YRES_MAX);

    for (m = connector->probed_modes; m; m = m->next) {
        if (m->info.hdisplay == XRES_DEF && m->info.vdisplay == YRES_DEF) {
            m->info.type |= DRM_MODE_TYPE_PREFERRED;
        }
    }
    return count;
}

static const struct drm_plane_funcs vkms_plane_funcs = {vkms_plane_check, NULL, NULL};
static const struct drm_crtc_funcs vkms_crtc_funcs = {NULL,
                                                      NULL,
                                                      NULL,
                                                      NULL,
                                                      vkms_crtc_flush,
                                                      NULL,
                                                      vkms_enable_vblank,
                                                      vkms_disable_vblank,
                                                      vkms_set_crc_source,
                                                      vkms_verify_crc_source,
                                                      vkms_get_crc_sources,
                                                      NULL,
                                                      vkms_get_vblank_timestamp};
static const struct drm_connector_funcs vkms_connector_funcs = {NULL, vkms_get_modes, NULL, NULL};

/** Make the next plane, of DRM_PLANE_TYPE_* `type`, as Linux's
 * vkms_plane_init: the five formats, linear alone, and the rotation
 * property with every rotation and reflection. NULL with no room or
 * memory. */
static struct drm_plane *plane_init(struct vkms_device *v, int type)
{
    struct drm_plane *plane = &v->planes[v->nplanes];

    if (drm_universal_plane_init(&v->drm, plane, 1, &vkms_plane_funcs, vkms_formats,
                                 sizeof(vkms_formats) / sizeof(vkms_formats[0]), NULL, type) != 0 ||
        drm_plane_create_rotation_property(plane, DRM_MODE_ROTATE_0,
                                           DRM_MODE_ROTATE_MASK | DRM_MODE_REFLECT_MASK) != 0) {
        return NULL;
    }
    v->nplanes++;
    return plane;
}

/**
 * Make the display in Linux's order, vkms_modeset_init and
 * vkms_output_init: the limits, the primary plane, the overlays when
 * `overlay`, the cursor when `cursor`, the CRTC with its gamma table of 256
 * entries, the virtual connector and its virtual encoder. 0 or -errno.
 */
static int32_t vkms_kms_init(struct vkms_device *v, int cursor, int overlay)
{
    struct drm_device *dev = &v->drm;
    struct drm_mode_config *mc = &dev->mode_config;
    struct drm_plane *primary, *cur = NULL;
    int32_t err;
    int n;

    err = drm_mode_config_init(dev);
    if (err != 0) {
        return err;
    }
    mc->min_width = XRES_MIN;
    mc->min_height = YRES_MIN;
    mc->max_width = XRES_MAX;
    mc->max_height = YRES_MAX;
    mc->cursor_width = 512;
    mc->cursor_height = 512;
    mc->preferred_depth = 0;
    mc->atomic_check = vkms_atomic_check;
    primary = plane_init(v, DRM_PLANE_TYPE_PRIMARY);
    if (!primary) {
        return -ENOMEM;
    }
    for (n = 0; overlay && n < NUM_OVERLAY_PLANES; n++) {
        if (!plane_init(v, DRM_PLANE_TYPE_OVERLAY)) {
            return -ENOMEM;
        }
    }
    if (cursor && !(cur = plane_init(v, DRM_PLANE_TYPE_CURSOR))) {
        return -ENOMEM;
    }
    if ((err = drm_crtc_init_with_planes(dev, &v->crtc, primary, cur, &vkms_crtc_funcs)) != 0 ||
        (err = drm_mode_crtc_set_gamma_size(&v->crtc, VKMS_LUT_SIZE)) != 0) {
        return err;
    }
    drm_crtc_enable_color_mgmt(&v->crtc, 0, 0, VKMS_LUT_SIZE);
    if ((err = drm_connector_init(dev, &v->connector, &vkms_connector_funcs,
                                  DRM_MODE_CONNECTOR_VIRTUAL)) != 0 ||
        (err = drm_encoder_init(dev, &v->encoder, DRM_MODE_ENCODER_VIRTUAL)) != 0) {
        return err;
    }
    v->encoder.possible_crtcs = 1;
    return drm_connector_attach_encoder(&v->connector, &v->encoder);
}

/** The driver's `release`, after a failed bind: the scene, the display's
 * objects and the record. A bound device is never released. */
static void vkms_release(struct drm_device *dev)
{
    struct vkms_device *v = to_vkms(dev);

    scene_put(&v->scene);
    drm_mode_config_cleanup(dev);
    kfree(v);
}

static const struct drm_driver vkms_driver = {DRM_IDENT_VKMS, NULL, NULL, drm_gem_shmem_dumb_create,
                                              NULL,           NULL, NULL, 0,
                                              vkms_release,   NULL, NULL};

/**
 * A boolean module parameter of Linux's, `key` on the command line, read
 * as kstrtobool reads it: y, Y, 1 or on for true, n, N, 0 or off for false.
 * `def` when it is absent or neither.
 */
static int vkms_param(const char *key, int def)
{
    char val[8];

    if (!kcmdline_value(key, val, sizeof(val))) {
        return def;
    }
    switch (val[0]) {
    case 'y':
    case 'Y':
    case '1':
        return 1;
    case 'n':
    case 'N':
    case '0':
        return 0;
    case 'o':
    case 'O':
        return val[1] == 'n' || val[1] == 'N' ? 1 : val[1] == 'f' || val[1] == 'F' ? 0 : def;
    default:
        return def;
    }
}

int32_t vkms_probe(void)
{
    struct vkms_device *v;
    int cursor, overlay;
    int32_t err;

    if (!kcmdline_has("rach2.vkms")) {
        return -ENODEV;
    }
    if (vkms_dev) {
        return -EBUSY;
    }
    cursor = vkms_param("vkms.enable_cursor", 1);
    overlay = vkms_param("vkms.enable_overlay", 0);
    v = (struct vkms_device *)kzalloc(sizeof(*v));
    if (!v) {
        return -ENOMEM;
    }
    drm_dev_init(&v->drm, &vkms_driver);
    v->drm.driver_priv = v;
    spin_lock_init(&v->timer_lock, "vkms_timer_lock");
    spin_lock_init(&v->lock, "vkms_lock");
    wait_queue_init(&v->composer_wait);
    wait_queue_init(&v->flush_wait);
    err = vkms_kms_init(v, cursor, overlay);
    if (err == 0) {
        err = drm_dev_register(&v->drm);
    }
    if (err != 0) {
        klog(KLOG_ERR, "vkms", "register: error %d", (int)err);
        drm_dev_put(&v->drm);
        return err;
    }
    vkms_dev = v;
    klog(KLOG_INFO, "vkms", "registered: vkms, %u planes%s%s, card %d", (unsigned)v->nplanes,
         cursor ? ", cursor" : "", overlay ? ", overlays" : "", v->drm.index);
    return 1;
}

void vkms_stats(uint32_t *overruns, uint32_t *skipped, uint32_t *missed, uint32_t *catchups)
{
    struct vkms_device *v = vkms_dev;
    uint32_t flags;

    *overruns = *skipped = *missed = *catchups = 0;
    if (!v) {
        return;
    }
    flags = spin_lock_irqsave(&v->timer_lock);
    *overruns = v->overruns;
    *skipped = v->skipped;
    spin_unlock_irqrestore(&v->timer_lock, flags);
    flags = spin_lock_irqsave(&v->lock);
    *missed = v->crc_missed;
    *catchups = v->catchups;
    spin_unlock_irqrestore(&v->lock, flags);
}

/** Return the next service deadline for the stopped idle tick. */
uint32_t vkms_idle_ticks(void)
{
    struct vkms_device *v = vkms_dev;
    int64_t gap, next;
    uint32_t flags;
    int on;

    if (!v) {
        return TICK_HZ;
    }
    flags = spin_lock_irqsave(&v->timer_lock);
    on = v->timer_on;
    next = v->next_ns;
    spin_unlock_irqrestore(&v->timer_lock, flags);
    if (!on) {
        return TICK_HZ;
    }
    gap = next - vkms_now();
    return gap <= 0 ? 1u
           : gap >= (int64_t)1000000000
               ? TICK_HZ
               : (uint32_t)tick_div64((uint64_t)gap + TICK_NS - 1u, TICK_NS, NULL);
}
