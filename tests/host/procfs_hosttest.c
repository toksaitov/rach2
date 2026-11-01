/**
 * procfs_hosttest.c: the files rach2/sys/fs/procfs.c makes: the stat text
 * and its allocation, /proc/uptime, the CPU times of a process and of a
 * thread, task ids, descriptor links, /proc/PID/mem, /proc/PID/exe, the
 * paths of /proc/PID/maps, a read of a process's arguments while the process
 * is reaped, and oom_score_adj.
 *
 * host-deps: rach2/sys/fs/name.c
 * host-deps: rach2/sys/lib/kstring.c rach2/sys/lib/kctypes.c
 * host-deps: rach2/sys/tick.c
 * host-cflags: -Wno-int-to-pointer-cast -Wno-pointer-to-int-cast
 *
 * The test includes the source whole, with its exports declared static
 * first, so that the compiler drops those no row reaches and their calls
 * need no stubs. Process 7 is the process of most rows, 9 a thread of it, 11
 * a process of another group, and 13 the process reaped during a read.
 */
#include "lib/kint.h"
struct vpath;
struct fd_entry;
struct vfs_ref;
struct vfs_name;
static struct vfs_name *procfs_exe_entry(int pid);
static int procfs_resolve_pid(int pid);
static int procfs_resolve_id(const struct vpath *vp);
static int procfs_open(const struct vpath *vp, int flags);
static int procfs_readlink_fd(int pid, int fd, char *buf, int cap);
static int procfs_readlink_exe(int pid, char *buf, int cap);
static int procfs_exe_name(int pid, char *buf, int cap);
static int procfs_readlink_dir(int pid, int root, char *buf, int cap);
static int procfs_exe_file(int pid, struct vfs_ref *out);
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
static int procfs_oom_owner(int pid, uint32_t *uid, uint32_t *gid);
static int procfs_fd_owner(struct fd_entry *e, uint32_t *uid, uint32_t *gid);
static int procfs_fd_writable(struct fd_entry *e);
static uint32_t sysfs_ino(int node, int key, int file);
static int sysfs_device_key(int node, int n);
static int sysfs_device_exists(int node, int key);
static int sysfs_block_find(const char *name, int len, int disk);
static int sysfs_block_devno(const char *name, int len);
static void sysfs_block_name(int key, char *out, int cap);
static int sysfs_block_whole(int key);
static int sysfs_block_loop(int key);
#include "fs/procfs.c"
#include "arch/i686/mmu.h"
#include "ns_first.h"

/* The counters /proc/net/snmp reports, which net_core.c defines, and those
 * /proc/net/snmp6 reports, which ip6.c defines. */
struct net_mib net_mib;
struct net_mib6 net_mib6;

/** No interface is up here, so /sys/class/net is empty. */
int netdev_count(void)
{
    return 0;
}

/** Every interface is in the caller's network namespace. */
int netdev_seen_count(void)
{
    return netdev_count();
}

/** The attribute files of a /sys directory: none. */
const char *sysfs_attr(int node, int n)
{
    (void)node;
    (void)n;
    return 0;
}

/** No interface is up: NULL for every index. */
struct netdev *netdev_at(int n)
{
    (void)n;
    return 0;
}

/** No mount here is made from a node of /dev, so a mount's device is its
 * synthetic number (`procfs_mount_dev`), which compares the mount's type
 * with these. */
const struct vfs_type vfs_devfs_type, vfs_procfs_type, vfs_sysfs_type;

/** No path names a node of /dev. Returns 0. */
int devfs_numbers(const char *path, uint32_t *major, uint32_t *minor)
{
    (void)path;
    (void)major;
    (void)minor;
    return 0;
}

extern int printf(const char *format, ...);
extern int sscanf(const char *str, const char *format, ...);
extern void *malloc(unsigned long size);
extern void *calloc(unsigned long count, unsigned long size);
extern void free(void *ptr);

static process_t process, thread, stranger;
static struct tgroup group, other_group;
/* The directory the reader runs in, which is not the reaped process's. */
static page_dir_t reader_dir;
struct percpu cpu_table[NR_CPUS] = {{.current = &process, .space = &reader_dir}};
static fd_table_t table;
static fd_entry_t entries[1];
static int refuse, allocations, failures;
/* Process 7's address space in the /proc/7/mem rows, the description of
 * their descriptor, and the reads that reached the memory. */
static struct mm space;
static struct open_file mem_ofd;
static int mem_reads;
/* The process whose arguments `reaped_during_read` reads, 13, from the heap,
 * since its end and its reap free its record and its address space; NULL
 * once reaped. */
static process_t *reading;
static struct tgroup reading_group;
static int reaps;
page_dir_t *_kernel_dir;

/** Record a failed invariant. */
static void check(int ok, const char *what)
{
    if (!ok) {
        printf("procfs-hosttest: fail: %s\n", what);
        failures++;
    }
}

/** Return the process whose stat file is generated, 7, a thread of it, 9,
 * a process of another group, 11, and the process whose arguments are read
 * while it is reaped, 13. */
process_t *get_process(uint32_t pid)
{
    if (pid == 9) {
        return &thread;
    }
    if (pid == 11) {
        return &stranger;
    }
    if (pid == 13) {
        return reading;
    }
    return pid == 7 ? &process : NULL;
}

/** The process the PID `pid` names, its kernel id in the first PID
 * namespace. */
process_t *proc_named(uint32_t pid)
{
    return get_process(pid);
}

/** The size of an address space, which a stat line gives and no row
 *  reads: 0. */
uintptr_t vm_total(const vm_space_t *vm)
{
    (void)vm;
    return 0;
}

/** No user page is present: 0. */
uintptr_t paging_user_phys(page_dir_t *dir, uintptr_t va)
{
    (void)dir;
    (void)va;
    return 0;
}

/** The resident pages of an address space, which mm.c counts; none here. */
unsigned long mm_resident(const struct mm *mm)
{
    (void)mm;
    return 0;
}

/** The whole process's time, its two threads' together. */
void proc_cpu_time(const process_t *p, uint64_t *user, uint64_t *sys)
{
    (void)p;
    *user = process.utime + thread.utime;
    *sys = process.stime + thread.stime;
}

/** The host's allocation, counted in `allocations`, or NULL while
 *  `refuse` is set. */
void *kmalloc(size_t size)
{
    allocations++;
    return refuse ? NULL : malloc(size);
}

/** Free what `kmalloc` or `kzalloc` gave. */
void kfree(void *ptr)
{
    free(ptr);
}

/** The host's zeroed allocation, which never fails. */
void *kzalloc(size_t size)
{
    void *p = malloc(size);

    if (p) {
        k_memset(p, 0, size);
    }
    return p;
}

/* The error a row has the descriptor table answer, or 0. */
static int alloc_error;

/** The one descriptor of the /proc/7/mem rows, or the error a row set. */
int fd_alloc(fd_table_t *t)
{
    (void)t;
    return alloc_error;
}

/** Take a user's count of `mm`. */
void mm_get(struct mm *mm)
{
    mm->users++;
}

/** Take a hold on the record of `mm`. */
void mm_hold(struct mm *mm)
{
    mm->holds++;
}

/**
 * Drop a hold on the record of `mm`, as mm.c does: the last hold frees the
 * record. Only the address space of `reaped_during_read` is from the heap;
 * the /proc/7/mem rows' is static and is not freed, so that a fault that
 * drops its last hold fails their checks and does not end the test in the
 * allocator.
 */
void mm_unhold(struct mm *mm)
{
    if (mm && --mm->holds == 0 && mm != &space) {
        free(mm);
    }
}

/** Take a user's count of `mm` while it has a user. Returns 1, or 0 when
 *  no user is left. */
int mm_get_live(struct mm *mm)
{
    if (mm->users == 0) {
        return 0;
    }
    mm->users++;
    return 1;
}

/** Drop a user's count of `mm`. The users together have one hold, which
 *  the last user's put gives back. */
void mm_put(struct mm *mm)
{
    if (--mm->users == 0) {
        mm_unhold(mm);
    }
}

/** Make `dir` the current page directory. */
void switch_page_directory(page_dir_t *dir)
{
    _curr_dir = dir;
}

/**
 * Read `len` bytes at `addr`. Process 7's memory, the /proc/7/mem rows',
 * reads as 'm', and the reads that reach it are counted. The arguments of
 * process 13 are all 'a': their first read sleeps for the page, and
 * meanwhile the process ends, putting its user's count of its address space,
 * and is reaped, which frees its record; each read then looks the page up in
 * `mm`, as `paging_access` looks up the region and the page after a sleep,
 * and finds none once no user is left, since the last user's put frees the
 * pages whatever holds keep the record.
 */
int paging_access(struct mm *mm, uintptr_t addr, void *buf, uint32_t len, int write)
{
    (void)write;
    if (mm == &space) {
        k_memset(buf, 'm', len);
        mem_reads++;
        return 0;
    }
    if (reading) {
        struct mm *own = reading->mm;

        free(reading);
        reading = NULL;
        mm_put(own);
        reaps++;
    }
    if (mm->users == 0) {
        check(0, "a read of the arguments reaches pages a user keeps");
        return -EFAULT;
    }
    check(addr >= mm->arg_start && addr + len <= mm->arg_end,
          "a read of the arguments stays in them");
    k_memset(buf, 'a', len);
    return 0;
}

/** Copy to the caller's buffer, which is host memory. Returns 0. */
int copy_to_user(void __user *dst, const void *src, size_t n)
{
    k_memcpy((void __force *)dst, src, n);
    return 0;
}

/** Copy from the caller's buffer, which is host memory. Returns 0. */
int copy_from_user(void *dst, const void __user *src, size_t n)
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

/** The pty of a descriptor: 0, since no row's descriptor is a pty. */
int pty_slave_index(struct fd_entry *e)
{
    (void)e;
    return 0;
}

/** No session of these rows has a controlling terminal. */
int tty_controlling(int sid, uint32_t *dev, int *fg_pgid)
{
    (void)sid;
    (void)dev;
    (void)fg_pgid;
    return 0;
}

/** No row's descriptor is a message queue's. */
const char *mq_fd_name(const struct fd_entry *e, int *unlinked)
{
    (void)e;
    (void)unlinked;
    return NULL;
}

/** The inode of a Unix socket, which no row's descriptor is: 0. */
uint32_t unix_inode(const fd_entry_t *e)
{
    (void)e;
    return 0;
}

/* The model KRFFS root of the exe rows, another mount, the program's file
 * and its description, and the path the root gives the program's node now,
 * NULL for none. */
krffs_file_system_t *krffs_fs;
const struct vfs_type vfs_krffs_type = {"krffs", 0, 0, 1, 0};
static struct vfs_mount root_mount, other_mount;
static krffs_file_system_t root_fs;
static krffs_node_t root_dir, program;
static struct open_file exe_ofd;
static const char *program_path;
static const char *mounted_path;
static const char *reader_root;
static int file_holds, mount_locks;
static int entry_model, entry_holds, entry_outside;
static struct vfs_mount entry_base;
static const char *entry_directory;
static krffs_node_t entry_parent, replacement;
static struct {
    const char *leaf;
    void *node;
} entry_nodes[4];

static void entry_hold(struct vfs_mount *mnt, void *node)
{
    (void)mnt;
    (void)node;
    entry_holds++;
}

static void entry_put(struct vfs_mount *mnt, void *node)
{
    (void)mnt;
    (void)node;
    entry_holds--;
}

static int entry_lookup(struct vfs_mount *mnt, void *parent, const char *leaf, void **node)
{
    unsigned i;

    check(parent == &entry_parent, "a name is looked up in its held parent");
    for (i = 0; i < 4; i++) {
        if (entry_nodes[i].node && __builtin_strcmp(leaf, entry_nodes[i].leaf) == 0) {
            *node = entry_nodes[i].node;
            entry_hold(mnt, *node);
            return 0;
        }
    }
    return -ENOENT;
}

static const struct vfs_ops entry_ops = {
    .lookup = entry_lookup, .hold = entry_hold, .put = entry_put};
static const struct vfs_type entry_type = {"entries", NULL, &entry_ops, 1, 0};
static char exec_path[] = "/tmp/a";
static char interp_path[] = "/lib/ld-linux.so.2";

/** The root mount, the model KRFFS root. */
struct vfs_mount *vfs_root_mount(void)
{
    return &root_mount;
}

/** The working directory of `p`, "/", which no row reads. */
const char *proc_cwd(const struct process *p)
{
    (void)p;
    return "/";
}

/** Model path reconstruction on a mounted file system and the reader's root. */
int vfs_node_path(struct vfs_mount *mnt, void *node, char *out, uint32_t cap)
{
    const char *path = entry_model && node == &entry_parent ? entry_directory
                       : node == &program                   ? mounted_path
                                                            : reader_root;
    uint32_t n;
    if (entry_model && entry_outside && mnt == &other_mount && node == &entry_parent) {
        return -ENOENT;
    }
    if (!path) {
        return -ENOENT;
    }
    n = (uint32_t)k_strlen(path);
    if (n + 1 > cap) {
        return -ENAMETOOLONG;
    }
    k_memcpy(out, path, n + 1);
    return 0;
}
/** The model executable has a link exactly while its current path exists. */
int vfs_getattr(struct vfs_mount *mnt, void *node, struct vfs_stat *st)
{
    (void)mnt;
    (void)node;
    k_memset(st, 0, sizeof(*st));
    st->nlink = mounted_path != NULL;
    if (entry_model) {
        st->ino = node == &entry_parent ? 11 : node == &program ? 12 : 13;
        st->mode = node == &entry_parent ? 0040755 : 0100755;
    }
    return 0;
}
void fd_ofd_ref(struct open_file *file)
{
    (void)file;
    file_holds++;
}
void fd_ofd_unref(struct open_file *file)
{
    (void)file;
    file_holds--;
}
void vfs_ns_lock(struct vfs_mount *mnt)
{
    (void)mnt;
    mount_locks++;
    if (mount_locks != 1) {
        __builtin_trap();
    }
}
void vfs_ns_unlock(struct vfs_mount *mnt)
{
    (void)mnt;
    mount_locks--;
}
int vfs_enter(struct vfs_mount *mnt)
{
    (void)mnt;
    return 0;
}
void vfs_ref_hold(const struct vfs_ref *ref)
{
    if (ref->mnt->type && ref->mnt->type->ops && ref->mnt->type->ops->hold) {
        ref->mnt->type->ops->hold(ref->mnt, ref->node);
    }
}
void vfs_ref_put(struct vfs_ref *ref)
{
    if (ref->mnt && ref->mnt->type && ref->mnt->type->ops && ref->mnt->type->ops->put) {
        ref->mnt->type->ops->put(ref->mnt, ref->node);
    }
    ref->mnt = NULL;
    ref->node = NULL;
}
void vfs_ref_release(struct vfs_ref *ref)
{
    vfs_ref_put(ref);
}
const struct vfs_mount *vfs_mount_base(const struct vfs_mount *mnt)
{
    return mnt->bind_of ? mnt->bind_of : mnt;
}
void vfs_desc_open(struct vfs_mount *mnt)
{
    mnt->refs++;
}
void vfs_desc_close(struct vfs_mount *mnt, void *node, void *handle)
{
    struct vfs_ref parent;

    check(handle == NULL && mnt->refs > 0, "a name owns one mount reference and no open handle");
    parent.mnt = mnt;
    parent.node = node;
    vfs_ref_put(&parent);
    mnt->refs--;
}

/** The path the root gives `node`: `program_path` for the program. Returns
 *  0, or -1 for another node, for a program with no name, and when `size`
 *  does not hold the path. */
int krffs_node_path(krffs_node_t *node, char *buf, uint32_t size)
{
    uint32_t n = program_path ? (uint32_t)k_strlen(program_path) : 0;

    if (node != &program || !program_path || n + 1 > size) {
        return -1;
    }
    k_memcpy(buf, program_path, n + 1);
    return 0;
}

/** The node at `path`: the program at `program_path`, and NULL for any
 *  other path. */
krffs_node_t *krffs_resolve(const char *path)
{
    return program_path && __builtin_strcmp(path, program_path) == 0 ? &program : NULL;
}

const struct vfs_type vfs_tmpfs_type = {0};

/** The name of a memfd, which no row's node is: NULL. */
const char *tmpfs_label(void *node)
{
    (void)node;
    return NULL;
}

/** The path of a synthetic directory, which no row reads: empty. */
void synth_dir_path(int kind, int pid, int tid, char *out, int cap)
{
    (void)kind;
    (void)pid;
    (void)tid;
    if (cap > 0) {
        out[0] = '\0';
    }
}

/** No file of /proc/net is opened here. */
const char *synth_net_name(int origin)
{
    (void)origin;
    return NULL;
}

/** No file of /proc/net is opened here, so none has a place: -1. */
int synth_net_slot(int origin)
{
    (void)origin;
    return -1;
}

/** No device of /sys is opened here. */
void synth_sys_path(int node, int key, char *out, int cap)
{
    (void)node;
    (void)key;
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

/* The processors' idle ticks summed, which the uptime row sets. */
static uint64_t idle_sum;

/** The processors' idle ticks, `idle_sum`. */
uint64_t sched_idle_ticks(void)
{
    return idle_sum;
}

/** Processor `c`'s user, busy and idle ticks, from the model's `cpu_table`. */
void sched_cpu_ticks(int c, uint64_t *user, uint64_t *busy, uint64_t *idle)
{
    *user = cpu_table[c].user_ticks;
    *busy = cpu_table[c].busy_ticks;
    *idle = cpu_table[c].idle_ticks;
}

/** `v` in decimal into `out` of `cap` bytes, as `diskinfo_u64_dec` writes
 *  it, by the host's divide; diskinfo_hosttest.c tests that function.
 *  Returns the length, or 0 when `cap` does not hold the digits. */
int diskinfo_u64_dec(char *out, int cap, uint64_t v)
{
    char tmp[24];
    int n = 0, i;

    do {
        tmp[n++] = (char)('0' + v % 10u);
        v /= 10u;
    } while (v != 0);
    if (n + 1 > cap) {
        return 0;
    }
    for (i = 0; i < n; i++) {
        out[i] = tmp[n - 1 - i];
    }
    out[n] = '\0';
    return n;
}

/** /proc/uptime gives the idle sum in seconds at its full 64 bits, and a
 * sum under a second with its leading zero. A hundredth of a second is
 * TICK_HZ / 100 ticks. */
static void uptime_rows(void)
{
    struct procfs_text text;

    cpu_table[0].idle_ticks = TICK_HZ;
    cpu_table[0].busy_ticks = TICK_HZ / 2u;
    idle_sum = 8589934560ull * (TICK_HZ / 100u);
    procfs_text_init(&text);
    check(procfs_gen_uptime(&text) == 17 &&
              __builtin_memcmp(text.buf, "1.50 85899345.60\n", 17) == 0,
          "the idle seconds past 32 bits of ticks are whole");
    kfree(text.buf);
    idle_sum = 7u * (TICK_HZ / 100u);
    procfs_text_init(&text);
    check(procfs_gen_uptime(&text) == 10 && __builtin_memcmp(text.buf, "1.50 0.07\n", 10) == 0,
          "and a sum under a second has its leading zero");
    kfree(text.buf);
    cpu_table[0].idle_ticks = 0;
    cpu_table[0].busy_ticks = 0;
}

/** A process's stat has the time of all its threads, and a thread's under
 * the process's task directory has its own, each in USER_HZ's clock_t. */
static void thread_times(void)
{
    struct procfs_text text;

    thread.state = PROC_READY;
    thread.tg = &group;
    k_memcpy(thread.comm, "test", 5);
    process.utime = 5u * (TICK_HZ / USER_HZ);
    process.stime = 6u * (TICK_HZ / USER_HZ);
    thread.utime = 30u * (TICK_HZ / USER_HZ);
    thread.stime = 40u * (TICK_HZ / USER_HZ);
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

    process.pid = 7;
    thread.pid = 9;
    stranger.pid = 11;
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
    if (sizeof(uintptr_t) < sizeof(uint64_t)) {
        uint64_t offset = (uint64_t)(uintptr_t)-1 + 1;

        mem_ofd.offset = offset;
        check(procfs_mem_read(&entries[0], (char __user *)buf, sizeof(buf)) == -EIO &&
                  space.users == 1 && mem_ofd.offset == offset && mem_reads == 1,
              "an offset past the address width releases its user and reaches no memory");
        mem_ofd.offset = sizeof(buf);
    }
    space.users = 0;
    check(procfs_mem_read(&entries[0], (char __user *)buf, sizeof(buf)) == 0 && mem_reads == 1,
          "a read once the process has ended returns 0 and reaches no memory");
    procfs_close(&entries[0]);
    check(space.holds == 1 && entries[0].priv == NULL, "the close lets go of the hold");
    process.mm = NULL;
}

/**
 * /proc/PID/exe of a process whose address space keeps its program's file:
 * on the KRFFS root the link reads the file's present path, and once its name
 * is gone the path the exec found it at with " (deleted)", which the name an
 * exec of the link gives leaves off; a mounted file gives its current path
 * too; a short buffer takes the first bytes; and the resolver goes on from
 * the file itself.
 */
static void exe_links(void)
{
    struct vfs_ref ref;
    char buf[64];

    krffs_fs = &root_fs;
    root_fs.node = &root_dir;
    root_mount.type = &vfs_krffs_type;
    k_memset(&space, 0, sizeof(space));
    space.users = 1;
    space.holds = 1;
    space.exe = exec_path;
    space.exe_file = &exe_ofd;
    exe_ofd.vmnt = &root_mount;
    exe_ofd.vnode = &program;
    k_strncpy((char *)program.name, "b", sizeof(program.name));
    program_path = "/tmp/b";
    process.mm = &space;
    check(procfs_readlink_exe(7, buf, (int)sizeof(buf)) == 6 &&
              __builtin_memcmp(buf, "/tmp/b", 6) == 0,
          "the link reads a renamed program's present path");
    check(procfs_readlink_exe(7, buf, 3) == 3 && __builtin_memcmp(buf, "/tm", 3) == 0,
          "a short buffer takes the first bytes");
    check(procfs_exe_file(7, &ref) == 0 && ref.mnt == &root_mount && ref.node == &program,
          "the resolver goes on from the program's file itself");
    program.name[0] = '\0';
    program_path = NULL;
    check(procfs_readlink_exe(7, buf, (int)sizeof(buf)) == 16 &&
              __builtin_memcmp(buf, "/tmp/a (deleted)", 16) == 0,
          "a program whose name is gone reads as its exec's path, deleted");
    check(procfs_exe_name(7, buf, (int)sizeof(buf)) == 6 && __builtin_memcmp(buf, "/tmp/a", 6) == 0,
          "the name an exec of the link gives has no mark");
    exe_ofd.vmnt = &other_mount;
    mounted_path = "/private/root/tmp/moved";
    check(procfs_readlink_exe(7, buf, (int)sizeof(buf)) == 23 &&
              __builtin_memcmp(buf, "/private/root/tmp/moved", 23) == 0,
          "a mounted file reads its present path");
    check(space.holds == 1 && file_holds == 0 && mount_locks == 0,
          "each path read releases its address space, description and mount lock");
    mounted_path = NULL;
    check(procfs_readlink_exe(7, buf, (int)sizeof(buf)) == 16 &&
              __builtin_memcmp(buf, "/tmp/a (deleted)", 16) == 0,
          "an unlinked mounted file keeps its saved name and deleted mark");
    process.mm = NULL;
    krffs_fs = NULL;
}

/** Read a real held-name record through procfs, including its deleted mark. */
static void entry_expect(struct vfs_name *name, void *node, const char *want)
{
    char text[128];
    struct vfs_name *held;
    int n;

    exe_ofd.name = name;
    exe_ofd.vnode = node;
    held = procfs_exe_entry(7);
    check(held == name, "an exec of the proc link shares its exact selected entry");
    vfs_name_put(held);
    n = procfs_readlink_exe(7, text, sizeof(text));
    check(n == (int)k_strlen(want) && __builtin_memcmp(text, want, (unsigned)n) == 0,
          "procfs keeps the selected entry's exact path and deleted state");
    check(file_holds == 0 && mount_locks == 0 && space.holds == 1,
          "a named executable read releases its temporary holds and lock");
}

/** Selected entries use the reader's root after releasing the file lock. */
static void entry_root_row(struct vfs_name *name)
{
    struct fs_struct fs;
    process_t *saved = current_proc;

    k_memset(&fs, 0, sizeof(fs));
    fs.root_mnt = &other_mount;
    fs.root_node = &root_dir;
    reader_root = "/private/root";
    process.fs = &fs;
    current_proc = &process;
    entry_expect(name, &program, "/bin/chosen");
    process.fs = NULL;
    current_proc = saved;
}

/** A name survives sharing, file and parent renames, and removal of its link.
 */
static void entry_alias_rows(struct vfs_ref *parent)
{
    struct vfs_name *name = NULL, *alias = NULL;

    check(vfs_name_capture(parent, "chosen", &name) == 0 && name,
          "capture the executed hard-link entry");
    check(vfs_name_capture(parent, "other", &alias) == 0 && alias,
          "capture another entry of the same inode independently");
    if (!name || !alias) {
        vfs_name_put(name);
        vfs_name_put(alias);
        return;
    }
    entry_expect(name, &program, "/private/root/bin/chosen");
    entry_expect(alias, &program, "/private/root/bin/other");
    entry_root_row(name);
    check(vfs_name_same(&other_mount, &entry_parent, "chosen", &entry_parent, "other"),
          "a rename between existing aliases is a same-inode no-op");
    entry_expect(name, &program, "/private/root/bin/chosen");
    check(vfs_name_ref(name) == name, "another description can share the selected entry");
    vfs_name_put(name);
    entry_nodes[0].leaf = "renamed";
    vfs_name_rename(&other_mount, &entry_parent, "chosen", &entry_parent, "renamed");
    entry_expect(name, &program, "/private/root/bin/renamed");
    entry_expect(alias, &program, "/private/root/bin/other");
    entry_directory = "/moved/bin";
    entry_outside = 1;
    entry_expect(name, &program, "/moved/bin/renamed");
    entry_nodes[0].node = NULL;
    vfs_name_unlink(&other_mount, &entry_parent, "renamed");
    entry_expect(name, &program, "/moved/bin/renamed (deleted)");
    entry_nodes[0].node = &replacement;
    entry_expect(name, &program, "/moved/bin/renamed (deleted)");
    entry_expect(alias, &program, "/moved/bin/other");
    vfs_name_put(name);
    vfs_name_put(alias);
}

/** Replacing one executable entry does not select its surviving alias. */
static void entry_replace_rows(struct vfs_ref *parent)
{
    struct vfs_name *victim = NULL, *source = NULL;

    entry_nodes[0].leaf = "chosen";
    entry_nodes[0].node = &program;
    entry_nodes[2].leaf = "source";
    entry_nodes[2].node = &replacement;
    check(vfs_name_capture(parent, "chosen", &victim) == 0 && victim,
          "capture the executable entry that will be replaced");
    check(vfs_name_capture(parent, "source", &source) == 0 && source,
          "capture the replacing file's entry");
    if (victim && source) {
        check(!vfs_name_same(&other_mount, &entry_parent, "source", &entry_parent, "chosen"),
              "different inode entries are not a rename no-op");
        entry_nodes[0].node = &replacement;
        entry_nodes[2].node = NULL;
        vfs_name_rename(&other_mount, &entry_parent, "source", &entry_parent, "chosen");
        entry_expect(victim, &program, "/moved/bin/chosen (deleted)");
        entry_expect(source, &replacement, "/moved/bin/chosen");
    }
    vfs_name_put(victim);
    vfs_name_put(source);
}

/** The actual name-record implementation over a model directory with aliases.
 */
static void exe_entries(void)
{
    struct vfs_ref parent;

    entry_model = 1;
    entry_directory = "/private/root/bin";
    mounted_path = "/private/root/bin/other";
    entry_nodes[0].leaf = "chosen";
    entry_nodes[0].node = &program;
    entry_nodes[1].leaf = "other";
    entry_nodes[1].node = &program;
    other_mount.type = &entry_type;
    entry_base.type = &entry_type;
    other_mount.bind_of = &entry_base;
    other_mount.refs = 0;
    parent.mnt = &other_mount;
    parent.node = &entry_parent;
    space.holds = 1;
    space.exe = exec_path;
    space.exe_file = &exe_ofd;
    process.mm = &space;
    exe_ofd.vmnt = &other_mount;
    entry_alias_rows(&parent);
    check(entry_holds == 0 && other_mount.refs == 0, "shared entry ownership ends exactly once");
    entry_replace_rows(&parent);
    check(entry_holds == 0 && other_mount.refs == 0, "replacement releases every parent and mount");
    exe_ofd.name = NULL;
    process.mm = NULL;
    other_mount.type = NULL;
    other_mount.bind_of = NULL;
    entry_outside = 0;
    entry_model = 0;
}

/** A reader's private root is removed only at a complete path component. */
static void exe_chroot(void)
{
    struct fs_struct fs;
    process_t *saved = current_proc;
    char buf[64];

    k_memset(&fs, 0, sizeof(fs));
    fs.root_mnt = &other_mount;
    fs.root_node = &root_dir;
    process.fs = &fs;
    process.mm = &space;
    current_proc = &process;
    space.exe_file = &exe_ofd;
    exe_ofd.vmnt = &other_mount;
    mounted_path = "/private/root/tmp/moved";
    reader_root = "/private/root";
    check(procfs_readlink_exe(7, buf, (int)sizeof(buf)) == 10 &&
              __builtin_memcmp(buf, "/tmp/moved", 10) == 0,
          "the renamed executable is relative to the reader's private root");
    reader_root = "/private/roo";
    check(procfs_readlink_exe(7, buf, (int)sizeof(buf)) == 23 &&
              __builtin_memcmp(buf, "/private/root/tmp/moved", 23) == 0,
          "a matching name prefix is not a root path component");
    check(file_holds == 0 && mount_locks == 0, "no description or mount lock remains");
    process.fs = NULL;
    process.mm = NULL;
    current_proc = saved;
}

/** The paths /proc/PID/maps gives the segments the exec loaded: the
 * program's, below the heap, and the dynamic linker's, above it. */
static void image_paths(void)
{
    vma_t r;

    k_memset(&space, 0, sizeof(space));
    k_memset(&r, 0, sizeof(r));
    space.exe = exec_path;
    space.interp = interp_path;
    space.vm.heap_start = 0x0804d000u;
    r.start = 0x08048000u;
    r.end = 0x08049000u;
    r.type = VM_CODE;
    check(maps_path(&space, &r) == exec_path, "a segment below the heap is the program's");
    r.start = 0xaffc2000u;
    r.end = 0xaffc3000u;
    r.type = VM_DATA;
    check(maps_path(&space, &r) == interp_path, "one above it is the dynamic linker's");
}

/* The descriptor the oom_score_adj rows read and write, on process 7. */
static struct procfs_file oom_file;
static struct open_file oom_desc;
static fd_entry_t oom_entry;

/** Write the text `text` to process 7's oom_score_adj as the caller `who`;
 * the call's answer. */
static int32_t oom_write(process_t *who, const char *text)
{
    process_t *self = current_proc;
    int32_t rc;

    current_proc = who;
    rc = procfs_oom_write(&oom_entry, (const char __user *)text, (uint32_t)__builtin_strlen(text));
    current_proc = self;
    return rc;
}

/** Whether a read of process 7's oom_score_adj from its start gives `want`. */
static int oom_reads(const char *want)
{
    char buf[16];
    int32_t n;

    oom_desc.offset = 0;
    n = procfs_oom_read(&oom_entry, (char __user *)buf, sizeof(buf));
    return n == (int32_t)__builtin_strlen(want) && __builtin_memcmp(buf, want, (size_t)n) == 0 &&
           procfs_oom_read(&oom_entry, (char __user *)buf, sizeof(buf)) == 0;
}

/**
 * A write to oom_score_adj takes an integer from -1000 to 1000 as Linux's
 * `kstrtoint` in base 0 reads it, spaces stripped at both ends, and at most
 * 12 bytes of the write; anything else is EINVAL, and a number an int does
 * not hold ERANGE. A read gives the value and a newline.
 */
static void oom_values(void)
{
    oom_file.origin_pid = 7;
    oom_file.origin_file = SYNTH_PROC_OOM_ADJ;
    oom_entry.priv = &oom_file;
    oom_entry.ofd = &oom_desc;
    check(oom_reads("0\n"), "oom_score_adj reads 0 at first");
    check(oom_write(&process, "500\n") == 4 && oom_reads("500\n"),
          "a write of 500 takes its four bytes and reads back");
    check(oom_write(&process, " -1000\n") == 7 && oom_reads("-1000\n"),
          "spaces around the value are stripped");
    check(oom_write(&process, "0x1f4") == 5 && oom_reads("500\n"), "0x begins a hexadecimal value");
    check(oom_write(&process, "-0764") == 5 && oom_reads("-500\n"), "0 begins an octal value");
    check(oom_write(&process, "  300        \n\n") == 12 && oom_reads("300\n"),
          "the first 12 bytes of a longer write are read");
    check(oom_write(&process, "1001") == -EINVAL && oom_write(&process, "-1001") == -EINVAL &&
              oom_reads("300\n"),
          "a value past -1000 to 1000 is EINVAL and changes nothing");
    check(oom_write(&process, "") == -EINVAL && oom_write(&process, "5 0") == -EINVAL &&
              oom_write(&process, "12a") == -EINVAL && oom_write(&process, "+-5") == -EINVAL &&
              oom_write(&process, "09") == -EINVAL,
          "text that is no integer is EINVAL");
    check(oom_write(&process, "99999999999") == -ERANGE &&
              oom_write(&process, "2147483648") == -ERANGE &&
              oom_write(&process, "-2147483649") == -ERANGE,
          "a number an int does not hold is ERANGE");
    check(oom_write(&process, "-2147483648") == -EINVAL &&
              oom_write(&process, "2147483647") == -EINVAL,
          "a number an int holds that is out of the range is EINVAL");
    check(oom_write(&process, "2147483648x") == -EINVAL &&
              oom_write(&process, "-2147483649z") == -EINVAL && oom_reads("300\n"),
          "text that is no integer is EINVAL however many digits it begins with");
    check(oom_write(&process, "\240100\240") == 5 && oom_reads("100\n"),
          "the byte 0xa0 at either end is a space, as Linux's isspace has it");
    check(oom_write(&process, "0") == 1 && oom_reads("0\n"), "a write of 0");
}

/**
 * A caller without privilege may raise the value and lower it to the floor,
 * which starts at 0, but not below it; a privileged caller's write sets the
 * floor to what it wrote.
 */
static void oom_floor(void)
{
    process_t user;

    k_memset(&user, 0, sizeof(user));
    user.euid = 1000;
    user.tg = &other_group;
    check(oom_write(&user, "300") == 3 && oom_write(&user, "0") == 1 && oom_reads("0\n"),
          "a caller without privilege raises the value and lowers it to the floor");
    check(oom_write(&user, "-1") == -EACCES && oom_reads("0\n"),
          "and may not lower it below the floor");
    check(oom_write(&process, "200") == 3 && group.oom_score_adj_min == 200,
          "a privileged write sets the floor");
    check(oom_write(&user, "150") == -EACCES && oom_write(&user, "250") == 3 &&
              oom_reads("250\n") && group.oom_score_adj_min == 200,
          "which a caller without privilege may not pass");
    check(oom_write(&process, "-1000") == 5 && oom_reads("-1000\n"),
          "a privileged caller writes below the floor");
    group.oom_score_adj = group.oom_score_adj_min = 0;
}

/** An open of oom_score_adj that gets no descriptor answers the table's
 * error: ENOMEM for a heap with no room, EMFILE at the limit. */
static void oom_open_errors(void)
{
    struct vpath vp;

    k_memset(&vp, 0, sizeof(vp));
    alloc_error = -ENOMEM;
    check(procfs_open_oom(&vp, 7, 0) == -ENOMEM,
          "an open of oom_score_adj the heap has no room for is ENOMEM");
    alloc_error = -EMFILE;
    check(procfs_open_oom(&vp, 7, 0) == -EMFILE,
          "an open of oom_score_adj at the descriptor limit is EMFILE");
    alloc_error = 0;
}

/**
 * A read of a process's arguments that sleeps for a page while the process
 * ends and is reaped reads the rest through the user's count it took of the
 * address space, whose put then frees it, and nothing of the freed record:
 * all 300 bytes come back, three reads of up to 128 each. Each later read
 * looks the page up in the address space again, and AddressSanitizer
 * reports a read of the freed record or of a freed address space.
 */
static void reaped_during_read(void)
{
    struct procfs_text text;
    struct mm *mm = calloc(1, sizeof(*mm));

    reading = calloc(1, sizeof(*reading));
    check(mm && reading, "allocate the process and its address space");
    if (!mm || !reading) {
        return;
    }
    mm->users = 1;
    mm->holds = 1;
    mm->arg_start = 0x1000u;
    mm->arg_end = 0x1000u + 300u;
    reading->pid = reading->tgid = 13;
    reading->state = PROC_READY;
    reading->tg = &reading_group;
    reading->mm = mm;
    reading_group.leader = reading;
    reading_group.live = 1;
    procfs_text_init(&text);
    check(procfs_gen_cmdline(&text, 13) == 300 && reaps == 1 && !reading,
          "a read of the arguments of a process reaped while it sleeps reads them all");
    check(text.len == 300 && text.buf[0] == 'a' && text.buf[299] == 'a',
          "and every byte comes from the address space");
    kfree(text.buf);
}

/**
 * The rows of a stat text, then those of the functions above. A stat text
 * whose allocation fails is empty and marked out of memory, for ENOMEM; one
 * whose growth fails keeps its first bytes, marked, to be freed; and one
 * made whole ends in a newline and a NUL and begins with the pid, the
 * command in parentheses and the state. Returns 0 when every check passed
 * and 1 otherwise.
 */
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
    uptime_rows();
    descriptor_links();
    task_ids();
    mem_descriptor();
    exe_links();
    exe_chroot();
    exe_entries();
    image_paths();
    reaped_during_read();
    oom_values();
    oom_floor();
    oom_open_errors();
    if (!failures) {
        printf("procfs-hosttest: result: pass: stat allocation failure and times, descriptor "
               "links, task ids, the mem descriptor's counts, the exe link, a read of a reaped "
               "process's arguments, oom_score_adj, the paths of the maps and the uptime idle "
               "sum\n");
    }
    return failures != 0;
}
