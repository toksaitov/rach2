/* Capabilities and user namespaces for a host test whose processes are in
 * the first user namespace: root holds every capability (proc/ns.h). */
#include "../../../rach2/sys/proc/ns.h"

/** Every capability for an effective user ID of 0, none otherwise. */
int cap_holds(const process_t *p, uint32_t cap)
{
    (void)cap;
    return p->euid == 0;
}

/** The first namespace maps every id. */
int ns_uid_mapped(const struct userns *ns, uint32_t uid)
{
    (void)ns;
    (void)uid;
    return 1;
}

/** The first namespace maps every id. */
int ns_ids_mapped(const struct userns *ns, uint32_t uid, uint32_t gid)
{
    (void)ns;
    (void)uid;
    (void)gid;
    return 1;
}

/** The first namespace's ids are the kernel's. */
int64_t ns_uid_to_kernel(const struct userns *ns, uint32_t id)
{
    (void)ns;
    return id;
}

/** The first namespace's ids are the kernel's. */
int64_t ns_gid_to_kernel(const struct userns *ns, uint32_t id)
{
    (void)ns;
    return id;
}

/** The first namespace's ids are the kernel's. */
uint32_t ns_uid_from_kernel(const struct userns *ns, uint32_t kid)
{
    (void)ns;
    return kid;
}

/** The first namespace's ids are the kernel's. */
uint32_t ns_gid_from_kernel(const struct userns *ns, uint32_t kid)
{
    (void)ns;
    return kid;
}
