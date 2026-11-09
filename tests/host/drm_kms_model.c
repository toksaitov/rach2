/*
 * drm_kms_model.c: the kernel around rach2/sys/drm's display code, for the
 * host tests that include drm_mode.c, drm_atomic.c, drm_legacy.c and
 * drm_edid.c whole: allocation with a live count, user copies, the clock,
 * waits that never sleep, events, buffer objects and fences.
 */

#include "drm/drm_mode.c"
#include "drm/drm_atomic.c"
#include "drm/drm_legacy.c"
#include "drm/drm_edid.c"

extern int printf(const char *format, ...);
extern void *calloc(unsigned long n, unsigned long size);
extern void free(void *p);
extern int strcmp(const char *a, const char *b);

struct percpu cpu_table[NR_CPUS];
static process_t kms_self;
static int failures;
static const char *test_name = "drm-kms-test";

#define CHECK(c, s)                                   \
    do {                                              \
        if (!(c)) {                                   \
            printf("%s: fail: %s\n", test_name, (s)); \
            failures++;                               \
        }                                             \
    } while (0)

/* ---- memory ---- */

static int allocations, fail_after = -1;

/** Zeroed host memory, counted; the `fail_after`-th next call fails. */
void *kmalloc(size_t size)
{
    void *p;

    if (fail_after == 0) {
        return NULL;
    }
    if (fail_after > 0) {
        fail_after--;
    }
    p = calloc(1, size ? size : 1);
    if (p) {
        allocations++;
    }
    return p;
}

/** Zeroed memory over the same count. */
void *kzalloc(size_t size)
{
    return kmalloc(size);
}

/** Free and uncount; NULL is allowed. */
void kfree(void *p)
{
    if (p) {
        allocations--;
    }
    free(p);
}

/* ---- user memory: a test pointer, and one below 4096 faults ---- */

/** Copy from a test address. */
int copy_from_user(void *dst, const void __user *src, size_t n)
{
    if ((uintptr_t)src < 4096) {
        return -EFAULT;
    }
    k_memcpy(dst, (const void *)src, n);
    return 0;
}

/** Copy to a test address. */
int copy_to_user(void __user *dst, const void *src, size_t n)
{
    if ((uintptr_t)dst < 4096) {
        return -EFAULT;
    }
    k_memcpy((void *)dst, src, n);
    return 0;
}

/** A user pointer of a test object. */
#define UPTR(p) ((uint64_t)(uintptr_t)(p))

/* ---- time and waits: one thread, so the commit lock is always free ---- */

/** A fixed monotonic time, 5.25 s. */
void rtc_monotonic(int64_t *sec, int64_t *nsec)
{
    *sec = 5;
    *nsec = 250000000;
}

/** Initialize a queue. */
void wait_queue_init(wait_queue_t *wq)
{
    k_memset(wq, 0, sizeof(*wq));
}

static int wakes;

/** Count a wake. */
void wq_wake_all(wait_queue_t *wq)
{
    (void)wq;
    wakes++;
}

/** A wait whose condition holds, as nothing else runs. */
int wq_wait_event(wait_queue_t *wq, wq_cond_fn cond, void *arg)
{
    (void)wq;
    CHECK(cond(arg), "a wait's condition holds in one thread");
    return 0;
}

/** The same, uninterruptible. */
int wq_wait_event_nointr(wait_queue_t *wq, wq_cond_fn cond, void *arg)
{
    return wq_wait_event(wq, cond, arg);
}

/* ---- events: reserved against the file's space and recorded when sent ---- */

#define MAX_SENT 16
static struct drm_event_vblank sent[MAX_SENT];
static int nsent, reserved;

/** Reserve `e` on `file`, as drm.c does. */
int32_t drm_event_reserve(struct drm_file *file, struct drm_pending_event *e)
{
    if (file->event_space < e->length) {
        return -ENOMEM;
    }
    file->event_space -= e->length;
    e->file = file;
    reserved++;
    return 0;
}

/** Record `e`'s bytes, give back its space and free it. */
void drm_send_event(struct drm_pending_event *e)
{
    if (nsent < MAX_SENT) {
        sent[nsent++] = e->event.vbl;
    }
    e->file->event_space += e->length;
    reserved--;
    kfree(e);
}

/** Take back a reserved `e`. */
void drm_event_cancel_free(struct drm_pending_event *e)
{
    e->file->event_space += e->length;
    reserved--;
    kfree(e);
}

/* ---- buffer objects: a table of handles over a few objects ---- */

#define MAX_GEM     8
#define MAX_HANDLES 32
static struct drm_gem_object gem[MAX_GEM];
static struct {
    struct drm_file *file;
    uint32_t id;
    struct drm_gem_object *obj;
} handles[MAX_HANDLES];

/** Make object `i` of `size` bytes and give `file` a handle to it; the
 * handle. */
static uint32_t gem_new(struct drm_file *file, int i, uint64_t size)
{
    uint32_t h = 0;

    gem[i].size = size;
    gem[i].refs = 1;
    CHECK(drm_gem_handle_create(file, &gem[i], &h) == 0, "a model handle is made");
    gem[i].refs--;
    return h;
}

/** Take a reference. */
void drm_gem_object_get(struct drm_gem_object *obj)
{
    if (obj) {
        obj->refs++;
    }
}

/** Drop a reference. */
void drm_gem_object_put(struct drm_gem_object *obj)
{
    if (obj) {
        CHECK(obj->refs > 0, "a buffer object is put no more than it is held");
        obj->refs--;
    }
}

/** The lowest free handle of `file` from 1, holding `obj`. */
int32_t drm_gem_handle_create(struct drm_file *file, struct drm_gem_object *obj, uint32_t *handle)
{
    uint32_t id;
    int i, free_slot = -1, taken;

    for (id = 1;; id++) {
        taken = 0;
        for (i = 0; i < MAX_HANDLES; i++) {
            if (handles[i].file == file && handles[i].id == id) {
                taken = 1;
            }
            if (!handles[i].file && free_slot < 0) {
                free_slot = i;
            }
        }
        if (!taken) {
            break;
        }
    }
    if (free_slot < 0) {
        return -ENOMEM;
    }
    handles[free_slot].file = file;
    handles[free_slot].id = id;
    handles[free_slot].obj = obj;
    obj->refs++;
    *handle = id;
    return 0;
}

/** The object of `handle`, held, or NULL. */
struct drm_gem_object *drm_gem_object_lookup(struct drm_file *file, uint32_t handle)
{
    int i;

    for (i = 0; i < MAX_HANDLES; i++) {
        if (handles[i].file == file && handles[i].id == handle) {
            handles[i].obj->refs++;
            return handles[i].obj;
        }
    }
    return NULL;
}

/** Drop `handle`: 0 or -EINVAL. */
int32_t drm_gem_handle_delete(struct drm_file *file, uint32_t handle)
{
    int i;

    for (i = 0; i < MAX_HANDLES; i++) {
        if (handles[i].file == file && handles[i].id == handle) {
            handles[i].obj->refs--;
            handles[i].file = NULL;
            return 0;
        }
    }
    return -EINVAL;
}

/* ---- fences: descriptor 5 is a sync file of one fence ---- */

static struct dma_fence in_fence;
static int fence_refs, fence_waits, fence_wait_rc;

/** The fence of sync file `fd`, held, or NULL. */
struct dma_fence *sync_file_get_fence(int32_t fd)
{
    if (fd != 5) {
        return NULL;
    }
    fence_refs++;
    return &in_fence;
}

/** Drop a fence; NULL is allowed. */
void dma_fence_put(struct dma_fence *fence)
{
    if (fence) {
        fence_refs--;
    }
}

/** A wait that ends at once with `fence_wait_rc`. */
int32_t dma_fence_wait_until(struct dma_fence *fence, int intr, int64_t deadline_ns)
{
    (void)fence;
    (void)deadline_ns;
    CHECK(intr, "a commit waits for its fences interruptibly");
    fence_waits++;
    return fence_wait_rc;
}

/* ---- files ---- */

/** Open `file` on `dev`, the root process's, with Linux's event space. */
static void file_open(struct drm_file *file, struct drm_device *dev)
{
    k_memset(file, 0, sizeof(*file));
    file->dev = dev;
    file->authenticated = 1;
    file->event_space = 4096;
}

/** Set up the current process, root. */
static void model_start(const char *name)
{
    test_name = name;
    cpu_table[0].current = &kms_self;
}
