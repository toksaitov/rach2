/**
 * The KRFFS root's data operations under the mount's lock, and the order the
 * lock is taken in.
 *
 * `tests/host/krffs_root_lock_mutate.sh` extracts from `rach2/sys/fs/vfs.c`,
 * unchanged, the root adapter's five data operations, `kfs_read`,
 * `kfs_write`, `kfs_truncate`, `kfs_fsync` and `kfs_readdir`, the bodies they
 * call, and the namespace lock they take, and compiles them here over the
 * real KRFFS core, with the thread glue of `krffs_open_threads.c`. In the
 * kernel each body reaches the image through the page cache's window, where
 * a copy can fault and the fill sleeps in the disk driver. The five took no
 * lock while that driver polled; once it slept, a second process entered the
 * ring and grew a file of its own under the first one's write, and the first
 * went on writing at the old place (entry 104 of milestone 50).
 *
 * The first rows count every access a body makes to the image: through the
 * core's backend for a read, a write and a truncate, at `pc_sync` for an
 * fsync, and at each step of the walk for a readdir. At every one of them the
 * caller must hold the mount's lock, and when the operation returns the lock
 * must be free again.
 *
 * The last row is the lock's line. A holder lets the lock go while another
 * process waits for it and asks for it again at once, which is what the
 * self-build's writer did for every module it filled: the waiter must have
 * the lock before the holder has it back, or it waits for as long as the
 * writer writes. Two host threads stand for the two processes, and one runs
 * at a time, as in the kernel, where a process in a system call gives way
 * only where it sleeps: the waiter runs until it sleeps in the lock's wait,
 * and the holder runs until it waits for its own turn, where the waiter is
 * resumed and runs to its end.
 */
#include "fs/krffs.c"
#include "fs/vfs.h"
#include "proc/process.h"

extern int printf(const char *format, ...);
extern void exit(int status);
extern void open_thread_start(void (*fn)(void));
extern void open_thread_wait(void);
extern void open_thread_finish(void);

int pc_sync(void);

#define IMAGE_BYTES 65536u

static uint8_t image[IMAGE_BYTES];
static struct vfs_mount mount;
static process_t writer, waiter;
struct percpu cpu_table[NR_CPUS];
static int failures;
static const char *under_test;
static int reached, unlocked, repoints;
/* The line row: whether it runs, how often the waiter slept in the lock's
 * wait and whether it sleeps there now, and who took the lock, in order. */
static int line_row_runs, waiter_sleeps, waiter_asleep;
static char taken[3];
static int ntaken;

/** Record a failed row, named by the operation it is about. */
static void check_op(int ok, const char *op, const char *what)
{
    if (!ok) {
        printf("krffs-root-lock: fail: %s %s\n", op, what);
        failures++;
    }
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

/**
 * Count one access to the image by the operation under test, and whether
 * its caller held the mount's lock at that moment.
 */
static void at_image(void)
{
    if (!under_test) {
        return;
    }
    reached++;
    if (mount.ns_depth <= 0 || mount.ns_holder != (int)current_proc->pid) {
        unlocked++;
    }
}

/** Read the image as the core's memory backend does, and count the access. */
static int watched_read(void *ctx, uint64_t offset, void *buffer, uint32_t size)
{
    at_image();
    return krffs_memory_read(ctx, offset, buffer, size);
}

/** Write the image as the core's memory backend does, and count the access. */
static int watched_write(void *ctx, uint64_t offset, const void *buffer, uint32_t size)
{
    at_image();
    return krffs_memory_write(ctx, offset, buffer, size);
}

/** A sync point of the core, where the kernel's writes wait for the disk. */
static int watched_sync(void *ctx)
{
    at_image();
    return krffs_memory_sync(ctx);
}

static const krffs_backend_t watched_backend = {watched_read, watched_write, watched_sync};

/** The page cache's flush, where an fsync of the root waits for the disk. */
int pc_sync(void)
{
    at_image();
    return 0;
}

/** A write that moved the file repoints its holders, under the same lock. */
void vfs_repoint(struct vfs_mount *mnt, const void *from, void *to)
{
    (void)from;
    (void)to;
    repoints++;
    check_op(mnt->ns_depth > 0 && mnt->ns_holder == (int)current_proc->pid, "kfs_write",
             "repoints a moved file's holders under the mount's lock");
}

/**
 * Sleep in the lock's wait.
 *
 * Outside the line row nobody else asks for the lock, so nobody may wait for
 * it. In the line row the waiter sleeps once, and the thread that stands for
 * it stops here until the holder resumes it; the holder waits only for its
 * own turn, behind the waiter, and its sleep is where the waiter runs.
 */
int wq_wait_event_nointr(wait_queue_t *wq, wq_cond_fn cond, void *arg)
{
    (void)wq;
    if (!line_row_runs) {
        printf("krffs-root-lock: fail: a caller waited for a lock nobody else held\n");
        exit(2);
    }
    if (current_proc == &waiter) {
        if (++waiter_sleeps > 1) {
            printf("krffs-root-lock: fail: the waiter was woken to a lock it could not take\n");
            exit(2);
        }
        waiter_asleep = 1;
        open_thread_wait();
        return 0;
    }
    if (!waiter_asleep) {
        printf("krffs-root-lock: fail: the holder waited with nobody ahead of it\n");
        exit(2);
    }
    waiter_asleep = 0;
    current_proc = &waiter;
    open_thread_finish();
    current_proc = &writer;
    check_op(cond(arg), "vfs_ns_lock", "gives the holder its turn once the waiter let the lock go");
    return 0;
}

/** Waking the line is a no-op: a sleeper here is resumed by its thread. */
void wq_wake_all(wait_queue_t *wq)
{
    (void)wq;
}

/**
 * A step of the readdir walk, which reads the next node's header.
 *
 * The core's own step folds the address through the kernel's 32-bit
 * `uintptr_t`, which on a 64-bit host drops the upper half of the image's
 * address, so this one steps by offset.
 */
static krffs_node_t *watched_next_node(krffs_file_system_t *fs, krffs_node_t *node)
{
    uint64_t next = krffs_offset_of(node) + node->size;

    at_image();
    return krffs_node_of(next >= fs->size ? next - fs->size : next);
}

#define krffs_get_next_node watched_next_node
#include "kfs_data.inc"
#undef krffs_get_next_node

/**
 * Lay a fresh file system into the image, mount it as the root, and put
 * two files into it, `alpha` and then `beta` right after it.
 */
static void setup(void)
{
    krffs_node_t *root = (krffs_node_t *)image;
    krffs_node_t *free_node = (krffs_node_t *)(image + sizeof(krffs_node_t));

    root->magic = KRFFS_FILE_SYSTEM_MAGIC;
    root->type = KRFFS_ROOT_NODE;
    root->id = KRFFS_ROOT_ID;
    root->mode = (uint16_t)(KRFFS_S_IFDIR | 0755);
    root->size = sizeof(krffs_node_t);
    root->previous_node_size = IMAGE_BYTES - sizeof(krffs_node_t);
    free_node->magic = KRFFS_FILE_SYSTEM_MAGIC;
    free_node->type = KRFFS_FREE_NODE;
    free_node->size = IMAGE_BYTES - sizeof(krffs_node_t);
    free_node->previous_node_size = sizeof(krffs_node_t);
    if (init_krffs(image, IMAGE_BYTES) != 0) {
        printf("krffs-root-lock: fail: the image does not mount\n");
        exit(2);
    }
    krffs_root_mount.backend = &watched_backend;
    if (krffs_create("/alpha") != 0 || krffs_write("/alpha", "AAAAAAAA", 8, 0) != 8 ||
        krffs_create("/beta") != 0 || krffs_write("/beta", "BBBBBBBB", 8, 0) != 8) {
        printf("krffs-root-lock: fail: the two files are not made\n");
        exit(2);
    }
    writer.pid = 1;
    waiter.pid = 2;
    current_proc = &writer;
    mount.ns_holder = -1;
}

/** Watch the image for the operation `op`. */
static void watch(const char *op)
{
    under_test = op;
    reached = unlocked = 0;
}

/** Stop watching, and judge what the operation did while it was watched. */
static void judge(const char *op)
{
    under_test = NULL;
    check_op(reached > 0, op, "reaches the image");
    check_op(unlocked == 0, op, "holds the mount's lock wherever it reaches the image");
    check_op(mount.ns_depth == 0 && mount.ns_holder == -1, op,
             "leaves the mount's lock free when it returns");
}

/**
 * Run each data operation once, watched, on the files `setup` made. Each
 * is judged as soon as it returns, before anything else reads the image.
 */
static void data_rows(void)
{
    krffs_node_t *alpha;
    struct vfs_dirent ent;
    uint32_t cursor = 0;
    char bytes[8];
    int32_t rc;

    alpha = krffs_resolve("/alpha");
    watch("kfs_read");
    rc = kfs_read(&mount, alpha, bytes, sizeof(bytes), 0);
    judge("kfs_read");
    check_op(rc == 8 && bytes[7] == 'A', "kfs_read", "reads the file's bytes");
    watch("kfs_write");
    rc = kfs_write(&mount, alpha, "C", 1, 8192);
    judge("kfs_write");
    check_op(rc == 1 && repoints == 1, "kfs_write",
             "grows the file past its neighbour and moves it");
    alpha = krffs_resolve("/alpha");
    watch("kfs_truncate");
    rc = kfs_truncate(&mount, alpha, 16);
    judge("kfs_truncate");
    alpha = krffs_resolve("/alpha");
    check_op(rc == 0 && alpha && alpha->data_size == 16, "kfs_truncate", "shortens the file");
    watch("kfs_fsync");
    rc = kfs_fsync(&mount, alpha);
    judge("kfs_fsync");
    check_op(rc == 0, "kfs_fsync", "flushes the file");
    watch("kfs_readdir");
    rc = kfs_readdir(&mount, krffs_fs->node, &cursor, &ent);
    judge("kfs_readdir");
    check_op(rc == 1, "kfs_readdir", "lists an entry of the root");
}

/** Note who has just taken the lock. */
static void took(char who)
{
    if (ntaken < (int)sizeof(taken)) {
        taken[ntaken++] = who;
    }
}

/** The waiting process: take the lock, note it, and let it go. */
static void wait_for_lock(void)
{
    vfs_ns_lock(&mount);
    took('W');
    vfs_ns_unlock(&mount);
}

/**
 * The holder lets the lock go while the waiter sleeps for it, and asks for
 * it again at once. The waiter must take it first.
 */
static void line_row(void)
{
    line_row_runs = 1;
    current_proc = &writer;
    vfs_ns_lock(&mount);
    current_proc = &waiter;
    open_thread_start(wait_for_lock);
    current_proc = &writer;
    check_op(waiter_asleep && ntaken == 0, "vfs_ns_lock",
             "keeps a second process waiting while the lock is held");
    vfs_ns_unlock(&mount);
    vfs_ns_lock(&mount);
    took('H');
    vfs_ns_unlock(&mount);
    if (waiter_asleep) {
        waiter_asleep = 0;
        current_proc = &waiter;
        open_thread_finish();
        current_proc = &writer;
    }
    check_op(ntaken == 2 && taken[0] == 'W' && taken[1] == 'H', "vfs_ns_lock",
             "serves a waiter before a holder that let the lock go and asked again");
    check_op(mount.ns_depth == 0 && mount.ns_holder == -1, "vfs_ns_lock",
             "leaves the lock free when both are done");
    line_row_runs = 0;
}

int main(void)
{
    setup();
    data_rows();
    line_row();
    if (!failures) {
        printf("krffs-root-lock: result: pass: the five data operations hold the mount's lock, "
               "taken in the order it was asked for\n");
    }
    return failures != 0;
}
