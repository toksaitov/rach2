# Milestone 44 notes

## Decisions taken while building

* Phase 1 fixed the order of writes in `krffs.c` rather than only writing it
  down. The plan puts the order in phase 4, but a Readme states the contract
  the tree keeps, and the two mutating paths did not keep it. The change is
  small and behaviour-preserving on a memory root, and the host rows of phase 2
  now prove it.
* `fsck.krffs` gained a third repair, one name on two nodes of a directory,
  because the protocol's move leaves that state. Two mutations proved it.
* The identifier of a regular file is not derived from its name in the images
  the host tools build (576 of 639 nodes in `os-real.krffs` disagree), so the
  check does not test it and a torn rename leaves nothing for it to repair.
  A file is found by its directory and its name.
* Phase 2 keeps the root's VFS adapter on node pointers. The plan's phase 2
  line says the adapter moves to encoded handles; the decision it rests on
  says the root keeps a pointer facade so `fd`, the syscalls, the orphan scan
  and the dirty bit are not rewritten in this milestone, and those two pull
  against each other. The encoding is what the mountable type needs, so it
  arrives with that type in phase 3, and the root's adapter is left alone.
  Nothing in the milestone's completion depends on the root's handles.

## What the rows found

* The order rows of `tests/host/krffs_hosttest.c` found a fault in the grow
  path on their first run: a file grown by less than a header wrote the tail's
  free header on top of the following free node's live header, so a machine
  stopping there left a torn node in the ring. The grow now takes the free
  node whole with one published size and splits the surplus back off with
  another. Four wrong orders were each caught after the fix.
* `tests/run all` on the first phase 2 tree failed `selfbuild` on the KRFFS
  root: the second build's console line arrived truncated, which is what the
  harness sees when the machine is slower than it expects. The cause was in
  the new core, which read a whole 357-byte header at every node of every
  walk, and a lookup reads the whole ring. The walks now read the ninety-nine
  bytes they match on and the name only when the rest matches. The suite
  passes again, at 330s against 354s for the run that failed.

## The Codex review at the phase 2 boundary

One unit, six files (`krffs.c`, `krffs.h`, `krffs.Readme.md`,
`krffs_hosttest.c`, `fsck.krffs.c`, `krffscheck.sh`), read-only, on tree
7058d84. It ran past the ten minutes the policy allows, at about eleven, and
returned seven findings. All are fixed:

1. A truncate that raised a file's length inside slack the node already had
   published the length before the caller zeroed the bytes, so a stop between
   them showed bytes the file used to hold. This predates the offset core.
   A resize now publishes only a shorter length; a longer one is the caller's
   to publish after the zeroes. Proved by a host row over the prefixes and by
   its mutation.
2. Writes that must land before a publication had their answers ignored. The
   backend contract allows a refusal, so a device could refuse a tail header,
   take the size that reveals it, and leave a ring nobody can walk. Every
   prerequisite write is now checked; a refused surplus split leaves the space
   inside the node, which is sound. Proved by a host row that refuses one
   write of an operation, over every write of three operations, and by its
   mutation.
3. `sizeof(header) + size` could wrap for a size near the top of the range and
   publish a node smaller than a header. Both the allocator and the resize now
   refuse a size larger than the region. Proved by a row and its mutation.
4. The pointer interface ignored its `krffs_file_system_t *` argument and
   subtracted pointers that need not belong to the image. It now compares
   addresses, refuses a pointer outside the root's image, and refuses a file
   system that is not the root's.
5. A name that filled its 256-byte field with no terminator passed the mount
   check, and the readers then read past it. The check now refuses such a
   node, `fsck.krffs` refuses it too, and the path builder is bounded. Proved
   by a row and its mutation.
6. Test gaps: the recorder could not refuse a write (fixed, above); no row
   grew a file inside existing slack (fixed, above); the system suite's grow
   model wrote one size where the implementation writes two, and its growth
   was too large to reach the case the two publications exist for (both
   fixed, with a row for a growth shorter than a header); its move row
   fabricates the state rather than driving the allocator, which the comment
   now says, the real relocation being the host rows' business.
7. Three claims in the Readme did not match the code: a bad backward link is
   refused by the kernel and repaired only by the host tool; a create
   publishes the node before its name, leaving a window with no name; and the
   dirty flag is a fourth thing the tool settles. All three rewritten.

The unit also said what it found sound: the bounds arithmetic, that every ring
walk is capped, that both published states of the grow are sound, and that
fsck defers its repairs until after its structural walk.

## The Codex review at the phase 4 boundary

One unit, six files (`krffsmount.c`, `krffs.c`, `krffs.Readme.md`,
`krffs_hosttest.c`, `krffsprobe.c`, `krffsmount.sh`), read-only, on tree
46fa9f2. It returned eleven findings, three of them states a stop could leave
that no repair could undo. All are fixed:

1. A move published the new node's length and its type with nothing between
   them, and freed the old node without the new one being on the medium. A
   stop could leave the file at length zero, or gone. A node is now published
   only once everything it is made of is on the medium, and the allocation
   syncs after the type that publishes it, which is what the removal that
   follows rests on.
2. A grow of less than a header wrote the tail's header before the enlarged
   size was on the medium, and that header lands where the free node's own
   header was. A stop could leave a node the walk cannot cross. The enlarged
   size is synced first.
3. A create published a node as reserved before its name, and a freed node
   keeps the name it had, so a stop could bring a removed file back. An empty
   name is written before the type now, and the real name last, after the
   directory, the mode and the owner are on the medium.
4. A write and a truncate ignored a failure of the length they publish, and
   answered success over it. Both check it.
5. The namespace operations ignored what the backend answered. A removal now
   answers, and create, mkdir and rename check every write.
6. A truncate that moved a node and then failed left every holder naming free
   space, and reported a full disk for an I/O error. The core answers where
   the node is whatever happened, the mount repoints from that, and the two
   errors are told apart.
7. A volume had no bookkeeping for open files: unlink freed the node under a
   reader. A volume now keeps sixteen open files, a name removed under a
   reader takes the name and leaves the node, and the last close frees it,
   which is what the root does.
8. `sync` did not take the mount's lock. It does.
9. The host rows modelled a cache by losing one write; a cache loses whole
   blocks. They now do both: every block written since the last sync, lost
   whole, and every single write, which is stricter than a cache and is the
   order between fields this file system keeps. The sync rule was off by one
   and hid the states a missing sync produces.
10. The refusal rows judged only the shape of the image. They now refuse
    syncs as well as writes, cover every call of each operation, and require
    that no name reaches a node that is not finished.
11. The crash row printed its token before writing anything, so the machine
    was killed inside a mount; it now writes first. The row that proves the
    repair is the volume broken on purpose, whose counts are exact. Three
    claims in the Readme did not match the code and are rewritten.

Each fix was proved by putting it back the way it was and watching a row fail,
except the two that the review found to be already covered elsewhere: the sync
before a removal (the allocation's own sync covers it, and the redundant one
was taken out) and the read-only flag on a writable mount (which phase 4
replaced).

## Where the third kind of device is proved

`request.c` is i386 assembly (it reads the flags register to refuse a
sleeping device where interrupts are off), so it does not compile on the
host, which is why `bcache_hosttest` stubs `block_request` rather than
linking it. A host test for the new caller-synchronous kind was written and
then removed for that reason. What can be proved on the host is the chain of
devices, which `chain.c` holds on its own and `bcache_hosttest` links: which
device is under which, how deep a chain may go, and the cache's rule that a
caller holding one device's block may take a block of a device under it and
not one above it. The dispatch itself, and that such a device takes no slot
of the request pool, are the guest's rows in phases 6 and 7, where a loop
device exists to be driven.

## The Codex review at the phase 6 boundary

One unit, six files (`loop.c`, `loop.h`, `chain.c`, `bcache.c`,
`loopdevprobe.c`, `loopdev.sh`), read-only, on tree f428c93. Twelve findings,
all fixed:

1. A dirty victim of a loop device was written with the caller's holds
   already at the limit, so the write's own re-entry took a fifth buffer and
   stopped the machine. The cache now passes over such a victim unless there
   is room for two more.
2. `bcache_sync` waited for a buffer its own caller held, which nothing could
   end. A sync while holding a block of the device it syncs is now named as
   the defect it is.
3. The attachment claimed its slot only after everything that can sleep, so
   two attaches could publish over each other. The slot is claimed first, the
   file is checked again after the open, and every failure gives the slot
   back.
4. A medium that went while a transfer was inside the file system holding the
   file cleared the state that transfer was using. Transfers are counted now,
   and the release waits for the last one out.
5. Detach had the same shape; it refuses while a transfer is in flight.
6. The transfer callbacks trusted their caller. They check the sector range
   and the byte arithmetic themselves.
7. The control node took `detach x`, `detach 0junk` and numbers that overflow.
   A device number is read in one place, must be a number, must name one of
   the four devices, and nothing may follow it.
8. The control node has no privilege check of its own; the node is 0600 and
   owned by root, which is the check, and the Readme now says so.
9. The rows that ask what a write to an attached file answers ran only if the
   open succeeded. The open must succeed.
10. The row for a device that is taken used the file that was already
    attached, so the duplicate rule answered it. It uses another image now.
11. The row that called the KRFFS image clean proved nothing about what had
    reached it. The host's own reader now reads the file the machine wrote
    out of the image.
12. `BLOCK_READ_ONLY` had no coverage, and the reason turned out to be that
    only one file system honoured it: a write-protected stick would have been
    mounted read-write by the other three. The mount table now forces
    `MS_RDONLY` for a medium that refuses writes, which is one place rather
    than four, and a row attaches a device read-only and is refused a create.

Each fix was proved by putting it back the way it was, except the two the
review named as depending on code outside its six files (the transfer bounds,
which the block layer also checks, and the privilege of the control node,
which is the node's mode).

## The milestone-end review round on Codex

The owner asked on 2026-09-05 for the whole milestone to be read again by
Codex on a model he had just made the default there, with as much time as it
wants and with permission to build its own context. The round is five units,
fixed here before the first one starts, running in parallel as a
milestone-end round may:

1. The format's core: `krffs.c`, `krffs.h`, `krffs.Readme.md`.
2. The mounted volume: `krffsmount.c`, `krffsmount.h`, `vfs.c`, `syscall.c`.
3. The loop device and the block layer: `loop.c`, `loop.h`, `chain.c`,
   `request.c`, `block.h`, `bcache.c`.
4. What proves it: `krffs_hosttest.c`, `bcache_hosttest.c`, `krffsprobe.c`,
   `loopdevprobe.c`, `krffscheck.sh`, `loopdev.sh`, `krffsmount.sh`.
5. The host tool and the documents: `fsck.krffs.c`, `krffs/Readme.md`,
   `block/Readme.md`, `fs/Readme.md`, `vfs.Readme.md`, `tests/Readme.md`.

Every unit is told what the milestone decided, what the three earlier reviews
already found and fixed, and that it may read anything else in the tree.

The third unit, the loop device and the block layer, reported five findings
and they were fixed here:

1. A file on the memory root that a loop device stood on could be unlinked,
   or renamed over, and its extent freed under the device. The root's rule
   for a file somebody holds is the descriptor tables, which an attachment is
   not in; the other file systems keep an unlinked node for whoever holds it.
   Both removals now answer `EBUSY` while a device stands on the file, and
   `vfs_repoint` follows a node the file system moves into the attachments as
   it already did into descriptions.
2. A detach of a slot an attachment had claimed but not yet published freed
   that claim, and the attach then published into a slot another attach could
   take. The claim is a state of its own now, and a detach of a slot in it is
   refused; the claim also clears the device this slot last stood on, so a
   medium going cannot match it.
3. The cache looked for the requested block again after a writeback only when
   the written buffer's device stood on another. Every writeback sleeps, so
   another caller can key that block whatever kind of device it was; the
   lookup is repeated after every one.
4. A write or a truncation that had passed the attachment check and was
   waiting for its description's lock ran after an attach published itself.
   The check is made again under the lock, and an attach over a file whose
   description is inside an operation is refused, which closes the window
   from the other side.
5. `detach 0 junk` detached: the number was read but what followed it was
   not. The command now takes nothing after the number.

Findings 1, 3, 4 and 5 have rows, each proved by putting the code back the
way it was. The claim state in finding 2 is between two processes at a
particular moment and has no row; it is a state a reading proves.

The first unit, the format's core, reported six findings:

1. A split published the smaller size of a free node while its length was
   still the length of the file removed there, and the mount check refused
   every node whose length was larger than the node. A free node's length
   means nothing; the kernel's check and `fsck.krffs` now ask it only of a
   node that has data.
2. One field can cross a cache block, so a `size` at the end of a block
   reaches the medium in two pieces and a stop between them leaves a value
   that is neither. Nothing in the order of writes prevents it. It is written
   down in `krffs.Readme.md` as the one state the format cannot rule out on a
   block device; ruling it out means keeping every header inside one block,
   which is a change to the allocator and to `mkfs.krffs`, and the owner
   should decide it rather than have it arrive inside this milestone.
3. A rename that published its new parent and stopped before its new name can
   put a file beside a directory of the same name, and the repair, which
   keeps the larger, could take the directory's name away and put its
   children out of reach. A directory never loses its name now.
4. A shrink wrote the surplus's free header inside the bytes the old length
   still counted, without putting the shorter length on the medium first. The
   length goes first, and a host row replays every stop of a shrink and
   requires the file to hold its own bytes.
5. A name the medium would not give up was read as a name that is not there,
   and the reaper frees such a node. A failed read now answers that the node
   has a name, which is the answer that frees nothing.
6. A move that could not free the node it copied from reported success, and a
   running kernel would then walk to the old node and read what the file used
   to be. It reports the failure, and the refusal sweep in the host test now
   holds every operation that reports success to leaving no name on two
   nodes.

The second unit, the mounted volume, reported ten. Fixed here: a truncate of
an unlinked file, which reached its parent directory through the path a
nameless node spells and could free it (the core takes a node now, not a
path); a create and a mkdir that threw the mode and the owner away; a create
over a name that is already there, which the core answers with success and
which is now `EEXIST`; a rename over an open file and a removal of an open
directory, which freed nodes under their holders; a handle held by a caller
that was asleep while the node moved, which the volume now follows through
the moves it remembers, with reads taking the mount's lock as writes do; the
format's sync point, which put the cache on the device but not the device's
own cache on the medium; a truncate through `open(O_TRUNC)` that did not ask
whether a loop device stood on the file, now asked in the VFS for every path;
an unmount that cleared the dirty flag and then failed, which now puts it
back through a new `abort_unmount` operation; and a last close that mutated
the allocator without the mount's lock and cleared a flag another open had
set in the slot it had given up.

The one finding of that unit not fixed here is the shared `exec_ctx` in
`syscall.c`: `execve` keeps one static context and its reads of a mounted
volume sleep, so two of them can interleave. It is not this milestone's code
and the fix is a lock or a per-call context; it is recorded for the owner
rather than changed inside a milestone about mounting.

The fourth unit read what proves the work and named eleven gaps. The two
that hid defects are fixed with the findings above: the refusal sweep ran
over too few calls to reach a move's last writes and threw the operation's
answer away, and the slack row's oracle did not look at the bytes a lost
write leaves. The rest are rows this milestone should add and are listed in
the plan's phase 9.

The fifth unit read the host tool and the documents. Its three faults in
`fsck.krffs` are fixed: the checker called the allocator on links it had not
yet rebuilt, so a corrupt backward link could merge a node with itself; it
never judged a node's own size, so a root of no size passed `--strict`
without the region being walked; and every early exit freed a pointer that
was not yet initialised. The documents it read are being corrected against
the code in phase 9.

## Phase 9

The fifth unit's documents are corrected against the code: what the mount
table takes as a source, what a missing `fsync` answers, what `link` and a
directory rename answer, how many links a mounted volume reports, where
`getdents64` keeps its place, which mounts are permanent, what the victim
rule is, what a write-through means for a type that marks its metadata
dirty, and the checker's own comments about what it validates, what it
returns and what it repairs. The checker's usage line now names the arguments
it takes.

The fourth unit's gaps are rows now, each proved by putting the code back:

* Two volumes at once, the third answering `ENOMEM`, and the slot coming back
  at an unmount, with each volume holding only its own files.
* A write through a loop device synced when it returns. The row fills the
  cache through a mounted image so that eviction alone writes its blocks
  back, and `/proc/loop` says how many writes the device made and how many it
  synced; without the sync in the write, 837 writes went unsynced.
* An image on an ext2 volume, attached, mounted and written, with a second
  name for that file made by `link` refused a device of its own. The earlier
  duplicate row used one path for both attempts, so a check by path passed
  it; this one fails such a check.
* A truncation that moves a file, with two descriptors on it, and an
  overwrite inside a file, which the mounted volume had no row for.

Two of that unit's gaps have no row and this is the reason. The refusal of a
detach while a transfer is in flight sits behind the refusal of a detach
while anything is mounted on the device, and a transfer with no mount over it
arises only when a medium goes, which the pull row already covers. A loop
device reached outside syscall context is a panic in the block layer that no
program can ask for; it is the kind of fault a mutation shows and a row
cannot.
