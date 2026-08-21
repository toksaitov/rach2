# Build the Rach2 kernel on Rach2 itself.
#
# This is the POSIX makefile pdpmake reads in the guest, driving the staged
# tcc through its /bin/cc name; the GNU Makefile beside it stays the host
# build and pdpmake cannot read it. The object list below must equal the
# host Makefile's ASM_SRC plus C_SRC; tools/check_guest.py has the two
# lists equal on the host, so they cannot drift apart.
#
# The link reads the object list from kernel.objs, a response file, because
# naming 87 objects on one command line would exceed the machine's 63-entry
# execve limit. The file is written whole, loader.o first: the multiboot
# header lives in loader.o and the linker keeps input order, so the header
# stays inside the first 8192 bytes of the kernel where the loaders search
# for it. /lib/tcc/libtcc1.a supplies the 64-bit helper routines tcc calls
# where gcc emits the arithmetic inline; -nostdlib keeps the C library and
# its startup files out of a kernel that must not have them.
#
# Run from a writable copy of this tree: make -f guest.mk

KCC = cc
KCFLAGS = -m32 -Wall -Wunsupported -nostdlib

OBJS = loader.o \
       sys/isr_handler.o \
       sys/tables/gdt_handler.o \
       sys/tables/idt_handler.o \
       sys/proc/switch.o \
       kernel.o \
       sys/kio.o \
       sys/serial.o \
       sys/syscall.o \
       sys/module.o \
       sys/cmdline.o \
       sys/isr.o \
       sys/pic.o \
       sys/board.o \
       sys/bootlog.o \
       sys/pit.o \
       sys/rtc.o \
       sys/kbd.o \
       sys/mouse.o \
       sys/panic.o \
       sys/speaker.o \
       sys/pci.o \
       sys/power.o \
       sys/block/block.o \
       sys/block/mbr.o \
       sys/block/ata.o \
       sys/block/diskcmd.o \
       sys/block/recovery.o \
       sys/block/diskinfo.o \
       sys/block/ahci.o \
       sys/display/vga_text.o \
       sys/display/vga_gfx.o \
       sys/audio/audio.o \
       sys/audio/audio_debug.o \
       sys/audio/dma.o \
       sys/audio/sb16.o \
       sys/audio/hda.o \
       sys/tables/gdt.o \
       sys/tables/tss.o \
       sys/tables/idt.o \
       sys/mm/heap.o \
       sys/mm/paging.o \
       sys/mm/usercopy.o \
       sys/mm/vm.o \
       sys/mm/frame.o \
       sys/mm/pagecache.o \
       sys/mm/modlayout.o \
       sys/diag.o \
       sys/udelay.o \
       sys/proc/process.o \
       sys/proc/sched.o \
       sys/proc/wait_queue.o \
       sys/ipc/pipe.o \
       sys/ipc/pipe_ring.o \
       sys/io/poll.o \
       sys/io/poll_select.o \
       sys/tty/n_tty.o \
       sys/tty/tty.o \
       sys/tty/pty.o \
       sys/proc/elf.o \
       sys/proc/uimage.o \
       sys/proc/tls.o \
       sys/fs/krffs.o \
       sys/fs/fd.o \
       sys/fs/devfs.o \
       sys/fs/synthfs.o \
       sys/fs/procfs.o \
       sys/network/netconf.o \
       sys/network/netif.o \
       sys/network/netconsole.o \
       sys/network/core/net_core.o \
       sys/network/driver/netdev.o \
       sys/network/driver/rtl8139.o \
       sys/network/driver/rtl8168.o \
       sys/network/ethernet/ethernet.o \
       sys/network/arp/arp.o \
       sys/network/ip/ip.o \
       sys/network/ip/icmp.o \
       sys/network/udp/udp.o \
       sys/network/tcp/tcp.o \
       sys/network/tcp/rcvwnd.o \
       sys/network/socket/socket.o \
       sys/network/socket/packet.o \
       sys/lib/kctypes.o \
       sys/lib/kstd.o \
       sys/lib/kstring.o \
       sys/lib/krandom.o

# The link lands in kernel.new and is renamed only on success, so a failed
# link cannot leave a newer partial kernel that the next make treats as
# current. The structural check runs on the host, after extraction.
kernel: $(OBJS) kernel.objs
	$(KCC) -static -nostdlib -o kernel.new @kernel.objs -Wl,-Ttext=0x100074 /lib/tcc/libtcc1.a
	mv kernel.new kernel

# Regenerated whenever this makefile changes, through a temporary file
# renamed only after every line landed: a printf that failed part way
# cannot leave a shorter list that a retry would treat as current.
kernel.objs: guest.mk
	printf '%s\n' loader.o sys/isr_handler.o sys/tables/gdt_handler.o sys/tables/idt_handler.o sys/proc/switch.o kernel.o > kernel.objs.new
	printf '%s\n' sys/kio.o sys/serial.o sys/syscall.o sys/module.o sys/cmdline.o sys/isr.o >> kernel.objs.new
	printf '%s\n' sys/pic.o sys/board.o sys/bootlog.o sys/pit.o sys/rtc.o sys/kbd.o sys/mouse.o sys/panic.o >> kernel.objs.new
	printf '%s\n' sys/speaker.o sys/pci.o sys/power.o sys/block/block.o sys/block/mbr.o sys/block/ata.o sys/block/diskcmd.o sys/block/recovery.o sys/block/diskinfo.o sys/block/ahci.o >> kernel.objs.new
	printf '%s\n' sys/display/vga_text.o sys/display/vga_gfx.o sys/audio/audio.o sys/audio/audio_debug.o sys/audio/dma.o sys/audio/sb16.o >> kernel.objs.new
	printf '%s\n' sys/audio/hda.o sys/tables/gdt.o sys/tables/tss.o sys/tables/idt.o sys/mm/heap.o sys/mm/paging.o >> kernel.objs.new
	printf '%s\n' sys/mm/usercopy.o sys/mm/vm.o sys/mm/frame.o sys/mm/pagecache.o sys/mm/modlayout.o sys/diag.o sys/udelay.o sys/proc/process.o sys/proc/sched.o >> kernel.objs.new
	printf '%s\n' sys/proc/wait_queue.o sys/ipc/pipe.o sys/ipc/pipe_ring.o sys/io/poll.o sys/io/poll_select.o sys/tty/n_tty.o >> kernel.objs.new
	printf '%s\n' sys/tty/tty.o sys/tty/pty.o sys/proc/elf.o sys/proc/uimage.o sys/proc/tls.o sys/fs/krffs.o >> kernel.objs.new
	printf '%s\n' sys/fs/fd.o sys/fs/devfs.o sys/fs/synthfs.o sys/fs/procfs.o sys/network/netconf.o sys/network/netif.o sys/network/netconsole.o sys/network/core/net_core.o sys/network/driver/netdev.o sys/network/driver/rtl8139.o sys/network/driver/rtl8168.o >> kernel.objs.new
	printf '%s\n' sys/network/ethernet/ethernet.o sys/network/arp/arp.o sys/network/ip/ip.o sys/network/ip/icmp.o sys/network/udp/udp.o sys/network/tcp/tcp.o sys/network/tcp/rcvwnd.o >> kernel.objs.new
	printf '%s\n' sys/network/socket/socket.o sys/network/socket/packet.o sys/lib/kctypes.o sys/lib/kstd.o sys/lib/kstring.o sys/lib/krandom.o >> kernel.objs.new
	mv kernel.objs.new kernel.objs

.SUFFIXES: .o .c .s

.c.o:
	$(KCC) $(KCFLAGS) -c -o $@ $<

.s.o:
	$(KCC) -c -o $@ $<

clean:
	rm -f $(OBJS) kernel.objs kernel.objs.new kernel.new kernel
