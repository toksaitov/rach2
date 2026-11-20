/*
 * irq.h: interrupt handlers as the shared kernel sees them, the vectors a
 * device takes for its messages, and the counts the kernel reports of them.
 */
#ifndef _IRQ_H_
#define _IRQ_H_

#include "arch/arch.h"

#include "lib/kint.h"

/*
 * Each vector has a chain of handlers, and `/proc/interrupts` and
 * `/proc/stat` report the counts. The architecture defines them
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

/**
 * Append `handler` to a vector's chain, and when the vector is a platform
 * line's, route the line to the processor.
 *
 * `name` is what `/proc/interrupts` calls the handler, and only the pointer
 * is kept, so it must stay valid while the handler is registered, as a
 * literal does. A handler already on
 * the chain is not added again and keeps its first name. A full chain is
 * reported in the log and the handler is dropped.
 */
void register_isr_handler(uint8_t vector, isr_handler_t handler, const char *name);

/* A handler's flag: it runs without the big lock, which takes nothing the
 * lock guards, as Linux's handlers take only their device's own locks. */
#define ISR_UNLOCKED 1u

/**
 * Give `handler`, already on `vector`'s chain, the flags `flags`
 * (ISR_UNLOCKED). The router of a device's interrupt or an exception runs
 * the chain without the big lock only while every handler on it carries
 * ISR_UNLOCKED; a handler added later runs under the lock until it is
 * marked too. A handler not on the chain is reported in the log.
 */
void isr_register_flags(uint8_t vector, isr_handler_t handler, unsigned int flags);

/** Mark every handler on the chain of exception `vector`, below 32,
 * ISR_UNLOCKED, as `isr_register_flags` does: the page fault's, once the
 * fault takes its own locks. */
void exception_unlock(uint8_t vector);

/** Replace a vector's entire chain with a single `handler` named `name`,
 * for a vector no other handler may share, and route a platform line as
 * `register_isr_handler` does. */
void set_isr_handler(uint8_t vector, isr_handler_t handler, const char *name);

/**
 * A vector for a device's messages, the lowest not yet given, or -1, with a
 * record in the log, when every one is taken. A driver takes one for its
 * device when it enables the device's messages, and `/proc/interrupts`
 * reports the vector with the device's name, which is how a reader finds
 * it: the number is not fixed.
 */
int isr_alloc_vector(void);

/**
 * As `isr_alloc_vector`, but the lowest free vector that is a multiple of
 * `align`, a power of two from 1 to 32: for a device that writes its message
 * number into the low bits of the data whatever its Multiple Message Enable
 * says, so that message 0 still lands on the vector it was given.
 */
int isr_alloc_vector_aligned(int align);

/**
 * Give back a vector from `isr_alloc_vector`, with every handler registered
 * on it, as a driver does when its device will not send messages after all.
 * The vector's next taker starts with an empty chain.
 */
void isr_free_vector(int vector);

/** The count of interrupts taken on a device's vector; 0 for a vector
 * outside `ISR_DEVICE_FIRST` to `ISR_DEVICE_LAST`. */
uint32_t isr_vector_count(int vector);

/** The interrupts taken on `vector` by logical processor `cpu`, or 0
 * outside the device-vector and processor ranges. */
uint32_t isr_vector_cpu_count(int vector, int cpu);

/** Return the free message vectors under the big lock. */
int isr_free_vector_count(void);

/** The name registered in `slot` of `vector`'s chain, or null past the last. */
const char *isr_vector_name(int vector, int slot);

/** Every device interrupt taken since the boot, for `/proc/stat`'s `intr`
 * line. */
uint32_t isr_interrupt_total(void);

/** Whether a device may be given a vector for its messages: the
 * architecture's message controller is up (on a PC, the local APIC). */
int isr_msi_ready(void);

/** Forward messages pending in this processor's controller to the
 * destination published by pci_msi_shutdown, before stopping. With no
 * normal shutdown prepared this is a no-op, including panic's stop. */
void isr_cpu_offline(void);

/** Whether the platform line `line` has raised an interrupt not yet taken. */
int isr_line_pending(int line);

/** Whether `vector` has a message pending on any online processor or a
 * chain entered but not completed. Uses smp_call for remote controller
 * reads; any context except an NMI, with no lock a callback may need. */
int isr_vector_pending(int vector);

/**
 * Wait until no delivery of `vector` is pending or running
 * (`isr_vector_pending`), as Linux's synchronize_irq: for a driver that has
 * turned its device's messages off and is about to give the vector back
 * (`isr_free_vector`), so that a message sent before that runs the chain it
 * was sent to, never the chain of the vector's next taker. Between reads the
 * big lock goes to a processor whose entry waits for it (`bkl_yield`). With
 * interrupts on, a message pending at this processor is taken; with them
 * off, as at shutdown, it is not waited for, and runs when they come on.
 * The caller holds no spinlock a handler of `vector` takes and runs none of
 * its handlers.
 */
void isr_vector_sync(int vector);

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
