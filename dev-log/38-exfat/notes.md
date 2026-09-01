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

## Phase 3 record

`exfat.c` reads: the volume check over both boot regions with the
two-region rule, the root's objects, the up-case table in its three forms
(held at most 16 KiB, uncompressed indexed through the cache, compressed
streamed through the cache), sets and their invariants, names through the
table, chains with and without the FAT, timestamps with the UTC offset,
`readdir`, `lookup`, `read`, `statfs`, and the `exfat` type in the table
beside `vfat`. What the rows found:

* The recommended up-case table's compressed form ends with a `0xFFFF`
  that is the mapping of U+FFFF to itself, not a run marker; the check
  read it as a marker with no count and refused the table. A trailing
  `0xFFFF` is a mapping, in the kernel and in the host reader alike.
* A read of a damaged chain gives the sound bytes and then `EIO`; the
  rows first asked for the sound count from one call, which the driver
  answers and then refuses on the next.
* `--short-stream` on a contiguous file walked into the next file's
  cluster, since a `NoFatChain` run has no end but its length; the
  variant damages the FAT-chained file instead, and `--chain-out`,
  `--loop` and `--short-stream` all read to the fault and answer `EIO`.
* The host test buffer was smaller than `pattern.bin` on the gate.
* The host test's `klog` stub prints the kernel's records into the
  runner's log, so a refusal or a skipped set names its reason there.

The gate: the volume `newfs_exfat` made and the host's driver filled (six
files, a subdirectory, a non-ASCII name, a name of four entries, a
102400-byte pattern) mounts and lists every manifest entry with its bytes
and its modification time, read through the UTC offset the host wrote.
`mkexfat_external.sh` makes that volume on every run of the host suite
where `hdiutil` and `newfs_exfat` exist, and the test says "skipped"
where they do not; `mkfs.exfat` is not on this host.

## The gate review round

Unit A (the fixture builder and its reader; Codex, ten minutes) found
what the specification requires and the builder or the reader missed;
all fixed:

* The volume GUID entry is a set of one and carries the set checksum;
  the builder wrote zero, and the reader now checks it.
* `PercentInUse` is the heap's percentage rounded down, or 0xFF; the
  builder wrote 1.
* The bitmap and the up-case table are FAT chains, not extents; the
  reader read them as contiguous bytes and now follows the FAT, and
  guards the entries' clusters before use.
* Every word of a checksum sector repeats the checksum; the reader read
  the first. The jump bytes, the reserved bytes, the revision, the
  extended boot sectors' signatures and FAT entries 0 and 1 were not
  checked; they are.
* A set may have up to 255 secondaries with vendor entries; the reader
  refused more than 18. The name entries must be consecutive right after
  the stream; the reserved flag bits zero; a directory's two lengths
  equal; the first 128 up-case mappings the mandatory ones; bytes past
  the table's last mapping a fault; a second up-case entry a fault.
* `--truncated-upcase-run` removed the last mapping, not a run's count;
  it cuts the table after a run's marker now.
* Undefined behaviour: a short image read before its size was known, a
  volume length whose product could overflow, an up-case pointer formed
  before its cluster was checked, and 16-bit chain ids that wrap; all
  four are gone.

Unit B (the kernel reader; Codex, twenty-four minutes, past the ten the
plan allows, and recorded as such: its job went to the background and
its output was read from the file it named) found fifteen things,
fourteen fixed and one kept:

* The constructor validated the main boot sector's fields before the
  backup was considered, and a fallback copied five fields; it reads
  both regions and their checksums first, takes every field but the
  flags from the region it trusts, and validates those.
* An unrecognised critical primary in the root makes the volume invalid
  by the specification; the plan's decision to read as far as possible
  and freeze the directory for writing stands, and is recorded here as
  the deviation it is.
* The bitmap and the up-case table are read through the FAT, as above.
* A rejected set was passed over one entry at a time, so an entry inside
  it could be read as a primary; it is passed over whole.
* A cluster naming itself is `EIO`; a longer loop within a file's length
  repeats its clusters and is the check's to find, which the Readme now
  says (the bound on steps cannot see it).
* `clusters_of` could wrap at the format's 64-bit limit; it divides by
  shift and adds the remainder's bit.
* The jump bytes, a volume length of at least 1 MiB, and every word of a
  checksum sector are checked.
* The set grammar: name entries consecutive after the stream, benign
  secondaries only after them, a second stream or an unknown critical
  secondary a fault, reserved flag bits zero, a directory's lengths equal
  and at most 256 MiB, `NoFatChain` with a length.
* A 128 KiB table with run markers is compressed, not uncompressed; the
  form is decided by content, and the mandatory first 128 mappings are
  checked.
* The parent walk kept only a directory's first cluster; it keeps the
  chain kind and the length per level.
* The root's chain leaving the heap or looping is refused.
* The bitmap is the one of the active FAT; a second of the active FAT's,
  or a second up-case table, is refused; the other FAT's keeps the mount
  read-only as before.
* A listed name past the component is cut at whole characters and
  always terminated.
* U+007F is a legal character; the shared check forbade it.

Rows that came with the fixes: a name outside ASCII found by its upper
case through the table; a main region that fails and whose root differs
read from the backup; a zoned timestamp with hundredths; `--zoned` in
the fixture; the `..` row's pointer initialised.

The plan's eleven reader mutations, through mutate38p3.sh and its two
reruns (a restoring script that compiles the host test as the runner
does, under the runner's `deadline`): skip the boot checksum; use the
main region when it fails; accept a backup that disagrees; skip the set
checksum; trust a false name hash; up-case ASCII only; index a compressed
table directly; read a NoFatChain stream through the FAT; treat the tail
past ValidDataLength as data; apply an invalid UTC offset; accept a short
FAT. Every one caught. Three needed a row the first rows lacked: the
false hash was invisible to lookup, which compares hashes itself, so the
skipped rows now require the set absent from the listing too; the
compressed table indexed directly gave the right answer for every
character up to U+0586, where the stored table's first run begins, so
the fixture has a name with U+1E01, the first case-mapped unit a run
moves; and the valid length of `valid.bin` now spans two clusters, so a
NoFatChain stream read through the FAT fails on the fixture and not only
on the gate. The tail mutant's first form looped rather than failed (a
read of zero bytes at the valid length never advanced), which the
watchdog ended; its second form reads the disk and fails the row.

## Phase 4 record

The writer: the dirty bit written to the main boot sector when a
writable mount is constructed and cleared last by the clean unmount;
allocation first-fit in the bitmap from the hint, `ENOSPC` before any
write; the order of writes with the bitmap in the FAT's place; the set
protocols (secondaries first, the file entry last with the checksum over
the published set; a new set's file and stream entries in one sector,
an inactive filler where the run would end a sector; a straddling set's
two writes, stream first); `NoFatChain` conversion before a contiguous
file or directory grows; `ValidDataLength` in every transition; every
mutating operation with the reclamation at the last put; frozen
directories; `path` for directory descriptors. What the rows found:

* A write through a description of an unlinked file republished its
  retired set; an unlinked node publishes nothing.
* A cross-directory rename computed the new set's checksum over the
  inactive file entry it wrote first, so the entry it published last did
  not match; the checksum is over the published set, and the in-use bit
  is cleared in the written bytes alone.
* The matrix counted the mount's own dirty-bit write as a stop; the
  count starts after the mount.
* The state between a chain's join and the length's publish is a surplus
  tail, one of the three repairs the check makes; the host reader names
  it and the matrix allows it, beside lost clusters, orphan secondaries
  and a checksum-only set.

The host reader classifies what a stop leaves: an in-use secondary
outside a set is an orphan (counted per directory), a set whose only
fault is its checksum is "checksum-only", and a FAT chain longer than
its length is a surplus. `--junk` fills every free cluster with bytes
nobody wrote, so the oracle sees a length published before its data.

The matrix: ten sequences (create, extend across a cluster, a gap of
zeros across a cluster, truncate down to nothing and up again, unlink
held and written through while another file is written, mkdir with a
file and rmdir, rename in place, across and over, the fixture's
contiguous file grown through the conversion, a change to the fixture's
straddling set, a write past another writer's valid length over junk),
137 stops, each walked by the reader and read by the oracle with the
fixture's own files intact. Two rows read the disk for what the matrix
cannot see: no file entry of a set this driver makes in a sector's last
slot (over the root's first two clusters, past the fixture's entries),
and the backup boot region byte for byte while the dirty bit is on and
after the clean unmount.

`fsck_exfat` on this host over what the rows left (one image per
sequence after its clean unmount, and one per sequence stopped before
its last write): every clean volume "appears to be OK"; every stop gets
the diagnosis its state implies, read without modifying (a bitmap to
repair for lost clusters, a chain with too many clusters for its size
for a surplus, unexpected secondary entries for orphans, an incorrect
checksum for the straddling set's half-written change), and after the
checker repairs a copy, both it and `--check` call the copy clean.

The plan's thirteen writer mutations, through mutate38p4.sh and its
reruns: publish a file entry before its name entries; write a
straddling set's file entry before its stream; place a new set across a
sector boundary; change a frozen directory; clear NoFatChain before the
run is in the FAT; raise ValidDataLength over an unzeroed gap; preserve
ValidDataLength through a shrink; allocate without setting the bitmap
bit; free a held node's clusters at unlink; free bitmap bits before the
file entry is inactive; hash a name without up-casing it; write the
flags into the backup boot sector; set the dirty bit at the first write
instead of at mount. Twelve caught. The second cannot be: under a 4 KiB
block cache a straddling set's two sectors lie in one block, and a stop
between the two writes of either order leaves a set whose checksum
alone fails, which the check repairs to a consistent volume, with the
change in one order and without it in the other; the two are
legitimate outcomes of a crash and no row can prefer one. The
convention stands (the stream's sector first, the file entry's last)
and is proved by reading `set_store`. Four needed a row the matrix
could not provide: the placement rule read from the root's raw entries;
the gap zeroed inside another writer's file with junk past its valid
length (`valid.bin`, the vdl sequence); a held unlinked file's clusters
not reused by a file written meanwhile; and the backup region compared
while the dirty bit is on, since the clean unmount put the mutant's
byte back before the first form of the row looked.

## Open items

(none)
