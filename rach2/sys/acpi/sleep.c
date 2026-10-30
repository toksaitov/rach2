/*
 * sleep.c: the soft-off state through ACPICA, as Linux's acpi_power_off
 * enters it: the firmware's _PTS and _SST first, then every status cleared
 * and every GPE disabled before the write of SLP_EN.
 */

#include "../klog.h"

#include "acpi.h"

#include "acpica.h"

/* Whether acpica_power_off_prepare ran _PTS(5), which loads the sleep types
 * that AcpiEnterSleepState writes. */
static int prepared;

int acpica_power_off_prepare(void)
{
    ACPI_STATUS status;

    if (!acpica_up()) {
        return -1;
    }
    status = AcpiEnterSleepStatePrep(ACPI_STATE_S5);
    if (ACPI_FAILURE(status)) {
        klog(KLOG_WARN, "poweroff", "acpica: prepare: %s", AcpiFormatException(status));
        return -1;
    }
    status = AcpiDisableAllGpes();
    if (ACPI_FAILURE(status)) {
        klog(KLOG_WARN, "poweroff", "acpica: gpes: %s", AcpiFormatException(status));
    }
    AcpiOsWaitEventsComplete();
    prepared = 1;
    klog(KLOG_INFO, "poweroff", "acpica: \\_PTS(5) run, gpes disabled");
    return 0;
}

int acpica_power_off(void)
{
    ACPI_STATUS status;

    if (!prepared) {
        return -1;
    }
    status = AcpiEnterSleepState(ACPI_STATE_S5);
    if (ACPI_FAILURE(status)) {
        klog(KLOG_ERR, "poweroff", "acpica: refused: %s", AcpiFormatException(status));
        return -1;
    }
    return 0;
}

/**
 * ACPICA's last step before the write of SLP_EN, with every status clear.
 * For S5 the write is made here, from the FADT's registers, and
 * AE_CTRL_TERMINATE returned, so that AcpiEnterSleepState returns at once
 * and acpica_power_off's caller decides how long to wait, where ACPICA
 * would wait ten seconds, write again and spin for good. `rega` and `regb`
 * are PM1a's and PM1b's control words with SLP_EN, or on a hardware-reduced
 * machine `rega` is the sleep control register's.
 */
ACPI_STATUS AcpiOsEnterSleep(UINT8 state, UINT32 rega, UINT32 regb)
{
    if (state != ACPI_STATE_S5) {
        return AE_OK;
    }
    klog(KLOG_INFO, "poweroff", "acpica: status cleared");
    klog(KLOG_INFO, "poweroff", "powering off");
    if (AcpiGbl_ReducedHardware) {
        (void)AcpiWrite(rega, &AcpiGbl_FADT.SleepControl);
        return AE_CTRL_TERMINATE;
    }
    (void)AcpiWrite(rega, &AcpiGbl_FADT.XPm1aControlBlock);
    if (AcpiGbl_FADT.XPm1bControlBlock.Address != 0) {
        (void)AcpiWrite(regb, &AcpiGbl_FADT.XPm1bControlBlock);
    }
    return AE_CTRL_TERMINATE;
}
