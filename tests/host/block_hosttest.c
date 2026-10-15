/**
 * block_hosttest.c: the partition view of rach2/sys/block/block.c, over a
 * backing device that records each request.
 *
 * host-deps:
 *
 * The test includes block.c whole and stubs the driver and command-line
 * calls that `block_probe` makes, so the static bounds checks of the view's
 * reads and writes are reachable.
 */

/* The stubs below answer the calls of block_probe, which no row runs. */

/** No AHCI controller. */
int ahci_init(void)
{
    return -1;
}
/** No NVMe controller. */
int nvme_init(void)
{
    return -1;
}
/** No platform disks. */
void platform_disks_probe(void)
{}
/** No token is on the command line. */
int kcmdline_has(const char *token)
{
    (void)token;
    return 0;
}
/** The kernel log; the records are dropped. */
void klog(int level, const char *subsystem, const char *format, ...)
{
    (void)level;
    (void)subsystem;
    (void)format;
}

#include "block/block.c"

static int fails;

#define CHECK(cond)    \
    do {               \
        if (!(cond)) { \
            fails++;   \
        }              \
    } while (0)

static uint32_t backing_reads;
static uint32_t backing_writes;
static uint64_t last_lba;
static uint32_t last_count;

/** The backing's read: record the range and count the call; moves no data
 *  and succeeds. */
static int backing_read(block_device_t *device, uint64_t lba, uint32_t count, void *buffer)
{
    (void)device;
    (void)buffer;
    last_lba = lba;
    last_count = count;
    ++backing_reads;
    return 0;
}

/** The backing's write: record the range and count the call; moves no data
 *  and succeeds. */
static int backing_write(block_device_t *device, uint64_t lba, uint32_t count, const void *buffer)
{
    (void)device;
    (void)buffer;
    last_lba = lba;
    last_count = count;
    ++backing_writes;
    return 0;
}

static uint32_t backing_flushes;
static struct block_request *flushed_with;

/** The backing's flush: record the request it was handed and count the
 *  call; succeeds. */
static int backing_flush(block_device_t *device, struct block_request *request)
{
    (void)device;
    flushed_with = request;
    ++backing_flushes;
    return 0;
}

static block_device_t backing = {512u,          1000u, BLOCK_FAULT_SAFE, 0, 0, 0, backing_read,
                                 backing_write, 0,     backing_flush,    0};

/* The lag wrapper and the registry, which the probe path reaches and this
   test of the partition view does not. */

/** No device is wrapped by the lag wrapper. */
block_device_t *block_lag_backing(block_device_t *dev)
{
    (void)dev;
    return 0;
}

/** The lag wrapper wraps nothing: `dev` itself. */
block_device_t *block_lag_wrap(block_device_t *dev)
{
    return dev;
}

/** The registry has no disks. */
int disk_count(void)
{
    return 0;
}

/** No record. */
struct disk *disk_at(int n)
{
    (void)n;
    return 0;
}

/** A scan that reads nothing and succeeds. */
int disk_scan(struct disk *d, block_device_t *dev)
{
    (void)d;
    (void)dev;
    return 0;
}

/**
 * Run the rows on a 200-sector view at LBA 100 of the 1000-sector backing.
 * Returns the count of failed checks, at most 100.
 *
 * An in-range read and write reach the backing with the base added, the
 * view's last sector included. A flush has no range to translate, and the
 * view hands it on with the request through which the block layer is told
 * of its end. A request past the view's end or of zero sectors fails
 * without reaching the backing. A view that starts past the device is
 * empty, and one that runs past it is cut to the remainder. The disk under
 * a view, and under a view of a view, is the backing, and a device that
 * wraps nothing is its own. A record that begins with its device and is the
 * device's `drvdata`, as a loop device's is, is no view, whatever its next
 * word holds.
 */
int main(void)
{
    block_partition_t part;
    char buf[512];

    block_partition_init(&part, &backing, 100u, 200u);
    CHECK(part.device.sector_count == 200u);

    backing_reads = 0;
    CHECK(part.device.read(&part.device, 10u, 5u, buf) == 0);
    CHECK(backing_reads == 1u && last_lba == 110u && last_count == 5u);
    {
        struct block_request flush_req;

        CHECK(part.device.flush != 0);
        CHECK(part.device.flush(&part.device, &flush_req) == 0);
        CHECK(backing_flushes == 1u && flushed_with == &flush_req);
    }

    CHECK(part.device.read(&part.device, 199u, 1u, buf) == 0);
    CHECK(last_lba == 299u);

    backing_reads = 0;
    CHECK(part.device.read(&part.device, 199u, 2u, buf) != 0);
    CHECK(part.device.read(&part.device, 200u, 1u, buf) != 0);
    CHECK(part.device.read(&part.device, 0u, 201u, buf) != 0);
    CHECK(part.device.read(&part.device, 0u, 0u, buf) != 0);
    CHECK(backing_reads == 0u);

    backing_writes = 0;
    CHECK(part.device.write(&part.device, 0u, 200u, buf) == 0);
    CHECK(last_lba == 100u && last_count == 200u);
    CHECK(part.device.write(&part.device, 1u, 200u, buf) != 0);
    CHECK(backing_writes == 1u);

    block_partition_init(&part, &backing, 1500u, 200u);
    CHECK(part.device.sector_count == 0u);
    CHECK(part.device.read(&part.device, 0u, 1u, buf) != 0);

    block_partition_init(&part, &backing, 900u, 500u);
    CHECK(part.device.sector_count == 100u);

    {
        block_partition_t inner;
        static struct {
            block_device_t dev;
            int used;
            int attaching;
        } loop;

        block_partition_init(&part, &backing, 100u, 200u);
        block_partition_init(&inner, &part.device, 10u, 20u);
        CHECK(block_physical(&part.device) == &backing);
        CHECK(block_physical(&inner.device) == &backing);
        CHECK(block_physical(&backing) == &backing);
        loop.dev.drvdata = &loop;
        loop.used = 1;
        loop.attaching = 0;
        CHECK(block_physical(&loop.dev) == &loop.dev);
    }

    return (fails > 100) ? 100 : fails;
}
