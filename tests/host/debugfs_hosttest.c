/* debugfs's tree, its VFS operations, and a software fence made, signalled
 * and read through sync/sw_sync and sync/info.
 * host-deps: rach2/sys/lib/kstring.c rach2/sys/lib/kctypes.c
 * host-deps: tests/host/compat/kheap_host.c
 * host-deps: tests/host/compat/fd_flat.c
 * host-deps: tests/host/compat/spinlock_host.c tests/host/compat/atomic_host.c
 * host-deps: tests/host/compat/mutex_none.c
 */
#include "fs/debugfs.c"
#include "dmabuf/fence.c"
#include "dmabuf/dmabuf.c"
#include "dmabuf/sync_file.c"
#include "dmabuf/sw_sync.c"

/** No interrupts on the host: the fences' masking is a no-op. */
uint32_t kirq_save(void)
{
    return 0;
}

/** The pair of kirq_save. */
void kirq_restore(uint32_t flags)
{
    (void)flags;
}

int printf(const char *format, ...);

struct percpu cpu_table[NR_CPUS];
static process_t self;
static fd_table_t table;
static fd_entry_t slots[64];
static struct open_file descriptions[64];
static int failures;

#define CHECK(c, s)                                \
    do {                                           \
        if (!(c)) {                                \
            printf("debugfs-test: fail: %s\n", s); \
            failures++;                            \
        }                                          \
    } while (0)

/** A fixed wall clock for the nodes' times. */
void get_current_time(int64_t *seconds, int64_t *nanoseconds)
{
    *seconds = 1700000000;
    *nanoseconds = 5;
}
/** A fixed monotonic clock for the fences' stamps. */
void rtc_monotonic(int64_t *sec, int64_t *ns)
{
    *sec = 1;
    *ns = 0;
}
/** A fixed tick count. */
uint64_t rtc_ticks_now(void)
{
    return 1000;
}
/** Host arithmetic for the kernel's division helper. */
uint64_t tick_div(uint64_t n, uint32_t d, uint32_t *rem)
{
    if (rem)
        *rem = (uint32_t)(n % d);
    return n / d;
}
/** Initialize a model queue. */
void wait_queue_init(wait_queue_t *wq)
{
    k_memset(wq, 0, sizeof(*wq));
}
/** Wakes need no scheduler here. */
void wq_wake(wait_queue_t *wq, uint32_t kinds)
{
    (void)wq;
    (void)kinds;
}
/** Wake every modeled kind. */
void wq_wake_all(wait_queue_t *wq)
{
    wq_wake(wq, WQ_ALL);
}
/** No case here waits. */
int wq_wait_event(wait_queue_t *wq, wq_cond_fn cond, void *arg)
{
    (void)wq;
    CHECK(cond(arg), "no case waits");
    return 0;
}
/** No case here waits. */
int wq_wait_event_nointr(wait_queue_t *wq, wq_cond_fn cond, void *arg)
{
    return wq_wait_event(wq, cond, arg);
}
/** Copy from a real test address, refusing the model's invalid page. */
int copy_from_user(void *dst, const void __user *src, size_t n)
{
    if ((uintptr_t)src < 4096)
        return -EFAULT;
    k_memcpy(dst, src, n);
    return 0;
}
/** Copy to a real test address. */
int copy_to_user(void __user *dst, const void *src, size_t n)
{
    if ((uintptr_t)dst < 4096)
        return -EFAULT;
    k_memcpy(dst, src, n);
    return 0;
}
/** Allocate a descriptor in the model's bounded table. */
int fd_alloc(fd_table_t *t)
{
    int i;
    for (i = 0; i < 64; i++)
        if (t->chunks[0][i].type == FD_TYPE_NONE) {
            k_memset(&descriptions[i], 0, sizeof(descriptions[i]));
            t->chunks[0][i].ofd = &descriptions[i];
            t->chunks[0][i].type = FD_TYPE_ANON;
            return i;
        }
    return -EMFILE;
}
/** Find a live model descriptor. */
fd_entry_t *fd_get(fd_table_t *t, int fd)
{
    return fd >= 0 && fd < 64 && t->chunks[0][fd].type != FD_TYPE_NONE ? &t->chunks[0][fd] : NULL;
}
/** Release the object attached to a model descriptor. */
int fd_free(fd_table_t *t, int fd)
{
    fd_entry_t *e = fd_get(t, fd);
    if (!e)
        return -EBADF;
    if (e->ops && e->ops->close)
        e->ops->close(e);
    k_memset(e, 0, sizeof(*e));
    return 0;
}

/** Look `name` up in `dir` of the mount, held, or NULL. */
static struct debugfs_node *look(struct vfs_mount *m, void *dir, const char *name)
{
    void *n = NULL;
    return dfs_lookup(m, dir, name, &n) == 0 ? n : NULL;
}

/** Open the file `n` as the system call layer does: a descriptor, then the
 * file's operations bound to it. The descriptor, or -errno. */
static int open_node(struct debugfs_node *n, int flags)
{
    int fd = fd_alloc(&table);
    int32_t rc;
    if (fd < 0)
        return fd;
    slots[fd].type = FD_TYPE_FILE;
    rc = debugfs_bind(n, &slots[fd], flags);
    if (rc != 0) {
        fd_free(&table, fd);
        return rc;
    }
    return fd;
}

/** Read the whole of `fd` into `buf`, NUL-terminated; the bytes read. */
static int read_all(int fd, char *buf, int cap)
{
    fd_entry_t *e = fd_get(&table, fd);
    int total = 0, n;
    while ((n = e->ops->read(e, buf + total, 7)) > 0 && total + 7 < cap)
        total += n;
    buf[total] = '\0';
    return total;
}

/** Whether the strings are equal. */
static int same(const char *a, const char *b)
{
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

/** Collect the names `dir` lists, each after a space. */
static void list(struct vfs_mount *m, void *dir, char *out, int cap)
{
    struct vfs_dirent ent;
    uint32_t cursor = 0;
    int len = 0;
    out[0] = '\0';
    while (dfs_readdir(m, dir, &cursor, &ent) == 1) {
        int n = (int)k_strlen(ent.name);
        if (len + n + 2 >= cap)
            break;
        out[len++] = ' ';
        k_memcpy(out + len, ent.name, (size_t)n + 1);
        len += n;
    }
}

/** The mount, the tree it shows and the attributes Linux gives them. */
static void tree_cases(struct vfs_mount *m)
{
    struct debugfs_node *sync, *sw, *info, *up;
    struct vfs_stat st;
    struct vfs_statfs sf;
    char text[128];
    CHECK(debugfs_mount(m, NULL, 0, NULL) == 0 && m->root == &root, "mount shows the root");
    CHECK(debugfs_mount(m, NULL, 0, "uid=0,gid=0,mode=700") == 0, "second mount, one tree");
    dfs_getattr(m, m->root, &st);
    CHECK(st.mode == (DFS_IFDIR | 0700u) && st.uid == 0 && st.ino == 1, "root is root's, 0700");
    CHECK(st.nlink == 3, "root links: itself, its name and sync's ..");
    sync = look(m, NULL, "sync");
    CHECK(sync && is_dir(sync), "sync is a directory");
    if (!sync)
        return;
    dfs_getattr(m, sync, &st);
    CHECK(st.mode == (DFS_IFDIR | 0755u) && st.mtime_sec == 1700000000, "sync is 0755");
    sw = look(m, sync, "sw_sync");
    info = look(m, sync, "info");
    CHECK(sw && info, "sync holds sw_sync and info");
    if (!sw || !info)
        return;
    dfs_getattr(m, sw, &st);
    CHECK(st.mode == (DFS_IFREG | 0644u) && st.size == 0 && st.nlink == 1, "sw_sync is 0644");
    dfs_getattr(m, info, &st);
    CHECK(st.mode == (DFS_IFREG | 0444u), "info is 0444");
    CHECK(look(m, sync, "nothing") == NULL, "no such name");
    CHECK(dfs_lookup(m, sw, "x", (void **)&up) == -ENOTDIR, "a file has no names");
    up = look(m, sync, "..");
    CHECK(up == &root, ".. of sync is the root");
    dfs_put(m, up);
    up = look(m, NULL, "..");
    CHECK(up == &root, ".. of the root is the root");
    dfs_put(m, up);
    list(m, NULL, text, sizeof(text));
    CHECK(same(text, " . .. sync"), "the root lists sync");
    list(m, sync, text, sizeof(text));
    CHECK(same(text, " . .. sw_sync info"), "sync lists its files in order");
    CHECK(dfs_path(m, sync, text, sizeof(text)) == 0 && same(text, "/sync"), "sync's path");
    CHECK(dfs_path(m, &root, text, sizeof(text)) == 0 && same(text, "/"), "the root's path");
    CHECK(dfs_path(m, sw, text, sizeof(text)) == -ENOTDIR, "a file has no directory path");
    CHECK(dfs_statfs(m, &sf) == 0 && sf.magic == 0x64626720u, "DEBUGFS_MAGIC");
    dfs_put(m, sw);
    dfs_put(m, info);
    dfs_put(m, sync);
}

static const struct file_ops none_ops = {NULL, NULL, NULL, NULL, NULL, NULL, NULL, 0};
static int binds;

/** An open hook that counts and refuses its second call. */
static int32_t refuse_second(fd_entry_t *e, int flags)
{
    (void)flags;
    CHECK(e->priv == &binds, "the open sees the registrant's data");
    return ++binds == 2 ? -EACCES : 0;
}

/** Registration's errors, a refused open, and removal under a hold. */
static void register_cases(struct vfs_mount *m)
{
    struct debugfs_node *dri, *card, *name;
    int fd;
    CHECK(debugfs_create_file("", 0444, &none_ops, NULL, NULL) == -EINVAL, "empty path");
    CHECK(debugfs_create_file("a/../b", 0444, &none_ops, NULL, NULL) == -EINVAL, "dot-dot");
    CHECK(debugfs_create_file("a", 0444, NULL, NULL, NULL) == -EINVAL, "no operations");
    CHECK(debugfs_create_file("sync/info", 0444, &none_ops, NULL, NULL) == -EEXIST, "taken");
    CHECK(debugfs_create_file("sync/info/x", 0444, &none_ops, NULL, NULL) == -ENOTDIR,
          "a file on the way");
    CHECK(debugfs_create_file("/dri/0/name", 0444, &none_ops, refuse_second, &binds) == 0,
          "directories come with a file");
    dri = look(m, NULL, "dri");
    card = dri ? look(m, dri, "0") : NULL;
    name = card ? look(m, card, "name") : NULL;
    CHECK(name != NULL, "dri/0/name is there");
    if (!name)
        return;
    fd = open_node(name, 0);
    CHECK(fd >= 0 && slots[fd].ops == &none_ops && slots[fd].priv == &binds, "open binds");
    CHECK(open_node(name, 0) == -EACCES, "an open the file refuses");
    debugfs_remove("dri");
    CHECK(look(m, NULL, "dri") == NULL, "removed at once");
    CHECK(name->removed && dfs_lookup(m, card, "name", (void **)&name) == -ENOENT,
          "a removed directory has no names");
    fd_free(&table, fd);
    dfs_put(m, name);
    dfs_put(m, card);
    dfs_put(m, dri);
}

/** A fence made, signalled and shown through the files. */
static void sync_cases(struct vfs_mount *m)
{
    struct debugfs_node *sync = look(m, NULL, "sync");
    struct debugfs_node *sw = sync ? look(m, sync, "sw_sync") : NULL;
    struct debugfs_node *info = sync ? look(m, sync, "info") : NULL;
    struct sw_sync_create_fence_data data;
    struct sync_file_info fi;
    struct sync_fence_info row;
    struct dma_fence *f;
    char text[512];
    uint32_t step = 1;
    int tl, rd, first, other;
    if (!sw || !info) {
        CHECK(0, "the sync files are there");
        return;
    }
    tl = open_node(sw, 2);
    CHECK(tl >= 0 && slots[tl].ops == &sw_file_ops, "an open of sw_sync is a timeline");
    k_memset(&data, 0, sizeof(data));
    data.value = 2;
    k_memcpy(data.name, "point", 6);
    CHECK(slots[tl].ops->ioctl(&slots[tl], SW_SYNC_IOC_CREATE_FENCE, (uintptr_t)&data) == 0,
          "CREATE_FENCE through the file");
    first = data.fence;
    f = sync_file_get_fence(first);
    CHECK(f && dma_fence_status(f) == 0, "the fence is active");
    data.value = 1;
    slots[tl].ops->ioctl(&slots[tl], SW_SYNC_IOC_CREATE_FENCE, (uintptr_t)&data);
    other = data.fence;
    k_memset(&fi, 0, sizeof(fi));
    fi.num_fences = 1;
    fi.sync_fence_info = (uint64_t)(uintptr_t)&row;
    CHECK(sync_ioctl(fd_get(&table, other), SYNC_IOC_FILE_INFO, (uintptr_t)&fi) == 0 &&
              same(row.obj_name, "debugfs-test") && same(row.driver_name, "sw_sync"),
          "the timeline has its opener's name");
    rd = open_node(info, 0);
    read_all(rd, text, sizeof(text));
    CHECK(same(text, "objs:\n--------------\n"
                     "debugfs-test: 0\n"
                     "  fence active: 1 / 0\n"
                     "  fence active: 2 / 0\n"
                     "\n"
                     "fences:\n--------------\n"),
          "sync/info lists the timeline and its points in order");
    CHECK(slots[rd].ops->llseek(&slots[rd], 0, 0) == 0, "info seeks to its start");
    fd_free(&table, rd);
    slots[tl].ops->ioctl(&slots[tl], SW_SYNC_IOC_INC, (uintptr_t)&step);
    CHECK(dma_fence_status(f) == 0, "one step leaves point 2 active");
    slots[tl].ops->ioctl(&slots[tl], SW_SYNC_IOC_INC, (uintptr_t)&step);
    CHECK(dma_fence_status(f) == 1, "two steps signal point 2");
    rd = open_node(info, 0);
    read_all(rd, text, sizeof(text));
    CHECK(same(text, "objs:\n--------------\ndebugfs-test: 2\n\nfences:\n--------------\n"),
          "a reached point leaves sync/info");
    fd_free(&table, rd);
    fd_free(&table, tl);
    rd = open_node(info, 0);
    read_all(rd, text, sizeof(text));
    CHECK(same(text, "objs:\n--------------\nfences:\n--------------\n"),
          "a closed timeline leaves sync/info");
    fd_free(&table, rd);
    fd_free(&table, first);
    fd_free(&table, other);
    dma_fence_put(f);
    dfs_put(m, sw);
    dfs_put(m, info);
    dfs_put(m, sync);
}

/** Run the cases and fail on any check. */
int main(void)
{
    struct vfs_mount m;
    int i;
    current_proc = &self;
    k_memcpy(self.comm, "debugfs-test", 13);
    table.chunks[0] = slots;
    table.cap = 64;
    self.fds = &table;
    k_memset(&m, 0, sizeof(m));
    tree_cases(&m);
    register_cases(&m);
    sync_cases(&m);
    for (i = 0; i < 64; i++)
        CHECK(slots[i].type == FD_TYPE_NONE, "all descriptors closed");
    if (!failures)
        printf("debugfs-test: passed\n");
    return failures != 0;
}
