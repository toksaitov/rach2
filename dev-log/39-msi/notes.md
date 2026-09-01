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

## Phase 1 record

`rach2.apicdiag` (`sys/apicdiag.c`) reads and prints: the two `cpuid`
bits, `IA32_APIC_BASE`, the APIC's identity, version, task priority,
spurious vector and its two local-vector entries, and the MSI and MSI-X
capabilities of the xHCI and HDA controllers. It writes nothing. The
`cpuid`, `rdmsr` and `wrmsr` helpers the plan puts in a header live in
`sys/lib/kcpu.c` behind `kcpu.h`, because a static function in a header
is a warning in every file that includes it and does not use it; the
three copies of `cpuid` in `hda.c`, `ahci.c` and `krandom.c` are phase
5's sweep.

The readings are in `hardware.md`. What they changed:

* QEMU's `qemu-xhci` has no MSI capability by default: it offers MSI-X
  alone. With `msi=on` it has MSI at 0x70, one vector of sixteen
  offered, 64-bit address, and MSI-X at 0x90 beside it, which is the
  case the plan's ordering rule is written for (MSI-X disabled first).
  The USB suite's device lines gain `msi=on` in phase 3.
* HDA in QEMU has MSI at 0x60, one vector, 64-bit, and no MSI-X.
* The specification numbers in the plan check out against the machine:
  the APIC page at 0xFEE00000, bit 11 of `IA32_APIC_BASE` as the enable,
  bit 10 as x2APIC, the identity at 0x20 in bits 24 and up, the version
  at 0x30, the task priority at 0x80, the spurious register at 0xF0 with
  bit 8 as the software enable, `LINT0` at 0x350 and `LINT1` at 0x360
  with bit 16 as the mask and bits 8 to 10 as the delivery mode. The MSI
  capability's control word at offset 2 has the enable in bit 0, the
  count offered in bits 1 to 3, the count used in bits 4 to 6 and the
  64-bit flag in bit 7, and MSI-X's has its enable in bit 15.
* The kernel log's formatter has no width or padding: `%08x` is printed
  verbatim and its argument is then read as the next conversion's, which
  cost a page fault at the address that argument held. The first draft
  of the report used `%08x` and `%02x`; they are gone.

## Open items

(none)
