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
 * one flag. So is the image's ABI, an i386 program's on the x86-64 kernel,
 * which Linux keeps as the thread flag TIF_ADDR32 that every thread of the
 * image has.
 */
#ifndef _MM_H_
#define _MM_H_

#include "../lib/kint.h"
#include "vm.h"

struct page_dir;
struct open_file;
struct vfs_mount;
struct vfs_ref;

/* The words of an auxiliary vector an address space keeps: two for each of
 * the UIMAGE_AUXV entries an image is given (proc/uimage.h), which
 * syscall_proc.c checks. */
#define MM_AUXV_WORDS 44

struct mm {
    uint32_t users;
    uint32_t holds;
    struct page_dir *dir; /* NULL once the last user has gone */
    uintptr_t space_root; /* the directory's physical address; 0 with `dir` */
    vm_space_t vm;
    struct open_file *exe_file;    /* the program's file; NULL for none */
    struct open_file *interp_file; /* the ELF interpreter; NULL for none */
    struct mm *exe_next;           /* the next on the list of those with one */
    /* The path the exec found the program at, from the heap; NULL for none. */
    char *exe;
    /* The dynamic linker the program names, from the heap; NULL for none. */
    char *interp;
    uintptr_t arg_start;
    uintptr_t arg_end;
    uintptr_t env_start;
    uintptr_t env_end;
    /* The auxiliary vector as the image's stack has it: in words of a
     * pointer's width, or for an i386 image (`compat`) in 4-byte words
     * packed from the start of the array, as Linux's saved_auxv keeps a
     * compat task's. */
    unsigned long auxv[MM_AUXV_WORDS];
    /* Whether the image is an i386 program the x86-64 kernel runs: its
     * address space ends at IA32_TASK_SIZE, its threads run 32-bit code,
     * and its core file is ELFCLASS32; 0 for a native image and on i686,
     * whose programs are native. A fork's copy takes it. */
    uint32_t compat;
    /* The address of the vDSO's image in the address space, AT_SYSINFO_EHDR,
     * or 0 for none, from which an i386 signal frame finds the vDSO's
     * sigreturn code (ia32.h). A fork's copy takes it. */
    uintptr_t vdso;
    /* Whether the process running this image may leave a core file, and be
     * traced or have its memory opened through /proc by a caller without
     * privilege: 1 for a new address space and for an image an exec builds,
     * 0 for an image that runs with ids or privilege its caller lacks or
     * that its caller may not read, and once any sharer's effective ids
     * move; PR_SET_DUMPABLE sets it, and fork's copy takes it. */
    uint32_t dumpable;
    /* The ranges drivers registered to hear of changes to the pages
     * (notify.h), among them those of the pins charged to the address
     * space (pin.h); a fork's copy has none. */
    struct mmn_range *notify;
    /* The pages pinned for devices and charged here, VmPin of
     * /proc/PID/status, which RLIMIT_MEMLOCK bounds apart from the locked
     * regions' VmLck, as Linux's pinned_vm. */
    unsigned long pinned;
    /* The x86-64's local descriptor table, which modify_ldt makes and every
     * thread of the space loads (arch/x86_64/ldt.c), from the heap; NULL
     * for none, and always on i686. A fork's copy has a copy of it
     * (`arch_mm_copy`), and the last user's put frees it
     * (`arch_mm_release`). */
    void *ldt;
    uint32_t ldt_entries; /* the entries up to the highest written */
    /* The present user pages of the page tables, VmRSS, which every change
     * of an entry's present bit keeps (`mm_resident_add`), and the most
     * there were before any went, as Linux's hiwater_rss; a fork's copy
     * counts its own from none. */
    unsigned long resident;
    unsigned long resident_peak;
    struct mm *dir_next; /* the next in its chain of the table by directory */
};

/** A new, empty, dumpable address space with one user and one hold; NULL
 * when no directory or record can be had. */
struct mm *mm_new(void);

/** Copy what `from` keeps beside its pages, for /proc and the dumpable flag,
 * into `to`, a copy of its address space that fork made: the program's file,
 * and its ELF interpreter, whose descriptions the two then share, and the
 * path when there is room. */
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
 * Keep the file at `ref` as the program of `mm`, or its ELF interpreter
 * when it already has a program: a
 * description of it that opens nothing (`fs_exe_open`), and `mm` on the list
 * `mm_exe_holds` reads. Nothing may sleep between the resolution that found
 * the node and this call unless the caller holds the node's namespace lock,
 * since until the list has `mm` the node may go. 0, -ETXTBSY for a writer,
 * or -ENOMEM.
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
 * file or ELF interpreter: the node the file is at now, which a relocation
 * moves in the description. The KRFFS root counts it as one more holder
 * of the node. */
int mm_exe_holds(const struct vfs_mount *mnt, const void *node);

/** Take a user's count of `mm`, for a caller that runs in it or that found
 * it as the address space of a process that runs in it, with no sleep
 * since. */
void mm_get(struct mm *mm);

/** Take a user's count of `mm` if a user remains, for a holder that is not
 * one: 1 when taken, 0 once the last user's put has freed the pages. */
int mm_get_live(struct mm *mm);

/** Drop a user's count; the last tells the ranges drivers registered and
 * releases the pins charged to the space (`mmn_release`), frees the pages,
 * the page tables, the directory and the regions, lets go of the program's
 * file and releases the files its regions mapped, either of which may
 * sleep, and then drops the users' hold. Its directory must not be the one
 * in use, which the caller switches away from first. */
void mm_put(struct mm *mm);

/** Keep the record of `mm`, and nothing of its memory. */
void mm_hold(struct mm *mm);

/** Let go of a hold `mm_hold` took; the last frees the record. */
void mm_unhold(struct mm *mm);

struct process;

/** Take `p`'s address space from it and drop its user's count, loading the
 * kernel's directory first when this is the last user and its directory is
 * the one in use. `p` is left with none, as a kernel thread has none. The
 * last user records the space's peak resident size in the thread group's
 * `maxrss_kb` when it is the largest yet, for `ru_maxrss`. */
void mm_drop(struct process *p);

/** The pages of `mm` in memory: the present user pages of its page tables,
 * as `mm_resident_add` has counted them, at no cost of a walk. */
unsigned long mm_resident(const struct mm *mm);

/** The most pages `mm` has had in memory at once, VmHWM and what
 * `ru_maxrss` reports of it. */
unsigned long mm_resident_peak(const struct mm *mm);

/**
 * Count `pages` more present user pages, or fewer for a negative count, in
 * the address space whose directory is `dir`. Each writer of a user entry
 * calls it for the present bits it set or cleared, under the big lock, so
 * that `getrusage` and /proc read the size without walking the tables.
 * Nothing for a directory no address space has.
 */
void mm_resident_add(const struct page_dir *dir, long pages);

/**
 * A description of the file at `ref` for an address space to keep its
 * program by, one that opens nothing, as an O_PATH open makes: it has a hold
 * of its own on the node and a reference on the mount, so that a volume is
 * not unmounted while a program on it runs, and on a type that keeps a node
 * only while it is open, mounted KRFFS, a pin, so that a rename or an unlink
 * leaves the file to it. NULL when the heap has no room or the pin fails.
 * The file system's part, in syscall_fs.c.
 */
struct open_file *fs_exe_open(const struct vfs_ref *ref);

/**
 * Take the lock the release of `ofd`, an address space's description of its
 * program's file, needs, before the address space leaves the list
 * `mm_exe_holds` reads, and return the mount locked: for a file of the KRFFS
 * root, the root's namespace lock; for a file of another type, no lock, and
 * NULL. May sleep, so the caller holds no namespace lock. In syscall_fs.c.
 */
struct vfs_mount *fs_exe_lock(const struct open_file *ofd);

/**
 * Let go of `ofd` once its address space is off the list `mm_exe_holds`
 * reads, with `locked` held as `fs_exe_lock` returned it: a file of the
 * KRFFS root that lost its name is freed when nothing else holds it, the
 * lock is given back, and the description's last reference closes it. May
 * sleep. In syscall_fs.c.
 */
void fs_exe_close(struct open_file *ofd, struct vfs_mount *locked);

#endif /* _MM_H_ */
