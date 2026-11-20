/*
 * acpi_pnp_hosttest.c: the ranges the kernel holds for the host from the
 * _CRS of ACPI's system devices, as pnp.c reads each kind of resource, and
 * the host bridges whose windows and slots pcihp.c takes from a namespace.
 *
 * host-cflags: -D__RACH2__ -I rach2/sys/acpi
 * host-cflags: -isystem build/vendor/acpica/include
 *
 * pnp.c and pcihp.c are included whole, over ACPICA's own headers, which
 * are the same for both architectures and come from i686's extracted tree.
 * The ACPICA calls they make are answered here: AcpiResourceToAddress64 as
 * ACPICA's takes the Word, DWord and QWord descriptors and refuses the
 * rest, pci_host_reserve and pci_root_window_add record what they are
 * given, and a model namespace of host bridges answers the walks. The rows
 * hand pnp.c's walker one resource at a time, as AcpiWalkResources would,
 * and check the range held: a 24-bit memory descriptor's address and
 * length shifted up by 8, the 32-bit and fixed ones as they are, an
 * address descriptor from its Minimum to its Maximum whatever its length
 * says, with its translation for a producer and without it for a consumer,
 * ports from 0x100 up only, and nothing for a range of bus numbers, one of
 * no length or one that ends below its start; and acpi_resource_range's
 * kind and address flag for each. Then the namespace's host bridges, three
 * of PCI segment 0 and three of segment 1, one of each with slots, one
 * with a _SEG that is no integer, show that only segment 0's windows and
 * slots are kept, with a producer's translation, for the root bus of the
 * last Word, DWord or QWord range of bus numbers in its _CRS, whose
 * Minimum counts whenever its length is not 0, whatever its Maximum.
 */

/* The kernel's headers first, whose NULL ACPICA's leaves alone; then
 * ACPICA's acpi.h, and the guard of the kernel's arch/x86/acpi.h, which
 * the host test's quoted paths would find for pnp.c's "acpi.h" before the
 * kernel build's -isystem does. */
#include "../../rach2/sys/klog.h"
#include "../../rach2/sys/pci.h"
#include <acpi.h>
#define _ACPI_H_

#include "../../rach2/sys/acpi/pnp.c"
#include "../../rach2/sys/acpi/pcihp.c"

extern int printf(const char *fmt, ...);
extern void *memset(void *s, int c, size_t n);

/* The FADT, whose blocks no row reserves. */
ACPI_TABLE_FADT AcpiGbl_FADT;

/* What pci_host_reserve was given last, and how often. */
static uint64_t held_start, held_end;
static int held_io, held_count;

/** Record a range held for the host. */
void pci_host_reserve(uint64_t start, uint64_t end, int io)
{
    held_start = start;
    held_end = end;
    held_io = io;
    held_count++;
}

/** The log is discarded. */
void klog(int level, const char *subsystem, const char *fmt, ...)
{
    (void)level;
    (void)subsystem;
    (void)fmt;
}

/** No device is the 8042's. */
int i8042_pnp_device(const char *const *ids, int n)
{
    (void)ids;
    (void)n;
    return -1;
}

/** No row asks for a firmware ID. */
const char *i8042_firmware_id(int port)
{
    (void)port;
    return "";
}

/** ACPICA's conversion: a Word, DWord or QWord address descriptor into the
 * QWord form; the rows give QWord ones. Anything else is refused. */
ACPI_STATUS AcpiResourceToAddress64(ACPI_RESOURCE *res, ACPI_RESOURCE_ADDRESS64 *out)
{
    if (res->Type != ACPI_RESOURCE_TYPE_ADDRESS64) {
        return AE_BAD_PARAMETER;
    }
    *out = res->Data.Address64;
    return AE_OK;
}

/* A host bridge of the model namespace: its _SEG and _BBN, each absent
 * when its status is AE_NOT_FOUND, its _CRS, and how many of its children
 * are slots, devices with _ADR and _EJ0. */
struct model_root {
    ACPI_STATUS seg_status;
    UINT64 seg;
    ACPI_STATUS bbn_status;
    UINT64 bbn;
    ACPI_RESOURCE crs[4];
    int ncrs;
    int nslots;
};

/* A slot of the model: its bridge and its device number. */
struct model_slot {
    const struct model_root *root;
    UINT64 adr;
};

static struct model_root model_roots[6];
static int model_nroots;
static struct model_slot model_slots[8];
static int model_nslots;

/* What pci_root_window_add was given, and the slots whose Notify was taken. */
static struct {
    uint8_t bus;
    struct pci_aperture w;
} kept[16];
static int nkept;

/** Whether `h` is a slot of the model. */
static struct model_slot *model_slot_of(ACPI_HANDLE h)
{
    struct model_slot *s = (struct model_slot *)h;

    return s >= model_slots && s < model_slots + model_nslots ? s : NULL;
}

/** Whether `h` is a host bridge of the model. */
static struct model_root *model_root_of(ACPI_HANDLE h)
{
    struct model_root *r = (struct model_root *)h;

    return r >= model_roots && r < model_roots + model_nroots ? r : NULL;
}

/** The model's host bridges, PNP0A03, in order; no other device. */
ACPI_STATUS AcpiGetDevices(char *hid, ACPI_WALK_CALLBACK fn, void *context, void **ret)
{
    int k;

    if (hid == NULL || strcmp(hid, "PNP0A03") != 0) {
        return AE_OK;
    }
    for (k = 0; k < model_nroots; k++) {
        (void)fn(&model_roots[k], 1, context, ret);
    }
    return AE_OK;
}

/** A slot has _EJ0; nothing else is found. */
ACPI_STATUS AcpiGetHandle(ACPI_HANDLE parent, const char *path, ACPI_HANDLE *ret)
{
    if (model_slot_of(parent) != NULL && strcmp(path, "_EJ0") == 0) {
        *ret = parent;
        return AE_OK;
    }
    return AE_NOT_FOUND;
}

/** No device's information is asked for. */
ACPI_STATUS AcpiGetObjectInfo(ACPI_HANDLE handle, ACPI_DEVICE_INFO **info)
{
    (void)handle;
    (void)info;
    return AE_NOT_FOUND;
}

/** A host bridge's _SEG and _BBN, as its status says, and a slot's _ADR. */
ACPI_STATUS AcpiEvaluateObjectTyped(ACPI_HANDLE handle, ACPI_STRING path, ACPI_OBJECT_LIST *args,
                                    ACPI_BUFFER *ret, ACPI_OBJECT_TYPE type)
{
    struct model_root *r = model_root_of(handle);
    struct model_slot *sl = model_slot_of(handle);
    ACPI_OBJECT *obj = (ACPI_OBJECT *)ret->Pointer;
    ACPI_STATUS status = AE_NOT_FOUND;
    UINT64 v = 0;

    (void)args;
    (void)type;
    if (r != NULL && strcmp(path, "_SEG") == 0) {
        status = r->seg_status;
        v = r->seg;
    } else if (r != NULL && strcmp(path, "_BBN") == 0) {
        status = r->bbn_status;
        v = r->bbn;
    } else if (sl != NULL && strcmp(path, "_ADR") == 0) {
        status = AE_OK;
        v = sl->adr;
    }
    if (ACPI_SUCCESS(status)) {
        obj->Type = ACPI_TYPE_INTEGER;
        obj->Integer.Value = v;
    }
    return status;
}

/** A host bridge's _CRS, one resource at a time. */
ACPI_STATUS AcpiWalkResources(ACPI_HANDLE handle, char *name, ACPI_WALK_RESOURCE_CALLBACK fn,
                              void *context)
{
    struct model_root *r = model_root_of(handle);
    int k;

    if (r == NULL || strcmp(name, "_CRS") != 0) {
        return AE_NOT_FOUND;
    }
    for (k = 0; k < r->ncrs; k++) {
        if (fn(&r->crs[k], context) == AE_CTRL_TERMINATE) {
            break;
        }
    }
    return AE_OK;
}

/** The slots of host bridge `parent` after `child`, in order. */
ACPI_STATUS AcpiGetNextObject(ACPI_OBJECT_TYPE type, ACPI_HANDLE parent, ACPI_HANDLE child,
                              ACPI_HANDLE *out)
{
    struct model_slot *c = model_slot_of(child);
    int k = c != NULL ? (int)(c - model_slots) + 1 : 0;

    (void)type;
    for (; k < model_nslots; k++) {
        if (model_slots[k].root == parent) {
            *out = &model_slots[k];
            return AE_OK;
        }
    }
    return AE_NOT_FOUND;
}

/** Every slot's Notify is taken. */
ACPI_STATUS AcpiInstallNotifyHandler(ACPI_HANDLE handle, UINT32 type, ACPI_NOTIFY_HANDLER fn,
                                     void *context)
{
    (void)handle;
    (void)type;
    (void)fn;
    (void)context;
    return AE_OK;
}

/** No row ejects. */
ACPI_STATUS AcpiEvaluateObject(ACPI_HANDLE handle, ACPI_STRING path, ACPI_OBJECT_LIST *args,
                               ACPI_BUFFER *ret)
{
    (void)handle;
    (void)path;
    (void)args;
    (void)ret;
    return AE_OK;
}

/** No row formats a status. */
const char *AcpiFormatException(ACPI_STATUS status)
{
    (void)status;
    return "";
}

/** Record a window kept for root bus `bus`. */
int pci_root_window_add(uint8_t bus, const struct pci_aperture *w)
{
    kept[nkept].bus = bus;
    kept[nkept].w = *w;
    nkept++;
    return 0;
}

/** Bus 0's windows, which only the log counts. */
int pci_root_windows(uint8_t bus, const struct pci_aperture **ap)
{
    (void)bus;
    *ap = NULL;
    return 0;
}

/* The PCI list, sysfs and virtio-pci, which no row reaches: no slot is
 * checked or ejected. */
int pci_slot_add(uint8_t bus, uint8_t device)
{
    (void)bus;
    (void)device;
    return 0;
}

int pci_slot_remove(uint8_t bus, uint8_t device)
{
    (void)bus;
    (void)device;
    return 0;
}

bool pci_nth(int nth, pci_device_t *out)
{
    (void)nth;
    (void)out;
    return false;
}

const char *pci_owner(uint8_t bus, uint8_t devfn)
{
    (void)bus;
    (void)devfn;
    return NULL;
}

int pci_assign_bars(uint8_t bus, uint8_t device, uint8_t function, const struct pci_aperture *ap,
                    int n)
{
    (void)bus;
    (void)device;
    (void)function;
    (void)ap;
    (void)n;
    return 0;
}

void pci_write_config16(uint8_t bus, uint8_t device, uint8_t function, uint8_t offset,
                        uint16_t value)
{
    (void)bus;
    (void)device;
    (void)function;
    (void)offset;
    (void)value;
}

int32_t sysfs_pci_uevent(int key, const char *action)
{
    (void)key;
    (void)action;
    return 0;
}

int virtio_pci_bind(uint8_t bus, uint8_t devfn)
{
    (void)bus;
    (void)devfn;
    return 0;
}

int virtio_pci_unbind(uint8_t bus, uint8_t devfn)
{
    (void)bus;
    (void)devfn;
    return 0;
}

/** Nothing is allocated. */
void AcpiOsFree(void *p)
{
    (void)p;
}

static int fails;

/** Count a failure and print `what` when `ok` is false. */
static void check(int ok, const char *what)
{
    if (!ok) {
        printf("  FAIL: %s\n", what);
        fails++;
    }
}

/** Give `res` to pnp.c's walker; whether it held `start` to `end`, of
 * ports when `io` is set. */
static int holds(ACPI_RESOURCE *res, uint64_t start, uint64_t end, int io)
{
    held_count = 0;
    (void)pnp_reserve_one(res, NULL);
    return held_count == 1 && held_start == start && held_end == end && held_io == io;
}

/** Give `res` to pnp.c's walker; whether it held nothing. */
static int holds_nothing(ACPI_RESOURCE *res)
{
    held_count = 0;
    (void)pnp_reserve_one(res, NULL);
    return held_count == 0;
}

/** Each address descriptor a system device's _CRS gives: Extended and
 * QWord ones, of producers and consumers. */
static void address_rows(void)
{
    ACPI_RESOURCE r;
    uint64_t start, end;
    int address;

    memset(&r, 0, sizeof(r));
    r.Type = ACPI_RESOURCE_TYPE_EXTENDED_ADDRESS64;
    r.Data.ExtAddress64.ResourceType = ACPI_MEMORY_RANGE;
    r.Data.ExtAddress64.Address.Minimum = 0xE0000000u;
    r.Data.ExtAddress64.Address.Maximum = 0xE00FFFFFu;
    r.Data.ExtAddress64.Address.TranslationOffset = 0x100000000ULL;
    r.Data.ExtAddress64.Address.AddressLength = 0x100000u;
    check(holds(&r, 0x1E0000000ULL, 0x1E00FFFFFULL, 0),
          "an Extended address descriptor's memory is held, a producer's translation added");
    r.Data.ExtAddress64.ProducerConsumer = ACPI_CONSUMER;
    r.Data.ExtAddress64.Address.Minimum = 0xFED00000u;
    r.Data.ExtAddress64.Address.Maximum = 0xFED00FFFu;
    r.Data.ExtAddress64.Address.AddressLength = 0x1000u;
    check(holds(&r, 0xFED00000u, 0xFED00FFFu, 0),
          "a consumer's Extended descriptor is held where it is, its translation not added");
    r.Data.ExtAddress64.Address.Maximum = 0xFED01FFFu;
    check(holds(&r, 0xFED00000u, 0xFED01FFFu, 0),
          "an Extended descriptor is held to its Maximum, past what its length says");
    r.Data.ExtAddress64.Address.Maximum = 0xFECFFFFFu;
    check(holds_nothing(&r) && acpi_resource_range(&r, &start, &end, &address) == 0,
          "nor one whose Maximum lies below its Minimum");
    r.Data.ExtAddress64.ProducerConsumer = ACPI_PRODUCER;
    r.Data.ExtAddress64.Address.Minimum = 0xE0000000u;
    r.Data.ExtAddress64.Address.Maximum = 0xE00FFFFFu;
    r.Data.ExtAddress64.Address.AddressLength = 0x100000u;
    check(acpi_resource_range(&r, &start, &end, &address) == ACPI_RANGE_MEMORY && address,
          "acpi_resource_range: an Extended descriptor is an address descriptor");
    r.Data.ExtAddress64.ResourceType = ACPI_BUS_NUMBER_RANGE;
    check(holds_nothing(&r) && acpi_resource_range(&r, &start, &end, &address) == ACPI_RANGE_BUS &&
              address,
          "an Extended descriptor of bus numbers is no range of the host's");

    memset(&r, 0, sizeof(r));
    r.Type = ACPI_RESOURCE_TYPE_ADDRESS64;
    r.Data.Address64.ResourceType = ACPI_IO_RANGE;
    r.Data.Address64.Address.Minimum = 0x2000u;
    r.Data.Address64.Address.Maximum = 0x20FFu;
    r.Data.Address64.Address.AddressLength = 0x100u;
    check(holds(&r, 0x2000u, 0x20FFu, 1), "a QWord descriptor's ports are held");
    r.Data.Address64.ProducerConsumer = ACPI_CONSUMER;
    r.Data.Address64.Address.TranslationOffset = 0x1000u;
    r.Data.Address64.Address.AddressLength = 0x10u;
    check(holds(&r, 0x2000u, 0x20FFu, 1),
          "a consumer's QWord descriptor is held to its Maximum, its translation not added");
    r.Data.Address64.Address.Minimum = 0xFF00u;
    r.Data.Address64.Address.Maximum = 0x10002u;
    check(holds(&r, 0xFF00u, 0x10002u, 1), "ports up to 0x10002 are held");
    r.Data.Address64.Address.Maximum = 0x10003u;
    check(holds_nothing(&r), "and none that end past it");
    r.Data.Address64.Address.Minimum = 0x2000u;
    r.Data.Address64.Address.Maximum = 0x20FFu;
    r.Data.Address64.ResourceType = ACPI_MEMORY_RANGE;
    r.Data.Address64.Address.AddressLength = 0;
    check(holds_nothing(&r), "nor a range of no length");
}

/** Each other kind of descriptor a system device's _CRS gives. */
static void reserve_rows(void)
{
    ACPI_RESOURCE r;
    uint64_t start, end;
    int address;

    memset(&r, 0, sizeof(r));
    r.Type = ACPI_RESOURCE_TYPE_MEMORY24;
    r.Data.Memory24.Minimum = 0xF000;
    r.Data.Memory24.Maximum = 0xF000;
    r.Data.Memory24.AddressLength = 0x0010;
    check(holds(&r, 0x00F00000u, 0x00F00FFFu, 0),
          "a 24-bit memory descriptor's address and length are shifted up by 8");
    check(acpi_resource_range(&r, &start, &end, &address) == ACPI_RANGE_MEMORY && !address &&
              start == 0x00F00000u && end == 0x00F00FFFu,
          "acpi_resource_range: a 24-bit descriptor is memory, not an address descriptor");

    memset(&r, 0, sizeof(r));
    r.Type = ACPI_RESOURCE_TYPE_MEMORY32;
    r.Data.Memory32.Minimum = 0xFED00000u;
    r.Data.Memory32.Maximum = 0xFED00000u;
    r.Data.Memory32.AddressLength = 0x400u;
    check(holds(&r, 0xFED00000u, 0xFED003FFu, 0), "a 32-bit memory descriptor is held as it is");

    memset(&r, 0, sizeof(r));
    r.Type = ACPI_RESOURCE_TYPE_FIXED_MEMORY32;
    r.Data.FixedMemory32.Address = 0xFED1C000u;
    r.Data.FixedMemory32.AddressLength = 0x4000u;
    check(holds(&r, 0xFED1C000u, 0xFED1FFFFu, 0), "a fixed 32-bit memory descriptor is held");

    memset(&r, 0, sizeof(r));
    r.Type = ACPI_RESOURCE_TYPE_IO;
    r.Data.Io.Minimum = 0x0400u;
    r.Data.Io.Maximum = 0x0400u;
    r.Data.Io.AddressLength = 0x80u;
    check(holds(&r, 0x0400u, 0x047Fu, 1), "an I/O descriptor's ports from 0x100 up are held");
    r.Data.Io.Minimum = 0x0060u;
    r.Data.Io.AddressLength = 1;
    check(holds_nothing(&r), "ports below 0x100, the PC's own, are not");

    memset(&r, 0, sizeof(r));
    r.Type = ACPI_RESOURCE_TYPE_FIXED_IO;
    r.Data.FixedIo.Address = 0x0800u;
    r.Data.FixedIo.AddressLength = 0x10u;
    check(holds(&r, 0x0800u, 0x080Fu, 1), "a fixed I/O descriptor's ports are held");

    memset(&r, 0, sizeof(r));
    r.Type = ACPI_RESOURCE_TYPE_IRQ;
    check(holds_nothing(&r) && acpi_resource_range(&r, &start, &end, &address) == 0,
          "an interrupt is no range");
}

/** Make `r` a QWord or Extended descriptor of `type` that a bridge gives
 * its bus, `min` to `max`, translated by `offset`. */
static void window(ACPI_RESOURCE *r, int extended, UINT8 type, UINT64 min, UINT64 max,
                   UINT64 offset)
{
    ACPI_ADDRESS64_ATTRIBUTE *a;

    memset(r, 0, sizeof(*r));
    if (extended) {
        r->Type = ACPI_RESOURCE_TYPE_EXTENDED_ADDRESS64;
        r->Data.ExtAddress64.ResourceType = type;
        r->Data.ExtAddress64.ProducerConsumer = ACPI_PRODUCER;
        a = &r->Data.ExtAddress64.Address;
    } else {
        r->Type = ACPI_RESOURCE_TYPE_ADDRESS64;
        r->Data.Address64.ResourceType = type;
        r->Data.Address64.ProducerConsumer = ACPI_PRODUCER;
        a = &r->Data.Address64.Address;
    }
    a->Minimum = min;
    a->Maximum = max;
    a->AddressLength = max - min + 1u;
    a->TranslationOffset = offset;
}

/** Add a host bridge of PCI segment `seg`, whose _SEG reads with `status`,
 * with `nslots` slots; its _CRS is filled by the row. */
static struct model_root *bridge(ACPI_STATUS status, UINT64 seg, int nslots)
{
    struct model_root *r = &model_roots[model_nroots++];
    int k;

    memset(r, 0, sizeof(*r));
    r->seg_status = status;
    r->seg = seg;
    r->bbn_status = AE_NOT_FOUND;
    for (k = 0; k < nslots; k++) {
        model_slots[model_nslots].root = r;
        model_slots[model_nslots].adr = (UINT64)(k + 1) << 16;
        model_nslots++;
    }
    return r;
}

/** Whether window `k` kept is `start` to `end` of memory, for root bus
 * `bus`. */
static int kept_is(int k, uint8_t bus, uint64_t start, uint64_t end)
{
    return k < nkept && kept[k].bus == bus && kept[k].w.start == start && kept[k].w.end == end &&
           !kept[k].w.io;
}

/**
 * The host bridges of a namespace, as pcihp.c walks them: segment 0's bus
 * 0x80, whose _CRS gives an Extended bus range first and then a QWord one,
 * and a translated window; segment 1's bus 0x80, with the window segment
 * 0's BAR at 0xD1000000 would otherwise lie in; segment 1's bus 0 with a
 * slot; a bridge whose _SEG is no integer; segment 0's bus 0, by its
 * _BBN, with two slots; and segment 0's bus 0x90, with no _BBN, whose
 * QWord bus range has a Maximum of 0 below its Minimum and a length.
 */
static void segment_rows(void)
{
    struct model_root *r;

    r = bridge(AE_NOT_FOUND, 0, 0);
    window(&r->crs[0], 1, ACPI_BUS_NUMBER_RANGE, 0x40, 0x40, 0);
    window(&r->crs[1], 0, ACPI_BUS_NUMBER_RANGE, 0x80, 0x80, 0);
    window(&r->crs[2], 0, ACPI_MEMORY_RANGE, 0x40000000u, 0x4FFFFFFFu, 0x80000000u);
    r->ncrs = 3;
    r = bridge(AE_OK, 1, 0);
    window(&r->crs[0], 0, ACPI_BUS_NUMBER_RANGE, 0x80, 0x80, 0);
    window(&r->crs[1], 0, ACPI_MEMORY_RANGE, 0xD0000000u, 0xDFFFFFFFu, 0);
    r->ncrs = 2;
    r = bridge(AE_OK, 0x10001, 1);
    window(&r->crs[0], 0, ACPI_MEMORY_RANGE, 0xE0000000u, 0xEFFFFFFFu, 0);
    r->ncrs = 1;
    r = bridge(AE_TYPE, 0, 0);
    window(&r->crs[0], 0, ACPI_MEMORY_RANGE, 0xF0000000u, 0xF0FFFFFFu, 0);
    r->ncrs = 1;
    r = bridge(AE_OK, 0, 2);
    r->bbn_status = AE_OK;
    window(&r->crs[0], 0, ACPI_MEMORY_RANGE, 0xA0000000u, 0xA0FFFFFFu, 0);
    r->ncrs = 1;
    r = bridge(AE_NOT_FOUND, 0, 0);
    window(&r->crs[0], 0, ACPI_BUS_NUMBER_RANGE, 0x90, 0, 0);
    r->crs[0].Data.Address64.Address.AddressLength = 0x10;
    window(&r->crs[1], 0, ACPI_MEMORY_RANGE, 0xB0000000u, 0xB0FFFFFFu, 0);
    r->ncrs = 2;

    acpi_pcihp_start();
    check(kept_is(0, 0x80, 0xC0000000u, 0xCFFFFFFFu),
          "pcihp: segment 0's window is kept for the last QWord bus range, translated");
    check(kept_is(1, 0, 0xA0000000u, 0xA0FFFFFFu) && nkept == 3,
          "pcihp: no window of segment 1, nor of a bridge whose _SEG is no integer, is kept");
    check(kept_is(2, 0x90, 0xB0000000u, 0xB0FFFFFFu),
          "pcihp: a bus range's Minimum is the root bus whenever its length is not 0, whatever "
          "its Maximum");
    check(nslots == 2 && slots[0].handle == &model_slots[1] && slots[1].handle == &model_slots[2],
          "pcihp: only segment 0's bus 0 gives its slots");
}

/** Run the rows. */
int main(void)
{
    reserve_rows();
    address_rows();
    segment_rows();
    if (fails) {
        printf("acpi_pnp_hosttest: %d failed\n", fails);
        return 1;
    }
    printf("acpi_pnp_hosttest: ok\n");
    return 0;
}
