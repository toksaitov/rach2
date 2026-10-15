/*
 * tls.c: the i686 thread pointer of ../arch.h, set_thread_area(2) and
 * clone's CLONE_SETTLS argument, all through GDT entry GDT_TLS_ENTRY.
 */

#include "tls.h"

#include "gdt.h"

#include "../arch.h"

#include "../../lib/kerrno.h"
#include "../../mm/usercopy.h"
#include "../../proc/process.h"
#include "../../syscall.h"

/* switch.s has the TLS selector, 0x33 for entry 6, as a `.set` constant it
 * cannot take from gdt.h; the build stops if gdt.h's values differ. */
typedef char tls_gdt_entry_matches_asm[(GDT_TLS_ENTRY == 6) ? 1 : -1];
typedef char tls_gdt_selector_matches_asm[(GDT_TLS_SELECTOR == 0x33) ? 1 : -1];

/*
 * musl's __set_thread_area passes entry -1 with the flags 0x51 (32-bit,
 * limit in pages, usable); GDT_TLS_ENTRY, the number written back, is also
 * taken, so that a caller can set the same entry again.
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
        return -1;
    }
    if (u->flags & UD_READ_EXEC_ONLY) {
        return -1;
    }
    if (u->flags & UD_SEG_NOT_PRESENT) {
        return -1;
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

uintptr_t arch_tls_get(void)
{
    return current_proc ? current_proc->tls_base : 0;
}

/**
 * set_thread_area(2), as on Linux i386: the argument points at a struct
 * user_desc, which `tls_resolve_base` checks. The base goes into the running
 * processor's entry GDT_TLS_ENTRY and the record's `tls_base`, which switch.s
 * writes into the entry at every switch to the thread, and GDT_TLS_ENTRY is
 * written back into the descriptor's `entry_number`, from which the C
 * library makes the selector it loads into %gs; the %gs that the entry to
 * the kernel saved is left as it was.
 *
 * Returns 0; -ESRCH with no current process; -EINVAL for a descriptor
 * refused; -EFAULT when the descriptor cannot be read, or when the entry
 * number cannot be written back, in which case the base is already set.
 */
long sys_set_thread_area(isr_params_t *params)
{
    uintptr_t uptr = SC_ARG(params, 0);
    user_desc_t desc;
    uint32_t base = 0;

    if (!current_proc) {
        return -ESRCH;
    }

    if (copy_from_user(&desc, (const void __user *)uptr, sizeof(user_desc_t)) != 0) {
        return -EFAULT;
    }

    if (tls_resolve_base(&desc, &base) != 0) {
        return -EINVAL;
    }

    gdt_set_tls(base);
    current_proc->tls_base = base;

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
