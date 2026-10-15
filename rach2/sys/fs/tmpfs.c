/*
 * The tmpfs of tmpfs.h, a file system in memory. Its operations take no
 * lock of their own; the type sets `ns_serialised`, so the namespace changes
 * and a description's uses of a file run under the mount's namespace lock.
 */

#include "tmpfs.h"

#include "../irq.h"
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
#define T_ISGID  0002000u

#define TMPFS_MAGIC 0x01021994u

/* Format: the size Linux's tmpfs gives each name in a directory's size. */
#define DIRENT_BYTES 20u

struct tmpfs_node;

/* A name in a directory. `off` is its place there, which readdir's cursor
 * counts: 2 on, rising along the list, and kept while the name is there, so
 * a name removed during a listing moves no other. */
struct tmpfs_dirent {
    struct tmpfs_dirent *next;
    struct tmpfs_node *node;
    char *name;
    uint32_t off;
};

/*
 * A node. `refs` counts the holds of lookups, descriptions, working
 * directories and mounts, and for a directory the hold each subdirectory has
 * on it through `parent`; `links` the names that name it. It is freed when
 * both are gone. A directory's names are in `entries`, in the order they
 * were made or renamed into it; a symbolic link's target is in `target`,
 * NULL for any other node.
 */
struct tmpfs_node {
    struct tmpfs_node *parent; /* a directory's own, held; the root's is itself */
    struct tmpfs_dirent *entries;
    uint32_t nentries;
    uint32_t subdirs;  /* the directories among the names */
    uint32_t next_off; /* a directory's place for its next name, 0 for 2 */
    uint32_t refs;
    uint32_t links;
    uint32_t mode;
    uint32_t uid;
    uint32_t gid;
    uint32_t rdev;  /* a device node's number, in the kernel's encoding */
    uint32_t seals; /* the TMPFS_SEAL_ bits a regular file has */
    uint64_t ino;
    int64_t atime, atime_ns, mtime, mtime_ns, ctime, ctime_ns;
    struct shmem data; /* a regular file's bytes */
    char *target;
    char *label; /* a memfd's name, for /proc/PID/fd; NULL for a named file */
};

/* The state of a mount, in its `priv`. */
struct tmpfs_mount {
    struct tmpfs_node *root;
    uint64_t next_ino;        /* the last inode number given; the root's is 1 */
    struct shmem_quota quota; /* the pages its files hold, against size= */
};

/* The times `stamp` sets: access, modification and change. */
#define STAMP_A 1
#define STAMP_M 2
#define STAMP_C 4

/** Set the times of `n` that `which`, of the STAMP_ bits, chooses to the
 * current time. */
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

/** Whether `n` is a directory. */
static int is_dir(const struct tmpfs_node *n)
{
    return (n->mode & T_IFMT) == T_IFDIR;
}

/** A new node of `mode` for the directory `dir`, or of no directory for the
 * root and a memfd, with no hold, no name, the next inode number and the
 * seal TMPFS_SEAL_SEAL; NULL when the heap has no room. It is owned as
 * Linux's `inode_init_owner` owns one: by the caller's effective ids, but
 * with the group of a setgid directory, which passes the bit on to a new
 * directory. */
static struct tmpfs_node *new_node(struct vfs_mount *mnt, const struct tmpfs_node *dir,
                                   uint32_t mode)
{
    struct tmpfs_mount *tm = (struct tmpfs_mount *)mnt->priv;
    struct tmpfs_node *n = kzalloc(sizeof(*n));

    if (!n) {
        return NULL;
    }
    n->uid = current_proc ? current_proc->euid : 0u;
    n->gid = current_proc ? current_proc->egid : 0u;
    if (dir && (dir->mode & T_ISGID)) {
        n->gid = dir->gid;
        if ((mode & T_IFMT) == T_IFDIR) {
            mode |= T_ISGID;
        }
    }
    n->mode = mode;
    n->seals = TMPFS_SEAL_SEAL;
    n->ino = ++tm->next_ino;
    n->data.quota = &tm->quota;
    stamp(n, STAMP_A | STAMP_M | STAMP_C);
    return n;
}

/**
 * Free a node that nothing holds and no name names, and let go of the hold a
 * directory has on its parent, which may free the parent in turn. A removed
 * directory some process still holds keeps its removed parent this way, so
 * its `..` stays a directory to list and to walk. The climb is a loop, since
 * a chain of removed directories may be as deep as a tree.
 */
static void settle(struct tmpfs_node *n)
{
    while (n && n->refs == 0 && n->links == 0) {
        struct tmpfs_node *up = is_dir(n) && n->parent != n ? n->parent : NULL;

        shmem_free(&n->data);
        kfree(n->target);
        kfree(n->label);
        kfree(n);
        if (up) {
            up->refs--;
        }
        n = up;
    }
}

/** The link in the list of names of `d` that points at the entry `name`, or
 * NULL when there is none. */
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

/** An entry for the name `name`, from the heap and in no directory yet; NULL
 * when the heap has no room for it. */
static struct tmpfs_dirent *new_entry(const char *name)
{
    struct tmpfs_dirent *e = kzalloc(sizeof(*e));
    uint32_t len = (uint32_t)k_strlen(name);

    if (!e) {
        return NULL;
    }
    e->name = kmalloc(len + 1u);
    if (!e->name) {
        kfree(e);
        return NULL;
    }
    k_memcpy(e->name, name, len + 1u);
    return e;
}

/**
 * Give the names of `d` new places, 2 on, in their order, when its next place
 * would be 0xFFFFFFFF: a listing's cursor goes one past the place of the name
 * it returns, and could not go past that one. A listing in progress may then
 * see a name twice or miss one, as it may when names are made or removed
 * during it; it takes about four billion names made in one directory.
 */
static void renumber(struct tmpfs_node *d)
{
    struct tmpfs_dirent *e;
    uint32_t off = 2;

    for (e = d->entries; e; e = e->next) {
        e->off = off++;
    }
    d->next_off = off;
}

/**
 * Put the entry `e` for `n` at the end of `d`, so a directory lists its
 * names in the order they were made, at the directory's next place. `n`
 * gains a link, and `d` new modification and change times. A directory
 * takes a hold on `d` as its parent and lets go of the one on the parent it
 * had, which a rename moves it from.
 */
static void link_entry(struct tmpfs_node *d, struct tmpfs_dirent *e, struct tmpfs_node *n)
{
    struct tmpfs_dirent **link = &d->entries;

    if (d->next_off == 0xFFFFFFFFu) {
        renumber(d);
    }
    if (d->next_off < 2) {
        d->next_off = 2;
    }
    e->off = d->next_off++;
    e->node = n;
    while (*link) {
        link = &(*link)->next;
    }
    *link = e;
    d->nentries++;
    if (is_dir(n)) {
        struct tmpfs_node *was = n->parent;

        d->subdirs++;
        d->refs++;
        n->parent = d;
        if (was) {
            was->refs--;
            settle(was);
        }
    }
    n->links++;
    stamp(d, STAMP_M | STAMP_C);
}

/** Add the name `name` for `n` at the end of `d`: 0, or -ENOSPC when the
 * heap has no room for the entry. */
static int add_entry(struct tmpfs_node *d, const char *name, struct tmpfs_node *n)
{
    struct tmpfs_dirent *e = new_entry(name);

    if (!e) {
        return -ENOSPC;
    }
    link_entry(d, e, n);
    return 0;
}

/** Take the name at `link` out of `d` and free its entry. The node it named
 * loses a link and is returned, for the caller to settle; both have new
 * change times, and `d` a new modification time. */
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

/** The directory `dir`, or the root of `mnt` for NULL. */
static struct tmpfs_node *dir_of(struct vfs_mount *mnt, void *dir)
{
    return dir ? (struct tmpfs_node *)dir : ((struct tmpfs_mount *)mnt->priv)->root;
}

/** The `lookup` of tmpfs: `.`, `..`, the parent, which at the root is the
 * root, or a name in the directory, held; -ENOTDIR for a node that is no
 * directory, or -ENOENT. */
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

/** The `hold` of tmpfs: one more hold on `node`. */
static void tmpfs_hold(struct vfs_mount *mnt, void *node)
{
    (void)mnt;
    ((struct tmpfs_node *)node)->refs++;
}

/** The `put` of tmpfs: one hold fewer, and the node freed when it has no
 * hold and no name left (`settle`). */
static void tmpfs_put(struct vfs_mount *mnt, void *node)
{
    struct tmpfs_node *n = (struct tmpfs_node *)node;

    (void)mnt;
    n->refs--;
    settle(n);
}

/** The `getattr` of tmpfs. A directory has two links and one for each
 * subdirectory, and a size of DIRENT_BYTES for each name and for `.` and
 * `..`; a symbolic link's size is its target's length; any other node has
 * the size of its bytes and the 512-byte blocks of the pages that hold
 * them. */
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
        st->blocks = (uint64_t)n->data.held * (PAGE_SIZE / 512u);
        st->blocks_known = 1;
    }
    st->atime_sec = n->atime;
    st->atime_nsec = n->atime_ns;
    st->mtime_sec = n->mtime;
    st->mtime_nsec = n->mtime_ns;
    st->ctime_sec = n->ctime;
    st->ctime_nsec = n->ctime_ns;
    return 0;
}

/**
 * Whether the seals of `n` allow its size to become `size`: F_SEAL_SHRINK
 * forbids a smaller one and F_SEAL_GROW a larger one, as on Linux. 0 or
 * -EPERM.
 */
static int seals_allow_size(const struct tmpfs_node *n, uint64_t size)
{
    if ((size < n->data.size && (n->seals & TMPFS_SEAL_SHRINK)) ||
        (size > n->data.size && (n->seals & TMPFS_SEAL_GROW))) {
        return -EPERM;
    }
    return 0;
}

/**
 * The `setattr` of tmpfs. A change of the execute bits under
 * TMPFS_SEAL_EXEC is -EPERM. A size change is -EISDIR on a directory,
 * -EINVAL on any other node but a regular file, -EPERM where the seals
 * forbid the size, or what `shmem_truncate` answers, and on success sets
 * the modification time. Every call that succeeds sets the change time.
 */
static int tmpfs_setattr(struct vfs_mount *mnt, void *node, int what, const struct vfs_stat *st)
{
    struct tmpfs_node *n = (struct tmpfs_node *)node;

    (void)mnt;
    if ((what & VFS_SET_MODE) && (n->seals & TMPFS_SEAL_EXEC) && ((st->mode ^ n->mode) & 0111u)) {
        return -EPERM;
    }
    if (what & VFS_SET_SIZE) {
        int rc;

        if (is_dir(n)) {
            return -EISDIR;
        }
        if ((n->mode & T_IFMT) != T_IFREG) {
            return -EINVAL;
        }
        rc = seals_allow_size(n, st->size);
        if (rc == 0) {
            rc = shmem_truncate(&n->data, st->size);
        }
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

/** The `open` of tmpfs: the node, held once more, is the handle. -ENXIO for
 * a device node, which open(2) opens as the /dev device of its number
 * without calling the type. */
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

/** The `release` of tmpfs: the open's hold put back. */
static void tmpfs_release(struct vfs_mount *mnt, void *handle)
{
    tmpfs_put(mnt, handle);
}

/** The `read` of tmpfs: bytes of a regular file (`shmem_read`), which sets
 * its access time; -EISDIR for a directory, -EINVAL for any other node. */
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

/** The `write` of tmpfs: bytes of a regular file (`shmem_write`), which sets
 * its modification and change times; -EISDIR for a directory, -EINVAL for
 * any other node. A write under F_SEAL_WRITE or F_SEAL_FUTURE_WRITE is
 * -EPERM, and one past the end under F_SEAL_GROW is -EPERM whole, as on
 * Linux. */
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
    if ((n->seals & (TMPFS_SEAL_WRITE | TMPFS_SEAL_FUTURE_WRITE)) ||
        ((n->seals & TMPFS_SEAL_GROW) && offset + count > n->data.size)) {
        return -EPERM;
    }
    stamp(n, STAMP_M | STAMP_C);
    return shmem_write(&n->data, buf, count, offset);
}

/** The `truncate` of tmpfs: a regular file's new size (`shmem_truncate`),
 * which sets its modification and change times; -EISDIR for a directory,
 * -EINVAL for any other node, or -EPERM where the seals forbid the size. */
static int tmpfs_truncate(struct vfs_mount *mnt, void *handle, uint64_t size)
{
    struct tmpfs_node *n = (struct tmpfs_node *)handle;

    (void)mnt;
    if ((n->mode & T_IFMT) != T_IFREG) {
        return is_dir(n) ? -EISDIR : -EINVAL;
    }
    if (seals_allow_size(n, size) != 0) {
        return -EPERM;
    }
    stamp(n, STAMP_M | STAMP_C);
    return shmem_truncate(&n->data, size);
}

/** The `fsync` of tmpfs, which has nothing to write: 0. */
static int tmpfs_fsync(struct vfs_mount *mnt, void *handle)
{
    (void)mnt;
    (void)handle;
    return 0;
}

/**
 * The name at or after `*cursor` in the directory `handle`: 0 is ".", 1 is
 * "..", and the names follow at their places, which removing a name does not
 * change, so a program that removes names while it lists them, as `rm -r`
 * does, misses none. The cursor goes past the place of the name returned.
 * 1 with `ent` filled, 0 at the end, or -ENOTDIR.
 */
static int tmpfs_readdir(struct vfs_mount *mnt, void *handle, uint32_t *cursor,
                         struct vfs_dirent *ent)
{
    struct tmpfs_node *d = (struct tmpfs_node *)handle;
    struct tmpfs_dirent *e;

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
        ent->type = VFS_DT_DIR;
        (*cursor)++;
        return 1;
    }
    for (e = d->entries; e && e->off < *cursor; e = e->next) {
    }
    if (!e) {
        return 0;
    }
    k_strncpy(ent->name, e->name, VFS_NAME_MAX);
    ent->ino = e->node->ino;
    ent->is_dir = is_dir(e->node);
    ent->type = (int)VFS_DT_OF(e->node->mode);
    *cursor = e->off + 1u;
    return 1;
}

/**
 * Make `name` in `dir` a node of the type the S_IF bits of `mode` give, a
 * regular file for none, with the device number `rdev` for a device. The
 * node comes back held. A directory and a symbolic link have operations of
 * their own and are -EINVAL here; a name already there is -EEXIST, and a
 * heap with no room -ENOSPC.
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
    n = new_node(mnt, d, type | (mode & 07777u));
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

/** The `create` of tmpfs: `make_node` with no device number. */
static int tmpfs_create(struct vfs_mount *mnt, void *dir, const char *name, uint32_t mode,
                        void **node)
{
    return make_node(mnt, dir, name, mode, 0, node);
}

/** The `mknod` of tmpfs: `make_node`. */
static int tmpfs_mknod(struct vfs_mount *mnt, void *dir, const char *name, uint32_t mode,
                       uint32_t rdev, void **node)
{
    return make_node(mnt, dir, name, mode, rdev, node);
}

/** The `unlink` of tmpfs: the name `name` taken out of `dir`, and its node
 * freed when nothing holds it; -ENOENT, or -EISDIR for a directory. */
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

/** The `mkdir` of tmpfs: a new directory `name` in `dir` with the permission
 * bits of `mode`, owned as `new_node` says; -EEXIST, or -ENOSPC when the heap
 * has no room. */
static int tmpfs_mkdir(struct vfs_mount *mnt, void *dir, const char *name, uint32_t mode)
{
    struct tmpfs_node *d = dir_of(mnt, dir);
    struct tmpfs_node *n;
    int rc;

    if (find(d, name)) {
        return -EEXIST;
    }
    n = new_node(mnt, d, T_IFDIR | (mode & 07777u));
    if (!n) {
        return -ENOSPC;
    }
    rc = add_entry(d, name, n);
    if (rc != 0) {
        kfree(n);
    }
    return rc;
}

/** The `rmdir` of tmpfs: the empty directory `name` taken out of `dir`, and
 * freed when nothing holds it; -ENOENT, -ENOTDIR, or -ENOTEMPTY. */
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

/** Whether directory `a` is `d` or one of the directories above it. */
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

/**
 * Move the name `oldname` in `olddir` to `newname` in `newdir`, replacing a
 * file or an empty directory there; a rename of a name to another name of
 * the same node does nothing. The new name's entry is taken from the heap
 * before either directory changes, so a rename the heap cannot serve
 * answers -ENOSPC with both names as they were. -ENOENT, -ENOTDIR or
 * -EISDIR for a directory and a file in each other's place, -ENOTEMPTY,
 * and -EINVAL for a directory moved below itself.
 */
static int tmpfs_rename(struct vfs_mount *mnt, void *olddir, const char *oldname, void *newdir,
                        const char *newname)
{
    struct tmpfs_node *od = dir_of(mnt, olddir);
    struct tmpfs_node *nd = dir_of(mnt, newdir);
    struct tmpfs_dirent **from = find(od, oldname);
    struct tmpfs_dirent **to = find(nd, newname);
    struct tmpfs_dirent *e;
    struct tmpfs_node *n;

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
    e = new_entry(newname);
    if (!e) {
        return -ENOSPC;
    }
    remove_entry(od, find(od, oldname));
    to = find(nd, newname);
    if (to) {
        settle(remove_entry(nd, to));
    }
    link_entry(nd, e, n);
    return 0;
}

/** The `link` of tmpfs: a new name `name` in `dir` for the file `node`,
 * which has a new change time; -EPERM for a directory, -EEXIST, or
 * -ENOSPC. */
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

/** The `readlink` of tmpfs: the target into `buf`, cut to `cap` bytes, with
 * no terminator; its length, or -EINVAL for a node that is no link. */
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

/**
 * The `statfs` of tmpfs, in blocks of a page. The mount's limit is its
 * blocks and the pages its files hold the blocks in use, as Linux counts a
 * tmpfs's use, so that df shows an empty /dev/shm empty; a mount without a
 * limit has the machine's frames as its blocks.
 */
static int tmpfs_statfs(struct vfs_mount *mnt, struct vfs_statfs *st)
{
    const struct shmem_quota *q = &((struct tmpfs_mount *)mnt->priv)->quota;
    size_t total = q->limit ? q->limit : frame_total_count();

    st->magic = TMPFS_MAGIC;
    st->bsize = PAGE_SIZE;
    st->blocks = total;
    st->bfree = q->used < total ? total - q->used : 0;
    st->namelen = VFS_NAME_MAX;
    return 0;
}

/* fallocate's flags that a tmpfs takes: the size kept, a hole punched. */
#define FALLOC_KEEP_SIZE  0x01u
#define FALLOC_PUNCH_HOLE 0x02u

/**
 * fallocate(2), as Linux's tmpfs takes it: the pages of the range made, the
 * size grown to its end unless FALLOC_FL_KEEP_SIZE, and with
 * FALLOC_FL_PUNCH_HOLE the range made to read as zeros and its whole pages
 * freed. A growth a seal forbids, or a hole in a file sealed against
 * writing, is -EPERM, any other mode -EOPNOTSUPP, and a node that is not a
 * regular file -ENODEV; a failed allocation answers the error of
 * `shmem_allocate`.
 */
static int tmpfs_fallocate(struct vfs_mount *mnt, void *handle, uint32_t mode, uint64_t offset,
                           uint64_t len)
{
    struct tmpfs_node *n = (struct tmpfs_node *)handle;
    uint64_t end = offset + len;
    int rc;

    (void)mnt;
    if ((n->mode & T_IFMT) != T_IFREG) {
        return -ENODEV;
    }
    if (mode == (FALLOC_PUNCH_HOLE | FALLOC_KEEP_SIZE)) {
        if (n->seals & (TMPFS_SEAL_WRITE | TMPFS_SEAL_FUTURE_WRITE)) {
            return -EPERM;
        }
        shmem_punch(&n->data, offset, len);
        stamp(n, STAMP_M | STAMP_C);
        return 0;
    }
    if (mode & ~FALLOC_KEEP_SIZE) {
        return -EOPNOTSUPP;
    }
    if (!(mode & FALLOC_KEEP_SIZE) && end > n->data.size && (n->seals & TMPFS_SEAL_GROW)) {
        return -EPERM;
    }
    rc = shmem_allocate(&n->data, offset, len);
    if (rc != 0) {
        return rc;
    }
    if (!(mode & FALLOC_KEEP_SIZE) && end > n->data.size) {
        n->data.size = end;
        stamp(n, STAMP_M | STAMP_C);
    }
    return 0;
}

/** The `sync` of tmpfs, which has nothing to write: 0. */
static int tmpfs_sync(struct vfs_mount *mnt)
{
    (void)mnt;
    return 0;
}

/** The first half of a tmpfs unmount, which has nothing to write: 0. */
static int tmpfs_prepare_unmount(struct vfs_mount *mnt)
{
    (void)mnt;
    return 0;
}

/**
 * Free the tree under `top`, the unmount's end: nothing holds a node of it,
 * since the VFS answers EBUSY to an unmount while anything does.
 *
 * The walk is a loop over the parent links and not a recursion, since a tree
 * may be as deep as a path is long and the kernel stack is 16 KiB. It goes
 * down into a first name that is a directory with names of its own, frees a
 * first name that is anything else, and goes back up from a directory it has
 * emptied, which is then the first name of its parent.
 */
static void free_tree(struct tmpfs_node *top)
{
    struct tmpfs_node *d = top;

    while (d != top || d->entries) {
        struct tmpfs_node *n;

        if (!d->entries) {
            d = d->parent;
            continue;
        }
        n = d->entries->node;
        if (is_dir(n) && n->entries) {
            d = n;
            continue;
        }
        remove_entry(d, &d->entries);
        n->refs = 0;
        settle(n);
    }
}

/** The second half of a tmpfs unmount: the tree, the root and the mount's
 * state freed. */
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

/** The `path` of tmpfs: the path of the directory `node` from the mount's
 * root, built from the end of `buf`, each directory's name in front of the
 * path so far, and moved to its start. -ENOENT for a directory taken out of
 * its parent, or -ENAMETOOLONG when the path does not fit `cap`. */
static int tmpfs_path(struct vfs_mount *mnt, void *node, char *buf, uint32_t cap)
{
    struct tmpfs_node *root = ((struct tmpfs_mount *)mnt->priv)->root;
    struct tmpfs_node *n = (struct tmpfs_node *)node;
    uint32_t len = 0;

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

/** The `symlink` of tmpfs: a link `name` in `dir`, mode 0777, to a copy of
 * `target`; -EEXIST, or -ENOSPC when the heap has no room. */
static int tmpfs_symlink(struct vfs_mount *mnt, void *dir, const char *name, const char *target)
{
    struct tmpfs_node *d = dir_of(mnt, dir);
    struct tmpfs_node *n;
    uint32_t len = (uint32_t)k_strlen(target);
    int rc;

    if (find(d, name)) {
        return -EEXIST;
    }
    n = new_node(mnt, d, T_IFLNK | 0777u);
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

/** The value of the option `name=` in `options`, or NULL; the value ends at
 * the next comma. */
static const char *option(const char *options, const char *name)
{
    const char *p = options;

    while (p && *p) {
        uint32_t i = 0;

        while (name[i] && p[i] == name[i]) {
            i++;
        }
        if (!name[i] && p[i] == '=') {
            return p + i + 1;
        }
        while (*p && *p != ',') {
            p++;
        }
        if (*p == ',') {
            p++;
        }
    }
    return NULL;
}

/** The root's permission bits, in octal from mode=, or 1777. */
static uint32_t root_mode(const char *options)
{
    const char *p = option(options, "mode");
    uint32_t mode = 0;

    if (!p) {
        return 01777u;
    }
    for (; *p >= '0' && *p <= '7'; p++) {
        mode = mode * 8u + (uint32_t)(*p - '0');
    }
    return mode & 07777u;
}

/**
 * The pages the mount's files may hold, from size=: bytes, with k, m or g
 * after them for KiB, MiB or GiB, or a percentage of the machine's memory
 * with %, rounded up to whole pages; 0, for size=0, is no limit. Without
 * size= the limit is half the machine's memory, as Linux's default is.
 * -EINVAL for a value that is no number.
 */
static int64_t size_pages(const char *options)
{
    const char *p = option(options, "size");
    uint64_t n = 0;
    size_t total = frame_total_count();
    int shift = 0;

    if (!p) {
        return (int64_t)(total / 2u);
    }
    if (*p < '0' || *p > '9') {
        return -EINVAL;
    }
    for (; *p >= '0' && *p <= '9' && n < (1ull << 40); p++) {
        n = n * 10u + (uint64_t)(*p - '0');
    }
    if (*p == '%') {
        n = n > 1000u ? 1000u : n;
        return (int64_t)((size_t)n * (total / 100u) + (size_t)n * (total % 100u) / 100u);
    }
    if (*p == 'k' || *p == 'K') {
        shift = 10;
    } else if (*p == 'm' || *p == 'M') {
        shift = 20;
    } else if (*p == 'g' || *p == 'G') {
        shift = 30;
    }
    p += shift != 0;
    if (*p != '\0' && *p != ',') {
        return -EINVAL;
    }
    if (n > (~0ull - (PAGE_SIZE - 1u)) >> shift) {
        n = (~0ull - (PAGE_SIZE - 1u)) >> shift;
    }
    n = ((n << shift) + PAGE_SIZE - 1u) >> 12;
    return (int64_t)(n > (size_t)~(size_t)0 ? (size_t)~(size_t)0 : n);
}

/** The constructor of tmpfs: a mount whose files may hold the pages size=
 * gives, with a root directory of the mode mode= gives, owned by the caller;
 * -EINVAL for a size that is no number, or -ENOMEM. */
static int tmpfs_mount(struct vfs_mount *mnt, block_device_t *dev, uint32_t flags,
                       const char *options)
{
    struct tmpfs_mount *tm;
    struct tmpfs_node *root;
    int64_t limit = size_pages(options);

    (void)dev;
    (void)flags;
    if (limit < 0) {
        return (int)limit;
    }
    tm = kzalloc(sizeof(*tm));
    if (!tm) {
        return -ENOMEM;
    }
    tm->quota.limit = (size_t)limit;
    mnt->priv = tm;
    root = new_node(mnt, NULL, T_IFDIR | root_mode(options));
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

int tmpfs_memfd(struct vfs_mount *mnt, const char *name, uint32_t mode, uint32_t seals, void **node)
{
    uint32_t len = (uint32_t)k_strlen(name);
    struct tmpfs_node *n = new_node(mnt, NULL, T_IFREG | (mode & 07777u));

    if (!n) {
        return -ENOMEM;
    }
    n->label = kmalloc(len + 1u);
    if (!n->label) {
        kfree(n);
        return -ENOMEM;
    }
    k_memcpy(n->label, name, len + 1u);
    n->seals = seals;
    n->refs = 1;
    *node = n;
    return 0;
}

uint32_t tmpfs_seals(void *node)
{
    return ((struct tmpfs_node *)node)->seals;
}

int tmpfs_add_seals(void *node, uint32_t seals)
{
    struct tmpfs_node *n = (struct tmpfs_node *)node;

    if (seals & ~TMPFS_SEAL_ALL) {
        return -EINVAL;
    }
    if (n->seals & TMPFS_SEAL_SEAL) {
        return -EPERM;
    }
    if ((seals & TMPFS_SEAL_EXEC) && (n->mode & 0111u)) {
        seals |= TMPFS_SEAL_SHRINK | TMPFS_SEAL_GROW | TMPFS_SEAL_WRITE | TMPFS_SEAL_FUTURE_WRITE;
    }
    n->seals |= seals;
    return 0;
}

const char *tmpfs_label(void *node)
{
    return ((struct tmpfs_node *)node)->label;
}

/**
 * The `remount` of tmpfs takes a new size=, as Linux's tmpfs does; one
 * smaller than the pages the files hold is -EINVAL. A tmpfs has nothing on
 * a device to settle, and the wrappers answer EROFS to every write to a
 * read-only mount, so the flags change nothing here.
 */
static int tmpfs_remount(struct vfs_mount *mnt, uint32_t flags, const char *options)
{
    struct tmpfs_mount *tm = (struct tmpfs_mount *)mnt->priv;
    int64_t limit;

    (void)flags;
    if (!option(options, "size")) {
        return 0;
    }
    limit = size_pages(options);
    if (limit < 0) {
        return (int)limit;
    }
    if (limit != 0 && (size_t)limit < tm->quota.used) {
        return -EINVAL;
    }
    tm->quota.limit = (size_t)limit;
    return 0;
}

static const struct vfs_ops tmpfs_ops = {
    tmpfs_lookup,
    0, /* lookup_rest */
    tmpfs_hold,
    tmpfs_put,
    tmpfs_getattr,
    tmpfs_setattr,
    tmpfs_open,
    tmpfs_release,
    tmpfs_read,
    tmpfs_write,
    tmpfs_truncate,
    tmpfs_fsync,
    tmpfs_readdir,
    tmpfs_create,
    tmpfs_unlink,
    tmpfs_mkdir,
    tmpfs_rmdir,
    tmpfs_rename,
    tmpfs_link,
    tmpfs_readlink,
    tmpfs_statfs,
    tmpfs_sync,
    tmpfs_prepare_unmount,
    0, /* abort_unmount */
    tmpfs_unmount,
    tmpfs_path,
    tmpfs_symlink,
    tmpfs_mknod,
    0, /* pin: a hold keeps a node */
    0, /* unpin */
    tmpfs_remount,
    tmpfs_fallocate,
};

const struct vfs_type vfs_tmpfs_type = {"tmpfs", tmpfs_mount, &tmpfs_ops, 1, 1};
