/**
 * Seam test for the block partition view (sys/block/block.c).
 *
 * host-deps:
 *
 * Includes block.c directly and stubs the driver/cmdline seam that block_probe
 * references, so the static partition read/write bounds logic is reachable. A
 * fake backing device records the sector range each request reaches it with. The
 * test asserts a partition forwards an in-range request with its base added,
 * rejects an out-of-range or zero-length request without touching the backing,
 * and clamps a partition that starts or runs past the backing to an empty or
 * truncated view. Returns the failed-check count (0 == pass).
 */

/* The seam block_probe links against; this test drives only the partition
 * view. */
int ata_init(void)
{
    return -1;
}
int ahci_init(void)
{
    return -1;
}
int kcmdline_has(const char *token)
{
    (void)token;
    return 0;
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

static block_device_t backing = {512u, 1000u, backing_read, backing_write, 0};

int main(void)
{
    block_partition_t part;
    char buf[512];

    /* A 200-sector slice starting at LBA 100 of the 1000-sector backing. */
    block_partition_init(&part, &backing, 100u, 200u);
    CHECK(part.device.sector_count == 200u);

    /* An in-range read forwards with the partition base added. */
    backing_reads = 0;
    CHECK(part.device.read(&part.device, 10u, 5u, buf) == 0);
    CHECK(backing_reads == 1u && last_lba == 110u && last_count == 5u);

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

    return (fails > 100) ? 100 : fails;
}
