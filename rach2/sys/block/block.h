#ifndef _BLOCK_H_
#define _BLOCK_H_

#include "../lib/kint.h"

/** The sector size every block device in Rach2 uses. */
#define BLOCK_SECTOR_SIZE 512

typedef struct block_device block_device_t;

/**
 * A sector-addressed storage device behind a small read/write vtable.
 *
 * `read` and `write` transfer `count` whole sectors starting at logical block
 * address `lba`, to or from `buffer`, and return 0 on success or a negative
 * errno (`-EIO`) on failure. They are synchronous and polled: a caller may
 * invoke them with interrupts off and without the scheduler running. `drvdata`
 * is the backend's private pointer.
 */
struct block_request;

/* The device completes a transfer by polling and may be used with interrupts
 * off. ATA, AHCI and every partition view over them have it; a device that
 * sleeps its caller does not, and the page-cache window refuses such a device
 * as a backing store. */
#define BLOCK_FAULT_SAFE 0x1u
/* Set by the buffer cache when a write to the device failed; cleared when
 * the cache drops the device at unmount. */
#define BLOCK_WRITE_FAILED 0x2u
/* The medium refuses writes and says so: a stick with its switch across, or
 * a partition view over one. Every mount of it is read-only. */
#define BLOCK_READ_ONLY 0x4u
/* The device sleeps its caller and answers in the same call: a loop device
 * over a file, whose read is a read of that file through the file system
 * holding it. It is neither of the two above: it may sleep, so it cannot be
 * called with interrupts off, and it must not take a request slot, because
 * the device under it takes one from the same pool of sixteen while it
 * waits. It is called directly, in syscall context only. */
#define BLOCK_CALLER_SYNC 0x10u
/* The device has gone: a stick pulled out. Nothing can reach it again, so an
 * unmount writes nothing and only lets go of what it holds. Set by the cache
 * when the disk layer tells it, cleared when the device is published again. */
#define BLOCK_GONE 0x8u

struct block_device {
    uint32_t sector_size;
    uint64_t sector_count;
    uint32_t flags;          /* BLOCK_FAULT_SAFE, or nothing */
    uint32_t deadline_ticks; /* sleeping path: give up after this many ticks;
                              * 0 means the 5-second default */
    int (*read)(block_device_t *device, uint64_t lba, uint32_t count,
                void *buffer);
    int (*write)(block_device_t *device, uint64_t lba, uint32_t count,
                 const void *buffer);
    /* Start one request and return; the device reports the end through
     * `block_request_complete`, from whatever context it completes in. Only a
     * device without `BLOCK_FAULT_SAFE` has one. */
    int (*submit)(block_device_t *device, struct block_request *request);
    /* Put what the device holds in a cache of its own on the medium, and
     * report the end the same way a transfer does. A device with nothing of
     * its own to flush has none of this, and the caller takes that for done.
     * A stick has one, because its cache is the reason a volume can be whole
     * on the wire and not on the medium. */
    int (*flush)(block_device_t *device, struct block_request *request);
    /* The device this one's bytes live on, or NULL for a device that is its
     * own medium. A loop device names the device of the file system holding
     * its file, and the buffer cache reads the chain to decide whether a
     * caller holding one device's buffer may take another's. */
    block_device_t *backing;
    void *drvdata;
};

/* How long a chain of backing devices may be: a loop device stands on a real
 * device and nothing stands on it. One is what the four buffers a process may
 * hold can serve: a file system on a loop device holds a buffer of it while
 * the file system under it holds two of the disk, and a second loop device in
 * the middle would want two more than there are. A device whose chain would
 * be longer is refused where it is attached. */
#define BLOCK_MAX_DEPTH 1

/**
 * How many devices are under `device`, following its backing chain.
 *
 * Zero for a device that is its own medium. A chain longer than
 * `BLOCK_MAX_DEPTH` answers `BLOCK_MAX_DEPTH + 1`, which is what a caller
 * refuses on.
 */
int block_depth(block_device_t *device);

/**
 * Whether `ancestor` is under `device`: its backing, or its backing's.
 */
int block_is_below(block_device_t *ancestor, block_device_t *device);

/**
 * The whole-disk boot device, or `NULL` until `block_probe` finds a disk.
 */
extern block_device_t *block0;

/**
 * Bring up the boot block device and publish it as `block0`.
 *
 * Selects the AHCI driver when `rach2.disk=ahci` is on the kernel command
 * line (falling back to ATA if no AHCI HBA is present), and the ATA driver
 * otherwise. Returns 0 when a device answered and `block0` is set, or -1
 * when none did. Polled and IRQ-free, so it is safe to call before the
 * scheduler runs.
 */
int block_probe(void);

/**
 * Report a boot disk, probing only if none has been established.
 *
 * `block_probe` resets and re-identifies a controller every time it is called,
 * which is work a boot has no reason to repeat and which several callers do:
 * the disk mount, the diagnostic, and the device discovery that runs before
 * either. This answers from `block0` when a disk has already answered, so a
 * working controller is never reset a second time.
 *
 * A failure is not remembered: a controller that did not answer at boot can
 * be probed again from the rescue shell, which exists to inspect a machine
 * after a failed start.
 *
 * @return 0 when a device answered and `block0` is set, -1 when none did.
 */
int block_ready(void);

/**
 * How many commands each driver has issued to the device, and how many of those
 * were cache flushes.
 *
 * The raw windows count their own calls into the block interface, which says
 * how well devfs batched but nothing about what the driver did underneath: a
 * driver that broke one 64-sector request into 64 commands would look identical
 * from above and cost exactly the million cache flushes batching exists to
 * avoid. These count where the commands actually leave for the device.
 */
uint64_t block_commands(void);
uint64_t block_flushes(void);

/** Record one command, and separately one cache flush, as it is issued. */
void block_count_command(void);
void block_count_flush(void);

/**
 * A partition: the sector range `[start_lba, start_lba + sector_count)` of a
 * backing device, presented as its own `block_device`.
 *
 * The view's `read`/`write` add `start_lba` to each request and forward it to
 * the backing device, rejecting any request that runs past the partition. The
 * `device` member is first, so a `block_device_t *` handed to the vtable can be
 * recovered as the enclosing `block_partition_t`.
 */
typedef struct block_partition {
    block_device_t device;
    block_device_t *backing;
    uint64_t start_lba;
} block_partition_t;

/**
 * Initializes `partition` as the `sector_count`-sector slice of `backing` that
 * starts at `start_lba`. After this, `&partition->device` is a usable device.
 */
void block_partition_init(
         block_partition_t *partition,
         block_device_t *backing,
         uint64_t start_lba,
         uint64_t sector_count
     );

/* How a request stands. A request the caller gave up on is quarantined with
 * its page until the device answers, late, so the answer has nowhere to land
 * but a page nothing else owns. */
#define BLOCK_REQ_FREE        0
#define BLOCK_REQ_IN_FLIGHT   1
#define BLOCK_REQ_DONE        2
#define BLOCK_REQ_QUARANTINED 3

/**
 * One request on a sleeping device: at most a page, in the request's own page.
 *
 * A write is copied into the page before `submit` and a read is copied out
 * after a successful completion, so a completion that arrives after the
 * deadline lands in the pool's page and not in memory the caller reused. The
 * slot is quarantined until then; the late completion frees it and advances
 * the generation, and a completion whose generation does not match the slot's
 * is dropped with a record.
 */
struct block_request {
    block_device_t *device;
    int write;
    uint64_t lba;
    uint32_t count;        /* sectors, at most BLOCK_REQ_SECTORS */
    uint8_t *page;         /* the pool's page for this slot */
    int status;            /* 0 or -errno, valid once done */
    volatile int state;    /* BLOCK_REQ_* */
    uint32_t generation;   /* advanced when the slot is reused or abandoned */
    uint32_t deadline;     /* tick at which the wait gives up */
};

#define BLOCK_REQ_SECTORS 8u

/**
 * Transfer `count` whole sectors, sleeping the caller when the device does.
 *
 * On a `BLOCK_FAULT_SAFE` device this calls the vtable and returns, from any
 * context, at boot included. On a device with `submit` it runs from syscall
 * context only, splits the transfer into requests of at most a page, and
 * sleeps between `submit` and the completion or the device's deadline.
 * Returns 0 or a negative errno; a deadline is `-EIO` with the slot
 * quarantined.
 */
int block_request(block_device_t *device, int write, uint64_t lba,
                  uint32_t count, void *buffer);

/**
 * Put what the device holds in a cache of its own on the medium.
 *
 * Sleeps the caller like `block_request` does, and from syscall context
 * only. A device with no `flush` has nothing of its own to flush and this
 * answers 0 at once; a stick's flush is `SYNCHRONIZE CACHE`, and the mounts
 * call this after the buffer cache has written back, at an unmount, a sync
 * and an fsync, because a volume whole on the wire is not yet whole on the
 * medium.
 */
int block_flush(block_device_t *device);

/**
 * Start a read of at most a page without waiting for it.
 *
 * For a caller that cannot sleep: the disk layer reads a stick's partition
 * table from the pump, where there is no process to put to sleep. The
 * request is the pool's slot, and `generation` is what the caller asks
 * after it with. Returns NULL when the pool is exhausted or the device
 * refused the request.
 */
struct block_request *block_request_begin(block_device_t *device, uint64_t lba, uint32_t count,
                                          uint32_t *generation);

/**
 * Whether a request begun with `block_request_begin` has ended.
 *
 * Returns 0 while it is in flight. Otherwise 1, with `status` the result
 * and, for a read that succeeded, its sectors copied into `buffer`; the slot
 * is freed. A request past its deadline is quarantined by the tick and ends
 * here with `EIO`, and its slot stays the device's until the late completion.
 */
int block_request_ended(struct block_request *r, uint32_t generation, void *buffer, int *status);

/**
 * A sleeping device reports one request's end.
 *
 * `generation` is the value the device captured at `submit`. A completion
 * whose generation is not the request's current one is dropped with a record;
 * one for a quarantined request frees the slot.
 */
void block_request_complete(struct block_request *request, uint32_t generation,
                            int status);

/**
 * How many of the pool's slots are still the device's: in flight, or
 * quarantined with a page the device may yet write into.
 *
 * A device whose driver has gone cannot be freed while either exists, since
 * a late completion names the slot and the slot names the device.
 */
int block_device_holds(block_device_t *device);

/** Advance the request deadlines; the timer calls it once per tick. */
void block_request_tick(void);

/**
 * The lag test device: `rach2.blocklag=N` wraps the boot disk for the
 * partition nodes and the mounts in a device without `BLOCK_FAULT_SAFE` whose
 * requests complete N ticks later, from the timer. The root window keeps the
 * real device. Returns `backing` unchanged when the option is absent.
 */
block_device_t *block_lag_wrap(block_device_t *backing);

/** Complete lagged requests whose ticks have passed; the timer calls it. */
void block_lag_tick(void);

/** The disk the lag device wraps, or NULL when `dev` is not the lag device. */
block_device_t *block_lag_backing(block_device_t *dev);

/**
 * The disk under `dev`: a partition view's backing, the lag device's disk,
 * repeated until a device that wraps nothing. Two ranges on one disk are
 * compared on this device, whatever views they were named through.
 */
block_device_t *block_physical(block_device_t *dev);

#endif
