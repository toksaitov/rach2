/**
 * drmcapture: read the frame a DRM card scans out, through GETFB2 and a
 * mapping of its buffer, and give its pixels' CRC-32, the same on Rach2 and
 * on Linux for the same frame.
 *
 *   drmcapture [-p] [-o FILE] [-v COUNT [-c]] [CARD]
 *
 * CARD is /dev/dri/card0 by default. The frame is that of the first CRTC
 * that scans out a framebuffer; with -p the program first draws a fixed
 * pattern in a dumb buffer of its own at the preferred mode of the first
 * connected connector, sets it on a CRTC of that connector as the card's
 * master, and reads that CRTC's frame back the same way. GETFB2 gives the
 * buffer's handle only to root or the card's master, as Linux does.
 *
 * It prints one line, "drmcapture: crtc ID fb ID WxH FORMAT crc32 0xSUM",
 * and with -o writes the frame to FILE as a binary PPM. The sum covers each
 * pixel's red, green and blue bytes in that order, row by row, without the
 * alpha or padding byte and the row's padding, so that two drivers that lay
 * the same picture out in different 32-bit formats or pitches give the same
 * sum. It reads linear 32-bit RGB formats alone (XR24, AR24, XB24, AB24).
 *
 * With -v it reads no frame: it asks for COUNT vblank events of the CRTC,
 * four ahead, and prints how late each reached it by CLOCK_MONOTONIC after
 * the vblank's own stamp, and with -c it has the CRTC's CRC source, "auto",
 * compose each frame meanwhile. On vkms, Linux's and Rach2's, the stamp is
 * the vblank timer's due time, so the lateness is the timer's and the
 * reader's wake together, and a stamp more than a period and a half after
 * the last counts the frames the timer skipped. It prints one line,
 * "drmcapture: vblanks: N at WxH, period P us: ...", with the events 1 and
 * 5 ms or more late, the median, 99th percentile and worst lateness, the
 * frames skipped and the CRC lines read, and a second, "drmcapture: cpus:",
 * with each processor's busy and steal time over the same span by
 * /proc/stat, the steal being the time its hypervisor ran something else.
 *
 * Exits 0, or 1 with "drmcapture: what: why" on standard error.
 *
 * It uses the C library and the kernel's ioctls alone, with Linux's
 * structures written out here, so that the musl link of Rach2's tests and
 * Debian's gcc build the same source and neither needs libdrm.
 */
#define _POSIX_C_SOURCE   200809L
#define _FILE_OFFSET_BITS 64
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <time.h>
#include <unistd.h>

/* Linux's ioctl numbers: the direction in the top two bits, the size of the
 * argument, the type 'd' and the number. */
#define DRM_IOW(nr, t)  (0x40000000UL | ((unsigned long)sizeof(t) << 16) | 0x6400UL | (nr))
#define DRM_IOWR(nr, t) (0xC0000000UL | ((unsigned long)sizeof(t) << 16) | 0x6400UL | (nr))

#define FOURCC(a, b, c, d) \
    ((uint32_t)(a) | (uint32_t)(b) << 8 | (uint32_t)(c) << 16 | (uint32_t)(d) << 24)
#define FORMAT_XR24 FOURCC('X', 'R', '2', '4')
#define FORMAT_AR24 FOURCC('A', 'R', '2', '4')
#define FORMAT_XB24 FOURCC('X', 'B', '2', '4')
#define FORMAT_AB24 FOURCC('A', 'B', '2', '4')

#define MODE_TYPE_PREFERRED (1u << 3)
#define MODE_FB_MODIFIERS   (1u << 1)
#define CONNECTED           1

/* The structures of Linux's include/uapi/drm/drm_mode.h and drm.h. */
struct mode_card_res {
    uint64_t fb_id_ptr, crtc_id_ptr, connector_id_ptr, encoder_id_ptr;
    uint32_t count_fbs, count_crtcs, count_connectors, count_encoders;
    uint32_t min_width, max_width, min_height, max_height;
};

struct mode_modeinfo {
    uint32_t clock;
    uint16_t hdisplay, hsync_start, hsync_end, htotal, hskew;
    uint16_t vdisplay, vsync_start, vsync_end, vtotal, vscan;
    uint32_t vrefresh, flags, type;
    char name[32];
};

struct mode_crtc {
    uint64_t set_connectors_ptr;
    uint32_t count_connectors, crtc_id, fb_id, x, y, gamma_size, mode_valid;
    struct mode_modeinfo mode;
};

struct mode_get_encoder {
    uint32_t encoder_id, encoder_type, crtc_id, possible_crtcs, possible_clones;
};

struct mode_get_connector {
    uint64_t encoders_ptr, modes_ptr, props_ptr, prop_values_ptr;
    uint32_t count_modes, count_props, count_encoders;
    uint32_t encoder_id, connector_id, connector_type, connector_type_id;
    uint32_t connection, mm_width, mm_height, subpixel, pad;
};

struct mode_fb_cmd2 {
    uint32_t fb_id, width, height, pixel_format, flags;
    uint32_t handles[4], pitches[4], offsets[4];
    uint64_t modifier[4];
};

struct mode_create_dumb {
    uint32_t height, width, bpp, flags, handle, pitch;
    uint64_t size;
};

struct mode_map_dumb {
    uint32_t handle, pad;
    uint64_t offset;
};

struct gem_close {
    uint32_t handle, pad;
};

/* drm.h's union drm_wait_vblank, whose `signal` is an unsigned long, and
 * struct drm_event_vblank. */
union wait_vblank {
    struct {
        uint32_t type, sequence;
        unsigned long signal;
    } request;
    struct {
        uint32_t type, sequence;
        long tval_sec, tval_usec;
    } reply;
};

struct event_vblank {
    uint32_t type, length;
    uint64_t user_data;
    uint32_t tv_sec, tv_usec, sequence, crtc_id;
};

#define VBLANK_ABSOLUTE   0x0
#define VBLANK_RELATIVE   0x1
#define VBLANK_EVENT      0x4000000
#define VBLANK_CRTC_SHIFT 1
#define VBLANK_CRTC_MASK  0x3e
#define EVENT_VBLANK      0x01

#define IOCTL_GEM_CLOSE         DRM_IOW(0x09, struct gem_close)
#define IOCTL_WAIT_VBLANK       DRM_IOWR(0x3A, union wait_vblank)
#define IOCTL_MODE_GETRESOURCES DRM_IOWR(0xA0, struct mode_card_res)
#define IOCTL_MODE_GETCRTC      DRM_IOWR(0xA1, struct mode_crtc)
#define IOCTL_MODE_SETCRTC      DRM_IOWR(0xA2, struct mode_crtc)
#define IOCTL_MODE_GETENCODER   DRM_IOWR(0xA6, struct mode_get_encoder)
#define IOCTL_MODE_GETCONNECTOR DRM_IOWR(0xA7, struct mode_get_connector)
#define IOCTL_MODE_RMFB         DRM_IOWR(0xAF, uint32_t)
#define IOCTL_MODE_CREATE_DUMB  DRM_IOWR(0xB2, struct mode_create_dumb)
#define IOCTL_MODE_MAP_DUMB     DRM_IOWR(0xB3, struct mode_map_dumb)
#define IOCTL_MODE_ADDFB2       DRM_IOWR(0xB8, struct mode_fb_cmd2)
#define IOCTL_MODE_GETFB2       DRM_IOWR(0xCE, struct mode_fb_cmd2)

#define MAX_IDS 32

/* The card's CRTCs and connectors, as GETRESOURCES lists them. */
struct card {
    int fd;
    uint32_t crtcs[MAX_IDS], connectors[MAX_IDS];
    uint32_t ncrtcs, nconnectors;
};

/* A dumb buffer of -p, set on a CRTC as a framebuffer. */
struct pattern {
    uint32_t handle, fb, crtc, width, height, pitch;
    uint64_t size;
    void *map;
};

/** Print "drmcapture: what: why" with errno's text and return 1. */
static int fail(const char *what)
{
    fprintf(stderr, "drmcapture: %s: %s\n", what, strerror(errno));
    return 1;
}

/** Issue `request` on `fd`, again after an interruption; 0 or -1. */
static int drm(int fd, unsigned long request, void *arg)
{
    int rc;

    do
        rc = ioctl(fd, request, arg);
    while (rc == -1 && (errno == EINTR || errno == EAGAIN));
    return rc;
}

/** List the card's CRTCs and connectors, up to MAX_IDS each; 0 or -1. */
static int resources(struct card *c)
{
    struct mode_card_res r;

    memset(&r, 0, sizeof(r));
    if (drm(c->fd, IOCTL_MODE_GETRESOURCES, &r) != 0)
        return -1;
    c->ncrtcs = r.count_crtcs < MAX_IDS ? r.count_crtcs : MAX_IDS;
    c->nconnectors = r.count_connectors < MAX_IDS ? r.count_connectors : MAX_IDS;
    memset(&r, 0, sizeof(r));
    r.count_crtcs = c->ncrtcs;
    r.count_connectors = c->nconnectors;
    r.crtc_id_ptr = (uint64_t)(uintptr_t)c->crtcs;
    r.connector_id_ptr = (uint64_t)(uintptr_t)c->connectors;
    if (drm(c->fd, IOCTL_MODE_GETRESOURCES, &r) != 0)
        return -1;
    c->ncrtcs = r.count_crtcs < c->ncrtcs ? r.count_crtcs : c->ncrtcs;
    c->nconnectors = r.count_connectors < c->nconnectors ? r.count_connectors : c->nconnectors;
    return 0;
}

/**
 * Find the first connected connector, its preferred mode, or its first
 * when none is preferred, and a CRTC that can drive it: its encoder's
 * current one, or else the first its encoder may use.
 *
 * @return 0, or -1 with errno ENODEV when no connector is connected.
 */
static int output(struct card *c, uint32_t *connector, uint32_t *crtc, struct mode_modeinfo *mode)
{
    struct mode_modeinfo modes[64];
    uint32_t encoders[8], i, k;

    for (i = 0; i < c->nconnectors; i++) {
        struct mode_get_connector g;
        struct mode_get_encoder e;

        memset(&g, 0, sizeof(g));
        g.connector_id = c->connectors[i];
        if (drm(c->fd, IOCTL_MODE_GETCONNECTOR, &g) != 0)
            return -1;
        if (g.connection != CONNECTED || g.count_modes == 0 || g.count_encoders == 0)
            continue;
        g.count_modes = g.count_modes < 64 ? g.count_modes : 64;
        g.count_encoders = g.count_encoders < 8 ? g.count_encoders : 8;
        g.count_props = 0;
        g.modes_ptr = (uint64_t)(uintptr_t)modes;
        g.encoders_ptr = (uint64_t)(uintptr_t)encoders;
        if (drm(c->fd, IOCTL_MODE_GETCONNECTOR, &g) != 0)
            return -1;
        *mode = modes[0];
        for (k = 0; k < g.count_modes && k < 64; k++)
            if (modes[k].type & MODE_TYPE_PREFERRED) {
                *mode = modes[k];
                break;
            }
        memset(&e, 0, sizeof(e));
        e.encoder_id = g.encoder_id ? g.encoder_id : encoders[0];
        if (drm(c->fd, IOCTL_MODE_GETENCODER, &e) != 0)
            return -1;
        *connector = g.connector_id;
        *crtc = e.crtc_id;
        for (k = 0; *crtc == 0 && k < c->ncrtcs; k++)
            if (e.possible_crtcs & (1u << k))
                *crtc = c->crtcs[k];
        if (*crtc != 0)
            return 0;
    }
    errno = ENODEV;
    return -1;
}

/** The fixed pattern of -p: red across, green down, blue their XOR. */
static uint32_t pattern_pixel(uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{
    uint32_t r = w > 1 ? x * 255 / (w - 1) : 0, g = h > 1 ? y * 255 / (h - 1) : 0;

    return r << 16 | g << 8 | ((x ^ y) & 0xff);
}

/**
 * Draw the pattern in a new dumb buffer at the preferred mode of the first
 * connected connector and set it on that connector's CRTC.
 *
 * @return 0 with `p` filled, or 1 after a failure, reported.
 */
static int draw(struct card *c, struct pattern *p)
{
    struct mode_create_dumb d;
    struct mode_map_dumb m;
    struct mode_fb_cmd2 f;
    struct mode_crtc s;
    struct mode_modeinfo mode;
    uint32_t connector, x, y;

    memset(p, 0, sizeof(*p));
    if (output(c, &connector, &p->crtc, &mode) != 0)
        return fail("connector");
    memset(&d, 0, sizeof(d));
    d.width = mode.hdisplay;
    d.height = mode.vdisplay;
    d.bpp = 32;
    if (drm(c->fd, IOCTL_MODE_CREATE_DUMB, &d) != 0)
        return fail("CREATE_DUMB");
    p->handle = d.handle;
    p->width = d.width;
    p->height = d.height;
    p->pitch = d.pitch;
    p->size = d.size;
    memset(&m, 0, sizeof(m));
    m.handle = d.handle;
    if (drm(c->fd, IOCTL_MODE_MAP_DUMB, &m) != 0)
        return fail("MAP_DUMB");
    p->map = mmap(NULL, (size_t)d.size, PROT_READ | PROT_WRITE, MAP_SHARED, c->fd, (off_t)m.offset);
    if (p->map == MAP_FAILED) {
        p->map = NULL;
        return fail("mmap");
    }
    for (y = 0; y < p->height; y++)
        for (x = 0; x < p->width; x++)
            ((uint32_t *)((char *)p->map + (size_t)y * p->pitch))[x] =
                pattern_pixel(x, y, p->width, p->height);
    memset(&f, 0, sizeof(f));
    f.width = p->width;
    f.height = p->height;
    f.pixel_format = FORMAT_XR24;
    f.handles[0] = p->handle;
    f.pitches[0] = p->pitch;
    if (drm(c->fd, IOCTL_MODE_ADDFB2, &f) != 0)
        return fail("ADDFB2");
    p->fb = f.fb_id;
    memset(&s, 0, sizeof(s));
    s.crtc_id = p->crtc;
    s.fb_id = p->fb;
    s.set_connectors_ptr = (uint64_t)(uintptr_t)&connector;
    s.count_connectors = 1;
    s.mode = mode;
    s.mode_valid = 1;
    if (drm(c->fd, IOCTL_MODE_SETCRTC, &s) != 0)
        return fail("SETCRTC");
    return 0;
}

/** Free what draw() made: the framebuffer, the mapping and the buffer. */
static void undraw(struct card *c, struct pattern *p)
{
    struct gem_close g;

    if (p->fb != 0)
        drm(c->fd, IOCTL_MODE_RMFB, &p->fb);
    if (p->map != NULL)
        munmap(p->map, (size_t)p->size);
    if (p->handle != 0) {
        memset(&g, 0, sizeof(g));
        g.handle = p->handle;
        drm(c->fd, IOCTL_GEM_CLOSE, &g);
    }
}

/** Fold `n` bytes of `b` into the CRC-32 `crc` (ISO-HDLC, as zlib's). */
static uint32_t crc32_add(uint32_t crc, const unsigned char *b, size_t n)
{
    size_t i;
    int k;

    crc = ~crc;
    for (i = 0; i < n; i++) {
        crc ^= b[i];
        for (k = 0; k < 8; k++)
            crc = crc >> 1 ^ (0xEDB88320u & (0u - (crc & 1)));
    }
    return ~crc;
}

/** Give the four characters of `format` in `out`. */
static void format_name(uint32_t format, char out[5])
{
    int i;

    for (i = 0; i < 4; i++)
        out[i] = (char)(format >> (8 * i));
    out[4] = 0;
}

/**
 * Find CRTC `crtc`, or the first CRTC when `crtc` is 0, that scans out a
 * framebuffer, and fill `g` with it.
 *
 * @return 0, or 1 after a failure, reported.
 */
static int scanout(struct card *c, uint32_t crtc, struct mode_crtc *g)
{
    uint32_t i;

    for (i = 0; i < c->ncrtcs; i++) {
        memset(g, 0, sizeof(*g));
        g->crtc_id = c->crtcs[i];
        if ((crtc == 0 || crtc == g->crtc_id) && drm(c->fd, IOCTL_MODE_GETCRTC, g) == 0 &&
            g->fb_id != 0 && g->mode_valid)
            return 0;
    }
    errno = ENOENT;
    return fail("no CRTC scans out a framebuffer");
}

/**
 * Give in `crc` the CRC-32 of the red, green and blue bytes of each pixel of
 * framebuffer `f`, mapped at `base`, and write the pixels to `ppm` as a
 * binary PPM when that is not NULL.
 *
 * @return 0, or 1 after a failure, reported.
 */
static int sum(const struct mode_fb_cmd2 *f, const unsigned char *base, const char *ppm,
               uint32_t *crc)
{
    int bgr = f->pixel_format == FORMAT_XB24 || f->pixel_format == FORMAT_AB24;
    FILE *out = NULL;
    uint32_t x, y;

    if (ppm != NULL && ((out = fopen(ppm, "wb")) == NULL ||
                        fprintf(out, "P6\n%u %u\n255\n", f->width, f->height) < 0))
        goto failed;
    *crc = 0;
    for (y = 0; y < f->height; y++) {
        const unsigned char *row = base + f->offsets[0] + (size_t)y * f->pitches[0];

        for (x = 0; x < f->width; x++) {
            const unsigned char *px = row + 4 * x;
            unsigned char rgb[3];

            rgb[0] = bgr ? px[0] : px[2];
            rgb[1] = px[1];
            rgb[2] = bgr ? px[2] : px[0];
            *crc = crc32_add(*crc, rgb, 3);
            if (out != NULL && fwrite(rgb, 1, 3, out) != 3)
                goto failed;
        }
    }
    if (out != NULL && fclose(out) != 0) {
        out = NULL;
        goto failed;
    }
    return 0;
failed:
    if (out != NULL)
        fclose(out);
    return fail(ppm);
}

/**
 * Read the frame of CRTC `crtc`, or of the first CRTC that scans out a
 * framebuffer when `crtc` is 0, print its line and write it to `ppm` when
 * that is not NULL.
 *
 * @return 0, or 1 after a failure, reported.
 */
static int capture(struct card *c, uint32_t crtc, const char *ppm)
{
    struct mode_crtc g;
    struct mode_fb_cmd2 f;
    struct mode_map_dumb m;
    struct gem_close handle;
    unsigned char *base;
    uint32_t crc = 0;
    size_t size;
    char name[5];
    int rc = 1;

    if (scanout(c, crtc, &g) != 0)
        return 1;
    memset(&f, 0, sizeof(f));
    f.fb_id = g.fb_id;
    if (drm(c->fd, IOCTL_MODE_GETFB2, &f) != 0)
        return fail("GETFB2");
    if (f.handles[0] == 0) {
        errno = EPERM;
        return fail("GETFB2 gave no handle, which needs root or the master role");
    }
    format_name(f.pixel_format, name);
    memset(&m, 0, sizeof(m));
    m.handle = f.handles[0];
    size = (size_t)f.offsets[0] + (size_t)f.pitches[0] * f.height;
    if ((f.pixel_format != FORMAT_XR24 && f.pixel_format != FORMAT_AR24 &&
         f.pixel_format != FORMAT_XB24 && f.pixel_format != FORMAT_AB24) ||
        ((f.flags & MODE_FB_MODIFIERS) && f.modifier[0] != 0))
        fprintf(stderr, "drmcapture: the format %s, modifier 0x%llx, is not linear 32-bit RGB\n",
                name, (unsigned long long)f.modifier[0]);
    else if (drm(c->fd, IOCTL_MODE_MAP_DUMB, &m) != 0)
        fail("MAP_DUMB");
    else if ((base = mmap(NULL, size, PROT_READ, MAP_SHARED, c->fd, (off_t)m.offset)) == MAP_FAILED)
        fail("mmap");
    else {
        rc = sum(&f, base, ppm, &crc);
        munmap(base, size);
    }
    if (rc == 0)
        printf("drmcapture: crtc %u fb %u %ux%u %s crc32 0x%08x\n", g.crtc_id, g.fb_id, f.width,
               f.height, name, crc);
    memset(&handle, 0, sizeof(handle));
    handle.handle = f.handles[0];
    drm(c->fd, IOCTL_GEM_CLOSE, &handle);
    return rc;
}

/* The events -v keeps asked for ahead of the one it waits for, and the
 * most whose lateness it keeps for its percentiles. */
#define VBLANK_AHEAD 4
#define VBLANK_MAX   100000

/* The processors /proc/stat lists that -v reports. */
#define STAT_CPUS 64

/* A processor's times by /proc/stat, in clock ticks. */
struct cpu_times {
    unsigned long long busy, steal;
};

/**
 * Read each processor's busy time, its user, nice, system, irq and softirq
 * times, and its steal time from /proc/stat into `t`.
 *
 * @return the processors read, or 0 when /proc/stat cannot be read.
 */
static int cpu_stat(struct cpu_times t[STAT_CPUS])
{
    unsigned long long v[8];
    char line[512];
    FILE *f = fopen("/proc/stat", "r");
    int n = 0, cpu;

    if (f == NULL)
        return 0;
    while (fgets(line, sizeof(line), f) != NULL && n < STAT_CPUS) {
        memset(v, 0, sizeof(v));
        /* "cpu " sums the processors; sscanf's %d would read it as cpu0 */
        if (strncmp(line, "cpu", 3) != 0 || line[3] < '0' || line[3] > '9' ||
            sscanf(line, "cpu%d %llu %llu %llu %llu %llu %llu %llu %llu", &cpu, &v[0], &v[1], &v[2],
                   &v[3], &v[4], &v[5], &v[6], &v[7]) < 5 ||
            cpu != n)
            continue;
        t[n].busy = v[0] + v[1] + v[2] + v[5] + v[6];
        t[n].steal = v[7];
        n++;
    }
    fclose(f);
    return n;
}

/** Print each processor's busy and steal time between `a` and `b`. */
static void cpu_report(const struct cpu_times *a, const struct cpu_times *b, int n)
{
    long hz = sysconf(_SC_CLK_TCK);
    int i;

    if (n == 0 || hz <= 0)
        return;
    printf("drmcapture: cpus:");
    for (i = 0; i < n; i++)
        printf("%s cpu%d busy %llu ms steal %llu ms", i ? "," : "", i,
               (b[i].busy - a[i].busy) * 1000 / (unsigned long long)hz,
               (b[i].steal - a[i].steal) * 1000 / (unsigned long long)hz);
    printf("\n");
}

/** The order of two latenesses, for qsort. */
static int by_lateness(const void *a, const void *b)
{
    long x = *(const long *)a, y = *(const long *)b;

    return x < y ? -1 : x > y;
}

/** CLOCK_MONOTONIC in microseconds, the clock of DRM's vblank stamps. */
static long long now_us(void)
{
    struct timespec t;

    clock_gettime(CLOCK_MONOTONIC, &t);
    return (long long)t.tv_sec * 1000000 + t.tv_nsec / 1000;
}

/**
 * Ask for the event of vblank `*seq` of the CRTC at `index` of the card's
 * list, or with `event` 0 give the CRTC's count now in `*seq`; 0 or -1.
 */
static int wait_vblank(struct card *c, uint32_t index, uint32_t *seq, int event)
{
    union wait_vblank w;

    memset(&w, 0, sizeof(w));
    w.request.type = (index << VBLANK_CRTC_SHIFT & VBLANK_CRTC_MASK) |
                     (event ? VBLANK_ABSOLUTE | VBLANK_EVENT : VBLANK_RELATIVE);
    w.request.sequence = event ? *seq : 0;
    w.request.signal = event ? *seq : 0;
    if (drm(c->fd, IOCTL_WAIT_VBLANK, &w) != 0)
        return -1;
    if (!event)
        *seq = w.reply.sequence;
    return 0;
}

/**
 * Open CRTC `index`'s crc/data on card `path` after setting its source to
 * "auto", so that the driver composes each frame while it is open.
 *
 * @return the descriptor, nonblocking, or -1 after a failure, reported.
 */
static int crc_open(const char *path, uint32_t index)
{
    char name[96];
    struct stat st;
    int fd;

    if (stat(path, &st) != 0) {
        fail(path);
        return -1;
    }
    snprintf(name, sizeof(name), "/sys/kernel/debug/dri/%u/crtc-%u/crc/control",
             (unsigned)minor(st.st_rdev), (unsigned)index);
    if ((fd = open(name, O_WRONLY | O_CLOEXEC)) < 0 || write(fd, "auto", 4) != 4) {
        fail(name);
        if (fd >= 0)
            close(fd);
        return -1;
    }
    close(fd);
    snprintf(name, sizeof(name), "/sys/kernel/debug/dri/%u/crtc-%u/crc/data",
             (unsigned)minor(st.st_rdev), (unsigned)index);
    if ((fd = open(name, O_RDONLY | O_NONBLOCK | O_CLOEXEC)) < 0)
        fail(name);
    return fd;
}

/* What -v counts: the CRTC at `index` of the card's list, its period in
 * us, the next vblank to ask for, and the events taken so far. */
struct vblank_run {
    uint32_t index, next;
    long period, n, late1, late5, worst, skipped, crcs;
    long long prev; /* the last event's stamp in us, 0 before the first */
    long late[VBLANK_MAX];
};

/**
 * Count the events in `buf`, `got` bytes that a read of the card gave, in
 * `r`, each late by the time now after its stamp, and ask for one more
 * vblank for each.
 *
 * @return 0, or 1 after a failure, reported.
 */
static int vblank_take(struct card *c, struct vblank_run *r, const char *buf, ssize_t got)
{
    ssize_t at;

    for (at = 0; at + (ssize_t)sizeof(struct event_vblank) <= got;) {
        struct event_vblank e;
        long long stamp, now = now_us();
        long l;

        memcpy(&e, buf + at, sizeof(e));
        at += e.length ? (ssize_t)e.length : got;
        if (e.type != EVENT_VBLANK)
            continue;
        stamp = (long long)e.tv_sec * 1000000 + e.tv_usec;
        l = (long)(now - stamp);
        if (r->prev != 0 && stamp - r->prev > r->period + r->period / 2)
            r->skipped += (long)((stamp - r->prev + r->period / 2) / r->period) - 1;
        r->prev = stamp;
        r->late1 += l >= 1000;
        r->late5 += l >= 5000;
        r->worst = l > r->worst ? l : r->worst;
        if (r->n < VBLANK_MAX)
            r->late[r->n] = l;
        r->n++;
        if (wait_vblank(c, r->index, &r->next, 1) != 0)
            return fail("WAIT_VBLANK");
        r->next++;
    }
    return 0;
}

/**
 * Wait for and take `count` vblank events into `r`, VBLANK_AHEAD asked for
 * ahead, reading and counting the CRC lines of `crcfd` meanwhile unless it
 * is -1.
 *
 * @return 0, or 1 after a failure, reported.
 */
static int vblank_loop(struct card *c, struct vblank_run *r, int crcfd, long count)
{
    struct pollfd fds[2];
    char buf[4096];
    uint32_t seq = 0;
    ssize_t got, at;

    if (wait_vblank(c, r->index, &seq, 0) != 0)
        return fail("WAIT_VBLANK");
    for (r->next = seq + 1; r->next != seq + 1 + VBLANK_AHEAD; r->next++)
        if (wait_vblank(c, r->index, &r->next, 1) != 0)
            return fail("WAIT_VBLANK");
    fds[0].fd = c->fd;
    fds[0].events = POLLIN;
    fds[1].fd = crcfd;
    fds[1].events = POLLIN;
    while (r->n < count) {
        if (poll(fds, crcfd >= 0 ? 2 : 1, 5000) <= 0) {
            errno = errno ? errno : ETIMEDOUT;
            return fail("poll for a vblank");
        }
        while (crcfd >= 0 && (got = read(crcfd, buf, sizeof(buf))) > 0)
            for (at = 0; at < got; at++)
                r->crcs += buf[at] == '\n';
        if (!(fds[0].revents & POLLIN))
            continue;
        if ((got = read(c->fd, buf, sizeof(buf))) <= 0)
            return fail("read of a vblank event");
        if (vblank_take(c, r, buf, got) != 0)
            return 1;
    }
    return 0;
}

/**
 * Take `count` vblank events of CRTC `crtc` of card `path`, open as `c`,
 * with its CRCs on when `crc`, and print how late they came and the steal
 * meanwhile, as the comment at the top of this file says.
 *
 * @return 0, or 1 after a failure, reported.
 */
static int vblanks(struct card *c, const char *path, uint32_t crtc, long count, int crc)
{
    static struct vblank_run r;
    struct cpu_times before[STAT_CPUS], after[STAT_CPUS];
    struct mode_crtc g;
    long k;
    int rc, ncpus, crcfd;

    for (r.index = 0; r.index < c->ncrtcs && c->crtcs[r.index] != crtc; r.index++)
        ;
    memset(&g, 0, sizeof(g));
    g.crtc_id = crtc;
    if (r.index == c->ncrtcs || drm(c->fd, IOCTL_MODE_GETCRTC, &g) != 0 || !g.mode_valid ||
        g.mode.clock == 0)
        return fail("GETCRTC");
    r.period = (long)((long long)g.mode.htotal * g.mode.vtotal * 1000 / g.mode.clock);
    crcfd = crc ? crc_open(path, r.index) : -1;
    if (crc && crcfd < 0)
        return 1;
    ncpus = cpu_stat(before);
    rc = vblank_loop(c, &r, crcfd, count);
    if (crcfd >= 0)
        close(crcfd);
    if (rc != 0)
        return rc;
    if (cpu_stat(after) != ncpus)
        ncpus = 0;
    k = r.n < VBLANK_MAX ? r.n : VBLANK_MAX;
    qsort(r.late, (size_t)k, sizeof(r.late[0]), by_lateness);
    printf("drmcapture: vblanks: %ld at %ux%u, period %ld us: %ld late by 1 ms or more, "
           "%ld by 5 ms or more; median %ld us, 99th percentile %ld us, worst %ld us; "
           "%ld frames skipped; %ld CRCs\n",
           r.n, g.mode.hdisplay, g.mode.vdisplay, r.period, r.late1, r.late5, r.late[k / 2],
           r.late[k * 99 / 100], r.worst, r.skipped, r.crcs);
    cpu_report(before, after, ncpus);
    return 0;
}

int main(int argc, char **argv)
{
    const char *card = "/dev/dri/card0", *ppm = NULL;
    struct card c;
    struct pattern p;
    struct mode_crtc g;
    long count = 0;
    int opt, draw_pattern = 0, crc = 0, rc;

    while ((opt = getopt(argc, argv, "po:v:c")) != -1) {
        if (opt == 'p')
            draw_pattern = 1;
        else if (opt == 'o')
            ppm = optarg;
        else if (opt == 'v' && (count = strtol(optarg, NULL, 10)) > 0)
            continue;
        else if (opt == 'c')
            crc = 1;
        else {
            fprintf(stderr, "usage: drmcapture [-p] [-o FILE] [-v COUNT [-c]] [CARD]\n");
            return 2;
        }
    }
    if (optind < argc)
        card = argv[optind];
    memset(&c, 0, sizeof(c));
    memset(&p, 0, sizeof(p));
    c.fd = open(card, O_RDWR | O_CLOEXEC);
    if (c.fd < 0)
        return fail(card);
    if (resources(&c) != 0) {
        rc = fail("GETRESOURCES");
    } else {
        rc = draw_pattern ? draw(&c, &p) : 0;
        if (rc == 0 && count > 0)
            rc = scanout(&c, draw_pattern ? p.crtc : 0, &g) ||
                 vblanks(&c, card, g.crtc_id, count, crc);
        else if (rc == 0)
            rc = capture(&c, draw_pattern ? p.crtc : 0, ppm);
    }
    undraw(&c, &p);
    close(c.fd);
    return rc;
}
