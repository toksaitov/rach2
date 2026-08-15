# `block/`: the disk layer and its two drivers

One disk, reached two ways. Every byte of a file system in Rach2 that does not live in the RAM module comes off a real disk through a single device, `block0`, behind a four-call vtable. Two drivers can publish that device: `ata.c`, the readable polled-PIO bring-up driver for the legacy IDE channel, and `ahci.c`, the modern SATA driver that covers most real hardware. They are siblings the way the audio drivers are: the PC speaker and SB16 are to the HD Audio controller what ATA PIO is to AHCI. The readable one shipped first and stays as the documented fallback; the modern one goes in beside it, behind the same interface, and nothing above the interface can tell which is underneath.

## The interface

`block.h` is the whole contract. A `block_device` is a sector size, a sector count, and `read`/`write` that move `count` whole 512-byte sectors to or from a buffer and return `0` or a negative errno. The one published instance is `block0`. Above it sit, in order, the MBR parse (`mbr.c`, a pure four-entry table reader plus the judgement that qualifies a FAT-typed partition 1 as the boot partition, both host-tested in `tests/host/mbr_hosttest.c`), the KRFFS root-probe and `block_partition_init` (a partition is just a `[start_lba, start_lba + count)` view that adds the offset and bounds-checks each request), the demand-paged page cache and its window, KRFFS, the dirty-mount flag, and fsck. None of them name a driver. They call `block0->read` and `block0->write`, so adding AHCI added a second producer of `block0` and changed nothing above it.

The defining clause of the contract is that `read` and `write` "are synchronous and polled: a caller may invoke them with interrupts off and without the scheduler running." That sentence is not a convenience. It is what lets the page-cache window fault use the disk at all.

## Why the I/O path polls

The page-cache window fault is the hot caller, and it runs in a place that cannot sleep. A miss on the demand-paged window at `0xC0000000` is a page fault, delivered through a 32-bit interrupt gate: the CPU clears `IF` on entry and the kernel never re-enables it on the fault path. So `pc_fault` and the `block0->read` beneath it run start to finish with interrupts off. The only sleep primitive, `wq_wait_event`, would `sti; hlt` in the middle of an exception, and a fault that yielded would also break the no-yield invariant the disk milestone leans on (a fault that never sleeps keeps KRFFS metadata mutations atomic without a file-system lock).

A timer cannot bound a wait with interrupts off either, so both drivers bound their waits by counting iterations, not time: a wedged or absent device makes the watchdog expire and the call returns `-EIO`, and the fault handler then panics fail-closed rather than hang. ATA polls its status register; AHCI polls a memory-mapped completion bit. Neither ever waits on an interrupt.

This is why AHCI here is driven by polled completion. AHCI is normally DMA plus a completion interrupt, and the completion interrupt is exactly what the fault path cannot service. The way out is to keep the DMA and drop the interrupt: mask the HBA interrupt (`GHC.IE` and `PxIE` stay clear), set the command's bit in `PxCI`, and busy-poll `PxCI` until the HBA clears it, watching the fatal `PxIS` bits (task-file error, host-bus fatal, host-bus data, interface fatal) and, once the bit clears, `PxTFD` ERR/DF for failure, all under the same bounded-iteration watchdog ATA uses. A fatal `PxIS` error can clear `PxCI` without setting `PxTFD.ERR`, so completion tests both, and then checks the byte count the HBA wrote back into the command header: a completion that moved fewer bytes than requested without raising an error is a failure too, since a short write that later gets marked clean would be silent data loss. The HBA still does the transfer in hardware; the CPU only spins on a status bit in memory. So a 4 KiB miss stalls the machine for one DMA, not for 256 `insw` per sector as PIO does, which makes AHCI the faster of the two on the stall it cannot avoid.

## Polled vs IRQ vs async

Polled completion is a deliberate choice, and the alternative is a deliberate deferral, not an oversight.

* Polled (what this is). The fault path is unchanged: no interrupt, no `sti`, no wait queue, no allocation on the I/O path. The cost is real and accepted: a cache miss stalls the whole machine for one transfer. For a teaching kernel with a small working set this is the right trade, and it is the same idiom the HD Audio driver already uses for the same reason (legacy PCI interrupt delivery here is unreliable, so even audio polls).
* IRQ-driven. Enabling `PxIE`/`GHC.IE`, taking the completion interrupt, and waking the faulting requester on a wait queue would force the fault path itself to become sleepable, which the interrupt-gate design forbids today. It would mean reworking the window fault, giving KRFFS a real lock (the fault no longer being atomic for free), and trusting an interrupt path that the hardware here delivers unreliably. It buys nothing for this working set.
* Asynchronous (NCQ, many in-flight commands, a request queue). That is a separate, larger effort, essentially "make the kernel's disk I/O asynchronous," best done as its own milestone behind a real VFS and locking layer, not smuggled into a driver bring-up.

So IRQ and async stay out on purpose. Whoever revisits them starts from this analysis rather than rediscovering the constraint.

## The AHCI driver

`ahci.c` is the polled path made concrete, reusing the HD Audio driver's PCI and DMA idioms with storage registers swapped in.

* Bring-up. Discover the HBA on the PCI bus (class `0x01`, subclass `0x06`, prog-if `0x01`), recover the ABAR from BAR5 (not BAR0), map it cache-disabled, enable memory decoding, mask all interrupts, quiesce every implemented port, select the first port with a device and the link up, program the command-list and received-FIS bases, enable bus mastering, and start the engine. The HBA-wide reset (`GHC.HR`) is deliberately skipped: without the deferred full per-port COMRESET handshake, an HR leaves the SATA link needing re-establishment and the device not ready on QEMU (`PxTFD` loses `DRDY`). The per-port stop-before-program gives a clean owned port without disturbing the link; a real-hardware bring-up would HR and then COMRESET each port.
* A transfer. One command slot, one Register Host-to-Device FIS, one PRDT entry. `READ DMA EXT` (`0x25`) and `WRITE DMA EXT` (`0x35`) carry a 48-bit LBA; a write is followed by `FLUSH CACHE EXT` (`0xEA`) so the data reaches durable media, the durability the dirty-flag and `pc_sync` write-back rely on. The page cache issues exactly 4 KiB (eight sectors) per call, which one PRDT entry covers; a larger request fails closed rather than silently truncating. On a failed or timed-out command the driver recovers the port in place (stop the engine, clear the latched errors, restart it) per AHCI 6.2.2 error recovery, then returns `-EIO`, so the next command does not issue on a stopped engine.
* DMA memory. The command list, received-FIS area, and command table are static `aligned` arrays in `.bss`, which lands below the 32 MiB identity-mapped limit, so the virtual address is the physical DMA address with no translation and the bytes are contiguous below 4 GiB. `ahci_dma_sync` flushes descriptors before issue and invalidates the read buffer after, a real-hardware requirement that is a no-op on the cache-coherent x86 DMA both QEMU and real hardware provide.

`block_probe` is the seam: with `rach2.disk=ahci` it brings up AHCI (falling back to ATA if no HBA answers), and without the flag it brings up ATA, so every existing IDE image boots byte-for-byte as before. The two drivers never fight over one image, because the ATA driver refuses a SATA signature and an AHCI-only disk is invisible to it.

## Compared to Linux

Linux `drivers/ata/libahci.c` enables per-port interrupts, issues commands into a tagged queue, and completes them in an interrupt handler that wakes the block layer; the block layer merges and schedules requests across a deep queue. Rach2's AHCI is that driver with the queue depth set to one, the interrupt removed, and the completion handler replaced by a bounded spin on `PxCI`, because its one caller runs in a context that cannot take the interrupt. The ATA driver is the same shape one register set down: Linux's `libata` PIO path, polled and single-sector-handshaked, with no IRQ.

## Deliberate simplifications

* One command slot, polled, no NCQ: all the page-cache fault path ever issues.
* One PRDT entry per command: every milestone caller hands it a page-sized, identity-mapped, contiguous buffer. A non-page-aligned or larger caller would need a multi-entry PRDT or a bounce buffer.
* First present port only, single disk: there is one `block0`, like the rest of the storage stack.
* 512-byte logical sectors, asserted: KRFFS sector math assumes them.
* No hot-plug, no port multipliers, no ATAPI: the driver scopes to a fixed boot or data disk.
* `GHC.HR` and the full COMRESET/staggered-spin-up handshake are deferred; the lighter per-port stop/program/start suffices for QEMU `ich9-ahci`.
* The BIOS/GRUB boot stays on the legacy IDE channel (it uses INT 13h, not this driver); AHCI is the in-kernel-driven root or data disk, reached by booting the kernel directly. Proving a full SeaBIOS-from-AHCI boot is out of scope.

## Verify

The disk probes run on a real disk, never the RAM module, and each has an ATA form on `if=ide` and an AHCI form on an `ich9-ahci` HBA selected by `rach2.disk=ahci`:

* `make test-disk` / `make test-disk-ahci`: `blockprobe` writes a working set larger than the resident cache, fsyncs, and reads it back byte for byte, so a dropped-dirty or mis-mapped page is a mismatch, not a crash. Token `BLOCKPROBE OK`.
* `make persist-test` / `make persist-test-ahci`: `persistprobe` writes and fsyncs `/persist.txt` on one boot and reads it back on the next, proving the write reached the disk. Tokens `PERSISTPROBE OK (wrote)` then `PERSISTPROBE OK (read back)`.
* `make stress-test` / `make stress-test-ahci`: `stressprobe` writes and re-reads 33 MiB, past the 32 MiB RAM cap, proving a file system larger than memory round-trips through the cache. Token `STRESSPROBE OK`.

The capstones run over AHCI by booting `os-real` from a SATA disk (`DISK=... DISKBUS=ahci ./run.sh`): the SSH login (`ssh -i krffs/rach2_demo_ed25519 -p 2222 root@localhost`) reaches an interactive busybox ash over a dropbear pty, and Doom reads `doom1.wad` off the AHCI-mounted root and runs. At the kernel debug shell, `ahci` brings up the HBA, dumps its registers, and reads the MBR over the polled path as a one-shot self-test.
