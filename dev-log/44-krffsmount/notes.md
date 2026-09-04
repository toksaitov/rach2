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
