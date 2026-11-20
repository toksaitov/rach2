/* The first user namespace and its capabilities, for the ext2 tests: the
 * mount options' ids and the reserved blocks' CAP_SYS_RESOURCE. */
#ifndef EXT2_IDS_FIRST_H
#define EXT2_IDS_FIRST_H

#include "../../rach2/sys/proc/ns.h"

/** Every process is in the first user namespace, which the stubs below
 * take as NULL. */
struct userns *proc_userns(const process_t *p)
{
    (void)p;
    return NULL;
}

/** The first user namespace maps every id to itself. */
int64_t ns_uid_to_kernel(const struct userns *ns, uint32_t id)
{
    (void)ns;
    return id;
}

/** The first user namespace maps every group id to itself. */
int64_t ns_gid_to_kernel(const struct userns *ns, uint32_t id)
{
    (void)ns;
    return id;
}

/** A capability is held with effective uid 0 unless `cap_lost` has it. */
int cap_holds(const process_t *p, uint32_t cap)
{
    return p->euid == 0 && cap < 32 && !(p->cap_lost[0] & (1u << cap));
}

#endif
