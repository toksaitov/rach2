# BusyBox patches

[`build_busybox.sh`](../build_busybox.sh) replaces the extracted BusyBox tree, then applies every `*.patch` in filename order before either configuration is built. The release archive remains unchanged. Project source adaptations belong here so that each change and its reason are reviewable.

## Current patches

* `0001-udhcpc-build-against-musl.patch` replaces two unavailable Linux packet headers in `networking/udhcp/dhcpc.c`. Musl's `netpacket/packet.h` supplies `sockaddr_ll` and `PACKET_AUXDATA`; the patch adds the auxiliary-data structure and status bit that the source also uses. The Linux filter header served only code disabled upstream with `#if 0`.
* `0002-free-uptime-include-sysinfo.patch` makes `procps/free.c` and `procps/uptime.c` include `sys/sysinfo.h` without testing `__linux__`. The bare `i686-elf-gcc` target does not define that macro, while the ported musl supplies the header and Rach2 implements `sysinfo`.
* `0003-mkfs-vfat-define-linux-requests.patch` replaces `util-linux/mkfs_vfat.c`'s includes of `linux/hdreg.h` and `linux/fd.h`, kernel headers musl does not install, with the two request numbers and structures the applet asks: `HDIO_GETGEO` with `struct hd_geometry`, and `FDGETPRM` with `struct floppy_struct`, as the i386 kernel defines them. The applet takes its defaults when either request fails.

Configuration choices stay in [`rach2.config`](../rach2.config) and [`rach2-suid.config`](../rach2-suid.config). A source patch should be limited to an adaptation that those configurations cannot express.

## Changing the set

Add a new patch with the next numeric prefix. Rebase every patch when changing the pinned BusyBox release. `patch -p1` must apply to a fresh extraction without an interactive correction.

The BusyBox stamp depends on this directory and on every current patch. Adding, editing, or deleting a patch invalidates the stamp, including deletion of the last patch. `make busybox` then recreates the tree and both configured binaries.

Run:

```
make busybox
make check-busybox
tests/run style
tests/run host
```

Run `tests/run dhcp` after changing the udhcpc patch and `tests/run procfiles` after changing the free or uptime patch.
