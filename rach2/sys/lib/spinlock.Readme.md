# Spin lock

`spinlock.c` is the kernel's spin lock, Linux 6.12.115's queued spin lock (`kernel/locking/qspinlock.c`, `include/asm-generic/qspinlock.h`, `arch/x86/include/asm/qspinlock.h`) without its paravirtual hooks. A free lock is taken with one compare-and-exchange and given back with one store; a lock that is held goes to its waiters in the order they came, and each waiter but the first two spins on a place of its own processor's, not on the lock. Under a hypervisor that may preempt the guest's processors, waiters take the lock by test-and-set instead, as Linux's guest without paravirtual spin locks does. `spinlock.h` gives the rules for using a lock: interrupts off, after the big lock, a short region that does not sleep.

## Interfaces

`spinlock.h` declares the lock and its calls:

* `struct spinlock` is the lock word `locked`, 0 when free, and in the checker's kernel its `lockdep_map`. `SPINLOCK_INIT_NAMED(name)`, `SPINLOCK_INIT` and `spin_lock_init(lock, name)` make a free lock; [the lock-order checker](lockdep.Readme.md) says how each names its class.
* `spin_lock`, `spin_lock_nested(lock, subclass)` and `spin_unlock` take and give back a lock with interrupts already off; `spin_lock_irqsave` turns them off first and `spin_unlock_irqrestore` puts them back.
* `spin_trylock` takes a lock only when its whole word is 0, so it fails while the lock is held or any processor waits for it, as Linux's `queued_spin_trylock`.
* `raw_spin_lock` and `raw_spin_unlock` do the same without the checker, for the checker's own locks.

`arch.h` gives the lock what it needs of the processor: `arch_atomic_cmpxchg`, `arch_store_u8` and `arch_store_u16`, a byte's and a half-word's store after every earlier access of the processor, and `arch_spin_virt`, which says whether waiters take the lock by test-and-set.

No code outside `spinlock.c` reads or writes the word but to make a free lock: `spin_lock_init`, the static initializers and `wait_queue_init_key` store 0, and memory that a record is allocated in zeroed is a free lock too.

## Design

The word is Linux's for fewer than 16,384 processors (`qspinlock_types.h`): byte 0 is the locked byte, byte 1 the pending byte, bits 16-17 the nesting level of the last waiter in the queue and bits 18-31 its processor + 1, 0 for none.

* Take: a compare-and-exchange of 0 to 1. Anything else is the slow path (`q_lock_slow`, Linux's `queued_spin_lock_slowpath`).
* Release: a store of 0 to the locked byte, as x86's `native_queued_spin_unlock`. The waiter that sees it takes the lock; the releaser cannot take it again ahead of it, since its own compare-and-exchange finds the pending bit or the tail.
* Pending: the first waiter of a lock with no queue sets the pending bit and spins on the locked byte. When the holder lets go it takes the lock with one store of the half-word, which sets the locked byte and clears the pending byte together. A taker that finds a hand-over from pending to locked under way waits up to 512 turns for it, as x86's `_Q_PENDING_LOOPS`, before it queues.
* Queue: every later waiter queues. It puts the tail code of its own place in the word with a compare-and-exchange that keeps the low half, links itself behind the place that was the tail, and spins on its place's `locked` until the waiter ahead makes it the head. The head spins on the word until the locked and pending bytes are clear, takes the lock, and makes the place behind it the head; when it is the last, it takes the lock and empties the tail in one compare-and-exchange. Only the pending waiter and the head read the word.
* Places: each processor has four, one for each level of its waits that may nest: a wait with interrupts on, one in an interrupt or in what `smp_service` runs, and one in an NMI, with one to spare, as Linux's `MAX_NODES`. A place is 16 bytes, its link to the next a tail code rather than a pointer, so that a processor's four share one 64-byte line on both architectures. A fifth level spins for the free word, as Linux's does.

A waiter still answers the requests other processors leave it, by calling `smp_service` on every turn, since a holder that drops a translation on every processor waits for this one's answer with the lock held. What that answer runs may take another lock at the next level. Each level records the lock it waits for, and a nested wait for the lock an outer wait of the same processor is for stops with a panic: the queue would never reach it, since the outer place is ahead of it.

Under a hypervisor, leaf 1's ECX bit 31, a waiter tries the free word on each turn instead (`arch_spin_virt`), unless KVM's `KVM_HINTS_REALTIME` says each of the guest's processors has a host processor of its own. This is Linux's `virt_spin_lock`, which a guest uses when it has no paravirtual spin lock (`arch/x86/kernel/paravirt.c:56-60`, `arch/x86/kernel/kvm.c:1120-1166`): in a queue, a waiter whose processor the host has preempted holds up every waiter behind it until the host runs it again. In QEMU with four processors on two host threads, the queue made rows of `tests/musl/mutexmusl.c` 40 to 950 times slower than the test-and-set lock. The first slow path reads the answer, once, since `cpuid` is an exit in a guest.

## Limits

* No paravirtual spin lock. Under KVM, Linux's waiters halt after a bounded spin and the releaser wakes them by hypercall (`kernel/locking/qspinlock_paravirt.h`); Rach2's take the lock by test-and-set there. A halted waiter could not answer `smp_service`'s requests, which the holder may be waiting for.
* A processor that the panic stops inside a wait stays queued, and the lock passes none behind it; the panic path takes no spin lock once the others are stopped (`console_lock` and `smp_serialized` answer at once while panicking).

## Tests

`tests/host/spinlock_hosttest.c` runs `spinlock.c` with each waiting processor a call on a stack of its own that yields at every turn of its wait. It checks the order of hand-over, a try against waiters, a waiter that answers a TLB request while pending, at the head and behind it, waits nested in a wait, a nested wait for the same lock, and the test-and-set waiters under a hypervisor ([the host tests](../../../tests/host/Readme.md)). It runs as the `spinlock` case of `tests/cases/host.host.json`. `tests/host/lockdep_hosttest.c` runs the checker through the lock's calls.
