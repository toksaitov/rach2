# Milestone 37: writing FAT32

## The goal

FAT32 gains writing. Files are created, written, extended, truncated, renamed and removed, directories made and removed, with long names, both FATs, FSInfo and the dirty flag kept the way Windows and Linux keep them. The buffer cache gets the write rules milestone 36 left to the first writer, and with them the one property this milestone is for: a power cut at any point between two completed sector writes leaves a volume that a checker calls consistent, with clusters at worst leaked and never shared.

The milestone is complete when a volume written by Rach2 in QEMU is read back identically by mtools and, where a host has it, passes `fsck.fat -n` clean; when a volume stopped after every one of the sector writes of a scripted sequence is consistent at every stop; when the same volume survives a reboot; and when a volume that something else left dirty is checked at mount and comes back writable. The hardware proof is a FAT32 partition of the Lenovo's own disk, if the owner approves creating it; if not, the hardware proof waits for the first USB stick in milestone 39 and this milestone closes on QEMU alone, and that is written here so that nobody has to decide it under pressure.

exFAT is milestone 38, on the same cache and the same rules. It was in this plan's first draft and was taken out by review: a first ordered write path, a full FAT32 writer, and a second file system read and write are two milestones, and combining them made the failures of one indistinguishable from the other's. USB storage is milestone 39, hubs 40, ext2 41.

This plan is the authority for the run. A conflict between this plan and the current tree is resolved by the decisions here; a conflict between this plan and `CLAUDE.md` is resolved by `CLAUDE.md`. The plan was revised twice after independent review. The first revision replaced the ordering model, the locking, the dirty-flag rules, the error rules, the mount-time check, the name rule and the hardware procedure. The second fixed the long-name write order, the empty-file extend, the mirror FAT after a crash, the order of the repairs themselves, the lock ownership, and the sequence on the machine.

## What milestone 36 leaves in place, and this plan relies on

* The file-system interface: `vfs_type` with a constructor, `vfs_ops` with `lookup`, `hold`, `put`, `getattr`, `setattr`, `open`, `release`, `read`, `write`, `truncate`, `fsync`, `readdir`, `create`, `unlink`, `mkdir`, `rmdir`, `rename`, `link`, `readlink`, `statfs`, `sync`, `prepare_unmount` and `unmount`. A `NULL` operation answers `EROFS` on a read-only mount. The common layer does the walk, the permissions, the cross-mount checks, the description lock, the mount namespace lock, and the lock order: table, then a mount's namespace lock, then a description, then buffers in ascending block order with at most four held.
* The FAT32 driver, read-only: the volume check, the active FAT, the 28-bit entries, directory reading with long names, the node pool with identities and a generation, an unlinked node held until its last `put`, and a `mount(2)` without `MS_RDONLY` answered with `EROFS`.
* The buffer cache, used to read: `bcache_get`, `bcache_release`, `bcache_dirty`, `bcache_sync` and `bcache_invalidate` exist; the rules for writing were left to this milestone, with their host rows.
* `block_request` with the request pool, the quarantine, and the lag device `rach2.blocklag=N`, which sleeps the caller on every request to a partition node or a mount. Claims by sector range, so a mounted partition and a raw writer exclude each other.
* `mkfatfixture` writes FAT32 volumes from the specification. `bootctl_hosttest` has the shape of an interruption matrix: a fake write callback that can drop a sector, stop the run after any chosen write, or tear a sector, with the properties asserted on the bytes at every stop.
* The kernel stamps times from `rtc_realtime`. The kernel does not preempt inside a syscall; a sleep is the only place two processes interleave, and there are no kernel threads. The three inits are one source, `utils/init.c`.
* On this development host: mtools with `mcopy`, `mdel`, `mmd`, `mattrib` and `mdir`, and no `fsck.fat`. On Debian: `dosfstools`. The checker is not a required tool and no suite depends on it.
* The Lenovo's disk has three partitions and a free fourth MBR entry, with about 250 GB unused after the recovery partition. Writing sector zero is a whole-disk write and belongs to the recovery kernel and `reflash`, with the owner's approval at the time. `tests/run sealed` boots the sealed disk image in QEMU through all of its boot paths, which is where any change to that disk is rehearsed first.

## Decisions on how writes reach the disk

### Metadata is written through, data is written back

The first draft dirtied buffers in a careful order and let the cache write them back in block order, which is no order at all. This plan takes the rule the Berkeley Fast File System took before journals existed and that every teaching kernel since Lions has used: every metadata write is synchronous, and every data write is not.

`bcache_write(buf)` is new. It writes one buffer through `block_request` at once, waits for it, and returns the result; the buffer is clean afterwards. The FAT32 driver uses it for every FAT sector, every directory sector, the boot sector and FSInfo, in the order the next section gives, so that the order in which the driver writes metadata is the order in which it reaches the disk. A file's data goes through `bcache_dirty` and is written back by eviction, by `fsync`, by `sync`, and by the periodic sync. Where a data write must reach the disk before a metadata write that describes it, the driver calls `bcache_sync` on the device first.

The cost is one disk write per metadata change, which on a flash drive over USB will be milliseconds each. That is the price of consistency without a journal, and it is paid by every FAT implementation that does not keep one.

### The rest of the cache's write rules

A dirty victim is written back before its buffer is reused. A write that fails, through `bcache_write` or through writeback, is recorded against the device: the mount that owns the device goes read-only with a record naming the sector, every later write on it answers `EROFS`, and `/proc/mounts` shows `ro` in its options. There is no finer policy, because a write-back cache cannot return a later failure to the `write(2)` that dirtied the buffer, and a volume whose disk has started refusing writes is not a volume to keep writing. `bcache_sync(dev)` writes the device's remaining dirty buffers in block order, since among data buffers order does not matter, and returns the first error after trying them all. `fsync(fd)` is `bcache_sync` on the file's device. `sync(2)` runs every mount's `sync` and then every device's `bcache_sync`.

The fake block device in the host tests gains the fault injector `bootctl_hosttest` has: stop after the Nth write, tear a sector into a prefix of the new bytes over the old, and report success while dropping a sector. The lag device gains `rach2.blockfail=N`, which fails the Nth write to it with `-EIO`.

### What the guarantee covers

The property holds between completed sector writes: a crash after any whole sector has reached the disk and before the next leaves a volume the walk below calls consistent. A sector torn in the middle of a write, or a write the disk acknowledged and lost, is outside it: FAT32 has no checksum on a directory sector and no transaction record, so such a sector can carry an entry that looks valid and is not. The walk finds what it can, a chain that is wrong or an entry that names a free cluster, and a torn entry that passes every check is an undetectable corruption, written down as such in the Readme. The matrix proves the first property at every stop; the tear rows prove only that a torn sector is either detected at mount or documented.

### The periodic sync

Data is written back every thirty seconds by a process, as V7's `update` did: `init` forks a child at boot that sleeps thirty seconds and calls `sync(2)`, on every image. The kernel does not do it, because the timer cannot sleep and this kernel has no thread of its own to give the work to, and a process that leaves a syscall is the wrong process to charge. The periodic sync writes data and FSInfo and never clears the dirty flag.

## Decisions on FAT32 writing

### What a write changes on the volume

A file's data lives in a chain of clusters; its directory entry records the first cluster and the size; the FAT records the chain; FSInfo records a free count and a next-free hint that no reader trusts; and the volume has a dirty flag in two places, which Windows and Linux mark at the first write and clear at a clean unmount: bit 27 of FAT entry 1, where a cleared bit means dirty, and bit 0 of the byte at offset 65 of the boot sector, where a set bit means dirty. Both are written, and either one in the dirty state is read as dirty. With mirroring on, every FAT write goes to both copies, the active one first; with mirroring off, to the active one only.

### One lock

Every operation that changes the volume, which is `write`, `truncate`, `create`, `unlink`, `mkdir`, `rmdir`, `rename`, `setattr`, allocation, `sync`, the dirty-flag transitions, the mount-time check and the deferred reclamation a last `put` performs, takes the mount's namespace lock from milestone 36 for its whole length, so that two writers, or a writer and a sync, never interleave across the sleeps that `bcache_write` makes. Milestone 36 gave that lock to the namespace operations only, with `write` under the description lock; that was enough while nothing wrote. The common layer owns both acquisitions and takes them in the order milestone 36 fixed: the mount's lock first, then the description's, then the global walk lock where the check runs, then buffers. `sync` as an operation takes the mount's lock and calls a helper that assumes it held; `prepare_unmount` calls the same helper under the lock it already holds, and `unmount` runs only after a successful `prepare_unmount`. Every mutating entry point asserts that it holds the mount's lock, which is what makes a missing lock a deterministic failure rather than a scheduling accident.

### The order of writes

The rule: a crash may leak clusters and may never cross-link them, and an entry never names a cluster that is free. So, with every metadata write synchronous:

* Create: the entry is written free of clusters, with size zero and first cluster zero. A create that needs a new directory cluster is a directory growth first.
* Extend: the new clusters are chained in the FAT with an end-of-chain mark, disconnected from the file; the data is written and the device synced; the FAT entry of the old tail is pointed at the new run; the entry's size grows. A crash before the last step leaves a chain longer than the size, which the walk treats as surplus and frees. A file with no clusters yet has no old tail: its new run is allocated disconnected, its data written and synced, and then its first cluster and its size are published together in the one entry write.
* A write past the end zero-fills the clusters between the old end and the new data before the size grows over them, so the file never reads stale bytes.
* Truncate: the entry's size shrinks first, and its first cluster goes to zero when the size does; the retained tail gets an end-of-chain mark; the detached tail is freed last.
* Unlink: the entry is freed first, long-name entries after the short one; the chain is freed last, and only when no description holds the node. A held node keeps its chain until its last `put`, and a crash in between leaks the chain.
* Mkdir: the cluster is allocated and chained, zeroed, given `.` and `..`, and the parent's entry is written last. A crash before the last step leaks the cluster.
* Rmdir: the directory must hold nothing but `.` and `..` and must not be held by a description or a working directory, else `EBUSY`; then as unlink.
* Directory growth: the new cluster is zeroed and marked end-of-chain before the old tail is pointed at it, so the directory never shows stale entries.
* Long names: the long entries of a name precede its short entry, and a reader trusts only a long sequence whose short entry follows with the matching checksum. So a name is created long entries first and short entry last, and freed short entry first and long entries last; at every stop in between the long entries are orphans, which every reader skips and which the walk frees. When a name's entries span two sectors, the sector without the short entry is written first and the sector with it last, for creation and for freeing alike.
* Rename within one directory, when the new name takes the same number of entries: the entries are rewritten in place under the long-name rule above. Otherwise, and across directories: the destination is checked for room first; if a destination file exists, its entry is freed and its chain freed or left to its holders as in unlink; the source entry is freed; the `..` of a moved directory is rewritten; the new entry is written last. A crash between the freeing of the source and the writing of the destination leaves the chain leaked and nameless, which the walk reports and a checker recovers; Windows does it the other way and accepts the cross-link, and this driver keeps its one rule.

The node pool is not tied to a directory slot: a rename rekeys the source node to its new location in one step, detaches a replaced destination node from that location so that it stays held and nameless until its last `put`, and advances the location's generation, so a description that had either file open before the rename keeps the file it opened, and the rows below prove it.

### The dirty flag and FSInfo

The dirty flag is set before the first metadata write after a mount, with `bcache_write`, and stays set for the whole life of a writable mount. It is cleared only by `prepare_unmount`, after `sync` has written every dirty buffer and `bcache_sync` has reported nothing, and it is cleared in both places; if the second of the two writes fails after the first succeeded, the first is written back to dirty and the mount stays read-only rather than leaving a modified volume marked clean. FSInfo is written by the periodic sync and by `prepare_unmount` with the count and the hint the driver keeps; it is never read to decide anything.

### Allocation

The free count is computed once, by reading the active FAT through the cache, the first time it is needed, which is the first `statfs` or the first allocation, and is kept from then on. The FAT of a 32 GB volume with 32 KiB clusters is 4 MiB; that read costs a fraction of a second on AHCI and is paid once per mount.

A cluster is allocated first-fit from a hint. When a file grows, the hint is the cluster after its last one, so that a file written in one go is contiguous. When that cluster is taken, the search continues from it and wraps once. `ENOSPC` is decided before any FAT sector is written, under the mount's lock, so a decision is not overtaken by another writer.

### Names

A name is checked before anything is written: at most 255 bytes of UTF-8, converted to at most 255 UTF-16 units, none of the characters FAT forbids, no trailing dot or space, and neither `.` nor `..`. A name that fails answers `EINVAL`. A name of 255 non-ASCII characters is legal on FAT and does not fit the kernel's 255-byte component; it is unreachable here, and the Readme says so as a limit of this kernel and not of the format.

Every new file has a short entry, and some have long entries in front of it. The rule is the Windows NT rule, which Linux offers as `shortname=winnt`: when the name fits 8.3 with each of its two parts all upper or all lower case, only the short entry is written, with the case bits in the reserved byte recording the lower-case parts, which is how `hello.c` is stored and read back by Windows and Linux alike. Otherwise long entries carry the name, and the short entry holds an alias. The alias is this driver's own deterministic rule and is not claimed to match Windows, whose rule depends on the code page and on collision history: the first six characters of the name that are legal in a short name, upper-cased, with the rest dropped, then a tilde and the lowest number that makes the eleven raw bytes unique among the directory's short entries, then the first three legal characters of the last extension. A short name whose first byte would be `0xE5` is stored as `0x05`. The collision check reads the directory once per candidate.

A lookup folds ASCII case for both short and long names and compares other characters exactly, as the read-only driver does.

### Attributes and times

`setattr` accepts a time change for modification and access time and stores it in the entry; accepts a mode change only for the write bits, which set or clear the read-only attribute, and answers `EPERM` for any other mode bit, for `uid` and for `gid`, because the volume cannot store them, which is what Linux's vfat does. Creation time is written once at creation; modification time is stamped at every write, rounded down to FAT's two seconds; access time is never advanced by the driver, as KRFFS does and for the same reason.

### A dirty volume at mount

A volume whose dirty flag is set was written by something that did not unmount it. Linux mounts it read-write and says nothing; that is not good enough for a kernel that will also be the thing that wrote it. So a dirty volume is checked at mount before the entry is published, under one global walk lock taken after the mount's own lock, with one static bitmap of 2^20 bits shared by every mount.

The check reads the active FAT, which is authoritative because it is the copy written first, and walks every chain the directory tree reaches, root included, under the bitmap: a cluster reached twice is a cross-link; a chain that leaves the volume, exceeds the cluster count or loops is bad; a chain shorter than its file's size is bad; a directory whose `.` is not itself or whose `..` is not its parent is bad; two entries with one name in one directory is bad. Then it compares the whole active FAT with the bitmap: a cluster marked used that nothing reached is lost, and a chain longer than its file's size has a surplus. Then it decides.

Four states are what this driver's own order leaves behind after a crash: a surplus tail, a lost cluster, an orphan long-name entry, and a mirror FAT that lags the active one. Three are repaired, in an order that a second crash cannot make worse, and every repair write is a stop in the matrix. A surplus tail is detached before it is freed: the last cluster the size justifies gets an end-of-chain mark first, then the suffix is freed; those clusters were never published, so nothing is lost. Orphan long-name entries are freed. With mirroring on, every mirror is rewritten from the active FAT after the other repairs. A lost cluster is not freed. A nameless chain after a crash may be a file that a rename or a removal had unpublished and not yet finished with, and its bytes are the only copy; the check counts lost clusters, records the count, leaves them allocated, and mounts writable, because an allocated cluster nothing names endangers no file. That is the leak the guarantee allows, and a host checker recovers such chains into files, which this driver does not attempt. A FAT link that leads from an unreached cluster into a reached chain is a fault, since a chain nothing names then shares clusters with a file, and the mount goes read-only. The record says how many of each were repaired or counted. Anything else mounts read-only with a record naming the first fault, and the volume waits for a checker on another machine. A volume with more than 2^20 clusters is checked for nothing when dirty and mounts read-only with a record, which puts the supported size for recovery at 32 GB with 32 KiB clusters; a clean volume of any size the read-only driver accepts mounts read-write.

### Errors

A metadata write that fails moves the mount to read-only for the rest of its life, with a record; a data writeback that fails does the same, as the cache section says. A read that fails is `EIO` as in milestone 36. A mount that went read-only is unmounted and mounted again; there is no remount.

## Memory and limits

| Limit | Value |
|-------|-------|
| Dirty-volume walk bitmap | 128 KiB static, 2^20 clusters, shared, one walk at a time |
| Largest volume checked when dirty | 2^20 clusters: 32 GB at 32 KiB clusters |
| Name assembly | 512 bytes UTF-16, per call |
| Aliases tried for one long name | 999999 |
| Buffer cache | unchanged, 64 buffers |
| Periodic sync | every 30 seconds, by a process |
| Largest file | 4 GiB less one byte, the format's limit, answered with `EFBIG` |

## What stays out

exFAT: milestone 38. USB storage, a drive pulled while mounted, request cancellation on disconnect and the cache invalidation that goes with it: milestone 39. Repair beyond the two safe cases: no; the Readme names the host tools. Access-time updates, `chown`, and modes beyond the read-only attribute: refused, because the volume cannot store them. `remount`: not planned. Symbolic and hard links: the format has none. A journal or read-after-write verification: not planned, and the guarantee is stated to their absence.

## Decisions on tests and tooling

* `tests/host/bcache_hosttest.c` gains the write rows: `bcache_write` reaching the fake device before it returns and in call order; a dirty victim written before reuse; dirty buffers written once by sync; a failed write recorded against the device and the record read back; a torn write and a dropped write leaving the volume for the walk to judge, with the tear row's expected result being detection or read-only and never silent success.
* `mkfatfixture` gains `--check`: an independent walk that verifies a volume's structure, compares its two FATs, lists what it finds, and reports lost clusters, surplus, short chains, loops, bad `..` and duplicate names each by name, so that every write test reads back through a reader that is not the driver. It also gains fixtures for the walk: a lost chain whose bytes must survive the check, an overlong chain, a short chain, diverging mirrors, a loop, a wrong `..`, a duplicate name, and a valid FSInfo whose count and hint are wrong.
* `tests/host/fat32_hosttest.c` gains the write half. First the sequences: create, write, extend across a cluster, extend from an empty file, a write past the end, truncate, unlink, unlink of an open file with reads and writes through the description afterwards, mkdir, rmdir, rmdir of a held directory, rename in place within one sector and across two, rename across directories, rename over an existing file, rename over an open file with the old description checked to keep its file, rename of an open file, rename of a directory, a long name and its alias, an alias collision, a name that fails each check, `fsync` with its data on the image before it returns and `fsync` failing into a read-only mount, and a full volume; each read back through the driver, through `--check`, and through mtools where the sequence leaves a volume mtools can read. Then the interruption matrix: each sequence run once per physical write it makes, stopped after that write with the fake device recording the write order, the cache discarded, the driver mounted fresh on the image, and the volume judged three ways: by the walk and by `--check` for structure, leaks and surplus allowed, cross-links and short chains and entries naming free clusters never; and by a content oracle, with every free cluster poisoned beforehand, that every byte a published size covers is either a byte the sequence wrote or a zero. The matrix is recursive: the mount-time check's own repairs are stopped after every FAT sector they write and remounted again, and must converge. Then the dirty-volume rows over the fixtures above, with the flag checked on the bytes in both places after the first write, each place alone read as dirty, both cleared only after a clean unmount, and the rollback when the second clear fails; and the mount-time check's three answers: writable after repair, read-only with the fault named, read-only for size. The torn and dropped fixtures are two lists: the detectable ones must mount read-only with the fault named, and the undetectable ones are the documented cases and are asserted as such.
* `tests/host/fatname_hosttest.c`: the conversion between UTF-8 and UTF-16 both ways, the forbidden characters, trailing dots and spaces, the 8.3 fit rule and the case bits, alias generation and its uniqueness on the raw eleven bytes, the `0x05` escape, and the recorded cases where mtools and Windows agree with this rule and where they do not.
* `tests/vfsprobe.c` gains the row milestone 36 left here: a directory descriptor used after its directory is renamed.
* `tests/lagprobe.c` gains a row with two processes creating and extending files on one lagged volume at once, and one with a writer and a `sync` at once, read back consistent. The lock is proved by the assertions at every mutating entry rather than by scheduling: a mutation that drops the lock trips an assertion on the first mutation, deterministically.
* `tests/system/suites/fatwrite.sh`, `tests/run fatwrite`, core tier: a FAT32 partition on the suite's disk. A probe writes, renames, removes and reads through it; the machine reboots and reads it back; the host reads the volume with mtools and compares every file's bytes, and, where `fsck.fat` is installed, runs it with `-n` and requires clean. Then a crash boot: the harness kills the machine during a scripted write sequence, the machine boots again, the mount's check runs and the volume comes back writable with its repairs recorded. Then `rach2.blockfail=N`: a metadata write fails and the mount goes read-only with the record and `/proc/mounts` says so. Then a BusyBox boot: `cp`, `mv`, `rm`, `mkdir` and `cat` from the shell, with `df` before and after, and the `update` child seen in `ps`.
* `rach2-recovery-iso` is a new image target beside `rach2-metal-iso`: the recovery kernel and its RAM image on the same El Torito hybrid image, so that a stick can boot the machine into `reflash` without the disk. It is built by the existing ISO builder with a different kernel and module and nothing else.
* `tools/setup/doctor.sh` lists `dosfstools` as optional, and the suite says at its end whether it had the checker.
* `tests/system/accept/metal_accept.sh` gains the write check on the scratch partition, run only when that partition exists.

## Documentation

* `rach2/sys/fs/fat32.Readme.md`: the write half, the order of writes and why, the lock, allocation, names and aliases and what they do not promise, attributes, the dirty-volume check and its two repairs, errors, the guarantee and its edge, and the verification.
* `rach2/sys/fs/vfs.Readme.md`: `bcache_write`, the write rules, the failure record, `fsync` and `sync`, the periodic sync process, the fault injector, `rach2.blockfail`, and the lock change.
* `rach2/sys/block/Readme.md`: `rach2.blockfail`.
* `utils/Readme.md`: the `update` child of `init`.
* `rach2/Readme.md`: the file systems the kernel writes, the boot options, the limits.
* `Readme.md`: the system paragraph; the machine's scratch partition under "Updating the installed machine", if it exists.
* `tests/Readme.md`, `tests/host/Readme.md`, `tests/system/Readme.md`, `tools/Readme.md` for the recovery ISO target, and `Readme.md`'s image list.
* `dev-log/37-fatwrite/Readme.md` and the `dev-log/Readme.md` entry, in phase 7.

## How the work is kept track of

`progress.md`, `current.md`, `notes.md` and `hardware.md` beside this plan, as in milestone 36, and the one-line report `M37: phase N of 7; X of Y items done; HEAD abc1234; tree clean|dirty` after every phase, recovery and batch of commits.

## Standing rules for the run

* Work directly. The only delegation is an independent review, one reviewer per unit, at most six files, at most ten minutes. There are six review checkpoints, after phases 2, 3, 4, 5, 6 and 7, with the number of units fixed in phase 1 from the files each will cover, and one focused re-review for a fix that changes a contract.
* Commit in scoped units in the house style, on the branch, no attribution, never push.
* Every commit passes `tests/run style` and `tests/run host`. A commit that touches a subsystem runs that subsystem's suites singly. The whole set runs after phases 3 and 5 and at the end of phase 7.
* A test is proved by making it fail. A mutation is driven from a script that restores the source in a trap and rebuilds before the verdict is believed.
* A change that alters a contract rewrites the owning Readme in the same commit.
* A change that makes something redundant removes it in the same commit. The tree after this milestone is not larger than the milestone's own additions.
* On the Lenovo, the writes are the kernel installation through `bootctl` and, only with the owner's approval given when the values are presented, the fourth partition through recovery. Every other write goes to that partition. The boot, root and recovery partitions are hashed before and after, and recovery is the saved default from before the first disk write until the last check passes.
* Nothing is deferred. A thing discovered mid-run goes into `notes.md` with the phase that will do it, and phase 7 closes every line or records the owner's decision to drop it. The one decision this plan hands to the owner in advance is the partition, and both answers are already written.

## The phases

### Phase 1: arrival and inventory

Read `AGENTS.md`, `CLAUDE.md` and this plan in full. Require a clean tree and record `git log --oneline -12`. Create the four tracking files in the first commit. List the files each review checkpoint will cover and fix the units.

Give `mkfatfixture` its `--check` and its new fixtures, and prove `--check` against the malformed fixtures that already exist, so that the reader every later test relies on is trusted before the writer exists. Measure what mtools writes: format a volume, copy a lower-case name, a mixed-case name, a long name and a colliding pair into it, and record what `--check` shows, so that the name rule is compared with a second writer before it is coded. Write the proposal for the Lenovo's fourth partition into `hardware.md`: a 1 GiB FAT32 volume with 8 KiB clusters starting at the first 1 MiB boundary after the recovery partition, its MBR entry, and the exact procedure of phase 6.

Proof: `--check` refuses every malformed fixture by name and accepts every good one; the tables in `notes.md` and `hardware.md`.

### Phase 2: the cache writes

`bcache_write`, the write rules, the failure record and the read-only transition it causes, `fsync` and `sync` through the mounts, the `update` child in `init`, the fault injector in the host fake device, and `rach2.blockfail`. The `bcache_hosttest.c` write rows.

Proof: the rows pass; a guest boot with `rach2.blockfail` shows a failed write and a mount gone read-only; `ps` shows the updater and a dirtied file reaches the disk within thirty seconds with nobody calling `sync`.

Mutations: let `bcache_write` return before the device has the sector; reuse a dirty victim without writing it; keep a mount writable after a failed writeback; let the updater's interval pass without a sync. Each must fail exactly its row.

### Phase 3: the writer

The lock, allocation and the free count, the order of writes for every operation, the dirty flag and FSInfo, both FATs, the node pool's rename update and deferred reclamation, and the mount-time check with its two repairs. The `EROFS` refusal of a read-write mount is removed. The host sequences and the interruption matrix, with short names only.

Proof: every sequence reads back through the driver, through `--check` and through mtools; every stop in the matrix passes the walk; the dirty-volume rows give their three answers; a volume written in QEMU is read by mtools with identical bytes.

Mutations, each observable because metadata is written through in call order: write the entry before the chain; free the chain before the entry; grow the size before syncing the data, which the content oracle must catch; point the old tail at an unzeroed directory cluster; write only the first FAT on a mirrored volume; trust the fixture's wrong FSInfo; never set the dirty flag, checked on the bytes after the first write; clear it in the periodic sync; keep writing after a failed metadata write; reclaim an open file's chain at unlink; treat a surplus tail as a fault; treat a short chain as surplus; free a surplus tail before detaching it, which the recursive matrix must catch; free a lost cluster, which the rename crash row must catch by the bytes of the nameless file; drop the lock from `write`, which the entry assertion must catch. Each must fail exactly its check.

### Phase 4: long names and attributes

Long-name creation with the alias and its collision search, the name checks, `setattr` for times and the read-only attribute, and the `vfsprobe` row for a renamed directory. `fatname_hosttest.c`. The matrix rerun with long names in every sequence.

Proof: a name of every kind is written and read back through the driver, `--check` and mtools with the same long name, and with the alias this driver's rule gives; a forbidden name answers `EINVAL`; a mode change outside the write bits answers `EPERM`; the matrix passes with long-name entries spanning sectors.

Mutations: write an alias that collides on the raw bytes; write a long name without its checksum; accept a trailing space; store an `0xE5` first byte unescaped; let `chmod` change an execute bit; let a rename lose the node's location. Each must fail exactly its check.

### Phase 5: the suite

`tests/run fatwrite` in full: the probe, the reboot, the mtools comparison and the checker where present, the crash boot and its repair record, the `blockfail` boot, the BusyBox boot, and the `lagprobe` rows for two writers and for a writer and a sync. `tests/run all`.

Proof: the suite passes, and so does everything else.

Mutations: drop the mount's lock from `write`, which the two-writer row must catch; let the crash boot's mount skip the check, which the repair record must catch. Each must fail exactly its check.

### Phase 6: the machine

Rehearse first, on the sealed disk image in QEMU: the sector-zero edit, the image write, and all of the sealed disk's boot paths afterwards through `tests/run sealed`, so that the procedure has been run once before it touches the machine.

Then the Lenovo, over SSH, on mains power with suspend off. Set recovery as the saved default before anything is written. Install the kernel through `bootctl` as the approved write, boot it once through `next_entry`, and commit it. Present the partition proposal from `hardware.md` with the values read from the machine at that moment: the disk's identity and size, its current four entries, the proposed entry, and the commands; and wait for an approval that names the disk. If refused, `notes.md` records it, the hardware half is a read-only mount as in milestone 36, the ordinary root becomes the saved default again, and the milestone closes on QEMU.

If approved, one more thing comes first: a USB stick carrying the recovery kernel and its RAM image, built by a new `rach2-recovery-iso` target and written with `dd`, is booted on the machine and shown to reach `reflash`, so that a sector zero gone wrong can be restored without opening the case. Then: boot recovery from the disk; read sector zero and save it on the host and on that stick, with its hash; hash the boot, root and recovery partitions; on the host, produce the new sector zero by changing bytes 494 through 509 only, and verify that bytes 0 through 493 and 510 through 511 are identical to the original and that the entry's type, start, count, alignment, bound and non-overlap check against the live table; immediately before the write, read sector zero again and require its hash unchanged and the disk's identity the same; write it through `reflash` at offset zero, read it back, compare byte for byte, and on any mismatch write the saved original back at once; write an `mformat`-made image whose `BPB_HiddSec` equals the start sector and whose length fits the entry through `reflash` at the entry's offset, read the whole range back and compare its hash with the image; boot the ordinary root once through `next_entry`, with recovery still the default; run the write check through the shell and through `metal_accept.sh`: write, rename, remove, `sync`, reboot the same way, read back, pull the partition over SSH and read it with mtools and, where present, the checker. Hash the three partitions again; they must match. Only then does the ordinary root become the saved default again.

Proof: the rehearsal passes every boot path; the three hashes match; the machine's volume reads back identically on the host.

### Phase 7: the record and the sweep

As in milestone 36, and as large. The documents, every one listed above, read against the code, with a Codex read of the reader-facing ones. The code: the read-only driver's refusals that the write path now handles removed rather than bypassed; no helper with one caller; the description-lock-only `write` path of milestone 36 gone, not shadowed. The build: every rule, manifest line, fixture and tool listed in `notes.md` with its reason; no `build_*.sh`; `guestcheck` and `tests/run selfbuild` pass. The tests: the tier decision measured; a boot that proves what another proves removed; the matrix and mutation scripts in `notes.md`; the full set twice with no machine left running, and `make test` timed against milestone 36. The record: `notes.md` closed line by line, `dev-log/37-fatwrite/Readme.md` with its trust statement, and the `dev-log/Readme.md` entry in the same commit. The tree is clean and nothing is pushed.

## Risks, in expected order

* An order the matrix does not reach. The matrix stops after every physical write of every sequence, which is the whole space for one process under the lock; two processes interleave only at a sleep, the lock covers every mutation, and the two-writer row is the proof that it does.
* The synchronous metadata cost on the lag device making the suite slow. Every write is one request; a create is about four. Measured in phase 5 and written into the tier decision.
* The alias rule differing from Windows. It is this driver's own rule and says so; the recorded cases show where it agrees with mtools.
* The mount-time check on a large dirty volume taking longer than a person expects. It reads the directory tree and the FAT once, and the record says it is running.
* The Lenovo's fourth partition. Rehearsed on the sealed image, approved with live values, done with recovery as the default, with sector zero saved and restored on mismatch, and with the three partitions hashed before and after; a refusal costs the hardware write proof until milestone 39 and nothing else.

## Recovery after a compaction

Read `AGENTS.md`, `CLAUDE.md`, this plan's decisions and the current phase, `progress.md`, `current.md`, `notes.md` and `hardware.md`. Run `git status --short --branch` and `git log --oneline -15`, inspect every uncommitted path, and resume the current phase's first unfinished item. Report the one line.

## The goal prompt

`Run milestone 37, writing FAT32, from dev-log/37-fatwrite/plan.md.`
