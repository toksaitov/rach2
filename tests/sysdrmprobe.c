/**
 * sysdrmprobe: the /sys entries by which libdrm, Mesa's loader and libudev
 * find display devices, as Linux 6.12 gives them to root, and the uevent a
 * write of `add` to a device's uevent file sends.
 *
 * The probe checks, in order:
 *
 * * `debugfs`: debugfs is mounted at /sys/kernel/debug, root's with mode
 *   0700, on a device of its own, with /sys/kernel as its `..`; it lists
 *   sync's sw_sync and info, which root opens, and refuses uid 65534 with
 *   EACCES.
 * * `class`: /sys/class/drm is a directory whose `version` reads
 *   "drm 1.1.0 20060810".
 * * `pci`: every function of /sys/bus/pci/devices has `vendor`, `device`,
 *   `subsystem_vendor`, `subsystem_device`, `class` and `revision` as
 *   pci-sysfs.c writes them, the same IDs at their offsets of `config` and
 *   in the PCI_ID of `uevent`, its own name as PCI_SLOT_NAME, a `subsystem`
 *   link to /sys/bus/pci, and `boot_vga` exactly when it is a VGA function.
 *   The first resourceN file found, root's with mode 0600, is opened as
 *   Linux judges a file, by the file-system user id: root is refused it
 *   with EACCES after setfsuid(1000), and opens it for reading and writing
 *   as its owner once capset has dropped every capability.
 * * `cards`: each node of /dev/dri, found as libdrm's drmGetDevices2 finds
 *   it: its numbers from stat, the subsystem from the last name of
 *   /sys/dev/char/226:N/device/subsystem, and for a PCI device its slot
 *   from the device's uevent, which is the name its `device` link leads to,
 *   and its IDs from the device's files, which `config` repeats; for a
 *   virtio device, as virtio-gpu's card has, its uevent's DRIVER and
 *   MODALIAS, its `device`, `vendor`, `modalias` and `driver`, and the same
 *   of the PCI function above it, reached through `device/..` as libdrm
 *   reaches it; for a platform device its uevent's DRIVER and MODALIAS.
 *   The device's `drm` directory lists the node, the node's `dev` gives its
 *   numbers and its uevent its DEVNAME, and /sys/class/drm links it. Not
 *   run on a machine with no DRM node.
 * * `dri`: the first card's debugfs files, those IGT 2.0's CRC and
 *   connector-forcing code opens among them, as Linux 6.12 gives them:
 *   /sys/kernel/debug/dri/N a link to the device's directory, whose `name`
 *   reads "DRIVER dev=DIR unique=DIR" and whose `clients` begins with
 *   Linux's header. For its first connector, which the probe probes as the
 *   card's master: crtc-0/crc/control and crc/data; `force` reads
 *   "unspecified", takes "off", which the next probe reports as
 *   disconnected, and "unspecified" back; `edid_override`, opened with
 *   O_TRUNC as IGT opens it, takes an EDID of 520 by 290 mm, reads it back
 *   and, on a connector whose /sys `edid` reads the sink's, gives the next
 *   probe that size, where a driver that reads no EDID, as vkms, keeps the
 *   size it had, as on Linux, and "reset" empties it; and the
 *   connector's /sys `status` reads "disconnected" at once after "off" and
 *   "connected" after "detect", as status_store probes. Not run on a
 *   machine with no card, and the connector's part not on a card without
 *   one.
 * * `uevent`: a socket of NETLINK_KOBJECT_UEVENT bound to group 1 hears
 *   `add@DEVPATH` with ACTION, SUBSYSTEM, SYNTH_UUID=0 and the SEQNUM that
 *   /sys/kernel/uevent_seqnum then reads, from the kernel (port 0) to group
 *   1, once the probe writes "add" to the uevent of the first card, or of
 *   the first PCI function on a machine with no card. The write queues the
 *   message before it returns, so the receive does not wait. The message
 *   carries SCM_CREDENTIALS of the kernel, all 0, as libudev requires.
 * * `relay`: root's datagram to group 2 of NETLINK_KOBJECT_UEVENT, as
 *   udevd passes a device on to libudev's monitors, reaches a socket in
 *   that group from the sender's port, with the sender's process and user
 *   ids in its SCM_CREDENTIALS.
 *
 * Prints "SYSDRMPROBE OK" and exits 0 when every check passes, and
 * otherwise "SYSDRMPROBE FAIL: what" and exits 1 at the first failure;
 * etude's report (probe_report.h) has an assertion for each check, for the
 * case ETUDE_CASE names. Freestanding; direct system calls (probe.h).
 */

#include "probe.h"
#include "probe_report.h"

#define O_RDONLY    0
#define O_WRONLY    1
#define O_RDWR      2
#define O_TRUNC     01000
#define O_DIRECTORY 0200000
#define O_CLOEXEC   02000000

#define S_IFMT  0170000
#define S_IFDIR 0040000
#define S_IFCHR 0020000

#define AF_NETLINK             16
#define SOCK_DGRAM             2
#define NETLINK_KOBJECT_UEVENT 15
#define MSG_DONTWAIT           0x40
#define SOL_SOCKET             1
#define SO_PASSCRED            16
#define SCM_CREDENTIALS        2

#define DRM_MAJOR 226

#define S_IFLNK 0120000

/* Linux's DRM_IOCTL_SET_MASTER and DRM_IOCTL_MODE_GETCONNECTOR, whose
 * struct drm_mode_get_connector is 80 bytes on both architectures. */
#define DRM_IOCTL_SET_MASTER        0x641eul
#define DRM_IOCTL_MODE_GETCONNECTOR 0xc05064a7ul
#define GETCONN_ID                  48 /* connector_id */
#define GETCONN_CONNECTION          60
#define GETCONN_MM_WIDTH            64
#define DRM_CONNECTED               1
#define DRM_DISCONNECTED            2

/* Linux's sockaddr_nl, iovec, msghdr and cmsghdr, whose longs and
 * pointers give each architecture's layout. */
struct sockaddr_nl {
    unsigned short nl_family;
    unsigned short nl_pad;
    unsigned int nl_pid;
    unsigned int nl_groups;
};

struct iovec {
    void *iov_base;
    unsigned long iov_len;
};

struct msghdr {
    void *msg_name;
    unsigned int msg_namelen;
    struct iovec *msg_iov;
    unsigned long msg_iovlen;
    void *msg_control;
    unsigned long msg_controllen;
    int msg_flags;
};

struct cmsghdr {
    unsigned long cmsg_len;
    int cmsg_level;
    int cmsg_type;
};

/* The first resourceN file of a PCI function found, or "". */
static char bar_path[128];

/* The first card found, whose uevent the last check writes, or "", and its
 * path below /sys; and the node the `drm` directory is searched for. */
static char card_name[32];
static char card_devpath[160];
static char node_name[32];

/** Exit with `status`. */
static void sys_exit(int status)
{
    syscall1(SYS_EXIT, (long)status);
    __builtin_unreachable();
}

/** Say `what` failed, with `detail` after it, fail the current assertion
 * and exit 1. */
static void fail2(const char *what, const char *detail)
{
    probe_line("SYSDRMPROBE FAIL: ", what, detail);
    report_stop(what);
    sys_exit(1);
}

/** Say `what` failed, fail the current assertion with it, and exit 1. */
static void fail(const char *what)
{
    fail2(what, 0);
}

/** The length of the string `s`. */
static int str_len(const char *s)
{
    int n = 0;

    while (s[n]) {
        n++;
    }
    return n;
}

/** Whether the strings `a` and `b` are equal. */
static int str_eq(const char *a, const char *b)
{
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

/** Whether `s` begins with `prefix`. */
static int starts(const char *s, const char *prefix)
{
    while (*prefix && *s == *prefix) {
        s++;
        prefix++;
    }
    return *prefix == '\0';
}

/** Write the strings `a`, `b` and `c`, each NULL for none, one after the
 * other into `out` of `cap` bytes, terminated. */
static void join(char *out, int cap, const char *a, const char *b, const char *c)
{
    const char *pieces[3];
    int n = 0, i;

    pieces[0] = a;
    pieces[1] = b;
    pieces[2] = c;
    for (i = 0; i < 3; i++) {
        const char *s = pieces[i];

        while (s && *s && n < cap - 1) {
            out[n++] = *s++;
        }
    }
    out[n] = '\0';
}

/** Write the decimal `v` into `out`, terminated. */
static void decimal(char *out, unsigned long v)
{
    char d[24];
    int i = 0, n = 0;

    do {
        d[i++] = (char)('0' + v % 10);
        v /= 10;
    } while (v);
    while (i > 0) {
        out[n++] = d[--i];
    }
    out[n] = '\0';
}

/** Read the file `path` into `buf` of `cap` bytes, terminated: its length,
 * or a negated errno. */
static long read_file(const char *path, char *buf, long cap)
{
    long fd = syscall3(SYS_OPEN, (long)path, O_RDONLY | O_CLOEXEC, 0), n, got = 0;

    if (fd < 0) {
        return fd;
    }
    while (got < cap - 1 && (n = syscall3(SYS_READ, fd, (long)(buf + got), cap - 1 - got)) > 0) {
        got += n;
    }
    syscall1(SYS_CLOSE, fd);
    buf[got] = '\0';
    return n < 0 ? n : got;
}

/** The text of the link `path` into `buf` of `cap` bytes, terminated: its
 * length, or a negated errno. */
static long read_link(const char *path, char *buf, long cap)
{
    long n = syscall3(SYS_READLINK, (long)path, (long)buf, cap - 1);

    buf[n > 0 ? n : 0] = '\0';
    return n;
}

/** The mode of `path` by stat, with its device numbers in `*rdev` when it
 * is not NULL: the mode, or 0 for a path that is not there. */
static unsigned file_mode(const char *path, unsigned long long *rdev)
{
    unsigned long long st[KSTAT_SIZE / 8 + 1];

    if (syscall2(SYS_STAT, (long)path, (long)st) != 0) {
        return 0;
    }
    if (rdev) {
        *rdev = *(unsigned long long *)((char *)st + KSTAT_RDEV);
    }
    return *(unsigned *)((char *)st + KSTAT_MODE);
}

/** The last name of `path`. */
static const char *base_name(const char *path)
{
    const char *b = path;

    for (; *path; path++) {
        if (*path == '/' && path[1]) {
            b = path + 1;
        }
    }
    return b;
}

/** The value of the hex number at `s`, after an optional 0x, up to the
 * first character that is not a digit. */
static unsigned long hex(const char *s)
{
    unsigned long v = 0;

    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        s += 2;
    }
    for (;; s++) {
        int d = *s >= '0' && *s <= '9'   ? *s - '0'
                : *s >= 'a' && *s <= 'f' ? *s - 'a' + 10
                : *s >= 'A' && *s <= 'F' ? *s - 'A' + 10
                                         : -1;

        if (d < 0) {
            return v;
        }
        v = v << 4 | (unsigned long)d;
    }
}

/** The value of the variable `key` ("KEY=") among the lines of `text`, or
 * NULL; the value runs to the end of its line. */
static const char *var_of(const char *text, const char *key)
{
    while (*text) {
        if (starts(text, key)) {
            return text + str_len(key);
        }
        while (*text && *text != '\n') {
            text++;
        }
        if (*text) {
            text++;
        }
    }
    return 0;
}

/** Whether the value at `v`, ended by a newline or NUL, is `want`. */
static int value_is(const char *v, const char *want)
{
    while (*want && *v == *want) {
        v++;
        want++;
    }
    return *want == '\0' && (*v == '\n' || *v == '\0');
}

/**
 * Call `each` with the name of each entry of the directory `path` but . and
 * .., and `arg`: the number of entries, or a negated errno. A directory
 * whose entries are only . and .. counts 0.
 */
static long each_entry(const char *path, void (*each)(const char *, void *), void *arg)
{
    char d[2048];
    long fd = syscall3(SYS_OPEN, (long)path, O_RDONLY | O_DIRECTORY | O_CLOEXEC, 0), n, at;
    long count = 0;

    if (fd < 0) {
        return fd;
    }
    while ((n = syscall3(SYS_GETDENTS64, fd, (long)d, (long)sizeof(d))) > 0) {
        for (at = 0; at < n; at += *(unsigned short *)(d + at + 16)) {
            const char *name = d + at + 19;

            if (str_eq(name, ".") || str_eq(name, "..")) {
                continue;
            }
            count++;
            if (each) {
                each(name, arg);
            }
        }
    }
    syscall1(SYS_CLOSE, fd);
    return n < 0 ? n : count;
}

/** Stat `path` into `st`, or fail naming it. */
static void stat_of(const char *path, unsigned long long *st)
{
    if (syscall2(SYS_STAT, (long)path, (long)st) != 0) {
        fail2("no stat of ", path);
    }
}

/** The device and inode numbers of the stat `st`, as one comparison. */
static int same_file(const unsigned long long *a, const unsigned long long *b)
{
    return *(const unsigned long long *)((const char *)a + KSTAT_DEV) ==
               *(const unsigned long long *)((const char *)b + KSTAT_DEV) &&
           *(const unsigned long long *)((const char *)a + KSTAT_INO) ==
               *(const unsigned long long *)((const char *)b + KSTAT_INO);
}

/** Count sync's two files, sw_sync and info, by their names. */
static void sync_file(const char *name, void *arg)
{
    if (str_eq(name, "sw_sync") || str_eq(name, "info")) {
        ++*(int *)arg;
    }
}

/**
 * debugfs is mounted at /sys/kernel/debug, root's with mode 0700, on a
 * device of its own, and `..` there is /sys/kernel; it lists sync with
 * sw_sync and info, which root opens, and a process of uid 65534 is refused
 * at the mount's root.
 */
static void debugfs_row(void)
{
    unsigned long long dbg[KSTAT_SIZE / 8 + 1], up[KSTAT_SIZE / 8 + 1], kern[KSTAT_SIZE / 8 + 1];
    unsigned mode;
    int count = 0, status = 0;
    long fd, pid;

    stat_of("/sys/kernel/debug", dbg);
    stat_of("/sys/kernel/debug/..", up);
    stat_of("/sys/kernel", kern);
    mode = *(unsigned *)((char *)dbg + KSTAT_MODE);
    if (mode != (S_IFDIR | 0700) || *(unsigned *)((char *)dbg + KSTAT_UID) != 0) {
        fail("/sys/kernel/debug is not root's directory of mode 0700");
    }
    if (!same_file(up, kern)) {
        fail("/sys/kernel/debug/.. is not /sys/kernel");
    }
    if (*(unsigned long long *)((char *)dbg + KSTAT_DEV) ==
        *(unsigned long long *)((char *)kern + KSTAT_DEV)) {
        fail("/sys/kernel/debug is on /sys's device, not a mount of its own");
    }
    if (each_entry("/sys/kernel/debug/sync", sync_file, &count) < 0 || count != 2) {
        fail("/sys/kernel/debug/sync does not list sw_sync and info");
    }
    fd = syscall3(SYS_OPEN, (long)"/sys/kernel/debug/sync/sw_sync", 2 | O_CLOEXEC, 0);
    if (fd < 0) {
        fail("root cannot open /sys/kernel/debug/sync/sw_sync");
    }
    syscall1(SYS_CLOSE, fd);
    pid = syscall0(SYS_FORK);
    if (pid == 0) {
        syscall3(SYS_SETRESUID, 65534, 65534, 65534);
        fd = syscall3(SYS_OPEN, (long)"/sys/kernel/debug/sync/info", O_RDONLY, 0);
        sys_exit(fd == -13 ? 0 : 1);
    }
    if (pid < 0 || syscall3(SYS_WAITPID, pid, (long)&status, 0) != pid || status != 0) {
        fail("uid 65534 is not refused below /sys/kernel/debug with EACCES");
    }
    report_held();
}

/** /sys/class/drm is a directory with Linux's `version`. */
static void class_row(void)
{
    char text[64];

    if ((file_mode("/sys/class/drm", 0) & S_IFMT) != S_IFDIR) {
        fail("/sys/class/drm is no directory");
    }
    if (read_file("/sys/class/drm/version", text, sizeof(text)) < 0 ||
        !str_eq(text, "drm 1.1.0 20060810\n")) {
        fail("/sys/class/drm/version is not drm 1.1.0 20060810");
    }
    report_held();
}

/** Read the file `name` of the device directory `dir` into `buf` of `cap`
 * bytes, or fail naming it. */
static long dev_file(const char *dir, const char *name, char *buf, long cap)
{
    char path[160];
    long n;

    join(path, sizeof(path), dir, "/", name);
    n = read_file(path, buf, cap);
    if (n <= 0) {
        fail2("no file ", path);
    }
    return n;
}

/** The identity a PCI device's directory gives in its files. */
struct pci_ident {
    unsigned long vendor, device, subvendor, subdevice, class, revision;
};

/** Read the identity of the PCI device's directory `dir` from its files,
 * checking each is 0x and as many lowercase digits as pci-sysfs.c writes,
 * and that `config` gives the same at its offsets. */
static void pci_files(const char *dir, struct pci_ident *id)
{
    static const char *const names[] = {"vendor",           "device", "subsystem_vendor",
                                        "subsystem_device", "class",  "revision"};
    static const int lens[] = {7, 7, 7, 7, 9, 5};
    unsigned long *v[6];
    unsigned char config[256];
    char text[32];
    int i, k;

    v[0] = &id->vendor;
    v[1] = &id->device;
    v[2] = &id->subvendor;
    v[3] = &id->subdevice;
    v[4] = &id->class;
    v[5] = &id->revision;
    for (i = 0; i < 6; i++) {
        if (dev_file(dir, names[i], text, sizeof(text)) != lens[i] || !starts(text, "0x")) {
            fail2("not pci-sysfs.c's form: ", names[i]);
        }
        for (k = 2; k < lens[i] - 1; k++) {
            if (!((text[k] >= '0' && text[k] <= '9') || (text[k] >= 'a' && text[k] <= 'f'))) {
                fail2("not lowercase hex: ", names[i]);
            }
        }
        *v[i] = hex(text);
    }
    if (dev_file(dir, "config", (char *)config, sizeof(config)) < 64) {
        fail2("a config shorter than its header: ", dir);
    }
    if ((unsigned long)(config[0] | config[1] << 8) != id->vendor ||
        (unsigned long)(config[2] | config[3] << 8) != id->device || config[8] != id->revision ||
        (unsigned long)(config[9] | config[10] << 8 | config[11] << 16) != id->class) {
        fail2("config disagrees with the files: ", dir);
    }
}

/** Whether the uevent text `ev` gives the IDs of `id` as PCI_ID, in
 * uppercase as Linux writes them. */
static int pci_id_matches(const char *ev, const struct pci_ident *id)
{
    const char *v = var_of(ev, "PCI_ID=");

    return v && v[4] == ':' && hex(v) == id->vendor && hex(v + 5) == id->device &&
           !(v[0] >= 'a' && v[0] <= 'f');
}

/** Check the PCI function `name` of /sys/bus/pci/devices; `arg` counts. */
static void pci_function(const char *name, void *arg)
{
    struct pci_ident id;
    char dir[96], path[128], text[512];
    const char *slot;
    int vga, bar;

    join(dir, sizeof(dir), "/sys/bus/pci/devices/", name, 0);
    pci_files(dir, &id);
    dev_file(dir, "uevent", text, sizeof(text));
    slot = var_of(text, "PCI_SLOT_NAME=");
    if (!pci_id_matches(text, &id) || !slot || !value_is(slot, name)) {
        fail2("the uevent disagrees with the files: ", name);
    }
    join(path, sizeof(path), dir, "/subsystem", 0);
    if (read_link(path, text, sizeof(text)) <= 0 || !str_eq(base_name(text), "pci")) {
        fail2("no subsystem link to the PCI bus: ", name);
    }
    join(path, sizeof(path), dir, "/boot_vga", 0);
    vga = (id.class >> 8) == 0x0300;
    if ((file_mode(path, 0) != 0) != vga) {
        fail2(vga ? "a VGA function without boot_vga: " : "boot_vga on a function not VGA: ", name);
    }
    if (vga && (read_file(path, text, sizeof(text)) != 2 || (text[0] != '0' && text[0] != '1'))) {
        fail2("boot_vga is not 0 or 1: ", name);
    }
    for (bar = '0'; !bar_path[0] && bar <= '5'; bar++) {
        text[0] = (char)bar;
        text[1] = '\0';
        join(path, sizeof(path), dir, "/resource", text);
        if (file_mode(path, 0) != 0) {
            join(bar_path, sizeof(bar_path), path, 0, 0);
        }
    }
    ++*(int *)arg;
}

/** In a child: 0 when root is refused `bar_path` after setfsuid(1000) and
 * opens it for reading and writing with every capability dropped, else the
 * step that failed. */
static int bar_file_child(void)
{
    /* capset's header, version 3 with the caller's pid, and its two words
     * of effective, permitted and inheritable sets, all empty. */
    static const unsigned int head[2] = {0x20080522u, 0}, sets[6] = {0, 0, 0, 0, 0, 0};
    long fd;

    syscall1(SYS_SETFSUID, 1000);
    fd = syscall3(SYS_OPEN, (long)bar_path, O_RDONLY | O_CLOEXEC, 0);
    if (fd != -13) {
        return 1;
    }
    syscall1(SYS_SETFSUID, 0);
    if (syscall2(SYS_CAPSET, (long)head, (long)sets) != 0) {
        return 2;
    }
    fd = syscall3(SYS_OPEN, (long)bar_path, O_RDWR | O_CLOEXEC, 0);
    return fd >= 0 ? 0 : 3;
}

/** Every PCI function has Linux's files, and there is at least one. */
static void pci_row(void)
{
    int count = 0;

    long pid;
    int status = 0;

    if (each_entry("/sys/bus/pci/devices", pci_function, &count) < 0 || count == 0) {
        fail("no function in /sys/bus/pci/devices");
    }
    if (!bar_path[0]) {
        fail("no function in /sys/bus/pci/devices has a resourceN file");
    }
    pid = syscall0(SYS_FORK);
    if (pid == 0) {
        sys_exit(bar_file_child());
    }
    if (pid < 0 || syscall3(SYS_WAITPID, pid, (long)&status, 0) != pid) {
        fail("the resourceN child was not reaped");
    }
    if (status == 1 << 8) {
        fail2("root opens a resourceN file after setfsuid(1000): ", bar_path);
    }
    if (status == 2 << 8) {
        fail("capset could not drop every capability");
    }
    if (status != 0) {
        fail2("uid 0 without capabilities cannot open its resourceN file: ", bar_path);
    }
    report_held();
}

/** Set `*(int *)arg` when `name` is the node's name in `node_name`. */
static void find_node(const char *name, void *arg)
{
    if (str_eq(name, node_name)) {
        *(int *)arg = 1;
    }
}

/** Find the PCI device of /sys/dev/char's `dir` by its identity, as libdrm
 * does: the slot in its uevent, its IDs in its files. The device is
 * `dir`/device, or for a card on a virtio device its parent, `dir`/device/..,
 * which libdrm's drmParseSubsystemType reads and whose name its
 * get_real_pci_path takes by cutting the virtio device's from the link. */
static void card_pci(const char *dir, const char *node, int virtio)
{
    struct pci_ident id;
    char dev[160], path[176], text[512];
    const char *slot;
    char *cut;

    join(dev, sizeof(dev), dir, virtio ? "/device/.." : "/device", 0);
    dev_file(dev, "uevent", text, sizeof(text));
    slot = var_of(text, "PCI_SLOT_NAME=");
    if (!slot) {
        fail2("no PCI_SLOT_NAME in the device's uevent of ", node);
    }
    pci_files(dev, &id);
    if (!pci_id_matches(text, &id)) {
        fail2("the device's uevent disagrees with its files: ", node);
    }
    join(path, sizeof(path), dir, "/device", 0);
    if (read_link(path, path, sizeof(path)) <= 0) {
        fail2("no device link of ", node);
    }
    if (virtio) {
        cut = (char *)base_name(path);
        if (!starts(cut, "virtio") || cut == path) {
            fail2("the device link does not lead to a virtio device: ", node);
        }
        cut[-1] = '\0';
    }
    if (!value_is(slot, base_name(path))) {
        fail2("the device link does not lead to the slot of ", node);
    }
}

/** Check the virtio device of /sys/dev/char's `dir`, as Linux's virtio.c
 * shows it: its uevent's DRIVER and MODALIAS, `device` and `vendor` as
 * "0x%04x", the same in `modalias`, and a `driver` link; then its parent's
 * subsystem, which must be PCI, as libdrm reads it through `device/..`. */
static void card_virtio(const char *dir, const char *node)
{
    char dev[160], path[176], text[512], device[16], vendor[16], head[32], alias[64];
    const char *drv, *mod;
    char *c;

    join(dev, sizeof(dev), dir, "/device", 0);
    dev_file(dev, "uevent", text, sizeof(text));
    drv = var_of(text, "DRIVER=");
    mod = var_of(text, "MODALIAS=virtio:d");
    if (!drv || !value_is(drv, "virtio_gpu") || !mod) {
        fail2("no DRIVER=virtio_gpu or virtio MODALIAS in the virtio device's uevent of ", node);
    }
    if (dev_file(dev, "device", device, sizeof(device)) != 7 || !starts(device, "0x") ||
        dev_file(dev, "vendor", vendor, sizeof(vendor)) != 7 || !starts(vendor, "0x")) {
        fail2("the virtio device's device or vendor is not 0x%04x: ", node);
    }
    device[6] = vendor[6] = '\0';
    join(head, sizeof(head), "virtio:d0000", device + 2, "v0000");
    join(alias, sizeof(alias), head, vendor + 2, "\n");
    for (c = alias; *c; c++) {
        if (*c >= 'a' && *c <= 'f') {
            *c = (char)(*c - 'a' + 'A');
        }
    }
    if (dev_file(dev, "modalias", text, sizeof(text)) <= 0 || !str_eq(text, alias)) {
        fail2("the virtio device's modalias is not its device and vendor: ", node);
    }
    join(path, sizeof(path), dev, "/driver", 0);
    if (read_link(path, text, sizeof(text)) <= 0 || !str_eq(base_name(text), "virtio_gpu")) {
        fail2("no driver link of the virtio device of ", node);
    }
    join(path, sizeof(path), dev, "/../subsystem", 0);
    if (read_link(path, text, sizeof(text)) <= 0 || !str_eq(base_name(text), "pci")) {
        fail2("the virtio device's parent is not on the PCI bus: ", node);
    }
    card_pci(dir, node, 1);
}

/** Check the DRM node `name` of /dev/dri as libdrm finds it, and keep the
 * first card's name and device path; `arg` counts the nodes. */
static void dri_node(const char *name, void *arg)
{
    unsigned long long rdev;
    unsigned long minor;
    char node[48], dir[64], path[96], text[512], num[16];
    int listed = 0;

    join(node, sizeof(node), "/dev/dri/", name, 0);
    if (!starts(name, "card") && !starts(name, "renderD")) {
        return;
    }
    if ((file_mode(node, &rdev) & S_IFMT) != S_IFCHR || ((rdev >> 8) & 0xfff) != DRM_MAJOR) {
        fail2("not a DRM character device: ", node);
    }
    minor = (unsigned long)((rdev & 0xff) | ((rdev >> 12) & 0xfff00));
    decimal(num, minor);
    join(dir, sizeof(dir), "/sys/dev/char/226:", num, 0);
    join(path, sizeof(path), dir, "/device/subsystem", 0);
    if (read_link(path, text, sizeof(text)) <= 0) {
        fail2("no device/subsystem link of ", dir);
    }
    if (str_eq(base_name(text), "pci")) {
        card_pci(dir, node, 0);
    } else if (str_eq(base_name(text), "virtio")) {
        card_virtio(dir, node);
    } else if (!str_eq(base_name(text), "platform")) {
        fail2("a device on neither PCI, virtio nor the platform bus: ", node);
    } else {
        join(path, sizeof(path), dir, "/device", 0);
        dev_file(path, "uevent", text, sizeof(text));
        if (!var_of(text, "DRIVER=") || !var_of(text, "MODALIAS=platform:")) {
            fail2("no DRIVER or MODALIAS in the platform device's uevent of ", node);
        }
    }
    join(node_name, sizeof(node_name), name, 0, 0);
    join(path, sizeof(path), dir, "/device/drm", 0);
    if (each_entry(path, find_node, &listed) < 0 || !listed) {
        fail2("the device's drm directory does not list ", name);
    }
    dev_file(dir, "dev", text, sizeof(text));
    join(path, sizeof(path), "226:", num, "\n");
    if (!str_eq(text, path)) {
        fail2("dev does not give the numbers of ", node);
    }
    dev_file(dir, "uevent", text, sizeof(text));
    join(path, sizeof(path), "dri/", name, 0);
    if (!var_of(text, "DEVNAME=") || !value_is(var_of(text, "DEVNAME="), path)) {
        fail2("the uevent's DEVNAME is not that of ", node);
    }
    join(path, sizeof(path), "/sys/class/drm/", name, 0);
    if (read_link(path, text, sizeof(text)) <= 0 || !starts(text, "../../devices/")) {
        fail2("/sys/class/drm does not link ", name);
    }
    if (starts(name, "card") && !card_name[0]) {
        join(card_name, sizeof(card_name), name, 0, 0);
        join(card_devpath, sizeof(card_devpath), text + 5, 0, 0); /* past "../.." */
    }
    ++*(int *)arg;
}

/** Each node of /dev/dri is found through /sys as libdrm finds it; not run
 * on a machine with none. */
static void cards_row(void)
{
    int count = 0;

    if (each_entry("/dev/dri", dri_node, &count) < 0 || count == 0) {
        report_skip("no DRM node on this machine");
        return;
    }
    report_held();
}

/** Write the `len` bytes at `data` to the file `path`, opened with
 * `flags` beside O_WRONLY: what write returns, or the open's negated
 * errno. */
static long write_file(const char *path, const void *data, long len, long flags)
{
    long fd = syscall3(SYS_OPEN, (long)path, O_WRONLY | O_CLOEXEC | flags, 0), n;

    if (fd < 0) {
        return fd;
    }
    n = syscall3(SYS_WRITE, fd, (long)data, len);
    syscall1(SYS_CLOSE, fd);
    return n;
}

/** Keep the name of the first connector of the card in `card_name`, past
 * its "cardN-", in the buffer at `arg`. */
static void first_connector(const char *name, void *arg)
{
    char *out = (char *)arg;
    int n = str_len(card_name);

    if (!out[0] && starts(name, card_name) && name[n] == '-') {
        join(out, 32, name + n + 1, 0, 0);
    }
}

/** An EDID 1.3 of one block: a digital sink 52 by 29 cm, 520 by 290 mm in
 * its 1024x768 timing at 60 Hz, with the checksum filled at its use. */
static unsigned char probe_edid[128] = {
    0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00, 0x48, 0x32, 0x02, 0x00, 0x01, 0x00, 0x00, 0x00,
    0x01, 0x22, 0x01, 0x03, 0x80, 0x34, 0x1d, 0x78, 0x0a, 0xee, 0x91, 0xa3, 0x54, 0x4c, 0x99, 0x26,
    0x0f, 0x50, 0x54, 0x21, 0x08, 0x00, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01,
    0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x64, 0x19, 0x00, 0x40, 0x41, 0x00, 0x26, 0x30, 0x18, 0x88,
    0x36, 0x00, 0x08, 0x22, 0x21, 0x00, 0x00, 0x18, 0x00, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};

/** Probe the connector `id` through the card `fd` as its master, as
 * drmModeGetConnector does: its connection, with its width in mm in
 * `*mm`. Fails at a refused call. */
static unsigned get_connector(long fd, unsigned id, unsigned *mm)
{
    unsigned long long req[10];

    for (*mm = 0; *mm < 10; ++*mm) {
        req[*mm] = 0;
    }
    *(unsigned *)((char *)req + GETCONN_ID) = id;
    if (syscall3(SYS_IOCTL, fd, (long)DRM_IOCTL_MODE_GETCONNECTOR, (long)req) != 0) {
        fail("GETCONNECTOR of the card's first connector is refused");
    }
    *mm = *(unsigned *)((char *)req + GETCONN_MM_WIDTH);
    return *(unsigned *)((char *)req + GETCONN_CONNECTION);
}

/** The connector `conn`'s file `file` in debugfs's `dir`, into `path`. */
static void conn_file(char *path, int cap, const char *dir, const char *conn, const char *file)
{
    join(path, cap, dir, "/", conn);
    join(path, cap, path, "/", file);
}

/** The connector `conn`'s file `file` in /sys, into `path`. */
static void conn_sys(char *path, int cap, const char *conn, const char *file)
{
    join(path, cap, "/sys/class/drm/", card_name, "-");
    join(path, cap, path, conn, "/");
    join(path, cap, path, file, 0);
}

/** `force` reads unspecified, takes off, which the next probe through `fd`
 * of the connector `id` sees, and unspecified back. */
static void dri_force(long fd, unsigned id, const char *dir, const char *conn)
{
    char path[160], text[64];
    unsigned mm;

    conn_file(path, sizeof(path), dir, conn, "force");
    if (read_file(path, text, sizeof(text)) < 0 || !str_eq(text, "unspecified\n")) {
        fail2("force does not read unspecified: ", path);
    }
    if (write_file(path, "off\n", 4, 0) != 4 || read_file(path, text, sizeof(text)) < 0 ||
        !str_eq(text, "off\n")) {
        fail2("force does not take off: ", path);
    }
    if (get_connector(fd, id, &mm) != DRM_DISCONNECTED) {
        fail2("the probe after force off does not find the connector disconnected: ", conn);
    }
    if (write_file(path, "unspecified", 11, 0) != 11 ||
        get_connector(fd, id, &mm) != DRM_CONNECTED) {
        fail2("force unspecified does not bring the connector back: ", conn);
    }
}

/** `edid_override`, opened with O_TRUNC, takes the probe's EDID and gives
 * it back, the next probe has its size where the driver reads an EDID and
 * its old size where it reads none, and reset empties it. */
static void dri_edid(long fd, unsigned id, const char *dir, const char *conn)
{
    char path[160], text[256], num[16];
    unsigned mm, old, i, sum = 0;
    int reads;

    /* Linux's probe uses the override where the driver reads an EDID, or
     * where its get_modes finds no mode; vkms's and simpledrm's find modes
     * without one. */
    conn_sys(path, sizeof(path), conn, "edid");
    reads = read_file(path, text, sizeof(text)) > 0;
    get_connector(fd, id, &old);
    conn_file(path, sizeof(path), dir, conn, "edid_override");
    for (i = 0; i < 127; i++) {
        sum += probe_edid[i];
    }
    probe_edid[127] = (unsigned char)(256 - (sum & 0xff));
    if (write_file(path, probe_edid, 128, O_TRUNC) != 128 ||
        read_file(path, text, sizeof(text)) != 128) {
        fail2("edid_override does not take and give back an EDID: ", path);
    }
    for (i = 0; i < 128; i++) {
        if ((unsigned char)text[i] != probe_edid[i]) {
            fail2("edid_override gives back other bytes: ", path);
        }
    }
    if (get_connector(fd, id, &mm) != DRM_CONNECTED || mm != (reads ? 520u : old)) {
        decimal(num, mm);
        fail2(reads ? "the probe after the override does not give 520 mm but "
                    : "the probe after the override changes the size of a connector without an "
                      "EDID to ",
              num);
    }
    if (write_file(path, "reset", 5, O_TRUNC) != 5 || read_file(path, text, sizeof(text)) != 0) {
        fail2("edid_override is not empty after reset: ", path);
    }
    if (get_connector(fd, id, &mm) != DRM_CONNECTED || mm != old) {
        fail2("the probe after reset does not give back the connector's own size: ", conn);
    }
}

/** The connector's /sys `status` reads disconnected at once after off and
 * connected after detect. */
static void dri_status(const char *conn)
{
    char sys[160], text[64];

    conn_sys(sys, sizeof(sys), conn, "status");
    if (write_file(sys, "off\n", 4, 0) != 4 || read_file(sys, text, sizeof(text)) < 0 ||
        !str_eq(text, "disconnected\n")) {
        fail2("status does not read disconnected at once after off: ", sys);
    }
    if (write_file(sys, "detect\n", 7, 0) != 7 || read_file(sys, text, sizeof(text)) < 0 ||
        !str_eq(text, "connected\n")) {
        fail2("status does not read connected after detect: ", sys);
    }
}

/**
 * The card's first connector `conn`: its CRTC's CRC files, forced off and
 * given an EDID through debugfs and forced through /sys, each seen by the
 * next probe, with everything set back; `dir` is the card's directory in
 * debugfs. The probe holds the card's master role for its probes.
 */
static void dri_connector(const char *dir, const char *conn)
{
    char path[160], sys[160], text[64];
    unsigned id, i;
    long fd;

    join(path, sizeof(path), dir, "/crtc-0/crc/control", 0);
    join(sys, sizeof(sys), dir, "/crtc-0/crc/data", 0);
    if (!file_mode(path, 0) || !file_mode(sys, 0)) {
        fail2("no crtc-0/crc/control or crc/data in ", dir);
    }
    conn_sys(sys, sizeof(sys), conn, "connector_id");
    if (read_file(sys, text, sizeof(text)) <= 0) {
        fail2("no connector_id of ", conn);
    }
    for (id = 0, i = 0; text[i] >= '0' && text[i] <= '9'; i++) {
        id = id * 10 + (unsigned)(text[i] - '0');
    }
    join(path, sizeof(path), "/dev/dri/", card_name, 0);
    fd = syscall3(SYS_OPEN, (long)path, O_RDWR | O_CLOEXEC, 0);
    if (fd < 0 || syscall3(SYS_IOCTL, fd, (long)DRM_IOCTL_SET_MASTER, 0) != 0) {
        fail2("the probe cannot be the master of ", path);
    }
    dri_force(fd, id, dir, conn);
    dri_edid(fd, id, dir, conn);
    dri_status(conn);
    syscall1(SYS_CLOSE, fd);
}

/**
 * The first card's debugfs files, as the header says; not run on a
 * machine with no card.
 */
static void dri_row(void)
{
    char link[64], target[64], dir[96], path[160], text[512], conn[32];
    unsigned long long st[KSTAT_SIZE / 8 + 1];
    const char *after;

    if (!card_name[0]) {
        report_skip("no DRM card on this machine");
        return;
    }
    join(link, sizeof(link), "/sys/kernel/debug/dri/", card_name + 4, 0);
    if (syscall2(SYS_LSTAT, (long)link, (long)st) != 0 ||
        (*(unsigned *)((char *)st + KSTAT_MODE) & S_IFMT) != S_IFLNK ||
        read_link(link, target, sizeof(target)) <= 0) {
        fail2("no link ", link);
    }
    join(dir, sizeof(dir), "/sys/kernel/debug/dri/", target, 0);
    if ((file_mode(dir, 0) & S_IFMT) != S_IFDIR) {
        fail2("the link does not lead to a directory: ", dir);
    }
    join(path, sizeof(path), link, "/name", 0);
    if (read_file(path, text, sizeof(text)) <= 0) {
        fail2("no name in ", link);
    }
    for (after = text; *after && *after != ' '; after++) {
    }
    join(path, sizeof(path), " dev=", target, 0);
    if (!starts(after, path)) {
        fail2("name does not give dev= the directory's name: ", text);
    }
    join(path, sizeof(path), " unique=", target, "\n");
    if (str_len(text) < str_len(path) || !str_eq(text + str_len(text) - str_len(path), path)) {
        fail2("name does not end with unique= the directory's name: ", text);
    }
    join(path, sizeof(path), dir, "/clients", 0);
    if (read_file(path, text, sizeof(text)) <= 0 ||
        !starts(text, "             command  tgid dev master a   uid      magic\n")) {
        fail2("clients does not begin with Linux's header: ", path);
    }
    conn[0] = '\0';
    join(path, sizeof(path), "/sys/class/drm/", card_name, "/");
    if (each_entry(path, first_connector, conn) < 0) {
        fail2("no listing of ", path);
    }
    if (conn[0]) {
        dri_connector(dir, conn);
    }
    report_held();
}

/** Keep the first entry's name in the buffer at `arg`. */
static void first_entry(const char *name, void *arg)
{
    char *out = (char *)arg;

    if (!out[0]) {
        join(out, 32, name, 0, 0);
    }
}

/** Whether the uevent message `msg` of `n` bytes holds the variable `var`,
 * a whole NUL-ended string. */
static int msg_has(const char *msg, long n, const char *var)
{
    long at = 0;

    while (at < n) {
        if (str_eq(msg + at, var)) {
            return 1;
        }
        at += str_len(msg + at) + 1;
    }
    return 0;
}

/** A socket of NETLINK_KOBJECT_UEVENT with SO_PASSCRED, bound to the
 * groups `groups`: its descriptor; fails the row on an error. */
static long uevent_socket(unsigned groups)
{
    struct sockaddr_nl sa;
    int one = 1;
    long fd = syscall3(SYS_SOCKET, AF_NETLINK, SOCK_DGRAM, NETLINK_KOBJECT_UEVENT);

    if (fd < 0) {
        fail("socket of NETLINK_KOBJECT_UEVENT");
    }
    if (syscall5(SYS_SETSOCKOPT, fd, SOL_SOCKET, SO_PASSCRED, (long)&one, sizeof(one)) != 0) {
        fail("SO_PASSCRED on a uevent socket");
    }
    sa.nl_family = AF_NETLINK;
    sa.nl_pad = 0;
    sa.nl_pid = 0;
    sa.nl_groups = groups;
    if (syscall3(SYS_BIND, fd, (long)&sa, sizeof(sa)) != 0) {
        fail("bind of a uevent socket");
    }
    return fd;
}

/**
 * Take a datagram queued on `fd` into `msg` of `cap` bytes, terminated,
 * without waiting, with its sender's address into `*sa` and the sender's
 * credentials, SCM_CREDENTIALS, into `cred`: its length, or a negated
 * errno; fails the row when no credentials came with it.
 */
static long recv_cred(long fd, char *msg, long cap, struct sockaddr_nl *sa, int cred[3])
{
    unsigned long control[8] = {0};
    struct iovec iov;
    struct msghdr mh;
    struct cmsghdr *c = (struct cmsghdr *)control;
    long n;

    iov.iov_base = msg;
    iov.iov_len = (unsigned long)cap - 1;
    mh.msg_name = sa;
    mh.msg_namelen = sizeof(*sa);
    mh.msg_iov = &iov;
    mh.msg_iovlen = 1;
    mh.msg_control = control;
    mh.msg_controllen = sizeof(control);
    mh.msg_flags = 0;
    n = syscall3(SYS_RECVMSG, fd, (long)&mh, MSG_DONTWAIT);
    if (n < 0) {
        return n;
    }
    msg[n] = '\0';
    if (mh.msg_controllen < sizeof(*c) + 12 || c->cmsg_level != SOL_SOCKET ||
        c->cmsg_type != SCM_CREDENTIALS) {
        fail("a datagram without SCM_CREDENTIALS on a socket with SO_PASSCRED");
    }
    cred[0] = ((int *)(c + 1))[0];
    cred[1] = ((int *)(c + 1))[1];
    cred[2] = ((int *)(c + 1))[2];
    return n;
}

/** Make the uevent file the uevent row writes, `path`, and the head of the
 * uevent it sends, `head`, and its SUBSYSTEM, `subsystem`: the first
 * card's, or the first PCI function's on a machine with none. */
static void uevent_target(char *path, char *head, char *subsystem)
{
    char fn[32];

    fn[0] = '\0';
    if (card_name[0]) {
        join(path, 192, "/sys/class/drm/", card_name, "/uevent");
        join(head, 200, "add@", card_devpath, 0);
        join(subsystem, 24, "SUBSYSTEM=drm", 0, 0);
    } else {
        each_entry("/sys/bus/pci/devices", first_entry, fn);
        join(path, 192, "/sys/bus/pci/devices/", fn, "/uevent");
        join(head, 200, "add@/devices/pci0000:00/", fn, 0);
        join(subsystem, 24, "SUBSYSTEM=pci", 0, 0);
    }
}

/**
 * A write of "add" to a uevent file is heard on NETLINK_KOBJECT_UEVENT,
 * from the kernel, port 0 and credentials 0, to group 1, as `add@DEVPATH`
 * with the device's variables and the sequence number
 * /sys/kernel/uevent_seqnum gives.
 */
static void uevent_row(void)
{
    struct sockaddr_nl sa;
    char path[192], head[200], devpath[208], msg[4096], text[32], subsystem[24];
    long fd = uevent_socket(1), wfd, n;
    int cred[3];

    uevent_target(path, head, subsystem);
    wfd = syscall3(SYS_OPEN, (long)path, O_WRONLY | O_CLOEXEC, 0);
    if (wfd < 0 || syscall3(SYS_WRITE, wfd, (long)"add\n", 4) != 4) {
        fail2("a write of add to ", path);
    }
    syscall1(SYS_CLOSE, wfd);
    join(devpath, sizeof(devpath), "DEVPATH=", head + 4, 0);
    do {
        n = recv_cred(fd, msg, sizeof(msg), &sa, cred);
        if (n <= 0) {
            fail2("no uevent heard after a write to ", path);
        }
    } while (!str_eq(msg, head));
    if (sa.nl_pid != 0 || sa.nl_groups != 1 || cred[0] != 0 || cred[1] != 0 || cred[2] != 0) {
        fail("the uevent is not from the kernel to group 1");
    }
    if (!msg_has(msg, n, "ACTION=add") || !msg_has(msg, n, subsystem) ||
        !msg_has(msg, n, "SYNTH_UUID=0") || !msg_has(msg, n, devpath)) {
        fail2("the uevent lacks ACTION, DEVPATH, SUBSYSTEM or SYNTH_UUID: ", head);
    }
    if (read_file("/sys/kernel/uevent_seqnum", text, sizeof(text)) <= 0) {
        fail("no /sys/kernel/uevent_seqnum");
    }
    text[str_len(text) - 1] = '\0';
    join(path, sizeof(path), "SEQNUM=", text, 0);
    if (!msg_has(msg, n, path)) {
        fail("the uevent's SEQNUM is not uevent_seqnum");
    }
    syscall1(SYS_CLOSE, fd);
    report_held();
}

/**
 * Root's datagram to group 2 of NETLINK_KOBJECT_UEVENT, as udevd passes a
 * device on to libudev's monitors, reaches a socket in that group whole,
 * from the sender's port with the sender's process and user ids.
 */
static void relay_row(void)
{
    static const char relay[] = "libudev\0sysdrmprobe";
    struct sockaddr_nl sa, to;
    char msg[64];
    long from = uevent_socket(0), in = uevent_socket(2), n;
    unsigned salen = sizeof(sa);
    int cred[3], i;

    if (syscall3(SYS_GETSOCKNAME, from, (long)&sa, (long)&salen) != 0 || sa.nl_pid == 0) {
        fail("no port of the sending uevent socket");
    }
    to.nl_family = AF_NETLINK;
    to.nl_pad = 0;
    to.nl_pid = 0;
    to.nl_groups = 2;
    if (syscall6(SYS_SENDTO, from, (long)relay, sizeof(relay), 0, (long)&to, sizeof(to)) !=
        (long)sizeof(relay)) {
        fail("root's send to uevent group 2");
    }
    n = recv_cred(in, msg, sizeof(msg), &to, cred);
    if (n != (long)sizeof(relay)) {
        fail("no datagram heard in uevent group 2");
    }
    for (i = 0; i < n; i++) {
        if (msg[i] != relay[i]) {
            fail("the datagram in uevent group 2 is not the one sent");
        }
    }
    if (to.nl_pid != sa.nl_pid || to.nl_groups != 2 || cred[0] != (int)syscall0(SYS_GETPID) ||
        cred[1] != 0) {
        fail("the datagram in group 2 is not from the sender's port and ids");
    }
    syscall1(SYS_CLOSE, in);
    syscall1(SYS_CLOSE, from);
    report_held();
}

void probe_main(unsigned long *sp);

PROBE_SP_ENTRY;

/** Run the rows in order; `_start` calls it with the initial stack. */
void probe_main(unsigned long *sp)
{
    static const char *const ids[] = {"debugfs", "class", "pci", "cards", "dri", "uevent", "relay"};
    unsigned i;

    report_begin(report_case((char **)(sp + 1) + sp[0] + 1, "guest.screen.sysdrmprobe"));
    for (i = 0; i < sizeof(ids) / sizeof(ids[0]); i++) {
        report_plan(ids[i]);
    }
    debugfs_row();
    class_row();
    pci_row();
    cards_row();
    dri_row();
    uevent_row();
    relay_row();
    report_end();
    probe_line("SYSDRMPROBE OK", 0, 0);
    sys_exit(0);
}
