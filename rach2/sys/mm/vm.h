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
/* Kept the same way: the region is a guard, as MADV_GUARD_INSTALL makes
 * one and MADV_GUARD_REMOVE takes away. It has no pages, and an access to
 * it, the kernel's on the process's behalf too, is refused as one outside
 * every region is: SIGSEGV with SEGV_MAPERR, or EFAULT. mprotect, mlock, a
 * fork and mremap keep it, MADV_DONTNEED leaves it, and munmap or a mapping
 * over it ends it. */
#define VM_PROT_GUARD       0x800u
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
/* The bits a change of the access bits keeps. */
#define VM_PROT_KEPT                                                                             \
    (VM_PROT_NOWRITE | VM_PROT_IO | VM_PROT_WC | VM_PROT_UC | VM_PROT_DONTFORK | VM_PROT_LOCKS | \
     VM_PROT_GUARD | VM_PROT_SEALED | VM_PROT_SECRET | VM_PROT_WIPEONFORK | VM_PROT_PKEY)

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
 * faults reading it, and the last one to go puts the file on the list that
 * `vm_file_reap` empties, calling `release`. A private mapping's `fill`
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
 */
struct vm_file {
    uint32_t refs;
    int (*fill)(struct vm_file *f, uint32_t pgoff, uintptr_t phys);
    int (*share)(struct vm_file *f, uint32_t pgoff, uintptr_t *phys);
    void (*release)(struct vm_file *f);
    char *path;           /* the file's path for /proc/PID/maps, or NULL */
    struct vm_file *dead; /* the next file waiting for `vm_file_reap` */
    int (*device)(struct vm_file *f, uint32_t pgoff, uintptr_t *phys);
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
 * 0 or a negative errno. Called under the big kernel lock.
 */
struct vm_device_ops {
    int (*page)(void *obj, uint32_t pgoff, uintptr_t *phys);
    void (*put)(void *obj);
    int (*access)(void *obj, uint64_t offset, void *buf, uint32_t len, int write);
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
    struct vm_policy *policy;          /* mbind's, with a reference; NULL for the default */
    struct vma *left, *right, *parent; /* AVL tree links */
    struct vma *prev, *next;           /* address order; next also links spare nodes */
    unsigned int height;               /* subtree height, a leaf's is 1 */
} vma_t;

struct vm_chunk;

/* The regions of an address space, with its break, its stack's limit and
 * its mmap base, which exec chooses and a fork's child keeps. */
typedef struct vm_space {
    vma_t *root;
    vma_t *first, *last;
    vma_t *spare;            /* unoccupied nodes, through next */
    struct vm_chunk *chunks; /* allocations owned by this space */
    uint32_t cap;            /* allocated nodes, including spares */
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
    /* Gap search proof: above gap_top, no hole fits gap_len in [gap_lo,gap_hi).
     * A zero gap_len is invalid; removing coverage clears it. */
    uintptr_t gap_top, gap_len, gap_lo, gap_hi;
} vm_space_t;

/* kernel.randomize_va_space, read at each exec (`exec_layout`): 0 places a
 * new image at the fixed addresses of the architecture's cpu.h; 1 moves its
 * stack, its mmap base and a position-independent program by random
 * offsets; 2 moves its heap's start as well. It starts at 2 where the
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
 * does not inherit them, and whose `def_prot` is 0; 0, or -1 with no
 * memory, with `dst` left empty. */
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
 * maps no file, or the same file without a gap. Returns 0, or -1, with
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

/** Read or write `len` bytes at the byte `offset` of the object a device
 * region's file `f` maps, through its `access`: 0, its error, or -EIO when
 * `f` is no device's or its object has no `access`. May sleep. */
int vm_device_access(struct vm_file *f, uint64_t offset, void *buf, uint32_t len, int write);

/** Take a reference to `f`; NULL is ignored. */
void vm_file_get(struct vm_file *f);

/** Drop a reference to `f`; NULL is ignored. The last one does not release
 * the file but puts it on the list `vm_file_reap` empties, so that a change
 * to the regions never sleeps part way through. */
void vm_file_put(struct vm_file *f);

/**
 * Release every file whose last reference went. A release may sleep while
 * it closes the file, and a region that still named the file would then be
 * copied by a fork, or its pages reached through the page tables, after the
 * file is gone. A caller runs this once the regions it changed and their
 * page tables agree, and never between the two.
 */
void vm_file_reap(void);

/** Take [start, end) out of every region it meets, as munmap does: a region
 * it covers goes, one it overlaps at one end is trimmed, and one that holds
 * it is split in two. An empty range changes nothing. 0, or -1 when the
 * split has no room, with nothing changed. */
int vm_remove(vm_space_t *vm, uintptr_t start, uintptr_t end);

/**
 * Give [start, end), which regions must cover without a gap, the access bits
 * `prot`, each region keeping its bits of VM_PROT_KEPT; the
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
 * VM_PROT_GUARD, VM_PROT_SEALED and VM_PROT_WIPEONFORK in the protection of
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

/** The start of the first region of [start, end) with VM_PROT_SEALED,
 * `start` when that region begins below it, or `end` when no sealed region
 * meets the range. */
uintptr_t vm_sealed_at(const vm_space_t *vm, uintptr_t start, uintptr_t end);

/** Make room for `n` more regions now, so the changes after cannot fail;
 * 0 ok, -1 at the limit or with no memory. */
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
 * touched. Below a guard (VM_PROT_GUARD) the growth is a region of its own,
 * which is no guard.
 */
int vm_grow_stack(vm_space_t *vm, uintptr_t page, uintptr_t as_limit);

#endif /* _VM_H_ */
