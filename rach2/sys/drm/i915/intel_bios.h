/*
 * intel_bios.h: the Video BIOS Table (VBT) and the ACPI OpRegion of an Intel
 * graphics device, decoded as Linux 6.12's intel_bios.c and intel_opregion.c
 * decode them, and the defaults that stand in when there is no table.
 */
#ifndef _INTEL_BIOS_H_
#define _INTEL_BIOS_H_

#include "../../lib/kint.h"
#include "../uapi.h"

/*
 * The table describes the board: which of the chip's ports are wired to
 * what connector, the panel's timings and power sequence, the eDP link the
 * panel was validated at. The firmware leaves it in mailbox 4 of the
 * OpRegion, a block of memory whose physical address the device's ASLS
 * register holds, or at RVDA beside it, or in the option ROM of older
 * machines. Nothing here reaches the chip's registers: the decoders read
 * bytes, and `intel_opregion_setup` and `intel_bios_rom_vbt` map memory.
 *
 * The layouts are Linux's intel_vbt_defs.h and intel_opregion.c, read here
 * by byte offset rather than through packed structures and bit fields,
 * whose layout C leaves to the compiler. Every field is little-endian.
 */

/* ---- ports and channels (Linux's enum port and enum aux_ch) ---- */

#define INTEL_PORT_NONE (-1)
#define INTEL_PORT_A    0
#define INTEL_PORT_B    1
#define INTEL_PORT_C    2
#define INTEL_PORT_D    3
#define INTEL_PORT_E    4
/* Haswell's DDI ports, A to E; E carries only FDI to the PCH's VGA. */
#define INTEL_NUM_PORTS 5

#define INTEL_AUX_CH_NONE (-1)
#define INTEL_AUX_CH_A    0
#define INTEL_AUX_CH_B    1
#define INTEL_AUX_CH_C    2
#define INTEL_AUX_CH_D    3

/* ---- the child device config (Linux's DEVICE_TYPE_*, DVO_PORT_*) ---- */

#define DEVICE_HANDLE_LFP1 0x0008
#define DEVICE_HANDLE_LFP2 0x0080

#define DEVICE_TYPE_INTERNAL_CONNECTOR (1u << 12)
#define DEVICE_TYPE_NOT_HDMI_OUTPUT    (1u << 11)
#define DEVICE_TYPE_MIPI_OUTPUT        (1u << 10)
#define DEVICE_TYPE_TMDS_DVI_SIGNALING (1u << 4)
#define DEVICE_TYPE_DISPLAYPORT_OUTPUT (1u << 2)
#define DEVICE_TYPE_ANALOG_OUTPUT      (1u << 0)

#define DVO_PORT_HDMIA 0
#define DVO_PORT_HDMIB 1
#define DVO_PORT_HDMIC 2
#define DVO_PORT_HDMID 3
#define DVO_PORT_LVDS  4
#define DVO_PORT_TV    5
#define DVO_PORT_CRT   6
#define DVO_PORT_DPB   7
#define DVO_PORT_DPC   8
#define DVO_PORT_DPD   9
#define DVO_PORT_DPA   10
#define DVO_PORT_DPE   11
#define DVO_PORT_HDMIE 12

/* The bytes of a child device config Linux keeps (sizeof its struct
 * child_device_config); a shorter one in the table reads as zeros past its
 * end, a longer one is cut. */
#define INTEL_CHILD_SIZE 40

/*
 * One output the table names, Linux's struct intel_bios_encoder_data with
 * what its accessors derive. `child` is the config as the table gave it,
 * zero past `child_size`, with Linux's correction of `device_type` applied.
 * `port` is the DDI port of `dvo_port`, or INTEL_PORT_NONE. The rest are
 * the accessors' answers for the table's version:
 *
 * `ddc_pin` is the GMBUS pin of an HDMI port's DDC, 0 for none; on Haswell
 * the table's value is the pin. `aux_ch` is the AUX channel of a DP port,
 * INTEL_AUX_CH_*. `hdmi_level_shift` indexes the HDMI buffer translation
 * table, -1 before version 158. `hdmi_max_tmds_clock` is in kHz, 0 for the
 * platform's limit. `dp_max_link_rate` is in kHz and `dp_max_lane_count` a
 * count, 0 when the table does not limit them. The boost levels are 0
 * unless the table asks for an I_boost.
 */
struct intel_bios_encoder {
    uint8_t child[INTEL_CHILD_SIZE];
    uint16_t handle;
    uint16_t device_type;
    uint8_t dvo_port;
    int8_t port;
    uint8_t ddc_pin;
    int8_t aux_ch;
    int8_t hdmi_level_shift;
    uint32_t hdmi_max_tmds_clock;
    uint32_t dp_max_link_rate;
    uint8_t dp_max_lane_count;
    uint8_t dp_boost_level;
    uint8_t hdmi_boost_level;
    uint8_t lane_reversal;
    uint8_t hpd_invert;
};

/* ---- the blocks Linux reads (enum bdb_block_id) ---- */

#define BDB_GENERAL_FEATURES    1
#define BDB_GENERAL_DEFINITIONS 2
#define BDB_DRIVER_FEATURES     12
#define BDB_EDP                 27
#define BDB_LFP_OPTIONS         40
#define BDB_LFP_DATA_PTRS       41
#define BDB_LFP_DATA            42
#define BDB_LFP_BACKLIGHT       43
#define BDB_LFP_POWER           44

/* A block as `intel_bios_init` keeps it: its bytes, copied from the table
 * into `size` bytes, the larger of the block's own size and the size
 * Linux's structure for it has, the rest zero; `raw_size` is the block's own
 * size. `data` is NULL for a block the table lacks. */
struct intel_bdb_block {
    uint8_t id;
    uint8_t *data;
    uint32_t size;
    uint32_t raw_size;
};

#define INTEL_BDB_BLOCKS 9

/*
 * What the table says of the device, Linux's struct intel_vbt_data, before
 * any panel is looked at. `present` is set when a valid table was decoded,
 * and the fields hold Linux's defaults otherwise. `version` is the BDB
 * version, 155 for the defaults made without a table, as Linux sets it.
 * `source` names where the table was found: "OpRegion (Mailbox #4)",
 * "OpRegion (RVDA)", "PCI ROM" or NULL.
 *
 * `int_tv_support`, `int_crt_support`, `int_lvds_support`, `lvds_use_ssc`,
 * `display_clock_mode` and `fdi_rx_polarity_inverted` are the flags of the
 * general and driver features blocks; `lvds_ssc_freq` is in kHz;
 * `orientation` is a DRM_MODE_PANEL_ORIENTATION_* value; `crt_ddc_pin` the
 * GMBUS pin of the VGA connector's DDC. `encoders` holds `nencoders`
 * outputs in the table's order, from the heap.
 */
struct intel_vbt {
    uint8_t present;
    uint16_t version;
    const char *source;
    char signature[21];
    uint8_t int_tv_support;
    uint8_t int_crt_support;
    uint8_t int_lvds_support;
    uint8_t lvds_use_ssc;
    uint32_t lvds_ssc_freq;
    uint8_t display_clock_mode;
    uint8_t fdi_rx_polarity_inverted;
    int8_t orientation;
    uint8_t crt_ddc_pin;
    uint8_t child_size;
    int nencoders;
    struct intel_bios_encoder *encoders;
    struct intel_bdb_block blocks[INTEL_BDB_BLOCKS];
};

/* The panel orientations of DRM (drm_connector.h). */
#define DRM_MODE_PANEL_ORIENTATION_UNKNOWN   (-1)
#define DRM_MODE_PANEL_ORIENTATION_NORMAL    0
#define DRM_MODE_PANEL_ORIENTATION_BOTTOM_UP 1

/* How a panel may change its refresh rate (Linux's enum drrs_type). */
#define DRRS_TYPE_NONE     0
#define DRRS_TYPE_STATIC   1
#define DRRS_TYPE_SEAMLESS 2

/*
 * An eDP panel's power sequence, in units of 100 us: T1+T3, the power up
 * to the panel's AUX channel; T8, the link's video to the backlight on; T9,
 * the backlight off to the link's video off; T10, the video off to the
 * power down; T11+T12, the power down to the next power up. Linux's
 * struct edp_power_seq, as the table gives it.
 */
struct intel_edp_power_seq {
    uint16_t t1_t3;
    uint16_t t8;
    uint16_t t9;
    uint16_t t10;
    uint16_t t11_t12;
};

/*
 * What the table says of one panel, Linux's panel->vbt. `panel_type` is
 * the table's index of it, 0 to 15, or -1 before `intel_bios_panel_init`
 * chose one. `lfp_mode` is the panel's mode from the LFP data block, valid
 * when `has_lfp_mode` is set, with its size in millimetres in `width_mm`
 * and `height_mm`; Linux takes the panel's own EDID modes first.
 * `bios_lvds_val` is the LVDS register's value the table gives, for LVDS
 * panels. `seamless_drrs_min_refresh_rate` is in Hz.
 *
 * The eDP settings: `bpp` the bits a pixel of the link carries, 18, 24 or
 * 30, 0 when unset; `pps` the power sequence; `rate` the fast link
 * training's rate in kHz and `lanes` its lane count; `preemphasis` and
 * `vswing` DisplayPort's training levels, 0 to 3; `low_vswing` set for a
 * panel of low voltage swing; `drrs_msa_timing_delay` 0 to 3;
 * `max_link_rate` in kHz, 0 when unset.
 *
 * The backlight: `bl_present` whether the panel's backlight is a PWM the
 * display drives; `bl_pwm_freq_hz`, `bl_active_low_pwm`, `bl_min_brightness`
 * (0 to 255) and `bl_controller` its settings.
 */
struct intel_vbt_panel {
    int panel_type;
    uint8_t lvds_dither;
    uint8_t drrs_type;
    uint8_t has_lfp_mode;
    struct drm_mode_modeinfo lfp_mode;
    uint16_t width_mm;
    uint16_t height_mm;
    uint32_t bios_lvds_val;
    uint8_t seamless_drrs_min_refresh_rate;
    uint8_t psr_enable;
    uint8_t vrr;
    struct {
        uint8_t bpp;
        struct intel_edp_power_seq pps;
        uint32_t rate;
        uint8_t lanes;
        uint8_t preemphasis;
        uint8_t vswing;
        uint8_t low_vswing;
        uint8_t drrs_msa_timing_delay;
        uint32_t max_link_rate;
        uint8_t hobl;
        uint8_t dsc_disable;
    } edp;
    uint8_t bl_present;
    uint16_t bl_pwm_freq_hz;
    uint8_t bl_active_low_pwm;
    uint8_t bl_min_brightness;
    uint8_t bl_controller;
};

/* ---- the OpRegion (intel_opregion.c) ---- */

#define OPREGION_SIZE            0x2000u /* 8 KiB */
#define OPREGION_ACPI_OFFSET     0x100u
#define OPREGION_SWSCI_OFFSET    0x200u
#define OPREGION_ASLE_OFFSET     0x300u
#define OPREGION_VBT_OFFSET      0x400u
#define OPREGION_ASLE_EXT_OFFSET 0x1c00u

/* The mailboxes of the header's MBOX field. */
#define MBOX_ACPI      (1u << 0)
#define MBOX_SWSCI     (1u << 1)
#define MBOX_ASLE      (1u << 2)
#define MBOX_ASLE_EXT  (1u << 4)
#define MBOX_BACKLIGHT (1u << 5)

/*
 * The OpRegion as `intel_opregion_setup` found it. `base` is where its
 * OPREGION_SIZE bytes are mapped, NULL when the device has none; `phys`
 * is ASLS. `major`, `minor` and `revision` are its version, and `mboxes`
 * the header's mailbox bits. `acpi`, `swsci`, `asle` and `asle_ext` point
 * into `base` at the mailboxes the header names and the driver uses, NULL
 * for the rest: Linux leaves mailbox 2 alone on a version 3 OpRegion.
 * `vbt` and `vbt_size` are the VBT the OpRegion holds, in `base` or in the
 * window `rvda` of `rvda_size` bytes, or NULL.
 */
struct intel_opregion {
    uint8_t *base;
    uint64_t phys;
    uint8_t major;
    uint8_t minor;
    uint8_t revision;
    uint32_t mboxes;
    uint8_t *acpi;
    uint8_t *swsci;
    uint8_t *asle;
    uint8_t *asle_ext;
    const uint8_t *vbt;
    uint32_t vbt_size;
    uint8_t *rvda;
    uint32_t rvda_size;
};

/**
 * Find the OpRegion at the physical address `asls`, the device's ASLS
 * register, and check it as Linux's intel_opregion_setup does: map its 8
 * KiB, refuse a bad signature, note its version and the mailboxes its
 * header names, tell the firmware through mailbox 1 that the driver takes
 * hotplug events itself and through mailbox 3 that it is not ready for
 * ASLE requests, and find the VBT at RVDA or in mailbox 4. The memory is
 * mapped uncached, as ACPICA maps the same pages for the firmware's AML.
 *
 * @return 0 with `op` filled, its `vbt` NULL when no valid VBT is in it;
 *         -ENODEV when `asls` is 0; -ENOMEM when it cannot be mapped;
 *         -EINVAL for a bad signature. `op` is left empty on an error.
 */
int32_t intel_opregion_setup(struct intel_opregion *op, uint32_t asls);

/** Unmap what `intel_opregion_setup` mapped and empty `op`. */
void intel_opregion_fini(struct intel_opregion *op);

/* ---- the VBT (intel_bios.c) ---- */

/**
 * Whether the `size` bytes at `buf` hold a whole VBT, as Linux's
 * intel_bios_is_valid_vbt judges: the "$VBT" signature, a size within
 * `size`, and a BDB header and BDB that fit inside it. The checksum is not
 * judged, as Linux does not.
 */
int intel_bios_is_valid_vbt(const uint8_t *buf, uint32_t size);

/**
 * The VBT in the `size` bytes of an option ROM at `rom`, found as Linux's
 * oprom_get_vbt finds it, by its signature on a 4-byte boundary: a copy
 * from the heap of its `*vbt_size` bytes, which the caller frees, or NULL
 * when there is none, it is incomplete or invalid, or the heap is out.
 */
uint8_t *intel_bios_rom_vbt(const uint8_t *rom, uint32_t size, uint32_t *vbt_size);

/**
 * Decode the VBT of `size` bytes at `buf`, found at `source`, into `vbt`
 * as Linux's intel_bios_init does for a Haswell display: Linux's defaults
 * first, then the blocks it reads, each copied and padded to Linux's size
 * for it; the general features, the general definitions with their child
 * devices, and the driver features; then each child's DDI port, with
 * Linux's corrections. With `buf` NULL or a table that is not valid, the
 * defaults stand and an output is made for each of Haswell's ports, eDP
 * on DDI A, as Linux's init_vbt_missing_defaults makes them.
 *
 * `vbt` must be zeroed or emptied by `intel_bios_fini`. Nothing of `buf` is
 * kept. Logs one line of what it found.
 *
 * @return 0, or -ENOMEM when the heap is out, with `vbt` holding what was
 *         decoded before.
 */
int32_t intel_bios_init(struct intel_vbt *vbt, const uint8_t *buf, uint32_t size,
                        const char *source);

/** Free what `intel_bios_init` took from the heap and empty `vbt`. */
void intel_bios_fini(struct intel_vbt *vbt);

/** The block `id` of the table, Linux's bdb_find_section, or NULL. */
const struct intel_bdb_block *intel_bios_block(const struct intel_vbt *vbt, uint8_t id);

/**
 * Choose the panel's index in the table and decode what the table says of
 * it, as Linux's intel_bios_init_panel: the LFP options, the LFP data and
 * its mode, the backlight, the driver features, the power conservation
 * features and the eDP block. `encoder` is the panel's output, for its
 * handle, or NULL. `edid` is the panel's EDID base block, 128 bytes, or
 * NULL before it is read; with it the panel's product id may choose the
 * index. Without `use_fallback` (Linux's early call) a table that names no
 * panel leaves `panel_type` -1 and the rest untouched; with it (the late
 * call) index 0 is taken. A panel already chosen is left as it is.
 *
 * The OpRegion's own panel type, which Linux asks the firmware for through
 * SWSCI and then uses only on machines of a quirk list, is not asked for.
 */
void intel_bios_panel_init(const struct intel_vbt *vbt, struct intel_vbt_panel *panel,
                           const struct intel_bios_encoder *encoder, const uint8_t *edid,
                           int use_fallback);

/* What an output can carry, Linux's intel_bios_encoder_supports_*. */
int intel_bios_encoder_supports_crt(const struct intel_bios_encoder *e);
int intel_bios_encoder_supports_dvi(const struct intel_bios_encoder *e);
int intel_bios_encoder_supports_hdmi(const struct intel_bios_encoder *e);
int intel_bios_encoder_supports_dp(const struct intel_bios_encoder *e);
int intel_bios_encoder_supports_edp(const struct intel_bios_encoder *e);
int intel_bios_encoder_supports_dsi(const struct intel_bios_encoder *e);
int intel_bios_encoder_supports_dp_dual_mode(const struct intel_bios_encoder *e);

/** The output the table names for `port`, Linux's
 * intel_bios_encoder_data_lookup, or NULL. */
const struct intel_bios_encoder *intel_bios_encoder_lookup(const struct intel_vbt *vbt, int port);

#endif /* _INTEL_BIOS_H_ */
