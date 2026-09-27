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
 */
struct page_dir;

struct mm {
    uint32_t refs;
    struct page_dir *dir;
    uint32_t cr3; /* the directory's physical address */
    vm_space_t vm;
};

/** A new, empty address space with one reference; NULL when no directory or
 * record can be had. */
struct mm *mm_new(void);

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
