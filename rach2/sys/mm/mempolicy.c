/*
 * mempolicy.c: the NUMA memory policy calls on a machine of one memory
 * node, node 0, answered as Linux 6.12's mm/mempolicy.c and mm/migrate.c
 * answer them on such a machine: a thread's policy (set_mempolicy), a
 * range's (mbind, set_mempolicy_home_node), both read back by
 * get_mempolicy, and the calls that move pages between nodes
 * (migrate_pages, move_pages), which find every page on node 0 already.
 * libnuma, numactl, QEMU's memory backends and the JVM make them.
 */

#include "mempolicy.h"

#include "filemap.h"
#include "kheap.h"
#include "mm.h"
#include "paging.h"
#include "shmem.h"
#include "usercopy.h"
#include "vm.h"
#include "../proc/ns.h"
#include "../proc/percpu.h"
#include "../proc/process.h"
#include "../arch/arch.h"
#include "../lib/kerrno.h"
#include "../lib/kstring.h"
#include "../syscall.h"

/* The flags of get_mempolicy: the node of a page or of the next interleave,
 * the policy of an address, and the nodes the thread may use. */
#define MPOL_F_NODE         1u
#define MPOL_F_ADDR         2u
#define MPOL_F_MEMS_ALLOWED 4u

/* The flags of mbind, and of them those move_pages takes: fail when a page
 * of the range is on a node the policy does not allow, move such pages, and
 * move those other processes map as well. */
#define MPOL_MF_STRICT   1u
#define MPOL_MF_MOVE     2u
#define MPOL_MF_MOVE_ALL 4u
#define MPOL_MF_VALID    (MPOL_MF_STRICT | MPOL_MF_MOVE | MPOL_MF_MOVE_ALL)

/* The nodes the machine has, Linux's nr_node_ids. */
#define NR_NODE_IDS 1u

#define BITS_PER_LONG (8u * sizeof(unsigned long))

/* The pages whose status move_pages reads and writes in one step, Linux's
 * DO_PAGES_STAT_CHUNK_NR. */
#define STAT_CHUNK 16u

/** Whether the mask `nodes` of VM_NODES_WORDS words names no node. */
static int nodes_empty(const unsigned long *nodes)
{
    uint32_t i;

    for (i = 0; i < VM_NODES_WORDS; i++) {
        if (nodes[i] != 0) {
            return 0;
        }
    }
    return 1;
}

/**
 * Read `bits` bits, VM_NODES_MAX at most, of the user's mask at `umask`
 * into `mask`, as Linux's get_bitmap: whole longs of the kernel's, or for
 * an i386 caller on x86-64 whole 32-bit words, with the bits past `bits` in
 * the last long cleared. 0 or -EFAULT.
 */
static int get_bitmap(unsigned long *mask, uintptr_t umask, unsigned long bits)
{
    unsigned long nlongs = (bits + BITS_PER_LONG - 1u) / BITS_PER_LONG;
    uint32_t words[VM_NODES_MAX / 32u];
    unsigned long nwords = (bits + 31u) / 32u;
    unsigned long i;

    if (in_compat_syscall()) {
        if (copy_from_user(words, (const void __user *)umask, nwords * sizeof(words[0])) != 0) {
            return -EFAULT;
        }
        k_memset(mask, 0, nlongs * sizeof(mask[0]));
        for (i = 0; i < nwords; i++) {
            mask[i * 32u / BITS_PER_LONG] |= (unsigned long)words[i] << (i * 32u % BITS_PER_LONG);
        }
    } else if (copy_from_user(mask, (const void __user *)umask, nlongs * sizeof(mask[0])) != 0) {
        return -EFAULT;
    }
    if (bits % BITS_PER_LONG) {
        mask[nlongs - 1u] &= (1ul << (bits % BITS_PER_LONG)) - 1ul;
    }
    return 0;
}

/**
 * Read the node mask at `umask` of `maxnode` bits into `nodes`, of
 * VM_NODES_WORDS words, as Linux's get_nodes: the mask has `maxnode - 1`
 * bits; a null `umask`, or a `maxnode` of 1, is the empty mask; more than
 * a page of bits is -EINVAL, and so is a `maxnode` of 0 with a mask, since
 * `maxnode - 1` wraps; and of a mask longer than VM_NODES_MAX the part past it
 * is read a long at a time from its end, the whole of the long that holds
 * its last bit included, and must be zeros, or the answer is -EINVAL. An
 * unreadable mask is -EFAULT.
 */
static int get_nodes(unsigned long *nodes, uintptr_t umask, unsigned long maxnode)
{
    unsigned long t;

    --maxnode;
    k_memset(nodes, 0, VM_NODES_WORDS * sizeof(nodes[0]));
    if (maxnode == 0 || umask == 0) {
        return 0;
    }
    if (maxnode > PAGE_SIZE * 8u) {
        return -EINVAL;
    }
    while (maxnode > VM_NODES_MAX) {
        if (get_bitmap(&t, umask + (maxnode - 1u) / BITS_PER_LONG * sizeof(unsigned long),
                       BITS_PER_LONG) != 0) {
            return -EFAULT;
        }
        maxnode = maxnode - BITS_PER_LONG >= VM_NODES_MAX ? maxnode - BITS_PER_LONG : VM_NODES_MAX;
        if (t != 0) {
            return -EINVAL;
        }
    }
    return get_bitmap(nodes, umask, maxnode);
}

/**
 * Split the `mode` argument of set_mempolicy and mbind into the mode, left
 * in `*mode`, and its flags, as Linux's sanitize_mpol_flags: a mode past
 * MPOL_WEIGHTED_INTERLEAVE, MPOL_F_STATIC_NODES with MPOL_F_RELATIVE_NODES,
 * and MPOL_F_NUMA_BALANCING with a mode other than MPOL_BIND and
 * MPOL_PREFERRED_MANY are -EINVAL.
 */
static int mode_split(int *mode, uint16_t *flags)
{
    *flags = (uint16_t)((unsigned int)*mode & MPOL_MODE_FLAGS);
    *mode = (int)((unsigned int)*mode & ~MPOL_MODE_FLAGS);
    if ((unsigned int)*mode >= MPOL_MAX) {
        return -EINVAL;
    }
    if ((*flags & MPOL_F_STATIC_NODES) && (*flags & MPOL_F_RELATIVE_NODES)) {
        return -EINVAL;
    }
    if ((*flags & MPOL_F_NUMA_BALANCING) && *mode != MPOL_BIND && *mode != MPOL_PREFERRED_MANY) {
        return -EINVAL;
    }
    return 0;
}

/**
 * Make the policy of `mode`, `flags` and the caller's mask `nodes` in
 * `*out`, with one reference, or NULL there for MPOL_DEFAULT, as Linux's
 * mpol_new and mpol_set_nodemask make it where node 0 is the one node with
 * memory and the one the caller may use. MPOL_DEFAULT and MPOL_LOCAL take
 * an empty mask alone, and MPOL_LOCAL no node flag; MPOL_PREFERRED with an
 * empty mask is MPOL_LOCAL, and takes no node flag then; the other modes
 * need a mask that leaves node 0 once it is cut to the machine's nodes,
 * which under MPOL_F_RELATIVE_NODES any node does, the mask being folded
 * onto the one node. The policy keeps the caller's mask when it has a
 * flag. 0, -EINVAL or -ENOMEM.
 */
static int policy_new(int mode, uint16_t flags, const unsigned long *nodes, struct vm_policy **out)
{
    int empty = nodes_empty(nodes);
    int node_flags = (flags & (MPOL_F_STATIC_NODES | MPOL_F_RELATIVE_NODES)) != 0;
    struct vm_policy *p;

    *out = NULL;
    if (mode == MPOL_DEFAULT) {
        return empty ? 0 : -EINVAL;
    }
    if (mode == MPOL_PREFERRED && empty) {
        if (node_flags) {
            return -EINVAL;
        }
        mode = MPOL_LOCAL;
    } else if (mode == MPOL_LOCAL ? !empty || node_flags : empty) {
        return -EINVAL;
    }
    if (mode != MPOL_LOCAL && !(flags & MPOL_F_RELATIVE_NODES) && !(nodes[0] & 1ul)) {
        return -EINVAL;
    }
    p = kzalloc(sizeof(*p));
    if (!p) {
        return -ENOMEM;
    }
    p->refs = 1;
    p->mode = (uint16_t)mode;
    p->flags = flags;
    p->home = -1;
    p->nodes = mode != MPOL_LOCAL;
    if (flags && mode != MPOL_LOCAL) {
        k_memcpy(p->user, nodes, sizeof(p->user));
    }
    *out = p;
    return 0;
}

const struct vm_policy *mempolicy_at(const vma_t *vma, uintptr_t addr)
{
    const struct shmem *sh = filemap_in_memory(vma->file) ? filemap_shmem(vma->file) : NULL;
    const struct shmem_policy *n;
    uint32_t pgoff;

    if (!sh) {
        return vma->policy;
    }
    pgoff = vma->pgoff + (uint32_t)((addr - vma->start) / PAGE_SIZE);
    for (n = sh->policies; n && n->start <= pgoff; n = n->next) {
        if (pgoff < n->end) {
            return &n->pol;
        }
    }
    return NULL;
}

/**
 * Give the pages [start, end) of the memory `sh` the policy `pol`, or with
 * NULL none, as Linux's shared_policy_replace: the records the range
 * covers go, those it cuts are trimmed, and one that holds it is split in
 * two around it. `spare` holds two records the caller allocated, which the
 * new record and a split's second half are taken from, so that nothing
 * here fails; a record taken leaves NULL in its place.
 */
static void shared_set(struct shmem *sh, uint32_t start, uint32_t end, const struct vm_policy *pol,
                       struct shmem_policy **spare)
{
    struct shmem_policy **link = &sh->policies;
    struct shmem_policy *n;

    while (*link && (*link)->end <= start) {
        link = &(*link)->next;
    }
    n = *link;
    if (n && n->start < start) {
        if (n->end > end) {
            *spare[1] = *n;
            spare[1]->start = end;
            n->next = spare[1];
            spare[1] = NULL;
        }
        n->end = start;
        link = &n->next;
    }
    while ((n = *link) != NULL && n->start < end) {
        if (n->end > end) {
            n->start = end;
            break;
        }
        *link = n->next;
        kfree(n);
    }
    if (pol) {
        n = spare[0];
        spare[0] = NULL;
        n->start = start;
        n->end = end;
        n->pol = *pol;
        n->next = *link;
        *link = n;
    }
}

/**
 * Give each region's part of [start, end) the policy `pol`, NULL for the
 * default, as Linux's mbind_range does region by region: a region whose
 * own policy is `pol` already is passed over; any other is split at the
 * range's ends and takes it (`vm_set_policy`), and the pages of a file in
 * memory that the part maps take it too (`shared_set`), which is the
 * policy get_mempolicy reads for them. Gaps are passed. The walk goes by
 * address and not by region, since a part that takes the policy may join
 * the region after the range. 0, or -ENOMEM when a split or a record has no
 * room, with the regions before changed.
 */
static int range_bind(struct mm *mm, uintptr_t start, uintptr_t end, struct vm_policy *pol)
{
    struct shmem_policy *spare[2] = {NULL, NULL};
    uintptr_t at = start;
    const vma_t *r;
    int rc = 0;

    while (rc == 0 && at < end && (r = vm_lower(&mm->vm, at)) != NULL && r->start < end) {
        uintptr_t s = r->start > at ? r->start : at;
        uintptr_t e = r->end < end ? r->end : end;
        struct shmem *sh = filemap_in_memory(r->file) ? filemap_shmem(r->file) : NULL;
        uint32_t pgoff = r->pgoff + (uint32_t)((s - r->start) / PAGE_SIZE);

        at = e;
        if (vm_policy_equal(r->policy, pol)) {
            continue;
        }
        spare[0] = spare[0] ? spare[0] : kmalloc(sizeof(*spare[0]));
        spare[1] = spare[1] ? spare[1] : kmalloc(sizeof(*spare[1]));
        if (!spare[0] || !spare[1] || vm_set_policy(&mm->vm, s, e, pol) != 0) {
            rc = -ENOMEM;
        } else if (sh) {
            shared_set(sh, pgoff, pgoff + (uint32_t)((e - s) / PAGE_SIZE), pol, spare);
        }
    }
    kfree(spare[0]);
    kfree(spare[1]);
    return rc;
}

/**
 * What Linux's queue_pages_range finds for mbind over [start, end) before
 * a policy is given: -EFAULT when no region meets the range, and, unless
 * the policy is the default's (`gaps_ok`), when the range begins before
 * its first region or a region inside it ends at a gap; else the pages
 * that could not be moved, 0 or more, or -EIO. A page is misplaced only
 * when the caller's mask leaves node 0 out (`misplaced`), every page being
 * there: a page of memory then, not one a read brought in zeroed, which is
 * Linux's zero page, and not device memory. Under MPOL_MF_STRICT alone
 * the first such page is -EIO; with a move flag each is moved, to node 0
 * again, which fails for one pinned for a device alone, and those are
 * counted. The checks run region by region in address order, a region's
 * gap before its pages.
 */
static long range_scan(struct mm *mm, uintptr_t start, uintptr_t end, unsigned int flags,
                       int gaps_ok, int misplaced)
{
    const vma_t *r = vm_lower(&mm->vm, start);
    struct paging_frame_counts counts;
    long failed = 0;

    if (!r || r->start >= end || (!gaps_ok && r->start > start)) {
        return -EFAULT;
    }
    for (; r && r->start < end; r = vm_next(r)) {
        uintptr_t s = r->start > start ? r->start : start;
        uintptr_t e = r->end < end ? r->end : end;

        if (!gaps_ok && r->end < end && (!vm_next(r) || vm_next(r)->start > r->end)) {
            return -EFAULT;
        }
        if (!misplaced || !(flags & MPOL_MF_STRICT) || (r->prot & VM_PROT_IO)) {
            continue;
        }
        paging_user_frames(mm->dir, s, e, &counts);
        if (counts.frames != 0 && !(flags & (MPOL_MF_MOVE | MPOL_MF_MOVE_ALL))) {
            return -EIO;
        }
        failed += (long)counts.pinned;
    }
    return failed;
}

/**
 * mbind(start, len, mode, nodemask, maxnode, flags), in Linux 6.12's order
 * of checks: a bad mode or mode flag (`mode_split`) and a bad mask
 * (`get_nodes`) come first; then a flag past MPOL_MF_VALID is -EINVAL,
 * MPOL_MF_MOVE_ALL without privilege -EPERM, a start not on a page
 * boundary and a range that wraps -EINVAL, and an empty range 0; then the
 * policy is made (`policy_new`), the range is checked (`range_scan`), and
 * the policy is given to each region of it (`range_bind`). MPOL_DEFAULT
 * drops MPOL_MF_STRICT and passes gaps. Pages a move flag could not move
 * are -EIO under MPOL_MF_STRICT, with the policy given.
 */
long sys_mbind(isr_params_t *params)
{
    uintptr_t start = SC_ARG(params, 0);
    uintptr_t end = start + ((SC_ARG(params, 1) + PAGE_SIZE - 1u) & ~(uintptr_t)(PAGE_SIZE - 1u));
    int mode = (int)SC_ARG(params, 2);
    unsigned int flags = (unsigned int)SC_ARG(params, 5);
    unsigned long nodes[VM_NODES_WORDS];
    struct vm_policy *pol;
    uint16_t mflags;
    long rc;

    rc = mode_split(&mode, &mflags);
    if (rc == 0) {
        rc = get_nodes(nodes, SC_ARG(params, 3), SC_ARG(params, 4));
    }
    if (rc != 0) {
        return rc;
    }
    if (flags & ~MPOL_MF_VALID) {
        return -EINVAL;
    }
    if ((flags & MPOL_MF_MOVE_ALL) && !proc_privileged(current_proc)) {
        return -EPERM;
    }
    if ((start & (PAGE_SIZE - 1u)) != 0 || end < start) {
        return -EINVAL;
    }
    if (end == start) {
        return 0;
    }
    if (mode == MPOL_DEFAULT) {
        flags &= ~MPOL_MF_STRICT;
    }
    rc = policy_new(mode, mflags, nodes, &pol);
    if (rc != 0) {
        return rc;
    }
    rc = current_proc->mm
             ? range_scan(current_proc->mm, start, end, flags, pol == NULL, !(nodes[0] & 1ul))
             : -EFAULT;
    if (rc >= 0) {
        long failed = rc;

        rc = range_bind(current_proc->mm, start, end, pol);
        if (rc == 0 && failed != 0 && (flags & MPOL_MF_STRICT)) {
            rc = -EIO;
        }
    }
    vm_policy_put(pol);
    return rc;
}

/**
 * set_mempolicy(mode, nodemask, maxnode): give the calling thread the
 * policy (`mode_split`, `get_nodes`, `policy_new`) in place of the one it
 * had; MPOL_DEFAULT leaves it none. A fork and a new thread take the
 * caller's, and an exec keeps it. 0 or -errno.
 */
long sys_set_mempolicy(isr_params_t *params)
{
    int mode = (int)SC_ARG(params, 0);
    unsigned long nodes[VM_NODES_WORDS];
    struct vm_policy *pol = NULL;
    uint16_t mflags;
    int rc;

    rc = mode_split(&mode, &mflags);
    if (rc == 0) {
        rc = get_nodes(nodes, SC_ARG(params, 1), SC_ARG(params, 2));
    }
    if (rc == 0) {
        rc = policy_new(mode, mflags, nodes, &pol);
    }
    if (rc != 0) {
        return rc;
    }
    vm_policy_put(current_proc->mempolicy);
    current_proc->mempolicy = pol;
    return 0;
}

/**
 * set_mempolicy_home_node(start, len, home_node, flags), as Linux 6.12's:
 * a start not on a page boundary, a flag, a node the machine does not
 * have and a range that wraps are -EINVAL, and an empty range is 0. Each
 * region of the range with a policy of its own, in address order, takes a
 * copy of it with node 0 as its home (`range_bind`), which get_mempolicy
 * does not report; a region without one is passed. A policy that is
 * neither MPOL_BIND nor MPOL_PREFERRED_MANY ends the walk with
 * -EOPNOTSUPP, the regions before it changed. 0 when the last region with
 * a policy took its home, -ENOENT when no region had one, or -ENOMEM.
 */
long sys_set_mempolicy_home_node(isr_params_t *params)
{
    uintptr_t start = SC_ARG(params, 0);
    uintptr_t end = start + ((SC_ARG(params, 1) + PAGE_SIZE - 1u) & ~(uintptr_t)(PAGE_SIZE - 1u));
    struct mm *mm = current_proc->mm;
    uintptr_t at = start;
    const vma_t *r;
    long rc = -ENOENT;

    if ((start & (PAGE_SIZE - 1u)) != 0 || SC_ARG(params, 3) != 0 ||
        SC_ARG(params, 2) >= NR_NODE_IDS || end < start) {
        return -EINVAL;
    }
    if (end == start) {
        return 0;
    }
    while (mm && at < end && (r = vm_lower(&mm->vm, at)) != NULL && r->start < end) {
        uintptr_t s = r->start > at ? r->start : at;
        struct vm_policy *pol;

        at = r->end < end ? r->end : end;
        if (!r->policy) {
            continue;
        }
        if (r->policy->mode != MPOL_BIND && r->policy->mode != MPOL_PREFERRED_MANY) {
            return -EOPNOTSUPP;
        }
        pol = kmalloc(sizeof(*pol));
        if (!pol) {
            return -ENOMEM;
        }
        *pol = *r->policy;
        pol->refs = 1;
        pol->home = 0;
        rc = range_bind(mm, s, at, pol);
        vm_policy_put(pol);
        if (rc != 0) {
            break;
        }
    }
    return rc;
}

/**
 * The node of the page at `addr` of the current process, brought in for a
 * read as Linux's get_user_pages does, which is node 0, or -EFAULT for a
 * page that cannot be read: one of a PROT_NONE region, a guard or device
 * memory, or secret memory.
 */
static int32_t mempolicy_page_node(uintptr_t addr)
{
    const vma_t *vma = vm_find(&current_proc->mm->vm, addr);

    if (!vma || (vma->prot & (VM_PROT_IO | VM_PROT_SECRET)) ||
        !vm_prefault_range(addr & ~(uintptr_t)(PAGE_SIZE - 1u), 1, 0)) {
        return -EFAULT;
    }
    return 0;
}

/**
 * Write the node mask `nodes` to the user's `umask` of `maxnode` bits as
 * Linux's copy_nodes_to_user writes it: as many bytes as `maxnode - 1` bits
 * take in 64-bit words, of which those past the machine's one long, a
 * 32-bit one for an i386 caller, are cleared, and more than a page is
 * -EINVAL. A native caller's `maxnode` of 1 so gets no byte, an i386
 * caller's always gets its one word, as on Linux. 0 or -errno.
 */
static int32_t mempolicy_put_nodes(uintptr_t umask, unsigned long maxnode, unsigned long nodes)
{
    unsigned long copy = ((maxnode - 1u + 63u) & ~63ul) / 8u;
    unsigned long nbytes = sizeof(unsigned long);
    static const unsigned char zeros[64];
    unsigned long at;

    if (in_compat_syscall()) {
        nbytes = sizeof(uint32_t);
    }
    if (copy > nbytes) {
        if (copy > PAGE_SIZE) {
            return -EINVAL;
        }
        for (at = nbytes; at < copy; at += sizeof(zeros)) {
            unsigned long n = copy - at < sizeof(zeros) ? copy - at : sizeof(zeros);

            if (copy_to_user((void __user *)(umask + at), zeros, n) != 0) {
                return -EFAULT;
            }
        }
        copy = nbytes;
    }
    if (in_compat_syscall()) {
        uint32_t word = (uint32_t)nodes;

        return copy_to_user((void __user *)umask, &word, sizeof(word)) != 0 ? -EFAULT : 0;
    }
    return copy_to_user((void __user *)umask, &nodes, copy) != 0 ? -EFAULT : 0;
}

/**
 * What get_mempolicy reports of the policy `pol`, NULL for the default:
 * its mode with its mode flags in `*mode`, and in `*nodes` the first long
 * of its node mask, which is the one long Linux copies out on a machine of
 * one node: the mask the caller gave for a policy with a flag, and else
 * node 0, or nothing for the default and MPOL_LOCAL.
 */
static void policy_report(const struct vm_policy *pol, int32_t *mode, unsigned long *nodes)
{
    *mode = pol ? (int32_t)(pol->mode | pol->flags) : MPOL_DEFAULT;
    *nodes = !pol ? 0 : pol->flags ? pol->user[0] : pol->nodes;
}

/**
 * The policy, written as an int to `policy` when it is not null, and the node
 * mask, to `nodemask` when it is not null, in Linux 6.12's order of checks:
 * a node mask with `maxnode` below the machine's one node, a flag Linux does
 * not define, MPOL_F_MEMS_ALLOWED with another flag, an address without
 * MPOL_F_ADDR, and MPOL_F_NODE without it unless the thread's policy
 * interleaves, are -EINVAL; an address in no region with MPOL_F_ADDR is
 * -EFAULT. MPOL_F_MEMS_ALLOWED gives 0 and the node mask of node 0. With
 * MPOL_F_ADDR the policy is the address's (`mempolicy_at`), the default
 * where mbind gave none, and without it the thread's. MPOL_F_NODE with
 * MPOL_F_ADDR gives in place of the mode the node of the page there, 0,
 * bringing the page in, and alone the node an interleaving thread
 * allocates from next, 0. An unwritable `policy` or `nodemask` is -EFAULT.
 */
long sys_get_mempolicy(isr_params_t *params)
{
    uintptr_t upolicy = SC_ARG(params, 0);
    uintptr_t umask = SC_ARG(params, 1);
    unsigned long maxnode = SC_ARG(params, 2);
    uintptr_t addr = SC_ARG(params, 3);
    unsigned long flags = SC_ARG(params, 4);
    const struct vm_policy *pol = current_proc->mempolicy;
    const vma_t *vma;
    unsigned long nodes = 1u;
    int32_t pval = 0;

    if (umask != 0 && maxnode < NR_NODE_IDS) {
        return -EINVAL;
    }
    if (flags & ~(unsigned long)(MPOL_F_NODE | MPOL_F_ADDR | MPOL_F_MEMS_ALLOWED)) {
        return -EINVAL;
    }
    if ((flags & MPOL_F_MEMS_ALLOWED) && (flags & (MPOL_F_NODE | MPOL_F_ADDR))) {
        return -EINVAL;
    }
    if (flags & MPOL_F_ADDR) {
        vma = current_proc->mm ? vm_find(&current_proc->mm->vm, addr) : NULL;
        if (!vma) {
            return -EFAULT;
        }
        pol = mempolicy_at(vma, addr);
    } else if (addr != 0 && !(flags & MPOL_F_MEMS_ALLOWED)) {
        return -EINVAL;
    }
    if (!(flags & MPOL_F_MEMS_ALLOWED)) {
        policy_report(pol, &pval, &nodes);
    }
    if ((flags & MPOL_F_NODE) && (flags & MPOL_F_ADDR)) {
        pval = mempolicy_page_node(addr);
        if (pval < 0) {
            return pval;
        }
    } else if (flags & MPOL_F_NODE) {
        if (!pol || (pol->mode != MPOL_INTERLEAVE && pol->mode != MPOL_WEIGHTED_INTERLEAVE)) {
            return -EINVAL;
        }
        pval = 0;
    }
    if (upolicy != 0 && copy_to_user((void __user *)upolicy, &pval, sizeof(pval)) != 0) {
        return -EFAULT;
    }
    return umask != 0 ? mempolicy_put_nodes(umask, maxnode, nodes) : 0;
}

/**
 * Whether the caller may inspect `t`, as Linux's ptrace_may_access with
 * PTRACE_MODE_READ_REALCREDS, which migrate_pages and move_pages ask of
 * another process: its own thread group, a privileged caller, or a caller
 * whose real user and group IDs are each of `t`'s three, with `t` dumpable
 * and not privileged.
 */
static int may_inspect(const process_t *t)
{
    if (t->tg == current_proc->tg || proc_privileged(current_proc)) {
        return 1;
    }
    return current_proc->ruid == t->ruid && current_proc->ruid == t->euid &&
           current_proc->ruid == t->suid && current_proc->rgid == t->rgid &&
           current_proc->rgid == t->egid && current_proc->rgid == t->sgid &&
           (!t->mm || t->mm->dumpable) && !proc_privileged(t);
}

/**
 * migrate_pages(pid, maxnode, old_nodes, new_nodes), in Linux 6.12's order
 * of checks: the two masks are read (`get_nodes`), a null one as the empty
 * mask; a `pid` that names no thread is -ESRCH, and one the caller may not
 * inspect -EPERM; new nodes other than node 0 are -EPERM without
 * privilege; no node 0 among them is -EINVAL, and so is a thread without
 * an address space. The pages to move are those on an old node that is not
 * node 0, and there are none, so the answer is then 0, the count of pages
 * that could not be moved.
 */
long sys_migrate_pages(isr_params_t *params)
{
    int pid = (int)SC_ARG(params, 0);
    unsigned long maxnode = SC_ARG(params, 1);
    unsigned long old[VM_NODES_WORDS], new[VM_NODES_WORDS];
    const process_t *t;
    int other, rc;
    uint32_t i;

    rc = get_nodes(old, SC_ARG(params, 2), maxnode);
    if (rc == 0) {
        rc = get_nodes(new, SC_ARG(params, 3), maxnode);
    }
    if (rc != 0) {
        return rc;
    }
    t = pid ? proc_named((uint32_t)pid) : current_proc;
    if (!t) {
        return -ESRCH;
    }
    if (!may_inspect(t)) {
        return -EPERM;
    }
    other = (new[0] & ~1ul) != 0;
    for (i = 1; i < VM_NODES_WORDS; i++) {
        other |= new[i] != 0;
    }
    if (other && !proc_privileged(current_proc)) {
        return -EPERM;
    }
    return (new[0] & 1ul) && t->mm ? 0 : -EINVAL;
}

/**
 * The status move_pages gives the page at `addr` of `mm`, as Linux's
 * folio_walk_start with FW_ZEROPAGE: node 0 for a resident RAM page;
 * -EFAULT for an address in no region or the zero page; and -ENOENT for
 * an absent page or device memory. A call that would move the page
 * (`move`) gets -EFAULT for the whole of a region of device memory,
 * which Linux cannot migrate.
 */
static int32_t page_status(struct mm *mm, uintptr_t addr, int move)
{
    const vma_t *vma = vm_find(&mm->vm, addr);

    if (!vma || (move && (vma->prot & VM_PROT_IO))) {
        return -EFAULT;
    }
    switch (paging_user_page(mm->dir, addr)) {
    case PAGING_PAGE_FRAME:
        return 0;
    case PAGING_PAGE_NONE:
        return -ENOENT;
    case PAGING_PAGE_DEVICE:
        return move ? -EFAULT : -ENOENT;
    default:
        return -EFAULT;
    }
}

/** Read entry `i` of move_pages' array of addresses at `upages`, of the
 * caller's pointers, 32-bit ones for an i386 caller on x86-64, into
 * `*addr`. 0 or -EFAULT. */
static int page_addr(uintptr_t upages, unsigned long i, uintptr_t *addr)
{
    uint32_t a32;

    if (!in_compat_syscall()) {
        return copy_from_user(addr, (const void __user *)(upages + i * sizeof(*addr)),
                              sizeof(*addr)) != 0
                   ? -EFAULT
                   : 0;
    }
    if (copy_from_user(&a32, (const void __user *)(upages + i * sizeof(a32)), sizeof(a32)) != 0) {
        return -EFAULT;
    }
    *addr = a32;
    return 0;
}

/**
 * move_pages with no nodes: write the status of each of `nr` pages of `mm`
 * (`page_status`) to `ustatus`, STAT_CHUNK at a time as Linux's
 * do_pages_stat does, so that the statuses of the chunks before an
 * unreadable address or an unwritable status are written. 0 or -EFAULT.
 */
static long pages_stat(struct mm *mm, unsigned long nr, uintptr_t upages, uintptr_t ustatus)
{
    int32_t status[STAT_CHUNK];
    unsigned long done, i, n;
    uintptr_t addr;

    for (done = 0; done < nr; done += n) {
        n = nr - done < STAT_CHUNK ? nr - done : STAT_CHUNK;
        for (i = 0; i < n; i++) {
            if (page_addr(upages, done + i, &addr) != 0) {
                return -EFAULT;
            }
            status[i] = page_status(mm, addr, 0);
        }
        if (copy_to_user((void __user *)(ustatus + done * sizeof(status[0])), status,
                         n * sizeof(status[0])) != 0) {
            return -EFAULT;
        }
    }
    return 0;
}

/**
 * move_pages with nodes: for each of `nr` pages of `mm` in turn, read its
 * address and its node, and write its status (`page_status`), as Linux's
 * do_pages_move does where every page is on the node asked for: an
 * unreadable address or node and an unwritable status are -EFAULT, and a
 * node other than node 0, which is no node with memory, is -ENODEV, each
 * with the statuses before it written. `user` holds the addresses of the
 * caller's arrays of addresses, nodes and statuses, in that order. 0 when
 * every page was passed.
 */
static long pages_move(struct mm *mm, unsigned long nr, const uintptr_t *user)
{
    unsigned long i;
    uintptr_t addr;
    int32_t node, status;

    for (i = 0; i < nr; i++) {
        if (page_addr(user[0], i, &addr) != 0 ||
            copy_from_user(&node, (const void __user *)(user[1] + i * sizeof(node)),
                           sizeof(node)) != 0) {
            return -EFAULT;
        }
        if (node != 0) {
            return -ENODEV;
        }
        status = page_status(mm, addr, 1);
        if (copy_to_user((void __user *)(user[2] + i * sizeof(status)), &status, sizeof(status)) !=
            0) {
            return -EFAULT;
        }
    }
    return 0;
}

/**
 * move_pages(pid, count, pages, nodes, status, flags), in Linux 6.12's
 * order of checks: a flag other than MPOL_MF_MOVE and MPOL_MF_MOVE_ALL is
 * -EINVAL, and MPOL_MF_MOVE_ALL without privilege -EPERM; a `pid` of 0 is
 * the caller, any other that names no thread is -ESRCH, one the caller may
 * not inspect -EPERM, and one without an address space -EINVAL. With
 * `nodes` each page is moved to its node (`pages_move`), and without it
 * each page's node is reported (`pages_stat`). The address space is held
 * across the copies, which may sleep.
 */
long sys_move_pages(isr_params_t *params)
{
    int pid = (int)SC_ARG(params, 0);
    unsigned int flags = (unsigned int)SC_ARG(params, 5);
    const process_t *t = current_proc;
    uintptr_t user[3];
    struct mm *mm;
    long rc;

    if (flags & ~(MPOL_MF_MOVE | MPOL_MF_MOVE_ALL)) {
        return -EINVAL;
    }
    if ((flags & MPOL_MF_MOVE_ALL) && !proc_privileged(current_proc)) {
        return -EPERM;
    }
    if (pid != 0) {
        t = proc_named((uint32_t)pid);
        if (!t) {
            return -ESRCH;
        }
        if (!may_inspect(t)) {
            return -EPERM;
        }
    }
    mm = t->mm;
    if (!mm) {
        return -EINVAL;
    }
    user[0] = SC_ARG(params, 2);
    user[1] = SC_ARG(params, 3);
    user[2] = SC_ARG(params, 4);
    mm_get(mm);
    rc = user[1] ? pages_move(mm, SC_ARG(params, 1), user)
                 : pages_stat(mm, SC_ARG(params, 1), user[0], user[2]);
    mm_put(mm);
    return rc;
}
