# rach2(7)

NAME: rach2 - an educational operating system for the 32-bit PC.

The entry point of the manual: what the system is, what it has, where to look. Sources: Readme.md (System, Images, Limits, Hardware status), rach2/Readme.md (Architecture, Root selection, Kernel command line, Hardware and limits), man/Readme.md.

Facts:
* A kernel in freestanding C89, no libgcc, for the i386 PC; a userland of the ported musl C library, BusyBox (the shell and the tools), the Dropbear SSH server, TinyCC as `/bin/tcc` and `/bin/cc`, pdpmake as `/bin/make`, and this project's own programs (section 8 and section 1 pages name them). The system rebuilds its own kernel: `/src/rach2`, `/src/utils`, `/src/ish` are staged on the images that carry the compiler, each with a `guest.mk` pdpmake reads.
* Boot: Multiboot1 through QEMU's `-kernel` or GRUB from a disk or a stick. The root is a KRFFS image in memory (the test, os-simple, os-real and recovery images), or a KRFFS or ext2 partition of a disk selected by identity on the kernel command line (`rach2.root=`); `rach2(7)` points to the kernel's own Readme for the command line and to disk(7) for the identities. `rach2.rescue` enters the kernel debug shell instead of `/sbin/init`.
* File systems: FAT32 (read and write), exFAT (read; mounts read-only), ext2 (read and write, hard and symbolic links), KRFFS (this project's, read and write); a regular file mounts through a loop device (`/dev/loop0` to `/dev/loop3`).
* Devices: ATA and AHCI disks; USB keyboards, mice and flash drives through xHCI, on a root port or behind a hub; RTL8139, e1000 (Intel 82540EM) and RTL8168 network cards; PS/2 keyboard and mouse; VGA text and mode 13h graphics; COM1 serial; Sound Blaster 16, Intel HDA and the PC speaker.
* Not implemented: UEFI, Multiboot2, SMP, threads, swap, dynamic linking (every program is static), IPv6, VESA graphics.
* The images: test (probes), os-simple (this project's own init, login, su, passwd, adduser, sudo), os-real (BusyBox's shell, login and su, Dropbear, the compilers and the sources), rach2-metal-disk (os-real for the machine's own disk, with init-metal supervising Dropbear), recovery (a RAM system with the installer and the repair tools).
* Where to look: intro to the sections: 1 programs, 2 system calls, 4 devices, 5 file formats, 7 overviews (disk(7), recovery(7), limits(7), memory(7)), 8 administration (rach2-install(8), rach2-recover(8), bootctl(8), growroot(8), reflash(8), loopctl(8), mount(8)). `/proc` files: proc(5) (phase 3). The manual itself: man(1), under `/usr/share/man/catN` as plain text.
* FILES: /usr/share/man, /src, /proc, /dev, /etc/rach2 (limits, sshd.conf), /var/log/services (what init's services print), /dev/bootlog (this boot's kernel log, for root).
* HISTORY: developed as a teaching system; its milestones are recorded under `dev-log` in the source tree.
