/**
 * Check proc stat generation when every text allocation fails, the CPU time
 * of a process's stat and of a thread's, the descriptor links of a process,
 * which a closed descriptor has none of, the ids a path under a process's
 * task directory needs, the counts a /proc/PID/mem descriptor takes of the
 * address space: a hold on its record, and a user's count for a read only
 * while a user remains, the text and the file of /proc/PID/exe, a read of a
 * process's arguments while the process ends and is reaped, and the reads
 * and writes of oom_score_adj.
 *
 * host-deps: rach2/sys/lib/kstring.c rach2/sys/lib/kctypes.c
 * host-cflags: -Wno-int-to-pointer-cast -Wno-pointer-to-int-cast
 *
 * Unused exports are private so the compiler removes unrelated generators.
 *
 * The first read of a process's arguments sleeps for the page, as a read of
 * a page of a file the process maps does, and meanwhile the process ends,
 * dropping its user's count of the address space, and is reaped, which
 * frees its record. The read goes on through the user's count it took, and
 * each later read looks the page up in the address space again: nothing may
 * be read from the freed record or a freed address space, which
 * AddressSanitizer judges.
 *
 * A write to oom_score_adj is read as Linux reads it, and the value must
 * be in -1000 to 1000; a caller without privilege may not write a value
 * below the floor a privileged caller's write sets, and a read gives the
 * value as it is at the read.
 */
#include "lib/kint.h"
struct vpath;
struct fd_entry;
struct vfs_ref;
static int procfs_resolve_pid(int pid);
static int procfs_resolve_id(const struct vpath *vp);
static int procfs_open(const struct vpath *vp, int flags);
static int procfs_readlink_fd(int pid, int fd, char *buf, int cap);
static int procfs_readlink_exe(int pid, char *buf, int cap);
static int procfs_exe_name(int pid, char *buf, int cap);
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
static int procfs_fd_oom_owner(struct fd_entry *e, uint32_t *uid, uint32_t *gid);
#include "fs/procfs.c"

extern int printf(const char *format, ...);
extern int sscanf(const char *str, const char *format, ...);
extern void *malloc(unsigned long size);
extern void *calloc(unsigned long count, unsigned long size);
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
/* The process whose arguments `reaped_during_read` reads, 13, from the heap,
 * since its end and its reap free its record and its address space; NULL
 * once reaped. */
static process_t *reading;
static struct tgroup reading_group;
static int reaps;
/* The directory the reader runs in, which is not the reaped process's. */
static page_dir_t reader_dir;
page_dir_t *_kernel_dir;
page_dir_t *_curr_dir = &reader_dir;

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

/* The error a row has the descriptor table answer, or 0. */
static int alloc_error;

/** The one descriptor of the /proc/7/mem rows, or the error a row set. */
int fd_alloc(fd_table_t *t)
{
    (void)t;
    return alloc_error;
}

/* The counts of an address space, as mm.c keeps them: the users together
 * have one hold, which the last user's put gives back, and the last hold
 * frees the record. Only the address space of `reaped_during_read` is from
 * the heap; the /proc/7/mem rows' is static, and a fault that takes its last
 * hold must fail their checks, not end the test in the allocator. */
void mm_get(struct mm *mm)
{
    mm->users++;
}

void mm_hold(struct mm *mm)
{
    mm->holds++;
}

void mm_unhold(struct mm *mm)
{
    if (mm && --mm->holds == 0 && mm != &space) {
        free(mm);
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
    if (--mm->users == 0) {
        mm_unhold(mm);
    }
}

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
int paging_access(struct mm *mm, uint32_t addr, void *buf, uint32_t len, int write)
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
static char exec_path[] = "/tmp/a";

struct vfs_mount *vfs_root_mount(void)
{
    return &root_mount;
}

int krffs_node_path(krffs_node_t *node, char *buf, uint32_t size)
{
    uint32_t n = program_path ? (uint32_t)k_strlen(program_path) : 0;

    if (node != &program || !program_path || n + 1 > size) {
        return -1;
    }
    k_memcpy(buf, program_path, n + 1);
    return 0;
}

krffs_node_t *krffs_resolve(const char *path)
{
    return program_path && __builtin_strcmp(path, program_path) == 0 ? &program : NULL;
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

/**
 * /proc/PID/exe of a process whose address space keeps its program's file:
 * on the KRFFS root the link reads the file's present path, and once its name
 * is gone the path the exec found it at with " (deleted)", which the name an
 * exec of the link gives leaves off; on another type it reads the path the
 * exec found it at; a short buffer takes the first bytes; and the resolver
 * goes on from the file itself.
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
    check(procfs_readlink_exe(7, buf, (int)sizeof(buf)) == 6 &&
              __builtin_memcmp(buf, "/tmp/a", 6) == 0,
          "a program on another type reads as its exec's path");
    check(space.holds == 1, "each read lets go of the record it held");
    process.mm = NULL;
    krffs_fs = NULL;
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
    check(oom_write(&process, "99999999999") == -ERANGE, "a number an int does not hold is ERANGE");
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
 * all 300 bytes come back, three reads of up to 128 each.
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
    exe_links();
    reaped_during_read();
    oom_values();
    oom_floor();
    oom_open_errors();
    if (!failures) {
        printf("procfs-hosttest: result: pass: stat allocation failure and times, descriptor "
               "links, task ids, the mem descriptor's counts, the exe link, a read of a reaped "
               "process's arguments and oom_score_adj\n");
    }
    return failures != 0;
}
