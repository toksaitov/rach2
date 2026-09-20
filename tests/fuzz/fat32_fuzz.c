/**
 * Fuzz harness for the FAT driver (sys/fs/fat32.c): the input is the boot
 * sector, the start of the first table and the start of the data region,
 * scattered into the fixture maker's 4 MiB volume, built in memory once by
 * the maker's own code; the rest of the volume is the seed's.
 *
 * fuzz-deps: rach2/sys/fs/fat32.c rach2/sys/fs/bcache.c rach2/sys/block/chain.c
 * fuzz-deps: rach2/sys/fs/fat32name.c rach2/sys/fs/fatcommon.c
 * fuzz-deps: rach2/sys/lib/kstring.c rach2/sys/lib/kctypes.c
 * fuzz-deps: tests/host/compat/kheap_host.c tests/host/compat/blockmax_host.c
 * fuzz-deps: tests/host/compat/pressure_host.c tests/host/mkfatfixture.c
 */
#include "../../rach2/sys/fs/fat32.h"
#include "fs_common.h"
#include "fuzz.h"

/* The input is what the driver parses, scattered into the seed volume:
   the boot sector, the first 4 KiB of the first table and the first 16 KiB
   of the data region, where the root directory and the first files are.
   The two offsets are read from the seed's own boot sector. */
#define IN_BOOT   512u
#define IN_FAT    4096u
#define IN_DATA   16384u
#define INPUT_MAX (IN_BOOT + IN_FAT + IN_DATA)

/* The maker's builder, in its own translation unit (FIXTURE_NO_MAIN). */
extern unsigned char *mkfatfixture_build(const char *option, unsigned long *bytes);

static const unsigned char *seed;
static unsigned long seed_bytes;
static unsigned long fat_at, data_at;

static uint32_t le16_at(const unsigned char *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8);
}

static uint32_t le32_at(const unsigned char *p)
{
    return le16_at(p) | (le16_at(p + 2) << 16);
}

/* The region at `at` takes the input's bytes from `from`, and zeros where
   the input ends early: a short input then loses the regions after its
   end instead of keeping the seed's, so libFuzzer cannot shorten an input
   to its first bytes and call the coverage the same. */
static void scatter(const unsigned char *data, unsigned long size, unsigned long at,
                    unsigned long from, unsigned long len)
{
    unsigned long have = size > from ? size - from : 0;

    if (have > len) {
        have = len;
    }
    __builtin_memcpy(fs_image + at, data + from, have);
    __builtin_memset(fs_image + at + have, 0, len - have);
}

int LLVMFuzzerTestOneInput(const unsigned char *data, unsigned long size)
{
    if (seed == 0) {
        seed = mkfatfixture_build(0, &seed_bytes);
        if (seed == 0 || seed_bytes > FS_IMAGE_MAX) {
            return 0;
        }
        fat_at = le16_at(seed + 14) * 512ul;
        data_at = fat_at + (unsigned long)seed[16] * le32_at(seed + 36) * 512ul;
    }
    if (size > INPUT_MAX) {
        size = INPUT_MAX;
    }
    __builtin_memcpy(fs_image, seed, seed_bytes);
    scatter(data, size, 0, 0, IN_BOOT);
    scatter(data, size, fat_at, IN_BOOT, IN_FAT);
    scatter(data, size, data_at, IN_BOOT + IN_FAT, IN_DATA);
    fs_image_bytes = seed_bytes;
    fs_exercise(&vfs_vfat_type);
    return 0;
}
