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
 * three or more, 2 an advanced gesture packet of the second finger. Y grows
 * upward and is turned over for the event device. A touchpad with an image
 * sensor tracks the fingers itself, and its first and second contacts go to
 * two slots; another one reports one contact and the finger count.
 */

#include "synaptics.h"

#include "ps2mouse.h"
#include "../../evdev.h"
#include "../../klog.h"
#include "../../lib/kstring.h"
#include "../../mouse.h"
#include "../../timer.h"

/* The mouse commands the touchpad's requests are made of. */
#define CMD_SET_SCALE11 0xE6
#define CMD_SET_RES     0xE8
#define CMD_GET_INFO    0xE9 /* three bytes after the ACK */
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
#define MODE_W               0x01
#define RATE_SET_MODE2       0x14
#define RATE_AGM             0xc8

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
 * `synaptics_capability`. A failed extended query leaves its bits 0. */
static int query_capabilities(void)
{
    if (query_int(QUE_CAPABILITIES, &info.capabilities) < 0) {
        return -1;
    }
    info.ext_cap = info.ext_cap_0c = 0;
    if (ID_FULL(info.identity) < 0x705 && CAP_SUBMODEL_ID(info.capabilities) != 0x47) {
        return -1; /* older firmware had the submodel fixed at 0x47 */
    }
    if (!CAP_EXTENDED(info.capabilities)) {
        info.capabilities = 0;
    }
    if (EXT_CAP_REQUESTS(info.capabilities) >= 1) {
        if (query_int(QUE_EXT_CAPAB, &info.ext_cap) < 0) {
            info.ext_cap = 0;
            klog(KLOG_WARN, "synaptics", "query: the extended capabilities would not read");
        } else if (CAP_MULTI_BUTTON_NO(info.ext_cap) > 8) {
            info.ext_cap &= ~CAP_MB_MASK;
        }
    }
    if (EXT_CAP_REQUESTS(info.capabilities) >= 4) {
        if (query_int(QUE_EXT_CAPAB_0C, &info.ext_cap_0c) < 0) {
            info.ext_cap_0c = 0;
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
 * `synaptics_resolution`; what a pad does not give stays 0. */
static void query_resolution(void)
{
    uint8_t r[3];

    if (ID_MAJOR(info.identity) < 4) {
        return;
    }
    if (send_query(QUE_RESOLUTION, r) == 0 && r[0] != 0 && (r[1] & 0x80u) && r[2] != 0) {
        info.x_res = r[0];
        info.y_res = r[2];
    }
    if (EXT_CAP_REQUESTS(info.capabilities) >= 5 && CAP_MAX_DIMENSIONS(info.ext_cap_0c)) {
        if (send_query(QUE_EXT_MAX_COORDS, r) < 0) {
            klog(KLOG_WARN, "synaptics", "query: the maximum coordinates would not read");
        } else {
            coords(r, &info.x_max, &info.y_max);
        }
    }
    /* Firmware 8.1 gives too few extended queries, and right minimums. */
    if (CAP_MIN_DIMENSIONS(info.ext_cap_0c) &&
        (EXT_CAP_REQUESTS(info.capabilities) >= 7 || ID_FULL(info.identity) == 0x801)) {
        if (send_query(QUE_EXT_MIN_COORDS, r) < 0) {
            klog(KLOG_WARN, "synaptics", "query: the minimum coordinates would not read");
        } else {
            coords(r, &info.x_min, &info.y_min);
        }
    }
}

/** Every query, as Linux's `synaptics_query_hardware`; -1 when one that
 * Linux needs failed or the pad is not a Synaptics pad after all. */
static int query_hardware(void)
{
    uint8_t bid[3];

    k_memset(&info, 0, sizeof(info));
    if (query_int(QUE_IDENTIFY, &info.identity) < 0 || !ID_IS_SYNAPTICS(info.identity)) {
        return -1;
    }
    if (query_int(QUE_MODEL, &info.model_id) < 0 ||
        query_int(QUE_FIRMWARE_ID, &info.firmware_id) < 0) {
        return -1;
    }
    if (ID_FULL(info.identity) >= 0x705) { /* earlier firmware has no board */
        if (send_query(QUE_MODES, bid) < 0) {
            return -1;
        }
        info.board_id = (uint32_t)(bid[0] & 0xfcu) << 6 | bid[1];
        if (MEXT_CAP_BIT(bid[0]) && query_int(QUE_MEXT_CAPAB_10, &info.ext_cap_10) < 0) {
            return -1;
        }
    }
    if (query_capabilities() < 0) {
        return -1;
    }
    query_resolution();
    return 0;
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

/**
 * Set the mode as Linux's `synaptics_set_mode` and `synaptics_set_rate` do
 * for the rate of 100 psmouse asks: absolute mode, gestures off on
 * firmware 4 and later, 80 packets a second, W mode when the pad has the
 * extended capabilities; then the advanced gesture mode of a pad that has
 * it.
 */
static int set_mode(void)
{
    mode = MODE_ABSOLUTE | MODE_HIGH_RATE;
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
    }
    return 0;
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
    if (query_hardware() < 0) {
        klog(KLOG_WARN, "synaptics", "query: the touchpad did not answer");
        goto relative;
    }
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
    /* psmouse_initialize: the rate, 200 dpi and 1:1 scaling. */
    mode_cmd(mode);
    command_arg(CMD_SET_RES, 3);
    command(CMD_SET_SCALE11);
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
 * Read the packet into `hw`, Linux's `synaptics_parse_hw_state` without the
 * ForcePad's press, which only a PNP ID names. Returns 1 for an advanced
 * gesture packet, which goes to `agm` and is not reported itself.
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
        if (CAP_CLICKPAD(info.ext_cap_0c)) {
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
 * extra buttons of a pad that wires them to a pass-through port's stick are
 * not reported, since that port is not driven. */
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
    if (!CAP_MULTI_BUTTON_NO(info.ext_cap) || CAP_EXT_BUTTONS_STICK(info.ext_cap_10)) {
        return;
    }
    /* Firmware 8.1 and 8.2 give the extra buttons only with the change bit. */
    if ((ID_FULL(info.identity) == 0x801 || ID_FULL(info.identity) == 0x802) &&
        !((packet[0] ^ packet[3]) & 0x02)) {
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

void synaptics_byte(uint8_t data)
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
        }
        return;
    }
    pktcnt = 0;
    if (pkt_type == PKT_NEWABS) {
        pkt_type = detect_pkt_type();
    }
    /* A stick's packet on the pass-through port, which is not driven. */
    if (CAP_PASS_THROUGH(info.capabilities) && pt_packet()) {
        return;
    }
    process_packet();
}
