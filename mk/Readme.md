# `mk/`: shared GNU-Make building blocks

Small, readable includes that every Rach2 component's Makefile shares, so the build configuration lives in one place instead of being copy-pasted. The build stays plain GNU Make and every compile and link command stays visible; these files remove duplication, they do not hide what the build does.

* `toolchain.mk` detects the freestanding cross GCC (`i686-elf-gcc`, else `x86_64-elf-gcc`) once and derives `CROSS_LD`, `CROSS_AR`, `CROSS_STRIP`, and `CROSS_OBJCOPY`. It fails with an actionable message if none is found; override with `make CROSS_CC=...`.
* `deps.mk` sets `DEPFLAGS = -MMD -MP` for automatic header-dependency tracking.
* `build.mk` puts every component's output out of tree under one repo-root `build/` directory that mirrors the source tree. A component includes it and gets `BUILD` (its own `build/<component>/`, kept relative as `$(TOP)/build/...`, which is what makes `make -C utils` and the top-level make resolve to the same directory), plus `BUILDROOT`, `TOP`, and `COMPONENT`; the objects, the `.d` files, and the final binaries all land under `BUILD`.
* `rules.mk` holds the canonical out-of-tree `$(BUILD)/%.o: %.c` compile rule (`mkdir -p` then compile) and the `-include $(OBJECTS:.o=.d)` line, for a component that compiles to objects and then links the plain way (`ish/`). Components with a special compile shape (`ports/doom`, the krffs FUSE tools) keep their own rules but still share the `build/` convention and `deps.mk`. The krffs tools do not include `toolchain.mk`, because they are host programs and build with the host `cc`.

## Out-of-tree build (`build/`)

Every in-house component builds out of tree into one repo-root `build/` directory whose layout mirrors the source tree: the kernel objects under `build/rach2/sys/...` and the kernel at `build/rach2/kernel`, the utils in `build/utils/`, the tests in `build/tests/`, the krffs host tools in `build/krffs/` and the RAM-disk images in `build/images/`, Doom in `build/ports/doom/`, and the staged BusyBox, Dropbear, TinyCC and pdpmake capstones in `build/vendor/`. The objects, the GCC-written `.d` files, and the final binaries all live there, never beside the sources, so the source tree never holds a build product. A component includes `../mk/build.mk`, which sets `BUILD` to its own `build/<component>/`, relative rather than absolute: each sub-make runs with `CURDIR` at its own component directory, so `../build` names the one repo-root directory whether it was reached by `make -C utils` or from the top, the echoed compile commands stay short, and a repository path containing a space is not embedded in every object path, which GNU Make could not then split correctly. The one step that needs an absolute path wraps it in `$(abspath ...)` at the call site; it then names its `OBJECTS` as `$(SRC:%.c=$(BUILD)/%.o)` and either includes `rules.mk` or has the same `mkdir -p $(dir $@)` compile shape. The single-source test and util programs (compile and link in one step, no `.o`) redirect their `.d` with `-MF $(BUILD)/...`. One root `.gitignore` entry (`build/`) covers it all, and each component's `make clean` removes its own `build/<component>` tree. The vendored musl, busybox, dropbear, tcc and pdpmake trees are built by their own upstream build systems inside `vendor/` (musl installs to `vendor/musl/install`); only what an image or a gate consumes is redirected into `build/vendor/`: the binaries, and beside them what a binary cannot work without or a check must read (tcc's runtime, headers and toolchain wrappers, busybox's applet tables and resolved configs).

## Automatic header dependencies (no more `make clean`)

With `$(DEPFLAGS)` on each C compile and `-include $(OBJECTS:.o=.d)` near the end of a Makefile, GCC writes a `.d` beside each `.o` (under `build/<component>/`) recording the headers that object actually included. Make then rebuilds exactly the objects a header edit affects. Editing a `.h` no longer needs a full `make clean && make`. The `.d` files are build artifacts (git-ignored) and `make clean` removes them.

A component Makefile uses these like this:

    include ../mk/toolchain.mk
    include ../mk/deps.mk
    include ../mk/build.mk
    CC := $(CROSS_CC)
    CFLAGS += $(DEPFLAGS)
    ...
    -include $(OBJECTS:.o=.d)

The host-side krffs FUSE tools build with the host compiler, so they include `deps.mk` and `build.mk` only, not `toolchain.mk`.

## IDE support (clangd): `compile_commands.json` and `.clangd`

For real autocomplete, go-to-definition, and inline diagnostics on the freestanding kernel, clangd needs a compilation database. GCC has no clang-style `-MJ`, so it is generated from the build's own commands:

    make compile-commands     # writes compile_commands.json at the repo root

The generator (`tools/gen_compile_commands.sh`) dry-runs each in-house component (`make -Bn`) and turns every i686-elf cross-GCC C compile into a database entry. It covers the kernel (`rach2`) and the musl guest programs (`tests`, `ish`, `utils`); the host krffs FUSE tools and the vendored and Doom trees are excluded. The file is git-ignored and regenerable; rerun it after adding a source or changing flags.

The checked-in `.clangd` points clangd at the i686-elf cross GCC for those trees, so it resolves the cross toolchain's freestanding headers (`<stddef.h>` and the rest) instead of the host's. clangd only queries a non-host driver when you allow it, so launch clangd with:

    --query-driver=**/i686-elf-*gcc

In VS Code, set `"clangd.arguments": ["--query-driver=**/i686-elf-*gcc"]`; JetBrains and others have an equivalent.

## Kernel source-list guard: `make check-sources`

The kernel's sources are an explicit list (`C_SRC` in `rach2/Makefile`), so a new `.c` that is not added there is silently left out of the build. `make check-sources` (`tools/check_sources.sh`) compares `C_SRC` against the `.c` files on disk under `rach2/` and fails if they disagree. It runs inside `make verify-style` as a hard gate. Doom and the vendored trees keep their own lists and are not gated.
