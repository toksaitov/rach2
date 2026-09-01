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

## Phase 1 record

`fatcommon.c` and `fatcommon.h` hold the name check and conversion
(`fat_name_units`, from `fat32name.c`), the DOS timestamp fields
(`fat_epoch`, `fat_stamp`, `fat_now`), the allocation hint's two rules
(`fat_hint_start`, `fat_hint_next`) and the dirty-volume walk's bitmap
with its lock (`fat_walk_acquire`, `fat_walk_release`,
`fat_walk_test_set`, `fat_walk_marked`), each body moved without change;
`fat32.c` and `fat32name.c` call them. `fatname_hosttest` links
`fatcommon.c` with stubs for the wait queue and the clock, which its rows
never reach. Every FAT32 test passed unchanged after the move: `tests/run
host`, `style`, `mount` and `fatwrite`.

### A newfs_exfat volume, read by the first form of mkexfatfixture --check

Made on this host by `hdiutil create -size 64m -layout NONE -fs ExFAT`
(which runs `newfs_exfat`), attached and filled through the host's own
driver with six files, and detached; the host's zone was +0600. The
manifest (name, size, the first sixteen hex digits of the SHA-256, the
modification time as an epoch second):

```text
hello.txt|12|8f6c6c887f16ffbd|1788245030
pattern.bin|102400|27783e87963a4efb|1788245030
empty|0|e3b0c44298fc1c14|1788245030
été.txt|7|8f8df9963c962874|1788245030
Sub Dir/inner.txt|6|940a68104d3b6904|1788245030
A name of some length that takes more than one name entry.txt|1|2d711642b726b044|1788245030
```

The boot sector, field by field beside the specification's offsets:
`FileSystemName` (3) `EXFAT   `; `VolumeLength` (72) 131072 sectors;
`FatOffset` (80) 128; `FatLength` (84) 128; `ClusterHeapOffset` (88)
256; `ClusterCount` (92) 16352; `FirstClusterOfRootDirectory` (96) 5;
`VolumeFlags` (106) 0000; `BytesPerSectorShift` (108) 9;
`SectorsPerClusterShift` (109) 3; `NumberOfFats` (110) 1; `PercentInUse`
(112) 0; the main boot checksum in sector 11 and the backup's in sector
23 both equal to the rotate-and-add sum over the eleven sectors before
them with bytes 106, 107 and 112 skipped, and the backup boot sector
equal to the main. `FAT[0]` fffffff8, `FAT[1]` ffffffff.

The root's objects: the bitmap entry, type 81, flags 00, `FirstCluster`
2, `DataLength` 2044 (16352 bits); the up-case entry, type 82,
`TableChecksum` e619d30d over its 5836 stored bytes (the compressed form
of the recommended table: `a` maps to 0041, U+00E9 to 00C9, U+00FF to
0178, and the 0xFFFF-run form expands to exactly 65536 mappings), at
cluster 3. The root at cluster 5, the heap's first file at 6.

The first file set, `/hello.txt`, as stored:

```text
85 02 41 ab 20 00 00 00  79 05 21 5d 79 05 21 5d  79 05 21 5d 22 22 e8 e8  e8 00 00 00 00 00 00 00
c0 03 00 09 46 30 00 00  0c 00 00 00 00 00 00 00  00 00 00 00 06 00 00 00  0c 00 00 00 00 00 00 00
c1 00 68 00 65 00 6c 00  6c 00 6f 00 2e 00 74 00  78 00 74 00 00 00 00 00  00 00 00 00 00 00 00 00
```

Type 85 with `SecondaryCount` 2, `SetChecksum` ab41 (the 16-bit
rotate-and-add over the three entries skipping bytes 2 and 3),
`FileAttributes` 0020 (archive); the three timestamps 5d210579, the 10
ms fields 22 (0.34 s) and the offset bytes e8: the valid bit set and
0x68 as a signed seven-bit count of quarter hours, -24, so -360 minutes.
The timestamp field decodes to 2026-09-01 00:43:50 and the manifest's
epoch second is 06:43:50 UTC, so the host writes the field as UTC plus
the offset it records and the reading is UTC = field - offset, which is
the specification's rule for a valid offset; the driver reads it so. The
stream entry, type c0, flags 03 (`AllocationPossible` and `NoFatChain`),
`NameLength` 9, `NameHash` 3046 (over the up-cased name `HELLO.TXT` as
UTF-16 bytes), `ValidDataLength` 12, `FirstCluster` 6, `DataLength` 12;
the name entry, type c1, flags 00, `hello.txt` in UTF-16. Every file the
host wrote is contiguous with `NoFatChain` set; the directory `Sub Dir`
too. Nothing in the volume is dirty after the detach.

## Phase 2 record

`mkexfatfixture` builds the well-formed volume and thirty variants;
`exfatcheck_hosttest` proves the reader on every one. `fsck_exfat` calls
the well-formed volume and its four readable variants clean (the 4 KiB
logical sector cannot be attached as a 512-byte device node, so that one
is proved by the reader alone). What it found on the way:

* The bitmap and the up-case table are chained through the FAT: their
  entries carry no `NoFatChain`, so the FAT is where their chains live,
  and `fsck_exfat` refuses a volume whose FAT has none. The builder
  chains them and the reader follows the FAT for them, as `fsck_exfat`
  does.
* A stream extension's `AllocationPossible` shall be 1, the
  specification says (7.6.2), and `fsck_exfat` reports "no stream
  allocation" for a stream with it clear; the plan's decision to clear it
  on a stream with no clusters is corrected here, before the writer:
  every stream this driver writes has it set, with `FirstCluster` 0 for
  no clusters, and the reader refuses a stream without it.
* `newfs_exfat` writes every file contiguous with `NoFatChain`; the
  builder's `chained.bin` is the FAT-chained case no host volume showed.

Four builder mutations, each caught by the well-formed row through
mutate38p2.sh and mutate38p2b.sh: the set checksum over its own bytes,
the bitmap from cluster 0, the up-case checksum over the expanded
mappings, the name hash over the name as written. The builder's sums are
separate functions from the reader's for that reason.

## Open items

(none)
