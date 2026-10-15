# GRUB boot images

This directory has eight GRUB boot images, the BIOS and UEFI boot code that the disk, ISO and stick builders write. They are committed as built, with their provenance in [`manifest.txt`](manifest.txt), because GRUB's tools do not build on every host Rach2 supports, macOS among them. A build reads these files and runs no GRUB tool and no container; only [`regen_grub.sh`](regen_grub.sh) makes them, and only by the procedure under Updating.

## Interfaces

Seven images boot by BIOS (GRUB's `i386-pc` platform) and one by UEFI:

* `bios-boot.img`: GRUB's MBR boot sector. `tools/image/grubembed.c` copies its first 440 bytes into a disk's MBR and keeps the disk identifier and partition table.
* `disk-core.img`: the core image of an MBR disk, which `grubembed` writes into the gap after the MBR. Its prefix is `(hd0,msdos1)/boot/grub` and its embedded configuration is `tools/image/grub-embed.cfg`.
* `disk-core-a.img` and `disk-core-b.img`: `disk-core.img` with three lines before the embedded configuration, which turn on the serial console and print `GRUBCORE=A` or `GRUBCORE=B`. `tools/image/mkgrubcore.sh` copies one out for the `grub-test` image, and `tests/run grub` installs them in turn and reads the marker to see which core ran.
* `gpt-core.img`: the core image of a GPT disk, with the same embedded configuration, prefix `(hd0,gpt1)/boot/grub`, and the modules for a GUID partition table and `multiboot2`. `grubembed` writes it at the start of the BIOS boot partition, and `bios-boot.img` in the protective MBR loads it.
* `eltorito.img`: the El Torito boot image of an ISO, GRUB's `i386-pc-eltorito` format, with prefix `/boot/grub` and no embedded configuration.
* `boot-hybrid.img`: GRUB's hybrid MBR, which `xorriso` writes into an ISO so that it also boots from a USB device.
* `bootx64.efi`: GRUB's `x86_64-efi` image, installed as `\EFI\BOOT\BOOTX64.EFI` on a GPT disk's EFI system partition, in an ISO's EFI boot image and on the install stick. Its prefix is `/boot/grub` on the device the firmware loaded it from, and its embedded configuration is `tools/image/grub-efi-embed.cfg`.

`manifest.txt` lists each image's modules, under `disk-modules`, `gpt-modules`, `iso-modules` and `efi-modules`. What the two embedded configurations do, and how the builders lay out a disk or an ISO, is in [`tools/Readme.md`](../../tools/Readme.md). The images are written by `tools/image/mkdisk_grub.sh`, `mkiso.sh`, `mkinstallstick.sh` and `mkgrubcore.sh`, and by the `sealed` and `uefi` suites, which install `gpt-core.img` on a running machine.

[`verify_grub.sh`](verify_grub.sh) checks images against the manifest:

```
sh vendor/grub/verify_grub.sh [NAME[=PATH] ...]
```

With no argument it checks every image in this directory. `NAME=PATH` checks the file at `PATH` against the manifest's entry for `NAME`. Every consumer copies the images it needs into its own directory and checks the copies, so the bytes checked are the bytes written. Every run also checks `tools/image/grub-embed.cfg` and `tools/image/grub-efi-embed.cfg` against the manifest's `embed-cfg` and `efi-embed-cfg` digests, since the images embed those files as they were when the images were made; a change to either fails the build until the images are made again. The script exits 1 on any mismatch. The root `Makefile` lists the images, the manifest and the script among the prerequisites of the disks and ISOs, so a new image rebuilds them.

## Sources and license

The images are made by `grub-mkimage` and copied from the Ubuntu packages `grub-pc-bin`, `grub-efi-amd64-bin` and `grub-common`, in the Ubuntu container whose digest [`tools/image/ubuntu-image.txt`](../../tools/image/ubuntu-image.txt) has. `manifest.txt` records the container, the archive snapshot, the packages' versions and `.deb` digests, every package the install added, the `grub-mkimage` version, the module lists, the embedded configurations' digests, and each image's size and SHA-256. GRUB is under the GNU General Public License, version 3 or later.

## Updating

The images change only through `regen_grub.sh`, a maintainer procedure that needs Docker:

```
sh vendor/grub/regen_grub.sh
```

The script runs the container for `linux/amd64`, installs `ca-certificates` from the container's default archive, then points apt at one dated state of the Ubuntu snapshot archive, `SNAPSHOT` in the script or `RACH2_GRUB_SNAPSHOT` in the environment, and installs the three GRUB packages from it. It first runs `cfgcheck grubembed` and `cfgcheck grubefi` over the two embedded configurations and stops when either fails. It makes all eight images and the manifest twice, in `build/grub-regen/round1` and `round2`, requires the two rounds to be equal byte for byte, and copies the first into this directory. The container digest and the snapshot identifier fix every input but the `ca-certificates` bootstrap and what it installs, which put trust anchors in the container and no byte in an image; the manifest records their versions as installed.

A new set of images is chosen on purpose and reviewed as a vendored source update is:

1. To move to newer packages, raise `SNAPSHOT` to a later timestamp the snapshot service has. To change the container, change `tools/image/ubuntu-image.txt` and say why in the commit. To change an embedded configuration, edit `tools/image/grub-embed.cfg` or `grub-efi-embed.cfg`.
2. Run `regen_grub.sh`.
3. Read the diff of `manifest.txt`: the package versions and `.deb` digests, the packages the install added, the `grub-mkimage` version, the module lists and the configuration digests. Every change in it must be one that was intended.
4. Run `sh vendor/grub/verify_grub.sh`.
5. Run `tests/run grub menu install sealed drill installer uefi`, and boot an ISO by BIOS, which no suite does. All must pass before the images and the manifest are committed together.

## Limits

* `bootx64.efi` is not signed, so a machine boots it only with Secure Boot turned off.
* `disk-core.img`, its two marked copies and `eltorito.img` have only the `multiboot` loader; `gpt-core.img` and `bootx64.efi` have `multiboot2` as well.

## Tests

* `tests/run grub`, in the core tier, boots an MBR disk through `disk-core.img` and replaces its core with `disk-core-a.img` and `disk-core-b.img`.
* `tests/run menu`, `install` and `drill` boot MBR disks through `disk-core.img`.
* `tests/run sealed` boots the sealed GPT disk, and installs `gpt-core.img` from the running machine with `bootctl grub` and boots through it.
* `tests/run installer` converts an MBR disk to boot by UEFI as well and requires its `BOOTX64.EFI` to equal `bootx64.efi`.
* `tests/run uefi` runs an x86-64 machine for either kernel. It boots through `bootx64.efi` with OVMF the installed GPT disk, the install stick and the recovery ISO as a CD, and it installs `gpt-core.img` with `bootctl grub` and boots through it by SeaBIOS.
