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
#include "tables/gdt.h"
#include "lib/krandom.h"
#include "lib/kerrno.h"
#include "lib/ksignal.h"
#include "fs/fd.h"
#include "fs/file.h"
#include "ipc/pipe.h"
#include "io/poll.h"
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
#include "network/netconf.h" /* net_ip_addr for getsockname */
#include "cmdline.h"         /* kcmdline_has, for the ENOSYS trace flag */
#include "syscall_internal.h"

/* Only one process owns the framebuffer at a time; when it exits or closes fb,
 * text mode is restored. */
volatile uint32_t fb_owner_pid = 0;

/*
 * ioctl request codes (Linux compatible subset)
 */
#define KIOCSOUND     0x4B2F /* Set PC speaker frequency (0 = off) */

#define TCGETS        0x5401 /* Get terminal attributes (termios) */

#define TCSETS        0x5402 /* Set terminal attributes now */

#define TCSETSW       0x5403 /* Set after the output drains */

#define TCSETSF       0x5404 /* Set after output drains, flush input */

#define TCFLSH        0x540B /* Flush input and/or output queues */

#define FIONREAD      0x541B /* Bytes available to read */

#define TIOCGWINSZ    0x5413 /* Get terminal window size */

#define TIOCSWINSZ    0x5414 /* Set terminal window size */

#define TIOCSCTTY     0x540E /* Make this tty the controlling terminal */

#define TIOCGPGRP     0x540F /* Get the foreground process group */

#define TIOCSPGRP     0x5410 /* Set the foreground process group */

#define TIOCNOTTY     0x5422 /* Detach from the controlling terminal */

#define FB_MODE_13H   0x4601 /* Switch to VGA Mode 13h */

#define FB_MODE_TEXT  0x4602 /* Switch back to text mode */

#define FB_SETPALETTE 0x4603 /* Set 256-color palette (768 bytes) */

/*
 * Audio ioctl request codes
 */
#define AUDIO_GET_INFO   0x4101 /* Get audio driver info */

#define AUDIO_START      0x4102 /* Start audio playback */

#define AUDIO_STOP       0x4103 /* Stop audio playback */

#define AUDIO_WRITE      0x4104 /* Write audio samples */

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
#define REBOOT_MAGIC1  0xfee1deadu

#define REBOOT_MAGIC2  672274793u /* 0x28121969 */

#define REBOOT_MAGIC2A 85072278u  /* 0x05121996 */

#define REBOOT_MAGIC2B 369367448u /* 0x16041998 */

#define REBOOT_MAGIC2C 537993216u /* 0x20112000 */

/* The commands. Two of them are negative read as a signed int, which is why
 * everything here compares raw 32-bit values. */
#define REBOOT_CMD_RESTART   0x01234567u

#define REBOOT_CMD_HALT      0xcdef0123u

#define REBOOT_CMD_POWER_OFF 0x4321fedcu

#define REBOOT_CMD_CAD_ON    0x89abcdefu

#define REBOOT_CMD_CAD_OFF   0x00000000u

/**
 * reboot(2): stop the machine. EBX = magic1, ECX = magic2, EDX = command.
 *
 * Does not return for the three commands that stop the machine. Returns
 * `-EPERM` for an unprivileged caller, `-EINVAL` for a wrong magic or an
 * unknown command, and 0 for `RB_DISABLE_CAD`.
 *
 * The privilege check comes before the magics, which inverts the order the rest
 * of this file uses, and Linux inverts it the same way. A magic-first handler
 * would let any uid learn the ABI of the one call that stops the machine and
 * erases the evidence of what it was doing; nothing is learned from `-EPERM`.
 *
 * `RB_DISABLE_CAD` returning 0 is what makes the rest testable. Every command
 * that works stops the machine, so without one that succeeds quietly a caller
 * cannot tell an accepted magic from a rejected one: both would answer
 * `-EINVAL`. It asks for a state this kernel is permanently in, since there is
 * no keyboard reboot to disable, so 0 is a true answer. `RB_ENABLE_CAD` asks
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
int32_t raw_seek_resolve(uint64_t cur, uint64_t size, long long delta, int whence,
                                uint64_t *out)
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

    /* -(delta) is undefined when delta is the most negative value, so the
       magnitude is built from delta + 1, which is always representable. */
    mag = (uint64_t)(-(delta + 1)) + 1u;
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

    /* Build the result in a kernel struct, then copy it out (this also closes a
     * previously unchecked raw write to the user pointer). */
    {
        int64_t seconds;
        int64_t nanoseconds;

        get_current_time(&seconds, &nanoseconds);
        ktv.tv_sec = (int32_t)seconds;
        /* Narrowed before the divide: a 64-bit division would call __divdi3,
         * and this kernel links no libgcc. A nanosecond field is under 1e9. */
        ktv.tv_usec = (int32_t)nanoseconds / 1000;
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
 * does not provide, so times are 32-bit here and this ABI ends in 2038.
 */
static int32_t sys_clock_gettime(isr_params_t *params)
{
    int clk = (int)params->ebx;
    struct kernel_timespec __user *ts = (struct kernel_timespec __user *)params->ecx;
    struct kernel_timespec kts;
    uint32_t ticks;

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
        ticks = kernel_ticks;
        kts.tv_sec = (int32_t)(ticks / 100);
        kts.tv_nsec = (int32_t)((ticks % 100) * 10000000); /* 1e7 ns per tick */
    }

    if (copy_to_user(ts, &kts, sizeof(kts)) != 0) {
        return -EFAULT;
    }
    return 0;
}

/**
 * Block-and-schedule sleep (nanosleep, syscall 162, 32-bit timespec).
 *
 * EBX = const struct timespec* req, ECX = struct timespec* rem (optional).
 * Rounds up to whole 10 ms ticks, then parks the process BLOCKED on its
 * wakeup_tick and yields; the timer wakes it at the deadline while OTHER
 * processes keep running. rem is written as zero on normal completion, or
 * the time left on -EINTR.
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
     * sub-second nanoseconds. Absurd durations are clamped so sec*100 cannot
     * overflow and the signed tick comparison below stays valid. */
    {
        uint32_t sec = (uint32_t)req.tv_sec;
        uint32_t nsec_ticks = ((uint32_t)req.tv_nsec + 9999999u) / 10000000u;
        if (sec > 21474836u) {
            sec = 21474836u; /* keeps the intermediate sec*100 below 2^31 */
        }
        ticks = sec * 100u + nsec_ticks;
        if (ticks > 0x7FFFFFFFu) {
            ticks = 0x7FFFFFFFu; /* keep the signed (target-now) delta valid */
        }
    }
    if (ticks == 0 && (req.tv_sec != 0 || req.tv_nsec != 0)) {
        ticks = 1; /* any nonzero request waits at least one tick */
    }

    target = kernel_ticks + ticks;
    if (target == 0) {
        target = 1; /* 0 is the "not sleeping" sentinel; never store it as a deadline */
    }
    while ((int32_t)(target - kernel_ticks) > 0) {
        current_proc->wakeup_tick = target;
        current_proc->state = PROC_BLOCKED;
        schedule(); /* yield to any other READY process; the timer wakes us */
        /* A signal cuts the sleep short: stop sleeping, report the time left,
         * and return -EINTR so it is acted on when the syscall returns to user.
         */
        if (signal_pending()) {
            uint32_t left = ((int32_t)(target - kernel_ticks) > 0) ? (target - kernel_ticks) : 0u;
            current_proc->wakeup_tick = 0;
            current_proc->state = PROC_RUNNING;
            if (urem) {
                struct kernel_timespec rem;
                rem.tv_sec = (int32_t)(left / 100u);
                rem.tv_nsec = (int32_t)((left % 100u) * 10000000u);
                (void)copy_to_user(urem, &rem, sizeof(rem));
            }
            return -EINTR;
        }
        if ((int32_t)(target - kernel_ticks) > 0) {
            /* schedule() found nobody else to run: idle until the next
             * interrupt (the tick), not busy-spinning on the scheduler.
             */
            idle_wait_irq_on();
        }
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

/* getrandom flag bits (Linux). We never block and have one source, so they only
 * gate validation here. */
#define GRND_NONBLOCK 0x0001u

#define GRND_RANDOM   0x0002u

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
        switch (request) {
        case FB_SETPALETTE:
            if (arg != 0) {
                /* 256 RGB triples = 768 bytes read straight from user. */
                if (!user_buffer_ok(arg, 768, 0)) {
                    return -EFAULT;
                }
                vga_mode13h_set_palette((const uint8_t *)arg);
                return 0;
            }
            return -EFAULT;

        case FB_MODE_13H:
            return 0; /* already in mode 13h while fb is open */

        case FB_MODE_TEXT:
            vga_mode13h_exit();
            fb_owner_pid = 0;
            return 0;

        default:
            return -ENOTTY;
        }
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
 * Return the caller's tid, ignoring the clear-child-tid pointer
 * (set_tid_address(2)).
 *
 * Rach2 has no threads, so this only returns the caller's PID as its thread id;
 * the pointer (EBX) is accepted and ignored.
 */
static int32_t sys_set_tid_address(isr_params_t *params)
{
    (void)params;

    if (!current_proc) {
        return -ESRCH;
    }
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

/* Name each missing syscall behind `rach2.trace=enosys`, once per number PER
 * PROCESS, not once per boot. Per boot was the first shape and it hides
 * exactly what such a trace is for: `login` asks for setitimer early, so a
 * later program asking for the same number would say nothing, and a reader
 * comparing before and after would conclude the later program asked for
 * nothing. Recording which process last reported a number costs one word per
 * number and makes each program's own gaps visible.
 *
 * A number the table cannot hold is printed every time, since there is nowhere
 * to remember it; nothing should ever ask for one, and a boot that fills the
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
     * tick. The threshold is three datagrams of the ring's 48: at one or two
     * the delivery is left to the timer, which is what lets the two crossing
     * SYNs of a simultaneous open both be queued before either is delivered.
     * The sender's own frames are gone by here, so no delivery runs under
     * them. Interrupts are off, the drain refuses to run inside itself, and it
     * takes at most 192 datagrams; what is left is taken by the next call's
     * exit or by the timer. */
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
    /* Only the *32 id forms are wired: musl remaps the whole legacy 16-bit
     * family to them on i386, and the 16-bit ABI's narrower ids and 0xFFFF
     * keep-sentinel differ, so serving those numbers with 32-bit semantics
     * would lie to a raw caller. They fall to ENOSYS instead. */
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
