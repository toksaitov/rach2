#ifndef _LAPIC_H_
#define _LAPIC_H_

#include "../../lib/kint.h"

/*
 * lapic: the local APIC, enabled so that PCI devices can send messages, and
 * the processor's receiver of the I/O APIC's interrupts and the APIC
 * timer's.
 *
 * `lapic_init` enables it or refuses with the reason in the kernel log: a
 * processor without an APIC or without the model-specific registers, one in
 * x2APIC mode, a base address this driver cannot map, or `rach2.nomsi` on
 * the boot line. Every device driver asks `lapic_ready` before it programs
 * a message and stays on its legacy line when the answer is no.
 *
 * `lapic_eoi` ends an interrupt that arrived through the APIC: a message, an
 * I/O APIC input, or the timer. While the 8259 pair delivers vectors 32 to
 * 47 it is acknowledged as before, which is why `lapic_init` leaves `LINT0`
 * an unmasked external-interrupt entry; the platform masks it when the I/O
 * APIC takes the lines.
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

/**
 * Whether `vector` is waiting at this processor or is being handled.
 *
 * A driver that pumps its device from both an interrupt and the timer asks
 * this to tell a message that has not been taken yet from work that no
 * message was coming for.
 */
int lapic_pending(uint8_t vector);

/** The APIC identity of the processor that enabled the APIC, read then:
 * the destination of a device's message and of an I/O APIC input. */
uint32_t lapic_id(void);

/** The APIC identity of the processor running this. */
uint32_t lapic_this_id(void);

/** End an interrupt that arrived by message. */
void lapic_eoi(void);

/** Whether `vector` is in this processor's request register: raised and
 * not yet taken, as the 8259's request register says of a line. */
int lapic_requested(uint8_t vector);

/** Write local-vector entry `LINT0` (`lint` 0) or `LINT1` (`lint` 1). */
void lapic_set_lint(int lint, uint32_t entry);

/** Raise `vector` on this processor by an interprocessor interrupt to
 * itself. */
void lapic_self_ipi(uint8_t vector);

/*
 * The timer, this processor's: programmed with `vector`, periodic or
 * one-shot, masked or not, from `initial` counts at the bus clock divided
 * by sixteen; stopped; and read.
 */
void lapic_timer_set(uint8_t vector, int periodic, int masked, uint32_t initial);
void lapic_timer_stop(void);
uint32_t lapic_timer_current(void);
uint32_t lapic_timer_initial(void);

/* Local-vector entry values: masked, and a non-maskable interrupt, active
 * low when `LAPIC_LVT_ACTIVE_LOW` is added. */
#define LAPIC_LVT_MASKED     (1u << 16)
#define LAPIC_LVT_NMI        (4u << 8)
#define LAPIC_LVT_ACTIVE_LOW (1u << 13)

/** The spurious vector's handler, and the count it keeps. */
void lapic_spurious(void);
uint32_t lapic_spurious_count(void);

#endif /* _LAPIC_H_ */
