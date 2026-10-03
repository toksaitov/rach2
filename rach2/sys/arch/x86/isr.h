#ifndef _ISR_H_
#define _ISR_H_

#include "../../kio.h"
#include "pic.h"

#include "../../irq.h"

#include "../../lib/kint.h"
#include "../../lib/kstring.h"

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

/* The vectors a device may be given for its messages: the sixteen above
 * the 8259's, which are the ones the IDT has entries for; a seventeenth
 * device is refused. */
#define ISR_MSI_FIRST 48
#define ISR_MSI_LAST  63

/*
 * Syscall interrupt handler (int 0x80)
 */
extern void isr128(void);

/** Clear the handler table and enable interrupts. */
void init_isr(void);

/**
 * Hand lines 0 to 15 to the I/O APIC: route each line a handler is already
 * registered on, and each one registered from now on. `pci` has a bit for
 * each line a PCI device's interrupt line register gives, which is
 * level-triggered; `local` has a bit for each line whose vector a source in
 * the local APIC raises instead (the timer, on line 0's vector), which is
 * not routed. Called with interrupts off, after the 8259 is retired.
 */
void isr_lines_to_apic(uint16_t pci, uint16_t local);

/** Whether the I/O APIC has the lines. */
int isr_lines_apic(void);

/**
 * Hold line `line` masked at the I/O APIC whatever is registered on its
 * vector: `pic_mask_irq` once the 8259 is retired, for a line a device
 * asserts that no handler here clears, the SCI's.
 */
void isr_line_hold(int line);

/**
 * The double fault's task: report the fault, and the kernel stack that ran
 * out when that is its cause, and panic. The processor starts it through
 * the task gate of vector 8; nothing calls it.
 */
void isr_double_fault(void);

#endif
