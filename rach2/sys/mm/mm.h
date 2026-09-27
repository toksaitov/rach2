#ifndef _MM_H_
#define _MM_H_

#include "../lib/kint.h"
#include "vm.h"

/*
 * An address space: a page directory and the regions it holds. A process
 * points at one; fork makes a copy, and clone with CLONE_VM, which vfork and
 * posix_spawn use, shares it, counted in `refs`. execve builds a new one
 * while the old one still runs and puts the old one after, so a failed load
 * returns to its caller, and a child that shares its parent's address space
 * leaves the parent's pages in place. The last reference frees the pages,
 * the directory and the regions.
 *
 * What /proc reports of the image is kept with it: the program's path,
 * which /proc/PID/exe names; where on the stack its arguments and its
 * environment are, which /proc/PID/cmdline and environ read; and the
 * auxiliary vector it was given, which /proc/PID/auxv reads.
 */
struct page_dir;

/* The words of an auxiliary vector an address space keeps. */
#define MM_AUXV_WORDS 32

struct mm {
    uint32_t refs;
    struct page_dir *dir;
    uint32_t cr3; /* the directory's physical address */
    vm_space_t vm;
    char *exe; /* the program's path, from the heap; NULL for none */
    uint32_t arg_start;
    uint32_t arg_end;
    uint32_t env_start;
    uint32_t env_end;
    uint32_t auxv[MM_AUXV_WORDS];
};

/** A new, empty address space with one reference; NULL when no directory or
 * record can be had. */
struct mm *mm_new(void);

/** Copy what `from` keeps for /proc into `to`, a copy of its address space
 * that fork made; the path is copied when there is room. */
void mm_copy_info(struct mm *to, const struct mm *from);

/** Keep the heap copy of the path `path` as `mm`'s program, in place of
 * any it had; none when there is no room. */
void mm_set_exe(struct mm *mm, const char *path);

/** Take a reference to `mm`. */
void mm_get(struct mm *mm);

/** Drop a reference; the last one frees the address space. Its directory
 * must not be the one in use, which the caller switches away from first. */
void mm_put(struct mm *mm);

struct process;

/** Take `p`'s address space from it and drop that reference, loading the
 * kernel's directory first when this is the last reference and its
 * directory is the one in use. `p` is left with none, as a kernel thread
 * has none. */
void mm_drop(struct process *p);

#endif /* _MM_H_ */
