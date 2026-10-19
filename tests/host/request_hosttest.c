/* The real block request pool and lag queue under explicit completion control.
 * host-deps: rach2/sys/lib/kstring.c rach2/sys/lib/kctypes.c
 */

#ifndef REQUEST_SOURCE
#define REQUEST_SOURCE "block/request.c"
#endif
#ifndef LAG_SOURCE
#define LAG_SOURCE "block/lag.c"
#endif
#include "block/block.c"
#include REQUEST_SOURCE
#include LAG_SOURCE

extern int printf(const char *format, ...);
extern void abort(void) __attribute__((noreturn));
extern void exit(int status) __attribute__((noreturn));

/** Stop at the first failed contract so a fault cannot corrupt later rows. */
static void check(int ok, const char *what)
{
    if (!ok) {
        printf("request-hosttest: fail: %s\n", what);
        exit(1);
    }
}

/* Reuse freed addresses immediately, and count records and DMA pages apart. */
static struct block_request records[16];
static uint8_t frames[16][PAGE_SIZE];
static int record_used[16];
static int frame_used[16];
static int record_count;
static int frame_count;
static uint32_t irq_depth;
struct percpu cpu_table[NR_CPUS];

#ifndef arch_cpu_id
/** Run as processor zero. */
unsigned int arch_cpu_id(void)
{
    return 0;
}
#endif

/** Whether two option strings are equal. */
static int same(const char *a, const char *b)
{
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

/** Save the model's interrupt nesting and enter a masked region. */
uint32_t kirq_save(void)
{
    return irq_depth++;
}

/** Restore the nesting saved by the matching bracket. */
void kirq_restore(uint32_t flags)
{
    check(irq_depth == flags + 1, "interrupt brackets nest");
    irq_depth = flags;
}

/** Whether the model's interrupts are enabled. */
int kirq_enabled(void)
{
    return irq_depth == 0;
}

/** Allocate a zeroed request record at the first free address. */
void *kzalloc(size_t size)
{
    int i;

    if (size != sizeof(records[0])) {
        abort();
    }
    for (i = 0; i < 16; i++) {
        if (!record_used[i]) {
            record_used[i] = 1;
            record_count++;
            k_memset(&records[i], 0, size);
            return &records[i];
        }
    }
    return 0;
}

/** Release a record, retaining its bytes to expose premature reuse. */
void kfree(void *p)
{
    int i;

    for (i = 0; i < 16; i++) {
        if (p == &records[i] && record_used[i]) {
            record_used[i] = 0;
            record_count--;
            return;
        }
    }
    abort();
}

/** Allocate the first free DMA page. */
void *alloc_kernel_frame(void)
{
    int i;

    for (i = 0; i < 16; i++) {
        if (!frame_used[i]) {
            frame_used[i] = 1;
            frame_count++;
            return frames[i];
        }
    }
    return 0;
}

/** Release a DMA page once. */
void free_kernel_frame(void *p)
{
    int i;

    for (i = 0; i < 16; i++) {
        if (p == frames[i] && frame_used[i]) {
            frame_used[i] = 0;
            frame_count--;
            return;
        }
    }
    abort();
}

/** These rows use only one-page requests. */
void *kalloc_pages_try(uint32_t count, uintptr_t *phys)
{
    (void)count;
    (void)phys;
    abort();
}

/** No multi-page allocation may be freed. */
void kfree_pages(void *p, uint32_t count)
{
    (void)p;
    (void)count;
    abort();
}

/** The model clock, advanced only by explicit request ticks. */
uint32_t timer_now_us(void)
{
    return req_ticks * 10000u;
}

/** The latency record has no effect on completion. */
void latency_span(int kind, uint32_t from, uint32_t to)
{
    (void)kind;
    (void)from;
    (void)to;
}

/** Discard diagnostic records. */
void klog(int level, const char *subsystem, const char *format, ...)
{
    (void)level;
    (void)subsystem;
    (void)format;
}

/** An unexpected path is a failed model setup. */
void k_panic(const char *why)
{
    printf("request-hosttest: panic: %s\n", why);
    abort();
}

/** Install a two-slot pool and a lag wrapper without write faults. */
int kcmdline_value(const char *key, char *out, unsigned int size)
{
    const char *value = same(key, "rach2.blocklag") ? "3" : same(key, "rach2.blockpool") ? "2" : 0;

    if (!value || size < 2) {
        return 0;
    }
    out[0] = value[0];
    out[1] = 0;
    return 1;
}

/** No disk probe option is present. */
int kcmdline_has(const char *key)
{
    (void)key;
    return 0;
}

/** The host fixture supplies its devices directly. */
int ahci_init(void)
{
    return -1;
}
/** The host fixture supplies its devices directly. */
int nvme_init(void)
{
    return -1;
}
/** The host fixture supplies its devices directly. */
void platform_disks_probe(void)
{}
/** There is no disk registry in this model. */
int disk_count(void)
{
    return 0;
}
/** No registry entry is requested. */
struct disk *disk_at(int i)
{
    (void)i;
    return 0;
}
/** No partition table is scanned. */
int disk_scan(struct disk *d, block_device_t *dev)
{
    (void)d;
    (void)dev;
    return 0;
}

static int backing_calls;
static uint64_t backing_lba;
static int inside_transfer;

/** Stage bytes without completing the request; try delivery before ready. */
static int read_backing(block_device_t *dev, uint64_t lba, uint32_t count, void *buf)
{
    (void)dev;
    backing_calls++;
    backing_lba = lba;
    if (inside_transfer) {
        check(block_lag_answer(next_generation, 0) == -EAGAIN,
              "an unfinished backing transfer cannot be answered");
        block_lag_tick();
    }
    k_memset(buf, 0x5A, count * BLOCK_SECTOR_SIZE);
    return 0;
}

/** Observe the request's private copy of a caller's write. */
static int write_backing(block_device_t *dev, uint64_t lba, uint32_t count, const void *buf)
{
    (void)dev;
    (void)lba;
    check(count == 1 && ((const uint8_t *)buf)[0] == 0x37,
          "the backing write receives the caller's bytes");
    backing_calls++;
    return 0;
}

/** A separate sleeping device accepts requests without completing them. */
static int accept(block_device_t *dev, struct block_request *r)
{
    (void)dev;
    k_memset(r->page, 0x6B, r->count * BLOCK_SECTOR_SIZE);
    return 0;
}

static int waits;
static uint8_t output[BLOCK_SECTOR_SIZE];

/** Complete a synchronous write or flush only after its caller blocks. */
int wq_wait_event_nointr(wait_queue_t *wq, wq_cond_fn cond, void *arg)
{
    struct block_request *r = arg;

    (void)wq;
    check(!irq_depth && !cond(arg), "the caller waits with interrupts enabled");
    check(r->held_by_caller && r->held_by_device,
          "both owners retain the request while the caller waits");
    check(r->page != output, "the device owns a private page");
    waits++;
    check(block_lag_answer(r->generation, 0) == 0, "the blocked call can be answered");
    check(cond(arg), "the completion satisfies the wait condition");
    check(block_lag_answer(r->generation, 1) == 0, "a duplicate before caller release is dropped");
    check(r->status == 0 && r->held_by_caller && !r->held_by_device,
          "a duplicate preserves the completed result and caller ownership");
    return 0;
}

/** Wakes are notifications; the model calls each wait condition itself. */
void wq_wake_all(wait_queue_t *wq)
{
    (void)wq;
}

/** Require a successful fixture setup before dereferencing a request. */
static struct block_request *begin(block_device_t *dev, uint32_t *gen)
{
    struct block_request *r = block_request_begin(dev, 7, 1, gen);

    if (!r) {
        printf("request-hosttest: fail: request setup\n");
        abort();
    }
    return r;
}

/** Test refused admission, cleaning up an unexpectedly accepted request. */
static int refused(block_device_t *dev)
{
    uint32_t gen;
    struct block_request *r = block_request_begin(dev, 0, 1, &gen);
    int status;

    if (!r) {
        return 1;
    }
    block_lag_answer(gen, 0);
    block_request_ended(r, gen, output, &status);
    block_lag_answer(gen, 1);
    return 0;
}

static block_device_t backing, other;
static block_device_t *lag;
static block_partition_t first, second;

/** Expire a held request, leave its caller, and deliver its answer late. */
static void quarantine(struct block_request *fresh, uint32_t gf)
{
    struct block_request *a, *b;
    uint32_t ga, gb;
    int status, i;

    for (i = 0; i < 5; i++) {
        block_request_tick();
    }
    check(fresh->state == BLOCK_REQ_IN_FLIGHT && fresh->overdue == 1,
          "one expired period keeps a request in flight");
    block_request_tick();
    check(fresh->state == BLOCK_REQ_QUARANTINED && block_quarantines() == 1,
          "the last period quarantines the held request");
    check(block_request_ended(fresh, gf, output, &status) && status == -EIO,
          "the caller can leave a quarantined request");
    check(record_count == 1 && frame_count == 1 && fresh->held_by_device && !fresh->held_by_caller,
          "caller release cannot free a page still owned by the device");
    check(refused(&first.device) && (lag->flags & BLOCK_STALLED),
          "a quarantined device admits no further request");
    b = begin(&other, &gb);
    check(block_lag_answer(gf, 0) == 0, "a late answer releases quarantine");
    check(record_count == 1 && frame_count == 1 && !(lag->flags & BLOCK_STALLED),
          "late completion frees the abandoned page and clears the stall");
    a = begin(&first.device, &ga);
    check(a == fresh && ga != gf, "a freed quarantine slot can be reused");
    check(block_lag_answer(gf, 1) == 0 && a->state == BLOCK_REQ_IN_FLIGHT,
          "a duplicate after quarantine cannot complete the next request");
    check(block_lag_answer(ga, 0) == 0, "the new request completes by its own generation");
    check(block_request_ended(a, ga, output, &status) && status == 0, "the new read succeeds");
    check(block_lag_answer(ga, 1) == 0, "the final duplicate is dropped after release");
    block_request_complete(b, gb, 0);
    check(block_request_ended(b, gb, output, &status) && status == 0,
          "the other device stays usable");
}

/** Complete independent reads and reject stale answers after address reuse. */
static void completion(void)
{
    struct block_request *a, *b, *fresh;
    uint32_t ga, gb, gf;
    int status, i;

    inside_transfer = 1;
    a = begin(&first.device, &ga);
    inside_transfer = 0;
    check(backing_lba == 107 && backing_calls == 1, "a partition forwards its physical LBA once");
    check(a->owner == lag && a->device == &first.device,
          "the partition is accounted to its submitter");
    check(refused(&second.device), "two partitions share their device's pool allowance");
    b = begin(&other, &gb);
    check(block_in_flight() == 2 && record_count == 2 && frame_count == 2,
          "an independent device can use the remaining slot");
    for (i = 0; i < 1000; i++) {
        block_lag_tick();
    }
    check(a->state == BLOCK_REQ_IN_FLIGHT && a->page[0] == 0xEE,
          "timer activity cannot deliver a held read");
    k_memset(output, 0x11, sizeof(output));
    check(!block_request_ended(a, ga, output, &status) && output[0] == 0x11,
          "an unfinished read copies no bytes to its caller");
    block_request_complete(a, ga + 100, -EIO);
    check(a->state == BLOCK_REQ_IN_FLIGHT && a->held_by_device,
          "a wrong generation cannot complete or release a request");
    block_request_complete(b, gb, 0);
    check(a->state == BLOCK_REQ_IN_FLIGHT && block_overlaps() == 1,
          "an independent request completes while the held one waits");
    check(block_request_ended(b, gb, output, &status) && status == 0 && output[0] == 0x6B,
          "the independent read returns its own bytes");
    check(block_lag_answer(ga + 100, 0) == -ENOENT, "an unknown capture changes no request");
    check(block_lag_answer(ga, 1) == -EAGAIN, "a duplicate cannot precede its answer");
    check(block_lag_answer(ga, 0) == 0 && a->state == BLOCK_REQ_DONE,
          "an explicit answer completes the selected read");
    check(a->held_by_caller && !a->held_by_device && frame_count == 1,
          "completion retains the page until its caller consumes it");
    check(block_lag_answer(ga, 0) == -EAGAIN, "an answer is delivered only once");
    check(block_request_ended(a, ga, output, &status) && status == 0 && output[0] == 0x5A,
          "the completed read returns staged bytes");
    check(record_count == 0 && frame_count == 0, "both releases free the request and page");
    fresh = begin(&second.device, &gf);
    check(fresh == a && gf != ga, "a new request reuses the address with a new generation");
    check(block_lag_answer(ga, 1) == 0, "deliver the old duplicate after address reuse");
    check(fresh->state == BLOCK_REQ_IN_FLIGHT && fresh->page[0] == 0xEE && fresh->held_by_device,
          "a stale duplicate cannot change the reused request or its page");
    check(block_lag_answer(ga, 1) == -ENOENT, "a duplicate is delivered only once");
    quarantine(fresh, gf);
}

/** A synchronous write and flush retain their request until completion. */
static void write_and_flush(void)
{
    process_t process;

    k_memset(&process, 0, sizeof(process));
    process.in_syscall = 1;
    cpu_table[0].current = &process;
    k_memset(output, 0x37, sizeof(output));
    check(block_request(&first.device, 1, 0, 1, output) == 0,
          "a synchronous write waits for completion");
    check(block_flush(&first.device) == 0 && waits == 2,
          "a flush uses the same wait and ownership path");
    cpu_table[0].current = 0;
}

/** Install the fixture, run the lifecycle rows and check every release. */
int main(void)
{
    int i;

    k_memset(&backing, 0, sizeof(backing));
    backing.read = read_backing;
    backing.write = write_backing;
    backing.sector_count = 1000;
    backing.sector_size = BLOCK_SECTOR_SIZE;
    check(block_lag_hold(1) == -ENODEV, "holding before installation fails");
    lag = block_lag_wrap(&backing);
    check(block_lag_answer(1, 0) == -EPERM, "explicit answers require held mode");
    check(block_lag_hold(1) == 0, "hold timer delivery");
    lag->deadline_ticks = 3;
    lag->deadline_limit = 2;
    block_partition_init(&first, lag, 100, 100);
    block_partition_init(&second, lag, 200, 100);
    other = *lag;
    other.submit = accept;
    other.flags = 0;
    completion();
    write_and_flush();
    check(record_count == 0 && frame_count == 0 && live == 0 && quarantined_count == 0,
          "every request and page is released at the end");
    for (i = 0; i < LAG_QUEUE; i++) {
        check(!queue[i].used, "every captured answer and duplicate is consumed");
    }
    check(irq_depth == 0, "all interrupt brackets are closed");
    printf("request-hosttest: result: pass\n");
    return 0;
}
