#ifndef _PERCPU_H_
#define _PERCPU_H_

#include "../arch/arch.h"

struct process;
struct page_dir;

/* The most processors the kernel runs on. */
#define NR_CPUS 64

/* A started processor's `admit`: still starting, admitted by its own first
 * C, or refused by the boot, which gave up waiting for it (`smp_start`). */
#define CPU_STARTING 0u
#define CPU_ADMITTED 1u
#define CPU_REFUSED  2u

/* A TLB request's page that asks for every translation of user addresses
 * to be dropped. */
#define CPU_TLB_ALL ((uintptr_t)-1)

/*
 * What the shared kernel keeps for each processor: the process it runs and
 * its idle task, the page directory it translates through, its depth in the
 * big lock, whether it is started and whether it is halted in an idle wait, the
 * requests other processors left it, and the ticks that found it idle, in
 * user mode and in the kernel, which /proc/stat reports. The architecture
 * keeps a record of its own for the rest, a processor's descriptor tables
 * and stacks among them.
 *
 * `this_cpu()` is the running processor's record, the entry of `cpu_table`
 * that `arch_cpu_id()` gives, and `current_proc` is its process. A processor
 * changes its own `current`, `space` and depths and no other's, so code that
 * runs on a processor reads that processor's. Another processor reads them
 * only while it holds the big lock, which the owner holds whenever it
 * changes them, and writes only the requests (proc/smp.c).
 */
struct percpu {
    struct process *current;                    /* the process this processor runs */
    struct process *idle;                       /* what it runs when nothing else is ready */
    struct page_dir *space;                     /* the page directory it translates through */
    int bkl_depth;                              /* entries the big lock covers; >0: it holds it */
    volatile int online;                        /* started, and taking work */
    volatile unsigned int admit;                /* CPU_STARTING, ADMITTED or REFUSED */
    volatile int idle_wait;                     /* halted in an idle wait, for a tick to see */
    volatile unsigned int tlb_request;          /* drop `tlb_page`'s translation */
    volatile uintptr_t tlb_page;                /* a user page, or CPU_TLB_ALL */
    volatile unsigned int stop_request;         /* stop for good */
    volatile unsigned int stopped;              /* it took the stop: no more writes */
    const struct page_dir *volatile space_drop; /* a `space` to give up for the kernel's */
    volatile int space_stale;                   /* a shootdown passed `space` by */
    uint32_t kernel_gen;                        /* the kernel mappings it last flushed for */
    volatile uint32_t idle_ticks;               /* ticks that found it in an idle wait */
    volatile uint32_t busy_ticks;               /* ticks that found it running code */
    volatile uint32_t user_ticks;               /* of those, ticks that found it in user mode */
    uint32_t accounted_tick;                    /* the global tick its counts reach */
};

extern struct percpu cpu_table[NR_CPUS];

#define this_cpu()   (&cpu_table[arch_cpu_id()])
#define current_proc (this_cpu()->current)

#endif /* _PERCPU_H_ */
