# mk/toolchain.mk: shared cross-toolchain detection for Rach2's freestanding
# components (the kernel, the user-mode tests/ish/utils, the Doom port). Include
# this once instead of re-deriving the compiler in every Makefile.
#
# It picks the i686 *freestanding* cross GCC, `i686-elf-gcc`. The matching
# binutils `ld`, `ar`, `strip`, and `objcopy` are derived from its name. The
# host FUSE tools in krffs/ build with the *host* compiler and deliberately do
# NOT include this file.
#
# The target prefix is fixed, not a preference: vendor/musl/build_libc.sh
# configures with CROSS_COMPILE=i686-elf- and both muslgcc wrappers call
# i686-elf-gcc by name, so a toolchain built for another target compiles the
# kernel and then fails in the userland. A toolchain installed elsewhere under
# that same prefix works, through CROSS_CC below.
#
# Override on the command line if your toolchain is named differently, e.g.
#   make CROSS_CC=/path/to/i686-elf-gcc
#   make CROSS_CC=/path/to/i686-elf-gcc CROSS_LD=/path/to/i686-elf-ld
#     (a toolchain whose names do not follow the pattern)
# CROSS_CC : the freestanding cross C compiler
# CROSS_LD : the matching linker (used for the kernel's `-T link.ld` link)
# CROSS_AR / CROSS_STRIP / CROSS_OBJCOPY : the matching binutils

# $(or ...) returns the first non-empty: a caller-supplied CROSS_CC wins; then
# an explicit command-line `make CC=...` (so the conventional knob works too,
# but NOT an incidental environment CC=clang, which is not a cross GCC); then
# i686-elf-gcc on the path. ($(or) short-circuits, so a later probe runs only
# if needed.)
CROSS_CC := $(or $(CROSS_CC),\
                 $(if $(filter command line,$(origin CC)),$(CC)),\
                 $(shell command -v i686-elf-gcc 2>/dev/null))

# Fail fast with an actionable message, not later running an empty
# compiler as a command. Only `clean` needs no toolchain, so error for a default
# build (no goal) or ANY goal other than clean (but not when clean is the sole
# goal) (`make clean` on a box without the cross GCC must work). `$(or
# $(MAKECMDGOALS), build)` stands in a placeholder for the empty default-build
# goal list. `sh tools/setup/doctor.sh` explains how to install the toolchain.
ifeq ($(strip $(CROSS_CC)),)
ifneq ($(filter-out clean,$(or $(MAKECMDGOALS),build)),)
$(error no cross compiler found: install i686-elf-gcc, or pass CROSS_CC=/path/to/gcc; see `sh tools/setup/doctor.sh`)
endif
endif

# Derive the binutils from the gcc name with a SUFFIX-only rewrite (`%gcc ->
# %ld`), so a path that happens to contain "gcc" (e.g.
# /opt/gcc-16/bin/i686-elf-gcc) is not mangled. Each is overridable for a
# toolchain whose names don't follow the pattern.
CROSS_LD      := $(or $(CROSS_LD),$(patsubst %gcc,%ld,$(CROSS_CC)))
CROSS_AR      := $(or $(CROSS_AR),$(patsubst %gcc,%ar,$(CROSS_CC)))
CROSS_STRIP   := $(or $(CROSS_STRIP),$(patsubst %gcc,%strip,$(CROSS_CC)))
CROSS_OBJCOPY := $(or $(CROSS_OBJCOPY),$(patsubst %gcc,%objcopy,$(CROSS_CC)))
