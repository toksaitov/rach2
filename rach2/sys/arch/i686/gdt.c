#include "gdt.h"
#include "tss.h"
#include "../x86/smpboot.h"

gdt_descriptor_t gdt_descriptors[NR_CPUS][GDT_ENTRY_COUNT];
static gdt_ptr_t gdt_address[NR_CPUS];
static struct i686_cpu cpus[NR_CPUS];

/**
 * Load the GDT pointer into GDTR and reload the segment registers.
 *
 * Writes `lgdt`, then reloads CS, DS, SS, ES and GS with the flat kernel
 * segments and FS with the processor's own, so the new descriptors take
 * effect. Implemented in gdt_handler.s.
 */
extern void _load_gdt(gdt_ptr_t *);

static void _gdt_set_descriptor(gdt_descriptor_t *gdt, size_t index, uint32_t base, uint32_t limit,
                                uint8_t access, uint8_t flags);

/**
 * Build processor `cpu`'s GDT and load it.
 *
 * Programs all nine descriptors: a null descriptor, flat 4 GB code and data
 * for ring 0 and ring 3, the processor's task-state segment, whose ring-0
 * stack a trap from user mode takes, a TLS data segment whose base is fixed
 * up later by `gdt_set_tls`, the processor's double fault task-state
 * segment, which the gate of vector 8 names, and the processor's own data
 * segment, which `_load_gdt` loads into %fs. The flat segments span the
 * full address space, so segmentation is effectively disabled and paging
 * does the protection.
 */
void init_gdt_cpu(uint32_t cpu)
{
    gdt_descriptor_t *gdt = gdt_descriptors[cpu];
    uint32_t tss_limit = sizeof(tss_t) - 1;

    cpus[cpu].id = cpu;
    gdt_address[cpu].base = (uintptr_t)gdt;
    gdt_address[cpu].limit = sizeof(gdt_descriptor_t) * GDT_ENTRY_COUNT - 1;

    _gdt_set_descriptor(gdt, 0, 0, 0, 0, 0);                /* Null descriptor */
    _gdt_set_descriptor(gdt, 1, 0, 0xFFFFFFFF, 0x9A, 0xCF); /* Kernel code */
    _gdt_set_descriptor(gdt, 2, 0, 0xFFFFFFFF, 0x92, 0xCF); /* Kernel data */
    _gdt_set_descriptor(gdt, 3, 0, 0xFFFFFFFF, 0xFA, 0xCF); /* User code */
    _gdt_set_descriptor(gdt, 4, 0, 0xFFFFFFFF, 0xF2, 0xCF); /* User data */
    /* TSS descriptor: access 0x89 = present, DPL 0, type 0x9 (32-bit TSS
     * available) */
    _gdt_set_descriptor(gdt, 5, (uint32_t)&kernel_tss[cpu], tss_limit, 0x89, 0x00);
    /* TLS descriptor (entry 6): same shape as user data (present, DPL 3, 4 GB
     * R/W data); base is reprogrammed at runtime by set_thread_area and the
     * context switch. Present from boot so loading 0x33 into %gs never faults
     * even before a thread has set its area. */
    _gdt_set_descriptor(gdt, GDT_TLS_ENTRY, 0, 0xFFFFFFFF, 0xF2, 0xCF);
    /* The double fault's TSS, entry 7: the same access byte as entry 5's. */
    _gdt_set_descriptor(gdt, DOUBLE_FAULT_TSS_SELECTOR >> 3, (uint32_t)&double_fault_tss[cpu],
                        tss_limit, 0x89, 0x00);
    /* The processor's own segment, entry 8: present, DPL 0, read/write data,
     * byte granular, just long enough for its record. */
    _gdt_set_descriptor(gdt, GDT_PERCPU_ENTRY, (uint32_t)&cpus[cpu], sizeof(cpus[cpu]) - 1, 0x92,
                        0x40);

    _load_gdt(&gdt_address[cpu]);
}

void init_gdt(void)
{
    init_gdt_cpu(0);
}

/**
 * The i686's part of a started processor's start: its GDT, which also gives
 * %fs its own segment, before anything reads `this_cpu()`, its TSS with the
 * stack `stack_top`, and the IDT every processor shares. The trampoline
 * has already given it the kernel's page directory.
 */
void arch_ap_tables(int cpu, uintptr_t stack_top)
{
    init_gdt_cpu((uint32_t)cpu);
    init_tss_cpu((uint32_t)cpu, (uint32_t)stack_top);
    idt_load();
}

/**
 * Point the running processor's TLS segment at `base`.
 *
 * Rewrites only the base field of the TLS descriptor, split across its three
 * scattered bytes; access, flags, and limit stay as `init_gdt_cpu` programmed
 * them (a 4 GB DPL-3 data segment). Called from `set_thread_area` and from
 * the context switch (switch.s), so %gs:0 always names the current thread's
 * TLS block. A processor's GDT is its own, so a thread running on another
 * processor keeps its base there.
 */
void gdt_set_tls(uint32_t base)
{
    gdt_descriptor_t *d = &gdt_descriptors[arch_cpu_id()][GDT_TLS_ENTRY];

    d->base_low = base & 0xFFFF;
    d->base_middle = (base >> 16) & 0xFF;
    d->base_high = (base >> 24) & 0xFF;
}

/**
 * Pack one GDT entry from its raw fields.
 *
 * The descriptor format scatters the 32-bit `base` across three fields and the
 * 20-bit `limit` across two, with the high limit nibble sharing a byte with the
 * `flags`. `access` is the full access byte (present/DPL/type); `flags` has
 * the granularity and size bits in its high nibble.
 */
static void _gdt_set_descriptor(gdt_descriptor_t *gdt, size_t index, uint32_t base, uint32_t limit,
                                uint8_t access, uint8_t flags)
{
    gdt[index].base_low = base & 0xFFFF;
    gdt[index].base_middle = (base >> 16) & 0xFF;
    gdt[index].base_high = (base >> 24) & 0xFF;

    gdt[index].limit_low = limit & 0xFFFF;
    gdt[index].flags_and_limit_high = (limit >> 16) & 0x0F;

    gdt[index].access = access;
    gdt[index].flags_and_limit_high |= flags & 0xF0;
}
