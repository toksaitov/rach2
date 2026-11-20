/*
 * acpi_pnp_hosttest.c: the ranges the kernel holds for the host from the
 * _CRS of ACPI's system devices, as pnp.c reads each kind of resource.
 *
 * host-cflags: -D__RACH2__ -I rach2/sys/acpi
 * host-cflags: -isystem build/vendor/acpica/include
 *
 * pnp.c is included whole, over ACPICA's own headers, which are the same
 * for both architectures and come from i686's extracted tree. The ACPICA
 * calls it makes are answered here: AcpiResourceToAddress64 as ACPICA's
 * takes the Word, DWord and QWord descriptors and refuses the rest, and
 * pci_host_reserve records what it is given. The rows hand pnp.c's walker
 * one resource at a time, as AcpiWalkResources would, and check the range
 * held: a 24-bit memory descriptor's address and length shifted up by 8,
 * the 32-bit and fixed ones as they are, an Extended address descriptor's
 * memory with its translation, ports from 0x100 up only, and nothing for a
 * range of bus numbers or one of no length; and acpi_resource_range's kind
 * and address flag for each.
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

/** No namespace: no row walks one. */
ACPI_STATUS AcpiGetDevices(char *hid, ACPI_WALK_CALLBACK fn, void *context, void **ret)
{
    (void)hid;
    (void)fn;
    (void)context;
    (void)ret;
    return AE_OK;
}

/** No namespace. */
ACPI_STATUS AcpiGetHandle(ACPI_HANDLE parent, const char *path, ACPI_HANDLE *ret)
{
    (void)parent;
    (void)path;
    (void)ret;
    return AE_NOT_FOUND;
}

/** No namespace. */
ACPI_STATUS AcpiGetObjectInfo(ACPI_HANDLE handle, ACPI_DEVICE_INFO **info)
{
    (void)handle;
    (void)info;
    return AE_NOT_FOUND;
}

/** No namespace. */
ACPI_STATUS AcpiEvaluateObjectTyped(ACPI_HANDLE handle, ACPI_STRING path, ACPI_OBJECT_LIST *args,
                                    ACPI_BUFFER *ret, ACPI_OBJECT_TYPE type)
{
    (void)handle;
    (void)path;
    (void)args;
    (void)ret;
    (void)type;
    return AE_NOT_FOUND;
}

/** No namespace. */
ACPI_STATUS AcpiWalkResources(ACPI_HANDLE handle, char *name, ACPI_WALK_RESOURCE_CALLBACK fn,
                              void *context)
{
    (void)handle;
    (void)name;
    (void)fn;
    (void)context;
    return AE_NOT_FOUND;
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

/** Each kind of descriptor a system device's _CRS gives. */
static void reserve_rows(void)
{
    ACPI_RESOURCE r;
    uint64_t start, len;
    int address;

    memset(&r, 0, sizeof(r));
    r.Type = ACPI_RESOURCE_TYPE_MEMORY24;
    r.Data.Memory24.Minimum = 0xF000;
    r.Data.Memory24.Maximum = 0xF000;
    r.Data.Memory24.AddressLength = 0x0010;
    check(holds(&r, 0x00F00000u, 0x00F00FFFu, 0),
          "a 24-bit memory descriptor's address and length are shifted up by 8");
    check(acpi_resource_range(&r, &start, &len, &address) == ACPI_RANGE_MEMORY && !address &&
              start == 0x00F00000u && len == 0x1000u,
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
    r.Type = ACPI_RESOURCE_TYPE_EXTENDED_ADDRESS64;
    r.Data.ExtAddress64.ResourceType = ACPI_MEMORY_RANGE;
    r.Data.ExtAddress64.Address.Minimum = 0xE0000000u;
    r.Data.ExtAddress64.Address.Maximum = 0xE00FFFFFu;
    r.Data.ExtAddress64.Address.TranslationOffset = 0x100000000ULL;
    r.Data.ExtAddress64.Address.AddressLength = 0x100000u;
    check(holds(&r, 0x1E0000000ULL, 0x1E00FFFFFULL, 0),
          "an Extended address descriptor's memory is held, its translation added");
    check(acpi_resource_range(&r, &start, &len, &address) == ACPI_RANGE_MEMORY && address,
          "acpi_resource_range: an Extended descriptor is an address descriptor");
    r.Data.ExtAddress64.ResourceType = ACPI_BUS_NUMBER_RANGE;
    check(holds_nothing(&r) && acpi_resource_range(&r, &start, &len, &address) == ACPI_RANGE_BUS &&
              address,
          "an Extended descriptor of bus numbers is no range of the host's");

    memset(&r, 0, sizeof(r));
    r.Type = ACPI_RESOURCE_TYPE_ADDRESS64;
    r.Data.Address64.ResourceType = ACPI_IO_RANGE;
    r.Data.Address64.Address.Minimum = 0x2000u;
    r.Data.Address64.Address.Maximum = 0x20FFu;
    r.Data.Address64.Address.AddressLength = 0x100u;
    check(holds(&r, 0x2000u, 0x20FFu, 1), "a QWord descriptor's ports are held");
    r.Data.Address64.ResourceType = ACPI_MEMORY_RANGE;
    r.Data.Address64.Address.AddressLength = 0;
    check(holds_nothing(&r), "nor a range of no length");

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
    check(holds_nothing(&r) && acpi_resource_range(&r, &start, &len, &address) == 0,
          "an interrupt is no range");
}

/** Run the rows. */
int main(void)
{
    reserve_rows();
    if (fails) {
        printf("acpi_pnp_hosttest: %d failed\n", fails);
        return 1;
    }
    printf("acpi_pnp_hosttest: ok\n");
    return 0;
}
