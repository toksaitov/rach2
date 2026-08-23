# Milestone 29 notes

This is the persistent record for information needed after a compaction or between documentation files. It is not evidence for a factual claim. Recheck every item against the repository before using it in an edit.

## Cross-document contradictions

* `CLAUDE.md` and `Readme.md` described all build output as living under `build/`. The root Makefile and `mk/build.mk` make that true for in-house components; vendored projects also build extracted trees and musl's install under `vendor/`. `CLAUDE.md` is corrected. Recheck the root Readme in its own pass.
* `CLAUDE.md` and `Readme.md` said the build does not run QEMU without distinguishing image builds from test targets. Image build targets do not boot QEMU. The run scripts and the test harnesses do. `CLAUDE.md` is corrected. Recheck the root Readme in its own pass.
* Both root-document contradictions above are corrected in `Readme.md`.

## Non-document defects

* The comment at the top of `rach2/guest.mk` says the response file avoids putting 87 objects on one command line. `OBJS` now has 88 entries, confirmed by `tools/check_guest.py`. This milestone does not edit source comments.
* `mk/build.mk` says a Docker `grub-mkrescue` call in `rach2/Makefile` uses `$(abspath ...)`. No `abspath` call remains in the in-house Makefiles or tools. This milestone does not edit Makefile comments.
* The `.clangd` comment says the host KRFFS tools receive host-compiler flags from the compilation database. `tools/gen_compile_commands.sh` excludes `krffs/`, so that database supplies no such flags. This milestone does not edit configuration comments.
* `make compile-commands` exits successfully but produces invalid JSON with the current Homebrew i686 toolchain. A wrapped recipe continuation matches the `i686-elf-*gcc` filter through its libgcc path, and `json_escape` does not escape the leading tab. The target's kernel coverage check does not parse the completed JSON. `mk/Readme.md` states the limitation; the generator needs separate code work.
* `tools/textcheck.sh` does not include `ports/doom/` among its roots, so it does not check the project-owned `ports/doom/Readme.md`. This milestone checks that file explicitly and does not edit the script.
* The default scope of `tools/prose_check.py --banned` excludes `vendor/`, so it does not check the six project-owned vendor integration Readmes or `vendor/busybox/patches/Readme.md`. This milestone checks those files explicitly and does not edit the script.
* A comment in `tools/mkdisk_grub.sh` calls `UBUNTU_IMAGE` a floating tag. The value is a digest read from `tools/ubuntu-image.txt`. This milestone does not edit script comments.
* The header comment in `tools/hosttest.sh` says the native tests run in milliseconds and implies that every exercised source also links into the kernel. `pwauth_hosttest.c` deliberately takes tens of seconds, and the suite also exercises utility sources and test-only hardware models. This milestone does not edit script comments.
* Comments in the root `Makefile` and `krffs/manifests/test.manifest` say `make test` checks every probe's success token. `tools/runtests.py` instead runs its explicit 63-program list, scores each program's debug-shell return status, and checks the guest clock separately. The image stages 105 guest programs, including helpers and programs reserved for dedicated or manual runs. This milestone does not edit Makefile or manifest comments.
* The header of `rach2/run.sh` says omitting the image boots the kernel into its debug shell. `k_main` refuses an unmounted root unless rescue mode was requested, so the working kernel-only command is `RESCUE=1 ./run.sh`. A direct harness boot confirmed both paths. This milestone does not edit script comments.
* `rach2/qemu_devices.sh` says `NET=off` disables networking, but it emits no QEMU network option in that mode. QEMU 11.1.0 creates its default e1000 device and SLIRP backend. The kernel does not support e1000, so guest networking is unavailable, but the virtual NIC is not removed. This milestone does not edit the script.
* The `mmap2` comments in `rach2/sys/syscall.c` say the syscall supports only anonymous private mappings. The implementation requires `MAP_ANONYMOUS` and rejects `MAP_FIXED`, but it neither requires `MAP_PRIVATE` nor rejects `MAP_SHARED` or unknown flags. Every accepted mapping receives private COW fork behavior. This milestone does not edit source comments or code.
* Comments in `rach2/sys/mm/vm.h` and `tests/musl/vmamusl.c` say four VMAs exist before a program runs. `elf_setup_vmas` inserts the loadable segments and one stack VMA; it records the empty heap only in `heap_start` and `brk`. The first non-empty `brk` growth inserts the heap VMA. This milestone does not edit source or test comments.

## Facts for subsystem passes

* `block_probe` tries ATA by default. With `rach2.disk=ahci`, it tries AHCI first and falls back to ATA. Disk-root discovery scans the four MBR entries and mounts the first entry whose first sector is a valid KRFFS root; it does not depend on a host device path.
* The current vendor patch count is one for musl, two for BusyBox, none for Dropbear, one for TinyCC, and one for pdpmake. Each build script verifies its pinned tarball by SHA-256.
* `tools/check_guest.py` reports 88 kernel sources, 29 utility programs, five ish objects, and three staged `/src` trees. `tools/selfbuild_test.sh` checks 18 userland smoke tokens.
* `make test` checks each probe's declared outcome; it is not accurate to say that every special probe simply exits zero.
* `make os-real-disk` creates a partitioned disk-image file. `rach2/run.sh` can attach that image through ATA or AHCI and mount it as the persistent root.
* The fresh-Mac manifest uses the Homebrew cask `fuse-t` and the `coreutils` formula, which provides `gtimeout`. The direct commands are `brew install --cask fuse-t` and `brew install coreutils`.
* `tools/Readme.md` names every tracked file under `tools/`. Its checker descriptions were verified with the self-tests for comments, prose, the dev-log, kernel ELF, user ELF, set-id, embedded GRUB, sealed networking, and probe scoring. `sh tools/bootstrap_mac.sh --dry-run` and `make doctor` also passed.
* `tests/host/Readme.md` names all 36 discovered host tests, the three compatibility headers, and the FAT32 fixture builder. `make verify-host` passed all 36 tests with the documented compiler, dependency, logging, and watchdog contract.
* `tests/Readme.md` accounts for all 70 root test programs and all 35 musl programs. The default `make test` list has 47 root programs and 16 musl programs; a fresh run passed all 63 status checks and the host-clock comparison.
* `rach2/Readme.md` now owns only the kernel-level build, boot, option, run-script, source-map, debug-shell, hardware, and limit contracts. `make check-sources`, `tools/check_guest.py`, and `tools/check_kernel.py` passed; direct QEMU harness runs confirmed rescue mode and the no-root refusal.
* `rach2/sys/mm/Readme.md` now describes the implemented physical-frame, page-table, VMA, demand-paging, COW, usercopy, and page-cache contracts. It includes the AHCI backing path, module-layout check, fixed limits, allocation-failure behavior, and the actual default and host tests. `make verify-host` passed all 36 tests.

## Decisions and deferred questions

None recorded yet.
