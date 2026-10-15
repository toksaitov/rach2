/*
 * lag_hosttest.c: the queue of the lag wrapper, rach2/sys/block/lag.c, under
 * a transfer that sleeps.
 *
 * host-deps: rach2/sys/lib/kstring.c rach2/sys/lib/kctypes.c
 *
 * The wrapper runs each transfer in the caller's context and answers it
 * from the timer a few ticks later. The driver under it can put the caller
 * to sleep, and a second caller then enters the wrapper while the first
 * transfer is unfinished; the timer can run meanwhile as well. The backing
 * device here enters the wrapper again, or runs the timer, from inside its
 * own write, which is what a sleeping transfer looks like to the queue.
 */

extern int printf(const char *fmt, ...);

#include "../../rach2/sys/block/block.h"
#include "../../rach2/sys/lib/kerrno.h"
#include "../../rach2/sys/lib/kstring.h"

/** Whether two strings are the same; the kernel's string library has no
 *  compare. */
static int same(const char *a, const char *b)
{
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

/**
 * The boot options the wrapper reads at install: `rach2.blocklag` is "3",
 * copied into `out`, and every other option is absent. Returns 1 when the
 * option is present and 0 otherwise.
 */
int kcmdline_value(const char *key, char *out, unsigned int outlen)
{
    if (same(key, "rach2.blocklag") && outlen > 1) {
        out[0] = '3';
        out[1] = '\0';
        return 1;
    }
    return 0;
}

/** The kernel log, which the test discards. */
void klog(int level, const char *subsystem, const char *format, ...)
{
    (void)level;
    (void)subsystem;
    (void)format;
}

/* The interrupt brackets open now. The claim runs with interrupts off in the
   kernel; here nothing interrupts and the brackets only count. */
static int brackets;

/** Open an interrupt bracket. Returns flags of 0. */
uint32_t kirq_save(void)
{
    brackets++;
    return 0;
}

/** Close the interrupt bracket `kirq_save` opened. */
void kirq_restore(uint32_t flags)
{
    (void)flags;
    brackets--;
}

/* What the wrapper delivers: for each request, how many completions came
   with its generation and a clean status, and how many carried the
   duplicate's error; and how many named no request of `reqs` or a stale
   generation. */
#define REQUESTS 4
static struct block_request reqs[REQUESTS];
static int answered[REQUESTS];
static int duplicated[REQUESTS];
static int wrong_generation;

/** Count a completion the wrapper delivers against its request in
 *  `answered` or `duplicated`, or in `wrong_generation`. */
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

/* The backing device's hooks, each a request index or -1 for none: the
   request `backing_write` submits from inside its transfer, the way a caller
   that slept in the driver is overtaken by another, and the request under
   whose transfer it runs the timer. */
static int reenter_with = -1;
static int tick_inside = -1;
static int backing_writes;

static void ticks(int n);

/**
 * The backing device's write, which counts itself. When `tick_inside` names
 * a request, it runs the timer past the lag and checks that the request,
 * whose slot is claimed and whose transfer is this one, was not delivered;
 * when `reenter_with` names one, it submits that request to the wrapper.
 * Each is done once and cleared. Returns 0.
 */
static int backing_write(block_device_t *device, uint64_t lba, uint32_t count, const void *buffer)
{
    (void)device;
    (void)lba;
    (void)count;
    (void)buffer;
    backing_writes++;
    if (tick_inside >= 0) {
        int mine = tick_inside;

        tick_inside = -1;
        ticks((int)lag_delay + 3);
        CHECK(answered[mine] == 0 && duplicated[mine] == 0,
              "a claimed slot whose transfer has not returned is not delivered");
    }
    if (reenter_with >= 0) {
        int inner = reenter_with;

        reenter_with = -1;
        CHECK(lag_submit(&lag_device, &reqs[inner]) == 0, "the inner submit is taken");
    }
    return 0;
}

/** The backing device's read: one sector of 0x5A. Returns 0. */
static int backing_read(block_device_t *device, uint64_t lba, uint32_t count, void *buffer)
{
    (void)device;
    (void)lba;
    (void)count;
    k_memset(buffer, 0x5A, BLOCK_SECTOR_SIZE);
    return 0;
}

static block_device_t backing;

/* A second disk, which the wrapper installed over `backing` is not over. */
static block_device_t other;

static uint32_t generations = 100;

/**
 * Make request `i` afresh: one sector at LBA `i` through page `i`, a write
 * when `write` is nonzero and a read otherwise, in flight, with its counts
 * at zero and a generation of its own, so an answer meant for the request
 * that used the slot before it is a stale one.
 */
static void request_init(int i, int write)
{
    k_memset(&reqs[i], 0, sizeof(reqs[i]));
    reqs[i].write = write;
    reqs[i].lba = (uint64_t)i;
    reqs[i].count = 1;
    reqs[i].page = pages[i];
    reqs[i].generation = ++generations;
    reqs[i].state = BLOCK_REQ_IN_FLIGHT;
    answered[i] = 0;
    duplicated[i] = 0;
}

/** Run the wrapper's timer `n` times. */
static void ticks(int n)
{
    while (n-- > 0) {
        block_lag_tick();
    }
}

/**
 * Install the wrapper over the backing device with a lag of three ticks and
 * run the rows. The wrapper delivers each completion after the lag, and a
 * duplicate with an error two ticks later. The rows require that:
 *
 * - a later wrap of another device returns that device unchanged, and a
 *   later wrap of the backing returns the wrapper again;
 * - a tick that runs under a transfer does not deliver that transfer's
 *   claimed slot, which is answered after its own lag instead;
 * - two writes, the second submitted from inside the first's transfer, are
 *   each answered once after the lag and once more with the duplicate's
 *   error, and nothing is delivered a third time;
 * - a read's page holds the 0xEE scrub until its answer and the backing's
 *   bytes then;
 * - a flush takes a slot and is answered the same way;
 * - every slot is free after its duplicate, and every interrupt bracket is
 *   closed.
 *
 * Returns the count of failed checks, at most 255.
 */
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
    k_memset(&other, 0, sizeof(other));
    other.read = backing_read;
    other.write = backing_write;
    CHECK(block_lag_wrap(&other) == &other, "another device is returned unchanged");
    CHECK(lag_backing == &backing, "and the wrapper stays over the backing");
    CHECK(block_lag_wrap(&backing) == &lag_device, "the backing gets its wrapper again");

    for (i = 0; i < REQUESTS; i++) {
        request_init(i, 1);
    }
    tick_inside = 0;
    CHECK(lag_submit(lag, &reqs[0]) == 0, "a submit under which the timer ran is taken");
    ticks(3);
    CHECK(answered[0] == 1, "and is answered after its own lag");
    ticks(2);
    CHECK(duplicated[0] == 1, "and its duplicate carries the error");
    request_init(0, 1);
    backing_writes = 0;
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

    for (i = 0; i < LAG_QUEUE; i++) {
        CHECK(!queue[i].used, "a slot is free after its duplicate");
    }
    CHECK(brackets == 0, "every interrupt bracket was closed");
    if (fails == 0) {
        printf("lag_hosttest: OK\n");
    }
    return fails > 255 ? 255 : fails;
}
