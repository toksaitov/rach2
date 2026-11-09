/*
 * ia32_time.c: the i386 forms of the time calls, with 32-bit times, and the
 * _time64 calls whose 64-bit timespec Linux reads with the high word of its
 * nanoseconds ignored, for the compat table (ia32_systab.c).
 */

#include "ia32.h"
#include "ia32_abi.h"

#include "../arch.h"
#include "../../fs/fd.h"
#include "../../fs/krffs.h"
#include "../../fs/vfs.h"
#include "../../io/epoll.h"
#include "../../io/poll.h"
#include "../../io/timerfd.h"
#include "../../lib/kstring.h"
#include "../../mm/usercopy.h"
#include "../../network/socket/socket.h"
#include "../../proc/process.h"
#include "../../syscall.h"
#include "../../syscall_internal.h"

/*
 * Each call converts the i386 structure into the x86-64 one, a pair of longs
 * for each time, and runs the x86-64 handler on it (`ia32_native`), whose
 * checks, clocks and errors are then the x86-64 call's; what the handler
 * writes back is converted to the i386 structure. A remainder or an old
 * setting the handler may leave unwritten is marked first with nanoseconds
 * of -1, which no handler writes, and copied out only when the handler wrote
 * it.
 */

/* A user pointer of an i386 call, zero-extended, and a conversion buffer's
 * address, as native arguments. */
#define UPTR(v) ((unsigned long)(uint32_t)(v))
#define KPTR(p) ((unsigned long)(uintptr_t)(p))

/* The mark of a time the handler has not written. */
#define UNWRITTEN (-1L)

/* Copy the pair `t` out as an i386 timespec or timeval, both two 32-bit
 * words, to `u`; 0 or -EFAULT. Linux cuts the seconds to 32 bits. */
static int put_time32(uintptr_t u, const long t[2])
{
    int32_t w[2];

    w[0] = (int32_t)t[0];
    w[1] = (int32_t)t[1];
    return copy_to_user((void __user *)u, w, sizeof(w)) != 0 ? -EFAULT : 0;
}

/* Read `n` i386 times, pairs of 32-bit words, at `u` into `t`, sign-extended;
 * 0 or -EFAULT. */
static int get_times32(uintptr_t u, long *t, int n)
{
    int32_t w[4];
    int i;

    if (copy_from_user(w, (const void __user *)u, (size_t)n * 8u) != 0) {
        return -EFAULT;
    }
    for (i = 0; i < 2 * n; i++) {
        t[i] = w[i];
    }
    return 0;
}

/* Copy `n` pairs of `t` out as i386 times to `u`; 0 or -EFAULT. */
static int put_times32(uintptr_t u, const long *t, int n)
{
    int32_t w[4];
    int i;

    for (i = 0; i < 2 * n; i++) {
        w[i] = (int32_t)t[i];
    }
    return copy_to_user((void __user *)u, w, (size_t)n * 8u) != 0 ? -EFAULT : 0;
}

/** time (13): real time's seconds, also stored as 32 bits at the address in
 * argument 0 when it is not 0. */
long compat_sys_time32(isr_params_t *params)
{
    int64_t sec, nsec;
    int32_t t;
    uintptr_t u = UPTR(SC_ARG(params, 0));

    get_current_time(&sec, &nsec);
    t = (int32_t)sec;
    if (u && copy_to_user((void __user *)u, &t, sizeof(t)) != 0) {
        return -EFAULT;
    }
    return t;
}

/** gettimeofday (78): an i386 timeval and the time zone, which the x86-64
 * call ignores as well. */
long compat_sys_gettimeofday(isr_params_t *params)
{
    long tv[2];
    uintptr_t u = UPTR(SC_ARG(params, 0));
    long rc = ia32_native(params, sys_gettimeofday, tv, sizeof(tv), u ? KPTR(tv) : 0,
                          UPTR(SC_ARG(params, 1)), 0, 0, 0, 0);

    if (rc != 0 || !u) {
        return rc;
    }
    return put_time32(u, tv);
}

/** settimeofday (79): an i386 timeval, or 0, and the time zone. */
long compat_sys_settimeofday(isr_params_t *params)
{
    long tv[2];
    uintptr_t u = UPTR(SC_ARG(params, 0));

    if (u && get_times32(u, tv, 1) != 0) {
        return -EFAULT;
    }
    return ia32_native(params, sys_settimeofday, tv, sizeof(tv), u ? KPTR(tv) : 0,
                       UPTR(SC_ARG(params, 1)), 0, 0, 0, 0);
}

/** setitimer (104): the timer, the new i386 itimerval and the address for the
 * old one, or 0. */
long compat_sys_setitimer(isr_params_t *params)
{
    long t[8]; /* the new setting, then the old */
    uintptr_t unew = UPTR(SC_ARG(params, 1));
    uintptr_t uold = UPTR(SC_ARG(params, 2));
    long rc;

    if (unew && get_times32(unew, t, 2) != 0) {
        return -EFAULT;
    }
    rc = ia32_native(params, sys_setitimer, t, sizeof(t), (unsigned long)(int)SC_ARG(params, 0),
                     unew ? KPTR(t) : 0, uold ? KPTR(t + 4) : 0, 0, 0, 0);
    if (rc != 0 || !uold) {
        return rc;
    }
    return put_times32(uold, t + 4, 2);
}

/** getitimer (105): the timer and the address for its i386 itimerval. */
long compat_sys_getitimer(isr_params_t *params)
{
    long t[4];
    uintptr_t u = UPTR(SC_ARG(params, 1));
    long rc = ia32_native(params, sys_getitimer, t, sizeof(t),
                          (unsigned long)(int)SC_ARG(params, 0), u ? KPTR(t) : 0, 0, 0, 0, 0);

    if (rc != 0) {
        return rc;
    }
    return put_times32(u, t, 2);
}

/* The sleep of `fn`, nanosleep or clock_nanosleep, with the request read
 * into `t` and the remainder at `urem`: the handler's arguments are `a0` and
 * `a1` before the request for clock_nanosleep, and none for nanosleep. The
 * remainder is copied out as an i386 timespec, or with `wide` as a 64-bit
 * one, when the handler wrote it. */
static long sleep_call(isr_params_t *params, syscall_fn_t fn, int clock, long t[4], uintptr_t urem,
                       int wide)
{
    unsigned long kreq = KPTR(t), krem = urem ? KPTR(t + 2) : 0;
    long rc;

    t[2] = 0;
    t[3] = UNWRITTEN;
    if (clock) {
        rc = ia32_native(params, fn, t, 4 * sizeof(long), (unsigned long)(int)SC_ARG(params, 0),
                         (unsigned long)(int)SC_ARG(params, 1), kreq, krem, 0, 0);
    } else {
        rc = ia32_native(params, fn, t, 4 * sizeof(long), kreq, krem, 0, 0, 0, 0);
    }
    if (urem && t[3] != UNWRITTEN) {
        int bad;

        if (wide) {
            struct ia32_timespec64 r;

            IA32_PUT64(&r, tv_sec, t[2]);
            IA32_PUT64(&r, tv_nsec, t[3]);
            bad = copy_to_user((void __user *)urem, &r, sizeof(r)) != 0;
        } else {
            bad = put_time32(urem, t + 2) != 0;
        }
        if (bad && rc == 0) {
            return -EFAULT;
        }
    }
    return rc;
}

/** nanosleep (162): the i386 timespec of the interval and the address for
 * the remainder, or 0. */
long compat_sys_nanosleep_time32(isr_params_t *params)
{
    long t[4];
    uintptr_t ureq = UPTR(SC_ARG(params, 0));

    if (!ureq || ia32_get_timespec(ureq, t) != 0) {
        return -EFAULT;
    }
    return sleep_call(params, sys_nanosleep, 0, t, UPTR(SC_ARG(params, 1)), 0);
}

/** clock_nanosleep (267): the clock, the flags, the i386 timespec of the
 * request and the address for the remainder, or 0. */
long compat_sys_clock_nanosleep_time32(isr_params_t *params)
{
    long t[4];
    uintptr_t ureq = UPTR(SC_ARG(params, 2));

    if (!ureq || ia32_get_timespec(ureq, t) != 0) {
        return -EFAULT;
    }
    return sleep_call(params, sys_clock_nanosleep, 1, t, UPTR(SC_ARG(params, 3)), 0);
}

/** clock_nanosleep_time64 (407): clock_nanosleep with 64-bit timespecs, the
 * high word of the request's nanoseconds ignored. */
long compat_sys_clock_nanosleep_time64(isr_params_t *params)
{
    long t[4];
    uintptr_t ureq = UPTR(SC_ARG(params, 2));

    if (!ureq || ia32_get_timespec64(ureq, t) != 0) {
        return -EFAULT;
    }
    return sleep_call(params, sys_clock_nanosleep, 1, t, UPTR(SC_ARG(params, 3)), 1);
}

/** clock_settime (264): the clock and an i386 timespec. */
long compat_sys_clock_settime32(isr_params_t *params)
{
    long t[2];

    if (ia32_get_timespec(UPTR(SC_ARG(params, 1)), t) != 0) {
        return -EFAULT;
    }
    return ia32_native(params, sys_clock_settime, t, sizeof(t),
                       (unsigned long)(int)SC_ARG(params, 0), KPTR(t), 0, 0, 0, 0);
}

/** clock_settime64 (404): the clock and a 64-bit timespec, the high word of
 * its nanoseconds ignored. */
long compat_sys_clock_settime64(isr_params_t *params)
{
    long t[2];

    if (ia32_get_timespec64(UPTR(SC_ARG(params, 1)), t) != 0) {
        return -EFAULT;
    }
    return ia32_native(params, sys_clock_settime, t, sizeof(t),
                       (unsigned long)(int)SC_ARG(params, 0), KPTR(t), 0, 0, 0, 0);
}

/* clock_gettime or clock_getres, `fn`: the clock and the address for an
 * i386 timespec, which clock_getres may leave 0. */
static long clock_read32(isr_params_t *params, syscall_fn_t fn)
{
    long t[2];
    uintptr_t u = UPTR(SC_ARG(params, 1));
    long rc = ia32_native(params, fn, t, sizeof(t), (unsigned long)(int)SC_ARG(params, 0),
                          u ? KPTR(t) : 0, 0, 0, 0, 0);

    if (rc != 0 || !u) {
        return rc;
    }
    return put_time32(u, t);
}

/** clock_gettime (265): the clock and the address for an i386 timespec. */
long compat_sys_clock_gettime32(isr_params_t *params)
{
    return clock_read32(params, sys_clock_gettime);
}

/** clock_getres (266): the clock and the address for an i386 timespec, or
 * 0. */
long compat_sys_clock_getres_time32(isr_params_t *params)
{
    return clock_read32(params, sys_clock_getres);
}

/** timer_create (259): the clock, an i386 sigevent or 0, whose 4-byte value
 * moves the fields after it (compat_sigevent), and the address for the id,
 * an int in both. */
long compat_sys_timer_create(isr_params_t *params)
{
    struct ia32_sigevent ce;
    struct {
        unsigned long value;
        int32_t signo;
        int32_t notify;
        int32_t tid;
        int32_t pad[11];
    } ev; /* Linux's x86-64 struct sigevent */
    uintptr_t uev = UPTR(SC_ARG(params, 1));

    if (uev) {
        if (copy_from_user(&ce, (const void __user *)uev, sizeof(ce)) != 0) {
            return -EFAULT;
        }
        k_memset(&ev, 0, sizeof(ev));
        ev.value = ce.sigev_value;
        ev.signo = ce.sigev_signo;
        ev.notify = ce.sigev_notify;
        ev.tid = ce.sigev_tid;
    }
    return ia32_native(params, sys_timer_create, &ev, sizeof(ev),
                       (unsigned long)(int)SC_ARG(params, 0), uev ? KPTR(&ev) : 0,
                       UPTR(SC_ARG(params, 2)), 0, 0, 0);
}

/* timer_settime or timerfd_settime, `fn`: the timer, the flags, the new
 * itimerspec, an i386 one or with `wide` one of 64-bit timespecs, and the
 * address for the old one, or 0, written in the same form. */
static long settime_call(isr_params_t *params, syscall_fn_t fn, int wide)
{
    long t[8]; /* the new setting, then the old */
    uintptr_t unew = UPTR(SC_ARG(params, 2));
    uintptr_t uold = UPTR(SC_ARG(params, 3));
    long rc;

    if (unew) {
        int bad = wide ? ia32_get_timespec64(unew, t) != 0 ||
                             ia32_get_timespec64(unew + sizeof(struct ia32_timespec64), t + 2) != 0
                       : get_times32(unew, t, 2) != 0;

        if (bad) {
            return -EFAULT;
        }
    }
    t[5] = UNWRITTEN;
    rc = ia32_native(params, fn, t, sizeof(t), (unsigned long)(int)SC_ARG(params, 0),
                     (unsigned long)(int)SC_ARG(params, 1), unew ? KPTR(t) : 0,
                     uold ? KPTR(t + 4) : 0, 0, 0);
    if (!uold || t[5] == UNWRITTEN) {
        return rc;
    }
    if (wide) {
        struct ia32_timespec64 o[2];
        int i;

        for (i = 0; i < 2; i++) {
            IA32_PUT64(&o[i], tv_sec, t[4 + 2 * i]);
            IA32_PUT64(&o[i], tv_nsec, t[5 + 2 * i]);
        }
        if (copy_to_user((void __user *)uold, o, sizeof(o)) != 0 && rc == 0) {
            return -EFAULT;
        }
    } else if (put_times32(uold, t + 4, 2) != 0 && rc == 0) {
        return -EFAULT;
    }
    return rc;
}

/* timer_gettime or timerfd_gettime, `fn`: the timer and the address for its
 * i386 itimerspec. */
static long gettime_call(isr_params_t *params, syscall_fn_t fn)
{
    long t[4];
    uintptr_t u = UPTR(SC_ARG(params, 1));
    long rc = ia32_native(params, fn, t, sizeof(t), (unsigned long)(int)SC_ARG(params, 0),
                          u ? KPTR(t) : 0, 0, 0, 0, 0);

    if (rc != 0) {
        return rc;
    }
    return put_times32(u, t, 2);
}

/** timer_settime (260): the id, the flags, the new i386 itimerspec and the
 * address for the old one. */
long compat_sys_timer_settime32(isr_params_t *params)
{
    return settime_call(params, sys_timer_settime, 0);
}

/** timer_settime64 (409): timer_settime with 64-bit timespecs, the high word
 * of each new nanoseconds ignored. */
long compat_sys_timer_settime64(isr_params_t *params)
{
    return settime_call(params, sys_timer_settime, 1);
}

/** timer_gettime (261): the id and the address for an i386 itimerspec. */
long compat_sys_timer_gettime32(isr_params_t *params)
{
    return gettime_call(params, sys_timer_gettime);
}

/** timerfd_settime (325): the descriptor, the flags, the new i386
 * itimerspec and the address for the old one. */
long compat_sys_timerfd_settime32(isr_params_t *params)
{
    return settime_call(params, sys_timerfd_settime, 0);
}

/** timerfd_settime64 (411): timerfd_settime with 64-bit timespecs, the high
 * word of each new nanoseconds ignored. */
long compat_sys_timerfd_settime64(isr_params_t *params)
{
    return settime_call(params, sys_timerfd_settime, 1);
}

/** timerfd_gettime (326): the descriptor and the address for an i386
 * itimerspec. */
long compat_sys_timerfd_gettime32(isr_params_t *params)
{
    return gettime_call(params, sys_timerfd_gettime);
}

/*
 * The calls that set a file's times. Each makes the pair of timespecs
 * utimensat takes and runs the x86-64 utimensat, whose permission checks are
 * Linux's do_utimes for every one of them: utime and utimes, of a path from
 * the working directory, futimesat, of a path from a directory, or of the
 * directory's own file for a null path, and utimensat itself. A null pair is
 * the current time.
 */

/* utimensat for the directory `dfd`, the user's path `upath`, the pair `ts`
 * or none, and the flags. */
static long utimes_call(isr_params_t *params, int dfd, uintptr_t upath, long ts[4], int flags)
{
    return ia32_native(params, sys_utimensat, ts, 4 * sizeof(long), (unsigned long)dfd, upath,
                       ts ? KPTR(ts) : 0, (unsigned long)flags, 0, 0);
}

/* The AT_FDCWD of the *at calls. */
#define IA32_AT_FDCWD (-100)

/** utime (30): the path and an i386 utimbuf of whole seconds, or 0. */
long compat_sys_utime32(isr_params_t *params)
{
    struct ia32_utimbuf b;
    long ts[4];
    uintptr_t ub = UPTR(SC_ARG(params, 1));

    if (!ub) {
        return utimes_call(params, IA32_AT_FDCWD, UPTR(SC_ARG(params, 0)), NULL, 0);
    }
    if (copy_from_user(&b, (const void __user *)ub, sizeof(b)) != 0) {
        return -EFAULT;
    }
    ts[0] = b.actime;
    ts[1] = 0;
    ts[2] = b.modtime;
    ts[3] = 0;
    return utimes_call(params, IA32_AT_FDCWD, UPTR(SC_ARG(params, 0)), ts, 0);
}

/* futimesat and utimes: the directory, the path and two i386 timevals at
 * `utv`, or 0; microseconds outside 0 to 999999 are -EINVAL, as Linux's
 * do_futimesat has them. */
static long futimes_call(isr_params_t *params, int dfd, uintptr_t upath, uintptr_t utv)
{
    long ts[4];

    if (!utv) {
        return utimes_call(params, dfd, upath, NULL, 0);
    }
    if (get_times32(utv, ts, 2) != 0) {
        return -EFAULT;
    }
    if (ts[1] < 0 || ts[1] >= 1000000 || ts[3] < 0 || ts[3] >= 1000000) {
        return -EINVAL;
    }
    ts[1] *= 1000;
    ts[3] *= 1000;
    return utimes_call(params, dfd, upath, ts, 0);
}

/** utimes (271): the path and two i386 timevals, or 0. */
long compat_sys_utimes_time32(isr_params_t *params)
{
    return futimes_call(params, IA32_AT_FDCWD, UPTR(SC_ARG(params, 0)), UPTR(SC_ARG(params, 1)));
}

/** futimesat (299): the directory, the path, or 0 for the directory's own
 * file, and two i386 timevals, or 0. */
long compat_sys_futimesat_time32(isr_params_t *params)
{
    return futimes_call(params, (int)SC_ARG(params, 0), UPTR(SC_ARG(params, 1)),
                        UPTR(SC_ARG(params, 2)));
}

/** utimensat (320): the directory, the path, two i386 timespecs, whose
 * UTIME_NOW and UTIME_OMIT keep their values, or 0, and the flags. */
long compat_sys_utimensat_time32(isr_params_t *params)
{
    long ts[4];
    uintptr_t uts = UPTR(SC_ARG(params, 2));

    if (uts && get_times32(uts, ts, 2) != 0) {
        return -EFAULT;
    }
    return utimes_call(params, (int)SC_ARG(params, 0), UPTR(SC_ARG(params, 1)), uts ? ts : NULL,
                       (int)SC_ARG(params, 3));
}

int ia32_never_restarts(syscall_fn_t fn)
{
    return fn == compat_sys_nanosleep_time32 || fn == compat_sys_clock_nanosleep_time32 ||
           fn == compat_sys_clock_nanosleep_time64 || fn == compat_sys_old_select ||
           fn == compat_sys_select || fn == compat_sys_pselect6_time32 ||
           fn == compat_sys_pselect6_time64 || fn == compat_sys_ppoll_time32 ||
           fn == compat_sys_ppoll_time64 || fn == compat_sys_epoll_pwait2;
}
