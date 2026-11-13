/*
 * input_scenario.c: the scenarios of tests/cases/machine.input.json, the
 * keyboards' and mice's event devices and the mixer, each case a boot of its
 * own.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "usbcase.h"

/* The family's cases begin with this, and their scenarios are named by the
 * rest. */
#define FAMILY "machine.input."

/* The kernel's records of a configured USB keyboard, mouse and tablet. */
#define KEYBOARD "class 3 protocol 1"
#define MOUSE    "class 3 protocol 2"
#define TABLET   "class 3 protocol 0"

/* The keys evprobe reads first, as the monitor's sendkey names them, and
 * the twelve that overflow the queue of the reader that reads nothing. */
#define KEYS  "a volumeup volumedown audiomute"
#define FLOOD "b c d e f g h i j k l m"

/** Begin step `boot`, boot the machine `input` into `b`, wait for `record`
 * on its console when it is not NULL and then for the debug shell, and
 * report assertion `shell`, and `keyboard` with a keyboard's record or
 * `device` with a mouse's or a tablet's. */
static void booted(struct usb_case *u, struct usb_boot *b, const char *record)
{
    usb_step(u, "boot");
    usb_boot(b, u, "input", 1, NULL);
    if (record != NULL)
        usb_seen(b, record);
    usb_wait(b, USB_SHELL);
    usb_assert(u, "shell", 1, "");
    if (record != NULL)
        usb_assert(u, strcmp(record, KEYBOARD) == 0 ? "keyboard" : "device", 1, "");
}

/** Run `command`, evprobe of a keyboard, press its keys when it asks for
 * them, and report assertion `keys` of its run. */
static int keys(struct usb_case *u, const char *record, const char *command)
{
    struct usb_boot b;
    struct usb_run r;
    uint64_t from;

    booted(u, &b, record);
    usb_step(u, "keys");
    from = usb_start(&b, command);
    usb_wait(&b, "EVPROBE: press the keys");
    usb_keys(&b, KEYS);
    usb_wait(&b, "EVPROBE: flood");
    usb_keys(&b, FLOOD);
    usb_finish(&b, from, &r);
    usb_probe(u, "keys", &r, "EVPROBE");
    free(r.text);
    return usb_end(u);
}

/** Send QMP's input-send-event with the events `events`, a JSON array. */
static void send(struct usb_boot *b, const char *events)
{
    usb_qmp(b, "input-send-event", "{\"events\": %s}", events);
}

/* The mouse's and the tablet's events, each a QMP command of its own so
 * that each is a packet: the left button pressed and released, and the
 * wheel turned up a detent and down one. */
#define LEFT_DOWN "[{\"type\": \"btn\", \"data\": {\"down\": true, \"button\": \"left\"}}]"
#define LEFT_UP   "[{\"type\": \"btn\", \"data\": {\"down\": false, \"button\": \"left\"}}]"
#define TURN(b)                                                                 \
    "[{\"type\": \"btn\", \"data\": {\"down\": true, \"button\": \"" b "\"}}, " \
    "{\"type\": \"btn\", \"data\": {\"down\": false, \"button\": \"" b "\"}}]"

/** Send the `n` commands of `events`, each once evprobe has printed that
 * it read the packet of the one before, "EVPROBE: `what`: packet N read",
 * and wait for the last's. QEMU's USB HID folds an event into the report
 * the guest has not taken when its buttons are the same, which sums the
 * wheel's two turns to nothing, and its PS/2 mouse keeps a packet that does
 * not fit its queue of 16 bytes until the next event. Sent together, the
 * events never all reach a slow guest, Linux as much as this kernel. */
static void packets(struct usb_boot *b, const char *what, const char *const events[], int n)
{
    char line[64];
    int i;

    for (i = 0; i < n; i++) {
        send(b, events[i]);
        snprintf(line, sizeof(line), "EVPROBE: %s: packet %d read", what, i + 1);
        usb_wait(b, line);
    }
}

/** Run `command`, evprobe of a mouse; when it asks, move the mouse by 10
 * and 5, press and release the left button, and turn the wheel up and down
 * (`packets`); report assertion `mouse` of its run. */
static int mouse(struct usb_case *u, const char *record, const char *command)
{
    static const char *const events[] = {
        "[{\"type\": \"rel\", \"data\": {\"axis\": \"x\", \"value\": 10}}, "
        "{\"type\": \"rel\", \"data\": {\"axis\": \"y\", \"value\": 5}}]",
        LEFT_DOWN, LEFT_UP, TURN("wheel-up"), TURN("wheel-down")};
    struct usb_boot b;
    struct usb_run r;
    uint64_t from;

    booted(u, &b, record);
    usb_step(u, "mouse");
    from = usb_start(&b, command);
    usb_wait(&b, "EVPROBE: move the mouse");
    packets(&b, "mouse", events, (int)(sizeof(events) / sizeof(events[0])));
    usb_finish(&b, from, &r);
    usb_probe(u, "mouse", &r, "EVPROBE");
    free(r.text);
    return usb_end(u);
}

/** Run evprobe of QEMU's tablet; when it asks, move the tablet to 10000
 * and 20000, press and release the left button, and turn the wheel up and
 * down (`packets`); report assertion `tablet` of its run. */
static int tablet(struct usb_case *u)
{
    static const char *const events[] = {
        "[{\"type\": \"abs\", \"data\": {\"axis\": \"x\", \"value\": 10000}}, "
        "{\"type\": \"abs\", \"data\": {\"axis\": \"y\", \"value\": 20000}}]",
        LEFT_DOWN, LEFT_UP, TURN("wheel-up"), TURN("wheel-down")};
    struct usb_boot b;
    struct usb_run r;
    uint64_t from;

    booted(u, &b, TABLET);
    usb_step(u, "tablet");
    from = usb_start(&b, "/evprobe usbtablet");
    usb_wait(&b, "EVPROBE: move the tablet");
    packets(&b, "tablet", events, (int)(sizeof(events) / sizeof(events[0])));
    usb_finish(&b, from, &r);
    usb_probe(u, "tablet", &r, "EVPROBE");
    free(r.text);
    return usb_end(u);
}

/** The PS/2 mouse's event device. */
static int ps2_mouse(struct usb_case *u)
{
    return mouse(u, NULL, "/evprobe mouse");
}

/** A USB mouse's event device. */
static int usb_mouse_case(struct usb_case *u)
{
    return mouse(u, MOUSE, "/evprobe usbmouse");
}

/** The PS/2 keyboard's event device. */
static int ps2(struct usb_case *u)
{
    return keys(u, NULL, "/evprobe ps2");
}

/** A USB keyboard's event device. */
static int usb(struct usb_case *u)
{
    return keys(u, KEYBOARD, "/evprobe usb");
}

/** Run `command`, evprobe of the mixer, and report assertion `mixer`. */
static int mixer(struct usb_case *u, const char *command)
{
    struct usb_boot b;
    struct usb_run r;

    booted(u, &b, NULL);
    usb_step(u, "mixer");
    usb_exec(&b, command, &r);
    usb_probe(u, "mixer", &r, "EVPROBE");
    free(r.text);
    return usb_end(u);
}

/** The mixer on Intel HD Audio. */
static int mixer_hda(struct usb_case *u)
{
    return mixer(u, "/evprobe mixer HDA");
}

/** The mixer on the Sound Blaster 16. */
static int mixer_sb16(struct usb_case *u)
{
    return mixer(u, "/evprobe mixer SB16");
}

/* Each case's scenario, by the last part of its identity. */
static const struct {
    const char *name;
    int (*run)(struct usb_case *u);
} scenarios[] = {
    {"ps2", ps2},
    {"usb", usb},
    {"mouse", ps2_mouse},
    {"usb-mouse", usb_mouse_case},
    {"usb-tablet", tablet},
    {"mixer-hda", mixer_hda},
    {"mixer-sb16", mixer_sb16},
};

int etude_scenario(struct etude_case *c)
{
    const char *id = etude_case_id(c);
    struct usb_case u;
    size_t i;

    usb_case(&u, c);
    if (strncmp(id, FAMILY, strlen(FAMILY)) != 0)
        return 1;
    for (i = 0; i < sizeof(scenarios) / sizeof(scenarios[0]); i++) {
        if (strcmp(id + strlen(FAMILY), scenarios[i].name) == 0)
            return scenarios[i].run(&u);
    }
    return 1;
}
