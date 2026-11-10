/* Fence completion, reservations, sync files and software timeline lifetimes.
 * host-deps: rach2/sys/lib/kstring.c rach2/sys/lib/kctypes.c
 * host-deps: tests/host/compat/kheap_host.c
 */
#include "dmabuf/fence.c"
#include "dmabuf/dmabuf.c"
#include "dmabuf/sync_file.c"
#include "dmabuf/sw_sync.c"

/* The model's interrupt flag, and an interrupt that a row raises while
 * interrupts are masked, taken when the flag comes on again, as a device's
 * interrupt waits for a thread's masked section to end. */
static int irq_on = 1;
static void (*irq_pending)(void);

/** Mask the model's interrupts; whether they were on. */
uint32_t kirq_save(void)
{
    uint32_t was = (uint32_t)irq_on;
    irq_on = 0;
    return was;
}

/** Restore the model's interrupt flag, and take a pending interrupt, with
 * interrupts masked as in a handler, once it is on. */
void kirq_restore(uint32_t flags)
{
    void (*handler)(void) = irq_pending;
    irq_on = (int)flags;
    if (irq_on && handler) {
        irq_pending = NULL;
        irq_on = 0;
        handler();
        irq_on = 1;
    }
}

int printf(const char *format, ...);
int snprintf(char *out, unsigned long size, const char *format, ...);

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
/* Set: every copy to the user faults, as to a read-only page. */
static int to_user_faults;
/** Copy to a real test address. */
int copy_to_user(void __user *dst, const void *src, size_t n)
{
    if ((uintptr_t)dst < 4096 || to_user_faults)
        return -EFAULT;
    k_memcpy(dst, src, n);
    return 0;
}
/** Allocate a descriptor in the model's bounded table, skipping a slot
 * that is in use or reserved, which has its description set. */
int fd_alloc(fd_table_t *t)
{
    int i;
    for (i = 0; i < 64; i++)
        if (t->entries[i].type == FD_TYPE_NONE && !t->entries[i].ofd) {
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
/** Clear a model descriptor's slot, reserved or not, without closing its
 * object, as the kernel's fd_free does. */
int fd_free(fd_table_t *t, int fd)
{
    if (fd < 0 || fd >= 64 || (t->entries[fd].type == FD_TYPE_NONE && !t->entries[fd].ofd))
        return -1;
    k_memset(&t->entries[fd], 0, sizeof(t->entries[fd]));
    return 0;
}
/** Close a model descriptor: its object's close, then its slot. */
static int close_fd(fd_table_t *t, int fd)
{
    fd_entry_t *e = fd_get(t, fd);
    if (!e)
        return -EBADF;
    if (e->ops && e->ops->close)
        e->ops->close(e);
    return fd_free(t, fd);
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
/* The fences of `interrupt_cases`: f1 under the first point, f2 under the
 * second. */
static struct dma_fence *irq_f1, *irq_f2;

/** The device's interrupt: f2 completes. */
static void signal_f2(void)
{
    dma_fence_signal(irq_f2);
}

/** A callback on f1 that runs before the first point's: a device raises
 * its interrupt while f1's signal is under way. */
static void raise_f2(struct dma_fence *f, struct dma_fence_cb *cb)
{
    (void)f;
    (void)cb;
    irq_pending = signal_f2;
}

/** The second point's waiter: drops the last reference to the point, which
 * frees it and the first point with it. */
static void drop_point(struct dma_fence *f, struct dma_fence_cb *cb)
{
    (void)cb;
    callbacks++;
    dma_fence_put(f);
}

/**
 * An interrupt that frees the owner of a callback already taken off the
 * list but not yet called: point c2 on f2 follows point c1 on f1, and c2's
 * waiter holds the only reference to c2, which holds the only one to c1.
 * While f1 signals, after a callback that raises f2's interrupt and before
 * c1's callback, the interrupt completes c2 and its waiter frees both
 * points. dma_fence_signal runs f1's callbacks with interrupts masked, so
 * the interrupt comes after c1's callback has returned; without that,
 * c1's callback runs in freed storage, which the sanitizer reports.
 */
static void interrupt_cases(void)
{
    uint64_t context = dma_fence_context_alloc(2);
    struct dma_fence_chain *c1 = dma_fence_chain_alloc(), *c2 = dma_fence_chain_alloc();
    struct dma_fence_cb raise, waiter;
    int before = callbacks;
    irq_f1 = new_fence(context, 1);
    irq_f2 = new_fence(context + 1, 1);
    dma_fence_chain_init(c1, NULL, dma_fence_get(irq_f1), 1);
    dma_fence_chain_init(c2, &c1->base, dma_fence_get(irq_f2), 2);
    CHECK(!dma_fence_is_signaled(&c1->base) && !dma_fence_is_signaled(&c2->base),
          "both points wait on their fences");
    CHECK(!dma_fence_add_callback(&c2->base, &waiter, drop_point), "the second point's waiter");
    CHECK(!dma_fence_add_callback(irq_f1, &raise, raise_f2), "the interrupt's trigger");
    dma_fence_signal(irq_f1);
    CHECK(irq_pending == NULL && irq_on, "the interrupt is taken once f1's callbacks end");
    CHECK(dma_fence_is_signaled(irq_f2) && callbacks == before + 1,
          "the interrupt completes the second point and its waiter runs");
    CHECK(irq_f1->refs == 1 && irq_f2->refs == 1 && !irq_f1->cbs && !irq_f2->cbs,
          "both points are freed with their fences' references and callbacks");
    dma_fence_put(irq_f1);
    dma_fence_put(irq_f2);
    irq_f1 = irq_f2 = NULL;
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
    close_fd(&table, merge.fence);
    close_fd(&table, syncfd);
    close_fd(&table, fd);
    dma_buf_put(b);
    dma_buf_put(other);
    dma_fence_put(w);
    dma_fence_put(r);
}
/** Imported writers from independent producers must all remain visible. */
static void import_cases(void)
{
    struct dma_buf *b = dma_buf_export(4096, &model_ops, NULL, NULL);
    struct dma_fence *a = new_fence(dma_fence_context_alloc(1), 1);
    struct dma_fence *c = new_fence(dma_fence_context_alloc(1), 1);
    struct dma_fence *r = new_fence(dma_fence_context_alloc(1), 1), *snapshot;
    struct dma_buf_import_sync_file data;
    struct wait_queue *q;
    int fd = dma_buf_fd(b, 0), fa = sync_file_create(a, "first");
    int fc = sync_file_create(c, "second");
    fd_entry_t *e = fd_get(&table, fd);
    dma_resv_add_fence(b->resv, r, 0);
    data.flags = DMA_BUF_SYNC_WRITE;
    data.fd = fa;
    CHECK(!buf_ioctl(e, DMA_BUF_IOCTL_IMPORT_SYNC_FILE, (uintptr_t)&data), "first writer import");
    data.fd = fc;
    CHECK(!buf_ioctl(e, DMA_BUF_IOCTL_IMPORT_SYNC_FILE, (uintptr_t)&data), "second writer import");
    snapshot = dma_resv_snapshot(b->resv, 1);
    CHECK(dma_fence_count(snapshot) == 3, "both writers and previous reader retained");
    dma_fence_put(snapshot);
    dma_fence_signal(c);
    CHECK(buf_poll(e, 5, &q) == 0, "second writer does not hide pending first writer");
    dma_fence_set_error(a, -EIO);
    dma_fence_signal(a);
    CHECK(buf_poll(e, 5, &q) == 1, "read ready but old reader still holds write access");
    snapshot = dma_resv_snapshot(b->resv, 0);
    CHECK(dma_fence_status(snapshot) == -EIO, "imported producer error retained");
    dma_fence_put(snapshot);
    dma_fence_signal(r);
    CHECK(buf_poll(e, 5, &q) == 5, "write ready only after every producer");
    close_fd(&table, fa);
    close_fd(&table, fc);
    close_fd(&table, fd);
    dma_buf_put(b);
    dma_fence_put(a);
    dma_fence_put(c);
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
    close_fd(&table, fd);
    dma_fence_put(f);
    data.value = 4;
    sw_ioctl(&timeline, SW_SYNC_IOC_CREATE_FENCE, (uintptr_t)&data);
    f = sync_file_get_fence(data.fence);
    sw_close(&timeline);
    CHECK(dma_fence_status(f) == -ENOENT, "producer close signals error");
    close_fd(&table, data.fence);
    dma_fence_put(f);
}
/** Whether only descriptor `fd` is in use or reserved. */
static int only_fd(int fd)
{
    int i;
    for (i = 0; i < 64; i++)
        if ((slots[i].type != FD_TYPE_NONE || slots[i].ofd != NULL) != (i == fd))
            return 0;
    return 1;
}
/** A sync file whose descriptor cannot be copied out is not made: each
 * call answers EFAULT and leaves no descriptor, and no sync file or fence
 * reference, which the leak sanitizer would report at exit. */
static void copy_out_cases(void)
{
    struct dma_fence *f = new_fence(dma_fence_context_alloc(1), 1);
    struct dma_buf *b = dma_buf_export(4096, &model_ops, NULL, NULL);
    struct dma_buf_export_sync_file exp;
    struct sync_merge_data merge;
    struct sw_sync_create_fence_data data;
    fd_entry_t timeline;
    int fd = sync_file_create(f, "one"), bfd;
    k_memset(&merge, 0, sizeof(merge));
    merge.fd2 = fd;
    to_user_faults = 1;
    CHECK(sync_ioctl(fd_get(&table, fd), SYNC_IOC_MERGE, (uintptr_t)&merge) == -EFAULT &&
              only_fd(fd),
          "a merge that cannot be copied out: EFAULT, nothing left");
    k_memset(&timeline, 0, sizeof(timeline));
    CHECK(!sw_sync_open(&timeline), "open software timeline");
    k_memset(&data, 0, sizeof(data));
    data.value = 1;
    CHECK(sw_ioctl(&timeline, SW_SYNC_IOC_CREATE_FENCE, (uintptr_t)&data) == -EFAULT && only_fd(fd),
          "a software fence that cannot be copied out: EFAULT, nothing left");
    sw_close(&timeline);
    to_user_faults = 0;
    CHECK(!dma_resv_add_fence(b->resv, f, 1), "a writer on a buffer");
    bfd = dma_buf_fd(b, 0);
    k_memset(&exp, 0, sizeof(exp));
    exp.flags = DMA_BUF_SYNC_WRITE;
    to_user_faults = 1;
    CHECK(fd_get(&table, bfd)
                  ->ops->ioctl(fd_get(&table, bfd), DMA_BUF_IOCTL_EXPORT_SYNC_FILE,
                               (uintptr_t)&exp) == -EFAULT,
          "an export that cannot be copied out: EFAULT");
    to_user_faults = 0;
    close_fd(&table, bfd);
    dma_buf_put(b);
    CHECK(only_fd(fd), "and nothing left");
    close_fd(&table, fd);
    dma_fence_put(f);
}
/** SET_NAME of a name with no NUL in 32 bytes and mmap through an
 * exporter without mmap both answer EINVAL, as Linux. */
static void buffer_answers(void)
{
    struct dma_buf *b = dma_buf_export(4096, &model_ops, NULL, NULL);
    char name[40];
    vma_t vma;
    int fd = dma_buf_fd(b, 0);
    fd_entry_t *e = fd_get(&table, fd);
    k_memset(name, 'x', sizeof(name));
    CHECK(e->ops->ioctl(e, DMA_BUF_SET_NAME_A, (uintptr_t)name) == -EINVAL,
          "a name of 32 bytes or more: EINVAL, as strndup_user");
    name[31] = 0;
    CHECK(!e->ops->ioctl(e, DMA_BUF_SET_NAME_A, (uintptr_t)name) && b->named, "a name of 31 bytes");
    k_memset(&vma, 0, sizeof(vma));
    vma.end = 4096;
    CHECK(e->ops->mmap(e, &vma) == -EINVAL, "mmap through an exporter without mmap: EINVAL");
    close_fd(&table, fd);
    dma_buf_put(b);
}
/** Whether two strings are equal. */
static int same(const char *x, const char *y)
{
    while (*x && *x == *y) {
        x++;
        y++;
    }
    return *x == *y;
}
/** The name FILE_INFO reports of sync file `fd`. */
static void name_of(int fd, char *out)
{
    struct sync_file_info info;
    k_memset(&info, 0, sizeof(info));
    CHECK(!sync_ioctl(fd_get(&table, fd), SYNC_IOC_FILE_INFO, (uintptr_t)&info), "file info");
    k_memcpy(out, info.name, sizeof(info.name));
}
/** A sync file is named as Linux's sync_file_get_name names it: the
 * user's name from MERGE, else "driver-timelineCONTEXT-SEQNO" of its
 * fence, whatever name CREATE_FENCE was given. */
static void name_cases(void)
{
    struct sw_sync_create_fence_data data;
    struct sync_merge_data merge;
    struct dma_fence *f;
    fd_entry_t timeline;
    char name[32], want[64];
    k_memset(&timeline, 0, sizeof(timeline));
    CHECK(!sw_sync_open(&timeline), "open software timeline");
    k_memset(&data, 0, sizeof(data));
    data.value = 5;
    k_strncpy(data.name, "ignored", sizeof(data.name));
    CHECK(!sw_ioctl(&timeline, SW_SYNC_IOC_CREATE_FENCE, (uintptr_t)&data), "a software fence");
    f = sync_file_get_fence(data.fence);
    snprintf(want, sizeof(want), "%s-%s%llu-%lld", f->ops->get_driver_name(f),
             f->ops->get_timeline_name(f), (unsigned long long)f->context, (long long)f->seqno);
    want[31] = 0;
    name_of(data.fence, name);
    CHECK(same(name, want), "named from its fence, not the caller's name");
    k_memset(&merge, 0, sizeof(merge));
    merge.fd2 = data.fence;
    k_strncpy(merge.name, "joined", sizeof(merge.name));
    CHECK(!sync_ioctl(fd_get(&table, data.fence), SYNC_IOC_MERGE, (uintptr_t)&merge), "merge");
    name_of(merge.fence, name);
    CHECK(same(name, "joined"), "a merge keeps the user's name");
    close_fd(&table, merge.fence);
    close_fd(&table, data.fence);
    dma_fence_put(f);
    sw_close(&timeline);
}
/** Run the model cases, or with `--interrupt-only` the interrupt's alone,
 * and fail on any assertion. */
int main(int argc, char **argv)
{
    int i;
    current_proc = &self;
    table.entries = slots;
    table.cap = 64;
    self.fds = &table;
    if (argc == 2 && same(argv[1], "--interrupt-only")) {
        interrupt_cases();
        goto finish;
    }
    fence_cases();
    interrupt_cases();
    merge_cases();
    buffer_cases();
    import_cases();
    timeline_cases();
    copy_out_cases();
    buffer_answers();
    name_cases();
finish:
    for (i = 0; i < 64; i++)
        CHECK(slots[i].type == FD_TYPE_NONE, "all descriptors closed");
    if (!failures)
        printf("fence-test: passed\n");
    return failures != 0;
}
