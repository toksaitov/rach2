/*
 * The system call layer's entry and its table: the dispatcher every system
 * call reaches, the table each architecture's `arch_syscall_init` fills, and
 * the calls that are the machine's own: time, identity, ioctl and the
 * terminal, the kernel log and the reboot. The process, file system and
 * socket calls are in syscall_proc.c, syscall_fs.c and syscall_net.c;
 * syscall_internal.h declares what the four share.
 */
#include "arch/arch.h"
#include "../kernel.h"
#include "mm/kheap.h"
#include "klog.h"
#include "syscall.h"
#include "power.h"
#include "serial.h"
#include "panic.h"
#include "kio.h"
#include "kbd.h"
#include "input.h"
#include "mouse.h"
#include "proc/process.h"
#include "proc/sched.h"
#include "proc/elf.h"
#include "proc/uimage.h"
#include "proc/futex.h"
#include "proc/tgroup.h"
#include "proc/ptrace.h"
#include "proc/seccomp.h"
#include "proc/ns.h"
#include "lib/krandom.h"
#include "lib/kerrno.h"
#include "lib/kirq.h"
#include "lib/ksignal.h"
#include "timer.h"
#include "fs/fd.h"
#include "fs/file.h"
#include "ipc/pipe.h"
#include "io/poll.h"
#include "usb/usb.h"
#include "tty/tty.h"
#include "tty/pty.h"
#include "rtc.h"
#include "block/loop.h"
#include "fs/krffs.h"
#include "fs/bcache.h"
#include "fs/devfs.h"
#include "fs/synthfs.h"
#include "fs/vfs.h"
#include "block/usbdisk.h"
#include "fs/procfs.h"
#include "mm/paging.h"
#include "mm/pressure.h"
#include "mm/pagecache.h"
#include "mm/heap.h"
#include "mm/usercopy.h"
#include "mm/vm.h"
#include "lib/kuser.h"
#include "lib/kstring.h"
#include "display/fb.h"
#include "speaker.h"
#include "audio/audio.h"
#include "network/socket/socket.h"
#include "network/socket/packet.h"
#include "network/socket/raw.h"
#include "network/ethernet/ethernet.h"
#include "network/netif.h"
#include "network/driver/netdev.h"
#include "network/udp/udp.h"
#include "network/tcp/tcp.h"
#include "network/ip/ip.h"
#include "network/ip/icmp.h"
#include "network/net.h"
#include "network/netconf.h" /* net_ip_addr, for the boot line's address */
#include "cmdline.h"         /* kcmdline_has, for the ENOSYS trace flag */
#include "io/eventfd.h"
#include "io/inotify.h"
#include "io/epoll.h"
#include "io/timerfd.h"
#include "ipc/sysv.h"
#include "ipc/mqueue.h"
#include "syscall_internal.h"

/* The thread group id of the one process that owns the framebuffer, 0 for
 * none. Text mode is restored when the owner exits, when it switches to text
 * mode and when it closes the last descriptor of its description; a close of
 * one descriptor of a shared description leaves the mode as it is, since the
 * description is still open. */
volatile uint32_t fb_owner_pid = 0;

/*
 * ioctl request codes (Linux compatible subset)
 */
#define KIOCSOUND 0x4B2F /* Set PC speaker frequency (0 = off) */

#define TCGETS 0x5401 /* Get terminal attributes (termios) */

#define TCSETS 0x5402 /* Set terminal attributes now */

#define TCSETSW 0x5403 /* Set after the output drains */

#define TCSETSF 0x5404 /* Set after output drains, flush input */

#define TCGETS2 0x802C542A /* Get the termios2 attributes: termios and speeds */

#define TCSETS2 0x402C542B /* Set the termios2 attributes now */

#define TCSETSW2 0x402C542C /* Set them after the output drains */

#define TCSETSF2 0x402C542D /* Set them after output drains, flush input */

#define TCFLSH 0x540B /* Flush input and/or output queues */

#define FIONREAD 0x541B /* Bytes available to read */

#define FIONBIO 0x5421 /* Set or clear O_NONBLOCK */

#define TIOCGWINSZ 0x5413 /* Get terminal window size */

#define TIOCSWINSZ 0x5414 /* Set terminal window size */

#define TIOCSCTTY 0x540E /* Make this tty the controlling terminal */

#define TIOCGPGRP 0x540F /* Get the foreground process group */

#define TIOCSPGRP 0x5410 /* Set the foreground process group */

#define TIOCNOTTY 0x5422 /* Detach from the controlling terminal */

#define FB_MODE_13H 0x4601 /* Switch to VGA Mode 13h */

#define FB_MODE_TEXT 0x4602 /* Switch back to text mode */

#define FB_SETPALETTE 0x4603 /* Set 256-color palette (768 bytes) */

/*
 * Audio ioctl request codes
 */
#define AUDIO_GET_INFO 0x4101 /* Get audio driver info */

#define AUDIO_START 0x4102 /* Start audio playback */

#define AUDIO_STOP 0x4103 /* Stop audio playback */

#define AUDIO_WRITE 0x4104 /* Write audio samples */

#define AUDIO_SET_VOLUME 0x4105 /* Set volume (0-100) */

/*
 * Audio info structure returned by AUDIO_GET_INFO
 */
struct audio_info {
    uint32_t driver_type;    /* AUDIO_DRIVER_NONE, SPEAKER, SB16, HDA */
    uint32_t has_real_audio; /* 1 if real audio (not PC speaker) */
};

/*
 * Audio start parameters for AUDIO_START
 */
struct audio_start_params {
    uint32_t sample_rate; /* e.g., 11025, 22050, 44100 */
    uint32_t format;      /* 0 = U8, 1 = S16LE */
    uint32_t channels;    /* 1 = mono, 2 = stereo */
};

/*
 * The argument of AUDIO_WRITE: the caller's samples, in the format
 * AUDIO_START set, and their length in bytes.
 */
struct audio_write_params {
    const uint8_t *buffer;
    uint32_t size;
};

/*
 * The handlers by call number, which `arch_syscall_init` fills; a null entry
 * is a call this kernel does not have.
 */
static syscall_fn_t _syscall_table[SYSCALL_TABLE_SIZE];

/* The magics Linux's reboot(2) demands, which musl passes for the caller: a
 * first word, one of four accepted second words, and then the command. */
#define REBOOT_MAGIC1 0xfee1deadu

#define REBOOT_MAGIC2 672274793u /* 0x28121969 */

#define REBOOT_MAGIC2A 85072278u /* 0x05121996 */

#define REBOOT_MAGIC2B 369367448u /* 0x16041998 */

#define REBOOT_MAGIC2C 537993216u /* 0x20112000 */

/* The commands. Two of them are negative read as a signed int, which is why
 * everything here compares raw 32-bit values. */
#define REBOOT_CMD_RESTART 0x01234567u

#define REBOOT_CMD_HALT 0xcdef0123u

#define REBOOT_CMD_POWER_OFF 0x4321fedcu

#define REBOOT_CMD_CAD_ON 0x89abcdefu

#define REBOOT_CMD_CAD_OFF 0x00000000u

/**
 * reboot(2): stop the machine. Its arguments are magic1, magic2 and command.
 *
 * Does not return for the three commands that stop the machine. Returns
 * `-EPERM` for an unprivileged caller, `-EINVAL` for a wrong magic or an
 * unknown command, and 0 for `RB_DISABLE_CAD`.
 *
 * The privilege check comes before the magics, as on Linux.
 *
 * `RB_DISABLE_CAD` returning 0 is what makes the rest testable: every other
 * command that this call accepts stops the machine, so a test has no other way
 * to learn whether a magic was accepted. The command requests a state this
 * kernel is permanently in, since there is no keyboard reboot to disable, so 0
 * is a true answer. `RB_ENABLE_CAD` requests a feature that does not exist and
 * answers `-EINVAL`, because a 0 there would be the one reply a caller could
 * not tell from a working one.
 */
long sys_reboot(isr_params_t *params)
{
    uint32_t magic1 = (uint32_t)SC_ARG(params, 0);
    uint32_t magic2 = (uint32_t)SC_ARG(params, 1);
    uint32_t cmd = (uint32_t)SC_ARG(params, 2);

    if (!cred_privileged()) {
        return -EPERM;
    }
    if (magic1 != REBOOT_MAGIC1) {
        return -EINVAL;
    }
    if (magic2 != REBOOT_MAGIC2 && magic2 != REBOOT_MAGIC2A && magic2 != REBOOT_MAGIC2B &&
        magic2 != REBOOT_MAGIC2C) {
        return -EINVAL;
    }

    switch (cmd) {
    case REBOOT_CMD_RESTART:
        kernel_shutdown(POWER_RESTART);
        break;
    case REBOOT_CMD_HALT:
        kernel_shutdown(POWER_HALT);
        break;
    case REBOOT_CMD_POWER_OFF:
        kernel_shutdown(POWER_OFF);
        break;
    case REBOOT_CMD_CAD_OFF:
        return 0;
    default:
        break;
    }

    return -EINVAL;
}

/**
 * Resolve a seek on a raw disk window, in unsigned arithmetic throughout.
 *
 * The obvious form adds the delta to the base as a signed 64-bit value and
 * then checks the result for negativity, which is undefined the moment the sum
 * leaves the signed range: a descriptor parked at the top of that range and
 * seeked forward by one has no defined result to test. So the base is unsigned,
 * the delta's magnitude is taken without ever negating it (negating the most
 * negative value is itself undefined), and both directions are bounds-checked
 * before the arithmetic, not after.
 *
 * @param cur    the window's current offset.
 * @param size   the window's byte size, for SEEK_END.
 * @param delta  the requested displacement, which may be negative.
 * @param whence 0 SEEK_SET, 1 SEEK_CUR, 2 SEEK_END.
 * @param out    the resolved offset, untouched on failure.
 * @return 0, `-EINVAL` for a bad whence or a result before the start, or
 *         `-EOVERFLOW` for one past what an offset can hold.
 */
int32_t raw_seek_resolve(uint64_t cur, uint64_t size, long long delta, int whence, uint64_t *out)
{
    uint64_t base;
    uint64_t mag;

    switch (whence) {
    case 0:
        base = 0;
        break;
    case 1:
        base = cur;
        break;
    case 2:
        base = size;
        break;
    default:
        return -EINVAL;
    }

    if (delta >= 0) {
        if ((uint64_t)delta > (uint64_t)0xFFFFFFFFFFFFFFFFull - base) {
            return -EOVERFLOW;
        }
        *out = base + (uint64_t)delta;
        return 0;
    }

    mag = (uint64_t)(-(delta + 1)) + 1u; /* -delta is undefined at the minimum */
    if (mag > base) {
        return -EINVAL;
    }
    *out = base - mag;
    return 0;
}

/*
 * The timeval of gettimeofday and settimeofday.
 */
struct kernel_timeval {
    long tv_sec;  /* seconds */
    long tv_usec; /* microseconds */
};

/**
 * gettimeofday(2) takes tv (timeval*) and tz (ignored). Reports real time from
 * the clock anchored at boot (clock.c), or time since boot when there was no
 * clock to read, in seconds as wide as a long; a null tv reads nothing, as on
 * Linux. Returns 0 or -errno.
 */
long sys_gettimeofday(isr_params_t *params)
{
    struct kernel_timeval __user *tv = (struct kernel_timeval __user *)SC_ARG(params, 0);
    struct kernel_timeval ktv;
    int64_t seconds;
    int64_t nanoseconds;

    if (!tv) {
        return 0;
    }
    get_current_time(&seconds, &nanoseconds);
    ktv.tv_sec = (long)seconds;
    ktv.tv_usec = (int32_t)nanoseconds / 1000; /* narrow first: no __divdi3 */
    if (copy_to_user(tv, &ktv, sizeof(ktv)) != 0) {
        return -EFAULT;
    }
    return 0;
}

/**
 * time(2): real time's seconds, as gettimeofday reads them, also stored at
 * tloc when it is not null. glibc on x86-64 calls it for time(), where a
 * vDSO would otherwise answer.
 */
long sys_time(isr_params_t *params)
{
    long __user *tloc = (long __user *)SC_ARG(params, 0);
    int64_t seconds;
    int64_t nanoseconds;
    long t;

    get_current_time(&seconds, &nanoseconds);
    t = (long)seconds;
    if (tloc && copy_to_user(tloc, &t, sizeof(t)) != 0) {
        return -EFAULT;
    }
    return t;
}

/* The clock ids of clock_gettime(2), clock_getres(2) and clock_nanosleep(2),
 * and the flag that makes a sleep's time absolute. */
#define CLOCK_REALTIME           0
#define CLOCK_MONOTONIC          1
#define CLOCK_PROCESS_CPUTIME_ID 2
#define CLOCK_THREAD_CPUTIME_ID  3
#define CLOCK_MONOTONIC_RAW      4
#define CLOCK_REALTIME_COARSE    5
#define CLOCK_MONOTONIC_COARSE   6
#define CLOCK_BOOTTIME           7
#define TIMER_ABSTIME            1u

/* A CPU clock named by a process or thread id, which Linux encodes in a
 * negative clock id: the complement of the id shifted left three places, a
 * thread bit, and which time the clock reads in the low two bits. Value 1
 * reads user time alone; 0 and 2 read user and system time together, the
 * second being Linux's scheduler count, which this kernel counts in ticks as
 * it counts the rest. Value 3 is a clock of a descriptor, which this kernel
 * has none of. */
#define CPUCLOCK_PERTHREAD 4u
#define CPUCLOCK_WHICH     3u
#define CPUCLOCK_VIRT      1u

/* The kinds of clock the ids name: real time and time since boot read to the
 * microsecond, the two at the last tick, and a process's CPU time, which
 * counts ticks. */
enum kclock_kind { KCLOCK_REAL, KCLOCK_REAL_COARSE, KCLOCK_MONO, KCLOCK_MONO_COARSE, KCLOCK_CPU };

/* What a clock id names: its kind, and for a CPU clock the process whose time
 * it reads and whether that is user time alone. */
struct kclock {
    enum kclock_kind kind;
    process_t *proc;
    int user_only;
    int thread; /* a CPU clock of the thread `proc`, not of its process */
};

/**
 * Resolve clock id `clk` for the current process.
 *
 * Clocks 0 to 7 are the fixed ones; a negative id is a CPU clock by process or
 * thread id, as `clock_getcpuclockid` and `pthread_getcpuclockid` make one.
 * Id 0 in one names the caller, a process id names any process that has not
 * been reaped, and a thread id must name a thread of the caller's process.
 *
 * @return 0, or -EINVAL for an id this kernel does not have: 8 and above, a
 *         descriptor's clock, a process clock named by an id that is not a
 *         process's, and a CPU clock of no such process or of another
 *         process's thread.
 */
static int32_t clock_resolve(int32_t clk, struct kclock *out)
{
    uint32_t id;

    out->proc = current_proc;
    out->user_only = 0;
    out->thread = 0;
    switch (clk) {
    case CLOCK_REALTIME:
        out->kind = KCLOCK_REAL;
        return 0;
    case CLOCK_REALTIME_COARSE:
        out->kind = KCLOCK_REAL_COARSE;
        return 0;
    case CLOCK_MONOTONIC:
    case CLOCK_MONOTONIC_RAW:
    case CLOCK_BOOTTIME:
        out->kind = KCLOCK_MONO;
        return 0;
    case CLOCK_MONOTONIC_COARSE:
        out->kind = KCLOCK_MONO_COARSE;
        return 0;
    case CLOCK_PROCESS_CPUTIME_ID:
        out->kind = KCLOCK_CPU;
        return 0;
    case CLOCK_THREAD_CPUTIME_ID:
        out->kind = KCLOCK_CPU;
        out->thread = 1;
        return 0;
    default:
        break;
    }
    if (clk >= 0 || ((uint32_t)clk & CPUCLOCK_WHICH) == 3u) {
        return -EINVAL;
    }
    out->kind = KCLOCK_CPU;
    out->user_only = ((uint32_t)clk & CPUCLOCK_WHICH) == CPUCLOCK_VIRT;
    out->thread = ((uint32_t)clk & CPUCLOCK_PERTHREAD) != 0;
    id = ~((uint32_t)clk >> 3) & 0x1FFFFFFFu;
    if (id == 0) {
        return 0;
    }
    out->proc = get_process(id);
    if (!out->proc || (out->thread && out->proc->tg != current_proc->tg) ||
        (!out->thread && out->proc->pid != out->proc->tgid)) {
        return -EINVAL;
    }
    return 0;
}

/** A CPU clock's reading in ticks: the thread's own, or every thread's of
 * the process. The thread's is read with interrupts masked, since the tick
 * adds to it. */
static uint32_t clock_cpu_ticks(const struct kclock *c)
{
    uint32_t user;
    uint32_t sys;

    if (c->thread) {
        uint32_t flags = kirq_save();

        user = c->proc->utime;
        sys = c->proc->stime;
        kirq_restore(flags);
    } else {
        proc_cpu_time(c->proc, &user, &sys);
    }
    return c->user_only ? user : user + sys;
}

/* The timespec of the time64 calls: seconds and nanoseconds of 64 bits. */
struct kernel_timespec64 {
    int64_t tv_sec;
    int64_t tv_nsec;
};

/**
 * Read clock `c` into `out`.
 *
 * Real time is the clock read at boot (clock.c) with the timer's place within
 * the tick added; its coarse form stops at the last tick. Time since boot is
 * the uptime seconds and the ticks within the second, with the timer's place
 * added as well (`rtc_monotonic`), read with interrupts masked, so neither a
 * torn second nor the tick counter's wrap at about 497 days can make it step
 * back; its coarse form stops at the tick. No clock is slewed: the machine
 * reads the CMOS once and then keeps its own time, which only a setting of
 * the real time steps, and it is never suspended, which is why
 * CLOCK_MONOTONIC, CLOCK_MONOTONIC_RAW and CLOCK_BOOTTIME read the same. A
 * CPU clock is the ticks the timer charged. The seconds are 64 bits; the
 * 32-bit timespec of i386's clock_gettime cuts them, which ends in 2038 for
 * real time, as Linux's does.
 */
static void clock_read(const struct kclock *c, struct kernel_timespec64 *out)
{
    int64_t seconds;
    int64_t nanoseconds;
    uint32_t seconds32, ticks32, total32;
    uint32_t ticks;

    switch (c->kind) {
    case KCLOCK_REAL:
        get_current_time(&seconds, &nanoseconds);
        out->tv_sec = seconds;
        out->tv_nsec = nanoseconds;
        break;
    case KCLOCK_REAL_COARSE:
        rtc_coarse_realtime(&seconds, &nanoseconds);
        out->tv_sec = seconds;
        out->tv_nsec = nanoseconds;
        break;
    case KCLOCK_MONO:
        rtc_monotonic(&seconds, &nanoseconds);
        out->tv_sec = seconds;
        out->tv_nsec = nanoseconds;
        break;
    case KCLOCK_MONO_COARSE:
        tick_now(&seconds32, &ticks32, &total32);
        out->tv_sec = seconds32;
        out->tv_nsec = ticks32 * 10000000u;
        break;
    case KCLOCK_CPU:
    default:
        ticks = clock_cpu_ticks(c);
        out->tv_sec = ticks / 100u;
        out->tv_nsec = (ticks % 100u) * 10000000u;
        break;
    }
}

/** A clock's resolution in nanoseconds: a microsecond for real time and time
 * since boot, which the timer's place is read in, and the tick for the rest. */
static int32_t clock_res_ns(const struct kclock *c)
{
    return c->kind == KCLOCK_REAL || c->kind == KCLOCK_MONO ? 1000 : 10000000;
}

int32_t clock_cpu_target(int32_t clk, struct tgroup **tg, uint32_t *tid, int *user_only)
{
    struct kclock c;

    if (clock_resolve(clk, &c) != 0 || c.kind != KCLOCK_CPU) {
        return -EINVAL;
    }
    *tg = c.proc->tg;
    (*tg)->refs++;
    *tid = c.thread ? c.proc->pid : 0u;
    *user_only = c.user_only;
    return 0;
}

int32_t clock_now(int32_t clk, int64_t *sec, int32_t *nsec)
{
    struct kclock c;
    struct kernel_timespec64 ts;
    int32_t rc = clock_resolve(clk, &c);

    if (rc != 0) {
        return rc;
    }
    clock_read(&c, &ts);
    *sec = ts.tv_sec;
    *nsec = (int32_t)ts.tv_nsec;
    return 0;
}

/**
 * Read a POSIX clock (clock_gettime, 32-bit timespec).
 *
 * Its arguments are clockid and struct timespec*. The clocks are
 * `clock_resolve`'s and the readings `clock_read`'s. clock_gettime64
 * reads the same with 64-bit fields. An unknown clock is -EINVAL before the
 * timespec is written, as on Linux.
 */
long sys_clock_gettime(isr_params_t *params)
{
    struct kernel_timespec __user *ts = (struct kernel_timespec __user *)SC_ARG(params, 1);
    struct kernel_timespec kts;
    struct kernel_timespec64 now;
    struct kclock c;
    int32_t rc = clock_resolve((int32_t)SC_ARG(params, 0), &c);

    if (rc != 0) {
        return rc;
    }
    clock_read(&c, &now);
    kts.tv_sec = (long)now.tv_sec;
    kts.tv_nsec = (long)now.tv_nsec;
    if (copy_to_user(ts, &kts, sizeof(kts)) != 0) {
        return -EFAULT;
    }
    return 0;
}

/**
 * clock_gettime64: clock_gettime with the 64-bit timespec GNU libc and
 * musl try first, so a clock read is one call rather than a failed call and a
 * second, with all 64 bits of the seconds.
 */
long sys_clock_gettime64(isr_params_t *params)
{
    struct kernel_timespec64 t;
    int64_t sec;
    int32_t nsec;
    int32_t rc = clock_now((int32_t)SC_ARG(params, 0), &sec, &nsec);

    if (rc != 0) {
        return rc;
    }
    t.tv_sec = sec;
    t.tv_nsec = nsec;
    return copy_to_user((void __user *)SC_ARG(params, 1), &t, sizeof(t)) == 0 ? 0 : -EFAULT;
}

/** clock_getres_time64: clock_getres with the 64-bit timespec. */
long sys_clock_getres_time64(isr_params_t *params)
{
    struct kernel_timespec64 t;
    struct kclock c;
    int32_t rc = clock_resolve((int32_t)SC_ARG(params, 0), &c);

    if (rc != 0) {
        return rc;
    }
    t.tv_sec = 0;
    t.tv_nsec = clock_res_ns(&c);
    if (SC_ARG(params, 1) && copy_to_user((void __user *)SC_ARG(params, 1), &t, sizeof(t)) != 0) {
        return -EFAULT;
    }
    return 0;
}

/**
 * Set the real time to `seconds` and `nanoseconds`, the common end of
 * clock_settime, settimeofday and stime: a privileged caller's, after the
 * call has checked its own clock and time. -EPERM for any other caller and
 * -EINVAL for a time before 1970 or nanoseconds outside a second, as Linux
 * answers them.
 */
static int32_t realtime_set(int64_t seconds, int64_t nanoseconds)
{
    if (!proc_privileged(current_proc)) {
        return -EPERM;
    }
    if (seconds < 0 || nanoseconds < 0 || nanoseconds > 999999999) {
        return -EINVAL;
    }
    rtc_set_realtime(seconds, (int32_t)nanoseconds);
    return 0;
}

/**
 * clock_settime takes clockid and the 32-bit timespec. Only CLOCK_REALTIME may
 * be set, as on Linux; another clock is -EINVAL.
 */
long sys_clock_settime(isr_params_t *params)
{
    struct kernel_timespec ts;

    if ((int32_t)SC_ARG(params, 0) != CLOCK_REALTIME) {
        return -EINVAL;
    }
    if (copy_from_user(&ts, (const void __user *)SC_ARG(params, 1), sizeof(ts)) != 0) {
        return -EFAULT;
    }
    return realtime_set(ts.tv_sec, ts.tv_nsec);
}

/** clock_settime64: clock_settime with the 64-bit timespec. */
long sys_clock_settime64(isr_params_t *params)
{
    struct kernel_timespec64 ts;

    if ((int32_t)SC_ARG(params, 0) != CLOCK_REALTIME) {
        return -EINVAL;
    }
    if (copy_from_user(&ts, (const void __user *)SC_ARG(params, 1), sizeof(ts)) != 0) {
        return -EFAULT;
    }
    return realtime_set(ts.tv_sec, ts.tv_nsec);
}

/* adjtimex's modes and states, Linux's (uapi/linux/timex.h). */
#define ADJ_OFFSET            0x0001u
#define ADJ_FREQUENCY         0x0002u
#define ADJ_MAXERROR          0x0004u
#define ADJ_ESTERROR          0x0008u
#define ADJ_STATUS            0x0010u
#define ADJ_TIMECONST         0x0020u
#define ADJ_TAI               0x0080u
#define ADJ_SETOFFSET         0x0100u
#define ADJ_MICRO             0x1000u
#define ADJ_NANO              0x2000u
#define ADJ_TICK              0x4000u
#define ADJ_OFFSET_SINGLESHOT 0x8001u
#define ADJ_OFFSET_SS_READ    0xa001u
#define STA_UNSYNC            0x0040
#define STA_NANO              0x2000
#define STA_RONLY             0xff00 /* the PPS, error, nano, mode and clock bits */
#define TIME_OK               0
#define TIME_ERROR            5
/* The tick, 10 ms at 100 Hz, in microseconds, and Linux's tolerance, 500
 * ppm in its scaled units. */
#define NTP_TICK_US           10000
#define NTP_TOLERANCE         (500L << 16)
#define NTP_FREQ_MAX          (500LL << 16)

/* The clock's state as adjtimex sets and reports it: Linux's at boot, the
 * clock unsynchronized and its errors at their limit. */
static struct {
    int64_t freq, maxerror, esterror, constant;
    int32_t status, tai;
} ntp = {0, 16000000, 16000000, 2, STA_UNSYNC, 0};

/* A timex in the kernel's own terms, which the three calls convert their
 * layouts to and from. */
struct ktimex {
    uint32_t modes;
    int32_t status, shift, tai;
    int64_t offset, freq, maxerror, esterror, constant, precision, tolerance;
    int64_t sec, usec, tick;
};

/** Step the real time by `sec` seconds and `nsec` nanoseconds, each
 * earlier when negative, `nsec` less than a second either way. */
static void realtime_step(int64_t sec, int32_t nsec)
{
    int64_t now, now_ns;

    rtc_realtime(&now, &now_ns);
    now += sec;
    now_ns += nsec;
    if (now_ns < 0) {
        now_ns += 1000000000;
        now--;
    } else if (now_ns >= 1000000000) {
        now_ns -= 1000000000;
        now++;
    }
    if (now >= 0) {
        rtc_set_realtime(now, (int32_t)now_ns);
    }
}

/** An offset of `v` microseconds, or nanoseconds with `nano`, as at most the
 * half second Linux takes one to be (MAXPHASE), in nanoseconds. */
static int32_t offset_ns(int64_t v, int nano)
{
    int64_t limit = nano ? 500000000 : 500000;

    v = v > limit ? limit : v < -limit ? -limit : v;
    return (int32_t)(nano ? v : v * 1000);
}

/**
 * adjtimex's work on `tx`, read and then filled in: the modes, then the
 * clock's state. The modes are Linux's, a privileged caller's alone but for
 * reading what a single-shot adjustment has left. This clock has no slew:
 * an offset, at most the half second Linux takes, a single-shot adjustment
 * and ADJ_SETOFFSET step the real time at once, so none is left in
 * progress, and the frequency, the time constant and the errors are kept
 * and reported but change nothing; a tick other than 10000 microseconds is
 * -EINVAL, since the timer's period is fixed. Returns Linux's state,
 * TIME_ERROR while STA_UNSYNC is set and TIME_OK otherwise.
 */
static int32_t ktimex_adjust(struct ktimex *tx)
{
    uint32_t m = tx->modes;
    int64_t sec, nsec;

    if (m != 0 && m != ADJ_OFFSET_SS_READ && !proc_privileged(current_proc)) {
        return -EPERM;
    }
    if (m == ADJ_OFFSET_SINGLESHOT) {
        realtime_step(0, offset_ns(tx->offset, 0));
    } else if (m != ADJ_OFFSET_SS_READ) {
        if ((m & ADJ_TICK) && tx->tick != NTP_TICK_US) {
            return -EINVAL;
        }
        if ((m & ADJ_SETOFFSET) &&
            (tx->usec < 0 || tx->usec >= ((m & ADJ_NANO) ? 1000000000 : 1000000))) {
            return -EINVAL;
        }
        if (m & ADJ_STATUS) {
            ntp.status = (ntp.status & STA_RONLY) | (tx->status & ~STA_RONLY);
        }
        if (m & ADJ_NANO) {
            ntp.status |= STA_NANO;
        }
        if (m & ADJ_MICRO) {
            ntp.status &= ~STA_NANO;
        }
        if (m & ADJ_FREQUENCY) {
            ntp.freq = tx->freq > NTP_FREQ_MAX    ? NTP_FREQ_MAX
                       : tx->freq < -NTP_FREQ_MAX ? -NTP_FREQ_MAX
                                                  : tx->freq;
        }
        if (m & ADJ_MAXERROR) {
            ntp.maxerror = tx->maxerror;
        }
        if (m & ADJ_ESTERROR) {
            ntp.esterror = tx->esterror;
        }
        if (m & ADJ_TIMECONST) {
            ntp.constant = tx->constant;
        }
        if (m & ADJ_TAI) {
            ntp.tai = (int32_t)tx->constant;
        }
        if (m & ADJ_SETOFFSET) {
            realtime_step(tx->sec, (int32_t)(tx->usec * ((m & ADJ_NANO) ? 1 : 1000)));
        }
        if (m & ADJ_OFFSET) {
            realtime_step(0, offset_ns(tx->offset, (ntp.status & STA_NANO) != 0));
        }
    }
    rtc_realtime(&sec, &nsec);
    tx->offset = 0;
    tx->freq = ntp.freq;
    tx->maxerror = ntp.maxerror;
    tx->esterror = ntp.esterror;
    tx->status = ntp.status;
    tx->constant = ntp.constant;
    tx->precision = 1;
    tx->tolerance = NTP_TOLERANCE;
    tx->sec = sec;
    tx->usec = (ntp.status & STA_NANO) ? nsec : (int64_t)((int32_t)nsec / 1000);
    tx->tick = NTP_TICK_US;
    tx->shift = 0;
    tx->tai = ntp.tai;
    return (ntp.status & STA_UNSYNC) ? TIME_ERROR : TIME_OK;
}

/* The bytes of a timex, in the layout of the 128-byte struct timex of
 * 32-bit longs, or of the 208-byte struct __kernel_timex of the 64-bit calls
 * and of x86-64's longs, at `wide`: each
 * field's offset in each, in the order modes, offset, freq, maxerror,
 * esterror, status, constant, precision, tolerance, the time's seconds and
 * fraction, tick, shift, tai and a pad. */
static const uint8_t timex_off[2][15] = {
    {0, 4, 8, 12, 16, 20, 24, 28, 32, 36, 40, 44, 56, 80, 0},
    {0, 8, 16, 24, 32, 40, 48, 56, 64, 72, 80, 88, 112, 160, 0}};

/** Field `field` of the timex bytes `b`, in the layout `wide` chooses: 32
 * bits, sign-extended, for the int fields and for every field of the narrow
 * layout, and 64 bits for the rest. */
static int64_t timex_get(const uint8_t *b, int wide, int field)
{
    const uint8_t *p = b + timex_off[wide][field];
    uint32_t lo =
        (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
    uint32_t hi;

    if (!wide || field == 0 || field == 5 || field == 12 || field == 13) {
        return (int64_t)(int32_t)lo;
    }
    hi = (uint32_t)p[4] | ((uint32_t)p[5] << 8) | ((uint32_t)p[6] << 16) | ((uint32_t)p[7] << 24);
    return (int64_t)(((uint64_t)hi << 32) | lo);
}

/** Store `v` in field `field` of the timex bytes `b`, at the width
 * `timex_get` reads it with. */
static void timex_put(uint8_t *b, int wide, int field, int64_t v)
{
    uint8_t *p = b + timex_off[wide][field];
    int n = (!wide || field == 0 || field == 5 || field == 12 || field == 13) ? 4 : 8;
    int i;

    for (i = 0; i < n; i++) {
        p[i] = (uint8_t)((uint64_t)v >> (8 * i));
    }
}

/* The ticks between two copies of the time to the CMOS clock. */
#define NTP_RTC_TICKS (11u * 60u * 100u)

/**
 * Copy the time to the CMOS clock after a change an NTP daemon made while
 * the clock is synchronized, at most once in 11 minutes, as Linux's
 * 11-minute mode does, so that the next boot starts from the time the
 * daemon kept and not from the CMOS clock's own drift. Rounded to the
 * nearest second, since the CMOS clock keeps no fraction.
 */
static void ntp_rtc_sync(void)
{
    static uint32_t last; /* kernel_ticks + 1 at the last copy, 0 for none */
    int64_t sec, nsec;
    rtc_time_t t;

    if ((ntp.status & STA_UNSYNC) || (last != 0 && kernel_ticks + 1u - last < NTP_RTC_TICKS)) {
        return;
    }
    rtc_realtime(&sec, &nsec);
    if (nsec >= 500000000) {
        sec++;
    }
    if (sec < 946684800 || sec > 4102444799LL) {
        return; /* the CMOS clock's century: 2000 to 2099 */
    }
    rtc_time_of_seconds((uint32_t)sec, &t);
    if (rtc_write_clock(&t) == 0) {
        last = kernel_ticks + 1u;
    }
}

/* Whether a struct timex of longs, which adjtimex and clock_adjtime take,
 * has the 64-bit layout of clock_adjtime64's: so where a long has 64 bits,
 * as on x86-64. */
#define TIMEX_LONGS_WIDE (sizeof(long) == 8)

/**
 * The common body of adjtimex, clock_adjtime and clock_adjtime64: the
 * caller's timex at `uaddr`, in the 64-bit layout with `wide`, read,
 * adjusted and written back. Returns the clock's state, as `ktimex_adjust`
 * gives it, or -errno.
 */
static int32_t timex_call(uintptr_t uaddr, int wide)
{
    uint8_t b[208];
    uint32_t size = wide ? 208u : 128u;
    struct ktimex tx;
    int32_t rc;

    if (copy_from_user(b, (const void __user *)uaddr, size) != 0) {
        return -EFAULT;
    }
    tx.modes = (uint32_t)timex_get(b, wide, 0);
    tx.offset = timex_get(b, wide, 1);
    tx.freq = timex_get(b, wide, 2);
    tx.maxerror = timex_get(b, wide, 3);
    tx.esterror = timex_get(b, wide, 4);
    tx.status = (int32_t)timex_get(b, wide, 5);
    tx.constant = timex_get(b, wide, 6);
    tx.sec = timex_get(b, wide, 9);
    tx.usec = timex_get(b, wide, 10);
    tx.tick = timex_get(b, wide, 11);
    rc = ktimex_adjust(&tx);
    if (rc < 0) {
        return rc;
    }
    if (tx.modes != 0 && proc_privileged(current_proc)) {
        ntp_rtc_sync();
    }
    timex_put(b, wide, 1, tx.offset);
    timex_put(b, wide, 2, tx.freq);
    timex_put(b, wide, 3, tx.maxerror);
    timex_put(b, wide, 4, tx.esterror);
    timex_put(b, wide, 5, tx.status);
    timex_put(b, wide, 6, tx.constant);
    timex_put(b, wide, 7, tx.precision);
    timex_put(b, wide, 8, tx.tolerance);
    timex_put(b, wide, 9, tx.sec);
    timex_put(b, wide, 10, tx.usec);
    timex_put(b, wide, 11, tx.tick);
    timex_put(b, wide, 12, tx.shift);
    timex_put(b, wide, 13, tx.tai);
    if (copy_to_user((void __user *)uaddr, b, size) != 0) {
        return -EFAULT;
    }
    return rc;
}

/** adjtimex takes the struct timex of longs, 128 bytes on i686 and 208 on
 * x86-64. */
long sys_adjtimex(isr_params_t *params)
{
    return timex_call(SC_ARG(params, 0), TIMEX_LONGS_WIDE);
}

/**
 * clock_adjtime and clock_adjtime64 take the clock and the timex, in the 64-bit
 * layout for the second. CLOCK_REALTIME is the one clock that adjusts; another
 * of the clocks is -EOPNOTSUPP and anything else -EINVAL, as Linux answers.
 */
static int32_t clock_adjtime_common(isr_params_t *params, int wide)
{
    int32_t id = (int32_t)SC_ARG(params, 0);

    if (id != 0) {
        return id > 0 && id <= 11 ? -EOPNOTSUPP : -EINVAL;
    }
    return timex_call(SC_ARG(params, 1), wide);
}

/** clock_adjtime: `clock_adjtime_common` with the struct timex of longs. */
long sys_clock_adjtime(isr_params_t *params)
{
    return clock_adjtime_common(params, TIMEX_LONGS_WIDE);
}

/** clock_adjtime64: `clock_adjtime_common` with the 64-bit layout. */
long sys_clock_adjtime64(isr_params_t *params)
{
    return clock_adjtime_common(params, 1);
}

/**
 * settimeofday takes timeval and timezone. The time zone is not kept, so a call
 * with only one is done and changes nothing; a call with neither is too, as on
 * Linux.
 */
long sys_settimeofday(isr_params_t *params)
{
    struct kernel_timeval tv;

    if (!SC_ARG(params, 0)) {
        return proc_privileged(current_proc) ? 0 : -EPERM;
    }
    if (copy_from_user(&tv, (const void __user *)SC_ARG(params, 0), sizeof(tv)) != 0) {
        return -EFAULT;
    }
    if (tv.tv_usec < 0 || tv.tv_usec > 999999) {
        return -EINVAL;
    }
    return realtime_set(tv.tv_sec, (int64_t)tv.tv_usec * 1000);
}

/** stime takes the seconds, a 32-bit time_t. */
long sys_stime(isr_params_t *params)
{
    int32_t seconds;

    if (copy_from_user(&seconds, (const void __user *)SC_ARG(params, 0), sizeof(seconds)) != 0) {
        return -EFAULT;
    }
    return realtime_set(seconds, 0);
}

/**
 * A clock's resolution (clock_getres, 32-bit timespec).
 *
 * Its arguments are clockid and struct timespec*, which may be NULL. Each
 * clock's true granularity: a microsecond for real time and time since boot,
 * which the timer's place is read in, and the 10 ms tick for every other
 * clock, the coarse clocks and the CPU clocks. Returns 0, -EINVAL for a clock
 * `clock_resolve` does not have, or -EFAULT.
 */
long sys_clock_getres(isr_params_t *params)
{
    struct kernel_timespec __user *ures = (struct kernel_timespec __user *)SC_ARG(params, 1);
    struct kernel_timespec res;
    struct kclock c;
    int32_t rc = clock_resolve((int32_t)SC_ARG(params, 0), &c);

    if (rc != 0) {
        return rc;
    }
    res.tv_sec = 0;
    res.tv_nsec = clock_res_ns(&c);
    if (ures && copy_to_user(ures, &res, sizeof(res)) != 0) {
        return -EFAULT;
    }
    return 0;
}

/**
 * End a sleep a signal cut short before its deadline, the tick `target`: the
 * process runs again, the kernel counts the sleep, and `urem`, when there is
 * one, gets the time left to that tick, less how far into the tick in
 * progress the timer is (`rtc_tick_phase`).
 */
static int32_t nanosleep_interrupted(struct kernel_timespec __user *urem, uint32_t target)
{
    current_proc->wakeup_tick = 0;
    current_proc->state = PROC_RUNNING;
    process_count_sleep_cut_short();
    if (urem) {
        struct kernel_timespec rem;
        uint32_t now, phase_ns, left;
        int32_t ns;

        rtc_tick_phase(&now, &phase_ns);
        left = (int32_t)(target - now) > 0 ? target - now : 0u;
        rem.tv_sec = (int32_t)(left / 100u);
        ns = (int32_t)((left % 100u) * 10000000u) - (int32_t)phase_ns;
        if (ns < 0 && rem.tv_sec > 0) {
            rem.tv_sec--;
            ns += 1000000000;
        }
        rem.tv_nsec = ns > 0 ? ns : 0;
        (void)copy_to_user(urem, &rem, sizeof(rem));
    }
    return -EINTR;
}

/**
 * Block until the tick `target`, or until a signal the process acts on.
 *
 * Marks the process PROC_BLOCKED on its wakeup_tick and yields; the timer
 * wakes it at the deadline while other processes run. The deadline and the
 * pending signals are tested, and the process blocked, with interrupts off,
 * so neither a signal nor the deadline's tick can come between the test and
 * the block unseen; the halt after a yield that ran nothing is masked for the
 * same reason.
 *
 * Once the deadline tick has come the sleep is over, and the call returns 0
 * whether or not a signal is pending. A signal the process acts on, found
 * pending before then, ends the sleep through `nanosleep_interrupted`, which
 * writes `urem` when it is not NULL and returns -EINTR; a wake by one it does
 * not act on sends it back to sleep with the deadline stored again.
 */
static int32_t sleep_until(uint32_t target, struct kernel_timespec __user *urem)
{
    for (;;) {
        uint32_t flags = kirq_save();
        int32_t left = (int32_t)(target - kernel_ticks);

        if (left <= 0) {
            kirq_restore(flags);
            break;
        }
        if (signal_pending()) {
            kirq_restore(flags);
            return nanosleep_interrupted(urem, target);
        }
        /* 0 means not sleeping, so a deadline of 0 is stored as 1. */
        current_proc->wakeup_tick = target != 0 ? target : 1u;
        current_proc->state = PROC_BLOCKED;
        kirq_restore(flags);
        schedule(); /* yield to any other READY process; the timer wakes us */
        flags = kirq_save();
        if (current_proc->state == PROC_BLOCKED) {
            idle_wait_irq_off();
        }
        kirq_restore(flags);
    }
    current_proc->wakeup_tick = 0;
    current_proc->state = PROC_RUNNING;
    return 0;
}

/**
 * A nonnegative interval as whole 10 ms ticks, rounded up, so any nonzero
 * interval is a tick at least.
 *
 * Converted without 64-bit division, which the freestanding kernel has no
 * libgcc `__udivdi3` for: 1 s is 100 ticks, plus the ceiling of the
 * sub-second nanoseconds. Absurd intervals are clamped, at the seconds'
 * full width before they are narrowed, so sec*100 cannot overflow and the
 * signed tick comparison in `sleep_until` stays valid with the tick in
 * progress added.
 */
static uint32_t timespec_ticks(const struct kernel_timespec *req)
{
    uint32_t sec = req->tv_sec > 21474836L ? 21474836u : (uint32_t)req->tv_sec;
    uint32_t nsec_ticks = ((uint32_t)req->tv_nsec + 9999999u) / 10000000u;
    uint32_t ticks;

    ticks = sec * 100u + nsec_ticks;
    if (ticks > 0x7FFFFFFEu) {
        ticks = 0x7FFFFFFEu; /* keep the signed (target-now) delta valid */
    }
    return ticks;
}

/** Whether `req` is a time a sleep takes: seconds not negative, and
 * nanoseconds from 0 through 999999999. */
static int timespec_valid(const struct kernel_timespec *req)
{
    return req->tv_sec >= 0 && req->tv_nsec >= 0 && req->tv_nsec < 1000000000;
}

/**
 * Sleep for `req`, to the first tick at or after its end, so a sleep is never
 * shorter than asked and at most a tick longer, up to the clamp in
 * `timespec_ticks`. The end is measured from where in the tick in progress
 * the call is (`rtc_tick_phase`): counting that tick as a whole would make
 * every sleep of a tick or less last two, so that `usleep(1000)` would take
 * 20 ms.
 *
 * A signal's remainder, written to `urem`, is the time left to the deadline
 * tick, so a caller that sleeps again for the remainder ends at the first
 * sleep's deadline, and later by the time it takes to call again.
 */
static int32_t sleep_for(const struct kernel_timespec *req, struct kernel_timespec __user *urem)
{
    uint32_t now, phase_ns, us, ticks;

    rtc_tick_phase(&now, &phase_ns);
    if (req->tv_sec == 0 && req->tv_nsec == 0) {
        return 0;
    }
    us = phase_ns / 1000u + ((uint32_t)req->tv_nsec + 999u) / 1000u;
    ticks = timespec_ticks(req) - ((uint32_t)req->tv_nsec + 9999999u) / 10000000u;
    ticks += (us + 9999u) / 10000u;
    return sleep_until(now + ticks, urem);
}

/**
 * Block-and-schedule sleep (nanosleep, 32-bit timespec).
 *
 * Its arguments are const struct timespec* req and struct timespec* rem
 * (optional). Sleeps to the first tick at or after the interval's end through
 * `sleep_for`, which has the rules of the deadline, the signals and the
 * remainder. rem is written as zero on normal completion.
 */
long sys_nanosleep(isr_params_t *params)
{
    const struct kernel_timespec __user *ureq =
        (const struct kernel_timespec __user *)SC_ARG(params, 0);
    struct kernel_timespec __user *urem = (struct kernel_timespec __user *)SC_ARG(params, 1);
    struct kernel_timespec req;
    int32_t rc;

    if (!current_proc || !ureq) {
        return -EFAULT;
    }
    if (copy_from_user(&req, ureq, sizeof(req)) != 0) {
        return -EFAULT;
    }
    if (!timespec_valid(&req)) {
        return -EINVAL;
    }
    rc = sleep_for(&req, urem);
    if (rc != 0) {
        return rc;
    }

    if (urem) {
        struct kernel_timespec zero;
        zero.tv_sec = 0;
        zero.tv_nsec = 0;
        if (copy_to_user(urem, &zero, sizeof(zero)) != 0) {
            return -EFAULT;
        }
    }
    return 0;
}

/** Whether real time or time since boot, `kind`, read to the microsecond as
 * `clock_gettime` reads it, has reached `when`. */
static int time_reached(enum kclock_kind kind, const struct kernel_timespec *when)
{
    int64_t sec;
    int64_t nsec;

    if (kind == KCLOCK_REAL) {
        get_current_time(&sec, &nsec);
    } else {
        rtc_monotonic(&sec, &nsec);
    }
    return sec > when->tv_sec || (sec == when->tv_sec && nsec >= when->tv_nsec);
}

/* The latest deadline an absolute sleep counts to, 2^40 seconds, some
 * 34000 years, which sleeps as long as any later one, and whose ticks a
 * 64-bit product holds. */
#define SLEEP_SECONDS_MOST ((int64_t)1 << 40)

/**
 * Sleep until real time or time since boot, `kind`, reads `when` or later.
 *
 * Both clocks advance with the tick: real time is the boot's epoch, from
 * `rtc_boot_epoch`, plus the time since boot, and time since boot at a tick
 * is that tick's count in hundredths of a second. So the sleep ends at the
 * first tick whose count reaches `when`, and a program that reads the clock
 * then reads `when` or later. Both are read to the microsecond, within the
 * tick, so a time the clock has reached returns at once, before the deadline
 * is rounded up to a tick. The tick count and the time since boot are read
 * as one reading (`tick_now`), since the tick changes both. A deadline
 * further than `sleep_until` can wait for, about 248 days, is waited for in
 * pieces, and the clock read again after each. Returns 0, or -EINTR when a
 * signal the process acts on comes first.
 */
static int32_t sleep_until_time(enum kclock_kind kind, const struct kernel_timespec *when)
{
    for (;;) {
        uint32_t seconds32, ticks32;
        uint64_t now;
        uint32_t base;
        int64_t sec = when->tv_sec;
        uint64_t due;
        int32_t rc;

        if (time_reached(kind, when)) {
            return 0;
        }
        tick_now(&seconds32, &ticks32, &base);
        now = (uint64_t)seconds32 * 100u + ticks32;
        if (kind == KCLOCK_REAL) {
            sec -= rtc_boot_epoch();
        }
        if (sec < 0) {
            return 0;
        }
        if (sec > SLEEP_SECONDS_MOST) {
            sec = SLEEP_SECONDS_MOST;
        }
        due = (uint64_t)sec * 100u + ((uint32_t)when->tv_nsec + 9999999u) / 10000000u;
        if (due <= now) {
            return 0;
        }
        if (due - now <= 0x7FFFFFFEu) {
            return sleep_until(base + (uint32_t)(due - now), NULL);
        }
        rc = sleep_until(base + 0x7FFFFFFEu, NULL);
        if (rc != 0) {
            return rc;
        }
    }
}

/**
 * Sleep until the CPU clock `clk`, a process's, reads `due` ticks or more.
 *
 * The clock is looked up again at every turn, since the process whose time it
 * reads may be reaped meanwhile. It is read again at every tick while it can
 * advance: another process's, and the caller's own while another thread of
 * the caller's process lives, whose time the clock counts. The caller's
 * clock with no other thread does not advance while it sleeps, and neither
 * does a reaped process's, so the wait for either ends only at a signal. A
 * relative sleep's remainder is the ticks still due at the last reading.
 * Returns 0, or -EINTR.
 */
static int32_t sleep_until_cpu(int32_t clk, uint32_t due, struct kernel_timespec __user *urem)
{
    uint32_t now = 0;

    for (;;) {
        struct kclock c;
        uint32_t wait = 0x7FFFFFFEu;
        int32_t rc;

        if (clock_resolve(clk, &c) == 0) {
            now = clock_cpu_ticks(&c);
            if (now >= due) {
                return 0;
            }
            if (c.proc->tg != current_proc->tg || current_proc->tg->live > 1) {
                wait = 1;
            }
        }
        rc = sleep_until(kernel_ticks + wait, NULL);
        if (rc != 0) {
            if (urem) {
                struct kernel_timespec rem;

                rem.tv_sec = (int32_t)((due - now) / 100u);
                rem.tv_nsec = (int32_t)(((due - now) % 100u) * 10000000u);
                (void)copy_to_user(urem, &rem, sizeof(rem));
            }
            return rc;
        }
    }
}

/**
 * Sleep against a chosen clock (clock_nanosleep, 32-bit
 * timespec).
 *
 * Its arguments are clockid, flags, const struct timespec *req and struct
 * timespec *rem. Flags 0 makes `req` an interval, which sleeps as `nanosleep`
 * does except that nothing is written to `rem` on completion; TIMER_ABSTIME
 * makes it a time of the clock, reached as `sleep_until_time` or
 * `sleep_until_cpu` describe, and returns at once for a time already past.
 * Other flag bits are ignored, as Linux ignores them, and `rem` is written only
 * for an interval a signal cut short.
 *
 * Real time, time since boot and boot time sleep; the raw and coarse clocks
 * and the calling thread's CPU clock return -EOPNOTSUPP, the clocks Linux
 * cannot sleep on either, and a thread CPU clock by id returns -EINVAL. The
 * caller's process CPU clock is Linux's too: while the caller sleeps it
 * advances by the time of the process's other threads alone, so with no
 * other thread an interval or a time not yet reached is waited out only by a
 * signal. The order of the checks is Linux's: the clock, then the request
 * (-EFAULT, then -EINVAL for a negative second or nanoseconds past a
 * second), then a CPU clock by id.
 */
long sys_clock_nanosleep(isr_params_t *params)
{
    int32_t clk = (int32_t)SC_ARG(params, 0);
    int absolute = (SC_ARG(params, 1) & TIMER_ABSTIME) != 0;
    const struct kernel_timespec __user *ureq =
        (const struct kernel_timespec __user *)SC_ARG(params, 2);
    struct kernel_timespec __user *urem = (struct kernel_timespec __user *)SC_ARG(params, 3);
    struct kernel_timespec req;
    struct kclock c;
    int32_t rc;

    if (clk > CLOCK_BOOTTIME) {
        return -EINVAL;
    }
    if (clk == CLOCK_THREAD_CPUTIME_ID || clk == CLOCK_MONOTONIC_RAW ||
        clk == CLOCK_REALTIME_COARSE || clk == CLOCK_MONOTONIC_COARSE) {
        return -EOPNOTSUPP;
    }
    if (!current_proc || !ureq || copy_from_user(&req, ureq, sizeof(req)) != 0) {
        return -EFAULT;
    }
    if (!timespec_valid(&req)) {
        return -EINVAL;
    }
    if (absolute) {
        urem = NULL;
    }
    if (clk < 0 && ((uint32_t)clk & CPUCLOCK_PERTHREAD)) {
        return -EINVAL;
    }
    rc = clock_resolve(clk, &c);
    if (rc != 0) {
        return rc;
    }
    if (c.kind == KCLOCK_CPU) {
        uint32_t due = timespec_ticks(&req);

        if (!absolute) {
            due += clock_cpu_ticks(&c);
        }
        return sleep_until_cpu(clk, due, urem);
    }
    if (!absolute) {
        return sleep_for(&req, urem);
    }
    return sleep_until_time(c.kind, &req);
}

/* setitimer's timers: of real time, and of the process's processor time
 * in user mode and in both modes. */
#define ITIMER_REAL    0
#define ITIMER_VIRTUAL 1
#define ITIMER_PROF    2

/*
 * The itimerval of setitimer and getitimer: the interval, then the value,
 * each in seconds and microseconds of a long, 32 bits on i686. musl passes
 * this form for its 64-bit time_t.
 */
struct kernel_itimerval {
    long interval_sec;
    long interval_usec;
    long value_sec;
    long value_usec;
};

/**
 * A time in seconds and microseconds as whole 10 ms ticks, rounded up. The
 * seconds are clamped where nanosleep clamps them and the ticks at 0x7FFFFFFD,
 * so the signed difference between the timer's tick and the count stays
 * positive with the tick in progress added and with the tick `proc_alarm_at`
 * adds when the sum wraps to zero. Zero only for a time of zero.
 */
static uint32_t itimer_ticks(long sec, long usec)
{
    uint32_t whole = (unsigned long)sec > 21474836u ? 21474836u : (uint32_t)sec;
    uint32_t ticks;

    ticks = whole * 100u + ((uint32_t)usec + 9999u) / 10000u;
    return ticks > 0x7FFFFFFDu ? 0x7FFFFFFDu : ticks;
}

/** The ticks left before the current process's timer is due, 0 when it is
 * disarmed. Part of the tick in progress has passed, so the time left lies
 * between the whole ticks to the due tick less one and that number; the
 * lower end is what is reported, and a timer due within a tick reads as one
 * tick left, since zero means disarmed. So a value read in the timer's last
 * tick and passed straight back to setitimer comes due a tick later: the
 * read reports one tick where less than one is left, and the set adds a tick
 * for the tick in progress. Interrupts are off: the tick rearms the timer. */
static uint32_t itimer_left(void)
{
    const process_t *p = current_proc->tg->leader;
    uint32_t left;

    if (p->alarm_tick == 0) {
        return 0;
    }
    left = p->alarm_tick - kernel_ticks;
    return (int32_t)left > 1 ? left - 1u : 1u;
}

/** `ticks` as the seconds and microseconds of an itimerval. */
static void itimer_put(uint32_t ticks, long *sec, long *usec)
{
    *sec = (long)(ticks / 100u);
    *usec = (long)((ticks % 100u) * 10000u);
}

/** What the current process's timer `which` is set to, as setitimer and
 * getitimer report it. Interrupts are off. */
static void itimer_read(int32_t which, struct kernel_itimerval *out)
{
    const struct tgroup *tg = current_proc->tg;

    if (which == ITIMER_REAL) {
        itimer_put(itimer_left(), &out->value_sec, &out->value_usec);
        itimer_put(tg->leader->alarm_interval, &out->interval_sec, &out->interval_usec);
    } else {
        itimer_put(tg->cpu_left[which - 1], &out->value_sec, &out->value_usec);
        itimer_put(tg->cpu_interval[which - 1], &out->interval_sec, &out->interval_usec);
    }
}

/**
 * Arm or disarm the current process's timer `which`: the first signal
 * `value` ticks from now and then every `interval`, or disarmed for a value
 * of 0. A timer of real time does not count the tick in progress, so the
 * first SIGALRM never comes before the time asked for; the later ones are
 * rearmed at a tick and come every `interval`. A timer of processor time
 * counts the ticks the tick handler charges to the process's threads.
 * Interrupts are off.
 */
static void itimer_set(int32_t which, uint32_t value, uint32_t interval)
{
    struct tgroup *tg = current_proc->tg;
    process_t *p = tg->leader;

    if (which != ITIMER_REAL) {
        tg->cpu_left[which - 1] = value;
        tg->cpu_interval[which - 1] = value != 0 ? interval : 0;
        return;
    }
    if (value == 0) {
        p->alarm_tick = 0;
        p->alarm_interval = 0;
        return;
    }
    p->alarm_tick = proc_alarm_at(kernel_ticks, value + 1u);
    p->alarm_interval = interval;
}

/**
 * setitimer: the interval timers.
 *
 * Its arguments are which, const struct itimerval *new and struct itimerval
 * *old, which may be null. ITIMER_REAL counts real time and posts SIGALRM,
 * ITIMER_VIRTUAL the process's processor time in user mode and posts SIGVTALRM,
 * and ITIMER_PROF its time in both modes and posts SIGPROF, which glibc's
 * profil and gprof's sampling use. Another `which` is EINVAL, as is a negative
 * time or microseconds past a second. A value of zero disarms the timer;
 * otherwise the first signal is due that long from now, rounded up to whole 10
 * ms ticks, then every interval, or only once for an interval of zero. `old`
 * receives the timer's setting before the call.
 */
long sys_setitimer(isr_params_t *params)
{
    const struct kernel_itimerval __user *unew =
        (const struct kernel_itimerval __user *)SC_ARG(params, 1);
    struct kernel_itimerval __user *uold = (struct kernel_itimerval __user *)SC_ARG(params, 2);
    struct kernel_itimerval set, was;
    int32_t which = (int32_t)SC_ARG(params, 0);
    uint32_t flags;

    if (which < ITIMER_REAL || which > ITIMER_PROF) {
        return -EINVAL;
    }
    if (!unew || copy_from_user(&set, unew, sizeof(set)) != 0) {
        return -EFAULT;
    }
    if (set.value_sec < 0 || set.value_usec < 0 || set.value_usec >= 1000000 ||
        set.interval_sec < 0 || set.interval_usec < 0 || set.interval_usec >= 1000000) {
        return -EINVAL;
    }
    flags = kirq_save();
    itimer_read(which, &was);
    itimer_set(which, itimer_ticks(set.value_sec, set.value_usec),
               itimer_ticks(set.interval_sec, set.interval_usec));
    kirq_restore(flags);
    if (uold && copy_to_user(uold, &was, sizeof(was)) != 0) {
        return -EFAULT;
    }
    return 0;
}

/**
 * getitimer: what an interval timer is set to.
 *
 * Its arguments are which and struct itimerval *cur: the time left before the
 * next signal, zero when the timer is disarmed, and the interval.
 */
long sys_getitimer(isr_params_t *params)
{
    struct kernel_itimerval __user *ucur = (struct kernel_itimerval __user *)SC_ARG(params, 1);
    struct kernel_itimerval cur;
    int32_t which = (int32_t)SC_ARG(params, 0);
    uint32_t flags;

    if (which < ITIMER_REAL || which > ITIMER_PROF) {
        return -EINVAL;
    }
    flags = kirq_save();
    itimer_read(which, &cur);
    kirq_restore(flags);
    if (!ucur || copy_to_user(ucur, &cur, sizeof(cur)) != 0) {
        return -EFAULT;
    }
    return 0;
}

/**
 * alarm: one SIGALRM `seconds` from now, or none for 0.
 *
 * Its argument is seconds. The same timer as setitimer's, with no interval.
 * Returns the seconds that were left before the earlier alarm, rounded as Linux
 * rounds them: to the nearest second, and up to one when any time was left.
 * musl's `alarm` calls setitimer; this is the call older programs make.
 */
long sys_alarm(isr_params_t *params)
{
    uint32_t flags = kirq_save();
    uint32_t left = itimer_left();
    uint32_t seconds = left / 100u;
    uint32_t usec = (left % 100u) * 10000u;

    itimer_set(
        ITIMER_REAL,
        itimer_ticks((int32_t)(SC_ARG(params, 0) > 21474836u ? 21474836u : SC_ARG(params, 0)), 0),
        0);
    kirq_restore(flags);
    if ((seconds == 0 && usec != 0) || usec >= 500000u) {
        seconds++;
    }
    return (int32_t)seconds;
}

/* getrandom flag bits (Linux). Nothing here blocks and there is one source,
 * so `GRND_NONBLOCK` and `GRND_RANDOM` only have their values checked;
 * `GRND_INSECURE` decides whether an untrusted generator answers with bytes
 * or with `EIO`. */
#define GRND_NONBLOCK 0x0001u

#define GRND_RANDOM 0x0002u

#define GRND_INSECURE 0x0004u

/**
 * Fill a buffer with random bytes (getrandom).
 *
 * Its arguments are buf, buflen and flags. The bytes come from the kernel
 * CSPRNG (lib/krandom, ChaCha20); musl's getrandom and getentropy call this.
 * Returns the byte count, like Linux, or the count copied before a fault, or
 * -errno: -EINVAL for an unknown flag bit or a length a positive int32 cannot
 * hold, -EFAULT, and -EIO with no trusted hardware entropy (RDSEED or RDRAND)
 * unless the flags have GRND_INSECURE. The call never blocks, so
 * GRND_NONBLOCK changes nothing, and there is a single source, so GRND_RANDOM
 * changes nothing either.
 *
 * Without trusted entropy the call fails closed rather than hand out the
 * weak boot-jitter stream for a key; a caller that accepts such bytes passes
 * GRND_INSECURE, the tier of AT_RANDOM and the stack canary. A large call
 * gives way every 4 KiB, between two pieces, so a reader the tick woke does
 * not wait for the whole call.
 */
long sys_getrandom(isr_params_t *params)
{
    void __user *buf = (void __user *)SC_ARG(params, 0);
    uint32_t len = USER_LEN32(SC_ARG(params, 1));
    uint32_t flags = (uint32_t)SC_ARG(params, 2);
    uint8_t kbuf[256];
    uint32_t done = 0;

    if (flags & ~(GRND_NONBLOCK | GRND_RANDOM | GRND_INSECURE)) {
        return -EINVAL; /* unknown flag bits, as Linux rejects */
    }
    if (len > 0x7FFFFFFFu) {
        return -EINVAL; /* keep the byte count representable as a positive int32 */
    }
    if (len == 0) {
        return 0;
    }
    if (!buf) {
        return -EFAULT;
    }
    if (!krandom_is_trusted() && !(flags & GRND_INSECURE)) {
        return -EIO;
    }
    while (done < len) {
        uint32_t chunk = len - done;
        if (chunk > sizeof(kbuf)) {
            chunk = sizeof(kbuf);
        }
        krandom_bytes(kbuf, chunk);
        if (copy_to_user((uint8_t __user *)buf + done, kbuf, chunk) != 0) {
            return (done > 0) ? (int32_t)done : -EFAULT;
        }
        done += chunk;
        usb_stamp_work(chunk);
        if ((done & 4095u) == 0) {
            sched_service_point();
        }
    }
    return (int32_t)done;
}

/** True if some process is in process group `pgid` within session `sid`. This
 * is the TIOCSPGRP rule that a foreground group must belong to the controlling
 * session.
 *
 * A zombie counts. A process leaves its group when it is reaped and not when it
 * exits, and a shell depends on that: putting a pipeline in the foreground
 * means setting the terminal's group to the first member's pid, and that member
 * can be a short command that has already exited by the time the shell gets
 * there. A test that excluded zombies would fail the TIOCSPGRP in that race,
 * and the shell would report that it could not set the terminal's process
 * group. */
int pgrp_in_session(int pgid, int sid)
{
    const process_t *p;

    for (p = proc_list; p; p = p->next) {
        if ((int)p->pgid == pgid && (int)p->sid == sid) {
            return 1;
        }
    }
    return 0;
}

/* The termios2 of TCGETS2 and the TCSETS2 family: the kernel termios, then
 * the input and output speeds. GNU libc uses these for tcgetattr, tcsetattr
 * and isatty. */
struct ktermios2 {
    struct ktermios tio;
    uint32_t c_ispeed;
    uint32_t c_ospeed;
};

typedef char ktermios2_is_44_bytes[sizeof(struct ktermios2) == 44 ? 1 : -1];

/**
 * The line speed that the baud code `code` names, as TCGETS2 reports it:
 * the sixteen codes from B0 to B38400 in the low four bits, and with CBAUDEX
 * (010000) the fifteen up to B4000000. CBAUDEX alone is BOTHER, a speed
 * given by number, which reads as 0, since no terminal here has a line speed
 * to keep one in.
 */
static uint32_t tty_code_speed(uint32_t code)
{
    static const uint32_t speeds[32] = {
        0,       50,      75,      110,     134,     150,     200,     300,
        600,     1200,    1800,    2400,    4800,    9600,    19200,   38400,
        0,       57600,   115200,  230400,  460800,  500000,  576000,  921600,
        1000000, 1152000, 1500000, 2000000, 2500000, 3000000, 3500000, 4000000};

    return speeds[(code & 017u) + ((code & 010000u) ? 16u : 0u)];
}

/**
 * TCGETS and TCGETS2: copy the terminal's termios to `arg`, and for TCGETS2
 * the speeds after it that its baud codes name, the input speed that of the
 * input code or, with none, the output speed.
 */
static int32_t tty_get_termios_at(const struct tty *t, uint32_t request, uintptr_t arg)
{
    struct ktermios2 kt2;
    uint32_t size = request == TCGETS2 ? sizeof(struct ktermios2) : sizeof(struct ktermios);
    uint32_t in;

    if (!arg) {
        return -EFAULT;
    }
    kt2.tio = t->ldisc.tio;
    in = (kt2.tio.c_cflag >> 16) & 010017u;
    kt2.c_ospeed = tty_code_speed(kt2.tio.c_cflag);
    kt2.c_ispeed = in ? tty_code_speed(in) : kt2.c_ospeed;
    if (copy_to_user((void __user *)arg, &kt2, size) != 0) {
        return -EFAULT;
    }
    return 0;
}

/**
 * The TCSETS and TCSETS2 families: install the termios at `arg`, which for
 * the second is the start of a termios2 whose speeds are not kept. The
 * install runs with interrupts off, so an input interrupt never reads a
 * half-written termios; TCSETSF and TCSETSF2 also discard pending input.
 */
static int32_t tty_set_termios_at(struct tty *t, uint32_t request, uintptr_t arg)
{
    struct ktermios2 kt2;
    int two = request == TCSETS2 || request == TCSETSW2 || request == TCSETSF2;
    uint32_t size = two ? sizeof(struct ktermios2) : sizeof(struct ktermios);

    if (!arg) {
        return -EFAULT;
    }
    if (copy_from_user(&kt2, (const void __user *)arg, size) != 0) {
        return -EFAULT;
    }
    tty_set_termios(t, &kt2.tio, request == TCSETSF || request == TCSETSF2);
    return 0;
}

/**
 * Whether `request` is one only a terminal answers, which any other
 * descriptor refuses with -ENOTTY, as Linux does.
 */
static int tty_request(uint32_t request)
{
    switch (request) {
    case TCGETS:
    case TCSETS:
    case TCSETSW:
    case TCSETSF:
    case TCGETS2:
    case TCSETS2:
    case TCSETSW2:
    case TCSETSF2:
    case TCFLSH:
    case TIOCGWINSZ:
    case TIOCSWINSZ:
    case TIOCGPGRP:
    case TIOCSPGRP:
    case TIOCSCTTY:
    case TIOCNOTTY:
        return 1;
    default:
        return 0;
    }
}

/**
 * FIONBIO: set O_NONBLOCK on the open-file description of `entry` when the
 * int at `arg` is nonzero, and clear it when it is zero, as F_SETFL does.
 * Linux answers it for every descriptor before the file's own ioctl, and so
 * does sys_ioctl.
 */
static int32_t fd_ioctl_nbio(fd_entry_t *entry, uintptr_t arg)
{
    int on;

    if (copy_from_user(&on, (const void __user *)arg, sizeof(on)) != 0) {
        return -EFAULT;
    }
    if (on) {
        entry->ofd->status_flags |= OFD_NONBLOCK;
    } else {
        entry->ofd->status_flags &= ~(uint32_t)OFD_NONBLOCK;
    }
    return 0;
}

/** The tty behind a descriptor for the shared terminal ioctls: the console for
 * a FD_TYPE_CONSOLE fd, the slave tty for a FD_TYPE_PTS (pty slave) fd, else
 * NULL. */
struct tty *fd_tty(fd_entry_t *e)
{
    if (e->type == FD_TYPE_CONSOLE) {
        return &console_tty;
    }
    if (e->type == FD_TYPE_PTS) {
        return pty_slave_tty(e);
    }
    return NULL;
}

/**
 * The terminal ioctls of the tty `t`.
 *
 * The termios requests and the session and job-control requests, which the
 * console and a pty slave share. Sets *handled = 1 and returns the result for
 * a recognized terminal request; sets *handled = 0 (return value unused)
 * otherwise, so the caller falls through to its own handling. TCGETS/TCSETS
 * exchange the 36-byte kernel termios (n_tty.h), and TCGETS2 and the TCSETS2
 * family the 44-byte termios2, whose speeds TCGETS2 takes from the baud codes
 * of c_cflag and a set does not keep; TCSETSF, TCSETSF2 and TCFLSH flush the
 * unread input. FIONREAD gives the bytes a read would return at once, a
 * complete cooked line or the queued count in raw mode, with a pending EOF
 * counted as 0; `tty_inq` drains the input source first, so input that has
 * just arrived is counted. TCFLSH's TCIFLUSH (0) and TCIOFLUSH (2) drop the
 * input; there is no output queue, so TCOFLUSH (1) does nothing.
 *
 * TIOCSPGRP needs the caller's session to be the one the terminal controls,
 * checked before the argument, as on Linux (-ENOTTY), and a group in that
 * session (`pgrp_in_session`, -EPERM). TIOCSCTTY lets a session leader take
 * a terminal no other session controls, with the caller's group in the
 * foreground; `tty_make_sole_ctty` publishes the session and the group and
 * drops every other terminal the session controlled, the console and any
 * other pty, in one step with interrupts off, so the console's drain in
 * interrupt context never sees a torn or doubled controlling terminal, and
 * /dev/tty resolves to `t`. TIOCNOTTY gives the terminal up when the caller's
 * session controls it.
 */
static int32_t tty_common_ioctl(struct tty *t, uint32_t request, uintptr_t arg, int *handled)
{
    *handled = 1;
    switch (request) {
    case TCGETS:
    case TCGETS2:
        return tty_get_termios_at(t, request, arg);

    case FIONREAD: {
        int navail;
        if (!arg) {
            return -EFAULT;
        }
        navail = tty_inq(t);
        if (copy_to_user((void __user *)arg, &navail, sizeof(navail)) != 0) {
            return -EFAULT;
        }
        return 0;
    }

    case TCSETS:
    case TCSETSW:
    case TCSETSF:
    case TCSETS2:
    case TCSETSW2:
    case TCSETSF2:
        return tty_set_termios_at(t, request, arg);

    case TCFLSH:
        if (arg > 2) {
            return -EINVAL;
        }
        if (arg == 0 || arg == 2) {
            tty_flush_input(t);
        }
        return 0;

    case TIOCGWINSZ:
        if (!arg) {
            return -EFAULT;
        }
        if (copy_to_user((void __user *)arg, &t->winsize, sizeof(t->winsize)) != 0) {
            return -EFAULT;
        }
        return 0;

    case TIOCSWINSZ:
        if (!arg) {
            return -EFAULT;
        }
        if (copy_from_user(&t->winsize, (const void __user *)arg, sizeof(t->winsize)) != 0) {
            return -EFAULT;
        }
        return 0;

    case TIOCGPGRP: {
        int pgrp = t->fg_pgid > 0 ? (int)pid_seen((uint32_t)t->fg_pgid) : 0;
        if (!arg) {
            return -EFAULT;
        }
        if (copy_to_user((void __user *)arg, &pgrp, sizeof(pgrp)) != 0) {
            return -EFAULT;
        }
        return 0;
    }

    case TIOCSPGRP: {
        int pgrp;
        if (!arg) {
            return -EFAULT;
        }
        if (copy_from_user(&pgrp, (const void __user *)arg, sizeof(pgrp)) != 0) {
            return -EFAULT;
        }
        if (t->sid == 0 || (int)current_proc->sid != t->sid) {
            return -ENOTTY;
        }
        if (pgrp <= 0) {
            return -EINVAL;
        }
        pgrp = (int)pid_named((uint32_t)pgrp);
        if (pgrp == 0) {
            return -ESRCH;
        }
        if (!pgrp_in_session(pgrp, t->sid)) {
            return -EPERM;
        }
        t->fg_pgid = pgrp;
        return 0;
    }

    case TIOCSCTTY:
        if ((int)current_proc->sid != (int)current_proc->tgid) {
            return -EPERM; /* not a session leader */
        }
        if (t->sid != 0 && t->sid != (int)current_proc->sid) {
            return -EPERM; /* owned by another session */
        }
        tty_make_sole_ctty(t, (int)current_proc->sid, (int)current_proc->pgid);
        return 0;

    case TIOCNOTTY:
        if (t->sid == (int)current_proc->sid) {
            tty_set_session(t, 0, 0);
        }
        return 0;

    default:
        *handled = 0;
        return 0;
    }
}

/**
 * Change framebuffer mode or palette only for the process that owns it.
 *
 * A descriptor can survive a return to text mode and another process's open.
 * Mode 13h reacquires an unowned display; other requests require ownership.
 */
static int32_t framebuffer_ioctl(uint32_t request, uintptr_t arg)
{
    if (request != FB_MODE_13H && request != FB_MODE_TEXT && request != FB_SETPALETTE) {
        return -ENOTTY;
    }
    if (fb_owner_pid != current_proc->tgid && (fb_owner_pid != 0 || request != FB_MODE_13H)) {
        return -EBUSY;
    }
    switch (request) {
    case FB_MODE_13H:
        if (!gfx_is_active()) {
            fb_enter();
        }
        fb_owner_pid = current_proc->tgid;
        return 0;
    case FB_MODE_TEXT:
        fb_exit();
        fb_owner_pid = 0;
        return 0;
    default: {
        static uint8_t palette[768];

        if (!arg || copy_from_user(palette, (const void __user *)arg, sizeof(palette)) != 0) {
            return -EFAULT;
        }
        fb_set_palette(palette);
        return 0;
    }
    }
}

/**
 * Device control (ioctl(2)).
 *
 * Its arguments are fd, request and arg. Returns 0, the request's result, or
 * -errno: -ENOTTY for a request the descriptor does not answer, a terminal
 * request on a descriptor that is no terminal among them, as on Linux.
 *
 * The descriptor is checked first, so a bad one is -EBADF whatever the
 * request, `ioctl(-1, TCGETS)` included, and an O_PATH one is -EBADF to every
 * request, as on Linux. The requests then go, in order, to: FIONBIO, which
 * every descriptor answers (`fd_ioctl_nbio`); FS_IOC_GETFLAGS and the other
 * file attribute requests (`fs_flags_ioctl`); the interface requests of
 * `netif_ioctl` on a socket of any family, as on Linux, where glibc's
 * if_nametoindex uses an AF_UNIX one and a DHCP client sets the machine's
 * address; the termios and session requests of `tty_common_ioctl` on the
 * console and a pty slave, ahead of the slave's own operations; and the
 * `ioctl` of an object with operations of its own, such as a pipe or a pty
 * master, -ENOTTY when it has none. The framebuffer answers its FB_* requests
 * (`framebuffer_ioctl`), and the console KIOCSOUND and the AUDIO_* requests;
 * these go by the descriptor's type, so a descriptor 1 reopened onto a
 * regular file never reaches the speaker. AUDIO_WRITE checks the caller's
 * samples with `user_buffer_ok`, and the driver reads them in place.
 */
long sys_ioctl(isr_params_t *params)
{
    int fd_num = (int)SC_ARG(params, 0);
    uint32_t request = (uint32_t)SC_ARG(params, 1);
    uintptr_t arg = SC_ARG(params, 2);
    fd_entry_t *entry;

    if (!current_proc) {
        return -ESRCH;
    }

    entry = fd_get(current_proc->fds, fd_num);
    if (!entry || (entry->flags & FD_FLAG_PATH)) {
        return -EBADF;
    }
    if (request == FIONBIO) {
        return fd_ioctl_nbio(entry, arg);
    }
    {
        int handled;
        int32_t r = fs_flags_ioctl(entry, request, arg, &handled);

        if (handled) {
            return r;
        }
    }

    if ((entry->type == FD_TYPE_SOCKET && entry->sock) || entry->type == FD_TYPE_UNIX) {
        int handled;
        int32_t r = netif_ioctl(request, arg, &handled);

        if (handled) {
            return r;
        }
    }

    {
        struct tty *t = fd_tty(entry);
        if (t) {
            int handled;
            int32_t r = tty_common_ioctl(t, request, arg, &handled);
            if (handled) {
                return r;
            }
        }
    }

    if (entry->ops) {
        return entry->ops->ioctl ? entry->ops->ioctl(entry, request, arg) : -ENOTTY;
    }
    if (tty_request(request)) {
        return -ENOTTY; /* terminal-only requests on a non-terminal fd */
    }

    if (entry->type == FD_TYPE_FRAMEBUFFER) {
        return framebuffer_ioctl(request, arg);
    }

    if (entry->type == FD_TYPE_CONSOLE) {
        if (request == KIOCSOUND) {
            if (arg == 0) {
                speaker_off();
            } else {
                speaker_on((uint32_t)arg);
            }
            return 0;
        }

        switch (request) {
        case AUDIO_GET_INFO: {
            struct audio_info kinfo;
            if (arg) {
                k_memset(&kinfo, 0, sizeof(kinfo));
                kinfo.driver_type = audio ? audio->type : AUDIO_DRIVER_NONE;
                kinfo.has_real_audio = audio_has_real_output() ? 1 : 0;
                if (copy_to_user((void __user *)arg, &kinfo, sizeof(kinfo)) != 0) {
                    return -EFAULT;
                }
            }
            return 0;
        }

        case AUDIO_START: {
            struct audio_start_params kp;
            if (!audio || !arg) {
                return -EFAULT;
            }
            if (copy_from_user(&kp, (const void __user *)arg, sizeof(kp)) != 0) {
                return -EFAULT;
            }
            return audio->start(kp.sample_rate, (audio_format_t)kp.format, (int)kp.channels);
        }

        case AUDIO_STOP:
            if (audio) {
                audio->stop();
                return 0;
            }
            return -ENODEV;

        case AUDIO_WRITE: {
            struct audio_write_params kp;
            if (!audio || !arg) {
                return -EFAULT;
            }
            if (copy_from_user(&kp, (const void __user *)arg, sizeof(kp)) != 0) {
                return -EFAULT;
            }
            if (!kp.buffer || !user_buffer_ok((uintptr_t)kp.buffer, kp.size, 0)) {
                return -EFAULT;
            }
            return audio->write(kp.buffer, kp.size);
        }

        case AUDIO_SET_VOLUME:
            if (audio) {
                audio->set_volume((int)arg);
                return 0;
            }
            return -ENODEV;
        default:
            break;
        }
    }

    return -ENOTTY;
}

/**
 * Whether the `len` bytes at `addr` lie wholly in user space, between the
 * architecture's USER_ADDR_MIN and USER_ADDR_MAX, so that a system call
 * argument cannot read or write kernel memory: `user_range_ok` of
 * mm/usercopy under the name the system call layer uses. 1 yes, 0 no.
 */
int user_access_ok(uintptr_t addr, size_t len)
{
    return user_range_ok(addr, len);
}

/**
 * Range-check a user buffer and bring in its pages before the kernel passes
 * the raw pointer to a lower layer (krffs, tcp, udp, the drivers) that
 * dereferences it directly. With demand paging a pointer inside user space
 * may still be unmapped (a never-touched heap page, or a wild address in no
 * VMA); `vm_prefault_range` faults the valid pages in here and fails on the
 * bad ones, so the lower layer's raw access does not fault in kernel mode.
 * 'write' is 1 when that layer writes into the buffer (so the pages need
 * write permission). Returns 1 or 0; it may sleep, and a 1 holds until the
 * caller sleeps.
 */
int user_buffer_ok(uintptr_t addr, size_t len, int write)
{
    return user_access_ok(addr, len) && vm_prefault_range(addr, len, write);
}

/**
 * Process times (times(2)).
 *
 * Its argument is struct tms *, which may be NULL when the caller wants only
 * the return value. The four fields are the CPU time the timer charged to the
 * caller in user mode and in the kernel, and the same two for the children it
 * has reaped, in ticks, which are the 100 Hz CLK_TCK units the structure uses.
 * They are read with interrupts masked, since the tick adds to them. The return
 * value is the monotonic tick count.
 */
long sys_times(isr_params_t *params)
{
    struct kernel_tms {
        long tms_utime;
        long tms_stime;
        long tms_cutime;
        long tms_cstime;
    } t;

    if (SC_ARG(params, 0)) {
        uint32_t user;
        uint32_t sys;

        proc_cpu_time(current_proc, &user, &sys);
        t.tms_utime = (int32_t)user;
        t.tms_stime = (int32_t)sys;
        t.tms_cutime = (int32_t)current_proc->tg->cutime;
        t.tms_cstime = (int32_t)current_proc->tg->cstime;
        if (copy_to_user((void __user *)SC_ARG(params, 0), &t, sizeof(t)) != 0) {
            return -EFAULT;
        }
    }
    return (int32_t)kernel_ticks;
}

/**
 * System statistics (sysinfo(2)).
 *
 * Fills the struct sysinfo of longs: uptime in seconds, the load
 * averages the scheduler keeps (`sched_loadavg`), with 16 bits of fraction
 * as Linux gives them, the count of process records including unreaped
 * children, total and free RAM from the frame counts, the buffer cache's
 * pages as the buffer RAM, which free(1) adds to /proc/meminfo's Cached for
 * its buff/cache, mem_unit = 1; shared and swap figures are reported as zero
 * (not tracked). Its argument is struct sysinfo *.
 *
 * The structure is Linux's, whose tail pads it to 64 bytes where a long has
 * 32 bits and to nothing where it has 64: 64 bytes on i686 and 112 on
 * x86-64, the size of the union.
 */
long sys_sysinfo(isr_params_t *params)
{
    union {
        struct kernel_sysinfo {
            long uptime;
            unsigned long loads[3];
            unsigned long totalram, freeram, sharedram, bufferram;
            unsigned long totalswap, freeswap;
            uint16_t procs;
            uint16_t pad;
            unsigned long totalhigh, freehigh;
            uint32_t mem_unit;
        } si;
        char bytes[64];
    } u;
    uint32_t avg[3];
    int n = 0;

    if (!SC_ARG(params, 0)) {
        return -EFAULT;
    }
    {
        const process_t *p;

        for (p = proc_list; p; p = p->next) {
            n++;
        }
    }
    k_memset(&u, 0, sizeof(u));
    u.si.uptime = (long)(kernel_ticks / 100);
    sched_loadavg(avg);
    u.si.loads[0] = avg[0] << (16 - SCHED_LOAD_SHIFT);
    u.si.loads[1] = avg[1] << (16 - SCHED_LOAD_SHIFT);
    u.si.loads[2] = avg[2] << (16 - SCHED_LOAD_SHIFT);
    u.si.totalram = (unsigned long)frame_total_count() * PAGE_SIZE;
    u.si.freeram = (unsigned long)frame_free_count() * PAGE_SIZE;
    u.si.sharedram = 0;
    u.si.bufferram = (unsigned long)bcache_resident() * PAGE_SIZE;
    u.si.totalswap = 0;
    u.si.freeswap = 0;
    u.si.procs = (uint16_t)n;
    u.si.pad = 0;
    u.si.totalhigh = 0;
    u.si.freehigh = 0;
    u.si.mem_unit = 1;
    if (copy_to_user((void __user *)SC_ARG(params, 0), &u, sizeof(u)) != 0) {
        return -EFAULT;
    }
    return 0;
}

/**
 * Record the address the caller's thread end clears and return the caller's
 * thread id (set_tid_address(2)).
 *
 * The address, its argument, becomes `clear_child_tid`, which the thread's
 * end clears and wakes as a futex when another process shares the address
 * space (`proc_thread_gone`). The id is the thread's, as the caller's pid
 * namespace sees it (`pid_seen`).
 */
long sys_set_tid_address(isr_params_t *params)
{
    if (!current_proc) {
        return -ESRCH;
    }
    current_proc->clear_child_tid = SC_ARG(params, 0);
    return (int32_t)pid_seen(current_proc->pid);
}

/*
 * The struct utsname of uname(2): six fields of 65 bytes, the new-style
 * layout musl expects.
 */
struct kernel_utsname {
    char sysname[65];
    char nodename[65];
    char release[65];
    char version[65];
    char machine[65];
    char domainname[65];
};

const char *uts_hostname(void)
{
    return proc_utsns(current_proc)->hostname;
}

const char *uts_domainname(void)
{
    return proc_utsns(current_proc)->domainname;
}

/** Set `field`, 65 bytes, to the `len` bytes of `name`, at most 64. */
static void uts_set(char *field, const char *name, uint32_t len)
{
    if (len > 64) {
        len = 64;
    }
    k_memcpy(field, name, len);
    field[len] = '\0';
}

void uts_set_hostname(const char *name, uint32_t len)
{
    uts_set(proc_utsns(current_proc)->hostname, name, len);
}

void uts_set_domainname(const char *name, uint32_t len)
{
    uts_set(proc_utsns(current_proc)->domainname, name, len);
}

int uts_privileged(void)
{
    return ns_owner_privileged(current_proc, &proc_utsns(current_proc)->c);
}

/** Copy a C string into a fixed 65-byte utsname field (always
 * NUL-terminated). */
static void uname_field(char *dst, const char *src)
{
    uint32_t n = (uint32_t)k_strlen(src);

    if (n > 64) {
        n = 64;
    }
    k_memcpy(dst, src, n);
    dst[n] = '\0';
}

/* syslog(2)'s actions, Linux's numbers. */
#define SYSLOG_ACTION_READ          2
#define SYSLOG_ACTION_READ_ALL      3
#define SYSLOG_ACTION_READ_CLEAR    4
#define SYSLOG_ACTION_CLEAR         5
#define SYSLOG_ACTION_CONSOLE_OFF   6
#define SYSLOG_ACTION_CONSOLE_ON    7
#define SYSLOG_ACTION_CONSOLE_LEVEL 8
#define SYSLOG_ACTION_SIZE_UNREAD   9
#define SYSLOG_ACTION_SIZE_BUFFER   10

/* Where syslog(2)'s reader goes on from, the one place every reader shares,
 * as Linux's syslog_seq; where a read of the whole ring begins after a
 * clear; and the screen's level CONSOLE_OFF put aside, -1 for none. */
static uint32_t syslog_read_at;
static uint32_t syslog_clear_at;
static int syslog_saved_level = -1;

/**
 * Copy the ring from the byte `*at` to the caller's `ubuf`, at most `len`
 * bytes, moving `*at` past what was copied, through a buffer on the stack so
 * the ring is never held across the copy. Returns the bytes copied, or
 * -EFAULT when none could be.
 */
static int32_t syslog_copy(char __user *ubuf, uint32_t len, uint32_t *at)
{
    char chunk[256];
    uint32_t done = 0, n;

    while (done < len &&
           (n = klog_read_from(chunk,
                               len - done < sizeof(chunk) ? len - done : (uint32_t)sizeof(chunk),
                               at)) > 0) {
        if (copy_to_user(ubuf + done, chunk, n) != 0) {
            return done > 0 ? (int32_t)done : -EFAULT;
        }
        done += n;
    }
    return (int32_t)done;
}

/** Where a read of the last `len` bytes of the ring from `from` begins: the
 * first record wholly within them. */
static uint32_t syslog_tail(uint32_t from, uint32_t len)
{
    uint32_t end = klog_written();
    char c;

    from = klog_clamp(from);
    if (end - from <= len) {
        return from;
    }
    from = end - len;
    while (from != end && klog_read_from(&c, 1, &from) == 1 && c != '\n') {
    }
    return from;
}

/**
 * syslog, which glibc calls klogctl: the kernel's ring as BusyBox's klogd
 * and util-linux's dmesg read it. Its arguments are the action, the buffer
 * and its length. READ waits for records past the shared reader's
 * place and takes them; READ_ALL gives the last records that fit since the
 * last CLEAR, and READ_CLEAR clears after; SIZE_UNREAD and SIZE_BUFFER are
 * the bytes the reader has not taken and the ring's size; CONSOLE_LEVEL sets
 * the lowest priority the screen shows to one less than the length, as
 * Linux's console_loglevel shows what is more urgent than it, CONSOLE_OFF
 * turns the screen down to the critical records and CONSOLE_ON back. CLOSE
 * and OPEN do nothing. Every action but those two needs a privileged caller,
 * as Linux with dmesg_restrict has it, since the ring is /dev/bootlog's,
 * which is root's. EINVAL for another action, a negative length or a level
 * out of 1 to 8, EINTR when a signal ends READ's wait.
 *
 * The ring wakes no one, since records come from any context, so READ looks
 * again every tenth of a second.
 */
long sys_syslog(isr_params_t *params)
{
    int32_t action = (int32_t)SC_ARG(params, 0);
    char __user *ubuf = (char __user *)SC_ARG(params, 1);
    int32_t len = (int32_t)SC_ARG(params, 2);
    uint32_t at;
    int32_t rc;

    if (action == 0 || action == 1) {
        return 0;
    }
    if (action < SYSLOG_ACTION_READ || action > SYSLOG_ACTION_SIZE_BUFFER) {
        return -EINVAL;
    }
    if (!proc_privileged(current_proc)) {
        return -EPERM;
    }
    switch (action) {
    case SYSLOG_ACTION_READ:
        if (len < 0) {
            return -EINVAL;
        }
        if (len == 0) {
            return 0;
        }
        while ((syslog_read_at = klog_clamp(syslog_read_at)) == klog_written()) {
            if (sleep_until(kernel_ticks + 10u, NULL) != 0) {
                return -EINTR;
            }
        }
        return syslog_copy(ubuf, (uint32_t)len, &syslog_read_at);
    case SYSLOG_ACTION_READ_ALL:
    case SYSLOG_ACTION_READ_CLEAR:
        if (len < 0) {
            return -EINVAL;
        }
        at = syslog_tail(syslog_clear_at, (uint32_t)len);
        rc = syslog_copy(ubuf, (uint32_t)len, &at);
        if (rc >= 0 && action == SYSLOG_ACTION_READ_CLEAR) {
            syslog_clear_at = klog_written();
        }
        return rc;
    case SYSLOG_ACTION_CLEAR:
        syslog_clear_at = klog_written();
        return 0;
    case SYSLOG_ACTION_CONSOLE_OFF:
        if (syslog_saved_level < 0) {
            syslog_saved_level = klog_screen_level();
        }
        klog_set_screen_level(KLOG_CRIT);
        return 0;
    case SYSLOG_ACTION_CONSOLE_ON:
        if (syslog_saved_level >= 0) {
            klog_set_screen_level(syslog_saved_level);
            syslog_saved_level = -1;
        }
        return 0;
    case SYSLOG_ACTION_CONSOLE_LEVEL:
        if (len < 1 || len > 8) {
            return -EINVAL;
        }
        klog_set_screen_level(len - 1);
        syslog_saved_level = -1;
        return 0;
    case SYSLOG_ACTION_SIZE_UNREAD:
        return (int32_t)(klog_written() - klog_clamp(syslog_read_at));
    default:
        return KLOG_RING_SIZE;
    }
}

/**
 * uname(2): fill the struct utsname at its argument with the fixed fields of
 * syscall.h, the host and domain names of the caller's UTS namespace and the
 * architecture's name. Returns 0 or -EFAULT.
 */
long sys_uname(isr_params_t *params)
{
    struct kernel_utsname __user *user_uts = (struct kernel_utsname __user *)SC_ARG(params, 0);
    struct kernel_utsname uts;

    k_memset(&uts, 0, sizeof(uts));
    uname_field(uts.sysname, UTS_SYSNAME);
    uname_field(uts.nodename, uts_hostname());
    uname_field(uts.release, UTS_RELEASE);
    uname_field(uts.version, UTS_VERSION);
    uname_field(uts.machine, ARCH_NAME);
    uname_field(uts.domainname, uts_domainname());

    if (copy_to_user(user_uts, &uts, sizeof(uts)) != 0) {
        return -EFAULT;
    }
    return 0;
}

/**
 * The body of sethostname and setdomainname: set the host name (uname's
 * nodename, what gethostname reads back) or the domain name of the caller's
 * UTS namespace, as `set` chooses, to the string of the first argument, the
 * second's count of bytes, at most 64. Returns 0; -EPERM for a caller not
 * privileged over the namespace, as on Linux; -EINVAL for a count outside 0
 * to 64; or -EFAULT.
 */
static int32_t uts_set_call(isr_params_t *params, void (*set)(const char *, uint32_t))
{
    const char __user *name = (const char __user *)SC_ARG(params, 0);
    int len = (int)SC_ARG(params, 1);
    char buf[65];

    if (!uts_privileged()) {
        return -EPERM;
    }
    if (len < 0 || len > 64) {
        return -EINVAL;
    }
    if (len > 0 && copy_from_user(buf, name, (uint32_t)len) != 0) {
        return -EFAULT;
    }
    set(buf, (uint32_t)len);
    return 0;
}

/** sethostname(2): `uts_set_call` on the host name. */
long sys_sethostname(isr_params_t *params)
{
    return uts_set_call(params, uts_set_hostname);
}

/** setdomainname(2): `uts_set_call` on the domain name. */
long sys_setdomainname(isr_params_t *params)
{
    return uts_set_call(params, uts_set_domainname);
}

/* Whether `rach2.trace=enosys` is on the command line, which makes
 * `syscall_missing` report each call number the table has no handler for.
 * A number is reported again whenever a process other than the last to
 * report it makes the call: one word a number keeps that process, so each
 * program's own missing calls appear even after an earlier program made the
 * same ones, and two programs making the same number in turn each report
 * it. */
static int _trace_enosys = 0;

static uint32_t _enosys_last_pid[SYSCALL_TABLE_SIZE];

/* The last number past the table that was reported, and the process that
 * made it: the numbers past the table share this one entry, so one process
 * making the same number twice reports it once, and two numbers in turn are
 * each reported. */
static uint32_t _enosys_big_num = 0;

static uint32_t _enosys_big_pid = 0xFFFFFFFFu;

/**
 * Whether the call the handler `fn` answers, with the arguments in `params`,
 * which a signal interrupted with EINTR, may be restarted, by a signal no
 * handler takes or by a handler with SA_RESTART. The calls that are not are
 * the ones Linux never restarts after a handler, which wait for a signal or
 * for a deadline, a timed futex wait among them (`futex_restartable`), and
 * the two returns from a handler, whose result register is the interrupted
 * call's. The call is told by its handler, since its number differs between
 * the architectures.
 */
static int syscall_restartable(syscall_fn_t fn, const isr_params_t *params)
{
    if (fn == sys_futex) {
        return futex_restartable(params);
    }
    return fn != sys_pause && fn != sys_select && fn != sys_sigreturn && fn != sys__newselect &&
           fn != sys_nanosleep && fn != sys_poll && fn != sys_rt_sigreturn &&
           fn != sys_rt_sigtimedwait && fn != sys_rt_sigsuspend && fn != sys_epoll_wait &&
           fn != sys_clock_nanosleep && fn != sys_pselect6 && fn != sys_ppoll &&
           fn != sys_epoll_pwait && fn != sys_epoll_pwait2;
}

/**
 * Report a call this kernel does not have, when `rach2.trace=enosys` is on
 * the command line: a missing call is otherwise invisible, -ENOSYS going back
 * in a register with nothing printed, so a program that fails for want of one
 * fails for no stated reason. Each number is reported once per process, a
 * number past the table as one in it, so a program that makes a missing call
 * in a loop prints one line and the console stays readable.
 */
static void syscall_missing(uint32_t syscall_num)
{
    uint32_t pid = current_proc ? current_proc->pid : 0;
    const char *who = current_proc ? current_proc->comm : "?";

    if (!_trace_enosys) {
        return;
    }
    if (syscall_num >= SYSCALL_TABLE_SIZE) {
        if (_enosys_big_num != syscall_num || _enosys_big_pid != pid) {
            _enosys_big_num = syscall_num;
            _enosys_big_pid = pid;
            klog(KLOG_WARN, "syscall", "%d: not implemented, and past the table (asked for by %s)",
                 (int)syscall_num, who);
        }
    } else if (_enosys_last_pid[syscall_num] != pid) {
        _enosys_last_pid[syscall_num] = pid;
        klog(KLOG_WARN, "syscall", "%d: not implemented (asked for by %s)", (int)syscall_num, who);
    }
}

/**
 * The system call entry's handler: dispatch on the call's number.
 *
 * Looks up the number in _syscall_table and calls the handler, storing the
 * result in the result register (SC_RET) for the return to user mode. A
 * number with no handler answers -ENOSYS, on which musl falls back to another
 * call rather than taking it for a failure. Marks the process non-preemptible
 * (in_syscall) around the dispatch and runs the body with interrupts on, so
 * the timer and the sound card keep interrupting during a long call, such as
 * Doom's framebuffer writes. A switch an interrupt requested during the call
 * is taken at its exit, with interrupts off again and the trapframe whole, so
 * a process that is always inside a call gives way once per call; a call that
 * does not return does not. The descriptors the call held in a shared table
 * are let go as the body returns, before a tracer's exit stop
 * (`fd_holds_release`).
 *
 * The entry stub keeps interrupts off while it saves the trapframe, and
 * in_syscall is set before they are enabled, so no timer interrupt can
 * reschedule a process whose trapframe is still half built; the timer does
 * not reschedule a process while in_syscall is set. Interrupts are off again
 * for the epilogue, which restores the trapframe, and the return to user
 * mode restores the caller's interrupt flag. Before the dispatch,
 * `usbdisk_service` publishes the USB sticks that arrived and reclaims those
 * that left, work that waits for a process context; it returns at once when
 * there is none.
 *
 * A tracer's stop at the entry may change the call through orig_nr, as
 * Linux's `syscall_trace_enter` lets it: the number is read again after the
 * stop, a number of no call answers -ENOSYS, and -1 runs no call, whose
 * result is then what the tracer left in the result register, -ENOSYS unless
 * it set one.
 * The thread's seccomp filters then judge the call the tracer left, as
 * Linux runs them after the stop, and may answer it without running it.
 *
 * Whether `deliver_signals` restarts a call a signal interrupted, on this
 * return, is recorded in `restart_nr` before the exit stop, so a tracer
 * there cancels the restart with -1 in orig_nr, and with a result other
 * than EINTR.
 *
 * At the exit the datagrams the call put on the loopback ring are delivered,
 * rather than only at the next tick, so a local transfer is not paced at a
 * ring a tick. The threshold is three datagrams of the ring's 64
 * (`ip_loop_crowded`): one or two are left to the timer, which lets the two
 * crossing SYNs of a simultaneous open both be queued before either is
 * delivered. The sender's own frames are gone by then, so no delivery runs
 * under them. Interrupts are off, the drain does not run inside itself, and
 * it takes at most four times the ring's slots in one run; what is left is
 * taken by the next call's exit or by the timer.
 */
static void _syscall_handler(isr_params_t *params)
{
    uint32_t syscall_num = (uint32_t)SC_NR(params);
    long ret = -ENOSYS;
    int skip = 0;

    if (current_proc) {
        current_proc->in_syscall = 1;
    }

    arch_irq_enable();

    usbdisk_service();

    if (current_proc) {
        current_proc->orig_nr = (int32_t)syscall_num;
        if (current_proc->ptrace & PT_SYSCALL) {
            ptrace_syscall_stop(params, syscall_num, 0);
            syscall_num = (uint32_t)current_proc->orig_nr;
            skip = current_proc->orig_nr == -1;
        }
        if (!skip && current_proc->seccomp_mode != SECCOMP_MODE_DISABLED &&
            !seccomp_permit(params, syscall_num, &ret)) {
            skip = 1;
            SC_RET(params) = (unsigned long)ret;
        }
    }
    if (skip) {
        ret = (long)SC_RET(params);
    } else if (syscall_num < SYSCALL_TABLE_SIZE && _syscall_table[syscall_num]) {
        ret = _syscall_table[syscall_num](params);
    } else {
        syscall_missing(syscall_num);
    }

    fd_holds_release();
    if (current_proc) {
        current_proc->restart_nr = 0;
        if (!skip && ret == -EINTR && syscall_num < SYSCALL_TABLE_SIZE &&
            _syscall_table[syscall_num] &&
            syscall_restartable(_syscall_table[syscall_num], params)) {
            current_proc->restart_nr = syscall_num + 1u;
        }
    }
    if (current_proc && (current_proc->ptrace & (PT_SYSCALL | PT_STEP))) {
        SC_RET(params) = (unsigned long)ret;
        ptrace_syscall_stop(params, syscall_num, 1);
        ret = (long)SC_RET(params);
        if (ret != -EINTR) {
            current_proc->restart_nr = 0;
        }
    }

    arch_irq_disable();

    if (current_proc) {
        current_proc->in_syscall = 0;
    }
    if (ip_loop_crowded()) {
        ip_loop_drain_from(IP_LOOP_FROM_CALL);
    }
    sched_take_preempt();

    /* The result, in the register the return to user mode restores it from. */
    SC_RET(params) = (unsigned long)ret;
}

void init_syscall(void)
{
    uint32_t i;

    init_kstacks();

    for (i = 0; i < SYSCALL_TABLE_SIZE; i++) {
        _syscall_table[i] = NULL;
    }
    arch_syscall_init(_syscall_table, _syscall_handler);

    _trace_enosys = kcmdline_has("rach2.trace=enosys");
}
