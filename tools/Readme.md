# Build and verification tools

The scripts under `tools/` are grouped by role: `setup/` prepares and reports on a machine, `verify/` implements the style suite, `inspect/` validates built artifacts, `image/` constructs disks and ISOs, and `dev/` holds developer utilities. The root Readme documents the public interface. The Makefile supplies prerequisites and arguments that are easy to omit when calling a script directly. The test runner, suites, capture checkers, and fixtures live under [`tests/system/`](../tests/system/Readme.md).

## Machine setup

* `setup/bootstrap_mac.sh` reads `mac-formulae.txt`, skips installed Homebrew packages, and installs the rest. `--dry-run` prints the commands without installing anything. It accepts only macOS and leaves the final decision to `doctor.sh`.
* `setup/mac-formulae.txt` lists the Homebrew formulae and casks for a fresh Mac. It includes the i686 cross toolchain, QEMU, FUSE-T, the verification tools, `coreutils` for `gtimeout`, and Expect for the SSH test.
* `setup/preflight.sh` is the build's own cached tool check. `make` and `tests/run` run it at each entry; it stats the recorded tool paths on a cache hit and repeats its compile probes only when the environment changed. A missing tool fails the build, named, before compilation. `--report` prints one line per tool without caching.
* `setup/doctor.sh` is the wider machine-setup report, run directly (the `make doctor` alias is retired). It covers the cross compiler and binutils, QEMU, `make`, a host C compiler, Python, and the verification tools, and reports Docker, OpenSSH tools, Git, Expect, FUSE, and `gtimeout` as optional because only some suites need them.

`doctor.sh` checks that command-line tools run, not merely that their names are present. For FUSE it runs `pkg-config --exists` for `fuse`, then for `fuse-t`.

## `tests/run style`

`verify/check.sh` runs a preflight followed by thirteen numbered checks:

* Build the kernel with warnings treated as errors.
* Run scoped cppcheck with `verify/cppcheck.suppress`.
* Run ShellCheck on project shell scripts.
* Check source conventions such as ASCII text, C comment form, header guards, and marked idle waits.
* Require clang-format on the new-code set and report legacy differences as advice.
* Link a fresh musl program and inspect its ELF ABI.
* Check the shared source manifests and the guest build descriptions.
* Run the mechanical text, comment-width, and banned-prose checks.
* Run the probe scorer's self-test.
* Run the set-id checker's self-test and manifest sweep.
* Build the KRFFS host tools and test deterministic image staging.
* Run the kernel, embedded-GRUB, sealed-network, and compilation-database checker self-tests.
* Check the dev-log index against the finished milestone directories.

The supporting files under `verify/` have narrower jobs:

* `check_sources.sh` compares `C_SRC` in `rach2/sources.mk` with `kernel.c` and the C files below `rach2/sys/`.
* `check_guest.py` checks five contracts: each component's host and guest builds derive from its shared source manifest (`rach2/sources.mk`, `utils/programs.mk`, `ish/sources.mk`), the kernel response file and link line, the utility program-to-source map, the guest file-descriptor and command-size limits, and the three source trees staged under `/src`.
* `textcheck.sh` checks ASCII, prose uses of spaced `--`, Markdown bullet form, and development-phase tags. Its default Markdown scope includes root documents and documents below `rach2/`, `ish/`, `utils/`, `tests/`, `krffs/`, `tools/`, `mk/`, and `vendor/`. It excludes `dev-log/` and all of `ports/doom/`.
* `check_comments.py` checks 80-column comment wrapping in the in-house C, shell, Make, and Python files. It includes the project glue under `ports/doom/` and excludes `ports/doom/src/`.
* `prose_check.py --banned` checks the mechanically prohibited vocabulary in comments, messages, and Markdown. Its default scope includes the root documents and the in-house source roots, including the Doom glue but excluding `vendor/` and `dev-log/`. A path argument checks a file outside that default. `--report` compares watched phrase rates with the pinned upstream corpus without failing the build.
* `prose_baseline.txt` records project phrase counts for `prose_check.py --report`; `prose_upstream.txt` records counts from the pinned upstream tarballs and Doom sources. They are measurements, not pass limits.
* `check_repro.py` stages the same scratch manifest twice with the real KRFFS tools, requires identical image bytes, and checks that `exec-dir` entries were stored in name order.
* `check_devlog.py` requires the dev-log index to name every finished milestone once, in directory order, and to omit plan-only milestones. It accepts `Readme.md` and milestone 6's `results.md` as completion records.
* `cppcheck.suppress` contains the project-wide and path-scoped cppcheck exceptions read by `check.sh`.

## Artifact and configuration checkers

The inspectors under `inspect/` validate built artifacts; the validation stamps in the root Makefile re-run them when a checker or its artifact changes.

* `check_kernel.py` validates a kernel ELF, its Multiboot header, its two load segments, its entry point, and its image end. The default profile requires GNU ld symbols; `--tcc` accepts the symbol-free TinyCC layout. `rach2/Makefile` runs it on every kernel link.
* `check_elf.py` validates user executables against both the kernel loader and the project's static-executable policy. `make check-tcc` and `make check-pdpmake` use it.
* `check_link.py` uses `i686-elf-nm` to require that the libraries and startup objects staged for TinyCC leave no required symbol undefined. `vendor/tcc/build_tcc.sh` runs it.
* `check_setid.py` has four related checks: manifest declarations, the resulting image nodes, the two BusyBox applet tables and resolved configs, and the strict and permissive init binaries. `tests/run style` runs its self-test and manifest sweep; `krffs/mkimage.sh` and `krffs/mkdisk.sh` run the image check; `make check-setid` runs the manifest, BusyBox, and init checks.
* `check_diskpolicy.py` requires a kernel to contain exactly one of the read-only and read-write policy banners. `check_recovery.py` does the same for ordinary and recovery disk authority. These checks detect a mis-built artifact; the behavioral suites test actual disk writes.
* `check_grubembed.py` requires `image/grub-embed.cfg` to contain exactly the ordered configuration calls for partitions 1 and 3. Both GRUB core builders run it.
* `check_sealednet.py` checks that the sealed disk's address, prefix, mask, gateway, and recovery fallback script describe one valid network configuration.

## clangd database

`dev/gen_compile_commands.sh` builds the clangd database, run directly (the `make compile-commands` alias is retired). It dry-runs the builds in `rach2/`, `tests/`, `ish/`, and `utils/`, joins wrapped recipe lines, selects the cross-compile commands by their first word, and publishes `compile_commands.json` only after the finished file passes a line-by-line JSON validation and the kernel coverage check. Its `--selftest` exercises the join, selection, escaping, and validation stages on fixtures and runs under `tests/run style`. See [the shared Make documentation](../mk/Readme.md) for the clangd settings.

## GRUB and ISO builders

* `image/mkdisk_grub.sh [KERNEL [OUTPUT]]` builds a BIOS-bootable disk with a FAT32 boot partition, a KRFFS root, and an optional FAT32 recovery partition. It stages a fixed-size candidate kernel slot, a fallback kernel, a fixed-size menu slot, and a GRUB environment block, then validates the FAT chains and layout before publishing the image.
* `image/grub_embed.py` writes GRUB's MBR boot code and core image while preserving the partition table. It patches the core-image pointers and rejects an image that does not fit the post-MBR gap or one tail slot.
* `image/mkgrubcore.sh MARKER OUTPUT` builds a replacement core image with a serial marker and the same embedded recovery configuration as the disk builder.
* `image/grub-embed.cfg` is the common embedded configuration. It tries the normal menu on partition 1 and then the recovery menu on partition 3.
* `image/mkiso.sh` builds `test`, `os-simple`, `os-real`, or `rach2-metal` as a GRUB ISO in Docker. It checks the selected kernel's disk policy before staging it. The top-level `make isos` target builds all four.
* `image/ubuntu-image.txt` pins the Ubuntu container base by digest for all three GRUB builders. The scripts still install GRUB, mtools, and related packages from Ubuntu's current archive during each build, so the package set is not pinned by this file.

`mkdisk_grub.sh` accepts ten `RACH2_DISK_*` settings. `RACH2_DISK_MANIFEST`, `RACH2_DISK_KRFFS_MB`, and `RACH2_DISK_APPEND` select the root contents, root size, and common kernel arguments. `RACH2_DISK_BOOT_MB` and `RACH2_DISK_CLUSTER` set the FAT boot geometry. `RACH2_DISK_RECOVERY_MB`, `RACH2_DISK_RECOVERY_KERNEL`, and `RACH2_DISK_RECOVERY_IMAGE` add recovery. `RACH2_DISK_DHCP_APPEND` and `RACH2_DISK_STATIC_APPEND` provide the two network command lines. The script fixes the FAT serial and uses `SOURCE_DATE_EPOCH` for repeatable FAT metadata.
