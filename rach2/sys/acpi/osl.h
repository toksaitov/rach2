/*
 * osl.h: what boot.c needs of osl.c, ACPICA's OS layer, beyond the AcpiOs*
 * functions ACPICA itself calls.
 */
#ifndef _ACPI_OSL_H_
#define _ACPI_OSL_H_

#include "../lib/kint.h"

/** Give `AcpiOsGetRootPointer` the RSDP the kernel's table reader found. */
void osl_set_root(uint64_t rsdp);

/**
 * Run the work `AcpiOsExecute` queued until none is left, in line, as the
 * boot does once ACPICA is up and before its thread is made. Not from an
 * interrupt, and not from inside a call into ACPICA, whose interpreter a
 * notify handler enters again.
 */
void osl_drain(void);

#endif /* _ACPI_OSL_H_ */
