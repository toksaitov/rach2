/*
 * pnp.c: the 8042's devices in the firmware's namespace, found as Linux's
 * pnpacpi and i8042 PNP drivers find them, so that the touchpad's driver
 * can choose Linux's quirks by the PS/2 mouse's PNP ID.
 */

#include "../klog.h"
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
