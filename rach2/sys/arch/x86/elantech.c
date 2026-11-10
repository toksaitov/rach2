/*
 * elantech.c: the Elantech touchpad on the PS/2 mouse's port, set up and
 * read as Linux's psmouse and elantech drivers do (elantech.h).
 *
 * The pad talks the PS/2 mouse's commands. It answers the magic knock, 1:1
 * scaling three times and a status request (0xE9), with 0x3C 0x03 and 0xC8
 * or 0x00, and Synaptics' sliced queries, 1:1 scaling followed by four
 * resolutions of two bits each and a status request, with three bytes. Its
 * firmware version, the query 0x01, gives the hardware version: 1 for the
 * oldest firmware, 2 for an IC body of 2 or 4, 3 for 5 and 4 for 6 to 15.
 * Versions 3 and 4 take a query as 0xF8, the query, and a status request,
 * and every version has registers, read and written by sequences of its
 * own, which set absolute mode.
 *
 * In absolute mode version 1 sends four-byte packets with three parity
 * bits, of one finger's place and the finger count; version 2 six-byte
 * packets of one finger's place, pressure and width, or of two fingers'
 * places at a lower resolution; version 3 the same, but for two fingers a
 * head and a tail packet, one for each; and version 4 a head packet that
 * places one of up to five fingers, motion packets of two fingers' moves,
 * and status packets that say which fingers touch. A pointing stick on
 * versions 3 and 4 sends six-byte packets of its own into the same stream.
 * Y grows upward and is turned over for the event device.
 * Documentation/input/devices/elantech.rst in Linux's tree has the bits.
 */

#include "elantech.h"

#include "ps2mouse.h"
#include "smbios.h"
#include "../../evdev.h"
#include "../../klog.h"
#include "../../lib/kstring.h"
#include "../../mouse.h"
#include "../../timer.h"
#include "../../udelay.h"

/* The mouse commands the pad's requests are made of. */
#define CMD_SET_SCALE11 0xE6
#define CMD_SET_RES     0xE8
#define CMD_GET_INFO    0xE9 /* three bytes after the ACK */
#define CMD_SET_RATE    0xF3
#define CMD_DISABLE     0xF5
#define CMD_RESET_DIS   0xF6
#define CMD_CUSTOM      0xF8 /* the prefix of a query or register access */
#define CMD_RESET       0xFF /* 0xAA and the ID 0x00 after the ACK */

/* The queries, Linux's ETP_*_QUERY. */
#define QUERY_FW_ID      0x00
#define QUERY_FW_VERSION 0x01
#define QUERY_CAPS       0x02
#define QUERY_SAMPLE     0x03
#define QUERY_RESOLUTION 0x04
#define QUERY_ICBODY     0x05

/* The register accesses' codes. */
#define REG_READ      0x10
#define REG_WRITE     0x11
#define REG_READWRITE 0x00

/* A retried command's tries and the wait between them, and the reads of
 * register 0x10 after the mode is set, in milliseconds. */
#define COMMAND_TRIES   3
#define COMMAND_DELAY   500
#define READ_BACK_TRIES 5
#define READ_BACK_DELAY 2000

/* Version 1's register 0x10 bit of absolute mode, and the capabilities'
 * bit of a rocker switch. */
#define R10_ABSOLUTE_MODE 0x04
#define CAP_HAS_ROCKER    0x04

/* The ranges of the versions that do not give theirs: version 1's from an
 * application note, less the bezel's fuzz, and older version 2's. */
#define EDGE_FUZZ_V1 32
#define XMIN_V1      (0 + EDGE_FUZZ_V1)
#define XMAX_V1      (576 - EDGE_FUZZ_V1)
#define YMIN_V1      (0 + EDGE_FUZZ_V1)
#define YMAX_V1      (384 - EDGE_FUZZ_V1)
#define XMAX_V2      1152
#define YMAX_V2      768
#define PMAX_V2      255
#define WMAX_V2      15

/* The kinds of a packet, Linux's PACKET_*. */
#define PACKET_UNKNOWN    1
#define PACKET_DEBOUNCE   2
#define PACKET_V3_HEAD    3
#define PACKET_V3_TAIL    4
#define PACKET_V4_HEAD    5
#define PACKET_V4_MOTION  6
#define PACKET_V4_STATUS  7
#define PACKET_TRACKPOINT 8

/* The fingers version 4 tracks, and the weight of a motion packet's move
 * when its bit 4 is set. */
#define MAX_FINGERS  5
#define WEIGHT_VALUE 5

/* The resolution query's third byte: the buses the pad offers. */
#define BUS_SMB_HST_NTFY_ONLY 2
#define BUS_PS2_SMB_HST_NTFY  4

/* A new IC that offers SMBus Host Notify: an IC body of 15, the bit of
 * SMBus notify and a firmware minor above 0. */
#define NEW_IC_SMBUS_HOST_NOTIFY(v) ((((v) & 0x0f2000u) == 0x0f2000u) && ((v) & 0xffu) > 0)

/* The protocol's number in Linux's `enum psmouse_type`, which EVIOCGID
 * gives as the product, and the vendor psmouse gives every mouse. */
#define PSMOUSE_ELANTECH 14
#define PSMOUSE_VENDOR   0x0002

/* What the queries found, Linux's `struct elantech_device_info`. */
struct elantech_info {
    uint8_t capabilities[3];
    uint8_t samples[3];
    int hw_version;
    uint8_t pattern;
    uint32_t fw_version;
    uint32_t ic_version;
    uint32_t product_id;
    uint32_t x_min, y_min, x_max, y_max;
    uint32_t x_res, y_res; /* units a millimetre */
    uint32_t width;        /* units a trace, version 4 */
    uint32_t bus;
    int jumpy_cursor;
    int reports_pressure;
    int crc_enabled;
    int set_hw_resolution;
    int has_trackpoint;
    int has_middle_button;
};

/* A finger's place, version 3's first and version 4's five. */
struct finger_pos {
    int x;
    int y;
};

static struct elantech_info etd_info;
static uint8_t etd_reg_07;    /* version 4's mode, written again after a rate */
static struct evdev *etd_pad; /* the touchpad's event device */
static struct evdev *etd_tp;  /* the pointing stick's, or NULL */
static unsigned int etd_single_finger_reports;
static struct finger_pos etd_mt[MAX_FINGERS];

/* The packet being assembled: its bytes, how many have come, its size and
 * the tick of the last. */
static uint8_t etd_packet[6];
static int etd_pktcnt;
static int etd_pktsize;
static uint32_t etd_last_tick;

/* The packets dropped for constant bits or parity out of form or for a
 * pause, which psmouse logs as lost synchronization. */
static unsigned int etd_sync_losses;

/** Send `cmd` and return 0 when the pad acknowledged it, -1 when not, as
 * Linux's `ps2_command` with no data. */
static int etd_command(uint8_t cmd)
{
    return ps2mouse_send(cmd) == PS2MOUSE_ACK ? 0 : -1;
}

/** Send a status request and read the three bytes that follow its ACK
 * into `param`; 0, or -1 when one did not come. */
static int etd_getinfo(uint8_t *param)
{
    int i, b;

    if (etd_command(CMD_GET_INFO) < 0) {
        return -1;
    }
    for (i = 0; i < 3; i++) {
        b = ps2mouse_receive();
        if (b < 0) {
            return -1;
        }
        param[i] = (uint8_t)b;
    }
    return 0;
}

/** Send `cmd`, or with a non-NULL `param` a status request whose reply it
 * takes, up to COMMAND_TRIES times half a second apart, as Linux's
 * `elantech_ps2_command`. */
static int etd_ps2_command(uint8_t *param, uint8_t cmd)
{
    int tries;

    for (tries = 0; tries < COMMAND_TRIES; tries++) {
        if (tries > 0) {
            k_udelay(COMMAND_DELAY * 1000u);
        }
        if ((param ? etd_getinfo(param) : etd_command(cmd)) == 0) {
            return 0;
        }
    }
    klog(KLOG_WARN, "elantech", "command: 0x%x failed", cmd);
    return -1;
}

/** Send `c` as Linux's `ps2_sliced_command`: 1:1 scaling, then four
 * resolutions, two bits of `c` each from the top. */
static int etd_sliced(uint8_t c)
{
    int i;

    if (etd_command(CMD_SET_SCALE11) < 0) {
        return -1;
    }
    for (i = 6; i >= 0; i -= 2) {
        if (etd_command(CMD_SET_RES) < 0 || etd_command((uint8_t)((c >> i) & 3u)) < 0) {
            return -1;
        }
    }
    return 0;
}

/** Ask the query `c` as a Synaptics sliced query, Linux's
 * `synaptics_send_cmd`, and read its three bytes into `param`. */
static int etd_synaptics_send_cmd(uint8_t c, uint8_t *param)
{
    if (etd_sliced(c) < 0 || etd_getinfo(param) < 0) {
        klog(KLOG_WARN, "elantech", "query: 0x%x failed", c);
        return -1;
    }
    return 0;
}

/** Ask the query `c` with the custom prefix of versions 3 and 4, Linux's
 * `elantech_send_cmd`, and read its three bytes into `param`. */
static int etd_elantech_send_cmd(uint8_t c, uint8_t *param)
{
    if (etd_command(CMD_CUSTOM) < 0 || etd_command(c) < 0 || etd_getinfo(param) < 0) {
        klog(KLOG_WARN, "elantech", "query: 0x%x failed", c);
        return -1;
    }
    return 0;
}

/** Ask the query `c` in the form the hardware version takes. */
static int etd_send_cmd(uint8_t c, uint8_t *param)
{
    return etd_info.hw_version >= 3 ? etd_elantech_send_cmd(c, param)
                                    : etd_synaptics_send_cmd(c, param);
}

/** Whether register `reg` is one Linux's accesses allow: 0x07 to 0x11 and
 * 0x20 to 0x26. */
static int etd_reg_ok(uint8_t reg)
{
    return reg >= 0x07 && reg <= 0x26 && !(reg > 0x11 && reg < 0x20);
}

/** Read register `reg` into `*val`, as Linux's `elantech_read_reg` for each
 * hardware version: version 4 gives the value in the second byte, the
 * others in the first. */
static int etd_read_reg(uint8_t reg, uint8_t *val)
{
    uint8_t param[3];
    int rc = 0;

    if (!etd_reg_ok(reg)) {
        return -1;
    }
    switch (etd_info.hw_version) {
    case 1:
        if (etd_sliced(REG_READ) < 0 || etd_sliced(reg) < 0 || etd_getinfo(param) < 0) {
            rc = -1;
        }
        break;
    case 2:
        if (etd_ps2_command(0, CMD_CUSTOM) || etd_ps2_command(0, REG_READ) ||
            etd_ps2_command(0, CMD_CUSTOM) || etd_ps2_command(0, reg) ||
            etd_ps2_command(param, CMD_GET_INFO)) {
            rc = -1;
        }
        break;
    default:
        if (etd_ps2_command(0, CMD_CUSTOM) || etd_ps2_command(0, REG_READWRITE) ||
            etd_ps2_command(0, CMD_CUSTOM) || etd_ps2_command(0, reg) ||
            etd_ps2_command(param, CMD_GET_INFO)) {
            rc = -1;
        }
        break;
    }
    if (rc) {
        klog(KLOG_WARN, "elantech", "register: 0x%x would not read", reg);
    } else {
        *val = etd_info.hw_version != 4 ? param[0] : param[1];
    }
    return rc;
}

/** Write `val` to register `reg`, as Linux's `elantech_write_reg` for each
 * hardware version. */
static int etd_write_reg(uint8_t reg, uint8_t val)
{
    int rc = 0;

    if (!etd_reg_ok(reg)) {
        return -1;
    }
    switch (etd_info.hw_version) {
    case 1:
        if (etd_sliced(REG_WRITE) < 0 || etd_sliced(reg) < 0 || etd_sliced(val) < 0 ||
            etd_command(CMD_SET_SCALE11) < 0) {
            rc = -1;
        }
        break;
    case 2:
    case 3:
        if (etd_ps2_command(0, CMD_CUSTOM) ||
            etd_ps2_command(0, etd_info.hw_version == 2 ? REG_WRITE : REG_READWRITE) ||
            etd_ps2_command(0, CMD_CUSTOM) || etd_ps2_command(0, reg) ||
            etd_ps2_command(0, CMD_CUSTOM) || etd_ps2_command(0, val) ||
            etd_ps2_command(0, CMD_SET_SCALE11)) {
            rc = -1;
        }
        break;
    default:
        if (etd_ps2_command(0, CMD_CUSTOM) || etd_ps2_command(0, REG_READWRITE) ||
            etd_ps2_command(0, CMD_CUSTOM) || etd_ps2_command(0, reg) ||
            etd_ps2_command(0, CMD_CUSTOM) || etd_ps2_command(0, REG_READWRITE) ||
            etd_ps2_command(0, CMD_CUSTOM) || etd_ps2_command(0, val) ||
            etd_ps2_command(0, CMD_SET_SCALE11)) {
            rc = -1;
        }
        break;
    }
    if (rc) {
        klog(KLOG_WARN, "elantech", "register: 0x%x would not take 0x%x", reg, val);
    }
    return rc;
}

/** Whether the pad is a clickpad, whose pad is its button, as Linux's
 * `elantech_is_buttonpad` tells it by bit 12 of the firmware version. */
static int etd_is_buttonpad(void)
{
    return (etd_info.fw_version & 0x001000u) != 0;
}

/** Whether this machine is the Fujitsu `name`, as Linux's DMI tables
 * match it. */
static int etd_fujitsu(const char *name)
{
    return smbios_system_is("FUJITSU", name);
}

/** Linux's `elantech_dmi_has_middle_button`: Fujitsu machines whose version
 * 4 pad has a middle button. */
static int etd_dmi_has_middle_button(void)
{
    return etd_fujitsu("CELSIUS H730") || etd_fujitsu("CELSIUS H760") ||
           etd_fujitsu("CELSIUS H780");
}

/** Linux's `elantech_dmi_force_crc_enabled`: Fujitsu machines whose version
 * 4 pad sends the CRC form of packets whatever its firmware says. */
static int etd_dmi_force_crc_enabled(void)
{
    static const char *const names[] = {"CELSIUS H730",  "CELSIUS H760",  "LIFEBOOK E544",
                                        "LIFEBOOK E546", "LIFEBOOK E547", "LIFEBOOK E554",
                                        "LIFEBOOK E556", "LIFEBOOK E557", "LIFEBOOK U745"};
    unsigned int i;

    for (i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        if (etd_fujitsu(names[i])) {
            return 1;
        }
    }
    return 0;
}

/**
 * The hardware version and what follows from the firmware version, as
 * Linux's `elantech_set_properties`. Returns -1 for an IC body Linux does
 * not know.
 */
static int etd_set_properties(struct elantech_info *info)
{
    info->ic_version = (info->fw_version & 0x0f0000u) >> 16;
    if (info->fw_version < 0x020030u || info->fw_version == 0x020600u) {
        info->hw_version = 1; /* early pads do not follow the rule */
    } else {
        switch (info->ic_version) {
        case 2:
        case 4:
            info->hw_version = 2;
            break;
        case 5:
            info->hw_version = 3;
            break;
        default:
            if (info->ic_version < 6) {
                return -1;
            }
            info->hw_version = 4;
            break;
        }
    }
    info->pattern = 0;
    if (info->ic_version == 0x0f && (info->fw_version & 0xffu) <= 0x02) {
        info->pattern = (uint8_t)(info->fw_version & 0xffu);
    }
    /* This firmware places the first touch wrong; its first two reports
     * of one finger are dropped. */
    info->jumpy_cursor = info->fw_version == 0x020022u || info->fw_version == 0x020600u;
    if (info->hw_version > 1 && info->fw_version >= 0x020800u) {
        info->reports_pressure = 1;
    }
    info->crc_enabled = (info->fw_version & 0x4000u) == 0x4000u || etd_dmi_force_crc_enabled();
    /* A Gigabyte U2442's version 3 pad fails when bits 3 and 1 of register
     * 0x10 are set. */
    info->set_hw_resolution = !smbios_system_is("GIGABYTE", "U2442");
    return 0;
}

/** A resolution query's four bits as units a millimetre, Linux's
 * `elantech_convert_res`: the value times 10 plus 790 is dots an inch. */
static uint32_t etd_convert_res(uint32_t val)
{
    return (val * 10u + 790u) * 10u / 254u;
}

/**
 * Linux's `elantech_change_report_id`: set a pointing stick's reports from
 * ID 0x5E to 0x5F by writing register 0x07's two bytes, 0x10 and 0x03, as
 * `elantech_write_reg_params` does, and read them back.
 */
static int etd_change_report_id(void)
{
    uint8_t param[3];

    if (etd_ps2_command(0, CMD_CUSTOM) || etd_ps2_command(0, REG_READWRITE) ||
        etd_ps2_command(0, CMD_CUSTOM) || etd_ps2_command(0, 0x07) ||
        etd_ps2_command(0, CMD_CUSTOM) || etd_ps2_command(0, 0x10) ||
        etd_ps2_command(0, CMD_CUSTOM) || etd_ps2_command(0, 0x03) ||
        etd_ps2_command(0, CMD_SET_SCALE11) || etd_ps2_command(0, CMD_CUSTOM) ||
        etd_ps2_command(0, REG_READWRITE) || etd_ps2_command(0, CMD_CUSTOM) ||
        etd_ps2_command(0, 0x07) || etd_ps2_command(param, CMD_GET_INFO) || param[0] != 0x10 ||
        param[1] != 0x03) {
        klog(KLOG_WARN, "elantech", "query: the pointing stick's report ID would not change");
        return -1;
    }
    return 0;
}

/**
 * The first queries of Linux's `elantech_query_info`: the firmware version
 * again, the capabilities, the samples, the IC body of a pad with a
 * pattern, and a pointing stick's report ID where its firmware needs it.
 * Returns -1 when one failed or the pad is one whose absolute mode Linux
 * does not use.
 */
static int etd_query_ids(struct elantech_info *info)
{
    uint8_t param[3];

    if (etd_synaptics_send_cmd(QUERY_FW_VERSION, param) < 0) {
        return -1;
    }
    info->fw_version = (uint32_t)param[0] << 16 | (uint32_t)param[1] << 8 | param[2];
    if (etd_set_properties(info) < 0) {
        klog(KLOG_WARN, "elantech", "query: unknown hardware, firmware 0x%x", info->fw_version);
        return -1;
    }
    if (etd_send_cmd(QUERY_CAPS, info->capabilities) < 0) {
        return -1;
    }
    if (info->hw_version != 1 && etd_send_cmd(QUERY_SAMPLE, info->samples) < 0) {
        return -1;
    }
    if (info->pattern > 0 && info->ic_version == 0x0f) {
        if (etd_send_cmd(QUERY_ICBODY, param) < 0) {
            return -1;
        }
        info->ic_version = (uint32_t)param[0] << 8 | param[1];
    }
    info->product_id = (uint32_t)info->samples[0] << 8 | info->samples[1];
    if (info->pattern == 0) {
        info->product_id &= 0xffu;
    }
    if (info->samples[1] == 0x74 && info->hw_version == 3) {
        klog(KLOG_WARN, "elantech", "query: absolute mode broken, set up as a mouse");
        return -1;
    }
    info->has_trackpoint = (info->capabilities[0] & 0x80u) != 0;
    if (info->has_trackpoint && info->ic_version == 0x0011 &&
        (info->product_id == 0x08 || info->product_id == 0x09 || info->product_id == 0x0d ||
         info->product_id == 0x0e)) {
        return etd_change_report_id();
    }
    return 0;
}

/**
 * The range of a version 2 pad, as Linux's `elantech_query_info` works it
 * out: fixed for three firmwares, else from the firmware ID, the samples of
 * a fixed-DPI pad, or the traces in the capabilities. Returns -1 when a
 * query failed.
 */
static int etd_query_range_v2(struct elantech_info *info)
{
    uint8_t param[3];
    uint32_t i;
    int fixed_dpi;

    if (info->fw_version == 0x020800u || info->fw_version == 0x020b00u ||
        info->fw_version == 0x020030u) {
        info->x_max = XMAX_V2;
        info->y_max = YMAX_V2;
        return 0;
    }
    i = info->fw_version > 0x020800u && info->fw_version < 0x020900u ? 1 : 2;
    if (etd_send_cmd(QUERY_FW_ID, param) < 0) {
        return -1;
    }
    fixed_dpi = param[1] & 0x10;
    if ((info->fw_version >> 16) == 0x14 && fixed_dpi) {
        if (etd_send_cmd(QUERY_SAMPLE, param) < 0) {
            return -1;
        }
        info->x_max = (info->capabilities[1] - i) * param[1] / 2;
        info->y_max = (info->capabilities[2] - i) * param[2] / 2;
    } else if (info->fw_version == 0x040216u) {
        info->x_max = 819;
        info->y_max = 405;
    } else if (info->fw_version == 0x040219u || info->fw_version == 0x040215u) {
        info->x_max = 900;
        info->y_max = 500;
    } else {
        info->x_max = (info->capabilities[1] - i) * 64;
        info->y_max = (info->capabilities[2] - i) * 64;
    }
    return 0;
}

/**
 * The range of a version 3 or 4 pad from the firmware ID, and on version 4
 * the width of a trace. Returns -1 when the query failed or the traces do
 * not fit the range.
 */
static int etd_query_range_v3(struct elantech_info *info)
{
    uint8_t param[3];
    uint8_t traces;

    if (etd_send_cmd(QUERY_FW_ID, param) < 0) {
        return -1;
    }
    info->x_max = (uint32_t)(param[0] & 0x0fu) << 8 | param[1];
    info->y_max = (uint32_t)(param[0] & 0xf0u) << 4 | param[2];
    if (info->hw_version == 3) {
        return 0;
    }
    traces = info->capabilities[1];
    if (traces < 2 || traces > info->x_max) {
        klog(KLOG_WARN, "elantech", "query: %u traces across %u units", traces, info->x_max);
        return -1;
    }
    info->width = info->x_max / (traces - 1u);
    return 0;
}

/**
 * Every query, as Linux's `elantech_query_info`: the identities, the
 * resolution of version 4, each version's range and the middle button.
 * Returns -1 when one that Linux needs failed or the pad is one whose
 * absolute mode Linux does not use.
 */
static int etd_query_info(struct elantech_info *info)
{
    uint8_t param[3];

    k_memset(info, 0, sizeof(*info));
    if (etd_query_ids(info) < 0) {
        return -1;
    }
    info->x_res = 31;
    info->y_res = 31;
    if (info->hw_version == 4) {
        if (etd_elantech_send_cmd(QUERY_RESOLUTION, param) < 0) {
            klog(KLOG_WARN, "elantech", "query: the resolution would not read");
        } else {
            info->x_res = etd_convert_res(param[1] & 0x0fu);
            info->y_res = etd_convert_res((param[1] & 0xf0u) >> 4);
            info->bus = param[2];
        }
    }
    if (info->hw_version == 1) {
        info->x_min = XMIN_V1;
        info->y_min = YMIN_V1;
        info->x_max = XMAX_V1;
        info->y_max = YMAX_V1;
    } else if ((info->hw_version == 2 ? etd_query_range_v2(info) : etd_query_range_v3(info)) < 0) {
        return -1;
    }
    info->has_middle_button = etd_dmi_has_middle_button() ||
                              (NEW_IC_SMBUS_HOST_NOTIFY(info->fw_version) && !etd_is_buttonpad());
    return 0;
}

/**
 * Put the pad in absolute mode, as Linux's `elantech_set_absolute_mode`:
 * the registers the Windows driver writes for each version, then register
 * 0x10 read back until the pad answers, and on version 1 checked for the
 * mode's bit. Version 4 has no register 0x10 and is not read back.
 */
static int etd_set_absolute_mode(void)
{
    int tries = READ_BACK_TRIES;
    uint8_t val = 0;
    int rc = 0;

    switch (etd_info.hw_version) {
    case 1:
        rc = etd_write_reg(0x10, 0x16) || etd_write_reg(0x11, 0x8f) ? -1 : 0;
        break;
    case 2:
        rc = etd_write_reg(0x10, 0x54) || etd_write_reg(0x11, 0x88) || etd_write_reg(0x21, 0x60)
                 ? -1
                 : 0;
        break;
    case 3:
        rc = etd_write_reg(0x10, etd_info.set_hw_resolution ? 0x0b : 0x01);
        break;
    default:
        etd_reg_07 = 0x01;
        rc = etd_write_reg(0x07, etd_reg_07);
        goto done;
    }
    if (rc == 0) {
        for (;;) {
            rc = etd_read_reg(0x10, &val);
            if (rc == 0 || --tries <= 0) {
                break;
            }
            k_udelay(READ_BACK_DELAY * 1000u);
        }
        if (rc == 0 && etd_info.hw_version == 1 && !(val & R10_ABSOLUTE_MODE)) {
            klog(KLOG_WARN, "elantech", "mode: the touchpad refuses absolute mode");
            rc = -1;
        }
    }
done:
    if (rc) {
        klog(KLOG_WARN, "elantech", "mode: the registers would not set");
    }
    return rc;
}

/** Give the pad's event device the axis `code` from `min` to `max`. */
static void etd_abs(uint16_t code, uint32_t min, uint32_t max)
{
    evdev_set_abs(etd_pad, code, (int32_t)min, (int32_t)max, 0, 0);
}

/**
 * Register the touchpad's event device with the name, numbers and
 * capabilities that Linux's psmouse and `elantech_set_input_params` give
 * the pad's version. Returns 0, or -1 when every node is taken or the
 * slots would not set up.
 */
static int etd_register_pad(void)
{
    const struct elantech_info *info = &etd_info;
    uint8_t keys[EVDEV_KEY_BYTES];
    int buttonpad = info->hw_version >= 3 && etd_is_buttonpad();

    k_memset(keys, 0, sizeof(keys));
    etd_pad = evdev_register("ETPS/2 Elantech Touchpad", EVDEV_BUS_I8042, PSMOUSE_VENDOR,
                             PSMOUSE_ELANTECH, keys);
    if (!etd_pad) {
        return -1;
    }
    evdev_describe(etd_pad, "isa0060/serio1/input0", 0);
    evdev_set_prop(etd_pad, INPUT_PROP_POINTER);
    evdev_set_capability(etd_pad, EV_KEY, BTN_LEFT);
    if (info->has_middle_button) {
        evdev_set_capability(etd_pad, EV_KEY, BTN_MIDDLE);
    }
    if (!buttonpad) {
        evdev_set_capability(etd_pad, EV_KEY, BTN_RIGHT);
    }
    evdev_set_capability(etd_pad, EV_KEY, BTN_TOUCH);
    evdev_set_capability(etd_pad, EV_KEY, BTN_TOOL_FINGER);
    evdev_set_capability(etd_pad, EV_KEY, BTN_TOOL_DOUBLETAP);
    evdev_set_capability(etd_pad, EV_KEY, BTN_TOOL_TRIPLETAP);
    if (buttonpad) {
        evdev_set_prop(etd_pad, INPUT_PROP_BUTTONPAD);
    }
    etd_abs(ABS_X, info->x_min, info->x_max);
    etd_abs(ABS_Y, info->y_min, info->y_max);
    switch (info->hw_version) {
    case 1:
        if (info->fw_version < 0x020000u && (info->capabilities[0] & CAP_HAS_ROCKER)) {
            evdev_set_capability(etd_pad, EV_KEY, BTN_FORWARD);
            evdev_set_capability(etd_pad, EV_KEY, BTN_BACK);
        }
        break;
    case 2:
    case 3:
        if (info->hw_version == 2) {
            evdev_set_capability(etd_pad, EV_KEY, BTN_TOOL_QUADTAP);
        }
        if (info->reports_pressure) {
            etd_abs(ABS_PRESSURE, 0, PMAX_V2);
            etd_abs(ABS_TOOL_WIDTH, 0, WMAX_V2);
        }
        etd_abs(ABS_MT_POSITION_X, info->x_min, info->x_max);
        etd_abs(ABS_MT_POSITION_Y, info->y_min, info->y_max);
        if (evdev_init_slots(etd_pad, 2, INPUT_MT_SEMI_MT) < 0) {
            return -1;
        }
        break;
    default:
        evdev_set_capability(etd_pad, EV_KEY, BTN_TOOL_QUADTAP);
        etd_abs(ABS_PRESSURE, 0, PMAX_V2);
        etd_abs(ABS_TOOL_WIDTH, 0, WMAX_V2);
        etd_abs(ABS_MT_POSITION_X, info->x_min, info->x_max);
        etd_abs(ABS_MT_POSITION_Y, info->y_min, info->y_max);
        etd_abs(ABS_MT_PRESSURE, 0, PMAX_V2);
        /* The traces a finger spans, in the pad's units. */
        etd_abs(ABS_MT_TOUCH_MAJOR, 0, WMAX_V2 * info->width);
        if (evdev_init_slots(etd_pad, MAX_FINGERS, 0) < 0) {
            return -1;
        }
        break;
    }
    evdev_set_abs_res(etd_pad, ABS_X, (int32_t)info->x_res);
    evdev_set_abs_res(etd_pad, ABS_Y, (int32_t)info->y_res);
    if (info->hw_version > 1) {
        evdev_set_abs_res(etd_pad, ABS_MT_POSITION_X, (int32_t)info->x_res);
        evdev_set_abs_res(etd_pad, ABS_MT_POSITION_Y, (int32_t)info->y_res);
    }
    return 0;
}

/** Register the pointing stick's event device as Linux's
 * `elantech_setup_ps2` does; 0, or -1 when every node is taken. */
static int etd_register_trackpoint(void)
{
    etd_tp = mouse_register("ETPS/2 Elantech TrackPoint", "isa0060/serio1/input1", EVDEV_BUS_I8042,
                            PSMOUSE_VENDOR, PSMOUSE_ELANTECH, MOUSE_POINTER);
    if (!etd_tp) {
        return -1;
    }
    evdev_set_prop(etd_tp, INPUT_PROP_POINTING_STICK);
    return 0;
}

/**
 * Linux's `elantech_detect`: the magic knock, whose reply is 0x3C 0x03 and
 * 0xC8 or 0x00, then the firmware version, which tells a pad from a mouse
 * that also answers the knock. Returns 0 for an Elantech pad.
 */
static int etd_detect(void)
{
    static const uint8_t rates[] = {200, 100, 80, 60, 40, 20, 10};
    uint8_t param[3];
    unsigned int i;

    etd_command(CMD_RESET_DIS);
    if (etd_command(CMD_DISABLE) < 0 || etd_command(CMD_SET_SCALE11) < 0 ||
        etd_command(CMD_SET_SCALE11) < 0 || etd_command(CMD_SET_SCALE11) < 0 ||
        etd_getinfo(param) < 0) {
        return -1;
    }
    if (param[0] != 0x3c || param[1] != 0x03 || (param[2] != 0xc8 && param[2] != 0x00)) {
        return -1;
    }
    /* Logitech mice answer the knock too; their version is a rate. */
    if (etd_synaptics_send_cmd(QUERY_FW_VERSION, param) < 0 || param[0] == 0) {
        return -1;
    }
    if (param[1] == 0) {
        return 0;
    }
    /* Some version 4 pads have a revision above 20, so a byte of 10 or 20. */
    if ((param[0] & 0x0fu) >= 0x06 && (param[1] & 0xafu) == 0x0f && param[2] < 40) {
        return 0;
    }
    for (i = 0; i < sizeof(rates); i++) {
        if (param[2] == rates[i]) {
            return -1;
        }
    }
    return 0;
}

/** Linux's `psmouse_reset`: the reset, whose two bytes, 0xAA and the ID,
 * are read and not judged. */
static void etd_reset(void)
{
    if (etd_command(CMD_RESET) == 0 && ps2mouse_receive() >= 0) {
        ps2mouse_receive();
    }
}

int elantech_init(void)
{
    const struct elantech_info *info = &etd_info;

    if (etd_detect() < 0) {
        return 0;
    }
    etd_reset();
    if (etd_query_info(&etd_info) < 0) {
        goto relative;
    }
    klog(KLOG_INFO, "elantech",
         "found: hardware version %d, firmware 0x%06x, capabilities 0x%02x 0x%02x 0x%02x, "
         "samples 0x%02x 0x%02x 0x%02x",
         info->hw_version, info->fw_version, info->capabilities[0], info->capabilities[1],
         info->capabilities[2], info->samples[0], info->samples[1], info->samples[2]);
    if (NEW_IC_SMBUS_HOST_NOTIFY(info->fw_version) || info->bus == BUS_SMB_HST_NTFY_ONLY ||
        info->bus == BUS_PS2_SMB_HST_NTFY) {
        klog(KLOG_INFO, "elantech", "found: the pad offers SMBus, driven over PS/2");
    }
    if (etd_set_absolute_mode() < 0) {
        goto relative;
    }
    /* The pointing stick's device first, as Linux registers it. */
    if (info->has_trackpoint && etd_register_trackpoint() < 0) {
        klog(KLOG_WARN, "elantech", "register: no event device for the pointing stick");
        goto relative;
    }
    if (etd_register_pad() < 0) {
        klog(KLOG_WARN, "elantech", "register: no event device for the touchpad");
        goto relative;
    }
    /* psmouse_initialize: 100 reports a second, then this firmware's mode
     * again, which a rate clears; 200 dpi and 1:1 scaling. */
    if (etd_command(CMD_SET_RATE) == 0) {
        etd_command(100);
    }
    if (info->fw_version == 0x381f17u) {
        etd_write_reg(0x07, etd_reg_07);
    }
    if (etd_command(CMD_SET_RES) == 0) {
        etd_command(3);
    }
    etd_command(CMD_SET_SCALE11);
    mouse_attach_touchpad(etd_pad);
    etd_pktsize = info->hw_version > 1 ? 6 : 4;
    etd_pktcnt = 0;
    etd_single_finger_reports = 0;
    return 1;

relative:
    if (etd_pad) {
        evdev_unregister(etd_pad);
        etd_pad = 0;
    }
    if (etd_tp) {
        evdev_unregister(etd_tp);
        etd_tp = 0;
    }
    etd_reset();
    return -1;
}

/** The odd parity bit of `b`, as Linux's `etd->parity` table: 1 when `b`
 * has an even number of bits set. */
static int etd_parity(uint8_t b)
{
    int n = 0;

    while (b) {
        n ^= 1;
        b &= (uint8_t)(b - 1u);
    }
    return n ^ 1;
}

/** Report the left, middle and right buttons of `b`'s low three bits to
 * `dev`, Linux's `psmouse_report_standard_buttons`. */
static void etd_report_buttons(struct evdev *dev, uint8_t b)
{
    evdev_report(dev, EV_KEY, BTN_LEFT, b & 0x01);
    evdev_report(dev, EV_KEY, BTN_MIDDLE, (b & 0x04) != 0);
    evdev_report(dev, EV_KEY, BTN_RIGHT, (b & 0x02) != 0);
}

/** Report a version 1 packet, Linux's `elantech_report_absolute_v1`. */
static void etd_report_v1(void)
{
    const uint8_t *p = etd_packet;
    int fingers;

    if (etd_info.fw_version < 0x020000u) {
        fingers = ((p[1] & 0x80) >> 7) + ((p[1] & 0x30) >> 4);
    } else {
        fingers = (p[0] & 0xc0) >> 6;
    }
    if (etd_info.jumpy_cursor) {
        if (fingers != 1) {
            etd_single_finger_reports = 0;
        } else if (etd_single_finger_reports < 2) {
            etd_single_finger_reports++;
            return;
        }
    }
    evdev_report(etd_pad, EV_KEY, BTN_TOUCH, fingers != 0);
    if (fingers) {
        evdev_report(etd_pad, EV_ABS, ABS_X, ((p[1] & 0x0c) << 6) | p[2]);
        evdev_report(etd_pad, EV_ABS, ABS_Y,
                     (int32_t)etd_info.y_max - (((p[1] & 0x03) << 8) | p[3]));
    }
    evdev_report(etd_pad, EV_KEY, BTN_TOOL_FINGER, fingers == 1);
    evdev_report(etd_pad, EV_KEY, BTN_TOOL_DOUBLETAP, fingers == 2);
    evdev_report(etd_pad, EV_KEY, BTN_TOOL_TRIPLETAP, fingers == 3);
    etd_report_buttons(etd_pad, p[0]);
    if (etd_info.fw_version < 0x020000u && (etd_info.capabilities[0] & CAP_HAS_ROCKER)) {
        evdev_report(etd_pad, EV_KEY, BTN_FORWARD, (p[0] & 0x40) != 0);
        evdev_report(etd_pad, EV_KEY, BTN_BACK, (p[0] & 0x80) != 0);
    }
    evdev_report(etd_pad, EV_SYN, SYN_REPORT, 0);
}

/** Report one of the two slots of a semi-MT pad, Linux's
 * `elantech_set_slot`. */
static void etd_set_slot(int slot, int active, int x, int y)
{
    evdev_report(etd_pad, EV_ABS, ABS_MT_SLOT, slot);
    evdev_mt_report_slot_state(etd_pad, MT_TOOL_FINGER, active);
    if (active) {
        evdev_report(etd_pad, EV_ABS, ABS_MT_POSITION_X, x);
        evdev_report(etd_pad, EV_ABS, ABS_MT_POSITION_Y, y);
    }
}

/**
 * Report the common part of a version 2 or 3 packet: the touch, the first
 * finger's place, the two slots, the finger count, the buttons and the
 * pressure and width, as Linux's `elantech_report_absolute_v2` and `_v3`.
 * `xy` holds the first finger's X and Y, then the second's.
 */
static void etd_report_semi_mt(unsigned int fingers, const int xy[4], int pres, int width)
{
    evdev_report(etd_pad, EV_KEY, BTN_TOUCH, fingers != 0);
    if (fingers != 0) {
        evdev_report(etd_pad, EV_ABS, ABS_X, xy[0]);
        evdev_report(etd_pad, EV_ABS, ABS_Y, xy[1]);
    }
    etd_set_slot(0, fingers != 0, xy[0], xy[1]);
    etd_set_slot(1, fingers >= 2, xy[2], xy[3]);
    evdev_report(etd_pad, EV_KEY, BTN_TOOL_FINGER, fingers == 1);
    evdev_report(etd_pad, EV_KEY, BTN_TOOL_DOUBLETAP, fingers == 2);
    evdev_report(etd_pad, EV_KEY, BTN_TOOL_TRIPLETAP, fingers == 3);
    if (etd_info.hw_version == 2) {
        evdev_report(etd_pad, EV_KEY, BTN_TOOL_QUADTAP, fingers == 4);
        etd_report_buttons(etd_pad, etd_packet[0]);
        if (!etd_info.reports_pressure) {
            evdev_report(etd_pad, EV_SYN, SYN_REPORT, 0);
            return;
        }
    } else if (etd_is_buttonpad()) {
        evdev_report(etd_pad, EV_KEY, BTN_LEFT, (etd_packet[0] & 0x03) != 0);
    } else {
        etd_report_buttons(etd_pad, etd_packet[0]);
    }
    evdev_report(etd_pad, EV_ABS, ABS_PRESSURE, pres);
    evdev_report(etd_pad, EV_ABS, ABS_TOOL_WIDTH, width);
    evdev_report(etd_pad, EV_SYN, SYN_REPORT, 0);
}

/** The 12-bit X of bytes 1 and 2 and the turned-over Y of bytes 4 and 5,
 * the place of one finger in versions 2, 3 and 4. */
static void etd_place(int *x, int *y)
{
    const uint8_t *p = etd_packet;

    *x = ((p[1] & 0x0f) << 8) | p[2];
    *y = (int)etd_info.y_max - (((p[4] & 0x0f) << 8) | p[5]);
}

/** The pressure of bytes 1 and 4 in versions 2, 3 and 4. */
static int etd_pressure(void)
{
    return (etd_packet[1] & 0xf0) | ((etd_packet[4] & 0xf0) >> 4);
}

/** Report a version 2 packet, Linux's `elantech_report_absolute_v2`. */
static void etd_report_v2(void)
{
    const uint8_t *p = etd_packet;
    unsigned int fingers = (p[0] & 0xc0u) >> 6;
    int xy[4] = {0, 0, 0, 0};
    int width = 0, pres = 0;

    switch (fingers) {
    case 3:
        if (p[3] & 0x80) {
            fingers = 4; /* four fingers or more */
        }
        /* fall through */
    case 1:
        etd_place(&xy[0], &xy[1]);
        pres = etd_pressure();
        width = ((p[0] & 0x30) >> 2) | ((p[3] & 0x30) >> 4);
        break;
    case 2:
        /* Two places of nine bits each, at a quarter of the resolution. */
        xy[0] = (((p[0] & 0x10) << 4) | p[1]) << 2;
        xy[1] = (int)etd_info.y_max - ((((p[0] & 0x20) << 3) | p[2]) << 2);
        xy[2] = (((p[3] & 0x10) << 4) | p[4]) << 2;
        xy[3] = (int)etd_info.y_max - ((((p[3] & 0x20) << 3) | p[5]) << 2);
        pres = 127; /* not given; Linux's values */
        width = 7;
        break;
    default:
        break;
    }
    etd_report_semi_mt(fingers, xy, pres, width);
}

/**
 * Report a pointing stick's packet, Linux's `elantech_report_trackpoint`:
 * of the four forms of its first four bytes, its moves of nine bits, the
 * sign bits in bytes 1 and 2 turned over; a packet whose move is out of
 * -127 to 127, which this firmware sometimes sends, is dropped, and so is a
 * packet of another form. The mouse device's pointer moves with it, as
 * Linux's mousedev moves it.
 */
static void etd_report_trackpoint(void)
{
    const uint8_t *p = etd_packet;
    uint32_t t = (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
    int x, y;

    switch (t & ~7u) {
    case 0x06000030u:
    case 0x16008020u:
    case 0x26800010u:
    case 0x36808000u:
        if (p[4] == 0x80 || p[5] == 0x80 || p[1] >> 7 == p[4] >> 7 || p[2] >> 7 == p[5] >> 7) {
            return;
        }
        if (!etd_tp) {
            return; /* a pad with no stick; Linux would report to no device */
        }
        x = p[4] - (int)((p[1] ^ 0x80) << 1);
        y = (int)((p[2] ^ 0x80) << 1) - p[5];
        etd_report_buttons(etd_tp, p[0]);
        evdev_report(etd_tp, EV_REL, REL_X, x);
        evdev_report(etd_tp, EV_REL, REL_Y, y);
        evdev_report(etd_tp, EV_SYN, SYN_REPORT, 0);
        if (!evdev_grabbed(etd_tp)) {
            mouse_push_packet((uint8_t)(p[0] & 0x07u), x, -y);
        }
        break;
    default:
        break;
    }
}

/** Report a version 3 packet of `type`, Linux's
 * `elantech_report_absolute_v3`: two fingers come as a head packet, which
 * is kept, and a tail packet, which reports both. */
static void etd_report_v3(int type)
{
    const uint8_t *p = etd_packet;
    unsigned int fingers = (p[0] & 0xc0u) >> 6;
    int xy[4] = {0, 0, 0, 0};

    switch (fingers) {
    case 3:
    case 1:
        etd_place(&xy[0], &xy[1]);
        break;
    case 2:
        if (type == PACKET_V3_HEAD) {
            etd_place(&etd_mt[0].x, &etd_mt[0].y);
            return; /* the tail reports */
        }
        xy[0] = etd_mt[0].x;
        xy[1] = etd_mt[0].y;
        etd_place(&xy[2], &xy[3]);
        break;
    default:
        break;
    }
    etd_report_semi_mt(fingers, xy, etd_pressure(), ((p[0] & 0x30) >> 2) | ((p[3] & 0x30) >> 4));
}

/** End a version 4 packet, Linux's `elantech_input_sync_v4`: the buttons,
 * both as the left on a clickpad, the pointer from the slots, and the
 * frame. */
static void etd_sync_v4(void)
{
    if (etd_is_buttonpad()) {
        evdev_report(etd_pad, EV_KEY, BTN_LEFT, (etd_packet[0] & 0x03) != 0);
    } else {
        etd_report_buttons(etd_pad, etd_packet[0]);
    }
    evdev_mt_report_pointer_emulation(etd_pad, 1);
    evdev_report(etd_pad, EV_SYN, SYN_REPORT, 0);
}

/** Report a version 4 status packet, Linux's `process_packet_status_v4`:
 * each finger whose bit in byte 1 is clear is lifted. */
static void etd_status_v4(void)
{
    unsigned int fingers = etd_packet[1] & 0x1fu;
    int i;

    for (i = 0; i < MAX_FINGERS; i++) {
        if ((fingers & (1u << i)) == 0) {
            evdev_report(etd_pad, EV_ABS, ABS_MT_SLOT, i);
            evdev_mt_report_slot_state(etd_pad, MT_TOOL_FINGER, 0);
        }
    }
    etd_sync_v4();
}

/** Report a version 4 head packet, Linux's `process_packet_head_v4`: the
 * place, pressure and width in traces of the finger byte 3 names. */
static void etd_head_v4(void)
{
    const uint8_t *p = etd_packet;
    int id = ((p[3] & 0xe0) >> 5) - 1;
    int traces;

    if (id < 0 || id >= MAX_FINGERS) {
        return;
    }
    etd_place(&etd_mt[id].x, &etd_mt[id].y);
    traces = (p[0] & 0xf0) >> 4;
    evdev_report(etd_pad, EV_ABS, ABS_MT_SLOT, id);
    evdev_mt_report_slot_state(etd_pad, MT_TOOL_FINGER, 1);
    evdev_report(etd_pad, EV_ABS, ABS_MT_POSITION_X, etd_mt[id].x);
    evdev_report(etd_pad, EV_ABS, ABS_MT_POSITION_Y, etd_mt[id].y);
    evdev_report(etd_pad, EV_ABS, ABS_MT_PRESSURE, etd_pressure());
    evdev_report(etd_pad, EV_ABS, ABS_MT_TOUCH_MAJOR, traces * (int)etd_info.width);
    evdev_report(etd_pad, EV_ABS, ABS_TOOL_WIDTH, traces);
    etd_sync_v4();
}

/** Report a version 4 motion packet, Linux's `process_packet_motion_v4`:
 * the signed moves of the fingers bytes 0 and 3 name, five times each with
 * bit 4 of byte 0 set. */
static void etd_motion_v4(void)
{
    const uint8_t *p = etd_packet;
    int id = ((p[0] & 0xe0) >> 5) - 1;
    int sid = ((p[3] & 0xe0) >> 5) - 1;
    int weight = (p[0] & 0x10) ? WEIGHT_VALUE : 1;

    if (id < 0 || id >= MAX_FINGERS) {
        return;
    }
    etd_mt[id].x += (int8_t)p[1] * weight;
    etd_mt[id].y -= (int8_t)p[2] * weight;
    evdev_report(etd_pad, EV_ABS, ABS_MT_SLOT, id);
    evdev_report(etd_pad, EV_ABS, ABS_MT_POSITION_X, etd_mt[id].x);
    evdev_report(etd_pad, EV_ABS, ABS_MT_POSITION_Y, etd_mt[id].y);
    if (sid >= 0 && sid < MAX_FINGERS) {
        etd_mt[sid].x += (int8_t)p[4] * weight;
        etd_mt[sid].y -= (int8_t)p[5] * weight;
        evdev_report(etd_pad, EV_ABS, ABS_MT_SLOT, sid);
        evdev_report(etd_pad, EV_ABS, ABS_MT_POSITION_X, etd_mt[sid].x);
        evdev_report(etd_pad, EV_ABS, ABS_MT_POSITION_Y, etd_mt[sid].y);
    }
    etd_sync_v4();
}

/** Whether the packet is the debounce packet of versions 2 and 3, whose
 * first byte is `b0` and the rest 0xFF 0xFF 0x02 0xFF 0xFF: the pad is
 * settling, and the packet is not reported. */
static int etd_debounce(uint8_t b0)
{
    static const uint8_t rest[5] = {0xff, 0xff, 0x02, 0xff, 0xff};
    int i;

    if (etd_packet[0] != b0) {
        return 0;
    }
    for (i = 0; i < 5; i++) {
        if (etd_packet[i + 1] != rest[i]) {
            return 0;
        }
    }
    return 1;
}

/** Whether a version 1 packet's three parity bits are right, Linux's
 * `elantech_packet_check_v1`. */
static int etd_check_v1(void)
{
    const uint8_t *p = etd_packet;
    int p1, p2, p3;

    if (etd_info.fw_version < 0x020000u) {
        p1 = (p[0] & 0x20) >> 5;
        p2 = (p[0] & 0x10) >> 4;
    } else {
        p1 = (p[0] & 0x10) >> 4;
        p2 = (p[0] & 0x20) >> 5;
    }
    p3 = (p[0] & 0x04) >> 2;
    return etd_parity(p[1]) == p1 && etd_parity(p[2]) == p2 && etd_parity(p[3]) == p3;
}

/** Whether a version 2 packet's constant bits are right, Linux's
 * `elantech_packet_check_v2`: a pad that reports pressure has the same in
 * every packet, an older one others for two fingers. */
static int etd_check_v2(void)
{
    const uint8_t *p = etd_packet;

    if (etd_info.reports_pressure) {
        return (p[0] & 0x0c) == 0x04 && (p[3] & 0x0f) == 0x02;
    }
    if ((p[0] & 0xc0) == 0x80) {
        return (p[0] & 0x0c) == 0x0c && (p[3] & 0x0e) == 0x08;
    }
    return (p[0] & 0x3c) == 0x3c && (p[1] & 0xf0) == 0x00 && (p[3] & 0x3e) == 0x38 &&
           (p[4] & 0xf0) == 0x00;
}

/** The kind of a version 3 packet by its constant bits, Linux's
 * `elantech_packet_check_v3`; a CRC pad's differ. */
static int etd_check_v3(void)
{
    const uint8_t *p = etd_packet;

    /* The debounce packet has the head's bytes 0 and 3. */
    if (etd_debounce(0xc4)) {
        return PACKET_DEBOUNCE;
    }
    if (etd_info.crc_enabled) {
        if ((p[3] & 0x09) == 0x08) {
            return PACKET_V3_HEAD;
        }
        if ((p[3] & 0x09) == 0x09) {
            return PACKET_V3_TAIL;
        }
    } else {
        if ((p[0] & 0x0c) == 0x04 && (p[3] & 0xcf) == 0x02) {
            return PACKET_V3_HEAD;
        }
        if ((p[0] & 0x0c) == 0x0c && (p[3] & 0xce) == 0x0c) {
            return PACKET_V3_TAIL;
        }
        if ((p[3] & 0x0f) == 0x06) {
            return PACKET_TRACKPOINT;
        }
    }
    return PACKET_UNKNOWN;
}

/** The kind of a version 4 packet, Linux's `elantech_packet_check_v4`: a
 * stick's, then by constant bits that depend on the CRC flag and the IC
 * body, a status, head or motion packet by the low bits of byte 3. */
static int etd_check_v4(void)
{
    const uint8_t *p = etd_packet;
    uint32_t ic_version = (etd_info.fw_version & 0x0f0000u) >> 16;
    int sane;

    if (etd_tp && (p[3] & 0x0f) == 0x06) {
        return PACKET_TRACKPOINT;
    }
    if (etd_info.crc_enabled) {
        sane = (p[3] & 0x08) == 0x00;
    } else if (ic_version == 7 && etd_info.samples[1] == 0x2A) {
        sane = (p[3] & 0x1c) == 0x10;
    } else {
        sane = (p[0] & 0x08) == 0x00 && (p[3] & 0x1c) == 0x10;
    }
    if (!sane) {
        return PACKET_UNKNOWN;
    }
    switch (p[3] & 0x03) {
    case 0:
        return PACKET_V4_STATUS;
    case 1:
        return PACKET_V4_HEAD;
    case 2:
        return PACKET_V4_MOTION;
    default:
        return PACKET_UNKNOWN;
    }
}

/** Check and report a whole packet, Linux's `elantech_process_byte`.
 * Returns -1 for a packet out of form, which is dropped. */
static int etd_process_packet(void)
{
    int type;

    switch (etd_info.hw_version) {
    case 1:
        if (!etd_check_v1()) {
            return -1;
        }
        etd_report_v1();
        break;
    case 2:
        if (etd_debounce(0x84)) {
            break;
        }
        if (!etd_check_v2()) {
            return -1;
        }
        etd_report_v2();
        break;
    case 3:
        type = etd_check_v3();
        if (type == PACKET_UNKNOWN) {
            return -1;
        }
        if (type == PACKET_TRACKPOINT) {
            etd_report_trackpoint();
        } else if (type != PACKET_DEBOUNCE) {
            etd_report_v3(type);
        }
        break;
    default:
        type = etd_check_v4();
        if (type == PACKET_UNKNOWN) {
            return -1;
        }
        if (type == PACKET_TRACKPOINT) {
            etd_report_trackpoint();
        } else if (type == PACKET_V4_STATUS) {
            etd_status_v4();
        } else if (type == PACKET_V4_HEAD) {
            etd_head_v4();
        } else {
            etd_motion_v4();
        }
        break;
    }
    return 0;
}

void elantech_byte(uint8_t data)
{
    uint32_t now = (uint32_t)kernel_ticks;

    if (etd_pktcnt && now - etd_last_tick > MS_TO_TICKS(500)) {
        etd_sync_losses++;
        etd_pktcnt = 0;
    }
    etd_last_tick = now;
    etd_packet[etd_pktcnt++] = data;
    if (etd_pktcnt < etd_pktsize) {
        return;
    }
    etd_pktcnt = 0;
    if (etd_process_packet() < 0) {
        etd_sync_losses++;
    }
}
