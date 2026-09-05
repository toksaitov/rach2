Milestone 44, phase 9 of ten. The plan is `plan.md` in this directory and is
the authority for what is decided; the tree is the authority for what is
built. Read `CLAUDE.md` first, then the plan's decisions, then this file.

Phases 1 to 8 are done and committed. A KRFFS volume is mounted by `mount(2)`
over a partition or over a loop device, read and written, and the format has
one implementation in the kernel. On the machine: the kernel installed
through `bootctl` and committed; an image on the ext2 root attached to a loop
device, mounted, written and judged clean here; and a KRFFS partition written
to the stick from the mac mini, then mounted on the notebook over the
SuperSpeed hub, read, written, unmounted, and judged clean here with every
file the machine wrote inside it. The acceptance script has rows for both and
is rehearsed against QEMU by `tests/run metal`.

The milestone-end review round ran five Codex units over the whole milestone.
Every code fault they found is fixed and proved by mutation, and `notes.md`
records each one; the two the owner asked about by name are fixed as well:
`execve` has a workspace per process, and a field the medium would take in
two pieces is refused with the reason in the log.

What is left, in order:

* Phase 9. The documents. The fifth unit listed about thirty statements in
  `krffs/Readme.md`, `rach2/sys/fs/Readme.md`, `vfs.Readme.md` and
  `block/Readme.md` that describe what the code used to do; they are in that
  unit's report and none of them is corrected yet. Then every other Readme
  read against the code, the comments in the systems this milestone changed,
  and a Codex proofreading round with its unit count fixed in `notes.md`
  before the first one starts.
* Phase 9 also owes five rows the fourth unit named: a loop device's
  durability judged at the write rather than after a later flush, an ext2
  volume under a loop device with a hard link to an attached file refused,
  a detach refused while a transfer is in flight, a loop device reached from
  the wrong context, and two KRFFS volumes mounted at once.
* Phase 10. Close the notes, run `tests/run all` on the final tree, write the
  milestone's Readme and the `dev-log/Readme.md` entry.

Two faults found on the way and not chased, both recorded here for the owner
to decide: `execve` clears `sig_pending` on a successful exec, so a signal
that arrived just before it is lost; and an unmount that clears a dirty flag
and then fails puts it back only for KRFFS, while ext2, FAT32 and exFAT have
the same hole and the hook to fix it.

The machine panicked once, with a page fault in kernel mode, while the stick
was being plugged and unplugged with a KRFFS volume mounted on it. It has not
happened again; `tests/run krffsmount` now pulls a stick from under a mounted
volume and plugs it back in while the mount is still there, and the machine
survives that in QEMU.
File being read when written: none.
