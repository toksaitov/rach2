/*
 * sleeplock_hosttest.c: the sleeping lock of rach2/sys/fs/sleeplock.c, its
 * order, its depth, the file locks it counts, its record of holds across a
 * disk wait, and the killable take, whose killed waiter leaves the line
 * with no memory to spare.
 *
 * host-deps: rach2/sys/fs/sleeplock.c rach2/sys/lib/kstring.c
 * host-deps: rach2/sys/lib/kctypes.c tests/host/compat/atomic_host.c
 * host-deps: tests/host/compat/spin_none.c
 *
 * One host thread plays every process. The stubbed wait stands for the
 * scheduler: each time a taker would sleep, it runs the next step of the
 * row's script as another process, which sets `current_proc` to that
 * process, acts and sets it back. A wait with no step left fails the test,
 * since nothing would ever wake the taker. The clock is a variable a row
 * moves, and a disk wait is a step of `disk_waits`, as the block layer
 * counts one. The spinlocks are counted, so that a row sees that none is
 * held across a wait or after a take or a give. The atomic operations are
 * the host's (`atomic_host.c`). The heap refuses every allocation, as an
 * exhausted one does, so that a lock that needed memory to let a killed
 * waiter go would keep it. Each panic row runs the
 * violating call in a forked child, whose exit status says whether the
 * panic came and named the rule.
 */

#include <setjmp.h>

#include "../../rach2/sys/fs/sleeplock.h"
#include "../../rach2/sys/lib/kerrno.h"
#include "../../rach2/sys/lib/ksignal.h"
#include "../../rach2/sys/proc/process.h"
#include "../../rach2/sys/proc/percpu.h"
#include "../../rach2/sys/proc/tgroup.h"

extern int printf(const char *fmt, ...);
extern int fork(void);
extern int waitpid(int pid, int *status, int options);
extern void _exit(int status);

static process_t proc_a, proc_b, proc_c;
static struct tgroup tg_a, tg_b, tg_c;
struct percpu cpu_table[NR_CPUS] = {{.current = &proc_a}};

static int fails;

/** Count a failed check and print `what`. */
static void check(int ok, const char *what)
{
    if (!ok) {
        fails++;
        printf("sleeplock_hosttest: FAIL: %s\n", what);
    }
}

/* Set before a violating call in a forked child; k_panic exits 42 when its
 * text contains this, 43 when it does not. */
static const char *expect_panic;

/** Return 1 when `needle` occurs in `hay`, and 0 otherwise. */
static int contains(const char *hay, const char *needle)
{
    const char *h;

    for (h = hay; *h; h++) {
        const char *a = h;
        const char *b = needle;

        while (*a && *b && *a == *b) {
            a++;
            b++;
        }
        if (!*b) {
            return 1;
        }
    }
    return 0;
}

/** The kernel's panic: exit 42 or 43 in a panic row's child, as
 * `expect_panic` says, and otherwise print the text and exit 44. */
void k_panic(const char *s)
{
    if (expect_panic) {
        _exit(contains(s, expect_panic) ? 42 : 43);
    }
    printf("sleeplock_hosttest: unexpected panic: %s\n", s);
    _exit(44);
}

/* The allocations the lock asked for, which the heap refused. */
static int allocations;

/** The heap, exhausted: count the request and refuse it. */
void *kzalloc(size_t size)
{
    (void)size;
    allocations++;
    return 0;
}

/** Nothing to free, since `kzalloc` gives nothing. */
void kfree(void *ptr)
{
    (void)ptr;
}

/* The clock, in microseconds, which a row moves, and the reads of it and
 * of the cycle counter. */
static uint32_t now_us;
static int clock_reads;

/** The clock: `now_us`. */
uint32_t timer_now_us(void)
{
    clock_reads++;
    return now_us;
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

/* Whether the processors' counters agree, which a row sets: a lock taken
 * while they do is stamped by the counter, and otherwise by the clock. */
static int cycles_agree = 1;

/** Whether the processors' counters agree: `cycles_agree`. */
int timer_cycles_agree(void)
{
    return cycles_agree;
}

/** Clear a wait queue. */
void wait_queue_init(wait_queue_t *wq)
{
    __builtin_memset(wq, 0, sizeof(*wq));
}

/* The wakes a give made. */
static int wakes;

/** Count a wake; the scripted steps stand for the sleepers running. */
void wq_wake_all(wait_queue_t *wq)
{
    (void)wq;
    wakes++;
}

/* The takers inside a wait whose condition was false, which on the machine
 * are on the queue while the step that stands for the holder runs. */
static int sleepers;

/** Whether a taker sleeps on the queue now. */
int wq_active(const wait_queue_t *wq)
{
    (void)wq;
    return sleepers > 0;
}

/* The spinlocks held now, a lock's own and the records', and the waits
 * that began with one held, which on the machine would sleep under it. */
static int spin_depth, spin_at_wait;

/* A step run once at the next take of a spinlock, before the take, as
 * another processor that acts just before it: a row sets it. */
static void (*on_spin)(void);

/** Take `lock` with interrupts off: one thread, so a lock already held is
 * the same path taking it twice, which on the machine spins for ever. */
uint32_t spin_lock_irqsave(struct spinlock *lock)
{
    void (*step)(void) = on_spin;

    if (step) {
        process_t *me = current_proc;

        on_spin = 0;
        step();
        cpu_table[0].current = me;
    }
    check(!lock->locked, "no spinlock is taken twice");
    lock->locked = 1;
    spin_depth++;
    return 0;
}

/** Give `lock` back. */
void spin_unlock_irqrestore(struct spinlock *lock, uint32_t flags)
{
    (void)flags;
    lock->locked = 0;
    spin_depth--;
}

/* The steps the waits run, in order, and the next one. */
#define MAX_STEPS 8
static void (*steps[MAX_STEPS])(void);
static int nsteps, next_step, waits;

/** Script `step` to run at the next wait that finds nothing scripted
 * before it. */
static void script(void (*step)(void))
{
    steps[nsteps++] = step;
}

/** The uninterruptible wait: run the next scripted step, as the processes
 * that run while the caller sleeps, or fail when none is left. */
int wq_wait_event_nointr(wait_queue_t *wq, wq_cond_fn cond, void *arg)
{
    process_t *me = current_proc;

    (void)wq;
    waits++;
    if (spin_depth != 0) {
        spin_at_wait++;
    }
    if (cond(arg)) {
        return 0;
    }
    if (next_step >= nsteps) {
        printf("sleeplock_hosttest: FAIL: a wait that nothing would end\n");
        _exit(1);
    }
    sleepers++;
    steps[next_step++]();
    sleepers--;
    cpu_table[0].current = me;
    return 0;
}

/** The killable wait: as the uninterruptible one, which the row's steps
 * end; a step that kills the waiter makes its condition true. */
int wq_wait_event_killable(wait_queue_t *wq, wq_cond_fn cond, void *arg, const char *name)
{
    (void)name;
    return wq_wait_event_nointr(wq, cond, arg);
}

/** Forget the script and the counts of the last row. */
static void script_clear(void)
{
    nsteps = next_step = waits = wakes = sleepers = 0;
}

static struct sleep_lock_record rec_one = SLEEP_LOCK_RECORD("one");
static struct sleep_lock_record rec_two = SLEEP_LOCK_RECORD("two");
static struct sleep_lock lock, file_lock, other;

/** A step as the signal's sender: SIGKILL pending for process A. */
static void kill_a(void)
{
    proc_a.sig_pending |= KSIG_BIT(SIGKILL);
}

/** A step as process A: give `lock` back, and ask for it again at once
 * with a killable take, which must wait, since B asked first; A is killed
 * in that wait and leaves the line. */
static void a_gives(void)
{
    cpu_table[0].current = &proc_a;
    check(sleep_lock_held(&lock), "A holds the lock while B waits");
    sleep_lock_give(&lock);
    check(lock.holder == -1 && lock.state == SLEEP_LOCK_HELD && lock.first == 0,
          "A's give hands the lock to B, the only waiter, and lists no other");
    script(kill_a);
    check(sleep_lock_take_killable(&lock) == -EINTR && waits == 2,
          "A, asking again before B has run, waits behind B");
    check(lock.state == SLEEP_LOCK_HELD && lock.first == 0 && lock.last == 0,
          "and, killed, leaves the list empty");
    proc_a.sig_pending = 0;
}

/** A step as process B: give `lock` back. */
static void b_gives(void)
{
    cpu_table[0].current = &proc_b;
    check(sleep_lock_held(&lock), "B holds the lock while A waits behind it");
    sleep_lock_give(&lock);
}

/** A step as process C: ask for `lock`, which B holds, and find it held. */
static void c_finds_b(void)
{
    cpu_table[0].current = &proc_c;
    check(!sleep_lock_held(&lock) && lock.holder == (int)proc_b.pid, "C finds the lock B holds");
}

/** A take and a give with no other process: no wait, the depth and the
 * line's places. */
static void row_alone(void)
{
    script_clear();
    cpu_table[0].current = &proc_a;
    sleep_lock_init(&lock, &rec_one, 0);
    check(lock.holder == -1 && lock.depth == 0 && !sleep_lock_held(&lock), "a new lock is free");
    sleep_lock_take(&lock);
    check(waits == 0 && sleep_lock_held(&lock) && lock.holder == (int)proc_a.pid && lock.depth == 1,
          "a free lock is taken with no wait");
    sleep_lock_take(&lock);
    check(waits == 0 && lock.depth == 2 && lock.state == SLEEP_LOCK_HELD && lock.first == 0,
          "its holder takes it again with no wait");
    sleep_lock_give(&lock);
    check(sleep_lock_held(&lock) && lock.depth == 1 && wakes == 0,
          "a give of the inner take keeps it held and wakes nobody");
    sleep_lock_give(&lock);
    check(!sleep_lock_held(&lock) && lock.holder == -1 && lock.state == SLEEP_LOCK_FREE &&
              wakes == 0,
          "the last give frees it and wakes nobody, since none waits");
    check(proc_a.file_locks == 0, "a lock that is not a file lock counts in no file_locks");
}

/** Two processes: B waits for A, and A, asking again, waits behind B. */
static void row_order(void)
{
    script_clear();
    sleep_lock_init(&lock, &rec_one, 0);
    cpu_table[0].current = &proc_a;
    sleep_lock_take(&lock);

    script(a_gives);
    cpu_table[0].current = &proc_b;
    sleep_lock_take(&lock);
    check(waits == 2 && next_step == 2 && sleep_lock_held(&lock) && lock.depth == 1,
          "B waits while A holds the lock and has it after A's give");

    script(c_finds_b);
    script(b_gives);
    cpu_table[0].current = &proc_a;
    sleep_lock_take(&lock);
    check(next_step == 4 && waits == 4 && sleep_lock_held(&lock),
          "A, asking after B had its place, waits until B gives it back");
    check(lock.state == SLEEP_LOCK_HELD && lock.first == 0, "and the list is empty");
    sleep_lock_give(&lock);
    check(lock.state == SLEEP_LOCK_FREE, "A's give frees the lock");
}

/** A step as process A: give `lock` back and ask for nothing more. */
static void a_gives_once(void)
{
    cpu_table[0].current = &proc_a;
    sleep_lock_give(&lock);
}

/** A take and a give pass no spinlock: B waits for A with none held, has
 * the lock once A gives it, and none is left held after any row. */
static void row_spin(void)
{
    script_clear();
    sleep_lock_init(&lock, &rec_one, 0);
    cpu_table[0].current = &proc_a;
    sleep_lock_take(&lock);
    check(spin_depth == 0, "a take holds no spinlock");
    script(a_gives_once);
    cpu_table[0].current = &proc_b;
    sleep_lock_take(&lock);
    check(waits == 1 && spin_at_wait == 0 && sleep_lock_held(&lock),
          "a taker waits with no spinlock held and has the lock after the give");
    check(wakes == 1, "the give with a taker asleep wakes it");
    sleep_lock_give(&lock);
    check(wakes == 1, "the give with no taker asleep wakes nobody");
    check(spin_depth == 0, "a give holds no spinlock");
}

/** A file lock raises its holder's file_locks once for a hold, whatever
 * its depth, and only its holder's. */
static void row_file_locks(void)
{
    script_clear();
    sleep_lock_init(&file_lock, 0, SLEEP_LOCK_FILE);
    sleep_lock_init(&other, 0, SLEEP_LOCK_FILE);
    cpu_table[0].current = &proc_b;
    sleep_lock_take(&file_lock);
    check(proc_b.file_locks == 1, "a file lock raises its holder's file_locks");
    sleep_lock_take(&file_lock);
    check(proc_b.file_locks == 1, "a second take of it does not");
    sleep_lock_take(&other);
    check(proc_b.file_locks == 2, "a second file lock does");
    check(proc_a.file_locks == 0 && proc_c.file_locks == 0, "no other process's count moves");
    sleep_lock_give(&other);
    sleep_lock_give(&file_lock);
    check(proc_b.file_locks == 1, "the inner give leaves the count");
    sleep_lock_give(&file_lock);
    check(proc_b.file_locks == 0, "the last give lowers it to zero");
}

/** With kernel.lock_stat off a take and a give read no clock and record
 * nothing, and a hold taken while it was off is not recorded when it is
 * turned on before the give. Once it is on, the record keeps holds that
 * crossed a disk wait, the longest with its holder, and passes over the
 * rest. A hold stamped by the clock, taken
 * while the processors' counters disagree, is measured by the clock's
 * difference across its wrap, and one stamped by the counter that reads the
 * counter below its stamp, as on another processor, reads 0. */
static void row_record(void)
{
    script_clear();
    sleep_lock_init(&lock, &rec_one, 0);
    __builtin_memcpy(proc_a.comm, "writer", 7);
    __builtin_memcpy(proc_b.comm, "reader", 7);

    cpu_table[0].current = &proc_a;
    sleep_lock_stat = 0;
    clock_reads = 0;
    now_us = 100;
    sleep_lock_take(&lock);
    proc_a.disk_waits++;
    now_us = 900;
    sleep_lock_give(&lock);
    check(clock_reads == 0 && rec_one.crossings == 0,
          "with kernel.lock_stat off a hold reads no clock and is not recorded");
    sleep_lock_take(&lock);
    proc_a.disk_waits++;
    sleep_lock_stat = 1;
    sleep_lock_give(&lock);
    check(clock_reads == 0 && rec_one.crossings == 0,
          "a hold taken while it was off is not recorded once it is on");

    now_us = 1000;
    sleep_lock_take(&lock);
    now_us = 9000;
    sleep_lock_give(&lock);
    check(rec_one.crossings == 0 && rec_one.longest_us == 0,
          "a long hold with no disk wait is not recorded");

    now_us = 10000;
    sleep_lock_take(&lock);
    sleep_lock_take(&lock);
    proc_a.disk_waits++;
    now_us = 10500;
    sleep_lock_give(&lock);
    check(rec_one.crossings == 0, "a hold is recorded at its last give, not an inner one");
    now_us = 15000;
    sleep_lock_give(&lock);
    check(rec_one.crossings == 1 && rec_one.longest_us == 5000 && rec_one.longest_pid == proc_a.pid,
          "a hold across a disk wait is recorded from its first take to its last give");
    check(contains(rec_one.longest_comm, "writer"), "with its holder's name");

    cpu_table[0].current = &proc_b;
    now_us = 20000;
    sleep_lock_take(&lock);
    proc_b.disk_waits += 2;
    now_us = 21000;
    sleep_lock_give(&lock);
    check(rec_one.crossings == 2 && rec_one.longest_us == 5000 && rec_one.longest_pid == proc_a.pid,
          "a shorter hold is counted and does not replace the longest");

    cycles_agree = 0;
    now_us = 0xfffff000u;
    sleep_lock_take(&lock);
    proc_b.disk_waits++;
    now_us = 0x1000u;
    sleep_lock_give(&lock);
    check(lock.taken_by_cycles == 0 && rec_one.crossings == 3 && rec_one.longest_us == 0x2000u &&
              rec_one.longest_pid == proc_b.pid && contains(rec_one.longest_comm, "reader"),
          "a hold the clock stamped is measured across its wrap by its difference");

    cycles_agree = 1;
    cpu_table[0].current = &proc_a;
    now_us = 0x5000u;
    sleep_lock_take(&lock);
    proc_a.disk_waits++;
    now_us = 0x4000u;
    sleep_lock_give(&lock);
    check(lock.taken_by_cycles == 1 && rec_one.crossings == 4 && rec_one.longest_us == 0x2000u &&
              rec_one.longest_pid == proc_b.pid,
          "a counter read below the stamp the counter made reads 0");
}

/** Each record is listed once, in the order a lock first named it, and a
 * lock with no record lists nothing. */
static void row_listing(void)
{
    const struct sleep_lock_record *r;

    sleep_lock_init(&other, &rec_two, 0);
    sleep_lock_init(&other, &rec_one, 0);
    sleep_lock_init(&other, 0, 0);
    r = sleep_lock_records();
    check(r == &rec_one && r->next == &rec_two && rec_two.next == 0,
          "the records are listed once each, in the order first named");
}

/** A lock taken before the first process has holder 0 and counts nothing
 * in a process. */
static void row_no_process(void)
{
    script_clear();
    cpu_table[0].current = 0;
    sleep_lock_init(&file_lock, &rec_two, SLEEP_LOCK_FILE);
    sleep_lock_take(&file_lock);
    check(file_lock.holder == 0 && sleep_lock_held(&file_lock),
          "a take with no process holds the lock as thread 0");
    sleep_lock_give(&file_lock);
    check(file_lock.holder == -1 && rec_two.crossings == 0, "and its give frees it");
    cpu_table[0].current = &proc_a;
}

/** A step as the signal's sender: SIGKILL pending for process B. */
static void kill_b(void)
{
    proc_b.sig_pending |= KSIG_BIT(SIGKILL);
}

/** A killed waiter leaves the line with -EINTR and holds nothing, with
 * every allocation refused; the next give hands the lock to the waiter
 * behind it, who has it after one wait, and a take after that finds the
 * lock free. A killable take of a free lock, or by its holder, has it
 * whatever is pending. */
static void row_killed(void)
{
    script_clear();
    allocations = 0;
    sleep_lock_init(&lock, &rec_one, SLEEP_LOCK_FILE);
    proc_b.sig_pending = 0;
    cpu_table[0].current = &proc_a;
    sleep_lock_take(&lock);

    script(kill_b);
    cpu_table[0].current = &proc_b;
    check(
        sleep_lock_take_killable(&lock) == -EINTR && waits == 1,
        "a waiter killed while A holds the lock gives up after its wait, with no memory to spare");
    check(!sleep_lock_held(&lock) && lock.holder == (int)proc_a.pid && proc_b.file_locks == 0,
          "and holds nothing, while A holds on");
    check(lock.state == SLEEP_LOCK_HELD && lock.first == 0 && lock.last == 0,
          "its record is off the list, which is empty");

    script(a_gives_once);
    cpu_table[0].current = &proc_c;
    sleep_lock_take(&lock);
    check(waits == 2 && sleep_lock_held(&lock) && lock.state == SLEEP_LOCK_HELD,
          "C, asking after B left, has the lock at A's give");
    sleep_lock_give(&lock);
    check(lock.state == SLEEP_LOCK_FREE && lock.first == 0, "C's give leaves no one in line");

    cpu_table[0].current = &proc_b;
    check(sleep_lock_take_killable(&lock) == 0 && sleep_lock_held(&lock) && waits == 2,
          "a killed process's take of a free lock has it with no wait");
    check(sleep_lock_take_killable(&lock) == 0 && lock.depth == 2,
          "and so does its holder's take again");
    sleep_lock_give(&lock);
    sleep_lock_give(&lock);
    check(proc_b.file_locks == 0 && proc_a.file_locks == 0 && proc_c.file_locks == 0,
          "every count of file locks is back to 0");
    check(allocations == 0, "no take or leave asked the heap for memory");
    proc_b.sig_pending = 0;
}

/** A step as the signal's sender: SIGKILL pending for process C. */
static void kill_c(void)
{
    proc_c.sig_pending |= KSIG_BIT(SIGKILL);
}

/** A step as process C: ask for `lock` with a killable take behind B,
 * and be killed in its wait. */
static void c_waits_killed(void)
{
    cpu_table[0].current = &proc_c;
    check(sleep_lock_take_killable(&lock) == -EINTR, "C, behind B, is killed and leaves");
    check(lock.first != 0 && lock.first == lock.last && lock.state == SLEEP_LOCK_QUEUED,
          "and B's record stays on the list, alone");
    proc_c.sig_pending = 0;
}

/** A waiter behind another that is killed leaves the line, and the one
 * ahead of it keeps its place and has the lock at the holder's give. */
static void row_killed_behind(void)
{
    script_clear();
    sleep_lock_init(&lock, &rec_one, 0);
    cpu_table[0].current = &proc_a;
    sleep_lock_take(&lock);

    script(c_waits_killed);
    script(kill_c);
    script(a_gives_once);
    cpu_table[0].current = &proc_b;
    sleep_lock_take(&lock);
    check(next_step == 3 && sleep_lock_held(&lock) && lock.first == 0 &&
              lock.state == SLEEP_LOCK_HELD,
          "B has the lock at A's give, with no one behind it");
    sleep_lock_give(&lock);
    check(lock.state == SLEEP_LOCK_FREE, "and B's give frees it");
}

/** A step as process A, at B's take of the list's lock as it leaves: A
 * gives `lock` back, which hands it to B. */
static void a_gives_at_leave(void)
{
    cpu_table[0].current = &proc_a;
    sleep_lock_give(&lock);
}

/** A step as the signal's sender: SIGKILL pending for process B, which A's
 * give follows at B's next take of a spinlock, the list's as it leaves. */
static void kill_b_then_give(void)
{
    kill_b();
    on_spin = a_gives_at_leave;
}

/** A waiter the lock is handed to as it leaves hands it on: with nobody
 * behind it the lock is free, and a take finds it so. */
static void row_turn_at_leave(void)
{
    script_clear();
    sleep_lock_init(&lock, &rec_one, 0);
    proc_b.sig_pending = 0;
    cpu_table[0].current = &proc_a;
    sleep_lock_take(&lock);

    script(kill_b_then_give);
    cpu_table[0].current = &proc_b;
    check(sleep_lock_take_killable(&lock) == -EINTR && on_spin == 0,
          "a waiter whose turn comes as it leaves gives up all the same");
    check(lock.holder == -1 && lock.state == SLEEP_LOCK_FREE && lock.first == 0,
          "and frees the lock, since nobody waits behind it");

    cpu_table[0].current = &proc_c;
    sleep_lock_take(&lock);
    check(waits == 1 && sleep_lock_held(&lock), "C then takes the lock with no wait");
    sleep_lock_give(&lock);
    proc_b.sig_pending = 0;
}

/* More killed takers than the 32 places a fixed table of left places held
 * at once. */
#define MANY_LEFT 40

/** Any number of takers already killed when they find the lock held give
 * up at once, with no wait, no record left on the list and no memory. */
static void row_many_left(void)
{
    int i, rc = 0;

    script_clear();
    allocations = 0;
    sleep_lock_init(&lock, &rec_one, 0);
    proc_b.sig_pending = KSIG_BIT(SIGKILL);
    cpu_table[0].current = &proc_a;
    sleep_lock_take(&lock);

    cpu_table[0].current = &proc_b;
    for (i = 0; i < MANY_LEFT; i++) {
        rc |= sleep_lock_take_killable(&lock) != -EINTR;
    }
    check(rc == 0 && waits == 0 && lock.first == 0 && lock.state == SLEEP_LOCK_HELD &&
              allocations == 0,
          "every killed taker gives up at once and leaves nothing on the list");

    cpu_table[0].current = &proc_a;
    sleep_lock_give(&lock);
    check(lock.state == SLEEP_LOCK_FREE && lock.holder == -1, "the give then frees the lock");
    cpu_table[0].current = &proc_c;
    sleep_lock_take(&lock);
    check(waits == 0 && sleep_lock_held(&lock), "C then takes the lock with no wait");
    sleep_lock_give(&lock);
    proc_b.sig_pending = 0;
}

/** A panic row's call: give back a lock nobody holds. */
static void give_free(void *arg)
{
    (void)arg;
    sleep_lock_init(&lock, 0, 0);
    sleep_lock_give(&lock);
}

/** A panic row's call: B gives back the lock A holds. */
static void give_other(void *arg)
{
    (void)arg;
    sleep_lock_init(&lock, 0, 0);
    cpu_table[0].current = &proc_a;
    sleep_lock_take(&lock);
    cpu_table[0].current = &proc_b;
    sleep_lock_give(&lock);
}

/** A panic row's call: B asserts that it holds the lock A holds. */
static void assert_other(void *arg)
{
    (void)arg;
    sleep_lock_init(&lock, 0, 0);
    cpu_table[0].current = &proc_a;
    sleep_lock_take(&lock);
    sleep_lock_assert(&lock, "test: a change outside the lock");
    cpu_table[0].current = &proc_b;
    sleep_lock_assert(&lock, "test: a change outside the lock");
}

/**
 * Run `fn(arg)` in a forked child that expects a panic whose text contains
 * `text`, and check the row `what` on the child's exit status: 42 for a
 * panic that names the rule, 44 when `fn` returns with no panic.
 */
static void panic_row(void (*fn)(void *), void *arg, const char *text, const char *what)
{
    int pid = fork();
    int status = 0;

    if (pid == 0) {
        expect_panic = text;
        fn(arg);
        _exit(44);
    }
    check(pid > 0 && waitpid(pid, &status, 0) == pid, "the panic child ran");
    check((status & 0x7f) == 0 && ((status >> 8) & 0xff) == 42, what);
}

int main(void)
{
    proc_a.pid = 11;
    proc_b.pid = 12;
    proc_c.pid = 13;
    proc_a.tg = &tg_a;
    proc_b.tg = &tg_b;
    proc_c.tg = &tg_c;

    row_alone();
    row_order();
    row_file_locks();
    row_record();
    row_listing();
    row_no_process();
    row_spin();
    row_killed();
    row_turn_at_leave();
    row_killed_behind();
    row_many_left();
    check(spin_at_wait == 0 && spin_depth == 0,
          "no row waits, or ends, with a spinlock held, the records' among them");
    check(allocations == 0, "no row asked the heap for memory");
    panic_row(give_free, 0, "does not hold", "a give of a free lock panics");
    panic_row(give_other, 0, "does not hold", "a give by a thread that does not hold it panics");
    panic_row(assert_other, 0, "a change outside the lock",
              "the assert passes for the holder and panics for another thread");

    if (fails) {
        printf("sleeplock_hosttest: %d failed\n", fails);
        return 1;
    }
    printf("sleeplock_hosttest: ok\n");
    return 0;
}
