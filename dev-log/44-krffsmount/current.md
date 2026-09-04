Milestone 44, not started. The plan is `plan.md` in this directory and is
the authority for what is decided; the tree is the authority for what is
built. Read `CLAUDE.md` first, then the plan's decisions, then phase 1.

The tree at the start: milestone 43 closed, the notebook running its own
build on an ext2 root with the KRFFS root as its fallback. KRFFS in the
kernel is a memory file system reached by pointers, mountable only as the
root; `mount(2)` takes vfat, exfat and ext2 over a partition window.

Next action: phase 1, the oracle and the crash model. Strengthen
`krffs/fsck.krffs.c` to check the backlink the kernel enforces and to
validate before it clears the dirty bit, then write the update protocol
and the mount check's repairs into the new
`rach2/sys/fs/krffs.Readme.md`, with the host rows and their mutations.
File being read when written: none.
