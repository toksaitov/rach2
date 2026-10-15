#ifndef _TLS_H_
#define _TLS_H_

#include "../../lib/kint.h"

/*
 * i686 thread-local storage: GDT entry 6, whose base is the thread pointer,
 * reached through %gs and set by set_thread_area(2) with a struct user_desc,
 * as on Linux i386, which musl's __set_thread_area calls. See
 * ../tls.Readme.md.
 */

/*
 * Linux i386's struct user_desc, the argument of set_thread_area and of
 * clone's CLONE_SETTLS. Linux declares the flags as bit-fields; here they are
 * one word read with the masks below, so that the layout does not depend on
 * the compiler's order of bit-fields.
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
#define UD_SEG_32BIT       0x01u /* bit 0    : 32-bit segment            */
#define UD_CONTENTS_MASK   0x06u /* bits 1-2 : 0 = data, !=0 = code/stack */
#define UD_READ_EXEC_ONLY  0x08u /* bit 3    : not writable              */
#define UD_LIMIT_IN_PAGES  0x10u /* bit 4    : limit is in 4 KB pages     */
#define UD_SEG_NOT_PRESENT 0x20u /* bit 5    : descriptor not present     */
#define UD_USEABLE         0x40u /* bit 6    : AVL bit                    */

/**
 * Check the descriptor `u` and store the base it asks for in `*base_out`.
 *
 * The descriptor must ask for the one TLS entry the kernel has: entry -1,
 * any free one, or GDT_TLS_ENTRY, as a present, 32-bit, writable data
 * segment. The limit, its granularity and the usable bit are ignored, since
 * the entry always has a 4 GiB limit.
 *
 * @return 0, or -1 for a null argument or a descriptor refused.
 */
int tls_resolve_base(const user_desc_t *u, uint32_t *base_out);

#endif /* _TLS_H_ */
