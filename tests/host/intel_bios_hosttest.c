/**
 * intel_bios_hosttest.c: the Intel graphics driver's OpRegion
 * (intel_opregion.c) and video BIOS table (intel_bios.c), against the
 * Lenovo's own bytes and the outputs and panel Linux made of them.
 *
 * host-deps: rach2/sys/drm/i915/intel_bios.c
 * host-deps: rach2/sys/drm/i915/intel_opregion.c
 * host-deps: rach2/sys/lib/kstring.c rach2/sys/lib/kctypes.c
 * host-deps: tests/host/compat/kheap_host.c
 * host-inputs: tests/fixtures/lenovo-gpu/i915_opregion.bin
 * host-inputs: tests/fixtures/lenovo-gpu/i915_vbt.bin
 *
 * The OpRegion's 8 KiB, read from the fixture, stand at the Lenovo's ASLS:
 * `mmio_map` hands out the test's buffers by physical address and records
 * each map and unmap, so a row sees what was mapped, with which cache
 * bits, and that each window went back. A row that needs another table
 * changes a copy of the Lenovo's bytes. The log keeps its lines since the
 * row's last fresh OpRegion or decode.
 *
 * The rows: the OpRegion found through ASLS, its version, mailboxes and
 * the driver's two writes to them, and the VBT of mailbox 4; no ASLS, a
 * map that fails, a bad signature, a bad VBT, mailbox 4 without mailbox 5,
 * RVDA on version 2.0 and 2.1 and an invalid one, and mailbox 2 on version
 * 3; the Lenovo's VBT decoded to the outputs Linux made, eDP on DDI A,
 * HDMI on DDI B and dual mode on DDI C, and to the panel's index, link
 * depth, power sequence, mode, DRRS and backlight, the sequence giving the
 * timings Linux reported; each block padded to Linux's size; a port A that
 * claims DVI; the panel's index from the table, from the EDID's product
 * id with and without a date, from the fallback, and refused when out of
 * range for either panel; malformed and missing LFP data pointers; a child
 * config too small; tables refused whole; Linux's defaults without a
 * table; and the VBT of an option ROM.
 *
 * Returns 0 when every row passes, 1 otherwise.
 */

#include <stdarg.h>

#include "drm/i915/intel_bios.h"
#include "drm/drm.h"
#include "lib/kerrno.h"
#include "lib/kstring.h"
#include "mm/paging.h"
#include "i915_lenovo.h"

extern int printf(const char *format, ...);
extern int vsnprintf(char *s, unsigned long n, const char *format, va_list ap);
extern void *calloc(unsigned long count, unsigned long size);
extern void free(void *p);
extern int memcmp(const void *a, const void *b, unsigned long n);
extern int strcmp(const char *a, const char *b);
extern char *strstr(const char *s, const char *t);
extern int open(const char *path, int flags, ...);
extern long read(int fd, void *buf, unsigned long n);
extern int close(int fd);

static int fails;

#define CHECK(what, cond)                                \
    do {                                                 \
        if (!(cond)) {                                   \
            printf("intel-bios-test: fail: %s\n", what); \
            fails++;                                     \
        }                                                \
    } while (0)

/* ---- the kernel around the driver ---- */

static char log_text[4096];
static unsigned long log_len;

/** The boot log: the lines since `log_clear`, as much as fits. */
void klog(int level, const char *subsystem, const char *format, ...)
{
    va_list ap;

    (void)level;
    (void)subsystem;
    if (log_len + 2 >= sizeof(log_text)) {
        return;
    }
    va_start(ap, format);
    vsnprintf(log_text + log_len, sizeof(log_text) - log_len - 1, format, ap);
    va_end(ap);
    while (log_text[log_len]) {
        log_len++;
    }
    log_text[log_len++] = '\n';
    log_text[log_len] = '\0';
}

/** Forget the lines logged so far. */
static void log_clear(void)
{
    log_len = 0;
    log_text[0] = '\0';
}

/** Whether a line since `log_clear` holds `text`. */
static int logged(const char *text)
{
    return strstr(log_text, text) != 0;
}

/* Physical memory the test stands at an address, and the maps made of it. */
#define REGIONS 4
static struct {
    uint64_t phys;
    uint32_t size;
    uint8_t *buf;
} regions[REGIONS];
static int maps, unmaps, map_refuse;
static pte_t map_cache;
static uintptr_t unmapped_va[8];
static size_t unmapped_size[8];

/** Empty the firmware's memory and the map counters. */
static void regions_clear(void)
{
    int i;

    for (i = 0; i < REGIONS; i++) {
        regions[i].buf = NULL;
    }
    maps = unmaps = map_refuse = 0;
}

/** Put the test's `buf` of `size` bytes at physical `phys`. */
static void region_add(uint64_t phys, uint8_t *buf, uint32_t size)
{
    int i;

    for (i = 0; i < REGIONS; i++) {
        if (!regions[i].buf) {
            regions[i].phys = phys;
            regions[i].size = size;
            regions[i].buf = buf;
            return;
        }
    }
}

/** The test's buffer that holds [phys, phys + size), or 0. */
uintptr_t mmio_map(uint64_t phys, size_t size, pte_t cache)
{
    int i;

    maps++;
    map_cache = cache;
    if (map_refuse) {
        return 0;
    }
    for (i = 0; i < REGIONS; i++) {
        if (regions[i].buf && phys >= regions[i].phys &&
            phys + size <= regions[i].phys + regions[i].size) {
            return (uintptr_t)(regions[i].buf + (phys - regions[i].phys));
        }
    }
    return 0;
}

/** Record the first eight unmaps. */
void mmio_unmap(uintptr_t va, size_t size)
{
    if (unmaps < 8) {
        unmapped_va[unmaps] = va;
        unmapped_size[unmaps] = size;
    }
    unmaps++;
}

/** DRM's refresh rate of a mode, rounded, as drm_mode.c gives it. */
uint32_t drm_mode_vrefresh(const struct drm_mode_modeinfo *m)
{
    uint64_t den = (uint64_t)m->htotal * m->vtotal;

    return den ? (uint32_t)(((uint64_t)m->clock * 1000 + den / 2) / den) : 0;
}

/** DRM's name of a mode, "WxH". */
void drm_mode_set_name(struct drm_mode_modeinfo *m)
{
    char *p = m->name;
    uint32_t v[2], d, i;
    int k;

    v[0] = m->hdisplay;
    v[1] = m->vdisplay;
    for (k = 0; k < 2; k++) {
        if (k) {
            *p++ = 'x';
        }
        for (d = 1; v[k] / d >= 10; d *= 10) {
        }
        for (i = d; i; i /= 10) {
            *p++ = (char)('0' + v[k] / i % 10);
        }
    }
    *p = '\0';
}

/* ---- the fixtures ---- */

static uint8_t lenovo_opregion[LENOVO_OPREGION_BYTES];
static uint8_t lenovo_vbt[LENOVO_VBT_BYTES];

/** Read `n` bytes of the file `path` into `buf`; 1 when all were read. */
static int load(const char *path, uint8_t *buf, long n)
{
    long got = 0, r;
    int fd = open(path, 0);

    if (fd < 0) {
        return 0;
    }
    while (got < n && (r = read(fd, buf + got, (unsigned long)(n - got))) > 0) {
        got += r;
    }
    close(fd);
    return got == n;
}

/** ASLS as the Lenovo's configuration space gives it. */
static uint32_t lenovo_asls(void)
{
    unsigned i;

    for (i = 0; i < sizeof(lenovo_gpu_config) / sizeof(lenovo_gpu_config[0]); i++) {
        if (lenovo_gpu_config[i].at == 0xfc) {
            return lenovo_gpu_config[i].value;
        }
    }
    return 0;
}

/** The little-endian 16 bits at `p`. */
static uint16_t rd16(const uint8_t *p)
{
    return (uint16_t)(p[0] | p[1] << 8);
}

/** Write `v` little-endian at `p`. */
static void wr16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

/** Write `v` little-endian at `p`. */
static void wr32(uint8_t *p, uint32_t v)
{
    wr16(p, (uint16_t)v);
    wr16(p + 2, (uint16_t)(v >> 16));
}

/** The offset in `vbt` of block `id`'s data, by the test's own walk of the
 * BDB; 0 for none. */
static uint32_t vbt_block(const uint8_t *vbt, uint8_t id)
{
    uint32_t bdb = rd16(vbt + 28) | (uint32_t)rd16(vbt + 30) << 16;
    uint32_t at = bdb + rd16(vbt + bdb + 18), end = bdb + rd16(vbt + bdb + 20);

    while (at + 3 < end) {
        if (vbt[at] == id) {
            return at + 3;
        }
        at += 3 + rd16(vbt + at + 1);
    }
    return 0;
}

/** A copy of the Lenovo's VBT, which the caller frees. */
static uint8_t *vbt_copy(void)
{
    uint8_t *p = (uint8_t *)calloc(1, LENOVO_VBT_BYTES);

    memcpy(p, lenovo_vbt, LENOVO_VBT_BYTES);
    return p;
}

/** Decode `buf` into `vbt` and the first LFP's panel, early (no EDID, no
 * fallback). */
static void decode(struct intel_vbt *vbt, struct intel_vbt_panel *panel, const uint8_t *buf)
{
    log_clear();
    k_memset(vbt, 0, sizeof(*vbt));
    CHECK("decode: init", intel_bios_init(vbt, buf, LENOVO_VBT_BYTES, "test") == 0);
    k_memset(panel, 0, sizeof(*panel));
    panel->panel_type = -1;
    intel_bios_panel_init(vbt, panel, intel_bios_encoder_lookup(vbt, INTEL_PORT_A), NULL, 0);
}

/* ---- the OpRegion ---- */

static uint8_t region[OPREGION_SIZE];

/** A fresh copy of the Lenovo's OpRegion at its ASLS. */
static void region_lenovo(void)
{
    log_clear();
    regions_clear();
    memcpy(region, lenovo_opregion, sizeof(region));
    region_add(lenovo_asls(), region, sizeof(region));
}

/** The Lenovo's OpRegion: its version, mailboxes, CHPD and ARDY written,
 * and mailbox 4 the VBT Linux read. */
static void row_opregion_lenovo(void)
{
    struct intel_opregion op;
    uint32_t asls = lenovo_asls();

    CHECK("opregion: ASLS is the Lenovo's", asls == LENOVO_GPU_ASLS);
    region_lenovo();
    region[0x100 + 0xa8] = 0; /* CHPD clear */
    region[0x300] = 1;        /* ARDY set */
    CHECK("opregion: setup", intel_opregion_setup(&op, asls) == 0);
    CHECK("opregion: one map of 8 KiB, uncached", maps == 1 && map_cache == PTE_NOCACHE);
    CHECK("opregion: base", op.base == region && op.phys == asls);
    CHECK("opregion: version 2.0.0", op.major == 2 && op.minor == 0 && op.revision == 0);
    CHECK("opregion: mailboxes", op.mboxes == LENOVO_OPREGION_MBOXES);
    CHECK("opregion: mailbox 1", op.acpi == region + 0x100);
    CHECK("opregion: mailbox 2 on version 2", op.swsci == region + 0x200);
    CHECK("opregion: mailbox 3", op.asle == region + 0x300);
    CHECK("opregion: mailbox 5", op.asle_ext == region + 0x1c00);
    CHECK("opregion: CHPD set", region[0x1a8] == 1 && region[0x1a9] == 0);
    CHECK("opregion: ARDY not ready", region[0x300] == 0);
    CHECK("opregion: VBT in mailbox 4, up to mailbox 5",
          op.vbt == region + 0x400 && op.vbt_size == 0x1800 && !op.rvda);
    CHECK("opregion: mailbox 4 is the VBT Linux read",
          op.vbt_size == LENOVO_VBT_BYTES && memcmp(op.vbt, lenovo_vbt, LENOVO_VBT_BYTES) == 0);
    CHECK("opregion: logged", logged("version 2.0.0") && logged("VBT in mailbox 4"));
    intel_opregion_fini(&op);
    CHECK("opregion: fini unmaps it",
          unmaps == 1 && unmapped_va[0] == (uintptr_t)region && unmapped_size[0] == OPREGION_SIZE);
    CHECK("opregion: fini empties", op.base == NULL && op.vbt == NULL);
}

/** No ASLS, a refused map, a bad signature and a bad VBT, with mailbox 4's
 * size without mailbox 5 and SWSCI ignored on version 3. */
static void row_opregion_refused(void)
{
    struct intel_opregion op;

    region_lenovo();
    CHECK("opregion: no ASLS", intel_opregion_setup(&op, 0) == -ENODEV && maps == 0);
    map_refuse = 1;
    CHECK("opregion: map refused", intel_opregion_setup(&op, lenovo_asls()) == -ENOMEM);
    CHECK("opregion: map refused leaves it empty", op.base == NULL);

    region_lenovo();
    region[5] ^= 0x20;
    CHECK("opregion: bad signature", intel_opregion_setup(&op, lenovo_asls()) == -EINVAL);
    CHECK("opregion: bad signature unmapped", unmaps == 1 && op.base == NULL);
    CHECK("opregion: bad signature logged", logged("no signature"));

    region_lenovo();
    region[0x400] = 'X';
    CHECK("opregion: bad VBT", intel_opregion_setup(&op, lenovo_asls()) == 0);
    CHECK("opregion: bad VBT found none", op.base == region && op.vbt == NULL);
    CHECK("opregion: bad VBT logged", logged("no valid VBT"));
    intel_opregion_fini(&op);

    region_lenovo();
    region[0x58] &= (uint8_t)~0x10; /* no mailbox 5 */
    CHECK("opregion: no mailbox 5", intel_opregion_setup(&op, lenovo_asls()) == 0);
    CHECK("opregion: mailbox 4 to the end", op.vbt_size == 0x1c00 && op.asle_ext == NULL);
    intel_opregion_fini(&op);

    region_lenovo();
    region[0x17] = 3; /* version 3 */
    CHECK("opregion: version 3", intel_opregion_setup(&op, lenovo_asls()) == 0);
    CHECK("opregion: mailbox 2 ignored on version 3", op.swsci == NULL && op.asle != NULL);
    intel_opregion_fini(&op);
}

/** The VBT at RVDA, an offset from 2.1 and physical on 2.0, and mailbox 4
 * taken when RVDA's is invalid. */
static void row_opregion_rvda(void)
{
    struct intel_opregion op;
    static uint8_t raw[LENOVO_VBT_BYTES];
    uint32_t asls = lenovo_asls();

    /* 2.1: RVDA counts from the OpRegion. */
    region_lenovo();
    memcpy(raw, lenovo_vbt, sizeof(raw));
    region_add((uint64_t)asls + 0x4000, raw, sizeof(raw));
    region[0x16] = 1;
    wr32(region + 0x300 + 0xba, 0x4000);
    wr32(region + 0x300 + 0xc2, 4400);
    CHECK("rvda 2.1: setup", intel_opregion_setup(&op, asls) == 0);
    CHECK("rvda 2.1: the VBT at RVDA", op.vbt == raw && op.vbt_size == 4400 && op.rvda == raw);
    CHECK("rvda 2.1: logged", logged("VBT at RVDA"));
    intel_opregion_fini(&op);
    CHECK("rvda 2.1: both windows unmapped",
          unmaps == 2 && unmapped_va[0] == (uintptr_t)raw && unmapped_size[0] == 4400);

    /* 2.0: RVDA is physical. */
    region_lenovo();
    region_add(0x12340000u, raw, sizeof(raw));
    wr32(region + 0x300 + 0xba, 0x12340000u);
    wr32(region + 0x300 + 0xc2, 4400);
    CHECK("rvda 2.0: setup", intel_opregion_setup(&op, asls) == 0);
    CHECK("rvda 2.0: the VBT at the address", op.vbt == raw && op.rvda == raw);
    intel_opregion_fini(&op);

    /* An invalid VBT at RVDA leaves mailbox 4. */
    region_lenovo();
    raw[0] = 'X';
    region_add(0x12340000u, raw, sizeof(raw));
    wr32(region + 0x300 + 0xba, 0x12340000u);
    wr32(region + 0x300 + 0xc2, 4400);
    CHECK("rvda invalid: setup", intel_opregion_setup(&op, asls) == 0);
    CHECK("rvda invalid: mailbox 4 taken", op.vbt == region + 0x400 && op.rvda == NULL);
    CHECK("rvda invalid: its window unmapped", unmaps == 1 && unmapped_va[0] == (uintptr_t)raw);
    intel_opregion_fini(&op);
}

/* ---- the VBT ---- */

/** The Lenovo's VBT from mailbox 4: its header, general features and
 * definitions, driver features and the three outputs Linux made. */
static void row_vbt_lenovo(void)
{
    struct intel_opregion op;
    struct intel_vbt vbt;
    const struct intel_bios_encoder *a, *b, *c;

    region_lenovo();
    CHECK("vbt: OpRegion", intel_opregion_setup(&op, lenovo_asls()) == 0);
    k_memset(&vbt, 0, sizeof(vbt));
    CHECK("vbt: init", intel_bios_init(&vbt, op.vbt, op.vbt_size, "OpRegion (Mailbox #4)") == 0);
    CHECK("vbt: present", vbt.present && strcmp(vbt.source, "OpRegion (Mailbox #4)") == 0);
    CHECK("vbt: signature", strcmp(vbt.signature, LENOVO_VBT_SIGNATURE) == 0);
    CHECK("vbt: version", vbt.version == LENOVO_VBT_VERSION);
    CHECK("vbt: logged", logged("version 177, 3 outputs"));

    /* The general features: no SSC, at 120 MHz, CRT and no TV; no
     * orientation before version 181. */
    CHECK("vbt: lvds_use_ssc", vbt.lvds_use_ssc == 0 && vbt.lvds_ssc_freq == 120000);
    CHECK("vbt: int_crt_support", vbt.int_crt_support == 1 && vbt.int_tv_support == 0);
    CHECK("vbt: display_clock_mode", vbt.display_clock_mode == 0);
    CHECK("vbt: fdi_rx_polarity_inverted", vbt.fdi_rx_polarity_inverted == 0);
    CHECK("vbt: orientation", vbt.orientation == DRM_MODE_PANEL_ORIENTATION_UNKNOWN);
    /* The general definitions and driver features: VGA DDC on pin 2,
     * children of 33 bytes, and LVDS config 3, which counts as eDP. */
    CHECK("vbt: crt_ddc_pin", vbt.crt_ddc_pin == 2);
    CHECK("vbt: child size", vbt.child_size == 33);
    CHECK("vbt: int_lvds_support", vbt.int_lvds_support == 0);

    CHECK("vbt: three outputs", vbt.nencoders == 3);
    a = intel_bios_encoder_lookup(&vbt, INTEL_PORT_A);
    b = intel_bios_encoder_lookup(&vbt, INTEL_PORT_B);
    c = intel_bios_encoder_lookup(&vbt, INTEL_PORT_C);
    CHECK("vbt: no DDI D or E", !intel_bios_encoder_lookup(&vbt, INTEL_PORT_D) &&
                                    !intel_bios_encoder_lookup(&vbt, INTEL_PORT_E));
    CHECK("vbt: outputs on A, B and C", a && b && c);
    if (a && b && c) {
        /* eDP-1 on DDI A. */
        CHECK("vbt: A is the LFP", a->handle == DEVICE_HANDLE_LFP1 && a->device_type == 0x78c6);
        CHECK("vbt: A is eDP", intel_bios_encoder_supports_edp(a) &&
                                   !intel_bios_encoder_supports_hdmi(a) &&
                                   !intel_bios_encoder_supports_dp_dual_mode(a));
        CHECK("vbt: A on AUX A", a->aux_ch == INTEL_AUX_CH_A);
        /* HDMI-A-1 alone on DDI B. */
        CHECK("vbt: B is HDMI only", intel_bios_encoder_supports_hdmi(b) &&
                                         !intel_bios_encoder_supports_dp(b) &&
                                         !intel_bios_encoder_supports_edp(b));
        CHECK("vbt: B's DDC on GMBUS pin 5 (DPB)", b->ddc_pin == 5);
        CHECK("vbt: B's level shift", b->hdmi_level_shift == 9);
        /* DP-1 and HDMI-A-2 on DDI C. */
        CHECK("vbt: C is DP and HDMI", intel_bios_encoder_supports_dp(c) &&
                                           intel_bios_encoder_supports_hdmi(c) &&
                                           !intel_bios_encoder_supports_edp(c));
        CHECK("vbt: C is dual mode", intel_bios_encoder_supports_dp_dual_mode(c));
        CHECK("vbt: C's DDC on GMBUS pin 4 (DPC) and AUX C",
              c->ddc_pin == 4 && c->aux_ch == INTEL_AUX_CH_C);
        CHECK("vbt: C's level shift", c->hdmi_level_shift == 6);
        CHECK("vbt: no limits before their versions",
              c->hdmi_max_tmds_clock == 0 && c->dp_max_link_rate == 0 &&
                  c->dp_max_lane_count == 0 && c->dp_boost_level == 0 && c->hpd_invert == 0);
    }
    intel_bios_fini(&vbt);
    CHECK("vbt: fini empties", vbt.encoders == NULL && vbt.nencoders == 0 && !vbt.present);
    intel_opregion_fini(&op);
}

/** Linux's delay in milliseconds of a field in 100 us units, the larger of
 * the table's and the firmware register's (intel_pps.c, pps_init_delays). */
static unsigned pps_ms(unsigned vbt, unsigned reg)
{
    return ((vbt > reg ? vbt : reg) + 9) / 10;
}

/** The Lenovo's panel: its type, eDP link, power sequence and the timings
 * Linux reported, its mode, DRRS, PSR and backlight. */
static void row_vbt_panel(void)
{
    struct intel_vbt vbt;
    struct intel_vbt_panel panel;
    const struct intel_bdb_block *edp;
    const struct drm_mode_modeinfo *m = &panel.lfp_mode;
    unsigned i, zero = 1;

    decode(&vbt, &panel, lenovo_vbt);
    CHECK("panel: type from the table", panel.panel_type == 2);
    /* The link: 6 bits a colour, which Linux used ("max bpc: 6"). */
    CHECK("panel: bpp", panel.edp.bpp == LENOVO_EDP_BPP);
    /* Fast link training's settings, which Linux 6.12 no longer uses: 1
     * lane, as Linux trained, at 1.62 Gb/s, levels 0. */
    CHECK("panel: fast link lanes", panel.edp.lanes == LENOVO_EDP_LANES);
    CHECK("panel: fast link rate", panel.edp.rate == 162000);
    CHECK("panel: fast link levels", panel.edp.preemphasis == 0 && panel.edp.vswing == 0);
    /* The power sequence in 100 us units, and the timings Linux reported
     * from it and the firmware's PP_ON_DELAYS 0x07d00000 (T1+T3 2000, T8
     * 0) and PP_OFF_DELAYS 0x01f407d0 (T10 500, T9 2000). */
    CHECK("panel: pps", panel.edp.pps.t1_t3 == 2000 && panel.edp.pps.t8 == 10 &&
                            panel.edp.pps.t9 == 2000 && panel.edp.pps.t10 == 500 &&
                            panel.edp.pps.t11_t12 == 5000);
    CHECK("panel: power up", pps_ms(panel.edp.pps.t1_t3, 0x7d0) == LENOVO_PANEL_POWER_UP_MS);
    CHECK("panel: backlight on", pps_ms(panel.edp.pps.t8, 0) == LENOVO_PANEL_BL_ON_MS);
    CHECK("panel: backlight off", pps_ms(panel.edp.pps.t9, 0x7d0) == LENOVO_PANEL_BL_OFF_MS);
    CHECK("panel: power down", pps_ms(panel.edp.pps.t10, 0x1f4) == LENOVO_PANEL_POWER_DOWN_MS);
    CHECK("panel: power cycle", pps_ms(panel.edp.pps.t11_t12, 0) == LENOVO_PANEL_POWER_CYCLE_MS);
    /* The eDP block ends at byte 204, before edp_vswing_preemph (173+),
     * which reads as zero: a low swing panel, as on Linux. */
    edp = intel_bios_block(&vbt, BDB_EDP);
    CHECK("panel: eDP block padded", edp && edp->raw_size == 204 && edp->size == 848);
    for (i = 204; edp && i < edp->size; i++) {
        zero &= edp->data[i] == 0;
    }
    CHECK("panel: padding zero", zero);
    CHECK("panel: low swing", panel.edp.low_vswing == 1);
    CHECK("panel: no max rate before 244", panel.edp.max_link_rate == 0);
    /* The LFP data's mode, a 1024x768 at 65 MHz that Linux sets aside for
     * the panel's EDID mode, 1366x768. */
    CHECK("panel: mode", panel.has_lfp_mode && m->hdisplay == 1024 && m->hsync_start == 1048 &&
                             m->hsync_end == 1184 && m->htotal == 1344 && m->vdisplay == 768 &&
                             m->vsync_start == 771 && m->vsync_end == 777 && m->vtotal == 806 &&
                             m->clock == 65000);
    CHECK("panel: mode flags", m->flags == (DRM_MODE_FLAG_NHSYNC | DRM_MODE_FLAG_NVSYNC) &&
                                   m->type == DRM_MODE_TYPE_PREFERRED);
    CHECK("panel: mode name and rate", strcmp(m->name, "1024x768") == 0 && m->vrefresh == 60);
    CHECK("panel: LVDS value", panel.bios_lvds_val == 0x300);
    /* Static DRRS, which the driver features leave; Linux uses it only for
     * a panel with a second, slower mode, which this one lacks, so its
     * eDP-1 has no i915_drrs_type file. */
    CHECK("panel: dither", panel.lvds_dither == 1);
    CHECK("panel: drrs", panel.drrs_type == DRRS_TYPE_STATIC);
    CHECK("panel: psr flag", panel.psr_enable == 1 && panel.vrr == 1);
    CHECK("panel: backlight", panel.bl_present && panel.bl_pwm_freq_hz == 200 &&
                                  !panel.bl_active_low_pwm && panel.bl_min_brightness == 5);
    intel_bios_fini(&vbt);
}

/** The blocks' sizes, raw and padded to Linux's structures, and a field
 * past a cut block's end that reads as zero. */
static void row_vbt_blocks(void)
{
    struct intel_vbt vbt;
    struct intel_vbt_panel panel;
    const struct intel_bdb_block *b;
    uint8_t *v = vbt_copy();
    uint32_t gf = vbt_block(v, BDB_GENERAL_FEATURES);

    decode(&vbt, &panel, lenovo_vbt);
    b = intel_bios_block(&vbt, BDB_GENERAL_FEATURES);
    CHECK("blocks: general features of 6 bytes padded to 7", b && b->raw_size == 6 && b->size == 7);
    b = intel_bios_block(&vbt, BDB_LFP_DATA);
    CHECK("blocks: LFP data to the names' end", b && b->raw_size == 1264 && b->size == 1366);
    b = intel_bios_block(&vbt, BDB_LFP_POWER);
    CHECK("blocks: LFP power", b && b->raw_size == 21 && b->size == 136);
    intel_bios_fini(&vbt);

    /* A field past a block's end reads as zero even when the table has
     * bytes there: byte 4 of the general features, past a block cut to 4. */
    wr16(v + gf - 2, 4);
    v[gf + 4] = 0xff;
    k_memset(&vbt, 0, sizeof(vbt));
    CHECK("blocks: cut block", intel_bios_init(&vbt, v, LENOVO_VBT_BYTES, "test") == 0);
    CHECK("blocks: cut block's flags are zero",
          vbt.int_crt_support == 0 && vbt.int_tv_support == 0);
    intel_bios_fini(&vbt);
    free(v);
}

/** A child on DDI A that claims DVI loses it, as Linux clears it. */
static void row_vbt_port_a_dvi(void)
{
    struct intel_vbt vbt;
    struct intel_vbt_panel panel;
    const struct intel_bios_encoder *a;
    uint8_t *v = vbt_copy();
    uint32_t gd = vbt_block(v, BDB_GENERAL_DEFINITIONS);

    wr16(v + gd + 5 + 2, 0x78d6); /* eDP with TMDS */
    decode(&vbt, &panel, v);
    a = intel_bios_encoder_lookup(&vbt, INTEL_PORT_A);
    CHECK("port A: DVI dropped", a && !intel_bios_encoder_supports_dvi(a) &&
                                     (a->device_type & DEVICE_TYPE_NOT_HDMI_OUTPUT) &&
                                     intel_bios_encoder_supports_edp(a));
    CHECK("port A: the kept child agrees", a && rd16(a->child + 2) == a->device_type);
    intel_bios_fini(&vbt);
    free(v);
}

/** The panel type found from the EDID's product id, with and without its
 * date, the fallback to type 0, and a type out of range refused. */
static void row_vbt_panel_type(void)
{
    struct intel_vbt vbt;
    struct intel_vbt_panel panel;
    struct intel_bios_encoder lfp2;
    uint8_t *v = vbt_copy(), edid[128];
    uint32_t lo = vbt_block(v, BDB_LFP_OPTIONS), lp = vbt_block(v, BDB_LFP_DATA_PTRS);
    uint32_t data = vbt_block(v, BDB_LFP_DATA), bdb = 0x30;
    uint8_t *pnp5, *pnp7;

    /* Entry 5's PnP id made unique, entry 7's without a date. */
    pnp5 = v + bdb + rd16(v + lp + 1 + 5 * 9 + 6);
    pnp7 = v + bdb + rd16(v + lp + 1 + 7 * 9 + 6);
    CHECK("panel type: tables in the data block", pnp5 > v + data && pnp7 > pnp5);
    memcpy(pnp5, "\x30\xae\x51\x40\x01\x02\x03\x04\x05\x17", 10);
    memcpy(pnp7, "\x30\xae\x52\x40\x01\x02\x03\x04\x00\x00", 10);
    v[lo] = 0xff; /* no panel named */
    k_memset(edid, 0, sizeof(edid));

    decode(&vbt, &panel, v);
    CHECK("panel type: none early without EDID", panel.panel_type == -1 && !panel.has_lfp_mode);
    intel_bios_panel_init(&vbt, &panel, NULL, NULL, 1);
    CHECK("panel type: fallback late", panel.panel_type == 0 && panel.has_lfp_mode);

    memcpy(edid + 8, "\x30\xae\x51\x40\x01\x02\x03\x04\x05\x17", 10);
    panel.panel_type = -1;
    panel.has_lfp_mode = 0;
    intel_bios_panel_init(&vbt, &panel, NULL, edid, 1);
    CHECK("panel type: EDID's product id", panel.panel_type == 5);
    memcpy(edid + 8, "\x30\xae\x52\x40\x01\x02\x03\x04\x09\x18", 10);
    panel.panel_type = -1;
    intel_bios_panel_init(&vbt, &panel, NULL, edid, 0);
    CHECK("panel type: product id without date", panel.panel_type == 7);
    intel_bios_fini(&vbt);

    v[lo] = 0x20; /* out of range */
    decode(&vbt, &panel, v);
    CHECK("panel type: out of range", panel.panel_type == -1 && logged("invalid panel type"));
    intel_bios_fini(&vbt);

    /* The second panel's index is checked too. */
    v[lo] = 2;
    v[lo + 1] = 0x30;
    decode(&vbt, &panel, v);
    k_memset(&lfp2, 0, sizeof(lfp2));
    lfp2.handle = DEVICE_HANDLE_LFP2;
    panel.panel_type = -1;
    intel_bios_panel_init(&vbt, &panel, &lfp2, NULL, 0);
    CHECK("panel type: second out of range",
          panel.panel_type == -1 && logged("invalid second panel type"));
    v[lo + 1] = 9;
    intel_bios_fini(&vbt);
    decode(&vbt, &panel, v);
    panel.panel_type = -1;
    intel_bios_panel_init(&vbt, &panel, &lfp2, NULL, 0);
    CHECK("panel type: second", panel.panel_type == 9);
    intel_bios_fini(&vbt);
    free(v);
}

/** Malformed LFP data pointers dropped with the panel's mode, and missing
 * ones made up from the data block. */
static void row_vbt_lfp_ptrs(void)
{
    struct intel_vbt vbt;
    struct intel_vbt_panel panel;
    uint8_t *v = vbt_copy();
    uint32_t lp = vbt_block(v, BDB_LFP_DATA_PTRS);

    /* An entry out of step is malformed: the pointers are dropped and with
     * them the panel's mode, while the eDP block still decodes. */
    wr16(v + lp + 1 + 3 * 9 + 3, (uint16_t)(rd16(v + lp + 1 + 3 * 9 + 3) + 2));
    decode(&vbt, &panel, v);
    CHECK("ptrs: malformed dropped", !intel_bios_block(&vbt, BDB_LFP_DATA_PTRS) &&
                                         logged("malformed LFP data table pointers"));
    CHECK("ptrs: no mode without them",
          panel.panel_type == 2 && !panel.has_lfp_mode && panel.edp.pps.t1_t3 == 2000);
    intel_bios_fini(&vbt);

    /* Without the block, version 155 and later make it up from the data
     * block, and the Lenovo's 38-byte fp_timing agrees with the standard. */
    memcpy(v, lenovo_vbt, LENOVO_VBT_BYTES);
    v[lp - 3] = 200;
    decode(&vbt, &panel, v);
    CHECK("ptrs: made up", intel_bios_block(&vbt, BDB_LFP_DATA_PTRS) != NULL);
    CHECK("ptrs: made-up mode", panel.has_lfp_mode && panel.lfp_mode.hdisplay == 1024 &&
                                    panel.lfp_mode.clock == 65000 && panel.bios_lvds_val == 0x300);
    intel_bios_fini(&vbt);
    free(v);
}

/** A child size below the legacy 33 bytes takes no children; another
 * size above it is taken. */
static void row_vbt_children(void)
{
    struct intel_vbt vbt;
    struct intel_vbt_panel panel;
    uint8_t *v = vbt_copy();
    uint32_t gd = vbt_block(v, BDB_GENERAL_DEFINITIONS);

    v[gd + 4] = 32; /* smaller than the legacy 33 */
    decode(&vbt, &panel, v);
    CHECK("children: too small, none taken", vbt.present && vbt.nencoders == 0);
    intel_bios_fini(&vbt);

    v[gd + 4] = 38; /* not version 177's, taken */
    decode(&vbt, &panel, v);
    CHECK("children: other size taken", vbt.nencoders >= 1 && vbt.child_size == 38);
    intel_bios_fini(&vbt);
    free(v);
}

/** A table with a bad signature, a size or BDB past its buffer is refused,
 * and the decoder then takes the defaults. */
static void row_vbt_refused(void)
{
    struct intel_vbt vbt;
    uint8_t *v = vbt_copy();

    CHECK("valid: the Lenovo's", intel_bios_is_valid_vbt(lenovo_vbt, LENOVO_VBT_BYTES));
    CHECK("valid: short buffer", !intel_bios_is_valid_vbt(lenovo_vbt, 47));
    CHECK("valid: vbt_size past the buffer", !intel_bios_is_valid_vbt(lenovo_vbt, 4399));
    wr16(v + 0x30 + 20, 4400); /* BDB past the VBT */
    CHECK("valid: BDB past the VBT", !intel_bios_is_valid_vbt(v, LENOVO_VBT_BYTES));
    memcpy(v, lenovo_vbt, LENOVO_VBT_BYTES);
    wr32(v + 28, 4390); /* BDB header past the VBT */
    CHECK("valid: BDB header past the VBT", !intel_bios_is_valid_vbt(v, LENOVO_VBT_BYTES));
    memcpy(v, lenovo_vbt, LENOVO_VBT_BYTES);
    v[1] = 'X';
    CHECK("valid: signature", !intel_bios_is_valid_vbt(v, LENOVO_VBT_BYTES));
    k_memset(&vbt, 0, sizeof(vbt));
    CHECK("refused: decodes defaults", intel_bios_init(&vbt, v, LENOVO_VBT_BYTES, "test") == 0);
    CHECK("refused: not present", !vbt.present && vbt.version == 155 && vbt.nencoders == 5);
    intel_bios_fini(&vbt);
    free(v);
}

/** Without a VBT the features, outputs and panel are Linux's defaults,
 * eDP on DDI A and DP with HDMI on B to D. */
static void row_vbt_defaults(void)
{
    struct intel_vbt vbt;
    struct intel_vbt_panel panel;
    const struct intel_bios_encoder *e;
    int port;

    k_memset(&vbt, 0, sizeof(vbt));
    CHECK("defaults: init", intel_bios_init(&vbt, NULL, 0, NULL) == 0);
    CHECK("defaults: logged", logged("no video BIOS table"));
    CHECK("defaults: version", !vbt.present && vbt.version == 155 && vbt.source == NULL);
    CHECK("defaults: features", vbt.crt_ddc_pin == 2 && vbt.int_tv_support && vbt.int_crt_support &&
                                    vbt.int_lvds_support && vbt.lvds_use_ssc &&
                                    vbt.lvds_ssc_freq == 120000);
    CHECK("defaults: an output a port", vbt.nencoders == INTEL_NUM_PORTS);
    e = intel_bios_encoder_lookup(&vbt, INTEL_PORT_A);
    CHECK("defaults: eDP on DDI A", e && intel_bios_encoder_supports_edp(e) &&
                                        !intel_bios_encoder_supports_dvi(e) &&
                                        e->dvo_port == DVO_PORT_HDMIA);
    for (port = INTEL_PORT_B; port <= INTEL_PORT_D; port++) {
        e = intel_bios_encoder_lookup(&vbt, port);
        CHECK("defaults: DP and HDMI on B to D",
              e && intel_bios_encoder_supports_dp(e) && intel_bios_encoder_supports_hdmi(e) &&
                  !intel_bios_encoder_supports_edp(e) && e->dvo_port == DVO_PORT_HDMIA + port);
    }
    e = intel_bios_encoder_lookup(&vbt, INTEL_PORT_E);
    CHECK("defaults: nothing on E", e && e->device_type == 0 && e->dvo_port == DVO_PORT_HDMIE);

    k_memset(&panel, 0, sizeof(panel));
    panel.panel_type = -1;
    intel_bios_panel_init(&vbt, &panel, NULL, NULL, 0);
    CHECK("defaults: no panel early", panel.panel_type == -1);
    intel_bios_panel_init(&vbt, &panel, NULL, NULL, 1);
    CHECK("defaults: panel 0 late, backlight and dither assumed",
          panel.panel_type == 0 && panel.bl_present && panel.lvds_dither && !panel.has_lfp_mode &&
              panel.edp.bpp == 0);
    intel_bios_fini(&vbt);
}

/** The VBT found in an option ROM's image at a 4-byte boundary, and not
 * off one or cut short. */
static void row_rom(void)
{
    uint8_t *rom = (uint8_t *)calloc(1, 0x20000), *got;
    uint32_t size = 0;

    memcpy(rom + 0x1000, lenovo_vbt, 4400);
    got = intel_bios_rom_vbt(rom, 0x20000, &size);
    CHECK("rom: found", got && size == 4400 && memcmp(got, lenovo_vbt, 4400) == 0);
    free(got);

    k_memset(rom, 0, 0x20000);
    memcpy(rom + 0x1002, lenovo_vbt, 4400);
    CHECK("rom: off a 4-byte boundary", intel_bios_rom_vbt(rom, 0x20000, &size) == NULL);

    k_memset(rom, 0, 0x20000);
    memcpy(rom + 0x20000 - 4000, lenovo_vbt, 4000);
    CHECK("rom: cut short", intel_bios_rom_vbt(rom, 0x20000, &size) == NULL);
    free(rom);
}

/** Run the rows on the fixtures; 0 when every check held. */
int main(void)
{
    if (!load(LENOVO_OPREGION_FILE, lenovo_opregion, LENOVO_OPREGION_BYTES) ||
        !load(LENOVO_VBT_FILE, lenovo_vbt, LENOVO_VBT_BYTES)) {
        printf("intel-bios-test: fail: the fixtures are not readable\n");
        return 1;
    }
    row_opregion_lenovo();
    row_opregion_refused();
    row_opregion_rvda();
    row_vbt_lenovo();
    row_vbt_panel();
    row_vbt_blocks();
    row_vbt_port_a_dvi();
    row_vbt_panel_type();
    row_vbt_lfp_ptrs();
    row_vbt_children();
    row_vbt_refused();
    row_vbt_defaults();
    row_rom();
    if (fails) {
        printf("intel-bios-test: %d failed\n", fails);
        return 1;
    }
    printf("intel-bios-test: ok\n");
    return 0;
}
