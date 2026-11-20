/**
 * loop_attach_hosttest.c: loop attachment, of rach2/sys/block/loop.c, across
 * the relocation of a KRFFS file and the reuse of its old extent.
 *
 * host-deps: rach2/sys/block/chain.c rach2/sys/lib/kstring.c
 * host-deps: rach2/sys/lib/kctypes.c
 * host-deps: tests/host/compat/nodelock_none.c
 * host-deps: tests/host/compat/spinlock_host.c tests/host/compat/bkl_held.c
 * host-cflags: -Wno-pointer-to-int-cast -Wno-int-to-pointer-cast
 *
 * The test includes the kernel's krffs.c and loop.c. The KRFFS core moves
 * the file while the attachment waits for a modelled namespace lock; VFS
 * dispatch, description counts and cache I/O are stubs.
 */
#include "fs/krffs.c"
#include "block/loop.c"

extern int printf(const char *format, ...);
extern int memcmp(const void *a, const void *b, unsigned long size);

/* The repair in krffs.c indexes names with these; no row here reaches it,
   so the index is never had and the search would be left owed. */
void *kmalloc(size_t size)
{
    (void)size;
    return (void *)0;
}

/** Nothing to give back, since `kmalloc` gives nothing. */
void kfree(void *p)
{
    (void)p;
}

#define IMAGE_BYTES 65536u
static uint8_t image[IMAGE_BYTES];
static struct vfs_mount mount, other_mount;
static block_device_t disk;
/** The phase of a file in the fixture's production boot image. */
static uint32_t root_phase(struct vfs_mount *mnt, void *node, void *handle)
{
    (void)mnt;
    (void)handle;
    return krffs_node_phase(node);
}

static const struct vfs_ops root_ops = {.phase = root_phase};
const struct vfs_type vfs_krffs_type = {"krffs", 0, &root_ops};
/* No processor runs a thread: the rows call no transfer, whose count of
 * loop transfers is the only reader of `current_proc` here. */
struct percpu cpu_table[NR_CPUS];
static krffs_node_t *original, *relocated;
/* What runs while the attachment waits for the namespace lock. */
static void (*waiting)(void);
/* The failed checks, the namespace lock's depth, the open handles, the
   published descriptions, the active resolver operations, whether the
   writer's descriptor is busy, and the resolutions. */
static int failures, depth, opened, refs, active, busy, resolves;
/* The faults a row sets: an open's error, a sync's error, a path resolving
   on another mount, and a missing path. */
static int open_error, sync_error, switch_mount, missing;
/* Whether a shared mapping may have stored into the file's pages, and the
   bytes the last overlay was asked for. */
static int stores;
static uint64_t overlay_off;
static uint32_t overlay_len;
static char payload[4096], replacement[512], output[512];

/** Count a failed check and print `what`. */
static void check(int ok, const char *what)
{
    if (!ok) {
        printf("loop-attach-hosttest: fail: %s\n", what);
        failures++;
    }
}

/** The kernel log; the records are dropped. */
void klog(int level, const char *subsystem, const char *format, ...)
{
    (void)level;
    (void)subsystem;
    (void)format;
}

/** The console; output is dropped. */
void console_printf(const char *format, ...)
{
    (void)format;
}

/** Supply a fixed timestamp. */
void rtc_realtime(int64_t *seconds, int64_t *nanoseconds)
{
    *seconds = 1;
    *nanoseconds = 0;
}

/** Finish the waiting writer before granting the attachment's lock. */
void vfs_ns_lock(struct vfs_mount *mnt)
{
    void (*run)(void) = waiting;

    check(mnt == &mount, "attachment locks the resolved mount");
    waiting = NULL;
    if (run) {
        check(depth == 1 && busy, "writer holds the lock before attachment waits");
        run();
        check(depth == 0 && !busy, "writer releases the lock before attachment resumes");
    }
    depth++;
}

/** Check lock balance. */
void vfs_ns_unlock(struct vfs_mount *mnt)
{
    check(mnt == &mount && depth > 0, "namespace lock is balanced");
    depth--;
}

/** Resolve through the production KRFFS path walker. */
int vfs_resolve(const struct vfs_ref *start, const char *path, int flags, struct vfs_ref *out,
                char *leaf, uint32_t cap)
{
    (void)start;
    (void)flags;
    (void)leaf;
    (void)cap;
    resolves++;
    if (missing) {
        return -ENOENT;
    }
    out->node = krffs_resolve(path);
    out->mnt = switch_mount ? &other_mount : &mount;
    if (!out->node) {
        return -ENOENT;
    }
    active++;
    return 0;
}

/** Release a resolver's active operation. */
void vfs_ref_release(struct vfs_ref *ref)
{
    check(ref->mnt != NULL && active > 0, "resolution reference is balanced");
    active--;
}

/** End a resolver operation after transfer to the loop description. */
void vfs_leave(struct vfs_mount *mnt)
{
    (void)mnt;
    check(active > 0, "publication ends an active operation");
    active--;
}

/** Read root-node attributes as the VFS root adapter does. */
int vfs_getattr(struct vfs_mount *mnt, void *node, struct vfs_stat *st)
{
    krffs_node_t *n = node;

    (void)mnt;
    k_memset(st, 0, sizeof(*st));
    st->mode = KRFFS_IS_DIR(n->mode) ? 0040000u : 0100000u;
    st->size = n->data_size;
    return 0;
}

/** Open a node, whose handle is its node pointer as on the KRFFS root;
 *  fails with `open_error` when it is set. */
int vfs_open_node(struct vfs_mount *mnt, void *node, int flags, void **handle)
{
    (void)mnt;
    (void)flags;
    if (open_error) {
        return open_error;
    }
    *handle = node;
    opened++;
    return 0;
}

/** Release an opened handle. */
void vfs_release(struct vfs_mount *mnt, void *handle)
{
    (void)mnt;
    (void)handle;
    check(opened > 0, "open handle is balanced");
    opened--;
}

/** Count the published description. */
void vfs_desc_open(struct vfs_mount *mnt)
{
    (void)mnt;
    refs++;
}

/** Start a description's cursors into the write-back errors at none,
 * which no row reports. */
void vfs_wb_err_sample(struct vfs_mount *mnt, uint32_t *file, uint32_t *fs)
{
    (void)mnt;
    *file = 0;
    *fs = 0;
}

/** Close the published description. */
void vfs_desc_close(struct vfs_mount *mnt, void *node, void *handle)
{
    (void)node;
    vfs_release(mnt, handle);
    refs--;
}

/** Read through the production root interface. */
int32_t vfs_read(struct vfs_mount *mnt, void *handle, void *buf, uint32_t count, uint64_t offset)
{
    (void)mnt;
    return krffs_read_node(handle, buf, count, (uint32_t)offset);
}

/** Write through the production root interface. */
int32_t vfs_write(struct vfs_mount *mnt, void *handle, const void *buf, uint32_t count,
                  uint64_t offset)
{
    krffs_node_t *node = handle;
    int rc = krffs_write_node(&node, buf, count, (uint32_t)offset);

    if (node != handle) {
        loop_repoint(mnt, handle, node);
    }
    return rc;
}

/** Sync the backing file and return `sync_error`. */
int vfs_fsync(struct vfs_mount *mnt, void *handle, uint32_t *seen)
{
    (void)mnt;
    (void)handle;
    (void)seen;
    return sync_error;
}

/** Whether a shared mapping may have stored into the file: `stores`. */
int filemap_disk_stores(const struct vfs_mount *mnt, uint64_t ino)
{
    (void)mnt;
    (void)ino;
    return stores;
}

/** Stand for the pages of a shared mapping: record the range asked for
 * and put 0x6D, which no file of the rows holds, in its first byte, under
 * the big lock that the pages are walked under. */
void filemap_disk_overlay(const struct vfs_mount *mnt, uint64_t ino, uint64_t off, void *buf,
                          uint32_t len)
{
    (void)mnt;
    (void)ino;
    check(bkl_held(), "a loop read takes a mapping's stores under the big lock");
    overlay_off = off;
    overlay_len = len;
    if (len) {
        *(uint8_t *)buf = 0x6D;
    }
}

/** No row keeps resident file pages for a private mapping. */
void filemap_disk_written(const struct vfs_mount *mnt, uint64_t ino, uint64_t off, const void *buf,
                          uint32_t len)
{
    (void)mnt;
    (void)ino;
    (void)off;
    (void)buf;
    (void)len;
}

/** Report the modeled writer's descriptor state. */
int fd_node_busy(struct vfs_mount *mnt, void *node)
{
    (void)mnt;
    (void)node;
    return busy;
}

/* The big lock of `compat/bkl_held.c`: a row that sets `bkl_host_free`
 * stands for a caller without it, and `bkl_host_takes` counts the takes. */
extern int bkl_host_free;
extern int bkl_host_takes;

/** The mount itself: these rows have no bind mounts. */
const struct vfs_mount *vfs_mount_base(const struct vfs_mount *m)
{
    return m;
}

/** No interrupt comes in these rows; the mask is not modelled. */
uint32_t kirq_save(void)
{
    return 0;
}

/** Nothing to put back. */
void kirq_restore(uint32_t flags)
{
    (void)flags;
}

/** No filesystem is mounted on the loop device in these rows. */
int vfs_device_holds(block_device_t *dev)
{
    (void)dev;
    return 0;
}

/** No loop cache buffer is held in these rows. */
int bcache_device_holds(block_device_t *dev)
{
    (void)dev;
    return 0;
}

/** No loop cache buffer is dirty in these rows. */
int bcache_sync(block_device_t *dev)
{
    (void)dev;
    return 0;
}

/** No loop cache buffer survives detach. */
int bcache_invalidate(block_device_t *dev)
{
    (void)dev;
    return 0;
}

/** No physical removal occurs in these rows. */
void bcache_device_gone(block_device_t *dev)
{
    (void)dev;
}

/** Make an image file followed by a file that prevents in-place growth. */
static void reset(void)
{
    krffs_node_t *root = (krffs_node_t *)image;
    krffs_node_t *free_node = (krffs_node_t *)(image + sizeof(*root));
    krffs_node_t *blocker;

    k_memset(image, 0, sizeof(image));
    k_memset(loops, 0, sizeof(loops));
    root->magic = free_node->magic = KRFFS_FILE_SYSTEM_MAGIC;
    root->type = KRFFS_ROOT_NODE;
    root->id = KRFFS_ROOT_ID;
    root->mode = KRFFS_S_IFDIR | 0755;
    root->size = free_node->previous_node_size = sizeof(*root);
    root->previous_node_size = free_node->size = IMAGE_BYTES - sizeof(*root);
    free_node->type = KRFFS_FREE_NODE;
    check(init_krffs(image, sizeof(image)) == 0, "initialize the KRFFS root");
    k_memset(payload, 0x31, sizeof(payload));
    k_memset(replacement, 0x72, sizeof(replacement));
    check(krffs_create("/image") == 0, "create the backing file");
    original = krffs_resolve("/image");
    check(krffs_write_node(&original, payload, 512, 0) == 512, "write the backing file");
    check(krffs_create("/blocker") == 0, "create the adjacent file");
    blocker = krffs_resolve("/blocker");
    check(krffs_write_node(&blocker, replacement, 512, 0) == 512, "write the adjacent file");
    mount.type = &vfs_krffs_type;
    mount.dev = &disk;
    depth = opened = refs = active = busy = resolves = 0;
    open_error = sync_error = switch_mount = missing = stores = 0;
    overlay_off = ~(uint64_t)0;
    overlay_len = 0;
    waiting = NULL;
}

/** Complete relocation while attachment waits, then release the writer. */
static void move_file(void)
{
    relocated = original;
    check(krffs_write_node(&relocated, payload, sizeof(payload), 0) == sizeof(payload) &&
              relocated != original && original->type == KRFFS_FREE_NODE,
          "writer relocates and frees the old extent during lock wait");
    loop_repoint(&mount, original, relocated);
    busy = 0;
    vfs_ns_unlock(&mount);
}

/** Reuse the freed location and require loop I/O to retain the image. */
static void relocation_row(void)
{
    krffs_node_t *reused;
    block_device_t *loop;
    int rc;

    reset();
    depth = busy = 1;
    waiting = move_file;
    rc = loop_attach(0, "/image", 1);
    check(rc == 0, "attachment succeeds after the writer completes");
    check(resolves == 2 && loops[0].node == relocated && loops[0].handle == relocated,
          "attachment publishes the relocated node and handle");
    check(krffs_create("/replacement") == 0, "reuse the released extent");
    reused = krffs_resolve("/replacement");
    check(krffs_write_node(&reused, replacement, 512, 0) == 512 && reused == original,
          "another file occupies the old backing location");
    loop = loop_attached_at(0);
    check(loop && loop->sector_count == sizeof(payload) / 512,
          "attachment uses the relocated file's size");
    check(loop && loop->read(loop, 0, 1, output) == 0 && memcmp(output, payload, 512) == 0,
          "loop read avoids the reused old extent");
    k_memset(output, 0x55, sizeof(output));
    check(loop && loop->write(loop, 0, 1, output) == 0, "loop writes the relocated image");
    bkl_host_free = 1;
    bkl_host_takes = 0;
    check(loop && loop->write(loop, 0, 1, output) == 0 && bkl_host_takes == 1 && !bkl_held() &&
              loops[0].in_flight == 0,
          "a write from a caller without the big lock takes it for the file system alone");
    bkl_host_free = 0;
    check(krffs_read_node(reused, output, 512, 0) == 512 && memcmp(output, replacement, 512) == 0,
          "loop write preserves the replacement file");
    check(loop_detach(0) == 0 && !depth && !opened && !refs && !active,
          "detach releases the relocated attachment");
}

/** The disk-root fixture writes directly to its image. */
static int root_flush(void)
{
    return 0;
}

/** Publish the relocated file's sector phase, then reuse the slot on RAM. */
static void phase_row(void)
{
    block_device_t *loop;
    uint32_t phase;

    reset();
    check(init_krffs_disk_root(image, sizeof(image), BLOCK_SECTOR_SIZE, root_flush, NULL) == 0,
          "phase: mount the backing image as a disk root");
    depth = busy = 1;
    waiting = move_file;
    check(loop_attach(0, "/image", 1) == 0, "phase: attach the relocated file");
    loop = loop_attached_at(0);
    phase = ((uint32_t)((uint8_t *)relocated - image) + sizeof(*relocated)) % BLOCK_SECTOR_SIZE;
    check(phase != 0 && loop && loop->sector_phase == phase,
          "phase: publish the relocated data's offset within the disk sector");
    check(loop_detach(0) == 0, "phase: detach the disk image");
    check(init_krffs(image, sizeof(image)) == 0 && loop_attach(0, "/image", 1) == 0,
          "phase: reuse the slot for a RAM image");
    loop = loop_attached_at(0);
    check(loop && loop->sector_phase == 0, "phase: the reused slot is aligned");
    check(loop_detach(0) == 0, "phase: detach the RAM image");
}

/**
 * A loop device over a file of the boot root, which has no device, so the
 * device has no `backing`, and a second one over a file of a volume on the
 * first: the second is refused with ELOOP, since the cache blocks of the
 * volume between them do not begin on sectors of the disk and the second
 * device's phase could not place their edges.
 */
static void nested_row(void)
{
    reset();
    mount.dev = NULL;
    check(loop_attach(0, "/image", 1) == 0 && loop_attached_at(0) &&
              loop_attached_at(0)->backing == NULL,
          "nested: a loop device over the boot root has no backing");
    mount.dev = loop_attached_at(0);
    check(loop_attach(1, "/blocker", 1) == -ELOOP && !loops[1].used,
          "nested: a loop device over a file on a loop device is refused");
    mount.dev = NULL;
    check(loop_detach(0) == 0 && !depth && !opened && !refs && !active,
          "nested: the first detaches whole");
}

/** Model a path that now resolves on another mount. */
static void change_mount(void)
{
    switch_mount = 1;
    busy = 0;
    vfs_ns_unlock(&mount);
}

/** Model a path removed while attachment waits. */
static void remove_path(void)
{
    missing = 1;
    busy = 0;
    vfs_ns_unlock(&mount);
}

/** Check cleanup after each failing step under the namespace lock: a path
 * that moved or went, an open or a sync that failed, and a writer in flight
 * on a KRFFS file, which may move it. */
static void error_rows(void)
{
    int row;

    for (row = 0; row < 5; row++) {
        int expected = -EBUSY;

        reset();
        if (row < 2) {
            depth = busy = 1;
            waiting = row == 0 ? change_mount : remove_path;
            expected = row == 0 ? -EBUSY : -ENOENT;
        } else if (row == 2) {
            open_error = expected = -EACCES;
        } else if (row == 3) {
            sync_error = expected = -EIO;
        } else {
            busy = 1;
        }
        check(loop_attach(0, "/image", 1) == expected,
              "attachment returns the failing step's error");
        check(!loops[0].used && !depth && !opened && !refs && !active,
              "failed attachment releases locks and references");
    }
}

/**
 * Read the attached file while a shared mapping may hold stores the file
 * lacks, from a caller without the big lock: the device's read takes them
 * over what the file system read, for the bytes it read, under the big lock,
 * and asks nothing of the pages when no mapping may have stored.
 */
static void mapped_row(void)
{
    block_device_t *loop;

    reset();
    check(loop_attach(0, "/image", 1) == 0, "mapped: attach the file");
    loop = loop_attached_at(0);
    stores = 1;
    bkl_host_free = 1;
    bkl_host_takes = 0;
    k_memset(output, 0, sizeof(output));
    check(loop && loop->read(loop, 0, 1, output) == 0 && output[0] == 0x6D &&
              memcmp(output + 1, payload + 1, 511) == 0 && overlay_off == 0 && overlay_len == 512 &&
              !bkl_held(),
          "mapped: a read takes a shared mapping's stores over the file's bytes");
    bkl_host_free = 0;
    stores = 0;
    overlay_len = 0;
    check(loop && loop->read(loop, 0, 1, output) == 0 && output[0] == payload[0] &&
              overlay_len == 0,
          "mapped: with no store, a read is the file's");
    check(loop_detach(0) == 0 && !depth && !opened && !refs && !active,
          "mapped: detach releases the attachment");
}

/** Copy a loop ioctl's status block in, which fails: no row issues one. */
int copy_from_user(void *dst, const void __user *src, size_t n)
{
    (void)dst;
    (void)src;
    (void)n;
    return -1;
}

/** Copy a loop ioctl's status block out, which fails: no row issues one. */
int copy_to_user(void __user *dst, const void *src, size_t n)
{
    (void)dst;
    (void)src;
    (void)n;
    return -1;
}

/** Run the relocation row and the error rows, and return 1 when any check
 *  failed. */
int main(void)
{
    relocation_row();
    phase_row();
    nested_row();
    mapped_row();
    error_rows();
    if (!failures) {
        printf("loop-attach-hosttest: result: pass: relocation, reuse and cleanup\n");
    }
    return failures != 0;
}
