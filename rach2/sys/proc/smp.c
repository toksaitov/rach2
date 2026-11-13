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
#include "../lib/kerrno.h"
#include "../lib/kirq.h"
#include "../lib/spinlock.h"
#include "../timer.h"
#include "../udelay.h"
#include "../watchdog.h"
#include "../lib/kstring.h"
#include "../mm/kheap.h"
#include "../mm/paging.h"
#include "../mm/usercopy.h"

/* The big lock's word: 1 while a processor holds it. Which one does is in
 * the records: the one whose `bkl_depth` is above 0. */
static volatile unsigned int bkl_word;

/* The processor that has waited long enough to go next, as its number plus
 * one, 0 for none (`bkl_acquire`). */
static volatile unsigned int bkl_claim;

/* A loan of the big lock (`bkl_lend`): the borrowing processor's number plus
 * one, with BKL_LENT_TAKEN set once it holds the lock, or 0 for none. The
 * word stays set through a loan, so no other processor takes the lock
 * between the lender's hold and the borrower's, or between the borrower's
 * and the lender's again. */
static volatile unsigned int bkl_lent;
#define BKL_LENT_TAKEN 0x80000000u

/* Updated with the big lock held; the waits are accumulated as TSC cycles
 * so that rounding at each acquisition loses no short waits. */
static uint64_t bkl_wait_cycles;
static uint64_t bkl_wait_max_cycles;
static volatile uint32_t bkl_releases; /* completed outer holds, modulo 2^32 */
static uint32_t bkl_wait_max_holds;    /* holds completed during the longest wait */
/* Keep the most recent releases and copy those of a new longest wait. */
struct bkl_hold_record {
    uint64_t cycles; /* full hold duration in TSC cycles */
    uint32_t vector; /* entry vector, 0 for none */
    int32_t nr;      /* entry system call number, -1 for none, or BKL_NR_* */
};
static struct bkl_hold_record bkl_recent[BKL_WAIT_HOLDS];
static struct bkl_hold_record bkl_wait_hold[BKL_WAIT_HOLDS];
static uint64_t bkl_hold_max_cycles;          /* the longest hold, from take to give back */
static uint32_t bkl_hold_vector;              /* its entry's vector, 0 for none */
static int32_t bkl_hold_nr = -1;              /* its entry's system call, -1 or BKL_NR_* */
static uint64_t bkl_seen_max_cycles;          /* the oldest hold an interrupt has found */
static uintptr_t bkl_seen_pc[BKL_SAMPLE_PCS]; /* that instruction, then its callers */

#define BKL_LONG_SLOTS 32
#define BKL_LONG_US    16000u
/* A nested interrupt's latest sample of a hold older than BKL_LONG_US.
 * The big lock and disabled local interrupts serialize the ring's writers. */
struct bkl_long_record {
    uint64_t clock; /* TSC at the sample, on the processors' shared clock */
    uint64_t age;   /* cycles since this hold's take */
    uint32_t cpu;
    uint32_t pid; /* thread interrupted, 0 when there is none */
    uint32_t vector;
    int32_t nr; /* entry system call, -1 for none, or BKL_NR_* */
    char comm[16];
    uintptr_t pc[BKL_SAMPLE_PCS];
};
static struct bkl_long_record bkl_long[BKL_LONG_SLOTS];
static unsigned int bkl_long_head, bkl_long_count;
static uint64_t bkl_long_since[NR_CPUS]; /* the hold sampled on this processor */
static unsigned int bkl_long_slot[NR_CPUS];
static unsigned char bkl_long_valid[NR_CPUS];

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

/* What `smp_calls_show` counts for an entry or an ioctl request since boot
 * or the last zeroing, in TSC cycles. A system call's `calls` and `wall`
 * count at its return, an interrupt's or an exception's `calls` at its entry,
 * each on its processor's `bkl_tally`; the holds are counted here, under the
 * lock, as `bkl_entry_cycles` counts them. */
struct bkl_call_count {
    uint64_t wall;   /* from before the entry's lock to the return */
    uint64_t held;   /* the holds charged to it */
    uint64_t waited; /* the waits for the lock that began those holds */
    uint32_t calls;
    uint32_t holds;
};
static struct bkl_call_count bkl_calls[BKL_ENTRY_KEYS];

/* The takes after a switch (`take 1`) divided by the system call the thread
 * slept in, its number below BKL_ENTRY_SYSCALLS or BKL_ENTRY_SYSCALLS for
 * any above: the rest of a call under the big lock after each sleep. They
 * count under `take 1` as well, and in no other line. */
static struct bkl_call_count bkl_slept[BKL_ENTRY_SYSCALLS + 1];

/* The holds of each kernel thread that it takes for itself, outside any
 * interrupt or exception it is in: at its first switch and after each
 * sleep (`take 1`), after an idle wait (`take 0`) and by `bkl_enter`,
 * `bkl_try_enter` or `bkl_take` in its own code (`vector 0`). The
 * processors' idle tasks count as one thread. Each hold is counted here
 * alone, under the thread's pid and name, so that bkl/calls names the
 * thread, and the lines it shows stay disjoint. A slot is the thread's
 * from its first such hold until the counts are zeroed; a thread that
 * finds every slot taken is counted under the key as before. The big
 * lock, which a hold's end holds, guards the slots. */
#define BKL_THREAD_SLOTS 32
struct bkl_thread_count {
    const void *key; /* its record, `cpu_table` for the idle tasks; NULL if free */
    uint32_t pid;
    char comm[16];
    struct bkl_call_count count;
};
static struct bkl_thread_count bkl_threads[BKL_THREAD_SLOTS];

/* The thread bkl/calls counts alone, which a write of `thread TID` or
 * `thread NAME` chooses (`smp_calls_store`): FILTER_NONE counts every
 * thread, FILTER_TID the thread whose ID is `filter_tid` and FILTER_COMM
 * each thread whose name is `filter_comm`, as Wine names its command
 * thread `wined3d_cs` before a program's loop gives it an ID. */
#define FILTER_NONE 0
#define FILTER_TID  1
#define FILTER_COMM 2
static volatile int filter_kind;
static uint32_t filter_tid;
static char filter_comm[16];

/* The chosen thread's switches away while it could still run, by the
 * thread switched to (`bkl_note_switch`): those of a sched_yield, and the
 * others, the tick's, an interrupt's or a call's exit. A slot is the
 * incoming thread's from its first such switch until the counts are
 * zeroed; one that finds every slot taken counts in `switches_lost`. The
 * run queues' lock, under which every switch is noted, guards them. */
#define SWITCH_SLOTS 32
struct bkl_switch_count {
    uint32_t pid; /* the thread switched to, 0 for a free slot */
    char comm[16];
    uint32_t yields;
    uint32_t preempts;
};
static struct bkl_switch_count switches[SWITCH_SLOTS];
static uint32_t switches_lost;

/* The chosen thread's sleeps by the system call it slept in, any number
 * from BKL_ENTRY_SYSCALLS up and a sleep outside a call counted at
 * BKL_ENTRY_SYSCALLS; a sleep in an ioctl counts under its request's slot
 * too (`bkl_ioctl_count.blocks`). Guarded as `switches`. */
static uint32_t blocks[BKL_ENTRY_SYSCALLS + 1];

/** Whether bkl/calls counts what thread `p` does: every thread while none
 * is chosen, else the chosen one alone. */
static int filter_wants(const struct process *p)
{
    int i;

    if (filter_kind == FILTER_NONE) {
        return 1;
    }
    if (!p) {
        return 0;
    }
    if (filter_kind == FILTER_TID) {
        return p->pid == filter_tid;
    }
    for (i = 0; i < (int)sizeof(filter_comm) && filter_comm[i] == p->comm[i]; i++) {
        if (filter_comm[i] == '\0') {
            return 1;
        }
    }
    return i == (int)sizeof(filter_comm);
}

/* A processor's own calls and their time for each key, then for each ioctl
 * slot, which a call counts with or without the big lock and a read of
 * bkl/calls sums over the processors, as Linux keeps per-processor counters
 * (`percpu_counter`). The boot processor's are static; each other's are
 * taken from the heap as it starts, and a processor without them does not
 * count. */
struct bkl_tally {
    uint64_t wall;
    uint32_t calls;
};
#define BKL_TALLIES (BKL_ENTRY_KEYS + BKL_IOCTL_SLOTS)
static struct bkl_tally bkl_tally_boot[BKL_TALLIES];
static struct bkl_tally *bkl_tally[NR_CPUS] = {bkl_tally_boot};

/* The ioctl requests by request number, open addressed; a slot once taken
 * keeps its request until boot, so a charge to it is never misplaced. */
struct bkl_ioctl_count {
    uint32_t cmd;
    volatile uint32_t used; /* IOCTL_FREE, IOCTL_CLAIM or IOCTL_USED */
    struct bkl_call_count count;
    uint32_t blocks; /* the chosen thread's sleeps in the request (`blocks`) */
};
static struct bkl_ioctl_count bkl_ioctls[BKL_IOCTL_SLOTS];
static volatile uint32_t bkl_ioctls_lost; /* calls of a request no slot took */

/* The profile of the kernel under the big lock: each sampled entry,
 * instruction and callers, open addressed, and the samples taken and lost. */
struct bkl_profile_slot {
    uintptr_t pc[BKL_PROFILE_FRAMES]; /* the instruction, then its callers, 0 past the chain */
    uint32_t entry;                   /* the process's `bkl_entry`: a key plus one, 0 for none */
    uint32_t cmd;                     /* the ioctl request for BKL_PROFILE_IOCTL, else 0 */
    uint32_t count;
};
static struct bkl_profile_slot bkl_profile[BKL_PROFILE_SLOTS];
static uint32_t bkl_profile_samples, bkl_profile_lost;
/* Set while `smp_profile_show` writes the profile or `smp_profile_store`
 * zeroes it, so that no tick changes the table and totals meanwhile; a tick
 * reads it under `profile_lock`, which keeps the table. */
static volatile int bkl_profile_showing;
static struct spinlock profile_lock = SPINLOCK_INIT_NAMED("bkl_profile");
/* Nonzero for the whole-kernel mode: every tick in kernel mode samples,
 * with or without the big lock. */
static volatile int bkl_profile_whole;
/* The thread the whole-kernel mode samples alone, by its ID; 0 for all. */
static volatile uint32_t bkl_profile_tid;

/* The tick's parts by row (`bkl_tick_charge`), in TSC cycles, kept by each
 * processor for its own ticks, which run without the big lock: a processor
 * writes its rows inside `seq`, odd while it writes, and zeroes them first
 * when `gen` is behind `bkl_tick_gen`, which a write of `bkl/tick` moves on,
 * so that no other processor writes them. The names `smp_tick_show` gives
 * the rows follow. */
struct bkl_tick_count {
    uint64_t cycles;
    uint64_t max;
    uint32_t calls;
};
struct bkl_tick_cpu {
    volatile uint32_t seq;
    volatile uint32_t gen;
    struct bkl_tick_count rows[BKL_TICK_ROWS];
};
static struct bkl_tick_cpu bkl_ticks[NR_CPUS];
static volatile uint32_t bkl_tick_gen;
static const char *const bkl_tick_names[BKL_TICK_ROWS] = {
    "clock",    "account", "wake",    "hda",     "block_lag", "block_request", "speaker",
    "i915",     "lock",    "signals", "load",    "netdev",    "netconsole",    "ath9k",
    "tcp",      "ip_frag", "ip6",     "netlink", "ip_loop",   "usb",           "input",
    "platform", "vkms",    "console", "klog",    "nested"};

/* The probes an open-addressed table makes before it gives up. */
#define BKL_PROBES 32

/* Set once the first hold, the boot processor's start-up, has ended. */
static int bkl_booted;

/* The spins after which a waiter claims the big lock, and the times a waiter
 * sees the lock free and unclaimed by it before it passes over the claim. The
 * second covers a claimant that does not run, since under a hypervisor a
 * virtual processor can be off the machine. */
#define BKL_CLAIM_AFTER 4096u
#define BKL_PASS_AFTER  (1u << 16)

/* One sender of requests at a time: a TLB request, a barrier, a page
 * directory to give up or a function to run. Each processor has one slot of
 * each kind, which a sender fills and waits for, so a second sender would
 * overwrite the first's request. A processor that waits for the lock
 * answers its own requests (`spin_lock`), so two senders do not wait on each
 * other. */
static struct spinlock ipi_lock = SPINLOCK_INIT_NAMED("ipi_lock");

/* The processors online, the boot processor counted (`smp_cpu_count`). */
static int cpus_online = 1;

/* The shootdowns that interrupted a processor, and the interrupts they sent
 * (`smp_tlb_count`); written under the big lock. */
static uint32_t tlb_counts[2];

/* The interrupts each processor sent, by kind (`smp_ipi_count`), each
 * written by its own processor alone. */
static uint32_t ipi_counts[NR_CPUS][SMP_IPI_KINDS];

static const char *const ipi_names[SMP_IPI_KINDS] = {"kick",  "tlb",  "kernel_tlb", "barrier",
                                                     "space", "call", "stop"};

/** Send processor `cpu` the interprocessor interrupt, counted under `kind`
 * on this processor. */
static void ipi_send(int cpu, int kind)
{
    ipi_counts[arch_cpu_id()][kind]++;
    platform_cpu_ipi(cpu);
}

/** Send each processor of the set `cpus` the interprocessor interrupt,
 * counted under `kind` once for each, in as few messages as the platform
 * allows (`platform_cpu_ipi_many`). */
static void ipi_send_many(uint64_t cpus, int kind)
{
    uint64_t left = cpus;

    if (cpus == 0) {
        return;
    }
    for (; left != 0; left &= left - 1u) {
        ipi_counts[arch_cpu_id()][kind]++;
    }
    platform_cpu_ipi_many(cpus);
}

uint32_t smp_ipi_count(int cpu, int kind)
{
    if (cpu < 0 || cpu >= NR_CPUS || kind < 0 || kind >= SMP_IPI_KINDS) {
        return 0;
    }
    return ipi_counts[cpu][kind];
}

const char *smp_ipi_name(int kind)
{
    return kind >= 0 && kind < SMP_IPI_KINDS ? ipi_names[kind] : "?";
}

/**
 * Spin until the big lock is free and take it, answering this processor's
 * requests meanwhile, since the holder may be waiting for the answer to its
 * own. Interrupts are off.
 *
 * An exchange alone favours the processor that last had the lock's cache
 * line, such as the holder's hyperthread, and can pass over the others for
 * seconds, while a process queued on a spinning processor waits as long. So a
 * waiter past `BKL_CLAIM_AFTER` spins claims the lock, and the others leave it
 * to the claimant.
 *
 * A lock that is free, unclaimed and not lent is taken at once, with no wait
 * to account and so no reading of the clock, as Linux's queued_spin_lock
 * takes a free lock with one exchange and leaves the rest to its slow path
 * (include/asm-generic/qspinlock.h:107-114): under QEMU's emulator the two
 * readings cost more than the rest of the take.
 */
static void bkl_acquire(struct percpu *cpu)
{
    unsigned int me = (unsigned int)arch_cpu_id() + 1u;
    uint32_t spins = 0, passed = 0;
    uint32_t releases;
    uint64_t start, waited;
    uint32_t wake_start, wake_wait;
    unsigned int i, n;

    if (!bkl_word && !bkl_claim && bkl_lent != me && arch_atomic_xchg(&bkl_word, 1u) == 0) {
        cpu->bkl_since = arch_cycles();
        cpu->bkl_waited = 0;
        k_memset(bkl_early_cycles[me - 1u], 0, sizeof(bkl_early_cycles[0]));
        return;
    }
    releases = bkl_releases;
    start = arch_cycles();
    wake_start = waketime_clock();
    cpu->wake_bkl_start = wake_start;
    cpu->wake_bkl_waiting = 1;
    for (;;) {
        unsigned int claim = bkl_claim;

        if (bkl_lent == me && arch_atomic_cmpxchg(&bkl_lent, me, me | BKL_LENT_TAKEN) == me) {
            break; /* lent: the word is the lender's, and stays set */
        }
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
    cpu->bkl_waited = waited;
    bkl_wait_cycles += waited;
    if (waited > bkl_wait_max_cycles) {
        bkl_wait_max_cycles = waited;
        bkl_wait_max_holds = bkl_releases - releases;
        n = bkl_wait_max_holds < BKL_WAIT_HOLDS ? bkl_wait_max_holds : BKL_WAIT_HOLDS;
        for (i = 0; i < n; i++) {
            bkl_wait_hold[i] = bkl_recent[(bkl_releases - n + i) % BKL_WAIT_HOLDS];
        }
    }
}

/** Add a hold of `held` cycles that waited `waited` for the lock to `c`. */
static void call_hold(struct bkl_call_count *c, uint64_t held, uint64_t waited)
{
    c->held += held;
    c->waited += waited;
    c->holds++;
}

/* The vector of a system call's entry, in a hold's record. */
#define BKL_CALL_VECTOR 0x80u

/** The key of `smp_entry_hold` that the hold under way on `cpu` counts
 * under, from its entry's vector and number. */
static int hold_key(const struct percpu *cpu)
{
    if (cpu->bkl_nr >= 0) {
        return cpu->bkl_nr < BKL_ENTRY_SYSCALLS ? cpu->bkl_nr : BKL_KEY_BIGCALL;
    }
    if (cpu->bkl_nr == BKL_NR_IDLE) {
        return BKL_KEY_IDLE;
    }
    if (cpu->bkl_nr == BKL_NR_SWITCH) {
        return BKL_KEY_SWITCH;
    }
    return BKL_ENTRY_SYSCALLS + (int)(cpu->bkl_vector & 0xffu);
}

/** The count of the holds of the kernel thread `cpu` runs, its slot taken
 * at its first hold, or NULL when the thread is no kernel thread or every
 * slot is another's. The processors' idle tasks share one slot, whose key
 * is `cpu_table`. A record used again by another thread is told apart by
 * its pid. The big lock is held. */
static struct bkl_call_count *thread_count(const struct percpu *cpu)
{
    const struct process *p = cpu->current;
    struct bkl_thread_count *spare = NULL, *t;
    const void *key;
    int i;

    if (!p) {
        return NULL;
    }
    key = p == cpu->idle ? (const void *)cpu_table : p->kthread_entry ? (const void *)p : NULL;
    if (!key) {
        return NULL;
    }
    for (i = 0; i < BKL_THREAD_SLOTS; i++) {
        t = &bkl_threads[i];
        if (t->key == key && t->pid == p->pid) {
            return &t->count;
        }
        if (!t->key && !spare) {
            spare = t;
        }
    }
    if (!spare) {
        return NULL;
    }
    spare->key = key;
    spare->pid = p->pid;
    k_memcpy(spare->comm, p->comm, sizeof(spare->comm));
    spare->comm[sizeof(spare->comm) - 1] = '\0';
    k_memset(&spare->count, 0, sizeof(spare->count));
    return &spare->count;
}

/** Charge a hold of `held` cycles that waited `waited` under its entry
 * `key`, its kernel thread's line, the call it slept in and its ioctl
 * request's slot plus one `ioctl`, as bkl/calls shows them. */
static void bkl_hold_count(const struct percpu *cpu, int key, int ioctl, uint64_t held,
                           uint64_t waited)
{
    struct bkl_call_count *mine;

    mine = key == BKL_ENTRY_SYSCALLS || key == BKL_KEY_SWITCH || key == BKL_KEY_IDLE
               ? thread_count(cpu)
               : NULL;
    call_hold(mine ? mine : &bkl_calls[key], held, waited);
    if (key == BKL_KEY_SWITCH && cpu->bkl_slept >= 0) {
        call_hold(
            &bkl_slept[cpu->bkl_slept < BKL_ENTRY_SYSCALLS ? cpu->bkl_slept : BKL_ENTRY_SYSCALLS],
            held, waited);
    }
    if (ioctl > 0) {
        call_hold(&bkl_ioctls[ioctl - 1].count, held, waited);
    }
}

/** Count the hold that ends now on `cpu`, which still holds the lock, under
 * its entry and against the longest. The first hold to end, the boot
 * processor's start-up, which nobody waits on, is left out. A take after an
 * idle wait or a switch is recorded as BKL_NR_IDLE or BKL_NR_SWITCH. */
static void bkl_hold_end(struct percpu *cpu)
{
    uint64_t now = arch_cycles(), held = now - cpu->bkl_since, waited;
    int c = arch_cpu_id();
    int key = hold_key(cpu), ioctl;
    int32_t nr = cpu->bkl_nr;

    bkl_recent[bkl_releases % BKL_WAIT_HOLDS].cycles = held;
    bkl_recent[bkl_releases % BKL_WAIT_HOLDS].vector = cpu->bkl_vector;
    bkl_recent[bkl_releases % BKL_WAIT_HOLDS].nr = nr;
    bkl_releases++;
    waited = cpu->bkl_waited;
    ioctl = cpu->bkl_ioctl;
    cpu->bkl_waited = 0;
    cpu->bkl_ioctl = 0;
    if (!bkl_booted) {
        bkl_booted = 1;
        return;
    }
    bkl_entry_cycles[key] += held;
    bkl_entry_holds[key]++;
    if (!cpu->bkl_unfiltered) {
        bkl_hold_count(cpu, key, ioctl, held, waited);
    }
    if (held > bkl_hold_max_cycles) {
        bkl_hold_max_cycles = held;
        bkl_hold_vector = cpu->bkl_vector;
        bkl_hold_nr = nr;
        k_memcpy(bkl_hold_early_cycles, bkl_early_cycles[c], sizeof(bkl_hold_early_cycles));
        k_memcpy(bkl_hold_early_pc, bkl_early_pc[c], sizeof(bkl_hold_early_pc));
    }
}

uint64_t bkl_tick_charge(int row, uint64_t start)
{
    uint64_t now = arch_cycles(), spent = now - start;
    struct bkl_tick_cpu *mine = &bkl_ticks[arch_cpu_id()];
    struct bkl_tick_count *c = &mine->rows[row];
    uint32_t gen = bkl_tick_gen;

    mine->seq++;
    k_barrier();
    if (mine->gen != gen) {
        k_memset(mine->rows, 0, sizeof(mine->rows));
        mine->gen = gen;
    }
    c->calls++;
    c->cycles += spent;
    if (spent > c->max) {
        c->max = spent;
    }
    k_barrier();
    mine->seq++;
    return now;
}

/** This processor's tally for `key`, an entry's key or BKL_ENTRY_KEYS plus
 * an ioctl slot, or NULL when it has none. Interrupts are off. */
static struct bkl_tally *tally_of(int key)
{
    struct bkl_tally *t = bkl_tally[arch_cpu_id()];

    return t ? &t[key] : NULL;
}

void bkl_note_entry(uint32_t vector, int32_t nr, int locked)
{
    struct percpu *cpu = this_cpu();
    struct bkl_tally *t;

    if (locked && cpu->bkl_depth == 1) {
        cpu->bkl_vector = vector;
        cpu->bkl_nr = nr;
        if (nr < 0) {
            cpu->bkl_ioctl = 0;
        }
    }
    if (nr < 0 && filter_wants(cpu->current)) {
        t = tally_of(BKL_ENTRY_SYSCALLS + (int)(vector & 0xffu));
        if (t) {
            t->calls++;
        }
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
    uint64_t now, held, held_us;

    if (cpu->bkl_depth < 2) {
        return;
    }
    now = arch_cycles();
    held = now - cpu->bkl_since;
    held_us = timer_cycles_us(held);
    top = cpu->current ? cpu->current->kernel_stack : 0;
    if (held_us >= BKL_LONG_US) {
        struct bkl_long_record *r;

        if (!bkl_long_valid[c] || bkl_long_since[c] != cpu->bkl_since) {
            bkl_long_slot[c] = bkl_long_head;
            bkl_long_head = (bkl_long_head + 1u) % BKL_LONG_SLOTS;
            if (bkl_long_count < BKL_LONG_SLOTS)
                bkl_long_count++;
            bkl_long_since[c] = cpu->bkl_since;
            bkl_long_valid[c] = 1;
        }
        r = &bkl_long[bkl_long_slot[c]];
        r->clock = now;
        r->age = held;
        r->cpu = (uint32_t)c;
        r->pid = cpu->current ? cpu->current->pid : 0;
        r->vector = cpu->bkl_vector;
        r->nr = cpu->bkl_nr;
        k_memset(r->comm, 0, sizeof(r->comm));
        if (cpu->current)
            k_memcpy(r->comm, cpu->current->comm, sizeof(r->comm) - 1);
        sample_chain(r->pc, pc, fp, top);
    }
    for (m = 0; m < BKL_SAMPLE_MARKS; m++) {
        if (!bkl_early_cycles[c][m] && held_us >= 1000000u << m) {
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

/* A slot's `used`: free, being claimed by the processor that will write its
 * request, and holding its request. */
#define IOCTL_FREE  0u
#define IOCTL_CLAIM 1u
#define IOCTL_USED  2u

/** The slot of ioctl request `cmd` plus one, claimed for it when it has none,
 * or 0 when none of the slots its hash names is free or its own. A slot is
 * claimed by a compare-and-exchange and its request written before it is
 * marked used, so two processors never take one slot for two requests, and
 * a reader that finds a slot being claimed waits for its request. */
static int ioctl_slot(uint32_t cmd)
{
    uint32_t h = (cmd * 2654435761u) % BKL_IOCTL_SLOTS;
    int i;

    for (i = 0; i < BKL_PROBES; i++) {
        uint32_t slot = (h + (uint32_t)i) % BKL_IOCTL_SLOTS;
        struct bkl_ioctl_count *s = &bkl_ioctls[slot];

        if (s->used == IOCTL_FREE &&
            arch_atomic_cmpxchg(&s->used, IOCTL_FREE, IOCTL_CLAIM) == IOCTL_FREE) {
            s->cmd = cmd;
            sched_mb();
            s->used = IOCTL_USED;
        }
        while (s->used == IOCTL_CLAIM) {
            arch_relax();
        }
        if (s->cmd == cmd) {
            return (int)slot + 1;
        }
    }
    return 0;
}

int bkl_note_ioctl(uint32_t cmd)
{
    struct percpu *cpu = this_cpu();
    int slot = ioctl_slot(cmd);
    uint32_t lost;

    if (slot == 0) {
        do {
            lost = bkl_ioctls_lost;
        } while (arch_atomic_cmpxchg(&bkl_ioctls_lost, lost, lost + 1u) != lost);
    } else if (cpu->bkl_depth == 1) {
        cpu->bkl_ioctl = slot;
    }
    return slot;
}

void smp_call_end(uint32_t nr, int ioctl, uint64_t start)
{
    uint64_t wall = start != 0 ? arch_cycles() - start : 0;
    struct bkl_tally *t = tally_of(nr < BKL_ENTRY_SYSCALLS ? (int)nr : BKL_KEY_BIGCALL);

    if (!filter_wants(current_proc)) {
        return;
    }
    if (t) {
        t->calls++;
        t->wall += wall;
    }
    t = ioctl > 0 ? tally_of(BKL_ENTRY_KEYS + ioctl - 1) : NULL;
    if (t) {
        t->calls++;
        t->wall += wall;
    }
}

void bkl_entry_push(struct bkl_entry_saved *saved, uint32_t vector, int32_t nr, int ioctl,
                    uint32_t cmd)
{
    struct process *p = current_proc;
    int key;

    if (!p) {
        saved->entry = 0;
        saved->cmd = 0;
        return;
    }
    saved->entry = p->bkl_entry;
    saved->cmd = p->bkl_cmd;
    if (ioctl) {
        key = BKL_PROFILE_IOCTL;
    } else if (nr >= 0) {
        key = nr < BKL_ENTRY_SYSCALLS ? (int)nr : BKL_KEY_BIGCALL;
    } else {
        key = BKL_ENTRY_SYSCALLS + (int)(vector & 0xffu);
    }
    p->bkl_cmd = ioctl ? cmd : 0;
    p->bkl_entry = (uint32_t)key + 1u;
}

void bkl_entry_pop(const struct bkl_entry_saved *saved)
{
    struct process *p = current_proc;

    if (p) {
        p->bkl_entry = saved->entry;
        p->bkl_cmd = saved->cmd;
    }
}

/** The entry key plus one that thread `p` is in, for its holds and the
 * profile's samples, and its ioctl request in `cmd`: the one its router
 * pushed (`bkl_entry_push`), or, in the body of a system call that ran
 * without the big lock and so pushed none, the call's (`isr_call_entry`);
 * 0 for none. `p` is this processor's current thread. */
static uint32_t entry_of(const struct process *p, uint32_t *cmd)
{
    int key;

    *cmd = 0;
    if (!p) {
        return 0;
    }
    if (p->bkl_entry != 0 || p->in_syscall != PROC_IN_CALL) {
        *cmd = p->bkl_cmd;
        return p->bkl_entry;
    }
    key = isr_call_entry(p, cmd);
    return key < 0 ? 0u : (uint32_t)key + 1u;
}

void bkl_note_switch(const struct process *old, const struct process *next, int yielded)
{
    struct bkl_switch_count *spare = NULL, *c = NULL;
    uint32_t cmd;
    int i, key;

    if (filter_kind == FILTER_NONE || !filter_wants(old)) {
        return;
    }
    if (old->state != PROC_READY) {
        key = (int)entry_of(old, &cmd) - 1;
        if (key == BKL_PROFILE_IOCTL) {
            i = ioctl_slot(cmd);
            if (i > 0) {
                bkl_ioctls[i - 1].blocks++;
            }
            key = old->orig_nr;
        }
        blocks[key >= 0 && key < BKL_ENTRY_SYSCALLS ? key : BKL_ENTRY_SYSCALLS]++;
        return;
    }
    for (i = 0; i < SWITCH_SLOTS; i++) {
        c = &switches[i];
        if (c->pid == next->pid) {
            break;
        }
        if (c->pid == 0 && !spare) {
            spare = c;
        }
    }
    if (i == SWITCH_SLOTS) {
        if (!spare) {
            switches_lost++;
            return;
        }
        c = spare;
        c->pid = next->pid;
        k_memcpy(c->comm, next->comm, sizeof(c->comm));
        c->comm[sizeof(c->comm) - 1] = '\0';
    }
    if (yielded) {
        c->yields++;
    } else {
        c->preempts++;
    }
}

/** Count the sample `chain` in the entry `entry`, the ioctl request `cmd`
 * for BKL_PROFILE_IOCTL, in the profile's table. `profile_lock` is held. */
static void profile_count(const uintptr_t chain[BKL_SAMPLE_PCS], uint32_t entry, uint32_t cmd)
{
    uintptr_t h;
    int i, f;

    bkl_profile_samples++;
    h = (uintptr_t)entry * 2654435761u ^ cmd;
    for (f = 0; f < BKL_PROFILE_FRAMES; f++) {
        h = h * 31u ^ (chain[f] >> 2);
    }
    for (i = 0; i < BKL_PROBES; i++) {
        struct bkl_profile_slot *s = &bkl_profile[(h + (uintptr_t)i) % BKL_PROFILE_SLOTS];

        if (s->count == 0) {
            k_memcpy(s->pc, chain, sizeof(s->pc));
            s->entry = entry;
            s->cmd = cmd;
        }
        f = 0;
        while (f < BKL_PROFILE_FRAMES && s->pc[f] == chain[f]) {
            f++;
        }
        if (f == BKL_PROFILE_FRAMES && s->entry == entry && s->cmd == cmd) {
            s->count++;
            return;
        }
    }
    bkl_profile_lost++;
}

/** Return nonzero when the profile's mode takes a sample of the tick on
 * `cpu`: a tick under the big lock, or any in the whole-kernel mode, and
 * there only while the chosen thread, if one is, runs. A tick calls it
 * without `profile_lock` to skip the frame walk, and again under it, since
 * `smp_profile_store` may change the mode between the two. */
static int profile_wants(const struct percpu *cpu)
{
    if (!bkl_profile_whole && cpu->bkl_depth < 1) {
        return 0;
    }
    return bkl_profile_tid == 0 || (cpu->current && cpu->current->pid == bkl_profile_tid);
}

void bkl_profile_sample(uintptr_t pc, uintptr_t fp)
{
    struct percpu *cpu = this_cpu();
    uintptr_t chain[BKL_SAMPLE_PCS];
    uint32_t entry, cmd;

    if (!profile_wants(cpu)) {
        return;
    }
    sample_chain(chain, pc, fp, cpu->current ? cpu->current->kernel_stack : 0);
    entry = entry_of(cpu->current, &cmd);
    spin_lock(&profile_lock);
    if (!bkl_profile_showing && profile_wants(cpu)) {
        profile_count(chain, entry, cmd);
    }
    spin_unlock(&profile_lock);
}

/* A text being written into a buffer of `cap` bytes, `len` its whole length
 * so far, which may pass `cap`; what passes it is counted, not written. */
struct bkl_text {
    char *buf;
    uint32_t cap;
    uint32_t len;
};

/** Append the character `c` to `t`. */
static void text_ch(struct bkl_text *t, char c)
{
    if (t->len < t->cap) {
        t->buf[t->len] = c;
    }
    t->len++;
}

/** Append the string `s` to `t`. */
static void text_str(struct bkl_text *t, const char *s)
{
    while (*s) {
        text_ch(t, *s++);
    }
}

/** Append `v` to `t` in decimal, by subtracting powers of ten, since i686's
 * kernel has no 64-bit division (no libgcc). */
static void text_dec(struct bkl_text *t, uint64_t v)
{
    uint64_t powers[20];
    int n, digit, shown = 0;

    powers[0] = 1u;
    for (n = 1; n < 20; n++) {
        powers[n] = powers[n - 1] * 10u;
    }
    for (n = 19; n >= 0; n--) {
        for (digit = 0; v >= powers[n]; digit++) {
            v -= powers[n];
        }
        if (digit != 0 || shown || n == 0) {
            text_ch(t, (char)('0' + digit));
            shown = 1;
        }
    }
}

/** Append `v` to `t` as `0x` and its hex digits, without leading zeros. */
static void text_hex(struct bkl_text *t, uint64_t v)
{
    char digits[16];
    int n = 0;

    do {
        digits[n++] = "0123456789abcdef"[v & 0xfu];
        v >>= 4;
    } while (v != 0);
    text_str(t, "0x");
    while (n > 0) {
        text_ch(t, digits[--n]);
    }
}

/** Append `kind`, `number` (in hex for `hex`) and the counts of `c`, the
 * times in microseconds, without the line's end. */
static void text_fields(struct bkl_text *t, const char *kind, uint64_t number, int hex,
                        const struct bkl_call_count *c)
{
    text_str(t, kind);
    text_ch(t, ' ');
    if (hex) {
        text_hex(t, number);
    } else {
        text_dec(t, number);
    }
    text_ch(t, ' ');
    text_dec(t, c->calls);
    text_ch(t, ' ');
    text_dec(t, timer_cycles_us(c->wall));
    text_ch(t, ' ');
    text_dec(t, c->holds);
    text_ch(t, ' ');
    text_dec(t, timer_cycles_us(c->held));
    text_ch(t, ' ');
    text_dec(t, timer_cycles_us(c->waited));
}

/** Append the line of `held`, with the processors' calls of tally `tally`
 * added (`text_fields`); nothing when it counts nothing. */
static void text_count(struct bkl_text *t, const char *kind, uint64_t number, int hex,
                       const struct bkl_call_count *held, int tally)
{
    struct bkl_call_count sum = *held;
    int cpu;

    for (cpu = 0; cpu < NR_CPUS; cpu++) {
        if (bkl_tally[cpu]) {
            sum.calls += bkl_tally[cpu][tally].calls;
            sum.wall += bkl_tally[cpu][tally].wall;
        }
    }
    if (sum.calls == 0 && sum.holds == 0) {
        return;
    }
    text_fields(t, kind, number, hex, &sum);
    text_ch(t, '\n');
}

/** Append the line of kernel thread `k`, its name last. */
static void text_thread(struct bkl_text *t, const struct bkl_thread_count *k)
{
    text_fields(t, "kthread", k->pid, 0, &k->count);
    text_ch(t, ' ');
    text_str(t, k->comm);
    text_ch(t, '\n');
}

/** Append the chosen thread's lines: `thread` with its ID or name, its
 * switches away by the thread switched to, and its sleeps by system call
 * and by ioctl request. */
static void text_filter(struct bkl_text *t)
{
    int i;

    text_str(t, "thread ");
    if (filter_kind == FILTER_TID) {
        text_dec(t, filter_tid);
    } else {
        text_str(t, filter_comm);
    }
    text_ch(t, '\n');
    for (i = 0; i < SWITCH_SLOTS; i++) {
        if (switches[i].pid != 0) {
            text_str(t, "switch ");
            text_dec(t, switches[i].pid);
            text_ch(t, ' ');
            text_dec(t, switches[i].yields);
            text_ch(t, ' ');
            text_dec(t, switches[i].preempts);
            text_ch(t, ' ');
            text_str(t, switches[i].comm);
            text_ch(t, '\n');
        }
    }
    text_str(t, "switch_lost ");
    text_dec(t, switches_lost);
    text_ch(t, '\n');
    for (i = 0; i <= BKL_ENTRY_SYSCALLS; i++) {
        if (blocks[i] != 0) {
            text_str(t, "block syscall ");
            text_dec(t, (uint64_t)i);
            text_ch(t, ' ');
            text_dec(t, blocks[i]);
            text_ch(t, '\n');
        }
    }
    for (i = 0; i < BKL_IOCTL_SLOTS; i++) {
        if (bkl_ioctls[i].used == IOCTL_USED && bkl_ioctls[i].blocks != 0) {
            text_str(t, "block ioctl ");
            text_hex(t, bkl_ioctls[i].cmd);
            text_ch(t, ' ');
            text_dec(t, bkl_ioctls[i].blocks);
            text_ch(t, '\n');
        }
    }
}

int32_t smp_calls_show(void *data, char *buf, uint32_t cap)
{
    struct bkl_text t;
    int key;

    (void)data;
    t.buf = buf;
    t.cap = cap;
    t.len = 0;
    text_str(&t, "kind number calls wall_us holds held_us wait_us\n");
    for (key = 0; key < BKL_ENTRY_SYSCALLS; key++) {
        text_count(&t, "syscall", (uint64_t)key, 0, &bkl_calls[key], key);
    }
    text_count(&t, "syscall", BKL_ENTRY_SYSCALLS, 0, &bkl_calls[BKL_KEY_BIGCALL], BKL_KEY_BIGCALL);
    for (key = BKL_ENTRY_SYSCALLS; key < BKL_KEY_BIGCALL; key++) {
        text_count(&t, "vector", (uint64_t)(key - BKL_ENTRY_SYSCALLS), 0, &bkl_calls[key], key);
    }
    text_count(&t, "take", 0, 0, &bkl_calls[BKL_KEY_IDLE], BKL_KEY_IDLE);
    text_count(&t, "take", 1, 0, &bkl_calls[BKL_KEY_SWITCH], BKL_KEY_SWITCH);
    for (key = 0; key <= BKL_ENTRY_SYSCALLS; key++) {
        if (bkl_slept[key].holds != 0) {
            text_fields(&t, "slept", (uint64_t)key, 0, &bkl_slept[key]);
            text_ch(&t, '\n');
        }
    }
    for (key = 0; key < BKL_THREAD_SLOTS; key++) {
        if (bkl_threads[key].key) {
            text_thread(&t, &bkl_threads[key]);
        }
    }
    for (key = 0; key < BKL_IOCTL_SLOTS; key++) {
        if (bkl_ioctls[key].used == IOCTL_USED) {
            text_count(&t, "ioctl", bkl_ioctls[key].cmd, 1, &bkl_ioctls[key].count,
                       BKL_ENTRY_KEYS + key);
        }
    }
    text_str(&t, "ioctl_lost ");
    text_dec(&t, bkl_ioctls_lost);
    text_ch(&t, '\n');
    if (filter_kind != FILTER_NONE) {
        text_filter(&t);
    }
    return (int32_t)t.len;
}

int32_t smp_calls_store(void *data, const char __user *buf, uint32_t len)
{
    static const char thread[7] = {'t', 'h', 'r', 'e', 'a', 'd', ' '};
    char word[32];
    uint32_t n = len < sizeof(word) ? len : sizeof(word), i, tid = 0, digit;
    int kind = FILTER_NONE, digits = 1;
    uint32_t flags;

    (void)data;
    if (n >= sizeof(thread) && copy_from_user(word, buf, n) == 0) {
        for (i = 0; i < sizeof(thread) && word[i] == thread[i]; i++) {
        }
        kind = i == sizeof(thread) ? FILTER_COMM : FILTER_NONE;
    }
    if (kind != FILTER_NONE) {
        if (n == sizeof(word) && n < len) {
            return -EINVAL;
        }
        while (n > sizeof(thread) && (word[n - 1] == '\n' || word[n - 1] == ' ')) {
            n--;
        }
        if (n == sizeof(thread) || n - sizeof(thread) >= sizeof(filter_comm)) {
            return -EINVAL;
        }
        for (i = sizeof(thread); i < n; i++) {
            if (word[i] < '0' || word[i] > '9') {
                digits = 0;
            } else if (digits) {
                digit = (uint32_t)(word[i] - '0');
                if (tid > (0x7fffffffu - digit) / 10u) {
                    return -EINVAL;
                }
                tid = tid * 10u + digit;
            }
        }
        if (digits) {
            kind = FILTER_TID;
        }
    }
    flags = sched_lock_take();
    filter_kind = FILTER_NONE;
    k_memset(bkl_calls, 0, sizeof(bkl_calls));
    k_memset(bkl_slept, 0, sizeof(bkl_slept));
    k_memset(bkl_threads, 0, sizeof(bkl_threads));
    for (i = 0; i < BKL_IOCTL_SLOTS; i++) {
        k_memset(&bkl_ioctls[i].count, 0, sizeof(bkl_ioctls[i].count));
        bkl_ioctls[i].blocks = 0;
    }
    for (i = 0; i < NR_CPUS; i++) {
        if (bkl_tally[i]) {
            k_memset(bkl_tally[i], 0, sizeof(struct bkl_tally) * BKL_TALLIES);
        }
    }
    bkl_ioctls_lost = 0;
    k_memset(switches, 0, sizeof(switches));
    switches_lost = 0;
    k_memset(blocks, 0, sizeof(blocks));
    k_memset(filter_comm, 0, sizeof(filter_comm));
    if (kind == FILTER_COMM) {
        k_memcpy(filter_comm, word + sizeof(thread), n - sizeof(thread));
    }
    filter_tid = tid;
    filter_kind = kind;
    sched_lock_give(flags);
    return (int32_t)len;
}

/** Add processor `c`'s rows to `sum`, read whole inside its `seq`, as
 * Linux's u64_stats_sync.h:22-24 reads a writer's counters; rows not yet
 * zeroed for the current generation count as zero. Interrupts are off, so
 * that this processor's own tick does not write while it is read. */
static void tick_rows_add(int c, struct bkl_tick_count *sum)
{
    const struct bkl_tick_cpu *cpu = &bkl_ticks[c];
    struct bkl_tick_count rows[BKL_TICK_ROWS];
    uint32_t seq;
    int row;

    for (;;) {
        seq = cpu->seq;
        if (seq & 1u) {
            arch_relax();
            continue;
        }
        k_barrier();
        if (cpu->gen != bkl_tick_gen) {
            return;
        }
        k_memcpy(rows, cpu->rows, sizeof(rows));
        k_barrier();
        if (cpu->seq == seq) {
            break;
        }
    }
    for (row = 0; row < BKL_TICK_ROWS; row++) {
        sum[row].calls += rows[row].calls;
        sum[row].cycles += rows[row].cycles;
        if (rows[row].max > sum[row].max) {
            sum[row].max = rows[row].max;
        }
    }
}

int32_t smp_tick_show(void *data, char *buf, uint32_t cap)
{
    struct bkl_tick_count snap[BKL_TICK_ROWS];
    struct bkl_text t;
    uint32_t flags;
    int row, c;

    (void)data;
    k_memset(snap, 0, sizeof(snap));
    flags = kirq_save();
    for (c = 0; c < NR_CPUS; c++) {
        tick_rows_add(c, snap);
    }
    kirq_restore(flags);
    t.buf = buf;
    t.cap = cap;
    t.len = 0;
    text_str(&t, "part calls cycles us max_us\n");
    for (row = 0; row < BKL_TICK_ROWS; row++) {
        text_str(&t, bkl_tick_names[row]);
        text_ch(&t, ' ');
        text_dec(&t, snap[row].calls);
        text_ch(&t, ' ');
        text_dec(&t, snap[row].cycles);
        text_ch(&t, ' ');
        text_dec(&t, timer_cycles_us(snap[row].cycles));
        text_ch(&t, ' ');
        text_dec(&t, timer_cycles_us(snap[row].max));
        text_ch(&t, '\n');
    }
    return (int32_t)t.len;
}

int32_t smp_tick_store(void *data, const char __user *buf, uint32_t len)
{
    uint32_t flags;

    (void)data;
    (void)buf;
    flags = kirq_save();
    bkl_tick_gen++;
    kirq_restore(flags);
    return (int32_t)len;
}

/** Append a profile slot's entry, as `bkl/calls` names one: `syscall` and
 * its number, `vector` and its number, `ioctl` and its request in hex, or
 * `kernel 0` for a process in none, such as a kernel thread. */
static void text_entry(struct bkl_text *t, uint32_t entry, uint32_t cmd)
{
    int key = (int)entry - 1;

    if (entry == 0) {
        text_str(t, "kernel 0");
    } else if (key == BKL_PROFILE_IOCTL) {
        text_str(t, "ioctl ");
        text_hex(t, cmd);
    } else if (key < BKL_ENTRY_SYSCALLS || key == BKL_KEY_BIGCALL) {
        text_str(t, "syscall ");
        text_dec(t, (uint64_t)(key < BKL_ENTRY_SYSCALLS ? key : BKL_ENTRY_SYSCALLS));
    } else {
        text_str(t, "vector ");
        text_dec(t, (uint64_t)(key - BKL_ENTRY_SYSCALLS));
    }
}

/** Hold every tick off the profile's table while `on`, and let them back
 * when not: the flag is set under `profile_lock`, so a tick counting a
 * sample when it is set finishes before this returns. */
static void profile_hold(int on)
{
    uint32_t flags = spin_lock_irqsave(&profile_lock);

    bkl_profile_showing = on;
    spin_unlock_irqrestore(&profile_lock, flags);
}

int32_t smp_profile_show(void *data, char *buf, uint32_t cap)
{
    struct bkl_text t;
    int i, f;

    (void)data;
    t.buf = buf;
    t.cap = cap;
    t.len = 0;
    profile_hold(1);
    text_str(&t, "samples ");
    text_dec(&t, bkl_profile_samples);
    text_str(&t, "\nlost ");
    text_dec(&t, bkl_profile_lost);
    text_ch(&t, '\n');
    for (i = 0; i < BKL_PROFILE_SLOTS; i++) {
        if (bkl_profile[i].count != 0) {
            text_entry(&t, bkl_profile[i].entry, bkl_profile[i].cmd);
            for (f = 0; f < BKL_PROFILE_FRAMES; f++) {
                text_ch(&t, ' ');
                text_hex(&t, bkl_profile[i].pc[f]);
            }
            text_ch(&t, ' ');
            text_dec(&t, bkl_profile[i].count);
            text_ch(&t, '\n');
        }
    }
    profile_hold(0);
    return (int32_t)t.len;
}

int32_t smp_profile_store(void *data, const char __user *buf, uint32_t len)
{
    static const char kernel[6] = {'k', 'e', 'r', 'n', 'e', 'l'};
    char word[24];
    uint32_t i, n, digit, tid = 0;
    int whole;

    (void)data;
    n = len < sizeof(word) ? len : sizeof(word);
    whole = n >= sizeof(kernel) && copy_from_user(word, buf, n) == 0;
    for (i = 0; whole && i < sizeof(kernel); i++) {
        whole = word[i] == kernel[i];
    }
    for (; whole && i < n && word[i] == ' '; i++) {
    }
    for (; whole && i < n && word[i] >= '0' && word[i] <= '9'; i++) {
        digit = (uint32_t)(word[i] - '0');
        if (tid > (0x7fffffffu - digit) / 10u) {
            return -EINVAL;
        }
        tid = tid * 10u + digit;
    }
    if (whole && i == n && n < len) {
        return -EINVAL;
    }
    profile_hold(1);
    k_memset(bkl_profile, 0, sizeof(bkl_profile));
    bkl_profile_samples = 0;
    bkl_profile_lost = 0;
    bkl_profile_whole = whole;
    bkl_profile_tid = whole ? tid : 0;
    profile_hold(0);
    return (int32_t)len;
}

/** Render a fixed copy of the last long holds, oldest first. The sizing
 * call copies the bounded ring with interrupts off; the output call uses
 * that same copy, as audio_proc_text does. Neither call sleeps, and both
 * run under the big lock. */
int32_t smp_long_show(void *data, char *buf, uint32_t cap)
{
    static struct bkl_long_record snap[BKL_LONG_SLOTS];
    static unsigned int count;
    struct bkl_text t;
    unsigned int i, j, first;
    uint32_t flags;

    (void)data;
    if (!buf) {
        flags = kirq_save();
        count = bkl_long_count;
        first = (bkl_long_head + BKL_LONG_SLOTS - count) % BKL_LONG_SLOTS;
        for (i = 0; i < count; i++)
            snap[i] = bkl_long[(first + i) % BKL_LONG_SLOTS];
        kirq_restore(flags);
    }
    t.buf = buf;
    t.cap = cap;
    t.len = 0;
    text_str(&t, "clock_us age_us cpu pid comm vector syscall pc[8]\n");
    for (i = 0; i < count; i++) {
        const struct bkl_long_record *r = &snap[i];

        text_dec(&t, timer_cycles_us(r->clock));
        text_ch(&t, ' ');
        text_dec(&t, timer_cycles_us(r->age));
        text_ch(&t, ' ');
        text_dec(&t, r->cpu);
        text_ch(&t, ' ');
        text_dec(&t, r->pid);
        text_ch(&t, ' ');
        if (!r->comm[0])
            text_ch(&t, '-');
        for (j = 0; j < sizeof(r->comm) && r->comm[j]; j++) {
            unsigned char ch = (unsigned char)r->comm[j];

            text_ch(&t, ch > ' ' && ch <= '~' ? (char)ch : '?');
        }
        text_ch(&t, ' ');
        if (r->nr == BKL_NR_IDLE || r->nr == BKL_NR_SWITCH) {
            text_str(&t, r->nr == BKL_NR_IDLE ? "take 0" : "take 1");
        } else {
            text_dec(&t, r->vector);
            text_ch(&t, ' ');
            if (r->nr < 0)
                text_str(&t, "-1");
            else
                text_dec(&t, (uint32_t)r->nr);
        }
        for (j = 0; j < BKL_SAMPLE_PCS; j++) {
            text_ch(&t, ' ');
            text_hex(&t, r->pc[j]);
        }
        text_ch(&t, '\n');
    }
    return (int32_t)t.len;
}

/** Empty the long-hold ring; `len`. A hold already under way can be
 * sampled anew. The caller holds the big lock. */
int32_t smp_long_store(void *data, const char __user *buf, uint32_t len)
{
    uint32_t flags = kirq_save();

    (void)data;
    (void)buf;
    bkl_long_head = bkl_long_count = 0;
    k_memset(bkl_long_valid, 0, sizeof(bkl_long_valid));
    kirq_restore(flags);
    return (int32_t)len;
}

/**
 * Name the hold `cpu` begins after the entry its thread is in, the mark
 * `bkl_entry_push` left or the call it runs without the lock (`entry_of`),
 * so that a hold an entry run without the lock takes counts under that
 * entry; a thread in none, a kernel thread, names none. An ioctl's hold is
 * charged to its request too. A router that took the lock names its own
 * entry after this (`bkl_note_entry`).
 */
static void bkl_mark_hold(struct percpu *cpu)
{
    const struct process *p = cpu->current;
    uint32_t cmd;
    int key = (int)entry_of(p, &cmd) - 1;

    cpu->bkl_vector = 0;
    cpu->bkl_nr = -1;
    cpu->bkl_ioctl = 0;
    cpu->bkl_unfiltered = !filter_wants(p);
    if (key == BKL_PROFILE_IOCTL) {
        cpu->bkl_vector = BKL_CALL_VECTOR;
        cpu->bkl_nr = p->orig_nr;
        cpu->bkl_ioctl = ioctl_slot(cmd);
    } else if (key >= 0 && key < BKL_ENTRY_SYSCALLS) {
        cpu->bkl_vector = BKL_CALL_VECTOR;
        cpu->bkl_nr = key;
    } else if (key == BKL_KEY_BIGCALL) {
        cpu->bkl_vector = BKL_CALL_VECTOR;
        cpu->bkl_nr = BKL_ENTRY_SYSCALLS;
    } else if (key >= BKL_ENTRY_SYSCALLS && key < BKL_KEY_BIGCALL) {
        cpu->bkl_vector = (uint32_t)(key - BKL_ENTRY_SYSCALLS);
    }
}

void bkl_enter(void)
{
    uint32_t flags = kirq_save();
    struct percpu *cpu = this_cpu();

    if (cpu->bkl_depth == 0) {
        lockdep_bkl_acquire((uintptr_t)(const char *)__builtin_return_address(0));
        bkl_acquire(cpu);
        bkl_mark_hold(cpu);
    }
    cpu->bkl_depth++;
    kirq_restore(flags);
}

int bkl_held(void)
{
    uint32_t flags = kirq_save();
    int held = this_cpu()->bkl_depth > 0;

    kirq_restore(flags);
    return held;
}

/**
 * Whether the running thread may spin for a sleeping lock (`sched_spin_on`):
 * it holds no big lock, or no other processor has claimed it and processor
 * `cpu`, whose thread holds the sleeping lock, is not waiting for it, which
 * that processor marks before its wait (`bkl_acquire`). With `cpu` -1 only
 * the claim counts. A spinner that holds the big lock otherwise keeps it
 * from the thread it waits for, and from every processor that claims it.
 */
int bkl_spin_ok(int cpu)
{
    if (!bkl_held()) {
        return 1;
    }
    return bkl_claim == 0 && (cpu < 0 || cpu >= NR_CPUS || !cpu_table[cpu].wake_bkl_waiting);
}

int bkl_try_enter(void)
{
    struct percpu *cpu = this_cpu();

    if (cpu->bkl_depth == 0) {
        if (bkl_word || bkl_claim || arch_atomic_xchg(&bkl_word, 1u) != 0) {
            return 0;
        }
        cpu->bkl_since = arch_cycles();
        cpu->bkl_waited = 0;
        k_memset(bkl_early_cycles[arch_cpu_id()], 0, sizeof(bkl_early_cycles[0]));
        bkl_mark_hold(cpu);
    }
    cpu->bkl_depth++;
    return 1;
}

/** Let go of the lock this processor holds: give a loan back to its lender,
 * whose word stays set, or clear the word. Interrupts are off. */
static void bkl_release(void)
{
    unsigned int me = (unsigned int)arch_cpu_id() + 1u;

    if (bkl_lent == (me | BKL_LENT_TAKEN)) {
        (void)arch_atomic_xchg(&bkl_lent, 0u);
    } else {
        (void)arch_atomic_xchg(&bkl_word, 0u);
    }
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
        bkl_release();
    }
    kirq_restore(flags);
}

/** The word of frame records `bkl_frame_push` keeps for this processor's
 * entry under way; interrupts are off. */
static uint32_t *bkl_frame_word(void)
{
    struct percpu *cpu = this_cpu();

    return cpu->current ? &cpu->current->bkl_frames : &cpu->bkl_frames;
}

void bkl_frame_push(int locked)
{
    uint32_t *w = bkl_frame_word();

    *w = (*w << 1) | (locked ? 1u : 0u);
}

int bkl_frame_pop(void)
{
    uint32_t *w = bkl_frame_word();
    int locked = (int)(*w & 1u);

    *w >>= 1;
    return locked;
}

int bkl_frame_top(void)
{
    return (int)(*bkl_frame_word() & 1u);
}

int bkl_drop(void)
{
    struct percpu *cpu = this_cpu();
    int depth = cpu->bkl_depth;

    if (depth > 0) {
        bkl_hold_end(cpu);
        cpu->bkl_depth = 0;
        bkl_release();
    }
    return depth;
}

void bkl_take(int depth)
{
    struct percpu *cpu = this_cpu();

    if (depth > 0) {
        lockdep_bkl_acquire((uintptr_t)(const char *)__builtin_return_address(0));
        bkl_acquire(cpu);
        bkl_mark_hold(cpu);
        cpu->bkl_depth = depth;
    }
}

void bkl_take_as(int depth, int key)
{
    struct percpu *cpu = this_cpu();

    if (depth > 0) {
        lockdep_bkl_acquire((uintptr_t)(const char *)__builtin_return_address(0));
        bkl_acquire(cpu);
        bkl_mark_hold(cpu);
        cpu->bkl_vector = 0;
        cpu->bkl_nr = key == BKL_KEY_IDLE ? BKL_NR_IDLE : BKL_NR_SWITCH;
        /* A kernel thread marked in a call has no system call number. */
        cpu->bkl_slept = key == BKL_KEY_SWITCH && cpu->current && cpu->current->in_syscall &&
                                 !cpu->current->kthread_entry
                             ? cpu->current->orig_nr
                             : -1;
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

int bkl_lend(int cpu, const struct process *thread)
{
    unsigned int to = (unsigned int)cpu + 1u, lent;
    struct percpu *them;

    if (this_cpu()->bkl_depth <= 0 || cpu < 0 || cpu >= NR_CPUS || cpu == (int)arch_cpu_id()) {
        return 0;
    }
    them = &cpu_table[cpu];
    if (!them->wake_bkl_waiting || them->current != thread ||
        arch_atomic_cmpxchg(&bkl_lent, 0u, to) != 0u) {
        return 0;
    }
    for (;;) {
        lent = bkl_lent;
        if (lent == 0u) {
            return 1; /* given back */
        }
        if (lent == to && !them->wake_bkl_waiting && arch_atomic_cmpxchg(&bkl_lent, to, 0u) == to) {
            return 0; /* withdrawn: it stopped waiting before it took the loan */
        }
        smp_service();
        arch_relax();
    }
}

int smp_cpu_count(void)
{
    return cpus_online;
}

int smp_serialized(void)
{
    struct percpu *cpu = this_cpu();

    if (k_panicking()) {
        return 1;
    }
    return !cpu->in_nmi && (cpu->bkl_depth > 0 || cpus_online < 2);
}

/**
 * Stop this processor for good: leave VMX or SVM operation first
 * (`k_panic_hook`), stop the watchdog's counter, then mark it stopped,
 * which is what `smp_stop_others` waits for, and halt. Linux's
 * `smp_stop_nmi_callback` and `sysvec_reboot` call
 * `cpu_emergency_disable_virtualization` before `stop_this_cpu` in the same
 * order (arch/x86/kernel/smp.c:121-141), since a processor left in VMX
 * operation blocks INIT, and the restart that follows the stop may need it
 * (arch/x86/kernel/reboot.c:556-599). `stop_this_cpu` masks the counter's
 * APIC entry (`disable_local_APIC`, arch/x86/kernel/process.c:822), since
 * its NMI would wake the halt and the stopped processor, taking no ticks,
 * would be reported as a hard lockup.
 */
static void smp_stop_here(struct percpu *cpu)
{
    if (k_panic_hook) {
        k_panic_hook();
    }
    platform_pmu_disable();
    cpu->online = 0;
    cpu->stopped = 1;
    arch_stop();
}

/**
 * A stop is final. A TLB request is answered by dropping the page's
 * translation, or all of them, and then clearing the word, which is what
 * the sender waits for; the processor does not go on with a translation it
 * was asked to drop, since the sender frees the page once it sees the word
 * clear. A barrier request is answered by the locked exchange that clears
 * it, a full barrier, after a serializing instruction when it asks for one.
 * A function to run is answered by clearing it after a full barrier, so the
 * sender sees all the function wrote once it sees the slot clear.
 * A turn of a wait with interrupts off is also the watchdog's
 * (`watchdog_wait`).
 */
void smp_service(void)
{
    struct percpu *cpu = this_cpu();

    watchdog_wait();

    if (cpu->stop_request) {
        smp_stop_here(cpu);
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
    if (cpu->call_fn) {
        cpu->call_fn(cpu->call_arg);
        sched_mb();
        cpu->call_fn = NULL;
    }
}

void smp_ipi(void)
{
    smp_service();
}

void smp_kick(int cpu)
{
    if (cpu != (int)arch_cpu_id() && cpu_table[cpu].online &&
        arch_atomic_cmpxchg(&cpu_table[cpu].idle_poll, CPU_POLL_ON, CPU_POLL_KICKED) !=
            CPU_POLL_ON) {
        ipi_send(cpu, SMP_IPI_KICK);
    }
}

void smp_kick_many(uint64_t cpus)
{
    uint64_t send = 0;
    int c;

    for (c = 0; cpus != 0; c++, cpus >>= 1) {
        if ((cpus & 1u) && c != (int)arch_cpu_id() && cpu_table[c].online &&
            arch_atomic_cmpxchg(&cpu_table[c].idle_poll, CPU_POLL_ON, CPU_POLL_KICKED) !=
                CPU_POLL_ON) {
            send |= (uint64_t)1 << c;
        }
    }
    ipi_send_many(send, SMP_IPI_KICK);
}

/** Leave processor `c` a request to drop the translations of [`start`,
 * `end`), or every one for CPU_TLB_ALL, for an interrupt the caller sends;
 * `ipi_lock` is held. */
static void tlb_request(int c, uintptr_t start, uintptr_t end)
{
    struct percpu *other = &cpu_table[c];

    other->tlb_page = start;
    other->tlb_end = end;
    other->tlb_request = 1;
}

/** Wait until every processor sent a TLB request has answered it,
 * answering this one's own meanwhile; `ipi_lock` is held. */
static void tlb_wait(void)
{
    int c;

    for (c = 0; c < NR_CPUS; c++) {
        while (cpu_table[c].tlb_request && cpu_table[c].online) {
            smp_service();
            arch_relax();
        }
    }
}

/**
 * A processor that runs `dir` is in user mode, in the kernel with
 * interrupts on, or spinning with them off and answering requests: each
 * answers. While this one waits it answers its own requests, a stop among
 * them. Taking `ipi_lock`, whose exchange is a full barrier, orders the
 * caller's changes to the entries before the reads of each processor's
 * `space` and `current`: a processor that loads `dir` after the read does so
 * with a write of CR3, which serializes, and its walks see the new entries.
 *
 * A processor whose thread has no address space, an idle task or a kernel
 * thread that kept `dir` loaded, is not interrupted: it is marked to load
 * `dir` again at its next switch to one of its threads (`space_stale`), as
 * Linux skips a processor in lazy TLB mode. The switch no longer holds the
 * big lock, so the mark is a handshake: the mark, a full barrier, and a
 * second read of that processor's `current`, against the switch's store of
 * `current`, the full barrier of letting the run queues' lock go, and its
 * read of the mark (`switch_space`, context.c). Either the switch sees the
 * mark and loads `dir` again, or the second read sees the new thread and
 * the processor is interrupted. This spares an idle processor a wake for
 * each page that `fork` or `munmap` changes in a process that last ran
 * there.
 *
 * A processor the hypervisor has preempted is not interrupted either when
 * the hypervisor takes a mark to flush its TLB before it runs again
 * (`arch_cpu_tlb_deferred`), as Linux's kvm_flush_tlb_multi leaves such a
 * vCPU out of the flush (arch/x86/kernel/kvm.c): the interrupt would wait
 * for the host to run that processor, and its flush would come no sooner.
 *
 * The records of other processors' current threads are read under
 * `ipi_lock`, which a record's free takes after the record left every
 * processor (`smp_record_quiesce`), so a record read here stays until the
 * scan ends, whether or not the caller holds the big lock, as a fault's
 * copy-on-write break does not. The caller has changed the pages' entries;
 * it frees a frame they named only after this returns.
 */
void smp_tlb_shootdown(const struct page_dir *dir, uintptr_t start, uintptr_t end)
{
    int me = (int)arch_cpu_id();
    int c, sent = 0;
    uint32_t flags;
    uint64_t send = 0;

    if (cpus_online < 2) {
        return;
    }
    flags = spin_lock_irqsave(&ipi_lock);
    for (c = 0; c < NR_CPUS; c++) {
        struct percpu *other = &cpu_table[c];

        if (c == me || !other->online || other->space != dir) {
            continue;
        }
        if (other->current->space_root == 0) {
            other->space_stale = 1;
            sched_mb();
            if (other->current->space_root == 0) {
                continue;
            }
        }
        if (arch_cpu_tlb_deferred(c)) {
            continue;
        }
        tlb_request(c, start, end);
        send |= (uint64_t)1 << c;
        sent++;
    }
    ipi_send_many(send, SMP_IPI_TLB);
    if (sent) {
        tlb_counts[0]++;
        tlb_counts[1] += (uint32_t)sent;
    }
    tlb_wait();
    spin_unlock_irqrestore(&ipi_lock, flags);
}

void smp_record_quiesce(void)
{
    uint32_t flags = spin_lock_irqsave(&ipi_lock);

    spin_unlock_irqrestore(&ipi_lock, flags);
}

/* The most pages a kernel flush drops one at a time; a longer range drops
 * every translation, as Linux's tlb_single_page_flush_ceiling bounds
 * `flush_tlb_kernel_range` (arch/x86/mm/tlb.c:998, 1109-1125). */
#define KERNEL_FLUSH_PAGES 33u

void smp_kernel_flush(uintptr_t start, uintptr_t end)
{
    int me = (int)arch_cpu_id();
    int c;
    uint32_t flags;
    uint64_t send = 0;

    if (cpus_online < 2) {
        return;
    }
    if (start != CPU_TLB_ALL && end - start > (uintptr_t)KERNEL_FLUSH_PAGES * PAGE_SIZE) {
        start = CPU_TLB_ALL;
    }
    flags = spin_lock_irqsave(&ipi_lock);
    for (c = 0; c < NR_CPUS; c++) {
        if (c != me && cpu_table[c].online) {
            tlb_request(c, start, end);
            send |= (uint64_t)1 << c;
        }
    }
    ipi_send_many(send, SMP_IPI_KERNEL_TLB);
    tlb_wait();
    spin_unlock_irqrestore(&ipi_lock, flags);
}

uint32_t smp_tlb_count(int which)
{
    return which == 0 || which == 1 ? tlb_counts[which] : 0;
}

void smp_barrier(uint64_t cpus, int sync_core)
{
    int me = (int)arch_cpu_id();
    uint32_t flags = spin_lock_irqsave(&ipi_lock);
    int c;

    for (c = 0; c < NR_CPUS; c++) {
        if (c != me && (cpus & ((uint64_t)1 << c)) && cpu_table[c].online) {
            cpu_table[c].barrier_request = sync_core ? SMP_BARRIER_SYNC : SMP_BARRIER;
            ipi_send(c, SMP_IPI_BARRIER);
        }
    }
    for (c = 0; c < NR_CPUS; c++) {
        while (cpu_table[c].barrier_request && cpu_table[c].online) {
            smp_service();
            arch_relax();
        }
    }
    spin_unlock_irqrestore(&ipi_lock, flags);
}

void smp_space_release(const struct page_dir *dir)
{
    int me = (int)arch_cpu_id();
    uint32_t flags;
    int c;

    if (cpus_online < 2) {
        return;
    }
    flags = spin_lock_irqsave(&ipi_lock);
    for (c = 0; c < NR_CPUS; c++) {
        if (c != me && cpu_table[c].online && cpu_table[c].space == dir) {
            cpu_table[c].space_drop = dir;
            ipi_send(c, SMP_IPI_SPACE);
        }
    }
    for (c = 0; c < NR_CPUS; c++) {
        while (cpu_table[c].space_drop && cpu_table[c].online) {
            smp_service();
            arch_relax();
        }
    }
    spin_unlock_irqrestore(&ipi_lock, flags);
}

void smp_call(int cpu, void (*fn)(void *), void *arg)
{
    uint32_t flags;

    if (cpu == (int)arch_cpu_id()) {
        flags = kirq_save();
        fn(arg);
        kirq_restore(flags);
        return;
    }
    if (cpu < 0 || cpu >= NR_CPUS) {
        return;
    }
    flags = spin_lock_irqsave(&ipi_lock);
    if (cpu_table[cpu].online) {
        cpu_table[cpu].call_arg = arg;
        cpu_table[cpu].call_fn = fn;
        ipi_send(cpu, SMP_IPI_CALL);
        while (cpu_table[cpu].call_fn && cpu_table[cpu].online) {
            smp_service();
            arch_relax();
        }
    }
    spin_unlock_irqrestore(&ipi_lock, flags);
}

void smp_kernel_mapping_changed(void)
{
    smp_kernel_flush(CPU_TLB_ALL, 0);
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
            ipi_send(c, SMP_IPI_STOP);
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
    smp_stop_here(cpu);
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
 * registers alone around it. It holds no big lock (`sched_idle_loop`).
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
    p->bkl_depth = 0;
    return p;
}

/**
 * A started processor's first C, on its idle task's stack with interrupts
 * off: it is online at once, so that the boot processor goes on to the next,
 * and takes work once it has had the big lock, which the boot holds until it
 * enters the first program, and let it go again, since the idle task holds
 * none. One that comes after the boot gave up waiting
 * for it finds its `admit` CPU_REFUSED (`smp_start`) and stops: online and
 * uncounted, it would miss the shootdowns, which are skipped while one
 * processor is counted.
 */
static void smp_cpu_main(int cpu)
{
    struct percpu *self = &cpu_table[cpu];

    self->current = self->idle;
    self->idle->on_cpu = 1;
    self->space = _kernel_dir;
    if (arch_atomic_cmpxchg(&self->admit, CPU_STARTING, CPU_ADMITTED) != CPU_STARTING) {
        arch_stop();
    }
    sched_account_start();
    self->online = 1;
    bkl_take(1);
    (void)bkl_drop();
    sched_idle_loop();
}

/* How long the boot waits for a started processor to be online, in
 * microseconds, after the platform has seen it reach its entry. */
#define CPU_ONLINE_WAIT_US 100000

/**
 * The boot processor's idle task is made first, since scheduling needs one on
 * every processor: a process that blocks or ends leaves for it when nothing
 * else is ready. Its stack is laid out for a first switch into
 * `sched_idle_loop`; a started processor runs its own from its start.
 * `maxcpus=N` limits the processors to N, as Linux's does, each occurrence
 * read in turn as its get_option reads it (`kcmdline_option`) into its
 * unsigned setup_max_cpus, so the last that stores a value sets N, and a
 * negative N is no limit; 0 and 1 both mean the boot processor alone.
 * A processor that does not answer is reported, and the next is still
 * tried. The boot admits a processor or refuses it in one
 * compare-and-exchange of its `admit`, which the processor makes too as
 * it comes online, so that one coming as the wait ends is either counted or
 * stopped, never online and uncounted.
 */
void smp_start(void)
{
    int limit = NR_CPUS, max;
    int n, cpu, waited;

    cpu_table[0].idle = smp_idle_task(0);
    if (!cpu_table[0].idle) {
        k_panic("smp: no idle task for the boot processor");
    }
    setup_process_stack(cpu_table[0].idle, sched_idle_loop);
    cpu_table[0].idle->bkl_depth = 0;
    cpu_table[0].online = 1;
    if (kcmdline_option("maxcpus", &max)) {
        limit = (unsigned int)max > NR_CPUS ? NR_CPUS : max < 1 ? 1 : max;
    }
    n = platform_cpu_count(limit);
    paging_temp_split((unsigned int)n);
    for (cpu = 1; cpu < n; cpu++) {
        struct percpu *c = &cpu_table[cpu];

        c->idle = smp_idle_task(cpu);
        if (!c->idle) {
            klog(KLOG_WARN, "smp", "cpu%d: no idle task; not started", cpu);
            break;
        }
        bkl_tally[cpu] = kzalloc(sizeof(struct bkl_tally) * BKL_TALLIES);
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
