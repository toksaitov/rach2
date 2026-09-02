# Milestone 43 notes

## Phase 1 record

Arrival on 2026-09-03 at `dd6ca64 Record the ext2 milestone` with a
clean tree. The twelve commits before it:

```
dd6ca64 Record the ext2 milestone
12af691 Read e2fsck's repaired status inside the sealed suite's own test
d7dde10 Name the boot's skips in the order the boot prints them and source the tool path only where the tree has it
af0dd08 Retire the scratch suite, whose fourth partition is the ext2 root now
dde6a30 Say what the code does about the commit, a staged menu, a failed trial and a pulled drive
d3f5651 Scope the ext2 judges to their sequences and make every row fail without its fact
e0823b4 Close the driver's review findings: the pool under the lock, ENOSPC before a bitmap, faults marked, counts restored
1a1a7ac Hold the ext2 root's syscalls to their contracts: set-id bits, umask, fchmod, the cwd, the lag and the raw writers
ba0a11c Check the ext2 root in the metal acceptance for milestone 43
cab1562 Describe the ext2 root, its update from recovery and what KRFFS is for now
ec6e11f Fold the ext2 driver's three one-caller helpers into their callers
73c5e6b Place the sealed disk's ext2 partition where the installed machine's fourth begins
```

### What the owner provided

The machine at 192.168.0.106 on the LAN, booted in the ordinary mode; a
USB keyboard and a USB mouse in two of its sockets; an unpowered hub; and
one flash drive, a SanDisk of 61.5 GB, on the development host. The plan
asked for two drives. With one, the same drive is the recovery stick
through phase 2, the data stick through phases 3 and 4, and the recovery
stick again from phase 5, which is when the plan rewrites the recovery
ISO in any case. The saved copies the disk rule requires on the stick go
on a writable partition after the ISO, so the copy on the stick exists
from before a write until that write is verified; a phase that reformats
the drive starts only after the preceding phase's writes are verified
and recovery is no longer the saved default.

## Differences

### Phase 1

1. Every USB keystroke printed milestone 39's latency record to the
   console. The record was unconditional; it now prints only with
   `rach2.usblatency` on the boot line. The usb suite's latency boot
   asks for the flag, and its typing boot fails on a record without it.
   Files: `rach2/sys/usb/usb.c`, `tests/system/suites/usb.sh`,
   `rach2/sys/usb/Readme.md`, `rach2/Readme.md`.
2. The owner's hub, 35d6:2510, carried nothing: a keyboard and a mouse
   plugged into it were never seen. A diagnostic kernel, booted as a
   trial and not committed, read the hub's descriptor: power switching
   ganged, power good in 0 ms. After the one ganged power request the
   scan ran at once and found port 1 powered and ports 2 to 4 not, so
   nothing plugged into those ports could raise a change. The driver now
   powers every port with its own request whatever the mode says, and
   waits at least 100 ms for power, the floor Linux uses. The host model
   has the hub's row: ganged, 0 ms, a port powered only by its own
   request, a keyboard on port 3. Files: `rach2/sys/usb/hub.c`,
   `tests/host/hub_hosttest.c`, `rach2/sys/usb/Readme.md`.

### Not a difference

Doom with the 28 MB Freedoom IWAD is slow and its sound breaks up on the
machine; with the 4 MB shareware IWAD it plays as it did before. The
kernel manages 32 MiB, so a large IWAD pages through the disk. The
owner wants this looked at: a WAD larger than the RAM the kernel manages
is a limit to state or a change to plan, after this milestone. Recorded
in `hardware.md`; not fixed here.
