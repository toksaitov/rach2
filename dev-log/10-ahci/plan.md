# Milestone 10: AHCI, the modern SATA half of the block interface

Status: complete. The driver, the driver-selection point, the test targets, and the end-to-end AHCI tests were completed. See `Readme.md` for the build record.

## The goal

Add an AHCI/SATA disk driver behind the block interface from the disk milestone, so KRFFS can use a SATA HBA through the interface already used by legacy ATA. This follows the audio-driver progression: a simple implementation came first, followed by the modern standard used by most hardware. HDA serves that role for audio, and AHCI serves it for storage. The disk milestone deliberately deferred AHCI; this milestone implements it and supersedes that note.

AHCI does not change the interfaces above `block0`. The page cache, demand-paged window, MBR root probe, `block_partition_init`, KRFFS, dirty flag, and fsck use only `block0->read` and `block0->write` (`block.h:20-28`). They do not depend on which driver published `block0`. This milestone adds a second producer of `block0` and changes nothing above it. The readable ATA PIO driver remains the fallback, as the PC speaker and SB16 remain available beside HDA.

## Scope and non-goals

In scope:

* A new `sys/block/ahci.c` + `sys/block/ahci.h` exposing `int ahci_init(void)`, mirroring `ata.c`/`ata.h` (`ata.c:272-338`, `ata.h:13`).
* PCI discovery of an AHCI HBA (class 0x01, subclass 0x06, prog-if 0x01), ABAR (BAR5) map, HBA + first-present-port bring-up, a single polled command slot, READ DMA EXT (0x25) / WRITE DMA EXT (0x35) over a command list / received-FIS / PRDT.
* Polled completion only: spin on `PxCI` clear with `PxIS`/`PxTFD` error checks and a bounded watchdog, callable from `pc_fault` at IF=0.
* A driver-selection point so the boot path selects AHCI when present, else ATA.
* QEMU `ich9-ahci` test topology on the existing `pc` machine, additive run/test/disk targets.
* A KRFFS mount and page-cache window over the AHCI disk, the disk probes over AHCI, and completed SSH, Doom, and GRUB-boot end-to-end tests on the AHCI image.

Non-goals (see "What stays deferred"): NCQ, IRQ-driven completion, hot-plug, port multipliers, ATAPI, multiple simultaneous disks, q35, sector sizes other than 512, and any change to the IF=0 polled-fault model.

## Design decision (confirm before code)

This decision controls whether the fault path changes. Confirm it before writing code.

The constraint is fixed in the code. The page-cache window fault is delivered through a 32-bit interrupt gate (vector 14, gate flag 0x8E, `idt.c:46`); the exception stub does `cli` (`isr_handler.s:17`) and the router never re-enables IF on the fault path (only the syscall path does, `isr_handler.s:89-93`). Therefore, `_page_fault_handler` (`paging.c:391`) and its window branch to `pc_fault` (`paging.c:434-444`, comment "The poll-only fill is safe with IF=0") run with IF=0 throughout and cannot sleep. The only sleep primitive, `wq_wait_event`, would execute `sti; hlt` during the exception. The block contract states that `read` and `write` "are synchronous and polled: a caller may invoke them with interrupts off and without the scheduler running" (`block.h:11-18`). The ATA driver meets this contract with a bounded iteration watchdog rather than a timer, because "with interrupts off there is no timer to bound by" (`ATA_SPIN_LIMIT = 10000000`, `ata.c:49-52`), and it masks its IRQ with nIEN (`ata.c:279-281`).

AHCI is normally DMA + completion-IRQ driven. The tension: a completion IRQ is exactly what the fault path cannot service.

The two ways out:

* Polled completion (recommended). Mask the HBA interrupt (leave `GHC.IE` clear, do not set `PxIE`). Issue a command by setting the slot's bit in `PxCI`, then busy-poll `PxCI` until that bit clears. Check `PxIS` (task-file-error `TFES`) and `PxTFD` (`STS.ERR`/`BSY`) for failure, with the same bounded-iteration watchdog pattern as ATA. The HBA performs the DMA in hardware; the CPU spins only on a memory-mapped status bit. HDA already uses this polling pattern for the GCTL/SRST/ICI/LPIB-advance loops (`hda.c:806-855`, `hda.c:662-691`, `hda.c:946-998`, `hda.c:2686-2714`), none of which sleep or use a wait queue. `ahci_read` and `ahci_write` then satisfy the `block.h:11-18` contract and fit into `pc_fault -> pc_read -> block_partition_read -> block0->read` (`pagecache.c:281`, `kernel.c:1462`, `block.c:23`) without changing the fault model, page cache, partition layer, or `kernel.c` mount flow. Compared with ATA, initialization also requires a small contiguous-DMA setup: the per-port command list (1 KiB aligned), received-FIS area (256 B aligned), and one command table with its PRDT (128 B aligned).

* IRQ-driven completion (not recommended). Enable `PxIE`/`GHC.IE`, take the AHCI completion IRQ, and wake the faulting requester through `wq_wait_event` on `PxIS`. This requires a sleepable fault path, which the current interrupt-gate design (`idt.c:46`, `isr_handler.s:17`) forbids. It would require changes to `pc_fault`, the window branch in `paging.c`, and the no-sleep concurrency invariant used by the disk milestone. A fault that never yields keeps KRFFS metadata mutations atomic without an FS-wide lock (`08-disk/plan.md:22-58`). This work provides no benefit for a teaching OS with a small working set.

Recommendation: keep the IF=0 polled-fault model unchanged and complete AHCI commands by polling `PxCI`, `PxIS`, and `PxTFD`. A cache miss stalls the machine for one 4 KiB transfer. The HBA performs DMA instead of the 256 `insw` operations per sector used by PIO, so the stall is shorter with AHCI. The bounded watchdog returns `-EIO` on a wedged port rather than hanging, after which the fault handler panics as it does for ATA. This supersedes `08-disk/plan.md:530-531`, which deferred AHCI because "AHCI needs the completion IRQ the fault cannot service". The HBA does not require service from an IRQ when the CPU polls the same completion bit.

This choice is confirmed. The phases below build the polled path, and Phase 6 records the decision and the polled-vs-IRQ-vs-async trade-off in a durable place a future kernel contributor would consult (a new `sys/block/Readme.md` and the `ahci.c` file header), so weighing IRQ completion later starts from the analysis rather than rediscovering it.

## The phases, and why they are ordered this way

Each phase ends with a serial token or clean boot, so a regression can be isolated to one phase. The IF=0 polling invariant is required from Phase 2 onward: no phase introduces an IRQ, a `sti`, a wait queue, or an allocation on the I/O path.

### Phase 0: PCI discovery, ABAR map, and an `ahci` debug command (no I/O)

Scope: verify that the HBA is found and its registers are readable, without issuing a command or using DMA. This corresponds to an IDENTIFY check for controller discovery.

Work:

* Add `PCI_CLASS_MASS_STORAGE 0x01`, `PCI_SUBCLASS_SATA 0x06` next to the existing multimedia/audio constants (`pci.h:41-42`). No prog-if-aware finder exists and `pci_find_device`/`pci_find_device_skip` match class+subclass only (`pci.c:141`, `pci.c:191`), so the driver filters on `dev.prog_if == 0x01` itself (already captured, `pci.c:121`). Use `pci_find_device_skip` to walk past a controller in IDE-compat/RAID mode, exactly as `hda_init` walks audio controllers (`hda.c:1569`).
* Recover the ABAR from BAR5, not BAR0: `abar = pci_bar_address(dev.bar[5])` (`PCI_BAR5 = 0x24`, `pci.h:34`; mask strips type bits, `pci.c:345`), first confirming `dev.bar[5]` is a memory BAR (a `pci_bar_is_memory` guard, against a pathological 64-bit BAR4 extending into BAR5). If `abar >= HARD_MEMORY_LIMIT` (0x2000000, `paging.h:16`) call `map_mmio(abar, size)` (`paging.c:471`), which identity-maps cache-disabled (PCD|PWT, `paging.c:501`, mandatory so status-bit polls are not stale) and returns the phys address, 0 on failure. This is the `hda_init` BAR idiom (`hda.c:1604-1611`) with BAR5 substituted for BAR0. Map size: a fixed 0x2000 over-covers the ~0x1100 + 0x80-per-port span.
* Copy `hda_read32`/`hda_write32` as `ahci_read32`/`ahci_write32` over `base + offset` (`hda.c:609-627`), plus a per-port offset helper `0x100 + port*0x80` analogous to `hda_sd_offset` (`hda.c:630`). MMIO is `volatile`.
* `pci_enable_memory_space` + `pci_enable_bus_master` (`pci.c:307-324`); bus-master is mandatory before any HBA DMA even in polled mode (`hda.c:2713-2714` names PCI BM as the cause of a silent no-DMA). Skip `pci_enable_interrupts` (polled). Optionally force D0 (`hda_pci_force_d0`, `hda.c:765-797`).
* Add an `ahci` debug command beside the existing `audio` HDA diagnostic (`kernel.c:1822`) that calls a discovery-only entry and dumps CAP, GHC, PI (implemented-port bitmask), and per-port `PxSSTS`/`PxSIG`/`PxTFD`. No `block0` change yet.
* Build: add `sys/block/ahci.c` to `C_SRC` right after `sys/block/ata.c` (`Makefile:30`); `make check-sources` (`Makefile:138-140`) fails until it is listed, and `-MMD` deps (`Makefile:133`) pick up the new header.

Risks and responses: ABAR is BAR5, which is easy to confuse with the BAR0 used by HDA. The HBA may report prog-if != 0x01 under an IDE-compatible QEMU model; filter and skip it. The dump command reports the HBA values before I/O depends on them.

Dependencies: none beyond the existing PCI layer (which self-scans; `init_pci` at `pci.c:366` is debug-only and never called, do not start calling it).

Verification: boot with the AHCI device attached (Phase 6 topology, brought forward as a manual run), run `ahci` at the rescue shell, and observe CAP/GHC/PI and a port with `PxSSTS.DET == 3`. The default RAM boot remains unchanged.

### Phase 1: HBA + first-present-port bring-up (DMA structures, no transfer)

Scope: take ownership, enable AHCI mode, reset, select the boot port, allocate and program the DMA structures, start the port engine. Still no command issued.

Work:

* BIOS/OS handoff: if `CAP2.BOH` is set, request through `BOHC.OOS` and poll until `BOHC.BOS` clears, with a bound. Check the capability and skip the handoff when it is clear, matching HDA's "no-op when capability absent" (`hda.c:765-797`); QEMU ich9-ahci typically has none.
* `GHC.AE` (bit 31) to present AHCI registers; `GHC.HR` (bit 0) HBA reset, poll clear (bounded, the `ata_wait`/`hda_reset` edge-poll idiom, `ata.c:82-93`, `hda.c:816-836`), re-assert `GHC.AE` (HR clears it).
* Enumerate `PI`; pick the lowest implemented port with a device present (`PxSSTS.DET == 3`, `IPM == 1`, `PxTFD` not BSY/DRQ). Choose the first present port rather than assuming port 0. This supports hardware variation while retaining the milestone's single-disk limit (ATA is primary-master only).
* Per-port stop-before-program (AHCI 10.1.2): clear `PxCMD.ST` then `PxCMD.FRE`, poll `PxCMD.CR` and `PxCMD.FR` clear (bounded).
* Allocate the DMA structures as static `volatile __attribute__((aligned(N)))` BSS arrays, following the existing HDA and SB16 pattern (`hda.c:42`, `hda.c:77-93`, `sb16.c:36`): command list `aligned(1024)` (32 headers x 32 B), received-FIS `aligned(256)`, command table `aligned(128)`. They are placed in `.bss` below `k_phys_mem_end`, hence below `HARD_MEMORY_LIMIT` (32 MiB), and are identity-mapped below 4 GiB. Therefore, `(uint32_t)(uintptr_t)arr` gives the physical DMA address without translation. The toolchain already honors these alignments (`bdl` is `aligned(1024)`, `hda.c:92`). Packed descriptor structs follow `hda_bdl_entry_t` (`hda.h:185-190`): command header (32 B), command table (128 B command-FIS area + PRDT), and PRDT entry (DBA/DBAU/reserved/DBC-with-I-bit).
* Program `PxCLB = (uint32_t)(uintptr_t)cmdlist`, `PxCLBU = 0`; `PxFB = fis`, `PxFBU = 0` (the zero-upper idiom, `hda.c:2398`). Clear `PxSERR` (write 1s, write-1-to-clear like `HDA_SD_STS`) and `PxIS`. Set `PxCMD.FRE`, then `PxCMD.ST`.
* Extend the `ahci` dump to show the programmed bases and `PxCMD` engine state.

Risks and responses: SATA link initialization can be slower under QEMU with IF=0. IF is still enabled during initialization, but the watchdog must allow enough time. The reset and spin handshake must remain bounded because a wedged port would otherwise hang before the scheduler starts. Give every transition a bounded countdown that returns failure, as ATA and HDA do.

Dependencies: Phase 0.

Verification: `ahci` shows the engine started (`PxCMD.ST`/`FRE`/`CR`/`FR` set), bases programmed, and no error in `PxTFD`. `block0` and the default boot remain unchanged.

### Phase 2: a single polled READ DMA EXT of sector 0, validated against the MBR

Scope: perform one READ DMA EXT for LBA 0 into a static identity-mapped buffer, poll to completion, and validate the known MBR 55AA signature. This verifies the complete DMA and polling path before connecting it to `block0`.

Work:

* IDENTIFY DEVICE (0xEC) to size the device from words 100..103 (48-bit count), replacing `ata_init`'s IDENTIFY (`ata.c:283-322`). AHCI uses LBA48, so the LBA28 cap (`ATA_LBA28_MAX_SECTORS`, `ata.c:46-47`) does not apply; advertise the full count (the window cap clamps usable size regardless). Clamp logical sector size to 512 (assert; `BLOCK_SECTOR_SIZE`, `block.h:7`; KRFFS sector math assumes 512).
* Build command header[0]: `CFL = 5` dwords (H2D FIS), `W = 0` (read), `PRDTL = 1`, `CTBA = cmdtable`, `CTBAU = 0`. Build a Register H2D FIS in the command table (type 0x27, C bit, command 0x25, LBA spread across lba0..5, count, device 0x40 for LBA mode). One PRDT entry: `DBA = buffer`, `DBAU = 0`, `DBC = bytecount - 1`.
* Issue: first poll `PxTFD` until `STS.BSY == 0` and `STS.DRQ == 0` (AHCI requires the task file idle before a command issues), bounded, then set `PxCI` bit 0. Poll `PxCI` clear with the `ATA_SPIN_LIMIT`-shaped bounded watchdog (`ata.c:52`); on each spin check `PxIS.TFES` and `PxTFD.STS.ERR` (bit 0) for failure, return `-EIO` on error or expiry, clear `PxIS`/`PxSERR` (write-1-to-clear) after.
* Cache coherency: add an `ahci_dma_sync_range` equivalent of `hda_dma_sync_range` (`hda.c:244-268`, CLFLUSH + barrier, WBINVD fallback, CLFLUSH probed via CPUID), flush the command table and PRDT before issue, and invalidate the buffer after completion. This is a no-op under QEMU but is required for hardware correctness.
* A `disk` debug-path variant or a one-shot self-test that reads LBA 0 via the AHCI path and prints the signature, run from the rescue shell.

Risks and responses: FIS and PRDT field layouts are easy to implement incorrectly. A single PRDT entry requires a physically contiguous data buffer. Under the identity map, every address below `HARD_MEMORY_LIMIT` is contiguous (phys == virt, `paging.c:140-175`), so any static or identity-mapped buffer there fits one PRDT entry regardless of page alignment. This includes the static self-test buffer, runtime page-cache frames (one page from `alloc_kernel_frame`, `pagecache.c:488`, `paging.c:525`), and the `kernel.c` MBR and partition buffers (`kernel.c:1487-1488`). `pc_host_pages` is the host-test backing (`pagecache.c:3-13`), not a runtime path. The command list, received-FIS, and command-table structures receive their AHCI alignment from `__attribute__((aligned))` as in Phase 1.

Dependencies: Phase 1.

Verification: the self-test reads LBA 0 over AHCI and reports the MBR 55AA signature that the ATA path reads from the same image. A deliberately wedged read verifies the polling bound by returning `-EIO` instead of hanging.

### Phase 3: the full block_device backend behind block0, with driver selection

Scope: extend the verified single read into the full read/write backend, publish `block0`, and add the probe that selects AHCI when present and ATA otherwise.

Work:

* `ahci_read`/`ahci_write` with the exact `block_device_t` signature (`block.h:20-28`): 512 sector size, the IDENTIFY count, polled completion. Multi-sector: the page cache issues exactly `PAGE_SIZE/BLOCK_SECTOR_SIZE = 8` sectors per call (`kernel.c:1465-1466`), one 4 KiB page = one PRDT entry, so a single-entry PRDT covers every exercised call. Bounds-check `count == 0`, `lba` past device, run-past device, replicating ATA's guards (`ata.c:183-186`, `ata.c:231-234`). WRITE DMA EXT (0x35) sets the `W` bit and the write command in the H2D FIS, and each write is followed by a polled FLUSH CACHE EXT (0xEA) so the data reaches durable media, matching the ATA driver's post-write flush (`ata.c:259-262`) that the dirty-flag and `pc_sync` durability contract relies on (`kernel.c:1581-1583`, `pagecache.c:296-317`).
* Fill `static block_device_t ahci_device` (read/write/sector_size/sector_count/drvdata) and publish `block0 = &ahci_device`, mirroring `ata_init` (`ata.c:328-333`).
* Driver selection. `block0` is currently defined in `ata.c:64`; two publishers require ownership by the block layer, so move the `block_device_t *block0 = NULL;` definition to `block.c`, which owns the `block.h` contract, and leave both drivers assigning it. This is the only block-layer edit required by AHCI. Add a thin `block_probe()` (or `disk_init()`) in `block.c`, called from all three sites (`kernel.c:1495`, and the two debug-shell self-tests at `kernel.c:1848`, `kernel.c:1880`) so they boot from the same device that mounts. The explicit policy preserves the default. Without the new exact-token command-line flag `rach2.disk=ahci` (or `rach2.fsahci`), `block_probe` calls `ata_init()` as before, so every existing `if=ide` image is byte-identical. With the flag, it calls `ahci_init()` and falls back to `ata_init()` only when no AHCI HBA is found. `kcmdline_has` matches whole tokens (`cmdline.c:24-58`), so `rach2.disk=ahci` never collides with `rach2.fsdisk`. The SATA-signature refusal in `ata_init` (`ata.c:300-305`) already makes an AHCI-only disk invisible to ATA, so the drivers do not compete for one image. The default probe order can be reconsidered after AHCI is verified.
* Above `block0`: zero changes. `init_krffs_disk`'s MBR probe (`kernel.c:1514-1537`), `block_partition_init` (`block.c:51-76`, backing-agnostic, inherits sector size), `krffs_disk_read`/`write` (`kernel.c:1462-1474`), `pc_fault`/`pc_evict`/`pc_sync`, and the three mount modes (`kernel.c:2530-2567`) are untouched.

Risks and responses: moving the `block0` definition must not change ATA behavior, so rerun the ATA disk targets after the move. Multi-sector writes must remain correct with CR0.WP set. Milestone 8 found silent corruption when it was not set; AHCI changes no page-table protection, but its write path must not assume WP is clear.

Dependencies: Phase 2.

Verification: with `rach2.disk=ahci`, `disk` and `disk write` make a round trip through `block0` over AHCI; the `BLOCKPROBE OK`-style token from the Phase 6 harness passes. Without the flag, the ATA path is byte-identical to its previous form.

### Phase 4: mount KRFFS from the AHCI disk + the page-cache window over AHCI

Scope: mount KRFFS on the AHCI disk through the unchanged MBR root probe and demand-paged window.

Work:

* `init_krffs_disk` (`kernel.c:1485-1586`) remains unchanged. Through `block_probe()` it reads the MBR through `block0->read`, scans the 4 entries for a KRFFS root (`kernel.c:1514-1537`), checks bounds against `block0->sector_count`, builds the partition view, and calls `pc_set_backing(krffs_disk_read, krffs_disk_write, part_bytes)` (`kernel.c:1567`). The fault chain `pc_fault -> pc_read -> partition.read -> block0->read` now reaches `ahci_read` instead of `ata_read`, without changing any link.
* Verify the ABAR MMIO does not collide with or clamp the window. `pc_probe_window_size` (`pagecache.c:427`) shrinks the window below the lowest PCI memory BAR in `[PCWIN_BASE, PCWIN_BASE + PC_WINDOW_CAP)` (base 0xC0000000, cap 256 MiB, `pagecache.h:16`, `pagecache.c:38`). A second MMIO BAR (the ABAR) now lives in the hole; confirm it does not land inside the window span and that the sizing still clears the lowest BAR. On QEMU `pc`, check where ich9-ahci's ABAR maps and assert the window survives.
* The KRFFS dirty-flag write-through (`kernel.c:1581-1583`, `KRFFS_MODE_DIRTY` + `pc_sync`) and the fsck contract sit above the block layer and are unchanged.

Risks and responses: a high ABAR could clamp the usable window and silently shrink the file system. Print the selected window size during boot and verify that it matches the ATA boot.

Dependencies: Phase 3.

Verification: boot `rach2.disk=ahci rach2.fsdisk`, mount KRFFS from the AHCI disk, read and write files across page faults and evictions, run `pc_sync`, and remount cleanly. The window size remains unchanged from the ATA mount.

### Phase 5: QEMU topology + run/test/disk targets for AHCI

Scope: make the AHCI path runnable and testable from the scripts, strictly additively. The existing `if=ide` paths stay for the ATA driver.

Work:

* QEMU attachment: `-device ich9-ahci,id=ahci0 -drive if=none,id=drv0,file=...,format=raw -device ide-hd,drive=drv0,bus=ahci0.0`. The existing default `pc` (i440FX) machine has a PCI bus, so `ich9-ahci` attaches without switching to q35. A q35 switch would change IRQ routing, PCI topology, and the firmware path without helping this work. The host `qemu-system-i386` includes `ich9-ahci` (alias `ahci`). Do not add a `-machine` flag.
* `harness.py` wires exactly one `--device` slot to the NIC (`harness.py:44`, `harness.py:64`) and accepts repeatable `--drive` (`harness.py:42-43`). Add a repeatable `--device`/`--qemu-arg` passthrough (more reusable than a narrow `--ahci` flag) so a test can add the two AHCI `-device` lines without disturbing the fixed `-device rtl8139`. Keep the pass/fail contract (the serial "Process exited with status N" line plus `--ok PATTERN`, `harness.py:185-212`) unchanged.
* Add Makefile targets corresponding to `test-disk`, `persist-test`, and `stress-test` (`Makefile:189-231`), such as `test-disk-ahci`. Reuse the same `$(MKDISK) ... 64 48` image: a 64 MB disk with 48 MB of KRFFS at LBA 2048. The geometry is fixed and duplicated, and `crash_test.sh:23-24` has the matching offset, so the AHCI test must use the same values. Append `rach2.fsdisk rach2.rescue` plus `rach2.disk=ahci`, and retain the `--ok "BLOCKPROBE OK"` token from the unchanged probes.
* In `run.sh` (`run.sh:63-71`), add an opt-in selector (`DISKBUS=ahci ./run.sh`) that reuses the existing `DISK=<file>` and retains one disk variable. It replaces the single `if=ide` `-drive` with the ich9-ahci, ide-hd, and `if=none` arguments. It appends both `rach2.disk=ahci`, which selects the AHCI backend, and `rach2.fsdisk`, which mounts KRFFS from the disk. These options are independent: selecting the backend alone boots without mounting. `qemu_devices.sh` is not part of the disk path and remains unchanged.
* CPU: `harness.py` passes no `-cpu`, so the disk probes run on QEMU's default CPU. AHCI needs no special CPU, so this is fine; note it only if an AHCI probe ever touches entropy.

Risks and responses: `harness.py` assumes one device argument. Add a small passthrough while retaining the existing NIC device handling.

Dependencies: Phase 4.

Verification: `make test-disk-ahci` passes `BLOCKPROBE OK`; `DISKBUS=ahci ./run.sh <image>` boots and mounts KRFFS over AHCI. The unchanged ATA `test-disk`, `persist-test`, and `stress-test` targets continue to pass.

### Phase 6: end-to-end SSH, Doom, GRUB-from-disk, and durability tests over AHCI

Scope: verify the full system on the AHCI image and its durability across boots.

Work:

* Persistence: add a two-boot `persist-test-ahci` (write on boot one, read back on boot two from the same image, following the `Makefile:203-212` pattern) and a `stress-test-ahci`. These confirm that the dirty flag and `pc_sync` write-back path write disk sectors through `ahci_write`.
* Run SSH (`ssh -i krffs/rach2_demo_ed25519 -p 2222 root@localhost`) and Doom with the root file system mounted from the AHCI disk. This verifies the page-cache window and KRFFS under a userland workload over AHCI.
* GRUB boot: the BIOS/GRUB stage uses INT13 and is unrelated to the in-kernel block driver. SeaBIOS can boot from ich9-ahci, but `mkdisk_grub.sh`/`run_from_disk.sh` hardcode `if=ide` and the GRUB chain is orthogonal. Decide: either keep the boot disk on legacy IDE (GRUB + BIOS) and attach the AHCI disk as the post-boot KRFFS data disk, or prove a full ich9-ahci BIOS boot. Recommend the former for the milestone (smaller, the kernel block driver is what is under test), and document it.
* `crash-test` coverage of AHCI is optional this milestone; if wanted, `crash_test.sh`'s inline `qemu_disk()` (`crash_test.sh:37-41`) grows an AHCI variant.
* Document the design choices for future work. Add `sys/block/Readme.md` (the block layer has no Readme yet, although CLAUDE.md requires one per component) for the block-device interface and its two drivers: ATA PIO and AHCI behind one `block0`, corresponding to the SB16-then-HDA progression. Record the polled-completion decision in full: the I/O path polls because the IF=0 page-fault model forbids sleeping; IRQ-driven and asynchronous completion remain deferred because they would require a sleepable fault path and a KRFFS lock. The `ahci.c` file header states the reason for polling at the point of use, and the completed milestone `Readme.md` also records the decision. A contributor considering asynchronous I/O can then find the reason in the driver documentation as well as this plan.

Risks and responses: distinguish the GRUB and BIOS boot path from the AHCI path. Test GRUB boot through legacy IDE, and test AHCI as the in-kernel data or root disk reached through `-kernel` or the harness. State this distinction.

Dependencies: Phase 5.

Verification: SSH returns `rc=0` over AHCI, Doom runs, persistence and stress tests pass across reboots, and fsck reports a clean file system after a clean unmount.

## Verification

Run these checks after each applicable phase and at milestone completion:

* `make verify-style` passes 8/8, and `make verify-host` passes.
* The module-path `make test` 28/28 unchanged.
* A boot, an SSH session (`ssh -i krffs/rach2_demo_ed25519 -p 2222 root@localhost` to an interactive BusyBox ash, `rc=0`), and Doom.
* The AHCI disk boots and passes its probes. `make test-disk-ahci`, persistence, and stress tests over AHCI pass the same `BLOCKPROBE OK`-style tokens through the AHCI HBA.
* The vendored musl/BusyBox/Dropbear trees stay untouched and out of the diff.
* The default RAM-module boot remains byte-identical. AHCI adds only a second producer of `block0` selected by a command-line flag; the RAM-disk and `if=ide` ATA paths remain unchanged. The ATA `test-disk`, `persist-test`, and `stress-test` targets pass.
* The IF=0 polled invariant holds throughout: no phase adds an IRQ handler, a `sti`, a wait queue, or an allocation on the I/O path; every poll is bounded and returns `-EIO` on expiry.

Substantial work receives a strict independent Codex review, including its prose, and every finding is fixed before commit. `make check-sources` checks the new `sys/block/ahci.c`. The code is freestanding C89 with -ansi, -Werror, and no libgcc. Diagnostics use lowercase `ahci: detail` on the serial console, matching `ata.c:335`.

## What stays deferred

* IRQ-driven completion. Polled completion is confirmed, so the completion IRQ and `wq_wait_event` sleeping path remain excluded. They would require a sleepable page-fault path and a KRFFS file-system lock. The fault does not yield today, so metadata mutations are atomic without an added lock. Legacy PCI IRQ delivery is also unreliable here, which is why even the HDA audio driver polls. Asynchronous disk I/O is a separate, larger effort that belongs in its own milestone after a VFS and locking layer, rather than being included in driver initialization. `sys/block/Readme.md` documents the trade-off in Phase 6 for later work.
* NCQ. One command slot, polled, is all the page-cache fault path issues.
* Hot-plug and port multipliers. Single boot/data disk, first-present-port only.
* ATAPI. The ATA driver already refuses packet signatures; AHCI scopes to disks.
* Multiple simultaneous disks. There is one `block0` (`block.h:33`); single-disk like the rest of the milestone.
* Large-disk capacity beyond the KRFFS/window bounds. LBA48 itself is in scope (READ/WRITE DMA EXT and IDENTIFY words 100-103 are LBA48), and AHCI advertises the full count, but the 4 GiB KRFFS logical cap and the 256 MiB window cap (`PC_WINDOW_CAP`) bound usable size; no new overflow path opens.
* Multi-PRDT / page-straddle robustness. A single-entry PRDT over an identity-mapped, page-aligned buffer covers every milestone-10 caller; a future non-page-aligned caller would need a per-page PRDT split or a bounce buffer.
* q35 and a full COMRESET/staggered-spinup handshake. The smaller per-port stop/program/start sequence suffices for QEMU ich9-ahci; the additional hardware handshake is deferred.
* 4Kn sectors. Clamped to 512-byte logical sectors.

The storage and audio work follow the same progression. AHCI follows ATA PIO as HDA follows the PC speaker and SB16. The readable initial driver remains the documented fallback behind one block interface, while the modern standard uses the existing DMA, MMIO, and PCI patterns: the `hda.c` BAR mapping, zero upper address pairs, static aligned DMA arrays, and bounded MMIO polling. Storage registers replace the audio registers. This plan supersedes the AHCI deferral in `08-disk/plan.md:530-531` by using polled completion for the condition that the earlier note left unresolved.
