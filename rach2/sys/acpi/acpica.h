/*
 * acpica.h: ACPICA, the firmware's interpreter of ACPI methods, as the rest
 * of the kernel sees it. Nothing here names a type of ACPICA's, so that a
 * file of the kernel's may include it beside the kernel's own acpi.h.
 */
#ifndef _ACPICA_H_
#define _ACPICA_H_

#include "../lib/kint.h"

/**
 * Bring ACPICA up from the RSDP at `rsdp`, which the kernel's table reader
 * found: its tables, its namespace and the objects' initialization, the
 * SCI's handler, `\_PIC`, and the GPEs whose methods the firmware gave. The
 * boot calls it once, after the heap, PCI, the tick and the scheduler are
 * up. `rach2.noacpica` on the boot line, or an `rsdp` of 0, leaves ACPICA
 * out, with a record why. Logs `acpi: acpica: N objects, E errors`.
 */
void acpica_boot(uint64_t rsdp);

/** Whether `acpica_boot` brought ACPICA up. */
int acpica_up(void);

/**
 * Make the kernel thread that runs ACPICA's deferred work: the notify
 * handlers and the methods of GPEs. Called once, when the first program
 * starts, so that the thread takes a pid after init's 1; until then the
 * work runs in line, or waits in the queue when an interrupt queued it.
 */
void acpica_thread_start(void);

/** The errors and the warnings ACPICA has reported since the boot. */
uint32_t acpica_errors(void);
uint32_t acpica_warnings(void);

/** The SCIs taken, for the tests: every one, and those ACPICA handled. */
uint32_t acpica_sci_count(void);
uint32_t acpica_sci_handled(void);

#endif /* _ACPICA_H_ */
