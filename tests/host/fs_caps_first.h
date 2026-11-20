/* Filesystem capabilities and ID maps in initial-namespace tests. */
#ifndef FS_CAPS_FIRST_H
#define FS_CAPS_FIRST_H

#include "proc/ns.h"

/** Word `i` of the capabilities `p` holds in the first user namespace:
 * those it has not lost for effective uid 0 or while its ambient set grants
 * them (`amb_granted`), none for another. */
uint32_t cap_word(const process_t *p, uint32_t i)
{
    uint32_t full = i == 0 ? 0xFFFFFFFFu : CAP_HIGH_WORD;

    return i < 2 && (p->euid == 0 || p->amb_granted) ? full & ~p->cap_lost[i] : 0u;
}

/** A word of the effective capabilities, as `cap_eff_word` gives it
 * in the first user namespace: the capabilities of CAP_FS_SET by the
 * file-system user id, the rest by the effective one. */
uint32_t cap_eff_word(const process_t *p, uint32_t i)
{
    uint32_t fs = i == 0 ? CAP_FS_WORD0 : CAP_FS_WORD1, held;

    if (i > 1) {
        return 0;
    }
    held = cap_word(p, i);
    if (p->fs_own & PROC_FSCAPS_OFF) {
        held &= ~fs;
    } else if (proc_fsuid(p) == 0) {
        held |= fs & ~p->cap_lost[i];
    } else if (p->euid == 0) {
        held &= ~fs;
    }
    return held;
}

/** Test one effective capability in the initial namespace. */
int cap_holds(const process_t *p, uint32_t cap)
{
    return cap <= CAP_LAST_CAP && (cap_eff_word(p, cap >> 5) & (1u << (cap & 31u)));
}

/** Whether the first user namespace maps the ids; it maps every one. */
int ns_ids_mapped(const struct userns *ns, uint32_t uid, uint32_t gid)
{
    (void)ns;
    (void)uid;
    (void)gid;
    return 1;
}

/** The kernel id `kid` itself, which the first user namespace maps. */
uint32_t ns_uid_from_kernel(const struct userns *ns, uint32_t kid)
{
    (void)ns;
    return kid;
}

/** The first user namespace maps every kernel user ID. */
int ns_uid_mapped(const struct userns *ns, uint32_t uid)
{
    (void)ns;
    (void)uid;
    return 1;
}

#endif
