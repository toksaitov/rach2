# Constants

LOADER = loader
KERNEL = kernel
KERNEL_BIN = $(BUILD)/$(KERNEL)

LINKER_SCRIPT = link

# The kernel source lists (ASM_SRC and C_SRC) live in sources.mk, which guest.mk
# includes too, so one file describes both builds.
include sources.mk

# Everything builds out of tree under $(BUILD) (build/rach2/, from mk/build.mk):
# the objects, the GCC-written .d files, the kernel binary, and the ISO staging,
# so the source tree never holds a build product.
ASM_OBJECTS = $(ASM_SRC:%.s=$(BUILD)/%.o)
C_OBJECTS   = $(C_SRC:%.c=$(BUILD)/%.o)

# Cross toolchain + automatic header-dependency tracking (shared; see ../mk/).
include ../mk/toolchain.mk
include ../mk/deps.mk
include ../mk/build.mk
CC := $(CROSS_CC)
# LD comes from CROSS_LD (mk/toolchain.mk): it honors an explicit `CROSS_LD=...`
# override and otherwise tracks CROSS_CC; and CROSS_CC itself absorbs a
# command-line `make CC=...`, so both the CC and CROSS_LD knobs reach the
# linker.
LD := $(CROSS_LD)

# -Werror makes the existing zero-warning policy a hard compiler gate (the
# quality gate builds with it on). Override with `make WERROR=` only for
# transient debugging.
WERROR ?= -Werror
CFLAGS  = -m32 -mno-sse -mno-sse2 -g -Wall -Wextra $(WERROR) -ansi -nostdlib -nostartfiles -nodefaultlibs $(DEPFLAGS)
LDFLAGS = -m elf_i386 -T $(LINKER_SCRIPT).ld

# Harness build (`make HARNESS=1`): the kernel reports each process exit status
# to QEMU's isa-debug-exit device so the test harness reads a real exit code.
ifdef HARNESS
CFLAGS += -DRACH2_HARNESS
endif

# Anything the caller wants added to every compile, which is how the second
# kernel is built: `make -C rach2 COMPONENT=rach2-metal
# KEXTRA=-DRACH2_DISK_READONLY` puts its objects and its binary under
# build/rach2-metal/ (mk/build.mk derives both from COMPONENT), so the two
# kernels never share an object. A build switch rather than a boot flag because
# the switch is what a command line edited at the GRUB prompt cannot undo, and
# the disk it protects is somebody's Windows.
KEXTRA ?=
CFLAGS += $(KEXTRA)

# General Rules

.PHONY : all
all : $(KERNEL_BIN)

# Bootable ISO images are built per RAM-disk image at the top level (make isos;
# see tools/image/mkiso.sh), so this Makefile only builds the kernel.

# Kernel

# The structural check runs on every link: a kernel the loaders would refuse, or
# one whose _end does not equal the image end, fails the build here rather than
# at a boot later. The link lands in a temporary file that is renamed only after
# the check passes, so a failed check cannot leave a newer kernel behind for the
# next make to treat as current. The checker is the project ELF validator, built
# with the host compiler; a checker newer than the binary rebuilds it first.
ELFCHECK = ../build/tools/inspect/elfcheck

$(KERNEL_BIN) : $(ASM_OBJECTS) $(C_OBJECTS)
	@mkdir -p $(dir $@)
	@if [ ! -x $(ELFCHECK) ] || [ ../tools/inspect/elfcheck.c -nt $(ELFCHECK) ]; then \
	    mkdir -p $(dir $(ELFCHECK)); \
	    cc -O2 -Wall -Wextra -o $(ELFCHECK) ../tools/inspect/elfcheck.c; \
	fi
	@tmp="$@.tmp.$$$$"; trap 'rm -f "$$tmp"' EXIT; \
	echo '$(LD) $(LDFLAGS) -o $@ ...'; \
	$(LD) $(LDFLAGS) -o "$$tmp" $^ && \
	$(ELFCHECK) --kernel "$$tmp" && \
	mv "$$tmp" $@

# The configuration fingerprint (mk/fingerprint.mk): make decides what to
# rebuild from timestamps, which say nothing about compiler flags, so building
# one tree twice with different switches would link some objects from each and
# produce a kernel that reports one policy and enforces the other, the single
# artifact this build must not ship. A changed configuration (such as
# -DRACH2_DISK_READONLY reaching the wrong tree) purges the whole object tree at
# parse time, so the component rebuilds from nothing and never mixes two option
# sets. The two policy kernels still live in two trees (COMPONENT=rach2-metal
# for the read-only one).
CONFIG_TEXT = $(CC) $(CFLAGS) $(LD) $(LDFLAGS)
include ../mk/fingerprint.mk


$(ASM_OBJECTS) : $(BUILD)/%.o : %.s
	@mkdir -p $(dir $@)
	$(CC) -m32 -g -c -o $@ $<

$(C_OBJECTS) : $(BUILD)/%.o : %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -o $@ -c $<

# Pull in the per-object header dependencies GCC wrote (-MMD), so editing a
# header rebuilds exactly the objects that include it. Missing on a first build
# (no .d yet): the leading '-' makes that silent. They live beside the objects
# in $(BUILD).
-include $(C_OBJECTS:.o=.d)

# Emit the explicit kernel source list (expanded), for
# tools/verify/check_sources.sh (the source-list guard that fails if a rach2 .c
# on disk is missing from C_SRC). .PHONY so it always runs; prints nothing but
# the list.
.PHONY : print-c-src
print-c-src :
	@echo $(C_SRC)

# The assembly list too, for tools/verify/check_guest.py (the guest build's list
# guard): the guest makefile must name exactly these sources.
.PHONY : print-asm-src
print-asm-src :
	@echo $(ASM_SRC)

.PHONY : clean
clean :
	rm -rf $(BUILD)
