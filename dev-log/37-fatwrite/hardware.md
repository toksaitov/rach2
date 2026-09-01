# Milestone 37 hardware notes

Nothing touches the Lenovo in this milestone. What the plan relies on from
milestones 25 and 35: the disk has three partitions and a free fourth MBR
entry, with about 250 GB unused after the recovery partition. Writing sector
zero is a whole-disk write and belongs to the recovery kernel and `reflash`,
under the disk rule in CLAUDE.md: the recovery stick booted and shown to
work, every sector about to be overwritten saved on the host and on the
stick, recovery the saved boot default until the write is verified.

## The fourth partition's procedure

(written in phase 1, rehearsed on the sealed disk image in phase 5, run in
milestone 43)
