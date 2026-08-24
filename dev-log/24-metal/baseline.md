# Windows baseline for the laptop

A Windows session measured these values on the Lenovo G50-70 before Rach2 accessed it. Every measurement used a read-only handle. The session never opened a write handle.

This file contains the complete retained record. Its source was a longer, untracked report in the repository root. That report has been deleted after preserving every lasting value here, including the four Windows facts at the end that a register dump cannot provide.

The acceptance run compares these values with the notebook. `rach2.diskdiag` provides `/proc/disk` and `/dev/disk0`, from which the run reads the same properties. Any difference is a result that requires investigation, regardless of which side is wrong.

## Machine

* Lenovo G50-70, serial YB04287142.
* Ethernet: Realtek RTL8168GU, station address `28:D2:44:7E:E0:AF`. The driver requires the expected masked revision `0x509`. Windows did not read it because that requires a raw MMIO access that no stock tool performs safely. The Rach2 diagnostic boot reads it.
* SATA controller: AHCI, ABAR at `0xC061B000`, 2048 bytes, MSI.
* The DMAR ACPI table shows that the chipset provides VT-d. If enabled, it blocks DMA and prevents the network card from working. The notebook session records the firmware setting.

## Disk

| Fact | Value |
|------|-------|
| Model | `Samsung SSD 870 EVO 1TB` |
| Serial | `S74ZNX0Y927920A` |
| Sectors | 1953525168 |
| Bytes | 1000204886016 |
| Partition style | MBR, no protective GPT entry |
| Disk signature | `0x4905B015` (bytes `15 B0 05 49` at offset 0x1B8) |

## Partition table

Two entries are used and two are free. The used entries cover the entire disk, leaving no unallocated space. Phase E must shrink a Windows file system before creating anything.

| Entry | Type | Start LBA | Sectors | Notes |
|-------|------|-----------|---------|-------|
| 1 | `0x07` | 2048 | 1024000 | active, System Reserved, 500 MiB |
| 2 | `0x07` | 1026048 | 1952499120 | C:, 931.02 GiB, boot |
| 3 | empty | 0 | 0 | free slot |
| 4 | empty | 0 | 0 | free slot |

Free entries 3 and 4 determine the phase E partition layout. It uses primary entries and requires no extended-partition support on this notebook.

## First sector

SHA-256 of the 512 bytes of the MBR:

```
652ad40fe72061185cf3a69c1a9a45394f68374f409153e082b7c1fa04c107d3
```

On the machine, the same number is produced by:

```sh
dd if=/dev/disk0 bs=512 count=1 2>/dev/null | sha256sum
```

Both systems hash the same 512 bytes with the same algorithm, making the comparison exact. Check a mismatch in this order: the diagnostic read a sector other than the requested one; the installed disk differs from this baseline; or the first sector has been written since the baseline. The milestone prohibits the third case, and the hash verifies that requirement.

## Windows configuration that must remain unchanged

The same Windows session recorded four installed-system facts that no register dump provides. They matter when opening or changing the notebook.

SATA link power management is disabled and must remain so. Under `HKLM\SYSTEM\CurrentControlSet\Services\iaStorA\Parameters\Device`:

    Controller0Phy0HIPM = 0
    Controller0Phy0DIPM = 0
    Controller0Phy1HIPM = 0
    Controller0Phy1DIPM = 0

The stock value for all four entries is 1. With those values, a warm reboot left the SATA link dead. Firmware reported no disk, continued to Realtek PXE, and reset in a loop. A complete power cycle restored the link. Do not restore the entries to 1. Their original values were exported on the notebook to `C:\Users\toksa_000\Documents\boot\iaStorA-Device-BEFORE.reg`. All other power settings remain at their stock values, including `EnableIdlePowerManagement = 1` in the controller's StorPort key. The Rach2 driver applies the same policy by writing `SCTL.IPM = 7` during reset, matching Linux `ATA_LPM_MAX_POWER`.

Fast Startup is enabled through `HiberbootEnabled = 1`, and `powercfg /a` lists it. Choosing Shut down therefore hibernates the kernel session instead of fully powering down the notebook. Hold SHIFT while selecting Shut down for a complete shutdown. A hybrid shutdown retains the preceding session's disk state and must not precede hardware changes.

BitLocker has never been enabled. `Get-BitLockerVolume` lists only the unencrypted C: volume, and the 500 MiB recovery partition is also not a BitLocker volume. The drive remains readable when removed from the notebook.

The final BIOS release for this board is InsydeH2O `9ACN32WW`, with SMBIOS version `9ACN32WW` and EC `9AEC32WW`. It was updated from `9ACN25WW` on 2026-08-10. The update disabled BIOS Back Flash, which must be re-enabled in setup before a downgrade.

## Expected difference

`/proc/disk` reports `sectors 1953525168` and `window-sectors 8388607` because a 32-bit file offset limits the whole-disk window. The output displays this intentional difference. Every LBA in this record lies within the window, so the complete baseline can be checked through it.
