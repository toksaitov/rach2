# tests/Makefile: build the guest probes, the freestanding programs the suites
# run on Rach2 to test the kernel's system calls.
#
# Each probe is built for the architecture mk/arch.mk names, into build/tests/
# or build/x86_64/tests/, and makes its system calls through probe.h, which has
# each architecture's numbers, entry instruction and structure layouts. The one
# exception is dnstest, which links the ported musl, as the utilities do, so
# that it and the resolver it tests (utils/resolv.c) call the C library.

# The cross toolchain and the header dependencies (../mk/).
include ../mk/toolchain.mk
include ../mk/deps.mk
include ../mk/build.mk
CC := $(CROSS_CC)

CFLAGS = $(ARCH_CFLAGS) -nostdlib -static -ffreestanding -Wl,-Ttext=$(ARCH_USER_TEXT) -Wl,--build-id=none $(ARCH_LDFLAGS) $(DEPFLAGS)
# Flags an analysis adds from the command line (tools/verify/analyze.sh).
EXTRA ?=
CFLAGS += $(EXTRA)

# The musl link for dnstest: the architecture's compiler (for its libgcc), and
# the ported musl's headers and crt objects, as utils/Makefile links.
MUSL_CC     := $(CROSS_CC)
MUSL_PREFIX := $(TOP)/$(ARCH_MUSL)
MUSL_LIBGCC := $(shell $(MUSL_CC) $(ARCH_CFLAGS) -print-libgcc-file-name)
MUSL_CFLAGS  = $(ARCH_CFLAGS) -static -nostdlib -nostartfiles -isystem $(MUSL_PREFIX)/include \
               -O2 -Wall -Wextra $(DEPFLAGS)
MUSL_PRE     = $(MUSL_PREFIX)/lib/crt1.o $(MUSL_PREFIX)/lib/crti.o
MUSL_POST    = $(MUSL_PREFIX)/lib/libc.a $(MUSL_LIBGCC) $(MUSL_PREFIX)/lib/crtn.o \
               -Wl,-z,noexecstack $(ARCH_LDFLAGS)

# All test programs
PROGS = hello forktest shelltest dirtest stattest usbprobe mouseprobe intrprobe keylatprobe evprobe \
        rmtest mvtest lstest stdintest dup2test \
        pingtest packetprobe rawprobe frag6probe netifprobe dgramprobe udptest tcptest sockettest dnstest auxvprobe \
        tlsprobe fsbaseprobe wow64probe errnoprobe vmprobe mmapdevprobe udmabufprobe drmprobe sysdrmprobe vmfault forkro cowregrant \
        usercopyfault fdstale dirgrow dirprobe unlinkdirprobe pathcanon timeprobe sigprobe credprobe ownprobe permprobe trapprobe ofdprobe \
        waitprobe exitprobe pipeprobe pollprobe devprobe ttyprobe jobprobe miscprobe \
        ptyprobe rngprobe orphanprobe loopprobe zombieprobe survivorprobe procprobe suidprobe suidhelper sudoprobe modeprobe searchprobe stickyprobe blockprobe bootprobe persistprobe stressprobe winprobe dirtyprobe rebootprobe lagprobe fpuprobe xsaveprobe memprobe poolprobe vfsprobe mountprobe fatwriteprobe usbdiskprobe ext2probe krffsprobe loopdevprobe bootlogprobe lockwaitprobe \
        idreport fdfull diskfull oomprobe limitprobe loadprobe conloadprobe partsprobe busprobe ahciprobe bigfileprobe blankprobe lenovoacpi watchdogprobe

# The probes of an interface only i386 has, built for i686 alone: tlsprobe
# tests set_thread_area and the %gs thread pointer, where x86-64 keeps the
# thread pointer in the FS base. A probe whose rows only partly test such an
# interface builds those rows for i686 alone and is built for both.
I686_ONLY = tlsprobe
ifeq ($(ARCH),x86_64)
PROGS := $(filter-out $(I686_ONLY),$(PROGS))
endif

# The probes of an interface only x86-64 has, built for x86-64 alone:
# wow64probe runs 32-bit code in a 64-bit process, which an i686 kernel has
# no 64-bit mode to do.
X86_64_ONLY = wow64probe
ifeq ($(ARCH),i686)
PROGS := $(filter-out $(X86_64_ONLY),$(PROGS))
endif

# A probe is compiled and linked in one step, so the only file besides the
# binary is the .d that -MF writes under $(BUILD) (build/tests/, from
# mk/build.mk). The image manifests stage the binaries from there.
TARGETS = $(addprefix $(BUILD)/, $(PROGS))

# The configuration fingerprint (mk/fingerprint.mk): a changed flag or a
# reinstalled compiler rebuilds every probe. The purge names only this
# Makefile's products, since build/tests/musl belongs to tests/musl/build.sh,
# which may be compiling there in a parallel build.
CONFIG_TEXT = $(CC) $(CFLAGS) $(MUSL_CC) $(MUSL_CFLAGS) $(UDMABUF_CFLAGS)
CONFIG_PURGE = $(TARGETS) $(BUILD)/*.d
include ../mk/fingerprint.mk

.PHONY: all clean

all: $(TARGETS)
$(TARGETS): $(CONFIG)

# A probe is one freestanding source. -MMD, from $(DEPFLAGS), records the
# headers it includes in the .d that -MF names, so a changed header rebuilds it.
$(BUILD)/%: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(if $(filter udmabufprobe,$*),$(UDMABUF_CFLAGS)) -MF $(BUILD)/$*.d -o $@ $<

# The shared kernel ABI header uses C89's bool typedef.
UDMABUF_CFLAGS = -std=c99

# dnstest links the stub resolver of utils/ to test its parser on crafted
# replies and resolve_host() against a live server, and links the ported musl
# for both. It compiles two sources in one step, and the .d may not record the
# headers of both, so dnstest.c, resolv.c and netdb.h are named here as
# prerequisites; the .d adds the headers they include.
$(BUILD)/dnstest: dnstest.c ../utils/resolv.c ../utils/netdb.h \
    $(MUSL_PRE) $(MUSL_PREFIX)/lib/libc.a $(MUSL_LIBGCC) \
    $(MUSL_PREFIX)/lib/crtn.o
	@mkdir -p $(dir $@)
	$(MUSL_CC) $(MUSL_CFLAGS) -MF $(BUILD)/dnstest.d -o $@ \
	    $(MUSL_PRE) dnstest.c ../utils/resolv.c $(MUSL_POST)

-include $(addprefix $(BUILD)/, $(PROGS:=.d))

clean:
	rm -rf $(BUILD)
