# disk(4)

NAME: disk, bootpart, grubcore - the disk nodes. Source: disk(7)'s sheet and rach2/sys/block/Readme.md "Raw disk devices"; devfs.c dev_table (bootpart 240/0 0600, grubcore 240/3 0600) and the disk nodes (major 240, disk 0 minor 1, disk N minor 64+16N; partition nodes; 0400).
* `/dev/diskN` and `/dev/diskNpM` (block devices to stat, mode 0400, root only; reads of any byte range; writes only on the recovery kernel, sector-aligned, one writer per disk, EBUSY under a claim; BLKRRPART); `/dev/bootpart` (MBR entry 1, FAT32, the boot partition window) and `/dev/grubcore` (LBA 1 to 2047), character devices, 0600, one open description at a time for grubcore; the reservation and the claims as disk(7) says; what a pulled stick answers.
* SEE ALSO: disk(7), loop(4), mount(8), reflash(8), bootctl(8).
