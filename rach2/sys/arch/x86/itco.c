/* Intel TCO version 2 watchdog and Linux's /dev/watchdog interface. */
#include "../../idle_timer.h"
#include "itco.h"
#include "../../lib/kint.h"
#include "../../lib/kerrno.h"
#include "../../lib/kdef.h"

/* Linux 6.12, drivers/watchdog/iTCO_wdt.c: offsets from PMBASE + 0x60. */
#define TCO_RLD           0x00
#define TCO1_STS          0x04
#define TCO2_STS          0x06
#define TCO1_CNT          0x08
#define TCO_TMR           0x12
#define TCO_HALT          0x0800u
#define TCO_NO_REBOOT     0x20u
#define TCO_MASK          0x03FFu
/* SMI_EN at PMBASE + 0x30, and its TCO_EN, bit 13, through which the first
 * stage's expiry raises an SMI to the firmware (iTCO_wdt.c:541). */
#define SMI_EN            0x30u
#define SMI_TCO_EN        0x2000u
/* TCO2_STS's SECOND_TO_STS, bit 1, which the chipset sets on the second
 * expiry, whether or not NO_REBOOT kept that expiry from resetting the
 * machine, and keeps across a reset (Intel 8-series PCH datasheet, 12.9.5). */
#define TCO_SECOND_TO_STS 0x0002u

/* Linux's WATCHDOG_TIMEOUT, iTCO_wdt.c:118: the heartbeat when none is
 * given or the one given is out of range. */
#define ITCO_DEFAULT_TIMEOUT 30

/* All callbacks are synchronous and do not sleep; the big lock serializes
 * the device, and the tick's half of the feed (`itco_feed_tick`) reads only
 * words that one store changes. The host test supplies the same boundary
 * over a TCO model. */
struct itco {
    uint16_t (*rd)(void *ctx, unsigned reg);
    void (*wr)(void *ctx, unsigned reg, uint16_t value);
    uint32_t (*gcs_rd)(void *ctx);
    void (*gcs_wr)(void *ctx, uint32_t value);
    void *ctx;
    int timeout; /* requested seconds, as Linux reports GETTIMEOUT */
    int opened;
    int magic; /* the last nonempty write contained V */
    /* started by a program and not since stopped: Linux's WDOG_ACTIVE */
    int running;
    int pinged;         /* started or fed since the last GETSTATUS */
    int hw_running;     /* counting toward a reset: Linux's WDOG_HW_RUNNING */
    int nowayout;       /* a stop is refused: Linux's WDOG_NO_WAY_OUT */
    int boot_feed;      /* the kernel feeds a running timer no program holds:
                           Linux's watchdog.handle_boot_enabled */
    uint32_t hz;        /* countdown units a second */
    uint32_t feed_left; /* units before the next feed tick */
    int open_limited;   /* a program must open before open_left expires */
    uint32_t open_left; /* Linux's open_deadline, counted down */
    /* the tick leaves each feed to the thread watchdogd: set while the
     * thread exists and the scheduler runs it */
    int threaded;
    volatile int feed_due; /* set by the tick, cleared by `itco_feed_work` */
    /* TCO1_STS and TCO2_STS as the attach found them, before it cleared
     * them; logged at boot */
    uint16_t boot_sts1, boot_sts2;
};

/** Set NO_REBOOT to `on`; 0 or -EIO when firmware refuses the change. */
static int itco_noreboot(struct itco *t, int on)
{
    uint32_t value = t->gcs_rd(t->ctx);

    if (value == 0xFFFFFFFFu) {
        return -EIO;
    }
    value = on ? value | TCO_NO_REBOOT : value & ~TCO_NO_REBOOT;
    t->gcs_wr(t->ctx, value);
    return t->gcs_rd(t->ctx) == value ? 0 : -EIO;
}

/** Halt the timer, then inhibit resets; 0 or -EIO if halt did not stick.
 * Either way NO_REBOOT is asked for, so the timer no longer runs toward a
 * reset unless firmware refuses both. */
static int itco_stop(struct itco *t)
{
    uint16_t value = t->rd(t->ctx, TCO1_CNT);

    t->wr(t->ctx, TCO1_CNT, (uint16_t)(value | TCO_HALT));
    value = t->rd(t->ctx, TCO1_CNT);
    if (itco_noreboot(t, 1) == 0 || (value != 0xFFFFu && (value & TCO_HALT))) {
        t->hw_running = 0;
    }
    return value != 0xFFFFu && (value & TCO_HALT) ? 0 : -EIO;
}

/** Reload both expiry stages, as a version 2 TCO_RLD write does. */
static void itco_ping(struct itco *t)
{
    t->wr(t->ctx, TCO_RLD, 1);
}

/** Enable resets, reload, and clear halt; 0 or -EIO, leaving halt on error. */
static int itco_start(struct itco *t)
{
    uint16_t value;

    if (itco_noreboot(t, 0) != 0) {
        return -EIO;
    }
    itco_ping(t);
    value = t->rd(t->ctx, TCO1_CNT);
    t->wr(t->ctx, TCO1_CNT, (uint16_t)(value & ~TCO_HALT));
    value = t->rd(t->ctx, TCO1_CNT);
    if (value == 0xFFFFu || (value & TCO_HALT)) {
        itco_stop(t);
        return -EIO;
    }
    t->hw_running = 1;
    return 0;
}

/** Program Linux's 3..614 seconds in 0.6 s ticks, preserving reserved bits;
 * 0, or -EINVAL for the range or a register that refuses the write, as
 * Linux's iTCO_wdt_set_timeout returns for both. */
static int itco_timeout(struct itco *t, int seconds)
{
    uint16_t value, ticks;

    if (seconds < 3 || seconds > 614) {
        return -EINVAL;
    }
    ticks = (uint16_t)((unsigned)seconds * 10u / 6u);
    value = t->rd(t->ctx, TCO_TMR);
    t->wr(t->ctx, TCO_TMR, (uint16_t)((value & ~TCO_MASK) | ticks));
    if ((t->rd(t->ctx, TCO_TMR) & TCO_MASK) != ticks) {
        return -EINVAL;
    }
    t->timeout = seconds;
    return 0;
}

/** Seconds in the current countdown, truncated as Linux GETTIMELEFT does. */
static int itco_timeleft(struct itco *t)
{
    return (int)((unsigned)(t->rd(t->ctx, TCO_RLD) & TCO_MASK) * 6u / 10u);
}

/**
 * Check the register block, clear NO_REBOOT, keep the old status and clear it,
 * and set the timeout to `heartbeat` seconds, or to 30
 * when the chip refuses it, as iTCO_wdt_probe does (Linux 6.12, iTCO_wdt.c:534
 * to 612); 0, 1 when 30 was taken in place of `heartbeat`, -ENODEV when
 * NO_REBOOT will not clear, as Linux refuses the device there (iTCO_wdt.c:534
 * to 539), or -EIO. A timer that is not halted is running whatever NO_REBOOT
 * was, as iTCO_wdt_set_running (iTCO_wdt.c:444 to 453) finds it: it is left
 * counting, now toward a reset, for the kernel to feed. A halted one gets
 * NO_REBOOT again (iTCO_wdt.c:593 to 599). The status words are read into
 * `boot_sts1` and `boot_sts2` before the clears that iTCO_wdt_probe makes
 * (iTCO_wdt.c:545 to 563), which Linux makes unread. All writes are bounded
 * register accesses, with no wait loop.
 */
static int itco_attach(struct itco *t, int heartbeat)
{
    uint16_t cnt = t->rd(t->ctx, TCO1_CNT);

    if (cnt == 0xFFFFu || t->rd(t->ctx, TCO_TMR) == 0xFFFFu || t->gcs_rd(t->ctx) == 0xFFFFFFFFu) {
        return -EIO;
    }
    if (itco_noreboot(t, 0) != 0) {
        return -ENODEV;
    }
    t->boot_sts1 = t->rd(t->ctx, TCO1_STS);
    t->boot_sts2 = t->rd(t->ctx, TCO2_STS);
    t->wr(t->ctx, TCO1_STS, 8);
    t->wr(t->ctx, TCO2_STS, 2);
    t->wr(t->ctx, TCO2_STS, 4);
    t->hw_running = !(cnt & TCO_HALT);
    if (!t->hw_running && itco_stop(t) != 0) {
        return -EIO;
    }
    if (itco_timeout(t, heartbeat) == 0) {
        return 0;
    }
    return itco_timeout(t, ITCO_DEFAULT_TIMEOUT) ? -EIO : 1;
}

/**
 * Start the timer for the kernel to feed, as firmware that leaves it
 * running does, and feed it at the next call of `itco_feed_tick`; 0 or the
 * start error. Until a program opens the device, the kernel feeds it, so
 * that a kernel whose tick stops lets the chipset reset the machine.
 */
static int itco_boot_arm(struct itco *t)
{
    int result = t->hw_running ? 0 : itco_start(t);

    t->feed_left = 0;
    return result;
}

/** Whether the kernel's feed is for `t` now: a running timer that no
 * program holds, before the open deadline, as watchdog_need_worker and
 * watchdog_worker_should_ping decide (Linux 6.12, watchdog_dev.c:76 to 97
 * and 204 to 215). */
static int itco_feeding(const struct itco *t)
{
    return !t->running && t->hw_running && t->boot_feed && !(t->open_limited && t->open_left == 0);
}

/**
 * Advance the kernel's feed by `passed` units at `hz` units a second,
 * for a running timer that no program holds,
 * as watchdog_dev.c's hrtimer does (watchdog_next_keepalive and
 * watchdog_timer_expired, lines 99 to 127 and 229 to 237): a feed every
 * half timeout until a program opens the device or the open deadline
 * passes, the last one a timeout before the deadline. A feed that falls due
 * is left to `itco_feed_work` in the thread watchdogd, as the hrtimer
 * queues watchdog_ping_work on the kthread worker of that name, so that a
 * kernel whose tick runs but whose threads no longer do is reset too; while
 * `threaded` is clear, during boot and in the kernel debug shell, where
 * the scheduler runs no thread, the tick feeds the timer itself. From the
 * boot processor's tick, with interrupts off.
 */
static void itco_feed_tick(struct itco *t, uint32_t passed)
{
    uint32_t heartbeat;

    if (!itco_feeding(t) || passed == 0) {
        return;
    }
    if (t->open_limited) {
        t->open_left = passed < t->open_left ? t->open_left - passed : 0;
        if (t->open_left == 0) {
            return;
        }
    }
    if (t->feed_left >= passed) {
        t->feed_left -= passed;
        return;
    }
    if (t->threaded) {
        t->feed_due = 1;
    } else {
        t->feed_due = 0;
        itco_ping(t);
    }
    heartbeat = (uint32_t)t->timeout * t->hz;
    t->feed_left = heartbeat / 2u;
    if (t->open_limited && t->open_left < heartbeat + t->feed_left) {
        t->feed_left = t->open_left > heartbeat ? t->open_left - heartbeat : t->open_left;
    }
}

/** The feed the tick left due, as watchdog_ping_work does it (Linux 6.12,
 * watchdog_dev.c:217 to 227): the timer is fed only if the kernel's feed is
 * still for it, since a program may have opened the device meanwhile. In
 * watchdogd, under the big lock. */
static void itco_feed_work(struct itco *t)
{
    if (!t->feed_due) {
        return;
    }
    t->feed_due = 0;
    if (itco_feeding(t)) {
        itco_ping(t);
    }
}

/** Start a halted timer, as Linux's watchdog_start does: a running one is
 * left alone. 0 or the start error, with the timer halted. */
static int itco_arm(struct itco *t)
{
    int result;

    if (t->running) {
        return 0;
    }
    result = itco_start(t);
    if (result == 0) {
        t->running = 1;
        t->pinged = 1;
    }
    return result;
}

/** Halt a running timer, as Linux's watchdog_stop does; 0, -EBUSY under
 * nowayout (watchdog_dev.c:299), or -EIO. */
static int itco_disarm(struct itco *t)
{
    if (!t->running) {
        return 0;
    }
    if (t->nowayout) {
        return -EBUSY;
    }
    if (itco_stop(t) != 0) {
        return -EIO;
    }
    t->running = 0;
    return 0;
}

/** Feed a running timer, as Linux's watchdog_ping does; a halted timer
 * is not touched. */
static void itco_keepalive(struct itco *t)
{
    if (t->running) {
        itco_ping(t);
        t->pinged = 1;
    }
}

/** Linux's watchdog_get_status for a driver without a status callback:
 * MAGICCLOSE while a magic close is pending, KEEPALIVEPING if started or
 * fed since the last call, which clears it. */
static int itco_status(struct itco *t)
{
    int status = (t->magic ? 0x100 : 0) | (t->pinged ? 0x8000 : 0);

    t->pinged = 0;
    return status;
}

#ifndef RACH2_HOSTTEST
#include "board.h"
#include "portio.h"
#include "../../fs/devfs.h"
#include "../../fs/file.h"
#include "../../mm/paging.h"
#include "../../mm/usercopy.h"
#include "../../boot.h"
#include "../../cmdline.h"
#include "../../klog.h"
#include "../../lib/kirq.h"
#include "../../lib/kstd.h"
#include "../../proc/process.h"
#include "../../proc/sched.h"
#include "../../proc/wait_queue.h"
#include "../../timer.h"
#include "../arch.h"

static struct itco watchdog;
static uint32_t feed_seen; /* kernel_ticks at the last countdown advance */
static uint16_t tco_base;
static uintptr_t gcs_address;
static int registered;
/* watchdogd, which sleeps on `feed_wq` until the tick leaves it a feed */
static process_t *feeder;
static wait_queue_t feed_wq;

/** Read a TCO word at `reg` in the validated I/O window. */
static uint16_t itco_rd(void *ctx, unsigned reg)
{
    (void)ctx;
    return inw((uint16_t)(tco_base + reg));
}

/** Write a TCO word at `reg` in the validated I/O window. */
static void itco_wr(void *ctx, unsigned reg, uint16_t value)
{
    (void)ctx;
    outw((uint16_t)(tco_base + reg), value);
}

/** Read the mapped root-complex GCS register. */
static uint32_t itco_gcs_rd(void *ctx)
{
    (void)ctx;
    return *(volatile uint32_t *)gcs_address;
}

/** Write the mapped root-complex GCS register; the caller reads it back. */
static void itco_gcs_wr(void *ctx, uint32_t value)
{
    (void)ctx;
    *(volatile uint32_t *)gcs_address = value;
}

/** Arm on an exclusive open; 0, -EBUSY, or the start error. */
static int32_t itco_open(struct devfs_node *node, fd_entry_t *e, int flags)
{
    int result;

    (void)node;
    (void)e;
    (void)flags;
    if (watchdog.opened) {
        return -EBUSY;
    }
    result = itco_arm(&watchdog);
    if (result == 0) {
        watchdog.opened = 1;
        watchdog.magic = 0;
        /* watchdog_dev.c:904: the open deadline is for the first open. */
        watchdog.open_limited = 0;
    }
    return result;
}

/** Feed a nonempty write and remember its magic close byte; count or
 * -EFAULT. A zero-length write neither feeds nor changes magic close. */
static int32_t itco_write(fd_entry_t *e, const char __user *buf, uint32_t count)
{
    uint32_t done, take, i;
    char bytes[128];

    (void)e;
    if (!count) {
        return 0;
    }
    watchdog.magic = 0;
    for (done = 0; done < count; done += take) {
        take = count - done;
        if (take > sizeof(bytes)) {
            take = sizeof(bytes);
        }
        if (copy_from_user(bytes, buf + done, take) != 0) {
            return -EFAULT;
        }
        for (i = 0; i < take; i++) {
            if (bytes[i] == 'V') {
                watchdog.magic = 1;
            }
        }
    }
    itco_keepalive(&watchdog);
    return (int32_t)count;
}

/** Stop a running timer on magic close, else feed it once and leave it
 * armed, as Linux's watchdog_release does; a halted timer is left alone. */
static void itco_close(fd_entry_t *e)
{
    (void)e;
    if (watchdog.running && (!watchdog.magic || itco_disarm(&watchdog) != 0)) {
        itco_keepalive(&watchdog);
        klog(KLOG_WARN, "watchdog", "close: did not stop: %s",
             !watchdog.magic     ? "no magic close"
             : watchdog.nowayout ? "nowayout"
                                 : "the timer refused halt");
    }
    watchdog.opened = 0;
    watchdog.magic = 0;
}

/** Linux watchdog ioctls, whose structures contain only 32-bit integers
 * and bytes on both ABIs; 0, -ENOTTY, -EFAULT, or a hardware/range error.
 * SETOPTIONS without either flag fails with -ENOTTY, as Linux's ioctl
 * returns -ENOIOCTLCMD there and the VFS turns it into -ENOTTY. */
static int32_t itco_ioctl(fd_entry_t *e, uint32_t request, uintptr_t arg)
{
    static const struct {
        uint32_t options, firmware;
        char identity[32];
    } info = {0x8180u, 2, "iTCO_wdt"};
    int value = 0, result;

    (void)e;
    switch (request) {
    case 0x80285700u: /* WDIOC_GETSUPPORT */
        return copy_to_user((void __user *)arg, &info, sizeof(info)) ? -EFAULT : 0;
    case 0x80045701u: /* WDIOC_GETSTATUS */
        value = itco_status(&watchdog);
        break;
    case 0x80045702u: /* WDIOC_GETBOOTSTATUS: 0, as Linux's iTCO_wdt.c:568 */
        break;
    case 0x80045704u: /* WDIOC_SETOPTIONS */
    case 0xC0045706u: /* WDIOC_SETTIMEOUT */
        if (copy_from_user(&value, (const void __user *)arg, sizeof(value))) {
            return -EFAULT;
        }
        if (request == 0x80045704u) {
            result = -ENOTTY;
            if (value & 1) {
                result = itco_disarm(&watchdog);
                if (result) {
                    return result;
                }
            }
            if (value & 2) {
                result = itco_arm(&watchdog);
            }
            return result;
        }
        result = itco_timeout(&watchdog, value);
        if (result) {
            return result;
        }
        itco_keepalive(&watchdog);
        value = watchdog.timeout;
        break;
    case 0x80045705u: /* WDIOC_KEEPALIVE */
        itco_keepalive(&watchdog);
        return 0;
    case 0x80045707u: /* WDIOC_GETTIMEOUT */
        value = watchdog.timeout;
        break;
    case 0x8004570Au: /* WDIOC_GETTIMELEFT */
        value = itco_timeleft(&watchdog);
        break;
    default:
        return -ENOTTY;
    }
    return copy_to_user((void __user *)arg, &value, sizeof(value)) ? -EFAULT : 0;
}

static const struct file_ops itco_ops = {NULL,       itco_write, NULL, itco_ioctl,
                                         itco_close, NULL,       NULL, 0};
static struct devfs_node itco_node = {"watchdog", 10, 130, 0600, &itco_ops, itco_open, NULL, 0};

/**
 * The number option `key`: `dflt` when the line lacks it, else the last of
 * its values that is a number in [`lo`, `hi`], with a record for each that
 * is not and for each bare `key`, which has no value. Linux's parse_args
 * applies each occurrence in order (Linux 6.12, kernel/params.c:117 to 158
 * and 161 to 212), refuses a bare one for a parameter that takes a value
 * (:136-138) and logs each refusal (:203-205), and one refused leaves the
 * value as it was.
 */
static long itco_option(const char *key, long dflt, long lo, long hi)
{
    char text[BOOT_CMDLINE_MAX];
    long long v;
    long value = dflt;
    int i, n = kcmdline_count(key);

    for (i = 0; i < n; i++) {
        if (!kcmdline_value_nth(key, i, text, sizeof(text)) ||
            k_strtoll(text, sizeof(text), &v) != 0 || v < lo || v > hi) {
            klog(KLOG_WARN, "watchdog", "refused: %s=%s", key, text);
            continue;
        }
        value = (long)v;
    }
    for (i = kcmdline_bare_count(key); i > 0; i--) {
        klog(KLOG_WARN, "watchdog", "refused: %s", key);
    }
    return value;
}

/**
 * The boolean option `key`, read as `kcmdline_param_bool` reads it, a bare
 * `key` true, with a record for each value that is no boolean, as Linux's
 * parse_args logs a value param_set_bool refuses (Linux 6.12,
 * kernel/params.c:203-205, 312-319), which leaves the option as it was.
 */
static int itco_bool(const char *key, int dflt)
{
    char text[BOOT_CMDLINE_MAX];
    int i, v, n = kcmdline_count(key);

    for (i = 0; i < n; i++) {
        if (!kcmdline_value_nth(key, i, text, sizeof(text)) || kcmdline_parse_bool(text, &v) != 0) {
            klog(KLOG_WARN, "watchdog", "refused: %s=%s", key, text);
        }
    }
    return kcmdline_param_bool(key, dflt);
}

/**
 * Clear TCO_EN in SMI_EN at `pm` + 0x30 when the line's
 * `iTCO_wdt.turn_SMI_watchdog_clear_off` is at least the TCO's version, 2,
 * as iTCO_wdt_probe does (iTCO_wdt.c:131 and 541); Linux's default, 1,
 * leaves the firmware's SMI on the first stage's expiry, which on the
 * Lenovo leaves the machine down instead of reset.
 */
static void itco_smi_option(uint16_t pm)
{
    uint32_t value;

    if (itco_option("iTCO_wdt.turn_SMI_watchdog_clear_off", 1, -0x7fffffffL, 0x7fffffffL) < 2) {
        return;
    }
    value = inl((uint16_t)(pm + SMI_EN));
    outl((uint16_t)(pm + SMI_EN), value & ~SMI_TCO_EN);
    klog(KLOG_INFO, "watchdog", "SMI_EN: TCO_EN cleared, was %d", (value & SMI_TCO_EN) != 0);
}

/**
 * Read Linux's options, iTCO_wdt's `heartbeat` and `nowayout` (iTCO_wdt.c:
 * 118 to 127) and the watchdog core's `handle_boot_enabled` and
 * `open_timeout` (watchdog_dev.c:1319 to 1325), and `rach2.itco.arm`, which
 * starts the timer at boot for the kernel to feed.
 */
static void itco_options(int *heartbeat, int *arm)
{
    long open_timeout = itco_option("watchdog.open_timeout", 0, 0, 4000000L);

    *heartbeat =
        (int)itco_option("iTCO_wdt.heartbeat", ITCO_DEFAULT_TIMEOUT, -0x7fffffffL, 0x7fffffffL);
    *arm = kcmdline_has("rach2.itco.arm");
    watchdog.nowayout = itco_bool("iTCO_wdt.nowayout", 0);
    watchdog.boot_feed = itco_bool("watchdog.handle_boot_enabled", 1);
    watchdog.hz = TICK_HZ;
    watchdog.open_limited = open_timeout != 0;
    watchdog.open_left = (uint32_t)open_timeout * TICK_HZ;
}

/**
 * Log the status words the attach found, and that the second stage expired
 * when SECOND_TO_STS was set. The bit does not say what reset the machine,
 * since the second expiry sets it under NO_REBOOT too, so the log names no
 * cause and WDIOC_GETBOOTSTATUS stays 0.
 */
static void itco_boot_log(const struct itco *t)
{
    klog(KLOG_INFO, "watchdog", "boot: TCO1_STS %X, TCO2_STS %X%s", t->boot_sts1, t->boot_sts2,
         t->boot_sts2 & TCO_SECOND_TO_STS
             ? ": the TCO timer's second stage expired before this boot"
             : "");
}

void itco_init(void)
{
    uint32_t rcba = board_rcba();
    uint16_t pm = board_pmbase();
    uintptr_t page;
    int heartbeat, arm, result;

    if (!rcba || !pm) {
        return;
    }
    page = map_mmio((uintptr_t)rcba + 0x3000u, 4096);
    if (!page) {
        return;
    }
    gcs_address = page + 0x410u;
    tco_base = (uint16_t)(pm + 0x60u);
    watchdog.rd = itco_rd;
    watchdog.wr = itco_wr;
    watchdog.gcs_rd = itco_gcs_rd;
    watchdog.gcs_wr = itco_gcs_wr;
    itco_options(&heartbeat, &arm);
    result = itco_attach(&watchdog, heartbeat);
    if (result == -ENODEV) {
        klog(KLOG_WARN, "watchdog", "refused: NO_REBOOT will not clear");
        return;
    }
    if (result < 0 || devfs_node_register(&itco_node) != 0) {
        klog(KLOG_WARN, "watchdog", "refused: the TCO registers or device node");
        return;
    }
    if (result == 1) {
        klog(KLOG_INFO, "watchdog", "refused: iTCO_wdt.heartbeat=%d: out of range, using %d",
             heartbeat, ITCO_DEFAULT_TIMEOUT);
    }
    itco_boot_log(&watchdog);
    itco_smi_option(pm);
    feed_seen = (uint32_t)kernel_ticks;
    registered = 1;
    if (arm && itco_boot_arm(&watchdog) != 0) {
        klog(KLOG_WARN, "watchdog", "refused: rach2.itco.arm: the timer would not start");
    }
    klog(KLOG_INFO, "watchdog", "ready: TCO version 2 at %X, %s, heartbeat %d s, nowayout %d",
         tco_base,
         !watchdog.hw_running ? "halted"
         : watchdog.boot_feed ? "running, fed by the kernel until opened"
                              : "running, unfed",
         watchdog.timeout, watchdog.nowayout);
}

/** Whether the tick left watchdogd a feed; the condition of its wait. */
static int feed_waiting(void *arg)
{
    (void)arg;
    return watchdog.feed_due;
}

/** The thread watchdogd: sleep until the tick leaves a feed, then do it,
 * holding the big lock as kernel code does. The tick's switch waits for its
 * next sleep (`no_tick_switch`), so a feed runs whole under the big lock,
 * and an open never finds one half done. */
static void itco_feeder(void)
{
    arch_irq_enable();
    for (;;) {
        (void)wq_wait_event_idle(&feed_wq, feed_waiting, NULL);
        itco_feed_work(&watchdog);
    }
}

void itco_thread_start(void)
{
    process_t *p;
    uintptr_t base;
    uint32_t flags;

    if (feeder || !registered) {
        return;
    }
    flags = kirq_save();
    base = kstack_alloc();
    p = base ? alloc_process() : NULL;
    if (!p) {
        if (base) {
            kstack_free(base + KERNEL_STACK_SIZE);
        }
        kirq_restore(flags);
        klog(KLOG_ERR, "watchdog", "refused: watchdogd: no record or stack for it");
        return;
    }
    p->kernel_stack = base + KERNEL_STACK_SIZE;
    p->space_root = 0;
    p->page_dir = NULL;
    p->parent_pid = 0;
    /* sched_set_fifo's MAX_RT_PRIO / 2, as watchdog_dev_init gives
     * watchdogd (Linux 6.12, watchdog_dev.c:1232 to 1237, and
     * kernel/sched/syscalls.c:873 to 877). */
    p->sched_policy = SCHED_FIFO;
    p->rt_priority = 50;
    proc_set_comm(p, "watchdogd");
    setup_process_stack(p, itco_feeder);
    feeder = p;
    sched_make_ready(p);
    kirq_restore(flags);
    klog(KLOG_DEBUG, "watchdog", "worker: watchdogd is pid %d", (int)p->pid);
}

void itco_tick(void)
{
    int was;
    uint32_t now = (uint32_t)kernel_ticks, passed = now - feed_seen;

    feed_seen = now;
    if (registered) {
        was = watchdog.feed_due;
        watchdog.threaded = feeder != NULL && sched_is_enabled();
        itco_feed_tick(&watchdog, passed);
        if (!was && watchdog.feed_due) {
            wq_wake_all(&feed_wq);
        }
    }
}

void itco_shutdown(void)
{
    if (registered) {
        itco_stop(&watchdog);
        watchdog.running = 0;
    }
}
#endif

/** Return the next service deadline for the stopped idle tick. */
uint32_t itco_idle_ticks(void)
{
    uint32_t elapsed = (uint32_t)kernel_ticks - feed_seen;
    uint32_t wait;

    if (!registered || !itco_feeding(&watchdog)) {
        return TICK_HZ;
    }
    wait = watchdog.feed_left + 1u;
    if (watchdog.open_limited && watchdog.open_left < wait) {
        wait = watchdog.open_left;
    }
    wait = elapsed < wait ? wait - elapsed : 1u;
    return wait < TICK_HZ ? wait : TICK_HZ;
}
