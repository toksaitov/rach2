/*
 * intel_bios.c: the Video BIOS Table, decoded as Linux 6.12's intel_bios.c
 * decodes it for a Haswell display: the device's outputs, the panel's
 * mode, power sequence and eDP link, and the defaults without a table.
 */

#include "intel_bios.h"

#include "../drm.h"
#include "../../klog.h"
#include "../../lib/kerrno.h"
#include "../../lib/kstring.h"
#include "../../mm/kheap.h"

/*
 * The table is a VBT header, which gives the BDB header's offset, then the
 * BDB header and its blocks: each a byte of id, two of size and the size's
 * bytes. Linux copies each block it reads into a buffer of the size of its
 * own structure for that block, zero past the block's end, so that a field
 * a short block lacks reads as zero and the table's version alone says
 * which fields mean anything. This file keeps that rule: the offsets below
 * are of Linux's structures, and every read of a block lies inside the
 * size Linux gives it.
 */

/* struct vbt_header and struct bdb_header. */
#define VBT_HDR_SIZE       48u
#define VBT_HDR_VBT_SIZE   24u
#define VBT_HDR_BDB_OFFSET 28u
#define BDB_HDR_SIZE       22u
#define BDB_HDR_VERSION    16u
#define BDB_HDR_HDR_SIZE   18u
#define BDB_HDR_BDB_SIZE   20u

/* Block 53, the MIPI sequence block, which from its version 3 keeps its
 * size in four bytes after its version byte. */
#define BDB_MIPI_SEQUENCE 53

/* Block 1, struct bdb_general_features. */
#define GF_SIZE  7u
#define GF_BITS2 1u /* enable_ssc 1, ssc_freq 2, display_clock_mode 6 */
#define GF_BITS3 2u /* rotate_180 2, fdi_rx_polarity_inverted 3 */
#define GF_BITS5 4u /* int_crt_support 0, int_tv_support 1 */
#define GF_BITS7 6u /* afc_startup_config 0-1 */

/* Block 2, struct bdb_general_definitions, then its child configs. */
#define GD_SIZE           5u
#define GD_CRT_DDC_PIN    0u
#define GD_CHILD_DEV_SIZE 4u
/* LEGACY_CHILD_DEVICE_CONFIG_SIZE, the least Linux takes. */
#define CHILD_LEGACY_SIZE 33

/* struct child_device_config. */
#define CHILD_HANDLE        0u
#define CHILD_DEVICE_TYPE   2u
#define CHILD_HDMI_LS       7u /* hdmi_level_shifter_value 0-4, max data rate 5-7 */
#define CHILD_DVO_PORT      16u
#define CHILD_DDC_PIN       19u
#define CHILD_FLAGS         23u /* lane_reversal 1, iboost 3, hpd_invert 4, lanes 6-7 */
#define CHILD_AUX_CHANNEL   25u
#define CHILD_IBOOST_LEVELS 37u /* dp 0-3, hdmi 4-7 */
#define CHILD_DP_MAX_RATE   38u /* 0-2 */

/* Block 12, struct bdb_driver_features. */
#define DF_SIZE                     19u
#define DF_EXT_BITS1                7u  /* u16: lvds_config 11-12 */
#define DF_FEATURE_FLAGS            17u /* u16: drrs 5, psr 9, dmrrs 12 */
#define BDB_DRIVER_FEATURE_INT_LVDS 1

/* Block 27, struct bdb_edp. */
#define EDP_SIZE            848u
#define EDP_PPS             0u   /* struct edp_power_seq[16], 10 bytes each */
#define EDP_COLOR_DEPTH     160u /* u32, 2 bits a panel */
#define EDP_FAST_LINK       164u /* struct edp_fast_link_params[16], 2 bytes */
#define EDP_SDRRS_MSA_DELAY 196u /* u32, 2 bits a panel */
#define EDP_VSWING_PREEMPH  204u /* u64, 4 bits a panel */
#define EDP_FAST_LINK_RATE  748u /* u16[16], in 200 kHz */
#define EDP_MAX_LINK_RATE   780u /* u16[16], in 200 kHz */
#define EDP_DSC_DISABLE     812u /* u16, a bit a panel */

/* Block 40, struct bdb_lfp_options. */
#define LO_SIZE        34u
#define LO_PANEL_TYPE  0u
#define LO_PANEL_TYPE2 1u
#define LO_BITS        2u  /* pixel_dither 5 */
#define LO_DPS_BITS    16u /* u32, 2 bits a panel */

/* Block 41, struct bdb_lfp_data_ptrs: a count, 16 entries of three tables
 * (fp_timing, dvo_timing, panel_pnp_id), each an offset of two bytes and a
 * size of one, then the panel names' table. */
#define LP_SIZE       148u
#define LP_ENTRY      1u
#define LP_ENTRY_SIZE 9u
#define LP_NAME       145u

/* Block 42: struct fp_timing, the DTD (struct bdb_edid_dtd) and the PnP id,
 * then struct bdb_lfp_data_tail. */
#define LD_SIZE          1184u /* sizeof(struct bdb_lfp_data) */
#define LD_FP_TIMING_STD 38
#define LD_DTD_SIZE      18
#define LD_PNP_SIZE      10
#define LD_NAME_SIZE     13
#define LD_TAIL_SIZE     310u
#define LD_TAIL_DRRS_MIN 210u /* u8[16] after the names and scaling_enable */
#define FP_X_RES         0u
#define FP_Y_RES         2u
#define FP_LVDS_REG_VAL  8u

/* Block 43, struct bdb_lfp_backlight. */
#define BL_SIZE                305u
#define BL_ENTRY_SIZE          0u
#define BL_DATA                1u /* struct lfp_backlight_data_entry[16], 6 bytes */
#define BL_ENTRY               6
#define BL_LEVEL               97u  /* u8[16], 162-233 */
#define BL_CONTROL             113u /* type 0-3, controller 4-7, 191+ */
#define BL_LEVEL234            129u /* struct lfp_brightness_level[16], 4 bytes */
#define BL_MIN234              193u
#define BL_PRECISION           257u
#define BDB_BACKLIGHT_TYPE_PWM 2

/* Block 44, struct bdb_lfp_power. */
#define LPW_SIZE  136u
#define LPW_PSR   24u
#define LPW_DRRS  26u
#define LPW_DMRRS 32u
#define LPW_HOBL  54u
#define LPW_VRR   56u

/* The blocks Linux reads that this file decodes, with Linux's sizes for
 * them; the LFP data block's comes from its pointers. */
static const struct {
    uint8_t id;
    uint32_t min_size;
} bdb_blocks[INTEL_BDB_BLOCKS] = {
    {BDB_GENERAL_FEATURES, GF_SIZE},
    {BDB_GENERAL_DEFINITIONS, GD_SIZE},
    {BDB_DRIVER_FEATURES, DF_SIZE},
    {BDB_EDP, EDP_SIZE},
    {BDB_LFP_OPTIONS, LO_SIZE},
    {BDB_LFP_DATA_PTRS, LP_SIZE},
    {BDB_LFP_DATA, 0},
    {BDB_LFP_BACKLIGHT, BL_SIZE},
    {BDB_LFP_POWER, LPW_SIZE},
};

/* Haswell's GMBUS pin of the VGA connector's DDC, Linux's
 * GMBUS_PIN_VGADDC, and the highest valid pin, GMBUS_PIN_DPD. */
#define GMBUS_PIN_VGADDC 2
#define GMBUS_PIN_MAX    6

/** The little-endian 16 bits at `p`. */
static uint16_t get16(const uint8_t *p)
{
    return (uint16_t)(p[0] | p[1] << 8);
}

/** The little-endian 32 bits at `p`. */
static uint32_t get32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

/** Write `v` little-endian at `p`. */
static void put16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

/** The `bits` bits of panel `panel_type` in `value`, Linux's panel_bits. */
static uint32_t panel_bits(uint32_t value, int panel_type, int bits)
{
    return (value >> (panel_type * bits)) & ((1u << bits) - 1);
}

/* ------------------------------------------------------------------------
 * Finding blocks
 */

/*
 * The table being decoded: the BDB at `bdb`, whose blocks end at
 * `bdb_size`; `avail` bytes of the VBT lie from `bdb` on, at least
 * `bdb_size`.
 */
struct bdb_view {
    const uint8_t *bdb;
    uint32_t bdb_size;
    uint32_t avail;
};

/** The size of the block whose id is at `at`, Linux's _get_blocksize; a
 * MIPI sequence block of version 3 or later past `avail` reads as one
 * that runs to the end. */
static uint32_t raw_block_size(const struct bdb_view *v, uint32_t at)
{
    if (v->bdb[at] == BDB_MIPI_SEQUENCE && at + 4 <= v->avail && v->bdb[at + 3] >= 3) {
        if (at + 8 > v->avail) {
            return 0xffffffffu;
        }
        return get32(v->bdb + at + 4);
    }
    return get16(v->bdb + at + 1);
}

/** The offset in the BDB of the data of the first block `id`, past its
 * id and size, Linux's find_raw_section; 0 when there is none or it runs
 * past the BDB. */
static uint32_t find_raw_section(const struct bdb_view *v, uint8_t id)
{
    uint32_t at = get16(v->bdb + BDB_HDR_HDR_SIZE), size;

    while (at + 3 < v->bdb_size) {
        uint8_t cur = v->bdb[at];

        size = raw_block_size(v, at);
        at += 3;
        if (size > v->bdb_size || at + size > v->bdb_size) {
            return 0;
        }
        if (cur == id) {
            return at;
        }
        at += size;
    }
    return 0;
}

/** The size of the block whose data is at `data`, Linux's get_blocksize. */
static uint32_t block_size_at(const struct bdb_view *v, uint32_t data)
{
    return raw_block_size(v, data - 3);
}

const struct intel_bdb_block *intel_bios_block(const struct intel_vbt *vbt, uint8_t id)
{
    int i;

    for (i = 0; i < INTEL_BDB_BLOCKS; i++) {
        if (vbt->blocks[i].id == id && vbt->blocks[i].data) {
            return &vbt->blocks[i];
        }
    }
    return NULL;
}

/* ------------------------------------------------------------------------
 * The LFP data pointers
 */

/** Entry `i`'s table `t` (0 fp_timing, 1 dvo_timing, 2 panel_pnp_id) of a
 * pointer block: the place of its offset; the size is the byte after. */
static uint8_t *lfp_ptr(uint8_t *ptrs, int i, int t)
{
    return ptrs + LP_ENTRY + i * LP_ENTRY_SIZE + t * 3;
}

/** The offset of entry `i`'s table `t`. */
static uint16_t lfp_off(const uint8_t *ptrs, int i, int t)
{
    return get16(ptrs + LP_ENTRY + i * LP_ENTRY_SIZE + t * 3);
}

/** The size of entry `i`'s table `t`. */
static uint8_t lfp_size(const uint8_t *ptrs, int i, int t)
{
    return ptrs[LP_ENTRY + i * LP_ENTRY_SIZE + t * 3 + 2];
}

/** Whether entries 1 to 15 have entry 0's table sizes, each table
 * `entry_size` bytes past the entry before's. */
static int lfp_ptrs_uniform(const uint8_t *ptrs, int entry_size)
{
    int i, t;

    for (i = 1; i < 16; i++) {
        for (t = 0; t < 3; t++) {
            if (lfp_size(ptrs, i, t) != lfp_size(ptrs, 0, t) ||
                (int)lfp_off(ptrs, i, t) - (int)lfp_off(ptrs, i - 1, t) != entry_size) {
                return 0;
            }
        }
    }
    return 1;
}

/** Whether every entry's tables of `size[]` bytes and the names' table lie
 * inside the LFP data block at `data`, of `data_size` bytes, and each
 * fp_timing ends in its 0xffff terminator. */
static int lfp_ptrs_fit(const struct bdb_view *v, uint32_t data, uint32_t data_size,
                        const uint8_t *ptrs, const int size[3])
{
    int i, t;

    for (i = 0; i < 16; i++) {
        for (t = 0; t < 3; t++) {
            if (lfp_off(ptrs, i, t) + (uint32_t)size[t] > data_size) {
                return 0;
            }
        }
    }
    if (get16(ptrs + LP_NAME) + 16u * ptrs[LP_NAME + 2] > data_size) {
        return 0;
    }
    for (i = 0; i < 16; i++) {
        if (get16(v->bdb + data + lfp_off(ptrs, i, 0) + size[0] - 2) != 0xffff) {
            return 0;
        }
    }
    return 1;
}

/**
 * Whether the pointers, already made relative to the LFP data block, lay
 * out sixteen entries of uniform size inside that block with each
 * fp_timing ending in its 0xffff terminator, Linux's
 * validate_lfp_data_ptrs.
 */
static int validate_lfp_data_ptrs(const struct bdb_view *v, const uint8_t *ptrs)
{
    int size[3], name_size, entry_size;
    uint32_t data, data_size;

    data = find_raw_section(v, BDB_LFP_DATA);
    if (!data) {
        return 0;
    }
    data_size = block_size_at(v, data);
    if (data_size == 0 || ptrs[0] != 3) {
        return 0;
    }
    size[0] = lfp_size(ptrs, 0, 0);
    size[1] = lfp_size(ptrs, 0, 1);
    size[2] = lfp_size(ptrs, 0, 2);
    name_size = ptrs[LP_NAME + 2];
    if (size[0] < 32 || size[1] != LD_DTD_SIZE || size[2] != LD_PNP_SIZE) {
        return 0;
    }
    if (name_size != 0 && name_size != LD_NAME_SIZE) {
        return 0;
    }
    entry_size = (int)lfp_off(ptrs, 1, 0) - (int)lfp_off(ptrs, 0, 0);
    if (16 * (long)entry_size > (long)data_size || !lfp_ptrs_uniform(ptrs, entry_size)) {
        return 0;
    }
    /* Every real table but Valleyview's and Cherryview's has six bytes in
     * fp_timing that its size leaves out, with the terminator in them. */
    if (size[0] + 6 + size[1] + size[2] == entry_size) {
        size[0] += 6;
    }
    if (size[0] + size[1] + size[2] != entry_size) {
        return 0;
    }
    if (lfp_off(ptrs, 0, 0) + size[0] != lfp_off(ptrs, 0, 1) ||
        lfp_off(ptrs, 0, 1) + size[1] != lfp_off(ptrs, 0, 2) ||
        lfp_off(ptrs, 0, 2) + size[2] != entry_size) {
        return 0;
    }
    return lfp_ptrs_fit(v, data, data_size, ptrs, size);
}

/** Make the copied pointers' offsets, which count from the BDB, count from
 * the LFP data block, and validate them, Linux's fixup_lfp_data_ptrs. */
static int fixup_lfp_data_ptrs(const struct bdb_view *v, uint8_t *ptrs)
{
    uint32_t offset = find_raw_section(v, BDB_LFP_DATA);
    int i, t;

    for (i = 0; i < 16; i++) {
        for (t = 0; t < 3; t++) {
            if (lfp_off(ptrs, i, t) < offset) {
                return 0;
            }
        }
        for (t = 0; t < 3; t++) {
            put16(lfp_ptr(ptrs, i, t), (uint16_t)(lfp_off(ptrs, i, t) - offset));
        }
    }
    if (ptrs[LP_NAME + 2]) {
        if (get16(ptrs + LP_NAME) < offset) {
            return 0;
        }
        put16(ptrs + LP_NAME, (uint16_t)(get16(ptrs + LP_NAME) - offset));
    }
    return validate_lfp_data_ptrs(v, ptrs);
}

/**
 * Make up the pointer block a table of version 155 or later leaves out, as
 * Linux's generate_lfp_data_ptrs: sixteen entries of a 38-byte fp_timing,
 * a DTD and a PnP id from the start of the LFP data block, then the names
 * when they fit. Offsets count from the BDB, as the table's would. A block
 * from the heap of LP_SIZE bytes, or NULL.
 */
static uint8_t *generate_lfp_data_ptrs(const struct bdb_view *v, uint16_t version)
{
    uint32_t data, data_size, entry = LD_FP_TIMING_STD + LD_DTD_SIZE + LD_PNP_SIZE;
    uint8_t *ptrs;
    int i;

    if (version < 155) {
        return NULL;
    }
    data = find_raw_section(v, BDB_LFP_DATA);
    if (!data) {
        return NULL;
    }
    data_size = block_size_at(v, data);
    if (entry * 16 > data_size) {
        return NULL;
    }
    ptrs = (uint8_t *)kzalloc(LP_SIZE);
    if (!ptrs) {
        return NULL;
    }
    ptrs[0] = 3;
    for (i = 0; i < 16; i++) {
        put16(lfp_ptr(ptrs, i, 0), (uint16_t)(data + i * entry));
        lfp_ptr(ptrs, i, 0)[2] = LD_FP_TIMING_STD;
        put16(lfp_ptr(ptrs, i, 1), (uint16_t)(data + i * entry + LD_FP_TIMING_STD));
        lfp_ptr(ptrs, i, 1)[2] = LD_DTD_SIZE;
        put16(lfp_ptr(ptrs, i, 2), (uint16_t)(data + i * entry + LD_FP_TIMING_STD + LD_DTD_SIZE));
        lfp_ptr(ptrs, i, 2)[2] = LD_PNP_SIZE;
    }
    if (16 * (entry + LD_NAME_SIZE) <= data_size) {
        put16(ptrs + LP_NAME, (uint16_t)(data + entry * 16));
        ptrs[LP_NAME + 2] = LD_NAME_SIZE;
    }
    return ptrs;
}

/** Linux's lfp_data_min_size: its structure's size, or more when the
 * names' table runs past it. */
static uint32_t lfp_data_min_size(const struct intel_vbt *vbt)
{
    const struct intel_bdb_block *ptrs = intel_bios_block(vbt, BDB_LFP_DATA_PTRS);
    uint32_t size = LD_SIZE;

    if (!ptrs) {
        return 0;
    }
    if (ptrs->data[LP_NAME + 2] && get16(ptrs->data + LP_NAME) + LD_TAIL_SIZE > size) {
        size = get16(ptrs->data + LP_NAME) + LD_TAIL_SIZE;
    }
    return size;
}

/**
 * Copy block `id` into slot `slot` of `vbt`, padded to `min_size`, as
 * Linux's init_bdb_block; a pointer block the table lacks is made up, and
 * one that does not validate is dropped. -ENOMEM when the heap is out.
 */
static int32_t init_bdb_block(struct intel_vbt *vbt, const struct bdb_view *v, int slot, uint8_t id,
                              uint32_t min_size)
{
    struct intel_bdb_block *b = &vbt->blocks[slot];
    uint8_t *made = NULL;
    const uint8_t *src;
    uint32_t data, size;

    b->id = id;
    data = find_raw_section(v, id);
    if (data) {
        src = v->bdb + data;
        size = block_size_at(v, data);
    } else if (id == BDB_LFP_DATA_PTRS) {
        made = generate_lfp_data_ptrs(v, vbt->version);
        if (!made) {
            return 0;
        }
        src = made;
        size = LP_SIZE;
    } else {
        return 0;
    }
    b->size = size > min_size ? size : min_size;
    b->data = (uint8_t *)kzalloc(b->size ? b->size : 1);
    if (!b->data) {
        kfree(made);
        b->size = 0;
        return -ENOMEM;
    }
    k_memcpy(b->data, src, size);
    b->raw_size = size;
    kfree(made);
    if (id == BDB_LFP_DATA_PTRS && !fixup_lfp_data_ptrs(v, b->data)) {
        klog(KLOG_ERR, "i915", "vbt: malformed LFP data table pointers");
        kfree(b->data);
        b->data = NULL;
        b->size = b->raw_size = 0;
    }
    return 0;
}

/* ------------------------------------------------------------------------
 * The device's blocks
 */

/** Linux's intel_bios_ssc_frequency for display version 7. */
static uint32_t ssc_frequency(int alternate)
{
    return alternate ? 100000 : 120000;
}

/** Linux's init_vbt_defaults for Haswell, which has a PCH. */
static void init_vbt_defaults(struct intel_vbt *vbt)
{
    vbt->crt_ddc_pin = GMBUS_PIN_VGADDC;
    vbt->int_tv_support = 1;
    vbt->int_crt_support = 1;
    vbt->int_lvds_support = 1;
    vbt->lvds_use_ssc = 1;
    vbt->lvds_ssc_freq = ssc_frequency(0);
    vbt->orientation = DRM_MODE_PANEL_ORIENTATION_UNKNOWN;
}

/** Linux's parse_general_features, for a display with DDI. */
static void parse_general_features(struct intel_vbt *vbt)
{
    const struct intel_bdb_block *b = intel_bios_block(vbt, BDB_GENERAL_FEATURES);
    const uint8_t *g;

    if (!b) {
        return;
    }
    g = b->data;
    vbt->int_tv_support = (g[GF_BITS5] >> 1) & 1;
    /* int_crt_support is not trusted on earlier tables. */
    if (vbt->version >= 155) {
        vbt->int_crt_support = g[GF_BITS5] & 1;
    }
    vbt->lvds_use_ssc = (g[GF_BITS2] >> 1) & 1;
    vbt->lvds_ssc_freq = ssc_frequency((g[GF_BITS2] >> 2) & 1);
    vbt->display_clock_mode = (g[GF_BITS2] >> 6) & 1;
    vbt->fdi_rx_polarity_inverted = (g[GF_BITS3] >> 3) & 1;
    if (vbt->version >= 181) {
        vbt->orientation = (g[GF_BITS3] >> 2) & 1 ? DRM_MODE_PANEL_ORIENTATION_BOTTOM_UP
                                                  : DRM_MODE_PANEL_ORIENTATION_NORMAL;
    } else {
        vbt->orientation = DRM_MODE_PANEL_ORIENTATION_UNKNOWN;
    }
}

/** Linux's child_device_expected_size: the size of a child config for the
 * table's version, or -1 past the versions it knows. */
static int child_device_expected_size(uint16_t version)
{
    if (version > 256) {
        return -1;
    } else if (version >= 256) {
        return 40;
    } else if (version >= 216) {
        return 39;
    } else if (version >= 196) {
        return 38;
    } else if (version >= 195) {
        return 37;
    } else if (version >= 111) {
        return CHILD_LEGACY_SIZE;
    } else if (version >= 106) {
        return 27;
    }
    return 22;
}

/** Add an output for the child config `child`, `size` bytes of it; NULL
 * when the heap is out. */
static struct intel_bios_encoder *add_encoder(struct intel_vbt *vbt, const uint8_t *child, int size)
{
    struct intel_bios_encoder *grown, *e;

    grown = (struct intel_bios_encoder *)kzalloc((vbt->nencoders + 1) * sizeof(*grown));
    if (!grown) {
        return NULL;
    }
    if (vbt->nencoders) {
        k_memcpy(grown, vbt->encoders, vbt->nencoders * sizeof(*grown));
    }
    kfree(vbt->encoders);
    vbt->encoders = grown;
    e = &grown[vbt->nencoders++];
    k_memcpy(e->child, child, size < INTEL_CHILD_SIZE ? size : INTEL_CHILD_SIZE);
    return e;
}

/**
 * Linux's parse_general_definitions: the VGA DDC pin and an output for
 * each child config with a device type. A config smaller than the legacy
 * size refuses them all; one of a size other than the version's is logged
 * and taken. -ENOMEM when the heap is out.
 */
static int32_t parse_general_definitions(struct intel_vbt *vbt)
{
    const struct intel_bdb_block *b = intel_bios_block(vbt, BDB_GENERAL_DEFINITIONS);
    int size, expected, n, i;
    const uint8_t *child;

    if (!b) {
        return 0;
    }
    if (b->raw_size < GD_SIZE) {
        return 0;
    }
    if (b->data[GD_CRT_DDC_PIN] >= 1 && b->data[GD_CRT_DDC_PIN] <= GMBUS_PIN_MAX) {
        vbt->crt_ddc_pin = b->data[GD_CRT_DDC_PIN];
    }
    size = b->data[GD_CHILD_DEV_SIZE];
    vbt->child_size = (uint8_t)size;
    expected = child_device_expected_size(vbt->version);
    if (expected < 0) {
        expected = INTEL_CHILD_SIZE;
    }
    if (size != expected) {
        klog(KLOG_ERR, "i915", "vbt: child device config of %d bytes, %d expected for version %u",
             size, expected, (unsigned)vbt->version);
    }
    if (size < CHILD_LEGACY_SIZE) {
        return 0;
    }
    n = (int)((b->raw_size - GD_SIZE) / (uint32_t)size);
    for (i = 0; i < n; i++) {
        child = b->data + GD_SIZE + i * size;
        if (get16(child + CHILD_DEVICE_TYPE) == 0) {
            continue;
        }
        if (!add_encoder(vbt, child, size)) {
            return -ENOMEM;
        }
    }
    return 0;
}

/** Linux's parse_driver_features for display version 5 and later, which
 * counts the internal SDVO LVDS setting as eDP. */
static void parse_driver_features(struct intel_vbt *vbt)
{
    const struct intel_bdb_block *b = intel_bios_block(vbt, BDB_DRIVER_FEATURES);

    if (!b) {
        return;
    }
    if (((get16(b->data + DF_EXT_BITS1) >> 11) & 3) != BDB_DRIVER_FEATURE_INT_LVDS) {
        vbt->int_lvds_support = 0;
    }
}

/* ------------------------------------------------------------------------
 * The outputs
 */

int intel_bios_encoder_supports_crt(const struct intel_bios_encoder *e)
{
    return (e->device_type & DEVICE_TYPE_ANALOG_OUTPUT) != 0;
}

int intel_bios_encoder_supports_dvi(const struct intel_bios_encoder *e)
{
    return (e->device_type & DEVICE_TYPE_TMDS_DVI_SIGNALING) != 0;
}

int intel_bios_encoder_supports_hdmi(const struct intel_bios_encoder *e)
{
    return intel_bios_encoder_supports_dvi(e) && !(e->device_type & DEVICE_TYPE_NOT_HDMI_OUTPUT);
}

int intel_bios_encoder_supports_dp(const struct intel_bios_encoder *e)
{
    return (e->device_type & DEVICE_TYPE_DISPLAYPORT_OUTPUT) != 0;
}

int intel_bios_encoder_supports_edp(const struct intel_bios_encoder *e)
{
    return intel_bios_encoder_supports_dp(e) && (e->device_type & DEVICE_TYPE_INTERNAL_CONNECTOR);
}

int intel_bios_encoder_supports_dsi(const struct intel_bios_encoder *e)
{
    return (e->device_type & DEVICE_TYPE_MIPI_OUTPUT) != 0;
}

/** The kind of a DVO port, Linux's dvo_port_type for the ports Haswell's
 * tables name: DVO_PORT_HDMIA for an HDMI port, DVO_PORT_DPA for a DP one. */
static uint8_t dvo_port_type(uint8_t dvo_port)
{
    switch (dvo_port) {
    case DVO_PORT_HDMIA:
    case DVO_PORT_HDMIB:
    case DVO_PORT_HDMIC:
    case DVO_PORT_HDMID:
    case DVO_PORT_HDMIE:
        return DVO_PORT_HDMIA;
    case DVO_PORT_DPA:
    case DVO_PORT_DPB:
    case DVO_PORT_DPC:
    case DVO_PORT_DPD:
    case DVO_PORT_DPE:
        return DVO_PORT_DPA;
    default:
        return dvo_port;
    }
}

int intel_bios_encoder_supports_dp_dual_mode(const struct intel_bios_encoder *e)
{
    if (!intel_bios_encoder_supports_dp(e) || !intel_bios_encoder_supports_hdmi(e)) {
        return 0;
    }
    if (dvo_port_type(e->dvo_port) == DVO_PORT_DPA) {
        return 1;
    }
    /* An HDMI port is DP++ only with an AUX channel. */
    return dvo_port_type(e->dvo_port) == DVO_PORT_HDMIA && e->child[CHILD_AUX_CHANNEL] != 0;
}

/** The DDI port of a DVO port, Linux's dvo_port_to_port for display
 * versions before 11: each port answers to its HDMI and DP values, and
 * port E also to the CRT's. */
static int dvo_port_to_port(uint8_t dvo_port)
{
    static const int8_t mapping[INTEL_NUM_PORTS][3] = {
        {DVO_PORT_HDMIA, DVO_PORT_DPA, -1},           {DVO_PORT_HDMIB, DVO_PORT_DPB, -1},
        {DVO_PORT_HDMIC, DVO_PORT_DPC, -1},           {DVO_PORT_HDMID, DVO_PORT_DPD, -1},
        {DVO_PORT_HDMIE, DVO_PORT_DPE, DVO_PORT_CRT},
    };
    int port, i;

    for (port = 0; port < INTEL_NUM_PORTS; port++) {
        for (i = 0; i < 3 && mapping[port][i] >= 0; i++) {
            if (dvo_port == (uint8_t)mapping[port][i]) {
                return port;
            }
        }
    }
    return INTEL_PORT_NONE;
}

/** The AUX channel of the table's value, Linux's map_aux_ch through its
 * direct map; INTEL_AUX_CH_NONE for 0 or a value it lacks. */
static int map_aux_ch(uint8_t aux_channel)
{
    static const uint8_t direct[] = {0x40, 0x10, 0x20, 0x30, 0x50, 0x60, 0x70, 0x80, 0x90};
    int i;

    if (aux_channel == 0) {
        return INTEL_AUX_CH_NONE;
    }
    for (i = 0; i < (int)sizeof(direct); i++) {
        if (direct[i] == aux_channel) {
            return i;
        }
    }
    klog(KLOG_DEBUG, "i915", "vbt: AUX channel 0x%x is not valid here, ignored",
         (unsigned)aux_channel);
    return INTEL_AUX_CH_NONE;
}

/** The I_boost level of the table's value, Linux's translate_iboost. */
static uint8_t translate_iboost(uint8_t val)
{
    static const uint8_t mapping[] = {1, 3, 7};

    return val < sizeof(mapping) ? mapping[val] : 0;
}

/** Linux's intel_bios_hdmi_max_tmds_clock, in kHz. */
static uint32_t hdmi_max_tmds_clock(uint16_t version, uint8_t rate)
{
    static const uint32_t khz[] = {0, 297000, 165000, 594000, 340000, 300000};

    if (version < 204 || rate >= sizeof(khz) / sizeof(khz[0])) {
        return 0;
    }
    return khz[rate];
}

/** Linux's intel_bios_dp_max_link_rate, in kHz. */
static uint32_t dp_max_link_rate(uint16_t version, uint8_t rate)
{
    static const uint32_t v230[] = {0, 162000, 270000, 540000, 810000, 1000000, 1350000, 2000000};
    static const uint32_t v216[] = {810000, 540000, 270000, 162000};

    if (version < 216) {
        return 0;
    }
    if (version >= 230) {
        return v230[rate & 7];
    }
    return rate < 4 ? v216[rate] : v216[0];
}

/**
 * Derive what Linux's accessors give for one output, and apply Linux's
 * parse_ddi_port corrections: port A does not carry DVI or HDMI before
 * display version 12, whatever the table says. Haswell has no level
 * shifter values above Broadwell's limit to clamp.
 */
static void parse_ddi_port(struct intel_vbt *vbt, struct intel_bios_encoder *e)
{
    const uint8_t *c = e->child;

    e->handle = get16(c + CHILD_HANDLE);
    e->device_type = get16(c + CHILD_DEVICE_TYPE);
    e->dvo_port = c[CHILD_DVO_PORT];
    e->port = (int8_t)dvo_port_to_port(e->dvo_port);
    if (e->port == INTEL_PORT_A && intel_bios_encoder_supports_dvi(e)) {
        klog(KLOG_DEBUG, "i915", "vbt: port A claims DVI%s, ignored",
             intel_bios_encoder_supports_hdmi(e) ? "/HDMI" : "");
        e->device_type &= ~DEVICE_TYPE_TMDS_DVI_SIGNALING;
        e->device_type |= DEVICE_TYPE_NOT_HDMI_OUTPUT;
    }
    put16(e->child + CHILD_DEVICE_TYPE, e->device_type);
    e->ddc_pin = c[CHILD_DDC_PIN];
    e->aux_ch = (int8_t)map_aux_ch(c[CHILD_AUX_CHANNEL]);
    e->hdmi_level_shift = vbt->version < 158 ? -1 : (int8_t)(c[CHILD_HDMI_LS] & 0x1f);
    e->hdmi_max_tmds_clock = hdmi_max_tmds_clock(vbt->version, c[CHILD_HDMI_LS] >> 5);
    e->dp_max_link_rate = dp_max_link_rate(vbt->version, c[CHILD_DP_MAX_RATE] & 7);
    e->dp_max_lane_count = vbt->version < 244 ? 0 : (uint8_t)(((c[CHILD_FLAGS] >> 6) & 3) + 1);
    if (vbt->version >= 196 && (c[CHILD_FLAGS] >> 3) & 1) {
        e->dp_boost_level = translate_iboost(c[CHILD_IBOOST_LEVELS] & 0xf);
        e->hdmi_boost_level = translate_iboost(c[CHILD_IBOOST_LEVELS] >> 4);
    }
    e->lane_reversal = (c[CHILD_FLAGS] >> 1) & 1;
    e->hpd_invert = vbt->version >= 196 && ((c[CHILD_FLAGS] >> 4) & 1);
}

/**
 * Linux's init_vbt_missing_defaults for a DDI display: an output for each
 * of Haswell's ports, none of which is Type-C. Ports B to D carry DVI and
 * DP, port A carries eDP, and port E nothing of either. The version is set
 * to 155 so that the checks of a baseline table pass. -ENOMEM when the heap
 * is out.
 */
static int32_t init_vbt_missing_defaults(struct intel_vbt *vbt)
{
    uint8_t child[INTEL_CHILD_SIZE];
    uint16_t type;
    int port;

    for (port = INTEL_PORT_A; port < INTEL_NUM_PORTS; port++) {
        k_memset(child, 0, sizeof(child));
        type = 0;
        child[CHILD_DVO_PORT] =
            (uint8_t)(port == INTEL_PORT_E ? DVO_PORT_HDMIE : DVO_PORT_HDMIA + port);
        if (port != INTEL_PORT_A && port != INTEL_PORT_E) {
            type |= DEVICE_TYPE_TMDS_DVI_SIGNALING;
        }
        if (port != INTEL_PORT_E) {
            type |= DEVICE_TYPE_DISPLAYPORT_OUTPUT;
        }
        if (port == INTEL_PORT_A) {
            type |= DEVICE_TYPE_INTERNAL_CONNECTOR;
        }
        put16(child + CHILD_DEVICE_TYPE, type);
        if (!add_encoder(vbt, child, INTEL_CHILD_SIZE)) {
            return -ENOMEM;
        }
    }
    vbt->version = 155;
    return 0;
}

/* ------------------------------------------------------------------------
 * The table whole
 */

int intel_bios_is_valid_vbt(const uint8_t *buf, uint32_t size)
{
    uint32_t vbt_size, bdb_offset;

    if (!buf || size < VBT_HDR_SIZE) {
        return 0;
    }
    if (buf[0] != '$' || buf[1] != 'V' || buf[2] != 'B' || buf[3] != 'T') {
        return 0;
    }
    vbt_size = get16(buf + VBT_HDR_VBT_SIZE);
    if (vbt_size > size) {
        return 0;
    }
    bdb_offset = get32(buf + VBT_HDR_BDB_OFFSET);
    if (bdb_offset > vbt_size || vbt_size - bdb_offset < BDB_HDR_SIZE) {
        return 0;
    }
    if (get16(buf + bdb_offset + BDB_HDR_BDB_SIZE) > vbt_size - bdb_offset) {
        return 0;
    }
    return 1;
}

uint8_t *intel_bios_rom_vbt(const uint8_t *rom, uint32_t size, uint32_t *vbt_size)
{
    const uint8_t *p = NULL;
    uint32_t i, n;
    uint8_t *copy;

    for (i = 0; i + 4 < size; i += 4) {
        if (rom[i] == '$' && rom[i + 1] == 'V' && rom[i + 2] == 'B' && rom[i + 3] == 'T') {
            p = rom + i;
            size -= i;
            break;
        }
    }
    if (!p || size < VBT_HDR_SIZE) {
        return NULL;
    }
    n = get16(p + VBT_HDR_VBT_SIZE);
    if (n > size) {
        return NULL;
    }
    copy = (uint8_t *)kmalloc(n ? n : 1);
    if (!copy) {
        return NULL;
    }
    k_memcpy(copy, p, n);
    if (!intel_bios_is_valid_vbt(copy, n)) {
        kfree(copy);
        return NULL;
    }
    *vbt_size = n;
    return copy;
}

/** Free the blocks and outputs, keeping the rest of `vbt`. */
static void free_tables(struct intel_vbt *vbt)
{
    int i;

    for (i = 0; i < INTEL_BDB_BLOCKS; i++) {
        kfree(vbt->blocks[i].data);
        vbt->blocks[i].data = NULL;
    }
    kfree(vbt->encoders);
    vbt->encoders = NULL;
    vbt->nencoders = 0;
}

void intel_bios_fini(struct intel_vbt *vbt)
{
    free_tables(vbt);
    k_memset(vbt, 0, sizeof(*vbt));
}

int32_t intel_bios_init(struct intel_vbt *vbt, const uint8_t *buf, uint32_t size,
                        const char *source)
{
    struct bdb_view v;
    uint32_t bdb_offset, min;
    int32_t err = 0;
    int i;

    free_tables(vbt);
    k_memset(vbt, 0, sizeof(*vbt));
    init_vbt_defaults(vbt);

    if (buf && intel_bios_is_valid_vbt(buf, size)) {
        bdb_offset = get32(buf + VBT_HDR_BDB_OFFSET);
        v.bdb = buf + bdb_offset;
        v.avail = get16(buf + VBT_HDR_VBT_SIZE) - bdb_offset;
        v.bdb_size = get16(v.bdb + BDB_HDR_BDB_SIZE);
        vbt->present = 1;
        vbt->source = source;
        vbt->version = get16(v.bdb + BDB_HDR_VERSION);
        k_memcpy(vbt->signature, buf, 20);
        vbt->signature[20] = '\0';

        for (i = 0; i < INTEL_BDB_BLOCKS && err == 0; i++) {
            min = bdb_blocks[i].min_size;
            if (bdb_blocks[i].id == BDB_LFP_DATA) {
                min = lfp_data_min_size(vbt);
            }
            err = init_bdb_block(vbt, &v, i, bdb_blocks[i].id, min);
        }
        if (err == 0) {
            parse_general_features(vbt);
            err = parse_general_definitions(vbt);
            parse_driver_features(vbt);
        }
    } else {
        klog(KLOG_INFO, "i915", "vbt: no video BIOS table; Linux's defaults for each port");
        err = init_vbt_missing_defaults(vbt);
    }

    for (i = 0; i < vbt->nencoders; i++) {
        parse_ddi_port(vbt, &vbt->encoders[i]);
    }
    if (vbt->present) {
        klog(KLOG_INFO, "i915", "vbt: \"%s\" from the %s, version %u, %d outputs", vbt->signature,
             source ? source : "firmware", (unsigned)vbt->version, vbt->nencoders);
    }
    return err;
}

const struct intel_bios_encoder *intel_bios_encoder_lookup(const struct intel_vbt *vbt, int port)
{
    int i;

    for (i = 0; i < vbt->nencoders; i++) {
        if (vbt->encoders[i].port == port) {
            return &vbt->encoders[i];
        }
    }
    return NULL;
}

/* ------------------------------------------------------------------------
 * The panel
 */

/**
 * The panel's index the LFP options give, Linux's vbt_get_panel_type: the
 * second panel's for the handle LFP2, -1 when the block is missing or the
 * index is neither 0 to 15 nor 0xff, which names no panel. Linux checks
 * only the first panel's index; an unchecked second one would index past
 * the tables (dev-log/78-intel/upstream.md).
 */
static int vbt_get_panel_type(const struct intel_vbt *vbt, const struct intel_bios_encoder *e)
{
    const struct intel_bdb_block *b = intel_bios_block(vbt, BDB_LFP_OPTIONS);
    uint8_t type;

    if (!b) {
        return -1;
    }
    type = b->data[LO_PANEL_TYPE];
    if (type > 0xf && type != 0xff) {
        klog(KLOG_DEBUG, "i915", "vbt: invalid panel type 0x%x", (unsigned)type);
        return -1;
    }
    if (e && e->handle == DEVICE_HANDLE_LFP2) {
        type = b->data[LO_PANEL_TYPE2];
        if (type > 0xf && type != 0xff) {
            klog(KLOG_DEBUG, "i915", "vbt: invalid second panel type 0x%x", (unsigned)type);
            return -1;
        }
    }
    return type;
}

/** The index whose PnP id matches the EDID's product id, bytes 8 to 17 of
 * its base block, Linux's pnpid_get_panel_type: a full match, else one
 * without the date where the table gives none; -1 for neither. */
static int pnpid_get_panel_type(const struct intel_vbt *vbt, const uint8_t *edid)
{
    const struct intel_bdb_block *ptrs, *data;
    const uint8_t *id;
    int i, j, best = -1, full, nodate;

    if (!edid) {
        return -1;
    }
    ptrs = intel_bios_block(vbt, BDB_LFP_DATA_PTRS);
    data = intel_bios_block(vbt, BDB_LFP_DATA);
    if (!ptrs || !data) {
        return -1;
    }
    for (i = 0; i < 16; i++) {
        id = data->data + lfp_off(ptrs->data, i, 2);
        full = nodate = 1;
        for (j = 0; j < LD_PNP_SIZE; j++) {
            if (id[j] != edid[8 + j]) {
                full = 0;
            }
            if (id[j] != (j >= 8 ? 0 : edid[8 + j])) {
                nodate = 0;
            }
        }
        if (full) {
            return i;
        }
        if (best < 0 && nodate) {
            best = i;
        }
    }
    return best;
}

/** Linux's get_panel_type without the OpRegion's answer, which counts
 * only on its quirk list's machines. */
static int get_panel_type(const struct intel_vbt *vbt, const struct intel_bios_encoder *e,
                          const uint8_t *edid, int use_fallback)
{
    int from_vbt = vbt_get_panel_type(vbt, e);
    int from_pnpid = pnpid_get_panel_type(vbt, edid);

    if (from_vbt == 0xff && from_pnpid >= 0) {
        return from_pnpid;
    }
    if (from_vbt != 0xff && from_vbt >= 0) {
        return from_vbt;
    }
    return use_fallback ? 0 : -1;
}

/** Linux's parse_panel_options. */
static void parse_panel_options(const struct intel_vbt *vbt, struct intel_vbt_panel *panel)
{
    const struct intel_bdb_block *b = intel_bios_block(vbt, BDB_LFP_OPTIONS);

    if (!b) {
        return;
    }
    panel->lvds_dither = (b->data[LO_BITS] >> 5) & 1;
    /* Tables of 4, 14, 16 and 24 bytes and more are seen, with no clear
     * tie to the version; the DRRS bits need 16. */
    if (b->raw_size < 16) {
        return;
    }
    switch (panel_bits(get32(b->data + LO_DPS_BITS), panel->panel_type, 2)) {
    case 0:
        panel->drrs_type = DRRS_TYPE_STATIC;
        break;
    case 2:
        panel->drrs_type = DRRS_TYPE_SEAMLESS;
        break;
    default:
        panel->drrs_type = DRRS_TYPE_NONE;
        break;
    }
}

/** The mode of the DTD at `d` into `panel`, Linux's
 * fill_detail_timing_data, with its trim of sync ends past the totals. */
static void fill_detail_timing_data(struct intel_vbt_panel *panel, const uint8_t *d)
{
    struct drm_mode_modeinfo *m = &panel->lfp_mode;

    k_memset(m, 0, sizeof(*m));
    m->hdisplay = (uint16_t)((d[4] >> 4) << 8 | d[2]);
    m->hsync_start = (uint16_t)(m->hdisplay + (((d[11] >> 6) & 3) << 8 | d[8]));
    m->hsync_end = (uint16_t)(m->hsync_start + (((d[11] >> 4) & 3) << 8 | d[9]));
    m->htotal = (uint16_t)(m->hdisplay + ((d[4] & 0xf) << 8 | d[3]));
    m->vdisplay = (uint16_t)((d[7] >> 4) << 8 | d[5]);
    m->vsync_start = (uint16_t)(m->vdisplay + (((d[11] >> 2) & 3) << 4 | d[10] >> 4));
    m->vsync_end = (uint16_t)(m->vsync_start + ((d[11] & 3) << 4 | (d[10] & 0xf)));
    m->vtotal = (uint16_t)(m->vdisplay + ((d[7] & 0xf) << 8 | d[6]));
    m->clock = (uint32_t)get16(d) * 10;
    m->type = DRM_MODE_TYPE_PREFERRED;
    m->flags |= (d[17] >> 6) & 1 ? DRM_MODE_FLAG_PHSYNC : DRM_MODE_FLAG_NHSYNC;
    m->flags |= (d[17] >> 5) & 1 ? DRM_MODE_FLAG_PVSYNC : DRM_MODE_FLAG_NVSYNC;
    panel->width_mm = (uint16_t)((d[14] >> 4) << 8 | d[12]);
    panel->height_mm = (uint16_t)((d[14] & 0xf) << 8 | d[13]);
    if (m->hsync_end > m->htotal) {
        m->hsync_end = m->htotal;
    }
    if (m->vsync_end > m->vtotal) {
        m->vsync_end = m->vtotal;
    }
    m->vrefresh = drm_mode_vrefresh(m);
    drm_mode_set_name(m);
    panel->has_lfp_mode = 1;
}

/** Linux's parse_lfp_data, with parse_lfp_panel_dtd. */
static void parse_lfp_data(const struct intel_vbt *vbt, struct intel_vbt_panel *panel)
{
    const struct intel_bdb_block *ptrs = intel_bios_block(vbt, BDB_LFP_DATA_PTRS);
    const struct intel_bdb_block *data = intel_bios_block(vbt, BDB_LFP_DATA);
    const uint8_t *fp, *tail;
    int t = panel->panel_type;

    if (!ptrs || !data) {
        return;
    }
    if (!panel->has_lfp_mode) {
        fill_detail_timing_data(panel, data->data + lfp_off(ptrs->data, t, 1));
        fp = data->data + lfp_off(ptrs->data, t, 0);
        /* The LVDS value only when the resolutions agree. */
        if (get16(fp + FP_X_RES) == panel->lfp_mode.hdisplay &&
            get16(fp + FP_Y_RES) == panel->lfp_mode.vdisplay) {
            panel->bios_lvds_val = get32(fp + FP_LVDS_REG_VAL);
        }
    }
    if (!ptrs->data[LP_NAME + 2]) {
        return;
    }
    tail = data->data + get16(ptrs->data + LP_NAME);
    if (vbt->version >= 188) {
        panel->seamless_drrs_min_refresh_rate = tail[LD_TAIL_DRRS_MIN + t];
    }
}

/** Linux's parse_lfp_backlight. */
static void parse_lfp_backlight(const struct intel_vbt *vbt, struct intel_vbt_panel *panel)
{
    const struct intel_bdb_block *b = intel_bios_block(vbt, BDB_LFP_BACKLIGHT);
    const uint8_t *entry;
    int t = panel->panel_type;
    uint16_t level, min_level;
    int scale;

    if (!b) {
        return;
    }
    if (b->data[BL_ENTRY_SIZE] != BL_ENTRY) {
        klog(KLOG_DEBUG, "i915", "vbt: backlight entries of %u bytes, not decoded",
             (unsigned)b->data[BL_ENTRY_SIZE]);
        return;
    }
    entry = b->data + BL_DATA + t * BL_ENTRY;
    panel->bl_present = (entry[0] & 3) == BDB_BACKLIGHT_TYPE_PWM;
    if (!panel->bl_present) {
        return;
    }
    panel->bl_controller = 0;
    if (vbt->version >= 191) {
        panel->bl_controller = b->data[BL_CONTROL + t] >> 4;
    }
    panel->bl_pwm_freq_hz = get16(entry + 1);
    panel->bl_active_low_pwm = (entry[0] >> 2) & 1;
    if (vbt->version >= 234) {
        level = get16(b->data + BL_LEVEL234 + 4 * t);
        min_level = get16(b->data + BL_MIN234 + 4 * t);
        scale = vbt->version >= 236 ? b->data[BL_PRECISION + t] == 16 : level > 255;
        if (scale) {
            min_level = (uint16_t)(min_level / 255);
        }
        if (min_level > 255) {
            klog(KLOG_WARN, "i915", "vbt: brightness minimum %u past 255", (unsigned)min_level);
        }
        panel->bl_min_brightness = (uint8_t)(min_level > 255 ? 255 : min_level);
    } else {
        panel->bl_min_brightness = entry[3];
    }
}

/** Linux's parse_panel_driver_features. */
static void parse_panel_driver_features(const struct intel_vbt *vbt, struct intel_vbt_panel *panel)
{
    const struct intel_bdb_block *b = intel_bios_block(vbt, BDB_DRIVER_FEATURES);
    uint16_t flags;

    if (!b || vbt->version >= 228) {
        return;
    }
    flags = get16(b->data + DF_FEATURE_FLAGS);
    /* The table writes static DRRS as 0 and no DRRS as drrs_enabled off. */
    if (!((flags >> 5) & 1) && panel->drrs_type != DRRS_TYPE_NONE) {
        panel->drrs_type = (flags >> 12) & 1 ? DRRS_TYPE_STATIC : DRRS_TYPE_NONE;
    }
    panel->psr_enable = (flags >> 9) & 1;
}

/** Linux's parse_power_conservation_features. */
static void parse_power_conservation_features(const struct intel_vbt *vbt,
                                              struct intel_vbt_panel *panel)
{
    const uint8_t *p;
    int t = panel->panel_type;

    panel->vrr = 1; /* as Windows */
    if (vbt->version < 228) {
        return;
    }
    if (!intel_bios_block(vbt, BDB_LFP_POWER)) {
        return;
    }
    p = intel_bios_block(vbt, BDB_LFP_POWER)->data;
    panel->psr_enable = (uint8_t)panel_bits(get16(p + LPW_PSR), t, 1);
    if (!panel_bits(get16(p + LPW_DRRS), t, 1) && panel->drrs_type != DRRS_TYPE_NONE) {
        panel->drrs_type =
            panel_bits(get16(p + LPW_DMRRS), t, 1) ? DRRS_TYPE_STATIC : DRRS_TYPE_NONE;
    }
    if (vbt->version >= 232) {
        panel->edp.hobl = (uint8_t)panel_bits(get16(p + LPW_HOBL), t, 1);
    }
    if (vbt->version >= 233) {
        panel->vrr = (uint8_t)panel_bits(get16(p + LPW_VRR), t, 1);
    }
}

/**
 * Linux's parse_edp: the link's bits a pixel, the power sequence, the fast
 * link training's rate, lanes, pre-emphasis and voltage swing, the low
 * swing of a panel, the DRRS MSA delay, the port's highest rate and DSC.
 * Unknown codes leave their field as it was, as Linux does. Linux's
 * module parameter edp_vswing, which can override the swing, is its
 * default, 0, here.
 */
static void parse_edp(const struct intel_vbt *vbt, struct intel_vbt_panel *panel)
{
    const struct intel_bdb_block *b = intel_bios_block(vbt, BDB_EDP);
    const uint8_t *e, *pps, *link;
    int t = panel->panel_type;
    uint32_t vswing;

    if (!b) {
        return;
    }
    e = b->data;
    switch (panel_bits(get32(e + EDP_COLOR_DEPTH), t, 2)) {
    case 0:
        panel->edp.bpp = 18;
        break;
    case 1:
        panel->edp.bpp = 24;
        break;
    case 2:
        panel->edp.bpp = 30;
        break;
    default:
        break;
    }
    pps = e + EDP_PPS + 10 * t;
    panel->edp.pps.t1_t3 = get16(pps);
    panel->edp.pps.t8 = get16(pps + 2);
    panel->edp.pps.t9 = get16(pps + 4);
    panel->edp.pps.t10 = get16(pps + 6);
    panel->edp.pps.t11_t12 = get16(pps + 8);

    link = e + EDP_FAST_LINK + 2 * t;
    if (vbt->version >= 224) {
        panel->edp.rate = (uint32_t)get16(e + EDP_FAST_LINK_RATE + 2 * t) * 20;
    } else {
        switch (link[0] & 0xf) {
        case 0:
            panel->edp.rate = 162000;
            break;
        case 1:
            panel->edp.rate = 270000;
            break;
        case 2:
            panel->edp.rate = 540000;
            break;
        default:
            klog(KLOG_DEBUG, "i915", "vbt: unknown eDP link rate %u", (unsigned)(link[0] & 0xf));
            break;
        }
    }
    switch (link[0] >> 4) {
    case 0:
        panel->edp.lanes = 1;
        break;
    case 1:
        panel->edp.lanes = 2;
        break;
    case 3:
        panel->edp.lanes = 4;
        break;
    default:
        klog(KLOG_DEBUG, "i915", "vbt: unknown eDP lane count %u", (unsigned)(link[0] >> 4));
        break;
    }
    /* The codes 0 to 3 are DisplayPort's levels 0 to 3 of each. */
    if ((link[1] & 0xf) <= 3) {
        panel->edp.preemphasis = link[1] & 0xf;
    }
    if ((link[1] >> 4) <= 3) {
        panel->edp.vswing = link[1] >> 4;
    }
    if (vbt->version >= 173) {
        vswing = (uint32_t)(e[EDP_VSWING_PREEMPH + t / 2] >> ((t & 1) * 4)) & 0xf;
        panel->edp.low_vswing = vswing == 0;
    }
    panel->edp.drrs_msa_timing_delay = (uint8_t)panel_bits(get32(e + EDP_SDRRS_MSA_DELAY), t, 2);
    if (vbt->version >= 244) {
        panel->edp.max_link_rate = (uint32_t)get16(e + EDP_MAX_LINK_RATE + 2 * t) * 20;
    }
    if (vbt->version >= 251) {
        panel->edp.dsc_disable = (uint8_t)panel_bits(get16(e + EDP_DSC_DISABLE), t, 1);
    }
}

void intel_bios_panel_init(const struct intel_vbt *vbt, struct intel_vbt_panel *panel,
                           const struct intel_bios_encoder *encoder, const uint8_t *edid,
                           int use_fallback)
{
    if (panel->panel_type >= 0) {
        return;
    }
    panel->panel_type = get_panel_type(vbt, encoder, edid, use_fallback);
    if (panel->panel_type < 0) {
        return;
    }
    /* Linux's init_vbt_panel_defaults. */
    panel->bl_present = 1;
    panel->lvds_dither = 1;

    parse_panel_options(vbt, panel);
    parse_lfp_data(vbt, panel);
    parse_lfp_backlight(vbt, panel);
    parse_panel_driver_features(vbt, panel);
    parse_power_conservation_features(vbt, panel);
    parse_edp(vbt, panel);
}
