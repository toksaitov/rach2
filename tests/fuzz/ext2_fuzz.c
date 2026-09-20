/**
 * Fuzz harness for the ext2 driver (sys/fs/ext2.c): the input is the
 * volume. The seed is a 64 KiB volume mke2fs made; the format carries no
 * checksum in this revision, so a mutation reaches the parser as it is.
 *
 * fuzz-deps: rach2/sys/fs/ext2.c rach2/sys/fs/bcache.c rach2/sys/fs/fatcommon.c
 * fuzz-deps: rach2/sys/block/chain.c
 * fuzz-deps: rach2/sys/lib/kstring.c rach2/sys/lib/kctypes.c
 * fuzz-deps: tests/host/compat/kheap_host.c tests/host/compat/blockmax_host.c
 * fuzz-deps: tests/host/compat/pressure_host.c
 */
#include "../../rach2/sys/fs/ext2.h"
#include "fs_common.h"
#include "fuzz.h"

int LLVMFuzzerTestOneInput(const unsigned char *data, unsigned long size)
{
    if (size > FS_IMAGE_MAX) {
        size = FS_IMAGE_MAX;
    }
    __builtin_memcpy(fs_image, data, size);
    __builtin_memset(fs_image + size, 0, FS_IMAGE_MAX - size);
    fs_image_bytes = (size + 511) & ~511ul;
    fs_exercise(&vfs_ext2_type);
    return 0;
}
