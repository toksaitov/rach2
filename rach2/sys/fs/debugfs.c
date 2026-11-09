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

/*
 * A directory or a file. A directory's children are a list in the order
 * they were made, which is the order it lists them. `refs` counts the
 * holds of the walks and of the descriptions open on the node; a node taken
 * out (`removed`) is off its parent's list and is freed by its last put.
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
    const struct file_ops *ops; /* a file's */
    debugfs_open_fn open;       /* a file's, or NULL */
    void *data;                 /* a file's registrant's, its descriptors' `priv` */
    int64_t time_s, time_ns;    /* when it was made: its access, change and modification */
};

/* The root directory, which lasts the life of the kernel. Linux's default
 * mode is 0700, root's: no one else may look below /sys/kernel/debug. */
static struct debugfs_node root = {"",   &root, NULL, NULL, DFS_IFDIR | 0700u, 0, 0, 1, 1, 0, NULL,
                                   NULL, NULL,  0,    0};
static uint64_t last_ino = 1;
static int started;

static int32_t sync_info_open(fd_entry_t *e, int flags);

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
 * now; NULL when the heap has no room. */
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
    get_current_time(&n->time_s, &n->time_ns);
    for (link = &d->children; *link; link = &(*link)->next) {
    }
    *link = n;
    return n;
}

/** Free `n` once it is taken out and nothing holds it. */
static void settle(struct debugfs_node *n)
{
    if (n != &root && n->removed && n->refs == 0 && !n->children) {
        kfree(n);
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

int32_t debugfs_create_file(const char *path, uint32_t mode, const struct file_ops *ops,
                            debugfs_open_fn open, void *data)
{
    struct debugfs_node *d = &root, *n;
    const char *name, *rest;
    int len;

    if (!ops || !path || !next_name(path, &len)) {
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
    n = make(d, name, (uint32_t)(rest - name), DFS_IFREG | (mode & 07777u));
    if (!n) {
        return -ENOMEM;
    }
    n->ops = ops;
    n->open = open;
    n->data = data;
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

int32_t debugfs_bind(void *node, fd_entry_t *e, int flags)
{
    struct debugfs_node *n = node;
    int32_t rc;

    e->ops = n->ops;
    e->priv = n->data;
    rc = n->open ? n->open(e, flags) : 0;
    if (rc != 0) {
        e->ops = NULL;
        e->priv = NULL;
    }
    return rc;
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
 * subdirectory, a file one, and both a size of 0, as Linux's debugfs has. */
static int dfs_getattr(struct vfs_mount *mnt, void *node, struct vfs_stat *st)
{
    struct debugfs_node *n = node_of(node), *c;

    (void)mnt;
    k_memset(st, 0, sizeof(*st));
    st->mode = n->mode;
    st->uid = n->uid;
    st->gid = n->gid;
    st->nlink = 1;
    if (is_dir(n)) {
        st->nlink = n->removed ? 0 : 2;
        for (c = n->children; c; c = c->next) {
            st->nlink += is_dir(c);
        }
    }
    st->ino = n->ino;
    st->atime_sec = st->mtime_sec = st->ctime_sec = n->time_s;
    st->atime_nsec = st->mtime_nsec = st->ctime_nsec = n->time_ns;
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
    for (c = d->children, i = 2; c && i < *cursor; c = c->next, i++) {
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

const struct vfs_ops debugfs_vops = {
    dfs_lookup,  0,                       /* lookup_rest */
    dfs_hold,    dfs_put, dfs_getattr, 0, /* setattr */
    0,                                    /* open */
    0,                                    /* release */
    0,                                    /* read: the descriptor's operations read a file */
    0,                                    /* write */
    0,                                    /* truncate */
    0,                                    /* fsync */
    dfs_readdir, 0,                       /* create */
    0,                                    /* unlink */
    0,                                    /* mkdir */
    0,                                    /* rmdir */
    0,                                    /* rename */
    0,                                    /* link */
    0,                                    /* readlink */
    dfs_statfs,  0,                       /* sync */
    0,                                    /* prepare_unmount */
    0,                                    /* abort_unmount */
    0,                                    /* unmount */
    dfs_path,    0,                       /* symlink */
    0,                                    /* mknod */
    0,                                    /* pin */
    0,                                    /* unpin */
    0,                                    /* remount */
    0,                                    /* fallocate */
    0,                                    /* fdatasync */
    0,                                    /* lock_node */
    0,                                    /* unlock_node */
    0,                                    /* shutdown */
};

/** The number at the start of `s` in `base`, 8 or 10, its end in `*end`. */
static uint32_t option_number(const char *s, uint32_t base, const char **end)
{
    uint32_t v = 0;

    while (*s >= '0' && *s < (char)('0' + base)) {
        v = v * base + (uint32_t)(*s - '0');
        s++;
    }
    *end = s;
    return v;
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

/** Apply Linux's root options `mode=`, `uid=` and `gid=` of `options`. */
static void root_options(const char *options)
{
    const char *s = options, *end;
    uint32_t v;

    while (s && *s) {
        if (option_is(s, "mode=")) {
            v = option_number(s + 5, 8, &end);
            if (end != s + 5) {
                root.mode = DFS_IFDIR | (v & 07777u);
            }
        } else if (option_is(s, "uid=")) {
            v = option_number(s + 4, 10, &end);
            if (end != s + 4) {
                root.uid = v;
            }
        } else if (option_is(s, "gid=")) {
            v = option_number(s + 4, 10, &end);
            if (end != s + 4) {
                root.gid = v;
            }
        }
        while (*s && *s != ',') {
            s++;
        }
        s += *s == ',';
    }
}

/* The operations of sync/info, a text made at the open (`sync_info_open`):
 * a `struct dfs_text` in the descriptor's `priv`, read at the description's
 * offset. */
struct dfs_text {
    uint32_t len;
    char text[1];
};

/** Read the text from the description's offset; bytes read, 0 at its end,
 * or -EFAULT. */
static int32_t text_read(fd_entry_t *e, char __user *buf, uint32_t count)
{
    struct dfs_text *t = e->priv;
    uint64_t off = e->ofd->offset;
    uint32_t n;

    if (off >= t->len) {
        return 0;
    }
    n = t->len - (uint32_t)off;
    n = n < count ? n : count;
    if (copy_to_user(buf, t->text + off, n) != 0) {
        return -EFAULT;
    }
    e->ofd->offset = off + n;
    return (int32_t)n;
}

/** Move the description's offset in the text: SEEK_SET, SEEK_CUR and
 * SEEK_END, -EINVAL for a negative result or another `whence`. */
static int64_t text_llseek(fd_entry_t *e, int64_t offset, int whence)
{
    struct dfs_text *t = e->priv;
    int64_t base = whence == 0 ? 0 : whence == 1 ? (int64_t)e->ofd->offset : (int64_t)t->len;

    if (whence < 0 || whence > 2 || base + offset < 0) {
        return -EINVAL;
    }
    return base + offset;
}

/** Free the text at the last close. */
static void text_close(fd_entry_t *e)
{
    kfree(e->priv);
    e->priv = NULL;
}

static const struct file_ops text_ops = {text_read,  NULL, NULL,       NULL,
                                         text_close, NULL, text_llseek};

/** Make sync/info's text, the software timelines and their points as
 * `sw_sync_info` writes them, for the descriptor `e`. 0, or -ENOMEM. */
static int32_t sync_info_open(fd_entry_t *e, int flags)
{
    uint32_t len = sw_sync_info(NULL, 0);
    struct dfs_text *t = kmalloc(sizeof(*t) + len);

    (void)flags;
    if (!t) {
        return -ENOMEM;
    }
    t->len = sw_sync_info(t->text, len + 1);
    e->priv = t;
    return 0;
}

/** Dispatch sw_sync's open, whose descriptor is a timeline of its own. */
static int32_t sw_sync_bind(fd_entry_t *e, int flags)
{
    (void)flags;
    return sw_sync_open(e);
}

/* sync/sw_sync's operations until `sw_sync_open` replaces them. */
static const struct file_ops sw_sync_unbound = {NULL, NULL, NULL, NULL, NULL, NULL, NULL};

int debugfs_mount(struct vfs_mount *mnt, block_device_t *dev, uint32_t flags, const char *options)
{
    (void)dev;
    (void)flags;
    if (!started) {
        int32_t a =
            debugfs_create_file("sync/sw_sync", 0644u, &sw_sync_unbound, sw_sync_bind, NULL);
        int32_t b = debugfs_create_file("sync/info", 0444u, &text_ops, sync_info_open, NULL);

        if (a != 0 || b != 0) {
            debugfs_remove("sync");
            return -ENOMEM;
        }
        started = 1;
    }
    root_options(options);
    mnt->priv = NULL;
    mnt->root = &root;
    root.refs++;
    return 0;
}
