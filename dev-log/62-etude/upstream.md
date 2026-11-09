# Upstream faults of milestone 62

Faults this milestone found in another project's code, one section each, in the format of [`dev-log/upstream.md`](../upstream.md), which indexes them.

## QEMU's WHPX returns a stale XSAVE area size

* Project: QEMU 11.1.0 as Windows' installed build reported it, `v11.1.0-12130-ge470268ff4`, whose exact source revision is missing; the same omission is in the tags v11.1.0, v11.1.1 and v11.1.2.
* Fails: under WHPX, CPUID leaf 0xD subleaf 0 returns in EBX the XSAVE size of a cached XCR0, `0x240`, after the guest has set XCR0 to `0xe7`, whose components end at `0x980`. The WHPX CPUID exit path takes Hyper-V's live `DefaultResultRbx` for subleaves 1 and 2 and omits subleaf 0 (`target/i386/whpx/whpx-all.c:2621` in v11.1.2).
* Found: 2026-10-25, on the Windows testing PC under WHPX, in milestone 62, when the unchanged xsaveprobe rejected the signal frame's size (issue #1161).
* Reproduction: boot the kernel of `0da2f8d327516deaaa26d22232c8004dfe8f76e2` with an initrd whose `/sbin/init` is the probe built with [`qemu-whpx-probe.patch`](qemu-whpx-probe.patch), under `-accel whpx`, as [the report](qemu-whpx-report.md) gives it command by command. A smaller guest that reads XGETBV and CPUID 0xD has not been run.
* Evidence: [`qemu-whpx-report.md`](qemu-whpx-report.md), with the decisive console output before and after the patch; commits `180c456fd`, `8bd3ee69f`, `65ba1e957`, `b5b0f2666`, `7538836b0`. The raw output was under `build/tests/` on the runner and is not kept; the report embeds what a reproduction needs.
* Rach2: [`tools/setup/qemu-whpx/whpx-live-xsave-size.patch`](../../tools/setup/qemu-whpx/whpx-live-xsave-size.patch), SHA-256 `bd8def6f2773d33eaa850ab0aa50296c62b103f464601085376b2754acef2de0`, adds subleaf 0 to the live result in a private QEMU 11.1.2, built by [the recipe](../../tools/setup/qemu-whpx/Readme.md). The unchanged probe passes with it.
* Status: unreported.
* Next: as the report's Upstream report section says: record the Windows build, processor and Hyper-V versions, reproduce on an unpatched build, report the CPUID patch apart from `kernel-irqchip=off`, and check current upstream. "failed to get xsave state: No error" and the missing QOM property `kernel-irqchip` remain open and are not shown to be faults.

## libslirp passes a Winsock socket to GLib as a descriptor

* Project: libslirp 4.9.3, `libslirp-v4.9.3.tar.gz` from GitLab, SHA-256 `ee698ca4ce05217ca7d520c7f0b1b1228fd7d32922dd32d1051c347152588417`.
* Fails: on Windows, `fork_exec` in `src/misc.c` passes a Winsock socket to `g_spawn_async_with_fds`, whose stream arguments are C runtime descriptors; GLib fails to duplicate it, and a guest-forward command closes before reaching its peer.
* Found: by 2026-10-26, the date of the repair, on the Windows testing PC under QEMU 11.1.2 and TCG, in milestone 62 (issue #1147).
* Reproduction: run the real-forward row, `etude_scenario_hosttest.exe --real-forward-only`, with the unpatched `libslirp-0.dll` on `PATH`; the guest-forward assertion fails with a failed descriptor duplication.
* Evidence: [`windows-guest-forward.md`](windows-guest-forward.md), with the digests of the archive, patch, DLL and fixture; commit `6bbc269ac`.
* Rach2: [`tools/setup/qemu-whpx/libslirp-windows-exec.patch`](../../tools/setup/qemu-whpx/libslirp-windows-exec.patch), SHA-256 `2986d1e89564178c61d0a1032bcbe816a223a7d45d1f1c7623b19e93f5e7efaf`, starts the relay with `CreateProcessW`, gives it pipes and moves bytes between them and the socket on two threads ([Local changes](../../tools/setup/qemu-whpx/Readme.md#local-changes)). All nine assertions of the row pass.
* Status: unreported.
* Next: check libslirp's current `src/misc.c` for the same call; separate the patch from Rach2's build and write a reproduction that needs no Rach2 fixture.

## mtools parses an offset with strtol

* Project: mtools 4.0.49, `vendor/mtools/mtools-4.0.49.tar.gz`.
* Fails: an image offset above 2 GiB is refused with ERANGE on i686: mtools keeps offsets in a 64-bit type but parses them with `strtol`, which is 32 bits there (`misc.c:212`).
* Found: 2026-10-24, in milestone 62's collection, when both large FAT32 cases failed on offset 12885950464 in the i686 image helper (issue #1042).
* Reproduction: on i686, give an mtools command an offset of 12885950464, as `machine.large.fat32` does.
* Evidence: `dev-log/62-etude/incidents.md`, "Mtools image offsets above 2 GiB (#1042)"; commit `69f39fb8a`.
* Rach2: `vendor/mtools/offset64.patch` uses `strtoll`.
* Status: unreported.
* Next: check whether current mtools still calls `strtol` there.

## mtools takes a block device's length only when its geometry fails

* Project: mtools 4.0.49.
* Fails: on a Linux block device, mtools sets the volume length from BLKGETSIZE only when HDIO_GETGEO fails (`devices.c:898-906`), so where both requests succeed the requested volume length stays in the device's place and an offset into a large disk is refused with `init: Offset bigger than base image`. Its regular-file path takes the full size.
* Found: 2026-10-24, in milestone 62's collection, run 16, under `windows-final-preflight/physical-first-wsl` (issue #1042).
* Reproduction: `mformat` with an offset of 12885950464 and a 131072-sector volume on a 25300992-sector block device whose kernel answers both requests.
* Evidence: `dev-log/62-etude/incidents.md`, "mtools confused the volume length with its containing block device (#1042)"; commit `8b2a06a65`; the errors were under `build/tests/portable-batch/windows-final-preflight/physical-first-wsl/retry-16/` and are not kept. Source review alone established the cause.
* Rach2: `vendor/mtools/block-geometry.patch` records the BLKGETSIZE result before the geometry query.
* Status: unreported.
* Next: reproduce on Linux with a loop device and the same offset; check current mtools.

## mtools leaves the Linux musl target without block sizing

* Project: mtools 4.0.49.
* Fails: built for a `linux-musl` host, mtools defines `OS_linux_musl`, which its sources do not recognize as Linux, so the Linux block geometry function is left out and the generic one keeps the requested volume length as the device's (the same refusal as above).
* Found: 2026-10-24, in milestone 62's collection, run 24, at `af1af2d49feba2263a313551f14181d3a1efea65` (issue #1101).
* Reproduction: build mtools for `i686-linux-musl` or `x86_64-linux-musl` and run the offset case above on a block device.
* Evidence: `dev-log/62-etude/incidents.md`, "mtools omitted block sizing for its Linux-musl target (#1101)"; commit `d737664b4`. Defining the full Linux platform needs kernel and floppy headers that the musl prefixes lack.
* Rach2: `vendor/mtools/block-geometry.patch` takes the size with BLKGETSIZE64 in the generic path under `OS_linux_musl`.
* Status: unreported.
* Next: check how current mtools' configure names a musl host and whether its Linux path needs the floppy headers; a report would ask for the size request on any Linux.

## TinyCC leaves a register variable unbound without a percent sign

* Project: TinyCC 0.9.27, `vendor/tcc/tcc-0.9.27.tar.bz2`.
* Fails: a local variable declared `register long r10 __asm__("r10")`, as GCC accepts it, is left unbound with no diagnostic: `asm_parse_regvar` takes the name only when it begins with `%` (`i386-asm.c:1503-1511`). The inline assembly allocator then cannot satisfy a syscall's six arguments on x86-64.
* Found: 2026-10-24, in milestone 62's collection, when whole run 16 could not compile `conloadprobe` with TinyCC (issue #1048).
* Reproduction: compile with x86-64 TinyCC a function that binds `r10`, `r8` and `r9` by bare name and passes them with `syscall`; TinyCC reports that an operand cannot satisfy its `r` constraint.
* Evidence: `dev-log/62-etude/incidents.md`, "TinyCC did not bind the x86-64 probe argument registers (#1048)"; commit `f6d9fc0ec`.
* Rach2: `tests/probe.h` names the registers `%r10`, `%r8` and `%r9` under `__TINYC__`. TinyCC is not patched for it.
* Status: unreported.
* Next: check TinyCC's current source, its mob branch, for the same test.

## BusyBox drops the line being typed when a background job ends

* Project: BusyBox 1.36.1.
* Fails: `read_line_input` in `libbb/lineedit.c` gives up the line whenever `poll` in `read_key` returns EINTR, so a SIGCHLD from a finished background job loses what was typed in ash and hush and runs the rest as a new command. This is BusyBox bug 15256.
* Found: 2026-10-23, in milestone 62 by date; the commit names issue #925 and no milestone, and the machine is not recorded.
* Reproduction: in ash, type part of a command while a background job such as `sleep 1 &` ends, then press Enter; the part typed before the end is lost.
* Evidence: `vendor/busybox/patches/0004-shell-keep-line-on-sigchld.patch`; commit `b25cfe4fb`.
* Rach2: the patch is upstream commit `93e0898c663a` as it is.
* Status: fixed upstream in 1.37.0, commit `93e0898c663a`.
* Next: none; drop the patch when BusyBox is updated to 1.37.0 or later.
