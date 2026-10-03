#ifndef _GDT_H_
#define _GDT_H_

#include "../../lib/kdef.h"
#include "../../lib/kint.h"
#include "../../proc/percpu.h"

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

/** Number of GDT entries: null, kernel code/data, user code/data, TSS, TLS,
 *  the double fault's TSS, and the processor's own data segment. */
#define GDT_ENTRY_COUNT 9

/**
 * Thread-local-storage descriptor slot. `set_thread_area` programs its base to
 * the per-process TCB and the context switch rewrites it on every switch.
 * Mirrors the dedicated TLS GDT entry Linux i386 uses (GDT_ENTRY_TLS_MIN).
 */
#define GDT_TLS_ENTRY    6
#define GDT_TLS_SELECTOR ((GDT_TLS_ENTRY << 3) | 3) /* index 6, RPL 3 -> 0x33 */

/**
 * The processor's own segment, entry 8 at RPL 0: a small data segment whose
 * base is the processor's `struct i686_cpu`, which every entry to the kernel
 * loads into %fs, so that `arch_cpu_id` is one load from %fs:0 (cpu.h). Each
 * processor has a GDT of its own, so the one selector gives each its own
 * base.
 */
#define GDT_PERCPU_ENTRY    8
#define GDT_PERCPU_SELECTOR (GDT_PERCPU_ENTRY << 3) /* 0x40 */

/** The flat user data selector, index 4 at RPL 3, which every user context
 *  runs with in ds, es, fs and ss. */
#define GDT_USER_DATA_SELECTOR 0x23u

/** The flat user code selector, index 3 at RPL 3, which every user context
 *  runs with in cs. */
#define GDT_USER_CODE_SELECTOR 0x1Bu

/** What the processor's own segment points at: its index, at %fs:0. */
struct i686_cpu {
    uint32_t id;
};

/** Each processor's Global Descriptor Table. */
extern gdt_descriptor_t gdt_descriptors[NR_CPUS][GDT_ENTRY_COUNT];

/** Build the boot processor's GDT and load it: the loader calls this before
 * the first C code, so that %fs has the processor's own segment from then
 * on, and the boot calls it again with the rest of the processor's tables. */
void init_gdt(void);

/** Build processor `cpu`'s GDT and load it on the processor running this. */
void init_gdt_cpu(uint32_t cpu);

/** Point the running processor's TLS descriptor (GDT entry GDT_TLS_ENTRY)
 * at `base`. */
void gdt_set_tls(uint32_t base);

#endif
