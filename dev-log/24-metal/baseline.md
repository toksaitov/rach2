# The laptop as Windows measured it

These are the numbers a Windows session on the Lenovo G50-70 read out of the machine before Rach2 had ever touched it. Everything below was read through read-only handles; no write handle was ever opened.

This file is now the whole of that record. It came from a longer report that lived untracked in the repository root, and everything from it that outlives the session has been carried in here, including the four Windows-side facts at the end that no register dump reports. The report itself is gone.

They exist to be compared against. On the laptop, `rach2.diskdiag` publishes `/proc/disk` and `/dev/disk0`, and the acceptance run reads the same facts back off the machine. A disagreement is the finding, whichever side is wrong.

## The machine

* Lenovo G50-70, serial YB04287142.
* Ethernet: Realtek RTL8168GU, station address `28:D2:44:7E:E0:AF`. The masked chip revision is expected to be `0x509`, which is what the driver gates on; it was NOT read under Windows, because that needs a raw MMIO read no stock tool does safely. The Rach2 diagnostic boot reads it.
* SATA controller: AHCI, ABAR at `0xC061B000`, 2048 bytes, MSI.
* A DMAR ACPI table is present, so the chipset exposes VT-d. If it is enabled, DMA is blocked and the network card will not work; the firmware setting is recorded in the laptop session.

## The disk

| Fact | Value |
|------|-------|
| Model | `Samsung SSD 870 EVO 1TB` |
| Serial | `S74ZNX0Y927920A` |
| Sectors | 1953525168 |
| Bytes | 1000204886016 |
| Partition style | MBR, no protective GPT entry |
| Disk signature | `0x4905B015` (bytes `15 B0 05 49` at offset 0x1B8) |

## The partition table

Two entries used, two free. The two used entries account for the whole disk, so there is no unallocated space and phase E's carve must shrink a Windows filesystem before anything can be created.

| Entry | Type | Start LBA | Sectors | Notes |
|-------|------|-----------|---------|-------|
| 1 | `0x07` | 2048 | 1024000 | active, System Reserved, 500 MiB |
| 2 | `0x07` | 1026048 | 1952499120 | C:, 931.02 GiB, boot |
| 3 | empty | 0 | 0 | free slot |
| 4 | empty | 0 | 0 | free slot |

Entries 3 and 4 being free is what decided phase E's shape: the carve uses primary slots, and no extended-partition support is needed on this machine.

## The first sector

SHA-256 of the 512 bytes of the MBR:

```
652ad40fe72061185cf3a69c1a9a45394f68374f409153e082b7c1fa04c107d3
```

On the machine, the same number is produced by:

```sh
dd if=/dev/disk0 bs=512 count=1 2>/dev/null | sha256sum
```

Both sides hash the same 512 bytes with the same algorithm, so the comparison is exact. A mismatch means one of three things, in the order worth checking: the diagnostic read a different sector than the one named, the disk is not the disk this baseline describes, or something has written the first sector since. The third is what this milestone promises cannot happen, and this hash is how that promise is checked rather than asserted.

## The Windows side, and what must not be undone

Four facts about the installed system that no register dump reports, and that matter to anyone who opens this machine. They were read from Windows in the same session as the numbers above.

SATA link power management is deliberately off, and must stay off. Under `HKLM\SYSTEM\CurrentControlSet\Services\iaStorA\Parameters\Device`:

    Controller0Phy0HIPM = 0
    Controller0Phy0DIPM = 0
    Controller0Phy1HIPM = 0
    Controller0Phy1DIPM = 0

The stock value for all four is 1. At stock, a warm reboot left the SATA link dead: the firmware reported no disk, fell through the boot list to Realtek PXE, and reset in a loop. A full power cycle cleared it. Do not restore these to 1. The originals were exported to `C:\Users\toksa_000\Documents\boot\iaStorA-Device-BEFORE.reg` on the machine. Everything else power-related is at stock, including `EnableIdlePowerManagement = 1` on the controller's StorPort key. Rach2 makes the same choice in its own driver, writing `SCTL.IPM = 7` in the reset path, which is Linux's `ATA_LPM_MAX_POWER` policy.

Fast Startup is enabled (`HiberbootEnabled = 1`, and `powercfg /a` lists it). So choosing Shut down does not fully shut the machine down: it hibernates the kernel session. Hold SHIFT while clicking Shut down for a real one. This matters before any work that changes hardware, because a hybrid shutdown leaves the previous session's view of the disks in place.

BitLocker has never been enabled. `Get-BitLockerVolume` lists only C:, unencrypted, and the 500 MiB recovery partition is not a BitLocker volume either. The drive that comes out of this machine is readable as it stands.

The BIOS is InsydeH2O `9ACN32WW` (SMBIOS version `9ACN32WW`, EC `9AEC32WW`), which is the final release for this board. It was flashed on 2026-08-10 from `9ACN25WW`, and the flash reset BIOS Back Flash to Disabled, so a downgrade needs that re-enabled in setup first.

## What is expected to differ

`/proc/disk` reports `sectors 1953525168` and `window-sectors 8388607`, because the whole-disk window is bounded by a 32-bit file offset. That disagreement is by design and is printed rather than hidden. Every LBA named above is inside the window, so the baseline is fully checkable through it.
