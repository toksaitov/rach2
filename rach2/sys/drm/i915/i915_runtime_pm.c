/*
 * i915_runtime_pm.c: runtime power management of Haswell's GPU, after
 * Linux 6.12's intel_runtime_pm.c and i915_driver.c's runtime suspend and
 * resume, with the part of the PM core (drivers/base/power/runtime.c and
 * sysfs.c) that i915 uses: the usage count, autosuspend, and the files of
 * the PCI function's power directory.
 *
 * A wakeref is held by whatever reaches the chip: the GT while it is
 * unparked, each running pipe, a commit, an ioctl or file of debugfs and
 * sysfs that reads registers, a connector's probe, a binding written into
 * the global GTT. The last put starts the autosuspend delay; the work that
 * runs after it takes the GEM lock, so that no buffer state changes under
 * the suspend, and suspends the device only if no wakeref was taken
 * meanwhile. A get resumes the device without the GEM lock, so that a
 * holder of it may take a wakeref. Neither change sleeps: the waits in
 * them are the hardware's, a few milliseconds at most, so that a get from
 * another thread spins until the change ends.
 */

#include "i915.h"

#include "../../arch/arch.h"
#include "../../fs/synthfs.h"
#include "../../fs/sysfs.h"
#include "../../klog.h"
#include "../../lib/kerrno.h"
#include "../../lib/kstd.h"
#include "../../lib/kstring.h"
#include "../../proc/percpu.h"
#include "../../rtc.h"
#include "../../timer.h"
#include "../../udelay.h"

/* Linux's defaults for an integrated GPU: autosuspend after 10 s idle
 * (intel_runtime_pm_enable), runtime PM allowed (pm_runtime_allow). */
#define I915_RPM_DELAY_MS 10000

/* How long a failed suspend waits before the next try, in milliseconds. */
#define I915_RPM_RETRY_MS 1000u

#define NS_PER_MS 1000000

/** Wait `ms` milliseconds without sleeping, for the PCI power changes. */
static void rpm_mdelay(unsigned int ms)
{
    k_udelay(ms * 1000u);
}

/** Add the time since `stamp_ns` to the active or suspended total, Linux's
 * update_pm_runtime_accounting. Under the lock. */
static void rpm_account(struct i915_runtime_pm *rpm)
{
    int64_t now = rtc_monotonic_ns();
    uint64_t d = now > rpm->stamp_ns ? (uint64_t)(now - rpm->stamp_ns) : 0;

    if (rpm->status == I915_RPM_SUSPENDED) {
        rpm->suspended_ns += d;
    } else {
        rpm->active_ns += d;
    }
    rpm->stamp_ns = now;
}

/** Set `status`, with the time before it accounted to the old one. Under
 * the lock. */
static void rpm_set_status(struct i915_runtime_pm *rpm, int status)
{
    rpm_account(rpm);
    rpm->status = (uint8_t)status;
}

/** Queue the autosuspend work `ms` milliseconds from now. Not under the
 * lock, since the queue takes the DRM core's. */
static void rpm_queue(struct drm_i915_private *i915, uint32_t ms)
{
    drm_dev_work_later_ms(&i915->drm, &i915->runtime_pm.work, ms);
}

/* ------------------------------------------------------------------------
 * The suspend and the resume
 */

/**
 * Point the global GTT's entries that were unbound while the device slept
 * at the scratch page, as the clears would have, then flush the GTT. A
 * lost range was logged when it was lost. The list takes no range once the
 * status has left SUSPENDED, so the resume reads it without the lock.
 */
static void rpm_replay_clears(struct drm_i915_private *i915)
{
    struct i915_runtime_pm *rpm = &i915->runtime_pm;
    uint32_t i, n, k, first;

    if (rpm->nclears == 0) {
        return;
    }
    for (i = 0; i < rpm->nclears; i++) {
        first = (uint32_t)(rpm->clears[i].start / I915_GTT_PAGE_SIZE);
        n = (uint32_t)((rpm->clears[i].end - rpm->clears[i].start) / I915_GTT_PAGE_SIZE);
        for (k = 0; k < n; k++) {
            i915_gtt_write(&i915->hw, first + k, i915->ggtt.scratch_pte);
        }
    }
    rpm->nclears = 0;
    i915_ggtt_invalidate(i915);
}

/**
 * Suspend the device, Linux's intel_runtime_suspend with the PCI core's
 * part around it: the mappings of the aperture revoked so that the next
 * access faults and resumes (i915_gem_runtime_suspend), the interrupts
 * off, forcewake released, hotplug polled, package C8 allowed, then the
 * function's state saved and the function put in D3hot. The caller holds
 * the GEM lock and the transition. 0, or -EBUSY with the device left as
 * it was when the display or forcewake is still held.
 */
static int32_t rpm_suspend(struct drm_i915_private *i915)
{
    struct i915_runtime_pm *rpm = &i915->runtime_pm;
    struct i915_vma *vma;
    int32_t err;

    if (i915->gt.awake) {
        return -EBUSY;
    }
    err = intel_pps_vdd_suspend(i915);
    if (err == 0) {
        err = i915_power_sync_idle(i915);
    }
    if (err < 0) {
        return err;
    }
    for (vma = i915->gem.bound; vma; vma = vma->ggtt_next) {
        i915_vma_revoke_mmap(vma);
        i915_vma_flush_writes(vma);
    }
    i915_irq_suspend(i915);
    i915_irq_sync(i915);
    err = i915_uncore_suspend(i915);
    if (err < 0) {
        i915_irq_resume(i915);
        return err;
    }
    intel_hotplug_suspend(i915);
    i915_pc8_enable(i915);
    pci_save_state(&i915->pci, &rpm->saved);
    i915->hw.asleep = 1;
    if (pci_set_power_state(&i915->pci, PCI_D3HOT, rpm_mdelay) < 0) {
        klog(KLOG_WARN, "i915", "rpm: the function did not take D3hot");
    }
    return 0;
}

/**
 * Resume the device, Linux's intel_runtime_resume with the PCI core's part
 * before it: the function in D0 with its state and message restored, then
 * package C8 left, forcewake's FIFO count read again, the interrupts on,
 * the fences, the swizzle and the GTT entries unbound meanwhile written,
 * and hotplug detection back with every output probed. The caller holds
 * the transition.
 */
static void rpm_resume(struct drm_i915_private *i915)
{
    struct i915_runtime_pm *rpm = &i915->runtime_pm;

    if (pci_set_power_state(&i915->pci, PCI_D0, rpm_mdelay) < 0) {
        klog(KLOG_WARN, "i915", "rpm: the function did not take D0");
    }
    pci_restore_state(&i915->pci, &rpm->saved);
    if (i915->msi_vector >= 0) {
        (void)pci_msi_enable(&i915->pci, (uint8_t)i915->msi_vector);
    }
    i915->hw.asleep = 0;
    i915_pc8_disable(i915);
    i915_uncore_resume(i915);
    i915_irq_resume(i915);
    i915_gem_restore_fences(i915);
    rpm_replay_clears(i915);
    intel_hotplug_resume(i915);
}

/* ------------------------------------------------------------------------
 * Wakerefs
 */

/** Whether the autosuspend may suspend now: available, enabled, awake and
 * no wakeref held. Under the lock. */
static int rpm_may_suspend(const struct i915_runtime_pm *rpm)
{
    return rpm->available && rpm->enabled && rpm->usage == 0 && rpm->status == I915_RPM_ACTIVE;
}

void i915_rpm_get(struct drm_i915_private *i915)
{
    struct i915_runtime_pm *rpm = &i915->runtime_pm;
    void *self = current_proc;
    uint32_t flags;

    for (;;) {
        flags = spin_lock_irqsave(&rpm->lock);
        if (rpm->status == I915_RPM_ACTIVE || rpm->owner == self) {
            rpm->usage++;
            spin_unlock_irqrestore(&rpm->lock, flags);
            return;
        }
        if (rpm->status == I915_RPM_SUSPENDED) {
            break;
        }
        spin_unlock_irqrestore(&rpm->lock, flags);
        arch_relax();
    }
    rpm->usage++;
    rpm_set_status(rpm, I915_RPM_RESUMING);
    rpm->owner = self;
    spin_unlock_irqrestore(&rpm->lock, flags);

    rpm_resume(i915);

    flags = spin_lock_irqsave(&rpm->lock);
    rpm_set_status(rpm, I915_RPM_ACTIVE);
    rpm->owner = NULL;
    rpm->resumes++;
    spin_unlock_irqrestore(&rpm->lock, flags);
}

int i915_rpm_get_if_active(struct drm_i915_private *i915)
{
    struct i915_runtime_pm *rpm = &i915->runtime_pm;
    void *self = current_proc;
    uint32_t flags;

    for (;;) {
        flags = spin_lock_irqsave(&rpm->lock);
        if (rpm->status == I915_RPM_ACTIVE || rpm->owner == self) {
            rpm->usage++;
            spin_unlock_irqrestore(&rpm->lock, flags);
            return 1;
        }
        if (rpm->status == I915_RPM_SUSPENDED) {
            spin_unlock_irqrestore(&rpm->lock, flags);
            return 0;
        }
        spin_unlock_irqrestore(&rpm->lock, flags);
        arch_relax();
    }
}

void i915_rpm_put(struct drm_i915_private *i915)
{
    struct i915_runtime_pm *rpm = &i915->runtime_pm;
    uint32_t flags = spin_lock_irqsave(&rpm->lock);
    int32_t delay;
    int idle;

    if (rpm->usage == 0) {
        spin_unlock_irqrestore(&rpm->lock, flags);
        klog(KLOG_ERR, "i915", "rpm: put with no wakeref held");
        return;
    }
    rpm->usage--;
    idle = rpm_may_suspend(rpm);
    if (rpm->usage == 0) {
        rpm->last_busy_ns = rtc_monotonic_ns();
    }
    delay = rpm->autosuspend_delay_ms;
    spin_unlock_irqrestore(&rpm->lock, flags);
    if (idle) {
        rpm_queue(i915, delay > 0 ? (uint32_t)delay : 0u);
    }
}

uint32_t i915_rpm_usage(struct drm_i915_private *i915)
{
    return i915->runtime_pm.usage;
}

int i915_rpm_get_or_defer_clear(struct drm_i915_private *i915, uint64_t offset, uint64_t size)
{
    struct i915_runtime_pm *rpm = &i915->runtime_pm;
    void *self = current_proc;
    uint32_t flags;
    int lost = 0;

    for (;;) {
        flags = spin_lock_irqsave(&rpm->lock);
        if (rpm->status == I915_RPM_ACTIVE || rpm->owner == self) {
            rpm->usage++;
            spin_unlock_irqrestore(&rpm->lock, flags);
            return 1;
        }
        if (rpm->status == I915_RPM_SUSPENDED) {
            break;
        }
        spin_unlock_irqrestore(&rpm->lock, flags);
        arch_relax();
    }
    if (rpm->nclears < I915_RPM_CLEARS) {
        rpm->clears[rpm->nclears].start = offset;
        rpm->clears[rpm->nclears].end = offset + size;
        rpm->nclears++;
    } else {
        rpm->clears_lost++;
        lost = 1;
    }
    spin_unlock_irqrestore(&rpm->lock, flags);
    if (lost) {
        klog(KLOG_WARN, "i915",
             "rpm: GTT clear of 0x%x bytes at 0x%x lost while suspended; the entries stay",
             (unsigned)size, (unsigned)offset);
    }
    return 0;
}

/**
 * The autosuspend, Linux's rpm_suspend from pm_runtime_put_autosuspend's
 * work: the GEM lock first, then, if the device may suspend and the delay
 * has passed since the last put, the suspend; before the delay, the work
 * again when it ends. A suspend that fails leaves the device active and
 * tries again after a second while nothing holds it.
 */
static void rpm_work(struct drm_work *work)
{
    struct drm_i915_private *i915 = to_i915(work->dev);
    struct i915_runtime_pm *rpm = &i915->runtime_pm;
    int64_t left;
    uint32_t flags;
    int32_t err;
    int taken = i915_gem_lock(i915), retry;

    flags = spin_lock_irqsave(&rpm->lock);
    if (!rpm_may_suspend(rpm) || rpm->autosuspend_delay_ms < 0) {
        spin_unlock_irqrestore(&rpm->lock, flags);
        i915_gem_unlock(i915, taken);
        return;
    }
    left = rpm->last_busy_ns + (int64_t)rpm->autosuspend_delay_ms * NS_PER_MS - rtc_monotonic_ns();
    if (left > 0) {
        spin_unlock_irqrestore(&rpm->lock, flags);
        i915_gem_unlock(i915, taken);
        rpm_queue(i915, (uint32_t)tick_div((uint64_t)left + NS_PER_MS - 1, NS_PER_MS, NULL));
        return;
    }
    rpm_set_status(rpm, I915_RPM_SUSPENDING);
    rpm->owner = current_proc;
    spin_unlock_irqrestore(&rpm->lock, flags);

    err = rpm_suspend(i915);

    flags = spin_lock_irqsave(&rpm->lock);
    rpm_set_status(rpm, err == 0 ? I915_RPM_SUSPENDED : I915_RPM_ACTIVE);
    rpm->owner = NULL;
    if (err == 0) {
        rpm->suspends++;
    } else {
        rpm->failures++;
    }
    retry = err != 0 && rpm_may_suspend(rpm);
    spin_unlock_irqrestore(&rpm->lock, flags);
    i915_gem_unlock(i915, taken);
    if (err != 0) {
        klog(KLOG_INFO, "i915", "rpm: not suspended: error %d", (int)err);
    }
    if (retry) {
        rpm_queue(i915, I915_RPM_RETRY_MS);
    }
}

/* ------------------------------------------------------------------------
 * The power directory, Linux's drivers/base/power/sysfs.c
 */

/* The files, by `arg`. */
#define RPM_CONTROL   0
#define RPM_DELAY     1
#define RPM_STATUS    2
#define RPM_ACTIVE_MS 3
#define RPM_SUSP_MS   4
#define RPM_ENABLED   5
#define RPM_USAGE     6

/** Write the text `s` and a newline into `buf`, `cap` bytes; its length,
 * which may pass `cap`. */
static int32_t emit_line(char *buf, uint32_t cap, const char *s)
{
    uint32_t n = (uint32_t)k_strlen(s), i;

    for (i = 0; i < n && i < cap; i++) {
        buf[i] = s[i];
    }
    if (n < cap) {
        buf[n] = '\n';
    }
    return (int32_t)(n + 1u);
}

/** Write `v`, negative when `neg`, in decimal and a newline, as "%lld\n". */
static int32_t emit_num(char *buf, uint32_t cap, uint64_t v, int neg)
{
    char digits[24];
    int n = 0;

    if (neg) {
        digits[n++] = '-';
    }
    {
        char rev[21];
        int m = 0;

        do {
            uint32_t r;

            v = tick_div(v, 10u, &r);
            rev[m++] = (char)('0' + r);
        } while (v);
        while (m > 0) {
            digits[n++] = rev[--m];
        }
    }
    digits[n] = '\0';
    return emit_line(buf, cap, digits);
}

/** A file of the power directory. */
static int32_t rpm_show(void *obj, int arg, char *buf, uint32_t cap)
{
    struct drm_i915_private *i915 = (struct drm_i915_private *)obj;
    struct i915_runtime_pm *rpm = &i915->runtime_pm;
    static const char *const names[] = {"active", "resuming", "suspended", "suspending"};
    uint32_t flags = spin_lock_irqsave(&rpm->lock);
    int32_t delay = rpm->autosuspend_delay_ms;
    uint64_t ms;
    const char *s;

    rpm_account(rpm);
    switch (arg) {
    case RPM_CONTROL:
        s = rpm->forbidden ? "on" : "auto";
        break;
    case RPM_STATUS:
        s = !rpm->enabled ? "unsupported" : names[rpm->status];
        break;
    case RPM_ENABLED:
        s = !rpm->enabled && rpm->forbidden ? "disabled & forbidden"
            : !rpm->enabled                 ? "disabled"
            : rpm->forbidden                ? "forbidden"
                                            : "enabled";
        break;
    case RPM_ACTIVE_MS:
    case RPM_SUSP_MS:
        ms = tick_div(arg == RPM_ACTIVE_MS ? rpm->active_ns : rpm->suspended_ns, NS_PER_MS, NULL);
        spin_unlock_irqrestore(&rpm->lock, flags);
        return emit_num(buf, cap, ms, 0);
    case RPM_USAGE:
        ms = rpm->usage;
        spin_unlock_irqrestore(&rpm->lock, flags);
        return emit_num(buf, cap, ms, 0);
    default:
        spin_unlock_irqrestore(&rpm->lock, flags);
        return emit_num(buf, cap, delay < 0 ? (uint64_t)(-(int64_t)delay) : (uint64_t)delay,
                        delay < 0);
    }
    spin_unlock_irqrestore(&rpm->lock, flags);
    return emit_line(buf, cap, s);
}

/**
 * Change `forbidden` or the delay under the lock, with the wakeref each
 * holds while it prevents suspend taken or put, as Linux's
 * pm_runtime_forbid, pm_runtime_allow and update_autosuspend do: a new
 * hold resumes the device, and one let go starts the delay.
 */
static void rpm_set(struct drm_i915_private *i915, int forbidden, int32_t delay)
{
    struct i915_runtime_pm *rpm = &i915->runtime_pm;
    uint32_t flags = spin_lock_irqsave(&rpm->lock);
    int held = rpm->forbidden + (rpm->autosuspend_delay_ms < 0);
    int hold = (forbidden != 0) + (delay < 0);

    rpm->forbidden = (uint8_t)(forbidden != 0);
    rpm->autosuspend_delay_ms = delay;
    spin_unlock_irqrestore(&rpm->lock, flags);
    while (hold > held) {
        i915_rpm_get(i915);
        held++;
    }
    while (hold < held) {
        i915_rpm_put(i915);
        held--;
    }
    /* A shorter delay takes effect now, as update_autosuspend's rpm_idle. */
    flags = spin_lock_irqsave(&rpm->lock);
    held = rpm_may_suspend(rpm);
    spin_unlock_irqrestore(&rpm->lock, flags);
    if (held) {
        rpm_queue(i915, 0);
    }
}

/** Whether the `n` bytes at `buf` are the word `w`. */
static int is_word(const char *buf, uint32_t n, const char *w)
{
    uint32_t i;

    for (i = 0; i < n && w[i] && buf[i] == w[i]; i++) {
    }
    return i == n && w[i] == '\0';
}

/** A write of control, "on" or "auto", or of autosuspend_delay_ms, a
 * signed decimal number, as Linux's control_store and
 * autosuspend_delay_ms_store take them. */
static int32_t rpm_store(void *obj, int arg, const char *buf, uint32_t len)
{
    struct drm_i915_private *i915 = (struct drm_i915_private *)obj;
    struct i915_runtime_pm *rpm = &i915->runtime_pm;
    uint32_t n = len;
    long long v;

    if (arg == RPM_DELAY) {
        if (k_strtoll(buf, len, &v) != 0 || v != (long long)(int32_t)v) {
            return -EINVAL;
        }
        rpm_set(i915, rpm->forbidden, (int32_t)v);
        return (int32_t)len;
    }
    if (n > 0 && buf[n - 1] == '\n') {
        n--;
    }
    if (is_word(buf, n, "on")) {
        rpm_set(i915, 1, rpm->autosuspend_delay_ms);
    } else if (is_word(buf, n, "auto")) {
        rpm_set(i915, 0, rpm->autosuspend_delay_ms);
    } else {
        return -EINVAL;
    }
    return (int32_t)len;
}

/* Linux's power_attrs and runtime_attrs for a device that uses
 * autosuspend. */
static const struct sysfs_attr rpm_attrs[] = {{"control", 0644u, rpm_show, rpm_store},
                                              {"autosuspend_delay_ms", 0644u, rpm_show, rpm_store},
                                              {"runtime_status", 0444u, rpm_show, NULL},
                                              {"runtime_active_time", 0444u, rpm_show, NULL},
                                              {"runtime_suspended_time", 0444u, rpm_show, NULL},
                                              {"runtime_enabled", 0444u, rpm_show, NULL},
                                              {"runtime_usage", 0444u, rpm_show, NULL}};

/** Make the PCI function's power directory with its files; logged when it
 * cannot be made, and the device then keeps the defaults. */
static void rpm_sysfs_add(struct drm_i915_private *i915)
{
    const pci_device_t *pdev = &i915->pci;
    int key = pdev->bus << 8 | pdev->device << 3 | pdev->function;
    int anchor = sysfs_anchor(SYSFS_PCI_DEVICE, key), dir, h;
    unsigned i;

    dir = anchor < 0 ? anchor : sysfs_add_dir(anchor, "power");
    if (dir < 0) {
        klog(KLOG_WARN, "i915", "rpm: power directory not made: error %d", dir);
        return;
    }
    for (i = 0; i < sizeof(rpm_attrs) / sizeof(rpm_attrs[0]); i++) {
        h = sysfs_add_attr(dir, &rpm_attrs[i], i915, (int)i);
        if (h < 0) {
            klog(KLOG_WARN, "i915", "rpm: power/%s not added: error %d", rpm_attrs[i].name, h);
        }
    }
    i915->runtime_pm.sysfs_dir = dir;
}

/* ------------------------------------------------------------------------
 * The bind and the unplug
 */

void i915_rpm_init(struct drm_i915_private *i915)
{
    struct i915_runtime_pm *rpm = &i915->runtime_pm;

    spin_lock_init(&rpm->lock, "i915_rpm");
    rpm->usage = 1;
    rpm->status = I915_RPM_ACTIVE;
    rpm->available = (uint8_t)i915_hw_is_mmio(&i915->hw);
    rpm->autosuspend_delay_ms = I915_RPM_DELAY_MS;
    rpm->stamp_ns = rtc_monotonic_ns();
    rpm->last_busy_ns = rpm->stamp_ns;
    rpm->work.fn = rpm_work;
}

void i915_rpm_enable(struct drm_i915_private *i915)
{
    struct i915_runtime_pm *rpm = &i915->runtime_pm;
    uint32_t flags;

    if (rpm->available) {
        rpm_sysfs_add(i915);
    }
    flags = spin_lock_irqsave(&rpm->lock);
    rpm->enabled = 1;
    spin_unlock_irqrestore(&rpm->lock, flags);
    i915_rpm_put(i915);
}

void i915_rpm_disable(struct drm_i915_private *i915)
{
    struct i915_runtime_pm *rpm = &i915->runtime_pm;
    uint32_t flags;

    i915_rpm_get(i915);
    flags = spin_lock_irqsave(&rpm->lock);
    rpm->enabled = 0;
    spin_unlock_irqrestore(&rpm->lock, flags);
    (void)drm_dev_work_cancel(&rpm->work);
    if (rpm->sysfs_dir > 0) {
        sysfs_remove(rpm->sysfs_dir);
        rpm->sysfs_dir = 0;
    }
}
