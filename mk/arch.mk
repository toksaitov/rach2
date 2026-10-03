# mk/arch.mk: the architecture a build is for.
#
# `make ARCH=x86_64` builds for x86-64; `ARCH` is `i686` when it is not
# given. The root Makefile and mk/toolchain.mk include this file, so the
# kernel, the guest programs and the vendored ports all read one setting. The
# host programs (the KRFFS tools, the checkers under tools/, the host tests)
# do not include it: they run on the build machine and are the same for
# either architecture.
#
# ARCH_SUFFIX is what a path of this architecture adds after `build`. It is
# empty for i686, whose products keep the paths they had before there was a
# second architecture (build/rach2/kernel), and `/x86_64` for x86-64
# (build/x86_64/rach2/kernel), so the two trees never share a file.
#
# ARCH_VENDOR is where a vendored port's generated work lives: its extracted
# tree, its stamp, and musl's install. For i686 that is the port's own
# directory under vendor/, as it always was (vendor/musl/install); for x86-64
# it is build/x86_64/vendor-work/, so nothing of the second architecture lands
# in the source tree.
#
# CROSS_PREFIX names the cross toolchain, ARCH_CFLAGS selects the word size
# for every guest compile and link, and ARCH_USER_TEXT is the address at which
# the freestanding programs (the probes, ish and Doom) place their text: the
# address TinyCC links a program at on that architecture, so a program built
# on Rach2 and the same program built here are laid out alike. ARCH_LDFLAGS
# is what a guest link adds: x86-64 links align segments to 4 KiB pages, not
# the 2 MiB the x86-64 linker uses by default.
ARCH ?= i686

ifeq ($(ARCH),i686)
ARCH_SUFFIX    =
ARCH_VENDOR    = vendor
CROSS_PREFIX   = i686-elf-
ARCH_CFLAGS    = -m32
ARCH_LDFLAGS   =
ARCH_USER_TEXT = 0x08048000
else ifeq ($(ARCH),x86_64)
ARCH_SUFFIX    = /x86_64
ARCH_VENDOR    = build/x86_64/vendor-work
CROSS_PREFIX   = x86_64-elf-
ARCH_CFLAGS    = -m64
ARCH_LDFLAGS   = -Wl,-z,max-page-size=0x1000
ARCH_USER_TEXT = 0x400000
else
$(error ARCH=$(ARCH): the architectures are i686 and x86_64)
endif

# The build tree of this architecture, relative to the repository root, and
# the install directory of the ported musl the guest programs link against.
ARCH_BUILD  = build$(ARCH_SUFFIX)
ARCH_MUSL   = $(ARCH_VENDOR)/musl/install
