# Milestone 39: interrupts from PCI devices

## The goal

The kernel receives interrupts from PCI devices through the local APIC and message-signalled interrupts, so that a completion on the xHCI controller wakes the process waiting for it at once instead of at the next timer tick, a keystroke on a USB keyboard reaches the terminal in microseconds instead of ten milliseconds, and the audio controller's interrupt arrives on the notebook, where its legacy line may never have. The PIC keeps what it has: the timer, the PS/2 keyboard and mouse, and the serial port, unchanged. No IOAPIC is programmed.

The milestone is complete when, in QEMU, the xHCI controller delivers its interrupts by MSI and `/proc/stat` counts them; when a USB keystroke's latency, measured from the emulator's injection to the terminal's read, drops from tens of milliseconds to under one; when the timer's pump still runs and still catches a completion an interrupt missed; and when the HDA driver's stream interrupt arrives by MSI. Everything the USB and audio stacks did before still works with interrupts off, because the timer path stays, and `rach2.nomsi` on the boot line turns the new path off for a machine that cannot take it. The notebook's APIC and its two controllers are read and proved in milestone 43, which boots the kernel there with `rach2.nomsi` first; that boot line is what makes a machine that cannot take MSI cost one boot and nothing else.

This milestone exists because the USB disk, milestone 40, was designed to poll from process context for want of interrupts, and every review of that design cost more than this milestone will. It is small on purpose.

This plan is the authority for the run. A conflict between this plan and the current tree is resolved by the decisions here; a conflict between this plan and `CLAUDE.md` is resolved by `CLAUDE.md`.

## What the tree settles before this plan

* Interrupts today: the 8259 pair remapped to vectors 32 to 47, stubs for vectors 0 to 47 and 128 in `isr_handler.s`, and `_irq_router`, which sends the PIC its end-of-interrupt for every vector it routes and then dispatches a chain of handlers registered with `register_isr_handler(vector, fn)`. The timer is the PIT on IRQ 0 at 100 Hz. There is no local APIC code, no IOAPIC code, no MSR helper, and `board.c` says of the ACPI interrupt that a PIC-only kernel cannot take it from an IOAPIC input.
* The HDA driver registers its legacy line and services its stream from the timer as well, because its comment says legacy PCI interrupts may not arrive on real hardware. The e1000 and RTL8168 drivers are polled.
* The USB stack is polled from the timer's `usb_poll`, which drains the controller's event ring and steps the device state machines; the controller's interrupter is programmed but `USBCMD.INTE` is never set and `IMAN.IE` is never set, so the controller raises nothing. From milestone 35's record, the notebook's controller is an Intel Lynx Point-LP xHCI at `00:14.0`; from Linux's driver, that part has an MSI capability and an MSI-X capability, and QEMU's `qemu-xhci` has both.
* `pci_find_capability` walks a device's capability list, and `map_mmio` identity-maps a device's registers; both were written for milestone 35. The kernel runs on one processor and has no notion of another; a page fault runs with interrupts off; a syscall is not preempted.
* The local APIC, from Intel's manual: enabled by bit 11 of the `IA32_APIC_BASE` model-specific register, at the physical address that register's bits 12 and up give, which is `0xFEE00000` on every machine this kernel has met; its registers at that page are the ID at `0x20`, the version at `0x30`, the task priority at `0x80`, the end-of-interrupt at `0xB0`, the spurious-interrupt vector at `0xF0` with its software-enable bit 8, and the local vector table entries above, of which `LINT0` and `LINT1` are what the firmware set for the PIC's interrupts to keep reaching the processor. Enabling the APIC's software bit does not change those entries, and the PIC's interrupts keep arriving as they did, which is the "virtual wire" arrangement every PC boots in.
* MSI, from the PCI specification: capability ID 5; a control word at offset 2 whose bit 0 enables the mechanism, whose bits 1 to 3 say how many vectors the device can use and bits 4 to 6 how many it may use, and whose bit 7 says the address is 64 bits; the address at offset 4, its upper half at 8 when 64 bits, and the data at 8 or 12 accordingly. A message to `0xFEE00000` with the destination in bits 12 to 19 and the data's low byte as the vector, with delivery mode fixed and edge trigger, arrives as that vector at the processor whose APIC ID the address names. An MSI is edge-triggered, so a device that has raised one raises no other until its own status is acknowledged and the condition recurs; a missed edge is a lost interrupt.
* xHCI's interrupter: `IMAN` at the runtime registers' interrupter zero has an interrupt-pending bit 0, cleared by writing one, and an interrupt-enable bit 1; `USBCMD.INTE` enables interrupts for the controller; `ERDP`'s event-handler-busy bit is cleared by the driver when it has consumed events. The controller raises its interrupt when an event is written to the ring and `IMAN.IP` goes from clear to set.

## Decisions

### The local APIC

`sys/lapic.c` is new, and it does four things. It reads `IA32_APIC_BASE` through an `rdmsr` helper that `sys/lib/kcpu.h` gains beside the `cpuid` two files already carry; it maps the APIC's page with `map_mmio`; it sets the software-enable bit in the spurious-interrupt register with the spurious vector at 255; and it offers `lapic_eoi`. It reads the APIC ID for the MSI address. It touches no local vector table entry, so the PIC's interrupts keep arriving as before. On a processor whose `cpuid` says there is no APIC, or when `rach2.nomsi` is on the boot line, it does nothing and says so, and every device stays on its old path.

### Vectors and the router

Vectors 48 to 63 are for MSI, sixteen of them, with stubs added to `isr_handler.s` in the shape of the existing ones. `_irq_router` learns the difference: a vector below 48 gets the PIC's end-of-interrupt as today, and a vector from 48 to 63 gets the APIC's, after the handler chain has run, because an edge-triggered message needs the device's own status acknowledged before the next can come and the handler is where that happens. `register_isr_handler` serves both ranges unchanged. `/proc/stat`'s `intr` line, which says zero today, becomes the count of every routed vector, and `/proc/interrupts` is new, one line per vector in use with its count and the name its handler registered under, in Linux's shape, because that is what a person reads to see whether an interrupt arrives.

### MSI on a device

`pci_msi_enable(dev, vector)` in `pci.c` finds the MSI capability, refuses a device that has none by name, writes the address and the data for one vector on this processor, sets the multiple-message-enable bits to zero so the device uses one vector, and sets the enable bit last; `pci_msi_disable` clears it. A device with only MSI-X is refused by name in this milestone: MSI-X's table lives in a BAR and is a second mechanism, and every controller this kernel drives has plain MSI. The legacy interrupt line is left as it is; a device with MSI enabled does not use it.

### xHCI on MSI

`xhci.c` enables its interrupter: `IMAN.IE`, `USBCMD.INTE`, and MSI on vector 48. Its handler is the pump that milestone 35's `usb_poll` already is: clear `IMAN.IP` by writing it, drain the event ring, step the device machines, write `ERDP` with the busy bit cleared, and wake whoever waits on a completion. That is the same function the timer calls, with interrupts off inside it as the timer has them, and it is the only place either context touches the ring, which is the rule milestone 40's block device relies on. The timer keeps calling it every tick as a fallback for a lost edge, so a missed interrupt costs at most ten milliseconds and never a hang; the record counts how often the timer's call found work, which on a machine whose interrupts arrive is zero. With `rach2.nomsi`, or when the controller has no MSI capability, the interrupter stays off and the timer is the only pump, as today.

A wake from the handler goes through `wq_wake_all`, which the keyboard's interrupt already uses from interrupt context, and the sleeper re-checks its condition with interrupts off before sleeping, as `wq_wait_event` does, so an interrupt that lands between the check and the sleep is not lost.

### HDA on MSI

`hda.c` enables MSI on vector 49 through the same call, registers its existing handler there, and keeps its legacy registration and its timer servicing exactly as they are, so a machine where MSI does not arrive is no worse than today. The interrupt count in `/proc/interrupts` is what says which path a machine uses.

### What the USB disk plan changes because of this

Milestone 40's "Waiting for a stick" loses its two-millisecond polling loop: `block_request` submits and sleeps on the request's done flag, the MSI handler's pump completes the request and wakes it, and the timer's pump is the fallback. Everything else in that plan stands: the one pump, interrupts off inside it, submission inside the same critical section, and the atomic predicate-and-sleep.

## Memory and limits

| Limit | Value |
|-------|-------|
| MSI vectors | 16, at 48 to 63 |
| Vectors per device | 1 |
| Devices on MSI | xHCI and HDA; the network drivers stay polled |
| Fallback pump | the timer, 100 Hz, unchanged |

## What stays out

The IOAPIC and the ACPI interrupt: not programmed; the ACPI SCI stays where `board.c` leaves it. MSI-X: refused by name. More than one vector per device: no. The network drivers on interrupts: they are polled and fast enough, and a change there has no test that needs it. The PIT replaced by the APIC timer: no. A second processor: no. Interrupt-driven AHCI: no, the window's fault path must poll and a sleeping AHCI would need the split milestone 36 refused.

## Decisions on tests and tooling

* `tests/host/lapic_hosttest.c`: `lapic.c` against a model of the APIC page and the MSR: enabling sets the spurious register's bit and nothing in the local vector table; a processor without an APIC leaves everything untouched; `rach2.nomsi` does the same; the EOI is one write to `0xB0`.
* `tests/host/pcimsi_hosttest.c`: `pci_msi_enable` against a configuration-space model: a device with a 32-bit MSI capability gets its address, its data and then its enable bit, in that order; a 64-bit one gets its upper half zero and its data at the later offset; a device with no MSI is refused; one with MSI-X only is refused; the multiple-message bits are zero; disable clears the enable bit and nothing else.
* `tests/host/xhci_hosttest.c` gains rows: `IMAN.IE` and `USBCMD.INTE` set when MSI is on and clear when it is off; the handler clears `IMAN.IP` before it drains and writes `ERDP` after; a completion that arrives while a sleeper is between its check and its sleep is not lost.
* `tests/system/suites/usb.sh` gains: a boot where `/proc/interrupts` shows the xHCI vector counting as keys are typed; the typing and mouse boots run again with `rach2.nomsi` and pass; a latency boot where the harness stamps a key's injection and the guest stamps its arrival through the monitor's clock, required under one millisecond with MSI and recorded with `rach2.nomsi`; and a lost-edge boot, where a mutation that never clears `IMAN.IP` is what makes the timer's fallback pump the only path, and the machine still types, ten milliseconds late, which is the row that proves the fallback.
* `tests/run audio` gains the interrupt count on the HDA vector.
* `tests/system/accept/metal_accept.sh` gains `/proc/interrupts` and the boot-line diagnostic below, written here and run in milestone 43.

## Documentation

* `rach2/sys/Readme.md` or the nearest owning document for interrupts: the two vector ranges, the APIC and what it is and is not used for, MSI, the fallback rule, `rach2.nomsi`, and the verification. If no document owns interrupts today, `rach2/sys/interrupts.Readme.md` is new and says so.
* `rach2/sys/usb/Readme.md`: the interrupter on, the pump shared by the handler and the timer, and "Interrupts" removed from the absent list.
* `rach2/sys/audio/Readme.md` or the HDA driver's owning document: MSI beside the legacy line.
* `rach2/sys/fs/synthfs.Readme.md`: `/proc/interrupts` and the `intr` line.
* `rach2/Readme.md`: the boot-option table, the subsystem table, the limits.
* `Readme.md`: the hardware status.
* `tests/Readme.md`, `tests/host/Readme.md`, `tests/system/Readme.md`.
* `dev-log/40-usbdisk/plan.md`: its "Waiting for a stick" section rewritten to this milestone, in the same commit as this plan.
* `dev-log/39-msi/Readme.md` and the `dev-log/Readme.md` entry, in phase 5.

## How the work is kept track of

`progress.md`, `current.md`, `notes.md` and `hardware.md` beside this plan, as before, and the one-line report `M39: phase N of 5; X of Y items done; HEAD abc1234; tree clean|dirty`.

## Standing rules for the run

* Work directly. The only delegation is an independent review, one reviewer per unit, at most six files, at most ten minutes. There are four review checkpoints, after phases 2, 3, 4 and 5, with the number of units fixed in phase 1.
* Commit in scoped units in the house style, on the branch, no attribution, never push.
* Every commit passes `tests/run style` and `tests/run host`. A commit that touches a subsystem runs that subsystem's suites singly. The whole set runs at the end of phase 3 and of phase 5.
* A test is proved by making it fail. A mutation is driven from a script that restores the source in a trap and rebuilds before the verdict is believed.
* A change that alters a contract rewrites the owning Readme in the same commit.
* A change that makes something redundant removes it in the same commit. The tree after this milestone is not larger than the milestone's own additions.
* Nothing touches the Lenovo in this milestone. Milestone 43 runs the diagnostic there before any kernel with MSI is installed, and boots that kernel with `rach2.nomsi` first.
* Nothing is deferred. A thing discovered mid-run goes into `notes.md` with the phase that will do it, and phase 5 closes every line or records the owner's decision to drop it.

## The phases

### Phase 1: arrival and inventory

Read `AGENTS.md`, `CLAUDE.md` and this plan in full. Require a clean tree and record `git log --oneline -12`. Create the four tracking files in the first commit. List the files each review checkpoint will cover and fix the units.

Record, in QEMU, what `cpuid` says about the APIC, what `IA32_APIC_BASE` holds, the APIC ID, the spurious register and the two `LINT` entries as the firmware left them, and the xHCI's and HDA's capability lists with their MSI and MSI-X offsets and their control words, through a boot-line diagnostic, `rach2.apicdiag`, that reads and prints and changes nothing; milestone 43 runs the same diagnostic on the Lenovo before anything else. Check every number in the two specification bullets above against the manuals and record each with its source.

Proof: `hardware.md` with QEMU's values and a column left for the Lenovo's; the table in `notes.md`.

### Phase 2: the APIC, the vectors and MSI

`lapic.c`, the MSR helper, the sixteen stubs, the router's two ranges, `/proc/interrupts` and the `intr` count, `pci_msi_enable` and `pci_msi_disable`, and `rach2.nomsi`. The two new host tests. No device uses any of it yet; the spurious vector's handler counts and returns.

Proof: the host rows pass; a boot shows the APIC enabled in the record and every existing suite passes, which is what says the PIC's interrupts still arrive.

Mutations: write a local vector table entry; send the PIC's end-of-interrupt for a vector above 47; set the enable bit before the address; accept a device with no MSI capability. Each must fail exactly its row.

### Phase 3: xHCI on MSI

The interrupter enabled, the handler as the pump, the wake, the timer's fallback and its counter, and the xHCI rows. The USB suite's new boots, including the latency one and the lost-edge one.

Proof: `/proc/interrupts` counts keystrokes; the latency is under a millisecond with MSI and recorded without; the lost-edge boot still types; `tests/run all` passes.

Mutations: drain the ring before clearing `IMAN.IP`, which the lost-edge boot must catch as a hang the fallback hides and the counter reveals; skip the APIC end-of-interrupt, which the second keystroke must catch; wake without the predicate re-check, which the model's between-check-and-sleep row must catch; let the handler and the timer drain without interrupts off, which the model's reentrancy row must catch. Each must fail exactly its check.

### Phase 4: HDA on MSI

HDA's MSI beside its legacy line, the audio suite's count, and `metal_accept.sh`'s lines for milestone 43: `/proc/interrupts` with a USB keyboard typed on and audio played, recording which path each device took.

Proof: `tests/run audio` counts HDA interrupts by MSI in QEMU and passes with `rach2.nomsi`.

### Phase 5: the record and the sweep

As in the milestones before. The documents, every one listed above, read against the code, with a Codex read of the reader-facing ones; the HDA comment about legacy interrupts rewritten to what the machine showed. The code: the two pump paths read for what became one; no helper with one caller. The build: every rule and tool listed in `notes.md` with its reason. The tests: the tier decisions measured; the mutation scripts in `notes.md`; the full set twice with no machine left running, and `make test` timed against milestone 38. The record: `notes.md` closed line by line, `dev-log/39-msi/Readme.md` with its trust statement, and the `dev-log/Readme.md` entry in the same commit. The tree is clean and nothing is pushed.

## Risks, in expected order

* The notebook's firmware leaving the APIC in a state this plan does not expect. Milestone 43 reads it with the diagnostic before anything is installed, and `rach2.nomsi` is the way to boot regardless.
* A lost edge on real hardware. The timer's pump is the fallback and the counter says how often it was needed; a machine that needs it often is recorded, not worked around.
* Two contexts in one pump. Interrupts are off inside it in both, the handler runs with them off by construction, and the model has the reentrancy row.
* The PIC's interrupts stopping when the APIC is enabled, on a machine whose firmware did not set virtual-wire mode. The diagnostic reads the `LINT` entries; a machine that would need them set is recorded in milestone 43 and boots with `rach2.nomsi` until a later milestone writes them.

## Recovery after a compaction

Read `AGENTS.md`, `CLAUDE.md`, this plan's decisions and the current phase, `progress.md`, `current.md`, `notes.md` and `hardware.md`. Run `git status --short --branch` and `git log --oneline -15`, inspect every uncommitted path, and resume the current phase's first unfinished item. Report the one line.

## The goal prompt

`Run milestone 39, interrupts from PCI devices, from dev-log/39-msi/plan.md.`
