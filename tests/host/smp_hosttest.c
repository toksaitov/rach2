/**
 * Host test for the processors proc/smp.c interrupts to drop a page's
 * translation or give up an address space.
 *
 * host-deps: rach2/sys/lib/kstring.c rach2/sys/lib/kctypes.c
 *
 * proc/smp.c is included whole and runs as processor 0, the caller; three
 * other processors are records of `cpu_table` that a row sets up. The
 * interprocessor interrupt is the test's: it counts the interrupts each
 * processor is sent and answers each request at once, as the processor's
 * `smp_service` would, so that the caller's wait ends.
 *
 * The rows: a processor that runs a thread of the directory is interrupted
 * and asked to drop the page; an idle processor that kept the directory
 * loaded is not interrupted and has it marked stale, which its next switch
 * to a thread of the directory answers by loading it again; a processor in
 * another directory is left alone; and before the directory is freed, the
 * idle processor that kept it is interrupted and gives it up for the
 * kernel's. tests/host/smp_mutate.sh compiles a shootdown that interrupts
 * idle processors, one that skips every processor, and a release that
 * interrupts none, and requires each to fail its row. Returns 0 when every
 * row passes, 1 otherwise.
 */

#include "proc/smp.c"

#include <setjmp.h>

extern int printf(const char *format, ...);
extern void *calloc(unsigned long count, unsigned long size);
extern void free(void *ptr);

static int fails;

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
 * request, whether it takes a stop it is asked for, and the microseconds
 * waited. */
static int ipis[NR_CPUS];
static uintptr_t ipi_page[NR_CPUS];
static int takes_stop[NR_CPUS];
static unsigned long waited_us;

void platform_cpu_ipi(int cpu)
{
    struct percpu *c = &cpu_table[cpu];

    ipis[cpu]++;
    if (c->stop_request && takes_stop[cpu]) {
        c->stopped = 1;
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

/* The processors the platform has, and the entry of the one the late row
 * starts, which reaches its first C only after the boot's wait. */
static int cpu_count = 1;
static void (*late_entry)(int cpu);

int platform_cpu_count(int limit)
{
    (void)limit;
    return cpu_count;
}

int platform_cpu_start(int cpu, uintptr_t stack_top, void (*entry)(int cpu))
{
    (void)cpu;
    (void)stack_top;
    late_entry = entry;
    return 0;
}

void *kzalloc(size_t size)
{
    return calloc(1, size);
}

void kfree(void *p)
{
    free(p);
}

/* The idle tasks' stacks, which the kernel keeps for good and records only
 * by their tops, one past the end; kept here so that the test can give them
 * back at its end and LeakSanitizer does not take them for lost. */
static void *guarded[NR_CPUS * 2];
static int nguarded;

void *kalloc_guarded(uint32_t count)
{
    void *p = calloc(count, PAGE_SIZE);

    if (p && nguarded < (int)(sizeof(guarded) / sizeof(guarded[0]))) {
        guarded[nguarded++] = p;
    }
    return p;
}

int kcmdline_value(const char *key, char *out, unsigned int outlen)
{
    (void)key;
    (void)out;
    (void)outlen;
    return 0;
}

void setup_process_stack(process_t *proc, void (*entry_point)(void))
{
    (void)proc;
    (void)entry_point;
}

void sched_idle_loop(void)
{}

void k_udelay(uint32_t usec)
{
    waited_us += usec;
}

void klog(int level, const char *subsystem, const char *format, ...)
{
    (void)level;
    (void)subsystem;
    (void)format;
}

void k_panic(const char *str)
{
    printf("smp-hosttest: panic: %s\n", str);
    fails++;
}

uintptr_t get_page_dir_phys(page_dir_t *dir)
{
    (void)dir;
    return 0;
}

/* The processor's primitives, which the host has none of. */
unsigned int arch_atomic_xchg(volatile unsigned int *p, unsigned int value)
{
    unsigned int old = *p;

    *p = value;
    return old;
}

void arch_relax(void)
{}

/* Where a processor that stops goes on the host: back to the row. */
static jmp_buf stopped;

void arch_stop(void)
{
    longjmp(stopped, 1);
}

unsigned int arch_atomic_cmpxchg(volatile unsigned int *p, unsigned int old, unsigned int value)
{
    unsigned int was = *p;

    if (was == old) {
        *p = value;
    }
    return was;
}

uintptr_t arch_space_root(void)
{
    return 0;
}

void arch_space_load(uintptr_t root)
{
    (void)root;
}

void arch_tlb_flush_page(uintptr_t va)
{
    (void)va;
}

/* The global tick a started processor's accounting begins from. */
volatile uint32_t kernel_ticks;

uint32_t kirq_save(void)
{
    return 0;
}

void kirq_restore(uint32_t flags)
{
    (void)flags;
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

    smp_tlb_shootdown(DIR_A, 0x400000u);
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

    takes_stop[1] = takes_stop[3] = 1;
    waited_us = 0;
    smp_stop_others();
    check(cpu_table[1].stop_request && cpu_table[2].stop_request && cpu_table[3].stop_request &&
              !cpu_table[0].stop_request,
          "a stop asks every other processor");
    check(waited_us == 1000000ul,
          "and waits a second for the one that does not take it, none for those that do");

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
