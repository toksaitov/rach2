#ifndef _MM_H_
#define _MM_H_

#include "../lib/kint.h"
#include "vm.h"

/*
 * An address space: a page directory and the regions it holds. A process
 * points at one; fork makes a copy, and clone with CLONE_VM, which vfork and
 * posix_spawn use, shares it. execve builds a new one while the old one still
 * runs and puts the old one after, so a failed load returns to its caller,
 * and a child that shares its parent's address space leaves the parent's
 * pages in place.
 *
 * It has two counts, as Linux's mm_struct has mm_users and mm_count. `users`
 * counts what runs in it or works on its pages: each thread that has it, an
 * execve that builds it, and a call that reads or writes the pages of another
 * process for the length of the call. The last user's put frees the pages, the
 * page tables, the directory and the regions. `holds` counts what keeps the
 * record alone: the users, together as one, and each /proc/PID/mem descriptor,
 * whose reads take a user's count only while a user remains, so a descriptor
 * keeps none of the memory of a process that has ended or executed another
 * program. The last hold frees the record.
 *
 * What /proc reports of the image is kept with it: the program's path,
 * which /proc/PID/exe names; where on the stack its arguments and its
 * environment are, which /proc/PID/cmdline and environ read; and the
 * auxiliary vector it was given, which /proc/PID/auxv reads. So is the
 * dumpable flag of prctl(2), as Linux keeps it with the address space: every
 * thread that shares the space has the one flag.
 */
struct page_dir;

/* The words of an auxiliary vector an address space keeps. */
#define MM_AUXV_WORDS 32

struct mm {
    uint32_t users;
    uint32_t holds;
    struct page_dir *dir; /* NULL once the last user has gone */
    uint32_t cr3;         /* the directory's physical address */
    vm_space_t vm;
    char *exe; /* the program's path, from the heap; NULL for none */
    uint32_t arg_start;
    uint32_t arg_end;
    uint32_t env_start;
    uint32_t env_end;
    uint32_t auxv[MM_AUXV_WORDS];
    /* Whether the process running this image may leave a core file, and be
     * traced or have its memory opened through /proc by a caller without
     * effective uid 0: 1 for a new address space and for an image an exec
     * builds, 0 for one that runs with ids its caller lacks or once any
     * sharer's effective ids move; PR_SET_DUMPABLE sets it, and fork's copy
     * takes it. */
    uint32_t dumpable;
};

/** A new, empty, dumpable address space with one user and one hold; NULL
 * when no directory or record can be had. */
struct mm *mm_new(void);

/** Copy what `from` keeps beside its pages, for /proc and the dumpable flag,
 * into `to`, a copy of its address space that fork made; the path is copied
 * when there is room. */
void mm_copy_info(struct mm *to, const struct mm *from);

/** Keep the heap copy of the path `path` as `mm`'s program, in place of
 * any it had; none when there is no room. A path without its leading slash,
 * as the debug shell names a program from the root, is given one, since
 * /proc/PID/exe is absolute. */
void mm_set_exe(struct mm *mm, const char *path);

/** Take a user's count of `mm`, for a caller that runs in it or that found
 * it as the address space of a process that runs in it, with no sleep
 * since. */
void mm_get(struct mm *mm);

/** Take a user's count of `mm` if a user remains, for a holder that is not
 * one: 1 when taken, 0 once the last user's put has freed the pages. */
int mm_get_live(struct mm *mm);

/** Drop a user's count; the last frees the pages, the page tables, the
 * directory and the regions, releases the files its regions mapped, which
 * may sleep, and then drops the users' hold. Its directory must not be the
 * one in use, which the caller switches away from first. */
void mm_put(struct mm *mm);

/** Keep the record of `mm`, and nothing of its memory. */
void mm_hold(struct mm *mm);

/** Let go of a hold `mm_hold` took; the last frees the record. */
void mm_unhold(struct mm *mm);

struct process;

/** Take `p`'s address space from it and drop its user's count, loading the
 * kernel's directory first when this is the last user and its directory is
 * the one in use. `p` is left with none, as a kernel thread has none. */
void mm_drop(struct process *p);

#endif /* _MM_H_ */
