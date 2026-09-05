Milestone 44 is finished. `Readme.md` in this directory is what it built and
what it proves; `notes.md` has every review finding and what was done with
it; `hardware.md` has what the machine did. The plan is `plan.md`.

Two faults were found on the way and left for the owner to decide, both
recorded in `notes.md`: `execve` clears `sig_pending` on a successful exec,
so a signal that arrived just before it is lost; and an unmount that clears a
dirty flag and then fails puts it back only for KRFFS, while ext2, FAT32 and
exFAT have the same hole and now have the hook that would close it.

The machine panicked once, with a page fault in kernel mode, while its stick
was being plugged and unplugged with a KRFFS volume mounted on it. It has not
happened again. `tests/run krffsmount` now pulls a stick from under a mounted
volume and plugs it back in while that mount is still there, and the machine
survives that in QEMU; the netconsole capture on the mac mini is what would
hold the record of a repeat.
File being read when written: none.
