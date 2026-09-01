#ifndef _LAPIC_H_
#define _LAPIC_H_

#include "lib/kint.h"

/*
 * lapic: the local APIC, enabled so that PCI devices can send messages.
 *
 * `lapic_init` enables it or refuses with the reason in the kernel log: a
 * processor without an APIC or without the model-specific registers, one in
 * x2APIC mode, a base address this driver cannot map, or `rach2.nomsi` on
 * the boot line. Every device driver asks `lapic_ready` before it programs
 * a message and stays on its legacy line when the answer is no.
 *
 * `lapic_eoi` ends an interrupt that arrived by message; the router calls it
 * for vectors 48 to 63, after the handler chain. The 8259 pair still
 * delivers vectors 32 to 47 and is acknowledged as before, which is why the
 * driver keeps `LINT0` an unmasked external-interrupt entry.
 */

/*
 * The registers, reached through this interface so that a host test can
 * model the page the way the xHCI driver's test models its controller.
 * Without a call to `lapic_set_io` the driver maps the page the processor
 * names and reads and writes it directly.
 */
struct lapic_io {
    uint32_t (*read)(uint32_t reg);
    void (*write)(uint32_t reg, uint32_t value);
};

void lapic_set_io(const struct lapic_io *ops);

/** Enable the APIC: 0 when messages can be received, -1 with the reason
 * named in the log. */
int lapic_init(void);

/** Whether the APIC is enabled and messages can be received. */
int lapic_ready(void);

/**
 * Write `LINT0` as an unmasked external interrupt again.
 *
 * For the one caller that finds the 8259's interrupts have stopped arriving
 * after the APIC was enabled, which is what a firmware or a model that masks
 * that entry behind this driver's back looks like from the timer.
 */
void lapic_repair_lint0(void);

/** The processor's APIC identity, the destination a message carries. */
uint32_t lapic_id(void);

/** End an interrupt that arrived by message. */
void lapic_eoi(void);

/** The spurious vector's handler, and the count it keeps. */
void lapic_spurious(void);
uint32_t lapic_spurious_count(void);

#endif /* _LAPIC_H_ */
