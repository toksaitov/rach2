/*
 * mempolicy.c: get_mempolicy(2) on a machine of one memory node, answered as
 * Linux 6.12's mm/mempolicy.c answers it on such a machine for a thread that
 * set no policy: the default policy, an empty node mask for it, node 0 for
 * every page, and node 0 alone allowed. libnuma asks it whether NUMA is
 * there at all.
 */

#include "mempolicy.h"

#include "mm.h"
#include "paging.h"
#include "usercopy.h"
#include "vm.h"
#include "../proc/percpu.h"
#include "../proc/process.h"
#include "../arch/arch.h"
#include "../lib/kerrno.h"
#include "../syscall.h"

/* The flags of get_mempolicy: the node of a page or of the next interleave,
 * the policy of an address, and the nodes the thread may use. */
#define MPOL_F_NODE         1u
#define MPOL_F_ADDR         2u
#define MPOL_F_MEMS_ALLOWED 4u

/* The policy of a thread that set none. */
#define MPOL_DEFAULT 0

/* The nodes the machine has, Linux's nr_node_ids. */
#define NR_NODE_IDS 1u

/**
 * The node of the page at `addr` of the current process, brought in for a
 * read as Linux's get_user_pages does, which is node 0, or -EFAULT for a
 * page that cannot be read: one of a PROT_NONE region, a guard or device
 * memory.
 */
static int32_t mempolicy_page_node(uintptr_t addr)
{
    const vma_t *vma = vm_find(&current_proc->mm->vm, addr);

    if (!vma || (vma->prot & VM_PROT_IO) ||
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
 * The policy, written as an int to `policy` when it is not null, and the node
 * mask, to `nodemask` when it is not null, in Linux 6.12's order of checks:
 * a node mask with `maxnode` below the machine's one node, a flag Linux does
 * not define, MPOL_F_MEMS_ALLOWED with another flag, an address without
 * MPOL_F_ADDR, and MPOL_F_NODE without it, are -EINVAL; an address in no
 * region with MPOL_F_ADDR is -EFAULT. MPOL_F_MEMS_ALLOWED gives 0 and the
 * node mask of node 0; MPOL_F_NODE with MPOL_F_ADDR gives the node of the
 * page there, 0, bringing the page in; the rest give MPOL_DEFAULT and an
 * empty mask, the default policy's, for the thread and for every address,
 * since no call here sets another. An unwritable `policy` or `nodemask` is
 * -EFAULT.
 */
long sys_get_mempolicy(isr_params_t *params)
{
    uintptr_t upolicy = SC_ARG(params, 0);
    uintptr_t umask = SC_ARG(params, 1);
    unsigned long maxnode = SC_ARG(params, 2);
    uintptr_t addr = SC_ARG(params, 3);
    unsigned long flags = SC_ARG(params, 4);
    unsigned long nodes = 0;
    int32_t pval = MPOL_DEFAULT;

    if (umask != 0 && maxnode < NR_NODE_IDS) {
        return -EINVAL;
    }
    if (flags & ~(unsigned long)(MPOL_F_NODE | MPOL_F_ADDR | MPOL_F_MEMS_ALLOWED)) {
        return -EINVAL;
    }
    if (flags & MPOL_F_MEMS_ALLOWED) {
        if (flags & (MPOL_F_NODE | MPOL_F_ADDR)) {
            return -EINVAL;
        }
        nodes = 1u;
    } else if (flags & MPOL_F_ADDR) {
        if (!current_proc->mm || !vm_find(&current_proc->mm->vm, addr)) {
            return -EFAULT;
        }
        if (flags & MPOL_F_NODE) {
            pval = mempolicy_page_node(addr);
            if (pval < 0) {
                return pval;
            }
        }
    } else if (addr != 0 || (flags & MPOL_F_NODE)) {
        return -EINVAL;
    }
    if (upolicy != 0 && copy_to_user((void __user *)upolicy, &pval, sizeof(pval)) != 0) {
        return -EFAULT;
    }
    return umask != 0 ? mempolicy_put_nodes(umask, maxnode, nodes) : 0;
}
