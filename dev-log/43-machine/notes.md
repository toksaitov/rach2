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
3. The boot log named a high-speed hub "a full-speed device" at its root
   port. A USB 2 port's speed field is valid only once the port is
   enabled, after its reset; the record now says a low-speed device by
   its pull-up and, for the rest, that the reset tells full from high
   speed. File: `rach2/sys/usb/usb.c`.

### Open

* On the trial boot with `rach2.nomsi rach2.blocklag=30`, the first SSH
  session's `bootctl status` was killed at 112.83 s: three `vm: user
  page: no free frames in 8192` records and `vm: fault: killing pid 28:
  bad write at 0x8124224 (err 0x7)`, with `MemFree` at 18952 kB read a
  minute later and every later command, the same `bootctl status`
  included, running with no fault. The same boot line on the sealed disk
  in QEMU with 32 MiB shows nothing of the kind. Something held every
  user frame for a moment at the first login under the lag. To be found
  in phase 7, from the allocator's side: which pool the message counts
  and what filled it.

### The review round after phase 1

One Codex unit over the four commits (f3d4abe, 40e7055, 8dccf4b,
1b7aafd), seven findings, all fixed in the phase:

1. The lag check accepted any count and ran `touch` after a failed
   `dd`. It now requires the read to succeed, the count to be nonzero,
   and the read to take at least 16N ticks by the machine's clock for
   `rach2.blocklag=N`; a mutation forcing N=30 on a plain boot fails it
   at 17 ticks.
2. `bPwrOn2PwrGood` can say 510 ms; the clamp was 500. Now 510.
3. A hub with no power switching had its wait lengthened by a tick,
   since the tick that set the wait from `HUB_TELL` had not been counted
   yet. The fresh-tick rule now applies only to a wait set by a
   completion.
4. The boot partition row never checked that the mount was read-only.
   It reads `/proc/mounts` now.
5. Missing partition nodes were a note on a machine with a slot report.
   A machine whose `bootctl status` answers owes `disk0p1` to `disk0p3`.
6. The usb suite's header said fifteen boots and eight without the 8042;
   the Readmes said twenty and eleven; the script has twenty-one
   invocations. All three now say twenty-one and eleven.
7. The explanation of the ganged hub misstated 11.11.1: one request
   energises the gang, and every other port stays in Powered-off until
   it has a request of its own. The comment and the Readme say that.

Checked and not a finding: the flag guard and boot 13 discriminate; the
Lenovo rows fail against the old code; per-port and unswitched hubs keep
their behaviour; the awk and shell are POSIX and BusyBox's; no prose
violation.

Open from this phase: the acceptance script's disk section fails the
installed system (`policy read-write`, the root mounted), since it was
written for the stick boot; phase 6 runs the script from the installed
root and owns that line. The frame exhaustion under the lagged boot,
above, gained facts: `bootctl status` holds about 9 MB while it runs,
under `rach2.blocklag=30` it runs for minutes, and two of them at once
exhaust the user frames; the first kill at 112.83 s had no second
`bootctl` and is still unexplained.

## Phase 2 record

The recovery stick: `build/images/rach2-recovery-stick.img`, the
recovery ISO (SHA-256 `533b85ca...`) with two MBR entries written by
`mkmbr`, the ISO area as type 0x17 and an 8 GiB FAT32 partition at
sector 32768 as type 0x0C, formatted on the mac mini with `newfs_msdos`.
The owner approved the SanDisk by name and wrote it with `dd` on
2026-09-03. The notebook booted it at the BIOS menu; recovery took
192.168.0.106 by DHCP with its own host key, listed `/dev/disk0` with
three partition nodes and the stick as `usb0` with two, and mounted the
stick's second partition writable at `/mnt/save`.

Sector zero, read from `/dev/disk0` in recovery: SHA-256
`f00ee34d3209ae7bd079155c3d48e4fd1be9a8003a3f3897d327e4aaa7145e8d`,
saved as `saved/sector0-before.bin` here and on the stick. Its table:
entry 1 type 0x0C at byte 1048576 for 67108864; entry 2 type 0x83 at
68157440 for 524288000; entry 3 type 0x0C at 592445440 for 67108864;
entry 4 empty. Milestone 36's `hardware.md` recorded entry 2 as 0x7f;
the machine and `mkdisk_grub.sh` say 0x83, and that record is corrected.

The new sector zero, `saved/sector0-new.bin`: the saved one with only
the sixteen bytes of entry 4 changed, to type 0x0C at sector 1290240 for
4194304 sectors, `00feffff0cfeffff00b0130000004000`; SHA-256
`a02966c9f463e19a2441eb8c983ddc83b9a06649b95ad4c1335a86b609946698`.
`mkmbr --list` reads the four entries as intended and the first 494
bytes and the signature are byte for byte the saved sector's.

The 2 GiB the partition will cover, sectors 1290240 to 5484543, is not
empty: it hashes to
`6c42495130749784052d2fb6566afc0923cb8cbf24fa35357f7c2a851c6ed06d`.
Its copy is written to the stick by the machine at about 0.6 MB/s and
then read from the stick on the mac mini for the host's copy, since the
same copy over SSH ran at 0.4 MB/s.

The FAT32 volume: `mformat -C -F -T 4194304`, 8-sector clusters, 32
reserved sectors, two FATs, 523261 free clusters, checked by
`mkfatfixture --check`; SHA-256
`73387be9570663858f4b7b6ba282a5cb16374ee9fa34374fec2136e14a6a5563`.
It goes to the machine gzipped, through `reflash`, as the Readme's root
procedure does.
