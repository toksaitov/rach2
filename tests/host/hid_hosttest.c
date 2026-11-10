/*
 * hid_hosttest.c: the USB HID class driver, usb/hid.c, fed keyboard and mouse
 * reports directly, with no controller and no keyboard.
 *
 * host-deps: rach2/sys/lib/kstring.c rach2/sys/lib/kctypes.c
 *
 * A report is a state and a scancode is an event. This class driver turns one
 * into the other and does nothing else. The interesting cases are the ones
 * a person produces without noticing: a key held down across reports, two keys
 * down at once, a key released while another stays down, a keyboard unplugged
 * with a key still held, and the report a keyboard sends when it has lost
 * count of its own keys.
 *
 * None of those can be produced from outside a machine. They are produced here
 * by handing the driver reports and reading what it gave the input layer.
 * The model's `set1_keycode` gives each scancode a key code of its own, the
 * scancode plus 0x100, so that the input layer's model can record each key
 * event as the set-1 bytes of its key, as the raw view gives them, and keep
 * each keyboard's held keys as the real one does.
 *
 * A device in the report protocol is driven through a model of the parser
 * and the usage mapping, which hid_parse_hosttest.c tests; the rows here
 * check what the driver itself decides: its names, which protocol a device
 * runs in, where its reports go and the order and retries of its output
 * reports.
 */

extern int printf(const char *fmt, ...);

#include "../../rach2/sys/usb/hid.h"

/* ---- the functions outside hid.c that it calls ---- */

#include "../../rach2/sys/input.h"
#include "../../rach2/sys/mouse.h"

/* Every code the driver sent since the last report, in order; codes past
 * MAX_CODES are dropped. The MSC_SCAN of the last key event. */
#define MAX_CODES 64
static unsigned char codes[MAX_CODES];
static int n_codes;
static uint32_t last_scan;

/* The model's key codes: the scancode plus KEY_BASE. */
#define KEY_BASE 0x100u

/** Do nothing: hid.c stamps each code for the latency record, which this test
 * does not check. */
void usb_stamp_wake(void)
{}

/** Record a code in `codes`. */
static void record(unsigned char code)
{
    if (n_codes < MAX_CODES) {
        codes[n_codes] = code;
        n_codes++;
    }
}

/**
 * Take a key event as the input layer does: drop a press of a key `kb`
 * holds and a release of one it does not, keep the held keys, and record
 * the event as its set-1 bytes, E0 before an extended key.
 */
void input_key_event(struct input_keyboard *kb, uint32_t scan, unsigned int keycode, int down)
{
    unsigned code = keycode - KEY_BASE;
    uint8_t bit = (uint8_t)(1u << (keycode & 7));

    last_scan = scan;
    if (keycode == 0) {
        return;
    }
    if (down ? (kb->down[keycode >> 3] & bit) != 0 : (kb->down[keycode >> 3] & bit) == 0) {
        return;
    }
    if (down) {
        kb->down[keycode >> 3] |= bit;
    } else {
        kb->down[keycode >> 3] &= (uint8_t)~bit;
    }
    if (code & 0x80u) {
        record(0xE0);
    }
    record((unsigned char)((code & 0x7Fu) | (down ? 0u : 0x80u)));
}

/** Release every key `kb` holds, from the lowest key code up, as the input
 * layer does when a keyboard goes. */
void input_keyboard_detach(struct input_keyboard *kb)
{
    unsigned code;

    for (code = 1; code < INPUT_KEY_BYTES * 8; code++) {
        if (kb->down[code >> 3] & (1u << (code & 7))) {
            input_key_event(kb, 0, code, 0);
        }
    }
    kb->dev = 0;
}

/* The mouse's event device: what registered it, and whether it was taken
 * away; its pointer is `mouse_registrations`, never followed. */
static int mouse_registrations;
static int mouse_unregistrations;
static char mouse_name[32];
static char mouse_phys[24];
static uint16_t mouse_bus;
static uint16_t mouse_vendor;
static uint16_t mouse_product;
static unsigned int mouse_caps;

/* The last mouse packet, its device, and the count of them. */
static struct evdev *mouse_dev;
static int mouse_dx;
static int mouse_dy;
static int mouse_wheel;
static int mouse_hwheel;
static unsigned int mouse_buttons;
static int mouse_reports;

/** Copy the string `from` into `to`, of `size` bytes. */
static void copy_string(char *to, const char *from, unsigned size)
{
    unsigned i;

    for (i = 0; i + 1 < size && from[i]; i++) {
        to[i] = from[i];
    }
    to[i] = 0;
}

/** Record the mouse's registration. */
struct evdev *mouse_register(const char *name, const char *phys, uint16_t bustype, uint16_t vendor,
                             uint16_t product, unsigned int caps)
{
    mouse_registrations++;
    copy_string(mouse_name, name, sizeof(mouse_name));
    copy_string(mouse_phys, phys, sizeof(mouse_phys));
    mouse_bus = bustype;
    mouse_vendor = vendor;
    mouse_product = product;
    mouse_caps = caps;
    return (struct evdev *)&mouse_registrations;
}

/** Record the last mouse packet, and count the reports. */
void mouse_report(struct evdev *dev, unsigned int buttons, int dx, int dy, int wheel, int hwheel)
{
    mouse_dev = dev;
    mouse_dx = dx;
    mouse_dy = dy;
    mouse_wheel = wheel;
    mouse_hwheel = hwheel;
    mouse_buttons = buttons;
    mouse_reports++;
}

/** Count the taking away of the mouse's event device; a NULL one, a
 * keyboard's slot, is ignored as evdev.c ignores it. */
void evdev_unregister(struct evdev *dev)
{
    if (dev) {
        mouse_unregistrations++;
    }
}

/** Compare two strings. */
static int same(const char *a, const char *b)
{
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

/** Return 1: a mouse is present. */
int input_mouse_present(void)
{
    return 1;
}

/** Do nothing: no row checks the announcement of a mouse. */
void input_mouse_found(void)
{}

/* The name of the last keyboard attached. */
static char keyboard_name[48];

/** Register no event device, keeping the name: the rows check the key
 * events alone. */
void input_keyboard_attach(struct input_keyboard *kb, const char *name, const char *phys,
                           uint16_t bustype, uint16_t vendor, uint16_t product)
{
    (void)kb;
    (void)phys;
    (void)bustype;
    (void)vendor;
    (void)product;
    copy_string(keyboard_name, name, sizeof(keyboard_name));
}

/** The model's key code of `scan`: the scancode plus KEY_BASE. */
int set1_keycode(unsigned scan)
{
    return (int)(scan + KEY_BASE);
}

#include "../../rach2/sys/klog.h"
#include "../../rach2/sys/lib/kerrno.h"
#include "../../rach2/sys/usb/hid_input.h"

extern void *calloc(unsigned long n, unsigned long size);
extern void *malloc(unsigned long size);
extern void free(void *p);

/** Allocate from the host's heap. */
void *kmalloc(size_t size)
{
    return malloc(size);
}

/** Allocate zeroed memory from the host's heap. */
void *kzalloc(size_t size)
{
    return calloc(1, size);
}

/** Free what `kmalloc` or `kzalloc` gave. */
void kfree(void *p)
{
    free(p);
}

/* The model of the parser and the mapping: whether a descriptor of no
 * bytes is refused, what `hidinput_connect` answers, whether the device
 * has LEDs and a multiplier, and what the driver gave them. */
static int model_inputs = 1;
static int model_leds = 1;
static int model_multiplier;
static int model_reports;
static uint32_t model_report_size;
static int model_disconnects;
static int model_multiplier_set = -1;
static char model_name[HID_NAME_MAX];
static char model_phys[24];
static char model_uniq[HID_UNIQ_MAX];
static uint16_t model_version;

/* The bytes of the model's LED report. */
static int model_led_len = 1;

/** Refuse a descriptor of no bytes; take any other as one input report of
 * 8 bytes and an output report of `model_led_len`, a keyboard's LEDs. */
int hid_parse(struct hid_desc *desc, const uint8_t *rdesc, int length)
{
    (void)rdesc;
    if (length <= 0) {
        return -EINVAL;
    }
    desc->max_fields = 1;
    desc->max_usages = 1;
    desc->max_collections = 1;
    desc->nreports = 2;
    desc->reports[0].type = HID_INPUT_REPORT;
    desc->reports[0].id = 0;
    desc->reports[0].size = 64;
    desc->reports[1].type = HID_OUTPUT_REPORT;
    desc->reports[1].id = 0;
    desc->reports[1].size = (uint32_t)model_led_len * 8;
    return 0;
}

/** The bytes of `r`, with its ID. */
uint32_t hid_report_len(const struct hid_report *r)
{
    return (r->size + 7) / 8 + (r->id ? 1u : 0u);
}

/** Answer `model_inputs`, keeping the name and phys the driver gave. */
int hidinput_connect(struct hid_dev *hid)
{
    copy_string(model_name, hid->name, sizeof(model_name));
    copy_string(model_phys, hid->phys, sizeof(model_phys));
    copy_string(model_uniq, hid->uniq, sizeof(model_uniq));
    model_version = hid->version;
    hid->ninputs = model_inputs > 0 ? (uint32_t)model_inputs : 0;
    return model_inputs;
}

/** Count a disconnect. */
void hidinput_disconnect(struct hid_dev *hid)
{
    (void)hid;
    model_disconnects++;
}

/** Count a report and keep its length. */
void hid_input_report(struct hid_dev *hid, const uint8_t *data, uint32_t size)
{
    (void)hid;
    (void)data;
    model_reports++;
    model_report_size = size;
}

/** The LED report: one byte of the LED_* bits, wValue 0x0200, or -1
 * without `model_leds`. */
int hidinput_led_report(struct hid_dev *hid, unsigned int leds, uint8_t *out, int size, int *value)
{
    int i;

    (void)hid;
    if (!model_leds || size < model_led_len) {
        return -1;
    }
    for (i = 0; i < model_led_len; i++) {
        out[i] = (uint8_t)leds;
    }
    *value = 0x0200;
    return model_led_len;
}

/* The LEDs beyond the first three that programs lit, a bit each. */
static unsigned int model_lit;

/** Answer `model_lit`. */
unsigned int hidinput_leds_lit(struct hid_dev *hid)
{
    (void)hid;
    return model_lit;
}

/** The multiplier report, Feature report 2 at 1, or -1 without
 * `model_multiplier`. */
int hidinput_multiplier_report(struct hid_dev *hid, uint8_t *out, int size, int *value)
{
    (void)hid;
    if (!model_multiplier || size < 2) {
        return -1;
    }
    out[0] = 2;
    out[1] = 1;
    *value = 0x0302;
    return 2;
}

/** Keep whether the multiplier was taken. */
void hidinput_multiplier_set(struct hid_dev *hid, int ok)
{
    (void)hid;
    model_multiplier_set = ok;
}

/* The LEDs the input layer wants, LED_* of kd.h, and the warnings logged. */
static unsigned int wanted_leds;
static int warnings;

/** Return `wanted_leds`. */
unsigned int input_leds(void)
{
    return wanted_leds;
}

/** Count a warning; the text is not checked. */
void klog(int level, const char *subsystem, const char *format, ...)
{
    (void)subsystem;
    (void)format;
    if (level == KLOG_WARN) {
        warnings++;
    }
}

#include "../../rach2/sys/usb/hid.c"

/* ---- the rows ---- */

static int fails;

#define CHECK(what, cond)                 \
    do {                                  \
        if (!(cond)) {                    \
            printf("  FAIL: %s\n", what); \
            fails++;                      \
        }                                 \
    } while (0)

/**
 * Send keyboard `slot` an 8-byte boot report with modifier byte `mods` and the
 * key usages `a`, `b` and `c` (0 for none) in the first three key slots,
 * after clearing `codes`.
 */
static void keys(int slot, unsigned mods, int a, int b, int c)
{
    unsigned char report[8];

    report[0] = (unsigned char)mods;
    report[1] = 0;
    report[2] = (unsigned char)a;
    report[3] = (unsigned char)b;
    report[4] = (unsigned char)c;
    report[5] = 0;
    report[6] = 0;
    report[7] = 0;
    n_codes = 0;
    hid_report(slot, report, 8);
}

/** Return 1 when the driver sent exactly the first `n` of `c0` to `c3`. */
static int sent(int n, int c0, int c1, int c2, int c3)
{
    int want[4];
    int i;

    want[0] = c0;
    want[1] = c1;
    want[2] = c2;
    want[3] = c3;
    if (n_codes != n) {
        return 0;
    }
    for (i = 0; i < n; i++) {
        if (codes[i] != want[i]) {
            return 0;
        }
    }
    return 1;
}

/* QEMU's identity, with and without its strings. */
static const struct hid_ids no_strings = {0x0627, 0x0001, 0x0001, "", "", ""};
static const struct hid_ids qemu_mouse = {0x0627, 0x0001, 0x0001, "QEMU", "QEMU USB Mouse", ""};
static const struct hid_ids qemu_tablet = {
    0x0627, 0x0001, 0x0001, "QEMU", "QEMU USB Tablet", "28754-0000:00:04.0-1"};
/* A serial of 62 letters and a two-byte character, which 63 bytes cannot
 * hold whole. */
static const struct hid_ids long_serial = {
    0x046d,         0xc52b,
    0x0111,         "Logitech",
    "USB Receiver", "ABCDEFGHIJABCDEFGHIJABCDEFGHIJABCDEFGHIJABCDEFGHIJABCDEFGHIJAB\xc3\xa9Z"};

/** The byte of the LED report `hid_output_report` offers `device`, with
 * its wValue in `value`; -1 for none. */
static int led_offer(int device, int *value)
{
    uint8_t out[8];
    int n = hid_output_report(device, out, sizeof(out), value);

    return n == 1 ? out[0] : -1;
}

/**
 * Check the LED report: each lock at its bit of the boot keyboard's output
 * report, offered once for a keyboard just attached and again only when
 * the LEDs wanted change, never after a failure until the keyboard is
 * attached again, and never for a mouse or a device not attached.
 */
static void leds_rows(void)
{
    uint8_t out[8];
    int value = 0;

    hid_attach_boot(5, HID_KEYBOARD, &no_strings);
    wanted_leds = 0;
    CHECK("leds: a keyboard just attached is offered its LEDs",
          led_offer(5, &value) == 0x00 && value == 0x0200);
    hid_output_sent(5, 1);
    CHECK("leds: and not again while they stay", led_offer(5, &value) == -1);
    wanted_leds = LED_NUM;
    CHECK("leds: Num Lock is bit 0", led_offer(5, &value) == 0x01);
    wanted_leds = LED_CAP;
    CHECK("leds: Caps Lock is bit 1", led_offer(5, &value) == 0x02);
    wanted_leds = LED_SCR;
    CHECK("leds: Scroll Lock is bit 2", led_offer(5, &value) == 0x04);
    wanted_leds = LED_SCR | LED_NUM | LED_CAP;
    CHECK("leds: all three", led_offer(5, &value) == 0x07);
    hid_output_sent(5, 1);
    CHECK("leds: taken", led_offer(5, &value) == -1);

    wanted_leds = LED_CAP;
    warnings = 0;
    CHECK("leds: Caps Lock offered", led_offer(5, &value) == 0x02);
    hid_output_sent(5, 0);
    CHECK("leds: a failure is logged", warnings == 1);
    CHECK("leds: nothing is offered after a failure", led_offer(5, &value) == -1);
    hid_output_sent(5, 0);
    CHECK("leds: and logged once", warnings == 1);
    hid_gone(5);
    CHECK("leds: nothing to a keyboard gone", led_offer(5, &value) == -1);
    hid_attach_boot(5, HID_KEYBOARD, &no_strings);
    CHECK("leds: a keyboard attached again is offered them", led_offer(5, &value) == 0x02);
    CHECK("leds: not in a buffer too small", hid_output_report(5, out, 0, &value) == -1);
    hid_gone(5);

    hid_attach_boot(6, HID_MOUSE, &no_strings);
    CHECK("leds: nothing to a mouse", led_offer(6, &value) == -1);
    hid_gone(6);
    CHECK("leds: nothing to a device never attached", led_offer(7, &value) == -1);
    CHECK("leds: nothing to a device out of range",
          led_offer(0, &value) == -1 && led_offer(USB_MAX_DEVICES + 1, &value) == -1);
}

/**
 * A device in the report protocol: its name and phys as usbhid gives them,
 * its reports passed whole to the input mapping, its multiplier report
 * before its LEDs and each tried once, and a descriptor refused or with no
 * input device leaving the device free for the boot protocol.
 */
static void report_rows(void)
{
    static const uint8_t rdesc[4] = {1, 2, 3, 4};
    uint8_t out[8], out20[32], report[8] = {0};
    int value = 0;

    model_multiplier = 0;
    CHECK("report: a tablet is taken", hid_attach(8, &qemu_tablet, rdesc, 4) == 0);
    CHECK("report: named by its strings, usb-8/input0, version bcdHID",
          same(model_name, "QEMU QEMU USB Tablet") && same(model_phys, "usb-8/input0") &&
              model_version == 1);
    CHECK("report: its serial string is its uniq", same(model_uniq, "28754-0000:00:04.0-1"));
    CHECK("report: taken once",
          hid_attach(8, &qemu_tablet, rdesc, 4) == 0 && hid_report_size(8) == 8);
    model_reports = 0;
    hid_report(8, report, 3);
    CHECK("report: a report goes to the mapping as it came",
          model_reports == 1 && model_report_size == 3);
    model_leds = 0;
    CHECK("report: no LED report from a device without LEDs",
          hid_output_report(8, out, sizeof(out), &value) == -1);
    model_disconnects = 0;
    hid_gone(8);
    CHECK("report: a device gone is disconnected", model_disconnects == 1);
    CHECK("report: and asks for its endpoint's packet", hid_report_size(8) == 0);

    CHECK("report: a device without strings is HID vvvv:pppp",
          hid_attach(8, &no_strings, rdesc, 4) == 0 && same(model_name, "HID 0627:0001"));
    CHECK("report: no serial string is an empty uniq", same(model_uniq, ""));
    hid_gone(8);
    hid_attach(8, &long_serial, rdesc, 4);
    CHECK("report: a long serial is cut before the character 63 bytes cannot hold",
          strlen(model_uniq) == 62 && model_uniq[61] == 'B');
    hid_gone(8);

    model_leds = 1;
    model_multiplier = 1;
    model_multiplier_set = -1;
    wanted_leds = LED_NUM;
    hid_attach(9, &qemu_mouse, rdesc, 4);
    CHECK("report: the multiplier report goes first",
          hid_output_report(9, out, sizeof(out), &value) == 2 && out[0] == 2 && value == 0x0302);
    hid_output_sent(9, 0);
    CHECK("report: its failure is passed on", model_multiplier_set == 0);
    CHECK("report: then the LEDs, not the multiplier again",
          hid_output_report(9, out, sizeof(out), &value) == 1 && out[0] == (1u << LED_NUML) &&
              value == 0x0200);
    hid_output_sent(9, 1);
    CHECK("report: and nothing while they stay",
          hid_output_report(9, out, sizeof(out), &value) == -1);
    model_lit = 1u << 4;
    CHECK("report: a program lighting Kana sends it with Num Lock",
          hid_output_report(9, out, sizeof(out), &value) == 1 &&
              out[0] == ((1u << LED_NUML) | (1u << 4)));
    model_lit = 0;
    hid_gone(9);

    model_multiplier = 0;
    model_led_len = 20;
    hid_attach(9, &qemu_mouse, rdesc, 4);
    CHECK("report: an LED report of 20 bytes is asked room for and sent whole",
          hid_output_size(9) == 20 && hid_output_report(9, out20, sizeof(out20), &value) == 20 &&
              out20[19] == (1u << LED_NUML));
    hid_gone(9);
    model_led_len = 1;
    model_multiplier = 0;

    CHECK("report: a refused descriptor is passed back",
          hid_attach(10, &qemu_mouse, rdesc, 0) == -EINVAL);
    model_inputs = -1;
    CHECK("report: a descriptor with no input device is -ENODEV",
          hid_attach(10, &qemu_mouse, rdesc, 4) == -ENODEV);
    model_inputs = 1;
    mouse_registrations = 0;
    hid_attach_boot(10, HID_MOUSE, &qemu_mouse);
    CHECK("report: and the device runs in the boot protocol, named by its strings",
          mouse_registrations == 1 && same(mouse_name, "QEMU QEMU USB Mouse") &&
              hid_report_size(10) == 0);
    hid_gone(10);
    CHECK("report: a device out of range is not taken",
          hid_attach(0, &qemu_mouse, rdesc, 4) == -ENODEV);
}

/** A mouse: its event device, and its reports with and without a wheel. */
static void mouse_rows(void)
{
    unsigned char mouse[4];

    /* A mouse is registered as Linux's boot-protocol driver names it. */
    hid_attach_boot(3, HID_MOUSE, &no_strings);
    CHECK("a mouse is registered once", mouse_registrations == 1);
    CHECK("as USB HIDBP Mouse 0627:0001 on usb-3/input0",
          same(mouse_name, "USB HIDBP Mouse 0627:0001") && same(mouse_phys, "usb-3/input0"));
    CHECK("on the USB bus with its vendor and product",
          mouse_bus == EVDEV_BUS_USB && mouse_vendor == 0x0627 && mouse_product == 0x0001);
    CHECK("with a wheel in 120ths and the side buttons",
          mouse_caps == (MOUSE_WHEEL | MOUSE_HI_RES | MOUSE_SIDE));
    hid_attach_boot(3, HID_MOUSE, &no_strings);
    CHECK("a mouse attached again is not registered again", mouse_registrations == 1);

    /* A USB report's Y is positive toward the user, as REL_Y is. */
    mouse[0] = 0x01;
    mouse[1] = (unsigned char)5;
    mouse[2] = (unsigned char)3;
    mouse[3] = 0;
    mouse_reports = 0;
    hid_report(3, mouse, 4);
    CHECK("a mouse report is passed on", mouse_reports == 1);
    CHECK("to the mouse's device", mouse_dev == (struct evdev *)&mouse_registrations);
    CHECK("with its movement", mouse_dx == 5);
    CHECK("its vertical movement as the report has it", mouse_dy == 3);
    CHECK("and its buttons", mouse_buttons == MOUSE_LEFT);

    mouse[0] = 0x1A; /* right, side and extra */
    mouse[1] = 0xFB; /* -5 */
    mouse[2] = 0xFD; /* -3 */
    mouse[3] = 0xFF; /* the wheel turned down a detent */
    hid_report(3, mouse, 4);
    CHECK("a negative movement stays negative", mouse_dx == -5 && mouse_dy == -3);
    CHECK("the wheel's byte is its turn", mouse_wheel == -1 && mouse_hwheel == 0);
    CHECK("the five buttons pass",
          mouse_buttons == (MOUSE_RIGHT | MOUSE_BUTTON_SIDE | MOUSE_BUTTON_EXTRA));
    hid_report(3, mouse, 3);
    CHECK("a report of three bytes turns no wheel", mouse_wheel == 0);

    mouse_reports = 0;
    hid_report(3, mouse, 2);
    CHECK("a short mouse report is dropped", mouse_reports == 0);

    hid_gone(3);
    CHECK("a mouse that goes takes its event device away", mouse_unregistrations == 1);
    hid_gone(3);
    CHECK("and only once", mouse_unregistrations == 1);
}

/**
 * Run the rows and print the result; return 1 when a row failed.
 *
 * Usage 0x01 (ErrorRollOver) or 0x02 (POSTFail) in a key slot says the
 * keyboard cannot tell which keys are down. Translating the slots would press
 * keys that are not down, and storing them would forget the keys that are
 * and press them again on the next report, so the rows require both kinds to
 * send nothing and to leave the held keys alone. A keyboard unplugged with
 * keys held sends no release of its own, so `hid_gone` must send them.
 */
int main(void)
{
    hid_attach_boot(1, HID_KEYBOARD, &no_strings);
    CHECK("a keyboard without strings is USB HIDBP Keyboard 0627:0001",
          same(keyboard_name, "USB HIDBP Keyboard 0627:0001"));

    /* A key pressed, held across two reports, and released. */
    keys(1, 0, 0x04, 0, 0); /* 'a' down */
    CHECK("a key pressed sends its make code", sent(1, 0x1E, 0, 0, 0));
    keys(1, 0, 0x04, 0, 0); /* still down */
    CHECK("a key held sends nothing", n_codes == 0);
    keys(1, 0, 0, 0, 0); /* up */
    CHECK("a key released sends its break code", sent(1, 0x9E, 0, 0, 0));

    /* Two keys, one released while the other is held. */
    keys(1, 0, 0x04, 0x05, 0); /* a and b */
    CHECK("two keys pressed send both", sent(2, 0x1E, 0x30, 0, 0));
    keys(1, 0, 0x05, 0, 0); /* a released, b held */
    CHECK("releasing one leaves the other alone", sent(1, 0x9E, 0, 0, 0));
    keys(1, 0, 0, 0, 0);
    CHECK("and the other is released in its turn", sent(1, 0xB0, 0, 0, 0));

    /* The modifiers, which are their own byte and not usages. */
    keys(1, 0x02, 0, 0, 0); /* left shift */
    CHECK("a modifier pressed sends its code", sent(1, 0x2A, 0, 0, 0));
    keys(1, 0x02, 0x04, 0, 0);
    CHECK("a key under a held modifier sends only the key", sent(1, 0x1E, 0, 0, 0));
    keys(1, 0, 0, 0, 0);
    CHECK("and releasing both sends both breaks, the modifier first", sent(2, 0xAA, 0x9E, 0, 0));

    /* A right-hand modifier is the extended form. */
    keys(1, 0x10, 0, 0, 0); /* right control */
    CHECK("a right-hand modifier is sent in its two-byte form", sent(2, 0xE0, 0x1D, 0, 0));
    keys(1, 0, 0, 0, 0);
    CHECK("and released the same way", sent(2, 0xE0, 0x9D, 0, 0));

    /* An arrow, which is extended too. */
    keys(1, 0, 0x52, 0, 0); /* up arrow */
    CHECK("an arrow is sent in its two-byte form", sent(2, 0xE0, 0x48, 0, 0));
    keys(1, 0, 0, 0, 0);

    /* Print Screen and Pause, which have extended codes of their own. */
    keys(1, 0, 0x46, 0, 0);
    CHECK("Print Screen is E0 37", sent(2, 0xE0, 0x37, 0, 0));
    CHECK("with its usage as MSC_SCAN", last_scan == 0x70046u);
    keys(1, 0, 0x48, 0, 0);
    CHECK("Pause pressed is E0 45, then Print Screen released", sent(4, 0xE0, 0x45, 0xE0, 0xB7));
    keys(1, 0, 0, 0, 0);

    /* ErrorRollOver: more keys are down than the keyboard can tell apart. */
    {
        unsigned char rollover[8];
        int i;

        for (i = 0; i < 8; i++) {
            rollover[i] = 1;
        }
        rollover[0] = 0;
        rollover[1] = 0;
        keys(1, 0, 0x04, 0, 0); /* 'a' down */
        n_codes = 0;
        hid_report(1, rollover, 8);
        CHECK("a rollover report's keys are dropped", n_codes == 0);

        /* The modifier byte of a rollover report is still valid. */
        rollover[0] = 0x02;
        n_codes = 0;
        hid_report(1, rollover, 8);
        CHECK("but its modifiers are read", sent(1, 0x2A, 0, 0, 0));
        rollover[0] = 0;
        n_codes = 0;
        hid_report(1, rollover, 8);
        CHECK("and released", sent(1, 0xAA, 0, 0, 0));

        /* POSTFail, another error usage, means the same. */
        for (i = 2; i < 8; i++) {
            rollover[i] = 2;
        }
        n_codes = 0;
        hid_report(1, rollover, 8);
        CHECK("a post-fail report is dropped as well", n_codes == 0);
        /* The keys held before the rollover are still held after it. */
        keys(1, 0, 0x04, 0, 0);
        CHECK("a key still down after a rollover is not pressed again", n_codes == 0);
        keys(1, 0, 0, 0, 0);
    }

    /* A report shorter than its layout is not read past. */
    keys(1, 0, 0x04, 0, 0);
    n_codes = 0;
    hid_report(1, (const unsigned char *)"\0\0\0", 3);
    CHECK("a short keyboard report is dropped", n_codes == 0);
    keys(1, 0, 0, 0, 0);

    /* A keyboard unplugged with keys held: the driver sends their releases. */
    keys(1, 0x01, 0x04, 0x05, 0);
    n_codes = 0;
    hid_gone(1);
    CHECK("everything held is released when the device goes", sent(3, 0x9D, 0x9E, 0xB0, 0));
    n_codes = 0;
    hid_gone(1);
    CHECK("and a device that has already gone releases nothing", n_codes == 0);

    /* Two keyboards, each with its own state. */
    hid_attach_boot(1, HID_KEYBOARD, &no_strings);
    hid_attach_boot(2, HID_KEYBOARD, &no_strings);
    keys(1, 0, 0x04, 0, 0);
    keys(2, 0, 0x04, 0, 0);
    CHECK("a second keyboard's first report is a press, not a hold", sent(1, 0x1E, 0, 0, 0));

    mouse_rows();

    leds_rows();

    report_rows();

    if (fails == 0) {
        printf("hid_hosttest: OK\n");
        return 0;
    }
    printf("hid_hosttest: %d failures\n", fails);
    return 1;
}
