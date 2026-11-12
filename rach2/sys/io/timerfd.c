/*
 * timerfd.c: timers read through descriptors, and POSIX timers, which share
 * their timer, as timerfd.h describes them.
 */

#include "timerfd.h"

#include "anon.h"
#include "../irq.h"
#include "../rtc.h"
#include "../timer.h"
#include "../fs/fd.h"
#include "../fs/file.h"
#include "../proc/process.h"
#include "../proc/sched.h"
#include "../proc/tgroup.h"
#include "../proc/waketime.h"
#include "../lib/ksignal.h"
#include "../proc/wait_queue.h"
#include "../lib/spinlock.h"
#include "../mm/kheap.h"
#include "../mm/usercopy.h"
#include "../lib/kerrno.h"
#include "../lib/kirq.h"
#include "../lib/kstring.h"

#define CLOCK_REALTIME       0
#define CLOCK_MONOTONIC      1
#define CLOCK_BOOTTIME       7
#define CLOCK_REALTIME_ALARM 8
#define CLOCK_BOOTTIME_ALARM 9

#define TFD_TIMER_ABSTIME       1
#define TFD_TIMER_CANCEL_ON_SET 2
#define TFD_NONBLOCK            04000
#define TFD_CLOEXEC             02000000

#define POLLIN 0x001

#define TIMER_ABSTIME   1
#define SIGEV_SIGNAL    0
#define SIGEV_NONE      1
#define SIGEV_THREAD_ID 4

/* Format: the kernel's ticks in a second, and nanoseconds in a tick. A
 * timer of the clocks keeps its time and interval in nanoseconds, as the
 * call gives them, and its next tick; one of processor time keeps whole
 * seconds and ticks below a second, and counts ticks. */
#define TICKS_PER_SEC ((uint32_t)TICK_HZ)
#define NS_PER_TICK   ((int32_t)TICK_NS)
#define NS_PER_SEC    1000000000
/* Policy: the most seconds a setting keeps, about 136 years: a longer time
 * is taken as that, as Linux takes one past its own limit, about 292
 * years. */
#define TIME_MAX      0xFFFFFFFFu
/* Policy: the latest time a timer of the clocks keeps, in nanoseconds on its
 * clock, about the year 2255: a later one is taken as that, as Linux takes
 * one past KTIME_MAX, in 2262. */
#define END_MAX       9000000000000000000ll

/* A timer: a timerfd's, read through its descriptor (`anon` and `wq`), or a
 * POSIX timer's, which `tg` names and whose expirations post a signal. A
 * timer of the clocks expires when `base` reaches `end`, which the tick
 * checks from the tick `due` on. A POSIX timer on a clock of processor time
 * counts the ticks charged to `cpu_tg`'s threads, or to its thread `cpu_tid`
 * alone, user ticks alone for `cpu_user`: `due` is then the ticks left, and
 * its interval is `isec` and `isub`. */
struct ktimer {
    struct anon_obj anon;
    struct ktimer *next; /* the list of armed timers the tick walks */
    int clock;
    int armed;
    uint64_t due;   /* the first tick that may see the next expiration;
                     * all ones while a periodic timerfd is stopped at its
                     * expiry (`expire`) */
    int base;       /* the clock `end` is on: `clock`, or for a relative
                     * timer of real time CLOCK_MONOTONIC, as Linux's
                     * hrtimer bases one */
    int64_t end;    /* the next expiration, in nanoseconds on `base` */
    int64_t set_at; /* the time on `base` at which `end` was last counted
                     * (`set_due`): a wake is late by the time past the
                     * later of the two */
    uint64_t ins;   /* the interval in nanoseconds, 0 for none */
    uint32_t isec;  /* processor time's interval, 0 and 0 for none: whole */
    uint32_t isub;  /* seconds, and ticks below a second */
    uint64_t count; /* a timerfd's expirations not yet read; a POSIX
                     * timer's notification that waits for another
                     * sender's signal of its number to be taken, as its
                     * overrun plus one, 0 for none (`ptimer_fire`) */
    wait_queue_t wq;
    struct tgroup *tg;       /* a POSIX timer's process, or NULL */
    struct ktimer *in_group; /* the process's next timer */
    int id;
    int notify; /* SIGEV_SIGNAL, SIGEV_NONE or SIGEV_THREAD_ID */
    int signo;
    uintptr_t value;
    uint32_t tid;          /* the thread SIGEV_THREAD_ID names */
    uint32_t overrun;      /* the intervals of the setting in force that
                            * the last periodic signal delivered since the
                            * setting counted past its first expiration, 0
                            * for none: Linux's it_overrun_last */
    uint64_t set_count;    /* the expirations of the setting in force since
                            * its signal, pending or waiting, was queued:
                            * Linux's it_overrun plus one, which a setting
                            * and a rearm make -1 (kernel/time/
                            * posix-timers.c:271-272, 881-882). Only a
                            * delivery under that setting adds them to the
                            * signal's overrun (`ptimer_rearm`), which
                            * otherwise holds what Linux's si_overrun holds
                            * while queued: one for each later setting that
                            * expired meanwhile (kernel/signal.c:2029-2036) */
    uint32_t gen;          /* a POSIX timer's setting, never 0 once set
                            * (`next_gen`): its signal carries it
                            * (`timer_gen`), and only a signal of the
                            * setting in force forwards the timer when
                            * taken, as Linux's it_requeue_pending
                            * (`ptimer_rearm`) */
    uint32_t count_gen;    /* the setting of the last expiration in `count`,
                            * which its signal carries when posted, as
                            * Linux's posix_timer_queue_signal writes
                            * si_sys_private at each expiration
                            * (kernel/time/posix-timers.c:288-302) */
    uint64_t wait_seq;     /* when `count` began to wait (`wait_order`):
                            * waiting notifications are posted oldest
                            * first, as Linux's queue delivers them */
    uint32_t key;          /* a POSIX timer's own number, from `next_gen` at
                            * timer_create and never 0: its signal carries it
                            * (`timer_key`), and only a pending signal that
                            * bears it is the timer's own, to whose overrun a
                            * later setting's first expiration adds one, and
                            * into whose setting's count (`set_count`) the
                            * later expirations go (`ptimer_fire`) */
    struct tgroup *cpu_tg; /* held: the process whose time it counts, or NULL */
    uint32_t cpu_tid;
    int cpu_user;

    struct ktimer *cnext;     /* the next timer on `cancelable` */
    int on_cancel;            /* on it: last set with TFD_TIMER_ABSTIME and
                               * TFD_TIMER_CANCEL_ON_SET on CLOCK_REALTIME */
    int canceled;             /* the real-time clock was set while it was on
                               * it, and no read or setting has said so */
    int stepped;              /* the clock was set while it was on it, since
                               * the last read or setting: Linux's tick for the
                               * step (fs/timerfd.c:108-111), which makes it
                               * readable */
    int expired;              /* it expired since the last read, setting or
                               * timerfd_gettime: Linux's `expired`, the mark of
                               * a timer stopped at its expiry until one of
                               * those starts it again (fs/timerfd.c:58-72) */
    int between;              /* its time may be armed between ticks
                               * (`arm_between`): every timerfd; a POSIX timer
                               * but SIGEV_NONE's when set, then while its last
                               * expiration set its signal pending, or was
                               * ignored with an interval of a tick or more, and
                               * again once that signal, or another sender's of
                               * its number that its notification waited for, is
                               * taken (`ptimer_rearm`) */
    uint64_t fire_fresh;      /* the expirations the tick counted and left for
                               * a holder of the big lock to signal
                               * (`ptimer_fire_later`) */
    struct ktimer *fire_next; /* the list of timers whose signal is due */
    int fire_queued;          /* on that list */
};

/* A setting as the call gives and takes it, whichever width of time: the
 * interval, then the first expiration. */
struct setting {
    int64_t isec;
    int32_t insec;
    int64_t vsec;
    int32_t vnsec;
};

/*
 * The armed timers, those of processor time, which `ptimer_cpu_tick`
 * counts, and the timerfds a setting of the real-time clock cancels, as
 * Linux's cancel_list holds them. Their lock covers the three lists, each
 * timer's count, arming and cancellation, and each process's list of POSIX
 * timers, against the ticks of every processor; it is taken with interrupts
 * off, after the big lock, and before the clock's own lock and the locks of
 * the wait queues and the run queues a firing takes.
 */
static struct spinlock timer_lock = SPINLOCK_INIT_NAMED("timer_lock");
static struct ktimer *armed;
static struct ktimer *cpu_armed;
static struct ktimer *cancelable;
/* The last number `next_gen` gave a setting or a timer. */
static uint32_t timer_gens;
/* The last `wait_seq` given, in the order notifications began to wait. */
static uint64_t wait_order;
/* The POSIX timers whose expirations the tick counted and left for a holder
 * of the big lock to signal (`ptimer_fire_later`), first expired first,
 * under `timer_lock`; and whether there are any, read without it. */
static struct ktimer *fire_head, **fire_tail = &fire_head;
static volatile int fire_due;
/* The processor that holds `timer_lock` while `ptimer_fire` posts a signal,
 * plus one, 0 for none: a stop or SIGCONT posted then discards other
 * signals (`ptimer_discard`) under the lock it already holds. */
static unsigned int fire_cpu;
/* The deleted timers whose records a discard under `fire_cpu` let go, on
 * their `in_group`, for the holder of `timer_lock` to free once it lets the
 * lock go (`timer_unlock`), under `timer_lock`. */
static struct ktimer *gone_later;

/** A number never given before and never 0, which a user's signal carries:
 * a new POSIX timer setting's generation, one for every setting of every
 * timer, so that neither a later setting nor a timer that takes a deleted
 * one's id matches a signal posted before it; and a new timer's `key`, so
 * that no other timer's expirations add to its signal. The caller holds
 * `timer_lock`. */
static uint32_t next_gen(void)
{
    timer_gens = timer_gens % 0xffffffffu + 1u; /* 1 to 2^32-1, never 0 */
    return timer_gens;
}

/** Take `t` off the list of armed timers it is on, if any. The caller holds
 * `timer_lock`. */
static void unlist(struct ktimer *t)
{
    struct ktimer **link = t->cpu_tg ? &cpu_armed : &armed;

    while (*link && *link != t) {
        link = &(*link)->next;
    }
    if (*link) {
        *link = t->next;
    }
}

/** Take `t` off the list of armed timers it is on and mark it disarmed. The
 * caller holds `timer_lock`. */
static void disarm(struct ktimer *t)
{
    unlist(t);
    t->armed = 0;
}

/** Put `t` on `cancelable` when `on` and take it off otherwise, as Linux's
 * timerfd_setup_cancel does (fs/timerfd.c:159-175). The caller holds
 * `timer_lock`. */
static void set_cancel(struct ktimer *t, int on)
{
    struct ktimer **link = &cancelable;

    if (on && !t->on_cancel) {
        t->cnext = cancelable;
        cancelable = t;
    } else if (!on && t->on_cancel) {
        while (*link != t) {
            link = &(*link)->cnext;
        }
        *link = t->cnext;
    }
    t->on_cancel = on;
}

/** `sec` seconds and `sub` ticks as ticks. */
static uint64_t ticks_of(uint32_t sec, uint32_t sub)
{
    return (uint64_t)sec * TICKS_PER_SEC + sub;
}

static int ptimer_fire(struct ktimer *t, uint64_t fresh);
static void ptimer_free(struct ktimer **link, int keep);
static void timer_unlock(uint32_t irq);
/** `n` divided by `d`, which is not 0, with the remainder in `*rem` when it
 * is not NULL; the freestanding kernel has no `__udivdi3`. */
static uint64_t div64(uint64_t n, uint64_t d, uint64_t *rem)
{
    uint64_t q = 0, r = 0;
    int i;

    if ((n >> 32) == 0 && (d >> 32) == 0) {
        q = (uint32_t)n / (uint32_t)d;
        r = (uint32_t)n % (uint32_t)d;
    } else {
        for (i = 63; i >= 0; i--) {
            r = (r << 1) | ((n >> i) & 1u);
            if (r >= d) {
                r -= d;
                q |= (uint64_t)1 << i;
            }
        }
    }
    if (rem) {
        *rem = r;
    }
    return q;
}

/** The time on `clock` now in `*ns`, in nanoseconds, never past END_MAX nor
 * before 0; 0, or `clock_now`'s error with `*ns` 0. */
static int32_t clock_ns(int clock, int64_t *ns)
{
    int64_t sec;
    int32_t nsec;
    int32_t rc = clock_now(clock, &sec, &nsec);

    if (rc != 0 || sec < 0) {
        *ns = 0;
    } else if (sec >= END_MAX / NS_PER_SEC) {
        *ns = END_MAX;
    } else {
        *ns = sec * NS_PER_SEC + nsec;
    }
    return rc;
}

/* A reading of a timer's clock and of the tick count, taken together. */
struct reading {
    int64_t clock;  /* the time on the timer's `base`, in nanoseconds */
    uint64_t tick;  /* the tick count as the clocks count it */
    uint32_t phase; /* nanoseconds into the tick in progress */
};

/**
 * Read the tick count and the clock `base` into `r`, the tick's from where
 * the clock is within the tick in progress (`rtc_tick_phase`); 0, or
 * `clock_now`'s error. The caller has interrupts off, so that no tick comes
 * between the two readings and the time to a timer's end counts from the
 * tick it is added to, as Linux's hrtimer adds a relative time to its
 * base's clock and queues the timer under the base's lock
 * (kernel/time/hrtimer.c:1260 and 1342). The tick is read first, so that a
 * delay between the readings, a virtual processor's preemption, can make
 * `due` early, which `expire` corrects, and never late.
 */
static int32_t read_now(int base, struct reading *r)
{
    rtc_tick_phase(&r->tick, &r->phase);
    return clock_ns(base, &r->clock);
}

/* The bases a timer of the clocks counts on, as `pass_read` indexes them. */
#define PASS_BASES 3

/*
 * The readings of one pass over the armed timers, each taken at its first
 * use and then shared by every timer of the pass, as Linux's
 * hrtimer_interrupt reads the clock once and compares every timer of a
 * base with that reading and the base's offset (kernel/time/hrtimer.c:1814,
 * 1861, 1879).
 */
struct pass {
    struct reading read[PASS_BASES]; /* CLOCK_REALTIME, MONOTONIC, BOOTTIME */
    int32_t rc[PASS_BASES];          /* `read_now`'s answer for each */
    uint8_t have;                    /* a bit for each reading taken */
    uint8_t have_offset;
    int64_t offset; /* with `have_offset`, CLOCK_REALTIME less MONOTONIC */
};

/** The reading of `base` for the pass `p`, taken at its first use; NULL
 * when the clock cannot be read. */
static const struct reading *pass_read(struct pass *p, int base)
{
    int i = base == CLOCK_REALTIME ? 0 : base == CLOCK_MONOTONIC ? 1 : 2;

    if (!(p->have & (1u << i))) {
        p->rc[i] = read_now(base, &p->read[i]);
        p->have |= (uint8_t)(1u << i);
    }
    return p->rc[i] == 0 ? &p->read[i] : NULL;
}

/**
 * Arm the interrupt between ticks for `t`'s end (`timer_wake_at`), however
 * far it is, as Linux starts a timer's hrtimer when the timer is set
 * (hrtimer_start_range_ns, kernel/time/hrtimer.c:1324-1347): the timer then
 * expires at its time, and not at the tick after it, and no pass needs to
 * come before its time to arm it. A processor keeps the earliest time asked
 * of it, and every pass arms each time still to come again (`timerfd_tick`,
 * `timerfd_due`), so a later time that an earlier one put aside is armed
 * once that is served.
 * The time is the end itself on the monotonic clock and the end less the
 * real-time clock's offset on CLOCK_REALTIME, the pass `p`'s when it is not
 * NULL, as Linux programs a timer at its expiry less its base's offset
 * (kernel/time/hrtimer.c:818, 922), so that no time passed between a
 * reading and the arming is added to it. A POSIX timer whose `between` is
 * clear is left to the ticks. The caller holds `timer_lock`.
 */
static void arm_between(const struct ktimer *t, struct pass *p)
{
    int64_t at = t->end;

    if (!t->between || t->end >= END_MAX) {
        return;
    }
    if (t->base == CLOCK_REALTIME) {
        int64_t sec, off;
        int32_t nsec;

        if (p && p->have_offset) {
            off = p->offset;
        } else {
            rtc_offset(&sec, &nsec);
            off = sec * NS_PER_SEC + nsec;
            if (p) {
                p->offset = off;
                p->have_offset = 1;
            }
        }
        at -= off;
    }
    timer_wake_at(at > 0 ? at : 1);
}

/** Move `t->end`, which the clock's reading `clock` has reached, on by
 * whole intervals past it, as hrtimer_forward does (kernel/time/hrtimer.c:
 * 1046-1076); returns the intervals moved, at least one. */
static uint64_t forward_end(struct ktimer *t, int64_t clock)
{
    uint64_t n = div64((uint64_t)(clock - t->end), t->ins, NULL) + 1u;
    /* under 2^64: the gap is under END_MAX and the interval under 2^63 */
    uint64_t step = n * t->ins;

    t->end = step > (uint64_t)(END_MAX - t->end) ? END_MAX : t->end + (int64_t)step;
    return n;
}

/** Set `t->due` to the first tick at or after `t->end`, counted from the
 * reading `r` (`read_now`), as `sleep_for` ends a sleep, or the tick in
 * progress when the clock has reached `t->end`; and at least the tick after
 * `now`, a tick the caller has handled, so that the tick's walk ends even
 * for a timer held at END_MAX by a clock past it. The end is armed between
 * ticks (`arm_between`), and the reading kept as the time it was counted
 * at (`set_at`). */
static void set_due(struct ktimer *t, const struct reading *r, uint64_t now)
{
    int64_t left = t->end - r->clock;

    t->set_at = r->clock;
    t->due = r->tick;
    if (left > 0) {
        t->due += div64((uint64_t)left + r->phase + NS_PER_TICK - 1u, NS_PER_TICK, NULL);
    }
    if (now != 0 && t->due <= now) {
        t->due = now + 1u;
    }
    arm_between(t, NULL);
}

/**
 * Leave `fresh` expirations of the POSIX timer `t` for a holder of the big
 * lock to signal (`ptimer_post_due`), since the tick runs without it and a
 * signal is posted under it; the expirations counted meanwhile join them,
 * as they would join a signal posted at the first. Linux posts a timer's
 * signal from its timer's callback (kernel/time/posix-timers.c:318-377).
 * Until then the timer is left to the ticks (`between`), as a timer whose
 * signal is pending is. The caller holds `timer_lock`.
 */
static void ptimer_fire_later(struct ktimer *t, uint64_t fresh)
{
    t->fire_fresh += fresh;
    t->between = 0;
    if (t->fire_queued) {
        return;
    }
    t->fire_next = NULL;
    *fire_tail = t;
    fire_tail = &t->fire_next;
    t->fire_queued = 1;
    fire_due = 1;
}

/** Take `t` off the list of timers whose signal is due, and return the
 * expirations it had waiting; the caller holds `timer_lock`. */
static uint64_t ptimer_fire_cancel(struct ktimer *t)
{
    struct ktimer **link = &fire_head;
    uint64_t fresh = t->fire_fresh;

    t->fire_fresh = 0;
    if (!t->fire_queued) {
        return fresh;
    }
    while (*link && *link != t) {
        link = &(*link)->fire_next;
    }
    if (*link) {
        *link = t->fire_next;
        if (fire_tail == &t->fire_next) {
            fire_tail = link;
        }
    }
    t->fire_queued = 0;
    t->fire_next = NULL;
    fire_due = fire_head != NULL;
    return fresh;
}

/** Signal the expirations of `t` the tick left (`ptimer_fire_later`), if
 * any, and arm its next time between ticks when its signal allows
 * (`ptimer_fire`), as `post_waiting` does. The caller holds the big lock and
 * `timer_lock`. */
static void ptimer_post_one(struct ktimer *t)
{
    uint64_t fresh = ptimer_fire_cancel(t);
    struct reading r;
    int between;

    if (fresh == 0) {
        return;
    }
    between = ptimer_fire(t, fresh);
    if (t->armed && !t->cpu_tg && read_now(t->base, &r) == 0) {
        t->between = between;
        if (between) {
            set_due(t, &r, 0);
        }
    }
}

int ptimer_posts_due(void)
{
    return fire_due;
}

void ptimer_post_due(void)
{
    uint32_t irq;

    if (!fire_due) {
        return;
    }
    irq = spin_lock_irqsave(&timer_lock);
    timer_wake_hold();
    while (fire_head) {
        ptimer_post_one(fire_head);
    }
    fire_due = 0;
    timer_wake_release();
    timer_unlock(irq);
}

/**
 * Start the wake record of the reader `p` of an expiring timerfd, the clock
 * `late` nanoseconds past the time it waited for (`waketime_expire_late`),
 * under the run queues' lock, taken after the queue's, which the caller
 * holds: a wake of `p` from elsewhere, a signal or another descriptor it
 * polls, moves the record's stage under that lock, and the record is begun
 * only while `p` is blocked and has none, so neither overwrites the other.
 * Linux's waker likewise tests a task's state under the task's `pi_lock`,
 * which every waker takes (`try_to_wake_up`, kernel/sched/core.c:4191-4194).
 */
static void timerfd_wake_record(process_t *p, uint64_t late)
{
    uint32_t flags = sched_lock_take();

    waketime_expire_late(p, (int64_t)late);
    sched_lock_give(flags);
}

/**
 * The tick `now` has reached `t->due`: when `t->base` has reached `t->end`,
 * an expiration; then its readers woken, or its signal left for a holder of
 * the big lock to post (`ptimer_fire_later`), since a pass runs without
 * it; SIGEV_NONE, which posts nothing, is counted at once. A periodic
 * POSIX timer counts those of an interval missed since, as Linux's
 * hrtimer_forward counts them from the time and interval it was given, and
 * is armed again by its interval, between ticks only once its signal is
 * set pending, or is ignored and its interval is a tick or more
 * (`ptimer_post_one`): its expirations are otherwise counted at ticks alone, so
 * that a short interval cannot raise interrupt after interrupt between
 * them. A periodic timerfd counts the one, and
 * stops at its expiry, armed but off the tick's list (`due` never), as
 * Linux's timerfd_tmrproc returns HRTIMER_NORESTART (fs/timerfd.c:58-80),
 * until a read, a setting or timerfd_gettime starts it again
 * (`timerfd_forward`). A one-shot timer is disarmed. A tick that comes
 * before the clock reaches the time, as one does when the clock runs ahead
 * of the tick count, only moves `due` on, as `deadline_passed` in poll.c
 * does, since Linux's timers never expire before their clock reaches their
 * time. `r` is the reading of `t->base` the decision is made on (`read_now`,
 * `pass_read`). A reader's wake is late by the time since the end or since
 * the end was counted (`set_at`), whichever is later, so that a timer set to
 * a time already past, or moved past by a step of the clock, counts only
 * the time since then; that time is read at the wake, and not taken from
 * `r`, which a pass shares with the timers before this one. The caller
 * holds `timer_lock`.
 */
static void expire(struct ktimer *t, uint64_t now, const struct reading *r)
{
    if (r->clock < t->end) {
        set_due(t, r, now);
        return;
    }
    t->expired = 1;
    if (!t->tg && t->wq.head) {
        int64_t from = t->set_at > t->end ? t->set_at : t->end, at = 0;

        (void)clock_ns(t->base, &at);
        wq_each_waiter(&t->wq, timerfd_wake_record, (uint64_t)(at > from ? at - from : 0));
    }
    if (t->tg) {
        uint64_t n = 1;

        if (t->ins != 0) {
            n = forward_end(t, r->clock);
        } else {
            disarm(t);
        }
        if (t->notify == SIGEV_NONE) {
            t->between = ptimer_fire(t, n);
        } else {
            ptimer_fire_later(t, n);
        }
        if (t->armed) {
            set_due(t, r, now);
        }
        return;
    }
    t->count++;
    if (t->ins != 0) {
        unlist(t);
        t->due = ~(uint64_t)0;
    } else {
        disarm(t);
    }
    wq_wake_all(&t->wq);
}

/**
 * Start `t`, a periodic timerfd stopped at its expiry (`expire`), again:
 * move its time on by whole intervals past its clock now and put it back on
 * the tick's list, as Linux's hrtimer_forward_now and hrtimer_restart do
 * (kernel/time/hrtimer.c:1046-1079). Returns the intervals it moved, the
 * expirations since the one counted plus that one, and 0 when a step of
 * the clock back has put the time ahead again, which leaves the time where
 * it was. `r` is a reading of `t->base` (`read_now`). Nothing is done
 * unless `t` is a timerfd that expired and is stopped. The caller holds
 * `timer_lock`.
 */
static uint64_t timerfd_forward(struct ktimer *t, const struct reading *r)
{
    uint64_t n = 0;

    if (t->tg || !t->expired || !t->armed || t->ins == 0) {
        return 0;
    }
    if (r->clock >= t->end) {
        n = forward_end(t, r->clock);
    }
    set_due(t, r, 0);
    t->next = armed;
    armed = t;
    return n;
}

/** `sec` seconds and `sub` ticks as ticks, at least one, so that a timer
 * of processor time armed for no time expires at the next tick charged. */
static uint64_t cpu_ticks_of(uint32_t sec, uint32_t sub)
{
    uint64_t ticks = ticks_of(sec, sub);

    return ticks != 0 ? ticks : 1u;
}

void ptimer_cpu_tick(const process_t *p, int user)
{
    struct ktimer *t;

    spin_lock(&timer_lock);
    t = cpu_armed;
    while (t) {
        struct ktimer *next = t->next;

        if (t->cpu_tg == p->tg && (t->cpu_tid == 0 || t->cpu_tid == p->pid) &&
            (user || !t->cpu_user) && --t->due == 0) {
            if (t->isec != 0 || t->isub != 0) {
                t->due = cpu_ticks_of(t->isec, t->isub);
            } else {
                disarm(t);
            }
            ptimer_fire_later(t, 1);
        }
        t = next;
    }
    spin_unlock(&timer_lock);
}

/** The place of the clock `base` in the order Linux's hrtimer interrupt runs
 * its bases: CLOCK_MONOTONIC's, CLOCK_REALTIME's, then CLOCK_BOOTTIME's
 * (kernel/time/hrtimer.c:71-87, 1791). */
static int base_rank(int base)
{
    return base == CLOCK_MONOTONIC ? 0 : base == CLOCK_REALTIME ? 1 : 2;
}

/**
 * Whether the timer `t`, which a pass is to expire, comes before `first`,
 * or `first` is NULL: by its base (`base_rank`), and on one base by its
 * end, as __hrtimer_run_queues runs each base's expired timers earliest
 * first (kernel/time/hrtimer.c:1791-1811, include/linux/timerqueue.h:
 * 20-26), so that their signals are posted, or wait for another sender's,
 * in the order they expired (`ptimer_fire_later`, `wait_seq`). Of two with
 * the same end, the one later on `armed`, which went on it first, comes
 * first, as the timerqueue puts an equal time after those queued before it.
 */
static int expires_first(const struct ktimer *t, const struct ktimer *first)
{
    int a, b;

    if (!first) {
        return 1;
    }
    a = base_rank(t->base);
    b = base_rank(first->base);
    return a != b ? a < b : t->end <= first->end;
}

/**
 * Each base's clock is read once for the pass (`pass_read`), at the first
 * timer due on it. A timer whose tick has come and whose clock has not
 * reached its time has its tick moved on (`expire`); those whose clock has
 * reached it expire in the order Linux runs them (`expires_first`), each
 * found again on the list after the one before, since an expiration can
 * take a timer off it. An expired timer's `due` is past `now`, so it is not
 * found twice. A timer whose time is still to come has it armed between
 * ticks again (`arm_between`), since a processor keeps only the earliest
 * time asked of it.
 */
void timerfd_tick(uint64_t now)
{
    struct pass p;
    struct ktimer *t, *first;
    const struct reading *r;

    p.have = 0;
    p.have_offset = 0;
    spin_lock(&timer_lock);
    for (;;) {
        first = NULL;
        for (t = armed; t; t = t->next) {
            if (!t->armed || now < t->due || (r = pass_read(&p, t->base)) == NULL) {
                continue;
            }
            if (r->clock < t->end) {
                expire(t, now, r); /* the clock short of its time: due moves on */
            } else if (expires_first(t, first)) {
                first = t;
            }
        }
        if (!first) {
            break;
        }
        expire(first, now, pass_read(&p, first->base));
    }
    for (t = armed; t; t = t->next) {
        if (t->armed && t->due != ~(uint64_t)0) {
            arm_between(t, &p); /* not a timerfd stopped at its expiry */
        }
    }
    spin_unlock(&timer_lock);
}

/**
 * Each base's clock is read once for the pass (`pass_read`), and decides,
 * not the tick count, as Linux's hrtimer_interrupt compares every timer
 * with one reading (kernel/time/hrtimer.c:1814, 1861). The timers whose
 * clock has reached their time expire in the order Linux runs them
 * (`expires_first`), each found again on the list after the one before; an
 * expired timer is not armed between ticks, or not on the list, so it is
 * not found twice. A timer whose time is still to come, since the
 * interrupt came a few counts before it or was armed for an earlier time,
 * has it armed again (`arm_between`).
 */
void timerfd_due(void)
{
    uint64_t now = tick_count();
    struct pass p;
    struct ktimer *t, *first;
    const struct reading *r;

    p.have = 0;
    p.have_offset = 0;
    spin_lock(&timer_lock);
    for (;;) {
        first = NULL;
        for (t = armed; t; t = t->next) {
            if (t->armed && t->between && (r = pass_read(&p, t->base)) != NULL &&
                r->clock >= t->end && expires_first(t, first)) {
                first = t;
            }
        }
        if (!first) {
            break;
        }
        expire(first, now, pass_read(&p, first->base));
    }
    for (t = armed; t; t = t->next) {
        if (t->armed && t->between && (r = pass_read(&p, t->base)) != NULL) {
            arm_between(t, &p);
        }
    }
    spin_unlock(&timer_lock);
}

/**
 * Linux's clock_was_set and timerfd_clock_was_set (kernel/time/hrtimer.c:
 * 951-987, fs/timerfd.c:97-115): each armed timer whose end is a time of
 * CLOCK_REALTIME has its tick counted again from one reading of the clock as
 * now set, and its end armed between ticks again, so that a step forward
 * past its end expires it at once and a step back keeps it waiting, with
 * the interrupt between ticks programmed once for them all
 * (`timer_wake_hold`), as clock_was_set reprograms each processor once; and
 * each timerfd on `cancelable` is marked canceled and stepped, which makes
 * it readable as Linux's tick for the step does,
 * and its readers woken, so that its next read or setting fails with
 * ECANCELED. Without `cancel`, for a step that stands for adjtimex's
 * ADJ_OFFSET, the timers are counted again and none is canceled, since
 * Linux slews that offset and calls clock_was_set only for ADJ_SETOFFSET, a
 * change of TAI or a leap second (kernel/time/ntp.c:754-755,
 * kernel/time/timekeeping.c:2490-2532).
 */
void timerfd_clock_was_set(int cancel)
{
    uint32_t irq = spin_lock_irqsave(&timer_lock);
    struct reading r;
    struct ktimer *t;
    int have = 0;

    timer_wake_hold();
    for (t = armed; t; t = t->next) {
        if (t->base == CLOCK_REALTIME) {
            if (!have) {
                (void)read_now(CLOCK_REALTIME, &r);
                have = 1;
            }
            set_due(t, &r, 0);
        }
    }
    timer_wake_release();
    for (t = cancel ? cancelable : NULL; t; t = t->cnext) {
        t->canceled = 1;
        t->stepped = 1;
        wq_wake_all(&t->wq);
    }
    spin_unlock_irqrestore(&timer_lock, irq);
}

/**
 * A time of `sec` seconds and `nsec` nanoseconds, below a second, as whole
 * seconds and ticks below a second, rounded up to a tick, for a timer of
 * processor time, which counts ticks. A negative time is none, and a time
 * past TIME_MAX seconds is that.
 */
static void to_wait(int64_t sec, int32_t nsec, uint32_t *s, uint32_t *sub)
{
    *s = 0;
    *sub = 0;
    if (sec < 0) {
        return;
    }
    if (sec >= (int64_t)TIME_MAX) {
        *s = TIME_MAX;
        return;
    }
    *s = (uint32_t)sec;
    *sub = ((uint32_t)nsec + NS_PER_TICK - 1u) / NS_PER_TICK;
    if (*sub == TICKS_PER_SEC) {
        *s += 1u;
        *sub = 0;
    }
}

/** Whether `t`'s next read or setting fails with ECANCELED, as Linux's
 * timerfd_canceled answers (fs/timerfd.c:151-157): the real-time clock was
 * set while `t` was on `cancelable`, and it is on it still. */
static int is_canceled(const struct ktimer *t)
{
    return t->on_cancel && t->canceled;
}

/** The wait condition of a read: the timer `arg` has expirations, or the
 * clock was set since its last read or setting (`stepped`), as Linux's
 * `ticks` counts both (fs/timerfd.c:258, 279). A timerfd canceled and then
 * set to disarmed is not readable, as in Linux, whose setting zeroes
 * `ticks` and keeps the cancel mark (fs/timerfd.c:200-201, 215-228). */
static int readable(void *arg)
{
    const struct ktimer *t = (const struct ktimer *)arg;

    return t->count != 0 || t->stepped;
}

/**
 * read(2) of a timerfd: the expirations since the last read, which it takes.
 *
 * The count is read and cleared under the timers' lock, since the timer
 * interrupt adds to it (`timerfd_tick`), and before the copy to the caller,
 * which can sleep in a page fault: an expiration that comes meanwhile is
 * kept for the next read, and a second reader finds none of the ones this
 * read took. A copy that fails loses the count, as in Linux.
 *
 * A canceled timer (`is_canceled`) fails with -ECANCELED once a blocking
 * read has something to read, at once for a nonblocking read or one a
 * signal ended, and drops its count, as Linux's read does (fs/timerfd.c:
 * 272-290). A periodic timer keeps running then, unless it expired and
 * nothing read, set or asked for it since (`expired`): Linux stops such a
 * timer at its expiry and only those three start it again, so it stays
 * disarmed until it is set. Otherwise a read with a count clears `expired`,
 * and a periodic timer stopped at its expiry is started again
 * (`timerfd_forward`), the expirations its forwarding counts added less the
 * one already counted, as Linux's read adds hrtimer_forward_now's count less
 * one only when both `expired` and `tintv` are set (fs/timerfd.c:292-313);
 * a one-shot timer's read returns its count of one. When a step of the
 * clock back leaves a periodic timer no count, the read returns -EAGAIN for
 * a nonblocking descriptor and 0 otherwise, as Linux's does
 * (fs/timerfd.c:276-279, 315-321). Returns 8, -EINVAL for a
 * buffer under eight bytes, -ECANCELED, -EAGAIN, -EINTR, -EFAULT or 0.
 */
static int32_t timerfd_read(fd_entry_t *e, char __user *buf, uint32_t count)
{
    struct ktimer *t = (struct ktimer *)e->priv;
    int nonblock = e->ofd && (e->ofd->status_flags & OFD_NONBLOCK);
    int32_t rc = 0;
    uint64_t n;

    if (count < sizeof(n)) {
        return -EINVAL;
    }
    for (;;) {
        uint32_t irq = spin_lock_irqsave(&timer_lock);

        if (is_canceled(t) && (readable(t) || nonblock || rc != 0)) {
            t->canceled = 0;
            if (t->expired && t->armed) {
                disarm(t);
            }
            t->count = 0;
            t->stepped = 0;
            t->expired = 0;
            spin_unlock_irqrestore(&timer_lock, irq);
            return -ECANCELED;
        }
        n = t->count;
        t->count = 0;
        t->stepped = 0;
        if (n != 0) {
            if (t->expired && t->armed && t->ins != 0) {
                struct reading r;

                (void)read_now(t->base, &r);
                n = n + timerfd_forward(t, &r) - 1u;
            }
            t->expired = 0;
            if (n == 0) {
                spin_unlock_irqrestore(&timer_lock, irq);
                return nonblock ? -EAGAIN : 0;
            }
        }
        spin_unlock_irqrestore(&timer_lock, irq);
        if (n != 0 || rc != 0) {
            break;
        }
        if (nonblock) {
            return -EAGAIN;
        }
        if (wq_wait_event(&t->wq, readable, t) < 0) {
            rc = -EINTR;
        }
    }
    if (n == 0) {
        return rc;
    }
    if (copy_to_user((void __user *)buf, &n, sizeof(n)) != 0) {
        return -EFAULT;
    }
    return (int32_t)sizeof(n);
}

/** poll of a timerfd: POLLIN among `events` while it is readable
 * (`readable`), and its wait queue. */
static short timerfd_poll(fd_entry_t *e, short events, struct wait_queue **wq)
{
    struct ktimer *t = (struct ktimer *)e->priv;

    if (wq) {
        *wq = &t->wq;
    }
    return (short)(readable(t) ? (events & POLLIN) : 0);
}

/** The last close of a timerfd: the timer is disarmed, taken off
 * `cancelable` and freed. */
static void timerfd_close(fd_entry_t *e)
{
    struct ktimer *t = (struct ktimer *)e->priv;
    uint32_t flags = spin_lock_irqsave(&timer_lock);

    if (t->armed) {
        disarm(t);
    }
    set_cancel(t, 0);
    spin_unlock_irqrestore(&timer_lock, flags);
    kfree(t);
    e->priv = NULL;
}

static const struct file_ops timerfd_ops = {timerfd_read,  NULL, timerfd_poll, NULL,
                                            timerfd_close, NULL, NULL,         0};

/** The timer of the caller's descriptor `fd`, or NULL when `fd` is not open
 * or not a timerfd. */
static struct ktimer *timer_of(int fd)
{
    fd_entry_t *e = fd_get(current_proc->fds, fd);

    return (e && e->ops == &timerfd_ops) ? (struct ktimer *)e->priv : NULL;
}

long sys_timerfd_create(struct isr_params *params)
{
    int clock = (int)SC_ARG(params, 0);
    uint32_t flags = (uint32_t)SC_ARG(params, 1);
    struct ktimer *t;
    fd_entry_t *e;
    int fd;

    if (clock == CLOCK_REALTIME_ALARM || clock == CLOCK_BOOTTIME_ALARM) {
        return -EPERM;
    }
    if ((clock != CLOCK_REALTIME && clock != CLOCK_MONOTONIC && clock != CLOCK_BOOTTIME) ||
        (flags & ~(uint32_t)(TFD_NONBLOCK | TFD_CLOEXEC))) {
        return -EINVAL;
    }
    t = kzalloc(sizeof(*t));
    if (!t) {
        return -ENOMEM;
    }
    t->anon.name = "[timerfd]";
    t->clock = clock;
    wait_queue_init(&t->wq);
    fd = fd_alloc(current_proc->fds);
    if (fd < 0) {
        kfree(t);
        return fd;
    }
    e = fd_slot(current_proc->fds, fd);
    e->type = FD_TYPE_ANON;
    e->ops = &timerfd_ops;
    e->priv = t;
    e->flags = FD_FLAG_READ | ((flags & TFD_CLOEXEC) ? FD_FLAG_CLOEXEC : 0u);
    if (flags & TFD_NONBLOCK) {
        e->ofd->status_flags |= OFD_NONBLOCK;
    }
    fd_install(current_proc->fds, fd);
    return fd;
}

/** The timer's setting now: the time to its next expiration and its
 * interval, to the nanosecond for a timer of the clocks, as Linux's hrtimer
 * keeps them. A disarmed timer has no time left, and a disarmed POSIX timer
 * no interval; a disarmed timerfd keeps the interval it was last set with,
 * as Linux's `tintv` (fs/timerfd.c:202, 568). It is read under the timers'
 * lock, since the tick moves the timer, and marks an expiration seen
 * (`expired`). A periodic timerfd stopped at its expiry is started again
 * first (`timerfd_forward`), its count moved by the forwarding's count less
 * one, as Linux's timerfd_gettime forwards it against the clock now and
 * restarts it (fs/timerfd.c:552-565), so a step of the clock back before it
 * reports the time to the expiry it stopped at. A periodic POSIX timer of
 * the clocks that the ticks alone count (`between` clear: SIGEV_NONE's, or
 * one whose signal is pending) has the expirations its clock has passed
 * counted first (`expire`), into the setting's count (`set_count`), which
 * the signal's delivery adds to its overrun, so that its
 * time is never reported as passed, as Linux's common_timer_get forwards
 * such a timer (kernel/time/posix-timers.c:670-678); an expired one-shot
 * timer of SIGEV_NONE has no time left, and any other one passed is
 * reported a nanosecond away, its signal not yet delivered
 * (kernel/time/posix-timers.c:690-701). The time left is counted from the
 * reading the forwarding was made on, as common_timer_get counts both from
 * one `now` (kernel/time/posix-timers.c:671-688), so that a short interval
 * that ends between two readings cannot leave a forwarded timer reported
 * as due. */
static void current_setting(struct ktimer *t, struct setting *s)
{
    uint32_t irq = spin_lock_irqsave(&timer_lock);
    struct reading r = {0, 0, 0};
    int32_t rc = 0;
    uint32_t rest;

    k_memset(s, 0, sizeof(*s));
    if (t->armed && !t->cpu_tg) {
        rc = read_now(t->base, &r);
    }
    if (t->expired && !t->tg && t->armed && t->ins != 0) {
        uint64_t n = timerfd_forward(t, &r);

        /* the forwarding's count less the one counted; Linux's ticks */
        t->count = n != 0 || t->count != 0 ? t->count + n - 1u : 0;
    }
    if (t->tg && !t->cpu_tg && t->armed && t->ins != 0 && !t->between && rc == 0) {
        expire(t, 0, &r);
    }
    if (t->tg) {
        ptimer_post_one(t);
    }
    t->expired = 0;
    if (t->armed && t->cpu_tg) {
        s->vsec = (int64_t)tick_div(t->due, TICKS_PER_SEC, &rest);
        s->vnsec = (int32_t)rest * NS_PER_TICK;
        s->isec = t->isec;
        s->insec = (int32_t)t->isub * NS_PER_TICK;
    } else if (t->armed || !t->tg) {
        int64_t left;
        uint64_t ns;

        if (t->armed) {
            left = t->end - r.clock;
            if (left > 0) {
                s->vsec = (int64_t)div64((uint64_t)left, NS_PER_SEC, &ns);
                s->vnsec = (int32_t)ns;
            } else if (!t->tg || t->notify != SIGEV_NONE) {
                s->vnsec = 1; /* due now: reported as not yet expired */
            }
        }
        s->isec = (int64_t)div64(t->ins, NS_PER_SEC, &ns);
        s->insec = (int32_t)ns;
    }
    timer_unlock(irq);
}

/** `sec` seconds and `nsec` nanoseconds, neither negative, as nanoseconds,
 * the seconds cut at TIME_MAX. */
static uint64_t ns_of(int64_t sec, int32_t nsec)
{
    if (sec > (int64_t)TIME_MAX) {
        sec = (int64_t)TIME_MAX;
    }
    return (uint64_t)sec * NS_PER_SEC + (uint32_t)nsec;
}

/**
 * List `t`, a timer of the clocks, as armed from the setting `s`, absolute
 * on `base` when `flags` has TFD_TIMER_ABSTIME and otherwise from the
 * reading `r`; 0, or -ECANCELED for a timerfd that is canceled
 * (`is_canceled`), which is armed all the same, as Linux's timerfd_setup arms
 * it (fs/timerfd.c:227-228). The caller holds `timer_lock`.
 */
static int32_t list_clock(struct ktimer *t, int base, int flags, const struct setting *s,
                          const struct reading *r)
{
    uint64_t v = ns_of(s->vsec, s->vnsec);

    if (flags & TFD_TIMER_ABSTIME) {
        t->end = s->vsec >= END_MAX / NS_PER_SEC ? END_MAX : s->vsec * NS_PER_SEC + s->vnsec;
    } else {
        t->end = v > (uint64_t)(END_MAX - r->clock) ? END_MAX : r->clock + (int64_t)v;
    }
    t->base = base;
    t->ins = ns_of(s->isec, s->insec);
    t->between = !t->tg || t->notify != SIGEV_NONE;
    set_due(t, r, 0); /* from a call: no tick handled */
    t->armed = 1;
    t->next = armed;
    armed = t;
    if (is_canceled(t)) {
        t->canceled = 0;
        return -ECANCELED;
    }
    return 0;
}

/**
 * Arm `t` from the setting `s`, absolute on its clock when `flags` has
 * TFD_TIMER_ABSTIME, or disarm it for a first expiration of zero, dropping
 * a timerfd's expirations not read. A POSIX timer starts a new generation
 * (`next_gen`), so that a signal of the old setting still pending forwards
 * nothing when taken, as Linux's posix_timer_set_common moves
 * it_requeue_pending on (kernel/time/posix-timers.c:879), and drops the
 * old setting's expirations since its signal was queued (`set_count`), as
 * posix_timer_set_common resets it_overrun (kernel/time/posix-timers.c:
 * 881-882); a notification that waits for another sender's signal stays,
 * with its own overrun and the setting it was posted for (`count_gen`),
 * since Linux leaves the timer's sigqueue queued across a setting; 0, -EINVAL
 * for a time out of range, or `clock_now`'s error. A timer of the clocks
 * keeps its time and interval to the nanosecond, as Linux's timerfd_setup
 * keeps them, and expires when its clock reaches its time, by the interrupt
 * between ticks (`arm_between`), as Linux's hrtimer does, or at the first
 * tick at which its clock has reached its time where there is none or the
 * timer is a POSIX timer left to the ticks (`set_due`, `expire`): never
 * early. Its clock and the tick are read under the timers' lock
 * (`read_now`). A time already past expires at once by that interrupt, or
 * at the next tick without it, with each interval since it counted.
 *
 * A timerfd goes on `cancelable` or off it by `flags`, as Linux's
 * timerfd_settime does before it arms the timer, and one armed while
 * canceled fails with -ECANCELED (`list_clock`). Every setting drops the
 * timerfd's mark of a step (`stepped`) and of an expiration (`expired`),
 * and a disarming one keeps its interval, as Linux's timerfd_setup zeroes
 * `ticks` and `expired` and stores `tintv` whatever the time
 * (fs/timerfd.c:199-202).
 */
static int32_t arm(struct ktimer *t, int flags, const struct setting *s)
{
    int on = s->vsec != 0 || s->vnsec != 0;
    int base =
        t->clock == CLOCK_REALTIME && !(flags & TFD_TIMER_ABSTIME) ? CLOCK_MONOTONIC : t->clock;
    int64_t wsec = s->vsec;
    int32_t wnsec = s->vnsec;
    uint32_t sec, sub, isec, isub;
    struct reading r = {0, 0, 0};
    int32_t rc = 0;
    uint32_t irq;

    if (s->vsec < 0 || s->vnsec < 0 || s->vnsec >= NS_PER_SEC) {
        return -EINVAL;
    }
    if (on && t->cpu_tg && (flags & TFD_TIMER_ABSTIME)) {
        int64_t now;
        int32_t nsec;

        rc = clock_now(t->clock, &now, &nsec);
        if (rc != 0) {
            return rc;
        }
        wsec = s->vsec - now;
        wnsec = s->vnsec - nsec;
        if (wnsec < 0) {
            wnsec += NS_PER_SEC;
            wsec--;
        }
    }
    to_wait(wsec, wnsec, &sec, &sub);
    to_wait(s->isec, s->insec, &isec, &isub);
    irq = spin_lock_irqsave(&timer_lock);
    if (on && !t->cpu_tg) {
        rc = read_now(base, &r);
        if (rc != 0) {
            spin_unlock_irqrestore(&timer_lock, irq);
            return rc;
        }
    }
    if (!t->tg) {
        set_cancel(t, t->clock == CLOCK_REALTIME && (flags & TFD_TIMER_ABSTIME) &&
                          (flags & TFD_TIMER_CANCEL_ON_SET));
    }
    if (t->armed) {
        disarm(t);
    }
    if (t->tg) {
        ptimer_post_one(t);
        t->gen = next_gen();
        t->set_count = 0;
        t->between = 0;
    } else {
        t->count = 0;
    }
    t->overrun = 0;
    t->stepped = 0;
    t->expired = 0;
    if (!on && !t->tg) {
        t->ins = ns_of(s->isec, s->insec);
    }
    if (on && t->cpu_tg) {
        t->due = cpu_ticks_of(sec, sub);
        t->isec = isec;
        t->isub = isub;
        t->armed = 1;
        t->next = cpu_armed;
        cpu_armed = t;
    } else if (on) {
        rc = list_clock(t, base, flags, s, &r);
    }
    timer_unlock(irq);
    return rc;
}

/**
 * Read the struct itimerspec at `u` into `s`, the interval first. Its
 * seconds and nanoseconds are longs, 32 bits on i686 and 64 on x86-64, or
 * with `wide` 64 bits, the time64 calls' on i686. 0, -EFAULT, or -EINVAL for
 * nanoseconds outside 0 to 999999999 or a negative time.
 */
static int32_t get_setting(uintptr_t u, int wide, struct setting *s)
{
    if (wide) {
        int64_t v[4];

        if (copy_from_user(v, (const void __user *)u, sizeof(v)) != 0) {
            return -EFAULT;
        }
        if (v[1] < 0 || v[1] >= NS_PER_SEC || v[3] < 0 || v[3] >= NS_PER_SEC) {
            return -EINVAL;
        }
        s->isec = v[0];
        s->insec = (int32_t)v[1];
        s->vsec = v[2];
        s->vnsec = (int32_t)v[3];
    } else {
        long v[4];

        if (copy_from_user(v, (const void __user *)u, sizeof(v)) != 0) {
            return -EFAULT;
        }
        if (v[1] < 0 || v[1] >= NS_PER_SEC || v[3] < 0 || v[3] >= NS_PER_SEC) {
            return -EINVAL;
        }
        s->isec = v[0];
        s->insec = (int32_t)v[1];
        s->vsec = v[2];
        s->vnsec = (int32_t)v[3];
    }
    return s->isec < 0 || s->vsec < 0 ? -EINVAL : 0;
}

/** Write `s` to the struct itimerspec at `u`, of the width `wide` selects
 * (`get_setting`); 0 or -EFAULT. */
static int32_t put_setting(uintptr_t u, int wide, const struct setting *s)
{
    if (wide) {
        int64_t v[4];

        v[0] = s->isec;
        v[1] = s->insec;
        v[2] = s->vsec;
        v[3] = s->vnsec;
        return copy_to_user((void __user *)u, v, sizeof(v)) != 0 ? -EFAULT : 0;
    } else {
        long v[4];

        v[0] = (long)s->isec;
        v[1] = s->insec;
        v[2] = (long)s->vsec;
        v[3] = s->vnsec;
        return copy_to_user((void __user *)u, v, sizeof(v)) != 0 ? -EFAULT : 0;
    }
}

/** Arm `t` from the setting at `want_u`, which `flags` may make absolute,
 * and write the setting it had to `old_u` when that is not 0; 0, or the
 * error of the read, the arming or the write. The timer stays as it was
 * armed when the write of the old setting fails. */
static int32_t set_and_report(struct ktimer *t, int flags, uintptr_t want_u, uintptr_t old_u,
                              int wide)
{
    struct setting now, want;
    int32_t rc = get_setting(want_u, wide, &want);

    if (rc != 0) {
        return rc;
    }
    current_setting(t, &now);
    rc = arm(t, flags, &want);
    if (rc == 0 && old_u) {
        rc = put_setting(old_u, wide, &now);
    }
    return rc;
}

/** The body of timerfd_settime, of the time64 call when `wide`; returns as
 * `sys_timerfd_settime` does. */
static int32_t settime(struct isr_params *params, int wide)
{
    struct ktimer *t = timer_of((int)SC_ARG(params, 0));
    int flags = (int)SC_ARG(params, 1);

    if (!fd_get(current_proc->fds, (int)SC_ARG(params, 0))) {
        return -EBADF;
    }
    if (!t || (flags & ~(TFD_TIMER_ABSTIME | TFD_TIMER_CANCEL_ON_SET))) {
        return -EINVAL;
    }
    return set_and_report(t, flags, SC_ARG(params, 2), SC_ARG(params, 3), wide);
}

/** The body of timerfd_gettime, of the time64 call when `wide`; returns as
 * `sys_timerfd_gettime` does. */
static int32_t gettime(struct isr_params *params, int wide)
{
    struct ktimer *t = timer_of((int)SC_ARG(params, 0));
    struct setting now;

    if (!fd_get(current_proc->fds, (int)SC_ARG(params, 0))) {
        return -EBADF;
    }
    if (!t) {
        return -EINVAL;
    }
    current_setting(t, &now);
    return put_setting(SC_ARG(params, 1), wide, &now);
}

long sys_timerfd_settime(struct isr_params *params)
{
    return settime(params, 0);
}

long sys_timerfd_settime64(struct isr_params *params)
{
    return settime(params, 1);
}

long sys_timerfd_gettime(struct isr_params *params)
{
    return gettime(params, 0);
}

long sys_timerfd_gettime64(struct isr_params *params)
{
    return gettime(params, 1);
}

/** The thread `tid` of the process `tg`, or NULL when it has none that has
 * not ended. */
static process_t *thread_of(struct tgroup *tg, uint32_t tid)
{
    process_t *p = get_process(tid);

    return p && p->tg == tg && p->state != PROC_ZOMBIE ? p : NULL;
}

/** `overrun` and `more` added, no more than INT_MAX, as Linux's
 * timer_overrun_to_int limits it (kernel/time/posix-timers.c:237-242). */
static uint32_t overrun_add(uint32_t overrun, uint64_t more)
{
    return more >= 0x7FFFFFFFu - (uint64_t)overrun ? 0x7FFFFFFFu : overrun + (uint32_t)more;
}

/** The thread the signal of the POSIX timer `t` goes to, or NULL for its
 * process: the thread SIGEV_THREAD_ID named, while it has not ended. */
static process_t *ptimer_thread(const struct ktimer *t)
{
    return t->notify == SIGEV_THREAD_ID ? thread_of(t->tg, t->tid) : NULL;
}

/**
 * An expiration of the POSIX timer `t`, or `fresh` of them, which the
 * setting in force (`gen`) has counted; or, with `fresh` 0, only the post
 * of its waiting notification. Every expiration counts in `set_count`,
 * which the signal's delivery adds to its overrun (`ptimer_rearm`).
 *
 * The signal is posted with SI_TIMER, the timer's id, its value, its own
 * `key` and the setting, as Linux's posix_timer_queue_signal writes
 * si_sys_private before send_sigqueue queues the timer's sigqueue with an
 * overrun of 0 (kernel/time/posix-timers.c:281-305, kernel/signal.c:
 * 2038-2043). While its signal is pending, the timer's own as its key says,
 * or while its notification waits for another sender's signal of its
 * number in `count`, an expiration is one more of the setting in force; or,
 * when the queued notification is of an earlier setting, it adds one to
 * that notification's overrun and makes it of this setting, as
 * send_sigqueue increments si_overrun of a sigqueue still queued
 * (kernel/signal.c:2034-2042). Linux stops the timer meanwhile, so no
 * setting expires twice into it; here the ticks go on counting it, into
 * `set_count`. A notification that waits is posted once that signal is
 * taken (`ptimer_rearm`), oldest first (`wait_seq`). SIGEV_NONE posts
 * nothing; a signal for a thread SIGEV_THREAD_ID named that has ended goes
 * to the process.
 *
 * Returns whether the timer's next time may be armed between ticks: 1 when
 * the signal is set pending now, or when the process ignores it, which
 * posting drops, and the interval is a tick or more; 0 for SIGEV_NONE, a
 * pending signal, or an ignored one of a shorter interval. Linux restarts a
 * timer whose signal is pending only when that signal is delivered
 * (posixtimer_rearm, kernel/time/posix-timers.c:258-279), which here arms
 * it between ticks again (`ptimer_rearm`); restarts one whose signal is
 * ignored at once, moved a tick on only when its interval is shorter
 * (kernel/time/posix-timers.c:318-374), which here leaves it to the ticks;
 * and never starts SIGEV_NONE's hrtimer (kernel/time/posix-timers.c:818).
 * Meanwhile the ticks count the timer. The caller holds `timer_lock`, and
 * the big lock, under which a signal is posted, unless `t` is SIGEV_NONE's.
 */
static int ptimer_fire(struct ktimer *t, uint64_t fresh)
{
    process_t *thread = ptimer_thread(t);
    uint64_t pending = thread ? thread->sig_pending : t->tg->pending;
    struct k_siginfo *held;
    struct k_siginfo info;

    if (t->notify == SIGEV_NONE) {
        t->count = 0;
        t->set_count = 0;
        return 0;
    }
    held = thread ? &thread->sig_info[t->signo] : &t->tg->info[t->signo];
    t->set_count += fresh;
    if (fresh != 0 && (pending & KSIG_BIT(t->signo)) && held->code == SI_TIMER &&
        held->timer_key == t->key) {
        if (held->timer_gen != t->gen) {
            held->uid = overrun_add(held->uid, 1u);
            held->timer_gen = t->gen;
        }
        return 0;
    }
    if (fresh != 0 && t->count == 0) {
        t->count = 1;
        t->count_gen = t->gen;
        t->wait_seq = ++wait_order;
    } else if (fresh != 0 && t->count_gen != t->gen) {
        t->count = (uint64_t)overrun_add((uint32_t)(t->count - 1u), 1u) + 1u;
        t->count_gen = t->gen;
    }
    if (t->count == 0 || (pending & KSIG_BIT(t->signo))) {
        return 0;
    }
    k_memset(&info, 0, sizeof(info));
    info.code = SI_TIMER;
    info.pid = (uint32_t)t->id;           /* si_timerid, where a child's pid is */
    info.uid = (uint32_t)(t->count - 1u); /* si_overrun */
    info.value = t->value;                /* si_value */
    info.status = (int32_t)t->value;
    info.timer_gen = t->count_gen;
    info.timer_key = t->key;
    t->count = 0;
    fire_cpu = arch_cpu_id() + 1u;
    if (thread) {
        signal_post_thread(thread, t->signo, &info);
        pending = thread->sig_pending;
    } else {
        signal_post_info(t->tg->leader, t->signo, &info);
        pending = t->tg->pending;
    }
    fire_cpu = 0;
    if (!(pending & KSIG_BIT(t->signo))) {
        t->set_count = 0; /* ignored: dropped, and no delivery counts it */
    }
    return (pending & KSIG_BIT(t->signo)) != 0 || t->ins >= (uint64_t)NS_PER_TICK;
}

/** `t` when its notification waits for the signal `signo`, which is no
 * longer pending where `t` posts it, and it began to wait before `first`'s,
 * or `first` has none; `first` otherwise. The caller holds `timer_lock`. */
static struct ktimer *earlier(struct ktimer *first, struct ktimer *t, int signo)
{
    process_t *thread;
    uint64_t pending;

    if (t->count == 0 || t->notify == SIGEV_NONE || t->signo != signo ||
        (first && first->wait_seq < t->wait_seq)) {
        return first;
    }
    thread = ptimer_thread(t);
    pending = thread ? thread->sig_pending : t->tg->pending;
    return pending & KSIG_BIT(signo) ? first : t;
}

/**
 * Post the notifications of `tg`'s timers, live or deleted (`timers_gone`),
 * that wait for the signal `signo` where it is no longer pending, oldest
 * first (`wait_seq`), as Linux's collect_signal delivers the first sigqueue
 * of a number queued (kernel/signal.c:566-582), so that no later
 * expiration overtakes one: each post sets the signal pending again where
 * it goes, and those that wait there go on waiting. A live timer whose
 * signal that sets pending is armed between ticks again. The caller holds
 * `timer_lock`.
 */
static void post_waiting(struct tgroup *tg, int signo)
{
    struct ktimer *t, *first;
    struct reading r;
    int between;

    for (;;) {
        first = NULL;
        for (t = tg->timers; t; t = t->in_group) {
            first = earlier(first, t, signo);
        }
        for (t = tg->timers_gone; t; t = t->in_group) {
            first = earlier(first, t, signo);
        }
        if (!first) {
            return;
        }
        between = ptimer_fire(first, 0);
        if (first->armed && !first->cpu_tg && read_now(first->base, &r) == 0) {
            first->between = between;
            if (between) {
                set_due(first, &r, 0);
            }
        }
    }
}

/** Take off `tg`'s `timers_gone` each deleted timer whose notification no
 * longer waits, and return them, on their `in_group`, for the caller to
 * free once it lets `timer_lock` go, which it holds. */
static struct ktimer *gone_reap(struct tgroup *tg)
{
    struct ktimer *t, **link = &tg->timers_gone, *done = NULL;

    while (*link) {
        t = *link;
        if (t->count == 0) {
            *link = t->in_group;
            t->in_group = done;
            done = t;
        } else {
            link = &t->in_group;
        }
    }
    return done;
}

/** Free the deleted timers `done`, linked on their `in_group`. */
static void gone_free(struct ktimer *done)
{
    struct ktimer *t;

    while (done) {
        t = done;
        done = t->in_group;
        kfree(t);
    }
}

/** Put the deleted timers `done`, linked on their `in_group`, on
 * `gone_later`, to be freed when `timer_lock`, which the caller holds, is
 * let go (`timer_unlock`). */
static void gone_defer(struct ktimer *done)
{
    struct ktimer *t;

    while (done) {
        t = done;
        done = t->in_group;
        t->in_group = gone_later;
        gone_later = t;
    }
}

/**
 * Let `timer_lock` go, with the interrupt flag `irq` its taking returned,
 * and free the deleted timers' records on `gone_later`: those a discard
 * let go while a signal was posted under the lock (`ptimer_discard`), as
 * Linux's flush_sigqueue_mask frees a deleted timer's sigqueue as it takes
 * it off the queue (kernel/signal.c:444-452, 794-795), and not at some
 * later dequeue that may never come. Every operation that can post a
 * timer's signal under the lock lets it go here.
 */
static void timer_unlock(uint32_t irq)
{
    struct ktimer *done = gone_later;

    gone_later = NULL;
    spin_unlock_irqrestore(&timer_lock, irq);
    gone_free(done);
}

/**
 * The signal `signo` of the process `tg`, whose siginfo is `info`, has
 * been taken off its pending signals, with interrupts off since then, so
 * that no tick posts the timer's signal again before its overrun is counted,
 * as dequeue_signal rearms the timer before it lets interrupts on
 * (kernel/signal.c:682-699). When it is a periodic timer's own (`key`),
 * posted or last added to for the setting in force (`gen`), as
 * posixtimer_rearm compares si_sys_private with it_requeue_pending
 * (kernel/time/posix-timers.c:267), the intervals the timer's clock has
 * passed are counted, its time moves past the clock and is armed between
 * ticks again, and the setting's expirations since its signal was queued
 * (`set_count`), less the one the signal stands for, are added to the
 * signal's overrun and become the timer's `overrun`, as posixtimer_rearm
 * forwards and restarts the timer, sets it_overrun_last from it_overrun and
 * adds that to si_overrun, for a periodic timer alone (kernel/time/
 * posix-timers.c:237-242, 244-251, 258-279). A signal of an earlier
 * setting, or of a deleted timer, adds none and forwards nothing, as
 * Linux's posix_timer_set_common resets it_overrun and collect_signal
 * rearms no sigqueue that has lost SIGQUEUE_PREALLOC (kernel/time/
 * posix-timers.c:879-882, kernel/signal.c:589-593). A timer of processor
 * time has its expirations counted at every tick charged, so only its
 * overrun is taken. Then the notifications that waited for the signal are
 * posted (`post_waiting`), and a deleted timer whose notification no longer
 * waits is freed. The interrupt between ticks is programmed once for them
 * all (`timer_wake_hold`). Called without `timer_lock`.
 */
void ptimer_rearm(struct tgroup *tg, int signo, struct k_siginfo *info)
{
    uint32_t irq;
    struct ktimer *t;
    struct reading r;

    if (!tg->timers && !tg->timers_gone) {
        return;
    }
    irq = spin_lock_irqsave(&timer_lock);
    timer_wake_hold();
    for (t = tg->timers; t; t = t->in_group) {
        int periodic = t->cpu_tg ? t->isec != 0 || t->isub != 0 : t->ins != 0;

        if (t->notify == SIGEV_NONE || t->signo != signo || info->code != SI_TIMER ||
            info->timer_key != t->key || info->timer_gen != t->gen) {
            continue;
        }
        if (!t->armed || !periodic) {
            t->set_count = 0;
        } else if (t->cpu_tg || read_now(t->base, &r) == 0) {
            if (!t->cpu_tg && r.clock >= t->end) {
                t->set_count += forward_end(t, r.clock);
            }
            if (t->set_count != 0) {
                info->uid = overrun_add(info->uid, t->set_count - 1u);
            }
            t->overrun = t->set_count != 0 ? overrun_add(0, t->set_count - 1u) : 0;
            t->set_count = 0;
            if (!t->cpu_tg) {
                t->between = 1;
                set_due(t, &r, 0);
            }
        }
        break;
    }
    post_waiting(tg, signo);
    gone_defer(gone_reap(tg));
    timer_wake_release();
    timer_unlock(irq);
}

/**
 * The signals `bits` of the process `tg` have been discarded, its and each
 * of its threads': a disposition that ignores them, or a stop or SIGCONT
 * that cancels the other. Each notification of a POSIX timer of those
 * signals that waits goes too, and with it a deleted timer's record, and a
 * live timer's expirations since its signal was queued (`set_count`), as
 * Linux's flush_sigqueue_mask takes every sigqueue of those numbers off the
 * queues and frees a deleted timer's (kernel/signal.c:444-452, 782-798,
 * 915-931, 4249-4255). With interrupts off; `timer_lock` is taken here
 * unless this processor holds it to post a signal (`fire_cpu`), whose job
 * control discards the others, when the records are freed as the poster
 * lets the lock go (`gone_defer`, `timer_unlock`).
 */
void ptimer_discard(struct tgroup *tg, uint64_t bits)
{
    int held = fire_cpu == arch_cpu_id() + 1u;
    struct ktimer *t;
    uint32_t irq = 0;

    if (!tg->timers && !tg->timers_gone) {
        return;
    }
    if (!held) {
        irq = spin_lock_irqsave(&timer_lock);
    }
    for (t = tg->timers; t; t = t->in_group) {
        if (t->notify != SIGEV_NONE && (bits & KSIG_BIT(t->signo))) {
            t->count = 0;
            t->set_count = 0;
        }
    }
    for (t = tg->timers_gone; t; t = t->in_group) {
        if (bits & KSIG_BIT(t->signo)) {
            t->count = 0;
        }
    }
    gone_defer(gone_reap(tg));
    if (!held) {
        timer_unlock(irq);
    }
}

/** The number of `tg`'s POSIX timers, those deleted whose notifications
 * still wait (`timers_gone`) among them. The caller holds `timer_lock`. */
static int ptimer_count(const struct tgroup *tg)
{
    const struct ktimer *t;
    int n = 0;

    for (t = tg->timers; t; t = t->in_group) {
        n++;
    }
    for (t = tg->timers_gone; t; t = t->in_group) {
        n += t->count != 0;
    }
    return n;
}

/** The lowest id `tg` has no timer of, or PTIMER_MAX when it has PTIMER_MAX
 * timers, deleted ones whose notifications wait among them (`ptimer_count`).
 * The caller holds `timer_lock`. */
static int free_id(const struct tgroup *tg)
{
    const struct ktimer *t;
    int id = 0;

    if (ptimer_count(tg) >= PTIMER_MAX) {
        return PTIMER_MAX;
    }
    for (t = tg->timers; t && t->id == id; t = t->in_group) {
        id++;
    }
    return id;
}

/** The calling process's POSIX timer `id`, or NULL. */
static struct ktimer *ptimer_of(int id)
{
    struct ktimer *t = current_proc->tg->timers;

    while (t && t->id != id) {
        t = t->in_group;
    }
    return t;
}

/** Take the target of the clock `clock` into `t`: none for real time and time
 * since boot, and for a clock of processor time the process it counts,
 * held, its thread and whether it counts user time alone. 0, or -EINVAL
 * for a clock `clock_cpu_target` refuses. */
static int32_t ptimer_clock(struct ktimer *t, int clock)
{
    if (clock == CLOCK_REALTIME || clock == CLOCK_MONOTONIC || clock == CLOCK_BOOTTIME) {
        return 0;
    }
    return clock_cpu_target(clock, &t->cpu_tg, &t->cpu_tid, &t->cpu_user);
}

/**
 * The id is stored before the timer is on the process's list, since the
 * store can sleep in a page fault, which the timers' lock cannot be held
 * across, and a timer already listed could be deleted by another thread
 * meanwhile. A thread that takes the id while the store sleeps has the next
 * free one tried. A deleted timer whose notification still waits counts
 * against PTIMER_MAX until it is posted or discarded, as the sigqueue
 * Linux leaves queued counts against RLIMIT_SIGPENDING until it is freed
 * (kernel/signal.c:428, 444-450, 1956-1959).
 */
long sys_timer_create(struct isr_params *params)
{
    int clock = (int)SC_ARG(params, 0);
    struct tgroup *tg = current_proc->tg;
    struct ktimer *t, **link = &tg->timers;
    struct {
        uintptr_t value;
        int32_t signo;
        int32_t notify;
        int32_t tid;
    } ev; /* the head of Linux's struct sigevent */
    uint32_t irq;
    int id = 0;

    ev.value = 0;
    ev.signo = SIGALRM;
    ev.notify = SIGEV_SIGNAL;
    ev.tid = 0;
    if (SC_ARG(params, 1) &&
        copy_from_user(&ev, (const void __user *)SC_ARG(params, 1), sizeof(ev)) != 0) {
        return -EFAULT;
    }
    if ((ev.notify != SIGEV_SIGNAL && ev.notify != SIGEV_NONE && ev.notify != SIGEV_THREAD_ID) ||
        (ev.notify != SIGEV_NONE && (ev.signo < 1 || ev.signo > KSIG_MAX)) ||
        (ev.notify == SIGEV_THREAD_ID && !thread_of(tg, (uint32_t)ev.tid))) {
        return -EINVAL;
    }
    t = kzalloc(sizeof(*t));
    if (!t) {
        return -EAGAIN;
    }
    if (ptimer_clock(t, clock) != 0) {
        kfree(t);
        return -EINVAL;
    }
    for (;;) {
        irq = spin_lock_irqsave(&timer_lock);
        id = free_id(tg);
        spin_unlock_irqrestore(&timer_lock, irq);
        if (id >= PTIMER_MAX ||
            copy_to_user((void __user *)SC_ARG(params, 2), &id, sizeof(id)) != 0) {
            tgroup_put(t->cpu_tg);
            kfree(t);
            return id >= PTIMER_MAX ? -EAGAIN : -EFAULT;
        }
        irq = spin_lock_irqsave(&timer_lock);
        for (link = &tg->timers; *link && (*link)->id < id; link = &(*link)->in_group) {
        }
        if ((!*link || (*link)->id != id) && ptimer_count(tg) < PTIMER_MAX) {
            break;
        }
        spin_unlock_irqrestore(&timer_lock, irq);
    }
    t->clock = clock;
    t->tg = tg;
    t->id = id;
    t->notify = ev.notify;
    t->signo = ev.signo;
    t->value = SC_ARG(params, 1) ? ev.value : (uintptr_t)id;
    t->tid = (uint32_t)ev.tid;
    t->key = next_gen();
    t->in_group = *link;
    *link = t;
    spin_unlock_irqrestore(&timer_lock, irq);
    return 0;
}

/** The body of timer_settime, of the time64 call when `wide`; returns as
 * `sys_timer_settime` does. */
static int32_t ptimer_settime(struct isr_params *params, int wide)
{
    struct ktimer *t = ptimer_of((int)SC_ARG(params, 0));

    if (!t || ((int)SC_ARG(params, 1) & ~TIMER_ABSTIME) || SC_ARG(params, 2) == 0) {
        return -EINVAL;
    }
    return set_and_report(t, (int)SC_ARG(params, 1), SC_ARG(params, 2), SC_ARG(params, 3), wide);
}

/** The body of timer_gettime, of the time64 call when `wide`; returns as
 * `sys_timer_gettime` does. */
static int32_t ptimer_gettime(struct isr_params *params, int wide)
{
    struct ktimer *t = ptimer_of((int)SC_ARG(params, 0));
    struct setting now;

    if (!t) {
        return -EINVAL;
    }
    current_setting(t, &now);
    return put_setting(SC_ARG(params, 1), wide, &now);
}

long sys_timer_settime(struct isr_params *params)
{
    return ptimer_settime(params, 0);
}

long sys_timer_settime64(struct isr_params *params)
{
    return ptimer_settime(params, 1);
}

long sys_timer_gettime(struct isr_params *params)
{
    return ptimer_gettime(params, 0);
}

long sys_timer_gettime64(struct isr_params *params)
{
    return ptimer_gettime(params, 1);
}

long sys_timer_getoverrun(struct isr_params *params)
{
    struct ktimer *t = ptimer_of((int)SC_ARG(params, 0));

    return t ? (int32_t)t->overrun : -EINVAL;
}

/**
 * Take the timer `link` names out of its process's list, disarmed, let go
 * of the process whose time it counts, and free it; or, with `keep`, when
 * its notification waits for another sender's signal of its number
 * (`ptimer_fire`), keep it on the process's `timers_gone` until that signal
 * is taken and it is posted (`ptimer_rearm`), or it is discarded
 * (`ptimer_discard`), as Linux's sigqueue_free leaves a queued sigqueue to
 * be delivered (kernel/signal.c:1961-1984). The notification keeps only
 * its own overrun, since no delivery forwards a deleted timer. With `keep`,
 * an expiration the tick counted and left for a holder of the big lock
 * (`ptimer_fire_later`) is signalled first, as Linux signalled it at the
 * expiry; without it, at an exec, whose signals of the timers go, and when
 * the last thread has ended, it is dropped. The caller holds the big lock.
 */
static void ptimer_free(struct ktimer **link, int keep)
{
    struct ktimer *t = *link;
    struct tgroup *cpu_tg = t->cpu_tg;
    uint32_t irq = spin_lock_irqsave(&timer_lock);

    if (t->armed) {
        disarm(t);
    }
    if (keep) {
        ptimer_post_one(t);
    } else {
        (void)ptimer_fire_cancel(t);
    }
    *link = t->in_group;
    t->cpu_tg = NULL;
    keep = keep && t->count != 0 && t->notify != SIGEV_NONE;
    if (keep) {
        t->in_group = t->tg->timers_gone;
        t->tg->timers_gone = t;
    }
    timer_unlock(irq);
    tgroup_put(cpu_tg);
    if (!keep) {
        kfree(t);
    }
}

long sys_timer_delete(struct isr_params *params)
{
    struct ktimer **link = &current_proc->tg->timers;

    while (*link && (*link)->id != (int)SC_ARG(params, 0)) {
        link = &(*link)->in_group;
    }
    if (!*link) {
        return -EINVAL;
    }
    ptimer_free(link, 1);
    return 0;
}

void ptimer_delete_all(struct tgroup *tg)
{
    while (tg->timers) {
        ptimer_free(&tg->timers, 0);
    }
    while (tg->timers_gone) {
        ptimer_free(&tg->timers_gone, 0);
    }
}

/**
 * Delete the POSIX timers of the process of `self` at an exec, and discard
 * their pending signals. Every timer goes, with the records of deleted ones
 * that wait (`ptimer_delete_all`), and then each signal of SI_TIMER pending
 * for `self` or for its process is discarded, as Linux 6.12.115's
 * posixtimer_exec calls exit_itimers and flush_itimer_signals
 * (fs/exec.c:1220-1229); flush_itimer_signals takes every queued
 * SI_TIMER sigqueue off the thread's and the process's queues
 * (kernel/signal.c:483-516), so that the new image takes no signal of a
 * timer it never made; a signal whose siginfo is another sender's stays.
 * The thread's mark of signal work may stay set, as Linux leaves
 * TIF_SIGPENDING, and finds nothing.
 */
void ptimer_exec(process_t *self)
{
    uint32_t irq;
    int signo;

    ptimer_delete_all(self->tg);
    irq = kirq_save();
    for (signo = 1; signo <= KSIG_MAX; signo++) {
        if (self->tg->info[signo].code == SI_TIMER) {
            self->tg->pending &= ~KSIG_BIT(signo);
        }
        if (self->sig_info[signo].code == SI_TIMER) {
            self->sig_pending &= ~KSIG_BIT(signo);
        }
    }
    kirq_restore(irq);
}
