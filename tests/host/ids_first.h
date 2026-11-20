/* The first user namespace, for host tests of code that maps the ids a
 * caller gives: every process is in it, and it maps every id to itself. */
#ifndef IDS_FIRST_H
#define IDS_FIRST_H

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

#endif
