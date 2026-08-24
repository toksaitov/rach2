# Milestone 12: the whole-system production-readiness review

Status: draft.

## The goal

Review the complete Rach2 system for correctness, security, consistency, organization, style, documentation, and test coverage. Fix findings confirmed by adversarial review, add missing tests, and correct or add documentation. Use the method established by `dev-log/09-audit` and `dev-log/11-audit`: parallel auditors identify candidate findings, an adversarial verifier tries to refute each candidate, and Codex independently checks every phase.

A fifteen-group parallel survey and an independent Codex proposal produced this plan. The survey identified candidate defects for the phases to examine: a per-fork kernel-stack leak, no CPU-exception handler other than the page-fault handler, stale raw file-node pointers in the fd layer, TCP closing-state data loss, signed-overflow undefined behavior in `k_itoa`, and a host tool that passes an untrusted path through `system()`. It also found structural work (`syscall.c` at 7343 lines, `hda.c` at 2907, `kernel.c` at 2645), missing automation (only 28 of 43 probes and none of the 19 musl tests are part of a required check), and missing or stale documentation (a 1356-line kernel Readme, six subsystems without Readmes, and about 470 comments longer than 80 columns).

## Scope and non-goals

In scope: the whole in-house tree, every `.c` and its matching headers. The kernel (`rach2/kernel.c` and `rach2/sys/**`), the in-house userland (`utils/` and the `ish/` shell), the in-house KRFFS tooling (`krffs/*.c`, not `krffs/vendor`), the tests (`tests/**`), the build and support scaffolding (`Makefile`, `rach2/Makefile`, `mk/`, `tools/`, `rach2/*.sh`, `krffs/*.sh`, the manifests), and all in-house documentation (every Readme, `dev-log/`, the comments). A phase owns its subsystem's headers, not only its `.c` files.

Out of scope are the vendored trees (`musl`, `busybox`, `dropbear`, and `krffs/vendor`, which contains `termbox2.h`), the Doom port (`ports/doom`), and IDE or project files for other platforms (the `krffs/vs-dokany` Windows files). They are used only as external inputs to end-to-end tests. A finding that requires editing one of them remains out of scope. Identify any proposed change to a test contract or built artifact before making it.

## The production-readiness rubric

Each subsystem must satisfy these criteria or record an explicit accepted exception.

1. Build contract: the subsystem builds under the project's required checks (`make doctor`, `make`, `make full`, `verify-style`, `verify-host`) with no skipped required check, stale artifact, or source omitted from `C_SRC`.
2. Freestanding C89: kernel code builds under `-ansi -nostdlib -nostartfiles -nodefaultlibs -Werror` with no new libgcc dependency and no unjustified extension.
3. User/kernel boundary: every user pointer is range-checked and prefaulted or copied through the usercopy helpers; no raw user dereference in kernel context.
4. Privilege separation: ring 3 cannot map, read, or write kernel memory or the page-cache window (`PCWIN_BASE`); `winprobe` cannot survive its window read.
5. Fault policy: a user fault terminates or signals the process; a kernel fault panics with a diagnostic; no silent recovery from kernel corruption. (A default CPU-exception handler is required, not only the page fault.)
6. Storage durability: writeback errors propagate, dirty pages are retained on a failed sync or evict, writes flush to media, and the persist and crash tests pass; no ignored I/O failure on a durability path.
7. Resource lifetime: every fd, OFD, socket, pipe, pty, framebuffer, process, page, VMA, kernel stack, and cache slot has a single owner and release path; no leak, double release, or stale pointer.
8. Concurrency: IRQ/syscall shared state has explicit locking or IF discipline, and every wait queue scans-then-parks safely; no lost-wakeup window, no unprotected shared network/audio/tty state.
9. ABI: syscall numbers, structures, errno returns, auxv, TLS, signal frames, and the stat/getdents/ioctl layouts match what the vendored musl/BusyBox/Dropbear actually use; no undocumented ABI drift.
10. Bounds and overflow: every address, length, sector, packet, path, ELF, and manifest calculation rejects wrap and truncation; a clamp-and-continue is documented and tested or it is a defect.
11. Fail-closed security: the random devices and `getrandom` fail without trusted entropy; malformed packets, images, ELFs, and manifests are rejected; a failed device probe leaves inert state.
12. Test evidence: each subsystem has a guest probe, a host interface test, a musl test, an end-to-end test, or an explicit accepted gap; no critical or high behavior is untested.
13. Documentation: the owning Readme and the comments describe current behavior and its limits; no stale contract doc.
14. Organization: a file has one coherent concern and a maintainable size; no dead code, no commented-out code, no perpetually-disabled debug path left shipping.
15. User-visible behavior: `os-simple` and `os-real` boot to usable shells, PID 1 reaps and respawns, Dropbear SSH works, the network utils work over SLIRP, and the rescue shell never masks an init failure.

## Method, common to every phase

Each phase uses one parallel Workflow audit that `/goal` runs to completion, followed by maintainer review. Codex participates in every phase:

* Run parallel auditors, one per file or group of concerns. Give each the checks below, their `file:line` anchors, and the rubric. An auditor reads but does not fix.
* Give every candidate finding to a separate adversarial verifier whose task is to refute it. A finding counts only if it survives this second review; most do not.
* Codex independently checks each phase. It reads the confirmed findings and every file owned by the phase against the rubric and code, tries to refute each finding, and rejects the phase if the parallel audit missed a concern. Record its conclusion with the phase token.
* Give each confirmed finding one scoped fix with a one-line commit message and no attribution. This review may add tests, split an oversized file at coherent boundaries, and rewrite a Readme. Three rules order that work. First, each phase writes the host and guest tests required by its own checks, so no phase depends on a test scheduled for later. Phase 24 then automates the complete suite, connects unautomated artifacts, and fills remaining cross-cutting gaps. Second, a contract or behavior change updates its owning Readme in the same commit, as required by `CLAUDE.md`; Phase 27 checks coherence and rewrites existing material rather than collecting earlier corrections. Third, Phase 26 owns the physical splits of `syscall.c`, `hda.c`, and `kernel.c`. Semantic phases review their handlers in place, and later phases do not refer to prematurely moved files. Identify a behavior or contract change before applying it.
* Each phase ends with its required checks and named token. The maintainer reviews the result before `/goal` proceeds. A phase completes only after its checks pass without weakening an earlier phase.

Each finding has a severity: critical, high, medium, or low. Critical and high findings require a definite result. Confirm or refute medium and low findings; related findings may share a commit.

The phases are ordered by risk. Kernel code that can corrupt memory, persist invalid data, or break the privilege boundary comes first. Drivers, networking, userland, and the cross-cutting test, build, and documentation reviews follow. End-to-end tests and a final independent adversarial review complete the work.

## Phase 0: baseline, scope, and the prior-audit regression map

Scope: record the passing baseline, define the scope, and confirm that the two prior audits still hold. Include the `C_SRC` source-list check and the build inventory (`Makefile`/`rach2/Makefile`/`mk/`). Nothing is fixed here.

Work: record passing results for `make verify-style` (8/8), `verify-host`, `make test` (28/28), `check-sources`, the ATA and AHCI disk probes, and a default boot. Use these results as the comparison for later phases. Confirm that `git diff` is empty. Build the file inventory and assign every changed contract file to a later phase. Check the current tree against the required invariants from `09-audit` and `11-audit`: the AHCI fixes, the `!user_mode` window restriction, crash-test image isolation, and source-list failure behavior.

Verification: baseline recorded; the map accounts for every in-house source; the prior-audit invariants re-confirmed. Codex replays the inventory, compares the phase map to `CLAUDE.md` and the two prior plans, and rejects any later phase whose file claims are absent. Token `REVIEW-00-BASELINE`.

## Phase 1: boot, traps, entry, and the kernel core file

Scope: the multiboot entry (`loader.s`), the GDT/TSS/IDT (`tables/`), the ISR router and trap epilogue (`isr.c`, `isr_handler.s`), the PIC/PIT, and the boot sequence and self-test/debug-shell glue in `kernel.c`.

Checks:

* (high) Only vector 14 has a handler. Every other CPU exception (0-13, 15-31) dispatches into a router that, with no registered handler, silently returns and re-faults, ending in a triple-fault reset with no diagnostic (`sys/isr.c:81-107`). Install a default exception handler that panics on a kernel fault and kills the faulting ring-3 process on a user fault, printing the vector, error code, and EIP. Rubric 5.
* The trap/IRQ entry preserves ring-3 state, the segment selectors, and the IF discipline before `iret`; the saved-register layout matches `isr_params_t` and the `init_proc` offset assertions (cross-checked with Phase 8's switch.s audit).
* (medium, organization) `kernel.c` is 2645 lines combining the boot sequence, self-tests, and debug shell. Record a split that moves the self-tests and debug-shell dispatch into separate units, leaving `k_main` and the boot flow focused. Phase 26 owns the split after this phase completes its review and tests. Rubric 14.
* Boot ordering: no init step, IRQ enable, or self-test can run before its prerequisite (paging before the cache, the cache frames before any window touch, the PIC before `sti`); the rescue and no-init paths are correct.

Verification: a default-exception boot trace (a deliberate fault panics with a vector, not a silent reset); the boot-order trace; `make test` 28/28 after the split; verify-style 8/8. Codex walks `k_main` top to bottom annotating every dependency and IRQ-enable point, and traces the saved-register layout against the offset asserts. Token `REVIEW-01-BOOT`.

## Phase 2: the physical-memory base

Scope: `sys/mm/frame.c` (the multiboot map walk, the frame bitmap, the 32 MiB cap), `sys/mm/heap.c`, the early reservations in `kernel.c`, and the linker layout.

Checks: the frame accounting, the reserved ranges, the kernel heap, and the static DMA regions (the AHCI/HDA/SB16 buffers below `HARD_MEMORY_LIMIT`) are mutually consistent and below the reachable physical limit; `mem_reserve` / `alloc_user_page` / `alloc_kernel_frame` cannot collide; frame exhaustion returns a graceful failure, not corruption. Confirm the heap's free-list and alignment.

Verification: `verify-host` (`frame_hosttest`), a boot, and a source-based proof that the DMA regions and frame ranges agree. Codex computes the frame ranges from the map handling and checks that every cited static DMA buffer remains within them. Token `REVIEW-02-FRAMES`.

## Phase 3: paging, the user boundary, and usercopy

Scope: `sys/mm/paging.c` (init, `CR0.WP`, `vm_resolve`, prefault, COW, the fault handler, `map_mmio`), `sys/mm/vm.c`, `sys/mm/usercopy.c`.

Checks:

* (medium) `map_mmio` installs page tables only into `_kernel_dir` with no premap, so a runtime call after the first fork is invisible to already-forked processes (`paging.c:471-512`). Boot-time MMIO is safe; the `ahci` debug command's runtime `ahci_init -> map_mmio` is the latent path. Premap the device-MMIO PDE band at boot, or forbid/guard runtime `map_mmio`, and document the contract. Rubric 7/8.
* The fault handler branch order (COW, then VMA demand paging, then the window branch restricted by `!user_mode`, then segfault or panic) and the `USER_ADDR_MAX == PCWIN_BASE` boundary; no user fault reaches `block0->read`. Rubric 4.
* `vm_prefault_range` self-defends against `addr+len` overflow (Phase 23 also owns this); every user range obeys `[0x02000000, 0xC0000000)` half-open.
* Tests to add: the `cow_fault` sole-owner in-place re-grant branch (distinct from the shared-copy branch), frame exhaustion to a clean `-EFAULT`/segfault, and a `strncpy_from_user` that crosses into a legitimately-unmapped trailing page.

Verification: `verify-host` (`vm`, `pagecache` boundaries), the `vmprobe`/`cowprobe`/`forkro`/ `vmro`/`vmfault`/`winprobe` probes, plus the new tests. Codex enumerates every fault branch and proves the user/window separation. Token `REVIEW-03-PAGING`.

## Phase 4: the page-cache window

Scope: `sys/mm/pagecache.c` (eviction, fault, sync, backing switch, capacity, the window probe) and `pagecache.h`.

Checks: a dirty page is never silently dropped on a failed writeback or evict; a kernel touch of the window above the backed file-system size panics (the intended fail-stop, confirmed); `pc_set_capacity` does not ignore an eviction failure; the open-addressed slot hash is correct under tombstoning. Test to add: the writeback-failure-then-skip path (the host test's fake writer never fails today).

Verification: `verify-host` (`pagecache_hosttest`, with the new failure case), a `rach2.fsdisk` boot reading and writing across faults and evictions. Codex traces the dirty-bit transitions across `pc_fault`/`pc_evict`/`pc_sync`/`pc_set_backing`. Token `REVIEW-04-PCACHE`.

## Phase 5: the block layer and the KRFFS disk mount

Scope: `sys/block/{block,ata,ahci}.c` and the kernel disk-mount flow in `kernel.c` (`init_krffs_disk`, the MBR probe, the dirty bit, the fsck contract). AHCI was audited in milestone 11; survey it only for whole-system consistency.

Checks: the read/write/flush bounds in both drivers are identical and correct; the partition view forwards and clamps correctly; the MBR probe and the size-vs-window-cap arithmetic are overflow-safe; the dirty-flag lifecycle and fsck recovery hold; the default RAM and `if=ide` boots stay byte-identical.

Verification: `test-disk`, `test-disk-ahci`, `persist-test`(-ahci), `crash-test`; the default boot unchanged. Codex re-verifies the bounds idiom and the mount arithmetic. Token `REVIEW-05-BLOCK`.

## Phase 6: the syscall interface and the ABI

Scope: `sys/syscall.c` (7343 lines, ~110 handlers) and the user/kernel ABI.

Checks:

* (high, organization) The file is one 7343-line translation unit. Plan the split into per-family units (file, proc, signal, memory, socket, ioctl, time) behind a shared internal header and fix the family boundaries here, but defer the physical split to the structure phase (Phase 26), after every semantic phase (7, 9, 10, 14, 15) has reviewed its handlers in `syscall.c` in place. The split is the largest structural change of the review; it must keep `make test` byte-identical and preserve every Doxygen contract comment. Rubric 14.
* Every syscall validates its user pointers through the usercopy helpers; no raw user dereference; errno returns are correct (the right negative errno, not a clamp or a wrong code); nested/indirect buffers (iovecs, socket addresses, `argv`) are bounds-checked. Rubric 3/9/10.
* Write the missing syscall-ABI Readme using the `mm`/`block` model: describe dispatch, the usercopy contract, each syscall family, and the musl and BusyBox requirements.
* (medium) `do_wait` re-implements the lost-wakeup sleep loop instead of reusing `wq_wait_event`; reconcile (Phase 7 owns the proc side).

Verification: `make test` 28/28, `verify-style`, `verify-host`, and the SSH and BusyBox end-to-end tests, which exercise the ABI. Complete the per-family usercopy and errno review and define the family boundaries for the split. Codex checks usercopy requirements for each family and confirms the Phase 26 map. Token `REVIEW-06-SYSCALL`.

## Phase 7: the process lifecycle

Scope: `sys/proc/{process,sched,wait_queue}.c` and the fork/exit/wait/reparent paths in `syscall.c`.

Checks:

* (high) A per-fork kernel stack is `mem_reserve`'d and never reclaimed on reap, a steady leak across fork/exit churn (`syscall.c:3471-3518`, `sys_fork`). Pool the stacks or free the reservation on reap. Rubric 7.
* The lost-wakeup-safe sleep (`wq_wait_event` and the duplicated `do_wait` loop): the scan-then-park is `cli`-bracketed across the `signal_pending` check and the `BLOCKED` set; reconcile the duplication with Phase 6.
* `init` death, orphan reparenting, the zombie/reap contract, and the scheduler's IF discipline and round-robin fairness.

Verification: `make test` (`forktest`, `jobprobe`, `orphanprobe`, `waitprobe`), a fork-churn boot that shows stable kernel-frame occupancy, and a new test for the fork stack-lifecycle. Codex traces the stack allocation/free across fork and reap. Token `REVIEW-07-PROC`.

## Phase 8: exec, ELF, the initial stack, and TLS

Scope: `sys/proc/{elf,uimage,tls}.c`, `sys/proc/switch.s`, and the execve teardown.

Checks: the ELF loader's malformed-image handling (segment bounds, window and overlap rejection, and protect-after-copy ordering under `CR0.WP`); the `build_user_stack` aux, argv, and envp layout; the TLS GDT update during context switches and the `switch.s`/PCB offset contract; destructive exec after teardown; and GP-register reset. Add rejection tests for a truncated ELF header, a segment beyond the window, and overlapping segments; these paths have no negative coverage.

Verification: `make test` (`shelltest`, `auxvprobe`, `tlsprobe`), the new ELF-rejection tests, and the SSH and Doom end-to-end tests, which execute user programs. Codex checks the offset contract and teardown order. Token `REVIEW-08-EXEC`.

## Phase 9: signals and job control

Scope: the signal mechanism (currently split between `syscall.c` and its own home), signal frames, masks, stops, `sigreturn`, and Ctrl-C delivery to the foreground group.

Checks: the signal mechanism's organization (it lives outside the cluster it belongs to; consider giving it a coherent home and Readme); the sigreturn frame restoration (full GP set including `eax`) stays independent of fork and exec; the stop/cont status encodings; `Ctrl-C`/`SIGINT` to the foreground process group across the controlling terminal.

Verification: `make test` (`sigprobe`, `jobprobe`), a manual Ctrl-C/job-control exercise. Codex re-verifies the sigreturn/fork/exec independence and the stop encodings. Token `REVIEW-09-SIGNAL`.

## Phase 10: the fd and OFD layer

Scope: `sys/fs/fd.c`/`fd.h`, the file-ops vtable, dup/fork/CLOEXEC, refcounts.

Checks:

* (high) `fd_entry.node` holds a raw `krffs_node_t *` with no refcount or pin, so a concurrent `unlink`/resize/relocate can leave a stale pointer (`fd.h:93-101`, `krffs.c`). Decide and implement: a refcount/pin on the open node, or a documented invariant that the single-threaded IF=0 model makes it safe, with a test that proves the chosen contract. Rubric 7.
* The OFD lifetime across dup/fork/CLOEXEC-exec and death; every fd-backed object (pipe, socket, pty, fb, synth) releases through one path.

Verification: `make test` (`dup2test`, `ofdprobe`, `pipeprobe`), the new stale-node test. Codex traces the node-pointer lifetime against unlink/resize. Token `REVIEW-10-FD`.

## Phase 11: the KRFFS core file system

Scope: `sys/fs/krffs.c` (the allocator, the circular-node walker, paths, dirs, write/truncate/relocate).

Checks:

* (high, coverage) The allocator and walker have no host unit test; a bug corrupts persistent disks. Add a host-side KRFFS allocator/walker test (the `synthfs`/`pagecache` host pattern) locking down split/coalesce/relocate and path resolution. Rubric 12.
* (medium) `creat`/`O_CREAT` on an existing directory silently succeeds (`krffs.c:924-937`); decide the correct errno.
* The in-place circular-list mutation under writes, the dirty-flag write-through, and the root-node invariants.

Verification: the new host test passing under ASan/UBSan, `make test` (`dirtest`, `stattest`), a `rach2.fsdisk` boot. Codex adversarially drives the allocator edges. Token `REVIEW-11-KRFFS`.

## Phase 12: the synthetic file systems

Scope: `sys/fs/{devfs,procfs,synthfs}.c` (the `/dev` and `/proc` classifiers).

Checks: the central VFS classifier; the `/proc/<pid>` bounds (a crafted huge pid cannot index past the proc table); device lseek/numbers and the `copy_to_user` targets cannot reach the window; the getdents/stat shapes.

Verification: `make test` (`devprobe`, `procprobe`, the `synthfs_hosttest`). Codex re-checks the pid bounds and the copy-to-user targets. Token `REVIEW-12-SYNTHFS`.

## Phase 13: TTY, console, and pseudo-terminals

Scope: `sys/tty/{tty,n_tty,pty}.c`, the keyboard/serial input, sessions, and the controlling-terminal model.

Checks: the N_TTY line discipline (canonical editing, ISIG, the echo suppression asymmetry between the keyboard IRQ and the serial drain); the console/pty controlling-terminal lifecycle (the session-leader invariant); the foreground-group and blocking-read/poll interplay. The pty half explicitly: the `/dev/ptmx` open and the `TIOCGPTN`/`TIOCSPTLCK` unlock path, `/dev/pts/N` resolution, the master/slave close ordering and the leader-ctty release, and the musl/Dropbear pty session lifecycle.

Verification: `make test` (`ttyprobe`, `ptyprobe`, `jobprobe`, `n_tty_hosttest`), the SSH end-to-end test, a manual type-ahead/Ctrl-C/line-editing exercise. Codex re-verifies the controlling-terminal invariant and the echo model. Token `REVIEW-13-TTY`.

## Phase 14: wait queues, poll/select, and pipes

Scope: `sys/proc/wait_queue.c`, `sys/io/{poll,poll_select}.c`, `sys/ipc/{pipe, pipe_ring}.c`.

Checks: the scan-then-park lost-wakeup safety generalized to many queues; the `fd_set`/pollfd translation (the fence-post arithmetic, direction gating); the pipe ring's EOF/SIGPIPE/nonblock and the closable-stdio wake path.

Verification: `make test` (`pollprobe`, `pipeprobe`, the `poll_select`/`pipe_ring`/ `wait_queue` host tests). Codex re-checks the park/dequeue and the translation edges. Token `REVIEW-14-POLL`.

## Phase 15: the network core, locking, and sockets

Scope: `sys/network/core/net_core.c`, `sys/network/socket/socket.c`, and the `net_lock` IF-save discipline bracketing the shared TX buffers, the TCB/socket pools, and the ARP cache.

Checks: every external TX entry brackets the shared static buffers under `net_lock`; the SPSC lock-free recv ring is correct; the socket layer's getsockname/getpeername/shutdown/setsockopt fill-out and the poll wakeups; no unprotected shared state between the IRQ and syscall contexts. Rubric 8.

Verification: `make test` over the network probes (wired in Phase 24), the SSH and httpd end-to-end tests. Codex audits the lock discipline at every TX site. Token `REVIEW-15-NETCORE`.

## Phase 16: TCP

Scope: `sys/network/tcp/tcp.c` (1002 lines: the state machine, timers, retransmit, the accept queue, FIN/TIME_WAIT, the socket wakeups).

Checks:

* (high) Closing-state data loss and unguarded FIN handling: `tcp_rx` in FIN_WAIT_1/ FIN_WAIT_2 (`tcp.c:936-964`) can lose in-flight data and mis-handle a FIN transition. Fix the closing-state transitions and the data delivery.
* (medium) Send-side backpressure: `tcp_send` (`tcp.c:362-396`) and socket `POLLOUT` report writable when the send ring is full, so a writer spins or loses data. Add backpressure and report `POLLOUT` only when space is available. Rubric 8/10.

Verification: a host-interface TCP test driving the state machine and the new backpressure (written this phase), the SSH end-to-end test over a long session, an httpd transfer larger than the send ring. Codex re-verifies the closing-state and backpressure fixes. Token `REVIEW-16-TCP`.

## Phase 17: IP/UDP/ARP/ICMP/Ethernet and the RTL8139 driver

Scope: `sys/network/{ip,udp,arp,icmp,ethernet}/*.c`, `sys/network/driver/rtl8139.c`, and the network headers (`net.h`, `byteorder.h`).

Checks: the packet parsing and checksum handling reject a malformed or short frame; the IP/UDP/ARP/ICMP paths bound every length; the rtl8139 RX/TX ring and descriptor handling, the PCI probe, and the IRQ path are correct. Rubric 10/11.

Verification with per-layer evidence: the malformed/short-frame rejection at each of Ethernet, IP, UDP, ARP, and ICMP; the checksum host-interface tests (written this phase); the rtl8139 RX-ring wrap, the TX-descriptor ownership, and the IRQ handoff; the `pingtest`/`udptest` probes. Codex re-checks the checksum and ring math at each layer. Token `REVIEW-17-NETLOW`.

## Phase 18: the audio drivers

Scope: `sys/audio/{audio,audio_debug,dma,sb16,hda}.c` and the audio headers (`audio.h`, `dma.h`, `sb16.h`, `hda.h`).

Checks:

* (medium, organization) `hda.c` is 2907 lines combining controller initialization, codec enumeration, ring, format and resampler code, and DMA. Record a split along the initialization, enumeration, and stream boundaries. Phase 26 owns the split after this phase adds the audio host tests below. Rubric 14.
* (medium) Remove the permanently disabled debug-logging path in `hda.c` or place it behind a boot option. Document the single owner of the HDA playback ring and LPIB-tracking state so two consumers cannot use them concurrently.
* The ISA-DMA `<16 MiB`/64 KiB-boundary buffer placement and the static-buffer virt==phys cast stay correct after any move.

Verification: a manual audio play over SB16 and HDA (the only exercise today), plus the first automated audio coverage, host-side unit tests for the ring/format/resampler/DMA bounds written this phase so they exist before the Phase 26 split. Codex re-verifies the DMA-buffer invariants. Token `REVIEW-18-AUDIO`.

## Phase 19: display, peripherals, and core hardware

Scope: `sys/display/{vga_text,vga_gfx}.c`, `sys/pci.c`, `sys/kbd.c`, `sys/mouse.c`, `sys/speaker.c`, and the core glue `sys/{pic,pit,serial,kio,module,cmdline,panic}.c`.

Checks:

* (medium, dead code) A dead `init_keyboard`/`_keyboard_test` path has a misleading comment (`kbd.c`), while `init_pci` is never called and its contract does not match the code (`pci.c:366`). Remove each path or give it a defined, documented role. Rubric 14.
* Stale header contracts, the speaker divisor clamp, the framebuffer ownership and the VGA bounds, the PCI BAR helpers, the cmdline tokenizer (the truncation fix from milestone 11 stays correct).

Verification: a boot exercising VGA text + mode-13h (Doom), the PS/2 keyboard/mouse, and the speaker; `make test`. Codex re-checks the dead-path removal and the BAR/bounds. Token `REVIEW-19-DEVICES`.

## Phase 20: the freestanding library and randomness

Scope: `sys/lib/{kctypes,kstd,kstring,krandom}.c`.

Checks:

* (medium, UB) `k_itoa` negates `INT_MIN` in signed `int` (`kstd.c:51-52`); fix the overflow. `k_strreverse` underflows on an empty string (`kstring.c:8-16`); guard it.
* (high, coverage) `kstd.c`/`k_itoa` has no test at all, and the `kstring` host test covers neither `k_strreverse` nor the case-fold helpers. Add an ASan/UBSan host test that compiles `kstd.c` and covers the missing functions. Rubric 12.
* `kstring.c`/`kctypes.c` violate the comment and prose policy through body narration. Correct these files in this phase; Phase 26 owns the tree-wide review. Confirm that both `krandom` interfaces report failure without trusted entropy.

Verification: the new and existing host tests pass, including `make test` (`rngprobe`). Codex checks the undefined-behavior fixes and failure behavior. Token `REVIEW-20-LIB`.

## Phase 21: the in-house userland

Scope: `utils/` (init and the simple Unix tools, the network utils httpd/nc/ping/ resolv) and the `ish/` shell (935 in-house lines, its own Readme, wired into the build): its parser, builtins, syscall wrappers, build, and Readme.

Checks: `init.c` PID-1 reaping and respawning; the `ish/` parser, builtins, and syscall-wrapper bounds; the simple tools' bounds and consistent prose; and network-utility behavior over SLIRP. Rubric 15.

Verification: the SSH/BusyBox/httpd/ping end-to-end tests, the `os-simple` boot to the `ish` shell, `orphanprobe`. Codex re-checks the init lifecycle and the `ish` parser. Token `REVIEW-21-UTILS`.

## Phase 22: the KRFFS host tooling

Scope: `krffs/*.c` (mkfs, fsck, defrag, edfs, fsviz, stageimage, `krffs_node.c`, `krffs_platform.c`), excluding `krffs/vendor`.

Checks:

* (medium, security) `defrag.krffs` runs fsck via `system()` with an untrusted path concatenated into a shell string (`defrag.krffs.c:105-125`); replace with a direct exec. `edfs.krffs` parses 64-bit on-disk fields with 32-bit `strtol()` and silently no-ops (`edfs.krffs.c:106,137,162,174,186`); fix the width and give feedback. Rubric 10/11.
* (medium) The `defrag` slide loop self-modifies the ring under `memmove` and is untested; validate it and the allocator under new ASan/UBSan host tests.

Verification: the new host tests passing, a defrag/fsck round-trip on a staged image. Codex re-verifies the slide and the `system()` replacement. Token `REVIEW-22-TOOLING`.

## Phase 23: cross-cutting security

Scope: review the complete trust boundary together: `usercopy.c` and every syscall caller, the `!user_mode` restrictions, integer-overflow cases, the CSPRNG, DMA buffers for audio, networking, and block I/O, and `/proc` and `/dev` access.

Checks: `vm_prefault_range` rejects `addr+len` overflow itself; the raw transfers behind `FB_SETPALETTE` and `AUDIO_WRITE` use bounce buffers or an established pinning invariant; ring 3 cannot reach a privileged path; and negative and boundary coverage expands beyond the current `errnoprobe`. Rubric 3/4/10/11.

Verification: run the security probes (`winprobe`, `vmfault`, and the expanded negative tests from Phase 24) and review the trust-boundary source paths. Codex independently checks every user-pointer entry and tries to construct a ring-3 escape. Token `REVIEW-23-SECURITY`.

## Phase 24: the test suite (extend it)

Scope: `tests/**`, `tools/runtests.py`, `tools/harness.py`. This is the automation-and-consolidation pass for the suite.

Work: each earlier phase already wrote the host and guest tests required by its checks: the KRFFS allocator and walker, `kstd`/`k_itoa`, audio ring, format and DMA bounds, network checksum, RX-ring and TCP state, page-cache writeback failure, the `cow_fault` sole-owner branch, ELF rejection, fork-stack lifetime, the fd stale-node contract, and expanded trust-boundary negatives. Phase 24 connects every artifact to a make target through `harness.py`. This includes freestanding probes absent from `make test`, the 19 musl integration tests, network probes, and manually run fault probes such as `winprobe`. Ensure a required check runs each test, fill remaining cross-cutting gaps, and either add CI configuration or record its deferral. Every test has a token and make target.

Verification: every new and existing test passing; `make test` count grows and stays deterministic; no flaky probe. Codex re-verifies each new test actually exercises the contract it claims (no weak assertion, the FAIL path reachable). Token `REVIEW-24-TESTS`.

## Phase 25: the build system and support scripts

Scope: `Makefile`, `rach2/Makefile`, `mk/`, the full `tools/` set (harness, runtests, check, check_sources, doctor, hosttest, textcheck, gen_compile_commands, the `cppcheck.suppress` list, mkdisk*, grub_embed, crash_test, mkiso), `rach2/*.sh`, `krffs/*.sh`, the manifests.

Checks: pin the network-dependent Docker base for the ISO and GRUB-disk builds; preserve `grub-mkrescue` stderr; replace the fixed delay in `crash_test.sh` with a serial-driven wait; enforce or centralize the shared geometry in `crash_test.sh` and `mkdisk.sh`; and check the scripts for any condition that can report success without running current work.

Verification: `verify-style` (shellcheck), `check-sources`, every disk target, and the ISO and GRUB build pass. Codex reconstructs each check and the cases that could falsely report success. Token `REVIEW-25-BUILD`.

## Phase 26: structure, comments, and style, tree-wide

Scope: the large-file structural splits deferred from the earlier phases, and the comment style across the whole kernel, against `CLAUDE.md`'s Comments and Writing sections. This runs after every semantic phase has reviewed its handlers in place and written its tests, and before the documentation phase, so the docs describe the new structure.

Work:

* Perform each deferred structural split as a mechanical reorganization that keeps `make test` byte-identical. Split `syscall.c` (7343 lines) into per-family units behind the shared internal header whose boundaries Phase 6 defined. Split `hda.c` (2907 lines) along the initialization, enumeration, and stream boundaries, using the audio host tests from Phase 18. Move the self-tests and debug-shell dispatch out of the `kernel.c` (2645 lines) boot flow. Give each split its own commit, update `C_SRC`, the `check-sources` source-list check, and the `-MMD` dependencies, preserve every Doxygen comment, and verify byte-identical `make test` results.
* The comment sweep: reflow the ~470 over-80 block-comment-body wraps and the ~44 over-80 code lines; convert the legacy files' body narration to the contract (a header block, no reasoning trace), worst offenders first (`syscall.c` has 254 in-body comments); normalize the Title-Case diagnostics (e.g. `hda.c`'s `HDA:`) to the lowercase `component: detail` shape tree-wide; confirm every function carries a Doxygen `/**` header consistent with its siblings.

Verification: `make test` byte-identical after each split; `verify-style` 8/8; `check-sources` clean; a sample re-read confirming each touched comment still describes the code. No behavior change. Codex audits each split for a dropped symbol, a moved comment, or a build-graph error, and spot-checks the comment voice. Token `REVIEW-26-STRUCTURE`.

## Phase 27: documentation, structure and rewrite

Scope: every in-house Readme and the doc set as a whole, now reflecting the file structure Phase 26 left. The per-phase contract-drift fixes already landed inline under the same-commit rule, so this is the coherence-and-rewrite pass.

Work:

* (high) Rewrite the stale ~1356-line kernel Readme (`rach2/Readme.md`), an old development diary in the voice forbidden by `CLAUDE.md`, as a concise current overview that points to subsystem Readmes.
* (high) Add the missing subsystem Readmes (`audio`, `display`, `lib`, `proc`, `tables`, `tty`, and the syscall-ABI Readme) using the established `mm`/`block`/`fs` template and describing the post-split structure. Make the old and new documents use one consistent voice.
* Confirm the per-phase inline Readme updates landed, and fix any residual contract drift; decide the table-vs-prose question per `CLAUDE.md`; confirm `dev-log/` stays historical.

Verification: `verify-style` over the docs; a read-through confirming every Readme states the current contract and the set is coherent. Codex reviews the structure and the voice, the part no checker enforces. Token `REVIEW-27-DOCS`.

## Phase 28: the end-to-end test matrix and whole-system manual pass

Scope: nothing new is read; the end-to-end exercise of every contract on emulated hardware. The `os-simple` and `os-real` boots, BusyBox `ash`, the Dropbear SSH end-to-end test (pubkey, interactive shell, `rc=0`), the network utils over SLIRP, Doom, and the disk/AHCI end-to-end tests (ATA and SATA, persist, stress, crash).

Verification: every end-to-end test passing over both the RAM and disk boots and both block buses; the default boot byte-identical to the Phase 0 baseline. Codex confirms the matrix is complete. Token `REVIEW-28-CAPSTONE`.

## Phase 29: final adversarial review and completion

Scope: review the cumulative change. Codex performs one independent final pass over every fix, split, test, and documentation rewrite, and tries to refute the result. Confirm that no critical or high finding remains open, earlier audit findings remain covered, every contract change has a matching test and document, and vendored trees and the Doom port remain outside the diff. Write `dev-log/12-review/Readme.md`, update `dev-log/Readme.md`, and complete the milestone.

Verification: the complete check set below passes together, and Codex records the final result `AUDIT-COMPLETE`. Token `REVIEW-29-COMPLETE`; the milestone is complete.

## Verification

Run the complete check set after every phase that changes behavior and once more at completion:

* `make doctor` (the toolchain check) and a clean `make` and `make full` build.
* `make verify-style` (8/8) and `make verify-host`.
* the module-path `make test` (28/28 or the grown count, deterministic) and a default RAM boot byte-identical to the Phase 0 baseline.
* the ATA and AHCI disk probes (`test-disk`(-ahci), `persist-test`(-ahci), `stress-test`(-ahci)) and `crash-test`.
* the `os-simple` boot to a usable shell, and the `os-real` boot to BusyBox with PID 1 reaping, the Dropbear SSH end-to-end test, the network utils over SLIRP, and Doom.

After a risky change, run the narrow affected tests first and the complete check set before recording the phase token. Vendored trees and the Doom port remain untouched. Do not change probe tokens, the module test set, or the byte-identical default boot without identifying the contract change first. Record each Codex result with its phase token.

## What stays deferred

* Phase 24 either adds CI configuration or records an explicit deferral; this plan does not assume a CI host exists.
* If pinning the Docker base does not make the build reproducible, record a fully offline ISO and GRUB build as follow-up work.
* Exclude subsystem rewrites based on preference rather than a confirmed defect or the organization criterion in rubric 14. Do not add optional features during this review.
* The vendored trees and the Doom port: no change, ever, this milestone.

## Baseline

The comparison baseline is `HEAD` at `b045704`, the completion of milestone 11. Phase 0 records `verify-style` at 8/8, `verify-host`, `make test` at 28/28, the ATA and AHCI disk probes, a default boot, the SSH end-to-end test, and Doom. Every later phase compares its result with this state, so a regression belongs to the phase that introduced it.
