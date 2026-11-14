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
#include "../lib/kirq.h"
#include "../lib/kstring.h"
#include "../mm/kheap.h"
#include "../mm/paging.h"
#include "../rtc.h"

#ifndef RACH2_HOSTTEST
#include "../arch/arch.h"
#include "../proc/process.h"
#include "../proc/sched.h"
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

/* The vblanks one tick counts at most before the timer gives up the ones
 * it missed, as an hrtimer that overran does. */
#define VKMS_MAX_CATCHUP 8

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
 * moves on by a period before it handles the vblank, as Linux's
 * hrtimer_forward does, so that the vblank being handled ended at
 * `next_ns - period_ns`. `scene` is what
 * the last commit programmed. While `composer_enabled`, the CRC source is
 * on: each vblank asks for the frames from `frame_start` to `frame_end`
 * (`crc_pending`), which the composer thread composes and enters. The tick
 * changes the timer and the request with interrupts off; everyone else
 * changes them with interrupts off too.
 */
struct vkms_device {
    struct drm_device drm;
    struct drm_plane planes[VKMS_MAX_PLANES];
    uint32_t nplanes;
    struct drm_crtc crtc;
    struct drm_encoder encoder;
    struct drm_connector connector;
    int timer_on;
    int64_t period_ns, next_ns;
    uint32_t overruns;
    struct vkms_scene scene;
    int composer_enabled, composer_hold;
    int crc_pending;
    uint64_t frame_start, frame_end;
    wait_queue_t composer_wait;
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

/* Linux's crc32_le table, of the reflected polynomial 0xedb88320. */
static uint32_t crc_table[256];

/** Fill the CRC table once. */
static void crc_init(void)
{
    uint32_t i, c;
    int k;

    if (crc_table[1]) {
        return;
    }
    for (i = 0; i < 256; i++) {
        c = i;
        for (k = 0; k < 8; k++) {
            c = (c & 1) ? (c >> 1) ^ 0xedb88320u : c >> 1;
        }
        crc_table[i] = c;
    }
}

/** Linux's crc32_le: `crc` carried over `len` bytes, with no inversion. */
static uint32_t crc32_le(uint32_t crc, const uint8_t *p, uint32_t len)
{
    while (len-- > 0) {
        crc = (crc >> 8) ^ crc_table[(crc ^ *p++) & 0xffu];
    }
    return crc;
}

/**
 * Read `len` bytes of the shmem object `obj` from byte `off` into `dst`, a
 * page at a time through a temporary mapping; a page that cannot be had
 * reads as zeros. May sleep.
 */
static void vkms_read(struct drm_gem_object *obj, uint64_t off, uint8_t *dst, uint32_t len)
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
 * reading the framebuffer through `row`, room for a row of the part shown.
 * A destination pixel (u, v) shows the pixel of the part shown that the
 * rotation, undone, and then the reflection, undone, name, as Linux's
 * drm_rect_rotate turns a rectangle: reflect first, then rotate
 * counterclockwise.
 */
static void compose_row(const struct vkms_frame *f, uint32_t v, uint32_t w, uint16_t *stage,
                        uint8_t *row)
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
            vkms_read(fb->obj[0],
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
    vkms_read(fb->obj[0],
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

/**
 * Compose the scene `s` row by row into Linux's pixel_argb_u16 on an opaque
 * black background, each plane blended over those below as premultiplied,
 * the gamma table applied, and return the CRC of the rows in `*crc`, as
 * Linux 6.12's blend and compose_active_planes do. 0, or -ENOMEM. May
 * sleep.
 */
static int32_t vkms_compose(const struct vkms_scene *s, uint32_t *crc)
{
    uint32_t w = s->width, ry, i, col, maxrow = 1, lut_n = 0, fw;
    uint16_t *out, *stage;
    uint8_t *row;
    uint64_t ratio = 0;
    const struct drm_color_lut *lut = NULL;
    const struct vkms_frame *f;
    int32_t x0;

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
    if (!out || !stage || !row) {
        kfree(out);
        kfree(stage);
        kfree(row);
        return -ENOMEM;
    }
    if (s->gamma && s->gamma->length >= sizeof(struct drm_color_lut)) {
        lut = (const struct drm_color_lut *)s->gamma->data;
        lut_n = s->gamma->length / (uint32_t)sizeof(struct drm_color_lut);
        ratio = drm_div64((uint64_t)(lut_n - 1u) << 32, 0xffffu);
    }
    crc_init();
    *crc = 0;
    for (ry = 0; ry < s->height; ry++) {
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
            compose_row(f, ry - (uint32_t)f->dst.y1, fw, stage, row);
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
    kfree(out);
    kfree(stage);
    kfree(row);
    return 0;
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

/**
 * Compose the frames the vblanks asked for, Linux's vkms_composer_worker:
 * the request and a copy of the scene taken with interrupts off, the
 * scene composed, and its CRC entered for each frame from `frame_start` to
 * `frame_end`, so that a composer that fell behind catches up. Nothing
 * when no frame is asked for. The composer thread's work, and the host
 * tests'. May sleep.
 */
static void vkms_composer_run(struct vkms_device *v)
{
    struct vkms_scene s;
    uint64_t start, end;
    uint32_t flags, crc;
    int pending;

    flags = kirq_save();
    pending = v->crc_pending;
    start = v->frame_start;
    end = v->frame_end;
    v->crc_pending = 0;
    s = v->scene;
    scene_get(&s);
    kirq_restore(flags);
    if (pending && s.active && s.width && s.height && vkms_compose(&s, &crc) == 0) {
        while (start <= end) {
            flags = kirq_save();
            (void)drm_crtc_add_crc_entry(&v->crtc, 1, (uint32_t)start, &crc);
            kirq_restore(flags);
            start++;
        }
    }
    scene_put(&s);
}

#ifndef RACH2_HOSTTEST
/** Whether a frame is asked for. */
static int composer_ready(void *arg)
{
    return ((struct vkms_device *)arg)->crc_pending;
}

/** The composer thread, kvkms: sleep in an idle wait until a vblank asks for a
 * frame, then compose it, holding the big lock as kernel code does. Like every
 * kernel thread, it is not switched by the tick (`no_tick_switch`, which
 * `setup_process_stack` sets), since a frame's last put of a framebuffer may
 * free an imported buffer through its exporter, virtio-gpu's among them; it
 * gives way where it sleeps and between frames, as Linux's vkms_composer_worker
 * runs on a workqueue whose kworker gives way only between works
 * (kernel/workqueue.c:3298-3299 in 6.12.115). */
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
 * the composer: from this one, or from the first the composer has yet to
 * take, to this one. With interrupts off.
 */
static void vkms_vblank(struct vkms_device *v)
{
    uint64_t frame;

    (void)drm_crtc_handle_vblank(&v->crtc);
    if (!v->composer_enabled || !v->scene.active) {
        return;
    }
    frame = drm_crtc_vblank_count_and_time(&v->crtc, NULL);
    if (!v->crc_pending) {
        v->frame_start = frame;
    }
    v->frame_end = frame;
    v->crc_pending = 1;
    wq_wake_all(&v->composer_wait);
}

void vkms_tick(void)
{
    struct vkms_device *v = vkms_dev;
    int64_t now;
    int n = 0;

    if (!v || !v->timer_on) {
        return;
    }
    now = vkms_now();
    while (v->timer_on && now >= v->next_ns) {
        if (++n > VKMS_MAX_CATCHUP) {
            v->next_ns = now + v->period_ns;
            v->overruns++;
            break;
        }
        v->next_ns += v->period_ns;
        vkms_vblank(v);
    }
    if (v->timer_on) {
        timer_service_wake_at(v->next_ns);
    }
}

/**
 * The time of the latest vblank, Linux's vkms_get_vblank_timestamp: while
 * the vblanks are on, the timer's expiry less a frame, since the tick moves
 * the expiry on before it handles the vblank, so that each vblank is
 * stamped with the end of its period and not with the time the tick ran,
 * which a tick of 4 ms puts 16 or 20 ms apart for a frame of 16.67 ms;
 * otherwise the clock now. Under the device's `vbl_lock`. Answers 1.
 */
static int vkms_get_vblank_timestamp(struct drm_crtc *crtc, int64_t *time_ns, int in_vblank_irq)
{
    struct vkms_device *v = to_vkms(crtc->dev);

    (void)in_vblank_irq;
    if (!crtc->vblank.enabled || !v->timer_on) {
        *time_ns = vkms_now();
        return 1;
    }
    *time_ns = v->next_ns - v->period_ns;
    return 1;
}

/**
 * The length of a frame of `mode` in nanoseconds, Linux's
 * drm_calc_timestamping_constants: the pixels of a frame, blanking
 * included, over the clock, halved for an interlaced mode; a frame of
 * 60 Hz for a mode with no clock.
 */
static int64_t frame_ns(const struct drm_mode_modeinfo *mode)
{
    uint64_t ns;

    if (mode->clock == 0 || mode->htotal == 0 || mode->vtotal == 0) {
        return 16666667;
    }
    ns = drm_div64((uint64_t)mode->htotal * mode->vtotal * 1000000u, mode->clock);
    if (mode->flags & DRM_MODE_FLAG_INTERLACE) {
        ns /= 2;
    }
    return ns ? (int64_t)ns : 1;
}

/** Start the timer, Linux's vkms_enable_vblank: the first vblank a frame of
 * the CRTC's mode from now. 0. */
static int32_t vkms_enable_vblank(struct drm_crtc *crtc)
{
    struct vkms_device *v = to_vkms(crtc->dev);
    int64_t period = frame_ns(&crtc->state->mode);
    uint32_t flags;

    flags = kirq_save();
    v->period_ns = period;
    v->next_ns = vkms_now() + period;
    v->timer_on = 1;
    kirq_restore(flags);
    timer_service_wake_at(v->next_ns);
    return 0;
}

/** Stop the timer; from a commit or from inside the tick's vblank. */
static void vkms_disable_vblank(struct drm_crtc *crtc)
{
    struct vkms_device *v = to_vkms(crtc->dev);
    uint32_t flags;

    flags = kirq_save();
    v->timer_on = 0;
    kirq_restore(flags);
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
    flags = kirq_save();
    v->composer_enabled = enabled;
    if (!enabled) {
        v->crc_pending = 0;
    }
    kirq_restore(flags);
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

/**
 * The CRTC's flush, Linux's vkms_crtc_atomic_flush: the new states become
 * the scene the composer composes, each visible plane of the CRTC with its
 * framebuffer held, bottom first. The scene it replaces is let go once the
 * new one is in place, with interrupts off between.
 */
static void vkms_crtc_flush(struct drm_crtc *crtc, struct drm_atomic_state *state)
{
    struct vkms_device *v = to_vkms(crtc->dev);
    struct vkms_scene s, old;
    const struct drm_plane_state *ps;
    struct vkms_frame *f;
    uint32_t i, flags;

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
    flags = kirq_save();
    old = v->scene;
    v->scene = s;
    kirq_restore(flags);
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

static const struct drm_driver vkms_driver = {
    DRM_IDENT_VKMS, NULL, NULL, drm_gem_shmem_dumb_create, NULL, NULL, NULL, 0, vkms_release};

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
    wait_queue_init(&v->composer_wait);
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

/** Return the next service deadline for the stopped idle tick. */
uint32_t vkms_idle_ticks(void)
{
    struct vkms_device *v = vkms_dev;
    int64_t gap;

    if (!v || !v->timer_on) {
        return TICK_HZ;
    }
    gap = v->next_ns - vkms_now();
    return gap <= 0 ? 1u
           : gap >= (int64_t)1000000000
               ? TICK_HZ
               : (uint32_t)tick_div64((uint64_t)gap + TICK_NS - 1u, TICK_NS, NULL);
}
