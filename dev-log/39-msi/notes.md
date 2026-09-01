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

## Phase 2 record

`sys/lapic.c` enables the APIC and refuses by name; `rach2.nomsi` is one of
the refusals. `LINT0` is written as an unmasked external interrupt when the
firmware left anything else there, and the boot asks the timer, straight
after the tick starts, whether the 8259's interrupts still arrive: 50 ms with
no tick writes `LINT0` again and says so, and 50 ms more with no tick says
that too. The mutation that masks `LINT0` behind the driver's back proves
both halves: without the repair the machine never finishes its self-tests,
with it the machine boots and the log says why.

Vectors 48 to 63 have stubs, gates and a branch in `_irq_router` that ends
the interrupt at the APIC after the handler chain. The APIC's spurious
vector, 255, has a stub and a gate of its own; without them a spurious
interrupt would be a general-protection fault.

`register_isr_handler` and `set_isr_handler` take the name the handler is
known by, because `/proc/interrupts` is worth little without it: the file has
one line per vector that has taken an interrupt, with the count, `PIC` or
`MSI`, and the names on that line. `/proc/stat`'s `intr` line is the total.
The `procfiles` suite reads both on the machine and checks the timer's line
against a count it takes from the same boot; three mutations (the name
dropped, the total left at zero, the per-vector count never incremented)
each fail exactly one of those rows.

A defect this phase found and fixed, outside the milestone's own work: a
record raised by an interrupt handler while another record was being written
out went to the ring and no further. The USB suite's replug row failed about
a third of the time because `usb: device: port N: gone` is raised from the
timer interrupt, and the machine is printing its self-tests at that moment.
`klog` now queues such a record and the delivery it interrupted writes it out
when its own line is finished. Ten unplugs in a row are now seen; before the
fix, four of ten were not. This is why phase 2 took longer than the plan
allowed for it.

## Phase 3 record

The controller's interrupter is enabled last in `usb_init`, after the devices
present at reset are enumerated, and `pci_msi_enable` gives it vector 48.
`usb_pump` is the one drain, turned by the handler and by the timer, with the
interrupt flag left as each context found it; the pump records once, at
runtime, if it ever returns with the flag raised.

Five things were learned at the machine rather than from the specification,
and each of them changed the code:

* QEMU's controller raises no further interrupt while the event-handler-busy
  bit is set, and the driver wrote the dequeue pointer only when its drain had
  taken something. A handler that found the ring already drained by the timer
  left that bit set and the controller went silent after one message. The
  write happens on an empty drain too whenever the interrupter is on.
* The acknowledgement and the drain are one call in the driver
  (`xhci_interrupt_take`), because that is the only way a host row can catch
  the order being reversed. With the drain first, an event written in between
  has its pending bit cleared without having been read.
* The controller's own pending bit is not the question to ask when the timer's
  pump finds work: QEMU sets it whether or not a message was sent, and a
  message that was sent is waiting in the processor's request register while
  this tick has interrupts off. `lapic_pending` asks the processor
  instead, and the fallback count is zero on a machine whose messages arrive
  and rises on one whose messages were silenced.
* A wake is worth nothing if the woken reader does not run. The router
  reschedules on the way out of a message vector unless it interrupted a
  process that is running inside its own system call; a process blocked inside
  one is not running, and switching away from it is the point. `keylatprobe`
  forks a child that spins in user mode, because without another runnable
  process the reader is the context the message interrupted and the reschedule
  cannot be seen to matter.
* The kernel had no clock finer than the 10 ms tick. `pit_now_us` reads where
  the 8254's counter is inside its period and counts the periods, and neither
  the tick nor the counter can count them alone: the counter says nothing
  about which period a reading belongs to, and two readings a period apart
  land in the same place, while the tick arrives a millisecond or two into the
  period it counts, so a clock built on it steps backwards there. Whichever
  sees the wrap first counts it, and the other does not count it again. The
  self-test takes the shortest of eight measurements of one wait, because a
  serial line takes milliseconds to write a record, and then walks fifty
  milliseconds a millisecond at a time with the tick measuring the same
  interval beside it. Three mutations fail it: the half of the period
  ignored, the tick counting a period a reading already counted, and a
  reading never counting a wrap.

The seven mutations the plan asks for each fail exactly one row: the bare
acknowledgement and the dequeue pointer without the busy bit fail
`xhci_hosttest`'s interrupter rows; the reversed order fails its injected
event; no end-of-interrupt at the APIC leaves the controller one message
instead of one per key, which `intrprobe` requires; no reschedule leaves the
reader waiting the better part of a tick, which the latency row requires
against the wake; interrupts enabled inside the pump print the record the
latency boot refuses; and a fallback counted without asking the processor
fails the count `intrprobe` requires to be zero.

## Phase 4 record

`hda.c` asks for vector 49 through the same call and registers the handler it
already had, keeping its legacy registration and the timer's servicing exactly
as they were. The handler begins by asking the controller whether the
interrupt is its own, so being called twice for one event costs a register
read.

The audio suite's boot now runs `intrprobe` after the notes and requires the
count on vector 49 to have moved. Its mutation is the controller left on its
legacy line: the notes still come out, which is the point of keeping that
line, and the count is zero. A second mutation, the handler registered on a
vector the device does not send, is not caught by any row and cannot be: the
legacy line still delivers, which is the same behaviour the machine had
before this milestone, and no row can tell the two apart without taking that
line away.

`metal_accept.sh` gained the interrupt record for milestone 43: every line of
`/proc/interrupts`, the two vectors, the spurious and fallback counts, and
what the boot log says the local APIC did. Two of them are failures rather
than notes, and both are the kernel's own claims: a timer vector that has
taken nothing, and a fallback count that has moved on a machine whose
messages arrive.

## The review round

Four units read the milestone. The Codex path could not return its findings
to this session: the forwarder is allowed one call and cannot poll, and
`/codex:status` and `/codex:result` are the owner's to run, so the units ran
on Claude at Opus's strength instead, which the policy allows. One Codex unit
did come back through its forwarder and its findings are folded in below.

Unit A, the APIC and the vectors, found two defects that would have shown on
the Lenovo and not here:

* `LINT0` was written before the spurious register's software enable. A
  processor whose APIC is software disabled holds every local-vector entry
  masked and ignores a write that would unmask one, so on a machine whose
  firmware left the APIC disabled the unmask would have been dropped and the
  8259's interrupts would have stopped the moment the APIC came up. QEMU's
  firmware leaves `LINT0` already right, so the driver never wrote it there
  and the fault could not appear. The write order is now the software enable
  first, and the host test's model holds the mask bit set while the APIC is
  disabled, which is what makes its order row mean anything.
* The reschedule on the way out of a message vector asked whether the
  interrupted process was running, which let it preempt kernel code inside a
  system call whose own bookkeeping was half done: `schedule` itself sets a
  process READY and then runs on with interrupts on, and `sys_exit` marks a
  process a zombie before it signals its parent. A message taken in either
  window would have stranded a process for the life of the boot. The question
  is now the one the plan gave: the interrupted context is in user mode, or
  it is not inside a system call.

It also found rows that would pass with the code broken (a refusal row that
checked one register instead of requiring no write at all, a `LINT0` row that
could not tell a write from the firmware's own value, and no row at all for
`lapic_pending`), a base above 4 GiB that was truncated rather than refused,
and a spurious-register write that cleared the bits beside the vector. All
are fixed.

Unit B, MSI on a device and the xHCI, found:

* Configuration space is reached through one address register that every
  access shares, and this milestone put an interrupt handler on that path for
  the first time: `rach2.msidrop` silences the controller from inside the
  handler. Every access now runs with interrupts off.
* `pci_write_config16` was a read of the dword and a write of it back, so
  writing the command word wrote the status word beside it, and the status
  word's faults are cleared by writing them as one. It is one `outw` at the
  data port the register's own offset selects.
* The vector's own mask register was never written, so a device left with
  this vector masked would have been programmed, enabled and silent; a
  capability near the end of configuration space could have had its registers
  wrap onto the identity registers at offset zero. Both are handled and both
  have rows.
* The two interrupter enables were set before the device's capability was
  programmed, which left a device whose firmware had it enabled free to send
  from a stale address. The capability goes first now.
* The acknowledgement written when the interrupter was enabled cleared a
  pending bit that was not this driver's to clear, `msi_on` was set after the
  device was armed, and the latency stamps could be read while a message was
  landing between two of the reads. Each is fixed, and the record now says it
  measures from the drain that took the event, which is the first moment this
  stack knows the event exists; the controller stamps nothing.
* Three host rows proved nothing: the empty-drain row reached its state by a
  sequence that cleared the busy bit first, the moderation-interval row began
  with the register already zero, and the MSI rows began with the device
  already disabled. Each now starts from the state a firmware would leave.

Six mutations were added with the fixes: the vector's mask left set, a
capability past the end followed, a halfword write taken as a dword, an
access taken with interrupts on, the drain that finds nothing not writing the
dequeue pointer, and the old `LINT0` order. Each fails one named row.

Unit C, the suites, found three checks that could not fail for the reason
they existed: the boot for a mouse over messages was the older mouse boot
with `msi=on` added and nothing else, so the timer's pump would have carried
it; the audio count required a number rather than a rise across the notes;
and `/proc/stat`'s line was compared against the timer's alone, which a file
reporting the timer and nothing else would pass. It also found that
`keylatprobe` never checked its `fork`, so a machine where the child could
not be made would have reported the easy number, and that `intrprobe`'s
buffer was smaller than the file's own worst case and its reader could not
tell a truncated file from a whole one. All are fixed: the mouse boot reads
the count after the movements, the audio suite reads the file before and
after, `/proc/stat` is compared against the sum of the vectors' own lines,
and both probes fail rather than report.

Unit D, the proofread, returned thirty-eight corrections. Nine were claims
the code does not support, of which the worst was the milestone's own Readme
stating the write order backwards, and the others were an `intr` line
described as the vectors' total when it counts the spurious vector too, four
refusals where there are six, a latency measured from a moment this kernel
cannot know, and the MSI order as it was before the review. The rest were the
register rule and repetition: three documents listed the same fourteen boots
and two of the lists were already wrong, so the system tests' Readme owns the
list now and the USB Readme refers to it.

## The document the plan allowed for

The plan said that if no document owned interrupts, `sys/interrupts.Readme.md`
would be new and would say so. The nearest owning document turned out to be
`rach2/Readme.md`, whose architecture section already had the IDT and the two
controllers: it now has the two vector ranges, the APIC and what it is and is
not used for, the refusals, the boot check, the fallback rule and the three
boot options, and it points at the USB Readme for the pump and at the
synthetic file systems' Readme for the two `/proc` files. A third document
would have been a third copy of what the proofread had just finished taking
down to one.

## The final tree

`tests/run all` on the closing tree: 63 of the 64 suites pass in one run, and
`selfbuild` fails there and passes on its own in 250 s. It clobbers its own
work disk when it runs beside the others, which is a property of the runner
and not of this milestone; milestone 38 recorded the same thing. `make test`
is 5 minutes 6 seconds against milestone 38's 4 minutes 25, and the six new
USB boots, the audio suite's two probe reads, the clock self-test's 66 ms and
the extra commands in `procfiles` are where the difference went.

## Open items

(none)
