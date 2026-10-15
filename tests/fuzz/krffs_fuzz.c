/**
 * Fuzz harness for the KRFFS core (sys/fs/krffs.c): the input is the volume,
 * reached through a memory backend as the host test reaches it. The harness
 * lists the root, walks to each entry and reads it. A call that fails has
 * rejected the volume, which is a valid result; the faults the harness exists
 * to find are the reads and writes out of bounds that the sanitizers report.
 *
 * fuzz-deps: rach2/sys/fs/krffs.c rach2/sys/lib/kstring.c
 * fuzz-deps: rach2/sys/lib/kctypes.c
 */
#include "../../rach2/sys/fs/krffs.h"
#include "fuzz.h"

/** The kernel log: discarded. */
void klog(int level, const char *subsystem, const char *format, ...)
{
    (void)level;
    (void)subsystem;
    (void)format;
}

/** The console: discarded. */
void console_printf(const char *format, ...)
{
    (void)format;
}

/** A fixed time of day, 1000000000 seconds after the epoch. */
void rtc_realtime(int64_t *seconds, int64_t *nanoseconds)
{
    *seconds = 1000000000;
    *nanoseconds = 0;
}

#define IMAGE_MAX (1u << 20)
static unsigned char image[IMAGE_MAX];
static uint32_t image_size;

/** Read `size` bytes at `offset` of the image; -1 past its end. */
static int memory_read(void *ctx, uint64_t offset, void *buffer, uint32_t size)
{
    (void)ctx;
    if (offset + size > image_size) {
        return -1;
    }
    __builtin_memcpy(buffer, image + offset, size);
    return 0;
}

/** Write `size` bytes at `offset` of the image; -1 past its end. */
static int memory_write(void *ctx, uint64_t offset, const void *buffer, uint32_t size)
{
    (void)ctx;
    if (offset + size > image_size) {
        return -1;
    }
    __builtin_memcpy(image + offset, buffer, size);
    return 0;
}

/** Nothing to flush: the image is memory. */
static int memory_sync(void *ctx)
{
    (void)ctx;
    return 0;
}

static const krffs_backend_t memory_backend = {memory_read, memory_write, memory_sync};

/**
 * Take the input, cut to `IMAGE_MAX` bytes, as the volume, and list up to
 * 64 entries of its root, while `krffs_readdir_at` returns 1 for an entry;
 * walk to each and read the first 4 KiB of each file.
 */
int LLVMFuzzerTestOneInput(const unsigned char *data, unsigned long size)
{
    krffs_mount_t mount;
    static char buf[4096];
    char name[256];
    char path[264];
    uint32_t i;
    int is_dir;

    if (size > IMAGE_MAX) {
        size = IMAGE_MAX;
    }
    __builtin_memcpy(image, data, size);
    image_size = (uint32_t)size;
    mount.backend = &memory_backend;
    mount.ctx = 0;
    mount.size = image_size;
    mount.grain = 0;
    for (i = 0; i < 64; i++) {
        uint64_t node;
        unsigned j;

        if (krffs_readdir_at(&mount, "/", i, name, sizeof(name), &is_dir) != 1) {
            break;
        }
        path[0] = '/';
        for (j = 0; j < sizeof(name) && name[j] != '\0'; j++) {
            path[j + 1] = name[j];
        }
        path[j + 1] = '\0';
        node = krffs_walk(&mount, path);
        if (node != 0 && !is_dir) {
            (void)krffs_read_at(&mount, node, buf, sizeof(buf), 0);
        }
    }
    return 0;
}
