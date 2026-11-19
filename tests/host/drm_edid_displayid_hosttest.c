/*
 * drm_edid_displayid_hosttest.c: the CTA data blocks a DisplayID extension
 * carries, which Linux 6.12's cea_db_iter walks after the CTA extensions',
 * through drm_edid.c's ELD and audio detection, on the fake device of
 * drm_edid_hosttest.c.
 *
 * host-deps: rach2/sys/lib/kstring.c rach2/sys/lib/kctypes.c
 * host-deps: tests/host/compat/resv_host.c
 * host-deps: tests/host/compat/mutex_none.c
 * host-deps: tests/host/compat/spinlock_host.c
 *
 * The rows: IGT's audio EDID with its audio and speaker blocks moved from
 * the CTA extension into a DisplayID extension's CTA block gives the ELD
 * the CTA extension gave, byte for byte, and audio; the same blocks in a
 * DisplayID section whose checksum fails give no SAD; and an EDID whose
 * only extension is that DisplayID one gives the ELD's SAD but no audio,
 * as Linux's _drm_detect_monitor_audio finds none without a CTA extension.
 *
 * Returns 0 when every row passes, 1 otherwise.
 */

#define main drm_edid_rows
#include "drm_edid_hosttest.c"
#undef main

/* IGT's CTA extension less its audio and speaker blocks: revision 3, basic
 * audio, and HDMI's VSDB at 225 MHz. */
static const uint8_t cta_vsdb[13] = {0x02, 0x03, 0x0d, 0x40, 0x68, 0x03, 0x0c,
                                     0x00, 0x10, 0x00, 0x38, 0x2d, 0x00};

/* A DisplayID 1.2 section of one CTA block holding IGT's audio block, one
 * LPCM SAD, and its speaker allocation, FLC/FRC; the section's checksum
 * follows at byte 16. */
static const uint8_t displayid_cta[16] = {0x70, 0x12, 11,   0x03, 0x00, 0x81, 0x00, 8,
                                          0x23, 0x09, 0x07, 0x07, 0x83, 0x20, 0x00, 0x00};

/** Lay out in `edid` IGT's base block with `exts` extensions, the CTA one
 * when `cta` and the DisplayID one after it, its section's checksum broken
 * when `bad`; the bytes laid out. */
static uint32_t layout(uint8_t *edid, int cta, int bad)
{
    uint8_t *b, sum = 0;
    uint32_t n = 1, i;

    k_memset(edid, 0, 3 * 128);
    k_memcpy(edid, igt_audio_edid, 128);
    if (cta) {
        k_memcpy(edid + 128 * n++, cta_vsdb, sizeof(cta_vsdb));
    }
    b = edid + 128 * n++;
    k_memcpy(b, displayid_cta, sizeof(displayid_cta));
    for (i = 1; i < 16; i++) {
        sum = (uint8_t)(sum + b[i]);
    }
    b[16] = (uint8_t)(0u - sum + (bad ? 1u : 0u));
    edid[126] = (uint8_t)(n - 1);
    for (i = 0; i < n; i++) {
        sum_block(edid + 128 * i);
    }
    return n * 128;
}

/** The SADs the ELD counts. */
static int eld_sads(void)
{
    return conn.eld[DRM_ELD_SAD_COUNT_CONN_TYPE] >> DRM_ELD_SAD_COUNT_SHIFT;
}

/** The audio and speaker blocks in DisplayID, after a CTA extension: the
 * ELD IGT's CTA extension gives, and audio. */
static void row_displayid_audio(void)
{
    struct drm_edid_hdmi_info info;
    uint8_t edid[3 * 128];
    uint32_t len = layout(edid, 1, 0);

    device_init(DRM_MODE_CONNECTOR_HDMIA);
    probe(edid, len);
    CHECK(memcmp(conn.eld, igt_audio_eld, sizeof(igt_audio_eld)) == 0,
          "displayid: the ELD of IGT's blocks in DisplayID is the CTA extension's");
    drm_edid_hdmi_info(&conn, edid, len, &info);
    CHECK(info.has_audio, "displayid: audio with a CTA extension");
    device_fini();
}

/** A DisplayID section whose checksum fails is not walked. */
static void row_displayid_bad(void)
{
    uint8_t edid[3 * 128];
    uint32_t len = layout(edid, 1, 1);

    device_init(DRM_MODE_CONNECTOR_HDMIA);
    probe(edid, len);
    CHECK(eld_sads() == 0 && conn.eld[DRM_ELD_SPEAKER] == 0,
          "displayid: a section with a bad checksum adds no SAD");
    device_fini();
}

/** DisplayID alone: its SAD in the ELD, but no audio. */
static void row_displayid_alone(void)
{
    struct drm_edid_hdmi_info info;
    uint8_t edid[3 * 128];
    uint32_t len = layout(edid, 0, 0);

    device_init(DRM_MODE_CONNECTOR_HDMIA);
    probe(edid, len);
    CHECK(eld_sads() == 1 && conn.eld[DRM_ELD_SPEAKER] == 0x20,
          "displayid alone: the ELD has its SAD and speakers");
    drm_edid_hdmi_info(&conn, edid, len, &info);
    CHECK(!info.has_audio, "displayid alone: no audio without a CTA extension");
    device_fini();
}

/** Run the rows. */
int main(void)
{
    int base;

    model_start("drm-edid-displayid-test");
    base = allocations;
    row_displayid_audio();
    row_displayid_bad();
    row_displayid_alone();
    CHECK(allocations == base, "every allocation freed");
    if (failures) {
        return 1;
    }
    printf("drm-edid-displayid-test: ok\n");
    return 0;
}
