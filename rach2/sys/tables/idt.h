#ifndef _IDT_H_
#define _IDT_H_

#include "../isr.h"
#include "../pic.h"

#include "../lib/kstring.h"

/**
 * One 8-byte IDT gate. The 32-bit handler address is split across
 * `offset_low` (bits 0..15) and `offset_high` (bits 16..31); `flags` is the
 * gate's type/DPL/present byte. Packed to the hardware layout.
 */
struct idt_descriptor {
    uint16_t offset_low;
    uint16_t segment_selector;
    uint8_t  reserved;
    uint8_t  flags;
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

/** Builds the IDT and loads it into the IDTR. */
void init_idt(void);

#endif
