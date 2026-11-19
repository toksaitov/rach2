/* The spinlock's acquire and release: Linux's queued spinlock. */
#include "spinlock.h"

#include "kirq.h"
#include "../arch/arch.h"
#include "../panic.h"
#include "../proc/smp.h"

/* The instruction that called the function this is used in, for the
 * checker's reports. */
#define SPIN_CALLER ((uintptr_t)(const char *)__builtin_return_address(0))

/*
 * The lock word, as Linux's (include/asm-generic/qspinlock_types.h with
 * NR_CPUS below 16384): byte 0 is the locked byte, byte 1 the pending byte,
 * bits 16-17 the tail's nesting level and bits 18-31 the tail's processor +
 * 1. The tail names the last waiter in the queue, 0 for none. The locked
 * byte is the word's first, the pending byte its second, as on every
 * little-endian processor; the unlock and the pending waiter's hand-over
 * store them alone.
 */
#define Q_LOCKED              0x1u
#define Q_LOCKED_MASK         0xffu
#define Q_PENDING             0x100u
#define Q_PENDING_MASK        0xff00u
#define Q_LOCKED_PENDING_MASK 0xffffu
#define Q_TAIL_MASK           0xffff0000u
#define Q_TAIL_IDX_OFFSET     16
#define Q_TAIL_IDX_MASK       0x3u
#define Q_TAIL_CPU_OFFSET     18

/* The turns a taker waits for a pending waiter's hand-over before it
 * queues, as x86's _Q_PENDING_LOOPS (arch/x86/include/asm/qspinlock.h:11). */
#define Q_PENDING_LOOPS 512u

/* The levels of waits a processor may nest, as Linux's MAX_NODES
 * (kernel/locking/qspinlock.c:70): a wait with interrupts on, one in an
 * interrupt or in `smp_service`, and one in an NMI, with one to spare. */
#define Q_LEVELS 4u

/*
 * A waiter's place in a lock's queue, Linux's mcs_spinlock
 * (kernel/locking/mcs_spinlock.h:18). `next` names the place behind by its
 * tail code, so that the place is 16 bytes on both architectures and a
 * processor's four share one 64-byte line.
 */
struct qnode {
    volatile unsigned int next;     /* the tail code of the waiter behind, 0 for none */
    volatile unsigned int locked;   /* 1 once the waiter ahead made this the queue's head */
    struct spinlock *volatile lock; /* the lock this level waits for, NULL when free */
};

/* A place padded to 16 bytes, which i686's pointer leaves 4 short of. */
union qslot {
    struct qnode node;
    char pad[16];
};

/* A processor's places, one for each level of its waits, in a 64-byte line
 * of their own, and the levels in use, which only the processor writes, in
 * the next. */
struct qcpu {
    union qslot slot[Q_LEVELS];
    volatile unsigned int depth;
    char pad[60];
};

static struct qcpu qcpus[NR_CPUS] __attribute__((aligned(64)));

/* Whether waiters take the lock by test-and-set (`arch_spin_virt`): -1
 * until the first slow path reads it, then 0 or 1. Every processor reads
 * the same, so two that read it at once store the same. */
static volatile int q_virt = -1;

/** The tail code of processor `cpu`'s place at level `level`. */
static unsigned int q_tail(unsigned int cpu, unsigned int level)
{
    return ((cpu + 1u) << Q_TAIL_CPU_OFFSET) | (level << Q_TAIL_IDX_OFFSET);
}

/** The place a tail code names. */
static struct qnode *q_node(unsigned int tail)
{
    unsigned int cpu = (tail >> Q_TAIL_CPU_OFFSET) - 1u;
    unsigned int level = (tail >> Q_TAIL_IDX_OFFSET) & Q_TAIL_IDX_MASK;

    return &qcpus[cpu].slot[level].node;
}

/**
 * A turn of a wait: answer the requests other processors left this one,
 * since interrupts are off and a holder that drops a translation on every
 * processor waits for this one's answer, then tell the processor it spins.
 */
static void q_turn(void)
{
    smp_service();
    arch_relax();
}

/** Take `lock` when its whole word is 0, free with no waiter, as Linux's
 * queued_spin_trylock (include/asm-generic/qspinlock.h:90): 1 when taken. */
static int q_trylock(struct spinlock *lock)
{
    return lock->locked == 0 && arch_atomic_cmpxchg(&lock->locked, 0, Q_LOCKED) == 0;
}

/** Set the pending byte and return the word before, as Linux's
 * queued_fetch_set_pending_acquire. */
static unsigned int q_fetch_set_pending(struct spinlock *lock)
{
    unsigned int old = lock->locked, was;

    while ((was = arch_atomic_cmpxchg(&lock->locked, old, old | Q_PENDING)) != old) {
        old = was;
    }
    return old;
}

/** Put `tail` in the word, keeping its locked and pending bytes, and return
 * the word before, as Linux's xchg_tail (qspinlock.c:221, the form for a
 * word whose halves have no exchange of their own). */
static unsigned int q_xchg_tail(struct spinlock *lock, unsigned int tail)
{
    unsigned int old = lock->locked, was;

    while ((was = arch_atomic_cmpxchg(&lock->locked, old, (old & Q_LOCKED_PENDING_MASK) | tail)) !=
           old) {
        old = was;
    }
    return old;
}

/** Give back the level of this processor's waits that `slot` is: no wait
 * nested in it reads its lock again. */
static void q_leave(struct qcpu *me, struct qnode *slot)
{
    if (slot) {
        slot->lock = NULL;
    }
    me->depth--;
}

/**
 * Take `lock` after the fast path found it held or waited for: Linux's
 * queued_spin_lock_slowpath (kernel/locking/qspinlock.c:313-563), without
 * its paravirtual hooks. `val` is the word the fast path read.
 *
 * The first waiter sets the pending bit and spins on the locked byte; the
 * holder's release lets it take the lock with one store, which clears the
 * pending bit with it. A waiter that finds the pending bit or a queue joins
 * the queue: it puts its place's code in the tail and links itself behind
 * the place that was there, and spins on its own place's `locked` until the
 * one ahead makes it the head. The head spins on the word until the locked
 * byte and the pending bit are clear, takes the lock, and makes the place
 * behind it the head. The lock goes to its waiters in the order they came,
 * and no waiter but the pending one and the head reads the word.
 *
 * Every level of a processor's waits records its lock, so that a wait that
 * nests in another, in `smp_service` or in an NMI, and asks for the lock
 * the outer wait is for, stops with a panic: the queue would never reach
 * it, since its own outer place is ahead of it.
 *
 * Under a hypervisor that may preempt the guest's processors
 * (`arch_spin_virt`), a waiter instead tries the free word on each turn,
 * as Linux's guest without paravirtual spin locks does: a queue there
 * stalls every waiter behind one whose processor the host preempted.
 */
static void q_lock_slow(struct spinlock *lock, unsigned int val)
{
    unsigned int cpu = arch_cpu_id();
    struct qcpu *me = &qcpus[cpu];
    struct qnode *node = NULL, *prev;
    unsigned int level, i, tail, old, next, n;

    if (q_virt < 0) {
        q_virt = arch_spin_virt();
    }
    if (q_virt) {
        /* Linux's virt_spin_lock (arch/x86/include/asm/qspinlock.h:88-110):
         * a waiter whose processor the host preempted would hold up every
         * waiter behind it in the queue. */
        while (!q_trylock(lock)) {
            q_turn();
        }
        return;
    }
    level = me->depth++;
    k_barrier();
    if (level < Q_LEVELS) {
        node = &me->slot[level].node;
        for (i = 0; i < level; i++) {
            if (me->slot[i].node.lock == lock) {
                k_panic("spinlock: a wait nested in another asked for the same lock");
            }
        }
        node->lock = lock;
    } else {
        /* A fifth level, which nested NMIs alone could make, spins on the
         * word as Linux's does (qspinlock.c:413-418). */
        while (!q_trylock(lock)) {
            q_turn();
        }
        q_leave(me, NULL);
        return;
    }

    /* A hand-over from the pending waiter to the lock is under way: wait a
     * bounded time for it (0,1,0 -> 0,0,1). */
    if (val == Q_PENDING) {
        for (n = Q_PENDING_LOOPS; n > 0 && (val = lock->locked) == Q_PENDING; n--) {
            arch_relax();
        }
    }
    if ((val & ~Q_LOCKED_MASK) == 0) {
        val = q_fetch_set_pending(lock);
        if ((val & ~Q_LOCKED_MASK) == 0) {
            /* Pending: wait for the holder, then take the lock and clear
             * the pending bit in one store (0,1,1 -> 0,1,0 -> 0,0,1). */
            while ((lock->locked & Q_LOCKED_MASK) != 0) {
                q_turn();
            }
            arch_store_u16((volatile unsigned short *)&lock->locked, (unsigned short)Q_LOCKED);
            q_leave(me, node);
            return;
        }
        /* Another came first: undo a pending bit this one set and queue. */
        if ((val & Q_PENDING_MASK) == 0) {
            arch_store_u8((volatile unsigned char *)&lock->locked + 1, 0);
        }
    }

    node->locked = 0;
    node->next = 0;
    if (q_trylock(lock)) {
        q_leave(me, node);
        return;
    }
    tail = q_tail(cpu, level);
    old = q_xchg_tail(lock, tail);
    next = 0;
    if ((old & Q_TAIL_MASK) != 0) {
        prev = q_node(old & Q_TAIL_MASK);
        prev->next = tail;
        while (!node->locked) {
            q_turn();
        }
        next = node->next;
    }

    /* The head: wait for the holder and the pending waiter to go. */
    while (((val = lock->locked) & Q_LOCKED_PENDING_MASK) != 0) {
        q_turn();
    }
    /* The last in the queue takes the lock and empties the tail in one
     * step; a waiter that came meanwhile makes the step fail. */
    if ((val & Q_TAIL_MASK) == tail && arch_atomic_cmpxchg(&lock->locked, val, Q_LOCKED) == val) {
        q_leave(me, node);
        return;
    }
    arch_store_u8((volatile unsigned char *)&lock->locked, (unsigned char)Q_LOCKED);
    while ((next = node->next) == 0) {
        q_turn();
    }
    q_node(next)->locked = 1;
    q_leave(me, node);
}

/**
 * The fast path is one compare-and-exchange of a free word to locked, as
 * Linux's queued_spin_lock (include/asm-generic/qspinlock.h:107); anything
 * else is the slow path's.
 */
void raw_spin_lock(struct spinlock *lock)
{
    unsigned int val = arch_atomic_cmpxchg(&lock->locked, 0, Q_LOCKED);

    if (val != 0) {
        q_lock_slow(lock, val);
    }
}

/** The release is one store of 0 to the locked byte, as Linux's
 * native_queued_spin_unlock (arch/x86/include/asm/qspinlock.h:44); the
 * pending waiter or the queue's head sees it. */
void raw_spin_unlock(struct spinlock *lock)
{
    arch_store_u8((volatile unsigned char *)&lock->locked, 0);
}

void spin_lock(struct spinlock *lock)
{
    lockdep_acquire(&lock->dep, 0, LOCKDEP_SPIN, SPIN_CALLER);
    raw_spin_lock(lock);
}

void spin_lock_nested(struct spinlock *lock, unsigned int subclass)
{
    lockdep_acquire(&lock->dep, subclass, LOCKDEP_SPIN, SPIN_CALLER);
    (void)subclass;
    raw_spin_lock(lock);
}

/** A waiter in the queue or pending makes the try fail, as Linux's, so a
 * try never takes the lock ahead of a waiter. */
int spin_trylock(struct spinlock *lock)
{
    if (!q_trylock(lock)) {
        return 0;
    }
    lockdep_acquire(&lock->dep, 0, LOCKDEP_TRY, SPIN_CALLER);
    return 1;
}

void spin_unlock(struct spinlock *lock)
{
    lockdep_release(&lock->dep, SPIN_CALLER);
    raw_spin_unlock(lock);
}

uint32_t spin_lock_irqsave(struct spinlock *lock)
{
    uint32_t flags = kirq_save();

    lockdep_acquire(&lock->dep, 0, LOCKDEP_SPIN, SPIN_CALLER);
    raw_spin_lock(lock);
    return flags;
}

void spin_unlock_irqrestore(struct spinlock *lock, uint32_t flags)
{
    lockdep_release(&lock->dep, SPIN_CALLER);
    raw_spin_unlock(lock);
    kirq_restore(flags);
}
