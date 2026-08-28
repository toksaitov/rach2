# Milestone 32: the third card

## The goal

Add an Intel 8254x (e1000) driver, and revise the network device interface so it describes three drivers instead of accommodating two. At the end, Rach2 drives the card QEMU attaches by default, a person can boot either emulated card by name, the named suites below pass on both, and the driver interface has one member per operation that something in the tree actually consumes.

This plan is the authority for the run. A conflict between this plan and the current tree is resolved by the decisions here; a conflict between this plan and `CLAUDE.md` is resolved by `CLAUDE.md`.

## Why

`netdev_probe` tries the RTL8139 and then the RTL8168GU. The first is what QEMU exposes when `run.sh` asks for it by name; the second is the chip in the Lenovo G50-70 and is emulated by nothing on the development host. Neither is the card QEMU attaches when nobody asks. `rach2/Readme.md` records the result: with `NET=off` the script stops passing `-device`, QEMU supplies its own e1000, and this kernel cannot drive it. Anyone running `qemu-system-i386` from a generic command line gets a machine with no network and no explanation.

The e1000 is also the driver this system should have had second. It is the descriptor-ring DMA shape that every modern card uses, without the PHY bring-up sequence and vendor firmware that make the RTL8168GU 1228 lines. It is what students meet in other courses, and it is the adapter VMware and VirtualBox default to.

Three drivers is also the point at which the interface can be described rather than guessed. `struct netdev_ops` was written for two. The parts of it that do not yet hold are listed under the end state.

## The decided end state

### The driver

`sys/network/driver/e1000.c` and `e1000.h` drive the Intel 82540EM, PCI `8086:100e`. That is what QEMU's `e1000` device is: `qemu-system-i386 -device help` lists `e1000-82540em` as its alias. The related `e1000e` device is the 82574L, a different register interface, and is out of scope; so are the 82544GC and 82545EM models unless they cost nothing, which is decided by reading during implementation and recorded either way.

* The register window is BAR0, a memory BAR, mapped with `map_mmio` at boot. The driver reads and writes nothing above `RAH0` at 0x5404.
* Every register access goes through an indirection, as `rtl8168.c` does, so the host test can drive the whole bring-up against a software model of the chip. The real binding is plain MMIO on the mapped window.
* The station address is read from `RAL0`/`RAH0` when `RAH0` has its address-valid bit, and from EEPROM words 0 through 2 through `EERD` otherwise. Both paths exist because a card at cold power-on may present only the second, and both are exercised by the host model. The 82540EM `EERD` field layout is the one implemented; a chip that answers with a different layout is refused rather than believed. An all-zero, broadcast, or multicast address is refused.
* 32 receive and 32 transmit descriptors. Both rings are 512 bytes, which satisfies the requirement that `RDLEN` and `TDLEN` be a multiple of 128. Receive buffers are 2048 bytes (`RCTL.BSIZE` 00 with `BSEX` clear); transmit buffers are 1536. The rings and buffers are static and 16-byte aligned, and the kernel is identity-mapped, so a pointer is also the address the card uses.
* `RCTL.SECRC` is set, so a received descriptor's length is the frame's length with no CRC arithmetic. Short transmit frames are padded to 60 bytes in software and `TCTL.PSP` is set as well, because the two do not agree on every emulated card.
* Receive: on interrupt, walk from the software head while the descriptor's `DD` is set. Four things are checked before the frame is handed up, and a failure of any one drops it and counts it: the descriptor's error byte must be clear, `EOP` must be set on the same descriptor, the reported length must be at least the 14-byte Ethernet header, and it must not exceed the 2048-byte buffer. Both existing drivers validate the equivalent before handing a buffer up, `rtl8139_rx` on its ring header and `rtl8168_poll` on `RX_RES` and the split case, and the 82540EM descriptor has a separate error byte that a completed end-of-packet descriptor can still report. An unchecked length would hand the stack bytes past the end of the buffer. Then `ethernet_rx`, then clear the status byte, then advance `RDT` to the last descriptor taken. QEMU may never produce any of these four conditions, so the host model produces all four.
* Transmit: round-robin over the ring with `RS`, `EOP`, and `IFCS` set. Before reusing a slot, wait a bounded number of short intervals for its `DD`, then drop the frame and count a stall, which is the rule `rtl8168_tx` already follows.
* The card is interrupt-driven, so `poll` is NULL. The 82540EM is meant to be used that way, the shared-line service loop `rtl8139_irq` uses is the pattern to follow, and a card driven from the 100 Hz tick would be limited to 100 Hz.
* `init` on a machine with no `8086:100e` returns `NETDEV_ABSENT` having written no register anywhere, which is what the probe table requires of every driver.

The probe order becomes RTL8139, e1000, RTL8168GU. The RTL8139 stays first because every existing suite boots against it and must keep binding it. The e1000 is second because it is what an emulated machine has when nobody chose. The RTL8168GU is last because it is the one physical target, and its `NETDEV_FAILED` verdict stopping the probe is the only diagnostic a machine with no serial port and no network has.

### The interface

`struct netdev_ops` gains three things and changes one. Each earns its place by having a consumer already in the tree, and each is implemented by all three drivers:

* `tx` returns a verdict instead of `void`. Both existing drivers drop frames (over-length in the RTL8139, a full ring in the RTL8168GU) and no layer above learns of it. `/proc/net/dev` prints its drop columns as literal zeros and its comment in `procfs.c` says the counters it does not keep are zero "which is what a device with no errors and no collisions reports anyway". That is not true of either driver now and would not be true of the e1000. With a verdict the column can be filled from what happened.
* `stop` silences the device: receiver and transmitter off, interrupts masked, and PCI bus mastering cleared, which is the write that actually ends DMA. Nothing quiesces a bus-mastering card today; it keeps writing into its rings until the machine stops. Three things about it are decided here rather than left to the implementation.

  `stop` must be safe on a partially initialized device, and `netdev_probe` calls it after a `NETDEV_FAILED` verdict. Without that rule the case the operation exists for is the one it misses: `rtl8168_init` enables bus mastering and can then still fail in PHY initialization or hardware start, leaving a DMA-capable card enabled and unbound, which the shutdown path cannot reach because nothing is bound. Each driver's `stop` is therefore written to run against a device it never finished starting.

  The call at shutdown goes in `kernel_shutdown` in `kernel.c`, after the page-cache flush and after its goodbye line, and not in `power.c`, whose header states that it knows nothing about subsystems and only decides how the machine stops.

  That position costs the three lines `power_stop` prints after it (`poweroff: powering off`, `reboot: restarting`, `halt: system halted`), because `rach2.netconsole` taps every serial character through the card. That is accepted, and the contract is written down instead of left to be discovered: netconsole's last message on the wire is the goodbye line from `kernel_shutdown`, and the three power lines reach serial only. The alternative, stopping the card inside `power_stop` immediately before the port write, is rejected because it puts subsystem knowledge in the one file whose header says it has none.

  The equivalent exposure in the storage drivers is real and is not in this milestone's scope; the network Readme says so rather than leaving it unsaid.
* `link` reports whether the link is up and at what speed and duplex. `SIOCGIFFLAGS` sets `IFF_RUNNING` when a device is bound, which is what `ifconfig` shows, and a bound card with an unplugged cable reports the same as a working one. The RTL8168GU driver already computes this in `r8168_link_speed`; the RTL8139 has it in `MSR`; the e1000 has it in `STATUS`. `link` returns 1 when it read the state and 0 when the card cannot tell. `IFF_RUNNING` is then set when the state was read and the link is up, or when the state could not be read and a device is bound, which preserves today's meaning for a card that cannot answer.

Driver-level counters live in `netdev.c`, not in the vtable: one `struct netdev_counts` with receive errors, receive drops, and transmit drops, three small `netdev_note_*` functions the drivers call, and one accessor `procfs.c` reads. A driver reports what it dropped; it does not own a counter table.

`init`, `poll`, `error`, `print_info`, and `name` keep their current meanings. `poll` stays NULL for an interrupt-driven device. The receive path stays what it is: a driver calls `ethernet_rx` directly from interrupt or poll context with a pointer into its own buffer, valid for the duration of the call. That is the simplest thing that works for all three and it is not changed for the sake of symmetry.

The order of the work is the driver first and the interface second. An interface designed from two drivers and validated by a third is a guess; designed from three it is a description. The e1000 is therefore written against the interface as it stands and edited once, mechanically, in the phase that revises it.

### QEMU and the run scripts

`rach2/qemu_devices.sh` gains `NIC`, which selects `rtl8139` (the default, so every existing invocation is unchanged) or `e1000`. `NET=off` keeps meaning that the script passes no network options. The four run and debug scripts document the new variable in the block comment each already has.

`rach2/Readme.md` currently says that with `NET=off` QEMU supplies an e1000 "which this kernel does not support". That sentence is replaced by what is then true.

### The tests

* `tests/host/e1000_hosttest.c`, a software model of the chip in the shape of `rtl8168_hosttest.c`, joins the `host` suite. It proves the reset handshake and its bounded failure, the `EERD` protocol and the `RAL0`/`RAH0` preference, the refusal of an invalid station address, that the rings and their lengths are programmed before `RCTL.EN` and `TCTL.EN`, the receive descriptor contract including `DD`, the status clear and the `RDT` advance, ring wrap in both directions, each of the four receive checks failing on its own, the transmit descriptor contract including `RS`, `EOP`, `IFCS` and the `DD` wait, the bounded transmit stall and its drop count, the over-length drop, and that `init` with no card present performs no register access at all.
* A new suite `e1000` runs, with `--device e1000,netdev=net0`, the probe boot and the ping, packet, netif, dgram, netconsole, raw, dhcp, and ssh work. The harness already takes `--device`, so most of those need nothing new. Two do: the `dhcp` target passes no `--device` at all and takes the harness default, and `ssh.sh` writes `-device rtl8139,netdev=n0` into its own QEMU command line. Both gain a card parameter defaulting to `rtl8139`, so every existing invocation is unchanged and the new suite passes `e1000`. The suite is a member of `all`.
* Which suites run on both cards is therefore exactly that list. `addressed`, and the disk, boot, account, and self-build suites, run on the RTL8139 only, because what they exercise is not the card. The Readme says which, rather than claiming the whole set.
* Probe order gets its own boots inside that suite: with both cards attached the machine binds the RTL8139 and says so; with only the e1000 attached it binds the e1000; with neither it reports no supported card and still boots.
* `stop` is proved by a host spy, not by the reset and shutdown suites. Those suites read serial output and whether QEMU stopped, so they pass whether or not the operation is ever called, and a proof that cannot fail is not one. The host models assert the sequence for each driver, that the enables are cleared before bus mastering, that it is safe on a device that never finished starting, and that `netdev_probe` calls it after a `NETDEV_FAILED` verdict. A netconsole boot then stops the machine and asserts the wire contract from the end state: the goodbye line arrives, and nothing after it does.
* `link` is proved in the host model for all three drivers and on a live boot through `ifconfig` showing `RUNNING`.
* The drop counters are proved in three parts, because no path from userland can force a driver to drop a frame and a test that pretends otherwise would prove nothing. `packet_send` refuses a payload over 1500 bytes, `ethernet_tx` refuses one independently, and the largest frame any driver is therefore handed is 1514, which all three accept. The three parts are: each host model forces its driver's own drop conditions (a full ring, a stalled descriptor, an over-length frame, and for receive the four checks above) and asserts the verdict the driver returns; a host test calls the `netdev_note_*` helpers and reads the totals back through `netdev_counts()`; and a live boot reads `/proc/net/dev` and asserts the sixteen fields are present, in order, and parse as numbers, which is the contract `ifconfig` depends on. That the columns read zero on a healthy machine is the correct result and is asserted as such.
* Every new check is proved by making it fail, driven from a script that restores the source in a trap, with the file touched or the tree rebuilt explicitly before the verdict is believed.

### The documents

`rach2/sys/network/Readme.md` is rewritten for three drivers, the revised interface, the probe order and its reason, and the counters. `rach2/Readme.md` updates its device list, its `NET=off` paragraph, and the network section. The root `Readme.md` updates the one sentence naming the supported cards. `tests/registry` gains the suite line. `dev-log/32-netdev/Readme.md` records the milestone and is added to `dev-log/Readme.md` in the same commit.

Two stale comments found while reading are corrected in the phase that touches their file: `rtl8139.c`'s header claims the driver waits for `TOK` before reusing a slot, which the function it describes says it deliberately does not do; and `net_core.c`'s header says `init_network` brings up the RTL8139, which stopped being true when the driver table was introduced.

### Out of scope

No e1000e or 82574L. No jumbo frames, offloads, MSI or MSI-X, multiple queues, VLAN handling, interrupt throttling, or runtime power management. No second interface: the stack has one `eth0` and this milestone does not change that. No protocol layer above Ethernet changes. Nothing touches the laptop, and no hardware operation is performed.

## Standing rules for the run

* Work directly. Delegation is only for an independent review, one reviewer per diff, with the limits in `CLAUDE.md`.
* Commit in scoped units in the house style, directly on the branch, no attribution, never push. A phase may contain several commits; each is one logical change leaving a working, proved tree.
* Every commit passes `tests/run style` and `tests/run host`, and the network suites when it touches anything they exercise.
* A change that alters a contract rewrites the owning Readme in the same commit. Historical records under `dev-log/` are never edited to match the present.
* Message shapes follow `CLAUDE.md`: `component: action` for progress, `component: result: detail` for an outcome.
* Probe success tokens and existing test contracts do not change.
* Register data comes from the 82540EM datasheet and is named where it is used. Where behavior is confirmed by running QEMU rather than by reading, the comment says so.

## The phases

Do the phases in order. Within a phase, land the smallest self-contained commits that keep every intermediate tree passing the required checks. Record deviations and deferred defects in `notes.md`.

### Phase 1: arrival

Read `AGENTS.md`, `CLAUDE.md`, and this plan in full. Require a clean tree, record `git log --oneline -12`, run `sh tools/setup/doctor.sh`, `make`, `tests/run style`, `tests/run host`, and `tests/run default`, and require every command to exit zero before any change. Record the kernel's current `text`, `data`, and `bss` sizes as the baseline the driver's static rings are measured against. Create `notes.md` and `current.md` beside this plan in the first commit, with the same roles they had in milestone 30: `current.md` holds only the resume state and is replaced at each checkpoint, `notes.md` accumulates deviations and stays part of the record.

### Phase 2: the driver

Write `e1000.h` and `e1000.c` against the interface as it stands, add the source to `rach2/sources.mk`, and add the table entry to `netdev.c` in the decided order with the reason in the comment. The commit that adds the file also adds the boot detail line and the `error` strings.

Proofs in this phase are compile-time and boot-time only: `tests/run style`, a boot with `-device e1000,netdev=net0` reaching the shell with the card bound and its MAC printed, and `tests/run default` unchanged because the RTL8139 still binds first. Nothing is claimed about the descriptor contract until phase 3.

### Phase 3: the host model and the suite

Write `tests/host/e1000_hosttest.c` with the coverage listed in the end state. Give the `dhcp` target and `ssh.sh` their card parameter, defaulting to `rtl8139`, and prove the default leaves both unchanged by running them once before and once after. Then add the `e1000` suite to `tests/registry` and the Makefile with the boots listed in the end state. Each host-model check is proved by making it fail: mutate the driver, rebuild, watch that specific check fail, restore, rebuild, watch it pass.

At the end of this phase the suites named in the end state pass on both emulated cards, and `tests/run all` passes.

### Phase 4: the interface

Revise `struct netdev_ops` and its consumers in the order that keeps each commit working:

1. `tx` returns a verdict; the three drivers return it; `ethernet_tx` records a drop; `netdev.c` holds the counters; `procfs.c` prints them. Prove it in the three parts the end state names, and correct the `procfs_gen_netdev` comment that says the counters it does not keep are zero, which stops being true here.
2. `stop`; the three drivers implement it and each is safe on a device that never finished starting; `pci_disable_bus_master` is added beside `pci_enable_bus_master`; `netdev_probe` calls it after a `NETDEV_FAILED` verdict; `kernel_shutdown` calls `netdev_stop()` after its goodbye line and before `power_stop`. Prove it with the host spy and the netconsole wire contract the end state names, not with the reset and shutdown suites, which cannot fail on this.
3. `link`; the three drivers implement it; `print_info` reports speed and duplex; `SIOCGIFFLAGS` derives `IFF_RUNNING` by the rule in the end state. Prove it in the host models and with `ifconfig` on a live boot.

Each of the three lands with its own Readme changes and its own failing-first proof. `check-sources` and `guestcheck` pass throughout, and the guest build's recipe length stays inside its budget with the added object.

### Phase 5: scripts and documents

`NIC` in `qemu_devices.sh` and the four script comments, matching the card parameter phase 3 gave the two suites. Then the Readmes: `sys/network/Readme.md` rewritten, `rach2/Readme.md` and the root `Readme.md` corrected, and the two stale comments fixed. Prove the documentation change with a repository search over current documents, comments, scripts, and makefiles for every claim that changed; `dev-log/` is excluded.

Any prose that resists a plain rewrite is sent to Codex to write, and what comes back is read against the code before it is committed.

### Phase 6: completion

Run `make` from clean, then `tests/run all`, and run `tests/run e1000`, `tests/run dhcp`, and `tests/run ssh` again as direct evidence even though `all` contains them. Record the kernel size against the phase 1 baseline. Write `dev-log/32-netdev/Readme.md` and add `32-netdev` to `dev-log/Readme.md` in the same commit. The tree is clean and nothing is pushed.

## Recovery after a compaction

Read `AGENTS.md`, `CLAUDE.md`, this plan's end state and the current phase, `notes.md`, and `current.md`. Run `git status --short --branch` and `git log --oneline -15`, inspect every uncommitted path, and resume the current phase's first unfinished item. `current.md` names the active phase, the item in progress, the evidence already checked, and the exact next action; replace its contents instead of appending. After each phase and after each recovery, report one line: `netdev: phase N; item; tree clean|dirty`.
