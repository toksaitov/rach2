# Milestone 40: the USB disk

## The goal

A flash drive plugged into a root port becomes a disk: enumerated by the USB core, driven by a mass-storage class driver over Bulk-Only Transport, published as a block device that sleeps its caller, with its partitions read from an MBR or a GUID partition table, mountable as FAT32 or exFAT through everything milestones 36 to 38 built, and safe to pull out at any moment. Then the kernel boots from a stick and mounts that stick's data partition, in QEMU and on the Lenovo G50-70, with the keyboard and mouse still working beside it.

The milestone is complete when, in QEMU, a virtual stick appears, mounts, is written, is pulled while mounted at every stage of a transfer and gives `EIO` rather than a crash, is plugged back in and mounts again with nothing inherited, and a machine booted from a stick image mounts that same stick's second partition. The Lenovo's part, the owner's stick in both table formats and the boot from a Rach2 stick, is written into `metal_accept.sh` here and run in milestone 43. Hubs are milestone 41: everything here is on a root port, and the Lenovo's three sockets are three root ports since milestone 35 moved them.

The milestone has two halves with a gate between them: the disk, with its removal and replug, through phase 5; and the stick image and the boot from it, from phase 6, which does not start until the first half is closed and reviewed. The first review of this plan asked for them to be separate milestones; they are separate halves of one instead, because the second is small and depends on nothing but the first.

This plan is the authority for the run. A conflict between this plan and the current tree is resolved by the decisions here; a conflict between this plan and `CLAUDE.md` is resolved by `CLAUDE.md`. The plan was revised once after independent review, which added the transfer generation and the close-before-reuse rule, the admission queue, the atomic wait, the teardown count, the held-buffer rule, the full BOT error matrix, the whole-array GPT check, and the stick image's layout, and removed three things that were not needed for a correct disk.

## What the tree settles before this plan

* The USB stack from milestone 35: `hcd.h` with `device_open(port, speed)`, `endpoint_open(device, endpoint)`, `submit`, `poll` and `command_done`, all non-blocking, drained from the timer's `usb_poll` at 100 Hz through one function; `core.c` with a four-entry device table, one command in flight for the whole stack, a closing state that waits for the controller to disable a slot, and a refusal by port for a device it has no driver for; `xhci.c` with four slots, endpoint numbers 0 to 3 only, interrupt and control endpoints, and one Normal TRB per transfer. A completion in `hcd.h` today names a device and an endpoint and nothing else. The core takes only alternate setting zero of an interface and has no `SET_INTERFACE`.
* The block path from milestones 36 and 37: `block_request` with a pool of sixteen requests of one page each, a write copied into the page before `submit` and a read copied out after, a device that provides `submit` instead of polled `read` and `write`, `block_complete(request, generation, status)`, a per-device deadline, the quarantine of a timed-out request, and the lag device as the only sleeping device so far. `wq_wait_event` re-checks its condition with interrupts off before every sleep, which is what makes it safe against a lost wakeup. `bcache_write` for metadata and write-back for data; a device whose write fails takes its mount read-only; `bcache_invalidate` refuses while a buffer is held.
* Mounts, claims and the partition nodes from milestone 36: `/dev/disk0p1` to `p4` from the MBR, read-only raw windows that report themselves as character devices, claims by sector range, a mount that is `unmounting` refusing new work, and `umount2` refusing `EBUSY` while anything holds the mount. `bootctl` and `reflash` use those windows and their tests stat them.
* The FAT32 and exFAT drivers, their dirty-volume checks, and the `update` process that syncs every thirty seconds.
* The kernel is non-preemptive inside a syscall and has no kernel threads; AHCI polls with interrupts off. Since milestone 39 the xHCI controller delivers its interrupts by MSI into the one pump the timer also runs, and `rach2.nomsi` turns that off.
* BusyBox in the os-real image has `blkid`, `findfs` and `mount LABEL=`, with `volume_id` for FAT, exFAT and ext; it finds devices by scanning `/dev` for block-device nodes and reads their first sectors.
* QEMU: `-drive if=none,id=stick,format=raw,file=stick.img -device usb-storage,bus=xhci.0,drive=stick` attaches a stick; from the monitor, `drive_add 0 if=none,id=...,file=...` then `device_add usb-storage,bus=xhci.0,drive=...,id=...` plugs one in and `device_del` pulls it out; `bootindex` makes SeaBIOS boot from it.
* Bulk-Only Transport: a 31-byte command block wrapper with the signature `USBC`, a tag, the transfer length, a direction flag, the LUN and a command of 1 to 16 bytes; a 13-byte status wrapper with `USBS`, the tag, the residue and a status of 0, 1 or 2, where 2 is a phase error; the class requests `Bulk-Only Mass Storage Reset` (0xFF) and `Get Max LUN` (0xFE, one byte, at most 15), both addressed to the interface's number; reset recovery as the reset request followed by clearing the halt on the IN and then the OUT endpoint; interface class 8, subclass 6 for the SCSI transparent set, protocol 0x50 for Bulk-Only and 0x62 for UAS.
* What real sticks do, from the field: fail `TEST UNIT READY` for a while after insertion, sometimes a dozen times; need `REQUEST SENSE` after any failure before the next command; return the last block's address from `READ CAPACITY`, not the count; stall `Get Max LUN`; hide the flash behind a CD-ROM LUN on promotional drives; answer `MODE SENSE(6)` short and put write protection in bit 7 of its byte 2; stop responding after a command they do not know; and crash when reset at initialisation, so reset is for recovery only. Write-back caches on some sticks make `SYNCHRONIZE CACHE` part of every unmount.
* A stick formatted on a Mac has a GUID partition table by default, behind a protective MBR of type `0xEE`. A stick formatted on Windows has an MBR. Some sticks have no table at all and carry a volume from sector zero.
* The Lenovo's three sockets are root ports of the one controller since the routing of milestone 35; `/proc/usb` lists what is enumerated.

## Decisions on the controller interface

`hcd.h` gains what a device that can vanish mid-transfer needs and a keyboard never did. A `usb_transfer` carries a generation the caller chooses, and the `usb_completion` for it carries the same, so a completion that arrives for a transfer the caller has given up on is told from the one it wants. `device_close` is asynchronous as it already is inside the core's closing state, and the core exposes that state: a device is `closing` from the moment it is pulled or given up until the controller has stopped its endpoints and disabled its slot, which are commands with completions, and nothing that belonged to it, its table entry, its endpoint handles, its class driver's state or its buffer, is reused before the state ends. A completion for a closing device is dropped with a record. The close has a deadline, and a controller that never answers leaves the entry a tombstone with a record, which is the one way a table entry is lost.

`xhci.c` learns bulk endpoints: endpoint context type 2 for OUT and 6 for IN, the maximum packet the descriptor gives (64 at full speed, 512 at high speed, 1024 at SuperSpeed), and endpoint numbers up to 15 in both directions, which means `XHCI_MAX_DCI` becomes 32 and the endpoint tables grow with it, because a real stick picks its own numbers. A transfer is at most one page, which never crosses the 64 KiB boundary the specification forbids a TRB to cross, so one Normal TRB carries it and no chaining is needed. The device table grows to eight entries and the slot count to eight, so that a keyboard, a mouse, two sticks and the four soldered devices the notebook refuses fit at once; the eighth is filled and a ninth refused in a test. The scratchpad and ring memory grow with the slots and are measured in `hardware.md`.

## Decisions on the class driver

### The transport

`sys/usb/msc.c` is the class driver, matched on class 8, subclass 6, protocol 0x50, on alternate setting zero, which is the only setting the core takes; a device whose Bulk-Only interface is on another setting, or that offers only protocol 0x62, is refused by name, and a composite device's interface number is kept and used in every class request. It is a state machine stepped from the one drain the timer and a waiting process share, with one command in flight per device: command wrapper out, data in or out, status wrapper in, and it advances as far as completions allow within one pass of the drain, so that a wrapper's completion submits the data stage in the same pass and the data's completion submits the status stage.

The error matrix is the specification's thirteen cases and the field's additions, each a row in the model. A status wrapper with the wrong signature, the wrong tag, a status byte outside 0 to 2, or a residue larger than the request, and a phase error, are reset recovery: the class reset, then the halt cleared on the IN endpoint, then on the OUT endpoint, then the command is failed with `EIO`. A STALL in the data stage is the halt cleared on that endpoint and the status wrapper read, with the residue reported. A STALL on the status wrapper is the halt cleared on IN and the wrapper read once more; a second STALL is reset recovery. A short wrapper is reset recovery. A `READ(10)` or `WRITE(10)` that succeeds with a nonzero residue is `EIO`, because a block is whole or it is not. Reset is never issued at initialisation. Every stage of recovery is a command through the core's one command slot, with the core's turn-taking, its own deadline, and a device that never answers becoming a tombstone.

### Bringing a stick up

`Get Max LUN`, with a STALL read as zero and a value above 15 refused. Each LUN in turn: `INQUIRY` for the device type, and for a direct-access one `TEST UNIT READY` repeated at 100 ms intervals for up to ten seconds with `REQUEST SENSE` after every failure, moving to the next LUN when the sense says the medium is absent and stopping at the first LUN that is ready; a promotional drive's CD-ROM LUN is passed over, and a device with no ready LUN in ten seconds is refused by name. `READ CAPACITY(10)`, with `0xFFFFFFFF` refused before one is added and a block size other than 512 refused by name, because the block layer is 512-byte sectors. `MODE SENSE(6)` for the write-protect bit, read as a short answer without a stall being an error; a protected stick is a read-only block device, which is a flag `struct block_device` gains and every partition view over it carries, and its mounts are read-only. Nothing else is sent at bring-up, and `PREVENT ALLOW MEDIUM REMOVAL` is never sent.

### Transfers

`READ(10)` and `WRITE(10)` in units of one page, eight sectors, through the request's own page, which is milestone 36's contract unchanged; a larger transfer is eight-sector requests one after another. The throughput that gives is measured and recorded, not promised, and a larger request is a later optimisation with its own review, not this milestone's. A command that fails with a check condition gets `REQUEST SENSE`, the sense key and code go into the record, a not-ready or unit-attention sense is retried once, and anything else is `EIO`.

`struct block_device` gains `flush`, which for a stick is `SYNCHRONIZE CACHE(10)` and for the polled disks nothing, and partition views pass it through. `bcache_sync` calls it after the device's dirty buffers are written, and so does `prepare_unmount`, so that a stick with a write-back cache has what the volume says it has before it is pulled. A flush the stick answers with ILLEGAL REQUEST is a stick with no cache to flush, recorded once and treated as done; one it answers with any other failure, or not at all, fails the sync or the unmount with `EIO`, the mount returns to `active` as milestone 36 says, and the volume stays dirty on the medium, which is what it is.

### The block device and its queue

Each stick is a `block_device` with `submit` and no fault-safe flag, a deadline of ten seconds because a stick may spin up a controller of its own, and one request in flight. Two processes may want it at once, since either can sleep in a request while the other runs, so the device has an admission queue: a `block_request` on a busy stick waits on the device's wait queue until the device is idle, then submits, in the order they arrived, and a flush waits its turn like a read, so it is ordered after every write before it. The completion is `block_complete` from the class driver's step. A timed-out request is quarantined as milestone 36 says, its transfer generation is abandoned, and the class driver runs reset recovery on the device before the slot is returned; a device that does not survive recovery is closed as if pulled.

### Waiting for a stick

The sleeping side of `block_request` on a USB device submits and sleeps on the request's done flag. Milestone 39 gave the controller its interrupt: the MSI handler runs the one pump, `usb_pump`, that the timer also runs every tick, with interrupts off inside it; the pump drains the event ring, advances the class driver's state machines, completes the request and wakes the sleeper through `wq_wake_all`. Submission runs inside the same critical section, so neither context sees a half-published request, and `wq_wait_event` re-checks the done flag with interrupts off before sleeping, so a completion that lands between the check and the sleep is not lost. A lost edge costs one tick, because the timer's pump is the fallback, and the counter milestone 39 added says how often that happened. With `rach2.nomsi` the timer is the only pump and a request completes at the next tick, which is slower and still correct.

## Decisions on naming and tables

### Nodes

A stick is `/dev/usbN`, with `N` the lowest free number when it is enumerated, and its partitions are `/dev/usbNp1` to `p4`; they are raw windows like `/dev/disk0` and its partition nodes, readable by root, and they report themselves as block devices in `stat`, which is what they are and what BusyBox's `blkid`, `findfs` and `mount LABEL=` scan `/dev` for; with that, `mount LABEL=STICK /mnt` works from the shell with no change to BusyBox, and a volume is found by its label rather than by the order sticks were plugged in. `/dev/disk0` and its windows stay character devices, as `bootctl`, `reflash` and their tests know them; changing them was in the first draft and is not needed for anything here. A stick with no partition table whose sector zero is a FAT or exFAT boot sector is published as `/dev/usbN` alone, mountable as it is, which is how such sticks are used everywhere. `/proc/usb` gains the vendor, product and serial strings and the node name for a stick.

### Partition tables

`mbr.c` already parses an MBR. `gpt.c` is new: a protective MBR of type `0xEE` at entry zero sends the reader to the GUID partition table header at sector 1, whose signature, revision, size, header CRC32, its own location, and the entry array's location, count, entry size and CRC32 are checked, with every product and offset computed in checked arithmetic. The whole entry array, up to 128 entries of at least 128 bytes each and at most 16 KiB, is streamed through the request path with its CRC32 computed over all of it, and only then are the first four entries with a nonzero type GUID published, wherever in the array they lie, each checked to have its first sector not after its last, both inside the usable range, no overlap with another, and its last sector within what `READ(10)` can address. When the primary header fails, the backup header at the last sector is read and validated against its own entry array, used with a record, and never written. CRC32 is the one in every GPT and zip file, thirty lines, in `lib/`. A stick with more than four partitions publishes the first four and says so. The same reader serves the AHCI disk from now on, which changes nothing there because the sealed disk has an MBR.

### Removal

A stick pulled out is a port that says so, which the core already sees. From that moment the device is `closing`: the transfer in flight, if any, is abandoned by generation and its request completed with `EIO`; every request waiting in the admission queue is completed with `EIO`; every later `block_request` on the device answers `EIO` at once; the buffer cache evicts the device's clean unheld buffers, marks its dirty ones failed, which takes the mount read-only as milestone 37 says, and marks its held ones for discard at their release, because a buffer a sleeping process holds cannot be reused under it; and every operation on the mount answers `EIO` from then on.

A device has one teardown count: its mounts, the descriptions on them, the working directories inside them, the requests accepted or quarantined for it, and the cache buffers held on it. A pulled device whose count is zero, which is a stick nobody opened, is reclaimed as soon as its closing state ends: its claim, its nodes and its table entry go back, so a stick plugged and pulled a hundred times exhausts nothing. A pulled device whose count is not zero is a tombstone until the count reaches zero: descriptions answer `EIO` until they are closed, `umount2` succeeds once nothing holds the mount, and the last release reclaims it. A stick plugged back in is a new device with the lowest free number, which may be the same one, and a new claim; nothing from before survives, because nothing from before is reused until the closing state ended.

Data written since the last sync is lost when a stick is pulled, at most thirty seconds of it because of `update`, and the volume is left dirty so that the next mount's check runs. That is the same loss Linux and Windows accept, and `umount` before pulling is what avoids it.

## Decisions on booting from a stick

The firmware and GRUB load the kernel and the RAM root from the stick over the BIOS, which milestone 24 proved on the notebook. New is the image: `rach2-stick-image` is the metal hybrid ISO followed by an exFAT volume, with an MBR that keeps the hybrid's boot code and first entry, and gains a second entry of type `0x07` whose first sector is the volume's, at a 1 MiB boundary after the ISO, and whose length is the volume's; the volume is built by `mkexfatfixture` with `PartitionOffset` equal to that first sector and `VolumeLength` equal to that length, so the kernel's check of both fields passes. The image is MBR-only; if the ISO builder's hybrid form carries a GUID table, the stick image does not use it, because a GUID table written to a larger stick leaves its backup header short of the last sector. Phase 1 reads what `tools/image/mkiso.sh` produces and records it. The finished image, not its parts, is validated with the kernel's partition rules and booted under the BIOS in QEMU. The kernel booted from it enumerates the stick as a disk, `/dev/usb0p2` is the data partition, and `mount -t exfat /dev/usb0p2 /mnt` is the capstone. The root stays in RAM; a root on a stick is milestone 42's question.

## Memory and limits

| Limit | Value |
|-------|-------|
| USB devices, and controller slots | 8 |
| Endpoints per device | 15 in each direction |
| Sticks at once | 2 |
| Request | one page, 8 sectors, milestone 36's pool |
| Admission queue per stick | 16, one per process |
| Bring-up deadline | 10 seconds of `TEST UNIT READY` |
| Request deadline | 10 seconds |
| Close deadline | 5 seconds, then a tombstone |
| Partitions per stick | 4, from an MBR or the first four used GPT entries |
| GPT entry array read | 16 KiB, 128 entries |
| Block size | 512 bytes; another size is refused by name |
| Wait inside a request | an atomic sleep, woken by the controller's interrupt or by the timer's fallback pump |

## What stays out

Hubs, and a stick behind one: milestone 41. Requests larger than a page and the throughput they buy: a later optimisation with its own review. `/proc/partitions` and block-device `stat` for `/dev/disk0`: not needed and not done. UAS, and Bulk-Only on a nonzero alternate setting: refused by name. Card readers with several media LUNs: the first ready direct-access LUN is taken and the rest are recorded, not served. `READ(16)` and drives over 2 TiB: refused by name. Ejecting a stick from software: no; a person pulls it. Booting the root from a stick: milestone 42.

## Decisions on tests and tooling

* `tests/host/msc_hosttest.c`: the class driver against a model of a Bulk-Only device reached through `hcd.h`, the way `xhci_hosttest.c` models a controller, and the model can be told to vanish at any stage. Rows: a stick that comes up at once; one that fails `TEST UNIT READY` twelve times then succeeds; one that never does and is refused at the deadline; `Get Max LUN` stalled and read as zero, and answered with 16 and refused; a CD-ROM on LUN 0 with the disk on LUN 1, and a medium-absent LUN 0 passed over; a composite device with the interface at number 2, with the class requests addressed to it; Bulk-Only on alternate setting 1 refused; `READ CAPACITY` with the last-block rule and with `0xFFFFFFFF`; a 4 KiB block size refused; `MODE SENSE` short with the protect bit set and clear; a read of a page and of one sector; a write; each case of the error matrix above answered exactly as specified; a check condition followed by `REQUEST SENSE`, retried once for unit attention and failed for a medium error; the next stage submitted in the same pump pass as the completion before it, which is the row that proves the state machine advances within a pass; a completion delivered by the model between the waiting side's check of the done flag and its sleep, which the atomic wait must not lose; a device that stops answering, hitting the deadline, quarantined, reset, and closed when reset never completes; `flush` as `SYNCHRONIZE CACHE` and its three answers; an unmount of a mount with buffered data on a write-back stick, with the data and the flush both seen by the model before `prepare_unmount` returns; two requests admitted in order with the second waiting; a flush ordered after the writes before it; and the device vanishing during the wrapper, the data in, the data out, the status, `REQUEST SENSE`, a reset stage, bring-up, and with two requests admitted, each completing every request with `EIO`, dropping a late completion by generation, and reclaiming the entry only after the close.
* `tests/host/gpt_hosttest.c`: a valid table; a bad header CRC with a good backup; both bad; an entry CRC broken in an entry beyond the fourth, which proves the whole array is checked; entries with the first used one at index six; an entry size of 256; an entry past the usable range; two overlapping entries; an entry past the `READ(10)` limit; a header whose array count overflows the arithmetic; a backup header whose array disagrees with it; a protective MBR with no table behind it; and an ordinary MBR left alone.
* `tests/host/xhci_hosttest.c` gains the bulk rows: an endpoint of type 2 and 6 opened with the right packet size at each speed, a bulk descriptor that arrives with an interrupt type refused, endpoint 8 IN opened, all eight slots filled and a ninth refused, a completion carrying its generation, and a completion for a closing device dropped.
* `tests/host/bcache_hosttest.c` gains the removal rows: a held clean buffer marked for discard and not reused, a dirty buffer marked failed, and the discard at release.
* `tests/usbdiskprobe.c`: on a QEMU stick, `/proc/usb` read back, a mount, a read, a write, a `sync`, and, after the harness pulls the stick, `EIO` on read and write, `EBUSY` on `umount2` while the file is open, success after it is closed; after the harness plugs it back, a mount of the same volume, with the image inspected on the host after the pull to show the dirty marker reached it; a stick plugged and pulled without being opened, ten times, with `/proc/usb` showing the same number free afterwards; and a process asleep in a read when the stick is pulled, returning with `EIO`.
* `tests/system/suites/usbdisk.sh`, `tests/run usbdisk`, core tier: a stick image with an MBR holding a FAT32 and an exFAT partition, and a second with a GPT holding the same with the first used entry not at index zero; a superfloppy stick; `usb-storage` on `qemu-xhci` beside the keyboard and the mouse; the probe boots; the pull-and-replug boots through `device_del` and `device_add` with `drive_add` first, with the pull placed by the harness during a read; a BusyBox boot with `mount LABEL=`, `blkid`, `cp` and `umount`; a throughput line, megabytes per second for a 16 MiB read, recorded in `notes.md` as a measurement; and, after the gate, a boot from the stick image with `bootindex` that mounts its own second partition.
* The harness learns `drive=add:` and `drive=del:` steps for the monitor's `drive_add` and `drive_del`, beside its `usb=` steps.
* `tests/run usb` keeps passing with the larger tables, and `lagprobe`'s sleeping rows run against a stick as well as the lag device.
* `tests/system/accept/metal_accept.sh` gains the stick checks, written here and run in milestone 43.

## Documentation

* `rach2/sys/usb/Readme.md`: the transfer generation and the closing state, bulk endpoints, the larger tables, the class driver and its state machine, bring-up and what the field taught, the error matrix, transfers and the request page, the block device and its queue, the pump and the atomic wait and why not interrupts, removal and the teardown count, naming, what is refused and why, and the verification.
* `rach2/sys/block/Readme.md`: the second sleeping device, `flush`, the read-only flag, the admission queue, GPT, and the block-device `stat` type of the USB nodes beside the unchanged windows.
* `rach2/sys/fs/vfs.Readme.md`: a mount whose device has gone, and `flush` in `sync` and `prepare_unmount`.
* `rach2/sys/fs/synthfs.Readme.md`: `/proc/usb`'s new columns and the `usbN` nodes.
* `rach2/Readme.md`: the subsystem table, the supported hardware, the limits, and booting from a stick.
* `Readme.md`: the system paragraph, the images, the hardware status.
* `tools/Readme.md`: the stick image target.
* `tests/Readme.md`, `tests/host/Readme.md`, `tests/system/Readme.md`.
* `dev-log/40-usbdisk/Readme.md` and the `dev-log/Readme.md` entry, in phase 7.

## How the work is kept track of

`progress.md`, `current.md`, `notes.md` and `hardware.md` beside this plan, as before, and the one-line report `M40: phase N of 7; X of Y items done; HEAD abc1234; tree clean|dirty`.

## Standing rules for the run

* Work directly. No delegate writes code, documents or tests; delegation is independent review and proofreading only, in units of at most six files and ten minutes, at most five running at once, none starting another, every delegate reporting findings while this agent makes every change, and every delegate on Codex or a model of Opus 5's strength or better, because a weaker model's judgement of code is worth less than the time it saves. The rounds are fixed: one gate round after phase 5, where the disk half must hold before the stick image, and one round before the sweep, plus one focused re-review for a fix that changed a contract; the units are listed when the round starts. Before any round, this agent reads the whole diff against the plan and the owning Readmes, checks bounds, error paths, lifetimes, locking, the build entries and redundant code, and assembles the test and mutation results, so that the round confirms rather than discovers.
* Commit in scoped units in the house style, on the branch, no attribution, never push.
* Gates, chosen for the ten-core machine and a run that should not wait on itself: a small commit runs the rows or the smallest suite that covers it; each phase ends with `tests/run style`, `tests/run host` and the affected subsystem suites, once; a new or changed check is proved by its mutation when it is written, once, through the restoring script, and a passing mutation is not rerun; the full set runs once on the milestone's final tree and again only after a fix that changed an executable, the build or a test.
* Before the review rounds, every document and comment the milestone touched is read against the register rule in `CLAUDE.md` and `prosecheck` runs; one review unit is a proofread of the reader-facing documents and comments, told to reject the banned terms and the contextual shorthand `CLAUDE.md` names, reversals and fragments written for effect, intentions given to code or hardware, vague verbs where a concrete one exists, body-narrating comments, headers that repeat Readmes, and documentation that records a phase instead of the contract.
* A test is proved by making it fail. A mutation is driven from a script that restores the source in a trap and rebuilds before the verdict is believed.
* A change that alters a contract rewrites the owning Readme in the same commit.
* A change that makes something redundant removes it in the same commit. The tree after this milestone is not larger than the milestone's own additions.
* Phase 6 does not start until phase 5 is closed and its review is done.
* Nothing touches the Lenovo in this milestone. Milestone 43 asks the owner for the stick, formatted with a GUID table and then with an MBR, and runs the checks written into `metal_accept.sh` here.
* Nothing is deferred. A thing discovered mid-run goes into `notes.md` with the phase that will do it, and phase 7 closes every line or records the owner's decision to drop it.

## The phases

### Phase 1: arrival and inventory

Read `AGENTS.md`, `CLAUDE.md` and this plan in full. Require a clean tree and record `git log --oneline -12`. Create the four tracking files in the first commit. List the files each review checkpoint will cover and fix the units.

Nothing here waits on the owner; the stick is milestone 43's. Build the two QEMU stick images and the superfloppy by hand once and read them with mtools and `mkexfatfixture --check`. Read what `tools/image/mkiso.sh` produces as a hybrid image, its MBR and whether it carries a GUID table, into `notes.md`.

Proof: the tables in `notes.md` and `hardware.md`.

### Phase 2: the controller interface and the tables

The transfer generation, the exposed closing state with its deadline, bulk endpoints, the endpoint numbers, the eight slots and devices. The new rows in `xhci_hosttest.c`. `tests/run usb` unchanged.

Proof: the rows pass; the USB suite passes with the larger tables.

Mutations: deliver a completion with a stale generation; reuse a slot before its close completed; open a bulk endpoint with an interrupt type; refuse endpoint 8; let a ninth device in. Each must fail exactly its row.

### Phase 3: the class driver

`msc.c` with the transport, bring-up, transfers, the error matrix and reset recovery; the model, with its vanishing; `msc_hosttest.c` in full. The block device with `submit`, `flush`, the read-only flag, the admission queue, and `usb_pump` with the atomic wait.

Proof: every row passes; in QEMU, `/proc/usb` shows the stick as a disk with its size, and a raw read of its first sector through `/dev/usb0` gives the MBR.

Mutations: send reset at initialisation, which the crash-on-reset model must catch; skip `REQUEST SENSE` after a failure; take the capacity as the last block; take the first LUN regardless of readiness; clear the OUT halt before the IN; retry a stalled status wrapper twice; accept a read with residue; advance one stage per tick, which the same-pass row must catch; let a second request overwrite the first, which the two-request row must catch; check the done flag without interrupts off before sleeping, which the model's completion-between-check-and-sleep row must catch; complete a request after its deadline into a live slot, which the quarantine row must catch. Each must fail exactly its check.

### Phase 4: naming, tables and removal

The `usbN` nodes as block devices, the `/proc/usb` columns, `gpt.c` and `gpt_hosttest.c`, the superfloppy case, removal with the teardown count, the held-buffer rule in the cache, and the replug as a new device. The harness `drive=` steps and `usbdiskprobe`.

Proof: the GPT rows pass; the probe passes on the MBR stick, the GPT stick and the superfloppy; `mount LABEL=` works from the shell; a pulled stick answers `EIO` and unmounts after its files close; a stick never opened is reclaimed at once; a replugged one mounts with nothing inherited.

Mutations: publish a partition from an array whose CRC fails beyond the fourth entry; keep serving a pulled device; free the device table entry while a description is open; free it while a request is quarantined; reuse a held buffer after removal; give a replugged stick its old claim; report the USB nodes as character devices, which `mount LABEL=` must catch; skip the flush in `prepare_unmount`, which the model's write-back stick must catch. Each must fail exactly its check.

### Phase 5: the suite

`tests/run usbdisk` without the boot from the stick, the throughput line, and `tests/run all`. This is the gate: the first half is closed here and reviewed before the image exists.

Proof: the suite passes and so does everything else; the throughput is recorded.

### Phase 6: the stick image

`rach2-stick-image` with the layout above, validated as a whole image by the kernel's partition rules and by `mkexfatfixture --check` on its second partition, and the boot from it in QEMU that mounts its own second partition, added to the suite.

Proof: the boot reaches the shell from the stick alone and mounts `/dev/usb0p2`.

Mutations: append the volume without its MBR entry; write `PartitionOffset` as zero; overlap the entries. Each must fail exactly its check.

### Phase 7: the record and the sweep

As in the milestones before, and as large. The documents, every one listed above, read against the code, with a Codex read of the reader-facing ones. The code: no helper with one caller; the refusal-by-port logic and the device table's growth read again for what the larger table made unnecessary; the closing state read for what the old close left behind. The build: every rule, manifest line, fixture and tool listed in `notes.md` with its reason; no `build_*.sh`; `guestcheck` and `tests/run selfbuild` pass. The tests: the tier decision measured; a boot that proves what another proves removed; the mutation scripts in `notes.md`; the full set once on the final tree with no machine left running, rerun only after a later fix, and `make test` timed against milestone 38. The record: `notes.md` closed line by line, `dev-log/40-usbdisk/Readme.md` with its trust statement, and the `dev-log/Readme.md` entry in the same commit. The tree is clean and nothing is pushed.

## Risks, in expected order

* The owner's stick doing something the model does not. The field list above is the model's row list, and a stick that does something else is refused by name and recorded, then the model gains the row and the driver the case, in that order.
* The pump shared by the interrupt handler, the timer and a submitting process. One function, interrupts off inside it, submission inside it, and the atomic wait; the model's row that completes between the check and the sleep is the deterministic proof, and the stress row in `lagprobe` is the second.
* Removal at the wrong moment. The model vanishes at every stage and the rows assert what is reclaimed and when; the teardown count is one number, and the probe reads it back through `/proc/usb` after a hundred plugs.
* Throughput. Measured in QEMU in phase 5 and on the notebook in milestone 43, with MSI and with `rach2.nomsi`; the page-sized request is the ceiling of this milestone and the number is in the record.
* The stick image. Phase 1 reads what the ISO builder produces before the layout is committed to, and the whole image is validated, not its parts.

## Recovery after a compaction

Read `AGENTS.md`, `CLAUDE.md`, this plan's decisions and the current phase, `progress.md`, `current.md`, `notes.md` and `hardware.md`. Run `git status --short --branch` and `git log --oneline -15`, inspect every uncommitted path, and resume the current phase's first unfinished item. Report the one line.

## The goal prompt

`Run milestone 40, the USB disk, from dev-log/40-usbdisk/plan.md.`
