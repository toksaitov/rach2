# The bounds

Every capacity limit the kernel, the tools, the image builders and the procedures still impose, with its class and its reason, gathered in phase 8 of milestone 53 from the named constants (`MAX`, `LIMIT`, `CAP`, `BOUND`, `DEPTH`, `SLOTS`, `ENTRIES`, `COUNT`, `SIZE`) and the numbers the build fixes. Register offsets, bit values, message codes and protocol field widths are not bounds and are left out. The classes:

* physical: what the hardware or the machine has; the number is the device's.
* format: what an on-disk format, a wire protocol or the Linux ABI fixes; the number is the specification's.
* policy: a choice this kernel makes, with the option or file that changes it where one exists.
* safety: an invariant that protects memory or a table; exceeding it is refused by name.

A bound that fits none of these is unfinished work and an entry in the shortcomings list. None was found. Two policy numbers are low enough to name for the owner: `CWD_MAX`, the 64 bytes a working directory's path may have, and `VFS_COVERED_PATH_MAX`, the 64 a mount point's path may have.

## The kernel: memory and processes

* `HARD_MEMORY_LIMIT` 32 MiB and `FRAME_DIRECT_MAP_LIMIT` (`mm/paging.h`, `mm/frame.h`): safety. The identity-mapped window; a frame above it is reached only through a mapping in the kernel's layout, and `init_paging` checks the two agree.
* `KVA_HEAP_SIZE` 256 MiB, `KVA_MMIO_SIZE` 128 MiB, `KVA_MODULES_SIZE` 64 MiB, `KVA_TEMP_SLOTS` 1024 (`mm/paging.h`): policy. The kernel's virtual layout above 3 GiB; the temporary window is the whole of its 4 MiB region, and a caller that finds no slot is told so.
* `USER_ADDR_MAX` 3 GiB and `ELF_USER_MAX` (`mm/usercopy.h`, `proc/elf.c`): format. The user half of the address space the ABI gives a process; the loader refuses a segment past it.
* `FRAME_REF_MAX` (`mm/paging.h`): safety. The reference words at and above it are states, not counts.
* `PC_SLOTS` 2048 and `PC_WINDOW_CAP` 512 MiB (`mm/pagecache.h`, `mm/pagecache.c`): policy. The page cache's slot table, and the default cap on its window when no device register block lies below it, the ceiling on a mounted disk file system, sized for a machine booting from its own disk and bounding the pre-mapped page tables to 128.
* `VMA_LIMIT` 65536, `VMA_FIRST` 16 and `VM_STACK_MAX_GROWTH` 8 MiB (`mm/vm.h`): policy. The mappings a process may hold, doubling from sixteen, and how far the main stack grows on demand; `tests/musl/vmamusl.c` takes three hundred.
* `KERNEL_STACK_SIZE` 16 KiB (`kernel.c`, `syscall_proc.c`): policy. A kernel stack per process, with `-Wframe-larger-than=4096` holding every frame to a quarter of it (phase 5).
* `USER_STACK_SIZE` 4 KiB (`kernel.c`): policy. The stack `enter_usermode` is given for the first process before its image's own.
* `PROC_COUNT_POLICY` 4096 (`proc/process.h`): policy. The processes one real user may have unless `prlimit64` or `/etc/rach2/limits` says otherwise; root is not limited, and the bound on processes is memory.
* `CWD_MAX` 64 (`proc/process.h`): policy. The working directory's path kept in the record; a deeper directory cannot be made current, ENAMETOOLONG. Named for the owner: it is the lowest path bound in the kernel.
* `NGROUPS_MAX` 8 (`proc/process.h`): format. The 4.2BSD number, which `sysconf` reports.
* `RLIM_COUNT` 3 (`proc/process.h`): format. The three resource limits this kernel keeps of the ABI's set.
* `KSIG_MAX` 64 (`lib/ksignal.h`): format. Signals 1 to 64, the Linux ABI.
* `FD_TABLE_LIMIT` 1024 (`fs/fd.h`): policy. A process's descriptors, doubling from `FD_TABLE_FIRST`; `RLIMIT_NOFILE` reports it. `POLL_MAX_FDS` follows it, and `IOV_MAX_LOCAL` 1024 is the vector count `readv`, `writev` and `sendmsg` accept, the Linux `IOV_MAX`.
* `EXEC_MAX_ENTRIES` 63, `EXEC_MAX_STRINGS` 3072, `EXEC_SHEBANG_MAX` 128, `UIMAGE_MAX_ARGS` 64 (`syscall_proc.c`, `proc/uimage.h`): policy. The argument vector an exec carries in its workspace, sized so a compiler's longest command line passes; `tests/musl/execargmusl.c` holds both numbers.
* `ELF_MAX_PHNUM` 32 and `ELF_MAX_SEGS` 8 (`proc/elf.h`, and `tools/inspect/elfcheck.c` the same): safety. The program header buffer and the segments recorded per image; a static musl binary has under ten of each.
* `KCMDLINE_MAX` 256 (`cmdline.c`): policy. The kernel command line kept, long enough for any GRUB or QEMU line here; a longer one is truncated, which at worst drops a trailing flag.
* `MAX_MODULES` 8 (`module.h`): policy. The boot modules recorded; a boot here passes one or two, and what a loader passes past eight is left unread with a record saying how many.
* `ISR_MAX_VECTOR_HANDLERS` 4 (`isr.h`): policy. The handlers one vector's chain holds; a fifth registration is refused by name.
* `GDT_ENTRY_COUNT` 7 (`tables/gdt.h`): format. The descriptors this kernel's segmentation uses.
* `SYSCALL_TABLE_SIZE` 356 (`syscall.h`): format. One past the highest Linux i386 number the table registers.

## The kernel: files and file systems

* `VFS_NAME_MAX` 255, `VFS_LINK_MAX` 40, `VFS_WALK_PATH_MAX` 512, `VFS_COVERED_PATH_MAX` 64 (`fs/vfs.h`): the first two are format, the Linux ABI's `NAME_MAX` and `MAXSYMLINKS`; the buffer a followed link's target is joined in and the path a mount may cover are policy, ENAMETOOLONG past them.
* `KRFFS_PATH_MAX_DEPTH` 64, `PATH_DEPTH` 64 in ext2 (`fs/krffs.h`, `fs/ext2.c`): policy. Directories climbed when a path is rebuilt from a node, ENAMETOOLONG past them.
* `KRFFS_FIELD_SIZE` 325, `KRFFS_FIELD_DATA_SIZE` 341, `KRFFS_FILE_NAME_BUFFER_SIZE` 256 (`fs/krffs.h`): format. The KRFFS node layout.
* `EXT2_NAME_MAX` 255, `EXT2_FT_MAX` 7 (`fs/ext2.c`, and `setidcheck.c` the same): format. The ext2 directory entry.
* `EXT2_CHECK_DEPTH` 32, exFAT's `CHECK_DEPTH` 32, FAT's `WALK_DEPTH` 32, `WALK_TAILS` 16 and `WALK_DIR_ENTRIES` 4096 (`fs/ext2.c`, `fs/exfat.c`, `fs/fat32.c`): policy. The depth and the breadth the mount-time check walks; a volume beyond them mounts read-only with a record naming the reason.
* `FAT_MAX_CLUSTERS`, `LFN_MAX_ENTRIES` 20 (`fs/fat32.c`): format. The FAT32 cluster numbers below the reserved range, and the long name entries that cover 255 code units.
* `DIR_MAX_BYTES` 256 MiB, `EX_FILE_LIMIT`, `SET_MAX` 19 (`fs/exfat.c`): format. The exFAT specification's directory size, file size and entry set.
* `BCACHE_HOLD_MAX` 4, `BCACHE_MIN_BUFFERS` 16, `BCACHE_BLOCK_SECTORS` 8 (`fs/bcache.h`): policy. Blocks one caller holds at once, the least the cache keeps, and the block a cache entry covers.
* `SYNTH_MAXC` 5 (`fs/synthfs.c`): policy. The components a path under `/dev` or `/proc` is split into.
* `PROCFS_MAX_DISK` (`fs/procfs.c`): follows `DISK_MAX`.
* `LOOP_PATH_MAX` 64 (`block/loop.h`): policy. The longest backing path a line written to `/dev/loop-control` may name; a longer write is refused.
* `STATFS_BSIZE` 1024 (`syscall_fs.c`): format. The block size `statfs` reports for the root image.

## The kernel: block devices

* `DISK_MAX` 8, `USBDISK_MAX` 8, `MSC_MAX_DEVICES` 8, `MSC_MAX_LUNS` 4, `PART_MAX` 15 (`block/disk.h`, `block/usbdisk.h`, `usb/msc.h`, `block/partition.h`): policy. Disks and sticks named at once, logical units of one reader, and the partitions of one disk that get nodes, which is Linux's fifteen; a table with more has the rest counted in `refused`, and no minor is above the range.
* `MBR_PART_COUNT` 4, `GPT_PART_COUNT` 4, `GPT_MAX_ENTRIES` 128, `GPT_MAX_ARRAY_BYTES` 16 KiB, `GPT_HEADER_MAX` 512 (`block/mbr.h`, `block/gpt.h`, `block/gpt.c`): format. The two partition tables as their specifications lay them out; `GPT_PART_COUNT` is the entries the kernel keeps of the 128 it reads.
* `BLOCK_REQ_MAX_SECTORS` 2048, `AHCI_MAX_SECTORS_PER_CMD` 8192, `ATA_MAX_SECTORS_PER_CMD` 256, `AHCI_PRDT_MAX` (`block/block.h`, `block/ahci.c`, `block/ata.c`): physical. The AHCI command table's PRDT of 4 MiB entries, the ATA sector count register's 256, and the request size each driver reports through `block_max_sectors`.
* `BLOCK_MAX_DEPTH` 1 (`block/block.h`): policy. One loop device may stack on a disk; a chain that would need more requests than the pool holds is refused where it is attached.
* `REQ_LIMIT` 16, `REQ_DEFAULT_DEADLINE` 3 s, `REQ_DEFAULT_DEADLINES` 4, `REQ_DEADLINE_MAX_S` 600 (`block/request.c`): policy. The requests live at once unless `rach2.blockpool` says otherwise, and a request's deadline and its retries, the most the option accepts.
* `ATA_POLL_LIMIT` 200000, `ATA_PAUSE_LOOPS`, `ATA_POLL_PLAIN` (`block/ata.c`): physical. The polls a legacy transfer waits before the disk is called gone.
* `DISKINFO_WINDOW_MAX_SECTORS` 2^32 (`block/diskinfo.h`): format. A window's sector count field.
* `DISK_SERIAL_MAX` 63, `MSC_SERIAL_MAX` 60, `SMBIOS_VENDOR_MAX` 64 (`block/disk.h`, `usb/msc.c`, `smbios.c`): format. The identify page's serial, the USB string descriptor's and the SMBIOS string's longest useful text.
* `BLOCK_SECTOR_SIZE` 512, `MSC_SECTOR_SIZE` 512 (`block/block.h`, `usb/msc.c`): format. The sector every driver here moves; a device reporting another size is refused by name.

## The kernel: USB

* `USB_MAX_DEVICES` 16, `HUB_MAX` 8, `XHCI_MAX_CONTROLLERS` 4, `XHCI_MAX_SCRATCHPADS` 512 (`usb/core.h`, `usb/hub.h`, `usb/xhci.h`): policy. The tables of records; a device, hub or controller past them is refused by name, and the notebooks here have two controllers.
* `XHCI_SLOTS` 255, `XHCI_MAX_PORTS` 255, `XHCI_MAX_DCI` 32, `USB_HUB_PORT_MAX` 31, `USB_NO_SLOTS` (`usb/xhci.c`, `usb/xhci.h`, `usb/hcd.h`): format. The xHCI and USB specifications' field widths.
* `USB_DESC_MAX` 3072 (`usb/core.h`): safety. What is left of a frame for a configuration descriptor read whole; a longer configuration is refused by name.
* `HUB_POWER_MAX_MS` 510 (`usb/hub.c`): format. The descriptor's power-on time at its byte's most, 255 units of 2 ms.
* `SETTLE_LIMIT_MS` 5000, `CLOSE_LIMIT_MS` 5000 (`usb/core.c`): policy. How long a bouncing connect and a closing device are given before their entries go.

## The kernel: network

* `NETDEV_MAX` 4, `E1000_MAX` 4 (`network/driver/netdev.h`, `network/driver/e1000.c`): policy. Interfaces driven at once, named `eth0` upward and numbered from 1 since Linux reserves index 0.
* `E1000_RING_SIZE` 32, `R8168_RING_SIZE` 64, `E1000_MTA_ENTRIES` 128, `R8168_STORM_ENTRIES` 16, `R8168_COUNTERS_WAIT_STEPS` 1000 (`network/driver/*.h`, `rtl8168.c`): physical. Descriptor rings and tables as the cards take them; the RTL8168 ring is the one number the reference driver does not vouch for, measured on the laptop in milestone 24.
* `E1000_FRAME_MAX` 1514, `R8168_FRAME_MAX` 1514, `RTL_RX_PKT_MAX` 1518, `E1000_RX_BUF_SIZE` 2048, `E1000_TX_BUF_SIZE` 1536, `RTL_TX_BUF_SIZE` 1536, `R8168_RX_BUF_SIZE` 16383, `R8168_RX_MAX_SIZE` 16384 (`network/driver/*.h`): format. The Ethernet frame and the buffers the cards' descriptors address.
* `ARP_CACHE_SIZE` 8 (`network/arp/arp.h`): policy. Neighbours kept; a full cache replaces the one gone longest without use, and nothing is refused for its being full.
* `LOOP_RING_SLOTS` 64 (`network/ip/ip.c`): policy. The loopback ring, more than the 45 segments one window's worth of data puts in it in one system call; entry 50 of the shortcomings list measured its peak at 37.
* `PACKET_QUEUE_SLOTS` 8, `RAW_QUEUE_SLOTS` 8, `PACKET_SLOT_SIZE` 1500, `RAW_SLOT_SIZE` 1500, `SOCK_RECV_BUF_SIZE` 1500 (`network/socket/*`): policy for the queue depths, a reply behind a burst of other traffic while the reader is scheduled; format for the slot, one datagram of the Ethernet MTU.
* `TCP_RING_SIZE` 65536, `TCP_WINDOW_SIZE` 65535, `RCVWND_MAX` 65535 (`network/tcp/tcp.h`, `rcvwnd.c`): format. An unscaled TCP window is a 16-bit field; the ring is one past it so an empty ring never advertises zero.
* `TCP_RTO_MAX` 60 s, `TCP_MAX_RETX` 7, `TCP_2MSL_TICKS`, `TCP_FIN_WAIT2_TICKS` (`network/tcp/tcp.h`): policy. The retransmission ceiling and the timers, short because the network is local.
* `NETCONSOLE_PAYLOAD_MAX` 512, `IFREQ_NAMESIZE` 16 (`network/netconsole.h`, `network/netif.c`): format. One record per datagram, and the Linux `ifreq` name.

## The kernel: console, input, audio, log

* `N_TTY_LINE_MAX` 256, `N_TTY_READ_MAX` 4096, `TTY_ECHO_MAX` 64, `TTY_PUMP_BUDGET` 64, `VT_MAX_PARAMS` 8, `CMD_BUFFER_SIZE` 64 (`tty/n_tty.h`, `tty/tty.c`, `kio.c`, `kernel.c`): policy. The editable line, the cooked bytes ready for a read, the echo a flood gets under interrupts off, the parameters a control sequence carries, and the debug shell's line.
* `PIPE_BUF_SIZE` 4096, `PTY_BUF_SIZE` 4096, `SERIAL_RX_SIZE` 4096, `SCANCODE_BUF_SIZE` 64, `MOUSE_BUF_SIZE` 64 (`ipc/pipe.h`, `tty/pty.h`, `serial.c`, `kbd.c`, `mouse.c`): policy. Rings, each a power of two for its mask; the pipe's page is the ABI's `PIPE_BUF`.
* `PS2_DRAIN_LIMIT` 4096 (`mouse.c`): safety. The bytes the initial drain reads before the controller is called stuck.
* `HDA_MAX_STREAMS` 32, `HDA_BDL_MAX_ENTRIES` 256, `HDA_CORB_SIZE` 256, `HDA_RIRB_SIZE` 256, `HDA_BUFFER_SIZE` 16 KiB, `HDA_MMIO_SIZE`, `SB16_BUFFER_SIZE` 160 (`audio/hda.h`, `audio/hda.c`, `audio/sb16.h`): physical. The HD Audio specification's stream count and ring sizes and the Sound Blaster's DMA block.
* `HDA_RING_SIZE` 8192, `RING_BUFFER_SIZE` (`audio/hda.c`, `audio/sb16.c`): policy. The write path's ring, sized for Doom's bursts.
* `KLOG_RECORD_MAX` 256, `KLOG_RING_SIZE` 16 KiB, `KLOG_PENDING_MAX` 8 (`klog.h`, `klog.c`): policy. The longest record, the machine's own copy, and the records queued for the wire before one is dropped and the drop counted.
* `LAT_COUNT` 4 (`latency.h`): policy. The delays `/proc/stat` keeps.
* `FPU_STATE_SIZE` 512, `VGA_GFX_SIZE` 64000, `VGA_FONT_SIZE` (`proc/fpu.h`, `display/vga_gfx.h`): physical. The `fxsave` area and mode 13h.
* `PIT_COUNTS_PER_SEC`, `US_PER_COUNT_THOUSANDTHS` (`udelay.c`, `pit.c`): physical. The timer's rate.

## The tools, the utilities and the tests

* `elfcheck.c`: `ELF_MAX_PHNUM`, `ELF_MAX_SEGS`, `ELF_USER_MAX` are the kernel's, repeated so the checker judges what the loader will accept.
* `setidcheck.c`: `EXT2_FT_MAX` is the format's; `MAX_LINE` is the checker's line buffer (safety).
* `guestcheck.c`: `FD_LIMIT` 16 is the descriptor table a compile on the machine has, which the deepest kernel include chain plus the descriptors a compile holds besides must fit; `MAX_LINE` 8192 and `MAX_TEXT` 1 MiB are its buffers (safety).
* `prosecheck.c`: `LIMIT` 80 is the comment column, policy; `MAX_FILE` 4 MiB a buffer. `sizecheck.c`: `LINE_MAX_LEN`, `NAME_MAX_LEN`, `PATH_MAX_LEN`, `SIG_MAX_LEN`, `REASONS_MAX` 1024 are its buffers, each refused by name when exceeded. `mancheck.c`: `NAME_MAX_LEN` 64 a name buffer. `cfgcheck.c`: `MAX_TEXT` 64 KiB the configuration read.
* `grubembed.c` and `utils/grubslot.h`: `MAX_IMAGE_SECTORS` 1024, the GRUB core image's 512 KiB, format: the post-MBR gap the disk layout leaves.
* `mkgpt.c`: `MAX_PARTS` 15, the kernel's `PART_MAX`.
* `krffs/krffs_platform.h`: `KRFFS_MAX_REGION_SIZE` 4095 MiB, format: a region's size is stored in 32 bits, and the cap stays clear of the edge.
* `utils/bootslot.h`, `bootslot.c`, `bootctl.c`: `BOOTSLOT_ENTRY_MAX` 24, `BOOTSLOT_FAT_MAX_SECTORS` 512, `BOOTSLOT_MAX_CLUSTERS` 65536, `BOOTSLOT_MAX_FILE_CLUSTERS` 8192, `DIR_DEPTH_MAX` 4, `SLOT_MAX` 4 MiB: format and policy together: the boot partition is a FAT16 volume of 64 MiB, and a kernel slot is 4 MiB of it, which phase 5 met when the trap kernel at 2.57 MB exceeded the 2 MiB the disks reserve.
* `utils/dhcpmsg.h`, `dhcpc.c`: `DHCP_MAX` 576 is the format's minimum message; `HARD_MAX_LEASE` seven days is policy.
* `utils/pwauth.h`, `pwauth.c`, `adduser.c`, `passwd.c`, `login.c`: `PWAUTH_MAX_NAME` 63, `PWAUTH_MAX_PASSWORD` 100, `PWAUTH_SIG_MAX` 65, `RECORD_MAX` 254, `SHADOW_RECORD_MAX` 254, `TEXT_MAX` 16 KiB, `LOGIN_MAX_FDS` 16: policy for the credential files, the BusyBox and Linux numbers for a passwd record.
* `utils/httpd.c`: `REQLINE_MAX` 64, `AGENT_MAX` 76, `HOST_MAX` 48, `METHPATH_MAX` 34: policy, what the log line keeps of a request. `utils/netcfg.h`, `netup.c`: `NETCFG_LINE_MAX` 256, `NETCFG_NAME_MAX` 16, `MAX_IFACES` 8: the configuration file's line, the Linux interface name, and the kernel's `NETDEV_MAX` with room. `utils/init.c`: `SERVICE_LOG_CAP` 256 KiB, policy, a service's captured output. `utils/rm.c`, `su.c`, `sudo.c`: `MAX_PATH` 256, `MAX_FD` 64: policy, the descriptors closed before an exec. `utils/growset.c`: `MAX_RANGES` 65536, safety, the ranges a save set holds.
* The harness: `MAX_STEPS` 128 and `MAX_ARGS` 128 in `tests/system/runner/harness.c`, policy, a suite's script and its QEMU line. The fixture makers' `CHK_MAX_DIR_CLUSTERS` and `CHK_MAX_ENTRIES` 4096 bound the check they run on what they wrote. The fuzz harnesses' `FUZZ_INPUT_MAX` 1 MiB, `FS_IMAGE_MAX` 4 MiB, `IMAGE_MAX` and `INPUT_MAX` are the inputs they accept. The probes' `LIMIT`, `MAX_TCP_ROWS`, `READ_LIMIT_MS` and the host tests' `MAX_FRAMES`, `MAX_SEGS`, `STREAM_MAX`, `WRITES_MAX`, `WLOG_MAX`, `CAP`, `TRACE_MAX`, `MODEL_LOG_MAX`, `MAX_CODES`, `MASK_OBS_MAX`, `DEVMODEL_PENDING_MAX` bound what one test records, each sized past what its rows produce.

## The build, the images and the procedures

* `E2FSCK_LIMIT` 737280 and `RESIZE2FS_LIMIT` 430080 in the root Makefile: policy, each set from the program's first build with a tenth of headroom, so a configure that let a library or a program back in is caught at the build and not in the recovery image; `vendor/e2fsprogs/Readme.md` states them.
* The images: the test image, os-simple and os-real as RAM disks sized by their manifests; `os-real-small.ext2` 96 MiB for the install suite and 48 MiB for the ext2 self-build; the os-real and sealed ext2 roots 1024 MiB; the fs-demo volume 4 MiB; the sealed disk's boot and recovery partitions 64 MiB each (`RACH2_DISK_BOOT_MB`, `RACH2_DISK_RECOVERY_MB`) and its ext2 root 1024 MiB (`RACH2_DISK_EXT2_MB`), grown on the machine by `growroot` to the disk. All policy, each a number in the Makefile or the manifest that makes the image.
* The kernel slot of 2 MiB on the disks the suites make and 4 MiB in the boot partition: policy; the checked kernels boot from the harness's `-kernel` because of it.
* The guest build's descriptor table of 16 (`guestcheck.c`) and the kernel's `FD_TABLE_LIMIT`: the self-build on the machine runs its compiles within them.
* The procedures: the disk offsets the Readme's "Installing and updating a machine" section depends on are the sealed disk's layout above, and the recovery stick's saved sectors are bounded by the stick's size, which the hardware rule has the owner read before a write.
