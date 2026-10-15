/*
 * hid_hosttest.c: the USB HID class driver, usb/hid.c, fed keyboard and mouse
 * reports directly, with no controller and no keyboard.
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
 */

extern int printf(const char *fmt, ...);

#include "../../rach2/sys/usb/hid.h"

/* ---- the functions outside hid.c that it calls ---- */

#include "../../rach2/sys/input.h"

/* Every code the driver sent since the last report, in order; codes past
 * MAX_CODES are dropped. */
#define MAX_CODES 64
static unsigned char codes[MAX_CODES];
static int n_codes;

/** Do nothing: hid.c stamps each code for the latency record, which this test
 * does not check. */
void usb_stamp_wake(void)
{}

/** Record a scancode in `codes`, as the input layer would queue it. */
void input_key(uint8_t scancode)
{
    if (n_codes < MAX_CODES) {
        codes[n_codes] = scancode;
        n_codes++;
    }
}

static int mouse_dx;
static int mouse_dy;
static int mouse_buttons;
static int mouse_reports;

/** Record the last mouse movement and buttons, and count the reports. */
void input_mouse(int dx, int dy, int buttons)
{
    mouse_dx = dx;
    mouse_dy = dy;
    mouse_buttons = buttons;
    mouse_reports++;
}

/** Return 1: a mouse is present. */
int input_mouse_present(void)
{
    return 1;
}

/** Do nothing: no row checks the announcement of a mouse. */
void input_mouse_found(void)
{}

/** Discard the message: no row checks the log. */
void klog(int level, const char *subsystem, const char *format, ...)
{
    (void)level;
    (void)subsystem;
    (void)format;
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
    hid_report(slot, HID_KEYBOARD, report, 8);
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
    unsigned char mouse[4];

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
        hid_report(1, HID_KEYBOARD, rollover, 8);
        CHECK("a rollover report's keys are dropped", n_codes == 0);

        /* The modifier byte of a rollover report is still valid. */
        rollover[0] = 0x02;
        n_codes = 0;
        hid_report(1, HID_KEYBOARD, rollover, 8);
        CHECK("but its modifiers are read", sent(1, 0x2A, 0, 0, 0));
        rollover[0] = 0;
        n_codes = 0;
        hid_report(1, HID_KEYBOARD, rollover, 8);
        CHECK("and released", sent(1, 0xAA, 0, 0, 0));

        /* POSTFail, another error usage, means the same. */
        for (i = 2; i < 8; i++) {
            rollover[i] = 2;
        }
        n_codes = 0;
        hid_report(1, HID_KEYBOARD, rollover, 8);
        CHECK("a post-fail report is dropped as well", n_codes == 0);
        /* The keys held before the rollover are still held after it. */
        keys(1, 0, 0x04, 0, 0);
        CHECK("a key still down after a rollover is not pressed again", n_codes == 0);
        keys(1, 0, 0, 0, 0);
    }

    /* A report shorter than its layout is not read past. */
    keys(1, 0, 0x04, 0, 0);
    n_codes = 0;
    hid_report(1, HID_KEYBOARD, (const unsigned char *)"\0\0\0", 3);
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
    keys(1, 0, 0x04, 0, 0);
    keys(2, 0, 0x04, 0, 0);
    CHECK("a second keyboard's first report is a press, not a hold", sent(1, 0x1E, 0, 0, 0));

    /* A USB mouse's Y grows downward, the kernel's queue's upward. */
    mouse[0] = 0x01;
    mouse[1] = (unsigned char)5;
    mouse[2] = (unsigned char)3;
    mouse[3] = 0;
    mouse_reports = 0;
    hid_report(3, HID_MOUSE, mouse, 4);
    CHECK("a mouse report is passed on", mouse_reports == 1);
    CHECK("with its movement", mouse_dx == 5);
    CHECK("its vertical movement turned the other way up", mouse_dy == -3);
    CHECK("and its buttons", mouse_buttons == 1);

    mouse[1] = 0xFB; /* -5 */
    mouse[2] = 0xFD; /* -3 */
    hid_report(3, HID_MOUSE, mouse, 4);
    CHECK("a negative movement stays negative", mouse_dx == -5);
    CHECK("and its vertical half is turned as well", mouse_dy == 3);

    mouse_reports = 0;
    hid_report(3, HID_MOUSE, mouse, 2);
    CHECK("a short mouse report is dropped", mouse_reports == 0);

    if (fails == 0) {
        printf("hid_hosttest: OK\n");
        return 0;
    }
    printf("hid_hosttest: %d failures\n", fails);
    return 1;
}
