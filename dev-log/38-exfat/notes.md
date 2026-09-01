# Milestone 38 notes

## Phase 1 inventory

Base: a clean tree on master at the close of milestone 37.

```text
a2c2cb2 Record milestone 37
09926dc Keep the raw suite's serial logs
5eeefa9 Accept the kernel ping's round-trip time in the raw probe
4e846df Accept the crash's lost clusters in the fatwrite suite's later walks
61e4783 Unmount a mount that went read-only without touching the volume
c2e3d93 Stage update in the grub test image
ecf640f Fix the review findings in the FAT32 writer, its check and its names
422738d Fix the cache review's findings
256ad39 Correct the documents against the code and the proofread
5bf8706 Record phase 5 of the FAT32 writing milestone
c11a2e4 Save every sector, set the boot default and rehearse the undo in the scratch suite
9abf104 Check each sync's result in the writers row and state what the row proves
```

The host: `newfs_exfat`, `fsck_exfat` and `hdiutil` under `/sbin` and `/usr/bin`; no `mkfs.exfat`.

## Review checkpoints (fixed now; at most two units run at once, the owner's rule in CLAUDE.md, which supersedes the plan's five)

The gate round after phase 3: unit A, the fixture builder and its reader: mkexfatfixture.c, fatcheck_hosttest.c's exFAT rows; unit B, the kernel reader: exfat.c (the volume check, the root's objects, the up-case table, sets, names, chains, timestamps), exfat.h, exfat_hosttest.c's read rows, fatname_hosttest.c's exFAT rows. The round before the sweep: unit C, the writer's order of writes and the set protocols: exfat.c's write half, exfat_hosttest.c's sequences and matrix; unit D, the dirty-volume check and its repairs: exfat.c's check, the dirty fixtures, the dirty rows; unit E, the shared code and the suite: fatcommon.c/h, fat32.c's and fat32name.c's use of it, exfat.sh, the probe; unit F, the proofread of the reader-facing documents. A focused re-review for a fix that changed a contract.

## What moves to fatcommon.c, and what stays

Moved without change to their bodies: the UTF-8 to UTF-16 conversion and the forbidden-character check (`fat32_name_units` and `forbidden`, from fat32name.c); the DOS timestamp bit fields (`fat_epoch`, `fat_stamp`, `fat_now`, from fat32.c); the dirty-volume walk's bitmap, its lock and its bit test (`walk_bits`, `walk_busy`, `walk_wq`, `bit_test_set`, from fat32.c); and the first-fit search from a hint that wraps once (the loop of `alloc_cluster`), over a caller's test of a cluster's freedom. The contiguous-run bookkeeping of `alloc_run` stays in fat32.c: each step of it is a FAT entry write, and exFAT's phase 4 decides whether its chains share it.

## Open items

(none)
