/*
 * evdev_sysfs.c: the input devices' entries in /sys, by which udev and
 * libinput find them, and their uevents, as Linux's input core makes them:
 * /sys/class/input/inputN with its eventN, below the device each came
 * from, an i8042 port or a USB interface, and kinputd, the worker that
 * makes them.
 */

#include "evdev.h"

#include "fs/synthfs.h"
#include "fs/sysfs.h"
#include "klog.h"
#include "lib/kerrno.h"
#include "lib/kstring.h"
#include "syscall.h"
#include "usb/core.h"
#ifndef RACH2_HOSTTEST
#include "arch/arch.h"
#include "lib/kirq.h"
#include "proc/process.h"
#include "proc/sched.h"
#include "proc/wait_queue.h"
#endif

/*
 * A driver registers its device in an interrupt as often as not, the USB
 * keyboards and mice from the timer's pass of the USB stack, and /sys's
 * entries take heap and the big lock's order, so the entries are not made
 * there. Each registration has a number of its own (`evdev_desc`'s `gen`);
 * `evdev_sysfs_sync` compares the devices registered with those published
 * here, takes away the entries of each that has gone, sending "remove",
 * and then makes those of each new one, sending "add". Two passes, so that
 * a device that comes back at the same USB port before a sync finds its
 * interface gone and makes it again.
 *
 * The parent of a device is read from its EVIOCGPHYS, which each driver
 * writes as Linux's do: "isa0060/serioK/input0" is the i8042's port K,
 * under /sys/devices/platform/i8042/serioK; "synaptics-pt/serio0/input0"
 * is the touchpad's pass-through port, serio2 below serio1, as Linux
 * numbers it after the i8042's two; and "usb-N/inputI" is interface
 * number I of the device of the USB stack's entry N - 1 (hid.c), whose
 * directory B-P:C.I is made below its device, one for each interface. Any
 * other, the ACPI buttons', is a device with no parent here, under
 * /sys/devices/virtual/input, as Linux puts one made by uinput.
 */

/* Linux's UEVENT_BUFFER_SIZE, the most bytes of a uevent's variables, and
 * KEY_MIN_INTERESTING, the first key a modalias names. */
#define UEVENT_VARS_MAX     2048
#define KEY_MIN_INTERESTING 113

/* The serio ports: the i8042's, 0 the keyboard's and 1 the mouse's, and
 * the touchpad's pass-through port, EVDEV_SERIO_PASS_THROUGH. */
#define SERIO_I8042_PORTS 2
#define SERIO_PORTS       3

/*
 * A USB interface's directory that published devices are below: its
 * handle, 0 for a free entry, its link in /sys/bus/usb/devices, the devices
 * below it, and the interface as the USB stack described it when the
 * directory was made, which its files show. The last device withdrawn
 * takes it away, after its own "remove", as Linux's USB core removes an
 * interface after the input devices below it.
 */
struct iface_sys {
    int dir, link;
    int users;
    struct usb_sys_row usb;
};

static struct iface_sys ifaces[EVDEV_MAX];

/*
 * A device published: the registration whose entries these are, 0 for
 * none; its description; the N of inputN, counted from 0 and never used
 * again, as Linux's input core numbers; the handles of sysfs.h of its
 * entries; and the USB interface it is below, or NULL.
 */
struct input_sys {
    uint32_t gen;
    uint32_t failed; /* the registration last refused, logged once */
    int slot;        /* the N of eventN, evdev's */
    int number;
    struct evdev_desc d;
    int holder; /* the directory inputN is in */
    int input, event;
    int class_input, class_event, char_link;
    struct iface_sys *iface;
};

static struct input_sys records[EVDEV_MAX];

/* The N of the next inputN. */
static uint32_t next_number;

/* The parents, made on first use: /sys/class/input,
 * /sys/devices/virtual/input, the i8042 with its link, /sys/bus/serio's
 * `devices`, and the serio ports and their bus links. The pass-through
 * port is removed on disconnect; the fixed parents stay. 0 for none. */
static struct {
    int class_input;
    int virtual_input;
    int i8042, i8042_link;
    int bus_serio, serio_devices;
    int serio[SERIO_PORTS];
    int serio_link[SERIO_PORTS];
} parents;

/* Each serio port's firmware ID, NULL for none; the functions that add its
 * driver's own files to its directory, which stay while it does, and its
 * protocol's, which a change of protocol replaces, NULL for none; and the
 * entries the protocol's function made, 0 past the last. */
#define SERIO_FILES_MAX 24
static const char *serio_firmware[SERIO_PORTS];
static int (*serio_driver[SERIO_PORTS])(int dir);
static int (*serio_files[SERIO_PORTS])(int dir);
static int serio_made[SERIO_PORTS][SERIO_FILES_MAX];

/* What a show writes: the bytes so far, which may pass `cap`, so that the
 * show returns how many it needs; a `cap` of 0 counts alone. */
struct emit {
    char *buf;
    uint32_t cap, len;
};

/** Start `e` on the show's buffer. */
static void emit_init(struct emit *e, char *buf, uint32_t cap)
{
    e->buf = buf;
    e->cap = cap;
    e->len = 0;
}

/** Append the `n` bytes at `s` to `e`, as far as its room goes. */
static void emit_bytes(struct emit *e, const char *s, uint32_t n)
{
    uint32_t i;

    for (i = 0; i < n; i++, e->len++) {
        if (e->len < e->cap) {
            e->buf[e->len] = s[i];
        }
    }
}

/** Append the string `s` to `e`. */
static void emit_str(struct emit *e, const char *s)
{
    emit_bytes(e, s, (uint32_t)k_strlen(s));
}

/** Append the decimal `v` to `e`. */
static void emit_u32(struct emit *e, uint32_t v)
{
    char digits[11];
    int n = 0;

    do {
        digits[n++] = (char)('0' + v % 10u);
        v /= 10u;
    } while (v);
    while (n > 0) {
        emit_bytes(e, &digits[--n], 1);
    }
}

/** Append `v` in hexadecimal to `e`, in at least `width` digits, zeros
 * before, in capitals when `upper` is nonzero. */
static void emit_hex(struct emit *e, uint32_t v, int width, int upper)
{
    const char *hex = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    char digits[8];
    int n = 0;

    do {
        digits[n++] = hex[v & 15u];
        v >>= 4;
    } while (v);
    while (n < width && n < 8) {
        digits[n++] = '0';
    }
    while (n > 0) {
        emit_bytes(e, &digits[--n], 1);
    }
}

/** Write "PREFIXN" into `out` of `cap` bytes, cut to fit. */
static void name_num(char *out, uint32_t cap, const char *prefix, uint32_t n)
{
    struct emit e;

    emit_init(&e, out, cap - 1);
    emit_str(&e, prefix);
    emit_u32(&e, n);
    out[e.len < cap - 1 ? e.len : cap - 1] = '\0';
}

/** Whether bit `n` of `bits` is set. */
static int bit_on(const uint8_t *bits, unsigned n)
{
    return (bits[n >> 3] >> (n & 7)) & 1;
}

/** The 32 bits at byte `at` of `bits`, the first byte lowest. */
static uint32_t word32(const uint8_t *bits, unsigned at)
{
    return (uint32_t)bits[at] | (uint32_t)bits[at + 1] << 8 | (uint32_t)bits[at + 2] << 16 |
           (uint32_t)bits[at + 3] << 24;
}

/**
 * Append the `count` bits at `bits` as Linux's input_print_bitmap writes
 * them: the kernel's longs in hexadecimal from the highest that has a bit,
 * separated by spaces, or "0" when none has. A long is 32 bits on i686 and
 * to an i386 program, whose `in_compat_syscall` is true, and 64 on x86-64,
 * where it is printed as its 32-bit halves, the low half padded to 8 digits
 * when the high one has a bit.
 */
static void emit_bitmap(struct emit *e, const uint8_t *bits, unsigned count)
{
    unsigned halves = sizeof(long) == 4 || in_compat_syscall() ? 1u : 2u;
    int longs = (int)((count + 32 * halves - 1) / (32 * halves));
    int i, j, top, printed = 0;

    for (i = longs - 1; i >= 0; i--) {
        /* The highest half of this long that has a bit, or -1. */
        for (top = (int)halves - 1; top >= 0; top--) {
            if (word32(bits, ((unsigned)i * halves + (unsigned)top) * 4u) != 0) {
                break;
            }
        }
        if (top < 0 && !printed) {
            continue;
        }
        if (top < 0) {
            top = 0;
        }
        for (j = top; j >= 0; j--) {
            emit_hex(e, word32(bits, ((unsigned)i * halves + (unsigned)j) * 4u), j < top ? 8 : 0,
                     0);
        }
        printed = 1;
        if (i > 0) {
            emit_str(e, " ");
        }
    }
    if (!printed) {
        emit_str(e, "0");
    }
}

/* The types of a device's capabilities/ files, in Linux's order, with
 * their names, the names of their uevent's variables and the codes of
 * each. */
static const struct {
    const char *name;
    const char *var;
    unsigned type;
    unsigned count;
} cap_types[] = {{"ev", "EV=", 0, EV_CNT},         {"key", "KEY=", EV_KEY, KEY_CNT},
                 {"rel", "REL=", EV_REL, REL_CNT}, {"abs", "ABS=", EV_ABS, ABS_CNT},
                 {"msc", "MSC=", EV_MSC, MSC_CNT}, {"led", "LED=", EV_LED, LED_CNT},
                 {"snd", "SND=", EV_SND, SND_CNT}, {"ff", "FF=", EV_FF, FF_CNT},
                 {"sw", "SW=", EV_SW, SW_CNT}};

#define CAP_TYPES ((int)(sizeof(cap_types) / sizeof(cap_types[0])))

/** The bits of `type` of `d`, 0 for the event types themselves. */
static const uint8_t *desc_bits(const struct evdev_desc *d, unsigned type)
{
    switch (type) {
    case EV_KEY:
        return d->key;
    case EV_REL:
        return d->rel;
    case EV_ABS:
        return d->abs;
    case EV_MSC:
        return d->msc;
    case EV_LED:
        return d->led;
    case EV_SND:
        return d->snd;
    case EV_FF:
        return d->ff;
    case EV_SW:
        return d->sw;
    default:
        return d->ev;
    }
}

/** Append `tag`, then each code set in `bits` from `from` up to but not
 * including `to`, in capital hexadecimal, each followed by a comma. */
static void emit_codes(struct emit *e, char tag, const uint8_t *bits, unsigned from, unsigned to)
{
    unsigned n;

    emit_bytes(e, &tag, 1);
    for (n = from; n < to; n++) {
        if (bit_on(bits, n)) {
            emit_hex(e, n, 0, 1);
            emit_str(e, ",");
        }
    }
}

/** A modalias's IDs and event types: "input:bBBBBvVVVVpPPPPeEEEE-e0,1,". */
static void emit_modalias_head(struct emit *e, const struct evdev_desc *d)
{
    emit_str(e, "input:b");
    emit_hex(e, d->id[0], 4, 1);
    emit_str(e, "v");
    emit_hex(e, d->id[1], 4, 1);
    emit_str(e, "p");
    emit_hex(e, d->id[2], 4, 1);
    emit_str(e, "e");
    emit_hex(e, d->id[3], 4, 1);
    emit_str(e, "-");
    emit_codes(e, 'e', d->ev, 0, EV_CNT - 1);
}

/** A modalias's codes after its keys: axes, MSC codes, LEDs, sounds,
 * effects and switches. */
static void emit_modalias_tail(struct emit *e, const struct evdev_desc *d)
{
    emit_codes(e, 'r', d->rel, 0, REL_CNT - 1);
    emit_codes(e, 'a', d->abs, 0, ABS_CNT - 1);
    emit_codes(e, 'm', d->msc, 0, MSC_CNT - 1);
    emit_codes(e, 'l', d->led, 0, LED_CNT - 1);
    emit_codes(e, 's', d->snd, 0, SND_CNT - 1);
    emit_codes(e, 'f', d->ff, 0, FF_CNT - 1);
    emit_codes(e, 'w', d->sw, 0, SW_CNT - 1);
}

/**
 * Append the modalias of `d` as Linux's input_print_modalias writes it into
 * `size` bytes with its NUL: whole when it fits, and otherwise with as many
 * keys as leave room for the codes after them, the keys ended by "+,", when
 * that fits. Each code of a type stops one short of the type's count, as
 * Linux's loop does.
 */
static void emit_modalias(struct emit *e, const struct evdev_desc *d, uint32_t size)
{
    struct emit count;
    long head, keys, tail, room, used = 1;
    unsigned n;

    emit_init(&count, NULL, 0);
    emit_modalias_head(&count, d);
    head = (long)count.len;
    emit_codes(&count, 'k', d->key, KEY_MIN_INTERESTING, KEY_MAX);
    keys = (long)count.len - head;
    emit_modalias_tail(&count, d);
    tail = (long)count.len - head - keys;
    emit_modalias_head(e, d);
    room = (long)size - tail - 3 - head;
    if ((long)size >= head + keys + tail + 1 || keys <= 3 || room < 1) {
        emit_codes(e, 'k', d->key, KEY_MIN_INTERESTING, KEY_MAX);
    } else {
        emit_str(e, "k");
        for (n = KEY_MIN_INTERESTING; n < KEY_MAX; n++) {
            if (bit_on(d->key, n)) {
                emit_init(&count, NULL, 0);
                emit_hex(&count, n, 0, 1);
                if (used + (long)count.len + 1 > room) {
                    break;
                }
                emit_hex(e, n, 0, 1);
                emit_str(e, ",");
                used += (long)count.len + 1;
            }
        }
        emit_str(e, "+,");
    }
    emit_modalias_tail(e, d);
}

/* The `arg` of inputN's own attributes. */
#define IN_NAME       0
#define IN_PHYS       1
#define IN_UNIQ       2
#define IN_MODALIAS   3
#define IN_PROPERTIES 4
#define IN_UEVENT     5

/**
 * The uevent's variables of the device `r`, as Linux's input_dev_uevent
 * gives them: its IDs, name and place, its properties and event types, the
 * codes of each type it has, and its modalias, cut to what is left of the
 * 2048 bytes once the variables before it, the longest action and SEQNUM
 * are counted, so that every uevent of the device fits, where Linux's own
 * cut leaves SEQNUM no room.
 */
static void emit_uevent(struct emit *e, const struct input_sys *r)
{
    char path[256];
    long room;
    int i;

    emit_str(e, "PRODUCT=");
    for (i = 0; i < 4; i++) {
        emit_hex(e, r->d.id[i], 0, 0);
        emit_str(e, i < 3 ? "/" : "\n");
    }
    if (r->d.name[0]) {
        emit_str(e, "NAME=\"");
        emit_str(e, r->d.name);
        emit_str(e, "\"\n");
    }
    if (r->d.phys[0]) {
        emit_str(e, "PHYS=\"");
        emit_str(e, r->d.phys);
        emit_str(e, "\"\n");
    }
    if (r->d.has_uniq) {
        emit_str(e, "UNIQ=\"");
        emit_str(e, r->d.uniq);
        emit_str(e, "\"\n");
    }
    emit_str(e, "PROP=");
    emit_bitmap(e, r->d.prop, INPUT_PROP_CNT);
    emit_str(e, "\n");
    for (i = 0; i < CAP_TYPES; i++) {
        if (i == 0 || bit_on(r->d.ev, cap_types[i].type)) {
            emit_str(e, cap_types[i].var);
            emit_bitmap(e, desc_bits(&r->d, cap_types[i].type), cap_types[i].count);
            emit_str(e, "\n");
        }
    }
    sysfs_node_path(r->input, path, (int)sizeof(path));
    room = UEVENT_VARS_MAX - (long)sizeof("ACTION=offline") - (long)sizeof("DEVPATH=") -
           ((long)k_strlen(path) - 4) - (long)sizeof("SUBSYSTEM=input") - (long)e->len -
           (long)sizeof("MODALIAS=") - (long)sizeof("SEQNUM=4294967295");
    emit_str(e, "MODALIAS=");
    emit_modalias(e, &r->d, room > 0 ? (uint32_t)room : 0u);
    emit_str(e, "\n");
}

/** An attribute of inputN, `arg` IN_*: its name, place and serial, its
 * modalias and properties, each on a line, and its uevent. */
static int32_t show_input(void *obj, int arg, char *buf, uint32_t cap)
{
    const struct input_sys *r = (const struct input_sys *)obj;
    struct emit e;

    emit_init(&e, buf, cap);
    switch (arg) {
    case IN_NAME:
        emit_str(&e, r->d.name);
        emit_str(&e, "\n");
        break;
    case IN_PHYS:
        emit_str(&e, r->d.phys);
        emit_str(&e, "\n");
        break;
    case IN_UNIQ:
        emit_str(&e, r->d.uniq);
        emit_str(&e, "\n");
        break;
    case IN_MODALIAS:
        emit_modalias(&e, &r->d, 4096u);
        emit_str(&e, "\n");
        break;
    case IN_PROPERTIES:
        emit_bitmap(&e, r->d.prop, INPUT_PROP_CNT);
        emit_str(&e, "\n");
        break;
    default:
        emit_uevent(&e, r);
        break;
    }
    return (int32_t)e.len;
}

/** A file of inputN/id, `arg` 0 to 3: the bus, vendor, product or version,
 * four hexadecimal digits. */
static int32_t show_id(void *obj, int arg, char *buf, uint32_t cap)
{
    const struct input_sys *r = (const struct input_sys *)obj;
    struct emit e;

    emit_init(&e, buf, cap);
    emit_hex(&e, r->d.id[arg & 3], 4, 0);
    emit_str(&e, "\n");
    return (int32_t)e.len;
}

/** A file of inputN/capabilities, `arg` the row of `cap_types`. */
static int32_t show_cap(void *obj, int arg, char *buf, uint32_t cap)
{
    const struct input_sys *r = (const struct input_sys *)obj;
    struct emit e;

    emit_init(&e, buf, cap);
    emit_bitmap(&e, desc_bits(&r->d, cap_types[arg].type), cap_types[arg].count);
    emit_str(&e, "\n");
    return (int32_t)e.len;
}

/** eventN's `dev`, "13:M", with `arg` 0, and its uevent with 1: its
 * numbers and its name in /dev, as Linux's device core gives them. */
static int32_t show_event(void *obj, int arg, char *buf, uint32_t cap)
{
    const struct input_sys *r = (const struct input_sys *)obj;
    uint32_t minor = EVDEV_MINOR_BASE + (uint32_t)r->slot;
    struct emit e;

    emit_init(&e, buf, cap);
    if (arg == 0) {
        emit_u32(&e, EVDEV_MAJOR);
        emit_str(&e, ":");
        emit_u32(&e, minor);
        emit_str(&e, "\n");
    } else {
        emit_str(&e, "MAJOR=");
        emit_u32(&e, EVDEV_MAJOR);
        emit_str(&e, "\nMINOR=");
        emit_u32(&e, minor);
        emit_str(&e, "\nDEVNAME=input/event");
        emit_u32(&e, (uint32_t)r->slot);
        emit_str(&e, "\n");
    }
    return (int32_t)e.len;
}

/* The `arg` of a serio port's files: the port times 16 plus one of
 * these. */
#define SERIO_DESCRIPTION 0
#define SERIO_MODALIAS    1
#define SERIO_BIND_MODE   2
#define SERIO_TYPE        3 /* to 6: id/type, proto, id and extra */
#define SERIO_UEVENT      7
#define SERIO_FIRMWARE_ID 8

/** A serio port's type, Linux's SERIO_8042_XL for the keyboard's
 * translated port, SERIO_8042 for the mouse's and SERIO_PS_PSTHRU for a
 * pass-through port. */
static uint32_t serio_type(int port)
{
    return port == 0 ? 0x06u : port == EVDEV_SERIO_PASS_THROUGH ? 0x05u : 0x01u;
}

/** Append a serio port's modalias, "serio:ty06pr00id00ex00", with its
 * type. */
static void emit_serio_alias(struct emit *e, int port)
{
    emit_str(e, "serio:ty");
    emit_hex(e, serio_type(port), 2, 1);
    emit_str(e, "pr00id00ex00");
}

/** A file of a serio port, as Linux's serio core writes it: its
 * description, modalias, bind mode, firmware ID, id/ files and uevent,
 * with the driver bound to it, atkbd or psmouse. The i8042's own uevent
 * with a `arg` of -1. */
static int32_t show_serio(void *obj, int arg, char *buf, uint32_t cap)
{
    int port, what;
    struct emit e;

    (void)obj;
    emit_init(&e, buf, cap);
    if (arg < 0) {
        emit_str(&e, "DRIVER=i8042\nMODALIAS=platform:i8042\n");
        return (int32_t)e.len;
    }
    port = arg >> 4;
    what = arg & 15;
    switch (what) {
    case SERIO_DESCRIPTION:
        emit_str(&e, port == 0                          ? "i8042 KBD port\n"
                     : port == EVDEV_SERIO_PASS_THROUGH ? "Synaptics pass-through\n"
                                                        : "i8042 AUX port\n");
        break;
    case SERIO_FIRMWARE_ID:
        emit_str(&e, serio_firmware[port] ? serio_firmware[port] : "");
        emit_str(&e, "\n");
        break;
    case SERIO_MODALIAS:
        emit_serio_alias(&e, port);
        emit_str(&e, "\n");
        break;
    case SERIO_BIND_MODE:
        emit_str(&e, "auto\n");
        break;
    case SERIO_UEVENT:
        emit_str(&e, port == 0 ? "DRIVER=atkbd\nSERIO_TYPE=" : "DRIVER=psmouse\nSERIO_TYPE=");
        emit_hex(&e, serio_type(port), 2, 0);
        emit_str(&e, "\nSERIO_PROTO=00\nSERIO_ID=00\nSERIO_EXTRA=00\nMODALIAS=");
        emit_serio_alias(&e, port);
        emit_str(&e, "\n");
        if (serio_firmware[port] && serio_firmware[port][0]) {
            emit_str(&e, "SERIO_FIRMWARE_ID=");
            emit_str(&e, serio_firmware[port]);
            emit_str(&e, "\n");
        }
        break;
    default:
        emit_hex(&e, what == SERIO_TYPE ? serio_type(port) : 0u, 2, 0);
        emit_str(&e, "\n");
        break;
    }
    return (int32_t)e.len;
}

/* The `arg` of a USB interface's files. */
#define IF_NUMBER    0
#define IF_ALTERNATE 1
#define IF_CLASS     2
#define IF_SUBCLASS  3
#define IF_PROTOCOL  4
#define IF_MODALIAS  5
#define IF_UEVENT    6

/** Append a USB interface's modalias, as Linux's usb_if_uevent writes it;
 * the device's class is 0, since the core keeps none and a HID device
 * gives its class in its interface. */
static void emit_usb_alias(struct emit *e, const struct usb_sys_row *u)
{
    emit_str(e, "usb:v");
    emit_hex(e, u->vendor, 4, 1);
    emit_str(e, "p");
    emit_hex(e, u->product, 4, 1);
    emit_str(e, "d");
    emit_hex(e, u->release, 4, 1);
    emit_str(e, "dc00dsc00dp00ic");
    emit_hex(e, (uint32_t)u->ifclass, 2, 1);
    emit_str(e, "isc");
    emit_hex(e, (uint32_t)u->ifsubclass, 2, 1);
    emit_str(e, "ip");
    emit_hex(e, (uint32_t)u->ifprotocol, 2, 1);
    emit_str(e, "in");
    emit_hex(e, (uint32_t)u->ifnum, 2, 1);
}

/** A file of a USB interface's directory, as Linux's USB core writes it:
 * its number, alternate setting, class, subclass and protocol, its
 * modalias and its uevent, with usbhid, Linux's HID driver, bound to it. */
static int32_t show_iface(void *obj, int arg, char *buf, uint32_t cap)
{
    const struct usb_sys_row *u = &((const struct iface_sys *)obj)->usb;
    struct emit e;

    emit_init(&e, buf, cap);
    switch (arg) {
    case IF_ALTERNATE:
        emit_str(&e, " 0\n");
        break;
    case IF_MODALIAS:
        emit_usb_alias(&e, u);
        emit_str(&e, "\n");
        break;
    case IF_UEVENT:
        emit_str(&e, "DEVTYPE=usb_interface\nDRIVER=usbhid\nPRODUCT=");
        emit_hex(&e, u->vendor, 0, 0);
        emit_str(&e, "/");
        emit_hex(&e, u->product, 0, 0);
        emit_str(&e, "/");
        emit_hex(&e, u->release, 0, 0);
        emit_str(&e, "\nTYPE=0/0/0\nINTERFACE=");
        emit_u32(&e, (uint32_t)u->ifclass);
        emit_str(&e, "/");
        emit_u32(&e, (uint32_t)u->ifsubclass);
        emit_str(&e, "/");
        emit_u32(&e, (uint32_t)u->ifprotocol);
        emit_str(&e, "\nMODALIAS=");
        emit_usb_alias(&e, u);
        emit_str(&e, "\n");
        break;
    default:
        emit_hex(&e,
                 (uint32_t)(arg == IF_NUMBER     ? u->ifnum
                            : arg == IF_CLASS    ? u->ifclass
                            : arg == IF_SUBCLASS ? u->ifsubclass
                                                 : u->ifprotocol),
                 2, 0);
        emit_str(&e, "\n");
        break;
    }
    return (int32_t)e.len;
}

/** Add each of the `n` attributes `attrs` below `dir`, with `obj` and the
 * argument `base` plus its place: 0, or the first error. */
static int add_attrs(int dir, const struct sysfs_attr *attrs, int n, void *obj, int base)
{
    int i, h;

    for (i = 0; i < n; i++) {
        h = sysfs_add_attr(dir, &attrs[i], obj, base + i);
        if (h < 0) {
            return h;
        }
    }
    return 0;
}

/** The directory `name` below `parent`, the one there or a new one: its
 * handle, or -errno. `*made` says which, when it is not NULL. */
static int dir_at(int parent, const char *name, int *made)
{
    int h;

    if (made) {
        *made = 0;
    }
    if (parent < 0) {
        return parent;
    }
    h = sysfs_node_child(parent, name, (int)k_strlen(name));
    if (h > 0 && sysfs_node_kind(h) == SYSFS_NODE_DIR) {
        return h;
    }
    h = sysfs_add_dir(parent, name);
    if (h > 0 && made) {
        *made = 1;
    }
    return h;
}

/** /sys/class/input, made on its first use. */
static int class_dir(void)
{
    if (sysfs_node_kind(parents.class_input) != SYSFS_NODE_DIR) {
        parents.class_input = dir_at(sysfs_anchor(SYSFS_CLASS, 0), "input", NULL);
    }
    return parents.class_input;
}

/** The i8042's directory under /sys/devices/platform, with its uevent,
 * its `subsystem` link and its link in /sys/bus/platform/devices, made
 * on its first use. */
static int i8042_dir(void)
{
    int made, h;

    if (sysfs_node_kind(parents.i8042) == SYSFS_NODE_DIR) {
        return parents.i8042;
    }
    h = dir_at(sysfs_anchor(SYSFS_PLATFORM, 0), "i8042", &made);
    if (h > 0 && made) {
        if (sysfs_add_uevent(h, show_serio, NULL, -1) < 0 ||
            sysfs_add_link(h, "subsystem", sysfs_anchor(SYSFS_BUS_PLATFORM, 0)) < 0) {
            sysfs_remove(h);
            return -ENOMEM;
        }
        parents.i8042_link = sysfs_add_link(sysfs_anchor(SYSFS_PLATFORM_LINKS, 0), "i8042", h);
        sysfs_uevent(h, "add", NULL);
    }
    parents.i8042 = h;
    return h;
}

/** /sys/bus/serio and its `devices`, made on their first use: `devices`,
 * or -errno. */
static int serio_bus_devices(void)
{
    if (sysfs_node_kind(parents.serio_devices) == SYSFS_NODE_DIR) {
        return parents.serio_devices;
    }
    parents.bus_serio = dir_at(sysfs_anchor(SYSFS_BUS, 0), "serio", NULL);
    (void)dir_at(parents.bus_serio, "drivers", NULL);
    parents.serio_devices = dir_at(parents.bus_serio, "devices", NULL);
    return parents.serio_devices;
}

/**
 * Have the protocol's function of the serio port `port` add its files to the
 * port's directory `dir`, and keep the entries it made, found as those that
 * were not below `dir` before, so that a change of protocol can remove them.
 * 0, or the function's -errno, with what it made removed.
 */
static int add_serio_files(int port, int dir)
{
    int before[SERIO_FILES_MAX * 3], nbefore, n, h, i, j, made = 0, rc;

    k_memset(serio_made[port], 0, sizeof(serio_made[port]));
    if (!serio_files[port]) {
        return 0;
    }
    nbefore = sysfs_node_count(dir);
    if (nbefore > (int)(sizeof(before) / sizeof(before[0]))) {
        return -ENOSPC;
    }
    for (i = 0; i < nbefore; i++) {
        before[i] = sysfs_node_nth(dir, i);
    }
    rc = serio_files[port](dir);
    n = sysfs_node_count(dir);
    for (i = 0; i < n; i++) {
        h = sysfs_node_nth(dir, i);
        for (j = 0; j < nbefore && before[j] != h; j++) {
        }
        if (j < nbefore) {
            continue;
        }
        if (made < SERIO_FILES_MAX) {
            serio_made[port][made++] = h;
        } else {
            sysfs_remove(h);
            i--;
            n--;
            rc = -ENOSPC; /* the table is sized for the largest protocol's */
        }
    }
    if (rc < 0) {
        for (i = 0; i < made; i++) {
            sysfs_remove(serio_made[port][i]);
        }
        k_memset(serio_made[port], 0, sizeof(serio_made[port]));
    }
    return rc;
}

/** The serio port `port`, serioK below the i8042, or below serio1 for the
 * pass-through port, with its files and links and its driver's and
 * protocol's, made on its first use. */
static int serio_dir(int port)
{
    static const struct sysfs_attr attrs[] = {{"description", 0444u, show_serio, NULL},
                                              {"modalias", 0444u, show_serio, NULL},
                                              {"bind_mode", 0444u, show_serio, NULL}};
    static const struct sysfs_attr firmware = {"firmware_id", 0444u, show_serio, NULL};
    static const struct sysfs_attr ids[] = {{"type", 0444u, show_serio, NULL},
                                            {"proto", 0444u, show_serio, NULL},
                                            {"id", 0444u, show_serio, NULL},
                                            {"extra", 0444u, show_serio, NULL}};
    int bus = serio_bus_devices(), made = 0, h, id;
    char name[16];

    if (sysfs_node_kind(parents.serio[port]) == SYSFS_NODE_DIR) {
        return parents.serio[port];
    }
    name_num(name, sizeof(name), "serio", (uint32_t)port);
    h = bus < 0
            ? bus
            : dir_at(port == EVDEV_SERIO_PASS_THROUGH ? serio_dir(1) : i8042_dir(), name, &made);
    if (h > 0 && made) {
        id = sysfs_add_dir(h, "id");
        if (add_attrs(h, attrs, 3, NULL, port * 16 + SERIO_DESCRIPTION) < 0 ||
            sysfs_add_attr(h, &firmware, NULL, port * 16 + SERIO_FIRMWARE_ID) < 0 || id < 0 ||
            add_attrs(id, ids, 4, NULL, port * 16 + SERIO_TYPE) < 0 ||
            sysfs_add_uevent(h, show_serio, NULL, port * 16 + SERIO_UEVENT) < 0 ||
            sysfs_add_link(h, "subsystem", parents.bus_serio) < 0 ||
            (serio_driver[port] && serio_driver[port](h) < 0) || add_serio_files(port, h) < 0) {
            sysfs_remove(h);
            return -ENOMEM;
        }
        parents.serio_link[port] = sysfs_add_link(bus, name, h);
        if (parents.serio_link[port] < 0) {
            sysfs_remove(h);
            parents.serio_link[port] = 0;
            return -ENOMEM;
        }
        sysfs_uevent(h, "add", NULL);
    }
    parents.serio[port] = h;
    return h;
}

/** The directory of interface number `ifnum` of the stack's entry `index`,
 * B-P:C.I below its device, made when absent, with its files, its
 * `subsystem` link and its link in /sys/bus/usb/devices, and counted as
 * `r`'s: its handle, or -errno, -ENOENT for an entry that is not running
 * or does not drive that interface. */
static int iface_dir(struct input_sys *r, int index, int ifnum)
{
    static const struct sysfs_attr attrs[] = {{"bInterfaceNumber", 0444u, show_iface, NULL},
                                              {"bAlternateSetting", 0444u, show_iface, NULL},
                                              {"bInterfaceClass", 0444u, show_iface, NULL},
                                              {"bInterfaceSubClass", 0444u, show_iface, NULL},
                                              {"bInterfaceProtocol", 0444u, show_iface, NULL},
                                              {"modalias", 0444u, show_iface, NULL}};
    char dev[SYSFS_NAME_MAX], name[SYSFS_NAME_MAX];
    struct usb_sys_row row;
    struct iface_sys *f = NULL;
    struct emit e;
    int key, made = 0, h, i, rc;

    if (usb_core_interface(index, ifnum, &row) != 0 || (key = sysfs_usb_key(&row)) < 0) {
        return -ENOENT;
    }
    synth_sys_device_name(SYSFS_USB_DEVICE, key, dev, (int)sizeof(dev));
    emit_init(&e, name, sizeof(name) - 1);
    emit_str(&e, dev);
    emit_str(&e, ":");
    emit_u32(&e, (uint32_t)row.config & 0xFFu);
    emit_str(&e, ".");
    emit_u32(&e, (uint32_t)row.ifnum & 0xFFu);
    name[e.len < sizeof(name) - 1 ? e.len : sizeof(name) - 1] = '\0';
    h = dir_at(sysfs_anchor(SYSFS_USB_DEVICE, key), name, &made);
    if (h <= 0) {
        return h;
    }
    for (i = 0; i < EVDEV_MAX && !f; i++) {
        if (made ? ifaces[i].dir == 0 : ifaces[i].dir == h) {
            f = &ifaces[i];
        }
    }
    if (!f) {
        if (made) {
            sysfs_remove(h);
        }
        return -ENOMEM;
    }
    if (made) {
        k_memset(f, 0, sizeof(*f));
        f->usb = row;
        if ((rc = add_attrs(h, attrs, 6, f, IF_NUMBER)) < 0 ||
            (rc = sysfs_add_uevent(h, show_iface, f, IF_UEVENT)) < 0 ||
            (rc = sysfs_add_link(h, "subsystem", sysfs_anchor(SYSFS_BUS_USB, 0))) < 0) {
            sysfs_remove(h);
            return rc;
        }
        f->dir = h;
        h = sysfs_add_link(sysfs_anchor(SYSFS_USB_LINKS, 0), name, f->dir);
        f->link = h > 0 ? h : 0;
        sysfs_uevent(f->dir, "add", NULL);
    }
    f->users++;
    r->iface = f;
    return f->dir;
}

/** The decimal number at `s` up to a character that is no digit, or -1
 * for none. */
static int number_at(const char *s)
{
    int n = 0, i;

    for (i = 0; s[i] >= '0' && s[i] <= '9' && i < 4; i++) {
        n = n * 10 + (s[i] - '0');
    }
    return i > 0 ? n : -1;
}

/** Whether `s` begins with `prefix`. */
static int starts(const char *s, const char *prefix)
{
    while (*prefix && *s == *prefix) {
        s++;
        prefix++;
    }
    return *prefix == '\0';
}

/** The parent of the device `r` by its EVIOCGPHYS: an i8042 port's
 * directory or a USB interface's, its handle; 0 for a device with no
 * parent here; or -errno. */
static int parent_of(struct input_sys *r)
{
    const char *phys = r->d.phys;
    int n;

    if (starts(phys, "isa0060/serio")) {
        n = number_at(phys + 13);
        return n >= 0 && n < SERIO_I8042_PORTS ? serio_dir(n) : 0;
    }
    if (starts(phys, "synaptics-pt/serio0/")) {
        return serio_dir(EVDEV_SERIO_PASS_THROUGH);
    }
    if (starts(phys, "usb-")) {
        n = number_at(phys + 4);
        for (phys += 4; *phys >= '0' && *phys <= '9'; phys++) {
        }
        if (n < 1 || !starts(phys, "/input") || number_at(phys + 6) < 0) {
            return 0;
        }
        return iface_dir(r, n - 1, number_at(phys + 6));
    }
    return 0;
}

/** Let go of the USB interface `r` is below; the last device to let go
 * takes it away, sending its "remove". */
static void drop_iface(struct input_sys *r)
{
    struct iface_sys *f = r->iface;

    r->iface = NULL;
    if (f && --f->users == 0) {
        sysfs_uevent(f->dir, "remove", NULL);
        sysfs_remove(f->link);
        sysfs_remove(f->dir);
        k_memset(f, 0, sizeof(*f));
    }
}

/**
 * Make inputN of `r` below `parent`, or below /sys/devices/virtual/input
 * for a `parent` of 0, with its attributes, its id/ and capabilities/,
 * its uevent and its links: 0, or -errno with nothing of it left.
 */
static int make_input(struct input_sys *r, int parent)
{
    static const struct sysfs_attr attrs[] = {{"name", 0444u, show_input, NULL},
                                              {"phys", 0444u, show_input, NULL},
                                              {"uniq", 0444u, show_input, NULL},
                                              {"modalias", 0444u, show_input, NULL},
                                              {"properties", 0444u, show_input, NULL}};
    static const struct sysfs_attr ids[] = {{"bustype", 0444u, show_id, NULL},
                                            {"vendor", 0444u, show_id, NULL},
                                            {"product", 0444u, show_id, NULL},
                                            {"version", 0444u, show_id, NULL}};
    struct sysfs_attr caps[CAP_TYPES];
    char name[16];
    int i, h, id, cap;

    if (parent > 0) {
        r->holder = dir_at(parent, "input", NULL);
    } else {
        parents.virtual_input =
            dir_at(dir_at(sysfs_anchor(SYSFS_DEVICES, 0), "virtual", NULL), "input", NULL);
        r->holder = parents.virtual_input;
    }
    name_num(name, sizeof(name), "input", (uint32_t)r->number);
    r->input = sysfs_add_dir(r->holder, name);
    if (r->input < 0) {
        return r->input;
    }
    for (i = 0; i < CAP_TYPES; i++) {
        caps[i].name = cap_types[i].name;
        caps[i].mode = 0444u;
        caps[i].show = show_cap;
        caps[i].store = NULL;
    }
    id = sysfs_add_dir(r->input, "id");
    cap = sysfs_add_dir(r->input, "capabilities");
    h = add_attrs(r->input, attrs, 5, r, IN_NAME);
    if (h >= 0) {
        h = id < 0 ? id : add_attrs(id, ids, 4, r, 0);
    }
    if (h >= 0) {
        h = cap < 0 ? cap : add_attrs(cap, caps, CAP_TYPES, r, 0);
    }
    if (h >= 0) {
        h = sysfs_add_uevent(r->input, show_input, r, IN_UEVENT);
    }
    if (h >= 0) {
        h = sysfs_add_link(r->input, "subsystem", class_dir());
    }
    if (h >= 0 && parent > 0) {
        h = sysfs_add_link(r->input, "device", parent);
    }
    if (h < 0) {
        sysfs_remove(r->input);
        r->input = 0;
        return h;
    }
    return 0;
}

/** Make eventN of `r` below its inputN, with its `dev`, uevent and
 * links: 0, or -errno with nothing of it left. */
static int make_event(struct input_sys *r)
{
    static const struct sysfs_attr dev_attr = {"dev", 0444u, show_event, NULL};
    char name[16];
    int h;

    name_num(name, sizeof(name), "event", (uint32_t)r->slot);
    r->event = sysfs_add_dir(r->input, name);
    if (r->event < 0) {
        return r->event;
    }
    if ((h = sysfs_add_attr(r->event, &dev_attr, r, 0)) < 0 ||
        (h = sysfs_add_uevent(r->event, show_event, r, 1)) < 0 ||
        (h = sysfs_add_link(r->event, "subsystem", class_dir())) < 0 ||
        (h = sysfs_add_link(r->event, "device", r->input)) < 0) {
        sysfs_remove(r->event);
        r->event = 0;
        return h;
    }
    return 0;
}

/** Take away the entries of `r` and, when it is left empty, the `input`
 * directory they were in, without a uevent. */
static void unmake(struct input_sys *r)
{
    sysfs_remove(r->class_event);
    sysfs_remove(r->class_input);
    sysfs_remove(r->char_link);
    sysfs_remove(r->input);
    if (r->holder > 0 && r->holder != parents.virtual_input && sysfs_node_count(r->holder) == 0) {
        sysfs_remove(r->holder);
    }
    drop_iface(r);
}

/** Publish the device `d` of evdev's slot `slot` in `r`: make its entries
 * and send "add" for inputN and then eventN. 0, or -errno with nothing
 * made but fixed parents. */
static int publish(struct input_sys *r, int slot, const struct evdev_desc *d)
{
    char name[16];
    int parent, h, class_input = class_dir();

    k_memset(r, 0, sizeof(*r));
    r->slot = slot;
    r->d = *d;
    r->number = (int)next_number;
    parent = class_input < 0 ? class_input : parent_of(r);
    h = parent < 0 ? parent : make_input(r, parent);
    if (h >= 0) {
        h = make_event(r);
    }
    if (h >= 0) {
        name_num(name, sizeof(name), "input", (uint32_t)r->number);
        r->class_input = h = sysfs_add_link(class_input, name, r->input);
    }
    if (h >= 0) {
        name_num(name, sizeof(name), "event", (uint32_t)slot);
        r->class_event = h = sysfs_add_link(class_input, name, r->event);
    }
    if (h >= 0) {
        name_num(name, sizeof(name), "13:", EVDEV_MINOR_BASE + (uint32_t)slot);
        r->char_link = h = sysfs_add_link(sysfs_anchor(SYSFS_DEV_CHAR, 0), name, r->event);
    }
    if (h < 0) {
        unmake(r);
        k_memset(r, 0, sizeof(*r));
        return h;
    }
    r->gen = d->gen;
    next_number++;
    sysfs_uevent(r->input, "add", NULL);
    sysfs_uevent(r->event, "add", NULL);
    return 0;
}

/** Withdraw the device of `r`: send "remove" for eventN and then inputN,
 * and take its entries away. */
static void withdraw(struct input_sys *r)
{
    uint32_t failed = r->failed;

    sysfs_uevent(r->event, "remove", NULL);
    sysfs_uevent(r->input, "remove", NULL);
    unmake(r);
    k_memset(r, 0, sizeof(*r));
    r->failed = failed;
}

void evdev_sysfs_serio_firmware_id(int port, const char *id)
{
    if (port >= 0 && port < SERIO_PORTS) {
        serio_firmware[port] = id;
    }
}

void evdev_sysfs_serio_remove(int port)
{
    int n, dir;

    if (port < 0 || port >= SERIO_PORTS) {
        return;
    }
    if (port == 1) {
        evdev_sysfs_serio_remove(EVDEV_SERIO_PASS_THROUGH);
    }
    dir = parents.serio[port];
    if (sysfs_node_kind(dir) == SYSFS_NODE_DIR) {
        for (n = 0; n < EVDEV_MAX; n++) {
            if (records[n].gen && sysfs_node_parent(records[n].holder) == dir) {
                withdraw(&records[n]);
            }
        }
        sysfs_uevent(dir, "remove", NULL);
        sysfs_remove(parents.serio_link[port]);
        sysfs_remove(dir);
    }
    parents.serio[port] = parents.serio_link[port] = 0;
    serio_firmware[port] = NULL;
    serio_driver[port] = NULL;
    serio_files[port] = NULL;
    k_memset(serio_made[port], 0, sizeof(serio_made[port]));
}

void evdev_sysfs_serio_driver(int port, int (*add)(int dir))
{
    if (port >= 0 && port < SERIO_PORTS) {
        serio_driver[port] = add;
    }
}

void evdev_sysfs_serio_files(int port, int (*add)(int dir))
{
    int i;

    if (port < 0 || port >= SERIO_PORTS) {
        return;
    }
    serio_files[port] = add;
    if (sysfs_node_kind(parents.serio[port]) != SYSFS_NODE_DIR) {
        return; /* the directory, when made, takes them */
    }
    for (i = 0; i < SERIO_FILES_MAX && serio_made[port][i]; i++) {
        sysfs_remove(serio_made[port][i]);
    }
    if (add_serio_files(port, parents.serio[port]) < 0) {
        klog(KLOG_WARN, "input", "serio%d: the protocol's files were not made", port);
    }
}

void evdev_sysfs_sync(void)
{
    struct evdev_desc d;
    uint8_t tried[EVDEV_MAX];
    uint32_t best, failed;
    int n, next, rc;

    (void)class_dir();
    for (n = 0; n < EVDEV_MAX; n++) {
        if (!records[n].gen) {
            continue;
        }
        if (!evdev_snapshot(n, &d) || d.gen != records[n].gen) {
            withdraw(&records[n]);
        } else {
            records[n].d = d; /* EVIOCSKEYCODE changes the keys a device has */
        }
    }
    /* The new devices in the order they registered, as Linux numbers them. */
    k_memset(tried, 0, sizeof(tried));
    for (;;) {
        next = -1;
        best = 0;
        for (n = 0; n < EVDEV_MAX; n++) {
            if (records[n].gen || tried[n] || !evdev_snapshot(n, &d)) {
                continue;
            }
            if (next < 0 || d.gen < best) {
                next = n;
                best = d.gen;
            }
        }
        if (next < 0 || !evdev_snapshot(next, &d)) {
            break;
        }
        tried[next] = 1;
        failed = records[next].failed; /* publish clears the record */
        rc = publish(&records[next], next, &d);
        if (rc < 0 && failed != d.gen) {
            klog(KLOG_WARN, "input", "sysfs: event%d: refused: error %d", next, (int)rc);
        }
        records[next].failed = rc < 0 ? d.gen : 0;
    }
}

#ifndef RACH2_HOSTTEST
static process_t *thread;

/** Whether evdev's count of changes is no longer the one in `*arg`. */
static int changed(void *arg)
{
    return evdev_changes() != *(const uint32_t *)arg;
}

/** The worker, kinputd: sync /sys with the devices, then sleep in an idle
 * wait until a device comes or goes, holding the big lock as kernel code
 * does. The count is read before each sync, so a change during one brings
 * another. */
static void sync_thread(void)
{
    uint32_t seen;

    arch_irq_enable();
    for (;;) {
        seen = evdev_changes();
        evdev_sysfs_sync();
        (void)wq_wait_event_idle(evdev_change_queue(), changed, &seen);
    }
}

void evdev_sysfs_start(void)
{
    process_t *p;
    uintptr_t base;
    uint32_t flags;

    if (thread) {
        return;
    }
    evdev_sysfs_sync();
    flags = kirq_save();
    base = kstack_alloc();
    p = base ? alloc_process() : NULL;
    if (!p) {
        if (base) {
            kstack_free(base + KERNEL_STACK_SIZE);
        }
        kirq_restore(flags);
        klog(KLOG_ERR, "input", "sysfs: worker: refused: no record or stack for it");
        return;
    }
    p->kernel_stack = base + KERNEL_STACK_SIZE;
    p->space_root = 0;
    p->page_dir = NULL;
    p->parent_pid = 0;
    proc_set_comm(p, "kinputd");
    setup_process_stack(p, sync_thread);
    thread = p;
    sched_make_ready(p);
    kirq_restore(flags);
    klog(KLOG_DEBUG, "input", "sysfs: worker: kinputd is pid %d", (int)p->pid);
}
#endif
