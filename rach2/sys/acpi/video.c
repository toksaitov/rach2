/*
 * video.c: the ACPI video buses as event devices, as Linux's acpi_video.c
 * finds them and reports their keys: a Notify on an output is a brightness
 * key and one on the bus the video mode's switch. The brightness itself is
 * left to user space and the backlight class; no level is changed here.
 */

#include "../evdev.h"
#include "../klog.h"
#include "../lib/kstring.h"

#include "acpi.h"

#include "devices.h"

/* The most video buses taken. */
#define VIDEO_BUSES 4

/* Linux's product number of the video bus's input device. */
#define VIDEO_PRODUCT 0x06

/* The bus's Notify of a switch of the active output. */
#define VIDEO_NOTIFY_SWITCH 0x80

/* The longest path of a bus logged. */
#define VIDEO_PATH_MAX 64

/* An output's Notify values, 0x85 to 0x89, and their keys. */
static const struct {
    UINT32 value;
    uint16_t key;
} output_keys[] = {{0x85, KEY_BRIGHTNESS_CYCLE},
                   {0x86, KEY_BRIGHTNESSUP},
                   {0x87, KEY_BRIGHTNESSDOWN},
                   {0x88, KEY_BRIGHTNESS_ZERO},
                   {0x89, KEY_DISPLAY_OFF}};

/* A video bus: its event device, the context of the notify handlers of the
 * bus and of each of its outputs. */
struct video_bus {
    struct evdev *dev;
    char path[VIDEO_PATH_MAX];
};

static struct video_bus buses[VIDEO_BUSES];
static int nbuses;

/** Report `key`, which the Notify `value` on `what` gave, with a record. */
static void video_key(struct video_bus *bus, const char *what, UINT32 value, uint16_t key)
{
    klog(KLOG_DEBUG, "acpi", "event: video bus: %s notify 0x%x: key %u", what, (unsigned)value,
         (unsigned)key);
    acpi_report_key(bus->dev, key);
}

/** A Notify on an output of the bus, in ACPICA's thread: 0x85 to 0x89 are
 * the brightness keys; the others give none. */
static void output_notify(ACPI_HANDLE device, UINT32 value, void *context)
{
    unsigned i;

    (void)device;
    for (i = 0; i < sizeof(output_keys) / sizeof(output_keys[0]); i++) {
        if (output_keys[i].value == value) {
            video_key((struct video_bus *)context, "output", value, output_keys[i].key);
        }
    }
}

/** A Notify on the bus itself: 0x80 cycles the active output and is
 * KEY_SWITCHVIDEOMODE; the rest give none. */
static void bus_notify(ACPI_HANDLE device, UINT32 value, void *context)
{
    (void)device;
    if (value == VIDEO_NOTIFY_SWITCH) {
        video_key((struct video_bus *)context, "bus", value, KEY_SWITCHVIDEOMODE);
    }
}

/** Whether `handle` has the child object `name`. */
static int has(ACPI_HANDLE handle, const char *name)
{
    char path[5];
    ACPI_HANDLE child;

    k_memcpy(path, name, 5);
    return ACPI_SUCCESS(AcpiGetHandle(handle, path, &child));
}

/**
 * Tell the firmware, by _DOS, not to switch the outputs itself, and on a
 * firmware that took Windows 8's path, which ACPICA's _OSI answers lead
 * to, not to change the brightness itself either: Linux's _DOS(0, 1).
 */
static void video_dos(ACPI_HANDLE handle, const char *path)
{
    static char dos[] = "_DOS";
    ACPI_OBJECT arg;
    ACPI_OBJECT_LIST args;
    ACPI_STATUS status;

    arg.Type = ACPI_TYPE_INTEGER;
    arg.Integer.Value = AcpiGbl_OsiData >= ACPI_OSI_WIN_8 ? 4 : 0;
    args.Count = 1;
    args.Pointer = &arg;
    status = AcpiEvaluateObject(handle, dos, &args, NULL);
    if (ACPI_FAILURE(status) && status != AE_NOT_FOUND) {
        klog(KLOG_WARN, "acpi", "video: %s: _DOS: %s", path, AcpiFormatException(status));
    }
}

/**
 * A present device that is a video bus, as Linux tells one, by _DOD or
 * _DOS: an event device "Video Bus", _DOS called, and the Notify of the bus
 * and of every output under it taken. The outputs are the bus's children,
 * which need not be present to be notified.
 */
static ACPI_STATUS video_found(ACPI_HANDLE handle, UINT32 depth, void *context, void **ret)
{
    uint8_t keys[EVDEV_KEY_BYTES];
    struct video_bus *bus;
    ACPI_HANDLE child = NULL;
    ACPI_BUFFER name;
    unsigned i;
    int outputs = 0;

    (void)depth;
    (void)context;
    (void)ret;
    if (!has(handle, "_DOD") && !has(handle, "_DOS")) {
        return AE_OK;
    }
    if (nbuses == VIDEO_BUSES) {
        return AE_CTRL_TERMINATE;
    }
    bus = &buses[nbuses++];
    name.Length = sizeof(bus->path);
    name.Pointer = bus->path;
    if (ACPI_FAILURE(AcpiGetName(handle, ACPI_FULL_PATHNAME_NO_TRAILING, &name))) {
        bus->path[0] = '\0';
    }
    k_memset(keys, 0, sizeof(keys));
    for (i = 0; i < sizeof(output_keys) / sizeof(output_keys[0]); i++) {
        keys[output_keys[i].key / 8] |= (uint8_t)(1u << (output_keys[i].key % 8));
    }
    keys[KEY_SWITCHVIDEOMODE / 8] |= (uint8_t)(1u << (KEY_SWITCHVIDEOMODE % 8));
    bus->dev = evdev_register("Video Bus", EVDEV_BUS_HOST, 0, VIDEO_PRODUCT, keys);
    evdev_describe(bus->dev, "LNXVIDEO/video/input0", 0);
    (void)AcpiInstallNotifyHandler(handle, ACPI_DEVICE_NOTIFY, bus_notify, bus);
    while (ACPI_SUCCESS(AcpiGetNextObject(ACPI_TYPE_DEVICE, handle, child, &child))) {
        if (ACPI_SUCCESS(AcpiInstallNotifyHandler(child, ACPI_DEVICE_NOTIFY, output_notify, bus))) {
            outputs++;
        }
    }
    video_dos(handle, bus->path);
    klog(KLOG_DEBUG, "acpi", "video: bus %s, outputs %d", bus->path, outputs);
    return AE_OK;
}

void acpi_video_start(void)
{
    (void)AcpiGetDevices(NULL, video_found, NULL, NULL);
}
