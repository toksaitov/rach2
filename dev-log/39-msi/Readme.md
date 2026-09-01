# Milestone 39: the local APIC and message-signalled interrupts

The kernel enables the local APIC, takes the xHCI's and the audio controller's interrupts as messages instead of polling for them, and keeps the timer's pump behind both as the fallback for a message that never arrives. A keystroke on a USB keyboard now reaches its reader within a millisecond of the controller writing the event, instead of waiting for a tick that is up to ten milliseconds away. Nothing touched the Lenovo; what this milestone leaves for milestone 43 is a boot-line diagnostic and a record in the metal acceptance.

## What was built

`sys/lib/kcpu.c` is `cpuid`, `rdmsr` and `wrmsr` in one place, with the two feature questions the APIC needs. Three files wrote `cpuid` out for themselves before it existed; the sweep at the end of this milestone took them down to this one.

`sys/apicdiag.c` is the `rach2.apicdiag` report, which reads and prints and writes nothing: the two `cpuid` bits, `IA32_APIC_BASE`, the APIC's identity, version, task priority, spurious register and its two local-vector entries, and the MSI and MSI-X capabilities of the two controllers this kernel drives by message. It runs on the boot line so that the machine it is asked about is the machine that answers, and milestone 43 runs it on the Lenovo before any kernel with MSI is installed there.

`sys/lapic.c` enables the APIC or refuses by name: a processor without one, without the model-specific registers, in x2APIC mode, with a base this driver cannot map, or a boot line with `rach2.nomsi`. The task priority goes to zero and `LINT0` is written as an unmasked external interrupt before the spurious register's software enable, so that the 8259's interrupts keep reaching the processor across the change. The boot asks the timer, straight after the tick starts, whether they still do: fifty milliseconds with no tick writes `LINT0` again and says so, and fifty more with no tick says that too.

`isr_handler.s` and `tables/idt.c` gained sixteen stubs and gates for vectors 48 to 63, and one for the APIC's spurious vector at 255, which without a gate would be a general-protection fault. `_irq_router` ends a message interrupt at the APIC after the handler chain has run, and reschedules on the way out unless it interrupted a process running inside its own system call. `register_isr_handler` and `set_isr_handler` take the name the handler is known by, `/proc/interrupts` is new, and `/proc/stat`'s `intr` line, which said zero on every machine, is the count of every routed vector.

`pci.c` programs a device's MSI capability in the order the specification requires and no other: the enable bit cleared, an MSI-X capability disabled beside it, the address and the data written while the mechanism is off, the count of vectors set to one, and the enable bit last.

`usb.c` has one pump, turned by the controller's message and by the timer, with the interrupt flag left as each context found it; the handler acknowledges and drains in one call, and the drain writes the dequeue pointer back with the event-handler-busy bit even when it took nothing. How often the timer finds work that no message was coming for is counted and reported as `FBK` in `/proc/interrupts`; the question of whether a message was coming is put to the processor's own request register and not to the controller. `hda.c` takes vector 49 the same way and keeps its legacy line and its timer servicing exactly as they were.

`sys/pit.c` gained the clock the latency record needed: microseconds from the counter under the tick, where neither the tick nor the counter can count the periods alone.

## What running it found

Five things were learned at the machine rather than from the specification, and each changed the code. The controller raises no further interrupt while the event-handler-busy bit is set, so a handler whose drain found the ring already empty silenced it for good. The acknowledgement and the drain had to become one call for a host row to be able to catch their order being reversed. The controller's own pending bit does not answer the question the fallback count asks, because it is set whether or not a message was sent; the processor's request register does. A wake is worth nothing if the woken reader does not run, and the reader that shows it is the one with another runnable process beside it. And the kernel had no clock finer than its 10 ms tick.

One defect outside this milestone's own work came out of it: a record raised by an interrupt handler while another record was being written out went to the ring and no further, so the USB suite's replug row failed about a third of the time. `klog` now queues such a record and the delivery it interrupted writes it out when its own line is finished.

## What the review found

(to be written when the review round closes)

## Verification

`tests/run host` runs two new host tests and the rows added to a third. `lapic_hosttest` drives the APIC driver against a model of the register page and the model-specific register: virtual wire, a masked `LINT0`, a `LINT0` that is not an external interrupt, the register's enable bit, each of the four refusals, the end-of-interrupt, the spurious counter, the repair, and the order of the writes. `pcimsi_hosttest` drives `pci_msi_enable` against a model of configuration space: a 32-bit capability, a 64-bit one, MSI-X disabled first, the two refusals, the disable, and the order of the writes. `xhci_hosttest` gained the interrupter: the moderation interval, the two enables, the acknowledgement that keeps the enable beside it, the dequeue pointer written with the busy bit so that the next event raises an interrupt of its own, an empty drain that still writes it, and an event written between the acknowledgement and the drain.

`tests/run usb` is fifteen boots, six of them this milestone's: the counts in `/proc/interrupts` after keys typed on messages, the same machine with `rach2.nomsi`, a mouse over messages, the four stamps of one keystroke's path, a machine silenced after three messages by `rach2.msidrop` that goes on typing on the timer's pump, and the 8259's own keyboard and timer with the APIC between them and the processor. `tests/run audio` requires the count on the audio controller's vector to have moved while the notes played, and `tests/run procfiles` reads `/proc/interrupts` and `/proc/stat` on the machine and checks the timer's line against a count taken from the same boot.

Every check added here was proved by a mutation that makes it fail: six for the APIC and the MSI capability in phase 2, seven for the interrupt path in phase 3, three for the two `/proc` files, three for the clock, three for the log's queue, three for `LINT0` and the boot check that repairs it, and one for the audio count. Two are recorded as not caught and cannot be: a handler registered on a vector the device does not send degrades to the legacy line this design keeps behind the messages, which is the behaviour the machine had before this milestone, and no row can tell the two apart without taking that line away.
