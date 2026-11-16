/*
 * msrdev.c: /dev/cpu/N/msr, each processor's model-specific registers read
 * and written by root at the register's index, after Linux 6.12's msr
 * driver (arch/x86/kernel/msr.c).
 */

#include "msrdev.h"
#include "kcpu.h"

#include "../../fs/devfs.h"
#include "../../fs/fd.h"
#include "../../fs/file.h"
#include "../../fs/synthfs.h"
#include "../../fs/sysfs.h"
#include "../../klog.h"
#include "../../lib/kerrno.h"
#include "../../lib/kirq.h"
#include "../../lib/kstd.h"
#include "../../lib/kstring.h"
#include "../../mm/usercopy.h"
#include "../../proc/ns.h"
#include "../../proc/percpu.h"
#include "../../proc/process.h"
#include "../../proc/smp.h"
#include "../../timer.h"

/* Linux's MSR_MAJOR, whose minor is the processor's number. */
#define MSR_MAJOR 202

/* The ioctls of <asm/msr.h>: _IOWR('c', 0xA0 and 0xA1, __u32[8]). */
#define X86_IOC_RDMSR_REGS 0xC02063A0u
#define X86_IOC_WRMSR_REGS 0xC02063A1u

/* The values of the parameter allow_writes, Linux's enum allow_write_msrs. */
#define MSR_WRITES_ON      0
#define MSR_WRITES_OFF     1
#define MSR_WRITES_DEFAULT 2

/* The seconds between two warnings of a write, Linux's fw_rs. */
#define MSR_WARN_SECONDS 30u

/* The sysfs attributes of a processor's device, by `arg`. */
#define MSR_ATTR_DEV    0
#define MSR_ATTR_UEVENT 1

/* A processor's device: its node, "cpu/N/msr", and its name in /sys. */
struct msr_dev {
    struct devfs_node node;
    char path[16];
    char name[8];
};

static struct msr_dev msr_devs[NR_CPUS];

/* Written by /sys/module/msr/parameters/allow_writes; read without a lock,
 * as Linux reads it. */
static volatile int allow_writes = MSR_WRITES_DEFAULT;

/* The tick of the last warning of a write, 0 before the first. */
static uint64_t warned_at;

/* One access of one processor's register, run there (`msr_on_cpu`). */
struct msr_call {
    int write;
    uint32_t index;
    uint64_t value;
    uint32_t *regs; /* the eight registers of an X86_IOC_*MSR_REGS, or NULL */
    int result;     /* 0, -1 on #GP, or -ENXIO until it has run */
};

/** The access `arg`, a `struct msr_call`, on this processor. */
static void msr_here(void *arg)
{
    struct msr_call *c = (struct msr_call *)arg;

    if (c->regs) {
        c->result = kcpu_msr_safe_regs(c->write, c->regs);
    } else if (c->write) {
        c->result = kcpu_wrmsr_safe(c->index, c->value);
    } else {
        c->result = kcpu_rdmsr_safe(c->index, &c->value);
    }
}

/**
 * Run the access `c` on processor `cpu`, Linux's rdmsr_safe_on_cpu and its
 * kin, with interrupts off so that the caller cannot move between the
 * processors meanwhile: 0, -EIO for a #GP, or -ENXIO for a processor not
 * online.
 */
static int msr_on_cpu(int cpu, struct msr_call *c)
{
    uint32_t flags = kirq_save();

    c->result = -ENXIO;
    smp_call(cpu, msr_here, c);
    kirq_restore(flags);
    return c->result == -1 ? -EIO : c->result;
}

/** Whether processor `cpu` is online. */
static int msr_cpu_online(uint32_t cpu)
{
    return cpu < NR_CPUS && cpu_table[cpu].online;
}

/**
 * Bind an open to its processor, Linux's msr_open: -EPERM without
 * CAP_SYS_RAWIO in the first user namespace, -ENXIO for a processor not
 * online, -EIO for one without MSRs.
 */
static int32_t msr_open(struct devfs_node *node, fd_entry_t *e, int flags)
{
    (void)flags;
    if (current_proc->userns || !cap_holds(current_proc, CAP_SYS_RAWIO)) {
        return -EPERM;
    }
    if (!msr_cpu_online(node->minor)) {
        return -ENXIO;
    }
    if (!kcpu_msr_present()) {
        return -EIO;
    }
    e->priv = node;
    return 0;
}

/** The processor of the device `e` is open on. */
static int msr_cpu_of(fd_entry_t *e)
{
    return ((struct devfs_node *)e->priv)->minor;
}

/**
 * Read the register at the description's offset, cut to 32 bits, eight
 * bytes for each eight asked, Linux's msr_read: the offset does not move.
 * -EINVAL for a count not a multiple of 8; else the bytes read, or the
 * first error, -EIO for a register the processor refuses, when none were.
 */
static int32_t msr_read(fd_entry_t *e, char __user *buf, uint32_t count)
{
    struct msr_call c;
    uint32_t bytes = 0;
    int err = 0;

    if (count % 8) {
        return -EINVAL;
    }
    k_memset(&c, 0, sizeof(c));
    c.index = (uint32_t)e->ofd->offset;
    for (; count; count -= 8) {
        if ((err = msr_on_cpu(msr_cpu_of(e), &c)) != 0) {
            break;
        }
        if (copy_to_user(buf + bytes, &c.value, 8) != 0) {
            err = -EFAULT;
            break;
        }
        bytes += 8;
    }
    return bytes ? (int32_t)bytes : err;
}

/**
 * Whether a write of register `index` goes on, Linux's filter_write: 0, or
 * -EPERM while allow_writes is off. While it is default, a warning names
 * the writer at most once in 30 seconds.
 */
static int msr_filter_write(uint32_t index)
{
    uint64_t now;

    if (allow_writes == MSR_WRITES_ON) {
        return 0;
    }
    if (allow_writes == MSR_WRITES_OFF) {
        return -EPERM;
    }
    now = tick_count();
    if (warned_at && tick_msecs(now - warned_at) < MSR_WARN_SECONDS * 1000u) {
        return 0;
    }
    warned_at = now ? now : 1;
    klog(KLOG_WARN, "msr", "write: unrecognized MSR 0x%x by %s (pid: %u)", index,
         current_proc->comm, current_proc->pid);
    return 0;
}

/**
 * Write the register at the description's offset, cut to 32 bits, from
 * each eight bytes given, Linux's msr_write: the offset does not move.
 * `msr_filter_write`'s -EPERM, -EINVAL for a count not a multiple of 8;
 * else the bytes written, or the first error when none were.
 */
static int32_t msr_write(fd_entry_t *e, const char __user *buf, uint32_t count)
{
    struct msr_call c;
    uint32_t bytes = 0;
    int err;

    k_memset(&c, 0, sizeof(c));
    c.write = 1;
    c.index = (uint32_t)e->ofd->offset;
    if ((err = msr_filter_write(c.index)) != 0) {
        return err;
    }
    if (count % 8) {
        return -EINVAL;
    }
    for (; count; count -= 8) {
        if (copy_from_user(&c.value, buf + bytes, 8) != 0) {
            err = -EFAULT;
            break;
        }
        if ((err = msr_on_cpu(msr_cpu_of(e), &c)) != 0) {
            break;
        }
        bytes += 8;
    }
    return bytes ? (int32_t)bytes : err;
}

/**
 * X86_IOC_RDMSR_REGS and X86_IOC_WRMSR_REGS, Linux's msr_ioctl: the access
 * with the eight registers at `arg`, given back there. -EBADF for a
 * descriptor not open to read, or to write for a write, -EFAULT, the
 * write's `msr_filter_write` error, -EIO for a #GP, -ENOTTY for any other
 * request.
 */
static int32_t msr_ioctl(fd_entry_t *e, uint32_t request, uintptr_t arg)
{
    uint32_t __user *uregs = (uint32_t __user *)arg;
    uint32_t regs[8];
    struct msr_call c;
    int err;

    if (request != X86_IOC_RDMSR_REGS && request != X86_IOC_WRMSR_REGS) {
        return -ENOTTY;
    }
    k_memset(&c, 0, sizeof(c));
    c.write = request == X86_IOC_WRMSR_REGS;
    if (!(e->flags & (c.write ? FD_FLAG_WRITE : FD_FLAG_READ))) {
        return -EBADF;
    }
    if (copy_from_user(regs, uregs, sizeof(regs)) != 0) {
        return -EFAULT;
    }
    if (c.write && (err = msr_filter_write(regs[1])) != 0) {
        return err;
    }
    c.regs = regs;
    if ((err = msr_on_cpu(msr_cpu_of(e), &c)) != 0) {
        return err;
    }
    return copy_to_user(uregs, regs, sizeof(regs)) != 0 ? -EFAULT : 0;
}

/** The largest file offset, Linux's OFFSET_MAX. */
#define MSR_OFFSET_MAX ((int64_t)(((uint64_t)1 << 63) - 1u))

/**
 * Seek, Linux's no_seek_end_llseek, which its msr device uses: SEEK_SET and
 * SEEK_CUR alone. -EINVAL for another `whence` and, as vfs_setpos, for a
 * negative result or a SEEK_CUR sum past MSR_OFFSET_MAX.
 */
static int64_t msr_llseek(fd_entry_t *e, int64_t offset, int whence)
{
    int64_t base;

    if (whence != 0 && whence != 1) {
        return -EINVAL;
    }
    base = whence == 1 ? (int64_t)e->ofd->offset : 0;
    if (offset > 0 && base > MSR_OFFSET_MAX - offset) {
        return -EINVAL;
    }
    return base + offset < 0 ? -EINVAL : base + offset;
}

static const struct file_ops msr_fops = {
    msr_read, msr_write, NULL,       msr_ioctl,
    NULL,     NULL,      msr_llseek, FILE_OPS_UNLOCKED | FILE_OPS_POSITIONED};

/** Append `s` to `buf` at `*len`, as far as `cap` bytes leave room. */
static void msr_emit(char *buf, uint32_t cap, uint32_t *len, const char *s)
{
    while (*s && *len < cap) {
        buf[(*len)++] = *s++;
    }
}

/** Show a processor's `dev`, "202:N", or its `uevent`, as Linux's do. */
static int32_t msr_show(void *obj, int arg, char *buf, uint32_t cap)
{
    struct msr_dev *d = (struct msr_dev *)obj;
    char num[12];
    uint32_t len = 0;

    (void)k_itoa(d->node.minor, num, sizeof(num), 10, false);
    if (arg == MSR_ATTR_DEV) {
        msr_emit(buf, cap, &len, "202:");
        msr_emit(buf, cap, &len, num);
    } else {
        msr_emit(buf, cap, &len, "MAJOR=202\nMINOR=");
        msr_emit(buf, cap, &len, num);
        msr_emit(buf, cap, &len, "\nDEVNAME=");
        msr_emit(buf, cap, &len, d->path);
    }
    msr_emit(buf, cap, &len, "\n");
    return (int32_t)len;
}

/** Show /sys/module/msr's `initstate`, `live`, or its parameter
 * `allow_writes`, as Linux's get_allow_writes does. */
static int32_t msr_module_show(void *obj, int arg, char *buf, uint32_t cap)
{
    const char *s = "live";
    uint32_t len = 0;

    (void)obj;
    if (arg) {
        s = allow_writes == MSR_WRITES_ON    ? "on"
            : allow_writes == MSR_WRITES_OFF ? "off"
                                             : "default";
    }
    msr_emit(buf, cap, &len, s);
    msr_emit(buf, cap, &len, "\n");
    return (int32_t)len;
}

/** Whether the `len` bytes at `buf`, without the space around them, are
 * the word `w`. */
static int msr_word_is(const char *buf, uint32_t len, const char *w)
{
    uint32_t n = (uint32_t)k_strlen(w);

    while (len && (*buf == ' ' || *buf == '\t' || *buf == '\n')) {
        buf++;
        len--;
    }
    while (len && (buf[len - 1] == ' ' || buf[len - 1] == '\t' || buf[len - 1] == '\n')) {
        len--;
    }
    if (len != n) {
        return 0;
    }
    while (n && buf[n - 1] == w[n - 1]) {
        n--;
    }
    return n == 0;
}

/** Take a write of allow_writes, Linux's set_allow_writes: `on`, `off`, and
 * anything else as `default`. */
static int32_t msr_module_store(void *obj, int arg, const char *buf, uint32_t len)
{
    (void)obj;
    (void)arg;
    allow_writes = msr_word_is(buf, len, "on")    ? MSR_WRITES_ON
                   : msr_word_is(buf, len, "off") ? MSR_WRITES_OFF
                                                  : MSR_WRITES_DEFAULT;
    return (int32_t)len;
}

/** The directory `name` below `parent`, made on its first use: its handle,
 * or -errno. */
static int msr_dir(int parent, const char *name)
{
    int h;

    if (parent < 0) {
        return parent;
    }
    h = sysfs_node_child(parent, name, (int)k_strlen(name));
    return h > 0 ? h : sysfs_add_dir(parent, name);
}

/**
 * Give processor `cpu`'s device its directory /sys/devices/virtual/msr/msrN
 * with `dev`, `uevent` and `subsystem`, and its links in /sys/class/msr and
 * /sys/dev/char, then its uevent `add`, as Linux's msr_device_create does.
 * 0, or -errno, with what was made left for the node to work without.
 */
static int msr_sysfs(struct msr_dev *d)
{
    static const struct sysfs_attr dev = {"dev", 0444u, msr_show, NULL};
    int cls = msr_dir(sysfs_anchor(SYSFS_CLASS, 0), "msr");
    int dir = msr_dir(msr_dir(sysfs_anchor(SYSFS_DEVICES, 0), "virtual"), "msr");
    char devno[16];
    int h;

    if (cls < 0 || dir < 0) {
        return cls < 0 ? cls : dir;
    }
    if ((dir = sysfs_add_dir(dir, d->name)) < 0) {
        return dir;
    }
    k_memcpy(devno, "202:", 4);
    (void)k_itoa(d->node.minor, devno + 4, sizeof(devno) - 4, 10, false);
    if ((h = sysfs_add_attr(dir, &dev, d, MSR_ATTR_DEV)) < 0 ||
        (h = sysfs_add_uevent(dir, msr_show, d, MSR_ATTR_UEVENT)) < 0 ||
        (h = sysfs_add_link(dir, "subsystem", cls)) < 0 ||
        (h = sysfs_add_link(cls, d->name, dir)) < 0 ||
        (h = sysfs_add_link(sysfs_anchor(SYSFS_DEV_CHAR, 0), devno, dir)) < 0) {
        return h;
    }
    sysfs_uevent(dir, "add", NULL);
    return 0;
}

/**
 * Make /sys/module/msr with `initstate` live and `parameters/allow_writes`,
 * as a loaded msr.ko has them, so that `modprobe msr`, which libkmod answers
 * from initstate when no modules.builtin names it, finds the driver there.
 */
static void msr_module(void)
{
    static const struct sysfs_attr initstate = {"initstate", 0444u, msr_module_show, NULL};
    static const struct sysfs_attr allow = {"allow_writes", 0600u, msr_module_show,
                                            msr_module_store};
    int made, dir = sysfs_module_dir("msr", &made), params;

    if (dir < 0 || sysfs_add_attr(dir, &initstate, NULL, 0) < 0 ||
        (params = sysfs_module_params("msr", &made)) < 0 ||
        sysfs_add_attr(params, &allow, NULL, 1) < 0) {
        klog(KLOG_WARN, "msr", "failed: /sys/module/msr not made");
    }
}

void msrdev_start(void)
{
    uint32_t cpu;
    int32_t rc;
    int made = 0;

    for (cpu = 0; cpu < NR_CPUS; cpu++) {
        struct msr_dev *d = &msr_devs[cpu];
        uint32_t at;

        if (!msr_cpu_online(cpu)) {
            continue;
        }
        k_memcpy(d->path, "cpu/", 4);
        (void)k_itoa((int)cpu, d->path + 4, sizeof(d->path) - 4, 10, false);
        at = (uint32_t)k_strlen(d->path);
        k_memcpy(d->path + at, "/msr", 5);
        k_memcpy(d->name, "msr", 3);
        (void)k_itoa((int)cpu, d->name + 3, sizeof(d->name) - 3, 10, false);
        d->node.name = d->path;
        d->node.major = MSR_MAJOR;
        d->node.minor = (uint16_t)cpu;
        d->node.mode = 0600;
        d->node.ops = &msr_fops;
        d->node.open = msr_open;
        d->node.priv = d;
        if ((rc = devfs_node_register(&d->node)) != 0) {
            klog(KLOG_WARN, "msr", "failed: /dev/%s not made: %d", d->path, rc);
            continue;
        }
        if ((rc = msr_sysfs(d)) != 0) {
            klog(KLOG_WARN, "msr", "failed: /sys of %s not made: %d", d->name, rc);
        }
        made++;
    }
    msr_module();
    klog(KLOG_DEBUG, "msr", "registered: %d processors", made);
}
