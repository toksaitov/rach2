/**
 * vnetprobe: a virtio network device as a process sees it: its interface's
 * link files, its place on the virtio bus, and the driver let go of and
 * bound again through /sys, with a ping before, between and after.
 *
 * The probe checks, in order:
 *
 * * `plugged`, with `plugged=1`, for a card hot plugged after boot: the
 *   probe prints "VNETPROBE: put the card in" and waits, without a bound
 *   of its own, until /sys/class/net/IF is there; the interface, which
 *   nothing configured, is down, as Linux leaves a new one, and is set up,
 *   given 10.0.2.15/24 and a default route as `configure` below does, after
 *   which it reads a `carrier` of 1. The card is put in while the probe
 *   runs, since ACPICA's thread, which runs the GPE method that announces
 *   it, starts with the first program and waits while the debug shell
 *   waits at its prompt.
 * * `link-files`: /sys/class/net/IF's `carrier` reads 1, and `speed`,
 *   `duplex` and `mtu` read what the arguments give, -1, "unknown" and 1500
 *   unless `speed=`, `duplex=` and `mtu=` name others: a virtio device says
 *   its speed and duplex only with VIRTIO_NET_F_SPEED_DUPLEX, and Linux's
 *   files read -1 and "unknown" without it.
 * * `bus`: the first device of /sys/bus/virtio/devices whose `device` reads
 *   0x0001, a network device, has a `driver` link whose last name is
 *   `virtio_net`, as Linux's, and the interface's `device` link leads to
 *   that device below its PCI function,
 *   ../../../devices/pci0000:00/0000:BB:DD.F/virtioN, the parent Linux
 *   gives a virtio interface, with the virtio device's files through it.
 * * `ping`: a ping to the gateway gets a reply.
 * * `unbind`: the device's PCI function, the name above it in the link
 *   /sys/bus/virtio/devices/virtioN, written to
 *   /sys/bus/pci/drivers/virtio-pci/unbind, takes the device off the bus:
 *   no network device is left there.
 * * `no-interface`: the interface went with it, as Linux's virtnet_remove
 *   unregisters it: /sys/class/net/IF is gone, SIOCGIFINDEX of its name
 *   fails with ENODEV, and a ping to the gateway fails.
 * * `bind`: the function's name written to /sys/bus/pci/drivers/virtio-pci/
 *   bind brings the device back on the bus with its `driver` link, and a
 *   new interface of the same name, the lowest free, whose `ifindex` is not
 *   the old one's, which is down, as Linux leaves a new interface, and whose
 *   `device` link leads to the device.
 * * `configure`: the interface set up with SIOCSIFFLAGS, given 10.0.2.15/24
 *   with SIOCSIFADDR and SIOCSIFNETMASK and a default route through the
 *   gateway with SIOCADDRT, as a program configures a new card, reads a
 *   `carrier` of 1.
 * * `ping-again`: a ping to the gateway gets a reply over the device bound
 *   again.
 *
 * * `gone`, with `plugged=1`, after the rows above: the probe prints
 *   "VNETPROBE: take the card out" and waits until /sys/class/net/IF is
 *   gone, after which SIOCGIFINDEX of its name fails with ENODEV.
 * * `back`, with `plugged=1`: the probe prints "VNETPROBE: put the card in
 *   again" and waits for the interface, which is down, is set up as
 *   `configure` does, and gets a reply to a ping to the gateway.
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

#define AF_INET    2
#define SOCK_DGRAM 2
#define SC_SOCKET  1

#define SIOCADDRT      0x890b
#define SIOCGIFFLAGS   0x8913
#define SIOCSIFFLAGS   0x8914
#define SIOCSIFADDR    0x8916
#define SIOCSIFNETMASK 0x891c
#define SIOCGIFINDEX   0x8933
#define IFF_UP         0x0001
#define RTF_UP         0x0001
#define RTF_GATEWAY    0x0002
#define ENODEV         19

/* struct ifreq and struct rtentry, by the word size: the ifreq's union at
 * 16, and the rtentry's destination, gateway, mask and flags. */
#if defined(__x86_64__)
#define IFREQ_SIZE 40
#define RT_SIZE    120
#define RT_DST     8
#define RT_GW      24
#define RT_MASK    40
#define RT_FLAGS   56
#else
#define IFREQ_SIZE 32
#define RT_SIZE    84
#define RT_DST     4
#define RT_GW      20
#define RT_MASK    36
#define RT_FLAGS   52
#endif

#define GUEST_IP  0x0A00020FUL /* 10.0.2.15 */
#define GUEST_NET 0xFFFFFF00UL

#define GATEWAY_IP 0x0A000202UL /* 10.0.2.2, in host byte order */

#define BUS     "/sys/bus/virtio/devices/"
#define DRIVERS "/sys/bus/pci/drivers/virtio-pci/"

/* A directory entry of getdents64(2), linux_dirent64: its length is at
 * offset 16 and its name begins at 19. */
#define DIRENT_RECLEN 16
#define DIRENT_NAME   19

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
        for (at = 0; at < n && !found; at += *(unsigned short *)(buf + at + DIRENT_RECLEN)) {
            const char *entry = buf + at + DIRENT_NAME;

            if (entry[0] == '.') {
                continue;
            }
            read_text(join(path, sizeof(path), BUS, entry, "/device"), id, sizeof(id));
            if (same(id, "0x0001")) {
                join(name, cap, entry, "", "");
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

/** Read the `device` link of the interface `ifname` into `to` of `cap`
 * bytes as a string, and give its path in `path` of `pathcap` bytes: the
 * link's length, or a negative number when the interface has none. */
static long parent_link(const char *ifname, char *path, size_t pathcap, char *to, size_t cap)
{
    char inner[64];
    long n;

    join(inner, sizeof(inner), ifname, "/device", "");
    join(path, pathcap, "/sys/class/net/", inner, "");
    n = syscall3(SYS_READLINK, (long)path, (long)to, (long)cap - 1);
    to[n > 0 ? n : 0] = '\0';
    return n;
}

/**
 * Whether the interface `ifname` has the virtio device `name` as its
 * parent, as Linux's has: its `device` link reads
 * ../../../devices/pci0000:00/FUNCTION/NAME, the device below its PCI
 * function, and the device's own `device` file, 0x0001, is read through
 * the link.
 */
static int parented(const char *ifname, const char *name)
{
    char path[96], file[112], to[160], want[160], function[16], below[64], id[16];

    if (parent_link(ifname, path, sizeof(path), to, sizeof(to)) <= 0 ||
        !function_of(name, function, sizeof(function))) {
        return 0;
    }
    join(below, sizeof(below), function, "/", name);
    join(want, sizeof(want), "../../../devices/pci0000:00/", below, "");
    read_text(join(file, sizeof(file), path, "/device", ""), id, sizeof(id));
    return same(to, want) && same(id, "0x0001");
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

/** Put the address `ip`, in host order, as a sockaddr_in at `sa`. */
static void put_sin(unsigned char *sa, uint32_t ip)
{
    int i;

    for (i = 0; i < 16; i++) {
        sa[i] = 0;
    }
    sa[0] = AF_INET;
    sa[4] = (unsigned char)(ip >> 24);
    sa[5] = (unsigned char)(ip >> 16);
    sa[6] = (unsigned char)(ip >> 8);
    sa[7] = (unsigned char)ip;
}

/** The ioctl `request` on `ifname` through a datagram socket, with `ifr`
 * cleared and named first and `fill` called on it when not NULL: the
 * call's result, a negated errno on failure, and `ifr` as the kernel left
 * it. */
static long if_ioctl(const char *ifname, uint32_t request, unsigned char *ifr,
                     void (*fill)(unsigned char *ifr))
{
    unsigned long args[3];
    long fd, r;
    int i;

    for (i = 0; i < IFREQ_SIZE; i++) {
        ifr[i] = 0;
    }
    for (i = 0; i < 15 && ifname[i] != '\0'; i++) {
        ifr[i] = (unsigned char)ifname[i];
    }
    if (fill) {
        fill(ifr);
    }
    args[0] = AF_INET;
    args[1] = SOCK_DGRAM;
    args[2] = 0;
    fd = sock_call(SC_SOCKET, args);
    if (fd < 0) {
        return fd;
    }
    r = syscall3(SYS_IOCTL, fd, (long)request, (long)ifr);
    syscall1(SYS_CLOSE, fd);
    return r;
}

/** Fill an ifreq with IFF_UP. */
static void fill_up(unsigned char *ifr)
{
    ifr[16] = IFF_UP;
}

/** Fill an ifreq with 10.0.2.15. */
static void fill_addr(unsigned char *ifr)
{
    put_sin(ifr + 16, GUEST_IP);
}

/** Fill an ifreq with 255.255.255.0. */
static void fill_mask(unsigned char *ifr)
{
    put_sin(ifr + 16, GUEST_NET);
}

/** The gateway SIOCADDRT is given, set before the call. */
static uint32_t route_gateway;

/** Add the default route through `route_gateway`, as `route add default
 * gw` does: 0, or a negated errno. */
static long add_default(void)
{
    unsigned char rt[RT_SIZE];
    unsigned long args[3];
    long fd, r;
    int i;

    for (i = 0; i < RT_SIZE; i++) {
        rt[i] = 0;
    }
    put_sin(rt + RT_DST, 0);
    put_sin(rt + RT_GW, route_gateway);
    put_sin(rt + RT_MASK, 0);
    rt[RT_FLAGS] = RTF_UP | RTF_GATEWAY;
    args[0] = AF_INET;
    args[1] = SOCK_DGRAM;
    args[2] = 0;
    fd = sock_call(SC_SOCKET, args);
    if (fd < 0) {
        return fd;
    }
    r = syscall3(SYS_IOCTL, fd, SIOCADDRT, (long)rt);
    syscall1(SYS_CLOSE, fd);
    return r;
}

/** Whether /sys/class/net/IF is there. */
static int listed(const char *ifname)
{
    char path[96];
    long fd = syscall3(SYS_OPEN, (long)join(path, sizeof(path), "/sys/class/net/", ifname, ""),
                       O_RDONLY | O_DIRECTORY, 0);

    if (fd >= 0) {
        syscall1(SYS_CLOSE, fd);
    }
    return fd >= 0;
}

/** Whether the interface `ifname` is down: SIOCGIFFLAGS answers without
 * IFF_UP. */
static int down(const char *ifname)
{
    unsigned char ifr[IFREQ_SIZE];

    return if_ioctl(ifname, SIOCGIFFLAGS, ifr, 0) == 0 && !(ifr[16] & IFF_UP);
}

/** Set the interface `ifname` up with 10.0.2.15/24 and the default route
 * through `gateway`: 1 when every call took. */
static int configure(const char *ifname, uint32_t gateway)
{
    unsigned char ifr[IFREQ_SIZE];

    route_gateway = gateway;
    return if_ioctl(ifname, SIOCSIFFLAGS, ifr, fill_up) == 0 &&
           if_ioctl(ifname, SIOCSIFADDR, ifr, fill_addr) == 0 &&
           if_ioctl(ifname, SIOCSIFNETMASK, ifr, fill_mask) == 0 && add_default() == 0;
}

/** Sleep for 10 ms. */
static void nap(void)
{
    static const long ts[2] = {0, 10000000};

    syscall2(SYS_NANOSLEEP, (long)ts, 0); /* interval: poll the interface's directory every 10 ms */
}

/** Ping `ip` up to five times, while the first replies may wait for ARP:
 * whether one was answered. */
static int ping_retry(uint32_t ip)
{
    int i;

    for (i = 0; i < 5; i++) {
        if (ping(ip) >= 0) {
            return 1;
        }
    }
    return 0;
}

/** Report each of the checks of the device bound again as not run, for the
 * reason `why`. */
static void notrun_from_bind(const char *why)
{
    report_notrun("bind", why);
    report_notrun("configure", why);
    report_notrun("ping-again", why);
}

/**
 * The checks of the device let go of and bound again: `unbind`,
 * `no-interface`, `bind`, `configure` and `ping-again`, for the virtio
 * device `name` of the interface `ifname`, pinging `gateway`. A check that
 * cannot run after one that failed is reported as not run.
 */
static void rebind(const char *name, const char *ifname, uint32_t gateway)
{
    char function[16], again[32], path[96], before[16], after[16] = "";
    unsigned char ifr[IFREQ_SIZE];
    int gone;

    read_text(join(path, sizeof(path), "/sys/class/net/", ifname, "/ifindex"), before,
              sizeof(before));
    gone = function_of(name, function, sizeof(function)) &&
           write_text(DRIVERS "unbind", function) && !find_device(again, sizeof(again));
    check("unbind", gone, "the write of the function's name to unbind left the device on the bus");
    if (!gone) {
        report_notrun("no-interface", "the device was not unbound");
        notrun_from_bind("the device was not unbound");
        return;
    }
    check("no-interface",
          !listed(ifname) && if_ioctl(ifname, SIOCGIFINDEX, ifr, 0) == -ENODEV && ping(gateway) < 0,
          "the unbound device's interface stayed in /sys/class/net or for SIOCGIFINDEX, or a "
          "ping got a reply");
    gone = !(write_text(DRIVERS "bind", function) && find_device(again, sizeof(again)) &&
             driven(again) && listed(ifname));
    if (!gone) {
        read_text(path, after, sizeof(after));
    }
    check("bind",
          !gone && after[0] != '\0' && !same(before, after) && down(ifname) &&
              parented(ifname, again),
          "the write of the function's name to bind did not bring back the device with its "
          "driver and a new interface of the same name, with a new index, down, whose device "
          "link leads to it");
    if (gone) {
        report_notrun("configure", "the device was not bound again");
        report_notrun("ping-again", "the device was not bound again");
        return;
    }
    gone = !(configure(ifname, gateway) && link_file(ifname, "carrier", "1"));
    check("configure", !gone,
          "the new interface could not be set up, given an address and a default route, or "
          "read no carrier once up");
    if (gone) {
        report_notrun("ping-again", "the new interface was not configured");
        return;
    }
    check("ping-again", ping_retry(gateway), "a ping over the device bound again got no reply");
}

/** Wait for the interface `ifname` to be there, `there`, or gone. */
static void wait_listed(const char *ifname, int there)
{
    while (!listed(ifname) != !there) {
        nap();
    }
}

/** Have the card taken out and put in again, the `gone` and `back`
 * checks. */
static void replug(const char *ifname, uint32_t gateway)
{
    unsigned char ifr[IFREQ_SIZE];

    say("VNETPROBE: take the card out");
    wait_listed(ifname, 0);
    check("gone", if_ioctl(ifname, SIOCGIFINDEX, ifr, 0) == -ENODEV,
          "SIOCGIFINDEX of the interface of the card taken out did not fail with ENODEV");
    say("VNETPROBE: put the card in again");
    wait_listed(ifname, 1);
    check("back", down(ifname) && configure(ifname, gateway) && ping_retry(gateway),
          "the interface of the card put in again was not down, could not be set up, or got no "
          "reply to a ping");
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

    if (same(probe_arg("plugged", "0"), "1")) {
        say("VNETPROBE: put the card in");
        wait_listed(ifname, 1);
        check("plugged",
              down(ifname) && configure(ifname, gateway) && link_file(ifname, "carrier", "1"),
              "the hot plugged card's interface was not down, or could not be set up, given an "
              "address and a default route");
    }
    check("link-files",
          link_file(ifname, "carrier", "1") &&
              link_file(ifname, "speed", probe_arg("speed", "-1")) &&
              link_file(ifname, "duplex", probe_arg("duplex", "unknown")) &&
              link_file(ifname, "mtu", probe_arg("mtu", "1500")),
          "the interface's carrier, speed, duplex or mtu file does not read as the device says");
    found = find_device(name, sizeof(name));
    check("bus", found && driven(name) && parented(ifname, name),
          "no network device of the virtio bus has a driver link to virtio_net, or the "
          "interface's device link does not lead to it");
    check("ping", ping(gateway) >= 0, "a ping to the gateway got no reply");
    if (found) {
        rebind(name, ifname, gateway);
    } else {
        report_notrun("unbind", "the bus has no network device");
        report_notrun("no-interface", "the bus has no network device");
        notrun_from_bind("the bus has no network device");
    }
    if (same(probe_arg("plugged", "0"), "1")) {
        replug(ifname, gateway);
    }
    report_end();
    if (failures == 0) {
        say("VNETPROBE OK");
        sys_exit(0);
    }
    say("VNETPROBE FAILED");
    sys_exit(1);
}
