# Milestone 46: memory

The kernel allocates physical frames beyond the old 32 MiB limit, frees kernel objects when their users finish with them, and reclaims cached data when memory runs short. Resource limits bound a user's processes and a process's descriptors and address space. The milestone also allocates device message vectors and adds probes for capacity, allocation failure, memory pressure and delay under load.

## What is built

`frame.c` builds the physical memory map from Multiboot entries in 64-bit arithmetic. It sorts and merges available ranges and subtracts every overlap with a range of another type. Available memory at or above 4 GiB is merged and subtracted the same way, then counted as unreachable. If neither part contains available memory, the builder falls back to `mem_upper`, clipping the usable range at 4 GiB and counting the excess as unreachable. The boot prints the original map and four counts: usable, unreachable, reserved and managed.

The frame table has one 32-bit word for every frame below the map's top: zero for a free frame, a reference count for an allocated frame, and `FRAME_ABSENT` for one the map does not offer. When the map supplies a top, boot checks that the table's storage lies in usable memory and stops otherwise. Boot reservations are counted, and the allocator maintains both the total free-frame count and the free count below the identity-mapped window.

The kernel's virtual gigabyte begins at `0xC0000000`. The 256 MiB heap begins there, followed by the page-cache window at `0xD0000000`, 128 MiB for memory-mapped device registers at `0xF0000000`, 1024 temporary page mappings at `0xF8000000`, and the boot-module region at `0xF8400000`. All 256 page tables for the gigabyte exist at boot and are shared by every cloned directory. `map_mmio` returns a virtual address in the device region; eight device and diagnostic implementations use that address.

`kheap.c` allocates objects in size classes from 16 to 2048 bytes and larger objects in page runs. It frees both kinds and never sleeps. Page tables and directories created after paging starts come from this heap, as do kernel stacks and the workspace each process keeps for exec. User pages and kernel objects take frames above the 32 MiB identity-mapped window first. They take frames below it only while more than sixty-four remain free there. Block-request pages and page-cache pages are allocated from the top of the window.

User-page zeroing, the copy-on-write copy and the loader's writes use temporary mappings through `kmap_temp`. Failure to obtain a mapping is reported to the caller: a fault remains unresolved and a load fails. Boot modules are mapped into their region on every boot. With `rach2.framefail` on the command line, a count written to `/dev/framefail` makes the count-th frame allocation inside a system call fail.

Processes, open-file descriptions, pipes, ptys, sockets, TCP control blocks and UDP, raw and packet bindings are heap objects on lists. The VFS keeps heap-allocated mount records in a growing array of pointers, with the root, `/dev` and `/proc` in its first three slots. Mounted ext2, FAT32, exFAT and KRFFS records, their nodes and open records, `/proc` snapshots and synthetic nodes also come from the heap.

A descriptor table starts with sixteen entries and doubles as needed to 1024, the width of a select set. A process's region array starts with sixteen entries and grows to 65536. Wait queues link records supplied by the sleepers; entering a queue allocates nothing. Poll and select allocate their per-call arrays from the heap. GCC checks the process fields used by `switch.s` at build time, and `init_proc` checks them at boot, including in the TinyCC build.

The `/proc` generators append to growing text buffers and return `ENOMEM` when allocation fails; `/proc/disk` retains its formatter's fixed bound. `/proc/net/tcp` counts connections under the network lock, allocates space with the lock released, then copies the rows in one locked pass. The copy is bounded by the earlier count, so growth between counting and copying can leave connections out of that report.

A transfer through a sleeping block device gets a request record and a page from the identity-mapped window. The caller and device each retain the request until finished with it; a timed-out request remains quarantined for a late completion. Completion checks the live-request list and the generation before using the record. The live-request limit is a policy of sixteen, reducible with `rach2.blockpool=N`. The deadline begins when a request is taken, after any wait for the limit or the device's share. Polled transfers do not allocate these requests.

The remaining bounds have stated purposes beside their definitions: four named loop devices, the temporary-mapping slots, boot-module records, TCP rings, the accept backlog, the working-directory path, raw and packet receive queues, and device message vectors. The eight-entry ARP table remains a cache whose entries can be replaced.

The page cache and buffer cache each have a target of an eighth of managed memory. A page-cache entry acquires its frame on first read, and the buffer cache allocates buffers as needed. Before allocations in fork, exec, mmap, brk and user-page faults, `mem_balance` starts reclaim when fewer than a sixteenth of managed frames are free. `mem_reclaim` tries to restore the free count to an eighth or stops when neither cache can release more. Each cache selects clean entries before dirty ones; writeback is allowed only inside a system call with interrupts enabled.

A thirty-second of managed memory is reserved. Frame allocation for a process with a nonzero effective uid stops at that reserve. Root, interrupt handlers, the kernel outside any process and a process performing reclaim may use it. Interrupt depth is kept in the interrupted process's record, with a separate count before any process exists. `/proc/meminfo` reports `Buffers` and `Cached` from resident counts, and `MemAvailable` as those counts plus free frames. `HeapPages` and `HeapUsed` report heap allocation for the leak probes.

`prlimit64`, `setrlimit` and `getrlimit` maintain soft and hard limits inherited across fork and exec. `RLIMIT_NPROC` counts processes by real uid across the machine at fork, after reclaim, and returns `EAGAIN` at the caller's limit; real uid zero is exempt. A process that changes identity is counted under its new real uid. `RLIMIT_NOFILE` sets the descriptor table's limit, never above 1024, with `EMFILE` for exhausted allocation and `EBADF` for an out-of-range explicit destination. Only root may raise a hard limit.

`RLIMIT_AS` measures the bytes covered by the process's regions at mmap, brk, stack growth and exec. An excessive mmap returns `ENOMEM`; raw brk leaves the break unchanged, and excessive stack growth leaves the fault unresolved. Exec checks the completed region map after replacing the old image, so an image over the limit ends the process.

Init reads `/etc/rach2/limits` before starting the first session and applies the limits to itself. Each line has `nproc`, `nofile` or `as`, followed by soft and hard values written as numbers or `unlimited`. Invalid values are reported and skipped. The os-real and installer images specify process limits of 256 soft and 512 hard, descriptor limits of 1024, and unlimited address space.

`isr_alloc_vector` assigns a device the lowest unused message vector from 48 through 63 when the device enables messages. `/proc/interrupts` reports the vector under the registered device name. `intrprobe` and the metal acceptance find the device by that name.

## What proves it

`tests/run memory` boots the same kernel at 64 MiB, 512 MiB, 2 GiB, 3 GiB and 4 GiB. It compares usable plus unreachable with the printed map and, after adding QEMU's holes, with the configured memory size. Managed must equal usable less reserved within a shortfall below 32 KiB for partial frames.

At the two ends, `memprobe` maps 24 MiB and 1 GiB respectively, writes and checks patterns on every page, and forks a child that rewrites every fourth page. The parent must retain its original data. Another child execs a fresh image whose anonymous mapping must read as zero. After unmapping, the allowed loss is 256 KiB plus the mapping's page tables, which remain until the process exits.

At both sizes, `memprobe fail` arms counts one through thirty-two separately around fork and exec under `rach2.framefail`. Each attempt must return `ENOMEM` or produce a child that completes normally; surviving pages must retain their data and accept further writes. The free count must stay within 64 KiB of its baseline after each attempt. An armed count beyond the operation's allocations need not fire; the harness requires an injected failure to appear in the boot log.

`frame_hosttest` runs the map builder against QEMU's recorded maps at all five sizes and synthetic maps with holes, overlaps, a range crossing 4 GiB, a wrapping length, a truncated entry, an empty map and too many disjoint ranges.

`tests/run pools` runs separate capacity checks for forty-eight pipes, forty ptys, forty-eight sockets and forty descendant processes. It then creates and closes a thousand pipes, ptys and sockets of each kind, requiring unchanged heap bytes in use and a heap-page count within two pages of the starting count. `sockfull`, `tcbfull` and `procfull` are retired with their pools.

In `tests/run default`, `fdfull` opens 1021 files beside its three inherited descriptors, requires `EMFILE` at the limit, and reuses a closed descriptor. `limitprobe` checks descriptor and address-space limits, permission to raise hard limits, inheritance, process counts and counting after setuid. `dup2test`, `mountprobe`, `pollprobe` and `rawprobe` use the new limits in place of assumptions about fixed tables.

Row 12 of `tests/run mount` starts sixteen readers under `rach2.blockpool=2` and a half-second lag. They share the one request the disk may have in flight, making the total service time about eight seconds despite a five-second transfer deadline. Every reader must succeed and the log must contain no quarantined request. The notes record that starting the deadline before the queue wait makes this row fail.

Row 13 writes a megabyte to a mounted KRFFS volume under a one-tick lag, then touches an anonymous mapping sized to all but a twentieth of managed memory. One further mmap enters the system call where dirty writeback can run. The row requires a smaller buffer cache, a nonzero written-back count in the reclaim log, the complete file after sync, and another successful write.

Row 14 boots os-real, logs in the test account and starts `memprobe hog`. Reads from `/dev/zero` into fresh pages consume memory until allocation stops, with `EFAULT` from a read or failure to create another region. Fork must then return `ENOMEM`. The session ends, root logs in afresh on the console, kills the probe and reads the free-memory count.

`tests/run ssh` starts forty silent connections and requires the guest to report at least five established connections before ending root's console session; the listener's backlog of fifteen holds what Dropbear has not yet taken, and the rest are refused with a reset. Root must then log in again and obtain a shell while the connections remain open.

`tests/run krffsmount` mounts three volumes where the old table allowed two. The ext2 host test retains two hundred existing nodes and, in a separate check, a hundred newly created nodes; the FAT32 host test retains forty. The TCP host test allocates forty control blocks, `vm_hosttest` grows and copies forty-eight regions, and `wait_queue_hosttest` queues sixty-four sleepers. The audio and USB suites use the device names to find their interrupt counts.

`tests/run load` runs for twenty seconds over a FAT32 partition through the lag device. It starts six spinning children, two writers of repeated 256 KiB files, a child that pings the gateway and sleeps fifty milliseconds between attempts, and a console reader. Each writer sleeps ten milliseconds between files. The harness sends six console lines at two-second intervals. Each writer syncs its file before the next.

The load probe prints the maxima from `/proc/stat`: `latency_timer_us`, the gap between timer ticks; `latency_disk_us`, a request from allocation to completion; `latency_diskwait_us`, waiting for the request limit; and `latency_console_us`, a console line from becoming readable to being read. It requires at least three lines read, a timer gap no greater than 200000 microseconds, a request delay no greater than 5000000, and a console delay no greater than 1000000. It prints the request-limit wait without imposing a bound.

The Codex review produced corrections across the memory, process, file-system and network code. FAT32 and exFAT rename now retain nodes across writes, KRFFS looks up open records again after writes, and VFS node puts precede releasing the mount references that protect them. Stream receive reloads its control-block pointer after sleeping. Passive TCP opens check the backlog and free-frame reserve before allocation.

Other corrections make frame selection and claim atomic against interrupt handlers, retain buffers while woken waiters still reference them, and restrict reclaim's dirty writeback to a context that may sleep. Duplication looks up its source descriptor again after table growth. The debug shell disables interrupts before freeing and replacing the current process record. The notes describe the remaining corrections and the failures that led to them.

The clean build again stopped on an image prerequisite, entry 5 of `dev-log/50-shortcomings/plan.md`; a second make completed it. Tests started against the incomplete build were stopped and restarted. Rebuilding the kernel while suites ran also disturbed results. The load work added entry 6: repeated system calls over a polled disk can starve other processes because the timer does not preempt a user process inside a system call and syscall exit does not reschedule. The writers' pause limits that workload until the scheduling correction.

## The machine

The Lenovo was not touched. The kernel proofs run in QEMU with `-cpu max`, and the host tests run on the development host. Milestone 50 plans the Lenovo run and the hour-long load measurement.

## Trust

The cache targets are policy choices, not measurements from a loaded Lenovo. The reserve was enlarged from the plan's hundred-and-twenty-eighth after the console-login case at 128 MiB: init must fork, and login loads two complete programs, requiring more than a megabyte. The emulator checks do not establish that the same fraction is sufficient for every workload.

A map without available memory can fall back to `mem_upper`; if the resulting map has no top, paging retains the legacy assumption of a 32 MiB window. The synthetic map tests do not boot that paging fallback. Boot modules use their virtual region in QEMU, but no recorded boot places a module above the identity-mapped window.

The dirty-cache row proves writeback from a system call. Page faults reclaim clean frames only. The vector checks exercise two devices; the sixteen-vector limit follows the installed IDT entries. No row tests a SYN flood against the memory reserve.

The load maxima are retained since boot, not reset when the probe starts. They measure the request layer and timer under QEMU's workload; they are not Lenovo measurements. Polled disk transfers record no request-layer delay. The network exposes driver drop counts in `/proc/net/dev`, but neither the load probe nor its harness records them, and frame arrival has no timestamp from which to measure network delay. The probe also discards its children's exit statuses, so its delay checks do not establish that every worker continued successfully for the full interval.

## What was accepted

* Commit: `05e366b`, the last change of the kernel and the probes; a host-test stub and the documents came after it.
* Build options: the repository's default kernel builds; the host kernel uses `-m32 -mno-sse -mno-sse2 -ansi` with `-Werror`. The guest kernel is also built with the staged TinyCC. The suites' boots use QEMU `-cpu max`, with `-m` from 64 MiB to 4 GiB, `rach2.framefail`, `rach2.blockpool=2`, `rach2.blocklag=1` and `rach2.blocklag=50` in their respective rows.
* SHA-256 of the kernels and images built from it, `build/` removed first:
  * `0ddb9a48452c85b526d39799c15a5c538f2cc2743aacb445b56d6bf34e0a65a7` `build/rach2/kernel`
  * `56dd96417226c61f5d24325e2dba8fe614f4f30a24e935faf8bbd7d7c25070fd` `build/rach2-recovery/kernel`
  * `e3488527672bac3184fb76270300f2c87e25b2cb497feef5f760ab7d444470b3` `build/rach2-metal/kernel`
  * `b50c72621b8ac00108051d2c1efcc3f41ee06fab3dcb0a2059208bda8af81c47` `build/images/test.krffs`
  * `96db474b6119954b5fc581d3faeeb3a8c2f0c551d63e1dc2af6dd6569d721915` `build/images/os-simple.krffs`
  * `5f5e86b7e9e656bb3070f8b332fb58de32e636afa53c0001922f5c5ad81e5798` `build/images/os-real.krffs`
  * `5aa3c7f54a63df0a99c4d7174d22853ff020bb8fc040d5d28c29f505baee95de` `build/images/rach2-recovery.krffs`
  * `ddac442e657af4624161a1de2135fff3eb16db94c3abaced1aa1a40f9bf5e2e8` `build/images/rach2-metal.krffs`
* Closing test command and result: `tests/run all` on 2026-09-06, from a removed `build/`, 75 of 77 suites passed in 2356 s at 20 workers; `ahci_hosttest` did not link on the host, since the driver's new call into paging had no stub there, and `pwauthmusl` timed out in the pool in `default-e1000`. The stub was added and both suites passed alone afterwards, `tests/run host default-e1000`; the probe's timeout in a pool is an instance of milestone 50's entry 2, and the same probe passed in `default` in the same set. The set before it, with the AHCI driver's change not yet made, passed 71 of 77.
* Hardware arrangement: Apple M4 host, macOS 26.6.2, QEMU 11.1.1. No run or write on the Lenovo.
