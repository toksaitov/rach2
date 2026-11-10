/*
 * synaptics.c: the Synaptics touchpad on the PS/2 mouse's port, set up and
 * read as Linux's psmouse and synaptics drivers do (synaptics.h).
 *
 * The touchpad talks the PS/2 mouse's commands. It takes a request of its
 * own as a "sliced" command, 1:1 scaling followed by four resolutions of two
 * bits each, the request's byte from the top: then a status request (0xE9)
 * reads a query's three bytes, and a sample rate of 0x14 sets the mode byte.
 * In absolute mode it sends six-byte packets:
 *
 *   Byte 0: [1|0|W3|W2|0|W1|R|L]   (bits 7 and 6 frame the packet)
 *   Byte 1: [Y11..Y8|X11..X8]
 *   Byte 2: Z, the pressure
 *   Byte 3: [1|1|Y12|X12|0|W0|R^|L^] (^: an extra button's change)
 *   Byte 4: X7..X0
 *   Byte 5: Y7..Y0
 *
 * W is the finger's width, 4 to 15, or a code below 4: 0 two fingers, 1
 * three or more, 2 an advanced gesture packet of the second finger, 3 a
 * packet of the pass-through port's device, whose bytes are bytes 1, 4, 5
 * and, for a four-byte device, 2. Y grows upward and is turned over for the
 * event device. A touchpad with an image sensor tracks the fingers itself,
 * and its first and second contacts go to two slots; another one reports
 * one contact and the finger count.
 *
 * What PS/2 does not tell, Linux takes from the PS/2 mouse's ACPI PNP ID
 * (i8042.h): the true coordinates of some ThinkPads' pads, the strip of
 * buttons at the top of others, and a ForcePad's click. A request to the
 * pass-through port's device is sliced too, its byte the request's, ended
 * by the sample rate 0x28; the device's answers come back one byte to a
 * pass-through packet.
 */

#include "synaptics.h"

#include "i8042.h"
#include "ps2mouse.h"
#include "smbios.h"
#include "../../dmi.h"
#include "../../evdev.h"
#include "../../klog.h"
#include "../../lib/kstring.h"
#include "../../mouse.h"
#include "../../timer.h"

/* The mouse commands the touchpad's requests are made of. */
#define CMD_SET_SCALE11 0xE6
#define CMD_SET_RES     0xE8
#define CMD_GET_INFO    0xE9 /* three bytes after the ACK */
#define CMD_GET_ID      0xF2 /* the ID after the ACK */
#define CMD_SET_RATE    0xF3
#define CMD_RESET       0xFF /* 0xAA and the ID 0x00 after the ACK */

/* The queries, Linux's SYN_QUE_*. */
#define QUE_IDENTIFY       0x00
#define QUE_MODES          0x01
#define QUE_CAPABILITIES   0x02
#define QUE_MODEL          0x03
#define QUE_RESOLUTION     0x08
#define QUE_EXT_CAPAB      0x09
#define QUE_FIRMWARE_ID    0x0a
#define QUE_EXT_CAPAB_0C   0x0c
#define QUE_EXT_MAX_COORDS 0x0d
#define QUE_EXT_MIN_COORDS 0x0f
#define QUE_MEXT_CAPAB_10  0x10

/* The mode byte's bits, and the sample rates that set the mode and turn
 * on the advanced gesture mode after a sliced command. */
#define MODE_ABSOLUTE        0x80
#define MODE_HIGH_RATE       0x40 /* 80 packets a second, not 40 */
#define MODE_DISABLE_GESTURE 0x04
#define MODE_FOUR_BYTE       0x02 /* the pass-through device's packets are four bytes */
#define MODE_W               0x01
#define RATE_SET_MODE2       0x14
#define RATE_AGM             0xc8
#define RATE_PASS_THROUGH    0x28 /* the sliced byte is for the pass-through device */

/* The identify query: the model, the firmware's major and minor version,
 * and 0x47 in the middle byte, which says a Synaptics pad answered. */
#define ID_MODEL(i)        (((i) >> 4) & 0x0fu)
#define ID_MAJOR(i)        ((i) & 0x0fu)
#define ID_MINOR(i)        (((i) >> 16) & 0xffu)
#define ID_FULL(i)         ((ID_MAJOR(i) << 8) | ID_MINOR(i))
#define ID_IS_SYNAPTICS(i) (((i) & 0xff00u) == 0x4700u)

/* The model ID: packets of the new absolute form. */
#define MODEL_NEWABS(m) ((m) & 0x80u)

/* The capabilities, the extended capabilities and those of queries 0x0c
 * and 0x10, Linux's SYN_CAP_*. */
#define CAP_EXTENDED(c)          ((c) & 0x800000u)
#define CAP_MIDDLE_BUTTON(c)     ((c) & 0x040000u)
#define CAP_PASS_THROUGH(c)      ((c) & 0x000080u)
#define CAP_FOUR_BUTTON(c)       ((c) & 0x000008u)
#define CAP_MULTIFINGER(c)       ((c) & 0x000002u)
#define CAP_PALMDETECT(c)        ((c) & 0x000001u)
#define CAP_SUBMODEL_ID(c)       (((c) >> 8) & 0xffu)
#define EXT_CAP_REQUESTS(c)      (((c) >> 20) & 0x07u)
#define CAP_MB_MASK              0xf000u
#define CAP_MULTI_BUTTON_NO(ec)  (((ec) & CAP_MB_MASK) >> 12)
#define MEXT_CAP_BIT(m)          ((m) & 0x02u)
#define CAP_CLICKPAD(c0c)        ((c0c) & 0x100000u)
#define CAP_MAX_DIMENSIONS(c0c)  ((c0c) & 0x020000u)
#define CAP_MIN_DIMENSIONS(c0c)  ((c0c) & 0x002000u)
#define CAP_ADV_GESTURE(c0c)     ((c0c) & 0x080000u)
#define CAP_REDUCED_FILTER(c0c)  ((c0c) & 0x000400u)
#define CAP_IMAGE_SENSOR(c0c)    ((c0c) & 0x000800u)
#define CAP_EXT_BUTTONS_STICK(c) ((c) & 0x010000u)
#define CAP_INTERTOUCH(c0c)      ((c0c) & 0x004000u)

/* The extra buttons a pad wires to its stick, Linux's
 * SYN_EXT_BUTTON_STICK_*: bits 0, 1 and 2, left, middle and right. */
#define STICK_L(eb) ((eb) & 1u)
#define STICK_M(eb) (((eb) >> 1) & 1u)
#define STICK_R(eb) (((eb) >> 2) & 1u)

/* The limits of the positions when the pad does not give its own, from the
 * Synaptics TouchPad Interfacing Guide as Linux has them; Y is turned over
 * about the nominal range. */
#define XMAX         6143
#define YMAX         6143
#define XMIN_NOMINAL 1472
#define XMAX_NOMINAL 5472
#define YMIN_NOMINAL 1408
#define YMAX_NOMINAL 4448

/* A position is 13 bits; one above X_MAX_POSITIVE is a small negative
 * number cut to 13 bits, and X_MAX_POSITIVE itself a finger at the edge
 * whose place the pad could not tell. */
#define ABS_POS_BITS   13
#define X_MAX_POSITIVE 8176
#define Y_MAX_POSITIVE 8176

/* The fuzz of a pad that filters its positions less, and the farthest a
 * contact moves between packets and keeps its slot, in millimetres. */
#define REDUCED_FILTER_FUZZ 8
#define DMAX                10

/* How long a ForcePad's single finger must press before its click is the
 * left button, in milliseconds. */
#define FORCEPAD_PRESS_MS 50

/* What Linux 6.12's synaptics.c chooses by the PS/2 mouse's PNP ID. The
 * pads with buttons in a strip at the top, which Synaptics listed: */
static const char *const topbuttonpad_pnp_ids[] = {
    "LEN0017", "LEN0018", "LEN0019", "LEN0023", "LEN002A", "LEN002B", "LEN002C",
    "LEN002D", "LEN002E", "LEN0033", "LEN0034", "LEN0035", "LEN0036", "LEN0037",
    "LEN0038", "LEN0039", "LEN0041", "LEN0042", "LEN0045", "LEN0047", "LEN2000",
    "LEN2001", "LEN2002", "LEN2003", "LEN2004", "LEN2005", "LEN2006", "LEN2007",
    "LEN2008", "LEN2009", "LEN200A", "LEN200B", 0};

/* The pads Linux moves to SMBus beside those, which Rach2 drives over PS/2
 * as Linux does without its SMBus driver: */
static const char *const smbus_pnp_ids[] = {
    "LEN0048", "LEN0046", "LEN0049", "LEN004a", "LEN005b", "LEN005e", "LEN006c", "LEN007a",
    "LEN0071", "LEN0072", "LEN0073", "LEN0091", "LEN0092", "LEN0093", "LEN0096", "LEN0097",
    "LEN0099", "LEN009b", "LEN0402", "LEN040f", "LEN0411", "LEN200f", "LEN2044", "LEN2054",
    "LEN2055", "LEN2068", "SYN3015", "SYN3052", "SYN3221", "SYN323d", "SYN3257", 0};

/* The ForcePads, which give their click as a clickpad's but also for more
 * than one finger: */
static const char *const forcepad_pnp_ids[] = {"SYN300D", "SYN3014", 0};

/*
 * The pads whose coordinates Linux sets in place of what they report,
 * Linux's `min_max_pnpid_table`: the IDs, the range of board IDs it holds
 * for, 0 at either end for no limit, and the limits, X then Y. The first
 * match wins.
 */
static const char *const len0033[] = {"LEN0033", 0};
static const char *const len0042[] = {"LEN0042", 0};
static const char *const len0034[] = {"LEN0034", "LEN0036", "LEN0037", "LEN0039",
                                      "LEN2002", "LEN2004", 0};
static const char *const len2000[] = {"LEN2000", 0};
static const char *const len2001[] = {"LEN2001", 0};
static const char *const len2006[] = {"LEN2006", 0};
static const struct min_max_quirk {
    const char *const *ids;
    uint32_t board_min, board_max;
    uint32_t x_min, x_max, y_min, y_max;
} min_max_quirks[] = {
    {len0033, 0, 0, 1024, 5052, 2258, 4832},    {len0042, 0, 0, 1232, 5710, 1156, 4696},
    {len0034, 0, 2961, 1024, 5112, 2024, 4832}, {len2000, 0, 0, 1024, 5113, 2021, 4832},
    {len2001, 0, 0, 1024, 5022, 2508, 4832},    {len2006, 2691, 2691, 1024, 5045, 2457, 4832},
    {len2006, 0, 0, 1264, 5675, 1171, 4688},    {0, 0, 0, 0, 0, 0, 0}};

/* The protocol's number in Linux's `enum psmouse_type`, which EVIOCGID
 * gives as the product, and the vendor psmouse gives every mouse. */
#define PSMOUSE_SYNAPTICS 7
#define PSMOUSE_VENDOR    0x0002

/* The forms of a packet, Linux's `enum synaptics_pkt_type`: a new absolute
 * packet whose form is not yet known, then one of the strict or the relaxed
 * check, and the old form. */
#define PKT_NEWABS         0
#define PKT_NEWABS_STRICT  1
#define PKT_NEWABS_RELAXED 2
#define PKT_OLDABS         3

/* What the queries found, Linux's `struct synaptics_device_info`. */
struct synaptics_info {
    uint32_t identity;
    uint32_t model_id;
    uint32_t firmware_id;
    uint32_t board_id;
    uint32_t capabilities;
    uint32_t ext_cap;
    uint32_t ext_cap_0c;
    uint32_t ext_cap_10;
    uint32_t x_res, y_res; /* units a millimetre, 0 when not given */
    uint32_t x_max, y_max; /* 0 when not given */
    uint32_t x_min, y_min;
};

/* One packet's reading, Linux's `struct synaptics_hw_state`. */
struct hw_state {
    int x;
    int y;
    int z;
    int w;
    unsigned int left : 1;
    unsigned int right : 1;
    unsigned int middle : 1;
    unsigned int up : 1;
    unsigned int down : 1;
    uint8_t ext_buttons;
    int8_t scroll;
};

static struct synaptics_info info;
static uint8_t mode;           /* the mode byte set */
static int pkt_type;           /* PKT_* */
static int is_forcepad;        /* the PNP ID names a ForcePad */
static int press;              /* a ForcePad's single finger presses */
static int report_press;       /* and has pressed long enough to click */
static uint32_t press_start;   /* the tick its press began */
static struct evdev *pad;      /* the event device */
static struct hw_state agm;    /* the second finger, from the last AGM packet */
static unsigned int agm_count; /* the fingers an AGM contact packet counted */
static int scroll;             /* the middle button's scroll, in steps */

/* The packet being assembled: its bytes, how many have come, and the tick
 * of the last. */
static uint8_t packet[6];
static int pktcnt;
static uint32_t last_tick;

/* The packets dropped for a byte out of form or a pause, which psmouse
 * logs as lost synchronization. */
static unsigned int sync_losses;

/** Send `cmd` and return 0 when the touchpad acknowledged it, -1 when not. */
static int command(uint8_t cmd)
{
    return ps2mouse_send(cmd) == PS2MOUSE_ACK ? 0 : -1;
}

/** Send `cmd` and its argument `arg`; 0 when both were acknowledged. */
static int command_arg(uint8_t cmd, uint8_t arg)
{
    return command(cmd) == 0 && command(arg) == 0 ? 0 : -1;
}

/** Read the `n` bytes that follow a command's ACK into `buf`; 0, or -1
 * when one did not come. */
static int receive(uint8_t *buf, int n)
{
    int i, b;

    for (i = 0; i < n; i++) {
        b = ps2mouse_receive();
        if (b < 0) {
            return -1;
        }
        buf[i] = (uint8_t)b;
    }
    return 0;
}

/** Send the touchpad's request `req` as Linux's `ps2_sliced_command`: 1:1
 * scaling, then four resolutions, two bits of `req` each from the top. */
static int sliced(uint8_t req)
{
    int i;

    if (command(CMD_SET_SCALE11) < 0) {
        return -1;
    }
    for (i = 6; i >= 0; i -= 2) {
        if (command_arg(CMD_SET_RES, (uint8_t)((req >> i) & 3u)) < 0) {
            return -1;
        }
    }
    return 0;
}

/** Ask the query `q` and read its three bytes into `param`. */
static int send_query(uint8_t q, uint8_t *param)
{
    if (sliced(q) < 0 || command(CMD_GET_INFO) < 0) {
        return -1;
    }
    return receive(param, 3);
}

/** Ask the query `q` and read its three bytes into `*val`, the first
 * highest. */
static int query_int(uint8_t q, uint32_t *val)
{
    uint8_t p[3];

    if (send_query(q, p) < 0) {
        return -1;
    }
    *val = (uint32_t)p[0] << 16 | (uint32_t)p[1] << 8 | p[2];
    return 0;
}

/** Set the mode byte `m`, as Linux's `synaptics_mode_cmd`. */
static int mode_cmd(uint8_t m)
{
    if (sliced(m) < 0) {
        return -1;
    }
    return command_arg(CMD_SET_RATE, RATE_SET_MODE2);
}

/**
 * Linux's `synaptics_detect`: four resolutions of 0 and a status request,
 * whose second byte a Synaptics pad answers with 0x47 and another mouse
 * with its resolution. A refused command is not a failure here: the reply
 * is what tells.
 */
static int detect(void)
{
    uint8_t param[3];
    int i;

    k_memset(param, 0, sizeof(param));
    for (i = 0; i < 4; i++) {
        command_arg(CMD_SET_RES, 0);
    }
    if (command(CMD_GET_INFO) == 0) {
        receive(param, 3);
    }
    return param[1] == 0x47 ? 0 : -1;
}

/** The capabilities query and those it leads to, as Linux's
 * `synaptics_capability`, into `in`. A failed extended query leaves its
 * bits 0. */
static int query_capabilities(struct synaptics_info *in)
{
    if (query_int(QUE_CAPABILITIES, &in->capabilities) < 0) {
        return -1;
    }
    in->ext_cap = in->ext_cap_0c = 0;
    if (ID_FULL(in->identity) < 0x705 && CAP_SUBMODEL_ID(in->capabilities) != 0x47) {
        return -1; /* older firmware had the submodel fixed at 0x47 */
    }
    if (!CAP_EXTENDED(in->capabilities)) {
        in->capabilities = 0;
    }
    if (EXT_CAP_REQUESTS(in->capabilities) >= 1) {
        if (query_int(QUE_EXT_CAPAB, &in->ext_cap) < 0) {
            in->ext_cap = 0;
            klog(KLOG_WARN, "synaptics", "query: the extended capabilities would not read");
        } else if (CAP_MULTI_BUTTON_NO(in->ext_cap) > 8) {
            in->ext_cap &= ~CAP_MB_MASK;
        }
    }
    if (EXT_CAP_REQUESTS(in->capabilities) >= 4) {
        if (query_int(QUE_EXT_CAPAB_0C, &in->ext_cap_0c) < 0) {
            in->ext_cap_0c = 0;
            klog(KLOG_WARN, "synaptics", "query: capability 0x0c would not read");
        }
    }
    return 0;
}

/** A coordinates query's reply in `r` as its X and Y, as Linux reads the
 * maximum and minimum coordinates. */
static void coords(const uint8_t *r, uint32_t *x, uint32_t *y)
{
    *x = (uint32_t)r[0] << 5 | (uint32_t)(r[1] & 0x0fu) << 1;
    *y = (uint32_t)r[2] << 5 | (uint32_t)(r[1] & 0xf0u) >> 3;
}

/** The resolution and the coordinates' limits, as Linux's
 * `synaptics_resolution`, into `in`; what a pad does not give stays 0. */
static void query_resolution(struct synaptics_info *in)
{
    uint8_t r[3];

    if (ID_MAJOR(in->identity) < 4) {
        return;
    }
    if (send_query(QUE_RESOLUTION, r) == 0 && r[0] != 0 && (r[1] & 0x80u) && r[2] != 0) {
        in->x_res = r[0];
        in->y_res = r[2];
    }
    if (EXT_CAP_REQUESTS(in->capabilities) >= 5 && CAP_MAX_DIMENSIONS(in->ext_cap_0c)) {
        if (send_query(QUE_EXT_MAX_COORDS, r) < 0) {
            klog(KLOG_WARN, "synaptics", "query: the maximum coordinates would not read");
        } else {
            coords(r, &in->x_max, &in->y_max);
        }
    }
    /* Firmware 8.1 gives too few extended queries, and right minimums. */
    if (CAP_MIN_DIMENSIONS(in->ext_cap_0c) &&
        (EXT_CAP_REQUESTS(in->capabilities) >= 7 || ID_FULL(in->identity) == 0x801)) {
        if (send_query(QUE_EXT_MIN_COORDS, r) < 0) {
            klog(KLOG_WARN, "synaptics", "query: the minimum coordinates would not read");
        } else {
            coords(r, &in->x_min, &in->y_min);
        }
    }
}

/** Every query, as Linux's `synaptics_query_hardware`, into `in`; -1 when
 * one that Linux needs failed or the pad is not a Synaptics pad after
 * all. */
static int query_hardware(struct synaptics_info *in)
{
    uint8_t bid[3];

    k_memset(in, 0, sizeof(*in));
    if (query_int(QUE_IDENTIFY, &in->identity) < 0 || !ID_IS_SYNAPTICS(in->identity)) {
        return -1;
    }
    if (query_int(QUE_MODEL, &in->model_id) < 0 ||
        query_int(QUE_FIRMWARE_ID, &in->firmware_id) < 0) {
        return -1;
    }
    if (ID_FULL(in->identity) >= 0x705) { /* earlier firmware has no board */
        if (send_query(QUE_MODES, bid) < 0) {
            return -1;
        }
        in->board_id = (uint32_t)(bid[0] & 0xfcu) << 6 | bid[1];
        if (MEXT_CAP_BIT(bid[0]) && query_int(QUE_MEXT_CAPAB_10, &in->ext_cap_10) < 0) {
            return -1;
        }
    }
    if (query_capabilities(in) < 0) {
        return -1;
    }
    query_resolution(in);
    return 0;
}

/** Set the coordinates of a pad whose PNP ID and board Linux's
 * `min_max_pnpid_table` names, as `synaptics_apply_quirks` does. */
static void apply_quirks(void)
{
    const struct min_max_quirk *q;

    for (q = min_max_quirks; q->ids; q++) {
        if (!i8042_matches_pnp_id(I8042_AUX_PORT, q->ids) ||
            (q->board_min && info.board_id < q->board_min) ||
            (q->board_max && info.board_id > q->board_max)) {
            continue;
        }
        info.x_min = q->x_min;
        info.x_max = q->x_max;
        info.y_min = q->y_min;
        info.y_max = q->y_max;
        klog(KLOG_INFO, "synaptics", "quirk: min/max coordinates: x [%u..%u], y [%u..%u]",
             info.x_min, info.x_max, info.y_min, info.y_max);
        return;
    }
}

/** Whether Linux 6.12, built with its SMBus driver, moves the pad to SMBus
 * by its PNP ID: one of `topbuttonpad_pnp_ids` or `smbus_pnp_ids`, as
 * `synaptics_setup_intertouch` asks. */
static int smbus_listed(void)
{
    return i8042_matches_pnp_id(I8042_AUX_PORT, topbuttonpad_pnp_ids) ||
           i8042_matches_pnp_id(I8042_AUX_PORT, smbus_pnp_ids);
}

/** Whether the pad sends advanced gesture packets of a second finger. */
static int has_agm(void)
{
    return CAP_ADV_GESTURE(info.ext_cap_0c) || CAP_IMAGE_SENSOR(info.ext_cap_0c);
}

/** Whether the pad counts fingers, in W or in AGM packets. */
static int has_multifinger(void)
{
    return CAP_MULTIFINGER(info.capabilities) || has_agm();
}

/** Whether the machine is one of Linux's `toshiba_dmi_table`. */
static int impaired_toshiba_kbc(void)
{
    return smbios_system_is("TOSHIBA", "Satellite") || smbios_system_is("TOSHIBA", "dynabook") ||
           smbios_system_is("TOSHIBA", "PORTEGE M300") ||
           (smbios_system_is("TOSHIBA", "Portable PC") &&
            smbios_field_has(DMI_PRODUCT_VERSION, "Version 1.0"));
}

/**
 * Set the mode as Linux's `synaptics_set_mode` does for the port's rate:
 * absolute mode, gestures off on firmware 4 and later, 80 packets a second
 * for a rate of 80 or more and 40 below, W mode when the pad has the
 * extended capabilities; then the advanced gesture mode of a pad that has
 * it.
 */
static int set_mode(void)
{
    mode = MODE_ABSOLUTE | (ps2mouse_port_rate() >= 80 ? MODE_HIGH_RATE : 0);
    if (ID_MAJOR(info.identity) >= 4) {
        mode |= MODE_DISABLE_GESTURE;
    }
    if (CAP_EXTENDED(info.capabilities)) {
        mode |= MODE_W;
    }
    if (mode_cmd(mode) < 0) {
        return -1;
    }
    if (has_agm() && (sliced(QUE_MODEL) < 0 || command_arg(CMD_SET_RATE, RATE_AGM) < 0)) {
        return -1;
    }
    return 0;
}

/** The limits of the position axes `x_code` and `y_code`, Linux's
 * `set_abs_position_params`. */
static void set_position(uint16_t x_code, uint16_t y_code)
{
    int32_t fuzz = CAP_REDUCED_FILTER(info.ext_cap_0c) ? REDUCED_FILTER_FUZZ : 0;

    evdev_set_abs(pad, x_code, info.x_min ? (int32_t)info.x_min : XMIN_NOMINAL,
                  info.x_max ? (int32_t)info.x_max : XMAX_NOMINAL, fuzz, 0);
    evdev_set_abs(pad, y_code, info.y_min ? (int32_t)info.y_min : YMIN_NOMINAL,
                  info.y_max ? (int32_t)info.y_max : YMAX_NOMINAL, fuzz, 0);
    evdev_set_abs_res(pad, x_code, (int32_t)info.x_res);
    evdev_set_abs_res(pad, y_code, (int32_t)info.y_res);
}

/**
 * Register the event device with the name, numbers and capabilities that
 * Linux's psmouse and `set_input_params` give an absolute Synaptics pad.
 * Returns 0, or -1 when every node is taken or the slots would not set up.
 */
static int register_pad(void)
{
    uint8_t keys[EVDEV_KEY_BYTES];
    unsigned int i;

    k_memset(keys, 0, sizeof(keys));
    pad = evdev_register("SynPS/2 Synaptics TouchPad", EVDEV_BUS_I8042, PSMOUSE_VENDOR,
                         PSMOUSE_SYNAPTICS, keys);
    if (!pad) {
        return -1;
    }
    evdev_describe(pad, "isa0060/serio1/input0", 0);
    /* The model's sensor and low byte, which Linux gives as the version. */
    evdev_set_version(
        pad, (uint16_t)(((info.model_id & 0x00ff0000u) >> 8) | (info.model_id & 0x000000ffu)));
    evdev_set_prop(pad, INPUT_PROP_POINTER);
    evdev_set_capability(pad, EV_KEY, BTN_LEFT);
    if (!CAP_CLICKPAD(info.ext_cap_0c)) {
        evdev_set_capability(pad, EV_KEY, BTN_RIGHT);
        if (CAP_MIDDLE_BUTTON(info.capabilities)) {
            evdev_set_capability(pad, EV_KEY, BTN_MIDDLE);
        }
    }
    set_position(ABS_X, ABS_Y);
    evdev_set_abs(pad, ABS_PRESSURE, 0, 255, 0, 0);
    if (CAP_IMAGE_SENSOR(info.ext_cap_0c)) {
        set_position(ABS_MT_POSITION_X, ABS_MT_POSITION_Y);
        evdev_set_abs(pad, ABS_MT_PRESSURE, 0, 255, 0, 0);
        if (evdev_init_slots(pad, 2, INPUT_MT_POINTER | INPUT_MT_TRACK) < 0) {
            return -1;
        }
        evdev_set_capability(pad, EV_KEY, BTN_TOOL_QUADTAP); /* four and five fingers */
        evdev_set_capability(pad, EV_KEY, BTN_TOOL_QUINTTAP);
    } else if (CAP_ADV_GESTURE(info.ext_cap_0c)) {
        set_position(ABS_MT_POSITION_X, ABS_MT_POSITION_Y);
        if (evdev_init_slots(pad, 2, INPUT_MT_POINTER | INPUT_MT_SEMI_MT) < 0) {
            return -1;
        }
        set_position(ABS_X, ABS_Y); /* the slots' copy dropped the fuzz */
    }
    if (CAP_PALMDETECT(info.capabilities)) {
        evdev_set_abs(pad, ABS_TOOL_WIDTH, 0, 15, 0, 0);
    }
    evdev_set_capability(pad, EV_KEY, BTN_TOUCH);
    evdev_set_capability(pad, EV_KEY, BTN_TOOL_FINGER);
    if (has_multifinger()) {
        evdev_set_capability(pad, EV_KEY, BTN_TOOL_DOUBLETAP);
        evdev_set_capability(pad, EV_KEY, BTN_TOOL_TRIPLETAP);
    }
    if (CAP_FOUR_BUTTON(info.capabilities) || CAP_MIDDLE_BUTTON(info.capabilities)) {
        evdev_set_capability(pad, EV_KEY, BTN_FORWARD);
        evdev_set_capability(pad, EV_KEY, BTN_BACK);
    }
    if (!CAP_EXT_BUTTONS_STICK(info.ext_cap_10)) {
        for (i = 0; i < CAP_MULTI_BUTTON_NO(info.ext_cap); i++) {
            evdev_set_capability(pad, EV_KEY, (uint16_t)(BTN_0 + i));
        }
    }
    if (CAP_CLICKPAD(info.ext_cap_0c)) {
        evdev_set_prop(pad, INPUT_PROP_BUTTONPAD);
        if (i8042_matches_pnp_id(I8042_AUX_PORT, topbuttonpad_pnp_ids) &&
            !CAP_EXT_BUTTONS_STICK(info.ext_cap_10)) {
            evdev_set_prop(pad, INPUT_PROP_TOPBUTTONPAD);
        }
    }
    return 0;
}

unsigned int synaptics_set_rate(unsigned int rate)
{
    if (rate >= 80) {
        mode |= MODE_HIGH_RATE;
    } else {
        mode &= (uint8_t)~MODE_HIGH_RATE;
    }
    mode_cmd(mode);
    return rate >= 80 ? 80 : 40;
}

/** The rate, resolution and scaling that Linux's `psmouse_initialize` sets
 * on the pad: the mode byte again for the port's rate, through
 * `synaptics_set_rate`, then the port's resolution and 1:1. */
static void initialize(void)
{
    ps2mouse_initialize(synaptics_set_rate);
}

int synaptics_init(void)
{
    uint8_t reply[2];

    if (detect() < 0) {
        return 0;
    }
    /* Linux's psmouse_reset, whose reply it does not judge here. */
    if (command(CMD_RESET) == 0) {
        receive(reply, 2);
    }
    if (query_hardware(&info) < 0) {
        klog(KLOG_WARN, "synaptics", "query: the touchpad did not answer");
        goto relative;
    }
    is_forcepad = i8042_matches_pnp_id(I8042_AUX_PORT, forcepad_pnp_ids);
    if (CAP_INTERTOUCH(info.ext_cap_0c) && !is_forcepad) {
        klog(KLOG_WARN, "synaptics",
             smbus_listed()
                 ? "smbus: the touchpad (%s) is one Linux drives on SMBus, which is not "
                   "driven; it stays on PS/2"
                 : "smbus: the touchpad (%s) says it can support a different bus; it stays on "
                   "PS/2",
             i8042_firmware_id(I8042_AUX_PORT));
    }
    apply_quirks();
    press = report_press = 0;
    if (set_mode() < 0) {
        klog(KLOG_WARN, "synaptics", "mode: the touchpad refused absolute mode");
        goto relative;
    }
    pkt_type = MODEL_NEWABS(info.model_id) ? PKT_NEWABS : PKT_OLDABS;
    klog(KLOG_INFO, "synaptics",
         "found: touchpad model %u, fw %u.%u, id 0x%x, caps 0x%x/0x%x/0x%x/0x%x, board id "
         "%u, fw id %u",
         ID_MODEL(info.identity), ID_MAJOR(info.identity), ID_MINOR(info.identity), info.model_id,
         info.capabilities, info.ext_cap, info.ext_cap_0c, info.ext_cap_10, info.board_id,
         info.firmware_id);
    if (register_pad() < 0) {
        klog(KLOG_WARN, "synaptics", "register: no event device for the touchpad");
        goto relative;
    }
    if (ps2mouse_port_rate() >= 80 && impaired_toshiba_kbc()) {
        klog(KLOG_INFO, "synaptics", "quirk: Toshiba %s detected, limiting rate to 40pps",
             dmi_field(DMI_PRODUCT_NAME));
        ps2mouse_port_rate_set(40);
    }
    initialize();
    mouse_attach_touchpad(pad);
    pktcnt = 0;
    return 1;

relative:
    if (pad) {
        evdev_unregister(pad);
        pad = 0;
    }
    mode_cmd(0); /* Linux's synaptics_reset: relative mode, gestures on */
    return -1;
}

/** Whether byte `idx` of the packet has the form of `type`, Linux's
 * `synaptics_validate_byte`. The sixth byte is not checked. */
static int valid_byte(int idx, int type)
{
    static const uint8_t newabs_mask[] = {0xC8, 0x00, 0x00, 0xC8, 0x00};
    static const uint8_t newabs_rel_mask[] = {0xC0, 0x00, 0x00, 0xC0, 0x00};
    static const uint8_t newabs_rslt[] = {0x80, 0x00, 0x00, 0xC0, 0x00};
    static const uint8_t oldabs_mask[] = {0xC0, 0x60, 0x00, 0xC0, 0x60};
    static const uint8_t oldabs_rslt[] = {0xC0, 0x00, 0x00, 0x80, 0x00};

    if (idx < 0 || idx > 4) {
        return 0;
    }
    switch (type) {
    case PKT_NEWABS:
    case PKT_NEWABS_RELAXED:
        return (packet[idx] & newabs_rel_mask[idx]) == newabs_rslt[idx];
    case PKT_NEWABS_STRICT:
        return (packet[idx] & newabs_mask[idx]) == newabs_rslt[idx];
    default:
        return (packet[idx] & oldabs_mask[idx]) == oldabs_rslt[idx];
    }
}

/** The form of the first whole packet, as Linux's
 * `synaptics_detect_pkt_type`: strict when its bits 3 are clear too. */
static int detect_pkt_type(void)
{
    int i;

    for (i = 0; i < 5; i++) {
        if (!valid_byte(i, PKT_NEWABS_STRICT)) {
            return PKT_NEWABS_RELAXED;
        }
    }
    return PKT_NEWABS_STRICT;
}

/** Y turned over, from the pad's way up to the screen's, as Linux's
 * `synaptics_invert_y`. */
static int invert_y(int y)
{
    return YMAX_NOMINAL + YMIN_NOMINAL - y;
}

/** Read an advanced gesture packet into `agm`, Linux's
 * `synaptics_parse_agm`: the second finger at half resolution, or the
 * fingers' count. */
static void parse_agm(const struct hw_state *hw)
{
    switch ((packet[5] & 0x30u) >> 4) {
    case 1:
        agm.w = hw->w;
        agm.x = (((packet[4] & 0x0f) << 8) | packet[1]) << 1;
        agm.y = (((packet[4] & 0xf0) << 4) | packet[2]) << 1;
        agm.z = ((packet[3] & 0x30) | (packet[5] & 0x0f)) << 1;
        break;
    case 2:
        agm_count = packet[1];
        break;
    default:
        break;
    }
}

/** Read the extra buttons of the packet into `hw`, Linux's
 * `synaptics_parse_ext_buttons`. */
static void parse_ext_buttons(struct hw_state *hw)
{
    unsigned int bits = (CAP_MULTI_BUTTON_NO(info.ext_cap) + 1) >> 1;
    unsigned int mask = (1u << bits) - 1u;

    hw->ext_buttons = (uint8_t)(packet[4] & mask);
    hw->ext_buttons |= (uint8_t)((packet[5] & mask) << bits);
}

/**
 * A ForcePad's left button, Linux's: a ForcePad gives the middle button's
 * bit for a press of one finger hard enough and also for more than one
 * finger, so only a single finger's press held for FORCEPAD_PRESS_MS
 * clicks, and the click stays until no finger touches.
 */
static int forcepad_left(const struct hw_state *hw)
{
    uint32_t now = (uint32_t)kernel_ticks;

    if (hw->z == 0) {
        press = report_press = 0;
    } else if (hw->w >= 4 && ((packet[0] ^ packet[3]) & 0x01)) {
        if (!press) {
            press_start = now;
            press = 1;
        } else if ((int32_t)(now - (press_start + MS_TO_TICKS(FORCEPAD_PRESS_MS))) > 0) {
            report_press = 1;
        }
    } else {
        press = 0;
    }
    return report_press;
}

/**
 * Read the packet into `hw`, Linux's `synaptics_parse_hw_state`. Returns 1
 * for an advanced gesture packet, which goes to `agm` and is not reported
 * itself.
 */
static int parse_hw_state(struct hw_state *hw)
{
    const uint8_t *b = packet;

    k_memset(hw, 0, sizeof(*hw));
    if (MODEL_NEWABS(info.model_id)) {
        hw->w = ((b[0] & 0x30) >> 2) | ((b[0] & 0x04) >> 1) | ((b[3] & 0x04) >> 2);
        if (has_agm() && hw->w == 2) {
            parse_agm(hw);
            return 1;
        }
        hw->x = ((b[3] & 0x10) << 8) | ((b[1] & 0x0f) << 8) | b[4];
        hw->y = ((b[3] & 0x20) << 7) | ((b[1] & 0xf0) << 4) | b[5];
        hw->z = b[2];
        hw->left = b[0] & 0x01;
        hw->right = (b[0] & 0x02) >> 1;
        if (is_forcepad) {
            hw->left = forcepad_left(hw);
        } else if (CAP_CLICKPAD(info.ext_cap_0c)) {
            /* A clickpad's click comes as the middle button. */
            hw->left = (b[0] ^ b[3]) & 0x01;
        } else if (CAP_MIDDLE_BUTTON(info.capabilities)) {
            hw->middle = (b[0] ^ b[3]) & 0x01;
            if (hw->w == 2) {
                hw->scroll = (int8_t)b[1];
            }
        }
        if (CAP_FOUR_BUTTON(info.capabilities)) {
            hw->up = (b[0] ^ b[3]) & 0x01;
            hw->down = ((b[0] ^ b[3]) & 0x02) >> 1;
        }
        if (CAP_MULTI_BUTTON_NO(info.ext_cap) > 0 && ((b[0] ^ b[3]) & 0x02)) {
            parse_ext_buttons(hw);
        }
    } else {
        hw->x = ((b[1] & 0x1f) << 8) | b[2];
        hw->y = ((b[4] & 0x1f) << 8) | b[5];
        hw->z = ((b[0] & 0x30) << 2) | (b[3] & 0x3f);
        hw->w = ((b[1] & 0x80) >> 4) | ((b[0] & 0x04) >> 1);
        hw->left = b[0] & 0x01;
        hw->right = (b[0] & 0x02) >> 1;
    }
    if (hw->x > X_MAX_POSITIVE) {
        hw->x -= 1 << ABS_POS_BITS;
    } else if (hw->x == X_MAX_POSITIVE) {
        hw->x = XMAX;
    }
    if (hw->y > Y_MAX_POSITIVE) {
        hw->y -= 1 << ABS_POS_BITS;
    } else if (hw->y == Y_MAX_POSITIVE) {
        hw->y = YMAX;
    }
    return 0;
}

/** Report the buttons of `hw`, Linux's `synaptics_report_buttons`; the
 * extra buttons of a pad that wires them to its pass-through port's stick
 * go to the stick's device, as Linux sends them out of band. */
static void report_buttons(const struct hw_state *hw)
{
    unsigned int bits = (CAP_MULTI_BUTTON_NO(info.ext_cap) + 1) >> 1;
    unsigned int i;

    evdev_report(pad, EV_KEY, BTN_LEFT, hw->left);
    evdev_report(pad, EV_KEY, BTN_RIGHT, hw->right);
    if (CAP_MIDDLE_BUTTON(info.capabilities)) {
        evdev_report(pad, EV_KEY, BTN_MIDDLE, hw->middle);
    }
    if (CAP_FOUR_BUTTON(info.capabilities)) {
        evdev_report(pad, EV_KEY, BTN_FORWARD, hw->up);
        evdev_report(pad, EV_KEY, BTN_BACK, hw->down);
    }
    if (!CAP_MULTI_BUTTON_NO(info.ext_cap)) {
        return;
    }
    /* Firmware 8.1 and 8.2 give the extra buttons only with the change bit. */
    if ((ID_FULL(info.identity) == 0x801 || ID_FULL(info.identity) == 0x802) &&
        !((packet[0] ^ packet[3]) & 0x02)) {
        return;
    }
    if (CAP_EXT_BUTTONS_STICK(info.ext_cap_10)) {
        if (CAP_PASS_THROUGH(info.capabilities)) {
            ps2mouse_guest_buttons((uint8_t)(STICK_L(hw->ext_buttons) |
                                             STICK_R(hw->ext_buttons) << 1 |
                                             STICK_M(hw->ext_buttons) << 2));
        }
        return;
    }
    for (i = 0; i < bits; i++) {
        evdev_report(pad, EV_KEY, (uint16_t)(BTN_0 + 2 * i), (hw->ext_buttons >> i) & 1u);
        evdev_report(pad, EV_KEY, (uint16_t)(BTN_1 + 2 * i), (hw->ext_buttons >> (i + bits)) & 1u);
    }
}

/** Report one slot of a semi-MT pad, Linux's
 * `synaptics_report_semi_mt_slot`. */
static void report_semi_mt_slot(int slot, int active, int x, int y)
{
    evdev_report(pad, EV_ABS, ABS_MT_SLOT, slot);
    evdev_mt_report_slot_state(pad, MT_TOOL_FINGER, active);
    if (active) {
        evdev_report(pad, EV_ABS, ABS_MT_POSITION_X, x);
        evdev_report(pad, EV_ABS, ABS_MT_POSITION_Y, invert_y(y));
    }
}

/** Report the box of the contacts of a semi-MT pad, Linux's
 * `synaptics_report_semi_mt_data`. */
static void report_semi_mt(const struct hw_state *a, const struct hw_state *b, int fingers)
{
    if (fingers >= 2) {
        report_semi_mt_slot(0, 1, a->x < b->x ? a->x : b->x, a->y < b->y ? a->y : b->y);
        report_semi_mt_slot(1, 1, a->x > b->x ? a->x : b->x, a->y > b->y ? a->y : b->y);
    } else if (fingers == 1) {
        report_semi_mt_slot(0, 1, a->x, a->y);
        report_semi_mt_slot(1, 0, 0, 0);
    } else {
        report_semi_mt_slot(0, 0, 0, 0);
        report_semi_mt_slot(1, 0, 0, 0);
    }
}

/**
 * Report the contacts of an image sensor, Linux's
 * `synaptics_report_mt_data`: the first finger and the AGM's second each
 * in the slot `evdev_mt_assign_slots` matches it to, the slots no contact
 * took lifted, the pointer of the oldest contact, the finger count the pad
 * gave, and the buttons.
 */
static void report_mt(const struct hw_state *sgm, int fingers)
{
    const struct hw_state *hw[2];
    struct evdev_mt_pos pos[2];
    int slot[2];
    int n, i;

    hw[0] = sgm;
    hw[1] = &agm;
    n = fingers < 0 ? 0 : fingers > 2 ? 2 : fingers;
    for (i = 0; i < n; i++) {
        pos[i].x = hw[i]->x;
        pos[i].y = invert_y(hw[i]->y);
    }
    evdev_mt_assign_slots(pad, slot, pos, n, DMAX * (int)info.x_res);
    for (i = 0; i < n; i++) {
        evdev_report(pad, EV_ABS, ABS_MT_SLOT, slot[i]);
        evdev_mt_report_slot_state(pad, MT_TOOL_FINGER, 1);
        evdev_report(pad, EV_ABS, ABS_MT_POSITION_X, pos[i].x);
        evdev_report(pad, EV_ABS, ABS_MT_POSITION_Y, pos[i].y);
        evdev_report(pad, EV_ABS, ABS_MT_PRESSURE, hw[i]->z);
    }
    evdev_mt_drop_unused(pad);
    evdev_mt_report_pointer_emulation(pad, 0);
    evdev_mt_report_finger_count(pad, fingers);
    report_buttons(sgm);
    evdev_report(pad, EV_SYN, SYN_REPORT, 0);
}

/** The fingers on an image sensor by its W and pressure, Linux's
 * `synaptics_image_sensor_process`. */
static int image_sensor_fingers(const struct hw_state *hw)
{
    if (hw->z == 0) {
        return 0;
    }
    if (hw->w >= 4) {
        return 1;
    }
    if (hw->w == 0) {
        return 2;
    }
    if (hw->w == 1) {
        return agm_count ? (int)agm_count : 3;
    }
    return 4;
}

/**
 * Report the middle button's scroll of a pad whose W is 2, Linux's: each
 * four steps are a press and release of BTN_BACK down or BTN_FORWARD up,
 * each its own packet.
 */
static void report_scroll(const struct hw_state *hw)
{
    scroll += hw->scroll;
    while (scroll >= 4) {
        evdev_report(pad, EV_KEY, BTN_BACK, !hw->down);
        evdev_report(pad, EV_SYN, SYN_REPORT, 0);
        evdev_report(pad, EV_KEY, BTN_BACK, hw->down);
        evdev_report(pad, EV_SYN, SYN_REPORT, 0);
        scroll -= 4;
    }
    while (scroll <= -4) {
        evdev_report(pad, EV_KEY, BTN_FORWARD, !hw->up);
        evdev_report(pad, EV_SYN, SYN_REPORT, 0);
        evdev_report(pad, EV_KEY, BTN_FORWARD, hw->up);
        evdev_report(pad, EV_SYN, SYN_REPORT, 0);
        scroll += 4;
    }
}

/** Report a whole packet, Linux's `synaptics_process_packet`. */
static void process_packet(void)
{
    struct hw_state hw;
    int fingers = 0, width = 0;

    if (parse_hw_state(&hw)) {
        return;
    }
    if (CAP_IMAGE_SENSOR(info.ext_cap_0c)) {
        report_mt(&hw, image_sensor_fingers(&hw));
        return;
    }
    if (hw.scroll) {
        report_scroll(&hw);
        return;
    }
    if (hw.z > 0 && hw.x > 1) {
        fingers = 1;
        width = 5;
        if (CAP_EXTENDED(info.capabilities)) {
            if (hw.w <= 1) {
                if (has_multifinger()) {
                    fingers = hw.w + 2;
                }
            } else if (hw.w >= 4 && CAP_PALMDETECT(info.capabilities)) {
                width = hw.w; /* W 2, a pen, is taken as a finger */
            }
        }
    }
    if (CAP_ADV_GESTURE(info.ext_cap_0c)) {
        report_semi_mt(&hw, &agm, fingers);
    }
    /* BTN_TOUCH first, which mousedev's conversion relies on. */
    if (hw.z > 30) {
        evdev_report(pad, EV_KEY, BTN_TOUCH, 1);
    }
    if (hw.z < 25) {
        evdev_report(pad, EV_KEY, BTN_TOUCH, 0);
    }
    if (fingers > 0) {
        evdev_report(pad, EV_ABS, ABS_X, hw.x);
        evdev_report(pad, EV_ABS, ABS_Y, invert_y(hw.y));
    }
    evdev_report(pad, EV_ABS, ABS_PRESSURE, hw.z);
    if (CAP_PALMDETECT(info.capabilities)) {
        evdev_report(pad, EV_ABS, ABS_TOOL_WIDTH, width);
    }
    evdev_report(pad, EV_KEY, BTN_TOOL_FINGER, fingers == 1);
    if (has_multifinger()) {
        evdev_report(pad, EV_KEY, BTN_TOOL_DOUBLETAP, fingers == 2);
        evdev_report(pad, EV_KEY, BTN_TOOL_TRIPLETAP, fingers == 3);
    }
    report_buttons(&hw);
    evdev_report(pad, EV_SYN, SYN_REPORT, 0);
}

/** Whether the packet is a pass-through port's, Linux's
 * `synaptics_is_pt_packet`. */
static int pt_packet(void)
{
    return (packet[0] & 0xFC) == 0x84 && (packet[3] & 0xCC) == 0xC4;
}

/** Give the pass-through packet's bytes to the guest, as Linux's
 * `synaptics_pass_pt_packet` gives them to the device that reports on the
 * port: bytes 1, 4 and 5, and 2 when its packets are four bytes. */
static void pass_pt_packet(void)
{
    uint8_t guest[4];

    guest[0] = packet[1];
    guest[1] = packet[4];
    guest[2] = packet[5];
    guest[3] = packet[2];
    ps2mouse_guest_packet(guest);
}

int synaptics_packet_start(void)
{
    return pktcnt == 0;
}

void synaptics_packet_restart(void)
{
    pktcnt = 0;
}

int synaptics_byte(uint8_t data)
{
    uint32_t now = (uint32_t)kernel_ticks;

    if (pktcnt && now - last_tick > MS_TO_TICKS(500)) {
        sync_losses++;
        pktcnt = 0;
    }
    last_tick = now;
    packet[pktcnt++] = data;
    if (pktcnt < 6) {
        if (!valid_byte(pktcnt - 1, pkt_type)) {
            sync_losses++;
            pktcnt = 0;
            return SYNAPTICS_BAD;
        }
        return SYNAPTICS_GOOD;
    }
    pktcnt = 0;
    if (pkt_type == PKT_NEWABS) {
        pkt_type = detect_pkt_type();
    }
    if (CAP_PASS_THROUGH(info.capabilities) && pt_packet()) {
        pass_pt_packet();
    } else {
        process_packet();
    }
    return SYNAPTICS_FULL;
}

int synaptics_has_pass_through(void)
{
    return pad && CAP_PASS_THROUGH(info.capabilities);
}

/**
 * Read the pass-through device's next byte: the next pass-through packet,
 * read whole, gives it in byte 1, and a packet of the pad's own between is
 * passed over, as Linux's handler passes it to the pad's device. Returns
 * the byte, or -1 when no packet came.
 */
static int pt_receive(void)
{
    int tries;

    for (tries = 0; tries < 8; tries++) {
        if (receive(packet, 6) < 0) {
            return -1;
        }
        if (pt_packet()) {
            return packet[1];
        }
    }
    return -1;
}

/** Send `cmd` to the pass-through device, Linux's `synaptics_pt_write`:
 * the byte sliced and the rate 0x28; its reply comes in a pass-through
 * packet. Returns the reply, or -1. */
static int pt_send(uint8_t cmd)
{
    if (sliced(cmd) < 0 || command_arg(CMD_SET_RATE, RATE_PASS_THROUGH) < 0) {
        return -1;
    }
    return pt_receive();
}

const struct ps2io synaptics_pt = {pt_send, pt_receive};

void synaptics_pt_activate(int packet_size)
{
    if (packet_size == 4) {
        mode |= MODE_FOUR_BYTE;
    } else {
        mode &= (uint8_t)~MODE_FOUR_BYTE;
    }
    if (mode_cmd(mode) < 0) {
        klog(KLOG_WARN, "synaptics", "pass-through: the touchpad refused the guest's packet size");
    }
}

int synaptics_reconnect(void)
{
    struct synaptics_info now;
    uint8_t reply[2];
    int retry = 0, error;

    do {
        if (command(CMD_RESET) == 0) {
            receive(reply, 2);
        }
        if (retry) {
            ps2mouse_msleep(1000); /* a pad may still be calibrating */
        }
        if (command(CMD_GET_ID) == 0) {
            receive(reply, 1);
        }
        error = detect();
    } while (error && ++retry < 3);
    if (error) {
        return -1;
    }
    if (query_hardware(&now) < 0) {
        klog(KLOG_WARN, "synaptics", "reconnect: the touchpad did not answer its queries");
        return -1;
    }
    if (set_mode() < 0) {
        klog(KLOG_WARN, "synaptics", "reconnect: the touchpad refused its mode");
        return -1;
    }
    if (now.identity != info.identity || now.model_id != info.model_id ||
        now.capabilities != info.capabilities || now.ext_cap != info.ext_cap) {
        klog(KLOG_WARN, "synaptics",
             "reconnect: another touchpad: id %u-%u, model %u-%u, caps %x-%x, ext %x-%x",
             info.identity, now.identity, info.model_id, now.model_id, info.capabilities,
             now.capabilities, info.ext_cap, now.ext_cap);
        return -1;
    }
    initialize();
    pktcnt = 0;
    return 0;
}

void synaptics_disconnect(void)
{
    if (pad) {
        evdev_unregister(pad);
        pad = 0;
        mode_cmd(0); /* Linux's synaptics_reset: relative mode, gestures on */
    }
    pktcnt = 0;
}
