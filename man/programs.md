# The programs of Rach2

Rach2 has two systems. The musl system is the static programs this repository builds. BusyBox, Dropbear, sox_ng, wpa_supplicant, iw, TinyCC, pdpmake, e2fsprogs and the in-house tools other than `ish` are linked against the ported musl libc; `ish`, the in-house shell, and Doom link no C library and have their own startup code and library routines. `make` builds it on every supported host, and every image stages it or a part of it. The GNU system is a dynamically linked root of glibc and the programs ported to it, installed over the musl system in an ext2 image; it builds on an x86-64 Linux host only.

`man/programs.sh` writes this file from the image manifests, the recipes and the build scripts, and `tests/run style` fails when the two differ. `make programs-md` writes it again.

## The musl system

### Programs

The programs of os-real, as [its manifest](../krffs/manifests/os-real.manifest) stages them, with the page of each and the project it comes from. A program built under another name has that name beside its project. The os-simple and recovery columns say yes where [os-simple](../krffs/manifests/os-simple.manifest) or [the recovery image](../krffs/manifests/rach2-recovery.manifest) stages the same program, give the project where it stages another program under the name, and are empty where it stages none.

| Program | Page | Project | os-simple | Recovery |
| --- | --- | --- | --- | --- |
| `audiotest` | [audiotest(1)](man1/audiotest.1) | in-house | yes |  |
| `bootctl` | [bootctl(8)](man8/bootctl.8) | in-house |  | yes |
| `busybox` | [sh(1)](man1/sh.1) | BusyBox |  | yes |
| `cc` | [tcc(1)](man1/tcc.1) | TinyCC (`tcc`) |  |  |
| `dmesg` | [dmesg(1)](man1/dmesg.1) | in-house (`dmesg.script`) |  | yes |
| `doom` | [doom(1)](man1/doom.1) | Doom |  |  |
| `dropbearmulti` | [dropbear(8)](man8/dropbear.8) | Dropbear |  | yes |
| `dumpkeys` | [dumpkeys(1)](man1/dumpkeys.1) | kbd |  |  |
| `init` | [init(8)](man8/init.8) | in-house (`init-login`) | yes | in-house (`init-recovery`) |
| `iw` | [iw(8)](man8/iw.8) | iw |  | yes |
| `kbd_mode` | [kbd_mode(1)](man1/kbd_mode.1) | kbd |  |  |
| `kbdrate` | [kbdrate(8)](man8/kbdrate.8) | kbd |  | yes |
| `loadkeys` | [loadkeys(1)](man1/loadkeys.1) | kbd |  | yes |
| `login` | [login(1)](man1/login.1) | BusyBox (`busybox-suid`) | in-house |  |
| `loopctl` | [loopctl(8)](man8/loopctl.8) | in-house | yes |  |
| `make` | [make(1)](man1/make.1) | pdpmake |  |  |
| `man` | [man(1)](man1/man.1) | in-house (`man.script`) |  | yes |
| `mixer` | [mixer(1)](man1/mixer.1) | in-house | yes | yes |
| `mke2fs` | [mke2fs(8)](man8/mke2fs.8) | e2fsprogs |  |  |
| `passwd` | [passwd(1)](man1/passwd.1) | BusyBox (`busybox-suid`) | in-house | yes |
| `play` | [play(1)](man1/play.1) | sox_ng (`mode`) |  |  |
| `rec` | [rec(1)](man1/rec.1) | sox_ng (`mode`) |  |  |
| `setleds` | [setleds(1)](man1/setleds.1) | kbd |  | yes |
| `sh` | [sh(1)](man1/sh.1) | BusyBox | in-house (`ish`) | BusyBox (`busybox`) |
| `showkey` | [showkey(1)](man1/showkey.1) | kbd |  |  |
| `sox` | [sox(1)](man1/sox.1) | sox_ng |  |  |
| `soxi` | [soxi(1)](man1/soxi.1) | sox_ng (`mode`) |  |  |
| `su` | [su(1)](man1/su.1) | BusyBox (`busybox-suid`) | in-house | yes |
| `tcc` | [tcc(1)](man1/tcc.1) | TinyCC |  |  |
| `update` | [update(8)](man8/update.8) | in-house | yes | yes |
| `wifi` | [wifi(1)](man1/wifi.1) | in-house | yes | yes |
| `wpa_cli` | [wpa_cli(8)](man8/wpa_cli.8) | hostap |  | yes |
| `wpa_passphrase` | [wpa_passphrase(8)](man8/wpa_passphrase.8) | hostap |  | yes |
| `wpa_supplicant` | [wpa_supplicant(8)](man8/wpa_supplicant.8) | hostap |  | yes |
| `wsimrelay` | [wsimrelay(8)](man8/wsimrelay.8) | in-house |  |  |
| `zzz` | [zzz(8)](man8/zzz.8) | in-house |  | yes |

os-real stages `doom` only when the build has made it.

### Programs os-real does not have

The programs os-simple or the recovery image stages that os-real does not, with the project each comes from.

| Program | Page | os-simple | Recovery |
| --- | --- | --- | --- |
| `adduser` | [adduser(8)](man8/adduser.8) | in-house |  |
| `cat` | [busybox-coreutils(1)](man1/busybox-coreutils.1) | in-house |  |
| `cp` | [busybox-coreutils(1)](man1/busybox-coreutils.1) | in-house |  |
| `dhcpc` | [dhcpc(8)](man8/dhcpc.8) | in-house |  |
| `diskctl` | [diskctl(8)](man8/diskctl.8) | in-house | in-house |
| `e2fsck` | [e2fsck(8)](man8/e2fsck.8) |  | e2fsprogs |
| `echo` | [busybox-coreutils(1)](man1/busybox-coreutils.1) | in-house |  |
| `false` | [busybox-coreutils(1)](man1/busybox-coreutils.1) | in-house |  |
| `growboot` | [growboot(8)](man8/growboot.8) | in-house | in-house |
| `growroot` | [growroot(8)](man8/growroot.8) | in-house | in-house |
| `growset` | [growset(8)](man8/growset.8) | in-house | in-house |
| `halt` | [halt(8)](man8/halt.8) | in-house |  |
| `httpd` | [httpd(8)](man8/httpd.8) | in-house |  |
| `ish` | [ish(1)](man1/ish.1) | in-house |  |
| `ls` | [busybox-coreutils(1)](man1/busybox-coreutils.1) | in-house |  |
| `mkdir` | [busybox-coreutils(1)](man1/busybox-coreutils.1) | in-house |  |
| `mv` | [busybox-coreutils(1)](man1/busybox-coreutils.1) | in-house |  |
| `nc` | [nc(1)](man1/nc.1) | in-house |  |
| `ndwatch` | [ndwatch(8)](man8/ndwatch.8) | in-house | in-house |
| `netup` | [netup(8)](man8/netup.8) | in-house |  |
| `ping` | [ping(1)](man1/ping.1) | in-house |  |
| `pwd` | [busybox-coreutils(1)](man1/busybox-coreutils.1) | in-house |  |
| `rach2-install` | [rach2-install(8)](man8/rach2-install.8) |  | in-house |
| `rach2-recover` | [rach2-recover(8)](man8/rach2-recover.8) |  | in-house |
| `reflash` | [reflash(8)](man8/reflash.8) | in-house | in-house |
| `rescue-static` | [rescue-static(8)](man8/rescue-static.8) |  | in-house (`rescue-static.script`) |
| `resize2fs` | [resize2fs(8)](man8/resize2fs.8) |  | e2fsprogs |
| `rm` | [busybox-coreutils(1)](man1/busybox-coreutils.1) | in-house |  |
| `touch` | [busybox-coreutils(1)](man1/busybox-coreutils.1) | in-house |  |
| `true` | [busybox-coreutils(1)](man1/busybox-coreutils.1) | in-house |  |
| `yes` | [busybox-coreutils(1)](man1/busybox-coreutils.1) | in-house |  |

### BusyBox applets

The `/bin/busybox` of os-real, the recovery image and the GNU root has 320 applets. Its ash finds an applet before it searches `PATH`, so a KRFFS image, which has no symbolic links, needs no file for each applet ([BusyBox](../vendor/busybox/Readme.md)); on the GNU root, where `/bin/sh` is bash, each applet that no port provides is a link to `/bin/busybox`. Each row is a page and the applets it covers.

| Page | Applets |
| --- | --- |
| [adduser(8)](man8/adduser.8) | `adduser` |
| [busybox-archival(1)](man1/busybox-archival.1) | `bunzip2`, `bzcat`, `bzip2`, `cpio`, `dpkg`, `dpkg-deb`, `gunzip`, `gzip`, `lzcat`, `lzma`, `lzop`, `rpm`, `rpm2cpio`, `tar`, `unlzma`, `unxz`, `unzip`, `xz`, `xzcat`, `zcat` |
| [busybox-console-tools(1)](man1/busybox-console-tools.1) | `clear`, `reset`, `resize`, `setlogcons` |
| [busybox-coreutils(1)](man1/busybox-coreutils.1) | `[`, `[[`, `arch`, `base32`, `base64`, `basename`, `cat`, `chgrp`, `chmod`, `chown`, `chroot`, `cksum`, `comm`, `cp`, `crc32`, `cut`, `date`, `dd`, `df`, `dirname`, `dos2unix`, `du`, `echo`, `env`, `expand`, `expr`, `factor`, `false`, `fold`, `fsync`, `groups`, `head`, `hostid`, `id`, `install`, `link`, `ln`, `logname`, `ls`, `md5sum`, `mkdir`, `mkfifo`, `mknod`, `mktemp`, `mv`, `nice`, `nl`, `nohup`, `nproc`, `od`, `paste`, `printenv`, `printf`, `pwd`, `readlink`, `realpath`, `rm`, `rmdir`, `seq`, `sha1sum`, `sha256sum`, `sha3sum`, `sha512sum`, `shred`, `shuf`, `sleep`, `sort`, `split`, `stat`, `stty`, `sum`, `sync`, `tac`, `tail`, `tee`, `test`, `timeout`, `touch`, `tr`, `true`, `truncate`, `tsort`, `tty`, `uname`, `unexpand`, `uniq`, `unix2dos`, `unlink`, `users`, `usleep`, `uudecode`, `uuencode`, `w`, `wc`, `who`, `whoami`, `yes` |
| [busybox-debianutils(1)](man1/busybox-debianutils.1) | `pipe_progress`, `run-parts`, `start-stop-daemon`, `which` |
| [busybox-e2fsprogs(8)](man8/busybox-e2fsprogs.8) | `chattr`, `fsck`, `lsattr` |
| [busybox-editors(1)](man1/busybox-editors.1) | `awk`, `cmp`, `diff`, `ed`, `patch`, `sed`, `vi` |
| [busybox-findutils(1)](man1/busybox-findutils.1) | `egrep`, `fgrep`, `find`, `grep`, `xargs` |
| [busybox-init(8)](man8/busybox-init.8) | `bootchartd`, `linuxrc`, `poweroff`, `reboot` |
| [busybox-klibc-utils(1)](man1/busybox-klibc-utils.1) | `resume` |
| [busybox-loginutils(8)](man8/busybox-loginutils.8) | `add-shell`, `addgroup`, `chpasswd`, `cryptpw`, `delgroup`, `deluser`, `mkpasswd`, `remove-shell` |
| [busybox-mailutils(1)](man1/busybox-mailutils.1) | `makemime`, `popmaildir`, `reformime`, `sendmail` |
| [busybox-miscutils(1)](man1/busybox-miscutils.1) | `adjtimex`, `ascii`, `bc`, `chat`, `crond`, `dc`, `devmem`, `hexedit`, `less`, `lsscsi`, `makedevs`, `microcom`, `mim`, `mt`, `readahead`, `runlevel`, `rx`, `setfattr`, `setserial`, `strings`, `time`, `tree`, `ts`, `ttysize`, `volname` |
| [busybox-modutils(8)](man8/busybox-modutils.8) | `depmod`, `insmod`, `lsmod`, `modinfo`, `modprobe`, `rmmod` |
| [busybox-networking(1)](man1/busybox-networking.1) | `arp`, `arping`, `dnsd`, `dnsdomainname`, `fakeidentd`, `ftpd`, `ftpget`, `ftpput`, `hostname`, `ifconfig`, `ifdown`, `ifup`, `inetd`, `ipcalc`, `netstat`, `nslookup`, `ntpd`, `pscan`, `route`, `ssl_client`, `tcpsvd`, `telnet`, `telnetd`, `tftp`, `tftpd`, `traceroute`, `udhcpc`, `udpsvd`, `vconfig`, `wget`, `whois` |
| [busybox-printutils(1)](man1/busybox-printutils.1) | `lpd`, `lpq`, `lpr` |
| [busybox-procps(1)](man1/busybox-procps.1) | `free`, `fuser`, `iostat`, `kill`, `killall`, `killall5`, `lsof`, `mpstat`, `nmeter`, `pgrep`, `pidof`, `pkill`, `pmap`, `powertop`, `ps`, `pstree`, `pwdx`, `smemcap`, `sysctl`, `top`, `uptime`, `watch` |
| [busybox-runit(8)](man8/busybox-runit.8) | `chpst`, `envdir`, `envuidgid`, `runsv`, `runsvdir`, `setuidgid`, `softlimit`, `sv`, `svc`, `svlogd`, `svok` |
| [busybox-shell(1)](man1/busybox-shell.1) | `ash`, `cttyhack`, `hush` |
| [busybox-sysklogd(8)](man8/busybox-sysklogd.8) | `klogd`, `logger`, `logread`, `syslogd` |
| [busybox-util-linux(1)](man1/busybox-util-linux.1) | `blkid`, `cal`, `chrt`, `fallocate`, `fatattr`, `fbset`, `fdflush`, `fdformat`, `fdisk`, `findfs`, `flock`, `freeramdisk`, `fsck.minix`, `getopt`, `hd`, `hexdump`, `hwclock`, `ipcrm`, `ipcs`, `last`, `linux32`, `linux64`, `lspci`, `lsusb`, `mesg`, `mkdosfs`, `mkfs.minix`, `mkfs.vfat`, `mkswap`, `more`, `mountpoint`, `nologin`, `nsenter`, `pivot_root`, `rdate`, `rdev`, `readprofile`, `renice`, `rev`, `rtcwake`, `script`, `scriptreplay`, `setarch`, `setsid`, `swapoff`, `swapon`, `taskset`, `unshare`, `xxd` |
| [halt(8)](man8/halt.8) | `halt` |
| [httpd(8)](man8/httpd.8) | `httpd` |
| [init(8)](man8/init.8) | `init` |
| [mount(8)](man8/mount.8) | `mount`, `umount` |
| [nc(1)](man1/nc.1) | `nc` |
| [sh(1)](man1/sh.1) | `busybox`, `sh` |

## The GNU system

The [GNU layer](../vendor/gnu/Readme.md) builds a cross toolchain with glibc, the ports below, binutils, GCC and GDB to run on Rach2, the SpiderMonkey shell, and Debian's desktop packages, and installs them over the musl system in the os-real-gnu root, an ext2 image. The path finds a port's program before a program of the musl system of the same name, and the ports' pages are on the machine but not in this repository; [the layer's Readme](../vendor/gnu/Readme.md#on-the-machine) has the details.

### The root over os-real

Besides the ports, [the root's manifest](../krffs/manifests/os-real-gnu.manifest) stages these programs that os-real does not.

| Program | Path | Page | Project |
| --- | --- | --- | --- |
| `chfn` | `/usr/bin/chfn` | on the machine | [shadow](../vendor/gnu/ports/shadow.sh) |
| `chsh` | `/usr/bin/chsh` | on the machine | [shadow](../vendor/gnu/ports/shadow.sh) |
| `crontab` | `/usr/bin/crontab` | on the machine | [cronie](../vendor/gnu/ports/cronie.sh) |
| `dumpe2fs` | `/sbin/dumpe2fs` | [dumpe2fs(8)](man8/dumpe2fs.8) | e2fsprogs |
| `e2fsck` | `/sbin/e2fsck` | [e2fsck(8)](man8/e2fsck.8) | e2fsprogs |
| `gpasswd` | `/usr/bin/gpasswd` | on the machine | [shadow](../vendor/gnu/ports/shadow.sh) |
| `ndwatch` | `/sbin/ndwatch` | [ndwatch(8)](man8/ndwatch.8) | in-house |
| `newgrp` | `/usr/bin/newgrp` | on the machine | [util-linux](../vendor/gnu/ports/util-linux.sh) |
| `resize2fs` | `/sbin/resize2fs` | [resize2fs(8)](man8/resize2fs.8) | e2fsprogs |
| `sudo` | `/usr/bin/sudo` | on the machine | [sudo](../vendor/gnu/ports/sudo.sh) |
| `term` | `/bin/term` | [term(1)](man1/term.1) | in-house |
| `tune2fs` | `/sbin/tune2fs` | [tune2fs(8)](man8/tune2fs.8) | e2fsprogs |
| `wall` | `/usr/bin/wall` | on the machine | [util-linux](../vendor/gnu/ports/util-linux.sh) |
| `write` | `/usr/bin/write` | on the machine | [util-linux](../vendor/gnu/ports/util-linux.sh) |

It does not stage `/bin/dropbearmulti`, `/bin/iw`, `/bin/sh`, `/bin/wpa_cli`, `/bin/wpa_passphrase` and `/bin/wpa_supplicant`, which os-real does; and `/sbin/init` is `init-metal`, where os-real has `init-login`.

### Desktop packages

The pinned [Debian desktop layer](../vendor/gnu/debian/Readme.md) stages these commands on both architectures. Mesa also supplies architecture-qualified names beside its canonical commands. The original Debian pages are rendered on the machine; the project pages below retain their text or document the tool family.

| Program | Package | Path | Page |
| --- | --- | --- | --- |
| `Xwayland` | `xwayland` | `/usr/bin/Xwayland` | [Xwayland(1)](man1/Xwayland.1) |
| `amd_hdmi_compliance` | `intel-gpu-tools` | `/usr/bin/amd_hdmi_compliance` | [intel-gpu-tools(1)](man1/intel-gpu-tools.1) |
| `amdgpu_stress` | `libdrm-tests` | `/usr/bin/amdgpu_stress` | [drm-tools(1)](man1/drm-tools.1) |
| `appres` | `x11-utils` | `/usr/bin/appres` | [appres(1)](man1/appres.1) |
| `chromium` | `chromium` | `/usr/bin/chromium` | [chromium(1)](man1/chromium.1) |
| `chromium-headless-shell` | `chromium-headless-shell` | `/usr/bin/chromium-headless-shell` | [chromium(1)](man1/chromium.1) |
| `dbus-cleanup-sockets` | `dbus-bin` | `/usr/bin/dbus-cleanup-sockets` | [dbus-cleanup-sockets(1)](man1/dbus-cleanup-sockets.1) |
| `dbus-daemon` | `dbus-daemon` | `/usr/bin/dbus-daemon` | [dbus-daemon(1)](man1/dbus-daemon.1) |
| `dbus-monitor` | `dbus-bin` | `/usr/bin/dbus-monitor` | [dbus-monitor(1)](man1/dbus-monitor.1) |
| `dbus-run-session` | `dbus-daemon` | `/usr/bin/dbus-run-session` | [dbus-run-session(1)](man1/dbus-run-session.1) |
| `dbus-send` | `dbus-bin` | `/usr/bin/dbus-send` | [dbus-send(1)](man1/dbus-send.1) |
| `dbus-update-activation-environment` | `dbus-bin` | `/usr/bin/dbus-update-activation-environment` | [dbus-update-activation-environment(1)](man1/dbus-update-activation-environment.1) |
| `dbus-uuidgen` | `dbus-bin` | `/usr/bin/dbus-uuidgen` | [dbus-uuidgen(1)](man1/dbus-uuidgen.1) |
| `dpcd_reg` | `intel-gpu-tools` | `/usr/bin/dpcd_reg` | [intel-gpu-tools(1)](man1/intel-gpu-tools.1) |
| `drmdevice` | `libdrm-tests` | `/usr/bin/drmdevice` | [drm-tools(1)](man1/drm-tools.1) |
| `editres` | `x11-utils` | `/usr/bin/editres` | [editres(1)](man1/editres.1) |
| `eglgears_wayland` | `mesa-utils` | `/usr/bin/eglgears_wayland` | [mesa-utils(1)](man1/mesa-utils.1) |
| `eglgears_x11` | `mesa-utils` | `/usr/bin/eglgears_x11` | [mesa-utils(1)](man1/mesa-utils.1) |
| `eglinfo` | `mesa-utils` | `/usr/bin/eglinfo` | [mesa-utils(1)](man1/mesa-utils.1) |
| `egltri_wayland` | `mesa-utils` | `/usr/bin/egltri_wayland` | [mesa-utils(1)](man1/mesa-utils.1) |
| `egltri_x11` | `mesa-utils` | `/usr/bin/egltri_x11` | [mesa-utils(1)](man1/mesa-utils.1) |
| `es2_info` | `mesa-utils` | `/usr/bin/es2_info` | [es2_info(1)](man1/es2_info.1) |
| `es2gears_wayland` | `mesa-utils` | `/usr/bin/es2gears_wayland` | [mesa-utils(1)](man1/mesa-utils.1) |
| `es2gears_x11` | `mesa-utils` | `/usr/bin/es2gears_x11` | [mesa-utils(1)](man1/mesa-utils.1) |
| `es2tri` | `mesa-utils` | `/usr/bin/es2tri` | [es2tri(1)](man1/es2tri.1) |
| `fc-cache` | `fontconfig` | `/usr/bin/fc-cache` | [fc-cache(1)](man1/fc-cache.1) |
| `fc-cat` | `fontconfig` | `/usr/bin/fc-cat` | [fc-cat(1)](man1/fc-cat.1) |
| `fc-conflist` | `fontconfig` | `/usr/bin/fc-conflist` | [fc-conflist(1)](man1/fc-conflist.1) |
| `fc-list` | `fontconfig` | `/usr/bin/fc-list` | [fc-list(1)](man1/fc-list.1) |
| `fc-match` | `fontconfig` | `/usr/bin/fc-match` | [fc-match(1)](man1/fc-match.1) |
| `fc-pattern` | `fontconfig` | `/usr/bin/fc-pattern` | [fc-pattern(1)](man1/fc-pattern.1) |
| `fc-query` | `fontconfig` | `/usr/bin/fc-query` | [fc-query(1)](man1/fc-query.1) |
| `fc-scan` | `fontconfig` | `/usr/bin/fc-scan` | [fc-scan(1)](man1/fc-scan.1) |
| `fc-validate` | `fontconfig` | `/usr/bin/fc-validate` | [fc-validate(1)](man1/fc-validate.1) |
| `firefox` | `firefox-esr` | `/usr/bin/firefox` | [firefox-esr(1)](man1/firefox-esr.1) |
| `firefox-esr` | `firefox-esr` | `/usr/bin/firefox-esr` | [firefox-esr(1)](man1/firefox-esr.1) |
| `glmark2` | `glmark2-x11` | `/usr/bin/glmark2` | [glmark2(1)](man1/glmark2.1) |
| `glmark2-drm` | `glmark2-drm` | `/usr/bin/glmark2-drm` | [glmark2-drm(1)](man1/glmark2-drm.1) |
| `glmark2-wayland` | `glmark2-wayland` | `/usr/bin/glmark2-wayland` | [glmark2-wayland(1)](man1/glmark2-wayland.1) |
| `glxdemo` | `mesa-utils` | `/usr/bin/glxdemo` | [glxdemo(1)](man1/glxdemo.1) |
| `glxgears` | `mesa-utils` | `/usr/bin/glxgears` | [glxgears(1)](man1/glxgears.1) |
| `glxheads` | `mesa-utils` | `/usr/bin/glxheads` | [glxheads(1)](man1/glxheads.1) |
| `glxinfo` | `mesa-utils` | `/usr/bin/glxinfo` | [glxinfo(1)](man1/glxinfo.1) |
| `gputop` | `intel-gpu-tools` | `/usr/bin/gputop` | [intel-gpu-tools(1)](man1/intel-gpu-tools.1) |
| `gtk-update-icon-cache` | `gtk-update-icon-cache` | `/usr/bin/gtk-update-icon-cache` | [gtk-update-icon-cache(1)](man1/gtk-update-icon-cache.1) |
| `gtk4-update-icon-cache` | `gtk-update-icon-cache` | `/usr/bin/gtk4-update-icon-cache` | [gtk4-update-icon-cache(1)](man1/gtk4-update-icon-cache.1) |
| `i915-perf-configs` | `intel-gpu-tools` | `/usr/bin/i915-perf-configs` | [intel-gpu-tools(1)](man1/intel-gpu-tools.1) |
| `i915-perf-control` | `intel-gpu-tools` | `/usr/bin/i915-perf-control` | [intel-gpu-tools(1)](man1/intel-gpu-tools.1) |
| `i915-perf-reader` | `intel-gpu-tools` | `/usr/bin/i915-perf-reader` | [intel-gpu-tools(1)](man1/intel-gpu-tools.1) |
| `i915-perf-recorder` | `intel-gpu-tools` | `/usr/bin/i915-perf-recorder` | [intel-gpu-tools(1)](man1/intel-gpu-tools.1) |
| `igt_facts` | `intel-gpu-tools` | `/usr/bin/igt_facts` | [intel-gpu-tools(1)](man1/intel-gpu-tools.1) |
| `igt_power` | `intel-gpu-tools` | `/usr/bin/igt_power` | [intel-gpu-tools(1)](man1/intel-gpu-tools.1) |
| `igt_stats` | `intel-gpu-tools` | `/usr/bin/igt_stats` | [intel-gpu-tools(1)](man1/intel-gpu-tools.1) |
| `intel-gen4asm` | `intel-gpu-tools` | `/usr/bin/intel-gen4asm` | [intel-gpu-tools(1)](man1/intel-gpu-tools.1) |
| `intel-gen4disasm` | `intel-gpu-tools` | `/usr/bin/intel-gen4disasm` | [intel-gpu-tools(1)](man1/intel-gpu-tools.1) |
| `intel-gfx-fw-info` | `intel-gpu-tools` | `/usr/bin/intel-gfx-fw-info` | [intel-gpu-tools(1)](man1/intel-gpu-tools.1) |
| `intel-gpu-overlay` | `intel-gpu-tools` | `/usr/bin/intel-gpu-overlay` | [intel-gpu-tools(1)](man1/intel-gpu-tools.1) |
| `intel_audio_dump` | `intel-gpu-tools` | `/usr/bin/intel_audio_dump` | [intel_audio_dump(1)](man1/intel_audio_dump.1) |
| `intel_backlight` | `intel-gpu-tools` | `/usr/bin/intel_backlight` | [intel-gpu-tools(1)](man1/intel-gpu-tools.1) |
| `intel_bios_dumper` | `intel-gpu-tools` | `/usr/bin/intel_bios_dumper` | [intel_bios_dumper(1)](man1/intel_bios_dumper.1) |
| `intel_display_bandwidth` | `intel-gpu-tools` | `/usr/bin/intel_display_bandwidth` | [intel-gpu-tools(1)](man1/intel-gpu-tools.1) |
| `intel_display_crc` | `intel-gpu-tools` | `/usr/bin/intel_display_crc` | [intel-gpu-tools(1)](man1/intel-gpu-tools.1) |
| `intel_display_poller` | `intel-gpu-tools` | `/usr/bin/intel_display_poller` | [intel-gpu-tools(1)](man1/intel-gpu-tools.1) |
| `intel_dp_compliance` | `intel-gpu-tools` | `/usr/bin/intel_dp_compliance` | [intel-gpu-tools(1)](man1/intel-gpu-tools.1) |
| `intel_dump_decode` | `intel-gpu-tools` | `/usr/bin/intel_dump_decode` | [intel-gpu-tools(1)](man1/intel-gpu-tools.1) |
| `intel_error_decode` | `intel-gpu-tools` | `/usr/bin/intel_error_decode` | [intel_error_decode(1)](man1/intel_error_decode.1) |
| `intel_firmware_decode` | `intel-gpu-tools` | `/usr/bin/intel_firmware_decode` | [intel-gpu-tools(1)](man1/intel-gpu-tools.1) |
| `intel_forcewaked` | `intel-gpu-tools` | `/usr/bin/intel_forcewaked` | [intel-gpu-tools(1)](man1/intel-gpu-tools.1) |
| `intel_framebuffer_dump` | `intel-gpu-tools` | `/usr/bin/intel_framebuffer_dump` | [intel-gpu-tools(1)](man1/intel-gpu-tools.1) |
| `intel_gem_info` | `intel-gpu-tools` | `/usr/bin/intel_gem_info` | [intel-gpu-tools(1)](man1/intel-gpu-tools.1) |
| `intel_gpu_abrt` | `intel-gpu-tools` | `/usr/bin/intel_gpu_abrt` | [intel-gpu-tools(1)](man1/intel-gpu-tools.1) |
| `intel_gpu_frequency` | `intel-gpu-tools` | `/usr/bin/intel_gpu_frequency` | [intel_gpu_frequency(1)](man1/intel_gpu_frequency.1) |
| `intel_gpu_time` | `intel-gpu-tools` | `/usr/bin/intel_gpu_time` | [intel-gpu-tools(1)](man1/intel-gpu-tools.1) |
| `intel_gpu_top` | `intel-gpu-tools` | `/usr/bin/intel_gpu_top` | [intel_gpu_top(1)](man1/intel_gpu_top.1) |
| `intel_gtt` | `intel-gpu-tools` | `/usr/bin/intel_gtt` | [intel_gtt(1)](man1/intel_gtt.1) |
| `intel_guc_logger` | `intel-gpu-tools` | `/usr/bin/intel_guc_logger` | [intel-gpu-tools(1)](man1/intel-gpu-tools.1) |
| `intel_gvtg_test` | `intel-gpu-tools` | `/usr/bin/intel_gvtg_test` | [intel-gpu-tools(1)](man1/intel-gpu-tools.1) |
| `intel_infoframes` | `intel-gpu-tools` | `/usr/bin/intel_infoframes` | [intel_infoframes(1)](man1/intel_infoframes.1) |
| `intel_l3_parity` | `intel-gpu-tools` | `/usr/bin/intel_l3_parity` | [intel-gpu-tools(1)](man1/intel-gpu-tools.1) |
| `intel_lid` | `intel-gpu-tools` | `/usr/bin/intel_lid` | [intel_lid(1)](man1/intel_lid.1) |
| `intel_opregion_decode` | `intel-gpu-tools` | `/usr/bin/intel_opregion_decode` | [intel-gpu-tools(1)](man1/intel-gpu-tools.1) |
| `intel_panel_fitter` | `intel-gpu-tools` | `/usr/bin/intel_panel_fitter` | [intel_panel_fitter(1)](man1/intel_panel_fitter.1) |
| `intel_perf_counters` | `intel-gpu-tools` | `/usr/bin/intel_perf_counters` | [intel-gpu-tools(1)](man1/intel-gpu-tools.1) |
| `intel_pm_rpm` | `intel-gpu-tools` | `/usr/bin/intel_pm_rpm` | [intel-gpu-tools(1)](man1/intel-gpu-tools.1) |
| `intel_reg` | `intel-gpu-tools` | `/usr/bin/intel_reg` | [intel_reg(1)](man1/intel_reg.1) |
| `intel_reg_checker` | `intel-gpu-tools` | `/usr/bin/intel_reg_checker` | [intel-gpu-tools(1)](man1/intel-gpu-tools.1) |
| `intel_residency` | `intel-gpu-tools` | `/usr/bin/intel_residency` | [intel-gpu-tools(1)](man1/intel-gpu-tools.1) |
| `intel_stepping` | `intel-gpu-tools` | `/usr/bin/intel_stepping` | [intel_stepping(1)](man1/intel_stepping.1) |
| `intel_tiling_detect` | `intel-gpu-tools` | `/usr/bin/intel_tiling_detect` | [intel-gpu-tools(1)](man1/intel-gpu-tools.1) |
| `intel_vbt_decode` | `intel-gpu-tools` | `/usr/bin/intel_vbt_decode` | [intel_vbt_decode(1)](man1/intel_vbt_decode.1) |
| `intel_watermark` | `intel-gpu-tools` | `/usr/bin/intel_watermark` | [intel-gpu-tools(1)](man1/intel-gpu-tools.1) |
| `libinput` | `libinput-tools` | `/usr/bin/libinput` | [libinput(1)](man1/libinput.1) |
| `listres` | `x11-utils` | `/usr/bin/listres` | [listres(1)](man1/listres.1) |
| `lsgpu` | `intel-gpu-tools` | `/usr/bin/lsgpu` | [intel-gpu-tools(1)](man1/intel-gpu-tools.1) |
| `mako-render` | `python3-mako` | `/usr/bin/mako-render` | [mako-render(1)](man1/mako-render.1) |
| `mesa-overlay-control.py` | `mesa-vulkan-drivers` | `/usr/bin/mesa-overlay-control.py` | [mesa-overlay-control.py(1)](man1/mesa-overlay-control.py.1) |
| `modeprint` | `libdrm-tests` | `/usr/bin/modeprint` | [drm-tools(1)](man1/drm-tools.1) |
| `modetest` | `libdrm-tests` | `/usr/bin/modetest` | [drm-tools(1)](man1/drm-tools.1) |
| `msm_dp_compliance` | `intel-gpu-tools` | `/usr/bin/msm_dp_compliance` | [intel-gpu-tools(1)](man1/intel-gpu-tools.1) |
| `pa-info` | `pulseaudio-utils` | `/usr/bin/pa-info` | [pa-info(1)](man1/pa-info.1) |
| `pacat` | `pulseaudio-utils` | `/usr/bin/pacat` | [pacat(1)](man1/pacat.1) |
| `pacmd` | `pulseaudio-utils` | `/usr/bin/pacmd` | [pacmd(1)](man1/pacmd.1) |
| `pactl` | `pulseaudio-utils` | `/usr/bin/pactl` | [pactl(1)](man1/pactl.1) |
| `padsp` | `pulseaudio-utils` | `/usr/bin/padsp` | [padsp(1)](man1/padsp.1) |
| `pamon` | `pulseaudio-utils` | `/usr/bin/pamon` | [pamon(1)](man1/pamon.1) |
| `paplay` | `pulseaudio-utils` | `/usr/bin/paplay` | [paplay(1)](man1/paplay.1) |
| `parec` | `pulseaudio-utils` | `/usr/bin/parec` | [parec(1)](man1/parec.1) |
| `parecord` | `pulseaudio-utils` | `/usr/bin/parecord` | [parecord(1)](man1/parecord.1) |
| `pasuspender` | `pulseaudio-utils` | `/usr/bin/pasuspender` | [pasuspender(1)](man1/pasuspender.1) |
| `pax11publish` | `pulseaudio-utils` | `/usr/bin/pax11publish` | [pax11publish(1)](man1/pax11publish.1) |
| `pdb3.13` | `python3.13` | `/usr/bin/pdb3.13` | [pdb3.13(1)](man1/pdb3.13.1) |
| `peglgears` | `mesa-utils` | `/usr/bin/peglgears` | [mesa-utils(1)](man1/mesa-utils.1) |
| `piglit` | `piglit` | `/usr/bin/piglit` | [piglit(1)](man1/piglit.1) |
| `proptest` | `libdrm-tests` | `/usr/bin/proptest` | [drm-tools(1)](man1/drm-tools.1) |
| `pulseaudio` | `pulseaudio` | `/usr/bin/pulseaudio` | [pulseaudio(1)](man1/pulseaudio.1) |
| `pydoc3.13` | `python3.13` | `/usr/bin/pydoc3.13` | [pydoc3.13(1)](man1/pydoc3.13.1) |
| `pygettext3.13` | `python3.13` | `/usr/bin/pygettext3.13` | [pygettext3.13(1)](man1/pygettext3.13.1) |
| `python3.13` | `python3.13-minimal` | `/usr/bin/python3.13` | [python3.13(1)](man1/python3.13.1) |
| `seatd` | `seatd` | `/usr/sbin/seatd` | [seatd(8)](man8/seatd.8) |
| `seatd-launch` | `seatd` | `/usr/bin/seatd-launch` | [seatd-launch(1)](man1/seatd-launch.1) |
| `setxkbmap` | `x11-xkb-utils` | `/usr/bin/setxkbmap` | [setxkbmap(1)](man1/setxkbmap.1) |
| `start-pulseaudio-x11` | `pulseaudio` | `/usr/bin/start-pulseaudio-x11` | [start-pulseaudio-x11(1)](man1/start-pulseaudio-x11.1) |
| `systemd-hwdb` | `udev` | `/usr/bin/systemd-hwdb` | [systemd-hwdb(8)](man8/systemd-hwdb.8) |
| `udevadm` | `udev` | `/usr/bin/udevadm` | [udevadm(8)](man8/udevadm.8) |
| `update-icon-caches` | `gtk-update-icon-cache` | `/usr/sbin/update-icon-caches` | [update-icon-caches(8)](man8/update-icon-caches.8) |
| `update-mime-database` | `shared-mime-info` | `/usr/bin/update-mime-database` | [update-mime-database(1)](man1/update-mime-database.1) |
| `vbltest` | `libdrm-tests` | `/usr/bin/vbltest` | [drm-tools(1)](man1/drm-tools.1) |
| `viewres` | `x11-utils` | `/usr/bin/viewres` | [viewres(1)](man1/viewres.1) |
| `vkcube` | `vulkan-tools` | `/usr/bin/vkcube` | [vkcube(1)](man1/vkcube.1) |
| `vkcubepp` | `vulkan-tools` | `/usr/bin/vkcubepp` | [vkcube(1)](man1/vkcube.1) |
| `vkgears` | `mesa-utils` | `/usr/bin/vkgears` | [mesa-utils(1)](man1/mesa-utils.1) |
| `vulkaninfo` | `vulkan-tools` | `/usr/bin/vulkaninfo` | [vulkaninfo(1)](man1/vulkaninfo.1) |
| `wcap-decode` | `weston` | `/usr/bin/wcap-decode` | [weston-clients(1)](man1/weston-clients.1) |
| `weston` | `weston` | `/usr/bin/weston` | [weston(1)](man1/weston.1) |
| `weston-calibrator` | `weston` | `/usr/bin/weston-calibrator` | [weston-clients(1)](man1/weston-clients.1) |
| `weston-clickdot` | `weston` | `/usr/bin/weston-clickdot` | [weston-clients(1)](man1/weston-clients.1) |
| `weston-cliptest` | `weston` | `/usr/bin/weston-cliptest` | [weston-clients(1)](man1/weston-clients.1) |
| `weston-constraints` | `weston` | `/usr/bin/weston-constraints` | [weston-clients(1)](man1/weston-clients.1) |
| `weston-content_protection` | `weston` | `/usr/bin/weston-content_protection` | [weston-clients(1)](man1/weston-clients.1) |
| `weston-debug` | `weston` | `/usr/bin/weston-debug` | [weston-debug(1)](man1/weston-debug.1) |
| `weston-dnd` | `weston` | `/usr/bin/weston-dnd` | [weston-clients(1)](man1/weston-clients.1) |
| `weston-editor` | `weston` | `/usr/bin/weston-editor` | [weston-clients(1)](man1/weston-clients.1) |
| `weston-eventdemo` | `weston` | `/usr/bin/weston-eventdemo` | [weston-clients(1)](man1/weston-clients.1) |
| `weston-flower` | `weston` | `/usr/bin/weston-flower` | [weston-clients(1)](man1/weston-clients.1) |
| `weston-fullscreen` | `weston` | `/usr/bin/weston-fullscreen` | [weston-clients(1)](man1/weston-clients.1) |
| `weston-image` | `weston` | `/usr/bin/weston-image` | [weston-clients(1)](man1/weston-clients.1) |
| `weston-multi-resource` | `weston` | `/usr/bin/weston-multi-resource` | [weston-clients(1)](man1/weston-clients.1) |
| `weston-presentation-shm` | `weston` | `/usr/bin/weston-presentation-shm` | [weston-clients(1)](man1/weston-clients.1) |
| `weston-resizor` | `weston` | `/usr/bin/weston-resizor` | [weston-clients(1)](man1/weston-clients.1) |
| `weston-scaler` | `weston` | `/usr/bin/weston-scaler` | [weston-clients(1)](man1/weston-clients.1) |
| `weston-screenshooter` | `weston` | `/usr/bin/weston-screenshooter` | [weston-clients(1)](man1/weston-clients.1) |
| `weston-simple-damage` | `weston` | `/usr/bin/weston-simple-damage` | [weston-clients(1)](man1/weston-clients.1) |
| `weston-simple-dmabuf-egl` | `weston` | `/usr/bin/weston-simple-dmabuf-egl` | [weston-clients(1)](man1/weston-clients.1) |
| `weston-simple-dmabuf-feedback` | `weston` | `/usr/bin/weston-simple-dmabuf-feedback` | [weston-clients(1)](man1/weston-clients.1) |
| `weston-simple-dmabuf-v4l` | `weston` | `/usr/bin/weston-simple-dmabuf-v4l` | [weston-clients(1)](man1/weston-clients.1) |
| `weston-simple-egl` | `weston` | `/usr/bin/weston-simple-egl` | [weston-clients(1)](man1/weston-clients.1) |
| `weston-simple-shm` | `weston` | `/usr/bin/weston-simple-shm` | [weston-clients(1)](man1/weston-clients.1) |
| `weston-simple-touch` | `weston` | `/usr/bin/weston-simple-touch` | [weston-clients(1)](man1/weston-clients.1) |
| `weston-smoke` | `weston` | `/usr/bin/weston-smoke` | [weston-clients(1)](man1/weston-clients.1) |
| `weston-stacking` | `weston` | `/usr/bin/weston-stacking` | [weston-clients(1)](man1/weston-clients.1) |
| `weston-subsurfaces` | `weston` | `/usr/bin/weston-subsurfaces` | [weston-clients(1)](man1/weston-clients.1) |
| `weston-tablet` | `weston` | `/usr/bin/weston-tablet` | [weston-clients(1)](man1/weston-clients.1) |
| `weston-terminal` | `weston` | `/usr/bin/weston-terminal` | [weston-clients(1)](man1/weston-clients.1) |
| `weston-touch-calibrator` | `weston` | `/usr/bin/weston-touch-calibrator` | [weston-clients(1)](man1/weston-clients.1) |
| `weston-transformed` | `weston` | `/usr/bin/weston-transformed` | [weston-clients(1)](man1/weston-clients.1) |
| `xdg-desktop-icon` | `xdg-utils` | `/usr/bin/xdg-desktop-icon` | [xdg-desktop-icon(1)](man1/xdg-desktop-icon.1) |
| `xdg-desktop-menu` | `xdg-utils` | `/usr/bin/xdg-desktop-menu` | [xdg-desktop-menu(1)](man1/xdg-desktop-menu.1) |
| `xdg-email` | `xdg-utils` | `/usr/bin/xdg-email` | [xdg-email(1)](man1/xdg-email.1) |
| `xdg-icon-resource` | `xdg-utils` | `/usr/bin/xdg-icon-resource` | [xdg-icon-resource(1)](man1/xdg-icon-resource.1) |
| `xdg-mime` | `xdg-utils` | `/usr/bin/xdg-mime` | [xdg-mime(1)](man1/xdg-mime.1) |
| `xdg-open` | `xdg-utils` | `/usr/bin/xdg-open` | [xdg-open(1)](man1/xdg-open.1) |
| `xdg-screensaver` | `xdg-utils` | `/usr/bin/xdg-screensaver` | [xdg-screensaver(1)](man1/xdg-screensaver.1) |
| `xdg-settings` | `xdg-utils` | `/usr/bin/xdg-settings` | [xdg-settings(1)](man1/xdg-settings.1) |
| `xdpyinfo` | `x11-utils` | `/usr/bin/xdpyinfo` | [xdpyinfo(1)](man1/xdpyinfo.1) |
| `xdriinfo` | `x11-utils` | `/usr/bin/xdriinfo` | [xdriinfo(1)](man1/xdriinfo.1) |
| `xe-perf-configs` | `intel-gpu-tools` | `/usr/bin/xe-perf-configs` | [intel-gpu-tools(1)](man1/intel-gpu-tools.1) |
| `xe-perf-control` | `intel-gpu-tools` | `/usr/bin/xe-perf-control` | [intel-gpu-tools(1)](man1/intel-gpu-tools.1) |
| `xe-perf-reader` | `intel-gpu-tools` | `/usr/bin/xe-perf-reader` | [intel-gpu-tools(1)](man1/intel-gpu-tools.1) |
| `xe-perf-recorder` | `intel-gpu-tools` | `/usr/bin/xe-perf-recorder` | [intel-gpu-tools(1)](man1/intel-gpu-tools.1) |
| `xeglgears` | `mesa-utils` | `/usr/bin/xeglgears` | [mesa-utils(1)](man1/mesa-utils.1) |
| `xeglthreads` | `mesa-utils` | `/usr/bin/xeglthreads` | [mesa-utils(1)](man1/mesa-utils.1) |
| `xev` | `x11-utils` | `/usr/bin/xev` | [xev(1)](man1/xev.1) |
| `xfd` | `x11-utils` | `/usr/bin/xfd` | [xfd(1)](man1/xfd.1) |
| `xfontsel` | `x11-utils` | `/usr/bin/xfontsel` | [xfontsel(1)](man1/xfontsel.1) |
| `xkbbell` | `x11-xkb-utils` | `/usr/bin/xkbbell` | [xkbbell(1)](man1/xkbbell.1) |
| `xkbcomp` | `x11-xkb-utils` | `/usr/bin/xkbcomp` | [xkbcomp(1)](man1/xkbcomp.1) |
| `xkbevd` | `x11-xkb-utils` | `/usr/bin/xkbevd` | [xkbevd(1)](man1/xkbevd.1) |
| `xkbprint` | `x11-xkb-utils` | `/usr/bin/xkbprint` | [xkbprint(1)](man1/xkbprint.1) |
| `xkbvleds` | `x11-xkb-utils` | `/usr/bin/xkbvleds` | [xkbvleds(1)](man1/xkbvleds.1) |
| `xkbwatch` | `x11-xkb-utils` | `/usr/bin/xkbwatch` | [xkbwatch(1)](man1/xkbwatch.1) |
| `xkill` | `x11-utils` | `/usr/bin/xkill` | [xkill(1)](man1/xkill.1) |
| `xlsatoms` | `x11-utils` | `/usr/bin/xlsatoms` | [xlsatoms(1)](man1/xlsatoms.1) |
| `xlsclients` | `x11-utils` | `/usr/bin/xlsclients` | [xlsclients(1)](man1/xlsclients.1) |
| `xlsfonts` | `x11-utils` | `/usr/bin/xlsfonts` | [xlsfonts(1)](man1/xlsfonts.1) |
| `xmessage` | `x11-utils` | `/usr/bin/xmessage` | [xmessage(1)](man1/xmessage.1) |
| `xprop` | `x11-utils` | `/usr/bin/xprop` | [xprop(1)](man1/xprop.1) |
| `xvinfo` | `x11-utils` | `/usr/bin/xvinfo` | [xvinfo(1)](man1/xvinfo.1) |
| `xwininfo` | `x11-utils` | `/usr/bin/xwininfo` | [xwininfo(1)](man1/xwininfo.1) |

GE-Proton is installed under `/opt/proton` on x86-64 ([proton(1)](man1/proton.1)); Khronos's WebGL conformance sources are under `/usr/share/webgl`. Their exact pins are in [sources-extra.txt](../vendor/gnu/sources-extra.txt).

### Compilers, debugger and browser

| Program | Version | Purpose |
| --- | --- | --- |
| [binutils](../vendor/gnu/native/Readme.md) | 2.47 | the assembler, the linker and the object file tools, run on Rach2 |
| [GCC](../vendor/gnu/native/Readme.md) | 16.2.0 | the C and C++ compiler, run on Rach2 |
| [GDB](../vendor/gnu/native/Readme.md) | 18.1 | the debugger, with gdbserver |
| [SpiderMonkey](../vendor/gnu/spidermonkey/Readme.md) | 140.16.0esr | js, the shell of Firefox's JavaScript engine, with its JIT |
| [chromium-headless-shell](../vendor/gnu/chromium/Readme.md) | Debian trixie at 20260927T082144Z | Debian's Chromium without a display, with D-Bus's system bus |

### The ports

Each port is a recipe of [`vendor/gnu/ports/`](../vendor/gnu/ports), with the version it builds and what it is: the first sentence of its comment, up to a semicolon.

| Port | Version | Purpose |
| --- | --- | --- |
| [autoconf](../vendor/gnu/ports/autoconf.sh) | 2.73 | GNU Autoconf, which writes a package's configure script. |
| [automake](../vendor/gnu/ports/automake.sh) | 1.19 | GNU Automake, which writes a package's Makefile.in. |
| [bash](../vendor/gnu/ports/bash.sh) | 5.3 | GNU bash with the official patches. |
| [bind9](../vendor/gnu/ports/bind9.sh) | 9.20.4 | BIND 9's tools for asking a name server: dig, host, nslookup, delv and nsupdate, with the libraries they share, on libuv, Userspace RCU, OpenSSL and libcap. |
| [bison](../vendor/gnu/ports/bison.sh) | 3.8.2 | GNU Bison, the parser generator, which runs m4 at /usr/bin/m4. |
| [brotli](../vendor/gnu/ports/brotli.sh) | 1.2.0 | brotli: the libraries libbrotlicommon, libbrotlidec and libbrotlienc, which Chawan links to read compressed pages, and the brotli program, with the headers and pkg-config files. |
| [bubblewrap](../vendor/gnu/ports/bubblewrap.sh) | 0.8.0 | bubblewrap, whose bwrap runs a program in new namespaces as a sandbox, as Claude Code's sandboxed shell runs its commands. |
| [bzip2](../vendor/gnu/ports/bzip2.sh) | 1.0.8 | bzip2 and libbz2, which Python's bz2 module, unzip and tar -j use. |
| [chawan](../vendor/gnu/ports/chawan.sh) | 0.5~dev1 | Chawan, cha, a web browser for the terminal with an HTML and CSS engine of its own, with flow, table and flex layout, and JavaScript in QuickJS, which its configuration turns on. |
| [cmake](../vendor/gnu/ports/cmake.sh) | 4.3.4 | CMake, the build system generator, with ccmake on the root's ncurses and the libraries its release carries, curl among them over the root's OpenSSL. |
| [cmus](../vendor/gnu/ports/cmus.sh) | 2.12.0 | cmus, the music player for the terminal, and cmus-remote. |
| [coreutils](../vendor/gnu/ports/coreutils.sh) | 9.12 | GNU coreutils. |
| [cronie](../vendor/gnu/ports/cronie.sh) | 1.7.2 | cronie, the cron daemon Fedora and Debian have, with crontab(1). |
| [curl](../vendor/gnu/ports/curl.sh) | 8.22.0 | curl and libcurl, with OpenSSL for TLS and zlib for compressed transfers. |
| [diffutils](../vendor/gnu/ports/diffutils.sh) | 3.12 | GNU diffutils: diff, cmp, diff3 and sdiff. |
| [dosfstools](../vendor/gnu/ports/dosfstools.sh) | 4.2 | dosfstools, the FAT tools Debian has: mkfs.fat, fsck.fat and fatlabel, with the names mkfs.vfat, mkdosfs, fsck.vfat, dosfsck and dosfslabel that Debian links to them. |
| [ffmpeg](../vendor/gnu/ports/ffmpeg.sh) | 9.0.2 | FFmpeg and ffprobe, with OSS input and output and the built-in audio decoders for WAV, MP3, FLAC, Ogg Vorbis, Opus and AAC. |
| [file](../vendor/gnu/ports/file.sh) | 5.48 | file, which names what a file holds, and libmagic, through which it reads the compiled magic database. |
| [findutils](../vendor/gnu/ports/findutils.sh) | 4.11.0 | GNU findutils: find, xargs, locate and updatedb. |
| [flex](../vendor/gnu/ports/flex.sh) | 2.6.4 | flex, the scanner generator, which runs m4 at /usr/bin/m4. |
| [gawk](../vendor/gnu/ports/gawk.sh) | 5.4.1 | GNU awk, with the extensions it loads by dlopen. |
| [git](../vendor/gnu/ports/git.sh) | 2.55.0 | Git, with curl for the HTTP transports, OpenSSL, zlib, Perl, and PCRE2 for git grep -P, and without Tcl/Tk, gettext, Python or expat: there is no gitk, git gui, translation, or push over WebDAV. |
| [gnupg](../vendor/gnu/ports/gnupg.sh) | 2.5.24 | GnuPG: gpg, gpg-agent, gpgsm, dirmngr with NTBTLS for hkps, keyboxd with SQLite, scdaemon and the tools. |
| [grep](../vendor/gnu/ports/grep.sh) | 3.12 | GNU grep, with -P through PCRE2. |
| [gzip](../vendor/gnu/ports/gzip.sh) | 1.15 | GNU gzip, with gunzip, zcat and its other scripts. |
| [htop](../vendor/gnu/ports/htop.sh) | 3.5.3 | htop, the interactive process viewer, with ncurses' wide-character library. |
| [iproute2](../vendor/gnu/ports/iproute2.sh) | 7.2.0 | iproute2: ip, which shows the machine's interfaces, addresses and routes through the kernel's netlink route socket, and ss, bridge and tc. |
| [iw](../vendor/gnu/ports/iw.sh) | 6.17 | iw: shows and sets a wireless card's state through nl80211, with libnl's generic netlink. |
| [jq](../vendor/gnu/ports/jq.sh) | 1.8.2 | jq, the JSON processor, with the Oniguruma its release carries for its regular expressions, and without its documentation's build, whose page the release has made. |
| [less](../vendor/gnu/ports/less.sh) | 710 | less, the pager. |
| [libassuan](../vendor/gnu/ports/libassuan.sh) | 3.0.2 | Libassuan, the protocol GnuPG's programs talk to each other and to pinentry in. |
| [libcap](../vendor/gnu/ports/libcap.sh) | 2.75 | libcap, the library of POSIX capabilities, which BIND needs on Linux, and its getcap, setcap and capsh, built by its Makefiles, without PAM's module and Go's bindings. |
| [libevent](../vendor/gnu/ports/libevent.sh) | 2.1.13-stable | libevent, the event loop tmux is built on, without its OpenSSL layer and its sample programs. |
| [libffi](../vendor/gnu/ports/libffi.sh) | 3.8.0 | libffi, which calls a C function whose arguments are known only when the program runs: Python's ctypes is built on it. |
| [libgcrypt](../vendor/gnu/ports/libgcrypt.sh) | 1.12.4 | Libgcrypt, the cryptography of GnuPG. |
| [libgpg-error](../vendor/gnu/ports/libgpg-error.sh) | 1.61 | libgpg-error, the error codes and the runtime GnuPG and its libraries share, with gpgrt-config, which the others' configure scripts ask for their flags and which reads the root's files through PKG_CONFIG_SYSROOT_DIR. |
| [libksba](../vendor/gnu/ports/libksba.sh) | 1.8.1 | Libksba, the X.509 and CMS library of gpgsm, dirmngr and NTBTLS. |
| [libnl](../vendor/gnu/ports/libnl.sh) | 3.12.0 | libnl: the netlink libraries wpa_supplicant and iw use to speak nl80211, libnl-3, libnl-genl-3 and libnl-route-3 with the rest of the family, shared only. |
| [libssh2](../vendor/gnu/ports/libssh2.sh) | 1.11.1 | libssh2, the SSH library through which Chawan browses SFTP, with OpenSSL for its cryptography and zlib for compression, and without its examples. |
| [liburcu](../vendor/gnu/ports/liburcu.sh) | 0.15.7 | Userspace RCU, the library of read-copy-update that BIND's tables use. |
| [libuv](../vendor/gnu/ports/libuv.sh) | 1.52.1 | libuv, the event loop BIND is built on, built with CMake, since its release has no configure script, and without its tests. |
| [lsof](../vendor/gnu/ports/lsof.sh) | 4.99.4 | lsof, which lists the files processes have open from /proc. |
| [m4](../vendor/gnu/ports/m4.sh) | 1.4.21 | GNU m4, the macro processor Autoconf, Bison and flex run. |
| [make](../vendor/gnu/ports/make.sh) | 4.4.1 | GNU make. |
| [nano](../vendor/gnu/ports/nano.sh) | 9.2 | GNU nano, the editor, with libmagic to choose a file's syntax. |
| [ncurses](../vendor/gnu/ports/ncurses.sh) | 6.6 | ncurses with wide characters, its default, its shared and static libraries, and the terminfo database. |
| [npth](../vendor/gnu/ports/npth.sh) | 1.8 | nPth, the threads GnuPG's agent and daemons run, over glibc's POSIX threads. |
| [ntbtls](../vendor/gnu/ports/ntbtls.sh) | 0.3.2 | NTBTLS, the TLS library GnuPG's dirmngr uses to reach a key server over hkps, built on Libgcrypt and Libksba, in place of GnuTLS, which is not ported. |
| [openssh](../vendor/gnu/ports/openssh.sh) | 10.5p1 | OpenSSH: the server, the client, ssh-keygen, sftp-server and the rest, with OpenSSL for its cryptography and zlib for compression. |
| [openssl](../vendor/gnu/ports/openssl.sh) | 4.0.2 | OpenSSL for its linux-x86 or linux-x86_64 target, with its shared libraries and zlib, and the libraries in /usr/lib on both. |
| [patch](../vendor/gnu/ports/patch.sh) | 2.8 | GNU patch. |
| [pcre2](../vendor/gnu/ports/pcre2.sh) | 10.49 | PCRE2, the library of Perl's regular expressions, which GNU grep's -P and Git's grep -P match with, and its pcre2grep and pcre2test. |
| [perl](../vendor/gnu/ports/perl.sh) | 5.44.0 | Perl. |
| [pinentry](../vendor/gnu/ports/pinentry.sh) | 1.3.3 | pinentry, which asks for a passphrase on behalf of gpg-agent: on a terminal with ncurses, or as a plain prompt on one that cannot draw. |
| [pkgconf](../vendor/gnu/ports/pkgconf.sh) | 2.5.1 | pkgconf, which answers as pkg-config, as Debian's does. |
| [procps-ng](../vendor/gnu/ports/procps-ng.sh) | 4.0.7 | procps-ng: ps, top, w, uptime, free, vmstat, watch, pgrep, pkill, pidof, pmap, kill, sysctl and slabtop, which read the machine's processes and memory from /proc, as a Debian machine has them. |
| [python](../vendor/gnu/ports/python.sh) | 3.14.8 | Python 3, with the modules of its standard library that link the root's libraries: zlib, bz2 and lzma, ssl and hashlib with OpenSSL, ctypes with libffi, sqlite3, readline, curses, and uuid with util-linux's libuuid. |
| [readline](../vendor/gnu/ports/readline.sh) | 8.3 | GNU Readline, with its released patches, linked with ncurses for the terminal's capabilities, which Python's interactive prompt and the SQLite and GnuPG programs read lines with. |
| [rsync](../vendor/gnu/ports/rsync.sh) | 3.5.1 | rsync, with OpenSSL's digests and zstd's compression beside its own zlib. |
| [runtime](../vendor/gnu/ports/runtime.sh) | 2.44 | The runtime of the GNU system, which every other port depends on. |
| [screen](../vendor/gnu/ports/screen.sh) | 5.0.2 | GNU screen, the terminal multiplexer, without PAM, which the root does not have, and not set-id: a user's sessions are sockets in ~/.screen, as screen keeps them when it is not installed set-id root. |
| [sed](../vendor/gnu/ports/sed.sh) | 4.10 | GNU sed. |
| [shadow](../vendor/gnu/ports/shadow.sh) | 4.20.2 | shadow, the account tools Debian has from it that BusyBox has not: useradd, usermod and userdel, groupadd, groupmod and groupdel, gpasswd, chsh, chfn and chage, chgpasswd, newusers, vipw and vigr, pwck and grpck, faillog, and the conversions to and from the shadow files, with /etc/login.defs. |
| [socat](../vendor/gnu/ports/socat.sh) | 1.8.1.3 | socat, the relay between two byte streams of almost any kind, which Claude Code's sandbox runs to carry its network proxy into the sandbox, with OpenSSL for its TLS addresses, and its filan and procan. |
| [sqlite](../vendor/gnu/ports/sqlite.sh) | 3.53.4 | SQLite: the library, which Python's sqlite3 module and GnuPG's keyboxd link, and the sqlite3 shell with readline, from the amalgamation's release, with the full-text, R-tree, session and statistics extensions Debian's has. |
| [strace](../vendor/gnu/ports/strace.sh) | 7.2 | strace, the system call tracer. |
| [sudo](../vendor/gnu/ports/sudo.sh) | 1.9.17p2 | sudo, which runs a command as another user by the rules of /etc/sudoers, checked against the shadow file, since the root has no PAM, and without its log server and the mail it would send. |
| [tar](../vendor/gnu/ports/tar.sh) | 1.35 | GNU tar. |
| [texinfo](../vendor/gnu/ports/texinfo.sh) | 7.3 | GNU Texinfo: info, the reader of the manuals in /usr/share/info, install-info, and makeinfo with the rest of texi2any, which is Perl and runs with the root's perl. |
| [tmux](../vendor/gnu/ports/tmux.sh) | 3.7c | tmux, the terminal multiplexer. |
| [tzdata](../vendor/gnu/ports/tzdata.sh) | 2026e | The time zone database, which glibc reads under /usr/share/zoneinfo for the zone TZ or /etc/localtime names. |
| [unzip](../vendor/gnu/ports/unzip.sh) | 6.0 | Info-ZIP's unzip, with funzip, unzipsfx, zipgrep and zipinfo. |
| [util-linux](../vendor/gnu/ports/util-linux.sh) | 2.42.4 | util-linux: the system's tools a Debian machine has from it, of which BusyBox's applets have fewer options or none: last, lsblk, findmnt, flock, prlimit, ionice, column, script, newgrp, dmesg, fdisk, lscpu and the rest. |
| [vim](../vendor/gnu/ports/vim.sh) | 9.2.1132 | Vim for the terminal, with its huge feature set and ncurses for the terminal's capabilities. |
| [wpa_supplicant](../vendor/gnu/ports/wpa_supplicant.sh) | 2.12 | wpa_supplicant, wpa_cli and wpa_passphrase: the station's WPA, WPA2 and WPA3 client, with PEAP, TTLS and TLS for enterprise networks, its control socket and bgscan. |
| [xz](../vendor/gnu/ports/xz.sh) | 5.8.4 | XZ Utils: xz and liblzma. |
| [zip](../vendor/gnu/ports/zip.sh) | 3.0 | Info-ZIP's zip, zipcloak, zipnote and zipsplit. |
| [zlib](../vendor/gnu/ports/zlib.sh) | 1.3.2 | zlib, which OpenSSL, OpenSSH, curl and Git link against. |
| [zstd](../vendor/gnu/ports/zstd.sh) | 1.5.7 | Zstandard: zstd with its other names, unzstd, zstdcat and zstdmt, and libzstd, which rsync links. |

### The agents layer

The [agents layer](../vendor/gnu/agents/Readme.md) installs vendors' published builds in the x86-64 root when the build names them in `GNU_AGENTS`. Each package is a recipe of [`vendor/gnu/agents/packages/`](../vendor/gnu/agents/packages), with the version it pins, its vendor's license and what it is: the first sentence of its comment, up to a semicolon.

| Package | Version | License | Purpose |
| --- | --- | --- | --- |
| [agy](../vendor/gnu/agents/packages/agy.sh) | 1.4.0 | Google Antigravity Terms of Service | agy, Google's Antigravity CLI, a coding agent for the terminal. |
| [claude](../vendor/gnu/agents/packages/claude.sh) | 2.1.296 | Proprietary | claude, Anthropic's Claude Code, a coding agent for the terminal. |
| [codex](../vendor/gnu/agents/packages/codex.sh) | 0.162.1 | Apache-2.0 | codex, OpenAI's Codex, a coding agent for the terminal. |
| [fd](../vendor/gnu/agents/packages/fd.sh) | 10.5.0 | MIT OR Apache-2.0 | fd, a search for file names in a tree. |
| [gh](../vendor/gnu/agents/packages/gh.sh) | 2.102.0 | MIT | gh, GitHub's command-line program for repositories, issues, pull requests, releases and the API. |
| [git-lfs](../vendor/gnu/agents/packages/git-lfs.sh) | 3.8.1 | MIT | git-lfs, Git's large file storage extension. |
| [muse](../vendor/gnu/agents/packages/muse.sh) | 1.4.10-R6427.1 | Meta Model API Terms of Service | muse, Meta's Muse Code, a coding agent for the terminal. |
| [ninja](../vendor/gnu/agents/packages/ninja.sh) | 1.13.2 | Apache-2.0 | ninja, a build tool for generated dependency graphs. |
| [node](../vendor/gnu/agents/packages/node.sh) | 26.13.0 | MIT AND Artistic-2.0 AND bundled licenses | node, Node.js, the JavaScript runtime with npm and npx. |
| [opencode](../vendor/gnu/agents/packages/opencode.sh) | 2.2.5 | MIT | opencode, OpenCode, a coding agent for the terminal. |
| [ripgrep](../vendor/gnu/agents/packages/ripgrep.sh) | 15.2.0 | Unlicense OR MIT | rg, ripgrep, which searches a tree of files for lines that match a regular expression and skips what .gitignore names. |
| [shellcheck](../vendor/gnu/agents/packages/shellcheck.sh) | 0.11.0 | GPL-3.0-or-later | ShellCheck, a static analyzer for shell scripts. |

## The vendored projects

The third-party code of the repository, each with the version the build pins, its upstream license and the document that describes it. Rach2's own code is under the GNU General Public License, version 2 only ([COPYING](../COPYING)).

| Project | Version | License | Readme |
| --- | --- | --- | --- |
| musl | 1.2.5 | MIT | [vendor/musl/Readme.md](../vendor/musl/Readme.md) |
| BusyBox | 1.36.1 | GPL 2 only | [vendor/busybox/Readme.md](../vendor/busybox/Readme.md) |
| Dropbear | 2022.83 | MIT-style | [vendor/dropbear/Readme.md](../vendor/dropbear/Readme.md) |
| sox_ng | 14.8.1 | GPL 2 only | [vendor/sox_ng/Readme.md](../vendor/sox_ng/Readme.md) |
| libFLAC | 1.5.0 | BSD 3-clause | [vendor/sox_ng/Readme.md](../vendor/sox_ng/Readme.md) |
| hostap | 2.12 | BSD 3-clause | [vendor/hostap/Readme.md](../vendor/hostap/Readme.md) |
| iw | 6.17 | ISC | [vendor/iw/Readme.md](../vendor/iw/Readme.md) |
| OpenSSL | 4.0.2 | Apache 2.0 | [vendor/openssl/Readme.md](../vendor/openssl/Readme.md) |
| libnl | 3.12.0 | LGPL 2.1 | [vendor/libnl/Readme.md](../vendor/libnl/Readme.md) |
| kbd | 2.10.0 | GPL-2.0-or-later | [vendor/kbd/Readme.md](../vendor/kbd/Readme.md) |
| TinyCC | 0.9.27 | LGPL 2.1 | [vendor/tcc/Readme.md](../vendor/tcc/Readme.md) |
| pdpmake | 2.0.4 | public domain | [vendor/pdpmake/Readme.md](../vendor/pdpmake/Readme.md) |
| e2fsprogs | 1.47.4 | GPL 2; its libraries LGPL 2, BSD-style and MIT-style | [vendor/e2fsprogs/Readme.md](../vendor/e2fsprogs/Readme.md) |
| GRUB | 2.12-1ubuntu7.3 | GPL 3 or later | [vendor/grub/Readme.md](../vendor/grub/Readme.md) |
| libtsm | 4.8.0 | MIT, with a key encoder from foot under its MIT notice | [vendor/libtsm/Readme.md](../vendor/libtsm/Readme.md) |
| wcwidth | libtsm 4.8.0's copy | MIT, with parts under Markus Kuhn's permissive notice | [vendor/libtsm/Readme.md](../vendor/libtsm/Readme.md) |
| xkeyboard-config | Ubuntu 2.41-2ubuntu1.1 | MIT | [vendor/keymaps/Readme.md](../vendor/keymaps/Readme.md) |
| doomgeneric | commit fc601639494e | GPL 2 or later, with Nuked OPL3 under LGPL 2.1 or later | [ports/doom/Readme.md](../ports/doom/Readme.md) |
| GNU ports | [each its own](#the-ports) | each port its own, which its tarball has | [vendor/gnu/Readme.md](../vendor/gnu/Readme.md) |
| binutils, GCC, GDB | 2.47, 16.2.0, 18.1 | GPL 3 or later | [vendor/gnu/native/Readme.md](../vendor/gnu/native/Readme.md) |
| SpiderMonkey | 140.16.0esr | MPL 2.0 | [vendor/gnu/spidermonkey/Readme.md](../vendor/gnu/spidermonkey/Readme.md) |
| chromium-headless-shell | Debian trixie at 20260927T082144Z | BSD 3-clause | [vendor/gnu/chromium/Readme.md](../vendor/gnu/chromium/Readme.md) |
| termbox2 | 2.7.0-dev | MIT | [krffs/Readme.md](../krffs/Readme.md) |
| Unifont | 18.0.01 | SIL OFL 1.1, or GPL 2 or later with the font embedding exception | [vendor/unifont/Readme.md](../vendor/unifont/Readme.md) |
| Multiboot header | none given | MIT-style | [rach2/vendor/multiboot.h](../rach2/vendor/multiboot.h) |
