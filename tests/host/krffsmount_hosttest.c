/*
 * krffsmount_hosttest.c: the mounted KRFFS volume, rach2/sys/fs/krffsmount.c
 * over rach2/sys/fs/krffs.c, with the descriptor table of fd.c: relocation,
 * O_PATH pins, writeback errors, the order of the clean flag, and the error
 * a failed creation returns, a directory whose identifier another directory
 * has among them, and an fsync that writes one file.
 *
 * host-deps: tests/host/compat/vfs_name_none.c
 * host-deps: rach2/sys/fs/fd.c rach2/sys/fs/lock.c rach2/sys/lib/kstring.c
 * host-deps: rach2/sys/lib/kctypes.c
 * host-deps: tests/host/compat/spinlock_host.c tests/host/compat/kirq_none.c
 * host-deps: tests/host/compat/bkl_none.c tests/host/compat/rcu_none.c
 * host-deps: tests/host/compat/atomic_host.c
 * host-cflags: -Wno-pointer-to-int-cast -Wno-int-to-pointer-cast
 *
 * `vfs_ns_lock` runs the operation queued in `waiting` before it gives the
 * caller the lock, as another process would run while the caller slept.
 * `cached`, `device` and `medium` model the buffer cache, the device's write
 * cache and the bytes that survive a power loss.
 *
 * The fixture tests/host/fixtures/krffs_open.c includes this file with
 * `KRFFS_OPEN_TEST` defined, which leaves out the lock, close and wait
 * functions that macro guards.
 */
/* A bound on the repair's index that the many-names row passes with a
   volume that fits the fixture, which the repair then searches in passes. */
#define KRFFS_REPAIR_NAMES 8u
#include "fs/krffs.c"
#include "fs/krffsmount.c"

extern int printf(const char *format, ...);
extern void *calloc(unsigned long count, unsigned long size);
extern void free(void *ptr);

#define IMAGE_BYTES  65536u
#define IMAGE_BLOCKS (IMAGE_BYTES / BCACHE_BLOCK_BYTES)

static unsigned char cached[IMAGE_BYTES], device[IMAGE_BYTES], medium[IMAGE_BYTES];
static struct bbuf buffers[IMAGE_BLOCKS];
static struct vfs_mount mount;
static block_device_t disk;
static fd_table_t table;
static struct open_file *descs[3];
static void (*waiting)(void);
/* The faults a row sets: `sync_error` and `flush_error` fail every sync or
   flush, `fail_flush_at` the flush of that count, and `fail_after_move` the
   first sync after the moved file's old node is free, counted in
   `syncs_after_free`. `flush_hook` runs at every flush. */
static int failures, depth, sync_error, flush_error, sync_calls, flush_calls;
static int check_clean_order, clean_writes, reuse_fd, truncating;
static int fail_after_move, syncs_after_free, fail_flush_at;
static int refuse_block = -1;
static int reclaim_error, recovery_active;
static int refuse_skip; /* matching gets before the one to fail */
/* Blocks copied from the cache to the device cache, by any of the syncs. */
static int block_writes;
static void (*flush_hook)(void);
static struct krffs_open *close_at_unlink;
static void *original;
static char payload[8192];
struct percpu cpu_table[NR_CPUS];
process_t *proc_list;
const struct vfs_type vfs_krffs_type = {0};

/** Count and print a failed row. */
static void check(int ok, const char *what)
{
    if (!ok) {
        printf("krffsmount-hosttest: fail: %s\n", what);
        failures++;
    }
}

/** Allocate zeroed host storage. */
void *kzalloc(size_t size)
{
    return calloc(1, size);
}

/* Set to have `kmalloc` refuse, as a kernel short of memory does. */
static int kmalloc_refuses;

/** Allocate host storage, zeroed as `kzalloc` does, or NULL while
 * `kmalloc_refuses`. */
void *kmalloc(size_t size)
{
    return kmalloc_refuses ? NULL : kzalloc(size);
}

/** Free a host allocation. */
void kfree(void *ptr)
{
    free(ptr);
}

/** Ignore diagnostics from the core. */
void klog(int level, const char *subsystem, const char *format, ...)
{
    (void)level;
    (void)subsystem;
    (void)format;
}

/** Ignore the core's debug listing. */
void console_printf(const char *format, ...)
{
    (void)format;
}

/** Use a fixed timestamp. */
void rtc_realtime(int64_t *seconds, int64_t *nanoseconds)
{
    *seconds = 1000;
    *nanoseconds = 0;
}

#ifndef KRFFS_OPEN_TEST
/** Run the operation queued in `waiting`, once, then take the lock. */
void vfs_ns_lock(struct vfs_mount *mnt)
{
    void (*run)(void) = waiting;

    (void)mnt;
    waiting = NULL;
    if (run) {
        run();
    }
    depth++;
}

/** Require balanced mount locking. */
void vfs_ns_unlock(struct vfs_mount *mnt)
{
    (void)mnt;
    check(depth > 0, "balanced mount lock");
    depth--;
}

#endif

/**
 * Repoint the descriptions from `from` to `to` with fd.c's
 * `fd_ofd_repoint`, and require the mount lock held.
 */
void vfs_repoint(struct vfs_mount *mnt, const void *from, void *to)
{
    check(depth > 0 || mnt->ns.depth > 0, "relocation holds mount lock");
    fd_ofd_repoint(mnt, from, to);
}

/* How many times the volume reported a halt. */
static int halts;

/** Turn the halted mount read-only, as vfs.c turns it and its binds. */
void vfs_mount_halted(struct vfs_mount *base)
{
    halts++;
    base->flags |= VFS_MS_RDONLY;
}

#ifndef KRFFS_OPEN_TEST
/** Release the mounted open record at the description's last close. */
void vfs_desc_close(struct vfs_mount *mnt, void *node, void *handle)
{
    (void)node;
    kv_release(mnt, handle);
}

#endif

/** Initialize a queue that these rows never wait on. */
void wait_queue_init(wait_queue_t *wq)
{
    wq->head = NULL;
}

/** Ignore unused descriptor wakeups. */
void wq_wake_all(wait_queue_t *wq)
{
    (void)wq;
}

/** Count a failed row, since no row waits for a lock, and return -1. */
int wq_wait_event(wait_queue_t *wq, wq_cond_fn cond, void *arg)
{
    (void)wq;
    (void)cond;
    (void)arg;
    check(0, "unexpected lock wait");
    return -1;
}

#ifndef KRFFS_OPEN_TEST
/** Count a failed row, since no row waits on a description, and return 0. */
int wq_wait_event_nointr(wait_queue_t *wq, wq_cond_fn cond, void *arg)
{
    (void)wq;
    (void)cond;
    (void)arg;
    check(0, "unexpected description wait");
    return 0;
}

#endif

/** Return one model cache block. */
struct bbuf *bcache_get(block_device_t *dev, uint64_t blk)
{
    check(dev == &disk && blk < IMAGE_BLOCKS, "cache block in volume");
    if (reclaim_error && --reclaim_error == 0) {
        dev->flags |= BLOCK_WRITE_FAILED;
        if (recovery_active) {
            dev->flags |= BLOCK_WRITES_HALTED;
        }
    }
    if (refuse_block >= 0 && blk == (uint64_t)refuse_block && refuse_skip-- == 0) {
        refuse_skip = 0;
        refuse_block = -1;
        return NULL;
    }
    return blk < IMAGE_BLOCKS ? &buffers[blk] : NULL;
}

/** Complete a pending last close when its name leaves the cache. */
void bcache_release(struct bbuf *b)
{
    (void)b;
    if (close_at_unlink && !cached[close_at_unlink->offset + KRFFS_FIELD_NAME]) {
        struct krffs_open *o = close_at_unlink;

        close_at_unlink = NULL;
        kv_release(&mount, o);
    }
}

/** Record a pending cache write; a tag the block has stays. */
void bcache_dirty(struct bbuf *b)
{
    b->dirty = 1;
}

/** Record a pending cache write of a block of `owner` alone. */
void bcache_dirty_owned(struct bbuf *b, uint64_t owner)
{
    b->dirty = 1;
    b->owner = owner;
}

/** Copy block `i` to the device cache, clear its mark and tag, and count it
 * in `block_writes`. */
static void write_block(unsigned i)
{
    k_memcpy(device + i * BCACHE_BLOCK_BYTES, buffers[i].data, BCACHE_BLOCK_BYTES);
    buffers[i].dirty = 0;
    buffers[i].owner = 0;
    block_writes++;
}

/** Write the dirty blocks tagged `owner`, or every dirty block for 0, with
 * no flush; -EIO with nothing written under `sync_error`. */
int bcache_sync_owned(block_device_t *dev, uint64_t owner)
{
    unsigned i;

    check(dev == &disk, "owned sync of the volume's device");
    if (sync_error) {
        return -EIO;
    }
    for (i = 0; i < IMAGE_BLOCKS; i++) {
        if (buffers[i].dirty && (owner == 0 || buffers[i].owner == owner)) {
            write_block(i);
        }
    }
    return 0;
}

/** Write block `blk` if it is dirty, whatever its tag, with no flush; -EIO
 * under `sync_error`. */
int bcache_sync_block(block_device_t *dev, uint64_t blk)
{
    check(dev == &disk && blk < IMAGE_BLOCKS, "block sync in volume");
    if (sync_error) {
        return -EIO;
    }
    if (blk < IMAGE_BLOCKS && buffers[blk].dirty) {
        write_block((unsigned)blk);
    }
    return 0;
}

/* Set by `bcache_halt_writes`: the cache writes none of the device's blocks. */
static int writes_halted;

/** The core halted the volume: no sync writes a block after this. */
void bcache_halt_writes(block_device_t *dev)
{
    dev->flags |= BLOCK_WRITES_HALTED;
    writes_halted = 1;
}

/** Mark the interval in which a failed cache victim stops the device. */
void bcache_recovering(block_device_t *dev, int active)
{
    check(dev == &disk, "set recovery on the claimed device");
    recovery_active = active;
}

/**
 * Copy the dirty blocks to the device cache. Fails with -EIO, keeping the
 * blocks dirty, after a halt (`writes_halted`), under `sync_error` or at the
 * fault `fail_after_move` sets.
 * With `check_clean_order`, a write of block 0 with the dirty flag clear
 * requires the earlier data on the medium, and is counted in
 * `clean_writes`.
 */
int bcache_sync_unflushed(block_device_t *dev)
{
    unsigned i;

    (void)dev;
    sync_calls++;
    if (writes_halted || (dev->flags & BLOCK_WRITES_HALTED)) {
        return -EIO;
    }
    if (fail_after_move &&
        ((krffs_node_t *)(cached + offset_of(&mount, original)))->type == KRFFS_FREE_NODE &&
        ++syncs_after_free == 1) {
        fail_after_move = 0;
        return -EIO;
    }
    if (sync_error) {
        return -EIO;
    }
    for (i = 0; i < IMAGE_BLOCKS; i++) {
        if (buffers[i].dirty) {
            if (check_clean_order && i == 0 &&
                !(((krffs_node_t *)cached)->mode & KRFFS_MODE_DIRTY)) {
                check(medium[IMAGE_BYTES - 1] == 0x5a,
                      "earlier data reaches medium before clean flag");
                clean_writes++;
            }
            write_block(i);
        }
    }
    return 0;
}

/** A durable cache sync also flushes a write-through device on error. */
int bcache_sync(block_device_t *dev)
{
    int rc = bcache_sync_unflushed(dev);

    if (dev->flags & BLOCK_WRITE_THROUGH) {
        int flushed = bcache_flush(dev);

        return rc ? rc : flushed;
    }
    return rc;
}

/**
 * Run `flush_hook`, then copy the device cache to the medium; or, under
 * `flush_error` or at flush `fail_flush_at`, fail with -EIO and copy
 * nothing.
 */
int block_flush(block_device_t *dev)
{
    (void)dev;
    flush_calls++;
    if (flush_hook) {
        flush_hook();
    }
    if (flush_error || flush_calls == fail_flush_at) {
        return -EIO;
    }
    k_memcpy(medium, device, sizeof(medium));
    return 0;
}

/** The cache's flush, which records a failure against the mounts: here the
 * device's flush alone, since no row reads the record. */
int bcache_flush(block_device_t *dev)
{
    return block_flush(dev);
}

/** Discard a refused constructor's buffers without issuing device I/O. */
int bcache_drop(block_device_t *dev)
{
    unsigned i;
    int dirty = 0;

    check(dev == &disk, "discard the constructor's own device");
    for (i = 0; i < IMAGE_BLOCKS; i++) {
        dirty += buffers[i].dirty;
        buffers[i].dirty = 0;
        buffers[i].owner = 0;
    }
    k_memcpy(cached, device, sizeof(cached));
    writes_halted = 0;
    dev->flags &= ~(BLOCK_WRITE_FAILED | BLOCK_WRITES_HALTED);
    return dirty;
}

/**
 * Format a 64 KiB volume of a root and one free node, mount it through the
 * block backend of krffsmount.c, make a descriptor table, and clear the
 * faults and the counts.
 */
static void setup_on_phase(uint32_t phase)
{
    krffs_node_t *root = (krffs_node_t *)cached;
    krffs_node_t *space = (krffs_node_t *)(cached + sizeof(*root));
    unsigned i;

    k_memset(cached, 0, sizeof(cached));
    k_memset(&mount, 0, sizeof(mount));
    k_memset(&disk, 0, sizeof(disk));
    k_memset(buffers, 0, sizeof(buffers));
    depth = sync_error = flush_error = check_clean_order = clean_writes = 0;
    writes_halted = 0;
    waiting = NULL;
    flush_hook = NULL;
    fail_flush_at = 0;
    refuse_block = -1;
    refuse_skip = 0;
    fail_after_move = syncs_after_free = block_writes = 0;
    root->magic = space->magic = KRFFS_FILE_SYSTEM_MAGIC;
    root->type = KRFFS_ROOT_NODE;
    root->mode = 0755;
    root->size = sizeof(*root);
    root->previous_node_size = IMAGE_BYTES - sizeof(*root);
    space->type = KRFFS_FREE_NODE;
    space->size = IMAGE_BYTES - sizeof(*root);
    space->previous_node_size = sizeof(*root);
    for (i = 0; i < IMAGE_BLOCKS; i++) {
        buffers[i].data = cached + i * BCACHE_BLOCK_BYTES;
        buffers[i].bytes = BCACHE_BLOCK_BYTES;
    }
    k_memcpy(device, cached, sizeof(device));
    k_memcpy(medium, cached, sizeof(medium));
    disk.sector_size = BLOCK_SECTOR_SIZE;
    disk.sector_phase = phase;
    disk.sector_count = IMAGE_BYTES / BLOCK_SECTOR_SIZE;
    mount.type = &vfs_krffs_volume_type;
    check(krffs_volume_mount(&mount, &disk, 0, NULL) == 0, "mount fixture");
    check(fd_table_init(&table) == 0, "descriptor table");
    sync_calls = flush_calls = 0;
}

/** Set up an unshifted disk fixture. */
static void setup(void)
{
    setup_on_phase(0);
}

/** Close every description and release the mount. */
static void teardown(void)
{
    int i;

    for (i = 0; i < table.cap; i++) {
        if (fd_slot(&table, i)->ofd) {
            fd_free(&table, i);
        }
    }
    fd_table_free(&table);
    check(vol_of(&mount)->opens == NULL, "last close frees open records");
    check(depth == 0, "all mount locks released");
    kv_unmount(&mount);
}

/** Create a fixture file while holding the namespace lock. */
static void *create(const char *name)
{
    void *node = NULL;

    vfs_ns_lock(&mount);
    check(kv_create(&mount, mount.root, name, 0644, &node) == 0, "create fixture file");
    vfs_ns_unlock(&mount);
    return node;
}

/** Open a separate description of a fixture node. */
static int open_node(void *node)
{
    int fd = fd_alloc(&table);
    struct open_file *ofd;

    check(fd >= 0, "allocate fixture description");
    ofd = fd_slot(&table, fd)->ofd;
    ofd->vmnt = &mount;
    ofd->vnode = node;
    check(kv_open(&mount, node, 2, &ofd->fhandle) == 0, "open fixture node");
    return fd;
}

/**
 * Write 4096 bytes to the target, which moves it from its first offset, A,
 * to a new one, B; create a file that takes offset A and open it; and create
 * a file after the target, so that it cannot grow in place again.
 */
static void first_move(void)
{
    void *reused;

    k_memset(payload, 'B', 4096);
    check(kv_write(&mount, descs[0]->fhandle, payload, 4096, 0) == 4096, "first relocation");
    check(descs[0]->vnode != original, "first relocation changes offset");
    reused = create("reuse");
    check(reused == original, "old offset reused by another file");
    reuse_fd = open_node(reused);
    check(kv_write(&mount, fd_slot(&table, reuse_fd)->ofd->fhandle, "R", 1, 0) == 1,
          "write reused node");
    create("block-b");
}

/**
 * Queue `first_move` to run when this call takes the lock, then write 8192
 * bytes or, with `truncating`, truncate to 8192, through the second
 * description, which still has offset A.
 */
static void second_move(void)
{
    int rc;

    waiting = first_move;
    if (truncating) {
        rc = kv_truncate(&mount, descs[1]->fhandle, sizeof(payload));
        check(rc == 0, "waiting truncation succeeds");
    } else {
        k_memset(payload, 'C', sizeof(payload));
        rc = kv_write(&mount, descs[1]->fhandle, payload, sizeof(payload), 0);
        check(rc == (int)sizeof(payload), "waiting write succeeds");
    }
}

/**
 * Check that the descriptions of a file follow it through two moves while
 * another file takes its old offset. A read through the third description
 * waits while `second_move` and `first_move` run, and must read the file's
 * bytes at its new place, as must a saved handle; every description is
 * repointed, the length is the second move's, and the file at the old
 * offset keeps its own bytes. With `truncate`, the second move is a
 * truncate.
 */
static void relocation(int truncate)
{
    struct vfs_stat st;
    char byte = 0;
    void *current, *held;
    int i;

    setup();
    original = create("target");
    for (i = 0; i < 3; i++) {
        descs[i] = fd_slot(&table, open_node(original))->ofd;
    }
    check(kv_write(&mount, descs[0]->fhandle, "A", 1, 0) == 1, "seed target");
    create("block-a");
    held = descs[2]->fhandle;
    truncating = truncate;
    waiting = second_move;
    check(kv_read(&mount, descs[2]->fhandle, &byte, 1, 0) == 1 && byte == 'B',
          "waiting read follows two moves despite offset reuse");
    check(kv_read(&mount, held, &byte, 1, 0) == 1 && byte == 'B',
          "saved open handle survives successive calls and relocations");
    check(kv_lookup(&mount, mount.root, "target", &current) == 0, "lookup moved target");
    for (i = 0; i < 3; i++) {
        check(descs[i]->vnode == current && handle_of(follow_moves(descs[i]->fhandle)) == current,
              "every description repointed from resolved offset");
    }
    check(kv_getattr(&mount, current, &st) == 0 && st.size == sizeof(payload),
          "second relocation updates target length");
    check(kv_read(&mount, fd_slot(&table, reuse_fd)->ofd->fhandle, &byte, 1, 0) == 1 && byte == 'R',
          "reused offset retains its own bytes");
    teardown();
}

/** Read a directory through its open record and close that record. */
static void directory(void)
{
    struct vfs_dirent ent;
    uint32_t cursor = 0;
    int fd;

    setup();
    create("entry");
    fd = open_node(mount.root);
    check(kv_readdir(&mount, fd_slot(&table, fd)->ofd->fhandle, &cursor, &ent) == 1 &&
              !__builtin_memcmp(ent.name, "entry", 6),
          "directory read resolves its open handle");
    check(kv_readdir(&mount, fd_slot(&table, fd)->ofd->fhandle, &cursor, &ent) == 0,
          "directory read reaches end");
    teardown();
}

/**
 * Pin a file for an O_PATH description, which opens nothing, unlink it and
 * create another: the pinned node keeps its offset and its attributes, and
 * the last close frees the record and the node.
 */
static void pinned(void)
{
    struct open_file *ofd;
    struct vfs_stat st;
    void *node;
    int fd;

    setup();
    fd = open_node(create("kept"));
    check(kv_write(&mount, fd_slot(&table, fd)->ofd->fhandle, "K", 1, 0) == 1, "seed pinned file");
    fd_free(&table, fd);
    check(kv_lookup(&mount, mount.root, "kept", &node) == 0, "look up pinned file");
    fd = fd_alloc(&table);
    ofd = fd_slot(&table, fd)->ofd;
    ofd->vmnt = &mount;
    ofd->vnode = node;
    vfs_ns_lock(&mount);
    check(mount.type->ops->pin(&mount, node, &ofd->pin) == 0, "pin the looked-up node");
    check(kv_unlink(&mount, mount.root, "kept") == 0, "unlink pinned file");
    vfs_ns_unlock(&mount);
    check(create("other") != node, "pinned node keeps its offset");
    check(kv_getattr(&mount, node, &st) == 0 && st.size == 1, "pinned node keeps its attributes");
    fd_free(&table, fd);
    check(vol_of(&mount)->opens == NULL, "last pin frees the record");
    check(create("again") == node, "last pin frees the unlinked node");
    teardown();
}

/**
 * Fail the first sync after a grow has moved the target and stored its old
 * node's type as free, the removal's own: the operation must return `EIO`,
 * and the file must stay at its new place, which holds the bytes, with every
 * description repointed there. With `truncate`, the grow is a truncate.
 */
static void failed_relocation(int truncate)
{
    void *current;
    int i, rc;

    setup();
    original = create("target");
    for (i = 0; i < 3; i++) {
        descs[i] = fd_slot(&table, open_node(original))->ofd;
    }
    check(kv_write(&mount, descs[0]->fhandle, "A", 1, 0) == 1, "seed failed relocation");
    create("block-a");
    fail_after_move = 1;
    k_memset(payload, 'F', sizeof(payload));
    rc = truncate ? kv_truncate(&mount, descs[0]->fhandle, sizeof(payload))
                  : kv_write(&mount, descs[0]->fhandle, payload, sizeof(payload), 0);
    check(rc == -EIO && syncs_after_free == 1, "the removal's sync fails after relocation");
    check(kv_lookup(&mount, mount.root, "target", &current) == 0 && current != original,
          "failed operation retains relocated node");
    {
        char byte = 0;

        check(kv_read(&mount, descs[1]->fhandle, &byte, 1, sizeof(payload) - 1) == 1 &&
                  byte == (truncate ? 0 : 'F'),
              "the relocated node holds what the operation wrote");
    }
    for (i = 0; i < 3; i++) {
        check(descs[i]->vnode == current && handle_of(follow_moves(descs[i]->fhandle)) == current,
              "failed relocation repoints every description");
    }
    teardown();
}

/**
 * Check the unmount's order of writes in four modes. A clean unmount (mode
 * 0) flushes before and after it clears the dirty flag, and the data is on
 * the medium before the flag is. A flush that fails before the clear (1)
 * returns `EIO` and leaves the flag set. On a read-only mount, an fsync (2)
 * or a sync (3) whose writeback fails returns `EIO` and keeps the block
 * dirty, a retry writes it, and the unmount leaves the dirty flag.
 */
static void durability(void)
{
    int mode;

    for (mode = 0; mode < 4; mode++) {
        setup();
        cached[IMAGE_BYTES - 1] = 0x5a;
        buffers[IMAGE_BLOCKS - 1].dirty = 1;
        check_clean_order = 1;
        if (mode == 1) {
            flush_error = 1;
            check(kv_prepare_unmount(&mount) == -EIO, "unmount returns pre-clear flush error");
            check(krffs_is_dirty(&vol_of(&mount)->fs), "failed pre-clear flush retains dirty flag");
            check(clean_writes == 0, "failed pre-clear flush never writes clean flag");
        } else if (mode == 2 || mode == 3) {
            mount.flags |= VFS_MS_RDONLY;
            sync_error = 1;
            check((mode == 2 ? kv_fsync(&mount, NULL) : kv_sync(&mount)) == -EIO,
                  "read-only sync reports pending write failure");
            check(sync_calls == 1 && buffers[IMAGE_BLOCKS - 1].dirty,
                  "read-only sync attempts dirty writeback");
            sync_error = 0;
            check((mode == 2 ? kv_fsync(&mount, NULL) : kv_sync(&mount)) == 0,
                  "read-only sync retries successfully");
            check(!buffers[IMAGE_BLOCKS - 1].dirty && device[IMAGE_BYTES - 1] == 0x5a,
                  "read-only sync writes pending bytes");
            check(kv_prepare_unmount(&mount) == 0 && krffs_is_dirty(&vol_of(&mount)->fs),
                  "read-only unmount retains dirty flag");
        } else {
            check(kv_prepare_unmount(&mount) == 0, "clean unmount succeeds");
            check(flush_calls == 2 && clean_writes == 1, "flush before and after clear");
            check(!(((krffs_node_t *)medium)->mode & KRFFS_MODE_DIRTY),
                  "clean flag reaches medium");
        }
        teardown();
    }
}

/**
 * Fail the unmount's second flush, the one after the clean flag is written,
 * with the data already durable. The unmount must return `EIO` and make the
 * mount read-only; a read still works, a write, a truncate and a create are
 * `EROFS`, and a sync can still finish the writeback.
 */
static void failed_clean_flush(void)
{
    void *node, *handle;
    char byte;
    int fd;

    setup();
    node = create("retained");
    fd = open_node(node);
    handle = fd_slot(&table, fd)->ofd->fhandle;
    check(kv_write(&mount, handle, "D", 1, 0) == 1, "seed clean-flush fixture");
    fd_free(&table, fd);
    cached[IMAGE_BYTES - 1] = 0x5a;
    buffers[IMAGE_BLOCKS - 1].dirty = 1;
    check_clean_order = 1;
    flush_calls = 0;
    fail_flush_at = 2;
    check(kv_prepare_unmount(&mount) == -EIO, "unmount returns clean-flag flush error");
    check(flush_calls == 2 && clean_writes == 1 && medium[IMAGE_BYTES - 1] == 0x5a,
          "second flush fails after durable data and clean-flag write");
    check(!(((krffs_node_t *)device)->mode & KRFFS_MODE_DIRTY),
          "failed clean-flag flush leaves device cache clean");
    fail_flush_at = 0;
    check((mount.flags & VFS_MS_RDONLY) != 0, "failed clean-flag flush makes mount read-only");
    fd = open_node(node);
    handle = fd_slot(&table, fd)->ofd->fhandle;
    check(kv_read(&mount, handle, &byte, 1, 0) == 1 && byte == 'D',
          "read after failed clean-flag flush succeeds");
    check(kv_write(&mount, handle, "X", 1, 0) == -EROFS,
          "write after failed clean-flag flush is refused");
    check(kv_truncate(&mount, handle, 0) == -EROFS,
          "truncate after failed clean-flag flush is refused");
    vfs_ns_lock(&mount);
    check(kv_create(&mount, mount.root, "later", 0644, &node) == -EROFS,
          "create after failed clean-flag flush is refused");
    vfs_ns_unlock(&mount);
    check(kv_sync(&mount) == 0 && block_flush(&disk) == 0,
          "read-only mount can finish writeback after failed unmount");
    teardown();
}

/**
 * A creation tells a failing volume from a full one: a sync that fails
 * makes it `EIO`, and a volume a file has grown to fill makes it `ENOSPC`.
 */
static void creation_errors(void)
{
    void *node = NULL;
    int fd;

    setup();
    sync_error = 1;
    vfs_ns_lock(&mount);
    check(kv_mknod(&mount, mount.root, "fifo", KRFFS_S_IFIFO | 0600, 0, &node) == -EIO,
          "a creation whose sync the medium refuses is EIO");
    vfs_ns_unlock(&mount);
    sync_error = 0;
    fd = open_node(create("big"));
    check(kv_truncate(&mount, fd_slot(&table, fd)->ofd->fhandle,
                      IMAGE_BYTES - 2 * sizeof(krffs_node_t) - 100) == 0,
          "a file grows to fill the volume");
    vfs_ns_lock(&mount);
    check(kv_create(&mount, mount.root, "more", 0644, &node) == -ENOSPC,
          "a creation on a full volume is ENOSPC");
    vfs_ns_unlock(&mount);
    teardown();
}

/**
 * A node whose owner falls across a 4 KiB block is made with its maker's
 * ids: a spacer file places a FIFO at offset 3826, whose uid starts at byte
 * 4095, and the caller of uid and gid 1000 owns it once it has a name. A
 * chown that would change bytes on both sides of the block is `EIO` and
 * leaves the owner; one that changes only the byte before it is stored.
 */
static void owner_across(void)
{
    static process_t maker;
    struct vfs_stat st;
    void *node = NULL;
    int fd;

    setup();
    fd = open_node(create("spacer"));
    check(kv_truncate(&mount, fd_slot(&table, fd)->ofd->fhandle, 3826 - 2 * sizeof(krffs_node_t)) ==
              0,
          "a spacer places the next node");
    maker.euid = maker.egid = 1000;
    current_proc = &maker;
    vfs_ns_lock(&mount);
    check(kv_mknod(&mount, mount.root, "fifo", KRFFS_S_IFIFO | 0600, 0, &node) == 0 &&
              offset_of(&mount, node) == 3826,
          "a FIFO is made across the block");
    vfs_ns_unlock(&mount);
    current_proc = NULL;
    check(kv_getattr(&mount, node, &st) == 0 && st.uid == 1000 && st.gid == 1000 &&
              (st.mode & 07777) == 0600,
          "a node made across a block has its maker's owner");
    st.uid = 0x01020304u;
    check(kv_setattr(&mount, node, VFS_SET_UID, &st) == -EIO,
          "a chown the block boundary refuses is EIO");
    check(kv_getattr(&mount, node, &st) == 0 && st.uid == 1000, "and the owner is kept");
    st.uid = 1001;
    check(kv_setattr(&mount, node, VFS_SET_UID, &st) == 0 && kv_getattr(&mount, node, &st) == 0 &&
              st.uid == 1001,
          "a chown of the byte before the boundary is stored");
    teardown();
}

/**
 * A node made in a set-group-ID directory has the directory's group, as
 * Linux's `inode_init_owner` gives it, and a directory made there takes the
 * bit too; in a directory without the bit a node has its maker's group.
 */
static void setgid_group(void)
{
    static process_t maker;
    struct vfs_stat st;
    void *dir = NULL, *node = NULL;

    setup();
    maker.euid = maker.egid = 1000;
    current_proc = &maker;
    vfs_ns_lock(&mount);
    check(kv_mkdir(&mount, mount.root, "sg", 0775) == 0 &&
              kv_lookup(&mount, mount.root, "sg", &dir) == 0,
          "a directory is made");
    vfs_ns_unlock(&mount);
    st.gid = 4242;
    st.mode = 02775;
    check(kv_setattr(&mount, dir, VFS_SET_GID, &st) == 0 &&
              kv_setattr(&mount, dir, VFS_SET_MODE, &st) == 0,
          "the directory takes group 4242 and the set-group-ID bit");
    vfs_ns_lock(&mount);
    check(kv_create(&mount, dir, "f", 0644, &node) == 0, "a file is made in it");
    vfs_ns_unlock(&mount);
    check(kv_getattr(&mount, node, &st) == 0 && st.uid == 1000 && st.gid == 4242 &&
              !(st.mode & 02000u),
          "a file made in a set-group-ID directory has its group");
    vfs_ns_lock(&mount);
    check(kv_mkdir(&mount, dir, "d", 0755) == 0 && kv_lookup(&mount, dir, "d", &node) == 0,
          "a directory is made in it");
    vfs_ns_unlock(&mount);
    check(kv_getattr(&mount, node, &st) == 0 && st.gid == 4242 && (st.mode & 02000u),
          "a directory made in a set-group-ID directory has its group and the bit");
    vfs_ns_lock(&mount);
    check(kv_create(&mount, mount.root, "plain", 0644, &node) == 0, "a file is made in the root");
    vfs_ns_unlock(&mount);
    check(kv_getattr(&mount, node, &st) == 0 && st.gid == 1000,
          "a file made elsewhere has its maker's group");
    current_proc = NULL;
    teardown();
}

/**
 * A directory whose identifier another directory has is `EINVAL`. `bC` and
 * `cb` hash alike, so the second in one directory would share the first
 * one's children, and the core fails it with `KRFFS_ETAKEN`, which the
 * volume reports as a name the file system cannot hold, not as a full or a
 * failing volume.
 */
static void taken_identifier(void)
{
    setup();
    vfs_ns_lock(&mount);
    check(kv_mkdir(&mount, mount.root, "bC", 0755) == 0, "a directory is made");
    check(kv_mkdir(&mount, mount.root, "cb", 0755) == -EINVAL,
          "a directory whose identifier another directory has is EINVAL");
    vfs_ns_unlock(&mount);
    teardown();
}

/**
 * Remove the directory `name` of the volume's root under the mount's lock,
 * as `do_rmdir` calls the type.
 */
static int remove_dir(const char *name)
{
    int rc;

    vfs_ns_lock(&mount);
    rc = kv_rmdir(&mount, mount.root, name);
    vfs_ns_unlock(&mount);
    return rc;
}

/**
 * A directory that is a process's working directory stays, with EBUSY. The
 * process record keeps the directory by the mount and the node's handle and
 * takes no hold, since the volume counts none, so a directory removed under
 * it would give its place to the next node made while the record still
 * points to it: once the process has left, the rmdir removes the directory and
 * the next node made takes that place.
 */
static void working_directory(void)
{
    static process_t walker;
    static struct fs_struct walker_fs;
    void *dir;

    setup();
    vfs_ns_lock(&mount);
    check(kv_mkdir(&mount, mount.root, "work", 0755) == 0, "a directory is made");
    vfs_ns_unlock(&mount);
    check(kv_lookup(&mount, mount.root, "work", &dir) == 0, "look up the directory");
    create("after");
    walker.fs = &walker_fs;
    walker_fs.cwd_mnt = &mount;
    walker_fs.cwd_node = dir;
    proc_list = &walker;
    check(remove_dir("work") == -EBUSY, "an rmdir of a process's working directory is EBUSY");
    check(create("next") != dir, "a working directory keeps its place");
    walker_fs.cwd_mnt = NULL;
    walker_fs.cwd_node = NULL;
    check(remove_dir("work") == 0, "an rmdir of the directory the process left removes it");
    check(create("again") == dir, "the next node made takes the removed directory's place");
    proc_list = NULL;
    teardown();
}

/** Rename `from` to `to` in the volume's root under the mount's lock, as
 * `do_rename` calls the type. */
static int rename_name(const char *from, const char *to)
{
    int rc;

    vfs_ns_lock(&mount);
    rc = kv_rename(&mount, mount.root, from, mount.root, to);
    vfs_ns_unlock(&mount);
    return rc;
}

/**
 * The refusals of unlink, rename and rmdir are the boot root's: a missing
 * name is ENOENT, an unlink of a directory EISDIR, a directory source EPERM,
 * a directory destination EISDIR and an rmdir of a file ENOTDIR. A rename of
 * an open file to its own name leaves it named, where taking the name off a
 * replaced file that is open would take it off the file that is moved.
 */
static void refusals(void)
{
    void *file;
    void *found;
    void *handle;

    setup();
    vfs_ns_lock(&mount);
    check(kv_mkdir(&mount, mount.root, "dir", 0755) == 0, "a directory is made");
    vfs_ns_unlock(&mount);
    file = create("file");
    vfs_ns_lock(&mount);
    check(kv_unlink(&mount, mount.root, "absent") == -ENOENT,
          "an unlink of a missing name is ENOENT");
    check(kv_unlink(&mount, mount.root, "dir") == -EISDIR, "an unlink of a directory is EISDIR");
    vfs_ns_unlock(&mount);
    check(remove_dir("absent") == -ENOENT, "an rmdir of a missing name is ENOENT");
    check(remove_dir("file") == -ENOTDIR, "an rmdir of a file is ENOTDIR");
    check(rename_name("absent", "new") == -ENOENT, "a rename of a missing name is ENOENT");
    check(rename_name("dir", "new") == -EPERM, "a rename of a directory is EPERM");
    check(rename_name("file", "dir") == -EISDIR, "a rename over a directory is EISDIR");
    check(kv_open(&mount, file, 2, &handle) == 0, "open the file");
    check(rename_name("file", "file") == 0, "a rename of an open file to its own name is 0");
    check(kv_lookup(&mount, mount.root, "file", &found) == 0 && found == file,
          "and the file keeps its name");
    kv_release(&mount, handle);
    teardown();
}

/**
 * A rename over an open file that the core will refuse, here on an
 * unsettled mount whose barrier fails, answers EIO with the destination
 * still named, since the core's checks run before its name is taken off.
 */
static void refused_rename_keeps_destination(void)
{
    void *found;
    void *handle;

    setup();
    create("src");
    check(kv_open(&mount, create("dst"), 2, &handle) == 0, "refused rename: open the destination");
    vol_of(&mount)->fs.unsettled = 1;
    sync_error = 1;
    check(rename_name("src", "dst") == -EIO, "a rename the core refuses answers EIO");
    sync_error = 0;
    vfs_ns_lock(&mount);
    check(kv_lookup(&mount, mount.root, "dst", &found) == 0 &&
              kv_lookup(&mount, mount.root, "src", &found) == 0,
          "and both names are still there");
    vfs_ns_unlock(&mount);
    kv_release(&mount, handle);
    teardown();
}

/**
 * A mount the core halted, whose moved file's copy could not be withdrawn,
 * takes no change through the adapter until the next mount: an unlink of an
 * open file and a rename answer EROFS, and an fsync EIO, since the copy may
 * still be on the medium beside the file; and the halt stops the cache's
 * writes of the volume, so that a sync answers EIO.
 */
static void halted_refusals(void)
{
    void *file;
    void *handle;
    void *found;

    setup();
    file = create("file");
    check(kv_open(&mount, file, 2, &handle) == 0, "halted: open the file");
    vol_of(&mount)->fs.halted = 1;
    vfs_ns_lock(&mount);
    check(kv_unlink(&mount, mount.root, "file") == -EROFS,
          "a halted mount refuses an unlink of an open file");
    vfs_ns_unlock(&mount);
    check(rename_name("file", "other") == -EROFS, "and a rename");
    check(kv_fsync(&mount, handle) == -EIO, "and answers EIO to an fsync");
    vfs_ns_lock(&mount);
    check(kv_mknod(&mount, mount.root, "dev", 0020644, 0, &found) == -EROFS,
          "and answers EROFS to a device node before EPERM");
    vfs_ns_unlock(&mount);
    halts = 0;
    vol_of(&mount)->fs.on_halt(&vol_of(&mount)->fs);
    check(halts == 1 && (mount.flags & VFS_MS_RDONLY) && writes_halted,
          "a halt turns the mounts that show the volume read-only and halts its writes");
    check(kv_sync(&mount) == -EIO, "and the volume writes nothing back");
    mount.flags &= ~(uint32_t)VFS_MS_RDONLY;
    vol_of(&mount)->fs.halted = 0;
    writes_halted = 0;
    kv_release(&mount, handle);
    teardown();
}

/**
 * The volume takes its device's phase for its sector edges, and gives a
 * loop device over one of its files the phase of the file's first data
 * byte: the device's, plus the node's offset and its header.
 */
static void phase_reported(void)
{
    void *file;
    void *handle;
    uint64_t at;

    setup_on_phase(100);
    file = create("file");
    check(kv_open(&mount, file, 2, &handle) == 0, "phase: open the file");
    at = ((struct krffs_open *)handle)->offset;
    check(vol_of(&mount)->fs.sector_phase == 100, "the volume takes its device's phase");
    check(krffs_volume_ops.phase(&mount, file, handle) ==
              (uint32_t)((100 + at + sizeof(krffs_node_t)) % 512),
          "and a file's phase adds the node's offset and its header");
    kv_release(&mount, handle);
    teardown();
}

/** The cache block that holds byte `at` of the volume. */
static unsigned block_of(uint64_t at)
{
    return (unsigned)(at / BCACHE_BLOCK_BYTES);
}

/**
 * An fsync writes one file. `one` and `other` each fill blocks of their own;
 * a chmod dirties `other`'s header and eight of its data blocks are left
 * dirty as a neighbour's writeback, and a write to `one` that changes no
 * length leaves its header dirty after the core's sync point. The fsync of
 * `one` writes its header block alone and leaves every block of `other`
 * dirty; the sync of the volume then writes them.
 */
static void owned_fsync(void)
{
    struct vfs_stat st;
    void *one, *other;
    unsigned i, first;
    int fd_one, fd_other;

    setup();
    one = create("one");
    fd_one = open_node(one);
    k_memset(payload, 'O', sizeof(payload));
    check(kv_write(&mount, fd_slot(&table, fd_one)->ofd->fhandle, payload, 4096, 0) == 4096,
          "fsync row: seed one");
    other = create("other");
    fd_other = open_node(other);
    for (i = 0; i < 4; i++) {
        check(kv_write(&mount, fd_slot(&table, fd_other)->ofd->fhandle, payload, sizeof(payload),
                       i * sizeof(payload)) == (int)sizeof(payload),
              "fsync row: seed other");
    }
    one = fd_slot(&table, fd_one)->ofd->vnode;
    other = fd_slot(&table, fd_other)->ofd->vnode;
    check(block_of(offset_of(&mount, one) + sizeof(krffs_node_t) - 1) <
              block_of(offset_of(&mount, other)),
          "fsync row: one's header shares no block with other");
    first = block_of(offset_of(&mount, other) + sizeof(krffs_node_t)) + 1;
    check(kv_write(&mount, fd_slot(&table, fd_one)->ofd->fhandle, "o", 1, 0) == 1,
          "fsync row: overwrite one");
    st.mode = 0600;
    check(kv_setattr(&mount, other, VFS_SET_MODE, &st) == 0, "fsync row: chmod other");
    for (i = first; i < first + 8; i++) {
        buffers[i].dirty = 1;
    }
    check(buffers[block_of(offset_of(&mount, one))].dirty, "write leaves one's header dirty");
    block_writes = 0;
    check(kv_fsync(&mount, fd_slot(&table, fd_one)->ofd->fhandle) == 0, "fsync of one succeeds");
    check(block_writes == 1 && !buffers[block_of(offset_of(&mount, one))].dirty,
          "fsync of one writes its header block alone");
    check(buffers[block_of(offset_of(&mount, other))].dirty, "fsync of one leaves other's header");
    for (i = first; i < first + 8; i++) {
        check(buffers[i].dirty, "fsync of one leaves other's data blocks");
    }
    block_writes = 0;
    check(kv_sync(&mount) == 0 && block_writes == 9, "sync of the volume writes the rest");
    teardown();
}

/**
 * The tag follows the file. `target` moves when it grows past `wall`, a
 * filler takes its old place, and `tail` is made after it. A shrink then
 * leaves the backward link of `tail` dirty, tagged by the target's open
 * record. The fsync of `target` through the description it had before the
 * move writes that block and its new header, and leaves the header of
 * `bystander`, dirtied by a chmod, in a block of its own.
 */
static void owned_across_move(void)
{
    struct vfs_stat st;
    void *bystander;
    uint64_t at, link;
    int fd, fd_tail, fd_fill;

    setup();
    bystander = create("bystander");
    fd = open_node(create("target"));
    k_memset(payload, 'M', sizeof(payload));
    check(kv_write(&mount, fd_slot(&table, fd)->ofd->fhandle, payload, 4000, 0) == 4000,
          "move row: seed target");
    create("wall");
    check(kv_write(&mount, fd_slot(&table, fd)->ofd->fhandle, payload, 4096, 2 * 4096) == 4096,
          "move row: grow target to 3 blocks");
    at = offset_of(&mount, fd_slot(&table, fd)->ofd->vnode);
    check(at > 4096, "move row: target moved past the first block");
    fd_fill = open_node(create("filler"));
    check(kv_truncate(&mount, fd_slot(&table, fd_fill)->ofd->fhandle, 4000) == 0,
          "move row: filler takes the old place");
    fd_tail = open_node(create("tail"));
    link = offset_of(&mount, fd_slot(&table, fd_tail)->ofd->vnode);
    check(link == at + sizeof(krffs_node_t) + 3 * 4096, "move row: tail follows target");
    check(kv_truncate(&mount, fd_slot(&table, fd)->ofd->fhandle, 100) == 0 &&
              offset_of(&mount, fd_slot(&table, fd)->ofd->vnode) == at,
          "move row: shrink in place");
    check(buffers[block_of(link)].dirty &&
              buffers[block_of(link)].owner == owner_of(fd_slot(&table, fd)->ofd->fhandle),
          "shrink tags tail's backward link with target's record");
    st.mode = 0600;
    check(kv_setattr(&mount, bystander, VFS_SET_MODE, &st) == 0, "move row: chmod bystander");
    check(block_of(offset_of(&mount, bystander)) != block_of(at) &&
              block_of(offset_of(&mount, bystander)) != block_of(link),
          "move row: bystander's header is in a block of its own");
    check(kv_fsync(&mount, fd_slot(&table, fd)->ofd->fhandle) == 0, "move row: fsync of target");
    check(!buffers[block_of(at)].dirty && !buffers[block_of(link)].dirty,
          "fsync after a move writes the new header and the tagged link");
    check(buffers[block_of(offset_of(&mount, bystander))].dirty,
          "fsync after a move leaves an unrelated header");
    teardown();
}

/**
 * The link a shrink leaves dirty is written by its offset. `a` shrinks and
 * leaves the backward link of `b`, the next file, dirty under `a`'s record.
 * Then the tag is lost: with `reopen` 0, a truncate of `b` to the size it
 * has, which resizes nothing and reaches no sync point, retags the block as
 * `b`'s; with `reopen` 1, `a` is closed and opened again, under a new record
 * while `other`, made before the shrink, since a creation reaches a sync
 * point, holds the old one's memory. The fsync of `a` writes the
 * link all the same.
 */
static void shrink_link(int reopen)
{
    void *a, *other;
    uint64_t at, link;
    struct krffs_open *was;
    int fd_a, fd_b;

    setup();
    a = create("a");
    fd_a = open_node(a);
    k_memset(payload, 'S', sizeof(payload));
    check(kv_write(&mount, fd_slot(&table, fd_a)->ofd->fhandle, payload, 4000, 0) == 4000,
          "link row: seed a");
    fd_b = open_node(create("b"));
    other = create("other");
    at = offset_of(&mount, fd_slot(&table, fd_a)->ofd->vnode);
    link = offset_of(&mount, fd_slot(&table, fd_b)->ofd->vnode) + KRFFS_FIELD_PREVIOUS;
    check(link - KRFFS_FIELD_PREVIOUS == at + sizeof(krffs_node_t) + 4000, "link row: b follows a");
    check(block_of(link) != block_of(at) &&
              block_of(link) != block_of(at + sizeof(krffs_node_t) - 1),
          "link row: b's link is not in a's header block");
    check(kv_truncate(&mount, fd_slot(&table, fd_a)->ofd->fhandle, 100) == 0, "link row: shrink a");
    was = fd_slot(&table, fd_a)->ofd->fhandle;
    check(buffers[block_of(link)].dirty && buffers[block_of(link)].owner == owner_of(was),
          "link row: the shrink leaves b's link dirty under a's record");
    if (!reopen) {
        check(kv_truncate(&mount, fd_slot(&table, fd_b)->ofd->fhandle, 0) == 0 &&
                  buffers[block_of(link)].dirty &&
                  buffers[block_of(link)].owner == owner_of(fd_slot(&table, fd_b)->ofd->fhandle),
              "link row: a truncate of b to its size retags the link as b's");
    } else {
        fd_free(&table, fd_a);
        open_node(other);
        fd_a = open_node(a);
        if (fd_slot(&table, fd_a)->ofd->fhandle == was) {
            /* The allocator gave the old record back: the tag still holds. */
            teardown();
            return;
        }
    }
    check(kv_fsync(&mount, fd_slot(&table, fd_a)->ofd->fhandle) == 0, "link row: fsync of a");
    check(!buffers[block_of(link)].dirty, "fsync of a shrunk file writes the next file's link");
    teardown();
}

/** After the free header reaches disk, refuse the link's second block. */
static void refuse_link_block(void)
{
    if (flush_calls == 2) {
        refuse_block = 1;
    }
}

/** A torn link makes removals walk forward and keeps the volume dirty. */
static void failed_link(void)
{
    krffs_mount_t *fs;
    krffs_node_t head;
    uint64_t a, b;
    int fd;
    void *handle;

    setup();
    fd = open_node(create("a"));
    handle = fd_slot(&table, fd)->ofd->fhandle;
    k_memset(payload, 'a', sizeof(payload));
    check(kv_write(&mount, handle, payload, 3048, 0) == 3048, "split link: seed a");
    a = follow_moves(handle);
    b = offset_of(&mount, create("b"));
    check(b + KRFFS_FIELD_PREVIOUS == BCACHE_BLOCK_BYTES - 1, "split link: b crosses a block");
    fs = &vol_of(&mount)->fs;
    flush_calls = 0;
    flush_hook = refuse_link_block;
    check(kv_truncate(&mount, handle, 100) == 0 && fs->stale_links, "split link: write failed");
    flush_hook = NULL;
    check(krffs_head(fs, b, &head) == 0 &&
              krffs_previous_offset(fs, b, &head) == a + sizeof(head) + 100,
          "split link: use the forward chain to find the new free node");
    check(kv_prepare_unmount(&mount) == 0 && (((krffs_node_t *)medium)->mode & KRFFS_MODE_DIRTY),
          "split link: unmount leaves the repair owed on disk");
    teardown();

    k_memcpy(cached, medium, IMAGE_BYTES);
    check(krffs_volume_mount(&mount, &disk, 0, NULL) == 0, "split link: dirty volume remounts");
    if (mount.priv) {
        check(krffs_chain_is_valid(&vol_of(&mount)->fs), "split link: remount rebuilt the link");
        kv_unmount(&mount);
    }
}

/** A failed second shrink flush leaves the old extent and shortened data. */
static void failed_shrink_flush(void)
{
    krffs_mount_t *fs;
    krffs_node_t head;
    uint64_t at;
    int fd;
    void *handle;

    setup();
    fd = open_node(create("a"));
    handle = fd_slot(&table, fd)->ofd->fhandle;
    k_memset(payload, 'a', sizeof(payload));
    check(kv_write(&mount, handle, payload, 4096, 0) == 4096, "shrink flush: seed a");
    at = follow_moves(handle);
    fs = &vol_of(&mount)->fs;
    flush_calls = 0;
    fail_flush_at = 2;
    check(kv_truncate(&mount, handle, 100) == -EIO && flush_calls == 2,
          "shrink flush: the failed header flush returns EIO");
    check(krffs_head(fs, at, &head) == 0 && head.size == sizeof(head) + 4096 &&
              head.data_size == 100 && krffs_chain_is_valid(fs),
          "shrink flush: the old extent and shorter length stay consistent");
    teardown();
}

/** Write a node's header into the fixture's cache: `type`, `size` bytes,
 * the link `previous`, and `name`, or none for NULL. */
static void place(uint32_t at, uint8_t type, uint32_t size, uint32_t previous, const char *name)
{
    krffs_node_t *head = (krffs_node_t *)(cached + at);

    head->magic = KRFFS_FILE_SYSTEM_MAGIC;
    head->type = type;
    head->size = size;
    head->previous_node_size = previous;
    head->mode = type == KRFFS_ROOT_NODE ? (uint16_t)(0755 | KRFFS_MODE_DIRTY) : 0644;
    if (name) {
        k_memcpy(head->name, name, k_strlen(name) + 1);
    }
}

/** Put the image built in the fixture's cache on the device and medium,
 * then mount it with the given sector phase. 0 or the mount's -errno. */
static int mount_cached(uint32_t phase)
{
    static unsigned char built[IMAGE_BYTES];

    k_memcpy(built, cached, sizeof(built));
    setup();
    teardown();
    k_memcpy(cached, built, sizeof(cached));
    k_memcpy(device, built, sizeof(device));
    k_memcpy(medium, built, sizeof(medium));
    disk.sector_phase = phase;
    return krffs_volume_mount(&mount, &disk, 0, NULL);
}

/**
 * Through the adapter, a volume takes its sector from the device, and a
 * merge whose size's low byte ends one sector and whose next byte starts
 * another, inside one block, keeps the free nodes apart.
 */
static void sector_merge(void)
{
    krffs_node_t head;
    int rc;

    k_memset(cached, 0, sizeof(cached));
    place(0, KRFFS_ROOT_NODE, 698, IMAGE_BYTES - 3000, NULL);
    place(698, KRFFS_RESERVED_NODE, 1025, 698, "victim");
    place(1723, KRFFS_FREE_NODE, 511, 1025, NULL);
    place(2234, KRFFS_RESERVED_NODE, 766, 511, "kept");
    place(3000, KRFFS_FREE_NODE, IMAGE_BYTES - 3000, 766, NULL);
    ((krffs_node_t *)cached)->mode = 0755;
    check(mount_cached(0) == 0 && vol_of(&mount)->fs.sector == BLOCK_SECTOR_SIZE,
          "sector merge: the volume takes the device's sector");
    if (!mount.priv) {
        return;
    }
    vfs_ns_lock(&mount);
    rc = kv_unlink(&mount, mount.root, "victim");
    vfs_ns_unlock(&mount);
    check(rc == 0 && krffs_head(&vol_of(&mount)->fs, 698, &head) == 0 &&
              head.type == KRFFS_FREE_NODE && head.size == 0x401 &&
              krffs_chain_is_valid(&vol_of(&mount)->fs),
          "sector merge: a merge whose size crosses a sector inside a block is not made");
    kv_unmount(&mount);
}

#define LOOP_FILE_DATA (2u * sizeof(krffs_node_t))
#define LOOP_CREATE    600u
#define LOOP_VICTIM    1008u
#define LOOP_RENAME    3869u

static uint8_t backing_image[IMAGE_BYTES + 2 * BLOCK_SECTOR_SIZE];
static uint8_t phase_created_name[KRFFS_FILE_NAME_BUFFER_SIZE];
static uint32_t phase_name_writes;

/**
 * Replay a loop flush on the physical sectors of the image file. Each
 * prefix must leave the inner ring walkable and any new name complete.
 * Count the physical sectors of the renamed file's name each flush changes.
 */
static void flush_physical_sectors(void)
{
    krffs_mount_t fs = {0};
    uint32_t end = LOOP_FILE_DATA + IMAGE_BYTES;
    uint32_t name = LOOP_FILE_DATA + LOOP_RENAME + KRFFS_FIELD_NAME;
    const uint8_t *created = backing_image + LOOP_FILE_DATA + LOOP_CREATE + KRFFS_FIELD_NAME;
    uint32_t sector;
    uint32_t changed = 0;

    fs.backend = &krffs_memory_backend;
    fs.ctx = backing_image + LOOP_FILE_DATA;
    fs.size = IMAGE_BYTES;
    for (sector = LOOP_FILE_DATA / BLOCK_SECTOR_SIZE * BLOCK_SECTOR_SIZE; sector < end;
         sector += BLOCK_SECTOR_SIZE) {
        uint32_t first = sector < LOOP_FILE_DATA ? LOOP_FILE_DATA : sector;
        uint32_t last = sector + BLOCK_SECTOR_SIZE;
        uint32_t i;
        int changed_name = 0;

        if (last > end) {
            last = end;
        }
        for (i = first; i < last; i++) {
            if (i >= name && i < name + KRFFS_FILE_NAME_BUFFER_SIZE &&
                backing_image[i] != device[i - LOOP_FILE_DATA]) {
                changed_name = 1;
            }
            backing_image[i] = device[i - LOOP_FILE_DATA];
        }
        changed += changed_name;
        check(krffs_chain_is_walkable(&fs), "loop phase: every physical sector prefix walks");
        check(created[0] == 0 || krffs_names_match(created, phase_created_name),
              "loop phase: every physical sector prefix leaves the new name empty or complete");
    }
    check(changed <= 1, "loop phase: each rename flush changes one sector of the name");
    phase_name_writes += changed;
}

/** Mount an image in an unaligned KRFFS extent and check both boundaries. */
static void loop_sector_phase(void)
{
    krffs_mount_t backing = {0};
    krffs_mount_t *fs;
    krffs_node_t *root = (krffs_node_t *)backing_image;
    krffs_node_t *image = (krffs_node_t *)(backing_image + sizeof(*root));
    krffs_node_t head;
    char old[231], new[231];
    void *created = NULL;
    uint32_t phase;
    uint32_t mid;
    int rc;

    k_memset(backing_image, 0, sizeof(backing_image));
    root->magic = image->magic = KRFFS_FILE_SYSTEM_MAGIC;
    root->type = KRFFS_ROOT_NODE;
    root->size = image->previous_node_size = sizeof(*root);
    root->previous_node_size = image->size = sizeof(backing_image) - sizeof(*root);
    image->type = KRFFS_RESERVED_NODE;
    image->data_size = IMAGE_BYTES;
    k_memcpy(image->name, "image", sizeof("image"));
    backing.backend = &krffs_memory_backend;
    backing.ctx = backing_image;
    backing.size = sizeof(backing_image);
    backing.sector = BLOCK_SECTOR_SIZE;
    check(krffs_chain_is_valid(&backing) && krffs_walk(&backing, "/image") == sizeof(*root),
          "loop phase: the outer KRFFS image contains the backing file");
    phase = krffs_file_phase(&backing, sizeof(*root));
    check(phase == LOOP_FILE_DATA % BLOCK_SECTOR_SIZE && phase != 0,
          "loop phase: sector zero starts inside a physical sector");

    k_memset(old, 'a', sizeof(old) - 1);
    k_memset(new, 'b', sizeof(new) - 1);
    old[sizeof(old) - 1] = new[sizeof(new) - 1] = '\0';
    k_memset(phase_created_name, 0, sizeof(phase_created_name));
    k_memset(phase_created_name, 'c', sizeof(old) - 1);
    k_memset(cached, 0, sizeof(cached));
    place(0, KRFFS_ROOT_NODE, LOOP_CREATE, IMAGE_BYTES - 4980, NULL);
    place(LOOP_CREATE, KRFFS_FREE_NODE, LOOP_VICTIM - LOOP_CREATE, LOOP_CREATE, NULL);
    place(LOOP_VICTIM, KRFFS_RESERVED_NODE, 1025, LOOP_VICTIM - LOOP_CREATE, "victim");
    place(2033, KRFFS_FREE_NODE, 511, 1025, NULL);
    place(2544, KRFFS_RESERVED_NODE, 1325, 511, "dest");
    ((krffs_node_t *)(cached + 2544))->mode = KRFFS_S_IFDIR | 0755;
    ((krffs_node_t *)(cached + 2544))->id = 0x1122334455667788ull;
    place(LOOP_RENAME, KRFFS_RESERVED_NODE, 1111, 1325, old);
    place(4980, KRFFS_FREE_NODE, IMAGE_BYTES - 4980, 1111, NULL);
    ((krffs_node_t *)cached)->mode = 0755;
    check(mount_cached(phase) == 0, "loop phase: mount fixture");
    if (!mount.priv) {
        return;
    }
    fs = &vol_of(&mount)->fs;
    check(fs->sector_phase == phase, "loop phase: the inner mount takes the device phase");
    check(krffs_length_steps(fs, 992, 0x401, 0x600, &mid) == 0 && mid == 0x501,
          "loop phase: a length straddling the physical boundary takes a safe middle step");
    k_memcpy(backing_image + LOOP_FILE_DATA, medium, sizeof(medium));
    phase_name_writes = 0;
    flush_hook = flush_physical_sectors;

    vfs_ns_lock(&mount);
    rc = kv_create(&mount, mount.root, (const char *)phase_created_name, 0644, &created);
    check(rc == 0 && created == handle_of(LOOP_CREATE),
          "loop phase: create the name across a physical sector boundary");
    rc = kv_unlink(&mount, mount.root, "victim");
    check(rc == 0 && krffs_head(fs, LOOP_VICTIM, &head) == 0 && head.type == KRFFS_FREE_NODE &&
              head.size == 1025,
          "loop phase: a merge may not change bytes at backing offsets 2047 and 2048");
    rc = kv_rename(&mount, mount.root, old, mount.root, new);
    check(rc == 0 && phase_name_writes == 3,
          "loop phase: rename writes three pieces across sector and cache boundaries");
    vfs_ns_unlock(&mount);
    flush_hook = NULL;
    check(krffs_chain_is_valid(fs), "loop phase: the live ring is valid");
    kv_unmount(&mount);
}

/**
 * A dirty volume of more names than the repair's index holds, with a torn
 * backward link and one name on two nodes, mounts writable with the link
 * rebuilt and the name on one node, found in passes over ranges of name
 * hashes, and clears its dirty flag at unmount. With no memory for the
 * index, it mounts with the link rebuilt, owes the check of names, and
 * keeps its dirty flag at unmount, so that fsck.krffs is still owed.
 */
static void owed_check(void)
{
    enum { NAMES = 10 };
    uint32_t node = sizeof(krffs_node_t) + 1;
    uint32_t end = sizeof(krffs_node_t) + NAMES * node;
    char name[3];
    int pass;
    int i;

    for (pass = 0; pass < 2; pass++) {
        k_memset(cached, 0, sizeof(cached));
        place(0, KRFFS_ROOT_NODE, sizeof(krffs_node_t), IMAGE_BYTES - end, NULL);
        for (i = 0; i < NAMES; i++) {
            name[0] = 'n';
            name[1] = (char)('0' + (i == NAMES - 1 ? 0 : i));
            name[2] = '\0';
            place(sizeof(krffs_node_t) + i * node, KRFFS_RESERVED_NODE, node,
                  i == 5   ? 0
                  : i == 0 ? sizeof(krffs_node_t)
                           : node,
                  name);
        }
        place(end, KRFFS_FREE_NODE, IMAGE_BYTES - end, node, NULL);
        kmalloc_refuses = pass;
        check(mount_cached(0) == 0, "owed check: a dirty volume of many names mounts");
        kmalloc_refuses = 0;
        if (!mount.priv) {
            return;
        }
        if (pass == 0) {
            check(vol_of(&mount)->writable && !vol_of(&mount)->fs.check_owed &&
                      krffs_chain_is_valid(&vol_of(&mount)->fs) &&
                      ((krffs_node_t *)(medium + sizeof(krffs_node_t) + (NAMES - 1) * node))
                              ->name[0] == '\0',
                  "many names: its links are rebuilt and its repeated name found in passes");
            check(kv_prepare_unmount(&mount) == 0 &&
                      !(((krffs_node_t *)medium)->mode & KRFFS_MODE_DIRTY),
                  "and its dirty flag is cleared at unmount");
        } else {
            check(!vol_of(&mount)->writable && (mount.flags & VFS_MS_RDONLY) &&
                      vol_of(&mount)->fs.check_owed && krffs_chain_is_valid(&vol_of(&mount)->fs),
                  "owed check: its links are rebuilt, the check of names is owed and the mount "
                  "is read-only");
            check(kv_prepare_unmount(&mount) == 0 &&
                      (((krffs_node_t *)medium)->mode & KRFFS_MODE_DIRTY),
                  "an owed check of names keeps the dirty flag at unmount");
        }
        kv_unmount(&mount);
    }
}

/**
 * A pass of the search for repeated names whose index fills exactly with
 * the nodes of its smallest hash, eight files named "a" of which two share
 * the root, with a larger hash left out, resolves them: every node of a
 * hash below the smallest left out is in the index.
 */
static void full_group(void)
{
    uint32_t node = sizeof(krffs_node_t) + 1;
    uint32_t end = sizeof(krffs_node_t) + 9 * node;
    krffs_node_t *first;
    krffs_node_t *second;
    int i;

    k_memset(cached, 0, sizeof(cached));
    place(0, KRFFS_ROOT_NODE, sizeof(krffs_node_t), IMAGE_BYTES - end, NULL);
    for (i = 0; i < 9; i++) {
        uint32_t at = sizeof(krffs_node_t) + i * node;

        place(at, KRFFS_RESERVED_NODE, node, i == 0 ? sizeof(krffs_node_t) : node,
              i == 8 ? "b" : "a");
        ((krffs_node_t *)(cached + at))->parent_id = i < 2 ? 0 : (uint64_t)i;
    }
    place(end, KRFFS_FREE_NODE, IMAGE_BYTES - end, node, NULL);
    check(mount_cached(0) == 0, "full group: a dirty volume of nine names mounts");
    if (!mount.priv) {
        return;
    }
    first = (krffs_node_t *)(medium + sizeof(krffs_node_t));
    second = (krffs_node_t *)(medium + sizeof(krffs_node_t) + node);
    check(!vol_of(&mount)->fs.check_owed && (first->name[0] == '\0') != (second->name[0] == '\0'),
          "full group: the repeated name that fills the index is resolved, and no check is owed");
    kv_unmount(&mount);
}

/**
 * A mount a failed write made read-only syncs the whole device at an fsync
 * of one file, since the failed sync point may have left any file's blocks
 * dirty: a block of nobody's is written with the file's header.
 */
static void owned_after_failure(void)
{
    int fd;

    setup();
    fd = open_node(create("lone"));
    check(kv_write(&mount, fd_slot(&table, fd)->ofd->fhandle, "x", 1, 0) == 1, "failure row: seed");
    buffers[IMAGE_BLOCKS - 1].dirty = 1;
    mount.flags |= VFS_MS_RDONLY;
    check(kv_fsync(&mount, fd_slot(&table, fd)->ofd->fhandle) == 0 &&
              !buffers[IMAGE_BLOCKS - 1].dirty,
          "fsync on a mount made read-only writes every dirty block");
    teardown();
}

/** The count of inotify watches, which fd.c reads at a close: none. */
int inotify_watches;

/**
 * Do nothing: fd.c reports a description's close to inotify, and no row
 * watches.
 */
void inotify_desc(struct open_file *ofd, uint32_t mask)
{
    (void)ofd;
    (void)mask;
}

/** No row's file system is watched. */
int inotify_mount_watched(const struct vfs_mount *mnt)
{
    (void)mnt;
    return 0;
}

/** Return `m`: no row makes a bind mount, so a mount is its own base. */
const struct vfs_mount *vfs_mount_base(const struct vfs_mount *m)
{
    return m;
}

/** Fail the length store's second block after its comparison read. */
static void refuse_length_end(void)
{
    flush_hook = NULL;
    refuse_block = 1;
    refuse_skip = 1;
}

/** Check the byte count and EOF after an append at a split length field. */
static void split_length_failure(void)
{
    const uint32_t at = BCACHE_BLOCK_BYTES - KRFFS_FIELD_DATA_SIZE - 4;
    krffs_node_t *head;
    struct krffs_open *o;
    char back[512];
    int rc;

    k_memset(cached, 0, sizeof(cached));
    place(0, KRFFS_ROOT_NODE, at, IMAGE_BYTES - at, NULL);
    place(at, KRFFS_RESERVED_NODE, IMAGE_BYTES - at, at, "edge");
    head = (krffs_node_t *)(cached + at);
    head->data_size = 256;
    check(mount_cached(0) == 0, "mount for split length");
    check(kv_open(&mount, handle_of(at), 0, (void **)&o) == 0, "open at block edge");
    k_memset(payload, 'x', 256);
    flush_hook = refuse_length_end;
    rc = kv_write(&mount, o, payload, 256, 256);
    check(refuse_block == -1 && rc == -EIO && kv_read(&mount, o, back, sizeof(back), 0) == 256,
          "a failed length store publishes no bytes of the append");
    check(kv_write(&mount, o, payload, 256, 256) == 256 &&
              kv_read(&mount, o, back, sizeof(back), 0) == 512 && back[511] == 'x',
          "the append succeeds after the block recovers");
    kv_release(&mount, o);
    kv_unmount(&mount);
}

/** Exercise each failure after the first half of a field was acquired. */
static void field_failure_rows(void)
{
    const uint32_t at = BCACHE_BLOCK_BYTES - 2;
    const uint32_t data = 0x11223344;
    struct krffs_volume *vol;
    int i;

    setup();
    vol = vol_of(&mount);
    for (i = 0; i < 2; i++) {
        k_memset(cached + at, 0, sizeof(data));
        buffers[0].dirty = buffers[1].dirty = 0;
        if (i == 0) {
            refuse_block = 1;
        } else {
            buffers[1].bytes = 1;
        }
        check(volume_write(vol, at, &data, sizeof(data)) != 0 && !cached[at] &&
                  !cached[at + 1] && !buffers[0].dirty && !buffers[1].dirty,
              "a missing or short second block leaves both field blocks alone");
    }
    buffers[1].bytes = BCACHE_BLOCK_BYTES;
    teardown();
}

/** Refuse a dirty mount during its orphan free and mount it again. */
static void refused_repair_row(void)
{
    uint32_t n = sizeof(krffs_node_t);
    int rc;

    setup();
    teardown();
    k_memset(cached, 0, sizeof(cached));
    place(0, KRFFS_ROOT_NODE, n, IMAGE_BYTES - 2 * n, NULL);
    place(n, KRFFS_RESERVED_NODE, n, n, NULL);
    place(2 * n, KRFFS_FREE_NODE, IMAGE_BYTES - 2 * n, n, NULL);
    k_memcpy(device, cached, sizeof(cached));
    k_memcpy(medium, cached, sizeof(cached));
    sync_error = 1;
    block_writes = 0;
    rc = krffs_volume_mount(&mount, &disk, 0, NULL);
    check(rc == -EIO && !mount.priv && !buffers[0].dirty && !writes_halted &&
              !block_writes && __builtin_memcmp(cached, medium, sizeof(cached)) == 0,
          "refusing an orphan repair drops its dirty blocks and its device halt");
    sync_error = 0;
    mount.flags = 0;
    rc = krffs_volume_mount(&mount, &disk, 0, NULL);
    check(rc == 0 && !(mount.flags & VFS_MS_RDONLY) && block_writes &&
              ((krffs_node_t *)(medium + n))->type == KRFFS_FREE_NODE,
          "a fresh writable mount repairs the recovered device");
    if (!rc) {
        kv_unmount(&mount);
    }
}

/** Fail validation before and after the dirty-volume repair. */
static void refused_chain_rows(void)
{
    static const int skips[] = {1, 2, 4, 5};
    unsigned i;
    int rc;

    setup();
    teardown();
    ((krffs_node_t *)cached)->mode &= ~KRFFS_MODE_DIRTY;
    k_memcpy(device, cached, sizeof(device));
    for (i = 0; i < sizeof(skips) / sizeof(skips[0]); i++) {
        refuse_skip = skips[i];
        refuse_block = 0;
        rc = krffs_volume_mount(&mount, &disk, 0, NULL);
        check(rc == -EIO && !mount.priv, "volume validation preserves a header read error");
    }
    sync_error = 1;
    rc = krffs_volume_mount(&mount, &disk, 0, NULL);
    check(rc == -EIO && !buffers[0].dirty && !(((krffs_node_t *)cached)->mode & KRFFS_MODE_DIRTY),
          "a refused dirty flag store leaves no dirty buffer for a later mount");
    sync_error = 0;
}

/** Fail the free at the last close of an unlinked file. */
static void last_close_failure(void)
{
    void *node, *handle;
    struct krffs_volume *vol;

    setup();
    node = create("open");
    vol = vol_of(&mount);
    check(kv_open(&mount, node, 0, &handle) == 0 &&
              kv_unlink(&mount, mount.root, "open") == 0, "unlink while open");
    refuse_block = 0;
    kv_release(&mount, handle);
    check(vol->fs.halted && vol->fs.check_owed && writes_halted && !vol->opens,
          "a last close that cannot read the orphan halts its volume");
    teardown();
}

/** Fail the reap when the last close lands during unlink or replacement. */
static void close_during_unlink_row(int replace)
{
    void *handle;
    int rc;

    setup();
    if (replace) {
        create("new");
    }
    original = create("old");
    check(kv_open(&mount, original, 0, &handle) == 0, "open the name being removed");
    close_at_unlink = handle;
    fail_after_move = 1;
    rc = replace ? kv_rename(&mount, mount.root, "new", mount.root, "old")
                 : kv_unlink(&mount, mount.root, "old");
    check(!close_at_unlink && rc == -EIO && vol_of(&mount)->fs.halted && writes_halted,
          "a failed reap after a concurrent last close stops the volume");
    teardown();
}

/** Fail a buffered repair write on a disk whose normal sync always flushes. */
static void repair_flush_row(void)
{
    setup();
    disk.flags |= BLOCK_WRITE_THROUGH;
    sync_error = 1;
    check(volume_sync(vol_of(&mount)) != 0 && flush_calls == 0,
          "a failed ordered write issues no trailing drive flush");
    teardown();
}

/** Fail reclaim while fetching the victim header at the last close. */
static void reclaim_during_reap(void)
{
    void *node, *handle;
    krffs_node_t *h;

    setup();
    node = create("held");
    h = (krffs_node_t *)(cached + offset_of(&mount, node));
    check(kv_open(&mount, node, 0, &handle) == 0 &&
              kv_unlink(&mount, mount.root, "held") == 0, "hold an unlinked node");
    reclaim_error = 1;
    kv_release(&mount, handle);
    check(!reclaim_error && !recovery_active && vol_of(&mount)->fs.halted &&
              h->type == KRFFS_RESERVED_NODE,
          "a failed cache victim halts the reap even when the requested block is supplied");
    teardown();
}

/** Halt while acquiring the second block of a field that spans two. */
static void reclaim_during_store(void)
{
    uint32_t data = 0x11223344;
    uint32_t at = BCACHE_BLOCK_BYTES - 2;
    struct krffs_volume *vol;

    setup();
    vol = vol_of(&mount);
    bcache_recovering(&disk, 1);
    reclaim_error = 2;
    check(volume_write(vol, at, &data, sizeof(data)) < 0 && !reclaim_error &&
              !cached[at] && !cached[at + 2] && !buffers[0].dirty && !buffers[1].dirty,
          "a halt while fetching the second block leaves the entire field alone");
    bcache_recovering(&disk, 0);
    teardown();
}

/** Run the independent volume failure rows. */
static void volume_failure_rows(void)
{
    split_length_failure();
    field_failure_rows();
    refused_repair_row();
    refused_chain_rows();
    last_close_failure();
    close_during_unlink_row(0);
    close_during_unlink_row(1);
    repair_flush_row();
    reclaim_during_reap();
    reclaim_during_store();
}

/**
 * Run every row: relocation by a write and by a truncate, a directory read,
 * an O_PATH pin, a failed relocation of each kind, the unmount's order of
 * writes, a failed clean flush, the errors of a creation, an owner across a
 * block, a taken identifier, a working directory, the refusals of unlink,
 * rename and rmdir, the group of a node made in a set-group-ID
 * directory, and an fsync that writes one file, across a move, with the
 * link a shrink leaves after its tag is lost, and after a failed write, and
 * the dirty flag an owed check of names keeps.
 * Returns 1 when a row failed, else 0.
 */
int main(int argc, char **argv)
{
    volume_failure_rows();
    if (argc == 2 && __builtin_strcmp(argv[1], "failures") == 0) {
        printf("krffsmount-hosttest: failure rows: %d failures\n", failures);
        return failures != 0;
    }
    relocation(0);
    relocation(1);
    directory();
    pinned();
    failed_relocation(0);
    failed_relocation(1);
    durability();
    failed_clean_flush();
    creation_errors();
    owner_across();
    taken_identifier();
    working_directory();
    refusals();
    halted_refusals();
    phase_reported();
    refused_rename_keeps_destination();
    setgid_group();
    owned_fsync();
    owned_across_move();
    shrink_link(0);
    shrink_link(1);
    failed_link();
    failed_shrink_flush();
    owed_check();
    full_group();
    sector_merge();
    loop_sector_phase();
    owned_after_failure();
    if (!failures) {
        printf("krffsmount-hosttest: result: pass: relocation and durability\n");
    }
    return failures != 0;
}
