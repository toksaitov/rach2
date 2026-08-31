# Milestone 40: hubs

## The goal

A USB hub works: a keyboard, a mouse and a stick plugged into one hub, in one socket of the Lenovo G50-70, are three devices this kernel drives, at their own speeds, including a low-speed keyboard behind a high-speed hub, which is the case the transaction translator exists for. The controller interface learns to describe a device by where it is in the tree and not only by a root port, the USB core learns that a port can belong to a hub, and a hub class driver learns to power, reset and watch its ports the way the root controller does its own.

The milestone is complete when, in QEMU, devices behind the emulated hub enumerate and work and a device behind a hub can be pulled and replugged like one on a root port; and when, on the Lenovo, a real high-speed hub carries the owner's keyboard, mouse and stick at once, the keyboard types, the mouse moves, and the stick mounts, unmounts and is pulled safely. QEMU's hub is a USB 1.1 hub, so QEMU proves the tree, the route string and the hub protocol, and only the notebook proves the transaction translator; the plan says which proof is which.

This plan is the authority for the run. A conflict between this plan and the current tree is resolved by the decisions here; a conflict between this plan and `CLAUDE.md` is resolved by `CLAUDE.md`. The plan was revised once after independent review, which moved the think time to the hub's own context, made a connection change close the old subtree first, added the hub-level status, the power modes and the initial port scan, took back the promise about SuperSpeed sticks behind a USB 3 hub, raised the device table from the notebook's inventory, and named the notebook's fixtures.

## What the tree settles before this plan

* `hcd.h` describes a device by `device_open(port, speed)`: a root port and a speed, nothing else. `xhci.c` builds a slot context with the route string zero and the root port number, and derives an endpoint's interval from the device's speed alone. The core scans the root ports, debounces a connection, resets the port through the controller, and keeps one command in flight for the whole stack. From milestone 39: a transfer carries a generation, a device has a closing state that must complete before anything of it is reused, the device table and the slot count are eight, endpoints go up to 15 in each direction, and a stick is a block device with an admission queue and a teardown count.
* The xHCI slot context has the fields a hub needs and this kernel has never written: the route string in bits 0 to 19 of the first word, four bits per tier for five tiers, each holding the port number on the hub above; the hub bit 26 and the multi-TT bit 25 of the first word; the number of downstream ports in bits 24 to 31 of the second word; the TT hub slot ID in bits 0 to 7, the TT port number in bits 8 to 15 and the TT think time in bits 16 to 17 of the third word. A low or full-speed device behind a high-speed hub is addressed with the slot and port of the hub that translates for it, and the controller then issues split transactions itself.
* The hub class, from the USB 2.0 specification's chapter 11: a hub is class 9, with one interrupt IN endpoint whose report is a bitmap, bit 0 for the hub and bit N for port N; the hub descriptor, type `0x29`, gives the port count, the characteristics with the power-switching mode and the TT think time, the power-on-to-power-good time in 2 ms units, and the removable-device mask; the class requests are `GET_DESCRIPTOR` for that descriptor, `GET_PORT_STATUS` giving a status word and a change word, and `SET_PORT_FEATURE` and `CLEAR_PORT_FEATURE` with the selectors `PORT_RESET` 4, `PORT_POWER` 8, and the change selectors `C_PORT_CONNECTION` 16, `C_PORT_ENABLE` 17, `C_PORT_SUSPEND` 18, `C_PORT_OVER_CURRENT` 19 and `C_PORT_RESET` 20; the status word has connection in bit 0, enable in bit 1, reset in bit 4, power in bit 8, low speed in bit 9 and high speed in bit 10. A high-speed hub's protocol byte is 1 for one transaction translator shared by its ports or 2 for one per port, the latter selected by the interface's alternate setting 1. A port reset is at least 10 ms and a connection is debounced 100 ms, as on a root port. Phase 1 checks every one of those numbers against the specification's text before they are coded, since this bullet is written from memory.
* A USB 3.0 hub is two hubs: a SuperSpeed hub with its own descriptor type `0x2A` on the USB 3 root port, and a high-speed hub with the descriptor above on the paired USB 2 root port. A device plugged into it that runs at SuperSpeed appears under the first; everything else appears under the second.
* QEMU's `usb-hub` is a full-speed USB 1.1 hub with eight ports; devices behind it run at full or low speed, there is no transaction translator, and it can be attached to `qemu-xhci` and have devices attached to it by name. The Lenovo's three sockets are root ports; the owner has no hub today.

## Decisions on the controller interface

`device_open(port, speed)` becomes `device_open(const struct usb_location *where, int speed)`, and `usb_location` is the tree address: the root port, the parent device handle or none, the port on the parent, the route string as the controller wants it, and, for a low or full-speed device below a high-speed hub, the handle and port of the translating hub. The core fills it; the controller writes it into the slot context; a second controller would translate it into whatever it needs. `device_hub(device, ports, multi_tt, think_time)` is new: after the core has read a hub's descriptor it tells the controller that this slot is a hub with that many ports and, for a high-speed hub, whether it has one translator per port and what its think time is, which the controller writes into the hub's own slot context through the same evaluate-context command it already uses for a full-speed device's packet size. The think time is the translating hub's property and lives in its slot context alone; a child carries only which hub and which port translate for it. A full-speed hub is told its port count and nothing about translators, because it has none. Nothing else in `hcd.h` changes, and every existing caller passes a location with no parent.

A route string is built from the parent's route string and the port on the parent, four bits per tier, and a depth beyond five tiers is refused by name, since that is the specification's limit and the field's. A port above 15 cannot be written into a nibble and a device on one is refused by name rather than encoded as something else, which is also why a hub publishes at most 15 ports.

## Decisions on the core

A port belongs to a root controller or to a hub, and the core's enumeration state machine stops caring which: a device record names its location, and the port operations it calls, status, reset begin, reset done, and settle, go to the controller for a root port and to the hub driver for a hub port, through one small vtable the device record holds. The debounce, the reset, the addressing, the descriptors, the configuration and the refusal-by-port are otherwise the code milestone 35 wrote, and the refusal bitmap becomes one per hub as well as one for the root.

The core matches class 9 to the hub driver the way it matches class 3 to HID and class 8 to storage. Because a hub brings devices with it, enumeration of a hub's ports starts only after the hub itself is configured, and one device is enumerated at a time across the whole tree, as it is across the root ports, so the one command slot is never contended.

When a hub is pulled, every device below it is pulled: the core closes them depth-first, children before their hub, each through milestone 39's closing state, so that a stick behind a pulled hub is torn down exactly as one on a root port, with `EIO` for its holders and reclamation at its count. When a hub is refused, or fails, its ports are never scanned.

The device table grows to twelve entries before anything else is written, from the notebook's own inventory: four soldered devices, a USB 3 hub's two halves, a keyboard, a mouse and a stick are nine, and a second stick or a second hub must still fit. Twelve is the constant, the tests fill it and refuse the thirteenth, and phase 6 records the notebook's count against it.

## Decisions on the hub driver

`sys/usb/hub.c` is the class driver. It reads the hub descriptor through the control endpoint, tells the controller the port count and the translator arrangement, selects alternate setting 1 when the hub offers one translator per port, powers every port with `SET_PORT_FEATURE(PORT_POWER)`, whether the hub switches them together or one at a time, and waits the power-good time. Power is applied the way the descriptor says the hub switches it: one request for all ports on a ganged hub, one per port on an individually switched one, none on a hub that cannot switch; after the power-good time every port's status is read once, so that a device already plugged in when the hub arrived, and a port that came up unpowered, disabled or over-current, enters the state machine without waiting for a change that may never be reported. Then it opens the interrupt IN endpoint and reads the change bitmap. Bit 0 is the hub itself: its status is read and its local-power and over-current changes cleared and recorded. For each port whose bit is set it reads the port status, acknowledges each change with its `CLEAR_PORT_FEATURE`, and hands the change to the core. A connection change is a change of identity whatever the status word says now: whatever was below that port is closed first, depth-first through the closing state, and the port's refusal bit cleared, and only when that has finished is the port debounced and, if a device is present, enumerated; that is what handles a device swapped for another between two reads. An enable lost is recorded and the device below closed. An over-current is recorded, the port powered off and its subtree closed, powered on again once after a second, and left off with a record if it recurs.

A port reset behind a hub is `SET_PORT_FEATURE(PORT_RESET)`, then the status read until the reset bit clears and the enable bit is set, with a deadline, then `C_PORT_RESET` cleared; the speed comes from the status word's two bits. That is the USB 2 procedure and the only one here, since a SuperSpeed hub is refused before its ports are ever looked at. A port that never finishes its reset is refused by name and its bit set in the hub's refusal bitmap. Every one of those is a control transfer through the core's one-at-a-time machinery, so a hub with a keyboard and a stick takes about as long to bring up as the two would on root ports plus the hub itself.

The status-change endpoint is the hub's interrupt endpoint and is read the way HID reads a report: a transfer always outstanding, resubmitted at each completion, with the bitmap's length being the port count plus one, rounded up to bytes. A hub that stalls it or stops answering is closed with its children.

Nothing here writes the hub's own descriptors, sets indicators, suspends a port, or handles hub-level power loss beyond recording it and closing the subtree.

## Decisions on the translator

A low or full-speed device below a high-speed hub gets, in its location, the slot and the port of the nearest high-speed hub above it, and the controller writes them into the device's slot context; the think time it needs is already in that hub's slot context from `device_hub`; with those the controller issues the split transactions and this kernel writes none. The interval derivation stays what it is, because the device's own `bInterval` means what it always meant for the device's own speed. A full-speed hub below a high-speed hub is itself a device with a translator above it, and a device below that full-speed hub uses the same translator, which is the specification's rule and one line in the location code.

QEMU cannot exercise any of this, since its hub is full speed and a full-speed hub on an xHCI root port translates nothing. The host model can: the controller model gains rows that check the slot context words for a low-speed device under a high-speed hub, and the notebook is the only place the hardware is proved.

## Decisions on USB 3 hubs

A SuperSpeed hub, class 9 with descriptor `0x2A` and its own port status layout, is refused by name in this milestone; its high-speed half, the second device the same hub presents, is driven, so a USB 3 hub in the blue socket carries USB 2 keyboards, mice and sticks at high speed and below. What a SuperSpeed-capable stick does behind such a hub is not promised: it may attach to the refused SuperSpeed half and appear nowhere, and only phase 6 can say whether the hub lets it fall back to high speed. Until it says so, a SuperSpeed stick behind a USB 3 hub is unsupported, the Readme says to use a USB 2 socket or hub for it, and the SuperSpeed hub's own port count and depth are not needed.

## Memory and limits

| Limit | Value |
|-------|-------|
| Tree depth | 5 tiers below the root, the specification's and the field's |
| Ports per hub | 15, the field's; a hub with more publishes 15 and says so |
| Hubs at once | 2, each with its refusal bitmap and change buffer |
| Devices in the tree | 12, from the notebook's inventory with headroom |
| Power-good wait | the hub's, at most 500 ms |
| Port reset deadline | 500 ms |

## What stays out

SuperSpeed hubs: refused by name, their high-speed half driven. Port suspend and resume, indicators, and over-current recovery: recorded, not handled. Hub-to-hub power budgeting: no. A hub pulled and replugged inherits nothing, as a stick does. Interrupt-driven completion: still a milestone of its own.

## Decisions on tests and tooling

* `tests/host/xhci_hosttest.c` gains the location rows: a device on a root port with an empty route string, a device on port 3 of a hub on root port 2 with the route string `0x3` and the root port 2, a device two tiers down, a device on the fifth tier accepted and a sixth refused, port 15 of a hub, a hub slot with the hub bit and its port count written through evaluate context, a multi-TT hub with its bit, a low-speed device under a high-speed hub with the translator's slot, port and think time in the third word, a full-speed device under a full-speed hub under a high-speed hub using the high-speed hub as translator, two hubs at once accepted and a third refused, and twelve devices filled with a thirteenth refused. Those rows prove that the words are what the specification says and that the tests can tell; they do not prove that a controller schedules from them, which only the notebook does.
* `tests/host/hub_hosttest.c`: the hub driver against a model of a hub reached through the core's control and interrupt paths, the way the storage model reaches the class driver. Rows: the descriptor read and the port count passed on, with the think time given to the hub's slot and not to a child's, and a full-speed hub given none; ganged, per-port and unswitched power each handled; the power-good wait and the scan of every port after it, finding a device already present; a hub-status change on bit 0 read and cleared; a connection reported in the bitmap, its status read, its change cleared, and the core told; a connection change with the status still connected closing the old subtree before enumerating the new device; an over-current powered off, retried once, and left off; a reset that completes and one that never does; the speed read from the status bits for low, full and high; a disconnection closing the device below; a hub pulled with two devices below closing both children first; a multi-TT hub selecting its setting; a stalled change endpoint closing the hub; a hub with 20 ports publishing 15; and a hub a sixth tier down refused.
* `tests/usbprobe.c` gains the tree: `/proc/usb` shows each device's location as `root port, hub port, ...`, and the probe checks a keyboard and a mouse behind the hub against it.
* `tests/system/suites/usb.sh` gains boots with `usb-hub` on `qemu-xhci`: a keyboard and a mouse behind the hub typing and moving on a machine with no 8042, as the root-port boots do; a stick behind the hub mounted, written, pulled through `device_del` and replugged; the hub itself pulled with all three behind it, the stick's mount answering `EIO` and unmounting, the keyboard's held keys released; a hub behind a hub with a keyboard on it; and eight devices in the tree with a ninth refused by name.
* `tests/system/accept/metal_accept.sh` gains the hub checks as named fixtures, each a line in the record: a low-speed keyboard behind a real high-speed hub typing; a full-speed device behind a full-speed hub behind that high-speed hub; a multi-TT hub's setting selected, if the owner's hub is one, and otherwise recorded as untested; a USB 3 hub in the blue socket with its SuperSpeed half refused and its high-speed half carrying the devices; and whether a SuperSpeed stick behind it appears at all. The translator's mutations in the controller test are rerun against the notebook: the malformed slot, port and think time must make the low-speed keyboard fail there, or the claim that the model proves them is narrowed in the Readme to the words alone.

## Documentation

* `rach2/sys/usb/Readme.md`: the location, the route string, the hub driver and its port machinery, the translator and why this kernel writes no split transaction, USB 3 hubs, the depth-first close, the limits, what is refused, and which proof is QEMU's and which the notebook's.
* `rach2/sys/fs/synthfs.Readme.md`: `/proc/usb`'s location column.
* `rach2/Readme.md`: the supported hardware and the limits.
* `Readme.md`: the hardware status.
* `tests/Readme.md`, `tests/host/Readme.md`, `tests/system/Readme.md`.
* `dev-log/40-hub/Readme.md` and the `dev-log/Readme.md` entry, in phase 7.

## How the work is kept track of

`progress.md`, `current.md`, `notes.md` and `hardware.md` beside this plan, as before, and the one-line report `M40: phase N of 7; X of Y items done; HEAD abc1234; tree clean|dirty`.

## Standing rules for the run

* Work directly. The only delegation is an independent review, one reviewer per unit, at most six files, at most ten minutes. There are six review checkpoints, after phases 2, 3, 4, 5, 6 and 7, with the number of units fixed in phase 1, and one focused re-review for a fix that changes a contract.
* Commit in scoped units in the house style, on the branch, no attribution, never push.
* Every commit passes `tests/run style` and `tests/run host`. A commit that touches a subsystem runs that subsystem's suites singly. The whole set runs after phases 3 and 5 and at the end of phase 7.
* A test is proved by making it fail. A mutation is driven from a script that restores the source in a trap and rebuilds before the verdict is believed.
* A change that alters a contract rewrites the owning Readme in the same commit.
* A change that makes something redundant removes it in the same commit. The tree after this milestone is not larger than the milestone's own additions.
* Phases 2 to 5 wait on nothing outside the tree. Phase 6 does not start until the owner has a high-speed hub, a low-speed keyboard and a full-speed device to put behind it; that is its entry condition, and phase 1 asks for them so that they are there by the time the code is.
* On the Lenovo, the writes are the kernel installation through `bootctl` and writes to the owner's stick, under milestone 39's rules. The SSD is not written beyond the kernel slot, and its partitions are hashed before and after.
* Nothing is deferred. A thing discovered mid-run goes into `notes.md` with the phase that will do it, and phase 7 closes every line or records the owner's decision to drop it.

## The phases

### Phase 1: arrival and inventory

Read `AGENTS.md`, `CLAUDE.md` and this plan in full. Require a clean tree and record `git log --oneline -12`. Create the four tracking files in the first commit. List the files each review checkpoint will cover and fix the units.

Check every number in the hub-class bullet above against the USB 2.0 specification's chapter 11 and the xHCI specification's slot context, and record each with its section in `notes.md`; a number that was wrong is corrected there and in this plan's Readme later. Ask the owner for a USB 2.0 or 3.0 hub for phase 6, a cheap unpowered one being the harder and the better test, and record what the kernel of milestone 39 says when the hub is plugged in, which is a refusal by name.

Proof: the table in `notes.md` with every number sourced.

### Phase 2: the location

`usb_location` in `hcd.h`, `device_open` on it, `device_hub`, the route string and its depth, the slot-context words in `xhci.c`, and every caller passing a root location. The location rows in `xhci_hosttest.c`. `tests/run usb` and `tests/run usbdisk` unchanged.

Proof: the rows pass; every existing suite passes.

Mutations: write the route string with the port in the wrong tier; forget the hub bit; write the translator's port as the device's own; accept a sixth tier. Each must fail exactly its row.

### Phase 3: the core on a tree

The port vtable on the device record, the hub-port operations, the depth-first close, the per-hub refusal bitmap, and class 9 matched. The hub model and `hub_hosttest.c`'s enumeration and close rows.

Proof: the rows pass; in QEMU a keyboard behind `usb-hub` enumerates and `/proc/usb` shows its location.

Mutations: scan a hub's ports before it is configured; close a hub before its children; let two devices enumerate at once across the tree; keep a pulled hub's child in the table. Each must fail exactly its check.

### Phase 4: the hub driver

`hub.c` in full: the descriptor, power, the change endpoint, port status and changes, reset, speed, multi-TT, and the failure cases. The rest of `hub_hosttest.c`.

Proof: every row passes; in QEMU a keyboard and a mouse behind the hub type and move on a machine with no 8042, and a stick behind it mounts, is written, is pulled and replugged.

Mutations: skip the power-good wait; skip the scan after it, which the already-present row must catch; clear the wrong change bit; read the speed from the wrong bit; resubmit the change transfer only once; never clear `C_PORT_RESET`, which the model's second connection must catch; enumerate on a connection change without closing what was there, which the swap row must catch; write the think time into a child's location. Each must fail exactly its check.

### Phase 5: the suite

The `usb.sh` hub boots, `usbprobe`'s tree, the eight-device boot, and `tests/run all`.

Proof: the suite passes and so does everything else.

### Phase 6: the machine

The Lenovo, over SSH, with the owner's hub. Install the kernel through `bootctl` as the approved write and commit it. Plug the hub into each of the three sockets in turn with the keyboard, the mouse and the stick on it: `/proc/usb` with the tree, typing and moving at the console, the stick mounted and written and unmounted, the stick pulled while mounted with the record showing `EIO`, a device replugged behind the hub, and the hub itself pulled with everything on it and replugged. A low-speed keyboard behind the high-speed hub is the translator's proof, and the record says which hub and which socket. If the hub is a USB 3.0 one, its SuperSpeed half is shown refused by name in the blue socket and its high-speed half carrying the devices. The device count is recorded against the twelve. `metal_accept.sh` runs its named fixtures.

Proof: the three devices work through the hub in every socket; the record has the translator's case; the SSD's hashes match.

### Phase 7: the record and the sweep

As in the milestones before, and as large. The documents, every one listed above, read against the code, with a Codex read of the reader-facing ones. The code: the root-port path and the hub-port path read for what became one and should be one function; no helper with one caller. The build: every rule, manifest line, fixture and tool listed in `notes.md` with its reason; no `build_*.sh`; `guestcheck` and `tests/run selfbuild` pass. The tests: the tier decision measured; a boot that proves what another proves removed; the mutation scripts in `notes.md`; the full set twice with no machine left running, and `make test` timed against milestone 39. The record: `notes.md` closed line by line, `dev-log/40-hub/Readme.md` with its trust statement, which names the translator as the notebook's proof alone, and the `dev-log/Readme.md` entry in the same commit. The tree is clean and nothing is pushed.

## Risks, in expected order

* The translator fields wrong in a way only the notebook can show. The host model checks the words against the specification, phase 1 checks the numbers against the text, and phase 6 has a low-speed keyboard behind a high-speed hub, which is the one case that fails loudly if they are wrong.
* A hub-class number in this plan wrong from memory. Phase 1 exists for that, and it precedes every line of code.
* The device table too small for the notebook's tree. The inventory above says nine with one hub; twelve is the constant, the tests fill it, and phase 6 records the count.
* The one command slot serialising a hub's bring-up into seconds. Measured and recorded; a hub with three devices is a few dozen control transfers, and the record says how long they took.
* The owner having no hub. Phases 2 to 5 close without one; phase 6 waits, and the plan says so.

## Recovery after a compaction

Read `AGENTS.md`, `CLAUDE.md`, this plan's decisions and the current phase, `progress.md`, `current.md`, `notes.md` and `hardware.md`. Run `git status --short --branch` and `git log --oneline -15`, inspect every uncommitted path, and resume the current phase's first unfinished item. Report the one line.

## The goal prompt

`Run milestone 40, hubs, from dev-log/40-hub/plan.md.`
