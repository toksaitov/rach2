# rach2/Makefile: the host build of the kernel with the cross compiler.
# Constants

# The architecture the kernel is built for, i686 unless the caller names
# another; sys/arch/$(ARCH)/ has its processor's files.
ARCH ?= i686

KERNEL = kernel
KERNEL_BIN = $(BUILD)/$(KERNEL)

LINKER_SCRIPT = sys/arch/$(ARCH)/link

# The kernel source lists, ASM_SRC and C_SRC, are in sources.mk, which guest.mk
# includes too, so one file lists the sources of both builds.
include sources.mk

# Everything this Makefile writes is under $(BUILD), from mk/build.mk,
# build/rach2/ or build/x86_64/rach2/ for an ordinary build: the objects, the
# .d files GCC writes, and the kernel, so the source tree never holds a build
# product.
ASM_OBJECTS = $(ASM_SRC:%.s=$(BUILD)/%.o)
C_OBJECTS   = $(C_SRC:%.c=$(BUILD)/%.o)

# ACPICA's core (sys/acpi/sources.mk), which vendor/acpica/build_acpica.sh
# extracts under this architecture's build tree, and its objects, under
# $(BUILD)/acpica/ by their paths in the core.
ACPICA          = $(BUILDROOT)/vendor/acpica
ACPICA_STAMP    = $(ACPICA)/.stamp
ACPICA_OBJECTS  = $(ACPICA_C_SRC:$(ACPICA)/%.c=$(BUILD)/acpica/%.o)
ACPI_OBJECTS    = $(ACPI_C_SRC:%.c=$(BUILD)/%.o)

# The cross toolchain, the header dependencies and the build directory, which
# every component shares (../mk/).
include ../mk/toolchain.mk
include ../mk/deps.mk
include ../mk/build.mk
CC := $(CROSS_CC)
# LD is CROSS_LD from mk/toolchain.mk: a `CROSS_LD=` on the command line, or
# else the linker beside CROSS_CC, which is itself taken from a command-line
# `make CC=` when no CROSS_CC is given.
LD := $(CROSS_LD)

# -Werror makes every warning an error, so a kernel with a warning does not
# build. `make WERROR=` turns it off for a build made while debugging.
WERROR ?= -Werror
include ../mk/warnings.mk
# The flags of the architecture. i686 code uses no SSE. x86-64 code runs in
# the kernel's top 2 GiB (-mcmodel=kernel), uses no red zone below the stack
# pointer, which an interrupt in the kernel would overwrite, and touches no
# MMX or SSE register, which hold the programs' state; its links align
# segments to 4 KiB pages, not the linker's 2 MiB, so the file follows the
# memory layout that the a.out kludge copies (sys/arch/x86_64/link.ld), and
# it writes no unwinding tables.
ifeq ($(ARCH),x86_64)
ARCH_AFLAGS   = -m64
ARCH_KFLAGS   = -m64 -mcmodel=kernel -mno-red-zone -mno-mmx -mno-sse -mno-sse2 -fno-asynchronous-unwind-tables
ARCH_KLDFLAGS = -m elf_x86_64 -z max-page-size=0x1000
else
ARCH_AFLAGS   = -m32
ARCH_KFLAGS   = -m32 -mno-sse -mno-sse2
ARCH_KLDFLAGS = -m elf_i386
endif
CFLAGS  = $(ARCH_KFLAGS) -g -Wall -Wextra -Wdeclaration-after-statement $(KWARN) $(WERROR) -ansi -nostdlib -nostartfiles -nodefaultlibs $(DEPFLAGS)
# -z noseparate-code: newer binutils split read-only data into a third
# PT_LOAD by default; the loader and elfcheck take the two-segment layout.
LDFLAGS = $(ARCH_KLDFLAGS) -z noseparate-code -T $(LINKER_SCRIPT).ld $(LDEXTRA)
# Linker flags an analysis adds from the command line (tools/verify/analyze.sh).
LDEXTRA ?=

# ACPICA is compiled as C89 with the architecture's flags and -Wall, not the
# kernel's whole warning set, under which its flexible array members, unnamed
# unions and unused parameters give thousands of warnings. __RACH2__ makes
# its acenv.h take the kernel's platform header, sys/acpi/acrach2.h. A
# sanitizer the caller asks for is the kernel's, and ACPICA, whose unaligned
# reads of the tables x86 permits, is left out of it. The kernel's own ACPI
# files include ACPICA's headers as system headers, so that its warnings stay
# out of the kernel's; ACPI_CFLAGS is their share, which the style checks
# that compile the kernel's files read through print-acpi-cflags.
ACPI_CFLAGS = -D__RACH2__ -Isys/acpi -isystem $(ACPICA)/include
ACPICA_CFLAGS = $(ARCH_KFLAGS) -g -Wall $(WERROR) -ansi -nostdlib -nostartfiles -nodefaultlibs \
                $(DEPFLAGS) -D__RACH2__ -Isys/acpi -I$(ACPICA)/include \
                $(filter-out -fsanitize%,$(KEXTRA))
$(ACPI_OBJECTS) : CFLAGS += $(ACPI_CFLAGS)

# Harness build (`make HARNESS=1`): when the init process, the probe a test
# harness runs, exits, the kernel writes its exit status to QEMU's
# isa-debug-exit device, and QEMU exits with it, so the harness reads the
# status as QEMU's exit code.
ifdef HARNESS
CFLAGS += -DRACH2_HARNESS
endif

# Flags the caller adds to every compile, which is how the other two kernels
# are built: `make -C rach2 COMPONENT=rach2-metal KEXTRA=-DRACH2_DISK_READONLY`
# for the kernel whose disk drivers issue no write (sys/block/diskcmd.c), and
# COMPONENT=rach2-recovery with KEXTRA=-DRACH2_RECOVERY for the recovery
# kernel. mk/build.mk puts each COMPONENT's objects and binary under a
# directory of its own, so no two kernels share an object. The read-only
# policy is a build switch rather than a boot option because a command line
# edited at the GRUB prompt cannot undo a build switch.
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
# next make to treat as current. The checker is the project ELF validator,
# tools/inspect/elfcheck, built with the host compiler by the top-level
# Makefile before each link, so a changed checker is rebuilt first.
#
# The kernel every loader reads has no debug sections. They are more than half
# of the linked file, which with them would nearly fill the 4 MiB kernel slot
# of a boot partition (tools/image/mkdisk_grub.sh). The linked file with them
# is kept beside the kernel as `kernel.debug`, which debug.sh loads into GDB.
# Both keep the symbol table, which the checker's `_end` test and `nm` read.
# The rule names this file among its inputs, so a change to how the kernel is
# made makes it again.
ELFCHECK = ../build/tools/inspect/elfcheck
# The checker takes the profile from the file: an ELF32 kernel loaded where
# its headers say, or the ELF64 x86-64 kernel loaded through the a.out
# kludge, whose copy must put every byte where the kernel is linked.
KERNEL_CHECK = $(ELFCHECK) --kernel

$(KERNEL_BIN) : $(ASM_OBJECTS) $(C_OBJECTS) $(ACPICA_OBJECTS) $(LINKER_SCRIPT).ld Makefile
	@mkdir -p $(dir $@)
	@$(MAKE) --no-print-directory -C .. build/tools/inspect/elfcheck
	@tmp="$@.tmp.$$$$"; trap 'rm -f "$$tmp" "$$tmp.debug"' EXIT; \
	echo '$(LD) $(LDFLAGS) -o $@.debug ...'; \
	$(LD) $(LDFLAGS) -o "$$tmp.debug" $(ASM_OBJECTS) $(C_OBJECTS) $(ACPICA_OBJECTS) && \
	$(CROSS_OBJCOPY) --strip-debug "$$tmp.debug" "$$tmp" && \
	$(KERNEL_CHECK) "$$tmp" && \
	mv "$$tmp.debug" $@.debug && \
	mv "$$tmp" $@

# The configuration fingerprint (mk/fingerprint.mk): make decides what to
# rebuild from timestamps, which say nothing about compiler flags, so building
# one tree twice with different switches would link some objects from each and
# could produce a kernel that reports one disk policy and enforces the other. A
# changed configuration, such as -DRACH2_DISK_READONLY reaching the wrong tree,
# removes the object tree before any object is compiled, so the component is
# rebuilt from nothing and never mixes two option sets. The kernels of the two
# policies are built in two trees (COMPONENT=rach2-metal for the read-only
# one).
CONFIG_TEXT = $(CC) $(CFLAGS) $(ACPICA_CFLAGS) $(LD) $(LDFLAGS)
include ../mk/fingerprint.mk
$(ASM_OBJECTS) $(C_OBJECTS) $(ACPICA_OBJECTS): $(CONFIG)


$(ASM_OBJECTS) : $(BUILD)/%.o : %.s
	@mkdir -p $(dir $@)
	$(CC) $(ARCH_AFLAGS) -g -c -o $@ $<

$(C_OBJECTS) : $(BUILD)/%.o : %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -o $@ -c $<

# The extraction runs when the tarball, the patch or the script is newer than
# its stamp, and leaves the tree alone when its stamp names the same three;
# the sources are made by it, and wait for it, but are not remade each time
# the stamp moves, so an object is recompiled only for a source extracted
# anew.
$(ACPICA_STAMP) : ../vendor/acpica/build_acpica.sh ../vendor/acpica/rach2-acenv.patch \
                  ../vendor/acpica/acpica-unix-20260930.tar.gz ../mk/arch.sh
	@cd .. && RACH2_ARCH=$(ARCH) sh vendor/acpica/build_acpica.sh

$(ACPICA_C_SRC) : | $(ACPICA_STAMP) ;

# The extracted core alone, for tools/verify/guestcheck.c.
.PHONY : acpica
acpica : $(ACPICA_STAMP)

$(ACPI_OBJECTS) : | $(ACPICA_STAMP)

$(ACPICA_OBJECTS) : $(BUILD)/acpica/%.o : $(ACPICA)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(ACPICA_CFLAGS) -o $@ -c $<

# Pull in the per-object header dependencies GCC wrote (-MMD), so editing a
# header rebuilds exactly the objects that include it. Missing on a first build
# (no .d yet): the leading '-' makes that silent. They are beside the objects
# in $(BUILD).
-include $(C_OBJECTS:.o=.d) $(ACPICA_OBJECTS:.o=.d)

# The shared code held to what every architecture can build
# (tools/verify/archcheck.sh): compiled for 32-bit and 64-bit ARM by clang's
# front end, with no architecture's or platform's header reached and no
# assembly. Nothing is linked, and no object is written.
.PHONY : check-arch
check-arch :
	@sh ../tools/verify/archcheck.sh $(C_SRC_SHARED) -- $(ARCH_C_SRC) $(PC_C_SRC)

# Print the kernel's C source list, expanded, and nothing else, for the tools
# that read it, such as tools/verify/check_sources.sh, which fails when a
# kernel .c on disk is missing from C_SRC. .PHONY so it always runs.
.PHONY : print-c-src
print-c-src :
	@echo $(C_SRC)

# ACPICA's list, by its paths in the core, for the same guard.
.PHONY : print-acpica-src
print-acpica-src :
	@echo $(ACPICA_C_SRC:$(ACPICA)/%=%)

# The flags the kernel's ACPI files take beside the kernel's, for the checks
# that compile the files of print-c-src (tests/cases/host/style.sh's clang
# and tools/verify/usercheck.py), relative to this directory.
.PHONY : print-acpi-cflags
print-acpi-cflags :
	@echo $(ACPI_CFLAGS)

# The assembly list too, for tools/verify/guestcheck.c (the guest build's list
# guard): the guest makefile must name exactly these sources.
.PHONY : print-asm-src
print-asm-src :
	@echo $(ASM_SRC)

.PHONY : clean
clean :
	rm -rf $(BUILD)
