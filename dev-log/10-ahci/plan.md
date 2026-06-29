# Milestone 10: AHCI, the modern SATA half of the block interface

Status: draft.

## The goal

Add an AHCI/SATA disk driver behind the block interface the disk milestone built, so KRFFS can ride a real SATA HBA the same way it already rides legacy ATA. AHCI is the storage analog of the HDA milestone in audio: the readable bring-up driver shipped first (PC-speaker, then ATA PIO), and now the modern standard that covers most real hardware goes in behind the same vtable (HDA for audio, AHCI for storage). The disk milestone deliberately deferred AHCI; this milestone delivers it and supersedes that note.

The whole point is that AHCI is transparent above `block0`. The page cache, the demand-paged window, the MBR root-probe, `block_partition_init`, KRFFS itself, the dirty flag, and fsck all speak only `block0->read`/`block0->write` (`block.h:20-28`). They neither know nor care which driver published `block0`. So this milestone adds a second producer of `block0` and changes nothing above it. The ATA PIO driver stays as the readable fallback sibling, exactly as the PC-speaker and SB16 stay beside HDA.

## Scope and non-goals

In scope:

* A new `sys/block/ahci.c` + `sys/block/ahci.h` exposing `int ahci_init(void)`, mirroring `ata.c`/`ata.h` (`ata.c:272-338`, `ata.h:13`).
* PCI discovery of an AHCI HBA (class 0x01, subclass 0x06, prog-if 0x01), ABAR (BAR5) map, HBA + first-present-port bring-up, a single polled command slot, READ DMA EXT (0x25) / WRITE DMA EXT (0x35) over a command list / received-FIS / PRDT.
* Polled completion only: spin on `PxCI` clear with `PxIS`/`PxTFD` error checks and a bounded watchdog, callable from `pc_fault` at IF=0.
* A probe seam so the boot path selects AHCI when present, else ATA.
* QEMU `ich9-ahci` test topology on the existing `pc` machine, additive run/test/disk targets.
* KRFFS mount + page-cache window over the AHCI disk, the disk probes over AHCI, and the SSH + Doom + GRUB-boot capstones proven on the AHCI image.

Non-goals (see "What stays deferred"): NCQ, IRQ-driven completion, hot-plug, port multipliers, ATAPI, multiple simultaneous disks, q35, sector sizes other than 512, and any change to the IF=0 polled-fault model.

## Design decision (confirm before code)

This is the load-bearing checkpoint. It must be confirmed before any code is written, because it decides whether the fault path changes at all.

The constraint is fixed and proven in code. The page-cache window fault is delivered through a 32-bit interrupt gate (vector 14, gate flag 0x8E, `idt.c:46`); the exception stub does `cli` (`isr_handler.s:17`) and the router never re-enables IF on the fault path (only the syscall path does, `isr_handler.s:89-93`). So `_page_fault_handler` (`paging.c:391`) and the window branch it dispatches to `pc_fault` (`paging.c:434-444`, comment "The poll-only fill is safe with IF=0") run with IF=0 start to finish and cannot sleep: the only sleep primitive, `wq_wait_event`, would `sti; hlt` mid-exception. The block contract codifies this: `read`/`write` "are synchronous and polled: a caller may invoke them with interrupts off and without the scheduler running" (`block.h:11-18`). The ATA driver honors it with a bounded iteration watchdog, not a timer, because "with interrupts off there is no timer to bound by" (`ATA_SPIN_LIMIT = 10000000`, `ata.c:49-52`), and it masks its IRQ with nIEN (`ata.c:279-281`).

AHCI is normally DMA + completion-IRQ driven. The tension: a completion IRQ is exactly what the fault path cannot service.

The two ways out:

* Polled completion (recommended). Mask the HBA interrupt (leave `GHC.IE` clear, do not set `PxIE`). Issue a command by setting the slot's bit in `PxCI`, then busy-poll `PxCI` until that bit clears, watching `PxIS` (task-file-error `TFES`) and `PxTFD` (`STS.ERR`/`BSY`) for failure, with the same bounded-iteration watchdog idiom as ATA. The HBA performs the DMA in hardware; the CPU only spins on a memory-mapped status bit. This is byte-for-byte the HDA polling idiom already proven in the tree (the GCTL/SRST/ICI/LPIB-advance loops, `hda.c:806-855`, `hda.c:662-691`, `hda.c:946-998`, `hda.c:2686-2714`), none of which sleep or use a wait queue. `ahci_read`/`ahci_write` then satisfy the `block.h:11-18` contract verbatim and drop into `pc_fault -> pc_read -> block_partition_read -> block0->read` (`pagecache.c:281`, `kernel.c:1462`, `block.c:23`) with zero change to the fault model, the page cache, the partition layer, or `kernel.c`'s mount flow. The only genuinely new cost over ATA is a small contiguous-DMA setup at init: the per-port command list (1 KiB aligned), received-FIS area (256 B aligned), and one command table with its PRDT (128 B aligned).

* IRQ-driven completion (not recommended). Enable `PxIE`/`GHC.IE`, take the AHCI completion IRQ, and wake the faulting requester via `wq_wait_event` on `PxIS`. This forces the fault path itself to become sleepable, which the interrupt-gate design (`idt.c:46`, `isr_handler.s:17`) forbids today, and would mean reworking `pc_fault`, the window branch in `paging.c`, and the no-sleep concurrency invariant the disk milestone leaned on (a fault that never yields keeps KRFFS metadata mutations atomic without an FS-wide lock, `08-disk/plan.md:22-58`). Far heavier, and it buys nothing for a teaching OS with a small working set.

Recommendation: keep the IF=0 polled-fault model unchanged and drive AHCI by polled `PxCI`/`PxIS`/`PxTFD` completion. Trade-off owned and not hidden: a cache miss stalls the whole machine for the duration of one 4 KiB transfer (the HBA's DMA, not 256 `insw` per sector as PIO did, so AHCI is actually faster on the stall), the bounded watchdog returns `-EIO` on a wedged port rather than hanging, and the fault handler then panics fail-closed exactly as it does for ATA today. This supersedes `08-disk/plan.md:530-531`, whose deferral reasoning ("AHCI needs the completion IRQ the fault cannot service") is inverted by the polled-completion realization: the HBA does not need its IRQ serviced if the CPU polls the same completion bit.

This choice is confirmed. The phases below build the polled path, and Phase 6 records the decision and the polled-vs-IRQ-vs-async trade-off in a durable place a future kernel contributor would consult (a new `sys/block/Readme.md` and the `ahci.c` file header), so weighing IRQ completion later starts from the analysis rather than rediscovering it.

## The phases, and why they are ordered this way

Smallest-testable-first. Each phase ends at a serial token or a clean boot, so a regression is bisectable to one phase. The IF=0 polled invariant is load-bearing from Phase 2 onward: no phase introduces an IRQ, a `sti`, a wait queue, or an allocation on the I/O path.

### Phase 0: PCI discovery, ABAR map, and an `ahci` debug command (no I/O)

Scope: prove the HBA is found and its registers are readable, with no command issued and no DMA. This is the IDENTIFY-equivalent of "can we even see the device".

Work:

* Add `PCI_CLASS_MASS_STORAGE 0x01`, `PCI_SUBCLASS_SATA 0x06` next to the existing multimedia/audio constants (`pci.h:41-42`). No prog-if-aware finder exists and `pci_find_device`/`pci_find_device_skip` match class+subclass only (`pci.c:141`, `pci.c:191`), so the driver filters on `dev.prog_if == 0x01` itself (already captured, `pci.c:121`). Use `pci_find_device_skip` to walk past a controller in IDE-compat/RAID mode, exactly as `hda_init` walks audio controllers (`hda.c:1569`).
* Recover the ABAR from BAR5, not BAR0: `abar = pci_bar_address(dev.bar[5])` (`PCI_BAR5 = 0x24`, `pci.h:34`; mask strips type bits, `pci.c:345`), first confirming `dev.bar[5]` is a memory BAR (a `pci_bar_is_memory` guard, against a pathological 64-bit BAR4 extending into BAR5). If `abar >= HARD_MEMORY_LIMIT` (0x2000000, `paging.h:16`) call `map_mmio(abar, size)` (`paging.c:471`), which identity-maps cache-disabled (PCD|PWT, `paging.c:501`, mandatory so status-bit polls are not stale) and returns the phys address, 0 on failure. This is the `hda_init` BAR idiom (`hda.c:1604-1611`) with BAR5 substituted for BAR0. Map size: a fixed 0x2000 over-covers the ~0x1100 + 0x80-per-port span.
* Copy `hda_read32`/`hda_write32` as `ahci_read32`/`ahci_write32` over `base + offset` (`hda.c:609-627`), plus a per-port offset helper `0x100 + port*0x80` analogous to `hda_sd_offset` (`hda.c:630`). MMIO is `volatile`.
* `pci_enable_memory_space` + `pci_enable_bus_master` (`pci.c:307-324`); bus-master is mandatory before any HBA DMA even in polled mode (`hda.c:2713-2714` names PCI BM as the cause of a silent no-DMA). Skip `pci_enable_interrupts` (polled). Optionally force D0 (`hda_pci_force_d0`, `hda.c:765-797`).
* Add an `ahci` debug command beside the existing `audio` HDA diagnostic (`kernel.c:1822`) that calls a discovery-only entry and dumps CAP, GHC, PI (implemented-port bitmask), and per-port `PxSSTS`/`PxSIG`/`PxTFD`. No `block0` change yet.
* Build: add `sys/block/ahci.c` to `C_SRC` right after `sys/block/ata.c` (`Makefile:30`); `make check-sources` (`Makefile:138-140`) fails until it is listed, and `-MMD` deps (`Makefile:133`) pick up the new header.

Hard parts: ABAR is BAR5 (the single easiest mistake porting the BAR0 HDA idiom). The HBA may report prog-if != 0x01 under an IDE-compat QEMU model; filter and skip. Mitigation: the dump command surfaces exactly what the HBA presents before any I/O depends on it.

Dependencies: none beyond the existing PCI layer (which self-scans; `init_pci` at `pci.c:366` is debug-only and never called, do not start calling it).

Gate: boot with the AHCI device attached (Phase 6 topology, brought forward as a manual run), run `ahci` at the rescue shell, see CAP/GHC/PI and a port with `PxSSTS.DET == 3`. Default RAM boot unchanged.

### Phase 1: HBA + first-present-port bring-up (DMA structures, no transfer)

Scope: take ownership, enable AHCI mode, reset, select the boot port, allocate and program the DMA structures, start the port engine. Still no command issued.

Work:

* BIOS/OS handoff: if `CAP2.BOH` is set, request via `BOHC.OOS` and poll `BOHC.BOS` clear (bounded). Gate on the capability and skip when clear, mirroring HDA's "no-op when capability absent" (`hda.c:765-797`); QEMU ich9-ahci typically has none.
* `GHC.AE` (bit 31) to present AHCI registers; `GHC.HR` (bit 0) HBA reset, poll clear (bounded, the `ata_wait`/`hda_reset` edge-poll idiom, `ata.c:82-93`, `hda.c:816-836`), re-assert `GHC.AE` (HR clears it).
* Enumerate `PI`; pick the lowest implemented port with a device present (`PxSSTS.DET == 3`, `IPM == 1`, `PxTFD` not BSY/DRQ). First-present-port, not always port 0, to match real hardware while staying single-disk like the rest of the milestone (ATA is primary-master only).
* Per-port stop-before-program (AHCI 10.1.2): clear `PxCMD.ST` then `PxCMD.FRE`, poll `PxCMD.CR` and `PxCMD.FR` clear (bounded).
* Allocate the DMA structures as static `volatile __attribute__((aligned(N)))` BSS arrays, the proven HDA/SB16 idiom (`hda.c:42`, `hda.c:77-93`, `sb16.c:36`): command list `aligned(1024)` (32 headers x 32 B), received-FIS `aligned(256)`, command table `aligned(128)`. They land in `.bss` below `k_phys_mem_end`, hence below `HARD_MEMORY_LIMIT` (32 MiB), hence identity-mapped and < 4 GiB, so `(uint32_t)(uintptr_t)arr` is the physical DMA address with no translation. The toolchain already honors these alignments (`bdl` is `aligned(1024)`, `hda.c:92`). Packed descriptor structs mirror `hda_bdl_entry_t` (`hda.h:185-190`): command header (32 B), command table (128 B command-FIS area + PRDT), PRDT entry (DBA/DBAU/reserved/DBC-with-I-bit).
* Program `PxCLB = (uint32_t)(uintptr_t)cmdlist`, `PxCLBU = 0`; `PxFB = fis`, `PxFBU = 0` (the zero-upper idiom, `hda.c:2398`). Clear `PxSERR` (write 1s, write-1-to-clear like `HDA_SD_STS`) and `PxIS`. Set `PxCMD.FRE`, then `PxCMD.ST`.
* Extend the `ahci` dump to show the programmed bases and `PxCMD` engine state.

Hard parts: the slower SATA link-up under QEMU with IF=0 (at init IF is still on, so this is the easier context, but keep the watchdog generous). The reset/spin handshake must never spin unbounded: a wedged port at init would hang before the scheduler. Mitigation: every edge is a bounded countdown returning failure, exactly as ATA and HDA do.

Dependencies: Phase 0.

Gate: `ahci` shows the engine started (`PxCMD.ST`/`FRE`/`CR`/`FR` set), bases programmed, no error in `PxTFD`. Still no `block0` change; default boot unchanged.

### Phase 2: a single polled READ DMA EXT of sector 0, validated against the MBR

Scope: the smallest real transfer. Issue one READ DMA EXT for LBA 0 into a static identity-mapped buffer, poll to completion, and validate against the known MBR (the 55AA signature). This is the IDENTIFY-equivalent milestone for the I/O path: prove the DMA-plus-poll round trip end to end before wiring `block0`.

Work:

* IDENTIFY DEVICE (0xEC) to size the device from words 100..103 (48-bit count), replacing `ata_init`'s IDENTIFY (`ata.c:283-322`). AHCI uses LBA48, so the LBA28 cap (`ATA_LBA28_MAX_SECTORS`, `ata.c:46-47`) does not apply; advertise the full count (the window cap clamps usable size regardless). Clamp logical sector size to 512 (assert; `BLOCK_SECTOR_SIZE`, `block.h:7`; KRFFS sector math assumes 512).
* Build command header[0]: `CFL = 5` dwords (H2D FIS), `W = 0` (read), `PRDTL = 1`, `CTBA = cmdtable`, `CTBAU = 0`. Build a Register H2D FIS in the command table (type 0x27, C bit, command 0x25, LBA spread across lba0..5, count, device 0x40 for LBA mode). One PRDT entry: `DBA = buffer`, `DBAU = 0`, `DBC = bytecount - 1`.
* Issue: first poll `PxTFD` until `STS.BSY == 0` and `STS.DRQ == 0` (AHCI requires the task file idle before a command issues), bounded, then set `PxCI` bit 0. Poll `PxCI` clear with the `ATA_SPIN_LIMIT`-shaped bounded watchdog (`ata.c:52`); on each spin check `PxIS.TFES` and `PxTFD.STS.ERR` (bit 0) for failure, return `-EIO` on error or expiry, clear `PxIS`/`PxSERR` (write-1-to-clear) after.
* Cache coherency: add an `ahci_dma_sync_range` equivalent of `hda_dma_sync_range` (`hda.c:244-268`, CLFLUSH + barrier, WBINVD fallback, CLFLUSH probed via CPUID) and flush the command table/PRDT before issue, invalidate the buffer after completion. No-op under QEMU, present for real-hardware honesty.
* A `disk` debug-path variant or a one-shot self-test that reads LBA 0 via the AHCI path and prints the signature, run from the rescue shell.

Hard parts: the FIS/PRDT field layout (the place most AHCI ports go wrong). The data buffer must be physically contiguous for a single PRDT entry. Under the identity map every address below `HARD_MEMORY_LIMIT` is contiguous (phys == virt, `paging.c:140-175`), so any static or identity-mapped buffer there fits one PRDT entry regardless of page alignment: the static self-test buffer, the runtime page-cache frames (one page from `alloc_kernel_frame`, `pagecache.c:488`, `paging.c:525`), and the `kernel.c` MBR/partition buffers (`kernel.c:1487-1488`) all qualify (`pc_host_pages` is the host-test backing, `pagecache.c:3-13`, not a runtime path). The command list, received-FIS, and command-table structures get their AHCI alignment from `__attribute__((aligned))` as in Phase 1.

Dependencies: Phase 1.

Gate: the self-test reads LBA 0 over AHCI and reports the MBR 55AA signature matching what the ATA path reads from the same image. Bounded poll proven (a deliberately wedged read returns `-EIO`, not a hang).

### Phase 3: the full block_device backend behind block0, with the probe seam

Scope: turn the proven single read into the full read/write backend, publish `block0`, and add the probe that selects AHCI when present else ATA.

Work:

* `ahci_read`/`ahci_write` with the exact `block_device_t` signature (`block.h:20-28`): 512 sector size, the IDENTIFY count, polled completion. Multi-sector: the page cache issues exactly `PAGE_SIZE/BLOCK_SECTOR_SIZE = 8` sectors per call (`kernel.c:1465-1466`), one 4 KiB page = one PRDT entry, so a single-entry PRDT covers every exercised call. Bounds-check `count == 0`, `lba` past device, run-past device, replicating ATA's guards (`ata.c:183-186`, `ata.c:231-234`). WRITE DMA EXT (0x35) sets the `W` bit and the write command in the H2D FIS, and each write is followed by a polled FLUSH CACHE EXT (0xEA) so the data reaches durable media, matching the ATA driver's post-write flush (`ata.c:259-262`) that the dirty-flag and `pc_sync` durability contract relies on (`kernel.c:1581-1583`, `pagecache.c:296-317`).
* Fill `static block_device_t ahci_device` (read/write/sector_size/sector_count/drvdata) and publish `block0 = &ahci_device`, mirroring `ata_init` (`ata.c:328-333`).
* The probe seam. `block0` is currently defined in `ata.c:64`; two publishers need it owned by the layer, so move the `block_device_t *block0 = NULL;` definition to `block.c` (it owns `block.h`'s contract) and leave both drivers assigning it. This is the one block-layer edit AHCI forces. Add a thin `block_probe()` (or `disk_init()`) in `block.c`, called from all three sites (`kernel.c:1495`, and the two debug-shell self-tests at `kernel.c:1848`, `kernel.c:1880`) so they boot from the same device that mounts. The selection policy is explicit and default-preserving: without the new exact-token cmdline flag `rach2.disk=ahci` (or `rach2.fsahci`), `block_probe` calls `ata_init()` exactly as today, so every existing `if=ide` image is byte-identical; with the flag it calls `ahci_init()`, falling back to `ata_init()` only if no AHCI HBA is found. `kcmdline_has` is whole-token (`cmdline.c:24-58`), so `rach2.disk=ahci` never collides with `rach2.fsdisk`. The `ata_init` SATA-signature refusal (`ata.c:300-305`) already means an AHCI-only disk is invisible to ATA, so the two never fight over one image. Once AHCI is proven, the default probe order can be revisited.
* Above `block0`: zero changes. `init_krffs_disk`'s MBR probe (`kernel.c:1514-1537`), `block_partition_init` (`block.c:51-76`, backing-agnostic, inherits sector size), `krffs_disk_read`/`write` (`kernel.c:1462-1474`), `pc_fault`/`pc_evict`/`pc_sync`, and the three mount modes (`kernel.c:2530-2567`) are untouched.

Hard parts: the `block0` definition move must not change the ATA path's behavior; verify the ATA disk targets stay green after the move. Multi-sector correctness for the write path under CR0.WP (must stay set, the milestone-8 silent-corruption bug; AHCI touches no page-table protection, but the write path must not assume WP off).

Dependencies: Phase 2.

Gate: with `rach2.disk=ahci`, `disk` and `disk write` round-trip through `block0` over AHCI; the `BLOCKPROBE OK`-style token (Phase 6 harness) passes. Without the flag, the ATA path is byte-identical to before.

### Phase 4: mount KRFFS from the AHCI disk + the page-cache window over AHCI

Scope: the payoff. Mount KRFFS on the AHCI disk through the unchanged MBR root-probe and demand-paged window.

Work:

* `init_krffs_disk` (`kernel.c:1485-1586`) reuses unchanged: through `block_probe()` it reads the MBR via `block0->read`, scans the 4 entries for a KRFFS root (`kernel.c:1514-1537`), bounds-checks against `block0->sector_count`, builds the partition view, and `pc_set_backing(krffs_disk_read, krffs_disk_write, part_bytes)` (`kernel.c:1567`). The live fault chain `pc_fault -> pc_read -> partition.read -> block0->read` now lands in `ahci_read` instead of `ata_read`, with no change to any link.
* Verify the ABAR MMIO does not collide with or clamp the window. `pc_probe_window_size` (`pagecache.c:427`) shrinks the window below the lowest PCI memory BAR in `[PCWIN_BASE, PCWIN_BASE + PC_WINDOW_CAP)` (base 0xC0000000, cap 256 MiB, `pagecache.h:16`, `pagecache.c:38`). A second MMIO BAR (the ABAR) now lives in the hole; confirm it does not land inside the window span and that the sizing still clears the lowest BAR. On QEMU `pc`, check where ich9-ahci's ABAR maps and assert the window survives.
* The KRFFS dirty-flag write-through (`kernel.c:1581-1583`, `KRFFS_MODE_DIRTY` + `pc_sync`) and the fsck contract sit above the block layer and are unchanged.

Hard parts: a high ABAR clamping the usable window (a regression that would silently shrink the FS). Mitigation: a boot diagnostic printing the chosen window size; the gate checks it is unchanged from the ATA boot.

Dependencies: Phase 3.

Gate: boot `rach2.disk=ahci rach2.fsdisk`, mount KRFFS from the AHCI disk, read and write files across page faults and evictions, `pc_sync` and remount cleanly. Window size unchanged from the ATA mount.

### Phase 5: QEMU topology + run/test/disk targets for AHCI

Scope: make the AHCI path runnable and testable from the scripts, strictly additively. The existing `if=ide` paths stay for the ATA driver.

Work:

* QEMU attach: `-device ich9-ahci,id=ahci0 -drive if=none,id=drv0,file=...,format=raw -device ide-hd,drive=drv0,bus=ahci0.0`. This rides the existing default `pc` (i440FX) machine, which carries a PCI bus, so `ich9-ahci` attaches without switching to q35 (q35 would reshuffle IRQ routing, PCI topology, and the firmware path for no benefit). `ich9-ahci` (alias `ahci`) is confirmed present in the host `qemu-system-i386`. Do not add a `-machine` flag.
* `harness.py` wires exactly one `--device` slot to the NIC (`harness.py:44`, `harness.py:64`) and accepts repeatable `--drive` (`harness.py:42-43`). Add a repeatable `--device`/`--qemu-arg` passthrough (more reusable than a narrow `--ahci` flag) so a test can add the two AHCI `-device` lines without disturbing the fixed `-device rtl8139`. Keep the pass/fail contract (the serial "Process exited with status N" line plus `--ok PATTERN`, `harness.py:185-212`) unchanged.
* New Makefile targets paralleling `test-disk`/`persist-test`/`stress-test` (`Makefile:189-231`), e.g. `test-disk-ahci`, reusing the same `$(MKDISK) ... 64 48` image (64 MB disk, 48 MB KRFFS at LBA 2048; the geometry is fixed and duplicated, `crash_test.sh:23-24` hardcodes the matching offset, so the AHCI test must use the same), the same `rach2.fsdisk rach2.rescue` append plus `rach2.disk=ahci`, and the same `--ok "BLOCKPROBE OK"` token from the unchanged probes.
* `run.sh` (`run.sh:63-71`): add an opt-in selector (`DISKBUS=ahci ./run.sh` reusing the existing `DISK=<file>`, keeping one disk variable) that swaps the single `if=ide` `-drive` for the ich9-ahci + ide-hd + `if=none` trio and appends both `rach2.disk=ahci` (select the AHCI backend) and `rach2.fsdisk` (mount KRFFS from the disk). The two are orthogonal: the backend selector alone boots but does not mount. `qemu_devices.sh` is not in the disk path and stays untouched.
* CPU: `harness.py` passes no `-cpu`, so the disk probes run on QEMU's default CPU. AHCI needs no special CPU, so this is fine; note it only if an AHCI probe ever touches entropy.

Hard parts: `harness.py`'s single-device assumption. Mitigation: the passthrough is small and additive; the NIC device stays wired as-is.

Dependencies: Phase 4.

Gate: `make test-disk-ahci` passes `BLOCKPROBE OK`; `DISKBUS=ahci ./run.sh <image>` boots and mounts KRFFS over AHCI. The ATA `test-disk`/`persist-test`/`stress-test` stay green unchanged.

### Phase 6: capstones (SSH, Doom, GRUB-from-disk) and durability over AHCI

Scope: prove the full system on the AHCI image, and prove durability across boots.

Work:

* Persistence: a two-boot `persist-test-ahci` (write on boot one, read back on boot two from the same image, `Makefile:203-212` pattern) and a `stress-test-ahci`, confirming the dirty flag + `pc_sync` write-back path lands real sectors through `ahci_write`.
* The SSH capstone (`ssh -i krffs/rach2_demo_ed25519 -p 2222 root@localhost`) and Doom, run with the root FS mounted from the AHCI disk, to prove the page-cache window and KRFFS work under real userland load over AHCI.
* GRUB boot: the BIOS/GRUB stage uses INT13 and is unrelated to the in-kernel block driver. SeaBIOS can boot from ich9-ahci, but `mkdisk_grub.sh`/`run_from_disk.sh` hardcode `if=ide` and the GRUB chain is orthogonal. Decide: either keep the boot disk on legacy IDE (GRUB + BIOS) and attach the AHCI disk as the post-boot KRFFS data disk, or prove a full ich9-ahci BIOS boot. Recommend the former for the milestone (smaller, the kernel block driver is what is under test), and document it.
* `crash-test` coverage of AHCI is optional this milestone; if wanted, `crash_test.sh`'s inline `qemu_disk()` (`crash_test.sh:37-41`) grows an AHCI variant.
* Documentation of the design choices, for future reference. Add `sys/block/Readme.md` (the block layer has no Readme yet, though CLAUDE.md asks for one per component) covering the block-device interface and its two drivers (ATA PIO and AHCI behind one `block0`, the SB16-then-HDA parallel), and recording the polled-completion decision in full: why the I/O path polls (the IF=0 page-fault model that forbids sleeping), the polled-vs-IRQ-vs-async trade-off, and that IRQ-driven completion is a deliberate deferral, not an oversight, because it would force the fault path to become sleepable and KRFFS to take a real lock. The `ahci.c` file header carries the one-paragraph why-polled rationale at the point of use, and the retired milestone `Readme.md` records the decision too. This is the appropriate home so a future contributor weighing the async path finds the reasoning where the driver lives, not only in this plan.

Hard parts: the GRUB/BIOS-vs-AHCI boot path (the open question every storage map raises). Mitigation: scope GRUB boot to legacy IDE, AHCI to the in-kernel-driven data/root disk reached via `-kernel`/harness, and state it.

Dependencies: Phase 5.

Gate: SSH capstone `rc=0` over AHCI, Doom runs, persist/stress pass across reboots, fsck clean after a clean unmount.

## Verification

Global gate, every phase and at milestone close:

* `make verify-style` 8/8 and `make verify-host` green.
* The module-path `make test` 28/28 unchanged.
* A real boot, the SSH capstone (`ssh -i krffs/rach2_demo_ed25519 -p 2222 root@localhost` to an interactive busybox ash, `rc=0`), and Doom.
* The AHCI disk boots and probes: `make test-disk-ahci` (and persist/stress over AHCI) pass the same `BLOCKPROBE OK`-style tokens through the AHCI HBA.
* The vendored musl/BusyBox/Dropbear trees stay untouched and out of the diff.
* The default RAM-module boot stays byte-identical: AHCI only adds a second producer of `block0` selected behind a cmdline flag, and the RAM-disk and `if=ide` ATA paths are unchanged. The ATA `test-disk`/`persist-test`/`stress-test` stay green.
* The IF=0 polled invariant holds throughout: no phase adds an IRQ handler, a `sti`, a wait queue, or an allocation on the I/O path; every poll is bounded and returns `-EIO` on expiry.

Substantial work gets a harsh independent Codex review with every finding fixed before commit, including voice. `make check-sources` gates the new `sys/block/ahci.c`. C89/-ansi/-Werror, no libgcc, freestanding; diagnostics lowercase `ahci: detail` to serial, matching `ata.c:335`.

## What stays deferred

* IRQ-driven completion. Polled is confirmed, so the completion IRQ plus the `wq_wait_event` sleeping path stays out. It would force the page-fault path to become sleepable and KRFFS to take a real FS lock (the fault never yields today, so metadata mutations are atomic for free, and legacy PCI IRQ delivery here is unreliable, which is why even the HDA audio driver polls). That is a separate, larger effort, essentially "make the kernel's disk I/O asynchronous", best done as its own milestone after a VFS/locking layer rather than smuggled into the driver bring-up. The trade-off is documented in `sys/block/Readme.md` (Phase 6) for whoever picks it up.
* NCQ. One command slot, polled, is all the page-cache fault path issues.
* Hot-plug and port multipliers. Single boot/data disk, first-present-port only.
* ATAPI. The ATA driver already refuses packet signatures; AHCI scopes to disks.
* Multiple simultaneous disks. There is one `block0` (`block.h:33`); single-disk like the rest of the milestone.
* Large-disk capacity beyond the KRFFS/window bounds. LBA48 itself is in scope (READ/WRITE DMA EXT and IDENTIFY words 100-103 are LBA48), and AHCI advertises the full count, but the 4 GiB KRFFS logical cap and the 256 MiB window cap (`PC_WINDOW_CAP`) bound usable size; no new overflow path opens.
* Multi-PRDT / page-straddle robustness. A single-entry PRDT over an identity-mapped, page-aligned buffer covers every milestone-10 caller; a future non-page-aligned caller would need a per-page PRDT split or a bounce buffer.
* q35 and a full COMRESET/staggered-spinup handshake. The lighter per-port stop/program/start suffices for QEMU ich9-ahci; the deeper real-hardware handshake is deferred.
* 4Kn sectors. Clamped to 512-byte logical sectors.

This mirrors the audio progression exactly: AHCI is to ATA PIO what HDA is to the PC-speaker and SB16. The readable bring-up driver stays as the documented fallback sibling behind one block interface, and the modern standard goes in beside it, reusing the same proven DMA/MMIO/PCI idioms (`hda.c` BAR map, zero-upper address pairs, static aligned DMA arrays, bounded MMIO polling) with storage registers swapped for audio ones. The plan supersedes the AHCI deferral in `08-disk/plan.md:530-531`: the polled-completion realization closes the exact gap that note left open.
