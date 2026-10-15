/* The i686 Interrupt Descriptor Table, which every processor shares. */
#ifndef _IDT_H_
#define _IDT_H_

#include "../x86/isr.h"
#include "../x86/pic.h"

#include "../../lib/kstring.h"

/**
 * One 8-byte IDT gate. The 32-bit handler address is split across
 * `offset_low` (bits 0..15) and `offset_high` (bits 16..31); `flags` is the
 * gate's type/DPL/present byte. Packed to the hardware layout.
 */
struct idt_descriptor {
    uint16_t offset_low;
    uint16_t segment_selector;
    uint8_t reserved;
    uint8_t flags;
    uint16_t offset_high;
} __attribute__((packed));

typedef struct idt_descriptor idt_descriptor_t;

/**
 * The operand for `lidt`: table byte limit and base address. Packed to the
 * hardware layout.
 */
struct idt_ptr {
    uint16_t limit;
    uint32_t base;
} __attribute__((packed));

typedef struct idt_ptr idt_ptr_t;

/** The 256-gate Interrupt Descriptor Table. */
extern idt_descriptor_t idt_descriptors[256];

/** The IDTR operand pointing at `idt_descriptors`. */
extern idt_ptr_t idt_address;

/**
 * Build the IDT and load it on this processor: gates for the exceptions,
 * the 8259's sixteen lines on vectors 32 to 47 (it calls `init_pic`, which
 * moves them there), the messages on 48 to 63, the local APIC's spurious
 * vector and IPI_VECTOR, and the system call's `int 0x80`. The boot calls
 * it once, from `platform_cpu_init`; a processor started later loads the
 * same table with `idt_load`.
 */
void init_idt(void);

#endif
