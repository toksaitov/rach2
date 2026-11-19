/*
 * Check Haswell's display status snapshots against register and AUX stubs.
 *
 * host-deps: rach2/sys/drm/i915/i915_display_status.c
 * host-deps: rach2/sys/drm/i915/intel_dp_aux.c
 * host-deps: rach2/sys/lib/kstring.c rach2/sys/lib/kctypes.c
 * host-deps: tests/host/compat/spinlock_host.c tests/host/compat/kirq_none.c
 */

#include "drm/i915/i915.h"
#include "drm/i915/i915_reg.h"
#include "drm/i915/intel_dp.h"
#include "klog.h"
#include "lib/kerrno.h"
#include "lib/kirq.h"
#include "timer.h"

extern int printf(const char *, ...);
extern char *strstr(const char *, const char *);

static struct drm_i915_private gpu;
static unsigned failures, reads, aux_reads, holds;
static int have_power = 1, locked, nack_status;
static uint32_t aux_data, aux_ctl, pch_ctl = 0xc0000000u, pch_pwm = 0x03aa01d5u;
static uint32_t cpu_ctl = 0xf8000000u;
static uint32_t granularity;

/** Record one failed assertion. */
static void check(int passed, const char *what)
{
    if (!passed) {
        printf("i915-display-status: %s\n", what);
        failures++;
    }
}

/** Stub the source registers and the AUX reply buffer. */
uint32_t i915_read(struct i915_hw *hw, uint32_t reg)
{
    (void)hw;
    check(locked && holds, "register read outside the commit lock or wakeref");
    reads++;
    switch (reg) {
    case HSW_SRD_CTL:
        return 0x80000000u;
    case HSW_SRD_STATUS:
        return 0x60000000u;
    case HSW_SRD_PERF_CNT:
        return 0xa000002au;
    case PP_STATUS:
        return 0xc8000008u;
    case PP_CONTROL:
        return 0xabcd0007u;
    case PP_ON_DELAYS:
        return 0x07d0000au;
    case PP_OFF_DELAYS:
        return 0x01f407d0u;
    case PP_DIVISOR:
        return 0x00123406u;
    case BLC_PWM_CPU_CTL:
        return 0x00fa009cu;
    case BLC_PWM_CPU_CTL2:
        return cpu_ctl;
    case BLC_PWM_PCH_CTL1:
        return pch_ctl;
    case BLC_PWM_PCH_CTL2:
        return pch_pwm;
    case SOUTH_CHICKEN2:
        return granularity;
    case ILK_DPFC_CONTROL_A:
        return 0;
    case ILK_DPFC_STATUS2_A:
        return 0xa00000aau;
    case GEN7_ERR_INT:
        return 0x08;
    default:
        if (reg == DP_AUX_CH_CTL(0)) {
            return aux_ctl;
        }
        if (reg == DP_AUX_CH_DATA(0, 0)) {
            return aux_data;
        }
        check(0, "unexpected register read");
        return 0;
    }
}

/** Answer native AUX reads and reject writes outside the transport. */
void i915_write(struct i915_hw *hw, uint32_t reg, uint32_t value)
{
    uint32_t addr;
    unsigned byte;

    (void)hw;
    check(locked && holds, "AUX transfer outside the commit lock or wakeref");
    if (reg == DP_AUX_CH_DATA(0, 0)) {
        aux_data = value;
        return;
    }
    if (reg != DP_AUX_CH_CTL(0)) {
        check(0, "display register written");
        return;
    }
    if (!(value & DP_AUX_CH_CTL_SEND_BUSY)) {
        aux_ctl = 0;
        return;
    }
    check(aux_data >> 28 == DP_AUX_NATIVE_READ, "AUX request was not a native read");
    addr = (aux_data >> 8) & 0xfffffu;
    switch (addr) {
    case DP_DPCD_REV:
        byte = 0x11u;
        break;
    case DP_PSR_EN_CFG:
        byte = 0x01u;
        break;
    case DP_PSR_STATUS:
        byte = 0x82u;
        break;
    case DP_PSR_ERROR_STATUS:
        byte = 0x05u;
        break;
    default:
        check(0, "unexpected DPCD address");
        byte = 0;
        break;
    }
    aux_data = byte << 16;
    if (addr == DP_PSR_STATUS && nack_status) {
        aux_data |= (uint32_t)DP_AUX_NATIVE_REPLY_NACK << 28;
    }
    aux_ctl = DP_AUX_CH_CTL_DONE | DP_AUX_CH_CTL_MESSAGE_SIZE(2);
    aux_reads++;
}

/** The AUX stub completes immediately. */
int32_t i915_wait_for(struct i915_hw *hw, const char *name, uint32_t reg, uint32_t mask,
                      uint32_t expected, uint32_t timeout_us)
{
    (void)name;
    (void)timeout_us;
    return (i915_read(hw, reg) & mask) == expected ? 0 : -ETIMEDOUT;
}

/** No device time passes during the stub's retry delay. */
void i915_delay_us(struct i915_hw *hw, uint32_t us)
{
    (void)hw;
    (void)us;
}

/** Take the stub's commit lock. */
int32_t drm_modeset_lock(struct drm_device *dev)
{
    check(dev == &gpu.drm && !locked, "taking the commit lock");
    locked = 1;
    return 0;
}

/** Release the stub's commit lock. */
void drm_modeset_unlock(struct drm_device *dev)
{
    check(dev == &gpu.drm && locked && !holds, "releasing the commit lock");
    locked = 0;
}

/** Take a wakeref on the stub if it is awake. */
int i915_rpm_get_if_awake(struct drm_i915_private *i915)
{
    if (i915->hw.asleep) {
        return 0;
    }
    holds++;
    return 1;
}

/** AUX may take another wakeref while the snapshot owns one. */
void i915_rpm_get(struct drm_i915_private *i915)
{
    check(i915 == &gpu && holds != 0 && !i915->hw.asleep, "unexpected runtime resume");
    holds++;
}

/** Release a wakeref. */
void i915_rpm_put(struct drm_i915_private *i915)
{
    check(i915 == &gpu && holds != 0, "extra runtime put");
    holds--;
}

/** The fake backend supplies the PCI identity of the simulated chip. */
static uint32_t fake_config(struct i915_hw *hw, uint32_t offset)
{
    (void)hw;
    (void)offset;
    return 0;
}

/** The display snapshot uses the panel power it finds. */
int intel_pps_have_power_or_vdd(struct drm_i915_private *i915)
{
    (void)i915;
    return have_power;
}

/** Fail if the diagnostic tries to force VDD. */
int intel_pps_vdd_on_unlocked(struct drm_i915_private *i915)
{
    (void)i915;
    check(0, "snapshot forced panel VDD on");
    return 0;
}

/** Fail if the diagnostic changes VDD's delayed release. */
void intel_pps_vdd_off_unlocked(struct drm_i915_private *i915, int sync)
{
    (void)i915;
    (void)sync;
    check(0, "snapshot changed panel VDD release");
}

/** This AUX channel has no external connector. */
int intel_dp_connected(struct intel_dp *dp)
{
    (void)dp;
    check(0, "external port was queried");
    return 0;
}

/** The stub channel never stays busy. */
void intel_display_msleep(struct drm_i915_private *i915, uint32_t ms)
{
    (void)i915;
    (void)ms;
    check(0, "diagnostic waited on the panel");
}

/** EDID is outside these native AUX reads. */
int32_t intel_edid_read(const struct intel_i2c_bus *bus, uint8_t *buf, uint32_t cap, uint32_t *len)
{
    (void)bus;
    (void)buf;
    (void)cap;
    (void)len;
    return -ENXIO;
}

/** DDC probing is outside these native AUX reads. */
int intel_i2c_probe_ddc(const struct intel_i2c_bus *bus)
{
    (void)bus;
    return 0;
}

/** Print no logs from the stub. */
void klog(int level, const char *tag, const char *fmt, ...)
{
    (void)level;
    (void)tag;
    (void)fmt;
}

/** Divide with the host runtime. */
uint64_t tick_div(uint64_t n, uint32_t d, uint32_t *rem)
{
    if (rem) {
        *rem = (uint32_t)(n % d);
    }
    return n / d;
}

/** Take a fresh snapshot and check a line or field. */
static void sample(enum i915_display_status file, const char *expect)
{
    char text[512];
    int32_t n = i915_display_status_sample(&gpu, file, text, sizeof(text));

    check(n > 0 && text[n] == '\0' && strstr(text, expect) != NULL, expect);
}

/** Check real, unavailable and removed registers, and both PWM sources. */
int main(void)
{
    struct i915_hw_ops ops = {0};
    static const struct i915_id id = {.ult = 1};
    unsigned before;

    gpu.hw.ops = &ops;
    gpu.id = &id;
    gpu.display.dp.i915 = &gpu;
    gpu.display.dp.present = gpu.display.dp.edp = 1;
    gpu.display.cdclk = 450000u;
    spin_lock_init(&gpu.display.bl_lock, "backlight");
    spin_lock_init(&gpu.irq.lock, "irq");

    sample(I915_STATUS_PSR, "SRD_CTL 0x80000000 enabled\n");
    sample(I915_STATUS_PSR, "SRD_STATUS 0x60000000 BUFOFF\n");
    sample(I915_STATUS_PSR, "SRD_PERF_CNT 0xa000002a count=42\n");
    sample(I915_STATUS_PSR, "DP_PSR_EN_CFG 0x01 enabled\n");
    sample(I915_STATUS_PSR, "DP_PSR_STATUS 0x82 active from RFB\n");
    sample(I915_STATUS_PSR, "DP_PSR_ERROR_STATUS 0x05 link_crc_error=1 rfb_storage_error=0");
    nack_status = 1;
    before = aux_reads;
    sample(I915_STATUS_PSR, "DP_PSR_STATUS unavailable (AUX error -5)\n");
    check(aux_reads == before + 6u, "a rejected register delayed the snapshot with retries");
    sample(I915_STATUS_PSR, "DP_PSR_ERROR_STATUS 0x05");
    before = aux_reads;
    have_power = 0;
    sample(I915_STATUS_PSR, "DP_PSR_EN_CFG unavailable (panel and VDD off)\n");
    check(aux_reads == before, "AUX traffic to an unpowered panel");

    sample(I915_STATUS_PPS, "PP_STATUS 0xc8000008 panel_on=1 ready=1 sequence=idle");
    sample(I915_STATUS_PPS, "PP_CONTROL 0xabcd0007 power_on=1 power_reset=1 backlight=1");
    sample(I915_STATUS_PPS, "PP_ON_DELAYS 0x07d0000a power_up=200000 us backlight_on=1000 us");
    sample(I915_STATUS_PPS, "PP_OFF_DELAYS 0x01f407d0 power_down=50000 us backlight_off=200000 us");
    sample(I915_STATUS_PPS, "PP_DIVISOR 0x00123406 reference_divider=4660 power_cycle=500 ms");
    sample(I915_STATUS_PWM, "frequency=200 Hz duty_source=PCH duty=469/938");
    sample(I915_STATUS_PWM, "cpu_pin_period=250 cpu_duty=156 cpu_pin_frequency=225000 Hz");
    sample(I915_STATUS_PWM, "transcoder=eDP blinking=1");
    pch_ctl &= ~BLM_PCH_OVERRIDE_ENABLE;
    granularity = LPT_PWM_GRANULARITY;
    sample(I915_STATUS_PWM, "frequency=1599 Hz duty_source=CPU duty=156/938");
    cpu_ctl &= ~BLM_PWM_ENABLE;
    sample(I915_STATUS_PWM, "frequency=1599 Hz duty_source=CPU (inactive)");
    pch_ctl &= ~BLM_PCH_PWM_ENABLE;
    sample(I915_STATUS_PWM, "frequency=1599 Hz output=disabled");
    pch_pwm = 0;
    sample(I915_STATUS_PWM, "frequency=unavailable (zero period)");

    sample(I915_STATUS_FBC, "ILK_DPFC_CONTROL 0x00000000 disabled (Rach2 does not configure FBC)");
    sample(I915_STATUS_FBC, "ILK_DPFC_STATUS2 0xa00000aa compressed_segments=170");
    gpu.display.crtc[I915_PIPE_A].underrun_count = 0x100000028ULL;
    sample(I915_STATUS_FIFO, "PIPE_A_UNDERRUNS 0x100000028 4294967336");
    sample(I915_STATUS_FIFO, "GEN7_ERR_INT 0x00000008 fifo_underrun_pending=0,1,0");

    before = reads;
    gpu.hw.asleep = 1;
    sample(I915_STATUS_PPS, "unavailable: device is runtime suspended");
    gpu.hw.asleep = 0;
    ops.config_read32 = fake_config;
    sample(I915_STATUS_PPS, "unavailable: fake i915 has no panel or scanout hardware");
    gpu.drm.unplugged = 1;
    check(i915_display_status_sample(&gpu, I915_STATUS_PSR, NULL, 0) == -ENODEV,
          "a removed device returns ENODEV");
    check(reads == before, "registers read on a suspended, simulated or unplugged device");
    if (failures == 0) {
        printf("i915-display-status: ok\n");
    }
    return failures != 0;
}
