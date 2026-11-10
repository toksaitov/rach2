/* The NUMA memory policy calls, on a machine of one memory node: a thread's
 * and a range's policy, and the calls that move pages between nodes. */
#ifndef _MEMPOLICY_H_
#define _MEMPOLICY_H_

#include "vm.h"
#include "../irq.h"

/* The modes of a policy, Linux's. MPOL_DEFAULT is the absence of one. */
#define MPOL_DEFAULT             0
#define MPOL_PREFERRED           1
#define MPOL_BIND                2
#define MPOL_INTERLEAVE          3
#define MPOL_LOCAL               4
#define MPOL_PREFERRED_MANY      5
#define MPOL_WEIGHTED_INTERLEAVE 6
#define MPOL_MAX                 7

/* The flags given in the mode argument beside a mode: the mask names the
 * nodes as they are, or relative to those the caller may use, and the
 * policy lets NUMA balancing move its pages. */
#define MPOL_F_STATIC_NODES   0x8000u
#define MPOL_F_RELATIVE_NODES 0x4000u
#define MPOL_F_NUMA_BALANCING 0x2000u
#define MPOL_MODE_FLAGS       (MPOL_F_STATIC_NODES | MPOL_F_RELATIVE_NODES | MPOL_F_NUMA_BALANCING)

/** get_mempolicy(mode, nodemask, maxnode, addr, flags); 0 or -errno. */
long sys_get_mempolicy(isr_params_t *params);

/** set_mempolicy(mode, nodemask, maxnode); 0 or -errno. */
long sys_set_mempolicy(isr_params_t *params);

/** mbind(start, len, mode, nodemask, maxnode, flags); 0 or -errno. */
long sys_mbind(isr_params_t *params);

/** set_mempolicy_home_node(start, len, home_node, flags); 0 or -errno. */
long sys_set_mempolicy_home_node(isr_params_t *params);

/** migrate_pages(pid, maxnode, old_nodes, new_nodes); the pages that could
 * not be moved, 0, or -errno. */
long sys_migrate_pages(isr_params_t *params);

/** move_pages(pid, count, pages, nodes, status, flags); 0 or -errno. */
long sys_move_pages(isr_params_t *params);

/**
 * The policy of the address `addr` in the region `vma`, as get_mempolicy
 * with MPOL_F_ADDR and /proc/PID/numa_maps report it, or NULL for the
 * default: the region's own, or, for a region that maps a file in memory
 * (a tmpfs file, a memfd, shared anonymous memory or a System V segment),
 * the policy mbind gave the file's page there through any mapping of it,
 * as Linux reads a shmem file's shared policy. The record is the region's
 * or the file's; it holds until the caller sleeps.
 */
const struct vm_policy *mempolicy_at(const vma_t *vma, uintptr_t addr);

#endif /* _MEMPOLICY_H_ */
