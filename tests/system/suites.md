# The suites

One line per suite, in the order the pool starts them, from `tests/run inventory`: the tier, the external commands the suite needs, `qemu` being the emulator of the architecture `ARCH` names, the most guest memory its machines ask for at once, the architectures it runs on when it does not run on both, and what it proves, which is the sentence in `tests/system/suites.mk` that names it. `tests/run style` compares this file with that output.

* `gnu` (full tier; needs qemu, ssh, scp, sftp, debugfs; 2048 MiB): the GNU system on its ext2 root, driven over OpenSSH with nobody at the console, logged in to by key and by password, and each port, GCC, GNU make, GDB, the SpiderMonkey shell and chromium-headless-shell doing its main job on the machine.
* `install` (full tier; needs qemu, mformat, mmd, mcopy, mke2fs, debugfs, e2fsck; 128 MiB): the machine installs its own kernel.
* `selfbuild` (full tier; needs qemu, mformat, mmd, mcopy; 128 MiB): the machine builds its own kernel.
* `installer-repair` (full tier; needs qemu, qemu-system-x86_64, qemu-img, xorriso, shasum, ssh, ssh-keygen, mcopy, mmd, mdeltree, mtype, minfo, debugfs; 128 MiB): the other half, the machine that will not boot.
* `installer` (full tier; needs qemu, xorriso, shasum, ssh, ssh-keygen, mformat, mcopy, mtype, mdel; 512 MiB): the install stick and the program on it.
* `menu` (full tier; needs qemu, mformat, mmd, mcopy, mke2fs, debugfs, e2fsck; 128 MiB): the boot menu that can be rewritten, and the stub that cannot.
* `uefi` (full tier; needs qemu-system-x86_64, xorriso, ssh, mformat, mmd, mcopy; 512 MiB): the machine booted by UEFI firmware, OVMF, from its installed disk and that disk's recovery entry, from the install stick's image as a USB device and from the recovery ISO as a CD, each answering SSH, installed from the stick onto a 4 TiB NVMe disk whose root grows past 2 TiB, and booted by SeaBIOS after recovery writes a damaged primary GUID table again.
* `installer-rehearse` (full tier; needs qemu, qemu-img, xorriso, shasum; 512 MiB): the rehearsal of the machine sitting in its small form, minutes: the same rows RACH2_REHEARSE_LENOVO runs at the machine's size in most of an hour.
* `selfbuild-ext2` (full tier; needs qemu, mformat, mmd, mcopy, mke2fs, debugfs, e2fsck; 128 MiB): the self-build on an ext2 root, the os-real system at 48 MiB on a partition at LBA 2048 as the KRFFS disk has it, booted with rach2.root=ext2; the suite reads the volume back with debugfs and e2fsck must find it consistent after the synced stop.
* `sealed` (full tier; needs qemu, ssh, mformat, mmd, mcopy; 128 MiB): the disk that goes into the laptop, operated the way it will be operated once it is in there.
* `dhcp-e1000` (full tier; needs qemu; 128 MiB): the dhcp suite's card half on the e1000 card.
* `dhcp` (full tier; needs qemu; 128 MiB): the machine taking an address it was not told, with both of its clients: BusyBox's `udhcpc`, which `ifup` runs, and os-simple's own `dhcpc`, which its `netup` runs.
* `netif` (full tier; needs qemu; 128 MiB): the interface ioctls, which is how a program sets this machine's address.
* `netif-e1000` (full tier; needs qemu; 128 MiB): the netif suite's card half on the e1000 card.
* `mount` (core tier; needs qemu, mformat, mcopy, mdir; 128 MiB): the partition nodes, the block path through a device that sleeps or never answers, and mount(2) and the claims it makes (see tests/system/suites/mount.sh).
* `fdlock` (full tier; needs qemu; 128 MiB): pending KRFFS relocation, orphan truncation, and concurrent raw offsets.
* `grow` (full tier; needs qemu, mke2fs, e2fsck, qemu-img, xorriso; 512 MiB): a 1 GiB ext2 root grown by `growroot` on the recovery system to a partition the size of the installed machine's disk, its writes checked against the grow set, a grow killed and repaired, one interrupted between the entry and the grow, and the memory it needs.
* `drill` (full tier; needs qemu, ssh, mformat, mmd, mcopy, mke2fs, debugfs, e2fsck; 128 MiB): the recovery drill, boots of one disk that carries the same three partitions the sealed disk has.
* `account` (full tier; needs qemu; 128 MiB): accounts created, authenticated, changed and deleted on both images, since both have account tools and they are different code.
* `large` (full tier; needs qemu, mke2fs, e2fsck, mformat, either hdiutil and newfs_exfat or mkfs.exfat and fsck.exfat; 128 MiB): a file past 4 GiB on ext2 and on exFAT, written, read back after a remount, sought, sized, truncated, appended and shared between two descriptors, and a write past each format's limit refused by name.
* `usb` (core tier; needs qemu; 128 MiB): the USB stack, from a port to a keystroke, in the boots that `tests/system/suites/usb.sh` counts and describes.
* `raw-e1000` (full tier; needs qemu; 128 MiB): the raw suite's card half on the e1000 card.
* `raw` (full tier; needs qemu; 128 MiB): the raw ICMP socket, which is what `ping` and `traceroute` are.
* `krffsmount` (full tier; needs qemu; 128 MiB): a KRFFS volume placed by mount(2), read through to its last byte and unmounted, and the three volumes the kernel refuses.
* `diskrate` (full tier; needs qemu, mformat; 512 MiB): the storage paths timed in the machine and held under bounds: the raw device, a file on exFAT and on FAT32 written and read, two hundred small files, and a raw read piped into a hash.
* `usbdisk` (core tier; needs qemu, xorriso, mformat, mcopy, mtype; 128 MiB): a flash drive as a disk.
* `stress-ahci` (full tier; needs qemu; 128 MiB): the stress suite's on-disk KRFFS larger than the page cache, on an AHCI controller.
* `default` (core tier; needs qemu; 128 MiB): the probe suite, every guest probe booted on the test image in one machine and scored by the runner.
* `stress` (full tier; needs qemu; 128 MiB): prove an on-disk KRFFS larger than the page cache works.
* `procfiles` (full tier; needs qemu; 128 MiB): the machine's own /proc files, on the boots that differ and through the programs that read them.
* `bigmem` (full tier; needs qemu; 73728 MiB; x86_64 only): all of an x86-64 machine's eight gigabytes is counted, and a program's pages fill memory past what lies below 4 GiB and read back as written; without 1 GiB pages, 72 GiB is counted to the direct map's 64 GiB (see tests/system/suites/bigmem.sh).
* `memory` (core tier; needs qemu; 4096 MiB): the kernel's account of physical memory at five sizes (see tests/system/suites/memory.sh).
* `parts` (core tier; needs qemu, mformat, mtype; 128 MiB): four tables on four AHCI disks (an MBR with logical partitions, a GPT, a GPT read from its backup, a blank disk), the nodes as the tables number them, the table read again, a 5 TiB disk's numbers past 32 bits in /sys/block and /proc/partitions, and the root chosen by identity: found when the disks are swapped, refused when two disks have it.
* `bootpart` (full tier; needs qemu; 128 MiB): /proc/cmdline and the level-one disk devices.
* `loopdev` (full tier; needs qemu, mformat, mcopy, mtype, mke2fs; 128 MiB): a file system in a file, through a loop device: a FAT32 image and an ext2 image on a FAT32 volume, each mounted, written and read back, and everything an attachment refuses.
* `tcc` (full tier; needs qemu; 128 MiB): the machine compiling a program on itself.
* `make` (full tier; needs qemu; 128 MiB): the machine building a program on itself, and the claim a compiler alone cannot make: that a second build does nothing, that touching one source rebuilds one object, and that the answer survives a reboot.
* `smp` (full tier; needs qemu; 512 MiB): the kernel on one processor, on four and on two of four, each finding its processors and passing a stress of fork, exec, pipes, futexes and munmap across them and sleeps on the last, the debug shell's preemption test and its ring-3 payload on two, an NMI on two that must stop the machine as a fault of the machine, and a parallel build on the installed system that runs on every one of four (see tests/system/suites/smp.sh).
* `host` (full tier; needs nothing beyond the build; no machine): the host tests, kernel code compiled and run natively under the sanitizers, and the fuzz corpora replayed through their harnesses.
* `noentropy` (full tier; needs qemu; 128 MiB): the entropy policy's fail-closed half, which no other target can reach.
* `shutdown` (full tier; needs qemu; 128 MiB): userland stopping the machine, on a disk fixture, with every claim of a stop asserted on the QEMU process ending rather than on a token, and every image checked by offline fsck afterwards.
* `fatwrite` (core tier; needs qemu, mformat, mcopy, mdir, python3; 128 MiB): FAT32 written from a program, read back after a reboot, judged by the host's walk and mtools, taken through a crash and its repair, written from BusyBox, and written by two processes under the lag.
* `ssh` (core tier; needs qemu, ssh, nc; 128 MiB): logins over SSH with nobody at the keyboard.
* `ssh-e1000` (full tier; needs qemu, ssh; 128 MiB): the ssh suite on the e1000 card.
* `exfat` (core tier; needs qemu, python3; 128 MiB): exFAT written from a program, read back after a reboot, judged by the host's reader and the external checker where the host has one, taken through a crash and its repair, written from BusyBox and by two processes under the lag, and unmounted after a failed write with its dirty bit still set.
* `ext2` (core tier; needs qemu, mke2fs, e2fsck, mtype; 128 MiB): the ext2 driver booted over a fixture disk the host's mke2fs wrote, with the walker's link rows on the mounted volume.
* `cred-persist` (full tier; needs qemu; 128 MiB): the credential replacement's durability across a reboot.
* `packet-e1000` (full tier; needs qemu; 128 MiB): the packet suite on the e1000 card.
* `addressed` (full tier; needs qemu; 128 MiB): the address named on the boot line, and the proof that it is the one in force.
* `ping` (full tier; needs qemu; 128 MiB): the ICMP path against the SLIRP gateway, which is why it is its own target rather than a probe in the suite.
* `packet` (full tier; needs qemu; 128 MiB): the packet socket, which is how a DHCP client sees a reply that arrives before this machine owns an address.
* `ping-e1000` (full tier; needs qemu; 128 MiB): the ping suite on the e1000 card.
* `load` (full tier; needs qemu, mformat; 128 MiB): the machine under load for twenty seconds and the longest delays it saw, from `/proc/stat`, within their bounds.
* `authority` (core tier; needs qemu; 128 MiB): which kernel may write the whole disk.
* `queue` (core tier; needs qemu, mformat, mke2fs; 128 MiB): the AHCI disks on their interrupts: a throttled disk beside a fast one, and the fallback root's faults beside the queue.
* `persist-ahci` (full tier; needs qemu; 128 MiB): fsync durability across a reboot with the disk on an AHCI controller.
* `subset` (core tier; needs qemu; 128 MiB): each optional device probe left out, and proved left out.
* `bus` (full tier; needs qemu, mformat, mtype; 128 MiB): four IDE drives on both channels, an AHCI and an IDE disk together with the root on each in turn, and a disk past 128 GiB.
* `exitline` (full tier; needs qemu; 128 MiB): the plain process-exit line is printed under the debug shell and nowhere else.
* `login` (core tier; needs qemu; 128 MiB): prove the console cannot fall back to an unauthenticated shell.
* `man` (core tier; needs qemu; 128 MiB): the manual pages on the machine.
* `metal` (core tier; needs qemu, ssh, mformat, mcopy, mmd, mke2fs, debugfs, e2fsck, python3; 128 MiB): unattended SSH on the metal image and acceptance of an installed ext2 root.
* `bootline` (full tier; needs qemu, mke2fs; 128 MiB): the boot line that contradicts itself.
* `servicelog` (full tier; needs qemu, mke2fs; 128 MiB): the log init's services write to is bounded within a boot, and a log a boot left full is removed at the next.
* `persist` (core tier; needs qemu; 128 MiB): fsync durability across a reboot.
* `style` (full tier; needs cppcheck, shellcheck, clang, clang-format; no machine): the source checks over the tree, tools/verify/check.sh, booting nothing.
* `dgram-e1000` (full tier; needs qemu; 128 MiB): the dgram suite on the e1000 card.
* `dgram` (core tier; needs qemu; 128 MiB): a connected datagram socket and the permission to broadcast, which is how a DHCP client renews a lease and how it asks for one.
* `diskdiag` (full tier; needs qemu, ssh; 128 MiB): a disk read without being mounted and without being written.
* `e1000` (core tier; needs qemu, ssh; 128 MiB): the network path on the other emulated card, and the probe order that decides which card a machine with more than one of them gets.
* `disk-ahci` (core tier; needs qemu; 128 MiB): the disk suite's page-cache integrity check with the disk on an AHCI controller.
* `grub` (core tier; needs qemu, mformat, mmd, mcopy; 128 MiB): the machine replacing its own boot loader.
* `disk-q35` (full tier; needs qemu; 128 MiB): the same disk proof on the q35 machine, whose AHCI controller is built in at 0:31.2, function 2 of the multi-function device 31.
* `doom` (full tier; needs qemu; 128 MiB): the Doom port's zone on a disk root, sized from the machine's free memory through a heap that grows with brk; one boot.
* `disk` (core tier; needs qemu; 128 MiB): the disk-backed page-cache integrity check.
* `diskpolicy` (full tier; needs qemu; 128 MiB): the read-only kernel does not write the disk, shown on the bytes rather than on a return value.
* `console` (core tier; needs qemu; 128 MiB): the two probes that read from the console, which the shared probe boot cannot run because they block until something is typed.
* `fpu` (core tier; needs qemu; 128 MiB): the floating-point state of a process is its own.
* `socket` (core tier; needs qemu; 128 MiB): the guest opening connections of its own, checked by a host that answers (see tests/system/suites/socket.sh).
* `bootorder` (full tier; needs qemu, mformat, mmd, mcopy; 128 MiB): the GRUB disk on the second AHCI port behind a disk holding nothing, booted by the firmware, its root found as disk 1.
* `reset` (core tier; needs qemu; 128 MiB): each reset mechanism on its own.
* `netconsole-e1000` (full tier; needs qemu; 128 MiB): the netconsole suite on the e1000 card.
* `disks` (core tier; needs qemu, mformat, mtype; 128 MiB): two disks of different sizes on one AHCI controller, both in /proc/disk with their ports and sizes, both with nodes, a partition of each mounted and written, and the second disk's file found on the second disk by the host.
* `audio` (core tier; needs qemu; 128 MiB): what the machine actually played.
* `netconsole` (core tier; needs qemu; 128 MiB): the kernel log on the wire.
* `crash` (core tier; needs qemu; 128 MiB): prove the dirty-mount flag detects an unclean shutdown.
* `pools` (core tier; needs qemu; 128 MiB): many pipes, ptys, sockets and processes live at once, and the kernel heap back where it started after a thousand each of pipes, ptys and sockets (see tests/poolprobe.c).
* `runner` (core tier; needs nothing beyond the build; no machine): the harness's selftest, then the runner's own, tests/run driven against a registry and a makefile of its own in a temporary directory (tests/system/selftest.sh).
* `diskfull` (core tier; needs qemu; 128 MiB): a file system with no room left.
* `fault` (core tier; needs qemu; 128 MiB): the three probes whose whole claim is that the kernel killed them.
* `imagerace` (full tier; needs nothing beyond the build; no machine): two builds of one image at the same moment, as a parallel run of the suites can make them, both of which must finish and leave the image whole.
* `krffscheck` (full tier; needs python3; no machine): what the offline checker says about images broken on purpose.
* `conload` (core tier; needs qemu, mformat; 128 MiB): every line typed at the console under load arrives, numbered and checksummed, with the reader parked, as type-ahead, after an interrupt character to a foreground child, and across an exec.
* `drive` (core tier; needs nothing beyond the build; no machine): the drivers' waits, drive.sh and the harness against fake machines that choose when they are ready, booting nothing.
