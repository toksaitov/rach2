/*
 * block.h: the block device every disk, partition, stick and loop device is,
 * the block request that lets a device sleep its caller, and the counters
 * /proc/disk prints.
 */

#ifndef _BLOCK_H_
#define _BLOCK_H_

#include "../lib/kint.h"
#include "../lib/kirq.h"

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
/* The disk makes a write durable before it answers it, with a flush of its
 * own cache after the transfer, unless the request asks it not to
 * (`BLOCK_REQ_NOFLUSH`). Its `unflushed` says whether it has answered such a
 * write since its last flush; `block_flush` sends a flush only then. */
#define BLOCK_WRITE_THROUGH 0x80u
/* The device's `submit` takes a read whose request names the caller's page
 * frames (`frames`) in place of a buffer of its own, and gives the
 * controller their physical addresses, so that the transfer lands in the
 * caller's pages with no copy: an AHCI disk that reaches every frame and an
 * NVMe namespace, on their interrupts. `block_read_frames` reads any other
 * device through a buffer. */
#define BLOCK_FRAMES        0x100u
/* A file system on the device halted after an error (`bcache_halt_writes`):
 * the cache writes none of its blocks, for a sync, an eviction or the
 * flusher alike, as Linux's ext4 writes nothing after a shutdown. Cleared
 * when the cache drops the device at unmount. */
#define BLOCK_WRITES_HALTED 0x200u
/* While repairing metadata, the first cache write error halts the device. */
#define BLOCK_RECOVERING    0x400u

/**
 * A sector-addressed storage device behind a small table of functions.
 *
 * `read` and `write` transfer `count` whole sectors starting at logical block
 * address `lba`, to or from `buffer`, in the call, and return 0 on success or
 * a negative errno (`-EIO`) on failure. On a `BLOCK_FAULT_SAFE` or
 * `BLOCK_POLLABLE` device they poll, and a caller may invoke them with
 * interrupts off and without the scheduler running. `drvdata` is the
 * driver's private pointer.
 *
 * Every driver serializes on locks of its own, as Linux's drivers do on
 * theirs (libata's `ap->lock`, drivers/ata/libata-core.c:5567). The block
 * layer takes no big lock, and a driver's functions may be called with it
 * or without it: a buffer-cache write holds it, a miss need not. The AHCI,
 * NVMe and ATA drivers need none; USB storage takes it, only when it is
 * free, to start a submitted request (usb/msc.c), and a loop device takes
 * it for the file system its file is on, unless the caller holds it or the
 * file system reads without it (loop.c). A driver completes its requests
 * from an interrupt or a pump that takes only its own locks, or the USB
 * stack's.
 */
struct block_device {
    uint32_t sector_size; /* always BLOCK_SECTOR_SIZE */
    /* The device's size in sectors. A driver whose disk may change size
     * while it is in use sets it with `block_set_sectors`, and a reader that
     * may run beside such a change reads it with `block_sectors`. */
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
     * device without `BLOCK_FAULT_SAFE` has one. It is called with
     * interrupts on and no spinlock held, without the big lock unless the
     * caller holds it; the driver keeps its queue under its own lock, and
     * completes the request under that lock or after it, as Linux's
     * `ata_qc_complete` runs under `ap->lock`
     * (drivers/ata/libata-core.c:4915-4927). */
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
    /* The blocks a sync of the buffer cache writes last, whether one may
     * name an owner's blocks, and their argument (`bcache_sync_last`). NULL
     * names none, and says yes to every owner. */
    int (*sync_last)(void *arg, uint64_t blk);
    int (*sync_names)(void *arg, uint64_t blk, uint64_t owner);
    void *sync_last_arg;
    /* A `BLOCK_WRITE_THROUGH` disk answered a write without its flush, and
     * no flush has reached the disk since. Set and cleared by its driver. */
    volatile int unflushed;
    /* The device stopped answering: every slot of the request pool it held
     * went through its deadline periods unanswered. Its waiters were
     * released with `EIO` and a new request is refused with `EIO`, until a
     * late answer clears it. Set and cleared by the request pool under its
     * lock, a word of its own so that no store to `flags` under another
     * lock can undo it. */
    volatile int stalled;
    /* What /proc/diskstats reports of the device: the transfers that ended
     * well, their sectors and their milliseconds, each way, and the requests
     * in progress. Counted by `block_request_flags`, on the device asked
     * and on the disk `block_physical` reaches under it, under the request
     * pool's lock. */
    struct block_stats {
        uint32_t reads, read_sectors, read_ms;
        uint32_t writes, write_sectors, write_ms;
        uint32_t in_flight, busy_ms;
    } stats;
    /* The most sectors one request may discard (`BLOCK_REQ_DISCARD`) or
     * write zeroes over (`BLOCK_REQ_ZEROES`) through `submit`; 0 for a
     * device that takes no such request. */
    uint32_t max_discard;
    uint32_t max_zeroes;
    /* The buffer cache's counts, under its lock: the writes of its `first`
     * buffers that the disk answered without its flush, a
     * `BLOCK_WRITE_THROUGH` disk when asked to and a disk that is not but
     * has a flush always, and that count when the last flush the cache
     * asked for began. While
     * the two are equal every `first` written is on the medium, and an
     * `after` needs no flush before it (bcache.c, `order_flush`). 64 bits,
     * so that neither wraps. */
    uint64_t firsts_unflushed;
    uint64_t firsts_flushed;
    /* Odd while `block_set_sectors` sets `sector_count`; moved on by each
     * change. */
    volatile uint32_t size_seq;
    /* Byte offset of sector zero within a sector of the backing medium.
     * Zero for a disk, or fixed for the lifetime of a view or attachment. */
    uint32_t sector_phase;
};

/**
 * The size of `device` in sectors, read whole. On i686 a 64-bit count is
 * two loads, and a change of size between them would join halves of two
 * sizes the device never had, so the read is retried while `size_seq` is
 * odd or moved past it, as Linux's i_size_read reads a size on 32-bit SMP
 * (include/linux/fs.h). The accesses are volatile, which keeps them in
 * program order, and x86 keeps loads in order, so no fence is needed. From
 * any context.
 */
static __inline__ uint64_t block_sectors(const block_device_t *device)
{
    const volatile uint64_t *count = &device->sector_count;
    uint32_t seq;
    uint64_t n;

    do {
        seq = device->size_seq;
        n = *count;
    } while ((seq & 1u) != 0 || seq != device->size_seq);
    return n;
}

/**
 * Give `device` the size `sectors` for readers of `block_sectors`, with
 * interrupts off, so that no reader in an interrupt of this processor
 * spins on the odd count. Writers of one device are serialized by its
 * driver's lock, or by the big lock where only processes set it; x86 keeps
 * stores in order, so no fence is needed.
 */
static __inline__ void block_set_sectors(block_device_t *device, uint64_t sectors)
{
    volatile uint64_t *count = &device->sector_count;
    uint32_t flags = kirq_save();

    device->size_seq++;
    *count = sectors;
    device->size_seq++;
    kirq_restore(flags);
}

/**
 * Whether `block_flush` of `device`, the submitter, has nothing to send: a
 * device with no `flush` has nothing of its own to flush, and a
 * `BLOCK_WRITE_THROUGH` disk not marked `unflushed` flushed every write
 * before it answered it.
 */
static __inline__ int block_flush_skips(const block_device_t *device)
{
    return !device->flush || ((device->flags & BLOCK_WRITE_THROUGH) && !device->unflushed);
}

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
 * Runs the AHCI, NVMe, virtio block and platform drivers (on a PC, ATA) in
 * that order; each registers the disks it finds, the first disk found is
 * `block0`, and every disk's partition table is then read. Under
 * `rach2.nodisk` it probes nothing. Returns 0 when a device answered and
 * `block0` is set, or -1 when none did. Polled and without interrupts, so
 * it may run before the scheduler does.
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
 * while a command is in flight, a wait tick one tick a caller slept for one;
 * `block_count_wait_ticks` counts `n` of them.
 * A driver that spins reads the status thousands of times a command; one
 * that sleeps reads it a few times and is woken. `/proc/disk` prints both, so
 * a test can require the sleep and not the seconds.
 */
uint64_t block_polls(void);
uint64_t block_wait_ticks(void);
void block_count_poll(void);
void block_count_wait_ticks(uint32_t n);

/**
 * The queued commands a polled command found active on its port and finished
 * first by polling, since boot (`ahci_drain`): each is a page fault's command
 * meeting the queue of the same disk. `/proc/disk` prints the count as
 * `disk-drains`, so a test can require the meeting and not a number of
 * transfers made beside it.
 */
uint64_t block_drains(void);
void block_count_drain(void);

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
struct wait_queue;

/**
 * What a read started without waiting calls at the end of each of its
 * requests (`block_read_frames_async`): `arg` as the caller gave it,
 * `index` the place among the caller's frames of the first of the `pages`
 * frames `frames` the request filled, and `status` 0 or a negative errno,
 * `-EIO` for a request given up on, whose frames may still be the device's
 * to fill. It runs in the context the device completes in, an interrupt
 * or the tick among them, under the driver's lock and with none of the
 * pool's, so it takes only spinlocks taken with interrupts off and does
 * not sleep, as Linux's `bi_end_io` runs (block/bio.c:1585-1617).
 */
typedef void (*block_frames_fn)(void *arg, uint32_t index, const uintptr_t *frames, uint32_t pages,
                                int status);

/* A completion and where its caller's frames start: `fn` is called with
 * `arg` and `index` plus the place of a request's first frame among the
 * frames it was given. */
struct block_frames_done {
    block_frames_fn fn;
    void *arg;
    uint32_t index;
};

struct block_request {
    struct block_request *next;   /* the list of live requests */
    struct block_request *queued; /* the device's own queue, oldest first */
    block_device_t *device;
    block_device_t *owner; /* the device the request is accounted to */
    int write;
    uint64_t lba;
    uint32_t count; /* sectors, at most the device's `max_sectors` */
    uint8_t *page;  /* the request's buffer: a DMA-pool frame, or
                     * `pages` heap pages for a larger transfer; NULL
                     * for a read into `frames` */
    uint32_t pages; /* pages of the buffer, 0 with `frames` */
    /* A read into page frames (`block_read_frames`): the physical address
     * of each page the transfer fills, in order, a page's eight sectors
     * each and the last one's what is left, with a reference of the
     * request's on every frame, so that a late answer lands in pages
     * nothing else owns; NULL for a request with a buffer. */
    uintptr_t *frames;
    int status;          /* 0 or -errno, valid once done */
    volatile int state;  /* BLOCK_REQ_* */
    uint32_t generation; /* unique to this request */
    uint32_t deadline;   /* tick at which the next period ends */
    uint32_t taken_tick; /* the deadlines' clock when it was taken */
    uint32_t overdue;    /* periods passed unanswered */
    int held_by_caller;  /* until the caller has read the answer or given up */
    int held_by_device;  /* until the device has answered */
    uint32_t taken_us;   /* when it was taken, for the completion's delay */
    uint32_t flags;      /* BLOCK_REQ_NOFLUSH, BLOCK_REQ_FLUSH or 0 */
    /* The queue on its caller's stack that the caller sleeps on for the
     * answer, which only the answer or the quarantine wakes, or NULL; set
     * and cleared by the caller and read by the waker, all under the
     * pool's lock (request.c). */
    struct wait_queue *waiter;
    /* A read started without a waiter (`block_read_frames_async`): what its
     * answer or its quarantine calls, with `done_arg` and the index of the
     * request's first frame among the caller's, `done_index`; NULL for a
     * request whose caller waits or polls. */
    block_frames_fn done_fn;
    void *done_arg;
    uint32_t done_index;
    /* The read's first sector as `owner`, the device that answers, numbers
     * it (`block_submitter_lba`), which `block_settle` compares: `lba` is
     * the caller's view's until a partition's submit adds its offset. */
    uint64_t done_lba;
    /* Who calls `done_fn` (request.c's REQ_END_*): the answer, unless the
     * tick claimed the call in the hold of the pool's lock that quarantined
     * the read, which leaves it to the tick whenever the answer comes. */
    int done_ended;
};

/* A write a `BLOCK_WRITE_THROUGH` disk answers without flushing its cache,
 * for a caller that flushes once after several writes, and a request that
 * is only that flush, with no transfer. A device that is not write-through
 * ignores the first and never sees the second. */
#define BLOCK_REQ_NOFLUSH 0x1u
#define BLOCK_REQ_FLUSH   0x2u
/* A request that moves no data: let the device forget `count` sectors at
 * `lba`, which then read as anything, or write zeroes over them, each on a
 * device whose `max_discard` or `max_zeroes` allows it (`block_discard`). */
#define BLOCK_REQ_DISCARD 0x4u
#define BLOCK_REQ_ZEROES  0x8u

/* A request of a device whose `max_sectors` is 0: one page. */
#define BLOCK_REQ_SECTORS     8u
/* The most any device may take in one request: 1 MiB, 256 pages of a heap
 * buffer described page by page to the controller. Each driver declares
 * its own limit within this one, and `block_max_sectors` caps it here: the
 * AHCI and ATA disks' limit is the whole 2048, which ATA moves in commands
 * of 64, an NVMe namespace's 8 times 2 to the power of its controller's
 * MDTS, at most 2048, and the USB stick's 1024. */
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
 * Read `count` sectors at `lba` into the page frames `frames`, sectors
 * 8i to 8i + 7 into the frame `frames[i]`, the last frame taking what is
 * left of `count` and the rest of it left as it was.
 *
 * On a device with `BLOCK_FRAMES`, from a caller that may sleep, the
 * controller is given the frames themselves, as Linux's bio names the
 * pages of the page cache (block/bio.c), so that the transfer touches no
 * buffer and the processor no byte of it; each request takes a reference
 * on its frames until the device has let go of it. Any other device, or a
 * caller that cannot sleep, reads through a buffer from the heap, a page
 * at a time when the heap has none of the length, and the bytes are copied
 * into the frames. The context rules and the answer are `block_request`'s.
 *
 * After an error the frames not filled may still be the device's: a request
 * given up on while its command is outstanding keeps its references until
 * the device answers or is reset, and its transfer may land then. The
 * caller lets go of such a frame and reads again into another, as Linux
 * leaves a folio locked and not up to date until its bio ends.
 */
int block_read_frames(block_device_t *device, uint64_t lba, uint32_t count,
                      const uintptr_t *frames);

/**
 * Start a read of `count` sectors at `lba` into the page frames `frames`,
 * laid out as `block_read_frames` lays them, and return once its requests
 * are submitted, without waiting for the device, as Linux's
 * `mpage_readahead` submits its bio and returns (fs/mpage.c). Each request
 * holds a reference on its frames until the device lets go of it, and its
 * answer, or its quarantine past its deadlines, calls `done->fn` once for
 * the frames it covers (`struct block_frames_done`).
 *
 * Only a device with `BLOCK_FRAMES` and `submit` takes such a read, from a
 * caller that may sleep, which waits, as `block_read_frames` does, for room
 * in the pool. The sectors whose requests were submitted, each of which
 * calls `done->fn`, from the first, a multiple of a page's but for the end of
 * `count`; `-EAGAIN` when the device or the caller cannot take the read so,
 * nothing started, for the caller to read with `block_read_frames`; or a
 * negative errno when nothing started.
 */
int block_read_frames_async(block_device_t *device, uint64_t lba, uint32_t count,
                            const uintptr_t *frames, const struct block_frames_done *done);

/**
 * Sleep until no read started with `block_read_frames_async` on `device`
 * and still in flight overlaps the `count` sectors at `lba`, for a file
 * system about to free those sectors to another file, so that no read of
 * the old file's lands after the new one's write, as Linux's truncation
 * waits on each locked folio (mm/truncate.c). The caller holds what keeps
 * new reads of the sectors from starting. From a caller that may sleep.
 */
void block_settle(block_device_t *device, uint64_t lba, uint64_t count);

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
 * Discard the `count` sectors at `lba` of `device`, or with `zeroes` write
 * zeroes over them, as Linux's `blkdev_issue_discard` and
 * `blkdev_issue_zeroout` do for BLKDISCARD and BLKZEROOUT.
 *
 * The range goes to the device under any partition views in requests of at
 * most its `max_discard` or `max_zeroes` sectors, each sleeping its caller
 * as `block_request` does, from syscall context only. Writing zeroes on a
 * device without `max_zeroes` writes pages of zeroes instead, as Linux's
 * `__blkdev_issue_zero_pages` does without BLKDEV_ZERO_NOFALLBACK.
 * @return 0; -EOPNOTSUPP to discard on a device without `max_discard` or
 *         without `submit`; -EIO for a range past the device, a request
 *         that failed or a stalled device; -ENOMEM.
 */
int block_discard(block_device_t *device, uint64_t lba, uint64_t count, int zeroes);

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
 * Sector `lba` of `dev` as `block_submitter(dev)` numbers it: `lba` plus the
 * offset of each partition view followed down to it, which is what a
 * partition's submit makes of a request's `lba`. Two ranges named through
 * different views of one device are compared so.
 */
uint64_t block_submitter_lba(block_device_t *dev, uint64_t lba);

/**
 * The lag test device: `rach2.blocklag=N` wraps disk 0 for the partition
 * nodes and the mounts in a device without `BLOCK_FAULT_SAFE` whose requests
 * complete N ticks later, from the timer. The root window keeps the real
 * device. Returns `backing` unchanged when the option is absent or is not
 * a count from 1 to 1000000, and when `backing` is not the device the
 * wrapper is over.
 */
block_device_t *block_lag_wrap(block_device_t *backing);

/** Complete lagged requests whose ticks have passed; the timer calls it. */
void block_lag_tick(void);

/**
 * Hold timer delivery when `hold` is nonzero, or resume it otherwise.
 * Kernel test fixtures only, under the kernel lock; -ENODEV before a lag
 * wrapper exists, 0 otherwise. Holding does not stop backing transfers or
 * request deadlines. Fixtures advance deadlines with `block_request_tick`
 * in a host model and must not mix its clock with real timer interrupts.
 */
int block_lag_hold(int hold);

/**
 * Deliver the held answer captured with `generation`, or its error duplicate
 * when `duplicate` is nonzero. Kernel test fixtures only, under the kernel
 * lock. Returns 0, -EPERM outside held mode, -ENOENT for no captured answer,
 * or -EAGAIN before the backing transfer returns or for the wrong phase.
 * A duplicate can follow caller release and request-address reuse; it never
 * reads the old request. Each answer and duplicate can be delivered once.
 */
int block_lag_answer(uint32_t generation, int duplicate);

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
