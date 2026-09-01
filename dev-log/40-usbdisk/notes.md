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

## Open items

(none)
