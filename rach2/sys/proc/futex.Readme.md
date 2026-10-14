# Futexes

A futex is a 32-bit word of user memory on which a thread sleeps until another wakes it, the word having held the value the sleeper expected when it went to sleep. C libraries build their mutexes, condition variables and one-time initialization on it, and enter the kernel only when a lock is contended. `futex.c` has the `futex` call, the robust lists through which a thread's end releases the locks it held, and the rest of a thread's end that concerns futexes, `proc_thread_gone`. The contracts are in [futex(2)](../../../man/man2/futex.2); this document describes how the kernel names a futex, files its waiters and keeps their deadlines.

## Interfaces

| Call | i686 | x86-64 | Manual page |
| --- | --- | --- | --- |
| `futex` | 240 | 202 | [futex(2)](../../../man/man2/futex.2) |
| `set_robust_list` | 311 | 273 | [futex(2)](../../../man/man2/futex.2) |
| `get_robust_list` | 312 | 274 | [futex(2)](../../../man/man2/futex.2) |

* `futex` takes four operations, `FUTEX_WAIT`, `FUTEX_WAKE`, `FUTEX_WAIT_BITSET` and `FUTEX_WAKE_BITSET`, each with `FUTEX_PRIVATE_FLAG`, and the bitset wait with `FUTEX_CLOCK_REALTIME`. The word's address must be aligned to four bytes, and a bitset must not be 0; either is `EINVAL`.
* `set_robust_list` takes the address of the list's head and its size, which must be the size of the head's three words: 12 bytes on i686 and 24 on x86-64.
* `get_robust_list` gives the head of the caller's list or of another thread's: a thread of the caller's process, any thread to a privileged caller, and a thread that is not privileged and whose real or effective user id is the caller's effective one.
* `proc_thread_gone` (`process.h`) runs at a thread's end and at its `execve` ([processes](Readme.md)).
* `futex_restartable` (`futex.h`) tells the system call dispatcher whether a `futex` call a signal interrupted may be restarted.

## Design

### Keys and buckets

A futex is named by a key of two words (`futex_key`): the address space, which is the page directory of the caller's process, and the user address. The threads of a process share a page directory, and so do a `vfork` child and its parent, and processes made with `CLONE_VM`. A word in a `VM_SHARED` region, shared anonymous memory or a file of `/dev/shm` mapped `MAP_SHARED`, is named instead by its physical address, the page's frame and the offset in it, with a marker in place of the address space, unless the operation has `FUTEX_PRIVATE_FLAG`. So processes that map the same page name its futexes alike, as a mutex made `PTHREAD_PROCESS_SHARED` needs.

A waiter's record, `struct futex_q`, is on its own kernel stack while it sleeps, filed in one of `FUTEX_BUCKETS`, 64, lists by a hash of its key. Each bucket has a wait queue its waiters sleep on. A wake walks one bucket, so it passes over the waiters of other futexes only when their keys hash alike.

### Wait and wake

`futex_wait` first turns a timeout into a deadline (below), then makes the word's page resident (`vm_prefault_range`), which may sleep. With interrupts off it then reads the word, returns `EAGAIN` when the word differs from the expected value, and otherwise files the waiter. With interrupts off no fault can come between the read and the filing, and the big lock keeps the kernel code of other processors out, so a wake that comes after the comparison finds the waiter. The waiter then sleeps on the bucket's queue (`wq_wait_event`) until a waker marks it, its deadline comes, or a signal arrives, and a waiter that was not marked takes itself off the list.

`futex_wake` marks up to the requested number of matching waiters, a waiter matching when its key is the wake's and its bitset meets the wake's, takes them off the list, and wakes the bucket's queue. The bucket's other sleepers find themselves unmarked and sleep again.

### Deadlines

A deadline is a tick of the kernel's 100 Hz clock. A relative timeout, `FUTEX_WAIT`'s, becomes whole 10 ms ticks, a part of a tick counted as one (`futex_ticks`); an absolute one, `FUTEX_WAIT_BITSET`'s, is measured when the call begins against the monotonic clock, or the real-time clock with `FUTEX_CLOCK_REALTIME` (`futex_ticks_until`). The deadline is the clock's current tick plus those ticks plus one, so a wait never ends before the time it asked for. It is fixed before the word's page is brought in, so a fault's sleep counts against it, and a deadline that the fault outlasted ends the call with `ETIMEDOUT` once the word is compared. The tick wakes the waiter through the record's `wakeup_tick` ([processes](Readme.md)).

A signal the process acts on ends a wait with `EINTR`. The dispatcher restarts a wait without a timeout, as it restarts a read, and does not restart a timed one (`futex_restartable`): a restart would measure the timeout again from the start, and a handler that ran often enough would keep the wait from ever ending.

### Robust lists and a thread's end

`proc_thread_gone` runs at a thread's end (`proc_exit_cleanup` in `exit.c`) and at its `execve`. An `execve` calls it before a thread that is not its process's leader takes the leader's id, so the id it compares is the one the thread locked its words with. It works in this order:

1. It walks the thread's robust list, as glibc lays it out (`struct robust_head`): the first entry, the offset from an entry to its futex word, and the entry the thread was taking or giving back. At most `ROBUST_LIST_LIMIT`, 2048, entries are walked, so a list with a loop in it ends. Each word that still has the thread's id gets `FUTEX_OWNER_DIED` in its place, with `FUTEX_WAITERS` kept, and one waiter is woken when the word has waiters, as Linux's `handle_futex_death` does; the next owner learns from `EOWNERDEAD` that the state the lock guarded may be half made.
2. It takes the pending entry last. When its word has no owner and it is not a priority-inheritance futex, the thread ended between an unlock and the unlock's wake, or between a wake and taking the lock; one waiter is woken, and the word is left as it is.
3. When another thread shares the address space, it writes 0 to the word that `CLONE_CHILD_CLEARTID` or `set_tid_address` named and wakes one waiter on it, which is how `pthread_join` learns of the end.

A word's page is made present and writable before the word is read. The read, the comparison and the write are then one step with interrupts off, and the write is a compare-and-exchange, read again when the word changed, so a thread on another processor that takes the lock or sets `FUTEX_WAITERS` meanwhile in user mode does not have its change written over.

## Limits

* The requeue, wake-and-modify and priority-inheritance operations answer `ENOSYS`, the second address is ignored, and i686 has no `futex_time64`.
* `FUTEX_CLOCK_REALTIME` is taken with `FUTEX_WAIT_BITSET` alone; with `FUTEX_WAIT` it is `ENOSYS`.
* A deadline is a whole number of 10 ms ticks.
* A timed wait that a signal interrupts is not restarted, also when no handler runs; Linux restarts it then with its first deadline.
* A thread's end walks at most 2048 entries of its robust list.
* `get_robust_list` takes a thread's kernel id, its id in the first PID namespace, and not its id in the caller's PID namespace.

## Tests

`tests/run default` runs these probes, and `ARCH=x86_64 tests/run default` runs them on x86-64 ([the tests' Readme](../../../tests/Readme.md)):

* `tests/musl/clonemusl.c` waits and wakes within one address space, times out on both clocks no sooner than asked, refuses a bitset of 0 and `FUTEX_CLOCK_REALTIME` with `FUTEX_WAIT`, and checks that a timed wait a handler with `SA_RESTART` interrupts ends with `EINTR`.
* `tests/musl/threadmusl.c` runs mutexes and condition variables among four threads, and a process-shared robust mutex in shared memory that a child's thread holds into an `execve`, which the probe then locks with `EOWNERDEAD`.
* `tests/musl/capmusl.c` checks that a thread without capabilities may not read the robust list of a process that has them.

`tests/system/probe_faults.txt` has the faults these probes must catch: `futex-timed-restart`, `futex-relative-nsec`, `futex-absolute-nsec`, `futex-wait-never-sleeps`, `exec-robust-tid` and `robust-list-ignores-privilege`; `sh tests/system/probe_faults.sh NAME` runs one.

In `tests/run smp`, two threads on two processors take turns through a futex word (`tests/musl/smpmusl.c`). `tests/run host` runs `tests/host/futex_hosttest.c` ([the host tests](../../../tests/host/Readme.md)), which checks what `futex.c` reads of a thread that may end while it works, the robust futexes a thread's end releases, a wait's deadline, and which interrupted calls are restarted.
