# Milestone 9: the post-disk audit

Status: complete. All ten phases executed and gated; see `Readme.md` for the
milestone record.

## The goal

The disk milestone and its four follow-up fixes landed about 3700 lines across
fifty files: the file system moved off the 32 MB RAM module onto a real ATA disk,
behind a kernel page cache and a demand-paged kernel-virtual window, and three
small fixes touched execve, the keyboard IRQ, and the Doom runtime. This audit
proves the new code correct, proves the unchanged code unbroken by the ripple, and
brings every comment, Readme, and support file back into agreement with the code.
It is a four-dimension sweep (code correctness, comments, documentation, support
files) executed phase by phase, in the milestone-6 discipline: every candidate
finding is adversarially verified before it counts, and most are expected to be
refuted.

## Blast radius this audit answers to

The disk milestone (`4029b7a..7a2b8b2`) added:

* a kernel page cache (`sys/mm/pagecache.c`): `PC_SLOTS=256` frames reserved at
  boot in `init_pagecache`, behind a demand-paged window at
  `PCWIN_BASE=0xC0000000`, which equals `USER_ADDR_MAX`. Clock eviction, dirty
  tracking by write-protect, an open-addressed lookup hash.
* a block layer and polled PIO ATA driver (`sys/block/{block,ata}.c`), published
  as `block0` with a `block_partition` view.
* the window-fault branch in `_page_fault_handler` and `CR0.WP` set in
  `_load_page_dir` (`3172b3c`, a silent-corruption fix), plus
  `alloc_kernel_frame` / `premap_kernel_window` / `map_kernel_frame`.
* the three-way mount restructure in `kernel.c` (default RAM module /
  `rach2.fswindow` / `rach2.fsdisk` with an MBR root-probe), the
  `KRFFS_MODE_DIRTY` flag, fsck recovery, and the `disk` debug command.
* host tools (`krffs --offset/--size`, `mkdisk.sh`, `mkdisk_grub.sh`,
  `grub_embed.py`) and probes (`blockprobe`, `persistprobe`, `stressprobe`,
  `pagecache_hosttest`), plus `harness.py --drive`.

The four follow-ups:

* `a97ae6f` / `e95ba2b`: the keyboard IRQ now cooks a keystroke into
  `console_tty` as type-ahead whenever `wq_active(...) || console_tty.sid != 0`;
  the `kbd.c` boot stub was stripped to its scancode-ring job.
* `349ed54`: Doom's `crt0.c` `_start` reads `argc`/`argv` from `%esp` in raw asm.
* `21a1d57`: `sys_execve` zeroes `ebp/edx/ebx/ecx/esi/edi` for the new image.

The invariants the audit must protect, and confirm are documented right:

* `CR0.WP` stays set; the page cache's dirty tracking depends on it, and no kernel
  write to a read-only mapping may newly fault.
* The window fault polls (vector 14 is an interrupt gate, IF=0) and never sleeps.
* The fault-handler branch order is COW, then VMA demand-paging (`in_user_range`),
  then the window branch (`addr >= PCWIN_BASE`), then segfault/panic.
* The default boot is the RAM module; existing images and the test harness boot
  byte-identically. The cache still reserves 256 frames at boot even with no disk.
* The exec GP-reset does not break musl startup, fork, or signal/sigreturn frames.

## Scope and non-goals

In scope: the whole kernel, the host tools, the docs, and the support files, read
against the four dimensions. Out of scope and off-limits: the vendored trees
(`musl`, `busybox`, `dropbear`, and `krffs/vendor`, which holds `termbox2.h`) stay
untouched; a finding that would edit them is out of scope by definition. The Doom port is in-house and in scope. This audit
fixes only what adversarial verification confirms; it does not refactor for taste,
and it does not change a built artifact or a test contract (probe success tokens,
the 28-probe module set, the byte-identical default boot) without flagging it as a
contract change first.

## Method, common to every phase

Each phase, and each sub-phase of Phase 6, is one Workflow fan-out that `/goal`
runs to completion, with me reviewing between them. The shape is fixed:

* Fan out parallel auditors, one per file or per dimension, each handed the
  concrete concerns below with their `file:line` anchors. An auditor reads, it
  does not fix.
* Every candidate finding is then handed to a separate adversarial verifier whose
  job is to refute it: read the surrounding code, the callers, the host test, the
  invariant it supposedly breaks, and either kill the finding or promote it to
  confirmed with a reproduction or a tight argument. This is the milestone-6 rule:
  a finding does not count until it survives a hostile second read. Most do not
  survive.
* Confirmed findings become scoped fixes, one logical change each, committed in the
  house one-line style with no attribution. A text-only fix changes no behavior and
  no built artifact. Comments and docs are fixed in the phase that owns them, not
  scattered.
* Each phase ends at its verification gate (below). I review, then `/goal`
  proceeds.

The concerns carry a severity from the maps (P1 highest). P1 and P2 items are
verified by hand or by reasoning to a definite yes/no; P3 items are confirmed or
refuted but may be deferred if cosmetic.

## Phase 1: the page cache and the MM core

Scope: `sys/mm/pagecache.{c,h}`, the `sys/mm/paging.c` window branch and `CR0.WP`,
the frame reservation, and the boot ordering in `kernel.c` that drives them. This
is the milestone's heart and the highest-risk code; it goes first.

Concerns to chase:

* P1 `CR0.WP` regression. `CR0.WP` is now enforced kernel-wide, so a kernel write
  through a read-only *current* user mapping that does not pre-fault first is a new
  panic where it used to silently succeed. The cited write paths are already safe
  and are the model: `copy_to_user` pre-faults with `write=1` before `k_memcpy`
  (`usercopy.c:78-85`), the ELF loader writes into physical frames before
  `paging_set_writable` (`sys/proc/elf.c:405-443`), `vm_resolve` zeroes the
  physical alias (`paging.c:297-305`), and `cow_fault` copies via the physical
  alias (`paging.c:381-385`). The audit target is the inverse: find any kernel
  write through a user *virtual* address that bypasses `copy_to_user` /
  `user_buffer_ok`, because that is the only path `CR0.WP` can newly fault.
* P1 fault branch order and the `PCWIN_BASE==USER_ADDR_MAX` boundary
  (`paging.c:400-439`). A fault at exactly `0xC0000000` must take the window
  branch, never the user demand-pager; `pc_window_size()==0` must not mis-route a
  real cache fault to the panic path.
* P1 ring-3 into the window. `USER_ADDR_MAX` is exclusive (`usercopy.h:22-23`,
  `paging.c:400`), so a fault at exactly `0xC0000000` skips the user demand-pager
  and reaches the window branch (`paging.c:430-438`) before the segfault branch
  (`:444-449`). `pc_fault` then installs a supervisor-only PTE (`map_kernel_frame`,
  `paging.c:593-594`), `iret` retries, and a ring-3 reader faults again. This is
  not an infinite livelock (each retry warms the cache), but a ring-3 process can
  drive disk reads into a supervisor mapping it cannot read. Decide whether the
  window branch must gate on `!user_mode` and fall through to segfault for a
  user-mode fault.
* P2 frame-pool accounting (`pagecache.c:487-498`, `paging.c:520-532`). Confirm the
  256-frame (1 MB) reservation does not starve `alloc_user_page` on the default or
  a low-memory boot and that the byte-identical-harness-boot invariant survives the
  change to `_frames` occupancy.
* P2 eviction and dirty correctness under failure (`pagecache.c:216-244,261-290`).
  Trace the writeback-failure and read-fill-failure paths; confirm no dirty page is
  silently dropped and that `valid <-> hash <-> PTE` stay in lockstep across
  `pc_evict`, `pc_sync`, `pc_set_backing`, `pc_set_capacity`.
* P2 open-addressed hash under tombstoning (`pagecache.c:142-179`). Verify
  `pc_tombs` accounting, the rebuild trigger, and that find/insert/remove cannot
  lose an entry or fail to find a free bucket.
* P2 `pc_set_backing` clamp-then-round and the page-alignment invariant on
  `pc_window_span` (`pagecache.c:339-347,427-471`). Confirm the span is always
  page-aligned so the round-up cannot exceed it.
* P2 host-test gaps (`tests/host/pagecache_hosttest.c:79-156`): no coverage for
  writeback failure, read-fill failure, remount drop-without-writeback, capacity
  change with live dirty pages, or the write-protect upgrade-on-write-hit. Confirm
  or add coverage for these correctness-critical branches.
* P2 `test_pagecache` self-test restoration (`kernel.c:1322-1373`): confirm
  `pc_set_backing(0,0,0)` fully invalidates resident frames so the default module
  path never faults the window after the self-test, and that `pc_has_backing()`
  gates the destructive probe.
* P3 `pc_set_capacity` header vs behavior (`pagecache.c:354-371` vs
  `pagecache.h:71-75`), the misleading `!current_proc` panic message
  (`paging.c:430-439`), and the early-boot ordering (frames reserved, no backing,
  nonzero window size).

Verification gate: `make verify-host` (the page-cache and frame host tests pass,
including any added cases), `make verify-style`, and a real boot of the default RAM
path plus a `rach2.fsdisk` boot that mounts, reads, and writes. Confirm the VMPROBE
PAGECACHE token string and field order are unchanged.

## Phase 2: MM-VM and the user/window boundary

Scope: `sys/mm/vm.c`, `sys/mm/usercopy.c`, `sys/mm/heap.c`, and the `brk`/`mmap2`/
`munmap`/`elf_setup_vmas` layout edges in `syscall.c` and `elf.c`. These files did
not change, but the window now abuts the top of the user range, so every boundary
is load-bearing.

Concerns to chase:

* P2 `CR0.WP` and `copy_to_user` (`usercopy.c:66-87`, `paging.c:265-315,207-224`).
  A kernel write to a read-only or not-yet-writable user page must be intercepted
  by `vm_prefault_range(write=1)` returning 0 -> `-EFAULT`, never a kernel-mode
  write fault now that WP is enforced. Walk `vm_resolve`'s write-permission
  handling and the COW-vs-RO distinction.
* P2 layout-edge arithmetic (`syscall.c:7040-7066,7120-7131`, `elf.c:503-506`,
  `vm.h:24,57-60`). Confirm `brk` and any mmap region can never reach
  `>= PCWIN_BASE`; that `VM_MMAP_BASE < PCWIN_BASE`; that `mmap_top` cannot bump
  into the heap when `PAGE_UP(brk)` could wrap; that `brk` exactly
  `== stack_limit` is intended-legal; and that a heap-already-exists grow is caught
  by `vm_overlaps`.
* P3 `user_range_ok` upper bound and `vm_prefault_range` loop bound
  (`usercopy.c:14-29`, `paging.c:317-337`): every span is strictly half-open at
  `0xC0000000`, so no pre-fault steps one page into the window.
* P3 `vm_grow_stack` swallow-guard inequality (`vm.c:179-184`) against exact-touch
  region edges (region ending at `r->start`, region starting at `page`); confirm
  the host test covers them.
* P3 `vm_remove` hole-punch atomicity (`vm.c:106-154`, `syscall.c:7164-7167`): a
  `-ENOMEM` split leaves frames mapped and VMAs untouched; confirm the swap loop
  terminates and the munmap contract is intended.

Verification gate: `make verify-host` (`vm_hosttest` green), `make verify-style`, a
real boot exercising fork/exec/brk/mmap under busybox.

## Phase 3: the block and ATA driver

Scope: `sys/block/{block,ata}.{c,h}` and the `kio.c` `rep insw/outsw` it relies on.
Entirely new in `7266d42`; invisible to the default RAM boot, exercised only by the
disk mounts and the probes.

Concerns to chase:

* P2 LBA/count bounds arithmetic (`ata.c:183-184,231-232`, `block.c:18-19,41-42`).
  Prove that `lba > sector_count` followed by `count > sector_count - lba` rejects
  every out-of-range or wrapping pair in both `ata_read/write` and
  `block_partition_read/write`, including `lba==sector_count`, `count==0`, and
  large multi-chunk counts. The `>` (not `>=`) is load-bearing.
* P2 partition-view forwarding (`block.c:9-49,51-76`). Confirm
  `block_partition_init`'s clamp (`block.c:61-66`) guarantees
  `start_lba <= backing->sector_count`, that `start_lba + lba` cannot overflow or
  escape the disk, and that the `sector_count=0` fail-closed view is correct.
* P3 ATA handshake (`ata.c:156-167,196-198,244-246,103-150`): the select-then-
  program ordering with `ata_wait_not_busy` before `ata_program_lba`, and the
  DRQ-clear-after-write wait, with no command issued while the drive holds DRQ.
* P3 IDENTIFY trust and no-drive detection (`ata.c:272-338`): `ata_wait_data`'s ERR
  check covers an aborted IDENTIFY; the floating-bus heuristic and re-probe.
* P3 `ata_init` idempotence (`ata.c:272-277`, `kernel.c:1842,1874`). Confirm the
  `disk` debug command cannot re-probe and silently repoint a mounted partition's
  backing, or that re-probe is harmless.
* P3 `ATA_SPIN_LIMIT` watchdog (`ata.c:52,82-150`): an iteration count, not a time
  budget; flag that a spurious `-EIO` would propagate up the `pc_fault` path.
  Confirm a `-EIO` yields a clean fault failure, not a partially-filled frame.

Verification gate: `make verify-host` (`pagecache_hosttest`, which `#include`s the
backing path), a `rach2.fsdisk` boot, and the `blockprobe` / `persistprobe` /
`stressprobe` disk probes under `harness.py --drive`. Confirm the run scripts attach
the image as IDE/ATA on the primary channel.

## Phase 4: KRFFS disk modes, dirty bit, and fsck

Scope: the disk behavior in `kernel.c` (`init_krffs_disk`, `init_krffs_window`,
`krffs_disk_mark_clean`, the MBR probe, the three-way dispatch), the
`KRFFS_MODE_DIRTY` bit in `krffs.h`, and the host side
(`krffs_platform.{c,h}` region args, `fsck.krffs.c`, `krffs_node.c`). The kernel
allocator core is untouched and the default RAM path must stay byte-identical.

Concerns to chase:

* P1 dirty-bit lifecycle. `krffs_disk_mark_clean` (`kernel.c:1447-1453`) has one
  direct caller: the debug-shell `halt` (`kernel.c:2180`). A normal `/sbin/init`
  disk boot calls `init_krffs_disk` (`kernel.c:2532-2534`), sets `KRFFS_MODE_DIRTY`
  (`kernel.c:1575-1576`), and neither `sys_sync` nor `sys_fsync` clears it
  (`syscall.c:1129-1145`), so every normal disk boot leaves the FS dirty and fsck
  always warns. Decide whether the debug `halt` is genuinely the only intended
  clean-unmount path (and if not, what shutdown/reboot/sync should clear it). Either
  way the comments that claim a clean unmount clears it (`kernel.c:1441-1445`,
  `krffs.h:38-42`, `krffs/krffs_node.h:29-38`) must state the real contract.
* P2 host/kernel size agreement (`kernel.c:1523-1525,1558-1562`). The probe accepts
  `root->size <= count*512` but mounts the FULL `sector_count*512`. Confirm
  mkdisk always makes them equal, and check the `uint32_t` `part_bytes` multiply
  for overflow above the window cap.
* P3 root node invariants on the disk path (`krffs_node.c:134-189`): `parent_id`
  always zero (relied on for `ROOT_ID==0`), magic/type re-checked, the dirty bit
  invisible to every `KRFFS_IS_DIR` / masked-mode comparison in kernel and host.
* P2 `krffs_parse_region_args` overflow/truncation (`krffs/krffs_platform.c:34-103`,
  `krffs_platform.h:88`): every disk-image path depends on this parser sizing the
  region correctly. Check the size gate and the `(size_t)`/`(off_t)` casts on a
  32-bit host, and the `KRFFS_MAX_REGION_SIZE` (4095 MiB) vs the kernel window cap
  (256 MiB, hard-capped separately in `krffs/mkdisk.sh:40-66`) mismatch where the
  host can mint an image the kernel rejects only at mount.
* P2 host frontend region plumbing. Each modified KRFFS host frontend must wire
  `--offset`/`--size` into the shared mapped region and not consume or reject the
  flags in its own option parser: `krffs/defrag.krffs.c:183-206`,
  `krffs/edfs.krffs.c:265-287`, `krffs/fsviz.krffs.c:242-259`,
  `krffs/fsck.krffs.c:127-149`, `krffs/mkfs.krffs.c:123-146`,
  `krffs/stageimage.krffs.c:778-798`.
* P2 fsck dirty-clear persistence (`krffs/fsck.krffs.c:214-217,360-370`): the P1
  dirty-bit audit is inconclusive unless the `MAP_SHARED` clear actually reaches
  disk through the unmap; confirm no early-return path frees before the flush.
* P3 dirty write trusts the window page (`kernel.c:1450-1452,1570-1577`): no
  remount/eviction can repoint `PCWIN_BASE`'s first page between mount and clear.

Verification gate: `make verify-host`, a `rach2.fsdisk` boot, the `crash-test`
target (dirty set / fsck recover), and confirmation that the default RAM mount is
byte-identical (no dirty bit, `pc_sync` a no-op).

## Phase 5: the recent fixes (exec GP-reset, keyboard type-ahead, Doom crt0)

Scope: `sys_execve`'s GP-reset and its interaction with fork/sigreturn
(`syscall.c`), the keyboard IRQ gate (`kernel.c`), and Doom's `crt0.c`. These three
follow-ups are CI-invisible (no host test exercises them) and must be reasoned
through plus exercised by hand.

Concerns to chase:

* P2 exec eax contract (`syscall.c:3407-3419` + `:7214`). execve clears six GP regs
  but leaves `eax` to the dispatcher (`params->eax=ret==0`). Confirm the new image
  starts with `eax==0` as musl/SysV expects, that no failure path reaches the
  GP-reset block, and that no path returns 0 without rewriting the trapframe.
  Confirm `%edx==0` is the only GP register musl `_start` and the crt0s rely on.
* P2 GP-reset vs sigreturn vs fork (`syscall.c:3585-3588,5183-5205,7214`). The
  three must stay independent: sigreturn restores the full GP set including `eax`
  and returns it so the dispatcher does not clobber it; fork sets child `eax=0` and
  inherits the parent registers; execve's clear must not have leaked into a shared
  helper.
* P2 `sys_fsync` global flush (`syscall.c:1119-1133`). No per-fd-type guard: fsync
  on a pipe/socket/tty returns 0 instead of `-EINVAL`, and can return an unrelated
  `-EIO`. Reconcile the "must succeed" comment with dropbear's actual fsync targets
  and flag the altitude gap.
* P1 keyboard echo busy-wait in IRQ context. IRQ1 installs `keyboard_handler`
  (`kernel.c:2432`); its type-ahead branch (`kernel.c:391-415`) calls
  `tty_input_char` (`tty.c:146-150`), which echoes through `console_sink`'s
  `serial_putc` (`tty.c:312-317`), a UART busy-wait, in the keyboard IRQ. The
  serial drain `console_pump_irq` deliberately suppresses echo to avoid exactly
  this (`tty.c:338-346`); the keyboard path does not. Bound the worst-case in-IRQ
  wait and decide whether the asymmetry needs guarding. Resolve this jointly with
  the Phase 6B tty/poll-composition concern: do not change echo suppression or
  type-ahead in one path without the other.
* P2 keyboard gate vs the debug shell (`kernel.c:391,417,2218`). The
  `wq_active(...) || console_tty.sid != 0` gate must not strand the debug shell's
  own keystrokes (sid!=0 while `kernel_shell_active`). Verify the rescue/no-init
  boot keeps `sid==0` and that `shell_reset_input` closes any window.
* P2 `kill_foreground_process` before the CR3 switch (`kernel.c:280-288` called at
  `:353-356`, switch at `:361`). It touches `console_tty` and walks `proc_table`
  under an arbitrary user CR3. `clone_page_directory` shares the identity-mapped
  kernel tables into every process (`paging.c:688-694`), so the globals should be
  reachable; verify that shared-kernel-PDE invariant holds for this pre-switch
  Ctrl+C path (`kernel.c:353-366`, `tty.c:67-80`) rather than assuming a stale
  higher-half mapping.
* P2 Doom keyboard/fb collision (`kernel.c:391-416` vs `syscall.c:1292-1303`). With
  Doom owning the framebuffer and `console_tty.sid != 0`, every keypress is echoed
  through the line discipline during mode-13h graphics while Doom reads raw
  scancodes. Verify Doom renders cleanly and takes input; decide whether an
  `fb_owner` guard is needed. Exercised end to end in Phase 6C's Doom run.
* P2 crt0 stale comment (`ports/doom/crt0.c:1150`): the prose says the kernel does
  not reset `%ebp`, but `21a1d57` now does. The code is correct (reads from `%esp`);
  recast the rationale.
* P3 stale keyboard comment (`syscall.c:1311-1315`): still credits
  `stdin_waiting_proc` with telling the IRQ a process owns the console, a mechanism
  `a97ae6f` replaced.

Verification gate: `make verify-host`, `make verify-style`, a real boot, the SSH
capstone (execve + dropbear fsync exercised), and a Doom run with sound and
keyboard input. Manually exercise PS/2 type-ahead into busybox ash with no parked
reader, line editing (^U/^W/^D/backspace), and Ctrl+C to the foreground group.

## Phase 6: the broader kernel sweep

The subsystems the milestone did not edit but that rest on the seams it touched.
This is too large for one fan-out, so it runs as three: 6A fs/proc/syscall, 6B
tty/io/ipc/poll, 6C net/audio/periph/Doom. Each is its own fan-out and gate; one
auditor per file, all chasing ripple-in.

### Phase 6A: fs, proc, syscall

Concerns to chase:

* P1 Doom's fb/mouse open path (`syscall.c:892-936,675-694`). fb/mouse are resolved
  by bare-name only when cwd is exactly `/`. Trace Doom's exact open sequence and
  cwd and prove doom1.wad/fb/mouse are all opened from `/`.
* P2 synth pool lifecycle across exec/exit (`procfs.c:368-374`,
  `syscall.c:107-131,234+,3031-3050`): `synth_dir`, `proc_files[8]`, and fb
  ownership must all release through `fd_release_backing` on CLOEXEC-exec and on
  death, or a slot/fb-mode leaks.
* P2 /proc pid bounds end to end (`synthfs.c:64-82`, `procfs.c:89-104,313-316`):
  `comp_uint` accepts up to 2.1e9, `procfs_ino_pid` masks to 16 bits, `get_process`
  takes `uint32_t`; confirm `get_process` rejects out-of-range pids so a crafted
  `/proc/<huge>` cannot index past the proc table.
* P2 copy-to-user cannot target the window (`devfs.c:68`, `procfs.c:336`): confirm
  `user_buffer_ok`/`copy_to_user` reject `>= USER_ADDR_MAX` so the window is never
  a writable target through `/dev/zero` or a `/proc` read.
* P2/P3 proc seams: PCB layout assertions (`process.c:34-38`) and `switch.s`
  offsets (48/52/56/584) still match `struct process`; `build_user_stack` bounds
  (`uimage.c:27,29`, `aux[10]` vs `naux=9`, `UIMAGE_MAX_ARGS` vs the 15-arg cap);
  `elf_load_image` protect-after-copy ordering against forced `CR0.WP`
  (`sys/proc/elf.c:405-443`); `wq_wait_event` callers re-test fresh shared state
  (`wait_queue.c:88-148`).
* P3 device fstat/lseek consistency (`syscall.c:1180-1184,2104-2108`), `devfs_open`
  ofd defensiveness (`devfs.c:242` vs `:264`).

Verification gate: module-path `make test` (28/28), `make verify-host`, a real boot,
the SSH capstone (exec/pty path).

### Phase 6B: tty, io, ipc, poll

Concerns to chase:

* P2 poll/keyboard composition and the interrupts-off scan (`poll.c:107-201`,
  `tty.c:219-244,325`): a console poller is woken by both keystrokes and serial
  input; `console_pump` echoing through the UART busy-wait runs under `cli` inside
  `poll_scan`, bounded by `TTY_PUMP_BUDGET`; the park/dequeue cannot leave
  `current_proc` a stale waiter across `-EINTR`/timeout. Resolve the keyboard
  echo-in-IRQ asymmetry from Phase 5 here, in one pass, so echo suppression and
  type-ahead stay consistent across the keyboard and serial paths.
* P3 `fd_poll` socket invariant (`poll.c:75-97`), pipe-ring readiness and the
  closable-stdio wake path.

Verification gate: module-path `make test` (28/28, incl. `pollprobe`, `ptyprobe`,
`pipeprobe`, `jobprobe`), `make verify-host` (`poll_select`, `pipe_ring`, `n_tty`
host tests), a real boot, and the manual PS/2 type-ahead + Ctrl+C exercises.

### Phase 6C: network, audio, periph, Doom hardware

Concerns to chase:

* P2 socket_poll wakes (`socket.c:403-444` and the wake sites): every TCP state
  transition that reaches ESTABLISHED/CLOSED/FIN wakes `sock->waiters` so a poller
  is not lost between SYN_SENT and ESTABLISHED.
* P2 Doom keyboard/fb collision (the Phase 5 concern, exercised here): with Doom
  owning the framebuffer and `console_tty.sid != 0`, every keypress is echoed
  through the line discipline during mode-13h graphics while Doom reads raw
  scancodes (`kernel.c:391-416` vs `syscall.c:1292-1303`). Confirm Doom renders
  cleanly and takes input; decide whether an `fb_owner` guard is needed.
* P3 periph DMA-buffer placement and AUDIO_WRITE prefault (`sb16.c:34-36,91-105`,
  `syscall.c:5637-5645`), HDA BAR vs the window (`hda.c:1596-1611` vs
  `pagecache.c:427-471`): the DMA buffers and the HDA MMIO BAR must not collide
  with the page-cache window or the reserved frames.

Verification gate: module-path `make test` (28/28, incl. the network probes), the
SSH capstone (TCP/socket/poll), and a Doom run with sound, keyboard input, and a
clean exit to text mode.

## Phase 7: comments

Scope: a tree-wide read of the comments the milestone made load-bearing or stale,
plus the over-80 wraps the house style forbids. Findings from the earlier phases
that were comment-only roll up here if not already fixed in place.

Concerns to chase:

* The `CR0.WP` claim (`paging.c:213-220`) and the window-prefault overflow claim
  (`paging.c:329`) are correct only because `USER_ADDR_MAX==PCWIN_BASE`; the
  comments should be verified against that coincidence, and the `paging.c:417` and
  `usercopy.h:18-23` comments checked for the new adjacency.
* `pagecache.h:18-22` ("256 frames is a fixed 1 MiB reservation ... the inert
  module path keeps its memory"), `pagecache.c:201-202` ordering, and
  `pagecache.h:71-75` vs the `pc_set_capacity` body.
* The RAM-disk-centric prose in `kernel.c`: `ls` prints "Files on RAM disk:"
  (`:1909`), the help text (`:1802`), `exec_program`/`boot_init_process` headers
  ("the RAM disk holds no /sbin/init", `:1244-1259`) now that init can come from a
  real disk.
* `syscall.c:1311-1315` (keyboard delivery still credits `stdin_waiting_proc`),
  `syscall.c:1109-1118` (fsync "must succeed" vs the `-EIO`/any-fd body),
  `kbd.c:3-7` (producer is now `keyboard_handler`), `block.h` and `ata.c` contract
  comments, `crt0.c` `%ebp` note.
* Over-80 wraps flagged by the maps (`vga_gfx.c:156/437`, `sb16.c:300/311`,
  `vga_text.c:150`): note only if `verify-style` flags them.

Verification gate: `make verify-style` (8/8), and a re-read confirming each edited
comment still describes the code exactly. No behavior change.

## Phase 8: documentation

Scope: the Readmes the milestone left describing the pre-disk world. This is the
largest doc debt of the milestone.

Concerns to chase:

* P1 `rach2/sys/mm/Readme.md`: the fault handler is described as three cases with no
  window branch (`:27-33`), and `pagecache.c` plus the whole `sys/block` layer are
  absent. The single most misleading doc post-disk.
* P1 `rach2/Readme.md`: the Limitations table denies persistent disk (`:1339`); the
  Memory Layout omits the `0xC0000000` window (`:104-113`); the Source Structure
  tree omits `sys/block` and most of `mm/` (`:152-162`); the debug-shell table
  misses the `disk` command and `halt`'s dirty-clear contract (`:1300-1328`).
* P2 `krffs/Readme.md` and root `Readme.md` Components row: still "RAM disk
  builder", no `--offset/--size`, no `mkdisk.sh`/`mkdisk_grub.sh`/`grub_embed.py`,
  no `make os-real-disk`. Rewrite per the same-commit Readme rule.
* P2 `tests/Readme.md`: missing `blockprobe`/`persistprobe`/`stressprobe`,
  `pagecache_hosttest`, and `harness.py --drive`.
* P2 window-cap reconciliation across `rach2/sys/fs/Readme.md:28`,
  `dev-log/08-disk/Readme.md:14,26`, and the code (`pc_probe_window_size` =
  min(partition, lowest MMIO BAR) capped at `PC_WINDOW_CAP=256 MiB`). Make all three
  state the same true thing, including the BAR clamp.
* P2 `make help`: lists `os-real-disk`/`os-real-grub-disk` but not
  `test-disk`/`persist-test`/`crash-test`/`stress-test`, so the gate targets are
  undiscoverable.
* P3 `ports/doom/Readme.md:22` ("C runtime: _start ..." now raw asm), the
  `rach2/Readme.md` execve section (`:309-330`) vs the GP-reset, and a check that
  no surviving doc claims KRFFS is RAM-only or that the debug-shell models pre-line-
  discipline stdin. Confirm `CLAUDE.md`/`AGENTS.md` need no change.
* P3 dev-log ownership. Classify the changed dev-log files as historical (read-only)
  or current (to verify): `dev-log/08-disk/plan.md:3-4` marks the retired plan
  complete and `dev-log/Readme.md:12` states the disk-milestone contract, both of
  which describe the current system and must read true; the older milestone plans
  stay as written history.

Verification gate: a read-through confirming every touched Readme matches the code;
`make verify-style` (the docs are linted for ASCII-only, `*` bullets, no em-dash,
no wrapped prose). A change that alters a contract rewrites the owning Readme in the
same commit, it does not append.

## Phase 9: support files

Scope: the build system, host tools, run/test scripts, manifests, and the disk
probes. The non-kernel scaffolding the gate itself depends on.

Concerns to chase:

* P2 disk-boot test timing (`Makefile:190-231`, `harness.py:46,127`). The disk-boot
  targets pass no `--steptmo`, so the default 15 s margin against a slower
  polled-PIO disk boot is untested. Measure actual disk-boot-to-prompt time, and
  `stressprobe` wall-time vs the fixed `wait=130` under `--timeout 150`.
* P2 the `--ok` token contract (`harness.py:186-191`): passes on token-seen with no
  clean-exit check and no assertion the FAIL token is absent. Confirm the OK tokens
  are unique and only printed on genuine success, and that a corrupt-then-OK or
  hang-after-OK cannot false-PASS. For `test-disk`, `persist-test`, and
  `stress-test`, require that no `FAIL` token appears, preserve the serial log, and
  use the `probe=`/isa-debug-exit status where available.
* P2 the harness-exit ABI (`rach2/sys/kio.c:135-148`, `tools/harness.py:196-212`):
  `harness_exit` writes status to port `0xF4` and the harness decodes serial status
  plus isa-debug-exit. The new disk-probe reliability rests on this contract;
  confirm the status encoding and the QEMU `isa-debug-exit` wiring still agree.
* P2 `os-real-grub-disk` end to end (`mkdisk_grub.sh:81-107`, `grub_embed.py:9-14`):
  Docker-only, apt-unpinned, hand-embeds GRUB at fixed boot.img offsets, in no
  gate. Confirm it still boots BIOS->MBR->GRUB->kernel->KRFFS and that the offset
  assumptions hold for the GRUB version apt resolves.
* P3 `crash_test.sh` magic numbers (`:19-20` vs `Makefile:217-219` +
  `mkdisk.sh:34-37`): `OFFSET=1048576`/`SIZE=50331648` must track the mkdisk
  geometry; the coupling is implicit. Plus the unconditioned `sleep 10`/`sleep 14`
  timers.
* P3 orphan and dead-path checks: `os-real-disk` (`Makefile:160-166`,
  `run.sh:28-29,63-69`) booted by no script; the `exec=` STEP form
  (`harness.py:22,83,151-159`) possibly dead; `grub_embed.py` argc guard
  (`:25,53,63`); the MBR CHS sentinels / type 0x7F accepted on both the hand-written
  (`mkdisk.sh:82-100`) and `parted` (`mkdisk_grub.sh:89-94`) paths.
* P2/P3 confirm the default RAM module boot is genuinely unchanged: `runtests.py`
  PROBES still 28, `test.manifest` untouched, the new probes disk-only and absent
  from the module test by design, not omission. Verify the two new Python tools have
  no linter gate and that the hand-copied `WINDOW_MB=256` constant in `mkdisk.sh`
  still matches `PC_WINDOW_CAP`.

Verification gate: `make verify-style` (shellcheck over `tools/*.sh`, `krffs/*.sh`,
`rach2/*.sh`), `make verify-host`, module-path `make test` (28/28), and a clean run
of every disk target (`test-disk`, `persist-test`, `crash-test`, `stress-test`)
plus the `os-real-grub-disk` boot.

## Phase 10: whole-system manual pass

Scope: nothing new is read; this is the end-to-end exercise of every contract the
earlier phases reasoned about, on real hardware emulation.

Method: boot each mode and run the gate by hand. Default RAM boot byte-identical to
the recorded baseline. `rach2.fswindow` and `rach2.fsdisk` boots: mount, ls, touch,
read back. The SSH capstone: `ssh -i krffs/rach2_demo_ed25519 -p 2222
root@localhost` over `os-real`, pubkey auth, interactive busybox ash, `rc==0`,
dropbear started from the console. Doom: run from `/`, with sound, keyboard input,
clean exit returns to text mode. The `os-real-grub-disk` real-disk boot through
GRUB. Keyboard type-ahead and Ctrl+C from the PS/2 path.

Verification gate: every item above passes; any regression found here reopens the
owning phase.

## Verification

The global gate, run at the end of every phase that touches behavior and once more
as a whole after Phase 10:

* `make verify-style` (8/8).
* `make verify-host`.
* module-path `make test` (28/28).
* a real boot of the default RAM path, byte-identical to the baseline.
* the SSH capstone over `os-real`.
* a Doom run from `/` with sound, keyboard input, framebuffer ownership, and a
  clean exit back to text mode.

Plus, for the disk-touching phases, the disk boots and probes (`rach2.fswindow`,
`rach2.fsdisk`, `blockprobe`, `persistprobe`, `stressprobe`, `crash-test`,
`os-real-grub-disk`). The vendored trees (`musl`, `busybox`, `dropbear`, and
`krffs/vendor`, which holds `termbox2.h`) stay untouched throughout; a finding that
would edit them is out of scope. Probe success
tokens, the 28-probe module set, and the byte-identical default boot are test
contracts and are not changed without flagging the change first.

## What stays deferred

* The milestone-6 backlog carried forward: the 645 over-80 kernel comment wraps
  (touch only the ones an edited block already crosses), the tree-wide Title-Case
  diagnostics normalization, and any cleanup that is taste rather than a confirmed
  defect.
* Reproducibility hardening of the Docker GRUB path (pinning `grub-pc-bin`,
  `parted`, `mtools`) is recorded as debt, not fixed here unless a confirmed break
  forces it; the path is manual and off the automated gate.
* A linter/type gate for the two Python tools (`grub_embed.py`, `harness.py`) is
  noted but not added in this audit.
* Any new host-test coverage beyond the page-cache failure branches called out in
  Phase 1 is deferred unless a confirmed defect needs a regression test.
* P3 cosmetic findings that survive verification but change no behavior may be
  batched into a single cleanup commit rather than blocking a phase gate.

## Baseline

The green state to diff against is `HEAD` at `21a1d57` ("Clear the general-purpose
registers across execve."), the tip of the four follow-ups. Before Phase 1, record
the baseline: `make verify-style` 8/8, `make verify-host` clean, module-path
`make test` 28/28, a default RAM boot to the prompt, the SSH capstone green, and a
Doom run with sound. Every later phase diffs against this recorded state, as in
milestone 6, so a regression is attributable to the phase that introduced it and not
to the milestone under audit.
