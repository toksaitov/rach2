/*
 * tls.c: the x86-64's thread pointer and arch_prctl, and the TLS
 * descriptors of 32-bit code with set_thread_area and get_thread_area, as
 * tls.h describes them.
 */

#include "tls.h"
#include "percpu.h"

#include "../../lib/kerrno.h"
#include "../../mm/usercopy.h"
#include "../../proc/process.h"
#include "../../syscall.h"

/** x86-64's thread pointer is FS's base, IA32_FS_BASE. */
void arch_tls_set(uintptr_t base)
{
    msr_write(MSR_FS_BASE, base);
}

/** The processor's FS base, which a selector loaded into FS may have
 * changed since the record was written. */
uintptr_t arch_tls_get(void)
{
    return (uintptr_t)msr_read(MSR_FS_BASE);
}

/** Clone's TLS argument on x86-64 is the base itself, which must be a user
 * address, as ARCH_SET_FS takes one. */
long arch_clone_tls(unsigned long arg, uintptr_t *base_out)
{
    if (arg >= USER_ADDR_MAX) {
        return -EPERM;
    }
    *base_out = arg;
    return 0;
}

long sys_arch_prctl(isr_params_t *params)
{
    unsigned long code = SC_ARG(params, 0);
    unsigned long addr = SC_ARG(params, 1);
    uint64_t base;

    if (!current_proc) {
        return -ESRCH;
    }
    switch (code) {
    case ARCH_SET_FS:
        if (addr >= USER_ADDR_MAX) {
            return -EPERM;
        }
        x86_64_fs_load(0);
        current_proc->tls_base = addr;
        msr_write(MSR_FS_BASE, addr);
        return 0;
    case ARCH_SET_GS:
        if (addr >= USER_ADDR_MAX) {
            return -EPERM;
        }
        x86_64_gs_load(0);
        current_proc->regs.gs_base = addr;
        /* While the kernel runs, the program's GS base is in this MSR. */
        msr_write(MSR_KERNEL_GS_BASE, addr);
        return 0;
    case ARCH_GET_FS:
    case ARCH_GET_GS:
        base = msr_read(code == ARCH_GET_FS ? MSR_FS_BASE : MSR_KERNEL_GS_BASE);
        if (copy_to_user((void __user *)addr, &base, sizeof(base)) != 0) {
            return -EFAULT;
        }
        return 0;
    default:
        return -EINVAL;
    }
}

/* The fields of a code or data descriptor (Intel SDM Vol. 3A, 3.4.5). */
#define DESC_TYPE_SHIFT 40
#define DESC_S          (1UL << 44)
#define DESC_DPL3       (3UL << 45)
#define DESC_P          (1UL << 47)
#define DESC_AVL        (1UL << 52)
#define DESC_L          (1UL << 53)
#define DESC_D          (1UL << 54)
#define DESC_G          (1UL << 55)

/** Whether `u` is Linux's `LDT_empty` form, which asks for no segment. */
static int ud_empty(const struct user_desc *u)
{
    return u->base_addr == 0 && u->limit == 0 &&
           (u->flags & 0xFFu) == (UD_READ_EXEC_ONLY | UD_SEG_NOT_PRESENT);
}

/** Whether `u` is all zero but its entry and the long-mode bit, which old
 * programs pass for no segment and Linux takes so (`LDT_zero`). */
static int ud_zero(const struct user_desc *u)
{
    return u->base_addr == 0 && u->limit == 0 && (u->flags & 0x7Fu) == 0;
}

/** Linux's `tls_desc_okay`: no segment at all, or a present 32-bit data
 * segment. */
static int ud_okay(const struct user_desc *u)
{
    if (ud_empty(u) || ud_zero(u)) {
        return 1;
    }
    return (u->flags & UD_SEG_32BIT) &&
           ((u->flags & UD_CONTENTS_MASK) >> UD_CONTENTS_SHIFT) <= 1u &&
           !(u->flags & UD_SEG_NOT_PRESENT);
}

/** The descriptor `u` describes, as Linux's `fill_ldt` makes it: DPL 3,
 * accessed, writable unless read_exec_only, and never a long-mode one; 0
 * for no segment. */
static uint64_t ud_to_desc(const struct user_desc *u)
{
    uint64_t d, base = u->base_addr, limit = u->limit, type;

    if (ud_empty(u) || ud_zero(u)) {
        return 0;
    }
    type = 1u | ((u->flags & UD_READ_EXEC_ONLY) ? 0u : 2u) |
           (((u->flags & UD_CONTENTS_MASK) >> UD_CONTENTS_SHIFT) << 2);
    d = (limit & 0xFFFFu) | ((base & 0xFFFFFFu) << 16) | (type << DESC_TYPE_SHIFT) | DESC_S |
        DESC_DPL3 | (((limit >> 16) & 0xFu) << 48) | ((base >> 24) << 56);
    if (!(u->flags & UD_SEG_NOT_PRESENT)) {
        d |= DESC_P;
    }
    if (u->flags & UD_USEABLE) {
        d |= DESC_AVL;
    }
    if (u->flags & UD_SEG_32BIT) {
        d |= DESC_D;
    }
    if (u->flags & UD_LIMIT_IN_PAGES) {
        d |= DESC_G;
    }
    return d;
}

/** The `struct user_desc` of TLS entry `entry` holding `d`, as Linux's
 * `fill_user_desc` writes it. */
static void desc_to_ud(uint64_t d, uint32_t entry, struct user_desc *u)
{
    uint32_t type = (uint32_t)(d >> DESC_TYPE_SHIFT) & 0xFu;

    u->entry_number = entry;
    u->base_addr = (uint32_t)(((d >> 16) & 0xFFFFFFu) | ((d >> 56) << 24));
    u->limit = (uint32_t)((d & 0xFFFFu) | (((d >> 48) & 0xFu) << 16));
    u->flags = 0;
    if (d & DESC_D) {
        u->flags |= UD_SEG_32BIT;
    }
    u->flags |= (type >> 2) << UD_CONTENTS_SHIFT;
    if (!(type & 2u)) {
        u->flags |= UD_READ_EXEC_ONLY;
    }
    if (d & DESC_G) {
        u->flags |= UD_LIMIT_IN_PAGES;
    }
    if (!(d & DESC_P)) {
        u->flags |= UD_SEG_NOT_PRESENT;
    }
    if (d & DESC_AVL) {
        u->flags |= UD_USEABLE;
    }
    if (d & DESC_L) {
        u->flags |= UD_LM;
    }
}

/**
 * Load `sel` again into each of DS, ES, FS and GS that holds it, after its
 * descriptor changed to `d`, as Linux does; with the null selector where
 * `d` is no segment, whose load would fault. A load into FS or GS sets its
 * base, which the record then takes, as the switch away would.
 */
static void tls_reload(unsigned int sel, uint64_t d)
{
    unsigned int to = d != 0 ? sel : 0u;

    if (x86_64_ds_read() == sel) {
        x86_64_ds_load(to);
    }
    if (x86_64_es_read() == sel) {
        x86_64_es_load(to);
    }
    if (x86_64_fs_read() == sel) {
        x86_64_fs_load(to);
        if (to == 0) {
            msr_write(MSR_FS_BASE, 0);
        }
        current_proc->tls_base = msr_read(MSR_FS_BASE);
    }
    if (x86_64_gs_read() == sel) {
        x86_64_gs_load(to);
        if (to == 0) {
            msr_write(MSR_KERNEL_GS_BASE, 0);
        }
        current_proc->regs.gs_base = msr_read(MSR_KERNEL_GS_BASE);
    }
}

/**
 * set_thread_area(2), i386's 243, through `int $0x80`, as Linux's 64-bit
 * kernel has it: EBX points at a `struct user_desc`. Its `entry_number` is
 * a TLS entry, 12 to 14, or -1 for the first empty one, which is written
 * back. A descriptor Linux calls empty (`LDT_empty`) or all zero clears the
 * entry; any other must be a present 32-bit data segment, and becomes a
 * DPL 3 descriptor of its base, limit and flags, with the long-mode bit
 * clear. The entry is the calling thread's, in its record and in the
 * processor's GDT, and a segment register that holds its selector is
 * loaded again, with the null selector when the entry was cleared.
 *
 * Returns 0; -EFAULT when the descriptor cannot be read or the entry number
 * written back; -EINVAL for a descriptor refused or an entry outside 12 to
 * 14; -ESRCH when -1 finds no empty entry, or with no current process.
 */
/**
 * Read the `struct user_desc` at `uptr` and make its descriptor, as Linux's
 * `do_set_thread_area` does before it sets one: the entry, 12 to 14, into
 * `*entry`, where -1 picks the calling thread's first empty one and writes
 * it back, and the descriptor into `*desc`. 0; -EFAULT when the structure
 * cannot be read or the entry written back; -EINVAL for a descriptor
 * refused or an entry outside 12 to 14; -ESRCH when -1 finds none empty.
 */
static long tls_desc_from_user(uintptr_t uptr, uint32_t *entry, uint64_t *desc)
{
    struct user_desc u;
    int i;

    if (copy_from_user(&u, (const void __user *)uptr, sizeof(u)) != 0) {
        return -EFAULT;
    }
    if (!ud_okay(&u)) {
        return -EINVAL;
    }
    *entry = u.entry_number;
    if (*entry == (uint32_t)-1) {
        i = 0;
        while (i < ARCH_TLS_ENTRIES && current_proc->regs.tls[i] != 0) {
            i++;
        }
        if (i == ARCH_TLS_ENTRIES) {
            return -ESRCH;
        }
        *entry = (uint32_t)(GDT_TLS_MIN + i);
        if (copy_to_user((void __user *)uptr, entry, sizeof(*entry)) != 0) {
            return -EFAULT;
        }
    }
    if (*entry < GDT_TLS_MIN || *entry > GDT_TLS_MAX) {
        return -EINVAL;
    }
    *desc = ud_to_desc(&u);
    return 0;
}

long sys_set_thread_area(isr_params_t *params)
{
    uint32_t entry;
    uint64_t d;
    long rc;

    if (!current_proc) {
        return -ESRCH;
    }
    rc = tls_desc_from_user(SC_ARG(params, 0), &entry, &d);
    if (rc != 0) {
        return rc;
    }
    current_proc->regs.tls[entry - GDT_TLS_MIN] = d;
    cpu_self()->gdt[entry] = d;
    tls_reload((entry << 3) | 3u, d);
    return 0;
}

/**
 * clone's CLONE_SETTLS in an i386 call, whose TLS argument at `uptr` is a
 * `struct user_desc`, as Linux's `set_new_tls` takes it from a compat call:
 * the entry and descriptor the child is to have, read and checked here in
 * the parent, as set_thread_area checks one, an entry of -1 picking the
 * parent's first empty one, which the child's copy of the entries has
 * empty too. The child's thread pointer stays the parent's.
 */
long ia32_clone_tls(unsigned long uptr, uint32_t *entry, uint64_t *desc)
{
    return tls_desc_from_user(uptr, entry, desc);
}

/** Give `child`, not yet run, the TLS descriptor `desc` in entry `entry`,
 * which `ia32_clone_tls` made; its first switch writes it into the GDT. */
void ia32_clone_tls_set(process_t *child, uint32_t entry, uint64_t desc)
{
    child->regs.tls[entry - GDT_TLS_MIN] = desc;
}

uint32_t ia32_tls_get(const process_t *t, struct user_desc *u)
{
    uint32_t i, set = 0;

    for (i = 0; i <= GDT_TLS_MAX - GDT_TLS_MIN; i++) {
        desc_to_ud(t->regs.tls[i], GDT_TLS_MIN + i, &u[i]);
        set += t->regs.tls[i] != 0;
    }
    return set;
}

/**
 * get_thread_area(2), i386's 244: EBX points at a `struct user_desc` whose
 * `entry_number`, 12 to 14, names the TLS entry to describe; the whole
 * structure is written, an empty entry as Linux's `LDT_empty` form.
 * Returns 0, -EFAULT, -EINVAL for another entry, or -ESRCH with no current
 * process.
 */
long sys_get_thread_area(isr_params_t *params)
{
    uintptr_t uptr = SC_ARG(params, 0);
    struct user_desc u;
    uint32_t entry;

    if (!current_proc) {
        return -ESRCH;
    }
    if (copy_from_user(&entry, (const void __user *)uptr, sizeof(entry)) != 0) {
        return -EFAULT;
    }
    if (entry < GDT_TLS_MIN || entry > GDT_TLS_MAX) {
        return -EINVAL;
    }
    desc_to_ud(current_proc->regs.tls[entry - GDT_TLS_MIN], entry, &u);
    if (copy_to_user((void __user *)uptr, &u, sizeof(u)) != 0) {
        return -EFAULT;
    }
    return 0;
}
