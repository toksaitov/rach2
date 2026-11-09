/**
 * hotplugprobe: a display change reaches user space as Linux 6.12 sends it,
 * a "change" uevent of the card with HOTPLUG=1, after which the connector
 * lists the new size as its preferred mode.
 *
 * The probe takes the first card of /dev/dri/card0 to card15 that has a
 * connected connector, as its master, and reads that connector's preferred
 * mode, the first with DRM_MODE_TYPE_PREFERRED or else the first listed. It
 * binds a socket of NETLINK_KOBJECT_UEVENT to group 1 and prints
 * "HOTPLUGPROBE READY NAME" with the mode's name. Then, for each uevent
 * `change@` of SUBSYSTEM=drm with HOTPLUG=1, it prints "HOTPLUGPROBE EVENT
 * DEVPATH", reads the connector again, which probes it, and prints
 * "HOTPLUGPROBE MODE NAME"; once the preferred mode differs from the first,
 * it prints "HOTPLUGPROBE OK" and exits 0. A failure prints "HOTPLUGPROBE
 * FAIL: what" and exits 1. It waits for as many events as come, since the
 * change is made from outside, by resizing QEMU's window or by a VNC
 * client's SetDesktopSize, as rach2/sys/drm/Readme.md describes for
 * virtio-gpu. Freestanding; direct system calls (probe.h).
 */

#include "probe.h"

#define O_RDWR    2
#define O_CLOEXEC 02000000

#define AF_NETLINK             16
#define SOCK_DGRAM             2
#define NETLINK_KOBJECT_UEVENT 15

#define DRM_IOWR(nr, t) (0xC0000000UL | ((unsigned long)sizeof(t) << 16) | 0x6400UL | (nr))
#define DRM_IO(nr)      (0x6400UL | (nr))

typedef unsigned int u32;
typedef unsigned long long u64;

struct sockaddr_nl {
    unsigned short nl_family;
    unsigned short nl_pad;
    unsigned int nl_pid;
    unsigned int nl_groups;
};

struct drm_mode_card_res {
    u64 fb_id_ptr;
    u64 crtc_id_ptr;
    u64 connector_id_ptr;
    u64 encoder_id_ptr;
    u32 count_fbs;
    u32 count_crtcs;
    u32 count_connectors;
    u32 count_encoders;
    u32 min_width, max_width;
    u32 min_height, max_height;
};

struct drm_mode_modeinfo {
    u32 clock;
    unsigned short hdisplay, hsync_start, hsync_end, htotal, hskew;
    unsigned short vdisplay, vsync_start, vsync_end, vtotal, vscan;
    u32 vrefresh;
    u32 flags;
    u32 type;
    char name[32];
};

struct drm_mode_get_connector {
    u64 encoders_ptr;
    u64 modes_ptr;
    u64 props_ptr;
    u64 prop_values_ptr;
    u32 count_modes;
    u32 count_props;
    u32 count_encoders;
    u32 encoder_id;
    u32 connector_id;
    u32 connector_type;
    u32 connector_type_id;
    u32 connection;
    u32 mm_width;
    u32 mm_height;
    u32 subpixel;
    u32 pad;
};

#define DRM_IOCTL_SET_MASTER        DRM_IO(0x1e)
#define DRM_IOCTL_MODE_GETRESOURCES DRM_IOWR(0xA0, struct drm_mode_card_res)
#define DRM_IOCTL_MODE_GETCONNECTOR DRM_IOWR(0xA7, struct drm_mode_get_connector)

#define DRM_MODE_CONNECTED      1
#define DRM_MODE_TYPE_PREFERRED 0x8

/* The most connectors and modes read. */
#define MAX_IDS   16
#define MAX_MODES 128

/** End the process with `status`; it does not return. */
static void sys_exit(int status)
{
    syscall1(SYS_EXIT, (long)status);
    __builtin_unreachable();
}

/** Say `what` failed and exit 1. */
static void fail(const char *what)
{
    probe_line("HOTPLUGPROBE FAIL: ", what, 0);
    sys_exit(1);
}

/** Clear `n` bytes at `p`. */
static void zero(void *p, unsigned long n)
{
    char *c = (char *)p;

    while (n--) {
        *c++ = 0;
    }
}

/** The length of the string `s`. */
static long str_len(const char *s)
{
    long n = 0;

    while (s[n]) {
        n++;
    }
    return n;
}

/** Whether `s` begins with `prefix`. */
static int starts(const char *s, const char *prefix)
{
    while (*prefix) {
        if (*s++ != *prefix++) {
            return 0;
        }
    }
    return 1;
}

/** Whether the strings `a` and `b` are the same. */
static int same(const char *a, const char *b)
{
    return starts(a, b) && starts(b, a);
}

/** A DRM ioctl: 0 or a negated errno. */
static long drm_ioctl(int fd, unsigned long cmd, void *arg)
{
    return syscall3(SYS_IOCTL, fd, (long)cmd, (long)arg);
}

/**
 * The first connected connector of the card `fd` with modes, into `conn`,
 * and its preferred mode's name into `name` of 32 bytes: 1, or 0 when no
 * connector is connected. Reading the connector with no room for modes
 * probes it, since the caller is the master.
 */
static int preferred(int fd, struct drm_mode_get_connector *conn, char *name)
{
    static struct drm_mode_modeinfo modes[MAX_MODES];
    static u32 conns[MAX_IDS];
    struct drm_mode_card_res res;
    u32 i, j, n;

    zero(&res, sizeof(res));
    res.connector_id_ptr = (u64)(unsigned long)conns;
    res.count_connectors = MAX_IDS;
    if (drm_ioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, &res) != 0) {
        return 0;
    }
    for (i = 0; i < res.count_connectors && i < MAX_IDS; i++) {
        zero(conn, sizeof(*conn));
        conn->connector_id = conns[i];
        if (drm_ioctl(fd, DRM_IOCTL_MODE_GETCONNECTOR, conn) != 0) {
            fail("GETCONNECTOR's probe");
        }
        if (conn->connection != DRM_MODE_CONNECTED || conn->count_modes == 0) {
            continue;
        }
        n = conn->count_modes < MAX_MODES ? conn->count_modes : MAX_MODES;
        conn->count_modes = n;
        conn->modes_ptr = (u64)(unsigned long)modes;
        conn->count_encoders = 0;
        conn->count_props = 0;
        if (drm_ioctl(fd, DRM_IOCTL_MODE_GETCONNECTOR, conn) != 0) {
            fail("GETCONNECTOR's modes");
        }
        n = conn->count_modes < n ? conn->count_modes : n;
        for (j = 0; j < n && !(modes[j].type & DRM_MODE_TYPE_PREFERRED); j++) {
        }
        j = j < n ? j : 0;
        for (n = 0; n < 31 && modes[j].name[n]; n++) {
            name[n] = modes[j].name[n];
        }
        name[n] = '\0';
        return 1;
    }
    return 0;
}

/** Whether the uevent `msg` of `n` bytes holds the variable `var`, a whole
 * NUL-ended string. */
static int msg_has(const char *msg, long n, const char *var)
{
    long at = 0;

    while (at < n) {
        if (same(msg + at, var)) {
            return 1;
        }
        at += str_len(msg + at) + 1;
    }
    return 0;
}

void probe_main(unsigned long *sp);

PROBE_SP_ENTRY;

/** Find the card, wait for display changes, and judge the mode after each;
 * `_start` calls it with the initial stack. */
void probe_main(unsigned long *sp)
{
    static char msg[4096];
    struct drm_mode_get_connector conn;
    struct sockaddr_nl sa;
    char path[] = "/dev/dri/cardNN", first[32], now[32];
    long sock, n;
    int fd = -1, i;

    (void)sp;
    for (i = 0; i < 16; i++) {
        path[13] = (char)(i < 10 ? '0' + i : '1');
        path[14] = (char)(i < 10 ? '\0' : '0' + i - 10);
        fd = (int)syscall3(SYS_OPEN, (long)path, O_RDWR | O_CLOEXEC, 0);
        if (fd < 0) {
            continue;
        }
        (void)drm_ioctl(fd, DRM_IOCTL_SET_MASTER, 0);
        if (preferred(fd, &conn, first)) {
            break;
        }
        syscall1(SYS_CLOSE, fd);
        fd = -1;
    }
    if (fd < 0) {
        fail("no card has a connected connector");
    }
    sock = syscall3(SYS_SOCKET, AF_NETLINK, SOCK_DGRAM, NETLINK_KOBJECT_UEVENT);
    if (sock < 0) {
        fail("socket of NETLINK_KOBJECT_UEVENT");
    }
    zero(&sa, sizeof(sa));
    sa.nl_family = AF_NETLINK;
    sa.nl_groups = 1;
    if (syscall3(SYS_BIND, sock, (long)&sa, sizeof(sa)) != 0) {
        fail("bind of a uevent socket");
    }
    probe_line("HOTPLUGPROBE READY ", first, 0);
    for (;;) {
        n = syscall6(SYS_RECVFROM, sock, (long)msg, sizeof(msg) - 1, 0, 0, 0);
        if (n < 0) {
            fail("recvfrom of a uevent");
        }
        msg[n] = '\0';
        if (!starts(msg, "change@") || !msg_has(msg, n, "SUBSYSTEM=drm") ||
            !msg_has(msg, n, "HOTPLUG=1")) {
            continue;
        }
        probe_line("HOTPLUGPROBE EVENT ", msg + 7, 0);
        if (!preferred(fd, &conn, now)) {
            fail("the connector is gone after the event");
        }
        probe_line("HOTPLUGPROBE MODE ", now, 0);
        if (!same(now, first)) {
            break;
        }
    }
    probe_line("HOTPLUGPROBE OK", 0, 0);
    sys_exit(0);
}
