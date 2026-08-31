# Milestone 38: exFAT

## The goal

The kernel reads and writes exFAT, the file system every flash drive above 32 GB and every SDXC card is sold with, on the buffer cache and under the write rules milestone 37 fixed for FAT32: metadata written through in dependency order, data written back, and a power cut between two completed sector writes leaving a volume the mount-time check calls consistent, with clusters at worst leaked and never shared, and no file that was reachable before the crash lost by the check afterwards; a crash inside a rename can leave a file nameless with its data intact, as on FAT32, and that is the one way a name is lost.

What this milestone proves is the file-system logic and emulated persistence. It is complete when volumes formatted by `newfs_exfat` on this host and, where available, by `mkfs.exfat` mount and read correctly; when a volume written by Rach2 in QEMU passes `fsck_exfat` or `fsck.exfat` clean and reads back identically through the independent fixture reader; when a volume stopped after every sector write of a scripted sequence is consistent at every stop with every file that existed before the stop intact; and when the same volume survives a reboot. What it does not prove, and says so: volumes written by Windows, cameras and phones, and any real USB transport. Those arrive with the first stick in milestone 40, and the reader's every refusal is by name so that a new case there is a named case.

The writer is not started until the reader has passed every external image the host can make. That gate is inside the milestone, in phase 3, and it is what keeps a reader that misreads a real volume from being built on.

This plan is the authority for the run. A conflict between this plan and the current tree is resolved by the decisions here; a conflict between this plan and `CLAUDE.md` is resolved by `CLAUDE.md`. The plan was revised twice after independent review. The first revision changed the set protocols, the repairs, the bitmap comparison, the up-case handling, the volume check, the dirty bit, and the tests, and reached back into milestone 37 to stop the mount-time check from freeing lost clusters. The second replaced the relocation protocol, which had a stop where the check would have hidden a file, and fixed three proofs. The third removed relocation altogether, because its marker could misfire on a volume another writer had cross-linked, and made the frozen-directory rule eager.

## What milestones 36 and 37 leave in place, and this plan relies on

* The file-system interface and its lock order: the table, then a mount's lock, then a description, then the global walk lock, then buffers in ascending block order with at most four held. Every mutating entry asserts that it holds the mount's lock. `sync` takes the lock and calls a lock-held helper; `prepare_unmount` uses the helper and `unmount` runs only after it succeeds.
* `bcache_write` for a synchronous metadata write, `bcache_dirty` and `bcache_sync` for data, a device that fails a write making its mount read-only for life, `fsync` as a device sync, `sync(2)` through every mount, and the `update` child of `init` every thirty seconds, which never clears a dirty flag.
* The node pool with identities and a generation, an unlinked node held until its last `put` with its clusters reclaimed then and not before, a rename that rekeys the source node and detaches a replaced destination.
* The FAT32 dirty-volume check: one global walk lock, one static bitmap of 2^20 bits, three repairs that cannot lose anything (surplus tails detached and freed, orphan entries freed, mirrors rewritten), lost clusters counted and left allocated, read-only for anything else, and the recursive interruption matrix that stops after every repair write.
* The name conversion between UTF-8 and UTF-16, the forbidden characters, and the 255-byte component limit that makes a 255-unit non-ASCII name unreachable by path, all in `fatcommon.c` once this milestone has moved them there.
* `mkfatfixture` with `--check`, and the interruption matrix as the shape of every write test: the fake device records each physical write, the sequence is stopped after each, the cache is discarded, the driver is mounted fresh on the image, and the volume is judged by the walk, by the fixture reader, and by a content oracle over poisoned free clusters that also requires every file that existed before the stop to be intact.
* The exFAT facts, from Microsoft's published specification: the boot sector's fields and offsets; the boot checksum over eleven sectors skipping bytes 106, 107 and 112; the entry types 0x81, 0x82, 0x83, 0x85, 0xA0, 0xC0, 0xC1, 0xE0 and 0xE1 with bit 7 as in-use, bit 6 as secondary and bit 5 as benign; `AllocationPossible` and `NoFatChain` in the secondary flags; the set checksum skipping its own two bytes; the name hash over the up-cased name; the timestamp bit fields, the 10 ms increments and the UTC offset byte; FAT entries 0xFFFFFFFF for end of chain and 0xFFFFFFF7 for bad; the bitmap with bit 0 of byte 0 as cluster 2; the up-case table's checksum and its compressed form with `0xFFFF` runs.
* Linux's driver, since 5.18, sets `VolumeDirty` when a volume is mounted for writing, keeps it set while mounted, and clears it only at unmount.
* On this host: `newfs_exfat` and `fsck_exfat`; on Debian: `exfatprogs` with `mkfs.exfat` and `fsck.exfat`. Neither is a required tool and no suite depends on one. This development host, which has the checker, is the designated verification environment: the checker runs here on every volume the kernel writes before the milestone closes, and the suite runs it wherever it finds it.
* mtools cannot read exFAT, so the reader that is not the driver is the fixture builder's own, and the builder and its reader share a source. That is why the external checkers and external images are required as well: two implementations from one author can share one misreading, and the milestone does not close on them alone.

## Decisions on reading

### The volume check

The boot sector is checked field by field, with every product of sectors and bytes computed in checked arithmetic: `EXFAT   ` at offset 3, fifty-three zero bytes at 11, a volume length within the partition, a partition offset that is zero or equal to the partition's first sector, a FAT offset at or past sector 24 so that both boot regions lie before it, a FAT length of at least `(ClusterCount + 2) * 4` bytes so that every cluster has an entry, a cluster heap that follows the FATs, a cluster count that fits the heap and is at most 2^32 minus 11, a root cluster within the heap, a file-system revision of 1.00, a bytes-per-sector shift of exactly 9, a sectors-per-cluster shift that keeps a cluster within 32 MiB, and a FAT count of one or two. Any other bytes-per-sector shift is a valid exFAT this kernel does not read, refused by name, because milestone 36 made every block device 512-byte sectors and a volume whose logical sector is larger is a different layout in every offset.

The boot checksum over the main boot region is computed and compared with its checksum sector, and the backup region at sector 12 is checked the same way, always. Both good and agreeing in every field but the flags and the percent in use: the mount proceeds. Main bad and backup good: the mount proceeds read-only from the backup, with a record, because the main copy is what the next driver will trust. Main good and backup bad, or the two disagreeing in geometry: the mount is read-only with the disagreement named, because a writer that trusted one of two conflicting descriptions could be wrong. A volume with two FATs is TexFAT and mounts read-only; so does one whose flags say `MediaFailure`. The percent in use is read and never trusted.

### The root and what it must hold

The root directory is read first. It must hold one allocation bitmap entry and one up-case table entry, each with a first cluster in the heap and a length that fits, the bitmap's at least `ceil(ClusterCount / 8)` bytes; their extents and the root's own chain must be disjoint, in bounds and marked allocated in the bitmap, and they are the three objects the dirty-volume check seeds its walk with, because they are allocated and are not files. A volume label is optional. A benign primary entry, such as a volume GUID, is kept byte for byte and never rewritten. A second bitmap entry, which only TexFAT has, keeps the mount read-only. A critical entry of a type this driver does not know cannot be a feature, because the mount requires revision 1.00 and this driver knows every critical type that revision defines; it is a corruption. Before a directory is changed for the first time in a mount it is read to its end, and one that holds such an entry is frozen: every change inside it answers `EROFS` with a record, it cannot be removed or renamed, its sets are read as far as they can be, and the rest of the volume stays as it was. That is why a writable mount does not scan the tree first: nothing valid below the root can withdraw the right to write, and a corrupt directory is found before it is touched.

### The up-case table

The table is the volume's, and every comparison and every name hash goes through it, for every UTF-16 unit, because that is what exFAT's case-insensitivity means; ASCII folding is not enough. At mount the table is decompressed once through the cache and checked: the runs must expand to exactly 65,536 mappings with no run of zero length, no truncated run and no run past the end, and its checksum over the stored bytes must match the entry. A table whose stored form is at most 16 KiB is kept per mount in that form and up-cased by walking its runs, which is a few thousand bytes per unit and is spent only on names. A larger table is not held: an uncompressed one, which is exactly 128 KiB and passes the structural check, is indexed through the cache at unit times two; a compressed one larger than 16 KiB is streamed from its beginning through the cache for every up-casing, which is correct and slow, and the record says which form the mount has.

### Entry sets, names and chains

A file is a set: the file entry, then exactly one stream extension in the second position, then one name entry per fifteen units, then any benign secondaries. The set is checked before it is trusted: `SecondaryCount` equals one plus the name entries plus the benign secondaries; `NameLength` is at least one and at most 255 and covers exactly the name entries; the set checksum over every byte of every entry but its own two matches; `ValidDataLength` is at most `DataLength`; a stream with `AllocationPossible` clear has a first cluster of zero and a length of zero; one with it set and a nonzero length has a first cluster in the heap; a `NoFatChain` stream's length fits its contiguous run in the heap; and the name hash equals the hash of the up-cased name. A set that fails any of these is skipped with a record, and an in-use set that fails is a fault for the dirty-volume check. A set may cross a sector and a cluster boundary, and the reader follows the directory's chain across both. Benign secondaries are kept and counted. A vendor allocation entry, type 0xE1, names clusters: its first cluster and length are read so that the walk marks them; its contents are never interpreted and never rewritten. A vendor extension, 0xE0, is kept as bytes.

A stream with `NoFatChain` set is contiguous and its FAT is never read; one without it follows the FAT, where the end of a chain is 0xFFFFFFFF, a bad cluster is 0xFFFFFFF7, and a value outside the heap ends the chain with `EIO`, as on FAT32, under the same bound. Bytes past `ValidDataLength` and up to `DataLength` read as zeros.

Lookup compares by hash first and then by up-cased units. A name of 255 units that does not fit the kernel's 255-byte component is unreachable by path, as on FAT32, and the Readme says so; the walk and `readdir` handle it without going through the component buffer.

Timestamps are decoded from their bit fields with the year based at 1980, the 10 ms increment added where the field exists, and the UTC offset applied when its valid bit is set. A timestamp without a valid offset was written by something that did not record its zone; it is reported as if it were UTC, which is the same approximation FAT32 makes, and the Readme calls it that rather than a conversion.

### Attributes

As FAT32: the mount's `uid`, `gid` and `umask`, the read-only attribute as the write bits, no links, no owners.

## Decisions on writing

### The dirty bit

`VolumeDirty`, bit 1 of the flags at offset 106, is set with `bcache_write` during the construction of a writable mount, after the dirty-volume check has run or been found unnecessary and before the mount is published, and stays set for the writable life of the mount, as Linux does. `prepare_unmount` clears it last: after `sync`, after every data and metadata write that can fail has succeeded, and after `bcache_sync` has reported nothing; if the clear itself fails, the mount goes read-only and stays marked dirty. The boot checksum is not touched, because bytes 106 and 107 are excluded from it, which is why the specification excludes them. The flags and the percent in use are written to the main boot sector only; the backup region is never written. The percent in use is written by the periodic sync and at unmount with the value the driver keeps.

### The order of writes

FAT32's rule with the bitmap in the FAT's place: a cluster's bitmap bit is set and its chain written before any entry refers to it; data is written and synced before a size is published; a set is unpublished before its clusters are freed, and the clusters of a node that a description holds are freed at its last `put` and not before; a truncation shortens the stream before it frees the tail; a directory grows by a zeroed, chained cluster before an entry is written into it. Every metadata write is `bcache_write`, in that order.

### The set protocols

A set is made to exist or to stop existing by one bit, the in-use bit of its file entry, and every protocol is arranged so that the reachable file is never invisible at a stop.

Creation writes the name entries and any benign secondaries, then the stream extension, then the file entry last with its in-use bit set and its checksum over the whole; a stop before the last write leaves secondaries with no primary, which every reader skips and the check frees. When a set's entries lie in two sectors, the sector without the file entry is written first. A creation test places the file and stream entries in one sector and across a boundary, and requires the end-of-directory marker after the set to be preserved at every stop.

A change that touches only the file entry and the stream extension, which is every size change, time change, `ValidDataLength` change and `NoFatChain` conversion, is written as one sector write when those two entries share a sector, which is the atomic unit the guarantee is built on; the file entry's checksum is recomputed over the new bytes and written in the same sector. Every set this driver creates has those two entries in one sector: when the free run in a directory would put the file entry in a sector's last slot, an inactive entry fills that slot and the set begins in the next sector, so the straddling case arises only on sets other writers made.

A change to a straddling set is two sector writes: the stream extension's sector first, the file entry's sector with the new checksum last. The stop between them leaves an in-use set whose checksum does not match but whose every other invariant holds, and the check below repairs exactly that by rewriting the checksum, which is what `fsck.exfat` does for the same state. A torn sector that happens to leave every other invariant holding would be repaired the same way, and that is the undetectable case FAT32 has too, written down as such. There is no relocation of a set: the first draft had one, and its marker could not be told from a cross-link some other writer had left, so it is gone.

A rename within one directory that keeps the entry count rewrites the name entries under the same two-write rule, name sectors first and the file entry's sector last. A rename that changes the entry count, a rename across directories, and one that replaces a destination: the destination's secondaries and an inactive file entry are written at the destination first; a replaced destination file is unpublished, its clusters kept for its holders as unlink does; the source file entry is written inactive; the destination file entry is published last. exFAT directories have no dot entries, so a moved directory needs nothing rewritten. A stop before the source is unpublished leaves inactive entries at the destination, which the check frees; a stop after it leaves a nameless allocated chain with its data intact, which the check counts and keeps, and a host checker recovers, as on FAT32. That is the one way a crash costs a name here. The node pool rekeys the source node to the destination in one step, detaches a replaced destination node so that it stays held and nameless until its last `put`, and advances the generation, as milestone 37 requires.

Removal writes the file entry inactive, which is the barrier; then the secondaries inactive; then, at the node's last `put`, the FAT chain and the bitmap bits.

### `NoFatChain`, `ValidDataLength` and allocation

Every file and directory this driver creates has a FAT chain and `NoFatChain` clear, which every reader accepts. A contiguous file or directory it extends has its existing run written into the FAT first, then its stream entry changed under the protocol above with the flag clear, and only then grows.

`ValidDataLength` is at most `DataLength` always. For everything this driver writes it equals `DataLength`, so nothing written here has an uninitialised tail. A change that writes no data preserves it as found. A shrink sets it to the smaller of its old value and the new length. A growth by `truncate` leaves it unchanged, so the new range reads as zeros without a write. A write beyond it zero-fills and syncs the whole gap first, including the part inside the last allocated cluster, and publishes the new lengths after.

`AllocationPossible` is set on every stream this driver writes that has clusters and clear on one that has none, as the specification says.

The free count is the bitmap's zero bits, counted once at the first need and kept. A cluster is allocated first-fit in the bitmap from the same hint as FAT32, the cluster after a file's last, so that a file written in one go is contiguous. `ENOSPC` is decided under the mount's lock before anything is written.

### Names

The checks are FAT32's, with exFAT's own forbidden set, and the 255-unit limit is the format's own. There are no short names and no aliases; the name is the name. The hash is computed over the up-cased name with the specification's function and written into the stream extension, and two names that up-case alike in one directory are one name.

### Timestamps

Written as UTC with the offset byte valid and zero: creation once, modification at every write with its 10 ms field, and access never advanced, as on FAT32 and KRFFS, rounded down to the format's two seconds and ten milliseconds.

### A dirty volume at mount

The check is FAT32's with the bitmap for the FAT, and it runs under the global walk lock on a volume whose dirty bit is set, before the mount is published. A clean volume is trusted as every other driver trusts it; the check is for what a crash left, this driver's or another's.

The walk is seeded with the root's chain, the bitmap's extent and the up-case table's extent, checked disjoint and marked allocated. Then every set the tree reaches, in every directory, is checked as the reader checks it and walked: a contiguous stream counted by length, a chained one followed, a vendor allocation's clusters marked. A cluster reached twice, a chain that leaves the heap or loops, a stream shorter than its length, a reached cluster whose bitmap bit is clear, an in-use set that fails any check other than its checksum, a critical entry this driver does not know, and two names that up-case alike in one directory are faults. An in-use set whose only failing check is its checksum is the state a straddling update leaves at its stop, and it is repaired, not faulted. Then the volume's bitmap is compared with the walk's, both ways: a bit set that nothing reached is lost, and a chain longer than its stream is surplus.

Three repairs are made, each write a stop in the recursive matrix, in this order: a set failing only its checksum has its checksum rewritten; orphan secondaries are written inactive; and surplus tails are detached at the last cluster the length justifies and then freed, since those clusters were never published. Lost clusters are counted, recorded and left allocated, because a nameless allocated chain after a crash may be a file a relocation or a rename had unpublished and not yet republished, and its bytes are the only copy; a host checker recovers such chains into files, and this driver does not. Any fault mounts read-only with the first one named. The 2^20-cluster bound is the shared bitmap: at exFAT's default 128 KiB clusters above 32 GB it covers 128 GB, and at the 32 KiB it uses below, 32 GB; a dirty volume beyond it mounts read-only and says why.

## What the two drivers share

`fatcommon.c` holds what FAT32 and exFAT both need and neither owns: the UTF-8 and UTF-16 conversion, the forbidden-character checks, the DOS timestamp bit fields, the dirty-volume walk's bitmap and its lock, the allocation hint, and the contiguous-run bookkeeping. Milestone 37 wrote those inside `fat32.c`; the first phase here moves them without changing them and the FAT32 host tests are what say nothing moved wrongly. Everything on disk stays in `fat32.c` and `exfat.c`.

## Memory and limits

| Limit | Value |
|-------|-------|
| Up-case table held per mount | 16 KiB stored form; a larger table is read through the cache |
| Entry set | 19 critical entries plus benign secondaries, at most 255 entries, assembled per call |
| Name | 255 UTF-16 units, the format's limit; 255 bytes of UTF-8 reachable by path, the kernel's |
| Dirty-volume walk | the shared 2^20-bit bitmap: 128 GB at 128 KiB clusters |
| Logical sector | 512 bytes; any other valid size is refused by name |
| Largest cluster | 32 MiB, the format's limit |
| Largest file | 2^64 bytes on the format; 4 GiB less one reachable here, because the kernel's file offset is 32 bits, answered with `EFBIG` on growth |

## What stays out

TexFAT, a second FAT and a second bitmap: read-only. Vendor entries: kept and marked, never made, never interpreted. Writing the backup boot region: never. Logical sectors other than 512 bytes: refused by name, and the Readme says which real media that is. A hardware proof and volumes from Windows, cameras and phones: milestone 40, on the first stick. A file above 4 GiB: the kernel's offset is 32 bits, milestone 36's decision, stated as a limit. Repair beyond the three safe cases: no.

## Decisions on tests and tooling

* `tests/host/mkexfatfixture.c` is new: an exFAT builder from the specification with a `--check` reader, the pair `mkfatfixture` is for FAT32. It writes both boot regions with their checksums, the bitmap, the recommended up-case table compressed with its checksum, an optional label, a GUID entry, a vendor extension and a vendor allocation entry, files with and without `NoFatChain`, a file with `ValidDataLength` short of `DataLength`, an empty file with no clusters, a name across several entries, sets crossing a sector and a cluster boundary, a set whose file and stream entries straddle a sector, a directory of two levels, and the malformed variants, each breaking one rule and each with a correct set checksum where the rule is not the checksum: a bad main boot checksum with a good backup, a bitmap shorter than the cluster count needs, a good main with a bad backup, two regions that disagree in geometry, a FAT too short for the cluster count, a FAT offset inside the boot regions, a bad up-case checksum, a truncated up-case run, a compressed up-case table over 16 KiB, an uncompressed up-case table, a set with a bad checksum, a wrong `SecondaryCount`, a wrong `NameLength`, a false name hash, `ValidDataLength` past `DataLength`, `AllocationPossible` clear with a cluster, a chain leaving the heap, a loop, a stream shorter than its length, a reached cluster with its bit clear, a lost bit, a surplus chain, a bitmap or up-case extent overlapping a file, a second bitmap entry, `MediaFailure` set, a 4 KiB logical sector, a timestamp without a valid offset, an unknown critical entry, and a dirty clean tree. Its `--check` walks a volume as the kernel's check does, compares the bitmap both ways, and names every fault.
* External images: phase 1 formats a volume with `newfs_exfat`, fills it through the host's own exFAT driver, which on this host is `hdiutil attach` and on Debian a mount, with a recorded manifest of names, sizes, hashes and times, and records its boot sector, its up-case entry and one set as hex in `notes.md`; the suite makes such a volume where the host has both tools, and on a Debian host `mkfs.exfat` is used the same way. Those are the images the reader must pass before the writer starts, against the manifest, so the proof is not vacuous.
* The external checker's expected answers are two: on a volume a completed operation left, clean; on a volume a stop in the matrix left, the diagnosis its state implies, lost chains or duplicate sets, read without modifying, and then clean after the checker has repaired a copy. A matrix image is never required to be clean at once.
* `tests/host/exfat_hosttest.c`: reading every fixture; every write sequence FAT32's test has, plus an open file unlinked and then written through while another file is created and extended, a file with a short `ValidDataLength` shrunk, grown by `truncate` and written past its valid length, a set change on a straddling set, and each rename kind; each read back through the driver and through `--check`; the interruption matrix with the content oracle, which requires every file that existed before a stop to be intact after it, recursive over the repairs; the dirty-volume rows; the set protocols observed on the bytes at every stop.
* `tests/host/fatname_hosttest.c` gains the exFAT rows: up-casing through a compressed table, through a large compressed table streamed, and through an uncompressed table indexed; the run boundaries and U+FFFF; the hash against values computed by hand; and two names that up-case alike.
* `tests/host/fat32_hosttest.c` runs unchanged after the move to `fatcommon.c`, which is the proof of the move.
* `tests/system/suites/exfat.sh`, `tests/run exfat`, core tier: an exFAT partition on the suite's disk built by `mkexfatfixture`. A probe writes, renames, removes and reads through it; the machine reboots and reads it back; the host reads the volume with `--check` and, where `fsck_exfat` or `fsck.exfat` is installed, runs it and requires clean. A crash boot with its repair record and its lost count. A `rach2.blockfail` boot. A BusyBox boot with `mount -t exfat` and the shell commands, and a `newfs_exfat` or `mkfs.exfat` volume mounted and read where the host has the tool.
* `tools/setup/doctor.sh` lists `exfatprogs` as optional beside `dosfstools`.

## Documentation

* `rach2/sys/fs/exfat.Readme.md` is new: why it exists, the volume check and the two boot regions, the root's objects, the up-case table and its three forms, entry sets and their invariants, the set protocols, the one-sector unit and the two-write rule, chains and `NoFatChain`, `ValidDataLength`, the dirty bit, allocation, names, timestamps and the offset-less approximation, the dirty-volume check and its three repairs and its one count, the limits and the named refusals, and the verification.
* `rach2/sys/fs/fat32.Readme.md`: what moved to `fatcommon.c`.
* `rach2/sys/fs/vfs.Readme.md`: the second type in the table.
* `rach2/sys/fs/synthfs.Readme.md`: `exfat` in `/proc/filesystems`.
* `rach2/Readme.md`: the file systems the kernel mounts, the limits.
* `Readme.md`: the system paragraph.
* `tests/Readme.md`, `tests/host/Readme.md`, `tests/system/Readme.md`, `tools/Readme.md`.
* `dev-log/38-exfat/Readme.md` and the `dev-log/Readme.md` entry, in phase 7.

## How the work is kept track of

`progress.md`, `current.md`, `notes.md` and `hardware.md` beside this plan, as before, and the one-line report `M38: phase N of 7; X of Y items done; HEAD abc1234; tree clean|dirty`.

## Standing rules for the run

* Work directly. No delegate writes code, documents or tests; delegation is independent review and proofreading only, in units of at most six files and ten minutes, at most five running at once, none starting another, every delegate reporting findings while this agent makes every change, and every delegate on Codex or a model of Opus 5's strength or better, because a weaker model's judgement of code is worth less than the time it saves. The rounds are fixed: one gate round after phase 3, where the reader must hold before the writer starts, and one round before the sweep, plus one focused re-review for a fix that changed a contract; the units are listed when the round starts. Before any round, this agent reads the whole diff against the plan and the owning Readmes, checks bounds, error paths, lifetimes, locking, the build entries and redundant code, and assembles the test and mutation results, so that the round confirms rather than discovers.
* Commit in scoped units in the house style, on the branch, no attribution, never push.
* Gates, chosen for the ten-core machine and a run that should not wait on itself: a small commit runs the rows or the smallest suite that covers it; each phase ends with `tests/run style`, `tests/run host` and the affected subsystem suites, once; a new or changed check is proved by its mutation when it is written, once, through the restoring script, and a passing mutation is not rerun; the full set runs once on the milestone's final tree and again only after a fix that changed an executable, the build or a test.
* Before the review rounds, every document and comment the milestone touched is read against the register rule in `CLAUDE.md` and `prosecheck` runs; one review unit is a proofread of the reader-facing documents and comments, told to reject the banned terms and the contextual shorthand `CLAUDE.md` names, reversals and fragments written for effect, intentions given to code or hardware, vague verbs where a concrete one exists, body-narrating comments, headers that repeat Readmes, and documentation that records a phase instead of the contract.
* A test is proved by making it fail. A mutation is driven from a script that restores the source in a trap and rebuilds before the verdict is believed.
* A change that alters a contract rewrites the owning Readme in the same commit.
* A change that makes something redundant removes it in the same commit. The tree after this milestone is not larger than the milestone's own additions.
* The writer does not start until phase 3's gate has passed on every external image the host can make.
* Nothing touches the Lenovo in this milestone.
* Nothing is deferred and nothing is left as a note for the owner. A thing discovered mid-run, whether it belongs to this milestone or not, goes into `notes.md` with the phase that will fix it, and it is fixed in this milestone, before the review rounds, with Codex's opinion taken as one review unit where the call is close; the sweep closes with `notes.md` empty. The two things that are not deferrals: a check that needs the machine goes onto milestone 43's list by design, and a change that needs a disk write on the machine goes there with it. Everything else is finished here.

## The phases

### Phase 1: arrival and the shared code

Read `AGENTS.md`, `CLAUDE.md` and this plan in full. Require a clean tree and record `git log --oneline -12`. Create the four tracking files in the first commit. List the files each review checkpoint will cover and fix the units.

Move what the two drivers share out of `fat32.c` into `fatcommon.c`, unchanged, and run every FAT32 test. Then read one `newfs_exfat` volume from this host with a small reader that becomes `mkexfatfixture --check`: its boot regions, its up-case entry and table form, its bitmap entry, one entry set, and its timestamps, recorded in `notes.md` as hex beside the specification's values.

Proof: the FAT32 host and guest suites pass unchanged after the move; the reader's record of the `newfs_exfat` volume matches the specification field by field.

### Phase 2: the fixture and the reader

`mkexfatfixture.c` in full, with `--check`, and its own self-test: every well-formed volume it writes passes `--check` and the host's checker; every malformed variant fails `--check` by the rule it breaks; the `newfs_exfat` volume passes `--check`.

Proof: the self-test; `fsck_exfat` clean on the well-formed volumes on this host.

Mutations, in the builder: a set checksum computed over its own bytes; a bitmap that starts at cluster 0; an up-case checksum computed over the expanded mappings rather than the stored bytes; a name hash over the name as written rather than up-cased. Each must fail exactly its `--check` row.

### Phase 3: the reader in the kernel, and the gate

`exfat.c` for reading: the volume check with both boot regions, the root's objects, the up-case table in its three forms, sets and their invariants, names, chains, `NoFatChain`, `ValidDataLength`, timestamps, `readdir`, `lookup`, `read`, `statfs`, and the `exfat` type. `exfat_hosttest.c`'s read rows over every fixture.

Proof: every well-formed fixture reads back what the builder wrote; every malformed one is refused at mount, skipped at the set, or fails at the file it damages, by name; the `newfs_exfat` volume and, where the host has it, the `mkfs.exfat` volume, each filled through the host's own driver, mount and list every file in their manifests with the right bytes and times. That last proof is the gate: phase 4 does not begin until it passes on every external image this host and a Debian host can make.

Mutations: skip the boot checksum; use the main region when it fails; accept a backup that disagrees; skip the set checksum; trust a false name hash; up-case ASCII only; index a compressed table directly; read a `NoFatChain` stream through a poisoned FAT; treat a stream's tail past `ValidDataLength` as data; apply an invalid UTC offset; accept a short FAT. Each must fail exactly its row.

### Phase 4: the writer

The dirty bit at construction, allocation and the free count, the order of writes, the set protocols with the one-sector unit and the relocation, the `NoFatChain` conversion, `ValidDataLength` in every transition, `AllocationPossible`, and every mutating operation with the deferred reclamation. The write rows and the interruption matrix with the content oracle.

Proof: every sequence reads back through the driver and `--check`; every stop passes the walk, the reader and the oracle with every pre-existing file intact and reachable by its name after the check has run, except the stop after a rename unpublished its source, where the oracle requires the chain intact and counted; the dirty bit is on the bytes from mount to clean unmount and off after; the checker on this host gives its expected answer on every volume the matrix leaves and is clean after repairing a copy.

Mutations: publish a file entry before its name entries; write a straddling set's file entry before its stream; place a new set across a sector boundary; change a frozen directory; clear `NoFatChain` before the run is in the FAT; raise `ValidDataLength` over an unzeroed gap; preserve `ValidDataLength` through a shrink; allocate without setting the bitmap bit; free a held node's clusters at unlink; free bitmap bits before the file entry is inactive; hash a name without up-casing it; write the flags into the backup boot sector; set the dirty bit at the first write instead of at mount. Each must fail exactly its check.

### Phase 5: the dirty-volume check

The seeded walk for exFAT, the bitmap comparison both ways, the three repairs and the one count, the recursive matrix over the repairs, and the dirty-volume rows.

Proof: a dirty volume with a clean tree mounts writable with its bit still set and the bit clears only after a clean unmount; one with a set failing only its checksum has it rewritten and the file reachable; one with orphan secondaries or a surplus chain is repaired with the record; one with a lost chain mounts writable with the count and the chain's bytes intact; one with a cross-link, a short stream, a reached-but-free cluster, a bad in-use set, an unknown critical entry or an overlapping system extent mounts read-only naming the fault; the bitmap and up-case bits are never counted lost.

Mutations: free a lost cluster, which the intact-bytes row must catch; rewrite the checksum of a set that also fails its name hash; treat a checksum-only failure as a fault; free a surplus tail before detaching it; skip the reached-but-free comparison; walk without seeding the system objects; treat a bad in-use set as skippable; skip the up-case comparison for duplicate names. Each must fail exactly its check.

### Phase 6: the suite

`tests/run exfat` in full. The checker on this host over every volume the suite and the matrix wrote.

Proof: the suite passes, and so does everything else; the checker is clean.

### Phase 7: the record and the sweep

As in milestones 36 and 37, and as large. The documents, every one listed above, read against the code, with a Codex read of the reader-facing ones. The code: nothing duplicated between the two drivers that `fatcommon.c` should hold; no helper with one caller. The build: every rule, manifest line, fixture and tool listed in `notes.md` with its reason; no `build_*.sh`; `guestcheck` and `tests/run selfbuild` pass. The tests: the tier decision measured; a boot that proves what another proves removed; the matrix and mutation scripts in `notes.md`; the full set once on the final tree with no machine left running, rerun only after a later fix, and `make test` timed against milestone 37. The record: `notes.md` closed line by line, `dev-log/38-exfat/Readme.md` with its trust statement, which names what only a stick and a Windows-written volume can prove, and the `dev-log/Readme.md` entry in the same commit. The tree is clean and nothing is pushed.

## Risks, in expected order

* An exFAT writer this driver has not seen. The fixture models the specification and `newfs_exfat` and `mkfs.exfat` are the external writers; the reader's refusals are by name so that a Windows, camera or phone volume in milestone 40 fails as a named case and not as silence.
* The builder and its reader sharing a misreading. The external images and the external checker are the answer, and the gate is where they are required.
* The checksum repair accepting a torn sector that happens to pass every other invariant. It is the same undetectable class FAT32 has, it is written down, and the external checker makes the same choice.
* The up-case table streamed through the cache on a volume with a large compressed table being slow. It is correct, it is rare, and the record says which form the mount has.
* The move to `fatcommon.c` changing FAT32 behaviour. The FAT32 tests run unchanged before anything exFAT exists, in phase 1, and that is the whole proof.

## Recovery after a compaction

Read `AGENTS.md`, `CLAUDE.md`, this plan's decisions and the current phase, `progress.md`, `current.md`, `notes.md` and `hardware.md`. Run `git status --short --branch` and `git log --oneline -15`, inspect every uncommitted path, and resume the current phase's first unfinished item. Report the one line.

## The goal prompt

`Run milestone 38, exFAT, from dev-log/38-exfat/plan.md.`
