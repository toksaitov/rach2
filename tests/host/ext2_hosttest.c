/*
 * ext2_hosttest.c: the ext2 driver, rach2/sys/fs/ext2.c, over the buffer
 * cache, on volumes the host's mke2fs makes.
 *
 * host-tools: setidcheck mke2fs debugfs dumpe2fs e2fsck
 * host-deps: tests/host/compat/vfs_name_none.c
 * host-deps: tests/host/compat/ext2_fixture.c tests/host/compat/command_host.c
 * host-deps: tools/etude/capture.c tools/etude/posix/exec.c
 * host-deps: tools/etude/posix/keeper.c tools/etude/posix/process.c
 * host-cflags: -I tools/etude/include
 * host-deps: rach2/sys/fs/ext2xattr.c
 * host-deps: rach2/sys/fs/ext2_alloc.c rach2/sys/fs/ext2_batch.c
 * host-deps: rach2/sys/fs/ext2_check.c rach2/sys/fs/ext2_dir.c
 * host-deps: rach2/sys/fs/ext2_file.c
 * host-deps: rach2/sys/fs/ext2.c rach2/sys/fs/bcache.c rach2/sys/fs/fatcommon.c
 * host-deps: rach2/sys/block/chain.c rach2/sys/fs/fd.c rach2/sys/fs/lock.c
 * host-deps: rach2/sys/fs/sleeplock.c tests/host/compat/park_host.c
 * host-deps: tests/host/compat/spin_none.c
 * host-deps: rach2/sys/lib/kstring.c rach2/sys/lib/kctypes.c
 * host-deps: tests/host/compat/kheap_host.c tests/host/compat/frames_host.c
 * host-deps: tests/host/compat/pressure_host.c tests/host/compat/fixdir_host.c
 * host-deps: tests/host/compat/spinlock_host.c tests/host/compat/kirq_none.c
 * host-deps: tests/host/compat/bkl_held.c
 * host-deps: tests/host/compat/mutex_none.c tests/host/compat/kirq_on.c
 * host-deps: tests/host/compat/atomic_host.c tests/host/compat/fsize_none.c
 * host-first: its interruption matrices run for minutes
 *
 * The volumes are made here from a staged tree by the host's mke2fs with the
 * features tools/image/mkext2.sh gives, and debugfs adds what a host tree
 * does not have: device nodes in both encodings, an owner and a set-user-ID
 * mode, and, for one row, a size past 4 GiB, with which the file must still
 * stat and open. Every file reads back against the tree, and every node
 * against setidcheck's reader, which shares no code with this driver. A
 * 1 KiB volume has a sparse file with a byte behind each level of
 * indirection and mounts read-only by name; an unknown incompatible feature
 * fails the mount. The tree, the volumes,
 * prepared setidcheck and what the independent tools print are kept in a
 * directory of the test's own (compat/fixdir_host.c). Native C creates the
 * fixtures and controls every command, so concurrent runs share no file.
 * The test includes no hosted header: the kernel headers define the
 * fixed-width types, and the C library calls it makes are declared below.
 */
#include "../../rach2/sys/fs/ext2.h"
#include "../../rach2/sys/fs/ext2xattr.h"
#include "../../rach2/sys/fs/bcache.h"
#include "../../rach2/sys/fs/sleeplock.h"
#include "../../rach2/sys/lib/kerrno.h"
#include "../../rach2/sys/proc/process.h"
#include "fixdir_host.h"
#include "command_host.h"
#include "ext2_fixture.h"

extern int printf(const char *fmt, ...);
extern int snprintf(char *s, unsigned long n, const char *fmt, ...);
extern int symlink(const char *target, const char *path);
extern int unlink(const char *path);
extern int link(const char *oldpath, const char *newpath);
extern int mkdir(const char *path, unsigned mode);
extern void exit(int status);
extern void *fopen(const char *path, const char *mode);
extern int fseek(void *f, long off, int whence);
extern unsigned long fread(void *buf, unsigned long size, unsigned long n, void *f);
extern unsigned long fwrite(const void *buf, unsigned long size, unsigned long n, void *f);
extern int fclose(void *f);
extern int fflush(void *f);
extern char *fgets(char *s, int n, void *f);
extern char *strstr(const char *s, const char *needle);
extern unsigned long strlen(const char *s);
extern long atol(const char *s);
extern long strtol(const char *s, char **end, int base);
extern unsigned long strtoul(const char *s, char **end, int base);
extern int strcmp(const char *a, const char *b);
extern int setenv(const char *name, const char *value, int overwrite);
extern int memcmp(const void *a, const void *b, unsigned long n);
extern void *memset(void *s, int c, unsigned long n);
extern void *malloc(unsigned long n);
extern void free(void *p);

/* compat/park_host.c: a second process's call, run inside a sleep of the
   first, on a stack of its own. */
extern int park_run(void (*fn)(void));
extern int park_resume(void);
extern void park(void);

#define SEEK_SET_ 0

/* ---- stubs for what the driver and the cache reach outside themselves ---- */

static struct vfs_mount mnt;
static void (*flush_pause)(void);
static void (*read_pause)(void);
static void (*write_pause)(void);
/* What another process does at a service point, while a row sets it. */
static void (*service_pause)(void);

process_t proc_a;
/* The second process of a row in which two threads take turns. */
static process_t proc_b = {.pid = 2};
struct percpu cpu_table[NR_CPUS] = {{.current = &proc_a}};
/* The one thread, which the descriptor table's growth walks. */
process_t *proc_list = &proc_a;

/** Print the panic's message and exit with status 3. */
void k_panic(const char *s)
{
    printf("ext2_hosttest: panic: %s\n", s);
    exit(3);
}

/* The last record, and the last one at warning level, which is where the
   driver puts a refusal or a read-only reason. */
static char last_log[160];
/* The frames the machine has free, which pressure_host.c answers with. */
extern uint32_t host_free_frames;
static char last_warn[160];
/* The last record of the mount's check, which precedes a read-only reason. */
static char last_check[160];
extern int vsnprintf(char *s, unsigned long n, const char *fmt, __builtin_va_list ap);

/**
 * Keep the record in `last_log`; a record at warning level (4) also in
 * `last_warn`, and a record of the mount's check, which begins "check: ",
 * also in `last_check`.
 */
void klog(int level, const char *sys, const char *fmt, ...)
{
    __builtin_va_list ap;

    (void)sys;
    __builtin_va_start(ap, fmt);
    vsnprintf(last_log, sizeof(last_log), fmt, ap);
    __builtin_va_end(ap);
    if (level == 4) {
        __builtin_memcpy(last_warn, last_log, sizeof(last_warn));
    }
    if (memcmp(last_log, "check: ", 7) == 0) {
        __builtin_memcpy(last_check, last_log, sizeof(last_check));
    }
}

/** Clear the queue. */
void wait_queue_init(wait_queue_t *wq)
{
    __builtin_memset(wq, 0, sizeof(*wq));
}
/** Do nothing: a sleeper looks at its condition at each turn it is given. */
void wq_wake_all(wait_queue_t *wq)
{
    (void)wq;
}

/** No sleeper is on any queue: no other process runs here. */
int wq_active(const wait_queue_t *wq)
{
    (void)wq;
    return 0;
}

/** The wake of a waker that holds a spinlock, which, as `wq_wake_all`
 *  here, has nobody to wake. */
void wq_wake_all_deferred(wait_queue_t *wq, uint64_t *cpus)
{
    (void)wq;
    (void)cpus;
}

/** Interrupt no processor: the test runs on one. */
void sched_kick_cpus(uint64_t cpus)
{
    (void)cpus;
}

/* The waits that slept, and how many turns one may give before the row is a
   deadlock, which would otherwise spin. */
static int sleeps;
#define WAIT_TURNS 100000

/* The kind of the first wait process B slept in since `start_turns`, or
   NULL while it has slept in none, and for a buffer's, its block. */
static const char *second_wait;
static uint64_t second_wait_block;
/* The blocks of the cache the last request of the device moved, the first
   and the count. */
static uint64_t request_block, request_blocks;

/**
 * The kind of the wait on `wq` with argument `arg`: a sleeping lock's kind
 * (sleeplock.h), whose wait passes a `struct sleep_turn` that begins with
 * the lock; "buffer" for the hold of a cache buffer, whose wait passes the
 * buffer; "ext2 node state" for anything else, which in ext2.c is a node's
 * load or death, or the admission of a remount. Each argument here is a
 * structure of at least a pointer. A buffer's block goes in `*block`.
 */
static const char *wait_kind(wait_queue_t *wq, void *arg, uint64_t *block)
{
    const struct sleep_lock *l = arg ? *(struct sleep_lock *const *)arg : 0;

    if (l && (uintptr_t)l + __builtin_offsetof(struct sleep_lock, wq) == (uintptr_t)wq) {
        return l->record ? l->record->kind : "sleep lock";
    }
    if (arg && (uintptr_t)arg + __builtin_offsetof(struct bbuf, wq) == (uintptr_t)wq) {
        *block = ((const struct bbuf *)arg)->blk;
        return "buffer";
    }
    return "ext2 node state";
}

/* A row of two processes: process A is the test's own stack and process B
   a parked call (compat/park_host.c), which `start_turns` arms and A's first
   turn starts. Only one runs, and each runs until it gives the turn. */
static void (*second_body)(void);
static int second_armed, second_started, second_inside;

/** Give the other process of a row the turn, and be this one again when it
 * comes back or process B has ended. */
static void pass_turn(void)
{
    process_t *me = current_proc;
    int ended;

    if (second_inside) {
        park();
    } else if (second_armed) {
        current_proc = &proc_b;
        second_inside = 1;
        ended = second_started ? park_resume() : park_run(second_body);
        second_inside = 0;
        second_started = 1;
        second_armed = !ended;
    }
    current_proc = me;
}

/** Whether a row has a process B that has not ended. */
static int turns_active(void)
{
    return second_armed;
}

/** Arm `body` as process B, which runs at the first turn it is given. */
static void start_turns(void (*body)(void))
{
    second_body = body;
    second_armed = 1;
    second_started = 0;
    second_wait = 0;
}

/** Give turns until process B has ended, starting it if none was given. */
static void finish_turns(void)
{
    while (second_armed) {
        pass_turn();
    }
}

/**
 * Return -1 at once, as an interrupted wait does, when no other process
 * runs; in a row of two, give turns until `cond` holds, and panic when it
 * never does.
 */
int wq_wait_event(wait_queue_t *wq, wq_cond_fn cond, void *arg)
{
    int turns = 0;

    if (!turns_active()) {
        return -1;
    }
    if (!cond(arg)) {
        sleeps++;
        if (current_proc == &proc_b && !second_wait) {
            second_wait = wait_kind(wq, arg, &second_wait_block);
        }
    }
    while (!cond(arg)) {
        if (++turns > WAIT_TURNS || !turns_active()) {
            k_panic("a wait that nothing ends");
        }
        pass_turn();
    }
    return 0;
}

/* The bytes of a user buffer a row has made absent, [gone_from, gone_to),
   which `vm_resident` calls not present until `vm_prefault_range` brings
   them in, and the prefaults asked for. Empty, every buffer is present. */
static uintptr_t gone_from, gone_to;
static int prefaults;

/** Whether [addr, addr + len) misses the absent bytes. */
int vm_resident(uintptr_t addr, uintptr_t len, int write)
{
    (void)write;
    return addr >= gone_to || addr + len <= gone_from;
}

/** Count the prefault and bring the absent bytes in when the range meets
 * them; always 1. */
int vm_prefault_range(uintptr_t addr, uintptr_t len, int write)
{
    (void)write;
    prefaults++;
    if (addr < gone_to && addr + len > gone_from) {
        gone_from = gone_to = 0;
    }
    return 1;
}

/** Return 0: the caller never holds the namespace lock here. */
int vfs_ns_held_by_caller(void)
{
    return 0;
}

/* The write-back errors recorded for the VFS's syncs to report, and the
 * last of them (`vfs_wb_err_set`). */
static int wb_err_sets, wb_err_last;

/** Note a write-back error recorded for the descriptions' syncs, which the
 * VFS reports and these rows read. */
void vfs_wb_err_set(struct vfs_mount *m, int rc)
{
    (void)m;
    wb_err_sets++;
    wb_err_last = rc;
}

/**
 * Record a failed write on the mounted device as the VFS does: keep the
 * first error in `write_error`, record this one for the syncs to report,
 * and make the mount read-only.
 */
void vfs_device_failed(block_device_t *d, uint64_t lba, int rc)
{
    (void)lba;
    if (mnt.priv && mnt.dev == d) {
        if (!mnt.write_error) {
            mnt.write_error = rc;
        }
        vfs_wb_err_set(&mnt, rc);
        mnt.flags |= VFS_MS_RDONLY;
    }
}
/** As `wq_wait_event`, and a panic for a wait no other process can end. */
int wq_wait_event_nointr(wait_queue_t *wq, wq_cond_fn cond, void *arg)
{
    if (!turns_active() && !cond(arg)) {
        k_panic("a wait with no other process");
    }
    return wq_wait_event(wq, cond, arg);
}
/** Return 0: no hold of a sleeping lock here is timed. */
uint32_t timer_now_us(void)
{
    return 0;
}

/** The cycle counter: the clock's microseconds, a cycle each. */
uint64_t arch_cycles(void)
{
    return timer_now_us();
}

/** A cycle is a microsecond here. */
uint64_t timer_cycles_us(uint64_t cycles)
{
    return cycles;
}

/** The cycles here are the clock's microseconds, the same on every
 * processor. */
int timer_cycles_agree(void)
{
    return 1;
}

/* The clock of a deadline, in ticks. */
static uint64_t host_ticks;

/** Run `service_pause`, which stands for what runs when the big lock is let
 * go at a service point. */
void sched_service_point(void)
{
    if (service_pause) {
        service_pause();
    }
}

/** The service point, as the kernel's with interrupts on: the code under
 * test runs with them on here. */
void sched_give_way(void)
{
    sched_service_point();
}

/** The tick count, one more at each look, so that a wait with a deadline
 * ends after a bounded number of turns. */
uint64_t tick_count(void)
{
    return ++host_ticks;
}
/** Panic: fd.c takes the mount's namespace lock only on a type that sets
 * `ns_serialised`, which ext2 does not. */
void vfs_ns_lock(struct vfs_mount *mnt)
{
    (void)mnt;
    k_panic("the namespace lock taken on ext2");
}
/** Panic, as `vfs_ns_lock` does. */
void vfs_ns_unlock(struct vfs_mount *mnt)
{
    (void)mnt;
    k_panic("the namespace lock given back on ext2");
}

/* ---- the image file as a block device ---- */

static void *image;
static uint64_t image_sectors;
/* The volume in memory in place of the image file, while a row sets it
   (ext2_pair_rows.h), so that a row can start again from a copy cheaply. */
static unsigned char *ram_image;
/* Reads to fail, for a row: each one reads nothing. */
static int read_fails;

/** Read `count` sectors of the image file at `lba`; 0, or -5 (EIO). */
static int img_read(block_device_t *d, uint64_t lba, uint32_t count, void *buf)
{
    (void)d;
    current_proc->disk_waits++; /* the wait the block layer counts */
    request_block = lba / (BCACHE_BLOCK_BYTES / 512);
    request_blocks = (count + BCACHE_BLOCK_BYTES / 512 - 1) / (BCACHE_BLOCK_BYTES / 512);
    if (read_pause) {
        read_pause();
    }
    if (read_fails > 0) {
        read_fails--;
        return -5;
    }
    if (lba + count > image_sectors) {
        return -5;
    }
    if (ram_image) {
        __builtin_memcpy(buf, ram_image + lba * 512, count * 512);
        return 0;
    }
    if (fseek(image, (long)(lba * 512), SEEK_SET_) != 0 ||
        fread(buf, 1, count * 512, image) != count * 512) {
        return -5;
    }
    return 0;
}

/* Writes land in the image file. `stop_after` is the interruption matrix's
   crash: the writes after that many fail without touching the image, which
   leaves it as a machine stopped after the Nth write would. */
static int writes_made;
static int write_attempts; /* writes asked of the device, refused ones too */
static int stop_after = -1;
/* The one block whose writes fail, or -1: a request that covers it fails and
 * every other goes through, unlike `stop_after`'s stop. */
static int64_t fail_block = -1;

/* The writes in the order the device saw them, for the rows that read that
   order: each one's first sector and its length, and, while `wlog_capture`
   is set, a copy of its bytes, so the disk can be replayed write by write.
   A log that overflows says so, since a row judging an order it did not see
   whole would judge nothing. */
#define WLOG_MAX 4096
static uint64_t wlog_lba[WLOG_MAX];
static uint32_t wlog_count[WLOG_MAX];
static unsigned char *wlog_data[WLOG_MAX];
static int wlog_n, wlog_capture, wlog_overflow;
static int flush_calls, flush_fail_at = -1;
static int flush_log[WLOG_MAX], flush_n;
/* Called with each write that lands, before it lands, while a row sets it. */
static void (*write_watch)(uint64_t lba, uint32_t count, const void *buf);

/** The device's request limit, or `BLOCK_REQ_SECTORS` when it sets none. */
uint32_t block_max_sectors(block_device_t *device)
{
    return device->max_sectors ? device->max_sectors : BLOCK_REQ_SECTORS;
}

/**
 * Flush the device. Call `flush_pause` first when a row set it; fail with
 * `-EIO` the flush numbered `flush_fail_at`; otherwise, while writes are
 * captured, record in `flush_log` how many writes the flush follows. A full
 * `flush_log` sets `wlog_overflow` and fails.
 */
int block_flush(block_device_t *device)
{
    (void)device;
    if (flush_pause) {
        flush_pause();
    }
    flush_calls++;
    if (flush_calls == flush_fail_at) {
        return -EIO;
    }
    if (wlog_capture) {
        if (flush_n == WLOG_MAX) {
            wlog_overflow = 1;
            return -EIO;
        }
        flush_log[flush_n++] = wlog_n;
    }
    return 0;
}

/** The device's flags. */
uint32_t block_flags(block_device_t *device)
{
    return device->flags;
}

/**
 * Do a request with flags. The device keeps no cache of its own; one marked
 * `BLOCK_WRITE_THROUGH` records a captured write asked for without
 * `BLOCK_REQ_NOFLUSH` as the write and its flush, as a disk that writes
 * through makes it.
 */
int block_request_flags(block_device_t *device, int write, uint64_t lba, uint32_t count,
                        void *buffer, uint32_t flags)
{
    int rc = block_request(device, write, lba, count, buffer);

    if (rc == 0 && write && (device->flags & BLOCK_WRITE_THROUGH) && !(flags & BLOCK_REQ_NOFLUSH) &&
        wlog_capture) {
        if (flush_n == WLOG_MAX) {
            wlog_overflow = 1;
            return -EIO;
        }
        flush_log[flush_n++] = wlog_n;
    }
    return rc;
}

/** Free the captured bytes and empty the write and flush logs. */
static void wlog_reset(void)
{
    int i;

    for (i = 0; i < wlog_n && i < WLOG_MAX; i++) {
        free(wlog_data[i]);
        wlog_data[i] = 0;
    }
    wlog_n = 0;
    wlog_overflow = 0;
    flush_n = 0;
}

/**
 * Write `count` sectors to the image file at `lba` and log the write, with a
 * copy of its bytes while `wlog_capture` is set. Once `stop_after` writes
 * are made, every later write fails and leaves the image as it is. Returns
 * 0, or -5 (EIO).
 */
static int img_write(block_device_t *d, uint64_t lba, uint32_t count, const void *buf)
{
    (void)d;
    write_attempts++;
    current_proc->disk_waits++;
    request_block = lba / (BCACHE_BLOCK_BYTES / 512);
    request_blocks = (count + BCACHE_BLOCK_BYTES / 512 - 1) / (BCACHE_BLOCK_BYTES / 512);
    if (write_pause) {
        write_pause();
    }
    if (lba + count > image_sectors) {
        return -5;
    }
    if (stop_after >= 0 && writes_made >= stop_after) {
        return -5;
    }
    if (fail_block >= 0 && (uint64_t)fail_block >= request_block &&
        (uint64_t)fail_block < request_block + request_blocks) {
        return -5;
    }
    if (write_watch) {
        write_watch(lba, count, buf);
    }
    writes_made++;
    if (wlog_n < WLOG_MAX) {
        wlog_lba[wlog_n] = lba;
        wlog_count[wlog_n] = count;
        wlog_data[wlog_n] = 0;
        if (wlog_capture) {
            wlog_data[wlog_n] = malloc(count * 512);
            if (wlog_data[wlog_n]) {
                __builtin_memcpy(wlog_data[wlog_n], buf, count * 512);
            }
        }
        wlog_n++;
    } else {
        wlog_overflow = 1;
    }
    if (ram_image) {
        __builtin_memcpy(ram_image + lba * 512, buf, count * 512);
        return 0;
    }
    if (fseek(image, (long)(lba * 512), SEEK_SET_) != 0 ||
        fwrite(buf, 1, count * 512, image) != count * 512) {
        return -5;
    }
    return 0;
}

static int64_t clock_seconds = 1000000000;
static int64_t clock_nanos;

/** The time selected by the row, 10^9 seconds after the epoch by default. */
void rtc_realtime(int64_t *seconds, int64_t *nanoseconds)
{
    *seconds = clock_seconds;
    *nanoseconds = clock_nanos;
}

static block_device_t dev = {512, 0, BLOCK_FAULT_SAFE, 0, 0, 0, img_read, img_write, 0, 0, 0};

/** Call the device's read or write. */
int block_request(block_device_t *device, int write, uint64_t lba, uint32_t count, void *data)
{
    if (write) {
        return device->write(device, lba, count, data);
    }
    return device->read(device, lba, count, data);
}

/* ---- the rows ---- */

static int fails;

/** Count and print a failed row. */
static void check(int ok, const char *what)
{
    if (!ok) {
        fails++;
        printf("FAIL: %s\n", what);
    }
}

/** Whether the two strings are equal. */
static int streq(const char *a, const char *b)
{
    return strcmp(a, b) == 0;
}

/**
 * Make the image at `path`, of `bytes` bytes, the device. The image open
 * before is synced through the cache and closed, and the cache is emptied.
 * Exits the test when the image does not open or the cache does not empty.
 */
static void use_image(const char *path, uint64_t bytes)
{
    if (image) {
        bcache_sync(&dev);
        fclose(image);
    }
    image = fopen(path, "r+b");
    if (!image) {
        printf("ext2_hosttest: %s did not open\n", path);
        exit(1);
    }
    image_sectors = bytes / 512;
    dev.sector_count = image_sectors;
    if (bcache_invalidate(&dev) != 0) {
        printf("ext2_hosttest: the cache did not empty for a new image\n");
        exit(1);
    }
}

/* The driver's own operations, and those the rows call: the same, but for
   a change of names, which runs under its directory's lock as the VFS calls
   it (`held_ops_init`). */
#define EXT2_OPS (vfs_ext2_type.ops)
#define OPS      (&held_ops)

static struct vfs_ops held_ops;

/** The directory `dir` names, the root for NULL. */
static void *held_dir(struct vfs_mount *m, void *dir)
{
    return dir ? dir : m->root;
}

/** Whether directory `a` is `b` or below it, by the `..` entries from `a`
 * up to the root; the walk holds no lock while it looks. */
static int held_below(struct vfs_mount *m, void *a, void *b)
{
    void *cur = a, *up;
    int depth;

    EXT2_OPS->hold(m, cur);
    for (depth = 0; depth < 256; depth++) {
        int at = cur == b, top = cur == m->root;

        if (at || top || EXT2_OPS->lookup(m, cur, "..", &up) != 0) {
            EXT2_OPS->put(m, cur);
            return at;
        }
        EXT2_OPS->put(m, cur);
        cur = up;
    }
    EXT2_OPS->put(m, cur);
    return 0;
}

/** The inode number of `node`, 0 when getattr fails. */
static uint64_t held_ino(struct vfs_mount *m, void *node)
{
    struct vfs_stat st;

    return EXT2_OPS->getattr(m, node, &st) == 0 ? st.ino : 0;
}

/** The `create` of `held_ops`: the driver's, under `dir`'s lock. */
static int held_create(struct vfs_mount *m, void *dir, const char *name, uint32_t mode, void **node)
{
    void *d = held_dir(m, dir);
    int rc;

    EXT2_OPS->lock_node(m, d);
    rc = EXT2_OPS->create(m, dir, name, mode, node);
    EXT2_OPS->unlock_node(m, d);
    return rc;
}

/** The `unlink` of `held_ops`: the driver's, under `dir`'s lock. */
static int held_unlink(struct vfs_mount *m, void *dir, const char *name)
{
    void *d = held_dir(m, dir);
    int rc;

    EXT2_OPS->lock_node(m, d);
    rc = EXT2_OPS->unlink(m, dir, name);
    EXT2_OPS->unlock_node(m, d);
    return rc;
}

/** The `mkdir` of `held_ops`: the driver's, under `dir`'s lock. */
static int held_mkdir(struct vfs_mount *m, void *dir, const char *name, uint32_t mode)
{
    void *d = held_dir(m, dir);
    int rc;

    EXT2_OPS->lock_node(m, d);
    rc = EXT2_OPS->mkdir(m, dir, name, mode);
    EXT2_OPS->unlock_node(m, d);
    return rc;
}

/** The `rmdir` of `held_ops`: the driver's, under `dir`'s lock. */
static int held_rmdir(struct vfs_mount *m, void *dir, const char *name)
{
    void *d = held_dir(m, dir);
    int rc;

    EXT2_OPS->lock_node(m, d);
    rc = EXT2_OPS->rmdir(m, dir, name);
    EXT2_OPS->unlock_node(m, d);
    return rc;
}

/**
 * The `rename` of `held_ops`: the driver's, under both directories' locks,
 * taken as `vfs_rename_lock` takes them: the mount's rename lock first when
 * they differ, then the ancestor before its descendant, and unrelated ones
 * by inode number.
 */
static int held_rename(struct vfs_mount *m, void *olddir, const char *oldname, void *newdir,
                       const char *newname)
{
    void *od = held_dir(m, olddir), *nd = held_dir(m, newdir), *first = od, *second = nd;
    int rc;

    if (od != nd) {
        sleep_lock_take(&m->rename);
        if (held_below(m, od, nd) ||
            (!held_below(m, nd, od) && held_ino(m, nd) < held_ino(m, od))) {
            first = nd;
            second = od;
        }
    }
    EXT2_OPS->lock_node(m, first);
    if (second != first) {
        EXT2_OPS->lock_node(m, second);
    }
    rc = EXT2_OPS->rename(m, olddir, oldname, newdir, newname);
    if (second != first) {
        EXT2_OPS->unlock_node(m, second);
    }
    EXT2_OPS->unlock_node(m, first);
    if (od != nd) {
        sleep_lock_give(&m->rename);
    }
    return rc;
}

/** The `link` of `held_ops`: the driver's, under `dir`'s lock and then the
 * node's. */
static int held_link(struct vfs_mount *m, void *dir, const char *name, void *node)
{
    void *d = held_dir(m, dir);
    int rc;

    EXT2_OPS->lock_node(m, d);
    EXT2_OPS->lock_node(m, node);
    rc = EXT2_OPS->link(m, dir, name, node);
    EXT2_OPS->unlock_node(m, node);
    EXT2_OPS->unlock_node(m, d);
    return rc;
}

/** The `symlink` of `held_ops`: the driver's, under `dir`'s lock. */
static int held_symlink(struct vfs_mount *m, void *dir, const char *name, const char *target)
{
    void *d = held_dir(m, dir);
    int rc;

    EXT2_OPS->lock_node(m, d);
    rc = EXT2_OPS->symlink(m, dir, name, target);
    EXT2_OPS->unlock_node(m, d);
    return rc;
}

/** The `mknod` of `held_ops`: the driver's, under `dir`'s lock. */
static int held_mknod(struct vfs_mount *m, void *dir, const char *name, uint32_t mode,
                      uint32_t rdev, void **node)
{
    void *d = held_dir(m, dir);
    int rc;

    EXT2_OPS->lock_node(m, d);
    rc = EXT2_OPS->mknod(m, dir, name, mode, rdev, node);
    EXT2_OPS->unlock_node(m, d);
    return rc;
}

/** Make `held_ops` the driver's operations with the namespace calls
 * wrapped, before the first row. */
static void held_ops_init(void)
{
    held_ops = *EXT2_OPS;
    held_ops.create = held_create;
    held_ops.unlink = held_unlink;
    held_ops.mkdir = held_mkdir;
    held_ops.rmdir = held_rmdir;
    held_ops.rename = held_rename;
    held_ops.link = held_link;
    held_ops.symlink = held_symlink;
    held_ops.mknod = held_mknod;
}

/* The kind of the mount's rename lock, as nodelock.c names it. */
static struct sleep_lock_record rename_record = SLEEP_LOCK_RECORD("rename");

/** Mount the device with `flags` on a cleared `mnt`, its rename lock free;
 * the mount's result. */
static int mount_flags(uint32_t flags)
{
    int rc;

    __builtin_memset(&mnt, 0, sizeof(mnt));
    mnt.type = &vfs_ext2_type;
    mnt.dev = &dev;
    mnt.flags = flags;
    mnt.ns.holder = -1;
    sleep_lock_init(&mnt.rename, &rename_record, 0);
    rc = vfs_ext2_type.mount(&mnt, &dev, flags, "");
    return rc;
}

/** Mount the device with no flags; the driver may make it read-only. */
static int mount_image(void)
{
    return mount_flags(0);
}

/** Mount read-only: no check, nothing written, the crash state as it is. */
static int mount_ro(void)
{
    return mount_flags(VFS_MS_RDONLY);
}

/** Drop the root and unmount, when a mount is up. */
static void unmount_image(void)
{
    if (mnt.root) {
        OPS->put(&mnt, mnt.root);
        OPS->unmount(&mnt);
        mnt.root = 0;
    }
}

/**
 * Resolve `path` under the root by a lookup per component. Returns the node,
 * held, which the caller puts; or NULL with the lookup's error in `*err`.
 */
static void *walk(const char *path, int *err)
{
    void *cur = mnt.root;
    char comp[256];

    OPS->hold(&mnt, cur);
    *err = 0;
    while (*path) {
        int n = 0;
        void *next;
        int rc;

        while (*path == '/') {
            path++;
        }
        if (!*path) {
            break;
        }
        while (*path && *path != '/') {
            comp[n++] = *path++;
        }
        comp[n] = '\0';
        rc = OPS->lookup(&mnt, cur, comp, &next);
        OPS->put(&mnt, cur);
        if (rc != 0) {
            *err = rc;
            return 0;
        }
        cur = next;
    }
    return cur;
}

/** Read up to `cap` bytes of a file from `offset`; the count, or an error. */
static int read_at(void *node, unsigned char *buf, uint32_t cap, uint64_t offset)
{
    void *h;
    int32_t got, total = 0;
    int rc = OPS->open(&mnt, node, 0, &h);

    if (rc != 0) {
        return rc;
    }
    for (;;) {
        got = OPS->read(&mnt, h, buf + total, cap - (uint32_t)total, offset + (uint64_t)total);
        if (got < 0) {
            OPS->release(&mnt, h);
            return got;
        }
        if (got == 0 || (uint32_t)total == cap) {
            break;
        }
        total += got;
    }
    OPS->release(&mnt, h);
    return total;
}

/** Read up to `cap` bytes of a host file at `offset`; the count, or -1. */
static long host_read(const char *path, unsigned char *buf, unsigned long cap, long offset)
{
    void *f = fopen(path, "rb");
    unsigned long got;

    if (!f) {
        return -1;
    }
    if (fseek(f, offset, SEEK_SET_) != 0) {
        fclose(f);
        return -1;
    }
    got = fread(buf, 1, cap, f);
    fclose(f);
    return (long)got;
}

static unsigned char abuf[1 << 20];
static unsigned char bbuf[1 << 20];

/** Check a file in the image against the same file in the tree, whole. */
static void same_as_tree(const char *guest, const char *what)
{
    int err;
    void *n = walk(guest, &err);
    char host[512];
    long hgot;
    int igot;

    if (!n) {
        printf("FAIL: %s: %s is not in the image (%d)\n", what, guest, err);
        fails++;
        return;
    }
    fix_format(host, sizeof(host), "%s/tree%s", fix_dir(), guest);
    hgot = host_read(host, bbuf, sizeof(bbuf), 0);
    igot = read_at(n, abuf, sizeof(abuf), 0);
    OPS->put(&mnt, n);
    if (hgot < 0 || igot < 0 || hgot != igot || memcmp(abuf, bbuf, (unsigned long)igot) != 0) {
        printf("FAIL: %s: %s reads %d bytes, the tree has %ld\n", what, guest, igot, hgot);
        fails++;
    }
}

/**
 * Check every node setidcheck's reader lists in `img` against the driver's
 * view of it: the mode, owner, group, size and inode number.
 */
static void against_reader(const char *img)
{
    char line[1024];
    const char *argv[] = {host_tool_path("setidcheck"), "--nodes", "--ext2", img, 0};
    void *f;
    int listed = 0, agreed = 0;

    if (host_command(argv, fix_path("nodes.txt"), 0, 0, 0) != 0) {
        check(0, "setidcheck lists the image's nodes");
        return;
    }
    f = fopen(fix_path("nodes.txt"), "r");
    if (!f) {
        check(0, "the node list opens");
        return;
    }
    while (fgets(line, sizeof(line), f)) {
        char path[512];
        unsigned long mode, uid, gid, size, ino;
        char *p = line;
        int i = 0;
        int err;
        void *n;
        struct vfs_stat st;

        while (*p && *p != ' ' && i < 511) {
            path[i++] = *p++;
        }
        path[i] = '\0';
        mode = strtoul(p, &p, 8);
        uid = strtoul(p, &p, 10);
        gid = strtoul(p, &p, 10);
        size = strtoul(p, &p, 10);
        ino = strtoul(p, &p, 10);
        listed++;
        n = walk(path, &err);
        if (!n) {
            printf("FAIL: the reader lists %s and the driver cannot find it (%d)\n", path, err);
            fails++;
            continue;
        }
        if (OPS->getattr(&mnt, n, &st) == 0 && st.mode == mode && st.uid == uid && st.gid == gid &&
            st.size == size && st.ino == ino) {
            agreed++;
        } else {
            printf(
                "FAIL: %s: the driver says %lo %lu %lu %lu %lu, the reader %lo %lu %lu %lu %lu\n",
                path, (unsigned long)st.mode, (unsigned long)st.uid, (unsigned long)st.gid,
                (unsigned long)st.size, (unsigned long)st.ino, mode, uid, gid, size, ino);
            fails++;
        }
        OPS->put(&mnt, n);
    }
    fclose(f);
    check(listed > 0 && agreed == listed, "every node the reader lists agrees with the driver");
}

#define PROFILE                                                                        \
    "sparse_super,large_file,filetype,^dir_index,resize_inode,^ext_attr,^has_journal," \
    "^extent,^64bit,^flex_bg,^metadata_csum,^huge_file,^dir_nlink,^extra_isize"

/* PROFILE with dir_index, which Linux's ext2 mounts writable. */
#define PROFILE_DIR_INDEX                                                             \
    "sparse_super,large_file,filetype,dir_index,resize_inode,^ext_attr,^has_journal," \
    "^extent,^64bit,^flex_bg,^metadata_csum,^huge_file,^dir_nlink,^extra_isize"

/* The profile tools/image/mkext2.sh makes, with 256-byte inodes. */
#define PROFILE_LARGE                                                                  \
    "sparse_super,large_file,filetype,^dir_index,resize_inode,^ext_attr,^has_journal," \
    "^extent,^64bit,^flex_bg,^metadata_csum,^huge_file,^dir_nlink,extra_isize"

/**
 * Stage the tree in the fixture directory: files, links, a sparse file and a
 * directory of 300 entries; and, in a tree of its own, a sparse file of
 * 70 MB with one byte behind each pointer level at 1 KiB blocks, where the
 * direct blocks end at 12 KiB, the indirect at 268 KiB and the
 * double-indirect at 64 MiB. The 1 KiB volume is made from that tree.
 */
static void build_tree(void)
{
    if (!ext2_fixture_tree(fix_dir())) {
        printf("ext2_hosttest: the native fixture tree could not be made\n");
        exit(1);
    }
}

/**
 * Make `img`, of `mib` MiB, from the tree named `tree` with the block and
 * inode sizes and the features given, and add debugfs's nodes and fields.
 * Returns the image's size in bytes; exits the test when a tool fails.
 */
static uint64_t build_image(const char *img, int block, int inode, const char *features,
                            unsigned long mib, const char *tree)
{
    static const char *const edits[] = {"mknod cold c 4 5",
                                        "mknod cnew c 1 1",
                                        "set_inode_field /cnew block[0] 0",
                                        "set_inode_field /cnew block[1] 286338160",
                                        "mknod bdev b 8 1",
                                        "set_inode_field /hello.txt uid 7",
                                        "set_inode_field /hello.txt gid 9",
                                        "set_inode_field /hello.txt mode 0104755"};
    unsigned i;
    if (!ext2_fixture_image(img, block, inode, features, mib, fix_path(tree))) {
        printf("ext2_hosttest: mke2fs failed for %s\n", img);
        exit(1);
    }
    for (i = 0; i < sizeof edits / sizeof edits[0]; i++) {
        if (!ext2_fixture_debugfs(img, edits[i], 1, 0)) {
            printf("ext2_hosttest: debugfs failed for %s\n", img);
            exit(1);
        }
    }
    return (uint64_t)mib << 20;
}

/**
 * The links the cached walk reads without the node's lock
 * (`readlink_cached`): a fast link's target from the pointer words, which a
 * buffer one byte short refuses, and none of a slow link's, which it leaves
 * to the full walk.
 */
static void cached_link_rows(void)
{
    char linkbuf[300];
    void *n;
    int err;

    n = walk("/fast", &err);
    check(n && OPS->readlink_cached(&mnt, n, linkbuf, sizeof(linkbuf)) == 9 &&
              streq(linkbuf, "hello.txt") && OPS->readlink_cached(&mnt, n, linkbuf, 9) == -1,
          "the cached walk reads a fast link's target without its lock");
    if (n) {
        OPS->put(&mnt, n);
    }
    n = walk("/slow", &err);
    check(n && OPS->readlink_cached(&mnt, n, linkbuf, sizeof(linkbuf)) == -1,
          "and leaves a slow link to the full walk");
    if (n) {
        OPS->put(&mnt, n);
    }
}

/** The rows of the profile's volume alone: the host's reader against the
 * driver (`against_reader`), and the cached walk's links. */
static void profile_rows(const char *img)
{
    against_reader(img);
    cached_link_rows();
}

/**
 * Mount `img` and read it: each file against the tree; the mode, owner and
 * links debugfs wrote; the paths of directories, rebuilt from `..`; fast and
 * slow links; devices in the old and the new encoding (debugfs writes only
 * the old, so the new one, major 300 and minor 70000, is 0x11112c70 written
 * by field into the second block word); a directory of 300 entries across
 * blocks; a hole; and statfs. With `on_profile`, every node is also checked
 * against setidcheck's reader.
 */
static void read_rows(const char *img, uint64_t bytes, const char *what, int on_profile)
{
    struct vfs_stat st;
    void *n;
    int err;
    char linkbuf[300];

    use_image(img, bytes);
    check(mount_image() == 0, what);
    if (!mnt.root) {
        return;
    }
    same_as_tree("/hello.txt", what);
    same_as_tree("/dir/sub/deep.txt", what);
    same_as_tree("/dir/big.bin", what);
    same_as_tree("/hello.link", what);
    same_as_tree("/sparse", what);

    n = walk("/hello.txt", &err);
    check(n && OPS->getattr(&mnt, n, &st) == 0 && st.mode == 0104755 && st.uid == 7 &&
              st.gid == 9 && st.nlink == 2 && st.size == 6,
          "hello.txt has the mode, owner, link count and size debugfs wrote");
    if (n) {
        OPS->put(&mnt, n);
    }
    n = walk("/dir", &err);
    check(n && OPS->getattr(&mnt, n, &st) == 0 && (st.mode & 0170000) == 0040000 && st.nlink == 3,
          "a directory reports its type and its two subdirectories' links");
    if (n) {
        char path[256];

        check(OPS->path(&mnt, n, path, sizeof(path)) == 0 && streq(path, "/dir"),
              "a directory's path is rebuilt from .. and the parent's entry");
        OPS->put(&mnt, n);
    }
    n = walk("/dir/sub", &err);
    if (n) {
        char path[256];

        check(OPS->path(&mnt, n, path, sizeof(path)) == 0 && streq(path, "/dir/sub"),
              "two levels down as well");
        OPS->put(&mnt, n);
    }

    n = walk("/fast", &err);
    check(n && OPS->getattr(&mnt, n, &st) == 0 && (st.mode & 0170000) == 0120000 &&
              OPS->readlink(&mnt, n, linkbuf, sizeof(linkbuf)) == 9 && streq(linkbuf, "hello.txt"),
          "a fast link is read from the inode");
    if (n) {
        OPS->put(&mnt, n);
    }
    n = walk("/slow", &err);
    check(n && OPS->readlink(&mnt, n, linkbuf, sizeof(linkbuf)) > 60 &&
              memcmp(linkbuf, "dir/sub/deep.txt/../../../dir/sub", 33) == 0,
          "a slow link is read from its block");
    check(n && OPS->readlink(&mnt, n, linkbuf, 10) == -ENAMETOOLONG,
          "a link longer than the buffer answers ENAMETOOLONG");
    if (n) {
        OPS->put(&mnt, n);
    }

    n = walk("/cold", &err);
    check(n && OPS->getattr(&mnt, n, &st) == 0 && (st.mode & 0170000) == 0020000 &&
              st.rdev == ((4u << 8) | 5u),
          "a device in the old encoding reports its numbers");
    if (n) {
        void *h;

        check(OPS->open(&mnt, n, 0, &h) == -EACCES, "and cannot be opened");
        OPS->put(&mnt, n);
    }
    n = walk("/cnew", &err);
    if (n && OPS->getattr(&mnt, n, &st) == 0 &&
        st.rdev != ((300u << 8) | (70000u & 0xFFu) | ((70000u & 0xFFF00u) << 12))) {
        printf("  cnew: rdev %lx\n", (unsigned long)st.rdev);
    }
    check(n && OPS->getattr(&mnt, n, &st) == 0 &&
              st.rdev == ((300u << 8) | (70000u & 0xFFu) | ((70000u & 0xFFF00u) << 12)),
          "a device in the new encoding reports its numbers");
    if (n) {
        OPS->put(&mnt, n);
    }
    n = walk("/bdev", &err);
    check(n && OPS->getattr(&mnt, n, &st) == 0 && (st.mode & 0170000) == 0060000,
          "a block device is a block device");
    if (n) {
        OPS->put(&mnt, n);
    }

    n = walk("/many", &err);
    if (n) {
        void *h;
        uint32_t cursor = 0;
        int count = 0, got;
        struct vfs_dirent de;

        if (OPS->open(&mnt, n, 0, &h) == 0) {
            while ((got = OPS->readdir(&mnt, h, &cursor, &de)) == 1) {
                if (!streq(de.name, ".") && !streq(de.name, "..")) {
                    count++;
                }
            }
            OPS->release(&mnt, h);
            check(got == 0 && count == 300, "a directory across blocks lists its 300 entries");
        }
        OPS->put(&mnt, n);
    }
    n = walk("/many/file299.txt", &err);
    check(n != 0, "the last entry of the spanning directory is found");
    if (n) {
        OPS->put(&mnt, n);
    }
    check(walk("/many/file300.txt", &err) == 0 && err == -ENOENT, "a name that is not there");
    check(walk("/hello.txt/x", &err) == 0 && err == -ENOTDIR, "a file is not a directory");

    n = walk("/sparse", &err);
    if (n) {
        int got = read_at(n, abuf, 16, 4999990);

        check(got == 13 && abuf[0] == 0 && abuf[9] == 0 && memcmp(abuf + 10, "end", 3) == 0,
              "a hole reads as zeros up to the bytes after it");
        OPS->put(&mnt, n);
    }

    if (on_profile) {
        profile_rows(img);
    }
    {
        struct vfs_statfs sf;

        check(OPS->statfs(&mnt, &sf) == 0 && sf.magic == 0xEF53 && sf.namelen == 255 &&
                  sf.blocks < bytes / sf.bsize && sf.blocks > bytes / sf.bsize / 2 &&
                  sf.bfree < sf.blocks,
              "statfs reports the format's magic, the volume's size less its own structures "
              "and a free count");
    }
    unmount_image();
}

/**
 * Read the 1 KiB volume: a byte through each pointer level and a hole. The
 * volume mounts read-only, by name, since the driver writes in order only
 * with blocks of 4096 bytes.
 */
static void levels_rows(const char *img, uint64_t bytes)
{
    void *n;
    int err;

    use_image(img, bytes);
    check(mount_image() == 0, "the 1 KiB volume mounts");
    check((mnt.flags & VFS_MS_RDONLY) != 0 &&
              memcmp(last_warn, "mounted read-only: a block size below 4096", 42) == 0,
          "and read-only, since a block below 4096 cannot be written in order");
    if (!mnt.root) {
        return;
    }
    n = walk("/levels", &err);
    if (n) {
        unsigned char b[1];

        check(read_at(n, b, 1, 0) == 1 && b[0] == 'A', "a byte through a direct block");
        check(read_at(n, b, 1, 12388) == 1 && b[0] == 'B', "a byte through the indirect block");
        check(read_at(n, b, 1, 300000) == 1 && b[0] == 'C',
              "a byte through the double-indirect block");
        check(read_at(n, b, 1, 69000000) == 1 && b[0] == 'D',
              "a byte through the triple-indirect block");
        check(read_at(n, b, 1, 40000000) == 1 && b[0] == 0, "a hole between them reads as zero");
        OPS->put(&mnt, n);
    } else {
        check(0, "the levels file is in the 1 KiB volume");
    }
    unmount_image();
}

/** Write `len` bytes at offset `at` of the image file; exit on a failure. */
static void poke(const char *img, long at, const unsigned char *bytes, unsigned long len)
{
    void *f = fopen(img, "r+b");

    if (!f || fseek(f, at, SEEK_SET_) != 0 || fwrite(bytes, 1, len, f) != len) {
        printf("ext2_hosttest: cannot poke %s\n", img);
        exit(1);
    }
    fclose(f);
}

/**
 * Damage a fresh volume for each row and require the mount, read or lookup
 * to fail, or the mount to go read-only, with the reason named in the log:
 *
 * - an unknown read-only-compatible feature mounts read-only; an unknown
 *   incompatible feature fails the mount;
 * - a file whose size debugfs sets past 4 GiB stats with that size and
 *   the sectors it holds, and opens;
 * - a block pointer past the volume, into the group descriptors or into the
 *   inode table fails the read;
 * - in the root directory, a record length past the block, an entry naming
 *   an inode past the count (the third entry, the first after `.` and `..`)
 *   and a `.` whose record ends four bytes short of its block, so that the
 *   next header would run past the block, each fail the lookup;
 * - a volume whose valid bit is clear is not checked for a read-only mount;
 * - a revision 0 volume without filetype reads, and readdir tells a
 *   directory from a file by its inode;
 * - a directory whose size ends inside its first entry fails a lookup;
 * - a slow link of 1021 bytes reads from its block, and one without a block
 *   answers `EIO`;
 * - a group free count past the group's size, a bitmap on the descriptors'
 *   block and an inode table at block 0 fail the mount;
 * - two hundred nodes are held at once, and a held name looked up again is
 *   the held node;
 * - a size of 4 GiB less one is reported, and its last bytes read as a hole;
 * - a superblock without the magic fails the mount.
 */
static void refusal_rows(void)
{
    const char *img = fix_path("mut.ext2");
    unsigned char b[4];
    uint64_t bytes;
    void *n;
    int err;
    char linkbuf[300];

    bytes = build_image(img, 4096, 128, PROFILE, 16, "tree");
    b[0] = 0x42;
    b[1] = 0;
    b[2] = 0;
    b[3] = 0;
    poke(img, 1024 + 96, b, 4);
    use_image(img, bytes);
    check(mount_image() == -EINVAL &&
              memcmp(last_warn, "refused: incompatible feature 0x40", 34) == 0,
          "an unknown incompatible feature refuses the mount by name");

    bytes = build_image(img, 4096, 128, PROFILE, 16, "tree");
    b[0] = 0x0b; /* sparse_super, large_file and huge_file */
    poke(img, 1024 + 100, b, 4);
    use_image(img, bytes);
    check(mount_image() == 0 && (mnt.flags & VFS_MS_RDONLY) != 0 &&
              memcmp(last_warn, "mounted read-only: unknown read-only", 36) == 0,
          "an unknown read-only-compatible feature mounts read-only by name");
    unmount_image();

    bytes = build_image(img, 4096, 128, PROFILE, 16, "tree");
    ext2_fixture_debugfs(fix_path("mut.ext2"), "set_inode_field /dir/big.bin size 5000000000", 1,
                         0);
    use_image(img, bytes);
    check(mount_image() == 0, "the volume with the big file mounts");
    n = walk("/dir/big.bin", &err);
    if (n) {
        struct vfs_stat st;
        void *h;

        check(OPS->getattr(&mnt, n, &st) == 0 && st.size == 5000000000ull,
              "a file past 4 GiB reports its size to stat");
        check(st.blocks_known && st.blocks >= 200000 / 512 && st.blocks < 1024,
              "and the sectors its 200000 bytes hold, which the new size did not add");
        check(OPS->open(&mnt, n, 0, &h) == 0, "and opens");
        if (h) {
            OPS->release(&mnt, h);
        }
        OPS->put(&mnt, n);
    }
    same_as_tree("/hello.txt", "the rest of the volume reads");
    unmount_image();

    bytes = build_image(img, 4096, 128, PROFILE, 16, "tree");
    ext2_fixture_debugfs(fix_path("mut.ext2"), "set_inode_field /hello.txt block[0] 9999999", 1, 0);
    use_image(img, bytes);
    check(mount_image() == 0, "the volume with the bad pointer mounts");
    n = walk("/hello.txt", &err);
    if (n) {
        check(read_at(n, abuf, 16, 0) == -EIO && memcmp(last_log, "inode ", 6) == 0,
              "a block past the volume fails the read by name");
        OPS->put(&mnt, n);
    }
    same_as_tree("/dir/sub/deep.txt", "the rest of the volume reads");
    unmount_image();

    bytes = build_image(img, 4096, 128, PROFILE, 16, "tree");
    ext2_fixture_debugfs(fix_path("mut.ext2"), "set_inode_field /hello.txt block[0] 1", 1, 0);
    use_image(img, bytes);
    check(mount_image() == 0, "the volume with the pointer into a table mounts");
    n = walk("/hello.txt", &err);
    if (n) {
        check(read_at(n, abuf, 16, 0) == -EIO && memcmp(last_warn, "inode ", 6) == 0,
              "a block inside the descriptors fails the read by name");
        OPS->put(&mnt, n);
    }
    unmount_image();

    bytes = build_image(img, 4096, 128, PROFILE, 16, "tree");
    {
        char line[256];
        void *f;
        unsigned long table = 0;

        ext2_fixture_line(img, "dumpe2fs", 0, "Inode table at", 0, fix_path("table.txt"));
        f = fopen(fix_path("table.txt"), "r");
        if (f && fgets(line, sizeof(line), f)) {
            char *p = line;

            while (*p && (*p < '0' || *p > '9')) {
                p++;
            }
            table = strtoul(p, 0, 10);
        }
        if (f) {
            fclose(f);
        }
        if (table != 0) {
            char cmd[512];

            snprintf(cmd, sizeof(cmd), "set_inode_field /hello.txt block[0] %lu", table);
            ext2_fixture_debugfs(img, cmd, 1, 0);
            use_image(img, bytes);
            check(mount_image() == 0, "the volume with the pointer into the inode table mounts");
            n = walk("/hello.txt", &err);
            if (n) {
                check(read_at(n, abuf, 16, 0) == -EIO && memcmp(last_warn, "inode ", 6) == 0,
                      "a block inside the inode table fails the read by name");
                OPS->put(&mnt, n);
            }
            unmount_image();
        } else {
            check(0, "dumpe2fs names the inode table for the row");
        }
    }

    bytes = build_image(img, 4096, 128, PROFILE, 16, "tree");
    use_image(img, bytes);
    check(mount_image() == 0, "the volume mounts before its root is damaged");
    if (mnt.root) {
        struct vfs_stat st;
        uint32_t root_block = 0;

        OPS->getattr(&mnt, mnt.root, &st);
        unmount_image();
        {
            char line[256];
            void *f;

            ext2_fixture_line(img, "debugfs", "stat /", "BLOCKS", 1, fix_path("blk.txt"));
            f = fopen(fix_path("blk.txt"), "r");
            if (f && fgets(line, sizeof(line), f)) {
                char *p = line;

                while (*p && *p != ':') {
                    p++;
                }
                if (*p == ':') {
                    root_block = (uint32_t)strtoul(p + 1, 0, 10);
                }
            }
            if (f) {
                fclose(f);
            }
        }
        if (root_block != 0) {
            b[0] = 0x00;
            b[1] = 0x20; /* rec_len 8192, past a 4096-byte block */
            poke(img, (long)root_block * 4096 + 4, b, 2);
            use_image(img, bytes);
            check(mount_image() == 0, "the volume with the malformed entry mounts");
            check(walk("/hello.txt", &err) == 0 && err == -EIO &&
                      memcmp(last_warn, "directory 2: entry at 0 is malformed", 36) == 0,
                  "a malformed entry fails the lookup by name");
            unmount_image();
            bytes = build_image(img, 4096, 128, PROFILE, 16, "tree");
            b[0] = 0xff;
            b[1] = 0xff;
            b[2] = 0xff;
            b[3] = 0x0f;
            poke(img, (long)root_block * 4096 + 24, b, 4);
            use_image(img, bytes);
            check(mount_image() == 0, "the volume with the entry past the count mounts");
            check(walk("/zzz", &err) == 0 && err == -EIO &&
                      memcmp(last_warn, "directory 2: entry at 24 is malformed", 37) == 0,
                  "an entry naming an inode past the count is malformed by name");
            unmount_image();
            bytes = build_image(img, 4096, 128, PROFILE, 16, "tree");
            b[0] = 0xfc;
            b[1] = 0x0f;
            poke(img, (long)root_block * 4096 + 4, b, 2);
            use_image(img, bytes);
            check(mount_image() == 0, "the volume with a record four bytes short mounts");
            check(walk("/hello.txt", &err) == 0 && err == -EIO,
                  "a header that would run past its block fails the lookup, with no byte "
                  "read past the block's copy, which AddressSanitizer would report");
            unmount_image();
        } else {
            check(0, "the root's block was found for the malformed-entry row");
        }
    }

    bytes = build_image(img, 4096, 128, PROFILE, 16, "tree");
    b[0] = 0;
    b[1] = 0;
    poke(img, 1024 + 58, b, 2);
    use_image(img, bytes);
    last_check[0] = 0;
    check(mount_ro() == 0 && (mnt.flags & VFS_MS_RDONLY) != 0 && last_check[0] == 0,
          "a volume whose valid bit is clear is not checked for a read-only mount");
    unmount_image();

    bytes = build_image(img, 4096, 128,
                        "^sparse_super,^large_file,^filetype,^dir_index,^resize_inode,"
                        "^ext_attr,^has_journal,^extent,^64bit,^flex_bg,"
                        "^metadata_csum,^huge_file,^dir_nlink,^extra_isize",
                        16, "tree");
    ext2_fixture_debugfs(fix_path("mut.ext2"), "ssv rev_level 0", 1, 0);
    use_image(img, bytes);
    check(mount_image() == 0, "a revision 0 volume mounts");
    same_as_tree("/hello.txt", "a revision 0 volume reads");
    n = walk("/dir", &err);
    if (n) {
        void *h;
        uint32_t cursor = 0;
        int got, dirs = 0, files = 0;
        struct vfs_dirent de;

        if (OPS->open(&mnt, n, 0, &h) == 0) {
            while ((got = OPS->readdir(&mnt, h, &cursor, &de)) == 1) {
                if (streq(de.name, "sub")) {
                    dirs += de.is_dir;
                } else if (streq(de.name, "big.bin")) {
                    files += !de.is_dir;
                }
            }
            OPS->release(&mnt, h);
            check(got == 0 && dirs == 1 && files == 1,
                  "without filetype, readdir tells a directory from a file by its inode");
        }
        OPS->put(&mnt, n);
    }
    unmount_image();

    bytes = build_image(img, 4096, 128, PROFILE, 16, "tree");
    ext2_fixture_debugfs(fix_path("mut.ext2"), "set_inode_field /dir size 1", 1, 0);
    use_image(img, bytes);
    check(mount_image() == 0, "the volume with the short directory mounts");
    check(walk("/dir/sub", &err) == 0 && err == -EIO && memcmp(last_warn, "directory ", 10) == 0,
          "an entry past the directory's size is malformed by name");
    unmount_image();

    bytes = build_image(img, 4096, 128, PROFILE, 16, "tree");
    {
        char target[1022];
        memset(target, 'a', sizeof target - 1);
        target[sizeof target - 1] = 0;
        if (symlink(target, fix_path("tree/long1021")) ||
            !ext2_fixture_image(img, 4096, 128, PROFILE, 16, fix_path("tree")) ||
            unlink(fix_path("tree/long1021"))) {
            printf("ext2_hosttest: the long-link fixture could not be made\n");
            exit(1);
        }
    }
    use_image(img, bytes);
    check(mount_image() == 0, "the volume with the long link mounts");
    n = walk("/long1021", &err);
    if (n) {
        static char big[2048];

        check(OPS->readlink(&mnt, n, big, sizeof(big)) == 1021 && big[0] == 'a' && big[1020] == 'a',
              "a slow link of 1021 bytes is read from its block");
        OPS->put(&mnt, n);
    } else {
        check(0, "the long link is in the volume");
    }
    unmount_image();
    bytes = build_image(img, 4096, 128, PROFILE, 16, "tree");
    ext2_fixture_debugfs(fix_path("mut.ext2"), "set_inode_field /slow block[0] 0", 1, 0);
    use_image(img, bytes);
    check(mount_image() == 0, "the volume with the blockless slow link mounts");
    n = walk("/slow", &err);
    if (n) {
        check(OPS->readlink(&mnt, n, linkbuf, sizeof(linkbuf)) == -EIO,
              "a slow link without a block answers EIO");
        OPS->put(&mnt, n);
    }
    unmount_image();

    bytes = build_image(img, 4096, 128, PROFILE, 16, "tree");
    ext2_fixture_debugfs(fix_path("mut.ext2"), "set_bg 0 free_blocks_count 65535", 1, 0);
    use_image(img, bytes);
    check(mount_image() == -EINVAL && streq(last_warn, "refused: group free count"),
          "a free count past the group is refused by name");
    bytes = build_image(img, 4096, 128, PROFILE, 16, "tree");
    ext2_fixture_debugfs(fix_path("mut.ext2"), "set_bg 0 block_bitmap 1", 1, 0);
    use_image(img, bytes);
    check(mount_image() == -EINVAL &&
              memcmp(last_warn, "refused: group 0: bitmaps at 1 and ", 35) == 0,
          "a bitmap on the descriptors' block is refused by name");
    bytes = build_image(img, 4096, 128, PROFILE, 16, "tree");
    ext2_fixture_debugfs(fix_path("mut.ext2"), "set_bg 0 inode_table 0", 1, 0);
    use_image(img, bytes);
    check(mount_image() == -EINVAL && memcmp(last_warn, "refused: group 0: bitmaps at ", 29) == 0 &&
              strstr(last_warn, ", table at 0, in blocks "),
          "an inode table at block zero is refused by name");

    bytes = build_image(img, 4096, 128, PROFILE, 16, "tree");
    use_image(img, bytes);
    check(mount_image() == 0, "the volume mounts for the node row");
    if (mnt.root) {
        void *held[200];
        int nheld = 0, i;
        void *many = walk("/many", &err);

        for (i = 0; many && i < 200; i++) {
            char name[32];
            void *node;

            snprintf(name, sizeof(name), "file%d.txt", i);
            if (OPS->lookup(&mnt, many, name, &node) != 0) {
                break;
            }
            held[nheld++] = node;
        }
        check(nheld == 200, "two hundred files are held at once");
        if (many) {
            void *node = 0;

            if (nheld > 150 && OPS->lookup(&mnt, many, "file150.txt", &node) == 0) {
                check(node == held[150], "a held name looked up again is the held node");
                OPS->put(&mnt, node);
            } else {
                check(0, "a held name looked up again is the held node");
            }
            for (i = 0; i < nheld; i++) {
                OPS->put(&mnt, held[i]);
            }
            OPS->put(&mnt, many);
        }
    }
    unmount_image();

    bytes = build_image(img, 4096, 128, PROFILE, 16, "tree");
    ext2_fixture_debugfs(fix_path("mut.ext2"), "set_inode_field /sparse size 4294967295", 1, 0);
    use_image(img, bytes);
    check(mount_image() == 0, "the volume with the largest file mounts");
    n = walk("/sparse", &err);
    if (n) {
        struct vfs_stat st;

        check(OPS->getattr(&mnt, n, &st) == 0 && st.size == 4294967295ull,
              "a size of 4 GiB less one is reported");
        check(read_at(n, abuf, 16, 4294967290ull) == 5 && abuf[0] == 0 && abuf[4] == 0,
              "and its last five bytes read as the hole they are");
        OPS->put(&mnt, n);
    }
    unmount_image();

    bytes = build_image(img, 4096, 128, PROFILE, 16, "tree");
    b[0] = 0;
    b[1] = 0;
    poke(img, 1024 + 56, b, 2);
    use_image(img, bytes);
    check(mount_image() == -EINVAL && streq(last_warn, "refused: magic"), "no magic is refused");
}

/* ---- the writer's rows ---- */

/* The mount flags the writer's rows mount with. They judge the strict
 * order, each write on the medium before the next, which `dirsync` asks
 * for; `default_mount_row` judges a mount without it. */
static uint32_t rw_flags = VFS_MS_DIRSYNC;

/** Mount the device read-write with `rw_flags`; the mount's result. */
static int mount_rw(void)
{
    return mount_flags(rw_flags);
}

/**
 * Unmount as the VFS does: prepare the unmount, sync the cache, then drop
 * the root and unmount. Returns the first error of the prepare or the sync.
 */
static int unmount_rw(void)
{
    int rc = 0;

    if (mnt.root) {
        rc = OPS->prepare_unmount(&mnt);
        if (rc == 0) {
            rc = bcache_sync(&dev);
        }
        OPS->put(&mnt, mnt.root);
        OPS->unmount(&mnt);
        mnt.root = 0;
    }
    return rc;
}

/** The directory at `path`, held, as `walk` resolves it. */
static void *dir_of(const char *path, int *err)
{
    return walk(path, err);
}

/** Create `name`, mode 0644, under `dir`; the node held, or NULL with `*rc`. */
static void *make_file(void *dir, const char *name, int *rc)
{
    void *n = 0;

    *rc = OPS->create(&mnt, dir, name, 0644, &n);
    return *rc == 0 ? n : 0;
}

/**
 * Write `len` bytes at `off` through a description opened for writing.
 * Returns 0, a negative errno, or -1 for a short write.
 */
static int write_at(void *node, const void *buf, uint32_t len, uint64_t off)
{
    void *h;
    int32_t got;
    int rc = OPS->open(&mnt, node, 1, &h);

    if (rc != 0) {
        return rc;
    }
    got = OPS->write(&mnt, h, buf, len, off);
    OPS->release(&mnt, h);
    return got < 0 ? got : (got == (int32_t)len ? 0 : -1);
}

/**
 * Whether the file at `path` holds exactly the `len` bytes of `want`. A
 * mismatch prints the length or the first byte that differs.
 */
static int file_is(const char *path, const unsigned char *want, uint32_t len)
{
    int err, got;
    void *n = walk(path, &err);

    if (!n) {
        return 0;
    }
    got = read_at(n, abuf, sizeof(abuf), 0);
    OPS->put(&mnt, n);
    if (got != (int)len) {
        printf("  %s: %d bytes, %u wanted\n", path, got, (unsigned)len);
        return 0;
    }
    if (len != 0 && memcmp(abuf, want, len) != 0) {
        uint32_t i = 0;

        while (i < len && abuf[i] == want[i]) {
            i++;
        }
        printf("  %s: byte %u is %d, %d wanted\n", path, (unsigned)i, abuf[i], want[i]);
        return 0;
    }
    return 1;
}

static unsigned char pattern[300000];

/** Fill `pattern` with bytes that repeat only every 256. */
static void fill_pattern(void)
{
    unsigned i;

    for (i = 0; i < sizeof(pattern); i++) {
        pattern[i] = (unsigned char)((i * 31 + 7) & 0xFF);
    }
}

/**
 * Whether `e2fsck -fn` finds `img` clean. It is the first of the three
 * judges of a finished sequence, with setidcheck's reader walking every node
 * and debugfs listing the root.
 */
static int e2fsck_clean(const char *img)
{
    return ext2_fixture_fsck(img, "-fn", fix_path("fsck.out")) == 0;
}

/**
 * Signed seconds and both range limits, set on a file with nanoseconds and
 * read again after each remount, on a volume of `inode`-byte inodes with
 * `features`. A 128-byte inode keeps 32 signed bits of seconds and no
 * nanoseconds; a 256-byte one keeps the nanoseconds and the two epoch bits,
 * which reach 2446. The clock supplies the same cases for the change time.
 */
static void time_row(int inode, const char *features)
{
    static const struct {
        int64_t requested;
        int64_t small; /* stored in a 128-byte inode */
        int64_t large; /* and in a 256-byte one */
    } cases[] = {
        {-(int64_t)2147483647 - 2, -2147483647 - 1, -2147483647 - 1},
        {-2147483647 - 1, -2147483647 - 1, -2147483647 - 1},
        {-31536000, -31536000, -31536000},
        {0, 0, 0},
        {2147483647, 2147483647, 2147483647},
        {(int64_t)2147483647 + 1, 2147483647, (int64_t)2147483647 + 1},
        {(int64_t)4294967295u + 2, 2147483647, (int64_t)4294967295u + 2},
        {2208988800, 2147483647, 2208988800}, /* 2040-01-01 */
        {((int64_t)3 << 32) + 2147483647, 2147483647, ((int64_t)3 << 32) + 2147483647},
        {((int64_t)3 << 32) + 2147483647 + 1, 2147483647, ((int64_t)3 << 32) + 2147483647}};
    const char *img = fix_path(inode == 128 ? "times.ext2" : "times256.ext2");
    int large = inode > 128;
    uint64_t bytes;
    struct vfs_stat set, got;
    unsigned i, j, count = sizeof(cases) / sizeof(cases[0]);
    int64_t a, m, c, an, mn, cn;
    void *node;
    int rc;

    bytes = build_image(img, 4096, inode, features, 16, "tree");
    use_image(img, bytes);
    check(mount_rw() == 0, "the timestamp volume mounts writable");
    node = make_file(mnt.root, "times", &rc);
    check(node != 0, "the timestamp file is created");
    if (!node) {
        unmount_rw();
        return;
    }
    for (i = 0; i < count; i++) {
        j = count - i - 1;
        memset(&set, 0, sizeof(set));
        set.atime_sec = cases[i].requested;
        set.atime_nsec = 123456789;
        set.mtime_sec = cases[j].requested;
        set.mtime_nsec = 999999999;
        clock_seconds = cases[i].requested;
        clock_nanos = 500000001;
        a = large ? cases[i].large : cases[i].small;
        m = large ? cases[j].large : cases[j].small;
        c = a;
        an = large ? 123456789 : 0;
        mn = large ? 999999999 : 0;
        cn = large ? 500000001 : 0;
        check(OPS->setattr(&mnt, node, VFS_SET_ATIME | VFS_SET_MTIME, &set) == 0,
              "ext2 accepts signed and out-of-range timestamp seconds");
        check(OPS->getattr(&mnt, node, &got) == 0 && got.atime_sec == a && got.mtime_sec == m &&
                  got.ctime_sec == c && got.atime_nsec == an && got.mtime_nsec == mn &&
                  got.ctime_nsec == cn,
              large ? "ext2 reports the times a 256-byte inode keeps, clamped at both limits"
                    : "ext2 reports signed timestamp seconds clamped at both limits");
        clock_seconds = 1000000000;
        clock_nanos = 0;
        OPS->put(&mnt, node);
        check(unmount_rw() == 0, "the timestamp volume unmounts");
        use_image(img, bytes);
        check(mount_rw() == 0, "the timestamp volume remounts");
        node = walk("/times", &rc);
        check(node != 0, "the timestamp file survives a remount");
        if (!node) {
            unmount_rw();
            return;
        }
        check(OPS->getattr(&mnt, node, &got) == 0 && got.atime_sec == a && got.mtime_sec == m &&
                  got.ctime_sec == c && got.atime_nsec == an && got.mtime_nsec == mn &&
                  got.ctime_nsec == cn,
              large ? "ext2 keeps a 256-byte inode's seconds and nanoseconds on disk"
                    : "ext2 preserves signed and clamped timestamps on disk");
    }
    OPS->put(&mnt, node);
    check(unmount_rw() == 0 && e2fsck_clean(img), "the timestamp volume is clean");
}

/**
 * Run `e2fsck -fn` on `img` and return its exit status: 0 clean, 4 with
 * faults left, 8 when it could not run. Each question e2fsck asked is
 * written to findings.txt in the fixture directory, a bare question such as
 * "Fix? no" replaced by the message line before it.
 */
static int e2fsck_status(const char *img)
{
    int rc = ext2_fixture_fsck(img, "-fn", fix_path("fsck.out"));
    if (!ext2_fixture_findings(fix_path("fsck.out"), fix_path("findings.txt")))
        return 8;
    return rc;
}

/* The repairs the order of writes allows at a stop: counts, bitmaps, a
   size or block count behind its pointers, an orphan, and the topology of
   a directory moved between parents. `#` in a pattern is a run of digits;
   anything else e2fsck asks about is a state the order does not allow. */
static const char *const allowed_findings[] = {
    "Free inodes count wrong",
    "Free blocks count wrong",
    "Directories count wrong",
    "Block bitmap differences",
    "Inode bitmap differences",
    "Inode #, i_size is",
    "Inode #, i_blocks is",
    "Inode # ref count is",
    "Deleted inode # has zero dtime",
    "Unattached inode #",
    "Unattached zero-length inode #",
    "Unconnected directory inode #",
    "'..' in ",
    "Entry '",
};

/** Whether `line` begins with `pat`, where `#` matches a run of digits. */
static int matches_pattern(const char *line, const char *pat)
{
    while (*pat) {
        if (*pat == '#') {
            if (*line < '0' || *line > '9') {
                return 0;
            }
            while (*line >= '0' && *line <= '9') {
                line++;
            }
            pat++;
        } else {
            if (*line != *pat) {
                return 0;
            }
            line++;
            pat++;
        }
    }
    return 1;
}

/**
 * Whether every question in findings.txt is a repair the order of writes
 * allows at a stop: one of `allowed_findings`, and a wrong link count only
 * where `counts` says the sequence writes a count apart from its entry.
 */
static int findings_allowed(int counts)
{
    void *f = fopen(fix_path("findings.txt"), "r");
    char line[512];
    int ok = 1;

    if (!f) {
        return 0;
    }
    while (fgets(line, sizeof(line), f)) {
        unsigned k;
        int any = 0;

        for (k = 0; k < sizeof(allowed_findings) / sizeof(allowed_findings[0]); k++) {
            if (matches_pattern(line, allowed_findings[k]) &&
                (counts || !matches_pattern(line, "Inode # ref count is"))) {
                any = 1;
            }
        }
        if (!any) {
            printf("  a repair the order of writes does not allow: %s", line);
            ok = 0;
        }
    }
    fclose(f);
    return ok;
}

/** Whether setidcheck's reader walks every node of `img` without a fault. */
static int reader_walks(const char *img)
{
    const char *argv[] = {host_tool_path("setidcheck"), "--nodes", "--ext2", img, 0};
    return host_command(argv, fix_path("nodes.txt"), fix_path("nodes.txt"), 0, 0) == 0;
}

/** Whether debugfs's answer to `request` has the text `text`. */
static int debugfs_says(const char *img, const char *request, const char *text)
{
    return ext2_fixture_debugfs(img, request, 0, fix_path("debugfs.txt")) &&
           ext2_fixture_contains(fix_path("debugfs.txt"), text);
}

/** Whether debugfs's `ls -l` of `dir` in `img` lists `name`. */
static int debugfs_lists(const char *img, const char *dir, const char *name)
{
    char request[600], line[4096];
    void *f;
    int found = 0;
    unsigned long n, len = strlen(name);
    snprintf(request, sizeof request, "ls -l %s", dir);
    if (!ext2_fixture_debugfs(img, request, 0, fix_path("debugfs.txt")))
        return 0;
    f = fopen(fix_path("debugfs.txt"), "r");
    while (f && fgets(line, sizeof line, f)) {
        n = strlen(line);
        if (n && line[n - 1] == '\n')
            line[--n] = 0;
        if (n > len && line[n - len - 1] == ' ' && !strcmp(line + n - len, name))
            found = 1;
    }
    if (f)
        fclose(f);
    return found;
}

/* The sequences, each on a fresh volume; a sequence returns 0 and leaves
   the volume as its oracle describes. A sequence records in `op_mark` the
   write count from which a name it made, or a file it renames or links,
   must have a name at every stop, so that the matrix can require one; -1
   when no such point is recorded. */
static int op_mark = -1;
/* The volume the write rows use, set by main. */
static const char *wimg;
static uint64_t wbytes;

/**
 * Create new.txt, then write 100 bytes of the pattern and 10000 more after
 * them. The name is committed once the create returns, so `op_mark` is set
 * there. Each sequence returns 0 or the first error.
 */
static int seq_create_write(void)
{
    int rc;
    void *n = make_file(mnt.root, "new.txt", &rc);

    if (!n) {
        return rc;
    }
    op_mark = writes_made;
    rc = write_at(n, pattern, 100, 0);
    if (rc == 0) {
        rc = write_at(n, pattern, 10000, 100);
    }
    OPS->put(&mnt, n);
    return rc;
}

/** Write 100000 bytes in three calls, across the twelve direct blocks. */
static int seq_extend(void)
{
    int rc;
    void *n = make_file(mnt.root, "big.bin", &rc);

    if (!n) {
        return rc;
    }
    rc = write_at(n, pattern, 40000, 0);
    if (rc == 0) {
        rc = write_at(n, pattern + 40000, 20000, 40000);
    }
    if (rc == 0) {
        rc = write_at(n, pattern + 60000, 40000, 60000);
    }
    OPS->put(&mnt, n);
    return rc;
}

/** Write 16 bytes at 100000 of a new file, leaving a hole before them. */
static int seq_past_end(void)
{
    int rc;
    void *n = make_file(mnt.root, "holey.bin", &rc);

    if (!n) {
        return rc;
    }
    rc = write_at(n, pattern, 16, 100000);
    OPS->put(&mnt, n);
    return rc;
}

/** Write 40000 bytes, truncate to 5000, then grow to 7000 by truncation. */
static int seq_truncate(void)
{
    int rc;
    void *n = make_file(mnt.root, "cut.bin", &rc);
    void *h;

    if (!n) {
        return rc;
    }
    rc = write_at(n, pattern, 40000, 0);
    if (rc == 0) {
        rc = OPS->open(&mnt, n, 1, &h);
        if (rc == 0) {
            rc = OPS->truncate(&mnt, h, 5000);
            if (rc == 0) {
                rc = OPS->truncate(&mnt, h, 7000);
            }
            OPS->release(&mnt, h);
        }
    }
    OPS->put(&mnt, n);
    return rc;
}

/** Write 12000 bytes to a new file and unlink it. */
static int seq_unlink(void)
{
    int rc;
    void *n = make_file(mnt.root, "gone.txt", &rc);

    if (!n) {
        return rc;
    }
    rc = write_at(n, pattern, 12000, 0);
    OPS->put(&mnt, n);
    if (rc == 0) {
        rc = OPS->unlink(&mnt, mnt.root, "gone.txt");
    }
    return rc;
}

/** Make newdir and newdir/inner, then make and remove olddir. */
static int seq_mkdir_rmdir(void)
{
    int rc = OPS->mkdir(&mnt, mnt.root, "newdir", 0755);
    int err;
    void *d;

    if (rc != 0) {
        return rc;
    }
    op_mark = writes_made;
    d = walk("/newdir", &err);
    if (!d) {
        return err;
    }
    rc = OPS->mkdir(&mnt, d, "inner", 0700);
    OPS->put(&mnt, d);
    if (rc == 0) {
        rc = OPS->mkdir(&mnt, mnt.root, "olddir", 0755);
    }
    if (rc == 0) {
        rc = OPS->rmdir(&mnt, mnt.root, "olddir");
    }
    return rc;
}

/** Write from.txt, rename it to to.txt, then move it to dir/moved.txt. */
static int seq_rename(void)
{
    int rc;
    void *n = make_file(mnt.root, "from.txt", &rc);
    void *d;
    int err;

    if (!n) {
        return rc;
    }
    rc = write_at(n, pattern, 3000, 0);
    OPS->put(&mnt, n);
    op_mark = writes_made;
    if (rc == 0) {
        rc = OPS->rename(&mnt, mnt.root, "from.txt", mnt.root, "to.txt");
    }
    if (rc == 0) {
        d = walk("/dir", &err);
        if (!d) {
            return err;
        }
        rc = OPS->rename(&mnt, mnt.root, "to.txt", d, "moved.txt");
        OPS->put(&mnt, d);
    }
    return rc;
}

/** Write src.txt and victim.txt, then rename src.txt over victim.txt. */
static int seq_rename_over(void)
{
    int rc;
    void *n = make_file(mnt.root, "src.txt", &rc);

    if (!n) {
        return rc;
    }
    rc = write_at(n, pattern, 2000, 0);
    OPS->put(&mnt, n);
    if (rc == 0) {
        n = make_file(mnt.root, "victim.txt", &rc);
        if (!n) {
            return rc;
        }
        rc = write_at(n, pattern + 100, 9000, 0);
        OPS->put(&mnt, n);
    }
    op_mark = writes_made;
    if (rc == 0) {
        rc = OPS->rename(&mnt, mnt.root, "src.txt", mnt.root, "victim.txt");
    }
    return rc;
}

/**
 * Make mover with a file in it, rename it into dir/sub as moved, then
 * rename it back to the root over the empty directory empty.
 */
static int seq_rename_dir(void)
{
    int rc = OPS->mkdir(&mnt, mnt.root, "mover", 0755);
    void *d, *sub;
    int err;

    if (rc != 0) {
        return rc;
    }
    d = walk("/mover", &err);
    if (!d) {
        return err;
    }
    {
        void *n = make_file(d, "inside.txt", &rc);

        if (n) {
            rc = write_at(n, pattern, 500, 0);
            OPS->put(&mnt, n);
        }
    }
    OPS->put(&mnt, d);
    if (rc != 0) {
        return rc;
    }
    sub = walk("/dir/sub", &err);
    if (!sub) {
        return err;
    }
    op_mark = writes_made;
    rc = OPS->rename(&mnt, mnt.root, "mover", sub, "moved");
    if (rc == 0) {
        rc = OPS->mkdir(&mnt, mnt.root, "empty", 0755);
    }
    if (rc == 0) {
        rc = OPS->rename(&mnt, sub, "moved", mnt.root, "empty");
    }
    OPS->put(&mnt, sub);
    return rc;
}

/** Write one.txt, link it as two.txt, then unlink one.txt. */
static int seq_link(void)
{
    int rc;
    void *n = make_file(mnt.root, "one.txt", &rc);

    if (!n) {
        return rc;
    }
    rc = write_at(n, pattern, 700, 0);
    op_mark = writes_made;
    if (rc == 0) {
        rc = OPS->link(&mnt, mnt.root, "two.txt", n);
    }
    OPS->put(&mnt, n);
    if (rc == 0) {
        rc = OPS->unlink(&mnt, mnt.root, "one.txt");
    }
    return rc;
}

/** Make a fast link, quick, and a slow link of 199 bytes, lengthy. */
static int seq_symlink(void)
{
    static char longtarget[200];
    int rc, i;

    for (i = 0; i < 199; i++) {
        longtarget[i] = 't';
    }
    longtarget[199] = '\0';
    rc = OPS->symlink(&mnt, mnt.root, "quick", "hello.txt");
    if (rc == 0) {
        op_mark = writes_made;
        rc = OPS->symlink(&mnt, mnt.root, "lengthy", longtarget);
    }
    return rc;
}

/**
 * Unlink a file while it is open, write 8000 bytes through it, read them
 * back, and close it.
 */
static int seq_open_unlinked(void)
{
    int rc;
    void *n = make_file(mnt.root, "temp.txt", &rc);
    void *h;

    if (!n) {
        return rc;
    }
    rc = OPS->open(&mnt, n, 1, &h);
    if (rc == 0) {
        rc = OPS->unlink(&mnt, mnt.root, "temp.txt");
        if (rc == 0) {
            rc = write_at(n, pattern, 8000, 0);
        }
        if (rc == 0) {
            int got = read_at(n, abuf, 8000, 0);

            rc = got == 8000 && memcmp(abuf, pattern, 8000) == 0 ? 0 : -1;
        }
        OPS->release(&mnt, h);
    }
    OPS->put(&mnt, n);
    return rc;
}

/**
 * Make three hundred files, which grows the root across blocks, then
 * remove the even-numbered half.
 */
static int seq_many(void)
{
    int i, rc = 0;

    for (i = 0; i < 300 && rc == 0; i++) {
        char name[32];
        void *n;

        snprintf(name, sizeof(name), "m%d.txt", i);
        n = make_file(mnt.root, name, &rc);
        if (n) {
            rc = write_at(n, pattern, 10, 0);
            OPS->put(&mnt, n);
        }
    }
    for (i = 0; i < 300 && rc == 0; i += 2) {
        char name[32];

        snprintf(name, sizeof(name), "m%d.txt", i);
        rc = OPS->unlink(&mnt, mnt.root, name);
    }
    return rc;
}

/* A device number of major 300 and minor 70000, which needs Linux's new
   encoding, and /dev/null's, which fits the old. */
#define WIDE_RDEV ((70000u & 0xFFu) | (300u << 8) | ((70000u & 0xFFF00u) << 12))
#define NULL_RDEV ((1u << 8) | 3u)

/**
 * Make a FIFO, a socket and two device nodes, one number in each encoding,
 * and require a second mknod of the FIFO's name to be `EEXIST`. The FIFO's
 * name is committed once its mknod returns, so `op_mark` is set there.
 */
static int seq_nodes(void)
{
    void *n = 0;
    int rc = OPS->mknod(&mnt, mnt.root, "fifo", 0010644, 0, &n);

    if (rc == 0) {
        OPS->put(&mnt, n);
        op_mark = writes_made;
        rc = OPS->mknod(&mnt, mnt.root, "sock", 0140755, 0, &n);
    }
    if (rc == 0) {
        OPS->put(&mnt, n);
        rc = OPS->mknod(&mnt, mnt.root, "null", 0020666, NULL_RDEV, &n);
    }
    if (rc == 0) {
        OPS->put(&mnt, n);
        rc = OPS->mknod(&mnt, mnt.root, "wide", 0060600, WIDE_RDEV, &n);
    }
    if (rc == 0) {
        OPS->put(&mnt, n);
        rc = OPS->mknod(&mnt, mnt.root, "fifo", 0010644, 0, &n) == -EEXIST ? 0 : -1;
    }
    return rc;
}

/** Whether the node at `path` has `mode`, device number `rdev` and size 0. */
static int node_is(const char *path, uint32_t mode, uint32_t rdev)
{
    struct vfs_stat st;
    int err, ok;
    void *n = walk(path, &err);

    if (!n) {
        return 0;
    }
    ok = OPS->getattr(&mnt, n, &st) == 0 && st.mode == mode && st.rdev == rdev && st.size == 0;
    if (!ok) {
        printf("  %s: mode 0%o rdev 0x%x\n", path, (unsigned)st.mode, (unsigned)st.rdev);
    }
    OPS->put(&mnt, n);
    return ok;
}

/* `counts` marks a sequence whose order writes a link count apart from the
   entry it counts: only there may a stop leave a count wrong. */
struct sequence {
    const char *name;
    int (*run)(void);
    int counts;
};

static const struct sequence sequences[] = {
    {"create and write", seq_create_write, 0},
    {"extend through the pointer levels", seq_extend, 0},
    {"write past the end", seq_past_end, 0},
    {"truncate", seq_truncate, 0},
    {"unlink", seq_unlink, 0},
    {"mkdir and rmdir", seq_mkdir_rmdir, 1},
    {"rename", seq_rename, 1},
    {"rename over a file", seq_rename_over, 1},
    {"rename a directory across parents and over an empty one", seq_rename_dir, 1},
    {"link", seq_link, 1},
    {"symlink", seq_symlink, 0},
    {"an open unlinked file", seq_open_unlinked, 0},
    {"many files", seq_many, 0},
    {"a FIFO, a socket and device nodes", seq_nodes, 0},
};

/** Whether hello.txt and dir/sub/deep.txt of the staged tree read as staged. */
static int base_intact(void)
{
    unsigned char want[8];

    __builtin_memcpy(want, "hello\n", 6);
    return file_is("/hello.txt", want, 6) &&
           file_is("/dir/sub/deep.txt", (const unsigned char *)"deep\n", 5);
}

/**
 * Whether the finished sequence `i` of `sequences` left what it should: the
 * base files intact, the files it made with their bytes, and the names it
 * removed or moved gone. Sequence 0 is judged by `oracle_create_write`,
 * which knows the bytes it writes, and is not a case here.
 */
static int oracle_after(int i)
{
    int err;
    void *n;
    struct vfs_stat st;

    if (!base_intact()) {
        return 0;
    }
    switch (i) {
    case 1:
        return file_is("/big.bin", pattern, 100000);
    case 2: {
        unsigned char want[100016];

        __builtin_memset(want, 0, 100000);
        __builtin_memcpy(want + 100000, pattern, 16);
        return file_is("/holey.bin", want, 100016);
    }
    case 3: {
        unsigned char want[7000];

        __builtin_memcpy(want, pattern, 5000);
        __builtin_memset(want + 5000, 0, 2000);
        return file_is("/cut.bin", want, 7000);
    }
    case 4:
        return walk("/gone.txt", &err) == 0 && err == -ENOENT;
    case 5:
        n = walk("/newdir/inner", &err);
        if (!n) {
            return 0;
        }
        OPS->put(&mnt, n);
        return walk("/olddir", &err) == 0 && err == -ENOENT;
    case 6:
        return file_is("/dir/moved.txt", pattern, 3000) && walk("/from.txt", &err) == 0 &&
               walk("/to.txt", &err) == 0;
    case 7:
        return file_is("/victim.txt", pattern, 2000) && walk("/src.txt", &err) == 0;
    case 8:
        return file_is("/empty/inside.txt", pattern, 500) && walk("/mover", &err) == 0 &&
               walk("/dir/sub/moved", &err) == 0;
    case 9:
        n = walk("/two.txt", &err);
        if (!n || OPS->getattr(&mnt, n, &st) != 0 || st.nlink != 1) {
            if (n) {
                OPS->put(&mnt, n);
            }
            return 0;
        }
        OPS->put(&mnt, n);
        return file_is("/two.txt", pattern, 700) && walk("/one.txt", &err) == 0;
    case 10: {
        char buf[300];

        n = walk("/quick", &err);
        if (!n) {
            return 0;
        }
        err = OPS->readlink(&mnt, n, buf, sizeof(buf));
        OPS->put(&mnt, n);
        if (err != 9 || memcmp(buf, "hello.txt", 9) != 0) {
            return 0;
        }
        n = walk("/lengthy", &err);
        if (!n) {
            return 0;
        }
        err = OPS->readlink(&mnt, n, buf, sizeof(buf));
        OPS->put(&mnt, n);
        return err == 199 && buf[0] == 't' && buf[198] == 't';
    }
    case 11:
        return walk("/temp.txt", &err) == 0 && err == -ENOENT;
    case 12:
        return file_is("/m1.txt", pattern, 10) && file_is("/m299.txt", pattern, 10) &&
               walk("/m0.txt", &err) == 0 && walk("/m298.txt", &err) == 0;
    case 13:
        return node_is("/fifo", 0010644, 0) && node_is("/sock", 0140755, 0) &&
               node_is("/null", 0020666, NULL_RDEV) && node_is("/wide", 0060600, WIDE_RDEV);
    default:
        return 0;
    }
}

/**
 * Whether new.txt holds what `seq_create_write` wrote: the first 100 bytes
 * of the pattern, then its first 10000.
 */
static int oracle_create_write(void)
{
    unsigned char want[10100];

    __builtin_memcpy(want, pattern, 100);
    __builtin_memcpy(want + 100, pattern, 10000);
    return file_is("/new.txt", want, 10100);
}

/** The index in the write log of the first write that covers block `blk`
 * of a 4 KiB-block volume, or -1. */
static int first_write_of(uint64_t blk)
{
    int i;

    for (i = 0; i < wlog_n; i++) {
        uint64_t from = wlog_lba[i] / 8, to = (wlog_lba[i] + wlog_count[i] - 1) / 8;

        if (blk >= from && blk <= to) {
            return i;
        }
    }
    return -1;
}

/** The index in the write log of the last write that covers block `blk`,
 * or -1: a pointer block is written twice by a write that makes it, its
 * zeros first with the bitmaps and its pointers after them. */
static int last_write_of(uint64_t blk)
{
    int i, at = -1;

    for (i = 0; i < wlog_n; i++) {
        uint64_t from = wlog_lba[i] / 8, to = (wlog_lba[i] + wlog_count[i] - 1) / 8;

        if (blk >= from && blk <= to) {
            at = i;
        }
    }
    return at;
}

/**
 * Store in `out`, at most `cap` of them, every number that follows `key` in
 * the output of the debugfs command `cmd` on the write rows' volume; the
 * count found. The output goes through a file, since the test is compiled
 * freestanding and declares the few host calls it makes.
 */
static int debugfs_numbers(const char *cmd, const char *key, uint64_t *out, int cap)
{
    char line[512];
    int n = 0;
    void *f;

    ext2_fixture_debugfs(wimg, cmd, 0, fix_path("debugfs.txt"));
    f = fopen(fix_path("debugfs.txt"), "r");
    if (!f) {
        return 0;
    }
    while (fgets(line, sizeof(line), f)) {
        const char *at = line;

        while ((at = strstr(at, key)) != 0) {
            at += strlen(key);
            if (n < cap) {
                out[n++] = strtoul(at, 0, 10);
            }
        }
    }
    fclose(f);
    return n;
}

/**
 * Store in `out`, at most `cap` of them, the run of numbers that follows
 * the first `key` in the output of the debugfs command `cmd`: what `ffb`
 * prints, a list on one line. The output is read whole, since the list is
 * longer than any line buffer and a number cut at a buffer's end would be
 * another number. Returns the count, or 0 when the output fills the
 * 64 KiB buffer, rather than a cut list.
 */
static int debugfs_list(const char *cmd, const char *key, uint64_t *out, int cap)
{
    static char text[65536];
    unsigned long got;
    const char *at;
    int n = 0;
    void *f;

    ext2_fixture_debugfs(wimg, cmd, 0, fix_path("debugfs.txt"));
    f = fopen(fix_path("debugfs.txt"), "r");
    if (!f) {
        return 0;
    }
    got = fread(text, 1, sizeof(text) - 1, f);
    fclose(f);
    text[got] = '\0';
    if (got == sizeof(text) - 1) {
        return 0;
    }
    at = strstr(text, key);
    if (!at) {
        return 0;
    }
    at += strlen(key);
    for (;;) {
        char *end;
        unsigned long v;

        while (*at == ' ' || *at == '\n') {
            at++;
        }
        v = strtoul(at, &end, 10);
        if (end == at) {
            break;
        }
        if (n < cap) {
            out[n++] = v;
        }
        at = end;
    }
    return n;
}

/**
 * Fill up to 3000 free blocks of the open image, from block `from` on, with
 * bytes 0xAB, which are neither zeros nor pointers into the volume, so that
 * a pointer block allocated from them holds stale contents until the driver
 * writes its zeros, as a used disk's would. A pointer published over those
 * contents is then visible. Returns the count of blocks, or -1.
 */
static int seed_free_blocks(uint64_t from)
{
    static uint64_t freeb[3000];
    static unsigned char junk[4096];
    char cmd[200];
    int n, i;

    snprintf(cmd, sizeof(cmd), "ffb 3000 %lu", (unsigned long)from);
    n = debugfs_list(cmd, "Free blocks found:", freeb, 3000);
    memset(junk, 0xAB, sizeof(junk));
    for (i = 0; i < n; i++) {
        if (fseek(image, (long)(freeb[i] * 4096), SEEK_SET_) != 0 ||
            fwrite(junk, 1, sizeof(junk), image) != sizeof(junk)) {
            return -1;
        }
    }
    return n;
}

/** Whether block `q` is allocated in the shadow's bitmap of group 0. */
static int shadow_allocated(const unsigned char *shadow, uint64_t bitmap_blk, uint64_t q,
                            uint64_t blocks)
{
    const unsigned char *bm = shadow + bitmap_blk * 4096;

    if (q == 0 || q >= blocks) {
        return 0;
    }
    return (bm[q / 8] >> (q % 8)) & 1;
}

/**
 * Whether every nonzero pointer in the shadow's block `blk` names an
 * allocated block; 0 with the first that does not in `*bad`.
 */
static int shadow_pointers_ok(const unsigned char *shadow, uint64_t blk, uint64_t bitmap_blk,
                              uint64_t blocks, uint64_t *bad)
{
    const unsigned char *p = shadow + blk * 4096;
    int i;

    for (i = 0; i < 1024; i++) {
        uint64_t q = (uint64_t)p[i * 4] | ((uint64_t)p[i * 4 + 1] << 8) |
                     ((uint64_t)p[i * 4 + 2] << 16) | ((uint64_t)p[i * 4 + 3] << 24);

        if (q != 0 && !shadow_allocated(shadow, bitmap_blk, q, blocks)) {
            *bad = q;
            return 0;
        }
    }
    return 1;
}

/* The file whose appended bytes `batch_size_ok` judges: its inode, 0 when
   no row judges one; its first appended logical block; whether a missing
   pointer is a hole allowed; the count of blocks; and the bytes appended. */
static uint32_t batch_publication_ino;
static uint32_t batch_publication_first = 1035;
static int batch_publication_hole;
static unsigned batch_publication_blocks = 32;
static const unsigned char *batch_publication_data;

/** The little-endian 32-bit field at `p`. */
static uint32_t batch_word(const unsigned char *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/**
 * Whether, in the image `shadow` of `blocks` blocks, every appended block
 * below the file's size there holds its bytes, found through the inode's
 * pointers as they stand in that image. The image has 4 KiB blocks,
 * 128-byte inodes and its group descriptors at block 1.
 */
static int batch_size_ok(const unsigned char *shadow, uint64_t blocks)
{
    uint32_t per_group = batch_word(shadow + 1024 + 40);
    uint32_t group = (batch_publication_ino - 1) / per_group;
    uint32_t table = batch_word(shadow + 4096 + group * 32 + 8);
    uint32_t index = (batch_publication_ino - 1) % per_group;
    const unsigned char *inode = shadow + table * 4096 + index * 128;
    uint32_t size = batch_word(inode + 4);
    unsigned i;

    for (i = 0; i < batch_publication_blocks && size > (batch_publication_first + i) * 4096; i++) {
        uint32_t logical = batch_publication_first + i;
        uint32_t block;

        if (logical < 12) {
            block = batch_word(inode + 40 + logical * 4);
        } else if (logical < 1036) {
            block = batch_word(inode + 40 + 12 * 4);
            if (!block && batch_publication_hole) {
                continue;
            }
            if (!block || block >= blocks) {
                return 0;
            }
            block = batch_word(shadow + block * 4096 + (logical - 12) * 4);
        } else {
            uint32_t rest = logical - 1036;

            block = batch_word(inode + 40 + 13 * 4);
            if (!block && batch_publication_hole) {
                continue;
            }
            if (!block || block >= blocks) {
                return 0;
            }
            block = batch_word(shadow + block * 4096 + (rest / 1024) * 4);
            if (!block && batch_publication_hole) {
                continue;
            }
            if (!block || block >= blocks) {
                return 0;
            }
            block = batch_word(shadow + block * 4096 + (rest % 1024) * 4);
        }
        if (!block && batch_publication_hole) {
            continue;
        }
        if (!block || block >= blocks ||
            memcmp(shadow + block * 4096, batch_publication_data + i * 4096, 4096) != 0) {
            return 0;
        }
    }
    return 1;
}

/**
 * Replay the captured writes on `shadow`, a snapshot of the disk taken
 * before them, and judge the disk after each. At each write that carries
 * one of the pointer blocks `ptr`, every block it names must be allocated
 * in the bitmap at `bitmap_blk` as it stands then, and a named pointer
 * block's own contents, as they stand then, must be zeros or pointers to
 * allocated blocks: the zeros written first are there so that no pointer
 * is published over stale bytes. While a batch row judges a file, its size
 * must also cover only bytes on the disk. Returns whether every write
 * passed; each failure is printed.
 */
static int publication_ok(unsigned char *shadow, uint64_t blocks, const uint64_t *ptr, int nptr,
                          uint64_t bitmap_blk)
{
    int k, i, ok = 1;

    for (k = 0; k < wlog_n; k++) {
        uint64_t from = wlog_lba[k] / 8, to = (wlog_lba[k] + wlog_count[k] - 1) / 8;

        if (!wlog_data[k]) {
            printf("  write %d was not captured\n", k);
            return 0;
        }
        __builtin_memcpy(shadow + wlog_lba[k] * 512, wlog_data[k], wlog_count[k] * 512);
        if (batch_publication_ino && !batch_size_ok(shadow, blocks)) {
            printf("  write %d publishes a size before its appended bytes\n", k);
            ok = 0;
        }
        for (i = 0; i < nptr; i++) {
            const unsigned char *p;
            uint64_t bad = 0;
            int e;

            if (ptr[i] < from || ptr[i] > to) {
                continue;
            }
            if (!shadow_pointers_ok(shadow, ptr[i], bitmap_blk, blocks, &bad)) {
                int m;

                printf("  write %d publishes pointer block %lu naming block %lu, not allocated\n",
                       k, (unsigned long)ptr[i], (unsigned long)bad);
                for (m = 0; m <= k; m++) {
                    uint64_t f2 = wlog_lba[m] / 8, t2 = (wlog_lba[m] + wlog_count[m] - 1) / 8;

                    if ((bitmap_blk >= f2 && bitmap_blk <= t2) || (ptr[i] >= f2 && ptr[i] <= t2)) {
                        printf("    write %d: blocks %lu to %lu%s\n", m, (unsigned long)f2,
                               (unsigned long)t2,
                               bitmap_blk >= f2 && bitmap_blk <= t2 ? " (the bitmap)" : "");
                    }
                }
                ok = 0;
                continue;
            }
            p = shadow + ptr[i] * 4096;
            for (e = 0; e < 1024; e++) {
                uint64_t q = (uint64_t)p[e * 4] | ((uint64_t)p[e * 4 + 1] << 8) |
                             ((uint64_t)p[e * 4 + 2] << 16) | ((uint64_t)p[e * 4 + 3] << 24);
                int j;

                if (q == 0) {
                    continue;
                }
                for (j = 0; j < nptr; j++) {
                    if (ptr[j] == q && !shadow_pointers_ok(shadow, q, bitmap_blk, blocks, &bad)) {
                        printf("  write %d publishes pointer block %lu naming pointer block %lu "
                               "whose contents on the disk name block %lu\n",
                               k, (unsigned long)ptr[i], (unsigned long)q, (unsigned long)bad);
                        ok = 0;
                    }
                }
            }
        }
    }
    return ok;
}

/**
 * Whether the image `media`, with write `write_index` applied, passes: the
 * batch file's size covers only its bytes, and each pointer block in `ptr`
 * that the write covers, and each pointer block it names, points only at
 * allocated blocks.
 */
static int media_write_ok(const unsigned char *media, uint64_t blocks, const uint64_t *ptr,
                          int nptr, uint64_t bitmap_blk, int write_index)
{
    uint64_t first = wlog_lba[write_index] / 8;
    uint64_t last = (wlog_lba[write_index] + wlog_count[write_index] - 1) / 8;
    int i, j, e;

    if (batch_publication_ino && !batch_size_ok(media, blocks)) {
        return 0;
    }
    for (i = 0; i < nptr; i++) {
        uint64_t bad;

        if (ptr[i] < first || ptr[i] > last) {
            continue;
        }
        if (!shadow_pointers_ok(media, ptr[i], bitmap_blk, blocks, &bad)) {
            return 0;
        }
        for (e = 0; e < 1024; e++) {
            uint32_t child = batch_word(media + ptr[i] * 4096 + e * 4);

            for (j = 0; child && j < nptr; j++) {
                if (ptr[j] == child &&
                    !shadow_pointers_ok(media, child, bitmap_blk, blocks, &bad)) {
                    return 0;
                }
            }
        }
    }
    return 1;
}

/**
 * Replay the captured writes between the recorded flushes over a separate
 * copy of the medium, `before`, as a disk with a volatile cache may make
 * them durable. Within an interval, each write may persist alone before the
 * older ones, and so may the newest one, two and so on up to all of them; a
 * flush then makes the whole interval durable. Each such state must pass
 * `media_write_ok`. Writes are atomic; a torn sector and the other subsets
 * of three or more pending writes are outside this model. Returns whether
 * every state passed, and prints the count checked.
 */
static int media_publication_ok(const unsigned char *before, uint64_t blocks, const uint64_t *ptr,
                                int nptr, uint64_t bitmap_blk)
{
    unsigned long bytes = (unsigned long)(blocks * 4096);
    unsigned char *durable = malloc(bytes), *trial = malloc(bytes);
    int interval, begin = 0, end, k, ok = 1, states = 0;

    if (!durable || !trial) {
        free(durable);
        free(trial);
        return 0;
    }
    __builtin_memcpy(durable, before, bytes);
    for (interval = 0; interval <= flush_n && ok; interval++) {
        end = interval < flush_n ? flush_log[interval] : wlog_n;
        if (end < begin || end > wlog_n) {
            ok = 0;
            break;
        }
        for (k = begin; k < end && ok; k++) {
            if (!wlog_data[k]) {
                ok = 0;
                break;
            }
            __builtin_memcpy(trial, durable, bytes);
            __builtin_memcpy(trial + wlog_lba[k] * 512, wlog_data[k], wlog_count[k] * 512);
            ok = media_write_ok(trial, blocks, ptr, nptr, bitmap_blk, k);
            states++;
        }
        __builtin_memcpy(trial, durable, bytes);
        for (k = end - 1; k >= begin && ok; k--) {
            __builtin_memcpy(trial + wlog_lba[k] * 512, wlog_data[k], wlog_count[k] * 512);
            ok = media_write_ok(trial, blocks, ptr, nptr, bitmap_blk, k);
            states++;
        }
        if (!ok) {
            printf("ext2-media: interval %d, writes %d through %d violate publication order\n",
                   interval, begin, end - 1);
            break;
        }
        for (k = begin; k < end; k++) {
            __builtin_memcpy(durable + wlog_lba[k] * 512, wlog_data[k], wlog_count[k] * 512);
        }
        begin = end;
    }
    printf("ext2-media: result: %d boundaries, %d durable states checked\n", flush_n, states);
    free(durable);
    free(trial);
    return ok;
}

/**
 * Append 32 blocks from logical block `first` through `handle`, one call
 * per block, and check that the calls write nothing to the device, that
 * the pending bytes read back, and that an fsync succeeds. Returns 0, or -1
 * after a short write.
 */
static int order_collect(void *handle, const unsigned char *buf, uint32_t first)
{
    int before = writes_made;
    int i, rc = 0;

    for (i = 0; i < 32; i++) {
        int32_t got =
            OPS->write(&mnt, handle, buf + i * 4096, 4096, ((uint64_t)first + (unsigned)i) * 4096);

        check(got == 4096, "a cross-call append returns its full block");
        if (got != 4096) {
            rc = -1;
            break;
        }
    }
    check(writes_made == before, "cross-call publication collection writes no blocks");
    check(OPS->read(&mnt, handle, abuf, 32 * 4096, (uint64_t)first * 4096) == 32 * 4096 &&
              memcmp(abuf, buf, 32 * 4096) == 0,
          "pending appends across indirect levels read their bytes");
    check(OPS->fsync(&mnt, handle) == 0, "the cross-call publication batch syncs");
    return rc;
}

/**
 * Check that the last write of each pointer block in `ptr`, the one with
 * its pointers, comes after the first write of every block bitmap in `bm`
 * and of the group descriptors, block 1 at this block size.
 */
static void order_allocations(const uint64_t *ptr, int nptr, const uint64_t *bm, int nbm)
{
    int i, j, ok = 1;

    for (i = 0; i < nptr; i++) {
        int at = last_write_of(ptr[i]);

        if (at < 0) {
            printf("  pointer block %lu was never written\n", (unsigned long)ptr[i]);
            ok = 0;
            continue;
        }
        for (j = 0; j < nbm; j++) {
            int bat = first_write_of(bm[j]);

            if (bat >= 0 && bat > at) {
                printf("  pointer block %lu written at %d, bitmap %lu at %d\n",
                       (unsigned long)ptr[i], at, (unsigned long)bm[j], bat);
                ok = 0;
            }
        }
        if (first_write_of(1) > at) {
            printf("  pointer block %lu written at %d, the descriptors at %d\n",
                   (unsigned long)ptr[i], at, first_write_of(1));
            ok = 0;
        }
    }
    check(ok, "every pointer block's pointers reached the disk after the bitmaps and descriptors "
              "of its write");
}

/**
 * Write thirty-two blocks from logical block `first` of a file under a
 * cache of `buffers` buffers, and judge the order of the writes that reach
 * the disk. A pointer block the cache evicts must not reach the disk before
 * the bitmap whose bit marks the block it names: the last write of every
 * pointer block, the one with its pointers (its first is the zeros that go
 * with the bitmaps), must come after the first write of every block bitmap
 * and of the descriptors. From block 1035 the write crosses from the single
 * into the double indirection; with sixteen buffers the write's pointer
 * blocks are evicted while it runs. The free blocks hold stale bytes, and
 * every pointer published must name an allocated block whose contents on
 * the disk are not stale. With `cross_calls`, the file is first truncated
 * to `first` blocks and the blocks are appended one call each through one
 * description (`order_collect`), and the replay over a volatile medium
 * must hold as well.
 */
static void order_row_under(uint32_t buffers, int cross_calls, uint32_t first)
{
    static unsigned char buf[32 * 4096];
    uint64_t ptr[8], bm[8];
    int nptr, nbm, i, rc;
    uint32_t freed = 0;
    void *f;
    unsigned char *shadow;
    void *handle = 0;
    struct vfs_stat st;

    for (i = 0; i < (int)sizeof(buf); i++) {
        buf[i] = (unsigned char)(i * 13 + 1);
    }
    wbytes = build_image(wimg, 4096, 128, PROFILE, 16, "tree");
    use_image(wimg, wbytes);
    if (bcache_resident() > buffers) {
        bcache_shrink(bcache_resident() - buffers, 0, &freed);
    }
    bcache_set_limit(buffers);
    check(bcache_resident() <= buffers, "the cache is cut for the sparse write");
    stop_after = -1;
    check(seed_free_blocks(0) > 0, "the free blocks are seeded with stale bytes");
    rc = mount_rw();
    check(rc == 0 && (mnt.flags & VFS_MS_RDONLY) == 0, "the order row's volume mounts for writing");
    f = rc == 0 ? make_file(mnt.root, "ordered", &rc) : 0;
    check(f != 0, "a file for the sparse write");
    if (!f) {
        printf("  rc %d; last warning '%s'; last check '%s'; last log '%s'\n", rc, last_warn,
               last_check, last_log);
    }
    if (f && cross_calls) {
        check(OPS->open(&mnt, f, 2, &handle) == 0, "the batch publication handle opens");
        check(handle && OPS->truncate(&mnt, handle, (uint64_t)first * 4096) == 0,
              "a sparse prefix precedes contiguous batch appends");
        check(OPS->getattr(&mnt, f, &st) == 0, "the publication inode is known");
        batch_publication_ino = (uint32_t)st.ino;
        batch_publication_first = first;
        batch_publication_data = buf;
    }
    shadow = malloc(wbytes);
    check(shadow != 0 && fseek(image, 0, SEEK_SET_) == 0 &&
              fread(shadow, 1, wbytes, image) == wbytes,
          "a snapshot of the disk before the write");
    wlog_reset();
    wlog_capture = 1;
    if (f) {
        if (cross_calls && handle) {
            rc = order_collect(handle, buf, first);
            OPS->release(&mnt, handle);
        } else {
            rc = write_at(f, buf, sizeof(buf), (uint64_t)first * 4096);
        }
        check(rc == 0, "thirty-two blocks from logical block 1035, written in one call");
        if (rc != 0) {
            printf("  rc %d\n", rc);
        }
        OPS->put(&mnt, f);
    }
    check(unmount_rw() == 0, "the order row's volume unmounts");
    bcache_set_limit(0);
    nptr = debugfs_numbers("stat /ordered", "(IND):", ptr, 4);
    nptr += debugfs_numbers("stat /ordered", "(DIND):", ptr + nptr, 4);
    nbm = debugfs_numbers("stats", "block bitmap at ", bm, 8);
    check(nptr >= (first < 1000 ? 1 : 2) && nbm >= 1,
          "debugfs names the file's pointer blocks and the volume's bitmaps");
    order_allocations(ptr, nptr, bm, nbm);
    check(!wlog_overflow, "the write log held every write of the row");
    if (cross_calls) {
        check(shadow != 0 && nbm >= 1 &&
                  media_publication_ok(shadow, wbytes / 4096, ptr, nptr, bm[0]),
              "volatile media preserves pointer and size dependencies");
    }
    check(shadow != 0 && nbm >= 1 && publication_ok(shadow, wbytes / 4096, ptr, nptr, bm[0]),
          "every pointer published names an allocated block whose contents on the disk are "
          "not stale");
    batch_publication_ino = 0;
    batch_publication_data = 0;
    wlog_capture = 0;
    wlog_reset();
    free(shadow);
    check(e2fsck_clean(wimg), "and the volume is clean");
}

/**
 * A mount without `sync` or `dirsync` writes back as Linux's ext2 does:
 * creates, writes and a remove stay dirty in the cache and reach the disk
 * not at all; an fsync writes them and flushes the disk, and the volume
 * unmounts clean. The device is marked write-through, so that a write sent
 * without `BLOCK_REQ_NOFLUSH` would count as a flush; the removed file's
 * inode and blocks are freed at its last put.
 */
static void default_mount_row(void)
{
    static unsigned char block[3 * 4096];
    void *f, *g;
    int rc, flushes;

    wbytes = build_image(wimg, 4096, 128, PROFILE, 16, "tree");
    use_image(wimg, wbytes);
    rw_flags = 0;
    rc = mount_rw();
    rw_flags = VFS_MS_DIRSYNC;
    check(rc == 0 && (mnt.flags & VFS_MS_RDONLY) == 0, "a mount without dirsync mounts writable");
    if (rc != 0) {
        return;
    }
    wlog_reset();
    wlog_capture = 1;
    dev.flags |= BLOCK_WRITE_THROUGH;
    memset(block, 0x5a, sizeof(block));
    f = make_file(mnt.root, "plain", &rc);
    g = f ? make_file(mnt.root, "gone", &rc) : 0;
    check(f && g && write_at(f, block, sizeof(block), 0) == 0 &&
              write_at(g, block, sizeof(block), 0) == 0 && OPS->unlink(&mnt, mnt.root, "gone") == 0,
          "two creates, writes of three blocks to each and the remove of one");
    if (g) {
        OPS->put(&mnt, g);
    }
    check(flush_n == 0 && wlog_n == 0, "stay in the cache on a mount without dirsync");
    flushes = flush_n;
    check(f && OPS->fsync(&mnt, 0) == 0 && wlog_n > 0 && flush_n > flushes,
          "and an fsync writes them and flushes the disk");
    if (f) {
        OPS->put(&mnt, f);
    }
    wlog_capture = 0;
    dev.flags &= ~BLOCK_WRITE_THROUGH;
    check(unmount_rw() == 0 && e2fsck_clean(wimg), "and the volume unmounts clean");
}

/**
 * An rmdir of a directory a holder has: the name goes at once, a listing of
 * the directory and a directory made in it are -ENOENT, and the inode goes
 * at the last put, leaving the volume clean.
 */
static void held_rmdir_row(void)
{
    struct vfs_dirent ent;
    uint32_t cursor = 0;
    int err;
    void *d, *gone, *h = 0;

    wbytes = build_image(wimg, 4096, 128, PROFILE, 16, "tree");
    use_image(wimg, wbytes);
    check(mount_rw() == 0, "the held directory's volume mounts writable");
    d = OPS->mkdir(&mnt, mnt.root, "held", 0755) == 0 ? walk("/held", &err) : 0;
    check(d != 0, "a directory, made and held");
    if (!d) {
        return;
    }
    check(OPS->rmdir(&mnt, mnt.root, "held") == 0, "an rmdir of the held directory succeeds");
    gone = walk("/held", &err);
    check(!gone && err == -ENOENT, "and its name is gone");
    check(OPS->open(&mnt, d, 0, &h) == 0 && OPS->readdir(&mnt, h, &cursor, &ent) == -ENOENT &&
              OPS->mkdir(&mnt, d, "in", 0755) == -ENOENT,
          "and the removed directory lists as ENOENT and takes no entry");
    if (h) {
        OPS->release(&mnt, h);
    }
    OPS->put(&mnt, d);
    check(unmount_rw() == 0 && e2fsck_clean(wimg),
          "and its inode goes at the last put, leaving the volume clean");
}

/* The bit of `i_flags` that marks a directory's hashed index. */
#define INDEX_FL 0x1000u

/** Whether the directory at `path` is marked indexed: 1 or 0, or -1 when
 * the lookup or getattr fails. */
static int index_flag(const char *path)
{
    struct vfs_stat st;
    int err, rc;
    void *n = walk(path, &err);

    if (!n) {
        return -1;
    }
    rc = OPS->getattr(&mnt, n, &st);
    OPS->put(&mnt, n);
    return rc != 0 ? -1 : (st.flags & INDEX_FL) != 0;
}

/* The changes the dir_index rows make, each to an indexed /many; each
   returns 0 or the first error. */

/** Create added.txt in /many. */
static int dx_create(void)
{
    int err, rc;
    void *d = walk("/many", &err);
    void *n = d ? make_file(d, "added.txt", &rc) : 0;

    if (d) {
        OPS->put(&mnt, d);
    }
    if (n) {
        OPS->put(&mnt, n);
    }
    return d ? (n ? 0 : rc) : err;
}

/** Remove file7.txt from /many. */
static int dx_unlink(void)
{
    int err, rc;
    void *d = walk("/many", &err);

    if (!d) {
        return err;
    }
    rc = OPS->unlink(&mnt, d, "file7.txt");
    OPS->put(&mnt, d);
    return rc;
}

/** Rename file8.txt over file9.txt in /many, which points the victim's
 * entry at the file in place, as Linux's ext2_set_link does, before the old
 * entry goes. */
static int dx_rename_over(void)
{
    int err, rc;
    void *d = walk("/many", &err);

    if (!d) {
        return err;
    }
    rc = OPS->rename(&mnt, d, "file8.txt", d, "file9.txt");
    OPS->put(&mnt, d);
    return rc;
}

/** Move /many into /dir, which rewrites its `..`. */
static int dx_move(void)
{
    int err, rc;
    void *d = walk("/dir", &err);

    if (!d) {
        return err;
    }
    rc = OPS->rename(&mnt, mnt.root, "many", d, "many");
    OPS->put(&mnt, d);
    return rc;
}

/**
 * Make the directory at `path` indexed with `e2fsck -fyD`, mount the volume
 * writable, run `change`, and require that the directory is no longer
 * marked indexed and that the volume unmounts clean under `e2fsck -fn`,
 * which reads every entry and would find an index left stale. `after` is
 * the directory's path once `change` has run, `what` the row's claim.
 */
static void dir_index_change(const char *path, int (*change)(void), const char *after,
                             const char *what)
{
    char request[300];
    int st = ext2_fixture_fsck(wimg, "-fyD", fix_path("fsck.out"));

    snprintf(request, sizeof request, "stat %s", path);
    check(st >= 0 && st <= 1 && debugfs_says(wimg, request, "Flags: 0x1000"),
          "e2fsck -D indexes the directory");
    check(mount_rw() == 0 && (mnt.flags & VFS_MS_RDONLY) == 0 && index_flag(path) == 1,
          "the dir_index volume mounts writable, the directory marked indexed");
    if (!mnt.root) {
        return;
    }
    check(change() == 0 && index_flag(after) == 0, what);
    check(unmount_rw() == 0 && e2fsck_clean(wimg), "and the volume is clean under e2fsck");
}

/**
 * A dir_index volume mounts writable, as Linux's ext2 mounts it, since the
 * feature is compatible: e2fsck -D indexes /many, of 300 entries, before
 * each change, and a create, a remove, a rename over a name and a move of
 * the directory each clear its index flag and leave the volume clean. The
 * names read back through debugfs and setidcheck's reader at the end.
 */
static void dir_index_row(void)
{
    wbytes = build_image(wimg, 4096, 128, PROFILE_DIR_INDEX, 16, "tree");
    use_image(wimg, wbytes);
    dir_index_change("/many", dx_create, "/many",
                     "a create in an indexed directory clears its index flag");
    dir_index_change("/many", dx_unlink, "/many",
                     "a remove from an indexed directory clears its index flag");
    dir_index_change("/many", dx_rename_over, "/many",
                     "a rename over a name of an indexed directory clears its index flag");
    dir_index_change("/many", dx_move, "/dir/many",
                     "a move of an indexed directory to another parent clears its index flag");
    check(debugfs_lists(wimg, "/dir/many", "added.txt") &&
              debugfs_lists(wimg, "/dir/many", "file9.txt") &&
              !debugfs_lists(wimg, "/dir/many", "file7.txt") &&
              !debugfs_lists(wimg, "/dir/many", "file8.txt") &&
              debugfs_lists(wimg, "/dir/many", "file299.txt") && reader_walks(wimg),
          "and debugfs and the reader see the changed names");
}

/* The volume block that holds /many's inode, whose first read the
   failed-store row fails, or -1 when the row is not failing one. */
static int64_t dx_inode_block = -1;

/** Fail the read that covers `dx_inode_block`, once. */
static void dx_fail_inode_read(void)
{
    if (dx_inode_block >= 0 && (uint64_t)dx_inode_block >= request_block &&
        (uint64_t)dx_inode_block < request_block + request_blocks) {
        read_pause = 0;
        dx_inode_block = -1;
        read_fails = 1;
    }
}

/** The volume block of `img` that holds the inode of `path`, by debugfs's
 * `imap`, or -1. */
static int64_t inode_block_of(const char *img, const char *path)
{
    char request[300], line[300];
    const char *at;
    int64_t found = -1;
    void *f;

    snprintf(request, sizeof request, "imap %s", path);
    if (!ext2_fixture_debugfs(img, request, 0, fix_path("debugfs.txt"))) {
        return -1;
    }
    f = fopen(fix_path("debugfs.txt"), "r");
    while (f && fgets(line, sizeof line, f)) {
        at = strstr(line, "located at block ");
        if (at) {
            found = (int64_t)strtoul(at + 17, 0, 10);
        }
    }
    if (f) {
        fclose(f);
    }
    return found;
}

/**
 * Bring the failed-store row's volume to its retry: /many indexed by
 * `e2fsck -fyD` and mounted writable, /many and its file7.txt held, the
 * cache emptied, and an unlink of file7.txt whose store of the cleared
 * index flag fails at the read of /many's inode-table block, before any
 * write. The held directory must keep its flag in memory, so that the
 * retry stores the cleared flag again. `*d` and `*c` are left held, or
 * NULL; 1, or 0 when the volume did not come to the retry. With `judge`
 * the claims are checked, which the first run does.
 */
static int dx_failed_store(void **d, void **c, int judge)
{
    int64_t block;
    int st, err, rc;

    *d = *c = 0;
    wbytes = build_image(wimg, 4096, 128, PROFILE_DIR_INDEX, 16, "tree");
    use_image(wimg, wbytes);
    st = ext2_fixture_fsck(wimg, "-fyD", fix_path("fsck.out"));
    block = inode_block_of(wimg, "/many");
    if (judge) {
        check(st >= 0 && st <= 1 && debugfs_says(wimg, "stat /many", "Flags: 0x1000") && block > 0,
              "e2fsck -D indexes the failed-store row's directory");
    }
    stop_after = -1;
    if (block <= 0 || mount_rw() != 0) {
        check(0, "the failed-store row's volume mounts writable");
        return 0;
    }
    *d = walk("/many", &err);
    *c = walk("/many/file7.txt", &err);
    if (!*d || !*c) {
        check(0, "the failed-store row's directory and file are held");
        return 0;
    }
    check(OPS->sync(&mnt) == 0 && bcache_invalidate(&dev) == 0,
          "the failed-store row's cache is emptied");
    dx_inode_block = block;
    read_pause = dx_fail_inode_read;
    rc = OPS->unlink(&mnt, *d, "file7.txt");
    read_pause = 0;
    if (judge) {
        check(rc == -EIO && dx_inode_block == -1 && (mnt.flags & VFS_MS_RDONLY) == 0,
              "an unlink whose index-flag store fails at the inode-table read fails with EIO, "
              "the mount writable");
        check(index_flag("/many") == 1,
              "and the held directory keeps its index flag in memory for the retry");
    }
    dx_inode_block = -1;
    return rc == -EIO;
}

/** The failed-store row's retry: the unlink of file7.txt again, then a
 * create of added.txt, which goes into the slack of `..` that holds the
 * index root. 0, or the first error. */
static int dx_retry(void *d)
{
    int rc = OPS->unlink(&mnt, d, "file7.txt");
    void *n = rc == 0 ? make_file(d, "added.txt", &rc) : 0;

    if (n) {
        OPS->put(&mnt, n);
    }
    return rc;
}

/**
 * A failed store of a directory's cleared index flag leaves the flag set in
 * memory, so that a later change stores it again before any entry changes.
 * The retry, an unlink and a create, runs once whole to count its writes,
 * and then on a fresh volume for each count k, stopped after write k: at
 * every stop, a /many still marked indexed on the disk must hold its
 * entries as e2fsck -D left them, file7.txt and no added.txt.
 */
static void dir_index_failed_store_row(void)
{
    void *d, *c;
    int k, n, rc;
    char what[200];

    if (!dx_failed_store(&d, &c, 1)) {
        if (c) {
            OPS->put(&mnt, c);
        }
        if (d) {
            OPS->put(&mnt, d);
        }
        unmount_image();
        return;
    }
    writes_made = 0;
    rc = dx_retry(d);
    n = writes_made;
    check(rc == 0 && index_flag("/many") == 0,
          "the retry changes the directory and clears its flag");
    OPS->put(&mnt, c);
    OPS->put(&mnt, d);
    check(unmount_rw() == 0 && !debugfs_says(wimg, "stat /many", "Flags: 0x1000") &&
              !debugfs_lists(wimg, "/many", "file7.txt") &&
              debugfs_lists(wimg, "/many", "added.txt"),
          "and debugfs sees the flag cleared and the changed names");
    for (k = 0; k < n; k++) {
        if (!dx_failed_store(&d, &c, 0)) {
            if (c) {
                OPS->put(&mnt, c);
            }
            if (d) {
                OPS->put(&mnt, d);
            }
            unmount_image();
            snprintf(what, sizeof what, "the failed store before stop %d of %d fails", k, n);
            check(0, what);
            return;
        }
        writes_made = 0;
        stop_after = k;
        (void)dx_retry(d);
        OPS->put(&mnt, c);
        OPS->put(&mnt, d);
        OPS->put(&mnt, mnt.root);
        OPS->unmount(&mnt);
        mnt.root = 0;
        bcache_device_gone(&dev);
        dev.flags = BLOCK_FAULT_SAFE;
        stop_after = -1;
        use_image(wimg, wbytes);
        snprintf(what, sizeof what,
                 "the retry stopped after write %d of %d leaves no changed directory "
                 "marked indexed",
                 k, n);
        check(!debugfs_says(wimg, "stat /many", "Flags: 0x1000") ||
                  (debugfs_lists(wimg, "/many", "file7.txt") &&
                   !debugfs_lists(wimg, "/many", "added.txt")),
              what);
    }
}

/**
 * A remount read-only syncs the volume and leaves it as an unmount does, a
 * create on it fails with `EROFS`, and a remount writable takes a create
 * again; a file removed while open keeps the mount writable, and the
 * remount fails with `EBUSY`.
 */
static void remount_row(void)
{
    unsigned char sb[2];
    int rc;
    void *f, *g;

    wbytes = build_image(wimg, 4096, 128, PROFILE, 16, "tree");
    use_image(wimg, wbytes);
    check(mount_rw() == 0, "the remount volume mounts writable");
    f = make_file(mnt.root, "before", &rc);
    check(f && write_at(f, pattern, 5000, 0) == 0, "a file written before the remount");
    if (f) {
        OPS->put(&mnt, f);
    }
    g = make_file(mnt.root, "open", &rc);
    check(g && OPS->unlink(&mnt, mnt.root, "open") == 0 &&
              OPS->remount(&mnt, rw_flags | VFS_MS_RDONLY, NULL) == -EBUSY,
          "a remount read-only with a removed file still open is EBUSY");
    if (g) {
        OPS->put(&mnt, g);
    }
    check(OPS->remount(&mnt, rw_flags | VFS_MS_RDONLY, NULL) == 0 &&
              host_read(wimg, sb, 2, 1024 + 58) == 2 && (sb[0] & 1) == 1 && e2fsck_clean(wimg),
          "a remount read-only sets the valid bit and leaves the volume clean, as an unmount "
          "does");
    mnt.flags = rw_flags | VFS_MS_RDONLY;
    check(make_file(mnt.root, "refused", &rc) == 0 && rc == -EROFS, "and a create on it is EROFS");
    check(OPS->remount(&mnt, rw_flags, NULL) == 0 && host_read(wimg, sb, 2, 1024 + 58) == 2 &&
              (sb[0] & 1) == 0,
          "a remount writable clears the valid bit again");
    mnt.flags = rw_flags;
    f = make_file(mnt.root, "after", &rc);
    check(f != 0, "and takes a create");
    if (f) {
        OPS->put(&mnt, f);
    }
    check(unmount_rw() == 0 && e2fsck_clean(wimg), "and the volume unmounts clean");
}

/**
 * Run `order_row_under` twice from block 1035: with the cache cut to
 * sixteen buffers, so the write's blocks are evicted as it runs in the
 * order it touched them; and with room for them all, so they go to the
 * disk at the unmount's sync in block order, which puts a parent pointer
 * block before the child it names. Each order is one a machine can
 * produce, and the publication check must hold in both.
 */
static void order_row(void)
{
    order_row_under(16, 0, 1035);
    order_row_under(64, 0, 1035);
}

/** Count a pending pair without fetching or draining a disk block. */
static void cache_pending_pair(void *node)
{
    struct vfs_cachestat cs;
    uint64_t waits = current_proc->disk_waits;
    int writes = writes_made;

    memset(&cs, 0, sizeof(cs));
    OPS->cachestat(&mnt, node, 1, 2, &cs);
    check(cs.cache == 2 && cs.dirty == 2,
          "cachestat counts both pending data pages as cached and dirty");
    check(current_proc->disk_waits == waits && writes_made == writes,
          "cachestat performs no disk request and drains no pending write");
}

/**
 * Keep a description open across calls under a cache of `buffers` buffers,
 * and check that the writes it collects reach the device only when
 * drained: sixteen rewrites of one block cost one data write and one inode
 * write at fsync; two appends write nothing until the close; and a
 * setattr, a rename and a truncate each drain a pending append.
 */
static void batch_rows_under(uint32_t buffers)
{
    static unsigned char want[6 * 4096];
    void *node, *handle = 0;
    struct vfs_stat st;
    int rc, i;
    uint32_t freed;

    wbytes = build_image(wimg, 4096, 128, PROFILE, 16, "tree");
    use_image(wimg, wbytes);
    if (bcache_resident() > buffers) {
        bcache_shrink(bcache_resident() - buffers, 0, &freed);
    }
    bcache_set_limit(buffers);
    check(mount_rw() == 0, "the cross-call count volume mounts");
    node = make_file(mnt.root, "batch", &rc);
    check(node && OPS->open(&mnt, node, 2, &handle) == 0, "the cross-call count handle opens");
    if (!node || !handle) {
        return;
    }
    memset(want, 0, sizeof(want));
    check(OPS->write(&mnt, handle, want, 4096, 0) == 4096 && OPS->fsync(&mnt, handle) == 0,
          "the rewrite fixture reaches the device");
    writes_made = 0;
    for (i = 1; i <= 16; i++) {
        memset(want, i, 4096);
        check(OPS->write(&mnt, handle, want, 4096, 0) == 4096,
              "a repeated write returns its whole block");
    }
    check(writes_made == 0, "cross-call rewrite collection writes no blocks");
    check(OPS->read(&mnt, handle, abuf, 4096, 0) == 4096 && memcmp(abuf, want, 4096) == 0,
          "a pending rewrite reads its latest bytes");
    check(OPS->fsync(&mnt, handle) == 0 && writes_made == 2,
          "sixteen rewrites use one data write and one inode write");
    printf("ext2-batch: result: cache %u rewrites %d writes\n", (unsigned)buffers, writes_made);

    memset(want + 4096, 64, 4096);
    memset(want + 8192, 65, 4096);
    writes_made = 0;
    check(OPS->write(&mnt, handle, want + 4096, 4096, 4096) == 4096 &&
              OPS->write(&mnt, handle, want + 8192, 4096, 8192) == 4096,
          "two direct-block appends are accepted");
    check(writes_made == 0, "cross-call append collection writes no blocks");
    cache_pending_pair(node);
    check(OPS->read(&mnt, handle, abuf, 12288, 0) == 12288 && memcmp(abuf, want, 12288) == 0,
          "pending appends read their full size and bytes");
    OPS->release(&mnt, handle);
    OPS->put(&mnt, node);
    check(writes_made > 0, "close drains pending inode and data state");
    check(file_is("/batch", want, 12288), "a reopened file retains the closed batch");
    node = walk("/batch", &rc);
    handle = 0;
    check(node && OPS->open(&mnt, node, 2, &handle) == 0, "the namespace row's handle opens");
    if (!node || !handle) {
        return;
    }
    memset(want + 12288, 66, 4096);
    check(OPS->write(&mnt, handle, want + 12288, 4096, 12288) == 4096,
          "an append precedes setattr");
    writes_made = 0;
    memset(&st, 0, sizeof(st));
    st.mode = 0600;
    check(OPS->setattr(&mnt, node, VFS_SET_MODE, &st) == 0 && writes_made > 0,
          "setattr drains pending state before storing an inode");
    memset(want + 16384, 67, 4096);
    check(OPS->write(&mnt, handle, want + 16384, 4096, 16384) == 4096, "an append precedes rename");
    writes_made = 0;
    check(OPS->rename(&mnt, mnt.root, "batch", mnt.root, "renamed") == 0 && writes_made > 0,
          "rename drains pending state before publishing a name");
    check(OPS->read(&mnt, handle, abuf, 20480, 0) == 20480 && memcmp(abuf, want, 20480) == 0,
          "the renamed handle retains all appended bytes");
    memset(want + 20480, 68, 4096);
    check(OPS->write(&mnt, handle, want + 20480, 4096, 20480) == 4096,
          "an append precedes truncate");
    writes_made = 0;
    check(OPS->truncate(&mnt, handle, 4096) == 0 && writes_made > 0,
          "truncate drains pending state before freeing blocks");
    check(OPS->fsync(&mnt, handle) == 0, "the truncated batch syncs");
    OPS->release(&mnt, handle);
    OPS->put(&mnt, node);
    check(file_is("/renamed", want, 4096), "truncate retains only the first block");
    check(unmount_rw() == 0, "the cross-call count volume unmounts");
    bcache_set_limit(0);
    check(e2fsck_clean(wimg), "the cross-call count volume is clean");
}

/** A one-terabyte sparse file visits only its allocated map branches.
 * Pending data is counted before sync; a cold map is never fetched. */
static void cache_sparse_row(void)
{
    struct vfs_cachestat cs;
    void *node, *handle = NULL;
    uint64_t waits, visits;
    int rc;

    wbytes = build_image(wimg, 4096, 128, PROFILE, 16, "tree");
    use_image(wimg, wbytes);
    check(mount_rw() == 0, "the sparse cache volume mounts");
    node = make_file(mnt.root, "sparse-cache", &rc);
    check(node && OPS->open(&mnt, node, 2, &handle) == 0, "the sparse cache file opens");
    if (!node || !handle) {
        return;
    }
    check(OPS->write(&mnt, handle, "x", 1, (1ull << 40) - 1u) == 1,
          "the sparse file has one page at one terabyte");
    memset(&cs, 0, sizeof(cs));
    waits = current_proc->disk_waits;
    visits = bcache_stat_visits();
    OPS->cachestat(&mnt, node, 0, 0xFFFFFFFFu, &cs);
    check(cs.cache == 1 && cs.dirty == 1, "the pending sparse page counts once and dirty");
    check(current_proc->disk_waits == waits && bcache_stat_visits() - visits < 16384,
          "the sparse query reads no disk and visits no logical-hole range");
    check(OPS->fsync(&mnt, handle) == 0, "the sparse file syncs before cache eviction");
    bcache_invalidate(&dev);
    memset(&cs, 0, sizeof(cs));
    waits = current_proc->disk_waits;
    OPS->cachestat(&mnt, node, 0, 0xFFFFFFFFu, &cs);
    check(cs.cache == 0 && cs.dirty == 0 && current_proc->disk_waits == waits,
          "a cold sparse map is not read merely to count pages");
    OPS->release(&mnt, handle);
    OPS->put(&mnt, node);
    check(unmount_rw() == 0 && e2fsck_clean(wimg), "the sparse cache volume stays clean");
}

/* The cache block of the pointer block `cache_partial_row` holds busy at
 * the walk's first service point, and the buffer once held. */
static uint64_t partial_pointer;
static struct bbuf *partial_held;

/** Hold the pointer block's buffer, as writeback would, at the first
 * service point only. */
static void hold_partial_pointer(void)
{
    if (!partial_held) {
        partial_held = bcache_get(&dev, partial_pointer);
    }
}

/**
 * On a 1 KiB volume, a page whose pointer block becomes busy part way
 * through it does not count. A 32 KiB file, which debugfs writes since the
 * driver mounts the volume read-only, has eight pages; its single indirect
 * block maps blocks 12 to 31, so page 3 is blocks 12 to 15. The walk's
 * sixteenth step, the service point, comes before block 14, and holding
 * the pointer block there leaves block 15 unread: pages 0 to 2 count, and
 * page 3 does not.
 */
static void cache_partial_row(void)
{
    static unsigned char data[32 * 1024];
    struct vfs_cachestat cs;
    char cmd[700];
    uint64_t ind[1];
    void *node, *f;
    int rc;

    memset(data, 0x5a, sizeof(data));
    f = fopen(fix_path("partial.bin"), "wb");
    check(f && fwrite(data, 1, sizeof(data), f) == sizeof(data), "the 32 KiB file is staged");
    if (f) {
        fclose(f);
    }
    wbytes = build_image(wimg, 1024, 128, PROFILE, 16, "tree");
    snprintf(cmd, sizeof cmd, "write %s partial-cache", fix_path("partial.bin"));
    check(ext2_fixture_debugfs(wimg, cmd, 1, 0), "debugfs writes the file on the 1 KiB volume");
    check(debugfs_numbers("stat /partial-cache", "(IND):", ind, 1) == 1,
          "debugfs names the file's indirect block");
    partial_pointer = (ind[0] << 10) / BCACHE_BLOCK_BYTES;
    use_image(wimg, wbytes);
    check(mount_image() == 0, "the partial-page cache volume mounts");
    node = walk("/partial-cache", &rc);
    check(node != 0, "the partial-page cache file is found");
    if (!node) {
        unmount_image();
        return;
    }
    check(read_at(node, abuf, sizeof(data), 0) == (int)sizeof(data) &&
              memcmp(abuf, data, sizeof(data)) == 0,
          "the file reads back whole and its blocks are cached");
    memset(&cs, 0, sizeof(cs));
    OPS->cachestat(&mnt, node, 0, 0xFFFFFFFFu, &cs);
    check(cs.cache == 8 && cs.dirty == 0, "all eight pages of the read file count, clean");
    partial_held = 0;
    service_pause = hold_partial_pointer;
    memset(&cs, 0, sizeof(cs));
    OPS->cachestat(&mnt, node, 0, 0xFFFFFFFFu, &cs);
    service_pause = 0;
    check(partial_held != 0, "the walk reached its service point inside the indirect block");
    check(cs.cache == 3, "a page whose pointer block went busy part way through does not count");
    if (cs.cache != 3) {
        printf("  cache %lu\n", (unsigned long)cs.cache);
    }
    if (partial_held) {
        bcache_release(partial_held);
        partial_held = 0;
    }
    OPS->put(&mnt, node);
    unmount_image();
}

/**
 * A failed drain is reported by the fsync that met it and recorded for the
 * VFS, which reports it once to each description (`vfs_fsync`); the type's
 * own fsync answers for its own writes alone, so a retry that succeeds is
 * 0, through a close and a reopen too, and the mount stays read-only; the
 * bytes the write took are kept.
 */
static void batch_error_row(void)
{
    unsigned char bytes[4096];
    void *node, *handle = 0;
    int rc, before;

    wbytes = build_image(wimg, 4096, 128, PROFILE, 16, "tree");
    use_image(wimg, wbytes);
    check(mount_rw() == 0, "the delayed-error volume mounts");
    node = make_file(mnt.root, "batch-error", &rc);
    check(node && OPS->open(&mnt, node, 2, &handle) == 0, "the delayed-error handle opens");
    if (!node || !handle) {
        return;
    }
    memset(bytes, 0x71, sizeof(bytes));
    check(OPS->write(&mnt, handle, bytes, sizeof(bytes), 0) == sizeof(bytes),
          "a write is accepted before its delayed error");
    before = writes_made;
    stop_after = before;
    wb_err_sets = wb_err_last = 0;
    check(OPS->fsync(&mnt, handle) == -EIO, "fsync reports a delayed device error");
    check(wb_err_sets > 0 && wb_err_last == -EIO && mnt.write_error == -EIO &&
              (mnt.flags & VFS_MS_RDONLY),
          "and the error is recorded for the VFS and keeps the mount read-only");
    stop_after = -1;
    check(OPS->fsync(&mnt, handle) == 0 && writes_made > before,
          "a retry that succeeds answers for itself alone");
    OPS->release(&mnt, handle);
    OPS->put(&mnt, node);
    node = walk("/batch-error", &rc);
    handle = 0;
    check(node && OPS->open(&mnt, node, 0, &handle) == 0, "the failed file reopens for reading");
    if (node && handle) {
        check(OPS->fsync(&mnt, handle) == 0 && mnt.write_error == -EIO,
              "a reopened fsync finds the retried writes on the medium");
        check(OPS->read(&mnt, handle, abuf, sizeof(bytes), 0) == sizeof(bytes) &&
                  memcmp(abuf, bytes, sizeof(bytes)) == 0,
              "the successful retry retained the accepted bytes");
        OPS->release(&mnt, handle);
        OPS->put(&mnt, node);
    }
    check(unmount_rw() == 0, "unmount completes after a successful retry");
    bcache_device_gone(&dev);
    dev.flags = BLOCK_FAULT_SAFE;
}

/**
 * Fail the flush numbered `barrier` of an fsync after two appends that
 * cross an indirect level, from block 1035, or with `large` from block
 * 524287, then retry. The fsync that met the failure reports it and
 * records it for the VFS, the retry flushes again and answers for itself,
 * as does an fsync after a close and a reopen, and both blocks must read
 * back. The barriers are those of the allocation, the data, the pointers,
 * the feature flag and the inode.
 */
static void batch_flush_error_row(int barrier, int large)
{
    unsigned char bytes[8192];
    uint64_t first = large ? 524287 : 1035;
    void *node, *handle = 0;
    int rc, before;
    int initial_failures = fails;

    wbytes = build_image(wimg, 4096, 128, PROFILE, 16, "tree");
    use_image(wimg, wbytes);
    check(mount_rw() == 0, "the failed-flush volume mounts");
    node = make_file(mnt.root, "flush-error", &rc);
    check(node && OPS->open(&mnt, node, 2, &handle) == 0, "the failed-flush handle opens");
    if (!node || !handle) {
        return;
    }
    check(OPS->truncate(&mnt, handle, first * 4096) == 0,
          "the failed-flush file has a sparse prefix");
    memset(bytes, 0x42, 4096);
    memset(bytes + 4096, 0x79, 4096);
    check(OPS->write(&mnt, handle, bytes, 4096, first * 4096) == 4096 &&
              OPS->write(&mnt, handle, bytes + 4096, 4096, (first + 1) * 4096) == 4096,
          "two appends cross an indirect level before the failed flush");
    before = flush_calls;
    flush_fail_at = before + barrier;
    wb_err_sets = wb_err_last = 0;
    check(OPS->fsync(&mnt, handle) == -EIO && flush_calls == flush_fail_at,
          "the selected stage flush fails fsync");
    check(wb_err_sets > 0 && wb_err_last == -EIO && mnt.write_error == -EIO,
          "and the flush error is recorded for the VFS");
    flush_fail_at = -1;
    before = flush_calls;
    check(OPS->fsync(&mnt, handle) == 0 && flush_calls > before,
          "retry flushes the retained stage and answers for itself");
    check(OPS->read(&mnt, handle, abuf, sizeof(bytes), first * 4096) == sizeof(bytes) &&
              memcmp(abuf, bytes, sizeof(bytes)) == 0,
          "a flush retry retains both appended blocks");
    OPS->release(&mnt, handle);
    OPS->put(&mnt, node);
    node = walk("/flush-error", &rc);
    handle = 0;
    check(node && OPS->open(&mnt, node, 0, &handle) == 0, "the failed-flush file reopens");
    if (node && handle) {
        check(OPS->fsync(&mnt, handle) == 0 && mnt.write_error == -EIO,
              "a reopened fsync succeeds and the mount keeps its first error");
        OPS->release(&mnt, handle);
        OPS->put(&mnt, node);
    }
    check(unmount_rw() == 0, "unmount completes after the failed flush is retried");
    bcache_device_gone(&dev);
    dev.flags = BLOCK_FAULT_SAFE;
    printf("ext2-media: result: barrier %d, large %d, failures %d\n", barrier, large,
           fails - initial_failures);
}

/* The file locks the calling process held at the last flush and the last
 * read of the device, -1 before one. */
static int flush_locks, read_locks;

/** Note the file locks held at a read of the device. */
static void read_locks_note(void)
{
    read_locks = current_proc->file_locks;
}

/** Note the file locks held at a flush. */
static void flush_locks_note(void)
{
    flush_locks = current_proc->file_locks;
}

/**
 * `sync`, `fsync` and a read of a block the cache lacks wait for the disk
 * with no file lock held, so that a slow disk holds up no other call.
 */
static void sync_unlocked_row(void)
{
    char text[6];
    void *node, *handle = 0;
    int rc;

    wbytes = build_image(wimg, 4096, 128, PROFILE, 16, "tree");
    use_image(wimg, wbytes);
    check(mount_rw() == 0, "the unlocked-sync volume mounts");
    node = make_file(mnt.root, "sync-unlocked", &rc);
    check(node && OPS->open(&mnt, node, 2, &handle) == 0, "the unlocked-sync handle opens");
    if (!node || !handle) {
        return;
    }
    flush_pause = flush_locks_note;
    check(OPS->write(&mnt, handle, "one", 3, 0) == 3, "the first write is taken");
    flush_locks = -1;
    check(OPS->fsync(&mnt, handle) == 0 && flush_locks == 0,
          "fsync flushes the disk with no file lock held");
    check(OPS->write(&mnt, handle, "two", 3, 3) == 3, "the second write is taken");
    flush_locks = -1;
    check(OPS->sync(&mnt) == 0 && flush_locks == 0, "sync flushes the disk with no file lock held");
    flush_pause = 0;
    check(proc_a.file_locks == 0, "sync and fsync leave no file lock held");
    check(bcache_invalidate(&dev) == 0, "the cache gives the file's block up");
    read_pause = read_locks_note;
    read_locks = -1;
    check(OPS->read(&mnt, handle, text, sizeof(text), 0) == sizeof(text) &&
              memcmp(text, "onetwo", sizeof(text)) == 0 && read_locks == 0,
          "a read fetches a missing block with the file's lock let go");
    read_pause = 0;
    OPS->release(&mnt, handle);
    OPS->put(&mnt, node);
    check(unmount_rw() == 0, "the unlocked-sync volume unmounts");
}

/* compat/bkl_held.c: whether a row reports the big lock free, and the
   takes the code made of it since the row last cleared the count. */
extern int bkl_host_free;
extern int bkl_host_takes;

/** How many of the names "busy" and "quiet" a listing of the directory
 * open at `h` finds, from its first entry; -1 for an error. The description
 * stays open, since its release drains the batch. */
static int listing(void *h)
{
    struct vfs_dirent de;
    uint32_t cursor = 0;
    int seen = 0, got;

    while ((got = OPS->readdir(&mnt, h, &cursor, &de)) == 1) {
        seen += streq(de.name, "busy") || streq(de.name, "quiet");
    }
    return got == 0 ? seen : -1;
}

/**
 * Open, read, list, stat and close files as the file calls do without the
 * big lock on a type that sets `reads_unlocked`, with the lock reported
 * free (compat/bkl_held.c), and count the takes the driver makes. While
 * another description's write waits in the batch, a read and a listing
 * take no lock but the batch's own and read the pending bytes from its
 * images, a hold that is not the last is dropped without it
 * (`ext2_put_unless_last`), and a release drains the batch into the cache
 * without it too (`ext2_writeback_drain`), the cache's write side having a
 * lock of its own. Once the batch is empty a read, a listing and a release
 * take it never, and a last put takes it once (`ext2_put`).
 */
static void unlocked_reader_row(void)
{
    static unsigned char want[4096], pend[4096];
    void *busy, *quiet, *bh = 0, *qh = 0, *rh = 0, *dh = 0;
    struct vfs_stat st;
    int rc, ok;

    wbytes = build_image(wimg, 4096, 128, PROFILE, 16, "tree");
    use_image(wimg, wbytes);
    check(mount_rw() == 0, "the unlocked reader volume mounts");
    busy = make_file(mnt.root, "busy", &rc);
    quiet = make_file(mnt.root, "quiet", &rc);
    memset(want, 'q', sizeof(want));
    memset(pend, 'b', sizeof(pend));
    ok = busy && quiet && write_at(quiet, want, sizeof(want), 0) == 0 &&
         OPS->open(&mnt, busy, 2, &bh) == 0;
    check(ok, "the unlocked reader's files are made and the busy one opens for writing");
    if (!ok) {
        return;
    }
    writes_made = 0;
    check(OPS->write(&mnt, bh, pend, sizeof(pend), 0) == (int32_t)sizeof(pend) && writes_made == 0,
          "the busy file's write waits in the batch");

    bkl_host_free = 1;
    bkl_host_takes = 0;
    check(OPS->open(&mnt, quiet, 0, &qh) == 0 && OPS->open(&mnt, busy, 0, &rh) == 0 &&
              OPS->open(&mnt, mnt.root, 0, &dh) == 0 && bkl_host_takes == 0,
          "read-only opens beside a busy batch take no big lock");
    check(OPS->read(&mnt, qh, abuf, sizeof(want), 0) == (int32_t)sizeof(want) &&
              memcmp(abuf, want, sizeof(want)) == 0 && bkl_host_takes == 0,
          "a read beside a busy batch takes no big lock and reads the file");
    bkl_host_takes = 0;
    check(OPS->read(&mnt, rh, abuf, sizeof(pend), 0) == (int32_t)sizeof(pend) &&
              memcmp(abuf, pend, sizeof(pend)) == 0 && bkl_host_takes == 0,
          "a read of a file whose write waits in the batch reads the pending bytes without it");
    bkl_host_takes = 0;
    check(listing(dh) == 2 && bkl_host_takes == 0,
          "a listing beside a busy batch takes no big lock and sees both names");
    check(OPS->getattr(&mnt, busy, &st) == 0 && st.size == sizeof(pend),
          "a stat without the big lock gives the pending size");
    bkl_host_takes = 0;
    OPS->hold(&mnt, quiet);
    OPS->put(&mnt, quiet);
    check(bkl_host_takes == 0, "a hold that is not the last is dropped without the big lock");
    writes_made = 0;
    OPS->release(&mnt, rh);
    check(bkl_host_takes == 0 && writes_made > 0,
          "a release beside a busy batch drains it without the big lock");

    bkl_host_takes = 0;
    writes_made = 0;
    check(OPS->read(&mnt, qh, abuf, sizeof(want), 0) == (int32_t)sizeof(want) &&
              memcmp(abuf, want, sizeof(want)) == 0,
          "a read with the batch empty reads the file");
    check(OPS->open(&mnt, busy, 0, &rh) == 0 &&
              OPS->read(&mnt, rh, abuf, sizeof(pend), 0) == (int32_t)sizeof(pend) &&
              memcmp(abuf, pend, sizeof(pend)) == 0,
          "a read with the batch empty reads the drained write from the cache");
    check(listing(dh) == 2, "a listing with the batch empty sees both names");
    OPS->release(&mnt, dh);
    OPS->release(&mnt, rh);
    OPS->release(&mnt, qh);
    check(bkl_host_takes == 0 && writes_made == 0,
          "reads, a listing and releases with the batch empty take no big lock and write nothing");
    OPS->put(&mnt, quiet);
    check(bkl_host_takes == 1, "the last put of a node takes the big lock once");
    bkl_host_free = 0;

    OPS->release(&mnt, bh);
    OPS->put(&mnt, busy);
    check(file_is("/busy", pend, sizeof(pend)) && file_is("/quiet", want, sizeof(want)),
          "both files read back after the unlocked calls");
    check(unmount_rw() == 0, "the unlocked reader volume unmounts");
    check(e2fsck_clean(wimg), "the unlocked reader volume is clean");
}

/**
 * On a mount that writes back, calls that store inodes and change names
 * leave a pending write in the batch, and their writes into a block the
 * batch holds reach its image too: a create whose inode shares the pending
 * file's table block, a mkdir, a chmod and a rename of the pending file,
 * and an append after them. The drain at the close keeps every one, read
 * back before and after a remount, and the volume is clean.
 */
static void held_block_row(void)
{
    static unsigned char data[4 * 4096];
    void *a, *b, *handle = 0;
    struct vfs_stat st;
    int rc;

    wbytes = build_image(wimg, 4096, 128, PROFILE, 16, "tree");
    use_image(wimg, wbytes);
    rw_flags = 0;
    rc = mount_rw();
    check(rc == 0, "the held-block volume mounts without dirsync");
    if (rc != 0) {
        rw_flags = VFS_MS_DIRSYNC;
        return;
    }
    memset(data, 0x4d, sizeof(data));
    memset(data + 12288, 0x4e, 4096);
    a = make_file(mnt.root, "held-a", &rc);
    check(a && OPS->open(&mnt, a, 2, &handle) == 0 &&
              OPS->write(&mnt, handle, data, 12288, 0) == 12288,
          "three blocks of a file wait in the batch");
    b = a ? make_file(mnt.root, "held-b", &rc) : 0;
    check(b != 0 && OPS->mkdir(&mnt, mnt.root, "held-c", 0755) == 0,
          "a create beside it and a mkdir");
    memset(&st, 0, sizeof(st));
    st.mode = 0600;
    check(a && OPS->setattr(&mnt, a, VFS_SET_MODE, &st) == 0 &&
              OPS->rename(&mnt, mnt.root, "held-a", mnt.root, "held-a2") == 0,
          "a chmod and a rename of the pending file");
    check(handle && OPS->write(&mnt, handle, data + 12288, 4096, 12288) == 4096,
          "and an append after them");
    check(file_is("/held-a2", data, sizeof(data)), "the pending file reads whole");
    if (handle) {
        OPS->release(&mnt, handle);
    }
    if (a) {
        OPS->put(&mnt, a);
    }
    if (b) {
        OPS->put(&mnt, b);
    }
    check(unmount_rw() == 0 && e2fsck_clean(wimg), "the held-block volume unmounts clean");
    rc = mount_rw();
    rw_flags = VFS_MS_DIRSYNC;
    check(rc == 0 && file_is("/held-a2", data, sizeof(data)) && file_is("/held-b", data, 0),
          "and after a remount both files read back");
    a = rc == 0 ? walk("/held-a2", &rc) : 0;
    check(a && OPS->getattr(&mnt, a, &st) == 0 && (st.mode & 07777) == 0600, "with the chmod kept");
    if (a) {
        OPS->put(&mnt, a);
    }
    b = walk("/held-c", &rc);
    check(b != 0, "and the directory there");
    if (b) {
        OPS->put(&mnt, b);
    }
    check(unmount_rw() == 0, "the held-block volume unmounts again");
}

/** The holds of `em->batch` and of the groups' locks, every mount's, that
 * crossed a disk request. */
static uint32_t alloc_crossings(void)
{
    const struct sleep_lock_record *r;
    uint32_t n = 0;

    for (r = sleep_lock_records(); r; r = r->next) {
        if (streq(r->kind, "ext2 batch") || streq(r->kind, "ext2 group")) {
            n += r->crossings;
        }
    }
    return n;
}

/**
 * On a mount that writes back, under a cache of 16 buffers that another
 * file's blocks have filled with dirty ones, a write of eight blocks and
 * the fsync that drains them: the cache writes the victims before the drain
 * takes `em->batch`, so that no hold of it crosses a disk request, and both
 * files read back after a remount on a clean volume.
 */
static void drain_victims_row(void)
{
    static unsigned char other[16 * 4096], data[8 * 4096];
    void *x, *y, *handle = 0;
    uint32_t freed, crossed;
    int rc;

    wbytes = build_image(wimg, 4096, 128, PROFILE, 16, "tree");
    use_image(wimg, wbytes);
    if (bcache_resident() > 16) {
        bcache_shrink(bcache_resident() - 16, 0, &freed);
    }
    bcache_set_limit(16);
    rw_flags = 0;
    rc = mount_rw();
    check(rc == 0, "the drain volume mounts without dirsync under 16 buffers");
    if (rc != 0) {
        rw_flags = VFS_MS_DIRSYNC;
        bcache_set_limit(0);
        return;
    }
    memset(other, 0x61, sizeof(other));
    memset(data, 0x62, sizeof(data));
    x = make_file(mnt.root, "victims", &rc);
    check(x && write_at(x, other, sizeof(other), 0) == 0,
          "sixteen blocks of one file are dirty in the cache");
    y = make_file(mnt.root, "drained", &rc);
    check(y && OPS->open(&mnt, y, 2, &handle) == 0, "another file is open");
    crossed = alloc_crossings();
    if (handle) {
        OPS->lock_node(&mnt, y);
        check(OPS->write(&mnt, handle, data, sizeof(data), 0) == (int32_t)sizeof(data),
              "eight blocks of it wait in the batch");
        OPS->unlock_node(&mnt, y);
        check(OPS->fsync(&mnt, handle) == 0, "and an fsync drains them past the victims");
    }
    check(alloc_crossings() == crossed, "with em->batch held across no disk request");
    if (handle) {
        OPS->release(&mnt, handle);
    }
    if (x) {
        OPS->put(&mnt, x);
    }
    if (y) {
        OPS->put(&mnt, y);
    }
    check(unmount_rw() == 0 && e2fsck_clean(wimg), "the drain volume unmounts clean");
    rc = mount_rw();
    rw_flags = VFS_MS_DIRSYNC;
    check(rc == 0 && file_is("/victims", other, sizeof(other)) &&
              file_is("/drained", data, sizeof(data)),
          "and after a remount both files read back");
    check(unmount_rw() == 0, "the drain volume unmounts again");
    bcache_set_limit(0);
}

/* The description of the second file of `overlap_punch_row`, and 1 once
 * process B's truncate of it succeeded or -1 once it failed. */
static void *overlap_handle;
static int overlap_done;

/** At the first truncate's first disk request, its read of the indirect
 * block, give process B the turn. */
static void overlap_turn(void)
{
    read_pause = 0;
    pass_turn();
}

/** Process B of `overlap_punch_row`: the second file truncated to nothing
 * under its lock. */
static void overlap_second(void)
{
    void *y = overlap_handle;

    OPS->lock_node(&mnt, y);
    overlap_done = OPS->truncate(&mnt, overlap_handle, 0) == 0 ? 1 : -1;
    OPS->unlock_node(&mnt, y);
}

/** Whether the writes up to the log's mark `synced` put inode `ino`'s table
 * block on the disk before the first write of the block bitmap and of the
 * descriptors, on the image of the unmounted volume. */
static int inodes_first(uint64_t ino, int synced)
{
    uint64_t table[2], bm[4];
    char cmd[64];
    int logged, nbm, ok;

    snprintf(cmd, sizeof(cmd), "imap <%lu>", (unsigned long)ino);
    ok = debugfs_numbers(cmd, "located at block ", table, 2) == 1;
    nbm = debugfs_numbers("stats", "block bitmap at ", bm, 4);
    check(ok && nbm >= 1 && !wlog_overflow, "debugfs names the inode table block and the bitmap");
    logged = wlog_n;
    wlog_n = synced;
    ok = ok && nbm >= 1 && last_write_of(table[0]) >= 0 && first_write_of(bm[0]) >= 0 &&
         last_write_of(table[0]) < first_write_of(bm[0]) &&
         last_write_of(table[0]) < first_write_of(1);
    wlog_n = logged;
    return ok;
}

/**
 * Two truncates of two files of twenty blocks, on a mount that writes back:
 * process B's runs whole while process A's waits for its indirect block.
 * Neither holds a group's lock across a disk request, each free keeps its own
 * list, and the cache's two classes keep the order of both: at the sync every
 * write of the files' inode table block comes before the first of the block
 * bitmap and of the descriptors, so no pointer on the disk names a block
 * whose bit is clear, and the volume is clean with both files empty.
 */
static void overlap_punch_row(void)
{
    static unsigned char data[20 * 4096];
    void *x, *y, *handle = 0;
    struct vfs_stat st;
    uint32_t crossed;
    int rc, synced;

    wbytes = build_image(wimg, 4096, 128, PROFILE, 16, "tree");
    use_image(wimg, wbytes);
    rw_flags = 0;
    rc = mount_rw();
    rw_flags = VFS_MS_DIRSYNC;
    check(rc == 0, "the overlap volume mounts without dirsync");
    if (rc != 0) {
        return;
    }
    memset(data, 0x3c, sizeof(data));
    x = make_file(mnt.root, "punch-x", &rc);
    y = x ? make_file(mnt.root, "punch-y", &rc) : 0;
    check(x && y && write_at(x, data, sizeof(data), 0) == 0 &&
              write_at(y, data, sizeof(data), 0) == 0,
          "two files of twenty blocks, past the direct pointers");
    overlap_handle = 0;
    check(x && y && OPS->open(&mnt, x, 2, &handle) == 0 &&
              OPS->open(&mnt, y, 2, &overlap_handle) == 0,
          "and a description of each");
    check(OPS->sync(&mnt) == 0 && bcache_sync(&dev) == 0 && bcache_invalidate(&dev) == 0,
          "both reach the disk and leave the cache");
    memset(&st, 0, sizeof(st));
    st.mode = 0600;
    check(x && y && OPS->setattr(&mnt, x, VFS_SET_MODE, &st) == 0 &&
              OPS->setattr(&mnt, y, VFS_SET_MODE, &st) == 0 && OPS->getattr(&mnt, x, &st) == 0,
          "their inodes' block is read again");
    if (!handle || !overlap_handle) {
        (void)unmount_rw();
        return;
    }
    overlap_done = 0;
    sleeps = 0;
    wlog_reset();
    crossed = alloc_crossings();
    read_pause = overlap_turn;
    start_turns(overlap_second);
    OPS->lock_node(&mnt, x);
    rc = OPS->truncate(&mnt, handle, 0);
    OPS->unlock_node(&mnt, x);
    finish_turns();
    read_pause = 0;
    check(rc == 0 && overlap_done == 1 && sleeps == 0,
          "one truncate runs whole while the other waits for its indirect block");
    check(alloc_crossings() == crossed, "and neither holds a group's lock across a disk request");
    check(OPS->sync(&mnt) == 0, "the two frees sync");
    synced = wlog_n;
    OPS->release(&mnt, handle);
    OPS->release(&mnt, overlap_handle);
    OPS->put(&mnt, x);
    OPS->put(&mnt, y);
    check(unmount_rw() == 0 && e2fsck_clean(wimg), "the overlap volume unmounts clean");
    check(inodes_first(st.ino, synced),
          "the inodes reached the disk before the bitmap and the descriptors");
    wlog_reset();
    rc = mount_rw();
    check(rc == 0 && file_is("/punch-x", data, 0) && file_is("/punch-y", data, 0),
          "and both files are empty after a remount");
    check(unmount_rw() == 0, "the overlap volume unmounts again");
}

#include "ext2_batch_cases.h"
#include "ext2_batch_lifecycle.h"
#include "ext2_node_rows.h"
#include "ext2_lock_rows.h"
#include "ext2_fsync_rows.h"
#include "ext2_order_rows.h"
#include "ext2_pair_rows.h"
#include "ext2_service_rows.h"
#include "ext2_group_rows.h"
#include "ext2_write_rows.h"
#include "ext2_names_rows.h"

/* The host heap's limit on one allocation, 0 for none, and the page
 * allocations it grants before it fails, -1 for no limit
 * (compat/kheap_host.c). */
extern unsigned long kheap_host_largest;
extern long kheap_host_pages_left;

/**
 * Mount read-write under a heap that grants no allocation longer than a
 * reserve run.
 *
 * The kernel heap keeps runs of 256 pages for large requests
 * (rach2/sys/mm/kheap.c); anything longer has to come from the ordinary
 * region in one piece, which a heap whose free pages the cache has
 * scattered may not have although memory is free. A read-write mount, the
 * write batch's buffers included, must therefore ask for no more than one
 * run at a time.
 */
static void reserve_run_row(void)
{
    wbytes = build_image(wimg, 4096, 128, PROFILE, 16, "tree");
    use_image(wimg, wbytes);
    kheap_host_largest = 256ul * 4096;
    check(mount_rw() == 0, "a read-write mount needs no run longer than a reserve run");
    kheap_host_largest = 0;
    check(unmount_rw() == 0, "and that mount unmounts cleanly");
}

/**
 * A remount writable whose batch the heap cannot give fails with `ENOMEM`
 * and keeps the mount read-only; the next remount, with the heap restored,
 * sets the whole batch up, and a file written through it leaves a clean
 * volume. The heap gives one page and refuses the second, so that the
 * failed remount has taken part of a batch.
 */
static void remount_nomem_row(void)
{
    int rc;
    void *f;

    wbytes = build_image(wimg, 4096, 128, PROFILE, 16, "tree");
    use_image(wimg, wbytes);
    check(mount_ro() == 0, "the remount volume mounts read-only");
    kheap_host_pages_left = 1;
    rc = OPS->remount(&mnt, rw_flags, NULL);
    kheap_host_pages_left = -1;
    check(rc == -ENOMEM, "a remount writable without the pages of a batch is ENOMEM");
    check(make_file(mnt.root, "refused", &rc) == 0 && rc == -EROFS,
          "and the mount stays read-only");
    check(OPS->remount(&mnt, rw_flags, NULL) == 0, "the next remount writable succeeds");
    mnt.flags = rw_flags;
    f = make_file(mnt.root, "after", &rc);
    check(f && write_at(f, pattern, 5000, 0) == 0, "and a file is written through its batch");
    if (f) {
        OPS->put(&mnt, f);
    }
    check(unmount_rw() == 0 && e2fsck_clean(wimg), "and the volume unmounts clean");
}

/**
 * The write batch's rows: the rows of ext2_batch_cases.h and
 * ext2_batch_lifecycle.h, `batch_rows_under` and the cross-call order rows
 * at 16 and 512 buffers, from blocks 11, 1035 and 2059, then the default
 * mount, an rmdir of a held directory, a remount, a remount the heap cannot
 * give a batch, the failed drain and flush rows, the unlocked sync, and on a
 * mount that writes back a batch kept across namespace calls, a drain past
 * dirty victims and two interleaved truncates.
 */
static void batch_rows(void)
{
    int barrier;

    reserve_run_row();
    batch_lifecycle_rows();
    batch_partial_row(0);
    batch_partial_row(1);
    batch_reuse_row();
    batch_limit_rows();
    batch_metadata_row();
    batch_handoff_row();
    batch_stage_rows();
    batch_abort_row();
    batch_description_row();
    batch_loss_row();
    batch_rows_under(16);
    batch_rows_under(512);
    cache_sparse_row();
    cache_partial_row();
    order_row_under(16, 1, 11);
    order_row_under(16, 1, 1035);
    order_row_under(16, 1, 2059);
    order_row_under(512, 1, 11);
    order_row_under(512, 1, 1035);
    order_row_under(512, 1, 2059);
    default_mount_row();
    held_rmdir_row();
    dir_index_row();
    dir_index_failed_store_row();
    remount_row();
    remount_nomem_row();
    batch_error_row();
    for (barrier = 1; barrier <= 5; barrier++) {
        batch_flush_error_row(barrier, 0);
    }
    batch_flush_error_row(5, 1);
    batch_flush_error_row(6, 1);
    sync_unlocked_row();
    unlocked_reader_row();
    held_block_row();
    drain_victims_row();
    overlap_punch_row();
    lock_rows();
}

/* ---- the file type of a replaced entry ---- */

/* The files `ftype_replace` makes and replaces by links, half of them by a
   rename over them and half by an unlink and a symlink, as Wine's prefix
   setup replaces its DLLs. */
#define FTYPE_FILES 48

/**
 * In a new directory `dlls`, make FTYPE_FILES files of a few bytes each,
 * then replace each by a symbolic link: the even ones by a link made under
 * a temporary name and renamed over the file, the odd ones by an unlink and
 * a symlink of the same name, which can take the freed inode; then rename
 * a new file over the first link, which puts a regular file back. Returns
 * 0, or the first error.
 */
static int ftype_replace(void)
{
    char name[32], temp[32];
    void *d, *n;
    int i, rc = OPS->mkdir(&mnt, mnt.root, "dlls", 0755);

    if (rc != 0) {
        return rc;
    }
    d = walk("/dlls", &rc);
    if (!d) {
        return rc;
    }
    for (i = 0; rc == 0 && i < FTYPE_FILES; i++) {
        snprintf(name, sizeof name, "f%02d.dll", i);
        n = make_file(d, name, &rc);
        if (n) {
            rc = write_at(n, pattern, 100 + i, 0);
            OPS->put(&mnt, n);
        }
    }
    for (i = 0; rc == 0 && i < FTYPE_FILES; i++) {
        snprintf(name, sizeof name, "f%02d.dll", i);
        if (i % 2 == 0) {
            snprintf(temp, sizeof temp, "f%02d.tmp", i);
            rc = OPS->symlink(&mnt, d, temp, "../hello.txt");
            if (rc == 0) {
                rc = OPS->rename(&mnt, d, temp, d, name);
            }
        } else {
            rc = OPS->unlink(&mnt, d, name);
            if (rc == 0) {
                rc = OPS->symlink(&mnt, d, name, "../hello.txt");
            }
        }
    }
    if (rc == 0) {
        n = make_file(d, "back.dll", &rc);
        if (n) {
            rc = write_at(n, pattern, 10, 0);
            OPS->put(&mnt, n);
        }
    }
    if (rc == 0) {
        rc = OPS->rename(&mnt, d, "back.dll", d, "f00.dll");
    }
    OPS->put(&mnt, d);
    return rc;
}

/**
 * Whether every name `ftype_replace` left reads back as it should: f00.dll
 * the 10-byte file renamed over it, and each other name a link to
 * ../hello.txt.
 */
static int ftype_after(void)
{
    char path[40], buf[64];
    void *n;
    int i, err, got;

    if (!file_is("/dlls/f00.dll", pattern, 10)) {
        return 0;
    }
    for (i = 1; i < FTYPE_FILES; i++) {
        snprintf(path, sizeof path, "/dlls/f%02d.dll", i);
        n = walk(path, &err);
        if (!n) {
            return 0;
        }
        got = OPS->readlink(&mnt, n, buf, sizeof buf);
        OPS->put(&mnt, n);
        if (got != 12 || memcmp(buf, "../hello.txt", 12) != 0) {
            printf("  %s: readlink %d\n", path, got);
            return 0;
        }
    }
    return 1;
}

/**
 * Replace files by symbolic links and a link by a file (`ftype_replace`)
 * on a mount with `flags`, unmount cleanly, and require e2fsck to find
 * each entry's file type that of the inode it names, as Linux's
 * `ext2_set_link` and `ext2_add_link` write it; then stop a rename of a
 * link over a file after each of its writes on a `dirsync` mount, and
 * require no stop to leave an entry whose type is not its inode's.
 */
static void ftype_rows(void)
{
    static const uint32_t flags[2] = {VFS_MS_DIRSYNC, 0};
    char what[200];
    int i, k, n, rc;
    void *d;

    fill_pattern();
    for (i = 0; i < 2; i++) {
        const char *how = flags[i] ? "a dirsync mount" : "a mount that writes back";

        wbytes = build_image(wimg, 4096, 128, PROFILE, 16, "tree");
        use_image(wimg, wbytes);
        stop_after = -1;
        rc = mount_flags(flags[i]);
        snprintf(what, sizeof what, "%s for the replaced entries mounts", how);
        check(rc == 0 && (mnt.flags & VFS_MS_RDONLY) == 0, what);
        if (rc != 0) {
            continue;
        }
        rc = ftype_replace();
        snprintf(what, sizeof what, "on %s, files replaced by links and a link by a file", how);
        check(rc == 0, what);
        if (rc != 0) {
            printf("  rc %d\n", rc);
        }
        check(ftype_after(), "and each name reads back as what replaced it");
        check(unmount_rw() == 0, "and the volume unmounts");
        snprintf(what, sizeof what, "on %s, e2fsck finds each replaced entry's type its inode's",
                 how);
        rc = e2fsck_clean(wimg);
        check(rc, what);
        if (!rc) {
            ext2_fixture_head(fix_path("fsck.out"), 20);
        }
    }

    /* The writes of one rename of a link over a file, counted on a full run. */
    n = -1;
    for (k = 0; n < 0 || k <= n; k++) {
        wbytes = build_image(wimg, 4096, 128, PROFILE, 16, "tree");
        use_image(wimg, wbytes);
        stop_after = -1;
        if (mount_flags(VFS_MS_DIRSYNC) != 0) {
            check(0, "the volume for the stopped renames mounts");
            return;
        }
        d = mnt.root;
        rc = OPS->symlink(&mnt, d, "link.tmp", "hello.txt");
        if (rc == 0) {
            writes_made = 0;
            stop_after = n < 0 ? -1 : k;
            rc = OPS->rename(&mnt, d, "link.tmp", d, "hello.txt");
        }
        if (n < 0) {
            check(rc == 0, "a link renames over a file on a dirsync mount");
            n = writes_made;
            stop_after = -1;
            check(unmount_rw() == 0 && e2fsck_clean(wimg), "and the volume is clean");
            k = -1;
            continue;
        }
        OPS->put(&mnt, mnt.root);
        OPS->unmount(&mnt);
        mnt.root = 0;
        bcache_device_gone(&dev);
        dev.flags = BLOCK_FAULT_SAFE;
        stop_after = -1;
        use_image(wimg, wbytes);
        (void)ext2_fixture_fsck(wimg, "-fn", fix_path("fsck.out"));
        snprintf(what, sizeof what,
                 "the rename of a link over a file stopped after write %d of %d leaves no "
                 "entry of the wrong type",
                 k, n);
        rc = !ext2_fixture_contains(fix_path("fsck.out"), "filetype");
        check(rc, what);
        if (!rc) {
            ext2_fixture_head(fix_path("fsck.out"), 20);
        }
    }
}

/**
 * Run each of `sequences` on a fresh volume and judge it by the driver,
 * by e2fsck, by setidcheck's reader, and by the driver again after a
 * remount. Then check the superblock: a mount for writing clears the valid
 * bit; a sequence leaves the free counts as they were and a sync writes
 * them; an unmount sets the valid bit. Last, debugfs must list the files,
 * directory and link this driver wrote, and read the kinds and device
 * numbers of its special files where Linux keeps them.
 */
static void write_rows(void)
{
    int i;

    fill_pattern();
    for (i = 0; i < (int)(sizeof(sequences) / sizeof(sequences[0])); i++) {
        char what[200];
        int rc;

        wbytes = build_image(wimg, 4096, 128, PROFILE, 16, "tree");
        use_image(wimg, wbytes);
        stop_after = -1;
        writes_made = 0;
        rc = mount_rw();
        snprintf(what, sizeof(what), "'%s': the volume mounts for writing", sequences[i].name);
        check(rc == 0 && (mnt.flags & VFS_MS_RDONLY) == 0, what);
        if (rc != 0) {
            continue;
        }
        rc = sequences[i].run();
        snprintf(what, sizeof(what), "'%s' runs", sequences[i].name);
        check(rc == 0, what);
        if (rc != 0) {
            printf("  rc %d\n", rc);
        }
        snprintf(what, sizeof(what), "'%s' reads back through the driver", sequences[i].name);
        check(i == 0 ? oracle_create_write() : oracle_after(i), what);
        rc = unmount_rw();
        snprintf(what, sizeof(what), "'%s' unmounts", sequences[i].name);
        check(rc == 0, what);
        snprintf(what, sizeof(what), "'%s' leaves a volume e2fsck calls clean", sequences[i].name);
        check(e2fsck_clean(wimg), what);
        if (!e2fsck_clean(wimg)) {
            ext2_fixture_head(fix_path("fsck.out"), 20);
        }
        snprintf(what, sizeof(what), "'%s' leaves a volume the reader walks", sequences[i].name);
        check(reader_walks(wimg), what);
        use_image(wimg, wbytes);
        check(mount_image() == 0 && (mnt.flags & VFS_MS_RDONLY) == 0,
              "the next mount finds the volume clean and writable");
        snprintf(what, sizeof(what), "'%s' reads back after a remount", sequences[i].name);
        check(i == 0 ? oracle_create_write() : oracle_after(i), what);
        unmount_image();
    }
    wbytes = build_image(wimg, 4096, 128, PROFILE, 16, "tree");
    use_image(wimg, wbytes);
    if (mount_rw() == 0) {
        unsigned char sb[16], sb2[16];

        /* s_free_blocks_count and s_free_inodes_count are at 12 and 16. */
        host_read(wimg, sb, 16, 1024 + 12);
        check((host_read(wimg, sb2, 2, 1024 + 58) == 2) && (sb2[0] & 1) == 0,
              "a mount for writing clears the valid bit before it is published");
        seq_create_write();
        seq_mkdir_rmdir();
        host_read(wimg, sb2, 16, 1024 + 12);
        check(memcmp(sb, sb2, 8) == 0,
              "a sequence leaves the superblock's free counts as they were");
        OPS->sync(&mnt);
        host_read(wimg, sb2, 16, 1024 + 12);
        check(memcmp(sb, sb2, 8) != 0, "a sync writes the counts");
        unmount_rw();
        host_read(wimg, sb2, 2, 1024 + 58);
        check((sb2[0] & 1) == 1, "an unmount sets the valid bit again");
    }

    wbytes = build_image(wimg, 4096, 128, PROFILE, 16, "tree");
    use_image(wimg, wbytes);
    if (mount_rw() == 0) {
        seq_create_write();
        seq_mkdir_rmdir();
        seq_symlink();
        unmount_rw();
        check(debugfs_lists(wimg, "/", "new.txt") && debugfs_lists(wimg, "/newdir", "inner") &&
                  debugfs_lists(wimg, "/", "quick"),
              "debugfs lists the file, the directory and the link this driver wrote");
    }

    wbytes = build_image(wimg, 4096, 128, PROFILE, 16, "tree");
    use_image(wimg, wbytes);
    if (mount_rw() == 0) {
        seq_nodes();
        unmount_rw();
        check(debugfs_says(wimg, "stat /fifo", "Type: FIFO") &&
                  debugfs_says(wimg, "stat /sock", "Type: socket") &&
                  debugfs_says(wimg, "stat /null", "number: 01:03") &&
                  debugfs_says(wimg, "stat /wide", "number: 300:70000"),
              "debugfs reads the FIFO, the socket and both device numbers this driver wrote");
    }
}

/* A copy of a volume of up to 16 MiB, taken by `snapshot`. */
static unsigned char snap[1 << 24];

/** Whether the first `bytes` bytes of `img` equal `snap`. */
static int image_equals_snap(const char *img, uint64_t bytes)
{
    void *f = fopen(img, "rb");
    uint64_t at = 0;

    if (!f) {
        return 0;
    }
    while (at < bytes) {
        unsigned long take = bytes - at < 65536 ? (unsigned long)(bytes - at) : 65536;

        if (fread(bbuf, 1, take, f) != take || memcmp(bbuf, snap + at, take) != 0) {
            fclose(f);
            return 0;
        }
        at += take;
    }
    fclose(f);
    return 1;
}

/** One byte of the image file, or 0 when it cannot be read. */
static unsigned image_byte(const char *img, long off)
{
    void *f = fopen(img, "rb");
    unsigned char c = 0;

    if (f) {
        if (fseek(f, off, SEEK_SET_) != 0 || fread(&c, 1, 1, f) != 1) {
            c = 0;
        }
        fclose(f);
    }
    return c;
}

/** Copy the first `bytes` bytes of `img` into `snap`. */
static void snapshot(const char *img, uint64_t bytes)
{
    void *f = fopen(img, "rb");

    if (f) {
        fread(snap, 1, (unsigned long)bytes, f);
        fclose(f);
    }
}

/**
 * The edges of the file limit L of 4096-byte blocks, 0x1FF7FDFE000, on a
 * new file of the mounted volume, as Linux's `s_maxbytes` draws them: a
 * truncate to L+1 is `EFBIG`, a two-byte write at L-1 writes the one byte
 * below L and returns 1 (`generic_write_check_limits`), and a file larger
 * than L, as a volume written under a larger limit holds, shrinks to a size
 * still above L but does not grow (`inode_newsize_ok`). The file ends
 * empty.
 */
static void limit_edge_rows(void)
{
    struct ext2_node *e;
    struct vfs_stat st;
    unsigned char two[2] = {0, 0};
    void *h = 0;
    int rc;

    e = make_file(mnt.root, "limit.bin", &rc);
    check(e != 0, "a file for the limit's edges is made");
    if (!e) {
        return;
    }
    if (OPS->open(&mnt, e, 1, &h) == 0) {
        check(OPS->truncate(&mnt, h, 0x1FF7FDFE001ull) == -EFBIG,
              "a truncate past the limit answers EFBIG");
        check(OPS->write(&mnt, h, pattern, 2, 0x1FF7FDFDFFFull) == 1,
              "a two-byte write at the limit less one writes one byte");
        check(OPS->getattr(&mnt, e, &st) == 0 && st.size == 0x1FF7FDFE000ull,
              "and the size is the limit");
        check(read_at(e, two, 2, 0x1FF7FDFDFFFull) == 1 && two[0] == pattern[0],
              "and the byte below the limit reads back");
        ext2_node_lock(e);
        ext2_node_size_set(e, 0x1FF7FE00000ull);
        rc = ext2_inode_store(ext2_em_of(&mnt), e);
        ext2_node_unlock(e);
        check(rc == 0, "the file is given the size L+8192 an older limit allowed");
        check(OPS->truncate(&mnt, h, 0x1FF7FDFF000ull) == 0 && OPS->getattr(&mnt, e, &st) == 0 &&
                  st.size == 0x1FF7FDFF000ull,
              "a truncate of the oversized file to L+4096 shrinks it");
        check(OPS->truncate(&mnt, h, 0x1FF7FE00000ull) == -EFBIG,
              "and a truncate back to L+8192 answers EFBIG");
        check(OPS->truncate(&mnt, h, 0) == 0 && OPS->getattr(&mnt, e, &st) == 0 && st.size == 0,
              "and a truncate to 0 empties it");
        OPS->release(&mnt, h);
    }
    OPS->put(&mnt, e);
}

/**
 * The failures the writer must decide before it writes anything, on a
 * volume of 4 MiB: a name of 256 bytes is `ENAMETOOLONG` and a write at
 * the format's file limit `EFBIG`, each leaving the volume byte for byte
 * as it was; a truncate past the limit, a write astride it and a shrink of
 * a file above it are `limit_edge_rows`. A byte written at 4 GiB is
 * accepted, sets large_file and keeps the high half of the size across a
 * remount. A caller who is not root gets `ENOSPC` at the reserved blocks,
 * and root writes into them. A hundred new files are held at once, and the
 * volume ends clean.
 */
static void refusal_write_rows(void)
{
    char name[300];
    int i, rc, err;
    void *n;
    void *h;

    wbytes = build_image(wimg, 4096, 128, PROFILE, 4, "tree");
    use_image(wimg, wbytes);
    check(mount_rw() == 0, "the small volume mounts for writing");
    if (!mnt.root) {
        return;
    }
    bcache_sync(&dev);
    snapshot(wimg, wbytes);
    for (i = 0; i < 256; i++) {
        name[i] = 'n';
    }
    name[256] = '\0';
    rc = OPS->create(&mnt, mnt.root, name, 0644, &n);
    check(rc == -ENAMETOOLONG, "a name of 256 bytes answers ENAMETOOLONG");
    bcache_sync(&dev);
    check(image_equals_snap(wimg, wbytes), "and the volume is as it was");
    name[255] = '\0';
    n = make_file(mnt.root, name, &rc);
    check(n != 0, "a name of 255 bytes is made");
    if (n) {
        OPS->put(&mnt, n);
    }
    n = walk("/hello.txt", &err);
    if (n) {
        bcache_sync(&dev);
        snapshot(wimg, wbytes);
        check(write_at(n, pattern, 1, 0x1FF7FDFE000ull) == -EFBIG &&
                  memcmp(last_warn,
                         "write: offset 2095101 MiB is past the file limit of "
                         "2095101 MiB for 4096-byte blocks",
                         82) == 0,
              "a write at the format's limit answers EFBIG by name");
        bcache_sync(&dev);
        check(image_equals_snap(wimg, wbytes), "and the volume is as it was");
        limit_edge_rows();
        check(write_at(n, pattern, 1, 0x100000000ull) == 0, "a byte written at 4 GiB is accepted");
        bcache_sync(&dev);
        {
            struct vfs_stat st;
            unsigned char one[1] = {0xFF};

            check(OPS->getattr(&mnt, n, &st) == 0 && st.size == 0x100000001ull,
                  "and the size is 4 GiB plus one");
            check(read_at(n, one, 1, 0x100000000ull) == 1 && one[0] == pattern[0],
                  "and it reads back");
            check(read_at(n, one, 1, 0x80000000ull) == 1 && one[0] == 0,
                  "and the hole reads zeros");
            check((image_byte(wimg, 1024 + 100) & 0x02) != 0,
                  "and the superblock has the large_file flag");
        }
        OPS->put(&mnt, n);
        unmount_image();
        check(mount_rw() == 0, "the volume remounts");
        n = walk("/hello.txt", &err);
        if (n) {
            struct vfs_stat st;

            check(OPS->getattr(&mnt, n, &st) == 0 && st.size == 0x100000001ull,
                  "and the high size half was stored");
            OPS->put(&mnt, n);
        }
    }
    proc_a.euid = 1000;
    n = make_file(mnt.root, "filler.bin", &rc);
    if (n) {
        uint64_t off = 0;
        int last = 0;

        while (off < 8u * 1024 * 1024) {
            last = write_at(n, pattern, 4096, off);
            if (last != 0) {
                break;
            }
            off += 4096;
        }
        check(last == -ENOSPC, "a caller who is not root answers ENOSPC at the reserve");
        proc_a.euid = 0;
        rc = write_at(n, pattern, 4096, off);
        if (rc != 0) {
            printf("  root's write: %d (%s)\n", rc, last_warn);
        }
        check(rc == 0, "and root writes into it");
        OPS->put(&mnt, n);
    }
    proc_a.euid = 0;
    {
        void *held[100];
        int took = 0;

        for (i = 0; i < 100; i++) {
            char nm[32];

            snprintf(nm, sizeof(nm), "p%d.txt", i);
            n = make_file(mnt.root, nm, &rc);
            if (!n) {
                break;
            }
            held[took++] = n;
        }
        check(took == 100, "a hundred new files are held at once");
        for (i = 0; i < took; i++) {
            OPS->put(&mnt, held[i]);
        }
    }
    rc = OPS->open(&mnt, mnt.root, 0, &h);
    if (rc == 0) {
        OPS->release(&mnt, h);
    }
    check(unmount_rw() == 0, "the small volume unmounts");
    check(e2fsck_clean(wimg), "and e2fsck calls it clean");
}

/* ---- the interruption matrix ---- */

static int copy_file(const char *src, const char *dst);

/**
 * Copy the matrix's volume to the write rows' image and return its size.
 * The volume is the fixture with its free blocks full of random bytes that
 * would read as data, so that a size published over bytes not yet synced
 * reads junk and not the zeros a fresh image has. It is made once, by
 * writing and removing a file of 14 MB, and a free block well past the
 * tree must then hold nonzero bytes, or the matrix would run over zeros and
 * its oracle would accept a size published before its data. Exits the test
 * when a step fails.
 */
static uint64_t build_junk_image(void)
{
    static uint64_t bytes;

    if (bytes == 0) {
        char cmd[700];

        unsigned char probe[64];
        int i, nonzero = 0;

        bytes = build_image(fix_path("junk.ext2"), 4096, 128, PROFILE, 16, "tree");
        snprintf(cmd, sizeof cmd, "write %s junk", fix_path("junk.bin"));
        if (!ext2_fixture_noise(fix_path("junk.bin"), 14000000) ||
            !ext2_fixture_debugfs(fix_path("junk.ext2"), cmd, 1, 0) ||
            !ext2_fixture_debugfs(fix_path("junk.ext2"), "rm junk", 1, 0)) {
            printf("ext2_hosttest: the junk volume did not build\n");
            exit(1);
        }
        if (host_read(fix_path("junk.ext2"), probe, sizeof(probe), 3500L * 4096) !=
            (long)sizeof(probe)) {
            printf("ext2_hosttest: the junk volume did not read\n");
            exit(1);
        }
        for (i = 0; i < (int)sizeof(probe); i++) {
            nonzero |= probe[i] != 0;
        }
        if (!nonzero) {
            printf("ext2_hosttest: the junk volume's free blocks are zeros\n");
            exit(1);
        }
    }
    if (!copy_file(fix_path("junk.ext2"), fix_path("w.ext2"))) {
        printf("ext2_hosttest: the junk volume did not copy\n");
        exit(1);
    }
    return bytes;
}

/**
 * Whether a crashed sequence's file at `path` is absent, a state before
 * its name was written, or reads either the `blen` bytes it had before the
 * sequence or at most the `alen` bytes it was to have after it, with a zero
 * wherever a byte was not yet synced. Anything else is junk that a size was
 * published over. A `before` of NULL means the file had no bytes before.
 */
static int crash_bytes_ok(const char *path, const unsigned char *before, uint32_t blen,
                          const unsigned char *after, uint32_t alen)
{
    int err, got;
    void *n = walk(path, &err);
    uint32_t i;
    int as_before = 1, as_after = 1;

    if (!n) {
        return 1;
    }
    got = read_at(n, abuf, sizeof(abuf), 0);
    OPS->put(&mnt, n);
    if (got < 0) {
        return 0;
    }
    if (before == 0 || (uint32_t)got != blen) {
        as_before = 0;
    }
    for (i = 0; as_before && i < (uint32_t)got; i++) {
        if (abuf[i] != before[i]) {
            as_before = 0;
        }
    }
    if ((uint32_t)got > alen) {
        as_after = 0;
    }
    for (i = 0; as_after && i < (uint32_t)got; i++) {
        if (abuf[i] != after[i] && abuf[i] != 0) {
            as_after = 0;
        }
    }
    if (!as_before && !as_after) {
        printf("  %s: %d bytes that are neither before nor after\n", path, got);
    }
    return as_before || as_after;
}

/**
 * Whether a node of the special-files sequence at a stop is absent, or
 * named with its whole mode, its device number and size 0, since mknod
 * writes the inode whole before the entry that names it.
 */
static int crash_node_ok(const char *path, uint32_t mode, uint32_t rdev)
{
    int err;
    void *n = walk(path, &err);

    if (!n) {
        return err == -ENOENT;
    }
    OPS->put(&mnt, n);
    return node_is(path, mode, rdev);
}

/**
 * Whether the files of sequence `i`, stopped after some write, read only
 * what `crash_bytes_ok` and `crash_node_ok` allow. The truncate sequence
 * wrote 40000 bytes, cut them to 5000 and grew the file to 7000, so its
 * file of 40000 bytes is a state too.
 */
static int crash_ok(int i)
{
    static unsigned char want[300000];

    switch (i) {
    case 0:
        __builtin_memcpy(want, pattern, 100);
        __builtin_memcpy(want + 100, pattern, 10000);
        return crash_bytes_ok("/new.txt", 0, 0, want, 10100);
    case 1:
        return crash_bytes_ok("/big.bin", 0, 0, pattern, 100000);
    case 2:
        __builtin_memset(want, 0, 100000);
        __builtin_memcpy(want + 100000, pattern, 16);
        return crash_bytes_ok("/holey.bin", 0, 0, want, 100016);
    case 3:
        __builtin_memcpy(want, pattern, 5000);
        __builtin_memset(want + 5000, 0, 2000);
        return crash_bytes_ok("/cut.bin", pattern, 40000, want, 7000);
    case 4:
        return crash_bytes_ok("/gone.txt", 0, 0, pattern, 12000);
    case 6:
        return crash_bytes_ok("/from.txt", 0, 0, pattern, 3000) &&
               crash_bytes_ok("/to.txt", 0, 0, pattern, 3000) &&
               crash_bytes_ok("/dir/moved.txt", 0, 0, pattern, 3000);
    case 7:
        return crash_bytes_ok("/src.txt", 0, 0, pattern, 2000) &&
               crash_bytes_ok("/victim.txt", pattern + 100, 9000, pattern, 2000);
    case 8:
        return crash_bytes_ok("/mover/inside.txt", 0, 0, pattern, 500) &&
               crash_bytes_ok("/dir/sub/moved/inside.txt", 0, 0, pattern, 500) &&
               crash_bytes_ok("/empty/inside.txt", 0, 0, pattern, 500);
    case 9:
        return crash_bytes_ok("/one.txt", 0, 0, pattern, 700) &&
               crash_bytes_ok("/two.txt", 0, 0, pattern, 700);
    case 11:
        return crash_bytes_ok("/temp.txt", 0, 0, pattern, 8000);
    case 13:
        return crash_node_ok("/fifo", 0010644, 0) && crash_node_ok("/sock", 0140755, 0) &&
               crash_node_ok("/null", 0020666, NULL_RDEV) &&
               crash_node_ok("/wide", 0060600, WIDE_RDEV);
    default:
        return 1;
    }
}

/**
 * Copy `src` to `dst`; whether every byte was written. The copy is made
 * here rather than by cp, since the matrix makes one per stop.
 */
static int copy_file(const char *src, const char *dst)
{
    void *in = fopen(src, "rb");
    void *out = in ? fopen(dst, "wb") : 0;
    unsigned long got;
    int ok = out != 0;

    while (ok && (got = fread(abuf, 1, sizeof(abuf), in)) > 0) {
        ok = fwrite(abuf, 1, got, out) == got;
    }
    if (in) {
        fclose(in);
    }
    if (out) {
        fclose(out);
    }
    return ok;
}

/**
 * Whether the mount's check accepts a copy of the crash state in `img`: a
 * writable mount, or a read-only one whose fault is one the order writes
 * in more than one step: a size or an i_blocks after the pointers, and,
 * where `counts_written` says the sequence writes counts apart from their
 * entries, a link count after its entry or a directory's two entries and
 * its `..` across a move.
 */
static int check_accepts(const char *img, uint64_t bytes, int counts_written)
{
    static const char *const counts[] = {
        "a link count that does not match its names",
        "a directory link count that does not match its entries",
        "named twice with one link",
        "a directory with two parents",
        "a second entry that is not the parent",
    };
    static const char *const sizes[] = {
        "a block past the size",
        "a pointer block past the size",
        "i_blocks that does not match its pointers",
    };
    int ok, i;

    if (!copy_file(img, fix_path("check.ext2"))) {
        return 0;
    }
    use_image(fix_path("check.ext2"), bytes);
    last_check[0] = 0;
    if (mount_image() != 0) {
        printf("  the check refused the stop: %s\n", last_warn);
        return 0;
    }
    ok = (mnt.flags & VFS_MS_RDONLY) == 0;
    for (i = 0; !ok && i < 3; i++) {
        ok = strstr(last_check, sizes[i]) != 0;
    }
    for (i = 0; !ok && counts_written && i < 5; i++) {
        ok = strstr(last_check, counts[i]) != 0;
    }
    if (!ok) {
        printf("  the check found: %s\n", last_check);
    }
    unmount_image();
    return ok;
}

/**
 * Whether the file sequence `i` made, moved or linked is reachable by any
 * of its names on the mounted volume; 0 for a sequence with no such file.
 */
static int names_present(int i)
{
    int err;

    switch (i) {
    case 0:
        return walk("/new.txt", &err) != 0;
    case 5:
        return walk("/newdir", &err) != 0;
    case 10:
        return walk("/quick", &err) != 0;
    case 13:
        return walk("/fifo", &err) != 0;
    case 6:
        return walk("/from.txt", &err) || walk("/to.txt", &err) || walk("/dir/moved.txt", &err);
    case 7:
        return walk("/src.txt", &err) || walk("/victim.txt", &err);
    case 8:
        return walk("/mover/inside.txt", &err) || walk("/dir/sub/moved/inside.txt", &err) ||
               walk("/empty/inside.txt", &err);
    case 9:
        return walk("/one.txt", &err) || walk("/two.txt", &err);
    default:
        return 0;
    }
}

/**
 * Whether `e2fsck -fy` repairs a copy of the stop in `img`, exiting 2 or
 * less, and the repaired copy mounts with every base file intact. When
 * `named_before`, the sequence's file had a name in the crash state, and it
 * must keep one after the repair: what the order of writes promises for
 * rename and link.
 */
static int repaired_ok(const char *img, uint64_t bytes, int i, int stop, int named_before)
{
    int rc;

    if (!copy_file(img, fix_path("repair.ext2")) ||
        (rc = ext2_fixture_fsck(fix_path("repair.ext2"), "-fy", fix_path("repair.out"))) < 0 ||
        rc > 2) {
        printf("  e2fsck -fy could not repair '%s' stopped after write %d\n", sequences[i].name,
               stop);
        return 0;
    }
    use_image(fix_path("repair.ext2"), bytes);
    rc = mount_image();
    if (rc != 0) {
        return 0;
    }
    rc = base_intact();
    if (rc && named_before && !names_present(i)) {
        printf("  '%s' stopped after write %d: the file lost every name\n", sequences[i].name,
               stop);
        rc = 0;
    }
    unmount_image();
    return rc;
}

/* ---- the dirty-volume rows ---- */

/** Run debugfs -w over `img` with the requests in `script`, one per line. */
static int debugfs_script(const char *img, const char *script)
{
    void *f = fopen(fix_path("dbg.cmd"), "wb");

    if (!f) {
        return 0;
    }
    fwrite(script, 1, strlen(script), f);
    fclose(f);
    {
        const char *argv[] = {host_tool_path("debugfs"), "-w", "-f", fix_path("dbg.cmd"), img, 0};
        return host_command(argv, 0, 0, 0, 0) == 0;
    }
}

/** The number right after `key` in debugfs's answer to `request`, or -1. */
static long debugfs_number(const char *img, const char *request, const char *key)
{
    char line[512];
    void *f;
    long v = -1;

    if (!ext2_fixture_debugfs(img, request, 0, fix_path("dbg.out")))
        return -1;
    f = fopen(fix_path("dbg.out"), "rb");
    if (!f) {
        return -1;
    }
    while (fgets(line, sizeof(line), f)) {
        char *at = strstr(line, key);

        if (at && at[strlen(key)] >= '0' && at[strlen(key)] <= '9') {
            v = atol(at + strlen(key));
            break;
        }
    }
    fclose(f);
    return v;
}

/** The superblock's state field on disk, or -1. */
static int disk_state(const char *img)
{
    unsigned char b[2];

    if (host_read(img, b, 2, 1024 + 58) != 2) {
        return -1;
    }
    return b[0] | (b[1] << 8);
}

/** Write `/fill` until the volume is full; 0 when ENOSPC ended it. */
static int fill_volume(void)
{
    int rc;
    void *n = make_file(mnt.root, "fill", &rc);
    uint64_t off = 0;

    if (!n) {
        return rc;
    }
    for (;;) {
        rc = write_at(n, pattern, 4096, off);
        if (rc != 0) {
            break;
        }
        off += 4096;
    }
    OPS->put(&mnt, n);
    return rc == -ENOSPC ? 0 : rc;
}

/** Make a tree volume with its state 0, not valid, and `script` applied. */
static uint64_t dirty_image(const char *img, const char *script)
{
    uint64_t bytes = build_image(img, 4096, 128, PROFILE, 16, "tree");
    char cmd[1024];

    snprintf(cmd, sizeof(cmd), "ssv state 0\n%s", script);
    if (!debugfs_script(img, cmd)) {
        printf("ext2_hosttest: debugfs failed on %s\n", img);
        exit(1);
    }
    return bytes;
}

static void snapshot(const char *img, uint64_t bytes);
static int image_equals_snap(const char *img, uint64_t bytes);

/**
 * Require the dirty volume `img`, of `bytes`, to mount writable with
 * `fault` named by the check and Linux's warning for a volume with errors,
 * the valid bit clear and the error bit set on the disk; to go read-only
 * and writable again by remounts, the second checking it again; and to
 * keep the error bit, without the valid bit, through the unmount.
 */
static void fault_on(const char *img, uint64_t bytes, const char *fault, const char *what)
{
    int rc;

    use_image(img, bytes);
    last_check[0] = 0;
    last_warn[0] = 0;
    rc = mount_rw();
    if (rc != 0 || (mnt.flags & VFS_MS_RDONLY) != 0 || !strstr(last_check, fault) ||
        !streq(last_warn, "mounting with errors, running e2fsck is recommended") ||
        disk_state(img) != 2) {
        printf("  mount %d, flags 0x%x, state %d, check: %s, warn: %s\n", rc, (unsigned)mnt.flags,
               disk_state(img), last_check, last_warn);
        check(0, what);
    } else {
        check(1, what);
        last_check[0] = 0;
        check(OPS->remount(&mnt, rw_flags | VFS_MS_RDONLY, NULL) == 0 &&
                  OPS->remount(&mnt, rw_flags, NULL) == 0 && strstr(last_check, fault) != 0 &&
                  mnt.write_error == 0,
              "and remounts read-only and writable, checked again");
    }
    if (rc == 0 || mnt.root) {
        unmount_rw();
    }
    check(disk_state(img) == 2, "and its unmount keeps the error bit and leaves it not valid");
}

/** Make a dirty volume with `script` applied, and require it to fault as
 * `fault_on` does. */
static void fault_row(const char *img, const char *script, const char *fault, const char *what)
{
    fault_on(img, dirty_image(img, script), fault, what);
}

/**
 * Mount volumes whose valid bit is clear, and require the check to do
 * what each needs:
 *
 * - a consistent volume is checked, mounts writable, and is valid again
 *   at unmount;
 * - on a machine too short of memory for the check's workspace it mounts
 *   writable unchecked, by name, and its unmount leaves the valid bit
 *   clear;
 * - a tree deeper than the check walks mounts writable unchecked as well;
 * - wrong free counts are recounted from the bitmaps, not used, and
 *   written back;
 * - a lost block is kept out of the allocator's reach and the error bit
 *   stays set, through a mount and an unmount, until `e2fsck -p` repairs
 *   it;
 * - an orphan inode is kept whole, with its blocks, for e2fsck;
 * - each fault (a used block marked free, a block claimed twice, a reached
 *   inode marked free, wrong link counts, a directory with two parents, a
 *   size or i_blocks behind its pointers, an entry of the wrong form) is
 *   named and mounts writable with the error bit set, as Linux mounts a
 *   volume with errors;
 * - a read-only mount runs no check and writes nothing.
 */
static int lost_block_reported(void)
{
    return ext2_fixture_contains(fix_path("fsck.out"), "-(4000)") ||
           ext2_fixture_contains(fix_path("fsck.out"), "-4000");
}

static int orphan_reported(long ino)
{
    char text[128];
    snprintf(text, sizeof text, "Unattached inode %ld", ino);
    return ext2_fixture_contains(fix_path("fsck.out"), text) &&
           !ext2_fixture_contains(fix_path("fsck.out"), "Multiply");
}

/**
 * Mount the dirty volume `img` with a tree of 34 directories, deeper than
 * the check walks: the check stops by name, which is no fault of the
 * volume, the volume mounts writable unchecked with Linux's warning and
 * takes a create, and its unmount leaves the valid bit clear, so that the
 * next writable mount checks it again.
 */
static void deep_row(const char *img)
{
    static char script[4096];
    uint64_t bytes = build_image(img, 4096, 128, PROFILE, 16, "tree");
    void *n;
    int at, i, j, rc;

    at = snprintf(script, sizeof(script), "ssv state 0\n");
    for (i = 1; i <= 34; i++) {
        at += snprintf(script + at, sizeof(script) - at, "mkdir /");
        for (j = 1; j < i; j++) {
            at += snprintf(script + at, sizeof(script) - at, "d/");
        }
        at += snprintf(script + at, sizeof(script) - at, "d\n");
    }
    if (!debugfs_script(img, script)) {
        check(0, "debugfs makes a tree of 34 directories");
        return;
    }
    use_image(img, bytes);
    last_check[0] = 0;
    last_warn[0] = 0;
    rc = mount_rw();
    check(rc == 0 && (mnt.flags & VFS_MS_RDONLY) == 0 &&
              strstr(last_check, "a directory deeper than the check walks") != 0 &&
              streq(last_warn, "mounting unchecked, running e2fsck is recommended"),
          "a dirty tree deeper than the check walks mounts writable unchecked by name");
    if (rc != 0) {
        return;
    }
    n = make_file(mnt.root, "new", &rc);
    check(n != 0, "and takes a create");
    if (n) {
        OPS->put(&mnt, n);
    }
    check(unmount_rw() == 0 && disk_state(img) == 0,
          "and its unmount leaves the valid bit clear for the next mount to check");
}

static void dirty_rows(void)
{
    const char *img = fix_path("dirty.ext2");
    uint64_t bytes;
    unsigned char b[4];
    char script[256];
    long blk, ino, table, root_block;
    struct vfs_statfs st;
    int rc;

    bytes = dirty_image(img, "");
    use_image(img, bytes);
    last_check[0] = 0;
    check(mount_rw() == 0 && (mnt.flags & VFS_MS_RDONLY) == 0 && streq(last_check, "check: clean"),
          "a consistent volume whose valid bit is clear is checked and mounts writable");
    rc = unmount_rw();
    check(rc == 0 && disk_state(img) == 1 && e2fsck_clean(img),
          "and its unmount marks it valid again");

    bytes = dirty_image(img, "");
    use_image(img, bytes);
    last_check[0] = 0;
    last_warn[0] = 0;
    host_free_frames = 0;
    rc = mount_rw();
    host_free_frames = 1u << 20;
    if (rc != 0 || (mnt.flags & VFS_MS_RDONLY) != 0 ||
        !strstr(last_check, "check: no memory for the") ||
        !streq(last_warn, "mounting unchecked, running e2fsck is recommended")) {
        printf("  mount %d, flags 0x%x, check: %s, warn: %s\n", rc, (unsigned)mnt.flags, last_check,
               last_warn);
        check(0, "a dirty volume on a machine short of memory mounts writable unchecked by name");
    } else {
        check(1, "a dirty volume on a machine short of memory mounts writable unchecked by name");
    }
    if (rc == 0) {
        check(unmount_rw() == 0 && disk_state(img) == 0,
              "and its unmount leaves the valid bit clear for the next mount to check");
    }

    deep_row(img);

    bytes = dirty_image(img, "ssv free_blocks_count 1\nssv free_inodes_count 1\n"
                             "set_bg 0 free_blocks_count 1\nset_bg 0 free_inodes_count 1\n");
    use_image(img, bytes);
    check(mount_rw() == 0 && OPS->statfs(&mnt, &st) == 0 && st.bfree > 1000,
          "a dirty volume's free counts are recounted from the bitmaps");
    check(unmount_rw() == 0 && e2fsck_clean(img), "and written back, so e2fsck finds them right");

    bytes = dirty_image(img, "setb 4000\n");
    use_image(img, bytes);
    last_check[0] = 0;
    check(mount_rw() == 0 && (mnt.flags & VFS_MS_RDONLY) == 0 &&
              strstr(last_check, "1 lost blocks, 0 orphan inodes") != 0,
          "a lost block mounts writable with the block counted");
    check(fill_volume() == 0 && unmount_rw() == 0 && disk_state(img) == 2 &&
              e2fsck_status(img) == 4 && lost_block_reported(),
          "and filling the volume leaves it lost: the error bit set, e2fsck still owed it");
    use_image(img, bytes);
    last_check[0] = 0;
    check(mount_rw() == 0 && strstr(last_check, "1 lost blocks") != 0 && unmount_rw() == 0 &&
              disk_state(img) == 2,
          "the error bit survives a mount and an unmount");
    check(ext2_fixture_fsck(fix_path("dirty.ext2"), "-fp", fix_path("repair.out")) == 1,
          "e2fsck -p repairs it");
    use_image(img, bytes);
    last_check[0] = 0;
    check(mount_rw() == 0 && (mnt.flags & VFS_MS_RDONLY) == 0 && last_check[0] == 0 &&
              unmount_rw() == 0 && disk_state(img) == 1,
          "and the repaired volume mounts without a check and unmounts valid");

    bytes = build_image(img, 4096, 128, PROFILE, 16, "tree");
    ino = debugfs_number(img, "stat /dir/big.bin", "Inode: ");
    blk = debugfs_number(img, "blocks /dir/big.bin", "");
    bytes = dirty_image(img, "unlink /dir/big.bin\n");
    use_image(img, bytes);
    last_check[0] = 0;
    check(ino > 0 && blk > 0 && mount_rw() == 0 && (mnt.flags & VFS_MS_RDONLY) == 0 &&
              strstr(last_check, "0 lost blocks, 1 orphan inodes") != 0,
          "an orphan inode mounts writable with the orphan counted");
    check(fill_volume() == 0 && unmount_rw() == 0 && e2fsck_status(img) == 4 &&
              orphan_reported(ino),
          "and filling the volume leaves the orphan and its blocks to e2fsck");

    bytes = build_image(img, 4096, 128, PROFILE, 16, "tree");
    blk = debugfs_number(img, "blocks /hello.txt", "");
    snprintf(script, sizeof(script), "unlink /hello.txt\nunlink /hello.link\nfreeb %ld\n", blk);
    fault_row(img, script, "a used block the bitmap calls free",
              "an orphan whose block the bitmap calls free mounts writable with errors");
    if (host_read(img, b, 4, 4096 + 8) == 4) {
        table = b[0] | (b[1] << 8) | (b[2] << 16) | ((long)b[3] << 24);
        snprintf(script, sizeof(script), "sif /dir/big.bin block[0] %ld\n", table);
        fault_row(img, script, "a block claimed twice",
                  "a file pointing into the inode table mounts writable with errors");
    } else {
        check(0, "the descriptor read for the inode-table row");
    }
    fault_row(img, "freei /dir/big.bin\n", "a reached inode the bitmap calls free",
              "a reached inode whose bit is clear mounts writable with errors");
    fault_row(img, "sif /dir/big.bin links_count 3\n", "a link count that does not match its names",
              "a link count above its names mounts writable with errors");
    fault_row(img, "sif /dir links_count 9\n",
              "a directory link count that does not match its entries",
              "a directory count above its entries mounts writable with errors");
    fault_row(img, "link /dir/sub /many/twice\nsif /many links_count 3\n",
              "a directory with two parents",
              "a directory named twice mounts writable with errors");
    fault_row(img, "sif /dir/big.bin size 100\n", "past the size",
              "a size behind its pointers mounts writable with errors");
    fault_row(img, "sif /dir/big.bin i_blocks 8\n", "i_blocks that does not match its pointers",
              "an i_blocks behind its pointers mounts writable with errors");
    bytes = build_image(img, 4096, 128, PROFILE, 16, "tree");
    root_block = debugfs_number(img, "blocks /", "");
    if (root_block > 0) {
        b[0] = 6;
        b[1] = 0;
        bytes = dirty_image(img, "");
        poke(img, root_block * 4096 + 4, b, 2);
        fault_on(img, bytes, "an entry of the wrong form",
                 "an entry of the wrong form mounts writable with errors");
    } else {
        check(0, "the root's block for the entry-form row");
    }

    bytes = dirty_image(img, "setb 4000\n");
    snapshot(img, bytes);
    use_image(img, bytes);
    last_check[0] = 0;
    check(mount_ro() == 0 && last_check[0] == 0,
          "a read-only mount of a dirty volume is not checked");
    unmount_rw();
    check(image_equals_snap(img, bytes), "and writes nothing");
}

static void sync_remount_row(void);

/**
 * Mount a clean volume read-only, read a file and a deep path on it, sync
 * the file through a description and the volume, and unmount it as the VFS
 * does; and mount a dirty volume read-only, which writes nothing, and
 * remount it writable, which its check's fault leaves writable with the
 * error bit set. The read-only mounts must leave each volume byte for byte
 * as it was, since Linux's ext2 writes nothing to a read-only mount. Then
 * `sync_remount_row`.
 */
static void readonly_rows(void)
{
    const char *img = fix_path("ro.ext2");
    uint64_t bytes = build_image(img, 4096, 128, PROFILE, 16, "tree");
    struct vfs_statfs st;
    void *n, *h = 0;
    int err, rc;

    snapshot(img, bytes);
    use_image(img, bytes);
    rc = mount_ro();
    check(rc == 0 && (mnt.flags & VFS_MS_RDONLY) != 0, "a clean volume mounts read-only");
    if (rc == 0) {
        n = walk("/dir/big.bin", &err);
        check(n && OPS->open(&mnt, n, 0, &h) == 0 && read_at(n, abuf, 4096, 0) == 4096 &&
                  OPS->fsync(&mnt, h) == 0,
              "a file of it reads and its description syncs");
        if (h) {
            OPS->release(&mnt, h);
        }
        if (n) {
            OPS->put(&mnt, n);
        }
        n = walk("/dir/sub/deep.txt", &err);
        if (n) {
            OPS->put(&mnt, n);
        }
        check(n != 0 && OPS->statfs(&mnt, &st) == 0 && OPS->sync(&mnt) == 0 &&
                  make_file(mnt.root, "new", &rc) == 0 && rc == -EROFS,
              "a deep file is found, statfs and sync answer, and a create is EROFS");
        check(unmount_rw() == 0, "and it unmounts");
    }
    check(image_equals_snap(img, bytes), "and the volume is byte for byte as it was");

    bytes = dirty_image(img, "sif /dir/big.bin links_count 3\n");
    snapshot(img, bytes);
    use_image(img, bytes);
    last_check[0] = 0;
    rc = mount_ro();
    check(rc == 0 && last_check[0] == 0 && image_equals_snap(img, bytes),
          "a dirty volume mounts read-only unchecked and writes nothing");
    if (rc == 0) {
        last_warn[0] = 0;
        check(OPS->remount(&mnt, rw_flags, NULL) == 0 &&
                  strstr(last_check, "a link count that does not match its names") != 0 &&
                  streq(last_warn, "mounting with errors, running e2fsck is recommended") &&
                  disk_state(img) == 2,
              "a remount writable whose check finds a fault goes writable with the error bit set");
        mnt.flags = rw_flags;
        check(unmount_rw() == 0 && disk_state(img) == 2,
              "and its unmount keeps the error bit and leaves it not valid");
    }
    sync_remount_row();
}

/* The result of process B's sync in `sync_remount_row`. */
static int race_sync_rc;

/** Process B of `sync_remount_row`: a sync of the mount. */
static void race_sync(void)
{
    race_sync_rc = OPS->sync(&mnt);
}

/** At a write of block 0 under `em->super`, the remount's write of the
 * superblock, give process B the turn, once. */
static void race_super_turn(void)
{
    if (request_block == 0 && sleep_lock_held(&ext2_em_of(&mnt)->super)) {
        write_pause = 0;
        pass_turn();
    }
}

/**
 * A sync beside a remount read-only, which Linux orders by `s_umount`: the
 * remount gives the turn to a sync at its write of the superblock, the
 * sync sleeps on that block's buffer and resumes once the remount has
 * made the mount read-only. The sync must note nothing and record no
 * failure, so that a remount writable after it goes through, and the
 * volume must unmount clean. Before the sync read `writable` again under
 * `em->super`, its note of the superblock was refused with EROFS and
 * recorded as the mount's write error.
 */
static void sync_remount_row(void)
{
    const char *img = fix_path("race.ext2");
    uint64_t bytes = build_image(img, 4096, 128, PROFILE, 16, "tree");
    int rc;

    use_image(img, bytes);
    rc = mount_rw();
    check(rc == 0 && OPS->sync(&mnt) == 0, "the race volume mounts writable and syncs");
    if (rc != 0) {
        return;
    }
    race_sync_rc = 1;
    write_pause = race_super_turn;
    start_turns(race_sync);
    rc = OPS->remount(&mnt, rw_flags | VFS_MS_RDONLY, NULL);
    finish_turns();
    write_pause = 0;
    check(rc == 0 && second_wait && streq(second_wait, "buffer") && second_wait_block == 0,
          "a sync run at a remount's write of the superblock sleeps on its buffer");
    check(race_sync_rc == 0 && mnt.write_error == 0,
          "and, resumed on a mount made read-only, records no failure");
    mnt.flags = rw_flags | VFS_MS_RDONLY;
    check(OPS->remount(&mnt, rw_flags, NULL) == 0, "and a remount writable after it goes through");
    mnt.flags = rw_flags;
    check(unmount_rw() == 0 && disk_state(img) == 1 && e2fsck_clean(img),
          "and the volume unmounts clean");
}

/* PROFILE with `ext_attr`, for volumes whose inodes have attribute blocks. */
#define ATTR_PROFILE                                                                  \
    "sparse_super,large_file,filetype,^dir_index,resize_inode,ext_attr,^has_journal," \
    "^extent,^64bit,^flex_bg,^metadata_csum,^huge_file,^dir_nlink,^extra_isize"

/* The attribute blocks of `attr_image`: deep.txt's, and the shared one. */
static long attr_own, attr_shared;

/**
 * Make a tree volume with its valid bit clear and with attribute blocks as
 * Linux writes them for 128-byte inodes: debugfs's `ea_set` gives one each
 * to /dir/sub/deep.txt, the fast link /fast and the device /cold, and one
 * to /many/file0.txt, which the next `names - 1` files then share, with
 * `refs` written as its header's reference count.
 */
static uint64_t attr_image(const char *img, int names, unsigned refs)
{
    uint64_t bytes = build_image(img, 4096, 128, ATTR_PROFILE, 16, "tree");
    unsigned char b[4] = {0, 0, 0, 0};
    char script[512];
    int len = 0, i;

    if (!debugfs_script(img, "ssv state 0\nea_set /dir/sub/deep.txt user.a 1\n"
                             "ea_set /fast user.a 1\nea_set /cold user.a 1\n"
                             "ea_set /many/file0.txt user.a 1\n")) {
        printf("ext2_hosttest: debugfs ea_set failed on %s\n", img);
        exit(1);
    }
    attr_own = debugfs_number(img, "stat /dir/sub/deep.txt", "File ACL: ");
    attr_shared = debugfs_number(img, "stat /many/file0.txt", "File ACL: ");
    for (i = 1; i < names; i++) {
        len += snprintf(script + len, sizeof(script) - len,
                        "sif /many/file%d.txt file_acl %ld\nsif /many/file%d.txt i_blocks 16\n", i,
                        attr_shared, i);
    }
    if (attr_own <= 0 || attr_shared <= 0 || (len > 0 && !debugfs_script(img, script))) {
        printf("ext2_hosttest: the attribute blocks of %s were not made\n", img);
        exit(1);
    }
    b[0] = (unsigned char)refs;
    poke(img, attr_shared * 4096 + 4, b, 4);
    return bytes;
}

/** Unlink `name` in the directory at `path`; 0, or -errno. */
static int unlink_in(const char *path, const char *name)
{
    int rc;
    void *d = walk(path, &rc);

    if (!d) {
        return rc;
    }
    rc = OPS->unlink(&mnt, d, name);
    OPS->put(&mnt, d);
    return rc;
}

/** The reference count in the header of attribute block `blk`, or -1. */
static long attr_refs(const char *img, long blk)
{
    unsigned char b[4];

    if (host_read(img, b, 4, blk * 4096 + 4) != 4) {
        return -1;
    }
    return b[0] | (b[1] << 8) | (b[2] << 16) | ((long)b[3] << 24);
}

/**
 * Mount volumes whose inodes have extended-attribute blocks, as Linux and
 * `mke2fs -d` leave them, and require:
 *
 * - the check to count each block in its inode's `i_blocks` and to mark a
 *   shared block once, so that a sound volume is clean;
 * - a fast link with an attribute block to read from its pointer words;
 * - an unlink to free an unshared block and to lower a shared block's
 *   count, freeing it with its last inode, leaving e2fsck nothing to do;
 * - a shared block counting more inodes than name it to mount writable
 *   and counted, with the error bit set;
 * - a block named by more inodes than its count, or without its header's
 *   magic number, to mount read-only, by name.
 */
static void attr_rows(void)
{
    const char *img = fix_path("attr.ext2");
    char linkbuf[64];
    char script[128];
    uint64_t bytes;
    void *n;
    int rc;

    bytes = attr_image(img, 2, 2);
    check(e2fsck_clean(img), "a volume with shared and unshared attribute blocks is clean");
    use_image(img, bytes);
    last_check[0] = 0;
    rc = mount_rw();
    check(rc == 0 && (mnt.flags & VFS_MS_RDONLY) == 0 && streq(last_check, "check: clean"),
          "and its check counts every attribute block once and mounts it writable");
    if (rc != 0) {
        return;
    }
    n = walk("/fast", &rc);
    check(n && OPS->readlink(&mnt, n, linkbuf, sizeof(linkbuf)) == 9 && streq(linkbuf, "hello.txt"),
          "a fast link with an attribute block reads its target from the pointer words");
    check(n && OPS->readlink_cached(&mnt, n, linkbuf, sizeof(linkbuf)) == 9 &&
              streq(linkbuf, "hello.txt"),
          "and the cached walk reads it too");
    if (n) {
        OPS->put(&mnt, n);
    }
    check(OPS->symlink(&mnt, mnt.root, "quick", "hello.txt") == 0, "a fast link is made");
    n = walk("/quick", &rc);
    check(n && OPS->readlink_cached(&mnt, n, linkbuf, sizeof(linkbuf)) == 9 &&
              streq(linkbuf, "hello.txt"),
          "and the cached walk reads the link it made from the pointer words");
    if (n) {
        OPS->put(&mnt, n);
    }
    check(unlink_in("/", "quick") == 0, "and it unlinks");
    check(unlink_in("/dir/sub", "deep.txt") == 0 && unlink_in("/", "fast") == 0 &&
              unlink_in("/", "cold") == 0 && unlink_in("/many", "file1.txt") == 0 &&
              unmount_rw() == 0,
          "files, a fast link and a device with attribute blocks unlink");
    snprintf(script, sizeof(script), "testb %ld", attr_own);
    check(e2fsck_clean(img) && debugfs_says(img, script, "not in use") &&
              attr_refs(img, attr_shared) == 1,
          "and the volume is clean, an unshared block freed and a shared one's count lowered");
    use_image(img, bytes);
    check(mount_rw() == 0 && unlink_in("/many", "file0.txt") == 0 && unmount_rw() == 0,
          "the shared block's last inode unlinks");
    snprintf(script, sizeof(script), "testb %ld", attr_shared);
    check(e2fsck_clean(img) && debugfs_says(img, script, "not in use"),
          "and the volume is clean with the block freed");

    bytes = attr_image(img, 2, 3);
    use_image(img, bytes);
    last_check[0] = 0;
    check(mount_rw() == 0 && (mnt.flags & VFS_MS_RDONLY) == 0 &&
              streq(last_check, "check: 1 attribute blocks with counts above their inodes, kept "
                                "for e2fsck") &&
              unmount_rw() == 0 && disk_state(img) == 2,
          "a shared block counting an inode too many mounts writable, counted, the error bit set");
    fault_on(img, attr_image(img, 3, 2), "an attribute block named past its count",
             "a shared block named by more inodes than its count mounts read-only");
    fault_on(img, attr_image(img, 2, 1), "a block claimed twice",
             "a block named by two inodes and counting one mounts read-only");
    bytes = attr_image(img, 1, 1);
    poke(img, attr_own * 4096, (const unsigned char *)"\0\0\0\0", 4);
    fault_on(img, bytes, "an attribute block of the wrong form",
             "an attribute block without its magic number mounts read-only");
}

#include "ext2_xattr_rows.h"

/**
 * The interruption matrix. Each sequence but the one of many files, which
 * would need thousands of stops that the others cover, runs once to count
 * its writes, not the mount's own, and then once for each count k, on the
 * junk volume, stopped after write k. At a stop the cache is dropped as a
 * gone device's buffers are, so that the image is the crash state and
 * nothing unsynced reaches it. Four judges follow:
 *
 * - a read-only mount reads the base files, and the sequence's files read
 *   only what was written or zeros; past `op_mark`, the file has a name;
 * - `e2fsck -fn` judges the state, clean or with only allowed findings;
 * - `e2fsck -fy` repairs a copy with nothing outside the operation lost;
 * - the mount's check accepts a copy: writable, or read-only on a fault
 *   the order leaves between two of its writes.
 *
 * Last, a base file changed under the repair must fail `base_intact`, so
 * that the comparison is shown to look.
 */
static void matrix_rows(void)
{
    int i, k, total = 0;

    for (i = 0; i < (int)(sizeof(sequences) / sizeof(sequences[0])); i++) {
        int n, mark;

        if (i == 12) {
            continue;
        }
        wbytes = build_image(wimg, 4096, 128, PROFILE, 16, "tree");
        use_image(wimg, wbytes);
        stop_after = -1;
        if (mount_rw() != 0) {
            check(0, "the matrix's volume mounts");
            return;
        }
        writes_made = 0;
        op_mark = -1;
        sequences[i].run();
        n = writes_made;
        mark = op_mark;
        unmount_rw();
        for (k = 1; k <= n; k++) {
            char what[200];
            int status, named_before = 0;

            wbytes = build_junk_image();
            use_image(wimg, wbytes);
            stop_after = -1;
            if (mount_rw() != 0) {
                printf("  stop %d of '%s': %s\n", k, sequences[i].name, last_warn);
                check(0, "the matrix's volume mounts again");
                return;
            }
            writes_made = 0;
            stop_after = k;
            sequences[i].run();
            OPS->put(&mnt, mnt.root);
            OPS->unmount(&mnt);
            mnt.root = 0;
            bcache_device_gone(&dev);
            dev.flags = BLOCK_FAULT_SAFE;
            stop_after = -1;
            use_image(wimg, wbytes);
            snprintf(what, sizeof(what),
                     "'%s' stopped after write %d of %d mounts and reads its base",
                     sequences[i].name, k, n);
            if (mount_ro() == 0) {
                check(base_intact(), what);
                snprintf(what, sizeof(what),
                         "'%s' stopped after write %d of %d reads only what was written or zeros",
                         sequences[i].name, k, n);
                check(crash_ok(i), what);
                named_before = names_present(i);
                if (mark >= 0 && k > mark) {
                    snprintf(what, sizeof(what),
                             "'%s' stopped after write %d of %d keeps a name for its file",
                             sequences[i].name, k, n);
                    check(named_before, what);
                    named_before = 1;
                }
                unmount_image();
            } else {
                check(0, what);
            }
            status = e2fsck_status(wimg);
            snprintf(what, sizeof(what),
                     "'%s' stopped after write %d of %d is a state e2fsck can judge",
                     sequences[i].name, k, n);
            if (status != 0 && status != 4) {
                printf("  e2fsck -fn exited %d:\n", status);
                ext2_fixture_head(fix_path("fsck.out"), 14);
            }
            check((status == 0 || status == 4) && findings_allowed(sequences[i].counts), what);
            snprintf(what, sizeof(what),
                     "'%s' stopped after write %d of %d repairs with its base intact",
                     sequences[i].name, k, n);
            check(repaired_ok(wimg, wbytes, i, k, named_before), what);
            snprintf(what, sizeof(what),
                     "'%s' stopped after write %d of %d passes the check or fails it on a count",
                     sequences[i].name, k, n);
            check(check_accepts(wimg, wbytes, sequences[i].counts), what);
            total++;
        }
    }
    printf("ext2_hosttest: %d stops walked\n", total);
    wbytes = build_image(wimg, 4096, 128, PROFILE, 16, "tree");
    ext2_fixture_debugfs(wimg, "write /dev/null hello.txt", 1, 0);
    ext2_fixture_debugfs(wimg, "set_inode_field /hello.txt size 5", 1, 0);
    use_image(wimg, wbytes);
    if (mount_image() == 0) {
        check(!base_intact(), "a base file changed under the repair is seen by the comparison");
        unmount_image();
    }
}

/** The count of inotify watches, which fd.c reads at a close: none. */
int inotify_watches;

/** Do nothing: fd.c reports a close to inotify, and no row watches. */
void inotify_desc(struct open_file *ofd, uint32_t mask)
{
    (void)ofd;
    (void)mask;
}

/** No row's file system is watched. */
int inotify_mount_watched(const struct vfs_mount *mnt)
{
    (void)mnt;
    return 0;
}

/** A mount is its own base here: no row makes a bind mount. */
const struct vfs_mount *vfs_mount_base(const struct vfs_mount *m)
{
    return m;
}

/** A held regular file's path follows both file and parent renames. */
static void executable_path_row(void)
{
    void *file, *dir;
    char path[100];
    int rc;

    wbytes = build_image(wimg, 4096, 128, PROFILE, 16, "tree");
    use_image(wimg, wbytes);
    if (mount_rw() != 0) {
        check(0, "the executable path fixture mounts");
        return;
    }
    file = make_file(mnt.root, "program", &rc);
    dir = walk("/dir", &rc);
    check(file && dir, "the executable and destination directory are held");
    if (file && dir) {
        check(OPS->path(&mnt, file, path, sizeof(path)) == 0 && streq(path, "/program"),
              "a regular file has a current path");
        check(OPS->rename(&mnt, mnt.root, "program", dir, "renamed") == 0 &&
                  OPS->path(&mnt, file, path, sizeof(path)) == 0 && streq(path, "/dir/renamed"),
              "the held file's path follows a move to another directory");
        check(OPS->rename(&mnt, mnt.root, "dir", mnt.root, "moved-parent") == 0 &&
                  OPS->path(&mnt, file, path, sizeof(path)) == 0 &&
                  streq(path, "/moved-parent/renamed"),
              "the held file's path follows its parent's rename");
        check(OPS->path(&mnt, file, path, 4) == -ENAMETOOLONG,
              "a regular file's path respects the output capacity");
        check(OPS->unlink(&mnt, dir, "renamed") == 0 &&
                  OPS->path(&mnt, file, path, sizeof(path)) == -ENOENT,
              "the last unlink removes the held file's current path");
    }
    if (file) {
        OPS->put(&mnt, file);
    }
    if (dir) {
        OPS->put(&mnt, dir);
    }
    check(unmount_rw() == 0, "the executable path fixture unmounts");
}

/** Make 600 linked files through the host, for mke2fs to stage unchanged. */
static int hardlink_tree(void)
{
    const char *tree = fix_path("hardlink-tree");
    char file[512], alias[512];
    unsigned i;
    void *f;
    int ok;

    if (mkdir(tree, 0700) != 0) {
        return 0;
    }
    for (i = 0; i < 600; i++) {
        snprintf(file, sizeof(file), "%s/a%03u", tree, i);
        snprintf(alias, sizeof(alias), "%s/b%03u", tree, i);
        f = fopen(file, "wb");
        if (!f) {
            return 0;
        }
        ok = fwrite("x", 1, 1, f) == 1;
        if (fclose(f) != 0 || !ok || link(file, alias) != 0) {
            return 0;
        }
    }
    return 1;
}

/** Compare all 600 name pairs with the driver's exact inode and link count. */
static unsigned hardlink_pairs(void)
{
    struct vfs_stat a, b;
    char path[32];
    void *first, *second;
    unsigned i, matched = 0;
    int rc;

    for (i = 0; i < 600; i++) {
        snprintf(path, sizeof(path), "/a%03u", i);
        first = walk(path, &rc);
        snprintf(path, sizeof(path), "/b%03u", i);
        second = walk(path, &rc);
        if (first && second && OPS->getattr(&mnt, first, &a) == 0 &&
            OPS->getattr(&mnt, second, &b) == 0 && a.ino == b.ino && a.nlink == 2 && b.nlink == 2) {
            matched++;
        }
        if (first) {
            OPS->put(&mnt, first);
        }
        if (second) {
            OPS->put(&mnt, second);
        }
    }
    return matched;
}

/** More than the old 512-file capacity is valid; a wrong count remains bad. */
static void hardlink_check_rows(void)
{
    const char *img = fix_path("hardlinks.ext2");
    uint64_t bytes = 16u << 20;
    int rc;

    check(hardlink_tree(), "the host prepares 600 files with two names each");
    if (!ext2_fixture_image(img, 4096, 128, PROFILE, 16, fix_path("hardlink-tree"))) {
        check(0, "mke2fs stages the actual hard links in a private volume");
        return;
    }
    check(e2fsck_clean(img), "independent e2fsck accepts all prepared hard-link counts");
    check(debugfs_script(img, "ssv state 0\n"), "the valid hard-link volume is marked dirty");
    use_image(img, bytes);
    last_check[0] = 0;
    rc = mount_rw();
    check(rc == 0 && !(mnt.flags & VFS_MS_RDONLY) && streq(last_check, "check: clean"),
          "the dirty volume with 600 linked files checks clean and mounts writable");
    if (rc != 0) {
        return;
    }
    check(hardlink_pairs() == 600, "all 600 inode pairs retain their exact two-link counts");
    check(unmount_rw() == 0 && e2fsck_clean(img),
          "the checked volume unmounts cleanly and independent e2fsck still accepts it");
    check(debugfs_script(img, "sif /a599 links_count 3\nssv state 0\n"),
          "the independent tool gives the last linked file an incorrect count");
    check(e2fsck_status(img) == 4, "independent e2fsck rejects the incorrect link count");
    fault_on(img, bytes, "a link count that does not match its names",
             "the same large link inventory with a wrong count mounts writable with errors");
    printf("ext2-hardlinks: checked 600 exact two-name pairs and rejected the bad count\n");
}

/** The timestamp rows, with 128-byte and with 256-byte inodes. */
static void time_rows(void)
{
    time_row(128, PROFILE);
    time_row(256, PROFILE_LARGE);
}

/* The options that run some rows alone, each with the rows it runs. */
static const struct {
    const char *option;
    void (*rows)(void);
} only_rows[] = {
    {"--hardlinks-only", hardlink_check_rows},
    {"--node-only", node_rows},
    {"--fsync-only", fsync_rows},
    {"--order-only", writeback_order_rows},
    {"--pairs-only", pair_rows},
    {"--service-only", service_rows},
    {"--groups-only", group_rows},
    {"--writes-only", write_lock_rows},
    {"--names-only", names_rows},
    {"--times-only", time_rows},
    {"--cache-partial-only", cache_partial_row},
    {"--xattr-only", xattr_rows},
    {"--dirty-only", dirty_rows},
    {"--readonly-only", readonly_rows},
    {"--ftype-only", ftype_rows},
};

/**
 * Make the native fixture tree using the prepared independent tools,
 * and run the rows: the reads at 4 KiB blocks with 128- and 256-byte inodes
 * and at 2 KiB, the 1 KiB levels, the failures, the write sequences, the
 * types of replaced entries, the order rows, the writer's failures, the
 * dirty volumes, the read-only volumes, the matrix and the batch rows, the
 * node rows, the fsync rows, the group rows, the rows of one file's writes
 * and the rows of operations on names, both without the big lock. An option
 * of `only_rows` runs its rows alone; `--capture-only` checks independent
 * tool output capture alone. Returns 0, and calls `fix_passed`, when every
 * row passed.
 */
int main(int argc, char **argv)
{
    uint64_t bytes;
    unsigned i;

    if (argc == 3 && streq(argv[1], "-fn") && streq(argv[2], "--capture-child"))
        return ext2_fixture_capture_child();
    /* Time the sleeping locks' holds, so that a hold across a disk request
     * shows in its record (`alloc_crossings`). */
    sleep_lock_stat = 1;
    held_ops_init();
    wimg = fix_path("w.ext2");
    check(ext2_fixture_capture_test(argv[0], fix_path("")),
          "e2fsck diagnostics stay separate from stdout repair questions");
    if (argc == 2 && streq(argv[1], "--capture-only"))
        goto finish;
    build_tree();

    for (i = 0; argc == 2 && i < sizeof(only_rows) / sizeof(only_rows[0]); i++) {
        if (streq(argv[1], only_rows[i].option)) {
            only_rows[i].rows();
            goto finish;
        }
    }

    bytes = build_image(fix_path("a.ext2"), 4096, 128, PROFILE, 16, "tree");
    read_rows(fix_path("a.ext2"), bytes, "the 4 KiB volume with 128-byte inodes mounts", 1);
    bytes = build_image(fix_path("b.ext2"), 4096, 256, PROFILE, 16, "tree");
    read_rows(fix_path("b.ext2"), bytes, "the 4 KiB volume with 256-byte inodes mounts", 0);
    bytes = build_image(fix_path("c.ext2"), 2048, 128, PROFILE, 16, "tree");
    read_rows(fix_path("c.ext2"), bytes, "the 2 KiB volume mounts", 0);
    bytes = build_image(fix_path("d.ext2"), 1024, 128, PROFILE, 96, "levels");
    levels_rows(fix_path("d.ext2"), bytes);
    refusal_rows();
    executable_path_row();
    write_rows();
    ftype_rows();
    time_rows();
    order_row();
    refusal_write_rows();
    dirty_rows();
    readonly_rows();
    hardlink_check_rows();
    attr_rows();
    xattr_rows();
    matrix_rows();
    batch_rows();
    node_rows();
    fsync_rows();
    writeback_order_rows();
    pair_rows();
    service_rows();
    group_rows();
    write_lock_rows();
    names_rows();

finish:
    if (image) {
        fclose(image);
    }
    if (fails) {
        printf("ext2_hosttest: %d failure(s)\n", fails);
        return 1;
    }
    printf("ext2_hosttest: PASS\n");
    fix_passed();
    return 0;
}
