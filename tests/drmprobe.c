/**
 * drmprobe: a DRM card's nodes, master role and dumb buffers, as Linux 6.12
 * gives them to root, on whatever driver binds the first card that has dumb
 * buffers.
 *
 * The probe opens /dev/dri/card0 to card15 in turn and takes the first that
 * answers GET_CAP with DRM_CAP_DUMB_BUFFER set. It reads VERSION with a name,
 * opens the render node of the same index, whose VERSION gives the same name
 * and which refuses CREATE_DUMB with EACCES, or skips that row for a driver
 * with a card alone, as bochs-drm and simpledrm are, and checks the master
 * role: the card's descriptor drops it, a second open takes it and the first's
 * SET_MASTER is EBUSY, and back again; a magic of a third open is spent by
 * AUTH_MAGIC once and refused the second time. It creates a dumb buffer of 64
 * by 64 pixels at 32 bits, maps it with MAP_DUMB and mmap, writes a pattern
 * and reads it back through a second mapping, and destroys it: MAP_DUMB of the
 * handle is then ENOENT while the mappings still read the pattern, since they
 * hold the buffer until munmap. Last, PRIME exports a buffer, reuses
 * self-import and repeated import handles, and retains the mapping after its
 * handles and dma-buf descriptors close. kcmp(2) says a dup of the card's
 * descriptor is the same open file and the second open is not. Then, as the
 * master, it sets every mode the first connected connector lists, each on a
 * dumb buffer of its size shown on the first CRTC by SETCRTC, and reads it
 * back with GETCRTC; the buffer's pattern reads the same through its mapping
 * after the mode is set, since a driver may move a buffer to scan it out. Each
 * mode set prints "DRMPROBE MODE NAME".
 *
 * Prints "DRMPROBE OK" and exits 0 when every check passes, and otherwise
 * "DRMPROBE FAIL: what" and exits 1 at the first failure. etude's report
 * (probe_report.h), for the case ETUDE_CASE names, has an assertion for
 * each check in order: `open`, `version`, `render`, `master`, `auth`,
 * `create`, `map`, `destroy`, `prime`, `kcmp` and `modes`. The master rows
 * and `modes` skip while another process holds the role. Freestanding;
 * direct system calls (probe.h).
 */

#include "probe.h"
#include "probe_report.h"

#define O_RDWR    2
#define O_CLOEXEC 02000000

#define PROT_READ  0x1
#define PROT_WRITE 0x2
#define MAP_SHARED 0x01

#define ENOENT 2
#define EACCES 13
#define EBUSY  16
#define EINVAL 22

#if defined __x86_64__
#define SYS_KCMP 312
#else
#define SYS_KCMP 349
#endif
#define KCMP_FILE 0

/* Linux's ioctl numbers: the direction in the top two bits, the size of
 * the argument, the type 'd' and the number. */
#define DRM_IO(nr)      (0x6400UL | (nr))
#define DRM_IOR(nr, t)  (0x80000000UL | ((unsigned long)sizeof(t) << 16) | 0x6400UL | (nr))
#define DRM_IOW(nr, t)  (0x40000000UL | ((unsigned long)sizeof(t) << 16) | 0x6400UL | (nr))
#define DRM_IOWR(nr, t) (0xC0000000UL | ((unsigned long)sizeof(t) << 16) | 0x6400UL | (nr))

typedef unsigned int u32;
typedef unsigned long long u64;

struct drm_version {
    int version_major;
    int version_minor;
    int version_patchlevel;
    unsigned long name_len;
    char *name;
    unsigned long date_len;
    char *date;
    unsigned long desc_len;
    char *desc;
};

struct drm_get_cap {
    u64 capability;
    u64 value;
};

struct drm_auth {
    u32 magic;
};

struct drm_mode_create_dumb {
    u32 height;
    u32 width;
    u32 bpp;
    u32 flags;
    u32 handle;
    u32 pitch;
    u64 size;
};

struct drm_mode_map_dumb {
    u32 handle;
    u32 pad;
    u64 offset;
};

struct drm_gem_close {
    u32 handle;
    u32 pad;
};

struct drm_prime_handle {
    u32 handle;
    u32 flags;
    int fd;
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

struct drm_mode_crtc {
    u64 set_connectors_ptr;
    u32 count_connectors;
    u32 crtc_id;
    u32 fb_id;
    u32 x, y;
    u32 gamma_size;
    u32 mode_valid;
    struct drm_mode_modeinfo mode;
};

struct drm_mode_fb_cmd {
    u32 fb_id;
    u32 width;
    u32 height;
    u32 pitch;
    u32 bpp;
    u32 depth;
    u32 handle;
};

#define DRM_CLOEXEC 02000000
#define DRM_RDWR    2

struct drm_mode_destroy_dumb {
    u32 handle;
};

#define DRM_IOCTL_GEM_CLOSE          DRM_IOW(0x09, struct drm_gem_close)
#define DRM_IOCTL_PRIME_HANDLE_TO_FD DRM_IOWR(0x2d, struct drm_prime_handle)
#define DRM_IOCTL_PRIME_FD_TO_HANDLE DRM_IOWR(0x2e, struct drm_prime_handle)

#define DRM_IOCTL_VERSION           DRM_IOWR(0x00, struct drm_version)
#define DRM_IOCTL_GET_MAGIC         DRM_IOR(0x02, struct drm_auth)
#define DRM_IOCTL_GET_CAP           DRM_IOWR(0x0c, struct drm_get_cap)
#define DRM_IOCTL_AUTH_MAGIC        DRM_IOW(0x11, struct drm_auth)
#define DRM_IOCTL_SET_MASTER        DRM_IO(0x1e)
#define DRM_IOCTL_DROP_MASTER       DRM_IO(0x1f)
#define DRM_IOCTL_MODE_CREATE_DUMB  DRM_IOWR(0xB2, struct drm_mode_create_dumb)
#define DRM_IOCTL_MODE_MAP_DUMB     DRM_IOWR(0xB3, struct drm_mode_map_dumb)
#define DRM_IOCTL_MODE_DESTROY_DUMB DRM_IOWR(0xB4, struct drm_mode_destroy_dumb)

#define DRM_IOCTL_MODE_GETRESOURCES DRM_IOWR(0xA0, struct drm_mode_card_res)
#define DRM_IOCTL_MODE_GETCRTC      DRM_IOWR(0xA1, struct drm_mode_crtc)
#define DRM_IOCTL_MODE_SETCRTC      DRM_IOWR(0xA2, struct drm_mode_crtc)
#define DRM_IOCTL_MODE_GETCONNECTOR DRM_IOWR(0xA7, struct drm_mode_get_connector)
#define DRM_IOCTL_MODE_ADDFB        DRM_IOWR(0xAE, struct drm_mode_fb_cmd)
#define DRM_IOCTL_MODE_RMFB         DRM_IOWR(0xAF, unsigned int)

#define DRM_CAP_DUMB_BUFFER 0x1

/* GETCONNECTOR's `connection` of a connected sink. */
#define DRM_MODE_CONNECTED 1

/* The most connectors, CRTCs and modes the mode row reads. */
#define MAX_IDS   16
#define MAX_MODES 64

/* The words of each mode's buffer the mode row writes and reads back. */
#define MODE_WORDS 4096

/* The dumb buffer's size in pixels and bits per pixel. */
#define DUMB_SIDE 64
#define DUMB_BPP  32

/** End the process with `status`; it does not return. */
static void sys_exit(int status)
{
    syscall1(SYS_EXIT, (long)status);
    __builtin_unreachable();
}

/** Say `what` failed, fail the current assertion with it, and exit 1. */
static void fail(const char *what)
{
    probe_line("DRMPROBE FAIL: ", what, 0);
    report_stop(what);
    sys_exit(1);
}

/** Clear `n` bytes at `p`. */
static void zero(void *p, unsigned long n)
{
    volatile char *b = (volatile char *)p;

    while (n-- > 0) {
        *b++ = 0;
    }
}

/** ioctl `cmd` on `fd` with the argument at `arg`: 0, or a negated errno. */
static long drm_ioctl(int fd, unsigned long cmd, void *arg)
{
    return syscall3(SYS_IOCTL, fd, (long)cmd, (long)arg);
}

/** Open `path` for reading and writing: a descriptor, or a negated errno. */
static int open_node(const char *path)
{
    return (int)syscall3(SYS_OPEN, (long)path, O_RDWR | O_CLOEXEC, 0);
}

/** Write `prefix` and the decimal `n` into `out` with a terminating NUL. */
static void node_path(char *out, const char *prefix, int n)
{
    char d[8];
    int len = 0, i = 0;

    while (*prefix) {
        out[len++] = *prefix++;
    }
    do {
        d[i++] = (char)('0' + n % 10);
        n /= 10;
    } while (n != 0);
    while (i > 0) {
        out[len++] = d[--i];
    }
    out[len] = '\0';
}

/** The driver's name of the node open at `fd`, read by VERSION into `name`
 * of `size` bytes and terminated: its length, or a negated errno. */
static long version_name(int fd, char *name, unsigned long size)
{
    struct drm_version v;
    long rc;

    zero(&v, sizeof(v));
    v.name = name;
    v.name_len = size - 1;
    rc = drm_ioctl(fd, DRM_IOCTL_VERSION, &v);
    if (rc != 0) {
        return rc;
    }
    if (v.name_len >= size) {
        v.name_len = size - 1;
    }
    name[v.name_len] = '\0';
    return (long)v.name_len;
}

/** A shared mapping of `len` bytes of the DRM file `fd` at the byte offset
 * `off`, as libdrm's drmMap gives one: its address, or a negated errno. On
 * i386 mmap2 takes the offset in pages, which keeps an offset above 4 GiB,
 * where a 32-bit program's buffers lie on an x86-64 kernel. */
static long map_drm(int fd, long len, u64 off)
{
#if defined __x86_64__
    return syscall6(SYS_MMAP, 0, len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, (long)off);
#else
    return syscall6(SYS_MMAP2, 0, len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, (long)(off >> 12));
#endif
}

/** Whether two NUL-terminated strings are equal. */
static int same_name(const char *a, const char *b)
{
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

/** Open the first card with dumb buffers and read its name into `name`:
 * the descriptor, with the card's index in `*index`. */
static int open_card(int *index, char *name, unsigned long size)
{
    struct drm_get_cap cap;
    char path[32];
    int n, fd;

    for (n = 0; n < 16; n++) {
        node_path(path, "/dev/dri/card", n);
        fd = open_node(path);
        if (fd < 0) {
            continue;
        }
        cap.capability = DRM_CAP_DUMB_BUFFER;
        cap.value = 0;
        if (drm_ioctl(fd, DRM_IOCTL_GET_CAP, &cap) == 0 && cap.value == 1) {
            *index = n;
            report_held();
            if (version_name(fd, name, size) <= 0) {
                fail("VERSION gives the card no name");
            }
            report_held();
            return fd;
        }
        syscall1(SYS_CLOSE, fd);
    }
    fail("no card of /dev/dri/card0 to card15 has dumb buffers");
    return -1;
}

/** The render node of card `index`: VERSION gives `name`, and CREATE_DUMB,
 * which Linux keeps from render clients, is EACCES. */
static void render_row(int index, const char *name)
{
    struct drm_mode_create_dumb c;
    char path[32], rname[64];
    int fd;

    node_path(path, "/dev/dri/renderD", 128 + index);
    fd = open_node(path);
    if (fd == -ENOENT) {
        report_skip("the driver has a card node alone");
        return;
    }
    if (fd < 0) {
        fail("open of the card's render node");
    }
    if (version_name(fd, rname, sizeof(rname)) <= 0 || !same_name(name, rname)) {
        fail("the render node's VERSION does not name the card's driver");
    }
    zero(&c, sizeof(c));
    c.width = DUMB_SIDE;
    c.height = DUMB_SIDE;
    c.bpp = DUMB_BPP;
    if (drm_ioctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &c) != -EACCES) {
        fail("CREATE_DUMB on the render node is not EACCES");
    }
    syscall1(SYS_CLOSE, fd);
    report_held();
}

/** The master role between the card's descriptor `fd` and a second open
 * `other`, then a magic of a third open, `path`, authenticated through `fd`.
 * The third is opened anew, since `other` has a master record of its own
 * once it was master, and its magic another namespace. */
static void master_rows(int fd, int other, const char *path)
{
    struct drm_auth a;
    int third;

    if (drm_ioctl(fd, DRM_IOCTL_DROP_MASTER, 0) == -EINVAL) {
        report_skip("another process holds the master role");
        report_skip("another process holds the master role");
        return;
    }
    if (drm_ioctl(other, DRM_IOCTL_SET_MASTER, 0) != 0 ||
        drm_ioctl(fd, DRM_IOCTL_SET_MASTER, 0) != -EBUSY ||
        drm_ioctl(other, DRM_IOCTL_DROP_MASTER, 0) != 0 ||
        drm_ioctl(fd, DRM_IOCTL_SET_MASTER, 0) != 0 ||
        drm_ioctl(other, DRM_IOCTL_SET_MASTER, 0) != -EBUSY) {
        fail("SET_MASTER and DROP_MASTER between two opens of the card");
    }
    report_held();
    third = open_node(path);
    if (third < 0) {
        fail("a third open of the card");
    }
    a.magic = 0;
    if (drm_ioctl(third, DRM_IOCTL_GET_MAGIC, &a) != 0 || a.magic == 0 ||
        drm_ioctl(fd, DRM_IOCTL_AUTH_MAGIC, &a) != 0 ||
        drm_ioctl(fd, DRM_IOCTL_AUTH_MAGIC, &a) != -EINVAL) {
        fail("AUTH_MAGIC does not spend a client's magic once");
    }
    syscall1(SYS_CLOSE, third);
    report_held();
}

/** Fill `n` words at `at` with a pattern of `seed`. */
static void fill(volatile u32 *at, long n, u32 seed)
{
    long i;

    for (i = 0; i < n; i++) {
        at[i] = seed ^ (u32)(i * 2654435761u);
    }
}

/** Whether `n` words at `at` hold the pattern of `seed`. */
static int holds(const volatile u32 *at, long n, u32 seed)
{
    long i;

    for (i = 0; i < n; i++) {
        if (at[i] != (seed ^ (u32)(i * 2654435761u))) {
            return 0;
        }
    }
    return 1;
}

/** Create, map, write, read back and destroy a dumb buffer on `fd`. */
static void dumb_rows(int fd)
{
    struct drm_mode_create_dumb c;
    struct drm_mode_map_dumb m;
    struct drm_mode_destroy_dumb d;
    long a, b, words;

    zero(&c, sizeof(c));
    c.width = DUMB_SIDE;
    c.height = DUMB_SIDE;
    c.bpp = DUMB_BPP;
    if (drm_ioctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &c) != 0 || c.handle == 0 ||
        c.pitch < DUMB_SIDE * DUMB_BPP / 8 || c.size < (u64)c.pitch * DUMB_SIDE) {
        fail("CREATE_DUMB of 64 by 64 at 32 bits");
    }
    report_held();
    zero(&m, sizeof(m));
    m.handle = c.handle;
    if (drm_ioctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &m) != 0 || m.offset == 0 ||
        (m.offset & 4095) != 0) {
        fail("MAP_DUMB gives no page-aligned offset");
    }
    a = map_drm(fd, (long)c.size, m.offset);
    b = map_drm(fd, (long)c.size, m.offset);
    if (sys_failed(a) || sys_failed(b)) {
        fail("mmap of the dumb buffer at its offset");
    }
    words = (long)(c.size / 4);
    fill((volatile u32 *)a, words, 0x5A5A0000u);
    if (!holds((volatile u32 *)b, words, 0x5A5A0000u)) {
        fail("a second mapping of the dumb buffer does not read the first's writes");
    }
    report_held();
    d.handle = c.handle;
    if (drm_ioctl(fd, DRM_IOCTL_MODE_DESTROY_DUMB, &d) != 0) {
        fail("DESTROY_DUMB");
    }
    if (drm_ioctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &m) != -ENOENT) {
        fail("MAP_DUMB of a destroyed handle is not ENOENT");
    }
    if (!holds((volatile u32 *)a, words, 0x5A5A0000u)) {
        fail("a mapping loses the buffer when its handle is destroyed");
    }
    if (syscall2(SYS_MUNMAP, a, (long)c.size) != 0 || syscall2(SYS_MUNMAP, b, (long)c.size) != 0) {
        fail("munmap of the dumb buffer's mappings");
    }
    report_held();
}

/** Export a dumb buffer, import it twice, and retain its pages after close. */
static void prime_row(int fd, int other)
{
    struct drm_mode_create_dumb c;
    struct drm_mode_destroy_dumb d;
    struct drm_prime_handle out, self, in, again, back;
    struct drm_gem_close close;
    long map;
    u64 size;

    zero(&c, sizeof(c));
    c.width = DUMB_SIDE;
    c.height = DUMB_SIDE;
    c.bpp = DUMB_BPP;
    if (drm_ioctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &c) != 0) {
        fail("PRIME source dumb buffer");
    }
    zero(&out, sizeof(out));
    out.handle = c.handle;
    out.flags = DRM_CLOEXEC | DRM_RDWR;
    if (drm_ioctl(fd, DRM_IOCTL_PRIME_HANDLE_TO_FD, &out) != 0) {
        fail("PRIME export");
    }
    zero(&self, sizeof(self));
    self.fd = out.fd;
    if (drm_ioctl(fd, DRM_IOCTL_PRIME_FD_TO_HANDLE, &self) != 0 || self.handle != c.handle) {
        fail("PRIME self-import returns another handle");
    }
    zero(&in, sizeof(in));
    in.fd = out.fd;
    again = in;
    if (drm_ioctl(other, DRM_IOCTL_PRIME_FD_TO_HANDLE, &in) != 0 ||
        drm_ioctl(other, DRM_IOCTL_PRIME_FD_TO_HANDLE, &again) != 0 || in.handle != again.handle) {
        fail("PRIME repeated import returns another handle");
    }
    map = map_drm(out.fd, (long)c.size, 0);
    if (sys_failed(map)) {
        fail("PRIME dma-buf mmap");
    }
    fill((volatile u32 *)map, (long)(c.size / 4), 0x11223344u);
    size = c.size;
    d.handle = c.handle;
    if (drm_ioctl(fd, DRM_IOCTL_MODE_DESTROY_DUMB, &d) != 0) {
        fail("PRIME close source handle");
    }
    zero(&back, sizeof(back));
    back.handle = in.handle;
    back.flags = DRM_CLOEXEC | DRM_RDWR;
    if (drm_ioctl(other, DRM_IOCTL_PRIME_HANDLE_TO_FD, &back) != 0) {
        fail("PRIME export after source handle close");
    }
    zero(&close, sizeof(close));
    close.handle = in.handle;
    if (drm_ioctl(other, DRM_IOCTL_GEM_CLOSE, &close) != 0 || syscall1(SYS_CLOSE, out.fd) != 0 ||
        syscall1(SYS_CLOSE, back.fd) != 0 ||
        !holds((volatile u32 *)map, (long)(size / 4), 0x11223344u) ||
        syscall2(SYS_MUNMAP, map, (long)size) != 0) {
        fail("PRIME mapping lifetime");
    }
    report_held();
}

/** kcmp of the card's descriptor `fd` against a dup, the same open file,
 * and against the second open `other`, which is not. */
static void kcmp_row(int fd, int other)
{
    long pid = syscall0(SYS_GETPID);
    long dup = syscall1(SYS_DUP, fd);
    long r;

    if (dup < 0) {
        fail("dup of the card's descriptor");
    }
    if (syscall5(SYS_KCMP, pid, pid, KCMP_FILE, fd, dup) != 0) {
        fail("kcmp says a dup is another open file");
    }
    r = syscall5(SYS_KCMP, pid, pid, KCMP_FILE, fd, other);
    if (r != 1 && r != 2) {
        fail("kcmp says a second open is the same open file");
    }
    syscall1(SYS_CLOSE, dup);
    report_held();
}

/** Whether two modes are the same mode: timings, clock, flags and name. */
static int same_mode(const struct drm_mode_modeinfo *a, const struct drm_mode_modeinfo *b)
{
    return a->clock == b->clock && a->hdisplay == b->hdisplay && a->htotal == b->htotal &&
           a->hsync_start == b->hsync_start && a->hsync_end == b->hsync_end &&
           a->vdisplay == b->vdisplay && a->vtotal == b->vtotal &&
           a->vsync_start == b->vsync_start && a->vsync_end == b->vsync_end &&
           a->flags == b->flags && same_name(a->name, b->name);
}

/** The first connected connector of the card at `fd` with modes into
 * `*conn` and its modes into `modes`, and the first CRTC's ID: the number
 * of modes, at most MAX_MODES. */
static u32 mode_list(int fd, struct drm_mode_get_connector *conn, struct drm_mode_modeinfo *modes,
                     u32 *crtc)
{
    static u32 crtcs[MAX_IDS], conns[MAX_IDS];
    struct drm_mode_card_res res;
    u32 i;

    zero(&res, sizeof(res));
    res.crtc_id_ptr = (u64)(unsigned long)crtcs;
    res.connector_id_ptr = (u64)(unsigned long)conns;
    res.count_crtcs = MAX_IDS;
    res.count_connectors = MAX_IDS;
    if (drm_ioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, &res) != 0 || res.count_crtcs == 0 ||
        res.count_connectors == 0) {
        fail("GETRESOURCES gives no CRTC or no connector");
    }
    *crtc = crtcs[0];
    for (i = 0; i < res.count_connectors && i < MAX_IDS; i++) {
        zero(conn, sizeof(*conn));
        conn->connector_id = conns[i];
        if (drm_ioctl(fd, DRM_IOCTL_MODE_GETCONNECTOR, conn) != 0) {
            fail("GETCONNECTOR's probe");
        }
        if (conn->connection != DRM_MODE_CONNECTED || conn->count_modes == 0) {
            continue;
        }
        if (conn->count_modes > MAX_MODES) {
            conn->count_modes = MAX_MODES;
        }
        conn->modes_ptr = (u64)(unsigned long)modes;
        conn->count_encoders = 0;
        conn->count_props = 0;
        if (drm_ioctl(fd, DRM_IOCTL_MODE_GETCONNECTOR, conn) != 0) {
            fail("GETCONNECTOR's modes");
        }
        return conn->count_modes > MAX_MODES ? MAX_MODES : conn->count_modes;
    }
    fail("no connector is connected with modes");
    return 0;
}

/** Set `mode` on `crtc` and `conn` from a dumb buffer of its size with a
 * pattern, read the CRTC back, and take the buffer down. */
static void mode_set(int fd, u32 crtc, u32 conn, const struct drm_mode_modeinfo *mode)
{
    struct drm_mode_create_dumb c;
    struct drm_mode_map_dumb m;
    struct drm_mode_destroy_dumb d;
    struct drm_mode_fb_cmd f;
    struct drm_mode_crtc set, get;
    long map;

    zero(&c, sizeof(c));
    c.width = mode->hdisplay;
    c.height = mode->vdisplay;
    c.bpp = 32;
    if (drm_ioctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &c) != 0) {
        fail("CREATE_DUMB of a mode's size");
    }
    zero(&m, sizeof(m));
    m.handle = c.handle;
    if (drm_ioctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &m) != 0 ||
        sys_failed(map = map_drm(fd, (long)c.size, m.offset))) {
        fail("a mode's buffer is not mapped");
    }
    fill((volatile u32 *)map, MODE_WORDS, 0x600DF00Du);
    zero(&f, sizeof(f));
    f.width = mode->hdisplay;
    f.height = mode->vdisplay;
    f.pitch = c.pitch;
    f.bpp = 32;
    f.depth = 24;
    f.handle = c.handle;
    if (drm_ioctl(fd, DRM_IOCTL_MODE_ADDFB, &f) != 0) {
        fail("ADDFB of a mode's buffer");
    }
    zero(&set, sizeof(set));
    set.set_connectors_ptr = (u64)(unsigned long)&conn;
    set.count_connectors = 1;
    set.crtc_id = crtc;
    set.fb_id = f.fb_id;
    set.mode_valid = 1;
    set.mode = *mode;
    if (drm_ioctl(fd, DRM_IOCTL_MODE_SETCRTC, &set) != 0) {
        probe_line("DRMPROBE FAIL MODE ", mode->name, 0);
        fail("SETCRTC of a listed mode");
    }
    zero(&get, sizeof(get));
    get.crtc_id = crtc;
    if (drm_ioctl(fd, DRM_IOCTL_MODE_GETCRTC, &get) != 0 || !get.mode_valid ||
        get.fb_id != f.fb_id || !same_mode(&get.mode, mode)) {
        probe_line("DRMPROBE FAIL MODE ", mode->name, 0);
        fail("GETCRTC does not read back the mode set");
    }
    if (!holds((volatile u32 *)map, MODE_WORDS, 0x600DF00Du)) {
        fail("a buffer's mapping changed when its mode was set");
    }
    probe_line("DRMPROBE MODE ", mode->name, 0);
    d.handle = c.handle;
    if (drm_ioctl(fd, DRM_IOCTL_MODE_RMFB, &f.fb_id) != 0 ||
        syscall2(SYS_MUNMAP, map, (long)c.size) != 0 ||
        drm_ioctl(fd, DRM_IOCTL_MODE_DESTROY_DUMB, &d) != 0) {
        fail("a mode's framebuffer and buffer are not taken down");
    }
}

/** As the master, set each mode of the first connected connector. */
static void modes_row(int fd)
{
    static struct drm_mode_modeinfo modes[MAX_MODES];
    struct drm_mode_get_connector conn;
    u32 crtc, n, i;

    if (drm_ioctl(fd, DRM_IOCTL_SET_MASTER, 0) != 0) {
        report_skip("another process holds the master role");
        return;
    }
    n = mode_list(fd, &conn, modes, &crtc);
    for (i = 0; i < n; i++) {
        mode_set(fd, crtc, conn.connector_id, &modes[i]);
    }
    report_held();
}

void probe_main(unsigned long *sp);

PROBE_SP_ENTRY;

/** Run the rows in order; `_start` calls it with the initial stack. */
void probe_main(unsigned long *sp)
{
    static const char *const ids[] = {"open", "version", "render", "master", "auth", "create",
                                      "map",  "destroy", "prime",  "kcmp",   "modes"};
    char name[64], path[32];
    int fd, other, index = 0;
    unsigned i;

    report_begin(report_case((char **)(sp + 1) + sp[0] + 1, "guest.screen.drmprobe"));
    for (i = 0; i < sizeof(ids) / sizeof(ids[0]); i++) {
        report_plan(ids[i]);
    }
    fd = open_card(&index, name, sizeof(name));
    render_row(index, name);
    node_path(path, "/dev/dri/card", index);
    other = open_node(path);
    if (other < 0) {
        fail("a second open of the card");
    }
    master_rows(fd, other, path);
    dumb_rows(fd);
    prime_row(fd, other);
    kcmp_row(fd, other);
    syscall1(SYS_CLOSE, other);
    modes_row(fd);
    syscall1(SYS_CLOSE, fd);
    report_end();
    probe_line("DRMPROBE OK", 0, 0);
    sys_exit(0);
}
