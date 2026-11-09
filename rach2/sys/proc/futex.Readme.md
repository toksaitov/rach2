# Futexes

A futex is a 32-bit word of user memory on which a thread sleeps until another wakes it, the word having held the value the sleeper expected when it went to sleep. C libraries build their mutexes, condition variables and one-time initialization on it, and enter the kernel only when a lock is contended. `futex.c` has the `futex` and `futex_waitv` calls, the robust lists through which a thread's end releases the locks it held, and the rest of a thread's end that concerns futexes, `proc_thread_gone`. The contracts are in [futex(2)](../../../man/man2/futex.2); this document describes how the kernel identifies a futex, files its waiters and keeps their deadlines.

## Interfaces

| Call | i686 | x86-64 | Manual page |
| --- | --- | --- | --- |
| `futex` | 240 | 202 | [futex(2)](../../../man/man2/futex.2) |
| `futex_waitv` | 449 | 449 | [futex(2)](../../../man/man2/futex.2) |
| `set_robust_list` | 311 | 273 | [futex(2)](../../../man/man2/futex.2) |
| `get_robust_list` | 312 | 274 | [futex(2)](../../../man/man2/futex.2) |

* `futex` takes four operations, `FUTEX_WAIT`, `FUTEX_WAKE`, `FUTEX_WAIT_BITSET` and `FUTEX_WAKE_BITSET`, each with `FUTEX_PRIVATE_FLAG`, and the bitset wait with `FUTEX_CLOCK_REALTIME`. The word's address must be aligned to four bytes, and a bitset must not be 0; either is `EINVAL`.
* `futex_waitv` waits on 1 to 128 words at once, each named by a `struct futex_waitv` of Linux's layout, the same in both ABIs, with `FUTEX2_SIZE_U32` and optionally `FUTEX2_PRIVATE`, and an absolute timeout of 64-bit fields on `CLOCK_MONOTONIC` or `CLOCK_REALTIME`. It returns the index of the futex a wake ended it on. The compat table of the x86-64 kernel has the same handler at the same number.
* `set_robust_list` takes the address of the list's head and its size, which must be the size of the head's three words: 12 bytes on i686 and 24 on x86-64.
* `get_robust_list` gives the head of the caller's list or of another thread's, named by its id in the caller's PID namespace: a thread of the caller's process, any thread to a privileged caller, and a thread that is not privileged and whose real or effective user id is the caller's effective one.
* `proc_thread_gone` (`process.h`) runs at a thread's end and at its `execve` ([processes](Readme.md)).
* `futex_restartable` (`futex.h`) tells the system call dispatcher whether a `futex` call a signal interrupted may be restarted.

## Design

### Keys and buckets

A futex is identified by a key of two words (`futex_key`): the address space, which is the page directory of the caller's process, and the user address. The threads of a process share a page directory, and so do a `vfork` child and its parent, and processes made with `CLONE_VM`. A word in a `VM_SHARED` region, shared anonymous memory or a file of `/dev/shm` mapped `MAP_SHARED`, is identified instead by its physical address, the page's frame and the offset in it, with a marker in place of the address space, unless the operation has `FUTEX_PRIVATE_FLAG`. So processes that map the same page identify its futexes alike, as a mutex made `PTHREAD_PROCESS_SHARED` needs.

A waiter's record, `struct futex_q`, is filed in one of `FUTEX_BUCKETS`, 64, lists by a hash of its key, while its thread sleeps on the record's `struct futex_sleeper`, which has a wait queue on which only that thread sleeps. A `futex` wait has one waiter on its kernel stack; a `futex_waitv` call has one waiter for each word, in an array it allocates, all pointing at its one sleeper, so a wake of any of the words wakes the thread. A wake walks one bucket, so it passes over the waiters of other futexes only when their keys hash alike.

### Wait and wake

`futex_wait` and `sys_futex_waitv` first turn a timeout into a deadline (below), and `futex_sleep_on` then makes each word's page resident (`vm_prefault_range`), which may sleep. With interrupts off it then checks that every page is still resident, since bringing in one page may have let another go, and starts again from the faults when one is not. Still with interrupts off it reads each word in order, and returns `EAGAIN` at the first that differs from its expected value, taking off the waiters it filed for the words before; otherwise it files each word's waiter. With interrupts off no fault can come between a read and the filing, and the big lock keeps the kernel code of other processors out, so a wake that comes after the comparison finds the waiter. The thread then sleeps on its sleeper's queue (`wq_wait_event`) until a waker marks one of its waiters, its deadline comes, or a signal arrives, and takes off every waiter that was not marked; the call returns the index of the last marked one, as Linux's `futex_wait_multiple` does.

`futex_wake` marks up to the requested number of matching waiters, a waiter matching when its key is the wake's and its bitset meets the wake's, takes them off the list, and wakes each one's sleeper's queue, so a wake of one makes one thread runnable and the bucket's other sleepers stay asleep. Each wake comes before interrupts are on again: the waiter and its sleeper are in memory the sleeping thread frees after its wait, and the thread cannot leave its wait meanwhile, since the big lock keeps it from running on another processor.

### Deadlines

A deadline is a tick of the kernel's clock, which counts `TICK_HZ` (`timer.h`) a second. A relative timeout, `FUTEX_WAIT`'s, becomes whole ticks, a part of a tick counted as one (`futex_ticks`); an absolute one, `FUTEX_WAIT_BITSET`'s and `futex_waitv`'s, is measured when the call begins against the monotonic clock, or the real-time clock with `FUTEX_CLOCK_REALTIME` or `futex_waitv`'s `CLOCK_REALTIME` (`futex_ticks_until`). `futex_waitv` does not refuse nanoseconds out of range, as Linux does not, and carries their whole seconds into the seconds (`futex_ts_norm`). The deadline is the clock's current tick plus those ticks plus one, so a wait never ends before the time it asked for. It is fixed before the word's page is brought in, so a fault's sleep counts against it, and a deadline that the fault outlasted ends the call with `ETIMEDOUT` once the word is compared. The tick wakes the waiter through the record's `wakeup_tick` ([processes](Readme.md)).

A signal the process acts on ends a wait with `EINTR`. The dispatcher restarts a `futex` wait without a timeout, as it restarts a read, and every `futex_waitv`, whose deadline is absolute; it does not restart a timed `futex` wait (`futex_restartable`): a restart would measure the timeout again from the start, and a handler that ran often enough would keep the wait from ever ending.

### Robust lists and a thread's end

`proc_thread_gone` runs at a thread's end (`proc_exit_cleanup` in `exit.c`) and at its `execve`. An `execve` calls it before a thread that is not its process's leader takes the leader's id, so the id it compares is the one the thread locked its words with. It works in this order:

1. It walks the thread's robust list, as glibc lays it out (`struct robust_head`): the first entry, the offset from an entry to its futex word, and the entry the thread was taking or giving back. At most `ROBUST_LIST_LIMIT`, 2048, entries are walked, so a list with a loop in it ends. Each word that still has the thread's id gets `FUTEX_OWNER_DIED` in its place, with `FUTEX_WAITERS` kept, and one waiter is woken when the word has waiters, as Linux's `handle_futex_death` does; the next owner learns from `EOWNERDEAD` that the state the lock guarded may be half made.
2. It takes the pending entry last. When its word has no owner and it is not a priority-inheritance futex, the thread ended between an unlock and the unlock's wake, or between a wake and taking the lock; one waiter is woken, and the word is left as it is.
3. When another thread shares the address space, it writes 0 to the word given with `CLONE_CHILD_CLEARTID` or `set_tid_address` and wakes one waiter on it, which is how `pthread_join` learns of the end.

A word's page is made present and writable before the word is read. The read, the comparison and the write are then one step with interrupts off, and the write is a compare-and-exchange, read again when the word changed, so a thread on another processor that takes the lock or sets `FUTEX_WAITERS` meanwhile in user mode does not have its change written over.

## Limits

* The requeue, wake-and-modify and priority-inheritance operations answer `ENOSYS`, the second address is ignored, and i686 has no `futex_time64`.
* `FUTEX_CLOCK_REALTIME` is taken with `FUTEX_WAIT_BITSET` alone; with `FUTEX_WAIT` it is `ENOSYS`.
* A deadline is a whole number of the kernel's ticks.
* A timed wait that a signal interrupts is not restarted, also when no handler runs; Linux restarts it then with its first deadline.
* A thread's end walks at most 2048 entries of its robust list.

## Tests

`tests/run default` runs these probes, and `ARCH=x86_64 tests/run default` runs them on x86-64 ([the tests' Readme](../../../tests/Readme.md)):

* `tests/musl/clonemusl.c` waits and wakes within one address space, times out on both clocks no sooner than asked, refuses a bitset of 0 and `FUTEX_CLOCK_REALTIME` with `FUTEX_WAIT`, and checks that a timed wait a handler with `SA_RESTART` interrupts ends with `EINTR`.
* `tests/musl/threadmusl.c` runs mutexes and condition variables among four threads, and a process-shared robust mutex in shared memory that a child's thread holds into an `execve`, which the probe then locks with `EOWNERDEAD`.
* `tests/futexvprobe.c` waits with `futex_waitv` on several words, private and in shared memory, and checks the index a wake returns, `EAGAIN`, both clocks' deadlines and the arguments Linux refuses; `tests/wow64probe.c` makes the same call through `int $0x80`.
* `tests/musl/capmusl.c` checks that a thread without capabilities may not read the robust list of a process that has them.
* `tests/musl/nsmusl.c` checks that the first process of a new PID namespace reads its own robust list as pid 1.

In `tests/run smp`, two threads on two processors take turns through a futex word (`tests/musl/smpmusl.c`). `tests/run host` runs `tests/host/futex_hosttest.c` ([the host tests](../../../tests/host/Readme.md)), which checks what `futex.c` reads of a thread that may end while it works, the robust futexes a thread's end releases, a wait's deadline, and which interrupted calls are restarted.
