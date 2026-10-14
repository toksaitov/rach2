# pdpmake

pdpmake is the `make` of the Rach2 system, a public-domain POSIX make. With TinyCC it builds programs on the machine, and it builds the kernel, the utilities and ISH there from the `guest.mk` makefiles of `rach2/`, `utils/` and `ish/`, since the host makefiles need GNU make. It is built statically against the ported [musl](../musl/Readme.md) and staged as `/bin/make`; [make(1)](../../man/man1/make.1) is its manual page on this system.

## Sources and license

The source is the release tarball from [frippery.org](https://frippery.org/make/) in this directory. [`build_pdpmake.sh`](build_pdpmake.sh) has its version in `VERSION` and its SHA-256 in `TARBALL_SHA256`, and refuses a tarball whose checksum differs; the root `Makefile` gives the same file as `PDPMAKE_TARBALL`. Its `LICENSE` puts the program in the public domain.

## Local changes

One patch, [`patches/0001-platform-macros.patch`](patches/0001-platform-macros.patch), changes the built-in macros in `rules.c`. POSIX leaves their values to the implementation, and upstream's name compilers the image does not have: `CC=c99` in POSIX.1-2017 mode, `CC=c17` in POSIX.1-2024 mode and `CC=cc` with the extensions. The patch sets all three to `CC=tcc`, and the common definition to `LDFLAGS=-static`, since the staged TinyCC links only static programs. A macro given on the command line or in a makefile still takes precedence.

The values are compiled in, not set in `/etc/profile`, so that a make run from a non-login shell, an SSH command or a direct `exec` has them too.

pdpmake runs each recipe with `system(3)`, which in the ported musl uses `posix_spawn`; the musl port patches that to use `fork`, as [its Readme](../musl/Readme.md) says.

## Building

`make pdpmake` builds for the architecture `ARCH` selects, i686 by default; `make pdpmake ARCH=x86_64` builds for x86-64. The root `make` builds it too, through the same stamp. The script takes the architecture from `RACH2_ARCH` through `mk/arch.sh`. An i686 build extracts into this directory and stages `build/vendor/pdpmake/make`; an x86-64 build extracts under `build/x86_64/vendor-work/pdpmake/` and stages `build/x86_64/vendor/pdpmake/make`. Each has its own stamp.

`build_pdpmake.sh`:

1. Checks the tarball's SHA-256, deletes the extracted tree and extracts it again.
2. Applies the platform patch with `patch -p1 -F 0 --forward`, and stops when the patch is missing or does not apply.
3. Requires `CC=tcc` and `LDFLAGS=-static` in the patched `rules.c`, and refuses one that still has `CC=cc`, `CC=c99` or `CC=c17`.
4. Builds with upstream's makefile, with `CC` set to the `muslgcc` wrapper and `LDFLAGS=-static`.
5. Copies the program to the stage, unstripped, and runs [`elfcheck`](../../tools/inspect/elfcheck.c) on it with the architecture's `--arch`.

The result is a static executable, ELF32 i386 or ELF64 x86-64. The stamp depends on the script, the tarball, the patch, `muslgcc`, `mk/arch.sh` and the musl stamp. `make check-pdpmake` requires the staged program to be nonempty and runs `elfcheck` on it again. The [manifests](../../krffs/manifests/) stage it at mode 0755 as `/bin/make`, with TinyCC and the `/src` trees.

## Updating

1. Put the new release tarball in this directory and delete the old one.
2. Change `VERSION` and `TARBALL_SHA256` in `build_pdpmake.sh`, and `PDPMAKE_TARBALL` in the root `Makefile`.
3. Rebase the platform patch, keeping its three `CC` definitions and the `LDFLAGS` one; the build refuses a patch that applies with fuzz.
4. Rebuild, and run the checks under Tests.

## Tests

`make check-pdpmake` runs with every image build that stages the program. `tests/run style` runs [`guestcheck`](../../tools/verify/guestcheck.c), which checks each `guest.mk` against the host build it mirrors. The suites, described in [suites.md](../../tests/system/suites.md):

* `tests/run make` builds a program of two sources and a shared header on os-real, as the `test` account, and requires a second build to do nothing, a touched source or header to rebuild only what depends on it, a command-line `CC` to override the makefile's, and the result to stand after a reboot.
* `tests/run selfbuild` builds the kernel, the utilities and ISH on os-real from the staged `/src` trees, and `selfbuild-ext2` does the same on an ext2 root.
