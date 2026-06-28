# Path B: KRFFS on a disk-backed page cache

Status: draft.

## The goal

Lift KRFFS off the 32 MB physical RAM-disk module and onto a real block device
of arbitrary size. KRFFS keeps pointer-walking one contiguous virtual window; a
touch of an absent window page faults; a new kernel-space fault handler reads the
backing sectors into a frame held by a page cache and maps it; dirty pages write
back; pages evict under pressure. Only the working set is resident, so the on-disk
filesystem can far exceed RAM. Then boot from that disk through GRUB.

KRFFS itself is almost untouched: it already confines all address arithmetic to
`[fs->node, fs->node + fs->size)` (`krffs_wrap_node_position`, `krffs_get_next_node`,
the `node + 1` data alias), with no physical-address, DMA, or alignment assumption.
The work is the machinery underneath the pointer: a block driver, a kernel page
cache, a fault-handler branch, a disk boot, and the host tools to build and check a
partitioned image.

## The decision that shapes everything: the fault polls, it does not sleep

The page-fault handler runs with interrupts disabled. Vector 14's stub does `cli`
(`isr_handler.s:17`) and the ISR router never re-enables IF for an exception
(`isr_handler.s:93`, "IRQ stubs run with IF=0 throughout"); only the syscall path
sets `in_syscall` and then `sti`. So `_page_fault_handler` (`paging.c:382`) runs at
IF=0 start to finish. The only sleep primitive, `wq_wait_event`, parks by calling
`schedule()` and then `sti; hlt; cli` (`wait_queue.c:134`). Sleeping inside the fault
would re-enable interrupts mid-exception, let the timer switch stacks against a
half-restartable instruction, and reopen the handler to reentrancy.

Therefore the block read on the fault path is **synchronous polled PIO**: program
the ATA registers, busy-wait BSY/DRQ on the status port with a bounded spin, copy
the sectors with `insw`, return. No ATA IRQ, no wait queue, no `sti`. This is not a
performance tradeoff; it is forced by the context. It is also the single biggest
simplification in the plan, because it closes three holes at once that every prior
design left open:

* **Concurrency.** A fault that never yields means a syscall body walking or
  mutating KRFFS never voluntarily context-switches mid-operation, and the timer
  cannot preempt it (the timer's reschedule is gated on `in_syscall`,
  `kernel.c:131`). So KRFFS's multi-store metadata mutations (split, resize, remove,
  rename) stay atomic with respect to other processes exactly as they are today in
  RAM. No FS-wide lock is needed. A sleeping fault would have broken this; a polling
  fault preserves it.
* **Boot context.** With no sleep, mounting the FS at boot (before the scheduler is
  enabled) needs no special idle-spin reasoning. The fault completes inline.
* **Reentrancy.** A nested fault during the read cannot happen, because the PIO
  driver reads into an identity-mapped physical frame by its low address, never
  through the window.

The cost is real and is owned, not hidden: a cache miss stalls the whole machine
for the duration of the sector transfer (256 `insw` per sector), missing the timer
and any in-flight NIC/HDA interrupt. For a teaching OS with a small working set this
is acceptable. The bounded spin returns `-EIO` on a wedged drive rather than hanging;
the fault handler then panics fail-closed (a disk that does not answer is not
recoverable), but the bound prevents a silent forever-spin.

## The other load-bearing invariant: pre-map the window in `_kernel_dir`

`clone_page_directory` copies all 1024 PDE slots **by value** at fork time
(`paging.c:555-557`): the child gets a snapshot of the kernel directory's page-table
pointers. A page table added to `_kernel_dir` *after* a process has forked is
invisible in that child's CR3. KRFFS is walked from syscall bodies running under the
calling process's own CR3, so a window fault from a forked process resolves against
that process's directory, which must already contain the window's page tables.

So every page table spanning the window must be allocated in `_kernel_dir` at mount
time, with its PTEs left not-present, **before the first fork**. The fault handler
and the reclaimer then only ever edit leaf PTEs in those shared tables; every
process sees the edits through the shared physical tables. This mirrors how
`init_paging` eagerly builds the identity map and how `map_mmio` builds MMIO tables.
Boot ordering already permits it: `init_krffs` is at `kernel.c:2136`, the first fork
(`boot_init_process`) is at `kernel.c:2204`.

Two corollaries the prior designs got wrong and this plan fixes:

* The fault-handler branch must key purely on the **fault address** being in the
  window, not on `have_user_proc`. A kernel-context fault (proc0 at boot, with
  `cr3 == 0`) has `have_user_proc` false (`paging.c:391`), and a CPL=0 syscall-body
  fault has `user_mode` false, so both fall straight to the panic at `paging.c:433`
  unless the new branch sits before it and is address-keyed.
* The window page tables must be installed **supervisor-only**. Every existing
  table-creating helper forces the U bit (`ensure_user_table` writes the PDE with
  `| 0x7`). The window PDEs must be hand-installed like the identity map (`| 0x3`),
  and the fault-time PTE writer must not route through `ensure_user_table`. Do not
  set the global (G) bit: CR4.PGE is off, so G buys nothing today and would, if PGE
  were ever enabled, let an evicted-then-reused frame keep a stale global translation
  across a context switch. A new `map_kernel_frame(window_va, frame, P|RW)` and a
  matching unmap are the seam.

## Milestone scope decisions

These are settled here so the phases do not relitigate them.

* **Polled PIO ATA, not AHCI, not IRQ-driven.** Legacy ATA on the primary channel
  (ports 0x1F0-0x1F7, control 0x3F6), LBA28, no PCI, no DMA pool. It is the textbook
  driver, it is the only thing the IF=0 fault path can use, and it fully exercises
  the block/cache design. AHCI is explicitly out of scope: it needs DMA (hence the
  IRQ for completion, which the fault path cannot service) and PCI MMIO choreography.
  The DMA pool module that earlier drafts proposed is dropped entirely as
  unexercised dead code. LBA28's 128 GiB ceiling clears the 4 GiB KRFFS cap.
* **Write-back, not write-through, with a real fsync.** Write-through would make
  every byte-granular KRFFS metadata store a synchronous sector write and still not
  make a multi-store atomic. Write-back marks the page dirty on a write fault and
  flushes on eviction and on fsync. `fsync`/`fdatasync` (no-ops today, `syscall.c:1117`)
  become a real flush of the window's dirty pages; dropbear's
  open/write/fsync/rename host-key idiom then becomes honestly durable. One
  exception: the dirty-mount superblock flag is written **write-through**, so a crash
  cannot leave it falsely clean (see the crash phase).
* **Dirty-flag + offline host fsck for crash recovery, no kernel journal.** On mount
  the kernel sets a dirty sentinel; a clean sync/unmount clears it. After a crash the
  operator runs the host `fsck.krffs`. KRFFS rename and multi-store metadata are not
  crash-atomic and the plan says so; fsck detects and reports structural damage and
  the dirty flag, and does not attempt to repair an interrupted node split. fsck does
  not validate the v2 parent_id hierarchy (it walks the physical ring only); that gap
  is stated, not closed.
* **The 4 GiB partition cap stays.** The OFD offset is `uint32_t` (`fd.h:83`),
  `_llseek` rejects `>0xFFFFFFFF` (`syscall.c:2666`), and `krffs_file_system.size` is
  `uint32_t` (`krffs.h:86`). Path B lifts the 32 MB *physical* cap, not the 4 GiB
  *logical* cap. In practice the window is tighter still: the window must sit at or
  above `USER_ADDR_MAX` (0xC0000000) and below the PCI MMIO hole (~0xFE000000), so
  the real ceiling is the window span, well under 1 GiB. `init_krffs` must reject a
  partition whose byte size exceeds the window span (and never let `nr_sectors * 512`
  overflow a 32-bit intermediate). 64-bit offset widening is a later milestone.
* **`block_device.sector_count` is `uint64_t`.** The device can describe a >4 GiB
  disk even while KRFFS cannot use one; the block layer does not inherit the 32-bit cap.
* **No VFS rewrite.** No `struct vnode`, no `mount_table[]`, no per-FS ops vtable.
  There is one real filesystem (KRFFS) plus the synthfs classifier, which already
  routes /dev, /proc, and /dev/pts self-containedly. Disk-KRFFS is the same `krffs.c`
  over the same window. The only edits are the mount site, a disk mount entry point, a
  real fsync, and the size gate. The ~30 KRFFS/synthfs call sites in `syscall.c` are
  untouched. Do not make /dev and /proc table-driven mounts: `getdents64` deliberately
  does not merge them into KRFFS listings, and converting them buys nothing.
* **The multiboot-module path stays the default and the dev/test fallback.** Disk
  boot is selected by a new cmdline flag. `make test`, `run.sh`, `runtests.py`, and
  the module mount stay byte-identical, so the existing gate never moves. The first
  cut proves the cache under the existing `-kernel` + `-drive` boot; GRUB-from-disk is
  the last phase.
* **The window starts at 0xC0000000 (== `USER_ADDR_MAX`).** Proven outside the user
  demand path: `in_user_range` is `[0x02000000, 0xC0000000)` (`paging.c:392`), so a
  0xC0000000+ fault never reaches `vm_resolve` or `cow_fault`. The span is clamped at
  runtime against the lowest probed device BAR, not a hardcoded constant, so a device
  whose MMIO lands low cannot silently collide with the window.

## The phases, and why they are ordered this way

The forced dependencies are: a block read/write primitive must exist before
anything can fill a cache frame; the page cache plus the fault-handler branch must
exist before KRFFS can be pointed at the window; the window must be proven over a
RAM stub before a real disk is introduced, to separate FS-relocation bugs from
driver bugs; the disk boot must come after the cache works, because it only changes
where the bytes come from; and the size gate must precede any image large enough to
need it. The host-tool offset work depends on nothing in the kernel and lands first,
de-risking the on-disk format early. Each phase is independently buildable, leaves
`make verify-style` (8/8) and `make verify-host` green, and the module path stays the
default so `make test` and a real boot stay green throughout.

### Phase 0: partition-aware host tools and the disk-image pipeline

Goal: build and inspect a KRFFS image that lives at a byte offset inside a larger
disk image, entirely on the host, before any kernel code changes. This pins the
on-disk format and the dirty-flag home up front.

Deliverables:
* Thread an `--offset BYTES --size BYTES` pair through the six host tools' `main`
  functions (`mkfs.krffs`, `fsck.krffs`, `stageimage.krffs`, `defrag.krffs`,
  `fsviz.krffs`, `edfs.krffs`). Each forces `krffs_map_file(fd, 0, st_size)` today;
  `krffs_map_file` already forwards an `off_t offset` to `mmap` (`krffs_platform.c:53`),
  so only the callers and the `st_size < sizeof(node)*2` guards change. A shared
  `krffs_parse_region_args` validates the pair: the offset must be 1 MiB-aligned and
  within the file, and because KRFFS's size is extrinsic (the node ring tiles exactly
  `[0, size)`), a nonzero offset names a partition that does not reach the end of the
  disk and so requires an explicit `--size` rather than defaulting to the rest of the
  file.
* The dirty-mount flag's on-disk home, decided and implemented in the same change.
  NOT the root node's `type` byte: every reader compares the whole byte for equality
  (`root->type != KRFFS_ROOT_NODE` at `krffs.c:1301`, and the same in the host tools),
  so a high bit there would fail the mount (Codex review finding). The root node has
  no spare field, so use a root-only field that no reader equality-checks for
  structure: a reserved high bit of the root's `mode` (the kernel masks mode to
  `0777`/file-type bits, never compares the full `uint16_t`) is the chosen home. It
  is not a format bump, so the existing mkfs/fsck/stageimage/fuse tools and the kernel
  reader keep reading the same v2 layout; `fsck.krffs` masks it out of the mode it
  prints and reports it as "not cleanly unmounted". Phase 0 confirms by grep that no
  reader equality-checks the root's `mode`.
* `krffs/mkdisk.sh`, paralleling `mkimage.sh`: `dd` a flat image, write a minimal MBR
  with one primary KRFFS partition starting at **LBA 2048** (1 MiB, page- and
  64-KiB-aligned, so the host `mmap` offset is always valid on Linux and Windows),
  then `mkfs.krffs --offset $((2048*512)) --size ...` and
  `stageimage.krffs --offset $((2048*512)) ...`. The KRFFS bytes written are
  byte-identical to today's module image; the host KRFFS tooling logic is otherwise
  unchanged.
* A `make os-real-disk` target (mirroring `os-real-iso`) that builds a 64 MB disk
  with a 48 MB KRFFS partition, large enough to later prove an on-disk FS bigger than
  the 32 MB resident cap.

Hard parts and mitigations:
* `mmap` EINVALs on a non-page-aligned offset. Standardize on LBA 2048 in
  `mkdisk.sh` and reject a misaligned `--offset` in the tools with
  `tool: offset not 1 MiB-aligned` rather than letting `mmap` fail opaquely. State the
  guard as "1 MiB", not "page-aligned", because Windows `MapViewOfFile` needs 64 KiB
  granularity.
* A 16-bit magic (0x4B52) plus `type == 0` is a weak partition signature. The kernel
  probe (Phase 6) tightens it; the host tools trust the explicit `--offset`.

Verification: on a 5 MiB `d.img`, `mkfs.krffs --offset 1048576 --size 4194304 d.img`
then `fsck.krffs --offset 1048576 --size 4194304 d.img` reports success, and
`fsck.krffs --offset 0 d.img` on the same file FAILS (no magic at byte 0), proving
the offset is honored. `mkfs.krffs --offset 512 d.img` exits non-zero (misaligned),
as does `mkfs.krffs --offset 1048576 d.img` (a nonzero offset with no `--size`). `make verify-style`
and `make verify-host` stay green. This phase touches no kernel code, so `make test`
is untouched by construction.

### Phase 1: the block layer and the polled PIO ATA driver

Goal: a `block_device` vtable with a polled ATA backend that reads and writes
page-sized, sector-aligned chunks, with no IRQ and no DMA.

Deliverables:
* `sys/block/block.h`: `block_device_t { uint32_t sector_size; uint64_t sector_count;
  int (*read)(block_device_t*, uint64_t lba, uint32_t count, void *buf);
  int (*write)(block_device_t*, uint64_t lba, uint32_t count, const void *buf);
  void *drvdata; }`, returning 0 or `-EIO`, plus `extern block_device_t *block0`. A
  thin partition view whose `read`/`write` add `start_lba` and forward to the
  whole-disk device is the offset+length slice.
* `sys/block/ata.c`: `ata_init` does IDENTIFY (capacity from words 60-61, or 100-103
  if word 83 bit 10 is set), `ata_read`/`ata_write` program LBA28 + READ/WRITE SECTORS
  (0x20/0x30), poll BSY/DRQ on the alt-status port (0x3F6), copy 256 words per sector
  with `insw`/`outsw`, and a WRITE issues FLUSH CACHE (0xE7). All polled, no `sti`.
* `insw`/`outsw` added beside `inw`/`outw` in `kio` (a two-instruction
  `rep insw`/`rep outsw` wrapper; no string-port primitive exists today).

Hard parts and mitigations:
* The 400 ns post-command settle: read alt-status four times before trusting BSY.
  This is the one in-body hardware-fact comment the standard allows.
* BSY/DRQ handshake is per-sector, not one-time: poll BSY clear then DRQ set before
  each sector's transfer, with an ERR/DF check and a bounded spin returning `-EIO`.
* The bounded spin is the only watchdog (IF=0 means no PIT to time by); a wedged or
  absent drive returns `-EIO`, never hangs.
* Even unshared, ATA holds IRQ 14 asserted until status is read. Since the driver is
  poll-only, no IRQ handler is registered, so this is moot for the cache; if a
  diagnostic ever wires IRQ 14, its handler must read status to deassert INTRQ.
* QEMU IDE topology: `-cdrom` is ATAPI on the secondary channel (IRQ 15), a different
  command set. The disk must be `-drive file=disk.img,format=raw,if=ide,index=0,media=disk`
  to land a plain READ-SECTORS disk on the primary channel at 0x1F0.

Dependencies: none in-kernel. Builds on `inb`/`outb`/`inw`/`outw` (`kio`), nothing
else.

Verification: add the disk to `run.sh`'s QEMU line and a `disk` command to the debug
shell that calls `ata_read(0, 1, buf)` and hexdumps 512 bytes; the 0x55AA signature at
offset 510 and the partition entry at 0x1BE must match `fdisk -l` of the image. Then
`ata_write(100, ...)` + `ata_read(100, ...)` round-trips, proving WRITE + FLUSH.
`block0->sector_count` must equal image size / 512, proving the IDENTIFY parse. Run
this **before** `init_sched` with `current_proc == proc0` to prove the polled path
needs no scheduler. `make verify-style`, `make verify-host`, and a module-path
`make test` stay green (the driver is dead code until Phase 3 calls it).

### Phase 2: the kernel page cache and the disk-backed window (the hard, novel phase)

Goal: a fixed-size page cache that fills, dirties, writes back, and evicts frames
behind a kernel-virtual window, serviced by a new branch in the fault handler. This
is the riskiest phase; it is decomposed into sub-steps, each independently checkable
against the Phase 1 driver or a RAM stub.

Deliverables: `sys/mm/pagecache.c` + `.h`, peer to `frame.c`/`paging.c`.

Sub-steps, in order:

* **2a. Window reservation and supervisor-only pre-mapping.** `init_pagecache`, called
  from `k_main` after `init_paging` and before `init_krffs`, fixes `PCWIN_BASE = 0xC0000000`
  (asserted `== USER_ADDR_MAX`), clamps `pc_window_size` against the lowest probed
  device BAR, and pre-allocates every level-2 page table spanning the window in
  `_kernel_dir` with not-present, supervisor-only PTEs. `map_kernel_frame` /
  `unmap_kernel_frame` write a single leaf PTE (`P|RW`, no U, no G) into those shared
  tables and `invlpg`. Check: an `assert(PCWIN_BASE == USER_ADDR_MAX)` and an assert
  that the window's PDEs exist in `_kernel_dir` before any fork.
* **2b. Frame reservation against OOM.** Pre-reserve `PC_SLOTS` frames for the cache
  at init out of the `_frames[]` pool (marked used, never `U`-bit, so
  `alloc_user_page` and `free_user_pages` never touch them), so a cold-cache fault
  always has a frame and the handler never panics on transient memory pressure. Size
  `PC_SLOTS` small and fixed (e.g. 1024 = 4 MiB) so user pages, page tables, and the
  cache coexist under the 32 MB pool. This is the fix for the OOM-to-panic hole: the
  resident cache is a fixed reservation, not a borrow that can be starved.
* **2c. The cache table and lookup.** `pc_entry { uint32_t disk_off; uintptr_t phys;
  uint8_t valid, dirty, accessed, pinned; }`, a fixed `pc_slots[PC_SLOTS]`, and a
  clock hand. One device, so the key is `disk_off`; an open-addressed hash on
  `disk_off >> 12` keeps lookup O(1). Check: the host seam test below.
* **2d. The fault branch.** In `_page_fault_handler`, before the panic at
  `paging.c:433` and address-keyed (not `have_user_proc`-keyed):
  `if (fault_address >= PCWIN_BASE && fault_address < PCWIN_BASE + pc_window_size) {
  if (pc_fault(fault_address, write)) return; k_panic("pagecache: disk fault"); }`.
  `pc_fault` computes the page-aligned disk offset, looks it up, on a miss reserves a
  free slot (pinned across the read), polls `block0->read` of the 8 sectors into the
  slot's frame, maps it supervisor-only at the window VA, sets `accessed` (and `dirty`
  on a write fault), unpins, and returns 1. Bound the branch by the actual mounted FS
  size, not the full window span, so a wild kernel pointer into the window still
  panics loudly. Assert `current_proc != NULL`.
* **2e. Eviction.** `pc_get_free_or_evict` first takes a free slot; under pressure it
  runs the clock (clear `accessed` and skip, else victim), never picks a `pinned`
  slot, writes back a dirty victim with `block0->write` (polled, still IF=0),
  `unmap_kernel_frame`s it, and reuses it. The `pinned` flag guards both the in-flight
  fill frame and any frame a straddling access just mapped. Writeback precedes PTE
  clear and frame reuse, so no dirty page is dropped.
* **2f. Real fsync.** `pc_sync` walks `pc_slots` and writes back every dirty entry,
  clearing dirty after each successful write. `sys_fsync` (`syscall.c:1117`) calls it;
  add a real `sys_sync` (none exists today) that calls it unconditionally. The
  superblock dirty-flag write is the one write-through store, so it reaches the disk
  before any later dirty page.

Hard parts and mitigations (beyond the structural ones already settled above):
* **A straddling access faults two adjacent pages.** KRFFS does byte-granular `memcpy`
  across node boundaries; the CPU faults each page in turn. `pc_fault` is idempotent
  and the `pinned`/`accessed` flags stop a re-fault from evicting the partner just
  mapped. No multi-page atomic fault is needed.
* **The frame is double-mapped (identity alias + window VA).** Every cache frame is
  also live at its low identity address, kernel-writable. The "supervisor-only window"
  gives no isolation from kernel code, only from ring 3. The invariant that makes
  dirty tracking correct: KRFFS writes go through the window base, so the window PTE
  is the one the CPU dirties; the design relies on `pc_dirty` set by the write fault,
  not on the PTE dirty bit, so the identity alias is irrelevant to writeback. State
  this invariant; do not write FS bytes through the identity alias.
* **A grow-relocation `memcpy` can demand more resident pages than fit.** `krffs_resize_reserved_node`
  copies a whole file in one synchronous loop; a file approaching the resident budget
  cannot be held entirely. Milestone scope caps a single file well under the
  `PC_SLOTS` budget; the size gate (Phase 5) and the stress test (Phase 7) bound and
  prove this. A free-list index for KRFFS to avoid the full-ring allocator scan is
  noted as a known cost and deferred.
* **The window source faults inside `copy_to_user`/`copy_from_user`** (Codex review).
  `sys_read`/`sys_write` hand KRFFS's node pointer (a window address) straight to the
  user-copy, which then reads/writes the window and faults its pages in. The existing
  read/write prefault covers the USER buffer, not the window side; that is fine,
  because the user-copy runs in syscall context where the window fault is serviceable
  by the polled fill. The only invariant to state: the copy must not run with a held
  state that the fault path also needs (it does not, since the fault touches only the
  page cache). No window prefault is added; the fault path is the single entry.

Dependencies: Phase 1 (the driver `pc_fault` calls). Can be developed first against a
RAM stub that reads `ramdisk->start + off`, decoupling it from the real disk.

Verification, smallest first:
* **Host seam test** `pagecache_hosttest.c` under `make verify-host`: the pure
  bookkeeping (slot lookup, clock victim, dirty set/clear, sector<->page math) under
  ASan/UBSan with a fake backing callback. Assert a fault fills the right sectors, a
  write dirties, an evict writes back iff dirty, a re-fault re-reads, the victim is the
  least-recently-used. No hardware.
* **Window-fault probe (RAM stub):** point the window at a stub returning a known
  pattern; touch `PCWIN_BASE`, `+0x1000`, `+0xFFF` (straddle); assert the bytes match
  and a second touch is a hit, not a re-read.
* **Eviction probe:** set `PC_SLOTS` tiny, write distinct patterns through more pages
  than slots, read all back; assert every page round-trips. Print
  `VMPROBE PAGECACHE OK n=<reads> evict=<e> wb=<w>` (the existing VMPROBE token
  convention) from a self-test so a boot-log assertion catches regressions.

### Phase 3: KRFFS on the window (the payoff, over the RAM stub)

Goal: mount KRFFS on the demand-paged window instead of the RAM module, with the
backing still the RAM stub, so the whole stack is exercised before a real disk
exists. If KRFFS is genuinely just pointer-walking the window, nothing above the FS
changes.

Deliverables:
* `init_krffs_window(uint32_t disk_size)` (or extend `init_krffs`) that stores
  `_krffs_fs_instance.node = (krffs_node_t *)PCWIN_BASE` and `.size = disk_size`
  (rejecting `disk_size > pc_window_size`), with the backing read pointed at the stub.
  `fs->size` must equal the size the fault branch bounds-checks against, so
  `krffs_wrap_node_position` folds at the same boundary the handler serves. `krffs.c`
  is otherwise untouched.
* The mount site at `kernel.c:2136` gains the window branch, selected (for now) by a
  build/debug switch so the module path stays the default.

Hard parts and mitigations:
* `init_krffs`'s first act dereferences `root->magic`/`root->type`; under the window
  that is the first fault, served before `krffs_count_files` walks. Fine, but note
  that the mount-time validation read is itself a demand fault.
* `krffs_count_files` (called right after mount, `kernel.c:2138`) walks the whole ring,
  faulting in every node-header page. On a large FS this is a full metadata read at
  boot. Accept it for the milestone; the stress phase bounds the cost.

Dependencies: Phases 1 and 2.

Verification: the existing ten self-tests, a real module-style boot, and the SSH
capstone (`ssh -i krffs/rach2_demo_ed25519 -p 2222 root@localhost` to busybox ash) all
stay green with KRFFS on the stub-backed window, because the FS code is unchanged.
This is the regression gate that proves the relocation is transparent. Run the
self-test write and the /sbin/init ELF load (which fault the binary in through
`exec_krffs_read`) to exercise the fault path under proc0.

### Phase 4: real disk backing and the in-guest cache probes

Goal: swap the RAM stub for the Phase 1 driver so the window is backed by real
sectors, and prove fill/dirty/writeback/evict end to end in the guest.

Deliverables:
* `pc_fault`/`pc_sync`/`pc_get_free_or_evict` call `block0->read`/`block0->write`
  against the mounted KRFFS partition instead of the stub.
* `harness.py` gains optional `--drive`/`--device`/`--machine` args (defaulting empty,
  so module runs are byte-identical) and the ability to omit `-initrd` for a
  disk-only boot. New `*probe` drivers: `blockprobe` (write a pattern through the
  window, read it back), `cacheprobe` (force at least one eviction, modify a page,
  evict it, re-read, assert the change survived).

Hard parts and mitigations:
* The probes must verify **data integrity** (checksums), not merely absence of a
  crash, since a dropped-dirty or freed-while-mapped bug shows as silent corruption.
* Persistence and crash tests must run against a **scratch copy** of the image
  (`cp` before boot), because QEMU mutates a raw drive in place and a `make` re-stage
  would wipe the evidence.

Dependencies: Phases 1-3.

Verification: `blockprobe` and `cacheprobe` exit 0 under `harness.py` with the disk
attached. The module-path `make test` and `make verify-host`/`verify-style` stay
green (the new args default empty, the new probes are separate invocations).

### Phase 5: the size gate and real fsync durability

Goal: enforce the 4 GiB / window-span ceiling honestly and prove fsync persists.

Deliverables:
* `init_krffs_window` rejects a partition whose byte size exceeds the window span,
  with `krffs: partition exceeds window` to stderr, and never overflows
  `nr_sectors * 512` in a 32-bit intermediate. `mkfs.krffs` and `mkdisk.sh` cap the
  partition at the window span (and at 4095 MiB regardless).
* `sys_fsync`/`sys_sync` durability proven, and the owning Readme (KRFFS's) rewritten
  in the same commit to state the new fsync contract (it was a RAM no-op; it now
  flushes dirty window pages), per the documentation rule.

Verification: a `persistprobe` two-boot against a scratch image: boot 1 writes
`/persist.txt` and `fsync`s, clean-exits; boot 2 reads it back and asserts content.
An fsync-durability check re-reads the underlying LBA (bypassing the cache) and
asserts the dirty page was persisted. The SSH capstone's dropbear host-key
open/write/fsync/rename path stays green, proving fsync is honestly durable.

### Phase 6: the dirty-mount flag and crash recovery

Goal: detect an unclean mount and recover through the host fsck.

Deliverables:
* The kernel sets the root-node dirty bit on mount (write-through to the device) and
  clears it on a clean `sync`/shutdown. `init_krffs_window` observes the bit and
  warns; the milestone proceeds (no kernel-side repair).
* `fsck.krffs --offset` already (Phase 0) reports the bit and walks the ring for link
  consistency.

Hard parts and mitigations:
* The dirty-flag write must reach the disk **before** the crash, so it is the one
  write-through store; if it sat in the cache and the crash dropped it, the image
  would falsely read clean and the test would pass for the wrong reason.
* fsck detects and reports an interrupted node split (the existing walk catches a bad
  `previous_node` link) but does not repair it; the parent_id hierarchy is not
  validated. State both gaps.

Verification: a `crashprobe` against a scratch copy: boot, start a large write, force
a hard stop (triple-fault under `-no-reboot`) before sync; then host
`fsck.krffs --offset 1048576 scratch.img` reports "not cleanly unmounted"; a re-boot
mounts and reaches `File system... OK (KRFFS, N files)`. A clean synced unmount leaves
the bit clear and fsck exits success.

### Phase 7: boot from disk through GRUB, and the >32 MB working-set proof

Goal: BIOS -> MBR -> GRUB -> kernel off a real disk partition, with the KRFFS root on
a second partition, and a stress test that proves the cap is lifted.

Deliverables:
* `krffs/mkdisk.sh` extended (or a sibling) to build a bootable MBR disk: partition 1
  a small GRUB-readable boot filesystem holding `/boot/kernel` (the existing ELF) and
  `/boot/grub/grub.cfg`, partition 2 the raw KRFFS image at LBA 2048-aligned start.
  GRUB embeds `core.img` in the post-MBR gap (first partition at >= 1 MiB leaves it);
  the kernel stays Multiboot 1. Prefer a FAT boot partition built with `mtools`
  (no privileged loop mount in Docker) over ext2 + `grub-install` on a loop device.
* A kernel-side MBR parser `blk_probe_krffs_partition` that reads LBA 0, checks the
  0x55AA signature, walks the four entries at 0x1BE, reads each non-empty partition's
  first sector, and accepts the first whose leading node has magic 0x4B52, type
  `KRFFS_ROOT_NODE`, and `previous_node_size == 0` plus a plausible size (tightening
  the weak 16-bit signature). It probes each partition's first LBA, never LBA 0 (which
  holds GRUB boot code).
* A `rach2.root=disk` cmdline flag (a whole-token `kcmdline_has` match, no new
  key=value parser needed) selecting the disk mount. The mount block at
  `kernel.c:2132-2150` is restructured to `if (use_disk) { disk } else if (num_modules)
  { module } else { none }`, so a zero-module disk boot does not fall to the
  "no ramdisk" arm.
* `run_from_disk.sh` (paralleling `run_from_iso.sh`): boots the raw disk as
  `if=ide,index=0,media=disk`, `-cpu max`, sourcing `qemu_devices.sh` and setting
  `NET_HOSTFWD` to `:2222-:22` so the SSH capstone reaches dropbear. No `-kernel`, no
  `-initrd`.

Hard parts and mitigations:
* The kernel ELF on partition 1 must stay in sync with `build/rach2/kernel`; unlike
  the module path GRUB does not reload a fresh kernel. `make os-real-disk: rach2 ...`
  rebuilds the disk, and `run_from_disk.sh` warns if the built kernel is newer than
  the image, so a stale-kernel boot is caught.
* The stress proof must keep the **instantaneous resident set** under 32 MB while the
  **on-disk FS** exceeds it; "working set > 32 MB" is the wrong framing. The disk is
  64 MB with a 48 MB KRFFS partition; the probe creates and re-reads files whose live
  on-disk data exceeds 32 MB, forcing continuous eviction, checksumming every read.

Dependencies: all prior phases (the disk boot only changes where the bytes come from;
the cache must already work).

Verification, staged:
* Host: `make os-real-disk` then `qemu-system-i386 -drive ...,if=ide` shows the GRUB
  menu and `multiboot /boot/kernel` reaches the debug shell off partition 1 (proves
  MBR + embed + boot-fs read) before any disk-KRFFS path runs. `fdisk -l` and a
  `dd | xxd` of partition 2's first sector confirm the 0x4B52 magic landed.
* In-guest: the `disk` debug command dumps LBA 0 and the MBR parser prints the found
  `start_lba`/`nr_sectors` matching `fdisk`.
* End to end: `run_from_disk.sh os-real` with `rach2.root=disk` boots to
  `File system... OK (KRFFS, N files)` with the same file count as the module boot,
  `/sbin/init` runs, and the SSH capstone logs into busybox ash, proving KRFFS is
  fully readable off the disk.
* `stressprobe` on the 64 MB disk exits 0 with every checksum intact, proving the
  32 MB physical cap is lifted.
* Regression: the module-path `make test`, `make verify-style` (8/8), and
  `make verify-host` stay green; a module-path real boot and the SSH capstone stay
  green.

## What stays deferred, and why it is sound to defer

* **AHCI / DMA / IRQ-driven I/O.** The IF=0 fault forces polled PIO; AHCI needs the
  completion IRQ the fault cannot service. ATA PIO fully exercises the design.
* **64-bit offset / `_llseek` widening.** Orthogonal to the physical-cap lift; the
  4 GiB logical cap stays, and the window span caps tighter still.
* **A kernel journal and crash-atomic rename.** Out of scope; the dirty flag plus
  offline fsck is the honest floor for a teaching FS, and the plan states that
  multi-store metadata is not crash-atomic.
* **A KRFFS free-list index.** The full-ring allocator scan faults many header pages
  per create; a known cost, deferred, bounded by the size gate and the stress test.
* **A real VFS (vnode/mount table).** Buys nothing until a second on-disk filesystem
  exists, which Path B does not introduce; deferring keeps it a clean additive change
  later rather than churned call sites now.

## House discipline

Each phase compiles clean under `-Werror`, lands in scoped commits in the repo's
one-line style with no attribution, leaves `make verify-style` at 8/8 and
`make verify-host` green, and keeps the module-path `make test` and a real boot green.
Substantial phases (Phase 2 above all) get a harsh independent Codex review with
every finding fixed before commit. A change that alters a contract (the fsync
durability change, Phase 5) rewrites the owning Readme in the same commit. The
vendored trees stay out of every diff.
