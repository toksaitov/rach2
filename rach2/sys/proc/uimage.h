#ifndef _UIMAGE_H_
#define _UIMAGE_H_

/* The initial stack of a new image, its strings, pointers and auxiliary
 * vector, built in frames of its own before it is mapped: by execve, while
 * the image it replaces still runs, and by the boot for its programs. */

#include "../lib/kint.h"
#include "../lib/kuser.h"
#include "elf.h"

struct page_dir;

/* The auxiliary vector's entries, AT_NULL included: an i386 image on the
 * x86-64 kernel has all of them, and another, which lacks AT_HWCAP,
 * AT_PLATFORM and AT_SYSINFO, and AT_SYSINFO_EHDR without a vDSO, has
 * AT_NULL in their places. */
#define UIMAGE_AUXV 22u

/* Linux's limits on the strings of an execve (fs/exec.c): one string, its
 * NUL included, is at most 32 pages; the strings with their pointers take a
 * quarter of RLIMIT_STACK, at most three quarters of 8 MiB and at least 32
 * pages. */
#define UIMAGE_STRING_MAX 131072u
#define UIMAGE_ARGS_MIN   131072u
#define UIMAGE_ARGS_MAX   6291456u

/*
 * The initial stack of a new image while execve builds it, which Linux calls
 * the argument pages of its `linux_binprm`.
 *
 * The pages are user frames taken for the new image while the image it
 * replaces still runs. The strings are copied into them from the top of the
 * stack down, as Linux copies them: the program's path, the environment
 * from its last string, and argv from its last string, so argv[0] is the
 * lowest. Once the image is loaded, the pointer arrays and the auxiliary
 * vector are written below the strings and the frames are mapped where the
 * stack is. `frames[i]` backs the page `i` pages below `top`.
 */
struct uimage {
    uintptr_t *frames; /* `cap` entries, in the heap */
    /* The bytes of a word of the information block, a pointer's of the
     * image: sizeof(unsigned long), or 4 for an i386 image on the x86-64
     * kernel; and AT_PLATFORM's string, NULL for none. `uimage_start`
     * sets the native word and no string. */
    uint32_t word;
    const char *platform;
    uint32_t cap;
    uint32_t nframes; /* frames taken, from the top page down */
    uint32_t given;   /* frames a page directory has taken over */
    uintptr_t top;    /* the address just past the stack */
    uint32_t used;    /* bytes of strings below `top` */
    uintptr_t execfn; /* the first string, the program's path: AT_EXECFN */
    /* Where `uimage_write` found argv's strings to begin and the
     * environment's to begin and end, and the auxiliary vector it wrote,
     * in words of `word` bytes packed from the array's start. */
    uintptr_t arg_start;
    uintptr_t env_start;
    uintptr_t env_end;
    unsigned long auxv[2 * UIMAGE_AUXV];
    uint32_t room; /* bytes the strings may take */
    uint32_t pad;  /* bytes left between the strings and AT_RANDOM's */
    uintptr_t sp;  /* the entry stack pointer, once placed */
};

/**
 * The bytes the strings of an exec and their pointers may take under a soft
 * RLIMIT_STACK of `stack_limit`, as Linux's `bprm_stack_limits` sizes them:
 * a quarter of the limit, at most UIMAGE_ARGS_MAX and at least
 * UIMAGE_ARGS_MIN.
 */
uint32_t uimage_budget(unsigned long stack_limit);

/**
 * Start in `u` an initial stack whose top is `top`, to have `argc` and
 * `envc` strings under a soft RLIMIT_STACK of `stack_limit`. The strings
 * and their pointers share the budget `uimage_budget` gives: the pointers
 * are set aside first, one for argv[0] even in an empty argv, and the
 * strings get what is left. Only the array of frames is taken here; the
 * frames are taken as the strings reach them. 0; -E2BIG when the pointers
 * alone take the budget; -ENOMEM when the array could not be had.
 */
int uimage_start(struct uimage *u, uintptr_t top, unsigned long stack_limit, uint32_t argc,
                 uint32_t envc);

/**
 * Copy the user string at `s` below the strings already there, as Linux's
 * `copy_strings` copies one: its length is found first, and a string longer
 * than UIMAGE_STRING_MAX bytes with its NUL, or one that does not fit the
 * room left, is -E2BIG. 0, -E2BIG, -EFAULT for an address that cannot be
 * read, or -ENOMEM.
 */
int uimage_push_user(struct uimage *u, const char __user *s);

/** Copy the kernel string `s` below the strings already there, under the
 * rules of `uimage_push_user`; 0, -E2BIG or -ENOMEM. */
int uimage_push(struct uimage *u, const char *s);

/** Take away the lowest string, argv[0], as Linux's `remove_arg_zero`
 * does before an interpreter's arguments are put in its place. Its bytes
 * are cleared, so none is left for the new image to find. 0, or -ENOMEM
 * when no temporary mapping was free. */
int uimage_drop_first(struct uimage *u);

/**
 * Move the stack `u`, whose strings are all copied and whose information
 * block is not yet placed, so that its top is `top`, a page boundary, and
 * leave `pad` bytes, a multiple of 16 below 8 KiB, between the strings and
 * the information block, as Linux's `arch_align_stack` leaves them when it
 * randomizes the stack. The strings keep their offsets within their pages,
 * so the frames that hold them hold them still.
 */
void uimage_move(struct uimage *u, uintptr_t top, uint32_t pad);

/**
 * Place the information block of `argc` and `envc` strings below the
 * strings and the pad, and take the frames it needs. Below them goes
 * AT_PLATFORM's string, when there is one, then the 16 bytes AT_RANDOM
 * points at, and below them argc, the argv pointers and their NULL, the
 * envp pointers and theirs, and the auxiliary vector, each a word of
 * `word` bytes, as Linux's `create_elf_tables` lays them. Their start, the
 * entry stack pointer, is aligned down to 16 bytes, as the call ABIs of
 * i386 and x86-64 have it. 0, -E2BIG or -ENOMEM.
 */
int uimage_place(struct uimage *u, uint32_t argc, uint32_t envc);

/* What the auxiliary vector says beside the program's image: where the
 * interpreter was loaded (AT_BASE, 0 without one), where the vDSO's image
 * is (AT_SYSINFO_EHDR, 0 without one, and then no entry), the ids the image
 * runs with, AT_SECURE, and for an i386 image AT_HWCAP and the vDSO's
 * __kernel_vsyscall (AT_SYSINFO), each 0 for no entry. */
struct uimage_aux {
    uintptr_t base;
    uintptr_t sysinfo_ehdr;
    uint32_t uid;
    uint32_t euid;
    uint32_t gid;
    uint32_t egid;
    int secure;
    uint32_t hwcap;
    uintptr_t sysinfo;
};

/**
 * Write the information block `uimage_place` placed for the image `img`:
 * AT_RANDOM's bytes from `random16`, argc, the pointers to the `argc`
 * strings of argv and then the `envc` of the environment, each array ending
 * in NULL, and the auxiliary vector (AT_PHDR, AT_PHENT, AT_PHNUM, AT_ENTRY,
 * AT_PAGESZ, AT_CLKTCK 100, AT_SECURE, AT_RANDOM, AT_BASE, AT_UID, AT_EUID,
 * AT_GID, AT_EGID, AT_EXECFN, AT_MINSIGSTKSZ, AT_RSEQ_FEATURE_SIZE,
 * AT_RSEQ_ALIGN, AT_HWCAP, AT_PLATFORM and AT_SYSINFO where `aux` and `u`
 * give them, AT_SYSINFO_EHDR where there is a vDSO, and AT_NULL, which
 * fills the vector's rest), which a dynamic linker reads to find itself and
 * the program it loads. The vector and the bounds of the strings are kept
 * in `u` too. 0, or -1 when no temporary mapping was free.
 */
int uimage_write(struct uimage *u, uint32_t argc, uint32_t envc, const elf_image_t *img,
                 const uint8_t random16[16], const struct uimage_aux *aux);

/**
 * Map each frame into `dir` at its place in the stack, read and write for
 * the user and not executable, as the stack's region is. The frames mapped
 * belong to `dir` from then on; 0, or -1 when a page table could not be
 * had, with the frames not yet mapped still `u`'s.
 */
int uimage_map(struct uimage *u, struct page_dir *dir);

/** The lowest address of the stack's pages. */
uintptr_t uimage_bottom(const struct uimage *u);

/** Give back the frames no page directory took, and the array; `u` has
 * nothing afterwards and may be released again. */
void uimage_release(struct uimage *u);

#endif /* _UIMAGE_H_ */
