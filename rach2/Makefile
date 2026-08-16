# Constants

LOADER = loader
KERNEL = kernel
KERNEL_BIN = $(BUILD)/$(KERNEL)

LINKER_SCRIPT = link

ASM_SRC = $(LOADER).s          \
	  sys/isr_handler.s        \
	  sys/tables/gdt_handler.s \
	  sys/tables/idt_handler.s \
	  sys/proc/switch.s

C_SRC   = $(KERNEL).c \
	  sys/kio.c       \
	  sys/serial.c    \
	  sys/syscall.c   \
	  sys/module.c    \
	  sys/cmdline.c   \
	  sys/isr.c       \
	  sys/pic.c       \
	  sys/pit.c       \
	  sys/rtc.c       \
	  sys/kbd.c       \
	  sys/mouse.c     \
	  sys/panic.c     \
	  sys/speaker.c   \
	  sys/pci.c       \
	  sys/power.c     \
	  sys/block/block.c     \
	  sys/block/mbr.c       \
	  sys/block/ata.c       \
	  sys/block/ahci.c      \
	  sys/display/vga_text.c \
	  sys/display/vga_gfx.c  \
	  sys/audio/audio.c     \
	  sys/audio/audio_debug.c \
	  sys/audio/dma.c       \
	  sys/audio/sb16.c      \
	  sys/audio/hda.c       \
	  sys/tables/gdt.c      \
	  sys/tables/tss.c      \
	  sys/tables/idt.c      \
	  sys/mm/heap.c         \
	  sys/mm/paging.c       \
	  sys/mm/usercopy.c     \
	  sys/mm/vm.c           \
	  sys/mm/frame.c        \
	  sys/mm/pagecache.c    \
	  sys/mm/modlayout.c    \
	  sys/diag.c            \
	  sys/udelay.c          \
	  sys/proc/process.c    \
	  sys/proc/sched.c      \
	  sys/proc/wait_queue.c \
	  sys/ipc/pipe.c        \
	  sys/ipc/pipe_ring.c   \
	  sys/io/poll.c         \
	  sys/io/poll_select.c  \
	  sys/tty/n_tty.c       \
	  sys/tty/tty.c         \
	  sys/tty/pty.c         \
	  sys/proc/elf.c        \
	  sys/proc/uimage.c     \
	  sys/proc/tls.c        \
	  sys/fs/krffs.c        \
	  sys/fs/fd.c           \
	  sys/fs/devfs.c        \
	  sys/fs/synthfs.c      \
	  sys/fs/procfs.c       \
	  sys/network/netconsole.c \
	  sys/network/core/net_core.c \
	  sys/network/driver/netdev.c \
	  sys/network/driver/rtl8139.c \
	  sys/network/driver/rtl8168.c \
	  sys/network/ethernet/ethernet.c \
	  sys/network/arp/arp.c \
	  sys/network/ip/ip.c \
	  sys/network/ip/icmp.c \
	  sys/network/udp/udp.c \
	  sys/network/tcp/tcp.c \
	  sys/network/socket/socket.c \
	  sys/lib/kctypes.c     \
	  sys/lib/kstd.c        \
	  sys/lib/kstring.c     \
	  sys/lib/krandom.c

# Everything builds out of tree under $(BUILD) (build/rach2/, from mk/build.mk): the
# objects, the GCC-written .d files, the kernel binary, and the ISO staging, so the
# source tree never holds a build product.
ASM_OBJECTS = $(ASM_SRC:%.s=$(BUILD)/%.o)
C_OBJECTS   = $(C_SRC:%.c=$(BUILD)/%.o)

# Cross toolchain + automatic header-dependency tracking (shared; see ../mk/).
include ../mk/toolchain.mk
include ../mk/deps.mk
include ../mk/build.mk
CC := $(CROSS_CC)
# LD comes from CROSS_LD (mk/toolchain.mk): it honors an explicit `CROSS_LD=...`
# override and otherwise tracks CROSS_CC; and CROSS_CC itself absorbs a
# command-line `make CC=...`, so both the CC and CROSS_LD knobs reach the linker.
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

# General Rules

.PHONY : all
all : $(KERNEL_BIN)

# Bootable ISO images are built per RAM-disk image at the top level (make isos; see
# tools/mkiso.sh), so this Makefile only builds the kernel.

# Kernel

# The structural check runs on every link: a kernel the loaders would refuse,
# or one whose _end does not equal the image end, fails the build here rather
# than at a boot later. The
# link lands in a temporary file that is renamed only after the check passes,
# so a failed check cannot leave a newer kernel behind for the next make to
# treat as current.
$(KERNEL_BIN) : $(ASM_OBJECTS) $(C_OBJECTS)
	@mkdir -p $(dir $@)
	@tmp="$@.tmp.$$$$"; trap 'rm -f "$$tmp"' EXIT; \
	echo '$(LD) $(LDFLAGS) -o $@ ...'; \
	$(LD) $(LDFLAGS) -o "$$tmp" $^ && \
	python3 ../tools/check_kernel.py "$$tmp" && \
	mv "$$tmp" $@

$(ASM_OBJECTS) : $(BUILD)/%.o : %.s
	@mkdir -p $(dir $@)
	$(CC) -m32 -g -c -o $@ $<

$(C_OBJECTS) : $(BUILD)/%.o : %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -o $@ -c $<

# Pull in the per-object header dependencies GCC wrote (-MMD), so editing a header
# rebuilds exactly the objects that include it. Missing on a first build (no .d
# yet): the leading '-' makes that silent. They live beside the objects in $(BUILD).
-include $(C_OBJECTS:.o=.d)

# Emit the explicit kernel source list (expanded), for tools/check_sources.sh (the
# source-list guard that fails if a rach2 .c on disk is missing from C_SRC). .PHONY so
# it always runs; prints nothing but the list.
.PHONY : print-c-src
print-c-src :
	@echo $(C_SRC)

# The assembly list too, for tools/check_guest.py (the guest build's list
# guard): the guest makefile must name exactly these sources.
.PHONY : print-asm-src
print-asm-src :
	@echo $(ASM_SRC)

.PHONY : clean
clean :
	rm -rf $(BUILD)
