/**
 * synthfs_hosttest.c: the path classifier of rach2/sys/fs/synthfs.c, which
 * sorts a path among the KRFFS root, /dev, /proc and /sys.
 *
 * host-deps: rach2/sys/fs/synthfs.c
 *
 * Every path call classifies its path, and the split into components and
 * the parsing of process ids and descriptor numbers are where an error of
 * one is easy to make, so the rows run here under the sanitizers. The model
 * has one network interface, eth0, and no disks.
 */

#include "fs/synthfs.h"

static int fails;

/** The index of the interface named by the `len` bytes at `name`: 2 for
 *  eth0, the one interface of the model, and -1 for any other. */
int sysfs_net_index(const char *name, int len)
{
    return len == 4 && name[0] == 'e' && name[1] == 't' && name[2] == 'h' && name[3] == '0' ? 2
                                                                                            : -1;
}

/** The name of the interface `index`, eth0 for any index, into `out` when
 *  `cap` holds it. */
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

/** The PCI key of eth0's card, 0x18 (bus 0, device 3, function 0), and -1
 *  for any other index. */
int sysfs_net_pci_key(int index)
{
    return index == 2 ? 0x18 : -1;
}

/** No disk node has a name, since the model has no disks: /sys/block and
 *  /sys/dev/block are empty. Returns -1. */
int sysfs_block_find(const char *name, int len, int disk)
{
    (void)name;
    (void)len;
    (void)disk;
    return -1;
}

/** No disk node has device numbers. Returns -1. */
int sysfs_block_devno(const char *name, int len)
{
    (void)name;
    (void)len;
    return -1;
}

/** The empty name, for a disk node the model does not have. */
void sysfs_block_name(int key, char *out, int cap)
{
    (void)key;
    if (cap > 0) {
        out[0] = '\0';
    }
}

/** No disk node is on a whole disk. Returns -1. */
int sysfs_block_whole(int key)
{
    (void)key;
    return -1;
}

/** No disk node is a loop device. Returns -1. */
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

/** Whether the strings `a` and `b` are equal. */
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
 * have is ENOENT. A processor's directory `cpuN` is found by its number,
 * written without a leading zero, and has no attribute; whether the
 * processor is started is for the caller to ask.
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
    CHECK(synth_classify("sys/devices/system/cpu/online", &vp) == VNODE_SYS_FILE &&
          vp.sysent == SYSFS_CPU && vp.procfile == 2);
    CHECK(synth_classify("sys/devices/system/cpu/cpu0", &vp) == VNODE_SYS_DIR &&
          vp.sysent == SYSFS_CPUN && vp.syskey == 0);
    CHECK(synth_classify("sys/devices/system/cpu/cpu13", &vp) == VNODE_SYS_DIR &&
          vp.sysent == SYSFS_CPUN && vp.syskey == 13);
    synth_sys_path(SYSFS_CPUN, 13, path, (int)sizeof(path));
    CHECK(eq(path, "/sys/devices/system/cpu/cpu13"));
    CHECK(synth_classify("sys/devices/system/cpu/cpu01", &vp) == VNODE_ENOENT);
    CHECK(synth_classify("sys/devices/system/cpu/cpu", &vp) == VNODE_ENOENT);
    CHECK(synth_classify("sys/devices/system/cpu/cpux", &vp) == VNODE_ENOENT);
    CHECK(synth_classify("sys/devices/system/cpu/cpu1/x", &vp) == VNODE_ENOENT);
}

/**
 * The parser's bounds, of a number and of a path's depth. A process id or a
 * descriptor number past 2147483639 is ENOENT, and is not wrapped to a small
 * number another process might own. A leading '-' is not a digit, so "-1"
 * is not read as SYNTH_PID_SELF (-1). The deepest path of /proc, a thread's
 * descriptor link, is classified whole, and a deeper path is ENOENT or
 * ENOTDIR and is not cut to a shorter one.
 */
static void bounds(void)
{
    struct vpath vp;

    CHECK(synth_classify("proc/99999999999", &vp) == VNODE_ENOENT);
    CHECK(synth_classify("proc/4294967297", &vp) == VNODE_ENOENT);
    CHECK(synth_classify("proc/2147483639", &vp) == VNODE_PROC_PID && vp.pid == 2147483639);
    CHECK(synth_classify("proc/2147483640", &vp) == VNODE_ENOENT);
    CHECK(synth_classify("proc/7/fd/99999999999", &vp) == VNODE_ENOENT);

    CHECK(synth_classify("proc/-1", &vp) == VNODE_ENOENT);
    CHECK(synth_classify("proc/-1/stat", &vp) == VNODE_ENOENT);

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

/** The path `synth_dir_path` writes back for a directory, a thread's under
 *  its process's task directory among them. */
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

/**
 * The rows of the root, /dev and /proc, then those of `bounds`, `paths` and
 * `sys_tree`:
 *
 * - the empty path and ordinary names are the KRFFS root's, "devices" and
 *   "proconsul" among them, which only begin as "dev" and "proc" do;
 * - /dev and a device are found, and a path through a device, which is a
 *   leaf, is ENOENT;
 * - /proc, /proc/self, a process's directory and its descriptor directory;
 * - /proc/cmdline, the boot line, is a file and not a process;
 * - a process's files, and a path through one;
 * - the descriptor links /proc/PID/fd/N, which ttyname reads;
 * - a process's exe, cwd and root links, the /proc/sys files, and the task
 *   directory, each thread's entry with the process's id and the thread's
 *   and no task directory in it; no thread has id 0;
 * - a name of /proc that is not a process, a file a process does not have,
 *   or a descriptor that is not a number is ENOENT and not the root's, so
 *   no file of the root takes its place;
 * - leading, trailing and doubled slashes are skipped;
 * - a `.` or `..` component is ENOENT. The caller resolves them before it
 *   classifies, and one that reaches the classifier fails rather than name
 *   the wrong node.
 *
 * Returns the count of failed checks, at most 255.
 */
int main(void)
{
    struct vpath vp;

    CHECK(synth_classify("", &vp) == VNODE_KRFFS);
    CHECK(synth_classify("hello.txt", &vp) == VNODE_KRFFS);
    CHECK(synth_classify("etc/passwd", &vp) == VNODE_KRFFS);
    CHECK(synth_classify("devices/x", &vp) == VNODE_KRFFS);
    CHECK(synth_classify("proconsul", &vp) == VNODE_KRFFS);

    CHECK(synth_classify("dev", &vp) == VNODE_DEV_DIR);
    CHECK(synth_classify("dev/null", &vp) == VNODE_DEV && eq(vp.name, "null"));
    CHECK(synth_classify("dev/urandom", &vp) == VNODE_DEV && eq(vp.name, "urandom"));
    CHECK(synth_classify("dev/null/x", &vp) == VNODE_ENOENT);

    CHECK(synth_classify("proc", &vp) == VNODE_PROC_DIR);
    CHECK(synth_classify("proc/self", &vp) == VNODE_PROC_PID && vp.pid == SYNTH_PID_SELF);
    CHECK(synth_classify("proc/7", &vp) == VNODE_PROC_PID && vp.pid == 7);
    CHECK(synth_classify("proc/12/fd", &vp) == VNODE_PROC_FD_DIR && vp.pid == 12);

    CHECK(synth_classify("proc/cmdline", &vp) == VNODE_PROC_CMDLINE);
    CHECK(synth_classify("proc/cmdline/x", &vp) == VNODE_ENOTDIR);
    CHECK(synth_classify("proc/cmdlinex", &vp) == VNODE_ENOENT);

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

    CHECK(synth_classify("proc/self/fd/0", &vp) == VNODE_PROC_FD_LINK && vp.pid == SYNTH_PID_SELF &&
          vp.procfd == 0);
    CHECK(synth_classify("proc/7/fd/3", &vp) == VNODE_PROC_FD_LINK && vp.pid == 7 &&
          vp.procfd == 3);

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

    CHECK(synth_classify("proc/abc", &vp) == VNODE_ENOENT);
    CHECK(synth_classify("proc/7/bogus", &vp) == VNODE_ENOENT);
    CHECK(synth_classify("proc/7/fd/xy", &vp) == VNODE_ENOENT);

    CHECK(synth_classify("/dev/null", &vp) == VNODE_DEV && eq(vp.name, "null"));
    CHECK(synth_classify("dev/", &vp) == VNODE_DEV_DIR);
    CHECK(synth_classify("proc/"
                         "/7/"
                         "/stat",
                         &vp) == VNODE_PROC_FILE &&
          vp.pid == 7);

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
