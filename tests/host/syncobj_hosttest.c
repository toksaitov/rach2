/* DRM syncobjs, binary and timeline, over the real fence, sync-file,
 * software timeline and syncobj code; syncobj64_hosttest.c runs the same
 * rows with x86-64's structures. Descriptors, eventfds, handle tables and
 * the scheduler are models, and a sleep runs the producer step a row arms.
 * host-deps: rach2/sys/lib/kstring.c rach2/sys/lib/kctypes.c
 * host-deps: tests/host/compat/fd_flat.c
 * host-deps: tests/host/compat/spinlock_host.c tests/host/compat/atomic_host.c
 */
#include "dmabuf/fence.c"
#include "dmabuf/sync_file.c"
#include "dmabuf/sw_sync.c"
#include "drm/drm_syncobj.c"

/** No interrupts on the host: the fences' masking is a no-op. */
uint32_t kirq_save(void)
{
    return 0;
}

/** The pair of kirq_save. */
void kirq_restore(uint32_t flags)
{
    (void)flags;
}

extern int printf(const char *format, ...);
extern void *calloc(unsigned long n, unsigned long size);
extern void free(void *p);

struct percpu cpu_table[NR_CPUS];
static process_t self;
static fd_table_t table;
static fd_entry_t slots[64];
static struct open_file descriptions[64];
static int failures, wait_mode, sleeps;
static int64_t now_ns = 1000000000ll;
static void (*on_sleep)(void);

#define CHECK(c, s)                                \
    do {                                           \
        if (!(c)) {                                \
            printf("syncobj-test: fail: %s\n", s); \
            failures++;                            \
        }                                          \
    } while (0)

/** The model's monotonic clock. */
void rtc_monotonic(int64_t *sec, int64_t *ns)
{
    *sec = now_ns / 1000000000ll;
    *ns = now_ns % 1000000000ll;
}
/** Milliseconds of the model's clock. */
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
/** Count a wake; the model has no other sleepers. */
void wq_wake(wait_queue_t *wq, uint32_t kinds)
{
    (void)kinds;
    wq->wakes[0]++;
}
/** Wake every kind. */
void wq_wake_all(wait_queue_t *wq)
{
    wq_wake(wq, WQ_ALL);
}
/** Sleep: run the producer step a row armed, or let the armed timer
 * expire, then require the wait's condition, as the real queue would. */
int wq_wait_event(wait_queue_t *wq, wq_cond_fn cond, void *arg)
{
    void (*step)(void) = on_sleep;
    (void)wq;
    sleeps++;
    if (cond(arg))
        return 0;
    if (wait_mode == 1)
        return -EINTR;
    on_sleep = NULL;
    if (step)
        step();
    else
        now_ns = (int64_t)current_proc->wakeup_tick * 1000000;
    CHECK(cond(arg), "wait condition after the scheduled event");
    return 0;
}
/** Uninterruptible waits ignore the model's signal. */
int wq_wait_event_nointr(wait_queue_t *wq, wq_cond_fn cond, void *arg)
{
    int saved = wait_mode, rc;
    wait_mode = 0;
    rc = wq_wait_event(wq, cond, arg);
    wait_mode = saved;
    return rc;
}
/** Copy from a test address, refusing the model's unmapped first page. */
int copy_from_user(void *dst, const void __user *src, size_t n)
{
    if ((uintptr_t)src < 4096)
        return -EFAULT;
    k_memcpy(dst, src, n);
    return 0;
}
/** Copy to a test address, refusing the first page. */
int copy_to_user(void __user *dst, const void *src, size_t n)
{
    if ((uintptr_t)dst < 4096)
        return -EFAULT;
    k_memcpy(dst, src, n);
    return 0;
}
/** Allocate a descriptor in the model's table. */
int fd_alloc(fd_table_t *t)
{
    int i;
    for (i = 0; i < 64; i++)
        if (t->chunks[0][i].type == FD_TYPE_NONE) {
            k_memset(&descriptions[i], 0, sizeof(descriptions[i]));
            t->chunks[0][i].ofd = &descriptions[i];
            t->chunks[0][i].type = FD_TYPE_FILE;
            return i;
        }
    return -EMFILE;
}
/** Find a live model descriptor. */
fd_entry_t *fd_get(fd_table_t *t, int fd)
{
    return fd >= 0 && fd < 64 && t->chunks[0][fd].type != FD_TYPE_NONE ? &t->chunks[0][fd] : NULL;
}
/** Close a model descriptor through its operations. */
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

static long allocations;
static int fail_after = -1;

/** Allocate zeroed storage, failing the chosen allocation. */
void *kmalloc(size_t size)
{
    void *p;
    if (fail_after == 0)
        return NULL;
    if (fail_after > 0)
        fail_after--;
    p = calloc(1, size ? size : 1);
    if (p)
        allocations++;
    return p;
}
/** Zeroed allocation over the same counter. */
void *kzalloc(size_t size)
{
    return kmalloc(size);
}
/** Free a counted allocation. */
void kfree(void *p)
{
    if (p)
        allocations--;
    free(p);
}

/* The handle table, as drm_gem.c keeps it: slots indexed by id, ids from
 * 1, the lowest free. */
int32_t drm_handle_insert(struct drm_handles *t, void *object, uint32_t *id)
{
    struct drm_handle *h, **slot;
    uint32_t want = 1;
    while (want < t->cap && t->slot[want])
        want++;
    if (want >= t->cap) {
        uint32_t cap = t->cap ? t->cap * 2 : 64;
        while (cap <= want)
            cap *= 2;
        slot = kmalloc(cap * sizeof(*slot));
        if (!slot)
            return -ENOMEM;
        k_memset(slot, 0, cap * sizeof(*slot));
        if (t->slot)
            k_memcpy(slot, t->slot, t->cap * sizeof(*slot));
        kfree(t->slot);
        t->slot = slot;
        t->cap = cap;
    }
    h = kmalloc(sizeof(*h));
    if (!h)
        return -ENOMEM;
    h->id = want;
    h->object = object;
    h->prime = NULL;
    t->slot[want] = h;
    *id = want;
    return 0;
}
/** Find a handle's object. */
void *drm_handle_find(struct drm_handles *t, uint32_t id)
{
    return id < t->cap && t->slot[id] ? t->slot[id]->object : NULL;
}
/** Find a handle's object and take a reference with `get`. */
void *drm_handle_find_get(struct drm_handles *t, uint32_t id, void (*get)(void *object))
{
    void *object = drm_handle_find(t, id);
    if (object)
        get(object);
    return object;
}
/** Take a handle out and return its object. */
void *drm_handle_remove(struct drm_handles *t, uint32_t id)
{
    struct drm_handle *h = id && id < t->cap ? t->slot[id] : NULL;
    void *object;
    if (!h)
        return NULL;
    object = h->object;
    t->slot[id] = NULL;
    kfree(h);
    return object;
}
/** The lowest id from `id` in use, or 0. */
uint32_t drm_handle_next(struct drm_handles *t, uint32_t id)
{
    for (; id < t->cap; id++)
        if (id && t->slot[id])
            return id;
    return 0;
}
/** Free the slots. */
void drm_handles_fini(struct drm_handles *t)
{
    kfree(t->slot);
    t->slot = NULL;
    t->cap = 0;
}

/* An eventfd: its references and the count of its signals. */
struct eventfd {
    int refs;
    uint64_t count;
};
/** Close a model eventfd descriptor. */
static void ev_close(fd_entry_t *e)
{
    eventfd_put(e->priv);
}
static const struct file_ops ev_ops = {NULL, NULL, NULL, NULL, ev_close, NULL, NULL, 0};
/** Take a reference to the eventfd of a descriptor, as eventfd_ctx_fdget. */
struct eventfd *eventfd_get(int fd, int32_t *err)
{
    fd_entry_t *e = fd_get(&table, fd);
    struct eventfd *ev;
    if (!e) {
        *err = -EBADF;
        return NULL;
    }
    if (e->ops != &ev_ops) {
        *err = -EINVAL;
        return NULL;
    }
    ev = e->priv;
    ev->refs++;
    return ev;
}
/** Drop an eventfd reference, freeing it with the last. */
void eventfd_put(struct eventfd *ev)
{
    if (--ev->refs == 0)
        kfree(ev);
}
/** Count a signal. */
void eventfd_signal(struct eventfd *ev)
{
    ev->count++;
}
/** A new model eventfd descriptor and a borrowed pointer to its counter. */
static int new_eventfd(struct eventfd **out)
{
    int fd = fd_alloc(&table);
    struct eventfd *ev = kzalloc(sizeof(*ev));
    ev->refs = 1;
    slots[fd].type = FD_TYPE_ANON;
    slots[fd].ops = &ev_ops;
    slots[fd].priv = ev;
    *out = ev;
    return fd;
}

static const struct drm_driver virtio = {DRM_IDENT_VIRTIO_GPU};
static const struct drm_driver bochs = {DRM_IDENT_BOCHS};
static struct drm_device gpu, binary_only, display;
static struct drm_file a, b;
static fd_entry_t producer;

/** A point of the software timeline, as a sync file descriptor. */
static int sw_point(fd_entry_t *tl, uint32_t value)
{
    struct sw_sync_create_fence_data data;
    k_memset(&data, 0, sizeof(data));
    data.value = value;
    CHECK(!sw_ioctl(tl, SW_SYNC_IOC_CREATE_FENCE, (uintptr_t)&data), "software point");
    return data.fence;
}
/** Advance a software timeline. */
static void sw_inc(fd_entry_t *tl, uint32_t step)
{
    sw_ioctl(tl, SW_SYNC_IOC_INC, (uintptr_t)&step);
}
/** Advance the producer's timeline by one, as a sleep's step. */
static void step_inc(void)
{
    sw_inc(&producer, 1);
}
/** Close the producer's timeline, failing its points, as a sleep's step. */
static void step_fail(void)
{
    sw_close(&producer);
}

/** Create a syncobj handle in `f`. */
static uint32_t create(struct drm_file *f, uint32_t flags)
{
    struct drm_syncobj_create c;
    c.handle = 0;
    c.flags = flags;
    CHECK(!drm_syncobj_create_ioctl(f->dev, &c, f), "create");
    return c.handle;
}
/** Convert between handle and descriptor, either way. */
static int32_t convert(struct drm_file *f, int to_fd, uint32_t handle, uint32_t flags, int32_t fd,
                       struct drm_syncobj_handle *out)
{
    struct drm_syncobj_handle h;
    int32_t rc;
    k_memset(&h, 0, sizeof(h));
    h.handle = handle;
    h.flags = flags;
    h.fd = fd;
    rc = to_fd ? drm_syncobj_handle_to_fd_ioctl(f->dev, &h, f)
               : drm_syncobj_fd_to_handle_ioctl(f->dev, &h, f);
    if (out)
        *out = h;
    return rc;
}
/** Import a sync file into a handle. */
static int32_t import(struct drm_file *f, uint32_t handle, int fd)
{
    return convert(f, 0, handle, DRM_SYNCOBJ_FD_TO_HANDLE_FLAGS_IMPORT_SYNC_FILE, fd, NULL);
}
/** Export a handle's fence as a sync file; the descriptor or -errno. */
static int32_t export(struct drm_file *f, uint32_t handle)
{
    struct drm_syncobj_handle h;
    int32_t rc = convert(f, 1, handle, DRM_SYNCOBJ_HANDLE_TO_FD_FLAGS_EXPORT_SYNC_FILE, -1, &h);
    return rc ? rc : h.fd;
}
/** SYNCOBJ_WAIT on handles. */
static int32_t wait(struct drm_file *f, uint32_t *handles, uint32_t n, uint32_t flags,
                    int64_t timeout, uint32_t *first)
{
    struct drm_syncobj_wait w;
    int32_t rc;
    k_memset(&w, 0, sizeof(w));
    w.handles = (uintptr_t)handles;
    w.count_handles = n;
    w.flags = flags;
    w.timeout_nsec = timeout;
    w.first_signaled = 77;
    w.deadline_nsec = (uint64_t)now_ns + 5;
    rc = drm_syncobj_wait_ioctl(f->dev, &w, f);
    if (first)
        *first = w.first_signaled;
    return rc;
}
/** SYNCOBJ_TIMELINE_WAIT on handles at points. */
static int32_t twait(struct drm_file *f, uint32_t *handles, uint64_t *points, uint32_t n,
                     uint32_t flags, int64_t timeout)
{
    struct drm_syncobj_timeline_wait w;
    k_memset(&w, 0, sizeof(w));
    w.handles = (uintptr_t)handles;
    w.points = (uintptr_t)points;
    w.count_handles = n;
    w.flags = flags;
    w.timeout_nsec = timeout;
    return drm_syncobj_timeline_wait_ioctl(f->dev, &w, f);
}
/** RESET or SIGNAL. */
static int32_t array(struct drm_file *f, int signal, uint32_t *handles, uint32_t n)
{
    struct drm_syncobj_array arr;
    arr.handles = (uintptr_t)handles;
    arr.count_handles = n;
    arr.pad = 0;
    return signal ? drm_syncobj_signal_ioctl(f->dev, &arr, f)
                  : drm_syncobj_reset_ioctl(f->dev, &arr, f);
}
/** TIMELINE_SIGNAL one handle at one point. */
static int32_t tsignal(struct drm_file *f, uint32_t handle, uint64_t point)
{
    struct drm_syncobj_timeline_array arr;
    arr.handles = (uintptr_t)&handle;
    arr.points = (uintptr_t)&point;
    arr.count_handles = 1;
    arr.flags = 0;
    return drm_syncobj_timeline_signal_ioctl(f->dev, &arr, f);
}
/** QUERY one handle; ~0 on error. */
static uint64_t query(struct drm_file *f, uint32_t handle, uint32_t flags)
{
    struct drm_syncobj_timeline_array arr;
    uint64_t point = ~0ull;
    arr.handles = (uintptr_t)&handle;
    arr.points = (uintptr_t)&point;
    arr.count_handles = 1;
    arr.flags = flags;
    if (drm_syncobj_query_ioctl(f->dev, &arr, f))
        return ~0ull;
    return point;
}
/** TRANSFER between points. */
static int32_t transfer(struct drm_file *f, uint32_t src, uint64_t src_point, uint32_t dst,
                        uint64_t dst_point, uint32_t flags)
{
    struct drm_syncobj_transfer t;
    k_memset(&t, 0, sizeof(t));
    t.src_handle = src;
    t.src_point = src_point;
    t.dst_handle = dst;
    t.dst_point = dst_point;
    t.flags = flags;
    return drm_syncobj_transfer_ioctl(f->dev, &t, f);
}
/** SYNC_IOC_FILE_INFO of a sync file: the status with rows asked for when
 * `rows` is set, else without; the row count in `*n` and the first rows. */
static int32_t file_info(int fd, int rows, uint32_t *n, struct sync_fence_info *out)
{
    struct sync_file_info info;
    k_memset(&info, 0, sizeof(info));
    if (rows) {
        info.num_fences = 4;
        info.sync_fence_info = (uintptr_t)out;
    }
    CHECK(!sync_ioctl(fd_get(&table, fd), SYNC_IOC_FILE_INFO, (uintptr_t)&info), "file info");
    *n = info.num_fences;
    return info.status;
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
/** The fence a handle holds, borrowed. */
static struct dma_fence *held(struct drm_file *f, uint32_t handle)
{
    return ((struct drm_syncobj *)drm_handle_find(&f->syncobjs, handle))->fence;
}

/** Create, destroy, convert, wait, reset and signal binary syncobjs. */
static void binary_cases(void)
{
    struct drm_syncobj_handle h;
    struct drm_syncobj_destroy d;
    struct dma_fence *f;
    uint32_t hs[2], first, other;
    int p1, fd, out;
    struct drm_syncobj_create c;

    hs[0] = create(&a, 0);
    hs[1] = create(&a, DRM_SYNCOBJ_CREATE_SIGNALED);
    CHECK(hs[0] == 1 && hs[1] == 2, "handles from 1");
    c.flags = 2;
    CHECK(drm_syncobj_create_ioctl(&gpu, &c, &a) == -EINVAL, "create refuses unknown flags");
    CHECK(wait(&a, hs, 1, 0, 0, NULL) == -EINVAL, "wait without a fence refuses");
    CHECK(wait(&a, hs, 1, DRM_SYNCOBJ_WAIT_FLAGS_WAIT_FOR_SUBMIT, 0, NULL) == -ETIME,
          "polled wait for submission times out");
    CHECK(!wait(&a, hs + 1, 1, 0, 0, &first) && first == 0, "created signalled");
    CHECK(wait(&a, hs, 0, 0, 0, NULL) == 0, "a wait on no handles: 0, as Linux");
    CHECK(wait(&a, hs, 2, 16, 0, NULL) == -EINVAL, "wait refuses unknown flags");
    CHECK(wait(&a, hs, 2, DRM_SYNCOBJ_WAIT_FLAGS_WAIT_AVAILABLE, 0, NULL) == -EINVAL,
          "binary wait refuses WAIT_AVAILABLE");
    CHECK(wait(&a, (uint32_t *)8, 2, 0, 0, NULL) == -EFAULT, "wait faults on bad handles");
    other = 99;
    CHECK(wait(&a, &other, 1, 0, 0, NULL) == -ENOENT, "wait refuses unknown handle");

    CHECK(!sw_sync_open(&producer), "open producer");
    p1 = sw_point(&producer, 1);
    CHECK(import(&a, 99, p1) == -ENOENT, "import into unknown handle");
    CHECK(import(&a, hs[0], 5) == -EINVAL, "import of no sync file");
    CHECK(!import(&a, hs[0], p1), "import sync file");
    CHECK(held(&a, hs[0]) == sync_file_get_fence(p1), "binary syncobj holds the fence itself");
    dma_fence_put(held(&a, hs[0]));
    CHECK(wait(&a, hs, 1, 0, 0, NULL) == -ETIME, "poll an active fence");
    CHECK(!wait(&a, hs, 2, 0, 0, &first) && first == 1, "any reports the signalled one");
    CHECK(wait(&a, hs, 2, DRM_SYNCOBJ_WAIT_FLAGS_WAIT_ALL, 0, NULL) == -ETIME, "poll all");
    wait_mode = 1;
    CHECK(wait(&a, hs, 2, DRM_SYNCOBJ_WAIT_FLAGS_WAIT_ALL, now_ns + 1000000000ll, NULL) == -EINTR,
          "a signal breaks the wait");
    wait_mode = 0;
    CHECK(wait(&a, hs, 1, 0, now_ns + 1000000, NULL) == -ETIME, "timed wait expires");
    on_sleep = step_inc;
    CHECK(!wait(&a, hs, 2, DRM_SYNCOBJ_WAIT_FLAGS_WAIT_ALL | DRM_SYNCOBJ_WAIT_FLAGS_WAIT_DEADLINE,
                now_ns + 1000000000ll, &first) &&
              first == 1,
          "wait all ends when the producer signals");
    f = sync_file_get_fence(p1);
    CHECK(f->deadline_ns != 0x7fffffffffffffffll, "deadline hint reached");
    dma_fence_put(f);
    CHECK(!array(&a, 0, hs, 2) && !held(&a, hs[0]) && !held(&a, hs[1]), "reset drops fences");
    CHECK(array(&a, 0, hs, 0) == -EINVAL, "reset refuses no handles");
    CHECK(!array(&a, 1, hs, 1) && dma_fence_is_signaled(held(&a, hs[0])), "signal");

    /* A point submitted while the waiter sleeps. */
    CHECK(!array(&a, 0, hs, 1), "reset again");
    fd = sw_point(&producer, 2);
    CHECK(export(&a, hs[0]) == -EINVAL, "export without a fence refuses");
    CHECK(export(&a, 77) == -ENOENT, "export of an unknown handle");
    CHECK(!import(&a, hs[0], fd), "import second point");
    out = export(&a, hs[0]);
    f = sync_file_get_fence(out);
    CHECK(out >= 0 && f == held(&a, hs[0]), "export shares the fence");
    dma_fence_put(f);
    fd_free(&table, out);
    fd_free(&table, fd);
    fd_free(&table, p1);

    /* Files: the same object in another open file. */
    CHECK(convert(&a, 1, 42, 0, -1, NULL) == -EINVAL, "handle to fd of an unknown handle");
    CHECK(convert(&a, 1, hs[0], 2, -1, NULL) == -EINVAL, "handle to fd refuses unknown flags");
    k_memset(&h, 0, sizeof(h));
    h.pad = 1;
    CHECK(drm_syncobj_handle_to_fd_ioctl(&gpu, &h, &a) == -EINVAL, "handle to fd refuses pad");
    CHECK(!convert(&a, 1, hs[0], 0, -1, &h), "handle to fd");
    fd = h.fd;
    CHECK((slots[fd].flags & FD_FLAG_CLOEXEC) &&
              same(((struct anon_obj *)slots[fd].priv)->name, "syncobj_file"),
          "syncobj file is close-on-exec and named as Linux's");
    CHECK(convert(&b, 0, 0, 0, -1, NULL) == -EINVAL, "fd to handle refuses a bad descriptor");
    out = export(&a, hs[0]);
    CHECK(convert(&b, 0, 0, 0, out, NULL) == -EINVAL, "fd to handle refuses a sync file");
    fd_free(&table, out);
    CHECK(!convert(&b, 0, 0, 0, fd, &h), "fd to handle");
    CHECK(drm_handle_find(&b.syncobjs, h.handle) == drm_handle_find(&a.syncobjs, hs[0]),
          "both files hold one syncobj");
    d.handle = hs[0];
    d.pad = 1;
    CHECK(drm_syncobj_destroy_ioctl(&gpu, &d, &a) == -EINVAL, "destroy refuses pad");
    d.pad = 0;
    CHECK(!drm_syncobj_destroy_ioctl(&gpu, &d, &a), "destroy");
    CHECK(drm_syncobj_destroy_ioctl(&gpu, &d, &a) == -EINVAL, "destroy twice");
    fd_free(&table, fd);
    on_sleep = step_inc;
    CHECK(!wait(&b, &h.handle, 1, 0, now_ns + 1000000000ll, NULL),
          "the other file's handle still waits");
    hs[0] = create(&a, 0);
    CHECK(hs[0] == 1, "freed handle reused");
    drm_syncobj_release(&b);
}

static uint32_t submit_handle;
static int submit_fd;

/** Import a prepared sync file into a prepared handle, as a sleep's step. */
static void step_submit(void)
{
    CHECK(!import(&a, submit_handle, submit_fd), "submission during the sleep");
}
/** Submit, then signal on the next sleep. */
static void step_submit_then_inc(void)
{
    step_submit();
    on_sleep = step_inc;
}

/** Waits for a point that is submitted while the waiter sleeps. */
static void submit_cases(void)
{
    uint32_t h = create(&a, 0), first;
    submit_handle = h;
    submit_fd = sw_point(&producer, 3);
    on_sleep = step_submit_then_inc;
    CHECK(!wait(&a, &h, 1, DRM_SYNCOBJ_WAIT_FLAGS_WAIT_FOR_SUBMIT, now_ns + 1000000000ll, &first) &&
              first == 0 && sleeps >= 2,
          "wait for submission, then for the fence");
    CHECK(!((struct drm_syncobj *)drm_handle_find(&a.syncobjs, h))->waits, "wait left the list");
    fd_free(&table, submit_fd);
}

/** A producer's error, which every consumer reads through the fence. */
static void error_cases(void)
{
    uint32_t bin = create(&a, 0), tl = create(&a, 0), other;
    struct drm_syncobj_handle h;
    struct sync_fence_info rows[4];
    uint32_t n;
    int p, e1, e2, e3, fd;

    p = sw_point(&producer, 10);
    CHECK(!import(&a, bin, p), "import a point that will fail");
    CHECK(!transfer(&a, bin, 0, tl, 3, 0), "transfer it to a timeline point");
    CHECK(!convert(&a, 1, bin, 0, -1, &h), "share the syncobj");
    fd = h.fd;
    CHECK(!convert(&b, 0, 0, 0, fd, &h), "second consumer's handle");
    other = h.handle;
    on_sleep = step_fail;
    CHECK(!wait(&a, &bin, 1, 0, now_ns + 1000000000ll, NULL),
          "a failed fence ends a syncobj wait without error, as on Linux");
    e1 = export(&a, bin);
    e2 = export(&b, other);
    e3 = export(&a, tl);
    CHECK(file_info(e1, 0, &n, rows) == -ENOENT && n == 1, "first consumer reads the error");
    CHECK(file_info(e2, 1, &n, rows) == -ENOENT && rows[0].status == -ENOENT,
          "second consumer reads the error");
    CHECK(file_info(e3, 1, &n, rows) == -ENOENT && n == 1 && rows[0].status == -ENOENT,
          "the timeline's leaf carries the error");
    CHECK(file_info(e3, 0, &n, rows) == 1, "the timeline point itself signals without it");
    CHECK(query(&a, tl, 0) == 3, "a failed point is reached");
    fd_free(&table, e1);
    fd_free(&table, e2);
    fd_free(&table, e3);
    fd_free(&table, fd);
    fd_free(&table, p);
    drm_syncobj_release(&b);
    CHECK(!sw_sync_open(&producer), "reopen producer");
}

/** Transfers of a timeline's points to a binary syncobj, and the refusals
 * of TRANSFER and QUERY. */
static void transfer_cases(uint32_t tl, uint32_t bin2)
{
    CHECK(!transfer(&a, tl, 9, bin2, 0, 0) && dma_fence_is_signaled(held(&a, bin2)),
          "transfer a point to a binary syncobj");
    CHECK(transfer(&a, tl, 12, bin2, 0, 0) == -EINVAL, "transfer of a point not submitted");
    CHECK(transfer(&a, tl, 12, bin2, 0, DRM_SYNCOBJ_WAIT_FLAGS_WAIT_FOR_SUBMIT) == -ETIME,
          "transfer waits five seconds for submission");
    CHECK(transfer(&a, 40, 0, bin2, 0, 0) == -ENOENT, "transfer from an unknown handle");
    CHECK(transfer(&a, tl, 0, 40, 0, 0) == -ENOENT, "transfer to an unknown handle");
    CHECK(query(&a, 40, 0) == ~0ull, "query of an unknown handle");
    CHECK(query(&a, tl, 2) == ~0ull, "query refuses unknown flags");
    CHECK(transfer(&a, tl, 9, bin2, 0, DRM_SYNCOBJ_WAIT_FLAGS_WAIT_ALL) == -EINVAL,
          "transfer refuses a flag but WAIT_FOR_SUBMIT, as Linux");
    CHECK(twait(&a, &bin2, NULL, 0, 0, 0) == 0, "a timeline wait on no handles: 0, as Linux");
    CHECK(tsignal(&a, tl, 0) == 0 && query(&a, tl, 0) == 9 &&
              query(&a, tl, DRM_SYNCOBJ_QUERY_FLAGS_LAST_SUBMITTED) == 9 &&
              held(&a, tl)->seqno == 9,
          "point 0 is chained, at the newest's number, as Linux");
}

/** Timeline points out of order, waits, transfers, queries and eventfds. */
static void timeline_cases(void)
{
    uint32_t tl = create(&a, 0), bin = create(&a, 0), bin2 = create(&a, 0), hs[2];
    fd_entry_t second;
    uint64_t pts[2];
    struct sync_fence_info rows[4];
    struct eventfd *ev, *ev2, *ev3;
    struct drm_syncobj_eventfd e;
    uint32_t n;
    int pa, pb, x, efd, efd2, efd3;

    CHECK(query(&a, tl, 0) == 0 && query(&a, bin, 0) == 0, "nothing submitted reads 0");
    k_memset(&second, 0, sizeof(second));
    CHECK(!sw_sync_open(&second), "second producer");
    pa = sw_point(&producer, 1);
    pb = sw_point(&second, 1);
    CHECK(!import(&a, bin, pa) && !import(&a, bin2, pb), "two producers");
    CHECK(!transfer(&a, bin, 0, tl, 5, 0) && !transfer(&a, bin2, 0, tl, 7, 0), "points 5 and 7");
    CHECK(query(&a, tl, 0) == 0, "no point reached");
    CHECK(query(&a, tl, DRM_SYNCOBJ_QUERY_FLAGS_LAST_SUBMITTED) == 7, "last submitted");
    hs[0] = tl;
    pts[0] = 5;
    CHECK(twait(&a, hs, pts, 1, 0, 0) == -ETIME, "point 5 pending");
    pts[0] = 9;
    CHECK(twait(&a, hs, pts, 1, 0, 0) == -EINVAL, "point 9 not submitted");
    CHECK(twait(&a, hs, pts, 1, DRM_SYNCOBJ_WAIT_FLAGS_WAIT_FOR_SUBMIT, 0) == -ETIME,
          "point 9 polled for submission");
    pts[0] = 7;
    CHECK(!twait(&a, hs, pts, 1, DRM_SYNCOBJ_WAIT_FLAGS_WAIT_AVAILABLE, 0), "point 7 available");

    x = export(&a, tl);
    CHECK(file_info(x, 1, &n, rows) == 0 && n == 2,
          "the newest point's sync file lists both leaves");
    fd_free(&table, x);

    efd = new_eventfd(&ev);
    efd2 = new_eventfd(&ev2);
    efd3 = new_eventfd(&ev3);
    k_memset(&e, 0, sizeof(e));
    e.handle = tl;
    e.point = 7;
    e.fd = efd;
    CHECK(!drm_syncobj_eventfd_ioctl(&gpu, &e, &a) && !ev->count, "eventfd waits for point 7");
    e.point = 9;
    e.fd = efd2;
    CHECK(!drm_syncobj_eventfd_ioctl(&gpu, &e, &a) && !ev2->count, "eventfd waits for point 9");
    e.point = 7;
    e.fd = efd3;
    e.flags = DRM_SYNCOBJ_WAIT_FLAGS_WAIT_AVAILABLE;
    CHECK(!drm_syncobj_eventfd_ioctl(&gpu, &e, &a) && ev3->count == 1,
          "eventfd for a point available signals at once");
    e.flags = 2;
    CHECK(drm_syncobj_eventfd_ioctl(&gpu, &e, &a) == -EINVAL, "eventfd refuses unknown flags");
    e.flags = 0;
    e.pad = 1;
    CHECK(drm_syncobj_eventfd_ioctl(&gpu, &e, &a) == -EINVAL, "eventfd refuses pad");
    e.pad = 0;
    e.fd = pa;
    CHECK(drm_syncobj_eventfd_ioctl(&gpu, &e, &a) == -EINVAL, "eventfd refuses another file");
    e.fd = 63;
    CHECK(drm_syncobj_eventfd_ioctl(&gpu, &e, &a) == -EBADF, "eventfd refuses no file");
    e.handle = 50;
    CHECK(drm_syncobj_eventfd_ioctl(&gpu, &e, &a) == -ENOENT, "eventfd refuses unknown handle");
    fd_free(&table, efd3);

    /* The second producer first: point 5 still waits on the first. */
    sw_inc(&second, 1);
    CHECK(query(&a, tl, 0) == 0 && !ev->count, "point 7's fence alone reaches nothing");
    pts[0] = 6;
    CHECK(twait(&a, hs, pts, 1, 0, 0) == -ETIME, "point 6 is covered by 7, which waits on 5");
    sw_inc(&producer, 1);
    CHECK(query(&a, tl, 0) == 7 && ev->count == 1, "both reached");
    fd_free(&table, efd);
    pts[0] = 0;
    CHECK(!twait(&a, hs, pts, 1, 0, 0), "point 0 waits for the newest");

    /* Out of order: point 3 after 7 starts a context of its own at 7. */
    CHECK(!tsignal(&a, tl, 3), "signal point 3 after 7");
    CHECK(held(&a, tl)->seqno == 7 && query(&a, tl, 0) == 7, "out-of-order point takes 7");
    fd_free(&table, pa);
    pa = sw_point(&producer, 2);
    CHECK(!import(&a, bin, pa) && !transfer(&a, bin, 0, tl, 4, 0), "point 4 after it, pending");
    CHECK(query(&a, tl, 0) == 0, "a pending point out of order hides those before it");
    CHECK(!tsignal(&a, tl, 9) && ev2->count == 0, "point 9 submitted waits on point 4");
    pts[0] = 8;
    CHECK(twait(&a, hs, pts, 1, 0, 0) == -ETIME, "point 8 covered by 9, which waits on 4");
    on_sleep = step_inc;
    CHECK(!twait(&a, hs, pts, 1, 0, now_ns + 1000000000ll), "point 8 after the producer");
    CHECK(query(&a, tl, 0) == 9 && ev2->count == 1, "every point reached, and its eventfd");
    fd_free(&table, efd2);

    transfer_cases(tl, bin2);
    fd_free(&table, pa);
    fd_free(&table, pb);
    sw_close(&second);
}

/** The calls answer only where the driver declares the feature. */
static void feature_cases(void)
{
    struct drm_file fa, fb;
    uint8_t args[64];
    struct drm_syncobj_create c;
    int i;
    static const struct {
        drm_ioctl_t fn;
        int timeline;
    } calls[] = {{drm_syncobj_create_ioctl, 0},
                 {drm_syncobj_destroy_ioctl, 0},
                 {drm_syncobj_handle_to_fd_ioctl, 0},
                 {drm_syncobj_fd_to_handle_ioctl, 0},
                 {drm_syncobj_wait_ioctl, 0},
                 {drm_syncobj_reset_ioctl, 0},
                 {drm_syncobj_signal_ioctl, 0},
                 {drm_syncobj_transfer_ioctl, 1},
                 {drm_syncobj_timeline_wait_ioctl, 1},
                 {drm_syncobj_eventfd_ioctl, 1},
                 {drm_syncobj_timeline_signal_ioctl, 1},
                 {drm_syncobj_query_ioctl, 1}};
    k_memset(&fa, 0, sizeof(fa));
    k_memset(&fb, 0, sizeof(fb));
    fa.dev = &display;
    fb.dev = &binary_only;
    for (i = 0; i < (int)(sizeof(calls) / sizeof(calls[0])); i++) {
        k_memset(args, 0, sizeof(args));
        CHECK(calls[i].fn(&display, args, &fa) == -EOPNOTSUPP, "no syncobjs on bochs");
        k_memset(args, 0, sizeof(args));
        CHECK((calls[i].fn(&binary_only, args, &fb) == -EOPNOTSUPP) == calls[i].timeline,
              "binary-only driver refuses the timeline calls alone");
    }
    c.flags = 0;
    CHECK(!drm_syncobj_create_ioctl(&binary_only, &c, &fb) && drm_handle_next(&fb.syncobjs, 1),
          "binary driver creates");
    drm_syncobj_release(&fb);
    CHECK(!drm_handle_next(&fa.syncobjs, 1) && !drm_handle_next(&fb.syncobjs, 1),
          "nothing left on the refused files");
}

/** Allocation failures leave nothing changed. */
static void failure_cases(void)
{
    struct drm_syncobj_create c;
    uint32_t h = create(&a, 0);
    long before;
    int i;
    c.flags = 0;
    for (i = 0; i < 2; i++) {
        before = allocations;
        fail_after = i;
        CHECK(drm_syncobj_create_ioctl(&gpu, &c, &a) == -ENOMEM && allocations == before,
              "create fails whole");
        fail_after = -1;
    }
    for (i = 0; i < 5; i++) {
        before = allocations;
        fail_after = i;
        CHECK(tsignal(&a, h, 2) == -ENOMEM && allocations == before && !held(&a, h),
              "timeline signal fails whole");
        fail_after = -1;
    }
    before = allocations;
    fail_after = 0;
    CHECK(wait(&a, &h, 1, 0, 0, NULL) == -ENOMEM && allocations == before, "wait fails whole");
    fail_after = -1;
}

/** Many points: signalled ones are dropped, and a long chain frees without
 * recursion. */
static void chain_cases(void)
{
    long before = allocations;
    uint32_t h = create(&a, 0), i;
    struct drm_syncobj *s = drm_handle_find(&a.syncobjs, h);
    struct drm_syncobj_destroy d;
    struct dma_fence *f;
    for (i = 1; i <= 1000; i++)
        tsignal(&a, h, i);
    CHECK(allocations - before == 3 && query(&a, h, 0) == 1000, "signalled points are dropped");
    for (i = 1001; i <= 200000; i++) {
        f = kzalloc(sizeof(*f));
        dma_fence_init(f, NULL, 7, i);
        drm_syncobj_add_point(s, dma_fence_chain_alloc(), f, i);
        dma_fence_put(f);
    }
    CHECK(query(&a, h, DRM_SYNCOBJ_QUERY_FLAGS_LAST_SUBMITTED) == 200000, "200,000 points");
    d.handle = h;
    d.pad = 0;
    CHECK(!drm_syncobj_destroy_ioctl(&gpu, &d, &a) && allocations == before,
          "a long chain frees whole");
}

/** Run the rows on one ABI's structures. */
int main(void)
{
    long base;
    table.chunks[0] = slots;
    table.cap = 64;
    self.fds = &table;
    cpu_table[0].current = &self;
    gpu.driver = &virtio;
    gpu.features = virtio.ident.features;
    binary_only.driver = &virtio;
    binary_only.features = DRIVER_GEM | DRIVER_RENDER | DRIVER_SYNCOBJ;
    display.driver = &bochs;
    display.features = bochs.ident.features;
    a.dev = &gpu;
    b.dev = &gpu;
    dma_fence_put(dma_fence_get_stub());
    base = allocations;
    binary_cases();
    submit_cases();
    error_cases();
    timeline_cases();
    feature_cases();
    failure_cases();
    chain_cases();
    drm_syncobj_release(&a);
    drm_syncobj_release(&b);
    sw_close(&producer);
    CHECK(allocations == base, "every allocation released");
    {
        int i;
        for (i = 0; i < 64; i++)
            CHECK(slots[i].type == FD_TYPE_NONE, "every descriptor closed");
    }
    printf("syncobj-test: %s\n", failures ? "failed" : "ok");
    return failures != 0;
}
