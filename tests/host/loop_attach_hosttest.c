/**
 * Check loop attachment across KRFFS relocation and extent reuse.
 *
 * host-deps: rach2/sys/block/chain.c rach2/sys/lib/kstring.c
 * host-deps: rach2/sys/lib/kctypes.c
 * host-cflags: -Wno-pointer-to-int-cast -Wno-int-to-pointer-cast
 *
 * The production KRFFS core moves the file during a modeled namespace-lock
 * wait. VFS dispatch, description counts and cache I/O are substitutes.
 */
#include "fs/krffs.c"
#include "block/loop.c"

extern int printf(const char *format, ...);
extern int memcmp(const void *a, const void *b, unsigned long size);

#define IMAGE_BYTES 65536u
static uint8_t image[IMAGE_BYTES];
static struct vfs_mount mount, other_mount;
static block_device_t disk;
const struct vfs_type vfs_krffs_type = {0};
static krffs_node_t *original, *relocated;
static void (*waiting)(void);
static int failures, depth, opened, refs, active, busy, resolves;
static int open_error, sync_error, switch_mount, missing, mapped, map_in_sync;
static char payload[4096], replacement[512], output[512];

/** Record failed assertions. */
static void check(int ok, const char *what)
{
    if (!ok) {
        printf("loop-attach-hosttest: fail: %s\n", what);
        failures++;
    }
}

/** Ignore production diagnostics. */
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

/** Root KRFFS uses its node pointer as the open handle. */
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

/** Supply successful or failed backing syncs, during which another process
 * may map the file shared. */
int vfs_fsync(struct vfs_mount *mnt, void *handle)
{
    (void)mnt;
    (void)handle;
    if (map_in_sync) {
        mapped = 1;
    }
    return sync_error;
}

/** Report whether a shared mapping has the file's pages in memory. */
int filemap_disk_mapped(const struct vfs_mount *mnt, uint64_t ino)
{
    (void)mnt;
    (void)ino;
    return mapped;
}

/** Report the modeled writer's descriptor state. */
int fd_node_busy(struct vfs_mount *mnt, void *node)
{
    (void)mnt;
    (void)node;
    return busy;
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
    open_error = sync_error = switch_mount = missing = mapped = map_in_sync = 0;
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
    loop = loop_device_at(0);
    check(loop && loop->sector_count == sizeof(payload) / 512,
          "attachment uses the relocated file's size");
    check(loop && loop->read(loop, 0, 1, output) == 0 && memcmp(output, payload, 512) == 0,
          "loop read avoids the reused old extent");
    k_memset(output, 0x55, sizeof(output));
    check(loop && loop->write(loop, 0, 1, output) == 0, "loop writes the relocated image");
    check(krffs_read_node(reused, output, 512, 0) == 512 && memcmp(output, replacement, 512) == 0,
          "loop write preserves the replacement file");
    check(loop_detach(0) == 0 && !depth && !opened && !refs && !active,
          "detach releases the relocated attachment");
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
 * that moved or went, an open or a sync that failed, a writer in flight, and
 * a shared mapping of the file made before the attach or while its sync
 * slept. */
static void error_rows(void)
{
    int row;

    for (row = 0; row < 7; row++) {
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
        } else if (row == 4) {
            busy = 1;
        } else if (row == 5) {
            mapped = 1;
        } else {
            map_in_sync = 1;
        }
        check(loop_attach(0, "/image", 1) == expected,
              "attachment returns the failing step's error");
        check(!loops[0].used && !depth && !opened && !refs && !active,
              "failed attachment releases locks and references");
    }
}

/** Exercise relocation, reuse and attachment cleanup. */
/* The loop ioctls copy a status block; no row here issues one. */
int copy_from_user(void *dst, const void __user *src, size_t n)
{
    (void)dst;
    (void)src;
    (void)n;
    return -1;
}

int copy_to_user(void __user *dst, const void *src, size_t n)
{
    (void)dst;
    (void)src;
    (void)n;
    return -1;
}

int main(void)
{
    relocation_row();
    error_rows();
    if (!failures) {
        printf("loop-attach-hosttest: result: pass: relocation, reuse and cleanup\n");
    }
    return failures != 0;
}
