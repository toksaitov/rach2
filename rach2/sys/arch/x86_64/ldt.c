/*
 * ldt.c: modify_ldt(2) and the local descriptor table of an address space,
 * which 32-bit code and the segments of 16-bit code load selectors of, for
 * the native and the compat table (systab.c, ia32_systab.c).
 */

#include "ia32.h"
#include "percpu.h"
#include "tls.h"

#include "../../lib/kstring.h"
#include "../../mm/kheap.h"
#include "../../mm/mm.h"
#include "../../mm/usercopy.h"
#include "../../proc/process.h"

/*
 * An address space's LDT is Linux's largest, LDT_ENTRIES descriptors of 8
 * bytes, from the heap at the first write and never moved, so a processor
 * that has it loaded never reads a freed table: a write changes a
 * descriptor in place, one 8-byte store, and the table goes only with the
 * address space (`arch_mm_release`). `ldt_entries` counts the entries up to
 * the highest written, as Linux's table has them, which modify_ldt's read
 * gives. The LDT register's limit spans the whole table, whose unwritten
 * entries are zero, not present, so a selector past the written ones
 * faults with #GP as past the end of Linux's.
 *
 * Every thread of the address space shares the table, as on Linux. The
 * switch loads the incoming thread's (`ldt_switch`); a processor running
 * another thread of the space when the table is made loads it at that
 * thread's first #GP on a selector of an LDT (`ldt_fault`), where Linux
 * sends an interprocessor interrupt to load it at once.
 */
#define LDT_ENTRIES    8192u
#define LDT_ENTRY_SIZE 8u
#define LDT_BYTES      (LDT_ENTRIES * LDT_ENTRY_SIZE)

/* What modify_ldt's read of the default LDT gives on Linux's 64-bit
 * kernel: that many zero bytes. */
#define LDT_DEFAULT_BYTES 128u

/* The access rights `lar` reads (Intel SDM Vol. 3A, 3.4.5): present, DPL
 * 3, a code or data segment, and code. */
#define AR_P    0x8000u
#define AR_DPL3 0x6000u
#define AR_S    0x1000u
#define AR_CODE 0x0800u

/* The selector bit that names the LDT. */
#define SEL_TI 4u

void ldt_switch(const struct mm *mm)
{
    struct cpu *c = cpu_self();
    const void *ldt = mm ? mm->ldt : NULL;
    uint64_t base, limit = LDT_BYTES - 1u;

    if (c->ldt == ldt) {
        return;
    }
    c->ldt = ldt;
    if (!ldt) {
        x86_64_lldt(0);
        return;
    }
    /* Present, DPL 0, type 2: an LDT, 16 bytes in long mode. */
    base = (uint64_t)(uintptr_t)ldt;
    c->gdt[GDT_LDT >> 3] = (limit & 0xFFFFu) | ((base & 0xFFFFFFu) << 16) | (0x82UL << 40) |
                           (((limit >> 16) & 0xFu) << 48) | (((base >> 24) & 0xFFu) << 56);
    c->gdt[(GDT_LDT >> 3) + 1] = base >> 32;
    x86_64_lldt(GDT_LDT);
}

/**
 * A #GP from user mode on a selector of an LDT, its error code's TI bit
 * set, while this processor has not loaded the address space's LDT: load
 * it, and return 1 to run the instruction again. Otherwise 0, and the
 * fault is the program's.
 */
int ldt_fault(const isr_params_t *p)
{
    struct mm *mm = current_proc ? current_proc->mm : NULL;

    if (p->vector != 13 || !(p->error_code & SEL_TI) || !mm || !mm->ldt ||
        cpu_self()->ldt == mm->ldt) {
        return 0;
    }
    ldt_switch(mm);
    return 1;
}

int ldt_code_valid(unsigned int sel)
{
    unsigned int ar;

    if (!(sel & SEL_TI)) {
        return 0;
    }
    ldt_switch(current_proc->mm);
    ar = x86_64_lar(sel);
    return (ar & (AR_P | AR_DPL3 | AR_S | AR_CODE)) == (AR_P | AR_DPL3 | AR_S | AR_CODE);
}

void arch_mm_release(struct mm *mm)
{
    if (!mm->ldt) {
        return;
    }
    if (cpu_self()->ldt == mm->ldt) {
        ldt_switch(NULL);
    }
    kfree(mm->ldt);
    mm->ldt = NULL;
    mm->ldt_entries = 0;
}

int arch_mm_copy(struct mm *to, const struct mm *from)
{
    if (!from->ldt) {
        return 0;
    }
    to->ldt = kmalloc(LDT_BYTES);
    if (!to->ldt) {
        return -ENOMEM;
    }
    k_memcpy(to->ldt, from->ldt, LDT_BYTES);
    to->ldt_entries = from->ldt_entries;
    return 0;
}

/** Write `n` zero bytes at the user address `u`: 0, or -EFAULT. */
static int zero_user(uintptr_t u, unsigned long n)
{
    static const uint8_t zeros[256];

    while (n > 0) {
        unsigned long k = n < sizeof(zeros) ? n : sizeof(zeros);

        if (copy_to_user((void __user *)u, zeros, k) != 0) {
            return -EFAULT;
        }
        u += k;
        n -= k;
    }
    return 0;
}

/** modify_ldt's read, Linux's `read_ldt`: the written entries, then zeros,
 * `bytecount` bytes in all, at most the whole table; 0 with no table. */
static long ldt_read(uintptr_t ptr, unsigned long bytecount)
{
    struct mm *mm = current_proc->mm;
    unsigned long have;

    if (!mm->ldt) {
        return 0;
    }
    if (bytecount > LDT_BYTES) {
        bytecount = LDT_BYTES;
    }
    have = (unsigned long)mm->ldt_entries * LDT_ENTRY_SIZE;
    if (have > bytecount) {
        have = bytecount;
    }
    if (copy_to_user((void __user *)ptr, mm->ldt, have) != 0 ||
        zero_user(ptr + have, bytecount - have) != 0) {
        return -EFAULT;
    }
    return (long)bytecount;
}

/** modify_ldt's read of the default LDT, Linux's `read_default_ldt`:
 * zeros, as many as asked, up to LDT_DEFAULT_BYTES. */
static long ldt_read_default(uintptr_t ptr, unsigned long bytecount)
{
    if (bytecount > LDT_DEFAULT_BYTES) {
        bytecount = LDT_DEFAULT_BYTES;
    }
    if (zero_user(ptr, bytecount) != 0) {
        return -EFAULT;
    }
    return (long)bytecount;
}

/**
 * Load each of DS and ES that holds a selector of the LDT again, so that
 * it has the descriptor a write just changed, or the null selector where
 * that no longer loads, as Linux's `refresh_ldt_segments`.
 */
static void ldt_refresh(void)
{
    unsigned int sel = x86_64_ds_read();

    if (sel & SEL_TI) {
        x86_64_ds_load(sel);
    }
    sel = x86_64_es_read();
    if (sel & SEL_TI) {
        x86_64_es_load(sel);
    }
}

/**
 * modify_ldt's write, Linux's `write_ldt`: the `struct user_desc` at `ptr`,
 * `bytecount` its size exactly, sets entry `entry_number`, or clears it for
 * the `LDT_empty` form and, in the old mode, for a base and a limit of 0;
 * the old mode also clears the descriptor's AVL bit. A conforming code
 * segment must be not present, and the old mode takes none, as Linux
 * refuses a present one. 16-bit segments are taken, as a Linux built with
 * CONFIG_X86_16BIT takes them.
 */
static long ldt_write(uintptr_t ptr, unsigned long bytecount, int oldmode)
{
    struct mm *mm = current_proc->mm;
    struct user_desc u;
    uint64_t desc = 0;
    uint32_t contents;

    if (bytecount != sizeof(u)) {
        return -EINVAL;
    }
    if (copy_from_user(&u, (const void __user *)ptr, sizeof(u)) != 0) {
        return -EFAULT;
    }
    if (u.entry_number >= LDT_ENTRIES) {
        return -EINVAL;
    }
    contents = (u.flags & UD_CONTENTS_MASK) >> UD_CONTENTS_SHIFT;
    if (contents == 3u && (oldmode || !(u.flags & UD_SEG_NOT_PRESENT))) {
        return -EINVAL;
    }
    if (!((oldmode && u.base_addr == 0 && u.limit == 0) || user_desc_empty(&u))) {
        desc = user_desc_fill(&u);
        if (oldmode) {
            desc &= ~(1UL << 52);
        }
    }
    if (!mm->ldt) {
        mm->ldt = kzalloc(LDT_BYTES);
        if (!mm->ldt) {
            return -ENOMEM;
        }
    }
    ((volatile uint64_t *)mm->ldt)[u.entry_number] = desc;
    if (u.entry_number + 1u > mm->ldt_entries) {
        mm->ldt_entries = u.entry_number + 1u;
    }
    ldt_switch(mm);
    ldt_refresh();
    return 0;
}

/**
 * Linux returns modify_ldt's result as an int zero-extended into the
 * register, so that a 64-bit caller finds an error in the low 32 bits
 * alone; the compat caller reads EAX either way.
 */
long sys_modify_ldt(isr_params_t *params)
{
    int func = (int)SC_ARG(params, 0);
    uintptr_t ptr = (uintptr_t)SC_ARG(params, 1);
    unsigned long bytecount = SC_ARG(params, 2);
    long ret;

    if (!current_proc || !current_proc->mm) {
        return -ESRCH;
    }
    switch (func) {
    case 0:
        ret = ldt_read(ptr, bytecount);
        break;
    case 1:
        ret = ldt_write(ptr, bytecount, 1);
        break;
    case 2:
        ret = ldt_read_default(ptr, bytecount);
        break;
    case 0x11:
        ret = ldt_write(ptr, bytecount, 0);
        break;
    default:
        ret = -ENOSYS;
        break;
    }
    return (long)(unsigned int)ret;
}
