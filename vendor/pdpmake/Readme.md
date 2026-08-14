# pdpmake

The make that runs ON the machine. Milestone 20 put a compiler there; this is
what turns a pile of compiler commands into a build.

Version 2.0.4, vendored as the pinned tarball beside this Readme and verified by
sha256 before extraction. Public domain (the Unlicense): the LICENSE file in the
tarball says so, and it is the only dependency in this tree that carries no
conditions at all.

## Why this make

Rach2 needed a make that is small enough to read, portable enough to cross-build
without a fight, and standard enough that what a student learns here is what
they will meet elsewhere. pdpmake is 4297 lines across nine `.c` files and one
header, and its build system is a 30-line POSIX makefile with no configure
script: the cross build here is the musl wrapper, `-static`, and nothing else.
It implements POSIX.1-2024 make, with POSIX.1-2017 selectable and the common
non-POSIX extensions on by default.

It is also the program BusyBox for Windows ships as its `make` applet. Mainline
BusyBox 1.36.1, which this tree vendors, has no make applet at all, so bumping
BusyBox would never have supplied one.

The runners-up, so the choice is not made twice: bmake is 31000 lines behind
autoconf and needs a `sys.mk` set staged before it will start; GNU make is the
make a student meets everywhere else, and it is 36000 lines, GPLv3, autoconf and
gnulib, and needs `--disable-posix-spawn` on this system; scc-make is smaller
than pdpmake and is not a POSIX make in practice (no archive members, no
`.PHONY`, no macro override on the command line, so `make CC=tcc` does nothing).

## Building

`make pdpmake` (or `./build_pdpmake.sh`) verifies the tarball, extracts it,
applies the patches under `patches/`, builds it against the ported musl through
`vendor/busybox/muslgcc`, and judges the result with `tools/check_elf.py`, which
knows what this kernel will actually load. The output is
`build/vendor/pdpmake/make`, staged into os-real as `/bin/make`.

## The one patch

`patches/0001-platform-macros.patch` sets the built-in macros this platform
needs: `CC=tcc` in all three modes and `LDFLAGS=-static`. POSIX leaves the values
of the built-in macros implementation-defined, so this is the port stating its
platform rather than a change of behaviour.

Both matter on their first use. The built-in rules call `$(CC)`, which upstream
is `cc` in the extensions mode, `c99` under POSIX 2017 and `c17` under POSIX
2024; this image carries one compiler, and the other two names are on no path
here, so a strict-POSIX build would fail with "not found". And `LDFLAGS` is
empty upstream, so the built-in link rule would ask tcc for a dynamic
executable, which tcc on this machine refuses by its own pinned patch: this
kernel has no dynamic linker and would load and jump into an image with nothing
relocated.

`build_pdpmake.sh` checks the values after applying the patch, and refuses to
build if any mode still names a compiler this image does not carry: a patch can
apply cleanly to the wrong place.

Neither could be done through `/etc/profile`. That file is read by a login shell
and not by a non-login shell, an SSH command, or a direct exec, so a make
started any other way would fail at the link.

## What it can and cannot do here

It runs recipes through `/bin/sh -c`, which is busybox's ash in os-real, using
`system(3)`. That call reaches a child through musl's `posix_spawn`, which this
tree patches to fork (see `vendor/musl/Readme.md`); before milestone 21 it did
not work at all and no recipe could run.

Parallel builds do not happen, and that is upstream's doing rather than the
port's: `-j` is parsed and passed on in `MAKEFLAGS`, and `main.c` says of it "not
implemented". Recipes run one after another whatever is asked for. Nothing here
tries to change that, and there would be little room if it did: `MAX_PROCS` is 16
system-wide and `MAX_FDS` 16 per process.

This repository's own makefiles are GNU makefiles and pdpmake cannot read them.
Building Rach2 on Rach2 is not what this is for; building a student's program on
Rach2 is.

## Updating from upstream

1. Download the new `pdpmake-X.Y.Z.tgz` from https://frippery.org/make and
   replace the tarball here.
2. Set `VERSION` and `TARBALL_SHA256` in `build_pdpmake.sh`, and `PDPMAKE_TARBALL`
   in the root `Makefile`, which names the tarball the stamp depends on: leave it
   and the build stops looking for the file you just replaced.
3. Rebase the patch under `patches/` onto the new source. `build_pdpmake.sh`
   stops the build when it does not apply, which is the point: a patch that no
   longer applies is a decision to make again, not a line to delete.
4. Rebuild (`make pdpmake`), then run `make make-test`, which is the gate that
   drives this program: a recipe run, a rebuild of only what changed, and a
   timestamp that survives a reboot. `make tcc-test` will not fail on a broken
   make, since it compiles by hand. `build_pdpmake.sh` asserts the patched macro
   values itself, so a patch that applied to the wrong place fails the build
   rather than the student.
