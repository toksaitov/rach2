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
#include "../udelay.h"
#include "../lib/kstring.h"
#include "../mm/kheap.h"
#include "../mm/paging.h"

/* The big lock's word: 1 while a processor holds it. Which one does is in
 * the records: the one whose `bkl_depth` is above 0. */
static volatile unsigned int bkl_word;

/* Counts the changes of kernel mappings; a processor that has seen fewer
 * drops its translations when it next takes the big lock. */
static volatile uint32_t kernel_gen;

static int cpus_online = 1;

/*
 * Spin until the big lock is free and take it, answering this processor's
 * requests meanwhile, since the holder may be waiting for the answer to its
 * own. Then drop every translation if a kernel mapping changed since this
 * processor last held the lock. Interrupts are off.
 */
static void bkl_acquire(struct percpu *cpu)
{
    while (arch_atomic_xchg(&bkl_word, 1u) != 0) {
        while (bkl_word) {
            smp_service();
            arch_relax();
        }
    }
    if (cpu->kernel_gen != kernel_gen) {
        cpu->kernel_gen = kernel_gen;
        arch_space_load(arch_space_root());
    }
}

void bkl_enter(void)
{
    uint32_t flags = kirq_save();
    struct percpu *cpu = this_cpu();

    if (cpu->bkl_depth == 0) {
        bkl_acquire(cpu);
    }
    cpu->bkl_depth++;
    kirq_restore(flags);
}

void bkl_exit(void)
{
    uint32_t flags = kirq_save();
    struct percpu *cpu = this_cpu();

    if (cpu->bkl_depth <= 0) {
        k_panic("bkl: an exit from the kernel without its entry");
    }
    if (--cpu->bkl_depth == 0) {
        (void)arch_atomic_xchg(&bkl_word, 0u);
    }
    kirq_restore(flags);
}

int bkl_drop(void)
{
    struct percpu *cpu = this_cpu();
    int depth = cpu->bkl_depth;

    if (depth > 0) {
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
        cpu->bkl_depth = depth;
    }
}

int smp_cpu_count(void)
{
    return cpus_online;
}

/**
 * Answer the requests left in this processor's record.
 *
 * A stop is final. A TLB request is answered by dropping the page's
 * translation, or all of them, and then clearing the word, which is what
 * the sender waits for; the processor does not go on with a translation it
 * was asked to drop, since the sender frees the page once it sees the word
 * clear.
 */
void smp_service(void)
{
    struct percpu *cpu = this_cpu();

    if (cpu->stop_request) {
        cpu->online = 0;
        arch_stop();
    }
    if (cpu->tlb_request) {
        uintptr_t va = cpu->tlb_page;

        if (va == CPU_TLB_ALL) {
            arch_space_load(arch_space_root());
        } else {
            arch_tlb_flush_page(va);
        }
        cpu->tlb_request = 0;
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
 * Have every other processor that translates through `dir` drop `va`, and
 * wait for each. The caller holds the big lock, so a processor that runs
 * `dir` is in user mode, idle in the kernel with interrupts on, or waiting
 * for the lock and answering requests: each answers. While this one waits
 * it answers a stop itself.
 *
 * A processor whose thread has no address space, an idle task or a kernel
 * thread that kept `dir` loaded, is not interrupted. It reaches `dir`'s user
 * pages only after a switch to one of its threads, which takes the big lock
 * this one holds, so marking `dir` stale there is enough: the switch loads
 * the directory again, as Linux skips a processor in lazy TLB mode. This
 * spares an idle processor a wake for each page that `fork` or `munmap`
 * changes in a process that last ran there.
 */
void smp_tlb_shootdown(const struct page_dir *dir, uintptr_t va)
{
    int me = (int)arch_cpu_id();
    int c;

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
        other->tlb_page = va;
        other->tlb_request = 1;
        platform_cpu_ipi(c);
    }
    for (c = 0; c < NR_CPUS; c++) {
        while (cpu_table[c].tlb_request && cpu_table[c].online) {
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

void smp_stop_others(void)
{
    int me = (int)arch_cpu_id();
    int c;

    for (c = 0; c < NR_CPUS; c++) {
        if (c != me && cpu_table[c].online) {
            cpu_table[c].stop_request = 1;
            cpu_table[c].online = 0;
            platform_cpu_ipi(c);
        }
    }
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
 * enters the first program.
 */
static void smp_cpu_main(int cpu)
{
    struct percpu *self = &cpu_table[cpu];

    self->current = self->idle;
    self->space = _kernel_dir;
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
 * Make the idle tasks and start the processors.
 *
 * The boot processor's idle task is made first, since scheduling needs one
 * on every processor: a process that blocks or ends leaves for it when
 * nothing else is ready. Its stack is laid out for a first switch into
 * `sched_idle_loop`; a started processor runs its own from its start. `maxcpus=N` limits the
 * processors to N, the boot one counted, as Linux's does; 0 and 1 both leave it alone. A processor
 * that does not answer is reported, and the next is still tried.
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
        for (waited = 0; !c->online && waited < CPU_ONLINE_WAIT_US; waited += 10) {
            k_udelay(10);
        }
        if (!c->online) {
            klog(KLOG_WARN, "smp", "cpu%d: started, never online", cpu);
            continue;
        }
        cpus_online++;
    }
    klog(KLOG_INFO, "smp", "processors: %d online of %d", cpus_online, n);
}
