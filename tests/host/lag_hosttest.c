/*
 * lag_hosttest.c: the lag wrapper's queue under a transfer that sleeps.
 *
 * host-deps: rach2/sys/lib/kstring.c rach2/sys/lib/kctypes.c
 *
 * The wrapper runs each transfer in the caller's context and answers it
 * from the timer a few ticks later. The driver under it can put the caller
 * to sleep, and a second caller then enters the wrapper while the first
 * transfer is unfinished. The queue's slot must be claimed before the
 * transfer, or the second caller finds the same slot free, takes it, and
 * one of the two requests is never answered: the request layer quarantines
 * it five seconds later and the device is declared stalled, which is how
 * the fatwrite suite's writers row failed once the mount lock's release
 * yielded. The fake backing device here enters the wrapper again from
 * inside its own write, which is what a switch during the transfer looks
 * like to the queue, and the rows require every request answered once with
 * its status and once more with the duplicate's error. Returns the failed
 * check count.
 */

extern int printf(const char *fmt, ...);

#include "../../rach2/sys/block/block.h"
#include "../../rach2/sys/lib/kerrno.h"
#include "../../rach2/sys/lib/kstring.h"

/* Whether two strings are the same; the kernel's string library has no
   compare. */
static int same(const char *a, const char *b)
{
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

/* The knobs the wrapper reads at install: a three-tick lag and nothing
   else. */
int kcmdline_value(const char *key, char *out, unsigned int outlen)
{
    if (same(key, "rach2.blocklag") && outlen > 1) {
        out[0] = '3';
        out[1] = '\0';
        return 1;
    }
    return 0;
}

void klog(int level, const char *subsystem, const char *format, ...)
{
    (void)level;
    (void)subsystem;
    (void)format;
}

/* The claim runs with interrupts off in the kernel; here nothing interrupts
   and the brackets only count. */
static int brackets;

uint32_t kirq_save(void)
{
    brackets++;
    return 0;
}

void kirq_restore(uint32_t flags)
{
    (void)flags;
    brackets--;
}

/* What the wrapper delivers: for each request, how many completions came
   with its generation and a clean status, and how many carried the
   duplicate's error. */
#define REQUESTS 4
static struct block_request reqs[REQUESTS];
static int answered[REQUESTS];
static int duplicated[REQUESTS];
static int wrong_generation;

void block_request_complete(struct block_request *request, uint32_t generation, int status)
{
    int i = (int)(request - reqs);

    if (i < 0 || i >= REQUESTS || generation != request->generation) {
        wrong_generation++;
        return;
    }
    if (status == 0) {
        answered[i]++;
    } else {
        duplicated[i]++;
    }
}

#include "block/lag.c"

static int fails;

#define CHECK(cond, what)                             \
    do {                                              \
        if (!(cond)) {                                \
            fails++;                                  \
            printf("lag_hosttest: FAIL: %s\n", what); \
        }                                             \
    } while (0)

static uint8_t pages[REQUESTS][BLOCK_SECTOR_SIZE];

/* The backing device. Its first write enters the wrapper again with the
   next request, the way a caller that slept in the driver is overtaken by
   another; every later transfer is plain. */
static int reenter_with = -1;
static int backing_writes;

static int backing_write(block_device_t *device, uint64_t lba, uint32_t count, const void *buffer)
{
    (void)device;
    (void)lba;
    (void)count;
    (void)buffer;
    backing_writes++;
    if (reenter_with >= 0) {
        int inner = reenter_with;

        reenter_with = -1;
        CHECK(lag_submit(&lag_device, &reqs[inner]) == 0, "the inner submit is taken");
    }
    return 0;
}

static int backing_read(block_device_t *device, uint64_t lba, uint32_t count, void *buffer)
{
    (void)device;
    (void)lba;
    (void)count;
    k_memset(buffer, 0x5A, BLOCK_SECTOR_SIZE);
    return 0;
}

static block_device_t backing;

static void request_init(int i, int write)
{
    k_memset(&reqs[i], 0, sizeof(reqs[i]));
    reqs[i].write = write;
    reqs[i].lba = (uint64_t)i;
    reqs[i].count = 1;
    reqs[i].page = pages[i];
    reqs[i].generation = 100u + (uint32_t)i;
    reqs[i].state = BLOCK_REQ_IN_FLIGHT;
}

static void ticks(int n)
{
    while (n-- > 0) {
        block_lag_tick();
    }
}

int main(void)
{
    block_device_t *lag;
    int i;

    k_memset(&backing, 0, sizeof(backing));
    backing.read = backing_read;
    backing.write = backing_write;
    lag = block_lag_wrap(&backing);
    CHECK(lag == &lag_device, "the wrapper installs over the backing");
    CHECK(lag_delay == 3, "the lag is the three ticks the option named");

    /* Two writers, the second entering while the first transfer runs. */
    for (i = 0; i < REQUESTS; i++) {
        request_init(i, 1);
    }
    reenter_with = 1;
    CHECK(lag_submit(lag, &reqs[0]) == 0, "the outer submit is taken");
    CHECK(reenter_with < 0, "the backing entered the wrapper again from the transfer");
    CHECK(backing_writes == 2, "both transfers ran");
    ticks(2);
    CHECK(answered[0] == 0 && answered[1] == 0, "nothing is answered before its lag");
    ticks(1);
    CHECK(answered[0] == 1, "the outer request is answered after the lag");
    CHECK(answered[1] == 1, "the inner request is answered too: its slot was its own");
    ticks(2);
    CHECK(duplicated[0] == 1 && duplicated[1] == 1, "each duplicate carries the error once");
    ticks(3);
    CHECK(answered[0] == 1 && answered[1] == 1 && duplicated[0] == 1 && duplicated[1] == 1,
          "nothing is delivered twice over");
    CHECK(wrong_generation == 0, "every completion named its request's generation");

    /* A flush claims a slot the same way, beside a read whose page is
       scrubbed until the answer comes. */
    request_init(2, 0);
    request_init(3, 0);
    k_memset(pages[2], 0x11, sizeof(pages[2]));
    CHECK(lag_submit(lag, &reqs[2]) == 0, "a read is taken");
    CHECK(pages[2][0] == 0xEE, "the read's page is scrubbed until the completion");
    CHECK(lag_flush(lag, &reqs[3]) == 0, "a flush is taken");
    ticks(3);
    CHECK(answered[2] == 1, "the read is answered");
    CHECK(pages[2][0] == 0x5A, "the read's bytes land with the completion");
    CHECK(answered[3] == 1, "the flush is answered");
    ticks(2);
    CHECK(duplicated[2] == 1 && duplicated[3] == 1, "their duplicates carry the error");

    /* The queue is empty again: every slot released after its duplicate. */
    for (i = 0; i < LAG_QUEUE; i++) {
        CHECK(!queue[i].used, "a slot is free after its duplicate");
    }
    CHECK(brackets == 0, "every interrupt bracket was closed");
    if (fails == 0) {
        printf("lag_hosttest: OK\n");
    }
    return fails;
}
