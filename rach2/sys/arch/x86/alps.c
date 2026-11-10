/*
 * alps.c: the ALPS touchpad on the PS/2 mouse's port, identified, set up and
 * read as Linux 6.12's psmouse and alps drivers do (alps.h).
 *
 * Every ALPS pad answers the "E6 report", a resolution of 0, three 1:1
 * scalings and a status request, with 00 00 0A or 00 00 64; its "E7 report",
 * the same with 2:1 scalings, gives the model's signature, and its "EC
 * report", with three wrap-mode resets, the firmware's. The old models are
 * rows of a table of E7 signatures; the newer ones, protocol versions 3 to
 * 8, are told apart by the EC report. Versions 3 to 8 have a command mode,
 * entered by the EC report and left by stream mode (0xEA), in which a
 * register of a 16-bit address space is read and written a nibble at a
 * time, each nibble a PS/2 command of the version's table. Version 6 writes
 * its one register in a monitor mode instead, and versions 1 and 2 take a
 * knock of four disables and an enable.
 *
 * In absolute mode a pad sends six-byte packets, eight on version 4, whose
 * forms Linux's Documentation/input/devices/alps.rst draws: one finger's
 * place and pressure, and on versions 3 to 5 a second packet of bitmaps of
 * the electrodes the fingers cover, which give the box of two contacts;
 * versions 7 and 8 give two contacts' places in a packet and the third and
 * fourth in the next. A DualPoint's stick sends packets of its own among the
 * pad's, or the PS/2 packets of a mouse, which a version 2 pad may cut into
 * the middle of its own.
 *
 * Linux's psmouse resynchronizes and resets a pad whose bytes lose their
 * form, and polls it after a pause; that is psmouse's, and this file drops
 * the packet, as Linux's ALPS handler does, and goes on.
 */

#include "alps.h"

#include "ps2mouse.h"
#include "smbios.h"
#include "../../evdev.h"
#include "../../klog.h"
#include "../../lib/kstring.h"
#include "../../mouse.h"
#include "../../timer.h"

/*
 * The PS/2 commands as Linux's libps2 encodes them: the command's byte in
 * the low eight bits, the bytes received after its ACK in bits 8 to 11, the
 * bytes sent after it in bits 12 to 15.
 */
#define CMD_SETSCALE11 0x00e6
#define CMD_SETSCALE21 0x00e7
#define CMD_SETRES     0x10e8
#define CMD_GETINFO    0x03e9
#define CMD_SETSTREAM  0x00ea
#define CMD_RESET_WRAP 0x00ec
#define CMD_SETPOLL    0x00f0
#define CMD_GETID      0x02f2 /* a mouse answers one byte, a keyboard two */
#define CMD_SETRATE    0x10f3
#define CMD_ENABLE     0x00f4
#define CMD_DISABLE    0x00f5
#define CMD_RESET_DIS  0x00f6
#define CMD_RESET_BAT  0x02ff
#define CMD_NIBBLE_10  0x01f2 /* GETID, its one byte read */
#define CMD_TP_READ_ID 0x02e1 /* a TrackPoint's identity */
#define CMD_POLL       0x00eb /* a packet after the ACK, its size in bits 8 to 11 */

/* The protocol versions, which Linux gives as the event devices' version. */
#define PROTO_V1          0x100
#define PROTO_V2          0x200
#define PROTO_V3          0x300
#define PROTO_V3_RUSHMORE 0x310
#define PROTO_V4          0x400
#define PROTO_V5          0x500
#define PROTO_V6          0x600
#define PROTO_V7          0x700
#define PROTO_V8          0x800
#define PROTO_V9          0x900

/* What a model has, Linux's ALPS_* flags. */
#define F_DUALPOINT       0x02  /* a stick */
#define F_PASS            0x04  /* a pass-through port, the pad behind the stick */
#define F_WHEEL           0x08  /* a wheel */
#define F_FW_BK_1         0x10  /* forward and back buttons, in one place */
#define F_FW_BK_2         0x20  /* forward and back buttons, in another */
#define F_FOUR_BUTTONS    0x40  /* four direction buttons */
#define F_PS2_INTERLEAVED 0x80  /* a stick's PS/2 packet within the pad's */
#define F_STICK_BITS      0x100 /* the stick's buttons in bits of their own */
#define F_BUTTONPAD       0x200 /* a clickpad */
#define F_STICK_PRESSURE  0x400 /* the stick reports its pressure */

/* A stick's buttons seen in its own packets, Linux's quirk. */
#define QUIRK_STICK_BUTTONS 1

/* The register bases of command mode. */
#define REG_BASE_RUSHMORE 0xc2c0
#define REG_BASE_V7       0xc2c0
#define REG_BASE_PINNACLE 0x0000

/* The most contacts a pad reports. */
#define MAX_TOUCHES 4

/* The geometry of version 5's and version 8's sensors. */
#define DOLPHIN_COUNT_PER_ELECTRODE 64
#define DOLPHIN_PROFILE_XOFFSET     8
#define DOLPHIN_PROFILE_YOFFSET     1
#define SS4_COUNT_PER_ELECTRODE     256
#define SS4_NUMSENSOR_XOFFSET       7
#define SS4_NUMSENSOR_YOFFSET       7
#define SS4_MIN_PITCH_MM            50
#define SS4PLUS_COUNT_PER_ELECTRODE 128
#define SS4PLUS_NUMSENSOR_XOFFSET   16
#define SS4PLUS_NUMSENSOR_YOFFSET   5
#define SS4PLUS_MIN_PITCH_MM        37

/* The places a version 8 pad gives a third and fourth contact it does not
 * have, with buttons and without. */
#define SS4_MFPACKET_NO_AX         8160
#define SS4_MFPACKET_NO_AY         4080
#define SS4_MFPACKET_NO_AX_BL      8176
#define SS4_MFPACKET_NO_AY_BL      4088
#define SS4_PLUS_MFPACKET_NO_AX    4080
#define SS4_PLUS_MFPACKET_NO_AX_BL 4088

/* Version 8's packets, by bits 4 and 5 of byte 3. */
#define SS4_PACKET_ID_IDLE  0
#define SS4_PACKET_ID_ONE   1
#define SS4_PACKET_ID_TWO   2
#define SS4_PACKET_ID_MULTI 3
#define SS4_PACKET_ID_STICK 4

/* Version 7's packets. */
#define V7_PACKET_ID_IDLE    0
#define V7_PACKET_ID_TWO     1
#define V7_PACKET_ID_MULTI   2
#define V7_PACKET_ID_NEW     3
#define V7_PACKET_ID_UNKNOWN 4

/* What `process_byte` makes of a byte, psmouse's psmouse_ret_t. */
#define BAD_DATA    0
#define GOOD_DATA   1
#define FULL_PACKET 2

/* The protocol numbers of Linux's `enum psmouse_type`, which EVIOCGID gives
 * as the product, and the vendor psmouse gives every mouse. */
#define PSMOUSE_PS2    1
#define PSMOUSE_ALPS   8
#define PSMOUSE_VENDOR 0x0002

/* Key codes and a property that evdev.h does not name. */
#define BTN_2                     0x102
#define BTN_3                     0x103
#define INPUT_PROP_POINTING_STICK 0x05

/* A nibble of a register's address or value, sent as a PS/2 command and
 * the byte after it, when the command takes one. */
struct nibble {
    unsigned int command;
    uint8_t data;
};

static const struct nibble v3_nibbles[16] = {
    {0x00f0, 0x00},      {CMD_RESET_DIS, 0x00}, {CMD_SETSCALE21, 0x00}, {CMD_SETRATE, 0x0a},
    {CMD_SETRATE, 0x14}, {CMD_SETRATE, 0x28},   {CMD_SETRATE, 0x3c},    {CMD_SETRATE, 0x50},
    {CMD_SETRATE, 0x64}, {CMD_SETRATE, 0xc8},   {CMD_NIBBLE_10, 0x00},  {CMD_SETRES, 0x00},
    {CMD_SETRES, 0x01},  {CMD_SETRES, 0x02},    {CMD_SETRES, 0x03},     {CMD_SETSCALE11, 0x00}};

static const struct nibble v4_nibbles[16] = {
    {CMD_ENABLE, 0x00},  {CMD_RESET_DIS, 0x00}, {CMD_SETSCALE21, 0x00}, {CMD_SETRATE, 0x0a},
    {CMD_SETRATE, 0x14}, {CMD_SETRATE, 0x28},   {CMD_SETRATE, 0x3c},    {CMD_SETRATE, 0x50},
    {CMD_SETRATE, 0x64}, {CMD_SETRATE, 0xc8},   {CMD_NIBBLE_10, 0x00},  {CMD_SETRES, 0x00},
    {CMD_SETRES, 0x01},  {CMD_SETRES, 0x02},    {CMD_SETRES, 0x03},     {CMD_SETSCALE11, 0x00}};

static const struct nibble v6_nibbles[16] = {
    {CMD_ENABLE, 0x00},  {CMD_SETRATE, 0x0a}, {CMD_SETRATE, 0x14},    {CMD_SETRATE, 0x28},
    {CMD_SETRATE, 0x3c}, {CMD_SETRATE, 0x50}, {CMD_SETRATE, 0x64},    {CMD_SETRATE, 0xc8},
    {CMD_GETID, 0x00},   {CMD_GETINFO, 0x00}, {CMD_SETRES, 0x00},     {CMD_SETRES, 0x01},
    {CMD_SETRES, 0x02},  {CMD_SETRES, 0x03},  {CMD_SETSCALE21, 0x00}, {CMD_SETSCALE11, 0x00}};

/* A protocol: its version, the mask and value byte 0 of a packet has, and
 * the model's F_* flags, Linux's `struct alps_protocol_info`. */
struct protocol {
    uint16_t version;
    uint8_t byte0;
    uint8_t mask0;
    unsigned int flags;
};

/* A model of the table, by its E7 signature. */
struct model {
    uint8_t signature[3];
    struct protocol protocol;
};

/* Linux 6.12's `alps_model_data`. */
static const struct model models[] = {
    {{0x20, 0x02, 0x0e}, {PROTO_V2, 0xf8, 0xf8, F_PASS | F_DUALPOINT}},
    {{0x22, 0x02, 0x0a}, {PROTO_V2, 0xf8, 0xf8, F_PASS | F_DUALPOINT}},
    {{0x22, 0x02, 0x14}, {PROTO_V2, 0xff, 0xff, F_PASS | F_DUALPOINT}}, /* Dell Latitude D600 */
    {{0x32, 0x02, 0x14}, {PROTO_V2, 0xf8, 0xf8, F_PASS | F_DUALPOINT}}, /* Toshiba Pro M10 */
    {{0x33, 0x02, 0x0a}, {PROTO_V1, 0x88, 0xf8, 0}},                    /* UMAX-530T */
    {{0x52, 0x01, 0x14},
     {PROTO_V2, 0xff, 0xff, F_PASS | F_DUALPOINT | F_PS2_INTERLEAVED}}, /* Toshiba A11-11L */
    {{0x53, 0x02, 0x0a}, {PROTO_V2, 0xf8, 0xf8, 0}},
    {{0x53, 0x02, 0x14}, {PROTO_V2, 0xf8, 0xf8, 0}},
    {{0x60, 0x03, 0xc8}, {PROTO_V2, 0xf8, 0xf8, 0}}, /* HP ze1115 */
    {{0x62, 0x02, 0x14},
     {PROTO_V2, 0xcf, 0xcf, F_PASS | F_DUALPOINT | F_PS2_INTERLEAVED}}, /* Dell E5500, E6400 */
    {{0x63, 0x02, 0x0a}, {PROTO_V2, 0xf8, 0xf8, 0}},
    {{0x63, 0x02, 0x14}, {PROTO_V2, 0xf8, 0xf8, 0}},
    {{0x63, 0x02, 0x28}, {PROTO_V2, 0xf8, 0xf8, F_FW_BK_2}}, /* Fujitsu S6010 */
    {{0x63, 0x02, 0x3c}, {PROTO_V2, 0x8f, 0x8f, F_WHEEL}},   /* Toshiba S2400 */
    {{0x63, 0x02, 0x50}, {PROTO_V2, 0xef, 0xef, F_FW_BK_1}}, /* NEC Versa L320 */
    {{0x63, 0x02, 0x64}, {PROTO_V2, 0xf8, 0xf8, 0}},
    {{0x63, 0x03, 0xc8}, {PROTO_V2, 0xf8, 0xf8, F_PASS | F_DUALPOINT}}, /* Dell Latitude D800 */
    {{0x73, 0x00, 0x0a}, {PROTO_V2, 0xf8, 0xf8, F_DUALPOINT}},          /* ThinkPad R61 */
    {{0x73, 0x00, 0x14}, {PROTO_V6, 0xff, 0xff, F_DUALPOINT}},          /* Dell XT2 */
    {{0x73, 0x02, 0x0a}, {PROTO_V2, 0xf8, 0xf8, 0}},
    {{0x73, 0x02, 0x14}, {PROTO_V2, 0xf8, 0xf8, F_FW_BK_2}},       /* Ahtec Laptop */
    {{0x73, 0x02, 0x50}, {PROTO_V2, 0xcf, 0xcf, F_FOUR_BUTTONS}}}; /* Dell Vostro 1400 */

/* The protocols told apart by the EC report. */
static const struct protocol v3_protocol = {PROTO_V3, 0x8f, 0x8f, F_DUALPOINT | F_STICK_PRESSURE};
static const struct protocol v3_rushmore = {PROTO_V3_RUSHMORE, 0x8f, 0x8f,
                                            F_DUALPOINT | F_STICK_PRESSURE};
static const struct protocol v4_protocol = {PROTO_V4, 0x8f, 0x8f, 0};
static const struct protocol v5_protocol = {PROTO_V5, 0xc8, 0xd8, 0};
static const struct protocol v7_protocol = {PROTO_V7, 0x48, 0x48, F_DUALPOINT | F_STICK_PRESSURE};
static const struct protocol v8_protocol = {PROTO_V8, 0x18, 0x18, 0};

/* The Dell Latitudes whose version 2 pads give the stick's buttons in bits
 * of their own, Linux's `alps_dmi_has_separate_stick_buttons`. */
static const char *const stick_bits_products[] = {"Latitude D420", "Latitude D430", "Latitude D620",
                                                  "Latitude D630"};

/* A packet's reading, Linux's `struct alps_fields`. */
struct fields {
    unsigned int x_map; /* the X electrodes covered, a bit each */
    unsigned int y_map;
    unsigned int fingers;
    int pressure;
    struct evdev_mt_pos st;              /* the one finger's place */
    struct evdev_mt_pos mt[MAX_TOUCHES]; /* the contacts' places */
    unsigned int first_mp : 1;           /* the first of two packets */
    unsigned int is_mp : 1;              /* the second of two */
    unsigned int left : 1;
    unsigned int right : 1;
    unsigned int middle : 1;
    unsigned int ts_left : 1; /* the stick's buttons */
    unsigned int ts_right : 1;
    unsigned int ts_middle : 1;
};

/* A run of set bits in an electrode bitmap. */
struct bitmap_point {
    int start_bit;
    int num_bits;
};

/* What identification and set-up found, and the state between packets,
 * Linux's `struct alps_data`. */
static struct {
    const struct nibble *nibbles; /* command mode's nibbles */
    unsigned int addr_command;    /* the command an address follows */
    uint16_t proto_version;       /* PROTO_* */
    uint8_t byte0, mask0;         /* byte 0 of a packet, masked, is byte0 */
    uint8_t dev_id[3];            /* the E7 report */
    uint8_t fw_ver[3];            /* the EC report */
    unsigned int flags;           /* F_* */
    int x_max, y_max;             /* the largest place */
    int x_bits, y_bits;           /* the electrodes of each bitmap */
    unsigned int x_res, y_res;    /* units a millimetre, 0 when not known */
    int (*hw_init)(void);
    void (*process_packet)(void);
    int (*decode_fields)(struct fields *f, const uint8_t *p);
    void (*set_abs_params)(void);
    int prev_fin;     /* version 1 and 2: the finger bit of the last packet */
    int multi_packet; /* a packet of two, or of version 4's three, under way */
    int second_touch; /* the bitmaps' corner of the second contact, -1 none */
    uint8_t multi_data[6];
    struct fields f;
    uint8_t quirks; /* QUIRK_* */
    int semi_mt;    /* the slots bound the contacts' box */
} priv;

/* The event devices: the pad's, the stick's, and the external port's
 * mouse's, which the first PS/2 packet from it registers. */
static struct evdev *pad;
static struct evdev *stick;
static struct evdev *bare;
static int bare_failed; /* the external mouse's registration failed */

/* The packet being assembled, psmouse's: its bytes, how many have come, its
 * size, the tick of the last, and the bad packets since the last good one.
 * A stick's PS/2 packet cut into a pad's makes the packet one byte longer
 * than its size before it is taken apart. */
static uint8_t packet[8];
static int pktcnt;
static int pktsize = 6;
static uint32_t last_tick;
static unsigned int out_of_sync;

/* The flush of an interleaving pad's last packet: armed, and its tick. */
static int flush_armed;
static uint32_t flush_at;

/* The packets dropped for a pause, which psmouse logs as lost
 * synchronization, and the stick packets of a pad without a stick, which
 * are logged once. */
static unsigned int sync_losses;
static unsigned int rejected_stick;

/* The name `alps_mouse_name` builds. */
static char mouse_name[48];

/**
 * Send `command`, encoded as Linux's libps2 encodes it, with its bytes from
 * `param`, and read the bytes it answers into `param`, as Linux's
 * `ps2_command`: a mouse answers GETID with one byte, and a reset with
 * 0xAA alone. Returns 0, or -1 when the device did not acknowledge a byte
 * or an answer did not come.
 */
static int ps2_command(uint8_t *param, unsigned int command)
{
    unsigned int send = (command >> 12) & 0xfu;
    unsigned int receive = (command >> 8) & 0xfu;
    unsigned int i;
    int b;

    if (ps2mouse_send((uint8_t)command) != PS2MOUSE_ACK) {
        return -1;
    }
    for (i = 0; i < send; i++) {
        if (ps2mouse_send(param[i]) != PS2MOUSE_ACK) {
            return -1;
        }
    }
    for (i = 0; i < receive; i++) {
        b = ps2mouse_receive();
        if (b < 0) {
            return command == CMD_RESET_BAT && i == 1 ? 0 : -1;
        }
        param[i] = (uint8_t)b;
        if (command == CMD_GETID && i == 0 && b != 0xab && b != 0xac) {
            break;
        }
    }
    return 0;
}

/** Linux's `psmouse_reset`: the reset, whose answer is 0xAA and the ID 0. */
static int psmouse_reset(void)
{
    uint8_t param[2];

    param[0] = param[1] = 0;
    if (ps2_command(param, CMD_RESET_BAT) < 0) {
        return -1;
    }
    return param[0] != 0xaa && param[1] != 0x00 ? -1 : 0;
}

/** Read up to `count` bytes the device sends unasked and drop them, as
 * Linux's `ps2_drain`. */
static void ps2_drain(int count)
{
    while (count-- > 0 && ps2mouse_receive() >= 0) {
    }
}

/** The low four bits of `v` as a signed number. */
static int sign4(int v)
{
    v &= 0x0f;
    return v & 0x08 ? v - 16 : v;
}

/** Whether `data` can be byte 0 of a packet. */
static int valid_first_byte(uint8_t data)
{
    return (data & priv.mask0) == priv.byte0;
}

/** Report the key `code` of `dev` held while `value` is not 0, as Linux's
 * `input_report_key`. */
static void report_key(struct evdev *dev, uint16_t code, int value)
{
    evdev_report(dev, EV_KEY, code, value != 0);
}

/** End the packet of `dev`. */
static void sync(struct evdev *dev)
{
    evdev_report(dev, EV_SYN, SYN_REPORT, 0);
}

/**
 * Report the buttons shared by the pad and the stick, Linux's
 * `alps_report_buttons`: on `dev1`, unless `dev2` holds the button down,
 * so that its release goes where its press went; `dev2`'s packet is ended
 * here and `dev1`'s by the caller.
 */
static void report_buttons(struct evdev *dev1, struct evdev *dev2, int left, int right, int middle)
{
    report_key(dev2 && evdev_key_down(dev2, BTN_LEFT) ? dev2 : dev1, BTN_LEFT, left);
    report_key(dev2 && evdev_key_down(dev2, BTN_RIGHT) ? dev2 : dev1, BTN_RIGHT, right);
    report_key(dev2 && evdev_key_down(dev2, BTN_MIDDLE) ? dev2 : dev1, BTN_MIDDLE, middle);
    if (dev2) {
        sync(dev2);
    }
}

/** Report the three buttons of a PS/2 packet's byte 0, Linux's
 * `psmouse_report_standard_buttons`. */
static void report_standard_buttons(struct evdev *dev, uint8_t buttons)
{
    report_key(dev, BTN_LEFT, buttons & 0x01);
    report_key(dev, BTN_MIDDLE, buttons & 0x04);
    report_key(dev, BTN_RIGHT, buttons & 0x02);
}

/** Log a stick packet from a pad without a stick, once, as Linux warns. */
static void reject_stick(void)
{
    if (rejected_stick++ == 0) {
        klog(KLOG_WARN, "alps", "rejected trackstick packet from non DualPoint device");
    }
}

/**
 * Report a packet of version 1 or 2, Linux's `alps_process_packet_v1_v2`:
 * a stick's, pressure 127 on a DualPoint, as motion of the stick; the
 * pad's as its finger's place and pressure, a hardware tap given pressure
 * 40, and a tap and drag given the lift between.
 */
static void process_packet_v1_v2(void)
{
    const uint8_t *p = packet;
    int x, y, z, ges, fin, left, right, middle;
    int back = 0, forward = 0;

    if (priv.proto_version == PROTO_V1) {
        left = p[2] & 0x10;
        right = p[2] & 0x08;
        middle = 0;
        x = p[1] | ((p[0] & 0x07) << 7);
        y = p[4] | ((p[3] & 0x07) << 7);
        z = p[5];
    } else {
        left = p[3] & 1;
        right = p[3] & 2;
        middle = p[3] & 4;
        x = p[1] | ((p[2] & 0x78) << (7 - 3));
        y = p[4] | ((p[3] & 0x70) << (7 - 4));
        z = p[5];
    }
    if (priv.flags & F_FW_BK_1) {
        back = p[0] & 0x10;
        forward = p[2] & 4;
    }
    if (priv.flags & F_FW_BK_2) {
        back = p[3] & 4;
        forward = p[2] & 4;
        middle = forward && back;
        if (middle) {
            forward = back = 0;
        }
    }
    ges = p[2] & 1;
    fin = p[2] & 2;

    if ((priv.flags & F_DUALPOINT) && z == 127) {
        evdev_report(stick, EV_REL, REL_X, x > 383 ? x - 768 : x);
        evdev_report(stick, EV_REL, REL_Y, -(y > 255 ? y - 512 : y));
        report_buttons(stick, pad, left, right, middle);
        sync(stick);
        return;
    }
    if (priv.flags & F_STICK_BITS) {
        left |= p[0] & 1;
        right |= p[0] & 2;
        middle |= p[0] & 4;
    }
    report_buttons(pad, stick, left, right, middle);
    if (ges && !fin) {
        z = 40;
    }
    if (ges && fin && !priv.prev_fin) {
        evdev_report(pad, EV_ABS, ABS_X, x);
        evdev_report(pad, EV_ABS, ABS_Y, y);
        evdev_report(pad, EV_ABS, ABS_PRESSURE, 0);
        report_key(pad, BTN_TOOL_FINGER, 0);
        sync(pad);
    }
    priv.prev_fin = fin;
    if (z > 30) {
        report_key(pad, BTN_TOUCH, 1);
    }
    if (z < 25) {
        report_key(pad, BTN_TOUCH, 0);
    }
    if (z > 0) {
        evdev_report(pad, EV_ABS, ABS_X, x);
        evdev_report(pad, EV_ABS, ABS_Y, y);
    }
    evdev_report(pad, EV_ABS, ABS_PRESSURE, z);
    report_key(pad, BTN_TOOL_FINGER, z > 0);
    if (priv.flags & F_WHEEL) {
        evdev_report(pad, EV_REL, REL_WHEEL, ((p[2] << 1) & 0x08) - ((p[0] >> 4) & 0x07));
    }
    if (priv.flags & (F_FW_BK_1 | F_FW_BK_2)) {
        report_key(pad, BTN_FORWARD, forward);
        report_key(pad, BTN_BACK, back);
    }
    if (priv.flags & F_FOUR_BUTTONS) {
        report_key(pad, BTN_0, p[2] & 4);
        report_key(pad, BTN_1, p[0] & 0x10);
        report_key(pad, BTN_2, p[3] & 4);
        report_key(pad, BTN_3, p[0] & 0x20);
    }
    sync(pad);
}

/** Find the first and second runs of set bits of `map`, counting the runs
 * in `*fingers`, Linux's `alps_get_bitmap_points`. */
static void get_bitmap_points(unsigned int map, struct bitmap_point *low, struct bitmap_point *high,
                              int *fingers)
{
    struct bitmap_point *point = low;
    int i, bit, prev_bit = 0;

    for (i = 0; map != 0; i++, map >>= 1) {
        bit = map & 1;
        if (bit) {
            if (!prev_bit) {
                point->start_bit = i;
                point->num_bits = 0;
                (*fingers)++;
            }
            point->num_bits++;
        } else if (prev_bit) {
            point = high;
        }
        prev_bit = bit;
    }
}

/** The place on an axis of `max` units and `bits` electrodes of the middle
 * of the run `pt`. */
static int bitmap_place(int max, int bits, const struct bitmap_point *pt)
{
    return (max * (2 * pt->start_bit + pt->num_bits - 1)) / (2 * (bits - 1));
}

/**
 * The contacts of the bitmaps in `f`, Linux's `alps_process_bitmap`: the
 * box of the runs the fingers cover, one run cut in two when an axis has
 * one, with the finger of the place packet in one corner and the second
 * contact in the corner across from it, chosen once a touch. Returns the
 * fingers counted, 0 when a bitmap is empty.
 */
static int process_bitmap(struct fields *f)
{
    struct bitmap_point x_low, x_high, y_low, y_high;
    struct evdev_mt_pos corner[4];
    int i, fingers_x = 0, fingers_y = 0, fingers, closest;

    if (!f->x_map || !f->y_map) {
        return 0;
    }
    k_memset(&x_low, 0, sizeof(x_low));
    k_memset(&x_high, 0, sizeof(x_high));
    k_memset(&y_low, 0, sizeof(y_low));
    k_memset(&y_high, 0, sizeof(y_high));
    get_bitmap_points(f->x_map, &x_low, &x_high, &fingers_x);
    get_bitmap_points(f->y_map, &y_low, &y_high, &fingers_y);
    fingers = fingers_x > fingers_y ? fingers_x : fingers_y;
    if (fingers_x == 1) {
        i = (x_low.num_bits - 1) / 2;
        x_low.num_bits = x_low.num_bits - i;
        x_high.start_bit = x_low.start_bit + i;
        x_high.num_bits = i > 1 ? i : 1;
    }
    if (fingers_y == 1) {
        i = (y_low.num_bits - 1) / 2;
        y_low.num_bits = y_low.num_bits - i;
        y_high.start_bit = y_low.start_bit + i;
        y_high.num_bits = i > 1 ? i : 1;
    }
    corner[0].x = bitmap_place(priv.x_max, priv.x_bits, &x_low);
    corner[0].y = bitmap_place(priv.y_max, priv.y_bits, &y_low);
    corner[1].x = bitmap_place(priv.x_max, priv.x_bits, &x_high);
    corner[1].y = corner[0].y;
    corner[2].x = corner[1].x;
    corner[2].y = bitmap_place(priv.y_max, priv.y_bits, &y_high);
    corner[3].x = corner[0].x;
    corner[3].y = corner[2].y;
    if (priv.proto_version == PROTO_V5) { /* its X bitmap runs the other way */
        for (i = 0; i < 4; i++) {
            corner[i].x = priv.x_max - corner[i].x;
        }
    }
    if (priv.proto_version == PROTO_V3 || priv.proto_version == PROTO_V4) {
        for (i = 0; i < 4; i++) {
            corner[i].y = priv.y_max - corner[i].y;
        }
    }
    if (priv.second_touch == -1) {
        closest = 0x7fffffff;
        for (i = 0; i < 4; i++) {
            int dx = f->st.x - corner[i].x;
            int dy = f->st.y - corner[i].y;
            int distance = dx * dx + dy * dy;

            if (distance < closest) {
                priv.second_touch = i;
                closest = distance;
            }
        }
        priv.second_touch = (priv.second_touch + 2) % 4;
    }
    f->mt[0] = f->st;
    f->mt[1] = corner[priv.second_touch];
    return fingers;
}

/** Put a contact at `x`, `y` in `slot` of the pad, Linux's `alps_set_slot`. */
static void set_slot(int slot, int x, int y)
{
    evdev_report(pad, EV_ABS, ABS_MT_SLOT, slot);
    evdev_mt_report_slot_state(pad, MT_TOOL_FINGER, 1);
    evdev_report(pad, EV_ABS, ABS_MT_POSITION_X, x);
    evdev_report(pad, EV_ABS, ABS_MT_POSITION_Y, y);
}

/** End the slots' frame, Linux's `input_mt_sync_frame` for slots set up to
 * drop the unused: the pointer with the finger count unless the slots are
 * a semi-MT box. */
static void sync_frame(void)
{
    evdev_mt_drop_unused(pad);
    evdev_mt_report_pointer_emulation(pad, !priv.semi_mt);
}

/** Report the `n` contacts of `priv.f.mt`, each in the slot that tracks it,
 * Linux's `alps_report_mt_data`. */
static void report_mt_data(int n)
{
    int i, slot[MAX_TOUCHES];

    evdev_mt_assign_slots(pad, slot, priv.f.mt, n, 0);
    for (i = 0; i < n; i++) {
        set_slot(slot[i], priv.f.mt[i].x, priv.f.mt[i].y);
    }
    sync_frame();
}

/** Report the box of a semi-MT pad's contacts, or its one finger, with the
 * buttons and pressure, Linux's `alps_report_semi_mt_data`. */
static void report_semi_mt_data(int fingers)
{
    struct fields *f = &priv.f;

    if (fingers < 2) {
        f->mt[0].x = f->st.x;
        f->mt[0].y = f->st.y;
        fingers = f->pressure > 0 ? 1 : 0;
        priv.second_touch = -1;
    }
    if (fingers >= 1) {
        set_slot(0, f->mt[0].x, f->mt[0].y);
    }
    if (fingers >= 2) {
        set_slot(1, f->mt[1].x, f->mt[1].y);
    }
    sync_frame();
    evdev_mt_report_finger_count(pad, fingers);
    report_key(pad, BTN_LEFT, f->left);
    report_key(pad, BTN_RIGHT, f->right);
    report_key(pad, BTN_MIDDLE, f->middle);
    evdev_report(pad, EV_ABS, ABS_PRESSURE, f->pressure);
    sync(pad);
}

/**
 * Report a version 3 stick packet, Linux's
 * `alps_process_trackstick_packet_v3`: its motion an eighth of what it
 * gives, its pressure, and its buttons once a press has come in a stick
 * packet, since most models give them in the pad's.
 */
static void process_trackstick_packet_v3(void)
{
    const uint8_t *p = packet;
    int x, y, z, left, right, middle;

    if (!(priv.flags & F_DUALPOINT)) {
        reject_stick();
        return;
    }
    if (!(p[0] & 0x40)) {
        return;
    }
    if (p[1] == 0x7f && p[2] == 0x7f && p[4] == 0x7f) {
        return; /* the end of a run of stick packets */
    }
    x = (int8_t)(((p[0] & 0x20) << 2) | (p[1] & 0x7f));
    y = (int8_t)(((p[0] & 0x10) << 3) | (p[2] & 0x7f));
    z = p[4] & 0x7f;
    x /= 8;
    y /= 8;
    evdev_report(stick, EV_REL, REL_X, x);
    evdev_report(stick, EV_REL, REL_Y, -y);
    evdev_report(stick, EV_ABS, ABS_PRESSURE, z);
    left = p[3] & 0x01;
    right = p[3] & 0x02;
    middle = p[3] & 0x04;
    if (!(priv.quirks & QUIRK_STICK_BUTTONS) && (left || right || middle)) {
        priv.quirks |= QUIRK_STICK_BUTTONS;
    }
    if (priv.quirks & QUIRK_STICK_BUTTONS) {
        report_key(stick, BTN_LEFT, left);
        report_key(stick, BTN_RIGHT, right);
        report_key(stick, BTN_MIDDLE, middle);
    }
    sync(stick);
}

/** The pad's and the stick's buttons of a version 3 place packet, Linux's
 * `alps_decode_buttons_v3`. */
static void decode_buttons_v3(struct fields *f, const uint8_t *p)
{
    f->left = (p[3] & 0x01) != 0;
    f->right = (p[3] & 0x02) != 0;
    f->middle = (p[3] & 0x04) != 0;
    f->ts_left = (p[3] & 0x10) != 0;
    f->ts_right = (p[3] & 0x20) != 0;
    f->ts_middle = (p[3] & 0x40) != 0;
}

/** Read a version 3 packet of the Pinnacle pads, Linux's
 * `alps_decode_pinnacle`. */
static int decode_pinnacle(struct fields *f, const uint8_t *p)
{
    f->first_mp = (p[4] & 0x40) != 0;
    f->is_mp = (p[0] & 0x40) != 0;
    if (f->is_mp) {
        f->fingers = (p[5] & 0x3) + 1;
        f->x_map = ((p[4] & 0x7e) << 8) | ((p[1] & 0x7f) << 2) | ((p[0] & 0x30) >> 4);
        f->y_map = ((p[3] & 0x70) << 4) | ((p[2] & 0x7f) << 1) | (p[4] & 0x01);
    } else {
        f->st.x = ((p[1] & 0x7f) << 4) | ((p[4] & 0x30) >> 2) | ((p[0] & 0x30) >> 4);
        f->st.y = ((p[2] & 0x7f) << 4) | (p[4] & 0x0f);
        f->pressure = p[5] & 0x7f;
        decode_buttons_v3(f, p);
    }
    return 0;
}

/** Read a version 3 packet of the Rushmore pads, Linux's
 * `alps_decode_rushmore`. */
static int decode_rushmore(struct fields *f, const uint8_t *p)
{
    int a, b;

    f->first_mp = (p[4] & 0x40) != 0;
    f->is_mp = (p[5] & 0x40) != 0;
    if (f->is_mp) {
        a = p[5] & 0x3;
        b = (p[5] >> 2) & 0x3;
        f->fingers = (unsigned int)(a > b ? a : b) + 1;
        f->x_map = ((p[5] & 0x10) << 11) | ((p[4] & 0x7e) << 8) | ((p[1] & 0x7f) << 2) |
                   ((p[0] & 0x30) >> 4);
        f->y_map =
            ((p[5] & 0x20) << 6) | ((p[3] & 0x70) << 4) | ((p[2] & 0x7f) << 1) | (p[4] & 0x01);
    } else {
        f->st.x = ((p[1] & 0x7f) << 4) | ((p[4] & 0x30) >> 2) | ((p[0] & 0x30) >> 4);
        f->st.y = ((p[2] & 0x7f) << 4) | (p[4] & 0x0f);
        f->pressure = p[5] & 0x7f;
        decode_buttons_v3(f, p);
    }
    return 0;
}

/** Read a version 5 packet of the Dolphin pads, Linux's
 * `alps_decode_dolphin`: the second holds the profiles of the electrodes,
 * Y's first. */
static int decode_dolphin(struct fields *f, const uint8_t *p)
{
    uint64_t palm_data;

    f->first_mp = (p[0] & 0x02) != 0;
    f->is_mp = (p[0] & 0x20) != 0;
    if (!f->is_mp) {
        f->st.x = (p[1] & 0x7f) | ((p[4] & 0x0f) << 7);
        f->st.y = (p[2] & 0x7f) | ((p[4] & 0xf0) << 3);
        f->pressure = (p[0] & 4) ? 0 : p[5] & 0x7f;
        decode_buttons_v3(f, p);
    } else {
        f->fingers = (unsigned int)(((p[0] & 0x6) >> 1) | ((p[0] & 0x10) >> 2));
        palm_data = (uint64_t)(p[1] & 0x7f) | ((uint64_t)(p[2] & 0x7f) << 7) |
                    ((uint64_t)(p[4] & 0x7f) << 14) | ((uint64_t)(p[5] & 0x7f) << 21) |
                    ((uint64_t)(p[3] & 0x07) << 28) | ((uint64_t)(p[3] & 0x70) << 27) |
                    ((uint64_t)(p[0] & 0x01) << 34);
        f->y_map = (unsigned int)(palm_data & ((1u << priv.y_bits) - 1u));
        f->x_map = (unsigned int)((palm_data >> priv.y_bits) & ((1u << priv.x_bits) - 1u));
    }
    return 0;
}

/**
 * Report a version 3 or 5 pad packet, Linux's
 * `alps_process_touchpad_packet_v3_v5`: a place packet with the mt bit is
 * kept until the bitmap packet after it, which gives the box of the
 * contacts; a bitmap packet alone is dropped, and so is a pressure of 0
 * with a place, which the hardware sends in the middle of a touch.
 */
static void process_touchpad_packet_v3_v5(void)
{
    struct fields *f = &priv.f;
    int fingers = 0;

    k_memset(f, 0, sizeof(*f));
    priv.decode_fields(f, packet);
    if (priv.multi_packet) {
        if (f->is_mp) {
            fingers = (int)f->fingers;
            priv.decode_fields(f, priv.multi_data);
            if (process_bitmap(f) == 0) {
                fingers = 0;
            }
        } else {
            priv.multi_packet = 0;
        }
    }
    if (f->is_mp) {
        return;
    }
    if (!priv.multi_packet && f->first_mp) {
        priv.multi_packet = 1;
        k_memcpy(priv.multi_data, packet, sizeof(priv.multi_data));
        return;
    }
    priv.multi_packet = 0;
    if (f->st.x && f->st.y && !f->pressure) {
        return;
    }
    report_semi_mt_data(fingers);
    if ((priv.flags & F_DUALPOINT) && !(priv.quirks & QUIRK_STICK_BUTTONS)) {
        report_key(stick, BTN_LEFT, f->ts_left);
        report_key(stick, BTN_RIGHT, f->ts_right);
        report_key(stick, BTN_MIDDLE, f->ts_middle);
        sync(stick);
    }
}

/** Report a version 3 packet, Linux's `alps_process_packet_v3`: a stick
 * packet ends in 0x3f, which the pad's never do. */
static void process_packet_v3(void)
{
    if (packet[5] == 0x3f) {
        process_trackstick_packet_v3();
        return;
    }
    process_touchpad_packet_v3_v5();
}

/**
 * Report a version 6 packet, Linux's `alps_process_packet_v6`: a stick's,
 * which ends in 0x7f, a quarter of its motion; or the pad's place and
 * pressure, whose buttons Linux reports on the stick's device. Linux leaves
 * that device's packet open until the stick's next, so a click of the pad
 * waits for the stick to move (dev-log/76-display/upstream.md); here the
 * stick's packet is ended with the pad's.
 */
static void process_packet_v6(void)
{
    uint8_t *p = packet;
    int x, y, z;

    if (p[5] == 0x7f) {
        if (!(priv.flags & F_DUALPOINT)) {
            reject_stick();
            return;
        }
        x = p[1] | ((p[3] & 0x20) << 2);
        y = p[2] | ((p[3] & 0x40) << 1);
        z = p[4];
        if (x == 0x7f && y == 0x7f && z == 0x7f) {
            x = 0; /* a lift, which Linux reports as no motion */
            y = 0;
        }
        evdev_report(stick, EV_REL, REL_X, (int8_t)x / 4);
        evdev_report(stick, EV_REL, REL_Y, -((int8_t)y / 4));
        report_standard_buttons(stick, p[3]);
        sync(stick);
        return;
    }
    x = p[1] | ((p[3] & 0x78) << 4);
    y = p[2] | ((p[4] & 0x78) << 4);
    z = p[5];
    if (z > 30) {
        report_key(pad, BTN_TOUCH, 1);
    }
    if (z < 25) {
        report_key(pad, BTN_TOUCH, 0);
    }
    if (z > 0) {
        evdev_report(pad, EV_ABS, ABS_X, x);
        evdev_report(pad, EV_ABS, ABS_Y, y);
    }
    evdev_report(pad, EV_ABS, ABS_PRESSURE, z);
    report_key(pad, BTN_TOOL_FINGER, z > 0);
    p[3] &= (uint8_t)~0x04u; /* no middle button */
    report_standard_buttons(stick, p[3]);
    sync(pad);
    sync(stick);
}

/**
 * Report a version 4 packet, Linux's `alps_process_packet_v4`: the place,
 * pressure and buttons each packet, and from the last two bytes of three
 * packets in a row, the first marked by bit 6, the bitmaps of the contacts.
 */
static void process_packet_v4(void)
{
    struct fields *f = &priv.f;
    const uint8_t *m = priv.multi_data;
    int offset;

    if (packet[6] & 0x40) {
        priv.multi_packet = 0;
    }
    if (priv.multi_packet > 2) {
        return;
    }
    offset = 2 * priv.multi_packet;
    priv.multi_data[offset] = packet[6];
    priv.multi_data[offset + 1] = packet[7];
    f->left = (packet[4] & 0x01) != 0;
    f->right = (packet[4] & 0x02) != 0;
    f->st.x = ((packet[1] & 0x7f) << 4) | ((packet[3] & 0x30) >> 2) | ((packet[0] & 0x30) >> 4);
    f->st.y = ((packet[2] & 0x7f) << 4) | (packet[3] & 0x0f);
    f->pressure = packet[5] & 0x7f;
    if (++priv.multi_packet > 2) {
        priv.multi_packet = 0;
        f->x_map = ((m[2] & 0x1f) << 10) | ((m[3] & 0x60) << 3) | ((m[0] & 0x3f) << 2) |
                   ((m[1] & 0x60) >> 5);
        f->y_map = ((m[5] & 0x01) << 10) | ((m[3] & 0x1f) << 5) | (m[1] & 0x1f);
        f->fingers = (unsigned int)process_bitmap(f);
    }
    report_semi_mt_data((int)f->fingers);
}

/** Whether the bytes of a version 7 packet so far have its form, Linux's
 * `alps_is_valid_package_v7`. */
static int valid_package_v7(void)
{
    switch (pktcnt) {
    case 3:
        return (packet[2] & 0x40) == 0x40;
    case 4:
        return (packet[3] & 0x48) == 0x48;
    case 6:
        return (packet[5] & 0x40) == 0x00;
    default:
        return 1;
    }
}

/** The kind of a version 7 pad packet, Linux's `alps_get_packet_id_v7`. */
static int packet_id_v7(const uint8_t *b)
{
    if (b[4] & 0x40) {
        return V7_PACKET_ID_TWO;
    }
    if (b[4] & 0x01) {
        return V7_PACKET_ID_MULTI;
    }
    if ((b[0] & 0x10) && !(b[4] & 0x43)) {
        return V7_PACKET_ID_NEW;
    }
    if (b[1] == 0x00 && b[4] == 0x00) {
        return V7_PACKET_ID_IDLE;
    }
    return V7_PACKET_ID_UNKNOWN;
}

/** The two contacts' places of a version 7 packet, Linux's
 * `alps_get_finger_coordinate_v7`. */
static void finger_coordinate_v7(struct evdev_mt_pos *mt, const uint8_t *pkt, int pkt_id)
{
    mt[0].x =
        ((pkt[2] & 0x80) << 4) | ((pkt[2] & 0x3F) << 5) | ((pkt[3] & 0x30) >> 1) | (pkt[3] & 0x07);
    mt[0].y = (pkt[1] << 3) | (pkt[0] & 0x07);
    mt[1].x = ((pkt[3] & 0x80) << 4) | ((pkt[4] & 0x80) << 3) | ((pkt[4] & 0x3F) << 4);
    mt[1].y = ((pkt[5] & 0x80) << 3) | ((pkt[5] & 0x3F) << 4);
    switch (pkt_id) {
    case V7_PACKET_ID_TWO:
        mt[1].x &= ~0x000F;
        mt[1].y |= 0x000F;
        if (mt[1].y == 0x7ff && mt[1].x == 0xff0) {
            mt[1].x = 0; /* a false touch at the largest place */
        }
        break;
    case V7_PACKET_ID_MULTI:
        mt[1].x &= ~0x003F;
        mt[1].y &= ~0x0020;
        mt[1].y |= ((pkt[4] & 0x02) << 4);
        mt[1].y |= 0x001F;
        break;
    case V7_PACKET_ID_NEW:
        mt[1].x &= ~0x003F;
        mt[1].x |= (pkt[0] & 0x20);
        mt[1].y |= 0x000F;
        break;
    default:
        break;
    }
    mt[0].y = 0x7FF - mt[0].y;
    mt[1].y = 0x7FF - mt[1].y;
}

/** The contacts of `mt` with a place, Linux's `alps_get_mt_count`. */
static int mt_count(const struct evdev_mt_pos *mt)
{
    int i, fingers = 0;

    for (i = 0; i < MAX_TOUCHES; i++) {
        if (mt[i].x != 0 || mt[i].y != 0) {
            fingers++;
        }
    }
    return fingers;
}

/**
 * Read a version 7 pad packet, Linux's `alps_decode_packet_v7`. Returns 0,
 * 1 for a NEW packet, which Linux drops since its finger count and second
 * X are not to be trusted, or -1 for a packet of no known kind.
 */
static int decode_packet_v7(struct fields *f, const uint8_t *p)
{
    int pkt_id = packet_id_v7(p);

    if (pkt_id == V7_PACKET_ID_IDLE) {
        return 0;
    }
    if (pkt_id == V7_PACKET_ID_UNKNOWN) {
        return -1;
    }
    if (pkt_id == V7_PACKET_ID_NEW) {
        return 1;
    }
    finger_coordinate_v7(f->mt, p, pkt_id);
    if (pkt_id == V7_PACKET_ID_TWO) {
        f->fingers = (unsigned int)mt_count(f->mt);
    } else {
        f->fingers = 3u + (p[5] & 0x03u);
    }
    f->left = (p[0] & 0x80) >> 7;
    if (priv.flags & F_BUTTONPAD) {
        if (p[0] & 0x20) {
            f->fingers++;
        }
        if (p[0] & 0x10) {
            f->fingers++;
        }
    } else {
        f->right = (p[0] & 0x20) >> 5;
        f->middle = (p[0] & 0x10) >> 4;
    }
    if (f->fingers == 1 && f->mt[0].x == 0 && f->mt[0].y == 0) {
        f->mt[0] = f->mt[1];
        f->mt[1].x = 0;
        f->mt[1].y = 0;
    }
    return 0;
}

/** Report a version 7 stick packet, Linux's
 * `alps_process_trackstick_packet_v7`. */
static void process_trackstick_packet_v7(void)
{
    const uint8_t *p = packet;
    int x, y, z;

    if (!(priv.flags & F_DUALPOINT)) {
        reject_stick();
        return;
    }
    x = (p[2] & 0xbf) | ((p[3] & 0x10) << 2);
    y = (p[3] & 0x07) | (p[4] & 0xb8) | ((p[3] & 0x20) << 1);
    z = (p[5] & 0x3f) | ((p[3] & 0x80) >> 1);
    evdev_report(stick, EV_REL, REL_X, (int8_t)x);
    evdev_report(stick, EV_REL, REL_Y, -((int8_t)y));
    evdev_report(stick, EV_ABS, ABS_PRESSURE, z);
    report_standard_buttons(stick, p[1]);
    sync(stick);
}

/** Report a version 7 pad packet, Linux's
 * `alps_process_touchpad_packet_v7`. */
static void process_touchpad_packet_v7(void)
{
    struct fields *f = &priv.f;

    k_memset(f, 0, sizeof(*f));
    if (priv.decode_fields(f, packet)) {
        return;
    }
    report_mt_data(mt_count(f->mt));
    evdev_mt_report_finger_count(pad, (int)f->fingers);
    report_key(pad, BTN_LEFT, f->left);
    report_key(pad, BTN_RIGHT, f->right);
    report_key(pad, BTN_MIDDLE, f->middle);
    sync(pad);
}

/** Report a version 7 packet, Linux's `alps_process_packet_v7`. */
static void process_packet_v7(void)
{
    if (packet[0] == 0x48 && (packet[4] & 0x47) == 0x06) {
        process_trackstick_packet_v7();
    } else {
        process_touchpad_packet_v7();
    }
}

/* Version 8's fields, Linux's SS4_* macros: `b` a packet, `i` its first or
 * second contact. */
#define SS4_IS_SS4PLUS(d) ((d)[0] == 0x73 && (d)[1] == 0x03 && (d)[2] == 0x28)
#define SS4_IS_IDLE_V2(b)                                                             \
    ((b)[0] == 0x18 && (b)[1] == 0x10 && (b)[2] == 0x00 && ((b)[3] & 0x88) == 0x08 && \
     (b)[4] == 0x10 && (b)[5] == 0x00)
#define SS4_1F_X_V2(b)                                                         \
    (((b)[0] & 0x0007) | (((b)[1] << 3) & 0x0078) | (((b)[1] << 2) & 0x0380) | \
     (((b)[2] << 5) & 0x1C00))
#define SS4_1F_Y_V2(b)                                                         \
    (((b)[2] & 0x000F) | (((b)[3] >> 2) & 0x0030) | (((b)[4] << 6) & 0x03C0) | \
     (((b)[4] << 5) & 0x0C00))
#define SS4_1F_Z_V2(b) (((b)[5] & 0x0F) | (((b)[5] >> 1) & 0x70) | ((b)[4] & 0x80))
#define SS4_BTN_V2(b)  (((b)[0] >> 5) & 0x07)
#define SS4_STD_MF_X_V2(b, i) \
    ((((b)[0 + (i) * 3] << 5) & 0x00E0) | (((b)[1 + (i) * 3] << 5) & 0x1F00))
#define SS4_PLUS_STD_MF_X_V2(b, i) \
    ((((b)[0 + (i) * 3] << 4) & 0x0070) | (((b)[1 + (i) * 3] << 4) & 0x0F80))
#define SS4_STD_MF_Y_V2(b, i)                                                  \
    ((((b)[1 + (i) * 3] << 3) & 0x0010) | (((b)[2 + (i) * 3] << 5) & 0x01E0) | \
     (((b)[2 + (i) * 3] << 4) & 0x0E00))
#define SS4_BTL_MF_X_V2(b, i)      (SS4_STD_MF_X_V2(b, i) | (((b)[0 + (i) * 3] >> 3) & 0x0010))
#define SS4_PLUS_BTL_MF_X_V2(b, i) (SS4_PLUS_STD_MF_X_V2(b, i) | (((b)[0 + (i) * 3] >> 4) & 0x0008))
#define SS4_BTL_MF_Y_V2(b, i)      (SS4_STD_MF_Y_V2(b, i) | (((b)[0 + (i) * 3] >> 3) & 0x0008))
#define SS4_MF_Z_V2(b, i)          (((b)[1 + (i) * 3] & 0x0001) | (((b)[1 + (i) * 3] >> 1) & 0x0002))
#define SS4_IS_MF_CONTINUE(b)      (((b)[2] & 0x10) == 0x10)
#define SS4_IS_5F_DETECTED(b)      (((b)[2] & 0x10) == 0x10)

/** The kind of a version 8 packet, Linux's `alps_get_pkt_id_ss4_v2`. */
static int packet_id_ss4_v2(const uint8_t *b)
{
    switch (b[3] & 0x30) {
    case 0x00:
        return SS4_IS_IDLE_V2(b) ? SS4_PACKET_ID_IDLE : SS4_PACKET_ID_ONE;
    case 0x10:
        return SS4_PACKET_ID_TWO;
    case 0x20:
        return SS4_PACKET_ID_STICK;
    default:
        return SS4_PACKET_ID_MULTI;
    }
}

/** The two contacts' X of a version 8 packet of two or more, by the pad's
 * kind, into `x0` and `x1`, and Y into `y0` and `y1`. */
static void ss4_mf_places(const uint8_t *p, int *x0, int *x1, int *y0, int *y1)
{
    int plus = SS4_IS_SS4PLUS(priv.dev_id);

    if (priv.flags & F_BUTTONPAD) {
        *x0 = plus ? SS4_PLUS_BTL_MF_X_V2(p, 0) : SS4_BTL_MF_X_V2(p, 0);
        *x1 = plus ? SS4_PLUS_BTL_MF_X_V2(p, 1) : SS4_BTL_MF_X_V2(p, 1);
        *y0 = SS4_BTL_MF_Y_V2(p, 0);
        *y1 = SS4_BTL_MF_Y_V2(p, 1);
    } else {
        *x0 = plus ? SS4_PLUS_STD_MF_X_V2(p, 0) : SS4_STD_MF_X_V2(p, 0);
        *x1 = plus ? SS4_PLUS_STD_MF_X_V2(p, 1) : SS4_STD_MF_X_V2(p, 1);
        *y0 = SS4_STD_MF_Y_V2(p, 0);
        *y1 = SS4_STD_MF_Y_V2(p, 1);
    }
}

/** Read a version 8 packet, Linux's `alps_decode_ss4_v2`. */
static int decode_ss4_v2(struct fields *f, const uint8_t *p)
{
    int pkt_id = packet_id_ss4_v2(p);
    int x0, x1, y0, y1, no_data_x, no_data_y;

    switch (pkt_id) {
    case SS4_PACKET_ID_ONE:
        f->mt[0].x = SS4_1F_X_V2(p);
        f->mt[0].y = SS4_1F_Y_V2(p);
        f->pressure = ((SS4_1F_Z_V2(p)) * 2) & 0x7f;
        f->fingers = f->pressure > 0 ? 1 : 0; /* a held button's place is 0 */
        f->first_mp = 0;
        f->is_mp = 0;
        break;
    case SS4_PACKET_ID_TWO:
        ss4_mf_places(p, &x0, &x1, &y0, &y1);
        f->mt[0].x = x0;
        f->mt[1].x = x1;
        f->mt[0].y = y0;
        f->mt[1].y = y1;
        f->pressure = SS4_MF_Z_V2(p, 0) ? 0x30 : 0;
        if (SS4_IS_MF_CONTINUE(p)) {
            f->first_mp = 1;
        } else {
            f->fingers = 2;
            f->first_mp = 0;
        }
        f->is_mp = 0;
        break;
    case SS4_PACKET_ID_MULTI:
        ss4_mf_places(p, &x0, &x1, &y0, &y1);
        f->mt[2].x = x0;
        f->mt[3].x = x1;
        f->mt[2].y = y0;
        f->mt[3].y = y1;
        if (priv.flags & F_BUTTONPAD) {
            no_data_x =
                SS4_IS_SS4PLUS(priv.dev_id) ? SS4_PLUS_MFPACKET_NO_AX_BL : SS4_MFPACKET_NO_AX_BL;
            no_data_y = SS4_MFPACKET_NO_AY_BL;
        } else {
            no_data_x = SS4_IS_SS4PLUS(priv.dev_id) ? SS4_PLUS_MFPACKET_NO_AX : SS4_MFPACKET_NO_AX;
            no_data_y = SS4_MFPACKET_NO_AY;
        }
        f->first_mp = 0;
        f->is_mp = 1;
        if (SS4_IS_5F_DETECTED(p)) {
            f->fingers = 5;
        } else if (f->mt[3].x == no_data_x && f->mt[3].y == no_data_y) {
            f->mt[3].x = 0;
            f->mt[3].y = 0;
            f->fingers = 3;
        } else {
            f->fingers = 4;
        }
        break;
    case SS4_PACKET_ID_STICK:
        f->first_mp = 0; /* the stick's motion is read where it is reported */
        f->is_mp = 0;
        break;
    default:
        k_memset(f, 0, sizeof(*f));
        break;
    }
    if (pkt_id == SS4_PACKET_ID_STICK) {
        f->ts_left = (SS4_BTN_V2(p) & 0x01) != 0;
        f->ts_right = (SS4_BTN_V2(p) & 0x02) != 0;
        f->ts_middle = (SS4_BTN_V2(p) & 0x04) != 0;
    } else {
        f->left = (SS4_BTN_V2(p) & 0x01) != 0;
        if (!(priv.flags & F_BUTTONPAD)) {
            f->right = (SS4_BTN_V2(p) & 0x02) != 0;
            f->middle = (SS4_BTN_V2(p) & 0x04) != 0;
        }
    }
    return 0;
}

/**
 * Report a version 8 packet, Linux's `alps_process_packet_ss4_v2`: a
 * packet of two contacts that says more follow is kept until the packet of
 * the third and fourth; a stick's packet moves the stick.
 */
static void process_packet_ss4_v2(void)
{
    struct fields *f = &priv.f;
    const uint8_t *p = packet;

    k_memset(f, 0, sizeof(*f));
    priv.decode_fields(f, p);
    if (priv.multi_packet) {
        if (f->is_mp) {
            priv.decode_fields(f, priv.multi_data);
        } else {
            priv.multi_packet = 0;
        }
    }
    if (f->is_mp) {
        return;
    }
    if (!priv.multi_packet && f->first_mp) {
        priv.multi_packet = 1;
        k_memcpy(priv.multi_data, p, sizeof(priv.multi_data));
        return;
    }
    priv.multi_packet = 0;
    if (packet_id_ss4_v2(p) == SS4_PACKET_ID_STICK) {
        if (!(priv.flags & F_DUALPOINT)) {
            reject_stick();
            return;
        }
        evdev_report(stick, EV_REL, REL_X, (int8_t)(((p[0] & 0x01) << 7) | (p[1] & 0x7F)));
        evdev_report(stick, EV_REL, REL_Y, -(int8_t)(((p[3] & 0x01) << 7) | (p[2] & 0x7F)));
        evdev_report(stick, EV_ABS, ABS_PRESSURE, (int8_t)(p[4] & 0x7F));
        report_key(stick, BTN_LEFT, f->ts_left);
        report_key(stick, BTN_RIGHT, f->ts_right);
        report_key(stick, BTN_MIDDLE, f->ts_middle);
        sync(stick);
        return;
    }
    report_mt_data(f->fingers <= 4 ? (int)f->fingers : 4);
    evdev_mt_report_finger_count(pad, (int)f->fingers);
    report_key(pad, BTN_LEFT, f->left);
    report_key(pad, BTN_RIGHT, f->right);
    report_key(pad, BTN_MIDDLE, f->middle);
    evdev_report(pad, EV_ABS, ABS_PRESSURE, f->pressure);
    sync(pad);
}

/** Whether the bytes of a version 8 packet so far have its form, Linux's
 * `alps_is_valid_package_ss4_v2`. */
static int valid_package_ss4_v2(void)
{
    if (pktcnt == 4 && (packet[3] & 0x08) != 0x08) {
        return 0;
    }
    if (pktcnt == 6 && (packet[5] & 0x10) != 0x0) {
        return 0;
    }
    return 1;
}

/** The mouse's event device, the mouse device's packets taken from its
 * events as mousedev takes a relative device's. */
static void mouse_event(struct evdev *dev, uint16_t type, uint16_t code, int32_t value)
{
    (void)dev;
    mouse_written(type, code, value);
}

/**
 * Register a stick or the external port's mouse: `name`, `phys` and
 * `product` as psmouse gives them, `version`, REL_X, REL_Y and three
 * buttons, ABS_PRESSURE when `pressure`, INPUT_PROP_POINTER, and
 * INPUT_PROP_POINTING_STICK when `pointing_stick`. Returns the device, or
 * NULL when every node is taken.
 */
static struct evdev *register_relative(const char *name, const char *phys, uint16_t product,
                                       uint16_t version, int pressure, int pointing_stick)
{
    uint8_t keys[EVDEV_KEY_BYTES];
    struct evdev *dev;

    k_memset(keys, 0, sizeof(keys));
    dev = evdev_register(name, EVDEV_BUS_I8042, PSMOUSE_VENDOR, product, keys);
    if (!dev) {
        return 0;
    }
    evdev_describe(dev, phys, 0);
    evdev_set_version(dev, version);
    evdev_set_capability(dev, EV_REL, REL_X);
    evdev_set_capability(dev, EV_REL, REL_Y);
    if (pressure) {
        evdev_set_abs(dev, ABS_PRESSURE, 0, 127, 0, 0);
    }
    evdev_set_capability(dev, EV_KEY, BTN_LEFT);
    evdev_set_capability(dev, EV_KEY, BTN_RIGHT);
    evdev_set_capability(dev, EV_KEY, BTN_MIDDLE);
    evdev_set_prop(dev, INPUT_PROP_POINTER);
    if (pointing_stick) {
        evdev_set_prop(dev, INPUT_PROP_POINTING_STICK);
    }
    evdev_set_handler(dev, mouse_event);
    return dev;
}

/**
 * Report a mouse's PS/2 packet, Linux's `alps_report_bare_ps2_packet`: a
 * version 2 DualPoint's is its stick's; another pad's is a mouse's on its
 * external port, whose event device the first such packet registers, as
 * Linux's delayed work does, and which that packet does not reach.
 */
static void report_bare_ps2_packet(const uint8_t *p, int with_buttons)
{
    struct evdev *dev, *dev2 = 0;
    int x, y;

    if (priv.proto_version == PROTO_V2 && (priv.flags & F_DUALPOINT)) {
        dev = stick;
        dev2 = pad;
    } else if (!bare) {
        if (!bare_failed) {
            bare = register_relative("PS/2 ALPS Mouse",
                                     stick ? "isa0060/serio1/input2" : "isa0060/serio1/input1",
                                     PSMOUSE_PS2, 0, 0, 0);
            bare_failed = bare == 0;
            if (bare_failed) {
                klog(KLOG_WARN, "alps", "register: no event device for the external mouse");
            }
        }
        return;
    } else {
        dev = bare;
    }
    if (with_buttons) {
        report_buttons(dev, dev2, p[0] & 1, p[0] & 2, p[0] & 4);
    }
    /* psmouse_report_standard_motion */
    x = p[1] ? p[1] - ((p[0] << 4) & 0x100) : 0;
    y = p[2] ? p[2] - ((p[0] << 3) & 0x100) : 0;
    evdev_report(dev, EV_REL, REL_X, x);
    evdev_report(dev, EV_REL, REL_Y, -y);
    sync(dev);
}

/**
 * The bytes of a pad that cuts a stick's PS/2 packet into its own, Linux's
 * `alps_handle_interleaved_ps2`: a sixth byte arms the flush; a seventh
 * with bit 7 set begins the next packet, and the six before are the pad's;
 * one without it is the stick's packet's last, after two in bytes 4 and 5.
 */
static int handle_interleaved_ps2(void)
{
    if (pktcnt < 6) {
        return GOOD_DATA;
    }
    if (pktcnt == 6) {
        flush_armed = 1;
        flush_at = (uint32_t)kernel_ticks + MS_TO_TICKS(20);
        return GOOD_DATA;
    }
    flush_armed = 0;
    if (packet[6] & 0x80) {
        if (((packet[3] | packet[4] | packet[5]) & 0x80) || !valid_first_byte(packet[6])) {
            return BAD_DATA;
        }
        priv.process_packet();
        packet[0] = packet[6];
        pktcnt = 1;
    } else {
        report_bare_ps2_packet(&packet[3], 0);
        packet[3] = packet[6] & 0xf7; /* not taken for a cut packet again */
        pktcnt = 4;
    }
    return GOOD_DATA;
}

/** Report the six bytes of an interleaving pad after 20 ms with no more,
 * unless bytes 4 to 6 look like a PS/2 packet's, Linux's
 * `alps_flush_packet`. */
static void flush_packet(void)
{
    flush_armed = 0;
    if (pktcnt == pktsize) {
        if (!((packet[3] | packet[4] | packet[5]) & 0x80)) {
            priv.process_packet();
        }
        pktcnt = 0;
    }
}

/**
 * Judge the byte just added to the packet, Linux's `alps_process_byte`: a
 * PS/2 packet, which only a pad in step can tell from its own; a PS/2
 * packet cut into the pad's; then byte 0's form, bit 7 clear in the others
 * before version 5, and the forms of versions 7 and 8. Returns BAD_DATA,
 * GOOD_DATA or FULL_PACKET.
 */
static int process_byte(void)
{
    if (priv.proto_version != PROTO_V8 && !out_of_sync && (packet[0] & 0xc8) == 0x08) {
        if (pktcnt == 3) {
            report_bare_ps2_packet(packet, 1);
            return FULL_PACKET;
        }
        return GOOD_DATA;
    }
    if ((priv.flags & F_PS2_INTERLEAVED) && pktcnt >= 4 && (packet[3] & 0x0f) == 0x0f) {
        return handle_interleaved_ps2();
    }
    if (!valid_first_byte(packet[0])) {
        return BAD_DATA;
    }
    if (priv.proto_version < PROTO_V5 && pktcnt >= 2 && pktcnt <= pktsize &&
        (packet[pktcnt - 1] & 0x80)) {
        /* Dells such as the Latitude E6440 shut often send 0xff last. */
        if (priv.proto_version == PROTO_V3_RUSHMORE && pktcnt == pktsize) {
            return FULL_PACKET;
        }
        return BAD_DATA;
    }
    if ((priv.proto_version == PROTO_V7 && !valid_package_v7()) ||
        (priv.proto_version == PROTO_V8 && !valid_package_ss4_v2())) {
        return BAD_DATA;
    }
    if (pktcnt == pktsize) {
        priv.process_packet();
        return FULL_PACKET;
    }
    return GOOD_DATA;
}

void alps_tick(void)
{
    if (flush_armed && (int32_t)((uint32_t)kernel_ticks - flush_at) >= 0) {
        flush_packet();
    }
}

int alps_byte(uint8_t data)
{
    uint32_t now = (uint32_t)kernel_ticks;

    alps_tick(); /* a flush due before this byte, which the tick has not run */
    if (pktcnt && now - last_tick > MS_TO_TICKS(500)) {
        sync_losses++;
        pktcnt = 0;
        flush_armed = 0;
    }
    last_tick = now;
    if (pktcnt >= (int)sizeof(packet)) {
        pktcnt = 0;
    }
    packet[pktcnt++] = data;
    switch (process_byte()) {
    case BAD_DATA:
        out_of_sync++;
        pktcnt = 0;
        return ALPS_BAD;
    case FULL_PACKET:
        pktcnt = 0;
        out_of_sync = 0;
        return ALPS_FULL;
    default:
        return ALPS_GOOD;
    }
}

int alps_packet_start(void)
{
    return pktcnt == 0;
}

void alps_packet_restart(void)
{
    pktcnt = 0;
    out_of_sync = 0;
    flush_armed = 0;
}

int alps_reset_after(void)
{
    return 2 * pktsize;
}

/* Command mode. */

/** Send nibble `n` of the version's table, Linux's
 * `alps_command_mode_send_nibble`. */
static int send_nibble(int n)
{
    unsigned int command = priv.nibbles[n].command;
    uint8_t param[4];

    param[0] = priv.nibbles[n].data;
    return ps2_command(param, command) < 0 ? -1 : 0;
}

/** Set the register address `addr`, Linux's `alps_command_mode_set_addr`. */
static int set_addr(int addr)
{
    int i;

    if (ps2_command(0, priv.addr_command) < 0) {
        return -1;
    }
    for (i = 12; i >= 0; i -= 4) {
        if (send_nibble((addr >> i) & 0xf) < 0) {
            return -1;
        }
    }
    return 0;
}

/** Read the register `addr` set last, whose answer names it, Linux's
 * `__alps_command_mode_read_reg`. Returns its value, or -1. */
static int read_set_reg(int addr)
{
    uint8_t param[4];

    if (ps2_command(param, CMD_GETINFO) < 0) {
        return -1;
    }
    if (addr != ((param[0] << 8) | param[1])) {
        return -1;
    }
    return param[2];
}

/** Read the register `addr`, Linux's `alps_command_mode_read_reg`. */
static int read_reg(int addr)
{
    if (set_addr(addr) < 0) {
        return -1;
    }
    return read_set_reg(addr);
}

/** Write `value` to the register set last, Linux's
 * `__alps_command_mode_write_reg`. */
static int write_set_reg(int value)
{
    if (send_nibble((value >> 4) & 0xf) < 0 || send_nibble(value & 0xf) < 0) {
        return -1;
    }
    return 0;
}

/** Write `value` to the register `addr`, Linux's
 * `alps_command_mode_write_reg`. */
static int write_reg(int addr, int value)
{
    if (set_addr(addr) < 0) {
        return -1;
    }
    return write_set_reg(value);
}

/** A report: `init_command`, when not 0, with a byte of 0, then
 * `repeated_command` three times and a status request, whose three bytes go
 * in `param`, Linux's `alps_rpt_cmd`. */
static int rpt_cmd(unsigned int init_command, unsigned int repeated_command, uint8_t *param)
{
    param[0] = 0;
    if (init_command && ps2_command(param, init_command) < 0) {
        return -1;
    }
    if (ps2_command(0, repeated_command) < 0 || ps2_command(0, repeated_command) < 0 ||
        ps2_command(0, repeated_command) < 0) {
        return -1;
    }
    param[0] = param[1] = param[2] = 0xff;
    return ps2_command(param, CMD_GETINFO) < 0 ? -1 : 0;
}

/** Whether the answer of entering command mode is an ALPS firmware's,
 * Linux's `alps_check_valid_firmware_id`. */
static int valid_firmware_id(const uint8_t *id)
{
    if (id[0] == 0x73) {
        return 1;
    }
    return id[0] == 0x88 &&
           (id[1] == 0x07 || id[1] == 0x08 || (id[1] & 0xf0) == 0xb0 || (id[1] & 0xf0) == 0xc0);
}

/** Enter command mode, Linux's `alps_enter_command_mode`. */
static int enter_command_mode(void)
{
    uint8_t param[4];

    if (rpt_cmd(0, CMD_RESET_WRAP, param) < 0) {
        klog(KLOG_WARN, "alps", "command: failed to enter command mode");
        return -1;
    }
    return valid_firmware_id(param) ? 0 : -1;
}

/** Leave command mode, Linux's `alps_exit_command_mode`. */
static int exit_command_mode(void)
{
    return ps2_command(0, CMD_SETSTREAM) < 0 ? -1 : 0;
}

/** Have commands go to the pad behind a version 2 DualPoint's stick, or to
 * the stick, Linux's `alps_passthrough_mode_v2`. */
static int passthrough_mode_v2(int enable)
{
    unsigned int cmd = enable ? CMD_SETSCALE21 : CMD_SETSCALE11;

    if (ps2_command(0, cmd) < 0 || ps2_command(0, cmd) < 0 || ps2_command(0, cmd) < 0 ||
        ps2_command(0, CMD_DISABLE) < 0) {
        return -1;
    }
    ps2_drain(3);
    return 0;
}

/** The absolute mode of versions 1 and 2: four disables and an enable,
 * then remote mode, Linux's `alps_absolute_mode_v1_v2`. */
static int absolute_mode_v1_v2(void)
{
    if (ps2_command(0, CMD_DISABLE) < 0 || ps2_command(0, CMD_DISABLE) < 0 ||
        ps2_command(0, CMD_DISABLE) < 0 || ps2_command(0, CMD_DISABLE) < 0 ||
        ps2_command(0, CMD_ENABLE) < 0) {
        return -1;
    }
    return ps2_command(0, CMD_SETPOLL) < 0 ? -1 : 0;
}

/** Send the twelve bits of `word` in monitor mode, the lowest nibble
 * first, Linux's `alps_monitor_mode_send_word`. */
static int monitor_mode_send_word(int word)
{
    int i;

    for (i = 0; i <= 8; i += 4) {
        if (send_nibble((word >> i) & 0xf) < 0) {
            return -1;
        }
    }
    return 0;
}

/** Write `value` to `addr` in monitor mode, Linux's
 * `alps_monitor_mode_write_reg`. */
static int monitor_mode_write_reg(int addr, int value)
{
    if (ps2_command(0, CMD_ENABLE) < 0 || monitor_mode_send_word(0x0A0) < 0 ||
        monitor_mode_send_word(addr) < 0 || monitor_mode_send_word(value) < 0 ||
        ps2_command(0, CMD_DISABLE) < 0) {
        return -1;
    }
    return 0;
}

/** Enter monitor mode, EC E9 F5 F5 E7 E6 E7 E9, or leave it, EC, Linux's
 * `alps_monitor_mode`; the status requests' three bytes are read and
 * dropped. */
static int monitor_mode(int enable)
{
    static const unsigned int enter[8] = {CMD_RESET_WRAP, CMD_GETINFO,    CMD_DISABLE,
                                          CMD_DISABLE,    CMD_SETSCALE21, CMD_SETSCALE11,
                                          CMD_SETSCALE21, CMD_GETINFO};
    uint8_t param[4];
    int i;

    if (!enable) {
        return ps2_command(0, CMD_RESET_WRAP) < 0 ? -1 : 0;
    }
    for (i = 0; i < 8; i++) {
        if (ps2_command(param, enter[i]) < 0) {
            return -1;
        }
    }
    return 0;
}

/** Version 6's absolute mode, register 0 of monitor mode set to 0x181,
 * Linux's `alps_absolute_mode_v6`. */
static int absolute_mode_v6(void)
{
    int ret;

    if (monitor_mode(1) < 0) {
        return -1;
    }
    ret = monitor_mode_write_reg(0x000, 0x181);
    if (monitor_mode(0) < 0) {
        ret = -1;
    }
    return ret;
}

/** Turn tapping on or off: E9 F5 F5, then F3 0A or E8 00, then the status
 * report F5 F5 F5 E9, Linux's `alps_tap_mode`. */
static int tap_mode(int enable)
{
    uint8_t param[4];
    uint8_t tap_arg = enable ? 0x0A : 0x00;

    if (ps2_command(param, CMD_GETINFO) < 0 || ps2_command(0, CMD_DISABLE) < 0 ||
        ps2_command(0, CMD_DISABLE) < 0 ||
        ps2_command(&tap_arg, enable ? CMD_SETRATE : CMD_SETRES) < 0) {
        return -1;
    }
    return rpt_cmd(0, CMD_DISABLE, param);
}

/** Set up a version 1 or 2 pad, Linux's `alps_hw_init_v1_v2`. */
static int hw_init_v1_v2(void)
{
    if ((priv.flags & F_PASS) && passthrough_mode_v2(1) < 0) {
        return -1;
    }
    if (tap_mode(1) < 0) {
        klog(KLOG_WARN, "alps", "init: failed to enable hardware tapping");
        return -1;
    }
    if (absolute_mode_v1_v2() < 0) {
        klog(KLOG_WARN, "alps", "init: failed to enable absolute mode");
        return -1;
    }
    if ((priv.flags & F_PASS) && passthrough_mode_v2(0) < 0) {
        return -1;
    }
    if (ps2_command(0, CMD_SETSTREAM) < 0) {
        klog(KLOG_WARN, "alps", "init: failed to enable stream mode");
        return -1;
    }
    return 0;
}

/** Put the stick, which commands reach in pass-through mode, in its
 * extended mode, Linux's `alps_trackstick_enter_extended_mode_v3_v6`. */
static int trackstick_enter_extended_mode_v3_v6(void)
{
    uint8_t param[2];

    param[0] = 0xC8;
    param[1] = 0x14;
    if (ps2_command(0, CMD_SETSCALE11) < 0 || ps2_command(0, CMD_SETSCALE11) < 0 ||
        ps2_command(0, CMD_SETSCALE11) < 0 || ps2_command(&param[0], CMD_SETRATE) < 0 ||
        ps2_command(&param[1], CMD_SETRATE) < 0) {
        return -1;
    }
    return 0;
}

/** Set up a version 6 pad, Linux's `alps_hw_init_v6`. */
static int hw_init_v6(void)
{
    int ret;

    if (passthrough_mode_v2(1) < 0) {
        return -1;
    }
    ret = trackstick_enter_extended_mode_v3_v6();
    if (passthrough_mode_v2(0) < 0) {
        return -1;
    }
    if (ret < 0) {
        return ret;
    }
    if (absolute_mode_v6() < 0) {
        klog(KLOG_WARN, "alps", "init: failed to enable absolute mode");
        return -1;
    }
    return 0;
}

/** Turn on or off pass-through to the stick, bit 0 of register
 * `reg_base` + 8, Linux's `alps_passthrough_mode_v3`. */
static int passthrough_mode_v3(int reg_base, int enable)
{
    int reg_val, ret = -1;

    if (enter_command_mode() < 0) {
        return -1;
    }
    reg_val = read_reg(reg_base + 0x0008);
    if (reg_val != -1) {
        reg_val = enable ? reg_val | 0x01 : reg_val & ~0x01;
        ret = write_set_reg(reg_val);
    }
    if (exit_command_mode() < 0) {
        ret = -1;
    }
    return ret;
}

/** Version 3's absolute mode, in command mode, Linux's
 * `alps_absolute_mode_v3`. */
static int absolute_mode_v3(void)
{
    int reg_val = read_reg(0x0004);

    if (reg_val == -1) {
        return -1;
    }
    return write_set_reg(reg_val | 0x06);
}

/** Whether a stick is there, bit 7 of register `reg_base` + 8, Linux's
 * `alps_probe_trackstick_v3_v7`: 0 when it is, -1 when not or the read
 * failed. */
static int probe_trackstick_v3_v7(int reg_base)
{
    int ret = -1, reg_val;

    if (enter_command_mode() == 0) {
        reg_val = read_reg(reg_base + 0x08);
        if (reg_val != -1) {
            ret = reg_val & 0x80 ? 0 : -1;
        }
    }
    exit_command_mode();
    return ret;
}

/* What `setup_trackstick_v3` returns: the stick is set up, there is none
 * after all, or the pad failed. */
#define STICK_OK   0
#define STICK_NONE 1
#define STICK_EIO  2

/**
 * Put the stick in its extended mode and tell the pad so, bit 1 of
 * register `reg_base` + 8, Linux's `alps_setup_trackstick_v3`; a stick that
 * fails its E7 report is taken for none.
 */
static int setup_trackstick_v3(int reg_base)
{
    uint8_t param[4];
    int ret = STICK_OK;
    int reg_val;

    if (passthrough_mode_v3(reg_base, 1) < 0) {
        return STICK_EIO;
    }
    if (rpt_cmd(0, CMD_SETSCALE21, param) < 0) {
        klog(KLOG_WARN, "alps", "init: failed to initialize trackstick (E7 report failed)");
        ret = STICK_NONE;
    } else if (trackstick_enter_extended_mode_v3_v6() < 0) {
        klog(KLOG_WARN, "alps", "init: failed to enter into trackstick extended mode");
        ret = STICK_EIO;
    }
    if (passthrough_mode_v3(reg_base, 0) < 0) {
        return STICK_EIO;
    }
    if (ret != STICK_OK) {
        return ret;
    }
    if (enter_command_mode() < 0) {
        return STICK_EIO;
    }
    reg_val = read_reg(reg_base + 0x08);
    if (reg_val == -1 || write_set_reg(reg_val | 0x02) < 0) {
        ret = STICK_EIO;
    }
    if (exit_command_mode() < 0) {
        return STICK_EIO;
    }
    return ret;
}

/** Set up a version 3 Pinnacle pad, Linux's `alps_hw_init_v3`. */
static int hw_init_v3(void)
{
    uint8_t param[4];
    int reg_val;

    if ((priv.flags & F_DUALPOINT) && setup_trackstick_v3(REG_BASE_PINNACLE) == STICK_EIO) {
        goto error;
    }
    if (enter_command_mode() < 0 || absolute_mode_v3() < 0) {
        klog(KLOG_WARN, "alps", "init: failed to enter absolute mode");
        goto error;
    }
    reg_val = read_reg(0x0006);
    if (reg_val == -1 || write_set_reg(reg_val | 0x01) < 0) {
        goto error;
    }
    reg_val = read_reg(0x0007);
    if (reg_val == -1 || write_set_reg(reg_val | 0x01) < 0) {
        goto error;
    }
    if (read_reg(0x0144) == -1 || write_set_reg(0x04) < 0) {
        goto error;
    }
    if (read_reg(0x0159) == -1 || write_set_reg(0x03) < 0) {
        goto error;
    }
    if (read_reg(0x0163) == -1 || write_reg(0x0163, 0x03) < 0) {
        goto error;
    }
    if (read_reg(0x0162) == -1 || write_reg(0x0162, 0x04) < 0) {
        goto error;
    }
    exit_command_mode();
    param[0] = 0x64;
    if (ps2_command(param, CMD_SETRATE) < 0 || ps2_command(0, CMD_ENABLE) < 0) {
        klog(KLOG_WARN, "alps", "init: failed to enable data reporting");
        return -1;
    }
    return 0;

error:
    exit_command_mode(); /* a pad left in command mode is dead until a reboot */
    return -1;
}

/** The resolution of versions 3 and 7 from the electrodes' pitch and count
 * at `reg_pitch`, Linux's `alps_get_v3_v7_resolution`. */
static int get_v3_v7_resolution(int reg_pitch)
{
    int reg, x_pitch, y_pitch, x_electrode, y_electrode, x_phys, y_phys;

    reg = read_reg(reg_pitch);
    if (reg < 0) {
        return reg;
    }
    x_pitch = 50 + 2 * sign4(reg); /* tenths of a millimetre */
    y_pitch = 36 + 2 * sign4(reg >> 4);
    reg = read_reg(reg_pitch + 1);
    if (reg < 0) {
        return reg;
    }
    x_electrode = 17 + sign4(reg);
    y_electrode = 13 + sign4(reg >> 4);
    x_phys = x_pitch * (x_electrode - 1);
    y_phys = y_pitch * (y_electrode - 1);
    priv.x_res = (unsigned int)(priv.x_max * 10 / x_phys);
    priv.y_res = (unsigned int)(priv.y_max * 10 / y_phys);
    return 0;
}

/** Set up a version 3 Rushmore pad, Linux's `alps_hw_init_rushmore_v3`. */
static int hw_init_rushmore_v3(void)
{
    int reg_val;

    if ((priv.flags & F_DUALPOINT) && setup_trackstick_v3(REG_BASE_RUSHMORE) == STICK_EIO) {
        goto error;
    }
    if (enter_command_mode() < 0 || read_reg(0xc2d9) == -1 || write_reg(0xc2cb, 0x00) < 0) {
        goto error;
    }
    if (get_v3_v7_resolution(0xc2da) < 0) {
        goto error;
    }
    reg_val = read_reg(0xc2c6);
    if (reg_val == -1 || write_set_reg(reg_val & 0xfd) < 0) {
        goto error;
    }
    if (write_reg(0xc2c9, 0x64) < 0) {
        goto error;
    }
    reg_val = read_reg(0xc2c4);
    if (reg_val == -1 || write_set_reg(reg_val | 0x02) < 0) {
        goto error;
    }
    exit_command_mode();
    return ps2_command(0, CMD_ENABLE) < 0 ? -1 : 0;

error:
    exit_command_mode();
    return -1;
}

/** Set up a version 4 pad, Linux's `alps_hw_init_v4`: absolute mode, its
 * registers, and the knock that makes its packets eight bytes, not nine. */
static int hw_init_v4(void)
{
    static const int regs[8][2] = {{0x0007, 0x8c}, {0x0149, 0x03}, {0x0160, 0x03}, {0x017f, 0x15},
                                   {0x0151, 0x01}, {0x0168, 0x03}, {0x014a, 0x03}, {0x0161, 0x03}};
    uint8_t param[4];
    int i, reg_val;

    if (enter_command_mode() < 0) {
        goto error;
    }
    reg_val = read_reg(0x0004);
    if (reg_val == -1 || write_set_reg(reg_val | 0x02) < 0) {
        klog(KLOG_WARN, "alps", "init: failed to enter absolute mode");
        goto error;
    }
    for (i = 0; i < 8; i++) {
        if (write_reg(regs[i][0], regs[i][1]) < 0) {
            goto error;
        }
    }
    exit_command_mode();
    param[0] = 0xc8;
    param[1] = 0x64;
    param[2] = 0x50;
    if (ps2_command(&param[0], CMD_SETRATE) < 0 || ps2_command(&param[1], CMD_SETRATE) < 0 ||
        ps2_command(&param[2], CMD_SETRATE) < 0 || ps2_command(param, CMD_GETID) < 0) {
        return -1;
    }
    param[0] = 0x64;
    if (ps2_command(param, CMD_SETRATE) < 0 || ps2_command(0, CMD_ENABLE) < 0) {
        klog(KLOG_WARN, "alps", "init: failed to enable data reporting");
        return -1;
    }
    return 0;

error:
    exit_command_mode();
    return -1;
}

/** Read a version 8 pad's one-time programmable bytes of `index` into
 * `otp`, Linux's `alps_get_otp_values_ss4_v2`. */
static int get_otp_values_ss4_v2(int index, uint8_t *otp)
{
    unsigned int cmd = index == 0 ? CMD_SETSTREAM : CMD_SETPOLL;

    if (ps2_command(0, cmd) < 0 || ps2_command(0, cmd) < 0 || ps2_command(otp, CMD_GETINFO) < 0) {
        return -1;
    }
    return 0;
}

/** A version 8 pad's size, resolution, buttons and stick from its OTP
 * bytes, Linux's `alps_update_device_area_ss4_v2`,
 * `alps_update_btn_info_ss4_v2` and `alps_update_dual_info_ss4_v2`. */
static void update_ss4_v2(uint8_t otp[2][4])
{
    int num_x, num_y, x_pitch, y_pitch, x_phys, y_phys, is_dual = 0, reg_val = 0;
    int plus = SS4_IS_SS4PLUS(priv.dev_id);

    if (plus) {
        num_x = SS4PLUS_NUMSENSOR_XOFFSET + (otp[0][2] & 0x0F);
        num_y = SS4PLUS_NUMSENSOR_YOFFSET + ((otp[0][2] >> 4) & 0x0F);
        priv.x_max = (num_x - 1) * SS4PLUS_COUNT_PER_ELECTRODE;
        priv.y_max = (num_y - 1) * SS4PLUS_COUNT_PER_ELECTRODE;
        x_pitch = (otp[0][1] & 0x0F) + SS4PLUS_MIN_PITCH_MM;
        y_pitch = ((otp[0][1] >> 4) & 0x0F) + SS4PLUS_MIN_PITCH_MM;
    } else {
        num_x = SS4_NUMSENSOR_XOFFSET + (otp[1][0] & 0x0F);
        num_y = SS4_NUMSENSOR_YOFFSET + ((otp[1][0] >> 4) & 0x0F);
        priv.x_max = (num_x - 1) * SS4_COUNT_PER_ELECTRODE;
        priv.y_max = (num_y - 1) * SS4_COUNT_PER_ELECTRODE;
        x_pitch = ((otp[1][2] >> 2) & 0x07) + SS4_MIN_PITCH_MM;
        y_pitch = ((otp[1][2] >> 5) & 0x07) + SS4_MIN_PITCH_MM;
    }
    x_phys = x_pitch * (num_x - 1);
    y_phys = y_pitch * (num_y - 1);
    priv.x_res = (unsigned int)(priv.x_max * 10 / x_phys);
    priv.y_res = (unsigned int)(priv.y_max * 10 / y_phys);

    if (plus ? (otp[1][0] >> 1) & 0x01 : (otp[1][1] >> 3) & 0x01) {
        priv.flags |= F_BUTTONPAD;
    }

    if (plus) {
        is_dual = (otp[0][0] >> 4) & 0x01;
        if (!is_dual) {
            /* the stick of the ThinkPad L and E series */
            if (exit_command_mode() == 0 && enter_command_mode() == 0) {
                reg_val = read_reg(0xD7);
            }
            exit_command_mode();
            ps2_command(0, CMD_ENABLE);
            if (reg_val == 0x0C || reg_val == 0x1D) {
                is_dual = 1;
            }
        }
    }
    if (is_dual) {
        priv.flags |= F_DUALPOINT | F_STICK_PRESSURE;
    }
}

/** Read a version 8 pad's OTP bytes and what they say, Linux's
 * `alps_set_defaults_ss4_v2`. */
static int set_defaults_ss4_v2(void)
{
    uint8_t otp[2][4];

    k_memset(otp, 0, sizeof(otp));
    if (get_otp_values_ss4_v2(1, otp[1]) < 0 || get_otp_values_ss4_v2(0, otp[0]) < 0) {
        return -1;
    }
    update_ss4_v2(otp);
    return 0;
}

/** A version 5 pad's electrodes, and so its size, from its register,
 * Linux's `alps_dolphin_get_device_area`. Returns -1 for a pad of fewer
 * than two Y electrodes, whose bitmaps have no place to divide. */
static int dolphin_get_device_area(void)
{
    uint8_t param[4];
    int num_x, num_y;

    k_memset(param, 0, sizeof(param));
    if (enter_command_mode() < 0) {
        return -1;
    }
    param[0] = 0x0a;
    if (ps2_command(0, CMD_RESET_WRAP) < 0 || ps2_command(0, CMD_SETPOLL) < 0 ||
        ps2_command(0, CMD_SETPOLL) < 0 || ps2_command(&param[0], CMD_SETRATE) < 0 ||
        ps2_command(&param[0], CMD_SETRATE) < 0) {
        return -1;
    }
    if (ps2_command(param, CMD_GETINFO) < 0) {
        return -1;
    }
    num_x = DOLPHIN_PROFILE_XOFFSET + (param[2] & 0x0F);
    num_y = DOLPHIN_PROFILE_YOFFSET + ((param[2] >> 4) & 0x0F);
    if (num_y < 2) { /* bitmap_place divides by num_y - 1; Linux does not check */
        exit_command_mode();
        return -1;
    }
    priv.x_bits = num_x;
    priv.y_bits = num_y;
    priv.x_max = (num_x - 1) * DOLPHIN_COUNT_PER_ELECTRODE;
    priv.y_max = (num_y - 1) * DOLPHIN_COUNT_PER_ELECTRODE;
    return exit_command_mode();
}

/** Set up a version 5 pad, Linux's `alps_hw_init_dolphin_v1`. */
static int hw_init_dolphin_v1(void)
{
    uint8_t param[2];

    param[0] = 0x64;
    param[1] = 0x28;
    if (ps2_command(0, CMD_SETSTREAM) < 0 || ps2_command(&param[0], CMD_SETRATE) < 0 ||
        ps2_command(&param[1], CMD_SETRATE) < 0) {
        return -1;
    }
    return 0;
}

/** Set up a version 7 pad, Linux's `alps_hw_init_v7`. */
static int hw_init_v7(void)
{
    int reg_val;

    if (enter_command_mode() < 0 || read_reg(0xc2d9) == -1) {
        goto error;
    }
    if (get_v3_v7_resolution(0xc397) < 0) {
        goto error;
    }
    if (write_reg(0xc2c9, 0x64) < 0) {
        goto error;
    }
    reg_val = read_reg(0xc2c4);
    if (reg_val == -1 || write_set_reg(reg_val | 0x02) < 0) {
        goto error;
    }
    exit_command_mode();
    return ps2_command(0, CMD_ENABLE) < 0 ? -1 : 0;

error:
    exit_command_mode();
    return -1;
}

/** Set up a version 8 pad, Linux's `alps_hw_init_ss4_v2`. */
static int hw_init_ss4_v2(void)
{
    uint8_t param[2];

    param[0] = 0x64;
    param[1] = 0x28;
    if (ps2_command(0, CMD_SETSTREAM) < 0 || ps2_command(0, CMD_SETSTREAM) < 0 ||
        ps2_command(&param[0], CMD_SETRATE) < 0 || ps2_command(&param[1], CMD_SETRATE) < 0) {
        goto error;
    }
    /* Fewer noise packets, as Linux has it. */
    if (exit_command_mode() < 0 || enter_command_mode() < 0 || write_reg(0x001D, 0x20) < 0) {
        goto error;
    }
    exit_command_mode();
    return ps2_command(0, CMD_ENABLE) < 0 ? -1 : 0;

error:
    exit_command_mode();
    return -1;
}

/** The limits of a pad of one finger, Linux's `alps_set_abs_params_st`. */
static void set_abs_params_st(void)
{
    evdev_set_abs(pad, ABS_X, 0, priv.x_max, 0, 0);
    evdev_set_abs(pad, ABS_Y, 0, priv.y_max, 0, 0);
    evdev_set_abs(pad, ABS_PRESSURE, 0, 127, 0, 0);
}

/** The slots' axes, Linux's `alps_set_abs_params_mt_common`. */
static void set_abs_params_mt_common(void)
{
    evdev_set_abs(pad, ABS_MT_POSITION_X, 0, priv.x_max, 0, 0);
    evdev_set_abs(pad, ABS_MT_POSITION_Y, 0, priv.y_max, 0, 0);
    evdev_set_abs_res(pad, ABS_MT_POSITION_X, (int32_t)priv.x_res);
    evdev_set_abs_res(pad, ABS_MT_POSITION_Y, (int32_t)priv.y_res);
    evdev_set_capability(pad, EV_KEY, BTN_TOOL_TRIPLETAP);
    evdev_set_capability(pad, EV_KEY, BTN_TOOL_QUADTAP);
}

/** The limits and slots of a semi-MT pad, Linux's
 * `alps_set_abs_params_semi_mt`. */
static void set_abs_params_semi_mt(void)
{
    set_abs_params_mt_common();
    evdev_set_abs(pad, ABS_PRESSURE, 0, 127, 0, 0);
    evdev_init_slots(pad, MAX_TOUCHES, INPUT_MT_POINTER | INPUT_MT_SEMI_MT);
    priv.semi_mt = 1;
}

/** The limits and slots of a version 7 pad, Linux's
 * `alps_set_abs_params_v7`. */
static void set_abs_params_v7(void)
{
    set_abs_params_mt_common();
    evdev_set_capability(pad, EV_KEY, BTN_TOOL_QUINTTAP);
    evdev_init_slots(pad, MAX_TOUCHES, INPUT_MT_POINTER | INPUT_MT_TRACK);
}

/** The limits and slots of a version 8 pad, Linux's
 * `alps_set_abs_params_ss4_v2`. */
static void set_abs_params_ss4_v2(void)
{
    set_abs_params_mt_common();
    evdev_set_abs(pad, ABS_PRESSURE, 0, 127, 0, 0);
    evdev_set_capability(pad, EV_KEY, BTN_TOOL_QUINTTAP);
    evdev_init_slots(pad, MAX_TOUCHES, INPUT_MT_POINTER | INPUT_MT_TRACK);
}

/** Whether the machine is one of the Dell Latitudes whose version 2 pads
 * give the stick's buttons in bits of their own. */
static int has_separate_stick_buttons(void)
{
    unsigned int i;

    for (i = 0; i < sizeof(stick_bits_products) / sizeof(stick_bits_products[0]); i++) {
        if (smbios_system_is("Dell Inc.", stick_bits_products[i])) {
            return 1;
        }
    }
    return 0;
}

/** The rest of `set_protocol` for versions 5 to 8. */
static int set_protocol_new(void)
{
    switch (priv.proto_version) {
    case PROTO_V5:
        priv.hw_init = hw_init_dolphin_v1;
        priv.process_packet = process_touchpad_packet_v3_v5;
        priv.decode_fields = decode_dolphin;
        priv.set_abs_params = set_abs_params_semi_mt;
        priv.nibbles = v3_nibbles;
        priv.addr_command = CMD_RESET_WRAP;
        priv.x_bits = 23;
        priv.y_bits = 12;
        if (dolphin_get_device_area() < 0) {
            return -1;
        }
        break;
    case PROTO_V6:
        priv.hw_init = hw_init_v6;
        priv.process_packet = process_packet_v6;
        priv.set_abs_params = set_abs_params_st;
        priv.nibbles = v6_nibbles;
        priv.x_max = 2047;
        priv.y_max = 1535;
        break;
    case PROTO_V7:
        priv.hw_init = hw_init_v7;
        priv.process_packet = process_packet_v7;
        priv.decode_fields = decode_packet_v7;
        priv.set_abs_params = set_abs_params_v7;
        priv.nibbles = v3_nibbles;
        priv.addr_command = CMD_RESET_WRAP;
        priv.x_max = 0xfff;
        priv.y_max = 0x7ff;
        if (priv.fw_ver[1] != 0xba) {
            priv.flags |= F_BUTTONPAD;
        }
        if (probe_trackstick_v3_v7(REG_BASE_V7) < 0) {
            priv.flags &= ~(unsigned int)F_DUALPOINT;
        }
        break;
    default: /* PROTO_V8 */
        priv.hw_init = hw_init_ss4_v2;
        priv.process_packet = process_packet_ss4_v2;
        priv.decode_fields = decode_ss4_v2;
        priv.set_abs_params = set_abs_params_ss4_v2;
        priv.nibbles = v3_nibbles;
        priv.addr_command = CMD_RESET_WRAP;
        if (set_defaults_ss4_v2() < 0) {
            return -1;
        }
        break;
    }
    return 0;
}

/**
 * Take the protocol `protocol` and what its version needs, Linux's
 * `alps_set_protocol`: the handlers, the limits, and the probes of a stick,
 * a version 5 pad's size and a version 8 pad's OTP bytes. Returns 0, or -1
 * when a version 5 or 8 pad did not answer its probe.
 */
static int set_protocol(const struct protocol *protocol)
{
    priv.proto_version = protocol->version;
    priv.byte0 = protocol->byte0;
    priv.mask0 = protocol->mask0;
    priv.flags = protocol->flags;
    priv.x_max = 2000;
    priv.y_max = 1400;
    priv.x_bits = 15;
    priv.y_bits = 11;
    priv.decode_fields = 0;
    priv.nibbles = 0;
    priv.addr_command = 0;

    switch (priv.proto_version) {
    case PROTO_V1:
    case PROTO_V2:
        priv.hw_init = hw_init_v1_v2;
        priv.process_packet = process_packet_v1_v2;
        priv.set_abs_params = set_abs_params_st;
        priv.x_max = 1023;
        priv.y_max = 767;
        if (has_separate_stick_buttons()) {
            priv.flags |= F_STICK_BITS;
        }
        break;
    case PROTO_V3:
        priv.hw_init = hw_init_v3;
        priv.process_packet = process_packet_v3;
        priv.set_abs_params = set_abs_params_semi_mt;
        priv.decode_fields = decode_pinnacle;
        priv.nibbles = v3_nibbles;
        priv.addr_command = CMD_RESET_WRAP;
        if (probe_trackstick_v3_v7(REG_BASE_PINNACLE) < 0) {
            priv.flags &= ~(unsigned int)F_DUALPOINT;
        }
        break;
    case PROTO_V3_RUSHMORE:
        priv.hw_init = hw_init_rushmore_v3;
        priv.process_packet = process_packet_v3;
        priv.set_abs_params = set_abs_params_semi_mt;
        priv.decode_fields = decode_rushmore;
        priv.nibbles = v3_nibbles;
        priv.addr_command = CMD_RESET_WRAP;
        priv.x_bits = 16;
        priv.y_bits = 12;
        if (probe_trackstick_v3_v7(REG_BASE_RUSHMORE) < 0) {
            priv.flags &= ~(unsigned int)F_DUALPOINT;
        }
        break;
    case PROTO_V4:
        priv.hw_init = hw_init_v4;
        priv.process_packet = process_packet_v4;
        priv.set_abs_params = set_abs_params_semi_mt;
        priv.nibbles = v4_nibbles;
        priv.addr_command = CMD_DISABLE;
        break;
    default:
        return set_protocol_new();
    }
    return 0;
}

/** The model table's row of the E7 signature `e7`, Linux's
 * `alps_match_table`, or NULL. */
static const struct protocol *match_table(const uint8_t *e7)
{
    unsigned int i;

    for (i = 0; i < sizeof(models) / sizeof(models[0]); i++) {
        if (e7[0] == models[i].signature[0] && e7[1] == models[i].signature[1] &&
            e7[2] == models[i].signature[2]) {
            return &models[i].protocol;
        }
    }
    return 0;
}

/**
 * The protocol of a newer pad, not in the model table, whose E7 report is
 * `e7` and EC report `ec`, by the EC report's firmware, as Linux's
 * `alps_identify` matches them. Returns NULL for a pad Linux does not
 * drive, version 9 among them.
 */
static const struct protocol *match_protocol(const uint8_t *e7, const uint8_t *ec)
{
    if (e7[0] == 0x73 && e7[1] == 0x02 && e7[2] == 0x64 && ec[2] == 0x8a) {
        return &v4_protocol;
    }
    if (e7[0] == 0x73 && e7[1] == 0x03 && e7[2] == 0x50 && ec[0] == 0x73 &&
        (ec[1] == 0x01 || ec[1] == 0x02)) {
        return &v5_protocol;
    }
    if (ec[0] == 0x88 && ((ec[1] & 0xf0) == 0xb0 || (ec[1] & 0xf0) == 0xc0)) {
        return &v7_protocol;
    }
    if (ec[0] == 0x88 && ec[1] == 0x08) {
        return &v3_rushmore;
    }
    if (ec[0] == 0x88 && ec[1] == 0x07 && ec[2] >= 0x90 && ec[2] <= 0x9d) {
        return &v3_protocol;
    }
    if (e7[0] == 0x73 && e7[1] == 0x03 && (e7[2] == 0x14 || e7[2] == 0x28)) {
        return &v8_protocol;
    }
    return 0;
}

/**
 * Identify the pad, Linux's `alps_identify`: the E6 report says an ALPS pad
 * answered, and the E7 and EC reports which, by the model table or by the
 * EC report's firmware. With `set`, the protocol is taken (`set_protocol`).
 * Returns 0, or -1 when no pad Linux drives answered.
 */
static int identify(int set)
{
    const struct protocol *protocol;
    uint8_t e6[4], e7[4], ec[4];

    if (rpt_cmd(CMD_SETRES, CMD_SETSCALE11, e6) < 0) {
        return -1;
    }
    if ((e6[0] & 0xf8) != 0 || e6[1] != 0 || (e6[2] != 10 && e6[2] != 100)) {
        return -1;
    }
    if (rpt_cmd(CMD_SETRES, CMD_SETSCALE21, e7) < 0 ||
        rpt_cmd(CMD_SETRES, CMD_RESET_WRAP, ec) < 0 || exit_command_mode() < 0) {
        return -1;
    }
    protocol = match_table(e7);
    if (!protocol) {
        protocol = match_protocol(e7, ec);
    }
    if (!protocol) {
        if (e7[0] == 0x73 && e7[1] == 0x03 && e7[2] == 0xc8) {
            klog(KLOG_WARN, "alps", "probe: unsupported ALPS V9 touchpad: E7=%x %x %x, EC=%x %x %x",
                 e7[0], e7[1], e7[2], ec[0], ec[1], ec[2]);
        }
        return -1;
    }
    if (set) {
        k_memcpy(priv.dev_id, e7, 3);
        k_memcpy(priv.fw_ver, ec, 3);
        return set_protocol(protocol);
    }
    return 0;
}

/** Whether the device is ALPS's CS19, a stick without a pad, which Linux
 * leaves to its TrackPoint driver, Linux's `alps_is_cs19_trackpoint`. */
static int is_cs19_trackpoint(void)
{
    uint8_t param[2];

    param[0] = param[1] = 0;
    if (ps2_command(param, CMD_TP_READ_ID) < 0) {
        return 0;
    }
    return param[0] == 0x02 && (param[1] & 0xf0) == 0x20; /* TP_VARIANT_ALPS */
}

/** Register the pad's event device with the name, numbers and capabilities
 * Linux's psmouse and `alps_init` give it. Returns 0, or -1 when every node
 * is taken. */
static int register_pad(void)
{
    uint8_t keys[EVDEV_KEY_BYTES];

    k_memset(keys, 0, sizeof(keys));
    pad = evdev_register(priv.flags & F_DUALPOINT ? "AlpsPS/2 ALPS DualPoint TouchPad"
                                                  : "AlpsPS/2 ALPS GlidePoint",
                         EVDEV_BUS_I8042, PSMOUSE_VENDOR, PSMOUSE_ALPS, keys);
    if (!pad) {
        return -1;
    }
    evdev_describe(pad, "isa0060/serio1/input0", 0);
    evdev_set_version(pad, priv.proto_version);
    evdev_set_prop(pad, INPUT_PROP_POINTER);
    evdev_set_capability(pad, EV_KEY, BTN_LEFT);
    evdev_set_capability(pad, EV_KEY, BTN_TOUCH);
    evdev_set_capability(pad, EV_KEY, BTN_TOOL_FINGER);
    priv.set_abs_params();
    if (priv.flags & F_WHEEL) {
        evdev_set_capability(pad, EV_REL, REL_WHEEL);
    }
    if (priv.flags & (F_FW_BK_1 | F_FW_BK_2)) {
        evdev_set_capability(pad, EV_KEY, BTN_FORWARD);
        evdev_set_capability(pad, EV_KEY, BTN_BACK);
    }
    if (priv.flags & F_FOUR_BUTTONS) {
        evdev_set_capability(pad, EV_KEY, BTN_RIGHT);
        evdev_set_capability(pad, EV_KEY, BTN_0);
        evdev_set_capability(pad, EV_KEY, BTN_1);
        evdev_set_capability(pad, EV_KEY, BTN_2);
        evdev_set_capability(pad, EV_KEY, BTN_3);
    } else if (priv.flags & F_BUTTONPAD) {
        evdev_set_prop(pad, INPUT_PROP_BUTTONPAD);
    } else {
        evdev_set_capability(pad, EV_KEY, BTN_RIGHT);
        evdev_set_capability(pad, EV_KEY, BTN_MIDDLE);
    }
    return 0;
}

/** Take away the event devices registered. */
static void unregister_all(void)
{
    evdev_unregister(stick);
    evdev_unregister(pad);
    stick = 0;
    pad = 0;
}

const char *alps_mouse_name(const char *proto)
{
    const char *model = priv.flags & F_DUALPOINT ? " ALPS DualPoint TouchPad" : " ALPS GlidePoint";
    size_t a = k_strlen(proto), b = k_strlen(model);

    if (a + b >= sizeof(mouse_name)) {
        a = sizeof(mouse_name) - 1 - b;
    }
    k_memcpy(mouse_name, proto, a);
    k_memcpy(mouse_name + a, model, b + 1);
    return mouse_name;
}

/** Linux's `psmouse_initialize`: the port's rate, resolution and 1:1
 * scaling. */
static void psmouse_initialize(void)
{
    ps2mouse_initialize(NULL);
}

int alps_poll(uint8_t badbyte, uint8_t *buf)
{
    uint8_t p[8];
    int failed;

    if (priv.flags & F_PASS) {
        (void)passthrough_mode_v2(1);
    }
    failed = ps2_command(p, CMD_POLL | ((unsigned int)pktsize << 8)) < 0;
    if (priv.flags & F_PASS) {
        (void)passthrough_mode_v2(0);
    }
    if (failed || (p[0] & priv.mask0) != priv.byte0) {
        return -1;
    }
    /* A first byte of the stick's form: the stick's packet is wanted. */
    if ((badbyte & 0xc8) == 0x08 && ps2_command(p, CMD_POLL | (3u << 8)) < 0) {
        return -1;
    }
    k_memcpy(buf, p, sizeof(p));
    return pktsize;
}

int alps_reconnect(void)
{
    psmouse_reset();
    if (identify(1) < 0) {
        return -1;
    }
    if (priv.hw_init() < 0) {
        klog(KLOG_WARN, "alps", "reconnect: the touchpad refused its set-up");
        return -1;
    }
    psmouse_initialize();
    pktsize = priv.proto_version == PROTO_V4 ? 8 : 6;
    pktcnt = 0;
    out_of_sync = 0;
    flush_armed = 0;
    return 0;
}

void alps_disconnect(void)
{
    psmouse_reset();
    flush_armed = 0;
    unregister_all();
    evdev_unregister(bare);
    bare = 0;
    bare_failed = 0;
    pktcnt = 0;
}

int alps_init(void)
{
    k_memset(&priv, 0, sizeof(priv));
    priv.second_touch = -1;
    ps2_command(0, CMD_RESET_DIS);
    /* alps_detect: a trial identification, the CS19 check, a reset, and
     * the identification kept. */
    if (identify(0) < 0) {
        return 0;
    }
    if (is_cs19_trackpoint()) {
        klog(KLOG_DEBUG, "alps", "probe: ALPS CS19 trackpoint-only device detected, ignoring");
        return 0;
    }
    psmouse_reset();
    if (identify(1) < 0) {
        return 0;
    }
    klog(KLOG_INFO, "alps", "found: protocol %x, E7 %x %x %x, EC %x %x %x, flags %x",
         priv.proto_version, priv.dev_id[0], priv.dev_id[1], priv.dev_id[2], priv.fw_ver[0],
         priv.fw_ver[1], priv.fw_ver[2], priv.flags);
    if (priv.hw_init() < 0) {
        klog(KLOG_WARN, "alps", "init: the touchpad refused its set-up");
        goto failed;
    }
    /* Linux registers the stick in alps_init, before psmouse registers the
     * pad. */
    if (priv.flags & F_DUALPOINT) {
        stick = register_relative("AlpsPS/2 ALPS DualPoint Stick", "isa0060/serio1/input1",
                                  PSMOUSE_ALPS, priv.proto_version,
                                  (priv.flags & F_STICK_PRESSURE) != 0, 1);
        if (!stick) {
            klog(KLOG_WARN, "alps", "register: no event device for the trackstick");
            goto failed;
        }
    }
    psmouse_initialize();
    if (register_pad() < 0) {
        klog(KLOG_WARN, "alps", "register: no event device for the touchpad");
        goto failed;
    }
    mouse_attach_touchpad(pad);
    pktsize = priv.proto_version == PROTO_V4 ? 8 : 6;
    pktcnt = 0;
    out_of_sync = 0;
    flush_armed = 0;
    return 1;

failed:
    unregister_all();
    psmouse_reset();
    return -1;
}
