/**
 * crcmodes: set one pattern on a panel at each mode asked for and read the
 * pipe's CRC of it through debugfs, the same program on Linux and on Rach2,
 * for 78-10d's comparison of the Lenovo's eDP panel.
 *
 *   crcmodes [-D CARD] [-d DIR] [-n COUNT] WxH[:SCALE]...
 *
 * CARD is /dev/dri/card0 and DIR /sys/kernel/debug/dri/0 by default. The
 * program takes the card's master, finds the first connected connector of
 * type eDP, its preferred mode and its CRTC, and for each argument draws a
 * fixed pattern in a dumb buffer of W by H pixels, sets the connector's
 * "scaling mode" to SCALE (aspect, full or center; unchanged when none is
 * given), and sets the CRTC by SETCRTC with the preferred mode's timings
 * and W by H as its size, as a panel's smaller mode is set through the
 * fitter. It then writes "auto" to DIR/crtc-N/crc/control, N the CRTC's
 * index, opens crc/data, drops its first two lines and reads COUNT (8)
 * more, each the CRC of one frame.
 *
 * It prints a line a mode, "crcmodes: mode WxH SCALE crc 0xCRC frames
 * COUNT" when every frame's first value is the same, "crcmodes: mode WxH
 * SCALE unstable: ..." with each value otherwise, or "crcmodes: mode WxH
 * SCALE failed: what: why", and at the end
 *
 *   crcmodes: result: M modes: stable S unstable U failed F
 *
 * A read that waits 2 s for a frame fails its mode. It puts the scaling
 * mode back as it found it; closing the card gives the screen back to the
 * console. Exits 0 when every mode was set and read, 1 otherwise.
 *
 * It uses the C library and the kernel's ioctls alone, with Linux's
 * structures written out here, as tests/graphics/capture/drmcapture.c does,
 * so that Debian's gcc and Rach2's build the same source without libdrm.
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
#include <unistd.h>

/* Linux's ioctl numbers: the direction in the top two bits, the size of the
 * argument, the type 'd' and the number. */
#define DRM_IO(nr)      (0x6400UL | (nr))
#define DRM_IOW(nr, t)  (0x40000000UL | ((unsigned long)sizeof(t) << 16) | 0x6400UL | (nr))
#define DRM_IOWR(nr, t) (0xC0000000UL | ((unsigned long)sizeof(t) << 16) | 0x6400UL | (nr))

#define FORMAT_XR24 0x34325258u /* 'X' 'R' '2' '4' */

#define MODE_TYPE_PREFERRED (1u << 3)
#define CONNECTED           1
#define CONNECTOR_EDP       14
#define PROP_ENUM           (1u << 3)

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

struct mode_get_property {
    uint64_t values_ptr, enum_blob_ptr;
    uint32_t prop_id, flags;
    char name[32];
    uint32_t count_values, count_enum_blobs;
};

struct mode_property_enum {
    uint64_t value;
    char name[32];
};

struct mode_connector_set_property {
    uint64_t value;
    uint32_t prop_id, connector_id;
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

#define IOCTL_GEM_CLOSE         DRM_IOW(0x09, struct gem_close)
#define IOCTL_SET_MASTER        DRM_IO(0x1e)
#define IOCTL_MODE_GETRESOURCES DRM_IOWR(0xA0, struct mode_card_res)
#define IOCTL_MODE_SETCRTC      DRM_IOWR(0xA2, struct mode_crtc)
#define IOCTL_MODE_GETENCODER   DRM_IOWR(0xA6, struct mode_get_encoder)
#define IOCTL_MODE_GETCONNECTOR DRM_IOWR(0xA7, struct mode_get_connector)
#define IOCTL_MODE_GETPROPERTY  DRM_IOWR(0xAA, struct mode_get_property)
#define IOCTL_MODE_SETPROPERTY  DRM_IOWR(0xAB, struct mode_connector_set_property)
#define IOCTL_MODE_RMFB         DRM_IOWR(0xAF, uint32_t)
#define IOCTL_MODE_CREATE_DUMB  DRM_IOWR(0xB2, struct mode_create_dumb)
#define IOCTL_MODE_MAP_DUMB     DRM_IOWR(0xB3, struct mode_map_dumb)
#define IOCTL_MODE_ADDFB2       DRM_IOWR(0xB8, struct mode_fb_cmd2)

#define MAX_IDS   32
#define MAX_PROPS 64
#define MAX_READS 64

/* The panel: the card, its eDP connector, the connector's preferred mode,
 * the CRTC and its index, and the scaling mode's property. */
struct panel {
    int fd;
    uint32_t connector, crtc, crtc_index;
    struct mode_modeinfo mode;
    uint32_t scaling_prop; /* 0 when the connector has none */
    uint64_t scaling_values[8];
    char scaling_names[8][32];
    uint32_t nscaling;
    uint64_t scaling_was; /* the value found at the start */
};

/* A dumb buffer of one mode, added as a framebuffer. */
struct buffer {
    uint32_t handle, fb, width, height, pitch;
    uint64_t size;
    void *map;
};

static char why[160]; /* the reason of the latest failure */

/** Note "what: errno's text" as the latest failure and return -1. */
static int failed(const char *what)
{
    snprintf(why, sizeof(why), "%s: %s", what, strerror(errno));
    return -1;
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

/**
 * Read the scaling mode's property of the connector from `ids` and
 * `values`, its `count` properties: its id, its enumeration and its value.
 */
static void scaling(struct panel *p, const uint32_t *ids, const uint64_t *values, uint32_t count)
{
    struct mode_property_enum enums[8];
    uint32_t i, k;

    for (i = 0; i < count; i++) {
        struct mode_get_property g;

        memset(&g, 0, sizeof(g));
        g.prop_id = ids[i];
        if (drm(p->fd, IOCTL_MODE_GETPROPERTY, &g) != 0 || strcmp(g.name, "scaling mode") != 0 ||
            !(g.flags & PROP_ENUM))
            continue;
        g.count_enum_blobs = g.count_enum_blobs < 8 ? g.count_enum_blobs : 8;
        g.count_values = 0;
        g.enum_blob_ptr = (uint64_t)(uintptr_t)enums;
        if (drm(p->fd, IOCTL_MODE_GETPROPERTY, &g) != 0)
            continue;
        p->scaling_prop = ids[i];
        p->scaling_was = values[i];
        for (k = 0; k < g.count_enum_blobs && k < 8; k++) {
            p->scaling_values[k] = enums[k].value;
            memcpy(p->scaling_names[k], enums[k].name, 32);
            p->scaling_names[k][31] = 0;
        }
        p->nscaling = k;
        return;
    }
}

/**
 * Find the first connected eDP connector of the card, its preferred mode,
 * its CRTC and that CRTC's index in the card's list, and its scaling mode.
 *
 * @return 0, or -1 with the reason noted.
 */
static int find_panel(struct panel *p)
{
    struct mode_card_res r;
    struct mode_modeinfo modes[64];
    uint32_t crtcs[MAX_IDS], connectors[MAX_IDS], encoders[8], props[MAX_PROPS];
    uint64_t values[MAX_PROPS];
    uint32_t ncrtcs, nconnectors, i, k;

    memset(&r, 0, sizeof(r));
    if (drm(p->fd, IOCTL_MODE_GETRESOURCES, &r) != 0)
        return failed("GETRESOURCES");
    ncrtcs = r.count_crtcs < MAX_IDS ? r.count_crtcs : MAX_IDS;
    nconnectors = r.count_connectors < MAX_IDS ? r.count_connectors : MAX_IDS;
    memset(&r, 0, sizeof(r));
    r.count_crtcs = ncrtcs;
    r.count_connectors = nconnectors;
    r.crtc_id_ptr = (uint64_t)(uintptr_t)crtcs;
    r.connector_id_ptr = (uint64_t)(uintptr_t)connectors;
    if (drm(p->fd, IOCTL_MODE_GETRESOURCES, &r) != 0)
        return failed("GETRESOURCES");
    for (i = 0; i < nconnectors && i < r.count_connectors; i++) {
        struct mode_get_connector g;
        struct mode_get_encoder e;

        memset(&g, 0, sizeof(g));
        g.connector_id = connectors[i];
        if (drm(p->fd, IOCTL_MODE_GETCONNECTOR, &g) != 0)
            return failed("GETCONNECTOR");
        if (g.connector_type != CONNECTOR_EDP || g.connection != CONNECTED || g.count_modes == 0 ||
            g.count_encoders == 0)
            continue;
        g.count_modes = g.count_modes < 64 ? g.count_modes : 64;
        g.count_encoders = g.count_encoders < 8 ? g.count_encoders : 8;
        g.count_props = g.count_props < MAX_PROPS ? g.count_props : MAX_PROPS;
        g.modes_ptr = (uint64_t)(uintptr_t)modes;
        g.encoders_ptr = (uint64_t)(uintptr_t)encoders;
        g.props_ptr = (uint64_t)(uintptr_t)props;
        g.prop_values_ptr = (uint64_t)(uintptr_t)values;
        if (drm(p->fd, IOCTL_MODE_GETCONNECTOR, &g) != 0)
            return failed("GETCONNECTOR");
        p->connector = g.connector_id;
        p->mode = modes[0];
        for (k = 0; k < g.count_modes && k < 64; k++)
            if (modes[k].type & MODE_TYPE_PREFERRED) {
                p->mode = modes[k];
                break;
            }
        memset(&e, 0, sizeof(e));
        e.encoder_id = g.encoder_id ? g.encoder_id : encoders[0];
        if (drm(p->fd, IOCTL_MODE_GETENCODER, &e) != 0)
            return failed("GETENCODER");
        for (k = 0; k < ncrtcs; k++)
            if (e.crtc_id ? crtcs[k] == e.crtc_id : (e.possible_crtcs & (1u << k)) != 0) {
                p->crtc = crtcs[k];
                p->crtc_index = k;
                scaling(p, props, values, g.count_props);
                return 0;
            }
    }
    errno = ENODEV;
    return failed("no connected eDP connector with a CRTC");
}

/** The pattern: red across, green down, blue their XOR, and a white
 * border, so that a frame shifted or cut by a pixel changes its CRC. */
static uint32_t pattern_pixel(uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{
    uint32_t r = w > 1 ? x * 255 / (w - 1) : 0, g = h > 1 ? y * 255 / (h - 1) : 0;

    if (x == 0 || y == 0 || x == w - 1 || y == h - 1)
        return 0xffffff;
    return r << 16 | g << 8 | ((x ^ y) & 0xff);
}

/**
 * Make a dumb buffer of `w` by `h`, draw the pattern in it and add it as a
 * framebuffer.
 *
 * @return 0 with `b` filled, or -1 with the reason noted; `b` holds what
 *         was made either way, for unmake().
 */
static int make(struct panel *p, struct buffer *b, uint32_t w, uint32_t h)
{
    struct mode_create_dumb d;
    struct mode_map_dumb m;
    struct mode_fb_cmd2 f;
    uint32_t x, y;

    memset(b, 0, sizeof(*b));
    memset(&d, 0, sizeof(d));
    d.width = w;
    d.height = h;
    d.bpp = 32;
    if (drm(p->fd, IOCTL_MODE_CREATE_DUMB, &d) != 0)
        return failed("CREATE_DUMB");
    b->handle = d.handle;
    b->width = w;
    b->height = h;
    b->pitch = d.pitch;
    b->size = d.size;
    memset(&m, 0, sizeof(m));
    m.handle = d.handle;
    if (drm(p->fd, IOCTL_MODE_MAP_DUMB, &m) != 0)
        return failed("MAP_DUMB");
    b->map = mmap(NULL, (size_t)d.size, PROT_READ | PROT_WRITE, MAP_SHARED, p->fd, (off_t)m.offset);
    if (b->map == MAP_FAILED) {
        b->map = NULL;
        return failed("mmap");
    }
    for (y = 0; y < h; y++)
        for (x = 0; x < w; x++)
            ((uint32_t *)((char *)b->map + (size_t)y * b->pitch))[x] = pattern_pixel(x, y, w, h);
    memset(&f, 0, sizeof(f));
    f.width = w;
    f.height = h;
    f.pixel_format = FORMAT_XR24;
    f.handles[0] = b->handle;
    f.pitches[0] = b->pitch;
    if (drm(p->fd, IOCTL_MODE_ADDFB2, &f) != 0)
        return failed("ADDFB2");
    b->fb = f.fb_id;
    return 0;
}

/** Free what make() made: the framebuffer, the mapping and the buffer. */
static void unmake(struct panel *p, struct buffer *b)
{
    struct gem_close g;

    if (b->fb != 0)
        drm(p->fd, IOCTL_MODE_RMFB, &b->fb);
    if (b->map != NULL)
        munmap(b->map, (size_t)b->size);
    if (b->handle != 0) {
        memset(&g, 0, sizeof(g));
        g.handle = b->handle;
        drm(p->fd, IOCTL_GEM_CLOSE, &g);
    }
    memset(b, 0, sizeof(*b));
}

/**
 * Set the connector's scaling mode to the entry named `name` (Linux's
 * "Full aspect", "Full" or "Center").
 *
 * @return 0, or -1 with the reason noted.
 */
static int set_scaling(struct panel *p, const char *name)
{
    struct mode_connector_set_property s;
    uint32_t k;

    if (p->scaling_prop == 0) {
        errno = ENOENT;
        return failed("the connector has no scaling mode");
    }
    for (k = 0; k < p->nscaling && strcmp(p->scaling_names[k], name) != 0; k++)
        ;
    if (k == p->nscaling) {
        errno = EINVAL;
        return failed(name);
    }
    memset(&s, 0, sizeof(s));
    s.connector_id = p->connector;
    s.prop_id = p->scaling_prop;
    s.value = p->scaling_values[k];
    if (drm(p->fd, IOCTL_MODE_SETPROPERTY, &s) != 0)
        return failed("SETPROPERTY");
    return 0;
}

/**
 * Set the CRTC to the panel's mode with `w` by `h` as its size, showing
 * framebuffer `fb`.
 *
 * @return 0, or -1 with the reason noted.
 */
static int set_mode(struct panel *p, uint32_t w, uint32_t h, uint32_t fb)
{
    struct mode_crtc s;

    memset(&s, 0, sizeof(s));
    s.crtc_id = p->crtc;
    s.fb_id = fb;
    s.set_connectors_ptr = (uint64_t)(uintptr_t)&p->connector;
    s.count_connectors = 1;
    s.mode = p->mode;
    s.mode.hdisplay = (uint16_t)w;
    s.mode.vdisplay = (uint16_t)h;
    s.mode.type = 0;
    snprintf(s.mode.name, sizeof(s.mode.name), "%ux%u", w, h);
    s.mode_valid = 1;
    if (drm(p->fd, IOCTL_MODE_SETCRTC, &s) != 0)
        return failed("SETCRTC");
    return 0;
}

/**
 * Read `count` CRCs of the CRTC from debugfs below `dir` into `crcs`,
 * after dropping the first two, each the first value of its line.
 *
 * @return 0, or -1 with the reason noted.
 */
static int read_crcs(struct panel *p, const char *dir, uint32_t *crcs, int count)
{
    char path[256], line[256];
    size_t have = 0;
    int fd, got = -2, rc = -1;

    snprintf(path, sizeof(path), "%s/crtc-%u/crc/control", dir, p->crtc_index);
    fd = open(path, O_WRONLY);
    if (fd < 0)
        return failed(path);
    if (write(fd, "auto", 4) != 4) {
        failed("crc/control");
        close(fd);
        return -1;
    }
    close(fd);
    snprintf(path, sizeof(path), "%s/crtc-%u/crc/data", dir, p->crtc_index);
    fd = open(path, O_RDONLY);
    if (fd < 0)
        return failed(path);
    while (got < count) {
        struct pollfd w;
        char *nl;
        ssize_t n;
        size_t from, to = have;

        w.fd = fd;
        w.events = POLLIN;
        w.revents = 0;
        if (poll(&w, 1, 2000) == 0) {
            errno = ETIMEDOUT;
            failed("crc/data: no frame in 2 s");
            goto out;
        }
        n = read(fd, line + have, sizeof(line) - 1 - have);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0) {
            if (n == 0)
                errno = EIO;
            failed("crc/data");
            goto out;
        }
        /* Each read gives one line with its NUL, LINE_LEN bytes. */
        for (from = have; from < have + (size_t)n; from++)
            if (line[from] != 0)
                line[to++] = line[from];
        have = to;
        line[have] = 0;
        while ((nl = strchr(line, '\n')) != NULL) {
            unsigned long frame, crc;

            *nl = 0;
            if (got >= 0 && got < count) {
                if (sscanf(line, "%lx %lx", &frame, &crc) != 2 &&
                    sscanf(line, "XXXXXXXXXX %lx", &crc) != 1) {
                    errno = EINVAL;
                    failed("crc/data: a line not read");
                    goto out;
                }
                crcs[got] = (uint32_t)crc;
            }
            got++;
            have -= (size_t)(nl + 1 - line);
            memmove(line, nl + 1, have + 1);
        }
        if (have == sizeof(line) - 1) {
            errno = E2BIG;
            failed("crc/data: a line past 255 bytes");
            goto out;
        }
    }
    rc = 0;
out:
    close(fd);
    return rc;
}

/* The scaling modes of the arguments, by Linux's names. */
static const struct {
    const char *arg, *name;
} scales[] = {{"aspect", "Full aspect"}, {"full", "Full"}, {"center", "Center"}};

/**
 * Set and read one argument, "WxH" or "WxH:SCALE", and print its line.
 *
 * @return 0 for a stable CRC, 1 for an unstable one, 2 for a failure.
 */
static int one(struct panel *p, const char *dir, const char *arg, int count)
{
    struct buffer b;
    uint32_t crcs[MAX_READS], w = 0, h = 0, i;
    const char *scale = NULL, *colon = strchr(arg, ':');
    int rc = 2;

    why[0] = 0;
    if (colon != NULL) {
        for (i = 0; i < sizeof(scales) / sizeof(scales[0]); i++)
            if (strcmp(colon + 1, scales[i].arg) == 0)
                scale = scales[i].name;
    }
    if (sscanf(arg, "%ux%u", &w, &h) != 2 || w == 0 || h == 0 || w > p->mode.hdisplay ||
        h > p->mode.vdisplay || (colon != NULL && scale == NULL)) {
        printf("crcmodes: mode %s failed: not WxH[:aspect|full|center] within %ux%u\n", arg,
               p->mode.hdisplay, p->mode.vdisplay);
        return 2;
    }
    if (make(p, &b, w, h) == 0 && (scale == NULL || set_scaling(p, scale) == 0) &&
        set_mode(p, w, h, b.fb) == 0 && read_crcs(p, dir, crcs, count) == 0) {
        for (i = 1; i < (uint32_t)count && crcs[i] == crcs[0]; i++)
            ;
        printf("crcmodes: mode %ux%u %s", w, h, colon ? colon + 1 : "-");
        if (i == (uint32_t)count) {
            printf(" crc 0x%08x frames %d\n", crcs[0], count);
            rc = 0;
        } else {
            printf(" unstable:");
            for (i = 0; i < (uint32_t)count; i++)
                printf(" 0x%08x", crcs[i]);
            printf("\n");
            rc = 1;
        }
    } else {
        printf("crcmodes: mode %ux%u %s failed: %s\n", w, h, colon ? colon + 1 : "-", why);
    }
    fflush(stdout);
    unmake(p, &b);
    return rc;
}

int main(int argc, char **argv)
{
    const char *card = "/dev/dri/card0", *dir = "/sys/kernel/debug/dri/0";
    struct panel p;
    int count = 8, opt, i, result[3] = {0, 0, 0};

    while ((opt = getopt(argc, argv, "D:d:n:")) != -1) {
        switch (opt) {
        case 'D':
            card = optarg;
            break;
        case 'd':
            dir = optarg;
            break;
        case 'n':
            count = atoi(optarg);
            break;
        default:
            count = 0;
            break;
        }
    }
    if (optind >= argc || count < 1 || count > MAX_READS) {
        fprintf(stderr, "usage: crcmodes [-D CARD] [-d DIR] [-n COUNT] WxH[:SCALE]...\n");
        return 2;
    }
    memset(&p, 0, sizeof(p));
    p.fd = open(card, O_RDWR | O_CLOEXEC);
    if (p.fd < 0 || drm(p.fd, IOCTL_SET_MASTER, NULL) != 0 || find_panel(&p) != 0) {
        if (why[0] == 0)
            failed(card);
        fprintf(stderr, "crcmodes: %s\n", why);
        return 1;
    }
    printf("crcmodes: panel %ux%u@%u %u kHz on CRTC %u (index %u), scaling mode %s\n",
           p.mode.hdisplay, p.mode.vdisplay, p.mode.vrefresh, p.mode.clock, p.crtc, p.crtc_index,
           p.scaling_prop ? "found" : "absent");
    for (i = optind; i < argc; i++)
        result[one(&p, dir, argv[i], count)]++;
    if (p.scaling_prop != 0) {
        struct mode_connector_set_property s;

        memset(&s, 0, sizeof(s));
        s.connector_id = p.connector;
        s.prop_id = p.scaling_prop;
        s.value = p.scaling_was;
        if (drm(p.fd, IOCTL_MODE_SETPROPERTY, &s) != 0)
            fprintf(stderr, "crcmodes: the scaling mode not put back: %s\n", strerror(errno));
    }
    close(p.fd);
    printf("crcmodes: result: %d modes: stable %d unstable %d failed %d\n", argc - optind,
           result[0], result[1], result[2]);
    return result[1] + result[2] != 0;
}
