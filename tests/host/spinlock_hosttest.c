/*
 * spinlock_hosttest.c: the kernel's spin lock of rach2/sys/lib/spinlock.c:
 * the order in which it hands the lock to its waiters, a try against its
 * waiters, a waiter that answers other processors' requests while it waits,
 * waits nested in a wait, and the test-and-set waiters under a hypervisor.
 *
 * host-deps: tests/host/compat/fiber_host.c
 * host-cflags: -DRACH2_HOST_CPU
 *
 * One host thread plays every processor. Each waiting processor is a call
 * on a stack of its own (fiber_host.c) that yields at every turn of a wait,
 * where the kernel's waiter calls `arch_relax`; the test plays processor 0,
 * resumes the waiters in an order each row chooses and sets `host_cpu`, the
 * processor `arch_cpu_id` names, before each. A row checks only what the
 * interface shows: which processor holds the lock in which order, what a
 * try returns, and what each waiter answered. Every row fails on the
 * test-and-set lock the kernel had before: its releaser takes the lock
 * again ahead of its waiters, a try takes it from them, the waiter resumed
 * first takes it, and a nested wait for the same lock spins. The row of
 * the hypervisor's waiters fails on the queue, whose waiter that does not
 * run holds up the one that does.
 */

#include "lib/spinlock.c"

#include "fiber_host.h"

extern int printf(const char *fmt, ...);
extern void abort(void);

unsigned int host_cpu;

/* Each processor's requests from others, as `smp_service` answers them:
 * a TLB request, the requests answered, and a function to run. */
static volatile int tlb_request[FIBERS];
static int answered[FIBERS];
static void (*call_fn[FIBERS])(void);

/* The order in which processors took a row's locks, and the panic a row
 * met, NULL for none. */
static int order_a[8], order_b[8];
static int taken_a, taken_b;
static const char *panicked;
static int fails;

static struct spinlock lock_a = SPINLOCK_INIT, lock_b = SPINLOCK_INIT;

/** Count a failed check and print `what`. */
static void check(int ok, const char *what)
{
    if (!ok) {
        printf("spinlock_hosttest: fail: %s\n", what);
        fails++;
    }
}

/* ---- what spinlock.c reaches outside itself ---- */

unsigned int arch_atomic_xchg(volatile unsigned int *p, unsigned int value)
{
    unsigned int was = *p;

    *p = value;
    return was;
}

unsigned int arch_atomic_cmpxchg(volatile unsigned int *p, unsigned int old, unsigned int value)
{
    unsigned int was = *p;

    if (was == old) {
        *p = value;
    }
    return was;
}

void arch_store_u8(volatile unsigned char *p, unsigned char value)
{
    *p = value;
}

void arch_store_u16(volatile unsigned short *p, unsigned short value)
{
    *p = value;
}

/** A turn of a wait: the waiter yields to the test. The test itself never
 * waits, since nothing would let it go on. */
void arch_relax(void)
{
    if (fiber_running() < 0) {
        printf("spinlock_hosttest: fail: the test's own processor waited\n");
        abort();
    }
    fiber_yield();
}

/** Answer this processor's TLB request and run its function, once each. */
void smp_service(void)
{
    void (*fn)(void) = call_fn[host_cpu];

    if (tlb_request[host_cpu]) {
        tlb_request[host_cpu] = 0;
        answered[host_cpu]++;
    }
    if (fn) {
        call_fn[host_cpu] = NULL;
        fn();
    }
}

/** The host is no guest: waiters queue, except in `row_virt`, which sets
 * `q_virt` itself. */
int arch_spin_virt(void)
{
    return 0;
}

uint32_t kirq_save(void)
{
    return 0;
}

void kirq_restore(uint32_t flags)
{
    (void)flags;
}

/** Note the panic and stop the processor that met it for good: the test
 * never resumes it again. */
void k_panic(const char *message)
{
    panicked = message;
    for (;;) {
        fiber_yield();
    }
}

/* ---- the processors ---- */

/** Take `lock_a` as processor `host_cpu`, note the order, hold it for a
 * turn and give it back. */
static void take_a(void)
{
    int cpu = (int)host_cpu;

    spin_lock(&lock_a);
    order_a[taken_a++] = cpu;
    fiber_yield();
    spin_unlock(&lock_a);
}

/** `take_a` for `lock_b`. */
static void take_b(void)
{
    int cpu = (int)host_cpu;

    spin_lock(&lock_b);
    order_b[taken_b++] = cpu;
    fiber_yield();
    spin_unlock(&lock_b);
}

/** Start processor `cpu`'s call `fn`, which runs until its first turn. */
static void start(int cpu, void (*fn)(void))
{
    host_cpu = (unsigned int)cpu;
    check(fiber_start(cpu, fn) == 0, "a waiter took a held lock");
    host_cpu = 0;
}

/** Resume processor `cpu` for one turn: 1 when its call returned. */
static int turn(int cpu)
{
    int done;

    host_cpu = (unsigned int)cpu;
    done = fiber_resume(cpu);
    host_cpu = 0;
    return done;
}

/** Resume the live processors of `cpus`, `n` of them, a turn each in the
 * order given, until all have returned; a hundred rounds of turns with one
 * left is a waiter that never takes its lock. */
static void run(const int *cpus, int n)
{
    int round, i, live = n;

    for (round = 0; live > 0 && round < 100; round++) {
        for (i = 0, live = 0; i < n; i++) {
            if (fiber_live(cpus[i]) && !turn(cpus[i])) {
                live++;
            }
        }
    }
    check(live == 0, "a waiter never took its lock");
    for (i = 0; i < n; i++) {
        if (fiber_live(cpus[i])) {
            fiber_drop(cpus[i]);
        }
    }
}

/** Begin a row: no order noted and no request left. */
static void row_begin(void)
{
    int i;

    taken_a = taken_b = 0;
    lock_a.locked = lock_b.locked = 0;
    for (i = 0; i < FIBERS; i++) {
        tlb_request[i] = 0;
        answered[i] = 0;
        call_fn[i] = NULL;
    }
    panicked = NULL;
}

/* ---- the rows ---- */

/**
 * Three processors wait for the lock processor 0 holds, coming as 1, 2 and
 * 3. Processor 0 gives it back and tries it at once: the try fails, since
 * the lock is the first waiter's. The waiters, resumed last first, take it
 * as 1, 2 and 3, and it ends free.
 */
static void row_order(void)
{
    static const int back[3] = {3, 2, 1};

    row_begin();
    spin_lock(&lock_a);
    start(1, take_a);
    start(2, take_a);
    start(3, take_a);
    spin_unlock(&lock_a);
    if (spin_trylock(&lock_a)) {
        check(0, "order: the releaser took the lock again ahead of its waiters");
        spin_unlock(&lock_a);
    }
    run(back, 3);
    check(taken_a == 3 && order_a[0] == 1 && order_a[1] == 2 && order_a[2] == 3,
          "order: the waiters took the lock out of the order they came in");
    check(lock_a.locked == 0, "order: the lock is not free at the end");
}

/**
 * Two processors wait for the lock processor 0 holds. Once processor 0
 * gives it back, a try by processor 5 fails while the first waiter has not
 * yet run, while it holds the lock, and once it gave the lock back to a
 * second waiter that has not yet run. With no waiter left, a try takes the
 * free lock.
 */
static void row_trylock(void)
{
    static const int both[2] = {1, 2};

    row_begin();
    spin_lock(&lock_a);
    start(1, take_a);
    start(2, take_a);
    spin_unlock(&lock_a);
    host_cpu = 5;
    if (spin_trylock(&lock_a)) {
        check(0, "trylock: a try took the lock from the waiter it was given to");
        spin_unlock(&lock_a);
    }
    host_cpu = 0;
    check(turn(1) == 0 && taken_a == 1 && order_a[0] == 1,
          "trylock: the first waiter did not take the lock");
    host_cpu = 5;
    check(!spin_trylock(&lock_a), "trylock: a try took a held lock");
    host_cpu = 0;
    check(turn(1) == 1, "trylock: the first waiter did not give the lock back");
    host_cpu = 5;
    if (spin_trylock(&lock_a)) {
        check(0, "trylock: a try took the lock ahead of the waiter queued for it");
        spin_unlock(&lock_a);
    }
    host_cpu = 0;
    run(both, 2);
    check(taken_a == 2 && order_a[1] == 2, "trylock: the second waiter did not take the lock");
    check(spin_trylock(&lock_a), "trylock: a try did not take the free lock");
    spin_unlock(&lock_a);
}

/**
 * Three processors wait for the lock processor 0 holds: the first by its
 * pending bit, the second as the queue's head, the third behind it, on its
 * own place. Processor 0, as a holder that drops a translation on every
 * processor, asks each for an answer and has it answered before it gives
 * the lock back; the waiters keep their places and take it as 1, 2 and 3.
 */
static void row_tlb(void)
{
    static const int back[3] = {3, 2, 1};
    int cpu;

    row_begin();
    spin_lock(&lock_a);
    start(1, take_a);
    start(2, take_a);
    start(3, take_a);
    for (cpu = 1; cpu <= 3; cpu++) {
        tlb_request[cpu] = 1;
    }
    for (cpu = 3; cpu >= 1; cpu--) {
        check(turn(cpu) == 0, "tlb: a waiter took a held lock");
        check(tlb_request[cpu] == 0 && answered[cpu] == 1,
              "tlb: a waiter did not answer while it waited");
    }
    check(taken_a == 0, "tlb: a waiter took the lock its holder still holds");
    spin_unlock(&lock_a);
    run(back, 3);
    check(taken_a == 3 && order_a[0] == 1 && order_a[1] == 2 && order_a[2] == 3,
          "tlb: a waiter that answered lost its place");
}

/**
 * Processor 0 holds `lock_a` and `lock_b`; processor 4 waits for `lock_b`,
 * and processors 1, 2 and 3 for `lock_a`. Processors 2 and 3, while they
 * wait, are asked to run a function that takes `lock_b`, a wait at the
 * next level of each: they queue for `lock_b` behind processor 4, in the
 * order they came, and keep their places in `lock_a`'s queue. `lock_b`
 * goes to 4, 2 and 3, then `lock_a` to 1, 2 and 3.
 */
static void row_nested(void)
{
    static const int all[4] = {4, 3, 2, 1};

    row_begin();
    spin_lock(&lock_a);
    spin_lock(&lock_b);
    start(4, take_b);
    start(1, take_a);
    start(2, take_a);
    start(3, take_a);
    call_fn[2] = take_b;
    call_fn[3] = take_b;
    check(turn(2) == 0 && call_fn[2] == NULL, "nested: processor 2 did not run its function");
    check(turn(3) == 0 && call_fn[3] == NULL, "nested: processor 3 did not run its function");
    check(taken_b == 0, "nested: a nested wait took a held lock");
    check(turn(4) == 0 && turn(3) == 0 && turn(2) == 0, "nested: a waiter took a held lock");
    spin_unlock(&lock_b);
    check(turn(3) == 0 && turn(2) == 0 && turn(4) == 0,
          "nested: a waiter returned with a lock held");
    check(taken_b == 1 && order_b[0] == 4, "nested: lock_b did not go to its first waiter");
    check(taken_a == 0, "nested: a waiter took lock_a while processor 0 holds it");
    spin_unlock(&lock_a);
    run(all, 4);
    check(taken_b == 3 && order_b[0] == 4 && order_b[1] == 2 && order_b[2] == 3,
          "nested: lock_b's nested waiters took it out of the order they came in");
    check(taken_a == 3 && order_a[0] == 1 && order_a[1] == 2 && order_a[2] == 3,
          "nested: lock_a's waiters lost their places to the nested waits");
    check(lock_a.locked == 0 && lock_b.locked == 0, "nested: a lock is not free at the end");
}

/** Take `lock_a` again from inside the wait for it. */
static void take_a_again(void)
{
    spin_lock(&lock_a);
    spin_unlock(&lock_a);
}

/**
 * Processor 7 waits for `lock_a`, and inside its wait is asked to run a
 * function that takes `lock_a` again: the queue would never reach that
 * wait, which is behind the outer one, so it stops with a panic. Processor
 * 7 is not resumed again. The test-and-set lock waited instead, which in
 * this model is the lock taken once the holder gives it back.
 */
static void row_same(void)
{
    row_begin();
    spin_lock(&lock_a);
    start(7, take_a);
    call_fn[7] = take_a_again;
    (void)turn(7);
    (void)turn(7);
    check(panicked != NULL, "same: a nested wait for the lock its own wait is for did not panic");
    fiber_drop(7);
    lock_a.locked = 0;
}

/**
 * Under a hypervisor waiters take the lock by test-and-set. Processors 1
 * and 2 wait for the lock processor 0 holds, coming in that order;
 * processor 1's turn never comes, as when the host preempted it, so
 * processor 2 takes the lock once processor 0 gives it back, then
 * processor 1 once 2 has. Each answers a TLB request while it waits.
 */
static void row_virt(void)
{
    static const int one[1] = {1};

    row_begin();
    q_virt = 1;
    spin_lock(&lock_a);
    start(1, take_a);
    start(2, take_a);
    tlb_request[1] = tlb_request[2] = 1;
    check(turn(2) == 0 && turn(1) == 0 && answered[1] == 1 && answered[2] == 1,
          "virt: a waiter did not answer while it waited");
    spin_unlock(&lock_a);
    check(turn(2) == 0 && taken_a == 1 && order_a[0] == 2,
          "virt: a waiter that runs did not take the lock ahead of one that does not");
    check(turn(2) == 1, "virt: the waiter did not give the lock back");
    run(one, 1);
    check(taken_a == 2 && order_a[1] == 1 && lock_a.locked == 0,
          "virt: the other waiter did not take the lock after it");
    q_virt = 0;
}

int main(void)
{
    q_virt = 0;
    row_order();
    row_trylock();
    row_tlb();
    row_nested();
    row_same();
    row_virt();
    if (fails) {
        printf("spinlock_hosttest: %d failed\n", fails);
        return fails > 255 ? 255 : fails;
    }
    printf("spinlock_hosttest: ok\n");
    return 0;
}
