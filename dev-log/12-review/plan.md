# Milestone 12: the whole-system production-readiness review

Status: draft.

## The goal

Make Rach2 a production-ready, stable, solid system: code, support files, and
documentation that are not embarrassing to show. This is the whole system, not one
subsystem. It reviews every in-house line against correctness, security,
consistency, organization, style, comments, documentation, and test coverage,
fixes what adversarial verification confirms, extends the test suite where coverage
is thin, and restructures and rewrites the documentation where it has drifted or
was never written. It is built and run in the discipline of the two prior audits
(`dev-log/09-audit`, `dev-log/11-audit`) and exceeds them in breadth: every phase
is a multi-agent fan-out whose candidate findings are adversarially verified before
they count, and Codex independently re-verifies every phase.

A grounding survey (a fifteen-cluster fan-out plus an independent Codex proposal)
seeded this plan. It already surfaced real defects the phases below will chase: a
per-fork kernel-stack leak, the absence of any CPU-exception handler but the page
fault, stale raw file-node pointers in the fd layer, TCP closing-state data loss, a
signed-overflow UB in `k_itoa`, and a host tool that shells out through `system()`
with an untrusted path; plus structural debt (`syscall.c` at 7343 lines, `hda.c` at
2907, `kernel.c` at 2645), a test-automation gap (28 of 43 probes and none of the
19 musl tests are wired into a gate), and documentation debt (a stale 1356-line
kernel Readme, six subsystems with no Readme, ~470 over-80 comment wraps).

## Scope and non-goals

In scope: the whole in-house tree, every `.c` and its matching headers. The kernel
(`rach2/kernel.c` and `rach2/sys/**`), the in-house userland (`utils/` and the
`ish/` shell), the in-house KRFFS tooling (`krffs/*.c`, not `krffs/vendor`), the
tests (`tests/**`), the build and support scaffolding (`Makefile`, `rach2/Makefile`,
`mk/`, `tools/`, `rach2/*.sh`, `krffs/*.sh`, the manifests), and all in-house
documentation (every Readme, `dev-log/`, the comments). A phase owns its subsystem's
headers, not only its `.c` files.

Out of scope and off-limits: the vendored trees (`musl`, `busybox`, `dropbear`, and
`krffs/vendor`, which holds `termbox2.h`), the Doom port (`ports/doom`), and the
other-platform IDE/project scaffolding (the `krffs/vs-dokany` Windows files). They
are black-box capstone inputs only; a finding that would edit them is out of scope.
The review may change a test contract or a built artifact only when it flags the
change first.

## The production-readiness rubric

Every phase holds its subsystem to this bar. A subsystem passes a criterion or
records an explicit, accepted exception.

1. Build contract: builds clean under the project's gates (`make doctor`, `make`,
   `make full`, `verify-style`, `verify-host`) with no skipped hard check, no stale
   artifact, no source omitted from `C_SRC`.
2. Freestanding C89: kernel code builds under `-ansi -nostdlib -nostartfiles
   -nodefaultlibs -Werror` with no new libgcc dependency and no unjustified
   extension.
3. User/kernel boundary: every user pointer is range-checked and prefaulted or
   copied through the usercopy helpers; no raw user dereference in kernel context.
4. Privilege separation: ring 3 cannot map, read, or write kernel memory or the
   page-cache window (`PCWIN_BASE`); `winprobe` cannot survive its window read.
5. Fault policy: a user fault terminates or signals the process; a kernel fault
   panics with a diagnostic; no silent recovery from kernel corruption. (A default
   CPU-exception handler is required, not only the page fault.)
6. Storage durability: writeback errors propagate, dirty pages are retained on a
   failed sync or evict, writes flush to media, and the persist and crash tests
   pass; no ignored I/O failure on a durability path.
7. Resource lifetime: every fd, OFD, socket, pipe, pty, framebuffer, process,
   page, VMA, kernel stack, and cache slot has a single owner and release path; no
   leak, double release, or stale pointer.
8. Concurrency: IRQ/syscall shared state has explicit locking or IF discipline, and
   every wait queue scans-then-parks safely; no lost-wakeup window, no unprotected
   shared network/audio/tty state.
9. ABI: syscall numbers, structures, errno returns, auxv, TLS, signal frames, and
   the stat/getdents/ioctl layouts match what the vendored musl/BusyBox/Dropbear
   actually use; no undocumented ABI drift.
10. Bounds and overflow: every address, length, sector, packet, path, ELF, and
    manifest calculation rejects wrap and truncation; a clamp-and-continue is
    documented and tested or it is a defect.
11. Fail-closed security: the random devices and `getrandom` fail without trusted
    entropy; malformed packets, images, ELFs, and manifests are rejected; a failed
    device probe leaves inert state.
12. Test evidence: each subsystem has a guest probe, a host seam test, a musl test,
    a capstone, or an explicit accepted gap; no untested critical or high behavior.
13. Documentation: the owning Readme and the comments describe current behavior and
    its limits; no stale contract doc.
14. Organization: a file has one coherent concern and a maintainable size; no dead
    code, no commented-out code, no perpetually-disabled debug path left shipping.
15. User-visible behavior: `os-simple` and `os-real` boot to usable shells, PID 1
    reaps and respawns, Dropbear SSH works, the network utils work over SLIRP, and
    the rescue shell never masks a real init failure.

## Method, common to every phase

Each phase is one Workflow fan-out that `/goal` runs to completion, the maintainer
reviewing at its gate. The shape is fixed and Codex is woven into every phase:

* Fan out parallel auditors, one per file or concern cluster, each handed the
  concerns below with their `file:line` anchors and the rubric. An auditor reads;
  it does not fix.
* Every candidate finding is handed to a separate adversarial verifier whose job is
  to refute it. A finding counts only if it survives a hostile second read; most do
  not.
* Codex independently re-verifies the phase: it re-reads the promoted findings and
  the phase surface against the rubric and the code, tries to refute each, and
  rejects the phase if the fan-out missed a concern in a file the phase owns. Its
  verdict is recorded with the phase token. This is the required Codex step at every
  phase, not only the hard ones.
* Confirmed findings become scoped fixes, one logical change each, in the house
  one-line no-attribution style. This review is licensed to do more than the prior
  audits: it may add tests, split an over-large file along its seams, and rewrite a
  Readme. Three rules order that work. First, a phase writes the host and guest tests
  its own gate needs, so the test-extension is distributed and a gate never depends
  on a test a later phase has not written yet; Phase 24 then automates the full
  suite, wires the unautomated artifacts, and fills the remaining cross-cutting gaps.
  Second, a contract or behavior change updates its owning Readme in the same commit
  (`CLAUDE.md`); Phase 27 is a coherence-and-rewrite pass, not where earlier drift is
  batched. Third, the large structural file splits (`syscall.c`, `hda.c`,
  `kernel.c`) are deferred to the structure phase (Phase 26), so every semantic phase
  reviews its handlers in place and no later phase references a moved file. A
  behavior or contract change is flagged before it lands.
* Each phase ends at its gate and its named token; the maintainer reviews, then
  `/goal` proceeds. A phase retires only when its gate is green and it weakens no
  earlier phase.

Findings carry a severity (critical, high, medium, low). Critical and high are
proven to a yes/no; medium and low are confirmed or refuted and may be batched.

The phases are risk-ordered: the kernel core that can corrupt memory, persist bad
data, or break the privilege boundary goes first, so later phases stand on verified
invariants; drivers, network, userland, and the cross-cutting test/build/doc passes
follow; the capstone and a final independent adversarial review close it.

## Phase 0: baseline, scope, and the prior-audit regression map

Scope: record the green baseline, fix the scope, and confirm the two prior audits
still hold. The `C_SRC` source-list guard and the build inventory
(`Makefile`/`rach2/Makefile`/`mk/`). Nothing is fixed here.

Work: capture `make verify-style` (8/8), `verify-host`, `make test` (28/28),
`check-sources`, the ATA and AHCI disk probes, and a default boot, all green, as the
reference every later phase diffs against. Confirm `git diff`-clean. Build the
file inventory and confirm every changed contract file is owned by a later phase.
Re-verify the `09-audit` and `11-audit` findings are still covered (the AHCI fixes,
the `!user_mode` window gate, the crash-test image isolation, the source-list
fail-closed) by re-reading the load-bearing invariants against the current tree.

Gate: baseline recorded; the map accounts for every in-house source; the prior-audit
invariants re-confirmed. Codex replays the inventory, compares the phase map to
`CLAUDE.md` and the two prior plans, and rejects any later phase whose file claims
are absent. Token `REVIEW-00-BASELINE`.

## Phase 1: boot, traps, entry, and the kernel core file

Scope: the multiboot entry (`loader.s`), the GDT/TSS/IDT (`tables/`), the ISR router
and trap epilogue (`isr.c`, `isr_handler.s`), the PIC/PIT, and the boot sequence and
self-test/debug-shell glue in `kernel.c`.

Concerns to chase:

* (high) Only vector 14 has a handler. Every other CPU exception (0-13, 15-31)
  dispatches into a router that, with no registered handler, silently returns and
  re-faults, ending in a triple-fault reset with no diagnostic (`sys/isr.c:81-107`).
  Install a default exception handler that panics on a kernel fault and kills the
  faulting ring-3 process on a user fault, printing the vector, error code, and EIP.
  Rubric 5.
* The trap/IRQ entry preserves ring-3 state, the segment selectors, and the IF
  discipline before `iret`; the saved-register layout matches `isr_params_t` and the
  `init_proc` offset assertions (cross-checked with Phase 8's switch.s audit).
* (medium, organization) `kernel.c` is 2645 lines fusing the boot sequence, the
  self-test battery, and the debug shell. Flag the split (the self-tests and the
  debug-shell dispatch into their own units, leaving `k_main` and the boot flow
  lean); the split itself is deferred to the structure phase (Phase 26), after this
  phase's review and tests. Rubric 14.
* Boot ordering: no init step, IRQ enable, or self-test can run before its
  prerequisite (paging before the cache, the cache frames before any window touch,
  the PIC before `sti`); the rescue and no-init paths are correct.

Gate: a default-exception boot trace (a deliberate fault panics with a vector, not a
silent reset); the boot-order trace; `make test` 28/28 after the split; verify-style
8/8. Codex walks `k_main` top to bottom annotating every dependency and IRQ-enable
point, and traces the saved-register layout against the offset asserts. Token
`REVIEW-01-BOOT`.

## Phase 2: the physical-memory base

Scope: `sys/mm/frame.c` (the multiboot map walk, the frame bitmap, the 32 MiB cap),
`sys/mm/heap.c`, the early reservations in `kernel.c`, and the linker layout.

Concerns to chase: the frame accounting, the reserved ranges, the kernel heap, and
the static DMA regions (the AHCI/HDA/SB16 buffers below `HARD_MEMORY_LIMIT`) are
mutually consistent and below the reachable physical limit; `mem_reserve` /
`alloc_user_page` / `alloc_kernel_frame` cannot collide; frame exhaustion returns a
graceful failure, not corruption. Confirm the heap's free-list and alignment.

Gate: `verify-host` (`frame_hosttest`), a boot, and a reasoning proof of the
DMA-region/frame consistency. Codex computes the frame ranges from the map handling
and checks every cited static DMA buffer stays in range. Token `REVIEW-02-FRAMES`.

## Phase 3: paging, the user boundary, and usercopy

Scope: `sys/mm/paging.c` (init, `CR0.WP`, `vm_resolve`, prefault, COW, the fault
handler, `map_mmio`), `sys/mm/vm.c`, `sys/mm/usercopy.c`.

Concerns to chase:

* (medium) `map_mmio` installs page tables only into `_kernel_dir` with no premap, so
  a runtime call after the first fork is invisible to already-forked processes
  (`paging.c:471-512`). Boot-time MMIO is safe; the `ahci` debug command's runtime
  `ahci_init -> map_mmio` is the latent path. Premap the device-MMIO PDE band at
  boot, or forbid/guard runtime `map_mmio`, and document the contract. Rubric 7/8.
* The fault handler branch order (COW, then VMA demand-paging, then the
  `!user_mode`-gated window branch, then segfault/panic) and the
  `USER_ADDR_MAX == PCWIN_BASE` boundary; no user fault reaches `block0->read`.
  Rubric 4.
* `vm_prefault_range` self-defends against `addr+len` overflow (Phase 23 also owns
  this); every user range obeys `[0x02000000, 0xC0000000)` half-open.
* Tests to add: the `cow_fault` sole-owner in-place re-grant branch (distinct from
  the shared-copy branch), frame exhaustion to a clean `-EFAULT`/segfault, and a
  `strncpy_from_user` that crosses into a legitimately-unmapped trailing page.

Gate: `verify-host` (`vm`, `pagecache` seams), the `vmprobe`/`cowprobe`/`forkro`/
`vmro`/`vmfault`/`winprobe` probes, plus the new tests. Codex enumerates every fault
branch and proves the user/window separation. Token `REVIEW-03-PAGING`.

## Phase 4: the page-cache window

Scope: `sys/mm/pagecache.c` (eviction, fault, sync, backing switch, capacity, the
window probe) and `pagecache.h`.

Concerns to chase: a dirty page is never silently dropped on a failed writeback or
evict; a kernel touch of the window above the backed file-system size panics (the
intended fail-stop, confirmed); `pc_set_capacity` does not ignore an eviction
failure; the open-addressed slot hash is correct under tombstoning. Test to add: the
writeback-failure-then-skip path (the host test's fake writer never fails today).

Gate: `verify-host` (`pagecache_hosttest`, with the new failure case), a
`rach2.fsdisk` boot reading and writing across faults and evictions. Codex traces the
dirty-bit transitions across `pc_fault`/`pc_evict`/`pc_sync`/`pc_set_backing`. Token
`REVIEW-04-PCACHE`.

## Phase 5: the block layer and the KRFFS disk mount

Scope: `sys/block/{block,ata,ahci}.c` and the kernel disk-mount flow in `kernel.c`
(`init_krffs_disk`, the MBR probe, the dirty bit, the fsck contract). AHCI was
audited in milestone 11; survey it only for whole-system consistency.

Concerns to chase: the read/write/flush bounds in both drivers are identical and
correct; the partition view forwards and clamps correctly; the MBR probe and the
size-vs-window-cap arithmetic are overflow-safe; the dirty-flag lifecycle and fsck
recovery hold; the default RAM and `if=ide` boots stay byte-identical.

Gate: `test-disk`, `test-disk-ahci`, `persist-test`(-ahci), `crash-test`; the
default boot unchanged. Codex re-verifies the bounds idiom and the mount arithmetic.
Token `REVIEW-05-BLOCK`.

## Phase 6: the syscall surface and the ABI

Scope: `sys/syscall.c` (7343 lines, ~110 handlers) and the user/kernel ABI.

Concerns to chase:

* (high, organization) The file is one 7343-line translation unit. Plan the split
  into per-family units (file, proc, signal, memory, socket, ioctl, time) behind a
  shared internal header and fix the family boundaries here, but defer the physical
  split to the structure phase (Phase 26), after every semantic phase (7, 9, 10, 14,
  15) has reviewed its handlers in `syscall.c` in place. The split is the largest
  structural change of the review; it must keep `make test` byte-identical and
  preserve every Doxygen contract comment. Rubric 14.
* Every syscall validates its user pointers through the usercopy helpers; no raw
  user dereference; errno returns are correct (the right negative errno, not a clamp
  or a wrong code); nested/indirect buffers (iovecs, socket addresses, `argv`) are
  bounds-checked. Rubric 3/9/10.
* Write the missing syscall-ABI Readme on the `mm`/`block` model: the dispatch, the
  usercopy contract, the per-family surface, the documented musl/BusyBox needs.
* (medium) `do_wait` re-implements the lost-wakeup sleep loop instead of reusing
  `wq_wait_event`; reconcile (Phase 7 owns the proc side).

Gate: `make test` 28/28, `verify-style`, `verify-host`, the SSH and BusyBox
capstones (the real ABI consumers); the per-family usercopy/errno audit complete and
the split's family boundaries fixed. Codex re-checks the usercopy discipline family
by family and confirms the family map is sound for the Phase 26 split. Token
`REVIEW-06-SYSCALL`.

## Phase 7: the process lifecycle

Scope: `sys/proc/{process,sched,wait_queue}.c` and the fork/exit/wait/reparent paths
in `syscall.c`.

Concerns to chase:

* (high) A per-fork kernel stack is `mem_reserve`'d and never reclaimed on reap, a
  steady leak across fork/exit churn (`syscall.c:3471-3518`, `sys_fork`). Pool the
  stacks or free the reservation on reap. Rubric 7.
* The lost-wakeup-safe sleep (`wq_wait_event` and the duplicated `do_wait` loop): the
  scan-then-park is `cli`-bracketed across the `signal_pending` check and the
  `BLOCKED` set; reconcile the duplication with Phase 6.
* `init` death, orphan reparenting, the zombie/reap contract, and the scheduler's IF
  discipline and round-robin fairness.

Gate: `make test` (`forktest`, `jobprobe`, `orphanprobe`, `waitprobe`), a fork-churn
boot that shows stable kernel-frame occupancy, and a new test for the fork
stack-lifecycle. Codex traces the stack allocation/free across fork and reap. Token
`REVIEW-07-PROC`.

## Phase 8: exec, ELF, the initial stack, and TLS

Scope: `sys/proc/{elf,uimage,tls}.c`, `sys/proc/switch.s`, and the execve teardown.

Concerns to chase: the ELF loader's hostile-image hardening (segment bounds, the
window/overlap rejection, the protect-after-copy ordering under `CR0.WP`); the
`build_user_stack` aux/argv/envp layout; the TLS GDT update on context switch and
the `switch.s`/PCB offset contract; the destructive-exec-after-teardown ordering and
the GP-register reset. Tests to add: the ELF loader's rejection paths (a truncated
header, a segment past the window, an overlapping pair) have zero negative coverage
today.

Gate: `make test` (`shelltest`, `auxvprobe`, `tlsprobe`), the new ELF-rejection
tests, the SSH/Doom capstones (real execs). Codex re-checks the offset contract and
the teardown ordering. Token `REVIEW-08-EXEC`.

## Phase 9: signals and job control

Scope: the signal mechanism (currently split between `syscall.c` and its own home),
signal frames, masks, stops, `sigreturn`, and Ctrl-C delivery to the foreground
group.

Concerns to chase: the signal mechanism's organization (it lives outside the cluster
it belongs to; consider giving it a coherent home and Readme); the sigreturn frame
restoration (full GP set including `eax`) stays independent of fork and exec; the
stop/cont status encodings; `Ctrl-C`/`SIGINT` to the foreground process group across
the controlling terminal.

Gate: `make test` (`sigprobe`, `jobprobe`), a manual Ctrl-C/job-control exercise.
Codex re-verifies the sigreturn/fork/exec independence and the stop encodings. Token
`REVIEW-09-SIGNAL`.

## Phase 10: the fd and OFD layer

Scope: `sys/fs/fd.c`/`fd.h`, the file-ops vtable, dup/fork/CLOEXEC, refcounts.

Concerns to chase:

* (high) `fd_entry.node` holds a raw `krffs_node_t *` with no refcount or pin, so a
  concurrent `unlink`/resize/relocate can leave a stale pointer (`fd.h:93-101`,
  `krffs.c`). Decide and implement: a refcount/pin on the open node, or a documented
  invariant that the single-threaded IF=0 model makes it safe, with a test that
  proves the chosen contract. Rubric 7.
* The OFD lifetime across dup/fork/CLOEXEC-exec and death; every fd-backed object
  (pipe, socket, pty, fb, synth) releases through one path.

Gate: `make test` (`dup2test`, `ofdprobe`, `pipeprobe`), the new stale-node test.
Codex traces the node-pointer lifetime against unlink/resize. Token `REVIEW-10-FD`.

## Phase 11: the KRFFS core file system

Scope: `sys/fs/krffs.c` (the allocator, the circular-node walker, paths, dirs,
write/truncate/relocate).

Concerns to chase:

* (high, coverage) The allocator and walker have no host unit test; a bug corrupts
  persistent disks. Add a host-side KRFFS allocator/walker test (the
  `synthfs`/`pagecache` host pattern) locking down split/coalesce/relocate and path
  resolution. Rubric 12.
* (medium) `creat`/`O_CREAT` on an existing directory silently succeeds
  (`krffs.c:924-937`); decide the correct errno.
* The in-place circular-list mutation under writes, the dirty-flag write-through, and
  the root-node invariants.

Gate: the new host test green under ASan/UBSan, `make test` (`dirtest`, `stattest`),
a `rach2.fsdisk` boot. Codex adversarially drives the allocator edges. Token
`REVIEW-11-KRFFS`.

## Phase 12: the synthetic file systems

Scope: `sys/fs/{devfs,procfs,synthfs}.c` (the `/dev` and `/proc` classifiers).

Concerns to chase: the central VFS classifier; the `/proc/<pid>` bounds (a crafted
huge pid cannot index past the proc table); device lseek/numbers and the
`copy_to_user` targets cannot reach the window; the getdents/stat shapes.

Gate: `make test` (`devprobe`, `procprobe`, the `synthfs_hosttest`). Codex re-checks
the pid bounds and the copy-to-user targets. Token `REVIEW-12-SYNTHFS`.

## Phase 13: TTY, console, and pseudo-terminals

Scope: `sys/tty/{tty,n_tty,pty}.c`, the keyboard/serial input, sessions, and the
controlling-terminal model.

Concerns to chase: the N_TTY line discipline (canonical editing, ISIG, the echo
suppression asymmetry between the keyboard IRQ and the serial drain); the
console/pty controlling-terminal lifecycle (the session-leader invariant); the
foreground-group and blocking-read/poll interplay. The pty half explicitly: the
`/dev/ptmx` open and the `TIOCGPTN`/`TIOCSPTLCK` unlock path, `/dev/pts/N`
resolution, the master/slave close ordering and the leader-ctty release, and the
musl/Dropbear pty session lifecycle.

Gate: `make test` (`ttyprobe`, `ptyprobe`, `jobprobe`, `n_tty_hosttest`), the SSH
capstone, a manual type-ahead/Ctrl-C/line-editing exercise. Codex re-verifies the
controlling-terminal invariant and the echo model. Token `REVIEW-13-TTY`.

## Phase 14: wait queues, poll/select, and pipes

Scope: `sys/proc/wait_queue.c`, `sys/io/{poll,poll_select}.c`, `sys/ipc/{pipe,
pipe_ring}.c`.

Concerns to chase: the scan-then-park lost-wakeup safety generalized to many queues;
the `fd_set`/pollfd translation (the fence-post arithmetic, direction gating); the
pipe ring's EOF/SIGPIPE/nonblock and the closable-stdio wake path.

Gate: `make test` (`pollprobe`, `pipeprobe`, the `poll_select`/`pipe_ring`/
`wait_queue` host tests). Codex re-checks the park/dequeue and the translation
edges. Token `REVIEW-14-POLL`.

## Phase 15: the network core, locking, and sockets

Scope: `sys/network/core/net_core.c`, `sys/network/socket/socket.c`, and the
`net_lock` IF-save discipline bracketing the shared TX buffers, the TCB/socket
pools, and the ARP cache.

Concerns to chase: every external TX entry brackets the shared static buffers under
`net_lock`; the SPSC lock-free recv ring is correct; the socket layer's
getsockname/getpeername/shutdown/setsockopt fill-out and the poll wakeups; no
unprotected shared state between the IRQ and syscall contexts. Rubric 8.

Gate: `make test` over the network probes (wired in Phase 24), the SSH and httpd
capstones. Codex audits the lock discipline at every TX site. Token
`REVIEW-15-NETCORE`.

## Phase 16: TCP

Scope: `sys/network/tcp/tcp.c` (1002 lines: the state machine, timers, retransmit,
the accept queue, FIN/TIME_WAIT, the socket wakeups).

Concerns to chase:

* (high) Closing-state data loss and unguarded FIN handling: `tcp_rx` in FIN_WAIT_1/
  FIN_WAIT_2 (`tcp.c:936-964`) can lose in-flight data and mis-handle a FIN
  transition. Fix the closing-state transitions and the data delivery.
* (medium) Send-side backpressure: `tcp_send` (`tcp.c:362-396`) and the socket
  `POLLOUT` report writable when the send ring is full, so a writer spins or loses
  data. Add honest backpressure and a truthful `POLLOUT`. Rubric 8/10.

Gate: a host-seam TCP test driving the state machine and the new backpressure
(written this phase), the SSH capstone over a long session, an httpd transfer larger
than the send ring. Codex re-verifies the closing-state and backpressure fixes. Token
`REVIEW-16-TCP`.

## Phase 17: IP/UDP/ARP/ICMP/Ethernet and the RTL8139 driver

Scope: `sys/network/{ip,udp,arp,icmp,ethernet}/*.c`,
`sys/network/driver/rtl8139.c`, and the network headers (`net.h`, `byteorder.h`).

Concerns to chase: the packet parsing and checksum handling reject a malformed or
short frame; the IP/UDP/ARP/ICMP paths bound every length; the rtl8139 RX/TX ring
and descriptor handling, the PCI probe, and the IRQ path are correct. Rubric 10/11.

Gate, with per-layer evidence: the malformed/short-frame rejection at each of
Ethernet, IP, UDP, ARP, and ICMP; the checksum host-seam tests (written this phase);
the rtl8139 RX-ring wrap, the TX-descriptor ownership, and the IRQ handoff; the
`pingtest`/`udptest` probes. Codex re-checks the checksum and ring math at each
layer. Token `REVIEW-17-NETLOW`.

## Phase 18: the audio drivers

Scope: `sys/audio/{audio,audio_debug,dma,sb16,hda}.c` and the audio headers
(`audio.h`, `dma.h`, `sb16.h`, `hda.h`).

Concerns to chase:

* (medium, organization) `hda.c` is 2907 lines fusing controller bring-up, codec
  enumeration, the ring/format/resampler, and DMA. Flag the split along its
  bring-up/enumeration/stream seams; the split itself is deferred to the structure
  phase (Phase 26), after this phase writes the audio host tests (below) so the
  split is verified against real coverage. Rubric 14.
* (medium) A perpetually-disabled debug logging path ships inside `hda.c`; retire it
  or gate it behind a boot flag. Two concurrent consumers of the HDA playback ring
  and the LPIB-tracking state need a documented single-owner contract.
* The ISA-DMA `<16 MiB`/64 KiB-boundary buffer placement and the static-buffer
  virt==phys cast stay correct after any move.

Gate: a manual audio play over SB16 and HDA (the only exercise today), plus the first
automated audio coverage, host-side unit tests for the ring/format/resampler/DMA
bounds written this phase so they exist before the Phase 26 split. Codex re-verifies
the DMA-buffer invariants. Token `REVIEW-18-AUDIO`.

## Phase 19: display, peripherals, and core hardware

Scope: `sys/display/{vga_text,vga_gfx}.c`, `sys/pci.c`, `sys/kbd.c`, `sys/mouse.c`,
`sys/speaker.c`, and the core glue `sys/{pic,pit,serial,kio,module,cmdline,panic}.c`.

Concerns to chase:

* (medium, dead code) A dead `init_keyboard`/`_keyboard_test` path with a misleading
  comment (`kbd.c`), and an `init_pci` that is never called and whose contract does
  not match the code (`pci.c:366`). Excise or give each a real, documented role.
  Rubric 14.
* Stale header contracts, the speaker divisor clamp, the framebuffer ownership and
  the VGA bounds, the PCI BAR helpers, the cmdline tokenizer (the truncation fix from
  milestone 11 stays correct).

Gate: a boot exercising VGA text + mode-13h (Doom), the PS/2 keyboard/mouse, and the
speaker; `make test`. Codex re-checks the dead-path removal and the BAR/bounds. Token
`REVIEW-19-DEVICES`.

## Phase 20: the freestanding library and randomness

Scope: `sys/lib/{kctypes,kstd,kstring,krandom}.c`.

Concerns to chase:

* (medium, UB) `k_itoa` negates `INT_MIN` in signed `int` (`kstd.c:51-52`); fix the
  overflow. `k_strreverse` underflows on an empty string (`kstring.c:8-16`); guard it.
* (high, coverage) `kstd.c`/`k_itoa` has no test at all, and the `kstring` host test
  covers neither `k_strreverse` nor the case-fold helpers. Add an ASan/UBSan host
  test that compiles `kstd.c` and covers the missing functions. Rubric 12.
* `kstring.c`/`kctypes.c` violate the comment/voice policy (body narration); bring
  them to the contract (Phase 26 owns the tree-wide sweep, fix these here as the
  phase touches them). Confirm `krandom`'s fail-closed contract holds at both gates.

Gate: the new and existing host tests green, `make test` (`rngprobe`). Codex
re-verifies the UB fixes and the fail-closed gates. Token `REVIEW-20-LIB`.

## Phase 21: the in-house userland

Scope: `utils/` (init and the simple Unix tools, the network utils httpd/nc/ping/
resolv) and the `ish/` shell (935 in-house lines, its own Readme, wired into the
build): its parser, builtins, syscall wrappers, build, and Readme.

Concerns to chase: `init.c`'s PID-1 reaper/respawner correctness; the `ish/` shell's
parser, builtins, and syscall-wrapper bounds; the simple tools' bounds and
one-author voice; the network utils' robustness over SLIRP. Rubric 15.

Gate: the SSH/BusyBox/httpd/ping capstones, the `os-simple` boot to the `ish` shell,
`orphanprobe`. Codex re-checks the init lifecycle and the `ish` parser. Token
`REVIEW-21-UTILS`.

## Phase 22: the KRFFS host tooling

Scope: `krffs/*.c` (mkfs, fsck, defrag, edfs, fsviz, stageimage, `krffs_node.c`,
`krffs_platform.c`), excluding `krffs/vendor`.

Concerns to chase:

* (medium, security) `defrag.krffs` runs fsck via `system()` with an untrusted path
  concatenated into a shell string (`defrag.krffs.c:105-125`); replace with a direct
  exec. `edfs.krffs` parses 64-bit on-disk fields with 32-bit `strtol()` and silently
  no-ops (`edfs.krffs.c:106,137,162,174,186`); fix the width and give feedback. Rubric
  10/11.
* (medium) The `defrag` slide loop self-modifies the ring under `memmove` and is
  untested; validate it and the allocator under new ASan/UBSan host tests.

Gate: the new host tests green, a defrag/fsck round-trip on a staged image. Codex
re-verifies the slide and the `system()` replacement. Token `REVIEW-22-TOOLING`.

## Phase 23: cross-cutting security

Scope: the whole trust boundary, consolidated: `usercopy.c` and every syscall caller,
the `!user_mode` gates, the integer-overflow surfaces, the CSPRNG, the DMA buffers
(audio/net/block), and the `/proc` and `/dev` access.

Concerns to chase: `vm_prefault_range` self-defends against `addr+len` overflow; the
raw hand-offs behind `FB_SETPALETTE` and `AUDIO_WRITE` are bounced or have a proven
pinned invariant; ring-3 cannot reach a privileged path; the negative/boundary test
surface for the trust boundary (thin today, only `errnoprobe`) is widened. Rubric
3/4/10/11.

Gate: the security probes (`winprobe`, `vmfault`, the widened negative tests from
Phase 24); a reasoning pass over the trust boundary. Codex independently re-audits
every user-pointer entry and tries to construct a ring-3 escape. Token
`REVIEW-23-SECURITY`.

## Phase 24: the test suite (extend it)

Scope: `tests/**`, `tools/runtests.py`, `tools/harness.py`. This is the
automation-and-consolidation pass for the suite.

Work: each earlier phase already wrote the host and guest tests its own gate needed
(the KRFFS allocator/walker, `kstd`/`k_itoa`, the audio ring/format/DMA bounds, the
network checksum/RX-ring/TCP state, the page-cache writeback-failure, the `cow_fault`
sole-owner branch, the ELF-rejection paths, the fork stack lifecycle, the fd
stale-node contract, the widened trust-boundary negatives). Phase 24 wires every
artifact into a make target through `harness.py` (the freestanding probes not yet in
`make test`, the 19 musl integration tests, the network probes, and the by-hand fault
probes including `winprobe`), confirms each test sits in a gate, fills any remaining
cross-cutting coverage gap no single phase owned, and decides whether a CI
configuration is in scope or an explicit, recorded deferral. Every test has a token
and a make target.

Gate: every new and existing test green; `make test` count grows and stays
deterministic; no flaky probe. Codex re-verifies each new test actually exercises the
contract it claims (no weak assertion, the FAIL path reachable). Token
`REVIEW-24-TESTS`.

## Phase 25: the build system and support scripts

Scope: `Makefile`, `rach2/Makefile`, `mk/`, the full `tools/` set (harness,
runtests, check, check_sources, doctor, hosttest, textcheck, gen_compile_commands,
the `cppcheck.suppress` list, mkdisk*, grub_embed, crash_test, mkiso), `rach2/*.sh`,
`krffs/*.sh`, the manifests.

Concerns to chase: pin the unpinned, network-dependent Docker base for the ISO and
GRUB-disk builds and stop swallowing `grub-mkrescue` stderr; replace
`crash_test.sh`'s fixed-sleep timing with a serial-driven wait; enforce (or
single-source) the geometry coupling between `crash_test.sh` and `mkdisk.sh`; sweep
the gate scripts for any other false-green or staleness.

Gate: `verify-style` (shellcheck), `check-sources`, a clean run of every disk target
and the ISO/GRUB build. Codex replays the gate construction and the false-green
cases. Token `REVIEW-25-BUILD`.

## Phase 26: structure, comments, and style, tree-wide

Scope: the large-file structural splits deferred from the earlier phases, and the
comment style across the whole kernel, against `CLAUDE.md`'s Comments and Writing
sections. This runs after every semantic phase has reviewed its handlers in place and
written its tests, and before the documentation phase, so the docs describe the new
structure.

Work:

* The deferred structural splits, each a pure mechanical reorganization that keeps
  `make test` byte-identical: split `syscall.c` (7343 lines) into the per-family
  units behind the shared internal header whose boundaries Phase 6 fixed; split
  `hda.c` (2907) along the bring-up/enumeration/stream seams, verified against the
  audio host tests from Phase 18; split `kernel.c` (2645)'s self-tests and
  debug-shell dispatch out of the boot flow. Each split is its own commit, updates
  `C_SRC` and the `check-sources` guard and the `-MMD` deps, preserves every Doxygen
  comment, and is proven byte-identical on `make test`.
* The comment sweep: reflow the ~470 over-80 block-comment-body wraps and the ~44
  over-80 code lines; convert the legacy files' body narration to the contract (a
  header block, no reasoning trace), worst offenders first (`syscall.c` has 254
  in-body comments); normalize the Title-Case diagnostics (e.g. `hda.c`'s `HDA:`) to
  the lowercase `component: detail` shape tree-wide; confirm every function carries a
  Doxygen `/**` header consistent with its siblings.

Gate: `make test` byte-identical after each split; `verify-style` 8/8;
`check-sources` clean; a sample re-read confirming each touched comment still
describes the code. No behavior change. Codex audits each split for a dropped symbol,
a moved comment, or a build-graph error, and spot-checks the comment voice. Token
`REVIEW-26-STRUCTURE`.

## Phase 27: documentation, structure and rewrite

Scope: every in-house Readme and the doc set as a whole, now reflecting the file
structure Phase 26 left. The per-phase contract-drift fixes already landed inline
under the same-commit rule, so this is the coherence-and-rewrite pass.

Work:

* (high) Rewrite the stale ~1356-line kernel Readme (`rach2/Readme.md`), a
  foundation-era development diary in the exact voice `CLAUDE.md` forbids, into a lean
  current overview that points at the subsystem Readmes.
* (high) Fill the missing subsystem Readmes (`audio`, `display`, `lib`, `proc`,
  `tables`, `tty`, and the syscall-ABI Readme) on the proven `mm`/`block`/`fs`
  template, describing the post-split structure; reconcile the two-generation doc
  split so the whole set reads as one author.
* Confirm the per-phase inline Readme updates landed, and fix any residual contract
  drift; decide the table-vs-prose question per `CLAUDE.md`; confirm `dev-log/` stays
  historical.

Gate: `verify-style` over the docs; a read-through confirming every Readme states the
current contract and the set is coherent. Codex reviews the structure and the voice,
the part no checker enforces. Token `REVIEW-27-DOCS`.

## Phase 28: the capstone matrix and whole-system manual pass

Scope: nothing new is read; the end-to-end exercise of every contract on emulated
hardware. The `os-simple` and `os-real` boots, BusyBox `ash`, the Dropbear SSH
capstone (pubkey, interactive shell, `rc=0`), the network utils over SLIRP, Doom, and
the disk/AHCI capstones (ATA and SATA, persist, stress, crash).

Gate: every capstone green over both the RAM and disk boots and both block buses; the
default boot byte-identical to the Phase 0 baseline. Codex confirms the matrix is
complete. Token `REVIEW-28-CAPSTONE`.

## Phase 29: final adversarial review and retirement

Scope: the cumulative change. Codex does a single independent final pass over the
whole review diff (every phase's fixes, splits, tests, and doc rewrites), tries to
refute the result, and confirms no critical or high finding is open, the prior-audit
findings remain covered, every contract change has a matching test and doc, and the
vendored trees and the Doom port stayed out of the diff. Write the milestone record
(`dev-log/12-review/Readme.md`), update `dev-log/Readme.md`, and retire.

Gate: the full global gate (below) green as a whole; Codex's final verdict
`AUDIT-COMPLETE`. Token `REVIEW-29-COMPLETE`; the milestone is retired.

## Verification

The global gate, run after every behavior-touching phase and once more as a whole at
close:

* `make doctor` (the toolchain check) and a clean `make` and `make full` build.
* `make verify-style` (8/8) and `make verify-host`.
* the module-path `make test` (28/28 or the grown count, deterministic) and a
  default RAM boot byte-identical to the Phase 0 baseline.
* the ATA and AHCI disk probes (`test-disk`(-ahci), `persist-test`(-ahci),
  `stress-test`(-ahci)) and `crash-test`.
* the `os-simple` boot to a usable shell, and the `os-real` boot to BusyBox with
  PID 1 reaping, the Dropbear SSH capstone, the network utils over SLIRP, and Doom.

After a risky change, the narrow affected tests run first, then the global gate
before the phase token. The vendored trees and the Doom port stay untouched
throughout; the probe tokens, the module test set, and the byte-identical default
boot are contracts not changed without a flag. Every phase's Codex verdict is
recorded with its token.

## What stays deferred

* A real CI configuration is decided in Phase 24 (in scope or an explicit, recorded
  deferral); this plan does not assume a CI host exists.
* Reproducibility hardening beyond pinning the Docker base (a fully offline ISO/GRUB
  build) is recorded as debt if the pin does not close it.
* Any subsystem rewrite that is taste rather than a confirmed defect or a stated
  organization criterion (rubric 14) stays out; the review fixes and restructures,
  it does not gold-plate.
* The vendored trees and the Doom port: no change, ever, this milestone.

## Baseline

The green state to diff against is `HEAD` at `b045704` (the milestone-11 retirement).
Phase 0 records it: `verify-style` 8/8, `verify-host`, `make test` 28/28, the ATA and
AHCI disk probes, a default boot, the SSH capstone, and Doom. Every later phase diffs
against this recorded state, so a regression is attributable to the phase that
introduced it.
