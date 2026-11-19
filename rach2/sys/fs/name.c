/* A held name identifies one directory entry, not every link to an inode. */
#include "name.h"
#include "vfs.h"
#include "../lib/kerrno.h"
#include "../lib/kstring.h"
#include "../lib/spinlock.h"
#include "../mm/kheap.h"

struct vfs_name {
    struct vfs_name *next;
    struct vfs_ref parent;
    uint32_t refs;
    int deleted;
    char leaf[VFS_NAME_MAX + 1];
};

/*
 * The list's links and every record's `refs` are under `names_lock`, a
 * spinlock taken with interrupts off and held only while they change or
 * the list is searched, so that an open or the last close of an ext2 file
 * makes or ends its record without the big lock. A record's parent, leaf
 * and `deleted` change under the big lock alone, while the record is held,
 * and are read under it; the walks that change them hold the spinlock or a
 * reference to each record they reach.
 */
static struct vfs_name *names;
static struct spinlock names_lock = SPINLOCK_INIT_NAMED("names");

/* The changes of names under way (`vfs_name_change_begin`), and a count
 * that moves at each one's beginning and end, both under `names_lock`. */
static uint32_t names_changing;
static uint32_t names_seq;

/** The mutable base whose operations implement this mount's file system. */
static struct vfs_mount *name_base(struct vfs_mount *mnt)
{
    return mnt->bind_of ? mnt->bind_of : mnt;
}

/**
 * A record of `leaf` in `parent`, held once, on the list, and `deleted`
 * from the start when that is set: 0 with it in `*out`; -ENOMEM; or, with
 * `checked`, -EAGAIN when a change of names began since `seq` was read
 * (`vfs_name_seq`), with nothing kept. The check and the insert are one
 * step under the lock, so a change that begins later finds the record.
 */
static int name_new(const struct vfs_ref *parent, const char *leaf, int checked, uint32_t seq,
                    int deleted, struct vfs_name **out)
{
    struct vfs_name *name = kzalloc(sizeof(*name));
    uint32_t flags;
    int moved;

    *out = NULL;
    if (!name) {
        return -ENOMEM;
    }
    name->parent = *parent;
    vfs_ref_hold(parent);
    vfs_desc_open(parent->mnt);
    name->refs = 1;
    name->deleted = deleted;
    k_strncpy(name->leaf, leaf, sizeof(name->leaf));
    name->leaf[sizeof(name->leaf) - 1] = '\0';
    flags = spin_lock_irqsave(&names_lock);
    moved = checked && (names_changing != 0 || names_seq != seq);
    if (!moved) {
        name->next = names;
        names = name;
    }
    spin_unlock_irqrestore(&names_lock, flags);
    if (moved) {
        vfs_desc_close(name->parent.mnt, name->parent.node, NULL);
        kfree(name);
        return -EAGAIN;
    }
    *out = name;
    return 0;
}

int vfs_name_capture(const struct vfs_ref *parent, const char *leaf, struct vfs_name **out)
{
    *out = NULL;
    if (!parent->mnt->type->ops->hold || !parent->mnt->type->ops->put) {
        return 0;
    }
    return name_new(parent, leaf, 0, 0, 0, out);
}

int vfs_name_capture_at(const struct vfs_ref *parent, const char *leaf, uint32_t seq,
                        struct vfs_name **out)
{
    *out = NULL;
    if (!parent->mnt->type->ops->hold || !parent->mnt->type->ops->put) {
        return 0;
    }
    return name_new(parent, leaf, 1, seq, 0, out);
}

int vfs_name_capture_deleted(const struct vfs_ref *parent, const char *leaf, struct vfs_name **out)
{
    *out = NULL;
    if (!parent->mnt->type->ops->hold || !parent->mnt->type->ops->put) {
        return 0;
    }
    return name_new(parent, leaf, 0, 0, 1, out);
}

int vfs_name_seq(uint32_t *seq)
{
    uint32_t flags = spin_lock_irqsave(&names_lock);
    int calm = names_changing == 0;

    *seq = names_seq;
    spin_unlock_irqrestore(&names_lock, flags);
    return calm;
}

int vfs_name_seq_peek(uint32_t *seq)
{
    *seq = *(volatile const uint32_t *)&names_seq;
    k_barrier();
    return *(volatile const uint32_t *)&names_changing == 0;
}

int vfs_name_seq_still(uint32_t seq)
{
    uint32_t flags = spin_lock_irqsave(&names_lock);
    int still = names_changing == 0 && names_seq == seq;

    spin_unlock_irqrestore(&names_lock, flags);
    return still;
}

void vfs_name_change_begin(void)
{
    uint32_t flags = spin_lock_irqsave(&names_lock);

    names_changing++;
    names_seq++;
    spin_unlock_irqrestore(&names_lock, flags);
}

void vfs_name_change_end(void)
{
    uint32_t flags = spin_lock_irqsave(&names_lock);

    names_changing--;
    names_seq++;
    spin_unlock_irqrestore(&names_lock, flags);
}

struct vfs_name *vfs_name_ref(struct vfs_name *name)
{
    uint32_t flags;

    if (name) {
        flags = spin_lock_irqsave(&names_lock);
        name->refs++;
        spin_unlock_irqrestore(&names_lock, flags);
    }
    return name;
}

/**
 * The last put takes the record off the list under the spinlock and then
 * closes its parent, which may sleep, with the lock let go. A record of a
 * type that sets `reads_unlocked`, whose parent's `put` takes the big lock
 * where it needs it, may end without the big lock; any other needs it.
 */
void vfs_name_put(struct vfs_name *name)
{
    struct vfs_name **link = &names;
    uint32_t flags;

    if (!name) {
        return;
    }
    flags = spin_lock_irqsave(&names_lock);
    if (--name->refs != 0) {
        spin_unlock_irqrestore(&names_lock, flags);
        return;
    }
    while (*link && *link != name) {
        link = &(*link)->next;
    }
    if (*link) {
        *link = name->next;
    }
    spin_unlock_irqrestore(&names_lock, flags);
    vfs_desc_close(name->parent.mnt, name->parent.node, NULL);
    kfree(name);
}

int vfs_name_deleted(const struct vfs_name *name)
{
    return name->deleted;
}

int vfs_name_dir(struct vfs_name *name, const struct vfs_mount *base, uint64_t *dir_ino,
                 const char **leaf)
{
    struct vfs_stat st;

    if (vfs_mount_base(name->parent.mnt) != base ||
        vfs_getattr(name->parent.mnt, name->parent.node, &st) != 0) {
        return -1;
    }
    *dir_ino = st.ino;
    *leaf = name->leaf;
    return 0;
}

/** Whether two held nodes of the same file system are the same inode. */
static int same_node(struct vfs_mount *mnt, void *a, void *b)
{
    struct vfs_stat x, y;

    return a == b ||
           (vfs_getattr(mnt, a, &x) == 0 && vfs_getattr(mnt, b, &y) == 0 && x.ino == y.ino);
}

/** Verify the entry without accepting a different name of the same file. */
static int name_still_present(struct vfs_name *name, void *node)
{
    struct vfs_ref found;
    struct vfs_mount *mnt = name->parent.mnt;
    int rc;

    found.mnt = mnt;
    rc = mnt->type->ops->lookup(name_base(mnt), name->parent.node, name->leaf, &found.node);
    if (rc != 0) {
        return rc;
    }
    rc = same_node(mnt, node, found.node) ? 0 : -ENOENT;
    vfs_ref_put(&found);
    return rc;
}

int vfs_name_path(struct vfs_name *name, void *node, char *out, uint32_t cap, int *deleted)
{
    uint32_t n, leaf;
    int rc;

    *deleted = name->deleted;
    if (!name->deleted && (rc = name_still_present(name, node)) != 0) {
        return rc;
    }
    rc = vfs_node_path(name->parent.mnt, name->parent.node, out, cap);
    if (rc == -ENOENT && name->parent.mnt->bind_of) {
        rc = vfs_node_path(name_base(name->parent.mnt), name->parent.node, out, cap);
    }
    if (rc != 0) {
        return rc;
    }
    n = (uint32_t)k_strlen(out);
    leaf = (uint32_t)k_strlen(name->leaf);
    if (n && out[n - 1] == '/') {
        n--;
    }
    if (n + 1 + leaf + 1 > cap) {
        return -ENAMETOOLONG;
    }
    out[n++] = '/';
    k_memcpy(out + n, name->leaf, leaf + 1);
    return 0;
}

/** Exact leaf equality, including the terminating byte. */
static int same_leaf(const char *a, const char *b)
{
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

/** Match an entry through its underlying file system, including binds. */
static int names_entry(const struct vfs_name *name, struct vfs_mount *mnt, void *dir,
                       const char *leaf)
{
    return !name->deleted && vfs_mount_base(name->parent.mnt) == vfs_mount_base(mnt) &&
           name->parent.node == dir && same_leaf(name->leaf, leaf);
}

void vfs_name_unlink(struct vfs_mount *mnt, void *dir, const char *leaf)
{
    struct vfs_name *name;
    uint32_t flags = spin_lock_irqsave(&names_lock);

    for (name = names; name; name = name->next) {
        if (names_entry(name, mnt, dir, leaf)) {
            name->deleted = 1;
        }
    }
    spin_unlock_irqrestore(&names_lock, flags);
}

int vfs_name_same(struct vfs_mount *mnt, void *from, const char *old, void *to, const char *leaf)
{
    struct vfs_ref a, b;
    struct vfs_mount *base = name_base(mnt);
    struct vfs_name *name;
    uint32_t flags = spin_lock_irqsave(&names_lock);
    int same = 0;

    for (name = names; name; name = name->next) {
        if (names_entry(name, mnt, from, old) || names_entry(name, mnt, to, leaf)) {
            break;
        }
    }
    spin_unlock_irqrestore(&names_lock, flags);
    if (!name || base->type->ops->lookup(base, from, old, &a.node) != 0) {
        return 0;
    }
    a.mnt = b.mnt = mnt;
    if (base->type->ops->lookup(base, to, leaf, &b.node) == 0) {
        same = same_node(mnt, a.node, b.node);
        vfs_ref_put(&b);
    }
    vfs_ref_put(&a);
    return same;
}

/** The record after `name`, or the list's first for NULL, held; NULL at the
 * end. */
static struct vfs_name *name_next(struct vfs_name *name)
{
    uint32_t flags = spin_lock_irqsave(&names_lock);
    struct vfs_name *next = name ? name->next : names;

    if (next) {
        next->refs++;
    }
    spin_unlock_irqrestore(&names_lock, flags);
    return next;
}

void vfs_name_rename(struct vfs_mount *mnt, void *from, const char *old, void *to, const char *leaf)
{
    struct vfs_name *name = name_next(NULL), *next;

    while (name) {
        if (names_entry(name, mnt, from, old)) {
            struct vfs_ref parent = name->parent;

            name->parent.node = to;
            vfs_ref_hold(&name->parent);
            vfs_ref_put(&parent);
            k_strncpy(name->leaf, leaf, sizeof(name->leaf));
            name->leaf[sizeof(name->leaf) - 1] = '\0';
        } else if (names_entry(name, mnt, to, leaf)) {
            name->deleted = 1;
        }
        next = name_next(name);
        vfs_name_put(name);
        name = next;
    }
}

/** Give the record `name` the name `leaf` in the directory `to`, which it
 * holds in place of its old one. */
static void name_move(struct vfs_name *name, void *to, const char *leaf)
{
    struct vfs_ref parent = name->parent;

    name->parent.node = to;
    vfs_ref_hold(&name->parent);
    vfs_ref_put(&parent);
    k_strncpy(name->leaf, leaf, sizeof(name->leaf));
    name->leaf[sizeof(name->leaf) - 1] = '\0';
}

void vfs_name_exchange(struct vfs_mount *mnt, void *from, const char *old, void *to,
                       const char *leaf)
{
    struct vfs_name *name = name_next(NULL), *next;
    char was[VFS_NAME_MAX + 1];

    k_strncpy(was, old, sizeof(was));
    was[sizeof(was) - 1] = '\0';
    while (name) {
        if (names_entry(name, mnt, from, was)) {
            name_move(name, to, leaf);
        } else if (names_entry(name, mnt, to, leaf)) {
            name_move(name, from, was);
        }
        next = name_next(name);
        vfs_name_put(name);
        name = next;
    }
}

void vfs_name_repoint(struct vfs_mount *mnt, const void *from, void *to)
{
    struct vfs_name *name;
    uint32_t flags = spin_lock_irqsave(&names_lock);

    for (name = names; name; name = name->next) {
        if (name->parent.mnt == mnt && name->parent.node == from) {
            name->parent.node = to;
        }
    }
    spin_unlock_irqrestore(&names_lock, flags);
}
