# `mk/` — shared GNU-Make building blocks

Small, readable includes that every Rach2 component's Makefile shares, so the build
configuration lives in **one** place instead of being copy-pasted. The build stays
plain GNU Make and every compile/link command stays visible — these files remove
duplication, they do not hide what the build does.

| file            | what it provides |
|-----------------|------------------|
| `toolchain.mk`  | detects the freestanding cross GCC (`i686-elf-gcc`, else `x86_64-elf-gcc`) once and derives `CROSS_LD/AR/STRIP/OBJCOPY`; errors with an actionable message if none is found (override with `make CROSS_CC=...`). |
| `deps.mk`       | `DEPFLAGS = -MMD -MP` — automatic header-dependency tracking. |
| `rules.mk`      | the canonical `%.o: %.c` compile rule + `-include $(OBJECTS:.o=.d)` for a component that compiles to objects then links the plain way (`ish/`). Components with a special compile shape (`ports/doom`, the krffs FUSE tools) keep their own rules but still share `toolchain.mk`/`deps.mk`. |

## Automatic header dependencies (no more `make clean`)

With `$(DEPFLAGS)` on each C compile and `-include $(OBJECTS:.o=.d)` near the end of a
Makefile, GCC writes a `.d` next to each `.o` recording the headers that object
actually included; make then rebuilds **exactly** the objects affected by a header
edit. Editing a `.h` no longer needs a full `make clean && make`. The `.d` files are
build artifacts (git-ignored) and are removed by `make clean`.

A component Makefile uses both like this:

    include ../mk/toolchain.mk
    include ../mk/deps.mk
    CC := $(CROSS_CC)
    CFLAGS += $(DEPFLAGS)
    ...
    -include $(OBJECTS:.o=.d)

The host-side krffs FUSE tools build with the *host* compiler, so they include
`deps.mk` only, not `toolchain.mk`.

## IDE support (clangd): `compile_commands.json` + `.clangd`

For real autocomplete, go-to-definition, and inline diagnostics on the **freestanding**
kernel, clangd needs a compilation database. GCC has no clang-style `-MJ`, so it is
generated from the build's own commands:

    make compile-commands     # writes compile_commands.json at the repo root

The generator (`tools/gen_compile_commands.sh`) dry-runs each in-house component
(`make -Bn`) and turns every i686-elf cross-GCC C compile into a database entry. It
covers the kernel (`rach2`) and the musl guest programs (`tests`, `ish`, `utils`); the
host krffs FUSE tools and the vendored/Doom trees are excluded. The file is git-ignored
and regenerable — rerun it after adding a source or changing flags.

The checked-in **`.clangd`** points clangd at the i686-elf cross GCC for those trees, so
it resolves the cross toolchain's freestanding headers (`<stddef.h>`, …) instead of the
host's. clangd only queries a non-host driver when you allow it, so launch clangd with:

    --query-driver=**/i686-elf-*gcc

(VS Code: set `"clangd.arguments": ["--query-driver=**/i686-elf-*gcc"]`; JetBrains and
others have an equivalent.)

## Kernel source-list guard: `make check-sources`

The kernel's sources are an **explicit** list (`C_SRC` in `rach2/Makefile`), so a new
`.c` that is not added there is silently left out of the build. `make check-sources`
(`tools/check_sources.sh`) compares `C_SRC` against the `.c` files on disk under
`rach2/` and fails if they disagree — and it runs inside `make verify-style` as a hard
gate. Doom and the vendored trees keep their own lists and are not gated.
