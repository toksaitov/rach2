/* Intel TCO version 2 watchdog and Linux's /dev/watchdog interface. */
#include "itco.h"
#include "../../lib/kint.h"
#include "../../lib/kerrno.h"
#include "../../lib/kdef.h"

/* Linux 6.12, drivers/watchdog/iTCO_wdt.c: offsets from PMBASE + 0x60. */
#define TCO_RLD       0x00
#define TCO1_STS      0x04
#define TCO2_STS      0x06
#define TCO1_CNT      0x08
#define TCO_TMR       0x12
#define TCO_HALT      0x0800u
#define TCO_NO_REBOOT 0x20u
#define TCO_MASK      0x03FFu

/* All callbacks are synchronous and do not sleep; the big lock serializes
 * the device. The host test supplies the same boundary over a TCO model. */
struct itco {
    uint16_t (*rd)(void *ctx, unsigned reg);
    void (*wr)(void *ctx, unsigned reg, uint16_t value);
    uint32_t (*gcs_rd)(void *ctx);
    void (*gcs_wr)(void *ctx, uint32_t value);
    void *ctx;
    int timeout; /* requested seconds, as Linux reports GETTIMEOUT */
    int opened;
    int magic;   /* the last nonempty write contained V */
    int running; /* started and not since stopped: Linux's WDOG_ACTIVE */
    int pinged;  /* started or fed since the last GETSTATUS */
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

/** Halt the timer, then inhibit resets; 0 or -EIO if halt did not stick. */
static int itco_stop(struct itco *t)
{
    uint16_t value = t->rd(t->ctx, TCO1_CNT);

    t->wr(t->ctx, TCO1_CNT, (uint16_t)(value | TCO_HALT));
    value = t->rd(t->ctx, TCO1_CNT);
    itco_noreboot(t, 1);
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

/** Check the register block, halt it and clear old status before use;
 * 0 or -EIO. All writes are bounded register accesses, with no wait loop. */
static int itco_attach(struct itco *t)
{
    if (t->rd(t->ctx, TCO1_CNT) == 0xFFFFu || t->rd(t->ctx, TCO_TMR) == 0xFFFFu ||
        t->gcs_rd(t->ctx) == 0xFFFFFFFFu) {
        return -EIO;
    }
    if (itco_stop(t) != 0) {
        return -EIO;
    }
    t->wr(t->ctx, TCO1_STS, 8);
    t->wr(t->ctx, TCO2_STS, 2);
    t->wr(t->ctx, TCO2_STS, 4);
    return itco_timeout(t, 30) ? -EIO : 0;
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

/** Halt a running timer, as Linux's watchdog_stop does; 0 or -EIO. */
static int itco_disarm(struct itco *t)
{
    if (!t->running) {
        return 0;
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
#include "../../klog.h"

static struct itco watchdog;
static uint16_t tco_base;
static uintptr_t gcs_address;
static int registered;

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
             watchdog.magic ? "the timer refused halt" : "no magic close");
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
    case 0x80045702u: /* WDIOC_GETBOOTSTATUS */
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
                                         itco_close, NULL,       NULL};
static struct devfs_node itco_node = {"watchdog", 10, 130, 0600, &itco_ops, itco_open, NULL};

void itco_init(void)
{
    uint32_t rcba = board_rcba();
    uint16_t pm = board_pmbase();
    uintptr_t page;

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
    if (itco_attach(&watchdog) != 0 || devfs_node_register(&itco_node) != 0) {
        klog(KLOG_WARN, "watchdog", "refused: the TCO registers or device node");
        return;
    }
    registered = 1;
    klog(KLOG_INFO, "watchdog", "ready: TCO version 2 at %X, halted, timeout 30 s", tco_base);
}

void itco_shutdown(void)
{
    if (registered) {
        itco_stop(&watchdog);
        watchdog.running = 0;
    }
}
#endif
