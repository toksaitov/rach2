/**
 * drmcompatprobe: the DRM core's requests whose i386 layouts differ from
 * x86-64's, made by an i386 program, for the compat ioctl path of the x86-64
 * kernel (rach2/ia32.md), which must answer them as Linux 6.12's
 * drm_ioc32.c does. On an i686 kernel the same calls are native.
 *
 * The probe opens /dev/dri/card0 to card15 in turn and takes the first that
 * answers GET_CAP with DRM_CAP_DUMB_BUFFER set. VERSION, with its lengths and
 * pointers in 4 bytes, gives the driver's name, date and description, first
 * their lengths alone and then into buffers. GETRESOURCES, whose 64-bit
 * pointers carry the probe's 32-bit addresses, counts the CRTCs and
 * connectors and then lists them. ADDFB2, whose modifiers i386 places at 68,
 * adds a framebuffer on a dumb buffer of 64 by 64 pixels and gives its id;
 * GETFB2 reads the framebuffer back, its modifiers at 68 too, before RMFB
 * removes it. WAIT_VBLANK, relative 0, gives the count and time of the last
 * vblank in its 16 bytes, or EOPNOTSUPP on a card with no vblank interrupt,
 * as bochs-drm on QEMU's std-vga is, with the request copied back. Each
 * structure lies before guard bytes that no call may touch.
 *
 * Prints "DRMCOMPATPROBE OK" and exits 0 when every check passes, and
 * otherwise "DRMCOMPATPROBE FAIL: what" and exits 1 at the first failure.
 * etude's report (probe_report.h), for the case ETUDE_CASE names, has an
 * assertion for each check in order: `open`, `version`, `resources`,
 * `addfb2`, `getfb2` and `vblank`. Built for i686 alone (tests/Makefile).
 * Freestanding; direct system calls (probe.h).
 */

#include "probe.h"
#include "probe_report.h"

#define O_RDWR    2
#define O_CLOEXEC 02000000

#define EOPNOTSUPP 95

/* Linux's ioctl numbers: the direction in the top two bits, the size of
 * the argument, the type 'd' and the number. */
#define DRM_IOR(nr, t)  (0x80000000UL | ((unsigned long)sizeof(t) << 16) | 0x6400UL | (nr))
#define DRM_IOWR(nr, t) (0xC0000000UL | ((unsigned long)sizeof(t) << 16) | 0x6400UL | (nr))

typedef unsigned int u32;
typedef unsigned long long u64;

/* The i386 layouts, as an i386 compiler lays out Linux's structures: a size_t
 * and a pointer of 4 bytes, and a 64-bit member aligned to 4. */
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

struct drm_mode_create_dumb {
    u32 height;
    u32 width;
    u32 bpp;
    u32 flags;
    u32 handle;
    u32 pitch;
    u64 size;
};

struct drm_mode_destroy_dumb {
    u32 handle;
};

struct drm_mode_fb_cmd2 {
    u32 fb_id;
    u32 width;
    u32 height;
    u32 pixel_format;
    u32 flags;
    u32 handles[4];
    u32 pitches[4];
    u32 offsets[4];
    u64 modifier[4];
};

/* drm_wait_vblank's request and reply, which share its 16 bytes. */
struct drm_wait_vblank_request {
    u32 type;
    u32 sequence;
    unsigned long signal;
};

struct drm_wait_vblank_reply {
    u32 type;
    u32 sequence;
    long tval_sec;
    long tval_usec;
};

union drm_wait_vblank {
    struct drm_wait_vblank_request request;
    struct drm_wait_vblank_reply reply;
};

/* A compile-time check of an i386 size: an array of -1 elements fails. */
#define I386_SIZE(t, n) typedef char t##_is_i386[sizeof(struct t) == (n) ? 1 : -1]

#if !defined __x86_64__
I386_SIZE(drm_version, 36);
I386_SIZE(drm_mode_fb_cmd2, 100);
I386_SIZE(drm_wait_vblank_reply, 16);
#endif

#define DRM_IOCTL_VERSION           DRM_IOWR(0x00, struct drm_version)
#define DRM_IOCTL_GET_CAP           DRM_IOWR(0x0c, struct drm_get_cap)
#define DRM_IOCTL_WAIT_VBLANK       DRM_IOWR(0x3a, union drm_wait_vblank)
#define DRM_IOCTL_MODE_GETRESOURCES DRM_IOWR(0xA0, struct drm_mode_card_res)
#define DRM_IOCTL_MODE_RMFB         DRM_IOWR(0xAF, unsigned int)
#define DRM_IOCTL_MODE_CREATE_DUMB  DRM_IOWR(0xB2, struct drm_mode_create_dumb)
#define DRM_IOCTL_MODE_DESTROY_DUMB DRM_IOWR(0xB4, struct drm_mode_destroy_dumb)
#define DRM_IOCTL_MODE_ADDFB2       DRM_IOWR(0xB8, struct drm_mode_fb_cmd2)
#define DRM_IOCTL_MODE_GETFB2       DRM_IOWR(0xCE, struct drm_mode_fb_cmd2)

#define DRM_CAP_DUMB_BUFFER  0x1
#define DRM_FORMAT_XRGB8888  0x34325258 /* 'XR24' */
#define _DRM_VBLANK_RELATIVE 0x1

/* The bytes after each structure that no call may write, and their value. */
#define GUARD      16
#define GUARD_BYTE 0xA5

/* The most CRTCs and connectors GETRESOURCES lists. */
#define MAX_IDS 16

/* The dumb buffer's size in pixels and bits per pixel. */
#define DUMB_SIDE 64
#define DUMB_BPP  32

/* A structure of up to 128 bytes and its guard. */
union guarded {
    unsigned char bytes[128 + GUARD];
    u64 align;
};

/** End the process with `status`; it does not return. */
static void sys_exit(int status)
{
    syscall1(SYS_EXIT, (long)status);
    __builtin_unreachable();
}

/** Say `what` failed, fail the current assertion with it, and exit 1. */
static void fail(const char *what)
{
    probe_line("DRMCOMPATPROBE FAIL: ", what, 0);
    report_stop(what);
    sys_exit(1);
}

/** Set `n` bytes at `p` to `c`. */
static void fill(void *p, int c, unsigned long n)
{
    volatile unsigned char *b = (volatile unsigned char *)p;

    while (n-- > 0) {
        *b++ = (unsigned char)c;
    }
}

/** Clear `size` bytes of `g` and set the guard after them. */
static void *guard_set(union guarded *g, unsigned long size)
{
    fill(g->bytes, 0, size);
    fill(g->bytes + size, GUARD_BYTE, GUARD);
    return g->bytes;
}

/** Fail with `what` unless the guard after `size` bytes of `g` is whole. */
static void guard_check(const union guarded *g, unsigned long size, const char *what)
{
    unsigned long i;

    for (i = 0; i < GUARD; i++) {
        if (g->bytes[size + i] != GUARD_BYTE) {
            fail(what);
        }
    }
}

/** ioctl `cmd` on `fd` with the argument at `arg`: 0, or a negated errno. */
static long drm_ioctl(int fd, unsigned long cmd, void *arg)
{
    return syscall3(SYS_IOCTL, fd, (long)cmd, (long)arg);
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

/** The first card of /dev/dri/card0 to card15 with dumb buffers. */
static int open_card(void)
{
    struct drm_get_cap cap;
    char path[32];
    int n, fd;

    for (n = 0; n < 16; n++) {
        node_path(path, "/dev/dri/card", n);
        fd = (int)syscall3(SYS_OPEN, (long)path, O_RDWR | O_CLOEXEC, 0);
        if (fd < 0) {
            continue;
        }
        cap.capability = DRM_CAP_DUMB_BUFFER;
        cap.value = 0;
        if (drm_ioctl(fd, DRM_IOCTL_GET_CAP, &cap) == 0 && cap.value == 1) {
            report_held();
            return fd;
        }
        syscall1(SYS_CLOSE, fd);
    }
    fail("no card of /dev/dri/card0 to card15 has dumb buffers");
    return -1;
}

/** VERSION: the lengths alone with no buffers, then the strings into
 * buffers, each length the same both times and each string within it. */
static void version_row(int fd, char *name, unsigned long size)
{
    static char date[64], desc[128];
    union guarded g;
    struct drm_version *v = (struct drm_version *)guard_set(&g, sizeof(*v));
    unsigned long name_len, date_len, desc_len;

    if (drm_ioctl(fd, DRM_IOCTL_VERSION, v) != 0) {
        fail("VERSION with no buffers");
    }
    guard_check(&g, sizeof(*v), "VERSION wrote past its 36 bytes");
    name_len = v->name_len;
    date_len = v->date_len;
    desc_len = v->desc_len;
    if (name_len == 0 || name_len >= size || date_len >= sizeof(date) || desc_len >= sizeof(desc)) {
        fail("VERSION's lengths");
    }
    v = (struct drm_version *)guard_set(&g, sizeof(*v));
    fill(name, 0, size);
    v->name_len = size - 1;
    v->name = name;
    v->date_len = sizeof(date) - 1;
    v->date = date;
    v->desc_len = sizeof(desc) - 1;
    v->desc = desc;
    if (drm_ioctl(fd, DRM_IOCTL_VERSION, v) != 0) {
        fail("VERSION into buffers");
    }
    guard_check(&g, sizeof(*v), "VERSION wrote past its 36 bytes");
    if (v->name_len != name_len || v->date_len != date_len || v->desc_len != desc_len) {
        fail("VERSION's lengths differ with buffers");
    }
    if (v->name != name || v->date != date || v->desc != desc) {
        fail("VERSION changed its buffers' pointers");
    }
    if (name[0] == '\0' || name[name_len] != '\0' || name[name_len - 1] == '\0') {
        fail("VERSION's name is not its length");
    }
    if (v->version_major < 0 || v->version_minor < 0 || v->version_patchlevel < 0) {
        fail("VERSION's numbers");
    }
    report_held();
}

/** GETRESOURCES: the counts with no arrays, then the ids into arrays at the
 * probe's 32-bit addresses, each count the same both times. */
static void resources_row(int fd)
{
    static u32 crtcs[MAX_IDS], connectors[MAX_IDS];
    union guarded g;
    struct drm_mode_card_res *r = (struct drm_mode_card_res *)guard_set(&g, sizeof(*r));
    u32 n_crtcs, n_connectors, i;

    if (drm_ioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, r) != 0) {
        fail("GETRESOURCES with no arrays");
    }
    guard_check(&g, sizeof(*r), "GETRESOURCES wrote past its 64 bytes");
    n_crtcs = r->count_crtcs;
    n_connectors = r->count_connectors;
    if (n_crtcs == 0 || n_crtcs > MAX_IDS || n_connectors == 0 || n_connectors > MAX_IDS) {
        fail("GETRESOURCES's counts");
    }
    if (r->max_width < DUMB_SIDE || r->max_height < DUMB_SIDE || r->min_width > r->max_width ||
        r->min_height > r->max_height) {
        fail("GETRESOURCES's limits");
    }
    r = (struct drm_mode_card_res *)guard_set(&g, sizeof(*r));
    r->crtc_id_ptr = (u64)(unsigned long)crtcs;
    r->count_crtcs = n_crtcs;
    r->connector_id_ptr = (u64)(unsigned long)connectors;
    r->count_connectors = n_connectors;
    if (drm_ioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, r) != 0) {
        fail("GETRESOURCES into arrays");
    }
    guard_check(&g, sizeof(*r), "GETRESOURCES wrote past its 64 bytes");
    if (r->count_crtcs != n_crtcs || r->count_connectors != n_connectors) {
        fail("GETRESOURCES's counts differ with arrays");
    }
    for (i = 0; i < n_crtcs; i++) {
        if (crtcs[i] == 0) {
            fail("GETRESOURCES lists a CRTC of id 0");
        }
    }
    for (i = 0; i < n_connectors; i++) {
        if (connectors[i] == 0) {
            fail("GETRESOURCES lists a connector of id 0");
        }
    }
    report_held();
}

/** ADDFB2 on a dumb buffer, then GETFB2 of the framebuffer it adds, the
 * modifiers at 68 in both; RMFB and DESTROY_DUMB clean up. */
static void fb2_rows(int fd)
{
    struct drm_mode_create_dumb c;
    struct drm_mode_destroy_dumb d;
    union guarded g;
    struct drm_mode_fb_cmd2 *f;
    u32 fb_id;

    fill(&c, 0, sizeof(c));
    c.width = DUMB_SIDE;
    c.height = DUMB_SIDE;
    c.bpp = DUMB_BPP;
    if (drm_ioctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &c) != 0 || c.handle == 0 ||
        c.pitch < DUMB_SIDE * 4) {
        fail("CREATE_DUMB of the framebuffer's buffer");
    }
    f = (struct drm_mode_fb_cmd2 *)guard_set(&g, sizeof(*f));
    f->width = DUMB_SIDE;
    f->height = DUMB_SIDE;
    f->pixel_format = DRM_FORMAT_XRGB8888;
    f->handles[0] = c.handle;
    f->pitches[0] = c.pitch;
    if (drm_ioctl(fd, DRM_IOCTL_MODE_ADDFB2, f) != 0) {
        fail("ADDFB2 of XRGB8888");
    }
    guard_check(&g, sizeof(*f), "ADDFB2 wrote past its 100 bytes");
    fb_id = f->fb_id;
    if (fb_id == 0) {
        fail("ADDFB2 gives a framebuffer of id 0");
    }
    if (f->width != DUMB_SIDE || f->handles[0] != c.handle || f->pitches[0] != c.pitch) {
        fail("ADDFB2 changed its request");
    }
    report_held();

    f = (struct drm_mode_fb_cmd2 *)guard_set(&g, sizeof(*f));
    f->fb_id = fb_id;
    fill(f->modifier, 0xFF, sizeof(f->modifier));
    if (drm_ioctl(fd, DRM_IOCTL_MODE_GETFB2, f) != 0) {
        fail("GETFB2 of the framebuffer ADDFB2 added");
    }
    guard_check(&g, sizeof(*f), "GETFB2 wrote past its 100 bytes");
    if (f->fb_id != fb_id || f->width != DUMB_SIDE || f->height != DUMB_SIDE ||
        f->pixel_format != DRM_FORMAT_XRGB8888 || f->pitches[0] != c.pitch || f->handles[0] == 0) {
        fail("GETFB2's framebuffer");
    }
    if (f->modifier[0] != 0 || f->modifier[1] != 0 || f->modifier[2] != 0 || f->modifier[3] != 0) {
        fail("GETFB2's modifiers at 68");
    }
    if (drm_ioctl(fd, DRM_IOCTL_MODE_RMFB, &fb_id) != 0) {
        fail("RMFB of the framebuffer");
    }
    d.handle = c.handle;
    if (drm_ioctl(fd, DRM_IOCTL_MODE_DESTROY_DUMB, &d) != 0) {
        fail("DESTROY_DUMB of the framebuffer's buffer");
    }
    report_held();
}

/** WAIT_VBLANK relative 0: the last vblank's count and time, or EOPNOTSUPP
 * with the request copied back on a card with no vblank interrupt. */
static void vblank_row(int fd)
{
    union guarded g;
    union drm_wait_vblank *w = (union drm_wait_vblank *)guard_set(&g, sizeof(*w));
    long rc;

    w->request.type = _DRM_VBLANK_RELATIVE;
    w->request.sequence = 0;
    rc = drm_ioctl(fd, DRM_IOCTL_WAIT_VBLANK, w);
    guard_check(&g, sizeof(*w), "WAIT_VBLANK wrote past its 16 bytes");
    if (rc == -EOPNOTSUPP) {
        if (w->reply.type != _DRM_VBLANK_RELATIVE || w->reply.sequence != 0 ||
            w->reply.tval_sec != 0 || w->reply.tval_usec != 0) {
            fail("WAIT_VBLANK's EOPNOTSUPP changed the request");
        }
        probe_line("DRMCOMPATPROBE VBLANK none", 0, 0);
    } else if (rc != 0) {
        fail("WAIT_VBLANK relative 0");
    } else if (w->reply.tval_sec < 0 || w->reply.tval_usec < 0 || w->reply.tval_usec >= 1000000) {
        fail("WAIT_VBLANK's time");
    }
    report_held();
}

void probe_main(unsigned long *sp);

PROBE_SP_ENTRY;

/** Run the rows in order; `_start` calls it with the initial stack. */
void probe_main(unsigned long *sp)
{
    static const char *const ids[] = {"open", "version", "resources", "addfb2", "getfb2", "vblank"};
    char name[64];
    unsigned i;
    int fd;

    report_begin(report_case((char **)(sp + 1) + sp[0] + 1, "guest.ia32.drmcompatprobe"));
    for (i = 0; i < sizeof(ids) / sizeof(ids[0]); i++) {
        report_plan(ids[i]);
    }
    fd = open_card();
    version_row(fd, name, sizeof(name));
    resources_row(fd);
    fb2_rows(fd);
    vblank_row(fd);
    syscall1(SYS_CLOSE, fd);
    report_end();
    probe_line("DRMCOMPATPROBE CARD ", name, 0);
    probe_line("DRMCOMPATPROBE OK", 0, 0);
    sys_exit(0);
}
