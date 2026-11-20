/* Check the inode size and saved record of a pstore file after truncation.
 * host-deps: rach2/sys/lib/kstring.c rach2/sys/lib/kctypes.c
 * host-deps: tests/host/compat/kheap_host.c rach2/sys/lib/kstd.c
 */
static void pstore_start(void);
#include "pstore/inode.c"

extern int printf(const char *, ...);
static int64_t clock_sec = 12;

static struct vfs_mount shared;

/** Install the shared superblock for the first mount. */
struct vfs_mount *vfs_kernel_mount(const struct vfs_type *t, void *node, uint32_t flags)
{
    shared.type = t;
    shared.root = node;
    shared.flags = flags;
    shared.refs = 1;
    return &shared;
}

/** Count another view of the shared superblock. */
void vfs_mount_get(struct vfs_mount *mnt)
{
    mnt->refs++;
}

/** Apply the superblock flags after a successful reconfiguration. */
int vfs_remount(struct vfs_mount *mnt, uint32_t flags, const char *options)
{
    int rc = mnt->type->ops->remount(mnt, flags, options);

    if (rc == 0)
        mnt->flags = flags;
    return rc;
}

/** The test supplies the bytes in memory. */
void ramoops_erase(int type, uint32_t id)
{
    (void)type;
    (void)id;
}

/** Give each size change the row's clock. */
void rtc_coarse_realtime(int64_t *sec, int64_t *ns)
{
    *sec = clock_sec;
    *ns = 0;
}

/** Record a nondefault dump threshold reported by the shared superblock. */
static void option_value(void *out, const char *name, uint64_t value, enum vfs_option_format fmt)
{
    *(int *)out = same(name, "kmsg_bytes") && fmt == VFS_OPT_DEC ? (int)value : -2;
}

/** Remount the shared superblock and check its rendered dump threshold. */
static int mount_options(void)
{
    struct vfs_mount first = {0}, second = {0};
    int ok, shown = -1;

    ok = pstore_mount(&first, NULL, 0, "kmsg_bytes=32") == 0 && kmsg_bytes == 32 &&
         pstore_mount(&second, NULL, VFS_MS_RDONLY, NULL) == 0 && first.bind_of == second.bind_of &&
         first.bind_of->flags == VFS_MS_RDONLY && first.bind_of->refs == 2;
    if (ok) {
        first.bind_of->show_options(first.bind_of, option_value, &shown);
        ok = shown == 32 && ps_remount(first.bind_of, 0, "kmsg_bytes=0,kmsg_bytes=bad") == 0 &&
             !pstore_dump_enabled() && ps_remount(first.bind_of, 0, "kmsg_bytes=10240") == 0 &&
             pstore_dump_enabled();
        shown = -1;
        second.bind_of->show_options(second.bind_of, option_value, &shown);
        ok = ok && shown == -1;
    }
    return ok;
}

/** Resize the inode by path and by descriptor and read the saved bytes. */
int main(void)
{
    struct pstore_file f = {0};
    struct vfs_stat st;
    char result[4];
    int ok;

    f.buf = "log";
    f.size = 3;
    f.inode_size = 3;
    f.time_s = f.ctime_s = 4;
    ok = ps_truncate(NULL, &f, 9, 0) == 0 && ps_getattr(NULL, &f, &st) == 0 && st.size == 9 &&
         st.mtime_sec == 4 && st.ctime_sec == 4;
    ok = ok && ps_truncate(NULL, &f, 0, 1) == 0;
    clock_sec++;
    ok = ok && ps_truncate(NULL, &f, ~(uint64_t)0, 1) == -EFBIG &&
         ps_truncate(NULL, &root, 0, 1) == -EISDIR;
    ok = ok && ps_getattr(NULL, &f, &st) == 0 && st.size == 0 && st.mtime_sec == 12 &&
         st.ctime_sec == 12 && ps_read(NULL, &f, result, sizeof(result), 0) == 3 &&
         __builtin_memcmp(result, "log", 3) == 0;
    ok = ok && mount_options();
    printf("pstore-inode: result: %s\n", ok ? "pass" : "fail");
    return !ok;
}
