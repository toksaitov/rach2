/*
 * vfs_ns.c: the mount namespaces' views of the mount table, the ids of the
 * live namespaces that see each mount (`ns_seen` in vfs.h).
 *
 * Each mount namespace has an id, the lowest free when it was made (proc/
 * ns.h), and each mount the set of the ids that see it, which grows with the
 * highest of them, so that the namespaces are bounded by memory, as on
 * Linux, and by the users' max_mnt_namespaces. A set holds only live ids:
 * the end of a namespace takes its id from every mount before the id can be
 * given again. A mount no id sees is let go of once nothing holds it
 * (`vfs_reap_detached` in vfs.c).
 */

#include "vfs.h"
#include "../lib/kerrno.h"
#include "../proc/ns.h"
#include "../proc/process.h"
#include "../proc/wait_queue.h"

/* The remount under way, 1 from its first check to its store of the
 * flags (`vfs_mntns_hold`), 0 when none is; where another remount and the
 * making of a namespace wait for it to end. Under the big lock; a zeroed
 * queue needs no init. */
static uint32_t remounts;
static wait_queue_t remounts_wq;

/** The id of the caller's mount namespace. */
static uint32_t caller_id(void)
{
    return proc_mntns(current_proc)->id;
}

int vfs_mount_seen(const struct vfs_mount *m)
{
    return idset_has(&m->ns_seen, caller_id());
}

int vfs_mount_seen_word(const struct vfs_mount *m)
{
    uint32_t id = caller_id();

    if (id >= 32u) {
        return -1;
    }
    return (int)((*(volatile const uint32_t *)&m->ns_seen.low >> id) & 1u);
}

int vfs_mount_shared(const struct vfs_mount *m)
{
    return m->ns_seen.count > (uint32_t)vfs_mount_seen(m);
}

int vfs_mount_unseen(const struct vfs_mount *m)
{
    return m->ns_seen.count == 0;
}

void vfs_mount_hide(struct vfs_mount *m)
{
    idset_del(&m->ns_seen, caller_id());
}

int vfs_mount_view_new(struct vfs_mount *m)
{
    return idset_add(&m->ns_seen, caller_id());
}

void vfs_mount_view_free(struct vfs_mount *m)
{
    idset_free(&m->ns_seen);
}

/** The wait condition of a remount and of a namespace's making: no
 * remount is under way. */
static int no_remounts(void *arg)
{
    (void)arg;
    return remounts == 0;
}

void vfs_mntns_wait(void)
{
    if (current_proc) {
        wq_wait_event_nointr(&remounts_wq, no_remounts, NULL);
    }
}

void vfs_mntns_hold(void)
{
    vfs_mntns_wait();
    remounts++;
}

void vfs_mntns_release(void)
{
    if (--remounts == 0) {
        wq_wake_all(&remounts_wq);
    }
}

/**
 * The mounts are those of the table that are not free, the mounts being made
 * and unmounted among them, as a namespace made meanwhile must see what its
 * parent will see. The caller has waited out the remount under way
 * (`vfs_mntns_wait`) and slept nowhere since, and no call here sleeps, so no
 * remount sits between its check and its store and the table does not
 * change under the loop. A failure takes `to` from every mount, which the
 * caller then frees.
 */
int vfs_mntns_copy(uint32_t from, uint32_t to)
{
    int i, n = vfs_mount_slots();

    for (i = 0; i < n; i++) {
        struct vfs_mount *m = vfs_mount_at(i);

        if (m && idset_has(&m->ns_seen, from) && idset_add(&m->ns_seen, to) != 0) {
            for (i = 0; i < n; i++) {
                m = vfs_mount_at(i);
                if (m) {
                    idset_del(&m->ns_seen, to);
                }
            }
            return -ENOMEM;
        }
    }
    return 0;
}

/**
 * The end of the mount namespace `id`: each active mount past the permanent
 * ones that it alone sees is unmounted, a child before its parent, and the
 * id is taken from every mount, which a namespace made later takes afresh.
 * A mount still busy stays, seen by none, until the reaper finds it free
 * (`vfs_reap_detached`). The id stays live until this returns, so no
 * namespace made meanwhile takes it, and each mount's view is read again as
 * the loop reaches it, since an unmount may sleep and the table change.
 */
void vfs_mntns_gone(uint32_t id)
{
    int i, progress = 1;

    while (progress) {
        progress = 0;
        for (i = VFS_PERMANENT; i < vfs_mount_slots(); i++) {
            struct vfs_mount *m = vfs_mount_at(i);

            if (m && m->state == VFS_ACTIVE && idset_has(&m->ns_seen, id) &&
                m->ns_seen.count == 1 && vfs_unmount_record(m) == 0) {
                progress = 1;
            }
        }
    }
    for (i = 0; i < vfs_mount_slots(); i++) {
        struct vfs_mount *m = vfs_mount_at(i);

        if (m) {
            idset_del(&m->ns_seen, id);
        }
    }
    vfs_reap_detached();
}
