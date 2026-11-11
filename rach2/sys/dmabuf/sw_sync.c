/* Software timelines create sync-file fences and signal them by increments. */
#include "dmabuf.h"
#include "../mm/kheap.h"
#include "../mm/usercopy.h"
#include "../lib/kerrno.h"
#include "../lib/kstring.h"
#include "../timer.h"

/* A point, which carries the name of its timeline, so that the name its
 * fence reports outlives the timeline's descriptor, as Linux's reference on
 * the timeline makes it. */
struct sw_point {
    struct dma_fence fence;
    struct sw_point *next;
    char timeline[16];
};
/* A timeline: its points not yet reached, in the order of their values, and
 * its place on the list of live timelines that sync/info shows, newest
 * last. `name` is the command of the process that opened it, as Linux names
 * it. */
struct sw_timeline {
    uint64_t context;
    uint32_t value;
    struct sw_point *points;
    struct sw_timeline *next;
    char name[16];
};

static struct sw_timeline *timelines;

/** The software fence driver's name. */
static const char *sw_driver_name(struct dma_fence *f)
{
    (void)f;
    return "sw_sync";
}

/** The software timeline's name, valid after its descriptor closes. */
static const char *sw_timeline_name(struct dma_fence *f)
{
    return ((struct sw_point *)f)->timeline;
}

static const struct dma_fence_ops sw_ops = {
    sw_driver_name, sw_timeline_name, NULL, NULL, NULL, NULL};

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
    struct sw_timeline *t = e->priv, **link;
    sw_advance(t, 0, 1);
    for (link = &timelines; *link && *link != t; link = &(*link)->next) {
    }
    if (*link)
        *link = t->next;
    kfree(t);
    e->priv = NULL;
}

/** Create a fence, increment the timeline or read a requested deadline. */
static int32_t sw_ioctl(fd_entry_t *e, uint32_t request, uintptr_t arg)
{
    struct sw_timeline *t = e->priv;
    struct sw_sync_create_fence_data data;
    struct sw_sync_get_deadline deadline;
    struct sw_point *point, **link;
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
    k_memcpy(point->timeline, t->name, sizeof(point->timeline));
    data.name[31] = 0;
    /* Linux ignores the caller's name and names the file from its fence. */
    fd = sync_file_copy_out(&point->fence, NULL, (void __user *)arg, &data, sizeof(data),
                            &data.fence);
    if (fd < 0) {
        dma_fence_put(&point->fence);
        return fd;
    }
    for (link = &t->points;
         *link && (int32_t)((uint32_t)(*link)->fence.seqno - (uint32_t)data.value) <= 0;
         link = &(*link)->next) {
    }
    point->next = *link;
    *link = point;
    sw_advance(t, 0, 0);
    return 0;
}

static const struct file_ops sw_file_ops = {NULL, NULL, NULL, sw_ioctl, sw_close, NULL, NULL, 0};

/** Give a just-opened debugfs descriptor its own software timeline. */
int32_t sw_sync_open(fd_entry_t *e)
{
    struct sw_timeline *t = kzalloc(sizeof(*t)), **link;
    if (!t)
        return -ENOMEM;
    t->context = dma_fence_context_alloc(1);
    if (current_proc)
        k_memcpy(t->name, current_proc->comm, sizeof(t->name) - 1);
    for (link = &timelines; *link; link = &(*link)->next) {
    }
    *link = t;
    e->ops = &sw_file_ops;
    e->priv = t;
    return 0;
}

/* The text sync/info is written into: `cap` bytes at `buf`, of which `len`
 * are written and `need` would be. */
struct sw_text {
    char *buf;
    uint32_t cap, len, need;
};

/** Append the string `s`. */
static void text_put(struct sw_text *t, const char *s)
{
    for (; *s; s++, t->need++) {
        if (t->len + 1 < t->cap)
            t->buf[t->len++] = *s;
    }
}

/** Append `v` in decimal, with its sign. */
static void text_num(struct sw_text *t, int64_t v)
{
    char digits[24];
    int i = (int)sizeof(digits) - 1;
    uint64_t u = v < 0 ? (uint64_t)0 - (uint64_t)v : (uint64_t)v;
    digits[i] = '\0';
    do {
        uint32_t rem;
        u = tick_div(u, 10, &rem);
        digits[--i] = (char)('0' + (int)rem);
    } while (u);
    if (v < 0)
        digits[--i] = '-';
    text_put(t, digits + i);
}

uint32_t sw_sync_info(char *buf, uint32_t cap)
{
    struct sw_text t;
    struct sw_timeline *tl;
    struct sw_point *p;
    t.buf = buf;
    t.cap = buf ? cap : 0;
    t.len = t.need = 0;
    text_put(&t, "objs:\n--------------\n");
    for (tl = timelines; tl; tl = tl->next) {
        text_put(&t, tl->name);
        text_put(&t, ": ");
        text_num(&t, (int32_t)tl->value);
        text_put(&t, "\n");
        for (p = tl->points; p; p = p->next) {
            text_put(&t, "  fence active: ");
            text_num(&t, (int64_t)p->fence.seqno);
            text_put(&t, " / ");
            text_num(&t, (int32_t)tl->value);
            text_put(&t, "\n");
        }
        text_put(&t, "\n");
    }
    text_put(&t, "fences:\n--------------\n");
    if (t.cap)
        t.buf[t.len] = '\0';
    return t.need;
}
