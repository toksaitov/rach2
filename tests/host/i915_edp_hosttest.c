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
 * host-deps: rach2/sys/drm/i915/intel_bios.c
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
 * that never sums to 0, and extension blocks good and bad.
 *
 * Returns 0 when every row passes, 1 otherwise.
 */

#include <stdarg.h>

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
    gpu.display.dp.port = 0;
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
    CHECK(intel_dp_dpcd_read(&gpu, DP_DPCD_REV, dpcd, DP_RECEIVER_CAP_SIZE) == DP_RECEIVER_CAP_SIZE,
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

    CHECK(intel_dp_dpcd_writeb(&gpu, DP_SET_POWER, DP_SET_POWER_D3) == 1 &&
              s->dpcd[DP_SET_POWER] == DP_SET_POWER_D3,
          "aux: a write lands");
    CHECK(intel_dp_dpcd_writeb(&gpu, DP_SET_POWER, DP_SET_POWER_D0) == 1 &&
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
    CHECK(intel_dp_dpcd_read(&gpu, DP_DPCD_REV, &rev, 1) == 1 && rev == 0x11,
          "retries: a read gets past four timeouts");
    CHECK(s->messages == 6, "retries: each timeout is sent again at once");

    s = fresh();
    s->rx_errors = 3;
    t0 = model.now_us;
    CHECK(intel_dp_dpcd_read(&gpu, DP_DPCD_REV, &rev, 1) == 1 && rev == 0x11,
          "retries: and past receive errors");
    CHECK(model.now_us - t0 >= 3u * 400u, "retries: each receive error costs 400 us");

    s = fresh();
    s->defers = 5;
    CHECK(intel_dp_dpcd_read(&gpu, DP_DPCD_REV, &rev, 1) == 1 && rev == 0x11,
          "retries: and past DEFER replies");
    CHECK(s->messages == 7, "retries: a DEFER is tried again");

    s = fresh();
    s->zero_replies = 2;
    CHECK(intel_dp_dpcd_read(&gpu, DP_DPCD_REV, &rev, 1) == 1 && rev == 0x11,
          "retries: and past empty replies");

    s = fresh();
    s->defers = 1000;
    CHECK(intel_dp_dpcd_read(&gpu, DP_DPCD_REV, &rev, 1) == -EIO,
          "retries: a sink that always defers is an I/O error");
    CHECK(s->messages == 32, "retries: after Linux's 32 tries");

    s = fresh();
    s->timeouts = 1000;
    t0 = model.now_us;
    CHECK(intel_dp_dpcd_read(&gpu, DP_DPCD_REV, &rev, 1) == -ETIMEDOUT,
          "retries: a sink that never answers times out");
    CHECK(s->messages == 32u * 5u, "retries: five sends of each of the 32 tries");
    CHECK(model.now_us - t0 < 200000u, "retries: in under 200 ms");

    s = fresh();
    i915_model_poke(&model, DP_AUX_CH_CTL(0), DP_AUX_CH_CTL_SEND_BUSY);
    s->busy_us = 0xffffffffu;
    CHECK(intel_dp_dpcd_read(&gpu, DP_DPCD_REV, &rev, 1) == -EBUSY,
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
    CHECK(intel_dp_dpcd_read(&gpu, DP_DPCD_REV, &rev, 1) == 1 && rev == 0x11,
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

    CHECK(intel_dp_read_edid(&gpu, edid, sizeof(edid), &len) == 0 && len == 128,
          "edid: the base block is read");
    CHECK(memcmp(edid, lenovo_panel_edid, 128) == 0, "edid: the panel's bytes");
    CHECK(s->i2c_reads == 8, "edid: in eight pieces of 16 bytes");

    s = fresh();
    s->i2c_defers = 3;
    CHECK(intel_dp_read_edid(&gpu, edid, sizeof(edid), &len) == 0 && len == 128 &&
              memcmp(edid, lenovo_panel_edid, 128) == 0,
          "edid: through I2C DEFER replies");

    s = fresh();
    s->edid[20] ^= 1;
    CHECK(intel_dp_read_edid(&gpu, edid, sizeof(edid), &len) == -EINVAL && len == 0,
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
    CHECK(intel_dp_read_edid(&gpu, edid, sizeof(edid), &len) == 0 && len == 256 &&
              memcmp(edid + 128, s->edid + 128, 128) == 0,
          "edid: an extension block is read after the base");
    s->edid[255] ^= 1;
    CHECK(intel_dp_read_edid(&gpu, edid, sizeof(edid), &len) == 0 && len == 128,
          "edid: a bad extension is dropped");
    CHECK(logged("edp: EDID block 1 bad"), "edid: and logged");
    CHECK(gpu.hw.timeouts == 0, "edid: no wait timed out");
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
    i915_model_free(&model);
    intel_bios_fini(&vbt);
    if (failures == 0) {
        printf("i915-edp-test: ok\n");
    }
    return failures != 0;
}
