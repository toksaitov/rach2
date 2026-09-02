# Build and verification tools

The scripts under `tools/` are grouped by role: `setup/` prepares and reports on a machine, `verify/` implements the style suite, `inspect/` validates built artifacts, `image/` constructs disks and ISOs, and `dev/` holds developer utilities. The root Readme documents the public interface. The Makefile supplies prerequisites and arguments that are easy to omit when calling a script directly. The test runner, suites, capture checkers, and fixtures live under [`tests/system/`](../tests/system/Readme.md).

## Machine setup

To build and boot Rach2 a machine needs GNU Make, a host C compiler, the
`i686-elf` freestanding cross toolchain, QEMU, `ssh-keygen`, e2fsprogs 1.43
or later, and the POSIX text utilities. The build finds `mke2fs`, `debugfs`
and `e2fsck` on the path, under the Homebrew keg, which is never linked, or
under `/sbin` and `/usr/sbin`, where Debian keeps them. To pass `tests/run
style` it also needs cppcheck, ShellCheck, and clang-format. The six suites that build a GRUB disk need mtools; a plain
`make` builds the three RAM images and reaches none of it. Nothing is fetched
during a build: the five vendored projects are pinned tarballs already in the
tree.

Install those, then run `sh tools/setup/doctor.sh`. It reports every tool in
four tiers and prints the install command for whatever is missing.

### macOS

Xcode's command line tools supply `make` and `cc`; Homebrew supplies the rest.
Install Homebrew first if the machine has none, and run the `shellenv` line its
installer prints, because it does not put `brew` on the path itself.

    xcode-select --install
    /bin/bash -c "$(curl -fsSL \
        https://raw.githubusercontent.com/Homebrew/install/HEAD/install.sh)"
    eval "$(/opt/homebrew/bin/brew shellenv)"

    brew install i686-elf-gcc i686-elf-binutils qemu mtools e2fsprogs \
                 cppcheck shellcheck clang-format

`ssh-keygen` ships with macOS. `xorriso` builds the optional ISOs
(`make isos`), and `pkg-config` with the `fuse-t` cask lets `make -C krffs
mount` mount an image on the host; neither is needed to build or test.

### Debian and Ubuntu

The archive has everything except the cross toolchain.

    sudo apt install build-essential curl qemu-system-x86 mtools e2fsprogs \
                     openssh-client cppcheck shellcheck clang-format

`xorriso` builds the optional ISOs, and `pkg-config` with `libfuse-dev` lets
`make -C krffs mount` mount an image; neither is needed to build or test.

No Debian package provides an `i686-elf` toolchain, so build one. It installs
under your home directory and touches nothing the distribution owns.

    sudo apt install curl bison flex texinfo libgmp-dev libmpfr-dev libmpc-dev

    export PREFIX="$HOME/opt/cross" TARGET=i686-elf
    export PATH="$PREFIX/bin:$PATH"

    curl -LO https://ftp.gnu.org/gnu/binutils/binutils-2.43.tar.xz
    tar xf binutils-2.43.tar.xz
    mkdir build-binutils && cd build-binutils
    ../binutils-2.43/configure --target=$TARGET --prefix="$PREFIX" \
        --with-sysroot --disable-nls --disable-werror
    make && make install && cd ..

    curl -LO https://ftp.gnu.org/gnu/gcc/gcc-14.2.0/gcc-14.2.0.tar.xz
    tar xf gcc-14.2.0.tar.xz
    mkdir build-gcc && cd build-gcc
    ../gcc-14.2.0/configure --target=$TARGET --prefix="$PREFIX" \
        --disable-nls --enable-languages=c --without-headers
    make all-gcc all-target-libgcc
    make install-gcc install-target-libgcc && cd ..

Build `all-target-libgcc` and not the compiler alone. The kernel is linked
`-nostdlib` and uses no libgcc, but the ported musl, BusyBox, and Dropbear
link it through the `muslgcc` wrappers, so a toolchain without it builds the
kernel and then fails in the userland. Put `$PREFIX/bin` on the path of every
shell that builds Rach2.

The target prefix is not negotiable: `vendor/musl/build_libc.sh` configures
with `CROSS_COMPILE=i686-elf-` and both `muslgcc` wrappers call
`i686-elf-gcc` by name. A toolchain installed elsewhere under that same
prefix works; one built for another target does not.

### The two checkers

* `setup/preflight.sh` is the build's own cached tool check. `make` and
  `tests/run` run it at each entry; it stats the recorded tool paths on a
  cache hit and repeats its compile probes only when the environment changed.
  A missing tool fails the build, named, before compilation. `--report`
  prints one line per tool without caching.
* `setup/e2fsprogs_path.sh` is the one place the three e2fsprogs programs are
  looked for off the path, sourced by `preflight.sh`, `doctor.sh` and
  `image/mkext2.sh`.
* `setup/doctor.sh` is the wider machine report, in four tiers: what a build
  and run need, which is everything `preflight.sh` requires plus QEMU; the
  three tools `tests/run style` refuses to start without; the four mtools
  commands the GRUB disk suites need; and the optional `xorriso`, Docker (the
  `vendor/grub` regeneration procedure only), Git, and FUSE. It checks that
  tools run rather than that their names are present, probing `awk`, `sed`,
  and `ssh-keygen` by doing the work the build does with them, since none of
  the three accepts `--version` everywhere. For FUSE it runs `pkg-config
  --exists` for `fuse`, then for `fuse-t`.

## `tests/run style`

`verify/check.sh` runs a preflight followed by fourteen numbered checks:

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
* Run the ELF, configuration, and compilation-database checker self-tests.
* Check the dev-log index against the finished milestone directories.
* Check `tests/registry` against `tests/system/suites.mk`: every suite has a target and every target is a suite.

The supporting files under `verify/` have narrower jobs:

* `check_sources.sh` compares `C_SRC` in `rach2/sources.mk` with `kernel.c` and the C files below `rach2/sys/`.
* `guestcheck.c` checks five contracts: each component's host and guest builds derive from its shared source manifest (`rach2/sources.mk`, `utils/programs.mk`, `ish/sources.mk`), the kernel response file and link line, the utility program-to-source map, the guest file-descriptor and command-size limits, and the three source trees staged under `/src`.
* `asciiscan.c` reports every line holding a byte outside tab, newline, carriage return, and the printable range 0x20 through 0x7e, which is the set the text checks enforce. `textcheck.sh` and `check.sh` compile it on first use and run it over the files they check.
* `sha256.c` prints the SHA-256 digest of each file it is given. The vendor builds refuse a tarball whose digest is wrong and the suites compare staged artifacts by digest, so this is plain C compiled on first use rather than a dependency on `sha256sum`.
* `textcheck.sh` checks ASCII, prose uses of spaced `--`, Markdown bullet form, and development-phase tags. Its default Markdown scope includes root documents and documents below `rach2/`, `ish/`, `utils/`, `tests/`, `krffs/`, `tools/`, `mk/`, and `vendor/`. It excludes `dev-log/` and all of `ports/doom/`.
* `prosecheck.c` holds the prose rules. `columns` checks 80-column comment wrapping in the in-house C, shell, Make, and Python files, including the project glue under `ports/doom/` and excluding `ports/doom/src/`. `banned` checks the mechanically prohibited vocabulary in comments, messages, and Markdown; its default scope is the root documents and the in-house source roots, including the Doom glue but excluding `vendor/` and `dev-log/`, and a path argument checks a file outside that default. `count` reports the word and phrase totals `prose_report.sh` tabulates, and `devlog` requires the dev-log index to name every finished milestone once, in directory order, and to omit plan-only milestones (it accepts `Readme.md` and milestone 6's `results.md` as completion records).
* `prose_report.sh` compares watched phrase rates with the pinned upstream corpus without failing the build; `--baseline` and `--fixture` compare with the committed records or rewrite them.
* `prose_baseline.txt` records project phrase counts for `prose_report.sh`; `prose_upstream.txt` records counts from the pinned upstream tarballs and Doom sources. They are measurements, not pass limits.
* `check_repro.sh` stages the same scratch manifest twice with the real KRFFS tools, requires identical image bytes, and checks that `exec-dir` and `tree` entries were stored in name order with the fixed staged instant, reading the node inventory through `setidcheck --nodes`.
* `cppcheck.suppress` contains the project-wide and path-scoped cppcheck exceptions read by `check.sh`.

## Artifact and configuration checkers

The inspectors under `inspect/` validate built artifacts; the validation stamps in the root Makefile re-run them when a checker or its artifact changes.

* `elfcheck.c` is the ELF validator. Without options it judges user executables against both the kernel loader and the project's static-executable policy; `make check-tcc` and `make check-pdpmake` use that form. `--kernel` validates a kernel ELF, its Multiboot header, its two load segments, its entry point, and its image end, requiring GNU ld symbols unless `--tcc` accepts the symbol-free TinyCC layout; `rach2/Makefile` runs it on every kernel link. `--selftest` runs both profiles' refusal batteries.
* `check_link.sh` uses `i686-elf-nm` to require that the libraries and startup objects staged for TinyCC leave no required symbol undefined. `vendor/tcc/build_tcc.sh` runs it.
* `setidcheck.c` has four related checks: manifest declarations, the resulting image nodes, the two BusyBox applet tables and resolved configs, and the strict and permissive init binaries. Its declarations live in `setid.allow` beside it. `tests/run style` runs its self-test and manifest sweep; `krffs/mkimage.sh` and `krffs/mkdisk.sh` run the image check; `make check-setid` runs the manifest, BusyBox, and init checks. `--free` and `--nodes` read a region's free bytes and node inventory for the suites. `--ext2 FILE --manifest PATH` inventories an ext2 image through a small read-only reader of its own, sharing nothing with the kernel's driver, which accepts exactly the profile `mkext2.sh` writes and refuses any other feature, any block outside the volume or inside its tables, any malformed directory entry and any inode it cannot place, so that an image the guard cannot read is an image the build rejects; it lists every node reached from the root with a second name for one inode as a node of its own, which is how a hard link to a set-id file meets the policy as an undeclared path. `--nodes --ext2 FILE` lists what it read. The self-test builds its ext2 fixtures with the host's `mke2fs` and `debugfs` and judges the reader against them.
* `binfind.c` answers whether a file contains a byte string, and can replace one in place. The TinyCC policy check reads the shipped compiler's four refusal diagnostics out of the binary this way, and the disk-policy and recovery checks and the menu suite use the same search. It replaces `strings` piped to `grep`, which put another host tool in the required set for one question.
* `check_diskpolicy.sh` requires a kernel to contain exactly one of the read-only and read-write policy banners. `check_recovery.sh` does the same for ordinary and recovery disk authority. These checks detect a mis-built artifact; the behavioral suites test actual disk writes.
* `cfgcheck.c` checks the two configuration files a sealed disk depends on. `grubembed` requires `image/grub-embed.cfg` to contain exactly the ordered configuration calls for partitions 1 and 3, which both GRUB core builders run; `sealednet` checks that the sealed disk's address, prefix, mask, gateway, and recovery fallback script describe one valid network configuration.

## clangd database

`dev/gen_compile_commands.sh` builds the clangd database. Run it directly. It dry-runs the builds in `rach2/`, `tests/`, `ish/`, and `utils/`, joins wrapped recipe lines, selects the cross-compile commands by their first word, and publishes `compile_commands.json` only after the finished file passes a line-by-line JSON validation and the kernel coverage check. Its `--selftest` exercises the join, selection, escaping, and validation stages on fixtures and runs under `tests/run style`. See [the shared Make documentation](../mk/Readme.md) for the clangd settings.

## GRUB and ISO builders

* `image/mkext2.sh MANIFEST OUTPUT [SIZE_MB]` builds an ext2 image from a manifest, 1 GiB and sparse by default: `stageimage.krffs --tree` materialises the manifest, the host's `mke2fs` formats the directory on the one profile the kernel's driver writes, revision 1, 4 KiB blocks, 128-byte inodes, `sparse_super`, `large_file` and `filetype` with every other feature named and off, the superblock is read back and refused outside that profile, `debugfs` sets each node's owner, mode and set-id bits from the staged list, `e2fsck -fn` must find the volume clean, and `setidcheck --ext2` reads it with the guard's own reader. The image is renamed into place only after every step passes. `make os-real-ext2` builds `build/images/os-real.ext2` this way.
* `image/mkdisk_grub.sh [KERNEL [OUTPUT]]` builds a BIOS-bootable disk natively: mtools stage the FAT32 boot partition, `mkmbr.c` writes the partition table, and the pinned GRUB boot code embeds from [`vendor/grub/`](../vendor/grub/Readme.md). The disk has a KRFFS root and an optional FAT32 recovery partition. It stages a fixed-size candidate kernel slot, a fallback kernel, a fixed-size menu slot, and a GRUB environment block, then validates the FAT chains and layout before publishing the image.
* `image/grubembed.c` writes GRUB's MBR boot code and core image while preserving the partition table. It patches the core-image pointers and rejects an image that does not fit the post-MBR gap or one tail slot.
* `image/mkstick.sh ISO OUT MKEXFATFIXTURE MKMBR STICKCHECK` builds the stick image `build/images/rach2-stick.img` (`make rach2-stick-image`): the metal hybrid ISO kept whole with its boot code, padded to the next 1 MiB boundary, followed by a 16 MiB exFAT volume built with its `PartitionOffset` at that sector, and two MBR entries written into the table bytes, the ISO's from sector 1 of type `0x17` and the volume's of type `0x07`. The firmware boots it from a USB socket and the kernel mounts the volume as `/dev/usb0p2`.
* `image/stickcheck.c IMAGE VOLUME-START VOLUME-SECTORS` judges a stick image whole, linked with the kernel's own `mbr.c` so the partition rule is the kernel's: boot code and the signature in sector 0, exactly two partitions with none passed over, the volume as the second at its start and length on a 1 MiB boundary, the ISO's entry ending before it, an exFAT boot sector whose `PartitionOffset` and `VolumeLength` are the entry's, and an image that ends where the volume ends. `mkexfatfixture --check` walks the volume afterwards.
* `image/mkgpt.c DISK START:SIZE:KIND[@INDEX] ...` writes a GUID partition table, a protective MBR, the primary and backup headers and arrays with their checksums, for the test suites' sticks; `@INDEX` places an entry away from the array's start.
* `image/mkgrubcore.sh {A|B} OUTPUT` verifies and hands out one of the two pinned marked core images the replacement suites install.
* `image/grub-embed.cfg` is the common embedded configuration. It tries the normal menu on partition 1 and then the recovery menu on partition 3.
* `image/mkiso.sh` builds `test`, `os-simple`, `os-real`, `rach2-metal`, or `rach2-recovery` as a GRUB ISO with native `xorriso` and the pinned El Torito and hybrid images under [`vendor/grub/`](../vendor/grub/Readme.md). It checks the selected kernel's disk policy before staging it. The top-level `make isos` target builds all four. `rach2-recovery` is the recovery kernel and its RAM image on the same hybrid image, so a stick boots a machine into `reflash` without its disk; its boot line takes an address from DHCP.
* `image/ubuntu-image.txt` pins the Ubuntu container base by digest for the maintainer regeneration of the pinned GRUB artifacts (`vendor/grub/regen_grub.sh`); no build or suite uses a container.

`mkdisk_grub.sh` accepts ten `RACH2_DISK_*` settings. `RACH2_DISK_MANIFEST`, `RACH2_DISK_KRFFS_MB`, and `RACH2_DISK_APPEND` select the root contents, root size, and common kernel arguments. `RACH2_DISK_BOOT_MB` and `RACH2_DISK_CLUSTER` set the FAT boot geometry. `RACH2_DISK_RECOVERY_MB`, `RACH2_DISK_RECOVERY_KERNEL`, and `RACH2_DISK_RECOVERY_IMAGE` add recovery. `RACH2_DISK_DHCP_APPEND` and `RACH2_DISK_STATIC_APPEND` provide the two network command lines. The script fixes the FAT serial and uses `SOURCE_DATE_EPOCH` for repeatable FAT metadata.
