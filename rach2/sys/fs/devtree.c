/* The directories and symbolic links that root makes under /dev, as Linux's
 * devtmpfs keeps what udev makes there (devtree.h). */
#include "devtree.h"

#include "../irq.h"
#include "fd.h"
#include "krffs.h"
#include "synthfs.h"
#include "../mm/kheap.h"
#include "../mm/paging.h"
#include "../proc/process.h"
#include "../lib/kerrno.h"
#include "../lib/kstring.h"
#include "../lib/kuser.h"
#include "../syscall_internal.h"

#define DT_IFDIR 0040000u
#define DT_IFLNK 0120000u
#define DT_IFMT  0170000u

/* An entry: a node of the list of all of them, in the order they were made. */
struct devtree_node {
    struct devtree_node *next;
    int id;        /* above 0, never given twice */
    int parent;    /* DEVTREE_ROOT, DEVTREE_ROW(r) or a directory's id */
    uint32_t mode; /* DT_IFDIR or DT_IFLNK and the permission bits */
    uint32_t uid;
    uint32_t gid;
    char *name;   /* from the heap, NUL-terminated */
    char *target; /* a link's text from the heap, NULL for a directory */
    int64_t atime, mtime, ctime;
    int64_t atime_ns, mtime_ns, ctime_ns;
};

static struct devtree_node *nodes;
static struct devtree_node **tail = &nodes;
static int next_id = 1;
static int count;

/** The entry `id`, or NULL. */
static struct devtree_node *by_id(int id)
{
    struct devtree_node *n;

    for (n = nodes; n; n = n->next) {
        if (n->id == id) {
            return n;
        }
    }
    return 0;
}

/** Whether the `len` bytes at `s` are the string `name`. */
static int same_name(const char *name, const char *s, uint32_t len)
{
    uint32_t i;

    for (i = 0; i < len; i++) {
        if (name[i] != s[i] || name[i] == '\0') {
            return 0;
        }
    }
    return name[len] == '\0';
}

/** The entry `len` bytes at `name` names in `parent`, or NULL. */
static struct devtree_node *find(int parent, const char *name, uint32_t len)
{
    struct devtree_node *n;

    for (n = nodes; n; n = n->next) {
        if (n->parent == parent && same_name(n->name, name, len)) {
            return n;
        }
    }
    return 0;
}

/** Whether `parent` is /dev, a row of its table, or a directory here. */
static int parent_valid(int parent)
{
    struct devtree_node *d;

    if (parent == DEVTREE_ROOT) {
        return 1;
    }
    if (parent < 0) {
        return synth_dev_dir_at(DEVTREE_ROW(0) - parent) != 0;
    }
    d = by_id(parent);
    return d && (d->mode & DT_IFMT) == DT_IFDIR;
}

/** Whether `parent` has an entry. */
static int has_children(int parent)
{
    struct devtree_node *n;

    for (n = nodes; n; n = n->next) {
        if (n->parent == parent) {
            return 1;
        }
    }
    return 0;
}

/** Set the times of `n` that `which` chooses, 1 for access, 2 for
 * modification and 4 for change, to now. */
static void stamp(struct devtree_node *n, int which)
{
    int64_t s, ns;

    get_current_time(&s, &ns);
    if (which & 1) {
        n->atime = s;
        n->atime_ns = ns;
    }
    if (which & 2) {
        n->mtime = s;
        n->mtime_ns = ns;
    }
    if (which & 4) {
        n->ctime = s;
        n->ctime_ns = ns;
    }
}

/** Stamp the modification and change times of the directory `parent` when
 * it is one made here. */
static void stamp_dir(int parent)
{
    struct devtree_node *d = parent > 0 ? by_id(parent) : 0;

    if (d) {
        stamp(d, 6);
    }
}

/** Take `n` off the list and free it. */
static void drop(struct devtree_node *n)
{
    struct devtree_node **link;

    for (link = &nodes; *link && *link != n; link = &(*link)->next) {
    }
    if (*link) {
        *link = n->next;
    }
    if (tail == &n->next) {
        tail = link;
    }
    kfree(n->name);
    kfree(n->target);
    kfree(n);
    count--;
}

/** A copy of the `len` bytes at `s` from the heap, NUL-terminated, or NULL. */
static char *copy_text(const char *s, uint32_t len)
{
    char *c = kmalloc(len + 1);

    if (c) {
        k_memcpy(c, s, len);
        c[len] = '\0';
    }
    return c;
}

int devtree_child(int parent, const char *name, uint32_t len)
{
    struct devtree_node *n = find(parent, name, len);

    return n ? n->id : 0;
}

int devtree_resolve(const char *rel, uint32_t len)
{
    const char *p = rel, *end = rel + len, *comp;
    struct devtree_node *n = 0;
    int parent = DEVTREE_ROOT, row;
    uint32_t clen;

    while (p < end) {
        while (p < end && *p == '/') {
            p++;
        }
        if (p == end) {
            break;
        }
        comp = p;
        while (p < end && *p != '/') {
            p++;
        }
        clen = (uint32_t)(p - comp);
        if (n && (n->mode & DT_IFMT) != DT_IFDIR) {
            return -ENOTDIR;
        }
        if (parent <= DEVTREE_ROOT && !n &&
            (row = synth_dev_dir_child(parent == DEVTREE_ROOT ? -1 : DEVTREE_ROW(0) - parent, comp,
                                       (int)clen)) >= 0) {
            parent = DEVTREE_ROW(row);
            continue;
        }
        n = find(parent, comp, clen);
        if (!n) {
            return -ENOENT;
        }
        parent = n->id;
    }
    return n ? n->id : -ENOENT;
}

int devtree_is_dir(int id)
{
    struct devtree_node *n = by_id(id);

    return !n ? -ENOENT : (n->mode & DT_IFMT) == DT_IFDIR;
}

int devtree_attr(int id, struct devtree_attr *a)
{
    struct devtree_node *n = by_id(id), *c;

    if (!n) {
        return -ENOENT;
    }
    k_memset(a, 0, sizeof(*a));
    a->mode = n->mode;
    a->uid = n->uid;
    a->gid = n->gid;
    a->ino = DEVTREE_INO_BASE + (uint32_t)n->id;
    a->nlink = 1;
    if ((n->mode & DT_IFMT) == DT_IFDIR) {
        a->nlink = 2;
        for (c = nodes; c; c = c->next) {
            if (c->parent == n->id && (c->mode & DT_IFMT) == DT_IFDIR) {
                a->nlink++;
            }
        }
    } else {
        a->size = (uint32_t)k_strlen(n->target);
    }
    a->atime = n->atime;
    a->mtime = n->mtime;
    a->ctime = n->ctime;
    a->atime_ns = n->atime_ns;
    a->mtime_ns = n->mtime_ns;
    a->ctime_ns = n->ctime_ns;
    return 0;
}

int devtree_readlink(int id, char *buf, uint32_t cap)
{
    struct devtree_node *n = by_id(id);
    uint32_t len;

    if (!n) {
        return -ENOENT;
    }
    if (!n->target) {
        return -EINVAL;
    }
    len = (uint32_t)k_strlen(n->target);
    k_memcpy(buf, n->target, len < cap ? len : cap);
    return (int)len;
}

void devtree_path(int id, char *out, int cap)
{
    const struct devtree_node *chain[64];
    const struct devtree_node *n = by_id(id);
    const struct synth_dev_dir *row = 0;
    int depth = 0, pos = 0, i;
    const char *part;

    if (cap <= 0) {
        return;
    }
    while (n && depth < 64) {
        chain[depth++] = n;
        if (n->parent <= 0) {
            break;
        }
        n = by_id(n->parent);
    }
    if (!n || depth == 0 || n->parent > 0) {
        k_strncpy(out, "/", (size_t)cap);
        out[cap - 1] = '\0';
        return;
    }
    if (n->parent < 0) {
        row = synth_dev_dir_at(DEVTREE_ROW(0) - n->parent);
    }
    for (i = -2; i < depth; i++) {
        part = i == -2 ? "/dev" : i == -1 ? (row ? row->name : 0) : chain[depth - 1 - i]->name;
        if (!part) {
            continue;
        }
        if (i != -2 && pos < cap - 1) {
            out[pos++] = '/';
        }
        while (*part && pos < cap - 1) {
            out[pos++] = *part++;
        }
    }
    out[pos] = '\0';
}

int devtree_dirent(int parent, int n, const char **name, uint32_t *ino, uint8_t *dtype)
{
    struct devtree_node *e;

    for (e = nodes; e; e = e->next) {
        if (e->parent == parent && n-- == 0) {
            *name = e->name;
            *ino = DEVTREE_INO_BASE + (uint32_t)e->id;
            *dtype = (uint8_t)(((e->mode & DT_IFMT) >> 12) & 017);
            return 1;
        }
    }
    return 0;
}

int devtree_make(int parent, const char *name, uint32_t mode, const char *target, uint32_t uid,
                 uint32_t gid)
{
    struct devtree_node *n;
    uint32_t len = (uint32_t)k_strlen(name);
    uint32_t tlen = target ? (uint32_t)k_strlen(target) : 0;

    if (len == 0 || (len == 1 && name[0] == '.') ||
        (len == 2 && name[0] == '.' && name[1] == '.')) {
        return -EINVAL;
    }
    if (len > 255 || tlen > 4095) {
        return -ENAMETOOLONG;
    }
    n = kzalloc(sizeof(*n));
    if (n) {
        n->name = copy_text(name, len);
        n->target = target ? copy_text(target, tlen) : 0;
    }
    if (!n || !n->name || (target && !n->target)) {
        if (n) {
            kfree(n->name);
            kfree(n->target);
        }
        kfree(n);
        return -ENOMEM;
    }
    /* The allocations may have slept; what they waited for is read now. */
    if (!parent_valid(parent) || find(parent, name, len) || count >= DEVTREE_MAX) {
        int rc = !parent_valid(parent) ? -ENOENT : count >= DEVTREE_MAX ? -ENOSPC : -EEXIST;

        kfree(n->name);
        kfree(n->target);
        kfree(n);
        return rc;
    }
    n->id = next_id++;
    n->parent = parent;
    n->mode = (target ? DT_IFLNK | 0777u : DT_IFDIR | (mode & 07777u));
    n->uid = uid;
    n->gid = gid;
    stamp(n, 7);
    *tail = n;
    tail = &n->next;
    count++;
    stamp_dir(parent);
    return n->id;
}

int devtree_remove(int parent, const char *name, int dir)
{
    struct devtree_node *n = find(parent, name, (uint32_t)k_strlen(name));

    if (!n) {
        return -ENOENT;
    }
    if (dir && (n->mode & DT_IFMT) != DT_IFDIR) {
        return -ENOTDIR;
    }
    if (!dir && (n->mode & DT_IFMT) == DT_IFDIR) {
        return -EISDIR;
    }
    if (dir && has_children(n->id)) {
        return -ENOTEMPTY;
    }
    drop(n);
    stamp_dir(parent);
    return 0;
}

/** Whether the directory `id` is `to` or one of its ancestors. */
static int above(int id, int to)
{
    struct devtree_node *d;

    while (to > 0) {
        if (to == id) {
            return 1;
        }
        d = by_id(to);
        to = d ? d->parent : DEVTREE_ROOT;
    }
    return 0;
}

/**
 * What rename(2) answers for moving `src` into `to` over `dst`, either of
 * them NULL for none, before anything moves: 1 to go on, 0 for a source
 * that is its own destination, which moves nothing, or the negative errno
 * `devtree_rename` lists.
 */
static int rename_refusal(const struct devtree_node *src, int to, const struct devtree_node *dst,
                          int noreplace)
{
    int isdir = src && (src->mode & DT_IFMT) == DT_IFDIR;

    if (!src || !parent_valid(to)) {
        return -ENOENT;
    }
    if (src == dst) {
        return 0;
    }
    if (isdir && above(src->id, to)) {
        return -EINVAL;
    }
    if (!dst) {
        return 1;
    }
    if (noreplace) {
        return -EEXIST;
    }
    if (isdir != ((dst->mode & DT_IFMT) == DT_IFDIR)) {
        return isdir ? -ENOTDIR : -EISDIR;
    }
    return isdir && has_children(dst->id) ? -ENOTEMPTY : 1;
}

int devtree_rename(int from, const char *oname, int to, const char *nname, int noreplace)
{
    uint32_t nlen = (uint32_t)k_strlen(nname);
    struct devtree_node *src, *dst;
    char *copy;
    int rc;

    if (nlen > 255) {
        return -ENAMETOOLONG;
    }
    copy = copy_text(nname, nlen);
    if (!copy) {
        return -ENOMEM;
    }
    src = find(from, oname, (uint32_t)k_strlen(oname));
    dst = find(to, nname, nlen);
    rc = rename_refusal(src, to, dst, noreplace);
    if (rc != 1) {
        kfree(copy);
        return rc;
    }
    if (dst) {
        drop(dst);
    }
    kfree(src->name);
    src->name = copy;
    src->parent = to;
    stamp(src, 4);
    stamp_dir(from);
    stamp_dir(to);
    return 0;
}

int devtree_setattr(int id, uint32_t mode, uint32_t uid, uint32_t gid)
{
    struct devtree_node *n = by_id(id);

    if (!n) {
        return -ENOENT;
    }
    if (mode != (uint32_t)-1) {
        n->mode = (n->mode & DT_IFMT) | (mode & 07777u);
    }
    if (uid != (uint32_t)-1) {
        n->uid = uid;
    }
    if (gid != (uint32_t)-1) {
        n->gid = gid;
    }
    stamp(n, 4);
    return 0;
}
