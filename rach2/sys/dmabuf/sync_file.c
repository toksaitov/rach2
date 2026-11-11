/* Sync file descriptors carry fences, merge them and report completion. */
#include "dmabuf.h"
#include "../mm/kheap.h"
#include "../mm/usercopy.h"
#include "../lib/kerrno.h"
#include "../lib/kstring.h"
#include "../timer.h"

struct sync_file {
    struct anon_obj anon;
    struct dma_fence *fence;
    char name[32]; /* the user's, from MERGE; empty for one made from the fence */
};

/** Poll a sync file through its fence's own completion wait queue. */
static short sync_poll(fd_entry_t *e, short events, struct wait_queue **wq)
{
    struct sync_file *s = e->priv;
    int32_t status = dma_fence_status(s->fence);
    /* A poller asks for the signal, as Linux's sync_file_poll adds a
     * callback, which also arms a merged fence. */
    if (wq && !status) {
        dma_fence_enable_sw_signaling(s->fence);
        status = dma_fence_status(s->fence);
    }
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

/** Append `src` to the 32 bytes at `out` from `*at`, cut to fit. */
static void name_put(char *out, uint32_t *at, const char *src)
{
    while (*src && *at < 31)
        out[(*at)++] = *src++;
    out[*at] = 0;
}

/** Append the decimal `v` to the 32 bytes at `out` from `*at`, as a signed
 * number when `sign`. */
static void name_dec(char *out, uint32_t *at, uint64_t v, int sign)
{
    char digits[21];
    uint32_t rem;
    int n = 0;
    if (sign && (int64_t)v < 0) {
        name_put(out, at, "-");
        v = 0 - v;
    }
    do {
        v = tick_div(v, 10, &rem);
        digits[n++] = (char)('0' + rem);
    } while (v);
    while (n > 0 && *at < 31)
        out[(*at)++] = digits[--n];
    out[*at] = 0;
}

/** The name FILE_INFO reports, Linux's sync_file_get_name: the user's from
 * MERGE, else the fence's driver and timeline, its context and its number,
 * as "%s-%s%llu-%lld". */
static void file_name(const struct sync_file *s, char *out)
{
    struct dma_fence *f = s->fence;
    uint32_t at = 0;
    if (s->name[0]) {
        sync_name(out, s->name);
        return;
    }
    out[0] = 0;
    name_put(out, &at, f->ops && f->ops->get_driver_name ? f->ops->get_driver_name(f) : "rach2");
    name_put(out, &at, "-");
    name_put(out, &at,
             f->ops && f->ops->get_timeline_name ? f->ops->get_timeline_name(f) : "dma_fence");
    name_dec(out, &at, f->context, 0);
    name_put(out, &at, "-");
    name_dec(out, &at, f->seqno, 1);
}

/**
 * Report the leaf fences, as Linux's sync_file_ioctl_fence_info: a
 * num_fences of zero asks for the count and the fence's own status, and a
 * larger one for every row, with the status of the first leaf that is
 * active or failed, else 1. The rows are filled before the one copy out,
 * since a leaf may signal while the copy sleeps.
 */
static int32_t sync_info(struct sync_file *s, uintptr_t arg)
{
    struct sync_file_info info;
    struct sync_fence_info *rows;
    struct dma_fence_unwrap u;
    struct dma_fence *f;
    uint32_t i, n;
    int32_t status, rc = 0;
    if (copy_from_user(&info, (void __user *)arg, sizeof(info)))
        return -EFAULT;
    if (info.flags || info.pad)
        return -EINVAL;
    n = dma_fence_count(s->fence);
    if (!info.num_fences) {
        info.status = dma_fence_status(s->fence);
    } else {
        if (info.num_fences < n)
            return -EINVAL;
        if ((size_t)n * sizeof(*rows) / sizeof(*rows) != n)
            return -ENOMEM;
        rows = kzalloc((size_t)n * sizeof(*rows));
        if (!rows)
            return -ENOMEM;
        info.status = 1;
        /* One walk, as Linux's second dma_fence_unwrap_for_each: a leaf
         * signalled since the count may have left, never one added. */
        i = 0;
        for (f = dma_fence_unwrap_first(s->fence, &u); f && i < n; f = dma_fence_unwrap_next(&u)) {
            sync_name(rows[i].obj_name, f->ops && f->ops->get_timeline_name
                                            ? f->ops->get_timeline_name(f)
                                            : "dma_fence");
            sync_name(rows[i].driver_name,
                      f->ops && f->ops->get_driver_name ? f->ops->get_driver_name(f) : "rach2");
            status = dma_fence_status(f);
            rows[i].status = status;
            rows[i].timestamp_ns = status ? f->timestamp_ns : 0;
            if (info.status > 0)
                info.status = status;
            i++;
        }
        dma_fence_unwrap_end(&u);
        n = i;
        if (info.sync_fence_info > (uintptr_t)-1 ||
            (uint64_t)n * sizeof(*rows) > (uintptr_t)-1 - info.sync_fence_info ||
            copy_to_user((void __user *)(uintptr_t)info.sync_fence_info, rows,
                         (size_t)n * sizeof(*rows)))
            rc = -EFAULT;
        kfree(rows);
        if (rc)
            return rc;
    }
    file_name(s, info.name);
    info.num_fences = n;
    return copy_to_user((void __user *)arg, &info, sizeof(info)) ? -EFAULT : 0;
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
    merged = dma_fence_unwrap_merge(inputs, 2);
    dma_fence_put(inputs[1]);
    if (!merged)
        return -ENOMEM;
    data.name[31] = 0;
    fd =
        sync_file_copy_out(merged, data.name, (void __user *)arg, &data, sizeof(data), &data.fence);
    dma_fence_put(merged);
    return fd;
}

static const struct file_ops sync_ops = {NULL,       NULL, sync_poll, sync_ioctl,
                                         sync_close, NULL, NULL,      0};

/** Find a caller's sync file and take a reference to its fence. */
struct dma_fence *sync_file_get_fence(int32_t fd)
{
    fd_entry_t *e = current_proc ? fd_get(current_proc->fds, fd) : NULL;
    return e && !(e->flags & FD_FLAG_PATH) && e->ops == &sync_ops
               ? dma_fence_get(((struct sync_file *)e->priv)->fence)
               : NULL;
}

struct sync_file *sync_file_new(struct dma_fence *f, const char *name)
{
    struct sync_file *s = kzalloc(sizeof(*s));
    if (!s)
        return NULL;
    s->anon.name = "sync_file";
    s->fence = dma_fence_get(f);
    sync_name(s->name, name);
    return s;
}

void sync_file_drop(struct sync_file *s)
{
    if (s) {
        dma_fence_put(s->fence);
        kfree(s);
    }
}

void sync_file_attach(int32_t fd, struct sync_file *s)
{
    fd_entry_t *e = fd_slot(current_proc->fds, fd);
    e->type = FD_TYPE_ANON;
    e->ops = &sync_ops;
    e->priv = s;
    e->flags = FD_FLAG_READ | FD_FLAG_WRITE | FD_FLAG_CLOEXEC;
    fd_install(current_proc->fds, fd);
}

/** Make the descriptor `fd`, reserved and given no type, a sync file of
 * `f` with a reference of its own. */
int32_t sync_file_install(int32_t fd, struct dma_fence *f, const char *name)
{
    struct sync_file *s;
    if (!f || !current_proc)
        return -EINVAL;
    s = sync_file_new(f, name);
    if (!s)
        return -ENOMEM;
    sync_file_attach(fd, s);
    return 0;
}

int32_t sync_file_copy_out(struct dma_fence *f, const char *name, void __user *dst, void *src,
                           uint32_t len, int32_t *fd_at)
{
    struct sync_file *s;
    int32_t fd;
    if (!f || !current_proc)
        return -EINVAL;
    fd = fd_alloc(current_proc->fds);
    if (fd < 0)
        return fd;
    s = sync_file_new(f, name);
    *fd_at = fd;
    if (!s || copy_to_user(dst, src, len)) {
        sync_file_drop(s);
        fd_free(current_proc->fds, fd);
        return s ? -EFAULT : -ENOMEM;
    }
    sync_file_attach(fd, s);
    return 0;
}

/** Create a close-on-exec descriptor, keeping the caller's fence reference. */
int32_t sync_file_create(struct dma_fence *f, const char *name)
{
    int32_t fd, err;
    if (!f || !current_proc)
        return -EINVAL;
    fd = fd_alloc(current_proc->fds);
    if (fd < 0)
        return fd;
    err = sync_file_install(fd, f, name);
    if (err < 0) {
        fd_free(current_proc->fds, fd);
        return err;
    }
    return fd;
}
