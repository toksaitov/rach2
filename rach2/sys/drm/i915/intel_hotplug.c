/*
 * intel_hotplug.c: hotplug of the monitors on DDI B to D, after Linux's
 * display/intel_hotplug.c and the Lynx Point part of intel_hotplug_irq.c:
 * the PCH's pulses, storms, and the probe that sends hotplug events.
 */

#include "i915.h"
#include "i915_reg.h"

#include "../../klog.h"
#include "../../lib/kerrno.h"
#include "../../rtc.h"
#include "../../timer.h"

/* The PCH's interrupt of each pin, Linux's hpd_cpt. */
static const uint32_t sde_bit[HPD_NUM_PINS] = {SDE_PORTB_HOTPLUG_CPT, SDE_PORTC_HOTPLUG_CPT,
                                               SDE_PORTD_HOTPLUG_CPT};

/** The DDI port of `pin`, or NULL when it was not made. */
static struct intel_ddi_port *pin_port(struct drm_i915_private *i915, int pin)
{
    struct intel_ddi_port *p = &i915->display.ddi[pin];

    return p->present ? p : NULL;
}

/** The interrupts of the pins whose port was made, `enabled` only those
 * in the state HPD_ENABLED, Linux's intel_hpd_hotplug_irqs and
 * intel_hpd_enabled_irqs. */
static uint32_t pin_irqs(struct drm_i915_private *i915, int enabled)
{
    uint32_t bits = 0;
    int pin;

    for (pin = 0; pin < HPD_NUM_PINS; pin++) {
        if (pin_port(i915, pin) && (!enabled || i915->display.hotplug.state[pin] == HPD_ENABLED)) {
            bits |= sde_bit[pin];
        }
    }
    return bits;
}

/**
 * Count a pulse on `pin` in its storm window, Linux's
 * intel_hpd_irq_storm_detect: a long pulse counts 10, a short one nothing,
 * as on a part with DisplayPort MST, and a window of 1000 ms that passed
 * starts again. 1 when the count passed HPD_STORM_THRESHOLD, the pin then
 * marked for disabling. Under the interrupt mask's lock.
 */
static int storm_detect(struct intel_hotplug *hp, int pin, int long_hpd, uint64_t now)
{
    if (!long_hpd) {
        return 0;
    }
    if (now < hp->storm_start[pin] ||
        now > hp->storm_start[pin] + MS_TO_TICKS(HPD_STORM_DETECT_MS)) {
        hp->storm_start[pin] = now;
        hp->storm_count[pin] = 0;
    }
    hp->storm_count[pin] += 10u;
    if (hp->storm_count[pin] <= HPD_STORM_THRESHOLD) {
        return 0;
    }
    hp->state[pin] = HPD_MARK_DISABLED;
    hp->storms++;
    return 1;
}

/**
 * The PCH's hotplug interrupt, Linux's ibx_hpd_irq_handler and
 * intel_hpd_irq_handler: PCH_PORT_HOTPLUG read and written back, which
 * clears its status bits; a port with DisplayPort keeps its pulses for
 * intel_dp_hpd_pulse, an HDMI port asks for a probe at once; a pin in a
 * storm is masked. Under the interrupt mask's lock.
 */
static void hpd_irq(struct drm_i915_private *i915, uint32_t iir, void *data)
{
    struct intel_hotplug *hp = &i915->display.hotplug;
    struct i915_hw *hw = &i915->hw;
    uint64_t now = rtc_ticks_now();
    uint32_t dig, storm = 0;
    int pin, queue = 0;

    (void)data;
    dig = i915_read(hw, PCH_PORT_HOTPLUG);
    i915_write(hw, PCH_PORT_HOTPLUG, dig);
    hp->pulses++;
    if (dig == 0) {
        return;
    }
    for (pin = 0; pin < HPD_NUM_PINS; pin++) {
        struct intel_ddi_port *p = pin_port(i915, pin);
        int long_hpd = (dig & PORT_HOTPLUG_LONG_DETECT(pin)) != 0;

        if (!(iir & sde_bit[pin]) || !p) {
            continue;
        }
        if (p->has_dp) {
            if (long_hpd) {
                hp->long_mask |= 1u << pin;
            } else {
                hp->short_mask |= 1u << pin;
            }
            queue = 1;
        }
        if (hp->state[pin] != HPD_ENABLED) {
            continue;
        }
        if (!p->has_dp) {
            hp->event_bits |= 1u << pin;
            long_hpd = 1;
            queue = 1;
        }
        if (storm_detect(hp, pin, long_hpd, now)) {
            hp->event_bits &= ~(1u << pin);
            storm |= sde_bit[pin];
            queue = 1;
        }
    }
    if (storm) {
        i915_irq_mask_locked(i915, I915_IRQ_SDE, storm);
    }
    if (queue) {
        drm_dev_work_later(&i915->drm, &hp->work);
    }
}

/** A hash of a cached EDID, FNV-1a, to see that a probe changed it, as
 * Linux's connector epoch counter does. */
static uint32_t edid_hash(const uint8_t *edid, uint32_t len)
{
    uint32_t h = 2166136261u;
    uint32_t i;

    for (i = 0; i < len; i++) {
        h = (h ^ edid[i]) * 16777619u;
    }
    return h ^ len;
}

/** The cached EDID's hash of `connector` of port `p`. */
static uint32_t connector_edid_hash(struct intel_ddi_port *p, struct drm_connector *connector)
{
    return connector == &p->dp_connector ? edid_hash(p->dp.edid, p->dp.edid_len)
                                         : edid_hash(p->hdmi.edid, p->hdmi.edid_len);
}

/**
 * Probe `connector` of port `p`, Linux's intel_hotplug_detect_connector:
 * its detect runs even when it is forced, its status is kept only when it
 * is not; 1 when the status or the EDID changed.
 */
static int detect_connector(struct intel_ddi_port *p, struct drm_connector *connector)
{
    int old_status = connector->status;
    uint32_t old_edid = connector_edid_hash(p, connector);
    int status = connector->funcs->detect(connector, 0);

    if (connector->force == DRM_FORCE_UNSPECIFIED) {
        connector->status = status;
    }
    return status != old_status || connector_edid_hash(p, connector) != old_edid;
}

/** Each connector of port `p` in a list of two, NULL for none. */
static void port_connectors(struct intel_ddi_port *p, struct drm_connector **out)
{
    out[0] = p->has_dp ? &p->dp_connector : NULL;
    out[1] = p->has_hdmi ? &p->hdmi_connector : NULL;
}

/**
 * The pulses on the ports with DisplayPort, Linux's
 * i915_digport_work_func: each handled by intel_dp_hpd_pulse; the pins
 * whose pulse asks for a probe join `*event_bits`.
 */
static void digport_work(struct drm_i915_private *i915, uint32_t long_mask, uint32_t short_mask,
                         uint32_t *event_bits)
{
    int pin;

    for (pin = 0; pin < HPD_NUM_PINS; pin++) {
        struct intel_ddi_port *p = pin_port(i915, pin);
        uint32_t bit = 1u << pin;

        if (!p || !p->has_dp || !((long_mask | short_mask) & bit)) {
            continue;
        }
        if (intel_dp_hpd_pulse(&p->dp, (long_mask & bit) != 0)) {
            *event_bits |= bit;
        }
    }
}

/**
 * Turn the pins a storm marked off and poll their connectors, Linux's
 * intel_hpd_irq_storm_switch_to_polling; once HPD_STORM_REENABLE_MS has
 * passed, Linux's intel_hpd_irq_storm_reenable_work, they come back on.
 */
static void storm_switch(struct drm_i915_private *i915, uint64_t now)
{
    struct intel_hotplug *hp = &i915->display.hotplug;
    uint32_t flags, back = 0;
    int pin, disabled = 0;

    flags = spin_lock_irqsave(&i915->irq.lock);
    for (pin = 0; pin < HPD_NUM_PINS; pin++) {
        if (hp->state[pin] == HPD_MARK_DISABLED) {
            hp->state[pin] = HPD_DISABLED;
            disabled = 1;
            klog(KLOG_WARN, "i915", "hotplug: storm on port %c; polling it", 'B' + pin);
        } else if (hp->state[pin] == HPD_DISABLED && hp->reenable_at && now >= hp->reenable_at) {
            hp->state[pin] = HPD_ENABLED;
            back |= sde_bit[pin];
        }
    }
    spin_unlock_irqrestore(&i915->irq.lock, flags);
    if (back) {
        hp->reenable_at = 0;
        i915_irq_unmask(i915, I915_IRQ_SDE, back);
        klog(KLOG_INFO, "i915", "hotplug: storm over; pins on again");
    }
    if (disabled) {
        hp->reenable_at = now + MS_TO_TICKS(HPD_STORM_REENABLE_MS);
        if (hp->poll_at == 0) {
            hp->poll_at = now + MS_TO_TICKS(HPD_POLL_MS);
        }
    }
}

/**
 * Poll the connectors of the pins a storm turned off, Linux's
 * output_poll_execute: a forced connector is left alone, and each other's
 * status taken; 1 for each pin whose connector changed.
 */
static uint32_t poll_disabled(struct drm_i915_private *i915)
{
    struct intel_hotplug *hp = &i915->display.hotplug;
    uint32_t changed = 0;
    int pin, i;

    for (pin = 0; pin < HPD_NUM_PINS; pin++) {
        struct intel_ddi_port *p = pin_port(i915, pin);
        struct drm_connector *cs[2];

        if (!p || hp->state[pin] != HPD_DISABLED) {
            continue;
        }
        port_connectors(p, cs);
        for (i = 0; i < 2; i++) {
            if (cs[i] && cs[i]->force == DRM_FORCE_UNSPECIFIED && detect_connector(p, cs[i])) {
                drm_connector_hotplug_later(cs[i]);
                changed |= 1u << pin;
            }
        }
    }
    return changed;
}

/** Set again each pipe whose DisplayPort link lost its training, Linux's
 * intel_dp_retrain_link; a pipe whose flip is still pending is tried
 * again on the next run. 1 when one waits. */
static int retrain_links(struct drm_i915_private *i915)
{
    int pin, again = 0;

    for (pin = 0; pin < HPD_NUM_PINS; pin++) {
        struct intel_ddi_port *p = pin_port(i915, pin);
        struct intel_crtc *c;
        int32_t err;

        if (!p || !p->has_dp || !p->dp.retrain) {
            continue;
        }
        c = intel_dp_active_crtc(&p->dp);
        if (!c) {
            p->dp.retrain = 0;
            continue;
        }
        err = intel_display_commit_pipe(c);
        if (err == -EBUSY) {
            again = 1;
            continue;
        }
        p->dp.retrain = 0;
        if (err != 0) {
            klog(KLOG_WARN, "i915", "%s: retrain of pipe %c failed: error %d", p->dp.name,
                 'A' + c->pipe, (int)err);
        }
    }
    return again;
}

/** The sooner of two times, 0 standing for none. */
static uint64_t sooner(uint64_t a, uint64_t b)
{
    return a == 0 ? b : b == 0 || a < b ? a : b;
}

/** intel_hotplug_work's body, under the device's mode_lock. */
static void hotplug_work_locked(struct drm_work *work)
{
    struct drm_i915_private *i915 = to_i915(work->dev);
    struct intel_hotplug *hp = &i915->display.hotplug;
    uint64_t now = rtc_ticks_now(), next;
    uint32_t flags, long_mask, short_mask, event_bits, retry_bits, changed = 0, retry = 0;
    int pin, i, again;

    if (!hp->ready) {
        return;
    }
    flags = spin_lock_irqsave(&i915->irq.lock);
    long_mask = hp->long_mask;
    short_mask = hp->short_mask;
    hp->long_mask = 0;
    hp->short_mask = 0;
    event_bits = hp->event_bits;
    hp->event_bits = 0;
    spin_unlock_irqrestore(&i915->irq.lock, flags);
    digport_work(i915, long_mask, short_mask, &event_bits);

    retry_bits = 0;
    if (hp->retry_at && now >= hp->retry_at) {
        retry_bits = hp->retry_bits;
        hp->retry_bits = 0;
        hp->retry_at = 0;
    }
    storm_switch(i915, now);
    if (hp->ignore_long_hpd) {
        /* Linux's i915_hotplug_work_func returns here, the pins taken. */
        event_bits = 0;
        retry_bits = 0;
    }

    /* Linux's i915_hotplug_work_func with intel_ddi_hotplug: each
     * connector on a pin that pulsed probed, an event for each change, and
     * one more probe 1 s later of a connector that did not change. */
    for (pin = 0; pin < HPD_NUM_PINS; pin++) {
        struct intel_ddi_port *p = pin_port(i915, pin);
        uint32_t bit = 1u << pin;
        struct drm_connector *cs[2];

        if (!p || !((event_bits | retry_bits) & bit)) {
            continue;
        }
        if (event_bits & bit) {
            hp->retries[pin] = 0;
        } else if (hp->retries[pin] < 255u) {
            hp->retries[pin]++;
        }
        port_connectors(p, cs);
        for (i = 0; i < 2; i++) {
            if (!cs[i]) {
                continue;
            }
            if (detect_connector(p, cs[i])) {
                klog(KLOG_INFO, "i915", "hotplug: %s: %s", cs[i]->name,
                     cs[i]->status == DRM_CONNECTOR_CONNECTED ? "connected" : "disconnected");
                drm_connector_hotplug_later(cs[i]);
                changed |= bit;
            } else if (hp->retries[pin] < 1u) {
                retry |= bit;
            }
        }
    }
    retry &= ~changed;
    if (retry) {
        hp->retry_bits |= retry;
        hp->retry_at = now + MS_TO_TICKS(HPD_RETRY_MS);
    }
    if (hp->poll_at && now >= hp->poll_at) {
        (void)poll_disabled(i915);
        hp->poll_at = 0;
        for (pin = 0; pin < HPD_NUM_PINS; pin++) {
            if (hp->state[pin] == HPD_DISABLED) {
                hp->poll_at = now + MS_TO_TICKS(HPD_POLL_MS);
            }
        }
    }
    again = retrain_links(i915);

    next = sooner(sooner(hp->retry_at, hp->reenable_at), hp->poll_at);
    if (again) {
        next = sooner(next, now + MS_TO_TICKS(20u));
    }
    if (next) {
        drm_dev_work_later_ms(&i915->drm, &hp->work, next > now ? (uint32_t)(next - now) : 1u);
    }
}

/* Under the mode lock, as Linux's i915_hotplug_work_func takes
 * mode_config.mutex (intel_hotplug.c:431-495) and its probes the connection
 * lock: the probes, the connectors' status and the retrains' commits are
 * the mode state, which MODE_ATOMIC changes without the big lock. */
void intel_hotplug_work(struct drm_work *work)
{
    drm_modeset_lock_nointr(work->dev);
    hotplug_work_locked(work);
    drm_modeset_unlock(work->dev);
}

int intel_hotplug_live(struct drm_i915_private *i915, int pin)
{
    if (pin < 0 || pin >= HPD_NUM_PINS) {
        return 0;
    }
    return (i915_read(&i915->hw, SDEISR) & sde_bit[pin]) != 0;
}

int32_t intel_hotplug_init(struct drm_i915_private *i915)
{
    struct intel_hotplug *hp = &i915->display.hotplug;
    uint32_t bits, clear = 0, set = 0;
    int32_t err;
    int pin;

    hp->work.fn = intel_hotplug_work;
    for (pin = 0; pin < HPD_NUM_PINS; pin++) {
        hp->state[pin] = HPD_ENABLED;
        hp->storm_count[pin] = 0;
        hp->retries[pin] = 0;
        clear |= PORT_HOTPLUG_ENABLE(pin) | PORT_PULSE_DURATION_MASK(pin);
        if (pin_port(i915, pin)) {
            set |= PORT_HOTPLUG_ENABLE(pin);
        }
    }
    /* Linux's ibx_hotplug_enables: on a Lynx Point LP, in the processor's
     * package, the eDP port's pin is enabled in the PCH too. */
    clear |= PORTA_HOTPLUG_ENABLE;
    if (i915->display.dp.present && i915->id && i915->id->ult) {
        set |= PORTA_HOTPLUG_ENABLE;
    }
    i915_rmw(&i915->hw, PCH_PORT_HOTPLUG, clear, set);
    bits = pin_irqs(i915, 0);
    if (bits == 0) {
        return 0;
    }
    err = i915_irq_register(i915, I915_IRQ_SDE, bits, hpd_irq, NULL);
    if (err != 0) {
        return err;
    }
    hp->ready = 1;
    /* The first run starts the worker from process context, so that an
     * interrupt finds it running. */
    drm_dev_work_later(&i915->drm, &hp->work);
    i915_irq_unmask(i915, I915_IRQ_SDE, pin_irqs(i915, 1));
    return 0;
}

void intel_hotplug_fini(struct drm_i915_private *i915)
{
    struct intel_hotplug *hp = &i915->display.hotplug;
    uint32_t bits = pin_irqs(i915, 0);

    if (!hp->ready) {
        return;
    }
    i915_irq_unregister(i915, I915_IRQ_SDE, bits);
    hp->ready = 0;
    drm_dev_work_cancel(&hp->work);
}
