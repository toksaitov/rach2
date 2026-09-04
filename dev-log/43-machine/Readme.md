# Milestone 43: the machine

Milestones 36 to 42 were built and proved on QEMU with their machine
checks written down for this one. This milestone ran them on the Lenovo
G50-70 in the order they were built, fixed every difference the machine
showed in the subsystem that owned it, with a row that fails without
the fix and a mutation that proves the row, and moved the machine to
the ext2 root with the KRFFS root as its fallback. The disk never left
its bay: every write to it was presented to the owner with what the
host reported about the target, saved sector for sector on the host and
on the recovery stick, and done from recovery with recovery the saved
default until the write was verified.

## What the machine proved

The partitions as mounts, the lag device's timing, the FAT32 scratch
volume, the exFAT stick with a GUID table and with an MBR in every
socket, pulled under a mount and plugged back, the stick image booted
from the firmware, the hub with the keyboard, the mouse and the stick in
every socket, the translator's case shown by the controller refusing a
slot context without it, the ext2 root with a trial, a deliberate
fallback, a commit and a power cut, the acceptance script end to end,
and the machine building its own kernel, utilities and shell from its
staged sources and running the kernel it built. `hardware.md` has every
reading.

## What the machine showed that QEMU had not

Twelve differences, each recorded in `notes.md` with its state. The
hub's ports powered one by one, since a ganged hub leaves the others
off. The USB controller's PCI line taken when messages are off, where
the timer's pump alone read a stick at 103 KB/s; on the line it reads
at 21 MB/s. The microsecond wait's sample of the timer chip torn by a
handler's own latch, which was the clock self-test's intermittent
failure. The FAT32 writer writing every cluster's table entry through
on its own, sixty-four synchronous writes per 64 KiB, now one block at
a time, and the cache handing out a blank buffer for a block written
whole. The SuperSpeed hub, driven by USB 3's rules after a review
caught the detection reading the wrong descriptor. A stick behind that
hub present at boot needing five seconds for its configuration. A boot
that lost every USB device to a command nobody aborted, one boot in
four, now aborted from the pump's clock with the enumeration turn taken
only on a free ring. A stick that stopped answering behind the
SuperSpeed hub, now handed back for a port reset. The Doom port
reading track 0 of a MIDI file only, so Freedoom's music sounded no
note and its restarts took the processor; every track is played now
and the game runs at sixty frames a second. The port's zone grown
through brk from a fixed 8 MiB heap. Records that say why: the clock
self-test, a refused configuration, the frame pool.

## What remains open

The panic at a pull of a stick that had stopped answering, whose
record the screen did not keep; the Mac now captures the machine's
netconsole frames for the next one. Root's SSH key refused for the
first minutes after a boot from the metal stick while the test
account's was taken. The hardware clock, which holds local time. A host
row for the Doom port's multi-track player, which needs a `GENMIDI`
lump the tree does not carry. The frame exhaustion under the lag
device, seen once and not reproduced in two trials with the record in
place.

## Trust

What this milestone proves is proved on one machine, the owner's Lenovo
G50-70 with its Samsung SSD, its RTL8168GU, its Intel xHCI, the owner's
USB 3.0 hub, keyboard, mouse and SanDisk stick. The translator, the
SuperSpeed hub, the controller's line interrupts, the stick's rates and
the real timer chip's behaviour are proved there and nowhere else; QEMU
models none of them. What QEMU proves, the suites prove on every run.
The open items above are limits of what was seen, not of what is
claimed.
