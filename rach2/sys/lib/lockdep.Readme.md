# Lock-order checker

`lockdep.c` is the lock-order checker of the debug kernel, after Linux 6.12.115's `kernel/locking/lockdep.c`. It sorts every lock into a class, learns the order in which classes are taken, and reports, once for each pair, an order that could deadlock: a cycle, a lock taken twice, a lock taken in an interrupt and elsewhere with interrupts on, a sleep or a sleeping lock under a spinlock or with interrupts off, and the big lock taken under a spinlock. The kernel goes on after a report. It is built only into the `rach2-lockdep` kernel; in every other kernel each hook is an empty macro and a lock carries no map, so their code and data do not change.

## Use

Build the kernel with `make rach2-lockdep`, for `ARCH=i686` or `ARCH=x86_64`, and boot it in place of `kernel`. Read the reports from the kernel log, `dmesg` or the serial port, under the subsystem `lockdep`, and the checker's state from `/proc/lockdep` and `/proc/lockdep_stats`, which only this kernel has; [proc(5)](../../../man/man5/proc.5) gives their format. A write of `abba` or `irq` to debugfs's `lockdep/inject` makes the checker report a possible deadlock or an interrupt inversion on locks of its own, to show that it reports ([debugfs(5)](../../../man/man5/debugfs.5)).

## Results

A report is one line that names the fault and the two places involved, the processor and the process, then the orders that make it, then the locks the context holds:

```
lockdep: possible deadlock: B at 0x... under A at 0x..., against the order before, cpu 0 pid 1
lockdep: order: B at 0x..., then A at 0x...
lockdep: held: A at 0x...
```

The first lines of the other reports:

* `recursive lock: X at .., already held at ..`: a class taken while held, without a subclass (`check_deadlock`, lockdep.c:3081).
* `lock taken in an interrupt and with interrupts on: X in an interrupt at .., with interrupts on at ..` (`mark_lock_irq` and `valid_state`, lockdep.c:4024).
* `interrupt inversion: I, taken in an interrupt at .., is taken before E, held with interrupts on at ..`: an order that lets an interrupt wait on a lock its own processor holds (`check_irq_usage`, lockdep.c:2839).
* `sleep at .. under spinlock S at ..`, `sleeping lock M at .. under spinlock S at ..`, and the same with `in an interrupt` or `with interrupts off`.
* `big lock at .. under spinlock S at ..`.
* `lock without a class: ADDR taken at .., keyed by its address`: a lock in allocated memory that no `spin_lock_init` reached.
* `release of a lock not held: X at ..` and `lock not held: X at ..`, the second from `lockdep_assert_held`.
* `off: the class pool is full`, or the order pool, the chain table, or too many locks held: the checker stops, once.

A class prints as its name, `name/N` at subclass N, and `lock@0xKEY` when it has none. An address is the instruction after the call that took the lock; `addr2line` on the kernel's symbols gives the line.

## Interfaces

`lockdep.h` declares the checker; `spinlock.h` calls it.

* `SPINLOCK_INIT_NAMED(name)` initialises a static spinlock, whose class is its own address, named `name`; `SPINLOCK_INIT` leaves it unnamed. `spin_lock_init(lock, name)` initialises a spinlock in an allocated record; every lock it initialises at one site belongs to one class, as Linux's `spin_lock_init` (include/linux/spinlock.h:331), since the macro declares a key at the site.
* `spin_lock`, `spin_lock_nested(lock, n)`, `spin_trylock` and `spin_unlock` call `lockdep_acquire` and `lockdep_release`; `raw_spin_lock` and `raw_spin_unlock` do not, for the checker's own lock. Two locks of one class nest only at different subclasses (`spin_lock_nested`, include/linux/spinlock.h:221), each of which is a class of its own.
* `lockdep_assert_held(lock)` reports, once for each site, a caller that does not hold the spinlock (include/linux/lockdep.h:285). `bkl_assert_held()` panics when the processor does not hold the big lock.
* `lockdep_might_sleep(ip)`, `lockdep_bkl_acquire(ip)` and `lockdep_acquire` with `LOCKDEP_SLEEPING` are the hooks for sleeps, the big lock and sleeping locks. `kirq_restore` calls `lockdep_irqs_on` when it turns interrupts on.
* `lockdep_proc_text` and `lockdep_stats_text` write the two files' texts, which `fs/procfs.c` serves.

Each processor's `struct percpu` holds the spinlocks it holds, `lockdep_spin`, and each process's `struct process` the sleeping locks it holds, `lockdep`; neither field exists outside this kernel.

## Design

### Classes

A static lock, one in the kernel image between `_data` and `_image_end`, is keyed by its own address, as Linux's `static_obj` (lockdep.c:824, used at 999). Any other lock is keyed by its init site's `struct lockdep_key`. The first take of a lock finds or makes its class in a hash of `LOCKDEP_HASH` buckets (`register_lock_class`, lockdep.c:1276) and caches its index in the map, plus one, for subclass 0. A lock with neither a key nor a static address is reported once and keyed by its own address, so a missed init site shows on the first boot that takes it.

### Orders and chains

Each take is checked against the locks its context holds. The context is the interrupt's spinlocks in an interrupt, and otherwise the process's sleeping locks followed by the processor's spinlocks taken outside interrupts (`check_prevs_add`, lockdep.c:3285, which splits by `irq_context` at 3304). The held locks' classes are hashed twice, 32 bits each, into a chain key; a chain seen before is not checked again (`validate_chain`, lockdep.c:3885). The chain table is read without the graph's lock and filled under it, and stops taking chains at three quarters full, so its open addressing keeps short probes.

For a new chain the checker first looks for the new class among the held locks: one found is a recursion, and since a subclass is a class of its own, two locks of one class nest only at different subclasses. Otherwise it adds an order to the new class from the innermost held lock, and from each lock further out while the lock inside it was taken by a trylock, as `check_prevs_add` does. An order A to B is added only when no path of orders leads from B back to A; the search is breadth-first over the order lists (`__bfs`, lockdep.c:1705; `check_noncircular`, 2208; `check_prev_add`, 3146), and a path found is printed, up to `LOCKDEP_PATH` orders. A trylock that succeeds is noted as held but is not checked and adds no order to itself, since it does not wait.

### Interrupts

A class gets `LD_USED_IRQ` when taken in an interrupt and `LD_ON_IRQ` when held with interrupts on, either at its take or when `kirq_restore` turns interrupts on under it (`mark_held_locks`, lockdep.c:4344). A class with both is reported. When a class gains `LD_USED_IRQ`, the checker searches the orders from it for a class held with interrupts on, and when it gains `LD_ON_IRQ`, the orders to it for a class used in interrupts (`mark_lock_irq`, lockdep.c:4024). When a new order is added, the checker searches backward from the held class for one used in interrupts and forward from the new class for one held with interrupts on, and reports the pair (`check_usage_backwards` and `check_usage_forwards`, lockdep.c:3966-4021).

### Storage and reentry

Classes, orders, chains and remembered reports are static pools in .bss: `LOCKDEP_CLASSES` 1024, `LOCKDEP_EDGES` 8192, `LOCKDEP_CHAINS` 16384 and `LOCKDEP_ONCE` 512, with `LOCKDEP_DEPTH` 32 locks held by one context. When one is full the checker reports which and stops for good; `/proc/lockdep_stats` keeps the reason.

The graph is guarded by `graph_lock`, a raw spinlock the checker does not check. A hook runs with interrupts off and with the processor's `lockdep_busy` count raised, and does nothing when the count is already raised, in an NMI, or once the checker has stopped (`lockdep_recursion`).

### Reports

A hook queues at most `LOCKDEP_QUEUE` reports while it holds the graph's lock and prints them after letting it go. Each report is made once for its kind and its pair of classes or its site.

The reports go through `klog_report_begin`, `klog_report` and `klog_report_end`, which write the log's ring under `ring_lock` at once, where an ordinary record made off the big lock is deferred, and the serial port under klog's report owner, waited for at most `KLOG_REPORT_WAIT_MS`, 100 ms. A processor that cannot have the port in that time, or that already has it because the report was raised in its own write, leaves the report in the ring alone; a panic writes the port without waiting.

## Building

`make rach2-lockdep` builds `rach2` again with `COMPONENT=rach2-lockdep KEXTRA=-DRACH2_LOCKDEP`, into a directory of its own, and prints the kernel's size, `rach2-lockdep: size: N bytes`. The Lenovo's 4 MiB kernel slots take a kernel up to 4,194,304 bytes; at 84-1 the x86-64 lockdep kernel was 3,770,008 bytes and the i686 one 3,041,236.

## Limits

* Spinlocks, the big lock, `struct mutex`, `struct rwsem` and `struct sleep_lock` are checked, and `schedule` reports a switch made with a spinlock held as a sleep under it (`lockdep_switch`). An interrupt is the depth `irq_enter` keeps (`in_interrupt`), so code an interrupt runs before `irq_enter` counts as the interrupted context.
* There are no read locks and no softirq states: the kernel has neither.
* Only `kirq_restore` tells the checker that interrupts come on; a raw `sti` elsewhere goes unseen, so a lock it uncovers is not marked as held with interrupts on.
* Code in an NMI is not checked.
* Reports go to the ring and the serial port, not to the screen or netconsole.

## Tests

`tests/host/lockdep_hosttest.c` compiles `lockdep.c` and `spinlock.c` on the host with `-DRACH2_LOCKDEP` and checks an AB-BA order, a cycle of three, an interrupt-inconsistent lock, an interrupt inversion, sleeps under a spinlock, recursion and subclasses, trylocks, misuse, the text of `/proc/lockdep` and `/proc/lockdep_stats`, and a full pool. It runs as the `lockdep` case of `tests/cases/host.host.json`.
