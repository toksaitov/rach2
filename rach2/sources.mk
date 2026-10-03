# rach2/sources.mk: the one kernel source list, read by the host Makefile and
# by guest.mk under pdpmake on Rach2. Both derive their object lists from
# ASM_SRC and C_SRC, so the host and guest builds cannot drift apart.
#
# C_SRC_SHARED is the code every architecture compiles. The architecture's
# own list, sys/arch/$(ARCH)/sources.mk, gives ARCH_ASM_SRC and ARCH_C_SRC:
# its processor's files and its platform's, the drivers of devices only that
# platform has among them. Order matters: the link keeps input order, and the
# multiboot header, first in ARCH_ASM_SRC, must stay within the first 8192
# bytes of the kernel where loaders search.

include sys/arch/$(ARCH)/sources.mk

ASM_SRC = $(ARCH_ASM_SRC)

C_SRC = $(C_SRC_SHARED) $(ARCH_C_SRC)

C_SRC_SHARED = kernel.c \
    sys/kio.c \
    sys/klog.c \
    sys/syscall.c \
    sys/syscall_proc.c \
    sys/syscall_fs.c \
    sys/syscall_net.c \
    sys/cmdline.c \
    sys/input.c \
    sys/panic.c \
    sys/block/block.c \
    sys/block/chain.c \
    sys/block/loop.c \
    sys/block/request.c \
    sys/block/lag.c \
    sys/fs/bcache.c \
    sys/fs/vfs.c \
    sys/fs/fat32.c \
    sys/fs/fat32name.c \
    sys/fs/fatcommon.c \
    sys/fs/exfat.c \
    sys/fs/ext2.c \
    sys/block/mbr.c \
    sys/block/gpt.c \
    sys/block/usbdisk.c \
    sys/block/diskcmd.c \
    sys/block/recovery.c \
    sys/block/diskinfo.c \
    sys/block/disk.c \
    sys/block/partition.c \
    sys/block/ahci.c \
    sys/audio/audio.c \
    sys/audio/audio_debug.c \
    sys/audio/hda.c \
    sys/mm/heap.c \
    sys/mm/paging.c \
    sys/mm/kheap.c \
    sys/mm/pressure.c \
    sys/latency.c \
    sys/mm/usercopy.c \
    sys/mm/vm.c \
    sys/mm/mm.c \
    sys/mm/filemap.c \
    sys/mm/shmem.c \
    sys/mm/filecache.c \
    sys/mm/frame.c \
    sys/mm/pagecache.c \
    sys/mm/modlayout.c \
    sys/proc/process.c \
    sys/proc/exit.c \
    sys/proc/sched.c \
    sys/proc/suspend.c \
    sys/proc/wait_queue.c \
    sys/proc/futex.c \
    sys/proc/ptrace.c \
    sys/proc/seccomp.c \
    sys/proc/ns.c \
    sys/proc/coredump.c \
    sys/ipc/pipe.c \
    sys/ipc/pipe_ring.c \
    sys/ipc/unix.c \
    sys/ipc/sysv.c \
    sys/ipc/mqueue.c \
    sys/io/poll.c \
    sys/io/poll_select.c \
    sys/io/eventfd.c \
    sys/io/inotify.c \
    sys/io/epoll.c \
    sys/io/timerfd.c \
    sys/tty/n_tty.c \
    sys/tty/tty.c \
    sys/tty/pty.c \
    sys/proc/elf.c \
    sys/proc/uimage.c \
    sys/fs/krffs.c \
    sys/fs/krffsmount.c \
    sys/fs/fd.c \
    sys/fs/lock.c \
    sys/fs/tmpfs.c \
    sys/fs/pathbuf.c \
    sys/fs/devfs.c \
    sys/fs/synthfs.c \
    sys/fs/procfs.c \
    sys/fs/cpuinfo.c \
    sys/usb/usb.c \
    sys/usb/stamp.c \
    sys/usb/xhci.c \
    sys/usb/core.c \
    sys/usb/hid.c \
    sys/usb/msc.c \
    sys/usb/hub.c \
    sys/network/netconf.c \
    sys/network/netif.c \
    sys/network/route.c \
    sys/network/netconsole.c \
    sys/network/core/net_core.c \
    sys/network/driver/netdev.c \
    sys/network/driver/e1000.c \
    sys/network/driver/rtl8168.c \
    sys/network/ethernet/ethernet.c \
    sys/network/arp/arp.c \
    sys/network/ip/ip.c \
    sys/network/ip/icmp.c \
    sys/network/udp/udp.c \
    sys/network/tcp/tcp.c \
    sys/network/tcp/rcvwnd.c \
    sys/network/socket/socket.c \
    sys/network/socket/netlink.c \
    sys/network/socket/packet.c \
    sys/network/socket/raw.c \
    sys/lib/kctypes.c \
    sys/lib/kfmt.c \
    sys/lib/kstd.c \
    sys/lib/kstring.c \
    sys/lib/krandom.c
