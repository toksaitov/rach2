#ifndef _GDT_H_
#define _GDT_H_

#include "../lib/kdef.h"
#include "../lib/kint.h"

/*
 * The GDT descriptor structure
 */
struct gdt_descriptor {
    uint16_t limit_low;
    uint16_t base_low;
    uint8_t  base_middle;
    uint8_t  access;
    uint8_t  flags_and_limit_high;
    uint8_t  base_high;
} __attribute__((packed));

/*
 * The GDT descriptor (type)
 */
typedef struct gdt_descriptor gdt_descriptor_t;

/*
 * The GDT address structure
 */
struct gdt_ptr {
    uint16_t limit;
    uint32_t base;
} __attribute__((packed));

/*
 * The GDT address (type)
 */
typedef struct gdt_ptr gdt_ptr_t;

/*
 * Number of GDT entries (null, kernel code/data, user code/data, TSS, TLS)
 */
#define GDT_ENTRY_COUNT 7

/*
 * Thread-local-storage descriptor slot. set_thread_area programs its base to
 * the per-process TCB and the context switch rewrites it on every switch.
 * Mirrors the dedicated TLS GDT entry Linux i386 uses (GDT_ENTRY_TLS_MIN).
 */
#define GDT_TLS_ENTRY    6
#define GDT_TLS_SELECTOR ((GDT_TLS_ENTRY << 3) | 3) /* index 6, RPL 3 -> 0x33 */

/*
 * The Global Descriptor Table.
 */
extern gdt_descriptor_t gdt_descriptors[GDT_ENTRY_COUNT];

/*
 * The GDT location and size
 */
extern gdt_ptr_t gdt_address;

/*
 * Initializes the Global Descriptor Table to flat setup.
 */
void init_gdt(void);

/*
 * Rewrites the base of the shared TLS descriptor (GDT entry GDT_TLS_ENTRY),
 * leaving its access/flags/limit (a 4 GB DPL-3 data segment) untouched. Called
 * by set_thread_area and, in asm, by the context switch for the incoming
 * process -- the i386 equivalent of Linux load_TLS() on __switch_to.
 */
void gdt_set_tls(uint32_t base);

#endif
