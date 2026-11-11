/*
 * trackpoint.c: a TrackPoint behind a touchpad's pass-through port or alone
 * on the auxiliary port, found, set up and adjusted as Linux 6.12's
 * trackpoint.c does (trackpoint.h).
 *
 * The stick takes the PS/2 mouse's commands and two of its own: read-ID,
 * 0xE1, answered by its kind and firmware, and 0xE2, followed by an
 * operation and its arguments: read a location of its RAM, write one,
 * toggle bits of one, or reset to its power-on values. Its settings are
 * those locations, as IBM's TrackPoint System Engineering document gives
 * them, and the files of its serio port in /sys read and write them.
 */

#include "trackpoint.h"

#include "ps2mouse.h"
#include "../../fs/sysfs.h"
#include "../../klog.h"
#include "../../lib/kerrno.h"

/* The commands, the 0xE2 operations and the locations Linux uses beside
 * the settings' own. */
#define TP_COMMAND           0xE2
#define TP_READ_ID           0xE1
#define TP_EXT_BTN           0x4B /* the extended button query */
#define TP_POR               0x7F /* the power-on reset */
#define TP_WRITE_MEM         0x81
#define TP_TOGGLE            0x47
#define TP_TOGGLE_MB         0x23 /* the middle button's disable */
#define TP_MASK_MB           0x01
#define TP_TOGGLE_SOURCE_TAG 0x20 /* bit 3 of a packet names its source */
#define TP_MASK_SOURCE_TAG   0x80
#define TP_TOGGLE_TWOHAND    0x2D /* two-handed mode */
#define TP_MASK_TWOHAND      0x01

/* The extended button query's answer when a stick gives none: three
 * buttons of three. */
#define TP_DEFAULT_BUTTONS 0x33

/* The reply of a power-on reset that passed. */
#define TP_POR_OK0 0xAA
#define TP_POR_OK1 0x00

/*
 * A setting, Linux's `struct trackpoint_attr_data`: its file's name; the
 * location it is written to, or toggled in; the bit of that location for a
 * toggle, 0 for a location written whole; whether the file shows the bit
 * turned over; and its value at power-on.
 */
struct tp_attr {
    const char *name;
    uint8_t command;
    uint8_t mask;
    uint8_t inverted;
    uint8_t power_on;
};

/* The settings in the order Linux's `trackpoint_attrs` lists them. */
#define TP_SENSITIVITY     0
#define TP_PRESS_TO_SELECT 11
#define TP_ATTRS           14
static const struct tp_attr attrs[TP_ATTRS] = {
    {"sensitivity", 0x4A, 0, 0, 0x80}, {"speed", 0x60, 0, 0, 0x61},
    {"inertia", 0x4D, 0, 0, 0x06},     {"reach", 0x57, 0, 0, 0x0A},
    {"draghys", 0x58, 0, 0, 0xFF},     {"mindrag", 0x59, 0, 0, 0x14},
    {"thresh", 0x5C, 0, 0, 0x08},      {"upthresh", 0x5A, 0, 0, 0xFF},
    {"ztime", 0x5E, 0, 0, 0x26},       {"jenks", 0x5D, 0, 0, 0x87},
    {"drift_time", 0x5F, 0, 0, 0x05},  {"press_to_select", 0x2C, 0x01, 0, 0x00},
    {"skipback", 0x2D, 0x08, 0, 0x00}, {"ext_dev", 0x23, 0x02, 1, 0x00}};

/* The order in which Linux's `trackpoint_sync` writes them. */
static const uint8_t sync_order[TP_ATTRS] = {0, 2, 1, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13};

/* The vendors, Linux's `trackpoint_variants`, by kind. */
static const char *const variants[] = {"",    "IBM",           "ALPS",     "Elan",
                                       "NXP", "JYT_Synaptics", "Synaptics"};

/* The stick found: its line, kind, firmware and buttons, and each
 * setting's value as Linux's `struct trackpoint_data` keeps it, a toggle
 * as 0 or 1 before any turning over. */
static const struct ps2io *line;
static uint8_t variant;
static uint8_t firmware;
static uint8_t buttons;
static uint8_t value[TP_ATTRS];

/** Read location `loc` into `*out`, Linux's `trackpoint_read`. */
static int tp_read(uint8_t loc, uint8_t *out)
{
    return ps2io_command(line, TP_COMMAND, &loc, 1, out, 1);
}

/** Write `val` to location `loc`, Linux's `trackpoint_write`. */
static int tp_write(uint8_t loc, uint8_t val)
{
    uint8_t param[3];

    param[0] = TP_WRITE_MEM;
    param[1] = loc;
    param[2] = val;
    return ps2io_command(line, TP_COMMAND, param, 3, 0, 0);
}

/** Toggle the bits `mask` of location `loc`, Linux's
 * `trackpoint_toggle_bit`, which refuses a location outside 0x20 to 0x2E. */
static int tp_toggle(uint8_t loc, uint8_t mask)
{
    uint8_t param[3];

    if (loc < 0x20 || loc >= 0x2F) {
        return -1;
    }
    param[0] = TP_TOGGLE;
    param[1] = loc;
    param[2] = mask;
    return ps2io_command(line, TP_COMMAND, param, 3, 0, 0);
}

/** Set the bits `mask` of location `loc` to `on`, reading it first and
 * toggling only when it differs, Linux's `trackpoint_update_bit`. */
static int tp_update_bit(uint8_t loc, uint8_t mask, int on)
{
    uint8_t data;

    if (tp_read(loc, &data) < 0) {
        return -1;
    }
    if (((data & mask) == mask) != (on != 0)) {
        return tp_toggle(loc, mask);
    }
    return 0;
}

/** The power-on reset, Linux's `trackpoint_power_on_reset`: 0 when the
 * stick answered 0xAA 0x00. */
static int tp_power_on_reset(void)
{
    uint8_t por = TP_POR, reply[2];

    if (ps2io_command(line, TP_COMMAND, &por, 1, reply, 2) < 0) {
        return -1;
    }
    return reply[0] == TP_POR_OK0 && reply[1] == TP_POR_OK1 ? 0 : -1;
}

/** Read the stick's ID into `*kind` and `*fw`, Linux's
 * `trackpoint_start_protocol`: 0 for one of the kinds Linux knows. */
static int tp_start(const struct ps2io *io, uint8_t *kind, uint8_t *fw)
{
    uint8_t id[2];

    id[0] = id[1] = 0;
    if (ps2io_command(io, TP_READ_ID, 0, 0, id, 2) < 0) {
        return -1;
    }
    if (id[0] < TP_VARIANT_IBM || id[0] > TP_VARIANT_SYNAPTICS) {
        return -1;
    }
    if (kind) {
        *kind = id[0];
    }
    if (fw) {
        *fw = id[1];
    }
    return 0;
}

/** Whether the setting `i` is on this stick, Linux's
 * `trackpoint_is_attr_available`: all on IBM's, two on the others. */
static int available(int i)
{
    return variant == TP_VARIANT_IBM || i == TP_SENSITIVITY || i == TP_PRESS_TO_SELECT;
}

/**
 * Write the settings to the stick, Linux's `trackpoint_sync`: on an IBM
 * stick in an unknown state, two-handed mode, the source tag and the middle
 * button's disable turned off first; then each setting the stick has, all
 * of them in an unknown state and only those off their power-on value after
 * a reset. A refusal is not reported, as Linux reports none.
 */
static void tp_sync(int in_power_on_state)
{
    const struct tp_attr *a;
    int k, i;

    if (!in_power_on_state && variant == TP_VARIANT_IBM) {
        (void)tp_update_bit(TP_TOGGLE_TWOHAND, TP_MASK_TWOHAND, 0);
        (void)tp_update_bit(TP_TOGGLE_SOURCE_TAG, TP_MASK_SOURCE_TAG, 0);
        (void)tp_update_bit(TP_TOGGLE_MB, TP_MASK_MB, 0);
    }
    for (k = 0; k < TP_ATTRS; k++) {
        i = sync_order[k];
        a = &attrs[i];
        if ((in_power_on_state && value[i] == a->power_on) || !available(i)) {
            continue;
        }
        if (!a->mask) {
            (void)tp_write(a->command, value[i]);
        } else {
            (void)tp_update_bit(a->command, a->mask, value[i]);
        }
    }
}

int trackpoint_detect(const struct ps2io *io)
{
    uint8_t kind, fw, info;
    int i;

    if (tp_start(io, &kind, &fw) < 0) {
        return -1;
    }
    line = io;
    variant = kind;
    firmware = fw;
    for (i = 0; i < TP_ATTRS; i++) {
        value[i] = attrs[i].power_on;
    }
    if (variant != TP_VARIANT_IBM) {
        info = TP_DEFAULT_BUTTONS; /* newer kinds have no extended query */
    } else if (tp_read(TP_EXT_BTN, &info) < 0) {
        klog(KLOG_WARN, "trackpoint", "buttons: no extended button data, assuming 3");
        info = TP_DEFAULT_BUTTONS;
    } else if (info == 0) {
        klog(KLOG_WARN, "trackpoint", "buttons: 0 in the extended button data, assuming 3");
        info = TP_DEFAULT_BUTTONS;
    }
    buttons = info;
    if (variant != TP_VARIANT_IBM || tp_power_on_reset() < 0) {
        tp_sync(0); /* not reset, so its state is unknown */
    }
    klog(KLOG_INFO, "trackpoint", "found: %s TrackPoint firmware: 0x%02x, buttons: %d/%d",
         variants[variant], firmware, (buttons & 0xf0) >> 4, buttons & 0x0f);
    return 0;
}

int trackpoint_identify(const struct ps2io *io)
{
    return tp_start(io, 0, 0);
}

const char *trackpoint_vendor(void)
{
    return variants[variant <= TP_VARIANT_SYNAPTICS ? variant : 0];
}

int trackpoint_buttons(void)
{
    return buttons & 0x0f;
}

int trackpoint_reconnect(const struct ps2io *io)
{
    int was_reset;

    if (tp_start(io, 0, 0) < 0) {
        return -1;
    }
    line = io;
    was_reset = variant == TP_VARIANT_IBM && tp_power_on_reset() == 0;
    tp_sync(was_reset);
    return 0;
}

/** A setting's file, Linux's `trackpoint_show_int_attr`: its value, a
 * toggle turned over where the setting says, and a newline. */
static int32_t show_attr(void *obj, int arg, char *buf, uint32_t cap)
{
    char text[4];
    unsigned int v = value[arg];
    int n = 0, i;

    (void)obj;
    if (attrs[arg].inverted) {
        v = !v;
    }
    do {
        text[n++] = (char)('0' + v % 10u);
        v /= 10u;
    } while (v);
    for (i = 0; i < n && (uint32_t)i < cap; i++) {
        buf[i] = text[n - 1 - i];
    }
    if ((uint32_t)n < cap) {
        buf[n] = '\n';
    }
    return n + 1;
}

/** Parse `buf`, `len` bytes, as Linux's `kstrtou8` in base 10: digits
 * after an optional '+', then an optional newline. 0, -EINVAL or
 * -ERANGE. */
static int parse_u8(const char *buf, uint32_t len, uint8_t *out)
{
    uint32_t i = 0, v = 0;
    int digits = 0;

    if (i < len && buf[i] == '+') {
        i++;
    }
    for (; i < len && buf[i] >= '0' && buf[i] <= '9'; i++, digits++) {
        v = v * 10u + (uint32_t)(buf[i] - '0');
        if (v > 255u) {
            v = 256u; /* held past the limit, so a long number stays out of range */
        }
    }
    if (digits == 0) {
        return -EINVAL;
    }
    if (i < len && buf[i] == '\n') {
        i++;
    }
    if (i < len) {
        return -EINVAL;
    }
    if (v > 255u) {
        return -ERANGE;
    }
    *out = (uint8_t)v;
    return 0;
}

/** Parse `buf`, `len` bytes, as Linux 6.12's `kstrtobool`, by its first
 * byte or two: y, t, 1 or "on" is true, n, f, 0 or "of" false, in either
 * case. 0 or -EINVAL. */
static int parse_bool(const char *buf, uint32_t len, int *out)
{
    char c = len > 0 ? buf[0] : 0, c2 = len > 1 ? buf[1] : 0;

    switch (c) {
    case 'y':
    case 'Y':
    case 't':
    case 'T':
    case '1':
        *out = 1;
        return 0;
    case 'n':
    case 'N':
    case 'f':
    case 'F':
    case '0':
        *out = 0;
        return 0;
    case 'o':
    case 'O':
        if (c2 == 'n' || c2 == 'N') {
            *out = 1;
            return 0;
        }
        if (c2 == 'f' || c2 == 'F') {
            *out = 0;
            return 0;
        }
        break;
    default:
        break;
    }
    return -EINVAL;
}

/**
 * A write to a setting's file, Linux's `trackpoint_set_int_attr` and
 * `trackpoint_set_bit_attr` inside `psmouse_attr_set_helper`: the touchpad
 * and the stick disabled; a number from 0 to 255 written to the stick, or a
 * truth turned over where the setting says and toggled on the stick when it
 * changes; the stick and the touchpad enabled again. The value kept changes
 * even when the stick refuses it, as Linux's does. Returns `len`, -ENODEV
 * with no stick reporting, -EINVAL or -ERANGE for text that is no value,
 * or -EIO when the stick refused.
 */
static int32_t store_attr(void *obj, int arg, const char *buf, uint32_t len)
{
    const struct tp_attr *a = &attrs[arg];
    const struct ps2io *io;
    uint8_t v = 0;
    uint32_t n;
    int on = 0, rc;

    (void)obj;
    for (n = 0; n < len && buf[n]; n++) { /* Linux's store reads a string */
    }
    io = ps2mouse_guest_change_begin();
    if (!io) {
        return -ENODEV;
    }
    line = io;
    if (!a->mask) {
        rc = parse_u8(buf, n, &v);
        if (rc == 0) {
            value[arg] = v;
            rc = tp_write(a->command, v) < 0 ? -EIO : 0;
        }
    } else {
        rc = parse_bool(buf, n, &on);
        if (rc == 0) {
            if (a->inverted) {
                on = !on;
            }
            if (value[arg] != (uint8_t)on) {
                value[arg] = (uint8_t)on;
                rc = tp_toggle(a->command, a->mask) < 0 ? -EIO : 0;
            }
        }
    }
    ps2mouse_guest_change_end();
    return rc < 0 ? rc : (int32_t)len;
}

int trackpoint_sysfs_add(int dir)
{
    struct sysfs_attr attr;
    int i, h;

    for (i = 0; i < TP_ATTRS; i++) {
        if (!available(i)) {
            continue;
        }
        attr.name = attrs[i].name;
        attr.mode = 0644u;
        attr.show = show_attr;
        attr.store = store_attr;
        h = sysfs_add_attr(dir, &attr, 0, i);
        if (h < 0) {
            return h;
        }
    }
    return 0;
}
