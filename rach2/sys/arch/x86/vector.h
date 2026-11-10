/*
 * vector.h: the pool of interrupt vectors a device's messages may take, and
 * the run of entry stubs the IDT gives them.
 */

#ifndef _VECTOR_H_
#define _VECTOR_H_

/*
 * The vectors a device may be given for its messages: every one above the
 * 8259's lines, 32 to 47, up to the local APIC's own, the interprocessor
 * interrupt at 0xF0 and the spurious vector at 0xFF, less 0x80, the i386
 * system call's gate: 191 vectors. The APIC's timer raises line 0's vector
 * and its error entry stays masked, so neither takes one of these.
 */
#define ISR_MSI_FIRST      48
#define ISR_MSI_LAST       0xEF
#define ISR_SYSCALL_VECTOR 0x80
#define ISR_MSI_COUNT      (ISR_MSI_LAST - ISR_MSI_FIRST) /* less 0x80 */

/* The entry stubs of vectors ISR_MSI_FIRST to ISR_MSI_LAST, one every
 * ISR_MSI_STUB_BYTES bytes from `isr_msi_stubs` (isr_handler.s); the
 * stubs end at `isr_msi_stubs_end`. */
#define ISR_MSI_STUB_BYTES 16
extern char isr_msi_stubs[];
extern char isr_msi_stubs_end[];

/**
 * Take the lowest free vector of the pool that is a multiple of `align`, a
 * power of two from 1 to 32, for a device that writes its message number
 * into the data's low bits; another `align` is taken as 1. The vector, or
 * -1 when none is free. Called under the big lock.
 */
int vector_take(int align);

/** Give `vector` back to the pool: 1, or 0 for a vector outside the pool
 * or not taken, which is left alone. */
int vector_give(int vector);

/** Whether `vector` is taken. */
int vector_taken(int vector);

/** The vectors of the pool free now. */
int vector_free_count(void);

#endif /* _VECTOR_H_ */
