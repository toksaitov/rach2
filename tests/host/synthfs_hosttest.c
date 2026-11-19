/**
 * synthfs_hosttest.c: the path classifier of rach2/sys/fs/synthfs.c, which
 * sorts a path among the KRFFS root, /dev, /proc and /sys.
 *
 * host-deps: rach2/sys/fs/synthfs.c rach2/sys/fs/sysfs.c
 * host-deps: rach2/sys/lib/kstring.c rach2/sys/lib/kctypes.c
 *
 * Every path call classifies its path, and the split into components and
 * the parsing of process ids and descriptor numbers are where an error of
 * one is easy to make, so the rows run here under the sanitizers. The model
 * has a wired interface eth0, a wireless wlan0 with its switch rfkill3, and
 * no disks; its interfaces' slots hold lo, eth0, wlan0 and wlan4, a radio
 * registered after others were removed. A driver's entries are added to
 * /sys through sysfs.c, the registry the classifier reads.
 */

#include "fs/synthfs.h"
#include "fs/sysfs.h"
#include "lib/kerrno.h"
#include "lib/kstring.h"
#include "mm/kheap.h"
#include "rfkill.h"
#include "dmi.h"

static int fails;

/** The index of the interface named by the `len` bytes at `name`: 2 for
 *  eth0, the one interface of the model, and -1 for any other. */
int sysfs_net_index(const char *name, int len)
{
    if (len == 5 && name[0] == 'w' && name[1] == 'l' && name[2] == 'a' && name[3] == 'n' &&
        name[4] == '0')
        return 3;
    return len == 4 && name[0] == 'e' && name[1] == 't' && name[2] == 'h' && name[3] == '0' ? 2
                                                                                            : -1;
}

/** The name of the interface `index`, eth0 for any index, into `out` when
 *  `cap` holds it. */
void sysfs_net_name(int index, char *out, int cap)
{
    if (index == 3 && cap > 5) {
        out[0] = 'w';
        out[1] = 'l';
        out[2] = 'a';
        out[3] = 'n';
        out[4] = '0';
        out[5] = '\0';
        return;
    }
    if (cap > 4) {
        out[0] = 'e';
        out[1] = 't';
        out[2] = 'h';
        out[3] = '0';
        out[4] = '\0';
    }
}

/** The interface in the slot `slot`: lo, eth0, wlan0 and wlan4 in 1 to 4,
 *  and none in 5. */
const char *sysfs_net_slot_name(int slot)
{
    static const char *const names[] = {"", "lo", "eth0", "wlan0", "wlan4", ""};

    return slot >= 0 && slot <= 5 ? names[slot] : "";
}

/** The PCI key of eth0's card, 0x18 (bus 0, device 3, function 0), and -1
 *  for any other index. */
int sysfs_net_pci_key(int index)
{
    return index == 2 ? 0x18 : -1;
}

/** wlan0 belongs to phy7; the wired interface has no radio. */
int sysfs_net_wiphy(int index)
{
    return index == 3 ? 7 : -1;
}

/** The switch named by the `len` bytes at `name`: 3 for rfkill3, phy7's,
 *  2 for rfkill2, of no radio, and -1 for any other. */
int rfkill_lookup(const char *name, int len)
{
    if (len != 7 || name[0] != 'r' || name[5] != 'l')
        return -1;
    return name[6] == '3' ? 3 : name[6] == '2' ? 2 : -1;
}

/** rfkill3 belongs to phy7. */
int rfkill_phy(int index)
{
    return index == 3 ? 7 : -1;
}

/** phy7 uses the software radio driver. */
const char *sysfs_wlan_driver(int index)
{
    return index == 7 ? "wsim" : 0;
}

/** No disk node has a name, since the model has no disks: /sys/block and
 *  /sys/dev/block are empty. Returns -1. */
int sysfs_block_find(const char *name, int len, int disk)
{
    (void)name;
    (void)len;
    (void)disk;
    return -1;
}

/** No disk node has device numbers. Returns -1. */
int sysfs_block_devno(const char *name, int len)
{
    (void)name;
    (void)len;
    return -1;
}

/** The empty name, for a disk node the model does not have. */
void sysfs_block_name(int key, char *out, int cap)
{
    (void)key;
    if (cap > 0) {
        out[0] = '\0';
    }
}

/** No disk node is on a whole disk. Returns -1. */
int sysfs_block_whole(int key)
{
    (void)key;
    return -1;
}

/** No disk node is a loop device. Returns -1. */
int sysfs_block_loop(int key)
{
    (void)key;
    return -1;
}

/** The path of an entry made under /dev, which the model has none of. */
void devtree_path(int id, char *out, int cap)
{
    (void)id;
    if (cap > 1) {
        out[0] = '/';
        out[1] = '\0';
    }
}

/** The backlight named by the `len` bytes at `name`: 0 for
 *  intel_backlight, the one backlight of the model, and -1 for any other. */
int backlight_find(const char *name, int len)
{
    static const char one[] = "intel_backlight";
    int i;

    if (len != (int)sizeof(one) - 1) {
        return -1;
    }
    for (i = 0; i < len; i++) {
        if (name[i] != one[i]) {
            return -1;
        }
    }
    return 0;
}

/** The name of backlight `i`: intel_backlight for 0, NULL past it. */
const char *backlight_name(int i)
{
    return i == 0 ? "intel_backlight" : 0;
}

/** The host's allocator, for the registry's entries. */
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

/** The hard-lockup detector can run on the model, so nmi_watchdog is
 * writable. */
int watchdog_hardlockup_available(void)
{
    return 1;
}

/** No uevent is sent here. */
void netlink_uevent(const uint8_t *msg, uint32_t len)
{
    (void)msg;
    (void)len;
}

#define CHECK(cond)    \
    do {               \
        if (!(cond)) { \
            fails++;   \
        }              \
    } while (0)

/** Whether the strings `a` and `b` are equal. */
static int eq(const char *a, const char *b)
{
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

/**
 * /sys: the fixed directories, a device's link and directory and uevent by
 * its name, and the names written back from the keys as they were read; a
 * name not of a device's form, or a hub port that four bits cannot hold, is
 * ENOENT, and a path through a uevent is ENOTDIR.
 */
static void sys_tree(void)
{
    struct vpath vp;
    char name[32], path[64];

    CHECK(synth_classify("sys", &vp) == VNODE_SYS_DIR && vp.sysent == SYSFS_ROOT);
    CHECK(synth_classify("sys/bus/usb/devices", &vp) == VNODE_SYS_DIR &&
          vp.sysent == SYSFS_USB_LINKS);
    CHECK(synth_classify("sys/devices/pci0000:00", &vp) == VNODE_SYS_DIR &&
          vp.sysent == SYSFS_PCI_ROOT);
    CHECK(synth_classify("sys/bus/pci/devices/0000:00:1f.2", &vp) == VNODE_SYS_LINK &&
          vp.sysent == SYSFS_PCI_DEVICE && vp.syskey == (0 << 8 | 0x1f << 3 | 2));
    synth_sys_device_name(SYSFS_PCI_DEVICE, vp.syskey, name, (int)sizeof(name));
    CHECK(eq(name, "0000:00:1f.2"));
    CHECK(synth_classify("sys/bus/pci/devices/0000:03:00.0/uevent", &vp) == VNODE_SYS_FILE &&
          vp.syskey == 3 << 8);
    CHECK(synth_classify("sys/devices/usb/2-17.3.4", &vp) == VNODE_SYS_DIR &&
          vp.sysent == SYSFS_USB_DEVICE && vp.syskey == (1 << 28 | 17 << 20 | 3 << 16 | 4 << 12));
    synth_sys_device_name(SYSFS_USB_DEVICE, vp.syskey, name, (int)sizeof(name));
    CHECK(eq(name, "2-17.3.4"));
    synth_sys_path(SYSFS_USB_DEVICE, vp.syskey, path, (int)sizeof(path));
    CHECK(eq(path, "/sys/devices/usb/2-17.3.4"));
    CHECK(synth_classify("sys/bus/usb/devices/1-2", &vp) == VNODE_SYS_LINK);
    CHECK(synth_classify("sys/bus/usb/devices/1-2/uevent/x", &vp) == VNODE_ENOTDIR);
    CHECK(synth_classify("sys/bus/usb/devices/1-2.16", &vp) == VNODE_ENOENT);
    CHECK(synth_classify("sys/bus/usb/devices/5-1", &vp) == VNODE_ENOENT);
    CHECK(synth_classify("sys/bus/usb/devices/1", &vp) == VNODE_ENOENT);
    CHECK(synth_classify("sys/bus/pci/devices/0000:00:20.0", &vp) == VNODE_ENOENT);
    CHECK(synth_classify("sys/bus/pci/devices/0000:00:1F.2", &vp) == VNODE_ENOENT);
    CHECK(synth_classify("sys/bus/pci/devices/0000:00:1f.2/vendor", &vp) == VNODE_SYS_FILE &&
          vp.procfile == SYSFS_PCI_VENDOR);
    CHECK(synth_classify("sys/bus/pci/devices/0000:00:1f.2/vendors", &vp) == VNODE_ENOENT);
    CHECK(synth_classify("sys/bus/pci/devices/0000:00:1f.2/resource0_wc", &vp) == VNODE_SYS_FILE &&
          vp.procfile == SYSFS_PCI_RESOURCE0_WC);
    CHECK(synth_classify("sys/devices/pci0000:00/0000:00:1f.2/resource5_wc", &vp) ==
              VNODE_SYS_FILE &&
          vp.procfile == SYSFS_PCI_RESOURCE0_WC + 5);
    CHECK(synth_classify("sys/bus/pci/devices/0000:00:1f.2/resource6_wc", &vp) == VNODE_ENOENT);
    CHECK(synth_classify("sys/bus/pci/devices/0000:00:1f.2/resource0_wc/x", &vp) == VNODE_ENOTDIR);
}

/** Check radio attributes and both routes to the driver link. */
static void sys_wireless(void)
{
    struct vpath vp;
    char path[128];

    CHECK(synth_classify("proc/net/wireless", &vp) == VNODE_PROC_NET_FILE &&
          vp.procfile == SYNTH_PROC_NETWIRELESS);
    CHECK(synth_classify("sys/class/ieee80211", &vp) == VNODE_SYS_DIR &&
          vp.sysent == SYSFS_CLASS_IEEE80211);
    CHECK(synth_classify("sys/class/ieee80211/phy7/index", &vp) == VNODE_SYS_FILE &&
          vp.sysent == SYSFS_WLAN_PHY && vp.syskey == 7 && vp.procfile == 0);
    CHECK(synth_classify("sys/class/ieee80211/phy7/macaddress/x", &vp) == VNODE_ENOTDIR);
    CHECK(synth_classify("sys/class/ieee80211/phy07", &vp) == VNODE_ENOENT);
    CHECK(synth_classify("sys/class/ieee80211/phy-1", &vp) == VNODE_ENOENT);
    CHECK(synth_classify("sys/class/net/wlan0/phy80211", &vp) == VNODE_SYS_LINK &&
          vp.sysent == SYSFS_WLAN_PHY && vp.syskey == 7);
    synth_sys_path(vp.sysent, vp.syskey, path, sizeof(path));
    CHECK(eq(path, "/sys/class/ieee80211/phy7"));
    CHECK(synth_classify("sys/class/net/wlan0/phy80211/name", &vp) == VNODE_SYS_FILE &&
          vp.sysent == SYSFS_WLAN_PHY && vp.procfile == 1);
    CHECK(synth_classify("sys/class/net/wlan0/device/driver", &vp) == VNODE_SYS_LINK &&
          vp.sysent == SYSFS_WLAN_DRIVER && vp.syskey == 7);
    synth_sys_path(vp.sysent, vp.syskey, path, sizeof(path));
    CHECK(eq(path, "/sys/class/ieee80211/phy7/device/wsim"));
    CHECK(synth_classify("sys/class/ieee80211/phy7/device/wsim", &vp) == VNODE_SYS_DIR);
    CHECK(synth_classify("sys/class/net/eth0/phy80211", &vp) == VNODE_ENOENT);
}

/** The rfkill class: a switch's directory, its attributes, and the `device`
 * link to its radio, or to itself when it has none. */
static void sys_rfkill(void)
{
    struct vpath vp;
    char path[128];

    CHECK(synth_classify("sys/class/rfkill", &vp) == VNODE_SYS_DIR &&
          vp.sysent == SYSFS_CLASS_RFKILL);
    CHECK(synth_classify("sys/class/rfkill/rfkill3", &vp) == VNODE_SYS_DIR &&
          vp.sysent == SYSFS_RFKILL && vp.syskey == 3);
    synth_sys_path(vp.sysent, vp.syskey, path, sizeof(path));
    CHECK(eq(path, "/sys/class/rfkill/rfkill3"));
    CHECK(synth_classify("sys/class/rfkill/rfkill3/soft", &vp) == VNODE_SYS_FILE &&
          vp.sysent == SYSFS_RFKILL && vp.procfile == RFKILL_ATTR_SOFT &&
          sysfs_attr_writable(vp.sysent, vp.procfile));
    CHECK(synth_classify("sys/class/rfkill/rfkill3/state", &vp) == VNODE_SYS_FILE &&
          sysfs_attr_writable(vp.sysent, vp.procfile));
    CHECK(synth_classify("sys/class/rfkill/rfkill3/uevent", &vp) == VNODE_SYS_FILE &&
          vp.procfile == RFKILL_ATTR_UEVENT && sysfs_attr_writable(vp.sysent, vp.procfile));
    CHECK(synth_classify("sys/class/rfkill/rfkill3/hard_block_reasons", &vp) == VNODE_SYS_FILE &&
          vp.procfile == RFKILL_ATTR_REASONS);
    CHECK(synth_classify("sys/class/rfkill/rfkill3/device", &vp) == VNODE_SYS_LINK &&
          vp.sysent == SYSFS_WLAN_PHY && vp.syskey == 7);
    synth_sys_path(vp.sysent, vp.syskey, path, sizeof(path));
    CHECK(eq(path, "/sys/class/ieee80211/phy7"));
    CHECK(synth_classify("sys/class/rfkill/rfkill3/device/name", &vp) == VNODE_SYS_FILE &&
          vp.sysent == SYSFS_WLAN_PHY && vp.procfile == 1);
    CHECK(synth_classify("sys/class/rfkill/rfkill2/device", &vp) == VNODE_SYS_LINK &&
          vp.sysent == SYSFS_RFKILL && vp.syskey == 2);
    synth_sys_path(vp.sysent, vp.syskey, path, sizeof(path));
    CHECK(eq(path, "/sys/class/rfkill/rfkill2"));
    CHECK(synth_classify("sys/class/rfkill/rfkill2/device/name", &vp) == VNODE_SYS_FILE &&
          vp.sysent == SYSFS_RFKILL && vp.syskey == 2);
    CHECK(synth_classify("sys/class/rfkill/rfkill3/type/x", &vp) == VNODE_ENOTDIR);
    CHECK(synth_classify("sys/class/rfkill/rfkill3/bogus", &vp) == VNODE_ENOENT);
    CHECK(synth_classify("sys/class/rfkill/rfkill4", &vp) == VNODE_ENOENT);
}

/**
 * /sys/class: under /sys/class/net an interface's directory, an attribute, a
 * counter of its statistics and its device link are found by name, and an
 * interface or attribute it does not have is ENOENT. Under
 * /sys/class/backlight the one backlight's directory is found by name, its
 * brightness is writable and its max_brightness is not.
 */
static void sys_class(void)
{
    struct vpath vp;
    char path[64];

    CHECK(synth_classify("sys/class", &vp) == VNODE_SYS_DIR && vp.sysent == SYSFS_CLASS);
    CHECK(synth_classify("sys/class/net/eth0", &vp) == VNODE_SYS_DIR && vp.sysent == SYSFS_NET_IF &&
          vp.syskey == 2);
    CHECK(synth_classify("sys/class/net/eth0/address", &vp) == VNODE_SYS_FILE &&
          vp.sysent == SYSFS_NET_IF && vp.procfile == 0);
    CHECK(synth_classify("sys/class/net/eth0/statistics/rx_bytes", &vp) == VNODE_SYS_FILE &&
          vp.sysent == SYSFS_NET_STATS && vp.procfile == 2);
    CHECK(synth_classify("sys/class/net/eth0/device", &vp) == VNODE_SYS_LINK &&
          vp.sysent == SYSFS_PCI_DEVICE && vp.syskey == 0x18);
    CHECK(synth_classify("sys/class/net/eth0/mtu/x", &vp) == VNODE_ENOTDIR);
    CHECK(synth_classify("sys/class/net/eth0/bogus", &vp) == VNODE_ENOENT);
    CHECK(synth_classify("sys/class/net/eth9", &vp) == VNODE_ENOENT);
    synth_sys_path(SYSFS_NET_STATS, 2, path, (int)sizeof(path));
    CHECK(eq(path, "/sys/class/net/eth0/statistics"));
    CHECK(synth_classify("sys/class/backlight", &vp) == VNODE_SYS_DIR &&
          vp.sysent == SYSFS_CLASS_BACKLIGHT);
    CHECK(synth_classify("sys/class/backlight/intel_backlight", &vp) == VNODE_SYS_DIR &&
          vp.sysent == SYSFS_BACKLIGHT && vp.syskey == 0);
    CHECK(synth_classify("sys/class/backlight/intel_backlight/brightness", &vp) == VNODE_SYS_FILE &&
          vp.sysent == SYSFS_BACKLIGHT && sysfs_attr_writable(vp.sysent, vp.procfile));
    CHECK(synth_classify("sys/class/backlight/intel_backlight/max_brightness", &vp) ==
              VNODE_SYS_FILE &&
          !sysfs_attr_writable(vp.sysent, vp.procfile));
    CHECK(synth_classify("sys/class/backlight/intel_backlight/type/x", &vp) == VNODE_ENOTDIR);
    CHECK(synth_classify("sys/class/backlight/acpi_video0", &vp) == VNODE_ENOENT);
    synth_sys_path(SYSFS_BACKLIGHT, 0, path, (int)sizeof(path));
    CHECK(eq(path, "/sys/class/backlight/intel_backlight"));
}

/**
 * The processors under /sys/devices/system/cpu: a processor's directory
 * `cpuN` is found by its number, written without a leading zero, and has no
 * attribute; whether the processor is started is for the caller to ask.
 */
static void sys_cpus(void)
{
    struct vpath vp;
    char path[64];

    CHECK(synth_classify("sys/devices/system/cpu/online", &vp) == VNODE_SYS_FILE &&
          vp.sysent == SYSFS_CPU && vp.procfile == 2);
    CHECK(synth_classify("sys/devices/system/cpu/cpu0", &vp) == VNODE_SYS_DIR &&
          vp.sysent == SYSFS_CPUN && vp.syskey == 0);
    CHECK(synth_classify("sys/devices/system/cpu/cpu13", &vp) == VNODE_SYS_DIR &&
          vp.sysent == SYSFS_CPUN && vp.syskey == 13);
    synth_sys_path(SYSFS_CPUN, 13, path, (int)sizeof(path));
    CHECK(eq(path, "/sys/devices/system/cpu/cpu13"));
    CHECK(synth_classify("sys/devices/system/cpu/cpu01", &vp) == VNODE_ENOENT);
    CHECK(synth_classify("sys/devices/system/cpu/cpu", &vp) == VNODE_ENOENT);
    CHECK(synth_classify("sys/devices/system/cpu/cpux", &vp) == VNODE_ENOENT);
    CHECK(synth_classify("sys/devices/system/cpu/cpu1/x", &vp) == VNODE_ENOENT);
}

/**
 * The files Wine reads below /sys/devices/system and /sys/class/dmi: a
 * processor's caches, `cache/indexM` with M from 0 to 15 and no leading
 * zero, keyed N << 4 | M, and their attributes; the one NUMA node; the
 * clock source; and the DMI fields, of which only the serial numbers and
 * the UUID are private.
 */
static void sys_system(void)
{
    struct vpath vp;
    char path[80];

    CHECK(synth_classify("sys/devices/system/cpu/cpu3/cache", &vp) == VNODE_SYS_DIR &&
          vp.sysent == SYSFS_CPU_CACHE && vp.syskey == 3);
    CHECK(synth_classify("sys/devices/system/cpu/cpu3/cache/index2", &vp) == VNODE_SYS_DIR &&
          vp.sysent == SYSFS_CPU_CACHE_INDEX && vp.syskey == (3 << 4 | 2));
    CHECK(synth_classify("sys/devices/system/cpu/cpu3/cache/index15/size", &vp) == VNODE_SYS_FILE &&
          vp.sysent == SYSFS_CPU_CACHE_INDEX && vp.syskey == (3 << 4 | 15) &&
          eq(sysfs_attr(vp.sysent, vp.procfile), "size"));
    CHECK(synth_classify("sys/devices/system/cpu/cpu0/cache/index0/shared_cpu_map", &vp) ==
              VNODE_SYS_FILE &&
          eq(sysfs_attr(vp.sysent, vp.procfile), "shared_cpu_map"));
    CHECK(synth_classify("sys/devices/system/cpu/cpu0/cache/index16", &vp) == VNODE_ENOENT);
    CHECK(synth_classify("sys/devices/system/cpu/cpu0/cache/index01", &vp) == VNODE_ENOENT);
    CHECK(synth_classify("sys/devices/system/cpu/cpu0/cache/index", &vp) == VNODE_ENOENT);
    CHECK(synth_classify("sys/devices/system/cpu/cpu0/cache/index0/x", &vp) == VNODE_ENOENT);
    CHECK(synth_classify("sys/devices/system/cpu/cpu0/cache/index0/level/x", &vp) == VNODE_ENOTDIR);
    CHECK(SYSFS_KEYED(SYSFS_CPU_CACHE) && SYSFS_KEYED(SYSFS_CPU_CACHE_INDEX));
    synth_sys_path(SYSFS_CPU_CACHE_INDEX, 13 << 4 | 3, path, (int)sizeof(path));
    CHECK(eq(path, "/sys/devices/system/cpu/cpu13/cache/index3"));
    synth_sys_path(SYSFS_CPU_CACHE, 13, path, (int)sizeof(path));
    CHECK(eq(path, "/sys/devices/system/cpu/cpu13/cache"));

    CHECK(synth_classify("sys/devices/system/node/node0/cpumap", &vp) == VNODE_SYS_FILE &&
          vp.sysent == SYSFS_NODE0 && vp.procfile == 1);
    CHECK(synth_classify("sys/devices/system/node/possible", &vp) == VNODE_SYS_FILE &&
          vp.sysent == SYSFS_NODE);
    CHECK(synth_classify("sys/devices/system/node/node1", &vp) == VNODE_ENOENT);
    synth_sys_path(SYSFS_NODE0, 0, path, (int)sizeof(path));
    CHECK(eq(path, "/sys/devices/system/node/node0"));

    CHECK(synth_classify("sys/devices/system/clocksource/clocksource0/current_clocksource", &vp) ==
              VNODE_SYS_FILE &&
          vp.sysent == SYSFS_CLOCKSOURCE0 && vp.procfile == 1);
    synth_sys_path(SYSFS_CLOCKSOURCE0, 0, path, (int)sizeof(path));
    CHECK(eq(path, "/sys/devices/system/clocksource/clocksource0"));

    CHECK(synth_classify("sys/class/dmi/id", &vp) == VNODE_SYS_DIR && vp.sysent == SYSFS_DMI_ID);
    CHECK(synth_classify("sys/class/dmi/id/bios_vendor", &vp) == VNODE_SYS_FILE &&
          vp.procfile == DMI_BIOS_VENDOR && !sysfs_attr_private(vp.sysent, vp.procfile));
    CHECK(synth_classify("sys/class/dmi/id/product_uuid", &vp) == VNODE_SYS_FILE &&
          vp.procfile == DMI_PRODUCT_UUID && sysfs_attr_private(vp.sysent, vp.procfile));
    CHECK(synth_classify("sys/class/dmi/id/chassis_serial", &vp) == VNODE_SYS_FILE &&
          sysfs_attr_private(vp.sysent, vp.procfile));
    CHECK(synth_classify("sys/class/dmi/id/modalias", &vp) == VNODE_SYS_FILE &&
          vp.procfile == SYSFS_DMI_MODALIAS);
    CHECK(synth_classify("sys/class/dmi/id/x", &vp) == VNODE_ENOENT);
    CHECK(synth_classify("sys/class/dmi/x", &vp) == VNODE_ENOENT);
    synth_sys_path(SYSFS_DMI_ID, 0, path, (int)sizeof(path));
    CHECK(eq(path, "/sys/class/dmi/id"));
}

/**
 * The parser's bounds, of a number and of a path's depth. A process id or a
 * descriptor number past 2147483639 is ENOENT, and is not wrapped to a small
 * number another process might own. A leading '-' is not a digit, so "-1"
 * is not read as SYNTH_PID_SELF (-1). The deepest path of /proc, a thread's
 * descriptor link, is classified whole, and a deeper path is ENOENT or
 * ENOTDIR and is not cut to a shorter one; past a file it is ENOTDIR with
 * the file's process or pty, for the caller to look up.
 */
static void bounds(void)
{
    struct vpath vp;

    CHECK(synth_classify("proc/99999999999", &vp) == VNODE_ENOENT);
    CHECK(synth_classify("proc/4294967297", &vp) == VNODE_ENOENT);
    CHECK(synth_classify("proc/2147483639", &vp) == VNODE_PROC_PID && vp.pid == 2147483639);
    CHECK(synth_classify("proc/2147483640", &vp) == VNODE_ENOENT);
    CHECK(synth_classify("proc/7/fd/99999999999", &vp) == VNODE_ENOENT);

    CHECK(synth_classify("proc/-1", &vp) == VNODE_ENOENT);
    CHECK(synth_classify("proc/-1/stat", &vp) == VNODE_ENOENT);

    CHECK(synth_classify("proc/7/task/9/fd", &vp) == VNODE_PROC_FD_DIR);
    CHECK(synth_classify("proc/7/task/9/fd/0", &vp) == VNODE_PROC_FD_LINK && vp.procfd == 0 &&
          vp.pid == 7 && vp.tid == 9);
    CHECK(synth_classify("proc/7/task/9/fd/12", &vp) == VNODE_PROC_FD_LINK && vp.procfd == 12);
    CHECK(synth_classify("proc/7/task/9/stat/junk", &vp) == VNODE_ENOTDIR && vp.pid == 7 &&
          vp.tid == 9);
    CHECK(synth_classify("proc/7/task/9/fd/0/x", &vp) == VNODE_ENOENT);
    CHECK(synth_classify("proc/7/task/9/fd/0/x/y/z", &vp) == VNODE_ENOENT);
    CHECK(synth_classify("proc/7/fd/0/a/b/c/d", &vp) == VNODE_ENOENT);
    CHECK(synth_classify("proc/cmdline/a/b/c/d/e/f", &vp) == VNODE_ENOTDIR);
    CHECK(synth_classify("dev/pts/1/a/b/c/d", &vp) == VNODE_ENOTDIR && vp.pts_index == 1);
}

/**
 * A path past a file is ENOTDIR, with the coordinates of the file for the
 * caller to look it up: past a device, by its name; past a pty's slave, by
 * its number; and past a process's file or its exe link, by the process's
 * id. A path past a pty name that is not a number is ENOENT, and so is one
 * past cwd or root, since the classifier follows no link. The directories
 * of /dev's table, input and dri, classify with their row, and a name in
 * one as the device of the directory, a slash and the name.
 */
static void past_a_file(void)
{
    struct vpath vp;

    CHECK(synth_classify("dev/null/x", &vp) == VNODE_ENOTDIR && eq(vp.name, "null"));
    CHECK(synth_classify("dev/pts/3", &vp) == VNODE_PTS && vp.pts_index == 3);
    CHECK(synth_classify("dev/pts/3/x", &vp) == VNODE_ENOTDIR && vp.pts_index == 3);
    CHECK(synth_classify("dev/pts/x/y", &vp) == VNODE_ENOENT);
    CHECK(synth_classify("dev/input", &vp) == VNODE_DEV_SUBDIR &&
          eq(synth_dev_dir_at(vp.devdir)->name, "input"));
    CHECK(synth_classify("dev/dri", &vp) == VNODE_DEV_SUBDIR &&
          eq(synth_dev_dir_at(vp.devdir)->name, "dri"));
    CHECK(synth_classify("dev/dri/card0", &vp) == VNODE_DEV && eq(vp.name, "dri/card0"));
    CHECK(synth_classify("dev/dri/renderD128/x", &vp) == VNODE_ENOTDIR &&
          eq(vp.name, "dri/renderD128"));
    CHECK(synth_classify("dev/null", &vp) == VNODE_DEV && vp.devdir == -1);
    CHECK(synth_dev_dir_find("dri/card0", 3) == 1 && synth_dev_dir_find("dr", 2) < 0 &&
          synth_dev_dir_find("pts", 3) < 0 && synth_dev_dir_at(synth_dev_dir_count()) == NULL);
    CHECK(synth_classify("dev/input/event3", &vp) == VNODE_DEV && eq(vp.name, "input/event3"));
    CHECK(synth_classify("dev/input/event3/x", &vp) == VNODE_ENOTDIR &&
          eq(vp.name, "input/event3"));
    CHECK(synth_classify("proc/acpi", &vp) == VNODE_PROC_ACPI_DIR);
    CHECK(synth_classify("proc/acpi/call", &vp) == VNODE_PROC_ACPI_CALL);
    CHECK(synth_classify("proc/fs", &vp) == VNODE_PROC_FS_DIR);
    CHECK(synth_classify("proc/fs/lockwait", &vp) == VNODE_PROC_FS_LOCKWAIT);
    CHECK(synth_classify("proc/fs/other", &vp) == VNODE_ENOENT);
    CHECK(synth_classify("proc/fs/lockwait/x", &vp) == VNODE_ENOTDIR);
    CHECK(synth_classify("proc/acpi/call/x", &vp) == VNODE_ENOTDIR);
    CHECK(synth_classify("proc/acpi/wakeup", &vp) == VNODE_ENOENT);
    CHECK(synth_classify("proc/5/maps/x", &vp) == VNODE_ENOTDIR && vp.pid == 5);
    CHECK(synth_classify("proc/5/stat/x", &vp) == VNODE_ENOTDIR && vp.pid == 5);
    CHECK(synth_classify("proc/self/cmdline/x", &vp) == VNODE_ENOTDIR && vp.pid == SYNTH_PID_SELF);
    CHECK(synth_classify("proc/7/exe/x", &vp) == VNODE_ENOTDIR && vp.pid == 7);
    CHECK(synth_classify("proc/7/cwd/x", &vp) == VNODE_ENOENT);
    CHECK(synth_classify("proc/7/root/x", &vp) == VNODE_ENOENT);
}

/**
 * The files of /proc/net, /proc/net/stat, /proc/sysvipc, /proc/PID/ns,
 * /proc/PID/fdinfo and /dev/fd. A path past one of them is ENOTDIR, and a
 * path past a name none of them has is ENOENT, since there is no file to be
 * not a directory.
 */
static void past_a_name(void)
{
    struct vpath vp;

    CHECK(synth_classify("proc/net", &vp) == VNODE_PROC_NET_DIR);
    CHECK(synth_classify("proc/net/udp", &vp) == VNODE_PROC_NET_FILE &&
          vp.procfile == SYNTH_PROC_NETUDP);
    CHECK(synth_classify("proc/net/tcp", &vp) == VNODE_PROC_NET_TCP);
    CHECK(synth_classify("proc/net/packet", &vp) == VNODE_PROC_NET_FILE &&
          vp.procfile == SYNTH_PROC_NETPACKET);
    CHECK(synth_classify("proc/net/stat/rt_cache", &vp) == VNODE_PROC_NET_FILE);
    CHECK(synth_classify("proc/net/udp/x", &vp) == VNODE_ENOTDIR);
    CHECK(synth_classify("proc/net/tcp/x", &vp) == VNODE_ENOTDIR);
    CHECK(synth_classify("proc/net/bogus", &vp) == VNODE_ENOENT);
    CHECK(synth_classify("proc/net/bogus/x", &vp) == VNODE_ENOENT);
    CHECK(synth_classify("proc/net/stat", &vp) == VNODE_PROC_NET_STAT_DIR);
    CHECK(synth_classify("proc/net/stat/rt_cache", &vp) == VNODE_PROC_NET_FILE &&
          vp.procfile == SYNTH_PROC_NETSTAT_RT);
    CHECK(synth_classify("proc/net/stat/rt_cache/x", &vp) == VNODE_ENOTDIR);
    CHECK(synth_classify("proc/net/stat/bogus/x", &vp) == VNODE_ENOENT);

    CHECK(synth_classify("proc/sysvipc/msg", &vp) == VNODE_PROC_SYSVIPC &&
          vp.procfile == SYNTH_PROC_SYSVIPC + 2);
    CHECK(synth_classify("proc/sysvipc/shm/x", &vp) == VNODE_ENOTDIR);
    CHECK(synth_classify("proc/sysvipc/bogus/x", &vp) == VNODE_ENOENT);

    CHECK(synth_classify("proc/7/ns/net", &vp) == VNODE_PROC_NS_LINK && vp.procfd == 3);
    CHECK(synth_classify("proc/7/ns/net/x", &vp) == VNODE_ENOTDIR && vp.pid == 7);
    CHECK(synth_classify("proc/7/ns/bogus", &vp) == VNODE_ENOENT);
    CHECK(synth_classify("proc/7/ns/bogus/x", &vp) == VNODE_ENOENT);

    CHECK(synth_classify("proc/7/fdinfo/3", &vp) == VNODE_PROC_FDINFO && vp.procfd == 3);
    CHECK(synth_classify("proc/7/fdinfo/3/x", &vp) == VNODE_ENOTDIR && vp.pid == 7);
    CHECK(synth_classify("proc/7/fdinfo/xy/z", &vp) == VNODE_ENOENT);

    CHECK(synth_classify("dev/fd/3", &vp) == VNODE_PROC_FD_LINK && vp.procfd == 3 &&
          vp.pid == SYNTH_PID_SELF);
    CHECK(synth_classify("dev/fd/3/x", &vp) == VNODE_ENOTDIR);
    CHECK(synth_classify("dev/fd/xy", &vp) == VNODE_ENOENT);
    CHECK(synth_classify("dev/fd/xy/z", &vp) == VNODE_ENOENT);
}

/** The path `synth_dir_path` writes back for a directory, a thread's under
 *  its process's task directory and a row of /dev's directories among
 *  them. */
static void paths(void)
{
    char out[64];

    synth_dir_path(VNODE_PROC_PID, 7, 0, out, (int)sizeof(out));
    CHECK(eq(out, "/proc/7"));
    synth_dir_path(VNODE_PROC_PID, 7, 9, out, (int)sizeof(out));
    CHECK(eq(out, "/proc/7/task/9"));
    synth_dir_path(VNODE_PROC_FD_DIR, 7, 9, out, (int)sizeof(out));
    CHECK(eq(out, "/proc/7/task/9/fd"));
    synth_dir_path(VNODE_PROC_TASK_DIR, 7, 0, out, (int)sizeof(out));
    CHECK(eq(out, "/proc/7/task"));
    synth_dir_path(VNODE_DEV_SUBDIR, synth_dev_dir_find("dri", 3), 0, out, (int)sizeof(out));
    CHECK(eq(out, "/dev/dri"));
    synth_dir_path(VNODE_DEV_SUBDIR, synth_dev_dir_find("input", 5), 0, out, (int)sizeof(out));
    CHECK(eq(out, "/dev/input"));
}

/**
 * /dev/cpu and its row for each processor, cpu/0 to cpu/63, classify with
 * their row, the second in the first, and a name in one as the device of
 * the row's path, a slash and the name, as /dev/cpu/N/msr; a name that is
 * no processor's row is a device of /dev/cpu, which devfs does not have.
 */
static void dev_cpu_rows(void)
{
    struct vpath vp;
    char out[64];

    CHECK(synth_classify("dev/cpu", &vp) == VNODE_DEV_SUBDIR &&
          eq(synth_dev_dir_at(vp.devdir)->name, "cpu"));
    CHECK(synth_classify("dev/cpu/3", &vp) == VNODE_DEV_SUBDIR &&
          eq(synth_dev_dir_at(vp.devdir)->name, "cpu/3") &&
          synth_dev_dir_at(vp.devdir)->parent == synth_dev_dir_find("cpu", 3) &&
          eq(synth_dev_dir_leaf(synth_dev_dir_at(vp.devdir)), "3"));
    CHECK(synth_classify("dev/cpu/3/msr", &vp) == VNODE_DEV && eq(vp.name, "cpu/3/msr"));
    CHECK(synth_classify("dev/cpu/3/msr/x", &vp) == VNODE_ENOTDIR && eq(vp.name, "cpu/3/msr"));
    CHECK(synth_classify("dev/cpu/x", &vp) == VNODE_DEV && eq(vp.name, "cpu/x"));
    CHECK(synth_dev_dir_child(-1, "cpu", 3) == synth_dev_dir_find("cpu", 3) &&
          synth_dev_dir_child(-1, "3", 1) < 0 &&
          synth_dev_dir_child(synth_dev_dir_find("cpu", 3), "63", 2) ==
              synth_dev_dir_find("cpu/63", 6) &&
          synth_dev_dir_find("cpu/64", 6) < 0);
    synth_dir_path(VNODE_DEV_SUBDIR, synth_dev_dir_find("cpu/3", 5), 0, out, (int)sizeof(out));
    CHECK(eq(out, "/dev/cpu/3"));
}

/**
 * /proc/sys/net/ipv6/conf: a directory for `all`, `default` and each
 * interface in a slot, whatever its name, wlan4 among them, and none for a
 * name no slot holds.
 */
static void ipv6_conf(void)
{
    struct vpath vp;
    char path[64];
    const char *dir;
    int key;

    CHECK(synth_classify("proc/sys/net/ipv6/conf/all/forwarding", &vp) == VNODE_PROC_SYS_FILE);
    CHECK(synth_classify("proc/sys/net/ipv6/conf/lo/accept_ra", &vp) == VNODE_PROC_SYS_FILE);
    CHECK(synth_classify("proc/sys/net/ipv6/conf/wlan4", &vp) == VNODE_PROC_SYS_DIR);
    synth_dir_path(VNODE_PROC_SYS_DIR, vp.sysent, 0, path, (int)sizeof(path));
    CHECK(eq(path, "/proc/sys/net/ipv6/conf/wlan4"));
    CHECK(synth_classify("proc/sys/net/ipv6/conf/wlan4/disable_ipv6", &vp) == VNODE_PROC_SYS_FILE &&
          synth_sys_ipv6(vp.sysent, &dir, &key) && eq(dir, "wlan4") && key == SYNTH_IPV6_DISABLE);
    CHECK(synth_classify("proc/sys/net/ipv6/conf/wlan1", &vp) == VNODE_ENOENT);
    CHECK(synth_classify("proc/sys/net/ipv6/conf/eth1/autoconf", &vp) == VNODE_ENOENT);
}

/**
 * The rows of the root, /dev and /proc, then those of `bounds`,
 * `past_a_file`, `past_a_name`, `paths`, `sys_tree`, `sys_class`,
 * `sys_wireless`, `sys_rfkill`, `sys_cpus` and `ipv6_conf`:
 *
 * - the empty path and ordinary names are the KRFFS root's, "devices" and
 *   "proconsul" among them, which only begin as "dev" and "proc" do;
 * - /dev and a device are found;
 * - /proc, /proc/self, a process's directory and its descriptor directory;
 * - /proc/cmdline, the boot line, is a file and not a process;
 * - a process's files;
 * - the descriptor links /proc/PID/fd/N, which ttyname reads;
 * - a process's exe, cwd and root links, the /proc/sys files, and the task
 *   directory, each thread's entry with the process's id and the thread's
 *   and no task directory in it; no thread has id 0;
 * - a name of /proc that is not a process, a file a process does not have,
 *   or a descriptor that is not a number is ENOENT and not the root's, so
 *   no file of the root takes its place;
 * - leading, trailing and doubled slashes are skipped;
 * - a `.` or `..` component is ENOENT, or below /dev ENOTDIR past a device
 *   of that name, which devfs does not have. The caller resolves them before
 *   it classifies, and one that reaches the classifier fails rather than
 *   name the wrong node.
 *
 * Returns the count of failed checks, at most 255.
 */
/** The files of fs.protected_* under /proc/sys/fs, each its own entry and
 * each one a privileged caller may write. */
static void sys_protected(void)
{
    struct vpath vp;

    CHECK(synth_classify("proc/sys/fs/protected_fifos", &vp) == VNODE_PROC_SYS_FILE &&
          vp.sysent == SYNTH_SYS_PROTECTED_FIFOS && synth_sys_writable(vp.sysent));
    CHECK(synth_classify("proc/sys/fs/protected_hardlinks", &vp) == VNODE_PROC_SYS_FILE &&
          vp.sysent == SYNTH_SYS_PROTECTED_FIFOS + 1 && synth_sys_writable(vp.sysent));
    CHECK(synth_classify("proc/sys/fs/protected_regular", &vp) == VNODE_PROC_SYS_FILE &&
          vp.sysent == SYNTH_SYS_PROTECTED_FIFOS + 2 && synth_sys_writable(vp.sysent));
    CHECK(synth_classify("proc/sys/fs/protected_symlinks", &vp) == VNODE_PROC_SYS_FILE &&
          vp.sysent == SYNTH_SYS_PROTECTED_SYMLINKS && synth_sys_writable(vp.sysent));
}

/**
 * The fixed directories drivers add below, /sys/kernel/debug, the PCI
 * function's attribute files and its `subsystem` link to /sys/bus/pci.
 */
static void sys_later(void)
{
    struct vpath vp;
    char path[64];

    CHECK(synth_classify("sys/kernel", &vp) == VNODE_SYS_DIR && vp.sysent == SYSFS_KERNEL);
    CHECK(synth_classify("sys/kernel/debug", &vp) == VNODE_SYS_DIR &&
          vp.sysent == SYSFS_KERNEL_DEBUG);
    CHECK(synth_classify("sys/kernel/debug/dri", &vp) == VNODE_ENOENT);
    CHECK(synth_classify("sys/kernel/uevent_seqnum", &vp) == VNODE_SYS_FILE &&
          vp.sysent == SYSFS_KERNEL);
    CHECK(synth_classify("sys/class/drm", &vp) == VNODE_SYS_DIR && vp.sysent == SYSFS_CLASS_DRM);
    CHECK(synth_classify("sys/class/drm/version", &vp) == VNODE_SYS_FILE &&
          vp.sysent == SYSFS_CLASS_DRM);
    CHECK(synth_classify("sys/class/drm/card0", &vp) == VNODE_ENOENT);
    CHECK(synth_classify("sys/dev/char", &vp) == VNODE_SYS_DIR && vp.sysent == SYSFS_DEV_CHAR);
    CHECK(synth_classify("sys/devices/platform", &vp) == VNODE_SYS_DIR &&
          vp.sysent == SYSFS_PLATFORM);
    CHECK(synth_classify("sys/bus/platform", &vp) == VNODE_SYS_DIR &&
          vp.sysent == SYSFS_BUS_PLATFORM);
    CHECK(synth_classify("sys/bus/platform/devices", &vp) == VNODE_SYS_DIR &&
          vp.sysent == SYSFS_PLATFORM_LINKS);
    synth_sys_path(SYSFS_PLATFORM_LINKS, 0, path, (int)sizeof(path));
    CHECK(eq(path, "/sys/bus/platform/devices"));
    synth_sys_path(SYSFS_KERNEL_DEBUG, 0, path, (int)sizeof(path));
    CHECK(eq(path, "/sys/kernel/debug"));
    CHECK(synth_classify("sys/devices/pci0000:00/0000:00:02.0/vendor", &vp) == VNODE_SYS_FILE &&
          vp.sysent == SYSFS_PCI_DEVICE && vp.syskey == 0x10 && vp.procfile == SYSFS_PCI_VENDOR);
    CHECK(synth_classify("sys/bus/pci/devices/0000:00:02.0/boot_vga", &vp) == VNODE_SYS_FILE &&
          vp.procfile == SYSFS_PCI_BOOT_VGA);
    CHECK(synth_classify("sys/devices/pci0000:00/0000:00:02.0/subsystem_device", &vp) ==
              VNODE_SYS_FILE &&
          vp.procfile == SYSFS_PCI_SUBSYSTEM_DEVICE);
    CHECK(synth_classify("sys/devices/pci0000:00/0000:00:02.0/class/x", &vp) == VNODE_ENOTDIR);
    CHECK(sysfs_attr_writable(SYSFS_PCI_DEVICE, SYSFS_PCI_CONFIG));
    CHECK(sysfs_attr_writable(SYSFS_PCI_DEVICE, SYSFS_PCI_UEVENT) &&
          sysfs_attr_writable(SYSFS_USB_DEVICE, 0) &&
          !sysfs_attr_writable(SYSFS_PCI_DEVICE, SYSFS_PCI_VENDOR));
    CHECK(synth_classify("sys/devices/pci0000:00/0000:00:02.0/subsystem", &vp) == VNODE_SYS_LINK &&
          vp.sysent == SYSFS_BUS_PCI);
    CHECK(synth_classify("sys/devices/pci0000:00/0000:00:02.0/subsystem/devices", &vp) ==
              VNODE_SYS_DIR &&
          vp.sysent == SYSFS_PCI_LINKS);
}

/**
 * A driver's entries: a directory below a PCI function with an attribute
 * and links to a class and back, reached by name, through each link, and
 * through a link to a fixed directory with more path after it; the links
 * read relative to themselves; and once removed, nothing is reached.
 */
static void sys_registry(void)
{
    static const struct sysfs_attr dev = {"dev", 0444u, 0, 0};
    struct vpath vp;
    char path[96];
    int pci = sysfs_anchor(SYSFS_PCI_DEVICE, 0x10), drm_class = sysfs_anchor(SYSFS_CLASS_DRM, 0);
    int card = sysfs_add_dir(sysfs_add_dir(pci, "drm"), "card0");
    int devf = sysfs_add_attr(card, &dev, 0, 0), sub = sysfs_add_link(card, "subsystem", drm_class);
    int up = sysfs_add_link(card, "device", pci), cl = sysfs_add_link(drm_class, "card0", card);
    int ch = sysfs_add_link(sysfs_anchor(SYSFS_DEV_CHAR, 0), "226:0", card);

    CHECK(card > 0 && devf > 0 && sub > 0 && up > 0 && cl > 0 && ch > 0);
    CHECK(sysfs_add_dir(card, "dev") == -EEXIST && sysfs_add_dir(devf, "x") == -ENOENT);
    CHECK(synth_classify("sys/devices/pci0000:00/0000:00:02.0/drm/card0", &vp) == VNODE_SYS_DIR &&
          vp.sysent == SYSFS_REG && vp.syskey == card);
    CHECK(synth_classify("sys/devices/pci0000:00/0000:00:02.0/drm/card0/dev", &vp) ==
              VNODE_SYS_FILE &&
          vp.sysent == SYSFS_REG && vp.syskey == devf);
    CHECK(synth_classify("sys/devices/pci0000:00/0000:00:02.0/drm/card0/dev/x", &vp) ==
          VNODE_ENOTDIR);
    CHECK(synth_classify("sys/devices/pci0000:00/0000:00:02.0/drm/card1", &vp) == VNODE_ENOENT);
    CHECK(synth_classify("sys/class/drm/card0", &vp) == VNODE_SYS_LINK && vp.sysreg == cl &&
          vp.sysent == SYSFS_REG && vp.syskey == card);
    CHECK(synth_classify("sys/class/drm/card0/dev", &vp) == VNODE_SYS_FILE && vp.syskey == devf);
    CHECK(synth_classify("sys/dev/char/226:0/device", &vp) == VNODE_SYS_LINK && vp.sysreg == up &&
          vp.sysent == SYSFS_PCI_DEVICE && vp.syskey == 0x10);
    CHECK(synth_classify("sys/dev/char/226:0/device/vendor", &vp) == VNODE_SYS_FILE &&
          vp.sysent == SYSFS_PCI_DEVICE && vp.procfile == SYSFS_PCI_VENDOR);
    CHECK(synth_classify("sys/dev/char/226:0/device/drm/card0/dev", &vp) == VNODE_SYS_FILE &&
          vp.syskey == devf);
    CHECK(synth_classify("sys/dev/char/226:0/subsystem/card0/subsystem/version", &vp) ==
              VNODE_SYS_FILE &&
          vp.sysent == SYSFS_CLASS_DRM);
    sysfs_node_path(card, path, (int)sizeof(path));
    CHECK(eq(path, "/sys/devices/pci0000:00/0000:00:02.0/drm/card0"));
    path[sysfs_link_text(cl, path, (int)sizeof(path) - 1)] = '\0';
    CHECK(eq(path, "../../devices/pci0000:00/0000:00:02.0/drm/card0"));
    path[sysfs_link_text(up, path, (int)sizeof(path) - 1)] = '\0';
    CHECK(eq(path, "../../../0000:00:02.0"));
    path[sysfs_link_text(sub, path, (int)sizeof(path) - 1)] = '\0';
    CHECK(eq(path, "../../../../../class/drm"));
    sysfs_remove(sysfs_node_parent(card));
    CHECK(synth_classify("sys/devices/pci0000:00/0000:00:02.0/drm", &vp) == VNODE_ENOENT);
    CHECK(synth_classify("sys/class/drm/card0", &vp) == VNODE_ENOENT);
    CHECK(sysfs_link_text(cl, path, (int)sizeof(path)) == -ENOENT);
    sysfs_remove(cl);
    sysfs_remove(ch);
    CHECK(sysfs_node_count(drm_class) == 0 && sysfs_node_kind(cl) == 0);
}

/**
 * The registry past the table's first 512 slots: 1500 attributes below one
 * directory, each reached by name and by path, a handle taken before the
 * table grew still naming its entry, and all gone with the directory.
 */
static void sys_registry_grows(void)
{
    static const struct sysfs_attr attr = {"a", 0444u, 0, 0};
    struct sysfs_attr a = attr;
    struct vpath vp;
    char name[8];
    int dir = sysfs_add_dir(sysfs_anchor(SYSFS_KERNEL, 0), "many"), h[1500], i, ok = 1;

    CHECK(dir > 0);
    for (i = 0; i < 1500; i++) {
        name[0] = 'a';
        name[1] = (char)('0' + i / 1000);
        name[2] = (char)('0' + i / 100 % 10);
        name[3] = (char)('0' + i / 10 % 10);
        name[4] = (char)('0' + i % 10);
        name[5] = '\0';
        a.name = name;
        h[i] = sysfs_add_attr(dir, &a, 0, i);
        ok = ok && h[i] > 0 && sysfs_node_child(dir, name, 5) == h[i];
    }
    CHECK(ok && sysfs_node_count(dir) == 1500);
    CHECK(sysfs_node_kind(dir) == SYSFS_NODE_DIR && eq(sysfs_node_name(h[0]), "a0000") &&
          eq(sysfs_node_name(h[1499]), "a1499"));
    CHECK(synth_classify("sys/kernel/many/a1234", &vp) == VNODE_SYS_FILE &&
          vp.sysent == SYSFS_REG && vp.syskey == h[1234]);
    sysfs_remove(dir);
    CHECK(sysfs_node_kind(dir) == 0 && sysfs_node_kind(h[0]) == 0 && sysfs_node_kind(h[1499]) == 0);
    CHECK(synth_classify("sys/kernel/many/a1234", &vp) == VNODE_ENOENT);
}

/**
 * VFIO's paths: /dev/vfio, its container and group nodes as devices of
 * the directory, and the entries VFIO adds to /sys through the registry,
 * as rach2/sys/vfio/group.c adds them: a group below
 * /sys/kernel/iommu_groups, a function's iommu_group link and
 * driver_override, and vfio-pci's directory below /sys/bus/pci/drivers
 * with drivers_probe beside it.
 */
static void vfio_paths(void)
{
    static const struct sysfs_attr type = {"type", 0444u, 0, 0};
    static const struct sysfs_attr over = {"driver_override", 0644u, 0, 0};
    static const struct sysfs_attr bind = {"bind", 0200u, 0, 0};
    static const struct sysfs_attr probe = {"drivers_probe", 0200u, 0, 0};
    struct vpath vp;
    char out[64];
    int pci = sysfs_anchor(SYSFS_PCI_DEVICE, 0x10);
    int groups = sysfs_add_dir(sysfs_anchor(SYSFS_KERNEL, 0), "iommu_groups");
    int g0 = sysfs_add_dir(groups, "0"), t = sysfs_add_attr(g0, &type, 0, 0);
    int devs = sysfs_add_link(sysfs_add_dir(g0, "devices"), "0000:00:02.0", pci);
    int link = sysfs_add_link(pci, "iommu_group", g0), o = sysfs_add_attr(pci, &over, 0, 0);
    int drv = sysfs_add_dir(sysfs_add_dir(sysfs_anchor(SYSFS_BUS_PCI, 0), "drivers"), "vfio-pci");
    int b = sysfs_add_attr(drv, &bind, 0, 0);
    int p = sysfs_add_attr(sysfs_anchor(SYSFS_BUS_PCI, 0), &probe, 0, 0);

    CHECK(synth_classify("dev/vfio", &vp) == VNODE_DEV_SUBDIR &&
          eq(synth_dev_dir_at(vp.devdir)->name, "vfio"));
    CHECK(synth_classify("dev/vfio/vfio", &vp) == VNODE_DEV && eq(vp.name, "vfio/vfio"));
    CHECK(synth_classify("dev/vfio/0", &vp) == VNODE_DEV && eq(vp.name, "vfio/0"));
    CHECK(synth_dev_dir_find("vfio", 4) == 2 && synth_dev_dir_at(2)->ino == 7u);
    synth_dir_path(VNODE_DEV_SUBDIR, synth_dev_dir_find("vfio", 4), 0, out, (int)sizeof(out));
    CHECK(eq(out, "/dev/vfio"));
    CHECK(t > 0 && devs > 0 && link > 0 && o > 0 && b > 0 && p > 0);
    CHECK(synth_classify("sys/kernel/iommu_groups/0/type", &vp) == VNODE_SYS_FILE &&
          vp.sysent == SYSFS_REG && vp.syskey == t);
    CHECK(synth_classify("sys/kernel/iommu_groups/0/devices/0000:00:02.0", &vp) == VNODE_SYS_LINK &&
          vp.sysent == SYSFS_PCI_DEVICE && vp.syskey == 0x10);
    CHECK(synth_classify("sys/bus/pci/devices/0000:00:02.0/iommu_group", &vp) == VNODE_SYS_LINK &&
          vp.sysreg == link && vp.syskey == g0);
    CHECK(synth_classify("sys/bus/pci/devices/0000:00:02.0/iommu_group/type", &vp) ==
              VNODE_SYS_FILE &&
          vp.syskey == t);
    CHECK(synth_classify("sys/devices/pci0000:00/0000:00:02.0/driver_override", &vp) ==
              VNODE_SYS_FILE &&
          vp.syskey == o);
    CHECK(synth_classify("sys/bus/pci/drivers/vfio-pci/bind", &vp) == VNODE_SYS_FILE &&
          vp.syskey == b);
    CHECK(synth_classify("sys/bus/pci/drivers_probe", &vp) == VNODE_SYS_FILE && vp.syskey == p);
    sysfs_remove(groups);
    sysfs_remove(link);
    sysfs_remove(o);
    CHECK(synth_classify("sys/kernel/iommu_groups/0", &vp) == VNODE_ENOENT &&
          synth_classify("sys/bus/pci/devices/0000:00:02.0/iommu_group", &vp) == VNODE_ENOENT);
}

/** Classify the khugepaged directory below the THP one, its files, which
 * of them root may write, and its path. */
static void khugepaged_rows(void)
{
    static const char *const names[] = {
        "alloc_sleep_millisecs", "defrag",          "full_scans",
        "max_ptes_none",         "max_ptes_shared", "max_ptes_swap",
        "pages_collapsed",       "pages_to_scan",   "scan_sleep_millisecs"};
    static const char prefix[] = "sys/kernel/mm/transparent_hugepage/khugepaged/";
    struct vpath vp;
    char path[128];
    unsigned int i;

    CHECK(synth_classify("sys/kernel/mm/transparent_hugepage/khugepaged", &vp) == VNODE_SYS_DIR &&
          vp.sysent == SYSFS_THP_KHUGEPAGED);
    synth_sys_path(SYSFS_THP_KHUGEPAGED, 0, path, sizeof(path));
    CHECK(eq(path, "/sys/kernel/mm/transparent_hugepage/khugepaged"));
    for (i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        k_memcpy(path, prefix, sizeof(prefix) - 1);
        k_memcpy(path + sizeof(prefix) - 1, names[i], k_strlen(names[i]) + 1);
        CHECK(synth_classify(path, &vp) == VNODE_SYS_FILE && vp.sysent == SYSFS_THP_KHUGEPAGED &&
              vp.procfile == (int)i && eq(sysfs_attr(SYSFS_THP_KHUGEPAGED, (int)i), names[i]));
        CHECK(sysfs_attr_writable(vp.sysent, vp.procfile) ==
              (i != SYSFS_KHP_FULL_SCANS && i != SYSFS_KHP_COLLAPSED));
    }
    CHECK(sysfs_attr(SYSFS_THP_KHUGEPAGED, 9) == 0);
    CHECK(synth_classify("sys/kernel/mm/transparent_hugepage/khugepaged/defrag/x", &vp) ==
          VNODE_ENOTDIR);
    CHECK(synth_classify("sys/kernel/mm/transparent_hugepage/khugepaged/missing", &vp) ==
          VNODE_ENOENT);
}

/** Classify the THP directory, its files, which of them root may write,
 * and paths past a file, and then its khugepaged directory. */
static void thp_rows(void)
{
    static const char *const names[] = {"enabled",        "defrag",        "use_zero_page",
                                        "hpage_pmd_size", "shmem_enabled", "shrink_underused"};
    struct vpath vp;
    char path[128];
    unsigned int i;

    CHECK(synth_classify("sys/kernel/mm", &vp) == VNODE_SYS_DIR && vp.sysent == SYSFS_KERNEL_MM);
    CHECK(synth_classify("sys/kernel/mm/transparent_hugepage", &vp) == VNODE_SYS_DIR &&
          vp.sysent == SYSFS_THP);
    synth_sys_path(SYSFS_THP, 0, path, sizeof(path));
    CHECK(eq(path, "/sys/kernel/mm/transparent_hugepage"));
    for (i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        static const char prefix[] = "sys/kernel/mm/transparent_hugepage/";

        k_memcpy(path, prefix, sizeof(prefix) - 1);
        k_memcpy(path + sizeof(prefix) - 1, names[i], k_strlen(names[i]) + 1);
        CHECK(synth_classify(path, &vp) == VNODE_SYS_FILE && vp.sysent == SYSFS_THP &&
              vp.procfile == (int)i && eq(sysfs_attr(SYSFS_THP, (int)i), names[i]));
        CHECK(sysfs_attr_writable(vp.sysent, vp.procfile) == (i != SYSFS_THP_PMD_SIZE));
    }
    CHECK(sysfs_attr(SYSFS_THP, 6) == 0);
    CHECK(synth_classify("sys/kernel/mm/transparent_hugepage/missing", &vp) == VNODE_ENOENT);
    CHECK(synth_classify("sys/kernel/mm/transparent_hugepage/enabled/more", &vp) == VNODE_ENOTDIR);
    khugepaged_rows();
}

/** Ordinary paths stay in the root file system. */
static void root_paths(void)
{
    struct vpath vp;

    CHECK(synth_classify("", &vp) == VNODE_KRFFS);
    CHECK(synth_classify("hello.txt", &vp) == VNODE_KRFFS);
    CHECK(synth_classify("etc/passwd", &vp) == VNODE_KRFFS);
    CHECK(synth_classify("devices/x", &vp) == VNODE_KRFFS);
    CHECK(synth_classify("proconsul", &vp) == VNODE_KRFFS);
}

/** Run the classifier's rows. */
int main(int argc, char **argv)
{
    struct vpath vp;

    thp_rows();
    if (argc == 2 && eq(argv[1], "--thp-only")) {
        return fails != 0;
    }
    root_paths();

    CHECK(synth_classify("dev", &vp) == VNODE_DEV_DIR);
    CHECK(synth_classify("dev/null", &vp) == VNODE_DEV && eq(vp.name, "null"));
    CHECK(synth_classify("dev/urandom", &vp) == VNODE_DEV && eq(vp.name, "urandom"));

    CHECK(synth_classify("proc", &vp) == VNODE_PROC_DIR);
    CHECK(synth_classify("proc/self", &vp) == VNODE_PROC_PID && vp.pid == SYNTH_PID_SELF);
    CHECK(synth_classify("proc/7", &vp) == VNODE_PROC_PID && vp.pid == 7);
    CHECK(synth_classify("proc/12/fd", &vp) == VNODE_PROC_FD_DIR && vp.pid == 12);

    CHECK(synth_classify("proc/cmdline", &vp) == VNODE_PROC_CMDLINE);
    CHECK(synth_classify("proc/cmdline/x", &vp) == VNODE_ENOTDIR);
    CHECK(synth_classify("proc/cmdlinex", &vp) == VNODE_ENOENT);

    CHECK(synth_classify("proc/7/stat", &vp) == VNODE_PROC_FILE && vp.procfile == SYNTH_PROC_STAT &&
          vp.pid == 7);
    CHECK(synth_classify("proc/9/cmdline", &vp) == VNODE_PROC_FILE &&
          vp.procfile == SYNTH_PROC_CMDLINE);
    CHECK(synth_classify("proc/self/maps", &vp) == VNODE_PROC_FILE &&
          vp.procfile == SYNTH_PROC_MAPS && vp.pid == SYNTH_PID_SELF);
    CHECK(synth_classify("proc/5/status", &vp) == VNODE_PROC_FILE &&
          vp.procfile == SYNTH_PROC_STATUS && vp.pid == 5);
    CHECK(synth_classify("proc/5/statm", &vp) == VNODE_PROC_FILE &&
          vp.procfile == SYNTH_PROC_STATM);
    CHECK(synth_classify("proc/5/smaps", &vp) == VNODE_PROC_FILE &&
          vp.procfile == SYNTH_PROC_SMAPS);
    CHECK(synth_classify("proc/5/auxv", &vp) == VNODE_PROC_FILE && vp.procfile == SYNTH_PROC_AUXV);
    CHECK(synth_classify("proc/5/environ", &vp) == VNODE_PROC_FILE &&
          vp.procfile == SYNTH_PROC_ENVIRON);
    CHECK(synth_classify("proc/5/comm", &vp) == VNODE_PROC_FILE && vp.procfile == SYNTH_PROC_COMM);

    CHECK(synth_classify("proc/self/fd/0", &vp) == VNODE_PROC_FD_LINK && vp.pid == SYNTH_PID_SELF &&
          vp.procfd == 0);
    CHECK(synth_classify("proc/7/fd/3", &vp) == VNODE_PROC_FD_LINK && vp.pid == 7 &&
          vp.procfd == 3);

    CHECK(synth_classify("proc/self/exe", &vp) == VNODE_PROC_EXE && vp.pid == SYNTH_PID_SELF);
    CHECK(synth_classify("proc/self/cwd", &vp) == VNODE_PROC_CWD && vp.pid == SYNTH_PID_SELF);
    CHECK(synth_classify("proc/5/root", &vp) == VNODE_PROC_ROOT && vp.pid == 5);
    CHECK(synth_classify("proc/sys/kernel/hostname", &vp) == VNODE_PROC_SYS_FILE &&
          vp.sysent == SYNTH_SYS_HOSTNAME);
    CHECK(synth_classify("proc/sys/net/ipv4/ip_forward", &vp) == VNODE_PROC_SYS_FILE);
    CHECK(synth_classify("proc/7/task", &vp) == VNODE_PROC_TASK_DIR && vp.pid == 7 && !vp.tid);
    CHECK(synth_classify("proc/7/task/9", &vp) == VNODE_PROC_PID && vp.pid == 7 && vp.tid == 9);
    CHECK(synth_classify("proc/7/task/9/stat", &vp) == VNODE_PROC_FILE &&
          vp.procfile == SYNTH_PROC_STAT && vp.pid == 7 && vp.tid == 9);
    CHECK(synth_classify("proc/7/task/9/comm", &vp) == VNODE_PROC_FILE &&
          vp.procfile == SYNTH_PROC_COMM && vp.pid == 7 && vp.tid == 9);
    CHECK(synth_classify("proc/7/task/9/exe", &vp) == VNODE_PROC_EXE && vp.pid == 7 && vp.tid == 9);
    CHECK(synth_classify("proc/7/task/9/task", &vp) == VNODE_ENOENT);
    CHECK(synth_classify("proc/7/task/x", &vp) == VNODE_ENOENT);
    CHECK(synth_classify("proc/7/task/0", &vp) == VNODE_ENOENT);

    CHECK(synth_classify("proc/abc", &vp) == VNODE_ENOENT);
    CHECK(synth_classify("proc/7/bogus", &vp) == VNODE_ENOENT);
    CHECK(synth_classify("proc/7/fd/xy", &vp) == VNODE_ENOENT);

    CHECK(synth_classify("/dev/null", &vp) == VNODE_DEV && eq(vp.name, "null"));
    CHECK(synth_classify("dev/", &vp) == VNODE_DEV_DIR);
    CHECK(synth_classify("proc/"
                         "/7/"
                         "/stat",
                         &vp) == VNODE_PROC_FILE &&
          vp.pid == 7);

    CHECK(synth_classify("proc/7/../9/stat", &vp) == VNODE_ENOENT);
    CHECK(synth_classify("proc/../etc/passwd", &vp) == VNODE_ENOENT);
    CHECK(synth_classify("dev/../dev/null", &vp) == VNODE_ENOTDIR);
    CHECK(synth_classify("proc/7/.", &vp) == VNODE_ENOENT);
    CHECK(synth_classify("dev/./null", &vp) == VNODE_ENOTDIR);

    bounds();
    past_a_file();
    dev_cpu_rows();
    past_a_name();
    paths();
    sys_tree();
    sys_later();
    sys_protected();
    sys_registry();
    sys_registry_grows();
    sys_class();
    sys_wireless();
    sys_rfkill();
    sys_cpus();
    sys_system();
    ipv6_conf();
    vfio_paths();
    return fails > 255 ? 255 : fails;
}
