/*
 * pathbuf.c: the pages the system calls copy their paths into, and the
 * working directory's path. The contract is in pathbuf.h.
 */

#include "pathbuf.h"

#include "vfs.h"
#include "../klog.h"
#include "../lib/kerrno.h"
#include "../lib/kstring.h"
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
 * deepest caller takes four: a rename's two paths, and the target of a link
 * the resolver follows with the path it joins it to. A call that finds all
 * `PATH_BUFS` in use has broken that count, and is refused with a record.
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

/**
 * Store the working directory's path: in the record when it fits `cwd`,
 * which almost every path does, and in a string from the heap when it is
 * longer. The new copy is made before the old one is freed, so `path` may
 * point at either.
 */
int proc_cwd_set(struct process *p, const char *path)
{
    uint32_t len = k_strlen(path);
    char *heap = NULL;

    if (len + 1 > VFS_PATH_MAX) {
        return -ENAMETOOLONG;
    }
    if (len + 1 > sizeof(p->fs->cwd)) {
        heap = kmalloc(len + 1);
        if (!heap) {
            return -ENOMEM;
        }
        k_memcpy(heap, path, len + 1);
    } else {
        k_memmove(p->fs->cwd, path, len + 1);
    }
    kfree(p->fs->cwd_long);
    p->fs->cwd_long = heap;
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
            kfree_pages(p->path_bufs[i], 1);
            p->path_bufs[i] = NULL;
        }
    }
    p->path_bufs_busy = 0;
}
