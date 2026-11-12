/**
 * Fuzz harness for the ext2 driver (sys/fs/ext2.c): the input is the
 * volume. The seed is a 64 KiB volume mke2fs made; the format carries no
 * checksum in this revision, so a mutation reaches the parser as it is.
 *
 * fuzz-deps: rach2/sys/fs/ext2xattr.c
 * fuzz-deps: rach2/sys/fs/ext2_alloc.c rach2/sys/fs/ext2_batch.c
 * fuzz-deps: rach2/sys/fs/ext2_check.c rach2/sys/fs/ext2_dir.c
 * fuzz-deps: rach2/sys/fs/ext2_file.c
 * fuzz-deps: rach2/sys/fs/ext2.c rach2/sys/fs/bcache.c rach2/sys/fs/fatcommon.c
 * fuzz-deps: rach2/sys/block/chain.c rach2/sys/fs/sleeplock.c
 * fuzz-deps: rach2/sys/lib/kstring.c rach2/sys/lib/kctypes.c
 * fuzz-deps: tests/host/compat/kheap_host.c tests/host/compat/blockmax_host.c
 * fuzz-deps: tests/host/compat/pressure_host.c tests/host/compat/atomic_host.c
 * fuzz-deps: tests/host/compat/bkl_held.c tests/host/compat/frames_host.c
 * fuzz-deps: tests/host/compat/mutex_none.c tests/host/compat/kirq_on.c
 * fuzz-deps: tests/host/compat/kirq_none.c tests/host/compat/fsize_none.c
 */
#include "../../rach2/sys/fs/ext2.h"
#include "fs_common.h"
#include "../../rach2/sys/fs/ext2xattr.h"
#include "fuzz.h"

/** 0: no input closes the mount, whose wait for its writers alone reads the
 * clock. */
uint64_t tick_count(void)
{
    return 0;
}

/** Nothing: there is no scheduler here. */
void sched_service_point(void)
{}

/** Return 0: no hold of a sleeping lock is timed here. */
uint32_t timer_now_us(void)
{
    return 0;
}

/** The service point, as the kernel's with interrupts on: the code under
 * test runs with them on here. */
void sched_give_way(void)
{
    sched_service_point();
}

/** Return 0: the cycle counter does not run here. */
uint64_t arch_cycles(void)
{
    return 0;
}

/** A cycle is a microsecond here. */
uint64_t timer_cycles_us(uint64_t cycles)
{
    return cycles;
}

/** The cycles here are the clock's microseconds, the same on every
 * processor. */
int timer_cycles_agree(void)
{
    return 1;
}

/** Return 1: every buffer the walk passes is present. */
int vm_resident(uintptr_t addr, uintptr_t len, int write)
{
    (void)addr;
    (void)len;
    (void)write;
    return 1;
}

/** Return 1: nothing the walk passes is ever absent. */
int vm_prefault_range(uintptr_t addr, uintptr_t len, int write)
{
    (void)addr;
    (void)len;
    (void)write;
    return 1;
}

/** A write-back error recorded for the syncs to report: ignored, since
 * no input syncs through the VFS. */
void vfs_wb_err_set(struct vfs_mount *mnt, int rc)
{
    (void)mnt;
    (void)rc;
}

/** Take `lock` with interrupts off: one thread, so nothing waits. */
uint32_t spin_lock_irqsave(struct spinlock *lock)
{
    lock->locked = 1;
    return 0;
}

/** Give `lock` back. */
void spin_unlock_irqrestore(struct spinlock *lock, uint32_t flags)
{
    (void)flags;
    lock->locked = 0;
}

/** Exercise the driver over the input, cut to `FS_IMAGE_MAX` bytes and
 * padded with zeros to a whole sector. */
int LLVMFuzzerTestOneInput(const unsigned char *data, unsigned long size)
{
    if (size > FS_IMAGE_MAX) {
        size = FS_IMAGE_MAX;
    }
    __builtin_memcpy(fs_image, data, size);
    __builtin_memset(fs_image + size, 0, FS_IMAGE_MAX - size);
    fs_image_bytes = (size + 511) & ~511ul;
    {
        unsigned char parsed[4096], replacement[4096], value[4096] = {0};
        struct vfs_xattr arg = {VFS_XATTR_SET, "DOSATTRIB", value, 1, 0};
        unsigned long used = size > sizeof(parsed) ? sizeof(parsed) : size;

        __builtin_memset(parsed, 0, sizeof(parsed));
        __builtin_memcpy(parsed, data, used);
        (void)ext2_xattr_check(parsed, sizeof(parsed));
        (void)ext2_xattr_read(parsed, sizeof(parsed), "DOSATTRIB", value, sizeof(value));
        (void)ext2_xattr_read(parsed, sizeof(parsed), 0, value, sizeof(value));
        (void)ext2_xattr_edit(parsed, replacement, sizeof(parsed), &arg);
    }
    fs_exercise(&vfs_ext2_type);
    return 0;
}
