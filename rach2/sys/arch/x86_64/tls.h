#ifndef _X86_64_TLS_H_
#define _X86_64_TLS_H_

#include "../arch.h"

/*
 * Thread-local storage on x86-64: the thread pointer is FS's base, which a
 * program sets with arch_prctl(ARCH_SET_FS) or gets from clone's
 * CLONE_SETTLS, and GS's base is the program's to use as it likes, through
 * ARCH_SET_GS. Both live in the process record (`tls_base` and
 * `regs.gs_base`) with the FS and GS selectors, read back from the
 * registers when the thread switches away and loaded when it switches in
 * (context.c): a selector the program loads into FS or GS changes the base
 * too, to the descriptor's.
 *
 * For 32-bit code there are also Linux's three TLS descriptors, GDT entries
 * 12 to 14 (percpu.h), which set_thread_area fills through `int $0x80` and
 * which the record keeps in `regs.tls`: a program loads a selector of one,
 * `(entry << 3) | 3`, into a segment register, usually GS, and addresses
 * its block through it.
 */

/* arch_prctl's codes, as Linux numbers them. */
#define ARCH_SET_GS 0x1001
#define ARCH_SET_FS 0x1002
#define ARCH_GET_FS 0x1003
#define ARCH_GET_GS 0x1004

/**
 * arch_prctl(2), Linux x86-64's 158: RDI the code, RSI the base to set or the
 * user address to store it at. A base must be a user address, below
 * USER_ADDR_MAX, as Linux requires: -EPERM otherwise. Setting a base loads
 * the null selector into its register first, as Linux does. ARCH_GET_FS and
 * ARCH_GET_GS store the bases the processor has, which a selector the
 * program loaded may have changed since the record was written. -EFAULT for
 * an address to store at that cannot be written, -EINVAL for any other
 * code, -ESRCH with no current process.
 */
long sys_arch_prctl(isr_params_t *params);

/* Linux's `struct user_desc`, set_thread_area's argument, 16 bytes in both
 * ABIs, and the bits of its `flags`, in their order in Linux's bit-fields. */
struct user_desc {
    uint32_t entry_number;
    uint32_t base_addr;
    uint32_t limit;
    uint32_t flags;
};
#define UD_SEG_32BIT       0x01u
#define UD_CONTENTS_MASK   0x06u
#define UD_CONTENTS_SHIFT  1
#define UD_READ_EXEC_ONLY  0x08u
#define UD_LIMIT_IN_PAGES  0x10u
#define UD_SEG_NOT_PRESENT 0x20u
#define UD_USEABLE         0x40u
#define UD_LM              0x80u

struct process;

/** Put the three TLS entries of the thread `t` in `u`, each as
 * get_thread_area describes it, an empty one in Linux's `LDT_empty` form.
 * Returns how many are set, Linux's `regset_tls_active`. */
uint32_t ia32_tls_get(const struct process *t, struct user_desc *u);

#endif /* _X86_64_TLS_H_ */
