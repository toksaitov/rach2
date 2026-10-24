/* A held name identifies one directory entry, not every link to an inode. */
#include "name.h"
#include "vfs.h"
#include "../lib/kerrno.h"
#include "../lib/kstring.h"
#include "../mm/kheap.h"

struct vfs_name {
    struct vfs_name *next;
    struct vfs_ref parent;
    uint32_t refs;
    int deleted;
    char leaf[VFS_NAME_MAX + 1];
};

static struct vfs_name *names;

/** The mutable base whose operations implement this mount's file system. */
static struct vfs_mount *name_base(struct vfs_mount *mnt)
{
    return mnt->bind_of ? mnt->bind_of : mnt;
}

static struct vfs_name *name_new(const struct vfs_ref *parent, const char *leaf)
{
    struct vfs_name *name = kzalloc(sizeof(*name));

    if (name) {
        name->parent = *parent;
        vfs_ref_hold(parent);
        vfs_desc_open(parent->mnt);
        name->refs = 1;
        k_strncpy(name->leaf, leaf, sizeof(name->leaf));
        name->leaf[sizeof(name->leaf) - 1] = '\0';
        name->next = names;
        names = name;
    }
    return name;
}

int vfs_name_capture(const struct vfs_ref *parent, const char *leaf, struct vfs_name **out)
{
    *out = NULL;
    if (!parent->mnt->type->ops->hold || !parent->mnt->type->ops->put) {
        return 0;
    }
    *out = name_new(parent, leaf);
    return *out ? 0 : -ENOMEM;
}

struct vfs_name *vfs_name_ref(struct vfs_name *name)
{
    if (name) {
        name->refs++;
    }
    return name;
}

void vfs_name_put(struct vfs_name *name)
{
    struct vfs_name **link = &names;

    if (!name || --name->refs != 0) {
        return;
    }
    while (*link && *link != name) {
        link = &(*link)->next;
    }
    if (*link) {
        *link = name->next;
    }
    vfs_desc_close(name->parent.mnt, name->parent.node, NULL);
    kfree(name);
}

int vfs_name_deleted(const struct vfs_name *name)
{
    return name->deleted;
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

    for (name = names; name; name = name->next) {
        if (names_entry(name, mnt, dir, leaf)) {
            name->deleted = 1;
        }
    }
}

int vfs_name_same(struct vfs_mount *mnt, void *from, const char *old, void *to, const char *leaf)
{
    struct vfs_ref a, b;
    struct vfs_mount *base = name_base(mnt);
    struct vfs_name *name;
    int same = 0;

    for (name = names; name; name = name->next) {
        if (names_entry(name, mnt, from, old) || names_entry(name, mnt, to, leaf)) {
            break;
        }
    }
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

void vfs_name_rename(struct vfs_mount *mnt, void *from, const char *old, void *to, const char *leaf)
{
    struct vfs_name *name = vfs_name_ref(names), *next;

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
        next = vfs_name_ref(name->next);
        vfs_name_put(name);
        name = next;
    }
}

void vfs_name_repoint(struct vfs_mount *mnt, const void *from, void *to)
{
    struct vfs_name *name;

    for (name = names; name; name = name->next) {
        if (name->parent.mnt == mnt && name->parent.node == from) {
            name->parent.node = to;
        }
    }
}
