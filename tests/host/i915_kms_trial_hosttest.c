/*
 * i915_kms_trial_hosttest.c: the path of the display trial on the Lenovo,
 * 78-10d, run whole against the host model of its HD 4400 as the firmware
 * leaves it, with the straps reporting DDI B and C and no monitor on them.
 *
 * host-deps: rach2/sys/drm/i915/i915_irq.c rach2/sys/drm/i915/i915_hw.c
 * host-deps: tests/host/i915_model.c rach2/sys/display/backlight.c
 * host-deps: rach2/sys/display/intelbl.c rach2/sys/lib/kstring.c
 * host-deps: rach2/sys/lib/kstd.c
 * host-deps: rach2/sys/lib/kctypes.c tests/host/compat/spinlock_host.c
 * host-deps: rach2/sys/drm/i915/intel_bios.c rach2/sys/drm/i915/i915_power.c
 * host-deps: tests/host/compat/resv_host.c
 * host-deps: tests/host/compat/i915_gem_lock_host.c
 * host-deps: tests/host/compat/mutex_none.c
 * host-inputs: tests/fixtures/lenovo-gpu/i915_vbt.bin
 *
 * The rows follow the trial's step in its order: the bind's adoption of
 * pipe A, which writes no register of the pipe, its link or the backlight
 * but writes the watermarks computed for it over the firmware's, and the
 * outputs probed with no hotplug event; GET_PIPE_FROM_CRTC_ID,
 * by which IGT maps each CRTC to its pipe; the desktop's first commit,
 * Weston's at boot, taken on the adopted pipe though the firmware left a
 * sprite watermark with no sprite on; then crcmodes'
 * (dev-log/78-intel/78-10d/crcmodes.c) mode sets, the panel's own mode and
 * four smaller ones under each scaling mode, each by SETPROPERTY and
 * SETCRTC with the panel's timings, its fitter's window as Linux's
 * pch_panel_fitting computes it and the pipe CRC of "auto" read through
 * crc/data as the chip gives it at its CRC done, after the vblank, and
 * IGT's read-crc, ten frames' lines in sequence; the backlight's levels through
 * intel_backlight as Linux's scale_user_to_hw maps them, level 0 dark; IGT's
 * kms_hdmi_inject, the eDP pipe off and HDMI-A-1 forced on with a 4K EDID
 * set on pipe A, with limited range through the colour matrix and no second
 * M/N written on a transcoder that is not eDP's; ten seconds with no
 * hotplug event while the pins are armed; and the panel's mode again. Each
 * row also finds no wait of the driver at its bound in the log.
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
#include "drm/i915/intel_color.c"
#include "drm/i915/intel_wm.c"
#include "i915_model.h"
#include "i915_model_ddi.h"
#include "i915_model_pipe.h"
#include "i915_model_ring.h"
#include "i915_model_sink.h"
#include "i915_lenovo.h"

extern int vsnprintf(char *s, unsigned long n, const char *format, va_list ap);
extern char *strstr(const char *s, const char *t);
extern int sscanf(const char *s, const char *format, ...);
extern int snprintf(char *s, unsigned long n, const char *format, ...);
extern int open(const char *path, int flags, ...);
extern long read(int fd, void *buf, unsigned long n);
extern int close(int fd);

/* ---- the kernel around the driver ---- */

static char log_text[32768];

/** The boot log, kept whole for the rows that read it, from its start
 * again when full. */
void klog(int level, const char *subsystem, const char *format, ...)
{
    unsigned long n = strlen(log_text);
    va_list ap;

    (void)level;
    (void)subsystem;
    if (n > sizeof(log_text) - 512) {
        n = 0;
    }
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

/** Whether the log holds a wait of the driver that reached its bound. */
static int timed_out(void)
{
    return logged("timed out") || logged("timeout") || logged("no vblank in 100 ms") ||
           logged("not ready");
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

/* ---- the driver's buffer objects: one a size, bound in the aperture by
 * the pin ---- */

#define NBO 9

const struct drm_gem_object_funcs i915_gem_object_funcs;
static struct drm_i915_gem_object bo[NBO];
static struct i915_vma bo_vma[NBO];

/** The GGTT address the pin binds object `i` at, room for a 4K frame. */
#define BO_GGTT(i) (0x01000000u + (uint32_t)(i) * 0x04000000u)

int32_t i915_gem_object_pin_to_display(struct drm_i915_gem_object *obj,
                                       const struct i915_remap_info *remap, uint32_t align,
                                       struct i915_vma **vma)
{
    struct i915_vma *v = &bo_vma[obj - bo];

    (void)remap;
    (void)align;
    if (!obj->vmas) {
        v->obj = obj;
        v->start = BO_GGTT(obj - bo);
        v->mappable = 1;
        obj->vmas = v;
    }
    v->pin_count++;
    *vma = v;
    return 0;
}

void i915_vma_unpin(struct i915_vma *vma)
{
    CHECK(vma->pin_count > 0, "a binding is unpinned no more than it is pinned");
    vma->pin_count--;
}

void i915_gem_object_flush_range(struct drm_i915_gem_object *obj, uint64_t start, uint64_t end)
{
    (void)obj;
    (void)start;
    (void)end;
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
static struct intel_display *disp = &gpu.display;
static struct intel_crtc *pa = &gpu.display.crtc[0];
static struct drm_gem_object *fw_obj;
static uint32_t log_at_init; /* the model's log when the display is made */

/* The Lenovo's HD 4400, a ULT part, as i915_pci.c finds it. */
static const struct i915_id lenovo_id = {LENOVO_GPU_ID, 1, 0, NULL};

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

/** One frame, 16.67 ms: a vblank of each pipe that is on and the
 * messages they raise, then each pipe's CRC done and those. */
static void frame(void)
{
    int pipe;

    model.now_us += 16667u;
    follow_clock(&model);
    for (pipe = 0; pipe < 3; pipe++) {
        i915_model_pipe_vblank(&model, pipe);
    }
    take_messages();
    for (pipe = 0; pipe < 3; pipe++) {
        i915_model_pipe_crc_done(&model, pipe);
    }
    take_messages();
}

/** What runs while a commit or a reader sleeps: frames, until the
 * condition holds, at most a second's. */
static void sleep_frames(wq_cond_fn cond, void *arg)
{
    int i;

    for (i = 0; i < 60 && !cond(arg); i++) {
        frame();
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

/** The latest bytes of the driver's log, for a row that failed. */
static void show_log(void)
{
    unsigned long n = strlen(log_text);

    printf("%s", n > 3000 ? log_text + n - 3000 : log_text);
}

/* The boot's backlight driver, which i915 takes "intel_backlight" from. */
static int boot_level = 937, boot_sets;

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
 * The model with the straps reporting DDI B and C, the device record over
 * it with its interrupts installed and the firmware's surface kept in the
 * GGTT, as the bind leaves them, the boot's backlight registered, and the
 * display made, which adopts pipe A.
 */
static int32_t bring_up(void)
{
    static uint8_t lenovo_vbt[LENOVO_VBT_BYTES];
    long got = 0, r;
    int fd;

    CHECK(i915_model_init(&model) == 0, "the model is made");
    clock_base_ns = model_now_ns - (int64_t)model.now_us * 1000;
    model.on_delay = follow_clock;
    i915_model_poke(&model, SFUSE_STRAP, SFUSE_STRAP_DDIB_DETECTED | SFUSE_STRAP_DDIC_DETECTED);
    /* The Lenovo firmware's level 0 of pipes A and B, read on the machine:
     * primary 120, sprite 56 and cursor 24, a sprite value with no sprite
     * on that no commit's intermediate can keep. */
    i915_model_poke(&model, WM0_PIPE_ILK(0), 0x00783818u);
    i915_model_poke(&model, WM0_PIPE_ILK(1), 0x00783818u);
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
    gpu.id = &lenovo_id;
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
    gpu.ggtt.kept[0].end = LENOVO_FW_FB_BYTES;
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

/* ---- the DRM requests, as crcmodes and IGT make them ---- */

/** Make object `i` of `size` bytes, linear, and give the master a handle
 * to it. */
static uint32_t bo_new(int i, uint64_t size)
{
    uint32_t h = 0;

    k_memset(&bo[i], 0, sizeof(bo[i]));
    k_memset(&bo_vma[i], 0, sizeof(bo_vma[i]));
    bo[i].base.dev = &gpu.drm;
    bo[i].base.size = size;
    bo[i].base.funcs = &i915_gem_object_funcs;
    bo[i].base.resv = &bo[i].base.resv_own;
    bo[i].base.refs = 1;
    bo[i].tiling = I915_TILING_NONE;
    CHECK(drm_gem_handle_create(&fa, &bo[i].base, &h) == 0, "a handle is made");
    bo[i].base.refs--;
    return h;
}

/** A linear XRGB8888 framebuffer of `w` by `h` on object `i`, its rows
 * 64-byte aligned as the dumb buffers of crcmodes and IGT are; its ID, or
 * 0. */
static uint32_t fb_new(int i, uint32_t w, uint32_t h)
{
    struct drm_mode_fb_cmd2 r;
    uint32_t pitch = (w * 4u + 63u) & ~63u;

    k_memset(&r, 0, sizeof(r));
    r.width = w;
    r.height = h;
    r.pixel_format = DRM_FORMAT_XRGB8888;
    r.handles[0] = bo_new(i, (uint64_t)pitch * h);
    r.pitches[0] = pitch;
    return drm_mode_addfb2_ioctl(&gpu.drm, &r, &fa) == 0 ? r.fb_id : 0;
}

/** GETCONNECTOR of `connector`, which probes it; its status, and its mode
 * of `w` by `h` at `hz` into `out` when `out` is not NULL, or -1 when it
 * has none. */
static int probe(struct drm_connector *connector, uint32_t w, uint32_t h, uint32_t hz,
                 struct drm_mode_modeinfo *out)
{
    struct drm_mode_get_connector gc;
    struct drm_mode_modeinfo modes[32];
    uint32_t i;

    k_memset(&gc, 0, sizeof(gc));
    gc.connector_id = connector->base.id;
    if (drm_mode_getconnector(&gpu.drm, &gc, &fa) != 0) {
        return -1;
    }
    gc.modes_ptr = UPTR(modes);
    gc.count_modes = 32;
    gc.count_props = 0;
    gc.count_encoders = 0;
    if (drm_mode_getconnector(&gpu.drm, &gc, &fa) != 0) {
        return -1;
    }
    if (out == NULL) {
        return (int)gc.connection;
    }
    for (i = 0; i < gc.count_modes && i < 32; i++) {
        if (modes[i].hdisplay == w && modes[i].vdisplay == h && modes[i].vrefresh == hz) {
            *out = modes[i];
            return (int)gc.connection;
        }
    }
    return -1;
}

/** SETCRTC of pipe `pipe` with `mode` and `fb_id` on `connector`, or off
 * when `connector` is NULL. */
static int32_t setcrtc(int pipe, struct drm_connector *connector,
                       const struct drm_mode_modeinfo *mode, uint32_t fb_id)
{
    struct drm_mode_crtc c;
    uint32_t conn = connector ? connector->base.id : 0;

    k_memset(&c, 0, sizeof(c));
    c.crtc_id = disp->crtc[pipe].base.base.id;
    if (connector) {
        c.fb_id = fb_id;
        c.mode_valid = 1;
        c.mode = *mode;
        c.set_connectors_ptr = UPTR(&conn);
        c.count_connectors = 1;
    }
    return drm_mode_setcrtc(&gpu.drm, &c, &fa);
}

/** The legacy SETPROPERTY of the panel's "scaling mode", as crcmodes sets
 * it. */
static int32_t set_scaling(uint64_t mode)
{
    struct drm_mode_connector_set_property s;

    k_memset(&s, 0, sizeof(s));
    s.connector_id = disp->connector.base.id;
    s.prop_id = disp->connector.scaling_mode_property->base.id;
    s.value = mode;
    return drm_connector_property_set_ioctl(&gpu.drm, &s, &fa);
}

/** crc/data's next line as its frame and first value. */
static int crc_line(uint32_t *frame_no, uint32_t *crc)
{
    char buf[96];

    if (drm_crtc_crc_read(&pa->base, buf, sizeof(buf), 1) <= 0) {
        return 0;
    }
    return sscanf(buf, "0x%x 0x%x", frame_no, crc) == 2;
}

/* ---- the bind ---- */

/**
 * The bind's adoption: no register of the pipes, planes, transcoders, DDI
 * buffers, clocks or backlight written; the watermarks computed and
 * written for the adopted state, as Linux's ilk_wm_sanitize does; pipe A
 * on in the atomic state with the firmware's framebuffer; the outputs
 * probed, the panel connected and HDMI-A-1, DP-1 and HDMI-A-2 not, with no
 * hotplug event, and the hotplug pins of B and C armed.
 */
static void row_adopt(void)
{
    struct drm_plane_state *ps = pa->primary.state;

    CHECK(!wrote_range(0x60000u, 0x6400fu, log_at_init) &&
              !wrote_range(0x64028u, 0x7ffffu, log_at_init) &&
              !wrote_range(0x46100u, 0x46114u, log_at_init) &&
              !wrote_range(0xc7200u, 0xc7203u, log_at_init) &&
              !wrote_range(0xc8250u, 0xc8258u, log_at_init) &&
              !wrote_range(0x48250u, 0x48254u, log_at_init),
          "adopt: nothing of the pipe, its link or the backlight written, so no blank");
    CHECK(
        ((i915_read(&gpu.hw, WM0_PIPE_ILK(0)) >> WM0_PIPE_SPRITE_SHIFT) & 0xffu) == 0 &&
            i915_read(&gpu.hw, WM0_PIPE_ILK(0)) >> WM0_PIPE_PRIMARY_SHIFT != 0 &&
            i915_read(&gpu.hw, WM0_PIPE_ILK(1)) == 0,
        "adopt: the watermarks computed for the adopted planes, the firmware's sprite value gone");
    CHECK(pa->base.state->active && ps->fb && intel_fb_ggtt(ps->fb) == 0 &&
              logged("display: pipe A adopted: 1366x768 in 1366x768@60"),
          "adopt: pipe A on with the firmware's framebuffer, and the log's line");
    fw_obj = ps->fb->obj[0];
    model_hotplugs = 0;
    CHECK(probe(&disp->connector, 0, 0, 0, NULL) == DRM_CONNECTOR_CONNECTED &&
              probe(&disp->ddi[0].hdmi_connector, 0, 0, 0, NULL) == DRM_CONNECTOR_DISCONNECTED &&
              probe(&disp->ddi[1].dp_connector, 0, 0, 0, NULL) == DRM_CONNECTOR_DISCONNECTED &&
              probe(&disp->ddi[1].hdmi_connector, 0, 0, 0, NULL) == DRM_CONNECTOR_DISCONNECTED &&
              model_hotplugs == 0,
          "adopt: eDP-1 connected, HDMI-A-1, DP-1 and HDMI-A-2 not, no hotplug event");
    CHECK((reg(PCH_PORT_HOTPLUG) &
           (PORT_HOTPLUG_ENABLE(HPD_PORT_B) | PORT_HOTPLUG_ENABLE(HPD_PORT_C))) ==
                  (PORT_HOTPLUG_ENABLE(HPD_PORT_B) | PORT_HOTPLUG_ENABLE(HPD_PORT_C)) &&
              !(reg(SDEIMR) & (SDE_PORTB_HOTPLUG_CPT | SDE_PORTC_HOTPLUG_CPT)),
          "adopt: the hotplug pins of B and C armed and unmasked");
    CHECK(pa->underrun_off && (reg(DEIMR) & DE_ERR_INT_IVB),
          "adopt: pipe A's underruns unreported until its first commit, as Linux's");
    CHECK(!timed_out(), "adopt: no wait at its bound");
}

/** GET_PIPE_FROM_CRTC_ID, by which IGT's igt_display_require maps each CRTC
 * to its pipe when the CRTC's i915_pipe cannot be read: each pipe's own,
 * and ENOENT for an ID that is no CRTC. */
static void row_pipe_ids(void)
{
    struct drm_i915_get_pipe_from_crtc_id g;
    int pipe, right = 0;

    for (pipe = 0; pipe < I915_NUM_PIPES; pipe++) {
        g.crtc_id = disp->crtc[pipe].base.base.id;
        g.pipe = 99;
        right +=
            intel_get_pipe_from_crtc_id_ioctl(&gpu.drm, &g, &fa) == 0 && g.pipe == (uint32_t)pipe;
    }
    g.crtc_id = disp->connector.base.id;
    CHECK(right == I915_NUM_PIPES &&
              intel_get_pipe_from_crtc_id_ioctl(&gpu.drm, &g, &fa) == -ENOENT,
          "igt: GET_PIPE_FROM_CRTC_ID gives each CRTC's pipe, ENOENT for another object");
}

/* ---- the desktop ---- */

/* One commit's properties, as a program lists them for ATOMIC. */
struct atomic_req {
    uint32_t objs[16], counts[16], props[48];
    uint64_t values[48];
    uint32_t nobjs, nprops;
};

/** Add `prop` of `value` to object `id` of `r`, after its others. */
static void atomic_add(struct atomic_req *r, uint32_t id, const struct drm_property *prop,
                       uint64_t value)
{
    if (r->nobjs == 0 || r->objs[r->nobjs - 1] != id) {
        r->objs[r->nobjs] = id;
        r->counts[r->nobjs++] = 0;
    }
    r->counts[r->nobjs - 1]++;
    r->props[r->nprops] = prop->base.id;
    r->values[r->nprops++] = value;
}

/**
 * Weston's first commit on the adopted pipe, as the trial root's desktop
 * made it at boot (its drm-backend log): the outputs it does not drive
 * and every plane off, then pipe A at the panel's mode with the eDP
 * connector at "max bpc" 12 and the primary plane showing a framebuffer
 * of its own, allowed to set the mode. Linux takes it.
 */
static void row_desktop(void)
{
    struct drm_mode_config *mc = &gpu.drm.mode_config;
    struct drm_connector *off[3];
    struct drm_property_blob *blob;
    struct drm_mode_modeinfo mode;
    struct drm_mode_atomic a;
    struct atomic_req r;
    uint32_t fb = fb_new(8, 1366, 768), i;
    int32_t err;

    off[0] = &disp->ddi[0].hdmi_connector;
    off[1] = &disp->ddi[1].dp_connector;
    off[2] = &disp->ddi[1].hdmi_connector;
    CHECK(fb != 0 && probe(&disp->connector, 1366, 768, 60, &mode) >= 0,
          "desktop: a framebuffer and the panel's mode");
    blob = drm_property_create_blob(&gpu.drm, sizeof(mode), &mode, &err);
    CHECK(blob != NULL, "desktop: the mode's blob");
    if (fb == 0 || blob == NULL) {
        return;
    }
    k_memset(&r, 0, sizeof(r));
    for (i = 0; i < 3; i++) {
        atomic_add(&r, off[i]->base.id, mc->prop_crtc_id, 0);
    }
    for (i = 0; i < mc->num_plane; i++) {
        atomic_add(&r, mc->planes[i]->base.id, mc->prop_crtc_id, 0);
        atomic_add(&r, mc->planes[i]->base.id, mc->prop_fb_id, 0);
    }
    atomic_add(&r, pa->base.base.id, mc->prop_mode_id, blob->base.id);
    atomic_add(&r, pa->base.base.id, mc->prop_active, 1);
    atomic_add(&r, pa->base.base.id, mc->prop_vrr_enabled, 0);
    atomic_add(&r, disp->connector.base.id, mc->prop_crtc_id, pa->base.base.id);
    atomic_add(&r, disp->connector.base.id, disp->connector.max_bpc_property, 12);
    atomic_add(&r, pa->primary.base.id, mc->prop_fb_id, fb);
    atomic_add(&r, pa->primary.base.id, mc->prop_crtc_id, pa->base.base.id);
    atomic_add(&r, pa->primary.base.id, mc->prop_src_x, 0);
    atomic_add(&r, pa->primary.base.id, mc->prop_src_y, 0);
    atomic_add(&r, pa->primary.base.id, mc->prop_src_w, (uint64_t)1366 << 16);
    atomic_add(&r, pa->primary.base.id, mc->prop_src_h, (uint64_t)768 << 16);
    atomic_add(&r, pa->primary.base.id, mc->prop_crtc_x, 0);
    atomic_add(&r, pa->primary.base.id, mc->prop_crtc_y, 0);
    atomic_add(&r, pa->primary.base.id, mc->prop_crtc_w, 1366);
    atomic_add(&r, pa->primary.base.id, mc->prop_crtc_h, 768);
    atomic_add(&r, pa->primary.base.id, pa->primary.rotation_property, DRM_MODE_ROTATE_0);
    k_memset(&a, 0, sizeof(a));
    a.flags = DRM_MODE_ATOMIC_ALLOW_MODESET | DRM_MODE_ATOMIC_NONBLOCK | DRM_MODE_PAGE_FLIP_EVENT;
    a.count_objs = r.nobjs;
    a.objs_ptr = UPTR(r.objs);
    a.count_props_ptr = UPTR(r.counts);
    a.props_ptr = UPTR(r.props);
    a.prop_values_ptr = UPTR(r.values);
    err = drm_mode_atomic_ioctl(&gpu.drm, &a, &fa);
    if (err != 0) {
        printf("i915-kms-trial-test: desktop: ATOMIC: error %d\n", (int)err);
    }
    CHECK(err == 0 && pa->base.state->active && pa->primary.state->fb &&
              pa->primary.state->fb->base.id == fb,
          "desktop: weston's first commit on the adopted pipe taken, as Linux takes it");
    CHECK(!timed_out(), "desktop: no wait at its bound");
}

/* ---- crcmodes ---- */

/* A mode of crcmodes' list: its size, and the scaling mode it is set
 * under. */
struct trial_mode {
    uint32_t w, h;
    int bo;
    uint64_t scale;
};

static const struct trial_mode trial_modes[] = {
    {1366, 768, 0, DRM_MODE_SCALE_ASPECT},     {1280, 720, 1, DRM_MODE_SCALE_ASPECT},
    {1280, 720, 1, DRM_MODE_SCALE_FULLSCREEN}, {1280, 720, 1, DRM_MODE_SCALE_CENTER},
    {1024, 768, 2, DRM_MODE_SCALE_ASPECT},     {1024, 768, 2, DRM_MODE_SCALE_FULLSCREEN},
    {1024, 768, 2, DRM_MODE_SCALE_CENTER},     {800, 600, 3, DRM_MODE_SCALE_ASPECT},
    {800, 600, 3, DRM_MODE_SCALE_FULLSCREEN},  {800, 600, 3, DRM_MODE_SCALE_CENTER},
    {640, 480, 4, DRM_MODE_SCALE_ASPECT},      {640, 480, 4, DRM_MODE_SCALE_FULLSCREEN},
    {640, 480, 4, DRM_MODE_SCALE_CENTER}};

/* A fitter window: its corner and size on the panel. */
struct window {
    uint32_t x, y, w, h;
};

/**
 * The fitter's window for a source of `sw` by `sh` on the 1366x768 panel
 * under `scale`, as Linux 6.12's pch_panel_fitting computes it, written out
 * here apart from the driver's.
 */
static struct window linux_window(uint32_t sw, uint32_t sh, uint64_t scale)
{
    const uint32_t pw = 1366, ph = 768;
    struct window r = {0, 0, 1366, 768};

    if (scale == DRM_MODE_SCALE_CENTER) {
        r.w = sw;
        r.h = sh;
        r.x = (pw - sw + 1) / 2;
        r.y = (ph - sh + 1) / 2;
    } else if (scale == DRM_MODE_SCALE_ASPECT) {
        uint32_t scaled_width = pw * sh, scaled_height = sw * ph;

        if (scaled_width > scaled_height) {
            r.w = scaled_height / sh + (scaled_height / sh & 1u);
            r.x = (pw - r.w + 1) / 2;
        } else if (scaled_width < scaled_height) {
            r.h = scaled_width / sw + (scaled_width / sw & 1u);
            r.y = (ph - r.h + 1) / 2;
        }
    }
    return r;
}

/** Set mode `t` as crcmodes does, on framebuffer `fb`: 1 when pipe A
 * shows it at its size. */
static int set_trial_mode(const struct trial_mode *t, uint32_t fb)
{
    struct drm_mode_modeinfo m = pa->hw.mode;

    m.hdisplay = (uint16_t)t->w;
    m.vdisplay = (uint16_t)t->h;
    m.type = 0;
    drm_mode_set_name(&m);
    return fb != 0 && set_scaling(t->scale) == 0 && setcrtc(0, &disp->connector, &m, fb) == 0 &&
           reg(PIPESRC(0)) == ((t->w - 1u) << 16 | (t->h - 1u)) &&
           (reg(TRANSCONF(I915_TRANS_EDP)) & TRANSCONF_ENABLE);
}

/** Whether the fitter of pipe A is as Linux sets it for mode `t`: off for
 * the panel's own size, else on at Linux's window. */
static int fitted_as_linux(const struct trial_mode *t)
{
    struct window r;

    if (t->w == 1366 && t->h == 768) {
        return reg(PF_CTL(0)) == 0;
    }
    r = linux_window(t->w, t->h, t->scale);
    if (reg(PF_CTL(0)) == (PF_ENABLE | PF_FILTER_MED_3x3) &&
        reg(PF_WIN_POS(0)) == (r.x << 16 | r.y) && reg(PF_WIN_SZ(0)) == (r.w << 16 | r.h)) {
        return 1;
    }
    printf("mode %ux%u scale %u: window %08x %08x, Linux's %08x %08x\n", (unsigned)t->w,
           (unsigned)t->h, (unsigned)t->scale, (unsigned)reg(PF_WIN_POS(0)),
           (unsigned)reg(PF_WIN_SZ(0)), (unsigned)(r.x << 16 | r.y), (unsigned)(r.w << 16 | r.h));
    return 0;
}

/** The CRC of "auto" on pipe A over four frames, the first dropped: 1 when
 * three lines came, each the frame's count and the chip's CRC; the source
 * released after. */
static int crcs_as_chip(void)
{
    const uint32_t want_ctl = PIPE_CRC_ENABLE | PIPE_CRC_SOURCE_PF_IVB;
    uint32_t fr, crc;
    int k, lines = 0;

    if (drm_crtc_crc_set_source(&pa->base, "auto", 4) != 4 || drm_crtc_crc_open(&pa->base) != 0 ||
        reg(PIPE_CRC_CTL(0)) != want_ctl) {
        drm_crtc_crc_release(&pa->base);
        return 0;
    }
    for (k = 0; k < 4; k++) {
        frame();
        fr = crc = 0;
        if (crc_line(&fr, &crc) && fr == (uint32_t)pa->base.vblank.count &&
            crc == i915_model_pipe_crc(&model, 0, want_ctl)) {
            lines++;
        }
    }
    drm_crtc_crc_release(&pa->base);
    return lines == 3;
}

/**
 * crcmodes' list: each mode set by SETPROPERTY of its scaling mode and
 * SETCRTC with the panel's timings at its size; the panel's own mode with
 * the fitter off, a smaller one through the fitter at Linux's window; the
 * pipe CRC of "auto", its first frame dropped, each line the chip's CRC of
 * the fitter's output; the source released after each.
 */
static void row_modes(void)
{
    uint32_t fbs[5] = {0, 0, 0, 0, 0}, i, bad_set = 0, bad_fit = 0, bad_crc = 0;

    for (i = 0; i < sizeof(trial_modes) / sizeof(trial_modes[0]); i++) {
        const struct trial_mode *t = &trial_modes[i];

        if (fbs[t->bo] == 0) {
            fbs[t->bo] = fb_new(t->bo, t->w, t->h);
        }
        if (!set_trial_mode(t, fbs[t->bo])) {
            printf("mode %ux%u scale %u: not set\n", (unsigned)t->w, (unsigned)t->h,
                   (unsigned)t->scale);
            bad_set++;
            continue;
        }
        bad_fit += !fitted_as_linux(t);
        frame();
        if (!crcs_as_chip()) {
            printf("mode %ux%u scale %u: not three CRC lines\n", (unsigned)t->w, (unsigned)t->h,
                   (unsigned)t->scale);
            bad_crc++;
        }
        frame();
    }
    CHECK(bad_set == 0, "modes: each of the 13 set at its size on the panel's timings");
    CHECK(bad_fit == 0, "modes: the panel's own without the fitter, each other at Linux's window");
    CHECK(bad_crc == 0, "modes: three CRCs of auto a mode, the chip's, its first frame dropped");
    CHECK(set_scaling(DRM_MODE_SCALE_ASPECT) == 0 &&
              setcrtc(0, &disp->connector, &pa->hw.mode, fbs[0]) == 0 && reg(PF_CTL(0)) == 0 &&
              (reg(TRANS_DDI_FUNC_CTL(I915_TRANS_EDP)) & TRANS_DDI_EDP_INPUT_MASK) ==
                  TRANS_DDI_EDP_INPUT_A_ON,
          "modes: the panel's own mode again, past the fitter");
    CHECK((reg(PIPE_CSC_MODE(0)) & CSC_POSITION_BEFORE_GAMMA) && reg(GAMMA_MODE(0)) == 0,
          "modes: full range, the matrix before the 8-bit gamma, as Linux's ivb_csc_mode");
    CHECK(!timed_out(), "modes: no wait at its bound");
    frame();
}

/**
 * IGT's kms_pipe_crc_basic read-crc-frame-sequence: "auto" started on pipe
 * A while it shows its frame; the frame the source started in, whose CRC
 * the chip computes over part of it, dropped; then ten lines, each the
 * chip's CRC of the frame and each frame one past the last, the CRC read
 * at its CRC done, which comes after the vblank.
 */
static void row_read_crc(void)
{
    const uint32_t want_ctl = PIPE_CRC_ENABLE | PIPE_CRC_SOURCE_PF_IVB;
    uint32_t fr = 0, crc = 0, last = 0;
    int k, good = 0;

    CHECK(drm_crtc_crc_set_source(&pa->base, "auto", 4) == 4 && drm_crtc_crc_open(&pa->base) == 0,
          "read-crc: auto on pipe A");
    frame();
    CHECK(!drm_crtc_crc_ready(&pa->base), "read-crc: the frame the source started in dropped");
    for (k = 0; k < 10; k++) {
        frame();
        if (crc_line(&fr, &crc) && crc == i915_model_pipe_crc(&model, 0, want_ctl) &&
            (k == 0 || fr == last + 1u)) {
            good++;
        }
        last = fr;
    }
    drm_crtc_crc_release(&pa->base);
    CHECK(good == 10, "read-crc: ten lines, each the frame's CRC, each frame one past the last");
    frame();
}

/* ---- FIFO underruns ---- */

/** How many times the log holds `text`. */
static int times_logged(const char *text)
{
    const char *p = log_text;
    int n = 0;

    while ((p = strstr(p, text)) != 0) {
        n++;
        p++;
    }
    return n;
}

/** An underrun of pipe A's FIFO, as the chip latches and signals it. */
static void underrun_a(void)
{
    i915_model_poke(&model, GEN7_ERR_INT,
                    i915_model_peek(&model, GEN7_ERR_INT) | ERR_INT_FIFO_UNDERRUN(0));
    i915_model_irq_event(&model, I915_IRQ_DE, DE_ERR_INT_IVB);
    take_messages();
    frame();
}

/** Underrun reporting through the error interrupt, as Linux's
 * intel_fifo_underrun.c does it on Haswell. */
static void row_underrun(void)
{
    const char *line = "display: CPU pipe A FIFO underrun";
    uint32_t fb = fb_new(7, 1366, 768);

    CHECK(!pa->underrun_off && !(reg(DEIMR) & DE_ERR_INT_IVB) && times_logged(line) == 0,
          "underrun: reported after the modes' commits, the error interrupt unmasked");
    underrun_a();
    CHECK(times_logged(line) == 1 && pa->underrun_off && (reg(DEIMR) & DE_ERR_INT_IVB) &&
              !(reg(GEN7_ERR_INT) & ERR_INT_FIFO_UNDERRUN(0)),
          "underrun: logged once, then unreported and masked, GEN7_ERR_INT cleared");
    i915_model_poke(&model, GEN7_ERR_INT, ERR_INT_FIFO_UNDERRUN(0));
    frame();
    CHECK(times_logged(line) == 1, "underrun: a second one not logged");
    CHECK(setcrtc(0, NULL, NULL, 0) == 0 && setcrtc(0, &disp->connector, &pa->hw.mode, fb) == 0 &&
              !pa->underrun_off && !(reg(DEIMR) & DE_ERR_INT_IVB) &&
              !(reg(GEN7_ERR_INT) & ERR_INT_FIFO_UNDERRUN(0)),
          "underrun: the pipe off and on again clears the latch and reports again");
    CHECK(!timed_out(), "underrun: no wait at its bound");
    frame();
}

/* ---- the backlight ---- */

/** Linux's scale_user_to_hw for the Lenovo's PWM: `level` of 937 onto 18
 * to 937, rounded to the closest. */
static uint32_t linux_hw_level(uint32_t level)
{
    return 18u + (level * 919u + 937u / 2u) / 937u;
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

    if (backlight_show(0, attr, buf, sizeof(buf)) <= 0) {
        return -1;
    }
    (void)sscanf(buf, "%d", &v);
    return v;
}

/** The step's backlight levels: 1, 468 and 937 at Linux's duty cycles,
 * read back as set; 0 dark by the sequencer's enable; and 937 again. */
static void row_backlight(void)
{
    static const uint32_t levels[3] = {1, 468, 937};
    char text[8];
    int i, bad = 0;

    CHECK(disp->bl_min == 18 && bl_show(BACKLIGHT_LEVELS) == 937 && boot_sets == 0 &&
              bl_show(BACKLIGHT_ACTUAL) == 937,
          "backlight: intel_backlight taken over at 937 of 937, the PWM from 18");
    for (i = 0; i < 3; i++) {
        (void)snprintf(text, sizeof(text), "%u", (unsigned)levels[i]);
        if (bl_store(BACKLIGHT_BRIGHTNESS, text) != 0 ||
            (reg(BLC_PWM_PCH_CTL2) & 0xffffu) != linux_hw_level(levels[i]) ||
            !(reg(PP_CONTROL) & EDP_BLC_ENABLE) || bl_show(BACKLIGHT_ACTUAL) != (int)levels[i]) {
            printf("backlight %u: duty %u, Linux's %u, actual %d\n", (unsigned)levels[i],
                   (unsigned)(reg(BLC_PWM_PCH_CTL2) & 0xffffu), (unsigned)linux_hw_level(levels[i]),
                   bl_show(BACKLIGHT_ACTUAL));
            bad++;
        }
    }
    CHECK(bad == 0, "backlight: 1, 468 and 937 at Linux's duty cycles 19, 477 and 937");
    CHECK(bl_store(BACKLIGHT_BRIGHTNESS, "0") == 0 && !(reg(PP_CONTROL) & EDP_BLC_ENABLE) &&
              (reg(BLC_PWM_PCH_CTL2) & 0xffffu) == 18 && bl_show(BACKLIGHT_ACTUAL) == 0,
          "backlight: 0 turns the enable off at the least duty cycle, as Linux");
    CHECK(bl_store(BACKLIGHT_BRIGHTNESS, "937") == 0 && (reg(PP_CONTROL) & EDP_BLC_ENABLE) &&
              (reg(BLC_PWM_PCH_CTL2) & 0xffffu) == 937 && boot_sets == 0,
          "backlight: 937 lights it again at full duty");
    frame();
}

/* ---- IGT's kms_hdmi_inject ---- */

/** Pack the 3840x2160@30 timing into the detailed descriptor at `o`, with
 * positive syncs. */
static void put_dtd_2160p30(uint8_t *o)
{
    const uint32_t clock = 297000, ha = 3840, hb = 560, hfp = 176, hs = 88;
    const uint32_t va = 2160, vb = 90, vfp = 8, vs = 10;

    o[0] = (uint8_t)(clock / 10u);
    o[1] = (uint8_t)(clock / 10u >> 8);
    o[2] = (uint8_t)ha;
    o[3] = (uint8_t)hb;
    o[4] = (uint8_t)((ha >> 8) << 4 | (hb >> 8));
    o[5] = (uint8_t)va;
    o[6] = (uint8_t)vb;
    o[7] = (uint8_t)((va >> 8) << 4 | (vb >> 8));
    o[8] = (uint8_t)hfp;
    o[9] = (uint8_t)hs;
    o[10] = (uint8_t)((vfp & 15u) << 4 | (vs & 15u));
    o[11] = (uint8_t)((hfp >> 8) << 6 | (hs >> 8) << 4 | (vfp >> 4) << 2 | (vs >> 4));
    o[12] = 0x13;
    o[13] = 0x2b;
    o[14] = 0x21;
    o[17] = 0x1e;
}

/** Make the 128-byte checksum of the block at `b` come right. */
static void sum_block(uint8_t *b)
{
    uint8_t s = 0;
    int i;

    for (i = 0; i < 127; i++) {
        s = (uint8_t)(s + b[i]);
    }
    b[127] = (uint8_t)(0u - s);
}

/** IGT's 4K EDID in `e`, as kms_hdmi_inject gives it: version 1.3, a
 * digital input, the preferred 3840x2160@30, and a CEA extension with VIC
 * 16 and HDMI's VSDB at 300 MHz naming HDMI VIC 1. Its length, 256. */
static uint32_t edid_4k(uint8_t *e)
{
    static const uint8_t head[8] = {0, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0};
    static const uint8_t cea[] = {0x41, 0x10, 0x6b, 0x03, 0x0c, 0x00, 0x10,
                                  0x00, 0x00, 0x3c, 0x20, 0x00, 0x20, 0x01};
    int i;

    k_memset(e, 0, 256);
    k_memcpy(e, head, 8);
    e[8] = 0x48;
    e[9] = 0x68;
    e[10] = 1;
    e[17] = 30;
    e[18] = 1;
    e[19] = 3;
    e[20] = 0x80;
    e[21] = 60;
    e[22] = 34;
    e[23] = 0x78;
    e[24] = 0x0a;
    for (i = 38; i < 54; i++) {
        e[i] = 1;
    }
    put_dtd_2160p30(e + 54);
    e[75] = 0x10;
    e[93] = 0x10;
    e[111] = 0x10;
    e[126] = 1;
    sum_block(e);
    e[128] = 0x02;
    e[129] = 0x03;
    e[130] = (uint8_t)(4u + sizeof(cea));
    k_memcpy(e + 132, cea, sizeof(cea));
    sum_block(e + 128);
    return 256;
}

/**
 * kms_hdmi_inject's inject-4k as it runs on the Lenovo: every CRTC off, so
 * the panel's pipe too; HDMI-A-1, with no monitor, forced on with the 4K
 * EDID override and found connected with its mode; 3840x2160@30 set on
 * pipe A, the first CRTC, through transcoder A and DDI B from WRPLL 1 at
 * Linux's 297 MHz, limited range by the matrix after the gamma, and no
 * second M/N on transcoder A; then the pipe off, the connector given back,
 * and the panel on pipe A again.
 */
static void row_inject(void)
{
    struct drm_connector *hdmi = &disp->ddi[0].hdmi_connector;
    static uint8_t edid[256];
    struct drm_mode_modeinfo m;
    uint32_t fb_4k, from, func, len = edid_4k(edid);

    CHECK(setcrtc(0, NULL, NULL, 0) == 0 && !(reg(TRANSCONF(I915_TRANS_EDP)) & TRANSCONF_ENABLE) &&
              !(reg(PP_CONTROL) & PANEL_POWER_ON),
          "inject: the panel's pipe off, its panel unpowered");
    CHECK(drm_edid_override_set(hdmi, edid, len) == 0 &&
              drm_connector_force_write(hdmi, "on\n", 3) == 3,
          "inject: the EDID override set and HDMI-A-1 forced on");
    CHECK(probe(hdmi, 3840, 2160, 30, &m) == DRM_CONNECTOR_CONNECTED &&
              (m.type & DRM_MODE_TYPE_PREFERRED) && disp->ddi[0].hdmi.info.is_hdmi,
          "inject: HDMI-A-1 connected, an HDMI sink, 3840x2160@30 preferred");
    fb_4k = fb_new(5, 3840, 2160);
    from = model.nlog;
    CHECK(fb_4k && setcrtc(0, hdmi, &m, fb_4k) == 0, "inject: SETCRTC of 3840x2160@30 on pipe A");
    func = reg(TRANS_DDI_FUNC_CTL(0));
    CHECK((func & TRANS_DDI_FUNC_ENABLE) &&
              (func & TRANS_DDI_PORT_MASK) == TRANS_DDI_SELECT_PORT(INTEL_PORT_B) &&
              (func & TRANS_DDI_MODE_SELECT_MASK) == TRANS_DDI_MODE_SELECT_HDMI &&
              (reg(TRANSCONF(0)) & TRANSCONF_ENABLE) &&
              (reg(DDI_BUF_CTL(INTEL_PORT_B)) & DDI_BUF_CTL_ENABLE),
          "inject: transcoder A in HDMI mode on DDI B, its buffer on");
    CHECK(reg(WRPLL_CTL(0)) == (WRPLL_PLL_ENABLE | WRPLL_REF_LCPLL | WRPLL_DIVIDER_REFERENCE(20) |
                                WRPLL_DIVIDER_FEEDBACK(22) | WRPLL_DIVIDER_POST(2)) &&
              reg(PORT_CLK_SEL(INTEL_PORT_B)) == PORT_CLK_SEL_WRPLL1,
          "inject: WRPLL 1 at Linux's 297 MHz feeds DDI B");
    CHECK(reg(PIPE_CSC_MODE(0)) == 0 && reg(PIPE_CSC_POSTOFF_HI(0)) == 0x100 &&
              reg(PIPE_CSC_COEFF_RY_GY(0)) == ((235u - 16u) << 4) << 16,
          "inject: limited range by the matrix after the gamma, as Linux's ivb_csc_mode");
    CHECK(!wrote_range(PIPE_DATA_M2(0), PIPE_LINK_N2(0), from),
          "inject: no second M/N written on transcoder A");
    CHECK((reg(HSW_TVIDEO_DIP_CTL(0)) & VIDEO_DIP_ENABLE_VS_HSW) && gpu.power.count == 2,
          "inject: the HDMI VIC in the vendor infoframe, the power well held");
    frame();
    frame();
    CHECK(setcrtc(0, NULL, NULL, 0) == 0 && !(reg(WRPLL_CTL(0)) & WRPLL_PLL_ENABLE) &&
              !(reg(DDI_BUF_CTL(INTEL_PORT_B)) & DDI_BUF_CTL_ENABLE) && gpu.power.count == 1,
          "inject: pipe A off, DDI B and WRPLL 1 off, the well let go");
    drm_edid_override_reset(hdmi);
    CHECK(drm_connector_force_write(hdmi, "unspecified", 11) == 11 &&
              probe(hdmi, 0, 0, 0, NULL) == DRM_CONNECTOR_DISCONNECTED,
          "inject: HDMI-A-1 given back, disconnected");
    CHECK(!timed_out(), "inject: no wait at its bound");
    frame();
}

/**
 * The panel back on pipe A with the console's buffer, as the console takes
 * the screen when the last master closes; then ten seconds of frames with
 * the hotplug pins armed and no monitor: no pulse, no event, no probe.
 */
static void row_quiet(void)
{
    uint32_t fb = fb_new(6, 1366, 768);
    int i;

    CHECK(fb && setcrtc(0, &disp->connector, &pa->hw.mode, fb) == 0 &&
              (reg(TRANSCONF(I915_TRANS_EDP)) & TRANSCONF_ENABLE) &&
              (reg(PP_CONTROL) & PANEL_POWER_ON) && (reg(PP_CONTROL) & EDP_BLC_ENABLE),
          "quiet: the panel on pipe A again, lit");
    model_hotplugs = 0;
    for (i = 0; i < 600; i++) {
        frame();
    }
    (void)drm_atomic_commit_work();
    CHECK(model_hotplugs == 0 && disp->hotplug.pulses == 0 && !timed_out(),
          "quiet: ten seconds with no hotplug pulse or event, no wait at its bound");
}

/** The display let go: every scanout unpinned. */
static void row_fini(void)
{
    int i, pinned = 0;

    intel_display_fini(&gpu);
    drm_mode_file_release(&fa);
    drm_mode_config_cleanup(&gpu.drm);
    for (i = 0; i < NBO; i++) {
        pinned += bo_vma[i].pin_count;
    }
    CHECK(pinned == 0, "fini: every scanout unpinned");
}

/* ---- RPS's waitboost and interactive mode, which the driver calls ---- */

/* The requests' fences, i915_gem.c's, which no flip here waits on. */
const struct dma_fence_ops i915_fence_ops = {0, 0, 0, 0, 0, 0, 0};

/** Whether the chip has begun `rq`; no request runs here, so only once
 * it has signalled. */
int i915_request_started(const struct i915_request *rq)
{
    return rq->fence.base.signaled;
}

/** The waitboost, RPS's, which no row here measures. */
void i915_rps_boost(struct i915_request *rq)
{
    (void)rq;
}

/** A page flip pending or done, which RPS counts; no row here reads it. */
void i915_rps_mark_interactive(struct drm_i915_private *i915, int on)
{
    (void)i915;
    (void)on;
}

int main(void)
{
    model_start("i915-kms-trial-test");
    CHECK(bring_up() == 0, "the display is made");
    row_adopt();
    row_pipe_ids();
    row_desktop();
    row_modes();
    row_read_crc();
    row_underrun();
    row_backlight();
    row_inject();
    row_quiet();
    row_fini();
    if (failures != 0) {
        show_log();
    }
    if (fw_obj && fw_obj->refs == 0) {
        fw_obj->funcs->free(fw_obj);
    }
    i915_model_free(&model);
    intel_bios_fini(&gpu.vbt);
    if (failures == 0) {
        printf("i915-kms-trial-test: ok\n");
    }
    return failures != 0;
}

/** Nothing to wait for: the model's handler runs only when a row calls it. */
void i915_irq_sync(struct drm_i915_private *i915)
{
    (void)i915;
}
