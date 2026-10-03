/**
 * Host test for the block partition view (sys/block/block.c).
 *
 * host-deps:
 *
 * Includes block.c directly and stubs the driver and cmdline calls that
 * block_probe references, so the static partition read/write bounds logic is
 * reachable. A fake backing device records the sector range each request
 * reaches it with. The test asserts a partition forwards an in-range request
 * with its base added, rejects an out-of-range or zero-length request
 * without touching the backing, and clamps a partition that starts or runs
 * past the backing to an empty or truncated view. `block_physical` reaches
 * the backing through a view and through a view of a view, and stops at a
 * device whose `drvdata` points at a record that begins with it, as a loop
 * device's does, which is no view and was once read as one. Returns the
 * failed-check count (0 == pass).
 */

/* The stubs block_probe links against; this test drives only the partition
 * view. */
int ahci_init(void)
{
    return -1;
}
int nvme_init(void)
{
    return -1;
}
void platform_disks_probe(void)
{}
int kcmdline_has(const char *token)
{
    (void)token;
    return 0;
}
/* The probe says what it is doing through the kernel log, which is a whole
   subsystem this test has no use for; the records are dropped. */
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

static int backing_read(block_device_t *device, uint64_t lba, uint32_t count, void *buffer)
{
    (void)device;
    (void)buffer;
    last_lba = lba;
    last_count = count;
    ++backing_reads;
    return 0;
}

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

static int backing_flush(block_device_t *device, struct block_request *request)
{
    (void)device;
    flushed_with = request;
    ++backing_flushes;
    return 0;
}

static block_device_t backing = {512u,          1000u, BLOCK_FAULT_SAFE, 0, 0, 0, backing_read,
                                 backing_write, 0,     backing_flush,    0};

block_device_t *block_lag_backing(block_device_t *dev)
{
    (void)dev;
    return 0;
}

/* The lag wrapper and the registry, which the probe path reaches and this
   test of the partition view does not. */
block_device_t *block_lag_wrap(block_device_t *dev)
{
    return dev;
}

int disk_count(void)
{
    return 0;
}

struct disk *disk_at(int n)
{
    (void)n;
    return 0;
}

int disk_scan(struct disk *d, block_device_t *dev)
{
    (void)d;
    (void)dev;
    return 0;
}

/** The rows: a 200-sector slice at LBA 100 of the 1000-sector backing,
 * its reads and writes forwarded with the base added and its bounds kept. */
int main(void)
{
    block_partition_t part;
    char buf[512];

    block_partition_init(&part, &backing, 100u, 200u);
    CHECK(part.device.sector_count == 200u);

    /* An in-range read forwards with the partition base added. */
    backing_reads = 0;
    CHECK(part.device.read(&part.device, 10u, 5u, buf) == 0);
    CHECK(backing_reads == 1u && last_lba == 110u && last_count == 5u);
    /* A flush has no range to translate; the view hands it on with the
       request the block layer will be told the end through. */
    {
        struct block_request flush_req;

        CHECK(part.device.flush != 0);
        CHECK(part.device.flush(&part.device, &flush_req) == 0);
        CHECK(backing_flushes == 1u && flushed_with == &flush_req);
    }

    /* The last sector, and a run ending exactly at the end, are in range. */
    CHECK(part.device.read(&part.device, 199u, 1u, buf) == 0);
    CHECK(last_lba == 299u);

    /* Out-of-range and zero-length requests are rejected, backing untouched. */
    backing_reads = 0;
    CHECK(part.device.read(&part.device, 199u, 2u, buf) != 0);
    CHECK(part.device.read(&part.device, 200u, 1u, buf) != 0);
    CHECK(part.device.read(&part.device, 0u, 201u, buf) != 0);
    CHECK(part.device.read(&part.device, 0u, 0u, buf) != 0);
    CHECK(backing_reads == 0u);

    /* Writes obey the same bounds. */
    backing_writes = 0;
    CHECK(part.device.write(&part.device, 0u, 200u, buf) == 0);
    CHECK(last_lba == 100u && last_count == 200u);
    CHECK(part.device.write(&part.device, 1u, 200u, buf) != 0);
    CHECK(backing_writes == 1u);

    /* A partition starting past the device is an empty view. */
    block_partition_init(&part, &backing, 1500u, 200u);
    CHECK(part.device.sector_count == 0u);
    CHECK(part.device.read(&part.device, 0u, 1u, buf) != 0);

    /* A partition whose length overruns the device clamps to the remainder. */
    block_partition_init(&part, &backing, 900u, 500u);
    CHECK(part.device.sector_count == 100u);

    /* The disk under a view, and under a view of a view, is the backing; a
       device that wraps nothing is its own. A record that begins with its
       device and is the device's `drvdata`, as a loop device's is, is no
       view, whatever its next word holds. */
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
