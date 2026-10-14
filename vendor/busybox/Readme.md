# BusyBox

BusyBox is the shell and the general command set of the os-real system and the images built from it. Rach2 builds it twice from one pinned source, statically against the ported [musl](../musl/Readme.md): a main binary with the shell and the ordinary applets, and a set-id binary with only `login`, `passwd` and `su`. The split keeps the shell and the other applets out of a program that runs as root, as BusyBox's own `Config.in` advises.

## Sources and license

The source is the release tarball from [busybox.net](https://busybox.net/downloads/) in this directory. [`build_busybox.sh`](build_busybox.sh) has its version in `VERSION` and its SHA-256 in `TARBALL_SHA256`, and refuses a tarball whose checksum differs; the root `Makefile` gives the same file as `BUSYBOX_TARBALL`. BusyBox is under version 2 of the GNU General Public License only, as its `LICENSE` says.

## Local changes

The tarball is not edited. The build applies the patches under `patches/` and copies in one of two configurations.

### Patches

`build_busybox.sh` applies every `patches/*.patch` with `patch -p1`, in name order, to a fresh extraction, before either configuration is built. A patch is only for an adaptation the configurations cannot express.

* `0001-udhcpc-build-against-musl.patch` replaces the includes of `linux/filter.h` and `linux/if_packet.h` in `networking/udhcp/dhcpc.c`, which musl does not install, with musl's `netpacket/packet.h`. That header has `sockaddr_ll` and `PACKET_AUXDATA` but not `struct tpacket_auxdata` or `TP_STATUS_CSUMNOTREADY`, so the patch defines both. The filter header served only a block upstream keeps under `#if 0`.
* `0002-free-uptime-include-sysinfo.patch` makes `procps/free.c` and `procps/uptime.c` include `sys/sysinfo.h` unconditionally. Upstream includes it only under `__linux__`, which `i686-elf-gcc` and `x86_64-elf-gcc` do not define.
* `0003-mkfs-vfat-define-linux-requests.patch` replaces the includes of `linux/hdreg.h` and `linux/fd.h` in `util-linux/mkfs_vfat.c` with the two requests the applet makes, `HDIO_GETGEO` with `struct hd_geometry` and `FDGETPRM` with `struct floppy_struct`, as the i386 Linux headers define them. The applet takes its defaults when either request fails.

### Configurations

[`rach2.config`](rach2.config) is the main binary and [`rach2-suid.config`](rach2-suid.config) the set-id one. Each is an input to Kconfig's `oldconfig`; the resolved `.config` and the generated `include/applet_tables.h` of each build are staged beside the binary, and those two files say what ships.

The main configuration is static, with 64-bit file offsets. `/bin/sh` is ash with job control, built standalone (`FEATURE_SH_STANDALONE`) with applets preferred, so ash finds an applet before it searches `PATH` and needs no file per applet; where it must execute one, it executes `/bin/busybox`, the `BUSYBOX_EXEC_PATH`. Its choices that differ from a desktop BusyBox:

* `login`, `passwd` and `su` are off; the set-id binary has them.
* `ping`, `ping6` and `traceroute6` are off. `/bin/ping` is the project's unprivileged program ([ping(1)](../../man/man1/ping.1)), and Rach2 has no IPv6 stack. `arping` and `traceroute` are on and need root.
* `dmesg` is off; the image's `/bin/dmesg` is the project's ([dmesg(1)](../../man/man1/dmesg.1)).
* `ifconfig`, `route`, `ifup`, `ifdown` and `udhcpc` are on, with `FEATURE_IFUPDOWN_IP` off, so ifupdown uses `ifconfig` and `route`.
* `adduser`, `deluser`, `addgroup`, `delgroup` and `chpasswd` are on, and they are the account programs of the BusyBox images.
* `cryptpw`, `mkpasswd` and `httpd` with `-m` are on, with SHA-512 as the default password hash.
* `mount`, `df`, `free`, `uptime`, `mkfs.vfat` and `mkdosfs` are on.
* SHA-1 and SHA-256 hardware acceleration are off.

The set-id configuration has `login`, `passwd` and `su` and nothing else: no shell (`SH_IS_NONE`), no installer, no individual applet binaries, and no `FEATURE_PREFER_APPLETS`. It turns off `FEATURE_SUID_CONFIG`, the parser of `/etc/busybox.conf`. In this release `check_suid` in `libbb/appletlib.c` frees the parsed list without clearing the global `suid_config`, so a later dispatch in the same process walks freed memory; with the feature off that code is not compiled. The main binary is not set-id, and no manifest stages `/etc/busybox.conf`.

## Building

`make busybox` builds for the architecture `ARCH` selects, i686 by default; `make busybox ARCH=x86_64` builds for x86-64. The root `make` builds it too, through the same stamp. The script takes the architecture from `RACH2_ARCH` through `mk/arch.sh`, which gives the cross prefix, `i686-elf-` or `x86_64-elf-`, and the directories. An i686 build extracts into this directory and stages under `build/vendor/busybox/`; an x86-64 build extracts under `build/x86_64/vendor-work/busybox/` and stages under `build/x86_64/vendor/busybox/`. Each has its own stamp.

`build_busybox.sh`:

1. Checks the tarball's SHA-256, deletes the extracted tree and extracts it again.
2. Applies `patches/*.patch` in name order.
3. Copies `rach2.config`, resolves it with `oldconfig`, and builds with `CROSS_COMPILE` set to the cross prefix, `CC` set to the `muslgcc` wrapper and `HOSTCC=cc`.
4. Strips the binary to `busybox` and stages `applet_tables-main.h` and `config-main` beside it.
5. Runs `make mrproper`, then repeats steps 3 and 4 with `rach2-suid.config`, staging `busybox-suid`, `applet_tables-suid.h` and `config-suid`.

Both binaries are stripped static executables, ELF32 i386 or ELF64 x86-64. The stamp depends on the script, the tarball, both configurations, `muslgcc`, `mk/arch.sh`, the musl stamp, the `patches` directory and every patch in it, so adding, changing or deleting a patch rebuilds both binaries. `make check-busybox` requires the six staged files to be nonempty. `make check-setid` runs [`setidcheck`](../../tools/inspect/setidcheck.c) twice: over the manifests against [`setid.allow`](../../tools/inspect/setid.allow), and with `--applets` over the staged tables and configurations, where it refuses a main table with `su`, `passwd`, `login`, `vlock`, `wall`, `crontab`, `ping`, `ping6`, `traceroute6` or `dmesg`, and a set-id table other than exactly `login`, `passwd` and `su`. `make clean` keeps the staged files; `make distclean` removes them, the extracted tree and the stamp.

The [manifests](../../krffs/manifests/) stage the main binary at mode 0755 as `/bin/busybox` and `/bin/sh`, and the set-id binary at mode 04755 under each applet's name, since KRFFS has no symbolic links and BusyBox dispatches on `argv[0]`.

## Updating

1. Put the new release tarball in this directory and delete the old one.
2. Change `VERSION` and `TARBALL_SHA256` in `build_busybox.sh`, and `BUSYBOX_TARBALL` in the root `Makefile`.
3. Rebase each patch so that `patch -p1` applies it cleanly to a fresh extraction, and delete one upstream has made unnecessary.
4. Build, then read `config-main`, `config-suid` and both applet tables for renamed symbols, changed defaults and new applets. Check whether the `check_suid` fault above is still present.
5. Give each new applet a manual page or a line in [`man/aliases`](../../man/aliases); `mancheck` reads the main applet table.
6. Run the checks under Tests.

## Limits

* BusyBox's account applets lock each file they rewrite with `F_SETLK`, but do not take `/etc/.pwlock`, the lock file of the project's account programs ([utils](../../utils/Readme.md)). A BusyBox writer and a project writer running at once can lose one update.
* `passwd`, `chpasswd`, `httpd -m`, and `cryptpw` and `mkpasswd` without `-S` make the salt from the process ID and the monotonic clock, not from the kernel's random source.
* The `init` applet is built but unused: the images run the project's `init`.

## Tests

`make check-busybox` and `make check-setid` run with every image build, and `tests/run style` runs `mancheck` over the main applet table. The suites that exercise BusyBox, all described in [suites.md](../../tests/system/suites.md), are `tests/run dhcp` (`ifup`, `udhcpc` and `arping`), `raw` (`traceroute`), `procfiles` (`ps`, `df`, `free` and `uptime`), `account` (the account applets, `mkpasswd` and the set-id `login`), `ext2` (`mkfs.vfat` on a loop device), `fatwrite` (FAT32 written from BusyBox), `ssh` (ash over a pty) and `selfbuild`. The `default` suite boots the test image, which has no BusyBox.
