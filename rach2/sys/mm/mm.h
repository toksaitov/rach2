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
 * page tables, the directory and the regions, and lets go of the program's
 * file. `holds` counts what keeps the record alone: the users, together as
 * one, and each /proc/PID/mem descriptor, whose reads take a user's count only
 * while a user remains, so a descriptor keeps none of the memory of a process
 * that has ended or executed another program. The last hold frees the record.
 *
 * The program's file is kept with it, as Linux keeps mm->exe_file: a
 * description of the file that opens nothing, as an O_PATH open makes one,
 * which /proc/PID/exe reaches wherever a rename or a relocation puts the file
 * and after its name is gone. On the KRFFS root the address space counts
 * among the holders of the file's node, so an unlink or a rename over it
 * leaves the file, without a name, until the last address space that keeps
 * it goes; a relocation moves the description's node with those of the
 * descriptors. The address spaces that keep a file are on one list, which the
 * holder count reads (`mm_exe_holds`), whoever has them: a process, an
 * execve's workspace, or a call that works on another process's pages.
 *
 * What /proc reports of the image is kept with it too: the path the exec found
 * the program at, which /proc/PID/exe reads where the file has no present path
 * to give; where on the stack its arguments and its environment are, which
 * /proc/PID/cmdline and environ read; and the auxiliary vector it was given,
 * which /proc/PID/auxv reads. So is the dumpable flag of prctl(2), as Linux
 * keeps it with the address space: every thread that shares the space has the
 * one flag.
 */
struct page_dir;
struct open_file;
struct vfs_mount;
struct vfs_ref;

/* The words of an auxiliary vector an address space keeps. */
#define MM_AUXV_WORDS 32

struct mm {
    uint32_t users;
    uint32_t holds;
    struct page_dir *dir; /* NULL once the last user has gone */
    uint32_t cr3;         /* the directory's physical address */
    vm_space_t vm;
    struct open_file *exe_file; /* the program's file; NULL for none */
    struct mm *exe_next;        /* the next on the list of those with one */
    /* The path the exec found the program at, from the heap; NULL for none. */
    char *exe;
    /* The dynamic linker the program names, from the heap; NULL for none. */
    char *interp;
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
 * into `to`, a copy of its address space that fork made: the program's file,
 * whose description the two then share, and the path when there is room. */
void mm_copy_info(struct mm *to, const struct mm *from);

/** Keep the heap copy of the path `path` as the path the exec found `mm`'s
 * program at, in place of any it had; none when there is no room. A path
 * without its leading slash, as the debug shell names a program from the
 * root, is given one, since /proc/PID/exe is absolute. */
void mm_set_exe(struct mm *mm, const char *path);

/** Record `path` as the dynamic linker of `mm`'s program, which
 * /proc/PID/maps gives for the linker's segments. */
void mm_set_interp(struct mm *mm, const char *path);

/**
 * Keep the file at `ref` as the program of `mm`, which keeps none yet: a
 * description of it that opens nothing (`fs_exe_open`), and `mm` on the list
 * `mm_exe_holds` reads. Nothing may sleep between the resolution that found
 * the node and this call unless the caller holds the node's namespace lock,
 * since until the list has `mm` the node may go. 0, or -ENOMEM.
 */
int mm_exe_open(struct mm *mm, const struct vfs_ref *ref);

/**
 * Let go of the program's file `mm` keeps, if any. The lock the release needs
 * (`fs_exe_lock`), the KRFFS root's namespace lock for a file of the root, is
 * taken while `mm` is still on the list, and `mm` leaves the list under it,
 * so that no count of the file's holders runs between its leaving and the
 * release, which frees a file that lost its name when no other holder is
 * left. The release, and the wait for the lock, may sleep.
 */
void mm_exe_close(struct mm *mm);

/** Whether an address space keeps the node `node` of `mnt` as its program's
 * file: the node the file is at now, which a relocation moves in the
 * description. The KRFFS root counts it as one more holder of the node. */
int mm_exe_holds(const struct vfs_mount *mnt, const void *node);

/** Take a user's count of `mm`, for a caller that runs in it or that found
 * it as the address space of a process that runs in it, with no sleep
 * since. */
void mm_get(struct mm *mm);

/** Take a user's count of `mm` if a user remains, for a holder that is not
 * one: 1 when taken, 0 once the last user's put has freed the pages. */
int mm_get_live(struct mm *mm);

/** Drop a user's count; the last frees the pages, the page tables, the
 * directory and the regions, lets go of the program's file and releases the
 * files its regions mapped, either of which may sleep, and then drops the
 * users' hold. Its directory must not be the one in use, which the caller
 * switches away from first. */
void mm_put(struct mm *mm);

/** Keep the record of `mm`, and nothing of its memory. */
void mm_hold(struct mm *mm);

/** Let go of a hold `mm_hold` took; the last frees the record. */
void mm_unhold(struct mm *mm);

struct process;

/** Take `p`'s address space from it and drop its user's count, loading the
 * kernel's directory first when this is the last user and its directory is
 * the one in use. `p` is left with none, as a kernel thread has none. The
 * last user records the space's resident size in the thread group's
 * `maxrss_kb` when it is the largest yet, for `ru_maxrss`. */
void mm_drop(struct process *p);

/** The pages of `mm` in memory: the present user pages of its page tables. */
uint32_t mm_resident(const struct mm *mm);

/*
 * The file system's part, in syscall_fs.c: a description of the file at
 * `ref` that opens nothing, with a hold of its own on the node and a
 * reference on the mount, or NULL when the heap has no room or the type
 * cannot pin the node; the lock the release of one needs, which
 * `fs_exe_lock` takes and returns the mount of, the KRFFS root's namespace
 * lock for a file of the root and none, NULL, for another; and the release
 * of one that no list has any more, with that lock held, which frees a file
 * of the KRFFS root that lost its name when nothing else holds it and then
 * gives the lock back.
 */
struct open_file *fs_exe_open(const struct vfs_ref *ref);
struct vfs_mount *fs_exe_lock(const struct open_file *ofd);
void fs_exe_close(struct open_file *ofd, struct vfs_mount *locked);

#endif /* _MM_H_ */
