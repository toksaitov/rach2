# Milestone 40 notes

## Phase 1 inventory

Base: a clean tree on master at the close of milestone 39.

```text
4aa5e60 Record milestone 39
a908fb2 Record why the milestone's interrupt material stayed in the kernel Readme
7ef96d2 Fix what the review round found in the suites and the documents
5c6071c Fix what the review round found in the APIC and the message path
41f890f Size /proc/interrupts for its worst case and bound it at compile time
8064335 Recast the phrases the register rule reviews in this milestone's prose
78ead71 Take the three copies of cpuid down to the one in kcpu
9e35dbb Record phase 4 of the MSI milestone
664eb05 Give the audio controller a message vector beside its legacy line
1b03dfb Record phase 3 of the MSI milestone
140a8ed Count the timer chip's periods from whichever reads them first
9edc9fb Take the xHCI's interrupts as messages, with the timer as the fallback pump
```

## Review checkpoints (fixed now; the units are named when each round starts)

The gate round after phase 5, on the disk half: unit A, the controller
interface and the tables (hcd.h, xhci.c's bulk and generation halves,
xhci_hosttest.c's new rows); unit B, the class driver (msc.c, msc_hosttest.c);
unit C, the block device, its queue and removal (block/request.c, block's
device table, bcache's removal rows, core.c's closing state); unit D, the
partition tables and the nodes (gpt.c, gpt_hosttest.c, devfs's usbN nodes).
The round before the sweep in phase 7: unit E, the suite and the probe; unit F,
the proofread of the reader-facing documents. Two units run at once, which is
what CLAUDE.md allows and what this plan's "at most five" is overruled by.

## Phase 1 record

Three stick images were built by hand and read back, with the tools the tree
already has: `build/tools/image/mkmbr` for a table, `mtools` for FAT32,
`build/tests/host/mkexfatfixture` for exFAT and for reading a volume back at
an offset.

* An MBR stick, 64 MiB: type 0x0C at sector 2048 for 65536 sectors, formatted
  by `mformat` and given a file that `mdir` reads back; type 0x07 at 67584
  for 32768 sectors, the fixture's own volume, which `mkexfatfixture --check`
  walks in place and finds whole.
* A GUID stick, the same two volumes, with a protective MBR of type 0xEE and
  a table whose first used entry is at index 6, so that a reader which took
  the first four entries of the array would find nothing. Both volumes read
  back. The table was written by a scratch script, since the tree has no GPT
  writer; the suite's builder is written with the suite.
* A superfloppy, the fixture's 16 MiB volume from sector zero, which checks
  whole.

The superfloppy is the case that decides how the scan tells a table from a
volume: an exFAT boot sector has the same `0x55AA` at offset 510 that an MBR
has, and its partition entries read as sixteen zero bytes each. A scan that
takes `0x55AA` for a table finds four entries of type zero; that is the state
in which sector zero is offered as a volume of its own.

`tools/image/mkiso.sh` produces a hybrid ISO whose first sector is GRUB's
hybrid boot code with a valid `0x55AA` and a partition table of four zero
entries, and there is no GUID table behind it: sector 1 is the ISO's system
area and holds zeros where a header would be. So the stick image of phase 6
does not keep a first entry, because there is none; it writes the ISO's own
entry and the exFAT volume's beside it.

## Phase 2 record

`usb_transfer` carries a generation and `usb_completion` gives it back; the
controller keeps one per endpoint, which is enough because one transfer is in
flight on an endpoint at a time. Endpoint numbers run to 15 in each
direction, so the device context index runs to 31 and the tables grew with
it; the sizes are in `hardware.md`. The slot count and the device table are
eight.

The close that never answers changed. It used to give the slot back and free
the entry with a record, which hands the next device rings the controller may
still be reading; now the entry becomes a tombstone, the slot is never given
back, and the record says the entry is lost. Five seconds is the deadline,
not one step, because the answer is a command like any other.

The rows are proved by four mutations: the generation not kept, a bulk
endpoint opened with an interrupt endpoint's context type, an endpoint above
three refused, and a handle past the tables addressed, which the sanitizer
stops at the read that the bound exists to prevent.

## Phase 3 record, part one

The core's walk of a configuration descriptor learned the second interface
this kernel drives: class 8, subclass 6, protocol 0x50 on alternate setting
zero, with the first bulk endpoint each way. A stick's two endpoints are two
commands, so enumeration gained a state between them, and a stick reaches
`ST_RUNNING` without the protocol request a keyboard takes.

`sys/usb/msc.c` is the class driver: the three transfers of a command, the
bring-up sequence, the transfers themselves, and the recovery the class
defines. What the field taught is in the code and named there: the drive that
fails `TEST UNIT READY` for a while, the one that stalls `Get Max LUN`, the
one that reports its last block rather than a count, the promotional drive
with a CD-ROM in front of its flash, and the reset that is recovery and never
initialisation.

The recovery's two clears go through `usb_core_clear_halt`, which takes the
core's own turn at the controller's one command. A class driver that issued
those commands itself would take the answer to somebody else's: the core
shares the command ring out, and this is how a driver borrows it.

A stick is a `block_device` with `submit`, `flush` and no fault-safe flag,
an admission queue of sixteen so that two processes can each sleep in a
request, and the block layer's own page, deadline and quarantine unchanged.
`struct block_device` gained `flush` and a read-only flag; the four tests
that build a device by position were written out member by member, so that
the next member added is a compile error rather than a silent shift.

`tests/host/msc_hosttest.c` drives the driver against a model of a
Bulk-Only device: a stick that comes up at once, one that fails twelve tests
first, one that never becomes ready, `Get Max LUN` stalled, a CD-ROM in
front of the flash, an empty card slot, the two capacities that are refused,
write protection with its refusal, a status wrapper naming another command
with the reset and the two clears in order, a device pulled during the data
stage, two requests admitted in order, and a flush.

In QEMU a stick attached to the emulated controller is enumerated and brought
up: `usbdisk: ready: device 1: lun 0, 131072 sectors of 512 bytes`, which is
the 64 MiB image the phase-1 tables describe.

Ten mutations prove the rows: the capacity taken as a count, a block size
other than 512 accepted, a unit that is not a disk taken anyway, the two
halts cleared the other way round, a stalled status wrapper retried without
end, a short read taken for a whole one, a status wrapper naming another
command accepted, a completion for an abandoned transfer taken as the answer
to the one in flight, a second request overwriting the first, and a reset
sent at initialisation. Each fails one named row and no other.

Two of the plan's mutations belong to milestone 36's own rows and are not
repeated here: the check of the done flag without interrupts off before
sleeping, and a completion delivered into a slot the caller has given up on,
which the request pool's quarantine already answers.

## Phase 4 record, so far

`gpt.c` reads a GUID table: the header judged before anything it says is
used, the entry array read whole and covered by its own checksum before a
single entry is looked at, and the first four used entries published wherever
in the array they lie. `gpt_hosttest.c` drives it against tables no disk would
carry.

`usbdisk.c` is the layer between the class driver and `/dev`. A stick that
comes up is given the lowest free number of two. Its table is read from the
pump without sleeping: `block_request_begin` starts one request of at most a
page and `block_request_ended` asks after it on the passes that follow, so
the reading is a state machine over the first sector, the GUID header and
the array a page at a time. The first design read the table at the syscall
entry, where a process can sleep; the suite's replug boot showed a stick
plugged in while the debug shell was idle waited sixty seconds for its
table, because no process entered the kernel until the harness gave up on
the step. Publishing the nodes and reclaiming a stick still wait for a
moment when no process is inside a syscall, which is either the syscall
entry or a pump pass that interrupted user mode or the idle machine; on one
processor a syscall is not preempted, so such a pass finds every kernel
structure at rest.

The teardown count is computed rather than kept: the mounts on the stick's
five views and what is open on them, the descriptions on its nodes counted by
walking the descriptor tables, its requests in flight or quarantined, and the
cache buffers held on it. A number kept up to date at every open and close
would leave a name taken for the life of the machine if it ever drifted by
one.

The class driver's slot is not freed when a stick goes. Its block device is
what a mount, a description and a held buffer point at, so `msc_detach` marks
the slot closing and `msc_release`, which only the disk layer calls, frees it.

`bcache_device_gone` is the cache's half: a clean unheld buffer is dropped, a
dirty one records the device failed and is then dropped, since an unmount
waits for the dirty ones and nothing will ever write them, and a held one is
marked to be dropped at its release.

`/proc/usb` gained a line per stick and a line counting the free names and the
sticks given up. The counting line is written only by a machine that has had a
stick, so a boot with no controller still has the file empty, which is what
`usbprobe` and the `rach2.nousb` boot require. `intrprobe` counted the file's
lines to count devices and now counts the lines that begin with `port`; that
was found by `tests/run usb` failing its fourteenth boot.

`tools/image/mkgpt.c` writes a GUID table into an image, since the tree had no
writer and the suite needs one to read. `tests/usbdiskprobe.c` has the parts
the harness runs between its plug and pull steps.

The three fixtures are built with the tools the tree has. The MBR stick, which
phase 4 was proved against:

    dd if=/dev/zero of=stick-mbr.img bs=1m count=64
    build/tools/image/mkmbr --lba stick-mbr.img 2048:65536:0c 67584:32768:07
    mformat -i stick-mbr.img@@1048576 -F -T 65536 -v STICKFAT ::
    mcopy -i stick-mbr.img@@1048576 hello.txt ::/hello.txt
    build/tests/host/mkexfatfixture exfat.img
    dd if=exfat.img of=stick-mbr.img bs=512 seek=67584 conv=notrunc

The suite found four more things, each fixed where it was found.

* An unmount of a mount whose stick has gone failed with `EIO`, because the
  unmount syncs, prepares and writes the cache back before it lets go, and
  each of those reaches a device that is not there. `BLOCK_GONE` is set on
  the stick's views by `bcache_device_gone`, and an unmount of such a device
  writes nothing and only lets go of what it held.
* The emulator drops a stick's medium with the device it was under, so a
  replug through the monitor is `drive_add` and then `device_add`; the
  harness gained `drive=add:` and `drive=del:` for it, as the plan said.
* With the controller's messages on, the first read of 16 MiB failed at once
  with `usbdisk: recovery: the command wrapper was refused`, every time, and
  never on the timer's pump. `msc_block_submit` ran in process context with
  interrupts on, and the emulator completes a transfer inside the doorbell
  write, so the interrupt for a wrapper arrived before `command_begin` had
  recorded the stage that sent it; the pump then read an idle stick with a
  completion for it, and the next wrapper went out into the middle of the
  command. The two process-context entries, `submit` and `flush`, now run
  with interrupts masked, which is the plan's "submission inside the pump"
  rule. The host test cannot see this: its model completes nothing until it
  is stepped.
* BusyBox's `blkid` found no volume on the stick. Two causes. The FAT32
  fixture had 64496 clusters, fewer than the 65525 a FAT32 volume must have;
  the kernel mounts it, but `blkid` judges by the cluster count, takes it for
  FAT16 and reads the label from the FAT16 field, which is empty. The
  fixtures' FAT32 partition is now 48 MiB. And the exFAT prober asks for the
  120 bytes of its superblock structure, which the whole-sector rule of the
  disk's windows refused, and a refused read marks the device unusable for
  every prober after it. The stick's nodes read any byte range, as a block
  device node on Linux does; the disk's windows keep their rule.

Throughput, 16 MiB read through `/dev/usb0` in 32 KiB pieces, measured in
QEMU on this host: 49648 KB/s over 330 ms with the controller's messages,
and 133 KB/s over 122880 ms on the timer's pump alone, which is exactly
three ten-millisecond ticks for each page-sized request: the wrapper, the
data and the status each wait for a tick. The suite's throughput boot runs
with messages; the timer measurement is this record's, because a boot that
takes two minutes is not one the core tier can hold.

`tests/run usbdisk` is eight boots and takes 103 s alone, which is inside
the core tier's rule; it runs alone after the pool like `tests/run usb`,
because both plug devices in and out through the monitor.

Nothing called a stick's `flush` until phase 4's mutations were laid out:
the class driver had `SYNCHRONIZE CACHE` and the block device had the
member, and no mount asked for it. `block_flush` sleeps its caller the way
`block_request` does, a partition view forwards it, and an unmount, a sync
and an fsync each call it after the cache has written back. A stick counts
the flushes sent to it on its line of `/proc/usb`, and the probe's write
part requires the unmount to have added one; `/proc/disk` counts them too,
but a machine booted from the RAM disk with no disk has no `/proc/disk`
counters at all.

Phase 4's eight mutations, each applied by a script that restores the source
under a trap and touches it so the rebuild is real, each failing exactly the
row named and no other:

* the entry array's checksum not checked: `a damaged entry array publishes
  nothing`, usbdisk_hosttest;
* a pulled stick still served: `and refuses what it is asked for`,
  msc_hosttest;
* the description count left out of the holds: `a stick with a description
  open is not reclaimed`, usbdisk_hosttest;
* the request count left out: `nor while a request of its is quarantined`,
  usbdisk_hosttest;
* a held buffer dropped at the removal instead of at its release: `the held
  buffer is still held`, bcache_hosttest;
* the class driver's slot freed at the departure: `its device is kept until
  the disk layer releases it`, msc_hosttest;
* the stick nodes reported as character devices: `the MBR stick boot`,
  tests/run usbdisk, at the probe's `/dev/usb0 is not a block device`;
* the flush skipped at the unmount: `the write boot`, tests/run usbdisk, at
  the probe's `the unmount did not flush the stick's cache`.

The seventh was first written in a form that did not compile, an unused
parameter under `-Werror`, and the script reported it as escaped; a mutation
that does not build proves nothing, and the script now keeps the run's
output so that case is told apart.

## Phase 5 record

`tests/run all` on the tree at the end of phase 4: all 65 suites passed.

The gate review of the disk half was two Claude reviewers at Opus strength,
six files each, run at once: the disk layer with the request pool, the cache
and the mounts; and the class driver with the GUID reader, the suite, the
probe and the disk layer's host test. Codex was not used: a Codex unit's
result cannot be read back from this session, which milestone 39 found and
recorded. Every finding was fixed before phase 6, and the fixes that change
what a test can see were each proved by a mutation.

The class driver:

* A check condition on a read or a write was reported as success. The retry
  path sent `REQUEST SENSE` and then judged the request by the sense
  command's own status wrapper, which is good. The sense is now read: a
  medium error fails the request, a unit attention has the command sent once
  more, and a `SYNCHRONIZE CACHE` answered with `ILLEGAL REQUEST` is a stick
  with no cache. The status wrapper's arm also cleared the retry flag before
  the arm that read it. Rows: the medium error, the unit attention retried
  once and failed the second time, the flush's three answers.
* A transfer that never answered had its request completed at the deadline
  with the controller still owning the transfer and the page, which the
  block layer gives to the next request. The request is no longer completed
  at the deadline: recovery is begun, its clearing of the endpoints takes
  the transfer back, and the request fails when recovery ends, late, which
  the pool's quarantine is for. Row: a vanished data stage, not completed
  before or at the deadline, a reset seen, then `EIO`.
* After a timeout the pass ended before the queue was dispatched, so a
  request submitted from a process in that window went out ahead of the
  ones waiting. The queue is dispatched at the end of every pass.
* Tags restarted at one on each attach while completions are matched by the
  controller's device handle. Tags now go up across every stick and every
  attach. Row: a second stick on the same handle starts past the first's.
* A flush was counted when its command was formed; it is counted when the
  stick answers it.

The disk layer and the cache:

* A held buffer marked for discard by a device that went, then dropped by a
  failed fill or reused after a writeback, kept the mark; the next user of
  that buffer, on a sound device, had its dirty block thrown away at its
  release. The mark is cleared where every buffer changes hands. Two rows,
  one per path, with the fake device pulling itself in the middle of a read
  and of a write.
* `settle` wrote `pending` from what its own pass had seen, and a stick that
  finished its table or went while it ran was written over and never
  published or reclaimed. The last word is decided from the states with
  interrupts off.
* A stick pulled during bring-up was never named, so nothing released the
  class driver's slot; two such pulls took both slots for the life of the
  machine. `usbdisk_left` releases the slot of a stick that was never named.
  Row and mutation.
* The read of a stick's table goes to the whole device and not a view, and
  a quarantined one was not in the teardown count. It is.
* The staging buffer of the raw windows was one buffer for everyone, safe
  while every window was over a device that polls; a stick's window sleeps
  inside its request with the sectors in that buffer. A transfer takes the
  buffer first and a second process sleeps until it is given back. No row:
  the interleaving needs two processes asleep in the kernel, which is the
  guest's; recorded here instead.
* The header's claim that publishing runs "where no process is inside a
  syscall" was wrong as written; what holds is that a syscall is not
  preempted and a sleeping process has recorded what it holds. The comments
  say that now.

The GUID reader bounds the whole array against the device in the header
check, not only its first sector; row and mutation. The probe's hold part
used a negative errno as a descriptor on one path in 128; its volumes part
now checks the table's entries and not only the signature; its report part
fails on a name count it could not read.

Two of the reviewers' prose findings were recast, and the second reviewer's
note that the model cannot see the submit race is recorded under phase 4.

## Open items

* The phase 3 proof also asks for a raw read of the stick's first sector
  through `/dev/usb0`. That is `usbdiskprobe volumes`, which reads the table
  back and passes.


