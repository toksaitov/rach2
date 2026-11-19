/**
 * procfs_hosttest.c: the files rach2/sys/fs/procfs.c makes: the stat text
 * and its allocation, /proc/uptime, the CPU times of a process and of a
 * thread, task ids, the syscall, wchan and schedstat files, descriptor links,
 * /proc/PID/mem, /proc/PID/exe, the paths of /proc/PID/maps, a read of a
 * process's arguments while the process is reaped, a read of its stat,
 * status and numa_maps while it is reaped as the read waits for its address
 * space's lock, a read of its maps while it is reaped as the program's path
 * is built, a write to its autogroup file, oom_score_adj, the
 * cache, NUMA, clock source and DMI files of /sys, and the /sys entries
 * and uevents of a DRM device and of a driver's own attributes, links, PCI
 * identity and resources, a writable attribute's offset and the mode its
 * open is judged by, the directions an attribute's open refuses before the
 * capability overrides, a PCI function's resourceN files, and those of a PS/2
 * keyboard and a USB mouse and their records in /proc/bus/input
 * (evdev_sysfs.c).
 *
 * host-deps: tests/host/compat/vm_iter_host.c
 * host-deps: rach2/sys/fs/sysfs.c rach2/sys/drm/drm_sysfs.c
 * host-deps: rach2/sys/evdev_sysfs.c
 * host-deps: rach2/sys/fs/name.c tests/host/compat/spinlock_host.c
 * host-deps: tests/host/compat/kirq_none.c
 * host-deps: rach2/sys/lib/kstring.c rach2/sys/lib/kctypes.c
 * host-deps: rach2/sys/lib/kstd.c
 * host-deps: rach2/sys/tick.c
 * host-deps: tests/host/compat/fd_flat.c
 * host-deps: tests/host/compat/mmap_lock_host.c
 * host-cflags: -Wno-int-to-pointer-cast -Wno-pointer-to-int-cast
 * host-cflags: -DTICK_HZ=1000
 *
 * The rows of times and the uptime count whole hundredths of a second in
 * ticks, so the kernel's sources are built at 1000 ticks a second.
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
static int procfs_filesystem_at(int idx, const char **name);
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
static uint32_t procfs_ino_irq(int vector, int attr);
static uint32_t procfs_ino_irqdir(void);
static int procfs_irq_exists(int vector);
static uint32_t procfs_ino_console(void);
static uint32_t procfs_ino_filesystems(void);
static uint32_t procfs_ino_netroute(void);
static uint32_t procfs_ino_netarp(void);
static uint32_t procfs_ino_nettcp(void);
static uint32_t procfs_fd_ino(struct fd_entry *e);
static uint32_t procfs_ino_pid(int pid);
static uint32_t procfs_task_links(int pid);
static int procfs_pid_owner(int pid, uint32_t mode, uint32_t *uid, uint32_t *gid);
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
#include "mm/thp.c"
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

/* The frames the model's allocator manages, which only the THP boot rows
 * set. */
static size_t model_frames;

/** The frame allocator is absent from this model: its count is the rows'. */
size_t frame_total_count(void)
{
    return model_frames;
}

/* The policy's changes, which wake khugepaged in the kernel. */
static int thp_changes;

/** Count a change of the policy. */
void thp_policy_changed(void)
{
    thp_changes++;
}

/* The clock, which only the wakealarm rows start: whether it reads, the
 * seconds since 1970 it reads, the alarm set, -1 for none, and the alarms
 * that came. */
static int clock_up, clock_came;
static int64_t clock_now, clock_alarm = -1;

/** The clock reads only while a row has it up. */
int rtc_read_clock(rtc_time_t *out)
{
    (void)out;
    return clock_up ? 0 : -EIO;
}

/** The seconds the clock reads. */
int64_t rtc_seconds_of(const rtc_time_t *t)
{
    (void)t;
    return clock_now;
}

/** The monotonic clock, which /proc/PID/sched reads twice for its
 * clock-delta; it moves 25 ns at each reading. */
int64_t rtc_monotonic_ns(void)
{
    static int64_t now;

    now += 25;
    return now;
}

/* Whether the model's scheduler keeps its statistics, kernel.sched_schedstats,
 * and the depth of its run queues' lock, which every reading of a thread's
 * entity takes and gives back. */
int fair_schedstats;
static int sched_lock_depth;

/** Take the run queues' lock of the model. */
uint32_t sched_lock_take(void)
{
    sched_lock_depth++;
    return 0x200u;
}

/** Give back the run queues' lock of the model. */
void sched_lock_give(uint32_t flags)
{
    (void)flags;
    sched_lock_depth--;
}

/** The alarm set, -1 for none. */
int64_t devfs_rtc_wakealarm(void)
{
    return clock_alarm;
}

/** Set the alarm, raise it at once for a time not after now, or clear it
 * for a negative `at`, as devfs's does; -EIO while the clock is down. */
int32_t devfs_rtc_set_wakealarm(int64_t at)
{
    if (!clock_up) {
        return -EIO;
    }
    clock_came += at >= 0 && at <= clock_now;
    clock_alarm = at < 0 || at <= clock_now ? -1 : at;
    return 0;
}

/** No attribute the rows write sends a uevent. */
int sysfs_attr_uevent(int node, int attr)
{
    (void)node;
    (void)attr;
    return 0;
}

/** Whether a /sys file takes writes, as synthfs.c judges it: a registered
 * attribute by sysfs.c, and of the fixed files those of transparent huge
 * pages but the page-directory size and khugepaged's two counts. */
int sysfs_file_writable(int node, int key, int attr)
{
    return node == SYSFS_REG ? sysfs_node_writable(key)
           : node == SYSFS_THP_KHUGEPAGED
               ? attr != SYSFS_KHP_FULL_SCANS && attr != SYSFS_KHP_COLLAPSED
               : node == SYSFS_THP && attr != SYSFS_THP_PMD_SIZE;
}

/** The DMI fields that are root's alone, mode 0400, as synthfs.c has them. */
int sysfs_attr_private(int node, int attr)
{
    return node == SYSFS_DMI_ID && (attr == DMI_PRODUCT_SERIAL || attr == DMI_PRODUCT_UUID ||
                                    attr == DMI_BOARD_SERIAL || attr == DMI_CHASSIS_SERIAL);
}

/** A fixed attribute's mode, as synthfs.c gives it. */
uint32_t sysfs_attr_mode(int node, int attr)
{
    return sysfs_file_writable(node, 0, attr) ? 0644u
           : sysfs_attr_private(node, attr)   ? 0400u
                                              : 0444u;
}

/* The length of the text the last backlight or radio switch store was
 * given, which the rows read. */
static int stored_len;

/** A backlight's store: the length kept, and the text read as the real
 * one reads it, by `k_strtoul`. */
int backlight_store(int i, int attr, const char *text, int len)
{
    unsigned long n;

    (void)i;
    (void)attr;
    stored_len = len;
    return k_strtoul(text, len < 0 ? 0 : (size_t)len, &n);
}

/** A radio switch's store, kept and read as a backlight's. */
int rfkill_store(int index, int attr, const char *text, int len)
{
    return backlight_store(index, attr, text, len);
}

/** No row writes /sys/power/state. */
int suspend_to_idle(void)
{
    return -EIO;
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

/** The fixed attributes used by these rows, as synthfs.c numbers them. */
const char *sysfs_attr(int node, int n)
{
    int count = node == SYSFS_THP               ? 6
                : node == SYSFS_THP_KHUGEPAGED  ? 9
                : node == SYSFS_CPU_CACHE_INDEX ? 10
                : node == SYSFS_NODE            ? 5
                : node == SYSFS_NODE0           ? 3
                : node == SYSFS_CLOCKSOURCE0    ? 2
                : node == SYSFS_DMI_ID          ? 23
                                                : 0;

    if (n >= 0 && n < count) {
        return "attribute";
    }
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

/* The dispositions of the model's threads, none set. */
static struct k_sigaction actions[KSIG_MAX + 1];
static process_t process = {.sigacts = actions}, thread = {.sigacts = actions},
                 stranger = {.sigacts = actions};
static struct tgroup group, other_group;
/* The directory the reader runs in, which is not the reaped process's. */
static page_dir_t reader_dir;
struct percpu cpu_table[NR_CPUS] = {{.current = &process, .space = &reader_dir, .online = 1}};
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
/* What a walk from outside an address space does before and after its lock
 * (compat/mmap_lock_host.c). */
extern void (*host_read_begin)(struct mm *mm);
extern void (*host_read_end)(struct mm *mm);
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

/** The model's threads have no signal pending or blocked. */
void signal_sets_of(const process_t *t, uint64_t *pending, uint64_t *shared, uint64_t *blocked)
{
    (void)t;
    *pending = *shared = *blocked = 0;
}

/** Whether `p` is a kernel thread, as process.c judges it. */
int proc_is_kthread(const process_t *p)
{
    return p->state != PROC_UNUSED && p->state != PROC_ZOMBIE && p->space_root == 0 &&
           (p->kthread_entry != NULL || !p->in_syscall);
}

/* The user frame of every thread in the syscall and wchan rows. */
static isr_params_t user_frame;

/** The frame of thread `t`'s entry to the kernel: `user_frame` for all. */
isr_params_t *proc_user_frame(const process_t *t)
{
    (void)t;
    return &user_frame;
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

/** One thread's time. */
void thread_cpu_time(const process_t *t, uint64_t *user, uint64_t *sys)
{
    *user = t->utime;
    *sys = t->stime;
}

/** The whole process's page faults, its two threads' together. */
void proc_faults(const process_t *p, unsigned long *minor, unsigned long *major)
{
    (void)p;
    *minor = process.min_flt + thread.min_flt;
    *major = process.maj_flt + thread.maj_flt;
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

/* Bytes a row makes unreadable to copy_from_user, [fault_from, fault_end),
 * or none while fault_from is NULL. */
static const char *fault_from, *fault_end;

/** Copy from the caller's buffer, which is host memory. Returns 0, or
 * -EFAULT for NULL or a range that reaches the unreadable bytes. */
int copy_from_user(void *dst, const void __user *src, size_t n)
{
    const char *from = (const char __force *)src;

    if (!src || (fault_from && n > 0 && from < fault_end && from + n > fault_from)) {
        return -EFAULT;
    }
    k_memcpy(dst, (const void __force *)src, n);
    return 0;
}

/** The descriptor table's size, as fd.c reports it. */
int fd_table_cap(const fd_table_t *t)
{
    return t ? t->cap : 0;
}

/** Whether descriptor `fd` of `t` is open and not closing, as fd.c has it. */
int fd_is_valid(fd_table_t *t, int fd)
{
    if (!t || fd < 0 || fd >= t->cap) {
        return 0;
    }
    return t->chunks[0][fd].type != FD_TYPE_NONE && !(t->chunks[0][fd].flags & FD_FLAG_CLOSING);
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

/** No process of these rows has a controlling terminal. */
int tty_controlling(const process_t *proc, uint32_t *dev, int *fg_pgid)
{
    (void)proc;
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
static char named_path[] = "/srv/named";
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
/* What another thread does at the next hold of a description, run once,
 * as the target may end while a reading of maps sleeps for the program's
 * path, or NULL. */
static void (*ofd_ref_hook)(void);

void fd_ofd_ref(struct open_file *file)
{
    void (*hook)(void) = ofd_ref_hook;

    (void)file;
    file_holds++;
    if (hook) {
        ofd_ref_hook = NULL;
        hook();
    }
}
void fd_ofd_unref(struct open_file *file)
{
    (void)file;
    file_holds--;
}
/* A peek with a hold, as syscall_fs.c's: the copy and a hold on its
 * description, which the matching drop gives back. */
int fd_peek_hold(struct fd_table *table, int fd, struct fd_entry *out)
{
    if (fd_peek(table, fd, out, FD_PEEK_REF) != 0) {
        return -EBADF;
    }
    if (out->ofd) {
        fd_ofd_ref(out->ofd);
    }
    return 0;
}
void fd_peek_drop(struct fd_entry *copy)
{
    if (copy->ofd) {
        fd_ofd_unref(copy->ofd);
    }
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

/** The header of `node`, a node in this file's memory: a copy, 0. */
int krffs_node_head(const krffs_node_t *node, krffs_node_t *out)
{
    *out = *node;
    return 0;
}

/** The node at `path`: the program at `program_path`, and NULL for any
 *  other path. */
krffs_node_t *krffs_resolve(const char *path)
{
    return program_path && __builtin_strcmp(path, program_path) == 0 ? &program : NULL;
}

/** `krffs_resolve`, with `*err` 0 when found and -ENOENT when not. */
krffs_node_t *krffs_resolve_err(const char *path, int *err)
{
    krffs_node_t *node = krffs_resolve(path);

    if (err) {
        *err = node ? 0 : -ENOENT;
    }
    return node;
}

const struct vfs_type vfs_tmpfs_type = {0};

/** Whether a node is memfd_secret's, which no row's is: 0. */
int tmpfs_secret(void *node)
{
    (void)node;
    return 0;
}

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

static uint8_t pci_bytes[4096];
static int pci_extended, pci_writes;
static uint16_t pci_write_at[16];
static uint8_t pci_write_width[16];

/** Record one access of its exact width in the modeled config space. */
static void pci_store(uint8_t bus, uint8_t device, uint8_t function, uint16_t at, uint32_t value,
                      unsigned width)
{
    unsigned i;

    check(bus == 0 && device == 4 && function == 0, "config write selects its function");
    check(pci_writes < 16, "config write trace fits");
    if (pci_writes < 16) {
        pci_write_at[pci_writes] = at;
        pci_write_width[pci_writes++] = (uint8_t)width;
    }
    for (i = 0; i < width; i++) {
        pci_bytes[at + i] = (uint8_t)(value >> (8 * i));
    }
}

/** Model a dword write of extended configuration space. */
void pci_write_config32_ext(uint8_t b, uint8_t d, uint8_t f, uint16_t at, uint32_t value)
{
    pci_store(b, d, f, at, value, 4);
}

/** Model a word write, without touching either adjacent byte. */
void pci_write_config16_ext(uint8_t b, uint8_t d, uint8_t f, uint16_t at, uint16_t value)
{
    pci_store(b, d, f, at, value, 2);
}

/** Model a byte write. */
void pci_write_config8_ext(uint8_t b, uint8_t d, uint8_t f, uint16_t at, uint8_t value)
{
    pci_store(b, d, f, at, value, 1);
}

/** One PCI function answers on this machine, a virtio display at 00:04.0,
 * 1af4:1050 with subsystem 1af4:1100; every other read is all ones. */
uint32_t pci_read_config32(uint8_t bus, uint8_t device, uint8_t function, uint8_t offset)
{
    if (bus != 0 || device != 4 || function != 0) {
        return 0xFFFFFFFFu;
    }
    if (offset >= 0x40) {
        return (uint32_t)pci_bytes[offset] | (uint32_t)pci_bytes[offset + 1] << 8 |
               (uint32_t)pci_bytes[offset + 2] << 16 | (uint32_t)pci_bytes[offset + 3] << 24;
    }
    return offset == 0x00   ? 0x10501af4u
           : offset == 0x04 ? 0x00100007u
           : offset == 0x08 ? 0x03000001u
           : offset == 0x2C ? 0x11001af4u
                            : 0u;
}

/** Extended space is present only while the config-file row enables it. */
uint32_t pci_read_config32_ext(uint8_t bus, uint8_t device, uint8_t function, uint16_t offset)
{
    if (offset < 256) {
        return pci_read_config32(bus, device, function, (uint8_t)offset);
    }
    if (pci_extended && bus == 0 && device == 4 && function == 0) {
        return (uint32_t)pci_bytes[offset] | (uint32_t)pci_bytes[offset + 1] << 8 |
               (uint32_t)pci_bytes[offset + 2] << 16 | (uint32_t)pci_bytes[offset + 3] << 24;
    }
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

/* The owner pci_owner gives every function: NULL, or a driver that shows
 * itself bound, as vfio-pci does, which a row sets. */
static const char *pci_owner_name;

const char *pci_owner(uint8_t bus, uint8_t devfn)
{
    (void)bus;
    (void)devfn;
    return pci_owner_name;
}

/* While set, the virtio display also has an I/O BAR 2 of 32 ports. */
static int pci_io_bar;
static int pci_no_prefetch, pci_no_wc;

/** The model's PAT provides write-combining unless a row disables it. */
pte_t arch_pte_wc(void)
{
    return pci_no_wc ? PTE_NOCACHE : PTE_WC;
}

/** The resources of the virtio display: a prefetchable BAR 0 of 8 MiB and
 * a 64-bit BAR 4 of 16 KiB, as pci.c gives them from the walk's sizes, and
 * BAR 2 while `pci_io_bar` is set. */
void pci_resource(uint8_t bus, uint8_t device, uint8_t function, int i, struct pci_resource *r)
{
    r->start = 0;
    r->end = 0;
    r->flags = 0;
    if (bus != 0 || device != 4 || function != 0) {
        return;
    }
    if (i == 0) {
        r->start = 0xFE000000u;
        r->end = 0xFE7FFFFFu;
        r->flags = 0x42208u;
    } else if (i == 4) {
        r->start = 0xFE800000u;
        r->end = 0xFE803FFFu;
        r->flags = 0x14220Cu;
    } else if (i == 2 && pci_io_bar) {
        r->start = 0xC000u;
        r->end = 0xC01Fu;
        r->flags = 0x40101u;
    }
    if (pci_no_prefetch) {
        r->flags &= ~PCI_IORESOURCE_PREFETCH;
    }
}

/* The last port access of the resourceN rows: its port, width and value,
 * and the value a read returns, the port's number with its width above. */
static uint32_t port_at, port_width, port_value;

uint8_t pci_io_read8(uint16_t port)
{
    port_at = port;
    port_width = 1;
    return (uint8_t)port;
}

uint16_t pci_io_read16(uint16_t port)
{
    port_at = port;
    port_width = 2;
    return (uint16_t)(0x2000u | port);
}

uint32_t pci_io_read32(uint16_t port)
{
    port_at = port;
    port_width = 4;
    return 0x40000000u | port;
}

void pci_io_write8(uint16_t port, uint8_t value)
{
    port_at = port;
    port_width = 1;
    port_value = value;
}

void pci_io_write16(uint16_t port, uint16_t value)
{
    port_at = port;
    port_width = 2;
    port_value = value;
}

void pci_io_write32(uint16_t port, uint32_t value)
{
    port_at = port;
    port_width = 4;
    port_value = value;
}

/* What the last `vm_device_map` was given, and its result. */
static const struct vm_device_ops *mapped_ops;
static void *mapped_obj;
static uint32_t mapped_cache;

int vm_device_map(vma_t *vma, const struct vm_device_ops *ops, void *obj, uint32_t cache)
{
    if (vma->type != VM_SHARED) {
        return -EINVAL;
    }
    mapped_ops = ops;
    mapped_obj = obj;
    mapped_cache = cache;
    return 0;
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

/** The mouse's interface 0 as `usb_core_row` gives it, and no other. */
int usb_core_interface(int index, int ifnum, struct usb_sys_row *row)
{
    return ifnum == 0 || ifnum == -1 ? usb_core_row(index, row) : -1;
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

/** Whether the stat line `line` has the page faults `minor`, `cminor`,
 *  `major` and `cmajor`, fields 10 to 13. */
static int stat_faults(const char *line, unsigned minor, unsigned cminor, unsigned major,
                       unsigned cmajor)
{
    const char *close = __builtin_strrchr(line, ')');
    unsigned f[4];

    return close &&
           sscanf(close + 1, " %*s %*s %*s %*s %*s %*s %*s %u %u %u %u", &f[0], &f[1], &f[2],
                  &f[3]) == 4 &&
           f[0] == minor && f[1] == cminor && f[2] == major && f[3] == cmajor;
}

/** Field `n`, from the third on, of the stat line `line` as a number in
 * `*v`: 1, or 0 when the line has fewer fields. The fields counted from the
 * third follow the last ')'. */
static int stat_field(const char *line, int n, long long *v)
{
    const char *at = __builtin_strrchr(line, ')');
    int i;

    if (!at) {
        return 0;
    }
    at++;
    for (i = 3; i < n; i++) {
        while (*at == ' ') {
            at++;
        }
        while (*at && *at != ' ' && *at != '\n') {
            at++;
        }
        if (*at != ' ') {
            return 0;
        }
    }
    return sscanf(at, " %lld", v) == 1;
}

/* The processors' idle ticks summed, which the uptime row sets. */
static uint64_t idle_sum;

/** The processors' idle ticks, `idle_sum`. */
uint64_t sched_idle_ticks(void)
{
    return idle_sum;
}

/** The switches of the model's processors, summed from `cpu_table`. */
uint64_t sched_switches(void)
{
    uint64_t sum = 0;
    int c;

    for (c = 0; c < NR_CPUS; c++) {
        sum += cpu_table[c].switches;
    }
    return sum;
}

/** Processor `c`'s user, busy, idle and steal ticks, from the model's
 *  `cpu_table`. */
void sched_cpu_ticks(int c, uint64_t *user, uint64_t *busy, uint64_t *idle, uint64_t *steal)
{
    *user = cpu_table[c].user_ticks;
    *busy = cpu_table[c].busy_ticks;
    *idle = cpu_table[c].idle_ticks;
    *steal = cpu_table[c].steal_ticks;
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

/** A process's stat has the time and page faults of all its threads, and a
 * thread's under the process's task directory has its own, the times in
 * USER_HZ's clock_t; both have the process's reaped children's faults. */
static void thread_times(void)
{
    struct procfs_text text;
    long long field;

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
    process.min_flt = 7;
    process.maj_flt = 1;
    thread.min_flt = 70;
    thread.maj_flt = 2;
    group.cmin_flt = 500;
    group.cmaj_flt = 4;
    procfs_text_init(&text);
    check(procfs_gen_stat(&text, 7, 1) > 0 && stat_faults(text.buf, 77, 500, 3, 4),
          "a process's stat has every thread's faults and its children's");
    kfree(text.buf);
    procfs_text_init(&text);
    check(procfs_gen_stat(&text, 9, 0) > 0 && stat_faults(text.buf, 70, 500, 2, 4),
          "a thread's stat has its own faults and the process's children's");
    kfree(text.buf);
    thread.cpu = 3;
    procfs_text_init(&text);
    check(procfs_gen_stat(&text, 9, 0) > 0 && stat_field(text.buf, 39, &field) && field == 3 &&
              stat_field(text.buf, 38, &field) && field == -1 && stat_field(text.buf, 52, &field) &&
              !stat_field(text.buf, 53, &field),
          "a thread's stat has 52 fields, its processor the 39th and -1 its exit signal");
    kfree(text.buf);
    thread.cpu = 0;
    process.utime = process.stime = thread.utime = thread.stime = 0;
    process.min_flt = process.maj_flt = thread.min_flt = thread.maj_flt = 0;
    group.cmin_flt = group.cmaj_flt = 0;
}

/** Whether the syscall file of `pid`, or its wchan file when `wchan`,
 * reads `want`. */
static int pid_file_reads(int pid, int wchan, const char *want)
{
    struct procfs_text text;
    int len, ok;

    procfs_text_init(&text);
    len = wchan ? procfs_gen_wchan(&text, pid) : procfs_gen_syscall(&text, pid);
    ok = len == (int)__builtin_strlen(want) && text.buf &&
         __builtin_memcmp(text.buf, want, (unsigned long)len) == 0;
    if (!ok) {
        printf("procfs-hosttest: /proc/%d/%s read '%.*s'\n", pid, wchan ? "wchan" : "syscall",
               len > 0 ? len : 0, text.buf ? text.buf : "");
    }
    kfree(text.buf);
    return ok;
}

/* The text Linux's proc_sched_show_task prints, built here with its own
 * formats (kernel/sched/debug.c:1133-1138), so that each line of procfs's
 * is checked against the printf it stands for. */
static char sched_want[4096];
static int sched_want_len;

/** Append Linux's `__PS` line of `name` and `v`. */
static void want_ps(const char *name, long long v)
{
    sched_want_len += __builtin_snprintf(sched_want + sched_want_len,
                                         sizeof(sched_want) - (unsigned long)sched_want_len,
                                         "%-45s:%21lld\n", name, v);
}

/** Append Linux's `__PSN` line of `name` and `ns`, split as `SPLIT_NS`. */
static void want_psn(const char *name, long long ns)
{
    long long high = ns < 0 ? -(-ns / 1000000) : ns / 1000000;
    long low = (long)((ns < 0 ? -ns : ns) % 1000000);

    sched_want_len += __builtin_snprintf(sched_want + sched_want_len,
                                         sizeof(sched_want) - (unsigned long)sched_want_len,
                                         "%-45s:%14lld.%06ld\n", name, high, low);
}

/** Whether /proc/`pid`/sched reads `sched_want`. */
static int sched_reads(int pid)
{
    struct procfs_text text;
    int len, ok;

    procfs_text_init(&text);
    len = procfs_gen_sched(&text, pid);
    ok = len == sched_want_len && text.buf &&
         __builtin_memcmp(text.buf, sched_want, (unsigned long)len) == 0;
    if (!ok) {
        printf("procfs-hosttest: /proc/%d/sched read\n%.*s\nwanted\n%s", pid, len > 0 ? len : 0,
               text.buf ? text.buf : "", sched_want);
    }
    kfree(text.buf);
    return ok;
}

/** The lines of /proc/7/sched after its schedstats, for the entity the
 * rows of `sched_rows` give process 7. */
static void want_sched_tail(void)
{
    want_ps("nr_switches", 7);
    want_ps("nr_voluntary_switches", 5);
    want_ps("nr_involuntary_switches", 2);
    want_ps("se.load.weight", 1024);
    want_ps("se.avg.load_sum", 47000);
    want_ps("se.avg.runnable_sum", 46000);
    want_ps("se.avg.util_sum", 45000);
    want_ps("se.avg.load_avg", 1023);
    want_ps("se.avg.runnable_avg", 1022);
    want_ps("se.avg.util_avg", 1021);
    want_ps("se.avg.last_update_time", 1234560000000LL);
    want_ps("se.avg.util_est", 5);
    want_ps("policy", 0);
    want_ps("prio", 118);
    want_ps("se.slice", 3000000);
    want_ps("clock-delta", 25);
}

/** /proc/PID/sched: Linux's proc_sched_show_task for an ordinary thread,
 * its name, id and thread count, its entity's times as milliseconds with
 * six places, a time above -1 ms without its sign as Linux's SPLIT_NS
 * prints it, its switches, weight and averages, util_est without
 * UTIL_AVG_UNCHANGED, its policy, Linux's prio of nice -2 and its slice; the
 * schedstats only while kernel.sched_schedstats is 1, with avg_atom and
 * avg_per_cpu; a write clears the schedstats; and the run queues' lock
 * given back after each reading. */
static void sched_rows(void)
{
    struct procfs_text text;

    process.se.exec_start = 1234567890123ULL;
    process.se.vruntime = 5000;
    process.se.sum_exec_runtime = 2500000;
    process.se.nr_migrations = 3;
    process.se.load.weight = 1024;
    process.se.avg.load_sum = 47000;
    process.se.avg.runnable_sum = 46000;
    process.se.avg.util_sum = 45000;
    process.se.avg.load_avg = 1023;
    process.se.avg.runnable_avg = 1022;
    process.se.avg.util_avg = 1021;
    process.se.avg.last_update_time = 1234560000000ULL;
    process.se.avg.util_est = 0x80000005u;
    process.se.slice = 3000000;
    process.se.stats.sum_sleep_runtime = -1500000;
    process.se.stats.sum_block_runtime = -500;
    process.se.stats.wait_sum = 7000000;
    process.se.stats.wait_count = 4;
    process.nvcsw = 5;
    process.nivcsw = 2;
    process.nice = -2;
    sched_want_len = __builtin_snprintf(
        sched_want, sizeof(sched_want), "test (7, #threads: 1)\n%s%s\n",
        "---------------------------------------------------------", "----------");
    want_psn("se.exec_start", 1234567890123LL);
    want_psn("se.vruntime", 5000);
    want_psn("se.sum_exec_runtime", 2500000);
    want_ps("se.nr_migrations", 3);
    want_sched_tail();
    check(sched_reads(7), "a thread's sched is Linux's text without the schedstats");
    fair_schedstats = 1;
    sched_want_len = __builtin_snprintf(
        sched_want, sizeof(sched_want), "test (7, #threads: 1)\n%s%s\n",
        "---------------------------------------------------------", "----------");
    want_psn("se.exec_start", 1234567890123LL);
    want_psn("se.vruntime", 5000);
    want_psn("se.sum_exec_runtime", 2500000);
    want_ps("se.nr_migrations", 3);
    want_psn("sum_sleep_runtime", -1500000);
    want_psn("sum_block_runtime", -500);
    want_psn("wait_start", 0);
    want_psn("sleep_start", 0);
    want_psn("block_start", 0);
    want_psn("sleep_max", 0);
    want_psn("block_max", 0);
    want_psn("exec_max", 0);
    want_psn("slice_max", 0);
    want_psn("wait_max", 0);
    want_psn("wait_sum", 7000000);
    want_ps("wait_count", 4);
    want_psn("iowait_sum", 0);
    want_ps("iowait_count", 0);
    want_ps("nr_migrations_cold", 0);
    want_ps("nr_failed_migrations_affine", 0);
    want_ps("nr_failed_migrations_running", 0);
    want_ps("nr_failed_migrations_hot", 0);
    want_ps("nr_forced_migrations", 0);
    want_ps("nr_wakeups", 0);
    want_ps("nr_wakeups_sync", 0);
    want_ps("nr_wakeups_migrate", 0);
    want_ps("nr_wakeups_local", 0);
    want_ps("nr_wakeups_remote", 0);
    want_ps("nr_wakeups_affine", 0);
    want_ps("nr_wakeups_affine_attempts", 0);
    want_ps("nr_wakeups_passive", 0);
    want_ps("nr_wakeups_idle", 0);
    want_psn("avg_atom", 2500000 / 7);
    want_psn("avg_per_cpu", 2500000 / 3);
    want_sched_tail();
    check(sched_reads(7), "and with kernel.sched_schedstats 1 its schedstats, avg_atom and "
                          "avg_per_cpu");
    check(procfs_sched_clear(7) == 0 && process.se.stats.wait_sum == 0 &&
              process.se.stats.sum_sleep_runtime == 0 && process.se.sum_exec_runtime == 2500000,
          "a write of sched clears the schedstats alone");
    check(procfs_sched_clear(5) == -ESRCH, "and of a process that is not there is ESRCH");
    procfs_text_init(&text);
    check(procfs_gen_sched(&text, 5) == -ENOENT, "which has no sched to read");
    kfree(text.buf);
    check(sched_lock_depth == 0, "and each reading gives the run queues' lock back");
    fair_schedstats = 0;
    process.nice = 0;
    process.nvcsw = process.nivcsw = 0;
    k_memset(&process.se, 0, sizeof(process.se));
}

/** Whether /proc/`pid`/schedstat reads `want`. */
static int schedstat_reads(int pid, const char *want)
{
    struct procfs_text text;
    int len, ok;

    procfs_text_init(&text);
    len = procfs_gen_schedstat(&text, pid);
    ok = len == (int)__builtin_strlen(want) && text.buf &&
         __builtin_memcmp(text.buf, want, (unsigned long)len) == 0;
    if (!ok) {
        printf("procfs-hosttest: /proc/%d/schedstat read '%.*s'\n", pid, len > 0 ? len : 0,
               text.buf ? text.buf : "");
    }
    kfree(text.buf);
    return ok;
}

/** /proc/PID/schedstat: a thread's run time in ns, its entity's
 * `sum_exec_runtime`, its wait on the run queues in ns and its switches onto
 * a processor, as Linux's proc_pid_schedstat, which the selftest steal_time
 * reads; the leader's file is the leader's, not the group's. Then
 * /proc/PID/sched's rows (`sched_rows`). */
static void schedstat_rows(void)
{
    struct procfs_text text;

    thread.se.sum_exec_runtime = 5000000;
    thread.run_delay = 123456789;
    thread.pcount = 42;
    process.run_delay = 1000;
    process.pcount = 1;
    check(schedstat_reads(9, "5000000 123456789 42\n"),
          "a thread's schedstat is its run time, its run_delay and its pcount");
    check(schedstat_reads(7, "0 1000 1\n"), "and the leader's is its own");
    procfs_text_init(&text);
    check(procfs_gen_schedstat(&text, 5) == -ENOENT, "a process that is not there has none");
    kfree(text.buf);
    check(sched_lock_depth == 0, "and each reading gives the run queues' lock back");
    thread.se.sum_exec_runtime = 0;
    thread.run_delay = process.run_delay = 0;
    thread.pcount = process.pcount = 0;
    sched_rows();
}

/**
 * /proc/PID/syscall and wchan: a thread blocked in a call shows the call's
 * number as a tracer left it, its six arguments, its stack and instruction
 * pointers, and the name its wait recorded; one blocked in a fault, or past
 * a handler's return, -1 and the two pointers; a ready thread "running" and
 * 0; a stopped one its stop's function; a kernel thread call 0 with every
 * field 0x0, an exiting thread past its address space its call, and an
 * ended one -1 with no pointers; and a caller that may not read the process
 * EPERM and 0.
 */
static void syscall_wchan_rows(void)
{
    process.pid = 7;
    thread.pid = 9;
    thread.tg = &group;
    thread.state = PROC_BLOCKED;
    thread.space_root = 0x1000;
    thread.orig_nr = 3;
    thread.wchan = "pipe_read";
    user_frame.vector = 0x80;
#ifdef RACH2_HOST_X86_64
    user_frame.error_code = SC_ENTRY_IA32;
    user_frame.rbx = 4;
    user_frame.rcx = 0x804a000u;
    user_frame.rdx = 0x100;
    user_frame.rbp = 0xbffff000u;
#else
    user_frame.ebx = 4;
    user_frame.ecx = 0x804a000u;
    user_frame.edx = 0x100;
    user_frame.ebp = 0xbffff000u;
#endif
    FRAME_SP(&user_frame) = 0xbfffefd0u;
    FRAME_PC(&user_frame) = 0x8048123u;
    check(pid_file_reads(9, 0, "3 0x4 0x804a000 0x100 0x0 0x0 0xbffff000 0xbfffefd0 0x8048123\n"),
          "a thread blocked in read shows the call, its arguments and its pointers");
    check(pid_file_reads(9, 1, "pipe_read"), "its wchan names its wait, with no newline");
    user_frame.vector = 14;
    check(pid_file_reads(9, 0, "-1 0xbfffefd0 0x8048123\n"),
          "a thread blocked in a page fault shows -1 and its pointers");
    user_frame.vector = 0x80;
    thread.orig_nr = -1;
    check(pid_file_reads(9, 0, "-1 0xbfffefd0 0x8048123\n"),
          "a thread past a handler's return shows -1 and its pointers");
    thread.orig_nr = -2;
    thread.state = PROC_STOPPED;
    check(pid_file_reads(9, 0, "-2 0xbfffefd0 0x8048123\n"),
          "a thread stopped at a call a tracer made -2 shows -2 and its pointers alone");
    thread.orig_nr = 240;
    check(pid_file_reads(9, 0, "240 0x4 0x804a000 0x100 0x0 0x0 0xbffff000 0xbfffefd0 0x8048123\n"),
          "a stopped thread shows the call it stopped in");
    check(pid_file_reads(9, 1, "do_signal_stop"), "a job-control stop is do_signal_stop");
    thread.ptrace_status = 0x137f;
    check(pid_file_reads(9, 1, "ptrace_stop"), "a ptrace stop is ptrace_stop");
    thread.ptrace_status = 0;
    thread.state = PROC_READY;
    check(pid_file_reads(9, 0, "running\n"), "a ready thread is running");
    check(pid_file_reads(9, 1, "0"), "a ready thread has no wchan");
    thread.state = PROC_BLOCKED;
    thread.space_root = 0;
    check(pid_file_reads(9, 0, "0 0x0 0x0 0x0 0x0 0x0 0x0 0x0 0x0\n"),
          "a kernel thread shows call 0 and zero fields, as Linux's zeroed frame gives");
    thread.in_syscall = 1;
    check(pid_file_reads(9, 0, "240 0x4 0x804a000 0x100 0x0 0x0 0xbffff000 0xbfffefd0 0x8048123\n"),
          "an exiting thread past its address space shows its call");
    thread.in_syscall = 0;
    thread.state = PROC_ZOMBIE;
    check(pid_file_reads(9, 0, "-1 0x0 0x0\n"), "an ended thread has no user frame");
    thread.state = PROC_BLOCKED;
    thread.space_root = 0x1000;
    stranger.pid = 11;
    stranger.tg = &other_group;
    stranger.state = PROC_BLOCKED;
    stranger.space_root = 0x1000;
    stranger.wchan = "futex_wait_queue";
    stranger.ruid = 2000;
    stranger.euid = 2000;
    process.ruid = 1000;
    process.euid = 1000;
    {
        struct procfs_text text;

        procfs_text_init(&text);
        check(procfs_gen_syscall(&text, 11) == -EPERM,
              "another user's process refuses its syscall file's text with EPERM");
        kfree(text.buf);
    }
    check(pid_file_reads(11, 1, "0"), "another user's process reads 0 from wchan");
    process.ruid = process.euid = 0;
    check(pid_file_reads(11, 1, "futex_wait_queue"), "a privileged caller reads its wchan");
    stranger.ruid = stranger.euid = 0;
    stranger.state = PROC_READY;
    stranger.space_root = 0;
    thread.state = PROC_READY;
    thread.space_root = 0;
    thread.orig_nr = 0;
    k_memset(&user_frame, 0, sizeof(user_frame));
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

/** A process's 0555 directories keep its effective IDs in every dumpable
 * state and after its address space is gone. Other nodes belong to root
 * while the space is absent or not dumpable. */
static void task_owners(void)
{
    struct mm *saved_mm = process.mm;
    uint32_t saved_uid = process.euid, saved_gid = process.egid;
    uint32_t saved_dumpable = space.dumpable, uid, gid;
    int dumpable;

    process.euid = 1000;
    process.egid = 1001;
    process.mm = &space;
    for (dumpable = 0; dumpable <= 2; dumpable++) {
        space.dumpable = (uint32_t)dumpable;
        check(procfs_pid_owner(7, 0040555u, &uid, &gid) == 0 && uid == 1000 && gid == 1001,
              "a public process directory keeps its effective IDs");
        check(procfs_pid_owner(7, 0040500u, &uid, &gid) == 0 &&
                  uid == (dumpable == 1 ? 1000u : 0u) && gid == (dumpable == 1 ? 1001u : 0u),
              "a private process directory has the dumpable owner");
        check(procfs_oom_owner(7, &uid, &gid) == 0 && uid == (dumpable == 1 ? 1000u : 0u) &&
                  gid == (dumpable == 1 ? 1001u : 0u),
              "a process file has the dumpable owner");
    }
    process.mm = NULL;
    check(procfs_pid_owner(7, 0040555u, &uid, &gid) == 0 && uid == 1000 && gid == 1001,
          "a public directory keeps its owner after the address space is gone");
    check(procfs_oom_owner(7, &uid, &gid) == 0 && uid == 0 && gid == 0,
          "a file belongs to root after the address space is gone");
    check(procfs_pid_owner(99, 0040555u, &uid, &gid) == -ENOENT, "a missing process has no owner");
    process.mm = saved_mm;
    process.euid = saved_uid;
    process.egid = saved_gid;
    space.dumpable = saved_dumpable;
}

/** An open descriptor has its link, and one a close has marked, whose
 * release waits for another thread's call or is under way, has none. */
static void descriptor_links(void)
{
    char buf[32];

    process.fds = &table;
    table.chunks[0] = entries;
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
    table.chunks[0] = entries;
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
    check(maps_path(&space, &r, NULL) == exec_path, "a segment below the heap is the program's");
    r.start = 0xaffc2000u;
    r.end = 0xaffc3000u;
    r.type = VM_DATA;
    check(maps_path(&space, &r, NULL) == interp_path, "one above it is the dynamic linker's");
}

/* Stand-ins for the vDSO's two files, for the VmFlags rows. */
static struct vm_file vdso_text_file, vdso_vvar_file;

/** The maps name of a stand-in of the vDSO's files, and NULL for any other
 * file. */
const char *vdso_region_name(const struct vm_file *f)
{
    return f == &vdso_text_file ? "[vdso]" : f == &vdso_vvar_file ? "[vvar]" : NULL;
}

/** Whether `f` is the stand-in of the vDSO's data page. */
int vdso_is_data(const struct vm_file *f)
{
    return f == &vdso_vvar_file;
}

/** Compare the VmFlags of a region of `type` and `prot` that maps `file`
 * with `want`, including Linux's bit order. */
static void smaps_flags_row(vm_type_t type, uint32_t prot, struct vm_file *file, const char *want)
{
    struct procfs_text t;
    vma_t r;

    k_memset(&r, 0, sizeof(r));
    r.type = type;
    r.prot = prot;
    r.file = file;
    procfs_text_init(&t);
    ap_smaps_flags(&t, &r);
    check(!t.nomem && t.buf && t.len == (int)__builtin_strlen(want) &&
              __builtin_memcmp(t.buf, want, (size_t)t.len) == 0,
          want);
    kfree(t.buf);
}

/** Possible access respects the region's restrictions; a shared mapping
 * that may not be made writable is ms but not sh, whether it is VM_SHARED
 * or served as a private one; the stack and the heap are charged, and a
 * MAP_NORESERVE mapping is not; device mappings cannot grow or enter a core;
 * the vDSO's pages have the flags of Linux's special mappings; and each
 * flag appears in Linux's order. */
static void smaps_flags_rows(void)
{
    smaps_flags_row(VM_ANON, 0, NULL, "VmFlags: mr mw me\n");
    smaps_flags_row(VM_STACK, VM_PROT_READ | VM_PROT_WRITE | VM_PROT_ACCOUNT, NULL,
                    "VmFlags: rd wr mr mw me gd ac\n");
    smaps_flags_row(VM_HEAP, VM_PROT_READ | VM_PROT_WRITE | VM_PROT_ACCOUNT | VM_PROT_LOCKONFAULT,
                    NULL, "VmFlags: rd wr mr mw me lf ac\n");
    smaps_flags_row(VM_ANON, VM_PROT_READ | VM_PROT_WRITE | VM_PROT_NORESERVE | VM_PROT_WIPEONFORK,
                    NULL, "VmFlags: rd wr mr mw me nr wf\n");
    smaps_flags_row(VM_SHARED, VM_PROT_READ, NULL, "VmFlags: rd sh mr mw me ms\n");
    smaps_flags_row(VM_SHARED, VM_PROT_READ | VM_PROT_NOWRITE, NULL, "VmFlags: rd mr me ms\n");
    smaps_flags_row(VM_FILE, VM_PROT_READ | VM_PROT_NOWRITE, NULL, "VmFlags: rd mr me ms\n");
    smaps_flags_row(VM_FILE, VM_PROT_READ, NULL, "VmFlags: rd mr mw me\n");
    smaps_flags_row(VM_SHARED, VM_PROT_READ | VM_PROT_WRITE | VM_PROT_IO | VM_PROT_DONTFORK, NULL,
                    "VmFlags: rd wr sh mr mw me ms pf io dc de dd\n");
    smaps_flags_row(VM_SHARED, VM_PROT_READ | VM_PROT_WRITE | VM_PROT_SECRET | VM_PROT_LOCKED, NULL,
                    "VmFlags: rd wr sh mr mw ms lo dd\n");
    smaps_flags_row(VM_ANON,
                    VM_PROT_READ | VM_PROT_LOCKED | VM_PROT_LOCKONFAULT | VM_PROT_WIPEONFORK |
                        VM_PROT_SEALED,
                    NULL, "VmFlags: rd mr mw me lo lf wf sl\n");
    smaps_flags_row(VM_FILE, VM_PROT_READ | VM_PROT_EXEC, &vdso_text_file,
                    "VmFlags: rd ex mr mw me de\n");
    smaps_flags_row(VM_SHARED, VM_PROT_READ | VM_PROT_NOWRITE, &vdso_vvar_file,
                    "VmFlags: rd mr pf io de dd\n");
}

/* A file of no file system for the maps rows, which `filemap_ofd` gives
 * no description, so that its identity is "00:00 0". */
static char maps_file_path[] = "/mnt/f";
static struct vm_file maps_file = {1, 0, 0, 0, maps_file_path, 0, 0};

/** No region of the maps rows maps a file of the page cache. */
const struct open_file *filemap_ofd(const struct vm_file *f)
{
    (void)f;
    return NULL;
}

/* The mount of memfds and shared anonymous memory, which the mount table
 * of these rows does not hold, so that its device reads 00:00. */
static struct vfs_mount memfd_mount;

/** The mount of memfds and shared anonymous memory. */
struct vfs_mount *vfs_memfd_mount(void)
{
    return &memfd_mount;
}

/* Shared anonymous memory for the identity row, with inode 7. */
static char anon_shared_path[] = "/dev/zero (deleted)";
static struct vm_file anon_shared_file = {1, 0, 0, 0, anon_shared_path, 0, 0};

/** Inode 7 for the identity row's shared anonymous memory; 0 for any
 * other file. */
uint64_t filemap_anon_ino(const struct vm_file *f)
{
    return f == &anon_shared_file ? 7u : 0u;
}

/* The mapped files of the naming row: one an mmap made and one of the
 * program's image, both open on the same description, whose mount's type
 * keeps no path, so that its link target is the path it was opened by. */
static struct vfs_mount named_mount;
static struct open_file named_ofd;
static fd_entry_t named_entry;
static struct vm_file named_file, image_file;
static int file_refs;

/** The entry of the naming row's two files; none for any other, as for
 * shared anonymous memory. */
const fd_entry_t *filemap_entry(const struct vm_file *f)
{
    return f == &named_file || f == &image_file ? &named_entry : NULL;
}

/** Count a reference a reading of maps takes on a file to name. */
void vm_file_get(struct vm_file *f)
{
    (void)f;
    file_refs++;
}

/** Count one given back. */
void vm_file_put(struct vm_file *f)
{
    (void)f;
    file_refs--;
}

/* Whether the mount table holds the mount of memfds, in place 4, where
 * vfs.c puts it; empty otherwise. */
static int memfd_mounted;

/** The places of the mount table the identity of a maps line searches. */
int vfs_mount_slots(void)
{
    return memfd_mounted ? 5 : 0;
}

/** The mount of memfds in place 4 while `memfd_mounted`; none elsewhere. */
struct vfs_mount *vfs_mount_at(int idx)
{
    return memfd_mounted && idx == 4 ? &memfd_mount : NULL;
}

/** Compare the first 22 bytes of the maps line of a region of `type` and
 * `prot` that maps `file`, the range, the rights and the p or s, with
 * `want`. */
static void maps_line_row(vm_type_t type, uint32_t prot, struct vm_file *file, const char *want)
{
    struct procfs_text t;
    vma_t r;

    k_memset(&space, 0, sizeof(space));
    k_memset(&r, 0, sizeof(r));
    r.start = 0x40000000u;
    r.end = 0x40001000u;
    r.type = type;
    r.prot = prot;
    r.file = file;
    procfs_text_init(&t);
    ap_maps_line(&t, &space, &r, NULL);
    check(!t.nomem && t.buf && t.len > 22 && __builtin_memcmp(t.buf, want, 22) == 0, want);
    kfree(t.buf);
}

/** Whether the `len` bytes of `text` hold `want`. */
static int text_has(const char *text, int len, const char *want)
{
    int n = (int)__builtin_strlen(want), i;

    for (i = 0; i + n <= len; i++) {
        if (__builtin_memcmp(text + i, want, (size_t)n) == 0) {
            return 1;
        }
    }
    return 0;
}

/** A reading of maps names a mapped file by its link's target, from the
 * reader's root, as Linux's d_path at each reading, and keeps the name in
 * no file, so that a reader with another root builds its own; the
 * segments of the program are named by the present path of the program's
 * file, and once its name is gone by the path exec recorded with
 * " (deleted)", as the exe link reads, though their file has an entry too;
 * a shared region with no entry and no inode is named by no target; and
 * every reference taken, to a file or to the address space, goes back. */
static void maps_names_row(void)
{
    struct mm *saved_mm = process.mm;
    struct open_file saved_ofd = exe_ofd;
    struct procfs_text t;
    vma_t code, file, shared;
    static struct vm_file shared_file;

    k_memset(&space, 0, sizeof(space));
    krffs_fs = &root_fs;
    root_fs.node = &root_dir;
    root_mount.type = &vfs_krffs_type;
    k_memset(&exe_ofd, 0, sizeof(exe_ofd));
    exe_ofd.vmnt = &root_mount;
    exe_ofd.vnode = &program;
    k_strncpy((char *)program.name, "b", sizeof(program.name));
    program_path = "/tmp/b";
    space.exe_file = &exe_ofd;
    space.users = 1;
    space.holds = 1;
    k_memset(&code, 0, sizeof(code));
    k_memset(&file, 0, sizeof(file));
    k_memset(&shared, 0, sizeof(shared));
    space.exe = exec_path;
    space.vm.heap_start = 0x0804d000u;
    code.start = 0x08048000u;
    code.end = 0x08049000u;
    code.type = VM_CODE;
    code.prot = VM_PROT_READ | VM_PROT_EXEC;
    code.file = &image_file;
    code.next = &file;
    file.start = 0x40000000u;
    file.end = 0x40001000u;
    file.type = VM_FILE;
    file.prot = VM_PROT_READ;
    file.file = &named_file;
    file.next = &shared;
    shared.start = 0x40001000u;
    shared.end = 0x40002000u;
    shared.type = VM_SHARED;
    shared.prot = VM_PROT_READ | VM_PROT_WRITE;
    shared.file = &shared_file;
    space.vm.first = &code;
    named_mount.type = &entry_type;
    named_ofd.vmnt = &named_mount;
    named_ofd.dirpath = named_path;
    named_entry.ofd = &named_ofd;
    process.mm = &space;
    procfs_text_init(&t);
    check(procfs_gen_maps(&t, 7) > 0 && !t.nomem, "maps with a mapped file is read");
    check(text_has(t.buf, t.len, "40000000-40001000 r--p 00000000 00:00 0") &&
              text_has(t.buf, t.len, " /srv/named\n40001000"),
          "a mapped file is named by its link's target at the reading");
    check(text_has(t.buf, t.len, " /tmp/b\n40000000"),
          "the program's segment is named by its file's present path at the reading");
    check(text_has(t.buf, t.len, "40001000-40002000 rw-s 00000000 00:00 0 \n"),
          "shared memory with no entry has no name");
    check(named_file.path == NULL && image_file.path == NULL, "and no file keeps the name");
    check(file_refs == 0 && space.users == 1 && space.holds == 1 && file_holds == 0 &&
              mount_locks == 0,
          "and every reference taken goes back");
    kfree(t.buf);
    program.name[0] = '\0';
    program_path = NULL;
    procfs_text_init(&t);
    check(procfs_gen_maps(&t, 7) > 0 && !t.nomem &&
              text_has(t.buf, t.len, " /tmp/a (deleted)\n40000000"),
          "a program whose name is gone is named by its exec's path, deleted");
    check(space.users == 1 && space.holds == 1 && file_holds == 0 && mount_locks == 0,
          "and the second reading gives back what it took");
    kfree(t.buf);
    exe_ofd = saved_ofd;
    krffs_fs = NULL;
    process.mm = saved_mm;
    k_memset(&space, 0, sizeof(space));
}

/** Shared anonymous memory reads the device of the mount of memfds, in
 * place 4 of the mount table, 0x14 by `procfs_mount_dev`'s count, and its
 * own inode, as Linux's line shows the shmem file's of shm_mnt; with the
 * mount not in the table, its device reads 00:00 and the inode stays. */
static void maps_anon_identity_rows(void)
{
    struct procfs_text t;
    vma_t r;

    k_memset(&space, 0, sizeof(space));
    k_memset(&r, 0, sizeof(r));
    r.start = 0x40000000u;
    r.end = 0x40001000u;
    r.type = VM_SHARED;
    r.prot = VM_PROT_READ | VM_PROT_WRITE;
    r.file = &anon_shared_file;
    memfd_mounted = 1;
    procfs_text_init(&t);
    ap_maps_line(&t, &space, &r, NULL);
    check(!t.nomem && t.len > 0 && text_has(t.buf, t.len, "rw-s 00000000 00:14 7 ") &&
              text_has(t.buf, t.len, "/dev/zero (deleted)\n"),
          "shared anonymous memory reads the memfd mount's device and its inode");
    kfree(t.buf);
    memfd_mounted = 0;
    procfs_text_init(&t);
    ap_maps_line(&t, &space, &r, NULL);
    check(!t.nomem && t.len > 0 && text_has(t.buf, t.len, "rw-s 00000000 00:00 7 "),
          "with the mount not in the table its device reads 00:00");
    kfree(t.buf);
}

/** The maps line's s column is Linux's VM_MAYSHARE: s for a shared region
 * and for a read-only shared mapping served as a private one, VM_FILE with
 * VM_PROT_NOWRITE, and p for a private mapping of a file and for both of
 * the vDSO's regions, as Linux maps them private. */
static void maps_shared_rows(void)
{
    maps_line_row(VM_SHARED, VM_PROT_READ | VM_PROT_WRITE, &maps_file, "40000000-40001000 rw-s");
    maps_line_row(VM_FILE, VM_PROT_READ | VM_PROT_NOWRITE, &maps_file, "40000000-40001000 r--s");
    maps_line_row(VM_FILE, VM_PROT_READ, &maps_file, "40000000-40001000 r--p");
    maps_line_row(VM_FILE, VM_PROT_READ | VM_PROT_EXEC, &vdso_text_file, "40000000-40001000 r-xp");
    maps_line_row(VM_SHARED, VM_PROT_READ | VM_PROT_NOWRITE, &vdso_vvar_file,
                  "40000000-40001000 r--p");
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

/** The status rows' process has no effective capabilities: 0. */
uint32_t cap_eff_word(const process_t *p, uint32_t i)
{
    (void)p;
    (void)i;
    return 0;
}

/** The status rows' process has no seccomp filter: 0. */
uint32_t seccomp_filter_count(const process_t *p)
{
    (void)p;
    return 0;
}

/** The status rows' address space has no page present. */
void paging_user_count(page_dir_t *dir, uintptr_t start, uintptr_t end, unsigned long *present,
                       unsigned long *dirty, unsigned long *anon)
{
    (void)dir;
    (void)start;
    (void)end;
    *present = *dirty = *anon = 0;
}

/** The most pages the status rows' address space had resident: none. */
unsigned long mm_resident_peak(const struct mm *mm)
{
    (void)mm;
    return 0;
}

/** No file of the status rows is in memory. */
int filemap_in_memory(const struct vm_file *f)
{
    (void)f;
    return 0;
}

/** Take the walk's user's count of `mm`, as mm_read_begin does; then, as
 * while the walk waits for the lock, process 13 ends and is reaped, which
 * frees its dispositions and its record and puts its user's count. */
static void reap_at_walk(struct mm *mm)
{
    mm_get(mm);
    if (reading) {
        struct mm *own = reading->mm;

        free(reading->sigacts);
        free(reading);
        reading = NULL;
        mm_put(own);
        reaps++;
    }
}

/** Take the walk's user's count of `mm`, as mm_read_begin does. */
static void get_at_walk(struct mm *mm)
{
    mm_get(mm);
}

/** Take one more reference to the record `p`. */
void proc_ref_get(process_t *p)
{
    p->refs++;
}

/** Drop a reference to the record `p`; the last frees it and its
 * dispositions, which are from the heap. */
void proc_ref_put(process_t *p)
{
    if (--p->refs == 0) {
        free(p->sigacts);
        free(p);
    }
}

/**
 * End process 13 and reap it, as exit.c and the reap do: its policy and its
 * user's count of its address space go at the exit, which `mm_drop` gives
 * back, and the list's reference to its record at the reap, which frees the
 * record unless a reader holds one.
 */
static void end_reading(void)
{
    struct mm *own;

    if (!reading) {
        return;
    }
    own = reading->mm;
    reading->mempolicy = NULL;
    reading->mm = NULL;
    mm_put(own);
    reading->state = PROC_UNUSED;
    proc_ref_put(reading);
    reading = NULL;
    reaps++;
}

/* The walk from outside an address space, counted from 1, during whose
 * wait `end_at_walk` ends process 13; 0 for none. */
static int end_walk;

/** Take the walk's user's count of `mm` and end process 13 while the walk
 * `end_walk` counts down to waits for the lock. */
static void end_at_walk(struct mm *mm)
{
    mm_get(mm);
    if (end_walk > 0 && --end_walk == 0) {
        end_reading();
    }
}

/** Take a reference to the policy `p`. */
struct vm_policy *vm_policy_get(struct vm_policy *p)
{
    if (p) {
        p->refs++;
    }
    return p;
}

/** No region of the numa_maps row has a policy of its own. */
const struct vm_policy *mempolicy_at(const vma_t *vma, uintptr_t addr)
{
    (void)vma;
    (void)addr;
    return NULL;
}

/** No page of the numa_maps row is present. */
void paging_user_frames(page_dir_t *dir, uintptr_t start, uintptr_t end,
                        struct paging_frame_counts *counts)
{
    (void)dir;
    (void)start;
    (void)end;
    k_memset(counts, 0, sizeof(*counts));
}

/** Drop the walk's user's count of `mm`, as mm_read_end does: the last
 * user's put frees the address space. */
static void put_at_walk(struct mm *mm)
{
    mm_put(mm);
}

/**
 * Make process 13 of process 7's group, so that its reader may see its
 * bounds, with an address space and dispositions from the heap, which
 * `reap_at_walk` frees: SIGINT ignored, its code, stack and data at known
 * addresses, and its last processor 2. 0 when the heap had no room.
 */
static int make_reaped(void)
{
    struct mm *mm = calloc(1, sizeof(*mm));
    struct k_sigaction *acts = calloc(KSIG_MAX + 1, sizeof(*acts));

    reading = calloc(1, sizeof(*reading));
    if (!mm || !acts || !reading) {
        free(mm);
        free(acts);
        free(reading);
        reading = NULL;
        return 0;
    }
    mm->users = 1;
    mm->holds = 1;
    mm->start_code = 0x8048000u;
    mm->end_code = 0x8049000u;
    mm->start_data = 0x804a000u;
    mm->end_data = 0x804b000u;
    mm->start_brk = 0x804c000u;
    mm->start_stack = 0xbffff000u;
    mm->arg_start = 0xbfffe000u;
    mm->arg_end = 0xbfffe100u;
    mm->env_start = 0xbfffe100u;
    mm->env_end = 0xbfffe200u;
    acts[2].handler = KSIG_IGN;
    reading->pid = reading->tgid = 13;
    reading->state = PROC_BLOCKED;
    reading->tg = &group;
    reading->mm = mm;
    reading->sigacts = acts;
    reading->cpu = 2;
    return 1;
}

/**
 * A read of the stat of a process that ends and is reaped while the read
 * waits for its address space's lock reads every field of the record before
 * the wait and the address space's bounds under the lock, and nothing of
 * either after: the walk's user's count is the last, and its put frees the
 * address space. AddressSanitizer reports a read of the freed record or of
 * the freed address space.
 */
static void stat_reaped_during_wait(void)
{
    struct procfs_text text;
    long long code, end, stack, ignored, cpu, data, env;
    int before = reaps, len;

    check(make_reaped(), "allocate the process whose stat is read");
    if (!reading) {
        return;
    }
    host_read_begin = reap_at_walk;
    host_read_end = put_at_walk;
    procfs_text_init(&text);
    len = procfs_gen_stat(&text, 13, 1);
    host_read_begin = NULL;
    host_read_end = NULL;
    check(len > 0 && reaps == before + 1 && !reading,
          "a stat read of a process reaped while it waits for the regions' lock ends");
    check(len > 0 && stat_field(text.buf, 26, &code) && code == 0x8048000 &&
              stat_field(text.buf, 27, &end) && end == 0x8049000 &&
              stat_field(text.buf, 28, &stack) && stack == 0xbffff000 &&
              stat_field(text.buf, 45, &data) && data == 0x804a000 &&
              stat_field(text.buf, 51, &env) && env == 0xbfffe200,
          "and gives the bounds its address space had under the lock");
    check(len > 0 && stat_field(text.buf, 33, &ignored) && ignored == 2 &&
              stat_field(text.buf, 39, &cpu) && cpu == 2,
          "and the dispositions and processor its record had before the wait");
    kfree(text.buf);
}

/**
 * A read of the status of a process that ends and is reaped while the read
 * waits for its address space's lock gives the lines after the memory
 * sizes from the record as it was before the wait, and reads nothing of it
 * after.
 */
static void status_reaped_during_wait(void)
{
    struct procfs_text text;
    int before = reaps, len;

    check(make_reaped(), "allocate the process whose status is read");
    if (!reading) {
        return;
    }
    host_read_begin = reap_at_walk;
    host_read_end = put_at_walk;
    procfs_text_init(&text);
    len = procfs_gen_status(&text, 13);
    host_read_begin = NULL;
    host_read_end = NULL;
    check(len > 0 && !text.nomem && reaps == before + 1 && !reading,
          "a status read of a process reaped while it waits for the regions' lock ends");
    check(len > 0 && !text.nomem && text.buf[len - 1] == '\n' &&
              __builtin_strstr(text.buf, "VmSize:") != NULL &&
              __builtin_strstr(text.buf, "\nThreads:\t1\n") != NULL &&
              __builtin_strstr(text.buf, "\nSigIgn:\t0000000000000002\n") != NULL &&
              __builtin_strstr(text.buf, "VmSize:") < __builtin_strstr(text.buf, "Threads:"),
          "and gives the record's lines after the memory sizes, as it was before the wait");
    kfree(text.buf);
}

/**
 * A reading of the maps of a process that ends and is reaped while the
 * program's path is built, which may sleep, keeps a user's count of the
 * address space from before that path to its end: the walk after takes no
 * count from 0, and the reading's own put is the last, which frees the
 * address space once. AddressSanitizer reports a put of a freed record.
 */
static void maps_reaped_during_names(void)
{
    static struct open_file gone_ofd;
    struct procfs_text text;
    int before = reaps, len;

    check(make_reaped(), "allocate the process whose maps are read");
    if (!reading) {
        return;
    }
    reading->refs = 1;
    k_memset(&gone_ofd, 0, sizeof(gone_ofd));
    gone_ofd.vmnt = &other_mount;
    gone_ofd.vnode = &program;
    reading->mm->exe_file = &gone_ofd;
    ofd_ref_hook = end_reading;
    host_read_begin = get_at_walk;
    host_read_end = put_at_walk;
    procfs_text_init(&text);
    len = procfs_gen_maps(&text, 13);
    host_read_begin = NULL;
    host_read_end = NULL;
    ofd_ref_hook = NULL;
    check(len == -ENOENT && reaps == before + 1 && !reading && file_holds == 0,
          "a maps read of a process reaped while its program's path is built ends with ENOENT");
    kfree(text.buf);
}

/**
 * A reading of the numa_maps of a process with a bind policy that ends and
 * is reaped while the reading waits for its address space's lock holds the
 * record across the wait and takes the policy under the lock, as Linux's
 * m_start does, so that it shows the default an ended thread has.
 */
static void numa_maps_policy_after_wait(void)
{
    static struct vm_policy bind = {1, 2, 0, -1, 1, {0}};
    static vma_t region;
    struct procfs_text text;
    int before = reaps, len;

    check(make_reaped(), "allocate the process whose numa_maps is read");
    if (!reading) {
        return;
    }
    reading->refs = 1;
    reading->mempolicy = &bind;
    k_memset(&region, 0, sizeof(region));
    region.start = 0x40000000u;
    region.end = 0x40001000u;
    region.type = VM_ANON;
    region.prot = VM_PROT_READ | VM_PROT_WRITE;
    reading->mm->vm.first = &region;
    host_read_begin = get_at_walk;
    host_read_end = put_at_walk;
    procfs_text_init(&text);
    len = procfs_gen_numa_maps(&text, 13);
    host_read_begin = NULL;
    host_read_end = NULL;
    check(len > 0 && reaps == before && reading, "a numa_maps read of a live process is read");
    check(len == 16 && !text.nomem && text_has(text.buf, len, "40000000 bind:0\n"),
          "and shows its bind policy");
    kfree(text.buf);
    end_walk = 2; /* the names' count of the regions is the first walk */
    host_read_begin = end_at_walk;
    host_read_end = put_at_walk;
    procfs_text_init(&text);
    len = procfs_gen_numa_maps(&text, 13);
    host_read_begin = NULL;
    host_read_end = NULL;
    end_walk = 0;
    check(len > 0 && reaps == before + 1 && !reading,
          "a numa_maps read of a process reaped while it waits for the regions' lock ends");
    check(len == 17 && !text.nomem && text_has(text.buf, len, "40000000 default\n"),
          "and shows the default policy of an ended thread");
    kfree(text.buf);
}

/* The nice value the last autogroup write set, or 99 for none. */
static int autogroup_set = 99;

/** Record the session group's nice value `nice` a write of process `p`'s
 * autogroup file sets. Returns 0. */
int autogroup_nice(process_t *p, int nice)
{
    (void)p;
    autogroup_set = nice;
    return 0;
}

/**
 * A write to /proc/PID/autogroup parses its first 12 bytes and answers 12
 * for a longer one, as Linux 6.12 clamps the count before its copy, so that
 * the writer sees the byte it lost; a shorter write answers its count.
 */
static void autogroup_write_rows(void)
{
    struct procfs_file file;
    fd_entry_t entry;
    const char *text = "000000000005X";

    k_memset(&file, 0, sizeof(file));
    k_memset(&entry, 0, sizeof(entry));
    entry.priv = &file;
    file.origin_pid = 7;
    check(procfs_autogroup_write(&entry, (const char __user *)text, 13) == 12 && autogroup_set == 5,
          "an autogroup write of 13 bytes parses 12 and answers 12");
    text = "-3\n";
    check(procfs_autogroup_write(&entry, (const char __user *)text, 3) == 3 && autogroup_set == -3,
          "an autogroup write of 3 bytes answers 3");
    autogroup_set = 99;
}

/** The rows of a process's files: those read while the process ends and is
 * reaped, among them maps and numa_maps, and the writes of its autogroup
 * file. */
static void pid_file_rows(void)
{
    reaped_during_read();
    stat_reaped_during_wait();
    status_reaped_during_wait();
    maps_reaped_during_names();
    numa_maps_policy_after_wait();
    autogroup_write_rows();
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

/** Write the `count` bytes at `text` to the THP attribute `attr` through
 * its descriptor's write; the call's answer. */
static int32_t thp_write(int attr, const char *text, uint32_t count)
{
    struct procfs_file file;
    struct open_file ofd;
    fd_entry_t entry;

    k_memset(&file, 0, sizeof(file));
    k_memset(&entry, 0, sizeof(entry));
    k_memset(&ofd, 0, sizeof(ofd));
    entry.ofd = &ofd;
    file.origin_file = SYNTH_SYS_UEVENT;
    file.origin_task = SYSFS_THP << 8 | attr;
    entry.priv = &file;
    return procfs_sysattr_write(&entry, (const char __user *)text, count);
}

/** Whether the THP attribute `attr` reads `want`. */
static int thp_reads(int attr, const char *want)
{
    char out[128];

    return sys_text(SYSFS_THP, 0, attr, out, sizeof(out)) > 0 && eq(out, want);
}

/**
 * Writes to the THP files longer than the other attributes' buffer reach
 * the stores whole up to a page, as Linux's kernfs gives them: 32 zeroes
 * to use_zero_page, kstrtobool's long words to shrink_underused, and a
 * write past a page, whose first PAGE_SIZE bytes alone are parsed and
 * taken. Each store's own limit stays: shmem_enabled refuses 16 bytes. An
 * empty write is 0, with no store called.
 */
static void thp_long_writes(void)
{
    static char text[PAGE_SIZE + 4];

    k_memset(text, '0', 32);
    check(thp_write(2, text, 32) == 32 && thp_reads(2, "0\n"),
          "use_zero_page takes 32 zeroes, as Linux's kstrtoul");
    k_memset(text, 'o', 40);
    text[0] = 'n';
    check(thp_write(5, text, 40) == 40 && thp_reads(5, "0\n"),
          "shrink_underused takes a 40-byte word kstrtobool reads as no");
    text[0] = 'Y';
    check(thp_write(5, text, 40) == 40 && thp_reads(5, "1\n"), "and one it reads as yes");
    k_memset(text, '0', PAGE_SIZE - 1);
    k_memcpy(text + PAGE_SIZE - 1, "1xyz", 4);
    check(thp_write(2, text, PAGE_SIZE + 3) == (int32_t)PAGE_SIZE && thp_reads(2, "1\n"),
          "a write past a page gives the store its first page, which it takes");
    k_memcpy(text, "never          \n", 16);
    check(thp_write(4, text, 16) == -EINVAL, "shmem_enabled still refuses 16 bytes");
    check(thp_write(2, text, 0) == 0 && thp_reads(2, "1\n"),
          "an empty write is 0 and reaches no store, as Linux's sysfs_kf_write");
    k_memset(text, 0, sizeof(text));
}

/** Write the `count` bytes at `text` to attribute `attr` of the /sys
 * directory `node` of device 0 through its descriptor's write; the call's
 * answer. */
static int32_t sysattr_write(int node, int attr, const char *text, uint32_t count)
{
    struct procfs_file file;
    struct open_file ofd;
    fd_entry_t entry;

    k_memset(&file, 0, sizeof(file));
    k_memset(&entry, 0, sizeof(entry));
    k_memset(&ofd, 0, sizeof(ofd));
    entry.ofd = &ofd;
    file.origin_task = node << 8 | attr;
    entry.priv = &file;
    return procfs_sysattr_write(&entry, (const char __user *)text, count);
}

/**
 * A backlight's and a radio switch's numbers reach their stores through
 * the writer whole, with the count written, as Linux gives the buffer to
 * kstrtoul: one newline is taken, and a space or a second newline after
 * the number is EINVAL.
 */
static void sysattr_number_writes(void)
{
    check(sysattr_write(SYSFS_BACKLIGHT, BACKLIGHT_BRIGHTNESS, "1\n", 2) == 2 && stored_len == 2,
          "a backlight's brightness takes a number and its newline, given whole");
    check(sysattr_write(SYSFS_BACKLIGHT, BACKLIGHT_BRIGHTNESS, "0x1 ", 4) == -EINVAL &&
              stored_len == 4,
          "and refuses one with a space after it, as Linux's kstrtoul");
    check(sysattr_write(SYSFS_BACKLIGHT, BACKLIGHT_POWER, "+1\n\n", 4) == -EINVAL &&
              stored_len == 4,
          "bl_power refuses a number with two newlines");
    check(sysattr_write(SYSFS_BACKLIGHT, BACKLIGHT_BRIGHTNESS,
                        "0000000000000000000000000000000000000001\n", 41) == 41 &&
              stored_len == 41,
          "a backlight's brightness takes 41 bytes whole, past the old 31-byte buffer");
    check(sysattr_write(SYSFS_RFKILL, RFKILL_ATTR_SOFT, "0\n", 2) == 2 && stored_len == 2,
          "a radio switch's soft takes a number and its newline");
    check(sysattr_write(SYSFS_RFKILL, RFKILL_ATTR_STATE, "1 \n", 3) == -EINVAL && stored_len == 3,
          "and state refuses one with a space before its newline");
}

/** Write the text `text` whole to rtc0's wakealarm; the call's answer. */
static int32_t wakealarm_write(const char *text)
{
    return sysattr_write(SYSFS_RTC0, 5, text, (uint32_t)__builtin_strlen(text));
}

/**
 * rtc0's wakealarm takes what Linux's wakealarm_store takes: the clock
 * read first, then seconds since 1970, `+N` from now or `+=N` to push a
 * set alarm, read as kstrtos64 with base 0, with Linux's errors.
 */
static void wakealarm_writes(void)
{
    check(wakealarm_write("+60") == -EIO, "wakealarm: a clock that does not read is EIO");
    clock_up = 1;
    clock_now = 1000000;
    clock_alarm = -1;
    check(wakealarm_write("+60\n") == 4 && clock_alarm == 1000060,
          "wakealarm: +N sets the alarm N seconds from now");
    check(wakealarm_write("1000100") == -EBUSY && clock_alarm == 1000060,
          "wakealarm: a new time while one is set is EBUSY");
    check(wakealarm_write("+=0x3c") == 6 && clock_alarm == 1000120,
          "wakealarm: +=N pushes the alarm set, its number in hexadecimal");
    check(wakealarm_write("+=-1000121") == -EINVAL && clock_alarm == 1000120 && !clock_came,
          "wakealarm: a push to before 1970 is EINVAL and keeps the alarm, as Linux's "
          "rtc_valid_tm");
    check(wakealarm_write("+=-120") == 6 && clock_alarm == -1 && clock_came == 1,
          "wakealarm: a push to a time not after now raises the alarm at once");
    check(wakealarm_write("+60") == 3 && clock_alarm == 1000060,
          "wakealarm: and then takes a new one");
    check(wakealarm_write("0") == 1 && clock_alarm == -1, "wakealarm: 0 clears it");
    check(wakealarm_write("+=5") == -EINVAL && clock_alarm == -1,
          "wakealarm: a push with none set is EINVAL");
    check(wakealarm_write("1000000abc") == -EINVAL && clock_alarm == -1,
          "wakealarm: junk after the number is EINVAL");
    check(wakealarm_write("1000060 ") == -EINVAL && clock_alarm == -1,
          "wakealarm: so is a space after it");
    check(wakealarm_write("99999999999999999999") == -ERANGE && clock_alarm == -1,
          "wakealarm: a number past 64 bits is ERANGE");
    check(wakealarm_write("+0x3c") == 5 && clock_alarm == 1000060,
          "wakealarm: +N in hexadecimal sets it");
    check(wakealarm_write("999") == 3 && clock_alarm == -1,
          "wakealarm: a time past clears it, as any time not after now");
    check(wakealarm_write("-5") == 2 && clock_alarm == -1, "wakealarm: as a negative one does");
    clock_up = 0;
    clock_now = 0;
    clock_alarm = -1;
    clock_came = 0;
}

static void khugepaged_rows(void);

/** Read the THP files at Linux 6.12's defaults with the policy never, and
 * write each policy as Linux's stores parse it. */
static void thp_rows(void)
{
    static const char *const expected[] = {"always [madvise] never\n",
                                           "always defer defer+madvise [madvise] never\n",
                                           "1\n",
#if ELF_CLASS == 2
                                           "2097152\n",
#else
                                           "4194304\n",
#endif
                                           "always within_size advise [never] deny force\n",
                                           "1\n"};
    static const struct {
        int attr;
        const char *text;
        int32_t rc;
        const char *after;
    } writes[] = {
        {0, "always\n", 7, "[always] madvise never\n"},
        {0, "madvise", 7, "always [madvise] never\n"},
        {0, "never ", -EINVAL, "always [madvise] never\n"},
        {0, "never\n\n", -EINVAL, "always [madvise] never\n"},
        {1, "defer+madvise\n", 14, "always defer [defer+madvise] madvise never\n"},
        {1, "defer", 5, "always [defer] defer+madvise madvise never\n"},
        {1, "sometimes", -EINVAL, "always [defer] defer+madvise madvise never\n"},
        {2, "+0\n", 3, "0\n"},
        {2, "2", -EINVAL, "0\n"},
        {2, "0001", 4, "1\n"},
        {2, "x", -EINVAL, "1\n"},
        {2, "", -EINVAL, "1\n"},
#if ELF_CLASS == 2
        {2, "18446744073709551616", -ERANGE, "1\n"},
        {2, "18446744073709551616x", -ERANGE, "1\n"},
        {2, "18446744073709551615", -EINVAL, "1\n"},
#else
        {2, "4294967296", -ERANGE, "1\n"},
        {2, "4294967296x", -EINVAL, "1\n"},
        {2, "4294967295", -EINVAL, "1\n"},
#endif
        {2, "99999999999999999999x", -ERANGE, "1\n"},
        {3, "1", -EINVAL, 0},
        {4, "within_size\n", 12, "always [within_size] advise never deny force\n"},
        {4, "force", 5, "always within_size advise never deny [force]\n"},
        {4, "never\n\n", -EINVAL, "always within_size advise never deny [force]\n"},
        {4, "never           \n", -EINVAL, "always within_size advise never deny [force]\n"},
        {4, "deny", 4, "always within_size advise never [deny] force\n"},
        {5, "off", 3, "0\n"},
        {5, "yes please", 10, "1\n"},
        {5, "F", 1, "0\n"},
        {5, "on", 2, "1\n"},
        {5, "o", -EINVAL, "1\n"},
        {5, "maybe", -EINVAL, "1\n"},
    };
    char out[128];
    char text[32];
    int i;

    for (i = 0; i < (int)(sizeof(expected) / sizeof(expected[0])); i++) {
        check(sys_text(SYSFS_THP, 0, i, out, sizeof(out)) > 0 && eq(out, expected[i]),
              "THP attribute at Linux's default in Linux's format");
    }
    check(sys_text(SYSFS_THP, 0, 6, out, sizeof(out)) == -ENOENT,
          "no attribute past the THP files");
    for (i = 0; i < (int)(sizeof(writes) / sizeof(writes[0])); i++) {
        uint32_t n = (uint32_t)__builtin_strlen(writes[i].text);

        k_memset(text, 0, sizeof(text));
        k_memcpy(text, writes[i].text, n);
        check(thp_store(writes[i].attr, text, n) == writes[i].rc,
              "THP store takes and refuses what Linux's does");
        check(!writes[i].after || (sys_text(SYSFS_THP, 0, writes[i].attr, out, sizeof(out)) > 0 &&
                                   eq(out, writes[i].after)),
              "THP file reads back the stored policy");
    }
    thp_long_writes();
    khugepaged_rows();
    sysattr_number_writes();
    wakealarm_writes();
    thp_policy.enabled = THP_MADVISE;
    thp_policy.defrag = THP_DEFRAG_MADVISE;
    thp_policy.shmem = 1;
    thp_policy.zero_page = 1;
    thp_policy.shrink_underused = 1;
    model_frames = (512u << 20) / PAGE_SIZE;
    thp_boot();
    check(thp_reads(0, "always [madvise] never\n") &&
              thp_reads(1, "always defer defer+madvise [madvise] never\n") && thp_reads(2, "1\n"),
          "with 512 MiB the THP defaults are Linux's");
    model_frames--;
    thp_boot();
    check(
        thp_reads(0, "always madvise [never]\n") &&
            thp_reads(1, "always defer defer+madvise madvise [never]\n") && thp_reads(2, "0\n") &&
            thp_reads(4, "always within_size advise [never] deny force\n") && thp_reads(5, "1\n"),
        "under 512 MiB defrag is never and use_zero_page 0, as Linux's hugepage_init leaves them");
    model_frames = 0;
    thp_policy.enabled = THP_MADVISE;
    thp_policy.defrag = THP_DEFRAG_MADVISE;
    thp_policy.zero_page = 1;
}

/**
 * The files of transparent_hugepage/khugepaged read Linux 6.12's defaults
 * and counts, and their stores take and refuse what Linux's do: the sleeps
 * and pages_to_scan any unsigned int but pages_to_scan 0, the max_ptes_
 * files at most 511, defrag 0 or 1, the two counts nothing; each write
 * taken wakes khugepaged.
 */
static void khugepaged_rows(void)
{
    static const char *const expected[] = {"60000\n", "1\n", "0\n",    "511\n",  "256\n",
                                           "64\n",    "0\n", "4096\n", "10000\n"};
    static const struct {
        int attr;
        const char *text;
        int32_t rc;
        const char *after;
    } writes[] = {
        {7, "0", -EINVAL, "4096\n"},
        {7, "8\n", 2, "8\n"},
        {7, "+4096", 5, "4096\n"},
        {7, "4294967296", -EINVAL, "4096\n"},
        {8, "4294967295", 10, "4294967295\n"},
        {8, "10000", 5, "10000\n"},
        {0, "-1", -EINVAL, "60000\n"},
        {3, "512", -EINVAL, "511\n"},
        {3, "0", 1, "0\n"},
        {3, "511\n", 4, "511\n"},
        {4, "511", 3, "511\n"},
        {4, "256", 3, "256\n"},
        {5, "1000", -EINVAL, "64\n"},
        {1, "2", -EINVAL, "1\n"},
        {1, "0", 1, "0\n"},
        {1, "1", 1, "1\n"},
        {SYSFS_KHP_FULL_SCANS, "5", -EINVAL, "0\n"},
        {SYSFS_KHP_COLLAPSED, "5", -EINVAL, "0\n"},
    };
    char out[128];
    char text[32];
    int i, changes;

    for (i = 0; i < (int)(sizeof(expected) / sizeof(expected[0])); i++) {
        check(sys_text(SYSFS_THP_KHUGEPAGED, 0, i, out, sizeof(out)) > 0 && eq(out, expected[i]),
              "khugepaged attribute at Linux's default in Linux's format");
    }
    check(sys_text(SYSFS_THP_KHUGEPAGED, 0, 9, out, sizeof(out)) == -ENOENT,
          "no attribute past the khugepaged files");
    check(!sysfs_file_writable(SYSFS_THP_KHUGEPAGED, 0, SYSFS_KHP_FULL_SCANS) &&
              sysfs_file_writable(SYSFS_THP_KHUGEPAGED, 0, 7),
          "full_scans is read-only and pages_to_scan writable");
    for (i = 0; i < (int)(sizeof(writes) / sizeof(writes[0])); i++) {
        uint32_t n = (uint32_t)__builtin_strlen(writes[i].text);

        k_memset(text, 0, sizeof(text));
        k_memcpy(text, writes[i].text, n);
        changes = thp_changes;
        check(khugepaged_store(writes[i].attr, text, n) == writes[i].rc,
              "khugepaged store takes and refuses what Linux's does");
        check(thp_changes == changes + (writes[i].rc > 0), "a write taken wakes khugepaged");
        check(sys_text(SYSFS_THP_KHUGEPAGED, 0, writes[i].attr, out, sizeof(out)) > 0 &&
                  eq(out, writes[i].after),
              "khugepaged file reads back the stored value");
    }
    thp_note(THP_FULL_SCANS);
    thp_note(THP_PAGES_COLLAPSED);
    thp_note(THP_PAGES_COLLAPSED);
    check(sys_text(SYSFS_THP_KHUGEPAGED, 0, SYSFS_KHP_FULL_SCANS, out, sizeof(out)) > 0 &&
              eq(out, "1\n") &&
              sys_text(SYSFS_THP_KHUGEPAGED, 0, SYSFS_KHP_COLLAPSED, out, sizeof(out)) > 0 &&
              eq(out, "2\n"),
          "full_scans and pages_collapsed read the counts");
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

void drm_modeset_lock_nointr(struct drm_device *dev)
{
    (void)dev;
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

/** The binary config file writes exact widths and reads current registers. */
static void pci_config_rows(void)
{
    struct vpath vp;
    char data[8] = {1, 2, 3, 4, 5, 6, 7, 8}, got[8];
    fd_entry_t *e = &entries[0];

    k_memset(&vp, 0, sizeof(vp));
    vp.kind = VNODE_SYS_FILE;
    vp.sysent = SYSFS_PCI_DEVICE;
    vp.procfile = SYSFS_PCI_CONFIG;
    vp.syskey = 0x20;
    process.fds = &table;
    table.chunks[0] = entries;
    table.cap = 1;
    k_memset(e, 0, sizeof(*e));
    k_memset(&mem_ofd, 0, sizeof(mem_ofd));
    e->ofd = &mem_ofd;
    process.euid = 1000;
    check(procfs_open_pciconfig(&vp, 2) == -EACCES, "unprivileged config write open is refused");
    process.euid = 0;
    check(procfs_open_pciconfig(&vp, 2 | PROCFS_O_CLOEXEC) == 0 &&
              e->flags == (FD_FLAG_READ | FD_FLAG_WRITE | FD_FLAG_CLOEXEC),
          "privileged binary config open keeps its access and close-on-exec flags");
    pci_writes = 0;
    pci_bytes[0x40] = pci_bytes[0x48] = 0xa5;
    e->ofd->offset = 0x41;
    check(procfs_pciconfig_write(e, data, 7) == 7 && e->ofd->offset == 0x48 && pci_writes == 3 &&
              pci_write_at[0] == 0x41 && pci_write_width[0] == 1 && pci_write_at[1] == 0x42 &&
              pci_write_width[1] == 2 && pci_write_at[2] == 0x44 && pci_write_width[2] == 4 &&
              pci_bytes[0x40] == 0xa5 && pci_bytes[0x48] == 0xa5,
          "unaligned writes use byte, word and dword without touching neighbors");
    e->ofd->offset = 0x41;
    check(procfs_pciconfig_read(e, got, 7) == 7 && __builtin_memcmp(data, got, 7) == 0,
          "config reads the bytes just written at a nonzero offset");
    pci_bytes[0x41] = 0x5a;
    e->ofd->offset = 0x41;
    check(procfs_pciconfig_read(e, got, 1) == 1 && got[0] == 0x5a,
          "another pread sees a hardware change without reopening");
    e->ofd->offset = 255;
    check(procfs_pciconfig_write(e, data, 8) == 1 && e->ofd->offset == 256,
          "a write is cut at the end of conventional config space");
    check(procfs_pciconfig_write(e, data, 8) == 0 && procfs_pciconfig_read(e, got, 8) == 0,
          "reads and writes at the end return zero");
    e->ofd->offset = 0x40;
    pci_writes = 0;
    check(procfs_pciconfig_write(e, NULL, 4) == -EFAULT && !pci_writes,
          "a bad user buffer writes no register");
    refuse = 1;
    check(procfs_pciconfig_write(e, data, 4) == -ENOMEM && !pci_writes,
          "an allocation failure writes no register");
    refuse = 0;
    pci_extended = 1;
    e->ofd->offset = 0x101;
    check(procfs_pciconfig_write(e, data, 7) == 7 && pci_writes == 3 && pci_write_at[0] == 0x101 &&
              pci_write_at[1] == 0x102 && pci_write_at[2] == 0x104,
          "narrow and dword writes reach extended space");
    e->ofd->offset = 0x101;
    check(procfs_pciconfig_read(e, got, 7) == 7 && __builtin_memcmp(data, got, 7) == 0,
          "extended bytes read back");
    e->ofd->offset = ~(uint64_t)0;
    check(procfs_pciconfig_read(e, got, 1) == 0 && procfs_pciconfig_write(e, data, 1) == 0,
          "a wide offset never wraps into the header");
    pci_extended = 0;
    process.euid = 1000;
    e->ofd->offset = 64;
    check(procfs_pciconfig_read(e, got, 1) == 0, "unprivileged reads stop at the header");
    process.euid = 0;
    procfs_close(e);
}

/** Open the resourceN file `attr` of function 0x20 with `flags` into
 * descriptor 0 as the rows' caller: the open's result. */
static int bar_file_open(int attr, int flags)
{
    struct vpath vp;
    fd_entry_t *e = &entries[0];

    k_memset(&vp, 0, sizeof(vp));
    vp.kind = VNODE_SYS_FILE;
    vp.sysent = SYSFS_PCI_DEVICE;
    vp.procfile = attr;
    vp.syskey = 0x20;
    process.fds = &table;
    table.chunks[0] = entries;
    table.cap = 1;
    k_memset(e, 0, sizeof(*e));
    k_memset(&mem_ofd, 0, sizeof(mem_ofd));
    e->ofd = &mem_ofd;
    return procfs_open_pcibar(&vp, flags);
}

/** Map `pages` pages of descriptor 0 from the page `pgoff`, shared. */
static int bar_file_map(uint32_t pgoff, uint32_t pages)
{
    vma_t v;

    k_memset(&v, 0, sizeof(v));
    v.end = (uintptr_t)pages * PAGE_SIZE;
    v.type = VM_SHARED;
    v.pgoff = pgoff;
    mapped_ops = NULL;
    return procfs_pcibar_mmap(&entries[0], &v);
}

/** Every file, directory and bus link has a distinct inode, within one
 * function, beside its neighbor and at the last PCI function's key. */
static void pci_inode_rows(void)
{
    static const int keys[] = {0, 1, 0xFFFF};
    uint32_t inodes[3 * (SYSFS_PCI_ATTRS + 1)], ino;
    int key, file, n = 0, i, ok = 1;

    for (key = 0; key < 3; key++) {
        for (file = 0; file <= SYSFS_PCI_ATTRS; file++) {
            ino = sysfs_ino(SYSFS_PCI_DEVICE, keys[key], file);
            for (i = 0; i < n; i++) {
                ok = ok && ino != inodes[i];
            }
            inodes[n++] = ino;
        }
    }
    check(ok, "PCI directories, bus links and all resource aliases have distinct inodes");
}

/** Which resourceN files a function has, one for each BAR with a length,
 * each as long as its BAR, and that root alone opens them. */
static void pci_bar_file_rows(void)
{
    check(sysfs_pci_attr_present(0x20, SYSFS_PCI_RESOURCE0) &&
              sysfs_pci_attr_present(0x20, SYSFS_PCI_RESOURCE0 + 4) &&
              !sysfs_pci_attr_present(0x20, SYSFS_PCI_RESOURCE0 + 1) &&
              !sysfs_pci_attr_present(0x20, SYSFS_PCI_RESOURCE0 + 5) &&
              !sysfs_pci_attr_present(0x28, SYSFS_PCI_RESOURCE0) &&
              sysfs_pci_attr_present(0x20, SYSFS_PCI_CONFIG),
          "a function has resource0 and resource4, its BARs with a length, and no others");
    check(sysfs_pci_bar_size(0x20, 0) == 0x800000u && sysfs_pci_bar_size(0x20, 4) == 0x4000u &&
              sysfs_pci_bar_size(0x20, 6) == 0,
          "and each is as long as its BAR");
    process.euid = 1000;
    check(bar_file_open(SYSFS_PCI_RESOURCE0, 0) == -EACCES,
          "an unprivileged caller opens no resource file, even to read");
    process.euid = 0;
    process.fsuid = 1000;
    process.fs_own = PROC_FSUID_OWN | PROC_FSCAPS_OFF;
    check(bar_file_open(SYSFS_PCI_RESOURCE0, 0) == -EACCES,
          "nor root once setfsuid(1000) takes its file-system id and with it CAP_DAC_OVERRIDE");
    process.fs_own = 0;
    process.cap_lost[0] = 0xFFFFFFFFu;
    process.cap_lost[1] = CAP_HIGH_WORD;
    check(bar_file_open(SYSFS_PCI_RESOURCE0, 2) == 0,
          "but uid 0 opens it as its owner with every capability dropped");
    procfs_close(&entries[0]);
    process.cap_lost[0] = process.cap_lost[1] = 0;
    process.euid = 1000;
    process.fsuid = 0;
    process.fs_own = PROC_FSUID_OWN;
    check(bar_file_open(SYSFS_PCI_RESOURCE0, 1) == 0,
          "and so does a user whose file-system id is root's");
    procfs_close(&entries[0]);
    process.euid = 0;
    process.fsuid = 0;
    process.fs_own = 0;
    check(bar_file_open(SYSFS_PCI_RESOURCE0 + 1, 2) == -ENOENT, "nor anyone a BAR without one");
}

/**
 * A memory BAR's resourceN file, as IGT's register access maps BAR 0
 * through libpciaccess: mapped shared and uncached, page by page at the
 * address the BAR holds, within its length, with no read or write.
 */
static void pci_bar_mem_rows(void)
{
    fd_entry_t *e = &entries[0];
    uintptr_t phys = 0;
    char got[4], put[2] = {0x34, 0x12};
    vma_t v;

    check(bar_file_open(SYSFS_PCI_RESOURCE0, 2 | PROCFS_O_CLOEXEC) == 0 &&
              e->ops == &procfs_pcibar_ops &&
              e->flags == (FD_FLAG_READ | FD_FLAG_WRITE | FD_FLAG_CLOEXEC),
          "root opens resource0 for reading and writing, close-on-exec");
    check(procfs_pcibar_read(e, got, 4) == -EIO && procfs_pcibar_write(e, put, 2) == -EIO,
          "a memory BAR has no read or write");
    check(bar_file_map(0, 512) == 0 && mapped_ops == &pcibar_vm_ops && mapped_cache == VM_PROT_UC,
          "2 MiB of BAR 0 map shared and uncached, as IGT maps the registers");
    check(mapped_ops && mapped_ops->page(mapped_obj, 1, &phys) == 0 && phys == 0xFE001000u &&
              mapped_ops->page(mapped_obj, 2047, &phys) == 0 && phys == 0xFE7FF000u &&
              mapped_ops->page(mapped_obj, 2048, &phys) == -1,
          "a page of the mapping is the BAR's page, and past the BAR's end is none");
    check(bar_file_map(2047, 2) == -EINVAL && bar_file_map(2048, 1) == -EINVAL &&
              bar_file_map(2047, 1) == 0,
          "a range past the BAR's end is refused, as pci_mmap_fits refuses it");
    k_memset(&v, 0, sizeof(v));
    v.end = PAGE_SIZE;
    v.type = VM_FILE;
    check(procfs_pcibar_mmap(e, &v) == -EINVAL, "and so is a private mapping");
    procfs_close(e);
    check(bar_file_open(SYSFS_PCI_RESOURCE0 + 4, 0) == 0 && bar_file_map(0, 4) == 0 &&
              mapped_ops->page(mapped_obj, 3, &phys) == 0 && phys == 0xFE803000u,
          "the 64-bit BAR 4 maps its four pages");
    procfs_close(e);
}

/** Prefetchable BARs have write-combining aliases only while PAT supplies
 * that type; the aliases keep the BAR's address, bounds and access rules. */
static void pci_bar_wc_rows(void)
{
    fd_entry_t *e = &entries[0];
    uintptr_t phys = 0;

    check(sysfs_pci_attr_present(0x20, SYSFS_PCI_RESOURCE0_WC) &&
              sysfs_pci_attr_present(0x20, SYSFS_PCI_RESOURCE0_WC + 4) &&
              !sysfs_pci_attr_present(0x20, SYSFS_PCI_RESOURCE0_WC + 1) &&
              !sysfs_pci_attr_present(0x28, SYSFS_PCI_RESOURCE0_WC),
          "prefetchable memory BARs alone have resourceN_wc files");
    pci_no_prefetch = 1;
    check(!sysfs_pci_attr_present(0x20, SYSFS_PCI_RESOURCE0_WC) &&
              sysfs_pci_attr_present(0x20, SYSFS_PCI_RESOURCE0),
          "a memory BAR without prefetch has only its uncached file");
    pci_no_prefetch = 0;
    pci_no_wc = 1;
    check(!sysfs_pci_attr_present(0x20, SYSFS_PCI_RESOURCE0_WC) &&
              bar_file_open(SYSFS_PCI_RESOURCE0_WC, 2) == -ENOENT &&
              sysfs_pci_attr_present(0x20, SYSFS_PCI_RESOURCE0),
          "without PAT write-combining, only the uncached file exists");
    pci_no_wc = 0;
    process.euid = 1000;
    check(bar_file_open(SYSFS_PCI_RESOURCE0_WC, 0) == -EACCES,
          "an unprivileged caller cannot open the write-combining alias");
    process.euid = 0;
    check(bar_file_open(SYSFS_PCI_RESOURCE0_WC, 2) == 0 && bar_file_map(1, 512) == 0 &&
              mapped_cache == VM_PROT_WC && mapped_ops == &pcibar_vm_ops &&
              mapped_ops->page(mapped_obj, 1, &phys) == 0 && phys == 0xFE001000u,
          "resource0_wc maps BAR 0's pages write-combining");
    check(bar_file_map(2047, 2) == -EINVAL && bar_file_map(2047, 1) == 0,
          "the write-combining alias keeps the BAR's bounds");
    procfs_close(e);
    check(bar_file_open(SYSFS_PCI_RESOURCE0_WC + 4, 0) == 0 && bar_file_map(0, 4) == 0 &&
              mapped_cache == VM_PROT_WC && mapped_ops->page(mapped_obj, 3, &phys) == 0 &&
              phys == 0xFE803000u,
          "resource4_wc names the 64-bit BAR 4");
    procfs_close(e);
    pci_io_bar = 1;
    check(!sysfs_pci_attr_present(0x20, SYSFS_PCI_RESOURCE0_WC + 2) &&
              bar_file_open(SYSFS_PCI_RESOURCE0_WC + 2, 0) == -ENOENT,
          "an I/O BAR has no write-combining alias");
    pci_io_bar = 0;
}

/** An I/O BAR's resourceN file: read and written a port at a time, 1, 2 or
 * 4 bytes, within the BAR, and not mapped, as on x86 Linux. */
static void pci_bar_io_rows(void)
{
    fd_entry_t *e = &entries[0];
    char got[4], put[2] = {0x34, 0x12};

    pci_io_bar = 1;
    check(sysfs_pci_attr_present(0x20, SYSFS_PCI_RESOURCE0 + 2) &&
              bar_file_open(SYSFS_PCI_RESOURCE0 + 2, 2) == 0,
          "an I/O BAR has its resource file");
    e->ofd->offset = 0x10;
    check(procfs_pcibar_read(e, got, 4) == 4 && port_at == 0xC010u && port_width == 4 &&
              (uint8_t)got[0] == 0x10 && (uint8_t)got[1] == 0xC0 && got[2] == 0 &&
              (uint8_t)got[3] == 0x40 && e->ofd->offset == 0x14,
          "a read of 4 bytes is one dword from the BAR's port at the offset, little-endian");
    e->ofd->offset = 0x4;
    check(procfs_pcibar_write(e, put, 2) == 2 && port_at == 0xC004u && port_width == 2 &&
              port_value == 0x1234u && e->ofd->offset == 0x6,
          "a write of 2 bytes is one word to its port");
    e->ofd->offset = 0x10;
    check(procfs_pcibar_read(e, got, 3) == -EINVAL && procfs_pcibar_read(e, got, 8) == -EINVAL &&
              e->ofd->offset == 0x10,
          "a width other than 1, 2 or 4 within the BAR is refused");
    check(bar_file_map(0, 1) == -ENODEV, "and an I/O BAR is not mapped, as on x86");
    procfs_close(e);
    pci_io_bar = 0;
}

/** An I/O BAR's resourceN file clips an access to the file before it
 * judges the width, as Linux's sysfs does before pci_resource_io. */
static void pci_bar_clip_rows(void)
{
    fd_entry_t *e = &entries[0];
    char got[4], put[4] = {0x34, 0x12, 0, 0};

    pci_io_bar = 1;
    check(bar_file_open(SYSFS_PCI_RESOURCE0 + 2, 2) == 0, "root opens the I/O BAR's file");
    e->ofd->offset = 0x1E;
    check(procfs_pcibar_read(e, got, 4) == 2 && port_at == 0xC01Eu && port_width == 2 &&
              e->ofd->offset == 0x20,
          "a read of 4 bytes 2 before the BAR's end is clipped to one word, as sysfs clips it");
    e->ofd->offset = 0x1D;
    check(procfs_pcibar_read(e, got, 4) == -EINVAL,
          "and one clipped to 3 bytes is refused, as pci_resource_io refuses it");
    e->ofd->offset = 0x1F;
    check(procfs_pcibar_write(e, put, 4) == 1 && port_at == 0xC01Fu && port_width == 1 &&
              port_value == 0x34u,
          "a write of 4 bytes 1 before the end is clipped to its first byte");
    e->ofd->offset = 0x1F;
    port_at = 0;
    fault_from = put + 1;
    fault_end = put + sizeof(put);
    check(procfs_pcibar_write(e, put, 4) == -EFAULT && port_at == 0 && e->ofd->offset == 0x1F,
          "but one whose last 3 bytes cannot be read is -EFAULT with no port written, as "
          "kernfs copies the whole count before sysfs clips it");
    fault_from = fault_end = NULL;
    e->ofd->offset = 0x20;
    check(procfs_pcibar_read(e, got, 4) == 0 && procfs_pcibar_write(e, put, 2) == -EFBIG &&
              procfs_pcibar_write(e, put, 0) == -EFBIG && e->ofd->offset == 0x20,
          "at the end a read gives 0 and a write -EFBIG");
    e->ofd->offset = 0x4;
    check(procfs_pcibar_read(e, got, 0) == 0 && procfs_pcibar_write(e, put, 0) == 0 &&
              e->ofd->offset == 0x4,
          "and an access of no bytes within the BAR moves nothing");
    procfs_close(e);
    pci_io_bar = 0;
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
    check(procfs_gen_pciresource(&t, 0xA0) == 13 * 57 &&
              __builtin_memcmp(t.buf, "0x0000000000000000 0x0000000000000000 0x0000000000000000\n",
                               57) == 0 &&
              __builtin_memcmp(t.buf + 12 * 57,
                               "0x0000000000000000 0x0000000000000000 0x0000000000000000\n",
                               57) == 0,
          "and a resource file of 13 empty lines, since it has no BARs");
    t.len = 0;
    check(procfs_gen_pciresource(&t, 0x20) == 13 * 57 &&
              __builtin_memcmp(t.buf, "0x00000000fe000000 0x00000000fe7fffff 0x0000000000042208\n",
                               57) == 0 &&
              __builtin_memcmp(t.buf + 4 * 57,
                               "0x00000000fe800000 0x00000000fe803fff 0x000000000014220c\n",
                               57) == 0 &&
              __builtin_memcmp(t.buf + 5 * 57,
                               "0x0000000000000000 0x0000000000000000 0x0000000000000000\n",
                               57) == 0,
          "a real function's resource file has its BARs in Linux's format, a line each");
    t.len = 0;
    check(procfs_gen_pciresource(&t, 0x28) == -ENOENT, "and a function not there has none");
    t.len = 0;
    check(procfs_gen_uevent(&t, SYSFS_PCI_DEVICE, 0xA0) > 0 && t.buf &&
              __builtin_strstr(t.buf, "PCI_ID=8086:0A16\n") &&
              __builtin_strstr(t.buf, "PCI_SLOT_NAME=0000:00:14.0\n"),
          "and its uevent the IDs and slot libdrm parses");
    kfree(t.buf);
    sysfs_pci_virtual_remove(0xA0);
    check(!sysfs_device_exists(SYSFS_PCI_DEVICE, 0xA0), "a withdrawn function is gone");
}

/* The value of the attribute of `sysattr_file_rows`, 'Y' or 'N'. */
static char flag_value = 'Y';

/** The attribute's text, its value and a newline, as a module parameter's
 * file reads. */
static int32_t show_flag(void *obj, int arg, char *buf, uint32_t cap)
{
    (void)obj;
    (void)arg;
    if (cap >= 2u) {
        buf[0] = flag_value;
        buf[1] = '\n';
    }
    return 2;
}

/** Take the first byte of a write as the value, and the whole count. */
static int32_t store_flag(void *obj, int arg, const char *buf, uint32_t len)
{
    (void)obj;
    (void)arg;
    flag_value = buf[0];
    return (int32_t)len;
}

/** Open the registered attribute `h` with `flags` as descriptor 0, its
 * description's offset at 0, as `bar_file_open` opens a BAR's file. */
static int reg_file_open(int h, int flags)
{
    fd_entry_t *e = &entries[0];

    process.fds = &table;
    table.chunks[0] = entries;
    table.cap = 1;
    k_memset(e, 0, sizeof(*e));
    k_memset(&mem_ofd, 0, sizeof(mem_ofd));
    e->ofd = &mem_ofd;
    return procfs_open_sysattr(SYSFS_REG, h, 0, flags);
}

/** Whether a read open of the registered attribute `h` with `flags` is
 * refused, as `procfs_open` asks before it renders the file. */
static int reg_open_refused(int h, int flags)
{
    struct vpath vp;

    k_memset(&vp, 0, sizeof(vp));
    vp.kind = VNODE_SYS_FILE;
    vp.sysent = SYSFS_REG;
    vp.syskey = h;
    return sysfs_read_refused(&vp, flags);
}

/** `reg_open_refused` of an O_RDONLY open. */
static int reg_read_refused(int h)
{
    return reg_open_refused(h, 0);
}

/** Whether a read open of the DMI field `attr` with `flags` is refused. */
static int dmi_open_refused(int attr, int flags)
{
    struct vpath vp;

    k_memset(&vp, 0, sizeof(vp));
    vp.kind = VNODE_SYS_FILE;
    vp.sysent = SYSFS_DMI_ID;
    vp.procfile = attr;
    return sysfs_read_refused(&vp, flags);
}

/**
 * O_TRUNC, which Linux's build_open_flags adds MAY_WRITE for, on a read
 * open: a user may not open a 0644 attribute with O_RDONLY | O_TRUNC, nor
 * a 0444 one, while root may by its owner's bits or CAP_DAC_OVERRIDE, and
 * uid 0 without capabilities only where the owner's bits write. The DMI
 * fields of mode 0400 are judged by the mode and the file-system user id:
 * root reads them with no capability left, a user holding
 * CAP_DAC_READ_SEARCH too, and not root after setfsuid(1000).
 */
static void sysattr_trunc_rows(void)
{
    static const struct sysfs_attr rw = {"rw", 0644u, show_flag, store_flag};
    static const struct sysfs_attr ro = {"ro", 0444u, show_flag, NULL};
    int dir = sysfs_add_dir(sysfs_anchor(SYSFS_KERNEL, 0), "trunc_rows");
    int hw = sysfs_add_attr(dir, &rw, NULL, 0), hr = sysfs_add_attr(dir, &ro, NULL, 0);

    check(dir > 0 && hw > 0 && hr > 0, "a 0644 and a 0444 attribute are made");
    process.euid = 1000;
    process.fsuid = 1000;
    check(!reg_read_refused(hw) && reg_open_refused(hw, PROCFS_O_TRUNC),
          "a user reads the 0644 one, and O_RDONLY | O_TRUNC needs the write it lacks");
    check(reg_open_refused(hr, PROCFS_O_TRUNC) && !dmi_open_refused(DMI_PRODUCT_NAME, 0) &&
              dmi_open_refused(DMI_PRODUCT_NAME, PROCFS_O_TRUNC),
          "as on a 0444 one, registered or fixed");
    check(dmi_open_refused(DMI_PRODUCT_SERIAL, 0) && dmi_open_refused(DMI_PRODUCT_UUID, 0),
          "a user may not read the 0400 serial and UUID");
    process.amb_granted = 1;
    check(!dmi_open_refused(DMI_PRODUCT_SERIAL, 0) && !reg_open_refused(hr, PROCFS_O_TRUNC),
          "but may holding CAP_DAC_READ_SEARCH, and with CAP_DAC_OVERRIDE truncate a 0444 file");
    process.amb_granted = 0;
    process.euid = 0;
    process.fsuid = 0;
    check(!reg_open_refused(hw, PROCFS_O_TRUNC) && !reg_open_refused(hr, PROCFS_O_TRUNC) &&
              !dmi_open_refused(DMI_PRODUCT_SERIAL, PROCFS_O_TRUNC),
          "root opens either with O_TRUNC, by its bits or CAP_DAC_OVERRIDE");
    process.cap_lost[0] = 0xFFFFFFFFu;
    process.cap_lost[1] = CAP_HIGH_WORD;
    process.nocaps = 1;
    check(!dmi_open_refused(DMI_BOARD_SERIAL, 0) && !reg_open_refused(hw, PROCFS_O_TRUNC),
          "uid 0 with no capability reads a 0400 field and truncates a 0644 file as owner");
    check(reg_open_refused(hr, PROCFS_O_TRUNC) &&
              dmi_open_refused(DMI_CHASSIS_SERIAL, PROCFS_O_TRUNC),
          "but not a file whose owner's bits do not write");
    process.cap_lost[0] = process.cap_lost[1] = 0;
    process.nocaps = 0;
    process.fsuid = 1000;
    process.fs_own = PROC_FSUID_OWN | PROC_FSCAPS_OFF;
    check(dmi_open_refused(DMI_PRODUCT_SERIAL, 0),
          "effective root after setfsuid(1000) may not read the 0400 serial");
    process.fsuid = 0;
    process.fs_own = 0;
    sysfs_remove(dir);
}

/**
 * A write-only attribute of mode 0200, with no show as VFIO's and i915's
 * bind files have none and with one, and a read-only one of 0444 with a
 * store, opened as Linux's kernfs_fop_open judges them after
 * generic_permission: a direction the file does not support is refused
 * before CAP_DAC_OVERRIDE can pass it, so that a user holding it opens
 * neither write-only file to read, alone or with a write, and no caller
 * writes the read-only one, while the mode lets root write a write-only
 * one, and the capability a user.
 */
static void sysattr_direction_rows(void)
{
    static const struct sysfs_attr bind = {"bind", 0200u, NULL, store_flag};
    static const struct sysfs_attr shown = {"shown", 0200u, show_flag, store_flag};
    static const struct sysfs_attr stored = {"stored", 0444u, show_flag, store_flag};
    int dir = sysfs_add_dir(sysfs_anchor(SYSFS_KERNEL, 0), "direction_rows");
    int hb = sysfs_add_attr(dir, &bind, NULL, 0);
    int hs = sysfs_add_attr(dir, &shown, NULL, 0);
    int hr = sysfs_add_attr(dir, &stored, NULL, 0);

    check(dir > 0 && hb > 0 && hs > 0 && hr > 0,
          "the write-only and read-only attributes are made");
    process.euid = 1000;
    process.fsuid = 1000;
    process.amb_granted = 1;
    check(cap_holds(&process, CAP_DAC_OVERRIDE), "a user holds CAP_DAC_OVERRIDE as ambient");
    check(reg_file_open(hb, 2) == -EACCES,
          "and is refused an O_RDWR open of a write-only file with no show");
    check(reg_file_open(hs, 2) == -EACCES, "and of a write-only file with one");
    check(reg_read_refused(hb) && reg_read_refused(hs),
          "and an O_RDONLY open of either, though the capability passes the mode");
    check(reg_file_open(hb, 1) == 0, "while its O_WRONLY open passes by the capability");
    procfs_close(&entries[0]);
    check(reg_file_open(hr, 1) == -EACCES && reg_file_open(hr, 2) == -EACCES,
          "and a read-only file with a store refuses it a write");
    check(!reg_read_refused(hr), "but reads to it");
    process.amb_granted = 0;
    check(reg_file_open(hb, 1) == -EACCES && reg_read_refused(hb),
          "a user without the capability opens the write-only file neither way");
    process.euid = 0;
    process.fsuid = 0;
    check(reg_file_open(hb, 1) == 0, "root writes the write-only file by its mode");
    procfs_close(&entries[0]);
    check(reg_file_open(hb, 2) == -EACCES && reg_read_refused(hb),
          "and opens it to read neither alone nor with a write");
    check(reg_file_open(hr, 1) == -EACCES, "nor does root write the read-only file");
    sysfs_remove(dir);
}

/**
 * A writable attribute of mode 0644, as /sys/module/printk/parameters has
 * them, through its descriptor, as Linux's kernfs gives it: a write moves
 * the offset past the count taken, so that a read through an O_RDWR
 * descriptor after `N\n` starts at 2 and finds the end, and a write past a
 * page moves it a page; and the open is judged by the mode and the
 * file-system user id, so that root after setfsuid(1000) is refused and
 * uid 0 without CAP_SYS_ADMIN, or a user whose file-system id is root's,
 * writes it as its owner. The rows of the directions an open refuses
 * follow (`sysattr_direction_rows`).
 */
static void sysattr_file_rows(void)
{
    static const struct sysfs_attr attr = {"flag", 0644u, show_flag, store_flag};
    static char text[PAGE_SIZE + 3];
    fd_entry_t *e = &entries[0];
    char out[4];
    int dir = sysfs_add_dir(sysfs_anchor(SYSFS_KERNEL, 0), "flag_rows");
    int h = sysfs_add_attr(dir, &attr, NULL, 0);

    check(dir > 0 && h > 0 && reg_file_open(h, 2) == 0,
          "root opens a 0644 attribute to read and write");
    check(procfs_sysattr_write(e, (const char __user *)"N\n", 2) == 2 && flag_value == 'N' &&
              mem_ofd.offset == 2,
          "a write of N and a newline moves the offset to 2, as kernfs_fop_write_iter");
    check(procfs_sysattr_read(e, (char __user *)out, sizeof(out)) == 0 && mem_ofd.offset == 2,
          "and a read through the same descriptor starts there, at the end");
    mem_ofd.offset = 0;
    check(procfs_sysattr_read(e, (char __user *)out, sizeof(out)) == 2 && out[0] == 'N',
          "while one from offset 0 reads the value written");
    k_memset(text, 'Y', sizeof(text));
    check(procfs_sysattr_write(e, (const char __user *)text, sizeof(text)) == (int32_t)PAGE_SIZE &&
              mem_ofd.offset == 2u + PAGE_SIZE,
          "a write past a page is taken in part and moves the offset by the page taken");
    procfs_close(e);

    process.euid = 0;
    process.fsuid = 1000;
    process.fs_own = PROC_FSUID_OWN | PROC_FSCAPS_OFF;
    check(reg_file_open(h, 1) == -EACCES,
          "effective root is refused once setfsuid(1000) takes its file-system id");
    process.fsuid = 0;
    process.fs_own = 0;
    process.cap_lost[0] = 1u << CAP_SYS_ADMIN;
    process.nocaps = 1;
    check(reg_file_open(h, 2) == 0, "uid 0 without CAP_SYS_ADMIN opens it as its owner, by mode");
    procfs_close(e);
    process.cap_lost[0] = 0xFFFFFFFFu;
    process.cap_lost[1] = CAP_HIGH_WORD;
    check(reg_file_open(h, 1) == 0, "and so it does with every capability dropped");
    procfs_close(e);
    process.cap_lost[0] = process.cap_lost[1] = 0;
    process.nocaps = 0;
    process.euid = 1000;
    process.fsuid = 0;
    process.fs_own = PROC_FSUID_OWN;
    check(reg_file_open(h, 1) == 0, "and a user whose file-system id is root's");
    procfs_close(e);
    process.fsuid = 1000;
    process.fs_own = 0;
    check(reg_file_open(h, 1) == -EACCES, "while another user is refused");
    process.euid = 0;
    process.fsuid = 0;
    sysfs_remove(dir);
    sysattr_direction_rows();
    sysattr_trunc_rows();
}

/** Whether the uevent of the PCI function `key` begins with `first`. */
static int pci_uevent_begins(int key, const char *first)
{
    struct procfs_text t;
    int ok;

    procfs_text_init(&t);
    ok = procfs_gen_uevent(&t, SYSFS_PCI_DEVICE, key) > 0 && t.buf &&
         __builtin_strncmp(t.buf, first, __builtin_strlen(first)) == 0;
    kfree(t.buf);
    return ok;
}

/** A function shown bound to a driver of the kernel, as Linux's driver
 * core shows it: the driver's directory and the two links, DRIVER= first
 * in the uevent, and all of it gone when it is unbound. */
static void sysfs_bound_rows(void)
{
    int dev, drivers, dir, link;
    const char *d;

    check(sysfs_pci_bind(0x20, "virtio-pci") == 0 && sysfs_pci_bind(0x20, "virtio-pci") == 0 &&
              sysfs_pci_bind(0x20, "bochs-drm") == -EBUSY,
          "bound: a function is shown bound to one driver");
    d = sysfs_pci_driver(0x20);
    check(d && __builtin_strcmp(d, "virtio-pci") == 0 && !sysfs_pci_driver(0x28),
          "bound: its driver is named, and an unbound function has none");
    dev = sysfs_anchor(SYSFS_PCI_DEVICE, 0x20);
    link = sysfs_node_child(dev, "driver", 6);
    check(link > 0 && link_reads(link, "../../../bus/pci/drivers/virtio-pci"),
          "bound: the function's driver link names the driver's directory");
    drivers = sysfs_node_child(sysfs_anchor(SYSFS_BUS_PCI, 0), "drivers", 7);
    dir = drivers > 0 ? sysfs_node_child(drivers, "virtio-pci", 10) : 0;
    link = dir > 0 ? sysfs_node_child(dir, "0000:00:04.0", 12) : 0;
    check(link > 0 && link_reads(link, "../../../../devices/pci0000:00/0000:00:04.0"),
          "bound: and the driver's directory links back to the function by its address");
    check(pci_uevent_begins(0x20, "DRIVER=virtio-pci\nPCI_CLASS="),
          "bound: the uevent names the driver first, as Linux's");
    sysfs_pci_unbind(0x20);
    check(!sysfs_pci_driver(0x20) && sysfs_node_child(dev, "driver", 6) <= 0 &&
              sysfs_node_child(dir, "0000:00:04.0", 12) <= 0 &&
              pci_uevent_begins(0x20, "PCI_CLASS="),
          "bound: unbound, both links and DRIVER= are gone");
    pci_owner_name = "vfio-pci";
    check(pci_uevent_begins(0x20, "DRIVER=vfio-pci\n"),
          "bound: a function vfio-pci holds names it in its uevent");
    pci_owner_name = PCI_OWNER_KERNEL;
    check(pci_uevent_begins(0x20, "PCI_CLASS="),
          "bound: one the kernel holds unnamed names no driver");
    pci_owner_name = 0;
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

/** Whether the file of /proc/bus/input `file` reads `want`. */
static int bus_input_reads(int file, const char *want)
{
    struct procfs_text text;
    int len, ok;

    procfs_text_init(&text);
    len = procfs_gen_bus_input(&text, file);
    ok = len == (int)__builtin_strlen(want) && text.buf &&
         __builtin_memcmp(text.buf, want, (unsigned long)len) == 0;
    if (!ok) {
        printf("procfs-hosttest: /proc/bus/input read '%.*s'\n", len > 0 ? len : 0,
               text.buf ? text.buf : "");
    }
    kfree(text.buf);
    return ok;
}

/**
 * /proc/bus/input/devices gives a record of each published device in the
 * order of its inputN, as Linux's input_devices_seq_show writes it: IDs,
 * name, place, path below /sys, serial, the event node of its one
 * handler, then its properties, event types and the codes of each type it
 * has, ended by a blank line; to an i386 program the masks are in 32-bit
 * words. `handlers` names evdev and its first minor.
 */
static void bus_input_rows(void)
{
    static const char kbd[] = "I: Bus=0011 Vendor=0001 Product=0001 Version=ab41\n"
                              "N: Name=\"AT Translated Set 2 keyboard\"\n"
                              "P: Phys=isa0060/serio0/input0\n"
                              "S: Sysfs=/devices/platform/i8042/serio0/input/input0\n"
                              "U: Uniq=\n"
                              "H: Handlers=event0 \n"
                              "B: PROP=0\n"
                              "B: EV=120013\n";
    static const char mouse[] = "I: Bus=0003 Vendor=0627 Product=0001 Version=0000\n"
                                "N: Name=\"USB HIDBP Mouse 0627:0001\"\n"
                                "P: Phys=usb-1/input0\n"
                                "S: Sysfs=/devices/usb/1-1/1-1:1.0/input/input1\n"
                                "U: Uniq=\n"
                                "H: Handlers=event1 \n"
                                "B: PROP=1\n"
                                "B: EV=7\n";
    char want[2048];

    __builtin_snprintf(want, sizeof(want),
                       "%sB: KEY=402000000 3803078f800d001 feffffdfffefffff fffffffffffffffe\n"
                       "B: MSC=10\nB: LED=7\n\n%sB: KEY=1f0000 0 0 0 0\nB: REL=1943\n\n",
                       kbd, mouse);
    check(bus_input_reads(EVDEV_PROC_DEVICES, want),
          "/proc/bus/input/devices gives each device's record as Linux's input core does");
    __builtin_snprintf(want, sizeof(want),
                       "%sB: KEY=4 2000000 3803078 f800d001 feffffdf ffefffff ffffffff fffffffe\n"
                       "B: MSC=10\nB: LED=7\n\n%sB: KEY=1f0000 0 0 0 0 0 0 0 0\nB: REL=1943\n\n",
                       kbd, mouse);
    compat_caller = 1;
    check(bus_input_reads(EVDEV_PROC_DEVICES, want), "and its masks in 32-bit words to i386");
    compat_caller = 0;
    check(bus_input_reads(EVDEV_PROC_HANDLERS, "N: Number=0 Name=evdev Minor=64\n"),
          "/proc/bus/input/handlers names evdev");
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
    evdev_sysfs_serio_unbound(0, 1);
    check(reg_is(child(in_serio, "uevent"), "SERIO_TYPE=06\nSERIO_PROTO=00\nSERIO_ID=00\n"
                                            "SERIO_EXTRA=00\nMODALIAS=serio:ty06pr00id00ex00\n"),
          "and names no driver while atkbd has left it");
    evdev_sysfs_serio_unbound(0, 0);
    check(reg_is(child(in_iface, "uevent"),
                 "DEVTYPE=usb_interface\nDRIVER=usbhid\nPRODUCT=627/1/0\nTYPE=0/0/0\n"
                 "INTERFACE=3/1/2\nMODALIAS=usb:v0627p0001d0000dc00dsc00dp00ic03isc01ip02in00\n") &&
              reg_is(child(in_iface, "bInterfaceProtocol"), "02\n"),
          "the USB interface gives its class, protocol and modalias");
    bus_input_rows();
}

/**
 * A mouse unplugged sends "remove" for its event node, itself and its
 * interface and leaves nothing of them; plugged again it is input2, since
 * Linux never numbers two input devices alike. Two input devices on one
 * interface each send their "remove" before the interface sends its own.
 * A device with every key has its keys cut from
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
                     "input2");
    check(in_mouse > 0 && child(in_class, "input2") > 0 && !child(in_class, "input1"),
          "plugged again it is input2, a number not used before");
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
    h = child(child(child(sysfs_anchor_find(SYSFS_DEVICES, 0), "virtual"), "input"), "input3");
    check(uevents_sent == sent + 2 && h > 0 &&
              uevent_has(mark, 0, "DEVPATH=/devices/virtual/input/input3") &&
              sent_has(mark, ",+,ramlsfw"),
          "a device with no parent is virtual, and its uevent's modalias is cut to fit");
    check(reg_text(child(h, "modalias"), text, (int)sizeof(text)) > 2048 &&
              __builtin_strstr(text, ",2FE,ramlsfw\n"),
          "while its modalias file names every key");
    input_devs[3] = input_devs[1];
    input_devs[3].gen = 400;
    k_strncpy(input_devs[3].name, "USB HIDBP Mouse Consumer Control", EVDEV_NAME_MAX);
    input_present[3] = 1;
    evdev_sysfs_sync();
    check(
        child(child(child(child(sysfs_anchor_find(SYSFS_USB_DEVICE, 1 << 20), "1-1:1.0"), "input"),
                    "input4"),
              "event3") > 0,
        "a second input device on the interface is input4, below the same interface");
    input_present[1] = input_present[3] = 0;
    usb_mouse_in = 0;
    mark = uevents_len;
    sent = uevents_sent;
    evdev_sysfs_sync();
    check(uevents_sent == sent + 5 &&
              __builtin_strcmp(uevents + mark,
                               "remove@/devices/usb/1-1/1-1:1.0/input/input2/event1") == 0 &&
              uevent_has(mark, 1, "DEVPATH=/devices/usb/1-1/1-1:1.0/input/input2") &&
              uevent_has(mark, 2, "DEVPATH=/devices/usb/1-1/1-1:1.0/input/input4/event3") &&
              uevent_has(mark, 3, "DEVPATH=/devices/usb/1-1/1-1:1.0/input/input4") &&
              uevent_has(mark, 4, "DEVPATH=/devices/usb/1-1/1-1:1.0"),
          "unplugged, each sends remove for its event node and itself, and the interface last");
    check(!child(sysfs_anchor_find(SYSFS_USB_LINKS, 0), "1-1:1.0") && !child(in_class, "input4"),
          "and nothing of them is left");
    input_present[0] = input_present[2] = 0;
    evdev_sysfs_sync();
    check(!child(in_class, "input0") && !child(in_class, "input3") &&
              sysfs_node_count(in_class) == 0 && !child(in_serio, "input"),
          "every device gone, /sys/class/input is empty and the port keeps no input");
}

/* Count each installation of the child port's driver and protocol files. */
static int guest_driver_calls, guest_protocol_calls;

/** The text of a fixture attribute on a pass-through port. */
static int32_t show_guest_file(void *obj, int arg, char *buf, uint32_t cap)
{
    (void)obj;
    (void)arg;
    if (cap >= 3) {
        k_memcpy(buf, "1\n", 3);
    }
    return 2;
}

/** Install a driver's protocol selector on the child port. */
static int add_guest_driver(int dir)
{
    static const struct sysfs_attr attr = {"protocol", 0444u, show_guest_file, NULL};

    guest_driver_calls++;
    return sysfs_add_attr(dir, &attr, NULL, 0) < 0 ? -ENOMEM : 0;
}

/** Install a protocol's setting on the child port. */
static int add_guest_protocol(int dir)
{
    static const struct sysfs_attr attr = {"sensitivity", 0444u, show_guest_file, NULL};

    guest_protocol_calls++;
    return sysfs_add_attr(dir, &attr, NULL, 0) < 0 ? -ENOMEM : 0;
}

/* A protocol can request a whole file table, or one entry too many. */
static int protocol_file_count;

/** Add numbered settings to exercise the real replacement table. */
static int add_numbered_protocol(int dir)
{
    struct sysfs_attr attr = {NULL, 0444u, show_guest_file, NULL};
    char name[16];
    int i;

    for (i = 0; i < protocol_file_count; i++) {
        __builtin_snprintf(name, sizeof(name), "setting%d", i);
        attr.name = name;
        if (sysfs_add_attr(dir, &attr, NULL, 0) < 0) {
            return -ENOMEM;
        }
    }
    return 0;
}

/** Replace the auxiliary port's protocol files through the real registry,
 * preserving the port's other files and removing a failed oversized set. */
static void input_protocol_rows(void)
{
    int dir, old, base, i, all;
    char name[16];

    dir = child(child(sysfs_anchor_find(SYSFS_PLATFORM, 0), "i8042"), "serio1");
    check(dir > 0, "the auxiliary port exists before replacing its protocol files");
    base = sysfs_node_count(dir);
    evdev_sysfs_serio_files(1, add_guest_protocol);
    old = child(dir, "sensitivity");
    check(old > 0 && reg_is(old, "1\n") && sysfs_node_count(dir) == base + 1,
          "the real auxiliary port lists its first protocol's file");
    protocol_file_count = 24;
    evdev_sysfs_serio_files(1, add_numbered_protocol);
    all = 1;
    for (i = 0; i < 24; i++) {
        __builtin_snprintf(name, sizeof(name), "setting%d", i);
        if (!reg_is(child(dir, name), "1\n")) {
            all = 0;
        }
    }
    check(!child(dir, "sensitivity") && !sysfs_node_kind(old) && all &&
              sysfs_node_count(dir) == base + 24,
          "replacement removes the old file and lists all 24 new settings");
    protocol_file_count = 25;
    evdev_sysfs_serio_files(1, add_numbered_protocol);
    check(sysfs_node_count(dir) == base && !child(dir, "setting24"),
          "a rejected oversized set leaves none of its files behind");
    evdev_sysfs_serio_files(1, add_guest_protocol);
    check(reg_is(child(dir, "sensitivity"), "1\n"),
          "a protocol's files can be installed after the oversized set failed");
    evdev_sysfs_serio_files(1, NULL);
    check(sysfs_node_count(dir) == base && child(dir, "description") > 0,
          "removing protocol files preserves the port's own files");
}

/** Make the pass-through port with no device below it, as a pad whose
 * port has no guest has it, through the real registry: a guest bound later
 * goes below it, and it outlives the guest until it is removed. The pad's
 * device, input_devs[0], is published. */
static void input_unbound_port_rows(void)
{
    struct evdev_desc *guest = &input_devs[1];
    int parent, bus, dir;

    parent = child(child(sysfs_anchor_find(SYSFS_PLATFORM, 0), "i8042"), "serio1");
    bus = child(child(sysfs_anchor_find(SYSFS_BUS, 0), "serio"), "devices");
    evdev_sysfs_serio_add(EVDEV_SERIO_PASS_THROUGH);
    dir = child(parent, "serio2");
    check(dir > 0 && child(dir, "description") > 0 &&
              sysfs_node_target(child(bus, "serio2")) == dir,
          "a port asked for without a device is made at once below serio1");
    check(reg_is(child(dir, "bind_mode"), "auto\n") &&
              sysfs_node_mode(child(dir, "bind_mode")) == 0644u,
          "its bind_mode reads auto, mode 0644 as Linux's");
    check(sysfs_node_store(child(dir, "bind_mode"), "manual", 6) == 6 &&
              reg_is(child(dir, "bind_mode"), "manual\n") &&
              evdev_sysfs_serio_manual(EVDEV_SERIO_PASS_THROUGH),
          "and takes manual");
    check(sysfs_node_store(child(dir, "bind_mode"), "manual\n", 7) == -EINVAL &&
              sysfs_node_store(child(dir, "bind_mode"), "x", 1) == -EINVAL,
          "refusing manual with its newline, as strncmp over the bytes written, and x");
    check(sysfs_node_store(child(dir, "bind_mode"), "a", 1) == 1 &&
              !evdev_sysfs_serio_manual(EVDEV_SERIO_PASS_THROUGH) &&
              sysfs_node_store(child(dir, "bind_mode"), "man", 3) == 3 &&
              evdev_sysfs_serio_manual(EVDEV_SERIO_PASS_THROUGH),
          "while a prefix of either word counts");
    guest->gen++;
    input_present[1] = 1;
    evdev_sysfs_serio_files(EVDEV_SERIO_PASS_THROUGH, add_guest_protocol);
    evdev_sysfs_sync();
    check(child(parent, "serio2") == dir && child(in_class, "event1") > 0 &&
              reg_is(child(dir, "sensitivity"), "1\n"),
          "a guest bound later goes below the same port");
    input_present[1] = 0;
    evdev_sysfs_serio_files(EVDEV_SERIO_PASS_THROUGH, NULL);
    evdev_sysfs_sync();
    check(child(parent, "serio2") == dir && !child(in_class, "event1") &&
              !child(dir, "sensitivity"),
          "the port stays when its guest and the guest's files go");
    evdev_sysfs_serio_remove(EVDEV_SERIO_PASS_THROUGH);
    evdev_sysfs_sync();
    check(!child(parent, "serio2") && !child(bus, "serio2"),
          "its removal ends the request, and a sync does not make it again");
    check(!evdev_sysfs_serio_manual(EVDEV_SERIO_PASS_THROUGH),
          "and a port made anew will be in auto mode");
}

/** Publish, remove and recreate a pass-through port through the real
 * sysfs registry, including its input and bus links and its callbacks, and
 * make one with no device below it (`input_unbound_port_rows`). */
static void input_guest_rows(void)
{
    struct evdev_desc *pad = &input_devs[0], *guest = &input_devs[1];
    int parent, dir, bus, file, link, event, mark, sent, installed;
    char text[16];

    k_memset(input_devs, 0, sizeof(input_devs));
    pad->gen = 200;
    pad->id[0] = EVDEV_BUS_I8042;
    k_strncpy(pad->name, "SynPS/2 Synaptics TouchPad", EVDEV_TEXT_MAX);
    k_strncpy(pad->phys, "isa0060/serio1/input0", EVDEV_TEXT_MAX);
    *guest = *pad;
    guest->gen = 201;
    k_strncpy(guest->name, "TPPS/2 IBM TrackPoint", EVDEV_TEXT_MAX);
    k_strncpy(guest->phys, "synaptics-pt/serio0/input0", EVDEV_TEXT_MAX);
    input_present[0] = input_present[1] = 1;
    evdev_sysfs_serio_driver(EVDEV_SERIO_PASS_THROUGH, add_guest_driver);
    evdev_sysfs_serio_files(EVDEV_SERIO_PASS_THROUGH, add_guest_protocol);
    evdev_sysfs_sync();
    parent = child(child(sysfs_anchor_find(SYSFS_PLATFORM, 0), "i8042"), "serio1");
    bus = child(child(sysfs_anchor_find(SYSFS_BUS, 0), "serio"), "devices");
    dir = child(parent, "serio2");
    file = child(dir, "sensitivity");
    link = child(bus, "serio2");
    event = child(in_class, "event1");
    check(dir > 0 && reg_is(child(dir, "protocol"), "1\n") && reg_is(file, "1\n") &&
              sysfs_node_target(link) == dir && event > 0 && guest_driver_calls == 1 &&
              guest_protocol_calls == 1,
          "a pass-through port has its real directories, files and bus link");
    input_present[1] = 0;
    mark = (int)uevents_len;
    sent = uevents_sent;
    evdev_sysfs_serio_remove(EVDEV_SERIO_PASS_THROUGH);
    check(!child(parent, "serio2") && !child(bus, "serio2") && !child(in_class, "event1") &&
              !sysfs_node_kind(event) && !sysfs_node_kind(link) &&
              sysfs_node_show(file, text, sizeof(text)) == -ENOENT,
          "removing the guest withdraws its input links, serio parent, bus link and files");
    check(uevents_sent == sent + 3 && uevent_has((uint32_t)mark, 0, "ACTION=remove") &&
              uevent_has((uint32_t)mark, 1, "ACTION=remove") &&
              uevent_has((uint32_t)mark, 2, "DEVPATH=/devices/platform/i8042/serio1/serio2"),
          "the input event and input device are removed before their serio port");
    evdev_sysfs_sync();
    check(child(in_class, "event0") > 0 && !child(parent, "serio2"),
          "a later sync keeps the pad and does not recreate the absent guest");

    guest->gen++;
    input_present[1] = 1;
    installed = guest_driver_calls + guest_protocol_calls;
    evdev_sysfs_sync();
    dir = child(parent, "serio2");
    check(dir > 0 && !child(dir, "protocol") && !child(dir, "sensitivity") &&
              guest_driver_calls + guest_protocol_calls == installed,
          "recreation without a binding cannot call callbacks of the removed port");
    input_present[1] = 0;
    evdev_sysfs_serio_remove(EVDEV_SERIO_PASS_THROUGH);
    guest->gen++;
    input_present[1] = 1;
    evdev_sysfs_serio_driver(EVDEV_SERIO_PASS_THROUGH, add_guest_driver);
    evdev_sysfs_serio_files(EVDEV_SERIO_PASS_THROUGH, add_guest_protocol);
    evdev_sysfs_sync();
    dir = child(parent, "serio2");
    check(reg_is(child(dir, "protocol"), "1\n") && reg_is(child(dir, "sensitivity"), "1\n") &&
              sysfs_node_target(child(bus, "serio2")) == dir && guest_driver_calls == 2 &&
              guest_protocol_calls == 2,
          "a new binding recreates the child port with its current callbacks and link");
    input_present[1] = 0;
    evdev_sysfs_serio_remove(EVDEV_SERIO_PASS_THROUGH);
    input_unbound_port_rows();
    input_present[0] = 0;
    evdev_sysfs_sync();
}

/* The affinity row's requested and effective masks, and the mask a
 * successful store last gave the PCI layer. */
static uint64_t irq_requested = 15, irq_effective = 8, irq_written;

/** Supply one live message at vector 48 to the procfs affinity rows. */
int pci_irq_affinity(int vector, uint64_t *requested, uint64_t *effective)
{
    if (vector != 48) {
        return -ENOENT;
    }
    *requested = irq_requested;
    *effective = irq_effective;
    return 0;
}

/** Record the mask the actual procfs writer parsed. */
int pci_irq_set_affinity(int vector, uint64_t mask)
{
    if (vector != 48) {
        return -ENOENT;
    }
    irq_written = mask;
    return 0;
}

/** Exercise the real affinity file operations without the other rows. */
static void irq_rows(void)
{
    struct procfs_file file;
    struct open_file desc;
    fd_entry_t entry;
    char buf[32];
    const char *text = "1,00000002\n";
    int32_t n;

    k_memset(&file, 0, sizeof(file));
    k_memset(&desc, 0, sizeof(desc));
    k_memset(&entry, 0, sizeof(entry));
    entry.priv = &file;
    entry.ofd = &desc;
    file.origin_pid = 48;
    n = procfs_irq_write(&entry, (const char __user *)text, 11);
    check(n == 11 && irq_written == 0x100000002ull,
          "a comma begins a new 32-bit word rather than one more digit");
    text = "1,2";
    check(procfs_irq_write(&entry, (const char __user *)text, 3) == 3 &&
              irq_written == 0x100000002ull,
          "a short lower word still has 32-bit place");
    text = "80000000,00000000";
    check(procfs_irq_write(&entry, (const char __user *)text, 17) == 17 &&
              irq_written == ((uint64_t)1 << 63),
          "the highest processor bit is retained");
    text = "0";
    check(procfs_irq_write(&entry, (const char __user *)text, 1) == 1 &&
              irq_written == ~(uint64_t)0,
          "a zero mask restores default affinity");
    text = "1,2,3";
    check(procfs_irq_write(&entry, (const char __user *)text, 5) == -EINVAL,
          "a mask wider than the processor map is refused");
    text = "fffffffff,1";
    check(procfs_irq_write(&entry, (const char __user *)text, 11) == -EINVAL,
          "an oversized high word is refused");
    check(procfs_irq_write(&entry, NULL, 1) == -EFAULT, "a failed user copy changes no affinity");
    file.origin_task = 1;
    check(procfs_irq_write(&entry, (const char __user *)"1", 1) == -EACCES,
          "effective_affinity is read-only");
    n = procfs_irq_read(&entry, (char __user *)buf, sizeof(buf));
    check(n == 2 && __builtin_memcmp(buf, "8\n", 2) == 0,
          "effective_affinity reads the processor actually selected");
    check(procfs_irq_read(&entry, (char __user *)buf, sizeof(buf)) == 0,
          "an affinity read reaches EOF");
    file.origin_task = 0;
    desc.offset = 0;
    irq_requested = 0x100000002ull;
    n = procfs_irq_read(&entry, (char __user *)buf, sizeof(buf));
    check(n == 11 && __builtin_memcmp(buf, "1,00000002\n", 11) == 0,
          "requested affinity prints both hexadecimal words");
    file.origin_pid = 49;
    check(procfs_irq_read(&entry, (char __user *)buf, sizeof(buf)) == -ENOENT,
          "a removed interrupt gives no stale mask");
}

int main(int argc, char **argv)
{
    struct procfs_text text;
    int len;

    process.state = PROC_READY;
    process.tg = &group;
    group.leader = &process;
    group.live = 1;
    k_memcpy(process.comm, "test", 5);
    if (argc == 2 && __builtin_strcmp(argv[1], "--thp-only") == 0) {
        thp_rows();
        printf("procfs-hosttest: thp: %s\n", failures ? "fail" : "pass");
        return failures != 0;
    }
    if (argc == 2 && __builtin_strcmp(argv[1], "--pci-config-only") == 0) {
        pci_config_rows();
        printf("procfs-hosttest: pci config: %s\n", failures ? "fail" : "pass");
        return failures != 0;
    }
    if (argc == 2 && __builtin_strcmp(argv[1], "--irq-only") == 0) {
        irq_rows();
        printf("procfs-hosttest: irq affinity: %s\n", failures ? "fail" : "pass");
        return failures != 0;
    }
    irq_rows();
    procfs_text_init(&text);
    thp_rows();
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
    task_owners();
    syscall_wchan_rows();
    schedstat_rows();
    mem_descriptor();
    exe_links();
    exe_chroot();
    exe_entries();
    image_paths();
    smaps_flags_rows();
    maps_shared_rows();
    maps_names_row();
    maps_anon_identity_rows();
    pid_file_rows();
    system_rows();
    oom_values();
    oom_floor();
    oom_open_errors();
    drm_register_rows();
    drm_link_rows();
    drm_file_rows();
    drm_event_rows();
    drm_ident_rows();
    pci_config_rows();
    pci_inode_rows();
    pci_bar_file_rows();
    pci_bar_mem_rows();
    pci_bar_wc_rows();
    pci_bar_io_rows();
    pci_bar_clip_rows();
    pci_uevent_rows();
    sysfs_reg_rows();
    sysattr_file_rows();
    sysfs_bound_rows();
    input_publish_rows();
    input_file_rows();
    input_change_rows();
    input_guest_rows();
    input_protocol_rows();
    if (!failures) {
        printf("procfs-hosttest: result: pass: stat allocation failure and times, descriptor "
               "links, task ids, the mem descriptor's counts, the exe link, a read of a reaped "
               "process's arguments, stat and status, autogroup writes, oom_score_adj, the maps' "
               "paths, the uptime idle sum, an interface's counts, a DRM device's /sys entries and "
               "uevents, a driver's attribute, link and PCI identity, a PCI function's BARs mapped "
               "and read through its resource files, writes of a PCI function's uevent, an input "
               "device's /sys entries, capability masks, uevents and /proc/bus/input record, and "
               "the syscall and wchan files of blocked, stopped and running threads\n");
    }
    return failures != 0;
}

/** No sound card on the model: /proc/asound's files have no names. */
int asound_name(int file, char *name, int cap)
{
    (void)file;
    (void)name;
    (void)cap;
    return -1;
}
