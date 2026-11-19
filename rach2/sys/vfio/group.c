/*
 * group.c: VFIO's groups, the PCI functions the IOMMU tells apart from the
 * rest, as Linux's pci_device_group forms them; their nodes /dev/vfio/N and
 * their entries of /sys; the functions given to vfio-pci by driver_override
 * and the driver's bind; and the start of VFIO at boot.
 */

#include "vfio.h"

#include "../fs/devfs.h"
#include "../fs/fd.h"
#include "../fs/file.h"
#include "../fs/synthfs.h"
#include "../fs/sysfs.h"
#include "../iommu.h"
#include "../klog.h"
#include "../lib/kerrno.h"
#include "../lib/kstd.h"
#include "../lib/kstring.h"
#include "../mm/usercopy.h"
#include "../proc/ns.h"
#include "../proc/process.h"
#include "../virtio/virtio.h"

struct vfio_device vfio_devices[VFIO_DEVICES_MAX];
int vfio_ndevices;
struct vfio_group vfio_groups[VFIO_GROUPS_MAX];
int vfio_ngroups;

/** Count live assignment resources for /proc/stat under the big lock. */
void vfio_resource_counts(uint32_t *counts)
{
    int i;
    struct vfio_container *c;
    struct vfio_dma *dma;

    for (i = 0; i < 5; i++)
        counts[i] = 0;
    for (i = 0; i < vfio_ndevices; i++)
        if (vfio_devices[i].refs != 0)
            counts[1]++;
    for (i = 0; i < vfio_ngroups; i++) {
        counts[0] += vfio_groups[i].attached != 0;
        c = vfio_groups[i].container;
        if (c == NULL || c->groups != &vfio_groups[i])
            continue;
        counts[2] += c->ndma;
        for (dma = c->dmas; dma != NULL; dma = dma->next) {
            if (pin_live(dma->pin)) {
                counts[3] += pin_ram_count(dma->pin);
                counts[4] += pin_count(dma->pin) - pin_ram_count(dma->pin);
            }
        }
    }
}

/* The device ids vfio-pci takes beside driver_override, as new_id adds them. */
#define VFIO_IDS_MAX 16
static struct {
    uint16_t vendor, device;
} vfio_ids[VFIO_IDS_MAX];
static int vfio_nids;

/* The PCI Express capability's device type, bits 7:4 of its flags. */
#define PCIE_TYPE_ROOT_PORT   4
#define PCIE_TYPE_UPSTREAM    5
#define PCIE_TYPE_DOWNSTREAM  6
#define PCIE_TYPE_PCI_BRIDGE  7 /* PCI Express to PCI */
#define PCIE_TYPE_PCIE_BRIDGE 8 /* PCI to PCI Express */
#define PCIE_TYPE_RC_EC       10

/* The ACS capability, extended capability 0x0D: its capability and control
 * words, and the controls Linux requires for isolation (REQ_ACS_FLAGS):
 * source validation, request and completion redirect, upstream forwarding. */
#define PCI_EXT_CAP_ID_ACS 0x0D
#define PCI_ACS_CAP        0x04
#define PCI_ACS_CTRL       0x06
#define PCI_ACS_SV         0x0001u
#define PCI_ACS_RR         0x0004u
#define PCI_ACS_CR         0x0008u
#define PCI_ACS_UF         0x0010u
#define PCI_ACS_EC         0x0020u
#define REQ_ACS_FLAGS      (PCI_ACS_SV | PCI_ACS_RR | PCI_ACS_CR | PCI_ACS_UF)

/** Whether the strings `a` and `b` are equal. */
static int str_eq(const char *a, const char *b)
{
    while (*a != '\0' && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

/** The value of the hexadecimal digit `c`, or -1. */
static int hex_value(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    c = (char)(c | 0x20);
    return c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
}

/** The devfn of `dev`. */
static uint8_t devfn_of(const pci_device_t *dev)
{
    return (uint8_t)(dev->device << 3 | dev->function);
}

/** The PCI Express device type of `dev`, or -1 for a function without the
 * capability. */
static int pcie_type(const pci_device_t *dev)
{
    uint8_t cap = pci_find_capability(dev->bus, dev->device, dev->function, PCI_CAP_ID_PCIE);

    if (cap == 0) {
        return -1;
    }
    return (pci_read_config16(dev->bus, dev->device, dev->function, (uint8_t)(cap + 2)) >> 4) & 0xF;
}

/** Whether function 0 of `dev`'s slot says the slot has several. */
static int multifunction(const pci_device_t *dev)
{
    return (pci_read_config8(dev->bus, dev->device, 0, PCI_HEADER_TYPE) & 0x80u) != 0;
}

/** The offset of `dev`'s extended capability `id`, or 0. The walk is
 * bounded, as `pci_find_capability`'s is. */
static uint16_t ext_capability(const pci_device_t *dev, uint16_t id)
{
    uint16_t at = 0x100;
    int hops;

    for (hops = 0; hops < 64 && at >= 0x100 && at <= 0xFFC; hops++) {
        uint32_t h = pci_read_config32_ext(dev->bus, dev->device, dev->function, at);

        if (h == 0 || h == 0xFFFFFFFFu) {
            return 0;
        }
        if ((h & 0xFFFFu) == id) {
            return at;
        }
        at = (uint16_t)((h >> 20) & 0xFFCu);
    }
    return 0;
}

/**
 * Whether `dev` is one of the Intel PCH root ports whose isolation Linux
 * takes on trust (pci_quirk_intel_pch_acs, drivers/pci/quirks.c): they
 * have no ACS capability but do not let peers reach each other. The
 * Lenovo's Lynx Point-LP ports, 0x9c10 to 0x9c1f, are among them.
 */
static int intel_pch_acs(const pci_device_t *dev)
{
    static const uint16_t first[] = {0x3b42, 0x1c10, 0x1e10, 0x8c10, 0x8c90,
                                     0x9c10, 0x9c90, 0x8d10, 0x9cb0, 0x9d10};
    static const uint16_t last[] = {0x3b51, 0x1c1f, 0x1e1f, 0x8c1f, 0x8c9f,
                                    0x9c1f, 0x9c9f, 0x8d1f, 0x9cbf, 0x9d1f};
    unsigned int i;

    if (dev->vendor_id != 0x8086 || pcie_type(dev) != PCIE_TYPE_ROOT_PORT) {
        return 0;
    }
    for (i = 0; i < sizeof(first) / sizeof(first[0]); i++) {
        if (dev->device_id >= first[i] && dev->device_id <= last[i]) {
            return 1;
        }
    }
    return 0;
}

/** Whether `dev` has the controls of REQ_ACS_FLAGS on, as Linux's
 * pci_acs_flags_enabled: a control the capability word lacks is taken as
 * fixed on. */
static int acs_flags_enabled(const pci_device_t *dev)
{
    uint16_t at = ext_capability(dev, PCI_EXT_CAP_ID_ACS);
    uint32_t w;
    uint16_t want;

    if (at == 0) {
        return 0;
    }
    w = pci_read_config32_ext(dev->bus, dev->device, dev->function, at + PCI_ACS_CAP);
    want = (uint16_t)(REQ_ACS_FLAGS & ((w & 0xFFFFu) | PCI_ACS_EC));
    return ((w >> 16) & want) == want;
}

/** Whether `dev` keeps its peers from reaching each other, as Linux's
 * pci_acs_enabled decides it by the device's type. */
static int acs_enabled(const pci_device_t *dev)
{
    int type;

    if (intel_pch_acs(dev)) {
        return 1;
    }
    type = pcie_type(dev);
    switch (type) {
    case -1:
    case PCIE_TYPE_PCIE_BRIDGE:
    case PCIE_TYPE_PCI_BRIDGE:
    case PCIE_TYPE_RC_EC:
        return 0;
    case PCIE_TYPE_DOWNSTREAM:
    case PCIE_TYPE_ROOT_PORT:
        return acs_flags_enabled(dev);
    case 0: /* an endpoint */
    case 1: /* a legacy endpoint */
    case PCIE_TYPE_UPSTREAM:
    case 9: /* a root complex integrated endpoint */
        return multifunction(dev) ? acs_flags_enabled(dev) : 1;
    default:
        return 1;
    }
}

/** Whether every bridge from `dev` up to the root bus isolates its peers,
 * as Linux's pci_acs_path_enabled with no end. */
static int acs_path_enabled(const pci_device_t *dev)
{
    pci_device_t at = *dev, up;
    int hops;

    for (hops = 0; hops < 32; hops++) {
        if (!acs_enabled(&at)) {
            return 0;
        }
        if (at.bus == 0 || !pci_find_bridge_for_bus(at.bus, &up)) {
            return 1;
        }
        up.vendor_id = (uint16_t)pci_read_config16(up.bus, up.device, up.function, PCI_VENDOR_ID);
        up.device_id = (uint16_t)pci_read_config16(up.bus, up.device, up.function, PCI_DEVICE_ID);
        at = up;
    }
    return 0;
}

/** The index of the function at (bus, devfn) in `vfio_devices`, or -1. */
static int device_index(uint8_t bus, uint8_t devfn)
{
    int i;

    for (i = 0; i < vfio_ndevices; i++) {
        if (vfio_devices[i].key == (uint16_t)(bus << 8 | devfn)) {
            return i;
        }
    }
    return -1;
}

/**
 * The function whose group `dev`'s is, as Linux's pci_device_group finds
 * it: up the buses, the first bridge above a bridge that does not isolate
 * its peers; then, for a function of a slot with several that does not
 * isolate them, the lowest function of the slot that does not either; or
 * `dev` itself. Returned in `*root`, as an index of `vfio_devices`.
 */
static int group_root(int i, int depth)
{
    pci_device_t dev = vfio_devices[i].pci, up;
    int f, j;

    if (depth > 32) {
        return i;
    }
    if (dev.bus != 0 && pci_find_bridge_for_bus(dev.bus, &up)) {
        up.vendor_id = pci_read_config16(up.bus, up.device, up.function, PCI_VENDOR_ID);
        up.device_id = pci_read_config16(up.bus, up.device, up.function, PCI_DEVICE_ID);
        if (!acs_path_enabled(&up)) {
            j = device_index(up.bus, devfn_of(&up));
            if (j >= 0) {
                return group_root(j, depth + 1);
            }
        }
    }
    if (multifunction(&dev) && !acs_enabled(&dev)) {
        for (f = 0; f < dev.function; f++) {
            j = device_index(dev.bus, (uint8_t)(dev.device << 3 | f));
            if (j >= 0 && !acs_enabled(&vfio_devices[j].pci)) {
                return group_root(j, depth + 1);
            }
        }
    }
    return i;
}

/** Name `d` as /sys names a function, 0000:BB:DD.F. */
static void device_name(struct vfio_device *d)
{
    static const char hex[] = "0123456789abcdef";
    char *n = d->name;

    k_memcpy(n, "0000:00:00.0", 13);
    n[5] = hex[d->pci.bus >> 4];
    n[6] = hex[d->pci.bus & 15];
    n[8] = hex[d->pci.device >> 4];
    n[9] = hex[d->pci.device & 15];
    n[11] = hex[d->pci.function & 7];
}

/**
 * Find every function in an IOMMU unit's scope and form the groups, as
 * Linux numbers them: the devices of each unit in turn, unit 0 first, in
 * the order of the bus, each group numbered as its first function is met.
 * The number of groups, 0 when no unit translates.
 */
int vfio_groups_build(void)
{
    pci_device_t dev;
    int n, unit, i, root;
    int gid[VFIO_DEVICES_MAX];

    vfio_ndevices = 0;
    vfio_ngroups = 0;
    if (!iommu_present()) {
        return 0;
    }
    for (n = 0; pci_nth(n, &dev) && vfio_ndevices < VFIO_DEVICES_MAX; n++) {
        if (iommu_unit_of(dev.bus, devfn_of(&dev)) < 0) {
            continue;
        }
        k_memset(&vfio_devices[vfio_ndevices], 0, sizeof(vfio_devices[0]));
        vfio_devices[vfio_ndevices].anon.name = "[vfio-device]";
        vfio_devices[vfio_ndevices].pci = dev;
        vfio_devices[vfio_ndevices].key = (uint16_t)(dev.bus << 8 | devfn_of(&dev));
        vfio_devices[vfio_ndevices].irq_type = -1;
        device_name(&vfio_devices[vfio_ndevices]);
        gid[vfio_ndevices] = -1;
        vfio_ndevices++;
    }
    for (unit = 0; unit < 8; unit++) {
        for (i = 0; i < vfio_ndevices; i++) {
            struct vfio_device *d = &vfio_devices[i];
            struct vfio_group *g;

            if (iommu_unit_of(d->pci.bus, devfn_of(&d->pci)) != unit) {
                continue;
            }
            root = group_root(i, 0);
            if (gid[root] < 0) {
                if (vfio_ngroups == VFIO_GROUPS_MAX) {
                    continue;
                }
                gid[root] = vfio_ngroups;
                g = &vfio_groups[vfio_ngroups];
                k_memset(g, 0, sizeof(*g));
                g->id = vfio_ngroups;
                g->unit = unit;
                vfio_ngroups++;
            }
            gid[i] = gid[root];
            g = &vfio_groups[gid[i]];
            d->group = gid[i];
            g->devs[g->ndev++] = i;
        }
    }
    return vfio_ngroups;
}

/** Whether a group may be opened: none of its functions but a bridge has a
 * driver of the kernel, as Linux's vfio_group_viable lets only vfio-pci's
 * functions, bridges and functions without a driver into a group in use. */
int vfio_group_viable(const struct vfio_group *g)
{
    int i;

    for (i = 0; i < g->ndev; i++) {
        const struct vfio_device *d = &vfio_devices[g->devs[i]];
        const char *owner = pci_owner(d->pci.bus, devfn_of(&d->pci));
        int bridge =
            (pci_read_config8(d->pci.bus, d->pci.device, d->pci.function, PCI_HEADER_TYPE) &
             0x7Fu) != 0;

        if (owner != NULL && !d->bound && !bridge) {
            return 0;
        }
    }
    return 1;
}

/** Whether the first `len` bytes of `name` are the whole of `s`. */
static int name_eq(const char *s, const char *name, int len)
{
    int i;

    for (i = 0; i < len; i++) {
        if (s[i] != name[i] || s[i] == '\0') {
            return 0;
        }
    }
    return s[len] == '\0';
}

/** The function named `name`, `len` bytes and maybe a newline, or NULL. */
struct vfio_device *vfio_device_by_name(const char *name, int len)
{
    int i;

    while (len > 0 && (name[len - 1] == '\n' || name[len - 1] == ' ')) {
        len--;
    }
    for (i = 0; i < vfio_ndevices; i++) {
        if (name_eq(vfio_devices[i].name, name, len)) {
            return &vfio_devices[i];
        }
    }
    return NULL;
}

int vfio_override_allows(int key, const char *driver)
{
    int i;

    for (i = 0; i < vfio_ndevices; i++) {
        if (vfio_devices[i].key == key) {
            return vfio_devices[i].override[0] == '\0' ||
                   name_eq(vfio_devices[i].override, driver, (int)k_strlen(driver));
        }
    }
    return 1;
}

/* The handles of the entries of /sys that every function shares. */
static int sys_groups; /* /sys/kernel/iommu_groups */
static int sys_driver; /* /sys/bus/pci/drivers/vfio-pci */

/** Make /sys show `d` bound: its `driver` link and the driver's link to it. */
static void sys_show_bound(struct vfio_device *d)
{
    int pci = sysfs_anchor(SYSFS_PCI_DEVICE, d->key);

    if (pci > 0 && sys_driver > 0) {
        d->sys_driver = sysfs_add_link(pci, "driver", sys_driver);
        d->sys_drvlink = sysfs_add_link(sys_driver, d->name, pci);
    }
}

int vfio_bind(struct vfio_device *d)
{
    uint8_t devfn = devfn_of(&d->pci);
    int r;

    if (d->bound) {
        return -EBUSY;
    }
    if ((pci_read_config8(d->pci.bus, d->pci.device, d->pci.function, PCI_HEADER_TYPE) & 0x7Fu) !=
        0) {
        return -EINVAL; /* a bridge, as Linux's vfio_pci_probe refuses */
    }
    r = pci_claim(d->pci.bus, devfn, VFIO_DRIVER);
    if (r != 0) {
        klog(KLOG_WARN, "vfio", "bind: %s: held by %s", d->name,
             pci_owner(d->pci.bus, devfn) ? pci_owner(d->pci.bus, devfn) : "?");
        return r;
    }
    d->bound = 1;
    sys_show_bound(d);
    klog(KLOG_INFO, "vfio", "bind: %s to vfio-pci, group %d", d->name, d->group);
    return 0;
}

int vfio_unbind(struct vfio_device *d)
{
    if (!d->bound) {
        return -ENODEV;
    }
    if (d->refs != 0) {
        return -EBUSY;
    }
    pci_unclaim(d->pci.bus, devfn_of(&d->pci), VFIO_DRIVER);
    d->bound = 0;
    if (d->sys_driver > 0) {
        sysfs_remove(d->sys_driver);
    }
    if (d->sys_drvlink > 0) {
        sysfs_remove(d->sys_drvlink);
    }
    d->sys_driver = 0;
    d->sys_drvlink = 0;
    klog(KLOG_INFO, "vfio", "unbind: %s", d->name);
    return 0;
}

/** Whether vfio-pci takes `d` by Linux's match: its driver_override names
 * vfio-pci, or it has no override and its ids were given by new_id. */
static int vfio_matches(const struct vfio_device *d)
{
    int i;

    if (d->override[0] != '\0') {
        return str_eq(d->override, VFIO_DRIVER);
    }
    for (i = 0; i < vfio_nids; i++) {
        if (vfio_ids[i].vendor == d->pci.vendor_id &&
            (vfio_ids[i].device == 0xFFFFu || vfio_ids[i].device == d->pci.device_id)) {
            return 1;
        }
    }
    return 0;
}

/* ---- /sys ---- */

/* What a show writes, as drm_sysfs.c's: the bytes so far, which may pass
 * `cap`. */
struct emit {
    char *buf;
    uint32_t cap, len;
};

/** Append the string `s` to `e`, as far as its room goes. */
static void emit_str(struct emit *e, const char *s)
{
    for (; *s != '\0'; s++, e->len++) {
        if (e->len < e->cap) {
            e->buf[e->len] = *s;
        }
    }
}

/** Append `v` to `e` as 0x and 16 hexadecimal digits. */
static void emit_hex64(struct emit *e, uint64_t v)
{
    static const char hex[] = "0123456789abcdef";
    char text[19];
    int i;

    text[0] = '0';
    text[1] = 'x';
    for (i = 0; i < 16; i++) {
        text[2 + i] = hex[(v >> (60 - 4 * i)) & 15];
    }
    text[18] = '\0';
    emit_str(e, text);
}

/** /sys/kernel/iommu_groups/N/type: the host's domain of the group,
 * identity, since the host's devices keep physical DMA addresses. */
static int32_t show_type(void *obj, int arg, char *buf, uint32_t cap)
{
    struct emit e;

    (void)obj;
    (void)arg;
    e.buf = buf;
    e.cap = cap;
    e.len = 0;
    emit_str(&e, "identity\n");
    return (int32_t)e.len;
}

/**
 * /sys/kernel/iommu_groups/N/reserved_regions: the regions of the group's
 * functions, as Linux's iommu_group_show_resv_regions writes them, each
 * once, sorted by start: start, end and kind on a line.
 */
static int32_t show_resv(void *obj, int arg, char *buf, uint32_t cap)
{
    static const char *const kind[] = {"direct", "direct-relaxable", "msi"};
    const struct vfio_group *g = &vfio_groups[arg];
    struct iommu_region all[32], one[16];
    struct emit e;
    int n = 0, i, j, k, m;

    (void)obj;
    for (i = 0; i < g->ndev; i++) {
        const struct vfio_device *d = &vfio_devices[g->devs[i]];

        m = iommu_reserved_regions(d->pci.bus, devfn_of(&d->pci), one, 16);
        for (j = 0; j < m && n < 32; j++) {
            for (k = 0; k < n; k++) {
                if (all[k].start == one[j].start && all[k].end == one[j].end &&
                    all[k].type == one[j].type) {
                    break;
                }
            }
            if (k == n) {
                all[n++] = one[j];
            }
        }
    }
    for (i = 1; i < n; i++) {
        struct iommu_region t = all[i];

        for (j = i; j > 0 && all[j - 1].start > t.start; j--) {
            all[j] = all[j - 1];
        }
        all[j] = t;
    }
    e.buf = buf;
    e.cap = cap;
    e.len = 0;
    for (i = 0; i < n; i++) {
        emit_hex64(&e, all[i].start);
        emit_str(&e, " ");
        emit_hex64(&e, all[i].end);
        emit_str(&e, " ");
        emit_str(&e, all[i].type >= 0 && all[i].type <= 2 ? kind[all[i].type] : "reserved");
        emit_str(&e, "\n");
    }
    return (int32_t)e.len;
}

/** driver_override: the driver named, or "(null)" for none, as Linux. */
static int32_t show_override(void *obj, int arg, char *buf, uint32_t cap)
{
    const struct vfio_device *d = &vfio_devices[arg];
    struct emit e;

    (void)obj;
    e.buf = buf;
    e.cap = cap;
    e.len = 0;
    emit_str(&e, d->override[0] != '\0' ? d->override : "(null)");
    emit_str(&e, "\n");
    return (int32_t)e.len;
}

/** A write of driver_override: the name, less a newline; an empty write
 * clears it. -EINVAL for a name past 31 bytes. */
static int32_t store_override(void *obj, int arg, const char *buf, uint32_t len)
{
    struct vfio_device *d = &vfio_devices[arg];
    uint32_t n = len;

    (void)obj;
    if (n > 0 && buf[n - 1] == '\n') {
        n--;
    }
    if (n >= sizeof(d->override)) {
        return -EINVAL;
    }
    k_memcpy(d->override, buf, n);
    d->override[n] = '\0';
    return (int32_t)len;
}

/** /sys/bus/pci/drivers/vfio-pci/bind: give the function named to
 * vfio-pci, if it matches; -ENODEV for one that is unknown or does not
 * match, -EBUSY for one a driver has. */
static int32_t store_bind(void *obj, int arg, const char *buf, uint32_t len)
{
    struct vfio_device *d = vfio_device_by_name(buf, (int)len);
    int r;

    (void)obj;
    (void)arg;
    if (d == NULL || !vfio_matches(d)) {
        return -ENODEV;
    }
    r = vfio_bind(d);
    return r != 0 ? r : (int32_t)len;
}

/** /sys/bus/pci/drivers/vfio-pci/unbind: take the function back. */
static int32_t store_unbind(void *obj, int arg, const char *buf, uint32_t len)
{
    struct vfio_device *d = vfio_device_by_name(buf, (int)len);
    int r;

    (void)obj;
    (void)arg;
    if (d == NULL) {
        return -ENODEV;
    }
    r = vfio_unbind(d);
    return r != 0 ? r : (int32_t)len;
}

/** /sys/bus/pci/drivers_probe: give the function named, if it has no
 * driver, to vfio-pci when vfio-pci matches it, and else to virtio-pci,
 * which takes a virtio function whose driver_override allows it
 * (`virtio_pci_bind`); a function no driver takes is left alone, as on
 * Linux, and the write succeeds. */
static int32_t store_probe(void *obj, int arg, const char *buf, uint32_t len)
{
    struct vfio_device *d = vfio_device_by_name(buf, (int)len);

    (void)obj;
    (void)arg;
    if (d == NULL) {
        return -ENODEV;
    }
    if (d->bound || pci_owner(d->pci.bus, devfn_of(&d->pci)) != NULL) {
        return (int32_t)len;
    }
    if (vfio_matches(d)) {
        (void)vfio_bind(d);
    } else {
        (void)virtio_pci_bind(d->pci.bus, devfn_of(&d->pci));
    }
    return (int32_t)len;
}

/** Read up to two hexadecimal words of `buf` into `w`: their count. */
static int parse_ids(const char *buf, uint32_t len, uint32_t *w)
{
    uint32_t i = 0;
    int n = 0;

    while (i < len && n < 2) {
        uint32_t v = 0;
        int digits = 0;

        while (i < len && (buf[i] == ' ' || buf[i] == '\n' || buf[i] == '\t')) {
            i++;
        }
        while (i < len && hex_value(buf[i]) >= 0 && digits < 8) {
            v = v * 16u + (uint32_t)hex_value(buf[i++]);
            digits++;
        }
        if (digits == 0) {
            break;
        }
        w[n++] = v;
    }
    return n;
}

/** new_id: "VENDOR DEVICE" in hexadecimal; then every function of those
 * ids with no driver is given to vfio-pci, as Linux's driver_attach. */
static int32_t store_new_id(void *obj, int arg, const char *buf, uint32_t len)
{
    uint32_t w[2];
    int i;

    (void)obj;
    (void)arg;
    if (parse_ids(buf, len, w) != 2 || w[0] > 0xFFFFu || w[1] > 0xFFFFu) {
        return -EINVAL;
    }
    if (vfio_nids == VFIO_IDS_MAX) {
        return -ENOSPC;
    }
    vfio_ids[vfio_nids].vendor = (uint16_t)w[0];
    vfio_ids[vfio_nids].device = (uint16_t)w[1];
    vfio_nids++;
    for (i = 0; i < vfio_ndevices; i++) {
        struct vfio_device *d = &vfio_devices[i];

        if (!d->bound && vfio_matches(d) && pci_owner(d->pci.bus, devfn_of(&d->pci)) == NULL) {
            (void)vfio_bind(d);
        }
    }
    return (int32_t)len;
}

/** remove_id: forget ids that new_id gave. -ENODEV for ids not given. */
static int32_t store_remove_id(void *obj, int arg, const char *buf, uint32_t len)
{
    uint32_t w[2];
    int i;

    (void)obj;
    (void)arg;
    if (parse_ids(buf, len, w) != 2) {
        return -EINVAL;
    }
    for (i = 0; i < vfio_nids; i++) {
        if (vfio_ids[i].vendor == w[0] && vfio_ids[i].device == w[1]) {
            vfio_ids[i] = vfio_ids[--vfio_nids];
            return (int32_t)len;
        }
    }
    return -ENODEV;
}

/**
 * Whether the write `buf`, `len` bytes, is the word `word`, as Linux's
 * sysfs_streq compares them: the write ends at its first NUL, and either
 * may have one newline more at its end.
 */
static int write_is(const char *buf, uint32_t len, const char *word)
{
    uint32_t i = 0;

    while (i < len && buf[i] != '\0' && buf[i] == word[i]) {
        i++;
    }
    if (i == len || buf[i] == '\0') {
        return word[i] == '\0' || (word[i] == '\n' && word[i + 1] == '\0');
    }
    return buf[i] == '\n' && word[i] == '\0' && (i + 1 == len || buf[i + 1] == '\0');
}

/**
 * Whether the group `g` has a DMA owner, Linux's owner_cnt, which a change
 * of its type waits out: a function with a driver of the kernel, as
 * really_probe makes every driver but vfio-pci one, or the group in a
 * container, which VFIO_GROUP_SET_CONTAINER claims as Linux's
 * iommu_group_claim_dma_owner does.
 */
static int group_owned(const struct vfio_group *g)
{
    int i;

    if (g->container) {
        return 1;
    }
    for (i = 0; i < g->ndev; i++) {
        const struct vfio_device *d = &vfio_devices[g->devs[i]];

        if (!d->bound && pci_owner(d->pci.bus, devfn_of(&d->pci)) != NULL) {
            return 1;
        }
    }
    return 0;
}

/**
 * /sys/kernel/iommu_groups/N/type, written: the group's default domain, as
 * Linux's iommu_group_store_type sets it. A writer without CAP_SYS_ADMIN
 * and CAP_SYS_RAWIO in the first user namespace is -EACCES, and a word
 * other than "identity", "DMA", "DMA-FQ" and "auto" -EINVAL. A group with
 * no function or with a DMA owner (`group_owned`) is then -EPERM. The
 * host's drivers keep physical DMA addresses, as under a Linux built
 * without CONFIG_IOMMU_DMA, whose iommu_get_default_domain_type gives
 * identity alone: "identity" and "auto" keep it, and "DMA" and "DMA-FQ"
 * are -EINVAL.
 */
static int32_t store_type(void *obj, int arg, const char *buf, uint32_t len)
{
    const struct vfio_group *g = &vfio_groups[arg];
    int keep;

    (void)obj;
    if (current_proc->userns || !cap_holds(current_proc, CAP_SYS_ADMIN) ||
        !cap_holds(current_proc, CAP_SYS_RAWIO)) {
        return -EACCES;
    }
    keep = write_is(buf, len, "identity") || write_is(buf, len, "auto");
    if (!keep && !write_is(buf, len, "DMA") && !write_is(buf, len, "DMA-FQ")) {
        return -EINVAL;
    }
    if (g->ndev == 0 || group_owned(g)) {
        return -EPERM;
    }
    return keep ? (int32_t)len : -EINVAL;
}

static const struct sysfs_attr attr_type = {"type", 0644u, show_type, store_type};
static const struct sysfs_attr attr_resv = {"reserved_regions", 0444u, show_resv, NULL};
static const struct sysfs_attr attr_override = {"driver_override", 0644u, show_override,
                                                store_override};
static const struct sysfs_attr attr_bind = {"bind", 0200u, NULL, store_bind};
static const struct sysfs_attr attr_unbind = {"unbind", 0200u, NULL, store_unbind};
static const struct sysfs_attr attr_new_id = {"new_id", 0200u, NULL, store_new_id};
static const struct sysfs_attr attr_remove_id = {"remove_id", 0200u, NULL, store_remove_id};
static const struct sysfs_attr attr_probe = {"drivers_probe", 0200u, NULL, store_probe};

/** The directory `name` below `parent`, made when it is not there yet. */
static int dir_at(int parent, const char *name)
{
    int h = sysfs_node_child(parent, name, (int)k_strlen(name));

    return h > 0 ? h : sysfs_add_dir(parent, name);
}

/**
 * Add VFIO's entries to /sys: /sys/kernel/iommu_groups/N with its type,
 * reserved_regions and devices; each function's iommu_group link and
 * driver_override; /sys/bus/pci/drivers/vfio-pci with bind, unbind,
 * new_id and remove_id; and /sys/bus/pci/drivers_probe. 0, or the first
 * error, with what was made kept.
 */
int vfio_sysfs_start(void)
{
    char num[12];
    int i, j, devs, pci, bus, drivers;

    sys_groups = dir_at(sysfs_anchor(SYSFS_KERNEL, 0), "iommu_groups");
    if (sys_groups < 0) {
        return sys_groups;
    }
    for (i = 0; i < vfio_ngroups; i++) {
        struct vfio_group *g = &vfio_groups[i];

        (void)k_itoa(g->id, num, sizeof(num), 'd', false);
        g->sys_dir = sysfs_add_dir(sys_groups, num);
        if (g->sys_dir < 0) {
            return g->sys_dir;
        }
        (void)sysfs_add_attr(g->sys_dir, &attr_type, NULL, i);
        (void)sysfs_add_attr(g->sys_dir, &attr_resv, NULL, i);
        devs = sysfs_add_dir(g->sys_dir, "devices");
        for (j = 0; j < g->ndev; j++) {
            struct vfio_device *d = &vfio_devices[g->devs[j]];

            pci = sysfs_anchor(SYSFS_PCI_DEVICE, d->key);
            if (pci <= 0 || devs <= 0) {
                continue;
            }
            (void)sysfs_add_link(devs, d->name, pci);
            d->sys_link = sysfs_add_link(pci, "iommu_group", g->sys_dir);
            d->sys_override = sysfs_add_attr(pci, &attr_override, NULL, g->devs[j]);
        }
    }
    bus = sysfs_anchor(SYSFS_BUS_PCI, 0);
    drivers = dir_at(bus, "drivers");
    sys_driver = drivers > 0 ? sysfs_add_dir(drivers, VFIO_DRIVER) : drivers;
    if (sys_driver < 0) {
        return sys_driver;
    }
    (void)sysfs_add_attr(sys_driver, &attr_bind, NULL, 0);
    (void)sysfs_add_attr(sys_driver, &attr_unbind, NULL, 0);
    (void)sysfs_add_attr(sys_driver, &attr_new_id, NULL, 0);
    (void)sysfs_add_attr(sys_driver, &attr_remove_id, NULL, 0);
    (void)sysfs_add_attr(bus, &attr_probe, NULL, 0);
    return 0;
}

/* ---- /dev/vfio ---- */

static int32_t group_open(struct devfs_node *node, struct fd_entry *e, int flags);
static int32_t group_ioctl(fd_entry_t *e, uint32_t request, uintptr_t arg);
static void group_close(fd_entry_t *e);

static const struct file_ops group_ops = {NULL,        NULL, NULL, group_ioctl,
                                          group_close, NULL, NULL, 0};

int vfio_file_is_valid(const struct fd_entry *e)
{
    return e && ((e->type == FD_TYPE_DEVICE && e->ops == &group_ops && e->priv) ||
                 vfio_device_file_is_valid(e));
}

/* The group's node, /dev/vfio/N, VFIO's major and minor N, 0600 as udev's
 * default rule gives it. */
#define VFIO_GROUP_MAJOR 242

static struct devfs_node group_nodes[VFIO_GROUPS_MAX];

/* /dev/vfio/vfio, the misc device 10:196, 0666 as Linux has it. */
static int32_t container_node_open(struct devfs_node *node, struct fd_entry *e, int flags);
static struct devfs_node container_node = {
    "vfio/vfio", 10, 196, 0666, &vfio_container_ops, container_node_open, NULL, 0};

/** An open of /dev/vfio/vfio makes a container. */
static int32_t container_node_open(struct devfs_node *node, struct fd_entry *e, int flags)
{
    return vfio_container_open(node, e, flags);
}

/** An open of /dev/vfio/N: -EBUSY while the group is open, as Linux allows
 * one open at a time. */
static int32_t group_open(struct devfs_node *node, struct fd_entry *e, int flags)
{
    struct vfio_group *g = (struct vfio_group *)node->priv;

    (void)flags;
    if (g->open || g->dev_open != 0) {
        return -EBUSY;
    }
    g->open = 1;
    e->priv = g;
    return 0;
}

/** The group goes out of its container once neither its descriptor nor a
 * device of it is open; a device holds its group, as on Linux. */
static void group_drop(struct vfio_group *g)
{
    if (!g->open && g->dev_open == 0 && g->container != NULL) {
        vfio_container_detach(g);
    }
}

void vfio_group_put_device(struct vfio_group *g)
{
    if (g->dev_open > 0) {
        g->dev_open--;
    }
    group_drop(g);
}

/** The last close of a group's descriptor. */
static void group_close(fd_entry_t *e)
{
    struct vfio_group *g = (struct vfio_group *)e->priv;

    if (g != NULL) {
        g->open = 0;
        group_drop(g);
    }
    e->priv = NULL;
}

/** VFIO_GROUP_GET_DEVICE_FD: a descriptor on the device the string at
 * `arg` names, which must be of the group and given to vfio-pci, with
 * the group in a container whose IOMMU is set. */
static int32_t get_device_fd(struct vfio_group *g, uintptr_t arg)
{
    char name[32];
    struct vfio_device *d;
    int32_t n;

    n = strncpy_from_user(name, (const char __user *)arg, sizeof(name));
    if (n == -ENAMETOOLONG) {
        return -ENODEV; /* longer than any device's name */
    }
    if (n < 0) {
        return n;
    }
    d = vfio_device_by_name(name, (int)n);
    if (d == NULL || &vfio_groups[d->group] != g || !d->bound) {
        return -ENODEV;
    }
    if (g->container == NULL || g->container->iommu_type == 0) {
        return -EINVAL;
    }
    return vfio_device_open_fd(d);
}

/** The ioctls of a group. */
static int32_t group_ioctl(fd_entry_t *e, uint32_t request, uintptr_t arg)
{
    struct vfio_group *g = (struct vfio_group *)e->priv;
    struct vfio_group_status st;
    struct vfio_container *c;
    int32_t fd;

    switch (request) {
    case VFIO_GROUP_GET_STATUS:
        if (copy_from_user(&st, (const void __user *)arg, sizeof(st)) != 0) {
            return -EFAULT;
        }
        if (st.argsz < sizeof(st)) {
            return -EINVAL;
        }
        st.flags = (vfio_group_viable(g) ? VFIO_GROUP_FLAGS_VIABLE : 0u) |
                   (g->container != NULL ? VFIO_GROUP_FLAGS_CONTAINER_SET : 0u);
        return copy_to_user((void __user *)arg, &st, sizeof(st)) != 0 ? -EFAULT : 0;
    case VFIO_GROUP_SET_CONTAINER:
        if (copy_from_user(&fd, (const void __user *)arg, sizeof(fd)) != 0) {
            return -EFAULT;
        }
        if (g->container != NULL) {
            return -EINVAL;
        }
        c = (struct vfio_container *)vfio_hw_fd_object(fd, &vfio_container_ops);
        if (c == NULL) {
            return -EBADF;
        }
        if (!vfio_group_viable(g)) {
            return -EPERM;
        }
        return vfio_container_attach(c, g);
    case VFIO_GROUP_UNSET_CONTAINER:
        if (g->container == NULL) {
            return -EINVAL;
        }
        if (g->dev_open != 0) {
            return -EBUSY;
        }
        vfio_container_detach(g);
        return 0;
    case VFIO_GROUP_GET_DEVICE_FD:
        return get_device_fd(g, arg);
    default:
        return -ENOTTY;
    }
}

/** Register /dev/vfio/vfio and each group's /dev/vfio/N. 0, or the first
 * error of the registry. */
int vfio_nodes_start(void)
{
    int i, r;

    r = devfs_node_register(&container_node);
    if (r != 0) {
        return r;
    }
    for (i = 0; i < vfio_ngroups; i++) {
        struct vfio_group *g = &vfio_groups[i];
        struct devfs_node *n = &group_nodes[i];

        k_memcpy(g->node_name, "vfio/", 5);
        (void)k_itoa(g->id, g->node_name + 5, sizeof(g->node_name) - 5, 'd', false);
        n->name = g->node_name;
        n->major = VFIO_GROUP_MAJOR;
        n->minor = (uint16_t)g->id;
        n->mode = 0600;
        n->ops = &group_ops;
        n->open = group_open;
        n->priv = g;
        g->node = n;
        r = devfs_node_register(n);
        if (r != 0) {
            return r;
        }
    }
    return 0;
}

/**
 * Start VFIO: form the groups of the IOMMU's units, and add the container's
 * node, each group's and VFIO's entries of /sys. Called once at boot, after
 * the IOMMU and the PCI bus are up. With no unit translating there are no
 * groups, but /dev/vfio/vfio, /sys/kernel/iommu_groups and vfio-pci's
 * directory are there, as on Linux.
 */
void vfio_start(void)
{
    int n = vfio_groups_build();
    int r = vfio_nodes_start();

    if (r == 0) {
        r = vfio_sysfs_start();
    }
    if (r != 0) {
        klog(KLOG_WARN, "vfio", "failed: %d", (int)r);
        return;
    }
    klog(KLOG_INFO, "vfio", "ready: %d groups of %d functions%s", n, vfio_ndevices,
         n == 0                 ? ", no IOMMU unit translates"
         : iommu_irq_remapped() ? ""
                                : ", interrupts not remapped");
}
