Milestone 44, phase 8 of ten. The plan is `plan.md` in this directory and is
the authority for what is decided; the tree is the authority for what is
built. Read `CLAUDE.md` first, then the plan's decisions, then this file.

Phases 1 to 7 are done and committed. A KRFFS volume is mounted by `mount(2)`
over a partition or over a loop device, read and written, and the format has
one implementation in the kernel: `krffs.c` addresses nodes by offset over a
backend, with a memory backend for the root (which keeps its pointer
interface) and a block backend through the buffer cache for a volume.
`/dev/loop0` to `/dev/loop3` stand on regular files, attached through
`/dev/loop-control` by `loopctl` and listed in `/proc/loop`; the block layer
has a third kind of device for them and the buffer cache follows a device's
backing chain. `tests/run loopdev` mounts a FAT32, an ext2 and a KRFFS image
from files, on a disk, on a stick that is then pulled, and on the memory
root; `tests/run krffsmount` is the KRFFS volume's own suite.

Three Codex reviews have run at the boundaries of phases 2, 4 and 6, with 7,
11 and 12 findings; every one is fixed and recorded in `notes.md`, and each
fix was proved by putting it back and watching a row fail.

The owner asked on 2026-09-04 that nothing on the Lenovo be scheduled before
phase 8 and that the run not stop to ask him anything until then. The
notebook is up and on the network; the stick is in the mac mini.

Next action: phase 8, the machine. It needs the owner: a KRFFS image file on
the notebook's ext2 root through a loop device, written, unmounted and
checked on the host; a KRFFS partition of the stick mounted and written; the
acceptance script's new lines. The disk rule in `CLAUDE.md` governs anything
written to the notebook's disk, and a kernel goes through `bootctl` from the
running system.
File being read when written: none.
