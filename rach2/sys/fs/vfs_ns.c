/*
 * vfs_ns.c: the mount namespaces' trees in the one mount table, the ids of
 * the live namespaces that see each mount (`ns_seen` in vfs.h), and the copy
 * of a tree that a new namespace takes.
 *
 * Each mount namespace has an id, the lowest free when it was made (proc/
 * ns.h), and each mount the set of the ids that see it, which grows with the
 * highest of them, so that the namespaces are bounded by memory, as on
 * Linux, and by the users' max_mnt_namespaces. A mount is seen by the one
 * namespace it was made or copied in, or by none once hidden; the memfd
 * mount, which no walk reaches, by all. The end of a namespace takes its id
 * from every mount before the id can be given again. A mount no id sees is
 * let go of once nothing holds it (`vfs_reap_detached` in vfs.c).
 */

#include "vfs.h"
#include "../lib/kerrno.h"
#include "../proc/ns.h"
#include "../proc/process.h"
#include "../proc/wait_queue.h"
#include "../mm/kheap.h"
#include "pathbuf.h"
#include "walkseq.h"

/* The remount under way, 1 from its first check to its store of the
 * flags (`vfs_mntns_hold`), 0 when none is; where another remount waits for
 * it to end. Under the big lock; a zeroed queue needs no init. */
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

int vfs_mount_unseen(const struct vfs_mount *m)
{
    return m->ns_seen.count == 0;
}

void vfs_mount_hide(struct vfs_mount *m)
{
    idset_del(&m->ns_seen, caller_id());
    vfs_mounts_changed();
}

int vfs_mount_view_new(struct vfs_mount *m)
{
    vfs_mounts_changed();
    return idset_add(&m->ns_seen, caller_id());
}

void vfs_mount_view_free(struct vfs_mount *m)
{
    idset_free(&m->ns_seen);
}

/** The wait condition of a remount: no other remount is under way. */
static int no_remounts(void *arg)
{
    (void)arg;
    return remounts == 0;
}

/** Wait until no remount is under way; a caller with no process, as at
 * boot, does not wait. */
static void vfs_mntns_wait(void)
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

int vfs_mntns_hold_unprivileged(const struct vfs_mount *m, uint32_t flags)
{
    vfs_mntns_hold();
    if (vfs_mount_flags(m) & VFS_MS_LOCKED & ~flags) {
        vfs_mntns_release();
        return -EPERM;
    }
    return 0;
}

void vfs_mntns_release(void)
{
    if (--remounts == 0) {
        wq_wake_all(&remounts_wq);
    }
}

/* What `vfs_mntns_copy` knows of a mount's parent (`parent_at`). */
#define COPY_UNDER_ITS_OWN (-1) /* none, or one the namespace copies not */
#define COPY_LEFT_OUT      (-2) /* one seen but not copied: left out too */

/**
 * The parent of `m`, of the `n` mounts of `orig`, for the copy: its index
 * there when it is copied too, COPY_UNDER_ITS_OWN when `m` has none or one
 * the namespace `from` does not see, under which the copy goes as it is,
 * and COPY_LEFT_OUT for one `from` sees that is not copied, being made or
 * unmounted, under which nothing is copied.
 */
static int parent_at(const struct vfs_mount *m, struct vfs_mount *const *orig, int n, uint32_t from)
{
    int j;

    if (!m->parent) {
        return COPY_UNDER_ITS_OWN;
    }
    for (j = 0; j < n; j++) {
        if (orig[j] == m->parent) {
            return j;
        }
    }
    return idset_has(&m->parent->ns_seen, from) ? COPY_LEFT_OUT : COPY_UNDER_ITS_OWN;
}

/**
 * A mount is copied once the mount it is on has been, and of those ready
 * the one made first goes first, so that the copies are made, and listed
 * by /proc/PID/mountinfo, in their originals' order, and the root, which a
 * new root of the boot (`vfs_root_over_image`) gives a later `seq` than the
 * mounts on it, before them. A mount being made or unmounted is not
 * copied, nor anything on it, as Linux's copy under `namespace_sem` finds
 * it either before or after. The copies are made from records the big
 * lock keeps as they are, since nothing here sleeps. A failure takes back
 * what was made (`vfs_mntns_gone`), which may sleep.
 */
int vfs_mntns_copy(uint32_t from, uint32_t to)
{
    int i, n = vfs_mount_slots();
    struct vfs_mount **orig, **made;
    int *up, rc = 0;

    orig = kzalloc((uint32_t)n * (2u * sizeof(*orig) + sizeof(*up)));
    if (!orig) {
        return -ENOMEM;
    }
    made = orig + n;
    up = (int *)(void *)(made + n);
    for (i = 0; i < n; i++) {
        struct vfs_mount *m = vfs_mount_at(i);

        if (!m || !idset_has(&m->ns_seen, from)) {
            continue;
        }
        if (m == vfs_memfd_mount()) {
            rc = idset_add(&m->ns_seen, to);
        } else if (m->state == VFS_ACTIVE) {
            orig[i] = m;
        }
    }
    for (i = 0; i < n; i++) {
        up[i] = orig[i] ? parent_at(orig[i], orig, n, from) : COPY_LEFT_OUT;
    }
    while (rc == 0) {
        int best = -1;

        for (i = 0; i < n; i++) {
            if (orig[i] && !made[i] && up[i] != COPY_LEFT_OUT && (up[i] < 0 || made[up[i]]) &&
                (best < 0 || orig[i]->seq < orig[best]->seq)) {
                best = i;
            }
        }
        if (best < 0) {
            break;
        }
        made[best] =
            vfs_mount_copy(orig[best], up[best] < 0 ? orig[best]->parent : made[up[best]], to);
        rc = made[best] ? 0 : -ENOMEM;
    }
    kfree(orig);
    if (rc != 0) {
        vfs_mntns_gone(to);
    }
    return rc;
}

/** The mount of the namespace of id `id` that stands for `m`: `m` when the
 * namespace sees it, the copy it made of `m`, or NULL. A record freed and
 * made again at the address of `m` while a copy of the old one lives is no
 * concern: `m` is held by the place that asks. */
static struct vfs_mount *copy_in(struct vfs_mount *m, uint32_t id)
{
    int i;

    if (idset_has(&m->ns_seen, id)) {
        return m;
    }
    for (i = 0; i < vfs_mount_slots(); i++) {
        struct vfs_mount *c = vfs_mount_at(i);

        if (c && c->copy_of == m && c->state == VFS_ACTIVE && idset_has(&c->ns_seen, id)) {
            return c;
        }
    }
    return NULL;
}

/**
 * A root of NULL is the root of the mount table, entry zero, which a new
 * namespace sees through its copy. A working directory of NULL, in /dev
 * or /proc or none, is kept by its path alone, which the new namespace's
 * walk reads the same way.
 */
void vfs_mntns_moved(process_t *p)
{
    uint32_t id = proc_mntns(p)->id;
    struct vfs_mount *rmnt = p->fs->root_mnt ? p->fs->root_mnt : vfs_root_mount();
    void *rnode = p->fs->root_mnt ? p->fs->root_node : rmnt->root;
    struct vfs_mount *to = copy_in(rmnt, id);

    vfs_seq_begin();
    if (to && to != rmnt) {
        vfs_root_set(p, to, rnode);
    }
    if (p->fs->cwd_mnt && (to = copy_in(p->fs->cwd_mnt, id)) != NULL && to != p->fs->cwd_mnt) {
        vfs_cwd_set(p, to, p->fs->cwd_node);
    }
    vfs_seq_end();
}

/**
 * The root is the namespace's own, whatever root its processes have, as
 * Linux starts from `mnt_ns->root` (`mntns_install`, fs/namespace.c in
 * 6.12): the mount pivot_root made its root (`ns_root`), else the mount it
 * sees that has no parent, the root of the mount table or the copy the
 * namespace made of it; the memfd mount, which every namespace sees and no
 * walk reaches, is neither. A mount stacked on that root is crossed, the
 * one placed last at the end, as the walk crosses them.
 */
void vfs_mntns_joined(process_t *p)
{
    uint32_t id = proc_mntns(p)->id;
    struct vfs_mount *top = NULL, *over;
    int i;

    vfs_mounts_changed();
    for (i = 0; i < vfs_mount_slots(); i++) {
        struct vfs_mount *m = vfs_mount_at(i);

        if (m && m->state == VFS_ACTIVE && m != vfs_memfd_mount() && idset_has(&m->ns_seen, id) &&
            (m->ns_root || (!m->parent && !(top && top->ns_root)))) {
            top = m;
        }
    }
    if (!top) {
        return;
    }
    do {
        over = NULL;
        for (i = 0; i < vfs_mount_slots(); i++) {
            struct vfs_mount *m = vfs_mount_at(i);

            if (m && m->state == VFS_ACTIVE && m->parent == top && m->covered &&
                m->covered == top->root && idset_has(&m->ns_seen, id)) {
                over = m;
            }
        }
        top = over ? over : top;
    } while (over);
    vfs_seq_begin();
    vfs_root_set(p, top, top->root);
    vfs_cwd_set(p, top, top->root);
    (void)proc_cwd_set(p, "/");
    vfs_seq_end();
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
