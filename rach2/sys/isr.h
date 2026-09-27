#ifndef _ISR_H_
#define _ISR_H_

#include "kio.h"
#include "pic.h"

#include "lib/kint.h"
#include "lib/kstring.h"

/*
 * Saved register frame pushed by the ISR common procedure.
 */
struct isr_params {
    uint32_t gs, fs, es, ds;
    uint32_t edi, esi, ebp, esp, ebx, edx, ecx, eax;
    uint32_t vector;
    uint32_t error_code;
    uint32_t eip, cs, eflags, user_esp, user_ss;
};

typedef struct isr_params isr_params_t;

/*
 * Interrupt handler prototype. The frame is passed by pointer so a handler can
 * edit the saved registers (for example, a syscall return value).
 */
typedef void (*isr_handler_t)(isr_params_t *);

/*
 * First 32 interrupt handlers
 */
extern void isr0(void);
extern void isr1(void);
extern void isr2(void);
extern void isr3(void);
extern void isr4(void);
extern void isr5(void);
extern void isr6(void);
extern void isr7(void);
extern void isr9(void);
extern void isr10(void);
extern void isr11(void);
extern void isr12(void);
extern void isr13(void);
extern void isr14(void);
extern void isr15(void);
extern void isr16(void);
extern void isr17(void);
extern void isr18(void);
extern void isr19(void);
extern void isr20(void);
extern void isr21(void);
extern void isr22(void);
extern void isr23(void);
extern void isr24(void);
extern void isr25(void);
extern void isr26(void);
extern void isr27(void);
extern void isr28(void);
extern void isr29(void);
extern void isr30(void);
extern void isr31(void);

/*
 * First 16 interrupt request handlers
 */
extern void irq0(void);
extern void irq1(void);
extern void irq2(void);
extern void irq3(void);
extern void irq4(void);
extern void irq5(void);
extern void irq6(void);
extern void irq7(void);
extern void irq8(void);
extern void irq9(void);
extern void irq10(void);
extern void irq11(void);
extern void irq12(void);
extern void irq13(void);
extern void irq14(void);
extern void irq15(void);

/* The sixteen message-signalled vectors, 48 through 63. */
extern void msi0(void);
extern void msi1(void);
extern void msi2(void);
extern void msi3(void);
extern void msi4(void);
extern void msi5(void);
extern void msi6(void);
extern void msi7(void);
extern void msi8(void);
extern void msi9(void);
extern void msi10(void);
extern void msi11(void);
extern void msi12(void);
extern void msi13(void);
extern void msi14(void);
extern void msi15(void);

/* The vector the APIC raises for an interrupt that went away. */
extern void spurious_isr(void);

/** The count of interrupts taken on a vector from 32 to 63. */
uint32_t isr_vector_count(int vector);

/** The name registered in `slot` of `vector`'s chain, or null past the last. */
const char *isr_vector_name(int vector, int slot);

/** Every interrupt routed since the boot. */
uint32_t isr_interrupt_total(void);

/** How many handlers a vector's chain has. */
#define ISR_MAX_VECTOR_HANDLERS 4

/* The vectors a device may be given for its messages: the sixteen above
 * the 8259's, which are the ones the IDT has entries for; a seventeenth
 * device is refused. */
#define ISR_MSI_FIRST 48
#define ISR_MSI_LAST  63

/**
 * A vector for a device's messages, the lowest not yet given, or -1 when
 * every one is taken. A device asks once, when it enables its messages,
 * and `/proc/interrupts` reports the vector with the device's name, which
 * is how a reader finds it: the number is not fixed.
 */
int isr_alloc_vector(void);

/*
 * Syscall interrupt handler (int 0x80)
 */
extern void isr128(void);

/** Clear the handler table and enable interrupts. */
void init_isr(void);

/**
 * The double fault's task: report the fault, and the kernel stack that ran
 * out when that is its cause, and panic. The processor starts it through
 * the task gate of vector 8; nothing calls it.
 */
void isr_double_fault(void);

/** Append `handler` to a vector's chain. */
void register_isr_handler(uint8_t vector, isr_handler_t handler, const char *name);

/** Replace a vector's entire chain with a single `handler`. */
void set_isr_handler(uint8_t vector, isr_handler_t handler, const char *name);

#endif
