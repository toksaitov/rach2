/*
 * pnp.c: the 8042's devices in the firmware's namespace, found as Linux's
 * pnpacpi and i8042 PNP drivers find them, so that the touchpad's driver
 * can choose Linux's quirks by the PS/2 mouse's PNP ID; and the ports and
 * memory that ACPI's fixed registers and system devices hold, which no
 * PCI BAR may be claimed over.
 */

#include "../klog.h"
#include "../pci.h"
#include "../arch/x86/i8042.h"

#include "acpi.h"

#include "devices.h"

/* The most IDs of a device taken: its _HID and up to seven _CIDs. */
#define PNP_IDS_MAX 8

/** Whether the device `handle` is present by its _STA, as Linux's
 * `acpi_bus_get_status` reads it: a device without _STA is. */
static int present(ACPI_HANDLE handle)
{
    ACPI_BUFFER buf;
    ACPI_OBJECT obj;
    ACPI_STATUS status;
    static char sta[] = "_STA";

    buf.Length = sizeof(obj);
    buf.Pointer = &obj;
    status = AcpiEvaluateObjectTyped(handle, sta, NULL, &buf, ACPI_TYPE_INTEGER);
    if (status == AE_NOT_FOUND) {
        return 1;
    }
    return ACPI_SUCCESS(status) && (obj.Integer.Value & ACPI_STA_DEVICE_PRESENT) != 0;
}

/**
 * A device of the namespace, as `pnpacpi_add_device` takes it: one with
 * _CRS that is present gives its _HID and _CIDs to `i8042_pnp_device`,
 * which keeps those of the keyboard and the PS/2 mouse.
 */
static ACPI_STATUS pnp_found(ACPI_HANDLE handle, UINT32 depth, void *context, void **ret)
{
    const char *ids[PNP_IDS_MAX];
    ACPI_DEVICE_INFO *info = NULL;
    ACPI_HANDLE crs;
    UINT32 i;
    int n = 0, port;

    (void)depth;
    (void)context;
    (void)ret;
    if (ACPI_FAILURE(AcpiGetHandle(handle, "_CRS", &crs)) ||
        ACPI_FAILURE(AcpiGetObjectInfo(handle, &info))) {
        return AE_OK;
    }
    if (info->Valid & ACPI_VALID_HID) {
        ids[n++] = info->HardwareId.String;
    }
    if (info->Valid & ACPI_VALID_CID) {
        for (i = 0; i < info->CompatibleIdList.Count && n < PNP_IDS_MAX; i++) {
            ids[n++] = info->CompatibleIdList.Ids[i].String;
        }
    }
    if (n > 0 && present(handle)) {
        port = i8042_pnp_device(ids, n);
        if (port >= 0) {
            klog(KLOG_INFO, "acpi", "pnp: i8042 %s: %s", port == I8042_KBD_PORT ? "kbd" : "aux",
                 i8042_firmware_id(port));
        }
    }
    ACPI_FREE(info);
    return AE_OK;
}

void acpi_pnp_i8042(void)
{
    (void)AcpiGetDevices(NULL, pnp_found, NULL, NULL);
}

/** Hold `len` bytes of the fixed register block `g` for the host, of ports
 * or of memory as its space says, as Linux's acpi_request_region. */
static void pnp_reserve_block(const ACPI_GENERIC_ADDRESS *g, UINT32 len)
{
    UINT64 at = g->Address;

    if (at == 0 || len == 0) {
        return;
    }
    if (g->SpaceId == ACPI_ADR_SPACE_SYSTEM_IO) {
        pci_host_reserve(at, at + len - 1u, 1);
    } else if (g->SpaceId == ACPI_ADR_SPACE_SYSTEM_MEMORY) {
        pci_host_reserve(at, at + len - 1u, 0);
    }
}

int acpi_resource_range(ACPI_RESOURCE *res, uint64_t *start, uint64_t *len, int *address)
{
    ACPI_RESOURCE_ADDRESS64 a;
    UINT8 type;

    *address = 0;
    switch (res->Type) {
    case ACPI_RESOURCE_TYPE_IO:
        *start = res->Data.Io.Minimum;
        *len = res->Data.Io.AddressLength;
        return ACPI_RANGE_IO;
    case ACPI_RESOURCE_TYPE_FIXED_IO:
        *start = res->Data.FixedIo.Address;
        *len = res->Data.FixedIo.AddressLength;
        return ACPI_RANGE_IO;
    case ACPI_RESOURCE_TYPE_MEMORY24:
        /* ACPICA keeps the descriptor's bits 23:8 of each. */
        *start = (uint64_t)res->Data.Memory24.Minimum << 8;
        *len = (uint64_t)res->Data.Memory24.AddressLength << 8;
        return ACPI_RANGE_MEMORY;
    case ACPI_RESOURCE_TYPE_MEMORY32:
        *start = res->Data.Memory32.Minimum;
        *len = res->Data.Memory32.AddressLength;
        return ACPI_RANGE_MEMORY;
    case ACPI_RESOURCE_TYPE_FIXED_MEMORY32:
        *start = res->Data.FixedMemory32.Address;
        *len = res->Data.FixedMemory32.AddressLength;
        return ACPI_RANGE_MEMORY;
    case ACPI_RESOURCE_TYPE_ADDRESS16:
    case ACPI_RESOURCE_TYPE_ADDRESS32:
    case ACPI_RESOURCE_TYPE_ADDRESS64:
        if (ACPI_FAILURE(AcpiResourceToAddress64(res, &a))) {
            return 0;
        }
        type = a.ResourceType;
        *start = a.Address.Minimum + a.Address.TranslationOffset;
        *len = a.Address.AddressLength;
        break;
    case ACPI_RESOURCE_TYPE_EXTENDED_ADDRESS64:
        /* AcpiResourceToAddress64 takes only the three above. */
        type = res->Data.ExtAddress64.ResourceType;
        *start = res->Data.ExtAddress64.Address.Minimum +
                 res->Data.ExtAddress64.Address.TranslationOffset;
        *len = res->Data.ExtAddress64.Address.AddressLength;
        break;
    default:
        return 0;
    }
    *address = 1;
    if (type == ACPI_MEMORY_RANGE) {
        return ACPI_RANGE_MEMORY;
    }
    if (type == ACPI_IO_RANGE) {
        return ACPI_RANGE_IO;
    }
    return type == ACPI_BUS_NUMBER_RANGE ? ACPI_RANGE_BUS : 0;
}

/**
 * A resource of a system device's _CRS held for the host, as Linux's
 * pnp/system.c reserves it: ports from 0x100 up, since those below are the
 * PC's own, which the PCI layer holds already, and memory of any address,
 * from every descriptor of either (`acpi_resource_range`).
 */
static ACPI_STATUS pnp_reserve_one(ACPI_RESOURCE *res, void *context)
{
    uint64_t start = 0, len = 0;
    int address, kind;

    (void)context;
    kind = acpi_resource_range(res, &start, &len, &address);
    if (len != 0 && (kind == ACPI_RANGE_MEMORY || (kind == ACPI_RANGE_IO && start >= 0x100u))) {
        pci_host_reserve(start, start + len - 1u, kind == ACPI_RANGE_IO);
    }
    return AE_OK;
}

/** A device of the namespace: one that is present and names PNP0C01 or
 * PNP0C02, a system device, by its _HID or a _CID has its _CRS held. */
static ACPI_STATUS pnp_system(ACPI_HANDLE handle, UINT32 depth, void *context, void **ret)
{
    static char crs[] = "_CRS";
    ACPI_DEVICE_INFO *info = NULL;
    ACPI_HANDLE h;
    UINT32 i;
    int system = 0;

    (void)depth;
    (void)context;
    (void)ret;
    if (ACPI_FAILURE(AcpiGetHandle(handle, crs, &h)) ||
        ACPI_FAILURE(AcpiGetObjectInfo(handle, &info))) {
        return AE_OK;
    }
    if (info->Valid & ACPI_VALID_HID) {
        system = strcmp(info->HardwareId.String, "PNP0C01") == 0 ||
                 strcmp(info->HardwareId.String, "PNP0C02") == 0;
    }
    for (i = 0; (info->Valid & ACPI_VALID_CID) && i < info->CompatibleIdList.Count; i++) {
        system |= strcmp(info->CompatibleIdList.Ids[i].String, "PNP0C01") == 0 ||
                  strcmp(info->CompatibleIdList.Ids[i].String, "PNP0C02") == 0;
    }
    ACPI_FREE(info);
    if (system && present(handle)) {
        (void)AcpiWalkResources(handle, crs, pnp_reserve_one, NULL);
    }
    return AE_OK;
}

void acpi_pnp_reserve(void)
{
    pnp_reserve_block(&AcpiGbl_FADT.XPm1aEventBlock, AcpiGbl_FADT.Pm1EventLength);
    pnp_reserve_block(&AcpiGbl_FADT.XPm1bEventBlock, AcpiGbl_FADT.Pm1EventLength);
    pnp_reserve_block(&AcpiGbl_FADT.XPm1aControlBlock, AcpiGbl_FADT.Pm1ControlLength);
    pnp_reserve_block(&AcpiGbl_FADT.XPm1bControlBlock, AcpiGbl_FADT.Pm1ControlLength);
    pnp_reserve_block(&AcpiGbl_FADT.XPm2ControlBlock, AcpiGbl_FADT.Pm2ControlLength);
    pnp_reserve_block(&AcpiGbl_FADT.XPmTimerBlock, AcpiGbl_FADT.PmTimerLength);
    pnp_reserve_block(&AcpiGbl_FADT.XGpe0Block, AcpiGbl_FADT.Gpe0BlockLength);
    pnp_reserve_block(&AcpiGbl_FADT.XGpe1Block, AcpiGbl_FADT.Gpe1BlockLength);
    (void)AcpiGetDevices(NULL, pnp_system, NULL, NULL);
}
