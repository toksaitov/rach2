/*
 * boot.c: bring ACPICA up at boot, as acpica.h describes, with the ACPI
 * devices of the kernel's own (devices.h) in their places among its steps.
 */

#include "../cmdline.h"
#include "../klog.h"
#include "../arch/x86/isr.h"

#include "acpi.h"

#include "acpica.h"
#include "devices.h"
#include "osl.h"

/* The tables ACPICA's root table array starts with; it grows past them. */
#define BOOT_TABLES 16

static int up;

int acpica_up(void)
{
    return up;
}

/** A node of the namespace, counted into the `uint32_t` at `context`. */
static ACPI_STATUS count_object(ACPI_HANDLE object, UINT32 depth, void *context, void **ret)
{
    (void)object;
    (void)depth;
    (void)ret;
    (*(uint32_t *)context)++;
    return AE_OK;
}

/** Whether `status` is a success; a failure is logged as the step `what`
 * that refused, which leaves ACPICA down. */
static int step(ACPI_STATUS status, const char *what)
{
    if (ACPI_FAILURE(status)) {
        klog(KLOG_ERR, "acpi", "acpica: refused: %s: %s", what, AcpiFormatException(status));
        return 0;
    }
    return 1;
}

/**
 * Tell the firmware the interrupt model, `\_PIC(1)` when the I/O APIC has
 * the lines and `\_PIC(0)` for the 8259, so that its `_PRT` answers for the
 * one in use. A firmware without `\_PIC` has one model.
 */
static void pic_model(void)
{
    static char path[] = "\\_PIC";
    ACPI_OBJECT arg;
    ACPI_OBJECT_LIST args;
    ACPI_STATUS status;

    arg.Type = ACPI_TYPE_INTEGER;
    arg.Integer.Value = isr_lines_apic() ? 1 : 0;
    args.Count = 1;
    args.Pointer = &arg;
    status = AcpiEvaluateObject(NULL, path, &args, NULL);
    if (ACPI_FAILURE(status) && status != AE_NOT_FOUND) {
        klog(KLOG_WARN, "acpi", "acpica: \\_PIC: %s", AcpiFormatException(status));
    }
}

/**
 * The order is ACPICA's: the tables and then the namespace they define; the
 * embedded controller's address space, which methods from here on may
 * reach; ACPI mode and the SCI; the objects' `_STA` and `_INI`, which run
 * methods; the EC's `_REG`, which may read what `_INI` set, and the
 * devices' handlers; and last the GPEs that have methods. The
 * SCI's line is held until `AcpiEnableSubsystem` registers its handler
 * (osl.c). The work an SCI queued meanwhile runs here, outside ACPICA,
 * before the thread is made.
 */
void acpica_boot(uint64_t rsdp)
{
    uint32_t objects = 0;
    ACPI_STATUS status;

    if (kcmdline_has("rach2.noacpica")) {
        klog(KLOG_INFO, "acpi", "acpica: skipped: rach2.noacpica");
        return;
    }
    if (rsdp == 0) {
        klog(KLOG_INFO, "acpi", "acpica: none: no RSDP");
        return;
    }
    osl_set_root(rsdp);
    if (!step(AcpiInitializeSubsystem(), "subsystem") ||
        !step(AcpiInitializeTables(NULL, BOOT_TABLES, TRUE), "tables") ||
        !step(AcpiLoadTables(), "namespace")) {
        return;
    }
    acpi_ec_probe();
    if (!step(AcpiEnableSubsystem(ACPI_FULL_INITIALIZATION), "acpi mode") ||
        !step(AcpiInitializeObjects(ACPI_FULL_INITIALIZATION), "objects")) {
        return;
    }
    pic_model();
    acpi_ec_start();
    acpi_buttons_start();
    acpi_video_start();
    acpi_ideapad_start();
    status = AcpiUpdateAllGpes();
    if (ACPI_FAILURE(status)) {
        klog(KLOG_WARN, "acpi", "acpica: gpes: %s", AcpiFormatException(status));
    }
    up = 1;
    osl_drain();
    (void)AcpiWalkNamespace(ACPI_TYPE_ANY, ACPI_ROOT_OBJECT, ACPI_UINT32_MAX, count_object, NULL,
                            &objects, NULL);
    klog(acpica_errors() ? KLOG_WARN : KLOG_INFO, "acpi", "acpica: %u objects, %u errors", objects,
         acpica_errors());
}
