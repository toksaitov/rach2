# The programs of Rach2

Rach2 has two systems. The musl system is the static programs this repository builds against the ported musl libc: the in-house tools, BusyBox, Dropbear, TinyCC, pdpmake, e2fsprogs and Doom. `make` builds it on every supported host, and every image stages it or a part of it. The GNU system is a dynamically linked root of glibc and the programs ported to it, installed over the musl system in an ext2 image; it builds on an x86-64 Linux host only.

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
| `init` | [init(8)](man8/init.8) | in-house (`init-login`) | yes | in-house (`init-recovery`) |
| `login` | [login(1)](man1/login.1) | BusyBox (`busybox-suid`) | in-house |  |
| `loopctl` | [loopctl(8)](man8/loopctl.8) | in-house | yes |  |
| `make` | [make(1)](man1/make.1) | pdpmake |  |  |
| `man` | [man(1)](man1/man.1) | in-house (`man.script`) |  | yes |
| `mke2fs` | [mke2fs(8)](man8/mke2fs.8) | e2fsprogs |  |  |
| `passwd` | [passwd(1)](man1/passwd.1) | BusyBox (`busybox-suid`) | in-house | yes |
| `ping` | [ping(1)](man1/ping.1) | in-house | yes | yes |
| `sh` | [sh(1)](man1/sh.1) | BusyBox (`busybox`) | in-house (`ish`) | yes |
| `su` | [su(1)](man1/su.1) | BusyBox (`busybox-suid`) | in-house | yes |
| `tcc` | [tcc(1)](man1/tcc.1) | TinyCC |  |  |
| `update` | [update(8)](man8/update.8) | in-house | yes | yes |

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
| `netup` | [netup(8)](man8/netup.8) | in-house |  |
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

The [GNU layer](../vendor/gnu/Readme.md) builds a cross toolchain with glibc, the ports below, binutils, GCC and GDB to run on Rach2, the SpiderMonkey shell, and Debian's chromium-headless-shell, and installs them over the musl system in the os-real-gnu root, an ext2 image. The path finds a port's program before a program of the musl system of the same name, and the ports' pages are on the machine but not in this repository; [the layer's Readme](../vendor/gnu/Readme.md#on-the-machine) has the details.

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
| `newgrp` | `/usr/bin/newgrp` | on the machine | [util-linux](../vendor/gnu/ports/util-linux.sh) |
| `resize2fs` | `/sbin/resize2fs` | [resize2fs(8)](man8/resize2fs.8) | e2fsprogs |
| `sudo` | `/usr/bin/sudo` | on the machine | [sudo](../vendor/gnu/ports/sudo.sh) |
| `tune2fs` | `/sbin/tune2fs` | [tune2fs(8)](man8/tune2fs.8) | e2fsprogs |
| `wall` | `/usr/bin/wall` | on the machine | [util-linux](../vendor/gnu/ports/util-linux.sh) |
| `write` | `/usr/bin/write` | on the machine | [util-linux](../vendor/gnu/ports/util-linux.sh) |

It does not stage `/bin/dropbearmulti` and `/bin/sh`, which os-real does; and `/sbin/init` is `init-metal`, where os-real has `init-login`.

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
| [bzip2](../vendor/gnu/ports/bzip2.sh) | 1.0.8 | bzip2 and libbz2, which Python's bz2 module, unzip and tar -j use. |
| [chawan](../vendor/gnu/ports/chawan.sh) | 0.5~dev1 | Chawan, cha, a web browser for the terminal with an HTML and CSS engine of its own, with flow, table and flex layout, and JavaScript in QuickJS, which its configuration turns on. |
| [cmake](../vendor/gnu/ports/cmake.sh) | 4.3.4 | CMake, the build system generator, with ccmake on the root's ncurses and the libraries its release carries, curl among them over the root's OpenSSL. |
| [coreutils](../vendor/gnu/ports/coreutils.sh) | 9.12 | GNU coreutils. |
| [cronie](../vendor/gnu/ports/cronie.sh) | 1.7.2 | cronie, the cron daemon Fedora and Debian have, with crontab(1). |
| [curl](../vendor/gnu/ports/curl.sh) | 8.22.0 | curl and libcurl, with OpenSSL for TLS and zlib for compressed transfers. |
| [diffutils](../vendor/gnu/ports/diffutils.sh) | 3.12 | GNU diffutils: diff, cmp, diff3 and sdiff. |
| [dosfstools](../vendor/gnu/ports/dosfstools.sh) | 4.2 | dosfstools, the FAT tools Debian has: mkfs.fat, fsck.fat and fatlabel, with the names mkfs.vfat, mkdosfs, fsck.vfat, dosfsck and dosfslabel that Debian links to them. |
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
| [jq](../vendor/gnu/ports/jq.sh) | 1.8.2 | jq, the JSON processor, with the Oniguruma its release carries for its regular expressions, and without its documentation's build, whose page the release has made. |
| [less](../vendor/gnu/ports/less.sh) | 710 | less, the pager. |
| [libassuan](../vendor/gnu/ports/libassuan.sh) | 3.0.2 | Libassuan, the protocol GnuPG's programs talk to each other and to pinentry in. |
| [libcap](../vendor/gnu/ports/libcap.sh) | 2.75 | libcap, the library of POSIX capabilities, which BIND needs on Linux, and its getcap, setcap and capsh, built by its Makefiles, without PAM's module and Go's bindings. |
| [libevent](../vendor/gnu/ports/libevent.sh) | 2.1.13-stable | libevent, the event loop tmux is built on, without its OpenSSL layer and its sample programs. |
| [libffi](../vendor/gnu/ports/libffi.sh) | 3.8.0 | libffi, which calls a C function whose arguments are known only when the program runs: Python's ctypes is built on it. |
| [libgcrypt](../vendor/gnu/ports/libgcrypt.sh) | 1.12.4 | Libgcrypt, the cryptography of GnuPG. |
| [libgpg-error](../vendor/gnu/ports/libgpg-error.sh) | 1.61 | libgpg-error, the error codes and the runtime GnuPG and its libraries share, with gpgrt-config, which the others' configure scripts ask for their flags and which reads the root's files through PKG_CONFIG_SYSROOT_DIR. |
| [libksba](../vendor/gnu/ports/libksba.sh) | 1.8.1 | Libksba, the X.509 and CMS library of gpgsm, dirmngr and NTBTLS. |
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
| [xz](../vendor/gnu/ports/xz.sh) | 5.8.4 | XZ Utils: xz and liblzma. |
| [zip](../vendor/gnu/ports/zip.sh) | 3.0 | Info-ZIP's zip, zipcloak, zipnote and zipsplit. |
| [zlib](../vendor/gnu/ports/zlib.sh) | 1.3.2 | zlib, which OpenSSL, OpenSSH, curl and Git link against. |
| [zstd](../vendor/gnu/ports/zstd.sh) | 1.5.7 | Zstandard: zstd with its other names, unzstd, zstdcat and zstdmt, and libzstd, which rsync links. |

## The vendored projects

The third-party code of the repository, each with the version the build pins, its upstream license and the document that describes it. The repository declares no license for its own code.

| Project | Version | License | Readme |
| --- | --- | --- | --- |
| musl | 1.2.5 | MIT | [vendor/musl/Readme.md](../vendor/musl/Readme.md) |
| BusyBox | 1.36.1 | GPL 2 only | [vendor/busybox/Readme.md](../vendor/busybox/Readme.md) |
| Dropbear | 2022.83 | MIT-style | [vendor/dropbear/Readme.md](../vendor/dropbear/Readme.md) |
| TinyCC | 0.9.27 | LGPL 2.1 | [vendor/tcc/Readme.md](../vendor/tcc/Readme.md) |
| pdpmake | 2.0.4 | public domain | [vendor/pdpmake/Readme.md](../vendor/pdpmake/Readme.md) |
| e2fsprogs | 1.47.4 | GPL 2; its libraries LGPL 2, BSD-style and MIT-style | [vendor/e2fsprogs/Readme.md](../vendor/e2fsprogs/Readme.md) |
| GRUB | 2.12-1ubuntu7.3 | GPL 3 or later | [vendor/grub/Readme.md](../vendor/grub/Readme.md) |
| doomgeneric | commit fc601639494e | GPL 2 or later, with Nuked OPL3 under LGPL 2.1 or later | [ports/doom/Readme.md](../ports/doom/Readme.md) |
| GNU ports | [each its own](#the-ports) | each port its own, which its tarball has | [vendor/gnu/Readme.md](../vendor/gnu/Readme.md) |
| binutils, GCC, GDB | 2.47, 16.2.0, 18.1 | GPL 3 or later | [vendor/gnu/native/Readme.md](../vendor/gnu/native/Readme.md) |
| SpiderMonkey | 140.16.0esr | MPL 2.0 | [vendor/gnu/spidermonkey/Readme.md](../vendor/gnu/spidermonkey/Readme.md) |
| chromium-headless-shell | Debian trixie at 20260927T082144Z | BSD 3-clause | [vendor/gnu/chromium/Readme.md](../vendor/gnu/chromium/Readme.md) |
| termbox2 | 2.7.0-dev | MIT | [krffs/Readme.md](../krffs/Readme.md) |
| Multiboot header | none given | MIT-style | [rach2/vendor/multiboot.h](../rach2/vendor/multiboot.h) |
