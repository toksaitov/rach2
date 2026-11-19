/*
 * video.c: the ACPI video buses as event devices, as Linux's acpi_video.c
 * finds them and reports their keys: a Notify on an output is a brightness
 * key and one on the bus the video mode's switch. On a machine with no
 * native backlight, an output's _BCL and _BCM are the backlight
 * acpi_video0, as Linux registers it when no GPU driver has one; a key
 * changes no level here, which is user space's.
 */

#include "../display/backlight.h"
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

/* The most brightness levels of a _BCL taken, past its first two. */
#define VIDEO_LEVELS 101

/*
 * The backlight acpi_video0: its output, its levels in rising order with
 * none repeated, the class's level asked last and the one _BCM was last
 * given, each an index into `levels`, and whether a _BCM waits on
 * ACPICA's thread. `want` and `queued` change with the class's lock held
 * and are read on that thread.
 */
static struct {
    ACPI_HANDLE output;
    int levels[VIDEO_LEVELS];
    int count;
    volatile int want;
    volatile int cur;
    volatile int queued;
} light;

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

/** Evaluate the integer method `name` of `handle` into `*value`; 0, or -1
 * for a method that is missing, fails or gives no integer. */
static int video_integer(ACPI_HANDLE handle, const char *name, UINT64 *value)
{
    char path[5];
    ACPI_OBJECT obj;
    ACPI_BUFFER ret;

    k_memcpy(path, name, 5);
    ret.Length = sizeof(obj);
    ret.Pointer = &obj;
    if (ACPI_FAILURE(AcpiEvaluateObjectTyped(handle, path, NULL, &ret, ACPI_TYPE_INTEGER))) {
        return -1;
    }
    *value = obj.Integer.Value;
    return 0;
}

/** Give _BCM of acpi_video0's output the level the class asked last, on
 * ACPICA's thread, where a method may run. */
static void video_bcm(void *context)
{
    static char bcm[] = "_BCM";
    ACPI_OBJECT arg;
    ACPI_OBJECT_LIST args;
    ACPI_STATUS status;
    int want;

    (void)context;
    light.queued = 0;
    want = light.want;
    arg.Type = ACPI_TYPE_INTEGER;
    arg.Integer.Value = (UINT64)light.levels[want];
    args.Count = 1;
    args.Pointer = &arg;
    status = AcpiEvaluateObject(light.output, bcm, &args, NULL);
    if (ACPI_FAILURE(status)) {
        klog(KLOG_WARN, "acpi", "video: _BCM %d: %s", light.levels[want],
             AcpiFormatException(status));
        return;
    }
    light.cur = want;
}

/** The class's `get`: the level _BCM was last given. */
static int video_get(void *ctx)
{
    (void)ctx;
    return light.cur;
}

/** The class's `set`, with its lock held and interrupts off, where no
 * method may run: keep the level and have ACPICA's thread give it to
 * _BCM, once for any number of sets before it runs. */
static void video_set(void *ctx, int level)
{
    (void)ctx;
    light.want = level < 0 ? 0 : level >= light.count ? light.count - 1 : level;
    if (!light.queued) {
        light.queued = 1;
        if (ACPI_FAILURE(AcpiOsExecute(OSL_NOTIFY_HANDLER, video_bcm, NULL))) {
            light.queued = 0;
        }
    }
}

static const struct backlight_ops video_ops = {video_get, video_set, NULL};

/**
 * Read the output `output`'s _BCL into `light`'s levels, as Linux's
 * acpi_video_init_brightness does: the integers after the first two, the
 * levels for AC and battery, sorted and with none repeated. 0, or -1 for a
 * _BCL that is missing or has fewer than two levels.
 */
static int video_levels(ACPI_HANDLE output)
{
    static char bcl[] = "_BCL";
    ACPI_BUFFER ret;
    ACPI_OBJECT *pkg;
    UINT32 i;
    int n = 0, j, v;

    ret.Length = ACPI_ALLOCATE_BUFFER;
    ret.Pointer = NULL;
    if (ACPI_FAILURE(AcpiEvaluateObjectTyped(output, bcl, NULL, &ret, ACPI_TYPE_PACKAGE))) {
        return -1;
    }
    pkg = (ACPI_OBJECT *)ret.Pointer;
    for (i = 2; i < pkg->Package.Count && n < VIDEO_LEVELS; i++) {
        if (pkg->Package.Elements[i].Type != ACPI_TYPE_INTEGER ||
            pkg->Package.Elements[i].Integer.Value > 100) {
            continue;
        }
        v = (int)pkg->Package.Elements[i].Integer.Value;
        for (j = n; j > 0 && light.levels[j - 1] > v; j--) {
            light.levels[j] = light.levels[j - 1];
        }
        if (j > 0 && light.levels[j - 1] == v) {
            for (; j < n; j++) {
                light.levels[j] = light.levels[j + 1];
            }
            continue;
        }
        light.levels[j] = v;
        n++;
    }
    AcpiOsFree(ret.Pointer);
    light.count = n;
    return n >= 2 ? 0 : -1;
}

/**
 * Register acpi_video0 on `output` of the bus at `path` when it has _BCL
 * and _BCM and no backlight is registered, as Linux's acpi_video registers
 * its backlight only where no native one is: the Lenovo's intel_backlight
 * comes first, so there it is not made. Its level is _BQC's when that
 * names one of the levels, else the highest, which _BCM is given, as
 * Linux sets an unknown level to the most. Returns 1 when registered.
 */
static int video_backlight(ACPI_HANDLE output, const char *path)
{
    UINT64 now;
    int i, k;

    if (light.output != NULL || backlight_count() > 0 || !has(output, "_BCM") ||
        video_levels(output) != 0) {
        return 0;
    }
    light.output = output;
    light.cur = light.count - 1;
    if (video_integer(output, "_BQC", &now) == 0) {
        for (i = 0; i < light.count; i++) {
            if ((UINT64)light.levels[i] == now) {
                light.cur = i;
            }
        }
    }
    light.want = light.cur;
    k = backlight_register("acpi_video0", light.count - 1, &video_ops, NULL);
    if (k < 0) {
        light.output = NULL;
        return 0;
    }
    backlight_set_type(k, "firmware");
    if (video_integer(output, "_BQC", &now) != 0 || (UINT64)light.levels[light.cur] != now) {
        video_set(NULL, light.cur);
    }
    klog(KLOG_INFO, "acpi", "video: %s: acpi_video0: levels 0 to %d, at %d", path, light.count - 1,
         light.cur);
    return 1;
}

/**
 * A present device that is a video bus, as Linux tells one, by _DOD or
 * _DOS: an event device "Video Bus", _DOS called, the Notify of the bus
 * and of every output under it taken, and the first output with a
 * brightness made acpi_video0 (`video_backlight`). The outputs are the
 * bus's children, which need not be present to be notified.
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
        (void)video_backlight(child, bus->path);
    }
    video_dos(handle, bus->path);
    klog(KLOG_DEBUG, "acpi", "video: bus %s, outputs %d", bus->path, outputs);
    return AE_OK;
}

void acpi_video_start(void)
{
    (void)AcpiGetDevices(NULL, video_found, NULL, NULL);
}
