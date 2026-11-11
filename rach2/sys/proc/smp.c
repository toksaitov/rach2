/*
 * smp.c: the big lock, the requests one processor leaves another, and the
 * start of the processors past the first, as smp.h describes them.
 */

#include "smp.h"

#include "process.h"
#include "sched.h"
#include "../cmdline.h"
#include "../klog.h"
#include "../panic.h"
#include "../platform.h"
#include "../lib/kirq.h"
#include "../timer.h"
#include "../udelay.h"
#include "../lib/kstring.h"
#include "../mm/kheap.h"
#include "../mm/paging.h"

/* The big lock's word: 1 while a processor holds it. Which one does is in
 * the records: the one whose `bkl_depth` is above 0. */
static volatile unsigned int bkl_word;

/* The processor that has waited long enough to go next, as its number plus
 * one, 0 for none (`bkl_acquire`). */
static volatile unsigned int bkl_claim;

/* Updated with the big lock held; the waits are accumulated as TSC cycles
 * so that rounding at each acquisition loses no short waits. */
static uint64_t bkl_wait_cycles;
static uint64_t bkl_wait_max_cycles;
static volatile uint32_t bkl_releases; /* completed outer holds, modulo 2^32 */
static uint32_t bkl_wait_max_holds;    /* holds completed during the longest wait */
/* Keep the most recent releases and copy those of a new longest wait. */
struct bkl_hold_record {
    uint64_t cycles; /* full hold duration in TSC cycles */
    uint32_t vector; /* entry vector, 0 for a take after a drop */
    int32_t nr;      /* entry system call number, -1 for none */
};
static struct bkl_hold_record bkl_recent[BKL_WAIT_HOLDS];
static struct bkl_hold_record bkl_wait_hold[BKL_WAIT_HOLDS];
static uint64_t bkl_hold_max_cycles;          /* the longest hold, from take to give back */
static uint32_t bkl_hold_vector;              /* its entry's vector, 0 for none */
static int32_t bkl_hold_nr = -1;              /* its entry's system call number, or -1 */
static uint64_t bkl_seen_max_cycles;          /* the oldest hold an interrupt has found */
static uintptr_t bkl_seen_pc[BKL_SAMPLE_PCS]; /* that instruction, then its callers */

/* Each processor's first sample past 1, 2, 4, 8 and 16 s into its hold under
 * way, each with its age in cycles, 0 until taken, and the longest hold's,
 * kept at its end. */
static uintptr_t bkl_early_pc[NR_CPUS][BKL_SAMPLE_MARKS][BKL_SAMPLE_PCS];
static uint64_t bkl_early_cycles[NR_CPUS][BKL_SAMPLE_MARKS];
static uintptr_t bkl_hold_early_pc[BKL_SAMPLE_MARKS][BKL_SAMPLE_PCS];
static uint64_t bkl_hold_early_cycles[BKL_SAMPLE_MARKS];

/* The holds of the big lock by the entry that took it (`smp_entry_hold`):
 * their sum in TSC cycles and their number. */
static uint64_t bkl_entry_cycles[BKL_ENTRY_KEYS];
static uint32_t bkl_entry_holds[BKL_ENTRY_KEYS];

/* Set once the first hold, the boot processor's start-up, has ended. */
static int bkl_booted;

/* The spins after which a waiter claims the big lock, and the times a waiter
 * sees the lock free and unclaimed by it before it passes over the claim. The
 * second covers a claimant that does not run, since under a hypervisor a
 * virtual processor can be off the machine. */
#define BKL_CLAIM_AFTER 4096u
#define BKL_PASS_AFTER  (1u << 16)

/* Counts the changes of kernel mappings; a processor that has seen fewer
 * drops its translations when it next takes the big lock. */
static volatile uint32_t kernel_gen;

/* The processors online, the boot processor counted (`smp_cpu_count`). */
static int cpus_online = 1;

/* The shootdowns that interrupted a processor, and the interrupts they sent
 * (`smp_tlb_count`); written under the big lock. */
static uint32_t tlb_counts[2];

/** Drop every translation of `cpu`, which has just taken the big lock, if a
 * kernel mapping changed since it last held it. */
static void bkl_flush(struct percpu *cpu)
{
    if (cpu->kernel_gen != kernel_gen) {
        cpu->kernel_gen = kernel_gen;
        arch_space_load(arch_space_root());
    }
}

/**
 * Spin until the big lock is free and take it, answering this processor's
 * requests meanwhile, since the holder may be waiting for the answer to its
 * own. Then drop every translation if a kernel mapping changed since this
 * processor last held the lock. Interrupts are off.
 *
 * An exchange alone favours the processor that last had the lock's cache
 * line, such as the holder's hyperthread, and can pass over the others for
 * seconds, while a process queued on a spinning processor waits as long. So a
 * waiter past `BKL_CLAIM_AFTER` spins claims the lock, and the others leave it
 * to the claimant.
 */
static void bkl_acquire(struct percpu *cpu)
{
    unsigned int me = (unsigned int)arch_cpu_id() + 1u;
    uint32_t spins = 0, passed = 0;
    uint32_t releases = bkl_releases;
    uint64_t start = arch_cycles(), waited;
    uint32_t wake_start = waketime_clock(), wake_wait;
    unsigned int i, n;

    cpu->wake_bkl_start = wake_start;
    cpu->wake_bkl_waiting = 1;
    for (;;) {
        unsigned int claim = bkl_claim;

        if (!bkl_word && (claim == 0 || claim == me || ++passed >= BKL_PASS_AFTER) &&
            arch_atomic_xchg(&bkl_word, 1u) == 0) {
            break;
        }
        smp_service();
        arch_relax();
        if (spins < BKL_CLAIM_AFTER) {
            spins++;
        } else if (claim == 0) {
            (void)arch_atomic_cmpxchg(&bkl_claim, 0u, me);
        }
    }
    if (bkl_claim == me) {
        (void)arch_atomic_cmpxchg(&bkl_claim, me, 0u);
    }
    cpu->bkl_since = arch_cycles();
    wake_wait = waketime_clock() - wake_start;
    cpu->wake_bkl_total += wake_wait;
    cpu->wake_bkl_waiting = 0;
    waketime_lock(wake_wait);
    k_memset(bkl_early_cycles[arch_cpu_id()], 0, sizeof(bkl_early_cycles[0]));
    waited = cpu->bkl_since - start;
    bkl_wait_cycles += waited;
    if (waited > bkl_wait_max_cycles) {
        bkl_wait_max_cycles = waited;
        bkl_wait_max_holds = bkl_releases - releases;
        n = bkl_wait_max_holds < BKL_WAIT_HOLDS ? bkl_wait_max_holds : BKL_WAIT_HOLDS;
        for (i = 0; i < n; i++) {
            bkl_wait_hold[i] = bkl_recent[(bkl_releases - n + i) % BKL_WAIT_HOLDS];
        }
    }
    bkl_flush(cpu);
}

/** Count the hold that ends now on `cpu`, which still holds the lock, under
 * its entry and against the longest. The first hold to end, the boot
 * processor's start-up, which nobody waits on, is left out. */
static void bkl_hold_end(struct percpu *cpu)
{
    uint64_t held = arch_cycles() - cpu->bkl_since;
    int c = arch_cpu_id();
    int key;

    bkl_recent[bkl_releases % BKL_WAIT_HOLDS].cycles = held;
    bkl_recent[bkl_releases % BKL_WAIT_HOLDS].vector = cpu->bkl_vector;
    bkl_recent[bkl_releases % BKL_WAIT_HOLDS].nr = cpu->bkl_nr;
    bkl_releases++;
    if (!bkl_booted) {
        bkl_booted = 1;
        return;
    }
    if (cpu->bkl_nr >= 0) {
        key = cpu->bkl_nr < BKL_ENTRY_SYSCALLS ? cpu->bkl_nr : BKL_ENTRY_KEYS - 1;
    } else {
        key = BKL_ENTRY_SYSCALLS + (int)(cpu->bkl_vector & 0xffu);
    }
    bkl_entry_cycles[key] += held;
    bkl_entry_holds[key]++;
    if (held > bkl_hold_max_cycles) {
        bkl_hold_max_cycles = held;
        bkl_hold_vector = cpu->bkl_vector;
        bkl_hold_nr = cpu->bkl_nr;
        k_memcpy(bkl_hold_early_cycles, bkl_early_cycles[c], sizeof(bkl_hold_early_cycles));
        k_memcpy(bkl_hold_early_pc, bkl_early_pc[c], sizeof(bkl_hold_early_pc));
    }
}

void bkl_note_entry(uint32_t vector, int32_t nr)
{
    struct percpu *cpu = this_cpu();

    if (cpu->bkl_depth == 1) {
        cpu->bkl_vector = vector;
        cpu->bkl_nr = nr;
    }
}

/** Fill `out` with `pc` and the return addresses of the frames from `fp`,
 * 0 past the last. The kernel keeps frame pointers: each frame holds the
 * caller's frame pointer, then the return address. The walk reads only the
 * interrupted thread's kernel stack, the KERNEL_STACK_SIZE bytes below
 * `top`, and only upward, since the outermost kernel frame holds whatever
 * the user left in the frame pointer; a `top` of 0 records `pc` alone. */
static void sample_chain(uintptr_t out[BKL_SAMPLE_PCS], uintptr_t pc, uintptr_t fp, uintptr_t top)
{
    uintptr_t low = fp;
    int i;

    out[0] = pc;
    for (i = 1; i < BKL_SAMPLE_PCS; i++) {
        if (top < KERNEL_STACK_SIZE || fp < top - KERNEL_STACK_SIZE ||
            fp > top - 2 * sizeof(uintptr_t) || fp % sizeof(uintptr_t) != 0 || fp < low) {
            break;
        }
        out[i] = ((const uintptr_t *)fp)[1];
        low = fp + 2 * sizeof(uintptr_t);
        fp = ((const uintptr_t *)fp)[0];
    }
    for (; i < BKL_SAMPLE_PCS; i++) {
        out[i] = 0;
    }
}

void bkl_hold_sample(uintptr_t pc, uintptr_t fp)
{
    struct percpu *cpu = this_cpu();
    int c = arch_cpu_id(), m;
    uintptr_t top;
    uint64_t held;

    if (cpu->bkl_depth < 2) {
        return;
    }
    held = arch_cycles() - cpu->bkl_since;
    top = cpu->current ? cpu->current->kernel_stack : 0;
    for (m = 0; m < BKL_SAMPLE_MARKS; m++) {
        if (!bkl_early_cycles[c][m] && timer_cycles_us(held) >= 1000000u << m) {
            bkl_early_cycles[c][m] = held;
            sample_chain(bkl_early_pc[c][m], pc, fp, top);
        }
    }
    if (held > bkl_seen_max_cycles) {
        bkl_seen_max_cycles = held;
        sample_chain(bkl_seen_pc, pc, fp, top);
    }
}

/** Read the acquisition and shootdown counters with the big lock held. */
void smp_wait_stats(uint64_t *total_us, uint64_t *max_us, uint32_t *holds)
{
    *total_us = timer_cycles_us(bkl_wait_cycles);
    *max_us = timer_cycles_us(bkl_wait_max_cycles);
    *holds = bkl_wait_max_holds;
}

/** Read a retained hold from the longest wait, with the big lock held. */
void smp_wait_hold(unsigned int index, uint64_t *held_us, uint32_t *vector, int32_t *nr)
{
    *held_us = timer_cycles_us(bkl_wait_hold[index].cycles);
    *vector = bkl_wait_hold[index].vector;
    *nr = bkl_wait_hold[index].nr;
}

void smp_hold_stats(uint64_t *max_us, uint32_t *vector, int32_t *nr)
{
    *max_us = timer_cycles_us(bkl_hold_max_cycles);
    *vector = bkl_hold_vector;
    *nr = bkl_hold_nr;
}

void smp_hold_sample(uint64_t *age_us, uintptr_t pc[BKL_SAMPLE_PCS])
{
    int i;

    *age_us = timer_cycles_us(bkl_seen_max_cycles);
    for (i = 0; i < BKL_SAMPLE_PCS; i++) {
        pc[i] = bkl_seen_pc[i];
    }
}

void smp_entry_hold(int key, uint64_t *held_us, uint32_t *holds)
{
    *held_us = timer_cycles_us(bkl_entry_cycles[key]);
    *holds = bkl_entry_holds[key];
}

void smp_hold_early(int mark, uint64_t *age_us, uintptr_t pc[BKL_SAMPLE_PCS])
{
    int i;

    *age_us = timer_cycles_us(bkl_hold_early_cycles[mark]);
    for (i = 0; i < BKL_SAMPLE_PCS; i++) {
        pc[i] = bkl_hold_early_cycles[mark] ? bkl_hold_early_pc[mark][i] : 0;
    }
}

void bkl_enter(void)
{
    uint32_t flags = kirq_save();
    struct percpu *cpu = this_cpu();

    if (cpu->bkl_depth == 0) {
        bkl_acquire(cpu);
        cpu->bkl_vector = 0;
        cpu->bkl_nr = -1;
    }
    cpu->bkl_depth++;
    kirq_restore(flags);
}

int bkl_try_enter(void)
{
    struct percpu *cpu = this_cpu();

    if (cpu->bkl_depth == 0) {
        if (bkl_word || bkl_claim || arch_atomic_xchg(&bkl_word, 1u) != 0) {
            return 0;
        }
        cpu->bkl_since = arch_cycles();
        k_memset(bkl_early_cycles[arch_cpu_id()], 0, sizeof(bkl_early_cycles[0]));
        cpu->bkl_vector = 0;
        cpu->bkl_nr = -1;
        bkl_flush(cpu);
    }
    cpu->bkl_depth++;
    return 1;
}

void bkl_exit(void)
{
    uint32_t flags = kirq_save();
    struct percpu *cpu = this_cpu();

    if (cpu->bkl_depth <= 0) {
        k_panic("bkl: an exit from the kernel without its entry");
    }
    if (--cpu->bkl_depth == 0) {
        bkl_hold_end(cpu);
        (void)arch_atomic_xchg(&bkl_word, 0u);
    }
    kirq_restore(flags);
}

int bkl_drop(void)
{
    struct percpu *cpu = this_cpu();
    int depth = cpu->bkl_depth;

    if (depth > 0) {
        bkl_hold_end(cpu);
        cpu->bkl_depth = 0;
        (void)arch_atomic_xchg(&bkl_word, 0u);
    }
    return depth;
}

void bkl_take(int depth)
{
    struct percpu *cpu = this_cpu();

    if (depth > 0) {
        bkl_acquire(cpu);
        cpu->bkl_vector = 0;
        cpu->bkl_nr = -1;
        cpu->bkl_depth = depth;
    }
}

void bkl_yield(void)
{
    struct percpu *cpu = this_cpu();
    unsigned int claim = bkl_claim;

    if (cpu->bkl_depth > 0 && claim != 0 && claim != (unsigned int)arch_cpu_id() + 1u) {
        bkl_take(bkl_drop());
    }
}

int smp_cpu_count(void)
{
    return cpus_online;
}

/**
 * A stop is final. A TLB request is answered by dropping the page's
 * translation, or all of them, and then clearing the word, which is what
 * the sender waits for; the processor does not go on with a translation it
 * was asked to drop, since the sender frees the page once it sees the word
 * clear. A barrier request is answered by the locked exchange that clears
 * it, a full barrier, after a serializing instruction when it asks for one.
 */
void smp_service(void)
{
    struct percpu *cpu = this_cpu();

    if (cpu->stop_request) {
        cpu->online = 0;
        cpu->stopped = 1;
        arch_stop();
    }
    if (cpu->tlb_request) {
        uintptr_t va = cpu->tlb_page;

        if (va == CPU_TLB_ALL) {
            arch_space_load(arch_space_root());
        } else {
            for (; va < cpu->tlb_end; va += PAGE_SIZE) {
                arch_tlb_flush_page(va);
            }
        }
        cpu->tlb_request = 0;
    }
    if (cpu->barrier_request) {
        if (cpu->barrier_request == SMP_BARRIER_SYNC) {
            arch_sync_core();
        }
        (void)arch_atomic_xchg(&cpu->barrier_request, 0);
    }
    if (cpu->space_drop) {
        if (cpu->space == cpu->space_drop) {
            cpu->space = _kernel_dir;
            arch_space_load(get_page_dir_phys(_kernel_dir));
        }
        cpu->space_drop = NULL;
    }
}

void smp_ipi(void)
{
    smp_service();
}

void smp_kick(int cpu)
{
    if (cpu != (int)arch_cpu_id() && cpu_table[cpu].online) {
        platform_cpu_ipi(cpu);
    }
}

/**
 * The caller holds the big lock, so a processor that runs `dir` is in user
 * mode, idle in the kernel with interrupts on, or waiting for the lock and
 * answering requests: each answers. While this one waits it answers its own
 * requests, a stop among them.
 *
 * A processor whose thread has no address space, an idle task or a kernel
 * thread that kept `dir` loaded, is not interrupted. It reaches `dir`'s user
 * pages only after a switch to one of its threads, which takes the big lock
 * this one holds, so marking `dir` stale there is enough: the switch loads
 * the directory again, as Linux skips a processor in lazy TLB mode. This
 * spares an idle processor a wake for each page that `fork` or `munmap`
 * changes in a process that last ran there.
 */
void smp_tlb_shootdown(const struct page_dir *dir, uintptr_t start, uintptr_t end)
{
    int me = (int)arch_cpu_id();
    int c, sent = 0;

    if (cpus_online < 2) {
        return;
    }
    for (c = 0; c < NR_CPUS; c++) {
        struct percpu *other = &cpu_table[c];

        if (c == me || !other->online || other->space != dir) {
            continue;
        }
        if (other->current->space_root == 0) {
            other->space_stale = 1;
            continue;
        }
        other->tlb_page = start;
        other->tlb_end = end;
        other->tlb_request = 1;
        platform_cpu_ipi(c);
        sent++;
    }
    if (sent) {
        tlb_counts[0]++;
        tlb_counts[1] += (uint32_t)sent;
    }
    for (c = 0; c < NR_CPUS; c++) {
        while (cpu_table[c].tlb_request && cpu_table[c].online) {
            smp_service();
            arch_relax();
        }
    }
}

uint32_t smp_tlb_count(int which)
{
    return which == 0 || which == 1 ? tlb_counts[which] : 0;
}

void smp_barrier(uint64_t cpus, int sync_core)
{
    int me = (int)arch_cpu_id();
    int c;

    for (c = 0; c < NR_CPUS; c++) {
        if (c != me && (cpus & ((uint64_t)1 << c)) && cpu_table[c].online) {
            cpu_table[c].barrier_request = sync_core ? SMP_BARRIER_SYNC : SMP_BARRIER;
            platform_cpu_ipi(c);
        }
    }
    for (c = 0; c < NR_CPUS; c++) {
        while (cpu_table[c].barrier_request && cpu_table[c].online) {
            smp_service();
            arch_relax();
        }
    }
}

void smp_space_release(const struct page_dir *dir)
{
    int me = (int)arch_cpu_id();
    int c;

    if (cpus_online < 2) {
        return;
    }
    for (c = 0; c < NR_CPUS; c++) {
        if (c != me && cpu_table[c].online && cpu_table[c].space == dir) {
            cpu_table[c].space_drop = dir;
            platform_cpu_ipi(c);
        }
    }
    for (c = 0; c < NR_CPUS; c++) {
        while (cpu_table[c].space_drop && cpu_table[c].online) {
            smp_service();
            arch_relax();
        }
    }
}

void smp_kernel_mapping_changed(void)
{
    this_cpu()->kernel_gen = ++kernel_gen;
}

/* How long a stop waits for the processors asked to take it by the
 * interprocessor interrupt, which one with its interrupts off takes when it
 * next turns them on or spins in a lock; and then for those sent the NMI,
 * as Linux's `native_stop_other_cpus` waits (arch/x86/kernel/smp.c). */
#define STOP_WAIT_US     1000000u
#define STOP_NMI_WAIT_US 10000u

/** Whether a processor of those `asked` marks has not yet stopped. */
static int stop_pending(const int *asked)
{
    int c;

    for (c = 0; c < NR_CPUS; c++) {
        if (asked[c] && !cpu_table[c].stopped) {
            return 1;
        }
    }
    return 0;
}

/**
 * The stop is Linux's: the interrupt to every other processor, a second's
 * wait for all, then a non-maskable interrupt to each still running, which
 * reaches one spinning with its interrupts off (`smp_stop_nmi`), and 10 ms
 * more; a processor stuck past both, as one whose APIC no longer answers, is
 * left. Nothing is logged: a processor that did not stop may hold the log's
 * lock.
 */
void smp_stop_others(void)
{
    int me = (int)arch_cpu_id();
    int c, asked[NR_CPUS];
    uint32_t waited;

    for (c = 0; c < NR_CPUS; c++) {
        asked[c] = c != me && cpu_table[c].online;
        if (asked[c]) {
            cpu_table[c].stop_request = 1;
            cpu_table[c].online = 0;
            platform_cpu_ipi(c);
        }
    }
    for (waited = 0; stop_pending(asked) && waited < STOP_WAIT_US; waited += 10u) {
        k_udelay(10);
    }
    if (!stop_pending(asked)) {
        return;
    }
    for (c = 0; c < NR_CPUS; c++) {
        if (asked[c] && !cpu_table[c].stopped) {
            platform_cpu_nmi(c);
        }
    }
    for (waited = 0; stop_pending(asked) && waited < STOP_NMI_WAIT_US; waited += 10u) {
        k_udelay(10);
    }
}

int smp_stop_nmi(void)
{
    struct percpu *cpu = this_cpu();

    if (!cpu->stop_request) {
        return 0;
    }
    cpu->online = 0;
    cpu->stopped = 1;
    arch_stop();
    return 1;
}

/**
 * The idle task of processor `cpu`, or NULL when the heap has no room.
 *
 * The scheduler runs it when nothing else may run there (sched.c). It is
 * off the process list, so no call names it and no reaper frees it, and it
 * is never freed. It has a kernel stack of its own with a guard below it,
 * the kernel's page directory, number 0 and the credentials of root, and
 * process 0's thread group, file system record and descriptor table, so
 * that code which reads the current process's finds them. It runs only on
 * `cpu`, and its floating-point area is never used: the switch leaves the
 * registers alone around it.
 */
static process_t *smp_idle_task(int cpu)
{
    process_t *p = kzalloc(sizeof(*p));
    void *stack;

    if (!p) {
        return NULL;
    }
    stack = kalloc_guarded(KERNEL_STACK_SIZE / PAGE_SIZE);
    if (!stack) {
        kfree(p);
        return NULL;
    }
    k_memset(stack, 0, KERNEL_STACK_SIZE);
    p->kernel_stack = (uintptr_t)stack + KERNEL_STACK_SIZE;
    p->state = PROC_RUNNING;
    k_memcpy(p->comm, "idle", 5);
    if (proc_idle) {
        p->tg = proc_idle->tg;
        p->sigacts = proc_idle->sigacts;
        p->fs = proc_idle->fs;
        p->fds = proc_idle->fds;
        p->userns = proc_idle->userns;
        p->pidns = proc_idle->pidns;
        p->nsset = proc_idle->nsset;
        k_memcpy(p->rlim, proc_idle->rlim, sizeof(p->rlim));
    }
    p->waiter.proc = p;
    p->cpu = cpu;
    p->affinity = (uint64_t)1 << cpu;
    p->bkl_depth = 1;
    return p;
}

/**
 * A started processor's first C, on its idle task's stack with interrupts
 * off: it is online at once, so that the boot processor goes on to the next,
 * and takes work once it has the big lock, which the boot holds until it
 * enters the first program. One that comes after the boot gave up waiting
 * for it finds its `admit` CPU_REFUSED (`smp_start`) and stops: online and
 * uncounted, it would miss the shootdowns, which are skipped while one
 * processor is counted.
 */
static void smp_cpu_main(int cpu)
{
    struct percpu *self = &cpu_table[cpu];

    self->current = self->idle;
    self->space = _kernel_dir;
    if (arch_atomic_cmpxchg(&self->admit, CPU_STARTING, CPU_ADMITTED) != CPU_STARTING) {
        arch_stop();
    }
    self->accounted_tick = (uint32_t)kernel_ticks;
    self->online = 1;
    bkl_take(1);
    sched_idle_loop();
}

/** The decimal number `text` begins with, at most `cap`; 0 for none. */
static int parse_count(const char *text, int cap)
{
    int n = 0;

    while (*text >= '0' && *text <= '9') {
        n = n * 10 + (*text++ - '0');
        if (n > cap) {
            return cap;
        }
    }
    return n;
}

/* How long the boot waits for a started processor to be online, in
 * microseconds, after the platform has seen it reach its entry. */
#define CPU_ONLINE_WAIT_US 100000

/**
 * The boot processor's idle task is made first, since scheduling needs one on
 * every processor: a process that blocks or ends leaves for it when nothing
 * else is ready. Its stack is laid out for a first switch into
 * `sched_idle_loop`; a started processor runs its own from its start.
 * `maxcpus=N` limits the processors to N, as Linux's does; 0 and 1 both mean
 * the boot processor alone. A processor that does not answer is reported,
 * and the next is still tried. The boot admits a processor or refuses it in
 * one compare-and-exchange of its `admit`, which the processor makes too as
 * it comes online, so that one coming as the wait ends is either counted or
 * stopped, never online and uncounted.
 */
void smp_start(void)
{
    char value[12];
    int limit = NR_CPUS;
    int n, cpu, waited;

    cpu_table[0].idle = smp_idle_task(0);
    if (!cpu_table[0].idle) {
        k_panic("smp: no idle task for the boot processor");
    }
    setup_process_stack(cpu_table[0].idle, sched_idle_loop);
    cpu_table[0].online = 1;
    if (kcmdline_value("maxcpus", value, sizeof(value))) {
        limit = parse_count(value, NR_CPUS);
        if (limit < 1) {
            limit = 1;
        }
    }
    n = platform_cpu_count(limit);
    for (cpu = 1; cpu < n; cpu++) {
        struct percpu *c = &cpu_table[cpu];

        c->idle = smp_idle_task(cpu);
        if (!c->idle) {
            klog(KLOG_WARN, "smp", "cpu%d: no idle task; not started", cpu);
            break;
        }
        if (platform_cpu_start(cpu, c->idle->kernel_stack, smp_cpu_main) != 0) {
            klog(KLOG_WARN, "smp", "cpu%d: did not start", cpu);
            continue;
        }
        for (waited = 0; c->admit == CPU_STARTING && waited < CPU_ONLINE_WAIT_US; waited += 10) {
            k_udelay(10);
        }
        if (arch_atomic_cmpxchg(&c->admit, CPU_STARTING, CPU_REFUSED) != CPU_ADMITTED) {
            klog(KLOG_WARN, "smp", "cpu%d: started, never online", cpu);
            continue;
        }
        cpus_online++;
    }
    klog(KLOG_INFO, "smp", "processors: %d online of %d", cpus_online, n);
}
