# Milestone 40 hardware

What this milestone measures about the machines it runs on. The Lenovo's
column is filled in milestone 43; nothing here touches it.

## The controller's tables

| Table | Before | After | Bytes before | Bytes after |
|-------|--------|-------|--------------|-------------|
| (filled in phase 2) | | | | |

## The stick images

| Image | Size | Table | Partitions | Read back by |
|-------|------|-------|------------|--------------|
| MBR stick | 64 MiB | MBR | 0x0C at 2048 for 65536, 0x07 at 67584 for 32768 | `mdir`, `mkexfatfixture --check` |
| GUID stick | 64 MiB | protective 0xEE and a GUID table, first used entry at index 6 | the same two volumes | `mdir`, `mkexfatfixture --check` |
| Superfloppy | 16 MiB | none | one exFAT volume from sector 0 | `mkexfatfixture --check` |

## The hybrid ISO

| Field | What `mkiso.sh` produces |
|-------|--------------------------|
| Sector 0 | GRUB's hybrid boot code, `0x55AA` at 510 |
| Partition entries | four zero entries |
| Sector 1 | the ISO system area, zeros; no GUID header |
