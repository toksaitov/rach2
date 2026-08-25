# Build, verification, and test tools

The scripts under `tools/` support the top-level Make targets and the suite runner. The root Readme documents the public interface. The Makefile supplies prerequisites and arguments that are easy to omit when calling a script directly.

Two scripts are also user entry points. `bootstrap_mac.sh` prepares a Mac, and `metal_accept.sh` reads acceptance data from a running machine over SSH. The other scripts are normally called by Make or by another tracked script.

## Machine setup

* `bootstrap_mac.sh` reads `mac-formulae.txt`, skips installed Homebrew packages, and installs the rest. `--dry-run` prints the commands without installing anything. It accepts only macOS and leaves the final decision to `make doctor`.
* `mac-formulae.txt` lists the Homebrew formulae and casks for a fresh Mac. It includes the i686 cross toolchain, QEMU, FUSE-T, the verification tools, `coreutils` for `gtimeout`, and Expect for the SSH test.
* `preflight.sh` is the build's own cached tool check. `make` and `tests/run` run it at each entry; it stats the recorded tool paths on a cache hit and repeats its compile probes only when the environment changed. A missing tool fails the build, named, before compilation. `--report` prints one line per tool without caching.
* `doctor.sh` is the wider machine-setup report, run directly (the `make doctor` alias is retired). It covers the cross compiler and binutils, QEMU, `make`, a host C compiler, Python, and the verification tools, and reports Docker, OpenSSH tools, Git, Expect, FUSE, and `gtimeout` as optional because only some suites need them.

`doctor.sh` checks that command-line tools run, not merely that their names are present. For FUSE it runs `pkg-config --exists` for `fuse`, then for `fuse-t`.

## `tests/run style`

`check.sh` runs a preflight followed by thirteen numbered checks:

* Build the kernel with warnings treated as errors.
* Run scoped cppcheck with `cppcheck.suppress`.
* Run ShellCheck on project shell scripts.
* Check source conventions such as ASCII text, C comment form, header guards, and marked idle waits.
* Require clang-format on the new-code set and report legacy differences as advice.
* Link a fresh musl program and inspect its ELF ABI.
* Compare the kernel source list and all three guest build descriptions with their host builds.
* Run the mechanical text, comment-width, and banned-prose checks.
* Run the probe scorer's self-test.
* Run the set-id checker's self-test and manifest sweep.
* Build the KRFFS host tools and test deterministic image staging.
* Run the kernel, embedded-GRUB, sealed-network, and compilation-database checker self-tests.
* Check the dev-log index against the finished milestone directories.

The supporting files have narrower jobs:

* `check_sources.sh` compares `C_SRC` in `rach2/sources.mk` with `kernel.c` and the C files below `rach2/sys/`.
* `check_guest.py` checks five contracts: each component's host and guest builds derive from its shared source manifest (`rach2/sources.mk`, `utils/programs.mk`, `ish/sources.mk`), the kernel response file and link line, the utility program-to-source map, the guest file-descriptor and command-size limits, and the three source trees staged under `/src`.
* `textcheck.sh` checks ASCII, prose uses of spaced `--`, Markdown bullet form, and development-phase tags. Its default Markdown scope includes root documents and documents below `rach2/`, `ish/`, `utils/`, `tests/`, `krffs/`, `tools/`, `mk/`, and `vendor/`. It excludes `dev-log/` and all of `ports/doom/`.
* `check_comments.py` checks 80-column comment wrapping in the in-house C, shell, Make, and Python files. It includes the project glue under `ports/doom/` and excludes `ports/doom/src/`.
* `prose_check.py --banned` checks the mechanically prohibited vocabulary in comments, messages, and Markdown. Its default scope includes the root documents and the in-house source roots, including the Doom glue but excluding `vendor/` and `dev-log/`. A path argument checks a file outside that default. `--report` compares watched phrase rates with the pinned upstream corpus without failing the build.
* `prose_baseline.txt` records project phrase counts for `prose_check.py --report`; `prose_upstream.txt` records counts from the pinned upstream tarballs and Doom sources. They are measurements, not pass limits.
* `check_repro.py` stages the same scratch manifest twice with the real KRFFS tools, requires identical image bytes, and checks that `exec-dir` entries were stored in name order.
* `check_devlog.py` requires the dev-log index to name every finished milestone once, in directory order, and to omit plan-only milestones. It accepts `Readme.md` and milestone 6's `results.md` as completion records.
* `cppcheck.suppress` contains the project-wide and path-scoped cppcheck exceptions read by `check.sh`.

## `tests/run host`

`hosttest.sh` discovers every `tests/host/*_hosttest.c`, compiles it with the host C compiler and AddressSanitizer plus UndefinedBehaviorSanitizer, and runs it. A test lists separately compiled sources in repeatable `host-deps:` comments. The script rejects a wrapped dependency declaration, missing dependencies, a compile failure, a nonzero test status, and an empty test set.

Each test is limited to 60 seconds when `timeout` or `gtimeout` is installed. Without either command the suite warns that it cannot stop a hung test, but it still runs.

See [the host-test Readme](../tests/host/Readme.md) for the individual tests and the compatibility headers used when a guest source is compiled natively.

## Artifact and configuration checkers

* `check_kernel.py` validates a kernel ELF, its Multiboot header, its two load segments, its entry point, and its image end. The default profile requires GNU ld symbols; `--tcc` accepts the symbol-free TinyCC layout. `rach2/Makefile` runs it on every kernel link.
* `check_elf.py` validates user executables against both the kernel loader and the project's static-executable policy. `make check-tcc` and `make check-pdpmake` use it.
* `check_link.py` uses `i686-elf-nm` to require that the libraries and startup objects staged for TinyCC leave no required symbol undefined. `vendor/tcc/build_tcc.sh` runs it.
* `check_setid.py` has four related checks: manifest declarations, the resulting image nodes, the two BusyBox applet tables and resolved configs, and the strict and permissive init binaries. `tests/run style` runs its self-test and manifest sweep; `krffs/mkimage.sh` and `krffs/mkdisk.sh` run the image check; `make check-setid` runs the manifest, BusyBox, and init checks.
* `check_diskpolicy.py` requires a kernel to contain exactly one of the read-only and read-write policy banners. `check_recovery.py` does the same for ordinary and recovery disk authority. These checks detect a mis-built artifact; the behavioral targets test actual disk writes.
* `check_grubembed.py` requires `grub-embed.cfg` to contain exactly the ordered configuration calls for partitions 1 and 3. Both GRUB core builders run it.
* `check_sealednet.py` checks that the sealed disk's address, prefix, mask, gateway, and recovery fallback script describe one valid network configuration.

## clangd database

`gen_compile_commands.sh` implements `make compile-commands`. It dry-runs the builds in `rach2/`, `tests/`, `ish/`, and `utils/`, joins wrapped recipe lines, selects the cross-compile commands by their first word, and publishes `compile_commands.json` only after the finished file passes a line-by-line JSON validation and the kernel coverage check. Its `--selftest` exercises the join, selection, escaping, and validation stages on fixtures and runs under `tests/run style`. See [the shared Make documentation](../mk/Readme.md) for the clangd settings.

## QEMU runners and capture checkers

* `harness.py` starts `qemu-system-i386` headless, accepts optional initrd, disk, network, device, and extra QEMU arguments, and executes ordered serial steps. Step forms include `expect=`, `send=`, `wait=`, `tcp=`, `udp=`, `exec=`, and `probe=`. It enforces a run deadline and per-step deadlines. With a harness kernel it can decode the guest process status through QEMU's `isa-debug-exit` device; `--ok` instead requires a serial token.
* `runtests.py` implements the shared boot used by `make test`. It runs the ordered `PROBES` list and scores each probe from the kernel line that returns its transient init process to the debug shell. Probes that need input, networking, a deliberate fault, or a nonzero contract are run by separate Make recipes through `harness.py`. `--selftest` checks the scorer without QEMU.
* `bcast_check.py` reads Ethernet pcap files produced by QEMU. Depending on its options, it checks a broadcast UDP datagram, a packet source address, an ARP ownership reply, or use of a gateway for an off-subnet destination.
* `netconsole_check.py` validates and reassembles Rach2 netconsole frames, including their broadcast address, boot id, sequence, payload length, checksum, and required log text.
* `rawwire_check.py` checks the hop limits and caller-supplied ICMP checksum emitted by the raw-socket probe.
* `mkbootfixture.py` rewrites a small MBR disk into the `/dev/bootpart` fixture and fills its FAT-typed partition with the probe's byte pattern.
* `mkwidefixture.py` creates a sparse 260 GiB disk with known sectors at 5 GiB and 258 GiB for 64-bit disk-offset tests.

## Target-specific test scripts

These scripts receive built artifacts and credentials from their Make targets. They use disposable copies or dedicated fixtures, and each script that starts QEMU installs cleanup for it.

* `account_test.sh` (`make account-test`) checks account creation, authentication, password changes, and database consistency in both os-real and os-simple. The os-real profile also checks deletion across reboots.
* `tcc_test.sh` (`make tcc-test`) compiles and runs programs as the unprivileged account, compares a machine-built `cat` with the staged one, and checks the unsupported TinyCC output forms.
* `make_test.sh` (`make make-test`) checks incremental pdpmake behavior, dependency rebuilding, `.PHONY`, compiler overrides, and timestamps across a reboot.
* `selfbuild_test.sh` (`make selfbuild-test`) builds the kernel twice in clean guest workspaces, requires equal hashes, builds utilities and ISH, extracts and checks the exact kernel, and boots it through the probe suite. It also boots that kernel through GRUB when Docker is available.
* `ssh_test.sh` (`make ssh-test`) checks root key login, unprivileged password login, pty and terminal-size propagation, and rejected authentication attempts. It requires Expect for the password exchange.
* `procfiles_test.sh` (`make procfiles-test`) checks `/proc/mounts` on RAM, page-cache, and disk roots, then checks the `/proc` data used by `ps`, `df`, `free`, `uptime`, `dmesg`, `route`, `arp`, and `netstat`.
* `login_test.sh` (`make login-test`) compares strict and permissive init behavior when `/bin/login` is absent, checks recovery of a stopped console session, and verifies `rach2.rescue`.
* `exitline_test.sh` (`make exitline-test`) requires plain process-exit diagnostics under the debug shell, suppresses them under init, and checks both `runtests.py` scoring markers and the no-token harness path.
* `shutdown_test.sh` (`make shutdown-test`) checks forced and signal-driven halt, poweroff, and reboot paths, root permission, QEMU termination where required, and offline KRFFS state.
* `bootline_test.sh` (`make bootline-test`) checks rejected disk-root command lines, an unmountable disk root, and the rescue exemptions.
* `crash_test.sh` (`make crash-test`) checks the dirty-mount flag after a killed boot, clean halt, poweroff, and attempts to clear the root's overlapping set-id bit.
* `diskpolicy_test.sh` (`make diskpolicy-test`) proves that the ordinary kernel changes a mounted fixture while the write fails under the read-only kernel and leaves every byte unchanged.
* `diskdiag_test.sh` (`make diskdiag-test`) checks read-only disk inspection over unattended SSH, compares selected sectors with the host image, requires the diagnostic disk copy to remain unchanged, and checks the no-disk case.
* `reset_test.sh` (`make reset-test`) runs each of the three restart mechanisms with the earlier mechanisms skipped.
* `metal_test.sh` (`make metal-test`) boots the metal image without console input, checks SSH and file transfer, kills Dropbear, and requires init to start a different server process. It also runs `metal_accept.sh` against the QEMU guest.
* `drill_test.sh` (`make drill-test`) destroys a root partition, enters the recovery image, reflashes the root over SSH, verifies the written extent, and boots the repaired system.
* `menu_test.sh` (`make menu-test`) checks the immutable GRUB stub, the rewritable menu slot, fallback numbering, static-address fallbacks, a truncated menu, and in-place slot writes.
* `grub_test.sh` (`make grub-test`) alternates marked GRUB core images between the two tail slots and proves each replacement by the marker printed by the running core.
* `install_test.sh` (`make install-test`) checks kernel trial, commit, fallback, repair, and determinate interrupted environment states on a GRUB disk.
* `sealed_test.sh` (`make sealed-test`) runs the finished sealed disk through an injected-fault sequence over SSH and a separate control run that must use neither fallback nor recovery.

`metal_accept.sh HOST [PORT]` is the non-destructive scripted half of real-machine acceptance. It reads identity, network, entropy, disk, and device information over root SSH and compares the disk facts for which milestone 24 recorded a baseline. Physical actions and the final disk write remain manual procedures under `dev-log/24-metal/` and `dev-log/25-sealed/`.

## GRUB and ISO builders

* `mkdisk_grub.sh [KERNEL [OUTPUT]]` builds a BIOS-bootable disk with a FAT32 boot partition, a KRFFS root, and an optional FAT32 recovery partition. It stages a fixed-size candidate kernel slot, a fallback kernel, a fixed-size menu slot, and a GRUB environment block, then validates the FAT chains and layout before publishing the image.
* `grub_embed.py` writes GRUB's MBR boot code and core image while preserving the partition table. It patches the core-image pointers and rejects an image that does not fit the post-MBR gap or one tail slot.
* `mkgrubcore.sh MARKER OUTPUT` builds a replacement core image with a serial marker and the same embedded recovery configuration as the disk builder.
* `grub-embed.cfg` is the common embedded configuration. It tries the normal menu on partition 1 and then the recovery menu on partition 3.
* `mkiso.sh` builds `test`, `os-simple`, `os-real`, or `rach2-metal` as a GRUB ISO in Docker. It checks the selected kernel's disk policy before staging it. The top-level `make isos` target builds all four.
* `ubuntu-image.txt` pins the Ubuntu container base by digest for all three GRUB builders. The scripts still install GRUB, mtools, and related packages from Ubuntu's current archive during each build, so the package set is not pinned by this file.

`mkdisk_grub.sh` accepts ten `RACH2_DISK_*` settings. `RACH2_DISK_MANIFEST`, `RACH2_DISK_KRFFS_MB`, and `RACH2_DISK_APPEND` select the root contents, root size, and common kernel arguments. `RACH2_DISK_BOOT_MB` and `RACH2_DISK_CLUSTER` set the FAT boot geometry. `RACH2_DISK_RECOVERY_MB`, `RACH2_DISK_RECOVERY_KERNEL`, and `RACH2_DISK_RECOVERY_IMAGE` add recovery. `RACH2_DISK_DHCP_APPEND` and `RACH2_DISK_STATIC_APPEND` provide the two network command lines. The script fixes the FAT serial and uses `SOURCE_DATE_EPOCH` for repeatable FAT metadata.
