/*
 * pm_runtime.c: runtime power management of a device, after Linux 6.12's
 * drivers/base/power/runtime.c and the runtime part of its sysfs.c.
 */

#include "pm_runtime.h"

#include "arch/arch.h"
#include "fs/sysfs.h"
#include "klog.h"
#include "lib/kerrno.h"
#include "lib/kstd.h"
#include "proc/percpu.h"
#include "rtc.h"
#include "timer.h"

/* How long a sleep refused waits before it is tried again: -EAGAIN for a
 * race that passes at once, -EBUSY for a state that lasts a while. */
#define RPM_RETRY_MS 10u
#define RPM_BUSY_MS  100u

/** The wait before a sleep refused with `err` is tried again. */
static int32_t retry_ms(int32_t err)
{
    return (int32_t)(err == -EAGAIN ? RPM_RETRY_MS : RPM_BUSY_MS);
}

/** The process running now, which a change of state records as its mover,
 * or the processor before the first process runs: never NULL, which
 * `mover` holds when no change is under way. */
static const void *rpm_current(void)
{
    const void *self = (const void *)current_proc;

    return self ? self : (const void *)this_cpu();
}

/** Add the time since the last account to the state's total. Under the
 * record's lock. */
static void account(struct pm_runtime *rpm, int64_t now)
{
    uint64_t delta = now > rpm->accounted ? (uint64_t)(now - rpm->accounted) : 0u;

    if (rpm->status == RPM_ACTIVE || rpm->status == RPM_SUSPENDING) {
        rpm->active_ns += delta;
    } else {
        rpm->suspended_ns += delta;
    }
    rpm->accounted = now;
}

/** Change the state to `status`, the times counted up to now. Under the
 * record's lock. */
static void set_status(struct pm_runtime *rpm, int status)
{
    account(rpm, rtc_monotonic_ns());
    rpm->status = status;
}

/** Whether a sleep may start now but for its time: no hold, sleeps
 * enabled, no error and a delay that is not negative, the device awake.
 * Under the record's lock. */
static int may_sleep(const struct pm_runtime *rpm)
{
    return rpm->usage == 0 && rpm->enabled && !rpm->error && rpm->delay_ms >= 0 &&
           rpm->status == RPM_ACTIVE;
}

/** The milliseconds until the sleep is due, 0 when it is due now. Under
 * the record's lock. */
static uint32_t due_in_ms(const struct pm_runtime *rpm, int64_t now)
{
    int64_t due = rpm->last_busy + (int64_t)rpm->delay_ms * 1000000;

    if (rpm->busy_until > due) {
        due = rpm->busy_until;
    }
    if (due <= now) {
        return 0;
    }
    return (uint32_t)tick_div64((uint64_t)(due - now) + 999999u, 1000000u, NULL);
}

void pm_runtime_init(struct pm_runtime *rpm, const struct pm_runtime_ops *ops, void *owner)
{
    spin_lock_init(&rpm->lock, "pm_runtime");
    rpm->ops = ops;
    rpm->owner = owner;
    rpm->usage = 1;
    rpm->status = RPM_ACTIVE;
    rpm->enabled = 0;
    rpm->forbidden = 0;
    rpm->error = 0;
    rpm->delay_ms = 0;
    rpm->last_busy = rtc_monotonic_ns();
    rpm->busy_until = 0;
    rpm->accounted = rpm->last_busy;
    rpm->active_ns = 0;
    rpm->suspended_ns = 0;
    rpm->mover = 0;
    rpm->suspends = 0;
    rpm->resumes = 0;
    rpm->dir = 0;
}

/** Ask the owner for the idle work once a sleep is due, if one may start.
 * Called without the record's lock, with `ms` from `due_in_ms` or -1 for
 * none. */
static void schedule_idle(struct pm_runtime *rpm, int32_t ms)
{
    if (ms >= 0 && rpm->ops->schedule) {
        rpm->ops->schedule(rpm->owner, (uint32_t)ms);
    }
}

/** The milliseconds to ask the idle work for, or -1 when no sleep may
 * start. Under the record's lock. */
static int32_t idle_after(const struct pm_runtime *rpm)
{
    return may_sleep(rpm) ? (int32_t)due_in_ms(rpm, rtc_monotonic_ns()) : -1;
}

void pm_runtime_enable(struct pm_runtime *rpm)
{
    uint32_t flags = spin_lock_irqsave(&rpm->lock);
    int32_t ms;

    rpm->enabled = 1;
    if (rpm->usage > 0) {
        rpm->usage--;
    }
    rpm->last_busy = rtc_monotonic_ns();
    ms = idle_after(rpm);
    spin_unlock_irqrestore(&rpm->lock, flags);
    schedule_idle(rpm, ms);
}

/** Record the error `err` of a callback, which stops the sleeps, and log
 * it. Under the record's lock. */
static void record_error(struct pm_runtime *rpm, const char *what, int32_t err)
{
    rpm->error = err;
    klog(KLOG_ERR, "pm", "runtime: %s failed: error %d; sleeps stopped", what, (int)err);
}

int32_t pm_runtime_get_sync(struct pm_runtime *rpm)
{
    uint32_t flags;
    int32_t err;

    for (;;) {
        flags = spin_lock_irqsave(&rpm->lock);
        if (rpm->status == RPM_ACTIVE || rpm->mover == rpm_current()) {
            rpm->usage++;
            spin_unlock_irqrestore(&rpm->lock, flags);
            return 0;
        }
        if (rpm->status == RPM_SUSPENDED) {
            break;
        }
        /* Another process suspends or resumes it; neither sleeps. */
        spin_unlock_irqrestore(&rpm->lock, flags);
        arch_relax();
    }
    set_status(rpm, RPM_RESUMING);
    rpm->mover = rpm_current();
    rpm->usage++;
    spin_unlock_irqrestore(&rpm->lock, flags);
    err = rpm->ops->resume(rpm->owner);
    flags = spin_lock_irqsave(&rpm->lock);
    rpm->mover = 0;
    set_status(rpm, RPM_ACTIVE);
    rpm->resumes++;
    rpm->last_busy = rtc_monotonic_ns();
    if (err != 0) {
        record_error(rpm, "resume", err);
    }
    spin_unlock_irqrestore(&rpm->lock, flags);
    return err;
}

int pm_runtime_get_if_active(struct pm_runtime *rpm)
{
    uint32_t flags = spin_lock_irqsave(&rpm->lock);
    int got = rpm->status == RPM_ACTIVE;

    if (got) {
        rpm->usage++;
    }
    spin_unlock_irqrestore(&rpm->lock, flags);
    return got;
}

void pm_runtime_get_noresume(struct pm_runtime *rpm)
{
    uint32_t flags = spin_lock_irqsave(&rpm->lock);

    rpm->usage++;
    spin_unlock_irqrestore(&rpm->lock, flags);
}

void pm_runtime_put(struct pm_runtime *rpm)
{
    uint32_t flags = spin_lock_irqsave(&rpm->lock);
    int32_t ms = -1;

    if (rpm->usage <= 0) {
        spin_unlock_irqrestore(&rpm->lock, flags);
        klog(KLOG_ERR, "pm", "runtime: put with no hold");
        return;
    }
    rpm->usage--;
    rpm->last_busy = rtc_monotonic_ns();
    if (rpm->usage == 0) {
        ms = idle_after(rpm);
    }
    spin_unlock_irqrestore(&rpm->lock, flags);
    schedule_idle(rpm, ms);
}

void pm_runtime_mark_busy(struct pm_runtime *rpm, uint32_t ms)
{
    uint32_t flags = spin_lock_irqsave(&rpm->lock);
    int64_t until = rtc_monotonic_ns() + (int64_t)ms * 1000000;
    int32_t after;

    if (until > rpm->busy_until) {
        rpm->busy_until = until;
    }
    after = idle_after(rpm);
    spin_unlock_irqrestore(&rpm->lock, flags);
    schedule_idle(rpm, after);
}

void pm_runtime_allow(struct pm_runtime *rpm)
{
    uint32_t flags = spin_lock_irqsave(&rpm->lock);
    int was = rpm->forbidden;

    rpm->forbidden = 0;
    spin_unlock_irqrestore(&rpm->lock, flags);
    if (was) {
        pm_runtime_put(rpm);
    }
}

int32_t pm_runtime_forbid(struct pm_runtime *rpm)
{
    uint32_t flags = spin_lock_irqsave(&rpm->lock);
    int was = rpm->forbidden;

    rpm->forbidden = 1;
    rpm->error = 0;
    spin_unlock_irqrestore(&rpm->lock, flags);
    return was ? 0 : pm_runtime_get_sync(rpm);
}

void pm_runtime_set_delay(struct pm_runtime *rpm, int32_t ms)
{
    uint32_t flags = spin_lock_irqsave(&rpm->lock);
    int32_t after;

    rpm->delay_ms = ms;
    after = idle_after(rpm);
    spin_unlock_irqrestore(&rpm->lock, flags);
    schedule_idle(rpm, after);
}

int32_t pm_runtime_disable(struct pm_runtime *rpm)
{
    uint32_t flags = spin_lock_irqsave(&rpm->lock);

    rpm->enabled = 0;
    spin_unlock_irqrestore(&rpm->lock, flags);
    return pm_runtime_get_sync(rpm);
}

/** Whether the sleep may start now: `may_sleep` and its time has come;
 * otherwise `*again` is the milliseconds to ask the work for, or -1. Under
 * the record's lock. */
static int sleep_now(const struct pm_runtime *rpm, int32_t *again)
{
    uint32_t ms;

    *again = -1;
    if (!may_sleep(rpm)) {
        return 0;
    }
    ms = due_in_ms(rpm, rtc_monotonic_ns());
    if (ms != 0) {
        *again = (int32_t)ms;
        return 0;
    }
    return 1;
}

void pm_runtime_idle(struct pm_runtime *rpm)
{
    uint32_t flags = spin_lock_irqsave(&rpm->lock);
    int32_t again, err;

    if (!sleep_now(rpm, &again)) {
        spin_unlock_irqrestore(&rpm->lock, flags);
        schedule_idle(rpm, again);
        return;
    }
    spin_unlock_irqrestore(&rpm->lock, flags);
    /* The device is awake meanwhile, so a get in `prepare`'s sleep waits
     * for nothing; one taken then is seen below. */
    err = rpm->ops->prepare ? rpm->ops->prepare(rpm->owner) : 0;
    flags = spin_lock_irqsave(&rpm->lock);
    if (err != 0 || !sleep_now(rpm, &again)) {
        spin_unlock_irqrestore(&rpm->lock, flags);
        schedule_idle(rpm, err != 0 ? retry_ms(err) : again);
        return;
    }
    set_status(rpm, RPM_SUSPENDING);
    rpm->mover = rpm_current();
    spin_unlock_irqrestore(&rpm->lock, flags);
    err = rpm->ops->suspend(rpm->owner);
    flags = spin_lock_irqsave(&rpm->lock);
    rpm->mover = 0;
    again = -1;
    if (err == 0) {
        set_status(rpm, RPM_SUSPENDED);
        rpm->suspends++;
    } else {
        set_status(rpm, RPM_ACTIVE);
        if (err == -EBUSY || err == -EAGAIN) {
            again = retry_ms(err);
        } else {
            record_error(rpm, "suspend", err);
        }
    }
    spin_unlock_irqrestore(&rpm->lock, flags);
    schedule_idle(rpm, again);
}

int pm_runtime_status(struct pm_runtime *rpm)
{
    uint32_t flags = spin_lock_irqsave(&rpm->lock);
    int status = rpm->status;

    spin_unlock_irqrestore(&rpm->lock, flags);
    return status;
}

int32_t pm_runtime_usage(struct pm_runtime *rpm)
{
    uint32_t flags = spin_lock_irqsave(&rpm->lock);
    int32_t usage = rpm->usage;

    spin_unlock_irqrestore(&rpm->lock, flags);
    return usage;
}

/* ------------------------------------------------------------------------
 * The power directory in /sys
 */

/* The attributes, by their `arg`. */
#define ATTR_CONTROL   0
#define ATTR_DELAY     1
#define ATTR_STATUS    2
#define ATTR_ACTIVE    3
#define ATTR_SUSPENDED 4

/** Append the NUL-terminated `s` to the `cap` bytes at `buf`, as far as it
 * goes, counting its whole length in `*len`. */
static void emit(char *buf, uint32_t cap, uint32_t *len, const char *s)
{
    while (*s) {
        if (*len + 1u < cap) {
            buf[*len] = *s;
            buf[*len + 1u] = '\0';
        }
        (*len)++;
        s++;
    }
}

/** Append the decimal `v` and a newline, as `emit` does. */
static void emit_u64(char *buf, uint32_t cap, uint32_t *len, uint64_t v)
{
    char digits[24];
    int i = (int)sizeof(digits) - 1;

    digits[i] = '\0';
    digits[--i] = '\n';
    do {
        uint32_t digit;

        v = tick_div(v, 10u, &digit);
        digits[--i] = (char)('0' + (int)digit);
    } while (v != 0);
    emit(buf, cap, len, &digits[i]);
}

/** runtime_status's word, Linux's runtime_status_show. Under the record's
 * lock. */
static const char *status_word(const struct pm_runtime *rpm)
{
    static const char *const words[] = {"active", "resuming", "suspended", "suspending"};

    if (rpm->error) {
        return "error";
    }
    if (!rpm->enabled) {
        return "unsupported";
    }
    return words[rpm->status];
}

/** Show an attribute of the record `obj`, by `arg`. */
static int32_t power_show(void *obj, int arg, char *buf, uint32_t cap)
{
    struct pm_runtime *rpm = (struct pm_runtime *)obj;
    uint32_t len = 0, flags = spin_lock_irqsave(&rpm->lock);
    const char *word = NULL;
    int64_t delay = rpm->delay_ms;
    uint64_t ns = 0;

    account(rpm, rtc_monotonic_ns());
    if (arg == ATTR_CONTROL) {
        word = rpm->forbidden ? "on\n" : "auto\n";
    } else if (arg == ATTR_STATUS) {
        word = status_word(rpm);
    } else {
        ns = arg == ATTR_ACTIVE ? rpm->active_ns : rpm->suspended_ns;
    }
    spin_unlock_irqrestore(&rpm->lock, flags);
    if (arg == ATTR_STATUS) {
        emit(buf, cap, &len, word);
        emit(buf, cap, &len, "\n");
    } else if (word) {
        emit(buf, cap, &len, word);
    } else if (arg == ATTR_DELAY && delay < 0) {
        emit(buf, cap, &len, "-");
        emit_u64(buf, cap, &len, (uint64_t)-delay);
    } else {
        emit_u64(buf, cap, &len,
                 arg == ATTR_DELAY ? (uint64_t)delay : tick_div64(ns, 1000000u, NULL));
    }
    return (int32_t)len;
}

/** Whether the `len` bytes at `buf`, less one newline at their end, are the
 * word `w`, as Linux's sysfs_streq compares them. */
static int word_is(const char *buf, uint32_t len, const char *w)
{
    uint32_t i;

    if (len > 0 && buf[len - 1] == '\n') {
        len--;
    }
    for (i = 0; i < len; i++) {
        if (w[i] != buf[i]) {
            return 0;
        }
    }
    return w[len] == '\0';
}

/** Store `control` or `autosuspend_delay_ms`, as Linux's control_store and
 * autosuspend_delay_ms_store: "on" or "auto", or a number of milliseconds.
 * `len`, or -EINVAL. */
static int32_t power_store(void *obj, int arg, const char *buf, uint32_t len)
{
    struct pm_runtime *rpm = (struct pm_runtime *)obj;
    long long ms;

    if (arg == ATTR_CONTROL) {
        if (word_is(buf, len, "on")) {
            (void)pm_runtime_forbid(rpm);
        } else if (word_is(buf, len, "auto")) {
            pm_runtime_allow(rpm);
        } else {
            return -EINVAL;
        }
        return (int32_t)len;
    }
    if (k_strtoll(buf, len, &ms) != 0 || ms < -2147483647LL - 1 || ms > 2147483647LL) {
        return -EINVAL;
    }
    pm_runtime_set_delay(rpm, (int32_t)ms);
    return (int32_t)len;
}

/* Linux's runtime attributes with their modes (drivers/base/power/sysfs.c). */
static const struct sysfs_attr power_attrs[] = {
    {"control", 0644u, power_show, power_store},
    {"autosuspend_delay_ms", 0644u, power_show, power_store},
    {"runtime_status", 0444u, power_show, NULL},
    {"runtime_active_time", 0444u, power_show, NULL},
    {"runtime_suspended_time", 0444u, power_show, NULL}};

int pm_runtime_sysfs_add(struct pm_runtime *rpm, int parent)
{
    int dir, h;
    unsigned i;

    if (rpm->dir > 0) {
        return rpm->dir;
    }
    dir = sysfs_add_dir(parent, "power");
    if (dir == -EEXIST) {
        dir = sysfs_node_child(parent, "power", 5);
    }
    if (dir <= 0) {
        return dir < 0 ? dir : -ENOENT;
    }
    for (i = 0; i < sizeof(power_attrs) / sizeof(power_attrs[0]); i++) {
        h = sysfs_add_attr(dir, &power_attrs[i], rpm, (int)i);
        if (h < 0) {
            sysfs_remove(dir);
            return h;
        }
    }
    rpm->dir = dir;
    return dir;
}

void pm_runtime_sysfs_remove(struct pm_runtime *rpm)
{
    if (rpm->dir > 0) {
        sysfs_remove(rpm->dir);
        rpm->dir = 0;
    }
}
