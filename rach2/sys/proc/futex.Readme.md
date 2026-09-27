# Futexes

`futex.c` implements `futex(2)`, system call 240, for waiters and wakers in one address space: `FUTEX_WAIT`, `FUTEX_WAKE`, `FUTEX_WAIT_BITSET` and `FUTEX_WAKE_BITSET`, with `FUTEX_PRIVATE_FLAG` accepted and `FUTEX_CLOCK_REALTIME` for the bitset wait. `man/man2/futex.2` gives the user contract.

A waiter files a `struct futex_q` on its own kernel stack in one of 64 buckets, hashed from its key: the address space, which is the process's page directory, and the user address. It reads the word after making its page resident, then compares it and files itself with interrupts off, so a wake between the comparison and the sleep finds it. It then sleeps on the bucket's wait queue until a waker marks it, its deadline tick comes, or a signal arrives; the waiter that was not woken takes itself off the list. A wake marks up to the requested number of matching waiters, takes them off the list, and wakes the bucket's queue, whose other sleepers find themselves unmarked and sleep again.

Deadlines are whole 10 ms ticks, rounded up, plus the tick in progress. A relative timeout is measured from the call; an absolute one is converted against the monotonic or real-time clock when the call begins.

A key names an address space by its page directory, so a waiter and its waker are the same process or processes that share one address space, as `vfork` and `clone` with `CLONE_VM` make them. A futex in memory two processes share needs a key made from the shared page instead, and the requeue, wake-and-modify and priority-inheritance operations answer `ENOSYS`.
