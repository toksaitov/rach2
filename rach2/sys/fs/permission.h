/* File access and ownership checks in the caller's user namespace. */
#ifndef _FS_PERMISSION_H_
#define _FS_PERMISSION_H_

#include "../proc/ns.h"

/** Whether the caller holds cap over an inode with these kernel IDs. */
static __inline__ int inode_capable(uint32_t uid, uint32_t gid, uint32_t cap)
{
    process_t *p = current_proc;

    return cap_holds(p, cap) && (!p->userns || ns_ids_mapped(p->userns, uid, gid));
}

/** Linux's inode_owner_or_capable needs only the inode's user ID mapped. */
static __inline__ int inode_owner_or_capable(uint32_t uid)
{
    process_t *p = current_proc;

    return uid == proc_fsuid(p) ||
           (cap_holds(p, CAP_FOWNER) && (!p->userns || ns_uid_mapped(p->userns, uid)));
}

/** Read one capability word using the credentials that govern this walk. */
static __inline__ uint32_t fs_access_caps(const process_t *p, uint32_t word, int real)
{
    if (!real) {
        return cap_eff_word(p, word);
    }
    if ((p->userns ? ns_uid_from_kernel(p->userns, p->ruid) : p->ruid) != 0) {
        return 0;
    }
    return cap_word(p, word);
}

/** Whether Linux's two DAC capabilities grant the `want` bits of an inode
 * the mode or its ACL refused, as `generic_permission` asks after
 * `acl_permission_check`: only for an inode whose ids the caller's user
 * namespace maps. Mode includes the file type. */
static __inline__ int inode_dac_override(uint32_t owner, uint32_t group, uint32_t mode,
                                         uint32_t want, int real)
{
    process_t *p = current_proc;
    uint32_t caps;
    int dir = (mode & 0170000u) == 0040000u;

    if (p->userns && !ns_ids_mapped(p->userns, owner, group)) {
        return 0;
    }
    caps = fs_access_caps(p, 0, real);
    if (((dir && !(want & 2u)) || want == 4u) && (caps & (1u << CAP_DAC_READ_SEARCH))) {
        return 1;
    }
    return (dir || !(want & 1u) || (mode & 0111u)) && (caps & (1u << CAP_DAC_OVERRIDE));
}

/** Whether the caller's file-system group, or its real group for `real`,
 * or one of its supplementary groups is `group`, Linux's `in_group_p`. */
static __inline__ int inode_in_group(uint32_t group, int real)
{
    process_t *p = current_proc;
    uint32_t i, gid = real ? p->rgid : proc_fsgid(p);

    for (i = 0; gid != group && i < p->ngroups; i++) {
        gid = p->groups[i];
    }
    return gid == group;
}

/** Apply the applicable mode class and then Linux's two DAC capabilities.
 * Mode includes the file type. Want holds the read, write and execute bits. */
static __inline__ int inode_may_access(uint32_t owner, uint32_t group, uint32_t mode, uint32_t want,
                                       int real)
{
    process_t *p = current_proc;
    uint32_t uid = real ? p->ruid : proc_fsuid(p);
    uint32_t bits = mode;

    if (uid == owner) {
        bits >>= 6;
    } else if (inode_in_group(group, real)) {
        bits >>= 3;
    }
    if (!(want & ~bits & 7u)) {
        return 1;
    }
    return inode_dac_override(owner, group, mode, want, real);
}

#endif
