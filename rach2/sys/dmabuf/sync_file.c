/* Sync file descriptors carry fences, merge them and report completion. */
#include "dmabuf.h"
#include "../mm/kheap.h"
#include "../mm/usercopy.h"
#include "../lib/kerrno.h"
#include "../lib/kstring.h"

struct sync_file {
    struct anon_obj anon;
    struct dma_fence *fence;
    char name[32];
};

/** Poll a sync file through its fence's own completion wait queue. */
static short sync_poll(fd_entry_t *e, short events, struct wait_queue **wq)
{
    struct sync_file *s = e->priv;
    int32_t status = dma_fence_status(s->fence);
    if (wq)
        *wq = &s->fence->wq;
    return (short)(status ? (events & 1) : 0);
}

/** Release the file's reference on the fence. */
static void sync_close(fd_entry_t *e)
{
    struct sync_file *s = e->priv;
    dma_fence_put(s->fence);
    kfree(s);
    e->priv = NULL;
}

/** Copy a bounded name and always terminate it. */
static void sync_name(char *out, const char *name)
{
    k_strncpy(out, name ? name : "", 32);
    out[31] = 0;
}

/** Report leaf fences, with zero num_fences meaning a count query. */
static int32_t sync_info(struct sync_file *s, uintptr_t arg)
{
    struct sync_file_info info;
    struct sync_fence_info row;
    struct dma_fence *f;
    uint32_t i, n;
    if (copy_from_user(&info, (void __user *)arg, sizeof(info)))
        return -EFAULT;
    if (info.flags || info.pad)
        return -EINVAL;
    n = dma_fence_count(s->fence);
    if (info.num_fences && info.num_fences < n)
        return -EINVAL;
    if (info.num_fences) {
        if (info.sync_fence_info > (uintptr_t)-1 ||
            (uint64_t)n * sizeof(row) > (uintptr_t)-1 - info.sync_fence_info)
            return -EFAULT;
        for (i = 0; i < n; i++) {
            f = dma_fence_at(s->fence, i);
            k_memset(&row, 0, sizeof(row));
            sync_name(row.obj_name, f->ops && f->ops->get_timeline_name
                                        ? f->ops->get_timeline_name(f)
                                        : "dma_fence");
            sync_name(row.driver_name,
                      f->ops && f->ops->get_driver_name ? f->ops->get_driver_name(f) : "rach2");
            row.status = dma_fence_status(f);
            row.timestamp_ns = f->timestamp_ns;
            if (copy_to_user(
                    (void __user *)(uintptr_t)(info.sync_fence_info + (uint64_t)i * sizeof(row)),
                    &row, sizeof(row)))
                return -EFAULT;
        }
    }
    sync_name(info.name, s->name);
    info.status = dma_fence_status(s->fence);
    info.num_fences = n;
    return copy_to_user((void __user *)arg, &info, sizeof(info)) ? -EFAULT : 0;
}

/** Merge active leaves as Linux's sync-file merge does, omitting work
 * already finished and retaining the last completed timestamp if empty. */
static struct dma_fence *sync_merge(struct dma_fence *a, struct dma_fence *b)
{
    struct dma_fence **leaves, *f, *out, *inputs[2];
    uint32_t i, j, n = 0, total;
    int64_t timestamp = 0;
    if (dma_fence_count(a) > 0xffffffffu - dma_fence_count(b))
        return NULL;
    total = dma_fence_count(a) + dma_fence_count(b);
    if ((size_t)total * sizeof(*leaves) / sizeof(*leaves) != total)
        return NULL;
    leaves = kmalloc((size_t)total * sizeof(*leaves));
    if (!leaves)
        return NULL;
    inputs[0] = a;
    inputs[1] = b;
    for (i = 0; i < 2; i++)
        for (j = 0; j < dma_fence_count(inputs[i]); j++) {
            f = dma_fence_at(inputs[i], j);
            if (!dma_fence_is_signaled(f))
                leaves[n++] = f;
            else if (f->timestamp_ns > timestamp)
                timestamp = f->timestamp_ns;
        }
    out = dma_fence_merge(leaves, n);
    if (!n && out)
        out->timestamp_ns = timestamp;
    kfree(leaves);
    return out;
}

/** Merge another sync file, query it, or record a deadline hint. */
static int32_t sync_ioctl(fd_entry_t *e, uint32_t request, uintptr_t arg)
{
    struct sync_file *s = e->priv;
    struct sync_merge_data data;
    struct sync_set_deadline deadline;
    struct dma_fence *inputs[2], *merged;
    int32_t fd;
    if (request == SYNC_IOC_FILE_INFO)
        return sync_info(s, arg);
    if (request == SYNC_IOC_SET_DEADLINE) {
        if (copy_from_user(&deadline, (void __user *)arg, sizeof(deadline)))
            return -EFAULT;
        if (deadline.pad || deadline.deadline_ns > 0x7fffffffffffffffull)
            return -EINVAL;
        dma_fence_set_deadline(s->fence, (int64_t)deadline.deadline_ns);
        return 0;
    }
    if (request != SYNC_IOC_MERGE)
        return -ENOTTY;
    if (copy_from_user(&data, (void __user *)arg, sizeof(data)))
        return -EFAULT;
    if (data.flags || data.pad)
        return -EINVAL;
    inputs[0] = s->fence;
    inputs[1] = sync_file_get_fence(data.fd2);
    if (!inputs[1])
        return -ENOENT;
    merged = sync_merge(inputs[0], inputs[1]);
    dma_fence_put(inputs[1]);
    if (!merged)
        return -ENOMEM;
    data.name[31] = 0;
    fd = sync_file_create(merged, data.name);
    dma_fence_put(merged);
    if (fd < 0)
        return fd;
    data.fence = fd;
    if (copy_to_user((void __user *)arg, &data, sizeof(data))) {
        fd_free(current_proc->fds, fd);
        return -EFAULT;
    }
    return 0;
}

static const struct file_ops sync_ops = {NULL, NULL, sync_poll, sync_ioctl, sync_close, NULL, NULL};

/** Find a caller's sync file and take a reference to its fence. */
struct dma_fence *sync_file_get_fence(int32_t fd)
{
    fd_entry_t *e = current_proc ? fd_get(current_proc->fds, fd) : NULL;
    return e && !(e->flags & FD_FLAG_PATH) && e->ops == &sync_ops
               ? dma_fence_get(((struct sync_file *)e->priv)->fence)
               : NULL;
}

/** Create a close-on-exec descriptor, keeping the caller's fence reference. */
int32_t sync_file_create(struct dma_fence *f, const char *name)
{
    struct sync_file *s;
    fd_entry_t *e;
    int32_t fd;
    if (!f || !current_proc)
        return -EINVAL;
    s = kzalloc(sizeof(*s));
    if (!s)
        return -ENOMEM;
    s->anon.name = "[sync_file]";
    s->fence = dma_fence_get(f);
    sync_name(s->name, name ? name : "dma_fence");
    fd = fd_alloc(current_proc->fds);
    if (fd < 0) {
        dma_fence_put(f);
        kfree(s);
        return fd;
    }
    e = &current_proc->fds->entries[fd];
    e->type = FD_TYPE_ANON;
    e->ops = &sync_ops;
    e->priv = s;
    e->flags = FD_FLAG_READ | FD_FLAG_WRITE | FD_FLAG_CLOEXEC;
    return fd;
}
