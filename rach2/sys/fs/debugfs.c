/*
 * debugfs.c: the debug file system, a tree of directories and files the
 * kernel registers, each file with operations its open binds to the
 * descriptor, as Linux's debugfs_create_file gives them.
 */

#include "debugfs.h"

#include "vfs.h"
#include "krffs.h"
#include "../drm/drm.h"
#include "../mm/kheap.h"
#include "../mm/usercopy.h"
#include "../lib/kerrno.h"
#include "../lib/kstring.h"
#include "../syscall_internal.h"

#define DFS_IFDIR 0040000u
#define DFS_IFREG 0100000u
#define DFS_IFLNK 0120000u

/*
 * A directory, a file or a symbolic link. A directory's children are a
 * list in the order they were made, which is the order it lists them.
 * `refs` counts the holds of the walks and of the descriptions open on the
 * node; a node taken out (`removed`) is off its parent's list and is freed
 * by its last put, which drops its hold on its owner.
 */
struct debugfs_node {
    char name[DEBUGFS_NAME_MAX + 1];
    struct debugfs_node *parent;
    struct debugfs_node *children; /* a directory's, oldest first */
    struct debugfs_node *next;     /* the next child of the parent */
    uint32_t mode;                 /* the S_IF type bits and the permission bits */
    uint32_t uid, gid;
    uint64_t ino;
    uint32_t refs;
    int removed;
    const struct file_ops *ops;            /* a file's */
    debugfs_open_fn open;                  /* a file's, or NULL */
    const struct debugfs_text_ops *text;   /* a text file's, or NULL */
    void *data;                            /* a file's registrant's, its descriptors' `priv` */
    char *target;                          /* a link's, kmalloc'd */
    uint64_t size;                         /* a file's, 0 until a truncation sets it */
    const struct debugfs_owner *owner_ops; /* what the node holds, or NULL */
    void *owner;
    /* Its access, modification and change times, each of its making until
     * a setattr or a truncation stores another, as seconds and nanoseconds. */
    int64_t atime_s, atime_ns, mtime_s, mtime_ns, ctime_s, ctime_ns;
};

/* The root directory, which lasts the life of the kernel. Linux's default
 * mode is 0700, root's: no one else may look below /sys/kernel/debug. */
static struct debugfs_node root = {"",   &root, NULL, NULL, DFS_IFDIR | 0700u,
                                   0,    0,     1,    1,    0,
                                   NULL, NULL,  NULL, NULL, NULL,
                                   0,    NULL,  NULL, 0,    0,
                                   0,    0,     0,    0};
static uint64_t last_ino = 1;
static int started;

/** Whether `n` is a directory. */
static int is_dir(const struct debugfs_node *n)
{
    return (n->mode & 0170000u) == DFS_IFDIR;
}

/** Whether the string `s` is the `len` bytes at `name`. */
static int same_name(const char *s, const char *name, uint32_t len)
{
    uint32_t i;

    for (i = 0; i < len && s[i] == name[i]; i++) {
    }
    return i == len && s[len] == '\0';
}

/** The child of `d` named by the `len` bytes at `name`, or NULL. */
static struct debugfs_node *child(const struct debugfs_node *d, const char *name, uint32_t len)
{
    struct debugfs_node *c;

    for (c = d->children; c; c = c->next) {
        if (same_name(c->name, name, len)) {
            return c;
        }
    }
    return NULL;
}

/** A new node `name[0..len)` of `mode` at the end of `d`'s list, stamped
 * now and holding `d`'s owner; NULL when the heap has no room. */
static struct debugfs_node *make(struct debugfs_node *d, const char *name, uint32_t len,
                                 uint32_t mode)
{
    struct debugfs_node *n = kzalloc(sizeof(*n));
    struct debugfs_node **link;

    if (!n) {
        return NULL;
    }
    k_memcpy(n->name, name, len);
    n->name[len] = '\0';
    n->parent = d;
    n->mode = mode;
    n->ino = ++last_ino;
    n->owner_ops = d->owner_ops;
    n->owner = d->owner;
    if (n->owner_ops) {
        n->owner_ops->get(n->owner);
    }
    get_current_time(&n->atime_s, &n->atime_ns);
    n->mtime_s = n->ctime_s = n->atime_s;
    n->mtime_ns = n->ctime_ns = n->atime_ns;
    for (link = &d->children; *link; link = &(*link)->next) {
    }
    *link = n;
    return n;
}

/** Free `n` once it is taken out and nothing holds it, then let go of its
 * owner, which may free what the owner's files use. */
static void settle(struct debugfs_node *n)
{
    if (n != &root && n->removed && n->refs == 0 && !n->children) {
        const struct debugfs_owner *ops = n->owner_ops;
        void *owner = n->owner;

        kfree(n->target);
        kfree(n);
        if (ops) {
            ops->put(owner);
        }
    }
}

/**
 * Walk `path` from the root: the length of its next component in `*len`
 * and its start in the return, past the slashes before it, or NULL at the
 * end. -1 in `*len` for a component that is empty past a slash, `.`, `..`
 * or longer than DEBUGFS_NAME_MAX.
 */
static const char *next_name(const char *path, int *len)
{
    const char *c;

    while (*path == '/') {
        path++;
    }
    if (*path == '\0') {
        return NULL;
    }
    for (c = path; *c && *c != '/'; c++) {
    }
    *len = (int)(c - path);
    if (*len > DEBUGFS_NAME_MAX ||
        (path[0] == '.' && (*len == 1 || (*len == 2 && path[1] == '.')))) {
        *len = -1;
    }
    return path;
}

/**
 * Make the node `path` of `mode`, its type bits among them, and the
 * directories on the way: the node in `*out`, or -EINVAL, -EEXIST,
 * -ENOTDIR or -ENOMEM as `debugfs_create_file` gives them.
 */
static int32_t create(const char *path, uint32_t mode, struct debugfs_node **out)
{
    struct debugfs_node *d = &root, *n;
    const char *name, *rest;
    int len;

    if (!path || !next_name(path, &len)) {
        return -EINVAL;
    }
    for (rest = path; (name = next_name(rest, &len)) != NULL; rest = name + len) {
        if (len < 0) {
            return -EINVAL;
        }
    }
    rest = path;
    for (;;) {
        name = next_name(rest, &len);
        rest = name + len;
        n = child(d, name, (uint32_t)len);
        if (!next_name(rest, &len)) {
            break;
        }
        if (n && !is_dir(n)) {
            return -ENOTDIR;
        }
        if (!n) {
            n = make(d, name, (uint32_t)(rest - name), DFS_IFDIR | 0755u);
            if (!n) {
                return -ENOMEM;
            }
        }
        d = n;
    }
    if (n) {
        return -EEXIST;
    }
    n = make(d, name, (uint32_t)(rest - name), mode);
    if (!n) {
        return -ENOMEM;
    }
    *out = n;
    return 0;
}

int32_t debugfs_create_file(const char *path, uint32_t mode, const struct file_ops *ops,
                            debugfs_open_fn open, void *data)
{
    struct debugfs_node *n;
    int32_t rc;

    if (!ops) {
        return -EINVAL;
    }
    rc = create(path, DFS_IFREG | (mode & 07777u), &n);
    if (rc == 0) {
        n->ops = ops;
        n->open = open;
        n->data = data;
    }
    return rc;
}

static const struct file_ops text_ops;

int32_t debugfs_create_text(const char *path, uint32_t mode, const struct debugfs_text_ops *ops,
                            void *data)
{
    struct debugfs_node *n;
    int32_t rc;

    if (!ops || !ops->show) {
        return -EINVAL;
    }
    rc = create(path, DFS_IFREG | (mode & 07777u), &n);
    if (rc == 0) {
        n->ops = &text_ops;
        n->text = ops;
        n->data = data;
    }
    return rc;
}

int32_t debugfs_create_dir(const char *path, const struct debugfs_owner *ops, void *owner)
{
    struct debugfs_node *n;
    int32_t rc = create(path, DFS_IFDIR | 0755u, &n);

    if (rc == 0 && ops) {
        if (n->owner_ops) {
            n->owner_ops->put(n->owner);
        }
        n->owner_ops = ops;
        n->owner = owner;
        ops->get(owner);
    }
    return rc;
}

int32_t debugfs_create_symlink(const char *path, const char *target)
{
    struct debugfs_node *n;
    uint32_t len;
    char *copy;
    int32_t rc;

    if (!target) {
        return -EINVAL;
    }
    len = (uint32_t)k_strlen(target);
    copy = kmalloc(len + 1u);
    if (!copy) {
        return -ENOMEM;
    }
    k_memcpy(copy, target, len + 1u);
    rc = create(path, DFS_IFLNK | 0777u, &n);
    if (rc != 0) {
        kfree(copy);
        return rc;
    }
    n->target = copy;
    return 0;
}

/** Take `n` and every node below it out of the tree; each is freed when
 * nothing holds it. */
static void take_out(struct debugfs_node *n)
{
    struct debugfs_node **link;

    while (n->children) {
        take_out(n->children);
    }
    for (link = &n->parent->children; *link && *link != n; link = &(*link)->next) {
    }
    if (*link) {
        *link = n->next;
    }
    n->next = NULL;
    n->removed = 1;
    settle(n);
}

void debugfs_remove(const char *path)
{
    struct debugfs_node *n = &root;
    const char *name;
    int len;

    while ((name = next_name(path, &len)) != NULL) {
        if (len < 0 || !is_dir(n) || (n = child(n, name, (uint32_t)len)) == NULL) {
            return;
        }
        path = name + len;
    }
    if (n != &root) {
        take_out(n);
    }
}

static int32_t text_bind(const struct debugfs_node *n, fd_entry_t *e);

int32_t debugfs_bind(void *node, fd_entry_t *e, int flags)
{
    struct debugfs_node *n = node;

    if (n->text) {
        return text_bind(n, e);
    }
    e->ops = n->ops;
    e->priv = n->data;
    return n->open ? n->open(e, flags) : 0;
}

/* The VFS operations. A node handle is a `struct debugfs_node`; NULL is the
 * root. */

/** The node `dir` names, the root for NULL. */
static struct debugfs_node *node_of(void *dir)
{
    return dir ? (struct debugfs_node *)dir : &root;
}

/** The `lookup` of debugfs: `.`, `..`, which at the root is the root, or a
 * name in the directory, held; -ENOTDIR for a file, -ENOENT for a name
 * that is not there or a directory taken out. */
static int dfs_lookup(struct vfs_mount *mnt, void *dir, const char *name, void **node)
{
    struct debugfs_node *d = node_of(dir), *n;

    (void)mnt;
    if (!is_dir(d)) {
        return -ENOTDIR;
    }
    if (d->removed) {
        return -ENOENT;
    }
    if (name[0] == '.' && name[1] == '\0') {
        n = d;
    } else if (name[0] == '.' && name[1] == '.' && name[2] == '\0') {
        n = d->parent;
    } else {
        n = child(d, name, (uint32_t)k_strlen(name));
        if (!n) {
            return -ENOENT;
        }
    }
    n->refs++;
    *node = n;
    return 0;
}

/** The `hold` of debugfs. */
static void dfs_hold(struct vfs_mount *mnt, void *node)
{
    (void)mnt;
    node_of(node)->refs++;
}

/** The `put` of debugfs: a node taken out goes with its last hold. */
static void dfs_put(struct vfs_mount *mnt, void *node)
{
    struct debugfs_node *n = node_of(node);

    (void)mnt;
    n->refs--;
    settle(n);
}

/** The `getattr` of debugfs: a directory has two links and one for each
 * subdirectory, a file or a symbolic link one, and each a size of 0 until a
 * truncation sets a file's, as Linux's debugfs has. */
static int dfs_getattr(struct vfs_mount *mnt, void *node, struct vfs_stat *st)
{
    struct debugfs_node *n = node_of(node), *c;

    (void)mnt;
    k_memset(st, 0, sizeof(*st));
    st->mode = n->mode;
    st->uid = n->uid;
    st->gid = n->gid;
    st->nlink = 1;
    st->size = n->size;
    if (is_dir(n)) {
        st->nlink = n->removed ? 0 : 2;
        for (c = n->children; c; c = c->next) {
            st->nlink += is_dir(c);
        }
    }
    st->ino = n->ino;
    st->atime_sec = n->atime_s;
    st->atime_nsec = n->atime_ns;
    st->mtime_sec = n->mtime_s;
    st->mtime_nsec = n->mtime_ns;
    st->ctime_sec = n->ctime_s;
    st->ctime_nsec = n->ctime_ns;
    return 0;
}

/** Set the change time of `n` to now, and its modification time with it
 * when `data` says the size changed. */
static void stamp(struct debugfs_node *n, int data)
{
    get_current_time(&n->ctime_s, &n->ctime_ns);
    if (data) {
        n->mtime_s = n->ctime_s;
        n->mtime_ns = n->ctime_ns;
    }
}

/**
 * The `setattr` of debugfs, as Linux's debugfs_setattr and simple_setattr:
 * the permission bits of `st->mode`, the type kept, the owner, the group,
 * the access and modification times and a file's size, as `what` selects,
 * and the change time set to now for every call, a VFS_SET_CTIME call with
 * nothing else among them. The caller has judged the permission. A size on
 * a directory is -EISDIR, and VFS_SET_FLAGS -EOPNOTSUPP, since debugfs
 * keeps no inode flags; both change nothing. 0 otherwise.
 */
static int dfs_setattr(struct vfs_mount *mnt, void *node, int what, const struct vfs_stat *st)
{
    struct debugfs_node *n = node_of(node);

    (void)mnt;
    if (what & VFS_SET_FLAGS) {
        return -EOPNOTSUPP;
    }
    if ((what & VFS_SET_SIZE) && is_dir(n)) {
        return -EISDIR;
    }
    if (what & VFS_SET_SIZE) {
        n->size = st->size;
    }
    if (what & VFS_SET_MODE) {
        n->mode = (n->mode & 0170000u) | (st->mode & 07777u);
    }
    if (what & VFS_SET_UID) {
        n->uid = st->uid;
    }
    if (what & VFS_SET_GID) {
        n->gid = st->gid;
    }
    if (what & VFS_SET_ATIME) {
        n->atime_s = st->atime_sec;
        n->atime_ns = st->atime_nsec;
    }
    stamp(n, (what & VFS_SET_SIZE) != 0);
    if (what & VFS_SET_MTIME) {
        n->mtime_s = st->mtime_sec;
        n->mtime_ns = st->mtime_nsec;
    }
    return 0;
}

/** The `readdir` of debugfs: 0 is ".", 1 is "..", and the children follow
 * in their order, the cursor counting them. 1 with `ent` filled, 0 at the
 * end, -ENOTDIR, or -ENOENT for a directory taken out. */
static int dfs_readdir(struct vfs_mount *mnt, void *handle, uint32_t *cursor,
                       struct vfs_dirent *ent)
{
    struct debugfs_node *d = node_of(handle), *c;
    uint32_t i;

    (void)mnt;
    if (!is_dir(d)) {
        return -ENOTDIR;
    }
    if (d->removed) {
        return -ENOENT;
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
    i = 2;
    for (c = d->children; c && i < *cursor; c = c->next) {
        i++;
    }
    if (!c) {
        return 0;
    }
    k_strncpy(ent->name, c->name, VFS_NAME_MAX);
    ent->ino = c->ino;
    ent->is_dir = is_dir(c);
    ent->type = (int)VFS_DT_OF(c->mode);
    (*cursor)++;
    return 1;
}

/** The `truncate` of debugfs, as Linux's simple_setattr: a file keeps no
 * bytes, so it takes the size, which stat reports, and the modification and
 * change times of now, as a truncation sets them. */
static int dfs_truncate(struct vfs_mount *mnt, void *handle, uint64_t size, int stamp_times)
{
    struct debugfs_node *n = node_of(handle);

    (void)mnt;
    if (is_dir(n)) {
        return -EISDIR;
    }
    if (size > 0x7FFFFFFFu) {
        return -EFBIG;
    }
    n->size = size;
    if (stamp_times) {
        stamp(n, 1);
    }
    return 0;
}

/** The `readlink` of debugfs: a link's target, cut to `cap` bytes, without
 * a NUL; its length, or -EINVAL for a node that is no link. */
static int dfs_readlink(struct vfs_mount *mnt, void *node, char *buf, uint32_t cap)
{
    struct debugfs_node *n = node_of(node);
    uint32_t len;

    (void)mnt;
    if (!n->target) {
        return -EINVAL;
    }
    len = (uint32_t)k_strlen(n->target);
    len = len < cap ? len : cap;
    k_memcpy(buf, n->target, len);
    return (int)len;
}

/** The `statfs` of debugfs, which has no blocks. */
static int dfs_statfs(struct vfs_mount *mnt, struct vfs_statfs *st)
{
    (void)mnt;
    k_memset(st, 0, sizeof(*st));
    st->magic = DEBUGFS_MAGIC;
    st->bsize = 4096;
    st->namelen = VFS_NAME_MAX;
    return 0;
}

/** The `path` of debugfs: the directory `node` from the root, "/" for the
 * root. -ENOTDIR for a file, -ENOENT for a directory taken out, or
 * -ENAMETOOLONG when it does not fit `cap`. */
static int dfs_path(struct vfs_mount *mnt, void *node, char *buf, uint32_t cap)
{
    struct debugfs_node *n = node_of(node);
    uint32_t len = 1;

    (void)mnt;
    if (!is_dir(n)) {
        return -ENOTDIR;
    }
    if (cap < 2) {
        return -ENAMETOOLONG;
    }
    buf[cap - 1] = '\0';
    for (; n != &root; n = n->parent) {
        uint32_t nl = (uint32_t)k_strlen(n->name);

        if (n->removed) {
            return -ENOENT;
        }
        if (len + nl + 1u > cap) {
            return -ENAMETOOLONG;
        }
        len += nl + 1u;
        k_memcpy(buf + cap - len + 1u, n->name, nl);
        buf[cap - len] = '/';
    }
    if (len == 1) {
        buf[0] = '/';
        buf[1] = '\0';
        return 0;
    }
    k_memmove(buf, buf + cap - len, len);
    return 0;
}

static int dfs_remount(struct vfs_mount *mnt, uint32_t flags, const char *options);

const struct vfs_ops debugfs_vops = {
    dfs_lookup,
    0, /* lookup_rest */
    dfs_hold,
    dfs_put,
    dfs_getattr,
    dfs_setattr,  /* setattr */
    0,            /* open */
    0,            /* release */
    0,            /* read: the descriptor's operations read a file */
    0,            /* write */
    dfs_truncate, /* truncate */
    0,            /* fsync */
    dfs_readdir,
    0,            /* create */
    0,            /* unlink */
    0,            /* mkdir */
    0,            /* rmdir */
    0,            /* rename */
    0,            /* link */
    dfs_readlink, /* readlink */
    dfs_statfs,
    0, /* sync */
    0, /* prepare_unmount */
    0, /* abort_unmount */
    0, /* unmount */
    dfs_path,
    0, /* symlink */
    0, /* mknod */
    0, /* pin */
    0, /* unpin */
    dfs_remount,
    0, /* fallocate */
    0, /* fdatasync */
    0, /* lock_node */
    0, /* unlock_node */
    0, /* shutdown */
    0, /* xattr */
    0, /* cachestat */
    0, /* put_unless_last */
    0, /* readlink_cached */
    0, /* read_frames */
    0, /* handle_node */
    0, /* writeback */
    0, /* lock_write */
    0, /* unlock_write */
    0, /* rename_flags */
    0, /* tmpfile */
    0, /* uncache */
    0, /* phase */
    0, /* data_at */
};

struct dfs_options {
    uint32_t mode, uid, gid;
    uint32_t supplied; /* VFS_SET bits for the options supplied */
};

static struct dfs_options mount_options = {0700u, 0, 0, 0};

/** Read one complete unsigned option, leaving the next comma in *end. */
static int option_number(const char *s, uint32_t base, const char **end, uint32_t *out)
{
    uint32_t v = 0;
    const char *start = s;

    while (*s >= '0' && *s < (char)('0' + base)) {
        uint32_t digit = (uint32_t)(*s++ - '0');

        if (v > (0xffffffffu - digit) / base) {
            return -EINVAL;
        }
        v = v * base + digit;
    }
    if (s == start || (*s && *s != ',')) {
        return -EINVAL;
    }
    *end = s;
    *out = v;
    return 0;
}

/** Whether the option at `s` starts with `key`, which ends in `=`. */
static int option_is(const char *s, const char *key)
{
    while (*key && *s == *key) {
        s++;
        key++;
    }
    return *key == '\0';
}

/** Parse a complete list before changing the root or its mount options. */
static int root_options(const char *s, struct dfs_options *opts)
{
    while (s && *s) {
        if (option_is(s, "mode=")) {
            if (option_number(s + 5, 8, &s, &opts->mode) != 0) {
                return -EINVAL;
            }
            opts->mode &= 07777u;
            opts->supplied |= VFS_SET_MODE;
        } else if (option_is(s, "uid=") || option_is(s, "gid=")) {
            int uid = *s == 'u';
            uint32_t *id = uid ? &opts->uid : &opts->gid;

            if (option_number(s + 4, 10, &s, id) != 0 || *id == 0xffffffffu) {
                return -EINVAL;
            }
            opts->supplied |= uid ? VFS_SET_UID : VFS_SET_GID;
        } else {
            while (*s && *s != ',') {
                s++;
            }
        }
        s += *s == ',';
    }
    return 0;
}

/** Save the option set; update only the root attributes the caller supplied. */
static void apply_options(const struct dfs_options *opts)
{
    mount_options = *opts;
    if (opts->supplied & VFS_SET_MODE) {
        root.mode = DFS_IFDIR | opts->mode;
    }
    if (opts->supplied & VFS_SET_UID) {
        root.uid = opts->uid;
    }
    if (opts->supplied & VFS_SET_GID) {
        root.gid = opts->gid;
    }
}

/** Apply a full remount to the singleton superblock. */
static int dfs_remount(struct vfs_mount *mnt, uint32_t flags, const char *options)
{
    struct dfs_options opts = {0700u, 0, 0, 0};
    int rc = root_options(options, &opts);

    (void)mnt;
    (void)flags;
    if (rc == 0) {
        apply_options(&opts);
    }
    return rc;
}

/*
 * The operations of a text file (`debugfs_create_text`): a `struct
 * dfs_text` in the descriptor's `priv` with the registrant's hooks and
 * data and the text the last read from offset 0 made, read at the
 * description's offset.
 */
struct dfs_text {
    const struct debugfs_text_ops *ops;
    void *data;
    char *text; /* kmalloc'd, or NULL before the first read */
    uint32_t len;
};

/** Make `t`'s text anew through its `show`, sizing it first and again if
 * it grew between the two calls. 0, `show`'s -errno or -ENOMEM. */
static int32_t text_fill(struct dfs_text *t)
{
    int32_t len, n;
    char *text;

    for (;;) {
        len = t->ops->show(t->data, NULL, 0);
        if (len < 0) {
            return len;
        }
        text = kmalloc((uint32_t)len + 1u);
        if (!text) {
            return -ENOMEM;
        }
        n = t->ops->show(t->data, text, (uint32_t)len + 1u);
        if (n >= 0 && n <= len) {
            break;
        }
        kfree(text);
        if (n < 0) {
            return n;
        }
    }
    kfree(t->text);
    t->text = text;
    t->len = (uint32_t)n;
    return 0;
}

/** Read the text from the description's offset, made anew at offset 0, as
 * seq_read does; bytes read, 0 at its end, `show`'s -errno, -ENOMEM or
 * -EFAULT. */
static int32_t text_read(fd_entry_t *e, char __user *buf, uint32_t count)
{
    struct dfs_text *t = e->priv;
    struct open_file *ofd = e->ofd;
    uint64_t off = ofd->offset;
    uint32_t n;
    int32_t rc;

    if (off == 0 || !t->text) {
        rc = text_fill(t);
        if (rc != 0) {
            return rc;
        }
    }
    if (off >= t->len) {
        return 0;
    }
    n = t->len - (uint32_t)off;
    n = n < count ? n : count;
    if (copy_to_user(buf, t->text + off, n) != 0) {
        return -EFAULT;
    }
    ofd->offset = off + n;
    return (int32_t)n;
}

/** Give a write to the file's `store`; -EINVAL for a file without one. */
static int32_t text_write(fd_entry_t *e, const char __user *buf, uint32_t count)
{
    struct dfs_text *t = e->priv;

    return t->ops->store ? t->ops->store(t->data, buf, count) : -EINVAL;
}

/** Move the description's offset in the text, as seq_lseek: SEEK_SET and
 * SEEK_CUR, -EINVAL for a negative result or another `whence`. */
static int64_t text_llseek(fd_entry_t *e, int64_t offset, int whence)
{
    int64_t base = whence == 1 ? (int64_t)e->ofd->offset : 0;

    if ((whence != 0 && whence != 1) || base + offset < 0) {
        return -EINVAL;
    }
    return base + offset;
}

/** Free the text at the last close. */
static void text_close(fd_entry_t *e)
{
    struct dfs_text *t = e->priv;

    kfree(t->text);
    kfree(t);
    e->priv = NULL;
}

static const struct file_ops text_ops = {text_read,  text_write, NULL,        NULL,
                                         text_close, NULL,       text_llseek, 0};

/** Bind the text file `n` to `e`: a `struct dfs_text` with no text yet.
 * 0, or -ENOMEM. */
static int32_t text_bind(const struct debugfs_node *n, fd_entry_t *e)
{
    struct dfs_text *t = kzalloc(sizeof(*t));

    if (!t) {
        return -ENOMEM;
    }
    t->ops = n->text;
    t->data = n->data;
    e->ops = &text_ops;
    e->priv = t;
    return 0;
}

/** sync/info's text: the software timelines and their points as
 * `sw_sync_info` writes them. */
static int32_t sync_info_show(void *data, char *buf, uint32_t cap)
{
    (void)data;
    return (int32_t)sw_sync_info(buf, cap);
}

static const struct debugfs_text_ops sync_info_ops = {sync_info_show, NULL};

/** The creators of live descriptions, including those with no descriptor. */
static int32_t file_creators_show(void *data, char *buf, uint32_t cap)
{
    (void)data;
    return fd_files_creators(buf, cap);
}

static const struct debugfs_text_ops file_creators_ops = {file_creators_show, NULL};

/** Dispatch sw_sync's open, whose descriptor is a timeline of its own. */
static int32_t sw_sync_bind(fd_entry_t *e, int flags)
{
    (void)flags;
    return sw_sync_open(e);
}

/* sync/sw_sync's operations until `sw_sync_open` replaces them. */
static const struct file_ops sw_sync_unbound = {NULL, NULL, NULL, NULL, NULL, NULL, NULL, 0};

/** Emit the options that set ownership and mode of the debugfs root. */
static void debugfs_options(const struct vfs_mount *mnt, vfs_option_emit emit, void *out)
{
    (void)mnt;
    if (mount_options.uid != 0) {
        emit(out, "uid", mount_options.uid, VFS_OPT_DEC);
    }
    if (mount_options.gid != 0) {
        emit(out, "gid", mount_options.gid, VFS_OPT_DEC);
    }
    if (mount_options.mode != 0700u) {
        emit(out, "mode", mount_options.mode, VFS_OPT_OCT);
    }
}

int debugfs_mount(struct vfs_mount *mnt, block_device_t *dev, uint32_t flags, const char *options)
{
    struct dfs_options opts = {0700u, 0, 0, 0};
    int rc = root_options(options, &opts);

    if (rc != 0) {
        return rc;
    }
    (void)dev;
    (void)flags;
    if (!started) {
        int32_t a =
            debugfs_create_file("sync/sw_sync", 0644u, &sw_sync_unbound, sw_sync_bind, NULL);
        int32_t b = debugfs_create_text("sync/info", 0444u, &sync_info_ops, NULL);
        int32_t c = debugfs_create_text("files/creators", 0400u, &file_creators_ops, NULL);

        if (a != 0 || b != 0 || c != 0) {
            debugfs_remove("sync");
            debugfs_remove("files");
            return -ENOMEM;
        }
        apply_options(&opts);
        started = 1;
    }
    mnt->priv = NULL;
    mnt->root = &root;
    mnt->show_options = debugfs_options;
    root.refs++;
    return 0;
}
