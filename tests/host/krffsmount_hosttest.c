/**
 * Check mounted KRFFS relocation, O_PATH pins, writeback errors, clean-flag
 * ordering, and the errno a failed creation answers, a directory whose
 * identifier another directory has among them.
 *
 * host-deps: rach2/sys/fs/fd.c rach2/sys/fs/lock.c rach2/sys/lib/kstring.c
 * host-deps: rach2/sys/lib/kctypes.c
 * host-cflags: -Wno-pointer-to-int-cast -Wno-int-to-pointer-cast
 *
 * Lock callbacks run queued operations before a waiting caller resumes.
 * Separate arrays model buffer cache, device cache and persistent bytes.
 */
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
static int failures, depth, sync_error, flush_error, sync_calls, flush_calls;
static int check_clean_order, clean_writes, reuse_fd, truncating;
static int fail_after_move, syncs_after_free, fail_flush_at;
static void (*flush_hook)(void);
static void *original;
static char payload[8192];
process_t *current_proc;
process_t *proc_list;
const struct vfs_type vfs_krffs_type = {0};

/** Record every failed invariant. */
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
/** Complete queued calls before giving the waiting call its lock. */
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

/** Apply the production descriptor scan to every relocation. */
void vfs_repoint(struct vfs_mount *mnt, const void *from, void *to)
{
    check(depth > 0 || mnt->ns_depth > 0, "relocation holds mount lock");
    fd_ofd_repoint(mnt, from, to);
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

/** Reject a lock wait, which no row makes. */
int wq_wait_event(wait_queue_t *wq, wq_cond_fn cond, void *arg)
{
    (void)wq;
    (void)cond;
    (void)arg;
    check(0, "unexpected lock wait");
    return -1;
}

#ifndef KRFFS_OPEN_TEST
/** Reject an unexpected description wait. */
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
    return blk < IMAGE_BLOCKS ? &buffers[blk] : NULL;
}

/** No cache eviction occurs in this model. */
void bcache_release(struct bbuf *b)
{
    (void)b;
}

/** Record a pending cache write. */
void bcache_dirty(struct bbuf *b)
{
    b->dirty = 1;
}

/** Copy dirty blocks to the device cache, retaining them on failure. */
int bcache_sync(block_device_t *dev)
{
    unsigned i;

    (void)dev;
    sync_calls++;
    if (fail_after_move &&
        ((krffs_node_t *)(cached + offset_of(&mount, original)))->type == KRFFS_FREE_NODE &&
        ++syncs_after_free == 2) {
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
            k_memcpy(device + i * BCACHE_BLOCK_BYTES, buffers[i].data, BCACHE_BLOCK_BYTES);
            buffers[i].dirty = 0;
        }
    }
    return 0;
}

/** Flush the device cache, or report its failure without persisting bytes. */
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

/** Format and mount a small volume through the production block backend. */
static void setup(void)
{
    krffs_node_t *root = (krffs_node_t *)cached;
    krffs_node_t *space = (krffs_node_t *)(cached + sizeof(*root));
    unsigned i;

    k_memset(cached, 0, sizeof(cached));
    k_memset(&mount, 0, sizeof(mount));
    k_memset(&disk, 0, sizeof(disk));
    k_memset(buffers, 0, sizeof(buffers));
    depth = sync_error = flush_error = check_clean_order = clean_writes = 0;
    waiting = NULL;
    flush_hook = NULL;
    fail_flush_at = 0;
    fail_after_move = syncs_after_free = 0;
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
    disk.sector_count = IMAGE_BYTES / BLOCK_SECTOR_SIZE;
    mount.type = &vfs_krffs_volume_type;
    check(krffs_volume_mount(&mount, &disk, 0, NULL) == 0, "mount fixture");
    check(fd_table_init(&table) == 0, "descriptor table");
    sync_calls = flush_calls = 0;
}

/** Close every description and release the mount. */
static void teardown(void)
{
    int i;

    for (i = 0; i < table.cap; i++) {
        if (table.entries[i].ofd) {
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
    ofd = table.entries[fd].ofd;
    ofd->vmnt = &mount;
    ofd->vnode = node;
    check(kv_open(&mount, node, 2, &ofd->fhandle) == 0, "open fixture node");
    return fd;
}

/** Move A to B, reuse A for another open file, and prevent growth in place. */
static void first_move(void)
{
    void *reused;

    k_memset(payload, 'B', 4096);
    check(kv_write(&mount, descs[0]->fhandle, payload, 4096, 0) == 4096, "first relocation");
    check(descs[0]->vnode != original, "first relocation changes offset");
    reused = create("reuse");
    check(reused == original, "old offset reused by another file");
    reuse_fd = open_node(reused);
    check(kv_write(&mount, table.entries[reuse_fd].ofd->fhandle, "R", 1, 0) == 1,
          "write reused node");
    create("block-b");
}

/** Queue a second mover with A before the first mover changes it to B. */
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

/** Read A after two waiting writers have moved it and another file reuses A. */
static void relocation(int truncate)
{
    struct vfs_stat st;
    char byte = 0;
    void *current, *held;
    int i;

    setup();
    original = create("target");
    for (i = 0; i < 3; i++) {
        descs[i] = table.entries[open_node(original)].ofd;
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
    check(kv_read(&mount, table.entries[reuse_fd].ofd->fhandle, &byte, 1, 0) == 1 && byte == 'R',
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
    check(kv_readdir(&mount, table.entries[fd].ofd->fhandle, &cursor, &ent) == 1 &&
              !__builtin_memcmp(ent.name, "entry", 6),
          "directory read resolves its open handle");
    check(kv_readdir(&mount, table.entries[fd].ofd->fhandle, &cursor, &ent) == 0,
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
    check(kv_write(&mount, table.entries[fd].ofd->fhandle, "K", 1, 0) == 1, "seed pinned file");
    fd_free(&table, fd);
    check(kv_lookup(&mount, mount.root, "kept", &node) == 0, "look up pinned file");
    fd = fd_alloc(&table);
    ofd = table.entries[fd].ofd;
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

/** Fail the data sync after relocation and require every holder to follow. */
static void failed_relocation(int truncate)
{
    void *current;
    int i, rc;

    setup();
    original = create("target");
    for (i = 0; i < 3; i++) {
        descs[i] = table.entries[open_node(original)].ofd;
    }
    check(kv_write(&mount, descs[0]->fhandle, "A", 1, 0) == 1, "seed failed relocation");
    create("block-a");
    fail_after_move = 1;
    k_memset(payload, 'F', sizeof(payload));
    rc = truncate ? kv_truncate(&mount, descs[0]->fhandle, sizeof(payload))
                  : kv_write(&mount, descs[0]->fhandle, payload, sizeof(payload), 0);
    check(rc == -EIO && syncs_after_free == 2, "data sync fails after relocation");
    check(kv_lookup(&mount, mount.root, "target", &current) == 0 && current != original,
          "failed operation retains relocated node");
    for (i = 0; i < 3; i++) {
        check(descs[i]->vnode == current && handle_of(follow_moves(descs[i]->fhandle)) == current,
              "failed relocation repoints every description");
    }
    teardown();
}

/** Require pre-clear durability, flush errors and read-only writeback. */
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

/** Fail the clean-flag flush after data is durable and attempt later writes. */
static void failed_clean_flush(void)
{
    void *node, *handle;
    char byte;
    int fd;

    setup();
    node = create("retained");
    fd = open_node(node);
    handle = table.entries[fd].ofd->fhandle;
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
    handle = table.entries[fd].ofd->fhandle;
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
 * A creation tells a failing volume from a full one: a sync the medium
 * refuses makes it EIO, and a volume a file has grown to fill makes it
 * ENOSPC.
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
    check(kv_truncate(&mount, table.entries[fd].ofd->fhandle,
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
 * 4095, and the caller of uid and gid 1000 owns it once it has a name.
 */
static void owner_across(void)
{
    static process_t maker;
    struct vfs_stat st;
    void *node = NULL;
    int fd;

    setup();
    fd = open_node(create("spacer"));
    check(kv_truncate(&mount, table.entries[fd].ofd->fhandle, 3826 - 2 * sizeof(krffs_node_t)) == 0,
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
    teardown();
}

/**
 * A directory whose identifier another directory has is EINVAL. `bC` and
 * `cb` hash alike, so the second in one directory would share the first
 * one's children, and the core refuses it with a result the volume reports
 * as a name the file system cannot hold, not as a full or a failing volume.
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

/** Remove the directory `name` of the volume's root under the mount's lock,
 * as `do_rmdir` calls the type. */
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
 * names it: once the process has left, the rmdir removes the directory and
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

/** Exercise both relocating operations and the unmount write order. */
/** fd.c reports a description's close to inotify, and no row watches. */
void inotify_desc(struct open_file *ofd, uint32_t mask)
{
    (void)ofd;
    (void)mask;
}

int main(void)
{
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
    if (!failures) {
        printf("krffsmount-hosttest: result: pass: relocation and durability\n");
    }
    return failures != 0;
}
