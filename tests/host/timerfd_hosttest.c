/**
 * Check a timerfd's read against the timer tick, and its settings.
 *
 * host-deps:
 * host-cflags: -Wno-pointer-to-int-cast -Wno-int-to-pointer-cast
 *
 * The timerfd code is the kernel's, included whole to reach its static read
 * and arm. The test owns the tick count and calls `timerfd_tick` as the
 * timer interrupt does; the interrupt mask is a counter.
 *
 * A read takes the count before its copy to the caller: an expiration that
 * comes while the copy sleeps is kept for the next read, a second read in
 * that time finds nothing, and a copy that fails loses what it took, as in
 * Linux.
 *
 * A timer of 200 days, longer than one wait of ticks can span, reports 200
 * days, does not expire a tick early and expires at its tick; an interval of
 * 200 days is reported and waited whole; and a time past the most seconds a
 * setting keeps is taken as that most.
 */
#include "io/timerfd.c"

extern int printf(const char *format, ...);

volatile uint32_t kernel_ticks;
static struct ktimer timer;
static fd_entry_t entry;
static struct open_file description;
static process_t reader;
process_t *current_proc = &reader;
static uint64_t user[2];
static int failures, masked, wakes;
static int tick_in_copy, read_in_copy;
static int32_t inner_result;
static uint64_t inner_value;

#define CHECK(condition, detail)                              \
    do {                                                      \
        if (!(condition)) {                                   \
            printf("timerfd-hosttest: failed: %s\n", detail); \
            failures++;                                       \
        }                                                     \
    } while (0)

/* The POSIX timers' part, which these rows do not reach. */
process_t *get_process(uint32_t pid)
{
    (void)pid;
    return 0;
}

void signal_post_info(process_t *target, int signo, const struct k_siginfo *info)
{
    (void)target;
    (void)signo;
    (void)info;
    CHECK(0, "a timerfd posted a signal");
}

void signal_post_thread(process_t *t, int signo, const struct k_siginfo *info)
{
    (void)t;
    (void)signo;
    (void)info;
    CHECK(0, "a timerfd posted a signal");
}

uint32_t kirq_save(void)
{
    return (uint32_t)masked++;
}

void kirq_restore(uint32_t flags)
{
    masked = (int)flags;
}

/**
 * The copy to the caller. With `tick_in_copy` the next tick comes first, and
 * with `read_in_copy` a second read, as either can while the copy sleeps. A
 * destination of 0 is a buffer that cannot be written.
 */
int copy_to_user(void __user *dst, const void *src, size_t len)
{
    if (tick_in_copy) {
        tick_in_copy = 0;
        timerfd_tick(++kernel_ticks);
    }
    if (read_in_copy) {
        read_in_copy = 0;
        inner_result = timerfd_read(&entry, (char *)&inner_value, sizeof(inner_value));
    }
    if (!dst) {
        return -EFAULT;
    }
    k_memcpy(dst, src, len);
    return 0;
}

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

void wq_wake_all(wait_queue_t *wq)
{
    (void)wq;
    wakes++;
}

void wait_queue_init(wait_queue_t *wq)
{
    wq->head = NULL;
}

int32_t clock_now(int32_t clk, int64_t *sec, int32_t *nsec)
{
    (void)clk;
    *sec = 1000;
    *nsec = 0;
    return 0;
}

/** No timer here counts processor time, so none holds a process. */
int32_t clock_cpu_target(int32_t clk, struct tgroup **tg, uint32_t *tid, int *user_only)
{
    (void)clk;
    (void)tg;
    (void)tid;
    (void)user_only;
    return -EINVAL;
}

void tgroup_put(struct tgroup *tg)
{
    (void)tg;
}

/** Making a descriptor, and finding one, are outside this test. */
void *kzalloc(size_t n)
{
    (void)n;
    return NULL;
}

void kfree(void *ptr)
{
    (void)ptr;
}

int fd_alloc(fd_table_t *table)
{
    (void)table;
    return -1;
}

fd_entry_t *fd_get(fd_table_t *table, int fd)
{
    (void)table;
    (void)fd;
    return NULL;
}

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

void *k_memset(void *dst, int c, size_t n)
{
    uint8_t *d = dst;
    size_t i;

    for (i = 0; i < n; i++) {
        d[i] = (uint8_t)c;
    }
    return dst;
}

/** A nonblocking descriptor over a timer armed to expire every tick from
 * the next, and the ticks run to its first expiration. */
static void setup(void)
{
    struct setting every = {0, NS_PER_TICK, 0, NS_PER_TICK};

    k_memset(&timer, 0, sizeof(timer));
    k_memset(&entry, 0, sizeof(entry));
    k_memset(&description, 0, sizeof(description));
    armed = NULL;
    kernel_ticks = 1000;
    description.status_flags = OFD_NONBLOCK;
    entry.ofd = &description;
    entry.priv = &timer;
    entry.flags = FD_FLAG_READ;
    user[0] = 0;
    inner_value = 0;
    inner_result = 0;
    tick_in_copy = read_in_copy = 0;
    CHECK(arm(&timer, 0, &every) == 0, "the timer arms");
    kernel_ticks += 2;
    timerfd_tick(kernel_ticks);
    CHECK(timer.count == 1, "the timer expires at its tick");
}

/* The timer disarmed, at tick 1000. */
static void reset(void)
{
    k_memset(&timer, 0, sizeof(timer));
    armed = NULL;
    kernel_ticks = 1000;
}

/** A timer of 200 days, past the most one wait of ticks can span, and one
 * past the most seconds a setting keeps. */
static void long_rows(void)
{
    struct setting days = {0, 0, 17280000, 0};
    struct setting every = {17280000, 0, 1, 0};
    struct setting forever = {0, 0, 0x7FFFFFFFFFFFFFFFll, 0};
    struct setting got;

    reset();
    CHECK(arm(&timer, 0, &days) == 0, "a timer of 200 days arms");
    current_setting(&timer, &got);
    CHECK(got.vsec == 17280000 && got.vnsec == NS_PER_TICK,
          "a timer of 200 days reports 200 days and the tick in progress");
    timerfd_tick(1000u + 1728000000u);
    CHECK(timer.count == 0 && timer.armed, "a timer of 200 days does not expire early");
    timerfd_tick(1000u + 1728000001u);
    CHECK(timer.count == 1 && !timer.armed, "a timer of 200 days expires at its tick");

    reset();
    CHECK(arm(&timer, 0, &every) == 0, "a timer every 200 days arms");
    kernel_ticks += 101;
    timerfd_tick(kernel_ticks);
    CHECK(timer.count == 1, "its first expiration comes after a second");
    current_setting(&timer, &got);
    CHECK(got.vsec == 17280000 && got.vnsec == 0 && got.isec == 17280000 && got.insec == 0,
          "its next expiration is 200 days away, and its interval 200 days");

    reset();
    CHECK(arm(&timer, 0, &forever) == 0, "a timer past the most seconds a setting keeps arms");
    current_setting(&timer, &got);
    CHECK(got.vsec == (int64_t)TIME_MAX, "it is taken as the most seconds a setting keeps");
}

int main(void)
{
    setup();
    tick_in_copy = 1;
    CHECK(timerfd_read(&entry, (char *)user, 8) == 8 && user[0] == 1,
          "the read returns the expiration before its copy");
    CHECK(timer.count == 1, "an expiration during the copy is kept for the next read");
    CHECK(timerfd_read(&entry, (char *)user, 8) == 8 && user[0] == 1,
          "the next read returns the expiration the copy let in");

    setup();
    read_in_copy = 1;
    CHECK(timerfd_read(&entry, (char *)user, 8) == 8 && user[0] == 1, "the first read");
    CHECK(inner_result == -EAGAIN, "a read during the first read's copy finds none");

    setup();
    CHECK(timerfd_read(&entry, NULL, 8) == -EFAULT, "a read into memory it cannot write");
    CHECK(timer.count == 0, "a failed copy loses the count it took");
    CHECK(timerfd_read(&entry, (char *)user, 7) == -EINVAL, "a read shorter than eight bytes");
    long_rows();
    CHECK(masked == 0, "every interrupt mask is taken back");

    printf("timerfd-hosttest: result: %s: %d failures\n", failures ? "fail" : "pass", failures);
    return failures ? 1 : 0;
}
