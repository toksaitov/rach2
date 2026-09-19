#ifndef _GDT_H_
#define _GDT_H_

#include "../lib/kdef.h"
#include "../lib/kint.h"

/** One GDT entry in the packed x86 descriptor format. */
struct gdt_descriptor {
    uint16_t limit_low;
    uint16_t base_low;
    uint8_t base_middle;
    uint8_t access;
    uint8_t flags_and_limit_high;
    uint8_t base_high;
} __attribute__((packed));

typedef struct gdt_descriptor gdt_descriptor_t;

/** The GDTR operand: table limit and base address. */
struct gdt_ptr {
    uint16_t limit;
    uint32_t base;
} __attribute__((packed));

typedef struct gdt_ptr gdt_ptr_t;

/** Number of GDT entries: null, kernel code/data, user code/data, TSS, TLS. */
#define GDT_ENTRY_COUNT 7

/**
 * Thread-local-storage descriptor slot. `set_thread_area` programs its base to
 * the per-process TCB and the context switch rewrites it on every switch.
 * Mirrors the dedicated TLS GDT entry Linux i386 uses (GDT_ENTRY_TLS_MIN).
 */
#define GDT_TLS_ENTRY    6
#define GDT_TLS_SELECTOR ((GDT_TLS_ENTRY << 3) | 3) /* index 6, RPL 3 -> 0x33 */

/** The Global Descriptor Table. */
extern gdt_descriptor_t gdt_descriptors[GDT_ENTRY_COUNT];

/** The GDTR operand naming the table's base and limit. */
extern gdt_ptr_t gdt_address;

/** Build the flat GDT and load it. */
void init_gdt(void);

/** Point the TLS descriptor (GDT entry GDT_TLS_ENTRY) at `base`. */
void gdt_set_tls(uint32_t base);

#endif
