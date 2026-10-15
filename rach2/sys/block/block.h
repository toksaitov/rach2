/*
 * block.h: the block device every disk, partition, stick and loop device is,
 * the block request that lets a device sleep its caller, and the counters
 * /proc/disk prints.
 */

#ifndef _BLOCK_H_
#define _BLOCK_H_

#include "../lib/kint.h"

/** The sector size every block device in Rach2 uses. */
#define BLOCK_SECTOR_SIZE 512

typedef struct block_device block_device_t;

struct block_request;

/* The device completes a transfer by polling and may be used with interrupts
 * off. ATA always has it, and AHCI and NVMe until `ahci_start` and
 * `nvme_start` move them to their interrupts; a device that sleeps its caller
 * does not, and the page-cache window refuses such a device as a backing
 * store. */
#define BLOCK_FAULT_SAFE    0x1u
/* Set by the buffer cache when a write to the device failed; cleared when
 * the cache drops the device at unmount. */
#define BLOCK_WRITE_FAILED  0x2u
/* The medium refuses writes and says so: a stick with its switch across, or
 * a partition view over one. Every mount of it is read-only. */
#define BLOCK_READ_ONLY     0x4u
/* The device sleeps its caller and answers in the same call: a loop device
 * over a file, whose read is a read of that file through the file system it
 * is on. It may sleep, so it cannot be called with interrupts off, and it
 * takes no request, because the device under it takes one from the same pool
 * while it waits. It is called directly, in syscall context only. */
#define BLOCK_CALLER_SYNC   0x10u
/* The device has gone: a stick pulled out. Nothing can reach it again, so an
 * unmount writes nothing and only lets go of what it holds. Set by the cache
 * when the disk layer tells it, cleared when the device is published again. */
#define BLOCK_GONE          0x8u
/* The device answers a polled `read` or `write` from any context, its queue
 * drained first: an AHCI or NVMe disk on its interrupts. A caller that cannot
 * sleep, a page fault with interrupts off, takes that path instead of a
 * request. */
#define BLOCK_POLLABLE      0x40u
/* The device stopped answering: every slot of the request pool it held went
 * through its deadline periods unanswered. Its waiters were released with
 * `EIO` and a new request is refused with `EIO`, until a late answer clears
 * it. Set and cleared by the request pool. */
#define BLOCK_STALLED       0x20u
/* The disk makes a write durable before it answers it, with a flush of its
 * own cache after the transfer, unless the request asks it not to
 * (`BLOCK_REQ_NOFLUSH`). Its `unflushed` says whether it has answered such a
 * write since its last flush; `block_flush` sends a flush only then. */
#define BLOCK_WRITE_THROUGH 0x80u

/**
 * A sector-addressed storage device behind a small table of functions.
 *
 * `read` and `write` transfer `count` whole sectors starting at logical block
 * address `lba`, to or from `buffer`, in the call, and return 0 on success or
 * a negative errno (`-EIO`) on failure. On a `BLOCK_FAULT_SAFE` or
 * `BLOCK_POLLABLE` device they poll, and a caller may invoke them with
 * interrupts off and without the scheduler running. `drvdata` is the
 * driver's private pointer.
 */
struct block_device {
    uint32_t sector_size; /* always BLOCK_SECTOR_SIZE */
    uint64_t sector_count;
    uint32_t flags;          /* BLOCK_* above */
    uint32_t deadline_ticks; /* sleeping path: the deadline's period in ticks;
                              * 0 means the thirty-second default */
    uint32_t deadline_limit; /* periods in a row a request is waited for
                              * before it is given up; 0 means the default of
                              * four */
    uint32_t max_sectors;    /* the most one request may move; 0 means a page */
    int (*read)(block_device_t *device, uint64_t lba, uint32_t count, void *buffer);
    int (*write)(block_device_t *device, uint64_t lba, uint32_t count, const void *buffer);
    /* Start one request and return; the device reports the end through
     * `block_request_complete`, from whatever context it completes in. Only a
     * device without `BLOCK_FAULT_SAFE` has one. */
    int (*submit)(block_device_t *device, struct block_request *request);
    /* Put what the device holds in a cache of its own on the medium, and
     * report the end the same way a transfer does. A device with nothing of
     * its own to flush has none of this, and the caller takes that for done.
     * A stick has one, because its cache is the reason a volume can be whole
     * on the wire and not on the medium, and so has a disk that answers
     * writes before they are out of its cache (`BLOCK_WRITE_THROUGH`). With
     * `request` NULL the flush is done in the call: by the device that
     * sleeps its caller, and by polling on a `BLOCK_POLLABLE` disk, for a
     * caller that cannot sleep. */
    int (*flush)(block_device_t *device, struct block_request *request);
    /* The device this one's bytes are on, or NULL for a device that is its
     * own medium. A loop device's is the device of the mount its file is on,
     * and the buffer cache reads the chain to decide whether a caller holding
     * one device's buffer may take another's. */
    block_device_t *backing;
    void *drvdata;
    /* A `BLOCK_WRITE_THROUGH` disk answered a write without its flush, and
     * no flush has reached the disk since. Set and cleared by its driver. */
    volatile int unflushed;
    /* What /proc/diskstats reports of the device: the transfers that ended
     * well, their sectors and their milliseconds, each way, and the requests
     * in progress. Counted by `block_request_flags`, on the device asked
     * and on the disk `block_physical` reaches under it. */
    struct block_stats {
        uint32_t reads, read_sectors, read_ms;
        uint32_t writes, write_sectors, write_ms;
        uint32_t in_flight, busy_ms;
    } stats;
};

/* How long a chain of backing devices may be: a loop device stands on a real
 * device and nothing stands on it. One is what the four buffers a process may
 * hold can serve: a file system on a loop device holds a buffer of it while
 * the file system under it holds two of the disk, and a second loop device in
 * the middle would need two more than there are. A device whose chain would
 * be longer is refused where it is attached. */
#define BLOCK_MAX_DEPTH 1

/**
 * The flags that decide how `device` is driven: a partition view's are its
 * backing's, read when asked and not when the view was cut, since a disk
 * may lose `BLOCK_FAULT_SAFE` when it goes over to its interrupts.
 */
uint32_t block_flags(block_device_t *device);

/**
 * How many devices are under `device`, following its backing chain.
 *
 * Zero for a device that is its own medium. A chain longer than
 * `BLOCK_MAX_DEPTH` answers `BLOCK_MAX_DEPTH + 1`, which is what a caller
 * refuses on.
 */
int block_depth(block_device_t *device);

/**
 * Whether `ancestor` is under `device`: its backing, or its backing's, as
 * far as `BLOCK_MAX_DEPTH + 1` levels down. A device is not under itself.
 */
int block_is_below(block_device_t *ancestor, block_device_t *device);

/**
 * The whole-disk boot device, or `NULL` until `block_probe` finds a disk.
 */
extern block_device_t *block0;

/**
 * Bring up the disks and publish the first as `block0`.
 *
 * Runs the AHCI, NVMe and platform drivers (on a PC, ATA) in that order;
 * each registers the disks it finds, the first disk found is `block0`, and
 * every disk's partition table is then read. Under `rach2.nodisk` it probes
 * nothing. Returns 0 when a device answered and `block0` is set, or -1 when
 * none did. Polled and without interrupts, so it may run before the
 * scheduler does.
 */
int block_probe(void);

/**
 * Read every registered disk's partition table and publish its partition
 * views. `block_probe` calls it after the drivers have registered their
 * disks, and the debug shell's re-probe calls it again.
 */
void block_scan_disks(void);

/**
 * Report a boot disk, probing only if none has been found.
 *
 * `block_probe` resets and identifies a controller each time it is called,
 * and several callers need a disk: root selection, boot-device discovery and
 * the `/proc/disk` probe. This returns at once when `block0` is set, so a
 * working controller is reset only once.
 *
 * A failure is not kept: a controller that did not answer at boot can be
 * probed again from the debug shell.
 *
 * @return 0 when a device answered and `block0` is set, -1 when none did.
 */
int block_ready(void);

/**
 * How many commands the drivers have issued to their devices since boot, and
 * how many of those were cache flushes, for /proc/disk.
 *
 * The raw windows count their own calls into the block interface, which shows
 * how devfs batched but not what the driver did under it: a driver that broke
 * one 64-sector request into 64 commands would give the same answer from
 * above, with a flush after each. These are counted where each command is
 * issued to the device.
 */
uint64_t block_commands(void);
uint64_t block_flushes(void);

/** Record one command, and separately one cache flush, as it is issued. */
void block_count_command(void);
void block_count_flush(void);

/**
 * The drivers' waits since boot: a poll is one read of a status register
 * while a command is in flight, a wait tick one tick a caller slept for one.
 * A driver that spins reads the status thousands of times a command; one
 * that sleeps reads it a few times and is woken. `/proc/disk` prints both, so
 * a test can require the sleep and not the seconds.
 */
uint64_t block_polls(void);
uint64_t block_wait_ticks(void);
void block_count_poll(void);
void block_count_wait_tick(void);

/**
 * A partition: the sector range `[start_lba, start_lba + sector_count)` of a
 * backing device, presented as its own `block_device`.
 *
 * The view's `read`, `write` and `submit` add `start_lba` to each request
 * and pass it to the backing device, refusing with `-EIO` an empty request
 * or one that runs past the partition; its `flush` is the backing's. The
 * `device` member is first, so a `block_device_t *` given to one of the
 * view's functions is the enclosing `block_partition_t`.
 */
typedef struct block_partition {
    block_device_t device;
    block_device_t *backing;
    uint64_t start_lba;
} block_partition_t;

/**
 * Initialize `partition` as the `sector_count`-sector slice of `backing` that
 * starts at `start_lba`, clamped to the sectors `backing` has: a start past
 * its end gives an empty view whose every request fails. After this,
 * `&partition->device` is a usable device. The view copies the backing's
 * flags and deadlines, but `block_flags` and `block_max_sectors` read the
 * backing's at each call, and `submit` is always present, since a disk gains
 * `submit` and loses `BLOCK_FAULT_SAFE` after its views exist.
 */
void block_partition_init(block_partition_t *partition, block_device_t *backing, uint64_t start_lba,
                          uint64_t sector_count);

/* How a request stands. A request the caller gave up on is quarantined with
 * its page until the device answers, late, so the answer has nowhere to land
 * but a page nothing else owns. */
#define BLOCK_REQ_IN_FLIGHT   1
#define BLOCK_REQ_DONE        2
#define BLOCK_REQ_QUARANTINED 3

/**
 * One request on a sleeping device, in pages of its own.
 *
 * The request is allocated for the transfer with a buffer of its own: a frame
 * of the DMA pool for a transfer of a page or less, or `pages` heap pages for
 * a larger one, whose physical addresses the device is given. A write is
 * copied into the buffer before `submit` and a read is copied out after a
 * successful completion, so a completion that arrives after the deadline
 * lands in the request's buffer and not in memory the caller reused. The
 * request is held by its caller and by its device and freed when both have
 * let go; one given up on is quarantined until the late completion, which
 * lets go for the device. Each request takes the next value of a counter as
 * its generation, so no two live requests share one, and a completion naming
 * a request that is no longer live, or a generation that is not its, is
 * dropped and logged.
 */
struct block_request {
    struct block_request *next;   /* the list of live requests */
    struct block_request *queued; /* the device's own queue, oldest first */
    block_device_t *device;
    block_device_t *owner; /* the device the request is accounted to */
    int write;
    uint64_t lba;
    uint32_t count;      /* sectors, at most the device's `max_sectors` */
    uint8_t *page;       /* the request's buffer: a DMA-pool frame, or
                          * `pages` heap pages for a larger transfer */
    uint32_t pages;      /* pages of the buffer */
    int status;          /* 0 or -errno, valid once done */
    volatile int state;  /* BLOCK_REQ_* */
    uint32_t generation; /* unique to this request */
    uint32_t deadline;   /* tick at which the next period ends */
    uint32_t overdue;    /* periods passed unanswered */
    int held_by_caller;  /* until the caller has read the answer or given up */
    int held_by_device;  /* until the device has answered */
    uint32_t taken_us;   /* when it was taken, for the completion's delay */
    uint32_t flags;      /* BLOCK_REQ_NOFLUSH, BLOCK_REQ_FLUSH or 0 */
};

/* A write a `BLOCK_WRITE_THROUGH` disk answers without flushing its cache,
 * for a caller that flushes once after several writes, and a request that
 * is only that flush, with no transfer. A device that is not write-through
 * ignores the first and never sees the second. */
#define BLOCK_REQ_NOFLUSH 0x1u
#define BLOCK_REQ_FLUSH   0x2u

/* A request of a device whose `max_sectors` is 0: one page. */
#define BLOCK_REQ_SECTORS     8u
/* The most any device may take in one request: 1 MiB, 256 pages of a heap
 * buffer described page by page to the controller. Each driver declares
 * its own limit within this one, and `block_max_sectors` caps it here: the
 * AHCI disk's limit is the whole 2048, an NVMe namespace's 8 times 2 to the
 * power of its controller's MDTS, at most 2048, the USB stick's 1024, and
 * the ATA disk's 64, since it moves a sector at a time through a port. */
#define BLOCK_REQ_MAX_SECTORS 2048u

/** The most one request to `device` may move, in sectors: the
 * `max_sectors` of the device under any partition views, capped at
 * `BLOCK_REQ_MAX_SECTORS`, or a page's worth when that is 0. */
uint32_t block_max_sectors(block_device_t *device);

/**
 * Transfer `count` whole sectors, sleeping the caller when the device does.
 *
 * On a `BLOCK_FAULT_SAFE` device this calls the vtable and returns, from any
 * context, at boot included, in calls of at most the device's `max_sectors`.
 * On a device with `submit` it runs from syscall context only, splits the
 * transfer into requests of at most the device's `max_sectors`, halving the
 * size when the heap has no buffer for it down to a page's, waits when as
 * many requests are live as the limit allows or the device holds its share,
 * and sleeps between `submit` and the completion or the end of the device's
 * deadline periods, the first of which starts when the request is taken.
 * Returns 0 or a negative errno; a request unanswered through its periods
 * is `-EIO` with the request quarantined, and so is a stalled device.
 */
int block_request(block_device_t *device, int write, uint64_t lba, uint32_t count, void *buffer);

/**
 * `block_request` with request `flags`: `BLOCK_REQ_NOFLUSH` lets a
 * write-through disk answer a write on its interrupts before the write is
 * out of its cache, and the caller then owes a `block_flush` before anything
 * that must follow the write on the medium. A disk answering by polling, a
 * device that is not write-through and a read ignore it.
 */
int block_request_flags(block_device_t *device, int write, uint64_t lba, uint32_t count,
                        void *buffer, uint32_t flags);

/**
 * Put what the device holds in a cache of its own on the medium.
 *
 * Sleeps the caller like `block_request` does, and from syscall context
 * only. The flush goes to the device a partition view stands on. A device
 * with no `flush` has nothing of its own to flush, and a write-through disk
 * with no write answered unflushed has nothing to put out; either answers 0
 * at once. A stick's flush is `SYNCHRONIZE CACHE` and a disk's `FLUSH CACHE
 * EXT`; the mounts call this after the buffer cache has written back, at an
 * unmount, a sync and an fsync, because a volume whole on the wire is not
 * yet whole on the medium, and the cache calls it at the end of a sync that
 * wrote without flushing.
 */
int block_flush(block_device_t *device);

/**
 * Start a read of at most a page without waiting for it.
 *
 * For a caller that cannot sleep: the disk layer reads a stick's partition
 * table from the pump, where there is no process to put to sleep. The
 * request is allocated, and `generation` is what the caller asks after it
 * with. Returns NULL when the limit is reached now, the device holds its
 * share or is stalled, the heap has no room, or the device refused the
 * request; this caller does not wait.
 */
struct block_request *block_request_begin(block_device_t *device, uint64_t lba, uint32_t count,
                                          uint32_t *generation);

/**
 * Whether a request begun with `block_request_begin` has ended.
 *
 * Returns 0 while it is in flight. Otherwise 1, with `status` the result
 * and, for a read that succeeded, its sectors copied into `buffer`; the
 * caller lets go of the request. A request past its deadline is quarantined
 * by the tick and ends here with `EIO`, and it stays the device's until the
 * late completion.
 */
int block_request_ended(struct block_request *r, uint32_t generation, void *buffer, int *status);

/**
 * A sleeping device reports one request's end.
 *
 * `generation` is the value the device captured at `submit`. A completion
 * for a request no longer live, or whose generation is not the request's, is
 * dropped with a record; one for a quarantined request lets go of it for the
 * device.
 */
void block_request_complete(struct block_request *request, uint32_t generation, int status);

/**
 * How many requests are still the device's: in flight, or quarantined with
 * a page the device may yet write into.
 *
 * A device whose driver has gone cannot be freed while either exists, since
 * a late completion names the request and the request names the device.
 */
int block_device_holds(block_device_t *device);

/** Advance the request deadlines; the timer calls it once per tick. */
void block_request_tick(void);

/** Completions that found another request in flight, see `disk-overlaps`. */
uint32_t block_overlaps(void);
/** Requests in flight now, see `disk-in-flight`. */
uint32_t block_in_flight(void);
/** Quarantines made, callers released with EIO, and the tick of the last
 * release, cumulative since boot, for /proc/disk. */
uint32_t block_quarantines(void);
uint32_t block_releases(void);
uint32_t block_release_tick(void);

/** Answers the lag device delivered after their lag, cumulative, for
 * /proc/disk: a probe that reads sixteen pages through it expects sixteen. */
uint32_t block_lag_completions(void);

/**
 * Read `rach2.blockpool` and set the limit on live requests, once, before
 * any device can submit; a later call does nothing. The boot calls it before
 * the disk is probed, and the first request calls it too, in case nothing
 * else did.
 */
void block_request_init(void);

/**
 * The device whose `submit` answers for `dev`: a partition view's backing,
 * followed to a device that is not a view; `dev` itself otherwise. A
 * request is accounted to this device, so a stick's views share the
 * stick's share of the limit and its stall.
 */
block_device_t *block_submitter(block_device_t *dev);

/**
 * The lag test device: `rach2.blocklag=N` wraps the boot disk for the
 * partition nodes and the mounts in a device without `BLOCK_FAULT_SAFE` whose
 * requests complete N ticks later, from the timer. The root window keeps the
 * real device. Returns `backing` unchanged when the option is absent.
 */
block_device_t *block_lag_wrap(block_device_t *backing);

/** Complete lagged requests whose ticks have passed; the timer calls it. */
void block_lag_tick(void);

/** The disk the lag device wraps, or NULL when `dev` is not the lag device
 * or the never-answering device beside it. */
block_device_t *block_lag_backing(block_device_t *dev);

/**
 * The device a partition view starting at `start_lba` of `backing` is built
 * over: under `rach2.blockdead=P`, for partition `P` of the lag device, a
 * device of its own that accepts every request and answers none, so a
 * device that stalls has a healthy neighbour on the same pool; `backing`
 * itself otherwise.
 */
block_device_t *block_lag_partition_backing(block_device_t *backing, uint64_t start_lba);

/**
 * The disk under `dev`: a partition view's backing, the lag device's disk,
 * repeated until a device that wraps nothing. Two ranges on one disk are
 * compared on this device, whatever views they were named through.
 */
block_device_t *block_physical(block_device_t *dev);

#endif
