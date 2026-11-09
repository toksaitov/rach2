/* Software timelines create sync-file fences and signal them by increments. */
#include "dmabuf.h"
#include "../mm/kheap.h"
#include "../mm/usercopy.h"
#include "../lib/kerrno.h"
#include "../lib/kstring.h"

struct sw_point {
    struct dma_fence fence;
    struct sw_point *next;
};
struct sw_timeline {
    uint64_t context;
    uint32_t value;
    struct sw_point *points;
};

/** The software fence driver's name. */
static const char *sw_driver_name(struct dma_fence *f)
{
    (void)f;
    return "sw_sync";
}

/** The software timeline's name, valid after its descriptor closes. */
static const char *sw_timeline_name(struct dma_fence *f)
{
    (void)f;
    return "sw_sync";
}

static const struct dma_fence_ops sw_ops = {sw_driver_name, sw_timeline_name, NULL, NULL};

/** Complete every point reached, detaching it before callbacks can run. */
static void sw_advance(struct sw_timeline *t, uint32_t increment, int close)
{
    struct sw_point **p = &t->points, *point;
    t->value += increment;
    while ((point = *p) != NULL) {
        if (close || (int32_t)(t->value - (uint32_t)point->fence.seqno) >= 0) {
            *p = point->next;
            point->next = NULL;
            if (close)
                dma_fence_set_error(&point->fence, -ENOENT);
            dma_fence_signal(&point->fence);
            dma_fence_put(&point->fence);
        } else
            p = &point->next;
    }
}

/** Signal orphaned points with ENOENT and release the timeline. */
static void sw_close(fd_entry_t *e)
{
    struct sw_timeline *t = e->priv;
    sw_advance(t, 0, 1);
    kfree(t);
    e->priv = NULL;
}

/** Create a fence, increment the timeline or read a requested deadline. */
static int32_t sw_ioctl(fd_entry_t *e, uint32_t request, uintptr_t arg)
{
    struct sw_timeline *t = e->priv;
    struct sw_sync_create_fence_data data;
    struct sw_sync_get_deadline deadline;
    struct sw_point *point;
    struct dma_fence *f;
    uint32_t increment;
    int32_t fd;
    if (request == SW_SYNC_IOC_INC) {
        if (copy_from_user(&increment, (void __user *)arg, sizeof(increment)))
            return -EFAULT;
        while (increment > 0x7fffffffu) {
            sw_advance(t, 0x7fffffffu, 0);
            increment -= 0x7fffffffu;
        }
        sw_advance(t, increment, 0);
        return 0;
    }
    if (request == SW_SYNC_GET_DEADLINE) {
        if (copy_from_user(&deadline, (void __user *)arg, sizeof(deadline)))
            return -EFAULT;
        if (deadline.pad || deadline.deadline_ns)
            return -EINVAL;
        f = sync_file_get_fence(deadline.fence_fd);
        if (!f)
            return -EINVAL;
        if (f->ops != &sw_ops) {
            dma_fence_put(f);
            return -EINVAL;
        }
        deadline.deadline_ns = (uint64_t)dma_fence_deadline(f);
        dma_fence_put(f);
        if (deadline.deadline_ns == 0x7fffffffffffffffull)
            return -ENOENT;
        return copy_to_user((void __user *)arg, &deadline, sizeof(deadline)) ? -EFAULT : 0;
    }
    if (request != SW_SYNC_IOC_CREATE_FENCE)
        return -ENOTTY;
    if (copy_from_user(&data, (void __user *)arg, sizeof(data)))
        return -EFAULT;
    point = kzalloc(sizeof(*point));
    if (!point)
        return -ENOMEM;
    dma_fence_init(&point->fence, &sw_ops, t->context, data.value);
    data.name[31] = 0;
    fd = sync_file_create(&point->fence, data.name);
    if (fd < 0) {
        dma_fence_put(&point->fence);
        return fd;
    }
    data.fence = fd;
    if (copy_to_user((void __user *)arg, &data, sizeof(data))) {
        fd_free(current_proc->fds, fd);
        dma_fence_put(&point->fence);
        return -EFAULT;
    }
    point->next = t->points;
    t->points = point;
    sw_advance(t, 0, 0);
    return 0;
}

static const struct file_ops sw_file_ops = {NULL, NULL, NULL, sw_ioctl, sw_close, NULL, NULL};

/** Give a just-opened debugfs descriptor its own software timeline. */
int32_t sw_sync_open(fd_entry_t *e)
{
    struct sw_timeline *t = kzalloc(sizeof(*t));
    if (!t)
        return -ENOMEM;
    t->context = dma_fence_context_alloc(1);
    e->ops = &sw_file_ops;
    e->priv = t;
    return 0;
}
