# The claims

Every Readme read against the tree, as phase 9 of milestone 53. One line per claim or group of claims that share an owner: the document and section, the claim, the owner (a file and function for what code does; a record under `dev-log/` for a hardware fact; a build rule or the doctor script for a prerequisite; `CLAUDE.md` for a policy), and what happens to it: kept, fixed, or removed. A claim two documents make is made once and linked from the other; those are marked "once in". The rewrite in phases 10 and 11 is done from this file.

## Readme.md, the root

* Purpose, "Rach2 is a small educational operating system...": kept; owner the tree as a whole, the images `krffs/manifests/*.manifest`.
* Build and run, the prerequisites: kept; owner `tools/setup/preflight.sh` and `tools/setup/doctor.sh`; once in `tools/Readme.md`, linked here.
* The credential files created by the build: kept; owner the `krffs/rach2_demo_password` and `krffs/rach2_test_password` rules in the root Makefile.
* The useful targets, `make test`, `tests/run NAME`, `tests/run selfbuild`, `style`, `host`, `clean`, `distclean`: kept; owner the root Makefile and `tests/run`.
* Images: the three RAM images and `os-real-ext2`: kept; owner the image rules in the root Makefile and `mk/images.mk`. The stick image and its mount through `/dev/diskNp2`: kept; owner `mk/images.mk` (`rach2-stick-image`) and `sys/block/usbdisk.c`.
* System, the kernel paragraph: once in `rach2/Readme.md`; this section keeps one sentence and the link.
* System, KRFFS and the mounts: kept; owners `sys/fs/krffs.c`, `sys/fs/ext2.c`, `sys/fs/fat32.c`, `sys/fs/exfat.c`, `sys/fs/vfs.c` (`vfs_do_mount`), `sys/block/loop.c`. "two KRFFS volumes may be mounted at once" under "What a fresh disk holds": owner to find in `sys/fs/krffsmount.c`; fixed to the number the code has.
* System, the network stack and the drivers: kept; owners `sys/network/`, `sys/network/driver/`; once in `sys/network/Readme.md`.
* Kernel drivers, the list: kept; owners the driver directories. "one that had stopped answering panicked the kernel once when pulled, which milestone 43 records as open": owner `dev-log/43-machine/notes.md`; checked against the shortcomings list at the rewrite and fixed if that entry closed.
* Building on Rach2: kept; owners `krffs/manifests/os-real.manifest` (`/src`), `rach2/guest.mk`, `utils/guest.mk`, `ish/guest.mk`, `tests/system/suites/selfbuild.sh`.
* Limits: the memory, process and descriptor sentences: kept; owners `sys/mm/frame.c`, `sys/proc/process.c`, `sys/fs/fd.h` (`FD_TABLE_FIRST`, `FD_TABLE_LIMIT`), `sys/syscall_proc.c` (`prlimit64`), `utils/init.c` (`/etc/rach2/limits`). "USB is keyboards, mice and flash drives ... with twelve table entries shared by every device, hubs included, and two hubs at once": stale; `USB_MAX_DEVICES` is 16 and `HUB_MAX` 8 (`sys/usb/core.h`, `sys/usb/hub.h`); fixed. The Lenovo load figures: owner `dev-log/43-machine/hardware.md`; kept as a pointer, the numbers once in that record. "does not implement UEFI, SMP, threads, swap, dynamic linking, IPv6, or VESA": kept; once here, `rach2/Readme.md` links.
* Hardware status: the machine's history, partitions, the install on 2026-09-12, the 361 build completions, the 47 problems: owners `dev-log/43-machine/hardware.md`, `dev-log/50-shortcomings/Readme.md` and its list; the section is rewritten to a short pointer to those records, as the plan decides, with the counts read from them at the rewrite (the list has 111 entries now). "the move was rehearsed ... `tests/run sealed`": owner `tests/system/suites/sealed.sh`. The lower-case sentence start "the move was rehearsed" is a fault in the prose.
* What a fresh disk holds: the three partitions and their types: kept; owner `tools/image/mkdisk_grub.sh` and `krffs/manifests/rach2-metal-disk.manifest`. The `/demo` images and the loop commands: kept; owners `krffs/manifests/rach2-metal-disk.manifest` (`/demo/Readme.txt` staged from `krffs/demo/Readme.txt`), `sys/block/loop.h` (`LOOP_DEVICES` 4), `utils/loopctl.c`. The `mkimage.sh` and `fs-demo-images` commands: owners `krffs/mkimage.sh`, the `fs-demo-images` rule.
* Installing and updating a machine: the install from the stick: owners `utils/rach2-install.sh`, `utils/rach2-install.txt`, `man/man8/rach2-install.8` (milestone 51); the section is moved to a pointer at the manual pages, as the plan decides. The three update paths with their commands: owners `utils/bootctl.c`, `utils/reflash.c`, `utils/rach2-install.sh --update`; the disk offsets: owner `tools/image/mkdisk_grub.sh` and `dev-log/43-machine/hardware.md`; the procedures move to the manual pages `bootctl(8)`, `reflash(8)`, `update(8)` and the offsets to the machine record. "`make sealed-test` rehearses them in emulation": stale, no such target; fixed to `tests/run sealed`.
* Working the machine from the host: the SSH form, the addresses, the raw window, the staged menu, the partition rewrite, the stick, the acceptance script, the netconsole capture, the clock: owners `utils/init.c` (dropbear `-g`), `sys/block/diskinfo.c`, `utils/bootctl.c`, `tools/image/mkdisk_grub.sh`, `tests/system/accept/metal_accept.sh`, `sys/network/netconsole.h` (`0x88b5`), `dev-log/43-machine/hardware.md`. The machine's addresses and the owner's network are facts of one machine and move to the machine record. The credential check: owner `krffs/gen_demo_key.sh`.
* Repository map: kept; owner the tree; the list is regenerated from the directories at the rewrite.
* Development and the third-party sources: kept; owners `CLAUDE.md`, `AGENTS.md`, the `build_*.sh` scripts.

## rach2/Readme.md, the kernel

* Purpose and the build section: kept; owners `loader.s`, `link.ld`, `rach2/Makefile`, `sources.mk`, `guest.mk`, `tools/inspect/elfcheck.c`, `mk/fingerprint.mk`. "The kernel-only build can also use the corresponding `x86_64-elf` cross tools": owner `mk/toolchain.mk`; checked at the rewrite.
* Architecture, the tables and vectors: kept; owners `sys/tables/`, `sys/isr.h` (`ISR_MSI_FIRST` 48, `ISR_MSI_LAST` 63). "`isr_alloc_vector`, which returns the lowest unallocated vector from 48 through 127": stale, the vectors are 48 through 63; fixed.
* Architecture, the local APIC and messages: kept; owners `sys/lapic.c`, `sys/pci.c` (`pci_msi_enable`), `sys/usb/xhci.c`, `sys/audio/hda.c`.
* Architecture, memory, descriptors, processes, exit: kept; owners `sys/mm/`, `sys/fs/fd.h`, `sys/proc/`, `sys/syscall_proc.c` (`sys_execve`, `sys_exit`, `do_wait`). The scheduling paragraph: owner `sys/proc/scheduler.c` and `sys/isr.c`.
* Root selection, the five forms and the boot rules: kept; owners `kernel.c` (`root_spec_matches`, `k_main`), `sys/block/partition.c`, `sys/block/ahci.c` (`ahci_start`), `sys/fs/ext2.c` (`ext2_mount_on`).
* Running QEMU: the scripts: kept; owners `rach2/run.sh`, `run_from_iso.sh`, `run_from_disk.sh`, `debug.sh`. "`make os-real-grub-disk`": stale, no such target, and `run_from_disk.sh` names `build/images/os-real-grub.disk`, which no rule makes; fixed to the disk a rule builds, or the script and the line go.
* Kernel command line, the table: kept; owner `sys/cmdline.c` and the readers of each option; each row is checked against a `cmdline_` call at the rewrite.
* Messages: kept; owners `sys/klog.c`, `sys/klog.h` (`KLOG_RING_SIZE`, `KLOG_RECORD_MAX`, `KLOG_PENDING_MAX`).
* Input: kept; owners `sys/input.c`, `sys/serial.c` (`SERIAL_RX_SIZE`), `sys/tty/n_tty.c`, `sys/tty/tty.c` (`TTY_ECHO_MAX`), `sys/fs/procfs.c` (`/proc/console`), `tests/system/suites/conload.sh` (360 lines).
* Debug shell: kept; owner `kernel.c` (`process_command`, `exec_program`), `sys/power.c` (the three resets).
* Source map: kept; owner the directories; regenerated at the rewrite.
* Hardware and limits: the supported list: kept; owners the drivers. "twelve table entries ... two hubs driven at once": stale as in the root Readme; fixed to 16 and 8. The descriptor and `fdfull` sentences: owners `sys/fs/fd.h`, `tests/fdfull.c`. The `pools` numbers: owner `tests/system/suites/pools.sh` and `tests/poolprobe.c`. The `prlimit64` paragraph: owner `sys/syscall_proc.c`, `tests/limitprobe.c`, `utils/init.c`.
* Verification: "`tests/run core`, which is the 21 system suites": stale, the core tier is 36 suites; fixed, and the count read from `tests/registry` rather than written.

## tools/Readme.md

* The grouping of `tools/` and the opening paragraph: kept; owner the directory.
* `dev/machine_load.sh`, `dev/machine_net.c`, `dev/poll_kernels.sh`, `dev/machine_net_in.sh`, the clock check, `dev/machine_grow_plan.sh`, `dev/machine_grow.sh`, `image/mkinstallstick.sh`: kept; owners those scripts and `utils/growset.c`, `utils/reflash.c`, `utils/growboot.c`; the milestone 50 procedure they cite is `dev-log/50-shortcomings/recovery-grow.md`. These paragraphs are the tool's contract and stay; the measurement guidance about unit D and the historical runs is history and goes to `dev-log/50-shortcomings/`.
* Machine setup, the prerequisites and the two package lists: kept; owners `setup/preflight.sh`, `setup/doctor.sh`, `setup/e2fsprogs_path.sh`; once here, the root Readme links. "The six suites that build a GRUB disk need mtools": the count is read from `tests/registry` at the rewrite.
* The Debian wrapper and the binutils defaults: kept; owners the wrapper text here, `mk/toolchain.mk`, `rach2/Makefile` (`-z noseparate-code`), `mk/host.mk`.
* The testing host: kept; owners `CLAUDE.md`, `dev/testing.sh`, `setup/judges`.
* The two checkers: kept; owners `setup/preflight.sh`, `setup/doctor.sh`, `setup/e2fsprogs_path.sh`.
* `tests/run style`, the twenty-one checks and the supporting files: kept; owner `verify/check.sh` and each file named; the list is regenerated from `check.sh`'s numbered header at the rewrite so it cannot drift again.
* Artifact and configuration checkers: kept; owners `inspect/elfcheck.c`, `inspect/check_link.sh`, `inspect/setidcheck.c`, `inspect/binfind.c`, `inspect/check_diskpolicy.sh`, `inspect/check_recovery.sh`, `inspect/cfgcheck.c`. "The checker's self-test has 24 cases": read from `cfgcheck --selftest` at the rewrite.
* "clangd database": the heading holds the `dev/testing.sh` paragraph, which belongs under the testing host; moved. `dev/gen_compile_commands.sh`: stale, the generator writes twelve databases under `build/compile_commands/` and `dbcheck.sh` requires every source in one (phase 1); fixed from `tools/dev/gen_compile_commands.sh` and `tools/verify/dbcheck.sh`.
* GRUB and ISO builders, `image/mkext2.sh`: kept; owner the script. `image/mkdisk_grub.sh`: stale, the disk has had three partitions since 2026-09-14, the boot partition, recovery on 2 and the ext2 root on 3, with no KRFFS root and no fourth partition; the candidate entries select `MBRID=1acef00d,3`; fixed from the script. "accepts eleven `RACH2_DISK_*` settings" and the list naming `RACH2_DISK_MANIFEST`, `RACH2_DISK_KRFFS_MB`, "the fourth partition": stale, the script reads ten, `APPEND`, `BOOT_MB`, `CLUSTER`, `DHCP_APPEND`, `EXT2_IMAGE`, `EXT2_MB`, `HELLO`, `RECOVERY_IMAGE`, `RECOVERY_KERNEL`, `RECOVERY_MB`; fixed. `image/grubembed.c`, `image/mkstick.sh`, `image/stickcheck.c`, `image/mkmbr.c`, `image/mkgpt.c`, `image/mkgrubcore.sh`, `image/grub-embed.cfg`, `image/mkiso.sh`, `image/ubuntu-image.txt`: kept; owners those files. "the kernel mounts the volume as `/dev/usb0p2`": stale, the stick is a disk, `/dev/diskNp2`, as the root Readme says; fixed. "The top-level `make isos` target builds all four": stale, five are listed and the rule names five; fixed.

## tests/Readme.md

* "The root contains 82 programs": stale, `tests/Makefile` lists 102; fixed, and the number read from the Makefile at the rewrite. "`musl/` contains 35 programs": kept; owner `tests/musl/`.
* The suite runner, its four paragraphs: kept; owner `tests/run`; the registry fields, the memory budget and the inventory are phase 7's. The idleness, lock, ledger and hash rules: owners `tests/run` (`run_pool`, `owned_stop`, `check_hashes`) and `tests/system/runner/runutil.c`.
* Build and image layout: "build the 82 root programs": stale as above; fixed. `dnstest`, `tests/musl/build.sh`, `link-deps:`, the test manifest: kept; owners `tests/Makefile`, `tests/musl/build.sh`, `krffs/manifests/test.manifest`.
* The default guest suite: "the 73 entries in its `PROBES` list", "The 57 root programs", "The 16 musl programs" and the two lists: kept, the list in `tests/system/runner/runtests.c` has 73 entries, 57 and 16; the lists are regenerated from it at the rewrite rather than copied. The run limits and the clock check: owners `runtests.c`, `tests/system/suites.mk` (`run-default`).
* Other guest programs and targets, the mapping of suites to probes: kept; owners the suite scripts under `tests/system/suites/` and `suites.mk`; each line is a second description of a suite and goes once the inventory `tests/system/suites.md` names what each proves, with this section keeping only the probes another test invokes. "`install-ext2`, `drill-ext2` and `selfbuild-ext2` are the install, drill and self-build suites on that root": stale, the registry has `selfbuild-ext2` alone, `install` and `drill` run on the ext2 root since 2026-09-14; fixed.
* The musl programs absent from the lists: kept; regenerated from the directory and the list at the rewrite.
* Manual runs and adding a guest test: kept; owners `rach2/run.sh`, `tests/Makefile`, `tests/musl/build.sh`, `tools/verify/check.sh`.

## tests/system/Readme.md

* The directory's parts and the runner's mechanisms: kept; owners `tests/run`, `tests/registry`, `runner/runutil.c`. "the registry is three fields": stale, five since phase 7; fixed. The pool, the idleness limit, the schedule order, the completion bounds, the accelerator shim, the images column and the interval rule: kept; owners `tests/run`, `tools/verify/suitecheck.sh`, `tests/system/intervals`. The runner description is made once, here, and `tests/Readme.md` keeps the short form and links.
* The runner's self-test, `drive.sh`, `runutil_group.sh`, `runutil_owned.sh`, `runutil_machine.c`, `ssh_flood_test.sh`: kept; owners those files. The host interfaces: kept; owner `runner/runutil.c`.
* QEMU runners, capture checkers and fixtures: kept; owners `runner/harness.c`, `runner/runtests.c`, `runner/hostpeer.c`, `runner/ptydrive.c`, `runner/deadline.c`, `checkers/wavcheck.c`, `checkers/pcapcheck.c`, `fixtures/mkfixture.c`.
* Suites, the host and style suites and the machine-grow proofs: kept; owners `suites/host.sh`, `machine_measure_test.sh`, `grow_measure_test.sh`, `machine_grow_check.sh`, `machine_grow_boot.sh`, `machine_grow_space_test.sh`, `machine_grow_full_test.sh`, `machine_grow_space.sh`, `grow_machine.sh`, `usbdisk_mutate.sh`, `usbdisk_script_test.sh`; these paragraphs describe proofs of milestone 50's procedure and are kept as the contract of those scripts, shortened to what each proves.
* The suite list: each bullet is a second description of a suite beside the one `tests/system/suites.md` now generates; at the rewrite the list keeps only what the inventory's sentence cannot hold, the rows and the numbers a reader needs to judge a failure, and the rest goes once. Stale bullets: `drill` "on a disk whose fourth partition is the ext2 root" and "`tests/run drill-ext2`", `install` "`tests/run install-ext2`" and "the fallback boots are the KRFFS root", `sealed` "the ext2 root on the fourth partition and the fallback entries the KRFFS root ... the KRFFS root is the one destroyed": stale since 2026-09-14, the disk has one root on partition 3 and both kernel entries boot it, the fallback being the previous kernel, and `install` and `drill` run on that root with no `-ext2` twin; fixed from `suites/sealed.sh`, `suites/install.sh`, `suites/drill.sh` and the registry. `usb` "two hubs with eleven devices filling the twelve-entry table with the thirteenth refused by name": checked against `suites/usb.sh` at the rewrite and fixed to the row's numbers under `USB_MAX_DEVICES` 16.
* Real-machine acceptance: kept; owners `accept/metal_accept.sh`, `accept/transport_test.sh`, `accept/password_test.sh`, `accept/su_command.sh`, `tests/system/suites/metal.sh`; "The installed ext2 root must be writable on partition 3": kept, that is the layout.

## tests/host/Readme.md

* The runner, its flags, `host-deps:`, `host-seconds:`, `host-cflags:`, the deadline, the eight measurement cases and `check-libc`: kept; owners `tests/system/suites/host.sh`, `tests/system/runner/deadline.c`, `tests/system/machine_measure_test.sh`; the corpus replay step is phase 5's and is added here.
* Adding a test, the compatibility headers, `compat/devmodel.c`, `compat/blockmax_host.c`: kept; owners those files.
* `mkfatfixture.c` and `mkexfatfixture.c`: kept; owners the two makers; the builders phase 5 exposed (`mkfatfixture_build`, `mkexfatfixture_build`) are added.
* "The suite contains 73 native test programs": stale, there are 75 `*_hosttest.c` files; fixed, and the number read from the directory at the rewrite.
* The test list, one paragraph per test: kept; owner each `tests/host/*_hosttest.c` named. The two tests added since the list was written are found by comparing the list with the directory at the rewrite. Each paragraph is the test's contract in the test's own header comment as well; the rewrite keeps one sentence here and leaves the rows to the source.

## rach2/sys/block/Readme.md

* The layer, devices and partitions, `BLOCK_CALLER_SYNC`, the chain, `BLOCK_FAULT_SAFE` and `BLOCK_POLLABLE`, the probe, the views: kept; owners `block.h`, `block.c`, `chain.c`, `request.c`.
* The partition service, the MBR and GPT rules, `PART_MAX` 15: kept; owners `partition.c`, `mbr.c`, `gpt.c`.
* The loop device, `/dev/loop-control`, `/proc/loop`, the attach and detach rules: kept; owners `loop.c`, `loop.h` (`LOOP_DEVICES` 4), `utils/loopctl.c`.
* The block request, its sizes, deadlines, the pool of sixteen, the quarantine and the lag device: kept; owners `request.c` (`REQ_LIMIT`, `REQ_DEFAULT_DEADLINE`, `REQ_DEFAULT_DEADLINES`), `lag.c`; the numbers are the bounds inventory's.
* Partition discovery and the disk root, the identity forms: kept; owners `kernel.c` (`root_spec_matches`), `partition.c`, `diskinfo.c`; once here, `rach2/Readme.md` keeps its table and links.
* The GUID partition table, the stick as a disk, ATA, AHCI, the raw disk devices, the registry, disk policy and recovery authority, disk information: kept; owners `gpt.c`, `usbdisk.c`, `ata.c`, `ahci.c`, `diskinfo.c`, `disk.c`, `diskcmd.c`, `tools/inspect/check_diskpolicy.sh`, `tools/inspect/check_recovery.sh`.
* Verification: the host tests: kept; owners the tests named. "`tests/run usbdisk` ... runs thirteen QEMU boots": the suite's script has thirteen numbered boots and `tests/system/Readme.md` says fourteen; the two are reconciled from `suites/usbdisk.sh` at the rewrite. "`tests/run bootorder` ... requires the KRFFS root to resolve as `/dev/disk1p2`": stale, the suite requires `root: MBRID=1acef00d,3: /dev/disk1p3, ext2`; fixed. The rest of the suite descriptions duplicate `tests/system/Readme.md` and `tests/Readme.md`; made once, in the suite inventory and the system Readme, and this section keeps the host tests and links.

## rach2/sys/usb/Readme.md

* The choice of xHCI, the layers, the controllers, the pump, the two contexts, the fallback and the latency stamps: kept; owners `usb.c` (`usb_pump`), `xhci.c` (`xhci_interrupt_take`), `core.c`. "`isr_alloc_vector` for the lowest unallocated vector from 48 through 127": stale, 48 through 63 (`sys/isr.h`); fixed.
* Nothing waits, what the controller is asked for, the Intel port routing, the refused device bitmap: kept; owners `core.c`, `xhci.c`.
* The fixed limits table: "Device slots ... 16 ... the device table has sixteen entries": kept, `USB_MAX_DEVICES` 16. "Controllers driven 4": kept, `XHCI_MAX_CONTROLLERS`. "Scratchpad pages given 32": stale, `XHCI_MAX_SCRATCHPADS` is 512 and the same file says 512 above; fixed. "Hubs driven at once 2" and "Tree depth 2 hubs, the hub driver's table" and "Hubs behind hubs ... two hubs being the driver's table; a third hub is not": stale, `HUB_MAX` is 8; fixed from `hub.h` and `hub.c`. "Root ports reached 16": checked against `xhci.c`'s port table at the rewrite. "Flash drives 8 with 4 units": kept, `MSC_MAX_DEVICES`, `MSC_MAX_LUNS`. The table's numbers are the bounds inventory's and are read from the headers at the rewrite.
* Boot protocol, reports to keystrokes, mass storage, hubs, what is absent, the guest's view, stopping: kept; owners `hid.c`, `msc.c`, `hub.c`, `sys/fs/procfs.c` (`/proc/usb`), `usb.c`.
* Verification: "the five host tests written for this stack": kept; owners `tests/host/hub_hosttest.c`, `xhci_hosttest.c`, `usb_hosttest.c`, `hid_hosttest.c`, `msc_hosttest.c`. "`tests/run usb` is twenty-one boots": `tests/system/Readme.md` says twenty-two, one in two parts; reconciled from `suites/usb.sh` at the rewrite, and the count read from the script.

## rach2/sys/network/Readme.md

* Device selection, the probe order, the interface record, `link` and `stop`, the three drivers and their omissions: kept; owners `driver/netdev.c`, `driver/rtl8139.c`, `driver/e1000.c`, `driver/rtl8168.c`; the Linux commit the RTL8168 sequence derives from is a provenance fact and stays.
* Interface configuration, the boot line, the ioctls, the routing, `/proc/net/*`: kept; owners `netif.c`, `netconf.c`, `ip/ip.c`, `arp/arp.c`, `sys/fs/procfs.c`.
* Ethernet, ARP and IPv4, the counters, the ARP cache of eight, the loopback ring of 64 slots: kept; owners `ethernet/ethernet.c`, `arp/arp.h` (`ARP_CACHE_SIZE`), `ip/ip.c` (`LOOP_RING_SLOTS`).
* ICMP and UDP, TCP, the socket interface, packet and raw sockets, netconsole, concurrency: kept; owners `icmp/`, `udp/`, `tcp/tcp.c`, `tcp/tcp.h` (the timers), `socket/socket.c`, `socket/packet.c`, `socket/raw.c`, `netconsole.c`. "A packet socket binds to interface index 0 or 1": stale, it binds to zero or any index the machine has, `netdev_count()` up to `NETDEV_MAX` 4 (`socket/packet.c`); fixed. "The fixed circular accept array has 16 entries ... at most 15": owner `tcp/tcp.c`; checked at the rewrite.
* Verification: "The default suite runs 72 guest probes": stale, the list has 73 entries; fixed, and the number read from `runtests.c`. The suite bullets duplicate the suite inventory; made once there and linked.

## rach2/sys/mm/Readme.md

* Physical memory, the map, the four numbers, `FRAME_MIN_MANAGED_KIB`, the identity window, the frame table, the kernel layout, the heap and its reserve, `rach2.framefail`, `mem_reserve`, `modlayout`: kept; owners `frame.c`, `paging.c`, `paging.h`, `kheap.c`, `modlayout.c`.
* The allocators' order and reserve, `mem_cache_target`, `mem_balance`, `mem_reclaim`: kept; owners `paging.c`, `frame.c`, `sys/fs/bcache.c`, `pagecache.c`. "The page cache also uses half the target as its capacity, with a minimum of one frame and a maximum of 1024 slots" against the page cache section's "up to 2048 frames, 8 MiB": the second is `PC_SLOTS` 2048; the first is fixed to it.
* Page tables and address spaces, VMAs, the three syscall contracts, page faults, copy-on-write, checked user access: kept; owners `paging.c`, `vm.c`, `vm.h` (`VMA_LIMIT`, `VM_STACK_MAX_GROWTH`), `usercopy.c`, `sys/syscall_proc.c` (`brk`, `mmap2`, `munmap`).
* The page cache, `PCWIN_BASE`, the 512 MiB cap, the slot's frame, `pc_sync`: kept; owners `pagecache.c`, `pagecache.h`.
* Files and verification: kept; owners the files named and `tests/host/{frame,modlayout,vm,pagecache}_hosttest.c`.

## rach2/sys/fs/Readme.md

* KRFFS's place, the files, paths and directories, descriptors, modes and times, mounts and durability, `mount` and `umount2`: kept; owners `krffs.c`, `krffsmount.c`, `fd.c`, `fd.h`, `vfs.c`, `sys/syscall_fs.c`. "The syscall-facing permission, descriptor, and metadata rules are in `../syscall.c`": stale since the split, they are in `../syscall_fs.c`; fixed.
* "`krffs/mkdisk.sh` limits a generated disk partition to the 512 MiB window cap" and "the host KRFFS tools also accept standalone regions up to 4095 MiB": kept; owners `krffs/mkdisk.sh`, `krffs/krffs_platform.h` (`KRFFS_MAX_REGION_SIZE`).
* Tests: kept; owners the probes named and `tests/host/readfile_hosttest.c`, `krffs_hosttest.c`; the suite descriptions are made once in the inventory.

## rach2/sys/fs/vfs.Readme.md

* The mount table, the mount point, the target path of 63 bytes, the states, a removed device: kept; owners `vfs.c` (`vfs_do_mount`, `mount_new_slot`), `vfs.h`.
* The resolver, physical `..`, symbolic links and the forty-link bound, `vfs_ref`, `VFS_WALK_PARENT`, `lookup_rest` and `VFS_CLIMB`: kept; owners `vfs.c` (`resolve_path`), `vfs.h` (`VFS_LINK_MAX`, `VFS_WALK_PATH_MAX`), `synthfs.c`.
* The operations, the wrappers' answers for a NULL pointer, descriptions and the working directory, `vfs_repoint`, the locks and their order: kept; owners `vfs.c`, `fd.c`, `sys/fs/bcache.c`.
* `mount` and `umount2`, the sources, the targets, the order of work, the flags: kept; owners `vfs.c`, `devfs.c` (`devfs_window_of`). "`/dev/usb0` to `/dev/usb1p4`" as a source: stale, the sticks are `/dev/diskN` and `/dev/diskNpM` since the registry took them (`devfs.c`, `usbdisk.c`); fixed. "`ENOMEM` when both of its volume slots are taken": stale, a KRFFS volume record comes from the heap (`krffsmount.c`); fixed.
* Claims and the buffer cache: kept; owners `devfs.c` (`DEVFS_CLAIMS` 8), `bcache.c`, `bcache.h` (`BCACHE_HOLD_MAX` 4, `BCACHE_MIN_BUFFERS` 16).
* The Limits paragraph: stale in five of its six sentences. The mount table starts at eight slots and grows by doubling, so there is no eight-mount limit; the synthetic nodes, the vfat nodes, the KRFFS volumes and their open records all come from the heap with no fixed count, so "sixteen synthetic nodes", "a vfat pool of 32 nodes per mount and four mounts" and "two krffs volumes at once" are all gone; the cache makes buffers as it needs them up to half the cache target and never fewer than sixteen, not "sixty-four"; the request slots are sixteen, which is right, but the request is up to 2048 sectors and its deadline is thirty seconds a period with four periods, not "eight sectors and a deadline of five seconds", which is the lag device's own period. Rewritten from `vfs.c`, `synthfs.c`, `fat32.c`, `krffsmount.c`, `bcache.h` and `block/request.c`, with the numbers read from the bounds inventory.
* What is absent, and verification: kept; owners the suites and `tests/system/vfs_unmount_test.sh`.

## rach2/sys/fs/krffs.Readme.md

* The format, the node header of 357 bytes, the ids, the dirty bit, what a mount requires, the order of writes and its six operations, what a stop leaves, the two backends, the allocator: kept; owners `krffs.c`, `krffs.h` (`KRFFS_FIELD_SIZE`, `KRFFS_FILE_NAME_BUFFER_SIZE`, `KRFFS_MODE_DIRTY`), `krffsmount.c`, `krffs/fsck.krffs`.
* "the mounted volume keeps a record per open file, sixteen to a volume": stale, the open records come from the heap (`krffsmount.c`); fixed.

## rach2/sys/fs/synthfs.Readme.md

* Path routing, devfs and its table, the pty pairs, `/proc/usb`, the loop device, the stick nodes, `/dev/screenlog`, `/dev/bootlog`: kept; owners `synthfs.c`, `devfs.c`, `procfs.c`, `tty/pty.c`, `block/loop.c`, `klog.c`. "A synthetic node lives for the length of one syscall in a pool of sixteen": stale, the node comes from the heap (`vfs.c`, `synth_take`); fixed.
* The devfs table's rows: the `/dev/usb0` and `/dev/usb0p1` rows are stale, the sticks are `/dev/diskN` and `/dev/diskNpM` with the minors `block/Readme.md` states; fixed, and the whole table checked against `devfs.c`'s registry at the rewrite.
* Raw disk windows, procfs, its files, the snapshot lifetime, verification: kept; owners `devfs.c`, `procfs.c`, `diskinfo.c`. "`MSI` for a device message vector allocated from 48 through 127": stale, 48 through 63; fixed.

## rach2/sys/fs/fat32.Readme.md

* What is read, the volume check, directories and names, the alias rule, chains and bounds, nodes and identities, times and modes, writing and its order, the dirty volume at mount: kept; owners `fat32.c`, `fat32name.c`, `fatcommon.c`, `fatcommon.h`.
* "Four volumes can be mounted at once": stale, a mount's record and its nodes come from the heap with no fixed count (`fat32.c`); fixed.
* The bitmap "of 2^22 bits, shared by every mount and by the ext2 check and used by one walk at a time": kept; owner `fatcommon.c`.
* Verification: kept; owners `tests/host/fatname_hosttest.c`, `fat32_hosttest.c`, and the suites.

## rach2/sys/fs/exfat.Readme.md

* The volume check, both boot regions, the root's objects, the up-case table and its three forms, entry sets and names, chains, writing and its order, a dirty volume at mount, nodes, times and modes: kept; owners `exfat.c`, `fatcommon.c`, `fatcommon.h`.
* The Limits section: "Mount records are allocated from the heap without a fixed count" is current. "The 2^22-bit walk bitmap shared with FAT32 and ext2 covers 512 GB at 128 KiB clusters": stale, the bitmap is taken from the heap for the volume's cluster count through `fat_walk_acquire`; fixed, and the same sentence in `fat32.Readme.md` and `ext2.Readme.md` with it. "The walk holds 32 levels of directories, 4096 names along the path, sixteen surplus tails, sixteen checksum-only sets and sixty-four orphan secondaries": `CHECK_DEPTH` 32 and `CHECK_ORPHANS` 64 are current; the other three are checked against `exfat.c` at the rewrite.
* Verification: kept; owners `tests/host/exfatcheck_hosttest.c`, `exfat_hosttest.c`, `tests/system/suites/exfat.sh`.

## rach2/sys/fs/ext2.Readme.md

* What is read, the features, the checked fields, the file and link forms, `readdir`, `path`, `statfs`: kept; owners `ext2.c`. "The volume may have at most 128 groups, 16 GiB at 4 KiB blocks; a larger one is refused by name": stale, the constructor refuses a group count that would wrap its table or not fit the volume, which is far above 128; fixed from `ext2_mount_on`.
* Writing and the seven operation bullets, the promise, the superblock's counts: kept; owner `ext2.c`.
* The check: kept; owner `ext2.c` (`ext2_check_volume`, `check_state`). "a reached-inode set of 2^20 bits": stale, the set is `(inodes + 7) / 8` bytes from the heap; fixed. "thirty-two directory levels, a table of 512 hard-linked files and one bitmap block" are `EXT2_CHECK_DEPTH`, `EXT2_CHECK_LINKS` and the 4096-byte block, all current.
* The root, `rach2.root=ext2`, the orderly stop: kept; owners `ext2.c`, `kernel.c`, `vfs.c` (`vfs_init_root`).

### ext2.Readme.md, the Limits table and the symbolic links section

* Block sizes, inode sizes, the largest file, the forty links, the target of 511 bytes, the name of 255 and the 2038 timestamps: kept; owners `ext2.c`, `vfs.h`.
* "Groups 128, which is 16 GiB at 4 KiB blocks": stale as above. "Held nodes per mount 64, a pool like FAT32's": stale, `node_new` takes a record from the heap; fixed, and the verification section's "the sixty-third node held answers `ENOMEM`" and "the sixty-fifth node answers `ENOMEM`" with it, from what `ext2_hosttest.c` asserts today. "Mounts at once 4": stale, no fixed count. "The check's bitmap 2^22 blocks ... 2^20 inodes": stale as above.
* Symbolic links, what stays out, verification: kept; owners `ext2.c`, `vfs.c`, `tests/host/ext2_hosttest.c`, `tests/system/suites/ext2.sh`.

## rach2/sys/proc/signal.Readme.md

* The dispositions, the masks, the exceptions' path, posting and permission, the default actions, delivery and the frame, the restorers, `sigreturn`: kept; owners `syscall_proc.c`, `sys/lib/ksignal.h`, `sys/proc/process.h`, `sys/isr_handler.s`, `sys/proc/fpu.c`.
* "The main implementation is in `sys/syscall.c`": stale since the split; the signal calls are in `syscall_proc.c`; fixed. Every other Readme that names `syscall.c` for a call the split moved is fixed the same way, from `sources.mk` and `syscall_internal.h`.

## rach2/sys/proc/tls.Readme.md

* The subset, the descriptor's fixed shape, the selector, the process state, the switch, `fork` and `execve`: kept; owners `proc/tls.c`, `proc/tls.h`, `tables/gdt.c`, `proc/switch.s`, `syscall_proc.c`.
* The source map's "`rach2/sys/syscall.c` implements system call 243 and the `fork` and `execve` TLS rules": call 243 is registered in `syscall.c`'s table and the `fork` and `execve` rules are in `syscall_proc.c`; the line is rewritten to name both.
* Verification: kept; owners `tests/tlsprobe.c`, `tests/host/tls_hosttest.c`.

## rach2/sys/lib/krandom.Readme.md

* The generator, its seeding and trust, the two devices, `AT_RANDOM`: kept; owners `lib/krandom.c`, `proc/uimage.c`, `fs/devfs.c`. "`rach2/sys/syscall.c` implements `getrandom`": current, `getrandom` is in `syscall.c`'s ABI arm; kept, and checked at the rewrite.

## krffs/Readme.md

* The build, the eight binaries and the optional FUSE mount, the nine tools, the region arguments: kept; owners `krffs/Makefile` and each tool's source; the count is read from the Makefile at the rewrite.
* The format, the node fields and the allocator: kept; owner `krffs/krffs.c` and `rach2/sys/fs/krffs.h`; the format is described once in `rach2/sys/fs/krffs.Readme.md` and this section keeps the host tools' view and links.
* Manifests and images, the six directives, the staging rules, `manifest_deps.sh`, `--tree`, `mkimage.sh`, `mkdisk.sh`, `gen_demo_key.sh`, `gen_demo_password.sh`: kept; owners `stageimage.krffs.c`, `manifest_deps.sh`, `mkimage.sh`, `mkdisk.sh`, `gen_demo_key.sh`, `gen_demo_password.sh`, `tools/inspect/setidcheck.c`, `tools/inspect/setid.allow`.
* Mounting a scratch image and verification: kept; owners `krffs_fuse*.c`, `create_file_system.krffs.sh`, `tests/host/readfile_hosttest.c`, `mkhash_hosttest.c`.

## utils/Readme.md

* "This directory contains 29 programs ... Nineteen ordinary programs go under `build/utils/`; ten programs selected explicitly by image manifests go under `build/utils/sbin/`": stale, `programs.mk` names 25 in `UTILITIES` and 11 in `SBIN_PROGS`, 36 in all, and `build/utils/sbin` holds the eleven; fixed, and the counts read from `programs.mk` at the rewrite. "the 12 portable file utilities" of the native build: checked against `Makefile.linux` at the rewrite.
* The build, the guest build through `programs.mk` and `guest.mk`, `guestcheck`: kept; owners `utils/Makefile`, `utils/programs.mk`, `utils/guest.mk`, `tools/verify/guestcheck.c`.
* The portable file utilities, `loopctl`, the network programs, `dhcpc`'s holder file, the shared libraries, `resolv.c`: kept; owners those sources.
* The init and account programs, the credential library, the boot and recovery programs: kept; owners `init.c`, `login.c`, `su.c`, `sudo.c`, `passwd.c`, `adduser.c`, `pwauth.c`, `bootctl.c`, `reflash.c`, `growroot.c`, `growset.c`, `growboot.c`, `diskctl.c`, `rach2-install.sh`; the manual pages milestone 51 wrote are the operator's reference and this Readme keeps the component's view and links to them.

## ish/Readme.md

* The shell's syntax, its two builtins, the path search, the statuses, the redirection, the buffers: kept; owners `ish/ish.c`, `ish/sources.mk`, `ish/guest.mk`.

## man/Readme.md

* The sections, `make man`, `render.sh`, the aliases, the reader, the recovery list, `mandoc` as a required tool: kept; owners `man/render.sh`, `man/aliases`, `man/recovery`, `krffs/man.script`, `tools/setup/preflight.sh`; this is milestone 51's document and its contract is current.
* "every call `rach2/sys/syscall.c` registers": stale since the split, `mancheck` reads the colon-separated list the Makefile passes, `syscall.c` and `syscall_net.c`; fixed.
* "The os-real RAM image is 18 MiB to hold the manual": checked against the root Makefile's image rule at the rewrite.

## ports/doom/Readme.md

* The port, its build, the zone sized from `MemFree`, the platform interface, graphics and input, sound and music, the game data and the optional staging: kept; owners `ports/doom/Makefile`, `crt0.c`, `doomgeneric_rach2.c`, the sound sources, `fetch_wad.sh`, the manifests. "the 80 engine sources named by `DOOM_SRC`": current, the list has 80 C sources; kept.

## rach2/sys/ipc/Readme.md and rach2/sys/io/Readme.md

* The pipe's ring, its creation and lifetime, its reads and writes, and the readiness interfaces and their projections: kept; owners `ipc/pipe.c`, `ipc/pipe.h` (`PIPE_BUF_SIZE`), `io/poll.c`, `io/poll.h` (`POLL_MAX_FDS`). "a process can hold at most 16 descriptors": stale, the table grows from sixteen to `FD_TABLE_LIMIT` 1024; fixed.

## mk/Readme.md

* The output directories, the header dependencies, the clangd database, the kernel source list: kept; owners `mk/rules.mk`, `mk/deps.mk`, `mk/build.mk`, `mk/host.mk`, `mk/warnings.mk`, `mk/fingerprint.mk`, `mk/images.mk`, `mk/toolchain.mk`. The fragments phase 1 added, `host.mk`, `warnings.mk`, `fingerprint.mk` and `images.mk`, are described at the rewrite; the file predates them.

## The vendor Readmes

`vendor/musl/`, `vendor/busybox/`, `vendor/dropbear/`, `vendor/tcc/`, `vendor/pdpmake/`, `vendor/e2fsprogs/` and `vendor/grub/` each describe a pinned tarball, its digest, its configuration and its patches. Their owner is the `build_*.sh` script beside them and the pin file it reads. They were read against those scripts: the tarball names, the digests, the configure options and the staged outputs agree, and `vendor/e2fsprogs/Readme.md`'s size limits agree with the root Makefile's `E2FSCK_LIMIT` and `RESIZE2FS_LIMIT`. `vendor/grub/Readme.md` describes the six committed artifacts and `regen_grub.sh`; its provenance manifest is the owner of the digests.

## What this inventory found

The stale claims are of three kinds. The first is arithmetic a milestone changed and a document kept: the USB table entries, the hub count, the scratchpad pages, the message vectors' range, the core tier's suite count, the probe and program counts, the group limit, the disk's partitions. The second is a fixed pool that became a heap allocation: the mount table, the synthetic nodes, the vfat and ext2 node pools, the KRFFS volumes and their open records, the walk bitmaps and the check's inode set; every "N at once" sentence about them is gone. The third is a name a change moved: `syscall.c` for the calls the split moved, `/dev/usb0` for the stick nodes the registry renamed, `make sealed-test` and `os-real-grub-disk` for targets that no longer exist, `install-ext2` and `drill-ext2` for suites the one-root disk removed.

Every claim above has an owner in the tree or a record under `dev-log/`. None was left without one, so nothing is removed for want of evidence; what is removed is duplication, which the rewrite makes once and links.
