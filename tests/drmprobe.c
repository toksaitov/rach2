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
 * mode set prints "DRMPROBE MODE NAME". On the first mode it flips once to a
 * second buffer with PAGE_FLIP, reads the FLIP_COMPLETE event from the card,
 * which the core sends at once on a CRTC without vblanks, reads the CRTC back
 * and prints "DRMPROBE FLIP NAME". Next, it chooses the CRC source "auto"
 * of the first CRTC through debugfs's dri/N/crtc-0/crc/control, as IGT
 * does, shows the first mode again, and reads two lines of crc/data in
 * Linux 6.12's text, a frame number as 0x%08x or ten X's, then each value as
 * " 0x%08x", a newline and a NUL, the second of a later frame; a driver
 * without CRCs, which refuses the source with EINVAL, skips it. Then, as
 * the master, it leases the first CRTC and connector to a new descriptor
 * with CREATE_LEASE, as a VR compositor does: the same objects again are
 * EBUSY, the lessee's GETRESOURCES gives that CRTC and connector alone, it
 * holds the master role (AUTH_MAGIC of 0 is EINVAL, not EACCES) and cannot
 * drop it (EINVAL), GET_LEASE gives at least the CRTC, the connector and
 * the primary plane the CRTC brings, the master's LIST_LESSEES gives the
 * lessee, and the lessee sets the first mode from a buffer of its own;
 * after REVOKE_LEASE its GETCRTC is ENOENT, and after its close the master
 * lists no lessee and a revoke of it is ENOENT. It prints "DRMPROBE
 * LEASE".
 *
 * With the arguments `hold SECONDS` it is a master that leaves without a
 * word, for the console's handover: it opens the first card with dumb
 * buffers, which makes it the master, shows the first mode of the first
 * connected connector from a buffer of colored bands, prints "DRMPROBE HOLD
 * NAME", sleeps SECONDS, from 1 to 3600, prints "DRMPROBE HOLD END" and
 * exits with the card open and the role held, which the exit releases.
 *
 * Prints "DRMPROBE OK" and exits 0 when every check passes, and otherwise
 * "DRMPROBE FAIL: what" and exits 1 at the first failure. etude's report
 * (probe_report.h), for the case ETUDE_CASE names, has an assertion for
 * each check in order: `open`, `version`, `render`, `master`, `auth`,
 * `create`, `map`, `destroy`, `prime`, `kcmp`, `modes`, `crc` and `lease`.
 * The master rows, `modes`, `crc` and `lease` skip while another process
 * holds the role. Freestanding; direct system calls (probe.h).
 */

#include "probe.h"
#include "probe_report.h"

#define O_RDONLY  0
#define O_WRONLY  1
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

struct drm_mode_crtc_page_flip {
    u32 crtc_id;
    u32 fb_id;
    u32 flags;
    u32 reserved;
    u64 user_data;
};

/* An event read from the card: its header, then a vblank's or a flip's. */
struct drm_event {
    u32 type;
    u32 length;
};

struct drm_event_vblank {
    struct drm_event base;
    u64 user_data;
    u32 tv_sec;
    u32 tv_usec;
    u32 sequence;
    u32 crtc_id;
};

#define DRM_MODE_PAGE_FLIP_EVENT 0x01
#define DRM_EVENT_FLIP_COMPLETE  0x02

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
#define DRM_IOCTL_MODE_PAGE_FLIP    DRM_IOWR(0xB0, struct drm_mode_crtc_page_flip)

struct drm_mode_create_lease {
    u64 object_ids;
    u32 object_count;
    u32 flags;
    u32 lessee_id;
    u32 fd;
};

struct drm_mode_list_lessees {
    u32 count_lessees;
    u32 pad;
    u64 lessees_ptr;
};

struct drm_mode_get_lease {
    u32 count_objects;
    u32 pad;
    u64 objects_ptr;
};

struct drm_mode_revoke_lease {
    u32 lessee_id;
};

#define DRM_IOCTL_MODE_CREATE_LEASE DRM_IOWR(0xC6, struct drm_mode_create_lease)
#define DRM_IOCTL_MODE_LIST_LESSEES DRM_IOWR(0xC7, struct drm_mode_list_lessees)
#define DRM_IOCTL_MODE_GET_LEASE    DRM_IOWR(0xC8, struct drm_mode_get_lease)
#define DRM_IOCTL_MODE_REVOKE_LEASE DRM_IOWR(0xC9, struct drm_mode_revoke_lease)

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
        at[i] = seed ^ (u32)i * 2654435761u;
    }
}

/** Whether `n` words at `at` hold the pattern of `seed`. */
static int holds(const volatile u32 *at, long n, u32 seed)
{
    long i;

    for (i = 0; i < n; i++) {
        if (at[i] != (seed ^ (u32)i * 2654435761u)) {
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
    zero(crtcs, sizeof(crtcs));
    zero(conns, sizeof(conns));
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

/* A dumb buffer of a mode's size, mapped, and its framebuffer. */
struct mode_fb {
    u32 handle;
    u32 fb_id;
    u64 size;
    long map;
};

/** Make a dumb buffer of `mode`'s size into `*b`, write the pattern of
 * `seed` into its first MODE_WORDS words, and add it as a framebuffer. */
static void fb_make(int fd, const struct drm_mode_modeinfo *mode, struct mode_fb *b, u32 seed)
{
    struct drm_mode_create_dumb c;
    struct drm_mode_map_dumb m;
    struct drm_mode_fb_cmd f;

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
        sys_failed(b->map = map_drm(fd, (long)c.size, m.offset))) {
        fail("a mode's buffer is not mapped");
    }
    fill((volatile u32 *)b->map, MODE_WORDS, seed);
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
    b->handle = c.handle;
    b->fb_id = f.fb_id;
    b->size = c.size;
}

/** Remove `b`'s framebuffer, which turns its CRTC off if it is shown, and
 * unmap and destroy its buffer. */
static void fb_drop(int fd, struct mode_fb *b)
{
    struct drm_mode_destroy_dumb d;

    d.handle = b->handle;
    if (drm_ioctl(fd, DRM_IOCTL_MODE_RMFB, &b->fb_id) != 0 ||
        syscall2(SYS_MUNMAP, b->map, (long)b->size) != 0 ||
        drm_ioctl(fd, DRM_IOCTL_MODE_DESTROY_DUMB, &d) != 0) {
        fail("a mode's framebuffer and buffer are not taken down");
    }
}

/** On `crtc`, which shows a framebuffer in `mode`, flip to a second one
 * with PAGE_FLIP and its event, read FLIP_COMPLETE from the card, read the
 * CRTC back, and take the second buffer down. */
static void flip_row(int fd, u32 crtc, const struct drm_mode_modeinfo *mode)
{
    struct drm_mode_crtc_page_flip flip;
    struct drm_event_vblank ev;
    struct drm_mode_crtc get;
    struct mode_fb b;

    fb_make(fd, mode, &b, 0xF11Bu);
    zero(&flip, sizeof(flip));
    flip.crtc_id = crtc;
    flip.fb_id = b.fb_id;
    flip.flags = DRM_MODE_PAGE_FLIP_EVENT;
    flip.user_data = 0xF11B0001ull;
    if (drm_ioctl(fd, DRM_IOCTL_MODE_PAGE_FLIP, &flip) != 0) {
        fail("PAGE_FLIP to a second framebuffer");
    }
    zero(&ev, sizeof(ev));
    if (syscall3(SYS_READ, fd, (long)&ev, sizeof(ev)) != (long)sizeof(ev) ||
        ev.base.type != DRM_EVENT_FLIP_COMPLETE || ev.base.length != sizeof(ev) ||
        ev.user_data != flip.user_data || ev.crtc_id != crtc) {
        fail("the card gives no FLIP_COMPLETE of the flip");
    }
    zero(&get, sizeof(get));
    get.crtc_id = crtc;
    if (drm_ioctl(fd, DRM_IOCTL_MODE_GETCRTC, &get) != 0 || get.fb_id != b.fb_id ||
        !same_mode(&get.mode, mode)) {
        fail("GETCRTC does not read back the flip");
    }
    if (!holds((volatile u32 *)b.map, MODE_WORDS, 0xF11Bu)) {
        fail("a buffer's mapping changed when it was flipped to");
    }
    probe_line("DRMPROBE FLIP ", mode->name, 0);
    fb_drop(fd, &b);
}

/** Show the framebuffer `fb_id` in `mode` on `crtc` and `conn` by SETCRTC:
 * 0, or a negated errno. */
static long set_crtc(int fd, u32 crtc, u32 conn, const struct drm_mode_modeinfo *mode, u32 fb_id)
{
    struct drm_mode_crtc set;

    zero(&set, sizeof(set));
    set.set_connectors_ptr = (u64)(unsigned long)&conn;
    set.count_connectors = 1;
    set.crtc_id = crtc;
    set.fb_id = fb_id;
    set.mode_valid = 1;
    set.mode = *mode;
    return drm_ioctl(fd, DRM_IOCTL_MODE_SETCRTC, &set);
}

/** Set `mode` on `crtc` and `conn` from a dumb buffer of its size with a
 * pattern, read the CRTC back, flip once if `flip` is set, and take the
 * buffer down. */
static void mode_set(int fd, u32 crtc, u32 conn, const struct drm_mode_modeinfo *mode, int flip)
{
    struct drm_mode_crtc get;
    struct mode_fb b;

    fb_make(fd, mode, &b, 0x600DF00Du);
    if (set_crtc(fd, crtc, conn, mode, b.fb_id) != 0) {
        probe_line("DRMPROBE FAIL MODE ", mode->name, 0);
        fail("SETCRTC of a listed mode");
    }
    zero(&get, sizeof(get));
    get.crtc_id = crtc;
    if (drm_ioctl(fd, DRM_IOCTL_MODE_GETCRTC, &get) != 0 || !get.mode_valid ||
        get.fb_id != b.fb_id || !same_mode(&get.mode, mode)) {
        probe_line("DRMPROBE FAIL MODE ", mode->name, 0);
        fail("GETCRTC does not read back the mode set");
    }
    if (!holds((volatile u32 *)b.map, MODE_WORDS, 0x600DF00Du)) {
        fail("a buffer's mapping changed when its mode was set");
    }
    probe_line("DRMPROBE MODE ", mode->name, 0);
    if (flip) {
        flip_row(fd, crtc, mode);
    }
    fb_drop(fd, &b);
}

/* The first mode `modes_row` set, on `first_crtc` and `first_conn`, for
 * `crc_row`; `first_crtc` is 0 when it set none. */
static struct drm_mode_modeinfo first_mode;
static u32 first_crtc, first_conn;

/** As the master, set each mode of the first connected connector, and
 * flip once on the first. */
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
        mode_set(fd, crtc, conn.connector_id, &modes[i], i == 0);
    }
    first_mode = modes[0];
    first_crtc = crtc;
    first_conn = conn.connector_id;
    report_held();
}

/* A line of crc/data with ten values, Linux's LINE_LEN(10). */
#define CRC_LINE (10 + 11 * 10 + 1 + 1)

/** Append `s` to the string `out` of `cap` bytes, as far as it goes. */
static void append(char *out, unsigned long cap, const char *s)
{
    unsigned long n = 0;

    while (out[n]) {
        n++;
    }
    while (*s && n + 1 < cap) {
        out[n++] = *s++;
    }
    out[n] = '\0';
}

/** Whether the ten bytes at `s` are "0x" and eight lowercase hex digits,
 * with their value in `*v`. */
static int crc_hex(const char *s, u32 *v)
{
    int i;

    if (s[0] != '0' || s[1] != 'x') {
        return 0;
    }
    for (*v = 0, i = 2; i < 10; i++) {
        if (s[i] >= '0' && s[i] <= '9') {
            *v = *v * 16 + (u32)(s[i] - '0');
        } else if (s[i] >= 'a' && s[i] <= 'f') {
            *v = *v * 16 + (u32)(s[i] - 'a' + 10);
        } else {
            return 0;
        }
    }
    return 1;
}

/** Read one line of the open crc/data `fd` into `line` and check Linux's
 * text: the frame as 0x%08x, or ten X's, into `*frame`, with `*counted`
 * set when it is a number, then one value or more as " 0x%08x", a newline
 * and a NUL. Leaves the line in `line` without its newline. */
static void crc_line(int fd, char *line, u32 *frame, int *counted)
{
    long n, at;
    u32 v;

    n = syscall3(SYS_READ, fd, (long)line, CRC_LINE);
    if (n < 10 + 11 + 2 || line[n - 1] != '\0' || line[n - 2] != '\n' || (n - 12) % 11 != 0) {
        fail("crc/data gives no line of a frame and its values");
    }
    *counted = crc_hex(line, frame);
    for (at = 0; !*counted && at < 10; at++) {
        if (line[at] != 'X') {
            fail("crc/data's line begins with neither a frame nor ten X's");
        }
    }
    for (at = 10; at < n - 2; at += 11) {
        if (line[at] != ' ' || !crc_hex(line + at + 1, &v)) {
            fail("crc/data's values are not \" 0x%08x\" each");
        }
    }
    line[n - 2] = '\0';
}

/** As the master, choose the source "auto" of the first CRTC, show the
 * first mode again, read two lines of crc/data, the second of a later
 * frame, and take the mode down; skipped on a driver without CRCs. The
 * card is card `index`, whose debugfs directory is dri/`index`. */
static void crc_row(int fd, int index)
{
    char control[64], data[64], line[CRC_LINE];
    u32 frame, next;
    int counted, counted_next, data_fd;
    long c, w;
    struct mode_fb b;

    if (!first_crtc) {
        report_skip("no mode was set");
        return;
    }
    node_path(control, "/sys/kernel/debug/dri/", index);
    node_path(data, "/sys/kernel/debug/dri/", index);
    append(control, sizeof(control), "/crtc-0/crc/control");
    append(data, sizeof(data), "/crtc-0/crc/data");
    c = syscall3(SYS_OPEN, (long)control, O_WRONLY | O_CLOEXEC, 0);
    if (c < 0) {
        fail("no crtc-0/crc/control in debugfs");
    }
    w = syscall3(SYS_WRITE, c, (long)"auto", 4);
    syscall1(SYS_CLOSE, c);
    if (w == -EINVAL) {
        report_skip("the driver has no CRCs");
        return;
    }
    if (w != 4) {
        fail("crc/control does not take the source auto");
    }
    fb_make(fd, &first_mode, &b, 0xC3C3C3C3u);
    if (set_crtc(fd, first_crtc, first_conn, &first_mode, b.fb_id) != 0) {
        fail("SETCRTC of the first mode again");
    }
    data_fd = (int)syscall3(SYS_OPEN, (long)data, O_RDONLY | O_CLOEXEC, 0);
    if (data_fd < 0) {
        fail("crc/data does not open on a CRTC that is on");
    }
    crc_line(data_fd, line, &frame, &counted);
    crc_line(data_fd, line, &next, &counted_next);
    if (counted != counted_next || (counted && next <= frame)) {
        fail("crc/data's second line is not of a later frame");
    }
    syscall1(SYS_CLOSE, data_fd);
    probe_line("DRMPROBE CRC ", line, 0);
    fb_drop(fd, &b);
    report_held();
}

/** LIST_LESSEES of `fd` into `ids`, room for MAX_IDS: their number, or a
 * negated errno. */
static long lessees(int fd, u32 *ids)
{
    struct drm_mode_list_lessees l;
    long rc;

    zero(&l, sizeof(l));
    l.count_lessees = MAX_IDS;
    l.lessees_ptr = (u64)(unsigned long)ids;
    rc = drm_ioctl(fd, DRM_IOCTL_MODE_LIST_LESSEES, &l);
    return rc != 0 ? rc : (long)l.count_lessees;
}

/** As the master, lease the first CRTC and connector, which bring the
 * CRTC's primary plane, to a new descriptor, as Linux's CREATE_LEASE: the
 * same objects again are EBUSY; the lessee sees that CRTC and connector
 * alone, holds the master role, cannot drop it, lists what it leases and
 * sets the first mode; the master lists it; a revoke leaves it nothing,
 * and its close takes it off the list. */
static void lease_row(int fd)
{
    struct drm_mode_create_lease cl;
    struct drm_mode_get_lease gl;
    struct drm_mode_revoke_lease rl;
    struct drm_mode_card_res res;
    struct drm_mode_crtc get;
    struct drm_auth auth;
    u32 ids[2], crtcs[MAX_IDS], conns[MAX_IDS], objs[MAX_IDS];
    struct mode_fb b;
    long rc;
    int lfd;

    if (!first_crtc) {
        report_skip("no mode was set");
        return;
    }
    ids[0] = first_crtc;
    ids[1] = first_conn;
    zero(&cl, sizeof(cl));
    cl.object_ids = (u64)(unsigned long)ids;
    cl.object_count = 2;
    cl.flags = O_CLOEXEC;
    if (drm_ioctl(fd, DRM_IOCTL_MODE_CREATE_LEASE, &cl) != 0 || cl.lessee_id == 0) {
        fail("CREATE_LEASE of the first CRTC and connector");
    }
    lfd = (int)cl.fd;
    if (drm_ioctl(fd, DRM_IOCTL_MODE_CREATE_LEASE, &cl) != -EBUSY) {
        fail("a second lease of the same objects is not EBUSY");
    }
    zero(&res, sizeof(res));
    res.crtc_id_ptr = (u64)(unsigned long)crtcs;
    res.connector_id_ptr = (u64)(unsigned long)conns;
    res.count_crtcs = MAX_IDS;
    res.count_connectors = MAX_IDS;
    if (drm_ioctl(lfd, DRM_IOCTL_MODE_GETRESOURCES, &res) != 0 || res.count_crtcs != 1 ||
        res.count_connectors != 1 || crtcs[0] != first_crtc || conns[0] != first_conn) {
        fail("the lessee does not see its CRTC and connector alone");
    }
    auth.magic = 0;
    if (drm_ioctl(lfd, DRM_IOCTL_AUTH_MAGIC, &auth) != -EINVAL ||
        drm_ioctl(lfd, DRM_IOCTL_DROP_MASTER, 0) != -EINVAL) {
        fail("the lessee is not master, or drops the role");
    }
    zero(&gl, sizeof(gl));
    gl.count_objects = MAX_IDS;
    gl.objects_ptr = (u64)(unsigned long)objs;
    if (drm_ioctl(lfd, DRM_IOCTL_MODE_GET_LEASE, &gl) != 0 || gl.count_objects < 3) {
        fail("GET_LEASE does not give the CRTC, the connector and the primary plane");
    }
    rc = lessees(fd, objs);
    if (rc != 1 || objs[0] != cl.lessee_id) {
        fail("LIST_LESSEES does not give the lessee");
    }
    fb_make(lfd, &first_mode, &b, 0x1EA5E000u);
    if (set_crtc(lfd, first_crtc, first_conn, &first_mode, b.fb_id) != 0) {
        fail("the lessee's SETCRTC of the first mode");
    }
    fb_drop(lfd, &b);
    rl.lessee_id = cl.lessee_id;
    if (drm_ioctl(fd, DRM_IOCTL_MODE_REVOKE_LEASE, &rl) != 0) {
        fail("REVOKE_LEASE");
    }
    zero(&get, sizeof(get));
    get.crtc_id = first_crtc;
    if (drm_ioctl(lfd, DRM_IOCTL_MODE_GETCRTC, &get) != -ENOENT) {
        fail("a revoked lessee still finds its CRTC");
    }
    syscall1(SYS_CLOSE, lfd);
    if (lessees(fd, objs) != 0 || drm_ioctl(fd, DRM_IOCTL_MODE_REVOKE_LEASE, &rl) != -ENOENT) {
        fail("the closed lessee is still listed");
    }
    probe_line("DRMPROBE LEASE", 0, 0);
    report_held();
}

/** The decimal number `s`, or 0 for one that is not 1 to 3600. */
static long hold_seconds(const char *s)
{
    long n = 0;

    while (*s >= '0' && *s <= '9' && n <= 3600) {
        n = n * 10 + (*s++ - '0');
    }
    return *s == '\0' && n >= 1 && n <= 3600 ? n : 0;
}

/** The hold mode: show colored bands as the master for `seconds` and exit
 * without closing the card or dropping the role. */
static void hold_main(long seconds)
{
    static struct drm_mode_modeinfo modes[MAX_MODES];
    struct drm_mode_get_connector conn;
    struct drm_mode_create_dumb c;
    struct drm_mode_map_dumb m;
    struct drm_mode_fb_cmd f;
    struct drm_mode_crtc set;
    struct {
        long sec, nsec;
    } ts;
    char name[64];
    volatile u32 *px;
    unsigned long i, words;
    int fd, index = 0;
    u32 crtc;
    long map;

    fd = open_card(&index, name, sizeof(name));
    if (drm_ioctl(fd, DRM_IOCTL_SET_MASTER, 0) != 0) {
        fail("SET_MASTER: another process holds the master role");
    }
    if (mode_list(fd, &conn, modes, &crtc) == 0) {
        fail("no mode to show");
    }
    zero(&c, sizeof(c));
    c.width = modes[0].hdisplay;
    c.height = modes[0].vdisplay;
    c.bpp = 32;
    zero(&m, sizeof(m));
    if (drm_ioctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &c) != 0 ||
        (m.handle = c.handle, drm_ioctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &m)) != 0 ||
        sys_failed(map = map_drm(fd, (long)c.size, m.offset))) {
        fail("the hold's buffer");
    }
    px = (volatile u32 *)map;
    words = (unsigned long)c.pitch / 4 * c.height;
    for (i = 0; i < words; i++) {
        px[i] = (i / (c.pitch / 4) / 64) % 2 ? 0x00FF8000u : 0x000080FFu;
    }
    zero(&f, sizeof(f));
    f.width = c.width;
    f.height = c.height;
    f.pitch = c.pitch;
    f.bpp = 32;
    f.depth = 24;
    f.handle = c.handle;
    zero(&set, sizeof(set));
    set.set_connectors_ptr = (u64)(unsigned long)&conn.connector_id;
    set.count_connectors = 1;
    set.crtc_id = crtc;
    set.mode_valid = 1;
    set.mode = modes[0];
    if (drm_ioctl(fd, DRM_IOCTL_MODE_ADDFB, &f) != 0 ||
        (set.fb_id = f.fb_id, drm_ioctl(fd, DRM_IOCTL_MODE_SETCRTC, &set)) != 0) {
        fail("SETCRTC of the hold's mode");
    }
    probe_line("DRMPROBE HOLD ", modes[0].name, 0);
    ts.sec = seconds;
    ts.nsec = 0;
    /* interval: the master holds the screen for the seconds asked */
    syscall2(SYS_NANOSLEEP, (long)&ts, 0);
    probe_line("DRMPROBE HOLD END", 0, 0);
    sys_exit(0);
}

void probe_main(unsigned long *sp);

PROBE_SP_ENTRY;

/** Run the rows in order; `_start` calls it with the initial stack. */
void probe_main(unsigned long *sp)
{
    static const char *const ids[] = {"open",   "version", "render",  "master", "auth",
                                      "create", "map",     "destroy", "prime",  "kcmp",
                                      "modes",  "crc",     "lease"};
    char name[64], path[32];
    char **argv = (char **)(sp + 1);
    int fd, other, index = 0;
    unsigned i;

    if (sp[0] == 3 && same_name(argv[1], "hold")) {
        long seconds = hold_seconds(argv[2]);

        if (seconds == 0) {
            probe_line("drmprobe: usage: drmprobe [hold SECONDS]", 0, 0);
            sys_exit(2);
        }
        hold_main(seconds);
    }
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
    crc_row(fd, index);
    lease_row(fd);
    syscall1(SYS_CLOSE, fd);
    report_end();
    probe_line("DRMPROBE OK", 0, 0);
    sys_exit(0);
}
