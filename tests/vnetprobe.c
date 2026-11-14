/**
 * vnetprobe: a virtio network device as a process sees it: its interface's
 * link files, its place on the virtio bus, and the driver let go of and
 * bound again through /sys, with a ping before, between and after.
 *
 * The probe checks, in order:
 *
 * * `link-files`: /sys/class/net/IF's `carrier` reads 1, and `speed`,
 *   `duplex` and `mtu` read what the arguments give, -1, "unknown" and 1500
 *   unless `speed=`, `duplex=` and `mtu=` name others: a virtio device says
 *   its speed and duplex only with VIRTIO_NET_F_SPEED_DUPLEX, and Linux's
 *   files read -1 and "unknown" without it.
 * * `bus`: the first device of /sys/bus/virtio/devices whose `device` reads
 *   0x0001, a network device, has a `driver` link whose last name is
 *   `virtio_net`, as Linux's.
 * * `ping`: a ping to the gateway gets a reply.
 * * `unbind`: the device's PCI function, the name above it in the link
 *   /sys/bus/virtio/devices/virtioN, written to
 *   /sys/bus/pci/drivers/virtio-pci/unbind, takes the device off the bus:
 *   no network device is left there.
 * * `no-carrier`: the interface stays, as the kernel keeps every interface
 *   it made at boot, with `carrier` reading 0, and a ping to the gateway
 *   fails.
 * * `bind`: the function's name written to /sys/bus/pci/drivers/virtio-pci/
 *   bind brings the device back on the bus with its `driver` link, and the
 *   interface's `carrier` reads 1.
 * * `ping-again`: a ping to the gateway gets a reply over the device bound
 *   again.
 *
 * The interface is `eth0` unless `if=` names another, and the gateway
 * 10.0.2.2 unless `gateway=` does.
 *
 * Prints "VNETPROBE OK" and exits 0 when every check held; otherwise exits
 * 1. Beside its own lines it writes etude's report records
 * (probe_report.h), one assertion for each check, for the case ETUDE_CASE
 * names or machine.virtionet.probe. Freestanding; direct system calls
 * (probe.h).
 */

#include "probe.h"
#include "probe_args.h"
#include "probe_report.h"

typedef unsigned int uint32_t;

#define O_RDONLY    0
#define O_WRONLY    1
#define O_DIRECTORY 0200000

#define GATEWAY_IP 0x0A000202UL /* 10.0.2.2, in host byte order */

#define BUS     "/sys/bus/virtio/devices/"
#define DRIVERS "/sys/bus/pci/drivers/virtio-pci/"

/* The directory entry of getdents64(2). */
struct dirent64_probe {
    unsigned long long d_ino;
    long long d_off;
    unsigned short d_reclen;
    unsigned char d_type;
    char d_name[1];
};

static int failures;

/** exit(2); does not return. */
static void sys_exit(int status)
{
    syscall1(SYS_EXIT, (long)status);
    __builtin_unreachable();
}

/** The length of the string `s`. */
static size_t length(const char *s)
{
    size_t n = 0;

    while (s[n]) {
        n++;
    }
    return n;
}

/** Whether the strings `a` and `b` are equal. */
static int same(const char *a, const char *b)
{
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

/** Write the string `s` and a newline to standard output. */
static void say(const char *s)
{
    syscall3(SYS_WRITE, 1, (long)s, (long)length(s));
    syscall3(SYS_WRITE, 1, (long)"\n", 1);
}

/** Put the strings `a`, `b` and `c` together in `out` of `cap` bytes, cut
 * where it is full, and return `out`. */
static char *join(char *out, size_t cap, const char *a, const char *b, const char *c)
{
    const char *part[3];
    size_t n = 0, i, k;

    part[0] = a;
    part[1] = b;
    part[2] = c;
    for (i = 0; i < 3; i++) {
        for (k = 0; part[i][k] != '\0' && n + 1 < cap; k++) {
            out[n++] = part[i][k];
        }
    }
    out[n] = '\0';
    return out;
}

/** Read the file `path` into `buf` of `cap` bytes as a string, less its
 * newline; an empty string for a file that cannot be read. */
static void read_text(const char *path, char *buf, long cap)
{
    long fd = syscall3(SYS_OPEN, (long)path, O_RDONLY, 0), n = 0;

    if (fd >= 0) {
        n = syscall3(SYS_READ, fd, (long)buf, cap - 1);
        syscall1(SYS_CLOSE, fd);
    }
    if (n < 0) {
        n = 0;
    }
    if (n > 0 && buf[n - 1] == '\n') {
        n--;
    }
    buf[n] = '\0';
}

/** Write the string `text` to the file `path` in one write: 1 when the
 * write took all of it, else 0. */
static int write_text(const char *path, const char *text)
{
    long fd = syscall3(SYS_OPEN, (long)path, O_WRONLY, 0), n;

    if (fd < 0) {
        return 0;
    }
    n = syscall3(SYS_WRITE, fd, (long)text, (long)length(text));
    syscall1(SYS_CLOSE, fd);
    return n == (long)length(text);
}

/** Whether the file `file` of the interface `ifname` reads `want`. */
static int link_file(const char *ifname, const char *file, const char *want)
{
    char path[96], inner[64], got[32];

    join(inner, sizeof(inner), ifname, "/", file);
    join(path, sizeof(path), "/sys/class/net/", inner, "");
    read_text(path, got, sizeof(got));
    return same(got, want);
}

/**
 * Find the first network device of the virtio bus: its name, `virtioN`, in
 * `name` of `cap` bytes, and 1; 0 when the bus has none.
 */
static int find_device(char *name, size_t cap)
{
    char buf[1024], path[96], id[16];
    long fd = syscall3(SYS_OPEN, (long)BUS, O_RDONLY | O_DIRECTORY, 0), n, at;
    int found = 0;

    if (fd < 0) {
        return 0;
    }
    while (!found && (n = syscall3(SYS_GETDENTS64, fd, (long)buf, sizeof(buf))) > 0) {
        for (at = 0; at < n && !found;) {
            struct dirent64_probe *d = (struct dirent64_probe *)(buf + at);

            at += d->d_reclen;
            if (d->d_name[0] == '.') {
                continue;
            }
            read_text(join(path, sizeof(path), BUS, d->d_name, "/device"), id, sizeof(id));
            if (same(id, "0x0001")) {
                join(name, cap, d->d_name, "", "");
                found = 1;
            }
        }
    }
    syscall1(SYS_CLOSE, fd);
    return found;
}

/** The last name of the path `path`. */
static const char *last_name(const char *path)
{
    const char *at = path;

    for (; *path; path++) {
        if (*path == '/') {
            at = path + 1;
        }
    }
    return at;
}

/** Whether the virtio device `name` has a `driver` link whose last name is
 * `virtio_net`. */
static int driven(const char *name)
{
    char path[96], to[128];
    long n = syscall3(SYS_READLINK, (long)join(path, sizeof(path), BUS, name, "/driver"), (long)to,
                      sizeof(to) - 1);

    if (n <= 0) {
        return 0;
    }
    to[n] = '\0';
    return same(last_name(to), "virtio_net");
}

/**
 * Give in `out` of `cap` bytes the PCI function of the virtio device
 * `name`: the name above the device's in the link the bus has for it,
 * ../../../devices/pci0000:00/0000:BB:DD.F/virtioN. 1, or 0 when the link
 * does not read so.
 */
static int function_of(const char *name, char *out, size_t cap)
{
    char path[96], to[160];
    long n = syscall3(SYS_READLINK, (long)join(path, sizeof(path), BUS, name, ""), (long)to,
                      sizeof(to) - 1);
    long end, from;

    if (n <= 0) {
        return 0;
    }
    to[n] = '\0';
    for (end = n; end > 0 && to[end - 1] != '/'; end--) {
    }
    if (end < 2) {
        return 0;
    }
    to[end - 1] = '\0';
    for (from = end - 1; from > 0 && to[from - 1] != '/'; from--) {
    }
    join(out, cap, to + from, "", "");
    return length(out) == 12;
}

/** Ping `ip`: the round-trip time in ticks, or a negated errno. */
static int ping(uint32_t ip)
{
    unsigned long args[1];

    args[0] = ip;
    return (int)sock_call(SC_PING, args);
}

/** Report the check `id`, which held when `held` is not 0 and failed for
 * the reason `why` otherwise, and print its line. */
static void check(const char *id, int held, const char *why)
{
    char line[160];

    if (held) {
        report_pass(id);
        say(join(line, sizeof(line), "vnetprobe: ", id, ": ok"));
    } else {
        failures++;
        report_fail(id, why);
        say(join(line, sizeof(line), "VNETPROBE FAIL: ", id, ""));
        say(why);
    }
}

/** The address the string `s` gives in dots, in host byte order, or
 * `fallback` when `s` is NULL or no address. */
static uint32_t address(const char *s, uint32_t fallback)
{
    uint32_t ip = 0, part = 0;
    int dots = 0, digits = 0;

    for (; s != 0 && *s != '\0'; s++) {
        if (*s >= '0' && *s <= '9') {
            part = part * 10 + (uint32_t)(*s - '0');
            digits++;
        } else if (*s == '.' && digits > 0 && part < 256) {
            ip = ip << 8 | part;
            part = 0;
            digits = 0;
            dots++;
        } else {
            return fallback;
        }
    }
    return dots == 3 && digits > 0 && part < 256 ? ip << 8 | part : fallback;
}

/**
 * The checks of the device let go of and bound again: `unbind`,
 * `no-carrier`, `bind` and `ping-again`, for the virtio device `name` of
 * the interface `ifname`, pinging `gateway`. A check that cannot run after
 * one that failed is reported as not run.
 */
static void rebind(const char *name, const char *ifname, uint32_t gateway)
{
    char function[16], again[32];
    int gone;

    gone = function_of(name, function, sizeof(function)) &&
           write_text(DRIVERS "unbind", function) && !find_device(again, sizeof(again));
    check("unbind", gone, "the write of the function's name to unbind left the device on the bus");
    if (!gone) {
        report_notrun("no-carrier", "the device was not unbound");
        report_notrun("bind", "the device was not unbound");
        report_notrun("ping-again", "the device was not unbound");
        return;
    }
    check("no-carrier", link_file(ifname, "carrier", "0") && ping(gateway) < 0,
          "the unbound device's interface kept its carrier or a ping got a reply");
    gone = !(write_text(DRIVERS "bind", function) && find_device(again, sizeof(again)) &&
             driven(again) && link_file(ifname, "carrier", "1"));
    check("bind", !gone,
          "the write of the function's name to bind did not bring back the device, "
          "its driver and the interface's carrier");
    if (gone) {
        report_notrun("ping-again", "the device was not bound again");
        return;
    }
    check("ping-again", ping(gateway) >= 0, "a ping over the device bound again got no reply");
}

void probe_main(unsigned long *sp);

PROBE_SP_ENTRY;

/** The probe's entry: the checks in order, then the verdict. */
void probe_main(unsigned long *sp)
{
    const char *ifname;
    uint32_t gateway;
    char name[32];
    int found;

    probe_args((int)sp[0], (char **)(sp + 1));
    ifname = probe_arg("if", "eth0");
    gateway = address(probe_arg("gateway", 0), GATEWAY_IP);
    report_begin(report_case((char **)(sp + 2 + sp[0]), "machine.virtionet.probe"));

    check("link-files",
          link_file(ifname, "carrier", "1") &&
              link_file(ifname, "speed", probe_arg("speed", "-1")) &&
              link_file(ifname, "duplex", probe_arg("duplex", "unknown")) &&
              link_file(ifname, "mtu", probe_arg("mtu", "1500")),
          "the interface's carrier, speed, duplex or mtu file does not read as the device says");
    found = find_device(name, sizeof(name));
    check("bus", found && driven(name),
          "no network device of the virtio bus has a driver link to virtio_net");
    check("ping", ping(gateway) >= 0, "a ping to the gateway got no reply");
    if (found) {
        rebind(name, ifname, gateway);
    } else {
        report_notrun("unbind", "the bus has no network device");
        report_notrun("no-carrier", "the bus has no network device");
        report_notrun("bind", "the bus has no network device");
        report_notrun("ping-again", "the bus has no network device");
    }
    report_end();
    if (failures == 0) {
        say("VNETPROBE OK");
        sys_exit(0);
    }
    say("VNETPROBE FAILED");
    sys_exit(1);
}
