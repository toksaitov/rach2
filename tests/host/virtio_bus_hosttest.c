/**
 * virtio_bus_hosttest.c: the virtio bus of rach2/sys/virtio/virtio_bus.c in
 * the /sys registry of rach2/sys/fs/sysfs.c, read back as synthfs.c
 * classifies it.
 *
 * host-deps: rach2/sys/virtio/virtio_bus.c
 * host-deps: rach2/sys/fs/synthfs.c rach2/sys/fs/sysfs.c
 * host-deps: rach2/sys/lib/kstring.c rach2/sys/lib/kctypes.c
 *
 * A model virtio-gpu device at 00:02.0 is added to the bus, which names it
 * virtio0, and checked against Linux 6.12's shape: its directory below the
 * PCI function, its attributes, its `subsystem` link and its link in
 * /sys/bus/virtio/devices, the link texts as kernfs writes them, and its
 * "add" uevent; then bound to virtio_gpu, with a card below it as
 * drm_sysfs.c makes one, whose /sys/dev/char link a `..` climbs from the
 * virtio device, as libdrm's drmParseSubsystemType reads it; then its
 * "bind", "unbind" and "remove" uevents. A device of a transport that shows
 * none, the shared region's, is left alone. The `..` rows check which
 * links' targets lie inside a walk's root, the whole of /sys or a bind of
 * a directory in it. The last rows add and remove devices and check that
 * each takes the lowest number free.
 *
 * Returns 0 when every row passes, 1 otherwise.
 */

#include "fs/synthfs.h"
#include "fs/sysfs.h"
#include "lib/kerrno.h"
#include "lib/kstring.h"
#include "virtio/virtio.h"

extern int printf(const char *format, ...);

static int failures;

#define CHECK(c, s)                                     \
    do {                                                \
        if (!(c)) {                                     \
            printf("virtio-bus-test: fail: %s\n", (s)); \
            failures++;                                 \
        }                                               \
    } while (0)

/* ---- what synthfs.c reads of the rest of the kernel: nothing here ---- */

int sysfs_net_index(const char *name, int len)
{
    (void)name;
    (void)len;
    return -1;
}

void sysfs_net_name(int index, char *out, int cap)
{
    (void)index;
    if (cap > 0) {
        out[0] = '\0';
    }
}

const char *sysfs_net_slot_name(int slot)
{
    (void)slot;
    return "";
}

int sysfs_net_pci_key(int index)
{
    (void)index;
    return -1;
}

int sysfs_net_wiphy(int index)
{
    (void)index;
    return -1;
}

int rfkill_lookup(const char *name, int len)
{
    (void)name;
    (void)len;
    return -1;
}

int rfkill_phy(int index)
{
    (void)index;
    return -1;
}

const char *sysfs_wlan_driver(int index)
{
    (void)index;
    return 0;
}

int sysfs_block_find(const char *name, int len, int disk)
{
    (void)name;
    (void)len;
    (void)disk;
    return -1;
}

int sysfs_block_devno(const char *name, int len)
{
    (void)name;
    (void)len;
    return -1;
}

void sysfs_block_name(int key, char *out, int cap)
{
    (void)key;
    if (cap > 0) {
        out[0] = '\0';
    }
}

int sysfs_block_whole(int key)
{
    (void)key;
    return -1;
}

int sysfs_block_loop(int key)
{
    (void)key;
    return -1;
}

void devtree_path(int id, char *out, int cap)
{
    (void)id;
    if (cap > 1) {
        out[0] = '/';
        out[1] = '\0';
    }
}

int backlight_find(const char *name, int len)
{
    (void)name;
    (void)len;
    return -1;
}

const char *backlight_name(int i)
{
    (void)i;
    return 0;
}

int watchdog_hardlockup_available(void)
{
    return 0;
}

void *kmalloc(size_t size)
{
    return __builtin_malloc(size);
}

void *kzalloc(size_t size)
{
    return __builtin_calloc(1, size);
}

void kfree(void *ptr)
{
    __builtin_free(ptr);
}

/* The last uevent sent, its NULs made newlines, and how many were sent. */
static char last_event[2400];
static int events;

void netlink_uevent(const uint8_t *msg, uint32_t len)
{
    uint32_t i;

    for (i = 0; i < len && i < sizeof(last_event) - 1; i++) {
        last_event[i] = msg[i] ? (char)msg[i] : '\n';
    }
    last_event[i] = '\0';
    events++;
}

/* ---- the model device ---- */

/** The model's status byte, the device's after a bind. */
static uint8_t get_status(struct virtio_dev *d)
{
    (void)d;
    return 0x0F; /* ACKNOWLEDGE, DRIVER, DRIVER_OK and FEATURES_OK */
}

static struct virtio_ops model_ops;

/** Whether the string `s` holds `part`. */
static int has(const char *s, const char *part)
{
    int i;

    for (; *s; s++) {
        for (i = 0; part[i] && s[i] == part[i]; i++) {
        }
        if (!part[i]) {
            return 1;
        }
    }
    return !*part;
}

/** Whether the strings `a` and `b` are equal. */
static int eq(const char *a, const char *b)
{
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

/** The text of the attribute `name` of the directory `dir` into `out` of
 * `cap` bytes, NUL-terminated, or "" when it is not there. */
static void attr_text(int dir, const char *name, char *out, int cap)
{
    int h = sysfs_node_child(dir, name, (int)k_strlen(name));
    int32_t n = h > 0 ? sysfs_node_show(h, out, (uint32_t)cap - 1) : 0;

    out[n > 0 && n < cap ? n : 0] = '\0';
}

/** The text of the link `name` of the directory `dir` into `out`, or "". */
static void link_text(int dir, const char *name, char *out, int cap)
{
    int h = sysfs_node_child(dir, name, (int)k_strlen(name));
    int n = h > 0 ? sysfs_link_text(h, out, cap - 1) : 0;

    out[n > 0 ? n : 0] = '\0';
}

/* The model device, virtio-gpu at 00:02.0, and a scratch path and text. */
static struct virtio_dev dev;
static struct vpath vp;
static char text[256], path[256];

/** virtio0 added below its function, with Linux's attributes, links and
 * add uevent. */
static void row_add(void)
{
    static const char features[] =
        "1100000000000000000000000000000010000000000000000000000000000000\n";
    int pci = sysfs_anchor(SYSFS_PCI_DEVICE, 2 << 3);

    model_ops.get_status = get_status;
    k_memset(&dev, 0, sizeof(dev));
    dev.ops = &model_ops;
    dev.id = VIRTIO_ID_GPU;
    dev.name = "virtio";
    dev.features = 0x3ull | 1ull << 32; /* VIRGL, EDID and VERSION_1 */
    CHECK(virtio_bus_add(&dev, pci, 0x1AF4) == 0 && dev.sys_dir > 0 && eq(dev.name, "virtio0"),
          "the first device added is virtio0");
    sysfs_node_path(dev.sys_dir, path, (int)sizeof(path));
    CHECK(eq(path, "/sys/devices/pci0000:00/0000:00:02.0/virtio0"),
          "virtio0 is below its PCI function");
    CHECK(events == 1 && has(last_event, "add@/devices/pci0000:00/0000:00:02.0/virtio0\n") &&
              has(last_event, "\nSUBSYSTEM=virtio\n") &&
              has(last_event, "\nMODALIAS=virtio:d00000010v00001AF4\n") &&
              !has(last_event, "DRIVER="),
          "the add uevent has SUBSYSTEM=virtio and Linux's MODALIAS, and no DRIVER");
    attr_text(dev.sys_dir, "device", text, (int)sizeof(text));
    CHECK(eq(text, "0x0010\n"), "device is the virtio type as 0x%04x");
    attr_text(dev.sys_dir, "vendor", text, (int)sizeof(text));
    CHECK(eq(text, "0x1af4\n"), "vendor is the subsystem vendor as 0x%04x");
    attr_text(dev.sys_dir, "status", text, (int)sizeof(text));
    CHECK(eq(text, "0x0000000f\n"), "status is the device's status as 0x%08x");
    attr_text(dev.sys_dir, "modalias", text, (int)sizeof(text));
    CHECK(eq(text, "virtio:d00000010v00001AF4\n"), "modalias is Linux's");
    attr_text(dev.sys_dir, "features", text, (int)sizeof(text));
    CHECK(eq(text, features), "features gives each of the 64 bits taken, bit 0 first");
    link_text(dev.sys_dir, "subsystem", text, (int)sizeof(text));
    CHECK(eq(text, "../../../../bus/virtio"), "subsystem leads to /sys/bus/virtio");
    CHECK(synth_classify("sys/bus/virtio/devices/virtio0", &vp) == VNODE_SYS_LINK &&
              vp.sysreg > 0 && sysfs_link_text(vp.sysreg, text, (int)sizeof(text) - 1) > 0,
          "/sys/bus/virtio/devices/virtio0 is a link");
    link_text(sysfs_node_parent(vp.sysreg), "virtio0", text, (int)sizeof(text));
    CHECK(eq(text, "../../../devices/pci0000:00/0000:00:02.0/virtio0"),
          "the bus's link leads to the device");
    CHECK(synth_classify("sys/bus/virtio/drivers", &vp) == VNODE_SYS_DIR,
          "/sys/bus/virtio/drivers is there");
}

/** virtio0 bound to virtio_gpu, with a card below it as drm_sysfs.c makes
 * one, and its bind and unbind uevents. */
static void row_bind(void)
{
    int card, drm, chr, dir;

    CHECK(virtio_driver_bind(&dev, "virtio_gpu") == 0, "virtio0 is bound to virtio_gpu");
    CHECK(virtio_driver_bind(&dev, "virtio_gpu") == -EBUSY, "a bound device is not bound again");
    link_text(dev.sys_dir, "driver", text, (int)sizeof(text));
    CHECK(eq(text, "../../../../bus/virtio/drivers/virtio_gpu"),
          "driver leads to /sys/bus/virtio/drivers/virtio_gpu");
    CHECK(synth_classify("sys/bus/virtio/drivers/virtio_gpu/virtio0", &vp) == VNODE_SYS_LINK,
          "the driver's directory links the device");
    attr_text(dev.sys_dir, "uevent", text, (int)sizeof(text));
    CHECK(eq(text, "DRIVER=virtio_gpu\nMODALIAS=virtio:d00000010v00001AF4\n"),
          "the uevent file gives DRIVER, then MODALIAS");
    CHECK(events == 1, "binding sends nothing before the driver is up");
    dir = virtio_bus_dir(&dev);
    drm = sysfs_add_dir(dir, "drm");
    card = drm > 0 ? sysfs_add_dir(drm, "card0") : drm;
    CHECK(card > 0 && sysfs_add_link(card, "device", dir) > 0, "the card is made below virtio0");
    chr = sysfs_add_link(sysfs_anchor(SYSFS_DEV_CHAR, 0), "226:0", card);
    link_text(card, "device", text, (int)sizeof(text));
    CHECK(chr > 0 && eq(text, "../../../virtio0"), "the card's device leads to virtio0");
    CHECK(synth_classify("sys/dev/char/226:0/device/subsystem", &vp) == VNODE_SYS_LINK,
          "the card's device has a subsystem link");
    CHECK(synth_sys_link_dir("sys/dev/char/226:0/device", 3, path, (int)sizeof(path)) == 1 &&
              eq(path, "/sys/devices/pci0000:00/0000:00:02.0/virtio0"),
          "a .. after the card's device link climbs from virtio0, to the PCI function");
    CHECK(synth_sys_link_dir("sys/dev/char/226:0/device/drm", 3, path, (int)sizeof(path)) == 0,
          "a path that ends on a directory is climbed by its text");
    CHECK(synth_sys_link_dir("sys/devices/pci0000:00/0000:00:02.0/virtio0/drm/card0/device",
                             (int)k_strlen("sys/devices/pci0000:00/0000:00:02.0"), path,
                             (int)sizeof(path)) == 1,
          "below a bind of the PCI function, the card's device link stays inside it");
    CHECK(synth_sys_link_dir("sys/bus/virtio/devices/virtio0",
                             (int)k_strlen("sys/bus/virtio/devices"), path,
                             (int)sizeof(path)) == -1 &&
              eq(path, "/sys/devices/pci0000:00/0000:00:02.0/virtio0"),
          "below a bind of /sys/bus/virtio/devices, virtio0's directory lies outside it, so the "
          "walk follows the link's text, whose .. climbs out of the bind");
    CHECK(synth_sys_link_dir("sys/devices/pci0000:00/0000:00:02.0/virtio0/drm/card0/device",
                             (int)k_strlen("sys/devices/pci0000:00/0000:00:02"), path,
                             (int)sizeof(path)) == -1,
          "a root that ends inside a name of the target is no directory it lies below");
    virtio_driver_bound(&dev);
    CHECK(events == 2 && has(last_event, "bind@/devices/pci0000:00/0000:00:02.0/virtio0\n") &&
              has(last_event, "\nDRIVER=virtio_gpu\n"),
          "the bind uevent carries DRIVER");
    virtio_driver_bound(&dev);
    CHECK(events == 2, "bind is sent once");
    sysfs_remove(chr);
    sysfs_remove(drm);
    virtio_driver_unbind(&dev);
    CHECK(events == 3 && has(last_event, "unbind@/devices/pci0000:00/0000:00:02.0/virtio0\n") &&
              !has(last_event, "DRIVER="),
          "unbind is sent after the driver's links are gone");
    CHECK(synth_classify("sys/devices/pci0000:00/0000:00:02.0/virtio0/driver", &vp) ==
                  VNODE_ENOENT &&
              synth_classify("sys/bus/virtio/drivers/virtio_gpu/virtio0", &vp) == VNODE_ENOENT,
          "the driver's links are gone");
    virtio_driver_unbind(&dev);
    CHECK(events == 3, "a device not bound sends no unbind");
}

/** virtio0 taken off the bus, and a device of a transport that shows
 * none. */
static void row_remove(void)
{
    struct virtio_dev shm;

    CHECK(virtio_driver_bind(&dev, "virtio_gpu") == 0, "virtio0 binds again");
    virtio_bus_remove(&dev);
    CHECK(events == 4 && has(last_event, "remove@/devices/pci0000:00/0000:00:02.0/virtio0\n") &&
              dev.sys_dir == 0,
          "remove is sent, the driver let go without an unbind since bind was not sent");
    CHECK(synth_classify("sys/devices/pci0000:00/0000:00:02.0/virtio0", &vp) == VNODE_ENOENT &&
              synth_classify("sys/bus/virtio/devices/virtio0", &vp) == VNODE_ENOENT &&
              synth_classify("sys/bus/virtio/drivers/virtio_gpu/virtio0", &vp) == VNODE_ENOENT,
          "the device, its bus link and its driver's link are gone");
    k_memset(&shm, 0, sizeof(shm));
    shm.ops = &model_ops;
    shm.name = "shm";
    CHECK(virtio_driver_bind(&shm, "virtio_gpu") == 0 && !shm.sys_driver &&
              virtio_bus_dir(&shm) == 0,
          "a device its transport does not show binds with nothing in /sys");
    virtio_driver_bound(&shm);
    virtio_driver_unbind(&shm);
    virtio_bus_remove(&shm);
    CHECK(events == 4, "and sends no uevent");
}

/** Each device added takes the lowest number no device on the bus holds,
 * and one taken off the bus gives its number back, as Linux's
 * virtio_index_ida. */
static void row_numbers(void)
{
    struct virtio_dev d[4];
    int i;

    k_memset(d, 0, sizeof(d));
    for (i = 0; i < 4; i++) {
        d[i].ops = &model_ops;
        d[i].id = VIRTIO_ID_NET;
        d[i].name = "virtio";
    }
    CHECK(virtio_bus_add(&d[0], sysfs_anchor(SYSFS_PCI_DEVICE, 3 << 3), 0x1AF4) == 0 &&
              virtio_bus_add(&d[1], sysfs_anchor(SYSFS_PCI_DEVICE, 4 << 3), 0x1AF4) == 0 &&
              eq(d[0].name, "virtio0") && eq(d[1].name, "virtio1"),
          "two devices on an empty bus are virtio0 and virtio1");
    virtio_bus_remove(&d[0]);
    CHECK(virtio_bus_add(&d[2], sysfs_anchor(SYSFS_PCI_DEVICE, 5 << 3), 0x1AF4) == 0 &&
              eq(d[2].name, "virtio0") &&
              synth_classify("sys/devices/pci0000:00/0000:00:05.0/virtio0", &vp) == VNODE_SYS_DIR,
          "the next device after virtio0's removal takes virtio0");
    CHECK(virtio_bus_add(&d[3], sysfs_anchor(SYSFS_PCI_DEVICE, 6 << 3), 0x1AF4) == 0 &&
              eq(d[3].name, "virtio2"),
          "and the one after it virtio2, the lowest number free");
    for (i = 1; i < 4; i++) {
        virtio_bus_remove(&d[i]);
    }
    CHECK(synth_classify("sys/bus/virtio/devices/virtio0", &vp) == VNODE_ENOENT &&
              synth_classify("sys/bus/virtio/devices/virtio1", &vp) == VNODE_ENOENT &&
              synth_classify("sys/bus/virtio/devices/virtio2", &vp) == VNODE_ENOENT,
          "every number is free again");
}

int main(void)
{
    row_add();
    row_bind();
    row_remove();
    row_numbers();
    if (failures) {
        return 1;
    }
    printf("virtio-bus-test: ok\n");
    return 0;
}
