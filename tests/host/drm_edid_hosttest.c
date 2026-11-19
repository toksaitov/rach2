/*
 * drm_edid_hosttest.c: a sink's EDID through rach2/sys/drm/drm_edid.c and
 * the probe of drm_mode.c, against the mode lists Linux 6.12 made of the
 * same EDIDs: QEMU's for virtio-gpu and std-vga, and the Lenovo panel's.
 * drm_edid64_hosttest.c runs the same rows with x86-64's structures.
 *
 * host-deps: rach2/sys/lib/kstring.c rach2/sys/lib/kctypes.c
 * host-deps: tests/host/compat/resv_host.c
 * host-deps: tests/host/compat/mutex_none.c
 * host-deps: tests/host/compat/spinlock_host.c
 *
 * A fake device has one CRTC, its primary plane, an encoder and a
 * connector whose get_modes gives the EDID under test to
 * drm_connector_update_edid; a row probes the connector as GETCONNECTOR
 * does and compares its modes, in order, and its EDID blob and size with
 * Linux's. The rows: virtio-gpu's 26 modes; std-vga's 15, which a CRTC
 * that refuses a mode needing more than half of 16 MiB of video memory
 * keeps, as Linux's drm_vram_helper_mode_valid does for bochs; the
 * Lenovo's one; a base block with a bad checksum, which leaves no EDID and
 * the DMT modes up to 1024x768; a CEA extension with a bad checksum, kept;
 * an extension of another kind with a bad checksum, dropped with the base
 * block's count and checksum mended; a header with two bytes swapped,
 * mended; a new blob for each probe; a disconnected sink, which clears
 * the EDID and the modes; and the ELD Linux's drm_edid_to_eld builds: of
 * IGT's HDMI audio EDID byte for byte, of an HDMI block with latencies and
 * AI on a DisplayPort connector with its audio delay, and none without an
 * EDID.
 *
 * Returns 0 when every row passes, 1 otherwise.
 */

#include "drm_kms_model.c"
#include "igt_audio_edid.h"

extern int memcmp(const void *a, const void *b, unsigned long n);

/* QEMU 10.2.1's EDID for -device virtio-gpu-pci,edid=on,xres=1280,yres=800,
 * which std-vga gives as well, as Linux 6.12 read it from
 * /sys/class/drm/card0-Virtual-1/edid: a base block and a CEA extension. */
static const uint8_t qemu_edid[256] = {
    0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00, 0x49, 0x14, 0x34, 0x12, 0x00, 0x00, 0x00, 0x00,
    0x2a, 0x18, 0x01, 0x04, 0xa5, 0x20, 0x14, 0x78, 0x06, 0xee, 0x91, 0xa3, 0x54, 0x4c, 0x99, 0x26,
    0x0f, 0x50, 0x54, 0x21, 0x08, 0x00, 0xe1, 0xc0, 0xd1, 0xc0, 0xd1, 0x00, 0xa9, 0x40, 0xb3, 0x00,
    0x95, 0x00, 0x81, 0x80, 0x81, 0x40, 0xea, 0x29, 0x00, 0xc0, 0x51, 0x20, 0x1c, 0x30, 0x40, 0x26,
    0x44, 0x40, 0x45, 0xcb, 0x10, 0x00, 0x00, 0x18, 0x00, 0x00, 0x00, 0xf7, 0x00, 0x0a, 0x00, 0x40,
    0x82, 0x00, 0x28, 0x20, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xfd, 0x00, 0x32,
    0x7d, 0x1e, 0xa0, 0xff, 0x01, 0x0a, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x00, 0x00, 0x00, 0xfc,
    0x00, 0x51, 0x45, 0x4d, 0x55, 0x20, 0x4d, 0x6f, 0x6e, 0x69, 0x74, 0x6f, 0x72, 0x0a, 0x01, 0x3a,
    0x02, 0x03, 0x0b, 0x00, 0x46, 0x7d, 0x65, 0x60, 0x59, 0x1f, 0x61, 0x00, 0x00, 0x00, 0x10, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x10, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x2f,
};

/* The Lenovo's eDP panel, an AUO B156XTN04.0, as Linux's modetest read it
 * (tests/fixtures/lenovo-gpu/modetest-connectors.out). */
static const uint8_t lenovo_edid[128] = {
    0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00, 0x06, 0xaf, 0xec, 0x40, 0x00, 0x00, 0x00, 0x00,
    0x29, 0x17, 0x01, 0x04, 0x95, 0x22, 0x13, 0x78, 0x02, 0xbb, 0xf5, 0x94, 0x55, 0x54, 0x90, 0x27,
    0x23, 0x50, 0x54, 0x00, 0x00, 0x00, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01,
    0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0xce, 0x1d, 0x56, 0xc0, 0x50, 0x00, 0x30, 0x30, 0x08, 0x0a,
    0x31, 0x00, 0x58, 0xc1, 0x10, 0x00, 0x00, 0x18, 0x00, 0x00, 0x00, 0x0f, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x20, 0x00, 0x00, 0x00, 0xfe, 0x00, 0x41,
    0x55, 0x4f, 0x0a, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x00, 0x00, 0x00, 0xfe,
    0x00, 0x42, 0x31, 0x35, 0x36, 0x58, 0x54, 0x4e, 0x30, 0x34, 0x2e, 0x30, 0x20, 0x0a, 0x00, 0xee,
};

/* A mode of Linux's list: its timings, clock, sync flags and whether it is
 * preferred. Every one is also DRM_MODE_TYPE_DRIVER. */
struct want {
    uint16_t h[4], v[4];
    uint32_t clock, flags;
    int preferred;
};

/* Linux 6.12's modes of qemu_edid on virtio-gpu, from modetest -c. */
static const struct want qemu_modes[] = {
    {{1280, 1600, 1638, 1728}, {800, 804, 808, 828}, 107300, NH | NV, 1},
    {{5120, 6216, 6304, 6600}, {2160, 2168, 2178, 2250}, 742500, PH | PV, 0},
    {{4096, 5064, 5152, 5280}, {2160, 2168, 2178, 2250}, 594000, PH | PV, 0},
    {{3840, 4016, 4104, 4400}, {2160, 2168, 2178, 2250}, 594000, PH | PV, 0},
    {{3840, 4016, 4104, 4400}, {2160, 2168, 2178, 2250}, 593407, PH | PV, 0},
    {{3840, 4896, 4984, 5280}, {2160, 2168, 2178, 2250}, 594000, PH | PV, 0},
    {{1920, 2048, 2256, 2600}, {1440, 1441, 1444, 1500}, 234000, NH | PV, 0},
    {{2560, 3108, 3152, 3300}, {1080, 1084, 1089, 1125}, 185625, PH | PV, 0},
    {{1856, 1952, 2176, 2528}, {1392, 1393, 1396, 1439}, 218250, NH | PV, 0},
    {{1792, 1920, 2120, 2448}, {1344, 1345, 1348, 1394}, 204750, NH | PV, 0},
    {{2048, 2074, 2154, 2250}, {1152, 1153, 1156, 1200}, 162000, PH | PV, 0},
    {{1920, 2056, 2256, 2592}, {1200, 1203, 1209, 1245}, 193250, NH | PV, 0},
    {{1920, 2008, 2052, 2200}, {1080, 1084, 1089, 1125}, 148500, NH | NV, 0},
    {{1920, 2448, 2492, 2640}, {1080, 1084, 1089, 1125}, 148500, PH | PV, 0},
    {{1600, 1664, 1856, 2160}, {1200, 1201, 1204, 1250}, 162000, PH | PV, 0},
    {{1680, 1784, 1960, 2240}, {1050, 1053, 1059, 1089}, 146250, NH | PV, 0},
    {{1400, 1488, 1632, 1864}, {1050, 1053, 1057, 1089}, 121750, NH | PV, 0},
    {{1280, 1328, 1440, 1688}, {1024, 1025, 1028, 1066}, 108000, PH | PV, 0},
    {{1440, 1520, 1672, 1904}, {900, 903, 909, 934}, 106500, NH | PV, 0},
    {{1280, 1376, 1488, 1800}, {960, 961, 964, 1000}, 108000, PH | PV, 0},
    {{1360, 1424, 1536, 1792}, {768, 771, 777, 795}, 85500, PH | PV, 0},
    {{1280, 1344, 1472, 1664}, {768, 771, 778, 798}, 79500, NH | PV, 0},
    {{1024, 1048, 1184, 1344}, {768, 771, 777, 806}, 65000, NH | NV, 0},
    {{800, 840, 968, 1056}, {600, 601, 605, 628}, 40000, PH | PV, 0},
    {{640, 656, 752, 800}, {480, 490, 492, 525}, 25200, NH | NV, 0},
    {{640, 656, 752, 800}, {480, 490, 492, 525}, 25175, NH | NV, 0},
};

/* Linux 6.12's mode of lenovo_edid on i915. */
static const struct want lenovo_modes[] = {
    {{1366, 1374, 1384, 1558}, {768, 771, 772, 816}, 76300, NH | NV, 1},
};

static struct drm_device dev;
static struct drm_plane primary;
static struct drm_crtc crtc;
static struct drm_encoder encoder;
static struct drm_connector conn;
static const uint8_t *sink_edid;
static uint32_t sink_len;
static int sink_status = DRM_CONNECTOR_CONNECTED, vram_limit;

/** The sink's state. */
static int sink_detect(struct drm_connector *connector, int force_probe)
{
    (void)connector;
    (void)force_probe;
    return sink_status;
}

/** The sink's EDID and its modes, as a driver's get_modes gives them. */
static int sink_get_modes(struct drm_connector *connector)
{
    return drm_connector_update_edid(connector, sink_edid, sink_len);
}

/** With `vram_limit`, refuse a mode whose frame of 4-byte pixels needs more
 * pages than half of 16 MiB, Linux's drm_vram_helper_mode_valid. */
static int32_t vram_mode_valid(struct drm_crtc *c, const struct drm_mode_modeinfo *mode)
{
    uint32_t pages = ((uint32_t)mode->hdisplay * mode->vdisplay * 4u + 4095u) / 4096u;

    (void)c;
    return vram_limit && pages > (16u << 20) / 2u / 4096u ? -EINVAL : 0;
}

static const struct drm_connector_funcs sink_funcs = {sink_detect, sink_get_modes, NULL, NULL};
static const struct drm_crtc_funcs crtc_funcs = {NULL, NULL, NULL, NULL, NULL, vram_mode_valid,
                                                 NULL, NULL, NULL, NULL, NULL, NULL};
static const uint32_t xrgb[] = {DRM_FORMAT_XRGB8888};

/** Make the fake device with a connector of `type`. */
static void device_init(uint32_t type)
{
    static struct drm_driver driver;

    k_memset(&dev, 0, sizeof(dev));
    k_memset(&primary, 0, sizeof(primary));
    k_memset(&crtc, 0, sizeof(crtc));
    k_memset(&encoder, 0, sizeof(encoder));
    k_memset(&conn, 0, sizeof(conn));
    driver.ident.name = "fake";
    dev.driver = &driver;
    dev.features = DRIVER_MODESET | DRIVER_ATOMIC;
    CHECK(drm_mode_config_init(&dev) == 0, "the mode configuration is made");
    CHECK(drm_universal_plane_init(&dev, &primary, 1, NULL, xrgb, 1, NULL,
                                   DRM_PLANE_TYPE_PRIMARY) == 0,
          "the primary plane is made");
    CHECK(drm_crtc_init_with_planes(&dev, &crtc, &primary, NULL, &crtc_funcs) == 0,
          "the CRTC is made");
    CHECK(drm_encoder_init(&dev, &encoder, DRM_MODE_ENCODER_VIRTUAL) == 0, "the encoder is made");
    encoder.possible_crtcs = 1;
    CHECK(drm_connector_init(&dev, &conn, &sink_funcs, type) == 0, "the connector is made");
    if (type == DRM_MODE_CONNECTOR_VIRTUAL) {
        drm_connector_attach_edid_property(&conn);
    }
    CHECK(drm_connector_attach_encoder(&conn, &encoder) == 0, "the encoder is attached");
}

/** Free the fake device. */
static void device_fini(void)
{
    drm_mode_config_cleanup(&dev);
}

/** Probe the connector with `edid` of `len` bytes; the number of modes. */
static int probe(const uint8_t *edid, uint32_t len)
{
    sink_edid = edid;
    sink_len = len;
    return drm_helper_probe_single_connector_modes(&conn, 8192, 8192);
}

/** The EDID property's value on the connector. */
static uint64_t edid_prop(void)
{
    uint64_t v = ~(uint64_t)0;

    CHECK(drm_object_property_get_value(&conn.base, dev.mode_config.edid_property, &v) == 0,
          "the connector has the EDID property");
    return v;
}

/** Whether `mi` is `w`, named as Linux names it. The CEA modes' picture
 * aspect is in their flags, which modetest does not print. */
static int mode_is(const struct drm_mode_modeinfo *mi, const struct want *w)
{
    uint32_t type = DRM_MODE_TYPE_DRIVER | (w->preferred ? DRM_MODE_TYPE_PREFERRED : 0);
    struct drm_mode_modeinfo named = *mi;

    drm_mode_set_name(&named);
    return mi->hdisplay == w->h[0] && mi->hsync_start == w->h[1] && mi->hsync_end == w->h[2] &&
           mi->htotal == w->h[3] && mi->vdisplay == w->v[0] && mi->vsync_start == w->v[1] &&
           mi->vsync_end == w->v[2] && mi->vtotal == w->v[3] && mi->clock == w->clock &&
           (mi->flags & ~(uint32_t)DRM_MODE_FLAG_PIC_AR_MASK) == w->flags && mi->type == type &&
           !strcmp(named.name, mi->name);
}

/** Whether the connector's modes are those of `want`'s `n` that `keep`
 * (NULL: all) allows, in order. */
static int modes_are(const struct want *want, int n, int (*keep)(const struct want *))
{
    const struct drm_display_mode *m = conn.modes;
    int i, ok = 1;

    for (i = 0; i < n; i++) {
        if (keep && !keep(&want[i])) {
            continue;
        }
        if (!m) {
            printf("%s: mode %d: missing\n", test_name, i);
            return 0;
        }
        if (!mode_is(&m->info, &want[i])) {
            printf("%s: mode %d: %ux%u %u flags %#x type %#x, want %ux%u %u\n", test_name, i,
                   m->info.hdisplay, m->info.vdisplay, m->info.clock, m->info.flags, m->info.type,
                   want[i].h[0], want[i].v[0], want[i].clock);
            ok = 0;
        }
        m = m->next;
    }
    if (m) {
        printf("%s: a mode past the list: %ux%u\n", test_name, m->info.hdisplay, m->info.vdisplay);
        ok = 0;
    }
    return ok;
}

/** Whether a mode of Linux's virtio-gpu list fits std-vga's video memory. */
static int fits_vram(const struct want *w)
{
    return ((uint32_t)w->h[0] * w->v[0] * 4u + 4095u) / 4096u <= 2048u;
}

/** Whether the connector's EDID blob holds `len` bytes equal to `edid`,
 * and the EDID property names it. */
static int blob_is(const uint8_t *edid, uint32_t len)
{
    const uint8_t *d;
    uint32_t i;

    if (!conn.edid || conn.edid->length != len || edid_prop() != conn.edid->base.id) {
        return 0;
    }
    d = (const uint8_t *)conn.edid->data;
    for (i = 0; i < len && d[i] == edid[i]; i++) {
    }
    return i == len;
}

/** virtio-gpu: Linux's 26 modes, the EDID blob whole and 320x200 mm. */
static void row_virtio(void)
{
    device_init(DRM_MODE_CONNECTOR_VIRTUAL);
    CHECK(probe(qemu_edid, sizeof(qemu_edid)) == 26, "virtio-gpu: 26 modes");
    CHECK(modes_are(qemu_modes, NELEM(qemu_modes), NULL), "virtio-gpu: Linux's modes in order");
    CHECK(blob_is(qemu_edid, sizeof(qemu_edid)), "virtio-gpu: the EDID blob is the sink's");
    CHECK(conn.mm_width == 320 && conn.mm_height == 200, "virtio-gpu: 320x200 mm");
    CHECK(conn.status == DRM_CONNECTOR_CONNECTED, "virtio-gpu: connected");
    device_fini();
}

/** std-vga: the same EDID, less what 8 MiB of frame cannot hold: 15. */
static void row_std_vga(void)
{
    device_init(DRM_MODE_CONNECTOR_VIRTUAL);
    vram_limit = 1;
    CHECK(probe(qemu_edid, sizeof(qemu_edid)) == 15, "std-vga: 15 modes");
    CHECK(modes_are(qemu_modes, NELEM(qemu_modes), fits_vram), "std-vga: Linux's modes in order");
    vram_limit = 0;
    device_fini();
}

/** The Lenovo's panel: one preferred mode, 340x190 mm. */
static void row_lenovo(void)
{
    device_init(DRM_MODE_CONNECTOR_eDP);
    CHECK(probe(lenovo_edid, sizeof(lenovo_edid)) == 1, "Lenovo: one mode");
    CHECK(modes_are(lenovo_modes, NELEM(lenovo_modes), NULL), "Lenovo: Linux's mode");
    CHECK(blob_is(lenovo_edid, sizeof(lenovo_edid)), "Lenovo: the EDID blob is the panel's");
    CHECK(conn.mm_width == 340 && conn.mm_height == 190, "Lenovo: 340x190 mm");
    device_fini();
}

/** A base block whose checksum fails: no EDID, no size, and Linux's DMT
 * modes up to 1024x768 at 61 Hz, largest first, none preferred. */
static void row_bad_base(void)
{
    static const struct want dmt[] = {
        {{1024, 1048, 1184, 1344}, {768, 771, 777, 806}, 65000, NH | NV, 0},
        {{800, 840, 968, 1056}, {600, 601, 605, 628}, 40000, PH | PV, 0},
        {{800, 824, 896, 1024}, {600, 601, 603, 625}, 36000, PH | PV, 0},
        {{848, 864, 976, 1088}, {480, 486, 494, 517}, 33750, PH | PV, 0},
        {{640, 656, 752, 800}, {480, 490, 492, 525}, 25175, NH | NV, 0},
    };
    uint8_t edid[256];

    k_memcpy(edid, qemu_edid, sizeof(edid));
    edid[0x7f] ^= 1;
    device_init(DRM_MODE_CONNECTOR_VIRTUAL);
    CHECK(probe(edid, sizeof(edid)) == 5, "bad base block: five modes");
    CHECK(modes_are(dmt, NELEM(dmt), NULL), "bad base block: the DMT modes to 1024x768");
    CHECK(!conn.edid && edid_prop() == 0, "bad base block: no EDID");
    CHECK(conn.mm_width == 0 && conn.mm_height == 0, "bad base block: no size");
    device_fini();
}

/** A CEA extension whose checksum fails is kept, as Linux keeps one. */
static void row_bad_cea(void)
{
    uint8_t edid[256];

    k_memcpy(edid, qemu_edid, sizeof(edid));
    edid[0xff] ^= 1;
    device_init(DRM_MODE_CONNECTOR_VIRTUAL);
    CHECK(probe(edid, sizeof(edid)) == 26, "bad CEA checksum: 26 modes");
    CHECK(modes_are(qemu_modes, NELEM(qemu_modes), NULL), "bad CEA checksum: the same modes");
    CHECK(blob_is(edid, sizeof(edid)), "bad CEA checksum: the blob as read");
    device_fini();
}

/** Whether a mode of the QEMU list comes from the base block: all but the
 * CEA extension's VICs (5120x2160, 4096x2160, the three 3840x2160,
 * 2560x1080, the 50 Hz 1920x1080 and the 25.2 MHz 640x480). */
static int base_mode(const struct want *w)
{
    return w->h[0] < 2560 && w->h[3] != 2640 && w->clock != 25200;
}

/** An extension of another kind whose checksum fails is dropped, and the
 * base block's count and checksum are mended in the blob. */
static void row_bad_other(void)
{
    uint8_t edid[256], want[128];
    uint32_t i, sum = 0;

    k_memcpy(edid, qemu_edid, sizeof(edid));
    edid[128] = 0x70;
    k_memcpy(want, qemu_edid, sizeof(want));
    want[0x7e] = 0;
    for (i = 0; i < 127; i++) {
        sum += want[i];
    }
    want[0x7f] = (uint8_t)(256u - sum % 256u);
    device_init(DRM_MODE_CONNECTOR_VIRTUAL);
    CHECK(probe(edid, sizeof(edid)) == 18, "bad other extension: 18 modes");
    CHECK(modes_are(qemu_modes, NELEM(qemu_modes), base_mode),
          "bad other extension: the base block's modes");
    CHECK(blob_is(want, sizeof(want)), "bad other extension: one block, count and sum mended");
    device_fini();
}

/** A header with bytes 1 and 7 swapped, whose sum is unchanged, is
 * mended, as Linux mends a header six of whose eight bytes are right. */
static void row_header(void)
{
    uint8_t edid[256];

    k_memcpy(edid, qemu_edid, sizeof(edid));
    edid[1] = 0x00;
    edid[7] = 0xff;
    device_init(DRM_MODE_CONNECTOR_VIRTUAL);
    CHECK(probe(edid, sizeof(edid)) == 26, "mended header: 26 modes");
    CHECK(blob_is(qemu_edid, sizeof(qemu_edid)), "mended header: the blob mended");
    device_fini();
}

/** Each probe makes a new blob before the old one goes, so the property's
 * value moves between two IDs, as Linux's does. */
static void row_new_blob(void)
{
    uint64_t first, second, third;

    device_init(DRM_MODE_CONNECTOR_VIRTUAL);
    probe(qemu_edid, sizeof(qemu_edid));
    first = edid_prop();
    probe(qemu_edid, sizeof(qemu_edid));
    second = edid_prop();
    probe(qemu_edid, sizeof(qemu_edid));
    third = edid_prop();
    CHECK(first != 0 && second != 0 && first != second && third == first,
          "each probe a new blob, the old ID free again");
    device_fini();
}

/** A sink unplugged: no EDID, no size and no modes. */
static void row_disconnected(void)
{
    device_init(DRM_MODE_CONNECTOR_VIRTUAL);
    probe(qemu_edid, sizeof(qemu_edid));
    sink_status = DRM_CONNECTOR_DISCONNECTED;
    CHECK(probe(qemu_edid, sizeof(qemu_edid)) == 0, "disconnected: no modes");
    CHECK(!conn.modes && !conn.edid && edid_prop() == 0, "disconnected: no EDID");
    CHECK(conn.mm_width == 0, "disconnected: no size");
    sink_status = DRM_CONNECTOR_CONNECTED;
    device_fini();
}

/** A GTF curve whose blanking duty cycle is 100%, from an EDID's secondary
 * curve with K 0 and 2J 200, makes no mode instead of dividing by zero;
 * the default curve still makes 1024x768. */
static void row_gtf_full_blank(void)
{
    struct drm_mode_modeinfo m;

    CHECK(gtf_mode_complex(1024, 768, 60, 0, 0, 0, 0, 0, 200, &m) == 0,
          "GTF at a 100% duty cycle: no mode");
    CHECK(gtf_mode(1024, 768, 60, &m) == 1 && m.hdisplay == 1024 && m.htotal == 1344,
          "GTF's default curve: 1024x768, 1344 pixels a line");
}

/** Mend the checksum of the EDID block at `b`. */
static void sum_block(uint8_t *b)
{
    uint8_t sum = 0;
    int i;

    for (i = 0; i < 127; i++) {
        sum = (uint8_t)(sum + b[i]);
    }
    b[127] = (uint8_t)(0u - sum);
}

/** IGT's HDMI audio EDID through an HDMI connector gives Linux's ELD byte
 * for byte, with no audio delay, and a sink unplugged clears it. */
static void row_eld_igt(void)
{
    struct drm_mode_modeinfo m;
    int i, rest = 0;

    device_init(DRM_MODE_CONNECTOR_HDMIA);
    probe(igt_audio_edid, sizeof(igt_audio_edid));
    CHECK(memcmp(conn.eld, igt_audio_eld, sizeof(igt_audio_eld)) == 0,
          "ELD of IGT's audio EDID: Linux's bytes");
    for (i = (int)sizeof(igt_audio_eld); i < DRM_ELD_MAX; i++) {
        rest |= conn.eld[i];
    }
    CHECK(rest == 0, "ELD of IGT's audio EDID: zero past its 28 bytes");
    CHECK(drm_eld_size(conn.eld) == 28, "ELD of IGT's audio EDID: 28 bytes");
    k_memset(&m, 0, sizeof(m));
    CHECK(drm_av_sync_delay(&conn, &m) == 0, "ELD of IGT's audio EDID: no latencies, no delay");
    sink_status = DRM_CONNECTOR_DISCONNECTED;
    probe(igt_audio_edid, sizeof(igt_audio_edid));
    CHECK(conn.eld[0] == 0 && conn.eld[DRM_ELD_BASELINE_ELD_LEN] == 0,
          "ELD of an unplugged sink: cleared");
    sink_status = DRM_CONNECTOR_CONNECTED;
    device_fini();
}

/** An HDMI block with Supports_AI and both latency pairs, on a DisplayPort
 * connector: the ELD's AI bit and DisplayPort type, and the delays Linux's
 * drm_av_sync_delay gives, 40 - 20 ms progressive and 60 - 20 interlaced. */
static void row_eld_latency(void)
{
    static const uint8_t data[] = {0x23, 0x09, 0x07, 0x07, 0x6c, 0x03, 0x0c, 0x00, 0x10,
                                   0x00, 0xb8, 0x2d, 0xc0, 21,   11,   31,   11};
    uint8_t edid[256];
    struct drm_mode_modeinfo m;

    k_memcpy(edid, igt_audio_edid, sizeof(edid));
    k_memset(edid + 128 + 4, 0, 123);
    k_memcpy(edid + 128 + 4, data, sizeof(data));
    edid[128 + 2] = (uint8_t)(4u + sizeof(data));
    sum_block(edid + 128);
    device_init(DRM_MODE_CONNECTOR_DisplayPort);
    probe(edid, sizeof(edid));
    CHECK(conn.eld[DRM_ELD_SAD_COUNT_CONN_TYPE] ==
              (1u << DRM_ELD_SAD_COUNT_SHIFT | DRM_ELD_CONN_TYPE_DP | DRM_ELD_SUPPORTS_AI),
          "ELD with latencies: one SAD, DisplayPort, AI");
    CHECK(conn.eld[DRM_ELD_SPEAKER] == 0, "ELD with no speaker block: no allocation");
    CHECK(conn.latency_present[0] && conn.latency_present[1] && conn.video_latency[1] == 31 &&
              conn.audio_latency[1] == 11,
          "ELD with latencies: both pairs kept");
    k_memset(&m, 0, sizeof(m));
    CHECK(drm_av_sync_delay(&conn, &m) == 20, "audio delay of a progressive mode: 20 ms");
    m.flags = DRM_MODE_FLAG_INTERLACE;
    CHECK(drm_av_sync_delay(&conn, &m) == 40, "audio delay of an interlaced mode: 40 ms");
    device_fini();
}

/** Run the rows. */
int main(void)
{
    int base;

    model_start("drm-edid-test");
    base = allocations;
    row_virtio();
    row_std_vga();
    row_lenovo();
    row_bad_base();
    row_bad_cea();
    row_bad_other();
    row_header();
    row_new_blob();
    row_disconnected();
    row_gtf_full_blank();
    row_eld_igt();
    row_eld_latency();
    CHECK(allocations == base, "every allocation freed");
    if (failures) {
        return 1;
    }
    printf("drm-edid-test: ok\n");
    return 0;
}
