/*
 * pcihp.c: PCI hot plug through ACPI on the root bus, as Linux's acpiphp
 * takes it: a card put in a slot is listed, given addresses and bound, and
 * one the firmware asks to eject is let go of before its _EJ0 runs.
 */

#include "../klog.h"
#include "../lib/kerrno.h"
#include "../pci.h"
#include "../fs/sysfs.h"
#include "../virtio/virtio.h"

#include "acpi.h"

#include "devices.h"

/* The slots kept: a bus has 32. */
#define PCIHP_SLOTS 32

/* A slot: its device in the namespace and its device number on bus 0. */
struct pcihp_slot {
    ACPI_HANDLE handle;
    uint8_t device;
};

static struct pcihp_slot slots[PCIHP_SLOTS];
static int nslots;
static int root0; /* bus 0's first host bridge was found and its slots taken */

/** The function's key in /sys, bus << 8 | devfn. */
static int pcihp_key(uint8_t device, uint8_t function)
{
    return device << 3 | function;
}

/**
 * A resource of a host bridge's _CRS: each range of memory or ports an
 * address descriptor passes to its bus is kept for the root bus that
 * `context` points to, as Linux's acpi_pci_root_create reads the windows;
 * a range of bus numbers, and memory below 1 MiB, which the legacy VGA
 * window is, are not kept.
 */
static ACPI_STATUS pcihp_window(ACPI_RESOURCE *res, void *context)
{
    struct pci_aperture w;
    uint64_t start, end;
    int address, kind = acpi_resource_range(res, &start, &end, &address);

    if (!address || (kind != ACPI_RANGE_MEMORY && kind != ACPI_RANGE_IO) ||
        (kind == ACPI_RANGE_MEMORY && start < 0x100000u)) {
        return AE_OK;
    }
    w.start = start;
    w.end = end;
    w.io = kind == ACPI_RANGE_IO;
    (void)pci_root_window_add(*(const uint8_t *)context, &w);
    return AE_OK;
}

/** A resource of a host bridge's _CRS: a range of bus numbers in a Word,
 * DWord or QWord descriptor puts its first bus in the UINT64 `context`
 * points to, the last such range winning, as Linux's
 * get_root_bridge_busnr_callback reads them. */
static ACPI_STATUS pcihp_busnr(ACPI_RESOURCE *res, void *context)
{
    uint64_t start, end;
    int address;

    if (res->Type != ACPI_RESOURCE_TYPE_EXTENDED_ADDRESS64 &&
        acpi_resource_range(res, &start, &end, &address) == ACPI_RANGE_BUS) {
        *(UINT64 *)context = start;
    }
    return AE_OK;
}

/**
 * Bring up the functions of slot `s` the list lacks, as acpiphp's
 * enable_slot does on a Device Check: each is listed, its BARs given
 * addresses in the host bridge's windows, announced with an "add" uevent,
 * and given to virtio-pci, the driver that takes a function after boot. A
 * function whose BARs fit nowhere is listed and given to no driver.
 */
static void pcihp_check(const struct pcihp_slot *s)
{
    const struct pci_aperture *ap;
    pci_device_t p;
    uint8_t devfn;
    int i, n, rc;

    if (pci_slot_add(0, s->device) == 0) {
        return;
    }
    for (i = 0; pci_nth(i, &p); i++) {
        if (p.bus != 0 || p.device != s->device ||
            pci_owner(0, (uint8_t)(p.device << 3 | p.function)) != NULL) {
            continue;
        }
        devfn = (uint8_t)(p.device << 3 | p.function);
        n = pci_root_windows(0, &ap);
        rc = pci_assign_bars(0, p.device, p.function, ap, n);
        klog(KLOG_INFO, "pci", "hotplug: added: 00:%02x.%d %04x:%04x%s", p.device, p.function,
             p.vendor_id, p.device_id, rc != 0 ? ", without addresses" : "");
        (void)sysfs_pci_uevent(pcihp_key(p.device, p.function), "add");
        if (rc == 0) {
            (void)virtio_pci_bind(0, devfn);
        }
    }
}

/**
 * Eject the card in slot `s`, as acpiphp's disable_slot and eject do on an
 * Eject Request: virtio-pci lets go of each function it has, each
 * function's decoding is turned off, it is announced with a "remove"
 * uevent and taken off the list, and then the slot's _EJ0 tells the
 * firmware the card may go. A function another driver has, which cannot
 * let go of it, keeps the card in, with a record.
 */
static void pcihp_eject(const struct pcihp_slot *s)
{
    static char ej0[] = "_EJ0";
    ACPI_OBJECT arg;
    ACPI_OBJECT_LIST args;
    ACPI_STATUS status;
    pci_device_t p;
    const char *owner;
    uint8_t devfn;
    int i;

    for (i = 0; pci_nth(i, &p); i++) {
        if (p.bus != 0 || p.device != s->device) {
            continue;
        }
        devfn = (uint8_t)(p.device << 3 | p.function);
        (void)virtio_pci_unbind(0, devfn);
        owner = pci_owner(0, devfn);
        if (owner != NULL) {
            klog(KLOG_WARN, "pci", "hotplug: kept: 00:%02x.%d: %s cannot let go of it", p.device,
                 p.function, owner);
            return;
        }
    }
    for (i = 0; pci_nth(i, &p); i++) {
        if (p.bus == 0 && p.device == s->device) {
            pci_write_config16(0, p.device, p.function, PCI_COMMAND, 0);
            (void)sysfs_pci_uevent(pcihp_key(p.device, p.function), "remove");
            klog(KLOG_INFO, "pci", "hotplug: removed: 00:%02x.%d", p.device, p.function);
        }
    }
    (void)pci_slot_remove(0, s->device);
    arg.Type = ACPI_TYPE_INTEGER;
    arg.Integer.Value = 1;
    args.Count = 1;
    args.Pointer = &arg;
    status = AcpiEvaluateObject(s->handle, ej0, &args, NULL);
    if (ACPI_FAILURE(status)) {
        klog(KLOG_WARN, "pci", "hotplug: eject: slot %d: %s", s->device,
             AcpiFormatException(status));
        return;
    }
    klog(KLOG_INFO, "pci", "hotplug: ejected: slot %d", s->device);
}

/** A slot's Notify, on kacpid with the big lock held: a Bus or Device
 * Check looks for a card, an Eject Request ejects it. */
static void pcihp_notify(ACPI_HANDLE handle, UINT32 value, void *context)
{
    const struct pcihp_slot *s = (const struct pcihp_slot *)context;

    (void)handle;
    if (value == ACPI_NOTIFY_BUS_CHECK || value == ACPI_NOTIFY_DEVICE_CHECK) {
        pcihp_check(s);
    } else if (value == ACPI_NOTIFY_EJECT_REQUEST) {
        pcihp_eject(s);
    }
}

/** Take each child of the host bridge with _ADR and _EJ0 as a slot, as
 * acpiphp's acpiphp_add_context does, and its Notify. */
static void pcihp_slots(ACPI_HANDLE root)
{
    static char adr[] = "_ADR", ej0[] = "_EJ0";
    ACPI_HANDLE child = NULL, method;
    ACPI_BUFFER buf;
    ACPI_OBJECT obj;
    struct pcihp_slot *s;

    while (nslots < PCIHP_SLOTS &&
           ACPI_SUCCESS(AcpiGetNextObject(ACPI_TYPE_DEVICE, root, child, &child))) {
        buf.Length = sizeof(obj);
        buf.Pointer = &obj;
        if (ACPI_FAILURE(AcpiGetHandle(child, ej0, &method)) ||
            ACPI_FAILURE(AcpiEvaluateObjectTyped(child, adr, NULL, &buf, ACPI_TYPE_INTEGER)) ||
            (obj.Integer.Value >> 16) > 31) {
            continue;
        }
        s = &slots[nslots];
        s->handle = child;
        s->device = (uint8_t)(obj.Integer.Value >> 16);
        if (ACPI_SUCCESS(AcpiInstallNotifyHandler(child, ACPI_SYSTEM_NOTIFY, pcihp_notify, s))) {
            nslots++;
        }
    }
}

/** Evaluate the integer `name` of `handle` into `*value`, 0 when the
 * object is absent, as Linux's acpi_pci_root_add reads _SEG and _BBN;
 * nonzero when it is there and cannot be read as an integer. */
static int pcihp_integer(ACPI_HANDLE handle, char *name, UINT64 *value)
{
    ACPI_BUFFER buf;
    ACPI_OBJECT obj;
    ACPI_STATUS status;

    buf.Length = sizeof(obj);
    buf.Pointer = &obj;
    status = AcpiEvaluateObjectTyped(handle, name, NULL, &buf, ACPI_TYPE_INTEGER);
    *value = ACPI_SUCCESS(status) ? obj.Integer.Value : 0;
    return ACPI_FAILURE(status) && status != AE_NOT_FOUND;
}

/**
 * A host bridge, as Linux's acpi_pci_root_add takes it: one whose _SEG,
 * which is 0 when absent, names another PCI segment is passed over, since
 * the configuration ports reach segment 0 alone, and so is one whose _SEG
 * or _BBN cannot be read. Its root bus is the first of the range of bus
 * numbers its _CRS gives (`pcihp_busnr`), or else its _BBN, or else 0; it
 * keeps the bridge's windows, and the first bridge of bus 0 gives its
 * slots.
 */
static ACPI_STATUS pcihp_root(ACPI_HANDLE handle, UINT32 depth, void *context, void **ret)
{
    static char bbn[] = "_BBN", crs[] = "_CRS", seg[] = "_SEG";
    UINT64 busnr = ~(UINT64)0, segment;
    uint8_t bus;

    (void)depth;
    (void)context;
    (void)ret;
    if (pcihp_integer(handle, seg, &segment)) {
        klog(KLOG_WARN, "pci", "host bridge: _SEG cannot be read: its windows are not kept");
        return AE_OK;
    }
    if ((segment & 0xFFFFu) != 0) {
        klog(KLOG_INFO, "pci", "host bridge: segment %lu: not reached, its windows are not kept",
             (unsigned long)(segment & 0xFFFFu));
        return AE_OK;
    }
    (void)AcpiWalkResources(handle, crs, pcihp_busnr, &busnr);
    if (busnr == ~(UINT64)0 && pcihp_integer(handle, bbn, &busnr)) {
        klog(KLOG_WARN, "pci", "host bridge: _BBN cannot be read: its windows are not kept");
        return AE_OK;
    }
    if (busnr > 0xFFu) {
        klog(KLOG_WARN, "pci", "host bridge: bus %lu is past 255: its windows are not kept",
             (unsigned long)busnr);
        return AE_OK;
    }
    bus = (uint8_t)busnr;
    (void)AcpiWalkResources(handle, crs, pcihp_window, &bus);
    if (bus == 0 && !root0) {
        root0 = 1;
        pcihp_slots(handle);
    }
    return AE_OK;
}

void acpi_pcihp_start(void)
{
    static char root_hid[] = "PNP0A03";
    const struct pci_aperture *ap;

    (void)AcpiGetDevices(root_hid, pcihp_root, NULL, NULL);
    if (nslots > 0) {
        klog(KLOG_INFO, "pci", "hotplug: %d slots on bus 0, %d windows", nslots,
             pci_root_windows(0, &ap));
    }
}
