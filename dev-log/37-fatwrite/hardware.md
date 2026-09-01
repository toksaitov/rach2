# Milestone 37 hardware notes

Nothing touches the Lenovo in this milestone. What the plan relies on from
milestones 25 and 35: the disk has three partitions and a free fourth MBR
entry, with about 250 GB unused after the recovery partition. Writing sector
zero is a whole-disk write and belongs to the recovery kernel and `reflash`,
under the disk rule in CLAUDE.md: the recovery stick booted and shown to
work, every sector about to be overwritten saved on the host and on the
stick, recovery the saved boot default until the write is verified.

## The fourth partition's procedure

Written here, rehearsed on the sealed disk image in QEMU in phase 5, run
in milestone 43 with the owner's approval of the named target at the time.

The sealed layout (`make rach2-sealed-disk`): a 1 MiB gap, the 64 MiB FAT
boot partition at sector 2048, the 500 MiB KRFFS root at sector 133120, the
64 MiB recovery partition at sector 1157120, ending at sector 1290239. The
Lenovo's disk is 250 GB, so the fourth MBR entry is free and about 250 GB
lie unused after sector 1290240. The scratch partition is entry 4: type
0x0c, starting at sector 1290240, 2 GiB (4194304 sectors), formatted FAT32
by mformat on the host.

Sector zero is a whole-disk write, so the recovery kernel and `reflash` do
it, under the disk rule:

1. Boot the recovery stick (`rach2-recovery-iso`, phase 5) on the machine
   and show it works: `reflash` answers, `/dev/disk0` reads.
2. Save what will be overwritten, on the host and on the stick: sector 0
   (`dd if=/dev/disk0 bs=512 count=1`), and the 2 GiB the partition will
   cover (a read of 4194304 sectors at 1290240; on the host as a file,
   compared by sha256 with the stick's copy).
3. Build the new sector 0 on the host from the saved one: a sparse file of
   the disk's size whose first sector is the saved sector 0, then `mkmbr
   --lba` with the three existing entries copied from `mkmbr --list`
   exactly and the fourth added; `mkmbr` rewrites only the 64 table bytes
   and the signature, so the boot code stays as saved. Verify with `mkmbr
   --list` that entries 1 to 3 are unchanged and 4 is the scratch range.
4. Make recovery the saved boot default (`bootctl env saved_entry
   recovery`) before any write, as the root procedure does.
5. Write sector 0: `reflash /dev/disk0 0 512` with the new sector; read
   it back and compare with the host's bytes.
6. Write the FAT32 volume: `mformat -C -F -T 4194304` into a host file,
   `--check` it, then `reflash /dev/disk0 660602880 2147483648` from it;
   read the first sectors back and `--check` them at the offset.
7. Reboot into the ordinary system; `/dev/disk0p4` appears; mount it
   read-only, then read-write once milestone 37's writer is installed.
8. Put the boot default back to `candidate`.

A step that fails after 5 leaves a valid table with an empty or partial
volume, which the stick can repair by writing the saved sector 0 back; no
step needs the disk out of the bay.
