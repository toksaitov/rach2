/*
 * button.c: the ACPI power and sleep buttons as event devices, as Linux's
 * button driver gives them: the FADT's fixed buttons, whose events come in
 * the SCI, and the devices PNP0C0C and PNP0C0E, whose Notify 0x80 is a press.
 */

#include "../evdev.h"
#include "../klog.h"
#include "../lib/kstring.h"

#include "acpi.h"

#include "devices.h"

/* Linux's product numbers of the buttons, its ACPI_BUTTON_TYPE_ values. */
#define BUTTON_POWER 0x01
#define BUTTON_SLEEP 0x03

/* The Notify value of a press, ACPI_BUTTON_NOTIFY_STATUS. */
#define BUTTON_NOTIFY_STATUS 0x80

/* The most button devices taken, of each kind together. */
#define BUTTONS 8

/* A button: its event device and its key. A device's is the context of
 * its notify handler; the fixed buttons' are their events' context. */
struct button {
    struct evdev *dev;
    uint16_t key;
    const char *what; /* "power button" or "sleep button", for the record */
};

static struct button buttons[BUTTONS];
static int nbuttons;

void acpi_report_key(struct evdev *dev, uint16_t code)
{
    evdev_report(dev, EV_KEY, code, 1);
    evdev_report(dev, EV_SYN, SYN_REPORT, 0);
    evdev_report(dev, EV_KEY, code, 0);
    evdev_report(dev, EV_SYN, SYN_REPORT, 0);
}

/**
 * A new button with its event device, named `name`, which reports `key`,
 * at `phys`; NULL when the table is full. The device is NULL, and its keys
 * are dropped, when every event node is taken.
 */
static struct button *button_new(const char *name, const char *phys, uint16_t product, uint16_t key,
                                 const char *what)
{
    uint8_t keys[EVDEV_KEY_BYTES];
    struct button *b;

    if (nbuttons == BUTTONS) {
        return NULL;
    }
    b = &buttons[nbuttons++];
    k_memset(keys, 0, sizeof(keys));
    keys[key / 8] |= (uint8_t)(1u << (key % 8));
    b->dev = evdev_register(name, EVDEV_BUS_HOST, 0, product, keys);
    evdev_describe(b->dev, phys, 0);
    b->key = key;
    b->what = what;
    return b;
}

/** A fixed button's event, in the SCI: its key, and a record. */
static UINT32 button_fixed(void *context)
{
    struct button *b = (struct button *)context;

    klog(KLOG_INFO, "acpi", "event: %s", b->what);
    acpi_report_key(b->dev, b->key);
    return ACPI_INTERRUPT_HANDLED;
}

/** A Notify on a button device, in ACPICA's thread: 0x80 is a press, and
 * the others, a wake among them, give no key. */
static void button_notify(ACPI_HANDLE device, UINT32 value, void *context)
{
    struct button *b = (struct button *)context;

    (void)device;
    if (value == BUTTON_NOTIFY_STATUS) {
        klog(KLOG_INFO, "acpi", "event: %s: notify", b->what);
        acpi_report_key(b->dev, b->key);
    }
}

/** Take the fixed button `event` when the FADT has it, which a clear
 * `method_flag` in its flags says, on a device of its own. */
static void fixed_button(UINT32 event, UINT32 method_flag, const char *name, const char *phys,
                         uint16_t product, uint16_t key, const char *what)
{
    struct button *b;
    ACPI_STATUS status;

    if (AcpiGbl_FADT.Flags & method_flag) {
        return;
    }
    b = button_new(name, phys, product, key, what);
    if (b == NULL) {
        return;
    }
    status = AcpiInstallFixedEventHandler(event, button_fixed, b);
    if (ACPI_FAILURE(status)) {
        klog(KLOG_WARN, "acpi", "button: fixed %s: %s", what, AcpiFormatException(status));
    }
}

/* What a walk of button devices registers: the kind's name, phys and key. */
struct button_kind {
    const char *name;
    const char *phys;
    uint16_t product;
    uint16_t key;
    const char *what;
};

/** A present device of a button's kind: a device of its own, and its
 * Notify taken. */
static ACPI_STATUS button_found(ACPI_HANDLE handle, UINT32 depth, void *context, void **ret)
{
    const struct button_kind *k = (const struct button_kind *)context;
    struct button *b;
    ACPI_STATUS status;

    (void)depth;
    (void)ret;
    b = button_new(k->name, k->phys, k->product, k->key, k->what);
    if (b == NULL) {
        return AE_CTRL_TERMINATE;
    }
    status = AcpiInstallNotifyHandler(handle, ACPI_DEVICE_NOTIFY, button_notify, b);
    if (ACPI_FAILURE(status)) {
        klog(KLOG_WARN, "acpi", "button: %s: notify: %s", k->what, AcpiFormatException(status));
    }
    return AE_OK;
}

/**
 * The fixed buttons first, then the devices, as Linux names them all:
 * "Power Button" and "Sleep Button", told apart by their phys, LNXPWRBN and
 * LNXSLPBN for the fixed ones and the device's _HID for the others. A
 * firmware may give the power button both ways, and then one press may
 * give two KEY_POWER.
 */
void acpi_buttons_start(void)
{
    static char power_hid[] = "PNP0C0C", sleep_hid[] = "PNP0C0E";
    static struct button_kind power = {"Power Button", "PNP0C0C/button/input0", BUTTON_POWER,
                                       KEY_POWER, "power button"};
    static struct button_kind sleep = {"Sleep Button", "PNP0C0E/button/input0", BUTTON_SLEEP,
                                       KEY_SLEEP, "sleep button"};

    fixed_button(ACPI_EVENT_POWER_BUTTON, ACPI_FADT_POWER_BUTTON, "Power Button",
                 "LNXPWRBN/button/input0", BUTTON_POWER, KEY_POWER, "power button");
    fixed_button(ACPI_EVENT_SLEEP_BUTTON, ACPI_FADT_SLEEP_BUTTON, "Sleep Button",
                 "LNXSLPBN/button/input0", BUTTON_SLEEP, KEY_SLEEP, "sleep button");
    (void)AcpiGetDevices(power_hid, button_found, &power, NULL);
    (void)AcpiGetDevices(sleep_hid, button_found, &sleep, NULL);
}
