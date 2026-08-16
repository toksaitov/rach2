# The laptop as Windows measured it

These are the numbers a Windows session on the Lenovo G50-70 read out of the machine before Rach2 had ever touched it. They are here because they are the only baseline the SSD half of this milestone has, and because the report they came from (`Lenovo_G50-70_report_temp.txt`, untracked in the repository root) is deleted when the hardware work concludes. Everything below was read through read-only handles; no write handle was ever opened.

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

## What is expected to differ

`/proc/disk` reports `sectors 1953525168` and `window-sectors 8388607`, because the whole-disk window is bounded by a 32-bit file offset. That disagreement is by design and is printed rather than hidden. Every LBA named above is inside the window, so the baseline is fully checkable through it.
