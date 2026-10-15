# mk/toolchain.mk: the cross toolchain of the components built for Rach2: the
# kernel, the guest programs of tests/, ish/ and utils/, and the Doom port.
#
# It reads the architecture from mk/arch.mk, which it includes, and picks that
# architecture's freestanding cross GCC: `i686-elf-gcc` for i686 and
# `x86_64-elf-gcc` for x86_64. The matching binutils `ld`, `ar`, `strip`, and
# `objcopy` are derived from its name. The krffs/ tools run on the build
# machine, are built with the host compiler, and do not include this file.
#
# The prefix is the architecture's and nothing else's. An x86_64-elf-gcc given
# -m32 compiles i386 code, but it has no 32-bit libgcc, and the vendored ports
# call their toolchain by the prefix mk/arch.sh gives them, so a build of
# one architecture with the other's compiler would fail part way, in musl.
# tools/setup/preflight.sh requires the toolchain of the ARCH in force, so a
# missing one stops the build there, named.
#
# A toolchain installed under other names is given on the command line:
#   make CROSS_CC=/path/to/i686-elf-gcc
#   make CROSS_CC=/path/to/i686-elf-gcc CROSS_LD=/path/to/i686-elf-ld
# CROSS_CC is the freestanding cross C compiler, CROSS_LD the matching linker
# (the kernel's `-T link.ld` link), and CROSS_AR, CROSS_STRIP and
# CROSS_OBJCOPY the matching binutils.
include $(dir $(lastword $(MAKEFILE_LIST)))arch.mk

# The compiler is the first of these that is set: CROSS_CC; a `make CC=...`
# given on the command line, though not a CC in the environment, which is
# usually the host compiler; the architecture's compiler found on the path.
CROSS_CC := $(or $(CROSS_CC),\
                 $(if $(filter command line,$(origin CC)),$(CC)),\
                 $(shell command -v $(CROSS_PREFIX)gcc 2>/dev/null))

# With no compiler found, make stops here and says what to install, rather
# than run an empty command later. Only `clean` needs no toolchain, so the
# error is for the default goal and for any goal other than `clean`;
# `$(or $(MAKECMDGOALS),build)` stands for the empty goal list of a default
# build.
ifeq ($(strip $(CROSS_CC)),)
ifneq ($(filter-out clean,$(or $(MAKECMDGOALS),build)),)
$(error no cross compiler found for ARCH=$(ARCH): install $(CROSS_PREFIX)gcc, or pass CROSS_CC=/path/to/gcc; see `sh tools/setup/doctor.sh`)
endif
endif

# The binutils are the compiler's name with its `gcc` suffix replaced (`%gcc`
# to `%ld`), so that a path with "gcc" elsewhere in it, such as
# /opt/gcc-16/bin/i686-elf-gcc, is left whole. Each can be set by hand.
CROSS_LD      := $(or $(CROSS_LD),$(patsubst %gcc,%ld,$(CROSS_CC)))
CROSS_AR      := $(or $(CROSS_AR),$(patsubst %gcc,%ar,$(CROSS_CC)))
CROSS_STRIP   := $(or $(CROSS_STRIP),$(patsubst %gcc,%strip,$(CROSS_CC)))
CROSS_OBJCOPY := $(or $(CROSS_OBJCOPY),$(patsubst %gcc,%objcopy,$(CROSS_CC)))
