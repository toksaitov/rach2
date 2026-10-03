/**
 * Dual-target host test for the synthetic-path classifier (fs/synthfs.c).
 *
 * host-deps: rach2/sys/fs/synthfs.c
 *
 * The component splitting and the /proc/<pid>/fd/<n> parsing are the
 * fence-post-prone part every path syscall leans on, so they run natively here
 * under ASan/UBSan: a path resolves to the right vnode kind with the right
 * pid/fd, leading/trailing/doubled slashes are tolerated, a non-pid or unknown
 * /proc entry is ENOENT, and an ordinary name stays KRFFS-bound. Returns the
 * failed-check count (0 == pass).
 */

#include "fs/synthfs.h"

static int fails;

/** The one interface of the model, eth0 with index 2. */
int sysfs_net_index(const char *name, int len)
{
    return len == 4 && name[0] == 'e' && name[1] == 't' && name[2] == 'h' && name[3] == '0' ? 2
                                                                                            : -1;
}

void sysfs_net_name(int index, char *out, int cap)
{
    (void)index;
    if (cap > 4) {
        out[0] = 'e';
        out[1] = 't';
        out[2] = 'h';
        out[3] = '0';
        out[4] = '\0';
    }
}

int sysfs_net_pci_key(int index)
{
    return index == 2 ? 0x18 : -1;
}

/* No disks: /sys/block and /sys/dev/block are empty. */
int sysfs_block_find(const char *name, int len, int disk)
{
    (void)name;
    (void)len;
    (void)disk;
    return -1;
}

int sysfs_block_devno(const char *name, int len)
{
    (void)name;
    (void)len;
    return -1;
}

void sysfs_block_name(int key, char *out, int cap)
{
    (void)key;
    if (cap > 0) {
        out[0] = '\0';
    }
}

int sysfs_block_whole(int key)
{
    (void)key;
    return -1;
}

int sysfs_block_loop(int key)
{
    (void)key;
    return -1;
}

#define CHECK(cond)    \
    do {               \
        if (!(cond)) { \
            fails++;   \
        }              \
    } while (0)

/* Equal NUL-terminated strings? */
static int eq(const char *a, const char *b)
{
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

/**
 * /sys: the fixed directories, a device's link and directory and uevent by
 * its name, and the names written back from the keys as they were read; a
 * name not of a device's form, or a hub port that four bits cannot hold, is
 * ENOENT, and a path through a uevent is ENOTDIR. Under /sys/class/net an
 * interface's directory, an attribute, a counter of its statistics and its
 * device link are found by name, and an interface or attribute it does not
 * have is ENOENT.
 */
static void sys_tree(void)
{
    struct vpath vp;
    char name[32], path[64];

    CHECK(synth_classify("sys", &vp) == VNODE_SYS_DIR && vp.sysent == SYSFS_ROOT);
    CHECK(synth_classify("sys/bus/usb/devices", &vp) == VNODE_SYS_DIR &&
          vp.sysent == SYSFS_USB_LINKS);
    CHECK(synth_classify("sys/devices/pci0000:00", &vp) == VNODE_SYS_DIR &&
          vp.sysent == SYSFS_PCI_ROOT);
    CHECK(synth_classify("sys/bus/pci/devices/0000:00:1f.2", &vp) == VNODE_SYS_LINK &&
          vp.sysent == SYSFS_PCI_DEVICE && vp.syskey == (0 << 8 | 0x1f << 3 | 2));
    synth_sys_device_name(SYSFS_PCI_DEVICE, vp.syskey, name, (int)sizeof(name));
    CHECK(eq(name, "0000:00:1f.2"));
    CHECK(synth_classify("sys/bus/pci/devices/0000:03:00.0/uevent", &vp) == VNODE_SYS_FILE &&
          vp.syskey == 3 << 8);
    CHECK(synth_classify("sys/devices/usb/2-17.3.4", &vp) == VNODE_SYS_DIR &&
          vp.sysent == SYSFS_USB_DEVICE && vp.syskey == (1 << 28 | 17 << 20 | 3 << 16 | 4 << 12));
    synth_sys_device_name(SYSFS_USB_DEVICE, vp.syskey, name, (int)sizeof(name));
    CHECK(eq(name, "2-17.3.4"));
    synth_sys_path(SYSFS_USB_DEVICE, vp.syskey, path, (int)sizeof(path));
    CHECK(eq(path, "/sys/devices/usb/2-17.3.4"));
    CHECK(synth_classify("sys/bus/usb/devices/1-2", &vp) == VNODE_SYS_LINK);
    CHECK(synth_classify("sys/bus/usb/devices/1-2/uevent/x", &vp) == VNODE_ENOTDIR);
    CHECK(synth_classify("sys/bus/usb/devices/1-2.16", &vp) == VNODE_ENOENT);
    CHECK(synth_classify("sys/bus/usb/devices/5-1", &vp) == VNODE_ENOENT);
    CHECK(synth_classify("sys/bus/usb/devices/1", &vp) == VNODE_ENOENT);
    CHECK(synth_classify("sys/bus/pci/devices/0000:00:20.0", &vp) == VNODE_ENOENT);
    CHECK(synth_classify("sys/bus/pci/devices/0000:00:1F.2", &vp) == VNODE_ENOENT);
    CHECK(synth_classify("sys/bus/pci/devices/0000:00:1f.2/vendor", &vp) == VNODE_ENOENT);
    CHECK(synth_classify("sys/class", &vp) == VNODE_SYS_DIR && vp.sysent == SYSFS_CLASS);
    CHECK(synth_classify("sys/class/net/eth0", &vp) == VNODE_SYS_DIR && vp.sysent == SYSFS_NET_IF &&
          vp.syskey == 2);
    CHECK(synth_classify("sys/class/net/eth0/address", &vp) == VNODE_SYS_FILE &&
          vp.sysent == SYSFS_NET_IF && vp.procfile == 0);
    CHECK(synth_classify("sys/class/net/eth0/statistics/rx_bytes", &vp) == VNODE_SYS_FILE &&
          vp.sysent == SYSFS_NET_STATS && vp.procfile == 2);
    CHECK(synth_classify("sys/class/net/eth0/device", &vp) == VNODE_SYS_LINK &&
          vp.sysent == SYSFS_PCI_DEVICE && vp.syskey == 0x18);
    CHECK(synth_classify("sys/class/net/eth0/mtu/x", &vp) == VNODE_ENOTDIR);
    CHECK(synth_classify("sys/class/net/eth0/bogus", &vp) == VNODE_ENOENT);
    CHECK(synth_classify("sys/class/net/eth9", &vp) == VNODE_ENOENT);
    synth_sys_path(SYSFS_NET_STATS, 2, path, (int)sizeof(path));
    CHECK(eq(path, "/sys/class/net/eth0/statistics"));
}

/** The parser's bounds, of a number and of a path's depth. */
static void bounds(void)
{
    struct vpath vp;

    /* A pid that would overflow or wrap a 32-bit int is rejected, never folded
     * to a small pid another process might own; the boundary is 2147483639. */
    CHECK(synth_classify("proc/99999999999", &vp) == VNODE_ENOENT);
    CHECK(synth_classify("proc/4294967297", &vp) == VNODE_ENOENT);
    CHECK(synth_classify("proc/2147483639", &vp) == VNODE_PROC_PID && vp.pid == 2147483639);
    CHECK(synth_classify("proc/2147483640", &vp) == VNODE_ENOENT);
    CHECK(synth_classify("proc/7/fd/99999999999", &vp) == VNODE_ENOENT);

    /* A leading '-' is not a digit, so a negative token cannot be read as the
     * SYNTH_PID_SELF (-1) sentinel. */
    CHECK(synth_classify("proc/-1", &vp) == VNODE_ENOENT);
    CHECK(synth_classify("proc/-1/stat", &vp) == VNODE_ENOENT);

    /* The deepest path, a thread's descriptor link, is whole, and a path
     * deeper than any is refused rather than cut to a shorter one. */
    CHECK(synth_classify("proc/7/task/9/fd", &vp) == VNODE_PROC_FD_DIR);
    CHECK(synth_classify("proc/7/task/9/fd/0", &vp) == VNODE_PROC_FD_LINK && vp.procfd == 0 &&
          vp.pid == 7 && vp.tid == 9);
    CHECK(synth_classify("proc/7/task/9/fd/12", &vp) == VNODE_PROC_FD_LINK && vp.procfd == 12);
    CHECK(synth_classify("proc/7/task/9/stat/junk", &vp) == VNODE_ENOENT);
    CHECK(synth_classify("proc/7/task/9/fd/0/x", &vp) == VNODE_ENOENT);
    CHECK(synth_classify("proc/7/task/9/fd/0/x/y/z", &vp) == VNODE_ENOENT);
    CHECK(synth_classify("proc/7/fd/0/a/b/c/d", &vp) == VNODE_ENOENT);
    CHECK(synth_classify("proc/cmdline/a/b/c/d/e/f", &vp) == VNODE_ENOTDIR);
    CHECK(synth_classify("dev/pts/1/a/b/c/d", &vp) == VNODE_ENOENT);
}

/** A directory's path, a thread's under a process's task directory
 * among them. */
static void paths(void)
{
    char out[64];

    synth_dir_path(VNODE_PROC_PID, 7, 0, out, (int)sizeof(out));
    CHECK(eq(out, "/proc/7"));
    synth_dir_path(VNODE_PROC_PID, 7, 9, out, (int)sizeof(out));
    CHECK(eq(out, "/proc/7/task/9"));
    synth_dir_path(VNODE_PROC_FD_DIR, 7, 9, out, (int)sizeof(out));
    CHECK(eq(out, "/proc/7/task/9/fd"));
    synth_dir_path(VNODE_PROC_TASK_DIR, 7, 0, out, (int)sizeof(out));
    CHECK(eq(out, "/proc/7/task"));
}

int main(void)
{
    struct vpath vp;

    /* Root and ordinary KRFFS names are not synthetic. */
    CHECK(synth_classify("", &vp) == VNODE_KRFFS);
    CHECK(synth_classify("hello.txt", &vp) == VNODE_KRFFS);
    CHECK(synth_classify("etc/passwd", &vp) == VNODE_KRFFS);
    CHECK(synth_classify("devices/x", &vp) == VNODE_KRFFS); /* not "dev" */
    CHECK(synth_classify("proconsul", &vp) == VNODE_KRFFS); /* not "proc" */

    /* /dev. */
    CHECK(synth_classify("dev", &vp) == VNODE_DEV_DIR);
    CHECK(synth_classify("dev/null", &vp) == VNODE_DEV && eq(vp.name, "null"));
    CHECK(synth_classify("dev/urandom", &vp) == VNODE_DEV && eq(vp.name, "urandom"));
    CHECK(synth_classify("dev/null/x", &vp) == VNODE_ENOENT); /* device is a leaf */

    /* /proc directories. */
    CHECK(synth_classify("proc", &vp) == VNODE_PROC_DIR);
    CHECK(synth_classify("proc/self", &vp) == VNODE_PROC_PID && vp.pid == SYNTH_PID_SELF);
    CHECK(synth_classify("proc/7", &vp) == VNODE_PROC_PID && vp.pid == 7);
    CHECK(synth_classify("proc/12/fd", &vp) == VNODE_PROC_FD_DIR && vp.pid == 12);

    /* /proc/cmdline: the global boot line, not a pid, and a leaf. */
    CHECK(synth_classify("proc/cmdline", &vp) == VNODE_PROC_CMDLINE);
    CHECK(synth_classify("proc/cmdline/x", &vp) == VNODE_ENOTDIR);
    CHECK(synth_classify("proc/cmdlinex", &vp) == VNODE_ENOENT); /* not a pid */

    /* /proc files. */
    CHECK(synth_classify("proc/7/stat", &vp) == VNODE_PROC_FILE && vp.procfile == SYNTH_PROC_STAT &&
          vp.pid == 7);
    CHECK(synth_classify("proc/9/cmdline", &vp) == VNODE_PROC_FILE &&
          vp.procfile == SYNTH_PROC_CMDLINE);
    CHECK(synth_classify("proc/self/maps", &vp) == VNODE_PROC_FILE &&
          vp.procfile == SYNTH_PROC_MAPS && vp.pid == SYNTH_PID_SELF);
    CHECK(synth_classify("proc/5/status", &vp) == VNODE_PROC_FILE &&
          vp.procfile == SYNTH_PROC_STATUS && vp.pid == 5);
    CHECK(synth_classify("proc/5/statm", &vp) == VNODE_PROC_FILE &&
          vp.procfile == SYNTH_PROC_STATM);
    CHECK(synth_classify("proc/5/smaps", &vp) == VNODE_PROC_FILE &&
          vp.procfile == SYNTH_PROC_SMAPS);
    CHECK(synth_classify("proc/5/auxv", &vp) == VNODE_PROC_FILE && vp.procfile == SYNTH_PROC_AUXV);
    CHECK(synth_classify("proc/5/environ", &vp) == VNODE_PROC_FILE &&
          vp.procfile == SYNTH_PROC_ENVIRON);
    CHECK(synth_classify("proc/5/comm", &vp) == VNODE_PROC_FILE && vp.procfile == SYNTH_PROC_COMM);
    CHECK(synth_classify("proc/5/maps/x", &vp) == VNODE_ENOENT);

    /* /proc/<pid>/fd/<n> magic links: the ttyname path. */
    CHECK(synth_classify("proc/self/fd/0", &vp) == VNODE_PROC_FD_LINK && vp.pid == SYNTH_PID_SELF &&
          vp.procfd == 0);
    CHECK(synth_classify("proc/7/fd/3", &vp) == VNODE_PROC_FD_LINK && vp.pid == 7 &&
          vp.procfd == 3);

    /* The program's link, and the threads' directory, each thread's entry
     * keeping the process's id and the thread's, with no task directory in
     * it; no thread has id 0. */
    CHECK(synth_classify("proc/self/exe", &vp) == VNODE_PROC_EXE && vp.pid == SYNTH_PID_SELF);
    CHECK(synth_classify("proc/self/cwd", &vp) == VNODE_PROC_CWD && vp.pid == SYNTH_PID_SELF);
    CHECK(synth_classify("proc/5/root", &vp) == VNODE_PROC_ROOT && vp.pid == 5);
    CHECK(synth_classify("proc/sys/kernel/hostname", &vp) == VNODE_PROC_SYS_FILE &&
          vp.sysent == SYNTH_SYS_HOSTNAME);
    CHECK(synth_classify("proc/sys/net/ipv4/ip_forward", &vp) == VNODE_PROC_SYS_FILE);
    CHECK(synth_classify("proc/7/exe/x", &vp) == VNODE_ENOENT);
    CHECK(synth_classify("proc/7/task", &vp) == VNODE_PROC_TASK_DIR && vp.pid == 7 && !vp.tid);
    CHECK(synth_classify("proc/7/task/9", &vp) == VNODE_PROC_PID && vp.pid == 7 && vp.tid == 9);
    CHECK(synth_classify("proc/7/task/9/stat", &vp) == VNODE_PROC_FILE &&
          vp.procfile == SYNTH_PROC_STAT && vp.pid == 7 && vp.tid == 9);
    CHECK(synth_classify("proc/7/task/9/comm", &vp) == VNODE_PROC_FILE &&
          vp.procfile == SYNTH_PROC_COMM && vp.pid == 7 && vp.tid == 9);
    CHECK(synth_classify("proc/7/task/9/exe", &vp) == VNODE_PROC_EXE && vp.pid == 7 && vp.tid == 9);
    CHECK(synth_classify("proc/7/task/9/task", &vp) == VNODE_ENOENT);
    CHECK(synth_classify("proc/7/task/x", &vp) == VNODE_ENOENT);
    CHECK(synth_classify("proc/7/task/0", &vp) == VNODE_ENOENT);

    /* Bad /proc entries are ENOENT, never KRFFS (so they don't
     * shadow-resolve). */
    CHECK(synth_classify("proc/abc", &vp) == VNODE_ENOENT);     /* not a pid */
    CHECK(synth_classify("proc/7/bogus", &vp) == VNODE_ENOENT); /* unknown file */
    CHECK(synth_classify("proc/7/fd/xy", &vp) == VNODE_ENOENT); /* not an fd number */

    /* Slash tolerance: leading, trailing, and doubled slashes. */
    CHECK(synth_classify("/dev/null", &vp) == VNODE_DEV && eq(vp.name, "null"));
    CHECK(synth_classify("dev/", &vp) == VNODE_DEV_DIR);
    CHECK(synth_classify("proc/"
                         "/7/"
                         "/stat",
                         &vp) == VNODE_PROC_FILE &&
          vp.pid == 7);

    /* The classifier requires a lexically pre-normalized path: a '.' or '..'
     * component is NOT collapsed here (only the KRFFS resolver does that), so a
     * dot path transiting a synthetic prefix deliberately fails closed rather
     * than reaching the wrong object. */
    CHECK(synth_classify("proc/7/../9/stat", &vp) == VNODE_ENOENT);
    CHECK(synth_classify("proc/../etc/passwd", &vp) == VNODE_ENOENT);
    CHECK(synth_classify("dev/../dev/null", &vp) == VNODE_ENOENT);
    CHECK(synth_classify("proc/7/.", &vp) == VNODE_ENOENT);
    CHECK(synth_classify("dev/./null", &vp) == VNODE_ENOENT);

    bounds();
    paths();
    sys_tree();
    return fails > 255 ? 255 : fails;
}
