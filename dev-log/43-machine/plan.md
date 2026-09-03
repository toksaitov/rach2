# Milestone 43: the machine

## The goal

Everything milestones 36 to 42 built is proved on the Lenovo G50-70, in one session, in the order it was built: the mount interface and the block path, writing FAT32, exFAT, interrupts, the USB disk, hubs, and the ext2 root. Each of those milestones closed on QEMU with its machine checks written into `tests/system/accept/metal_accept.sh` and its procedures written into its `hardware.md`; this milestone runs them, fixes what the machine shows, and moves the machine to the latest of everything, with its disk never leaving its bay.

The milestone is complete when the notebook runs the final kernel on an ext2 root with the KRFFS root as its fallback, mounts and writes its FAT32 scratch partition, takes interrupts from its USB and audio controllers, reads and writes the owner's stick in both partition-table formats on a root port and behind a hub with a keyboard and a mouse beside it, boots from a Rach2 stick and uses it, and the acceptance script passes end to end; and when every difference between the machine and QEMU that the session found is fixed in the owning subsystem, with a host-model row for it, rather than worked around here.

This is the one milestone that needs the owner, and it says at its start exactly what it needs: the machine on mains power with suspend off and reachable over SSH; two flash drives, one of which becomes the recovery stick and stays that for the whole session, the other the data stick that is reformatted several times; a USB 2.0 or 3.0 hub; the USB keyboard and mouse from milestone 35; a person at the machine for the steps a machine cannot do to itself, choosing the stick at the BIOS boot menu, plugging and pulling devices, typing and moving at the console, and pressing reset for the crash test; and the approvals the disk rule in `CLAUDE.md` requires, each presented with the values read from the machine at that moment.

This plan is the authority for the run. A conflict between this plan and the current tree is resolved by the decisions here; a conflict between this plan and `CLAUDE.md` is resolved by `CLAUDE.md`. The plan was revised once after independent review, which gave the recovery stick its own drive for the whole session, listed every physical action and every approval, and defined what counts as a difference. The procedures below are the ones the earlier plans wrote and rehearsed; where this plan is shorter than they were, their `hardware.md` is the detail.

## What the tree settles before this plan

* The machine: a Lenovo G50-70, Haswell-ULT with the Lynx Point-LP chipset, an AHCI disk in its bay with a 64 MiB FAT32 boot partition, a 500 MiB KRFFS root, a 64 MiB FAT32 recovery partition, and about 250 GB free after them; an RTL8168GU on the network; an xHCI at `00:14.0` whose USB2 pins milestone 35 routes to it at every boot, so that all three sockets are root ports; an HDA controller; a PS/2 keyboard and touchpad on the 8042 that this kernel leaves alone. It boots from a USB stick over the BIOS, which milestone 24 proved. It is updated over SSH as `Readme.md` describes: a kernel through `bootctl` into a slot with a trial and a fallback, a root or a partition through recovery and `reflash`.
* The disk rule in `CLAUDE.md`: the disk does not leave its bay; before any disk write that is not a kernel install, the recovery stick is booted on the machine and shown to reach `reflash`, the sectors about to change are saved on the host and on that stick, and recovery is the saved boot default until the write is verified. A procedure that cannot be undone from that stick is not run.
* What each earlier milestone left for here, in its `hardware.md` and in `metal_accept.sh`: milestone 36, the read-only mounts of the boot and recovery partitions and the lag check through the shell; 37, the fourth partition's creation and the FAT32 write check, rehearsed on the sealed image in QEMU; 38, nothing of its own, since exFAT arrives with the stick; 39, the APIC diagnostic `rach2.apicdiag`, the interrupt counts and the `rach2.nomsi` boot; 40, the stick in both table formats, the pull and the replug, and the boot from the Rach2 stick image; 41, the hub with the three devices, the translator's case, and the USB 3 hub's two halves; 42, the ext2 image into the fourth partition, the boot menu with two command lines, the trial, the deliberate fallback, and the commit last.
* `rach2-recovery-iso` from milestone 37 boots a recovery kernel and its RAM image from a stick, with `reflash` and, from milestone 42, `e2fsck`. `rach2-stick-image` from milestone 40 boots the metal image from a stick with an exFAT data partition beside it.
* The record of milestone 35's session: the machine differs from QEMU in what it reports about its controller, its firmware and its ports, and every such difference was fixed in the driver with a model row, which is the rule this milestone keeps.

## Decisions

### The order

The checks run in the milestones' order, because each later one stands on the earlier: a stick cannot be mounted before FAT32 writes and exFAT are proved on the machine's own disk, a hub is not worth plugging in before a stick on a root port works, and the root is not switched before everything else runs on the KRFFS root. A check that fails stops the session at that point until its cause is fixed in the tree, its host model has a row for it, and its suites pass again; only then does the session continue, with the fixed kernel installed through `bootctl` as before.

### The kernel installs

Every kernel this session installs goes through `bootctl kernel`, a trial boot and `bootctl commit`, with the previous kernel as the fallback, which is the mechanism milestone 23 built and this milestone does not change. The first install is booted with `rach2.nomsi` before it is booted without, because milestone 39's interrupts are the one thing in the kernel that the machine has never seen and that could keep a boot from reaching the shell; `rach2.nomsi` is the boot line that makes that cost one boot.

### The recovery stick

Before anything is written to the disk, the recovery ISO is written to the first stick, the machine is booted from it at the BIOS menu, and `reflash` is shown to read sector zero back over SSH. That boot is the proof the disk rule requires, and the stick then carries the saved copies the rule names. It stays the recovery stick for the whole session and is never reformatted; the recovery ISO is written to it again before milestone 42's step, because that ISO gained `e2fsck`, and the boot is proved again then. Every other job a stick has in this plan is the second stick's.

### What the owner is asked, and when

Once at the start: to put the machine on mains power with suspend off, plug in the USB keyboard and mouse, and hand over the two sticks and the hub. Then, each at its own step, with the values read from the machine at that moment, an approval for every write to the disk, which the disk rule requires one by one: the fourth partition's entry in sector zero, the FAT32 image into it, the ext2 image into it, the boot menu file, and, at the end, the commit of the ext2 root; and any write the differences phase needs, presented the same way. To reformat the data stick between steps, with the exact command for the Mac each time; to choose the stick at the BIOS menu for the boots from a stick; to plug, move and pull devices when a step says; to type and move at the console when a step says; and to press reset once for the crash test. Nothing else, and nothing in between; the session runs on its own from one ask to the next, and each ask says what it is for and what happens if it is refused.

### What is fixed here and what is not

A difference is a check in `metal_accept.sh` that fails or a contract in a Readme that the machine breaks. A measurement that comes out different from QEMU's, throughput above all, is evidence and is recorded, and it is a difference only when it crosses a threshold the owning plan stated, so that a session is not spent modelling a number nobody promised. A difference is fixed in the subsystem that owns it, with a row in that subsystem's host model that reproduces the machine's behaviour, and with its Readme changed if a contract changed; that is milestone 35's rule and it stands. A fix goes through the same reviews as the milestone that owns the code would have given it. Nothing is worked around in the acceptance script, and nothing is deferred: every difference is fixed in this milestone, with Codex's opinion taken where the call is close, and the one recorded exception is a limit of the hardware itself, stated in the owning Readme as what the machine is.

## Decisions on tests and tooling

* `tests/system/accept/metal_accept.sh` is the script, grown by each milestone; this milestone runs it in full, and its every line is a check that was written when the code was, not here.
* `tests/run metal` and `make metal-test` rehearse the script against QEMU before the session, as they did in milestone 24.
* A host-model row for every difference the machine shows, in the owning test.

## Documentation

* `Readme.md`: the hardware status, rewritten to what the machine does at the end of this session, and "Updating the installed machine" with the ext2 root, the fallback, and the stick.
* `rach2/Readme.md`: the supported hardware and the limits the session confirmed or changed.
* Every subsystem Readme that milestones 36 to 42 said would be proved on the machine: the sentence that said so becomes the result.
* `tests/system/Readme.md`: the acceptance procedure.
* `dev-log/43-machine/Readme.md` and the `dev-log/Readme.md` entry, in phase 9; `hardware.md` here consolidates what the seven earlier ones recorded and what this session read.

## How the work is kept track of

`progress.md`, `current.md`, `notes.md` and `hardware.md` beside this plan, as before, and the one-line report `M43: phase N of 9; X of Y items done; HEAD abc1234; tree clean|dirty`.

## Standing rules for the run

* Work directly. A fix's review is the owning subsystem's, one reviewer per unit, at most six files, at most ten minutes; the number of review units is fixed in phase 1 as one per phase that finds a difference, and a phase that finds none has none.
* Commit in scoped units in the house style, on the branch, no attribution, never push.
* A fix's commits run the rows they touch; a fix closes with `tests/run style`, `tests/run host` and the owning subsystem's suites, once, before its kernel is installed on the machine. Delegation follows the same rules as every milestone: review and proofreading only, at most five units at once, none starting another, on Codex or a model of Opus 5's strength or better.
* The disk rule in `CLAUDE.md`, at every step, every time. Every disk write is presented with the machine's own values, approved by name, and hashed before and after.
* Nothing is worked around in the acceptance script; nothing is deferred.

## The phases

### Phase 1: arrival and the first install

Read `AGENTS.md`, `CLAUDE.md`, this plan, and the `hardware.md` of milestones 36 to 42 in full. Require a clean tree and record `git log --oneline -12`. Create the four tracking files in the first commit, `hardware.md` starting as the seven earlier ones joined. Ask the owner for the machine, the stick, the hub and the two input devices, once.

Over SSH: record what the machine runs now, `bootctl status`, `/proc/mounts`, `/proc/usb`, and the hashes of its three partitions. Run `rach2.apicdiag` through a trial boot of the milestone 39 diagnostic and record the APIC, the `LINT` entries and the two controllers' capability lists beside QEMU's. Install the final kernel through `bootctl`, trial it with `rach2.nomsi`, then without, and commit it. With it running: `/proc/interrupts` read before and after a USB key is typed and before and after audio is played through the console for five seconds, under MSI and again under `rach2.nomsi`, so that both controllers' counts are seen to move or not; a disk read and a network transfer of a few megabytes with their counters, which are polled paths and must simply still work. Run `metal_accept.sh`'s milestone 36 lines: the partition nodes, the read-only mounts of the boot and recovery partitions with their listings compared with `bootctl status`, the lag check through the shell, and the partition hashes unchanged afterwards.

Proof: the kernel is committed; `/proc/interrupts` counts the xHCI's and HDA's interrupts, or the record says which path each took and why; the milestone 36 lines pass; the hashes match.

### Phase 2: the fourth partition and FAT32

Write the recovery ISO to the first stick, boot the machine from it at the BIOS menu, and show `reflash` reading sector zero. Present the fourth partition's proposal from milestone 37's `hardware.md` with the disk's identity, size and current entries read now, and wait for approval. Then milestone 37's procedure exactly: recovery as the saved default, sector zero and the partition table saved on the host and on the stick, the sixteen bytes of the fourth entry changed and every other byte compared, the sector written and read back and restored on any mismatch, the `mformat` image written into the range after its own approval and hashed back, the ordinary root booted once through `next_entry`, and the milestone 37 lines of `metal_accept.sh`: write, rename, remove, `sync`, reboot, read back, the partition pulled over SSH and read with mtools and the checker. Then the three older partitions hashed again.

Proof: the fourth partition exists and its volume reads back identically on the host; the three hashes match; the ordinary root is the saved default again.

### Phase 3: the stick on a root port

The data stick reformatted on the Mac as exFAT with a GUID table, plugged into each socket in turn: `/proc/usb`, `blkid`, a mount, a listing compared with the Mac's, a file written and synced, `umount`, and the file read on the Mac; the pull while mounted with the `EIO` in the record and the unmount after; the replug. Then the same with an MBR. Then the milestone 38 checks that needed a real exFAT writer: the volume the Mac formatted, read and written by this kernel and checked clean by `fsck_exfat` afterwards. Then the stick image written to the data stick, the machine booted from it at the BIOS menu, its second partition mounted and written, and the stick read on the Mac.

Proof: milestone 40's and 38's lines of `metal_accept.sh` pass; the throughput on the machine is recorded with MSI and with `rach2.nomsi`; the file crosses both ways in both formats; the boot from the stick reaches the shell and mounts.

### Phase 4: the hub

The hub in each socket in turn with the keyboard, the mouse and the stick on it: `/proc/usb` with the tree, typing and moving at the console, the stick mounted and written and unmounted, the stick pulled while mounted, a device replugged behind the hub, and the hub pulled with everything on it and replugged. The low-speed keyboard behind the high-speed hub is the translator's proof, and the record says which hub and which socket. A USB 3.0 hub's SuperSpeed half is shown refused by name in the blue socket and its high-speed half carrying the devices, and whether a SuperSpeed stick appears behind it is recorded. The translator's mutations from milestone 41's controller test are rerun against the machine, and the device count is recorded against the twelve.

Proof: milestone 41's lines pass; the three devices work through the hub in every socket; the record has the translator's case.

### Phase 5: the ext2 root

The recovery ISO written to the recovery stick again and booted at the BIOS menu, since milestone 42 added `e2fsck` to it, with the saved copies refreshed. Present the ext2 image's reflash into the fourth partition, from recovery, with milestone 37's safeguards, and wait for approval; write it and hash it back. Present the `bootctl config` diff that puts `rach2.root=ext2` on the candidate entries and leaves the fallback on `rach2.root=disk`, with the menu file saved on the host and on the stick, and wait for approval; write it. Trial boot. On the ext2 root: `bootctl status`, `tcc` builds a program, a file survives a reboot of the same trial, `df` shows the volume, SSH works, the keyboard, the mouse, the stick and the hub work as before. Boot the fallback deliberately, which is the KRFFS root, show it comes up, and boot the candidate again. Present the commit and wait for approval; commit. Then a crash test the owner agrees to: the machine reset with a write in flight, the dirty root's check recorded on the next boot, and, if the check found a fault, recovery's `e2fsck` from the stick repairing it.

Proof: milestone 42's lines pass; the machine runs on ext2 with the KRFFS root one menu entry away and proved; the KRFFS root and recovery partitions' hashes match; the boot partition's change is the predicted list of files.

### Phase 6: the acceptance script end to end

`metal_accept.sh` in full, from the ext2 root, with everything plugged in: the hub with the keyboard, the mouse and the data stick on it in one socket. The recovery stick is not touched, so the lines that write the disk run with their recovery path intact, and a line that would need the recovery stick's boot re-proved has it re-proved first. `tests/run metal` against QEMU on the same script.

Proof: every line passes on the machine and in QEMU.

### Phase 7: the differences

Every difference the session found, listed in `notes.md` with the phase that found it, is fixed in its subsystem with its model row and its Readme, reviewed, its suites run, and the fixed kernel installed and the phase's lines rerun on the machine. A difference the owner decided to leave is written into the owning Readme as a limit.

The owner decided on 2026-09-03 that the stick in the blue socket, alone and behind the hub, is not a limit but work of this milestone. Alone in the blue socket the stick already works, as a SuperSpeed device on a root port. Behind the hub it does not, because the hub in that socket is two hubs, and the stick attaches to the SuperSpeed one, which the kernel refuses. So this phase adds the SuperSpeed hub to the hub driver: its descriptor, which is the USB 3 hub descriptor and not the USB 2 one; its port status words, whose speed and link fields differ; its port power, reset and change handling, which the core drives through the same vtable; and the controller's slot context for a device below it, whose route string is the same and whose translator fields are empty. A SuperSpeed device behind it is enumerated at its speed, and the record names the hub's two halves and which devices sit on which. The host test gains rows for the USB 3 descriptor, the status words and the tree, each proved by a mutation; QEMU's hub cannot prove it, so the machine does, with the stick behind the hub in the blue socket mounted and written and pulled.

Proof: `notes.md` has no open difference; every subsystem's suites pass; the machine runs the last kernel; the stick behind the hub in the blue socket mounts and takes a file.

### Phase 8: the machine's own build

`tests/run selfbuild` on the machine, from the ext2 root, through the staged TinyCC and pdpmake, which is the read test of a real working set on real hardware, and the kernel it builds installed and committed through `bootctl` as the final kernel.

Proof: the machine builds its own kernel and runs it.

### Phase 9: the record and the sweep

As in the milestones before. The documents: every Readme that said "proved on the machine in milestone 43" now says what was proved, `Readme.md`'s hardware status rewritten, "Updating the installed machine" current, and a Codex read of the reader-facing documents. The code: the acceptance script read for lines that prove what another proves; no workaround left in it. The build: nothing new, and anything a fix added listed in `notes.md` with its reason. The tests: every model row a difference added is named in the owning Readme; the full set once with no machine left running, rerun only after a later fix, and `make test` timed against milestone 42. The record: `notes.md` closed line by line, `hardware.md` consolidated, `dev-log/43-machine/Readme.md` with its trust statement, which says what the machine proved that QEMU could not and what remains unproved on any hardware but this one, and the `dev-log/Readme.md` entry in the same commit. The tree is clean and nothing is pushed.

## Risks, in expected order

* The first kernel with MSI not reaching the shell on this firmware. `rach2.nomsi` is booted first, the trial's fallback is the previous kernel, and the diagnostic has read the APIC before the install.
* A sector-zero write. It is one step, rehearsed in QEMU in milestone 37, done with the recovery stick proved and the original saved on it and on the host, with recovery as the default until verified, and restored on any mismatch.
* The stick or the hub doing something the models never saw. That is what this milestone is for; the rule is a row in the model and a fix in the driver, and the session stops until it has them.
* The ext2 root switch. The KRFFS root and the previous kernel are the untouched fallback until the end, the fallback is booted on purpose, and the commit is the last approval.
* Time. Seven milestones of hardware checks in one session; the order is fixed, each phase closes on its own, and a session interrupted between phases resumes from `current.md` with the machine in a known state, because no phase leaves it half-written.

## Recovery after a compaction

Read `AGENTS.md`, `CLAUDE.md`, this plan's decisions and the current phase, `progress.md`, `current.md`, `notes.md` and `hardware.md`. Run `git status --short --branch` and `git log --oneline -15`, inspect every uncommitted path, read `bootctl status` and `/proc/mounts` from the machine to learn what it runs, and resume the current phase's first unfinished item. Report the one line.

## The goal prompt

`Run milestone 43, the machine, from dev-log/43-machine/plan.md.`
