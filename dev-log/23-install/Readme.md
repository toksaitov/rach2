# Milestone 23: installing the kernel from the machine

Milestone 22 made the machine build its own kernel; this one lets it install what it built and boot into it, with no host tool anywhere in the loop. The mechanism was chosen for the machine this project means to run on again: an old Rach2 booted a real notebook from the GRUB disk, a future milestone will make the current one do so again, and the install flow works the same there as in QEMU, because everything it touches lives on the disk.

## What a student does with it

```
# cd /src/rach2
# make -f guest.mk
# install-kernel kernel    # a one-boot trial; a crash falls back
# reboot -f
...
# install-kernel --commit  # the booted kernel becomes the saved default
```

On the GRUB disk (`make os-real-grub-disk`, booted by `rach2/run_from_disk.sh` with no `-kernel`), the boot partition holds two kernels: `/boot/kernel`, the candidate slot the installer rewrites in place, and `/boot/fallback`, staged at build and never written again. Every install is a trial: GRUB boots the candidate once through its `next_entry` state and a power cycle after a crash returns to the fallback, so a kernel that does not boot costs a reboot, not the machine. The debug menu entry always boots the fallback with the kernel debug shell behind `rach2.rescue`.

## The design, and why stock GRUB stays the loader

The alternatives were argued to agreement before the plan: a custom GRUB module reading KRFFS would keep `/boot/kernel` a normal file but pins a GRUB source build and a second implementation of the on-disk format into the project forever; an in-house bootloader is the most teaching value and no longer needs GRUB at all, but it is real-mode assembly, INT 13h, E820 and BIOS quirks, and it belongs to the bare-metal milestone, which can reuse these slots unchanged; kexec boots a kernel warm and installs nothing. What shipped instead: the FAT boot partition GRUB already reads gains a second kernel file and GRUB's environment block, the guest rewrites only data sectors through a new root-only raw device, and the environment block's `saved_entry`/`next_entry`, compared against exact menu ids, decide every boot. No FAT driver entered the kernel; no GRUB was modified.

Phase 0 was an experiment before any code, and it changed the design: GRUB's multiboot loader takes load extents from the program headers, so a zero-padded candidate boots identically; `save_env` rewrites the block in place on our FAT32; but `next_entry` is NOT consumed built-in by this GRUB build (the config carries the scripted consumption Ubuntu's own configuration uses), and the `fallback` variable does not resolve an `--id` string (it parses as entry number 0 and retries the broken default), so the config pins `fallback=1` with the menu order as part of its contract. Both corrections came from observed boots, and the second manual observation was itself overturned once: a driver bug fed every variant the same config, the kept artifacts recorded which config each boot ran, and the corrected driver reproduced the original findings exactly.

## What shipped, per phase

* `4f60b46` phase 0: the plan's GRUB hypotheses replaced with observations from a scripted, artifact-keeping experiment driver.
* `a7a3a7f` phase 1: every `/dev` entry gained a mode the open path enforces with the file systems' own nine-bit check (everything existing stays 0666; the fstat path now reports the entry's mode rather than a constant); `/dev/bootpart`, mode 0600 root, a bounded seekable window over MBR partition 1 when the parsed table qualifies it (FAT-typed, contained, disjoint from the KRFFS partition), whole sectors only, every transfer through a kernel bounce buffer because the AHCI driver programs physical DMA addresses; the MBR parse lifted out of the mount into `sys/block/mbr.c`, pure and host-tested; and `/proc/cmdline`, a global proc file covering classifier, snapshot, stat, listing and fd-origin seams, so a program can read which GRUB entry booted the machine. `bootprobe` (`make bootpart-test`) drives every seam on a fixture disk of known bytes and proves the device absent on a single-partition disk.
* `28ff6b2` phase 2: `install-kernel` over `utils/bootslot.c`, a pure core behind caller-supplied sector callbacks: the BPB validated before one field is trusted, both FATs compared whole, the namespace walked recursively under a visited bitmap (a cluster reached twice is a cross-link and a refusal), every reachable regular file's size held to its chain, the three named files resolved, kernels validated to the loader surface (program headers, loads with physical addresses equal to virtual, the exact multiboot flags, entry), the environment block edited in place with GRUB's header required and every write read back. The install order is the interruption matrix's; the host seam test drives the same core over fixtures from an independent python FAT writer, with a write callback that drops, tears and cuts.
* `8eaa057` phase 3: the GRUB disk carries the slots, the initialized environment block, the `loadenv` module and the exact-id config with scripted consumption; the build verifies every staged chain and the four boot files' disjointness fail-closed, and `run_from_disk.sh` stopped advising a rebuild that would discard installed state.
* `4c2caae` phase 4: `make install-test`, the loop proved twice per invocation on the booted disk: slot identity from `/proc/cmdline`, a banner marker edited into `/src/rach2` and built in place (the marker is what proves which bytes are running; the slot token is menu text and a slot hash is bytes at rest), the armed-commit refusal, the trial reboot, the commit, a cold boot, corruption at the `--where`-named sector, the fallback boot proved marker-free, the repair from inside, and the matrix's determinate states booted from host-crafted blocks.

## What the reviews caught

Four adversarial rounds on the plan before any code (25 findings), then one per phase. The catches that changed contracts:

* A torn environment block can still load (the format has no checksum), so the config compares values against exact menu ids instead of assuming a torn write fails to parse, and the matrix holds that every state boots a bootable kernel rather than that every state boots the fallback.
* The kernel validator accepted files GRUB cannot boot; it now judges the loader surface, and the fallback file is validated before the install's first write, because after `saved_entry=fallback` a broken fallback would be the saved default.
* `env_set` preserved a stale armed `next_entry` across a new install, so a retry could leave a half-written slot armed; one transition now moves the saved entry and disarms the trial together, and every environment write is read back, so a dropped sector is an error at install time and not a wrong boot later.
* A commit could save a kernel that never booted, two ways: an install under the running candidate leaves the trial armed, and an INTERRUPTED install leaves no trial state at all while the slot holds partial bytes. A commit now requires the consumed-trial marker (`next_entry=`, the empty value GRUB's consumption saves): the armed line and the absent line are both refusals, each with its own diagnostic and seam case.
* The disk build's chain verification failed open twice (a pipeline hid `mshowfat` failures; two sizes were never checked); it runs each command under `sh -e` and requires all four records with exact counts.
* The harness's own mutation run caught the harness: with the arming write removed, the commit-refusal probe was no longer refused and committed the unbooted kernel, which then booted as the saved default with its marker in place, so the identity interval would have passed. The binding assertion is the refusal itself, host-side, and the plan records the observed shape in place of its prediction. The same round removed two fail-open paths: success tokens are written split so a command's echo cannot satisfy them, and a feeder whose step never arrives stops feeding instead of driving commands into the wrong session, both re-proved by end-to-end negatives (a disk whose saved default is the fallback; a config without consumption).

## What was refuted or disproved

The plan's shown-failing prediction for phase 4 (a fallback boot without the marker) was disproved by running it: the observed failure is the unrefused commit described above. The first manual phase 0 observation of `next_entry` consumption contradicted the driver's reproduction until the driver's own variant-selection bug was found; with it fixed, non-consumption stood. The milestone 22 record briefly said 25 utilities and was restored to its history's 24: `install-kernel` is this milestone's, and only the current-contract documents count it.

## What stays deferred, and why

* The bare-metal gaps the design was shaped around but does not solve: the AHCI driver's deferred real-hardware reset sequence, the embedded `(hd0,msdos1)` prefix (a search by file-system identifier would survive BIOS disk reordering), reset fallbacks beyond port 0xCF9, and UEFI-only machines. They gate the bare-metal revival, not the install mechanism, which was built to survive them.
* One administrator at a time: `install-kernel` takes no lock over the device, and two concurrent installs are outside its contract, stated in its documentation.
* The QEMU `-kernel` developer flow cannot install (QEMU reloads the host file on reset); `install-kernel` exits nonzero naming the missing slot state there. The plain `os-real.disk` has no boot partition and the device never appears.
* A commit is bound to "a trial boot ran and no install or interruption followed it", read from the environment block's trial state, not to a digest of the exact slot bytes; binding to a digest needs trusted running-state the kernel does not carry. What remains outside the rule is deliberate concurrent misuse under the stated one-administrator contract.

## The gate

The whole gate ran from a tree `make distclean` had emptied: `make full`, then `verify-style`, `verify-host`, `test` (61 probes), `tcc-test`, `make-test`, `selfbuild-test`, `bootpart-test`, `install-test`, `ssh-test`, `account-test`, `shutdown-test`, `login-test`, `ping-test`, `crash-test`, `test-disk`, `test-disk-ahci`, `persist-test`, `persist-test-ahci`, `cred-persist-test` and `test-noentropy`. Every step passed.
