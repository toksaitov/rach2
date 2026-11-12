/**
 * Host test of proc/smp.c: the processors it interrupts to drop a page's
 * translation or give up an address space, to stop, and the start of the
 * others at boot.
 *
 * host-deps: rach2/sys/lib/kstring.c rach2/sys/lib/kctypes.c
 * host-cflags: -DRACH2_HOST_CPU
 *
 * proc/smp.c is included whole and runs as processor 0, the caller; three
 * other processors are records of `cpu_table` that a row sets up. The
 * interprocessor interrupt is the test's: it counts the interrupts each
 * processor is sent and answers each request at once, as the processor's
 * `smp_service` would, so that the caller's wait ends.
 *
 * The rows: a processor that runs a thread of the directory is interrupted and
 * asked to drop the page; an idle processor that kept the directory loaded is
 * not interrupted and has it marked stale, which its next switch to a thread of
 * the directory answers by loading it again; a processor in another directory
 * is left alone; and before the directory is freed, the idle processor that
 * kept it is interrupted and gives it up for the kernel's. A stop asks every
 * other processor and waits a second for one that does not stop, then sends it
 * an NMI, which the real `smp_stop_nmi` takes, run as that processor; each
 * processor that stops, by the interrupt's `smp_service` or by the NMI, runs
 * `k_panic_hook` before it marks itself stopped. A call (`smp_call`) to the
 * caller's own processor runs at once with interrupts off; one to another
 * processor runs in that processor's `smp_service`, whose slot is cleared only
 * after the function returns; and the wait for one ends when the processor goes
 * offline. A wait that spins `RELAX_BOUND` turns of `arch_relax` has lost the
 * test that ends it: the model fails the run and ends the wait, instead of
 * stalling the suite. At boot the wait for a started processor ends at
 * `CPU_ONLINE_WAIT_US`, and a processor that comes after it stops, neither
 * online nor counted. The profiler's sample of a hold of the big lock walks the
 * frames of the interrupted thread's kernel stack and stops at a saved frame
 * pointer above it, the user's value that the outermost kernel frame holds. The
 * big lock's counts: each processor's calls in its own tally, summed by
 * bkl/calls; a hold charged to the entry of a thread that ran without the lock;
 * the takes after an idle wait and a switch under their own keys; each thread's
 * record of its entry frames; the ioctl slots; and a loan of the lock to
 * one waiting processor alone, which gives it back with the word never clear.
 *
 * Returns 0 when every row passes, 1 otherwise.
 */

#include "proc/smp.c"
#include "compat/waketime_none.h"

#include <setjmp.h>
#include <string.h>

extern int printf(const char *format, ...);
extern void *calloc(unsigned long count, unsigned long size);
extern void free(void *ptr);

static int fails;

/** No row runs a system call without the big lock, so a hold names no call
 * by its number (`entry_of`). */
int isr_call_entry(const struct process *p, uint32_t *cmd)
{
    (void)p;
    *cmd = 0;
    return -1;
}

/** Count a failure and print `row` when `ok` is 0. */
static void check(int ok, const char *row)
{
    if (!ok) {
        printf("smp-hosttest: fail: %s\n", row);
        fails++;
    }
}

/* ---- what smp.c reaches outside itself ---- */

struct percpu cpu_table[NR_CPUS];
process_t *proc_idle;
page_dir_t *_kernel_dir;

/* The interrupts each processor was sent, the page of its last TLB
 * request, whether it takes a stop it is asked for, whether it goes offline
 * at the next interrupt instead of answering it, and the microseconds
 * waited. */
static int ipis[NR_CPUS];
static uintptr_t ipi_page[NR_CPUS];
static int takes_stop[NR_CPUS];
static int goes_offline[NR_CPUS];
static unsigned long waited_us;

/* The turns a wait may spin since the last interrupt sent before the model
 * ends it. Every request the model is sent is answered, or its processor
 * gone offline, before the sender's wait first looks, so a wait that spins
 * at all has lost the test that ends it. */
#define RELAX_BOUND 1000
static int relax_turns;
/* What another processor does at the next turn of a wait, once, in place of
 * counting it: the borrower of a loan of the big lock. */
static void (*relax_hook)(void);

/* Whether the model's interrupts are off, which `kirq_save` and
 * `spin_lock_irqsave` set and their restores put back. */
static uint32_t irqs_off;

/** The host model runs no code a barrier must serialize. */
void arch_sync_core(void)
{}

/* The processor the code runs as, which `arch_cpu_id` gives. */
unsigned int host_cpu;

/* The NMIs sent to each processor, whether it takes one, and, for each
 * processor, how many times `k_panic_hook` ran there and whether it was
 * already marked stopped when it last did. */
static int nmis[NR_CPUS];
static int takes_nmi[NR_CPUS];
static int hooked[NR_CPUS];
static int hooked_late[NR_CPUS];

/** The virtualization cleanup, as vmx.c's: count it on this processor. */
static void hook(void)
{
    hooked[host_cpu]++;
    hooked_late[host_cpu] = cpu_table[host_cpu].stopped;
}

void (*k_panic_hook)(void) = hook;

/* Where a processor that stops goes on the host: back to the row. */
static jmp_buf stopped;

/** Jump back to the row, as the processor that stops. */
void arch_stop(void)
{
    longjmp(stopped, 1);
}

/** Run `fn` as processor `cpu`, returning when it returns or stops. */
static void run_as(int cpu, int (*fn)(void))
{
    unsigned int was = host_cpu;

    host_cpu = (unsigned int)cpu;
    if (setjmp(stopped) == 0) {
        (void)fn();
    }
    host_cpu = was;
}

/** `smp_service`, for `run_as`. */
static int service(void)
{
    smp_service();
    return 0;
}

/** No processor has begun a panic here. */
int k_panicking(void)
{
    return 0;
}

/** No performance counter here: a stopped processor has none to stop. */
void platform_pmu_disable(void)
{}

/** Count an NMI sent to `cpu` and, when `takes_nmi` says it takes one, run
 * the real stop handler there; one that does not stays stuck. */
void platform_cpu_nmi(int cpu)
{
    nmis[cpu]++;
    if (takes_nmi[cpu]) {
        run_as(cpu, smp_stop_nmi);
    }
}

/** Count an interrupt sent to `cpu` and answer its requests at once: a
 * stop, when `takes_stop` says so, and a function to run, by the real
 * `smp_service` run there; a page to drop, and a directory to give up for
 * the kernel's, as `smp_service` would. When `goes_offline` says so, the
 * processor goes offline instead and answers nothing. */
void platform_cpu_ipi(int cpu)
{
    struct percpu *c = &cpu_table[cpu];

    ipis[cpu]++;
    relax_turns = 0;
    if (goes_offline[cpu]) {
        c->online = 0;
        return;
    }
    if ((c->stop_request && takes_stop[cpu]) || c->call_fn) {
        run_as(cpu, service);
        return;
    }
    if (c->tlb_request) {
        ipi_page[cpu] = c->tlb_page;
        c->tlb_request = 0;
    }
    if (c->space_drop) {
        if (c->space == c->space_drop) {
            c->space = _kernel_dir;
        }
        c->space_drop = NULL;
    }
}

/** Interrupt each processor of the set as `platform_cpu_ipi` does one. */
void platform_cpu_ipi_many(uint64_t cpus)
{
    int c;

    for (c = 0; cpus != 0; c++, cpus >>= 1) {
        if (cpus & 1u) {
            platform_cpu_ipi(c);
        }
    }
}

/** The host model has no watchdog. */
void watchdog_wait(void)
{}

/* The processors the platform has, and the entry of the one the late row
 * starts, which reaches its first C only after the boot's wait. */
static int cpu_count = 1;
static void (*late_entry)(int cpu);

/** `cpu_count`, whatever `limit`. */
int platform_cpu_count(int limit)
{
    (void)limit;
    return cpu_count;
}

/** Nothing: the test maps no temporary slot. */
void paging_temp_split(unsigned int cpus)
{
    (void)cpus;
}

/** Keep `entry` in `late_entry` for the row to call after the wait; 0. */
int platform_cpu_start(int cpu, uintptr_t stack_top, void (*entry)(int cpu))
{
    (void)cpu;
    (void)stack_top;
    late_entry = entry;
    return 0;
}

/** The host's calloc. */
void *kzalloc(size_t size)
{
    return calloc(1, size);
}

/** The host's free. */
void kfree(void *p)
{
    free(p);
}

/* The idle tasks' stacks, which the kernel keeps for good and records only
 * by their tops, one past the end; kept here so that the test can give them
 * back at its end and LeakSanitizer does not take them for lost. */
static void *guarded[NR_CPUS * 2];
static int nguarded;

/** `count` zeroed pages from the host heap, kept in `guarded`. */
void *kalloc_guarded(uint32_t count)
{
    void *p = calloc(count, PAGE_SIZE);

    if (p && nguarded < (int)(sizeof(guarded) / sizeof(guarded[0]))) {
        guarded[nguarded++] = p;
    }
    return p;
}

/** 0: the command line has no values. */
int kcmdline_option(const char *key, int *out)
{
    (void)key;
    (void)out;
    return 0;
}

/** Nothing: no idle task runs. */
void setup_process_stack(process_t *proc, void (*entry_point)(void))
{
    (void)proc;
    (void)entry_point;
}

/** Nothing: no row reads a processor's accounted time. */
void sched_account_start(void)
{}

/** Nothing: no idle task runs. */
void sched_idle_loop(void)
{}

/** Add `usec` to the time waited, without waiting. */
void k_udelay(uint32_t usec)
{
    waited_us += usec;
}

/* The cycle counter, which only the profiler's row moves. */
static uint64_t cycles_now;

/** `cycles_now`: the other rows do not measure a lock's hold or wait. */
uint64_t arch_cycles(void)
{
    return cycles_now;
}

/** Zero: no hypervisor here flushes a preempted processor's TLB. */
int arch_cpu_tlb_deferred(int cpu)
{
    (void)cpu;
    return 0;
}

/** Zero: this model calibrates no invariant counter. */
uint64_t timer_cycles_us(uint64_t cycles)
{
    (void)cycles;
    return 0;
}

/** Nothing: no row reads the log. */
void klog(int level, const char *subsystem, const char *format, ...)
{
    (void)level;
    (void)subsystem;
    (void)format;
}

/** Print `str` and count a failure. */
void k_panic(const char *str)
{
    printf("smp-hosttest: panic: %s\n", str);
    fails++;
}

/** 0: the directories have no physical address. */
uintptr_t get_page_dir_phys(page_dir_t *dir)
{
    (void)dir;
    return 0;
}

/** Exchange `*p` and `value` on the host's one thread; the old value. */
unsigned int arch_atomic_xchg(volatile unsigned int *p, unsigned int value)
{
    unsigned int old = *p;

    *p = value;
    return old;
}

/**
 * Count a turn of a wait for another processor and, past `RELAX_BOUND`
 * turns, count a failure and end the wait: every processor but the one
 * running is marked offline and its requests cleared, so that a wait that
 * lost its test of `online`, or whose processor never clears its slot,
 * fails the row instead of stalling the suite, which sets no time limit.
 * Linux's CSD lock debugging reports a call whose wait passes its bound in
 * the same way (`csd_lock_wait_toolong`, kernel/smp.c:229-312, 318-331).
 */
void arch_relax(void)
{
    void (*hook)(void) = relax_hook;
    int c;

    if (hook) {
        relax_hook = NULL;
        hook();
        return;
    }
    if (++relax_turns <= RELAX_BOUND) {
        return;
    }
    check(0, "a wait for another processor ends without the model's bound");
    for (c = 0; c < NR_CPUS; c++) {
        if ((unsigned int)c != host_cpu) {
            cpu_table[c].online = 0;
            cpu_table[c].call_fn = NULL;
            cpu_table[c].tlb_request = 0;
            cpu_table[c].barrier_request = 0;
            cpu_table[c].space_drop = NULL;
        }
    }
    relax_turns = 0;
}

/** Compare and exchange `*p` on the host's one thread; the old value. */
unsigned int arch_atomic_cmpxchg(volatile unsigned int *p, unsigned int old, unsigned int value)
{
    unsigned int was = *p;

    if (was == old) {
        *p = value;
    }
    return was;
}

/** 0: the host has no translation root. */
uintptr_t arch_space_root(void)
{
    return 0;
}

/** Nothing: the host has no translation root. */
void arch_space_load(uintptr_t root)
{
    (void)root;
}

/** Nothing: the host has no TLB. */
void arch_tlb_flush_page(uintptr_t va)
{
    (void)va;
}

/* The global tick a started processor's accounting begins from. */
volatile uint64_t kernel_ticks;

/** Turn the model's interrupts off; whether they were. */
uint32_t kirq_save(void)
{
    uint32_t was = irqs_off;

    irqs_off = 1;
    return was;
}

/** Put the model's interrupts back as `flags` says. */
void kirq_restore(uint32_t flags)
{
    irqs_off = flags;
}

/* ---- the rows ---- */

/* Three directories, which smp.c only compares, so any distinct addresses
 * serve. */
static long dirs[3];
#define KERNEL_DIR ((page_dir_t *)&dirs[0])
#define DIR_A      ((page_dir_t *)&dirs[1])
#define DIR_B      ((page_dir_t *)&dirs[2])

static process_t self, idle2, user_a, user_b;

/** Put processor `c` online in `dir`, running `p`. */
static void place(int c, page_dir_t *dir, process_t *p)
{
    cpu_table[c].online = 1;
    cpu_table[c].space = dir;
    cpu_table[c].current = p;
    cpu_table[c].space_stale = 0;
    ipis[c] = 0;
    ipi_page[c] = 0;
}

/* A kernel stack with room above its top, where the user's frame pointer
 * leads, and the word there that the walk must not read. */
#define STACK_WORDS (KERNEL_STACK_SIZE / sizeof(uintptr_t))
static uintptr_t kstack_model[STACK_WORDS + 4];
#define ABOVE_SECRET 0x5ec2e7u

/** Sample a hold through two frames whose outer one saved a frame pointer
 * just above the stack, and check that the walk records both return
 * addresses and stops there. */
static void row_sample_stack(void)
{
    uintptr_t *k = kstack_model, pc[BKL_SAMPLE_PCS];
    uint64_t age;

    k[STACK_WORDS - 8] = (uintptr_t)&k[STACK_WORDS - 4];
    k[STACK_WORDS - 7] = 0x1111u;
    k[STACK_WORDS - 4] = (uintptr_t)&k[STACK_WORDS + 2];
    k[STACK_WORDS - 3] = 0x2222u;
    k[STACK_WORDS + 3] = ABOVE_SECRET;
    self.kernel_stack = (uintptr_t)&k[STACK_WORDS];
    cpu_table[0].current = &self;
    cpu_table[0].bkl_depth = 2;
    cpu_table[0].bkl_since = 0;
    cycles_now = 100;
    bkl_hold_sample(0x0aaau, (uintptr_t)&k[STACK_WORDS - 8]);
    cycles_now = 0;
    cpu_table[0].bkl_depth = 0;
    smp_hold_sample(&age, pc);
    check(pc[0] == 0x0aaau && pc[1] == 0x1111u && pc[2] == 0x2222u,
          "a hold's sample records the pc and the return addresses on the kernel stack");
    check(pc[3] == 0, "and stops at a saved frame pointer above the stack, reading nothing there");
}

/* The tallies of two processors, for the rows of bkl/calls. */
static struct bkl_tally tally_rows[2][BKL_TALLIES];

/* The system call `call_on` counts. */
static uint32_t call_nr;

/** Count one call of `call_nr` as the running processor, for `run_as`. */
static int call_on(void)
{
    smp_call_end(call_nr, 0, 0);
    return 0;
}

/** Whether `text` holds `line` at the start of one of its lines. */
static int has_line(const char *text, const char *line)
{
    const char *t = text;

    while (*t) {
        const char *a = t, *b = line;

        while (*b && *a == *b) {
            a++;
            b++;
        }
        if (!*b) {
            return 1;
        }
        while (*t && *t != '\n') {
            t++;
        }
        if (*t) {
            t++;
        }
    }
    return 0;
}

/**
 * The big lock's counts: each processor counts its calls in its own tally,
 * which a read of bkl/calls sums; a hold taken for a thread whose entry ran
 * without the lock is charged to that entry; the takes after an idle wait
 * and after a switch count under keys of their own; each thread keeps its
 * own record of which entry frames took the lock; and an ioctl request
 * keeps the slot it claimed while another takes a second.
 */
static void row_bkl_counts(void)
{
    static char text[16384];
    int a, b;

    bkl_tally[0] = tally_rows[0];
    bkl_tally[1] = tally_rows[1];
    bkl_booted = 1;
    call_nr = 39;
    run_as(0, call_on);
    run_as(1, call_on);
    run_as(1, call_on);
    text[smp_calls_show(NULL, text, sizeof(text) - 1)] = 0;
    check(tally_rows[0][39].calls == 1 && tally_rows[1][39].calls == 2 &&
              has_line(text, "syscall 39 3 "),
          "each processor counts its calls, and bkl/calls sums them");

    cpu_table[0].current = &self;
    cpu_table[0].bkl_depth = 0;
    self.bkl_entry = 7u + 1u;
    bkl_enter();
    check(bkl_held() && cpu_table[0].bkl_nr == 7 && cpu_table[0].bkl_vector == BKL_CALL_VECTOR,
          "a hold taken inside an entry that ran without the lock is charged to that entry");
    bkl_exit();
    check(!bkl_held() && bkl_calls[7].holds == 1, "and counted under it when it ends");
    self.bkl_entry = 0;

    bkl_take_as(1, BKL_KEY_IDLE);
    check(bkl_drop() == 1 && bkl_calls[BKL_KEY_IDLE].holds == 1,
          "a take after an idle wait counts under its own key");
    bkl_take_as(1, BKL_KEY_SWITCH);
    check(bkl_drop() == 1 && bkl_calls[BKL_KEY_SWITCH].holds == 1,
          "and a take after a switch under another");
    text[smp_calls_show(NULL, text, sizeof(text) - 1)] = 0;
    check(has_line(text, "take 0 0 0 1 ") && has_line(text, "take 1 0 0 1 "),
          "bkl/calls prints both takes");

    bkl_frame_push(1);
    cpu_table[0].current = &user_a;
    bkl_frame_push(0);
    check(bkl_frame_top() == 0, "a thread's entry frame that ran unlocked is its own");
    cpu_table[0].current = &self;
    check(bkl_frame_pop() == 1 && self.bkl_frames == 0,
          "and another thread's frame that took the lock stays its own");
    cpu_table[0].current = &user_a;
    check(bkl_frame_pop() == 0 && user_a.bkl_frames == 0, "each pops its own");
    cpu_table[0].current = &self;

    a = ioctl_slot(0xc0406469u);
    b = ioctl_slot(0x80086401u);
    check(a > 0 && b > 0 && a != b && ioctl_slot(0xc0406469u) == a,
          "an ioctl request keeps its slot, and another request takes a second");
    bkl_tally[0] = bkl_tally_boot;
    bkl_tally[1] = NULL;
}

/** A kernel thread's entry, which the rows never run. */
static void kthread_model(void)
{}

/**
 * A kernel thread's own holds, its take after a switch and its own
 * `bkl_enter` outside any entry, count under its pid and name in bkl/calls,
 * and not under `take 1` or `vector 0` as well; an interrupt's hold while
 * it runs stays under the interrupt's vector; two processors' idle tasks
 * count as one thread; and a write zeroes the threads' lines.
 */
static void row_kthread_counts(void)
{
    static char text[16384];
    static process_t worker, idle_a, idle_b; /* idle tasks, pid 0 each */
    process_t *was_idle;

    bkl_booted = 1;
    k_memset(&worker, 0, sizeof(worker));
    worker.pid = 41;
    k_memcpy(worker.comm, "kmodel", 7);
    worker.kthread_entry = kthread_model;
    cpu_table[0].current = &worker;
    cpu_table[0].bkl_depth = 0;
    (void)smp_calls_store(NULL, NULL, 1);

    bkl_take_as(1, BKL_KEY_SWITCH);
    (void)bkl_drop();
    bkl_enter();
    bkl_exit();
    text[smp_calls_show(NULL, text, sizeof(text) - 1)] = 0;
    check(has_line(text, "kthread 41 0 0 2 ") && strstr(text, " kmodel\n") != NULL,
          "a kernel thread's take after a switch and its own enter count under its pid and name");
    check(bkl_calls[BKL_KEY_SWITCH].holds == 0 && bkl_calls[BKL_ENTRY_SYSCALLS].holds == 0 &&
              !has_line(text, "take 1 ") && !has_line(text, "vector 0 "),
          "and under no key besides");

    worker.bkl_entry = BKL_ENTRY_SYSCALLS + 32u + 1u;
    bkl_enter();
    bkl_exit();
    worker.bkl_entry = 0;
    check(bkl_calls[BKL_ENTRY_SYSCALLS + 32].holds == 1,
          "an interrupt's hold while the kernel thread runs counts under its vector");

    k_memcpy(idle_a.comm, "idle", 5);
    k_memcpy(idle_b.comm, "idle", 5);
    was_idle = cpu_table[0].idle;
    cpu_table[0].current = cpu_table[0].idle = &idle_a;
    (void)bkl_try_enter();
    bkl_exit();
    cpu_table[0].current = cpu_table[0].idle = &idle_b;
    bkl_enter();
    bkl_exit();
    cpu_table[0].idle = was_idle;
    cpu_table[0].current = &self;
    text[smp_calls_show(NULL, text, sizeof(text) - 1)] = 0;
    check(has_line(text, "kthread 0 0 0 2 ") && strstr(text, " idle\n") != NULL,
          "the idle tasks' own holds count as one thread's, pid 0");

    (void)smp_calls_store(NULL, NULL, 1);
    text[smp_calls_show(NULL, text, sizeof(text) - 1)] = 0;
    check(!has_line(text, "kthread "), "a write zeroes the kernel threads' lines");
}

/* What the borrower of a loan of the big lock found while it held it, and
 * after it gave it back. */
static int borrowed, returned;

/** Run as processor 1, which waits for the big lock: take it by the loan,
 *  find the word still set and the lender's depth unchanged, and give it
 *  back at the outermost exit, which leaves the word set. */
static void borrow_turn(void)
{
    unsigned int was = host_cpu;

    host_cpu = 1;
    bkl_enter();
    borrowed = bkl_held() && bkl_word == 1u && bkl_lent == (2u | BKL_LENT_TAKEN) &&
               cpu_table[0].bkl_depth == 2;
    bkl_exit();
    returned = !bkl_held() && bkl_lent == 0u && bkl_word == 1u;
    host_cpu = was;
}

/** Processor 1 stops waiting for the big lock without taking the loan. */
static void stop_waiting(void)
{
    cpu_table[1].wake_bkl_waiting = 0;
}

/**
 * A loan of the big lock goes to the processor named alone, and only while
 * it waits for the lock and runs the thread named; the borrower takes it
 * with the word still set, gives it back at its outermost exit, and the
 * lender holds it again at its own depth, with no moment the word was
 * clear. A loan the borrower stopped waiting for is withdrawn.
 */
static void row_bkl_lend(void)
{
    bkl_tally[0] = tally_rows[0];
    bkl_tally[1] = tally_rows[1];
    cpu_table[0].current = &self;
    cpu_table[1].current = &user_a;
    cpu_table[0].bkl_depth = 0;
    cpu_table[1].bkl_depth = 0;
    cpu_table[1].wake_bkl_waiting = 0;
    check(bkl_lend(1, &user_a) == 0, "a processor that does not hold the big lock lends nothing");
    bkl_enter();
    bkl_enter();
    check(bkl_lend(1, &user_a) == 0 && bkl_lent == 0u,
          "nothing is lent to a processor that does not wait for the lock");
    cpu_table[1].wake_bkl_waiting = 1;
    check(bkl_lend(1, &user_b) == 0 && bkl_lend(0, &self) == 0 && bkl_lent == 0u,
          "or to one that runs another thread, or to the lender itself");
    relax_hook = borrow_turn;
    borrowed = returned = 0;
    check(bkl_lend(1, &user_a) == 1 && borrowed && returned,
          "the waiter takes the lent lock with the word set and gives it back");
    check(cpu_table[0].bkl_depth == 2 && bkl_word == 1u && bkl_lent == 0u &&
              !cpu_table[1].wake_bkl_waiting,
          "and the lender holds it again at its own depth");
    cpu_table[1].wake_bkl_waiting = 1;
    relax_hook = stop_waiting;
    check(bkl_lend(1, &user_a) == 0 && bkl_lent == 0u && bkl_word == 1u,
          "a loan its borrower stopped waiting for is withdrawn");
    bkl_exit();
    bkl_exit();
    check(!bkl_held() && bkl_word == 0u, "the lender's last exit frees the lock");
    cpu_table[1].current = NULL;
    bkl_tally[0] = bkl_tally_boot;
    bkl_tally[1] = NULL;
}

/**
 * A stop asks every other processor: processor 1 takes the interrupt, 2
 * does not and takes the NMI, and 3 takes neither. Each that stops runs
 * the virtualization cleanup before it is marked stopped.
 */
static void row_stop(void)
{
    takes_stop[1] = 1;
    takes_nmi[2] = 1;
    waited_us = 0;
    smp_stop_others();
    check(cpu_table[1].stop_request && cpu_table[2].stop_request && cpu_table[3].stop_request &&
              !cpu_table[0].stop_request,
          "a stop asks every other processor");
    check(waited_us == 1010000ul,
          "and waits a second for those that do not take it and 10 ms after their NMIs");
    check(nmis[2] == 1 && nmis[3] == 1 && nmis[1] == 0 && nmis[0] == 0,
          "an NMI goes to each processor that did not take the stop, and to no other");
    check(cpu_table[1].stopped && cpu_table[2].stopped && !cpu_table[3].stopped,
          "the interrupt stops one processor, the NMI another, and a third takes neither");
    check(hooked[1] == 1 && hooked[2] == 1 && !hooked_late[1] && !hooked_late[2],
          "each leaves VMX operation before it is marked stopped");
    check(hooked[0] == 0 && hooked[3] == 0,
          "and neither the caller nor the stuck processor runs the hook");
}

/* What the last function `smp_call` ran saw: the processor, its argument,
 * whether interrupts were off and whether its slot still named it, and
 * how many times it ran. */
static struct {
    unsigned int cpu;
    void *arg;
    uint32_t irqs_off;
    int slot_set;
    int runs;
} called;

/** The function the call rows run: record what it sees in `called`. */
static void record_call(void *arg)
{
    called.cpu = host_cpu;
    called.arg = arg;
    called.irqs_off = irqs_off;
    called.slot_set = cpu_table[host_cpu].call_fn == record_call;
    called.runs++;
}

/**
 * `smp_call`: on the caller's own processor the function runs at once with
 * interrupts off, and they come back on after; on another processor it
 * runs in that processor's `smp_service`, whose slot still names it while
 * it runs and is clear once the wait ends; and the wait ends, with the
 * function not run, when the processor goes offline instead of answering.
 */
static void row_call(void)
{
    int arg;

    k_memset(&called, 0, sizeof(called));
    irqs_off = 0;
    smp_call(0, record_call, &arg);
    check(called.runs == 1 && called.cpu == 0 && called.arg == &arg && called.irqs_off,
          "a call to the caller's own processor runs there at once with interrupts off");
    check(!irqs_off && ipis[0] == 0,
          "and interrupts come back on after it, with no interrupt sent");

    k_memset(&called, 0, sizeof(called));
    cpu_table[2].online = 1;
    ipis[2] = 0;
    smp_call(2, record_call, &arg);
    check(called.runs == 1 && called.cpu == 2 && called.arg == &arg && called.slot_set &&
              ipis[2] == 1,
          "a call to another processor runs in its smp_service, its slot set until it returns");
    check(cpu_table[2].call_fn == NULL && !irqs_off && !ipi_lock.locked,
          "and the slot is clear when the wait ends, interrupts on and the lock let go");

    k_memset(&called, 0, sizeof(called));
    cpu_table[3].online = 1;
    goes_offline[3] = 1;
    ipis[3] = 0;
    smp_call(3, record_call, &arg);
    check(called.runs == 0 && ipis[3] == 1 && !irqs_off && !ipi_lock.locked,
          "the wait for a processor that goes offline ends, the function not run there");
    goes_offline[3] = 0;
    cpu_table[3].call_fn = NULL;
    cpu_table[2].online = 0;
}

/** Take `lock`: one thread, so nothing waits. */
void spin_lock(struct spinlock *lock)
{
    lock->locked = 1;
}

/** Give `lock` back. */
void spin_unlock(struct spinlock *lock)
{
    lock->locked = 0;
}

/** Take `lock` with interrupts off: one thread, so nothing waits. */
uint32_t spin_lock_irqsave(struct spinlock *lock)
{
    lock->locked = 1;
    return kirq_save();
}

/** Give `lock` back and put interrupts back as `flags` says. */
void spin_unlock_irqrestore(struct spinlock *lock, uint32_t flags)
{
    lock->locked = 0;
    kirq_restore(flags);
}

/** Copy `n` bytes from `src`: the test's user memory is its own. */
int copy_from_user(void *dst, const void __user *src, size_t n)
{
    memcpy(dst, (const void *)src, n);
    return 0;
}

/** Run every row; 0 when each passes, 1 otherwise. */
int main(void)
{
    _kernel_dir = KERNEL_DIR;
    user_a.space_root = 0x1000;
    user_a.page_dir = DIR_A;
    user_b.space_root = 0x2000;
    user_b.page_dir = DIR_B;
    self.space_root = 0x1000;
    self.page_dir = DIR_A;
    cpus_online = 4;
    place(0, DIR_A, &self);
    place(1, DIR_A, &user_a);
    place(2, DIR_A, &idle2);
    place(3, DIR_B, &user_b);

    smp_tlb_shootdown(DIR_A, 0x400000u, 0x401000u);
    check(ipis[1] == 1 && ipi_page[1] == 0x400000u && !cpu_table[1].space_stale,
          "a processor that runs a thread of the directory is interrupted to drop the page");
    check(ipis[2] == 0 && cpu_table[2].space_stale,
          "an idle processor that kept the directory is not interrupted and has it marked stale");
    check(ipis[3] == 0 && !cpu_table[3].space_stale,
          "a processor in another directory is left alone");
    check(ipis[0] == 0, "the caller does not interrupt itself");

    place(1, DIR_B, &user_b);
    smp_space_release(DIR_A);
    check(
        ipis[2] == 1 && cpu_table[2].space == KERNEL_DIR,
        "before a directory is freed, an idle processor that kept it gives it up for the kernel's");
    check(ipis[1] == 0 && ipis[3] == 0 && cpu_table[1].space == DIR_B,
          "a release leaves processors in other directories alone");

    row_stop();

    k_memset(cpu_table, 0, sizeof(cpu_table));
    cpus_online = 1;
    cpu_count = 2;
    waited_us = 0;
    smp_start();
    check(waited_us == CPU_ONLINE_WAIT_US && cpus_online == 1 && late_entry,
          "the boot waits for a started processor and counts none that did not come");
    if (late_entry && setjmp(stopped) == 0) {
        late_entry(1);
    }
    check(!cpu_table[1].online && cpu_table[1].admit == CPU_REFUSED && cpus_online == 1,
          "and one that comes after the wait stops, neither online nor counted");

    row_sample_stack();
    row_bkl_counts();
    row_kthread_counts();
    row_bkl_lend();
    row_call();

    while (nguarded > 0) {
        free(guarded[--nguarded]);
    }
    if (fails) {
        printf("smp-hosttest: result: fail: %d rows\n", fails);
        return 1;
    }
    printf("smp-hosttest: result: pass\n");
    return 0;
}
