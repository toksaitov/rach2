# Milestone 23: installing the kernel from the machine

Milestone 22 made Rach2 build its kernel. This milestone lets it install and boot that kernel without a host tool. The design also serves the intended real machine. An older Rach2 booted a notebook from a GRUB disk, and a later milestone will restore that capability. Installation behaves the same on the notebook and in QEMU because it changes only disk state.

## Installing a kernel

```
# cd /src/rach2
# make -f guest.mk
# install-kernel kernel    # a one-boot trial; a crash falls back
# reboot -f
...
# install-kernel --commit  # the booted kernel becomes the saved default
```

Build the GRUB disk with `make os-real-grub-disk` and boot it through `rach2/run_from_disk.sh` without `-kernel`. Its boot partition has two kernel slots. The installer rewrites the candidate at `/boot/kernel` in place. The build stages `/boot/fallback`, which installation never modifies. Every installation is a one-boot trial: GRUB selects the candidate once through `next_entry`, while a power cycle after a crash returns to the fallback. A failed candidate therefore costs one reboot. The debug menu entry always starts the fallback and enables the kernel debug shell through `rach2.rescue`.

## Why the design keeps stock GRUB

Planning rejected three alternatives. A custom GRUB module could read KRFFS and keep `/boot/kernel` as an ordinary file, but it would permanently require a GRUB source build and a second implementation of the disk format. An in-house loader would provide the most teaching value and remove GRUB, but real-mode assembly, INT 13h, E820, and BIOS behavior belong to the bare-metal milestone; that loader can reuse these slots. Kexec performs a warm boot and does not install a kernel. The shipped design instead adds a second kernel and a GRUB environment block to the existing FAT boot partition. The guest writes only data sectors through a new root-only raw device. Exact menu ids and the environment's `saved_entry` and `next_entry` values determine each boot. The kernel gained no FAT driver, and GRUB remained unmodified.

Phase 0 began with a boot experiment that changed the design. GRUB's multiboot loader derives extents from the program headers, so zero padding does not change how a candidate boots. `save_env` updates the environment block in place on this FAT32 partition. This GRUB build does not consume `next_entry` automatically; the configuration implements the same scripted consumption used by Ubuntu. The `fallback` variable also cannot resolve an `--id` value. It interprets the string as entry number 0 and retries the broken default. The configuration therefore fixes `fallback=1` and makes menu order part of the contract. Observed boots established both corrections. A later driver run initially contradicted the second observation because a bug supplied every variant with the same configuration. Retained artifacts identified the configuration used for each boot, and the corrected driver reproduced the original results.

## What shipped

* Phase 0, `4f60b46`: replaced the plan's GRUB assumptions with observations from a scripted experiment driver that retains its artifacts.
* Phase 1, `a7a3a7f`: added a mode to every `/dev` entry and made open enforce it through the file systems' nine-bit permission check. Existing entries remain 0666, and fstat now reports each entry's mode instead of a constant. Added `/dev/bootpart` as a bounded, root-only 0600 seekable window over MBR partition 1. The device appears only when the parsed entry has a FAT type, lies within the disk, and does not overlap the KRFFS partition. It accepts whole-sector transfers and uses a kernel bounce buffer because AHCI programs physical DMA addresses. Moved MBR parsing from mount code into the pure, host-tested `sys/block/mbr.c`. Added global `/proc/cmdline` handling for classification, snapshots, stat, listing, and fd origins so programs can identify the GRUB entry that booted. `bootprobe`, through `make bootpart-test`, exercises each path on a fixture disk with known bytes and proves that the device is absent on a single-partition disk.
* Phase 2, `28ff6b2`: implemented `install-kernel` over the pure core in `utils/bootslot.c`, with sector callbacks supplied by its caller. The core validates the BPB before trusting any field, compares both FATs in full, and walks directories recursively with a visited bitmap. Reaching one cluster twice indicates a cross-link and causes refusal. Each reachable regular file's size must fit its chain. The code resolves the three named files and validates each kernel for GRUB: program headers, equal physical and virtual load addresses, exact multiboot flags, and the entry. It requires GRUB's environment header, edits the block in place, and reads back every write. Installation follows the interruption matrix. A host test drives the same core with fixtures from an independent Python FAT writer and callbacks that drop, tear, or cut writes.
* Phase 3, `8eaa057`: added both slots, an initialized environment block, the `loadenv` module, and an exact-id configuration with scripted consumption to the GRUB disk. The build checks every staged chain and fails closed unless all four boot files are disjoint. `run_from_disk.sh` no longer recommends rebuilding the disk because that would discard installed state.
* Phase 4, `4c2caae`: added `make install-test`, which completes the installation loop twice on the booted disk in each invocation. It identifies the slot through `/proc/cmdline`, edits a banner marker in `/src/rach2`, and builds in place. The marker proves which bytes execute; the slot token is only menu text, and a slot hash describes bytes at rest. The test checks refusal while a trial is armed, performs the trial reboot and commit, cold-boots the result, corrupts the sector named by `--where`, requires a marker-free fallback boot, repairs the candidate from inside the guest, and boots every determinate matrix state from host-crafted environment blocks.

## What the reviews caught

The plan received four adversarial reviews before implementation, producing 25 findings, followed by one review per phase. These findings changed the contracts:

* The environment format has no checksum, so a torn block can still load. The configuration compares values with exact menu ids instead of assuming parse failure. The matrix requires every state to boot some bootable kernel, not necessarily the fallback.
* The kernel validator accepted files that GRUB could not boot. It now checks GRUB's loader requirements. It also validates the fallback before the installation's first write because a broken fallback would become the saved default after `saved_entry=fallback`.
* `env_set` retained an armed `next_entry` across a new installation, allowing a retry to arm a partially written slot. One transition now changes the saved entry and disarms the trial together. Every environment write is read back, so a dropped sector fails during installation instead of selecting the wrong kernel later.
* A commit could save an unbooted kernel in two ways. Installing while the candidate runs leaves the trial armed, while an INTERRUPTED installation can leave partial slot bytes with no trial state. Commit now requires the consumed-trial marker, `next_entry=`, whose empty value GRUB saves during consumption. An armed line and an absent line are separate refusals, each with its own diagnostic and host test.
* Disk-chain verification failed open in two places: a pipeline concealed `mshowfat` failures, and two sizes were unchecked. Each command now runs under `sh -e`, and the build requires exact counts for all four records.
* A harness mutation exposed a harness defect. Removing the arming write made the commit-refusal probe accept and commit an unbooted kernel. That kernel then booted as the saved default with its marker, so the identity interval would have passed. The host-side refusal is the binding assertion, and the plan records this observed failure instead of its prediction. The same review removed two more fail-open paths. Success tokens are emitted in pieces so a command echo cannot satisfy them. If a feeder step never arrives, the feeder stops instead of sending later commands into the wrong session. End-to-end negatives check both fixes with a disk whose saved default is the fallback and a configuration without trial consumption.

## Corrected predictions

The phase 4 plan predicted a fallback boot without the marker. The actual mutation instead produced the unrefused commit described above. A driver run also appeared to contradict the first phase 0 observation that `next_entry` was not consumed. Fixing the driver's variant-selection bug restored the original non-consumption result. The milestone 22 record briefly counted 25 utilities and was returned to its historical count of 24. `install-kernel` belongs to this milestone and is counted only by current-contract documents.

## Deferred work

* The design does not close several bare-metal gaps: the AHCI driver's real-hardware reset sequence, the fixed `(hd0,msdos1)` prefix instead of a file-system identifier that survives BIOS disk reordering, reset methods beyond port 0xCF9, and UEFI-only machines. These gaps block a return to bare metal but do not change the installation mechanism, which was designed to survive their later fixes.
* `install-kernel` assumes one administrator and does not lock the device. Its documentation excludes two concurrent installations.
* Installation cannot work through QEMU's `-kernel` developer path because reset reloads the host file. In that mode, `install-kernel` exits nonzero and identifies the missing slot state. The plain `os-real.disk` has no boot partition, so the device does not appear.
* Commit is bound to "a trial boot ran and no install or interruption followed it" through the environment's trial state, not to a digest of the exact slot bytes. Digest binding would require trusted running-state data that the kernel does not retain. Deliberate concurrency remains outside the one-administrator contract.

## Verification

The complete verification run passed after `make distclean` emptied the build tree: `make full`, `verify-style`, `verify-host`, `test` with 61 probes, `tcc-test`, `make-test`, `selfbuild-test`, `bootpart-test`, `install-test`, `ssh-test`, `account-test`, `shutdown-test`, `login-test`, `ping-test`, `crash-test`, `test-disk`, `test-disk-ahci`, `persist-test`, `persist-test-ahci`, `cred-persist-test`, and `test-noentropy`.
