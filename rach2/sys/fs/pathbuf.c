/*
 * pathbuf.c: the pages the system calls copy their paths into, and the
 * working directory's path. The contract is in pathbuf.h.
 */

#include "pathbuf.h"

#include "vfs.h"
#include "../klog.h"
#include "../lib/kerrno.h"
#include "../lib/kstring.h"
#include "../lib/spinlock.h"
#include "../mm/kheap.h"
#include "../mm/paging.h"
#include "../proc/process.h"

/* A path buffer is one page from `kalloc_pages`, which has no header. */
typedef char pathbuf_is_one_page[VFS_PATH_MAX == PAGE_SIZE ? 1 : -1];

/**
 * A free page of the caller's set, or a new one added to it.
 *
 * The pages stay with the record once taken, so a process pays a frame for
 * the most paths one of its calls used at once and not for every call. The
 * deepest caller takes seven: an openat2's path, the path its walk reached,
 * the directory it starts in and that joined to the path, the target of a
 * link the resolver follows with the path it joins it to, and the path of
 * /dev, /proc or /sys the walk classifies (`synth_rest`). A call that
 * finds all `PATH_BUFS` in use has broken that count; it gets NULL, and
 * the kernel log a line.
 */
char *path_buf_get(void)
{
    process_t *p = current_proc;
    char *buf;
    int i;

    if (!p) {
        return kalloc_pages(1, NULL);
    }
    for (i = 0; i < PATH_BUFS; i++) {
        if (p->path_bufs[i] && !(p->path_bufs_busy & (1u << i))) {
            p->path_bufs_busy |= 1u << i;
            return p->path_bufs[i];
        }
    }
    for (i = 0; i < PATH_BUFS; i++) {
        if (!p->path_bufs[i]) {
            buf = kalloc_pages(1, NULL);
            if (!buf) {
                return NULL;
            }
            p->path_bufs[i] = buf;
            p->path_bufs_busy |= 1u << i;
            return buf;
        }
    }
    klog(KLOG_ERR, "path", "pid %d: all %d path pages in use", (int)p->pid, PATH_BUFS);
    return NULL;
}

/** Release the mount retained while a path page was in use. */
static void path_mount_put(process_t *p, int i)
{
    struct vfs_mount *mnt = p->path_mount[i];

    if (mnt) {
        p->path_mount[i] = NULL;
        p->paths_on_mounts--;
        vfs_desc_close(mnt, p->path_node[i], NULL);
        p->path_node[i] = NULL;
        p->path_prefix[i] = 0;
    }
}

int path_buf_set_dir(char *buf, struct vfs_mount *mnt, void *node, const char *base)
{
    process_t *p = current_proc;
    struct vfs_ref ref;
    int i, task;

    if (!p || (!p->paths_on_mounts && (!mnt || mnt->type != &vfs_procfs_type)))
        return 0;
    for (i = 0; i < PATH_BUFS; i++) {
        if (p->path_bufs[i] != buf)
            continue;
        ref.mnt = mnt;
        ref.node = node;
        task = mnt && node && base && vfs_proc_dir_owner(&ref);
        if (mnt && !task)
            ref.node = mnt->root;
        if (mnt && mnt->type == &vfs_procfs_type) {
            vfs_ref_hold(&ref);
            vfs_desc_open(mnt);
        }
        path_mount_put(p, i);
        if (!mnt || mnt->type != &vfs_procfs_type)
            return 0;
        p->path_mount[i] = mnt;
        p->path_node[i] = ref.node;
        if (task) {
            while (*base == '/')
                base++;
            p->path_prefix[i] = (uint32_t)k_strlen(base) + 1;
        }
        p->paths_on_mounts++;
        return task;
    }
    return 0;
}

void path_buf_set_mount(char *buf, struct vfs_mount *mnt)
{
    path_buf_set_dir(buf, mnt, NULL, NULL);
}

/** Locate a held mount in the caller's root, or in the whole mount table
 * for a directory marked as outside that root. Return its visible prefix. */
static const char *mount_prefix(const process_t *p, const struct vfs_mount *mnt, const char *path)
{
    char root[VFS_COVERED_PATH_MAX];
    struct vfs_ref ref;
    const char *place = mnt->covered_path;
    size_t len;
    int rc;

    if (!p->fs->root_mnt || (path[0] == '/' && path[1] == '/')) {
        return place;
    }
    ref.mnt = p->fs->root_mnt;
    ref.node = p->fs->root_node;
    if (vfs_enter(ref.mnt) != 0) {
        return NULL;
    }
    vfs_ref_hold(&ref);
    rc = vfs_node_path(ref.mnt, ref.node, root, sizeof(root));
    vfs_ref_release(&ref);
    if (rc != 0) {
        return NULL;
    }
    if (root[0] == '/' && !root[1]) {
        return place;
    }
    len = 0;
    while (root[len] && place[len] == root[len]) {
        len++;
    }
    if (root[len] || (place[len] && place[len] != '/')) {
        return NULL;
    }
    return place + len;
}

struct vfs_mount *path_buf_mount(const char **path, void **node)
{
    process_t *p = current_proc;
    int i;

    if (!p || !p->paths_on_mounts || !*path) {
        return NULL;
    }
    for (i = 0; i < PATH_BUFS; i++) {
        struct vfs_mount *m = p->path_mount[i];
        const char *s = *path, *prefix;

        if (!m || (s != p->path_bufs[i] && s != p->path_bufs[i] + 1)) {
            continue;
        }
        if (p->path_prefix[i]) {
            uint32_t len = p->path_prefix[i] - 1;
            while (*s == '/')
                s++;
            if (k_strlen(s) < len || (len && s[len] && s[len] != '/'))
                return NULL;
            s += len;
            while (*s == '/')
                s++;
            *path = s;
            *node = p->path_node[i];
            return m;
        }
        prefix = mount_prefix(p, m, s);
        if (!prefix) {
            return NULL;
        }
        while (*prefix == '/') {
            prefix++;
        }
        while (*s == '/') {
            s++;
        }
        if (*prefix) {
            while (*prefix && *prefix == *s) {
                prefix++;
                s++;
            }
            if (*prefix || (*s && *s != '/')) {
                return NULL;
            }
        }
        while (*s == '/') {
            s++;
        }
        *path = s;
        *node = m->root;
        return m;
    }
    return NULL;
}

void path_buf_put(char *buf)
{
    process_t *p = current_proc;
    int i;

    if (!buf) {
        return;
    }
    if (p) {
        for (i = 0; i < PATH_BUFS; i++) {
            if (p->path_bufs[i] == buf) {
                path_mount_put(p, i);
                p->path_bufs_busy &= ~(1u << i);
                return;
            }
        }
    }
    kfree_pages(buf, 1);
}

const char *proc_cwd(const struct process *p)
{
    return p->fs->cwd_long ? p->fs->cwd_long : p->fs->cwd;
}

int proc_cwd_read(const struct process *p, char *out)
{
    uint32_t flags = spin_lock_irqsave(&p->fs->lock);
    const char *cwd = proc_cwd(p);
    int proc = p->fs->cwd_mnt && p->fs->cwd_mnt->type == &vfs_procfs_type;

    k_memcpy(out, cwd, k_strlen(cwd) + 1);
    spin_unlock_irqrestore(&p->fs->lock, flags);
    return proc;
}

/**
 * Store the working directory's path: in the record when it fits `cwd`,
 * which almost every path does, and in a string from the heap when it is
 * longer. The new copy is made before the old one is freed, so `path` may
 * point at either, and the switch is made under the `fs_struct`'s lock, so
 * that `proc_cwd_read` reads one path or the other whole.
 */
int proc_cwd_set(struct process *p, const char *path)
{
    uint32_t len = (uint32_t)k_strlen(path);
    char *heap = NULL, *old;
    uint32_t flags;

    if (len + 1 > VFS_PATH_MAX) {
        return -ENAMETOOLONG;
    }
    if (len + 1 > sizeof(p->fs->cwd)) {
        heap = kmalloc(len + 1);
        if (!heap) {
            return -ENOMEM;
        }
        k_memcpy(heap, path, len + 1);
    }
    flags = spin_lock_irqsave(&p->fs->lock);
    if (!heap) {
        k_memmove(p->fs->cwd, path, len + 1);
    }
    old = p->fs->cwd_long;
    p->fs->cwd_long = heap;
    spin_unlock_irqrestore(&p->fs->lock, flags);
    kfree(old);
    return 0;
}

int proc_cwd_copy(struct process *to, const struct process *from)
{
    return proc_cwd_set(to, proc_cwd(from));
}

void path_bufs_free(struct process *p)
{
    int i;

    for (i = 0; i < PATH_BUFS; i++) {
        if (p->path_bufs[i]) {
            path_mount_put(p, i);
            kfree_pages(p->path_bufs[i], 1);
            p->path_bufs[i] = NULL;
        }
    }
    p->path_bufs_busy = 0;
}
