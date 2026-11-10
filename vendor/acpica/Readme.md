# ACPICA

ACPICA is Intel's reference implementation of ACPI: the AML interpreter, the namespace, the tables, the hardware's fixed and general-purpose events and the global lock, in portable C. The kernel carries its core on the PC, with an OS layer of its own in [`rach2/sys/acpi/`](../../rach2/sys/acpi/Readme.md). Rach2 compiles the core from one pinned release, with one header patched, and builds it with the kernel on the host and on Rach2 itself.

## Sources and license

The source is `acpica-unix-20260930.tar.gz`, the Unix release tarball published with the release `20260930` of [github.com/open-acpica/acpica](https://github.com/open-acpica/acpica/releases/tag/20260930), in this directory. [`build_acpica.sh`](build_acpica.sh) has its version in `VERSION` and its SHA-256, `aa18901b92e30749be0edc3081c8d550c61fce4fa37546fc6a65d367a4ae71a5`, in `TARBALL_SHA256`, and refuses a tarball whose checksum differs. Each file is Intel's, under the BSD-3-Clause license or version 2 of the GNU General Public License only, at the recipient's choice (`SPDX-License-Identifier: BSD-3-Clause OR GPL-2.0-only`). Rach2 takes it under the BSD-3-Clause license. The extracted core keeps both license files, `LICENSE.BSD-3-Clause` and `LICENSE.GPL-2.0-only`, and every file its own notice, and the images stage them with the sources.

## Local changes

The tarball is not edited, and only the core is used: the nine directories of `source/components/` that the kernel compiles (`dispatcher`, `events`, `executer`, `hardware`, `namespace`, `parser`, `resources`, `tables` and `utilities`) and `source/include/`. The debugger, the disassembler and the tools are left out, and `resources/rsdump.c`, which serves the debugger, is extracted but not compiled.

[`rach2-acenv.patch`](rach2-acenv.patch) is the one change. `include/platform/acenv.h` chooses the host's header from the compiler's macros; the patch makes it include `acrach2.h` when `__RACH2__` is defined, ahead of the Linux test, since TinyCC defines `__linux__` on Rach2 and the Linux branch would include `aclinux.h` and the C library's headers. `acrach2.h` is the kernel's, in `rach2/sys/acpi/`, with the C library functions and the global lock ACPICA takes from its host.

## Building

The kernel's build runs the script; `make -C rach2 acpica` runs it alone. `build_acpica.sh`:

1. Checks the tarball's SHA-256.
2. Extracts it, with the time of the extraction on every file, and moves the core into `build/vendor/acpica/` flattened, so that a file is `dispatcher/dsargs.c` and the headers are under `include/`. An x86-64 build extracts into `build/x86_64/vendor/acpica/`, where the stager's path map looks.
3. Applies the patch with `patch -p1 -F 0` and checks that `acenv.h` names `__RACH2__`.
4. Packs the core, its files in name order, as `acpica.tar.bz2` in the same directory.
5. Writes `.stamp`, the SHA-256 of the tarball, the patch and the script, and renames the directory into place.

When the stamp already names the same three and the archive is there, the script only touches the stamp, so a fresh checkout's newer tarball recompiles nothing. Two kernels built at once, such as the ordinary and the recovery kernel, take turns: the script holds `acpica.lock`, beside the tree, from its check of the stamp until the tree is in place, so neither removes a tree the other compiles, and the second finds the first's stamp. A lock whose holder has died is taken over.

`rach2/sys/acpi/sources.mk` lists the files under `$(ACPICA)`, which the host `Makefile` sets to the extracted core and `guest.mk` to `vendor/acpica`. The host build compiles them into `build/rach2/acpica/` with flags of their own: the architecture's, `-ansi -Wall -Werror`, `-D__RACH2__` and the include directories of the core and of `sys/acpi`.

The images that stage the kernel's sources (`os-real`, `os-real-gnu`, `grub-test` and `rach2-metal-disk`) stage the archive as `/src/rach2/vendor/acpica.tar.bz2`, not the tree. The tree takes 4 MiB and the archive 0.45 MiB, and os-real's RAM root, a module that must end well below 64 MiB, where the DMA pool and i686's frame table are carved (the root Makefile gives its sizes), has room for the archive alone. `guest.mk` unpacks it into `vendor/acpica` before it compiles anything, with `tar -xmoj`, so the files take the time of unpacking and the user's ownership.

## Updating

1. Put the new release's `acpica-unix-VERSION.tar.gz` in this directory and delete the old one.
2. Change `VERSION` and `TARBALL_SHA256` in `build_acpica.sh`, and the tarball's name in the stamp's prerequisites in `rach2/Makefile`.
3. Rebase the patch so that it applies with no fuzz to `include/platform/acenv.h`.
4. Compare the new release's file lists with `rach2/sys/acpi/sources.mk`, and its `actypes.h`, `acpiosxf.h` and `acpixf.h` with the OS layer, for a new or changed `AcpiOs` interface.
5. Build both architectures, run `tools/verify/guestcheck`, which compares `guest.mk`'s objects with the host's and bounds the include depth, and run the checks under Tests.

## Tests

`tests/run acpi` boots each architecture on QEMU's `pc` and `q35` machines and reads ACPICA's record, `acpi: acpica: N objects, 0 errors`, and the power button's event after `system_powerdown`. `tests/run selfbuild` builds the kernel, ACPICA with it, on Rach2 from the staged archive.
