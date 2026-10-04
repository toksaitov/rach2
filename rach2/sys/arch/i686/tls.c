#include "tls.h"

#include "gdt.h"

#include "../arch.h"

#include "../../lib/kerrno.h"
#include "../../mm/usercopy.h"
#include "../../proc/process.h"
#include "../../syscall.h"

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
 * The descriptor must ask for the one TLS slot the kernel has: a present,
 * 32-bit, writable data segment in GDT entry GDT_TLS_ENTRY; a request for
 * any other entry or kind of segment is refused. Only the base is taken: the
 * limit, its granularity bit and the usable bit are ignored, since the slot
 * always has a 4 GiB limit.
 *
 * musl's __set_thread_area passes entry_number = -1 (allocate) with flags =
 * 0x51 (seg_32bit | limit_in_pages | useable); an explicit GDT_TLS_ENTRY is
 * also accepted so a caller can reprogram the same slot.
 *
 * @return 0 with `*base_out` set, or -1 for a refused descriptor or a NULL
 *         argument.
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

/** The thread pointer of ../arch.h: the base of the GDT's TLS entry, which
 * the selector a program loaded into %gs reads through. */
void arch_tls_set(uintptr_t base)
{
    gdt_set_tls((uint32_t)base);
}

/**
 * Set up thread-local storage (set_thread_area(2), Linux i386).
 *
 * The argument points at a struct user_desc. Validates it, programs the shared
 * TLS GDT descriptor (entry GDT_TLS_ENTRY) and this process's PCB base, then
 * writes the chosen entry number back so the C library can load (entry_number
 * << 3) | 3 into %gs. The base is reapplied to the GDT on every context switch
 * (switch.s). Returns 0 or a negative errno on a bad pointer/request.
 */
long sys_set_thread_area(isr_params_t *params)
{
    uintptr_t uptr = SC_ARG(params, 0);
    user_desc_t desc;
    uint32_t base = 0;

    if (!current_proc) {
        return -ESRCH; /* TLS is only meaningful for a user process */
    }

    /* Read the descriptor through the fault-checked helper (it range-checks the
     * full struct internally, so the prior explicit user_access_ok is
     * redundant). */
    if (copy_from_user(&desc, (const void __user *)uptr, sizeof(user_desc_t)) != 0) {
        return -EFAULT;
    }

    if (tls_resolve_base(&desc, &base) != 0) {
        return -EINVAL;
    }

    /* Program the descriptor now and remember the base so the context switch
     * can reprogram entry GDT_TLS_ENTRY for this process when it is resumed. */
    gdt_set_tls(base);
    current_proc->tls_base = base;

    /* Report the allocated slot back (musl reads user_desc.entry_number). */
    desc.entry_number = GDT_TLS_ENTRY;
    if (copy_to_user((void __user *)uptr, &desc.entry_number, sizeof(desc.entry_number)) != 0) {
        return -EFAULT;
    }

    return 0;
}

/** Clone's TLS argument on an i686, a pointer to a struct user_desc as
 * set_thread_area takes, read and resolved to the base it describes. */
long arch_clone_tls(unsigned long arg, uintptr_t *base_out)
{
    user_desc_t desc;
    uint32_t base = 0;

    if (copy_from_user(&desc, (const void __user *)arg, sizeof(desc)) != 0) {
        return -EFAULT;
    }
    if (tls_resolve_base(&desc, &base) != 0) {
        return -EINVAL;
    }
    *base_out = base;
    return 0;
}
