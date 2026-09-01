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
comes up is given the lowest free number of two; the reading of its table
happens in a syscall, not in the pump, because a transfer to a stick sleeps
its caller and the pump runs in an interrupt. `usbdisk_service` is called from
the syscall entry and returns at once when there is nothing to do.

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

## Open items

* The phase 3 proof also asks for a raw read of the stick's first sector
  through `/dev/usb0`. That is `usbdiskprobe volumes`, which reads the table
  back and passes.
* `usbdisk.c`'s scan reports an unpartitioned volume as `mbr` with no
  partitions, since an exFAT boot sector carries the same `0x55AA` an MBR
  does and its four entries read as empty. The behaviour is right and the
  record is not: the log line should say `one volume`, which is what
  `volume_boot_sector` is there to decide.
* Phase 4's remaining work: the harness `drive=add:`/`drive=del:` steps, the
  suite, and the eight mutations the plan names.


