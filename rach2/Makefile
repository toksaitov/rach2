# Constants

BIN_DIR = bin
ISO_DIR = iso

LOADER = loader
KERNEL = kernel

LINKER_SCRIPT = link

ISO_IMAGE = $(BIN_DIR)/boot.iso

# RAM disk (optional, from krffs/)
RAMDISK_SRC = ../krffs/file_system.krffs
RAMDISK_DST = $(ISO_DIR)/boot/file_system.krffs
GRUB_CFG = $(ISO_DIR)/boot/grub/grub.cfg

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
	  sys/isr.c       \
	  sys/pic.c       \
	  sys/pit.c       \
	  sys/kbd.c       \
	  sys/mouse.c     \
	  sys/panic.c     \
	  sys/speaker.c   \
	  sys/pci.c       \
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
	  sys/network/core/net_core.c \
	  sys/network/driver/rtl8139.c \
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

# Out-of-tree intermediates: objects + the GCC-written .d files live under .build/
# (git-ignored), so the source tree stays clean. The final kernel binary stays in
# place (rach2/kernel) for the run scripts and the ISO/RAM-disk steps.
BUILD = .build

ASM_OBJECTS = $(ASM_SRC:%.s=$(BUILD)/%.o)
C_OBJECTS   = $(C_SRC:%.c=$(BUILD)/%.o)

# Cross toolchain + automatic header-dependency tracking (shared; see ../mk/).
include ../mk/toolchain.mk
include ../mk/deps.mk
CC := $(CROSS_CC)
# LD comes from CROSS_LD (mk/toolchain.mk): it honors an explicit `CROSS_LD=...`
# override and otherwise tracks CROSS_CC -- and CROSS_CC itself absorbs a
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
all : $(KERNEL)

# ISO Image Creation (requires Docker)

.PHONY : iso
iso : $(ISO_IMAGE)

$(ISO_IMAGE) : $(KERNEL)
	@mkdir -p $(BIN_DIR)
	@# Copy without creating sparse files (some GRUB setups crash on sparse modules)
	@cat $(KERNEL) > $(ISO_DIR)/boot/$(KERNEL)
	@# Generate grub.cfg with or without ramdisk module
	@if [ -f $(RAMDISK_SRC) ]; then \
		cat $(RAMDISK_SRC) > $(RAMDISK_DST); \
		echo "set timeout=3" > $(GRUB_CFG); \
		echo "set default=0" >> $(GRUB_CFG); \
		echo "" >> $(GRUB_CFG); \
		echo "menuentry \"Rach2 Kernel (RAM disk)\" {" >> $(GRUB_CFG); \
		echo "    multiboot /boot/kernel" >> $(GRUB_CFG); \
		echo "    module /boot/file_system.krffs" >> $(GRUB_CFG); \
		echo "    boot" >> $(GRUB_CFG); \
		echo "}" >> $(GRUB_CFG); \
		echo "" >> $(GRUB_CFG); \
		echo "menuentry \"Rach2 Kernel (no RAM disk)\" {" >> $(GRUB_CFG); \
		echo "    multiboot /boot/kernel" >> $(GRUB_CFG); \
		echo "    boot" >> $(GRUB_CFG); \
		echo "}" >> $(GRUB_CFG); \
		echo "Including RAM disk: $(RAMDISK_SRC)"; \
	else \
		echo "set timeout=3" > $(GRUB_CFG); \
		echo "set default=0" >> $(GRUB_CFG); \
		echo "" >> $(GRUB_CFG); \
		echo "menuentry \"Rach2 Kernel\" {" >> $(GRUB_CFG); \
		echo "    multiboot /boot/kernel" >> $(GRUB_CFG); \
		echo "    boot" >> $(GRUB_CFG); \
		echo "}" >> $(GRUB_CFG); \
		echo "No RAM disk found (build with krffs/build_ramdisk.sh first)"; \
	fi
	@docker run --rm --platform linux/amd64 -v "$(CURDIR)":/work -w /work ubuntu:24.04 sh -c \
		"apt-get update -qq && \
		 apt-get install -y -qq grub-pc-bin grub-common xorriso mtools >/dev/null 2>&1 && \
		 grub-mkrescue -o $(ISO_IMAGE) $(ISO_DIR) 2>/dev/null"
	@rm -f $(ISO_DIR)/boot/$(KERNEL) $(RAMDISK_DST)
	@echo "Created $(ISO_IMAGE)"

# Kernel

$(KERNEL) : $(ASM_OBJECTS) $(C_OBJECTS)
	$(LD) $(LDFLAGS) -o $@ $^

$(ASM_OBJECTS) : $(BUILD)/%.o : %.s
	@mkdir -p $(dir $@)
	$(CC) -m32 -g -c -o $@ $<

$(C_OBJECTS) : $(BUILD)/%.o : %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -o $@ -c $<

# Pull in the per-object header dependencies GCC wrote (-MMD), so editing a header
# rebuilds exactly the objects that include it. Missing on a first build (no .d
# yet) -- the leading '-' makes that silent. They live beside the objects in .build/.
-include $(C_OBJECTS:.o=.d)

# Emit the explicit kernel source list (expanded), for tools/check_sources.sh -- the
# source-list guard that fails if a rach2 .c on disk is missing from C_SRC. .PHONY so
# it always runs; prints nothing but the list.
.PHONY : print-c-src
print-c-src :
	@echo $(C_SRC)

.PHONY : clean
clean :
	rm -rf $(BUILD)
	rm -f $(KERNEL) $(ISO_IMAGE) $(ISO_DIR)/boot/$(KERNEL) $(RAMDISK_DST) $(GRUB_CFG)
	@# Belt-and-suspenders: drop any stray in-tree objects/deps from an older build.
	@find . \( -name '*.o' -o -name '*.d' \) -type f -exec rm -f {} +
