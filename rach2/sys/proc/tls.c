#include "tls.h"

#include "../tables/gdt.h"

/*
 * proc/switch.s hard-codes the TLS GDT entry (6) and selector (0x33) as .set
 * constants it cannot share with these headers. Fail the build if the C
 * definitions ever drift from those asm literals.
 */
typedef char tls_gdt_entry_matches_asm[(GDT_TLS_ENTRY == 6) ? 1 : -1];
typedef char tls_gdt_selector_matches_asm[(GDT_TLS_SELECTOR == 0x33) ? 1 : -1];

/**
 * Validate a Linux i386 struct user_desc and return its base address.
 *
 * Checks the descriptor against the one TLS slot we model: a present, 32-bit,
 * writable data segment in GDT entry GDT_TLS_ENTRY. Anything we cannot
 * represent is rejected, not silently misprogrammed.
 *
 * musl's __set_thread_area passes entry_number = -1 (allocate) with flags =
 * 0x51 (seg_32bit | limit_in_pages | useable); an explicit GDT_TLS_ENTRY is
 * also accepted so a caller can reprogram the same slot.
 *
 * @return 0 with `*base_out` set, or -1 if the descriptor is unrepresentable.
 */
int tls_resolve_base(const user_desc_t *u, uint32_t *base_out)
{
    if (u == NULL || base_out == NULL) {
        return -1;
    }

    if (u->entry_number != (uint32_t)-1 && u->entry_number != (uint32_t)GDT_TLS_ENTRY) {
        return -1;
    }

    if (!(u->flags & UD_SEG_32BIT)) {
        return -1;
    }
    if (u->flags & UD_CONTENTS_MASK) {
        return -1; /* code/stack segment requested, not our data TLS slot */
    }
    if (u->flags & UD_READ_EXEC_ONLY) {
        return -1; /* TLS must be writable */
    }
    if (u->flags & UD_SEG_NOT_PRESENT) {
        return -1; /* we keep the TLS slot present */
    }

    *base_out = u->base_addr;
    return 0;
}
