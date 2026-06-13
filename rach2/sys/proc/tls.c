#include "tls.h"

#include "../tables/gdt.h"

/*
 * proc/switch.s hard-codes the TLS GDT entry (6) and selector (0x33) as .set
 * constants it cannot share with these headers. Fail the build if the C
 * definitions ever drift from those asm literals.
 */
typedef char tls_gdt_entry_matches_asm[(GDT_TLS_ENTRY == 6) ? 1 : -1];
typedef char tls_gdt_selector_matches_asm[(GDT_TLS_SELECTOR == 0x33) ? 1 : -1];

/*
 * Validate a Linux i386 struct user_desc against the one TLS slot we model -- a
 * present, 32-bit, writable data segment in GDT entry GDT_TLS_ENTRY -- and hand
 * back the base address to program. We reject anything we cannot represent
 * rather than silently misprogramming the descriptor.
 *
 * musl's __set_thread_area passes entry_number = -1 (allocate) with
 * flags = 0x51 (seg_32bit | limit_in_pages | useable); we also accept an
 * explicit GDT_TLS_ENTRY so a caller can reprogram the same slot.
 */
int tls_resolve_base(const user_desc_t *u, uint32_t *base_out)
{
    if (u == NULL || base_out == NULL)
    {
        return -1;
    }

    /* Single TLS slot: honor the allocate request (-1) or an explicit entry 6. */
    if (u->entry_number != (uint32_t)-1 &&
        u->entry_number != (uint32_t)GDT_TLS_ENTRY)
    {
        return -1;
    }

    /* We model a 32-bit, writable, present data segment and nothing else. */
    if (!(u->flags & UD_SEG_32BIT))
    {
        return -1;
    }
    if (u->flags & UD_CONTENTS_MASK)
    {
        return -1;  /* code/stack segment requested -- not our data TLS slot */
    }
    if (u->flags & UD_READ_EXEC_ONLY)
    {
        return -1;  /* TLS must be writable */
    }
    if (u->flags & UD_SEG_NOT_PRESENT)
    {
        return -1;  /* we keep the TLS slot present */
    }

    *base_out = u->base_addr;
    return 0;
}
