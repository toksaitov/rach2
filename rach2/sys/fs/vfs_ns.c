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
 *
 * A namespace may also hold flags of its own for a mount it shares with
 * others (`views` in vfs.h), keyed by its view, its inode number, which a
 * namespace made from it copies. They outlive an unmount in the namespace
 * and the namespace's end while a description opened through them is open,
 * since a call through a description judges by the view it was opened in
 * (`vfs_view_enter`).
 */

#include "vfs.h"
#include "fd.h"
#include "../lib/kdef.h"
#include "../lib/kerrno.h"
#include "../mm/kheap.h"
#include "../proc/ns.h"
#include "../proc/process.h"

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
    struct vfs_mount_view *v = m->views, *next;

    idset_free(&m->ns_seen);
    m->views = NULL;
    for (; v; v = next) {
        next = v->next;
        kfree(v);
    }
}

uint32_t vfs_mntns_view(void)
{
    if (current_proc && current_proc->mnt_view) {
        return current_proc->mnt_view;
    }
    return proc_mntns(current_proc)->c.inum;
}

uint32_t vfs_view_enter(uint32_t view)
{
    uint32_t prev;

    if (!current_proc) {
        return 0;
    }
    prev = current_proc->mnt_view;
    if (view) {
        current_proc->mnt_view = view;
    }
    return prev;
}

void vfs_view_leave(uint32_t prev)
{
    if (current_proc) {
        current_proc->mnt_view = prev;
    }
}

int vfs_view_fs(void)
{
    return current_proc && current_proc->mnt_view == VFS_VIEW_FS;
}

struct vfs_mount_view *vfs_mount_view_of(const struct vfs_mount *m, uint32_t view)
{
    struct vfs_mount_view *v;

    for (v = *(struct vfs_mount_view *const volatile *)&m->views; v; v = v->next) {
        if (*(volatile const uint32_t *)&v->ns == view) {
            return v;
        }
    }
    return NULL;
}

struct vfs_mount_view *vfs_mount_view_find(const struct vfs_mount *m)
{
    return m->views ? vfs_mount_view_of(m, vfs_mntns_view()) : NULL;
}

/**
 * `vfs_mount_view_make` for the view `view`. An entry whose namespace ended
 * is taken again only when no description holds it, since a description
 * keeps looking its view up for its life (`fd_mount_view_held`), and no new
 * description can name the view of a namespace that has ended.
 */
static struct vfs_mount_view *view_make(struct vfs_mount *m, uint32_t view, uint32_t flags)
{
    struct vfs_mount_view *v = vfs_mount_view_of(m, view);

    if (v) {
        return v;
    }
    for (v = m->views; v; v = v->next) {
        if (v->gone && !fd_mount_view_held(m, v->ns)) {
            v->flags = flags;
            v->gone = 0;
            k_barrier();
            v->ns = view;
            return v;
        }
    }
    v = (struct vfs_mount_view *)kzalloc(sizeof(*v));
    if (!v) {
        return NULL;
    }
    v->ns = view;
    v->flags = flags;
    v->next = m->views;
    k_barrier();
    m->views = v;
    return v;
}

struct vfs_mount_view *vfs_mount_view_make(struct vfs_mount *m, uint32_t flags)
{
    return view_make(m, vfs_mntns_view(), flags);
}

/** Mark the view `view`'s own flags for `m`, if it has any, as those of a
 * namespace that has ended. */
static void view_gone(struct vfs_mount *m, uint32_t view)
{
    struct vfs_mount_view *v = vfs_mount_view_of(m, view);

    if (v) {
        v->gone = 1;
    }
}

/**
 * The mounts are those of the table that are not free, the mounts being made
 * and unmounted among them, as a namespace made meanwhile must see what its
 * parent will see. `to` takes `from`'s own flags for each mount too, as
 * Linux's `copy_tree` copies each mount's flags. No call here sleeps, so the
 * table does not change under the loop. A failure takes `to` from every
 * mount, which the caller then frees.
 */
int vfs_mntns_copy(uint32_t from, uint32_t from_view, uint32_t to, uint32_t to_view)
{
    int i, n = vfs_mount_slots();

    for (i = 0; i < n; i++) {
        struct vfs_mount *m = vfs_mount_at(i);
        struct vfs_mount_view *v = m ? vfs_mount_view_of(m, from_view) : NULL;

        if (m && idset_has(&m->ns_seen, from) &&
            (idset_add(&m->ns_seen, to) != 0 || (v && !view_make(m, to_view, v->flags)))) {
            for (i = 0; i < n; i++) {
                m = vfs_mount_at(i);
                if (m) {
                    idset_del(&m->ns_seen, to);
                    view_gone(m, to_view);
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
void vfs_mntns_gone(uint32_t id, uint32_t view)
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
            view_gone(m, view);
        }
    }
    vfs_reap_detached();
}
