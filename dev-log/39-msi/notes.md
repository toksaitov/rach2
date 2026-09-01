# Milestone 39 notes

## Phase 1 inventory

Base: a clean tree on master at the close of milestone 38.

```text
2aa267d Record milestone 38
d11f30f Fail the suites' walks when the checker says nothing and name exfat in /proc/filesystems
2baf337 Add the rows the review round asked for and align the host reader
2e733c4 Fix the review round's findings in the two FAT drivers
b920f47 Record phase 6 of the exFAT milestone
cd2dcc7 Add the exfat suite
0c8710a Build an empty exFAT volume and read a file out of one
fd601b2 Let the write probes name the file-system type
ddf3ce3 Grow an exFAT directory when its chain ends past the end marker
b6a02f2 Record phase 5 of the exFAT milestone
a69cd0a Add the exFAT dirty rows and the repair matrix
a3b1117 Build the dirty exFAT fixtures and name duplicate names in the reader
```

What the tree has today: the 8259 pair remapped to vectors 32 to 47 in `sys/pic.c`; stubs for 0 to 47 and 128 in `sys/isr_handler.s`; `_irq_router` in `sys/isr.c`, which acks the master (and the slave above 39) and then dispatches the chain that `register_isr_handler` fills; the PIT at 100 Hz on vector 32; the keyboard on 33, the mouse on 44, SB16 on its own, HDA on `32 + irq` beside its timer servicing. No local APIC code, no MSR helper, no `/proc/interrupts`. `cpuid` is written out three times, in `hda.c`, `ahci.c` and `lib/krandom.c`; this milestone gives it and `rdmsr` a home in `lib/kcpu.h`.

## Review checkpoints (fixed now; Codex units, which the owner's cap does not count)

The round after phase 4: unit A, the APIC and the vectors: lapic.c/h, isr.c, isr_handler.s, lib/kcpu.h, lapic_hosttest.c; unit B, MSI on a device and the xHCI: pci.c's MSI half, pcimsi_hosttest.c, xhci.c's interrupter and handler, xhci_hosttest.c's new rows; unit C, the suites: usb.sh's new boots, the audio count, metal_accept.sh; unit D, the proofread of the reader-facing documents.

## Open items

(none)
