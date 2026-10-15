/*
 * auxv.h: the auxiliary vector, the pairs of words the kernel puts on a new
 * image's stack just past envp's NULL terminator (uimage.c).
 *
 * The C runtime (musl's __libc_start_main and __init_tls) walks it to learn
 * the page size, the program header table, the entropy seed, and more. The
 * AT_ codes are Linux's, the same on i386 and x86-64, so a stock musl reads
 * the vector unchanged.
 *
 * Reference: the System V ABI's i386 and x86-64 supplements; Linux's <elf.h>
 * AT_ constants.
 */

#ifndef _AUXV_H_
#define _AUXV_H_

#include "../lib/kint.h"

/*
 * Auxiliary vector entry types (a_type). The kernel writes those marked
 * "populated" (`uimage_write`); the rest are listed for completeness.
 */
#define AT_NULL        0 /* End of vector                          (populated) */
#define AT_IGNORE      1
#define AT_EXECFD      2
#define AT_PHDR        3 /* Address of the program header table    (populated) */
#define AT_PHENT       4 /* Size in bytes of one program header    (populated) */
#define AT_PHNUM       5 /* Number of program headers              (populated) */
#define AT_PAGESZ      6 /* System page size                       (populated) */
#define AT_BASE        7 /* Where the interpreter was loaded, or 0 (populated) */
#define AT_FLAGS       8
#define AT_ENTRY       9 /* Program entry point                    (populated) */
#define AT_NOTELF      10
#define AT_UID         11 /* Real user id                           (populated) */
#define AT_EUID        12 /* Effective user id                      (populated) */
#define AT_GID         13 /* Real group id                          (populated) */
#define AT_EGID        14 /* Effective group id                     (populated) */
#define AT_CLKTCK      17 /* times(2) ticks per second, 100         (populated) */
#define AT_SECURE      23 /* 1 when euid != ruid or egid != rgid    (populated) */
#define AT_RANDOM      25 /* Address of 16 random bytes             (populated) */
#define AT_EXECFN      31 /* Address of the program's path          (populated) */
#define AT_MINSIGSTKSZ 51 /* The least alternate stack a frame fits (populated) */

/*
 * One auxv entry, as a program reads it: a {type, value} pair of machine
 * words, 32 bits on i686 and 64 on x86-64. The vector is consecutive pairs
 * ending in an AT_NULL entry, which uimage.c writes as unsigned longs.
 */
typedef struct {
    unsigned long a_type;
    unsigned long a_val;
} Elf_auxv_t;

#endif /* _AUXV_H_ */
