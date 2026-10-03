#ifndef _IRQ_H_
#define _IRQ_H_

#include "arch/arch.h"

#include "lib/kint.h"

/*
 * Interrupt handlers, as the shared kernel sees them: a chain of handlers on
 * each vector, the vectors a device takes for its messages, and the counts
 * `/proc/interrupts` and `/proc/stat` report. The architecture defines them
 * (arch/x86/isr.c); a vector is its number for an interrupt, which a driver
 * gets from `IRQ_VECTOR` for a platform line or from `isr_alloc_vector` for
 * its messages.
 */

/*
 * A handler, given the saved registers by pointer so that it can change them
 * (a system call's result, for one) before the return.
 */
typedef void (*isr_handler_t)(isr_params_t *);

/** How many handlers a vector's chain has. */
#define ISR_MAX_VECTOR_HANDLERS 4

/** Append `handler` to a vector's chain. */
void register_isr_handler(uint8_t vector, isr_handler_t handler, const char *name);

/** Replace a vector's entire chain with a single `handler`. */
void set_isr_handler(uint8_t vector, isr_handler_t handler, const char *name);

/**
 * A vector for a device's messages, the lowest not yet given, or -1 when
 * every one is taken. A device asks once, when it enables its messages,
 * and `/proc/interrupts` reports the vector with the device's name, which
 * is how a reader finds it: the number is not fixed.
 */
int isr_alloc_vector(void);

/** The count of interrupts taken on a device's vector. */
uint32_t isr_vector_count(int vector);

/** The name registered in `slot` of `vector`'s chain, or null past the last. */
const char *isr_vector_name(int vector, int slot);

/** Every interrupt routed since the boot. */
uint32_t isr_interrupt_total(void);

/** Whether a device may be given a vector for its messages: the
 * architecture's message controller is up (on a PC, the local APIC). */
int isr_msi_ready(void);

/** Whether the platform line `line` has raised an interrupt not yet taken. */
int isr_line_pending(int line);

/** Whether `vector` has a message delivered and not yet taken. */
int isr_vector_pending(int vector);

/** The interrupts that went away before the processor took them, which the
 * interrupt controller reports on a vector of its own. */
uint32_t isr_spurious_count(void);

/**
 * What delivers interrupts on a vector from `ISR_DEVICE_FIRST` to
 * `ISR_DEVICE_LAST`, for `/proc/interrupts`: on a PC "PIC" for the 8259
 * pair, "IO-APIC" for the I/O APIC, "LAPIC" for the local APIC's timer, and
 * "MSI" for a device's message.
 */
const char *isr_vector_source(int vector);

#endif /* _IRQ_H_ */
