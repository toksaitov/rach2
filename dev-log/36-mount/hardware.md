# What the two machines report (milestone 36)

## QEMU (the suite's disk)
mkdisk.sh test disk: MBR entry 1 type 0x7f at LBA 2048, 48 MiB KRFFS; suite adds entry 2 type 0x0c at LBA 100352, 14 MiB FAT32 (mformat -F, 512-byte sectors, 1-sector clusters). AHCI via -device ahci or ATA default; blockprobe proves both.

## Lenovo G50-70 (recorded; nothing touches it this milestone)
From milestones 25 and 35: 1 MiB gap; p1 FAT32 0x0c, 64 MiB, 8-sector clusters, the boot slots (bootctl's volume); p2 KRFFS 0x83, 500 MiB at byte 68157440 (read from the machine's sector zero in milestone 43; an earlier draft here said 0x7f); p3 FAT32, 64 MiB, recovery kernel and RAM image; entry 4 free; ~250 GB unused beyond. Disk on AHCI (rach2.disk=ahci). Milestone 43 mounts p1 and p3 read-only and runs the shell lag check there.
