/*
 * input_scenario.c: the scenarios of tests/cases/machine.input.json, the
 * keyboards' event devices and the mixer, each case a boot of its own.
 */
#include <stdlib.h>
#include <string.h>

#include "usbcase.h"

/* The family's cases begin with this, and their scenarios are named by the
 * rest. */
#define FAMILY "machine.input."

/* The kernel's record of a configured USB keyboard. */
#define KEYBOARD "class 3 protocol 1"

/* The keys evprobe reads first, as the monitor's sendkey names them, and
 * the twelve that overflow the queue of the reader that reads nothing. */
#define KEYS  "a volumeup volumedown audiomute"
#define FLOOD "b c d e f g h i j k l m"

/** Begin step `boot`, boot the machine `input` into `b`, wait for `record`
 * on its console when it is not NULL and then for the debug shell, and
 * report assertion `shell`, and `keyboard` with a record. */
static void booted(struct usb_case *u, struct usb_boot *b, const char *record)
{
    usb_step(u, "boot");
    usb_boot(b, u, "input", 1, NULL);
    if (record != NULL)
        usb_seen(b, record);
    usb_wait(b, USB_SHELL);
    usb_assert(u, "shell", 1, "");
    if (record != NULL)
        usb_assert(u, "keyboard", 1, "");
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
