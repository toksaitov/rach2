/*
 * i915_display_status.c: read Haswell's panel, backlight, FBC and FIFO
 * registers for debugfs, without bringing the display out of sleep.
 */

#include "i915.h"
#include "i915_debugfs_text.h"
#include "i915_reg.h"
#include "intel_dp.h"

#include "../../lib/kerrno.h"
#include "../../lib/kirq.h"

/** The state of a hardware enable bit. */
static const char *enabled(uint32_t bits)
{
    return bits ? "enabled" : "disabled";
}

/** Read a PSR sink register through AUX, with the commit lock and a wakeref.
 * A missing or unpowered panel has no sample. A failed read of one
 * register leaves the others readable. */
static void psr_sink(struct emit *e, struct intel_dp *dp, uint32_t addr, const char *name)
{
    static const char *const states[] = {"inactive",
                                         "entering and capturing",
                                         "active from RFB",
                                         "active on sink timing",
                                         "exiting and resynchronizing",
                                         "reserved",
                                         "reserved",
                                         "internal error"};
    uint8_t val;
    int32_t rc;

    if (!dp->present) {
        emitf(e, "%s unavailable (no eDP sink)\n", name);
        return;
    }
    rc = intel_dp_dpcd_read_powered(dp, addr, &val, 1);
    if (rc == -ENXIO) {
        emitf(e, "%s unavailable (panel and VDD off)\n", name);
    } else if (rc != 1) {
        emitf(e, "%s unavailable (AUX error %d)\n", name, (int)rc);
    } else if (addr == DP_PSR_EN_CFG) {
        emitf(e, "%s 0x%02x %s\n", name, (unsigned)val, enabled(val & DP_PSR_ENABLE));
    } else if (addr == DP_PSR_STATUS) {
        emitf(e, "%s 0x%02x %s\n", name, (unsigned)val, states[val & DP_PSR_SINK_STATE_MASK]);
    } else {
        emitf(e, "%s 0x%02x link_crc_error=%u rfb_storage_error=%u vsc_sdp_error=%u\n", name,
              (unsigned)val, (unsigned)!!(val & DP_PSR_LINK_CRC_ERROR),
              (unsigned)!!(val & DP_PSR_RFB_STORAGE_ERROR),
              (unsigned)!!(val & DP_PSR_VSC_SDP_UNCORRECTABLE_ERROR));
    }
}

/** Sample Haswell's source PSR state and its eDP sink, with the commit lock
 * and a wakeref, as Linux's intel_psr_status and i915_psr_sink_status do. */
static void psr_status(struct drm_i915_private *i915, struct emit *e)
{
    static const char *const states[] = {"IDLE",  "SRDONACK", "SRDENT",    "BUFOFF",
                                         "BUFON", "AUXACK",   "SRDOFFACK", "SRDENT_ON"};
    struct i915_hw *hw = &i915->hw;
    struct intel_dp *dp = &i915->display.dp;
    uint32_t ctl = i915_read(hw, HSW_SRD_CTL);
    uint32_t status = i915_read(hw, HSW_SRD_STATUS);
    uint32_t perf = i915_read(hw, HSW_SRD_PERF_CNT);

    emitf(e, "SRD_CTL 0x%08x %s\n", ctl, enabled(ctl & EDP_PSR_ENABLE));
    emitf(e, "SRD_STATUS 0x%08x %s\n", status, states[status >> EDP_PSR_STATE_SHIFT]);
    emitf(e, "SRD_PERF_CNT 0x%08x count=%u\n", perf, perf & EDP_PSR_PERF_CNT_MASK);
    psr_sink(e, dp, DP_PSR_EN_CFG, "DP_PSR_EN_CFG");
    psr_sink(e, dp, DP_PSR_STATUS, "DP_PSR_STATUS");
    psr_sink(e, dp, DP_PSR_ERROR_STATUS, "DP_PSR_ERROR_STATUS");
}

/** Sample the panel power sequencer with the commit lock and a wakeref. */
static void panel_timings(struct drm_i915_private *i915, struct emit *e)
{
    static const char *const sequences[] = {"idle", "power-up", "power-down", "reserved"};
    struct i915_hw *hw = &i915->hw;
    uint32_t flags = spin_lock_irqsave(&i915->display.bl_lock);
    uint32_t status = i915_read(hw, PP_STATUS), ctl = i915_read(hw, PP_CONTROL);
    uint32_t on = i915_read(hw, PP_ON_DELAYS), off = i915_read(hw, PP_OFF_DELAYS);
    uint32_t div = i915_read(hw, PP_DIVISOR), cycle = div & PANEL_POWER_CYCLE_DELAY_MASK;

    spin_unlock_irqrestore(&i915->display.bl_lock, flags);
    emitf(e, "PP_STATUS 0x%08x panel_on=%u ready=%u sequence=%s cycle_delay_active=%u state=0x%x\n",
          status, (unsigned)!!(status & PP_ON), (unsigned)!!(status & PP_READY),
          sequences[(status & PP_SEQUENCE_MASK) >> 28],
          (unsigned)!!(status & PP_CYCLE_DELAY_ACTIVE), status & PP_SEQUENCE_STATE_MASK);
    emitf(e, "PP_CONTROL 0x%08x power_on=%u power_reset=%u backlight=%u force_vdd=%u\n", ctl,
          (unsigned)!!(ctl & PANEL_POWER_ON), (unsigned)!!(ctl & PANEL_POWER_RESET),
          (unsigned)!!(ctl & EDP_BLC_ENABLE), (unsigned)!!(ctl & EDP_FORCE_VDD));
    emitf(e, "PP_ON_DELAYS 0x%08x power_up=%u us backlight_on=%u us\n", on,
          ((on >> PANEL_POWER_UP_DELAY_SHIFT) & PANEL_DELAY_FIELD_MASK) * 100u,
          (on & PANEL_DELAY_FIELD_MASK) * 100u);
    emitf(e, "PP_OFF_DELAYS 0x%08x power_down=%u us backlight_off=%u us\n", off,
          ((off >> PANEL_POWER_DOWN_DELAY_SHIFT) & PANEL_DELAY_FIELD_MASK) * 100u,
          (off & PANEL_DELAY_FIELD_MASK) * 100u);
    emitf(e, "PP_DIVISOR 0x%08x reference_divider=%u power_cycle=%u ms\n", div,
          div >> PP_REFERENCE_DIVIDER_SHIFT, cycle ? (cycle - 1u) * 100u : 0u);
}

/** Sample both backlight PWM blocks with the commit lock, a wakeref and
 * bl_lock. Lynx Point's override selects the duty source for its pin. */
static void backlight_pwm(struct drm_i915_private *i915, struct emit *e)
{
    static const char *const transcoders[] = {"A", "B", "C", "eDP"};
    struct i915_hw *hw = &i915->hw;
    uint32_t flags = spin_lock_irqsave(&i915->display.bl_lock);
    uint32_t cpu = i915_read(hw, BLC_PWM_CPU_CTL), cpu_ctl = i915_read(hw, BLC_PWM_CPU_CTL2);
    uint32_t pch_ctl = i915_read(hw, BLC_PWM_PCH_CTL1), pch = i915_read(hw, BLC_PWM_PCH_CTL2);
    uint32_t chicken = i915_read(hw, SOUTH_CHICKEN2);
    uint32_t pch_step = LPT_PWM_STEP(chicken), pch_period = pch >> 16;
    uint32_t pch_clock = LPT_PWM_CLOCK_HZ(i915->id && i915->id->ult);
    uint32_t cpu_period = cpu >> 16, cpu_clock = i915->display.cdclk * 1000u;
    uint32_t cpu_step = cpu_ctl & BLM_CPU_PWM_GRANULARITY ? 8u : 128u;
    uint32_t cpu_clocks = cpu_period * cpu_step, pch_clocks = pch_period * pch_step;
    int override = (pch_ctl & BLM_PCH_OVERRIDE_ENABLE) != 0;
    int pch_on = (pch_ctl & BLM_PCH_PWM_ENABLE) != 0;
    int cpu_on = (cpu_ctl & BLM_PWM_ENABLE) != 0;
    uint32_t cpu_duty = cpu & BACKLIGHT_DUTY_CYCLE_MASK, pch_duty = pch & BACKLIGHT_DUTY_CYCLE_MASK;

    spin_unlock_irqrestore(&i915->display.bl_lock, flags);
    emitf(e, "BLC_PWM_CPU_CTL 0x%08x cpu_pin_period=%u cpu_duty=%u", cpu, cpu_period, cpu_duty);
    if (cpu_clocks != 0 && cpu_clock != 0) {
        emitf(e, " cpu_pin_frequency=%u Hz", (cpu_clock + cpu_clocks / 2u) / cpu_clocks);
    } else {
        emit_s(e, " cpu_pin_frequency=unavailable");
    }
    emitf(e, "\nBLC_PWM_CPU_CTL2 0x%08x %s transcoder=%s blinking=%u\n", cpu_ctl, enabled(cpu_on),
          transcoders[(cpu_ctl >> BLM_TRANSCODER_SHIFT) & BLM_TRANSCODER_MASK],
          (unsigned)!!(cpu_ctl & BLM_PWM_BLINK_ENABLE));
    emitf(e, "BLC_PWM_PCH_CTL1 0x%08x %s override=%u polarity=%s\n", pch_ctl, enabled(pch_on),
          (unsigned) override, pch_ctl & BLM_PCH_POLARITY ? "active-low" : "active-high");
    emitf(e, "BLC_PWM_PCH_CTL2 0x%08x period=%u pch_duty=%u", pch, pch_period, pch_duty);
    if (pch_clocks != 0) {
        emitf(e, " frequency=%u Hz", (pch_clock + pch_clocks / 2u) / pch_clocks);
    } else {
        emit_s(e, " frequency=unavailable (zero period)");
    }
    if (!pch_on) {
        emit_s(e, " output=disabled");
    } else if (!override && !cpu_on) {
        emit_s(e, " duty_source=CPU (inactive)");
    } else if (pch_period != 0) {
        emitf(e, " duty_source=%s duty=%u/%u", override ? "PCH" : "CPU",
              override ? pch_duty : cpu_duty, pch_period);
    }
    emitf(e, "\nSOUTH_CHICKEN2 0x%08x pwm_step=%u clocks\n", chicken, pch_step);
}

/** Sample Haswell's frame buffer compression control and segment count. */
static void fbc_status(struct drm_i915_private *i915, struct emit *e)
{
    uint32_t ctl = i915_read(&i915->hw, ILK_DPFC_CONTROL_A);
    uint32_t status = i915_read(&i915->hw, ILK_DPFC_STATUS2_A);

    emitf(e, "ILK_DPFC_CONTROL 0x%08x %s (%s)\n", ctl, enabled(ctl & DPFC_CTL_EN),
          ctl & DPFC_CTL_EN ? "hardware enable set" : "Rach2 does not configure FBC");
    emitf(e, "ILK_DPFC_STATUS2 0x%08x compressed_segments=%u\n", status,
          status & DPFC_COMP_SEG_MASK_IVB);
}

/** Sample underrun latches without acknowledging them, under irq.lock with
 * the device awake. Hardware can combine several underruns in one latch. */
static void fifo_underrun_status(struct drm_i915_private *i915, struct emit *e)
{
    uint64_t counts[I915_NUM_PIPES];
    uint32_t flags = spin_lock_irqsave(&i915->irq.lock);
    uint32_t status = i915_read(&i915->hw, GEN7_ERR_INT);
    int pipe;

    for (pipe = 0; pipe < I915_NUM_PIPES; pipe++) {
        counts[pipe] = i915->display.crtc[pipe].underrun_count;
    }
    spin_unlock_irqrestore(&i915->irq.lock, flags);
    emitf(e, "GEN7_ERR_INT 0x%08x fifo_underrun_pending=%u,%u,%u (A,B,C)\n", status,
          (unsigned)!!(status & ERR_INT_FIFO_UNDERRUN(I915_PIPE_A)),
          (unsigned)!!(status & ERR_INT_FIFO_UNDERRUN(I915_PIPE_B)),
          (unsigned)!!(status & ERR_INT_FIFO_UNDERRUN(I915_PIPE_C)));
    for (pipe = 0; pipe < I915_NUM_PIPES; pipe++) {
        emitf(e, "PIPE_%c_UNDERRUNS 0x%Lx %Lu latches cleared since bind\n", 'A' + pipe,
              counts[pipe], counts[pipe]);
    }
}

int32_t i915_display_status_sample(struct drm_i915_private *i915, enum i915_display_status file,
                                   char *buf, uint32_t cap)
{
    struct emit e;
    int32_t rc;

    e.buf = buf;
    e.cap = cap;
    e.len = 0;
    rc = drm_modeset_lock(&i915->drm);
    if (rc != 0) {
        return rc;
    }
    if (i915->drm.unplugged) {
        rc = -ENODEV;
    } else if (i915->hw.ops->config_read32) {
        emit_s(&e, "unavailable: fake i915 has no panel or scanout hardware\n");
    } else if (!i915_rpm_get_if_awake(i915)) {
        emit_s(&e, "unavailable: device is runtime suspended\n");
    } else {
        switch (file) {
        case I915_STATUS_PSR:
            psr_status(i915, &e);
            break;
        case I915_STATUS_PPS:
            panel_timings(i915, &e);
            break;
        case I915_STATUS_PWM:
            backlight_pwm(i915, &e);
            break;
        case I915_STATUS_FBC:
            fbc_status(i915, &e);
            break;
        case I915_STATUS_FIFO:
            fifo_underrun_status(i915, &e);
            break;
        default:
            rc = -EINVAL;
            break;
        }
        i915_rpm_put(i915);
    }
    drm_modeset_unlock(&i915->drm);
    if (rc != 0) {
        return rc;
    }
    return e.len < cap ? emit_end(&e) : -EOVERFLOW;
}
