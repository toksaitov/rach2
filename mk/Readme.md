# Shared host Make rules

The files under `mk/` define the common GNU Make settings for in-house host builds. Each component includes only the files it needs. Vendored projects use their own build systems.

* `toolchain.mk` selects `i686-elf-gcc`, or `x86_64-elf-gcc` if the i686 compiler is absent. It derives the matching linker, archiver, strip tool, and `objcopy`. Command-line `CC` and `CROSS_*` settings override the defaults. Only `make clean` works without a cross compiler.
* `deps.mk` defines `DEPFLAGS := -MMD -MP`. These options write header dependencies and tolerate a header removed since the preceding build.
* `build.mk` defaults `TOP` to `..`, `COMPONENT` to the current directory name, and `BUILD` to `$(TOP)/build/$(COMPONENT)`. A Makefile can override each value before including it.
* `rules.mk` supplies a `$(BUILD)/%.o` C compile rule and includes the resulting `.d` files. `ish/` uses it directly. Other components have compile rules for their own source and link layouts.

## Output directories

In-house builds put objects, dependency files, and binaries under the repository's ignored `build/` directory. The layout follows component names: for example, the kernel is `build/rach2/kernel`, its objects follow the paths below `build/rach2/`, host KRFFS tools are under `build/krffs/`, and Doom is under `build/ports/doom/`. The top-level build puts images under `build/images/` and stages image inputs from BusyBox, Dropbear, TinyCC, and pdpmake under `build/vendor/`.

`BUILD` stays relative to a component's working directory. Both `make -C utils` and the top-level sub-make therefore resolve `../build/utils` to the same directory. This also keeps absolute repository paths, including paths with spaces, out of Make's target names.

Vendored projects are the exception to the output convention. Their build scripts extract working trees under `vendor/`, and musl installs under `vendor/musl/install/`. `make clean` preserves these vendor builds and the staged files under `build/vendor/`; `make distclean` removes them.

## Header dependencies

An object compile with `$(DEPFLAGS)` writes a `.d` file beside its `.o`. Makefiles include those files so a header edit rebuilds the objects that use it. Tests and utilities compile and link most programs in one command, so their rules use `-MF` to put each `.d` file under the component's `BUILD` directory.

The kernel, tests, ISH, utilities, KRFFS tools, and Doom all use this dependency tracking. The details differ because their compile rules differ. A header edit does not require `make clean`.

## clangd compilation database

Run this target from the repository root:

```sh
make compile-commands
```

`tools/gen_compile_commands.sh` runs `make -Bn` in `rach2/`, `tests/`, `ish/`, and `utils/`, then filters the output for lines containing an i686 cross-GCC path and a C source operand. Before replacing `compile_commands.json`, it requires an entry for every kernel source in `C_SRC`. The generated file is ignored by Git.

The generator currently mishandles wrapped recipes on the Homebrew toolchain. A continuation line can match because its libgcc path contains `i686-elf-gcc`; the script then writes the line's tab into a JSON string without escaping it. The target reports success, but the resulting file is not valid JSON. Do not rely on `make compile-commands` until `tools/gen_compile_commands.sh` validates and escapes its output.

The database excludes the host KRFFS tools, vendored projects, and Doom. The checked-in `.clangd` selects `i686-elf-gcc` for the four covered trees. Allow clangd to query that cross compiler:

```text
--query-driver=**/i686-elf-*gcc
```

For VS Code, set `"clangd.arguments": ["--query-driver=**/i686-elf-*gcc"]`. Other editors expose the same clangd option.

## Kernel source list

`C_SRC` in `rach2/Makefile` is explicit. A new kernel C file can therefore exist without being compiled. `make check-sources` compares the list with `kernel.c` and the C files below `rach2/sys/`, and fails on an unlisted or missing file. `make verify-style` runs this check. It does not check Doom or vendor source lists.
