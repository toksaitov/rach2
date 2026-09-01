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

## Open items

(none)
