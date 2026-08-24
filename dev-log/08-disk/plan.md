# Path B: KRFFS on a disk-backed page cache

Status: complete. All eight phases were built, verified, and committed; see `Readme.md` for the milestone record.

## The goal

Move KRFFS from the 32 MB physical RAM-disk module to a block device of arbitrary size. KRFFS continues to walk pointers through one contiguous virtual window. Accessing an absent window page causes a fault; a new kernel fault path reads the backing sectors into a page-cache frame and maps it. Dirty pages are written back, and pages are evicted under pressure. Only the working set remains resident, so the on-disk filesystem can be much larger than RAM. The final phase boots from that disk through GRUB.

KRFFS itself is almost untouched: it already confines all address arithmetic to `[fs->node, fs->node + fs->size)` (`krffs_wrap_node_position`, `krffs_get_next_node`, the `node + 1` data alias), with no physical-address, DMA, or alignment assumption. The work is the machinery underneath the pointer: a block driver, a kernel page cache, a fault-handler branch, a disk boot, and the host tools to build and check a partitioned image.

## Synchronous fault I/O

The page-fault handler runs with interrupts disabled. Vector 14's stub does `cli` (`isr_handler.s:17`) and the ISR router never re-enables IF for an exception (`isr_handler.s:93`, "IRQ stubs run with IF=0 throughout"); only the syscall path sets `in_syscall` and then `sti`. So `_page_fault_handler` (`paging.c:382`) runs at IF=0 start to finish. The only sleep primitive, `wq_wait_event`, parks by calling `schedule()` and then `sti; hlt; cli` (`wait_queue.c:134`). Sleeping inside the fault would re-enable interrupts mid-exception, let the timer switch stacks against a half-restartable instruction, and reopen the handler to reentrancy.

The fault path therefore uses synchronous polled PIO. It programs the ATA registers, waits for BSY/DRQ on the status port with a bounded spin, copies the sectors with `insw`, and returns. It uses no ATA IRQ, wait queue, or `sti`. This choice follows from the interrupt state and has three consequences:

* Concurrency. A fault that never yields means a syscall body walking or mutating KRFFS never voluntarily context-switches mid-operation, and the timer cannot preempt it because timer rescheduling occurs only when `in_syscall` permits it (`kernel.c:131`). So KRFFS's multi-store metadata mutations (split, resize, remove, rename) stay atomic with respect to other processes exactly as they are today in RAM. No FS-wide lock is needed. A sleeping fault would have broken this; a polling fault preserves it.
* Boot context. With no sleep, mounting the FS at boot (before the scheduler is enabled) needs no special idle-spin reasoning. The fault completes inline.
* Reentrancy. A nested fault during the read cannot happen, because the PIO driver reads into an identity-mapped physical frame by its low address, never through the window.

Each cache miss stalls the machine for the sector transfer (256 `insw` per sector), so timer and in-flight NIC or HDA interrupts are delayed. This is acceptable for a teaching OS with a small working set. The bounded spin returns `-EIO` when a drive does not respond, and the fault handler then panics because it cannot recover. The bound prevents an infinite wait.

## Shared window page tables in `_kernel_dir`

`clone_page_directory` copies all 1024 PDE slots by value at fork time (`paging.c:555-557`): the child gets a snapshot of the kernel directory's page-table pointers. A page table added to `_kernel_dir` *after* a process has forked is invisible in that child's CR3. KRFFS is walked from syscall bodies running under the calling process's own CR3, so a window fault from a forked process resolves against that process's directory, which must already contain the window's page tables.

So every page table spanning the window must be allocated in `_kernel_dir` at mount time, with its PTEs left not-present, before the first fork. The fault handler and the reclaimer then only ever edit leaf PTEs in those shared tables; every process sees the edits through the shared physical tables. This mirrors how `init_paging` eagerly builds the identity map and how `map_mmio` builds MMIO tables. Boot ordering already permits it: `init_krffs` is at `kernel.c:2136`, the first fork (`boot_init_process`) is at `kernel.c:2204`.

The plan corrects two points from earlier designs:

* The fault-handler branch must key purely on the fault address being in the window, not on `have_user_proc`. A kernel-context fault (proc0 at boot, with `cr3 == 0`) has `have_user_proc` false (`paging.c:391`), and a CPL=0 syscall-body fault has `user_mode` false, so both fall straight to the panic at `paging.c:433` unless the new address-keyed branch appears first.
* The window page tables must be installed supervisor-only. Every existing table-creating helper forces the U bit (`ensure_user_table` writes the PDE with `| 0x7`). The window PDEs must be hand-installed like the identity map (`| 0x3`), and the fault-time PTE writer must not route through `ensure_user_table`. Do not set the global (G) bit: CR4.PGE is off, so G buys nothing today and would, if PGE were ever enabled, let an evicted-then-reused frame keep a stale global translation across a context switch. A new `map_kernel_frame(window_va, frame, P|RW)` and a matching unmap provide this interface.

## Milestone scope decisions

These are settled here so the phases do not relitigate them.

* The block driver uses polled PIO ATA rather than AHCI or IRQ-driven I/O. It supports legacy ATA on the primary channel (ports 0x1F0-0x1F7, control 0x3F6), LBA28, no PCI, and no DMA pool. This textbook driver is compatible with the IF=0 fault path and exercises the block and cache design. AHCI is out of scope because it needs DMA, an IRQ for completion that the fault path cannot service, and PCI MMIO setup. The unused DMA-pool module proposed by earlier drafts is removed from the plan. LBA28's 128 GiB ceiling exceeds the 4 GiB KRFFS cap.
* The cache uses write-back with a working `fsync`, rather than write-through. Write-through would make every byte-granular KRFFS metadata store a synchronous sector write without making a multi-store atomic. Write-back marks the page dirty on a write fault and flushes it on eviction or `fsync`. `fsync` and `fdatasync`, which are no-ops at `syscall.c:1117`, flush the window's dirty pages so Dropbear's open/write/fsync/rename host-key sequence persists its changes. The dirty-mount superblock flag is the exception: it is written through so a crash cannot leave it falsely clean (see the crash phase).
* Dirty-flag + offline host fsck for crash recovery, no kernel journal. On mount the kernel sets a dirty sentinel; a clean sync/unmount clears it. After a crash the operator runs the host `fsck.krffs`. KRFFS rename and multi-store metadata are not crash-atomic and the plan says so; fsck detects and reports structural damage and the dirty flag, and does not attempt to repair an interrupted node split. fsck does not validate the v2 parent_id hierarchy (it walks the physical ring only); that gap is stated, not closed.
* The 4 GiB partition cap stays. The OFD offset is `uint32_t` (`fd.h:83`), `_llseek` rejects `>0xFFFFFFFF` (`syscall.c:2666`), and `krffs_file_system.size` is `uint32_t` (`krffs.h:86`). Path B lifts the 32 MB *physical* cap, not the 4 GiB *logical* cap. The window imposes a tighter limit: it must begin at or above `USER_ADDR_MAX` (0xC0000000) and end below the PCI MMIO hole (~0xFE000000), leaving well under 1 GiB. `init_krffs` must reject a partition whose byte size exceeds the window span and must not let `nr_sectors * 512` overflow a 32-bit intermediate. A later milestone can widen offsets to 64 bits.
* `block_device.sector_count` is `uint64_t`. The device can describe a >4 GiB disk even while KRFFS cannot use one; the block layer does not inherit the 32-bit cap.
* The work does not rewrite the VFS. It adds no `struct vnode`, `mount_table[]`, or per-filesystem operations table. KRFFS is the only disk filesystem, and the synthfs classifier already routes /dev, /proc, and /dev/pts independently. Disk-backed KRFFS uses the same `krffs.c` over the same window. The edits are limited to the mount site, a disk mount entry point, `fsync`, and the size check. The roughly 30 KRFFS and synthfs call sites in `syscall.c` remain unchanged. /dev and /proc remain outside a table-driven mount design because `getdents64` deliberately omits them from KRFFS listings.
* The multiboot-module path remains the default and the development and test fallback. A new command-line flag selects disk boot. `make test`, `run.sh`, `runtests.py`, and the module mount remain byte-identical, so the existing verification path does not change. The first implementation tests the cache under the existing `-kernel` and `-drive` boot; GRUB disk boot is the last phase.
* The window starts at 0xC0000000, equal to `USER_ADDR_MAX`. `in_user_range` covers `[0x02000000, 0xC0000000)` (`paging.c:392`), so a fault at 0xC0000000 or above does not reach `vm_resolve` or `cow_fault`. At runtime, the lowest probed device BAR limits the span. This avoids a collision with a device whose MMIO BAR is below the expected address.

## The phases, and why they are ordered this way

The dependency order is fixed. A block read/write primitive is needed before a cache frame can be filled. The page cache and fault branch are needed before KRFFS can use the window. The window is tested over a RAM stub before a disk is introduced, which separates filesystem relocation defects from driver defects. Disk boot follows the working cache because it changes only the source of the bytes. The size check precedes any image large enough to require it. Host-tool offset support has no kernel dependency and comes first so the on-disk format is tested early. Each phase can be built independently, passes `make verify-style` (8/8) and `make verify-host`, and preserves the default module path so `make test` and a boot continue to pass.

### Phase 0: partition-aware host tools and the disk-image pipeline

Goal: build and inspect, entirely on the host, a KRFFS image at a byte offset inside a larger disk image before changing kernel code. This establishes the on-disk format and the location of the dirty flag.

Deliverables:
* Thread an `--offset BYTES --size BYTES` pair through the six host tools' `main` functions (`mkfs.krffs`, `fsck.krffs`, `stageimage.krffs`, `defrag.krffs`, `fsviz.krffs`, `edfs.krffs`). Each forces `krffs_map_file(fd, 0, st_size)` today; `krffs_map_file` already forwards an `off_t offset` to `mmap` (`krffs_platform.c:53`), so only the callers and the `st_size < sizeof(node)*2` guards change. A shared `krffs_parse_region_args` validates the pair: the offset must be 1 MiB-aligned and within the file, and because KRFFS's size is extrinsic (the node ring tiles exactly `[0, size)`), a nonzero offset selects a partition that does not reach the end of the disk and so requires an explicit `--size` rather than defaulting to the rest of the file.
* Decide and implement the dirty-mount flag's on-disk location in the same change. The root node's `type` byte cannot store it because every reader compares that entire byte for equality (`root->type != KRFFS_ROOT_NODE` at `krffs.c:1301`, and the same in the host tools); setting a high bit there would fail the mount (Codex review finding). The root node has no spare field, so use a reserved high bit in the root's `mode`, a root-only field that no reader compares in full for structural validation. The kernel masks the mode to `0777` and file-type bits and never compares the full `uint16_t`. This does not change the format, so the existing mkfs, fsck, stageimage, FUSE tools, and kernel reader continue to use the v2 layout. `fsck.krffs` masks the bit out of the printed mode and reports it as "not cleanly unmounted". Phase 0 confirms by grep that no reader compares the root's `mode` in full.
* `krffs/mkdisk.sh`, paralleling `mkimage.sh`: `dd` a flat image, write a minimal MBR with one primary KRFFS partition starting at LBA 2048 (1 MiB, page- and 64-KiB-aligned, so the host `mmap` offset is always valid on Linux and Windows), then `mkfs.krffs --offset $((2048*512)) --size ...` and `stageimage.krffs --offset $((2048*512)) ...`. The KRFFS bytes written are byte-identical to today's module image; the host KRFFS tooling logic is otherwise unchanged.
* A `make os-real-disk` target (mirroring `os-real-iso`) that builds a 64 MB disk with a 48 MB KRFFS partition, large enough to demonstrate an on-disk filesystem larger than the 32 MB resident cap.

Hard parts and mitigations:
* `mmap` EINVALs on a non-page-aligned offset. Standardize on LBA 2048 in `mkdisk.sh` and reject a misaligned `--offset` in the tools with `tool: offset not 1 MiB-aligned` rather than letting `mmap` fail opaquely. State the guard as "1 MiB", not "page-aligned", because Windows `MapViewOfFile` needs 64 KiB granularity.
* A 16-bit magic (0x4B52) plus `type == 0` is a weak partition signature. The kernel probe (Phase 6) tightens it; the host tools trust the explicit `--offset`.

Verification: on a 5 MiB `d.img`, `mkfs.krffs --offset 1048576 --size 4194304 d.img` then `fsck.krffs --offset 1048576 --size 4194304 d.img` reports success. `fsck.krffs --offset 0 d.img` on the same file FAILS because byte 0 has no magic, demonstrating that the tools use the offset. `mkfs.krffs --offset 512 d.img` exits non-zero because the offset is misaligned, as does `mkfs.krffs --offset 1048576 d.img` because a nonzero offset requires `--size`. `make verify-style` and `make verify-host` pass. This phase changes no kernel code, so it cannot affect `make test`.

### Phase 1: the block layer and the polled PIO ATA driver

Goal: a `block_device` vtable with a polled ATA backend that reads and writes page-sized, sector-aligned chunks, with no IRQ and no DMA.

Deliverables:
* `sys/block/block.h`: `block_device_t { uint32_t sector_size; uint64_t sector_count; int (*read)(block_device_t*, uint64_t lba, uint32_t count, void *buf); int (*write)(block_device_t*, uint64_t lba, uint32_t count, const void *buf); void *drvdata; }`, returning 0 or `-EIO`, plus `extern block_device_t *block0`. A thin partition view whose `read`/`write` add `start_lba` and forward to the whole-disk device is the offset+length slice.
* `sys/block/ata.c`: `ata_init` sets nIEN (the channel raises no IRQ for the poll-only driver) and does IDENTIFY; capacity is the 28-bit user-addressable count in words 60-61, since the driver issues only LBA28 commands and so advertises exactly what those can address. `ata_read`/`ata_write` program LBA28 + READ/WRITE SECTORS (0x20/0x30), poll BSY then ERR/DF/DRQ (status is valid only once BSY clears) on the status port, copy 256 words per sector with `insw`/`outsw`, and a WRITE issues FLUSH CACHE (0xE7) with the post-write and post-flush waits checking ERR/DF. All polled, no `sti`.
* `insw`/`outsw` added beside `inw`/`outw` in `kio` (a two-instruction `rep insw`/`rep outsw` wrapper; no string-port primitive exists today).

Hard parts and mitigations:
* The 400 ns post-command settle: read alt-status four times before trusting BSY. This is the one in-body hardware-fact comment the standard allows.
* BSY/DRQ handshake is per-sector, not one-time: poll BSY clear then DRQ set before each sector's transfer, with an ERR/DF check and a bounded spin returning `-EIO`.
* The bounded spin is the only watchdog (IF=0 means no PIT to time by); a wedged or absent drive returns `-EIO`, never hangs.
* Even unshared, ATA holds IRQ 14 asserted until status is read. Since the driver is poll-only, no IRQ handler is registered, so this is moot for the cache; if a diagnostic ever wires IRQ 14, its handler must read status to deassert INTRQ.
* QEMU IDE topology: `-cdrom` is ATAPI on the secondary channel (IRQ 15), a different command set. The disk must be `-drive file=disk.img,format=raw,if=ide,index=0,media=disk` to land a plain READ-SECTORS disk on the primary channel at 0x1F0.

Dependencies: none in-kernel. Builds on `inb`/`outb`/`inw`/`outw` (`kio`), nothing else.

Verification: a `DISK=` option on `run.sh` attaches the disk, and a `disk` command in the debug shell calls `ata_read(0, 1, buf)` and hexdumps 512 bytes. The 0x55AA signature at offset 510 and the partition entry at 0x1BE must match `fdisk -l` for the image. A separate, explicit `disk write` round-trips the disk's last sector, which lies beyond the partition and boot gap and therefore changes neither a mounted file system nor a future bootloader. This checks WRITE + FLUSH + READ. `block0->sector_count` must equal the image size divided by 512, checking the IDENTIFY parse. The debug shell runs with `current_proc == proc0` and no other process scheduled, so success there shows that the polled path needs no scheduler. `make verify-style`, `make verify-host`, and the module-path `make test` pass; the driver is unused until Phase 3 calls it.

### Phase 2: the kernel page cache and the disk-backed window

Goal: add a fixed-size page cache that fills, dirties, writes back, and evicts frames behind a kernel virtual window serviced by a new fault-handler branch. This phase has the most implementation risk, so its substeps can each be checked independently against the Phase 1 driver or a RAM stub.

Deliverables: `sys/mm/pagecache.c` + `.h`, peer to `frame.c`/`paging.c`.

Sub-steps, in order:

* 2a. Window reservation and supervisor-only pre-mapping. `init_pagecache`, called from `k_main` after `init_paging` and before `init_krffs`, fixes `PCWIN_BASE = 0xC0000000` (asserted `== USER_ADDR_MAX`), clamps `pc_window_size` against the lowest probed device BAR, and pre-allocates every level-2 page table spanning the window in `_kernel_dir` with not-present, supervisor-only PTEs. `map_kernel_frame` / `unmap_kernel_frame` write a single leaf PTE (`P|RW`, no U, no G) into those shared tables and `invlpg`. Check: an `assert(PCWIN_BASE == USER_ADDR_MAX)` and an assert that the window's PDEs exist in `_kernel_dir` before any fork.
* 2b. Frame reservation against OOM. Pre-reserve `PC_SLOTS` frames for the cache at init out of the `_frames[]` pool (marked used, never `U`-bit, so `alloc_user_page` and `free_user_pages` never touch them), so a cold-cache fault always has a frame and the handler never panics on transient memory pressure. Size `PC_SLOTS` small and fixed (256 = 1 MiB) so user pages, page tables, and the cache coexist under the 32 MB pool with room for the os-real end-to-end test, even though the reservation also exists on the inactive module path. A fixed reservation prevents user allocations from exhausting the frames needed by a cache fault.
* 2c. The cache table and lookup. `pc_entry { uint32_t disk_off; uintptr_t phys; uint8_t valid, dirty, accessed, pinned; }`, a fixed `pc_slots[PC_SLOTS]`, and a clock hand. One device, so the key is `disk_off`; an open-addressed hash on `disk_off >> 12` keeps lookup O(1). Check this logic with the host test below.
* 2d. The fault branch. In `_page_fault_handler`, before the panic at `paging.c:433` and address-keyed (not `have_user_proc`-keyed): `if (fault_address >= PCWIN_BASE && fault_address < PCWIN_BASE + pc_window_size) { if (pc_fault(fault_address, write)) return; k_panic("pagecache: disk fault"); }`. `pc_fault` computes the page-aligned disk offset, looks it up, on a miss reserves a free slot (pinned across the read), polls `block0->read` of the 8 sectors into the slot's frame, maps it supervisor-only at the window VA, sets `accessed` (and `dirty` on a write fault), unpins, and returns 1. Bound the branch by the actual mounted FS size rather than the full window span, so a wild kernel pointer into the window still causes a panic. Assert `current_proc != NULL`.
* 2e. Eviction. `pc_get_free_or_evict` first takes a free slot; under pressure it runs the clock (clear `accessed` and skip, else victim), never picks a `pinned` slot, writes back a dirty victim with `block0->write` (polled, still IF=0), `unmap_kernel_frame`s it, and reuses it. The `pinned` flag guards both the in-flight fill frame and any frame a straddling access just mapped. Writeback precedes PTE clear and frame reuse, so no dirty page is dropped.
* 2f. Working fsync. `pc_sync` walks `pc_slots` and writes back every dirty entry, clearing dirty after each successful write. `sys_fsync` (`syscall.c:1117`) calls it; add `sys_sync`, which does not exist today, and have it call `pc_sync` unconditionally. The superblock dirty-flag write is the one write-through store, so it reaches the disk before any later dirty page.

Hard parts and mitigations (beyond the structural ones already settled above):
* A straddling access faults two adjacent pages. KRFFS does byte-granular `memcpy` across node boundaries; the CPU faults each page in turn. `pc_fault` is idempotent and the `pinned`/`accessed` flags stop a re-fault from evicting the partner just mapped. No multi-page atomic fault is needed.
* The frame is double-mapped (identity alias + window VA). Every cache frame is also live and kernel-writable at its low identity address. The "supervisor-only window" isolates the mapping from ring 3 but not from kernel code. KRFFS writes through the window base, so the CPU dirties the window PTE. Dirty tracking uses `pc_dirty`, which the write fault sets, rather than the PTE dirty bit; the identity alias therefore does not affect writeback. State this invariant and do not write filesystem bytes through the identity alias.
* A grow-relocation `memcpy` can demand more resident pages than fit. `krffs_resize_reserved_node` copies a whole file in one synchronous loop; a file approaching the resident budget cannot be held entirely. Milestone scope caps a single file well under the `PC_SLOTS` budget; the size check (Phase 5) and stress test (Phase 7) set and verify this bound. A free-list index for KRFFS to avoid the full-ring allocator scan is a known cost and remains deferred.
* The window source faults inside `copy_to_user`/`copy_from_user` (Codex review). `sys_read`/`sys_write` pass KRFFS's node pointer, a window address, directly to the user-copy, which then reads or writes the window and faults in its pages. The existing read/write prefault covers the USER buffer but not the window side. The user-copy runs in syscall context, where the polled fill can service a window fault. The copy must acquire no resource that the fault path also needs; it currently does not, because the fault path touches only the page cache. No window prefault is added; the fault path remains the single entry.

Dependencies: Phase 1 (the driver `pc_fault` calls). Can be developed first against a RAM stub that reads `ramdisk->start + off`, decoupling it from the block device.

Verification, smallest first:
* Host test `pagecache_hosttest.c` under `make verify-host`: check the bookkeeping (slot lookup, clock victim, dirty set/clear, and sector-to-page math) under ASan/UBSan with a fake backing callback. Assert that a fault fills the right sectors, a write marks a page dirty, an eviction writes back if and only if the page is dirty, a repeated fault reads again, and the victim is the least recently used. This test needs no hardware.
* Window-fault probe (RAM stub): point the window at a stub returning a known pattern; touch `PCWIN_BASE`, `+0x1000`, `+0xFFF` (straddle); assert the bytes match and a second touch is a hit, not a re-read.
* Eviction probe: set `PC_SLOTS` tiny, write distinct patterns through more pages than slots, read all back; assert every page round-trips. Print `VMPROBE PAGECACHE OK n=<reads> evict=<e> wb=<w>` (the existing VMPROBE token convention) from a self-test so a boot-log assertion catches regressions.

### Phase 3: KRFFS on the window over the RAM stub

Goal: mount KRFFS on the demand-paged window instead of the RAM module, while retaining the RAM stub as backing, so the whole stack is exercised before adding a disk. If KRFFS only walks pointers through the window, code above the filesystem remains unchanged.

Deliverables:
* `init_krffs_window(uint32_t disk_size)` (or extend `init_krffs`) that stores `_krffs_fs_instance.node = (krffs_node_t *)PCWIN_BASE` and `.size = disk_size` (rejecting `disk_size > pc_window_size`), with the backing read pointed at the stub. `fs->size` must equal the size the fault branch bounds-checks against, so `krffs_wrap_node_position` folds at the same boundary the handler serves. `krffs.c` is otherwise untouched.
* The mount site at `kernel.c:2136` gains the window branch, selected (for now) by a build/debug switch so the module path stays the default.

Hard parts and mitigations:
* `init_krffs`'s first act dereferences `root->magic`/`root->type`; under the window that is the first fault, served before `krffs_count_files` walks. Fine, but note that the mount-time validation read is itself a demand fault.
* `krffs_count_files` (called right after mount, `kernel.c:2138`) walks the whole ring, faulting in every node-header page. On a large FS this is a full metadata read at boot. Accept it for the milestone; the stress phase bounds the cost.

Dependencies: Phases 1 and 2.

Verification: the existing ten self-tests, a module-style boot, and the SSH end-to-end test (`ssh -i krffs/rach2_demo_ed25519 -p 2222 root@localhost` to BusyBox ash) all pass with KRFFS on the stub-backed window because the filesystem code is unchanged. This regression check verifies that relocation is transparent. Run the self-test write and the /sbin/init ELF load, which faults the binary in through `exec_krffs_read`, to exercise the fault path under proc0.

### Phase 4: block-device backing and in-guest cache probes

Goal: replace the RAM stub with the Phase 1 driver so disk sectors back the window, then verify fill, dirty, writeback, and eviction behavior end to end in the guest.

Deliverables:
* `pc_fault`/`pc_sync`/`pc_get_free_or_evict` call `block0->read`/`block0->write` against the mounted KRFFS partition instead of the stub.
* `harness.py` gains optional `--drive`/`--device`/`--machine` args (defaulting empty, so module runs are byte-identical) and the ability to omit `-initrd` for a disk-only boot. New `*probe` drivers: `blockprobe` (write a pattern through the window, read it back), `cacheprobe` (force at least one eviction, modify a page, evict it, re-read, assert the change survived).

Hard parts and mitigations:
* The probes must verify data integrity (checksums), not merely absence of a crash, since a dropped-dirty or freed-while-mapped bug shows as silent corruption.
* Persistence and crash tests must run against a scratch copy of the image (`cp` before boot), because QEMU mutates a raw drive in place and a `make` re-stage would wipe the evidence.

Dependencies: Phases 1-3.

Verification: `blockprobe` and `cacheprobe` exit 0 under `harness.py` with the disk attached. The module-path `make test` passes, as do the `make verify-host` and `verify-style` targets, because the new arguments default to empty and the new probes use separate invocations.

### Phase 5: the size limit and fsync durability

Goal: enforce the 4 GiB and window-span limits and verify that `fsync` persists data.

Deliverables:
* `init_krffs_window` rejects a partition whose byte size exceeds the window span, with `krffs: partition exceeds window` to stderr, and never overflows `nr_sectors * 512` in a 32-bit intermediate. `mkfs.krffs` and `mkdisk.sh` cap the partition at the window span (and at 4095 MiB regardless).
* Verify `sys_fsync` and `sys_sync` durability, and rewrite the owning KRFFS Readme in the same commit to state the new `fsync` contract. It was a RAM no-op and now flushes dirty window pages, so the documentation rule requires the update.

Verification: run `persistprobe` across two boots against a scratch image. Boot 1 writes `/persist.txt`, calls `fsync`, and exits cleanly; boot 2 reads the file and checks its contents. A durability check bypasses the cache, reads the underlying LBA, and verifies that the dirty page was persisted. The SSH end-to-end test's Dropbear host-key open/write/fsync/rename path also passes, verifying persistence through that sequence.

### Phase 6: the dirty-mount flag and crash recovery

Goal: detect an unclean mount and recover through the host fsck.

Deliverables:
* The kernel sets the root-node dirty bit on mount (write-through to the device) and clears it on a clean `sync`/shutdown. `init_krffs_window` observes the bit and warns; the milestone proceeds (no kernel-side repair).
* `fsck.krffs --offset` already (Phase 0) reports the bit and walks the ring for link consistency.

Hard parts and mitigations:
* The dirty-flag write must reach the disk before the crash, so it is the one write-through store; if it sat in the cache and the crash dropped it, the image would falsely read clean and the test would pass for the wrong reason.
* fsck detects and reports an interrupted node split (the existing walk catches a bad `previous_node` link) but does not repair it; the parent_id hierarchy is not validated. State both gaps.

Verification: a `crashprobe` against a scratch copy: boot, start a large write, force a hard stop (triple-fault under `-no-reboot`) before sync; then host `fsck.krffs --offset 1048576 scratch.img` reports "not cleanly unmounted"; a re-boot mounts and reaches `File system... OK (KRFFS, N files)`. A clean synced unmount leaves the bit clear and fsck exits success.

### Phase 7: boot from disk through GRUB and test more than 32 MB of live data

Goal: boot the kernel from a disk partition through BIOS, MBR, and GRUB, use a second partition for the KRFFS root, and run a stress test that verifies removal of the physical cap.

Deliverables:
* Extend `krffs/mkdisk.sh`, or add a sibling, to build a bootable MBR disk. Partition 1 is a small GRUB-readable boot filesystem containing `/boot/kernel` (the existing ELF) and `/boot/grub/grub.cfg`; partition 2 is the raw KRFFS image at an LBA 2048-aligned start. GRUB embeds `core.img` in the post-MBR gap, which remains available because the first partition starts at or beyond 1 MiB. The kernel remains Multiboot 1. Prefer a FAT boot partition built with `mtools` (no privileged loop mount in Docker) over ext2 plus `grub-install` on a loop device.
* A kernel-side MBR parser `blk_probe_krffs_partition` that reads LBA 0, checks the 0x55AA signature, walks the four entries at 0x1BE, reads each non-empty partition's first sector, and accepts the first whose leading node has magic 0x4B52, type `KRFFS_ROOT_NODE`, and `previous_node_size == 0` plus a plausible size (tightening the weak 16-bit signature). It probes each partition's first LBA, never LBA 0, which contains GRUB boot code.
* A `rach2.root=disk` cmdline flag (a whole-token `kcmdline_has` match, no new key=value parser needed) selecting the disk mount. The mount block at `kernel.c:2132-2150` is restructured to `if (use_disk) { disk } else if (num_modules) { module } else { none }`, so a zero-module disk boot does not fall to the "no ramdisk" arm.
* `run_from_disk.sh` (paralleling `run_from_iso.sh`): boots the raw disk as `if=ide,index=0,media=disk`, `-cpu max`, sourcing `qemu_devices.sh` and setting `NET_HOSTFWD` to `:2222-:22` so the SSH end-to-end test reaches Dropbear. No `-kernel`, no `-initrd`.

Hard parts and mitigations:
* The kernel ELF on partition 1 must stay in sync with `build/rach2/kernel`; unlike the module path GRUB does not reload a fresh kernel. `make os-real-disk: rach2 ...` rebuilds the disk, and `run_from_disk.sh` warns if the built kernel is newer than the image, so a stale-kernel boot is caught.
* The stress test must keep the instantaneous resident set under 32 MB while the on-disk filesystem exceeds it. A "working set > 32 MB" description would be inaccurate. The disk is 64 MB with a 48 MB KRFFS partition; the probe creates and rereads files whose live on-disk data exceeds 32 MB, forcing continuous eviction and checksumming every read.

Dependencies: all prior phases (the disk boot only changes where the bytes come from; the cache must already work).

Verification, staged:
* Host: `make os-real-disk` then `qemu-system-i386 -drive ...,if=ide` shows the GRUB menu and `multiboot /boot/kernel` reaches the debug shell off partition 1, verifying MBR, embedding, and boot-filesystem reads before any disk-KRFFS path runs. `fdisk -l` and a `dd | xxd` of partition 2's first sector confirm the 0x4B52 magic landed.
* In-guest: the `disk` debug command dumps LBA 0 and the MBR parser prints the found `start_lba`/`nr_sectors` matching `fdisk`.
* End to end: `run_from_disk.sh os-real` with `rach2.root=disk` boots to `File system... OK (KRFFS, N files)` with the same file count as the module boot, `/sbin/init` runs, and the SSH end-to-end test logs into BusyBox ash. This checks that KRFFS is fully readable from the disk.
* `stressprobe` on the 64 MB disk exits 0 with every checksum intact, verifying that the 32 MB physical cap is removed.
* Regression: the module-path `make test`, `make verify-style` (8/8), and `make verify-host` pass. A module-path boot and the SSH end-to-end test also pass.

## Deferred work

* AHCI / DMA / IRQ-driven I/O. The IF=0 fault forces polled PIO; AHCI needs the completion IRQ the fault cannot service. ATA PIO fully exercises the design.
* 64-bit offset / `_llseek` widening. Orthogonal to the physical-cap lift; the 4 GiB logical cap stays, and the window span caps tighter still.
* A kernel journal and crash-atomic rename. These are out of scope. The dirty flag plus offline fsck is the minimum recovery policy for this teaching filesystem, and the plan states that multi-store metadata is not crash-atomic.
* A KRFFS free-list index. The full-ring allocator scan faults many header pages per create. This known cost is bounded by the size check and stress test and remains deferred.
* A VFS with vnodes and a mount table. Path B does not introduce a second on-disk filesystem, so it defers the VFS until it can be added without first modifying unused call sites.

## House discipline

Each phase compiles cleanly under `-Werror`, lands in scoped commits using the repository's one-line style without attribution, passes `make verify-style` at 8/8 and `make verify-host`, and keeps the module-path `make test` and a boot passing. Substantial phases, especially Phase 2, receive a strict independent Codex review, and every finding is fixed before commit. A change that alters a contract, such as the Phase 5 `fsync` durability change, rewrites the owning Readme in the same commit. Vendored trees remain outside every diff.
