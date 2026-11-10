/*
 * membarrier.c: membarrier(2), a barrier on the processors that run the
 * threads of the caller's process or of every process, as Linux 6.12's
 * kernel/sched/membarrier.c gives it on x86.
 */
#include "membarrier.h"

#include "percpu.h"
#include "process.h"
#include "rseq.h"
#include "smp.h"
#include "../arch/arch.h"
#include "../lib/kerrno.h"
#include "../mm/mm.h"

/* The commands and their flag, Linux's values. */
#define MEMBARRIER_CMD_QUERY                                0
#define MEMBARRIER_CMD_GLOBAL                               (1 << 0)
#define MEMBARRIER_CMD_GLOBAL_EXPEDITED                     (1 << 1)
#define MEMBARRIER_CMD_REGISTER_GLOBAL_EXPEDITED            (1 << 2)
#define MEMBARRIER_CMD_PRIVATE_EXPEDITED                    (1 << 3)
#define MEMBARRIER_CMD_REGISTER_PRIVATE_EXPEDITED           (1 << 4)
#define MEMBARRIER_CMD_PRIVATE_EXPEDITED_SYNC_CORE          (1 << 5)
#define MEMBARRIER_CMD_REGISTER_PRIVATE_EXPEDITED_SYNC_CORE (1 << 6)
#define MEMBARRIER_CMD_PRIVATE_EXPEDITED_RSEQ               (1 << 7)
#define MEMBARRIER_CMD_REGISTER_PRIVATE_EXPEDITED_RSEQ      (1 << 8)
#define MEMBARRIER_CMD_GET_REGISTRATIONS                    (1 << 9)
#define MEMBARRIER_CMD_BITMASK                              0x3FF
#define MEMBARRIER_CMD_FLAG_CPU                             1u

/* The three kinds of private barrier. */
#define MB_PLAIN     0
#define MB_SYNC_CORE 1
#define MB_RSEQ      2

/** A full memory barrier on this processor: a locked exchange, which no
 * load or store crosses. */
static void mb_here(void)
{
    volatile unsigned int word = 0;

    (void)arch_atomic_xchg(&word, 0);
}

/**
 * The processors started whose current thread has an address space, `mm`
 * when it is not NULL, or one registered for the global barrier with
 * `global`, a bit for each; the caller holds the big lock, so none of them
 * switches while it reads.
 */
static uint64_t mb_running(const struct mm *mm, int global)
{
    uint64_t mask = 0;
    int c;

    for (c = 0; c < NR_CPUS; c++) {
        const process_t *p = cpu_table[c].current;

        if (!cpu_table[c].online || !p || !p->mm) {
            continue;
        }
        if ((mm && p->mm != mm) ||
            (global && !(p->mm->membarrier & MEMBARRIER_STATE_GLOBAL_EXPEDITED))) {
            continue;
        }
        mask |= (uint64_t)1 << c;
    }
    return mask;
}

/**
 * A private barrier of kind `kind` on the processors that run the caller's
 * address space, or on processor `cpu_id` alone when it is not negative, as
 * Linux's membarrier_private_expedited: -EPERM before the kind's
 * registration; nothing to do but the caller's own barrier when its address
 * space has one user or one processor is started, but for SYNC_CORE; a
 * processor not started, or not running the address space, is skipped.
 * The caller's processor is left to its own barrier, but for SYNC_CORE,
 * which serializes it, and for RSEQ named by `cpu_id`, which marks the
 * caller's own thread. The RSEQ kind aborts the critical section of each
 * thread it reaches when it next returns to user mode (`rseq_preempt`).
 */
static long mb_private(int kind, int cpu_id)
{
    struct mm *mm = current_proc->mm;
    uint32_t ready = kind == MB_SYNC_CORE ? MEMBARRIER_STATE_PRIVATE_EXPEDITED_SYNC_CORE_READY
                     : kind == MB_RSEQ    ? MEMBARRIER_STATE_PRIVATE_EXPEDITED_RSEQ_READY
                                          : MEMBARRIER_STATE_PRIVATE_EXPEDITED_READY;
    uint64_t me = (uint64_t)1 << arch_cpu_id();
    uint64_t mask;
    int c;

    if (!(mm->membarrier & ready)) {
        return -EPERM;
    }
    if (kind != MB_SYNC_CORE && (mm->users == 1 || smp_cpu_count() == 1)) {
        return 0;
    }
    mb_here();
    if (cpu_id >= 0) {
        if (cpu_id >= NR_CPUS || !cpu_table[cpu_id].online || !cpu_table[cpu_id].current ||
            cpu_table[cpu_id].current->mm != mm) {
            mb_here();
            return 0;
        }
        mask = (uint64_t)1 << cpu_id;
    } else {
        mask = mb_running(mm, 0);
        if (kind != MB_SYNC_CORE) {
            mask &= ~me;
        }
    }
    if (kind == MB_RSEQ) {
        for (c = 0; c < NR_CPUS; c++) {
            if (mask & ((uint64_t)1 << c)) {
                rseq_preempt(cpu_table[c].current);
            }
        }
    }
    smp_barrier(mask & ~me, kind == MB_SYNC_CORE);
    if ((mask & me) && kind == MB_SYNC_CORE) {
        arch_sync_core();
    }
    mb_here();
    return 0;
}

/** Register the caller's address space for the private barrier of kind
 * `kind`, as Linux's membarrier_register_private_expedited: the kind's
 * bits, and PRIVATE_EXPEDITED's without its _READY for the other kinds. */
static long mb_register_private(int kind)
{
    struct mm *mm = current_proc->mm;
    uint32_t set = MEMBARRIER_STATE_PRIVATE_EXPEDITED;
    uint32_t ready = MEMBARRIER_STATE_PRIVATE_EXPEDITED_READY;

    if (kind == MB_SYNC_CORE) {
        set |= MEMBARRIER_STATE_PRIVATE_EXPEDITED_SYNC_CORE;
        ready = MEMBARRIER_STATE_PRIVATE_EXPEDITED_SYNC_CORE_READY;
    } else if (kind == MB_RSEQ) {
        set |= MEMBARRIER_STATE_PRIVATE_EXPEDITED_RSEQ;
        ready = MEMBARRIER_STATE_PRIVATE_EXPEDITED_RSEQ_READY;
    }
    mm->membarrier |= set | ready;
    return 0;
}

/** The registration commands the caller's address space has made, as
 * Linux's membarrier_get_registrations reads them from its state. */
static long mb_registrations(void)
{
    static const uint32_t states[4] = {
        MEMBARRIER_STATE_GLOBAL_EXPEDITED | MEMBARRIER_STATE_GLOBAL_EXPEDITED_READY,
        MEMBARRIER_STATE_PRIVATE_EXPEDITED | MEMBARRIER_STATE_PRIVATE_EXPEDITED_READY,
        MEMBARRIER_STATE_PRIVATE_EXPEDITED_SYNC_CORE |
            MEMBARRIER_STATE_PRIVATE_EXPEDITED_SYNC_CORE_READY,
        MEMBARRIER_STATE_PRIVATE_EXPEDITED_RSEQ | MEMBARRIER_STATE_PRIVATE_EXPEDITED_RSEQ_READY};
    static const long cmds[4] = {MEMBARRIER_CMD_REGISTER_GLOBAL_EXPEDITED,
                                 MEMBARRIER_CMD_REGISTER_PRIVATE_EXPEDITED,
                                 MEMBARRIER_CMD_REGISTER_PRIVATE_EXPEDITED_SYNC_CORE,
                                 MEMBARRIER_CMD_REGISTER_PRIVATE_EXPEDITED_RSEQ};
    uint32_t state = current_proc->mm->membarrier;
    long mask = 0;
    int i;

    for (i = 0; i < 4; i++) {
        if (state & states[i]) {
            mask |= cmds[i];
        }
    }
    return mask;
}

/**
 * GLOBAL waits for every processor running a thread to pass a barrier, as
 * Linux's synchronize_rcu waits for each to pass a quiescent state; a
 * processor in the kernel or idle passes one when it next takes the big
 * lock, which the caller holds. GLOBAL_EXPEDITED reaches only the
 * processors running a registered address space.
 */
long sys_membarrier(isr_params_t *params)
{
    int cmd = (int)SC_ARG(params, 0);
    uint32_t flags = (uint32_t)SC_ARG(params, 1);
    int cpu_id = (int)SC_ARG(params, 2);
    uint64_t me = (uint64_t)1 << arch_cpu_id();

    if (cmd == MEMBARRIER_CMD_PRIVATE_EXPEDITED_RSEQ) {
        if (flags != 0 && flags != MEMBARRIER_CMD_FLAG_CPU) {
            return -EINVAL;
        }
    } else if (flags != 0) {
        return -EINVAL;
    }
    if (!(flags & MEMBARRIER_CMD_FLAG_CPU)) {
        cpu_id = -1;
    }
    switch (cmd) {
    case MEMBARRIER_CMD_QUERY:
        return MEMBARRIER_CMD_BITMASK;
    case MEMBARRIER_CMD_GLOBAL:
    case MEMBARRIER_CMD_GLOBAL_EXPEDITED:
        if (smp_cpu_count() > 1) {
            mb_here();
            smp_barrier(mb_running(NULL, cmd == MEMBARRIER_CMD_GLOBAL_EXPEDITED) & ~me, 0);
            mb_here();
        }
        return 0;
    case MEMBARRIER_CMD_REGISTER_GLOBAL_EXPEDITED:
        current_proc->mm->membarrier |=
            MEMBARRIER_STATE_GLOBAL_EXPEDITED | MEMBARRIER_STATE_GLOBAL_EXPEDITED_READY;
        return 0;
    case MEMBARRIER_CMD_PRIVATE_EXPEDITED:
        return mb_private(MB_PLAIN, cpu_id);
    case MEMBARRIER_CMD_REGISTER_PRIVATE_EXPEDITED:
        return mb_register_private(MB_PLAIN);
    case MEMBARRIER_CMD_PRIVATE_EXPEDITED_SYNC_CORE:
        return mb_private(MB_SYNC_CORE, cpu_id);
    case MEMBARRIER_CMD_REGISTER_PRIVATE_EXPEDITED_SYNC_CORE:
        return mb_register_private(MB_SYNC_CORE);
    case MEMBARRIER_CMD_PRIVATE_EXPEDITED_RSEQ:
        return mb_private(MB_RSEQ, cpu_id);
    case MEMBARRIER_CMD_REGISTER_PRIVATE_EXPEDITED_RSEQ:
        return mb_register_private(MB_RSEQ);
    case MEMBARRIER_CMD_GET_REGISTRATIONS:
        return mb_registrations();
    default:
        return -EINVAL;
    }
}

void membarrier_switch(const struct process *p)
{
    if (p->mm && (p->mm->membarrier & MEMBARRIER_STATE_PRIVATE_EXPEDITED_SYNC_CORE)) {
        arch_sync_core();
    }
}
