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
#include "kd.h"
#include "mouse.h"
#include "proc/process.h"
#include "proc/sched.h"
#include "proc/smp.h"
#include "proc/elf.h"
#include "proc/uimage.h"
#include "proc/futex.h"
#include "proc/tgroup.h"
#include "proc/ptrace.h"
#include "proc/seccomp.h"
#include "proc/smp.h"
#include "proc/ns.h"
#include "lib/krandom.h"
#include "lib/kerrno.h"
#include "lib/kirq.h"
#include "lib/ksignal.h"
#include "timer.h"
#include "vdso.h"
#include "fs/fd.h"
#include "fs/file.h"
#include "ipc/pipe.h"
#include "io/poll.h"
#include "usb/usb.h"
#include "tty/tty.h"
#include "tty/pty.h"
#include "ntp.h"
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
#include "display/blank.h"
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

/*
 * ioctl request codes (Linux compatible subset)
 */
#define KIOCSOUND 0x4B2F /* sound the speaker: a PIT divisor, 0 silences */
#define KDMKTONE  0x4B30 /* a tone: ms in the high 16 bits, divisor in the low */

#define TIOCLINUX 0x541C /* The Linux console's subcodes, in the byte at arg */

#define TIOCL_UNBLANKSCREEN 4  /* Unblank the console */
#define TIOCL_BLANKSCREEN   14 /* Blank it until it is unblanked */
#define TIOCL_BLANKEDSCREEN 15 /* Whether it is blank */

#define TCGETS 0x5401 /* Get terminal attributes (termios) */

#define TCSETS 0x5402 /* Set terminal attributes now */

#define TCSETSW 0x5403 /* Set after the output drains */

#define TCSETSF 0x5404 /* Set after output drains, flush input */

#define TCGETS2 0x802C542A /* Get the termios2 attributes: termios and speeds */

#define TCSETS2 0x402C542B /* Set the termios2 attributes now */

#define TCSETSW2 0x402C542C /* Set them after the output drains */

#define TCSETSF2 0x402C542D /* Set them after output drains, flush input */

#define TCFLSH 0x540B /* Flush input and/or output queues */

#define TCXONC 0x540A /* Stop or restart output or input: tcflow(3) */

#define FIONREAD 0x541B /* Bytes available to read */

#define FIONBIO 0x5421 /* Set or clear O_NONBLOCK */

#define FIONCLEX 0x5450 /* Clear close-on-exec */

#define FIOCLEX 0x5451 /* Set close-on-exec */

#define TIOCGWINSZ 0x5413 /* Get terminal window size */

#define TIOCSWINSZ 0x5414 /* Set terminal window size */

#define TIOCSCTTY 0x540E /* Make this tty the controlling terminal */

#define TIOCGPGRP 0x540F /* Get the foreground process group */

#define TIOCSPGRP 0x5410 /* Set the foreground process group */

#define TIOCNOTTY 0x5422 /* Detach from the controlling terminal */

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

#if defined(IA32_SYSCALL_TABLE_SIZE)
/* The handlers of the i386 calls a frame's SC_COMPAT marks, by i386's
 * numbers, which `ia32_syscall_init` fills. */
static syscall_fn_t _ia32_syscall_table[IA32_SYSCALL_TABLE_SIZE];

/* The compat ioctl (arch/x86_64/ia32_ioctl.c), which restarts as ioctl does
 * (`syscall_restartable`). */
long compat_sys_ioctl(isr_params_t *params);

/* i386's rt_sigprocmask (arch/x86_64/ia32_signal.c), which calls the native
 * handler and runs without the big lock as it does (`init_syscall`). */
long compat_sys_rt_sigprocmask(isr_params_t *params);
/* i386 socketcall uses the same per-family lock dispatch as native calls. */
long compat_sys_socketcall(isr_params_t *params);
#endif

/* The numbers of each table whose calls run without the big lock
 * (`syscall_unlock`), a bit each. */
static uint32_t _syscall_unlocked[(SYSCALL_TABLE_SIZE + 31) / 32];
#if defined(IA32_SYSCALL_TABLE_SIZE)
static uint32_t _ia32_syscall_unlocked[(IA32_SYSCALL_TABLE_SIZE + 31) / 32];
#endif

/** The table the call in `params` is numbered in, and its size in `size`:
 * the compat table for a call SC_COMPAT marks, else the native one. */
static syscall_fn_t *syscall_table_of(const isr_params_t *params, uint32_t *size)
{
#if defined(IA32_SYSCALL_TABLE_SIZE)
    if (SC_COMPAT(params)) {
        *size = IA32_SYSCALL_TABLE_SIZE;
        return _ia32_syscall_table;
    }
#endif
    (void)params;
    *size = SYSCALL_TABLE_SIZE;
    return _syscall_table;
}

void syscall_unlock(int abi, syscall_fn_t handler)
{
    syscall_fn_t *table = _syscall_table;
    uint32_t *bits = _syscall_unlocked;
    uint32_t size = SYSCALL_TABLE_SIZE, i;

    if (abi == SYSCALL_ABI_IA32) {
#if defined(IA32_SYSCALL_TABLE_SIZE)
        table = _ia32_syscall_table;
        bits = _ia32_syscall_unlocked;
        size = IA32_SYSCALL_TABLE_SIZE;
#else
        return;
#endif
    } else if (abi != SYSCALL_ABI_NATIVE) {
        return;
    }
    for (i = 0; i < size; i++) {
        if (handler != 0 && table[i] == handler) {
            bits[i / 32] |= 1u << (i % 32);
        }
    }
}

int syscall_entry_locked(const isr_params_t *params)
{
    const process_t *p = current_proc;
    const uint32_t *bits = _syscall_unlocked;
    uint32_t nr = (uint32_t)SC_NR(params), size = SYSCALL_TABLE_SIZE;

#if defined(IA32_SYSCALL_TABLE_SIZE)
    if (SC_COMPAT(params)) {
        bits = _ia32_syscall_unlocked;
        size = IA32_SYSCALL_TABLE_SIZE;
    }
#endif
    if (!p || p->fd_holding || usbdisk_waiting) {
        return 1;
    }
    if ((p->work & WORK_ENTRY) &&
        ((p->ptrace & (PT_SYSCALL | PT_STEP)) || p->seccomp_mode != SECCOMP_MODE_DISABLED)) {
        return 1;
    }
    /* A number is marked only where its table has a handler. */
    return nr >= size || !(bits[nr / 32] & (1u << (nr % 32)));
}

int in_compat_syscall(void)
{
    return current_proc && current_proc->in_syscall && SC_COMPAT(proc_user_frame(current_proc));
}

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

/* The clock calls made, `clock_calls`; a count that wraps, kept without
 * a lock, since a test reads it while nothing else reads the clock. */
static volatile uint32_t clock_call_count;

uint32_t clock_calls(void)
{
    return clock_call_count;
}

/*
 * The timeval of gettimeofday and settimeofday.
 */
struct kernel_timeval {
    long tv_sec;  /* seconds */
    long tv_usec; /* microseconds */
};

/*
 * The struct timezone of gettimeofday and settimeofday, the same in every
 * ABI, and the kernel's, Linux's sys_tz (kernel/time/time.c:50), which only
 * settimeofday changes and nothing in the kernel reads. Each field is one
 * store, read and written without a lock, as Linux's are.
 */
struct kernel_timezone {
    int32_t tz_minuteswest; /* minutes west of UTC, -900 to 900 */
    int32_t tz_dsttime;     /* the kind of summer time, not checked */
};

static struct kernel_timezone sys_tz;

/**
 * gettimeofday(2) takes tv (timeval*) and tz (timezone*), each of which may
 * be null, as Linux's (kernel/time/time.c:140-156). Reports real time from
 * the clock anchored at boot (clock.c), or time since boot when there was no
 * clock to read, in seconds as wide as a long, at tv, then the time zone
 * settimeofday last set at tz. Returns 0 or -EFAULT, with tv written when
 * only tz faults.
 */
long sys_gettimeofday(isr_params_t *params)
{
    struct kernel_timeval __user *tv = (struct kernel_timeval __user *)SC_ARG(params, 0);
    struct kernel_timezone __user *tz = (struct kernel_timezone __user *)SC_ARG(params, 1);
    struct kernel_timeval ktv;
    struct kernel_timezone ktz;
    int64_t seconds;
    int64_t nanoseconds;

    clock_call_count++;
    if (tv) {
        get_current_time(&seconds, &nanoseconds);
        ktv.tv_sec = (long)seconds;
        ktv.tv_usec = (int32_t)nanoseconds / 1000; /* narrow first: no __divdi3 */
        if (copy_to_user(tv, &ktv, sizeof(ktv)) != 0) {
            return -EFAULT;
        }
    }
    if (tz) {
        ktz.tz_minuteswest = sys_tz.tz_minuteswest;
        ktz.tz_dsttime = sys_tz.tz_dsttime;
        if (copy_to_user(tz, &ktz, sizeof(ktz)) != 0) {
            return -EFAULT;
        }
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

    clock_call_count++;
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

/* The kinds of clock the ids name: real time and the monotonic clock read
 * within the tick, the two at the last tick, the raw clock, which the NTP
 * state does not slew, and a process's CPU time, which counts ticks. */
enum kclock_kind {
    KCLOCK_REAL,
    KCLOCK_REAL_COARSE,
    KCLOCK_MONO,
    KCLOCK_MONO_COARSE,
    KCLOCK_MONO_RAW,
    KCLOCK_CPU
};

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
    case CLOCK_BOOTTIME:
        out->kind = KCLOCK_MONO;
        return 0;
    case CLOCK_MONOTONIC_RAW:
        out->kind = KCLOCK_MONO_RAW;
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
static uint64_t clock_cpu_ticks(const struct kclock *c)
{
    uint64_t user;
    uint64_t sys;

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
 * The monotonic clock is the uptime seconds and the ticks within the second,
 * each tick the NTP state's length, with the timer's place added
 * (`rtc_monotonic`), read with interrupts masked, so no torn second can make
 * it step back; its coarse form stops at the tick. Real time is that clock
 * plus the offset the reading at boot and each setting made (clock.c); its
 * coarse form stops at the last tick. The raw clock is the timer's ticks
 * alone, which no slew changes (`rtc_raw_monotonic`). Where the clock runs
 * from the TSC (vdso.h), each is that clock's, which the vDSO reads too, so
 * that a call here and a reading there agree. The machine is never
 * suspended, which is why CLOCK_MONOTONIC and CLOCK_BOOTTIME read the same,
 * as Linux's do until a suspend moves offs_boot
 * (kernel/time/timekeeping.c:168-170). A CPU clock is the ticks the timer
 * charged. The seconds are 64 bits; the
 * 32-bit timespec of i386's clock_gettime cuts them, which ends in 2038 for
 * real time, as Linux's does.
 */
static void clock_read(const struct kclock *c, struct kernel_timespec64 *out)
{
    int64_t seconds;
    int64_t nanoseconds;
    uint32_t rest;

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
        rtc_coarse_monotonic(&seconds, &nanoseconds);
        out->tv_sec = seconds;
        out->tv_nsec = nanoseconds;
        break;
    case KCLOCK_MONO_RAW:
        rtc_raw_monotonic(&seconds, &nanoseconds);
        out->tv_sec = seconds;
        out->tv_nsec = nanoseconds;
        break;
    case KCLOCK_CPU:
    default:
        out->tv_sec = (int64_t)tick_div(clock_cpu_ticks(c), TICK_HZ, &rest);
        out->tv_nsec = rest * TICK_NS;
        break;
    }
}

/** A clock's resolution in nanoseconds: for real time and the clocks since
 * boot, the resolution of the timer's place within the tick, which they are
 * read to (`timer_resolution_ns`), and the tick for the rest. */
static int32_t clock_res_ns(const struct kclock *c)
{
    if (c->kind != KCLOCK_REAL && c->kind != KCLOCK_MONO && c->kind != KCLOCK_MONO_RAW) {
        return (int32_t)TICK_NS;
    }
    return (int32_t)(vclock_running() ? vclock_resolution_ns() : timer_resolution_ns());
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

    clock_call_count++;
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

    clock_call_count++;
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
 * call has checked its own clock and time. -EPERM for any other caller, and
 * -EINVAL for nanoseconds outside a second or a time the clock refuses,
 * before 1970, past Linux's TIME_SETTOD_SEC_MAX or before the monotonic
 * clock (`rtc_set_realtime`), as Linux answers them.
 */
static int32_t realtime_set(int64_t seconds, int64_t nanoseconds)
{
    if (!proc_privileged(current_proc)) {
        return -EPERM;
    }
    if (nanoseconds < 0 || nanoseconds > 999999999) {
        return -EINVAL;
    }
    return rtc_set_realtime(seconds, (int32_t)nanoseconds) == 0 ? 0 : -EINVAL;
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

/**
 * adjtimex's work on `tx`, read and then filled in: the modes checked as
 * Linux's timekeeping_validate_timex checks them (kernel/time/timekeeping.c:
 * 2392-2452), then ADJ_SETOFFSET's step (`rtc_shift_realtime`), then the
 * NTP state's modes and the clock's state (`rtc_adjtimex`), as Linux's
 * do_adjtimex runs them (kernel/time/timekeeping.c:2473-2532). A mode but
 * adjtime's read of what is left is a privileged caller's alone. The
 * offsets of ADJ_OFFSET and of adjtime are slewed, and only ADJ_SETOFFSET
 * steps the clock. Returns the clock's state, or -EPERM, or -EINVAL for an
 * adjtime call with other modes than its own, a tick outside 9000 to 11000
 * microseconds, a fraction of ADJ_SETOFFSET outside a second, a frequency
 * whose scaled value would overflow, or a step to a time the clock refuses.
 */
static int32_t ktimex_adjust(struct ktimex *tx)
{
    uint32_t m = tx->modes;
    int privileged = proc_privileged(current_proc);

    if (m & ADJ_ADJTIME) {
        if (!(m & ADJ_OFFSET)) {
            return -EINVAL;
        }
        if (!(m & ADJ_OFFSET_READONLY) && !privileged) {
            return -EPERM;
        }
    } else {
        if (m != 0 && !privileged) {
            return -EPERM;
        }
        if ((m & ADJ_TICK) && (tx->tick < NTP_TICK_MIN || tx->tick > NTP_TICK_MAX)) {
            return -EINVAL;
        }
    }
    if (m & ADJ_SETOFFSET) {
        if (!privileged) {
            return -EPERM;
        }
        if (tx->usec < 0 || tx->usec >= ((m & ADJ_NANO) ? 1000000000 : 1000000)) {
            return -EINVAL;
        }
    }
    if ((m & ADJ_FREQUENCY) && (tx->freq < -NTP_FREQ_LIMIT || tx->freq > NTP_FREQ_LIMIT)) {
        return -EINVAL;
    }
    if ((m & ADJ_SETOFFSET) &&
        rtc_shift_realtime(tx->sec, (int32_t)(tx->usec * ((m & ADJ_NANO) ? 1 : 1000))) != 0) {
        return -EINVAL;
    }
    return rtc_adjtimex(tx);
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
#define NTP_RTC_TICKS (11u * 60u * TICK_HZ)

/**
 * Copy the time to the CMOS clock after a change an NTP daemon made while
 * the clock is synchronized, its status `status` without STA_UNSYNC, at
 * most once in 11 minutes, as Linux's 11-minute mode does, so that the
 * next boot starts from the time the daemon kept and not from the CMOS
 * clock's own drift. Rounded to the nearest second, since the CMOS clock
 * keeps no fraction.
 */
static void ntp_rtc_sync(int32_t status)
{
    static uint64_t last; /* kernel_ticks + 1 at the last copy, 0 for none */
    int64_t sec, nsec;
    rtc_time_t t;

    if ((status & STA_UNSYNC) || (last != 0 && tick_count() + 1u - last < NTP_RTC_TICKS)) {
        return;
    }
    rtc_realtime(&sec, &nsec);
    if (nsec >= 500000000) {
        sec++;
    }
    if (sec < 946684800 || sec > 4102444799LL) {
        return; /* the CMOS clock's century: 2000 to 2099 */
    }
    rtc_time_of_seconds(sec, &t);
    if (rtc_write_clock(&t) == 0) {
        last = tick_count() + 1u;
    }
}

/* Whether a struct timex of longs, which adjtimex and clock_adjtime take,
 * has the 64-bit layout of clock_adjtime64's: so where a long has 64 bits,
 * as on x86-64, except for an i386 call there, whose timex is the 128-byte
 * one of 32-bit longs, as Linux's compat adjtimex reads it. */
#define TIMEX_LONGS_WIDE (sizeof(long) == 8 && !in_compat_syscall())

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
        ntp_rtc_sync(tx.status);
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
    /* the PPS fields, from ppsfreq to stbcnt, 0 as Linux without PPS gives
     * them (kernel/time/ntp.c:225-236) */
    k_memset(b + timex_off[wide][11] + (wide ? 8 : 4), 0,
             (size_t)(timex_off[wide][13] - timex_off[wide][11] - (wide ? 8 : 4)));
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
 * settimeofday takes timeval and timezone, each of which may be null, and
 * checks them in Linux's order (kernel/time/time.c:169-221): each copied,
 * -EFAULT, and the time's microseconds and seconds, -EINVAL; then the
 * privilege, -EPERM, even with neither; then the zone's minutes, -EINVAL
 * past 15 hours either way. A zone is then kept, for gettimeofday and the
 * vDSO's (`vclock_set_tz`), and the time set (`realtime_set`), which may
 * still refuse a time before the monotonic clock with the zone kept, as
 * Linux's do_settimeofday64 does after it. The first zone set at all, when
 * it comes alone and is not UTC, moves real time on by its minutes, as
 * Linux's timekeeping_warp_clock takes a machine's clock kept in local time
 * to UTC (kernel/time/timekeeping.c:1452-1462), which is how BusyBox's and
 * util-linux's hwclock tell the kernel of such a clock.
 */
long sys_settimeofday(isr_params_t *params)
{
    static int tz_set_before;
    const void __user *utv = (const void __user *)SC_ARG(params, 0);
    const void __user *utz = (const void __user *)SC_ARG(params, 1);
    struct kernel_timeval tv;
    struct kernel_timezone tz;
    int64_t sec = 0; /* as wide as the clock's limit on either kernel */

    if (utv) {
        if (copy_from_user(&tv, utv, sizeof(tv)) != 0) {
            return -EFAULT;
        }
        if (tv.tv_usec < 0 || tv.tv_usec > 999999) {
            return -EINVAL;
        }
        sec = tv.tv_sec;
    }
    if (utz && copy_from_user(&tz, utz, sizeof(tz)) != 0) {
        return -EFAULT;
    }
    if (sec < 0 || sec >= RTC_SETTOD_SEC_MAX) {
        return -EINVAL;
    }
    if (!proc_privileged(current_proc)) {
        return -EPERM;
    }
    if (utz) {
        if (tz.tz_minuteswest > 15 * 60 || tz.tz_minuteswest < -15 * 60) {
            return -EINVAL;
        }
        sys_tz.tz_minuteswest = tz.tz_minuteswest;
        sys_tz.tz_dsttime = tz.tz_dsttime;
        vclock_set_tz(tz.tz_minuteswest, tz.tz_dsttime);
        if (!tz_set_before) {
            tz_set_before = 1;
            if (!utv && tz.tz_minuteswest != 0) {
                rtc_shift_realtime((int64_t)tz.tz_minuteswest * 60, 0);
            }
        }
    }
    if (!utv) {
        return 0;
    }
    return realtime_set(sec, (int64_t)tv.tv_usec * 1000);
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
 * clock's true granularity: for real time and time since boot, that of the
 * timer's place within the tick, which they are read to, and the tick for every
 * other clock, the coarse clocks and the CPU clocks. Returns 0, -EINVAL for a
 * clock `clock_resolve` does not have, or -EFAULT.
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
 * Write the remainder `rem` of a sleep a signal cut short where the call
 * asked for it, which the thread's restart block keeps (`restart.urem`), as
 * Linux's restart_block keeps rmtp: two 64-bit words with `restart.wide`,
 * else two 32-bit words, so a 32-bit program's call on the x86-64 kernel
 * gets its own layout. Nothing for no address. Returns 0, or -EFAULT when
 * the remainder cannot be written.
 */
static int32_t sleep_rem_put(const struct kernel_timespec *rem)
{
    const struct restart_block *b = &current_proc->restart;
    void __user *u = (void __user *)b->urem;

    if (!b->urem) {
        return 0;
    }
    if (b->wide) {
        int64_t w[2];

        w[0] = rem->tv_sec;
        w[1] = rem->tv_nsec;
        return copy_to_user(u, w, sizeof(w)) != 0 ? -EFAULT : 0;
    } else {
        int32_t w[2];

        w[0] = (int32_t)rem->tv_sec;
        w[1] = (int32_t)rem->tv_nsec;
        return copy_to_user(u, w, sizeof(w)) != 0 ? -EFAULT : 0;
    }
}

/**
 * Begin a sleep call: nothing for restart_syscall to resume, as Linux's
 * nanosleep and clock_nanosleep clear it at their entry, and the remainder,
 * when a signal cuts the sleep short, goes to `urem`, 0 for none, as two
 * 64-bit words with `wide`, else two 32-bit words.
 */
static void sleep_begin(uintptr_t urem, int wide)
{
    current_proc->restart.fn = NULL;
    current_proc->restart.urem = urem;
    current_proc->restart.wide = (uint8_t)(wide != 0);
}

/**
 * End a sleep a signal cut short before its deadline, the tick `target`: the
 * process runs again, and with `rem` the time left goes where the call asked
 * (`sleep_rem_put`). The time left is to the sleep's end (`wakeup_ns`) by
 * the monotonic clock where the sleep would end then, by the interrupt
 * between ticks (`timer_wakes_between`), as Linux reports the time left to
 * its hrtimer's expiry (kernel/time/hrtimer.c:2106-2114); otherwise it is to
 * that tick, less how far into the tick in progress the timer is
 * (`rtc_tick_phase`). Returns -EINTR, or -EFAULT when the remainder cannot
 * be written, as Linux answers; 0 when a remainder was asked for and none
 * is left, by the clock or by the tick, as Linux's do_nanosleep returns 0
 * for a remainder of 0 or less (kernel/time/hrtimer.c:2107-2111). The
 * caller counts the sleep as cut short when it returns -EINTR or -EFAULT
 * (`sleep_cut_short`).
 */
static int32_t nanosleep_interrupted(int rem, uint64_t target)
{
    int64_t end = current_proc->wakeup_ns;
    struct kernel_timespec left;

    current_proc->wakeup_tick = 0;
    current_proc->state = PROC_RUNNING;
    if (!rem) {
        return -EINTR;
    }
    if (end != 0 && timer_wakes_between()) {
        int64_t to = end - rtc_monotonic_ns();
        uint32_t rest = 0;

        left.tv_sec = to > 0 ? (long)tick_div((uint64_t)to, 1000000000u, &rest) : 0;
        left.tv_nsec = to > 0 ? (long)rest : 0;
    } else {
        uint64_t now;
        uint32_t phase_ns, rest;
        int32_t ns;

        rtc_tick_phase(&now, &phase_ns);
        left.tv_sec = (long)tick_div(target > now ? target - now : 0u, TICK_HZ, &rest);
        ns = (int32_t)(rest * TICK_NS) - (int32_t)phase_ns;
        if (ns < 0 && left.tv_sec > 0) {
            left.tv_sec--;
            ns += 1000000000;
        }
        left.tv_nsec = ns > 0 ? ns : 0;
    }
    if (left.tv_sec == 0 && left.tv_nsec == 0 && current_proc->restart.urem != 0) {
        return 0;
    }
    return sleep_rem_put(&left) != 0 ? -EFAULT : -EINTR;
}

/**
 * Count the nanosleep that ends with `rc` as one a signal cut short when `rc`
 * is -EINTR, or -EFAULT, which a sleep returns only when a signal ended it
 * and its remainder could not be written; not when its remainder came to
 * none and it returns 0 (`process_count_sleep_cut_short`, /proc/stat's
 * sleeps_interrupted). Returns `rc`.
 */
static int32_t sleep_cut_short(int32_t rc)
{
    if (rc == -EINTR || rc == -EFAULT) {
        process_count_sleep_cut_short();
    }
    return rc;
}

/**
 * Whether the current sleep's time of the monotonic clock, `wakeup_ns`, has
 * come. When it has not, the sleep goes on the timer list now, before the
 * interrupt between ticks can look for it, and that interrupt is armed for
 * its time (`timer_wake_at`), however far it is, as Linux starts the
 * sleep's hrtimer when the sleep begins (kernel/time/hrtimer.c:2085-2093),
 * so that no pass of the timer needs to come before its time to arm it. The
 * clock decides, not the tick count, which runs behind it by up to half a
 * tick, and, while the boot processor runs with interrupts off, by the
 * ticks until another processor makes the clock's pass (`timer_pass_late`,
 * kernel.c). The process is marked blocked, and interrupts are off.
 */
static int sleep_time_come(void)
{
    int64_t want = current_proc->wakeup_ns, now;

    if (want == 0) {
        return 0;
    }
    now = rtc_monotonic_ns();
    if (now >= want) {
        return 1;
    }
    proc_timer_update(current_proc);
    timer_wake_at(want);
    return 0;
}

/* Policy: how long a sleep of zero lasts, Linux's default timer slack of a
 * thread that is not real-time, 50 us, to which a zero relative sleep's
 * timer may expire. */
#define SLEEP_SLACK_NS 50000

/**
 * `sec` seconds and `nsec` nanoseconds since boot as nanoseconds, for a
 * sleep's `wakeup_ns`; 0, a sleep that ends at its tick, for a time before
 * boot or one that does not fit, as Linux's ktime_set takes such a time as
 * KTIME_MAX (include/linux/ktime.h:36-41): one centuries away.
 */
static int64_t sleep_ns(int64_t sec, int64_t nsec)
{
    if (sec < 0 || sec >= 9223372036ll) {
        return 0;
    }
    return sec * 1000000000ll + nsec;
}

/**
 * Block until the tick `target`, or until a signal the process acts on.
 *
 * Marks the process PROC_BLOCKED on its wakeup_tick and yields; the timer
 * wakes it at the deadline while other processes run. The process is
 * marked blocked before the deadline and the pending signals are tested,
 * with interrupts off, so neither a signal nor the deadline's tick can come
 * between the test and the block unseen; the halt after a yield that ran
 * nothing is masked for the same reason.
 *
 * Once the deadline tick has come the sleep is over, and the call returns 0
 * whether or not a signal is pending. A signal the process acts on, found
 * pending before then, ends the sleep through `nanosleep_interrupted`, which
 * with `rem` writes the remainder and returns -EINTR, or -EFAULT when the
 * write fails; a wake by one it does not act on sends it back to sleep with
 * the deadline stored again.
 *
 * A sleep whose `rt_steps` the caller set, to an absolute time of real time,
 * returns 1 once the clock has been set since (`rtc_step_count`), for the
 * caller to count its tick again from the clock as set; the tick wakes it
 * for that (`proc_wake_sleepers`).
 *
 * A sleep whose `wakeup_ns` the caller set, the time of the monotonic clock
 * it ends at, ends at that time, by the interrupt between ticks
 * (`sleep_time_come`), as Linux's hrtimer ends it, and not at the tick.
 *
 * `wchan` names the sleep for /proc/PID/wchan as Linux's wchan names the
 * caller's: the first function on Linux's stack that is not `__sched`.
 */
static int32_t sleep_until(uint64_t target, int rem, const char *wchan)
{
    int32_t rc = 0;

    for (;;) {
        uint32_t flags = kirq_save();

        /* 0 means not sleeping, so a deadline of 0 is stored as 1. */
        current_proc->wakeup_tick = target != 0 ? target : 1u;
        current_proc->wchan = wchan;
        sched_set_current_state(PROC_BLOCKED);
        if (current_proc->rt_steps != 0 && current_proc->rt_steps != rtc_step_count()) {
            current_proc->state = PROC_RUNNING;
            kirq_restore(flags);
            rc = 1;
            break;
        }
        if (tick_count() >= target || sleep_time_come()) {
            current_proc->state = PROC_RUNNING;
            kirq_restore(flags);
            break;
        }
        if (signal_pending()) {
            current_proc->wakeup_tick = 0;
            current_proc->state = PROC_RUNNING;
            kirq_restore(flags);
            return nanosleep_interrupted(rem, target);
        }
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
    return rc;
}

/** A nonnegative interval as whole ticks, rounded up, so any nonzero
 * interval is a tick at least (`tick_span`). */
static uint64_t timespec_ticks(const struct kernel_timespec *req)
{
    return tick_span(req->tv_sec, (uint32_t)req->tv_nsec);
}

/** Whether `req` is a time a sleep takes: seconds not negative, and
 * nanoseconds from 0 through 999999999. */
static int timespec_valid(const struct kernel_timespec *req)
{
    return req->tv_sec >= 0 && req->tv_nsec >= 0 && req->tv_nsec < 1000000000;
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

/**
 * Sleep until real time or the monotonic clock, `kind`, reads `when` or
 * later.
 *
 * The monotonic clock at a tick is that tick's count in TICK_HZ parts of a
 * second, the tick count following the NTP state's slew as the clock does
 * (clock.c's `rtc_tick_advance`). A deadline of real time is the monotonic
 * time at which real time reaches it, real time less its offset
 * (`rtc_mono_of_real`), as Linux's hrtimer subtracts its base's whole
 * offset (kernel/time/hrtimer.c:922, 2140). So the sleep ends at the first
 * tick whose count reaches `when`, and a program that reads the clock then
 * reads `when` or later. Both are read within the tick, so a time the clock
 * has reached returns at once, before the deadline is rounded up to a tick.
 * The tick count and the uptime are read as one reading (`tick_now`), since
 * the tick changes both. A step of real time during a sleep on it, a setting
 * or a leap second, wakes the sleep, which counts its tick again from the
 * clock as it then is; the step count (`rt_steps`) is read before the clock,
 * so that a step between the two is not missed. A step forward past `when`
 * thus ends the sleep at the next tick, and a step back keeps it sleeping
 * until the clock reaches `when`, as Linux's clock_was_set retriggers the
 * hrtimer of an absolute sleep on CLOCK_REALTIME (kernel/time/hrtimer.c:
 * 951-987, 2133-2142). A sleep returns only once the clock reads `when`,
 * since Linux's hrtimer never expires before its clock reaches its time
 * (hrtimer.c:1814-1815): a sleep whose tick has come while the clock, which
 * the TSC's clock may keep up to a millisecond behind the tick
 * (arch/x86_64/vclock.c), still reads short of it, sleeps a tick more and
 * looks again. Returns 0, or -EINTR when a signal the process acts on comes
 * first. `wchan` names the sleep, as for `sleep_until`.
 */
static int32_t sleep_until_time(enum kclock_kind kind, const struct kernel_timespec *when,
                                const char *wchan)
{
    uint32_t seconds32, ticks32, steps;
    uint64_t now, base, due;
    int64_t sec;
    int32_t rc, nsec;

    do {
        steps = rtc_step_count();
        if (time_reached(kind, when)) {
            return 0;
        }
        tick_now(&seconds32, &ticks32, &base);
        now = (uint64_t)seconds32 * TICK_HZ + ticks32;
        sec = when->tv_sec;
        nsec = (int32_t)when->tv_nsec;
        if (kind == KCLOCK_REAL) {
            rtc_mono_of_real(when->tv_sec, (int32_t)when->tv_nsec, &sec, &nsec);
        }
        due = sec < 0 ? 0 : tick_span(sec, (uint32_t)nsec);
        if (due <= now) {
            due = now + 1u;
        }
        current_proc->rt_steps = kind == KCLOCK_REAL ? steps : 0;
        current_proc->wakeup_ns = sleep_ns(sec, nsec);
        rc = sleep_until(base + (due - now), 0, wchan);
        current_proc->wakeup_ns = 0;
        current_proc->rt_steps = 0;
    } while (rc == 0 || rc == 1);
    return rc;
}

static long sleep_resume(isr_params_t *params);

/**
 * Sleep to the tick `target` and then, unless `end` has no seconds and no
 * nanoseconds, until time since boot reaches `end`, the deadline
 * `sleep_for` fixed; the sleep to the tick ends at `end` when that comes
 * first (`wakeup_ns`). A signal's remainder is the time left to `end` where
 * the interrupt between ticks ends the sleep, otherwise to `target`, and
 * once that tick has come the time left until the clock reaches `end`. A
 * remainder of none, with a place for it, returns 0, as Linux's
 * do_nanosleep does for a remainder of 0 or less (kernel/time/hrtimer.c:
 * 2107-2111), and the sleep is counted as cut short only when it returns
 * -EINTR or -EFAULT (`sleep_cut_short`).
 *
 * When a signal cuts the sleep short, the deadline goes into the thread's
 * restart block and the call stands for Linux's ERESTART_RESTARTBLOCK: a
 * signal that runs no handler, as a stop and a continue, restarts it
 * through restart_syscall (`sleep_resume`), which sleeps on to this same
 * deadline, and a handler gives the caller EINTR and the remainder. `wchan`
 * names the sleep, as for `sleep_until`.
 */
static int32_t sleep_to(uint64_t target, const struct kernel_timespec *end, const char *wchan)
{
    struct restart_block *b = &current_proc->restart;
    int64_t sec, nsec;
    int32_t rc;

    if (end->tv_sec != 0 || end->tv_nsec != 0) {
        current_proc->wakeup_ns = sleep_ns(end->tv_sec, end->tv_nsec);
    }
    rc = sleep_until(target, 1, wchan);
    current_proc->wakeup_ns = 0;
    if (rc == 0 && (end->tv_sec != 0 || end->tv_nsec != 0)) {
        rc = sleep_until_time(KCLOCK_MONO, end, wchan);
        if (rc != 0) {
            struct kernel_timespec rem;

            rtc_monotonic(&sec, &nsec);
            rem.tv_sec = (long)(end->tv_sec - sec);
            rem.tv_nsec = end->tv_nsec - (long)nsec;
            if (rem.tv_nsec < 0) {
                rem.tv_sec--;
                rem.tv_nsec += 1000000000L;
            }
            if (rem.tv_sec < 0 || (rem.tv_sec == 0 && rem.tv_nsec == 0)) {
                if (b->urem != 0) {
                    return 0; /* none left: do_nanosleep's rem <= 0 */
                }
                rem.tv_sec = 0;
                rem.tv_nsec = 0;
            }
            if (sleep_rem_put(&rem) != 0) {
                return sleep_cut_short(-EFAULT);
            }
        }
    }
    if (sleep_cut_short(rc) == -EINTR) {
        b->fn = sleep_resume;
        b->tick = target;
        b->sec = end->tv_sec;
        b->nsec = end->tv_nsec;
        b->flags = 0;
        current_proc->restart_kind = RESTART_BLOCK;
    }
    return rc;
}

/**
 * Sleep for `req`, to the first tick at or after its end, so a sleep is never
 * shorter than asked and at most a tick longer, up to the clamp in
 * `timespec_ticks`. The end is measured from where in the tick in progress
 * the call is (`rtc_tick_phase`): counting that tick as a whole would make
 * every sleep of a tick or less last two, so that `usleep(1000)` would take
 * 2 ms.
 *
 * The sleep also lasts until time since boot, read at the call, has advanced
 * by `req`. A tick the timer lost while the processor could not take it
 * leaves the tick count behind that clock, which keeps its last reading
 * (`rtc_monotonic`), and a sleep counted in ticks alone would end before the
 * clock showed it had lasted `req`. A sleep that `tick_span` cuts ends at
 * its tick.
 *
 * A zero interval blocks for SLEEP_SLACK_NS and ends between ticks, as
 * Linux arms a zero relative sleep's timer to expire within its default
 * slack (kernel/time/hrtimer.c:2141, init/init_task.c:142), so a caller can
 * give a woken thread time to run; where there is no interrupt between
 * ticks, it lasts to the first tick after that time.
 *
 * A signal's remainder is the time left to the end where the interrupt
 * between ticks ends the sleep, and otherwise to the deadline tick, so a
 * caller that sleeps again for the remainder ends where the first sleep
 * would have, and later by the time it takes to call again; once that tick
 * has come, it is the time left until the clock reaches the end. A
 * remainder that cannot be written makes the call -EFAULT, and a sleep that
 * completes writes none. A signal that runs no handler resumes the sleep to
 * the same deadline (`sleep_to`).
 */
static int32_t sleep_for(const struct kernel_timespec *req)
{
    struct kernel_timespec end;
    uint64_t now, ticks;
    uint32_t phase_ns, us;
    int64_t sec, nsec, asked = req->tv_sec;

    end.tv_sec = 0;
    end.tv_nsec = 0;
    rtc_monotonic(&sec, &nsec);
    /* Linux's do_nanosleep is __sched, so wchan shows its caller. */
    if (req->tv_sec == 0 && req->tv_nsec == 0) {
        nsec += SLEEP_SLACK_NS;
        if (nsec >= 1000000000) {
            nsec -= 1000000000;
            sec++;
        }
        end.tv_sec = (long)sec;
        end.tv_nsec = (long)nsec;
        return sleep_to(tick_count() + 1u, &end, "hrtimer_nanosleep");
    }
    rtc_tick_phase(&now, &phase_ns);
    us = phase_ns / 1000u + ((uint32_t)req->tv_nsec + 999u) / 1000u;
    ticks = tick_span(req->tv_sec, 0) + (us + (TICK_US - 1u)) / TICK_US;
    if (asked < ((int64_t)1 << 40)) {
        end.tv_sec = (long)(sec + req->tv_sec);
        end.tv_nsec = (long)nsec + req->tv_nsec;
        if (end.tv_nsec >= 1000000000L) {
            end.tv_sec++;
            end.tv_nsec -= 1000000000L;
        }
    }
    return sleep_to(now + ticks, &end, "hrtimer_nanosleep");
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
 * relative sleep's remainder, with `rem`, is the ticks still due at the
 * last reading. Returns 0, -EINTR, or -EFAULT when the remainder cannot be
 * written.
 */
static int32_t sleep_until_cpu(int32_t clk, uint64_t due, int rem)
{
    uint64_t now = 0;

    for (;;) {
        struct kclock c;
        uint64_t wait = (uint64_t)1 << 62;
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
        rc = sleep_until(tick_count() + wait, 0, "do_cpu_nanosleep");
        if (rc != 0) {
            if (rem) {
                struct kernel_timespec left;
                uint32_t rest;

                left.tv_sec = (long)tick_div(due - now, TICK_HZ, &rest);
                left.tv_nsec = (long)(rest * TICK_NS);
                if (sleep_rem_put(&left) != 0) {
                    return -EFAULT;
                }
            }
            return rc;
        }
    }
}

/**
 * Sleep on the CPU clock `clk` until it reads `due` ticks, a relative
 * sleep's end, as `sleep_until_cpu` does, writing the remainder. A signal
 * that cuts it short leaves the end in the restart block, and one that runs
 * no handler resumes the sleep to it (`sleep_resume`), as Linux's
 * posix_cpu_nsleep_restart does.
 */
static int32_t sleep_cpu_for(int32_t clk, uint64_t due)
{
    struct restart_block *b = &current_proc->restart;
    int32_t rc = sleep_cut_short(sleep_until_cpu(clk, due, 1));

    if (rc == -EINTR) {
        b->fn = sleep_resume;
        b->tick = due;
        b->flags = clk;
        b->sec = -1;
        current_proc->restart_kind = RESTART_BLOCK;
    }
    return rc;
}

/**
 * The name /proc/PID/wchan gives a sleep that restart_syscall resumed: the
 * first of Linux's frames under it that is not `__sched`, which
 * hrtimer_nanosleep_restart and do_nanosleep are. Linux's x86-64 kernel, built
 * for ORC without frame pointers, reaches hrtimer_nanosleep_restart by tail
 * calls from its entry, `do_syscall_64` for a call of its own ABI and
 * `do_int80_emulation` for an i386 call by int 0x80, which Debian's 6.12.115
 * shows. The native name goes by the kernel's architecture, with the i386
 * ABI built or not, since Linux's native entry is the same without
 * CONFIG_IA32_EMULATION. Its i386 kernel keeps frame pointers and so every
 * frame, and the first is `__do_sys_restart_syscall`, which
 * `__ia32_sys_restart_syscall` aliases and kallsyms lists first.
 */
static const char *sleep_resume_wchan(void)
{
#if defined(__x86_64__)
    return in_compat_syscall() ? "do_int80_emulation" : "do_syscall_64";
#else
    return "__do_sys_restart_syscall";
#endif
}

/**
 * The resumption of a relative sleep a signal that ran no handler cut short
 * (restart_syscall): on to the deadline the restart block keeps, with the
 * remainder where the first call asked for it, and the block kept for a
 * further signal, as Linux's hrtimer_nanosleep_restart keeps it. A CPU
 * clock's sleep is marked by seconds of -1.
 */
static long sleep_resume(isr_params_t *params)
{
    struct restart_block *b = &current_proc->restart;
    struct kernel_timespec end;

    (void)params;
    if (b->sec == -1) {
        return sleep_cpu_for(b->flags, b->tick);
    }
    end.tv_sec = (long)b->sec;
    end.tv_nsec = (long)b->nsec;
    return sleep_to(b->tick, &end, sleep_resume_wchan());
}

long nanosleep_core(const struct kernel_timespec *req, uintptr_t urem, int wide)
{
    if (!current_proc || !req) {
        return -EFAULT;
    }
    if (!timespec_valid(req)) {
        return -EINVAL;
    }
    sleep_begin(urem, wide);
    return sleep_for(req);
}

/**
 * Block-and-schedule sleep (nanosleep, 32-bit timespec).
 *
 * Its arguments are const struct timespec* req and struct timespec* rem
 * (optional). Sleeps to the first tick at or after the interval's end through
 * `sleep_for`, which has the rules of the deadline, the signals and the
 * remainder: rem is written only when a signal cuts the sleep short, as
 * Linux writes it, and a rem that cannot be written is then -EFAULT.
 */
long sys_nanosleep(isr_params_t *params)
{
    const struct kernel_timespec __user *ureq =
        (const struct kernel_timespec __user *)SC_ARG(params, 0);
    struct kernel_timespec req;
    int ok = ureq && copy_from_user(&req, ureq, sizeof(req)) == 0;

    return nanosleep_core(ok ? &req : NULL, SC_ARG(params, 1), sizeof(long) > 4u);
}

long clock_nanosleep_core(int32_t clk, unsigned long flags, const struct kernel_timespec *req,
                          uintptr_t urem, int wide)
{
    int absolute = (flags & TIMER_ABSTIME) != 0;
    struct kclock c;
    int32_t rc;

    if (clk > CLOCK_BOOTTIME) {
        return -EINVAL;
    }
    if (clk == CLOCK_THREAD_CPUTIME_ID || clk == CLOCK_MONOTONIC_RAW ||
        clk == CLOCK_REALTIME_COARSE || clk == CLOCK_MONOTONIC_COARSE) {
        return -EOPNOTSUPP;
    }
    if (!current_proc || !req) {
        return -EFAULT;
    }
    if (!timespec_valid(req)) {
        return -EINVAL;
    }
    if (clk < 0 && ((uint32_t)clk & CPUCLOCK_PERTHREAD)) {
        return -EINVAL;
    }
    rc = clock_resolve(clk, &c);
    if (rc != 0) {
        return rc;
    }
    sleep_begin(absolute ? 0 : urem, wide);
    if (c.kind == KCLOCK_CPU) {
        uint64_t due = timespec_ticks(req);

        if (absolute) {
            rc = sleep_until_cpu(clk, due, 0);
        } else {
            return sleep_cpu_for(clk, due + clock_cpu_ticks(&c));
        }
    } else if (!absolute) {
        return sleep_for(req);
    } else {
        rc = sleep_until_time(c.kind, req, "hrtimer_nanosleep");
    }
    if (sleep_cut_short(rc) == -EINTR) {
        /* An absolute sleep restarts with its own arguments. */
        current_proc->restart_kind = RESTART_NOHAND;
    }
    return rc;
}

/**
 * Sleep against a chosen clock (clock_nanosleep, 32-bit
 * timespec).
 *
 * Its arguments are clockid, flags, const struct timespec *req and struct
 * timespec *rem. Flags 0 makes `req` an interval, which sleeps as `nanosleep`
 * does; TIMER_ABSTIME makes it a time of the clock, reached as
 * `sleep_until_time` or
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
 *
 * A signal that runs no handler resumes an interval to its first deadline
 * (`sleep_to`, `sleep_cpu_for`) and restarts an absolute sleep, as Linux
 * does; a handler gives EINTR either way.
 */
long sys_clock_nanosleep(isr_params_t *params)
{
    const struct kernel_timespec __user *ureq =
        (const struct kernel_timespec __user *)SC_ARG(params, 2);
    struct kernel_timespec req;
    int ok = ureq && copy_from_user(&req, ureq, sizeof(req)) == 0;

    return clock_nanosleep_core((int32_t)SC_ARG(params, 0), SC_ARG(params, 1), ok ? &req : NULL,
                                SC_ARG(params, 3), sizeof(long) > 4u);
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

/** Convert a validated timeval to nanoseconds, saturating at ktime's
 * signed limit, as Linux's timeval_to_ktime does. */
static uint64_t itimer_ns(long sec, long usec)
{
    uint64_t top = (uint64_t)0x7FFFFFFFFFFFFFFFull;
    uint64_t sub = (uint64_t)usec * 1000u;

    if ((uint64_t)sec > tick_div64(top - sub, 1000000000u, NULL)) {
        return top;
    }
    return (uint64_t)sec * 1000000000u + sub;
}

/** Store a nanosecond span as a timeval, truncating below a microsecond. */
static void itimer_put_ns(uint64_t ns, long *sec, long *usec)
{
    uint64_t rem;
    uint64_t seconds = tick_div64(ns, 1000000000u, &rem);

    if (seconds > (uint64_t)(~0UL >> 1)) {
        *sec = (long)(~0UL >> 1);
        *usec = 999999;
    } else {
        *sec = (long)seconds;
        *usec = (long)tick_div64(rem, 1000u, NULL);
    }
}

/** What the current process's timer `which` is set to, as setitimer and
 * getitimer report it. Interrupts are off. */
static void itimer_read(int32_t which, struct kernel_itimerval *out)
{
    const struct tgroup *tg = current_proc->tg;

    if (which == ITIMER_REAL) {
        uint64_t interval;
        uint64_t left = proc_alarm_read_ns(tg->leader, &interval);

        itimer_put_ns(left, &out->value_sec, &out->value_usec);
        itimer_put_ns(interval, &out->interval_sec, &out->interval_usec);
    } else {
        struct tgroup *g = current_proc->tg;
        uint64_t left, interval;

        spin_lock(&g->cpu_lock);
        left = g->cpu_left[which - 1];
        interval = g->cpu_interval[which - 1];
        spin_unlock(&g->cpu_lock);
        if (left != 0 && left < 1000u) {
            left = 1000u; /* armed: never reported as disarmed */
        }
        itimer_put_ns(left, &out->value_sec, &out->value_usec);
        itimer_put_ns(interval, &out->interval_sec, &out->interval_usec);
    }
}

/**
 * Arm or disarm timer `which`, after `value` nanoseconds and then each
 * `interval`, or disarm it for zero value. Real time has its own event;
 * processor time is counted down by the ticks charged to the process's
 * threads (`cpu_timer_tick`).
 * The caller has interrupts off and holds the big lock.
 */
static void itimer_set(int32_t which, uint64_t value, uint64_t interval)
{
    struct tgroup *tg = current_proc->tg;
    process_t *p = tg->leader;

    if (which != ITIMER_REAL) {
        spin_lock(&tg->cpu_lock);
        tg->cpu_left[which - 1] = value;
        tg->cpu_interval[which - 1] = value != 0 ? interval : 0;
        spin_unlock(&tg->cpu_lock);
        return;
    }
    proc_alarm_set_ns(p, value, interval);
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
 * otherwise the first signal is due that long from now, then every interval,
 * or only once for an interval of zero. Processor time is charged by the
 * tick, so its signal comes at the first tick that reaches the time. `old`
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
    itimer_set(which, itimer_ns(set.value_sec, set.value_usec),
               itimer_ns(set.interval_sec, set.interval_usec));
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
    uint64_t rest, interval;
    uint64_t left = proc_alarm_read_ns(current_proc->tg->leader, &interval);
    uint32_t seconds = (uint32_t)tick_div64(left, 1000000000u, &rest);

    itimer_set(ITIMER_REAL, (uint64_t)(uint32_t)SC_ARG(params, 0) * 1000000000u, 0);
    kirq_restore(flags);
    if ((seconds == 0 && rest != 0) || rest >= 500000000u) {
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

/** Whether a thread, process group or session still holds the kernel PID
 * `pid`, zombies included, as Linux's find_vpid finds a number until its
 * last task detaches. A terminal's foreground group alone does not hold
 * the number in that lookup. The caller holds the big kernel lock. */
static int tty_pid_exists(uint32_t pid)
{
    const process_t *p;

    if (!pid) {
        return 0;
    }
    for (p = proc_list; p; p = p->next) {
        if (p->pid == pid || p->tgid == pid || p->pgid == pid || p->sid == pid) {
            return 1;
        }
    }
    return 0;
}

/** The session of a group, or of the thread whose PID is `pid` if no group
 * has that number, as Linux's session_of_pgrp permits for TIOCSPGRP.
 * Returns zero when neither exists. The caller holds the big kernel lock. */
static uint32_t tty_pgrp_session(uint32_t pid)
{
    const process_t *p;

    for (p = proc_list; p; p = p->next) {
        if (p->pgid == pid) {
            return p->sid;
        }
    }
    p = get_process(pid);
    return p ? p->sid : 0;
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
    case TCXONC:
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
 * FIONBIO: set O_NONBLOCK on the open file description of `entry` when the
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
    fd_ofd_status_set(entry->ofd, OFD_NONBLOCK, on ? OFD_NONBLOCK : 0u);
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

int fd_tty_hung(fd_entry_t *e)
{
    struct tty *t = fd_tty(e);

    return t && e->ofd && tty_desc_hung(t, e->ofd->tty_hangups);
}

/**
 * TCFLSH on the console or a pty slave: `which` 0 (TCIFLUSH) discards the
 * unread input, 1 (TCOFLUSH) the output, 2 (TCIOFLUSH) both; anything else is
 * -EINVAL. The console writes its output at once, so it has none to discard;
 * a slave's `flush` discards the output its master has not read and tells a
 * master in packet mode of each flush (`TTY_FLUSH_*`), as Linux's
 * tty_perform_flush does through n_tty_flush_buffer and pty_flush_buffer.
 */
static int32_t tty_flush_at(struct tty *t, uintptr_t which)
{
    if (which > 2) {
        return -EINVAL;
    }
    if (which != 1) {
        tty_flush_input(t);
    }
    if (t->flush) {
        t->flush(t, (which != 1 ? TTY_FLUSH_IN : 0) | (which != 0 ? TTY_FLUSH_OUT : 0));
    }
    return 0;
}

/**
 * The job-control check (`tty_job_check`, SIGTTOU) of a terminal request that
 * changes `t`, as Linux's tty_check_change: TCSETS and its kin, TCFLSH,
 * TCXONC and TIOCSPGRP, whose EIO for an orphaned group Linux's tiocspgrp
 * turns into ENOTTY. Returns 0 for any other request.
 */
static int32_t tty_job_gate(struct tty *t, uint32_t request)
{
    int rc;

    switch (request) {
    case TCSETS:
    case TCSETSW:
    case TCSETSF:
    case TCSETS2:
    case TCSETSW2:
    case TCSETSF2:
    case TCFLSH:
    case TCXONC:
    case TIOCSPGRP:
        rc = tty_job_check(t, SIGTTOU);
        return request == TIOCSPGRP && rc == -EIO ? -ENOTTY : rc;
    default:
        return 0;
    }
}

/** TIOCSWINSZ: read the size at the user's `arg` whole and only then make
 * it `*ws`, as Linux's tiocswinsz copies before it changes the size; 0, or
 * -EFAULT with `*ws` as it was. */
static int32_t tty_set_winsize(struct tty_winsize *ws, uintptr_t arg)
{
    struct tty_winsize in;

    if (!arg || copy_from_user(&in, (const void __user *)arg, sizeof(in)) != 0) {
        return -EFAULT;
    }
    *ws = in;
    return 0;
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
 * just arrived is counted. TCFLSH discards the queues (`tty_flush_at`), and
 * TCXONC stops and restarts output or sends VSTOP and VSTART (`tty_tcflow`).
 *
 * A process of a background group of the session the terminal controls is
 * stopped with SIGTTOU before TCSETS and its kin, TCFLSH, TCXONC and
 * TIOCSPGRP, unless it blocks or ignores the signal (`tty_job_check`), as
 * Linux's tty_check_change; an orphaned group's call is EIO, and its
 * TIOCSPGRP ENOTTY, as Linux's tiocspgrp turns that EIO.
 *
 * TIOCSPGRP needs the caller's session to be the one the terminal controls,
 * and a group in that session, or a thread in it when the number names no
 * group (`tty_pgrp_session`, -EPERM). A negative number is EINVAL; one no
 * thread, group or session holds is ESRCH. TIOCSCTTY lets a session leader
 * take a terminal no other session controls, with the caller's group in the
 * foreground; `tty_make_sole_ctty` publishes the session and the group and
 * drops every other terminal the session controlled, the console and any
 * other pty, in one step with interrupts off, so the console's drain in
 * interrupt context never sees a torn or doubled controlling terminal, and
 * /dev/tty resolves to `t`. TIOCNOTTY gives the terminal up when the caller's
 * session controls it.
 */
static int32_t tty_common_ioctl(struct tty *t, uint32_t request, uintptr_t arg, int *handled)
{
    int32_t rc = tty_job_gate(t, request);

    *handled = 1;
    if (rc != 0) {
        return rc;
    }
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
        return tty_flush_at(t, arg);

    case TCXONC:
        return arg > TTY_TCION ? -EINVAL : tty_tcflow(t, (int)arg);

    case TIOCGWINSZ:
        if (!arg) {
            return -EFAULT;
        }
        if (copy_to_user((void __user *)arg, &t->winsize, sizeof(t->winsize)) != 0) {
            return -EFAULT;
        }
        return 0;

    case TIOCSWINSZ:
        return tty_set_winsize(&t->winsize, arg);

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
        if (pgrp < 0) {
            return -EINVAL;
        }
        if (t->sid == 0 || (int)current_proc->sid != t->sid) {
            return -ENOTTY;
        }
        pgrp = (int)pid_named((uint32_t)pgrp);
        if (!tty_pid_exists((uint32_t)pgrp)) {
            return -ESRCH;
        }
        if (tty_pgrp_session((uint32_t)pgrp) != (uint32_t)t->sid) {
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
 * TIOCLINUX, the Linux console's requests, whose subcode is the byte at
 * `arg`: TIOCL_BLANKSCREEN blanks the console until TIOCL_UNBLANKSCREEN,
 * which a key does not undo, TIOCL_UNBLANKSCREEN unblanks it, and
 * TIOCL_BLANKEDSCREEN returns 1 while it is blank and 0 otherwise, as
 * Linux's first console would (display/blank.h). The caller's session must
 * control the console, or the caller be privileged, as on Linux (-EPERM).
 * Another subcode is -EINVAL.
 */
static int32_t console_tioclinux(uintptr_t arg)
{
    unsigned char sub;

    if (copy_from_user(&sub, (const void __user *)arg, 1) != 0) {
        return -EFAULT;
    }
    if ((console_tty.sid == 0 || console_tty.sid != (int)current_proc->sid) &&
        !proc_privileged(current_proc)) {
        return -EPERM;
    }
    switch (sub) {
    case TIOCL_BLANKSCREEN:
        console_blank_now();
        return 0;
    case TIOCL_UNBLANKSCREEN:
        console_unblank_now();
        return 0;
    case TIOCL_BLANKEDSCREEN:
        return console_is_blank();
    default:
        return -EINVAL;
    }
}

/**
 * Answer the console's requests: TIOCLINUX (`console_tioclinux`), KIOCSOUND
 * and KDMKTONE on the PC speaker in Linux's encodings (`speaker_tone`),
 * Linux's KD* keyboard requests (`kd_ioctl`), and the AUDIO_* requests on
 * the active driver. The AUDIO_* requests other than AUDIO_GET_INFO are
 * -ENODEV with no driver, and AUDIO_START and AUDIO_WRITE return the
 * driver's -errno. AUDIO_WRITE checks the caller's samples with
 * `user_buffer_ok`, and the driver reads them in place. Any other request is
 * -ENOTTY.
 */
static int32_t console_ioctl(uint32_t request, uintptr_t arg)
{
    int32_t rc;

    if (request == TIOCLINUX) {
        return console_tioclinux(arg);
    }
    if (request == KIOCSOUND) {
        speaker_tone((uint32_t)arg, 0);
        return 0;
    }
    if (request == KDMKTONE) {
        /* A length of 0 silences, as Linux's does. */
        if ((arg >> 16) & 0xFFFF) {
            speaker_tone((uint32_t)(arg & 0xFFFF), (uint32_t)((arg >> 16) & 0xFFFF));
        } else {
            speaker_tone(0, 0);
        }
        return 0;
    }
    rc = kd_ioctl(request, arg);
    if (rc != -ENOTTY) {
        return rc;
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
        if (!audio) {
            return -ENODEV;
        }
        if (!arg) {
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
        if (!audio) {
            return -ENODEV;
        }
        if (!arg) {
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
    return -ENOTTY;
}

/**
 * vhangup(2): hang up the caller's controlling terminal as Linux's
 * `tty_vhangup_self` does (`tty_vhangup`), which needs CAP_SYS_TTY_CONFIG
 * in the first user namespace. 0, also for a caller with no controlling
 * terminal, or -EPERM. The caller's own descriptors of the terminal are hung
 * up with the rest, so a getty or a login opens it again.
 */
long sys_vhangup(isr_params_t *params)
{
    struct tty *t;

    (void)params;
    if (!current_proc) {
        return -ESRCH;
    }
    if (current_proc->userns || !cap_holds(current_proc, CAP_SYS_TTY_CONFIG)) {
        return -EPERM;
    }
    t = tty_session_tty((int)current_proc->sid);
    if (t) {
        tty_vhangup(t);
    }
    return 0;
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
 * request, as on Linux. The requests then go, in order, to: FIOCLEX and
 * FIONCLEX, which set and clear the descriptor's close-on-exec flag, and
 * FIONBIO (`fd_ioctl_nbio`), which every descriptor answers, as Linux's
 * `do_vfs_ioctl` (fs/ioctl.c:809-817); a hung-up terminal description,
 * which answers -EIO, or -ENOTTY to TIOCSPGRP; FS_IOC_GETFLAGS and the other
 * file attribute requests (`fs_flags_ioctl`); FICLONE and FICLONERANGE
 * (`fs_clone_ioctl`); SIOCGSTAMP and SIOCGSTAMPNS on an Internet or packet
 * socket (`socket_gettstamp`); the interface requests of
 * `netif_ioctl` on a socket of any family, as on Linux, where glibc's
 * if_nametoindex uses an AF_UNIX one and a DHCP client sets the machine's
 * address; the termios and session requests of `tty_common_ioctl` on the
 * console and a pty slave, ahead of the slave's own operations; and the
 * `ioctl` of an object with operations of its own, such as a pipe or a pty
 * master, -ENOTTY when it has none; the framebuffer's operations answer
 * its FB_* requests (display/fbdev.c). The console answers its sound
 * requests (`console_ioctl`), by the descriptor's type, so a descriptor 1
 * reopened onto a regular file never reaches the speaker. `ioctl_fd` is the
 * call with its arguments taken out of the frame.
 *
 * A type whose ioctl runs without the big lock (`file_ioctl_unlocked`) has
 * every request but those three go to its own `ioctl` without it, as
 * Linux's `unlocked_ioctl`, but for FICLONE and FICLONERANGE and a Unix
 * socket's interface requests (`ioctl_unlocked`); the type answers the
 * rest of the list itself, if it is a terminal. Every other type's
 * requests run under the big lock.
 */
long sys_ioctl(isr_params_t *params)
{
    return ioctl_fd((int)SC_ARG(params, 0), (uint32_t)SC_ARG(params, 1), SC_ARG(params, 2));
}

/** The requests of `ioctl_fd` after FIOCLEX, FIONCLEX and FIONBIO, on
 * `entry`, an open descriptor of the caller's, under the big lock. */
static long ioctl_locked(fd_entry_t *entry, uint32_t request, uintptr_t arg)
{
    if (fd_tty_hung(entry)) {
        return request == TIOCSPGRP ? -ENOTTY : -EIO;
    }
    {
        int handled;
        int32_t r = fs_flags_ioctl(entry, request, arg, &handled);

        if (!handled) {
            r = fs_clone_ioctl(entry, request, arg, &handled);
        }
        if (handled) {
            return r;
        }
    }

    if (entry->type == FD_TYPE_SOCKET && entry->sock) {
        int handled;
        int32_t r = socket_gettstamp(entry->sock, request, arg, &handled);

        if (handled) {
            return r;
        }
    }
    if ((entry->type == FD_TYPE_SOCKET && entry->sock) || entry->type == FD_TYPE_UNIX) {
        int handled;
        int32_t r = netif_ioctl(
            request, arg, entry->type == FD_TYPE_SOCKET ? (int)entry->sock->family : 1, &handled);

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

    if (entry->type == FD_TYPE_CONSOLE) {
        return console_ioctl(request, arg);
    }

    return -ENOTTY;
}

/**
 * The requests of `ioctl_fd` after FIOCLEX, FIONCLEX and FIONBIO on
 * `entry`, whose type's ioctl runs without the big lock
 * (`file_ioctl_unlocked`), in Linux's order: FICLONE and FICLONERANGE,
 * which `do_vfs_ioctl` answers for a descriptor of any type
 * (fs/ioctl.c); the type's own ioctl; and for a Unix socket a
 * request that ioctl does not answer goes to the interface requests
 * (`netif_ioctl`), as `sock_do_ioctl` passes it to `dev_ioctl`
 * (net/socket.c), which musl's if_nametoindex reaches through
 * an AF_UNIX socket. The first and the last take the big lock.
 */
static long ioctl_unlocked(fd_entry_t *entry, uint32_t request, uintptr_t arg)
{
    int handled = 0;
    long rc;

    if (fs_clone_request(request)) {
        bkl_enter();
        rc = fs_clone_ioctl(entry, request, arg, &handled);
        bkl_exit();
        return rc;
    }
    rc = entry->ops->ioctl ? entry->ops->ioctl(entry, request, arg) : -ENOTTY;
    if (rc == -ENOTTY && entry->type == FD_TYPE_UNIX) {
        bkl_enter();
        rc = netif_ioctl(request, arg, 1, &handled);
        bkl_exit();
        if (!handled) {
            rc = -ENOTTY;
        }
    }
    return rc;
}

long ioctl_fd(int fd_num, uint32_t request, uintptr_t arg)
{
    fd_entry_t *entry;
    long rc;

    if (!current_proc) {
        return -ESRCH;
    }

    entry = fd_get(current_proc->fds, fd_num);
    if (!entry || (entry->flags & FD_FLAG_PATH)) {
        return -EBADF;
    }
    if (request == FIOCLEX || request == FIONCLEX) {
        fd_set_flags(current_proc->fds, entry, request == FIONCLEX ? FD_FLAG_CLOEXEC : 0,
                     request == FIOCLEX ? FD_FLAG_CLOEXEC : 0);
        return 0;
    }
    if (request != FIONBIO && file_ioctl_unlocked(entry)) {
        return ioctl_unlocked(entry, request, arg);
    }
    bkl_enter();
    rc = request == FIONBIO ? fd_ioctl_nbio(entry, arg) : ioctl_locked(entry, request, arg);
    bkl_exit();
    return rc;
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
 * has reaped, in the `clock_t` of USER_HZ, Linux's CLK_TCK, which the
 * structure uses whatever the tick's rate. They are read with interrupts
 * masked, since the tick adds to them. The return value is the tick count in
 * the same units, cut to a long.
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
        uint64_t user;
        uint64_t sys;

        proc_cpu_time(current_proc, &user, &sys);
        t.tms_utime = (long)TICKS_TO_CLOCK(user);
        t.tms_stime = (long)TICKS_TO_CLOCK(sys);
        t.tms_cutime = (long)TICKS_TO_CLOCK(current_proc->tg->cutime);
        t.tms_cstime = (long)TICKS_TO_CLOCK(current_proc->tg->cstime);
        if (copy_to_user((void __user *)SC_ARG(params, 0), &t, sizeof(t)) != 0) {
            return -EFAULT;
        }
    }
    return (long)TICKS_TO_CLOCK(tick_count());
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
    u.si.uptime = (long)tick_div(tick_count(), TICK_HZ, NULL);
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
            if (sleep_until(tick_count() + MS_TO_TICKS(100), 0, "syslog_print") != 0) {
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

/* The execution domain of a 32-bit x86 program, personality(2)'s low byte,
 * under which the x86-64 kernel's uname reports i686, as Linux's
 * COMPAT_UTS_MACHINE; `setarch i686` and `linux32` set it. */
#define PER_LINUX32 0x8u

/**
 * uname(2): fill the struct utsname at its argument with the fixed fields of
 * syscall.h, the host and domain names of the caller's UTS namespace and the
 * architecture's name, which on x86-64 is i686 for a process whose
 * execution domain is PER_LINUX32, whatever its code. Returns 0 or -EFAULT.
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
    if (sizeof(long) > 4u && (current_proc->persona & 0xFFu) == PER_LINUX32) {
        uname_field(uts.machine, "i686");
    }
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

/** Whether an ioctl, with the arguments in `params`, that failed with
 * EINTR may be restarted: not one of KVM's (type 0xAE), whose EINTR Linux
 * gives the caller as it is, as KVM_RUN's after a signal or immediate_exit,
 * which QEMU waits for. */
static int ioctl_restartable(const isr_params_t *params)
{
    return (((uint32_t)SC_ARG(params, 1) >> 8) & 0xFFu) != 0xAEu;
}

/**
 * Whether the call the handler `fn` answers, with the arguments in `params`,
 * which a signal interrupted with EINTR and which set no restart kind of
 * its own, stands for Linux's ERESTARTSYS, which a signal no handler takes
 * or a handler with SA_RESTART restarts. The calls that do not are the ones
 * Linux gives EINTR in every case: those that wait for a signal or for
 * events, rt_sigtimedwait and the epoll waits, the System V semaphore
 * waits, those through ipc(2) among them (`sysv_ipc_restartable`), an ioctl
 * of KVM's, by either ABI (`ioctl_restartable`), restart_syscall with
 * nothing to resume, and the two returns from a handler, whose result
 * register is the interrupted call's. The call is told by its handler,
 * since its number differs between the architectures; an i386 call on the
 * x86-64 kernel by its compat handler, which `ia32_never_restarts` judges.
 * The calls Linux restarts otherwise, the sleeps, pause, the sigsuspends,
 * the poll and select calls, the System V queue waits and the futex waits,
 * set their kind (`restart_kind`).
 */
static int syscall_restartable(syscall_fn_t fn, const isr_params_t *params)
{
    if (fn == sys_ioctl) {
        return ioctl_restartable(params);
    }
#if defined(IA32_SYSCALL_TABLE_SIZE)
    if (fn == compat_sys_ioctl) {
        return ioctl_restartable(params);
    }
#endif
    if (fn == sys_ipc) {
        return sysv_ipc_restartable(params);
    }
#if defined(IA32_SYSCALL_TABLE_SIZE)
    if (ia32_never_restarts(fn, params)) {
        return 0;
    }
#endif
    return fn != sys_sigreturn && fn != sys_rt_sigreturn && fn != sys_restart_syscall &&
           fn != sys_rt_sigtimedwait && fn != sys_epoll_wait && fn != sys_epoll_pwait &&
           fn != sys_epoll_pwait2 && fn != sys_semop && fn != sys_semtimedop_time64;
}

/** The code of Linux's for the restart kind `kind` (`restart_kind`), as a
 * tracer reads it in the result register: -ERESTARTSYS and the rest, or
 * -EINTR for RESTART_NONE. */
static long restart_code(int kind)
{
    switch (kind) {
    case RESTART_SYS:
        return -ERESTARTSYS;
    case RESTART_NOINTR:
        return -ERESTARTNOINTR;
    case RESTART_NOHAND:
        return -ERESTARTNOHAND;
    case RESTART_BLOCK:
        return -ERESTART_RESTARTBLOCK;
    default:
        return -EINTR;
    }
}

/**
 * restart_syscall(2): resume the call the thread's restart block holds,
 * which `deliver_signals` makes after a signal that ran no handler cut short
 * a call Linux restarts through its block (RESTART_BLOCK). With nothing to
 * resume it returns -EINTR, as Linux's do_no_restart_syscall does; a handler
 * clears the block when it returns (`signal_return`), and each resuming
 * handler that Linux's clears clears it.
 */
long sys_restart_syscall(isr_params_t *params)
{
    long (*fn)(struct isr_params *);

    if (!current_proc || !current_proc->restart.fn) {
        return -EINTR;
    }
    fn = current_proc->restart.fn;
    return fn(params);
}

/**
 * Translate the -EINTR of handler `fn` into Linux's restart code, as the
 * kind the handler set says, or as `syscall_restartable` says for a handler
 * that set none, and mark the thread (`restart_due`) when it makes one.
 * Keep other results, and those of a call the dispatcher did not run (a
 * NULL `fn`). Signal delivery decides whether to restart from the registers
 * after the tracer's stops.
 */
static long restart_result(const isr_params_t *params, syscall_fn_t fn, long ret)
{
    if (ret != -EINTR || !fn) {
        return ret;
    }
    if (current_proc->restart_kind == RESTART_SYS && !syscall_restartable(fn, params)) {
        current_proc->restart_kind = RESTART_NONE;
    }
    current_proc->restart_due = current_proc->restart_kind != RESTART_NONE;
    if (current_proc->restart_due) {
        proc_work_set(current_proc, WORK_SIGNAL);
    }
    return restart_code(current_proc->restart_kind);
}

/**
 * Report a call this kernel does not have, when `rach2.trace=enosys` is on
 * the command line: a missing call is otherwise invisible, -ENOSYS going back
 * in a register with nothing printed, so a program that fails for want of one
 * fails for no stated reason. A number in the table is reported unless the
 * process that last reported it makes it again; the numbers past the table
 * share one entry, so one of them is reported unless it and its process are
 * the last reported. A program that makes one missing call in a loop prints
 * one line, but two processes alternating on one number, or one process
 * alternating between two numbers past the table, print a line each time.
 * An i386 call, `compat` set, is named so, and shares the native number's
 * entry.
 */
static void syscall_missing(uint32_t syscall_num, int compat)
{
    const char *abi = compat ? "i386 call " : "";
    uint32_t pid = current_proc ? current_proc->pid : 0;
    const char *who = current_proc ? current_proc->comm : "?";

    if (!_trace_enosys) {
        return;
    }
    if (syscall_num >= SYSCALL_TABLE_SIZE) {
        if (_enosys_big_num != syscall_num || _enosys_big_pid != pid) {
            _enosys_big_num = syscall_num;
            _enosys_big_pid = pid;
            klog(KLOG_WARN, "syscall",
                 "%s%d: not implemented, and past the table (asked for by %s)", abi,
                 (int)syscall_num, who);
        }
    } else if (_enosys_last_pid[syscall_num] != pid) {
        _enosys_last_pid[syscall_num] = pid;
        klog(KLOG_WARN, "syscall", "%s%d: not implemented (asked for by %s)", abi, (int)syscall_num,
             who);
    }
}

/** Take the big lock for a call's work that needs it when the call runs
 * without it; nonzero when this took it, for `syscall_relock_end`. */
static int syscall_relock(void)
{
    if (bkl_held()) {
        return 0;
    }
    bkl_enter();
    return 1;
}

/** Give back the lock `syscall_relock` took, when `relock` says it did. */
static void syscall_relock_end(int relock)
{
    if (relock) {
        bkl_exit();
    }
}

/** Let go of the descriptors the call of thread `p`, the current one, held
 * (`fd_holds_release`), when it held any; each release takes the big lock
 * when its object needs it (`fd_teardown`). */
static void syscall_holds_release(const process_t *p)
{
    if (p->fd_holding) {
        fd_holds_release();
    }
}

void syscall_loop_drain(void)
{
    if (!ip_loop_pending()) {
        return;
    }
    if (bkl_held()) {
        ip_loop_drain_from(IP_LOOP_FROM_CALL);
    } else if (bkl_try_enter()) {
        ip_loop_drain_from(IP_LOOP_FROM_CALL);
        bkl_exit();
    } else {
        proc_work_set(current_proc, WORK_LOOP);
    }
}

/**
 * Mark the mmap family to run without the big lock, native and i386, as
 * Linux's take mmap_lock alone: each holds its address space's regions'
 * lock to write across its change (`regions_begin`, mm/regions.h) and
 * takes the big lock only around the steps that reach what has no lock of
 * its own, the change of entries that takes a page away or a right from
 * it, the change notice and the release of files; its balance and populate
 * run with neither. The i386 table's own mmap and mmap2 are marked as it is
 * filled (`ia32_syscall_init`).
 */
static void syscall_mm_unlock(void)
{
    static syscall_fn_t const calls[] = {
        sys_brk,           sys_mmap,     sys_mmap2,      sys_munmap, sys_mprotect,
        sys_pkey_mprotect, sys_mremap,   sys_madvise,    sys_mlock,  sys_munlock,
        sys_mlock2,        sys_mlockall, sys_munlockall,
    };
    uint32_t i;

    for (i = 0; i < sizeof(calls) / sizeof(calls[0]); i++) {
        syscall_unlock(SYSCALL_ABI_NATIVE, calls[i]);
        syscall_unlock(SYSCALL_ABI_IA32, calls[i]);
    }
}

/**
 * Mark the calls that read the caller's own identity to run without the big
 * lock, native and i386, as Linux's read the task's own fields and its
 * credentials under RCU: getpid, gettid, getppid and getpgrp read the
 * thread's record; getuid, geteuid, getgid, getegid, getresuid, getresgid
 * and getgroups its credentials, which only the thread itself changes
 * (`sys_setuid` and the rest change `current_proc` alone); getpgid and
 * getsid the caller's group or session for a pid of 0. Each word is read
 * once, and a word another thread writes under the big lock, the parent's
 * PID at a reparenting or the group a setpgid gives, reads as its old
 * value or its new one, as on Linux. A call takes the big lock itself for
 * a lookup of another process and in a user or PID namespace below the
 * first, whose maps and lookups the lock guards (`ids_lock`,
 * syscall_proc.c). The i386 forms with 16-bit ids, which call these, are
 * marked as their table is filled (`ia32_syscall_init`).
 */
static void syscall_ids_unlock(void)
{
    static syscall_fn_t const calls[] = {
        sys_getpid,    sys_gettid,    sys_getppid,   sys_getpgrp, sys_getpgid,
        sys_getsid,    sys_getuid,    sys_geteuid,   sys_getgid,  sys_getegid,
        sys_getresuid, sys_getresgid, sys_getgroups,
    };
    uint32_t i;

    for (i = 0; i < sizeof(calls) / sizeof(calls[0]); i++) {
        syscall_unlock(SYSCALL_ABI_NATIVE, calls[i]);
        syscall_unlock(SYSCALL_ABI_IA32, calls[i]);
    }
}

/**
 * Mark the futex calls to run without the big lock, native and i386, as
 * Linux's futexes have their bucket locks alone: each names its futex and
 * reads its word under the address space's regions' lock and the bucket's
 * spinlock, and the PI operations take the big lock themselves before a
 * bucket's lock (`futex_pi_call`, proc/futex.c). get_robust_list keeps the
 * big lock, since it looks another thread up. The i386 forms with their own
 * timespec are marked as their tables are filled (`arch_syscall_init`,
 * `ia32_syscall_init`).
 */
static void syscall_futex_unlock(void)
{
    static syscall_fn_t const calls[] = {
        sys_futex,      sys_futex_waitv,   sys_futex_wake,
        sys_futex_wait, sys_futex_requeue, sys_set_robust_list,
    };
    uint32_t i;

    for (i = 0; i < sizeof(calls) / sizeof(calls[0]); i++) {
        syscall_unlock(SYSCALL_ABI_NATIVE, calls[i]);
        syscall_unlock(SYSCALL_ABI_IA32, calls[i]);
    }
}

/**
 * The system call entry's handler: dispatch on the call's number.
 *
 * Looks up the number in _syscall_table, or for a call SC_COMPAT marks in
 * the compat table, and calls the handler, storing the result in the result
 * register (SC_RET) for the return to user mode. A
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
 * An interrupted call's result is translated into Linux's restart code
 * before the exit stop, or left as -EINTR for a call Linux never restarts.
 * The code stays in the result register through the signal-delivery stop.
 * `deliver_signals` then reads it and orig_nr to decide the restart, so a
 * tracer's changes at either stop apply. The translation counts as signal
 * work there even when another thread took the signal meanwhile, as Linux
 * keeps TIF_SIGPENDING until get_signal runs; a code a tracer writes into
 * a call that was not interrupted, with no other signal work, reaches the
 * program unchanged, as on Linux.
 *
 * A call that runs without the big lock (`syscall_entry_locked`) takes it
 * for the work of its entry and exit that needs it, which a tracer that
 * attached or a filter another thread installed meanwhile may bring: the
 * tracer's stops, the filters, the descriptors it held. The USB sticks wait
 * for a call that holds the lock. The entry's work, the tracer's stop and
 * the filters, is behind the thread's WORK_ENTRY bits, and the exit's
 * tracer stop behind WORK_TRACE, so that a call with none tests one word
 * at each end.
 *
 * The rest of the exit's work is the return's (`isr_leave`): a switch an
 * interrupt asked for, and whatever the loopback ring holds, a lone
 * datagram too, so that a receive that follows the call finds what it
 * sent, as on Linux 6.12.115, where loopback_xmit queues the datagram for
 * the softirq (drivers/net/loopback.c:69-92) and the sender's
 * local_bh_enable runs that softirq before the send returns
 * (kernel/softirq.c:402-407). The thread that puts a datagram on the ring
 * marks itself (WORK_LOOP), so a receive that follows the sender's return
 * needs no drain of its own. The sender's own frames are gone by then, so
 * no delivery runs under them. Interrupts are off, the drain does not run
 * inside itself, and it takes at most four times the ring's slots in one
 * run; what is left is taken by the next return or by the timer
 * (`syscall_loop_drain`).
 */
static void _syscall_handler(isr_params_t *params)
{
    process_t *p = current_proc;
    uint32_t syscall_num = (uint32_t)SC_NR(params);
    uint32_t size;
    syscall_fn_t *table = syscall_table_of(params, &size);
    syscall_fn_t fn = NULL;
    long ret = -ENOSYS;
    int skip = 0, relock;

    if (!p) {
        arch_irq_enable();
        usbdisk_service();
        if (syscall_num < size && table[syscall_num]) {
            ret = table[syscall_num](params);
        } else {
            syscall_missing(syscall_num, SC_COMPAT(params));
        }
        arch_irq_disable();
        SC_RET(params) = (unsigned long)ret;
        return;
    }
    p->in_syscall = PROC_IN_CALL;
    p->orig_nr = (int32_t)syscall_num;
    p->restart_kind = RESTART_SYS;
    p->restart_due = 0;

    arch_irq_enable();

    if (p->bkl_frames & 1u) {
        usbdisk_service();
    }
    if (p->work & WORK_ENTRY) {
        if (p->ptrace & PT_SYSCALL) {
            relock = syscall_relock();
            ptrace_syscall_stop(params, syscall_num, 0);
            syscall_num = (uint32_t)p->orig_nr;
            skip = p->orig_nr == -1;
            syscall_relock_end(relock);
        }
        if (!skip && p->seccomp_mode != SECCOMP_MODE_DISABLED) {
            relock = syscall_relock();
            if (!seccomp_permit(params, syscall_num, &ret)) {
                skip = 1;
                SC_RET(params) = (unsigned long)ret;
            }
            syscall_relock_end(relock);
        }
        p->restart_kind = RESTART_SYS;
        p->restart_due = 0;
    }
    if (skip) {
        ret = (long)SC_RET(params);
    } else if (syscall_num < size && table[syscall_num]) {
        fn = table[syscall_num];
        ret = fn(params);
    } else {
        syscall_missing(syscall_num, SC_COMPAT(params));
    }

    syscall_holds_release(p);
    if (ret == -EINTR) {
        ret = restart_result(params, fn, ret);
    }
    if ((p->work & WORK_TRACE) && (p->ptrace & (PT_SYSCALL | PT_STEP))) {
        relock = syscall_relock();
        SC_RET(params) = (unsigned long)ret;
        ptrace_syscall_stop(params, syscall_num, 1);
        ret = (long)SC_RET(params);
        syscall_relock_end(relock);
    }

    arch_irq_disable();

    p->in_syscall = 0;

    /* The result, in the register the return to user mode restores it from. */
    SC_RET(params) = (unsigned long)ret;
}

/**
 * Mark the file calls that every architecture's table maps to these
 * handlers (`syscall_unlock`): read, write, pread64, pwrite64, ioctl, fstat
 * and close run a descriptor's operations without the big lock only for a
 * type that declares FILE_OPS_UNLOCKED (`fd_call_lock`, `file_ops_lock`),
 * and readv and writev for a pipe end or Unix socket (`vec_call_lock`),
 * or for a file or directory of a type that sets `reads_unlocked`
 * (`file_vfs_unlocked`); getdents64 lists such a directory, fstatat and
 * newfstatat stat a path the name cache finishes (`stat_at_unlocked`), and
 * open and openat make a read-only descriptor of such a file
 * (`open_unlocked`), and access, faccessat and faccessat2 check a path the
 * name cache finishes (`access_unlocked`), and readlink and readlinkat
 * read a link it finds (`readlink_unlocked`), each taking the big lock for
 * anything else; poll and
 * ppoll scan their descriptors the same way and sleep without it, taking it
 * for the signal state when the thread is marked for a signal or the call
 * changes its mask (`poll_signal_pending`, `poll_mask_set`); flock and
 * fcntl's lock commands take the advisory locks' own lock (fs/lock.c), and
 * the big lock only for a file keyed by its node (`fd_locks_by_node`), and
 * fcntl takes it for the commands `fcntl_big` names; lseek and _llseek
 * take it for a descriptor `seek_unlocked` does not admit; write,
 * pwrite64 and writev write a regular file of a type that sets
 * `writes_unlocked` without it (`file_vfs_writes_unlocked`), and
 * ftruncate, fallocate, fsync and fdatasync run without it for such a file
 * alone (`fd_change_lock`); splice between two pipes and tee need only the
 * pipes' locks, and splice takes the big lock for any other pair
 * (`splice_file`). Every
 * other step those calls make takes the locks of what it touches; the
 * user-memory copies take the big lock until address spaces have locks of
 * their own. The forms of an architecture's own, i686's split offsets and
 * the i386 calls of x86-64, are marked as its tables are filled
 * (`arch_syscall_init`, `ia32_syscall_init`).
 */
static void unlock_file_calls(void)
{
    static const syscall_fn_t calls[] = {
        sys_read,      sys_write,     sys_pread64,   sys_pwrite64,   sys_ioctl,      sys_fstat64,
        sys_close,     sys_poll,      sys_ppoll,     sys_fstatat64,  sys_getdents64, sys_open,
        sys_openat,    sys_readv,     sys_writev,    sys_flock,      sys_fcntl64,    sys_lseek,
        sys__llseek,   sys_access,    sys_faccessat, sys_faccessat2, sys_readlink,   sys_readlinkat,
        sys_ftruncate, sys_fallocate, sys_fsync,     sys_fdatasync,  sys_splice,     sys_tee,
    };
    uint32_t i;

    for (i = 0; i < sizeof(calls) / sizeof(calls[0]); i++) {
        syscall_unlock(SYSCALL_ABI_NATIVE, calls[i]);
        syscall_unlock(SYSCALL_ABI_IA32, calls[i]);
    }
}

/**
 * Mark the epoll calls to run without the big lock, native and i386: a
 * wait scans its set under the set's own lock and takes the big lock only
 * for a set that watches a descriptor whose poll needs it, or another set
 * (`scan_set`, io/epoll.c), and a change of a set takes it only to add or
 * remove a set inside another, as Linux's eventpoll has `ep->lock`,
 * `ep->mtx` and `epnested_mutex` (fs/eventpoll.c:314-326, 392). The i386
 * epoll_pwait2 of x86-64, which converts its timespec, is marked as its
 * table is filled (`ia32_syscall_init`).
 */
static void syscall_epoll_unlock(void)
{
    static syscall_fn_t const calls[] = {
        sys_epoll_wait,
        sys_epoll_pwait,
        sys_epoll_pwait2,
        sys_epoll_ctl,
    };
    uint32_t i;

    for (i = 0; i < sizeof(calls) / sizeof(calls[0]); i++) {
        syscall_unlock(SYSCALL_ABI_NATIVE, calls[i]);
        syscall_unlock(SYSCALL_ABI_IA32, calls[i]);
    }
}

/**
 * Mark socket entries unlocked: Unix sockets have their own locks and
 * socket_call takes the big lock for another family alone. Argument and
 * message-vector conversions touch only the caller's state and use the
 * address space's lock for user copies.
 */
static void syscall_socket_unlock(void)
{
    static syscall_fn_t const calls[] = {
        sys_socketcall, sys_socket,   sys_socketpair,  sys_bind,           sys_connect,
        sys_listen,     sys_accept4,  sys_getsockname, sys_getpeername,    sys_sendto,
        sys_recvfrom,   sys_sendmsg,  sys_recvmsg,     sys_shutdown,       sys_setsockopt,
        sys_getsockopt, sys_sendmmsg, sys_recvmmsg,    sys_recvmmsg_time64};
    uint32_t i;

    for (i = 0; i < sizeof(calls) / sizeof(calls[0]); i++) {
        syscall_unlock(SYSCALL_ABI_NATIVE, calls[i]);
        syscall_unlock(SYSCALL_ABI_IA32, calls[i]);
    }
#if defined(IA32_SYSCALL_TABLE_SIZE)
    syscall_unlock(SYSCALL_ABI_IA32, compat_sys_socketcall);
#endif
}

void init_syscall(void)
{
    uint32_t i;

    for (i = 0; i < SYSCALL_TABLE_SIZE; i++) {
        _syscall_table[i] = NULL;
    }
    arch_syscall_init(_syscall_table, _syscall_handler);
#if defined(IA32_SYSCALL_TABLE_SIZE)
    for (i = 0; i < IA32_SYSCALL_TABLE_SIZE; i++) {
        _ia32_syscall_table[i] = NULL;
    }
    ia32_syscall_init(_ia32_syscall_table);
#endif
    /* The calls that take no lock but the scheduler's. */
    syscall_unlock(SYSCALL_ABI_NATIVE, sys_sched_yield);
    syscall_unlock(SYSCALL_ABI_IA32, sys_sched_yield);
    /* getrusage takes the big lock itself for any target but the thread. */
    syscall_unlock(SYSCALL_ABI_NATIVE, sys_getrusage);
    syscall_unlock(SYSCALL_ABI_IA32, sys_getrusage);
    /* rt_sigprocmask takes the signal state's lock, and the big lock only
     * for a change that lock alone cannot make. */
    syscall_unlock(SYSCALL_ABI_NATIVE, sys_rt_sigprocmask);
#if defined(IA32_SYSCALL_TABLE_SIZE)
    syscall_unlock(SYSCALL_ABI_IA32, compat_sys_rt_sigprocmask);
#endif
    unlock_file_calls();
    syscall_epoll_unlock();
    syscall_socket_unlock();
    syscall_mm_unlock();
    syscall_futex_unlock();
    syscall_ids_unlock();

    _trace_enosys = kcmdline_has("rach2.trace=enosys");
    sched_cycle_account = kcmdline_has("rach2.cycleaccount");
}
