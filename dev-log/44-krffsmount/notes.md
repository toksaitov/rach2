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
