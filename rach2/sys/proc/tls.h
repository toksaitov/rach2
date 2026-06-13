#ifndef _TLS_H_
#define _TLS_H_

#include "../lib/kint.h"

/*
 * Thread-local storage, the i386 way: a GDT descriptor whose base is the
 * thread control block, reached through %gs. This mirrors Linux's i386
 * set_thread_area(2) / struct user_desc mechanism -- the same syscall and ABI
 * musl's __set_thread_area calls from __init_tls. See Readme in this directory.
 */

/*
 * The descriptor a set_thread_area caller passes (Linux i386 struct user_desc).
 * The bitfield word is kept as a plain uint32_t with the bit masks below so the
 * layout matches exactly what the C library pushes on the stack.
 */
struct user_desc {
    uint32_t entry_number;
    uint32_t base_addr;
    uint32_t limit;
    uint32_t flags;
};

typedef struct user_desc user_desc_t;

/*
 * user_desc.flags bit layout (LSB first), as Linux defines it.
 */
#define UD_SEG_32BIT       0x01u  /* bit 0    : 32-bit segment            */
#define UD_CONTENTS_MASK   0x06u  /* bits 1-2 : 0 = data, !=0 = code/stack */
#define UD_READ_EXEC_ONLY  0x08u  /* bit 3    : not writable              */
#define UD_LIMIT_IN_PAGES  0x10u  /* bit 4    : limit is in 4 KB pages     */
#define UD_SEG_NOT_PRESENT 0x20u  /* bit 5    : descriptor not present     */
#define UD_USEABLE         0x40u  /* bit 6    : AVL bit                    */

/*
 * Validate a user_desc against the single 4 GB DPL-3 data slot we model for
 * TLS and output the base to program into the GDT entry. Returns 0 on success
 * or -1 if the request cannot be represented (wrong entry, not 32-bit, not a
 * writable data segment, or marked not-present). Pure logic with no kernel
 * state, so the dual-target host seam test exercises exactly this code. The
 * syscall glue (user copy, gdt_set_tls, PCB update) lives in sys/syscall.c.
 */
int tls_resolve_base(const user_desc_t *u, uint32_t *base_out);

#endif /* _TLS_H_ */
