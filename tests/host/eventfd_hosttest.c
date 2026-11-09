/**
 * Check that an eventfd read takes its value before the copy to the caller,
 * and the kernel's side of an eventfd: references, signals and hooks.
 *
 * host-deps: tests/host/compat/spinlock_host.c
 * host-cflags: -Wno-pointer-to-int-cast -Wno-int-to-pointer-cast
 *
 * The eventfd code is the kernel's, included whole to reach its static read.
 * The copy to the caller runs a second read of the same counter first, as a
 * thread sharing the descriptor can while the copy sleeps in a page fault:
 * the second read must find the counter taken, the first must return the
 * whole value, and the counter must end at zero, not wrap below it. With
 * EFD_SEMAPHORE and a counter of 2, each of the two reads takes one. A copy
 * that fails loses the value it took, as in Linux. A read wakes the counter's
 * queue for room alone and a write for data alone, so an edge-triggered epoll
 * registration for output is not told of a write.
 *
 * The kernel's side: a reference from a descriptor, refused for a
 * descriptor that is not an eventfd or no descriptor; a signal from the
 * kernel, which wakes the readers and tells each hook, and a hook that
 * takes the counter, as irqfd does; a program's write telling the hooks
 * the same way; a hook added to a counter already raised, which is told
 * so; the last close, which detaches every hook and tells it, keeps the
 * counter for a kernel holder and frees it at that holder's put; a signal
 * from inside a hangup, the callback racing the close, which reaches no
 * detached hook; a writer whose copy from the caller runs the close, so
 * the write lands after the hooks are gone; and a hook added after the
 * close, whose hangup runs at once. The spinlock is the host's model,
 * which stops the test when a hook's callback takes the counter's lock
 * again.
 */
#include "io/eventfd.c"
#include "lib/kstring.h"

extern int printf(const char *format, ...);

static struct eventfd counter;
static fd_entry_t entry;
static struct open_file description;
static process_t reader;
struct percpu cpu_table[NR_CPUS] = {{.current = &reader}};
/* The caller's buffer. */
static uint64_t user[4];
static int failures, wakes, inner_reads;
/* The kinds of every wake since `setup`, ORed. */
static uint32_t wake_kinds;
/* The result and value of the read made during a copy. */
static int32_t inner_result;
static uint64_t inner_value;
/* Whether the next copy to the caller runs a second read first. */
static int nested;
/* Whether the next copy from the caller runs the last close first. */
static int close_in_copy;
/* The last pointer `kfree` was given, and how many it was given. */
static void *freed;
static int frees;
/* A descriptor that is open on something other than an eventfd. */
static fd_entry_t other;

/** Count and print a failure when `condition` is false. */
#define CHECK(condition, detail)                              \
    do {                                                      \
        if (!(condition)) {                                   \
            printf("eventfd-hosttest: failed: %s\n", detail); \
            failures++;                                       \
        }                                                     \
    } while (0)

/**
 * The copy to the caller. When `nested` is set, a second read of the same
 * counter runs first, as another thread would while the copy sleeps. A
 * destination of 0 is a buffer that cannot be written.
 */
int copy_to_user(void __user *dst, const void *src, size_t len)
{
    if (nested) {
        nested = 0;
        inner_reads++;
        inner_result = eventfd_read(&entry, (char *)&inner_value, sizeof(inner_value));
    }
    if (!dst) {
        return -EFAULT;
    }
    k_memcpy(dst, src, len);
    return 0;
}

/** Copy `len` bytes from the caller; 0. When `close_in_copy` is set, the
 * last close runs first, as another thread's close could while the copy
 * sleeps if the writer did not hold the descriptor. */
int copy_from_user(void *dst, const void __user *src, size_t len)
{
    if (close_in_copy) {
        close_in_copy = 0;
        eventfd_close(&entry);
    }
    k_memcpy(dst, src, len);
    return 0;
}

/** A read that would wait is interrupted; the rows use nonblocking reads. */
int wq_wait_event(wait_queue_t *wq, wq_cond_fn cond, void *arg)
{
    (void)wq;
    (void)cond;
    (void)arg;
    return -EINTR;
}

/** Count a wake and note its kinds. */
void wq_wake(wait_queue_t *wq, uint32_t kinds)
{
    (void)wq;
    wakes++;
    wake_kinds |= kinds;
}

/** Make `wq` empty. */
void wait_queue_init(wait_queue_t *wq)
{
    wq->head = NULL;
}

/** Making a descriptor is outside this test: NULL. */
void *kzalloc(size_t n)
{
    (void)n;
    return NULL;
}

/** Note what is freed; nothing was allocated. */
void kfree(void *ptr)
{
    freed = ptr;
    frees++;
}

/* The interrupt flag of the model's one processor. */
static uint32_t irq_on = 1;

/** Read the interrupt flag and turn interrupts off. */
uint32_t kirq_save(void)
{
    uint32_t flags = irq_on;

    irq_on = 0;
    return flags;
}

/** Put the interrupt flag back. */
void kirq_restore(uint32_t flags)
{
    irq_on = flags;
}

/** Descriptor 3 is the eventfd's, 4 another object's, and the rest are
 * not open. */
fd_entry_t *fd_get(fd_table_t *table, int fd)
{
    (void)table;
    return fd == 3 ? &entry : fd == 4 ? &other : NULL;
}

/** Making a descriptor is outside this test: -1. */
int fd_alloc(fd_table_t *table)
{
    (void)table;
    return -1;
}

/** memcpy, for eventfd.c. */
void *k_memcpy(void *dst, const void *src, size_t n)
{
    uint8_t *d = dst;
    const uint8_t *s = src;
    size_t i;

    for (i = 0; i < n; i++) {
        d[i] = s[i];
    }
    return dst;
}

/** memset, for the rows. */
void *k_memset(void *dst, int c, size_t n)
{
    uint8_t *d = dst;
    size_t i;

    for (i = 0; i < n; i++) {
        d[i] = (uint8_t)c;
    }
    return dst;
}

/** A nonblocking descriptor over a counter of `value`. */
static void setup(uint64_t value, int semaphore)
{
    k_memset(&counter, 0, sizeof(counter));
    k_memset(&entry, 0, sizeof(entry));
    k_memset(&description, 0, sizeof(description));
    counter.count = value;
    counter.semaphore = semaphore;
    description.status_flags = OFD_NONBLOCK;
    entry.ofd = &description;
    entry.priv = &counter;
    entry.ops = &eventfd_ops;
    entry.flags = FD_FLAG_READ | FD_FLAG_WRITE;
    user[0] = 0;
    inner_value = 0;
    inner_result = 0;
    inner_reads = 0;
    wakes = 0;
    wake_kinds = 0;
    counter.refs = 1;
    freed = NULL;
    frees = 0;
}

/* A hook of the rows: what it was told, and whether it takes the counter. */
struct row_hook {
    struct eventfd_hook hook;
    int signals, hangups, take;
    int signal_in_hangup; /* signal the counter from the hangup */
};

/** Count a signal, and take the counter when the row asks; interrupts
 * must be off. */
static int row_signal(struct eventfd_hook *h)
{
    struct row_hook *r = (struct row_hook *)h;

    CHECK(!irq_on, "a hook is told with interrupts off");
    r->signals++;
    return r->take;
}

/** Count a hangup, and signal the counter from it when the row asks. */
static void row_hangup(struct eventfd_hook *h)
{
    struct row_hook *r = (struct row_hook *)h;

    r->hangups++;
    if (r->signal_in_hangup) {
        eventfd_signal(&counter);
    }
}

/** A hook of the rows, told nothing yet. */
static void hook_init(struct row_hook *r, int take)
{
    k_memset(r, 0, sizeof(*r));
    r->hook.signal = row_signal;
    r->hook.hangup = row_hangup;
    r->take = take;
}

/** The rows of the kernel's side. */
static void kernel_rows(void)
{
    struct row_hook a, b;
    struct eventfd *ev;
    int32_t err = 0;
    uint64_t cnt = 7;

    setup(0, 0);
    ev = eventfd_get(3, &err);
    CHECK(ev == &counter && counter.refs == 2, "a reference from an eventfd's descriptor");
    eventfd_put(ev);
    CHECK(counter.refs == 1 && frees == 0, "a put that is not the last frees nothing");
    CHECK(eventfd_get(4, &err) == NULL && err == -EINVAL, "another object's descriptor is refused");
    CHECK(eventfd_get(9, &err) == NULL && err == -EBADF, "no descriptor is refused");

    setup(0, 0);
    hook_init(&a, 0);
    hook_init(&b, 1);
    CHECK(eventfd_hook_add(&counter, &a.hook) == 0, "a hook added to a counter at zero");
    eventfd_signal(&counter);
    CHECK(counter.count == 1 && a.signals == 1, "a kernel signal adds one and tells the hook");
    CHECK(wake_kinds == WQ_DATA, "a kernel signal wakes the readers");
    CHECK(eventfd_hook_add(&counter, &b.hook) == 1, "a hook added to a raised counter is told so");
    wake_kinds = 0;
    eventfd_signal(&counter);
    CHECK(a.signals == 2 && b.signals == 1, "each hook is told");
    CHECK(counter.count == 0, "a hook that takes the counter takes it, as irqfd does");
    CHECK(wake_kinds == (WQ_DATA | WQ_ROOM), "a counter taken by a hook wakes the writers too");
    user[0] = 5;
    CHECK(eventfd_write(&entry, (const char *)user, 8) == 8, "a program's write");
    CHECK(a.signals == 3 && b.signals == 2 && counter.count == 0,
          "a program's write tells the hooks as a signal does");
    eventfd_hook_remove(&counter, &b.hook, NULL);
    user[0] = 2;
    CHECK(eventfd_write(&entry, (const char *)user, 8) == 8 && b.signals == 2 && a.signals == 4 &&
              counter.count == 2,
          "a hook taken off is told no more");
    eventfd_hook_remove(&counter, &a.hook, &cnt);
    CHECK(cnt == 2 && counter.count == 0, "taking a hook off can take the counter");
    eventfd_hook_remove(&counter, &a.hook, NULL);
    CHECK(counter.hooks == NULL, "taking off a hook not on the list changes nothing");

    setup(0, 0);
    hook_init(&a, 0);
    a.signal_in_hangup = 1;
    eventfd_hook_add(&counter, &a.hook);
    ev = eventfd_get(3, &err);
    eventfd_close(&entry);
    CHECK(a.hangups == 1 && counter.hooks == NULL, "the last close detaches the hook and tells it");
    CHECK(a.signals == 0 && counter.count == 1,
          "a signal from the hangup, racing the close, reaches no detached hook");
    CHECK(frees == 0 && counter.refs == 1, "a kernel holder keeps the counter after the close");
    CHECK(eventfd_hook_add(&counter, &b.hook) == -1 && b.hangups == 1,
          "a hook added after the close is hung up at once");
    eventfd_signal(&counter);
    CHECK(counter.count == 2, "the holder can still signal the closed counter");
    eventfd_put(ev);
    CHECK(frees == 1 && freed == &counter, "the holder's put frees the closed counter");

    setup(0, 0);
    hook_init(&a, 0);
    eventfd_hook_add(&counter, &a.hook);
    ev = eventfd_get(3, &err);
    close_in_copy = 1;
    user[0] = 4;
    CHECK(eventfd_write(&entry, (const char *)user, 8) == 8 && counter.count == 4,
          "a write whose copy runs the close still lands");
    CHECK(a.hangups == 1 && a.signals == 0, "the write after the close tells no detached hook");
    eventfd_put(ev);
    CHECK(frees == 1, "the counter is freed once");
}

/** Run the rows; 0 when every check passed, 1 otherwise. */
int main(void)
{
    setup(1, 0);
    nested = 1;
    CHECK(eventfd_read(&entry, (char *)user, 8) == 8 && user[0] == 1,
          "the first read returns the whole counter");
    CHECK(inner_reads == 1 && inner_result == -EAGAIN,
          "a read during the first read's copy finds the counter taken");
    CHECK(counter.count == 0, "the counter ends at zero");
    CHECK(wakes >= 1, "the read wakes the writers");
    CHECK(wake_kinds == WQ_ROOM, "a read wakes the queue for room alone");

    setup(0, 0);
    user[0] = 3;
    CHECK(eventfd_write(&entry, (const char *)user, 8) == 8 && counter.count == 3,
          "a write adds to the counter");
    CHECK(wakes == 1 && wake_kinds == WQ_DATA, "a write wakes the queue for data alone");

    setup(2, 1);
    nested = 1;
    CHECK(eventfd_read(&entry, (char *)user, 8) == 8 && user[0] == 1,
          "a semaphore read returns one");
    CHECK(inner_result == 8 && inner_value == 1, "the read during its copy takes the other one");
    CHECK(counter.count == 0, "the semaphore counter ends at zero");

    setup(5, 0);
    CHECK(eventfd_read(&entry, NULL, 8) == -EFAULT, "a read into memory it cannot write");
    CHECK(counter.count == 0, "a failed copy loses the value it took");

    setup(0, 0);
    CHECK(eventfd_read(&entry, (char *)user, 8) == -EAGAIN, "a nonblocking read of zero");
    CHECK(eventfd_read(&entry, (char *)user, 7) == -EINVAL, "a read shorter than eight bytes");

    kernel_rows();

    printf("eventfd-hosttest: result: %s: %d failures\n", failures ? "fail" : "pass", failures);
    return failures ? 1 : 0;
}
