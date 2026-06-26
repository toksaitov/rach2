# Rach2 Syscall Test Programs Makefile
#
# Builds freestanding ELF32 executables for testing kernel syscalls. They run in
# user mode on the Rach2 kernel. The one exception is dnstest, which links the
# ported musl (like the utils) so it and the shared resolver it pulls in
# (utils/resolv.c) use the real libc rather than hand-rolled int 0x80; see its
# rule below.

# Shared cross toolchain + automatic header-dependency tracking (see ../mk/).
include ../mk/toolchain.mk
include ../mk/deps.mk
CC := $(CROSS_CC)

CFLAGS = -m32 -nostdlib -static -ffreestanding -Wl,-Ttext=0x08048000 $(DEPFLAGS)

# The musl link for dnstest: i686-elf-gcc specifically (for the 32-bit libgcc),
# the ported musl headers and crt objects, exactly as utils/Makefile links.
MUSL_CC     := $(or $(shell command -v i686-elf-gcc 2>/dev/null),$(CROSS_CC))
MUSL_PREFIX := ../vendor/musl/install
MUSL_LIBGCC := $(shell $(MUSL_CC) -m32 -print-libgcc-file-name)
MUSL_CFLAGS  = -m32 -static -nostdlib -nostartfiles -isystem $(MUSL_PREFIX)/include \
               -O2 -Wall -Wextra $(DEPFLAGS)
MUSL_PRE     = $(MUSL_PREFIX)/lib/crt1.o $(MUSL_PREFIX)/lib/crti.o
MUSL_POST    = $(MUSL_PREFIX)/lib/libc.a $(MUSL_LIBGCC) $(MUSL_PREFIX)/lib/crtn.o \
               -Wl,-z,noexecstack

# All test programs
PROGS = hello filetest forktest shelltest dirtest stattest \
        touchtest rmtest mvtest lstest stdintest dup2test \
        pingtest udptest tcptest sockettest dnstest auxvprobe \
        tlsprobe errnoprobe vmprobe vmfault vmro forkro cowprobe dirprobe \
        timeprobe sigprobe ofdprobe waitprobe pipeprobe pollprobe devprobe \
        ttyprobe jobprobe miscprobe ptyprobe rngprobe

# These compile+link in one step, so there are no .o intermediates; only the
# GCC-written .d, which -MF redirects into .build/ to keep the source tree clean. The
# final probe binaries stay in place for the RAM-disk staging.
BUILD = .build

.PHONY: all clean

all: $(PROGS)

# Each probe is a single freestanding source compiled+linked in one step; -MMD (from
# $(DEPFLAGS)) writes the deps, -MF puts that .d under .build/, so editing a shared
# header rebuilds it.
%: %.c
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -MF $(BUILD)/$*.d -o $@ $<

# dnstest links the shared stub resolver from utils/ so it can exercise both the pure
# parser (crafted vectors) and resolve_host() (live), and it links the ported musl so
# both it and resolv.c use the real libc. It compiles+links two sources in one step,
# where -MF records only the last source's headers, so the build inputs (dnstest.c,
# resolv.c, netdb.h) are declared as explicit prerequisites; the .build/ .d adds the
# resolver's transitive headers on top.
dnstest: dnstest.c ../utils/resolv.c ../utils/netdb.h
	@mkdir -p $(BUILD)
	$(MUSL_CC) $(MUSL_CFLAGS) -MF $(BUILD)/dnstest.d -o $@ \
	    $(MUSL_PRE) dnstest.c ../utils/resolv.c $(MUSL_POST)

-include $(addprefix $(BUILD)/, $(PROGS:=.d))

clean:
	rm -rf $(BUILD)
	rm -f $(PROGS)
	@find . \( -name '*.o' -o -name '*.d' \) -type f -exec rm -f {} +
