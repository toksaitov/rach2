/* The regions of an address space, in an AVL tree ordered by address. */
#ifndef _VM_H_
#define _VM_H_

#include "../lib/kint.h"
#include "../lib/kdef.h"

/* The default region limit and the number of stable nodes per allocation.
 * vm_max_map_count bounds live regions, not spare nodes. */
#define VMA_LIMIT 65536
#define VMA_FIRST 16

/* How far below its base the main stack VMA may grow on demand (8 MB). */
#define VM_STACK_MAX_GROWTH 0x800000u

/* Linux's default gap below a growing stack: 256 pages of 4096 bytes. */
#define VM_STACK_GUARD_GAP 0x100000u

/* Region protection bits; the first three are the values of PROT_READ,
 * PROT_WRITE and PROT_EXEC. */
#define VM_PROT_READ        0x1u
#define VM_PROT_WRITE       0x2u
#define VM_PROT_EXEC        0x4u
/* Given to paging_protect_range, and not kept in a region, for a shared
 * region, whose pages are shared on purpose: a write goes to the frame they
 * share, never to a copy. */
#define VM_PROT_SHARED      0x8u
/* Given to paging_protect_range beside VM_PROT_SHARED, and not kept in a
 * region, for a shared region whose file counts its stores (`mkwrite`): a
 * page gains write access only at its write fault. */
#define VM_PROT_MKWRITE     0x800000u
/* Kept in a region's protection beside the three access bits: the region
 * may never be made writable. A shared mapping of a file opened without
 * write access has it, and so has a shared mapping of a file on a disk,
 * which is served as a private one and whose stores would not reach the
 * file. `vm_protect` keeps the bit and answers a request for write with -3
 * while it is set, as Linux's mprotect answers EACCES for a mapping without
 * VM_MAYWRITE; splits, merges, copies and moves keep it with the rest of
 * the protection. */
#define VM_PROT_NOWRITE     0x10u
/* Kept the same way: the region maps device memory, a device object's
 * pages (`vm_device_map`), whose fault asks the object for the page and
 * whose frames need not be the allocator's. Nothing reads it on a process's
 * behalf, a core file or /proc/PID/mem, since a read of a device register
 * can change the device. */
#define VM_PROT_IO          0x20u
/* Kept the same way, beside VM_PROT_IO: the pages are mapped
 * write-combining (PTE_WC), or uncached where the processor has no
 * write-combining; with VM_PROT_UC, uncached (PTE_NOCACHE); with neither,
 * cached, as for a page of RAM. */
#define VM_PROT_WC          0x40u
#define VM_PROT_UC          0x80u
/* Kept the same way: a fork's child does not get the region, as Linux's
 * VM_DONTCOPY, which MADV_DONTFORK sets and MADV_DOFORK clears. */
#define VM_PROT_DONTFORK    0x100u
/* Kept the same way: the region is locked, as Linux's VM_LOCKED, by mlock,
 * mlock2, mlockall or MAP_LOCKED. Its pages count in VmLck and against
 * RLIMIT_MEMLOCK, are brought in when it is locked, and madvise may not
 * drop them but with MADV_DONTNEED_LOCKED; a fork's child does not inherit
 * the lock. With VM_PROT_LOCKONFAULT beside it, as Linux's VM_LOCKONFAULT,
 * the pages are locked as they are touched and not brought in at once.
 * Device memory is never locked. */
#define VM_PROT_LOCKED      0x200u
#define VM_PROT_LOCKONFAULT 0x400u
#define VM_PROT_LOCKS       (VM_PROT_LOCKED | VM_PROT_LOCKONFAULT)
/* Kept the same way: the region is sealed, as mseal makes it and Linux's
 * VM_SEALED: munmap, mremap, mprotect, a mapping over it, and the madvise
 * kinds that discard pages of anonymous memory it may not write fail
 * with EPERM. A brk that would remove it keeps the break; shmdt returns 0
 * and leaves sealed regions mapped. Nothing takes the seal away; a fork's
 * child keeps it, and exec and exit end the region. */
#define VM_PROT_SEALED      0x1000u
/* Kept the same way: the region maps a file of memfd_secret, whose pages
 * only the mappings of the file reach. It is never executable, so mprotect
 * answers EACCES to PROT_EXEC as to write on VM_PROT_NOWRITE; ptrace,
 * /proc/PID/mem, process_vm_readv, the pins of devices and KVM are refused
 * its pages; and a core file leaves it out, as Linux's VM_DONTDUMP. */
#define VM_PROT_SECRET      0x2000u
/* Kept the same way: a fork's child gets the region without its pages, so
 * that it reads zeros, as Linux's VM_WIPEONFORK, which MADV_WIPEONFORK sets
 * on private anonymous memory and MADV_KEEPONFORK clears. The child keeps
 * the mark. */
#define VM_PROT_WIPEONFORK  0x4000u
/* Kept the same way, in bits 15 to 18: the region's memory protection key,
 * 0 to 15, which its pages carry in their page table entries
 * (`paging_prot_bits`) and the running thread's PKRU judges (../fpu.h).
 * A new region has key 0, but one mmap makes execute-only, which takes
 * the address space's execute-only key where it can have one;
 * pkey_mprotect gives a range the key it names, and mprotect keeps each
 * region's, or moves a range made execute-only to the execute-only key and
 * one that stops being so back to 0, as Linux does (`vm_protect_key`). */
#define VM_PROT_PKEY_SHIFT  15
#define VM_PROT_PKEY        0x78000u
#define VM_PROT_KEY(prot)   (((prot) & VM_PROT_PKEY) >> VM_PROT_PKEY_SHIFT)
/* Kept the same way: the region counts as committed memory, as Linux's
 * VM_ACCOUNT, which /proc/PID/smaps reports: the stack, the heap, a
 * writable segment of a program, and a private mapping that was writable
 * when it was made or that mprotect first makes writable, unless
 * VM_PROT_NORESERVE is set. The mark stays when the region stops being
 * writable, except on anonymous memory no page of which was written while
 * the region, or the one it was split from, held it (`vma_t`'s `anon`), as
 * Linux's mprotect_fixup drops it from a region with no anon_vma when the
 * protection changes. This kernel keeps no commit count; only smaps reads
 * it. */
#define VM_PROT_ACCOUNT     0x80000u
/* Kept the same way: the mapping was made with MAP_NORESERVE, as Linux's
 * VM_NORESERVE under overcommit_memory 0, and is never VM_PROT_ACCOUNT. */
#define VM_PROT_NORESERVE   0x100000u
/* Kept the same way: the region is the vDSO's data page and KVM's clock
 * page, [vvar], which Linux maps VM_IO | VM_PFNMAP and without VM_MAYEXEC.
 * Only the process's own accesses reach its pages: ptrace, /proc/PID/mem,
 * process_vm_readv, vmsplice and the pins of devices are refused them, as
 * Linux's get_user_pages refuses the region, and mprotect answers EACCES
 * to PROT_EXEC. */
#define VM_PROT_VVAR        0x200000u
/* Given with the access bits, which a change of them replaces: mmap or
 * mprotect gave the region write or execute without PROT_READ, so it has
 * VM_PROT_READ only because x86 paging cannot withhold a read from either.
 * Linux keeps such a region without VM_READ, which /proc/PID/maps, smaps and
 * a core file's program header show, and its get_user_pages refuses it a
 * read: process_vm_readv's and a VFIO map's always (PIN_SLOW), and
 * vmsplice's, which Linux takes by its fast path, unless the page is
 * present and not the zero page (`paging_pin_allows`). */
#define VM_PROT_NOREAD      0x400000u
/* Whether the protection `prot` has Linux's VM_READ: PROT_READ was asked. */
#define VM_READABLE(prot)   (((prot) & (VM_PROT_READ | VM_PROT_NOREAD)) == VM_PROT_READ)
/* The bits a change of the access bits keeps. */
#define VM_PROT_KEPT                                                                             \
    (VM_PROT_NOWRITE | VM_PROT_IO | VM_PROT_WC | VM_PROT_UC | VM_PROT_DONTFORK | VM_PROT_LOCKS | \
     VM_PROT_SEALED | VM_PROT_SECRET | VM_PROT_WIPEONFORK | VM_PROT_PKEY | VM_PROT_ACCOUNT |     \
     VM_PROT_NORESERVE | VM_PROT_VVAR)

/* The kind of a region, which /proc/PID/maps reports and the fault, fork
 * and brk paths read. */
typedef enum {
    VM_NONE = 0,
    VM_CODE,  /* ELF text: read + execute */
    VM_DATA,  /* ELF data/bss: read + write */
    VM_HEAP,  /* brk-managed heap */
    VM_STACK, /* the main stack (grows down) */
    VM_ANON,  /* anonymous mmap */
    VM_FILE,  /* a private mapping of a file */
    VM_SHARED /* a shared mapping: of a file in memory, or of shared anonymous memory */
} vm_type_t;

/*
 * A file a region maps. The record is the first member of its owner's own,
 * which keeps the open file; `refs` counts the regions that map it and the
 * faults reading it, each change made in one atomic step, and the last one
 * to go puts the file on the list that `vm_file_reap` empties, under a lock
 * of its own, calling `release`. A private mapping's `fill`
 * reads the file's page `pgoff` into the frame `phys`, with zeros past the
 * end of the file: 0 when the file cache had the page, 1 when it was read
 * through the file system, which makes the fault a major one, or -1 when
 * the read fails; it may sleep. A file with `share` instead lends its
 * resident frame in `*phys`, with a reference for the mapping: a shared
 * region writes it in place and a private region copies it on its first
 * write. It returns 0 for a resident page, 1 after a file-system read,
 * -ENXIO past EOF, or another negative errno when no page can be had, and
 * may sleep. A device region's `device`, set by `vm_device_map` alone,
 * gives the physical address of page `pgoff` in `*phys` and returns 0, -1
 * when the object has no page there now, which raises SIGBUS, or 1 to run
 * the access again, as `struct vm_device_ops`'s `page` says; it may sleep.
 * A file with `share` may also have `around`, which lends the frames of
 * its pages `first` to `first + count - 1` that it has in memory, in
 * `phys`, with a reference for the mapping each, and 0 for each other page,
 * reading none, and returns the count it lent; a read fault maps those
 * around its page, as Linux's do_fault_around does with a file's map_pages
 * (mm/memory.c:5254-5284 and 5300-5330). With `hits` set the file counts
 * the pages it lends as found in memory, which keeps up its read-around;
 * the fault sets it when it asks before reading its page, and clears it
 * when it asks again after a read, whose pages are no such sign. The
 * fault calls each with the regions' lock let go, and with the big lock
 * held unless `unlocked` is set, which the owner sets for a file whose
 * operations take only locks of their own, as Linux's filemap_fault takes
 * no global lock (mm/filemap.c:3305-3370). A file with `share` whose
 * shared mappings' stores it must count, a disk file's, sets `mkwrite`,
 * Linux's page_mkwrite: a writable shared region then maps each page
 * read-only until a write fault calls `mkwrite` for page `pgoff` and its
 * held frame `phys`. It returns 0 before the page gains write access, or a
 * negative errno, which leaves it read-only and fails the access. -EAGAIN
 * retries a removed or replaced page. It validates that the frame still
 * belongs to this page before marking it. Kernel process-memory writes
 * call it too. It may
 * sleep, and runs as `share` does.
 */
struct vm_file {
    uint32_t refs;
    int (*fill)(struct vm_file *f, uint32_t pgoff, uintptr_t phys);
    int (*share)(struct vm_file *f, uint32_t pgoff, uintptr_t *phys);
    void (*release)(struct vm_file *f);
    char *path;           /* the file's path for /proc/PID/maps, or NULL */
    struct vm_file *dead; /* the next file waiting for `vm_file_reap` */
    int (*device)(struct vm_file *f, uint32_t pgoff, uintptr_t *phys);
    int unlocked; /* `fill`, `share`, `around` and `device` run without the
                     big lock */
    uint32_t (*around)(struct vm_file *f, uint32_t first, uint32_t count, uintptr_t *phys,
                       int hits);
    int (*mkwrite)(struct vm_file *f, uint32_t pgoff, uintptr_t phys);
    /* Whether `b`, a file with the same `same`, maps the pages `a` does
     * through one open file, so that two neighbouring regions of them whose
     * offsets continue merge, as Linux merges regions of one struct file;
     * NULL for a file that merges only with its own regions. */
    int (*same)(const struct vm_file *a, const struct vm_file *b);
};

/*
 * What a device object does for a mapping of it (`vm_device_map`), Linux's
 * vm_operations_struct for a mapping of device memory. `page` gives the
 * physical address of the object's page `pgoff` in `*phys`: a frame of the
 * object's own, which it keeps at least until it zaps the mappings of the
 * page (`paging_zap_device`), or a frame of a device's range. It returns 0;
 * -1 when the object has no page there now, past its end, revoked or gone
 * with its device, and the access then raises SIGBUS as on Linux; or 1
 * when the page cannot be had yet, a signal having ended its wait or room
 * being short for now, and the access from user mode then runs again once
 * the thread's signals are handled, as Linux's VM_FAULT_NOPAGE, while one
 * of the kernel's copies fails with EFAULT. It may sleep. `put` drops
 * the reference to the object that the mapping held. `access`, or NULL,
 * reads or writes `len` bytes at the byte `offset` of the object for
 * ptrace and /proc/PID/mem, which cannot reach a device region's pages by
 * its page tables, as Linux's vm_operations_struct's `access`; it returns
 * 0 or a negative errno. `whole` is 1 for an object whose `page` never
 * sleeps and whose every page a mapping may take at once: the first fault
 * of a region then maps each page of it not yet present, as Linux's
 * remap_pfn_range maps a whole region at mmap for virtio-gpu's blobs of
 * host memory (virtgpu_vram.c:32-67) and i915's fault maps the whole
 * object (i915_gem_mman.c:253-295). Called under the big kernel lock.
 */
struct vm_device_ops {
    int (*page)(void *obj, uint32_t pgoff, uintptr_t *phys);
    void (*put)(void *obj);
    int (*access)(void *obj, uint64_t offset, void *buf, uint32_t len, int write);
    int whole;
};

/* The nodes a mask given to the memory policy calls may name, Linux's
 * MAX_NUMNODES as Debian builds it (CONFIG_NODES_SHIFT 10), and the words
 * of a mask of that many. The machine has one of them, node 0. */
#define VM_NODES_MAX   1024u
#define VM_NODES_WORDS (VM_NODES_MAX / (8u * sizeof(unsigned long)))

/*
 * A memory policy, Linux's struct mempolicy on a machine of one node, which
 * set_mempolicy gives a thread and mbind a region (mempolicy.c). MPOL_DEFAULT
 * is no record. A record is never changed once made: its holders share it by
 * `refs`, and a change makes a new one.
 */
struct vm_policy {
    uint32_t refs;
    uint16_t mode;  /* MPOL_PREFERRED to MPOL_WEIGHTED_INTERLEAVE */
    uint16_t flags; /* the MPOL_F_ mode flags the caller gave */
    int16_t home;   /* the node set_mempolicy_home_node gave, or -1 */
    uint16_t nodes; /* 1 for node 0; 0, no node, for MPOL_LOCAL alone */
    /* The mask the caller gave, which Linux keeps and reports when `flags`
     * is not 0; zeros otherwise. */
    unsigned long user[VM_NODES_WORDS];
};

/* A single virtual memory area: [start, end) page-aligned. A region that
 * maps a file has it in `file`, and `pgoff` is the page of the file at
 * `start`; an anonymous region has neither. */
typedef struct vma {
    uintptr_t start; /* inclusive, page-aligned */
    uintptr_t end;   /* exclusive, page-aligned */
    uint32_t prot;   /* VM_PROT_* */
    vm_type_t type;
    struct vm_file *file;
    uint32_t pgoff;
    struct vm_policy *policy; /* mbind's, with a reference; NULL for the default */
    /* 1 once `mm_note_anon` (mm.c) found a page of it written, or a
     * private copy of a file's page in it, as Linux gives a region an
     * anon_vma at its first private page; a split, a move and a fork's copy
     * keep it, unless VM_PROT_WIPEONFORK, and a merge takes either side's.
     * VM_PROT_ACCOUNT's drop reads it, and `vm_merge` refuses two private
     * regions of separate records of a file that both have it. */
    uint8_t anon;
    struct vma *left, *right, *parent; /* AVL tree links */
    struct vma *prev, *next;           /* address order; next also links spare nodes */
    unsigned int height;               /* subtree height, a leaf's is 1 */
} vma_t;

struct vm_chunk;

/* A run of guard pages, [start, end), page-aligned, that MADV_GUARD_INSTALL
 * made. */
struct vm_guard {
    uintptr_t start;
    uintptr_t end;
};

/* The regions of an address space, with its break, its stack's limit and
 * its mmap base, which exec chooses and a fork's child keeps. */
typedef struct vm_space {
    vma_t *root;
    vma_t *first, *last;
    vma_t *spare; /* unoccupied nodes, through next */
    /* Nodes of regions `vm_detach` took out, through next, each still
     * holding its file until `vm_release_detached`; `held` counts them. */
    vma_t *detached;
    uint32_t held;
    struct vm_chunk *chunks; /* allocations owned by this space */
    uint32_t cap;            /* allocated nodes: live, detached and spare */
    uint32_t count;
    uintptr_t total;       /* bytes covered by all regions */
    uintptr_t heap_start;  /* initial program break (base of the heap VMA) */
    uintptr_t brk;         /* current program break (top of the heap VMA)  */
    uintptr_t stack_limit; /* lowest address the stack VMA may grow down to */
    uintptr_t mmap_base;   /* a mapping without a hint goes below it */
    /* The VM_PROT_LOCKS bits a new region of mmap or brk takes, which
     * mlockall's MCL_FUTURE sets and munlockall clears; 0 in a new space
     * and in a fork's copy, as Linux's def_flags. */
    uint32_t def_prot;
    /* The guard pages, as Linux 6.13 and later mark them in the page
     * tables and not in the regions: `nguards` runs in address order, none
     * empty and no two touching, each inside the regions, in an array of
     * `guard_cap` that the space owns. An access to one is refused as one
     * outside every region is (`vm_guarded`), a region that holds one is
     * split and merged as any other, and its count is the regions' alone. */
    struct vm_guard *guards;
    uint32_t nguards, guard_cap;
    /* Gap search proof: above gap_top, no hole fits gap_len in [gap_lo,gap_hi).
     * A zero gap_len is invalid; removing coverage clears it. */
    uintptr_t gap_top, gap_len, gap_lo, gap_hi;
} vm_space_t;

/* kernel.randomize_va_space, read at each exec (`exec_layout`): 0 places a
 * new image at the fixed addresses of the architecture's cpu.h; any other
 * value moves its stack, its mmap base and a position-independent program
 * by random offsets, and 2 or more moves its heap's start as well, as
 * Linux reads the int its `proc_dointvec` takes. It starts at 2 where the
 * architecture draws offsets and at 0 where it draws none (ASLR_MMAP_PAGES
 * 0), and there a value written changes no address. */
extern int randomize_va_space;

/** Gap candidates examined since boot, modulo 2^32, under the big lock. */
uint32_t vm_gap_steps(void);

/* vm.max_map_count: the regions a process may hold, VMA_LIMIT at boot. A
 * change that would take a process past it fails, as one past VMA_LIMIT
 * did; a process already past a value written keeps what it has. */
extern int vm_max_map_count;

/** Reset an address space to empty, dropping the files its regions map;
 * the nodes it has, if any, are kept, and the heap's start, the break, the
 * stack's limit and the mmap base go to 0. */
void vm_init(vm_space_t *vm);

/** Drop the files the regions map and give the nodes back; the space is
 * empty after. */
void vm_free(vm_space_t *vm);

/** Make `dst`, which owns no nodes, a copy of `src` without its regions
 * marked VM_PROT_DONTFORK, whose regions take a reference each to the files
 * they map and to their policies and lose their locks, as a fork's child
 * does not inherit them, and whose `def_prot` is 0. The copy keeps the
 * guards of the regions it copies but those marked VM_PROT_WIPEONFORK, as
 * Linux's fork copies the page tables, and their guard markers, of every
 * region but those. 0, or -1 with no memory, with `dst` left empty. */
int vm_copy(vm_space_t *dst, const vm_space_t *src);

/** The region that holds `addr` (`start <= addr < end`), or NULL. */
vma_t *vm_find(vm_space_t *vm, uintptr_t addr);

/** The first region in address order, or NULL for an empty space. */
vma_t *vm_first(const vm_space_t *vm);

/** The next region in address order, or NULL after the last one.
 * A region stays at its address until removed or merged into its neighbour.
 * Callers hold the big kernel lock and must not keep it across a sleep. */
vma_t *vm_next(const vma_t *r);

/** The first region whose end is above addr, or NULL. */
vma_t *vm_lower(const vm_space_t *vm, uintptr_t addr);

/** 1 if any region meets [start, end), else 0. */
int vm_overlaps(const vm_space_t *vm, uintptr_t start, uintptr_t end);

/**
 * Add the region [start, end) with the protection `prot`, the access bits
 * and those of VM_PROT_KEPT the region needs, and the kind
 * `type`. The range must be non-empty, page-aligned at both ends and clear
 * of every region. The region has the default policy and is merged with a
 * neighbour it touches that has the same kind, protection and policy and
 * maps no file, or the same file without a gap, through the same record or
 * one its `same` accepts. Returns 0, or -1, with
 * nothing changed, for a range that breaks those rules, at
 * vm_max_map_count, or when no node can be allocated.
 */
int vm_insert(vm_space_t *vm, uintptr_t start, uintptr_t end, uint32_t prot, vm_type_t type);

/** The same, for a region that maps `file` from its page `pgoff`; the region
 * takes a reference of its own. */
int vm_insert_file(vm_space_t *vm, uintptr_t start, uintptr_t end, uint32_t prot, vm_type_t type,
                   struct vm_file *file, uint32_t pgoff);

/** Add the region [start, end) as `vm_insert_file` does, with the
 * protection, kind, file and policy of `like`, a region or a copy of one
 * whose file and policy the caller holds, mapping the file from its page
 * `pgoff`: a mapping mremap moves or grows keeps its policy, as Linux's
 * copy_vma and vma_merge_extend keep it. 0 or -1. */
int vm_insert_like(vm_space_t *vm, uintptr_t start, uintptr_t end, const vma_t *like,
                   uint32_t pgoff);

/** Take a reference to `p` and return it; NULL is passed through. */
struct vm_policy *vm_policy_get(struct vm_policy *p);

/** Drop a reference to `p`, freeing it with the last; NULL is ignored. */
void vm_policy_put(struct vm_policy *p);

/** Whether `a` and `b` are one policy, as Linux's mpol_equal: both the
 * default, or records of the same mode, flags, home node and nodes, and
 * with flags the same mask of the caller's. */
int vm_policy_equal(const struct vm_policy *a, const struct vm_policy *b);

/**
 * Give [start, end), which lies in one region, the policy `pol`, NULL for
 * the default, of which the region takes a reference: the region is split
 * at the range's ends and the piece merged with the neighbours that then
 * match, as Linux's mbind_range does for one region. A range in no region,
 * or one that passes its region's end, changes nothing. 0, or -1, with
 * nothing changed, when a split has no room.
 */
int vm_set_policy(vm_space_t *vm, uintptr_t start, uintptr_t end, struct vm_policy *pol);

/**
 * Give `vma`, a region mmap made for a file's `mmap` operation
 * (sys/fs/file.h), the backing of the device object `obj`: its pages are
 * faulted in through `ops->page` and mapped with the cache type `cache`,
 * VM_PROT_WC, VM_PROT_UC or 0 for cached. The region becomes VM_SHARED with
 * VM_PROT_IO and `cache` in its protection, and its file a new record that
 * takes over the caller's reference to `obj`, which `ops->put` drops when
 * the last region mapping it goes: at munmap, exec or exit. A fork's copy
 * of the region shares the record. 0; -EINVAL for a private mapping, whose
 * type mmap set to VM_FILE, since device memory has no copy-on-write; or
 * -ENOMEM; on an error the reference is still the caller's.
 */
int vm_device_map(vma_t *vma, const struct vm_device_ops *ops, void *obj, uint32_t cache);

/**
 * Map `len` bytes that a driver backs into the calling process, shared,
 * where the kernel finds room, as Linux's vm_mmap of a driver's own file
 * does for an ioctl that maps on the caller's behalf. `map` is given a
 * region not yet in the address space, from 0 to the length rounded up to
 * pages, VM_SHARED with the VM_PROT_* protection `prot`, and gives it its
 * file and page offset as a file's `mmap` does (vm_device_map), or an
 * error, which is returned. Returns the address, or -errno as mmap does:
 * EINVAL for a length of 0, ENOMEM for one past the address space or no
 * room for it, ESRCH with no process. It is mmap(2)'s, in syscall_proc.c.
 */
long mmap_device(uintptr_t len, uint32_t prot, int32_t (*map)(vma_t *vma, void *arg), void *arg);

/**
 * Whether madvise's `advice` over [start, end), which regions of `vm` cover
 * without a gap, is EINVAL for a region it meets: the dropping advice and
 * MADV_DOFORK over device memory; MADV_DONTNEED, MADV_FREE, MADV_COLD and
 * MADV_PAGEOUT over a locked region; and the guard advice over a region that
 * may not hold a guard. 1 or 0. madvise and process_madvise ask it;
 * it is madvise(2)'s, in syscall_proc.c.
 */
int madvise_refused(vm_space_t *vm, uintptr_t start, uintptr_t end, uint32_t advice);

/** The object a device region's file `f` maps (`vm_device_map`), or NULL
 * when `f` is NULL or no device's. */
void *vm_device_object(const struct vm_file *f);

/** Whether a fault in a device region of the file `f` may map every page
 * of the region at once, its object's `whole`: 1 or 0. */
int vm_device_whole(const struct vm_file *f);

/** Read or write `len` bytes at the byte `offset` of the object a device
 * region's file `f` maps, through its `access`: 0, its error, or -EIO when
 * `f` is no device's or its object has no `access`. May sleep. */
int vm_device_access(struct vm_file *f, uint64_t offset, void *buf, uint32_t len, int write);

/** Take a reference to `f`; NULL is ignored. */
void vm_file_get(struct vm_file *f);

/** Take a reference to `f` unless its last one has gone, as Linux's
 * get_file_rcu: 1 when taken, 0 for a file that waits for `vm_file_reap`.
 * For a walk that finds `f` with no reference of its own, since a region's
 * last put runs under the regions' lock alone. */
int vm_file_tryget(struct vm_file *f);

/** Drop a reference to `f`; NULL is ignored. The last one does not release
 * the file but puts it on the list `vm_file_reap` empties, so that a change
 * to the regions never sleeps part way through. The count falls to 0 under
 * the list's lock, in the same hold that puts the file on the list, so a
 * reap that follows a reading of 0 finds the file there or already taken. */
void vm_file_put(struct vm_file *f);

/** Whether a file waits for `vm_file_reap`; read without the list's lock,
 * as a hint. */
int vm_file_dead(void);

/**
 * Release every file whose last reference went. A release may sleep while
 * it closes the file, and a region that still named the file would then be
 * copied by a fork, or its pages reached through the page tables, after the
 * file is gone. A caller runs this once the regions it changed and their
 * page tables agree, and never between the two.
 */
void vm_file_reap(void);

/**
 * Take [start, end) out of every region it meets, as munmap does: a region
 * it covers goes, one it overlaps at one end is trimmed, and one that holds
 * it is split in two. The guard pages of the range go with it. An empty
 * range changes nothing. 0, or -1 when the split of a region or of a run of
 * guard pages has no room, with nothing changed.
 *
 * A region that goes keeps its reference to the file it maps, on the
 * space's detached list, until `vm_release_detached`, which the caller runs
 * once the range's entries are cleared and their translations flushed from
 * every processor. The last reference would otherwise reach the dead list
 * first, and another thread's `vm_file_reap`, which no lock of this space
 * holds off, could release a device object or a buffer whose memory a
 * processor still reaches through the old entries. Linux's munmap frees
 * the detached regions only after unmap_region has cleared the entries and
 * finished the TLB gather (mm/vma.c:1122,1164,1180 in Linux 6.12.115).
 */
int vm_detach(vm_space_t *vm, uintptr_t start, uintptr_t end);

/** Drop the file references of the regions `vm_detach` took out of `vm`
 * and give their nodes back; the caller holds the regions' lock to write,
 * and the range's entries are gone from every processor. */
void vm_release_detached(vm_space_t *vm);

/** `vm_detach` and `vm_release_detached` at once, for a range no entry
 * maps, or whose files the caller holds across the clearing of its
 * entries. */
int vm_remove(vm_space_t *vm, uintptr_t start, uintptr_t end);

/**
 * Give [start, end), which regions must cover without a gap, the access bits
 * `prot`, each region keeping its bits of VM_PROT_KEPT but VM_PROT_ACCOUNT,
 * which a private region made writable takes and anonymous memory whose
 * `anon` is 0 loses when left without write; the
 * regions the range cuts are split at its ends, and the pieces merge with
 * neighbours that then match. An empty range changes nothing. Returns 0; -1
 * for a gap, as Linux's mprotect answers ENOMEM; -2 when a split has no
 * room; -3 when `prot` has write and a region of the range has
 * VM_PROT_NOWRITE, or execute and a region has VM_PROT_SECRET, as Linux
 * answers EACCES. Of -1 and -3, the one met first
 * in address order is the answer, and every error leaves the regions as
 * they were.
 */
int vm_protect(vm_space_t *vm, uintptr_t start, uintptr_t end, uint32_t prot);

/**
 * `vm_protect`, with each region's protection key chosen as Linux's
 * arch_override_mprotect_pkey chooses it: `key` when it is 0 or more, as
 * pkey_mprotect names one; otherwise, as mprotect, the region's own, but 0
 * for a region whose key is `exec_key`, the address space's execute-only
 * key, or -1 for none. The answers are `vm_protect`'s.
 */
int vm_protect_key(vm_space_t *vm, uintptr_t start, uintptr_t end, uint32_t prot, int key,
                   int exec_key);

/**
 * Set the bits `set` and clear the bits `clear` of VM_PROT_DONTFORK,
 * VM_PROT_SEALED and VM_PROT_WIPEONFORK in the protection of
 * each region of [start, end), which regions must cover without a gap,
 * splitting and merging as `vm_protect` does. Returns 0; -1 for a gap; -2 when
 * a split has no room. An error leaves the regions as they were.
 */
int vm_advise(vm_space_t *vm, uintptr_t start, uintptr_t end, uint32_t set, uint32_t clear);

/**
 * Give each region of [start, end) but those of device memory and of
 * memfd_secret the lock bits `bits`, VM_PROT_LOCKED alone, with
 * VM_PROT_LOCKONFAULT, or 0 to unlock, in place of those it had, as Linux's
 * mlock_fixup passes those over; [start, end) must be covered without a
 * gap, and the regions it cuts are split at its ends and merged as
 * `vm_protect` does. Returns 0; -1 for a gap; -2 when a split has no room.
 * An error leaves the regions as they were.
 */
int vm_lock(vm_space_t *vm, uintptr_t start, uintptr_t end, uint32_t bits);

/** Give every region but those of device memory and of memfd_secret the
 * lock bits `bits`, as `vm_lock` does over the whole space, for mlockall
 * and munlockall; no region is split, so it cannot fail. */
void vm_lock_all(vm_space_t *vm, uint32_t bits);

/** Bytes of [start, end) that locked regions cover, and with the whole
 * address space for a range, VmLck of /proc/PID/status. */
uintptr_t vm_locked_in(const vm_space_t *vm, uintptr_t start, uintptr_t end);

/** Bytes the locked regions cover together: VmLck of /proc/PID/status and
 * what RLIMIT_MEMLOCK bounds for the mlock calls. */
uintptr_t vm_locked(const vm_space_t *vm);

/**
 * Where Linux's mprotect, which changes the regions of [start, end) one
 * after another, stops for the access bits `prot`: the first address of
 * the range that no region holds, `*why` -1, as Linux answers ENOMEM; the
 * start of the first region that refuses `prot`, `*why` -3, as `vm_protect`
 * answers EACCES; or the start of the first sealed region, `*why` -4, as
 * Linux answers EPERM, a region that refuses first giving -3; `start`
 * itself when the first region stops the change there. With no stop, `end`
 * and `*why` 0.
 */
uintptr_t vm_protect_stop(const vm_space_t *vm, uintptr_t start, uintptr_t end, uint32_t prot,
                          int *why);

/** The start of the first region of [start, end) with VM_PROT_SEALED,
 * `start` when that region begins below it, or `end` when no sealed region
 * meets the range. */
uintptr_t vm_sealed_at(const vm_space_t *vm, uintptr_t start, uintptr_t end);

/** Make room for `n` more regions now, and for `n` more runs of guard
 * pages when the space has any, so the changes after cannot fail; 0 ok, -1
 * at the limit or with no memory. */
int vm_reserve(vm_space_t *vm, uint32_t n);

/** The start of the highest free range of `len` bytes inside [lo, hi), or 0
 * when there is none. */
uintptr_t vm_find_free(vm_space_t *vm, uintptr_t len, uintptr_t lo, uintptr_t hi);

/** Find a mapping below `base` in [lo, hi), then search the whole range
 * from low addresses when the top-down search has no room. Returns 0 when
 * no gap holds `len` bytes. The caller holds the address space stable. */
uintptr_t vm_find_mmap(vm_space_t *vm, uintptr_t len, uintptr_t lo, uintptr_t base, uintptr_t hi);

/** Bytes of [start, end) that regions cover. */
uintptr_t vm_covered(const vm_space_t *vm, uintptr_t start, uintptr_t end);

/** Bytes the regions cover together: what RLIMIT_AS is measured against. */
uintptr_t vm_total(const vm_space_t *vm);

/**
 * Grow a VM_STACK region down to start at `page`, page-aligned, for a fault
 * below it: 1 if grown, 0 if not. `page` must lie below the region's start
 * and at or above `stack_limit`; the growth counts against `as_limit`
 * (RLIMIT_AS) as a mapping does; and the new span must be clear of every
 * other region, so that a segment or a mapping placed in the stack's room
 * is not taken into the stack. The pages between are backed as they are
 * touched.
 */
int vm_grow_stack(vm_space_t *vm, uintptr_t page, uintptr_t as_limit);

/** Whether the page at `addr` is a guard page (`vm_space_t`'s `guards`):
 * 1 or 0. The caller holds the regions' lock, to read or to write. */
int vm_guarded(const vm_space_t *vm, uintptr_t addr);

/**
 * Make each page of [start, end), page-aligned, a guard page when `on`, as
 * MADV_GUARD_INSTALL does, or no longer one, as MADV_GUARD_REMOVE does; a
 * page that already is, or is not, stays so. The regions must cover the
 * range when `on`; none is split. The caller clears the range's entries.
 * 0, or -1 with no room, with nothing changed.
 */
int vm_guard_set(vm_space_t *vm, uintptr_t start, uintptr_t end, int on);

/**
 * Give [to, to + len), which holds no guard page, the guard pages of
 * [from, from + len) at the same offsets, for mremap's move of a mapping,
 * whose guards go with its pages as Linux's move_page_tables carries the
 * markers; and make room for the cut of a run of guard pages, which the
 * `vm_detach` of the source that follows may need, so that it cannot fail.
 * The two ranges do not overlap. 0, or -1 with no room, with nothing
 * changed.
 */
int vm_guard_copy(vm_space_t *vm, uintptr_t from, uintptr_t len, uintptr_t to);

#endif /* _VM_H_ */
