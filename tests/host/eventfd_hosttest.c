/**
 * Check that an eventfd read takes its value before the copy to the caller.
 *
 * host-deps:
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

/** Copy `len` bytes from the caller; 0. */
int copy_from_user(void *dst, const void __user *src, size_t len)
{
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

/** Nothing is allocated to free. */
void kfree(void *ptr)
{
    (void)ptr;
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
    entry.flags = FD_FLAG_READ | FD_FLAG_WRITE;
    user[0] = 0;
    inner_value = 0;
    inner_result = 0;
    inner_reads = 0;
    wakes = 0;
    wake_kinds = 0;
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

    printf("eventfd-hosttest: result: %s: %d failures\n", failures ? "fail" : "pass", failures);
    return failures ? 1 : 0;
}
