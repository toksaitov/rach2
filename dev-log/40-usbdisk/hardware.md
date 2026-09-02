# Milestone 40 hardware

What this milestone measures about the machines it runs on. The Lenovo's
column is filled in milestone 43; nothing here touches it.

## The controller's tables

Four slots and eight endpoint indexes before this milestone; eight slots and
thirty-two indexes after it, which is fifteen endpoints in each direction and
the number a stick picks its own from. The scratchpad is the controller's own
demand and did not change: QEMU asks for none and the notebook for one.

| Table | Before | After |
|-------|--------|-------|
| Device context base array | 40 | 72 |
| Device contexts | 2560 | 18432 |
| Input context | 576 | 2112 |
| Endpoint rings | 10240 | 73728 |
| Command and event rings | 8192 | 8192 |
| Total | 21608 | 102536 |

## The stick images

| Image | Size | Table | Partitions | Read back by |
|-------|------|-------|------------|--------------|
| MBR stick | 80 MiB | MBR | 0x0C at 2048 for 98304, 0x07 at 100352 for 32768 | `mdir`, `mkexfatfixture --check` |
| GUID stick | 80 MiB | protective 0xEE and a GUID table, first used entry at index 6 | the same two volumes | `mdir`, `mkexfatfixture --check` |
| Superfloppy | 16 MiB | none | one exFAT volume from sector 0 | `mkexfatfixture --check` |

The FAT32 partition was 32 MiB in phase 1 and is 48 MiB in the suite: a
FAT32 volume must have at least 65525 clusters, and a 32 MiB one made by
`mformat` has 64496, which BusyBox's `blkid` takes for FAT16.

## Throughput

A 16 MiB read through `/dev/usb0` in 32 KiB pieces, timed by the machine's
own clock. The Lenovo's column is milestone 43's.

| Pump | QEMU on this host | Lenovo G50-70 |
|------|-------------------|---------------|
| The controller's messages | 49648 KB/s, 330 ms | |
| The timer alone (`rach2.nomsi`) | 133 KB/s, 122880 ms | |

On the timer alone each page-sized request costs three ticks, one for each
of the wrapper, the data and the status.

## The hybrid ISO

| Field | What `mkiso.sh` produces |
|-------|--------------------------|
| Sector 0 | GRUB's hybrid boot code, `0x55AA` at 510 |
| Partition entries | four zero entries |
| Sector 1 | the ISO system area, zeros; no GUID header |
