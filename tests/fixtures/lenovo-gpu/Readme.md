# Lenovo GPU fixtures

What Linux reads from the Lenovo's Intel HD 4400 (Haswell ULT, 8086:0a16), kept so that the i915 driver and its host tests can be checked against the real machine without it. The files are as Linux printed them, unedited; a tool's text output is named `.out`, since it is not in-house text and keeps the tool's own characters.

## Use

* `i915_vbt.bin` (6,144 bytes) and `i915_opregion.bin` (8,192 bytes): the video BIOS table and the ACPI OpRegion, read from i915's debugfs files of the same names. `facts.out` is the record they came with: the kernel's command line, `uname` and the DMAR, IOMMU, stolen memory and i915 lines of the kernel's log.
* `debugfs/`: every file of `/sys/kernel/debug/dri/0/` that i915 serves, one file each, a connector's files prefixed with the connector's name and a pipe's with its CRTC's; `debugfs-list.out`, `debugfs-gt0-list.out` and `debugfs-tree.out` name what was read.
* The rest are the outputs of the tools they are named for: `drm_info.out`, `modetest-*.out`, `connectors.out`, `eglinfo-*.out`, `vulkaninfo-summary.out` and `vulkaninfo-full.out.gz` (compressed, since a line of it is the one git writes between the sides of a conflict), `vainfo.out`, `lspci*.out`, `gt-sysfs.out`, `igd-uevent.out`, `devdri.out`, `dmesg-gfx.out`, `packages.out` and `system.out`.

## Updating

Two runs made these files. `i915_vbt.bin`, `i915_opregion.bin` and `facts.out` come from step 11 of the second discovery run, on 2026-11-09, under Debian 13.7 (kernel 6.12.115+deb13.1, with `intel_iommu=on`) on the Lenovo's p5. The rest come from the run of 2026-11-04 under Alpine Linux 3.24.3 (kernel 6.18.58-0-lts), booted over the network. The development log's graphics research has both runs' reports. A new reading replaces a file whole, and its commit message says what was run and where.
