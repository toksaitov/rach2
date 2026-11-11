/*
 * i915_display_hosttest.c: the Intel graphics driver's display on Haswell,
 * intel_display.c and the files beside it, driven through the DRM core's
 * ioctl handlers against the host model of the Lenovo's chip, whose pipe
 * block keeps the firmware's picture on pipe A.
 *
 * host-deps: rach2/sys/drm/i915/i915_irq.c rach2/sys/drm/i915/i915_hw.c
 * host-deps: tests/host/i915_model.c rach2/sys/display/backlight.c
 * host-deps: rach2/sys/display/intelbl.c rach2/sys/lib/kstring.c
 * host-deps: rach2/sys/lib/kstd.c
 * host-deps: rach2/sys/lib/kctypes.c tests/host/compat/spinlock_host.c
 * host-deps: rach2/sys/drm/i915/intel_bios.c rach2/sys/drm/i915/i915_power.c
 * host-deps: tests/host/compat/resv_host.c
 * host-inputs: tests/fixtures/lenovo-gpu/i915_vbt.bin
 *
 * The rows: the readout of pipe A, the eDP transcoder, DDI A, the plane and the
 * panel power sequencer, against what Linux's i915_display_info read on the
 * Lenovo (78-9.1); the bind's adoption, which writes no register of the pipe,
 * its link or the backlight but reads the panel over AUX, the eDP connector's
 * probe, and the console's takeover commit, which keeps the mode, pins the new
 * framebuffer and lets the firmware's go once the next flip replaced it, while
 * a refresh rate the panel lacks is refused (78-9.2); the watermarks of the
 * console's plane, the low-power levels on after the vblank; the planes'
 * formats, Linux's Haswell lists, and their checks of stride, tiling and
 * offset, the X-tiled offset's split and the sprite's registers and watermarks
 * (78-9.3); a page flip's event at the vblank that shows it, with its count and
 * a rising stamp, an async flip's at flip done, and the count of vblanks the
 * frame counter kept while the interrupt was off (78-9.4), and the
 * interactive mark of RPS held by each pipe of a commit until its own
 * flip ends (80-46.1); the cursor's
 * registers and the backlight, taken over from the boot's driver, set and
 * switched, off with the pipe, its transcoder and the panel's power, and given
 * back at the end (78-9.5); the pipe CRC through crc/control and crc/data,
 * the fitter forced through by a mode set while a source runs and let go after,
 * "auto" the fitter's output and its first frame dropped (78-9.6); and a mode
 * smaller than the panel's, set in Linux's register order through the fitter
 * with its aspect kept, then stretched by "scaling mode", and the panel's own
 * mode again, and a link that fails to train, its status set bad for a hotplug
 * event and good again with the next mode set (78-10b); and a framebuffer
 * 16384 pixels wide, past the planes' strides, shown from x 9000 on the
 * primary plane and on the sprite through a remapped view of its source
 * rectangle, each pixel the plane reads checked against the source's, and
 * each view unpinned once the flip after the one that replaced it latched
 * (78-16).
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
#include "drm/i915/intel_gmbus.c"
#include "drm/i915/intel_dpll.c"
#include "drm/i915/intel_hdmi.c"
#include "drm/i915/intel_hotplug.c"
#include "drm/i915/intel_ddi.c"
#include "drm/i915/intel_dp_link_training.c"
#include "drm/i915/intel_dp.c"
#include "drm/i915/intel_panel.c"
#include "drm/i915/intel_pfit.c"
#include "drm/i915/intel_wm.c"
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
    obj->resv = &obj->resv_own;
    obj->size = size;
    obj->funcs = funcs;
    obj->refs = 1;
}

/** The file's tables are zeroed and never shared here: nothing to make. */
void drm_file_locks_init(struct drm_file *file)
{
    (void)file;
}

/* ---- the driver's buffer objects: a few, bound in the aperture by the
 * pin, which counts what it is asked ---- */

#define NBO    20
#define NREMAP 4

const struct drm_gem_object_funcs i915_gem_object_funcs;
static struct drm_i915_gem_object bo[NBO];
static struct i915_vma bo_vma[NBO];
static struct i915_vma remap_vma[NBO][NREMAP]; /* each object's remapped views */
static int pins, unpins, flushes;
static uint32_t pin_align;
static uint64_t flush_start, flush_end;

/** The GGTT address the pin binds object `i` at, and its remapped view in
 * slot `j`. */
#define BO_GGTT(i)       (0x01000000u + (uint32_t)(i) * 0x00800000u)
#define REMAP_GGTT(i, j) (0x10000000u + ((uint32_t)(i) * NREMAP + (uint32_t)(j)) * 0x00400000u)

/** Whether the remapped views `a` and `b` are one. */
static int remap_same(const struct i915_remap_info *a, const struct i915_remap_info *b)
{
    return a->offset == b->offset && a->width == b->width && a->height == b->height &&
           a->src_stride == b->src_stride;
}

/** The pin, as i915_vma.c's: a binding of the same view reused, a
 * remapped view in a slot of its own, the first unpinned one, while a
 * binding stays on its object's list once made. */
int32_t i915_gem_object_pin_to_display(struct drm_i915_gem_object *obj,
                                       const struct i915_remap_info *remap, uint32_t align,
                                       struct i915_vma **vma)
{
    int i = (int)(obj - bo), j, slot = -1;
    struct i915_vma *v = &bo_vma[i];

    if (remap) {
        for (j = 0; j < NREMAP; j++) {
            if (remap_vma[i][j].obj && remap_same(&remap_vma[i][j].remap, remap)) {
                slot = j;
                break;
            }
            if (slot < 0 && remap_vma[i][j].pin_count == 0) {
                slot = j;
            }
        }
        CHECK(slot >= 0, "the pin finds a slot for a remapped view");
        if (slot < 0) {
            return -ENOSPC;
        }
        v = &remap_vma[i][slot];
        v->start = REMAP_GGTT(i, slot);
        v->remap = *remap;
        v->view_pages = remap->width * remap->height;
    } else {
        v->start = BO_GGTT(i);
    }
    if (!v->obj) {
        v->obj = obj;
        v->mappable = 1;
        v->obj_next = obj->vmas;
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
/* The page flips RPS counts pending, as i915_rps_mark_interactive keeps
 * them, never below none. */
static int interactive;
/* The waitboosts asked, and the request of the last. */
static int boosts_done;
static struct i915_request *boosted_last;
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
 * one: its vblank and the messages it raises, then its CRC done and
 * those. */
static void frame(void)
{
    model.now_us += 16667u;
    follow_clock(&model);
    i915_model_pipe_vblank(&model, 0);
    take_messages();
    i915_model_pipe_crc_done(&model, 0);
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
    /* The reference the bind's i915_power_init holds for its life. */
    gpu.power.count = 1;
    gpu.power.on = 1;
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
    k_memset(remap_vma[i], 0, sizeof(remap_vma[i]));
    bo[i].base.dev = &gpu.drm;
    bo[i].base.size = size;
    bo[i].base.funcs = &i915_gem_object_funcs;
    bo[i].base.resv = &bo[i].base.resv_own;
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

/** The bind's watermarks, Linux's ilk_wm_sanitize at the latencies 2, 2,
 * 18, 45 and 80 us: level 0, and LP1 level 1, LP2 level 3 and LP3 level 4
 * with each level's latency field, for the adopted plane of 1366 pixels. */
static void row_adopt_wm(void)
{
    CHECK(reg(WM0_PIPE_ILK(0)) == 12u << 16 && reg(WM1_LP_ILK) == 0x82300c00u &&
              reg(WM2_LP_ILK) == 0x8650d900u && reg(WM3_LP_ILK) == 0x88715800u,
          "adopt: the watermarks computed for the adopted plane and written");
}

/** The bind's adoption: no register of the pipes, planes, transcoders,
 * DDI buffers, clocks or backlight written, the panel's sequencer given
 * Linux's delays and AUX messages sent; pipe A on in the atomic state with
 * the firmware's mode and framebuffer, and its configuration read. */
static void row_adopt(void)
{
    struct drm_plane_state *ps = pa->primary.state;

    CHECK(!wrote_range(0x60000u, 0x6400fu, log_at_init) &&
              !wrote_range(0x64028u, 0x7ffffu, log_at_init) &&
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
    /* A plane of the same 1366 pixels needs the watermarks the bind
     * wrote, and Linux's ilk_write_wm_values writes only a change. */
    CHECK(reg(WM0_PIPE_ILK(0)) == 12u << 16 && !pa->wm_pending &&
              !wrote_range(0x45100u, 0x4512bu, from) && reg(WM1_LP_ILK) == 0x82300c00u &&
              reg(WM2_LP_ILK) == 0x8650d900u && reg(WM3_LP_ILK) == 0x88715800u,
          "takeover: the watermarks of 1366 pixels kept, none written");
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
    /* The vblank handled at its start: PIPEDSL gives line 767 of the
     * panel's mode, 48 lines of 1558 pixels at 76.3 MHz before the
     * vertical blank ends, as Linux's i915_get_crtc_scanoutpos counts. */
    i915_model_pipes(&model)->pipe[0].us = 0;
    frame();
    CHECK(pa->base.vblank.time_ns == model_now_ns + 980131,
          "flip: the vblank stamped at the end of the vertical blank from PIPEDSL");
    CHECK(nsent == n0 + 1 && sent[n0].base.type == DRM_EVENT_FLIP_COMPLETE &&
              sent[n0].user_data == 0x1234 && sent[n0].crtc_id == pa->base.base.id,
          "flip: the event sent at the vblank");
    CHECK(sent[n0].sequence == (uint32_t)pa->base.vblank.count &&
              (int64_t)sent[n0].tv_sec * 1000000000ll + (int64_t)sent[n0].tv_usec * 1000ll ==
                  (model_now_ns + 980131) / 1000 * 1000,
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

/**
 * A commit's interactive mark of RPS, one a commit as Linux's
 * state->rps_interactive, kept until each of the commit's flips ends: a
 * commit on pipes B and C counts one mark, which the later of the two
 * flips lets go; a second commit, on pipe A, counts one of its own; a
 * failed commit leaves none. `interactive_handover` is driven directly,
 * the pipes' pending flips set by hand, since the model lights pipe A
 * alone and no frame runs meanwhile.
 */
static void row_interactive(void)
{
    struct intel_crtc *b = &gpu.display.crtc[1], *c = &gpu.display.crtc[2];
    int n0 = interactive;

    b->base.flip_pending = c->base.flip_pending = DRM_FLIP_ARMED;
    i915_rps_mark_interactive(&gpu, 1);
    interactive_handover(&gpu, 6u, 0);
    CHECK(interactive == n0 + 1 && b->rps_mark && b->rps_mark == c->rps_mark,
          "interactive: the commit's pending pipes share one mark");
    pa->base.flip_pending = DRM_FLIP_ARMED;
    i915_rps_mark_interactive(&gpu, 1);
    interactive_handover(&gpu, 1u, 0);
    CHECK(interactive == n0 + 2 && pa->rps_mark && pa->rps_mark != b->rps_mark,
          "interactive: a second commit counts a mark of its own");
    pa->base.flip_pending = DRM_FLIP_NONE;
    intel_crtc_interactive_end(pa);
    CHECK(interactive == n0 + 1 && !pa->rps_mark, "interactive: its flip ends, its mark goes");
    b->base.flip_pending = DRM_FLIP_NONE;
    intel_crtc_interactive_end(b);
    CHECK(interactive == n0 + 1 && !b->rps_mark && c->rps_mark,
          "interactive: pipe B's flip ends, the commit's mark stays for pipe C");
    c->base.flip_pending = DRM_FLIP_NONE;
    intel_crtc_interactive_end(c);
    CHECK(interactive == n0 && !c->rps_mark, "interactive: the last flip ends, no mark left");
    b->base.flip_pending = c->base.flip_pending = DRM_FLIP_ARMED;
    i915_rps_mark_interactive(&gpu, 1);
    interactive_handover(&gpu, 6u, -EBUSY);
    CHECK(interactive == n0 && !b->rps_mark && !c->rps_mark,
          "interactive: a failed commit leaves no mark");
    b->base.flip_pending = c->base.flip_pending = DRM_FLIP_NONE;
}

/**
 * The boosts at the vblank, Linux's intel_display_rps_boost_after_vblank
 * and do_rps_boost: a flip whose in-fence is another driver's boosts
 * nothing and takes no other fence in its place; one whose in-fence is an
 * i915 request, and a second commit's request before the vblank, keep an
 * entry each; the vblank boosts the request the chip has not begun and
 * lets both go; a pipe whose vblanks go off ends its boosts at once, as
 * Linux's drm_crtc_vblank_off wakes their waits; and a deferred commit's
 * tail boosts at once a request the chip has not begun, as Linux's
 * intel_atomic_commit_fence_wait does, and nothing else.
 */
static void row_boosts(void)
{
    static const struct dma_fence_ops other_ops = {0, 0, 0, 0, 0, 0};
    struct intel_crtc *b = &gpu.display.crtc[1];
    struct i915_request rq[3];
    struct dma_fence other;
    struct drm_plane_state ps;
    int n0 = boosts_done, refs0 = fence_refs;

    k_memset(rq, 0, sizeof(rq));
    k_memset(&other, 0, sizeof(other));
    k_memset(&ps, 0, sizeof(ps));
    rq[0].fence.base.ops = rq[1].fence.base.ops = rq[2].fence.base.ops = &i915_fence_ops;
    other.ops = &other_ops;
    ps.fence = unsignaled = &other;
    flip_boost(pa, &ps);
    CHECK(!pa->boosts && fence_refs == refs0,
          "boosts: another driver's in-fence boosts nothing, and no other fence in its place");
    ps.fence = unsignaled = &rq[0].fence.base;
    flip_boost(pa, &ps);
    intel_crtc_boost_after_vblank(pa, &rq[1].fence.base);
    CHECK(pa->boosts && pa->boosts->next && !pa->boosts->next->next && fence_refs == refs0 + 2,
          "boosts: an i915 in-fence and a second commit's request keep an entry each");
    rq[1].fence.base.signaled = 1;
    frame();
    CHECK(boosts_done == n0 + 1 && boosted_last == &rq[0] && !pa->boosts && fence_refs == refs0,
          "boosts: the vblank boosts the request not begun alone and lets both go");
    /* Pipe B's vblanks on as a mode set turns them on; their off leaves
     * them held off again, as the core's reset made them. */
    drm_crtc_vblank_on(&b->base);
    intel_crtc_boost_after_vblank(b, &rq[2].fence.base);
    CHECK(b->boosts && fence_refs == refs0 + 1, "boosts: one on pipe B");
    intel_crtc_vblank_off(b);
    CHECK(!b->boosts && boosts_done == n0 + 2 && boosted_last == &rq[2] && fence_refs == refs0 &&
              b->base.vblank.inmodeset && b->base.vblank.refs == 1,
          "boosts: pipe B's vblanks off end it at once, boosted");
    n0 = boosts_done;
    mc->commit_fence_wait(&gpu.drm, &other);
    mc->commit_fence_wait(&gpu.drm, &rq[1].fence.base);
    CHECK(boosts_done == n0, "boosts: a deferred commit's tail boosts no other driver's fence "
                             "and no request begun");
    mc->commit_fence_wait(&gpu.drm, &rq[0].fence.base);
    CHECK(boosts_done == n0 + 1 && boosted_last == &rq[0],
          "boosts: a deferred commit's tail boosts a request not begun at once");
    unsignaled = 0;
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

/** Whether object `o` has property `prop`. */
static int has_prop(const struct drm_mode_object *o, const struct drm_property *prop)
{
    uint32_t i;

    for (i = 0; i < o->count_props; i++) {
        if (o->props[i] == prop) {
            return 1;
        }
    }
    return 0;
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
    CHECK(!has_prop(&pa->primary.base, mc->prop_fb_damage_clips) &&
              !has_prop(&pa->sprite.base, mc->prop_fb_damage_clips) &&
              !has_prop(&pa->cursor.base, mc->prop_fb_damage_clips),
          "formats: no FB_DAMAGE_CLIPS, as Linux's planes before display version 12");
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

/**
 * ADDFB2 of tiled objects, as Linux's intel_framebuffer_init checks them
 * (IGT's kms_addfb_basic): an X-tiled object's framebuffer made without
 * modifiers takes X tiles and its object counts it, at a pitch other than
 * the object's stride refused; a modifier that is not the object's tiling
 * refused; a Y-tiled object refused without modifiers; X tiles that do not
 * fit the object in whole rows of tiles refused.
 */
static void row_addfb_tiling(void)
{
    uint32_t hx = bo_new(10, 4096u * 1024u, I915_TILING_X, 4096);
    uint32_t hy = bo_new(11, 4096u * 1024u, I915_TILING_Y, 4096);
    uint32_t hs = bo_new(9, 4096u * 1023u, I915_TILING_X, 4096);
    struct drm_framebuffer *f;
    uint32_t fb;

    fb = addfb(hx, 512, 512, DRM_FORMAT_XRGB8888, 4096, NO_MODIFIER);
    f = drm_framebuffer_lookup(&gpu.drm, &fa, fb);
    CHECK(f && f->modifier == I915_FORMAT_MOD_X_TILED && bo[10].fb_refs == 1,
          "addfb: an X-tiled object without modifiers, X tiles, counted by the object");
    drm_framebuffer_put(f);
    CHECK(drm_mode_rmfb_ioctl(&gpu.drm, &fb, &fa) == 0 && bo[10].fb_refs == 0,
          "addfb: the count let go with the framebuffer");
    CHECK(addfb(hx, 512, 512, DRM_FORMAT_XRGB8888, 2048, NO_MODIFIER) == 0,
          "addfb: a pitch not the object's stride refused");
    CHECK(addfb(hx, 1024, 1024, DRM_FORMAT_XRGB8888, 4096, DRM_FORMAT_MOD_LINEAR) == 0,
          "addfb: linear of an X-tiled object refused");
    CHECK(addfb(hy, 512, 512, DRM_FORMAT_XRGB8888, 4096, NO_MODIFIER) == 0,
          "addfb: a Y-tiled object without modifiers refused");
    fb = addfb(hs, 1024, 1016, DRM_FORMAT_XRGB8888, 4096, NO_MODIFIER);
    CHECK(addfb(hs, 1024, 1020, DRM_FORMAT_XRGB8888, 4096, NO_MODIFIER) == 0 && fb != 0 &&
              drm_mode_rmfb_ioctl(&gpu.drm, &fb, &fa) == 0,
          "addfb: X tiles past the object in whole rows refused, within it taken");
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
    CHECK(((reg(WM0_PIPE_ILK(0)) >> 8) & 0xffu) == 6u && reg(WM1S_LP_ILK) == 6u &&
              reg(WM2S_LP_IVB) == 14u && reg(WM3S_LP_IVB) == 18u &&
              (reg(WM3_LP_ILK) & WM_LP_ENABLE) && !(reg(WM_MISC) & WM_MISC_DATA_PARTITION_5_6),
          "sprite: its watermarks at levels 0, 1, 3 and 4, the FIFO split 1:1");
    frame();
    CHECK(i915_model_pipes(&model)->pipe[0].sprite.surf == BO_GGTT(8),
          "sprite: scanned after the vblank");
    CHECK(plane_commit(&pa->sprite, 0, &full, 0) == 0 && reg(SPRCTL(0)) == 0, "sprite: turned off");
    frame();
}

/* ---- 78-16: framebuffers wider than a plane's stride ---- */

/** The byte of pixel (`px`, `py`) of a surface of `cpp` bytes a pixel at
 * `pitch`, X-tiled, 512 bytes by 8 rows a tile, or linear. */
static uint64_t pixel_byte(int tiled, uint32_t pitch, uint32_t cpp, uint32_t px, uint32_t py)
{
    uint32_t tw = 512u / cpp;

    if (!tiled) {
        return (uint64_t)py * pitch + (uint64_t)px * cpp;
    }
    return ((uint64_t)(py / 8u) * (pitch / 512u) + px / tw) * 4096u + (py % 8u) * 512u +
           (px % tw) * cpp;
}

/* A plane's source: the framebuffer's layout, X-tiled or linear, its
 * bytes a pixel and stride, its object's first byte its start, and the
 * rectangle's corner and size. */
struct source {
    int tiled;
    uint32_t cpp, pitch;
    uint32_t sx, sy, w, h;
};

/** Whether the plane whose surface, stride and offset registers are
 * `surf`, `stride` and `offset`, reading `src` through the remapped binding
 * `v`, reads at each pixel the byte of the source's pixel: the view's page
 * list checked against the source rectangle, pixel by pixel. */
static int view_reads_source(const struct i915_vma *v, uint32_t surf, uint32_t stride,
                             uint32_t offset, const struct source *src)
{
    uint32_t i, j, x = offset & 0xffffu, y = offset >> 16;

    if (surf < v->start) {
        return 0;
    }
    for (j = 0; j < src->h; j++) {
        for (i = 0; i < src->w; i++) {
            uint64_t at =
                (surf - v->start) + pixel_byte(src->tiled, stride, src->cpp, x + i, y + j);
            uint64_t page = at >> 12;

            if (page >= v->view_pages ||
                (uint64_t)I915_REMAP_PAGE(&v->remap, (uint32_t)page) * 4096u + (at & 0xfffu) !=
                    pixel_byte(src->tiled, src->pitch, src->cpp, src->sx + i, src->sy + j)) {
                return 0;
            }
        }
    }
    return 1;
}

/**
 * An X-tiled framebuffer 16384 pixels wide, whose 64 KiB stride passes the
 * primary plane's 32 KiB, shown from (9000, 100) through a remapped view
 * of the source's tiles alone, as Linux's intel_plane_remap_gtt makes it; a
 * pan binds the next view, and each is unpinned once the flip after the
 * one that replaced it has latched, as the other scanouts are.
 */
static void row_remap_primary(void)
{
    static const struct place at = {9000, 100, 1366, 768, 0, 0, 1366, 768};
    static const struct place pan = {9100, 100, 1366, 768, 0, 0, 1366, 768};
    static const struct source src = {1, 4, 65536, 9000, 100, 1366, 768};
    static const struct source panned = {1, 4, 65536, 9100, 100, 1366, 768};
    uint32_t h = bo_new(16, 65536u * 1024u, I915_TILING_X, 65536);
    uint32_t fb = addfb(h, 16384, 1024, DRM_FORMAT_XRGB8888, 65536, I915_FORMAT_MOD_X_TILED);
    const struct i915_vma *v = &remap_vma[16][0], *next = &remap_vma[16][1];

    CHECK(fb && plane_commit(&pa->primary, fb, &at, 0) == 0,
          "remap: a 16384-wide X-tiled framebuffer on the primary plane from x 9000");
    CHECK(v->pin_count == 1 && bo_vma[16].pin_count == 0 && v->remap.offset == 12u * 128u + 70u &&
              v->remap.width == 11 && v->remap.height == 97 && v->remap.src_stride == 128,
          "remap: a view of the source's 11 by 97 tiles pinned, not the whole object");
    CHECK(reg(DSPSURF(0)) == REMAP_GGTT(16, 0) && reg(DSPSTRIDE(0)) == 11u * 512u &&
              reg(DSPOFFSET(0)) == (4u << 16 | 40u) && (reg(DSPCNTR(0)) & DISP_TILED),
          "remap: the plane reads the view at a stride of 11 tiles, the corner in the offset");
    CHECK(view_reads_source(v, reg(DSPSURF(0)), reg(DSPSTRIDE(0)), reg(DSPOFFSET(0)), &src),
          "remap: each pixel the primary plane reads is the source's");
    frame();
    CHECK(i915_model_pipes(&model)->pipe[0].primary.surf == REMAP_GGTT(16, 0),
          "remap: the view scanned after the vblank");
    CHECK(plane_commit(&pa->primary, fb, &pan, 0) == 0 && next->pin_count == 1 &&
              next->remap.offset == 12u * 128u + 71u && v->pin_count == 1 &&
              reg(DSPSURF(0)) == REMAP_GGTT(16, 1) && reg(DSPOFFSET(0)) == (4u << 16 | 12u),
          "remap: a pan to x 9100 binds the next view, the shown one kept");
    CHECK(view_reads_source(next, reg(DSPSURF(0)), reg(DSPSTRIDE(0)), reg(DSPOFFSET(0)), &panned),
          "remap: each pixel read after the pan is the source's");
    frame();
    CHECK(plane_commit(&pa->primary, fb_con, &full, 0) == 0 && v->pin_count == 0 &&
              next->pin_count == 1,
          "remap: back to the console, the first view unpinned, the latched one kept");
    frame();
    CHECK(plane_commit(&pa->primary, fb_con, &full, 0) == 0 && next->pin_count == 0 &&
              reg(DSPSTRIDE(0)) == 5504 && reg(DSPSURF(0)) == BO_GGTT(0),
          "remap: the second view unpinned after the next flip, the console as before");
    frame();
}

/**
 * A linear framebuffer 16384 pixels wide, its 64 KiB stride whole pages
 * past the sprite's 16 KiB, shown from (9000, 300) through a remapped view
 * of a page of each source row; unpinned once the sprite is off and a flip
 * has passed.
 */
static void row_remap_sprite(void)
{
    static const struct place at = {9000, 300, 64, 64, 10, 20, 64, 64};
    static const struct source src = {0, 4, 65536, 9000, 300, 64, 64};
    uint32_t h = bo_new(17, 65536u * 1024u, I915_TILING_NONE, 0);
    uint32_t fb = addfb(h, 16384, 1024, DRM_FORMAT_XRGB8888, 65536, NO_MODIFIER);
    const struct i915_vma *v = &remap_vma[17][0];

    CHECK(fb && plane_commit(&pa->sprite, fb, &at, 0) == 0,
          "remap: a 16384-wide linear framebuffer on the sprite from x 9000");
    CHECK(v->pin_count == 1 && bo_vma[17].pin_count == 0 &&
              v->remap.offset == (300u * 65536u + 9000u * 4u) / 4096u && v->remap.width == 1 &&
              v->remap.height == 64 && v->remap.src_stride == 16,
          "remap: the sprite's view, a page of each of its 64 rows");
    CHECK(reg(SPRSURF(0)) == REMAP_GGTT(17, 0) && reg(SPRSTRIDE(0)) == 4096 &&
              reg(SPROFFSET(0)) == 808u,
          "remap: the sprite reads the view at a stride of a page, the corner in the offset");
    CHECK(view_reads_source(v, reg(SPRSURF(0)), reg(SPRSTRIDE(0)), reg(SPROFFSET(0)), &src),
          "remap: each pixel the sprite reads is the source's");
    frame();
    CHECK(plane_commit(&pa->sprite, 0, &full, 0) == 0 && v->pin_count == 1,
          "remap: the sprite off, its view kept until the flip latches");
    frame();
    CHECK(plane_commit(&pa->sprite, 0, &full, 0) == 0 && v->pin_count == 0,
          "remap: the sprite's view unpinned after the next flip");
    frame();
}

/* ---- 78-9.5: the cursor and the backlight ---- */

/** A cursor lower than it is wide, as Linux's i9xx_cursor_size_ok takes
 * on Haswell: shown from a taller framebuffer, as IGT's probe of a size
 * asks, refused under 8 lines, and its height in CUR_FBC_CTL; `h` is the
 * handle of a 64-pixel square. */
static void row_cursor_height(uint32_t h)
{
    static const struct place low = {0, 0, 64, 32, 0, 0, 64, 32};
    static const struct place thin = {0, 0, 64, 4, 0, 0, 64, 4};
    struct drm_mode_cursor c;
    uint32_t fb = addfb(h, 64, 64, DRM_FORMAT_ARGB8888, 256, NO_MODIFIER);

    CHECK(fb && plane_commit(&pa->cursor, fb, &low, DRM_MODE_ATOMIC_TEST_ONLY) == 0,
          "cursor: 64 by 32 of a taller framebuffer taken");
    CHECK(plane_commit(&pa->cursor, fb, &thin, DRM_MODE_ATOMIC_TEST_ONLY) == -EINVAL,
          "cursor: under 8 lines refused");
    CHECK(drm_mode_rmfb_ioctl(&gpu.drm, &fb, &fa) == 0, "cursor: the framebuffer removed");
    k_memset(&c, 0, sizeof(c));
    c.crtc_id = pa->base.base.id;
    c.flags = DRM_MODE_CURSOR_BO;
    c.handle = h;
    c.width = 64;
    c.height = 32;
    CHECK(drm_mode_cursor_ioctl(&gpu.drm, &c, &fa) == 0 &&
              reg(CUR_FBC_CTL(0)) == (CUR_FBC_EN | 31u) &&
              reg(CURCNTR(0)) == MCURSOR_MODE_64_ARGB_AX,
          "cursor: 64 by 32, its height in CUR_FBC_CTL");
    frame();
}

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
    row_cursor_height(h);
    c.handle = 0;
    CHECK(drm_mode_cursor_ioctl(&gpu.drm, &c, &fa) == 0 && reg(CURCNTR(0)) == 0 &&
              reg(CUR_FBC_CTL(0)) == 0,
          "cursor: hidden");
    frame();
}

/**
 * Cursor images set within one frame through the legacy ioctl, which has
 * no stall: the one the pipe latched stays pinned until a vblank has
 * passed, and one never latched goes at once.
 */
static void row_cursor_unpin(void)
{
    struct drm_mode_cursor c;
    uint32_t k1 = bo_new(12, 64u * 64u * 4u, I915_TILING_NONE, 0);
    uint32_t k2 = bo_new(13, 64u * 64u * 4u, I915_TILING_NONE, 0);
    uint32_t k3 = bo_new(14, 64u * 64u * 4u, I915_TILING_NONE, 0);

    k_memset(&c, 0, sizeof(c));
    c.crtc_id = pa->base.base.id;
    c.flags = DRM_MODE_CURSOR_BO;
    c.width = 64;
    c.height = 64;
    c.handle = k1;
    CHECK(drm_mode_cursor_ioctl(&gpu.drm, &c, &fa) == 0, "cursor unpin: K1 set");
    frame();
    c.handle = k2;
    CHECK(drm_mode_cursor_ioctl(&gpu.drm, &c, &fa) == 0, "cursor unpin: K2 set");
    c.handle = k3;
    CHECK(drm_mode_cursor_ioctl(&gpu.drm, &c, &fa) == 0, "cursor unpin: K3 set in the same frame");
    CHECK(bo_vma[12].pin_count == 1 && bo_vma[13].pin_count == 0 && bo_vma[14].pin_count == 1,
          "cursor unpin: K1, which the pipe latched, stays pinned; K2, never latched, goes");
    frame();
    c.handle = k1;
    CHECK(drm_mode_cursor_ioctl(&gpu.drm, &c, &fa) == 0 && bo_vma[12].pin_count == 1 &&
              bo_vma[14].pin_count == 1,
          "cursor unpin: after a vblank the old K1 goes, K3 kept as the latched one");
    c.handle = 0;
    CHECK(drm_mode_cursor_ioctl(&gpu.drm, &c, &fa) == 0 && bo_vma[12].pin_count == 0 &&
              bo_vma[14].pin_count == 1,
          "cursor unpin: hidden in the same frame, K1 goes and K3 stays");
    frame();
}

/** Whether the pipe takes `m` and the eDP connector, whose list it would
 * be in, refuses it. */
static int pipe_only(const struct drm_mode_modeinfo *m)
{
    struct drm_connector *conn = &gpu.display.connector;

    return intel_crtc_mode_valid(&pa->base, m) == 0 && conn->funcs->mode_valid(conn, m) == -EINVAL;
}

/** The transcoder's least blanking and an hsync at the end of the active
 * area keep a mode out of the connector's list, as Linux's
 * intel_cpu_transcoder_mode_valid in its encoders' mode_valid, while the
 * pipe's own check takes it, as Linux's intel_mode_valid does at a commit. */
static void row_mode_limits(void)
{
    struct drm_connector *conn = &gpu.display.connector;
    struct drm_mode_modeinfo m = pa->hw.mode;
    int ok = intel_crtc_mode_valid(&pa->base, &m) == 0 && conn->funcs->mode_valid(conn, &m) == 0;

    m.hsync_start = m.hdisplay;
    ok &= pipe_only(&m);
    m = pa->hw.mode;
    m.htotal = (uint16_t)(m.hdisplay + 16u);
    ok &= pipe_only(&m);
    m = pa->hw.mode;
    m.vtotal = (uint16_t)(m.vdisplay + 4u);
    ok &= pipe_only(&m);
    m = pa->hw.mode;
    m.hdisplay = 32;
    m.hsync_start = 40;
    ok &= pipe_only(&m);
    CHECK(ok, "mode: the panel's mode taken; no front porch, a short blank or 32 pixels left out "
              "of the connector's modes and taken by the pipe");
}

/** The framebuffer limit of Linux's display 7, 16384 pixels each way: one
 * across three 3840-pixel monitors is made. */
static void row_fb_limit(void)
{
    uint32_t h = bo_new(15, 11520u * 4u * 2160u, I915_TILING_NONE, 0);
    uint32_t fb = addfb(h, 11520, 2160, DRM_FORMAT_XRGB8888, 11520u * 4u, NO_MODIFIER);

    CHECK(mc->max_width == 16384 && mc->max_height == 16384 && fb != 0,
          "fb: 16384 by 16384 at most, a framebuffer 11520 wide made");
    CHECK(fb && drm_mode_rmfb_ioctl(&gpu.drm, &fb, &fa) == 0, "fb: the wide one removed");
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
    /* Linux's scale_user_to_hw: 500 of 937 onto the PWM's 18 to 937, 18
     * being the VBT's least of 5 in 255. */
    CHECK(bl_store(BACKLIGHT_BRIGHTNESS, "500\n") == 0 &&
              (reg(BLC_PWM_PCH_CTL2) & 0xffffu) == 508 && boot_sets == 0 &&
              bl_show(BACKLIGHT_ACTUAL) == 500,
          "backlight: a level through i915's registers, not the boot's, scaled as Linux's");
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
    CHECK((reg(BLC_PWM_PCH_CTL2) & 0xffffu) == 508, "backlight: the level kept");
    frame();
}

/* ---- 78-9.6: the pipe CRC ---- */

/** crc/data's next line as its frame and CRC. */
static int crc_line(uint32_t *frame_no, uint32_t *crc)
{
    /* A line and its NUL are 67 bytes; a shorter read is refused. */
    char buf[96];

    if (drm_crtc_crc_read(&pa->base, buf, sizeof(buf), 1) <= 0) {
        return 0;
    }
    return sscanf(buf, "0x%x 0x%x", frame_no, crc) == 2;
}

/** The CRC of pipe A: a source forces the eDP pipe's fitter through by a
 * mode set and the release undoes it; "auto" is the fitter's output, its
 * first frame dropped, a line a frame with the vblank's count and the
 * chip's result. */
static void row_crc(void)
{
    uint32_t fr = 0, crc = 0;

    CHECK(drm_crtc_crc_set_source(&pa->base, "pipe\n", 5) == 5, "crc: pipe taken on the eDP pipe");
    CHECK(drm_crtc_crc_set_source(&pa->base, "auto", 4) == 4, "crc: auto");
    CHECK(drm_crtc_crc_open(&pa->base) == 0, "crc: crc/data opened");
    CHECK(reg(PIPE_CRC_CTL(0)) == (PIPE_CRC_ENABLE | PIPE_CRC_SOURCE_PF_IVB),
          "crc: auto is the fitter's output");
    CHECK(pa->cfg.force_thru && !pa->cfg.pfit &&
              (reg(TRANS_DDI_FUNC_CTL(I915_TRANS_EDP)) & TRANS_DDI_EDP_INPUT_MASK) ==
                  TRANS_DDI_EDP_INPUT_A_ONOFF &&
              (reg(TRANSCONF(I915_TRANS_EDP)) & TRANSCONF_ENABLE),
          "crc: the eDP input set through the fitter's well by a mode set");
    frame();
    CHECK(!drm_crtc_crc_ready(&pa->base), "crc: the first frame dropped");
    frame();
    CHECK(crc_line(&fr, &crc) && fr == (uint32_t)pa->base.vblank.count &&
              crc == reg(PIPE_CRC_RES_HSW(0)) &&
              crc == i915_model_pipe_crc(&model, 0, PIPE_CRC_ENABLE | PIPE_CRC_SOURCE_PF_IVB),
          "crc: the frame's count and the chip's CRC");
    frame();
    CHECK(crc_line(&fr, &crc) && fr == (uint32_t)pa->base.vblank.count, "crc: a line a frame");
    drm_crtc_crc_release(&pa->base);
    CHECK(reg(PIPE_CRC_CTL(0)) == 0 && pa->crc_ctl == 0 && !pa->cfg.force_thru &&
              (reg(TRANS_DDI_FUNC_CTL(I915_TRANS_EDP)) & TRANS_DDI_EDP_INPUT_MASK) ==
                  TRANS_DDI_EDP_INPUT_A_ON,
          "crc: stopped at the release, the eDP input past the fitter again");
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

/**
 * A link that fails to train in a mode set: the commit done with the
 * connector's link status bad and a hotplug event sent from the commit
 * worker, Linux's modeset retry; the next mode set trains it and makes the
 * status good again.
 */
static void row_link_retry(void)
{
    struct i915_model_sink *s = i915_model_sink(&model);
    struct drm_mode_modeinfo small = pa->hw.mode;
    uint32_t swing = s->cr_swing;

    small.hdisplay = 1024;
    drm_mode_set_name(&small);
    s->cr_swing = 3; /* more than Haswell's swing at any pre-emphasis */
    model_hotplugs = 0;
    CHECK(setcrtc(&small, fb_con) == 0 &&
              gpu.display.connector.state->link_status == DRM_MODE_LINK_STATUS_BAD,
          "retry: the commit done, the link's status bad");
    CHECK(model_hotplugs == 0 && drm_atomic_commit_work() >= 1 && model_hotplugs == 1,
          "retry: the hotplug event sent from the worker");
    s->cr_swing = swing;
    frame();
    CHECK(setcrtc(&pa->hw.mode, fb_con) == 0 &&
              gpu.display.connector.state->link_status == DRM_MODE_LINK_STATUS_GOOD &&
              gpu.display.dp.link_trained && s->trained,
          "retry: the next mode set trains the link, its status good");
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

/* ---- RPS's waitboost and interactive mode, which the driver calls ---- */

/* The requests' fences, i915_gem.c's, which no flip here waits on. */
const struct dma_fence_ops i915_fence_ops = {0, 0, 0, 0, 0, 0};

/** Whether the chip has begun `rq`; no request runs here, so only once
 * it has signalled. */
int i915_request_started(const struct i915_request *rq)
{
    return rq->fence.base.signaled;
}

/** The waitboost, RPS's, counted. */
void i915_rps_boost(struct i915_request *rq)
{
    boosts_done++;
    boosted_last = rq;
}

/** A page flip pending, with `on`, or done, counted in `interactive`. */
void i915_rps_mark_interactive(struct drm_i915_private *i915, int on)
{
    (void)i915;
    if (on) {
        interactive++;
    } else if (interactive > 0) {
        interactive--;
    }
}

int main(void)
{
    model_start("i915-display-test");
    CHECK(bring_up() == 0, "the display is made");
    row_readout();
    row_adopt();
    row_adopt_wm();
    row_probe();
    row_formats();
    row_takeover();
    row_flip();
    row_async();
    row_interactive();
    row_boosts();
    row_count();
    row_primary();
    row_addfb_tiling();
    row_sprite();
    row_remap_primary();
    row_remap_sprite();
    row_cursor();
    row_cursor_unpin();
    row_mode_limits();
    row_fb_limit();
    row_backlight();
    row_crc();
    row_modeset();
    row_link_retry();
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
