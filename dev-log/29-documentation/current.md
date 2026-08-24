# Milestone 29 current-file checkpoint

This file is a bounded resume map for the one documentation file in progress. It is not evidence. Reopen the named repository location before using a checked fact in an edit. Replace obsolete state instead of appending a diary.

## Position

* File ready to commit: `vendor/tcc/Readme.md`.
* First unchecked file after this commit boundary: `vendor/pdpmake/Readme.md`. Do not start it before the TinyCC commit.
* The 68-line original, 110-line rewrite, and complete product diff have been read whole.
* The only product document in this commit will be `vendor/tcc/Readme.md`.
* Intended commit: the TinyCC integration Readme with its worklist, notes, and checkpoint updates.

## Material claims

* Release pin, digest, tracked inputs, patch, toolchain shims, configure edits, stamp, artifact, and guest staging.
* Cross-build requirements, static host artifact, guest search paths, musl mode, and libtcc1 construction.
* Output-policy refusals, ELF checker contract, missing compiler-runtime helpers, and link checker.
* Guest compile/link behavior, supported and unsupported modes, self-build ownership, update procedure, and exact tests.

## Narrow evidence map

* Build: `vendor/tcc/build_tcc.sh`, archive, output-policy patch, root stamp rule, extracted config, artifacts, and staged paths.
* Toolchain and linking: generated shims, BusyBox `muslgcc`, libtcc Make rules, libgcc extraction, and `tools/check_link.py`.
* Policy and verification: patch hunk, `tools/check_elf.py`, `check-tcc`, `tcc-test`, self-build scripts, and manifests.

## Open checks

* None. The file is ready to commit.

## Checks run for this file

* Five files are tracked: `.gitignore`, this Readme, `build_tcc.sh`, `rach2-output-policy.patch`, and `tcc-0.9.27.tar.bz2`.
* The direct archive SHA-256 is `de23af78fca90ce32dff2dd45b3432b2334740bb9bb7b05bf60fdbfc396ceb9c`, matching the build script. The official Savannah release listing still ends at 0.9.27.
* The stamp depends on the script, policy patch, BusyBox musl wrapper, exact archive, and musl stamp. Its recipe removes the compiler, runtime archive, one required header, and stamp before rebuilding.
* The script replaces the extracted tree, applies the one patch with `--batch --forward`, confirms two policy strings, and creates `rach2-gcc`, `rach2-ar`, and `rach2-strip` shims under the ignored stage directory.
* Configure receives `--cpu=i386`, `--config-musl`, the generated cross prefix, `/lib/tcc`, `/lib/tcc/include:/include`, `/lib`, and `/lib` as guest search paths. The script rewrites `TARGETOS=Linux`, removes `CONFIG_OSX`, and asserts the target and paths in generated config files.
* The resolved `config.mak` confirms `ARCH=i386`, `TARGETOS=Linux`, `CONFIG_musl=yes`, the absolute generated shims, and all guest paths. `config.h` confirms `CONFIG_TCCDIR=/lib/tcc`.
* The build uses the host cross compiler for `i386-libtcc1-usegcc=yes`, then extracts `_udivmoddi4.o`, `_divmoddi4.o`, and `_mulxc3.o` from 32-bit libgcc and adds them to `libtcc1.a`.
* Musl's current archive has undefined references to all three helper symbols in several members. The staged runtime exports all three. `tools/check_link.py` passes with 2,031 definitions and nothing left undefined after its documented program, linker, weak, and unreachable-bounds exclusions.
* The script stages the compiler, `libtcc1.a`, and five TinyCC headers. `check-tcc` requires those seven artifacts, runs the ten-case ELF checker selftest and artifact check, and finds the four policy diagnostics in the shipped binary.
* The current compiler is a 275,400-byte stripped static ELF32 i386 `ET_EXEC`; the runtime archive is 24,034 bytes. `tools/check_elf.py` accepts it. Like Dropbear, it currently has a writable, executable GNU stack.
* The patch refuses `-run`, `-shared`, an executable link without `-static`, and a non-ELF output format. It leaves preprocessing and object compilation available. Rach2 still has no `mprotect`; the loader still ignores non-`PT_LOAD` headers, so the dynamic-output refusal remains required.
* os-real, GRUB-test, and metal-disk profiles stage the compiler as `/bin/tcc` and `/bin/cc`, its runtime and five headers under `/lib/tcc`, musl headers and static link inputs, pdpmake, and the three project source trees.
* `make tcc-test` compiles and runs a static hello, refuses all four unsupported outputs with status 1 and exact diagnostics and no leftover outputs, compiles an object, builds and checks the project cat over multi-block and multi-file input, links two-source ping, and rejects any unimplemented syscall requested by `tcc`.
* `make selfbuild-test` builds the kernel twice from clean guest trees, builds utilities and ish, checks userland smoke behavior, extracts and hashes the kernel, runs the structural checker and full default probe suite on it, optionally boots it through GRUB, and rebuilds staged ish in place as root.
* The rewrite is 110 lines and 6,433 bytes. It states the current pin, build lifecycle, cross choices, staged toolchain, output policy, static link closure, update procedure, and exact verification without the compiler-selection argument and development history.
* The relative checker link and every named repository path resolve. The official TinyCC release directory returns HTTP 200. Every named Make target exists. The banned-term, watched-phrase, direct ASCII, and `git diff --check` checks pass.
* `make tcc` reports the build current. `make check-tcc` passes the seven artifact checks, ten-case ELF selftest, compiler ELF check, and diagnostic-string checks.
* `make tcc-test` passes the compiler, output-policy, object, project `cat`, and two-source `ping` checks.
* `make make-test` passes its incremental build, no-op build, changed-source rebuild, and reboot checks.
* `make selfbuild-test` passes two clean kernel builds with equal hashes, the utilities and ISH builds, the kernel structural check, all 63 default probes, the host-clock comparison, and the in-place ISH rebuild. Its explicit no-Docker branch skipped the optional GRUB boot after Docker Desktop 4.87.0 stalled every bind-mounted container tested on this machine.
* `make verify-style` passes all 13 sections. `make verify-host` passes all 36 host tests.
* `make full` passes and rebuilds the three RAM-disk images.

## Exact next action

Stage the four scoped files, inspect the staged diff and identity, and commit.
