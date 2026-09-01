# Milestone 37 notes

## Phase 1 inventory

Base: a clean tree on master at the close of milestone 36.

```text
207fb2a Add the full set's time to the milestone 36 record
64829b4 Record milestone 36
7262391 Correct the mount documents and the VFS header against the code
b1bd997 Remove the KRFFS walk predicate and resolve readlink through the mount table
43fbb0e Record the review round and the contracts it changed
1f5c1d9 Accept a debug-exit status under --ok and expect EISDIR for creat of the root
8054353 Make the probe rows prove what their names claim
70c9380 Validate long names, times and options in vfat and prove them on the host
813b0aa Retry failed writeback and check buffer ownership in the cache
889a956 Fix the review findings in the VFS and the syscall layer
cacbdd7 Own a request by generation, claim ranges on the physical disk, and stage lagged reads
ba29673 Describe the mount table, the vfat type and the block request in their Readmes
```

## Review checkpoints (fixed now; at most two units run at once)

Phase 2: unit A, the cache's write half: bcache.c/h, block.h, lag.c,
bcache_hosttest.c, the init updater. Phase 3: unit B, the writer's order of
writes and lock: fat32.c/h, vfs.c's lock change, fat32_hosttest.c's
sequences and matrix; unit C, the check and the dirty-volume repairs:
mkfatfixture.c, fatcheck_hosttest.c, the mount-time check in fat32.c. Phase
4: unit D, names and attributes: the name code in fat32.c,
fatname_hosttest.c, vfsprobe.c. Phase 5: unit E, the suite and the
rehearsal: fatwrite.sh, the probes, lagprobe.c, the recovery ISO target,
metal_accept.sh; unit F, the proofread of the reader-facing documents. Six
units, two at a time, plus re-reviews only for contract changes.

## What mtools writes (measured in phase 1)

`mformat -C -F -T 28672` (28672 sectors, one-sector clusters): 32 reserved
sectors, two FATs of 221 sectors, mirroring on, root cluster 2, FSInfo in
sector 1 with the exact free count and the hint at the next free cluster,
both dirty flags clear.

| Copied as | Stored | Case byte |
|-----------|--------|-----------|
| `hello.c` | `HELLO   C  `, no long entry | 0x18 (base and extension lower) |
| `Mixed.c` | one long entry, alias `MIXED   C  ` | 0x00 |
| `long name.txt` | one long entry, alias `LONGNA~1TXT` | 0x00 |
| `longname1.txt` | one long entry, alias `LONGNA~2TXT` | 0x00 |
| `longname2.txt` | one long entry, alias `LONGNA~3TXT` | 0x00 |
| `UPPER.TXT` | `UPPER   TXT`, no long entry | 0x00 |
| `Upper2.TXT` | one long entry, alias `UPPER2  TXT` | 0x00 |
| `Sub` (directory) | one long entry, alias `SUB        ` | 0x00 |

So mtools keeps the Windows NT rule for the short-only case (a name whose
two parts are each all upper or all lower is stored short, with the case
bits), writes a long entry otherwise, and numbers colliding aliases in
creation order (`~1`, `~2`, `~3`), not by any property of the name. This
driver's alias rule (phase 4) is recorded against this table.

## Standing rule changed by the owner

The plan's rule of five concurrent delegated units is superseded: at most
two run at once (CLAUDE.md, Agents), set during milestone 36's review round.

## Phase 2 record

Metadata is written through (`bcache_write`), data is written back; a
dirty victim is written before its buffer is reused; a failed write is
recorded against the device through `vfs_device_failed`, which puts every
mount on it read-only with a log record naming the sector and leaves the
buffer dirty for a later sync. `sync(2)` runs every active mount's `sync`
and the cache's writeback before the page cache's; `fsync(2)` on a mounted
file writes the device's dirty buffers when the type has no `fsync`.
`/bin/update` sleeps thirty seconds and syncs, started and kept running by
all three inits; os-real, rach2-metal and rach2-recovery stage it by name
because they have no `exec-dir` line, os-simple through its `exec-dir`.
`rach2.blockfail=N` fails the Nth write through the lag device.

The plan's guest proof "a boot with rach2.blockfail shows a failed write
and a mount gone read-only" needs a write through the cache on a mount,
which nothing does until the writer exists; that boot is phase 3's, with
the mutation "keep a mount writable after a failed writeback" proved
there. The updater's proof ran by hand: on an os-real disk, `ps` shows one
`update`, and a file written with no sync call was on the disk after forty
seconds (readfile.krffs on the killed machine's image); the suite row is
phase 5's.

Phase 2 mutations, proved once each through mutate37p2.sh (backup, edit,
touch, rebuild, one check, restore):

* bcache_write returning before the device has the sector: caught by the
  write-through row
* a dirty victim reused without being written: caught by the victim row
* the updater's interval passing without a sync: caught by the by-hand
  boot (the file is not on the disk)

## Phase 3 record

The writer: FAT entries written through to the active copy and then the
mirror, allocation first-fit from the hint with the run given back on a
failure, the dirty flag in both places before the first metadata write,
FSInfo at sync and unmount, every mutation under the mount's namespace
lock, and the order of writes fat32.Readme.md gives. Short names only;
long names are phase 4's. The mount-time check walks a dirty volume under
a shared bitmap and repairs a surplus tail, orphan long-name entries and a
lagging mirror; anything else mounts read-only with a record.

What the rows found:

* A write through the description of an unlinked file rewrote its freed
  entry, so a later create found the name twice; an unlinked node writes
  no entry now.
* A failed run allocation left its partial chain behind (2842 lost
  clusters after ENOSPC); the run is given back on failure.
* The matrix carried dirty buffers from one stop into the next fixture;
  the loader empties the cache first.
* Directory growth zero-filled the new cluster for writeback, so the link
  could reach the disk before the zeros: found by the unzeroed-cluster
  mutation, which escaped until a sequence grew a directory over a
  junk-filled fixture. Directory clusters are written through now.
* The syscall layer still refused open(O_CREAT) on any type but the root
  (a milestone 36 stop); it creates on a writable type after the parent's
  write check.
* The kernel log formatter has no `%u`; the sector numbers printed as `u`.
* mtools refuses a boot sector with zero heads and sectors per track; the
  fixture builder writes a geometry now.

Phase 3 mutations, proved once each through mutate37p3.sh and
mutate37p3b.sh (restoring scripts; host rows through tests/run host, the
guest one through the blockfail boot):

* the entry written before the chain is joined: caught by the matrix
* the chain freed before the entry: caught by the matrix
* the size grown before the data is synced: caught by the content oracle
* a directory grown with an unzeroed cluster: caught by the matrix (with
  the growth sequence and the junk fixture)
* only the active FAT written on a mirrored volume: caught by the clean
  unmount's walk
* the FSInfo count trusted: caught by the wrong-FSInfo row
* the dirty flag never set: caught by the first-write row
* the mount kept writable after a failed write: caught by the blockfail
  boot (phase 2's deferred proof, closed here)

## Phase 4 record

Names live in `fat32name.c`, apart from the driver so the host test reads
them alone: the UTF-8 to UTF-16 conversion and what it refuses, the
Windows NT short rule with the case bits, the alias (this driver's own:
six characters of the base, `~` and the smallest free number, three of the
extension; a character outside the short set or outside ASCII as `_`), the
checksum and the long entries. Long entries are written before their short
entry and freed after it; a rename within one directory to a name of the
same entry count rewrites in place. `setattr` stores the times and the
write bits as the read-only attribute and answers EPERM for anything else.

What the rows found:

* The alias walked UTF-8 bytes, not characters: a three-byte character
  became three underscores.
* The plan's row for a directory descriptor after its directory's rename
  cannot live in vfsprobe: KRFFS refuses directory renames by design. It
  lives in mountprobe on the vfat mount, and needed a `path` operation in
  the interface: a type whose directories can be renamed rebuilds a
  directory's path from its `..` chain, and the `*at` calls use it, so the
  descriptor follows the rename instead of keeping a stale spelling.
* The rename syscall's generic arm ran the type's rename outside the
  namespace lock; it is locked now.
* The unmount deadlocked on its own namespace lock: `vfs_do_umount` holds
  it while calling the type's `sync`, which locks for the `sync(2)` path
  that arrives unlocked; a last `put` under a namespace change would do
  the same. The lock is re-entrant by holder now, which vfs.Readme.md
  says and why.
* The probe's own output lines were lost from the serial capture between
  kernel log lines during the hunt; the kernel's records were what showed
  where the hang was.

Phase 4 mutations, proved once each through mutate37p4.sh (host rows):

* an alias that collides on the raw bytes: caught by the numbering row
* long entries written without their checksum: caught by the lookup row
* a trailing space accepted: caught by the name test
* chmod changing an execute bit: caught by the attribute row
* a rename losing the node's location: caught by the open-file rename row

The plan's sixth, an `0xE5` first byte stored unescaped, is unreachable
from a UTF-8 name: the byte is a lead byte of a three-byte character and
never a name's first stored byte, and the alias maps a character outside
ASCII to `_`. The escape stays in the code for a raw eleven-byte caller and
is not proved by a mutation.

## Phase 5 record

`tests/run fatwrite` is six boots on a disk whose second partition is a
FAT32 volume: `fatwriteprobe` writes a tree, a second boot reads it back,
the host's walk and mtools judge the volume (and `fsck.fat` where the host
has it), a boot is killed while writing, the next mount repairs the volume
and the probe checks the record, BusyBox writes through the shell, and two
`lagprobe` writers run under the lag device. `tests/run scratch` (full
tier) rehearses milestone 43's fourth partition on the sealed disk image:
the recovery ISO boots as the stick, `reflash` rewrites sector 0 with the
partition and then the volume, and the ordinary system mounts and writes
it. `rach2-recovery-iso` is the stick's image, with `rach2.disk=ahci` on
its boot line because the machine's disk is on the AHCI controller.

What the rows found:

* The suite passed the boot fixture tool where the volume check was
  wanted; `MKFATFIXTURE` builds `mkfatfixture` for the suites.
* FSInfo's free count was stale after a crash, since the check repaired
  the volume and left the count the crash had written; the check counts
  free clusters as it walks and rewrites FSInfo, and the unmount counts
  when the mount never learned the number.
* The assertion of the mount's lock found the generic unlink, mkdir and
  rmdir arms unlocked, as phase 4 had found rename; all four lock now.
  The host test stands in for the syscall layer and holds the lock
  across its rows.
* `lagprobe` took `write` for `writers`, since the two names share five
  letters; the mount suite's bootpart boot found it.
* The recovery stick had no `/dev/disk0` until its boot line named the
  AHCI controller.
* `cppcheck`: locals named `x` and `y` shadow the console cursor globals
  that `vga_text.h` declares.

Phase 5 mutations, proved once each through mutate37p5.sh (restoring
script; one boot by hand, one full suite):

* `begin_write` without the lock: caught by the assertion in the writers
  boot (the panic is in the serial log)
* the crash boot's mount skipping the check: caught by the suite's repair
  step

Review unit E (Codex, the phase's diff over six files, ten minutes) found
seven things, all fixed before the phase closed:

* `begin_write` decided read-only before taking the lock, so a writer
  queued behind the one whose write failed went on writing; the check is
  under the lock now.
* The mkdir and open arms looked the name up before the lock, and the
  driver did not look again, so two creators could make the name twice;
  create and mkdir refuse an existing name under the lock (EEXIST), and
  an open that loses the race opens what the winner made.
* The check's count skipped every cluster the walk had marked, and the
  first cluster of a freed surplus tail is marked, so FSInfo was one
  short per repair; the count reads the entry whether marked or not.
* The rehearsal saved eight sectors of the range, never set the boot
  default and never rehearsed the undo; it saves every sector, makes
  recovery the saved default before the first write, writes the saved
  sector 0 back and reads it before the volume is written, and puts the
  default back at the end. The stick's own copy cannot be rehearsed
  from a read-only ISO; hardware.md says what milestone 43 arranges.
* The first `statfs` counted the FAT outside the lock, racing the first
  allocation; it counts under the lock.
* The probe's `fail` never printed `FATWRITE FAIL`, so the suite's
  `--never` rows could not fire, and a crash sequence of two hundred
  files could finish inside the harness's grace; the token is printed and
  the sequence runs until the kill.
* The writers row ignored `sync`'s result and claimed to prove the sync's
  turn under the lock, which it cannot observe; it checks the result and
  its comment says what it proves.

Two rows came with the fixes and were proved by mutate37p5b.sh: a second
create of a name is EEXIST (caught with the check dropped), and the free
count after a surplus repair is the FAT's (caught with the marked cluster
skipped).

Times on this host: fatwrite 76 s, scratch 30 s, mount 103 s.

## Phase 6 record

The documents read against the code found: fat32.Readme.md said FSInfo
is read by nothing, while the mount reads its hint; vfs.Readme.md still
said the type is read-only, that a mount without `MS_RDONLY` is EROFS,
that writing to vfat is absent, and that `tests/run mount` is eight boots;
rach2/Readme.md's directory table said read-only FAT32; tests/Readme.md
listed four of lagprobe's six modes; tests/host/Readme.md had no line for
`fatname_hosttest` and none for the long-name rows. All corrected.

The code sweep: the driver's `EROFS` returns are the six read-only checks
(a mount option or a failed write) and no refusal of an unimplemented
operation remains; the milestone 36 write path that held only the
description is gone, since `write` reaches `fat_write` and its
`begin_write` takes the mount's lock. Helpers with one caller that name a
step were kept: `alloc_cluster`, `alias_taken`, `bit_test_set`,
`walk_dir`, `rewrite_mirror`, `fat_check_volume` and `forbidden`, beside
milestone 36's `node_for`, `put_utf8` and `parse_options`; none is a
wrapper.

The build, every addition and its reason:

* `rach2/sources.mk`: `fat32name.c`, the name rules apart from the driver
  so the host test reads them alone.
* `utils/programs.mk` and the os-real, rach2-metal and rach2-recovery
  manifests: `update`, the periodic sync; os-simple stages it through its
  `exec-dir` line.
* `tests/Makefile`: `fatwriteprobe`.
* `tests/system/suites.mk`: `MKFATFIXTURE`, the one Make rule for the
  fixture builder, because the suites walk a volume the kernel wrote; the
  host tests keep compiling their own copy into their fixture directory,
  as they did before this milestone, so `tests/run host` needs no Make
  rule. `run-fatwrite` and `run-scratch`.
* `tests/registry`: `fatwrite` in the core tier (76 s, six boots: within
  the milestone 36 budget for a core suite) and `scratch` in the full
  tier, since it boots the recovery ISO and the sealed system over SSH.
* `Makefile` and `tools/image/mkiso.sh`: `rach2-recovery-iso`, the
  stick's image, with `rach2.ip=none rach2.disk=ahci` on its boot line.
* `tools/setup/doctor.sh`: the optional `dosfstools` row, for `fsck.fat`
  as a second reader in the fatwrite suite.
* `tests/system/accept/metal_accept.sh`: the scratch partition's write
  check before the verdict, for milestone 43.
* `tests/host/mkfatfixture.c`: `--check` and `--list`, and the fixtures
  `--dirty`, `--dirty-surplus`, `--dirty-orphan`, `--dirty-mirror`,
  `--dirty-cross`, `--fsinfo`, `--fsinfo-wrong`, `--overlong-chain`,
  `--short-chain`, `--lost-chain`, `--cross-link`, `--dup-name`,
  `--bad-dotdot`, `--dir-loop`, `--junk`, `--cfg-short`, `--no-grubenv`,
  `--bpb-root-out` and `--bpb-fat-oversized`, each the subject of a
  `fatcheck_hosttest` or `fat32_hosttest` row.

No `build_*.sh` was added or run by hand. `guestcheck` runs in
`tests/run style`; `tests/run selfbuild` ran on the final tree.

The mutation scripts, each restoring its source in a trap and rebuilding
before the verdict, in the session's scratch directory: mutate37p2.sh
(the cache's three), mutate37p3.sh and mutate37p3b.sh (the writer's
eight), mutate37p4.sh (names and attributes, five), mutate37p5.sh (the
lock assertion and the crash boot's check, two) and mutate37p5b.sh (the
review round's two rows). Twenty mutations in the phases and eight more from the review round, every one caught.

## Phase 6 review round

The plan's checkpoints named units A to D for phases 2 to 4, and the
phase records show they did not run there; they ran in phase 6, two at a
time, against the final tree, with unit E's and unit F's findings.

Unit A (the cache's write half) found four things, all fixed:

* `bcache_sync` passed over a dirty buffer another process held and
  returned 0, so an unmount could finish with a write still in flight;
  it waits for the holder now, and `vfs_do_umount` returns `EBUSY` when
  the drop finds a buffer held or dirty instead of ignoring the answer.
* A failed victim write cleared the holder without waking the waiters
  for the buffer's old key; they are woken, and look again.
* A dirty victim of another device was written outside that mount's
  lock, and when the write failed the mount went read-only while a
  writer could be inside an operation; the cache marks the device
  (`BLOCK_WRITE_FAILED`) and refuses its next metadata write with
  `EROFS`, so the operation stops at its next step, and the mark is
  cleared when the unmount drops the device. The eviction itself stays
  outside the lock: writing a data buffer's current bytes earlier than
  the driver would is always within the order of writes, and the
  evicting process holds nothing the victim's holder can wait for.
* The host test's wait stub returned with the condition still false, so
  a row that reached it hung instead of failing; the stub fails the run.

The row "a device that failed a write takes no metadata write until it
is dropped" came with the fix and was proved by mutate37p6.sh.

Unit B (the writer's order of writes and its lock) found five things,
all fixed:

* The cluster count of a size near 4 GiB was computed as `(size +
  cbytes - 1) / cbytes` and wrapped; it is `(size - 1) / cbytes + 1`.
* A truncate that grew a file from nothing or from a cluster boundary
  linked and published its zero-filled run without a sync, so a crash
  after the entry write showed old disk bytes inside the new size; the
  run is synced before the link. The matrix's oracle accepted the stale
  bytes because they equalled the pattern; the truncate file's oracle
  now expects zeros at its regrown size.
* Rename freed the destination before `free_slots` could answer
  `ENOSPC`, so a full directory lost the destination and kept the
  source; the name and its room are decided with the destination in
  place.
* `alloc_run` discovered `ENOSPC` after writing part of a run, against
  the Readme's promise; the free count decides before the first FAT
  write, and the row counts the device's writes.
* `fat_sync` tested the dirty flag before taking the lock, so a sync
  could miss a first mutation whose flag writes were in flight; it
  decides under the lock.

Unit C (the check and its repairs, and the host checker) found six, five
fixed and one recorded:

* The walk freed a surplus tail when it found it, before the rest of
  the tree was walked, so a tail that ran into a later entry's chain
  was freed and that entry's clusters with it. The tail is walked and
  marked like the rest of the chain and recorded (sixteen at most, more
  is a refusal); the repairs run after the whole walk, when a tail
  that another chain enters has already been refused as a cluster
  reached twice. The fixture `--dirty-surplus-cross` builds the case;
  the dirty rows require the refusal and the image untouched.
* The bitmap was indexed by cluster number with a bound on the cluster
  count, two short at the top; the bound counts the two.
* The duplicate-name scan compared raw aliases and stopped at 4096
  entries silently; past that it refuses now. Comparing folded long
  names in the kernel was not done: it needs the names of every earlier
  entry, which the walk's memory does not hold, and this driver never
  writes two entries with one long name (a create checks under the
  lock); the host checker compares them, since the host has the memory.
* The host checker's duplicate-name storage was static across its
  recursion and it read at most 256 clusters of a directory silently;
  both are per call now, with a finding past the limits.
* The cross-link fixture ran a later file into an earlier one, which
  the old repair could not damage; the new fixture runs the earlier one
  into the later.

Unit D (names and attributes) found eight, seven fixed and one recorded:

* A directory could be renamed into itself or under itself, cutting the
  tree; the `..` chain of the destination is walked first (`EINVAL`).
* A held directory kept its old entry location after a rename, so a
  later `setattr` wrote the freed slot; the location follows.
* A long name past the kernel's 255-byte component was listed by its
  alias and refused by lookup with `ENAMETOOLONG`; the alias is its
  name in both.
* Writes and truncates stamped the creation hundredths along with the
  modification time; the stamp leaves them alone, and the host clock
  stub moves its hundredths so a row can see the difference.
* The name converter enforced the unit limit but not the byte limit it
  documents; a name of 256 bytes in 128 characters is `ENAMETOOLONG`.
* Creation hundredths past 199 were believed; they read as zero.
* `setattr` ignored a field it does not store (the size); it is `EPERM`.
* A FAT date after 2106-02-07 is clipped by the kernel's 32-bit epoch;
  recorded as the kernel's limit in the Readme, not changed.
* The name test's checksum row compared a call with itself; it checks
  221 for `BOOT`. A C89 fault (a declaration after a statement) and the
  file's read-only header comment were corrected.

The rows that came with the fixes were proved by mutate37p6b.sh,
mutate37p6c.sh and mutate37p6d.sh: the tail freed on discovery, ENOSPC
decided by writing, the run's zeros unsynced (which first escaped
because the script looked for the wrong words in the log, and was
rerun), the ancestry check dropped, the held directory's location kept,
and the creation hundredths stamped. Eight review mutations in all,
twenty-eight for the milestone.

Unit F (the proofread of six reader-facing documents) found counts the
tree had outgrown (42 host programs for 49, 81 root programs for 82, 61
suites for 63, 23 core for 24, eight mount boots for nine, a 223-line
runner), a wrong link, a claim about a zero-length raw read that the
code contradicts, the FSInfo sentence again (read at every mount, not
only a writable one; the other FAT read by the mirror check), and a
list of register faults ("holds", "charges", "keep deadlock out", "sleeps
its caller", "willingness") and dense sentences. All are corrected but
two: the `xhci_hosttest` line in tests/host/Readme.md and the `usb.sh`
line in tests/system/Readme.md, which list milestone 35's rows in one
sentence each; splitting a list of rows into sentences adds nothing,
and neither line is this milestone's contract.

The full set on the final tree found three things, fixed before the
rerun: the grub test image's manifest lacked the `update` line that
os-real gained in phase 2, and the grub suite compares the two; a mount
that went read-only after a failed write could no longer be unmounted,
since its `prepare_unmount` wrote FSInfo and the flag through a cache
that now refuses the failed device (the unmount leaves the volume as it
is, marked dirty for the next mount's check); and the crash boot, which
now runs until the kill, leaves lost clusters that the mount's check
counts and leaves by contract, which the suite's later walks then
refused (the repair step records the one lost line and the later walks
accept exactly it).

The rerun of the full set passed every suite but `raw`, milestone 33's
raw-socket suite, which failed once more when run alone and then passed
three times; the recipe kept no log, so eight boots by hand with logs
caught one failure: "the machine can still reach the gateway". The
kernel's ping answers its round-trip time in ticks, and the probe
required exactly zero, so a reply later than ten milliseconds, which a
loaded host gives now and then, failed the row. Both ping rows accept
any non-negative answer and print it, and the recipe keeps the two
boots' serial logs. Nothing in this milestone touches the network; the
full set found a row of milestone 33's that had passed by timing.

No boot proves what another proves: the six fatwrite boots each add a
reboot, a crash, a repair, a shell or a second writer, and the scratch
rehearsal is the only run of the fourth-partition procedure.

## Open items

(none)
