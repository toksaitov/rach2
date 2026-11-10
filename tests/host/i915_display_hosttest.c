/*
 * i915_display_hosttest.c: the Intel graphics driver's display on Haswell,
 * intel_display.c and the files beside it, driven through the DRM core's
 * ioctl handlers against the host model of the Lenovo's chip, whose pipe
 * block keeps the firmware's picture on pipe A.
 *
 * host-deps: rach2/sys/drm/i915/i915_irq.c rach2/sys/drm/i915/i915_hw.c
 * host-deps: tests/host/i915_model.c rach2/sys/display/backlight.c
 * host-deps: rach2/sys/display/intelbl.c rach2/sys/lib/kstring.c
 * host-deps: rach2/sys/lib/kctypes.c tests/host/compat/spinlock_host.c
 * host-deps: rach2/sys/drm/i915/intel_bios.c
 * host-inputs: tests/fixtures/lenovo-gpu/i915_vbt.bin
 *
 * The rows: the readout of pipe A, the eDP transcoder, DDI A, the plane and
 * the panel power sequencer, against what Linux's i915_display_info read on
 * the Lenovo (78-9.1); the bind's adoption, which writes no register of the
 * pipe, its link or the backlight but reads the panel over AUX, the eDP
 * connector's probe, and the console's takeover commit, which keeps the
 * mode, pins the new framebuffer and lets the firmware's go once the next
 * flip replaced it, while a refresh rate the panel lacks is refused (78-9.2);
 * the planes' formats, Linux's Haswell lists, and their checks of stride,
 * tiling and offset, the X-tiled offset's split and the sprite's registers
 * (78-9.3); a page flip's event at the vblank that shows it, with its count
 * and a rising stamp, an async flip's at flip done, and the count of
 * vblanks the frame counter kept while the interrupt was off (78-9.4); the
 * cursor's registers and the backlight, taken over from the boot's driver,
 * set and switched, off with the pipe, its transcoder and the panel's
 * power, and given back at the end (78-9.5); the pipe CRC through
 * crc/control and crc/data, its first frame dropped, "auto" the primary
 * plane on the eDP pipe and "pipe" refused there (78-9.6); and a mode
 * smaller than the panel's, set in Linux's register order through the
 * fitter with its aspect kept, then stretched by "scaling mode", and the
 * panel's own mode again (78-10b).
 *
 * Returns 0 when every row passes, 1 otherwise.
 */

#include <stdarg.h>

#define KMS_MODEL_KERNEL
#include "drm_kms_model.c"
#include "drm/i915/intel_display.c"
#include "drm/i915/intel_plane.c"
#include "drm/i915/intel_cursor.c"
#include "drm/i915/intel_vblank.c"
#include "drm/i915/i915_display_debugfs.c"
#include "drm/i915/intel_backlight.c"
#include "drm/i915/intel_cdclk.c"
#include "drm/i915/intel_pps.c"
#include "drm/i915/intel_dp_aux.c"
#include "drm/i915/intel_ddi.c"
#include "drm/i915/intel_dp_link_training.c"
#include "drm/i915/intel_dp.c"
#include "drm/i915/intel_panel.c"
#include "drm/i915/intel_pfit.c"
#include "i915_model.h"
#include "i915_model_pipe.h"
#include "i915_model_ring.h"
#include "i915_model_sink.h"
#include "i915_lenovo.h"

extern int vsnprintf(char *s, unsigned long n, const char *format, va_list ap);
extern char *strstr(const char *s, const char *t);
extern int memcmp(const void *a, const void *b, unsigned long n);
extern int sscanf(const char *s, const char *format, ...);
extern int open(const char *path, int flags, ...);
extern long read(int fd, void *buf, unsigned long n);
extern int close(int fd);

/* ---- the kernel around the driver ---- */

static char log_text[8192];

/** The boot log, kept whole for the rows that read it. */
void klog(int level, const char *subsystem, const char *format, ...)
{
    unsigned long n = strlen(log_text);
    va_list ap;

    (void)level;
    (void)subsystem;
    va_start(ap, format);
    vsnprintf(log_text + n, sizeof(log_text) - n - 1, format, ap);
    va_end(ap);
    n = strlen(log_text);
    log_text[n] = '\n';
    log_text[n + 1] = 0;
}

/** Whether the log holds `text`. */
static int logged(const char *text)
{
    return strstr(log_text, text) != 0;
}

uint32_t kirq_save(void)
{
    return 0;
}

void kirq_restore(uint32_t flags)
{
    (void)flags;
}

/** The scheduler, which never runs here: the driver's sleeps are waits
 * on the model's clock. */
int sched_is_enabled(void)
{
    return 0;
}

void k_udelay(uint32_t usec)
{
    (void)usec;
}

/** Configuration space, which no row reads: all ones. */
uint16_t pci_read_config16(uint8_t bus, uint8_t device, uint8_t function, uint8_t offset)
{
    (void)bus;
    (void)device;
    (void)function;
    (void)offset;
    return 0xffffu;
}

/* The MMIO backend, which no row uses. */
int pat_write_combining(void)
{
    return 1;
}

uintptr_t map_mmio_typed(uintptr_t phys_addr, size_t size, pte_t cache)
{
    (void)phys_addr;
    (void)size;
    (void)cache;
    return 0;
}

/** Make `obj` a buffer object of `dev`, held once, as drm_gem.c does. */
void drm_gem_object_init(struct drm_device *dev, struct drm_gem_object *obj, uint64_t size,
                         const struct drm_gem_object_funcs *funcs)
{
    obj->dev = dev;
    obj->size = size;
    obj->funcs = funcs;
    obj->refs = 1;
}

/* ---- the driver's buffer objects: a few, bound in the aperture by the
 * pin, which counts what it is asked ---- */

#define NBO 12

const struct drm_gem_object_funcs i915_gem_object_funcs;
static struct drm_i915_gem_object bo[NBO];
static struct i915_vma bo_vma[NBO];
static int pins, unpins, flushes;
static uint32_t pin_align;
static uint64_t flush_start, flush_end;

/** The GGTT address the pin binds object `i` at. */
#define BO_GGTT(i) (0x01000000u + (uint32_t)(i) * 0x00800000u)

int32_t i915_gem_object_pin_to_display(struct drm_i915_gem_object *obj, uint32_t align,
                                       struct i915_vma **vma)
{
    struct i915_vma *v = &bo_vma[obj - bo];

    if (!obj->vmas) {
        v->obj = obj;
        v->start = BO_GGTT(obj - bo);
        v->mappable = 1;
        obj->vmas = v;
    }
    v->pin_count++;
    pins++;
    pin_align = align;
    *vma = v;
    return 0;
}

void i915_vma_unpin(struct i915_vma *vma)
{
    CHECK(vma->pin_count > 0, "a binding is unpinned no more than it is pinned");
    vma->pin_count--;
    unpins++;
}

void i915_gem_object_flush_range(struct drm_i915_gem_object *obj, uint64_t start, uint64_t end)
{
    (void)obj;
    flushes++;
    flush_start = start;
    flush_end = end;
}

/** The boot's probe, which backlight.c's init calls and no row does. */
void intelbl_init(void)
{}

/** The boot's own backlight after i915: none, so i915 leaves its own
 * stand-in. */
int intelbl_take_back(void)
{
    return -1;
}

/* ---- the device ---- */

static struct drm_i915_private gpu;
static struct drm_driver driver;
static struct drm_file fa;
static struct i915_model model;
static struct drm_mode_config *mc = &gpu.drm.mode_config;
static struct intel_crtc *pa = &gpu.display.crtc[0];
static struct drm_gem_object *fw_obj;
static uint32_t log_at_init; /* the model's log when the display is made */

/** Take every message the model has pending, as the processor calls the
 * handler. */
static void take_messages(void)
{
    while (i915_model_take_message(&model)) {
        i915_irq_handle(&gpu);
    }
}

static int64_t clock_base_ns; /* the monotonic time at the model's 0 */

/** After each wait of the driver: the monotonic clock follows the
 * model's. */
static void follow_clock(struct i915_model *m)
{
    model_now_ns = clock_base_ns + (int64_t)m->now_us * 1000;
}

/** One frame of pipe A, 16.67 ms on the model's clock and the monotonic
 * one: its vblank and the messages it raises. */
static void frame(void)
{
    model.now_us += 16667u;
    follow_clock(&model);
    i915_model_pipe_vblank(&model, 0);
    take_messages();
}

static int sleeps; /* the frames the waits slept */

/** What runs while a commit or a reader sleeps: frames of pipe A, until
 * the condition holds, at most a second's. */
static void sleep_frames(wq_cond_fn cond, void *arg)
{
    int i;

    for (i = 0; i < 60 && !cond(arg); i++) {
        frame();
        sleeps++;
    }
}

/** Whether the log of the model holds a write to a register in
 * [first, last] at or after entry `from`. */
static int wrote_range(uint32_t first, uint32_t last, uint32_t from)
{
    uint32_t i;

    for (i = from; i < model.nlog; i++) {
        if (model.log[i].op == I915_MODEL_WRITE && model.log[i].reg >= first &&
            model.log[i].reg <= last) {
            return 1;
        }
    }
    return 0;
}

/** A register's value in the model's file. */
static uint32_t reg(uint32_t r)
{
    return i915_model_peek(&model, r);
}

/* The boot's backlight driver, which i915 takes "intel_backlight" from. */
static int boot_level = 900, boot_sets;

/** The boot driver's `get`. */
static int boot_get(void *ctx)
{
    (void)ctx;
    return boot_level;
}

/** The boot driver's `set`, which must not run once i915 drives it. */
static void boot_set(void *ctx, int level)
{
    (void)ctx;
    boot_level = level;
    boot_sets++;
}

static const struct backlight_ops boot_ops = {boot_get, boot_set, NULL};

/**
 * The model, the device record over it with its interrupts installed and
 * the firmware's surface kept in the GGTT, as the bind leaves them, the
 * boot's backlight registered, and the display made, which adopts pipe A.
 */
static int32_t bring_up(void)
{
    static uint8_t lenovo_vbt[LENOVO_VBT_BYTES];
    long got = 0, r;
    int fd;

    CHECK(i915_model_init(&model) == 0, "the model is made");
    clock_base_ns = model_now_ns - (int64_t)model.now_us * 1000;
    model.on_delay = follow_clock;
    fd = open(LENOVO_VBT_FILE, 0);
    while (fd >= 0 && got < LENOVO_VBT_BYTES &&
           (r = read(fd, lenovo_vbt + got, (unsigned long)(LENOVO_VBT_BYTES - got))) > 0) {
        got += r;
    }
    if (fd >= 0) {
        close(fd);
    }
    CHECK(got == LENOVO_VBT_BYTES &&
              intel_bios_init(&gpu.vbt, lenovo_vbt, LENOVO_VBT_BYTES, "test") == 0,
          "the VBT is read and decoded");
    gpu.pci = model.pci;
    gpu.hw = model.hw;
    gpu.msi_vector = 48;
    driver.ident.name = "i915";
    gpu.drm.driver = &driver;
    gpu.drm.driver_priv = &gpu;
    gpu.drm.features = DRIVER_GEM | DRIVER_MODESET | DRIVER_ATOMIC;
    gpu.drm.refs = 1;
    gpu.ggtt.nkept = 1;
    gpu.ggtt.kept[0].start = 0;
    gpu.ggtt.kept[0].end = 0x408000u;
    i915_irq_reset(&gpu);
    i915_irq_install(&gpu);
    CHECK(backlight_register("intel_backlight", 937, &boot_ops, NULL) == 0,
          "the boot registers intel_backlight");
    file_open(&fa, &gpu.drm);
    gpu.drm.master = &fa;
    fa.is_master = 1;
    fa.atomic = fa.universal_planes = 1;
    log_at_init = model.nlog;
    model_wait = sleep_frames;
    return intel_display_init(&gpu);
}

/** Make object `i` of `size` bytes with `tiling` and `stride`, and give
 * the master a handle to it. */
static uint32_t bo_new(int i, uint64_t size, uint32_t tiling, uint32_t stride)
{
    uint32_t h = 0;

    k_memset(&bo[i], 0, sizeof(bo[i]));
    k_memset(&bo_vma[i], 0, sizeof(bo_vma[i]));
    bo[i].base.dev = &gpu.drm;
    bo[i].base.size = size;
    bo[i].base.funcs = &i915_gem_object_funcs;
    bo[i].base.refs = 1;
    bo[i].tiling = tiling;
    bo[i].stride = stride;
    CHECK(drm_gem_handle_create(&fa, &bo[i].base, &h) == 0, "a handle is made");
    bo[i].base.refs--;
    return h;
}

/* ADDFB2's modifier when none is given. */
#define NO_MODIFIER (~(uint64_t)0)

/** ADDFB2 of `w` by `h` pixels of `format` over the handle `hd`, with
 * `pitch` and `modifier`; its ID, or 0. */
static uint32_t addfb(uint32_t hd, uint32_t w, uint32_t h, uint32_t format, uint32_t pitch,
                      uint64_t modifier)
{
    struct drm_mode_fb_cmd2 r;

    k_memset(&r, 0, sizeof(r));
    r.width = w;
    r.height = h;
    r.pixel_format = format;
    r.handles[0] = hd;
    r.pitches[0] = pitch;
    if (modifier != NO_MODIFIER) {
        r.flags = DRM_MODE_FB_MODIFIERS;
        r.modifier[0] = modifier;
    }
    return drm_mode_addfb2_ioctl(&gpu.drm, &r, &fa) == 0 ? r.fb_id : 0;
}

/* An atomic request being built. */
struct req {
    uint32_t objs[4], counts[4], props[24];
    uint64_t values[24];
    uint32_t nobj, nprop;
};

/** Add `prop` = `value` of object `obj` to `r`. */
static void req_add(struct req *r, struct drm_mode_object *obj, struct drm_property *prop,
                    uint64_t value)
{
    if (r->nobj == 0 || r->objs[r->nobj - 1] != obj->id) {
        r->objs[r->nobj] = obj->id;
        r->counts[r->nobj++] = 0;
    }
    r->counts[r->nobj - 1]++;
    r->props[r->nprop] = prop->base.id;
    r->values[r->nprop++] = value;
}

/** ATOMIC of `r` with `flags`. */
static int32_t req_commit(const struct req *r, uint32_t flags)
{
    struct drm_mode_atomic a;

    k_memset(&a, 0, sizeof(a));
    a.flags = flags;
    a.count_objs = r->nobj;
    a.objs_ptr = UPTR(r->objs);
    a.count_props_ptr = UPTR(r->counts);
    a.props_ptr = UPTR(r->props);
    a.prop_values_ptr = UPTR(r->values);
    return drm_mode_atomic_ioctl(&gpu.drm, &a, &fa);
}

/* A plane's place: the source's corner and size, the CRTC's corner and
 * size. */
struct place {
    uint32_t sx, sy, sw, sh;
    int32_t cx, cy;
    uint32_t cw, ch;
};

/* The whole panel from the framebuffer's corner. */
static const struct place full = {0, 0, 1366, 768, 0, 0, 1366, 768};

/** ATOMIC of `plane` on pipe A showing `fb_id` at `p`, with `flags`. */
static int32_t plane_commit(struct drm_plane *plane, uint32_t fb_id, const struct place *p,
                            uint32_t flags)
{
    struct req r;

    k_memset(&r, 0, sizeof(r));
    req_add(&r, &plane->base, mc->prop_fb_id, fb_id);
    req_add(&r, &plane->base, mc->prop_crtc_id, fb_id ? pa->base.base.id : 0);
    req_add(&r, &plane->base, mc->prop_src_x, (uint64_t)p->sx << 16);
    req_add(&r, &plane->base, mc->prop_src_y, (uint64_t)p->sy << 16);
    req_add(&r, &plane->base, mc->prop_src_w, (uint64_t)p->sw << 16);
    req_add(&r, &plane->base, mc->prop_src_h, (uint64_t)p->sh << 16);
    req_add(&r, &plane->base, mc->prop_crtc_x, (uint64_t)(int64_t)p->cx);
    req_add(&r, &plane->base, mc->prop_crtc_y, (uint64_t)(int64_t)p->cy);
    req_add(&r, &plane->base, mc->prop_crtc_w, p->cw);
    req_add(&r, &plane->base, mc->prop_crtc_h, p->ch);
    return req_commit(&r, flags);
}

/** ATOMIC of pipe A's ACTIVE. */
static int32_t set_active(int on)
{
    struct req r;

    k_memset(&r, 0, sizeof(r));
    req_add(&r, &pa->base.base, mc->prop_active, (uint64_t)on);
    return req_commit(&r, DRM_MODE_ATOMIC_ALLOW_MODESET);
}

/** SETCRTC of pipe A with `mode` and `fb_id` on the eDP connector. */
static int32_t setcrtc(const struct drm_mode_modeinfo *mode, uint32_t fb_id)
{
    struct drm_mode_crtc c;
    uint32_t conn = gpu.display.connector.base.id;

    k_memset(&c, 0, sizeof(c));
    c.crtc_id = pa->base.base.id;
    c.fb_id = fb_id;
    c.mode_valid = 1;
    c.mode = *mode;
    c.set_connectors_ptr = UPTR(&conn);
    c.count_connectors = 1;
    return drm_mode_setcrtc(&gpu.drm, &c, &fa);
}

/** PAGE_FLIP of pipe A to `fb_id` with `flags` and the event's
 * `user_data`. */
static int32_t page_flip(uint32_t fb_id, uint32_t flags, uint64_t user_data)
{
    struct drm_mode_crtc_page_flip f;

    k_memset(&f, 0, sizeof(f));
    f.crtc_id = pa->base.base.id;
    f.fb_id = fb_id;
    f.flags = flags;
    f.user_data = user_data;
    return drm_mode_page_flip_ioctl(&gpu.drm, &f, &fa);
}

/* ---- 78-9.1: the readout ---- */

/** Pipe A's mode as Linux read it on the Lenovo: "1366x768": 60 76300
 * 1366 1374 1384 1558 768 771 772 816 0x48 0xa. */
static void row_readout_mode(void)
{
    const struct drm_mode_modeinfo *m = &pa->hw.mode;

    CHECK(m->clock == 76300 && m->hdisplay == 1366 && m->hsync_start == 1374 &&
              m->hsync_end == 1384 && m->htotal == 1558,
          "readout: the horizontal timings and dot clock of i915_display_info");
    CHECK(m->vdisplay == 768 && m->vsync_start == 771 && m->vsync_end == 772 && m->vtotal == 816,
          "readout: the vertical timings");
    CHECK(m->vrefresh == 60 && strcmp(m->name, "1366x768") == 0 &&
              m->flags == (DRM_MODE_FLAG_NHSYNC | DRM_MODE_FLAG_NVSYNC) &&
              m->type == (DRM_MODE_TYPE_DRIVER | DRM_MODE_TYPE_PREFERRED),
          "readout: 60 Hz, its name, negative syncs, the driver's preferred mode");
}

/** The rest of pipe A as Linux read it: 18 bpp dithered, one lane of DDI
 * A at 270 MHz, the firmware's XRGB8888 plane at GTT 0 of stride 5504;
 * the other pipes off, the panel powered with its backlight on. */
static void row_readout(void)
{
    const struct intel_pipe_readout *hw = &pa->hw;
    const struct intel_pps_readout *pps = &gpu.display.pps;

    CHECK(hw->active && hw->trans == I915_TRANS_EDP && hw->port == 0,
          "readout: pipe A on, fed to the eDP transcoder and DDI A");
    row_readout_mode();
    CHECK(hw->bpp == 18 && hw->dither && hw->lanes == 1 && hw->port_clock == 270000,
          "readout: 6 bits a colour dithered, one lane at 270000 kHz");
    CHECK(hw->link_m == LENOVO_LINK_M && hw->link_n == LENOVO_LINK_N && hw->tu == 64 &&
              hw->data_n == LENOVO_DATA_N,
          "readout: the link's M/N and the transfer unit");
    CHECK(hw->src_w == 1366 && hw->src_h == 768 && hw->pf_ctl == 0,
          "readout: the pipe's source the panel's size, the fitter off");
    CHECK(hw->plane_on && hw->format == DRM_FORMAT_XRGB8888 &&
              hw->modifier == DRM_FORMAT_MOD_LINEAR && hw->surf == 0 && hw->stride == 5504,
          "readout: the firmware's linear XRGB8888 plane at GTT 0, stride 5504");
    CHECK(!gpu.display.crtc[1].hw.active && !gpu.display.crtc[2].hw.active,
          "readout: pipes B and C off");
    CHECK(pps->panel_on && pps->backlight_on && pps->on_delays == LENOVO_FW_PP_ON_DELAYS,
          "readout: the panel powered, its backlight on, the sequencer's delays");
}

/* ---- 78-9.2: adoption and the takeover ---- */

/** The bind's adoption: no register of the pipes, planes, transcoders,
 * DDI buffers, clocks or backlight written, the panel's sequencer given
 * Linux's delays and AUX messages sent; pipe A on in the atomic state with
 * the firmware's mode and framebuffer, and its configuration read. */
static void row_adopt(void)
{
    struct drm_plane_state *ps = pa->primary.state;

    CHECK(!wrote_range(0x60000u, 0x6400fu, log_at_init) &&
              !wrote_range(0x64028u, 0x7ffffu, log_at_init) &&
              !wrote_range(0x45000u, 0x452ffu, log_at_init) &&
              !wrote_range(0x46100u, 0x46114u, log_at_init) &&
              !wrote_range(0xc7200u, 0xc7203u, log_at_init) &&
              !wrote_range(0xc8250u, 0xc8258u, log_at_init) &&
              !wrote_range(0x48250u, 0x48254u, log_at_init),
          "adopt: nothing of the pipe, its link or the backlight written");
    CHECK(wrote_range(DP_AUX_CH_CTL(0), DP_AUX_CH_CTL(0), log_at_init) &&
              i915_model_find_write(&model, PP_ON_DELAYS, 0x07d00001u, log_at_init) >= 0 &&
              (reg(PP_CONTROL) & PANEL_POWER_ON),
          "adopt: the panel read over AUX, the sequencer given its delays, the panel on");
    CHECK(pa->base.state->active && pa->base.state->enable &&
              drm_mode_equal(&pa->base.state->mode, &pa->hw.mode),
          "adopt: pipe A on with the firmware's mode");
    CHECK(ps->fb && ps->crtc == &pa->base && ps->visible && ps->fb->width == 1366 &&
              ps->fb->height == 768 && ps->fb->pitches[0] == 5504 &&
              ps->fb->format == DRM_FORMAT_XRGB8888 && intel_fb_ggtt(ps->fb) == 0,
          "adopt: the primary plane shows the firmware's framebuffer at GTT 0");
    CHECK(gpu.display.fw_taken && pa->shown[INTEL_PLANE_PRIMARY].fb == ps->fb,
          "adopt: the firmware's framebuffer taken and held as shown");
    fw_obj = ps->fb->obj[0];
    CHECK(logged("display: pipe A adopted: 1366x768 in 1366x768@60, 76300 kHz, 18 bpp, 1 lane at "
                 "270000 kHz") &&
              logged("edp: DPCD 1.1, eDP 0, 1 lane, rates 162000 to 270000 kHz, 6 bpc, EDID 128 "
                     "bytes") &&
              pa->cfg.active && !pa->cfg.pfit && pa->cfg.bpp == 18 && pa->cfg.lanes == 1 &&
              gpu.display.dp.link_trained,
          "adopt: the log's lines, the pipe's configuration and the link trained");
}

/** The eDP connector's probe after the adoption: connected, with the
 * panel's one mode, on its encoder, which reaches every pipe, and Linux's
 * "scaling mode" and "max bpc". */
static void row_probe(void)
{
    struct drm_mode_get_connector gc;
    struct drm_mode_modeinfo modes[2];
    uint32_t enc;

    k_memset(&gc, 0, sizeof(gc));
    gc.connector_id = gpu.display.connector.base.id;
    CHECK(drm_mode_getconnector(&gpu.drm, &gc, &fa) == 0 && gc.count_modes == 1,
          "probe: GETCONNECTOR probes one mode");
    gc.modes_ptr = UPTR(modes);
    gc.count_modes = 2;
    gc.encoders_ptr = UPTR(&enc);
    gc.count_encoders = 1;
    gc.count_props = 0;
    CHECK(drm_mode_getconnector(&gpu.drm, &gc, &fa) == 0, "probe: GETCONNECTOR");
    CHECK(gc.connection == DRM_CONNECTOR_CONNECTED && gc.connector_type == DRM_MODE_CONNECTOR_eDP &&
              gc.count_modes == 1 && drm_mode_equal(&modes[0], &pa->hw.mode) &&
              gc.encoder_id == gpu.display.encoder.base.id,
          "probe: eDP-1 connected, the firmware's mode alone, on its encoder");
    CHECK(gpu.display.encoder.possible_crtcs == 7u && gpu.display.connector.scaling_mode_property &&
              gpu.display.connector.max_bpc_property &&
              gpu.display.connector.state->scaling_mode == DRM_MODE_SCALE_ASPECT,
          "probe: the panel reaches every pipe, with scaling mode Full aspect and max bpc");
}

static uint32_t h_con, fb_con;

/** The console's takeover: a dumb buffer of the loader's size shown with
 * the adopted mode, a commit that writes the plane alone, pinned at the
 * scanout's alignment. */
static void row_takeover(void)
{
    struct drm_mode_modeinfo other = pa->hw.mode;
    uint32_t from = model.nlog;

    h_con = bo_new(0, 5504u * 768u, I915_TILING_NONE, 0);
    fb_con = addfb(h_con, 1366, 768, DRM_FORMAT_XRGB8888, 5504, NO_MODIFIER);
    CHECK(fb_con != 0, "takeover: ADDFB2 of the console's buffer");
    CHECK(setcrtc(&pa->hw.mode, fb_con) == 0, "takeover: SETCRTC with the firmware's mode");
    CHECK(pins == 1 && pin_align == 256u * 1024u && bo_vma[0].pin_count == 1,
          "takeover: the buffer pinned at 256 KiB for scanout");
    CHECK(reg(DSPSURF(0)) == BO_GGTT(0) && reg(DSPSTRIDE(0)) == 5504 &&
              reg(DSPCNTR(0)) == (DISP_ENABLE | DISP_FORMAT_BGRX888) && reg(DSPOFFSET(0)) == 0,
          "takeover: the primary plane's registers");
    CHECK(!wrote_range(0x60000u, 0x6ffffu, from) && !wrote_range(0xc7200u, 0xc72ffu, from),
          "takeover: no transcoder, pipe or panel register written");
    CHECK(reg(CPU_VGACNTRL) & VGA_DISP_DISABLE, "takeover: the VGA plane off");
    frame();
    CHECK(i915_model_pipes(&model)->pipe[0].primary.surf == BO_GGTT(0),
          "takeover: the plane scans the console's buffer after the vblank");
    CHECK(pa->shown[0].fb && pa->shown[0].fb->base.id == fb_con && pa->parked[0].fb &&
              pa->parked[0].fb->obj[0] == fw_obj,
          "takeover: the firmware's framebuffer parked until the next flip");
    other.clock = 50000;
    CHECK(setcrtc(&other, fb_con) == -EINVAL, "takeover: a refresh rate the panel lacks refused");
}

/* ---- 78-9.4: flips and their events ---- */

/** A flip's event: sent at the vblank that latches it, not before, with
 * the count and a stamp that rises; the firmware's framebuffer, parked,
 * let go by the next one. */
static void row_flip(void)
{
    uint32_t h, fb;
    int n0 = nsent;

    h = bo_new(1, 5504u * 768u, I915_TILING_NONE, 0);
    fb = addfb(h, 1366, 768, DRM_FORMAT_XRGB8888, 5504, NO_MODIFIER);
    CHECK(page_flip(fb, DRM_MODE_PAGE_FLIP_EVENT, 0x1234) == 0, "flip: PAGE_FLIP");
    CHECK(nsent == n0 && pa->base.flip_pending == DRM_FLIP_ARMED && pa->event,
          "flip: the event held until the vblank");
    CHECK(i915_model_pipes(&model)->pipe[0].primary.surf == BO_GGTT(0) &&
              reg(DSPSURF(0)) == BO_GGTT(1),
          "flip: the surface written, the old one scanned");
    CHECK(page_flip(fb_con, DRM_MODE_PAGE_FLIP_EVENT, 0) == -EBUSY,
          "flip: a second flip before the vblank refused");
    frame();
    CHECK(nsent == n0 + 1 && sent[n0].base.type == DRM_EVENT_FLIP_COMPLETE &&
              sent[n0].user_data == 0x1234 && sent[n0].crtc_id == pa->base.base.id,
          "flip: the event sent at the vblank");
    CHECK(sent[n0].sequence == (uint32_t)pa->base.vblank.count &&
              (int64_t)sent[n0].tv_sec * 1000000000ll + (int64_t)sent[n0].tv_usec * 1000ll ==
                  model_now_ns / 1000 * 1000,
          "flip: the event's count and stamp are the vblank's");
    CHECK(pa->base.flip_pending == DRM_FLIP_NONE && !pa->event, "flip: nothing pending");
    CHECK(fw_obj->refs == 0, "flip: the firmware's framebuffer let go");
    CHECK(page_flip(fb_con, DRM_MODE_PAGE_FLIP_EVENT, 0x5678) == 0, "flip: back to the console");
    CHECK(bo_vma[1].pin_count == 1 && bo_vma[0].pin_count == 1,
          "flip: the shown and the parked buffers stay pinned");
    frame();
    CHECK(nsent == n0 + 2 && sent[n0 + 1].user_data == 0x5678 &&
              sent[n0 + 1].sequence == sent[n0].sequence + 1 &&
              (sent[n0 + 1].tv_sec > sent[n0].tv_sec ||
               (sent[n0 + 1].tv_sec == sent[n0].tv_sec && sent[n0 + 1].tv_usec > sent[n0].tv_usec)),
          "flip: the next event one count on, its stamp later");
    CHECK(page_flip(fb, 0, 0) == 0, "flip: one without an event");
    frame();
    CHECK(bo_vma[1].pin_count == 1 && bo_vma[0].pin_count == 1 && unpins >= 1,
          "flip: a buffer two flips back unpinned");
}

/** An async flip of X-tiled buffers: the surface taken at once and its
 * event sent at flip done, without a vblank; a linear one refused. */
static void row_async(void)
{
    uint32_t h2, h3, fb2, fb3, flips;
    int n0;

    h2 = bo_new(2, 5632u * 768u, I915_TILING_X, 5632);
    h3 = bo_new(3, 5632u * 768u, I915_TILING_X, 5632);
    fb2 = addfb(h2, 1366, 768, DRM_FORMAT_XRGB8888, 5632, NO_MODIFIER);
    fb3 = addfb(h3, 1366, 768, DRM_FORMAT_XRGB8888, 5632, I915_FORMAT_MOD_X_TILED);
    CHECK(fb2 && fb3, "async: two X-tiled framebuffers");
    CHECK(page_flip(fb2, DRM_MODE_PAGE_FLIP_ASYNC | DRM_MODE_PAGE_FLIP_EVENT, 0) == -EINVAL,
          "async: from a linear buffer refused");
    CHECK(page_flip(fb2, DRM_MODE_PAGE_FLIP_EVENT, 0) == 0, "async: a synchronous flip first");
    CHECK(reg(DSPCNTR(0)) == (DISP_ENABLE | DISP_FORMAT_BGRX888 | DISP_TILED),
          "async: an object's X tiling taken without a modifier");
    frame();
    n0 = nsent;
    flips = i915_model_pipes(&model)->pipe[0].flips;
    CHECK(page_flip(fb3, DRM_MODE_PAGE_FLIP_ASYNC | DRM_MODE_PAGE_FLIP_EVENT, 0x77) == 0,
          "async: PAGE_FLIP_ASYNC");
    CHECK(i915_model_pipes(&model)->pipe[0].flips == flips + 1 &&
              i915_model_pipes(&model)->pipe[0].primary.surf == BO_GGTT(3),
          "async: the plane takes the surface at once");
    take_messages();
    CHECK(nsent == n0 + 1 && sent[n0].user_data == 0x77, "async: the event sent at flip done");
    CHECK(reg(DEIMR) & DE_PLANE_FLIP_DONE_IVB(0), "async: flip done masked again");
    CHECK(page_flip(fb_con, DRM_MODE_PAGE_FLIP_ASYNC, 0) == -EINVAL,
          "async: to a linear buffer refused");
    CHECK(page_flip(fb_con, DRM_MODE_PAGE_FLIP_EVENT, 0) == 0, "async: back to the console");
    frame();
}

/** The vblanks of the frame counter while the interrupt was off are
 * counted when it comes on again. */
static void row_count(void)
{
    uint64_t before;
    int i;

    model.now_us += (uint64_t)DRM_VBLANK_OFFDELAY_MS * 1000u;
    frame();
    CHECK(!pa->base.vblank.enabled && !pa->vbl_on && (reg(DEIMR) & DE_PIPE_VBLANK_IVB(0)),
          "count: the interrupt off once nothing holds it");
    before = pa->base.vblank.count;
    for (i = 0; i < 5; i++) {
        frame();
    }
    CHECK(pa->base.vblank.count == before, "count: no interrupt while off");
    CHECK(drm_crtc_vblank_get(&pa->base) == 0 && pa->base.vblank.count == before + 5,
          "count: the frame counter's five taken in");
    frame();
    CHECK(pa->base.vblank.count == before + 6, "count: one a vblank");
    drm_crtc_vblank_put(&pa->base);
}

/* ---- 78-9.3: the planes ---- */

/** The formats of a plane by GETPLANE into `fmts`; their number. */
static uint32_t plane_formats(struct drm_plane *plane, uint32_t *fmts, uint32_t cap)
{
    struct drm_mode_get_plane p;

    k_memset(&p, 0, sizeof(p));
    p.plane_id = plane->base.id;
    p.format_type_ptr = UPTR(fmts);
    p.count_format_types = cap;
    CHECK(drm_mode_getplane(&gpu.drm, &p, &fa) == 0, "planes: GETPLANE");
    return p.count_format_types;
}

/** The formats: Linux's Haswell lists, no alpha on the primary plane. */
static void row_formats(void)
{
    static const uint32_t primary[] = {DRM_FORMAT_C8,           DRM_FORMAT_RGB565,
                                       DRM_FORMAT_XRGB8888,     DRM_FORMAT_XBGR8888,
                                       DRM_FORMAT_XRGB2101010,  DRM_FORMAT_XBGR2101010,
                                       DRM_FORMAT_XBGR16161616F};
    static const uint32_t sprite[] = {DRM_FORMAT_XRGB8888,      DRM_FORMAT_XBGR8888,
                                      DRM_FORMAT_XRGB2101010,   DRM_FORMAT_XBGR2101010,
                                      DRM_FORMAT_XRGB16161616F, DRM_FORMAT_XBGR16161616F,
                                      DRM_FORMAT_YUYV,          DRM_FORMAT_YVYU,
                                      DRM_FORMAT_UYVY,          DRM_FORMAT_VYUY};
    uint32_t f[16];

    CHECK(plane_formats(&pa->primary, f, 16) == 7 && memcmp(f, primary, sizeof(primary)) == 0,
          "formats: the primary plane's seven");
    CHECK(plane_formats(&pa->sprite, f, 16) == 10 && memcmp(f, sprite, sizeof(sprite)) == 0,
          "formats: the sprite's ten");
    CHECK(plane_formats(&pa->cursor, f, 16) == 1 && f[0] == DRM_FORMAT_ARGB8888,
          "formats: the cursor's ARGB8888");
    CHECK(pa->primary.type == DRM_PLANE_TYPE_PRIMARY && pa->sprite.type == DRM_PLANE_TYPE_OVERLAY &&
              pa->cursor.type == DRM_PLANE_TYPE_CURSOR && pa->sprite.possible_crtcs == 1u &&
              gpu.display.crtc[2].sprite.possible_crtcs == 4u,
          "formats: three planes a pipe, each on its own");
}

/** The primary plane's checks, and an X-tiled source corner split between
 * the surface and the offset register. */
static void row_primary(void)
{
    static const struct place corner = {100, 50, 1366, 768, 0, 0, 1366, 768};
    static const struct place small = {0, 0, 1000, 700, 0, 0, 1000, 700};
    uint32_t h, fb;
    uint32_t big = 33024u * 768u;

    h = bo_new(4, 5504u * 768u, I915_TILING_NONE, 0);
    fb = addfb(h, 1366, 768, DRM_FORMAT_XRGB8888, 5480, NO_MODIFIER);
    CHECK(fb && plane_commit(&pa->primary, fb, &full, DRM_MODE_ATOMIC_TEST_ONLY) == -EINVAL,
          "primary: a linear stride off 64 bytes refused");
    fb = addfb(h, 1366, 768, DRM_FORMAT_C8, 1408, NO_MODIFIER);
    CHECK(fb && plane_commit(&pa->primary, fb, &full, DRM_MODE_ATOMIC_TEST_ONLY) == -EINVAL,
          "primary: C8 refused without a LUT");
    fb = addfb(h, 1366, 768, DRM_FORMAT_ARGB8888, 5504, NO_MODIFIER);
    CHECK(fb && plane_commit(&pa->primary, fb, &full, DRM_MODE_ATOMIC_TEST_ONLY) == -EINVAL,
          "primary: ARGB8888 refused");
    fb = addfb(h, 1366, 768, DRM_FORMAT_RGB565, 2752, NO_MODIFIER);
    CHECK(fb && plane_commit(&pa->primary, fb, &full, DRM_MODE_ATOMIC_TEST_ONLY) == 0,
          "primary: RGB565 taken");
    CHECK(plane_commit(&pa->primary, fb, &small, DRM_MODE_ATOMIC_TEST_ONLY) == -EINVAL,
          "primary: one short of the CRTC refused");
    h = bo_new(5, big, I915_TILING_NONE, 0);
    fb = addfb(h, 1366, 768, DRM_FORMAT_XRGB8888, 33024, NO_MODIFIER);
    CHECK(fb && plane_commit(&pa->primary, fb, &full, DRM_MODE_ATOMIC_TEST_ONLY) == -EINVAL,
          "primary: a stride past 32 KiB refused");
    h = bo_new(6, 5632u * 768u, I915_TILING_X, 5632);
    fb = addfb(h, 1366, 768, DRM_FORMAT_XRGB8888, 5632, DRM_FORMAT_MOD_LINEAR);
    CHECK(fb == 0 || plane_commit(&pa->primary, fb, &full, DRM_MODE_ATOMIC_TEST_ONLY) == -EINVAL,
          "primary: an X-tiled object shown linear refused");
    h = bo_new(7, 8192u * 1024u, I915_TILING_X, 8192);
    fb = addfb(h, 2048, 1024, DRM_FORMAT_XRGB8888, 8192, I915_FORMAT_MOD_X_TILED);
    CHECK(fb && plane_commit(&pa->primary, fb, &corner, 0) == 0,
          "primary: an X-tiled source from (100, 50)");
    CHECK(reg(DSPSURF(0)) == BO_GGTT(7) + (6u * 16u) * 4096u &&
              reg(DSPOFFSET(0)) == (2u << 16 | 100u) && reg(DSPSTRIDE(0)) == 8192 &&
              (reg(DSPCNTR(0)) & DISP_TILED),
          "primary: the tile row in the surface, the rest in the offset");
    frame();
    CHECK(plane_commit(&pa->primary, fb_con, &full, 0) == 0, "primary: back to the console");
    frame();
}

/** The sprite's registers, its checks, and the low-power watermarks off
 * once it shows. */
static void row_sprite(void)
{
    static const struct place at = {0, 0, 64, 64, 10, 20, 64, 64};
    static const struct place odd = {1, 0, 62, 64, 10, 20, 62, 64};
    uint32_t h, fb;

    h = bo_new(8, 16448u * 64u, I915_TILING_NONE, 0);
    fb = addfb(h, 64, 64, DRM_FORMAT_YUYV, 128, NO_MODIFIER);
    CHECK(fb && plane_commit(&pa->sprite, fb, &odd, DRM_MODE_ATOMIC_TEST_ONLY) == -EINVAL,
          "sprite: YUV from an odd pixel refused");
    fb = addfb(h, 64, 64, DRM_FORMAT_XRGB8888, 16448, NO_MODIFIER);
    CHECK(fb && plane_commit(&pa->sprite, fb, &at, DRM_MODE_ATOMIC_TEST_ONLY) == -EINVAL,
          "sprite: a stride past 16 KiB refused");
    fb = addfb(h, 64, 64, DRM_FORMAT_XBGR8888, 256, NO_MODIFIER);
    CHECK(fb && plane_commit(&pa->sprite, fb, &at, 0) == 0, "sprite: shown at (10, 20)");
    CHECK(reg(SPRSURF(0)) == BO_GGTT(8) && reg(SPRSTRIDE(0)) == 256 &&
              reg(SPRPOS(0)) == (20u << 16 | 10u) && reg(SPRSIZE(0)) == (63u << 16 | 63u) &&
              reg(SPRCTL(0)) == (SPRITE_ENABLE | SPRITE_PLANE_GAMMA_DISABLE |
                                 SPRITE_FORMAT_RGBX888 | SPRITE_RGB_ORDER_RGBX),
          "sprite: its registers");
    CHECK(gpu.display.lp_wm_off && !(reg(WM1_LP_ILK) & WM_LP_ENABLE) &&
              ((reg(WM0_PIPE_ILK(0)) >> 8) & 0x7fu) != 0,
          "sprite: its level 0 watermark, the low-power ones off");
    frame();
    CHECK(i915_model_pipes(&model)->pipe[0].sprite.surf == BO_GGTT(8),
          "sprite: scanned after the vblank");
    CHECK(plane_commit(&pa->sprite, 0, &full, 0) == 0 && reg(SPRCTL(0)) == 0, "sprite: turned off");
    frame();
}

/* ---- 78-9.5: the cursor and the backlight ---- */

/** The cursor through the legacy ioctl: its mode, a negative place in
 * sign and magnitude, and its base; a size it does not take refused. */
static void row_cursor(void)
{
    struct drm_mode_cursor c;
    uint32_t h = bo_new(9, 64u * 64u * 4u, I915_TILING_NONE, 0);

    k_memset(&c, 0, sizeof(c));
    c.crtc_id = pa->base.base.id;
    c.flags = DRM_MODE_CURSOR_BO;
    c.handle = h;
    c.width = 64;
    c.height = 64;
    CHECK(drm_mode_cursor_ioctl(&gpu.drm, &c, &fa) == 0, "cursor: a 64-pixel cursor");
    CHECK(reg(CURCNTR(0)) == MCURSOR_MODE_64_ARGB_AX && reg(CURBASE(0)) == BO_GGTT(9),
          "cursor: its mode and base");
    frame();
    c.flags = DRM_MODE_CURSOR_MOVE;
    c.x = -10;
    c.y = 20;
    CHECK(drm_mode_cursor_ioctl(&gpu.drm, &c, &fa) == 0, "cursor: moved");
    CHECK(reg(CURPOS(0)) == (20u << 16 | CURSOR_POS_X_SIGN | 10u), "cursor: CURPOS's sign");
    frame();
    CHECK(i915_model_pipes(&model)->pipe[0].cursor.pos == reg(CURPOS(0)),
          "cursor: the place latched at the vblank");
    c.flags = DRM_MODE_CURSOR_BO;
    c.width = 32;
    c.height = 32;
    CHECK(drm_mode_cursor_ioctl(&gpu.drm, &c, &fa) == -EINVAL, "cursor: 32 pixels refused");
    c.handle = 0;
    CHECK(drm_mode_cursor_ioctl(&gpu.drm, &c, &fa) == 0 && reg(CURCNTR(0)) == 0, "cursor: hidden");
    frame();
}

/** Write `text` to attribute `attr` of backlight 0. */
static int bl_store(int attr, const char *text)
{
    return backlight_store(0, attr, text, (int)strlen(text));
}

/** The level backlight 0 shows for `attr`. */
static int bl_show(int attr)
{
    char buf[16];
    int v = -1;

    CHECK(backlight_show(0, attr, buf, sizeof(buf)) > 0, "backlight: an attribute read");
    (void)sscanf(buf, "%d", &v);
    return v;
}

/** The backlight: taken over from the boot's driver under its name, set
 * through the PCH's PWM, switched by the sequencer's enable, which stays
 * off while the pipe is off. */
static void row_backlight(void)
{
    CHECK(gpu.display.bl_index == 0 && backlight_count() == 1 &&
              backlight_find("intel_backlight", 15) == 0,
          "backlight: intel_backlight rebound, not registered again");
    CHECK(gpu.display.bl.max == 937 && gpu.display.bl.switched && bl_show(BACKLIGHT_LEVELS) == 937,
          "backlight: the PWM's period, switched by the sequencer");
    CHECK(bl_store(BACKLIGHT_BRIGHTNESS, "500\n") == 0 &&
              (reg(BLC_PWM_PCH_CTL2) & 0xffffu) == 500 && boot_sets == 0,
          "backlight: a level through i915's registers, not the boot's");
    CHECK(bl_store(BACKLIGHT_POWER, "4") == 0 && !(reg(PP_CONTROL) & EDP_BLC_ENABLE),
          "backlight: bl_power off clears the enable");
    CHECK(bl_store(BACKLIGHT_POWER, "0") == 0 && (reg(PP_CONTROL) & EDP_BLC_ENABLE),
          "backlight: bl_power on sets it");
    CHECK(set_active(0) == 0 && !(reg(PP_CONTROL) & EDP_BLC_ENABLE) && reg(DSPCNTR(0)) == 0,
          "backlight: off with the pipe, its planes off");
    CHECK(!(reg(TRANSCONF(I915_TRANS_EDP)) & TRANSCONF_ENABLE) &&
              !(reg(PP_CONTROL) & PANEL_POWER_ON) && !(reg(BLC_PWM_PCH_CTL1) & BLM_PCH_PWM_ENABLE),
          "backlight: the transcoder, the panel's power and the PWM off");
    CHECK(bl_store(BACKLIGHT_POWER, "0") == 0 && !(reg(PP_CONTROL) & EDP_BLC_ENABLE),
          "backlight: bl_power on waits for the pipe");
    frame();
    CHECK(set_active(1) == 0 && (reg(PP_CONTROL) & EDP_BLC_ENABLE) &&
              reg(DSPSURF(0)) == BO_GGTT(0) && (reg(DSPCNTR(0)) & DISP_ENABLE),
          "backlight: on with the pipe, the console's plane shown again");
    CHECK((reg(BLC_PWM_PCH_CTL2) & 0xffffu) == 500, "backlight: the level kept");
    frame();
}

/* ---- 78-9.6: the pipe CRC ---- */

/** crc/data's next line as its frame and CRC. */
static int crc_line(uint32_t *frame_no, uint32_t *crc)
{
    char buf[64];

    if (drm_crtc_crc_read(&pa->base, buf, sizeof(buf), 1) <= 0) {
        return 0;
    }
    return sscanf(buf, "0x%x 0x%x", frame_no, crc) == 2;
}

/** The CRC of pipe A: "pipe" refused on the eDP pipe with its fitter off,
 * "auto" the primary plane's, its first frame dropped, a line a frame with
 * the vblank's count and the chip's result. */
static void row_crc(void)
{
    uint32_t fr = 0, crc = 0;

    CHECK(drm_crtc_crc_set_source(&pa->base, "pipe\n", 5) == -EINVAL,
          "crc: pipe refused on the eDP pipe");
    CHECK(drm_crtc_crc_set_source(&pa->base, "auto", 4) == 4, "crc: auto");
    CHECK(drm_crtc_crc_open(&pa->base) == 0, "crc: crc/data opened");
    CHECK(reg(PIPE_CRC_CTL(0)) == (PIPE_CRC_ENABLE | PIPE_CRC_SOURCE_PRIMARY_IVB),
          "crc: auto is the primary plane");
    frame();
    CHECK(!drm_crtc_crc_ready(&pa->base), "crc: the first frame dropped");
    frame();
    CHECK(crc_line(&fr, &crc) && fr == (uint32_t)pa->base.vblank.count &&
              crc == reg(PIPE_CRC_RES_HSW(0)) &&
              crc == i915_model_pipe_crc(&model, 0, PIPE_CRC_ENABLE | PIPE_CRC_SOURCE_PRIMARY_IVB),
          "crc: the frame's count and the chip's CRC");
    frame();
    CHECK(crc_line(&fr, &crc) && fr == (uint32_t)pa->base.vblank.count, "crc: a line a frame");
    drm_crtc_crc_release(&pa->base);
    CHECK(reg(PIPE_CRC_CTL(0)) == 0 && pa->crc_ctl == 0, "crc: stopped at the release");
    CHECK(drm_crtc_crc_set_source(&pa->base, "plane2", 6) == 6 &&
              drm_crtc_crc_open(&pa->base) == 0 &&
              reg(PIPE_CRC_CTL(0)) == (PIPE_CRC_ENABLE | PIPE_CRC_SOURCE_SPRITE_IVB),
          "crc: plane2 is the sprite");
    drm_crtc_crc_release(&pa->base);
}

/* ---- 78-10b: mode sets ---- */

/** The log index of the first write to `r` whose bits under `mask` are
 * `want`, at or after `from`, or -1. */
static long first_write(uint32_t r, uint32_t mask, uint32_t want, uint32_t from)
{
    uint32_t i;

    for (i = from; i < model.nlog; i++) {
        if (model.log[i].op == I915_MODEL_WRITE && model.log[i].reg == r &&
            (model.log[i].value & mask) == want) {
            return (long)i;
        }
    }
    return -1;
}

/** Whether the indices `n` are each found and in rising order. */
static int in_order(const long *n, int count)
{
    int i;

    for (i = 0; i < count; i++) {
        if (n[i] < 0 || (i > 0 && n[i] <= n[i - 1])) {
            return 0;
        }
    }
    return 1;
}

/** Set the connector's "scaling mode" to `mode` by ATOMIC. */
static int32_t set_scaling(uint64_t mode)
{
    struct req r;

    k_memset(&r, 0, sizeof(r));
    req_add(&r, &gpu.display.connector.base, gpu.display.connector.scaling_mode_property, mode);
    return req_commit(&r, DRM_MODE_ATOMIC_ALLOW_MODESET);
}

/**
 * A mode smaller than the panel's: the pipe off and on again in Linux's
 * register order (hsw_crtc_disable, then hsw_crtc_enable with the eDP
 * encoder's hooks), the source 1024x768 fitted to the panel's 1366x768
 * with its aspect kept, then stretched by "scaling mode" Full, and the
 * panel's own mode again with the fitter off.
 */
static void row_modeset(void)
{
    struct drm_mode_modeinfo small = pa->hw.mode;
    uint32_t from = model.nlog;
    long off[7], on[9];

    small.hdisplay = 1024;
    drm_mode_set_name(&small);
    log_text[0] = 0;
    CHECK(setcrtc(&small, fb_con) == 0, "modeset: SETCRTC of 1024x768");
    off[0] = first_write(PP_CONTROL, EDP_BLC_ENABLE, 0, from);
    off[1] = first_write(TRANSCONF(I915_TRANS_EDP), TRANSCONF_ENABLE, 0, from);
    off[2] = first_write(TRANS_DDI_FUNC_CTL(I915_TRANS_EDP), TRANS_DDI_FUNC_ENABLE, 0, from);
    off[3] = first_write(DDI_BUF_CTL(0), DDI_BUF_CTL_ENABLE, 0, from);
    off[4] = first_write(PP_CONTROL, PANEL_POWER_ON, 0, from);
    off[5] = first_write(PORT_CLK_SEL(0), PORT_CLK_SEL_MASK, PORT_CLK_SEL_NONE, from);
    off[6] = first_write(PORT_CLK_SEL(0), PORT_CLK_SEL_MASK, PORT_CLK_SEL_LCPLL_1350, from);
    CHECK(in_order(off, 7),
          "modeset: off as Linux: backlight, transcoder, its DDI function, buffer, power, clock");
    from = (uint32_t)off[6];
    on[0] = from;
    on[1] = first_write(DDI_BUF_CTL(0), DDI_BUF_CTL_ENABLE, DDI_BUF_CTL_ENABLE, from);
    on[2] = first_write(PIPESRC(0), 0xffffffffu, 1023u << 16 | 767u, from);
    on[3] = first_write(TRANS_HTOTAL(I915_TRANS_EDP), 0xffffffffu, 1365u | 1557u << 16, from);
    on[4] = first_write(PF_CTL(0), 0xffffffffu, PF_ENABLE | PF_FILTER_MED_3x3, from);
    on[5] = first_write(TRANS_DDI_FUNC_CTL(I915_TRANS_EDP), TRANS_DDI_FUNC_ENABLE,
                        TRANS_DDI_FUNC_ENABLE, from);
    on[6] = first_write(TRANSCONF(I915_TRANS_EDP), TRANSCONF_ENABLE, TRANSCONF_ENABLE, from);
    on[7] = first_write(DP_TP_CTL(0), DP_TP_CTL_LINK_TRAIN_MASK, DP_TP_CTL_LINK_TRAIN_NORMAL, from);
    on[8] = first_write(PP_CONTROL, EDP_BLC_ENABLE, EDP_BLC_ENABLE, from);
    CHECK(in_order(on, 9),
          "modeset: on as Linux: clock, trained buffer, source, timings, fitter, DDI function, "
          "transcoder, normal pattern, backlight");
    CHECK(reg(PF_WIN_POS(0)) == PF_WIN_XPOS(171) &&
              reg(PF_WIN_SZ(0)) == (PF_WIN_XSIZE(1024) | 768u) &&
              (reg(TRANS_DDI_FUNC_CTL(I915_TRANS_EDP)) & TRANS_DDI_EDP_INPUT_MASK) ==
                  TRANS_DDI_EDP_INPUT_A_ONOFF,
          "modeset: Full aspect pillarboxes 1024x768 in the middle, through the fitter's well");
    CHECK((reg(TRANSCONF(I915_TRANS_EDP)) & TRANSCONF_ENABLE) &&
              (reg(PP_CONTROL) & PANEL_POWER_ON) && (reg(DSPCNTR(0)) & DISP_ENABLE) &&
              reg(DSPSURF(0)) == BO_GGTT(0),
          "modeset: the transcoder, the panel and the console's plane on");
    CHECK(logged("display: pipe A set: 1024x768 in 1366x768@60, 76300 kHz, 18 bpp, 1 lane at "
                 "270000 kHz, fitted"),
          "modeset: the log's line");
    frame();
    frame();
    CHECK(set_scaling(DRM_MODE_SCALE_FULLSCREEN) == 0 && reg(PF_WIN_POS(0)) == 0 &&
              reg(PF_WIN_SZ(0)) == (PF_WIN_XSIZE(1366) | 768u),
          "modeset: scaling mode Full stretches it, by a mode set");
    frame();
    CHECK(setcrtc(&pa->hw.mode, fb_con) == 0 && reg(PF_CTL(0)) == 0 &&
              reg(PIPESRC(0)) == (1365u << 16 | 767u) &&
              (reg(TRANS_DDI_FUNC_CTL(I915_TRANS_EDP)) & TRANS_DDI_EDP_INPUT_MASK) ==
                  TRANS_DDI_EDP_INPUT_A_ON,
          "modeset: the panel's own mode with the fitter off");
    frame();
}

/** The display let go: the backlight left to a stand-in, every pin and
 * framebuffer released. */
static void row_fini(void)
{
    intel_display_fini(&gpu);
    CHECK(gpu.display.bl_index == -1 && bl_show(BACKLIGHT_ACTUAL) == 0,
          "fini: intel_backlight left without a driver");
    drm_mode_file_release(&fa);
    drm_mode_config_cleanup(&gpu.drm);
    CHECK(bo_vma[0].pin_count == 0 && bo_vma[1].pin_count == 0 && bo_vma[3].pin_count == 0 &&
              bo_vma[7].pin_count == 0,
          "fini: every scanout unpinned");
}

int main(void)
{
    model_start("i915-display-test");
    CHECK(bring_up() == 0, "the display is made");
    row_readout();
    row_adopt();
    row_probe();
    row_formats();
    row_takeover();
    row_flip();
    row_async();
    row_count();
    row_primary();
    row_sprite();
    row_cursor();
    row_backlight();
    row_crc();
    row_modeset();
    row_fini();
    if (fw_obj && fw_obj->refs == 0) {
        fw_obj->funcs->free(fw_obj);
    }
    i915_model_free(&model);
    intel_bios_fini(&gpu.vbt);
    if (failures == 0) {
        printf("i915-display-test: ok\n");
    }
    return failures != 0;
}
