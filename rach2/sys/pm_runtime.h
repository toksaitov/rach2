/*
 * pm_runtime.h: runtime power management of a device, after Linux's
 * drivers/base/power/runtime.c: a count of the device's users, a sleep its
 * driver puts it in once the count has been 0 for the autosuspend delay,
 * the wake at its next use, and the power directory of the device in /sys.
 */
#ifndef _PM_RUNTIME_H_
#define _PM_RUNTIME_H_

#include "lib/kint.h"
#include "lib/spinlock.h"

/* A device's runtime state, as runtime_status names it. */
#define RPM_ACTIVE     0
#define RPM_RESUMING   1
#define RPM_SUSPENDED  2
#define RPM_SUSPENDING 3

/*
 * What the driver does for the core, each with the record's `owner`.
 *
 * `prepare` runs before a suspend, in process context under the big lock
 * and with nothing of the driver's held, while the device is still awake:
 * it takes back what a sleep would break and a user would make again only
 * after a get, such as programs' maps of the device's memory. It may
 * sleep. 0, or -EAGAIN or -EBUSY to have the suspend tried again 10 or
 * 100 ms later.
 *
 * `suspend` puts the device to sleep and `resume` wakes it. Each runs with
 * the count at its value then, the record's state RPM_SUSPENDING or
 * RPM_RESUMING, and no lock of the core's held, and neither sleeps: its
 * waits spin, so that a get may wake the device in any process context.
 * `suspend` returns 0; -EAGAIN for a race that passes at once or -EBUSY
 * for a state that lasts a while, to stay awake and be tried again 10 or
 * 100 ms later; or another -errno, which leaves the device awake and
 * stops runtime power management until `control` is written. `resume`
 * returns 0 or -errno, which is recorded the same way, the device then
 * taken as awake, as Linux's i915 takes it.
 *
 * `schedule` has `pm_runtime_idle` called in process context under the big
 * lock `ms` milliseconds from now at the soonest, once however often it is
 * asked before then. It may be called in any context.
 */
struct pm_runtime_ops {
    int32_t (*prepare)(void *owner);
    int32_t (*suspend)(void *owner);
    int32_t (*resume)(void *owner);
    void (*schedule)(void *owner, uint32_t ms);
};

/*
 * A device's runtime power management. Every field but `ops`, `owner` and
 * `dir` is under `lock`, taken with interrupts off.
 */
struct pm_runtime {
    struct spinlock lock;
    const struct pm_runtime_ops *ops;
    void *owner;
    int32_t usage;         /* holds that keep the device awake; the driver's
                            * bind holds one until pm_runtime_enable */
    int status;            /* RPM_ACTIVE to RPM_SUSPENDING */
    uint8_t enabled;       /* the driver lets the device sleep */
    uint8_t forbidden;     /* `control` reads "on", which holds one of `usage` */
    int32_t error;         /* a callback's error that stopped the sleeps, or 0 */
    int32_t delay_ms;      /* `autosuspend_delay_ms`; below 0 the device stays
                            * awake */
    int64_t last_busy;     /* rtc_monotonic_ns of the last put */
    int64_t busy_until;    /* rtc_monotonic_ns before which no sleep starts */
    int64_t accounted;     /* rtc_monotonic_ns up to which the times count */
    uint64_t active_ns;    /* time awake, RPM_ACTIVE or RPM_SUSPENDING */
    uint64_t suspended_ns; /* time asleep, RPM_SUSPENDED or RPM_RESUMING */
    const void *mover;     /* the process that runs the change under way */
    uint32_t suspends;     /* sleeps the device has entered */
    uint32_t resumes;      /* wakes from them */
    int dir;               /* the power directory's handle in /sys, or 0 */
};

/** Make `rpm` for a device that is awake, with one hold, its driver's, and
 * runtime power management not yet enabled; `control` reads "auto" and the
 * delay is 0 until the driver sets it. */
void pm_runtime_init(struct pm_runtime *rpm, const struct pm_runtime_ops *ops, void *owner);

/** Add `power` below the device's directory `parent` in /sys, with Linux's
 * `control`, `autosuspend_delay_ms`, `runtime_status`, `runtime_active_time`
 * and `runtime_suspended_time`. The handle, or -errno. */
int pm_runtime_sysfs_add(struct pm_runtime *rpm, int parent);

/** Take the power directory out of /sys. */
void pm_runtime_sysfs_remove(struct pm_runtime *rpm);

/** Let the device sleep, as Linux's pm_runtime_enable, and drop the hold
 * of `pm_runtime_init`, as i915's put at the end of its probe. */
void pm_runtime_enable(struct pm_runtime *rpm);

/**
 * Wake the device and keep it awake for good, with a hold the caller never
 * drops, as Linux's pm_runtime_get_sync and pm_runtime_disable at a
 * driver's removal: no sleep starts after. A sleep's work under way when
 * this is called either finds the core disabled or ends first. 0, or the
 * wake's -errno.
 */
int32_t pm_runtime_disable(struct pm_runtime *rpm);

/**
 * Take a hold and have the device awake when this returns, waking it if it
 * sleeps, as Linux's pm_runtime_get_sync. Process context, outside the
 * driver's `suspend` and `resume` but for the process that runs them,
 * whose get only counts, and with no spinlock held that they take. A get
 * meeting a sleep or a wake under way in another process spins until it
 * ends. 0, or the wake's -errno, with the hold taken either way.
 */
int32_t pm_runtime_get_sync(struct pm_runtime *rpm);

/** Take a hold only if the device is awake now: 1 with the hold, 0
 * without. Any context. */
int pm_runtime_get_if_active(struct pm_runtime *rpm);

/** Take a hold without waking the device, for a caller that knows another
 * hold keeps it awake, as Linux's pm_runtime_get_noresume. Any context. */
void pm_runtime_get_noresume(struct pm_runtime *rpm);

/** Drop a hold, mark the device busy now, and at the last have the sleep
 * tried once the delay has passed, as Linux's pm_runtime_put_autosuspend.
 * Any context. */
void pm_runtime_put(struct pm_runtime *rpm);

/** Keep the device from sleeping for `ms` milliseconds from now whatever
 * its holds, as Linux's i915 keeps its GTT faults' wakeref
 * (intel_wakeref_auto). Any context. */
void pm_runtime_mark_busy(struct pm_runtime *rpm, uint32_t ms);

/** `control` "auto": drop the hold "on" took, as Linux's pm_runtime_allow. */
void pm_runtime_allow(struct pm_runtime *rpm);

/** `control` "on": take a hold and wake the device, as Linux's
 * pm_runtime_forbid; clears a recorded error. 0, or the wake's -errno. */
int32_t pm_runtime_forbid(struct pm_runtime *rpm);

/** Set the autosuspend delay to `ms`, and have the sleep tried after it if
 * nothing holds the device. Process context. */
void pm_runtime_set_delay(struct pm_runtime *rpm, int32_t ms);

/**
 * The work `schedule` runs: when nothing holds the device, its sleeps are
 * enabled and allowed, no error stopped them and the delay since the last
 * put has passed, `prepare`, then `suspend`; otherwise nothing, or the work
 * asked for again at the time the delay ends. Process context, under the
 * big lock. May sleep in `prepare`.
 */
void pm_runtime_idle(struct pm_runtime *rpm);

/** The state now, RPM_ACTIVE to RPM_SUSPENDING. */
int pm_runtime_status(struct pm_runtime *rpm);

/** The holds now, for a report. */
int32_t pm_runtime_usage(struct pm_runtime *rpm);

#endif /* _PM_RUNTIME_H_ */
