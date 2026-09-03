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

### Phase 3

* Under `rach2.nomsi` the stick is eighty times slower than under MSI:
  103 KB/s raw against 8.4 MB/s in a high-speed socket and 23.2 MB/s in
  the SuperSpeed one. The timer is the only pump without messages, and
  `/proc/interrupts` shows no line interrupt for the controller and a
  fallback count of zero, so every stage of every transfer waits for
  the next 10 ms tick. A 32 KiB read costs about 310 ms, which is some
  thirty ticks. Fixed in phase 7: with messages off the controller
  raises its interrupts on the PCI line the firmware routed for it, IRQ
  7 on the notebook and IRQ 11 under QEMU, where it shares the line
  with the network card; the acknowledgement clears the status
  register's event bit as well as the pending bit, since the line is
  level-triggered, and the pump's test for an interrupt already raised
  reads the 8259's request register on that path. Under QEMU the 16 MiB
  read that took two minutes on the timer's pump takes 340 ms on the
  line. Rows: the usb suite's nomsi boot requires the line's count and
  no fallback; the controller's host test has the line's enables, the
  event bit's clearing and a device that names no line. Mutations: the
  line refused by name fails the boot; the event bit left set fails the
  host row.
* The clock self-test fails on some boots of the machine and passes
  on others: 2018 us on the MSI boot of this phase, 1361 us on the
  nomsi boot, 1492 us on the first ext2 boot, 2019 us and a pass in
  phase 1. Fixed in phase 7: the wait's sample of the 8254 latched the
  counter and read it with interrupts enabled, and a handler that
  latches the counter itself, the tick's clock or the USB handler's
  stamps, between the latch and the reads consumed the latch, so the
  reads came back live and torn across a borrow, 256 below the truth;
  the wait overcounted the drop by that much and ended early, about six
  tears in the 639 us the nomsi boot was short. The sample now runs with
  interrupts off. Row: `udelay_hosttest`, a model of channel 0 with a
  handler that consumes a latch left open at a read with interrupts on;
  mutation, the interrupts left on, caught by two rows. The self-test's
  record now names the check that failed and the last walk's readings.
* The acceptance script's stick section read the line of `usb0` only.
  The machine gave the data stick `usb1` when the recovery stick's slot
  had not been reclaimed yet, and `usb0` after the slot came back. Fixed
  in phase 6: the section uses whichever slot holds the stick.

* The machine's clock runs six hours ahead of UTC. The Lenovo's RTC
  holds local time, +06, as firmware set up for Windows does, and the
  kernel reads it as UTC, so `date` on the machine says 17:42 UTC at
  11:42 UTC and the files it writes to the exFAT volume show on the Mac
  six hours in the future. The kernel has no notion of a zone. To decide
  in phase 7: a boot flag for the RTC's offset, or the limit stated in
  the RTC's Readme with the owner setting the RTC to UTC.

* On the stick boot, SSH as root with the demo key was refused for
  the first three minutes after the boot, at about 60 s, 100 s and
  170 s of uptime, with the key offered and the server answering
  `publickey,password` again; the test account's key was accepted at
  about 150 s, and root's key was accepted at 230 s and after. In
  between, a `su` from the test account read `/root/.ssh`. The same
  image accepts the key at once under QEMU. Phase 7 reproduces it on
  the machine with `dropbear -v` on the console.

### Phase 4

* In the blue socket the owner's USB 3.0 hub shows two devices, the
  high-speed hub on port 2 and the SuperSpeed hub on port 10, and the
  stick, a USB 3 device, attaches to the SuperSpeed half and is absent.
  The keyboard and mouse, USB 2 devices, are on the high-speed half and
  work. Milestone 41 recorded the SuperSpeed hub as not driven; this is
  the consequence for a stick behind it. The owner decided on
  2026-09-03 that this is work of the milestone and not a limit. Done
  in phase 7: the hub driver drives a SuperSpeed hub by USB 3's rules,
  the USB 3 descriptor, `SET_HUB_DEPTH` before the controller is told
  it is a hub, no translator and no think time, SuperSpeed for every
  device below it, and the warm reset, link state and configuration
  error changes acknowledged with their feature numbers; the core
  accepts protocol 3. Rows: the hub host test's SuperSpeed section,
  eleven checks and a hub that refuses its descriptor. Mutations: the
  depth not sent, the USB 2 descriptor asked, the speed read from the
  status bits, the link state change not acknowledged; each caught by
  its row. A Codex review of the commit found the detection wrong:
  USB 3.0 10.13.1 puts a SuperSpeed hub's protocol 3 in the device
  descriptor and leaves the interface's at zero, which is what the
  notebook's log showed for port 10, so the driver would have asked the
  USB 2 descriptor of the real hub and closed it. The hub is now known
  by its speed. The review also found no warm reset for a link fallen
  to SS.Inactive, a host model shaped like a USB 2 device, a row that
  proved one of the three change bits, and a depth-refused row that
  refused the descriptor instead; all five fixed, with the warm reset's
  row exposing a core defect beside it: a device pulled during its
  reset through a hub kept the enumeration turn waiting for a command
  that was never issued, and every later device waited with it. Fixed
  in the core with its own row. Mutations for the round: SuperSpeed
  taken from the interface protocol, no warm reset, the configuration
  error bit not acknowledged, the turn kept; each caught. Proved on the
  machine in the install round: the stick behind the hub in the blue
  socket enumerates as a SuperSpeed device on the USB 3 hub, mounts,
  reads at 22 MB/s and survives a pull. The pull showed one more case:
  the hub reports the link fallen inactive before the disconnection, so
  the warm reset asked for never ended on the empty port and the replug
  was not seen. A connection change now ends a warm reset, with a row
  of its own, and the replug is enumerated in 0.3 s.

* The stick on the hub when the machine boots is refused: `usb:
  device: port 1.1: it refused its configuration` at 1.95 s, 0.66 s
  after the hub's descriptor, on two boots in port 1, with the keyboard
  and mouse on the same hub enumerated at 1.99 s and 2.03 s. The same
  stick on a root port at boot enumerated at 1.41 s, and plugged into
  the hub after the boot it enumerated in every socket. Found in phase
  7 once the record said how the transfer ended: no answer within the
  step's 500 ms, a timeout and not a stall, the stick's controller
  still starting its flash. The configuration request now gets five
  seconds, as the Linux driver allows it; row in the hub host test, a
  device answering after 1.5 s; mutation, the half second back. On the
  machine the stick on the hub at boot enumerates.

* Writes through the FAT32 driver to the stick's 8 GiB save volume are
  slow and get slower: 0.65 MB/s falling to 0.17 MB/s by 900 MB in
  phase 2, and about 0.05 MB/s for a 64 MiB copy in phase 5 with a
  second writer beside it. The same stick reads raw at 8.4 MB/s and
  takes exFAT writes at 2.1 MB/s. Measured under QEMU: a 4 MiB FAT32
  write ran at 1.2 MB/s against exFAT's 8.5 MB/s with one flush, so
  it was the writer's requests and not the stick's cache: every cluster
  of a run cost a write-through of its FAT entry in each copy and
  another to chain it, sixty-four synchronous writes per 64 KiB append
  on 4 KiB clusters, and a removal the same per cluster. Fixed in phase
  7: the entries of a run allocated or freed together are set in the
  cache and written through one FAT block at a time, per copy, with the
  run whole on the disk before anything names it; and a block a writer
  replaces whole is taken from the cache blank instead of read first.
  Under QEMU the write is 2.9 MB/s and the disk commands of a 4 MiB
  write fell from 69265 to 3857, of its removal from 32821 to 53. Rows:
  the fatwrite suite's BusyBox boot bounds both counts; the cache's
  host test has the blank get. Mutations: an entry written through on
  its own fails the bound; a blank get that reads fails three rows. The
  fall over the file's length is measured on the machine in the next
  install round.

* Doom with the 28 MB Freedoom IWAD is slow and its sound breaks up
  on the machine, bits of sound between silences whenever the game
  struggles to render; with the 4 MB shareware IWAD it plays as it did
  before. The kernel manages 32 MiB, so a large IWAD pages through the
  disk, and the owner decided on 2026-09-03 that this is work of the
  milestone. Found in phase 7, under QEMU with the Freedoom WAD: the
  port's zone, where a level's lumps live, was 6 MiB from a fixed 8 MiB
  heap in its own crt0, while the kernel's brk heap went unused; the
  heap now grows through brk and the zone is sized from MemFree less
  8 MiB, 12 MiB on a 32 MiB machine, with `tests/run doom` as its row
  and the fixed heap as its mutation. The disk traffic did not move
  with it, 3843 commands in 25 s of the demo loop against 3957, about
  0.6 MB/s of level loading that an SSD serves without effort, so the
  machine's slowness is not the reads: what remains to measure on the
  machine is the frame time with the large levels and the OPL3 music
  emulator, since the sound is fed from the game loop and a slow frame
  starves it. That measurement is the install round's.

### Phase 5

* The disk's recovery partition, written in milestone 37, has no
  `e2fsck`; the tree's recovery image has had it since milestone 42.
  The plan's repair of a dirty root from recovery works from the stick
  only. Phase 7 presents the write of the current recovery image into
  partition 3, by the Readme's procedure, with its saved copy; until
  then the stick is the recovery with `e2fsck`.

### Not a difference

The first ext2 root written in phase 5 was `build/images/os-real.ext2`,
the plain image whose init starts no network and no SSH server, and the
trial boot came up on ext2 at the console with no address and no
server. The Readme's "Updating the installed machine" names the right
image, `build/images/rach2-metal-disk.ext2`, built from the metal-disk
manifest with the metal init; the session read the Makefile instead of
the Readme. The root was written again with the right image. Nothing in
the tree was wrong.

`fsck_exfat -n` run by the owner on the mounted data stick reported
the Mac's own Spotlight index file, `live.0.indexPostings`, with a
chain longer than its size. The volume was mounted and Spotlight was
writing that file. `diskutil verifyVolume`, which unmounts first, found
the volume clean before and after that run, and the kernel never opened
the file.

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
  user frame for a moment at the first login under the lag. The message
  counts the whole pool of 8192 frames: the boot's reservations, the
  kernel's frames from the top, the page cache's at most 1024 among
  them, and the user frames between. The record now says how many of
  each and which process asked (phase 7); the lag boot is rerun on the
  machine in the install round to read it.

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

Approval, 2026-09-03: the owner approved `/dev/disk0` on the Lenovo by
name for both writes, the fourth entry in sector zero and the FAT32
volume into its range, after the table above was shown.

The copy of the 2 GiB the fourth partition covers was cut to what the
write changes. The FAT32 image is zero past its first 8232 sectors, the
32 reserved sectors, the two FATs of 4096 sectors and the root cluster,
which a byte scan of the image showed; a free cluster's content is not
part of a FAT volume, and the driver zero-fills clusters it allocates.
So the write became `reflash /dev/disk0 660602880 4214784`, and the
sectors it overwrites are inside the first 16 MiB of the range, saved as
`saved/region-head-16MiB-before.bin` on the host, SHA-256
`2c1c569a332417e6bc29e1042ba7c6b386cadf984e1d5f170ef0bf6aa1aa15ce`, and
as the head of `region-before.bin` on the stick, which the machine's
`dd` through the FAT driver had reached 954 MB of when it was stopped;
the machine hashed both heads and the disk's to the same value. The
full copy through the driver ran at 0.65 MB/s at first and 0.17 MB/s
after 900 MB; the raw rate of the stick is phase 3's to measure, and
whether the fall is the stick's flash or the writer's ordered flushes
is decided there.

The writes, 2026-09-03 from the stick's recovery: recovery made the
saved default; the new sector zero written, read back as `a02966c9...`;
the saved sector written back and read as `f00ee34d...`; the new one
written again and read as `a02966c9...`. The metadata written in 13 s
and read back as `07c73b27...`, the image head's own hash. The volume
with a zero tail passes `mkfatfixture --check`.

The trial of the ordinary root: `bootctl trial candidate` from the
stick's recovery is refused, since that boot line has no slot, so the
saved default booted recovery from partition 3 (an older recovery
kernel, with its own host key) and the trial was armed there. The
candidate booted with `/dev/disk0p4`; the volume mounted for writing,
took a file, a rename, a removal, a 4 MB copy and `sync`, was unmounted
and mounted again with the file and the copy intact, and the
acceptance script's scratch line passed. Its head pulled to the host
lists with `mdir` and checks clean with three files. The boot
partition's kernel bytes, fallback and `grub.cfg` hash as before the
step, the recovery partition hashes as before, and the KRFFS root, the
live root, differs by its own use. The saved default is the candidate
again.

Noted for phase 6: the acceptance script's stick section wrote to the
recovery stick's ISO partition and failed; the script must leave a
recovery stick alone. The script's scratch line left `accept.txt` on
the volume; fixed in 9b7bf52. Phase 6 fixed the stick section, the
disk section's demand for the read-only kernel, and the ext2 section's
reading of a clean check; the script passes on the machine and in QEMU.
