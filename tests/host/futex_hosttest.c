/**
 * Check what `proc/futex.c` reads of a thread that may end while it works,
 * the robust futexes a thread's end releases, a wait's deadline, which
 * interrupted calls are restarted, and the bucket locks: a requeued
 * waiter's own unqueue, a wake's last store, a word that is not there under
 * a bucket's lock, and the big lock the PI operations take.
 *
 * host-deps: rach2/sys/lib/kstring.c rach2/sys/lib/kctypes.c
 * host-deps: rach2/sys/tick.c
 * host-deps: tests/host/compat/mmap_lock_host.c
 * host-cflags: -Wno-int-to-pointer-cast -Wno-pointer-to-int-cast
 *
 * The source is included whole, to reach its lists of waiters. User memory
 * is a byte array, and a user address is an offset into it. A copy out to
 * user memory may sleep while its page is brought in, and the model's copy
 * can end the thread that `get_robust_list` names and free its record then,
 * as another process's wait may: the head that thread's list had must still
 * be the one reported, which AddressSanitizer checks by the freed record.
 *
 * The model's fault for a write, which `copy_to_user` takes first, can stand
 * for a sleep in which a waiter runs: with interrupts on, it sets
 * FUTEX_WAITERS in the word a row names, as that waiter would before it
 * sleeps. A thread that ends holding that robust futex must leave the word
 * with FUTEX_WAITERS and FUTEX_OWNER_DIED, and wake the waiter; so must one
 * whose word a thread on another processor changes between the release's
 * load and its compare-and-exchange, which the model's exchange can do with
 * interrupts off, so that the release reads the word again. Of the futex
 * it was taking or giving back, one with no owner must have a waiter woken
 * and be left 0, unless it is a priority-inheritance futex.
 *
 * A fault can also stand for a sleep of some ticks, which a wait's deadline,
 * fixed when the call begins, must count. The model's wait ends at once,
 * noting the tick it was armed for.
 *
 * A thread's end clears its CLONE_CHILD_CLEARTID word only when another
 * thread uses its address space, which a hold on the record, as a
 * /proc/PID/mem descriptor has, does not count as.
 *
 * A word is read and exchanged under a bucket's lock without a fault; the
 * model can hold one word's page away until a fault brings it in, as a page
 * never touched would be, and the call must let its locks go, bring the
 * page in and start again. The model's spinlocks trap when taken twice,
 * count the takes of bucket locks and whether the big lock was held, and
 * note a second lock taken below the first one's address.
 */
#include "proc/futex.c"

extern void *calloc(unsigned long count, unsigned long size);
extern void free(void *ptr);
extern int printf(const char *format, ...);

static process_t self;
struct percpu cpu_table[NR_CPUS] = {{.current = &self}};
volatile uint64_t kernel_ticks;

/** The tick count as the kernel reads it whole: the model's `kernel_ticks`. */
uint64_t tick_count(void)
{
    return kernel_ticks;
}
volatile uint32_t kernel_uptime_seconds;
volatile uint32_t kernel_second_ticks;
static uint8_t user[1024];

/* A tick the timer has raised and the boot processor has not yet counted,
 * which the pending row sets. */
static uint32_t tick_pending;

/** The tick count as the clocks count it: `kernel_ticks` and a pending
 * tick. */
uint64_t rtc_ticks_now(void)
{
    return kernel_ticks + tick_pending;
}

/** The tick's counts as one reading, with interrupts off as the kernel's
 * reads them, which on the host's one thread is all one reading needs. */
void tick_now(uint32_t *seconds, uint32_t *second_ticks, uint64_t *ticks)
{
    uint32_t flags = kirq_save();

    *seconds = kernel_uptime_seconds;
    *second_ticks = kernel_second_ticks;
    *ticks = kernel_ticks;
    kirq_restore(flags);
}

/* Thread 5 while it has not ended, and the thread the next copy out ends. */
static process_t *named;
static void (*pi_wait_step)(void);
static int pi_wait_errno;
static process_t *ends_at_copy;
/* The word whose waiter sets FUTEX_WAITERS in the next fault for a write. */
static uint32_t waiters_at;
/* The address whose next fault takes `slow_ticks` ticks, and the tick the
 * last wait was armed for. */
static uint32_t slow_at, slow_ticks;
static uint64_t armed;
static struct futex_q waiter;
static struct futex_sleeper waiter_s;
static int masked, failures, wakes, waits, cmpxchgs;
/* The address space of the model's thread, which every row has but the
 * one that gives it another. */
static struct mm self_mm;
/* The word whose page is not there until a fault for it, and the faults
 * that brought it in. */
static uint32_t absent_at;
static int absent_faults;
/* The big lock's depth, the bucket locks taken, those taken under the big
 * lock, the lock taken last, and the second locks taken out of address
 * order. */
static int bkl_depth, hb_takes, hb_takes_bkl, nested_misordered;
static struct spinlock *last_taken;
/* The word a thread on another processor sets FUTEX_WAITERS in, once, between
 * a load and the compare-and-exchange after it, whatever the interrupts. */
static uint32_t raced_at;

/** Count and print a failure when `c` is false. */
#define CHECK(c, detail)                              \
    do {                                              \
        if (!(c)) {                                   \
            printf("futex-test: fail: %s\n", detail); \
            failures++;                               \
        }                                             \
    } while (0)

/** Whether [addr, addr + len) lies in the model's user memory. */
static int in_user(uint32_t addr, uint32_t len)
{
    return addr != 0 && addr <= sizeof(user) && len <= sizeof(user) - addr;
}

/** The word at user address `addr`. */
static uint32_t word_at(uint32_t addr)
{
    uint32_t w;

    k_memcpy(&w, user + addr, sizeof(w));
    return w;
}

/** Store `w` at user address `addr`. */
static void word_set(uint32_t addr, uint32_t w)
{
    k_memcpy(user + addr, &w, sizeof(w));
}

/** Store `w` at user address `addr` as a long, a word of the robust list. */
static void long_set(uint32_t addr, unsigned long w)
{
    k_memcpy(user + addr, &w, sizeof(w));
}

/** A user span is valid when it lies in the model's user area. */
int user_span_ok(uintptr_t addr, size_t len)
{
    return in_user((uint32_t)addr, (uint32_t)len);
}

/** Bring a range in; a fault for a write taken with interrupts on is a
 * sleep in which the waiter on the word at `waiters_at` sets its bit, and
 * one at `slow_at` a sleep of `slow_ticks` ticks. The page of `absent_at`
 * is there after it. */
int vm_prefault_range(uintptr_t addr, uintptr_t len, int write)
{
    if (!in_user(addr, len)) {
        return 0;
    }
    if (absent_at != 0 && absent_at == addr) {
        absent_at = 0;
        absent_faults++;
    }
    if (write && !masked && waiters_at != 0 && waiters_at == addr) {
        word_set(addr, word_at(addr) | FUTEX_WAITERS);
        waiters_at = 0;
    }
    if (!masked && slow_at != 0 && slow_at == addr) {
        kernel_ticks += slow_ticks;
        slow_at = 0;
    }
    return 1;
}

/**
 * Copy `n` bytes from user address `src` through the model's prefault; 0, or
 * -EFAULT outside user memory.
 */
int copy_from_user(void *dst, const void __user *src, size_t n)
{
    uint32_t at = (uint32_t)(unsigned long)src;

    if (!vm_prefault_range(at, n, 0)) {
        return -EFAULT;
    }
    k_memcpy(dst, user + at, n);
    return 0;
}

/** Copy out, ending the thread `ends_at_copy` first, once, as a sleep for
 * the page could let it end and be reaped. */
int copy_to_user(void __user *dst, const void *src, size_t n)
{
    uint32_t at = (uint32_t)(unsigned long)dst;

    if (ends_at_copy) {
        if (named == ends_at_copy) {
            named = NULL;
        }
        free(ends_at_copy);
        ends_at_copy = NULL;
    }
    if (!vm_prefault_range(at, n, 1)) {
        return -EFAULT;
    }
    k_memcpy(user + at, src, n);
    return 0;
}

/** Whether the word at `addr` may be reached without a fault: aligned, in
 * user memory, and not the word whose page is away. */
static int word_there(uintptr_t addr)
{
    return (addr & 3u) == 0 && in_user((uint32_t)addr, 4) && (absent_at == 0 || absent_at != addr);
}

/** The word at `src`, as one load reads it, or -EFAULT where it is not
 * there; nothing is brought in. */
int get_user_u32_nofault(uint32_t *val, uintptr_t src)
{
    if (!word_there(src)) {
        return -EFAULT;
    }
    *val = word_at((uint32_t)src);
    return 0;
}

/** The word at `dst` exchanged when it holds `old`, as one
 * compare-and-exchange, or -EFAULT where it is not there; counted. */
int cmpxchg_user_u32_nofault(uintptr_t dst, uint32_t old, uint32_t value, uint32_t *prev)
{
    if (!word_there(dst)) {
        return -EFAULT;
    }
    cmpxchgs++;
    if (raced_at != 0 && raced_at == dst) {
        word_set((uint32_t)dst, word_at((uint32_t)dst) | FUTEX_WAITERS);
        raced_at = 0;
    }
    *prev = word_at((uint32_t)dst);
    if (*prev == old) {
        word_set((uint32_t)dst, value);
    }
    return 0;
}

/** Mask interrupts; whether they were masked before. */
uint32_t kirq_save(void)
{
    uint32_t prior = (uint32_t)masked;

    masked = 1;
    return prior;
}

/** Mask interrupts again or not, as `flags` from `kirq_save` says. */
void kirq_restore(uint32_t flags)
{
    masked = (int)flags;
}

/** Take the big lock: count the depth. */
void bkl_enter(void)
{
    bkl_depth++;
}

/** Give the big lock back. */
void bkl_exit(void)
{
    bkl_depth--;
}

/** Take `lock`, trapping when it is held, which on the machine would spin
 * for ever; count a bucket's take and whether the big lock was held. */
static void host_spin_take(struct spinlock *lock)
{
    if (lock->locked) {
        __builtin_trap();
    }
    lock->locked = 1;
    last_taken = lock;
    hb_takes++;
    if (bkl_depth > 0) {
        hb_takes_bkl++;
    }
}

/** Take `lock`. */
void spin_lock(struct spinlock *lock)
{
    host_spin_take(lock);
}

/** Take `lock` as the second of its class, which must lie above the first,
 * the lock taken last and still held. */
void spin_lock_nested(struct spinlock *lock, unsigned int subclass)
{
    (void)subclass;
    if (!last_taken || !last_taken->locked || last_taken >= lock) {
        nested_misordered++;
    }
    host_spin_take(lock);
}

/** Give `lock` back. */
void spin_unlock(struct spinlock *lock)
{
    lock->locked = 0;
}

/** Mask interrupts and take `lock`; whether they were masked before. */
uint32_t spin_lock_irqsave(struct spinlock *lock)
{
    uint32_t flags = kirq_save();

    host_spin_take(lock);
    return flags;
}

/** Give `lock` back and put the interrupt mask back as `flags` says. */
void spin_unlock_irqrestore(struct spinlock *lock, uint32_t flags)
{
    spin_unlock(lock);
    kirq_restore(flags);
}

/** Whether every bucket's lock is free, as each call must leave it. */
static int hb_all_free(void)
{
    int i;

    for (i = 0; i < FUTEX_BUCKETS; i++) {
        if (_futex_hb[i].lock.locked) {
            return 0;
        }
    }
    return 1;
}

/** The one thread besides the caller, pid 5, while it has not ended. */
process_t *get_process(uint32_t pid)
{
    process_t *p;

    if (pid == self.pid && pid != 0) {
        return &self;
    }
    for (p = named; p; p = p->next) {
        if (p->pid == pid) {
            return p;
        }
    }
    return NULL;
}

/** The thread the caller names `pid`; in the model its PID namespace is
 * the first, so the thread with kernel id `pid`. */
process_t *proc_named(uint32_t pid)
{
    return get_process(pid);
}

/** PID namespaces are the initial namespace in this model. */
uint32_t pid_seen_by(const process_t *viewer, uint32_t kid)
{
    (void)viewer;
    return kid;
}

/** Priority changes need no processor interrupt in this model. */
void sched_priority_changed(process_t *p)
{
    (void)p;
}

/** A wait that ends at its deadline, noting the tick it was armed for. */
int wq_wait_event(wait_queue_t *wq, wq_cond_fn cond, void *arg)
{
    (void)wq;
    (void)cond;
    (void)arg;
    waits++;
    armed = current_proc->wakeup_tick;
    if (pi_wait_step) {
        void (*step)(void) = pi_wait_step;

        pi_wait_step = NULL;
        step();
    }
    return pi_wait_errno;
}

/** A named wait, which the model takes as `wq_wait_event`. */
int wq_wait_event_as(wait_queue_t *wq, wq_cond_fn cond, void *arg, const char *name)
{
    (void)name;
    return wq_wait_event(wq, cond, arg);
}

/** The model completes a futex owner's exit before its waiter resumes. */
int wq_wait_event_nointr(wait_queue_t *wq, wq_cond_fn cond, void *arg)
{
    return wq_wait_event(wq, cond, arg);
}

/** Count a wake. */
void wq_wake_all(wait_queue_t *wq)
{
    (void)wq;
    wakes++;
}

/** A waiter's queue needs nothing in the model. */
void wait_queue_init(wait_queue_t *wq)
{
    (void)wq;
}

/** The wall clock: 100 s. */
void get_current_time(int64_t *seconds, int64_t *nanoseconds)
{
    *seconds = 100;
    *nanoseconds = 0;
}

/** The model has no regions: NULL. */
vma_t *vm_find(vm_space_t *vm, uintptr_t addr)
{
    (void)vm;
    (void)addr;
    return NULL;
}

/** The model's user memory is always present, at a frame past its own
 * offset; any other address has no page. */
uintptr_t paging_user_phys(page_dir_t *dir, uintptr_t addr)
{
    (void)dir;
    return in_user(addr, 1) ? 0x100000u + addr : 0;
}

/** Whether [addr, addr + len) is user memory: the model's. */
int user_range_ok(uintptr_t addr, size_t len)
{
    return in_user(addr, len);
}

/** Every call here is a native program's, whose robust list is the
 * native one. */
int in_compat_syscall(void)
{
    return 0;
}

/** futex_waitv's vector, from the host's heap. */
void *kmalloc(size_t size)
{
    return calloc(1, size);
}

/** Give back what `kmalloc` returned. */
void kfree(void *p)
{
    free(p);
}

/** File the model's waiter on the word at `uaddr`, as a sleeping FUTEX_WAIT
 * files itself; the caller's address space names it. */
static void waiter_file(uint32_t uaddr)
{
    k_memset(&waiter, 0, sizeof(waiter));
    k_memset(&waiter_s, 0, sizeof(waiter_s));
    waiter.space = self.page_dir;
    waiter.uaddr = uaddr;
    waiter.bitset = FUTEX_MATCH_ANY;
    waiter.s = &waiter_s;
    waiter.task = &self;
    futex_queue(&waiter, futex_hash(waiter.space, waiter.uaddr));
    wakes = 0;
}

/** Take the model's waiter off its list when no wake did. */
static void waiter_unfile(void)
{
    (void)futex_unqueue_q(&waiter);
}

/**
 * Give the caller a robust list at user address 64: its head, three longs,
 * names the entry at 256 as the list's one when `listed`, and the entry at
 * 128 as the one being taken or given back when `pending`, with
 * `pending_bits` in its low bit. A futex word is 16 bytes past its entry.
 */
static void robust_list_set(int listed, int pending, uint32_t pending_bits)
{
    const uint32_t w = (uint32_t)sizeof(long);

    long_set(64, listed ? 256u : 64u);
    long_set(64 + w, 16u);
    long_set(64 + 2 * w, pending ? 128u | pending_bits : 0u);
    long_set(256, 64u);
    self.robust_list = 64;
    self.robust_len = 3 * w;
}

/**
 * A thread that ends holding a robust futex on its list, while a waiter
 * sets FUTEX_WAITERS in the sleep the fault for the write stands for: the
 * word ends with the waiters bit and FUTEX_OWNER_DIED, and the waiter is
 * woken, and the list is forgotten.
 */
static void owner_released(void)
{
    robust_list_set(1, 0, 0);
    word_set(272, 42u);
    waiters_at = 272;
    waiter_file(272);
    proc_thread_gone(&self);
    CHECK(word_at(272) == (FUTEX_WAITERS | FUTEX_OWNER_DIED),
          "the word keeps the waiters bit a waiter set during the fault");
    CHECK(waiter.woken && wakes == 1, "and the waiter is woken");
    CHECK(self.robust_list == 0, "and the thread's list is forgotten");
    if (!waiter.woken) {
        waiter_unfile();
    }
    waiters_at = 0;
}

/**
 * A thread that ends holding a robust futex on its list, while a thread on
 * another processor sets FUTEX_WAITERS between the release's load of the
 * word and its write, with interrupts off: the compare-and-exchange finds
 * the word changed, the release reads it again, and the word ends with the
 * waiters bit and FUTEX_OWNER_DIED, and the waiter is woken.
 */
static void owner_raced(void)
{
    robust_list_set(1, 0, 0);
    word_set(272, 42u);
    raced_at = 272;
    cmpxchgs = 0;
    waiter_file(272);
    proc_thread_gone(&self);
    CHECK(word_at(272) == (FUTEX_WAITERS | FUTEX_OWNER_DIED) && cmpxchgs == 2,
          "a release that a store from another processor raced reads the word again");
    CHECK(waiter.woken && wakes == 1, "and wakes the waiter whose bit it kept");
    if (!waiter.woken) {
        waiter_unfile();
    }
    raced_at = 0;
}

/**
 * The futex a thread was taking or giving back when it ended, the head's
 * pending entry: with no owner in its word, a wake is owed and one waiter
 * is woken with the word left 0; not when it is a priority-inheritance
 * futex, and not when another thread owns it. One the thread owns is
 * released as a listed one is.
 */
static void pending_released(void)
{
    static const struct {
        uint32_t bits;
        uint32_t word;
        uint32_t after;
        int woken;
        const char *what;
    } rows[] = {
        {0u, 0u, 0u, 1, "an unlock the thread stored and did not wake wakes a waiter"},
        {1u, 0u, 0u, 0, "an ownerless priority-inheritance futex wakes no one"},
        {0u, 77u | FUTEX_WAITERS, 77u | FUTEX_WAITERS, 0, "another thread's futex is left"},
        {0u, 42u | FUTEX_WAITERS, FUTEX_WAITERS | FUTEX_OWNER_DIED, 1,
         "a futex the thread holds is released"},
    };
    unsigned int i;

    for (i = 0; i < sizeof(rows) / sizeof(rows[0]); i++) {
        robust_list_set(0, 1, rows[i].bits);
        word_set(144, rows[i].word);
        waiter_file(144);
        proc_thread_gone(&self);
        CHECK(waiter.woken == rows[i].woken && wakes == rows[i].woken, rows[i].what);
        CHECK(word_at(144) == rows[i].after, "and the word is as it should be after");
        if (!waiter.woken) {
            waiter_unfile();
        }
    }
}

/**
 * A thread's end clears the word CLONE_CHILD_CLEARTID named, and wakes its
 * waiter, only when another thread runs in the address space: by the count
 * of its users, not of the holds on its record, which a /proc/PID/mem
 * descriptor adds to.
 */
static void child_tid_cleared(void)
{
    struct mm space;

    k_memset(&space, 0, sizeof(space));
    space.users = 1;
    space.holds = 2;
    self.mm = &space;
    self.clear_child_tid = 700;
    word_set(700, 42u);
    waiter_file(700);
    proc_thread_gone(&self);
    CHECK(word_at(700) == 42u && wakes == 0 && self.clear_child_tid == 0,
          "a thread alone in an address space a descriptor holds leaves its word");
    waiter_unfile();
    space.users = 2;
    self.clear_child_tid = 700;
    waiter_file(700);
    proc_thread_gone(&self);
    CHECK(word_at(700) == 0 && waiter.woken && wakes == 1,
          "a thread that shares its address space clears the word and wakes the waiter");
    if (!waiter.woken) {
        waiter_unfile();
    }
    self.mm = &self_mm;
}

/** Wait on the word at 600, which holds 7, for `sec` s and `nsec` ns from
 * tick `now`, while bringing the word's page in takes `fault` ticks. */
static int32_t timed_wait(uint32_t now, int32_t sec, int32_t nsec, uint32_t fault)
{
    isr_params_t p;
    struct kernel_timespec ts;

    ts.tv_sec = sec;
    ts.tv_nsec = nsec;
    k_memcpy(user + 608, &ts, sizeof(ts));
    word_set(600, 7u);
    kernel_ticks = now;
    absent_at = 600;
    slow_at = 600;
    slow_ticks = fault;
    waits = 0;
    armed = 0;
    k_memset(&p, 0, sizeof(p));
    p.ebx = 600;
    p.ecx = FUTEX_WAIT | FUTEX_PRIVATE_FLAG;
    p.edx = 7;
    p.esi = 608;
    return sys_futex(&p);
}

/**
 * A wait's deadline is fixed when the call begins: a wait of one tick whose
 * page is not there and takes 50 ticks to come in has passed its deadline
 * and ends without sleeping, and a wait of a second from tick 2000 sleeps
 * until a second and a tick later, 2001 + TICK_HZ, whatever the fault took.
 */
static void deadline_fixed(void)
{
    CHECK(timed_wait(1000, 0, (int32_t)TICK_NS, 50) == -ETIMEDOUT && waits == 0,
          "a deadline a fault outlasted ends the wait without a sleep");
    CHECK(timed_wait(2000, 1, 0, 50) == -ETIMEDOUT && waits == 1 && armed == 2001u + TICK_HZ,
          "a wait of a second from tick 2000 sleeps until a second and a tick later");
    tick_pending = 1;
    CHECK(timed_wait(2000, 1, 0, 0) == -ETIMEDOUT && waits == 1 && armed == 2002u + TICK_HZ,
          "a wait of a second begun while tick 2001 is pending sleeps until a second and a "
          "tick past the clock's 2001");
    tick_pending = 0;
}

/**
 * How a futex wait a signal interrupted is filed for its restart: one with
 * no timeout keeps the dispatcher's ERESTARTSYS, and a timed one stands for
 * ERESTART_RESTARTBLOCK with its word, value and deadline tick in the
 * restart block, whose resumption sleeps on to that same tick.
 */
static void restarts(void)
{
    isr_params_t p;

    pi_wait_errno = -EINTR;
    self.restart_kind = RESTART_SYS;
    self.restart.fn = NULL;
    word_set(600, 7u);
    k_memset(&p, 0, sizeof(p));
    p.ebx = 600;
    p.ecx = FUTEX_WAIT | FUTEX_PRIVATE_FLAG;
    p.edx = 7;
    CHECK(sys_futex(&p) == -EINTR && self.restart_kind == RESTART_SYS && !self.restart.fn,
          "a wait with no timeout keeps ERESTARTSYS");
    slow_ticks = 0;
    CHECK(timed_wait(3000, 1, 0, 0) == -EINTR && self.restart_kind == RESTART_BLOCK &&
              self.restart.fn == futex_wait_restart && self.restart.uaddr == 600 &&
              self.restart.val == 7 && self.restart.tick == 3001u + TICK_HZ,
          "a timed wait files its word, value and deadline for restart_syscall");
    pi_wait_errno = 0;
    kernel_ticks = 3500;
    waits = 0;
    armed = 0;
    CHECK(futex_wait_restart(&p) == -ETIMEDOUT && waits == 1 && armed == 3001u + TICK_HZ &&
              !self.restart.fn,
          "the resumed wait sleeps to the first deadline and clears the block");
    kernel_ticks = 3000u + TICK_HZ;
    self.restart.fn = futex_wait_restart;
    waits = 0;
    armed = 0;
    CHECK(futex_wait_restart(&p) == -ETIMEDOUT && waits == 1 && armed == 3001u + TICK_HZ,
          "a wait resumed one tick before its deadline still sleeps to it");
    kernel_ticks = 3001u + TICK_HZ;
    self.restart.fn = futex_wait_restart;
    waits = 0;
    CHECK(futex_wait_restart(&p) == -ETIMEDOUT && waits == 0,
          "a wait resumed at its deadline ends without a sleep");
}

/**
 * get_robust_list reads the head of thread 5's list before it copies
 * anything out: the first copy ends the thread and frees its record, and the
 * head and the length must still arrive.
 */
static void robust_head_read(void)
{
    isr_params_t p;
    process_t *t = calloc(1, sizeof(*t));

    if (!t) {
        CHECK(0, "the model has a record for thread 5");
        return;
    }
    t->pid = t->tgid = 5;
    t->state = PROC_RUNNING;
    t->robust_list = 0x40;
    named = t;
    ends_at_copy = t;
    k_memset(&p, 0, sizeof(p));
    p.ebx = 5;
    p.ecx = 512;
    p.edx = 520;
    CHECK(sys_get_robust_list(&p) == 0, "get_robust_list of another thread succeeds");
    CHECK(word_at(512) == 0x40, "the head reported is the one the thread had");
    CHECK(word_at(520) == 3 * sizeof(long), "and the length is the head's, three longs");
    CHECK(named == NULL, "the thread ended during the copy");
    if (ends_at_copy) {
        free(ends_at_copy);
        ends_at_copy = NULL;
    }
}

/**
 * A requeue moves a waiter to the second futex's bucket under both buckets'
 * locks, the lower first, and the waiter's own unqueue then finds it there,
 * as Linux's futex_unqueue follows lock_ptr. A wake takes a waiter off,
 * wakes its sleeper and clears its bucket last, so its unqueue finds a wake
 * took it off.
 */
static void bucket_rows(void)
{
    isr_params_t p;

    waiter_file(300);
    k_memset(&p, 0, sizeof(p));
    p.ebx = 300;
    p.ecx = FUTEX_REQUEUE | FUTEX_PRIVATE_FLAG;
    p.edx = 0;
    p.esi = 1;
    p.edi = 304;
    nested_misordered = 0;
    CHECK(futex_hash(self.page_dir, 300) != futex_hash(self.page_dir, 304),
          "the two words hash to two buckets");
    CHECK(sys_futex(&p) == 1 && !waiter.woken && waiter.uaddr == 304 &&
              waiter.hb == futex_hash(self.page_dir, 304),
          "a requeue moves the waiter to the second futex's bucket");
    CHECK(nested_misordered == 0, "and takes the two buckets' locks in address order");
    CHECK(futex_unqueue_q(&waiter) == 1 && waiter.hb == NULL &&
              futex_hash(self.page_dir, 304)->head == NULL,
          "the waiter's unqueue takes it off the bucket it was moved to");
    waiter_file(400);
    p.ebx = 400;
    p.ecx = FUTEX_WAKE | FUTEX_PRIVATE_FLAG;
    p.edx = 1;
    CHECK(sys_futex(&p) == 1 && waiter.woken && waiter_s.woken && waiter.hb == NULL && wakes == 1,
          "a wake marks the waiter and its sleeper and clears its bucket");
    CHECK(futex_unqueue_q(&waiter) == 0, "and the waiter's unqueue finds a wake took it off");
    CHECK(hb_all_free() && !masked, "every bucket's lock is free and interrupts are on after");
}

/**
 * A word that is not there under its bucket's lock: the call lets its locks
 * go, brings the page in and starts again, as Linux's callers of
 * futex_get_value_locked do. A wait on a word that differs is then EAGAIN
 * with no waiter left filed, and FUTEX_WAKE_OP changes its second word once
 * and wakes the first word's waiter.
 */
static void fault_rows(void)
{
    isr_params_t p;

    word_set(500, 3u);
    absent_at = 500;
    absent_faults = 0;
    k_memset(&p, 0, sizeof(p));
    p.ebx = 500;
    p.ecx = FUTEX_WAIT | FUTEX_PRIVATE_FLAG;
    p.edx = 7;
    CHECK(sys_futex(&p) == -EAGAIN && absent_faults == 1 &&
              futex_hash(self.page_dir, 500)->head == NULL,
          "a wait whose word was not there brings it in, compares it and files nothing");
    word_set(504, 1u);
    absent_at = 504;
    absent_faults = 0;
    waiter_file(500);
    p.ecx = FUTEX_WAKE_OP | FUTEX_PRIVATE_FLAG;
    p.edx = 1;
    p.esi = 1;
    p.edi = 504;
    p.ebp = ((uint32_t)FUTEX_OP_ADD << 28) | ((uint32_t)FUTEX_OP_CMP_EQ << 24) | (2u << 12) | 1u;
    CHECK(sys_futex(&p) == 1 && word_at(504) == 3u && absent_faults == 1 && waiter.woken,
          "FUTEX_WAKE_OP brings its second word in, changes it once and wakes the first");
    if (!waiter.woken) {
        waiter_unfile();
    }
    CHECK(hb_all_free() && !masked, "every bucket's lock is free and interrupts are on after");
    absent_at = 0;
}

/**
 * The PI operations take the big lock before a bucket's lock and give it
 * back, and the others take the bucket's lock alone, so that futex(2) runs
 * without the big lock except where PI states change.
 */
static void bkl_rows(void)
{
    isr_params_t p;

    word_set(520, 0u);
    k_memset(&p, 0, sizeof(p));
    p.ebx = 520;
    p.ecx = FUTEX_LOCK_PI | FUTEX_PRIVATE_FLAG;
    hb_takes = hb_takes_bkl = 0;
    CHECK(sys_futex(&p) == 0 && word_at(520) == self.pid, "LOCK_PI takes a free word");
    p.ecx = FUTEX_UNLOCK_PI | FUTEX_PRIVATE_FLAG;
    CHECK(sys_futex(&p) == 0 && word_at(520) == 0, "UNLOCK_PI frees it");
    CHECK(hb_takes == 2 && hb_takes_bkl == 2 && bkl_depth == 0,
          "the PI operations take their buckets under the big lock and give it back");
    hb_takes = hb_takes_bkl = 0;
    p.ecx = FUTEX_WAKE | FUTEX_PRIVATE_FLAG;
    p.edx = 1;
    CHECK(sys_futex(&p) == 0 && hb_takes == 1 && hb_takes_bkl == 0,
          "a wake takes its bucket without the big lock");
}

/** Run the rows; 0 when every check passed, 1 otherwise. */
int main(void)
{
    self.pid = self.tgid = 42;
    self.state = PROC_RUNNING;
    self.mm = &self_mm;
    robust_head_read();
    owner_released();
    owner_raced();
    pending_released();
    child_tid_cleared();
    deadline_fixed();
    restarts();
    bucket_rows();
    fault_rows();
    bkl_rows();
    if (failures) {
        return 1;
    }
    printf("futex-test: pass: the robust head, the robust release, the child's word, the "
           "deadline, the restarts and the bucket locks\n");
    return 0;
}
