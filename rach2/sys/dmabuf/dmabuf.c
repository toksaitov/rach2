/* Exported buffers, implicit reservations and their anonymous descriptors. */
#include "dmabuf.h"
#include "../mm/kheap.h"
#include "../mm/usercopy.h"
#include "../lib/kerrno.h"
#include "../lib/kstring.h"
#include "../proc/mutex.h"

/** Make `r` empty, with its lock, of one class for every reservation. */
void dma_resv_init(struct dma_resv *r)
{
    k_memset(r, 0, sizeof(*r));
    mutex_init(&r->lock, "dma_resv");
}

/** Take the reservation's lock; may sleep. */
void dma_resv_lock(struct dma_resv *r)
{
    mutex_lock(&r->lock);
}

int dma_resv_trylock(struct dma_resv *r)
{
    return mutex_trylock(&r->lock);
}

/** Give the reservation's lock back. */
void dma_resv_unlock(struct dma_resv *r)
{
    mutex_unlock(&r->lock);
}

/** Drop every reservation reference and return it to its empty state, for
 * its last user, which needs no lock; the lock stays made. */
void dma_resv_fini(struct dma_resv *r)
{
    uint32_t i;
    dma_fence_put(r->write);
    for (i = 0; i < r->nreads; i++)
        dma_fence_put(r->reads[i]);
    kfree(r->reads);
    r->write = NULL;
    r->reads = NULL;
    r->nreads = 0;
    r->cap = 0;
}

/** Whether `f` takes the place of `old` in a reservation, as Linux's
 * dma_resv_add_fence replaces a fence that has signalled, or one of the
 * same context that `f` is or comes after by its producer's ordering,
 * dma_fence_is_later_or_same (dma-resv.c:306-317), so that a 32-bit
 * timeline's wrap never drops a pending point: a single fence, not an
 * array or a chain's point, whose leaves may be of other contexts. */
static int resv_replaces(struct dma_fence *old, struct dma_fence *f)
{
    if (dma_fence_is_signaled(old))
        return 1;
    return old->context == f->context && !dma_fence_is_container(old) &&
           !dma_fence_is_container(f) && dma_fence_is_later_or_same(f, old);
}

/**
 * Make `f` a writer or a reader of `r`, with the lock held, as Linux's
 * dma_resv_add_fence (dma-resv.c:287-326): a fence it replaces goes, so
 * that a timeline's newer point takes its older one's place with nothing
 * made; a writer that does not replace the one before, or holds other
 * fences, is merged with it, and a reader that replaces none is added to
 * the readers. 0, -EINVAL for no fence, or -ENOMEM.
 */
int32_t dma_resv_add_fence_locked(struct dma_resv *r, struct dma_fence *f, int write)
{
    struct dma_fence **p, *inputs[2], *merged;
    uint32_t i, n;
    int placed = 0;
    lockdep_assert_held(&r->lock);
    if (!f)
        return -EINVAL;
    if (write) {
        /* A container is flattened by the merge, as the writer always
         * was, so that the writer is never a chain's point. */
        if (!dma_fence_is_container(f) && (!r->write || resv_replaces(r->write, f))) {
            merged = dma_fence_get(f);
        } else {
            inputs[0] = dma_fence_is_signaled(r->write) ? NULL : r->write;
            inputs[1] = f;
            merged = dma_fence_merge(inputs, 2);
            if (!merged)
                return -ENOMEM;
        }
        dma_fence_put(r->write);
        r->write = merged;
        return 0;
    }
    for (i = 0; i < r->nreads;) {
        if (!resv_replaces(r->reads[i], f)) {
            i++;
        } else if (!placed) {
            /* The take before the put, since the old may be `f`. */
            merged = r->reads[i];
            r->reads[i++] = dma_fence_get(f);
            dma_fence_put(merged);
            placed = 1;
        } else {
            dma_fence_put(r->reads[i]);
            r->reads[i] = r->reads[--r->nreads];
        }
    }
    if (placed)
        return 0;
    if (r->nreads == r->cap) {
        n = r->cap ? r->cap * 2 : 4;
        if (n < r->cap || (size_t)n * sizeof(*p) / sizeof(*p) != n)
            return -ENOMEM;
        p = kmalloc((size_t)n * sizeof(*p));
        if (!p)
            return -ENOMEM;
        if (r->nreads)
            k_memcpy(p, r->reads, r->nreads * sizeof(*p));
        kfree(r->reads);
        r->reads = p;
        r->cap = n;
    }
    r->reads[r->nreads++] = dma_fence_get(f);
    return 0;
}

/** `dma_resv_add_fence_locked` under the reservation's lock. */
int32_t dma_resv_add_fence(struct dma_resv *r, struct dma_fence *f, int write)
{
    int32_t rc;
    dma_resv_lock(r);
    rc = dma_resv_add_fence_locked(r, f, write);
    dma_resv_unlock(r);
    return rc;
}

/** Snapshot work that must finish before a read or a write may start, with
 * the lock held. */
struct dma_fence *dma_resv_snapshot_locked(struct dma_resv *r, int write)
{
    struct dma_fence **p, *f;
    uint32_t i, n = write ? r->nreads : 0;
    lockdep_assert_held(&r->lock);
    if (n == 0xffffffffu || ((size_t)n + 1) * sizeof(*p) / sizeof(*p) != (size_t)n + 1)
        return NULL;
    p = kmalloc(((size_t)n + 1) * sizeof(*p));
    if (!p)
        return NULL;
    p[0] = r->write;
    for (i = 0; i < n; i++)
        p[i + 1] = r->reads[i];
    f = dma_fence_merge(p, n + 1);
    kfree(p);
    return f;
}

/** `dma_resv_snapshot_locked` under the reservation's lock. */
struct dma_fence *dma_resv_snapshot(struct dma_resv *r, int write)
{
    struct dma_fence *f;
    dma_resv_lock(r);
    f = dma_resv_snapshot_locked(r, write);
    dma_resv_unlock(r);
    return f;
}

/**
 * The fences of `r` now, each with a reference, under its lock, as Linux's
 * dma_resv_get_fences (dma-resv.c:569-617), which takes them by a read
 * that restarts when a writer changes the reservation meanwhile; without
 * RCU the lock is taken for the copy instead. `*fences` is the write
 * fence, which may be NULL, and with `all` the reads after it, `*count`
 * in all. 0, or -ENOMEM with nothing taken.
 */
int32_t dma_resv_get_fences(struct dma_resv *r, int all, struct dma_fence ***fences,
                            uint32_t *count)
{
    struct dma_fence **p;
    uint32_t i, n;
    dma_resv_lock(r);
    n = all ? r->nreads + 1u : 1u;
    /* A count whose array's size would wrap is refused. */
    p = n != 0 && n <= 0xffffffffu / sizeof(*p) ? kmalloc((size_t)n * sizeof(*p)) : NULL;
    if (!p) {
        dma_resv_unlock(r);
        return -ENOMEM;
    }
    p[0] = dma_fence_get(r->write);
    for (i = 1; i < n; i++)
        p[i] = dma_fence_get(r->reads[i - 1]);
    dma_resv_unlock(r);
    *fences = p;
    *count = n;
    return 0;
}

/** Drop the references `dma_resv_get_fences` took and free its array. */
void dma_resv_put_fences(struct dma_fence **fences, uint32_t count)
{
    uint32_t i;
    for (i = 0; i < count; i++)
        dma_fence_put(fences[i]);
    kfree(fences);
}

/** The reservation's write fence, with a reference, or NULL. */
struct dma_fence *dma_resv_get_write(struct dma_resv *r)
{
    struct dma_fence *f;
    dma_resv_lock(r);
    f = dma_fence_get(r->write);
    dma_resv_unlock(r);
    return f;
}

/** Add `delta` to `*refs` atomically and return the old count. */
static uint32_t refs_add(volatile uint32_t *refs, uint32_t delta)
{
    uint32_t old;
    do {
        old = *refs;
    } while (arch_atomic_cmpxchg((volatile unsigned int *)refs, old, old + delta) != old);
    return old;
}

/** Take a buffer reference, atomically, as Linux's get_file on the
 * buffer's file, since PRIME takes and drops them on any processor. */
struct dma_buf *dma_buf_ref(struct dma_buf *b)
{
    if (b)
        refs_add(&b->refs, 1u);
    return b;
}

/** Release completion watches and exporter storage after the last holder. */
void dma_buf_put(struct dma_buf *b)
{
    int i;
    if (!b || refs_add(&b->refs, ~0u) != 1u)
        return;
    for (i = 0; i < 2; i++) {
        dma_fence_remove_callback(b->poll_fences[i], &b->poll_cb[i].cb);
        dma_fence_put(b->poll_fences[i]);
    }
    if (b->ops->release)
        b->ops->release(b);
    if (b->resv == &b->own)
        dma_resv_fini(&b->own);
    kfree(b);
}

/** Make a buffer with one reference and a distinct stat inode. */
struct dma_buf *dma_buf_export(uint64_t size, const struct dma_buf_ops *ops, void *priv,
                               struct dma_resv *resv)
{
    static uint64_t next_ino = 1;
    static struct spinlock ino_lock = SPINLOCK_INIT_NAMED("dma_buf_ino");
    uint32_t flags;
    struct dma_buf *b;
    if (!size || size > 0x7fffffffffffffffull || !ops)
        return NULL;
    b = kzalloc(sizeof(*b));
    if (!b)
        return NULL;
    b->anon.name = "dmabuf";
    b->refs = 1;
    b->size = size;
    flags = spin_lock_irqsave(&ino_lock);
    b->ino = next_ino++;
    spin_unlock_irqrestore(&ino_lock, flags);
    b->ops = ops;
    b->priv = priv;
    b->resv = resv ? resv : &b->own;
    if (!resv)
        dma_resv_init(&b->own);
    wait_queue_init(&b->wq);
    return b;
}

/** Wake pollers after any watched reservation fence completes. */
static void buf_ready(struct dma_fence *f, struct dma_fence_cb *cb)
{
    struct dma_buf *b = ((struct dma_buf_poll_cb *)cb)->buf;
    (void)f;
    wq_wake_all(&b->wq);
}

/**
 * Add the poll callback to the first leaf of a snapshot that accepts it,
 * as Linux's dma_buf_poll_add_cb (dma-buf.c:234-251): one leaf is
 * listened for, and the next only when the program polls again, so that
 * a poll arms no other producer's signal. Returns that leaf with a
 * reference, or NULL when every leaf has signalled.
 */
static struct dma_fence *buf_poll_add_cb(struct dma_fence *snapshot, struct dma_buf_poll_cb *cb)
{
    struct dma_fence_unwrap u;
    struct dma_fence *leaf;
    for (leaf = dma_fence_unwrap_first(snapshot, &u); leaf; leaf = dma_fence_unwrap_next(&u))
        if (!dma_fence_add_callback(leaf, &cb->cb, buf_ready)) {
            dma_fence_get(leaf);
            dma_fence_unwrap_end(&u);
            return leaf;
        }
    return NULL;
}

/** Poll snapshots of the reservation, arming a callback before sleeping. */
static short buf_poll(fd_entry_t *e, short events, struct wait_queue **wq)
{
    struct dma_buf *b = e->priv;
    struct dma_fence *f;
    short mask = 0;
    int i;
    if (wq)
        *wq = &b->wq;
    for (i = 0; i < 2; i++) {
        if (!(events & (i ? 4 : 1)))
            continue;
        f = dma_resv_snapshot(b->resv, i);
        if (!f) {
            mask |= 8;
            continue;
        }
        dma_fence_remove_callback(b->poll_fences[i], &b->poll_cb[i].cb);
        dma_fence_put(b->poll_fences[i]);
        b->poll_cb[i].buf = b;
        b->poll_fences[i] = buf_poll_add_cb(f, &b->poll_cb[i]);
        dma_fence_put(f);
        if (!b->poll_fences[i])
            mask |= i ? 4 : 1;
    }
    return mask;
}

/** Permit only Linux's dma-buf size query and rewind, from the size, which
 * does not change, so without the big lock (FILE_OPS_SEEK_UNLOCKED), as
 * Linux's dma_buf_llseek takes none (drivers/dma-buf/dma-buf.c:168-191). */
static int64_t buf_seek(fd_entry_t *e, int64_t offset, int whence)
{
    struct dma_buf *b = e->priv;
    if (whence != 0 && whence != 2)
        return -EINVAL;
    if (offset)
        return -EINVAL;
    return whence == 2 ? (int64_t)b->size : 0;
}

/** Map through the exporter after checking the exported extent; EINVAL
 * for an exporter without mmap or a range past the buffer, as Linux. */
static int32_t buf_mmap(fd_entry_t *e, vma_t *vma)
{
    struct dma_buf *b = e->priv;
    uint64_t off = (uint64_t)vma->pgoff << 12;
    if (!b->ops->mmap || off > b->size || vma->end - vma->start > b->size - off)
        return -EINVAL;
    return b->ops->mmap(b, vma);
}

/**
 * Give the exporter CPU access, then, at the start, wait on a reservation
 * snapshot, as Linux's dma_buf_begin_cpu_access and dma_buf_end_cpu_access
 * (dma-buf.c:1415-1423, 1450-1451). The exporter's hook comes first so that
 * it can wait its own way: i915's spins on and boosts its requests, and the
 * wait after it then finds them done. Returns the hook's error without
 * waiting, else the wait's.
 */
static int32_t buf_sync(struct dma_buf *b, uint64_t flags)
{
    struct dma_fence *f;
    int32_t rc;
    if ((flags & ~((uint64_t)DMA_BUF_SYNC_VALID_FLAGS_MASK)) || !(flags & DMA_BUF_SYNC_RW))
        return -EINVAL;
    rc = b->ops->cpu_access ? b->ops->cpu_access(b, flags) : 0;
    if (rc || (flags & DMA_BUF_SYNC_END))
        return rc;
    f = dma_resv_snapshot(b->resv, (flags & DMA_BUF_SYNC_WRITE) != 0);
    if (!f)
        return -ENOMEM;
    rc = dma_fence_wait_until(f, 1, -1);
    dma_fence_put(f);
    return rc;
}

/** Check sync-file import or export access flags. */
static int valid_access(uint32_t flags)
{
    return flags && !(flags & ~(uint32_t)DMA_BUF_SYNC_RW);
}

/** Set the buffer's name, synchronize CPU access, or share fence snapshots. */
static int32_t buf_ioctl(fd_entry_t *e, uint32_t request, uintptr_t arg)
{
    struct dma_buf *b = e->priv;
    struct dma_buf_sync sync;
    struct dma_buf_export_sync_file data;
    struct dma_fence *f;
    int32_t rc;
    if (request == DMA_BUF_SET_NAME_A || request == DMA_BUF_SET_NAME_B) {
        char name[DMA_BUF_NAME_LEN];
        uint32_t i;

        k_memset(name, 0, sizeof(name));
        for (i = 0; i < sizeof(name); i++) {
            if (arg + i < arg || copy_from_user(&name[i], (void __user *)(arg + i), 1))
                return -EFAULT;
            if (name[i] == '\0') {
                k_memcpy(b->name, name, sizeof(name));
                b->named = 1;
                return 0;
            }
        }
        return -EINVAL; /* Linux's strndup_user */
    }
    if (request == DMA_BUF_IOCTL_SYNC) {
        if (copy_from_user(&sync, (void __user *)arg, sizeof(sync)))
            return -EFAULT;
        return buf_sync(b, sync.flags);
    }
    if (request != DMA_BUF_IOCTL_EXPORT_SYNC_FILE && request != DMA_BUF_IOCTL_IMPORT_SYNC_FILE)
        return -ENOTTY;
    if (copy_from_user(&data, (void __user *)arg, sizeof(data)))
        return -EFAULT;
    if (!valid_access(data.flags))
        return -EINVAL;
    if (request == DMA_BUF_IOCTL_IMPORT_SYNC_FILE) {
        f = sync_file_get_fence(data.fd);
        if (!f)
            return -EINVAL;
        rc = dma_resv_add_fence(b->resv, f, (data.flags & DMA_BUF_SYNC_WRITE) != 0);
        dma_fence_put(f);
        if (!rc)
            wq_wake_all(&b->wq);
        return rc;
    }
    f = dma_resv_snapshot(b->resv, (data.flags & DMA_BUF_SYNC_WRITE) != 0);
    if (!f)
        return -ENOMEM;
    rc = sync_file_copy_out(f, NULL, (void __user *)arg, &data, sizeof(data), &data.fd);
    dma_fence_put(f);
    return rc;
}

/** Drop the descriptor's buffer reference. */
static void buf_close(fd_entry_t *e)
{
    dma_buf_put(e->priv);
    e->priv = NULL;
}

static const struct file_ops buf_ops = {NULL,      NULL,     buf_poll, buf_ioctl,
                                        buf_close, buf_mmap, buf_seek, FILE_OPS_SEEK_UNLOCKED};

/** Identify dma-bufs by their operations, including duplicated descriptors. */
int is_dma_buf_file(const fd_entry_t *e)
{
    return e && e->ops == &buf_ops;
}

/** Find and reference a caller's dma-buf descriptor. */
struct dma_buf *dma_buf_get(int32_t fd)
{
    fd_entry_t *e = current_proc ? fd_get(current_proc->fds, fd) : NULL;
    return is_dma_buf_file(e) && !(e->flags & FD_FLAG_PATH) ? dma_buf_ref(e->priv) : NULL;
}

/** Install a read/write descriptor with its own buffer reference. */
int32_t dma_buf_fd(struct dma_buf *b, uint32_t flags)
{
    fd_entry_t *e;
    int32_t fd;
    if (!current_proc || (flags & ~02000000u))
        return -EINVAL;
    fd = fd_alloc(current_proc->fds);
    if (fd < 0)
        return fd;
    e = fd_slot(current_proc->fds, fd);
    e->type = FD_TYPE_ANON;
    e->ops = &buf_ops;
    e->priv = dma_buf_ref(b);
    e->flags = FD_FLAG_READ | (b->read_only ? 0 : FD_FLAG_WRITE) | (flags ? FD_FLAG_CLOEXEC : 0);
    fd_install(current_proc->fds, fd);
    return fd;
}
