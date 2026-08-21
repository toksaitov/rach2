#include "gdt.h"
#include "tss.h"

gdt_descriptor_t gdt_descriptors[GDT_ENTRY_COUNT];
gdt_ptr_t gdt_address;

/**
 * Load the GDT pointer into GDTR and reload the segment registers.
 *
 * Writes `lgdt`, then reloads CS, DS, SS, ES, FS, and GS so the new
 * descriptors take effect. Implemented in gdt_handler.s.
 */
extern void _load_gdt(gdt_ptr_t*);

static void _gdt_set_descriptor(size_t index, uint32_t base,
                                              uint32_t limit,
                                              uint8_t  access,
                                              uint8_t  flags);

/**
 * Build the flat GDT and load it.
 *
 * Programs all seven GDT descriptors: a null descriptor, flat 4 GB code and
 * data for ring 0 and ring 3, the task-state segment used for the ring-0 stack
 * on traps, and a TLS data segment whose base is fixed up later by
 * `gdt_set_tls`. All segments span the full address space, so segmentation is
 * effectively disabled and paging does the protection.
 */
void init_gdt(void)
{
    uint32_t tss_base = (uint32_t)&kernel_tss;
    uint32_t tss_limit = sizeof(tss_t) - 1;

    gdt_address.base  = (uintptr_t) &gdt_descriptors;
    gdt_address.limit = sizeof(gdt_descriptor_t) * GDT_ENTRY_COUNT - 1;

    _gdt_set_descriptor(0, 0, 0, 0, 0);                /* Null descriptor */
    _gdt_set_descriptor(1, 0, 0xFFFFFFFF, 0x9A, 0xCF); /* Kernel code */
    _gdt_set_descriptor(2, 0, 0xFFFFFFFF, 0x92, 0xCF); /* Kernel data */
    _gdt_set_descriptor(3, 0, 0xFFFFFFFF, 0xFA, 0xCF); /* User code */
    _gdt_set_descriptor(4, 0, 0xFFFFFFFF, 0xF2, 0xCF); /* User data */
    /* TSS descriptor: access 0x89 = present, DPL 0, type 0x9 (32-bit TSS
     * available) */
    _gdt_set_descriptor(5, tss_base, tss_limit, 0x89, 0x00);
    /* TLS descriptor (entry 6): same shape as user data (present, DPL 3, 4 GB
     * R/W data); base is reprogrammed at runtime by set_thread_area and the
     * context switch. Present from boot so loading 0x33 into %gs never faults
     * even before a thread has set its area. */
    _gdt_set_descriptor(GDT_TLS_ENTRY, 0, 0xFFFFFFFF, 0xF2, 0xCF);

    _load_gdt(&gdt_address);
}

/**
 * Point the TLS segment at `base`.
 *
 * Rewrites only the base field of the TLS descriptor, split across its three
 * scattered bytes; access, flags, and limit stay as `init_gdt` programmed them
 * (a 4 GB DPL-3 data segment). Called from `set_thread_area`; the context
 * switch performs the same base rewrite inline in asm (switch.s) rather than
 * calling this, so %gs:0 always names the current thread's TLS block.
 */
void gdt_set_tls(uint32_t base)
{
    gdt_descriptors[GDT_TLS_ENTRY].base_low    = base & 0xFFFF;
    gdt_descriptors[GDT_TLS_ENTRY].base_middle = (base >> 16) & 0xFF;
    gdt_descriptors[GDT_TLS_ENTRY].base_high   = (base >> 24) & 0xFF;
}

/**
 * Pack one GDT entry from its raw fields.
 *
 * The descriptor format scatters the 32-bit `base` across three fields and the
 * 20-bit `limit` across two, with the high limit nibble sharing a byte with the
 * `flags`. `access` is the full access byte (present/DPL/type); `flags` carries
 * the granularity and size bits in its high nibble.
 */
static void _gdt_set_descriptor(size_t index, uint32_t base,
                                              uint32_t limit,
                                              uint8_t access,
                                              uint8_t flags)
{
    gdt_descriptors[index].base_low    = base & 0xFFFF;
    gdt_descriptors[index].base_middle = (base >> 16) & 0xFF;
    gdt_descriptors[index].base_high   = (base >> 24) & 0xFF;

    gdt_descriptors[index].limit_low = limit & 0xFFFF;
    gdt_descriptors[index].flags_and_limit_high = (limit >> 16) & 0x0F;

    gdt_descriptors[index].access = access;
    gdt_descriptors[index].flags_and_limit_high |= flags & 0xF0;
}
