Milestone 44, phase 2 of ten. The plan is `plan.md` in this directory and is
the authority for what is decided; the tree is the authority for what is
built. Read `CLAUDE.md` first, then the plan's decisions, then this file.

Phase 1 is done and committed (8107836, 755d6fc, 307711a, 9ad6301). The
kernel now writes a KRFFS image in an order a stop between two fields can
survive: a split writes the tail's header before the size that reveals it, a
grow publishes its length only once the bytes are there, a truncate publishes
the shorter length first, and a move keeps the larger of the two nodes that
briefly hold one name. `rach2/sys/fs/krffs.Readme.md` is that order and what
the check repairs. `fsck.krffs` resolves a repeated name, and
`tests/run krffscheck` replays every prefix of every operation against it,
with five wrong orders that must be caught. Both fsck mutations were caught
under a restoring trap with the object removed before the rebuild.

The owner asked on 2026-09-04 that nothing on the Lenovo be scheduled before
phase 8, and that the run not stop to ask him anything until then. The
notebook is up and on the network, and the stick is in the mac mini.

Next action: phase 2, the offset core. `krffs.c` moves to offsets over a
backend of read, write, sync and a size, with the memory backend only; the
root keeps a pointer facade so `fd`, the syscalls, the orphan scan and the
dirty bit are not rewritten in this milestone; the VFS adapter moves to
handles that are the offset plus one. The proof is `tests/run all` unchanged,
both roots booting, and the host rows over the core. A Codex review of that
boundary comes before phase 3.
File being read when written: none.
