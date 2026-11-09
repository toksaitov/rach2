/**
 * sysdrmprobe: the /sys entries by which libdrm, Mesa's loader and libudev
 * find display devices, as Linux 6.12 gives them to root, and the uevent a
 * write of `add` to a device's uevent file sends.
 *
 * The probe checks, in order:
 *
 * * `debugfs`: /sys/kernel/debug is an empty directory, where debugfs is
 *   mounted.
 * * `class`: /sys/class/drm is a directory whose `version` reads
 *   "drm 1.1.0 20060810".
 * * `pci`: every function of /sys/bus/pci/devices has `vendor`, `device`,
 *   `subsystem_vendor`, `subsystem_device`, `class` and `revision` as
 *   pci-sysfs.c writes them, the same IDs at their offsets of `config` and
 *   in the PCI_ID of `uevent`, its own name as PCI_SLOT_NAME, a `subsystem`
 *   link to /sys/bus/pci, and `boot_vga` exactly when it is a VGA function.
 * * `cards`: each node of /dev/dri, found as libdrm's drmGetDevices2 finds
 *   it: its numbers from stat, the subsystem from the last name of
 *   /sys/dev/char/226:N/device/subsystem, and for a PCI device its slot
 *   from the device's uevent, which is the name its `device` link leads to,
 *   and its IDs from the device's files, which `config` repeats; for a
 *   platform device its uevent's DRIVER and MODALIAS. The device's `drm`
 *   directory lists the node, the node's `dev` gives its numbers and its
 *   uevent its DEVNAME, and /sys/class/drm links it. Not run on a machine
 *   with no DRM node.
 * * `uevent`: a socket of NETLINK_KOBJECT_UEVENT bound to group 1 hears
 *   `add@DEVPATH` with ACTION, SUBSYSTEM, SYNTH_UUID=0 and the SEQNUM that
 *   /sys/kernel/uevent_seqnum then reads, from the kernel (port 0) to group
 *   1, once the probe writes "add" to the uevent of the first card, or of
 *   the first PCI function on a machine with no card. The write queues the
 *   message before it returns, so the receive does not wait.
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
#define O_DIRECTORY 0200000
#define O_CLOEXEC   02000000

#define S_IFMT  0170000
#define S_IFDIR 0040000
#define S_IFCHR 0020000

#define AF_NETLINK             16
#define SOCK_DGRAM             2
#define NETLINK_KOBJECT_UEVENT 15
#define MSG_DONTWAIT           0x40

#define DRM_MAJOR 226

/* Linux's sockaddr_nl. */
struct sockaddr_nl {
    unsigned short nl_family;
    unsigned short nl_pad;
    unsigned int nl_pid;
    unsigned int nl_groups;
};

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

/** /sys/kernel/debug is a directory with no entry. */
static void debugfs_row(void)
{
    if ((file_mode("/sys/kernel/debug", 0) & S_IFMT) != S_IFDIR) {
        fail("/sys/kernel/debug is no directory");
    }
    if (each_entry("/sys/kernel/debug", 0, 0) != 0) {
        fail("/sys/kernel/debug is not empty");
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
    int vga;

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
    ++*(int *)arg;
}

/** Every PCI function has Linux's files, and there is at least one. */
static void pci_row(void)
{
    int count = 0;

    if (each_entry("/sys/bus/pci/devices", pci_function, &count) < 0 || count == 0) {
        fail("no function in /sys/bus/pci/devices");
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
 * does: the slot in its uevent, its IDs in its files. */
static void card_pci(const char *dir, const char *node)
{
    struct pci_ident id;
    char dev[160], path[176], text[512];
    const char *slot;

    join(dev, sizeof(dev), dir, "/device", 0);
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
    if (read_link(path, path, sizeof(path)) <= 0 || !value_is(slot, base_name(path))) {
        fail2("the device link does not lead to the slot of ", node);
    }
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
        card_pci(dir, node);
    } else if (!str_eq(base_name(text), "platform")) {
        fail2("a device on neither PCI nor the platform bus: ", node);
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

/**
 * A write of "add" to a uevent file is heard on NETLINK_KOBJECT_UEVENT,
 * from the kernel to group 1, as `add@DEVPATH` with the device's
 * variables and the sequence number /sys/kernel/uevent_seqnum gives.
 */
static void uevent_row(void)
{
    struct sockaddr_nl sa;
    char path[192], head[200], devpath[208], msg[4096], text[32], subsystem[24], fn[32];
    long fd, wfd, n;
    unsigned salen;

    fn[0] = '\0';
    if (card_name[0]) {
        join(path, sizeof(path), "/sys/class/drm/", card_name, "/uevent");
        join(head, sizeof(head), "add@", card_devpath, 0);
        join(subsystem, sizeof(subsystem), "SUBSYSTEM=drm", 0, 0);
    } else {
        each_entry("/sys/bus/pci/devices", first_entry, fn);
        join(path, sizeof(path), "/sys/bus/pci/devices/", fn, "/uevent");
        join(head, sizeof(head), "add@/devices/pci0000:00/", fn, 0);
        join(subsystem, sizeof(subsystem), "SUBSYSTEM=pci", 0, 0);
    }
    fd = syscall3(SYS_SOCKET, AF_NETLINK, SOCK_DGRAM, NETLINK_KOBJECT_UEVENT);
    if (fd < 0) {
        fail("socket of NETLINK_KOBJECT_UEVENT");
    }
    sa.nl_family = AF_NETLINK;
    sa.nl_pad = 0;
    sa.nl_pid = 0;
    sa.nl_groups = 1;
    if (syscall3(SYS_BIND, fd, (long)&sa, sizeof(sa)) != 0) {
        fail("bind to the kernel's uevent group");
    }
    wfd = syscall3(SYS_OPEN, (long)path, O_WRONLY | O_CLOEXEC, 0);
    if (wfd < 0 || syscall3(SYS_WRITE, wfd, (long)"add\n", 4) != 4) {
        fail2("a write of add to ", path);
    }
    syscall1(SYS_CLOSE, wfd);
    join(devpath, sizeof(devpath), "DEVPATH=", head + 4, 0);
    do {
        salen = sizeof(sa);
        n = syscall6(SYS_RECVFROM, fd, (long)msg, sizeof(msg) - 1, MSG_DONTWAIT, (long)&sa,
                     (long)&salen);
        if (n <= 0) {
            fail2("no uevent heard after a write to ", path);
        }
        msg[n] = '\0';
    } while (!str_eq(msg, head));
    if (sa.nl_pid != 0 || sa.nl_groups != 1) {
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

void probe_main(unsigned long *sp);

PROBE_SP_ENTRY;

/** Run the rows in order; `_start` calls it with the initial stack. */
void probe_main(unsigned long *sp)
{
    static const char *const ids[] = {"debugfs", "class", "pci", "cards", "uevent"};
    unsigned i;

    report_begin(report_case((char **)(sp + 1) + sp[0] + 1, "guest.screen.sysdrmprobe"));
    for (i = 0; i < sizeof(ids) / sizeof(ids[0]); i++) {
        report_plan(ids[i]);
    }
    debugfs_row();
    class_row();
    pci_row();
    cards_row();
    uevent_row();
    report_end();
    probe_line("SYSDRMPROBE OK", 0, 0);
    sys_exit(0);
}
