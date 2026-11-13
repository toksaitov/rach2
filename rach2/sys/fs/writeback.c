/*
 * writeback.c: the flusher, the kernel thread that writes dirty data back
 * by age and by the background threshold. The policy is in writeback.h.
 */

#include "writeback.h"
#include "bcache.h"
#include "vfs.h"

#include "../klog.h"
#include "../arch/arch.h"
#include "../lib/kirq.h"
#include "../mm/filemap.h"
#include "../mm/pagecache.h"
#include "../proc/process.h"
#include "../proc/sched.h"
#include "../proc/wait_queue.h"
#include "../timer.h"

/* Linux's defaults (mm/page-writeback.c:90-100, fs/fs-writeback.c:2303). */
int dirty_expire_interval = 3000;
int dirty_writeback_interval = 500;
int dirtytime_expire_interval = 12 * 60 * 60;

/*
 * The buffer cache counts its dirty marks and stamps a buffer with the count
 * at which it went dirty, and keeps no clock. The flusher reads the count at
 * each turn and keeps the readings with their ticks, so that "dirty since
 * before tick T" is "dirty at or before the count read at or before T". The
 * readings are spaced a 32nd of the expiry period apart at least, so the 64
 * kept reach two periods back; a reading old enough is taken, never a
 * newer one, so a block is written no sooner than its period allows.
 */
#define READINGS 64
static struct {
    uint64_t tick;
    uint64_t marks;
} readings[READINGS];
static int reading_first, reading_count;

static wait_queue_t flusher_wait;
static volatile unsigned int flusher_gen; /* moved by every kick */
static volatile int flusher_busy;         /* in a turn, which needs no wake */
static process_t *flusher;

/** The expiry period in ticks. */
static uint64_t expire_ticks(void)
{
    return tick_centisecs((uint32_t)dirty_expire_interval);
}

/** Keep the count of marks read at tick `now`, unless the last reading is
 * nearer than the spacing; a full ring gives up its oldest. */
static void reading_add(uint64_t now, uint64_t marks)
{
    uint64_t gap = expire_ticks() >> 5;
    int last = (reading_first + reading_count - 1) % READINGS;

    if (reading_count > 0 && now - readings[last].tick < gap) {
        return;
    }
    if (reading_count == READINGS) {
        reading_first = (reading_first + 1) % READINGS;
        reading_count--;
    }
    last = (reading_first + reading_count) % READINGS;
    readings[last].tick = now;
    readings[last].marks = marks;
    reading_count++;
}

/**
 * The count of marks that bounds the buffers dirty since tick `before` or
 * longer: that of the newest reading at or before `before`, with the older
 * ones given up; 0, which writes nothing, when no reading is that old.
 */
static uint64_t reading_at(uint64_t before)
{
    uint64_t found = 0;

    while (reading_count > 0 && readings[reading_first].tick <= before) {
        found = readings[reading_first].marks;
        if (reading_count == 1 || readings[(reading_first + 1) % READINGS].tick > before) {
            break;
        }
        reading_first = (reading_first + 1) % READINGS;
        reading_count--;
    }
    return found;
}

/**
 * Write the oldest dirty buffers back, a reading at a time from the oldest,
 * until the cache is under its background threshold, then, if it is not,
 * every dirty buffer, as Linux's background writeback writes the oldest
 * inodes first until `wb_over_bg_thresh` says no more (fs/fs-writeback.c).
 * Each file system's pending writes stay for their own age, since the cache
 * does not count them.
 */
static void write_background(void)
{
    int i;

    for (i = 0; i < reading_count && bcache_dirty_level() > 0; i++) {
        vfs_writeback_all(0, readings[(reading_first + i) % READINGS].marks);
    }
    if (bcache_dirty_level() > 0) {
        vfs_writeback_all(0, bcache_marks());
    }
}

/* What the flusher's sleep waits for: a kick, which moves `flusher_gen`
 * past `gen`, or the tick `due`, 0 for none. */
struct flusher_wait_arg {
    unsigned int gen;
    uint64_t due;
};

/** Whether the flusher's sleep, its `struct flusher_wait_arg`, is over. */
static int flusher_due(void *arg)
{
    const struct flusher_wait_arg *a = (const struct flusher_wait_arg *)arg;

    return flusher_gen != a->gen || (a->due != 0 && tick_count() >= a->due);
}

/**
 * The flusher's loop, Linux's `wb_workfn` with `wb_check_old_data_flush`
 * and `wb_check_background_flush`: at each turn the reading of the marks,
 * once a period the pages that keep no age, then the write-back of what has
 * expired, those pages' stores among it once their mount says they may be
 * that old (`vfs_writeback_aged`), then the background write-back; then a
 * sleep for the writeback interval, or until a wake when the interval is 0.
 * While the cache holds dirty buffers the sleep ends at the readings'
 * spacing too, for a reading only, and the first dirty buffer of a clean
 * cache wakes the thread, so that a buffer waits at most that spacing
 * longer than on Linux, which stamps each inode with the time it went
 * dirty. The kicks' count is read at the start of the turn, after the turn
 * is published busy, and the sleep ends at once when it has moved since:
 * a kick that came during the turn, whose wake the turn's being busy kept
 * back, may have come after the turn's last look at the cache.
 *
 * A turn runs marked as in a call (`in_syscall`), as sync(2) does the same
 * writes, so that a disk sleeps the thread for its answer and a loop device,
 * which sleeps every caller, takes its writes; outside a call the request
 * layer polls a disk that can be polled and stops the kernel at any other.
 * The mark is off while the thread sleeps between turns; with it or
 * without, the thread is a kernel thread, which takes no signal
 * (`proc_is_kthread`).
 */
static void flusher_main(void)
{
    uint64_t now, period, last_pages = tick_count(), last_turn = tick_count();
    uint64_t interval, soon;
    uint32_t flags;
    struct flusher_wait_arg arg;

    arch_irq_enable();
    for (;;) {
        flusher_busy = 1;
        current_proc->in_syscall = 1;
        sched_mb();
        arg.gen = flusher_gen;
        now = tick_count();
        period = expire_ticks();
        interval = tick_centisecs((uint32_t)dirty_writeback_interval);
        reading_add(now, bcache_marks());
        if (interval != 0 && now - last_turn >= interval) {
            last_turn = now;
            if (now - last_pages >= period) {
                last_pages = now;
                filemap_sync_all();
                (void)pc_sync();
            }
            if (period == 0) {
                vfs_writeback_all(now, bcache_marks());
            } else if (now > period) {
                vfs_writeback_all(now - period, reading_at(now - period));
            }
        }
        write_background();
        current_proc->in_syscall = 0;
        flusher_busy = 0;
        sched_mb();
        flags = kirq_save();
        arg.due = interval != 0 ? last_turn + interval : 0;
        soon = now + ((period >> 5) ? (period >> 5) : 1);
        if (interval != 0 && period != 0 && soon < arg.due && bcache_dirty_count() > 0) {
            arg.due = soon;
        }
        current_proc->wakeup_tick = arg.due;
        kirq_restore(flags);
        (void)wq_wait_event_idle(&flusher_wait, flusher_due, &arg);
        current_proc->wakeup_tick = 0;
    }
}

/** Move the kicks' count, atomically, which is a full barrier too: a
 * flusher that reads the count after it has moved sees what the kicker
 * stored before. */
static void flusher_count(void)
{
    unsigned int gen;

    do {
        gen = flusher_gen;
    } while (arch_atomic_cmpxchg(&flusher_gen, gen, gen + 1u) != gen);
}

void writeback_kick(void)
{
    flusher_count();
    wq_wake_all(&flusher_wait);
}

/** The cache's call at its first dirty buffer and past the background
 * threshold: the kicks' count moved, and a wake unless the flusher is in a
 * turn, since every dirty mark past the threshold makes one. A turn ends
 * with another when the count has moved since its start (`flusher_main`);
 * the move comes before the look at `flusher_busy`, and the flusher clears
 * that before it reads the count for its sleep, each with a full barrier
 * between, so either the kicker sees the turn over and wakes the thread or
 * the thread sees the count moved. */
static void flusher_kick(void)
{
    flusher_count();
    if (!flusher_busy) {
        wq_wake_all(&flusher_wait);
    }
}

/** A kernel thread named `name` that begins at `entry`, ready; NULL when
 * no record or stack can be had. */
static process_t *kthread_start(const char *name, void (*entry)(void))
{
    process_t *p;
    uintptr_t base;
    uint32_t flags = kirq_save();

    base = kstack_alloc();
    p = base ? alloc_process() : NULL;
    if (!p) {
        if (base) {
            kstack_free(base + KERNEL_STACK_SIZE);
        }
        kirq_restore(flags);
        return NULL;
    }
    p->kernel_stack = base + KERNEL_STACK_SIZE;
    p->space_root = 0;
    p->page_dir = NULL;
    p->parent_pid = 0;
    proc_set_comm(p, name);
    setup_process_stack(p, entry);
    sched_make_ready(p);
    kirq_restore(flags);
    return p;
}

void writeback_init(void)
{
    flusher = kthread_start("writeback", flusher_main);
    if (!flusher) {
        klog(KLOG_ERR, "writeback", "flusher: refused: no record or stack for it");
        return;
    }
    bcache_set_flusher(flusher_kick);
}
