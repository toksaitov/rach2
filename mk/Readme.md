# `mk/` — shared GNU-Make building blocks

Small, readable includes that every Rach2 component's Makefile shares, so the build
configuration lives in **one** place instead of being copy-pasted. The build stays
plain GNU Make and every compile/link command stays visible — these files remove
duplication, they do not hide what the build does.

| file            | what it provides |
|-----------------|------------------|
| `toolchain.mk`  | detects the freestanding cross GCC (`i686-elf-gcc`, else `x86_64-elf-gcc`) once and derives `CROSS_LD/AR/STRIP/OBJCOPY`; errors with an actionable message if none is found (override with `make CROSS_CC=...`). |
| `deps.mk`       | `DEPFLAGS = -MMD -MP` — automatic header-dependency tracking. |

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
