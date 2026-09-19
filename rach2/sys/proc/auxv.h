/*
 * auxv.h - SysV/i386 auxiliary vector (auxv) definitions
 *
 * The kernel hands each new process an auxiliary vector on its initial stack,
 * just past envp's NULL terminator. The C runtime (musl's __libc_start_main and
 * __init_tls) walks it to learn the page size, the program-header table, the
 * entropy seed, and more. The values and AT_* codes match the Linux i386 ABI so
 * a stock musl consumes them unchanged.
 *
 * Reference: System V ABI i386 supplement; Linux <elf.h> AT_* constants.
 */

#ifndef _AUXV_H_
#define _AUXV_H_

#include "../lib/kint.h"

/*
 * Auxiliary vector entry types (a_type). Only the subset marked "populated" is
 * emitted by the kernel today; the rest are listed for ABI completeness.
 */
#define AT_NULL   0 /* End of vector                          (populated) */
#define AT_IGNORE 1
#define AT_EXECFD 2
#define AT_PHDR   3 /* Address of the program header table    (populated) */
#define AT_PHENT  4 /* Size in bytes of one program header    (populated) */
#define AT_PHNUM  5 /* Number of program headers              (populated) */
#define AT_PAGESZ 6 /* System page size                       (populated) */
#define AT_BASE   7
#define AT_FLAGS  8
#define AT_ENTRY  9 /* Program entry point                    (populated) */
#define AT_NOTELF 10
#define AT_UID    11
#define AT_EUID   12
#define AT_GID    13
#define AT_EGID   14
#define AT_CLKTCK 17 /* Clock ticks per second (our PIT: 100)  (populated) */
#define AT_SECURE 23 /* 1 when euid != ruid or egid != rgid    (populated) */
#define AT_RANDOM 25 /* Address of 16 random bytes             (populated) */
#define AT_EXECFN 31

/*
 * One auxv entry: a {type, value} pair of machine words (32-bit on i386). The
 * kernel writes consecutive pairs onto the initial stack, terminated by an
 * AT_NULL entry.
 */
typedef struct {
    uint32_t a_type;
    uint32_t a_val;
} Elf32_auxv_t;

#endif /* _AUXV_H_ */
