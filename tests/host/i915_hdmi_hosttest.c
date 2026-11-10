/*
 * i915_hdmi_hosttest.c: the Intel graphics driver's external monitors on
 * Haswell, DDI B to D, driven through the DRM core against the host model
 * of the Lenovo's chip with its straps reporting DDI B and C.
 *
 * host-deps: rach2/sys/drm/i915/i915_irq.c rach2/sys/drm/i915/i915_hw.c
 * host-deps: tests/host/i915_model.c rach2/sys/display/backlight.c
 * host-deps: rach2/sys/display/intelbl.c rach2/sys/lib/kstring.c
 * host-deps: rach2/sys/lib/kctypes.c tests/host/compat/spinlock_host.c
 * host-deps: rach2/sys/drm/i915/intel_bios.c rach2/sys/drm/i915/i915_power.c
 * host-inputs: tests/fixtures/lenovo-gpu/i915_vbt.bin
 *
 * The rows: the ports the VBT and the straps give, HDMI on B and DP++ on
 * C; an EDID read over GMBUS from the model's DDC, and again by
 * bit-banging the GPIO pins once a GMBUS transfer hangs (78-10c.1); the
 * WRPLL's dividers for a table of clocks, Linux's hsw_ddi_calculate_wrpll
 * run on the same clocks, and the SPLL's value (78-10c.3); a monitor
 * plugged into B, found by its long pulse, and set at 1920x1080 on pipe B
 * with Linux's WRPLL, the VBT's buffer translation, the AVI and SPD
 * infoframes, limited range, the second pipe's power well and its
 * watermarks (78-10c.2); a DisplayPort monitor on C trained
 * and shown on pipe C (78-10c.4); hotplug's connect and disconnect, a
 * pulse that changed nothing probed again 1 s later, and a storm that
 * masks the pin and polls it until it is turned on again 2 minutes later;
 * and IGT's kms_hdmi_inject, a connector with no monitor forced on with a
 * 4K EDID override and set at 3840x2160@30 (78-10c.5).
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
#include "i915_model_ddi.h"
#include "i915_model_pipe.h"
#include "i915_model_ring.h"
#include "i915_lenovo.h"

extern int vsnprintf(char *s, unsigned long n, const char *format, va_list ap);
extern char *strstr(const char *s, const char *t);
extern int memcmp(const void *a, const void *b, unsigned long n);
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

/* ---- the driver's buffer objects: a few, bound in the aperture by the
 * pin ---- */

#define NBO 4

const struct drm_gem_object_funcs i915_gem_object_funcs;
static struct drm_i915_gem_object bo[NBO];
static struct i915_vma bo_vma[NBO];

/** The GGTT address the pin binds object `i` at. */
#define BO_GGTT(i) (0x01000000u + (uint32_t)(i) * 0x02000000u)

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

/** The boot's own backlight after i915: none. */
int intelbl_take_back(void)
{
    return -1;
}

/* ---- the device ---- */

static struct drm_i915_private gpu;
static struct drm_driver driver;
static struct drm_file fa;
static struct i915_model model;
static struct i915_model_ddi *ddi;
static struct intel_display *disp = &gpu.display;
static struct drm_gem_object *fw_obj;

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

/** Let `us` microseconds pass on the model's clock and the monotonic
 * one. */
static void pass_us(uint64_t us)
{
    model.now_us += us;
    follow_clock(&model);
}

/** One frame, 16.67 ms: a vblank of each pipe that is on and the
 * messages they raise. */
static void frame(void)
{
    int pipe;

    pass_us(16667u);
    for (pipe = 0; pipe < 3; pipe++) {
        i915_model_pipe_vblank(&model, pipe);
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

/** A register's value in the model's file. */
static uint32_t reg(uint32_t r)
{
    return i915_model_peek(&model, r);
}

/** The model's log of the latest bytes of the driver's log, for a row
 * that failed. */
static void show_log(void)
{
    unsigned long n = strlen(log_text);

    printf("%s", n > 3000 ? log_text + n - 3000 : log_text);
}

/* ---- EDIDs ---- */

/* A detailed timing: the clock in kHz, then each direction's active,
 * front porch, sync and blank, and the sync's polarity bits. */
struct dtd {
    uint32_t clock;
    uint32_t ha, hfp, hs, hb;
    uint32_t va, vfp, vs, vb;
};

static const struct dtd dtd_1080p = {148500, 1920, 88, 44, 280, 1080, 4, 5, 45};
static const struct dtd dtd_2160p30 = {297000, 3840, 176, 88, 560, 2160, 8, 10, 90};

/** Pack `t` into the 18 bytes of a detailed timing descriptor at `o`,
 * with positive syncs. */
static void put_dtd(uint8_t *o, const struct dtd *t)
{
    uint32_t c = t->clock / 10u;

    o[0] = (uint8_t)c;
    o[1] = (uint8_t)(c >> 8);
    o[2] = (uint8_t)t->ha;
    o[3] = (uint8_t)t->hb;
    o[4] = (uint8_t)((t->ha >> 8) << 4 | (t->hb >> 8));
    o[5] = (uint8_t)t->va;
    o[6] = (uint8_t)t->vb;
    o[7] = (uint8_t)((t->va >> 8) << 4 | (t->vb >> 8));
    o[8] = (uint8_t)t->hfp;
    o[9] = (uint8_t)t->hs;
    o[10] = (uint8_t)((t->vfp & 15u) << 4 | (t->vs & 15u));
    o[11] = (uint8_t)((t->hfp >> 8) << 6 | (t->hs >> 8) << 4 | (t->vfp >> 4) << 2 | (t->vs >> 4));
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

/**
 * A monitor's EDID into `e`: version 1.3, or 1.4 for DisplayPort, a
 * digital input, the preferred timing `t`, its name and range limits;
 * with `cea` an extension of `cea_len` bytes of data blocks after it. The
 * length, 128 or 256.
 */
static uint32_t make_edid(uint8_t *e, const struct dtd *t, int dp, const uint8_t *cea,
                          uint32_t cea_len)
{
    static const uint8_t head[8] = {0, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0};
    static const uint8_t name[13] = {'R', 'a', 'c', 'h',  '2', ' ', 'T',
                                     'E', 'S', 'T', 0x0a, ' ', ' '};
    int i;

    k_memset(e, 0, 256);
    k_memcpy(e, head, 8);
    e[8] = 0x48; /* "RCH" */
    e[9] = 0x68;
    e[10] = dp ? 2 : 1;
    e[17] = 30;
    e[18] = 1;
    e[19] = dp ? 4 : 3;
    e[20] = dp ? 0xa5 : 0x80; /* digital; DisplayPort at 8 bits a colour */
    e[21] = 60;
    e[22] = 34;
    e[23] = 0x78;
    e[24] = 0x0a;
    for (i = 38; i < 54; i++) {
        e[i] = 1;
    }
    put_dtd(e + 54, t);
    e[75] = 0xfc;
    k_memcpy(e + 77, name, 13);
    e[93] = 0xfd;
    e[95] = 24;
    e[96] = 75;
    e[97] = 15;
    e[98] = 140;
    e[99] = 30;
    e[100] = 0x0a;
    e[111] = 0x10;
    e[126] = cea ? 1 : 0;
    sum_block(e);
    if (!cea) {
        return 128;
    }
    e[128] = 0x02;
    e[129] = 0x03;
    e[130] = (uint8_t)(4u + cea_len);
    k_memcpy(e + 132, cea, cea_len);
    sum_block(e + 128);
    return 256;
}

/* An HDMI monitor's data blocks: VICs 16, 4 and 3, 1080p native, and
 * HDMI's VSDB at 225 MHz. */
static const uint8_t cea_1080p[] = {0x43, 0x90, 0x04, 0x03, 0x67, 0x03,
                                    0x0c, 0x00, 0x10, 0x00, 0x00, 0x2d};

/* A 4K television's data blocks: VIC 16, and HDMI's VSDB at 300 MHz with
 * HDMI VIC 1, 3840x2160@30. */
static const uint8_t cea_2160p30[] = {0x41, 0x10, 0x6b, 0x03, 0x0c, 0x00, 0x10,
                                      0x00, 0x00, 0x3c, 0x20, 0x00, 0x20, 0x01};

/* The DDC pins of the ports, as the Lenovo's VBT gives them. */
#define PIN_B GMBUS_PIN_DPB
#define PIN_C GMBUS_PIN_DPC

/** Put the HDMI monitor of 1080p on DDC pin `pin`. */
static void plug_hdmi_ddc(int pin)
{
    struct i915_model_ddc *c = &ddi->ddc[pin];

    c->edid_len = make_edid(c->edid, &dtd_1080p, 0, cea_1080p, sizeof(cea_1080p));
    c->present = 1;
}

/** Put a DisplayPort monitor of 1080p, DPCD 1.2 at 2.7 Gb/s on four
 * lanes, behind port C's AUX channel. */
static void plug_dp_sink(void)
{
    struct i915_model_dp_sink *s = &ddi->sink[INTEL_PORT_C - 1];

    k_memset(s, 0, sizeof(*s));
    s->dpcd[DP_DPCD_REV] = 0x12;
    s->dpcd[DP_MAX_LINK_RATE] = DP_LINK_BW_2_7;
    s->dpcd[DP_MAX_LANE_COUNT] = 4 | DP_ENHANCED_FRAME_CAP;
    s->dpcd[DP_MAX_DOWNSPREAD] = DP_MAX_DOWNSPREAD_0_5;
    s->dpcd[DP_MAIN_LINK_CHANNEL_CODING] = DP_CAP_ANSI_8B10B;
    s->dpcd[DP_SINK_COUNT] = 1;
    s->dpcd[DP_SET_POWER] = DP_SET_POWER_D0;
    s->edid_len = make_edid(s->edid, &dtd_1080p, 1, NULL, 0);
    s->present = 1;
}

/* ---- the DRM requests ---- */

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

/** A linear XRGB8888 framebuffer of `w` by `h` on object `i`; its ID, or
 * 0. */
static uint32_t fb_new(int i, uint32_t w, uint32_t h)
{
    struct drm_mode_fb_cmd2 r;

    k_memset(&r, 0, sizeof(r));
    r.width = w;
    r.height = h;
    r.pixel_format = DRM_FORMAT_XRGB8888;
    r.handles[0] = bo_new(i, (uint64_t)w * 4u * h);
    r.pitches[0] = w * 4u;
    return drm_mode_addfb2_ioctl(&gpu.drm, &r, &fa) == 0 ? r.fb_id : 0;
}

/** Probe `connector` as GETCONNECTOR does and find its mode of `w` by `h`
 * at `hz` into `out`: 1, or 0 when it has none. */
static int find_mode(struct drm_connector *connector, uint32_t w, uint32_t h, uint32_t hz,
                     struct drm_mode_modeinfo *out)
{
    struct drm_mode_get_connector gc;
    struct drm_mode_modeinfo modes[32];
    uint32_t i;

    k_memset(&gc, 0, sizeof(gc));
    gc.connector_id = connector->base.id;
    if (drm_mode_getconnector(&gpu.drm, &gc, &fa) != 0) {
        return 0;
    }
    gc.modes_ptr = UPTR(modes);
    gc.count_modes = 32;
    gc.count_props = 0;
    gc.count_encoders = 0;
    if (drm_mode_getconnector(&gpu.drm, &gc, &fa) != 0) {
        return 0;
    }
    for (i = 0; i < gc.count_modes && i < 32; i++) {
        if (modes[i].hdisplay == w && modes[i].vdisplay == h && modes[i].vrefresh == hz) {
            *out = modes[i];
            return 1;
        }
    }
    return 0;
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

/** Plug or unplug the monitor of hotplug pin `pin` with a long pulse and
 * take its interrupt. */
static void pulse(int pin, int connected, int long_pulse)
{
    i915_model_ddi_pulse(&model, pin, connected, long_pulse);
    take_messages();
}

/**
 * The model, the device record over it with its interrupts installed, as
 * the bind leaves them, the straps reporting DDI B and C, and the display
 * made.
 */
static int32_t bring_up(void)
{
    static uint8_t lenovo_vbt[LENOVO_VBT_BYTES];
    long got = 0, r;
    int fd;

    CHECK(i915_model_init(&model) == 0, "the model is made");
    ddi = i915_model_ddi(&model);
    clock_base_ns = model_now_ns - (int64_t)model.now_us * 1000;
    model.on_delay = follow_clock;
    i915_model_poke(&model, SFUSE_STRAP, SFUSE_STRAP_DDIB_DETECTED | SFUSE_STRAP_DDIC_DETECTED);
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
    file_open(&fa, &gpu.drm);
    gpu.drm.master = &fa;
    fa.is_master = 1;
    fa.atomic = fa.universal_planes = 1;
    model_wait = sleep_frames;
    return intel_display_init(&gpu);
}

/* ---- the ports ---- */

/** DDI B with HDMI on GMBUS pin 5 at level 9, DDI C DP++ with AUX C and
 * pin 4, as the VBT says; DDI D, which the straps do not report, not
 * made; the hotplug detection of B and C enabled; and each connector
 * probed once, as the console's client does at boot, with no monitor. */
static void row_ports(void)
{
    struct intel_ddi_port *b = &disp->ddi[0], *c = &disp->ddi[1];
    struct drm_mode_modeinfo m;

    CHECK(b->present && b->has_hdmi && !b->has_dp && b->hdmi.ddc_pin == PIN_B && b->hdmi.level == 9,
          "ports: DDI B is HDMI on pin 5 at level 9");
    CHECK(c->present && c->has_hdmi && c->has_dp && c->hdmi.ddc_pin == PIN_C && c->hdmi.dual_mode &&
              c->dp.aux_port == INTEL_PORT_C,
          "ports: DDI C is DP++ on AUX C and pin 4");
    CHECK(!disp->ddi[2].present, "ports: DDI D, not strapped, not made");
    CHECK(logged("display: port B: HDMI") && logged("display: port C: DP+HDMI"),
          "ports: the log's lines");
    CHECK((reg(PCH_PORT_HOTPLUG) &
           (PORT_HOTPLUG_ENABLE(HPD_PORT_B) | PORT_HOTPLUG_ENABLE(HPD_PORT_C))) ==
                  (PORT_HOTPLUG_ENABLE(HPD_PORT_B) | PORT_HOTPLUG_ENABLE(HPD_PORT_C)) &&
              !(reg(PCH_PORT_HOTPLUG) & PORT_HOTPLUG_ENABLE(HPD_PORT_D)) &&
              (reg(PCH_PORT_HOTPLUG) & PORTA_HOTPLUG_ENABLE),
          "ports: hotplug detection of B and C on, and eDP's on a Lynx Point LP");
    (void)find_mode(&b->hdmi_connector, 0, 0, 0, &m);
    (void)find_mode(&c->dp_connector, 0, 0, 0, &m);
    (void)find_mode(&c->hdmi_connector, 0, 0, 0, &m);
    CHECK(b->hdmi_connector.status == DRM_CONNECTOR_DISCONNECTED &&
              c->dp_connector.status == DRM_CONNECTOR_DISCONNECTED &&
              c->hdmi_connector.status == DRM_CONNECTOR_DISCONNECTED,
          "ports: HDMI-A-1, DP-1 and HDMI-A-2 probed, disconnected");
}

/* ---- 78-10c.1: GMBUS ---- */

/**
 * An EDID of two blocks read from pin 5's DDC over GMBUS, offset by an
 * indexed transfer; then, with the controller hung, the transfer timed out
 * and done again by bit-banging, which the bus keeps while it works.
 */
static void row_gmbus(void)
{
    struct intel_i2c_bus *bus = intel_gmbus_bus(&gpu, PIN_B);
    struct intel_gmbus *g = &disp->gmbus[PIN_B];
    struct i915_model_ddc *c = &ddi->ddc[PIN_B];
    static uint8_t buf[512];
    uint32_t len = 0, bits = g->bit_xfers, timeouts = g->timeouts;

    plug_hdmi_ddc(PIN_B);
    CHECK(bus && intel_edid_read(bus, buf, sizeof(buf), &len) == 0 && len == 256 &&
              memcmp(buf, c->edid, 256) == 0,
          "gmbus: the EDID's two blocks read over GMBUS");
    CHECK(c->gmbus_bytes == 256 && c->bit_bytes == 0 && g->bit_xfers == bits &&
              g->timeouts == timeouts,
          "gmbus: every byte through the controller");
    k_memset(buf, 0, sizeof(buf));
    ddi->gmbus_hangs = 1;
    CHECK(intel_edid_read(bus, buf, sizeof(buf), &len) == 0 && len == 256 &&
              memcmp(buf, c->edid, 256) == 0,
          "gmbus: read again with the controller hung");
    CHECK(g->timeouts == timeouts + 1 && c->bit_bytes == 256 &&
              (g->force_bit & GMBUS_FORCE_BIT_RETRY) &&
              logged("gmbus dpb: timed out, falling back to bit banging on pin 5"),
          "gmbus: the transfer timed out and was bit-banged, which the bus keeps");
    CHECK(intel_i2c_probe_ddc(intel_gmbus_bus(&gpu, PIN_C)) == 0,
          "gmbus: no monitor on pin 4 answers");
    g->force_bit = 0;
}

/* ---- 78-10c.3: the PLLs ---- */

/* A clock and its dividers and frequency as Linux's
 * hsw_ddi_calculate_wrpll and hsw_ddi_wrpll_get_freq give them. */
struct wrpll_case {
    uint32_t clock, r2, n2, p, freq;
};

static const struct wrpll_case wrpll_cases[] = {
    {25175, 33, 40, 26, 25174},  {25200, 15, 21, 30, 25200},  {27000, 14, 21, 30, 27000},
    {65000, 19, 32, 14, 64962},  {74250, 16, 22, 10, 74250},  {85500, 15, 19, 8, 85500},
    {108000, 15, 24, 8, 108000}, {135000, 14, 21, 6, 135000}, {148500, 20, 33, 6, 148500},
    {154000, 31, 53, 6, 153870}, {162000, 15, 18, 4, 162000}, {241500, 19, 17, 2, 241578},
    {268500, 95, 94, 2, 267156}, {297000, 20, 22, 2, 297000}, {540000, 2, 2, 1, 540000}};

/** The WRPLL value of `r2`, `n2` and `p` from the LCPLL, enabled. */
static uint32_t wrpll_value(uint32_t r2, uint32_t n2, uint32_t p)
{
    return WRPLL_PLL_ENABLE | WRPLL_REF_LCPLL | WRPLL_DIVIDER_REFERENCE(r2) |
           WRPLL_DIVIDER_FEEDBACK(n2) | WRPLL_DIVIDER_POST(p);
}

/** Each clock's dividers and frequency as Linux's; the SPLL's 1.35 GHz
 * for a link of 2.7 Gb/s and nothing for another. */
static void row_dpll(void)
{
    uint32_t i, r2, n2, p, bad = 0;

    for (i = 0; i < sizeof(wrpll_cases) / sizeof(wrpll_cases[0]); i++) {
        const struct wrpll_case *w = &wrpll_cases[i];

        intel_dpll_calc_wrpll(w->clock * 1000u, &r2, &n2, &p);
        if (r2 != w->r2 || n2 != w->n2 || p != w->p ||
            intel_dpll_wrpll_freq(&gpu, wrpll_value(r2, n2, p)) != w->freq) {
            printf("wrpll %u kHz: r2 %u n2 %u p %u freq %u\n", (unsigned)w->clock, (unsigned)r2,
                   (unsigned)n2, (unsigned)p,
                   (unsigned)intel_dpll_wrpll_freq(&gpu, wrpll_value(r2, n2, p)));
            bad++;
        }
    }
    CHECK(bad == 0, "dpll: each WRPLL's dividers and frequency as Linux's");
    CHECK(intel_dpll_spll_freq(SPLL_PLL_ENABLE | SPLL_FREQ_1350MHz | SPLL_REF_MUXED_SSC) ==
                  270000 &&
              intel_dpll_spll_freq(SPLL_PLL_ENABLE | SPLL_FREQ_810MHz) == 162000,
          "dpll: the SPLL's link clock as Linux's");
}

/** The WRPLL's special reference: the SSC on a ULT part, and on another
 * the non-SSC reference of 24 MHz that FUSE_STRAP3 selects, as Linux's
 * hsw_update_dpll_ref_clks reads it. */
static void row_dpll_ref(void)
{
    static const struct i915_id desktop_id = {0x0412, 0, 0, NULL};
    struct intel_dpll saved[INTEL_NUM_DPLLS];
    uint32_t v = WRPLL_PLL_ENABLE | WRPLL_REF_SPECIAL_HSW | WRPLL_DIVIDER_REFERENCE(20) |
                 WRPLL_DIVIDER_FEEDBACK(22) | WRPLL_DIVIDER_POST(2);
    uint32_t fuse = i915_model_peek(&model, FUSE_STRAP3);
    uint32_t ult, nssc;

    memcpy(saved, disp->dpll, sizeof(saved));
    i915_model_poke(&model, FUSE_STRAP3, fuse | HSW_REF_CLK_SELECT);
    intel_dpll_init(&gpu);
    ult = intel_dpll_wrpll_freq(&gpu, v);
    gpu.id = &desktop_id;
    nssc = intel_dpll_wrpll_freq(&gpu, v);
    gpu.id = &lenovo_id;
    i915_model_poke(&model, FUSE_STRAP3, fuse);
    intel_dpll_init(&gpu);
    memcpy(disp->dpll, saved, sizeof(saved));
    CHECK(ult == (135000u * 22u / 10u) / (2u * 20u) * 2u &&
              nssc == (24000u * 22u / 10u) / (2u * 20u) * 2u,
          "dpll: the special reference is the SSC on ULT, the fused 24 MHz on another");
}

/* ---- 78-10c.2 and .5: HDMI and hotplug ---- */

static uint32_t fb_b, fb_c, fb_4k;

/** A long pulse on B with the monitor there: the connector found
 * connected from the worker with an event, and its modes those of the
 * EDID. */
static void row_hdmi_plug(void)
{
    struct drm_connector *hdmi = &disp->ddi[0].hdmi_connector;
    struct drm_mode_modeinfo m;

    model_hotplugs = 0;
    (void)drm_atomic_commit_work();
    CHECK(hdmi->status == DRM_CONNECTOR_DISCONNECTED, "plug: HDMI-A-1 not yet connected");
    pulse(HPD_PORT_B, 1, 1);
    CHECK(disp->hotplug.pulses >= 1 && (disp->hotplug.event_bits & (1u << HPD_PORT_B)),
          "plug: the pulse's interrupt asks for a probe");
    model_hotplugs = 0;
    CHECK(drm_atomic_commit_work() >= 1 && hdmi->status == DRM_CONNECTOR_CONNECTED &&
              model_hotplugs == 1 && logged("hotplug: HDMI-A-1: connected"),
          "plug: HDMI-A-1 connected from the worker, with an event");
    CHECK(disp->ddi[0].hdmi.info.is_hdmi && disp->ddi[0].hdmi.info.max_tmds_clock == 225000,
          "plug: the sink is HDMI at 225 MHz");
    CHECK(find_mode(hdmi, 1920, 1080, 60, &m) && (m.type & DRM_MODE_TYPE_PREFERRED) &&
              find_mode(hdmi, 1280, 720, 60, &m) && find_mode(hdmi, 720, 480, 60, &m),
          "plug: the EDID's modes, 1080p preferred");
}

/**
 * 1920x1080 on pipe B through HDMI B: WRPLL 1 at Linux's dividers for
 * 148.5 MHz feeding the port, the transcoder in HDMI mode at 8 bits a
 * colour, the VBT's translation as entry 9, the AVI, SPD and GCP
 * infoframes, limited range through the pipe's colour space conversion,
 * the power well held for the pipe, and the pipe's watermark for its
 * plane.
 */
static void row_hdmi_set(void)
{
    struct drm_connector *hdmi = &disp->ddi[0].hdmi_connector;
    struct drm_mode_modeinfo m;
    uint32_t func, avi, spd;

    CHECK(find_mode(hdmi, 1920, 1080, 60, &m), "hdmi: the 1080p mode");
    fb_b = fb_new(1, 1920, 1080);
    log_text[0] = 0;
    model.nlog = 0;
    model.log_lost = 0;
    CHECK(fb_b && setcrtc(1, hdmi, &m, fb_b) == 0, "hdmi: SETCRTC of 1080p on pipe B");
    CHECK(model.log_lost == 0 &&
              i915_model_find_write(&model, DSPCNTR(1), DISP_PIPE_CSC_ENABLE, 0) >= 0 &&
              (reg(DSPCNTR(1)) & (DISP_ENABLE | DISP_PIPE_CSC_ENABLE)) ==
                  (DISP_ENABLE | DISP_PIPE_CSC_ENABLE),
          "hdmi: the plane, off and on, keeps the pipe's colour space conversion");
    CHECK(reg(WRPLL_CTL(0)) == wrpll_value(20, 33, 6) &&
              reg(PORT_CLK_SEL(INTEL_PORT_B)) == PORT_CLK_SEL_WRPLL1 &&
              disp->dpll[DPLL_ID_WRPLL1].active == (1u << 1),
          "hdmi: WRPLL 1 at Linux's 148.5 MHz feeds port B");
    func = reg(TRANS_DDI_FUNC_CTL(1));
    CHECK((func & TRANS_DDI_FUNC_ENABLE) &&
              (func & TRANS_DDI_PORT_MASK) == TRANS_DDI_SELECT_PORT(INTEL_PORT_B) &&
              (func & TRANS_DDI_MODE_SELECT_MASK) == TRANS_DDI_MODE_SELECT_HDMI &&
              (func & TRANS_DDI_BPC_MASK) == TRANS_DDI_BPC_8 &&
              (reg(TRANSCONF(1)) & TRANSCONF_ENABLE) &&
              (reg(DDI_BUF_CTL(INTEL_PORT_B)) & DDI_BUF_CTL_ENABLE),
          "hdmi: transcoder B in HDMI mode at 8 bits a colour on port B, its buffer on");
    CHECK(reg(DDI_BUF_TRANS_LO(INTEL_PORT_B, 9)) == 0x00ffffffu &&
              reg(DDI_BUF_TRANS_HI(INTEL_PORT_B, 9)) == 0x000c0004u,
          "hdmi: the VBT's level 9 as entry 9 of the buffer's translations");
    avi = reg(HSW_TVIDEO_DIP_AVI_DATA(1));
    spd = reg(HSW_TVIDEO_DIP_SPD_DATA(1));
    CHECK(
        (reg(HSW_TVIDEO_DIP_CTL(1)) &
         (VIDEO_DIP_ENABLE_AVI_HSW | VIDEO_DIP_ENABLE_SPD_HSW | VIDEO_DIP_ENABLE_GCP_HSW)) ==
                (VIDEO_DIP_ENABLE_AVI_HSW | VIDEO_DIP_ENABLE_SPD_HSW | VIDEO_DIP_ENABLE_GCP_HSW) &&
            avi == 0x000d0282u && (reg(HSW_TVIDEO_DIP_AVI_DATA(1) + 8) & 0xffu) == 16 &&
            spd == 0x00190183u && reg(HSW_TVIDEO_DIP_SPD_DATA(1) + 8) == 0x00006c65u,
        "hdmi: AVI of VIC 16, SPD of \"Intel\", GCP on");
    CHECK(reg(PIPE_CSC_POSTOFF_HI(1)) == 0x100 && reg(PIPE_CSC_POSTOFF_ME(1)) == 0x100 &&
              reg(PIPE_CSC_POSTOFF_LO(1)) == 0x100,
          "hdmi: limited range through the colour space conversion");
    CHECK(gpu.power.count == 2 && disp->crtc[1].cfg.well, "hdmi: the pipe holds the power well");
    CHECK(((reg(WM0_PIPE_ILK(1)) >> WM0_PIPE_PRIMARY_SHIFT) & 0xffu) != 0,
          "hdmi: pipe B's level 0 watermark written for its plane");
    CHECK(logged("display: pipe B set: 1920x1080@60 on HDMI B, 148500 kHz, 24 bpp, TMDS 148500 "
                 "kHz from WRPLL 1, limited range"),
          "hdmi: the log's line");
    frame();
    frame();
}

/** Pipe B's mode read back at other depths, as Linux's
 * intel_crtc_dotclock: the TMDS clock alone at 6 bits a colour, and at 10
 * bits from a TMDS clock of 267156 kHz the dot clock rounded to the
 * nearest kHz, 213725. */
static void row_hdmi_dotclock(void)
{
    struct intel_pipe_readout hw;
    uint32_t func = i915_model_peek(&model, TRANS_DDI_FUNC_CTL(1));
    uint32_t wrpll = i915_model_peek(&model, WRPLL_CTL(0));
    uint32_t six, ten;

    i915_model_poke(&model, TRANS_DDI_FUNC_CTL(1), (func & ~TRANS_DDI_BPC_MASK) | TRANS_DDI_BPC_6);
    intel_pipe_readout(&gpu, 1, &hw);
    six = hw.mode.clock;
    i915_model_poke(&model, TRANS_DDI_FUNC_CTL(1), (func & ~TRANS_DDI_BPC_MASK) | TRANS_DDI_BPC_10);
    i915_model_poke(&model, WRPLL_CTL(0), wrpll_value(95, 94, 2));
    intel_pipe_readout(&gpu, 1, &hw);
    ten = hw.mode.clock;
    i915_model_poke(&model, TRANS_DDI_FUNC_CTL(1), func);
    i915_model_poke(&model, WRPLL_CTL(0), wrpll);
    CHECK(six == 148500 && ten == 213725,
          "hdmi: the dot clock read back at 6 and 10 bits a colour as Linux's");
}

/** Two connectors routed to one pipe, HDMI B and HDMI C: refused, since a
 * DDI encoder clones with none, as Linux's "rejecting invalid cloning
 * configuration"; pipe B keeps HDMI B alone. */
static void row_hdmi_clone(void)
{
    struct drm_connector *hdmi = &disp->ddi[0].hdmi_connector;
    uint32_t conns[2];
    struct drm_mode_crtc c;
    struct drm_mode_modeinfo m;

    CHECK(find_mode(hdmi, 1920, 1080, 60, &m), "clone: the 1080p mode");
    conns[0] = hdmi->base.id;
    conns[1] = disp->ddi[1].hdmi_connector.base.id;
    k_memset(&c, 0, sizeof(c));
    c.crtc_id = disp->crtc[1].base.base.id;
    c.fb_id = fb_b;
    c.mode_valid = 1;
    c.mode = m;
    c.set_connectors_ptr = UPTR(conns);
    c.count_connectors = 2;
    CHECK(drm_mode_setcrtc(&gpu.drm, &c, &fa) == -EINVAL &&
              disp->ddi[1].hdmi_connector.state->crtc == 0 &&
              hdmi->state->crtc == &disp->crtc[1].base,
          "clone: HDMI B and HDMI C on pipe B refused, pipe B left on HDMI B");
}

/**
 * Hotplug on B: the monitor unplugged and plugged again, each change an
 * event; a long pulse that changed nothing probed again 1 s later and no
 * more; a storm of pulses that masks the pin, which is polled while
 * masked and on again 2 minutes later.
 */
static void row_hotplug(void)
{
    struct drm_connector *hdmi = &disp->ddi[0].hdmi_connector;
    struct intel_hotplug *hp = &disp->hotplug;
    int i;

    ddi->ddc[PIN_B].present = 0;
    pulse(HPD_PORT_B, 0, 1);
    model_hotplugs = 0;
    CHECK(drm_atomic_commit_work() >= 1 && hdmi->status == DRM_CONNECTOR_DISCONNECTED &&
              model_hotplugs == 1 && logged("hotplug: HDMI-A-1: disconnected"),
          "hotplug: unplugged, disconnected with an event");
    pass_us(200000u);
    ddi->ddc[PIN_B].present = 1;
    pulse(HPD_PORT_B, 1, 1);
    model_hotplugs = 0;
    CHECK(drm_atomic_commit_work() >= 1 && hdmi->status == DRM_CONNECTOR_CONNECTED &&
              model_hotplugs == 1,
          "hotplug: plugged again, connected with an event");
    pass_us(1100000u);
    pulse(HPD_PORT_B, 1, 1);
    model_hotplugs = 0;
    CHECK(drm_atomic_commit_work() >= 1 && model_hotplugs == 0 &&
              (hp->retry_bits & (1u << HPD_PORT_B)) && hp->retry_at != 0,
          "hotplug: a pulse that changed nothing, a probe again in 1 s");
    pass_us(1002000u);
    CHECK(drm_atomic_commit_work() >= 1 && hp->retries[HPD_PORT_B] == 1 && hp->retry_bits == 0 &&
              hp->retry_at == 0 && model_hotplugs == 0,
          "hotplug: probed again once, unchanged, and no more");
    pass_us(1100000u);
    for (i = 0; i < 6; i++) {
        pulse(HPD_PORT_B, 1, 1);
        pass_us(10000u);
    }
    CHECK(hp->state[HPD_PORT_B] == HPD_MARK_DISABLED && hp->storms == 1 &&
              (reg(SDEIMR) & SDE_PORTB_HOTPLUG_CPT),
          "hotplug: six pulses in 60 ms a storm, the pin masked");
    (void)drm_atomic_commit_work();
    CHECK(hp->state[HPD_PORT_B] == HPD_DISABLED && hp->reenable_at != 0 && hp->poll_at != 0 &&
              logged("hotplug: storm on port B; polling it"),
          "hotplug: the pin polled while the storm lasts");
    ddi->ddc[PIN_B].present = 0;
    pass_us(10002000u);
    model_hotplugs = 0;
    CHECK(drm_atomic_commit_work() >= 1 && hdmi->status == DRM_CONNECTOR_DISCONNECTED &&
              model_hotplugs == 1,
          "hotplug: the poll finds the monitor gone");
    ddi->ddc[PIN_B].present = 1;
    pass_us(110000000u);
    (void)drm_atomic_commit_work();
    CHECK(hp->state[HPD_PORT_B] == HPD_ENABLED && !(reg(SDEIMR) & SDE_PORTB_HOTPLUG_CPT) &&
              logged("hotplug: storm over; pins on again"),
          "hotplug: on again 2 minutes later, the pin unmasked");
    pulse(HPD_PORT_B, 1, 1);
    model_hotplugs = 0;
    CHECK(drm_atomic_commit_work() >= 1 && hdmi->status == DRM_CONNECTOR_CONNECTED &&
              model_hotplugs == 1,
          "hotplug: the next pulse finds the monitor");
    pass_us(1100000u);
    (void)drm_atomic_commit_work();
}

/* ---- 78-10c.4: DisplayPort ---- */

/**
 * A DisplayPort monitor plugged into C: DP-1 connected by its long pulse,
 * HDMI-A-2 on the same port not; 1920x1080 on pipe C trained on four
 * lanes at 1.62 Gb/s from the LCPLL, the transcoder in SST mode, the
 * well held by both external pipes; then the pipe off.
 */
static void row_dp(void)
{
    struct intel_ddi_port *p = &disp->ddi[1];
    struct i915_model_dp_sink *s = &ddi->sink[INTEL_PORT_C - 1];
    struct drm_mode_modeinfo m;
    uint32_t func;

    plug_dp_sink();
    pulse(HPD_PORT_C, 1, 1);
    model_hotplugs = 0;
    CHECK(drm_atomic_commit_work() >= 1 && p->dp_connector.status == DRM_CONNECTOR_CONNECTED &&
              p->hdmi_connector.status != DRM_CONNECTOR_CONNECTED && model_hotplugs == 1 &&
              logged("hotplug: DP-1: connected"),
          "dp: DP-1 connected by its pulse, HDMI-A-2 not");
    CHECK(find_mode(&p->dp_connector, 1920, 1080, 60, &m), "dp: the monitor's 1080p mode");
    fb_c = fb_new(2, 1920, 1080);
    log_text[0] = 0;
    CHECK(fb_c && setcrtc(2, &p->dp_connector, &m, fb_c) == 0, "dp: SETCRTC of 1080p on pipe C");
    func = reg(TRANS_DDI_FUNC_CTL(2));
    CHECK(s->trained && s->normal && s->trained_lanes == 4 && s->trained_bw == DP_LINK_BW_1_62 &&
              reg(PORT_CLK_SEL(INTEL_PORT_C)) == PORT_CLK_SEL_LCPLL_810,
          "dp: trained on four lanes at 1.62 Gb/s from the LCPLL, the normal pattern on");
    CHECK((func & TRANS_DDI_FUNC_ENABLE) &&
              (func & TRANS_DDI_PORT_MASK) == TRANS_DDI_SELECT_PORT(INTEL_PORT_C) &&
              (func & TRANS_DDI_MODE_SELECT_MASK) == TRANS_DDI_MODE_SELECT_DP_SST &&
              (reg(TRANSCONF(2)) & TRANSCONF_ENABLE),
          "dp: transcoder C in SST mode on port C");
    CHECK(gpu.power.count == 3, "dp: both external pipes hold the power well");
    CHECK(logged("display: pipe C set: 1920x1080 in 1920x1080@60, 148500 kHz, 24 bpp, 4 lanes at "
                 "162000 kHz, DP C"),
          "dp: the log's line");
    frame();
    CHECK(setcrtc(2, NULL, NULL, 0) == 0 && !(reg(TRANSCONF(2)) & TRANSCONF_ENABLE) &&
              !(reg(DDI_BUF_CTL(INTEL_PORT_C)) & DDI_BUF_CTL_ENABLE) && gpu.power.count == 2,
          "dp: pipe C off, its port off and the well let go");
    frame();
}

/**
 * IGT's kms_hdmi_inject: HDMI-A-2, with no monitor, forced on with a 4K
 * EDID override; its modes those of the override, and 3840x2160@30 set on
 * pipe C from WRPLL 2 at Linux's 297 MHz with the HDMI VIC in the vendor
 * infoframe; then the pipe off and the connector given back.
 */
static void row_inject(void)
{
    struct drm_connector *hdmi = &disp->ddi[1].hdmi_connector;
    static uint8_t edid[256];
    struct drm_mode_modeinfo m;
    uint32_t len = make_edid(edid, &dtd_2160p30, 0, cea_2160p30, sizeof(cea_2160p30));

    CHECK(drm_edid_override_set(hdmi, edid, len) == 0, "inject: the EDID override set");
    hdmi->force = DRM_FORCE_ON;
    (void)drm_helper_probe_single_connector_modes(hdmi, 8192, 8192);
    CHECK(hdmi->status == DRM_CONNECTOR_CONNECTED && disp->ddi[1].hdmi.info.is_hdmi &&
              disp->ddi[1].hdmi.adaptor == DP_DUAL_MODE_NONE,
          "inject: HDMI-A-2 forced on, an HDMI sink, no DP++ adaptor assumed");
    CHECK(find_mode(hdmi, 3840, 2160, 30, &m) && (m.type & DRM_MODE_TYPE_PREFERRED),
          "inject: the override's 3840x2160@30 preferred");
    fb_4k = fb_new(3, 3840, 2160);
    log_text[0] = 0;
    CHECK(fb_4k && setcrtc(2, hdmi, &m, fb_4k) == 0, "inject: SETCRTC of 3840x2160@30 on pipe C");
    CHECK(reg(WRPLL_CTL(1)) == wrpll_value(20, 22, 2) &&
              reg(PORT_CLK_SEL(INTEL_PORT_C)) == PORT_CLK_SEL_WRPLL2 &&
              reg(WRPLL_CTL(0)) == wrpll_value(20, 33, 6),
          "inject: WRPLL 2 at Linux's 297 MHz feeds port C, WRPLL 1 still pipe B's");
    CHECK((reg(HSW_TVIDEO_DIP_CTL(2)) & VIDEO_DIP_ENABLE_VS_HSW) &&
              (reg(HSW_TVIDEO_DIP_AVI_DATA(2) + 8) & 0xffu) == 0,
          "inject: the HDMI VIC in the vendor infoframe, none in AVI");
    CHECK(logged("display: pipe C set: 3840x2160@30 on HDMI C, 297000 kHz, 24 bpp, TMDS 297000 "
                 "kHz from WRPLL 2"),
          "inject: the log's line");
    frame();
    CHECK(setcrtc(2, NULL, NULL, 0) == 0 && !(reg(WRPLL_CTL(1)) & WRPLL_PLL_ENABLE) &&
              gpu.power.count == 2,
          "inject: pipe C off, WRPLL 2 off");
    drm_edid_override_reset(hdmi);
    hdmi->force = DRM_FORCE_UNSPECIFIED;
    (void)drm_helper_probe_single_connector_modes(hdmi, 8192, 8192);
    CHECK(hdmi->status == DRM_CONNECTOR_DISCONNECTED, "inject: HDMI-A-2 given back, disconnected");
    frame();
}

/** The display let go: pipe B off with its PLL and well, the hotplug
 * interrupt off, every scanout unpinned. */
static void row_fini(void)
{
    CHECK(setcrtc(1, NULL, NULL, 0) == 0 && !(reg(WRPLL_CTL(0)) & WRPLL_PLL_ENABLE) &&
              gpu.power.count == 1,
          "fini: pipe B off, WRPLL 1 off, the well back to the bind's");
    frame();
    intel_display_fini(&gpu);
    CHECK(!disp->hotplug.ready, "fini: hotplug off");
    drm_mode_file_release(&fa);
    drm_mode_config_cleanup(&gpu.drm);
    CHECK(bo_vma[1].pin_count == 0 && bo_vma[2].pin_count == 0 && bo_vma[3].pin_count == 0,
          "fini: every scanout unpinned");
}

int main(void)
{
    model_start("i915-hdmi-test");
    CHECK(bring_up() == 0, "the display is made");
    fw_obj = disp->crtc[0].primary.state && disp->crtc[0].primary.state->fb
                 ? disp->crtc[0].primary.state->fb->obj[0]
                 : NULL;
    row_ports();
    row_gmbus();
    row_dpll();
    row_dpll_ref();
    row_hdmi_plug();
    row_hdmi_set();
    row_hdmi_dotclock();
    row_hdmi_clone();
    row_hotplug();
    row_dp();
    row_inject();
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
        printf("i915-hdmi-test: ok\n");
    }
    return failures != 0;
}
