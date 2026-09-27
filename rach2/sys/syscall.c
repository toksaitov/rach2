/*
 * The system call layer's entry and its table: the dispatcher every trap
 * reaches, the registration of every handler, and the calls that are the
 * machine's own: time, identity, the terminal and the reboot. The process,
 * file system and socket calls are in syscall_proc.c, syscall_fs.c and
 * syscall_net.c; syscall_internal.h is what the four share.
 */
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
#include "proc/tls.h"
#include "proc/futex.h"
#include "tables/gdt.h"
#include "lib/krandom.h"
#include "lib/kerrno.h"
#include "lib/kirq.h"
#include "lib/ksignal.h"
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
#include "display/vga_gfx.h"
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
#include "syscall_internal.h"

/* Only one process owns the framebuffer at a time. Text mode is restored when
 * the owner exits and when it asks for it; a close of one descriptor of a
 * shared description leaves the mode as it is, since the description is still
 * open. */
volatile uint32_t fb_owner_pid = 0;

extern volatile uint32_t kernel_uptime_seconds;
extern volatile uint32_t kernel_second_ticks;

/*
 * ioctl request codes (Linux compatible subset)
 */
#define KIOCSOUND 0x4B2F /* Set PC speaker frequency (0 = off) */

#define TCGETS 0x5401 /* Get terminal attributes (termios) */

#define TCSETS 0x5402 /* Set terminal attributes now */

#define TCSETSW 0x5403 /* Set after the output drains */

#define TCSETSF 0x5404 /* Set after output drains, flush input */

#define TCFLSH 0x540B /* Flush input and/or output queues */

#define FIONREAD 0x541B /* Bytes available to read */

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
 * Audio write parameters for AUDIO_WRITE
 */
struct audio_write_params {
    const uint8_t *buffer;
    uint32_t size;
};

/*
 * Syscall function type - takes pointer to params, returns int32_t
 */
typedef int32_t (*syscall_fn_t)(isr_params_t *);

/*
 * Syscall dispatch table
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
 * reboot(2): stop the machine. EBX = magic1, ECX = magic2, EDX = command.
 *
 * Does not return for the three commands that stop the machine. Returns
 * `-EPERM` for an unprivileged caller, `-EINVAL` for a wrong magic or an
 * unknown command, and 0 for `RB_DISABLE_CAD`.
 *
 * The privilege check comes before the magics, which inverts the order the rest
 * of this file uses; Linux inverts it the same way.
 *
 * `RB_DISABLE_CAD` returning 0 is what makes the rest testable: every other
 * command that this call accepts stops the machine, so a test has no other way
 * to ask whether a magic was accepted. It asks for a state this kernel is
 * permanently in, since there is no keyboard reboot to disable, so 0 is a true
 * answer. `RB_ENABLE_CAD` asks
 * for a feature that does not exist and answers `-EINVAL`, because a 0 there
 * would be the one reply a caller could not tell from a working one.
 */
static int32_t sys_reboot(isr_params_t *params)
{
    uint32_t magic1 = (uint32_t)params->ebx;
    uint32_t magic2 = (uint32_t)params->ecx;
    uint32_t cmd = (uint32_t)params->edx;

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
 * timeval structure for gettimeofday
 */
struct kernel_timeval {
    int32_t tv_sec;  /* seconds */
    int32_t tv_usec; /* microseconds */
};

/*
 * gettimeofday(2): EBX = tv (timeval*), ECX = tz (ignored). Reports real time
 * from the clock anchored at boot (sys/rtc.c), or time since boot when there
 * was no clock to read. Returns 0 or -errno.
 */
static int32_t sys_gettimeofday(isr_params_t *params)
{
    struct kernel_timeval __user *tv = (struct kernel_timeval __user *)params->ebx;
    struct kernel_timeval ktv;

    if (!tv) {
        return -EFAULT;
    }

    {
        int64_t seconds;
        int64_t nanoseconds;

        get_current_time(&seconds, &nanoseconds);
        ktv.tv_sec = (int32_t)seconds;
        ktv.tv_usec = (int32_t)nanoseconds / 1000; /* narrow first: no __divdi3 */
    }

    if (copy_to_user(tv, &ktv, sizeof(ktv)) != 0) {
        return -EFAULT;
    }

    return 0;
}

/**
 * Read a POSIX clock (clock_gettime, syscall 265, 32-bit timespec).
 *
 * EBX = clockid, ECX = struct timespec*. CLOCK_REALTIME(0) reports real time
 * from the clock read at boot (sys/rtc.c); CLOCK_MONOTONIC(1) reports time
 * since boot, which is what the scheduler, the sleep path and the audio timing
 * loops mean by a clock. Both run at the 10 ms tick resolution, and neither is
 * disciplined by anything: the machine reads the CMOS once and then keeps its
 * own time. musl falls back here from clock_gettime64 (403), which this kernel
 * does not provide. The signed seconds field ends in 2038 for real time
 * and after about 68 years since boot for monotonic time. Reading the uptime
 * pair with interrupts masked avoids both a torn second and tick-count wrap.
 */
static int32_t sys_clock_gettime(isr_params_t *params)
{
    int clk = (int)params->ebx;
    struct kernel_timespec __user *ts = (struct kernel_timespec __user *)params->ecx;
    struct kernel_timespec kts;
    uint32_t flags;

    if (!ts) {
        return -EFAULT;
    }
    if (clk != 0 && clk != 1) { /* CLOCK_REALTIME / CLOCK_MONOTONIC only */
        return -EINVAL;
    }

    if (clk == 0) {
        int64_t seconds;
        int64_t nanoseconds;

        get_current_time(&seconds, &nanoseconds);
        kts.tv_sec = (int32_t)seconds;
        kts.tv_nsec = (int32_t)nanoseconds;
    } else {
        flags = kirq_save();
        kts.tv_sec = (int32_t)kernel_uptime_seconds;
        kts.tv_nsec = (int32_t)(kernel_second_ticks * 10000000);
        kirq_restore(flags);
    }

    if (copy_to_user(ts, &kts, sizeof(kts)) != 0) {
        return -EFAULT;
    }
    return 0;
}

/**
 * End a sleep a signal cut short `left` ticks before its deadline: the
 * process runs again, the kernel counts the sleep, and `urem`, when there is
 * one, gets the ticks left less the tick added for the tick in progress.
 */
static int32_t nanosleep_interrupted(struct kernel_timespec __user *urem, uint32_t left)
{
    current_proc->wakeup_tick = 0;
    current_proc->state = PROC_RUNNING;
    process_count_sleep_cut_short();
    if (urem) {
        struct kernel_timespec rem;
        uint32_t unslept = left - 1u;

        rem.tv_sec = (int32_t)(unslept / 100u);
        rem.tv_nsec = (int32_t)((unslept % 100u) * 10000000u);
        (void)copy_to_user(urem, &rem, sizeof(rem));
    }
    return -EINTR;
}

/**
 * Block-and-schedule sleep (nanosleep, syscall 162, 32-bit timespec).
 *
 * EBX = const struct timespec* req, ECX = struct timespec* rem (optional).
 * Rounds up to whole 10 ms ticks, then parks the process BLOCKED on its
 * wakeup_tick and yields; the timer wakes it at the deadline while OTHER
 * processes keep running. The tick in progress is not counted, so a sleep is
 * never shorter than asked, up to the clamp below. The deadline and the
 * pending signals are tested, and the process marked BLOCKED, with interrupts
 * off, so neither a signal nor the deadline's tick can come between the test
 * and the block unseen; the halt after a yield that ran nothing is masked for
 * the same reason.
 *
 * Once the deadline tick has come the sleep is over, and the call returns 0
 * whether or not a signal is pending. A signal the process acts on, found
 * pending before then, ends the sleep with -EINTR; a wake by one it does not
 * act on sends it back to sleep with the deadline stored again. rem is the
 * ticks left to the deadline less the tick added for the tick in progress, so
 * a caller that sleeps again for the remainder in the same tick ends at the
 * first sleep's deadline, and later by the ticks it takes to call again; a
 * signal in the last tick leaves no remainder, so that sleep can end up to a
 * tick before the time first asked. rem is written as zero on normal
 * completion.
 */
static int32_t sys_nanosleep(isr_params_t *params)
{
    const struct kernel_timespec __user *ureq = (const struct kernel_timespec __user *)params->ebx;
    struct kernel_timespec __user *urem = (struct kernel_timespec __user *)params->ecx;
    struct kernel_timespec req;
    uint32_t ticks, target;

    if (!current_proc || !ureq) {
        return -EFAULT;
    }
    if (copy_from_user(&req, ureq, sizeof(req)) != 0) {
        return -EFAULT;
    }
    if (req.tv_sec < 0 || req.tv_nsec < 0 || req.tv_nsec >= 1000000000) {
        return -EINVAL;
    }

    /* Convert to whole 10 ms ticks without 64-bit division (the freestanding
     * kernel has no libgcc __udivdi3): 1 s == 100 ticks, plus the ceil of the
     * sub-second nanoseconds, so any nonzero request is a tick at least.
     * Absurd durations are clamped so sec*100 cannot overflow and the signed
     * tick comparison below stays valid with the tick in progress added. */
    {
        uint32_t sec = (uint32_t)req.tv_sec;
        uint32_t nsec_ticks = ((uint32_t)req.tv_nsec + 9999999u) / 10000000u;
        if (sec > 21474836u) {
            sec = 21474836u; /* keeps the intermediate sec*100 below 2^31 */
        }
        ticks = sec * 100u + nsec_ticks;
        if (ticks > 0x7FFFFFFEu) {
            ticks = 0x7FFFFFFEu; /* keep the signed (target-now) delta valid */
        }
    }

    target = kernel_ticks + ticks + (ticks != 0 ? 1u : 0u);
    for (;;) {
        uint32_t flags = kirq_save();
        int32_t left = (int32_t)(target - kernel_ticks);

        if (left <= 0) {
            kirq_restore(flags);
            break;
        }
        if (signal_pending()) {
            kirq_restore(flags);
            return nanosleep_interrupted(urem, (uint32_t)left);
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
/* ITIMER_REAL, the one interval timer this kernel keeps. */
#define ITIMER_REAL 0

/*
 * The itimerval of i386 setitimer and getitimer: the interval, then the
 * value, each in seconds and microseconds of 32 bits. musl passes this form
 * for its 64-bit time_t.
 */
struct kernel_itimerval {
    int32_t interval_sec;
    int32_t interval_usec;
    int32_t value_sec;
    int32_t value_usec;
};

/*
 * A time in seconds and microseconds as whole 10 ms ticks, rounded up. The
 * seconds are clamped where nanosleep clamps them and the ticks at 0x7FFFFFFD,
 * so the signed difference between the timer's tick and the count stays
 * positive with the tick in progress added and with the tick `proc_alarm_at`
 * adds when the sum wraps to zero. Zero only for a time of zero.
 */
static uint32_t itimer_ticks(int32_t sec, int32_t usec)
{
    uint32_t whole = (uint32_t)sec;
    uint32_t ticks;

    if (whole > 21474836u) {
        whole = 21474836u;
    }
    ticks = whole * 100u + ((uint32_t)usec + 9999u) / 10000u;
    return ticks > 0x7FFFFFFDu ? 0x7FFFFFFDu : ticks;
}

/* The ticks left before the current process's timer is due, 0 when it is
 * disarmed. Part of the tick in progress has passed, so the time left lies
 * between the whole ticks to the due tick less one and that number; the
 * lower end is what is reported, and a timer due within a tick reads as one
 * tick left, since zero means disarmed. So a value read in the timer's last
 * tick and passed straight back to setitimer comes due a tick later: the
 * read reports one tick where less than one is left, and the set adds a tick
 * for the tick in progress. Interrupts are off: the tick rearms the timer. */
static uint32_t itimer_left(void)
{
    uint32_t left;

    if (current_proc->alarm_tick == 0) {
        return 0;
    }
    left = current_proc->alarm_tick - kernel_ticks;
    return (int32_t)left > 1 ? left - 1u : 1u;
}

/* What the current process's timer is set to, as setitimer and getitimer
 * report it. Interrupts are off. */
static void itimer_read(struct kernel_itimerval *out)
{
    uint32_t left = itimer_left();

    out->value_sec = (int32_t)(left / 100u);
    out->value_usec = (int32_t)((left % 100u) * 10000u);
    out->interval_sec = (int32_t)(current_proc->alarm_interval / 100u);
    out->interval_usec = (int32_t)((current_proc->alarm_interval % 100u) * 10000u);
}

/*
 * Arm or disarm the current process's timer: the first SIGALRM `value`
 * ticks from now and then every `interval`, or disarmed for a value of 0.
 * The tick in progress is not counted, so the first SIGALRM never comes
 * before the time asked for; the later ones are rearmed at a tick and come
 * every `interval`. Interrupts are off.
 */
static void itimer_set(uint32_t value, uint32_t interval)
{
    if (value == 0) {
        current_proc->alarm_tick = 0;
        current_proc->alarm_interval = 0;
        return;
    }
    current_proc->alarm_tick = proc_alarm_at(kernel_ticks, value + 1u);
    current_proc->alarm_interval = interval;
}

/**
 * setitimer(2), syscall 104: the real-time timer.
 *
 * EBX = which, ECX = const struct itimerval *new, EDX = struct itimerval
 * *old, which may be null. ITIMER_REAL is the only timer: the virtual and
 * profiling timers count a process's own processor time, which this kernel
 * does not keep, and are refused with EINVAL, as is a negative time or
 * microseconds past a second. A value of zero disarms the timer; otherwise
 * the first SIGALRM is due that long from now, rounded up to whole 10 ms
 * ticks, then every interval, or only once for an interval of zero. `old`
 * receives the timer's setting before the call.
 */
static int32_t sys_setitimer(isr_params_t *params)
{
    const struct kernel_itimerval __user *unew =
        (const struct kernel_itimerval __user *)params->ecx;
    struct kernel_itimerval __user *uold = (struct kernel_itimerval __user *)params->edx;
    struct kernel_itimerval set, was;
    uint32_t flags;

    if ((int32_t)params->ebx != ITIMER_REAL) {
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
    itimer_read(&was);
    itimer_set(itimer_ticks(set.value_sec, set.value_usec),
               itimer_ticks(set.interval_sec, set.interval_usec));
    kirq_restore(flags);
    if (uold && copy_to_user(uold, &was, sizeof(was)) != 0) {
        return -EFAULT;
    }
    return 0;
}

/**
 * getitimer(2), syscall 105: what the real-time timer is set to.
 *
 * EBX = which, ECX = struct itimerval *cur: the time left before the next
 * SIGALRM, zero when the timer is disarmed, and the interval.
 */
static int32_t sys_getitimer(isr_params_t *params)
{
    struct kernel_itimerval __user *ucur = (struct kernel_itimerval __user *)params->ecx;
    struct kernel_itimerval cur;
    uint32_t flags;

    if ((int32_t)params->ebx != ITIMER_REAL) {
        return -EINVAL;
    }
    flags = kirq_save();
    itimer_read(&cur);
    kirq_restore(flags);
    if (!ucur || copy_to_user(ucur, &cur, sizeof(cur)) != 0) {
        return -EFAULT;
    }
    return 0;
}

/**
 * alarm(2), syscall 27: one SIGALRM `seconds` from now, or none for 0.
 *
 * EBX = seconds. The same timer as setitimer's, with no interval. Returns
 * the seconds that were left before the earlier alarm, rounded as Linux
 * rounds them: to the nearest second, and up to one when any time was left.
 * musl's `alarm` calls setitimer; this is the call older programs make.
 */
static int32_t sys_alarm(isr_params_t *params)
{
    uint32_t flags = kirq_save();
    uint32_t left = itimer_left();
    uint32_t seconds = left / 100u;
    uint32_t usec = (left % 100u) * 10000u;

    itimer_set(itimer_ticks((int32_t)(params->ebx > 21474836u ? 21474836u : params->ebx), 0), 0);
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
 * Fill a buffer with random bytes (getrandom, syscall 355).
 *
 * EBX = buf, ECX = buflen, EDX = flags. Backed by the kernel CSPRNG
 * (lib/krandom, ChaCha20). Returns the byte count, like Linux; musl's
 * getrandom/arc4random sit on top of this. We never block (GRND_NONBLOCK is
 * a no-op) and have a single source (GRND_RANDOM is equivalent); unknown
 * flag bits are rejected.
 */
static int32_t sys_getrandom(isr_params_t *params)
{
    void __user *buf = (void __user *)params->ebx;
    uint32_t len = params->ecx;
    uint32_t flags = params->edx;
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
    /* Fail closed: with no trusted hardware entropy (RDSEED/RDRAND), refuse,
     * not hand out the weak boot-jitter stream for a key. A caller that
     * explicitly accepts insecure bytes passes GRND_INSECURE (the same tier as
     * AT_RANDOM / the stack canary). */
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
        /* A large call gives way every 4 KiB, between two pieces, so a
         * reader the tick woke does not wait for the whole call. */
        if ((done & 4095u) == 0) {
            sched_service_point();
        }
    }
    return (int32_t)done;
}

/* True if some process is in process group `pgid` within session `sid`. This is
 * the TIOCSPGRP rule that a foreground group must belong to the controlling
 * session.
 *
 * A zombie counts. A process leaves its group when it is reaped and not when it
 * exits, and a shell depends on that: putting a pipeline in the foreground
 * means setting the terminal's group to the first member's pid, and that member
 * can be a short command that has already exited by the time the shell gets
 * there. Excluding zombies refused the claim in exactly that race and left the
 * shell reporting that it could not set the terminal's process group. */
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

/* The tty behind a descriptor for the shared terminal ioctls: the console for a
 * FD_TYPE_CONSOLE fd, the slave tty for a FD_TYPE_PTS (pty slave) fd, else
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
 * The shared terminal ioctls operating on a GIVEN tty.
 *
 * The real termios layer plus the session/job-control requests, so the
 * console and a pty slave share one implementation. Sets *handled = 1 and
 * returns the result for a recognized terminal request; sets *handled = 0
 * (return value unused) otherwise, so the caller falls through to its own
 * handling. TCGETS/TCSETS exchange the 36-byte kernel termios (n_tty.h);
 * TCSETSF and TCFLSH flush the unread input.
 */
static int32_t tty_common_ioctl(struct tty *t, uint32_t request, uint32_t arg, int *handled)
{
    *handled = 1;
    switch (request) {
    case TCGETS:
        if (!arg) {
            return -EFAULT;
        }
        if (copy_to_user((void __user *)arg, &t->ldisc.tio, sizeof(struct ktermios)) != 0) {
            return -EFAULT;
        }
        return 0;

    case FIONREAD: {
        /* Bytes a read would return right now (a complete cooked line, or
         * the queued count in raw mode); a pending EOF counts as 0. Drains
         * the input source first so freshly arrived input is counted. */
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
    case TCSETSF: {
        struct ktermios kt;
        if (!arg) {
            return -EFAULT;
        }
        if (copy_from_user(&kt, (const void __user *)arg, sizeof(struct ktermios)) != 0) {
            return -EFAULT;
        }
        /* Install with interrupts off (vs IRQ-side input); TCSETSF also
         * discards pending input. */
        tty_set_termios(t, &kt, request == TCSETSF);
        return 0;
    }

    case TCFLSH:
        /* arg is the queue selector: TCIFLUSH(0)/TCIOFLUSH(2) drop input;
         * we hold no output queue, so TCOFLUSH(1) is a no-op. Else invalid.
         */
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
        int pgrp = t->fg_pgid;
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
        /* The caller must hold this terminal as its controlling terminal
         * (checked before the argument, as Linux does)... */
        if (t->sid == 0 || (int)current_proc->sid != t->sid) {
            return -ENOTTY;
        }
        if (pgrp <= 0) {
            return -EINVAL;
        }
        /* ...and the new foreground group must live in that session. */
        if (!pgrp_in_session(pgrp, t->sid)) {
            return -EPERM;
        }
        t->fg_pgid = pgrp;
        return 0;
    }

    case TIOCSCTTY:
        /* A session leader with no controlling tty claims this terminal for
         * its session; re-claiming by the same session is idempotent. */
        if ((int)current_proc->sid != (int)current_proc->pid) {
            return -EPERM; /* not a session leader */
        }
        if (t->sid != 0 && t->sid != (int)current_proc->sid) {
            return -EPERM; /* owned by another session */
        }
        /* Claim t as this session's SOLE controlling terminal: one atomic
         * step publishes t (sid + foreground group) AND drops every other
         * terminal the session held (the console and any other pty), so the
         * timer-IRQ serial drain never sees a torn or doubled ctty and
         * /dev/tty resolves to t. */
        tty_make_sole_ctty(t, (int)current_proc->sid, (int)current_proc->pgid);
        return 0;

    case TIOCNOTTY:
        /* Give up the controlling terminal if it belongs to our session. */
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
static int32_t framebuffer_ioctl(uint32_t request, uint32_t arg)
{
    if (request != FB_MODE_13H && request != FB_MODE_TEXT && request != FB_SETPALETTE) {
        return -ENOTTY;
    }
    if (fb_owner_pid != current_proc->pid && (fb_owner_pid != 0 || request != FB_MODE_13H)) {
        return -EBUSY;
    }
    switch (request) {
    case FB_MODE_13H:
        if (!vga_graphics_mode) {
            vga_mode13h_enter();
        }
        fb_owner_pid = current_proc->pid;
        return 0;
    case FB_MODE_TEXT:
        vga_mode13h_exit();
        fb_owner_pid = 0;
        return 0;
    default:
        if (!arg || !user_buffer_ok(arg, 768, 0)) {
            return -EFAULT;
        }
        vga_mode13h_set_palette((const uint8_t *)arg);
        return 0;
    }
}

/**
 * Device control (ioctl(2)).
 *
 * EBX = fd, ECX = request, EDX = arg. A tty fd (console or pty slave) routes
 * to the shared termios/session handler first; an ops-backed object handles
 * its own remaining requests; the framebuffer and console handle their
 * device-specific ones (FB_*, KIOCSOUND, AUDIO_*). Returns 0, the request
 * result, or -errno (-ENOTTY for a terminal request on a non-terminal fd).
 */
static int32_t sys_ioctl(isr_params_t *params)
{
    int fd_num = (int)params->ebx;
    uint32_t request = params->ecx;
    uint32_t arg = params->edx;
    fd_entry_t *entry;

    if (!current_proc) {
        return -ESRCH;
    }

    /* Validate the fd first, so a bad fd reports -EBADF, not a request-
     * specific status (e.g. ioctl(-1, TCGETS) must be -EBADF, not -ENOTTY). */
    entry = fd_get(&current_proc->fds, fd_num);
    if (!entry) {
        return -EBADF;
    }

    /* A socket accepts the interface ioctls: this is where a program sets the
     * machine's address, which is what a DHCP client is for. They are checked
     * before the tty block because a socket is not a terminal and would
     * otherwise fall through to -ENOTTY. */
    if (entry->type == FD_TYPE_SOCKET && entry->sock) {
        int handled;
        int32_t r = netif_ioctl(request, arg, &handled);

        if (handled) {
            return r;
        }
    }

    /* tty-family fds (the console and a pty slave) share the termios/session
     * ioctls, handled here, BEFORE the generic ops dispatch, so a pty slave's
     * TCGETS/TIOCSCTTY reach the shared handler, not its FIONBIO-only
     * ops->ioctl. On a non-terminal fd the terminal-only requests stay -ENOTTY
     * (the fallthrough below), as Linux reports. */
    {
        struct tty *t = fd_tty(entry);
        if (t) {
            int handled;
            int32_t r = tty_common_ioctl(t, request, arg, &handled);
            if (handled) {
                return r;
            }
            /* not a terminal ioctl: fall through (a pts slave still has its
             * ops->ioctl for FIONBIO; the console falls to the audio block). */
        }
    }

    /* A backing-ops object (pipe/pty/device) owns its remaining ioctls:
     * FIONBIO, and (for the pty master) TIOCGPTN/TIOCSPTLCK/TIOCSWINSZ. */
    if (entry->ops) {
        return entry->ops->ioctl ? entry->ops->ioctl(entry, request, arg) : -ENOTTY;
    }
    if (request == TCGETS || request == TCSETS || request == TCSETSW || request == TCSETSF ||
        request == TCFLSH || request == TIOCGWINSZ || request == TIOCSWINSZ ||
        request == TIOCGPGRP || request == TIOCSPGRP || request == TIOCSCTTY ||
        request == TIOCNOTTY) {
        return -ENOTTY; /* terminal-only requests on a non-terminal fd */
    }

    /* KIOCSOUND is handled by the console-type block below, keyed on the
     * backing object's type (not the fd number) so it cannot fire on an fd 1/2
     * that has been closed and reopened onto a regular file (now that
     * close(0/1/2) is legal). */

    if (entry->type == FD_TYPE_FRAMEBUFFER) {
        return framebuffer_ioctl(request, arg);
    }

    if (entry->type == FD_TYPE_CONSOLE) {
        if (request == KIOCSOUND) {
            if (arg == 0) {
                speaker_off();
            } else {
                speaker_on(arg);
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
            /* Copy the param struct in through the helper, then range-check
             * the PCM buffer it points at before the driver reads it (the
             * bulk copy is the driver's, as with the framebuffer/socket
             * data paths). */
            if (copy_from_user(&kp, (const void __user *)arg, sizeof(kp)) != 0) {
                return -EFAULT;
            }
            if (!kp.buffer || !user_buffer_ok((uint32_t)(uintptr_t)kp.buffer, kp.size, 0)) {
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

/*
 * User address window. The kernel is identity-mapped in the low 32 MB and user
 * space lives above it, below 0xC0000000. A user pointer and the whole length
 * it spans must fall inside this window; anything else is rejected so a syscall
 * argument cannot read or write kernel memory. The window and the check now
 * live in mm/usercopy (USER_ADDR_MIN/MAX, user_range_ok); user_access_ok is a
 * thin alias kept for the existing call sites.
 */
int user_access_ok(uint32_t addr, uint32_t len)
{
    return user_range_ok(addr, len);
}

/*
 * Range-check a user buffer AND back its pages before the kernel passes the
 * raw pointer to a lower layer (krffs, tcp, udp, the drivers) that
 * dereferences it directly. With demand paging a numerically valid pointer
 * may still be unmapped (a never-touched heap page, or a wild address in no
 * VMA); vm_prefault_range faults the valid pages in here and rejects the bad
 * ones, so the lower layer's raw access can never fault in kernel mode and
 * panic. 'write' is 1 when that layer writes into the buffer (so the pages
 * need write permission).
 */
int user_buffer_ok(uint32_t addr, uint32_t len, int write)
{
    return user_access_ok(addr, len) && vm_prefault_range(addr, len, write);
}

/**
 * Set up thread-local storage (set_thread_area(2), Linux i386).
 *
 * EBX points at a struct user_desc. Validates it, programs the shared TLS
 * GDT descriptor (entry GDT_TLS_ENTRY) and this process's PCB base, then
 * writes the chosen entry number back so the C library can load
 * (entry_number << 3) | 3 into %gs. The base is reapplied to the GDT on
 * every context switch (switch.s). Returns 0 or a negative errno on a bad
 * pointer/request.
 */
static int32_t sys_set_thread_area(isr_params_t *params)
{
    uint32_t uptr = params->ebx;
    user_desc_t desc;
    uint32_t base = 0;

    if (!current_proc) {
        return -ESRCH; /* TLS is only meaningful for a user process */
    }

    /* Read the descriptor through the fault-checked helper (it range-checks the
     * full struct internally, so the prior explicit user_access_ok is
     * redundant). */
    if (copy_from_user(&desc, (const void __user *)uptr, sizeof(user_desc_t)) != 0) {
        return -EFAULT;
    }

    if (tls_resolve_base(&desc, &base) != 0) {
        return -EINVAL;
    }

    /* Program the descriptor now and remember the base so the context switch
     * can reprogram entry GDT_TLS_ENTRY for this process when it is resumed. */
    gdt_set_tls(base);
    current_proc->tls_base = base;

    /* Report the allocated slot back (musl reads user_desc.entry_number). */
    desc.entry_number = GDT_TLS_ENTRY;
    if (copy_to_user((void __user *)uptr, &desc.entry_number, sizeof(desc.entry_number)) != 0) {
        return -EFAULT;
    }

    return 0;
}

/**
 * Process times (times(2)).
 *
 * Per-process CPU time is not tracked, so the four tms fields are reported
 * as zero; the return value is the monotonic tick count (already in CLK_TCK
 * = 100 Hz units). EBX = struct tms * (NULL is allowed: the caller may want
 * only the return value).
 */
static int32_t sys_times(isr_params_t *params)
{
    struct kernel_tms {
        int32_t tms_utime;
        int32_t tms_stime;
        int32_t tms_cutime;
        int32_t tms_cstime;
    } t;

    if (params->ebx) {
        t.tms_utime = 0;
        t.tms_stime = 0;
        t.tms_cutime = 0;
        t.tms_cstime = 0;
        if (copy_to_user((void __user *)params->ebx, &t, sizeof(t)) != 0) {
            return -EFAULT;
        }
    }
    return (int32_t)kernel_ticks;
}

/**
 * System statistics (sysinfo(2)).
 *
 * Fills the 64-byte i386 struct sysinfo: uptime in seconds, the count of
 * process records including unreaped children, total and free RAM from the
 * frame counts, mem_unit = 1; CPU load, shared, buffer and swap figures are
 * reported as zero (not tracked). EBX = struct sysinfo *.
 */
static int32_t sys_sysinfo(isr_params_t *params)
{
    struct kernel_sysinfo {
        int32_t uptime;
        uint32_t loads[3];
        uint32_t totalram;
        uint32_t freeram;
        uint32_t sharedram;
        uint32_t bufferram;
        uint32_t totalswap;
        uint32_t freeswap;
        uint16_t procs;
        uint16_t pad;
        uint32_t totalhigh;
        uint32_t freehigh;
        uint32_t mem_unit;
        char _f[8];
    } si;
    uint32_t i;
    int n = 0;

    if (!params->ebx) {
        return -EFAULT;
    }
    {
        const process_t *p;

        for (p = proc_list; p; p = p->next) {
            n++;
        }
    }
    /* Fill every field of the ABI struct explicitly: uptime, the process count,
     * mem_unit, and physical RAM (from the frame bitmap) are real; load average
     * and swap are not tracked, so they are reported as zero. */
    si.uptime = (int32_t)(kernel_ticks / 100);
    si.loads[0] = 0;
    si.loads[1] = 0;
    si.loads[2] = 0;
    si.totalram = frame_total_count() * PAGE_SIZE;
    si.freeram = frame_free_count() * PAGE_SIZE;
    si.sharedram = 0;
    si.bufferram = 0;
    si.totalswap = 0;
    si.freeswap = 0;
    si.procs = (uint16_t)n;
    si.pad = 0;
    si.totalhigh = 0;
    si.freehigh = 0;
    si.mem_unit = 1;
    for (i = 0; i < sizeof(si._f); i++) {
        si._f[i] = 0;
    }
    if (copy_to_user((void __user *)params->ebx, &si, sizeof(si)) != 0) {
        return -EFAULT;
    }
    return 0;
}

/**
 * Record the address the caller's thread end clears and return the caller's
 * thread id (set_tid_address(2)).
 *
 * The address, EBX, becomes `clear_child_tid`; with one thread per process the
 * thread id is the process id.
 */
static int32_t sys_set_tid_address(isr_params_t *params)
{
    if (!current_proc) {
        return -ESRCH;
    }
    current_proc->clear_child_tid = params->ebx;
    return (int32_t)current_proc->pid;
}

/*
 * uname(2): fill the six 65-byte fields of struct utsname (new-style, as musl
 * expects) in user memory. EBX = struct utsname *.
 */
struct kernel_utsname {
    char sysname[65];
    char nodename[65];
    char release[65];
    char version[65];
    char machine[65];
    char domainname[65];
};

/* The mutable host name (uname's nodename, what gethostname reads, sethostname
 * sets). Single global; Rach2 is one host. */
static char k_hostname[65] = "rach2";

/* Copy a C string into a fixed 65-byte utsname field (always
 * NUL-terminated). */
static void uname_field(char *dst, const char *src)
{
    uint32_t n = k_strlen(src);

    if (n > 64) {
        n = 64;
    }
    k_memcpy(dst, src, n);
    dst[n] = '\0';
}

static int32_t sys_uname(isr_params_t *params)
{
    struct kernel_utsname __user *user_uts = (struct kernel_utsname __user *)params->ebx;
    struct kernel_utsname uts;

    k_memset(&uts, 0, sizeof(uts));
    uname_field(uts.sysname, "Rach2");
    uname_field(uts.nodename, k_hostname);
    uname_field(uts.release, "1.0.0");
    uname_field(uts.version, "Rach2 musl port");
    uname_field(uts.machine, "i686");
    uname_field(uts.domainname, "(none)");

    if (copy_to_user(user_uts, &uts, sizeof(uts)) != 0) {
        return -EFAULT;
    }
    return 0;
}

/* sethostname(74): set the single mutable host name (uname's nodename, what
 * gethostname reads back). */
static int32_t sys_sethostname(isr_params_t *params)
{
    const char __user *name = (const char __user *)params->ebx;
    int len = (int)params->ecx;
    char buf[65];

    if (len < 0 || len > 64) {
        return -EINVAL;
    }
    if (len > 0 && copy_from_user(buf, name, (uint32_t)len) != 0) {
        return -EFAULT;
    }
    buf[len] = '\0';
    k_memcpy(k_hostname, buf, (uint32_t)len + 1);
    return 0;
}

/* Name each missing syscall behind `rach2.trace=enosys`, once per number for
 * the process that last asked for it, not once per boot. Per boot hides
 * exactly what such a trace is for: a program that asks for a missing number
 * early would leave a later program asking for the same number saying
 * nothing, and a reader comparing before and after would conclude the later
 * program asked for nothing. Recording which process last reported a number
 * costs one word per number and makes each program's own gaps visible; two
 * programs asking for the same number in turn each say so, since the word
 * holds one of them.
 *
 * A number past the table is remembered as one number and one process, so a
 * program asking for the same one twice says it once and two numbers in turn
 * say it each time; nothing should ever ask for one, and a boot that fills the
 * console with one is telling the truth about a program in a retry loop. Off
 * unless the flag is on the command line, so no other boot sees a line it did
 * not before. */
static int _trace_enosys = 0;

static uint32_t _enosys_last_pid[SYSCALL_TABLE_SIZE];

/* The last number past the table and who asked for it, which is the same
   suppression the array above gives the numbers inside it. */
static uint32_t _enosys_big_num = 0;

static uint32_t _enosys_big_pid = 0xFFFFFFFFu;

/**
 * The syscall trap handler: dispatch on the number in EAX.
 *
 * Looks up _syscall_table[EAX] and calls it, storing the result back in
 * params->eax for popal/iret to return to userspace. Marks the process
 * non-preemptible (in_syscall) around the dispatch and runs the body with
 * interrupts on so the PIT/HDA keep ticking during long syscalls. A switch an
 * interrupt asked for during the call is taken at its exit, with interrupts
 * off again and the trapframe whole, so a process that is always inside a
 * call gives way once per call; a call that does not return does not.
 */
static void _syscall_handler(isr_params_t *params)
{
    uint32_t syscall_num = params->eax;
    /* Default to -ENOSYS so an unimplemented or out-of-range number reports the
     * real "no such syscall" errno; musl relies on -ENOSYS to fall back to an
     * alternate path instead of treating it as a generic -EPERM failure. */
    int32_t ret = -ENOSYS;

    /* Mark this process non-preemptible BEFORE enabling interrupts. The ISR
     * stub keeps IF=0 through entry (segments/%gs are still being saved), so
     * setting in_syscall here and only then doing sti closes the window in
     * which a timer IRQ could reschedule a process whose trapframe is still
     * half-built. The timer handler skips scheduling while in_syscall is set.
     */
    if (current_proc) {
        current_proc->in_syscall = 1;
    }

    /* Enable interrupts for the syscall body so the PIT/HDA keep ticking during
     * long syscalls (e.g. Doom's framebuffer writes); preemption stays disabled
     * via in_syscall above. The stub no longer does sti; iret restores IF. */
    __asm__ __volatile__("sti");

    /* A stick that arrived or was pulled out was learned in an interrupt,
     * where nothing may sleep; reading its partition table and reclaiming
     * what it held both do. This is the first place after that where a
     * process is running, so the work is done here. It returns at once when
     * there is none. */
    usbdisk_service();

    if (syscall_num < SYSCALL_TABLE_SIZE && _syscall_table[syscall_num]) {
        ret = _syscall_table[syscall_num](params);
    } else if (_trace_enosys) {
        /* A missing syscall is otherwise invisible: -ENOSYS goes back in a
         * register and nothing is printed, so a program that fails for want of
         * one fails for no stated reason. */
        uint32_t pid = current_proc ? current_proc->pid : 0;
        const char *who = current_proc ? current_proc->comm : "?";

        if (syscall_num >= SYSCALL_TABLE_SIZE) {
            /* Said once per number per process, as an in-table one is: musl
               asks for `clock_gettime64` before every `clock_gettime`, so a
               program that reads the clock in a loop printed a line a second
               and the console could not be read. */
            if (_enosys_big_num != syscall_num || _enosys_big_pid != pid) {
                _enosys_big_num = syscall_num;
                _enosys_big_pid = pid;
                klog(KLOG_WARN, "syscall",
                     "%d: not implemented, and past the table (asked for by %s)", (int)syscall_num,
                     who);
            }
        } else if (_enosys_last_pid[syscall_num] != pid) {
            _enosys_last_pid[syscall_num] = pid;
            klog(KLOG_WARN, "syscall", "%d: not implemented (asked for by %s)", (int)syscall_num,
                 who);
        }
    }

    /* Disable interrupts again so the epilogue restores the trapframe (segments
     * and %gs) atomically; iret then restores the caller's IF. */
    __asm__ __volatile__("cli");

    if (current_proc) {
        current_proc->in_syscall = 0;
    }
    /* Deliver what this call put on the loopback ring, here at its exit rather
     * than only on the next tick, so a local transfer is not paced at a ring a
     * tick. The threshold is three datagrams, `LOOP_DRAIN_AT`, of the ring's
     * 64: at one or two the delivery is left to the timer, which is what
     * lets the two crossing SYNs of a simultaneous open both be queued
     * before either is delivered. The sender's own frames are gone by here,
     * so no delivery runs under them. Interrupts are off, the drain refuses
     * to run inside itself, and it takes at most four times the ring's slots
     * in one run; what is left is taken by the next call's exit or by the
     * timer. */
    if (ip_loop_crowded()) {
        ip_loop_drain_from(IP_LOOP_FROM_CALL);
    }
    sched_take_preempt();

    /* Store return value in EAX - will be restored by popal */
    params->eax = (uint32_t)ret;
}

void init_syscall(void)
{
    uint32_t i;

    init_kstacks();

    for (i = 0; i < SYSCALL_TABLE_SIZE; i++) {
        _syscall_table[i] = NULL;
    }

    _syscall_table[SYS_EXIT] = sys_exit;
    _syscall_table[SYS_FORK] = sys_fork;
    _syscall_table[SYS_READ] = sys_read;
    _syscall_table[SYS_WRITE] = sys_write;
    _syscall_table[SYS_OPEN] = sys_open;
    _syscall_table[SYS_CLOSE] = sys_close;
    _syscall_table[SYS_WAITPID] = sys_waitpid;
    _syscall_table[SYS_WAIT4] = sys_wait4;
    _syscall_table[SYS_CREAT] = sys_creat;
    _syscall_table[SYS_UNLINK] = sys_unlink;
    _syscall_table[SYS_MKDIR] = sys_mkdir;
    _syscall_table[SYS_RMDIR] = sys_rmdir;
    _syscall_table[SYS_EXECVE] = sys_execve;
    _syscall_table[SYS_CHDIR] = sys_chdir;
    _syscall_table[SYS_BRK] = sys_brk;
    _syscall_table[SYS_MMAP2] = sys_mmap2;
    _syscall_table[SYS_MUNMAP] = sys_munmap;
    _syscall_table[SYS_MPROTECT] = sys_mprotect;
    _syscall_table[SYS_CLONE] = sys_clone;
    _syscall_table[SYS_FUTEX] = sys_futex;
    _syscall_table[SYS_LSEEK] = sys_lseek;
    _syscall_table[SYS_RENAME] = sys_rename;
    _syscall_table[SYS_IOCTL] = sys_ioctl;
    _syscall_table[SYS_DUP] = sys_dup;
    _syscall_table[SYS_DUP2] = sys_dup2;
    _syscall_table[SYS_PIPE] = sys_pipe;
    _syscall_table[SYS_PIPE2] = sys_pipe2;
    _syscall_table[SYS_POLL] = sys_poll;
    _syscall_table[SYS_PPOLL] = sys_ppoll;
    _syscall_table[SYS_SELECT] = sys_select;
    _syscall_table[SYS_NEWSELECT] = sys__newselect;
    _syscall_table[SYS_PSELECT6] = sys_pselect6;
    _syscall_table[SYS_GETTIMEOFDAY] = sys_gettimeofday;
    _syscall_table[SYS_STAT] = sys_stat;
    _syscall_table[SYS_LSTAT] = sys_lstat;
    _syscall_table[SYS_GETCWD] = sys_getcwd;
    _syscall_table[SYS_STAT64] = sys_stat64;
    _syscall_table[SYS_LSTAT64] = sys_lstat64;
    _syscall_table[SYS_FSTAT64] = sys_fstat64;
    _syscall_table[SYS_STATFS64] = sys_statfs64;
    _syscall_table[SYS_FSTATFS64] = sys_fstatfs64;
    _syscall_table[SYS_FSTATAT64] = sys_fstatat64;
    _syscall_table[SYS_OPENAT] = sys_openat;
    _syscall_table[SYS_READLINK] = sys_readlink;
    _syscall_table[SYS_READLINKAT] = sys_readlinkat;
    _syscall_table[SYS_ACCESS] = sys_access;
    _syscall_table[SYS_FACCESSAT] = sys_faccessat;
    _syscall_table[SYS_LLSEEK] = sys__llseek;
    _syscall_table[SYS_FCNTL64] = sys_fcntl64;
    _syscall_table[SYS_GETDENTS64] = sys_getdents64;
    _syscall_table[SYS_UTIMENSAT] = sys_utimensat;
    _syscall_table[SYS_CLOCK_GETTIME] = sys_clock_gettime;
    _syscall_table[SYS_NANOSLEEP] = sys_nanosleep;
    _syscall_table[SYS_SETITIMER] = sys_setitimer;
    _syscall_table[SYS_GETITIMER] = sys_getitimer;
    _syscall_table[SYS_ALARM] = sys_alarm;
    _syscall_table[SYS_GETRANDOM] = sys_getrandom;
    _syscall_table[SYS_KILL] = sys_kill;
    _syscall_table[SYS_TKILL] = sys_tkill;
    _syscall_table[SYS_TGKILL] = sys_tgkill;
    _syscall_table[SYS_RT_SIGACTION] = sys_rt_sigaction;
    _syscall_table[SYS_RT_SIGPROCMASK] = sys_rt_sigprocmask;
    _syscall_table[SYS_RT_SIGRETURN] = sys_rt_sigreturn;
    _syscall_table[SYS_SIGRETURN] = sys_sigreturn;
    /* Identity: the four getters read the PCB credentials. */
    _syscall_table[SYS_GETUID32] = sys_getuid;
    _syscall_table[SYS_GETGID32] = sys_getgid;
    _syscall_table[SYS_GETEUID32] = sys_geteuid;
    _syscall_table[SYS_GETEGID32] = sys_getegid;
    _syscall_table[SYS_GETRESUID32] = sys_getresuid;
    _syscall_table[SYS_GETRESGID32] = sys_getresgid;
    _syscall_table[SYS_GETGROUPS32] = sys_getgroups;
    _syscall_table[SYS_SOCKETCALL] = sys_socketcall;
    _syscall_table[SYS_SET_THREAD_AREA] = sys_set_thread_area;
    _syscall_table[SYS_GETPID] = sys_getpid;
    _syscall_table[SYS_SETSID] = sys_setsid;
    _syscall_table[SYS_SETPGID] = sys_setpgid;
    _syscall_table[SYS_GETPGID] = sys_getpgid;
    _syscall_table[SYS_GETPGRP] = sys_getpgrp;
    _syscall_table[SYS_GETSID] = sys_getsid;
    _syscall_table[SYS_PAUSE] = sys_pause;
    _syscall_table[SYS_RT_SIGSUSPEND] = sys_rt_sigsuspend;
    _syscall_table[SYS_GETPPID] = sys_getppid;
    _syscall_table[SYS_UMASK] = sys_umask;
    _syscall_table[SYS_TIMES] = sys_times;
    _syscall_table[SYS_SYSINFO] = sys_sysinfo;
    _syscall_table[SYS_VFORK] = sys_fork; /* conservative alias */
    _syscall_table[SYS_READV] = sys_readv;
    _syscall_table[SYS_WRITEV] = sys_writev;
    _syscall_table[SYS_UNAME] = sys_uname;
    _syscall_table[SYS_SETHOSTNAME] = sys_sethostname;
    _syscall_table[SYS_SETUID32] = sys_setuid;
    _syscall_table[SYS_SETGID32] = sys_setgid;
    _syscall_table[SYS_SETGROUPS32] = sys_setgroups;
    _syscall_table[SYS_GETRLIMIT] = sys_getrlimit;
    _syscall_table[SYS_SETRLIMIT] = sys_setrlimit;
    _syscall_table[SYS_UGETRLIMIT] = sys_getrlimit;
    _syscall_table[SYS_PRLIMIT64] = sys_prlimit64;
    _syscall_table[SYS_DUP3] = sys_dup3;
    _syscall_table[SYS_SYNC] = sys_sync;
    _syscall_table[SYS_REBOOT] = sys_reboot;
    _syscall_table[SYS_FSYNC] = sys_fsync;
    _syscall_table[SYS_FTRUNCATE] = sys_ftruncate;
    _syscall_table[SYS_FTRUNCATE64] = sys_ftruncate64;
    _syscall_table[SYS_MOUNT] = sys_mount;
    _syscall_table[SYS_UMOUNT2] = sys_umount2;
    _syscall_table[SYS_FDATASYNC] = sys_fsync;
    _syscall_table[SYS_LINK] = sys_link;
    _syscall_table[SYS_LINKAT] = sys_linkat;
    _syscall_table[SYS_SYMLINK] = sys_symlink;
    _syscall_table[SYS_SYMLINKAT] = sys_symlinkat;
    /* Of the set-id family only the *32 forms are wired: musl remaps the whole
     * legacy 16-bit family to them on i386, and the 16-bit ABI's narrower ids
     * and 0xFFFF keep-sentinel differ, so serving those numbers with 32-bit
     * semantics would lie to a raw caller. They fall to ENOSYS instead. The
     * ownership calls below are wired under both numbers, since `chown` takes
     * its ids the same way in each. */
    _syscall_table[SYS_SETREUID32] = sys_setreuid;
    _syscall_table[SYS_SETREGID32] = sys_setregid;
    _syscall_table[SYS_SETRESUID32] = sys_setresuid;
    _syscall_table[SYS_SETRESGID32] = sys_setresgid;
    _syscall_table[SYS_CHMOD] = sys_chmod;
    _syscall_table[SYS_FCHMODAT] = sys_fchmodat;
    _syscall_table[SYS_FCHMOD] = sys_fchmod;
    _syscall_table[SYS_CHOWN] = sys_chown;
    _syscall_table[SYS_CHOWN32] = sys_chown;
    _syscall_table[SYS_LCHOWN] = sys_chown;
    _syscall_table[SYS_LCHOWN32] = sys_chown;
    _syscall_table[SYS_FCHOWNAT] = sys_fchownat;
    _syscall_table[SYS_FCHOWN] = sys_fchown;
    _syscall_table[SYS_FCHOWN32] = sys_fchown;
    _syscall_table[SYS_SET_TID_ADDRESS] = sys_set_tid_address;
    /* musl's exit() issues exit_group(); for a single-threaded process it is
     * exactly _exit(). */
    _syscall_table[SYS_EXIT_GROUP] = sys_exit;

    _trace_enosys = kcmdline_has("rach2.trace=enosys");

    register_isr_handler(SYSCALL_VECTOR, _syscall_handler, "syscall");
}
