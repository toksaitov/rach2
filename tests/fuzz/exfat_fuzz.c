/**
 * Fuzz harness for the exFAT driver (sys/fs/exfat.c): the input is the two
 * boot regions, the start of the table and the start of the cluster heap,
 * scattered into a 1 MiB volume the fixture maker builds in memory. The
 * boot checksum sector of each region is recomputed after the scatter, since
 * the driver refuses a region whose checksum does not match and the fields
 * behind the boot sector would never be read. The driver accepts either
 * region, and its checksum skips bytes 106, 107 and 112, so a mutation of
 * those three reaches it whatever the checksum says.
 *
 * fuzz-deps: rach2/sys/fs/exfat.c rach2/sys/fs/bcache.c rach2/sys/block/chain.c
 * fuzz-deps: rach2/sys/fs/fatcommon.c
 * fuzz-deps: rach2/sys/lib/kstring.c rach2/sys/lib/kctypes.c
 * fuzz-deps: tests/host/compat/kheap_host.c tests/host/compat/blockmax_host.c
 * fuzz-deps: tests/host/compat/pressure_host.c tests/host/mkexfatfixture.c
 */
#include "../../rach2/sys/fs/exfat.h"
#include "fs_common.h"
#include "fuzz.h"

/* The input is what the driver parses, scattered into the seed volume: the
   two boot regions, the first 4 KiB of the table and the first 32 KiB of
   the cluster heap, where the bitmap, the up-case table and the root
   directory are. The two offsets are read from the seed's boot sector. */
#define IN_BOOT      (24u * 512u)
#define IN_FAT       4096u
#define IN_HEAP      32768u
#define INPUT_MAX    (IN_BOOT + IN_FAT + IN_HEAP)
#define SEED_SECTORS 2048u

extern unsigned char *mkexfatfixture_build(unsigned long sectors, unsigned long *bytes);

static const unsigned char *seed;
static unsigned long seed_bytes;
static unsigned long fat_at, heap_at;

static uint32_t le32_at(const unsigned char *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/**
 * The region at `at` takes the input's bytes from `from`, and zeros where the
 * input ends early.
 *
 * A short input then loses the regions after its end instead of keeping the
 * seed's, so libFuzzer cannot shorten an input to its first bytes and call
 * the coverage the same. Nothing is read from `data` when the input ends
 * before `from`, since `data + from` would be past the allocation.
 */
static void scatter(const unsigned char *data, unsigned long size, unsigned long at,
                    unsigned long from, unsigned long len)
{
    unsigned long have = size > from ? size - from : 0;

    if (have > len) {
        have = len;
    }
    if (have > 0) {
        __builtin_memcpy(fs_image + at, data + from, have);
    }
    __builtin_memset(fs_image + at + have, 0, len - have);
}

/**
 * The boot checksum of one region, sectors 0 to 10 folded into sector 11,
 * skipping the three bytes the specification leaves out.
 */
static void boot_checksum(unsigned char *region)
{
    uint32_t sum = 0;
    unsigned i;

    for (i = 0; i < 11 * 512; i++) {
        if (i == 106 || i == 107 || i == 112) {
            continue;
        }
        sum = ((sum & 1) ? 0x80000000u : 0u) + (sum >> 1) + region[i];
    }
    for (i = 0; i < 512; i += 4) {
        region[11 * 512 + i] = (unsigned char)sum;
        region[11 * 512 + i + 1] = (unsigned char)(sum >> 8);
        region[11 * 512 + i + 2] = (unsigned char)(sum >> 16);
        region[11 * 512 + i + 3] = (unsigned char)(sum >> 24);
    }
}

int LLVMFuzzerTestOneInput(const unsigned char *data, unsigned long size)
{
    if (seed == 0) {
        seed = mkexfatfixture_build(SEED_SECTORS, &seed_bytes);
        if (seed == 0 || seed_bytes > FS_IMAGE_MAX) {
            return 0;
        }
        fat_at = le32_at(seed + 80) * 512ul;
        heap_at = le32_at(seed + 88) * 512ul;
    }
    if (size > INPUT_MAX) {
        size = INPUT_MAX;
    }
    __builtin_memcpy(fs_image, seed, seed_bytes);
    scatter(data, size, 0, 0, IN_BOOT);
    scatter(data, size, fat_at, IN_BOOT, IN_FAT);
    scatter(data, size, heap_at, IN_BOOT + IN_FAT, IN_HEAP);
    boot_checksum(fs_image);
    boot_checksum(fs_image + 12 * 512);
    fs_image_bytes = seed_bytes;
    fs_exercise(&vfs_exfat_type);
    return 0;
}
