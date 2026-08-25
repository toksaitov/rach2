# GRUB boot artifacts

This directory pins the six GRUB boot artifacts the disk and ISO builders consume, so a developer build needs neither a GRUB toolchain nor Docker. GRUB cannot be built on macOS, which is how a Docker container came to own these bytes; pinning the generated artifacts, with their provenance recorded, keeps the container out of every build and every suite. `manifest.txt` records the container digest, the archive state, each package version with its `.deb` SHA-256, the tool version, the module lists, the embedded configuration's digest, and each artifact's size and SHA-256.

The artifacts, all for BIOS `i386-pc` boot:

* `bios-boot.img`: GRUB's MBR boot sector template. `grub_embed.py` copies its first 446 bytes into a disk's MBR, preserving the partition table.
* `disk-core.img`: the disk core image, with the embedded configuration `tools/image/grub-embed.cfg` and the modules to read the FAT boot partition and start a Multiboot kernel. `grub_embed.py` writes it into the post-MBR gap.
* `disk-core-a.img` and `disk-core-b.img`: the same core prefixed with a serial marker (`GRUBCORE=A`, `GRUBCORE=B`). The GRUB replacement suites install them alternately and prove a swap by the marker the running core prints. `tools/image/mkgrubcore.sh` verifies and hands one out.
* `eltorito.img`: the El Torito boot image for ISOs, with the `iso9660` module, built in GRUB's `i386-pc-eltorito` output format.
* `boot-hybrid.img`: GRUB's hybrid MBR template, placed by `xorriso` so an ISO also boots when written to a USB device.

`verify_grub.sh [ARTIFACT ...]` compares size and SHA-256 against the manifest; every consumer runs it before using an artifact, so a modified file fails the build instead of reaching a disk.

## Regeneration

`regen_grub.sh` is a maintainer procedure and needs Docker; nothing in the ordinary build or the suites runs it. It generates every artifact twice from clean directories in the pinned Ubuntu container, requires the rounds byte-identical, and rewrites the artifacts and manifest together. Regenerating is how a GRUB upgrade is chosen: the change is reviewed like any vendored-source update, and `tests/run grub`, `tests/run menu`, `tests/run install`, `tests/run sealed`, and `tests/run drill`, plus an ISO boot, must pass on the result before it lands.

This directory is the one exception to the source-first vendoring rule, recorded in `CLAUDE.md`: the artifacts are generated output, pinned because the generating toolchain does not exist on every supported host. The `.deb` digests and archive state in the manifest name the exact inputs a future regeneration must fetch, through the Ubuntu snapshot service, to reproduce them.
