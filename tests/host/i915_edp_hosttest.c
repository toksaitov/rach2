/*
 * i915_edp_hosttest.c: the Intel graphics driver's eDP output on Haswell,
 * its AUX channel, panel power sequencer, link training and full mode set,
 * against the host model's panel (i915_model_sink.c), which answers as the
 * Lenovo's B156XTN04.0 does.
 *
 * host-deps: rach2/sys/drm/i915/i915_irq.c rach2/sys/drm/i915/i915_hw.c
 * host-deps: tests/host/i915_model.c rach2/sys/display/backlight.c
 * host-deps: rach2/sys/display/intelbl.c rach2/sys/lib/kstring.c
 * host-deps: rach2/sys/lib/kctypes.c tests/host/compat/spinlock_host.c
 * host-deps: rach2/sys/drm/i915/intel_bios.c rach2/sys/drm/i915/i915_power.c
 * host-inputs: tests/fixtures/lenovo-gpu/i915_vbt.bin
 *
 * The model's clock moves only when the driver waits, and the monotonic
 * clock follows it, so a row reads every delay of the sequence from the
 * model's own times.
 *
 * The rows: the AUX channel's messages, their divider, precharge and
 * timeout, a DPCD read after Linux's probe of DPCD_REV and a write, the
 * retries after timeouts, receive errors, DEFER replies and empty replies,
 * each bounded, a channel that stays busy, and VDD forced on for a panel
 * without power and let go five power cycles later; the EDID read in
 * pieces of 16 bytes over I2C, through I2C DEFER replies, a base block
 * that never sums to 0, and extension blocks good and bad; the link's
 * training at 2.7 Gb/s on one lane in Linux's register and DPCD order, the
 * buffer driving the levels the sink was told, and trainings that fail in
 * clock recovery or equalization, tried again and then fallen back from as
 * Linux does, down to no link left; the output's init, which finds the
 * panel's fixed mode, the rates both ends take and its colour depth; a
 * mode's configuration, its bits a pixel, link and M/N values as Linux
 * computes them, and the refresh rate a hertz off it takes; the fitter's
 * windows for each scaling mode, its limits and its pixel rate; the
 * sequencer's delays from the firmware and the VBT, and the panel off and
 * on again with T9, T10, the power cycle, T1+T3 and T8 kept.
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
#include "i915_model_sink.h"
#include "i915_lenovo.h"

extern int vsnprintf(char *s, unsigned long n, const char *format, va_list ap);
extern char *strstr(const char *s, const char *t);
extern int memcmp(const void *a, const void *b, unsigned long n);
extern int open(const char *path, int flags, ...);
extern long read(int fd, void *buf, unsigned long n);
extern int close(int fd);

/* ---- the kernel around the driver ---- */

static char log_text[16384];

/** The log since the row's last `log_clear`, kept whole for the rows that
 * read it. */
void klog(int level, const char *subsystem, const char *format, ...)
{
    unsigned long n = strlen(log_text);
    va_list ap;

    (void)level;
    (void)subsystem;
    if (n + 256 >= sizeof(log_text)) {
        return;
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

/** Empty the log. */
static void log_clear(void)
{
    log_text[0] = 0;
}

uint32_t kirq_save(void)
{
    return 0;
}

void kirq_restore(uint32_t flags)
{
    (void)flags;
}

void k_udelay(uint32_t usec)
{
    (void)usec;
}

/** The scheduler, which never runs here: the driver's sleeps are waits
 * on the model's clock. */
int sched_is_enabled(void)
{
    return 0;
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

int32_t i915_gem_object_pin_to_display(struct drm_i915_gem_object *obj, uint32_t align,
                                       struct i915_vma **vma)
{
    struct i915_vma *v = &bo_vma[obj - bo];

    (void)align;
    if (!obj->vmas) {
        v->obj = obj;
        v->start = 0x01000000u + (uint32_t)(obj - bo) * 0x00800000u;
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
static struct i915_model model;
static struct intel_vbt vbt;
static uint8_t lenovo_vbt[LENOVO_VBT_BYTES];
static int64_t clock_base_ns; /* the monotonic time at the model's 0 */

/** Read `n` bytes of the file `path` into `buf`; 1 when all were read. */
static int load(const char *path, uint8_t *buf, long n)
{
    long got = 0, r;
    int fd = open(path, 0);

    if (fd < 0) {
        return 0;
    }
    while (got < n && (r = read(fd, buf + got, (unsigned long)(n - got))) > 0) {
        got += r;
    }
    close(fd);
    return got == n;
}

/** After each wait of the driver: the monotonic clock follows the
 * model's. */
static void follow_clock(struct i915_model *m)
{
    model_now_ns = clock_base_ns + (int64_t)m->now_us * 1000;
}

/** The model's time in milliseconds. */
static uint64_t now_ms_model(void)
{
    return model.now_us / 1000u;
}

/** Let `ms` milliseconds pass on the model, as an idle system would. */
static void idle_ms(uint32_t ms)
{
    i915_delay_us(&gpu.hw, ms * 1000u);
}

/** A register's value in the model's file. */
static uint32_t reg(uint32_t r)
{
    return i915_model_peek(&model, r);
}

/**
 * A fresh model and device record: the firmware's picture, the CD clock
 * read, the panel's VBT entry decoded with its EDID, and the sequencer
 * set up, as the eDP output's init leaves them before its first AUX
 * message. The sink's counts start at 0.
 */
static struct i915_model_sink *fresh(void)
{
    struct i915_model_sink *s;

    i915_model_free(&model);
    k_memset(&gpu, 0, sizeof(gpu));
    CHECK(i915_model_init(&model) == 0, "the model is made");
    clock_base_ns = model_now_ns - (int64_t)model.now_us * 1000;
    model.on_delay = follow_clock;
    gpu.pci = model.pci;
    gpu.hw = model.hw;
    /* The reference the bind's i915_power_init holds for its life. */
    gpu.power.count = 1;
    gpu.power.on = 1;
    gpu.display.dp.i915 = &gpu;
    gpu.display.dp.name = "edp";
    gpu.display.dp.edp = 1;
    gpu.display.dp.port = INTEL_PORT_A;
    gpu.display.dp.aux_port = INTEL_PORT_A;
    gpu.display.dp.vbt.panel_type = -1;
    intel_bios_panel_init(&vbt, &gpu.display.dp.vbt, intel_bios_encoder_lookup(&vbt, INTEL_PORT_A),
                          lenovo_panel_edid, 1);
    intel_cdclk_init(&gpu);
    intel_pps_init(&gpu);
    s = i915_model_sink(&model);
    s->messages = s->native_reads = s->native_writes = s->i2c_reads = 0;
    log_clear();
    return s;
}

/** The log index of the first write to `r` with `bits` set at or after
 * `from`, or -1. */
static long find_write_bits(uint32_t r, uint32_t bits, uint32_t from)
{
    uint32_t i;

    for (i = from; i < model.nlog; i++) {
        if (model.log[i].op == I915_MODEL_WRITE && model.log[i].reg == r &&
            (model.log[i].value & bits) == bits) {
            return (long)i;
        }
    }
    return -1;
}

/* ---- AUX ---- */

/** A DPCD read and a write on the lit panel: Linux's control word, the
 * probe read of DPCD_REV before the read, and the bytes. */
static void row_aux_dpcd(void)
{
    struct i915_model_sink *s = fresh();
    uint8_t dpcd[16];
    uint32_t from = model.nlog, ctl;
    long at;

    uint64_t t0 = model.now_us;

    k_memset(dpcd, 0xaa, sizeof(dpcd));
    CHECK(gpu.display.cdclk == 450000, "aux: the CD clock is 450 MHz");
    CHECK(intel_dp_dpcd_read(&gpu.display.dp, DP_DPCD_REV, dpcd, DP_RECEIVER_CAP_SIZE) ==
              DP_RECEIVER_CAP_SIZE,
          "aux: the receiver caps are read");
    CHECK(memcmp(dpcd, s->dpcd, DP_RECEIVER_CAP_SIZE) == 0 && dpcd[DP_RECEIVER_CAP_SIZE] == 0xaa,
          "aux: the caps are the sink's, and no byte more is written");
    CHECK(dpcd[DP_DPCD_REV] == 0x11 && dpcd[DP_MAX_LINK_RATE] == DP_LINK_BW_2_7 &&
              (dpcd[DP_MAX_LANE_COUNT] & DP_MAX_LANE_COUNT_MASK) == 1,
          "aux: DPCD 1.1, 2.7 Gb/s, one lane");
    CHECK(s->messages == 2 && s->native_reads == 2, "aux: DPCD_REV is probed before the read");
    at = find_write_bits(DP_AUX_CH_CTL(0), DP_AUX_CH_CTL_SEND_BUSY, from);
    CHECK(at >= 0, "aux: a message is sent");
    ctl = at >= 0 ? model.log[at].value : 0;
    CHECK((ctl & DP_AUX_CH_CTL_BIT_CLOCK_2X_MASK) == 225,
          "aux: the divider takes the 450 MHz CD clock to 2 MHz");
    CHECK((ctl & (0xfu << 16)) == DP_AUX_CH_CTL_PRECHARGE_2US(3) &&
              (ctl & DP_AUX_CH_CTL_TIME_OUT_MASK) == DP_AUX_CH_CTL_TIME_OUT_400us &&
              (ctl & DP_AUX_CH_CTL_MESSAGE_SIZE_MASK) == DP_AUX_CH_CTL_MESSAGE_SIZE(4),
          "aux: 6 us more precharge, a 400 us timeout and a 4-byte header");
    CHECK(
        (ctl & (DP_AUX_CH_CTL_DONE | DP_AUX_CH_CTL_TIME_OUT_ERROR | DP_AUX_CH_CTL_RECEIVE_ERROR)) ==
            (DP_AUX_CH_CTL_DONE | DP_AUX_CH_CTL_TIME_OUT_ERROR | DP_AUX_CH_CTL_RECEIVE_ERROR),
        "aux: the send clears the last message's status");
    CHECK((reg(PP_CONTROL) & EDP_FORCE_VDD) && gpu.display.dp.pps.vdd_off_due != 0,
          "aux: VDD is forced for the message, as Linux does, and let go later");
    CHECK(model.now_us - t0 < 5000u, "aux: without the power up delay of a panel with power");

    CHECK(intel_dp_dpcd_writeb(&gpu.display.dp, DP_SET_POWER, DP_SET_POWER_D3) == 1 &&
              s->dpcd[DP_SET_POWER] == DP_SET_POWER_D3,
          "aux: a write lands");
    CHECK(intel_dp_dpcd_writeb(&gpu.display.dp, DP_SET_POWER, DP_SET_POWER_D0) == 1 &&
              s->dpcd[DP_SET_POWER] == DP_SET_POWER_D0 && s->native_writes == 2,
          "aux: and the next");
    CHECK(gpu.hw.timeouts == 0, "aux: no wait timed out");
}

/** Faults the sink makes, each retried as Linux does and each bounded. */
static void row_aux_retries(void)
{
    struct i915_model_sink *s = fresh();
    uint8_t rev = 0;
    uint64_t t0;

    s->timeouts = 4;
    CHECK(intel_dp_dpcd_read(&gpu.display.dp, DP_DPCD_REV, &rev, 1) == 1 && rev == 0x11,
          "retries: a read gets past four timeouts");
    CHECK(s->messages == 6, "retries: each timeout is sent again at once");

    s = fresh();
    s->rx_errors = 3;
    t0 = model.now_us;
    CHECK(intel_dp_dpcd_read(&gpu.display.dp, DP_DPCD_REV, &rev, 1) == 1 && rev == 0x11,
          "retries: and past receive errors");
    CHECK(model.now_us - t0 >= 3u * 400u, "retries: each receive error costs 400 us");

    s = fresh();
    s->defers = 5;
    CHECK(intel_dp_dpcd_read(&gpu.display.dp, DP_DPCD_REV, &rev, 1) == 1 && rev == 0x11,
          "retries: and past DEFER replies");
    CHECK(s->messages == 7, "retries: a DEFER is tried again");

    s = fresh();
    s->zero_replies = 2;
    CHECK(intel_dp_dpcd_read(&gpu.display.dp, DP_DPCD_REV, &rev, 1) == 1 && rev == 0x11,
          "retries: and past empty replies");

    s = fresh();
    s->defers = 1000;
    CHECK(intel_dp_dpcd_read(&gpu.display.dp, DP_DPCD_REV, &rev, 1) == -EIO,
          "retries: a sink that always defers is an I/O error");
    CHECK(s->messages == 32, "retries: after Linux's 32 tries");

    s = fresh();
    s->timeouts = 1000;
    t0 = model.now_us;
    CHECK(intel_dp_dpcd_read(&gpu.display.dp, DP_DPCD_REV, &rev, 1) == -ETIMEDOUT,
          "retries: a sink that never answers times out");
    CHECK(s->messages == 32u * 5u, "retries: five sends of each of the 32 tries");
    CHECK(model.now_us - t0 < 200000u, "retries: in under 200 ms");

    s = fresh();
    i915_model_poke(&model, DP_AUX_CH_CTL(0), DP_AUX_CH_CTL_SEND_BUSY);
    s->busy_us = 0xffffffffu;
    CHECK(intel_dp_dpcd_read(&gpu.display.dp, DP_DPCD_REV, &rev, 1) == -EBUSY,
          "retries: a channel that stays busy is refused");
    CHECK(s->messages == 0, "retries: and nothing is sent on it");
    CHECK(logged("aux: not started"), "retries: the busy channel is logged");
}

/** A panel without power: VDD forced on for the message after the power
 * cycle and the power up delay, and let go five cycles after the last. */
static void row_aux_vdd(void)
{
    struct i915_model_sink *s = fresh();
    struct i915_model_panel *p = i915_model_panel(&model);
    uint8_t rev = 0;
    uint64_t t0;

    intel_pps_vdd_on_unlocked(&gpu);
    intel_pps_off(&gpu);
    CHECK(!(reg(PP_CONTROL) & (PANEL_POWER_ON | EDP_FORCE_VDD)) && !p->on && !s->powered,
          "vdd: the panel is off");
    t0 = now_ms_model();
    CHECK(intel_dp_dpcd_read(&gpu.display.dp, DP_DPCD_REV, &rev, 1) == 1 && rev == 0x11,
          "vdd: the panel without power answers");
    CHECK(reg(PP_CONTROL) & EDP_FORCE_VDD, "vdd: through VDD forced on");
    CHECK(now_ms_model() - t0 >= 500u + 200u, "vdd: after the power cycle and the power up delay");
    CHECK(gpu.display.dp.pps.vdd_off_due != 0 && !gpu.display.dp.pps.want_vdd,
          "vdd: and is let go later");
    idle_ms(2400);
    intel_pps_vdd_work(&gpu);
    CHECK(reg(PP_CONTROL) & EDP_FORCE_VDD, "vdd: not before five power cycles");
    idle_ms(200);
    intel_pps_vdd_work(&gpu);
    CHECK(!(reg(PP_CONTROL) & EDP_FORCE_VDD) && !s->powered, "vdd: and then let go");
    CHECK(gpu.hw.timeouts == 0, "vdd: no wait timed out");
}

/* ---- the EDID ---- */

/** The EDID, read 16 bytes at a time over I2C, and its faults. */
static void row_edid(void)
{
    struct i915_model_sink *s = fresh();
    uint8_t edid[INTEL_EDID_BLOCKS * 128];
    uint32_t len = 0, i;
    uint8_t sum;

    CHECK(intel_dp_read_edid(&gpu.display.dp, edid, sizeof(edid), &len) == 0 && len == 128,
          "edid: the base block is read");
    CHECK(memcmp(edid, lenovo_panel_edid, 128) == 0, "edid: the panel's bytes");
    CHECK(s->i2c_reads == 8, "edid: in eight pieces of 16 bytes");

    s = fresh();
    s->i2c_defers = 3;
    CHECK(intel_dp_read_edid(&gpu.display.dp, edid, sizeof(edid), &len) == 0 && len == 128 &&
              memcmp(edid, lenovo_panel_edid, 128) == 0,
          "edid: through I2C DEFER replies");

    s = fresh();
    s->edid[20] ^= 1;
    CHECK(intel_dp_read_edid(&gpu.display.dp, edid, sizeof(edid), &len) == -EINVAL && len == 0,
          "edid: a base block that never sums to 0 is refused");
    CHECK(s->i2c_reads == 4u * 8u, "edid: after four reads of it");

    s = fresh();
    s->edid[126] = 1;
    s->edid[127] = (uint8_t)(s->edid[127] - 1);
    s->edid[128] = 0x02;
    s->edid[129] = 0x03;
    for (sum = 0, i = 128; i < 255; i++) {
        sum = (uint8_t)(sum + s->edid[i]);
    }
    s->edid[255] = (uint8_t)(0u - sum);
    s->edid_len = 256;
    CHECK(intel_dp_read_edid(&gpu.display.dp, edid, sizeof(edid), &len) == 0 && len == 256 &&
              memcmp(edid + 128, s->edid + 128, 128) == 0,
          "edid: an extension block is read after the base");
    s->edid[255] ^= 1;
    CHECK(intel_dp_read_edid(&gpu.display.dp, edid, sizeof(edid), &len) == 0 && len == 128,
          "edid: a bad extension is dropped");
    CHECK(logged("edp: EDID block 1 bad"), "edid: and logged");
    CHECK(gpu.hw.timeouts == 0, "edid: no wait timed out");
}

/* ---- link training ---- */

/** The pixel clock of the panel's mode, kHz. */
#define PANEL_CLOCK 76300u

/** What the connector's init leaves for a training at 2.7 Gb/s on one
 * lane: the sink's caps, the rates both ends take, the buffer's bits of
 * the firmware's that stay, and DDI A's translations written. */
static struct i915_model_sink *link_setup(void)
{
    struct i915_model_sink *s = fresh();
    struct intel_dp *dp = &gpu.display.dp;

    k_memcpy(dp->dpcd, s->dpcd, sizeof(dp->dpcd));
    dp->rates[0] = 162000;
    dp->rates[1] = 270000;
    dp->nrates = 2;
    dp->max_lanes = 1;
    dp->link_rate = 270000;
    dp->lane_count = 1;
    dp->saved_port_bits = reg(DDI_BUF_CTL(0)) & (DDI_BUF_PORT_REVERSAL | DDI_A_4_LANES);
    intel_ddi_prepare_buf_trans(&gpu.display.dp);
    intel_ddi_init_dp_buf_reg(&gpu.display.dp, dp->lane_count);
    return s;
}

/** A training that needs one step of swing and one of pre-emphasis, in
 * Linux's register and DPCD order. */
static void row_train(void)
{
    struct i915_model_sink *s = link_setup();
    uint32_t from = model.nlog;
    long buf_off, off, tp, on;

    CHECK(reg(DDI_BUF_TRANS_LO(0, 8)) == 0x80d75fffu && reg(DDI_BUF_TRANS_HI(0, 8)) == 0x000b0000u,
          "train: DDI A's translations are Linux's");
    s->trained = s->normal = 0;
    CHECK(intel_dp_start_link_train(&gpu.display.dp, PANEL_CLOCK) == 0, "train: the link trains");
    CHECK(s->trained && s->trained_bw == DP_LINK_BW_2_7 && s->trained_lanes == 1,
          "train: at 2.7 Gb/s on one lane");
    CHECK(s->levels_bad == 0, "train: the buffer drives the levels the sink was told");
    CHECK(s->cr_reads == 2 && s->eq_reads == 2,
          "train: one step of swing in clock recovery and one of pre-emphasis after");
    CHECK((reg(DDI_BUF_CTL(0)) & DDI_BUF_EMP_MASK) == DDI_BUF_TRANS_SELECT(5),
          "train: swing 1 and pre-emphasis 1 are translation 5");
    CHECK(s->dpcd[DP_LINK_BW_SET] == DP_LINK_BW_2_7 &&
              s->dpcd[DP_LANE_COUNT_SET] == (1 | DP_LANE_COUNT_ENHANCED_FRAME_EN) &&
              s->dpcd[DP_DOWNSPREAD_CTRL] == 0 &&
              s->dpcd[DP_MAIN_LINK_CHANNEL_CODING_SET] == DP_SET_ANSI_8B10B,
          "train: the sink is set to 8b/10b, 2.7 Gb/s and one lane with enhanced framing");
    off = i915_model_find_write(&model, DP_TP_CTL(0), LENOVO_FW_DP_TP_CTL_A & ~DP_TP_CTL_ENABLE,
                                from);
    tp = i915_model_find_write(
        &model, DP_TP_CTL(0),
        DP_TP_CTL_ENABLE | DP_TP_CTL_LINK_TRAIN_PAT1 | DP_TP_CTL_ENHANCED_FRAME_ENABLE, from);
    on = i915_model_find_write(&model, DDI_BUF_CTL(0), DDI_BUF_CTL_ENABLE, from);
    buf_off = i915_model_find_write(&model, DDI_BUF_CTL(0),
                                    LENOVO_FW_DDI_BUF_CTL_A & ~DDI_BUF_CTL_ENABLE, from);
    CHECK(buf_off >= 0 && off > buf_off && tp > off && on > tp,
          "train: the firmware's link goes off, then pattern 1, then the buffer on one lane");
    CHECK((reg(DP_TP_CTL(0)) & DP_TP_CTL_LINK_TRAIN_MASK) == DP_TP_CTL_LINK_TRAIN_IDLE,
          "train: the port is left on its idle pattern");
    intel_dp_stop_link_train(&gpu.display.dp);
    CHECK(s->dpcd[DP_TRAINING_PATTERN_SET] == DP_TRAINING_PATTERN_DISABLE && s->normal &&
              gpu.display.dp.link_trained,
          "train: the end of training sends pictures on the trained link");
    CHECK(gpu.hw.timeouts == 0, "train: no wait timed out");
}

/** Trainings that fail: tried again at once, then the most parameters,
 * then the same link while the mode needs it, then a lower rate. */
static void row_train_fail(void)
{
    struct i915_model_sink *s = link_setup();
    struct intel_dp *dp = &gpu.display.dp;

    s->cr_swing = 3; /* more than Haswell's swing at any pre-emphasis */
    CHECK(intel_dp_start_link_train(&gpu.display.dp, PANEL_CLOCK) == -EAGAIN && !s->trained,
          "fail: a sink that never recovers its clock fails, for a retry");
    CHECK(s->cr_reads == 2u * 6u,
          "fail: each try ends after a read at swing 0 and the same swing asked 5 times");
    CHECK(dp->train_failures == 2 && dp->use_max_params && dp->link_max_rate == 0,
          "fail: two failures in a row, then the sink's most is tried");
    CHECK(logged("edp: link training failed at 270000 kHz on 1 lanes"), "fail: and logged");
    log_clear();
    CHECK(intel_dp_start_link_train(&gpu.display.dp, PANEL_CLOCK) == -EAGAIN &&
              dp->link_max_rate == 0,
          "fail: then the same link, since the mode needs 2.7 Gb/s");
    CHECK(logged("same parameters"), "fail: and that is logged");
    CHECK(intel_dp_start_link_train(&gpu.display.dp, 50000) == -EAGAIN &&
              dp->link_max_rate == 162000 && dp->link_max_lanes == 1,
          "fail: a mode that fits 1.62 Gb/s falls back to it");

    dp->link_rate = 162000;
    log_clear();
    CHECK(intel_dp_start_link_train(&gpu.display.dp, 50000) == -EIO && logged("can't reduce"),
          "fail: no link left below 1.62 Gb/s on one lane");

    s = link_setup();
    s->eq_preemph = 3; /* pre-emphasis 3 leaves no swing for the clock */
    CHECK(intel_dp_start_link_train(&gpu.display.dp, PANEL_CLOCK) == -EAGAIN && !s->trained,
          "fail: a sink that never equalizes fails");
    CHECK(logged("clock recovery lost in channel equalization"),
          "fail: when the clock is lost for the pre-emphasis");
    CHECK(gpu.hw.timeouts == 0, "fail: no wait timed out");
}

/* ---- the panel and a mode's configuration ---- */

/** The output's init against the panel: its DPCD, EDID and fixed mode,
 * the rates both ends take and its colour depth, as Linux reported them
 * on the Lenovo. */
static void row_init(void)
{
    struct intel_dp *dp = &gpu.display.dp;
    const struct drm_mode_modeinfo *f = &dp->fixed[0];

    fresh();
    gpu.vbt = vbt;
    CHECK(intel_dp_init(&gpu) == 0 && dp->present, "init: the panel answers");
    CHECK(dp->nfixed == 1 && f->hdisplay == 1366 && f->vdisplay == 768 && f->clock == PANEL_CLOCK &&
              f->htotal == 1558 && f->vtotal == 816 && (f->type & DRM_MODE_TYPE_PREFERRED),
          "init: the EDID's 1366x768 the one fixed mode");
    CHECK(dp->nrates == 2 && dp->rates[0] == 162000 && dp->rates[1] == 270000 &&
              dp->max_lanes == 1 && dp->edid_bpc == 6 && dp->use_max_params,
          "init: 1.62 and 2.7 Gb/s on one lane, 6 bits a colour, the most for eDP before 1.4");
    CHECK(logged("panel: the EDID's preferred fixed mode 1366x768@60, 76300 kHz") &&
              logged("edp: DPCD 1.1, eDP 0, 1 lane, rates 162000 to 270000 kHz, 6 bpc, EDID 128 "
                     "bytes"),
          "init: the log's lines");
    k_memset(&gpu.vbt, 0, sizeof(gpu.vbt)); /* the row's copy; `vbt` owns it */
}

/** A configuration of the panel's own mode on pipe A for `conn`. */
static int32_t config(const struct drm_connector_state *conn, uint32_t src_w, uint32_t src_h,
                      struct intel_crtc_config *cfg)
{
    k_memset(cfg, 0, sizeof(*cfg));
    cfg->mode = gpu.display.dp.fixed[0];
    cfg->src_w = src_w;
    cfg->src_h = src_h;
    return intel_dp_compute_config(&gpu.display.dp, I915_PIPE_A, conn, 0, cfg);
}

/** The link and the M/N values of the panel's mode, the firmware's own;
 * a refresh rate a hertz off taken, more refused. */
static void row_config(void)
{
    struct drm_connector_state conn;
    struct intel_crtc_config cfg;
    struct drm_mode_modeinfo m;

    k_memset(&conn, 0, sizeof(conn));
    conn.scaling_mode = DRM_MODE_SCALE_ASPECT;
    conn.max_requested_bpc = 12;
    CHECK(config(&conn, 1366, 768, &cfg) == 0 && cfg.bpp == 18 && cfg.dither &&
              cfg.port_clock == 270000 && cfg.lanes == 1 && !cfg.pfit && !cfg.force_thru,
          "config: 18 bits a pixel, dithered, on one lane at 2.7 Gb/s, no fitter");
    /* Linux's data M, 171675 kB/s of 270000 scaled to N 0x800000; the
     * firmware's is 21 more, its own rounding. */
    CHECK(cfg.tu == 64 && cfg.data_m == 5333756u && cfg.data_n == LENOVO_DATA_N &&
              cfg.link_m == LENOVO_LINK_M && cfg.link_n == LENOVO_LINK_N,
          "config: Linux's M/N values, the link's the firmware's");
    gpu.display.dp.use_max_params = 1;
    CHECK(config(&conn, 1366, 768, &cfg) == 0 && cfg.port_clock == 270000,
          "config: after failures, the sink's most");
    gpu.display.dp.use_max_params = 0;
    m = gpu.display.dp.fixed[0];
    m.clock = 77600; /* 61 Hz */
    CHECK(intel_panel_compute_config(&gpu, &m) == 0 && m.clock == PANEL_CLOCK,
          "config: 61 Hz is the panel's 60");
    m.clock = 79000; /* 62 Hz */
    CHECK(intel_panel_compute_config(&gpu, &m) == -EINVAL, "config: 62 Hz refused");
    m = gpu.display.dp.fixed[0];
    CHECK(intel_panel_mode_valid(&gpu, &m) == 0, "config: the panel's size and rate valid");
    m.hdisplay = 1024;
    CHECK(intel_panel_mode_valid(&gpu, &m) == -EINVAL, "config: another size not the panel's");
}

/** A fitter's window for a source of `w` by `h` on `pipe` with
 * `scaling`: 0 and its position and size, or the error. */
static int32_t window(int pipe, uint32_t scaling, uint32_t w, uint32_t h, uint32_t *pos,
                      uint32_t *size)
{
    struct intel_crtc_config cfg;
    int32_t err;

    k_memset(&cfg, 0, sizeof(cfg));
    cfg.mode = gpu.display.dp.fixed[0];
    cfg.src_w = w;
    cfg.src_h = h;
    err = intel_pfit_compute_config(pipe, scaling, &cfg);
    *pos = cfg.pf_pos;
    *size = cfg.pf_size;
    return err == 0 && !cfg.pfit ? -1 : err;
}

/** The fitter's windows as Linux's intel_pch_panel_fitting sets them,
 * its limits, and the pixel rate it reads at. */
static void row_pfit(void)
{
    struct intel_crtc_config cfg;
    uint32_t pos, size;

    CHECK(window(0, DRM_MODE_SCALE_ASPECT, 800, 600, &pos, &size) == 0 && pos == PF_WIN_XPOS(171) &&
              size == (PF_WIN_XSIZE(1024) | 768u),
          "pfit: Full aspect pillarboxes 800x600");
    CHECK(window(0, DRM_MODE_SCALE_ASPECT, 1366, 600, &pos, &size) == 0 && pos == 84u &&
              size == (PF_WIN_XSIZE(1366) | 600u),
          "pfit: and letterboxes 1366x600");
    CHECK(window(0, DRM_MODE_SCALE_CENTER, 800, 600, &pos, &size) == 0 &&
              pos == (PF_WIN_XPOS(283) | 84u) && size == (PF_WIN_XSIZE(800) | 600u),
          "pfit: Center leaves 800x600 unscaled in the middle");
    CHECK(window(0, DRM_MODE_SCALE_FULLSCREEN, 800, 600, &pos, &size) == 0 && pos == 0 &&
              size == (PF_WIN_XSIZE(1366) | 768u),
          "pfit: Full stretches it");
    CHECK(window(0, DRM_MODE_SCALE_FULLSCREEN, 1600, 900, &pos, &size) == -ERANGE,
          "pfit: a scale down past 1.125 refused");
    CHECK(window(0, DRM_MODE_SCALE_FULLSCREEN, 2100, 1200, &pos, &size) == -ERANGE &&
              window(1, DRM_MODE_SCALE_FULLSCREEN, 2100, 1200, &pos, &size) == -EINVAL,
          "pfit: pipe B's fitter takes no source wider than 2048");
    k_memset(&cfg, 0, sizeof(cfg));
    cfg.mode = gpu.display.dp.fixed[0];
    cfg.src_w = 1500;
    cfg.src_h = 800;
    CHECK(intel_pfit_compute_config(0, DRM_MODE_SCALE_FULLSCREEN, &cfg) == 0 &&
              intel_pfit_pixel_rate(&cfg) == 87275u,
          "pfit: a scale down reads the source faster");
    cfg.src_w = 800;
    cfg.src_h = 600;
    CHECK(intel_pfit_compute_config(0, DRM_MODE_SCALE_FULLSCREEN, &cfg) == 0 &&
              intel_pfit_pixel_rate(&cfg) == PANEL_CLOCK,
          "pfit: a scale up at the panel's clock");
}

/* ---- the panel power sequencer ---- */

/** The sequencer's delays: the larger of the firmware's and the VBT's,
 * written with the backlight's as 1 and the power cycle in the divisor. */
static void row_pps_init(void)
{
    struct intel_pps *pps;
    uint32_t from;

    i915_model_free(&model);
    k_memset(&gpu, 0, sizeof(gpu));
    CHECK(i915_model_init(&model) == 0, "the model is made");
    gpu.hw = model.hw;
    /* The reference the bind's i915_power_init holds for its life. */
    gpu.power.count = 1;
    gpu.power.on = 1;
    gpu.display.dp.vbt.panel_type = -1;
    intel_bios_panel_init(&vbt, &gpu.display.dp.vbt, intel_bios_encoder_lookup(&vbt, INTEL_PORT_A),
                          lenovo_panel_edid, 1);
    from = model.nlog;
    log_clear();
    intel_pps_init(&gpu);
    pps = &gpu.display.dp.pps;
    CHECK(pps->power_up_ms == 200 && pps->power_down_ms == 50 && pps->power_cycle_ms == 500 &&
              pps->backlight_on_ms == 1 && pps->backlight_off_ms == 200,
          "pps init: the delays Linux reported on the Lenovo");
    CHECK(i915_model_find_write(&model, PP_ON_DELAYS, 0x07d00001u, from) >= 0 &&
              i915_model_find_write(&model, PP_OFF_DELAYS, 0x01f40001u, from) >= 0,
          "pps init: the delays written, the backlight's as 1");
    CHECK(i915_model_find_write(&model, PP_DIVISOR, 0x0004af06u, from) >= 0,
          "pps init: 100 us of the 24 MHz raw clock and a 500 ms power cycle");
    CHECK(logged("pps: power up 200 ms, down 50 ms, cycle 500 ms, backlight on 1 ms, off 200 ms"),
          "pps init: logged");
}

/** The panel off and on again in Linux's order, each delay of the
 * sequence kept on the model's clock. */
static void row_pps_order(void)
{
    struct i915_model_panel *p;
    struct intel_pps *pps;
    uint64_t t0;

    fresh();
    p = i915_model_panel(&model);
    pps = &gpu.display.dp.pps;
    CHECK(p->on && intel_pps_have_power_or_vdd(&gpu), "pps: the firmware's panel is on");
    t0 = model.now_us;
    intel_pps_backlight_off(&gpu);
    CHECK(!(reg(PP_CONTROL) & EDP_BLC_ENABLE) && p->blc_off_at == t0,
          "pps: the backlight goes off first");
    CHECK(model.now_us - p->blc_off_at >= 200000u, "pps: and the video stays for T9");
    intel_pps_vdd_on_unlocked(&gpu);
    intel_pps_off(&gpu);
    CHECK(p->down_from >= p->blc_off_at + 200000u, "pps: the power goes off after T9");
    CHECK(!p->on && p->down_at - p->down_from >= 50000u && model.now_us >= p->down_at,
          "pps: the sequencer took T10 and the driver waited for it");
    CHECK(!(reg(PP_CONTROL) & (PANEL_POWER_ON | EDP_FORCE_VDD | EDP_BLC_ENABLE)) && !pps->want_vdd,
          "pps: power, VDD and backlight go off together");

    intel_pps_on(&gpu);
    CHECK(p->on && p->up_at >= p->down_at + 500000u + 200000u,
          "pps: power on waits the power cycle, then T1+T3");
    CHECK((reg(PP_CONTROL) & (PANEL_POWER_ON | PANEL_POWER_RESET)) ==
              (PANEL_POWER_ON | PANEL_POWER_RESET),
          "pps: power on with the reset bit, as Linux sets it past Ironlake");
    CHECK(model.now_us >= p->up_at && (reg(PP_STATUS) & PP_SEQUENCE_STATE_MASK) == 8,
          "pps: the driver waits until the panel is on and idle");
    intel_pps_backlight_on(&gpu);
    CHECK((reg(PP_CONTROL) & EDP_BLC_ENABLE) && p->blc_on_at >= p->up_at + 1000u,
          "pps: the backlight goes on T8 after the power");
    CHECK(p->power_ups == 1 && p->power_downs == 1, "pps: one cycle of the panel");

    t0 = model.now_us;
    intel_pps_backlight_off(&gpu);
    intel_pps_vdd_on_unlocked(&gpu);
    intel_pps_off(&gpu);
    intel_pps_on(&gpu);
    CHECK(p->up_at - p->down_at >= 500000u, "pps: an off and on at once still waits the cycle");
    CHECK(gpu.hw.timeouts == 0, "pps: no wait timed out");
}

int main(void)
{
    model_start("i915-edp-test");
    if (!load(LENOVO_VBT_FILE, lenovo_vbt, LENOVO_VBT_BYTES)) {
        printf("i915-edp-test: fail: %s not read\n", LENOVO_VBT_FILE);
        return 1;
    }
    CHECK(intel_bios_init(&vbt, lenovo_vbt, LENOVO_VBT_BYTES, "test") == 0, "the VBT is decoded");
    row_aux_dpcd();
    row_aux_retries();
    row_aux_vdd();
    row_edid();
    row_train();
    row_train_fail();
    row_init();
    row_config();
    row_pfit();
    row_pps_init();
    row_pps_order();
    i915_model_free(&model);
    intel_bios_fini(&vbt);
    if (failures == 0) {
        printf("i915-edp-test: ok\n");
    }
    return failures != 0;
}
