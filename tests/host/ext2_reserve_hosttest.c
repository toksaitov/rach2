/**
 * Check ext2 range admission and per-piece reserved-block limits.
 *
 * host-deps: rach2/sys/lib/kstring.c rach2/sys/lib/kctypes.c
 *
 * The production writer uses a small memory-backed cache. The mount and inode
 * are constructed directly so the allocator can be tested without preflight.
 */
#include "fs/vfs.h"
#include "fs/ext2.c"

extern int memcmp(const void *a, const void *b, unsigned long size);
extern int printf(const char *format, ...);
extern void exit(int status);
extern void free(void *ptr);
extern void *malloc(unsigned long size);
extern void *calloc(unsigned long count, unsigned long size);

#define TEST_BLOCKS 64u
static uint8_t disk[TEST_BLOCKS][4096], saved[TEST_BLOCKS][4096];
static uint8_t storage[(EXT2_WB_DATA + EXT2_WB_META) * 4096];
static struct bbuf buffers[TEST_BLOCKS];
static struct ext2_mount volume;
static struct ext2_writeback batch;
static struct ext2_node file;
static struct vfs_mount mount;
static block_device_t device;
static uint8_t descriptors[32], payload[8192], result[8192];
static process_t process;
process_t *current_proc = &process;
static int failures, writes;

/** Record failed assertions. */
static void check(int ok, const char *what)
{
    if (!ok) {
        printf("ext2-reserve-hosttest: fail: %s\n", what);
        failures++;
    }
}

/** Fail on a production invariant violation. */
void k_panic(const char *what)
{
    printf("ext2-reserve-hosttest: panic: %s\n", what);
    exit(2);
}

/** Ignore production diagnostics. */
void klog(int level, const char *subsystem, const char *format, ...)
{
    (void)level;
    (void)subsystem;
    (void)format;
}

/** Supply a fixed timestamp. */
void rtc_realtime(int64_t *seconds, int64_t *nanoseconds)
{
    *seconds = 1;
    *nanoseconds = 0;
}

/** Retain the test's inode throughout each row. */
void kfree(void *ptr)
{
    check(ptr != &file, "writer retains the caller's inode reference");
    if (ptr != &file) {
        free(ptr);
    }
}

/** Supply allocations for linked driver paths. */
void *kmalloc(size_t size)
{
    return malloc(size);
}

/** Supply zeroed allocations for linked driver paths. */
void *kzalloc(size_t size)
{
    return calloc(1, size);
}

/** Supply the batch's buffer pages; these rows build their batch by hand. */
void *kalloc_pages(uint32_t count, uintptr_t *phys)
{
    if (phys) {
        *phys = 0;
    }
    return malloc(4096ul * count);
}

/** Give back a page `kalloc_pages` supplied. */
void kfree_pages(void *p, uint32_t count)
{
    (void)count;
    free(p);
}

/** Mount checking is outside these writer rows. */
int fat_walk_acquire(uint32_t count)
{
    (void)count;
    k_panic("unexpected mount check");
    return 0;
}

/** Reject an unexpected mount-check workspace query. */
uint32_t fat_walk_bytes(uint32_t count)
{
    (void)count;
    k_panic("unexpected mount check");
    return 0;
}

/** Reject an unexpected mount-check release. */
void fat_walk_release(void)
{
    k_panic("unexpected mount check");
}

/** Reject an unexpected mount-check update. */
int fat_walk_test_set(uint32_t block)
{
    (void)block;
    k_panic("unexpected mount check");
    return 0;
}

/** Reject an unexpected mount-check lookup. */
int fat_walk_marked(uint32_t block)
{
    (void)block;
    k_panic("unexpected mount check");
    return 0;
}

/** Model a recursive namespace lock. */
void vfs_ns_lock(struct vfs_mount *mnt)
{
    mnt->ns_holder = (int)process.pid;
    mnt->ns_depth++;
}

/** Check lock balance. */
void vfs_ns_unlock(struct vfs_mount *mnt)
{
    check(mnt->ns_depth > 0, "namespace lock is balanced");
    if (--mnt->ns_depth == 0) {
        mnt->ns_holder = -1;
    }
}

/** Return a cache block from the constructed volume. */
struct bbuf *bcache_get(block_device_t *dev, uint64_t block)
{
    check(dev == &device && block < TEST_BLOCKS, "cache access is in the volume");
    return block < TEST_BLOCKS ? &buffers[block] : NULL;
}

/** Supply a block that the writer overwrites completely. */
struct bbuf *bcache_get_blank(block_device_t *dev, uint64_t block)
{
    return bcache_get(dev, block);
}

/** No eviction occurs in this cache model. */
void bcache_release(struct bbuf *buffer)
{
    (void)buffer;
}

/** Count published cache writes. */
void bcache_dirty(struct bbuf *buffer)
{
    (void)buffer;
    writes++;
}

/** Count metadata writes outside the batch. */
void bcache_dirty_ordered(struct bbuf *buffer, int order)
{
    (void)order;
    bcache_dirty(buffer);
}

/** Complete a write-through request. */
int bcache_write(struct bbuf *buffer)
{
    bcache_dirty(buffer);
    return 0;
}

/** Complete cache synchronization. */
int bcache_sync(block_device_t *dev)
{
    (void)dev;
    return 0;
}

/** Complete device synchronization. */
int block_flush(block_device_t *dev)
{
    (void)dev;
    return 0;
}

/** Construct one group with the requested number of free blocks. */
static void reset(uint32_t available)
{
    uint32_t i;

    k_memset(disk, 0, sizeof(disk));
    k_memset(&volume, 0, sizeof(volume));
    k_memset(&file, 0, sizeof(file));
    k_memset(&mount, 0, sizeof(mount));
    k_memset(&batch, 0, sizeof(batch));
    k_memset(descriptors, 0, sizeof(descriptors));
    for (i = 0; i < TEST_BLOCKS; i++) {
        buffers[i].data = disk[i];
        buffers[i].bytes = 4096;
        if (i < TEST_BLOCKS - available) {
            disk[2][i / 8] |= (uint8_t)(1u << (i % 8));
        }
    }
    LE32_PUT(descriptors, 2);
    LE32_PUT(descriptors + 4, 3);
    LE32_PUT(descriptors + 8, 4);
    LE16_PUT(descriptors + 12, available);
    k_memcpy(disk[1], descriptors, sizeof(descriptors));
    volume.dev = &device;
    volume.mnt = &mount;
    volume.block_size = 4096;
    volume.block_shift = 12;
    volume.file_limit = (uint64_t)1 << 41;
    volume.blocks = volume.bpg = TEST_BLOCKS;
    volume.ipg = volume.inodes = 32;
    volume.groups = 1;
    volume.inode_size = 128;
    volume.gd = descriptors;
    volume.free_blocks = available;
    volume.r_blocks = 8;
    volume.writable = 1;
    volume.ro_compat = EXT2_RO_LARGE_FILE;
    volume.wb = &batch;
    for (i = 0; i < EXT2_WB_DATA + EXT2_WB_META; i++) {
        batch.slot[i] = storage + i * 4096;
    }
    mount.priv = &volume;
    file.ino = 12;
    file.mode = S_IFREG_ | 0600;
    file.refs = file.nlink = 1;
    process.euid = 1000;
    process.pid = 1;
    writes = 0;
    k_memset(payload, 0x6b, sizeof(payload));
}

/** Check exact admission on both sides of each pointer boundary. */
static void boundary_rows(void)
{
    static const struct {
        uint32_t first, needed;
    } rows[] = {{11, 3},      {1035, 5},           {2059, 5},
                {1049611, 7}, {1049612 + 1023, 6}, {1049612 + 1048575, 7}};
    unsigned i;

    for (i = 0; i < sizeof(rows) / sizeof(rows[0]); i++) {
        uint64_t offset = (uint64_t)rows[i].first * 4096;
        struct ext2_node before;
        int rc;

        reset(8 + rows[i].needed - 1);
        before = file;
        k_memcpy(saved, disk, sizeof(saved));
        rc = ext2_write(&mount, &file, payload, sizeof(payload), offset);
        check(rc == -ENOSPC && writes == 0 && volume.free_blocks == 8 + rows[i].needed - 1 &&
                  memcmp(&before, &file, sizeof(file)) == 0 &&
                  memcmp(saved, disk, sizeof(saved)) == 0 && batch.count == 0,
              "boundary shortage is refused before accepting bytes");
        check(volume.free_blocks >= volume.r_blocks,
              "unprivileged boundary write preserves reserved blocks");
        process.euid = 0;
        check(ext2_write(&mount, &file, payload, sizeof(payload), offset) == sizeof(payload) &&
                  volume.free_blocks == 7,
              "root may allocate the reserved boundary block");

        reset(8 + rows[i].needed);
        check(ext2_write(&mount, &file, payload, sizeof(payload), offset) == sizeof(payload) &&
                  volume.free_blocks == 8 && file.blocks == rows[i].needed * 8,
              "boundary exact quota counts each missing pointer once");
        check(read_data(&volume, &file, offset, result, sizeof(result)) == 0 &&
                  memcmp(result, payload, sizeof(result)) == 0,
              "accepted boundary bytes read back");
        check(ext2_write(&mount, &file, payload, sizeof(payload), offset) == sizeof(payload),
              "allocated overwrites need no free blocks");
    }
}

/** Check existing and pending pointer paths without charging them again. */
static void existing_rows(void)
{
    reset(20);
    check(ext2_write(&mount, &file, payload, 4096, (uint64_t)2059 * 4096) == 4096,
          "create the double-indirect root and one leaf");
    volume.r_blocks = (uint32_t)volume.free_blocks - 3;
    check(ext2_write(&mount, &file, payload, 8192, (uint64_t)2060 * 4096) == 8192 &&
              volume.free_blocks == volume.r_blocks,
          "existing root and missing leaf use exactly three blocks");

    reset(20);
    file.size = (uint64_t)2059 * 4096;
    check(ext2_write(&mount, &file, payload, 4096, file.size) == 4096 && batch.count == 1,
          "append retains a pending pointer path");
    volume.r_blocks = (uint32_t)volume.free_blocks - 2;
    check(ext2_write(&mount, &file, payload, 4096, file.size) == 4096 &&
              volume.free_blocks == volume.r_blocks && batch.count == 1,
          "pending root is counted once across append calls");
}

/** Check reservation quotas independently of the range preflight. */
static void reservation_rows(void)
{
    struct ext2_node before;
    uint32_t remaining;
    int metas, datas;

    reset(11);
    check(writeback_piece(&volume, &file, 0, payload, 4096) == 0, "accept an earlier piece");
    before = file;
    remaining = (uint32_t)volume.free_blocks;
    metas = batch.metas;
    datas = batch.datas;
    check(writeback_piece(&volume, &file, (uint64_t)1049612 * 4096, payload, 4096) == -ENOSPC,
          "piece quota includes uncommitted reservations");
    check(volume.free_blocks == remaining && memcmp(&before, &file, sizeof(file)) == 0 &&
              batch.metas == metas && batch.datas == datas && batch.count == 1 && writes == 0,
          "refused piece preserves the earlier pending piece");
    check(writeback_drain(&volume) == 0 && read_data(&volume, &file, 0, result, 4096) == 0 &&
              memcmp(result, payload, 4096) == 0,
          "earlier piece survives a refused reservation");

    reset(2);
    process.euid = 0;
    check(writeback_piece(&volume, &file, (uint64_t)1036 * 4096, payload, 4096) == -ENOSPC &&
              volume.free_blocks == 2 && batch.count == 0,
          "root cannot reserve more blocks than are free");
}

/** Run the focused allocation rows. */
int main(void)
{
    boundary_rows();
    existing_rows();
    reservation_rows();
    if (!failures) {
        printf("ext2-reserve-hosttest: result: pass: boundaries and reservation quotas\n");
    }
    return failures != 0;
}
