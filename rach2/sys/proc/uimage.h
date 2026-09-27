#ifndef _UIMAGE_H_
#define _UIMAGE_H_

#include "../lib/kint.h"
#include "../lib/kuser.h"
#include "elf.h"

struct page_dir;

/* Linux's limits on what execve carries (fs/exec.c): one string, its NUL
 * included, is at most 32 pages; the strings with their pointers take a
 * quarter of RLIMIT_STACK, at most three quarters of 8 MiB and at least 32
 * pages. */
#define UIMAGE_STRING_MAX 131072u
#define UIMAGE_ARGS_MIN   131072u
#define UIMAGE_ARGS_MAX   6291456u

/*
 * The initial stack of a new image while execve builds it, which Linux calls
 * the argument pages of its `linux_binprm`.
 *
 * The pages are user frames taken for the new image while the old one
 * still runs. The strings are copied into them from the top of the stack
 * down, as Linux copies them: the program's path, the environment from its
 * last string, and argv from its last string, so argv[0] is the lowest. Once
 * the image is loaded, the pointer arrays and the auxiliary vector are
 * written below the strings and the frames are mapped where the stack is.
 * `frames[i]` backs the page `i` pages below `top`.
 */
struct uimage {
    uintptr_t *frames; /* `cap` entries, in the heap */
    uint32_t cap;
    uint32_t nframes; /* frames taken, from the top page down */
    uint32_t given;   /* frames a page directory has taken over */
    uint32_t top;     /* the address just past the stack */
    uint32_t used;    /* bytes of strings below `top` */
    uint32_t execfn;  /* the first string, the program's path: AT_EXECFN */
    uint32_t room;    /* bytes the strings may take */
    uint32_t esp;     /* the entry stack pointer, once placed */
};

/** The bytes the strings and their pointers may take under a soft
 * RLIMIT_STACK of `stack_limit`. */
uint32_t uimage_budget(uint32_t stack_limit);

/** Start an initial stack whose top is `top` for `argc` and `envc` strings
 * under a soft RLIMIT_STACK of `stack_limit`; 0, -E2BIG, -ENOMEM. */
int uimage_start(struct uimage *u, uint32_t top, uint32_t stack_limit, uint32_t argc,
                 uint32_t envc);

/** Copy the user string at `s` below the strings already there; 0, -E2BIG,
 * -EFAULT, -ENOMEM. */
int uimage_push_user(struct uimage *u, const char __user *s);

/** Copy the kernel string `s` below the strings already there; 0, -E2BIG,
 * -ENOMEM. */
int uimage_push(struct uimage *u, const char *s);

/** Take away the lowest string, argv[0]; 0, or -ENOMEM when no temporary
 * mapping was free. */
int uimage_drop_first(struct uimage *u);

/** Place the information block of `argc` and `envc` strings below them and
 * take the frames it needs; 0, -E2BIG, -ENOMEM. */
int uimage_place(struct uimage *u, uint32_t argc, uint32_t envc);

/* What the auxiliary vector says beside the program's image: where the
 * interpreter was loaded (AT_BASE, 0 without one), the ids the image runs
 * with, and AT_SECURE. */
struct uimage_aux {
    uint32_t base;
    uint32_t uid;
    uint32_t euid;
    uint32_t gid;
    uint32_t egid;
    int secure;
};

/** Write the pointers, AT_RANDOM's bytes and the auxiliary vector; 0, or -1
 * when no temporary mapping was free. */
int uimage_write(struct uimage *u, uint32_t argc, uint32_t envc, const elf_image_t *img,
                 const uint8_t random16[16], const struct uimage_aux *aux);

/** Map every frame into `dir` where the stack is; 0, or -1 for a page table
 * that could not be had. The mapped frames belong to `dir` from then on. */
int uimage_map(struct uimage *u, struct page_dir *dir);

/** The lowest address of the stack's pages. */
uint32_t uimage_bottom(const struct uimage *u);

/** Give back the frames no page directory took and the array. */
void uimage_release(struct uimage *u);

#endif /* _UIMAGE_H_ */
