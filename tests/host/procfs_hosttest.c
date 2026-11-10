/**
 * procfs_hosttest.c: the files rach2/sys/fs/procfs.c makes: the stat text
 * and its allocation, /proc/uptime, the CPU times of a process and of a
 * thread, task ids, descriptor links, /proc/PID/mem, /proc/PID/exe, the
 * paths of /proc/PID/maps, a read of a process's arguments while the process
 * is reaped, oom_score_adj, the cache, NUMA, clock source and DMI files of
 * /sys, and the /sys entries and uevents of a DRM device
 * and of a driver's own attributes, links and PCI identity, and those of
 * a PS/2 keyboard and a USB mouse (evdev_sysfs.c).
 *
 * host-deps: tests/host/compat/vm_iter_host.c
 * host-deps: rach2/sys/fs/sysfs.c rach2/sys/drm/drm_sysfs.c
 * host-deps: rach2/sys/evdev_sysfs.c
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
struct vfs_mount;
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
static int sysfs_net_wiphy(int index);
static const char *sysfs_wlan_driver(int index);
static uint32_t procfs_mount_id(const struct vfs_mount *m);
static uint32_t procfs_synth_mount_id(uint64_t dev, const struct vfs_mount *hint);
static int sysfs_attr_present(int node, int attr);
#include "fs/procfs.c"
#include "drm/drm.h"
#include "evdev.h"
#include "arch/i686/mmu.h"
#include "ns_first.h"

/** No dma-buf descriptor is installed in this model. */
int is_dma_buf_file(const fd_entry_t *e)
{
    (void)e;
    return 0;
}

/** No backlight is registered in this model. */
int backlight_count(void)
{
    return 0;
}

/** No backlight has attributes. */
int backlight_show(int index, int attr, char *out, int cap)
{
    (void)index;
    (void)attr;
    (void)out;
    (void)cap;
    return -ENOENT;
}

/** No radio switch is registered in this model. */
int rfkill_present(int index)
{
    (void)index;
    return 0;
}

/** No radio switch has attributes. */
int rfkill_show(int index, int attr, char *out, int cap)
{
    (void)index;
    (void)attr;
    (void)out;
    (void)cap;
    return -ENOENT;
}

/** No disk has a sysfs row in this model. */
int devfs_disk_row(int index, struct devfs_disk_row *row)
{
    (void)index;
    (void)row;
    return 0;
}

/** No loop device has a backing file. */
const char *loop_backing(int index, int *autoclear)
{
    (void)index;
    *autoclear = 0;
    return NULL;
}

/** No radio is registered. */
struct wlan *wlan_by_index(unsigned index)
{
    (void)index;
    return NULL;
}

/** The frame allocator is absent from this model. */
size_t frame_total_count(void)
{
    return 0;
}

/** The clock is absent from this model. */
int rtc_read_clock(rtc_time_t *out)
{
    (void)out;
    return -EIO;
}

/** No successful clock read supplies a date for conversion. */
int64_t rtc_seconds_of(const rtc_time_t *t)
{
    (void)t;
    return 0;
}

/** No RTC wake alarm is armed. */
int64_t devfs_rtc_wakealarm(void)
{
    return -1;
}

/** Each model processor has its own core in package zero. */
void cpuinfo_place(int cpu, struct cpu_place *p)
{
    k_memset(p, 0, sizeof(*p));
    p->core = (uint32_t)cpu;
}

/** Only CPU zero is online in this model. */
uint64_t cpuinfo_siblings(int cpu, int level)
{
    (void)level;
    return cpu == 0 ? 1 : 0;
}

/** CPU 0 of the model has two caches: a 32K L1 data cache of its own, 8
 * ways of 64 sets of 64-byte lines, and a 4M L2 that its package's
 * processors share, 16 ways of 4096 sets. */
int cpuinfo_cache(int cpu, int index, struct cpu_cache *cache)
{
    if (cpu != 0 || index < 0 || index > 1) {
        return 0;
    }
    cache->level = (uint32_t)index + 1;
    cache->type = index == 0 ? 1 : 3;
    cache->line = 64;
    cache->partitions = 1;
    cache->ways = index == 0 ? 8 : 16;
    cache->sets = index == 0 ? 64 : 4096;
    cache->sharing = index == 0 ? 1 : 16;
    cache->id = 0;
    cache->shared = 1;
    return 1;
}

/** The model's firmware table, as QEMU's SeaBIOS gives one, with no UUID
 * and no board. */
const char *dmi_field(int field)
{
    return field == DMI_BIOS_VENDOR    ? "SeaBIOS"
           : field == DMI_BIOS_RELEASE ? "0.0"
           : field == DMI_SYS_VENDOR   ? "QEMU"
           : field == DMI_PRODUCT_NAME ? "Standard PC (i440FX + PIIX, 1996)"
           : field == DMI_CHASSIS_TYPE ? "1"
                                       : 0;
}

/** The clock is kept by the TSC, measured against the HPET. */
const char *timer_clocksource(int n)
{
    return n < 0 ? "tsc" : n == 0 ? "tsc" : n == 1 ? "hpet" : 0;
}

/* The counters /proc/net/snmp reports, which net_core.c defines, and those
 * /proc/net/snmp6 reports, which ip6.c defines. */
struct net_mib net_mib;
struct net_mib6 net_mib6;

/* The one interface, `eth0` of index 2, while the counter row runs, and
 * NULL in every other row, which see /sys/class/net empty. Its driver keeps
 * no counts of its own. */
static const struct netdev_ops iface_ops;
static struct netdev *iface;
/* The depth of `net_lock` brackets, which the counter row checks are closed. */
static int lock_depth;

/** One interface while the counter row runs, else none. */
int netdev_count(void)
{
    return iface != 0;
}

/** The model's interface belongs to the caller's network namespace. */
int net_ns_seen(void)
{
    return netdev_count();
}

/** Every interface is in the caller's network namespace. */
int netdev_seen_count(void)
{
    return netdev_count();
}

/** The attribute files of a /sys directory: the statistics of an interface,
 * named as in synthfs.c, and none other. */
const char *sysfs_attr(int node, int n)
{
    return node == SYSFS_NET_STATS && n >= 0 && n < 10 ? "statistic" : 0;
}

/** The interface `iface` at 0, else NULL. */
struct netdev *netdev_at(int n)
{
    return n == 0 ? iface : 0;
}

/** Whether the interface's link is up: no driver here says. */
int netdev_link(const struct netdev *nd, struct netdev_link *out)
{
    (void)nd;
    (void)out;
    return 0;
}

/** Whether the interface's card is running: no card here. */
int netdev_running(const struct netdev *nd)
{
    (void)nd;
    return 0;
}

/** Open a bracket of the network lock, counting its depth. */
uint32_t net_lock(void)
{
    return (uint32_t)lock_depth++;
}

/** Close the bracket `net_lock` opened. */
void net_unlock(uint32_t flags)
{
    lock_depth = (int)flags;
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

/** The paths of the fixed /sys directories the DRM rows add below, as
 * synthfs.c gives them, and the empty string for any other. */
void synth_sys_path(int node, int key, char *out, int cap)
{
    const char *path = node == SYSFS_CLASS_DRM        ? "/sys/class/drm"
                       : node == SYSFS_DEV_CHAR       ? "/sys/dev/char"
                       : node == SYSFS_KERNEL         ? "/sys/kernel"
                       : node == SYSFS_PLATFORM       ? "/sys/devices/platform"
                       : node == SYSFS_BUS_PCI        ? "/sys/bus/pci"
                       : node == SYSFS_PCI_ROOT       ? "/sys/devices/pci0000:00"
                       : node == SYSFS_CLASS          ? "/sys/class"
                       : node == SYSFS_BUS            ? "/sys/bus"
                       : node == SYSFS_DEVICES        ? "/sys/devices"
                       : node == SYSFS_BUS_PLATFORM   ? "/sys/bus/platform"
                       : node == SYSFS_PLATFORM_LINKS ? "/sys/bus/platform/devices"
                       : node == SYSFS_BUS_USB        ? "/sys/bus/usb"
                       : node == SYSFS_USB_LINKS      ? "/sys/bus/usb/devices"
                       : node == SYSFS_USB_DEVICE     ? "/sys/devices/usb/1-1"
                                                      : "";

    if (node == SYSFS_PCI_DEVICE && cap >= 32) {
        __builtin_snprintf(out, (unsigned long)cap, "/sys/devices/pci0000:00/0000:%02x:%02x.%x",
                           (unsigned)(key >> 8), (unsigned)((key >> 3) & 31), (unsigned)(key & 7));
        return;
    }
    if (cap > 0) {
        k_strncpy(out, path, (size_t)cap - 1);
        out[cap - 1] = '\0';
    }
}

/** A PCI function's name, as synthfs.c gives it, and the one USB device's,
 * at the first port of the first controller. */
void synth_sys_device_name(int node, int key, char *out, int cap)
{
    if (node == SYSFS_USB_DEVICE) {
        __builtin_snprintf(out, (unsigned long)cap, "1-1");
        return;
    }
    __builtin_snprintf(out, (unsigned long)cap, "0000:%02x:%02x.%x", (unsigned)(key >> 8),
                       (unsigned)((key >> 3) & 31), (unsigned)(key & 7));
}

/** The kernel log, which the rows do not read. */
void klog(int level, const char *subsystem, const char *format, ...)
{
    (void)level;
    (void)subsystem;
    (void)format;
}

/* The uevents sent, NUL-ended strings one after another, and their bytes. */
static char uevents[32768];
static uint32_t uevents_len;
static int uevents_sent;

/** Keep the uevent `msg` of `len` bytes after those before it. */
void netlink_uevent(const uint8_t *msg, uint32_t len)
{
    if (uevents_len + len <= sizeof(uevents)) {
        k_memcpy(uevents + uevents_len, msg, len);
        uevents_len += len;
    }
    uevents_sent++;
}

/** One PCI function answers on this machine, a virtio display at 00:04.0,
 * 1af4:1050 with subsystem 1af4:1100; every other read is all ones. */
uint32_t pci_read_config32(uint8_t bus, uint8_t device, uint8_t function, uint8_t offset)
{
    if (bus != 0 || device != 4 || function != 0) {
        return 0xFFFFFFFFu;
    }
    return offset == 0x00   ? 0x10501af4u
           : offset == 0x04 ? 0x00100007u
           : offset == 0x08 ? 0x03000001u
           : offset == 0x2C ? 0x11001af4u
                            : 0u;
}

/** No function has extended configuration space. */
uint32_t pci_read_config32_ext(uint8_t bus, uint8_t device, uint8_t function, uint16_t offset)
{
    (void)bus;
    (void)device;
    (void)function;
    (void)offset;
    return 0xFFFFFFFFu;
}

uint16_t pci_read_config16(uint8_t bus, uint8_t device, uint8_t function, uint8_t offset)
{
    return (uint16_t)pci_read_config32(bus, device, function, offset);
}

uint8_t pci_read_config8(uint8_t bus, uint8_t device, uint8_t function, uint8_t offset)
{
    return (uint8_t)pci_read_config32(bus, device, function, offset);
}

/** The bus has no function to list. */
bool pci_nth(int nth, pci_device_t *out)
{
    (void)nth;
    (void)out;
    return false;
}

/* While set, the USB stack's entry 0 runs a boot-protocol mouse, 0627:0001,
 * at the first port of the first controller; else it has no device. */
static int usb_mouse_in;

/** The USB stack's mouse while `usb_mouse_in` is set, else no device. */
int usb_core_row(int index, struct usb_sys_row *row)
{
    if (!usb_mouse_in || index != 0) {
        return -1;
    }
    k_memset(row, 0, sizeof(*row));
    row->bus = 1;
    row->depth = 1;
    row->ports[0] = 1;
    row->devnum = 1;
    row->vendor = 0x0627;
    row->product = 0x0001;
    row->config = 1;
    row->ifclass = 3;
    row->ifsubclass = 1;
    row->ifprotocol = 2;
    return 0;
}

/* The event devices the input rows register, by slot, and whether each is
 * there; and whether the caller is an i386 program. */
static struct evdev_desc input_devs[EVDEV_MAX];
static int input_present[EVDEV_MAX];
static int compat_caller;

/** The device of slot `n` the input rows registered. */
int evdev_snapshot(int n, struct evdev_desc *out)
{
    if (n < 0 || n >= EVDEV_MAX || !input_present[n]) {
        return 0;
    }
    *out = input_devs[n];
    return 1;
}

/** Whether the caller is an i386 program, as the row sets. */
int in_compat_syscall(void)
{
    return compat_caller;
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

/** /proc/net/dev and /sys/class/net/eth0/statistics give an interface's
 * counts whole past 2^32, each copied inside a `net_lock` bracket. */
static void netdev_rows(void)
{
    static const char want[] =
        "  eth0: 4886718345 4294967297 7 12884901891 0 0 0 0 5000000000 8589934594 0 9 0 0 0 0\n";
    static const char *const stats[] = {"0\n", "0\n",          "4886718345\n", "12884901891\n",
                                        "7\n", "4294967297\n", "5000000000\n", "9\n",
                                        "0\n", "8589934594\n"};
    struct netdev nd;
    struct procfs_text text;
    const char *line;
    int len, i;

    k_memset(&nd, 0, sizeof(nd));
    k_memcpy(nd.name, "eth0", 5);
    nd.index = 2;
    nd.ops = &iface_ops;
    nd.rx_bytes = 0x123456789ull;
    nd.rx_packets = 0x100000001ull;
    nd.rx_errors = 7;
    nd.rx_dropped = 0x300000003ull;
    nd.tx_bytes = 5000000000ull;
    nd.tx_packets = 0x200000002ull;
    nd.tx_dropped = 9;
    iface = &nd;
    /* The text is not terminated: each row compares its last `len` bytes. */
    procfs_text_init(&text);
    len = procfs_gen_netdev(&text);
    line = len >= (int)sizeof(want) - 1 ? text.buf + len - (sizeof(want) - 1) : 0;
    check(line && __builtin_memcmp(line, want, sizeof(want) - 1) == 0,
          "/proc/net/dev gives the counts past 2^32 whole");
    if (line && __builtin_memcmp(line, want, sizeof(want) - 1) != 0) {
        printf("procfs-hosttest: /proc/net/dev: %.*s", (int)(sizeof(want) - 1), line);
    }
    kfree(text.buf);
    for (i = 0; i < 10; i++) {
        procfs_text_init(&text);
        len = procfs_gen_netattr(&text, SYSFS_NET_STATS, 2, i);
        check(len == (int)__builtin_strlen(stats[i]) &&
                  __builtin_memcmp(text.buf, stats[i], (unsigned long)len) == 0,
              "a file of /sys/class/net/eth0/statistics gives its count whole");
        kfree(text.buf);
    }
    check(lock_depth == 0, "each copy of the counts closes its net_lock bracket");
    iface = 0;
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
/** Whether the uevents sent since `from` hold the variable `var`, a whole
 * NUL-ended string, in the uevent numbered `nth` from there. */
static int uevent_has(uint32_t from, int nth, const char *var)
{
    uint32_t at = from, n = (uint32_t)__builtin_strlen(var) + 1;

    for (; nth > 0 && at < uevents_len; at++) {
        if (uevents[at] == '\0' && at + 1 < uevents_len &&
            __builtin_strncmp(uevents + at + 1, "SEQNUM=", 7) == 0) {
            at += 1 + (uint32_t)__builtin_strlen(uevents + at + 1);
            nth--;
        }
    }
    for (; at + n <= uevents_len; at += (uint32_t)__builtin_strlen(uevents + at) + 1) {
        if (__builtin_memcmp(uevents + at, var, n) == 0) {
            return 1;
        }
        if (__builtin_strncmp(uevents + at, "SEQNUM=", 7) == 0) {
            return 0;
        }
    }
    return 0;
}

/** The text of the registered attribute `h`, as an open renders it, NUL-
 * terminated into `out` of `cap` bytes; its length, or the error. */
static int reg_text(int h, char *out, int cap)
{
    struct procfs_text text;
    struct vpath vp;
    int key = 0, len;

    k_memset(&vp, 0, sizeof(vp));
    vp.kind = VNODE_SYS_FILE;
    vp.sysent = SYSFS_REG;
    vp.syskey = h;
    procfs_text_init(&text);
    len = procfs_gen_sysfile(&text, &vp, &key);
    if (len >= 0 && len < cap) {
        k_memcpy(out, text.buf, (size_t)len);
        out[len] = '\0';
    }
    kfree(text.buf);
    return len;
}

/** Whether the strings `a` and `b` are equal. */
static int eq(const char *a, const char *b)
{
    return __builtin_strcmp(a, b) == 0;
}

/** The text of the attribute `attr` of the fixed /sys directory `node`,
 * with device `key`, as an open renders it, NUL-terminated into `out` of
 * `cap` bytes; its length, or the error. */
static int sys_text(int node, int key, int attr, char *out, int cap)
{
    struct procfs_text text;
    struct vpath vp;
    int k = 0, len;

    k_memset(&vp, 0, sizeof(vp));
    vp.kind = VNODE_SYS_FILE;
    vp.sysent = node;
    vp.syskey = key;
    vp.procfile = attr;
    procfs_text_init(&text);
    len = procfs_gen_sysfile(&text, &vp, &k);
    if (len >= 0 && len < cap) {
        k_memcpy(out, text.buf, (size_t)len);
        out[len] = '\0';
    }
    kfree(text.buf);
    return len;
}

/**
 * The files Wine reads below /sys/devices/system and /sys/class/dmi, in
 * Linux's text: a cache's size with its K, its type by name and its
 * sharing processors; the one NUMA node; the clock sources, each with a
 * space after it; the DMI fields the table holds, and ENOENT for one it
 * does not; and modalias, whose fields lose their spaces and the colons.
 */
static void system_rows(void)
{
    char out[256];

    check(sys_text(SYSFS_CPU_CACHE_INDEX, 0, 7, out, (int)sizeof(out)) > 0 && eq(out, "32K\n"),
          "an L1 data cache of 64 sets of 8 ways of 64 bytes is 32K");
    check(sys_text(SYSFS_CPU_CACHE_INDEX, 1, 7, out, (int)sizeof(out)) > 0 && eq(out, "4096K\n"),
          "the L2's size");
    check(sys_text(SYSFS_CPU_CACHE_INDEX, 0, 8, out, (int)sizeof(out)) > 0 && eq(out, "Data\n"),
          "a data cache's type");
    check(sys_text(SYSFS_CPU_CACHE_INDEX, 1, 8, out, (int)sizeof(out)) > 0 && eq(out, "Unified\n"),
          "a unified cache's type");
    check(sys_text(SYSFS_CPU_CACHE_INDEX, 0, 9, out, (int)sizeof(out)) > 0 && eq(out, "8\n"),
          "a cache's ways");
    check(sys_text(SYSFS_CPU_CACHE_INDEX, 0, 5, out, (int)sizeof(out)) > 0 && eq(out, "0\n"),
          "a cache's sharing processors as a list");
    check(sys_text(SYSFS_CPU_CACHE_INDEX, 2, 7, out, (int)sizeof(out)) == -ENOENT,
          "a cache the processor does not have is ENOENT");
    check(sys_text(SYSFS_NODE0, 0, 2, out, (int)sizeof(out)) > 0 && eq(out, "10\n"),
          "node0 is at distance 10 from itself");
    check(sys_text(SYSFS_NODE, 0, 4, out, (int)sizeof(out)) > 0 && eq(out, "0\n"),
          "node 0 is the one node possible");
    check(sys_text(SYSFS_CLOCKSOURCE0, 0, 0, out, (int)sizeof(out)) > 0 && eq(out, "tsc hpet \n"),
          "the clock sources, each with a space after it");
    check(sys_text(SYSFS_CLOCKSOURCE0, 0, 1, out, (int)sizeof(out)) > 0 && eq(out, "tsc\n"),
          "the clock source in use");
    check(sys_text(SYSFS_DMI_ID, 0, DMI_PRODUCT_NAME, out, (int)sizeof(out)) > 0 &&
              eq(out, "Standard PC (i440FX + PIIX, 1996)\n"),
          "a DMI field's text");
    check(sys_text(SYSFS_DMI_ID, 0, DMI_BOARD_NAME, out, (int)sizeof(out)) == -ENOENT &&
              !sysfs_attr_present(SYSFS_DMI_ID, DMI_BOARD_NAME) &&
              sysfs_attr_present(SYSFS_DMI_ID, SYSFS_DMI_MODALIAS),
          "a DMI field the table does not hold is not there");
    check(sys_text(SYSFS_DMI_ID, 0, SYSFS_DMI_MODALIAS, out, (int)sizeof(out)) > 0 &&
              eq(out, "dmi:bvnSeaBIOS:br0.0:svnQEMU:pnStandardPC(i440FX+PIIX,1996):ct1:\n"),
          "modalias, as Linux's dmi-id.c makes it");
}

/** The entry below `dir` named `name`. */
static int child(int dir, const char *name)
{
    return sysfs_node_child(dir, name, (int)__builtin_strlen(name));
}

/** Whether the link `h` reads `want`. */
static int link_reads(int h, const char *want)
{
    char text[128];
    int n = sysfs_link_text(h, text, (int)sizeof(text) - 1);

    if (n < 0) {
        return 0;
    }
    text[n] = '\0';
    return __builtin_strcmp(text, want) == 0;
}

/*
 * A virtio_gpu device at 0000:00:02.0, card 0 with render node 128, and its
 * one connector, Virtual-1, connected, with one mode and an EDID, as the
 * DRM rows register them.
 */
static const struct drm_driver row_driver = {DRM_IDENT_VIRTIO_GPU};

/** The console's hotplug, which drm_kms_hotplug_event calls: nothing. */
void drm_client_dev_hotplug(struct drm_device *dev)
{
    (void)dev;
}

static struct drm_device row_dev;
static struct drm_connector row_conn;
static struct drm_display_mode row_mode;
static struct drm_property_blob row_edid;
static uint8_t row_edid_bytes[128];
static pci_device_t row_pci;

/* The commit lock and the probe of drm_mode.c, which a write of a
 * connector's status calls: the probe finds the connector off when forced
 * off and connected otherwise, as for a sink with no detect, and counts
 * itself. */
static int row_probes;

int32_t drm_modeset_lock(struct drm_device *dev)
{
    (void)dev;
    return 0;
}

void drm_modeset_unlock(struct drm_device *dev)
{
    (void)dev;
}

int drm_helper_probe_single_connector_modes(struct drm_connector *connector, uint32_t max_width,
                                            uint32_t max_height)
{
    (void)max_width;
    (void)max_height;
    row_probes++;
    connector->status =
        connector->force == DRM_FORCE_OFF ? DRM_CONNECTOR_DISCONNECTED : DRM_CONNECTOR_CONNECTED;
    return 0;
}

/** Fill the device the DRM rows register. */
static void row_device(void)
{
    int i;

    row_pci.bus = 0;
    row_pci.device = 2;
    row_pci.function = 0;
    row_dev.driver = &row_driver;
    row_dev.features = row_driver.ident.features;
    row_dev.index = 0;
    row_dev.pci = &row_pci;
    k_memcpy(row_dev.unique, "pci:0000:00:02.0", 17);
    for (i = 0; i < 128; i++) {
        row_edid_bytes[i] = (uint8_t)(i * 7);
    }
    row_edid.length = 128;
    row_edid.data = row_edid_bytes;
    k_memcpy(row_mode.info.name, "1024x768", 9);
    row_conn.dev = &row_dev;
    row_conn.index = 0;
    row_conn.base.id = 34;
    k_memcpy(row_conn.name, "Virtual-1", 10);
    row_conn.status = DRM_CONNECTOR_CONNECTED;
    row_conn.modes = &row_mode;
    row_conn.edid = &row_edid;
    row_dev.mode_config.num_connector = 1;
    row_dev.mode_config.connectors[0] = &row_conn;
}

/* The entries the DRM rows find after registering. */
static int row_class, row_char, row_pci_dir, row_card, row_render, row_conn_dir;

/** Whether the registered attribute `h` reads `want` whole. */
static int reg_is(int h, const char *want)
{
    char text[256];

    return reg_text(h, text, (int)sizeof(text)) >= 0 && __builtin_strcmp(text, want) == 0;
}

/**
 * A registered DRM device sends an `add` uevent for its card, then its
 * render node, then its connector, each with the variables udev reads.
 */
static void drm_register_rows(void)
{
    row_device();
    check(drm_sysfs_register(&row_dev) == 0, "a DRM device registers its /sys entries");
    check(uevents_sent == 3 &&
              __builtin_strcmp(uevents, "add@/devices/pci0000:00/0000:00:02.0/drm/card0") == 0,
          "with the uevent add of its card first");
    check(uevent_has(0, 0, "ACTION=add") && uevent_has(0, 0, "SUBSYSTEM=drm") &&
              uevent_has(0, 0, "DEVPATH=/devices/pci0000:00/0000:00:02.0/drm/card0") &&
              uevent_has(0, 0, "MAJOR=226") && uevent_has(0, 0, "MINOR=0") &&
              uevent_has(0, 0, "DEVNAME=dri/card0") && uevent_has(0, 0, "DEVTYPE=drm_minor") &&
              uevent_has(0, 0, "SEQNUM=1"),
          "which carries the card's variables and its sequence number");
    check(uevent_has(0, 1, "DEVNAME=dri/renderD128") && uevent_has(0, 1, "MINOR=128") &&
              uevent_has(0, 2, "DEVTYPE=drm_connector"),
          "then the render node's and the connector's");
}

/**
 * The card, the render node and the connector are below the PCI function's
 * `drm`, linked from /sys/class/drm and each node from /sys/dev/char, with
 * Linux's relative link texts.
 */
static void drm_link_rows(void)
{
    row_class = sysfs_anchor_find(SYSFS_CLASS_DRM, 0);
    row_char = sysfs_anchor_find(SYSFS_DEV_CHAR, 0);
    row_pci_dir = sysfs_anchor_find(SYSFS_PCI_DEVICE, 2 << 3);
    row_card = child(child(row_pci_dir, "drm"), "card0");
    row_render = child(child(row_pci_dir, "drm"), "renderD128");
    row_conn_dir = child(row_card, "card0-Virtual-1");
    check(row_card > 0 && row_render > 0 && row_conn_dir > 0,
          "card0, renderD128 and the connector are there");
    check(
        link_reads(child(row_class, "card0"), "../../devices/pci0000:00/0000:00:02.0/drm/card0") &&
            link_reads(child(row_class, "renderD128"),
                       "../../devices/pci0000:00/0000:00:02.0/drm/renderD128") &&
            link_reads(child(row_class, "card0-Virtual-1"),
                       "../../devices/pci0000:00/0000:00:02.0/drm/card0/card0-Virtual-1"),
        "/sys/class/drm links each node and the connector");
    check(link_reads(child(row_char, "226:0"), "../../devices/pci0000:00/0000:00:02.0/drm/card0") &&
              link_reads(child(row_char, "226:128"),
                         "../../devices/pci0000:00/0000:00:02.0/drm/renderD128"),
          "/sys/dev/char links each node by its numbers");
    check(link_reads(child(row_card, "device"), "../../../0000:00:02.0") &&
              link_reads(child(row_card, "subsystem"), "../../../../../class/drm") &&
              link_reads(child(row_conn_dir, "device"), "../../card0"),
          "a node's device is the PCI function and its subsystem the class");
}

/** The files of the nodes and the connector read as Linux's do. */
static void drm_file_rows(void)
{
    char text[256];

    check(reg_is(child(row_card, "dev"), "226:0\n") &&
              reg_is(child(row_render, "dev"), "226:128\n"),
          "dev gives each node's numbers");
    check(reg_is(child(row_card, "uevent"),
                 "MAJOR=226\nMINOR=0\nDEVNAME=dri/card0\nDEVTYPE=drm_minor\n"),
          "a node's uevent file has its variables");
    check(reg_is(child(row_conn_dir, "status"), "connected\n") &&
              reg_is(child(row_conn_dir, "enabled"), "disabled\n") &&
              reg_is(child(row_conn_dir, "dpms"), "Off\n") &&
              reg_is(child(row_conn_dir, "modes"), "1024x768\n") &&
              reg_is(child(row_conn_dir, "connector_id"), "34\n"),
          "the connector gives its status, enabled, dpms, modes and ID");
    check(reg_text(child(row_conn_dir, "edid"), text, (int)sizeof(text)) == 128 &&
              __builtin_memcmp(text, row_edid_bytes, 128) == 0,
          "and the sink's EDID whole");
}

/**
 * A hotplug, a write of a uevent file and a forced status send their
 * uevents, and unregistering sends `remove` and leaves nothing.
 */
static void drm_event_rows(void)
{
    uint32_t mark = uevents_len;
    int sent = uevents_sent;
    int probes;

    drm_kms_hotplug_event(&row_dev);
    check(uevents_sent == sent + 1 &&
              __builtin_strcmp(uevents + mark,
                               "change@/devices/pci0000:00/0000:00:02.0/drm/card0") == 0 &&
              uevent_has(mark, 0, "HOTPLUG=1") && uevent_has(mark, 0, "DEVNAME=dri/card0"),
          "a hotplug is the card's change with HOTPLUG=1");
    mark = uevents_len;
    check(sysfs_node_store(child(row_card, "uevent"), "add\n", 4) == 4 &&
              __builtin_strncmp(uevents + mark, "add@", 4) == 0 &&
              uevent_has(mark, 0, "SYNTH_UUID=0"),
          "a write of add to a uevent file sends add again");
    check(sysfs_node_store(child(row_card, "uevent"), "plug\n", 5) == -EINVAL,
          "and another word is refused");
    mark = uevents_len;
    probes = row_probes;
    check(sysfs_node_store(child(row_conn_dir, "status"), "off\n", 4) == 4 &&
              row_conn.force == DRM_FORCE_OFF && uevent_has(mark, 0, "HOTPLUG=1"),
          "a forced status is set and told as a hotplug");
    check(row_probes == probes + 1 && row_conn.status == DRM_CONNECTOR_DISCONNECTED,
          "and the connector is probed again at once, as status_store does");
    sent = uevents_sent;
    check(sysfs_node_store(child(row_conn_dir, "status"), "off", 3) == 3 &&
              row_probes == probes + 1 && uevents_sent == sent,
          "the same force again probes nothing and sends nothing");
    check(sysfs_node_store(child(row_conn_dir, "status"), "detect\n", 7) == 7 &&
              row_conn.force == DRM_FORCE_UNSPECIFIED && row_probes == probes + 2,
          "detect probes again");
    mark = uevents_len;
    sent = uevents_sent;
    drm_sysfs_unregister(&row_dev);
    check(uevents_sent == sent + 3 && __builtin_strncmp(uevents + mark, "remove@", 7) == 0 &&
              uevent_has(mark, 0, "DEVTYPE=drm_connector") &&
              uevent_has(mark, 2, "DEVNAME=dri/card0"),
          "unregistering sends remove for the connector, the render node and the card");
    check(!child(row_class, "card0") && !child(row_char, "226:0") && !child(row_pci_dir, "drm") &&
              sysfs_node_kind(row_card) == 0 && sysfs_node_kind(row_conn_dir) == 0,
          "and leaves no entry");
}

/**
 * A DRM device on a real PCI function that reports another identity, as
 * the fake Haswell does on QEMU's device, shows that identity in the
 * function's files, its uevent and the header of its config, and the
 * function's own once the device is gone.
 */
static void drm_ident_rows(void)
{
    static const struct sysfs_pci_ident haswell = {0x8086,   0x0a16, 0x17aa, 0x2214,
                                                   0x030000, 0x0b,   1};
    static pci_device_t fn = {0};
    static struct drm_device dev;
    struct procfs_text t;
    int rendered;

    fn.device = 4;
    dev.driver = &row_driver;
    dev.index = 1;
    dev.pci = &fn;
    dev.pci_ident = &haswell;
    procfs_text_init(&t);
    check(procfs_gen_pciattr(&t, 0x20, SYSFS_PCI_VENDOR) == 7 &&
              __builtin_memcmp(t.buf, "0x1af4\n", 7) == 0,
          "a real function gives its own vendor");
    check(drm_sysfs_register(&dev) == 0, "a device that reports a PCI identity registers");
    t.len = 0;
    check(procfs_gen_pciattr(&t, 0x20, SYSFS_PCI_DEVICE_ID) == 7 &&
              __builtin_memcmp(t.buf, "0x0a16\n", 7) == 0,
          "its function then gives the reported device");
    t.len = 0;
    check(procfs_gen_pciattr(&t, 0x20, SYSFS_PCI_SUBSYSTEM_VENDOR) == 7 &&
              __builtin_memcmp(t.buf, "0x17aa\n", 7) == 0,
          "and subsystem vendor");
    t.len = 0;
    rendered = procfs_gen_pciconfig(&t, 0x20);
    check(rendered == 256 && (uint8_t)t.buf[0] == 0x86 && (uint8_t)t.buf[1] == 0x80 &&
              (uint8_t)t.buf[2] == 0x16 && (uint8_t)t.buf[3] == 0x0a && t.buf[4] == 0x07 &&
              (uint8_t)t.buf[8] == 0x0b,
          "and its config the reported IDs over the device's own command register");
    t.len = 0;
    check(procfs_gen_uevent(&t, SYSFS_PCI_DEVICE, 0x20) > 0 &&
              __builtin_strstr(t.buf, "PCI_ID=8086:0A16\n"),
          "and its uevent the reported IDs");
    check(sysfs_device_key(SYSFS_PCI_DEVICE, 0) == -1,
          "and a reported identity at a real function's address is not listed again");
    drm_sysfs_unregister(&dev);
    t.len = 0;
    check(procfs_gen_pciattr(&t, 0x20, SYSFS_PCI_DEVICE_ID) == 7 &&
              __builtin_memcmp(t.buf, "0x1050\n", 7) == 0,
          "and the function's own once the device is gone");
    kfree(t.buf);
}

/**
 * A write of a PCI function's uevent sends that action with the function's
 * variables, as udev's coldplug writes it: a bare action carries
 * SYNTH_UUID=0, a UUID and arguments are passed on, and another word, a
 * short UUID or a function that is gone is refused.
 */
static void pci_uevent_rows(void)
{
    static const char change[] = "change 01234567-89ab-cdef-0123-456789abcdef SEAT=seat0\n";
    uint32_t mark = uevents_len;

    check(procfs_uevent_write(SYSFS_PCI_DEVICE, 0x20, 0, "add\n", 4) == 4 &&
              __builtin_strcmp(uevents + mark, "add@/devices/pci0000:00/0000:00:04.0") == 0 &&
              uevent_has(mark, 0, "SUBSYSTEM=pci") && uevent_has(mark, 0, "SYNTH_UUID=0") &&
              uevent_has(mark, 0, "PCI_ID=1AF4:1050") &&
              uevent_has(mark, 0, "PCI_SLOT_NAME=0000:00:04.0"),
          "a write of add to a PCI function's uevent sends its add");
    mark = uevents_len;
    check(procfs_uevent_write(SYSFS_PCI_DEVICE, 0x20, 0, change, sizeof(change) - 1) ==
                  (int32_t)sizeof(change) - 1 &&
              uevent_has(mark, 0, "ACTION=change") &&
              uevent_has(mark, 0, "SYNTH_UUID=01234567-89ab-cdef-0123-456789abcdef") &&
              uevent_has(mark, 0, "SYNTH_ARG_SEAT=seat0"),
          "and passes a UUID and arguments on");
    mark = uevents_len;
    check(procfs_uevent_write(SYSFS_PCI_DEVICE, 0x20, 0, "plug\n", 5) == -EINVAL &&
              procfs_uevent_write(SYSFS_PCI_DEVICE, 0x20, 0, "add 1234\n", 9) == -EINVAL &&
              procfs_uevent_write(SYSFS_PCI_DEVICE, 0x28, 0, "add\n", 4) == -ENOENT &&
              uevents_len == mark,
          "and refuses another word, a short UUID and a function that is gone");
}

/* What the driver's attribute in the registration rows holds. */
static char override[16];

/** The driver's attribute: what was written last. */
static int32_t show_override(void *obj, int arg, char *buf, uint32_t cap)
{
    uint32_t n = (uint32_t)__builtin_strlen((const char *)obj);

    (void)arg;
    if (n <= cap) {
        k_memcpy(buf, obj, n);
    }
    return (int32_t)n;
}

/** Take a write of the driver's attribute, up to 15 bytes. */
static int32_t store_override(void *obj, int arg, const char *buf, uint32_t len)
{
    (void)arg;
    if (len >= sizeof(override)) {
        return -EINVAL;
    }
    k_memcpy(obj, buf, len);
    ((char *)obj)[len] = '\0';
    return (int32_t)len;
}

/**
 * A driver adds an attribute it reads and writes and a link of its own
 * below a PCI function, and a directory below /sys/kernel the link leads
 * to; both read back as an open renders them. A function a driver reports
 * with no device behind it has the identity it gave in each file.
 */
static void sysfs_reg_rows(void)
{
    static const struct sysfs_attr attr = {"driver_override", 0644u, show_override, store_override};
    struct sysfs_pci_ident id;
    struct procfs_text t;
    char text[64];
    int pci = sysfs_anchor(SYSFS_PCI_DEVICE, 0x10), groups, group, h, link;

    groups = sysfs_add_dir(sysfs_anchor(SYSFS_KERNEL, 0), "iommu_groups");
    group = sysfs_add_dir(groups, "3");
    h = sysfs_add_attr(pci, &attr, override, 0);
    link = sysfs_add_link(pci, "iommu_group", group);
    check(pci > 0 && groups > 0 && group > 0 && h > 0 && link > 0,
          "a driver adds an attribute, directories and a link");
    check(sysfs_add_attr(pci, &attr, override, 0) == -EEXIST, "a name is taken once");
    check(sysfs_node_writable(h) && sysfs_node_mode(h) == 0644u,
          "the attribute takes writes, as its mode says");
    check(sysfs_node_store(h, "vfio-pci", 8) == 8 && reg_text(h, text, (int)sizeof(text)) == 8 &&
              __builtin_strcmp(text, "vfio-pci") == 0,
          "and reads back what was written");
    check(link_reads(link, "../../../kernel/iommu_groups/3"), "the link reads relative to itself");
    sysfs_remove(groups);
    check(sysfs_node_kind(group) == 0 && sysfs_link_text(link, text, (int)sizeof(text)) == -ENOENT,
          "removing a directory removes what is below it, and its links lead nowhere");
    sysfs_remove(link);
    sysfs_remove(h);
    check(sysfs_node_count(pci) == 0, "and the function has nothing left of the driver's");

    k_memset(&id, 0, sizeof(id));
    id.vendor = 0x8086;
    id.device = 0x0a16;
    id.subsystem_vendor = 0x17aa;
    id.subsystem_device = 0x2214;
    id.class = 0x030000;
    id.revision = 0x0b;
    id.boot_vga = 1;
    check(sysfs_pci_virtual_add(0xA0, &id) == 0 && sysfs_pci_virtual_add(0xA0, &id) == -EEXIST,
          "a driver reports a PCI function once");
    check(sysfs_device_exists(SYSFS_PCI_DEVICE, 0xA0) &&
              sysfs_device_key(SYSFS_PCI_DEVICE, 0) == 0xA0,
          "which is there and listed");
    procfs_text_init(&t);
    check(procfs_gen_pciattr(&t, 0xA0, SYSFS_PCI_VENDOR) == 7 &&
              __builtin_memcmp(t.buf, "0x8086\n", 7) == 0,
          "its vendor is the one reported");
    t.len = 0;
    check(procfs_gen_pciattr(&t, 0xA0, SYSFS_PCI_DEVICE_ID) == 7 &&
              __builtin_memcmp(t.buf, "0x0a16\n", 7) == 0,
          "and its device");
    t.len = 0;
    check(procfs_gen_pciattr(&t, 0xA0, SYSFS_PCI_CLASS) == 9 &&
              __builtin_memcmp(t.buf, "0x030000\n", 9) == 0,
          "and its class");
    t.len = 0;
    check(procfs_gen_pciattr(&t, 0xA0, SYSFS_PCI_BOOT_VGA) == 2 &&
              __builtin_memcmp(t.buf, "1\n", 2) == 0,
          "and its boot_vga");
    t.len = 0;
    check(procfs_gen_uevent(&t, SYSFS_PCI_DEVICE, 0xA0) > 0 && t.buf &&
              __builtin_strstr(t.buf, "PCI_ID=8086:0A16\n") &&
              __builtin_strstr(t.buf, "PCI_SLOT_NAME=0000:00:14.0\n"),
          "and its uevent the IDs and slot libdrm parses");
    kfree(t.buf);
    sysfs_pci_virtual_remove(0xA0);
    check(!sysfs_device_exists(SYSFS_PCI_DEVICE, 0xA0), "a withdrawn function is gone");
}

/** Whether the uevents sent since `from` hold the bytes of `text`. */
static int sent_has(uint32_t from, const char *text)
{
    uint32_t n = (uint32_t)__builtin_strlen(text), at;

    for (at = from; at + n <= uevents_len; at++) {
        if (__builtin_memcmp(uevents + at, text, n) == 0) {
            return 1;
        }
    }
    return 0;
}

/** Set bit `n` of `bits`. */
static void bit_set(uint8_t *bits, unsigned n)
{
    bits[n >> 3] |= (uint8_t)(1u << (n & 7));
}

/** Store the 64-bit `word` at word `i` of `bits`, its lowest byte first. */
static void word_set(uint8_t *bits, int i, uint64_t word)
{
    int b;

    for (b = 0; b < 8; b++) {
        bits[i * 8 + b] = (uint8_t)(word >> (8 * b));
    }
}

/**
 * Slot 0 an AT keyboard with the keys Linux's atkbd gives a translated
 * one, and slot 1 a USB boot mouse with five buttons, a wheel and a tilt
 * wheel in detents and in 120ths, the registrations numbered `gen` and
 * one more.
 */
static void input_devices(uint32_t gen)
{
    struct evdev_desc *kb = &input_devs[0], *ms = &input_devs[1];
    unsigned n;

    k_memset(input_devs, 0, sizeof(input_devs));
    kb->gen = gen;
    k_strncpy(kb->name, "AT Translated Set 2 keyboard", EVDEV_TEXT_MAX);
    k_strncpy(kb->phys, "isa0060/serio0/input0", EVDEV_TEXT_MAX);
    kb->id[0] = 0x11;
    kb->id[1] = 0x1;
    kb->id[2] = 0x1;
    kb->id[3] = 0xab41;
    kb->ev[0] = 0x13;
    kb->ev[2] = 0x12;
    word_set(kb->key, 0, 0xfffffffffffffffeull);
    word_set(kb->key, 1, 0xfeffffdfffefffffull);
    word_set(kb->key, 2, 0x03803078f800d001ull);
    word_set(kb->key, 3, 0x402000000ull);
    kb->msc[0] = 0x10;
    kb->led[0] = 0x07;
    ms->gen = gen + 1;
    k_strncpy(ms->name, "USB HIDBP Mouse 0627:0001", EVDEV_TEXT_MAX);
    k_strncpy(ms->phys, "usb-1/input0", EVDEV_TEXT_MAX);
    ms->id[0] = 0x03;
    ms->id[1] = 0x0627;
    ms->id[2] = 0x0001;
    ms->ev[0] = 0x07;
    for (n = BTN_LEFT; n <= BTN_EXTRA; n++) {
        bit_set(ms->key, n);
    }
    bit_set(ms->rel, REL_X);
    bit_set(ms->rel, REL_Y);
    bit_set(ms->rel, REL_HWHEEL);
    bit_set(ms->rel, REL_WHEEL);
    bit_set(ms->rel, REL_WHEEL_HI_RES);
    bit_set(ms->rel, REL_HWHEEL_HI_RES);
    bit_set(ms->prop, INPUT_PROP_POINTER);
    input_present[0] = input_present[1] = 1;
}

/* The entries the input rows find. */
static int in_class, in_kbd, in_mouse, in_serio, in_iface;

static void input_link_rows(void);

/**
 * A sync publishes the keyboard below the i8042's first port and the mouse
 * below its USB interface, sending "add" for each parent it made, then for
 * inputN and eventN, and links each from /sys/class/input and its event
 * node from /sys/dev/char, as Linux's input core and evdev do.
 */
static void input_publish_rows(void)
{
    uint32_t mark = uevents_len;
    int sent = uevents_sent, platform;

    input_devices(100);
    usb_mouse_in = 1;
    evdev_sysfs_sync();
    platform = sysfs_anchor_find(SYSFS_PLATFORM, 0);
    in_class = child(sysfs_anchor_find(SYSFS_CLASS, 0), "input");
    in_serio = child(child(platform, "i8042"), "serio0");
    in_kbd = child(child(in_serio, "input"), "input0");
    in_iface = child(sysfs_anchor_find(SYSFS_USB_DEVICE, 1 << 20), "1-1:1.0");
    in_mouse = child(child(in_iface, "input"), "input1");
    check(uevents_sent == sent + 7 && in_class > 0 && in_kbd > 0 && in_mouse > 0,
          "a sync publishes a keyboard and a mouse with seven uevents");
    check(__builtin_strcmp(uevents + mark, "add@/devices/platform/i8042") == 0 &&
              uevent_has(mark, 1, "DEVPATH=/devices/platform/i8042/serio0") &&
              uevent_has(mark, 1, "SUBSYSTEM=serio") && uevent_has(mark, 1, "DRIVER=atkbd") &&
              uevent_has(mark, 2, "DEVPATH=/devices/platform/i8042/serio0/input/input0") &&
              uevent_has(mark, 2, "SUBSYSTEM=input") && uevent_has(mark, 2, "EV=120013") &&
              uevent_has(mark, 2, "NAME=\"AT Translated Set 2 keyboard\"") &&
              uevent_has(mark, 3, "DEVNAME=input/event0") &&
              uevent_has(mark, 4, "DEVPATH=/devices/usb/1-1/1-1:1.0") &&
              uevent_has(mark, 4, "SUBSYSTEM=usb") &&
              uevent_has(mark, 4, "DEVTYPE=usb_interface") &&
              uevent_has(mark, 5, "DEVPATH=/devices/usb/1-1/1-1:1.0/input/input1") &&
              uevent_has(mark, 6, "DEVPATH=/devices/usb/1-1/1-1:1.0/input/input1/event1"),
          "the i8042, its port, the keyboard, its event node, the USB interface, the mouse "
          "and its event node are added in that order, each with its subsystem");
    input_link_rows();
}

/** The links of the input rows' devices: /sys/class/input's, /sys/dev/char's,
 * each device's to its parent and subsystem, and the buses' to their
 * devices. */
static void input_link_rows(void)
{
    check(
        link_reads(child(in_class, "input0"), "../../devices/platform/i8042/serio0/input/input0") &&
            link_reads(child(in_class, "event0"),
                       "../../devices/platform/i8042/serio0/input/input0/event0") &&
            link_reads(child(in_class, "input1"), "../../devices/usb/1-1/1-1:1.0/input/input1") &&
            link_reads(child(sysfs_anchor_find(SYSFS_DEV_CHAR, 0), "13:65"),
                       "../../devices/usb/1-1/1-1:1.0/input/input1/event1"),
        "/sys/class/input links inputN and eventN, and /sys/dev/char each event node");
    check(link_reads(child(in_kbd, "device"), "../../../serio0") &&
              link_reads(child(in_kbd, "subsystem"), "../../../../../../class/input") &&
              link_reads(child(in_mouse, "device"), "../../../1-1:1.0") &&
              link_reads(child(child(in_kbd, "event0"), "device"), "../../input0") &&
              link_reads(child(in_serio, "subsystem"), "../../../../bus/serio") &&
              link_reads(child(in_iface, "subsystem"), "../../../../bus/usb") &&
              link_reads(child(sysfs_anchor_find(SYSFS_USB_LINKS, 0), "1-1:1.0"),
                         "../../../devices/usb/1-1/1-1:1.0") &&
              link_reads(child(sysfs_anchor_find(SYSFS_PLATFORM_LINKS, 0), "i8042"),
                         "../../../devices/platform/i8042"),
          "each device links its parent and its subsystem, and the buses link their devices");
}

/**
 * The files of inputN render the IDs, the texts and the capability masks
 * as Linux prints them: the kernel's longs in hexadecimal from the highest
 * set, and 32-bit words to an i386 program.
 */
static void input_file_rows(void)
{
    int caps = child(in_kbd, "capabilities"), mcaps = child(in_mouse, "capabilities");
    char text[2048];

    check(reg_is(child(caps, "ev"), "120013\n") &&
              reg_is(child(caps, "key"),
                     "402000000 3803078f800d001 feffffdfffefffff fffffffffffffffe\n") &&
              reg_is(child(caps, "msc"), "10\n") && reg_is(child(caps, "led"), "7\n") &&
              reg_is(child(caps, "rel"), "0\n") && reg_is(child(caps, "sw"), "0\n"),
          "a keyboard's capability masks read as Linux prints them");
    check(reg_is(child(mcaps, "ev"), "7\n") && reg_is(child(mcaps, "key"), "1f0000 0 0 0 0\n") &&
              reg_is(child(mcaps, "rel"), "1943\n") && reg_is(child(in_mouse, "properties"), "1\n"),
          "and a mouse's, with its buttons, wheels and pointer property");
    compat_caller = 1;
    check(reg_is(child(caps, "key"),
                 "4 2000000 3803078 f800d001 feffffdf ffefffff ffffffff fffffffe\n") &&
              reg_is(child(mcaps, "key"), "1f0000 0 0 0 0 0 0 0 0\n"),
          "to an i386 program in 32-bit words");
    compat_caller = 0;
    check(reg_is(child(in_kbd, "name"), "AT Translated Set 2 keyboard\n") &&
              reg_is(child(in_kbd, "phys"), "isa0060/serio0/input0\n") &&
              reg_is(child(in_kbd, "uniq"), "\n") &&
              reg_is(child(child(in_kbd, "id"), "bustype"), "0011\n") &&
              reg_is(child(child(in_kbd, "id"), "version"), "ab41\n"),
          "the name, place, serial and IDs");
    check(reg_is(child(in_mouse, "modalias"),
                 "input:b0003v0627p0001e0000-e0,1,2,k110,111,112,113,114,r0,1,6,8,B,C,amlsfw\n"),
          "the modalias names each code in capitals");
    check(reg_is(child(in_mouse, "uevent"),
                 "PRODUCT=3/627/1/0\nNAME=\"USB HIDBP Mouse 0627:0001\"\nPHYS=\"usb-1/input0\"\n"
                 "PROP=1\nEV=7\nKEY=1f0000 0 0 0 0\nREL=1943\nMODALIAS=input:b0003v0627p0001e0000-"
                 "e0,1,2,k110,111,112,113,114,r0,1,6,8,B,C,amlsfw\n"),
          "a mouse's uevent has the variables Linux's input core gives");
    check(reg_text(child(in_kbd, "uevent"), text, (int)sizeof(text)) > 0 &&
              __builtin_strstr(text, "MSC=10\nLED=7\nMODALIAS=input:b0011v0001p0001eAB41-e0,1,4,"
                                     "11,14,k71,72,") &&
              __builtin_strstr(text, ",ram4,l0,1,2,sfw\n"),
          "and a keyboard's its scan codes, LEDs and keys");
    check(reg_is(child(child(in_kbd, "event0"), "dev"), "13:64\n") &&
              reg_is(child(child(in_kbd, "event0"), "uevent"),
                     "MAJOR=13\nMINOR=64\nDEVNAME=input/event0\n"),
          "eventN gives its numbers and its name in /dev");
    check(reg_is(child(in_serio, "uevent"), "DRIVER=atkbd\nSERIO_TYPE=06\nSERIO_PROTO=00\n"
                                            "SERIO_ID=00\nSERIO_EXTRA=00\n"
                                            "MODALIAS=serio:ty06pr00id00ex00\n") &&
              reg_is(child(in_serio, "description"), "i8042 KBD port\n") &&
              reg_is(child(child(in_serio, "id"), "type"), "06\n"),
          "the i8042's port gives its driver, type and modalias");
    check(reg_is(child(in_iface, "uevent"),
                 "DEVTYPE=usb_interface\nDRIVER=usbhid\nPRODUCT=627/1/0\nTYPE=0/0/0\n"
                 "INTERFACE=3/1/2\nMODALIAS=usb:v0627p0001d0000dc00dsc00dp00ic03isc01ip02in00\n") &&
              reg_is(child(in_iface, "bInterfaceProtocol"), "02\n"),
          "the USB interface gives its class, protocol and modalias");
}

/**
 * A mouse unplugged sends "remove" for its event node, itself and its
 * interface and leaves nothing of them; plugged again it is input1 again,
 * the lowest number free. A device with every key has its keys cut from
 * its uevent's modalias, ended by "+,", so that the uevent fits in 2048
 * bytes, while its modalias file has them all.
 */
static void input_change_rows(void)
{
    uint32_t mark = uevents_len;
    int sent = uevents_sent, h;
    char text[4096];
    unsigned n;

    input_present[1] = 0;
    usb_mouse_in = 0;
    evdev_sysfs_sync();
    check(uevents_sent == sent + 3 &&
              __builtin_strcmp(uevents + mark,
                               "remove@/devices/usb/1-1/1-1:1.0/input/input1/event1") == 0 &&
              uevent_has(mark, 1, "DEVPATH=/devices/usb/1-1/1-1:1.0/input/input1") &&
              uevent_has(mark, 2, "DEVPATH=/devices/usb/1-1/1-1:1.0"),
          "an unplugged mouse sends remove for its event node, itself and its interface");
    check(!child(in_class, "input1") && !child(in_class, "event1") &&
              !child(sysfs_anchor_find(SYSFS_DEV_CHAR, 0), "13:65") &&
              sysfs_node_kind(in_iface) == 0 && sysfs_node_kind(in_mouse) == 0 &&
              !child(sysfs_anchor_find(SYSFS_USB_LINKS, 0), "1-1:1.0"),
          "and leaves nothing of them");
    input_present[1] = 1;
    input_devs[1].gen = 200;
    usb_mouse_in = 1;
    evdev_sysfs_sync();
    in_mouse = child(child(child(sysfs_anchor_find(SYSFS_USB_DEVICE, 1 << 20), "1-1:1.0"), "input"),
                     "input1");
    check(in_mouse > 0 && child(in_class, "input1") > 0, "plugged again it is input1 again");
    k_memset(&input_devs[2], 0, sizeof(input_devs[2]));
    input_devs[2].gen = 300;
    k_strncpy(input_devs[2].name, "every key", EVDEV_TEXT_MAX);
    input_devs[2].ev[0] = 0x03;
    for (n = 1; n < KEY_CNT; n++) {
        bit_set(input_devs[2].key, n);
    }
    input_present[2] = 1;
    mark = uevents_len;
    sent = uevents_sent;
    evdev_sysfs_sync();
    h = child(child(child(sysfs_anchor_find(SYSFS_DEVICES, 0), "virtual"), "input"), "input2");
    check(uevents_sent == sent + 2 && h > 0 &&
              uevent_has(mark, 0, "DEVPATH=/devices/virtual/input/input2") &&
              sent_has(mark, ",+,ramlsfw"),
          "a device with no parent is virtual, and its uevent's modalias is cut to fit");
    check(reg_text(child(h, "modalias"), text, (int)sizeof(text)) > 2048 &&
              __builtin_strstr(text, ",2FE,ramlsfw\n"),
          "while its modalias file names every key");
    input_present[0] = input_present[1] = input_present[2] = 0;
    usb_mouse_in = 0;
    evdev_sysfs_sync();
    check(!child(in_class, "input0") && !child(in_class, "input2") &&
              sysfs_node_count(in_class) == 0 && !child(in_serio, "input"),
          "every device gone, /sys/class/input is empty and the port keeps no input");
}

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
    netdev_rows();
    descriptor_links();
    task_ids();
    mem_descriptor();
    exe_links();
    exe_chroot();
    exe_entries();
    image_paths();
    reaped_during_read();
    system_rows();
    oom_values();
    oom_floor();
    oom_open_errors();
    drm_register_rows();
    drm_link_rows();
    drm_file_rows();
    drm_event_rows();
    drm_ident_rows();
    pci_uevent_rows();
    sysfs_reg_rows();
    input_publish_rows();
    input_file_rows();
    input_change_rows();
    if (!failures) {
        printf("procfs-hosttest: result: pass: stat allocation failure and times, descriptor "
               "links, task ids, the mem descriptor's counts, the exe link, a read of a reaped "
               "process's arguments, oom_score_adj, the paths of the maps, the uptime idle "
               "sum, an interface's counts, a DRM device's /sys entries and uevents, and a "
               "driver's attribute, link and PCI identity, writes of a PCI function's uevent, and "
               "an input device's /sys entries, capability masks and uevents\n");
    }
    return failures != 0;
}
