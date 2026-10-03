#ifndef _PERCPU_H_
#define _PERCPU_H_

#include "../arch/arch.h"

struct process;

/* The most processors the kernel runs on. */
#define NR_CPUS 64

/*
 * What the shared kernel keeps for each processor: the process it runs,
 * whether it is halted in an idle wait, and the ticks that found it idle and
 * busy, which /proc/stat reports. The architecture keeps a record of its own
 * for the rest, a processor's descriptor tables and stacks among them.
 *
 * `this_cpu()` is the running processor's record, the entry of `cpu_table`
 * that `arch_cpu_id()` gives, and `current_proc` is its process. A processor
 * changes its own `current` and no other's, at a switch (proc/sched.c), so
 * code that runs on a processor reads that processor's process.
 */
struct percpu {
    struct process *current;      /* the process this processor runs */
    volatile int idle_wait;       /* halted in an idle wait, for a tick to see */
    volatile uint32_t idle_ticks; /* ticks that found it in an idle wait */
    volatile uint32_t busy_ticks; /* ticks that found it running code */
};

extern struct percpu cpu_table[NR_CPUS];

#define this_cpu()   (&cpu_table[arch_cpu_id()])
#define current_proc (this_cpu()->current)

#endif /* _PERCPU_H_ */
