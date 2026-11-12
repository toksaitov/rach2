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

/** Count the hold that ends now on `cpu`, which still holds the lock, under
 * its entry and against the longest. The first hold to end, the boot
 * processor's start-up, which nobody waits on, is left out. A take after an
 * idle wait or a switch is recorded as BKL_NR_IDLE or BKL_NR_SWITCH. */
static void bkl_hold_end(struct percpu *cpu)
{
    uint64_t held = arch_cycles() - cpu->bkl_since, waited;
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
    call_hold(&bkl_calls[key], held, waited);
    if (ioctl > 0) {
        call_hold(&bkl_ioctls[ioctl - 1].count, held, waited);
    }
    if (held > bkl_hold_max_cycles) {
        bkl_hold_max_cycles = held;
        bkl_hold_vector = cpu->bkl_vector;
        bkl_hold_nr = nr;
        k_memcpy(bkl_hold_early_cycles, bkl_early_cycles[c], sizeof(bkl_hold_early_cycles));
        k_memcpy(bkl_hold_early_pc, bkl_early_pc[c], sizeof(bkl_hold_early_pc));
    }
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
    if (nr < 0) {
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
    uint64_t wall = arch_cycles() - start;
    struct bkl_tally *t = tally_of(nr < BKL_ENTRY_SYSCALLS ? (int)nr : BKL_KEY_BIGCALL);

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

void bkl_profile_sample(uintptr_t pc, uintptr_t fp)
{
    struct percpu *cpu = this_cpu();
    uintptr_t chain[BKL_SAMPLE_PCS];
    uint32_t entry, cmd;

    if (!bkl_profile_whole && cpu->bkl_depth < 1) {
        return;
    }
    sample_chain(chain, pc, fp, cpu->current ? cpu->current->kernel_stack : 0);
    entry = cpu->current ? cpu->current->bkl_entry : 0;
    cmd = cpu->current ? cpu->current->bkl_cmd : 0;
    spin_lock(&profile_lock);
    if (!bkl_profile_showing) {
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

/** Append the line of `c`: `kind`, `number` (in hex for `hex`) and its
 * counts, the times in microseconds; nothing when `c` counts nothing. */
static void text_count(struct bkl_text *t, const char *kind, uint64_t number, int hex,
                       const struct bkl_call_count *held, int tally)
{
    struct bkl_call_count sum = *held;
    const struct bkl_call_count *c = &sum;
    int cpu;

    for (cpu = 0; cpu < NR_CPUS; cpu++) {
        if (bkl_tally[cpu]) {
            sum.calls += bkl_tally[cpu][tally].calls;
            sum.wall += bkl_tally[cpu][tally].wall;
        }
    }
    if (c->calls == 0 && c->holds == 0) {
        return;
    }
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
    text_ch(t, '\n');
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
    for (key = 0; key < BKL_IOCTL_SLOTS; key++) {
        if (bkl_ioctls[key].used == IOCTL_USED) {
            text_count(&t, "ioctl", bkl_ioctls[key].cmd, 1, &bkl_ioctls[key].count,
                       BKL_ENTRY_KEYS + key);
        }
    }
    text_str(&t, "ioctl_lost ");
    text_dec(&t, bkl_ioctls_lost);
    text_ch(&t, '\n');
    return (int32_t)t.len;
}

int32_t smp_calls_store(void *data, const char __user *buf, uint32_t len)
{
    int i;

    (void)data;
    (void)buf;
    k_memset(bkl_calls, 0, sizeof(bkl_calls));
    for (i = 0; i < BKL_IOCTL_SLOTS; i++) {
        k_memset(&bkl_ioctls[i].count, 0, sizeof(bkl_ioctls[i].count));
    }
    for (i = 0; i < NR_CPUS; i++) {
        if (bkl_tally[i]) {
            k_memset(bkl_tally[i], 0, sizeof(struct bkl_tally) * BKL_TALLIES);
        }
    }
    bkl_ioctls_lost = 0;
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
    char word[6];
    int whole;
    uint32_t i;

    (void)data;
    whole = len >= sizeof(word) && copy_from_user(word, buf, sizeof(word)) == 0;
    for (i = 0; whole && i < sizeof(word); i++) {
        whole = word[i] == kernel[i];
    }
    profile_hold(1);
    k_memset(bkl_profile, 0, sizeof(bkl_profile));
    bkl_profile_samples = 0;
    bkl_profile_lost = 0;
    bkl_profile_whole = whole;
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
 * `bkl_entry_push` left, so that a hold an entry run without the lock takes
 * counts under that entry; a thread in none, a kernel thread, names none.
 * An ioctl's hold is charged to its request too. A router that took the
 * lock names its own entry after this (`bkl_note_entry`).
 */
static void bkl_mark_hold(struct percpu *cpu)
{
    const struct process *p = cpu->current;
    int key = p ? (int)p->bkl_entry - 1 : -1;

    cpu->bkl_vector = 0;
    cpu->bkl_nr = -1;
    cpu->bkl_ioctl = 0;
    if (key == BKL_PROFILE_IOCTL) {
        cpu->bkl_vector = BKL_CALL_VECTOR;
        cpu->bkl_nr = p->orig_nr;
        cpu->bkl_ioctl = ioctl_slot(p->bkl_cmd);
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

/** The word of frame records `bkl_frame_push` keeps for this processor's
 * entry under way; interrupts are off. */
static uint32_t *bkl_frame_word(void)
{
    struct percpu *cpu = this_cpu();

    return cpu->current ? &cpu->current->bkl_frames : &cpu->bkl_frames;
}

void bkl_frame_push(int locked)
{
    uint32_t flags = kirq_save();
    uint32_t *w = bkl_frame_word();

    *w = (*w << 1) | (locked ? 1u : 0u);
    kirq_restore(flags);
}

int bkl_frame_pop(void)
{
    uint32_t flags = kirq_save();
    uint32_t *w = bkl_frame_word();
    int locked = (int)(*w & 1u);

    *w >>= 1;
    kirq_restore(flags);
    return locked;
}

int bkl_frame_top(void)
{
    uint32_t flags = kirq_save();
    int locked = (int)(*bkl_frame_word() & 1u);

    kirq_restore(flags);
    return locked;
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
    if (cpu != (int)arch_cpu_id() && cpu_table[cpu].online) {
        platform_cpu_ipi(cpu);
    }
}

/** Leave processor `c` a request to drop the translations of [`start`,
 * `end`), or every one for CPU_TLB_ALL, and interrupt it; `ipi_lock` is
 * held. */
static void tlb_send(int c, uintptr_t start, uintptr_t end)
{
    struct percpu *other = &cpu_table[c];

    other->tlb_page = start;
    other->tlb_end = end;
    other->tlb_request = 1;
    platform_cpu_ipi(c);
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
 * The caller holds the big lock, which keeps the records of other
 * processors' current threads from being freed while they are read here
 * (`proc_release` takes the lock), and has changed the pages' entries; it
 * frees a frame they named only after this returns.
 */
void smp_tlb_shootdown(const struct page_dir *dir, uintptr_t start, uintptr_t end)
{
    int me = (int)arch_cpu_id();
    int c, sent = 0;
    uint32_t flags;

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
        tlb_send(c, start, end);
        sent++;
    }
    if (sent) {
        tlb_counts[0]++;
        tlb_counts[1] += (uint32_t)sent;
    }
    tlb_wait();
    spin_unlock_irqrestore(&ipi_lock, flags);
}

void smp_kernel_flush(uintptr_t start, uintptr_t end)
{
    int me = (int)arch_cpu_id();
    int c;
    uint32_t flags;

    if (cpus_online < 2) {
        return;
    }
    flags = spin_lock_irqsave(&ipi_lock);
    for (c = 0; c < NR_CPUS; c++) {
        if (c != me && cpu_table[c].online) {
            tlb_send(c, start, end);
        }
    }
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
            platform_cpu_ipi(c);
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
            platform_cpu_ipi(c);
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
        platform_cpu_ipi(cpu);
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
    self->accounted_tick = (uint32_t)kernel_ticks;
    self->online = 1;
    bkl_take(1);
    (void)bkl_drop();
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
    cpu_table[0].idle->bkl_depth = 0;
    cpu_table[0].online = 1;
    if (kcmdline_value("maxcpus", value, sizeof(value))) {
        limit = parse_count(value, NR_CPUS);
        if (limit < 1) {
            limit = 1;
        }
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
