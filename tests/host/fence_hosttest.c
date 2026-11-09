/* Fence completion, reservations, sync files and software timeline lifetimes.
 * host-deps: rach2/sys/lib/kstring.c rach2/sys/lib/kctypes.c
 * host-deps: tests/host/compat/kheap_host.c
 */
#include "dmabuf/fence.c"
#include "dmabuf/dmabuf.c"
#include "dmabuf/sync_file.c"
#include "dmabuf/sw_sync.c"

int printf(const char *format, ...);

struct percpu cpu_table[NR_CPUS];
static process_t self;
static fd_table_t table;
static fd_entry_t slots[64];
static struct open_file descriptions[64];
static int failures, callbacks, wakes, wait_mode;
static int64_t now_ns;
static struct dma_fence *signal_on_wait;

#define CHECK(c, s)                              \
    do {                                         \
        if (!(c)) {                              \
            printf("fence-test: fail: %s\n", s); \
            failures++;                          \
        }                                        \
    } while (0)

/** Fixed monotonic clock. */
void rtc_monotonic(int64_t *sec, int64_t *ns)
{
    *sec = now_ns / 1000000000ll;
    *ns = now_ns % 1000000000ll;
}
/** Fixed tick count. */
uint64_t rtc_ticks_now(void)
{
    return (uint64_t)(now_ns / 1000000);
}
/** Host arithmetic for the kernel's division helper. */
uint64_t tick_div(uint64_t n, uint32_t d, uint32_t *rem)
{
    if (rem)
        *rem = (uint32_t)(n % d);
    return n / d;
}
/** Initialize a model queue. */
void wait_queue_init(wait_queue_t *wq)
{
    k_memset(wq, 0, sizeof(*wq));
}
/** Record a wake without scheduling. */
void wq_wake(wait_queue_t *wq, uint32_t kinds)
{
    (void)wq;
    (void)kinds;
    wakes++;
}
/** Wake every modeled kind. */
void wq_wake_all(wait_queue_t *wq)
{
    wq_wake(wq, WQ_ALL);
}
/** Advance to an armed timeout or complete a producer during the sleep. */
int wq_wait_event(wait_queue_t *wq, wq_cond_fn cond, void *arg)
{
    (void)wq;
    if (wait_mode == 1)
        return -EINTR;
    if (signal_on_wait)
        dma_fence_signal(signal_on_wait);
    else
        now_ns = (int64_t)current_proc->wakeup_tick * 1000000;
    CHECK(cond(arg), "wait condition after scheduled event");
    return 0;
}
/** Uninterruptible waits ignore the model's interrupt. */
int wq_wait_event_nointr(wait_queue_t *wq, wq_cond_fn cond, void *arg)
{
    int saved = wait_mode, rc;
    wait_mode = 0;
    rc = wq_wait_event(wq, cond, arg);
    wait_mode = saved;
    return rc;
}
/** Copy from a real test address, refusing the model's invalid page. */
int copy_from_user(void *dst, const void __user *src, size_t n)
{
    if ((uintptr_t)src < 4096)
        return -EFAULT;
    k_memcpy(dst, src, n);
    return 0;
}
/** Copy to a real test address. */
int copy_to_user(void __user *dst, const void *src, size_t n)
{
    if ((uintptr_t)dst < 4096)
        return -EFAULT;
    k_memcpy(dst, src, n);
    return 0;
}
/** Allocate a descriptor in the model's bounded table. */
int fd_alloc(fd_table_t *t)
{
    int i;
    for (i = 0; i < 64; i++)
        if (t->entries[i].type == FD_TYPE_NONE) {
            k_memset(&descriptions[i], 0, sizeof(descriptions[i]));
            t->entries[i].ofd = &descriptions[i];
            return i;
        }
    return -EMFILE;
}
/** Find a live model descriptor. */
fd_entry_t *fd_get(fd_table_t *t, int fd)
{
    return fd >= 0 && fd < 64 && t->entries[fd].type != FD_TYPE_NONE ? &t->entries[fd] : NULL;
}
/** Release the object attached to a model descriptor. */
int fd_free(fd_table_t *t, int fd)
{
    fd_entry_t *e = fd_get(t, fd);
    if (!e)
        return -EBADF;
    if (e->ops && e->ops->close)
        e->ops->close(e);
    k_memset(e, 0, sizeof(*e));
    return 0;
}
/** Count one completion callback. */
static void counted(struct dma_fence *f, struct dma_fence_cb *cb)
{
    (void)f;
    (void)cb;
    callbacks++;
}
/** Allocate an ordinary fence for a chosen timeline and point. */
static struct dma_fence *new_fence(uint64_t context, uint64_t seqno)
{
    struct dma_fence *f = kzalloc(sizeof(*f));
    dma_fence_init(f, NULL, context, seqno);
    return f;
}
/** Drive distinct contexts, callbacks, timed and interrupted waits. */
static void fence_cases(void)
{
    uint64_t context = dma_fence_context_alloc(3);
    struct dma_fence *a = new_fence(context, 1), *b = new_fence(context + 1, 1);
    struct dma_fence_cb cb;
    CHECK(dma_fence_context_alloc(1) == context + 3, "context ranges do not overlap");
    CHECK(!dma_fence_add_callback(a, &cb, counted), "add callback");
    CHECK(dma_fence_remove_callback(a, &cb), "withdraw callback");
    CHECK(!dma_fence_remove_callback(a, &cb), "withdraw twice");
    CHECK(!dma_fence_add_callback(a, &cb, counted), "rearm callback");
    CHECK(dma_fence_wait_until(a, 1, 0) == -ETIME, "expired wait");
    self.wakeup_tick = 999;
    CHECK(dma_fence_wait_until(a, 1, 2000000) == -ETIME, "timed wait");
    CHECK(self.wakeup_tick == 999, "wait restores previous timer");
    wait_mode = 1;
    CHECK(dma_fence_wait_until(a, 1, -1) == -EINTR, "interruptible wait");
    signal_on_wait = a;
    CHECK(!dma_fence_wait_until(a, 0, -1), "uninterruptible producer completion");
    wait_mode = 0;
    signal_on_wait = NULL;
    CHECK(callbacks == 1 && a->timestamp_ns == now_ns, "callback and timestamp");
    CHECK(dma_fence_signal(a) == -EINVAL, "signal only once");
    CHECK(dma_fence_add_callback(a, &cb, counted) == -ENOENT, "completed callback");
    dma_fence_set_error(b, -EIO);
    dma_fence_signal(b);
    CHECK(dma_fence_status(b) == -EIO, "producer error");
    dma_fence_set_error(b, -ENODEV);
    CHECK(b->error == -EIO, "completion immutable");
    dma_fence_put(a);
    dma_fence_put(b);
}
/** Test merge flattening, duplicate timelines, errors and callback teardown. */
static void merge_cases(void)
{
    uint64_t c = dma_fence_context_alloc(2);
    struct dma_fence *a = new_fence(c, 1), *b = new_fence(c, 2);
    struct dma_fence *d = new_fence(c + 1, 1), *f, *g, *inputs[3];
    inputs[0] = a;
    inputs[1] = b;
    inputs[2] = d;
    f = dma_fence_merge(inputs, 3);
    CHECK(f && dma_fence_count(f) == 2, "merge keeps latest timeline point");
    inputs[0] = f;
    inputs[1] = d;
    g = dma_fence_merge(inputs, 2);
    CHECK(dma_fence_count(g) == 2, "nested merge flattened");
    dma_fence_put(f);
    CHECK(!dma_fence_status(g), "merge remains active");
    dma_fence_set_error(b, -ENODEV);
    dma_fence_signal(b);
    CHECK(!dma_fence_status(g), "all producers must complete");
    dma_fence_signal(d);
    CHECK(dma_fence_status(g) == -ENODEV, "merged error reaches waiter");
    dma_fence_put(g);
    inputs[0] = a;
    f = dma_fence_merge(inputs, 1);
    dma_fence_put(f);
    CHECK(a->cbs == NULL && a->refs == 1, "close active merge detaches callbacks");
    dma_fence_put(a);
    dma_fence_put(b);
    dma_fence_put(d);
}
/** A model exporter owns no memory beyond its buffer record. */
static const struct dma_buf_ops model_ops = {NULL, NULL, NULL};
/** Exercise descriptor identity, reservations, imports, exports and poll. */
static void buffer_cases(void)
{
    struct dma_buf *b = dma_buf_export(8192, &model_ops, NULL, NULL);
    struct dma_buf *other = dma_buf_export(4096, &model_ops, NULL, NULL);
    struct dma_fence *w = new_fence(dma_fence_context_alloc(1), 1);
    struct dma_fence *r = new_fence(dma_fence_context_alloc(1), 1), *snap;
    struct dma_buf_export_sync_file data;
    struct dma_buf_sync access;
    struct sync_file_info info;
    struct sync_merge_data merge;
    struct sync_fence_info rows[2];
    struct wait_queue *q;
    int fd = dma_buf_fd(b, 02000000), syncfd, old_wakes;
    fd_entry_t *e = fd_get(&table, fd);
    CHECK(b->ino != other->ino && b->refs == 2, "buffer inode and descriptor reference");
    CHECK(is_dma_buf_file(e) && e->flags & FD_FLAG_CLOEXEC, "buffer descriptor identity");
    CHECK(e->ops->llseek(e, 0, 2) == 8192 && e->ops->llseek(e, 0, 0) == 0,
          "buffer size and rewind");
    CHECK(e->ops->llseek(e, 1, 2) == -EINVAL && e->ops->llseek(e, 0, 1) == -EINVAL,
          "invalid seeks");
    CHECK(!dma_resv_add_fence(b->resv, w, 1), "install writer");
    CHECK(!dma_resv_add_fence(b->resv, r, 0), "install reader");
    CHECK(!e->ops->poll(e, 5, &q) && q == &b->wq, "buffer waits on implicit fences");
    snap = dma_resv_snapshot(b->resv, 1);
    CHECK(dma_fence_count(snap) == 2, "write waits for readers too");
    dma_fence_put(snap);
    data.flags = DMA_BUF_SYNC_WRITE;
    data.fd = -1;
    CHECK(!e->ops->ioctl(e, DMA_BUF_IOCTL_EXPORT_SYNC_FILE, (uintptr_t)&data), "export sync file");
    syncfd = data.fd;
    k_memset(&info, 0, sizeof(info));
    CHECK(!sync_ioctl(fd_get(&table, syncfd), SYNC_IOC_FILE_INFO, (uintptr_t)&info) &&
              info.num_fences == 2 && !info.status,
          "count sync file leaves");
    info.num_fences = 1;
    CHECK(sync_info(fd_get(&table, syncfd)->priv, (uintptr_t)&info) == -EINVAL, "short info array");
    info.num_fences = 2;
    info.sync_fence_info = (uintptr_t)rows;
    CHECK(!sync_info(fd_get(&table, syncfd)->priv, (uintptr_t)&info) && !rows[0].status,
          "leaf info copied");
    k_memset(&merge, 0, sizeof(merge));
    merge.fd2 = syncfd;
    CHECK(!sync_ioctl(fd_get(&table, syncfd), SYNC_IOC_MERGE, (uintptr_t)&merge),
          "merge sync files");
    old_wakes = wakes;
    dma_fence_signal(w);
    CHECK(wakes > old_wakes && e->ops->poll(e, 5, &q) == 1, "writer wakes readers");
    dma_fence_signal(r);
    CHECK(e->ops->poll(e, 5, &q) == 5, "all work complete");
    CHECK(sync_poll(fd_get(&table, merge.fence), 1, &q) == 1, "merged file ready");
    access.flags = DMA_BUF_SYNC_READ;
    CHECK(!e->ops->ioctl(e, DMA_BUF_IOCTL_SYNC, (uintptr_t)&access), "CPU access");
    access.flags = 0;
    CHECK(e->ops->ioctl(e, DMA_BUF_IOCTL_SYNC, (uintptr_t)&access) == -EINVAL, "CPU access flags");
    data.fd = syncfd;
    data.flags = DMA_BUF_SYNC_READ;
    CHECK(!e->ops->ioctl(e, DMA_BUF_IOCTL_IMPORT_SYNC_FILE, (uintptr_t)&data), "import read fence");
    fd_free(&table, merge.fence);
    fd_free(&table, syncfd);
    fd_free(&table, fd);
    dma_buf_put(b);
    dma_buf_put(other);
    dma_fence_put(w);
    dma_fence_put(r);
}
/** Drive software points, deadline hints, wraparound and closing a producer. */
static void timeline_cases(void)
{
    fd_entry_t timeline;
    struct sw_sync_create_fence_data data;
    struct sw_sync_get_deadline deadline;
    struct sync_set_deadline hint;
    struct dma_fence *f;
    uint32_t step = 1;
    int fd;
    k_memset(&timeline, 0, sizeof(timeline));
    CHECK(!sw_sync_open(&timeline), "open software timeline");
    k_memset(&data, 0, sizeof(data));
    data.value = 2;
    CHECK(!sw_ioctl(&timeline, SW_SYNC_IOC_CREATE_FENCE, (uintptr_t)&data),
          "create software fence");
    fd = data.fence;
    f = sync_file_get_fence(fd);
    CHECK(f && !dma_fence_status(f), "software fence active");
    hint.deadline_ns = 1234567;
    hint.pad = 0;
    CHECK(!sync_ioctl(fd_get(&table, fd), SYNC_IOC_SET_DEADLINE, (uintptr_t)&hint), "set deadline");
    k_memset(&deadline, 0, sizeof(deadline));
    deadline.fence_fd = fd;
    CHECK(!sw_ioctl(&timeline, SW_SYNC_GET_DEADLINE, (uintptr_t)&deadline) &&
              deadline.deadline_ns == 1234567,
          "software deadline roundtrip");
    sw_ioctl(&timeline, SW_SYNC_IOC_INC, (uintptr_t)&step);
    CHECK(!dma_fence_status(f), "first increment below point");
    sw_ioctl(&timeline, SW_SYNC_IOC_INC, (uintptr_t)&step);
    CHECK(dma_fence_status(f) == 1, "second increment signals point");
    fd_free(&table, fd);
    dma_fence_put(f);
    data.value = 4;
    sw_ioctl(&timeline, SW_SYNC_IOC_CREATE_FENCE, (uintptr_t)&data);
    f = sync_file_get_fence(data.fence);
    sw_close(&timeline);
    CHECK(dma_fence_status(f) == -ENOENT, "producer close signals error");
    fd_free(&table, data.fence);
    dma_fence_put(f);
}
/** Run the model cases and fail on any assertion. */
int main(void)
{
    int i;
    current_proc = &self;
    table.entries = slots;
    table.cap = 64;
    self.fds = &table;
    fence_cases();
    merge_cases();
    buffer_cases();
    timeline_cases();
    for (i = 0; i < 64; i++)
        CHECK(slots[i].type == FD_TYPE_NONE, "all descriptors closed");
    if (!failures)
        printf("fence-test: passed\n");
    return failures != 0;
}
