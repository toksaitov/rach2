# Milestone 29 current-file checkpoint

This file is a bounded resume map for the one documentation file in progress. It is not evidence. Reopen the named repository location before using a checked fact in an edit. Replace obsolete state instead of appending a diary.

## Position

* File ready to commit: `vendor/pdpmake/Readme.md`.
* First unchecked file after this commit boundary: `ports/doom/Readme.md`. Do not start it before the pdpmake commit.
* The 96-line original, 90-line rewrite, and complete product diff have been read whole.
* The only product document in this commit will be `vendor/pdpmake/Readme.md`.
* Intended commit: the pdpmake integration Readme with its worklist, notes, and checkpoint updates.

## Material claims

* Release pin, digest, tracked inputs, patch, stamp, build options, artifact, and guest staging.
* Built-in modes and macros, recipe execution, command-line overrides, incremental behavior, and `-j` handling.
* Guest makefiles, self-build role, system limits, update procedure, and exact verification.

## Narrow evidence map

* Build: `vendor/pdpmake/build_pdpmake.sh`, archive, platform-macros patch, root stamp rule, artifact, and extracted source.
* Behavior: upstream macro tables, option parsing, job execution, `system(3)` path, and the three `guest.mk` files.
* Staging and verification: manifests, `check-pdpmake`, `make-test`, `selfbuild-test`, and `tools/check_guest.py`.

## Open checks

* None. The file is ready to commit.

## Checks run for this file

* Five files are tracked: `.gitignore`, this Readme, `build_pdpmake.sh`, the platform-macros patch, and `pdpmake-2.0.4.tgz`.
* The direct archive SHA-256 is `7e19294d54edf360591d76d3b7a0a511864902ac3a75a0b5b11c7b3cae14c13f`, matching the build script.
* The stamp depends on the script, exact patch, BusyBox musl wrapper, exact archive, and musl stamp. Its recipe removes the artifact and stamp before rebuilding.
* The script replaces the extracted tree, applies the named patch with zero fuzz, asserts every compiler macro and `LDFLAGS=-static`, builds with the musl wrapper, stages the result, and runs the kernel ELF checker.
* The current artifact is a 126,568-byte static ELF32 i386 executable with debug information. `tools/check_elf.py` accepts it.
* os-real, GRUB-test, and metal-disk manifests stage it as `/bin/make`, with TinyCC, its static sysroot, and the three project source trees under `/src`.
* Upstream enables extensions and POSIX 2024 by default, supports strict POSIX 2017 and 2024 modes, and selects a distinct built-in macro block for each mode. The patch sets `CC=tcc` in all three blocks and the common `LDFLAGS=-static`.
* `-j` is accepted outside strict POSIX 2017, validated as digits, and copied into `MAKEFLAGS`, but upstream labels job execution unimplemented. Recipes still run serially.
* Recipes call `system(3)`. The installed musl implementation invokes `/bin/sh -c` through `posix_spawn`, whose project patch replaces the unsupported clone path with fork.
* Command-line macro definitions override makefile definitions and enter `MAKEFLAGS`; `make-test` uses a deliberately invalid makefile `CC` to prove `make CC=tcc` overrides it.
* `rach2/guest.mk`, `utils/guest.mk`, and `ish/guest.mk` are the POSIX guest build descriptions. The adjacent host Makefiles remain GNU make input. `tools/check_guest.py` passes with 88 kernel sources, 29 utility programs, five ish objects, and three staged source trees.
* `make-test` proves the compiler override, initial build and execution, no-op rebuilds, one-source and shared-header dependency rebuilds, `.PHONY`, a self-built `cat`, and timestamp behavior across a reboot.
* `selfbuild-test` builds and hashes two clean kernels, builds all utilities and ish, runs 18 userland checks, extracts and checks the guest-built kernel, runs the default probe suite on it, optionally boots it with GRUB, and rebuilds staged ish in place as root.
* The implementation confirms the documented 16-process system limit and 16-descriptor process limit. The update procedure names every version, digest, tarball, patch, build, and focused test input that must change or run.
* The rewrite is 90 lines and 4,954 bytes. It replaces the selection history and false self-build denial with the current integration contract.
* The relative link and every named repository path resolve. The named Make targets exist. The banned-term, watched-phrase, direct ASCII, and `git diff --check` checks pass.
* `make pdpmake` reports the build current. `make check-pdpmake` accepts the non-empty static ELF32 artifact.
* `make make-test` passes the initial build and execution, no-op and dependency rebuilds, compiler override, `.PHONY`, project `cat`, and both reboot checks.
* `make selfbuild-test` passes two clean kernel builds with equal hashes, the utilities and ish builds, 18 userland checks, the kernel structural check, all 63 default probes, the host-clock comparison, and the in-place ish rebuild. Its no-Docker branch skipped the optional GRUB boot.
* `make full` passes and rebuilds the three RAM-disk images. `make verify-style` passes all 13 sections. `make verify-host` passes all 36 host tests.

## Exact next action

Stage the four scoped files, inspect the staged diff and identity, and commit.
