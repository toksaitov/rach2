# pdpmake

pdpmake is the `make` implementation installed on Rach2. The guest uses it with TinyCC to build ordinary programs and the staged Rach2 source trees.

This tree carries pdpmake 2.0.4 as `pdpmake-2.0.4.tgz`. Its SHA-256 digest is:

```
7e19294d54edf360591d76d3b7a0a511864902ac3a75a0b5b11c7b3cae14c13f
```

The upstream `LICENSE` releases the program into the public domain.

## Build

Run the port through the root Makefile:

```
make pdpmake
```

The stamp rule depends on the build script, the exact archive, the platform patch, the BusyBox musl compiler wrapper, and the installed musl libc. It removes the old stamp and staged executable before invoking `vendor/pdpmake/build_pdpmake.sh`. A failed build therefore leaves neither a trusted stamp nor a stale output.

The script performs these steps:

1. Verify the archive digest.
2. Replace the extracted `vendor/pdpmake/pdpmake-2.0.4/` tree.
3. Apply `patches/0001-platform-macros.patch` with no fuzz.
4. Check the patched built-in macros.
5. Build with `vendor/busybox/muslgcc` and `LDFLAGS=-static`.
6. Copy the executable to `build/vendor/pdpmake/make`.
7. Pass the staged executable to `tools/check_elf.py`.

The output is a static ELF32 i386 executable. The os-real, GRUB-test, and metal-disk manifests install it as `/bin/make`.

## Platform macros

Upstream enables its extensions and POSIX.1-2024 support by default. Strict POSIX.1-2024 and POSIX.1-2017 modes select separate built-in macro sets.

`patches/0001-platform-macros.patch` changes the platform-dependent values in all three modes:

```
CC=tcc
LDFLAGS=-static
```

The image has TinyCC as `/bin/tcc` and `/bin/cc`, but it has no `c99` or `c17` command. All three built-in compiler definitions are therefore `CC=tcc`. Rach2 has no dynamic linker, and the staged TinyCC rejects dynamic executable output, so the common `LDFLAGS` definition requests a static link.

The build script checks these values after patching and rejects any remaining `CC=cc`, `CC=c99`, or `CC=c17` definition. Command-line definitions still take precedence. For example, `make CC=tcc` overrides a `CC` definition in a makefile and propagates the override through `MAKEFLAGS`.

## Recipe execution

pdpmake executes each non-empty recipe with `system(3)`. The installed musl implementation runs `/bin/sh -c` through `posix_spawn`. The [musl integration](../musl/Readme.md) patches `posix_spawn` to use the fork operation implemented by Rach2.

The `-j` option is accepted in the extension and POSIX.1-2024 modes and copied into `MAKEFLAGS`, but pdpmake 2.0.4 does not implement parallel jobs. Recipes run serially even when `-j` is present. Rach2 also limits the system to 16 processes and each process to 16 file descriptors.

## Project builds in the guest

The repository's host Makefiles require GNU Make. pdpmake reads the three POSIX guest makefiles instead:

* `rach2/guest.mk` builds the kernel.
* `utils/guest.mk` builds 29 utility programs.
* `ish/guest.mk` builds the shell from five objects.

Each guest makefile includes its component's shared source manifest, the same file the host build reads; `tools/verify/check_guest.py` checks the derivations. The self-build images stage the corresponding trees as `/src/rach2`, `/src/utils`, and `/src/ish`, together with pdpmake, TinyCC, its runtime archive, and the static musl development files. An ordinary user builds from a writable copy. Root may build the staged trees in place.

Use the named guest makefile from its directory:

```
make -f guest.mk
```

## Updating

1. Download `pdpmake-X.Y.Z.tgz` from `https://frippery.org/make` and replace the pinned archive.
2. Update `VERSION` and `TARBALL_SHA256` in `build_pdpmake.sh`.
3. Update `PDPMAKE_TARBALL` in the root `Makefile`.
4. Rebase `patches/0001-platform-macros.patch`. Keep its three `CC` changes and common static-link definition.
5. Run `make pdpmake`, `make make-test`, and `make selfbuild-test`.

A change to any prerequisite named by the stamp rule invalidates the stamp. If the patch no longer applies exactly or no longer produces the required macro definitions, the build stops.

## Verification

`make check-pdpmake` requires a non-empty staged executable and checks its ELF layout and static link. The build script separately checks the patched built-in macros.

`make make-test` boots the os-real image twice. It checks a command-line compiler override, a clean two-source build, no-op rebuilds, one-source and shared-header rebuilds, `.PHONY`, one project utility, executable behavior, and timestamps preserved across a reboot.

`make selfbuild-test` builds the kernel twice from clean guest trees and requires identical hashes. It also builds the utilities and ISH, runs 18 userland checks, extracts and checks the exact guest-built kernel, runs the default guest probes on that kernel, optionally boots it through GRUB, and rebuilds `/src/ish` in place as root.

Run `python3 tools/verify/check_guest.py` after changing a host or guest source list, a guest makefile, or the staged `/src` trees.
