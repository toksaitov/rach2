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

## Open items

(none)
