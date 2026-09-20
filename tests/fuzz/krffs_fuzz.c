/**
 * Fuzz harness for the KRFFS core (sys/fs/krffs.c): the input is the volume,
 * reached through a memory backend as the host test reaches it. The walk
 * lists the root, walks to each entry and reads it; a node the core refuses
 * is its answer, and the sanitizers watch what it reads on the way.
 *
 * fuzz-deps: rach2/sys/fs/krffs.c rach2/sys/lib/kstring.c
 * fuzz-deps: rach2/sys/lib/kctypes.c
 */
#include "../../rach2/sys/fs/krffs.h"
#include "fuzz.h"

void klog(int level, const char *subsystem, const char *format, ...)
{
    (void)level;
    (void)subsystem;
    (void)format;
}

void console_printf(const char *format, ...)
{
    (void)format;
}

void rtc_realtime(int64_t *seconds, int64_t *nanoseconds)
{
    *seconds = 1000000000;
    *nanoseconds = 0;
}

#define IMAGE_MAX (1u << 20)
static unsigned char image[IMAGE_MAX];
static uint32_t image_size;

static int memory_read(void *ctx, uint64_t offset, void *buffer, uint32_t size)
{
    (void)ctx;
    if (offset + size > image_size) {
        return -1;
    }
    __builtin_memcpy(buffer, image + offset, size);
    return 0;
}

static int memory_write(void *ctx, uint64_t offset, const void *buffer, uint32_t size)
{
    (void)ctx;
    if (offset + size > image_size) {
        return -1;
    }
    __builtin_memcpy(image + offset, buffer, size);
    return 0;
}

static int memory_sync(void *ctx)
{
    (void)ctx;
    return 0;
}

static const krffs_backend_t memory_backend = {memory_read, memory_write, memory_sync};

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

        /* One entry read: the walk answers 1 for an entry and 0 at the end,
           so the loop runs while it answers 1. */
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
