/*
 * drm_client.c: the console as an in-kernel DRM client, Linux's fbdev
 * emulation: it draws on a buffer of its device that it scans out itself,
 * and gives the screen and the keyboard to a master of that device until
 * the device's last open file closes, or a display changes while no file
 * holds the role, as Linux's fbdev emulation sets its mode again only from
 * drm_lastclose and its hotplug handler.
 */

#include "drm.h"
#include "drm_blank.h"

#include "../arch/arch.h"
#include "../cmdline.h"
#include "../display/blank.h"
#include "../display/fb.h"
#include "../display/fbcon.h"
#include "../kd.h"
#include "../kio.h"
#include "../klog.h"
#include "../lib/kerrno.h"
#include "../lib/kirq.h"
#include "../lib/kstring.h"
#include "../mm/kheap.h"
#include "../mm/paging.h"
#include "../proc/process.h"
#include "../proc/sched.h"
#include "../proc/wait_queue.h"
#include "../rtc.h"
#include "../timer.h"

/*
 * The console takes the first device with mode setting and dumb buffers
 * that registers, and keeps it until it goes; the native driver that takes
 * the firmware's framebuffer registers after the firmware device has gone,
 * so the console moves to it.
 *
 * Until the device's last file first closes, the console stays where it was
 * at boot: on the loader's framebuffer, or on the VGA's text mode. When the
 * last file closes, or the outputs change while no file holds the role, the
 * console probes the connectors and sets its own mode on each connected
 * one: a buffer of XRGB8888 as large as the largest mode, which every lit
 * CRTC scans out from its top left corner, and the grid drawn in the part
 * that all of them show. Each connector's mode is the size `video=` names,
 * or else the size of the loader's screen, so that the console looks as it
 * did at boot, or else the connector's preferred mode. A CRTC with no
 * connector of the console's is turned off. All of it is one atomic
 * commit, so that a failure changes no output.
 *
 * The loader's framebuffer may go while the console is still on it: the
 * native driver that shows it through its aperture gives its device to
 * another owner (`drm_client_boot_screen_gone`). The console then draws
 * nothing, as after the loss of its own buffer, and the next device that
 * registers gets the console's mode at once.
 *
 * A driver whose buffers the kernel cannot address as one range, such as
 * virtio-gpu's of scattered pages, gives no `vmap`; there fbcon draws on a
 * shadow in the kernel's heap, and a worker, `kdrmcon`, copies the rows
 * fbcon changed (`fbcon_take_damage`) into the buffer's pages through
 * their `page` hook and flushes them to the display as DIRTYFB would,
 * CON_FLUSH_MS after the last flush at the soonest, as Linux's fbdev
 * emulation does for a shmem driver with its shadow and damage worker.
 *
 * The firmware's framebuffer (fbdrm) is the screen itself, so there the
 * console draws on it directly, as it did before the device registered,
 * and turns the device's CRTC off when it takes the screen back, which
 * takes the master's picture off the screen.
 *
 * From a master's arrival until the console takes the screen back, the
 * console is handed over: fbcon draws nothing, /fb's writes wait (fb.h's
 * `fb_hold`), and the keyboard is in K_OFF (kd.h's `kd_hold`). A program
 * that drops the role, or closes the master's file, keeps the screen while
 * another file of the device is open, so that it may give the role to
 * another of its files, as IGT's kms_flip_event_leak does, and no CRTC
 * changes under it. Taking the screen back runs in the caller's process
 * context and may sleep for the commit, and a new master may come
 * meanwhile, which `gen` tells.
 */

/* The pixel format of the console's buffer, XRGB8888: red in bits 16 to
 * 23, green in 8 to 15, blue in 0 to 7, of a 32-bit little-endian word. */
#define CON_BPP         32
#define CON_RED_SHIFT   16
#define CON_GREEN_SHIFT 8
#define CON_BLUE_SHIFT  0

/* The least time between two flushes of a shadow, Linux's fbdev deferred
 * I/O delay of HZ/20. */
#define CON_FLUSH_MS 50u

/* An output the console lights: a CRTC, the connector it drives, and the
 * mode. */
struct con_output {
    struct drm_crtc *crtc;
    struct drm_connector *connector;
    struct drm_mode_modeinfo mode;
};

/*
 * The console's state. `dev` is its device, held; `file` the kernel's own
 * file on it, through which the buffer is made, and which has no handle
 * once the buffer is made; `fb` and `obj` the buffer, held, and `screen`
 * what fbcon was last given of it. `lit` says a CRTC of the device was
 * active when the console last looked, at its probe or its placement,
 * which a blank can turn off. `on_buffer` says fbcon draws on that
 * buffer; `lost` that it did on a buffer that went, with its device or out
 * of reach, so that nothing is drawn until another buffer is set. `gen`
 * changes with every change of the master role and every device that comes
 * or goes. `shadow` is what fbcon draws on for a buffer without `vmap`,
 * `shadow_bytes` long, or NULL; `next` a shadow of another size that the
 * next attach moves fbcon to, and `spare` one it left, freed once the
 * console's lock is let go.
 */
static struct {
    struct drm_device *dev;
    int firmware;
    int handed;
    int lit;
    int on_buffer;
    int lost;
    uint32_t gen;
    struct drm_file file;
    struct drm_framebuffer *fb;
    struct drm_gem_object *obj;
    struct boot_framebuffer screen;
    uint8_t *shadow;
    uint32_t shadow_bytes;
    uint8_t *next;
    uint8_t *spare;
} con;

static wait_queue_t con_wake;
static uint64_t con_deadline; /* the tick of the next flush; 0 for none */

/* The console's blank as the worker carries it out (`drm_client_blank`):
 * whether the blank asks for the display off; what the worker has done,
 * 1 for off, 0 for on, or -1 after a run whose commit failed, which left
 * some CRTCs as asked and others not, so that either ask has work; the
 * CRTCs, one bit each by index, that it turned off and turns on again;
 * the ask whose commit failed, which the worker tries again, or -1 for
 * none; and the tick of that try. Only the worker writes `con_blank_retry`,
 * which another processor could not read whole on i686, and reads it only
 * while `con_blank_retry_ask` is set; a placement, which lights the
 * outputs anew, ends the try by setting that to -1. */
static volatile int con_blank_asked;
static int con_blank_done;
static uint32_t con_blank_crtcs;
static volatile int con_blank_retry_ask = -1;
static uint64_t con_blank_retry;

/* The milliseconds between tries of a blank's commit that failed. */
#define CON_BLANK_RETRY_MS 1000

static void con_master_changed(struct drm_device *dev, struct drm_file *master);

static struct drm_master_notifier con_notifier = {con_master_changed, NULL};
static int con_listening;

/** Read a decimal number at `*p`, moving past it; 0 for none. */
static uint32_t con_number(const char **p)
{
    uint32_t n = 0;

    while (**p >= '0' && **p <= '9' && n < 100000u) {
        n = n * 10u + (uint32_t)(**p - '0');
        (*p)++;
    }
    return n;
}

/** The size the option `video=` names, Linux's `[CONNECTOR:]WxH...`, the
 * rest of the option ignored. 1 with `*w` and `*h`, or 0 for none. */
static int con_video_size(uint32_t *w, uint32_t *h)
{
    char value[64];
    const char *p;
    uint32_t i;

    if (!kcmdline_value("video", value, sizeof(value))) {
        return 0;
    }
    p = value;
    for (i = 0; value[i]; i++) {
        if (value[i] == ':') {
            p = value + i + 1;
        }
    }
    *w = con_number(&p);
    if (*p != 'x') {
        return 0;
    }
    p++;
    *h = con_number(&p);
    return *w != 0 && *h != 0;
}

/** The first mode of `c`'s list, which the probe sorts with the preferred
 * first, that is `w` by `h`; NULL for none. */
static const struct drm_display_mode *con_mode_of_size(const struct drm_connector *c, uint32_t w,
                                                       uint32_t h)
{
    const struct drm_display_mode *m;

    for (m = c->modes; m; m = m->next) {
        if (m->info.hdisplay == w && m->info.vdisplay == h) {
            return m;
        }
    }
    return NULL;
}

/** The console's mode on `c`, as the comment at the top says; NULL for a
 * connector with no modes. */
static const struct drm_display_mode *con_pick_mode(const struct drm_connector *c)
{
    const struct boot_framebuffer *boot = fbcon_boot_screen();
    const struct drm_display_mode *m = NULL;
    uint32_t w, h;

    if (con_video_size(&w, &h)) {
        m = con_mode_of_size(c, w, h);
    }
    if (!m && (boot->type == BOOT_FB_RGB || boot->type == BOOT_FB_INDEXED)) {
        m = con_mode_of_size(c, boot->width, boot->height);
    }
    return m ? m : c->modes;
}

/**
 * Find connector `i` a CRTC in its mask `reach[i]`, by an augmenting path
 * through the CRTCs not yet in `*seen`: a free CRTC, or one whose
 * connector `crtc_of` holds can move to another. 1 when found, with
 * `crtc_of` updated; 0 otherwise. Recurses once for each CRTC at most.
 */
static int con_route(const uint32_t *reach, int *crtc_of, uint32_t num_crtc, uint32_t i,
                     uint32_t *seen)
{
    uint32_t k;

    for (k = 0; k < num_crtc; k++) {
        if (!(reach[i] & (1u << k)) || (*seen & (1u << k))) {
            continue;
        }
        *seen |= 1u << k;
        if (crtc_of[k] < 0 || con_route(reach, crtc_of, num_crtc, (uint32_t)crtc_of[k], seen)) {
            crtc_of[k] = (int)i;
            return 1;
        }
    }
    return 0;
}

/**
 * Probe `dev`'s connectors and choose the console's outputs into `outs`:
 * each connected connector with a mode, or each of unknown status when
 * none is connected, as many as can be lit at once. A connector goes
 * through its first encoder, the one the commit routes it through
 * (`single_encoder` in drm_atomic.c), so of two that share it, as
 * DisplayPort and HDMI share a DDI port's, the first is chosen; and each
 * on a CRTC that encoder reaches, found by augmenting paths, so that a
 * connector whose encoder reaches one CRTC is not left dark by an earlier
 * one that could take another, as Linux's drm_client_pick_crtcs searches;
 * an encoder that names no CRTC drives any, as the commit's routing allows.
 * A connector, once lit, stays lit as later ones are added. The number
 * chosen, in the connectors' order. Under the commit lock; may sleep in
 * the probes.
 */
static uint32_t con_outputs(struct drm_device *dev, struct con_output *outs)
{
    struct drm_mode_config *mc = &dev->mode_config;
    const struct drm_display_mode *m[DRM_MAX_CONNECTORS];
    uint32_t reach[DRM_MAX_CONNECTORS];
    int crtc_of[DRM_MAX_CRTCS];
    struct drm_encoder *e;
    struct drm_connector *c;
    uint32_t i, j, k, n = 0, taken = 0, seen;
    int want, connected = 0;

    for (i = 0; i < mc->num_connector; i++) {
        drm_helper_probe_single_connector_modes(mc->connectors[i], mc->max_width, mc->max_height);
        connected |= mc->connectors[i]->status == DRM_CONNECTOR_CONNECTED;
    }
    want = connected ? DRM_CONNECTOR_CONNECTED : DRM_CONNECTOR_UNKNOWN;
    for (k = 0; k < DRM_MAX_CRTCS; k++) {
        crtc_of[k] = -1;
    }
    for (i = 0; i < mc->num_connector; i++) {
        c = mc->connectors[i];
        m[i] = c->status == want ? con_pick_mode(c) : NULL;
        reach[i] = 0;
        j = 0;
        while (j < mc->num_encoder && !(c->possible_encoders & (1u << j))) {
            j++;
        }
        if (!m[i] || j == mc->num_encoder || (taken & (1u << j))) {
            continue;
        }
        taken |= 1u << j;
        e = mc->encoders[j];
        reach[i] = (e->possible_crtcs ? e->possible_crtcs : ~0u) & ((1u << mc->num_crtc) - 1u);
        seen = 0;
        (void)con_route(reach, crtc_of, mc->num_crtc, i, &seen);
    }
    for (i = 0; i < mc->num_connector; i++) {
        for (k = 0; k < mc->num_crtc; k++) {
            if (crtc_of[k] == (int)i) {
                outs[n].crtc = mc->crtcs[k];
                outs[n].connector = mc->connectors[i];
                outs[n].mode = m[i]->info;
                n++;
                break;
            }
        }
    }
    return n;
}

/**
 * Make a buffer of `w` by `h` pixels of XRGB8888 on `dev` and its
 * framebuffer, through the console's own file, which keeps no handle to
 * it. 0 with both in `*fbp` and `*objp`, each with a reference of the
 * caller's, or -errno. May sleep.
 */
static int32_t con_buffer_make(struct drm_device *dev, uint32_t w, uint32_t h,
                               struct drm_framebuffer **fbp, struct drm_gem_object **objp)
{
    struct drm_mode_create_dumb args;
    struct drm_mode_fb_cmd2 r;
    struct drm_gem_object *obj;
    struct drm_framebuffer *fb;
    int32_t err;

    k_memset(&args, 0, sizeof(args));
    args.width = w;
    args.height = h;
    args.bpp = CON_BPP;
    err = dev->driver->dumb_create(&con.file, dev, &args);
    if (err != 0) {
        return err;
    }
    obj = drm_gem_object_lookup(&con.file, args.handle);
    k_memset(&r, 0, sizeof(r));
    r.width = w;
    r.height = h;
    r.pixel_format = DRM_FORMAT_XRGB8888;
    r.handles[0] = args.handle;
    r.pitches[0] = args.pitch;
    fb = drm_internal_framebuffer_create(dev, &r, &con.file, &err);
    drm_gem_handle_delete(&con.file, args.handle);
    if (!obj || !fb) {
        drm_framebuffer_put(fb);
        drm_gem_object_put(obj);
        return err != 0 ? err : -ENOMEM;
    }
    *fbp = fb;
    *objp = obj;
    return 0;
}

/** Add to `state` `crtc` showing `fb` in `mode` on `connector`, or turned
 * off for NULL `mode`. Under the commit lock. 0 or -errno. */
static int32_t con_set(struct drm_atomic_state *state, struct drm_crtc *crtc,
                       struct drm_framebuffer *fb, const struct drm_mode_modeinfo *mode,
                       struct drm_connector *connector)
{
    struct drm_connector *list[1];
    struct drm_mode_set set;

    list[0] = connector;
    k_memset(&set, 0, sizeof(set));
    set.crtc = crtc;
    set.fb = mode ? fb : NULL;
    set.mode = mode;
    set.connectors = mode ? list : NULL;
    set.num_connectors = mode ? 1u : 0u;
    return drm_atomic_set_config_state(&set, state);
}

/** The size of a buffer that every output of `outs` shows, `*w` by `*h`,
 * and of the part all of them show, `*vw` by `*vh`. */
static void con_extent(const struct con_output *outs, uint32_t n, uint32_t *w, uint32_t *h,
                       uint32_t *vw, uint32_t *vh)
{
    uint32_t i;

    *w = *h = 0;
    *vw = *vh = 0xFFFFu;
    for (i = 0; i < n; i++) {
        *w = outs[i].mode.hdisplay > *w ? outs[i].mode.hdisplay : *w;
        *h = outs[i].mode.vdisplay > *h ? outs[i].mode.vdisplay : *h;
        *vw = outs[i].mode.hdisplay < *vw ? outs[i].mode.hdisplay : *vw;
        *vh = outs[i].mode.vdisplay < *vh ? outs[i].mode.vdisplay : *vh;
    }
}

/** Whether a CRTC of `dev` is active. Under the commit lock. */
static int con_any_active(struct drm_device *dev)
{
    struct drm_mode_config *mc = &dev->mode_config;
    uint32_t k;

    for (k = 0; k < mc->num_crtc; k++) {
        if (mc->crtcs[k]->state && mc->crtcs[k]->state->active) {
            return 1;
        }
    }
    return 0;
}

/** Commit `fb` on each CRTC of `dev` that `outs` names, and turn off every
 * other CRTC that is on, in one atomic commit, as Linux's
 * drm_client_modeset_commit_atomic, so that a failure changes no output
 * and leaves none scanning out a buffer the console does not draw on.
 * Under the commit lock; may sleep. 0 or the commit's error. */
static int32_t con_light(struct drm_device *dev, const struct con_output *outs, uint32_t n,
                         struct drm_framebuffer *fb)
{
    struct drm_mode_config *mc = &dev->mode_config;
    struct drm_atomic_state *state = drm_atomic_state_alloc(dev);
    const struct con_output *out;
    struct drm_crtc *crtc;
    uint32_t i, k;
    int32_t err = state ? 0 : -ENOMEM;

    for (k = 0; err == 0 && k < mc->num_crtc; k++) {
        crtc = mc->crtcs[k];
        out = NULL;
        for (i = 0; i < n; i++) {
            if (outs[i].crtc == crtc) {
                out = &outs[i];
            }
        }
        if (out || (crtc->state && crtc->state->enable)) {
            err = con_set(state, crtc, fb, out ? &out->mode : NULL, out ? out->connector : NULL);
        }
    }
    if (err == 0) {
        err = drm_atomic_commit_config(state);
    }
    if (state) {
        drm_atomic_state_free(state);
    }
    return err;
}

/** Describe the console's buffer to fbcon in `*screen`: XRGB8888 rows of
 * the buffer's pitch, of which the top left `vw` by `vh` pixels show. */
static void con_describe(struct boot_framebuffer *screen, uint32_t vw, uint32_t vh)
{
    k_memset(screen, 0, sizeof(*screen));
    screen->pitch = con.fb->pitches[0];
    screen->width = vw;
    screen->height = vh;
    screen->bpp = CON_BPP;
    screen->type = BOOT_FB_RGB;
    screen->red_shift = CON_RED_SHIFT;
    screen->red_bits = 8;
    screen->green_shift = CON_GREEN_SHIFT;
    screen->green_bits = 8;
    screen->blue_shift = CON_BLUE_SHIFT;
    screen->blue_bits = 8;
}

/** The kernel's address of the console's buffer, or NULL for one the
 * driver cannot map. */
static volatile uint8_t *con_vmap(void)
{
    return con.obj->funcs && con.obj->funcs->vmap ? con.obj->funcs->vmap(con.obj) : NULL;
}

/** Where fbcon draws for the console's buffer: the buffer itself, or its
 * shadow. */
static volatile uint8_t *con_base(void)
{
    volatile uint8_t *v = con_vmap();

    return v != NULL ? v : con.shadow;
}

/**
 * A shadow for the console's buffer, `h` rows of its pitch, in `*base`:
 * the one fbcon has when it is that size, else a new one in `con.next`,
 * zeroed. 0, -ENODEV for a device whose buffers can be neither mapped nor
 * flushed (no `page` hook or no `fb_dirty`), or -ENOMEM.
 */
static int32_t con_shadow_make(struct drm_device *dev, uint32_t h, volatile uint8_t **base)
{
    uint32_t bytes = con.fb->pitches[0] * h;

    if (!con.obj->funcs || !con.obj->funcs->page || !dev->mode_config.fb_dirty) {
        return -ENODEV;
    }
    if (con.shadow && con.shadow_bytes == bytes) {
        *base = con.shadow;
        return 0;
    }
    kfree(con.next);
    con.next = (uint8_t *)kzalloc(bytes);
    if (!con.next) {
        return -ENOMEM;
    }
    *base = con.next;
    return 0;
}

/** The console's buffer for `w` by `h` on `dev` in `*fb` and `*obj`, with a
 * reference of the caller's on each: the one it has when it is that size,
 * else a new one. 0 or the error of con_buffer_make. */
static int32_t con_buffer_get(struct drm_device *dev, uint32_t w, uint32_t h,
                              struct drm_framebuffer **fb, struct drm_gem_object **obj)
{
    if (con.fb && con.fb->width == w && con.fb->height == h) {
        *fb = con.fb;
        *obj = con.obj;
        drm_framebuffer_get(*fb);
        drm_gem_object_get(*obj);
        return 0;
    }
    return con_buffer_make(dev, w, h, fb, obj);
}

/** After the commits that lit `fb`: make `fb` and `obj`, which the caller
 * holds, the console's buffer when `err` is 0 and the console still has
 * `dev`, which an unregister in the commits' sleep may have ended; else, or
 * when they are already its buffer, let the caller's references go. */
static void con_buffer_keep(struct drm_device *dev, int32_t err, struct drm_framebuffer *fb,
                            struct drm_gem_object *obj)
{
    if (!fb) {
        return;
    }
    if (err == 0 && con.dev == dev && fb != con.fb) {
        drm_framebuffer_put(con.fb);
        drm_gem_object_put(con.obj);
        con.fb = fb;
        con.obj = obj;
        return;
    }
    drm_framebuffer_put(fb);
    drm_gem_object_put(obj);
}

/**
 * Light the console's outputs on `dev`, as the comment at the top says, and
 * describe where fbcon draws in `*screen` and `*base`: the loader's
 * framebuffer and NULL on the firmware's device, else the console's buffer
 * and the kernel's address of it. `gen` is the console's generation when
 * the caller decided to place it; a master or a device that came while the
 * commit lock was waited for changed it, and the console then commits
 * nothing, as Linux's client holds the master lock across its commit. 0 or
 * -errno: -EAGAIN for a changed generation, -ENODEV for no output, a buffer
 * the kernel cannot reach or a device unregistered during the commits, or
 * the error of the buffer or the commit. A commit that succeeded lit the
 * console's outputs and turned the rest off; while the console is blank,
 * the worker is then woken and turns them off again. A commit that failed
 * changed no output. May sleep.
 */
static int32_t con_place(struct drm_device *dev, uint32_t gen, struct boot_framebuffer *screen,
                         volatile uint8_t **base)
{
    struct con_output outs[DRM_MAX_CRTCS];
    struct drm_framebuffer *fb = NULL;
    struct drm_gem_object *obj = NULL;
    uint32_t n = 0, w, h, vw, vh;
    int32_t err = 0;
    int lit = 0;

    drm_modeset_lock_nointr(dev);
    if (con.dev != dev || con.gen != gen || dev->master) {
        err = -EAGAIN;
    } else if (!con.firmware && (n = con_outputs(dev, outs)) == 0) {
        err = -ENODEV;
    }
    con_extent(outs, n, &w, &h, &vw, &vh);
    if (err == 0 && !con.firmware) {
        err = con_buffer_get(dev, w, h, &fb, &obj);
    }
    if (err == 0) {
        err = con_light(dev, outs, n, fb);
        lit = err == 0;
    }
    con_buffer_keep(dev, err, fb, obj);
    if (err == 0 && con.dev != dev) {
        err = -ENODEV; /* unregistered while the commit slept */
    }
    if (lit && con.dev == dev) {
        /* The console's outputs are lit and every other CRTC is off; a
         * blank that lasts turns them off anew, at once. */
        con.lit = con_any_active(dev);
        con_blank_done = 0;
        con_blank_crtcs = 0;
        con_blank_retry_ask = -1;
    }
    drm_modeset_unlock(dev);
    if (lit && con_blank_asked) {
        wq_wake_all(&con_wake);
    }
    if (err != 0) {
        return err;
    }
    if (con.firmware) {
        *screen = *fbcon_boot_screen();
        *base = NULL;
        return 0;
    }
    *base = con_vmap();
    if (*base == NULL && (err = con_shadow_make(dev, h, base)) != 0) {
        return err;
    }
    con_describe(screen, vw, vh);
    return 0;
}

/**
 * Move fbcon to the screen `con_place` returned, with `rc` its result: on
 * success there, and on a failure back to where it can still draw, the
 * buffer it had or the loader's framebuffer, or nowhere: a buffer that can
 * no longer be mapped is forgotten, and nothing is drawn until a later
 * place succeeds. The caller holds the console's lock. Returns `rc`, or
 * -ENODEV when fbcon refused the screen.
 */
static int32_t con_attach(int32_t rc, const struct boot_framebuffer *screen, volatile uint8_t *base)
{
    volatile uint8_t *old;

    if (rc == 0) {
        if (fbcon_attach(screen, base) != 0) {
            return -ENODEV;
        }
        con.on_buffer = !con.firmware;
        con.lost = 0;
        if (con.on_buffer) {
            con.screen = *screen;
        }
        if (con.next && base == con.next) {
            con.spare = con.shadow;
            con.shadow = con.next;
            con.shadow_bytes = con.fb->pitches[0] * con.fb->height;
            con.next = NULL;
        }
        return 0;
    }
    if (con.on_buffer) {
        old = con_base();
        if (old != NULL && fbcon_attach(&con.screen, old) == 0) {
            return rc;
        }
        fbcon_detach(1);
        con.on_buffer = 0;
        con.lost = 1;
    } else if (!con.lost && fbcon_active) {
        fbcon_attach(fbcon_boot_screen(), NULL);
    }
    return rc;
}

/** Whether a shadow waits for its flushes: the console draws on it and no
 * master holds the device. */
static int con_shadowed(void)
{
    return con.dev && con.shadow && con.on_buffer && !con.handed;
}

/** Copy the bytes of the shadow from `from` up to `to` into the console's
 * buffer, a page at a time through the buffer's `page` hook. Does not
 * sleep. */
static void con_copy(uint32_t from, uint32_t to)
{
    uint32_t at = from, n, in;
    uintptr_t phys;
    uint8_t *va;

    while (at < to) {
        in = at & (PAGE_SIZE - 1);
        n = PAGE_SIZE - in < to - at ? PAGE_SIZE - in : to - at;
        phys = con.obj->funcs->page(con.obj, at / PAGE_SIZE);
        va = phys ? (uint8_t *)kmap_temp(phys) : NULL;
        if (!va) {
            return;
        }
        k_memcpy(va + in, con.shadow + at, n);
        kunmap_temp(va);
        at += n;
    }
}

/** Copy the rows fbcon changed on the shadow into the console's buffer and
 * flush them to the display. Process context; the flush may sleep. */
static void con_flush(void)
{
    struct drm_framebuffer *fb;
    struct drm_device *dev;
    struct drm_mode_rect r;
    uint32_t top, end, flags;
    int have;

    if (!con_shadowed()) {
        return;
    }
    flags = console_lock();
    have = fbcon_take_damage(&top, &end);
    console_unlock(flags);
    if (!have) {
        return;
    }
    con_copy(top * con.fb->pitches[0], end * con.fb->pitches[0]);
    dev = con.dev;
    fb = con.fb;
    drm_dev_get(dev);
    /* A framebuffer's references and its flush are the mode lock's, which
     * a commit on another processor may hold without the big lock. */
    drm_modeset_lock_nointr(dev);
    drm_framebuffer_get(fb);
    r.x1 = 0;
    r.y1 = (int32_t)top;
    r.x2 = (int32_t)fb->width;
    r.y2 = (int32_t)end;
    (void)drm_framebuffer_dirty(dev, fb, &r);
    drm_framebuffer_put(fb);
    drm_modeset_unlock(dev);
    drm_dev_put(dev);
}

/** Whether the blank `off`, the console's ask, has work left: an ask
 * other than what the worker did, which after a failed run is either
 * (`con_blank_done` -1), or, with the display asked on, a CRTC the blank
 * turned off that a placement left off. */
static int con_blank_pending(int off)
{
    return off != con_blank_done || (!off && con_blank_crtcs != 0);
}

/** The worker's wait, whose deadline `*arg` the worker armed, 0 for none:
 * until that deadline has passed, which the wait needs once it is armed,
 * whatever changed meanwhile; until a blank has work, at once for an ask
 * other than the one that failed and else at the try's tick; until the
 * flush's tick when one is set, else until a shadow wants flushes. */
static int con_flush_ready(void *arg)
{
    uint64_t due = arg ? *(const uint64_t *)arg : 0;
    int off = con_blank_asked, retry = con_blank_retry_ask;

    if (due != 0 && rtc_ticks_now() >= due) {
        return 1;
    }
    if (retry >= 0 ? off != retry : con_blank_pending(off)) {
        return 1;
    }
    return con_deadline != 0 ? rtc_ticks_now() >= con_deadline : con_shadowed();
}

/** Commit `crtc` of `dev` with its `active` set to `active` and its mode,
 * planes and connectors kept, as Linux's drm_fb_helper_dpms does. Under
 * the commit lock; may sleep. 0 or -errno. */
static int32_t con_crtc_active(struct drm_device *dev, struct drm_crtc *crtc, int active)
{
    struct drm_atomic_state *state = drm_atomic_state_alloc(dev);
    struct drm_crtc_state *cs = NULL;
    int32_t rc = state ? drm_atomic_add_affected_connectors(state, crtc) : -ENOMEM;

    if (rc == 0) {
        cs = drm_atomic_get_crtc_state(state, crtc);
        rc = cs ? 0 : -ENOMEM;
    }
    if (rc == 0) {
        cs->active = active != 0;
        rc = drm_atomic_commit(state);
    }
    if (state) {
        drm_atomic_state_free(state);
    }
    return rc;
}

/**
 * Commit each CRTC of `dev` the blank asks for: off, when `off`, each one
 * lit, and on again each one the blank turned off. A CRTC whose commit
 * fails keeps its mark and its state, and the first failure of a run is
 * logged (`*failing`). Stops with no mark kept when the console's device,
 * its generation `gen` or the device's master changed. Under the commit
 * lock; may sleep. Returns whether a commit failed.
 */
static int con_blank_commits(struct drm_device *dev, int off, uint32_t gen, int *failing)
{
    struct drm_mode_config *mc = &dev->mode_config;
    int failed = 0;
    uint32_t k;
    int32_t rc;

    for (k = 0; k < mc->num_crtc && k < 32u; k++) {
        struct drm_crtc *crtc = mc->crtcs[k];

        if (con.dev != dev || con.gen != gen || dev->master) {
            con_blank_crtcs = 0;
            return 0;
        }
        if (off && crtc->state && crtc->state->active) {
            rc = con_crtc_active(dev, crtc, 0);
            con_blank_crtcs |= rc == 0 ? 1u << k : 0u;
        } else if (!off && (con_blank_crtcs & 1u << k) && crtc->state && crtc->state->enable) {
            rc = con_crtc_active(dev, crtc, 1);
            con_blank_crtcs &= rc == 0 ? ~(1u << k) : ~0u;
        } else {
            continue;
        }
        if (rc != 0) {
            if (!*failing) {
                klog(KLOG_WARN, "console", "card%d: crtc %u not turned %s: error %d; tried again",
                     dev->index, (unsigned)k, off ? "off" : "on", (int)rc);
            }
            failed = 1;
        }
    }
    return failed;
}

/**
 * Carry out the blank the console asked for last: turn off each lit CRTC
 * of the console's native device, or on again those turned off
 * (`con_blank_commits`). A commit that fails leaves the blank half done,
 * and the worker tries it again every CON_BLANK_RETRY_MS until it
 * succeeds or the console asks otherwise, so that a wake that failed
 * once, for memory or a busy device, does not leave the display dark
 * while the console shows itself unblanked. A run that failed leaves
 * work for either ask: a wake turns on every CRTC the blank turned off,
 * and a blank turns off every CRTC a half-failed wake lit. On the firmware's
 * framebuffer, with no device, or while a master holds the device, only
 * the record changes, and CRTCs that a master or a later placement set
 * are left to them. Process context, the worker's; may sleep.
 */
static void con_blank_service(void)
{
    static int failing;
    struct drm_device *dev = con.dev;
    int off = con_blank_asked, failed;
    uint32_t gen = con.gen;

    if (!con_blank_pending(off)) {
        con_blank_retry_ask = -1;
        return;
    }
    if (!dev || con.firmware || con.handed) {
        con_blank_done = off;
        con_blank_crtcs = 0;
        con_blank_retry_ask = -1;
        return;
    }
    drm_dev_get(dev);
    drm_modeset_lock_nointr(dev);
    failed = con_blank_commits(dev, off, gen, &failing);
    if (!off && !failed) {
        con_blank_crtcs = 0;
    }
    failing = failed;
    con_blank_done = failed ? -1 : off;
    con_blank_retry = rtc_ticks_now() + MS_TO_TICKS(CON_BLANK_RETRY_MS);
    con_blank_retry_ask = failed ? off : -1;
    drm_modeset_unlock(dev);
    if (!failed) {
        klog(KLOG_INFO, "console", "card%d: display %s for the blank", dev->index,
             off ? "off" : "on");
    }
    drm_dev_put(dev);
}

/** The tick at which the worker's wait ends, 0 for none: the try again of
 * a failed blank or the flush's tick, the sooner. The worker's. */
static uint64_t con_wait_due(void)
{
    return con_blank_retry_ask >= 0 && (con_deadline == 0 || con_blank_retry < con_deadline)
               ? con_blank_retry
               : con_deadline;
}

void drm_client_blank(int blank)
{
    con_blank_asked = blank != 0;
    wq_wake_all(&con_wake);
}

int drm_client_can_blank(void)
{
    return con.dev != NULL && !con.firmware && !con.handed && con.lit;
}

#ifndef RACH2_HOSTTEST
static process_t *con_thread;

/** The worker, kdrmcon: carry out the console's blank, and while a shadow is
 * in use, flush it every CON_FLUSH_MS; otherwise sleep in an idle wait until
 * one is or a blank asks. Like every kernel thread, it is
 * not switched by the tick (`no_tick_switch`, which `setup_process_stack`
 * sets), so a flush runs a driver's dirty path as the DIRTYFB call does, with
 * the driver's state holding through it; it gives way where it sleeps and
 * between flushes. */
static void con_thread_main(void)
{
    uint64_t due;

    arch_irq_enable();
    for (;;) {
        sched_service_point();
        con_deadline = con_shadowed() ? rtc_ticks_now() + MS_TO_TICKS(CON_FLUSH_MS) : 0;
        due = con_wait_due();
        current_proc->wakeup_tick = due;
        (void)wq_wait_event_idle(&con_wake, con_flush_ready, &due);
        current_proc->wakeup_tick = 0;
        con_blank_service();
        con_flush();
    }
}

/** Start the worker once, on the first native device or shadow. A failure
 * is logged, and a shadow is then never shown nor the display blanked. */
static void con_worker_start(void)
{
    process_t *p;
    uintptr_t stack;
    uint32_t flags;

    if (con_thread) {
        return;
    }
    flags = kirq_save();
    stack = kstack_alloc();
    p = stack ? alloc_process() : NULL;
    if (!p) {
        if (stack) {
            kstack_free(stack + KERNEL_STACK_SIZE);
        }
        kirq_restore(flags);
        klog(KLOG_ERR, "console", "worker: refused: no record or stack for it");
        return;
    }
    p->kernel_stack = stack + KERNEL_STACK_SIZE;
    p->space_root = 0;
    p->page_dir = NULL;
    p->parent_pid = 0;
    proc_set_comm(p, "kdrmcon");
    setup_process_stack(p, con_thread_main);
    con_thread = p;
    sched_make_ready(p);
    kirq_restore(flags);
}
#else
/** The host tests call `con_flush` themselves. */
static void con_worker_start(void)
{}
#endif

/** After an attach, with the console's lock let go: free the shadows fbcon
 * no longer draws on, and start the worker for one it does. */
static void con_settle(void)
{
    kfree(con.spare);
    kfree(con.next);
    con.spare = NULL;
    con.next = NULL;
    if (con_shadowed()) {
        con_worker_start();
        wq_wake_all(&con_wake);
    }
}

/** A master of `dev`, the console's device, took the screen: unblank the
 * console, as taking /fb does, so that the backlights light the master's
 * picture, then hand the screen, /fb and the keyboard over. Does not
 * sleep. */
static void con_hand_over(struct drm_device *dev)
{
    uint32_t flags;

    if (con.handed) {
        return;
    }
    con.handed = 1;
    console_unblank_now();
    flags = console_lock();
    fbcon_detach(0);
    fb_hold();
    console_unlock(flags);
    kd_hold();
    klog(KLOG_INFO, "console", "card%d: handed to a master, the keyboard off", dev->index);
}

/** No master holds `dev`, the console's device, now, and its last file
 * has closed or its outputs changed: start the countdown
 * to the console's blank again (`console_poke`), before the tick can see
 * the screen as the console's with the old count, then take the screen
 * back on the console's own outputs, /fb and the keyboard. Nothing if a
 * master comes or the device goes while the commit sleeps. */
static void con_take_back(struct drm_device *dev)
{
    struct boot_framebuffer screen;
    volatile uint8_t *base = NULL;
    uint32_t gen = con.gen, flags;
    int32_t rc;

    rc = con_place(dev, gen, &screen, &base);
    if (con.dev != dev || con.gen != gen) {
        return;
    }
    con.handed = 0;
    console_poke();
    flags = console_lock();
    rc = con_attach(rc, &screen, base);
    fb_unhold();
    console_unlock(flags);
    con_settle();
    kd_unhold();
    klog(KLOG_INFO, "console", "card%d: taken back from a master, the keyboard on", dev->index);
    if (rc != 0) {
        klog(KLOG_WARN, "console", "card%d: screen not set: error %d", dev->index, (int)rc);
    } else if (!con.firmware) {
        klog(KLOG_INFO, "console", "card%d: %ux%u, the font at %ux", dev->index,
             (unsigned)screen.width, (unsigned)screen.height, (unsigned)fbcon_scale());
    } else {
        klog(KLOG_INFO, "console", "card%d: the firmware's framebuffer again", dev->index);
    }
}

/** The master listener: hand the screen over when a master of the
 * console's device comes. One that leaves gives nothing back, as Linux's
 * DROP_MASTER and master release restore no fbdev mode: the screen comes
 * back at the device's last close or a hotplug with no master. */
static void con_master_changed(struct drm_device *dev, struct drm_file *master)
{
    if (dev != con.dev) {
        return;
    }
    con.gen++;
    if (master) {
        con_hand_over(dev);
    }
}

void drm_client_dev_lastclose(struct drm_device *dev)
{
    if (dev == con.dev && con.handed && !dev->master) {
        con_take_back(dev);
    }
}

/**
 * Probe each connector of `dev` once, as Linux's fbdev client does in its
 * initial configuration, so that sysfs and GETCONNECTOR give the displays'
 * status and modes while the console stays on the boot screen, and note
 * whether a CRTC is lit, as a driver that took the firmware's picture
 * over shows it (`con.lit`). May sleep.
 */
static void con_probe(struct drm_device *dev)
{
    struct drm_mode_config *mc = &dev->mode_config;
    uint32_t i;

    drm_modeset_lock_nointr(dev);
    for (i = 0; i < mc->num_connector; i++) {
        drm_helper_probe_single_connector_modes(mc->connectors[i], mc->max_width, mc->max_height);
    }
    con.lit = con_any_active(dev);
    drm_modeset_unlock(dev);
}

void drm_client_dev_register(struct drm_device *dev)
{
    struct boot_framebuffer screen;
    volatile uint8_t *base = NULL;
    uint32_t gen, flags;
    int32_t rc;

    if (con.dev || !drm_core_check_feature(dev, DRIVER_MODESET) || !dev->driver->dumb_create) {
        return;
    }
    if (!con_listening) {
        drm_master_notifier_register(&con_notifier);
        con_listening = 1;
    }
    drm_dev_get(dev);
    k_memset(&con.file, 0, sizeof(con.file));
    con.file.dev = dev;
    con.file.minor_type = DRM_MINOR_PRIMARY;
    con.file.console = 1;
    con.dev = dev;
    con.firmware = drm_aperture_is_firmware(dev);
    con.handed = 0;
    con.lit = 0;
    con.on_buffer = 0;
    gen = ++con.gen;
    klog(KLOG_INFO, "console", "card%d: %s", dev->index,
         con.firmware ? "the firmware's framebuffer" : "a native display");
    if (!con.firmware) {
        con_worker_start();
    }
    con_blank_done = 0;
    con_blank_crtcs = 0;
    con_blank_retry_ask = -1;
    if (con_blank_asked) {
        wq_wake_all(&con_wake);
    }
    if (!con.lost) {
        con_probe(dev);
        return;
    }
    rc = con_place(dev, gen, &screen, &base);
    if (con.dev != dev || con.gen != gen) {
        return;
    }
    flags = console_lock();
    rc = con_attach(rc, &screen, base);
    fb_screen_moved();
    console_unlock(flags);
    con_settle();
    if (rc != 0) {
        klog(KLOG_WARN, "console", "card%d: screen not set: error %d", dev->index, (int)rc);
    }
}

void drm_client_boot_screen_gone(void)
{
    uint32_t flags;

    if (con.on_buffer || con.lost) {
        return;
    }
    flags = console_lock();
    fbcon_detach(1);
    con.lost = 1;
    console_unlock(flags);
    klog(KLOG_INFO, "console", "the loader's framebuffer has gone with its device");
}

void drm_client_dev_unregister(struct drm_device *dev)
{
    int was_handed = con.handed;
    uint32_t flags;

    if (dev != con.dev) {
        return;
    }
    con.gen++;
    con.handed = 0;
    flags = console_lock();
    if (con.on_buffer) {
        fbcon_detach(1);
        con.lost = 1;
    } else if (was_handed && fbcon_active && !con.lost) {
        fbcon_attach(fbcon_boot_screen(), NULL);
    }
    if (was_handed) {
        fb_unhold();
    } else {
        fb_screen_moved();
    }
    console_unlock(flags);
    if (was_handed) {
        kd_unhold();
    }
    con.on_buffer = 0;
    kfree(con.shadow);
    con.shadow = NULL;
    con.shadow_bytes = 0;
    drm_modeset_lock_nointr(dev);
    drm_framebuffer_put(con.fb);
    drm_modeset_unlock(dev);
    drm_gem_object_put(con.obj);
    con.fb = NULL;
    con.obj = NULL;
    con.dev = NULL;
    klog(KLOG_INFO, "console", "card%d: left", dev->index);
    drm_dev_put(dev);
}

void drm_client_dev_hotplug(struct drm_device *dev)
{
    struct boot_framebuffer screen;
    volatile uint8_t *base = NULL;
    uint32_t gen = con.gen, flags;
    int32_t rc;

    if (dev != con.dev || dev->master) {
        return;
    }
    if (con.handed) {
        /* A program dropped the role and holds the card still; Linux's
         * hotplug handler sets the console's mode once no master holds
         * the device. */
        con_take_back(dev);
        return;
    }
    if (con.firmware || !fbcon_active) {
        return;
    }
    rc = con_place(dev, gen, &screen, &base);
    if (con.dev != dev || con.gen != gen) {
        return;
    }
    flags = console_lock();
    rc = con_attach(rc, &screen, base);
    fb_screen_moved();
    console_unlock(flags);
    con_settle();
    if (rc != 0) {
        klog(KLOG_WARN, "console", "card%d: hotplug: screen not set: error %d", dev->index,
             (int)rc);
    }
}
