/**
 * Check proc stat generation when every text allocation fails, the CPU time
 * of a process's stat and of a thread's, the descriptor links of a process,
 * which a closed descriptor has none of, the ids a path under a process's
 * task directory needs, and the counts a /proc/PID/mem descriptor takes of
 * the address space: a hold on its record, and a user's count for a read
 * only while a user remains.
 *
 * host-deps: rach2/sys/lib/kstring.c rach2/sys/lib/kctypes.c
 * host-cflags: -Wno-int-to-pointer-cast -Wno-pointer-to-int-cast
 *
 * Unused exports are private so the compiler removes unrelated generators.
 */
#include "lib/kint.h"
struct vpath;
struct fd_entry;
static int procfs_resolve_pid(int pid);
static int procfs_resolve_id(const struct vpath *vp);
static int procfs_open(const struct vpath *vp, int flags);
static int procfs_readlink_fd(int pid, int fd, char *buf, int cap);
static int procfs_readlink_exe(int pid, char *buf, int cap);
static int procfs_nth_pid(int n);
static uint32_t procfs_ino_dir(void);
static uint32_t procfs_ino_cmdline(void);
static uint32_t procfs_ino_netdir(void);
static uint32_t procfs_ino_netdev(void);
static uint32_t procfs_ino_disk(void);
static uint32_t procfs_ino_uptime(void);
static uint32_t procfs_ino_statsys(void);
static uint32_t procfs_ino_meminfo(void);
static uint32_t procfs_ino_mounts(void);
static uint32_t procfs_ino_usb(void);
static uint32_t procfs_ino_audio(void);
static uint32_t procfs_ino_loop(void);
static uint32_t procfs_ino_interrupts(void);
static uint32_t procfs_ino_console(void);
static uint32_t procfs_ino_filesystems(void);
static uint32_t procfs_ino_netroute(void);
static uint32_t procfs_ino_netarp(void);
static uint32_t procfs_ino_nettcp(void);
static uint32_t procfs_fd_ino(struct fd_entry *e);
static uint32_t procfs_ino_pid(int pid);
static uint32_t procfs_task_links(int pid);
#include "fs/procfs.c"

extern int printf(const char *format, ...);
extern int sscanf(const char *str, const char *format, ...);
extern void *malloc(unsigned long size);
extern void free(void *ptr);

static process_t process, thread, stranger;
static struct tgroup group, other_group;
process_t *current_proc = &process;
static fd_table_t table;
static fd_entry_t entries[1];
static int refuse, allocations, failures;
/* Process 7's address space in the /proc/7/mem rows, the description of
 * their descriptor, and the reads that reached the memory. */
static struct mm space;
static struct open_file mem_ofd;
static int mem_reads;
page_dir_t *_kernel_dir;
page_dir_t *_curr_dir;

/** Record a failed invariant. */
static void check(int ok, const char *what)
{
    if (!ok) {
        printf("procfs-hosttest: fail: %s\n", what);
        failures++;
    }
}

/** Return the process whose stat file is generated, 7, a thread of it, 9,
 * and a process of another group, 11. */
process_t *get_process(uint32_t pid)
{
    if (pid == 9) {
        return &thread;
    }
    if (pid == 11) {
        return &stranger;
    }
    return pid == 7 ? &process : NULL;
}

/** The whole process's time, its two threads' together. */
void proc_cpu_time(const process_t *p, uint32_t *user, uint32_t *sys)
{
    (void)p;
    *user = process.utime + thread.utime;
    *sys = process.stime + thread.stime;
}

/** Fail every text allocation when requested. */
void *kmalloc(size_t size)
{
    allocations++;
    return refuse ? NULL : malloc(size);
}

/** Release the snapshot's host memory. */
void kfree(void *ptr)
{
    free(ptr);
}

void *kzalloc(size_t size)
{
    void *p = malloc(size);

    if (p) {
        k_memset(p, 0, size);
    }
    return p;
}

/** The one descriptor of the /proc/7/mem rows. */
int fd_alloc(fd_table_t *t)
{
    (void)t;
    return 0;
}

/* The counts of an address space, as mm.c keeps them. */
void mm_hold(struct mm *mm)
{
    mm->holds++;
}

void mm_unhold(struct mm *mm)
{
    if (mm) {
        mm->holds--;
    }
}

int mm_get_live(struct mm *mm)
{
    if (mm->users == 0) {
        return 0;
    }
    mm->users++;
    return 1;
}

void mm_put(struct mm *mm)
{
    mm->users--;
}

void switch_page_directory(page_dir_t *dir)
{
    _curr_dir = dir;
}

/** Memory that reads as 'm', counting the reads that reach it. */
int paging_access(struct mm *mm, uint32_t addr, void *buf, uint32_t len, int write)
{
    (void)mm;
    (void)addr;
    (void)write;
    k_memset(buf, 'm', len);
    mem_reads++;
    return 0;
}

int copy_to_user(void __user *dst, const void *src, uint32_t n)
{
    k_memcpy((void __force *)dst, src, n);
    return 0;
}

int copy_from_user(void *dst, const void __user *src, uint32_t n)
{
    k_memcpy(dst, (const void __force *)src, n);
    return 0;
}

/** The descriptor table's size, as fd.c reports it. */
int fd_table_cap(const fd_table_t *t)
{
    return t ? t->cap : 0;
}

/** The link rows use a pipe end, which names no device, terminal, socket,
 * file or directory. */
const char *devfs_fd_name(struct fd_entry *e)
{
    (void)e;
    return "null";
}

int pty_slave_index(struct fd_entry *e)
{
    (void)e;
    return 0;
}

uint32_t unix_inode(const fd_entry_t *e)
{
    (void)e;
    return 0;
}

int krffs_node_path(krffs_node_t *node, char *buf, uint32_t size)
{
    (void)node;
    (void)buf;
    (void)size;
    return -1;
}

krffs_node_t *krffs_resolve(const char *path)
{
    (void)path;
    return NULL;
}

const struct vfs_type vfs_tmpfs_type = {0};

const char *tmpfs_label(void *node)
{
    (void)node;
    return NULL;
}

void synth_dir_path(int kind, int pid, int tid, char *out, int cap)
{
    (void)kind;
    (void)pid;
    (void)tid;
    if (cap > 0) {
        out[0] = '\0';
    }
}

/** Whether the stat line `line` has the user and system times `user` and
 * `sys`, its fields 14 and 15, the twelfth and thirteenth after the
 * command's closing parenthesis. */
static int stat_times(const char *line, unsigned user, unsigned sys)
{
    const char *close = __builtin_strrchr(line, ')');
    unsigned u, s;

    return close &&
           sscanf(close + 1, " %*s %*s %*s %*s %*s %*s %*s %*s %*s %*s %*s %u %u", &u, &s) == 2 &&
           u == user && s == sys;
}

/** A process's stat has the time of all its threads, and a thread's under
 * the process's task directory has its own. */
static void thread_times(void)
{
    struct procfs_text text;

    thread.state = PROC_READY;
    thread.tg = &group;
    k_memcpy(thread.comm, "test", 5);
    process.utime = 5;
    process.stime = 6;
    thread.utime = 30;
    thread.stime = 40;
    procfs_text_init(&text);
    check(procfs_gen_stat(&text, 7, 1) > 0 && stat_times(text.buf, 35, 46),
          "a process's stat has every thread's time");
    kfree(text.buf);
    procfs_text_init(&text);
    check(procfs_gen_stat(&text, 9, 0) > 0 && stat_times(text.buf, 30, 40),
          "a thread's stat has its own time");
    kfree(text.buf);
    procfs_text_init(&text);
    check(procfs_gen_stat(&text, 7, 0) > 0 && stat_times(text.buf, 5, 6),
          "the leader's stat under task has the leader's own time");
    kfree(text.buf);
    process.utime = process.stime = thread.utime = thread.stime = 0;
}

/** A path under a task directory needs its process and a thread of it,
 * which is what the path shows. */
static void task_ids(void)
{
    struct vpath vp;

    thread.state = PROC_READY;
    thread.tg = &group;
    stranger.state = PROC_READY;
    stranger.tg = &other_group;
    k_memset(&vp, 0, sizeof(vp));
    vp.pid = 7;
    check(procfs_resolve_id(&vp) == 7, "a process's own path is the process's");
    vp.tid = 9;
    check(procfs_resolve_id(&vp) == 9, "a thread's path under its process is the thread's");
    vp.tid = 11;
    check(procfs_resolve_id(&vp) == -1, "another process's thread is no task of this one");
    vp.pid = 99;
    vp.tid = 9;
    check(procfs_resolve_id(&vp) == -1, "a task path under no process names nothing");
    vp.pid = 7;
    thread.state = PROC_ZOMBIE;
    check(procfs_resolve_id(&vp) == -1, "an ended thread other than the leader is no task");
    thread.state = PROC_READY;
}

/** An open descriptor has its link, and one a close has marked, whose
 * release waits for another thread's call or is under way, has none. */
static void descriptor_links(void)
{
    char buf[32];

    process.fds = &table;
    table.entries = entries;
    table.cap = 1;
    entries[0].type = FD_TYPE_PIPE;
    entries[0].flags = FD_FLAG_READ;
    check(procfs_readlink_fd(7, 0, buf, (int)sizeof(buf)) == 8 &&
              __builtin_memcmp(buf, "pipe:[0]", 8) == 0,
          "an open descriptor has its link");
    entries[0].flags |= FD_FLAG_CLOSING;
    check(procfs_readlink_fd(7, 0, buf, (int)sizeof(buf)) == -ENOENT,
          "a closed descriptor has no link");
    entries[0].flags |= FD_FLAG_RELEASING;
    check(procfs_readlink_fd(7, 0, buf, (int)sizeof(buf)) == -ENOENT,
          "a descriptor being released has no link");
}

/**
 * A /proc/PID/mem descriptor holds the record of the address space and is no
 * user of it: its open takes a hold, a read takes a user's count for its
 * length while the process runs in the space, a read once the last user has
 * gone returns 0 and reaches no memory, and the close lets go of the hold.
 */
static void mem_descriptor(void)
{
    struct vpath vp;
    char buf[4];

    k_memset(&vp, 0, sizeof(vp));
    space.users = 1;
    space.holds = 1;
    space.dumpable = 1;
    process.mm = &space;
    process.fds = &table;
    table.entries = entries;
    table.cap = 1;
    k_memset(&entries[0], 0, sizeof(entries[0]));
    entries[0].ofd = &mem_ofd;
    check(procfs_open_mem(&vp, 7, 0) == 0 && space.users == 1 && space.holds == 2,
          "an open of /proc/PID/mem holds the record and is no user of it");
    check(procfs_mem_read(&entries[0], (char __user *)buf, sizeof(buf)) == 4 && mem_reads == 1 &&
              space.users == 1,
          "a read while the process runs is a user for its length");
    space.users = 0;
    check(procfs_mem_read(&entries[0], (char __user *)buf, sizeof(buf)) == 0 && mem_reads == 1,
          "a read once the process has ended returns 0 and reaches no memory");
    procfs_close(&entries[0]);
    check(space.holds == 1 && entries[0].priv == NULL, "the close lets go of the hold");
    process.mm = NULL;
}

/** Exercise empty and partially allocated text, then normal generation. */
int main(void)
{
    struct procfs_text text;
    int len;

    process.state = PROC_READY;
    process.tg = &group;
    group.leader = &process;
    group.live = 1;
    k_memcpy(process.comm, "test", 5);
    procfs_text_init(&text);
    refuse = 1;
    len = procfs_gen_stat(&text, 7, 1);
    check(len == 0 && text.nomem && !text.buf && allocations > 0,
          "stat allocation failure retains empty text for ENOMEM");
    refuse = 0;
    check(procfs_text_room(&text, 0) == 0, "allocate partial text");
    text.cap = 2;
    text.nomem = 0;
    refuse = 1;
    len = procfs_gen_stat(&text, 7, 1);
    check(len == 1 && text.nomem && text.buf[0] == '7',
          "stat growth failure retains partial text for disposal");
    kfree(text.buf);
    procfs_text_init(&text);
    refuse = 0;
    len = procfs_gen_stat(&text, 7, 1);
    check(len > 0 && !text.nomem && text.buf[len] == '\0' && text.buf[len - 1] == '\n',
          "stat success terminates the complete snapshot");
    check(__builtin_memcmp(text.buf, "7 (test) R ", 11) == 0, "stat fields retain their order");
    kfree(text.buf);
    thread_times();
    descriptor_links();
    task_ids();
    mem_descriptor();
    if (!failures) {
        printf("procfs-hosttest: result: pass: stat allocation failure and times, descriptor "
               "links, task ids and the mem descriptor's counts\n");
    }
    return failures != 0;
}
