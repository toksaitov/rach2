/*
 * tmpfs.c: a file system in memory, as tmpfs.h describes it.
 */

#include "tmpfs.h"

#include "../isr.h"
#include "fd.h"
#include "krffs.h"
#include "../mm/kheap.h"
#include "../mm/paging.h"
#include "../mm/shmem.h"
#include "../proc/process.h"
#include "../lib/kerrno.h"
#include "../lib/kstring.h"
#include "../lib/kuser.h"
#include "../syscall_internal.h"

#define T_IFMT   0170000u
#define T_IFSOCK 0140000u
#define T_IFLNK  0120000u
#define T_IFREG  0100000u
#define T_IFBLK  0060000u
#define T_IFDIR  0040000u
#define T_IFCHR  0020000u
#define T_IFIFO  0010000u

#define TMPFS_MAGIC 0x01021994u

/* Format: the size Linux's tmpfs gives each name in a directory's size. */
#define DIRENT_BYTES 20u

struct tmpfs_node;

struct tmpfs_dirent {
    struct tmpfs_dirent *next;
    struct tmpfs_node *node;
    char *name;
};

/*
 * A node. `refs` counts the holds of lookups, descriptions, working
 * directories and mounts; `links` the names that name it. It is freed when
 * both are gone.
 */
struct tmpfs_node {
    struct tmpfs_node *parent; /* a directory's own; the root's is itself */
    struct tmpfs_dirent *entries;
    uint32_t nentries;
    uint32_t subdirs;
    uint32_t refs;
    uint32_t links;
    uint32_t mode;
    uint32_t uid;
    uint32_t gid;
    uint32_t rdev; /* a device node's number, in the kernel's encoding */
    uint64_t ino;
    int64_t atime, atime_ns, mtime, mtime_ns, ctime, ctime_ns;
    struct shmem data;
    char *target;
};

struct tmpfs_mount {
    struct tmpfs_node *root;
    uint64_t next_ino;
};

#define STAMP_A 1
#define STAMP_M 2
#define STAMP_C 4

static void stamp(struct tmpfs_node *n, int which)
{
    int64_t s, ns;

    get_current_time(&s, &ns);
    if (which & STAMP_A) {
        n->atime = s;
        n->atime_ns = ns;
    }
    if (which & STAMP_M) {
        n->mtime = s;
        n->mtime_ns = ns;
    }
    if (which & STAMP_C) {
        n->ctime = s;
        n->ctime_ns = ns;
    }
}

static int is_dir(const struct tmpfs_node *n)
{
    return (n->mode & T_IFMT) == T_IFDIR;
}

static struct tmpfs_node *new_node(struct vfs_mount *mnt, uint32_t mode)
{
    struct tmpfs_mount *tm = (struct tmpfs_mount *)mnt->priv;
    struct tmpfs_node *n = kzalloc(sizeof(*n));

    if (!n) {
        return NULL;
    }
    n->mode = mode;
    n->uid = current_proc ? current_proc->euid : 0u;
    n->gid = current_proc ? current_proc->egid : 0u;
    n->ino = ++tm->next_ino;
    stamp(n, STAMP_A | STAMP_M | STAMP_C);
    return n;
}

/* Free a node that nothing holds and no name names. */
static void settle(struct tmpfs_node *n)
{
    if (n->refs == 0 && n->links == 0) {
        shmem_free(&n->data);
        kfree(n->target);
        kfree(n);
    }
}

static struct tmpfs_dirent **find(struct tmpfs_node *d, const char *name)
{
    struct tmpfs_dirent **link;

    for (link = &d->entries; *link; link = &(*link)->next) {
        const char *a = (*link)->name;
        const char *b = name;

        while (*a && *a == *b) {
            a++;
            b++;
        }
        if (*a == *b) {
            return link;
        }
    }
    return NULL;
}

/* Add the name `name` for `n` at the end of `d`, so a directory lists its
 * names in the order they were made. */
static int add_entry(struct tmpfs_node *d, const char *name, struct tmpfs_node *n)
{
    struct tmpfs_dirent **link = &d->entries;
    struct tmpfs_dirent *e = kzalloc(sizeof(*e));
    uint32_t len = (uint32_t)k_strlen(name);

    if (!e) {
        return -ENOSPC;
    }
    e->name = kmalloc(len + 1u);
    if (!e->name) {
        kfree(e);
        return -ENOSPC;
    }
    k_memcpy(e->name, name, len + 1u);
    e->node = n;
    while (*link) {
        link = &(*link)->next;
    }
    *link = e;
    d->nentries++;
    if (is_dir(n)) {
        d->subdirs++;
        n->parent = d;
    }
    n->links++;
    stamp(d, STAMP_M | STAMP_C);
    return 0;
}

/* Take the name at `link` out of `d`; the node's link count drops. */
static struct tmpfs_node *remove_entry(struct tmpfs_node *d, struct tmpfs_dirent **link)
{
    struct tmpfs_dirent *e = *link;
    struct tmpfs_node *n = e->node;

    *link = e->next;
    kfree(e->name);
    kfree(e);
    d->nentries--;
    if (is_dir(n)) {
        d->subdirs--;
    }
    n->links--;
    stamp(d, STAMP_M | STAMP_C);
    stamp(n, STAMP_C);
    return n;
}

static struct tmpfs_node *dir_of(struct vfs_mount *mnt, void *dir)
{
    return dir ? (struct tmpfs_node *)dir : ((struct tmpfs_mount *)mnt->priv)->root;
}

static int tmpfs_lookup(struct vfs_mount *mnt, void *dir, const char *name, void **node)
{
    struct tmpfs_node *d = dir_of(mnt, dir);
    struct tmpfs_node *found;
    struct tmpfs_dirent **link;

    if (!is_dir(d)) {
        return -ENOTDIR;
    }
    if (name[0] == '.' && name[1] == '\0') {
        found = d;
    } else if (name[0] == '.' && name[1] == '.' && name[2] == '\0') {
        found = d->parent;
    } else {
        link = find(d, name);
        if (!link) {
            return -ENOENT;
        }
        found = (*link)->node;
    }
    found->refs++;
    *node = found;
    return 0;
}

static void tmpfs_hold(struct vfs_mount *mnt, void *node)
{
    (void)mnt;
    ((struct tmpfs_node *)node)->refs++;
}

static void tmpfs_put(struct vfs_mount *mnt, void *node)
{
    struct tmpfs_node *n = (struct tmpfs_node *)node;

    (void)mnt;
    n->refs--;
    settle(n);
}

static int tmpfs_getattr(struct vfs_mount *mnt, void *node, struct vfs_stat *st)
{
    struct tmpfs_node *n = (struct tmpfs_node *)node;

    (void)mnt;
    k_memset(st, 0, sizeof(*st));
    st->mode = n->mode;
    st->uid = n->uid;
    st->gid = n->gid;
    st->nlink = is_dir(n) ? 2u + n->subdirs : n->links;
    st->ino = n->ino;
    st->rdev = n->rdev;
    if (is_dir(n)) {
        st->size = (uint64_t)(n->nentries + 2u) * DIRENT_BYTES;
    } else if (n->target) {
        st->size = k_strlen(n->target);
    } else {
        st->size = n->data.size;
    }
    st->atime_sec = n->atime;
    st->atime_nsec = n->atime_ns;
    st->mtime_sec = n->mtime;
    st->mtime_nsec = n->mtime_ns;
    st->ctime_sec = n->ctime;
    st->ctime_nsec = n->ctime_ns;
    return 0;
}

static int tmpfs_setattr(struct vfs_mount *mnt, void *node, int what, const struct vfs_stat *st)
{
    struct tmpfs_node *n = (struct tmpfs_node *)node;

    (void)mnt;
    if (what & VFS_SET_SIZE) {
        int rc;

        if (is_dir(n)) {
            return -EISDIR;
        }
        if ((n->mode & T_IFMT) != T_IFREG) {
            return -EINVAL;
        }
        rc = shmem_truncate(&n->data, st->size);
        if (rc != 0) {
            return rc;
        }
        stamp(n, STAMP_M);
    }
    if (what & VFS_SET_MODE) {
        n->mode = (n->mode & T_IFMT) | (st->mode & 07777u);
    }
    if (what & VFS_SET_UID) {
        n->uid = st->uid;
    }
    if (what & VFS_SET_GID) {
        n->gid = st->gid;
    }
    if (what & VFS_SET_ATIME) {
        n->atime = st->atime_sec;
        n->atime_ns = st->atime_nsec;
    }
    if (what & VFS_SET_MTIME) {
        n->mtime = st->mtime_sec;
        n->mtime_ns = st->mtime_nsec;
    }
    stamp(n, STAMP_C);
    return 0;
}

static int tmpfs_open(struct vfs_mount *mnt, void *node, int flags, void **handle)
{
    struct tmpfs_node *n = (struct tmpfs_node *)node;

    (void)mnt;
    (void)flags;
    if ((n->mode & T_IFMT) == T_IFCHR || (n->mode & T_IFMT) == T_IFBLK) {
        return -ENXIO;
    }
    n->refs++;
    *handle = node;
    return 0;
}

static void tmpfs_release(struct vfs_mount *mnt, void *handle)
{
    tmpfs_put(mnt, handle);
}

static int32_t tmpfs_read(struct vfs_mount *mnt, void *handle, void *buf, uint32_t count,
                          uint64_t offset)
{
    struct tmpfs_node *n = (struct tmpfs_node *)handle;

    (void)mnt;
    if (is_dir(n)) {
        return -EISDIR;
    }
    if ((n->mode & T_IFMT) != T_IFREG) {
        return -EINVAL;
    }
    stamp(n, STAMP_A);
    return shmem_read(&n->data, buf, count, offset);
}

static int32_t tmpfs_write(struct vfs_mount *mnt, void *handle, const void *buf, uint32_t count,
                           uint64_t offset)
{
    struct tmpfs_node *n = (struct tmpfs_node *)handle;

    (void)mnt;
    if (is_dir(n)) {
        return -EISDIR;
    }
    if ((n->mode & T_IFMT) != T_IFREG) {
        return -EINVAL;
    }
    stamp(n, STAMP_M | STAMP_C);
    return shmem_write(&n->data, buf, count, offset);
}

static int tmpfs_truncate(struct vfs_mount *mnt, void *handle, uint64_t size)
{
    struct tmpfs_node *n = (struct tmpfs_node *)handle;

    (void)mnt;
    if ((n->mode & T_IFMT) != T_IFREG) {
        return is_dir(n) ? -EISDIR : -EINVAL;
    }
    stamp(n, STAMP_M | STAMP_C);
    return shmem_truncate(&n->data, size);
}

static int tmpfs_fsync(struct vfs_mount *mnt, void *handle)
{
    (void)mnt;
    (void)handle;
    return 0;
}

/* The cursor counts names: 0 is ".", 1 is "..", and the names follow in
 * order. A name removed during a listing moves the ones after it down. */
static int tmpfs_readdir(struct vfs_mount *mnt, void *handle, uint32_t *cursor,
                         struct vfs_dirent *ent)
{
    struct tmpfs_node *d = (struct tmpfs_node *)handle;
    struct tmpfs_dirent *e;
    uint32_t i;

    (void)mnt;
    if (!is_dir(d)) {
        return -ENOTDIR;
    }
    k_memset(ent, 0, sizeof(*ent));
    if (*cursor < 2) {
        ent->name[0] = '.';
        ent->name[1] = *cursor == 1 ? '.' : '\0';
        ent->ino = *cursor == 0 ? d->ino : d->parent->ino;
        ent->is_dir = 1;
        ent->type = T_IFDIR;
        (*cursor)++;
        return 1;
    }
    for (e = d->entries, i = 2; e && i < *cursor; e = e->next, i++) {
    }
    if (!e) {
        return 0;
    }
    k_strncpy(ent->name, e->name, VFS_NAME_MAX);
    ent->ino = e->node->ino;
    ent->is_dir = is_dir(e->node);
    ent->type = e->node->mode & T_IFMT;
    (*cursor)++;
    return 1;
}

/**
 * Make `name` in `dir` a node of the type the S_IF bits of `mode` give, a
 * regular file for none, with the device number `rdev` for a device. The
 * node comes back held. A directory and a symbolic link have operations of
 * their own and are EINVAL here.
 */
static int make_node(struct vfs_mount *mnt, void *dir, const char *name, uint32_t mode,
                     uint32_t rdev, void **node)
{
    struct tmpfs_node *d = dir_of(mnt, dir);
    struct tmpfs_node *n;
    uint32_t type = mode & T_IFMT;
    int rc;

    if (find(d, name)) {
        return -EEXIST;
    }
    if (type == 0) {
        type = T_IFREG;
    }
    if (type == T_IFDIR || type == T_IFLNK) {
        return -EINVAL;
    }
    n = new_node(mnt, type | (mode & 07777u));
    if (!n) {
        return -ENOSPC;
    }
    if (type == T_IFCHR || type == T_IFBLK) {
        n->rdev = rdev;
    }
    rc = add_entry(d, name, n);
    if (rc != 0) {
        kfree(n);
        return rc;
    }
    n->refs = 1;
    *node = n;
    return 0;
}

static int tmpfs_create(struct vfs_mount *mnt, void *dir, const char *name, uint32_t mode,
                        void **node)
{
    return make_node(mnt, dir, name, mode, 0, node);
}

static int tmpfs_mknod(struct vfs_mount *mnt, void *dir, const char *name, uint32_t mode,
                       uint32_t rdev, void **node)
{
    return make_node(mnt, dir, name, mode, rdev, node);
}

static int tmpfs_unlink(struct vfs_mount *mnt, void *dir, const char *name)
{
    struct tmpfs_node *d = dir_of(mnt, dir);
    struct tmpfs_dirent **link = find(d, name);

    if (!link) {
        return -ENOENT;
    }
    if (is_dir((*link)->node)) {
        return -EISDIR;
    }
    settle(remove_entry(d, link));
    return 0;
}

static int tmpfs_mkdir(struct vfs_mount *mnt, void *dir, const char *name, uint32_t mode)
{
    struct tmpfs_node *d = dir_of(mnt, dir);
    struct tmpfs_node *n;
    int rc;

    if (find(d, name)) {
        return -EEXIST;
    }
    n = new_node(mnt, T_IFDIR | (mode & 07777u));
    if (!n) {
        return -ENOSPC;
    }
    rc = add_entry(d, name, n);
    if (rc != 0) {
        kfree(n);
    }
    return rc;
}

static int tmpfs_rmdir(struct vfs_mount *mnt, void *dir, const char *name)
{
    struct tmpfs_node *d = dir_of(mnt, dir);
    struct tmpfs_dirent **link = find(d, name);
    struct tmpfs_node *n;

    if (!link) {
        return -ENOENT;
    }
    n = (*link)->node;
    if (!is_dir(n)) {
        return -ENOTDIR;
    }
    if (n->entries) {
        return -ENOTEMPTY;
    }
    settle(remove_entry(d, link));
    return 0;
}

/* Whether directory `a` is `d` or one of the directories above it. */
static int is_above(const struct tmpfs_node *a, const struct tmpfs_node *d)
{
    for (;;) {
        if (d == a) {
            return 1;
        }
        if (d->parent == d) {
            return 0;
        }
        d = d->parent;
    }
}

static int tmpfs_rename(struct vfs_mount *mnt, void *olddir, const char *oldname, void *newdir,
                        const char *newname)
{
    struct tmpfs_node *od = dir_of(mnt, olddir);
    struct tmpfs_node *nd = dir_of(mnt, newdir);
    struct tmpfs_dirent **from = find(od, oldname);
    struct tmpfs_dirent **to = find(nd, newname);
    struct tmpfs_node *n;
    int rc;

    if (!from) {
        return -ENOENT;
    }
    n = (*from)->node;
    if (to) {
        struct tmpfs_node *old = (*to)->node;

        if (old == n) {
            return 0;
        }
        if (is_dir(n) && !is_dir(old)) {
            return -ENOTDIR;
        }
        if (!is_dir(n) && is_dir(old)) {
            return -EISDIR;
        }
        if (is_dir(old) && old->entries) {
            return -ENOTEMPTY;
        }
    }
    if (is_dir(n) && is_above(n, nd)) {
        return -EINVAL;
    }
    n->refs++; /* kept while it has no name */
    remove_entry(od, find(od, oldname));
    to = find(nd, newname);
    if (to) {
        settle(remove_entry(nd, to));
    }
    rc = add_entry(nd, newname, n);
    n->refs--;
    if (rc != 0) {
        settle(n);
    }
    return rc;
}

static int tmpfs_link(struct vfs_mount *mnt, void *dir, const char *name, void *node)
{
    struct tmpfs_node *d = dir_of(mnt, dir);
    struct tmpfs_node *n = (struct tmpfs_node *)node;
    int rc;

    if (is_dir(n)) {
        return -EPERM;
    }
    if (find(d, name)) {
        return -EEXIST;
    }
    rc = add_entry(d, name, n);
    if (rc == 0) {
        stamp(n, STAMP_C);
    }
    return rc;
}

static int tmpfs_readlink(struct vfs_mount *mnt, void *node, char *buf, uint32_t cap)
{
    struct tmpfs_node *n = (struct tmpfs_node *)node;
    uint32_t len;

    (void)mnt;
    if (!n->target) {
        return -EINVAL;
    }
    len = (uint32_t)k_strlen(n->target);
    if (len > cap) {
        len = cap;
    }
    k_memcpy(buf, n->target, len);
    return (int)len;
}

static int tmpfs_statfs(struct vfs_mount *mnt, struct vfs_statfs *st)
{
    (void)mnt;
    st->magic = TMPFS_MAGIC;
    st->bsize = PAGE_SIZE;
    st->blocks = frame_total_count();
    st->bfree = frame_free_count();
    st->namelen = VFS_NAME_MAX;
    return 0;
}

static int tmpfs_sync(struct vfs_mount *mnt)
{
    (void)mnt;
    return 0;
}

static int tmpfs_prepare_unmount(struct vfs_mount *mnt)
{
    (void)mnt;
    return 0;
}

/* Free a directory's tree, the unmount's end: nothing holds a node of it,
 * since the common layer refuses an unmount while anything does. */
static void free_tree(struct tmpfs_node *d)
{
    while (d->entries) {
        struct tmpfs_dirent *e = d->entries;
        struct tmpfs_node *n = e->node;

        if (is_dir(n)) {
            free_tree(n);
        }
        remove_entry(d, &d->entries);
        n->refs = 0;
        settle(n);
    }
}

static void tmpfs_unmount(struct vfs_mount *mnt)
{
    struct tmpfs_mount *tm = (struct tmpfs_mount *)mnt->priv;

    free_tree(tm->root);
    tm->root->refs = 0;
    tm->root->links = 0;
    settle(tm->root);
    kfree(tm);
    mnt->priv = NULL;
    mnt->root = NULL;
}

static int tmpfs_path(struct vfs_mount *mnt, void *node, char *buf, uint32_t cap)
{
    struct tmpfs_node *root = ((struct tmpfs_mount *)mnt->priv)->root;
    struct tmpfs_node *n = (struct tmpfs_node *)node;
    uint32_t len = 0;

    /* Built from the end: each directory's name before the path so far. */
    if (cap < 2) {
        return -ENAMETOOLONG;
    }
    buf[cap - 1] = '\0';
    len = 1;
    while (n != root) {
        struct tmpfs_node *p = n->parent;
        struct tmpfs_dirent *e;
        uint32_t nl;

        for (e = p->entries; e && e->node != n; e = e->next) {
        }
        if (!e) {
            return -ENOENT;
        }
        nl = (uint32_t)k_strlen(e->name);
        if (len + nl + 1u > cap) {
            return -ENAMETOOLONG;
        }
        len += nl + 1u;
        k_memcpy(buf + cap - len + 1u, e->name, nl);
        buf[cap - len] = '/';
        n = p;
    }
    if (len == 1) {
        buf[0] = '/';
        buf[1] = '\0';
        return 0;
    }
    k_memmove(buf, buf + cap - len, len);
    return 0;
}

static int tmpfs_symlink(struct vfs_mount *mnt, void *dir, const char *name, const char *target)
{
    struct tmpfs_node *d = dir_of(mnt, dir);
    struct tmpfs_node *n;
    uint32_t len = (uint32_t)k_strlen(target);
    int rc;

    if (find(d, name)) {
        return -EEXIST;
    }
    n = new_node(mnt, T_IFLNK | 0777u);
    if (!n) {
        return -ENOSPC;
    }
    n->target = kmalloc(len + 1u);
    if (!n->target) {
        kfree(n);
        return -ENOSPC;
    }
    k_memcpy(n->target, target, len + 1u);
    rc = add_entry(d, name, n);
    if (rc != 0) {
        kfree(n->target);
        kfree(n);
    }
    return rc;
}

/* The options Linux's tmpfs takes that change something here: mode=, the
 * root's permission bits in octal. The rest are accepted and ignored. */
static uint32_t root_mode(const char *options)
{
    const char *p = options;

    while (p && *p) {
        if (p[0] == 'm' && p[1] == 'o' && p[2] == 'd' && p[3] == 'e' && p[4] == '=') {
            uint32_t mode = 0;

            for (p += 5; *p >= '0' && *p <= '7'; p++) {
                mode = mode * 8u + (uint32_t)(*p - '0');
            }
            return mode & 07777u;
        }
        while (*p && *p != ',') {
            p++;
        }
        if (*p == ',') {
            p++;
        }
    }
    return 01777u;
}

static int tmpfs_mount(struct vfs_mount *mnt, block_device_t *dev, uint32_t flags,
                       const char *options)
{
    struct tmpfs_mount *tm = kzalloc(sizeof(*tm));
    struct tmpfs_node *root;

    (void)dev;
    (void)flags;
    if (!tm) {
        return -ENOMEM;
    }
    mnt->priv = tm;
    root = new_node(mnt, T_IFDIR | root_mode(options));
    if (!root) {
        kfree(tm);
        mnt->priv = NULL;
        return -ENOMEM;
    }
    root->parent = root;
    root->links = 1;
    root->refs = 1;
    tm->root = root;
    mnt->root = root;
    return 0;
}

struct shmem *tmpfs_shmem(void *node)
{
    struct tmpfs_node *n = (struct tmpfs_node *)node;

    return (n->mode & T_IFMT) == T_IFREG ? &n->data : NULL;
}

static const struct vfs_ops tmpfs_ops = {
    tmpfs_lookup,  0, /* lookup_rest */
    tmpfs_hold,    tmpfs_put,   tmpfs_getattr,         tmpfs_setattr, tmpfs_open,    tmpfs_release,
    tmpfs_read,    tmpfs_write, tmpfs_truncate,        tmpfs_fsync,   tmpfs_readdir, tmpfs_create,
    tmpfs_unlink,  tmpfs_mkdir, tmpfs_rmdir,           tmpfs_rename,  tmpfs_link,    tmpfs_readlink,
    tmpfs_statfs,  tmpfs_sync,  tmpfs_prepare_unmount, 0, /* abort_unmount */
    tmpfs_unmount, tmpfs_path,  tmpfs_symlink,         tmpfs_mknod,
};

const struct vfs_type vfs_tmpfs_type = {"tmpfs", tmpfs_mount, &tmpfs_ops, 1, 1};
