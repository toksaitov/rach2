# Shared host Make rules

The files under `mk/` define the common GNU Make settings for in-house host builds. Each component includes only the files it needs. Vendored projects use their own build systems.

* `arch.mk` names the architecture a build is for: `ARCH`, `i686` unless `make ARCH=x86_64` names the other, and refuses any third value. From it come `ARCH_SUFFIX`, empty for i686 and `/x86_64` for x86-64; `ARCH_BUILD`, the build tree, `build` or `build/x86_64`; `ARCH_VENDOR`, where a vendored port's extracted tree, stamp and musl's install go, `vendor` or `build/x86_64/vendor-work`; `ARCH_MUSL`, musl's install directory; `CROSS_PREFIX`; `ARCH_CFLAGS`, `-m32` or `-m64`; `ARCH_LDFLAGS`, which aligns an x86-64 program's segments to 4 KiB pages; and `ARCH_USER_TEXT`, the address the freestanding programs put their text at, where TinyCC puts a program's on that architecture: `0x08048000` or `0x400000`. The root Makefile and `toolchain.mk` include it.
* `arch.sh` is the same table for the shell, sourced by the vendored ports' build scripts, the musl wrapper and the image scripts, which the root Makefile runs with `RACH2_ARCH` set to `ARCH`. It also takes `ARCH` and a command-line `ARCH=` out of what it passes on, since musl, BusyBox and TinyCC have make variables of that name with other meanings.
* `toolchain.mk` selects the cross compiler of the architecture in force, `i686-elf-gcc` or `x86_64-elf-gcc`, and derives the matching linker, archiver, strip tool, and `objcopy`. Command-line `CC` and `CROSS_*` settings override the defaults. Only `make clean` works without a cross compiler.
* `deps.mk` defines `DEPFLAGS := -MMD -MP`. These options write header dependencies and tolerate a header removed since the preceding build.
* `build.mk` defaults `TOP` to `..`, `COMPONENT` to the current directory name, and `BUILD` to `$(TOP)/build/$(COMPONENT)`, or `$(TOP)/build/x86_64/$(COMPONENT)` for a component built for x86-64. A host component, which includes no toolchain, builds into `build/` for either architecture. A Makefile can override each value before including it.
* `rules.mk` supplies a `$(BUILD)/%.o` C compile rule and includes the resulting `.d` files. `ish/` uses it directly. Other components have compile rules for their own source and link layouts.
* `fingerprint.mk` records the component's resolved configuration (compiler path and identity, flags) in `$(BUILD)/.config`. The file is a target of its own, an explicit step the build runs rather than work done while Make parses, and its recipe rewrites the file only when the text has changed and then purges what the change invalidates; a dry run writes and removes nothing. Objects and programs depend on it, so a flag edit or a reinstalled compiler rebuilds the affected tree automatically.
* `host.mk` is the one rule text every host program is compiled with: `HOSTCC`, `HOSTCFLAGS` (`-O2 -Wall -Wextra -Werror`) and `HOST_LINK`, which links a rule's C prerequisites into its target. The checkers under `tools/`, the runners and fixture makers under `tests/`, and the image tools are all built through it, so a flag added there reaches every program that decides a verdict.
* `images.mk` holds the products that are not RAM images: the metal and recovery ISOs, the stick image and the install stick, and the three tools the stick scripts run. The install stick includes the sealed disk, its manifest and the separate ext2 root image, with 256 MiB added for metadata and backups of the two 64 MiB partitions. Its rule depends on both images and this makefile, so a layout change rebuilds the stick. The root Makefile includes it before the test include, and `tools/verify/check.sh` refuses a rule under `tests/` that makes a product.
* `gnu.mk` builds the GNU layer of milestone 54, which is not part of `all` and builds on a Linux host only. `make gnu-toolchain` runs `vendor/gnu/build_toolchain.sh` for the i686-linux-gnu cross toolchain under `build/vendor/gnu/`, again whenever the script, `fetch.sh` or the toolchain's own lines of `sources.txt` change: `toolchain.pins` beside it has those lines and is rewritten only when they differ, so that pinning a port does not rebuild the compilers. `make gnu-ports` runs `vendor/gnu/build_port.sh --all`, which builds every recipe into an empty `build/vendor/gnu/root`, again whenever the toolchain, the script, a recipe, `fetch.sh` or `sources.txt` changes.
* `gnu-extra.mk` has the targets of the GNU layer's programs that have build scripts of their own, each behind a stamp under `build/vendor/gnu/` and run from nothing again whenever its script or a list of sources it reads changes: `make gnu-native` runs `vendor/gnu/native/build_native.sh` for GCC and binutils to run on Rach2, `make gnu-gdb` runs `build_gdb.sh` there for GDB and gdbserver, `make gnu-js` runs `vendor/gnu/spidermonkey/build_js.sh` for the SpiderMonkey shell, and `make gnu-chromium` runs `vendor/gnu/chromium/fetch_chromium.sh` for Debian's chromium-headless-shell and its libraries. The three that compile are rebuilt when the cross toolchain is, and all four build on a Linux host only and are not part of `all`.
* `gnu-image.mk` has `make os-real-gnu-ext2`, the GNU system's root: `tools/image/mkext2.sh --overlay vendor/gnu/install_root.sh` over `krffs/manifests/os-real-gnu.manifest`, a 4096 MiB ext2 volume at `build/images/os-real-gnu.ext2`. Its prerequisites are those of os-real's ext2 image without Dropbear, the overlay, `fetch.sh` and `sources-extra.txt`, which pins the CA bundle the overlay installs, and the stamps of the ports, the native compiler, GDB, the shell and the browser, so it is built again when any of those is and builds on a Linux host only; it is not part of `all`. The root Makefile includes it after the two above, whose stamps it names, and before `tests/system/suites.mk`, whose `run-gnu` boots it.

## Output directories

In-house builds put objects, dependency files, and binaries under the repository's ignored `build/` directory. The layout follows component names: for example, the kernel is `build/rach2/kernel`, its objects follow the paths below `build/rach2/`, host KRFFS tools are under `build/krffs/`, and Doom is under `build/ports/doom/`. The top-level build puts images under `build/images/` and stages image inputs from BusyBox, Dropbear, TinyCC, pdpmake and e2fsprogs under `build/vendor/`.

An x86-64 build (`make ARCH=x86_64`) has the same layout under `build/x86_64/`: `build/x86_64/rach2/kernel`, `build/x86_64/utils/`, `build/x86_64/images/`, `build/x86_64/vendor/`, and the vendored ports' extracted trees and musl's install under `build/x86_64/vendor-work/`. The host tools under `build/tools/` and `build/krffs/` and the rendered manual under `build/man/` serve both architectures. The manifests name their host files by the i686 paths; for x86-64 the root Makefile exports `RACH2_PATH_MAP`, and the stager, `krffs/manifest_deps.sh` and the set-id guard read `build/...` from `build/x86_64/...` and musl's install from the x86-64 one.

`BUILD` stays relative to a component's working directory. Both `make -C utils` and the top-level sub-make therefore resolve `../build/utils` to the same directory. This also keeps absolute repository paths, including paths with spaces, out of Make's target names.

Vendored projects are the exception to the output convention. For i686 their build scripts extract working trees under `vendor/`, and musl installs under `vendor/musl/install/`. `make clean` preserves these vendor builds and the staged files under `build/vendor/`, and an x86-64 `make clean` those under `build/x86_64/vendor/` and `build/x86_64/vendor-work/`; `make distclean` removes them all.

## Header dependencies

An object compile with `$(DEPFLAGS)` writes a `.d` file beside its `.o`. Makefiles include those files so a header edit rebuilds the objects that use it. Tests and utilities compile and link most programs in one command, so their rules use `-MF` to put each `.d` file under the component's `BUILD` directory.

The kernel, tests, ISH, utilities, KRFFS tools, and Doom all use this dependency tracking. The details differ because their compile rules differ. A header edit does not require `make clean`.

## clangd compilation database

Run the generator from the repository root:

```sh
sh tools/dev/gen_compile_commands.sh
```

`tools/dev/gen_compile_commands.sh` builds one database per unit under `build/compile_commands/` from the build's own commands, by a dry run (`make -Bn`, or a build script's print mode) that joins wrapped recipe lines back into whole commands and keeps each command whose first word is the unit's compiler, one entry per C source operand. The twelve units are the four kernel variants (`rach2`, `rach2-recovery`, `rach2-metal`, `rach2-harness`), the guest programs (`tests`, `ish`, `utils`, `musl-programs`, `doom`) and the host programs (`krffs`, `tools`, `host-tests`). Strings are escaped for JSON, each database is validated line by line before it is published, and `tools/verify/dbcheck.sh` requires every in-house C source to be in one of them. Their union replaces `compile_commands.json` at the root. The generated files are ignored by Git. `tests/run style` runs the generator's `--selftest`, which covers the wrapped-recipe and validation cases on fixtures.

The database excludes the vendored projects. The checked-in `.clangd` selects `i686-elf-gcc` for the cross-compiled trees, `rach2/`, `tests/`, `ish/` and `utils/`; the host KRFFS tools keep the host compiler their entries name. Allow clangd to query that cross compiler:

```text
--query-driver=**/i686-elf-*gcc
```

For VS Code, set `"clangd.arguments": ["--query-driver=**/i686-elf-*gcc"]`. Other editors expose the same clangd option.

## Kernel source list

`C_SRC` in `rach2/sources.mk` is explicit. A new kernel C file can therefore exist without being compiled. `make check-sources` compares the list with `kernel.c` and the C files below `rach2/sys/`, and fails on an unlisted or missing file. `tests/run style` runs this check. It does not check Doom or vendor source lists.
