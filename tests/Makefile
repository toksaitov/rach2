# Rach2 Syscall Test Programs Makefile
#
# Builds freestanding executables for testing kernel syscalls, for the
# architecture mk/arch.mk names, into build/tests/ or build/x86_64/tests/. They
# run in user mode on the Rach2 kernel and make their system calls through
# probe.h, which has each architecture's numbers, entry and layouts. The one
# exception is dnstest, which links the ported musl (like the utils) so it and
# the shared resolver it pulls in (utils/resolv.c) use the real libc rather
# than hand-rolled system calls; see its rule below.

# Shared cross toolchain + automatic header-dependency tracking (see ../mk/).
include ../mk/toolchain.mk
include ../mk/deps.mk
include ../mk/build.mk
CC := $(CROSS_CC)

CFLAGS = $(ARCH_CFLAGS) -nostdlib -static -ffreestanding -Wl,-Ttext=$(ARCH_USER_TEXT) -Wl,--build-id=none $(ARCH_LDFLAGS) $(DEPFLAGS)
# Flags an analysis adds from the command line (tools/verify/analyze.sh).
EXTRA ?=
CFLAGS += $(EXTRA)

# The musl link for dnstest: the architecture's compiler (for its libgcc), the
# ported musl headers and crt objects, exactly as utils/Makefile links.
MUSL_CC     := $(CROSS_CC)
MUSL_PREFIX := $(TOP)/$(ARCH_MUSL)
MUSL_LIBGCC := $(shell $(MUSL_CC) $(ARCH_CFLAGS) -print-libgcc-file-name)
MUSL_CFLAGS  = $(ARCH_CFLAGS) -static -nostdlib -nostartfiles -isystem $(MUSL_PREFIX)/include \
               -O2 -Wall -Wextra $(DEPFLAGS)
MUSL_PRE     = $(MUSL_PREFIX)/lib/crt1.o $(MUSL_PREFIX)/lib/crti.o
MUSL_POST    = $(MUSL_PREFIX)/lib/libc.a $(MUSL_LIBGCC) $(MUSL_PREFIX)/lib/crtn.o \
               -Wl,-z,noexecstack $(ARCH_LDFLAGS)

# All test programs
PROGS = hello filetest forktest shelltest dirtest stattest usbprobe mouseprobe intrprobe keylatprobe \
        touchtest rmtest mvtest lstest stdintest dup2test \
        pingtest packetprobe rawprobe netifprobe dgramprobe udptest tcptest sockettest dnstest auxvprobe \
        tlsprobe fsbaseprobe errnoprobe vmprobe vmfault vmro forkro cowprobe cowregrant \
        usercopyfault forkleak fdstale dirgrow dirprobe unlinkdirprobe pathcanon timeprobe sigprobe credprobe ownprobe permprobe trapprobe ofdprobe \
        waitprobe exitprobe pipeprobe pollprobe devprobe ttyprobe jobprobe miscprobe \
        ptyprobe rngprobe orphanprobe loopprobe zombieprobe survivorprobe procprobe suidprobe suidhelper sudoprobe modeprobe searchprobe stickyprobe blockprobe bootprobe persistprobe stressprobe winprobe dirtyprobe rebootprobe lagprobe fpuprobe xsaveprobe memprobe poolprobe vfsprobe mountprobe fatwriteprobe usbdiskprobe calibprobe placeprobe ext2probe krffsprobe loopdevprobe \
        idreport fdfull diskfull oomprobe limitprobe loadprobe conloadprobe disksprobe partsprobe busprobe ahciprobe bigfileprobe

# The probes of an interface only i386 has, built for i686 alone: tlsprobe
# tests set_thread_area and the %gs thread pointer, where x86-64 keeps the
# thread pointer in the FS base. A probe whose rows only partly test such an
# interface builds those rows for i686 alone and is built for both.
I686_ONLY = tlsprobe
ifeq ($(ARCH),x86_64)
PROGS := $(filter-out $(I686_ONLY),$(PROGS))
endif

# These compile+link in one step, so there are no .o intermediates; only the
# GCC-written .d, which -MF puts under $(BUILD) (build/tests/, from
# mk/build.mk), so the source tree stays clean and the RAM-disk staging picks
# the binaries up there.
TARGETS = $(addprefix $(BUILD)/, $(PROGS))

# The configuration fingerprint (mk/fingerprint.mk): a changed flag or a
# reinstalled compiler rebuilds every probe automatically. The purge names this
# Makefile's own products because build/tests/musl belongs to
# tests/musl/build.sh, which can be compiling there in a parallel build.
CONFIG_TEXT = $(CC) $(CFLAGS) $(MUSL_CC) $(MUSL_CFLAGS)
CONFIG_PURGE = $(TARGETS) $(BUILD)/*.d
include ../mk/fingerprint.mk

.PHONY: all clean

all: $(TARGETS)
$(TARGETS): $(CONFIG)

# Each probe is a single freestanding source compiled+linked in one step; -MMD
# (from $(DEPFLAGS)) writes the deps, -MF puts that .d under $(BUILD), so
# editing a shared header rebuilds it.
$(BUILD)/%: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -MF $(BUILD)/$*.d -o $@ $<

# dnstest links the shared stub resolver from utils/ so it can exercise both the
# pure parser (crafted vectors) and resolve_host() (live), and it links the
# ported musl so both it and resolv.c use the real libc. It compiles+links two
# sources in one step, where -MF records only the last source's headers, so the
# build inputs (dnstest.c, resolv.c, netdb.h) are declared as explicit
# prerequisites; the $(BUILD) .d adds the resolver's transitive headers on top.
$(BUILD)/dnstest: dnstest.c ../utils/resolv.c ../utils/netdb.h \
    $(MUSL_PRE) $(MUSL_PREFIX)/lib/libc.a $(MUSL_LIBGCC) \
    $(MUSL_PREFIX)/lib/crtn.o
	@mkdir -p $(dir $@)
	$(MUSL_CC) $(MUSL_CFLAGS) -MF $(BUILD)/dnstest.d -o $@ \
	    $(MUSL_PRE) dnstest.c ../utils/resolv.c $(MUSL_POST)

-include $(addprefix $(BUILD)/, $(PROGS:=.d))

clean:
	rm -rf $(BUILD)
