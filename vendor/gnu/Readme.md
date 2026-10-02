# The GNU layer

This directory builds the GNU system of milestone 54 (`dev-log/54-gnu/plan.md`): an i686-linux-gnu cross toolchain, and with it the programs of a root that the GNU system is installed from. The layer builds on a Linux host only and is not part of `make` or `make all`; `make gnu-toolchain` and `make gnu-ports` build it (`mk/gnu.mk`). Three directories have programs with build scripts of their own (`mk/gnu-extra.mk`), each with its Readme: `native/`, GCC, binutils and GDB that run on Rach2; `spidermonkey/`, the SpiderMonkey shell; and `chromium/`, Debian's chromium-headless-shell with its libraries. `make os-real-gnu-ext2` installs all of it over the os-real system in an ext2 root that the kernel boots (`mk/gnu-image.mk`, and [the root image](#the-root-image) below).

## Sources

`sources.txt` lists every tarball of the layer with its version, URL and SHA-256, and its header says how each digest was verified: with gpgv, against an upstream signature that covers the release. For most projects that is the release's own signature; for Perl it is the signed CPAN checksum file of the uploader's directory, and for Vim, whose tag archive is not signed, the signed git tag. The tarballs are not committed: GCC's is larger than GitHub accepts for one file, and together they are about twice the rest of the repository. `fetch.sh` downloads a tarball into `downloads/`, which Git ignores, once, and checks its digest every time it is used; a file with another digest is removed and refused. `fetch.sh` with no arguments fetches everything, and `fetch.sh --path NAME` prints one tarball's path. `sources-extra.txt`, in the same format, lists what the three directories above build from, among them Firefox's source, Rust and Debian's archive keys, and the CA bundle the root image installs, with how its digest was checked; `fetch.sh` reads both lists. GDB 18.1 is pinned in `sources.txt`, and `native/build_gdb.sh` builds it.

## The toolchain

`make gnu-toolchain` runs `build_toolchain.sh`, which builds into `build/vendor/gnu/`:

* `cross/`: binutils 2.47 and GCC 16.2.0 for C and C++ (with GMP 6.3.0, MPFR 4.2.2 and MPC 1.3.1 built in GCC's tree), as `i686-linux-gnu-gcc` and the rest, configured for the i686 instruction set.
* `sysroot/`: the target's system: the Linux 6.18.54 headers, glibc 2.44 with the dynamic loader in `lib/`, and libxcrypt 4.5.2, which provides the `crypt` glibc 2.44 no longer has.

The order is binutils, the headers, a first GCC for C without a C library, glibc, the final GCC, and libxcrypt. glibc keeps its default minimum kernel, 3.2.0, so it falls back from the newer system calls to the older ones Rach2 has. The script takes step names (`binutils`, `headers`, `gcc1`, `glibc`, `gcc2`, `libxcrypt`) to run only those over an earlier build; with none, it builds the toolchain again from nothing and leaves the ports' and Chromium's build directories alone. Under make its sub-makes share the caller's jobs, so `make -j32 gnu-toolchain` built it in ten minutes on the testing host.

The host needs a C and C++ compiler, GNU make 4, gawk, bison, flex, m4, Perl, Python 3, texinfo, xz, patch, and curl or wget. A pkg-config, where the host has one, is used by the ports' configure scripts and reads only the root's files; none of the ports needs it.

## The runtime

The first port in the root, `build/vendor/gnu/root`, is the runtime, `ports/runtime.sh`, which has no tarball. It installs the toolchain's sysroot: glibc 2.44 with its dynamic loader as `/lib/ld-linux.so.2` and its shared libraries in `/lib`, as an i386 GNU system without multiarch directories has them; glibc's headers, start files and static libraries and the Linux headers in `/usr`; and libxcrypt's `libcrypt.so.1` in `/usr/lib`. To these it adds GCC's shared libraries, `libgcc_s.so.1` in `/lib` and `libstdc++.so.6` in `/usr/lib`, and a locale archive, `/usr/lib/locale/locale-archive`, with `C.UTF-8` and `en_US.UTF-8`, which the target's own `localedef` writes, run on the build host through the staged loader. libxcrypt's soname is `libcrypt.so.1` because the toolchain builds it with the glibc-compatible interface (`--enable-obsolete-api=glibc`); Debian's i386 packages ask for the same name. With the headers and libraries in it, the root is a sysroot as well as a system.

## The ports

`build_port.sh NAME...` builds ports with the toolchain; `build_port.sh --all`, which `make gnu-ports` runs, builds every recipe into an empty root. Each port has a recipe in `ports/`: a shell fragment that sets its version, the ports it depends on, its configure arguments, the configure answers a cross build cannot find out for itself, and the pinned patches applied after the extract. A port that autoconf does not configure has its own build and install steps, and its recipe's comment says how and why. A port autoconf configures has the prefix `/usr`, and `/etc` and `/var` for its configuration and its state, as Debian's packages have them, so that a system-wide file such as `/etc/nanorc`, `/etc/tmux.conf` or `/etc/gnupg/gpg.conf` is the one the program reads; a recipe's own `--sysconfdir` or `--localstatedir` overrides them.

* The compilers run through wrappers in `build/vendor/gnu/wrap/` that give them the root as their sysroot, so a port finds the headers and libraries of the ports before it where the target has them.
* A port's dependencies are built first when the root does not have them, and every port depends on the runtime.
* A port installs into `build/vendor/gnu/stage/NAME`. There the target's `strip` removes the debugging information from its ELF files, which keep their symbol tables, and its libtool archives and Info index are removed; the rest is copied into the root and listed in `stage/NAME.files`.
* Programs link with the shared glibc and ask for `/lib/ld-linux.so.2`. `GNU_LINK=static` links the ports that autoconf configures statically instead, with `-all-static` for a port with libtool, which reads `-static` as leaving the C library shared.

A program in the root runs on the build host through the root's own loader, which loads the root's glibc rather than the host's i386 one:

    R=build/vendor/gnu/root
    $R/lib/ld-linux.so.2 --library-path $R/lib:$R/usr/lib $R/usr/bin/git --version

The table gives each port's version, the time it took under `make -j32 gnu-ports` on the testing host, and the size of what it installs after stripping. The whole run took 565 seconds and made a root of 326 MB.

| Port      | Version    | Time  | Size    |
| --------- | ---------- | ----- | ------- |
| runtime   | glibc 2.44 | 3 s   | 65.0 MB |
| zlib      | 1.3.2      | 1 s   | 0.4 MB  |
| OpenSSL   | 4.0.2      | 32 s  | 31.5 MB |
| OpenSSH   | 10.5p1     | 30 s  | 10.1 MB |
| ncurses   | 6.6        | 43 s  | 11.1 MB |
| less      | 710        | 4 s   | 0.5 MB  |
| Vim       | 9.2.1132   | 17 s  | 38.0 MB |
| Perl      | 5.44.0     | 109 s | 80.3 MB |
| PCRE2     | 10.49      | 31 s  | 5.2 MB  |
| curl      | 8.22.0     | 20 s  | 5.9 MB  |
| Git       | 2.55.0     | 20 s  | 32.3 MB |
| coreutils | 9.12       | 43 s  | 12.4 MB |
| bash      | 5.3        | 23 s  | 5.8 MB  |
| make      | 4.4.1      | 7 s   | 0.9 MB  |
| grep      | 3.12       | 30 s  | 0.5 MB  |
| sed       | 4.10       | 19 s  | 0.5 MB  |
| gawk      | 5.4.1      | 14 s  | 4.1 MB  |
| findutils | 4.11.0     | 28 s  | 1.3 MB  |
| diffutils | 3.12       | 21 s  | 0.7 MB  |
| patch     | 2.8        | 13 s  | 0.3 MB  |
| tar       | 1.35       | 22 s  | 1.4 MB  |
| gzip      | 1.15       | 12 s  | 0.3 MB  |
| xz        | 5.8.4      | 11 s  | 1.4 MB  |
| file      | 5.48       | 7 s   | 10.7 MB |
| tzdata    | 2026e      | 0 s   | 1.5 MB  |
| iproute2  | 7.2.0      | 4 s   | 4.0 MB  |

The ports milestone 55 added were built one at a time, so the table of them gives each one's version and the size of what it installs.

| Port         | Version       | Size     |
| ------------ | ------------- | -------- |
| pkgconf      | 2.5.1         | 0.5 MB   |
| procps-ng    | 4.0.7         | 3.3 MB   |
| util-linux   | 2.42.4        | 12.4 MB  |
| zstd         | 1.5.7         | 3.6 MB   |
| rsync        | 3.5.1         | 1.1 MB   |
| libevent     | 2.1.13-stable | 2.3 MB   |
| tmux         | 3.7c          | 1.4 MB   |
| nano         | 9.2           | 1.0 MB   |
| Texinfo      | 7.3           | 10.6 MB  |
| strace       | 7.2           | 1.4 MB   |
| lsof         | 4.99.4        | 0.4 MB   |
| cronie       | 1.7.2         | 0.2 MB   |
| zip          | 3.0           | 0.6 MB   |
| unzip        | 6.0           | 0.4 MB   |
| bzip2        | 1.0.8         | 0.5 MB   |
| libffi       | 3.8.0         | 0.3 MB   |
| Readline     | 8.3           | 1.9 MB   |
| SQLite       | 3.53.4        | 4.8 MB   |
| Python       | 3.14.8        | 78.5 MB  |
| libgpg-error | 1.61          | 0.5 MB   |
| Libgcrypt    | 1.12.4        | 1.6 MB   |
| Libassuan    | 3.0.2         | 0.2 MB   |
| Libksba      | 1.8.1         | 0.5 MB   |
| nPth         | 1.8           | 0.1 MB   |
| NTBTLS       | 0.3.2         | 0.2 MB   |
| pinentry     | 1.3.3         | 0.2 MB   |
| GnuPG        | 2.5.24        | 9.4 MB   |
| htop         | 3.5.3         |          |
| dosfstools   | 4.2           |          |
| shadow       | 4.20.2        |          |

These notes say what some ports provide and what they leave to the system they are installed on.

* OpenSSL is built for `linux-x86`, shared, with zlib. Its configuration is in `/etc/ssl`, where the programs that use it look for certificates; the root has none, and the root image installs the CA bundle there.
* OpenSSH installs `ssh`, `scp`, `sftp`, `ssh-keygen` and the other clients in `/usr/bin`, `sshd` in `/usr/sbin`, and `sshd-session`, `sshd-auth`, `sftp-server` and the helpers in `/usr/libexec`, with its configuration in `/etc/ssh` and no PAM. The server's privilege-separated child runs as the user `sshd` in `/var/empty`, so the system needs that user and a `/var/empty` owned by root. The child that talks to a client before it authenticates runs in the rlimit sandbox, `--with-sandbox=rlimit`, with limits of 0 on the descriptors it may open, the processes it may fork and the size of the files it may grow, which it writes none of (`prlimit64(2)`). The kernel has no seccomp for the sandbox configure would choose, and configure takes the rlimit sandbox only after a test program it cannot run in a cross build, which shows that poll and select work under a descriptor limit of 0; the recipe gives it that answer, which limitprobe checks on the kernel. The host keys are made on the system. The root image has the user and the directory, and its init makes the keys.
* ncurses has wide characters and installs the terminfo database, 1861 entries, in `/usr/share/terminfo`. bash's readline, Vim and less read the terminal's capabilities through it. The console's entry, `rach2`, is not among them: `rach2.ti` has it, and `install_root.sh` compiles it into the root.
* Vim has its huge feature set, for the terminal only.
* Perl is configured as a native build whose programs run on the build host, for the reasons its recipe gives, with interpreter threads, as Debian's has, and has 143 extensions.
* Git has curl, OpenSSL, zlib and Perl, and no Tcl/Tk, gettext, Python, expat or Rust. Its manual pages come from the `git-manpages` tarball.
* coreutils and make start their children with glibc's `posix_spawn`, which shares the caller's address space through `clone` with `CLONE_VM` and `CLONE_VFORK`.
* pkgconf is built twice: for the build host, as `i686-linux-gnu-pkg-config` beside the cross tools, which answers `configure`'s questions about the root's libraries, and for the root, as `pkg-config`. Every port depends on it, so the build host needs no `pkg-config` of its own.
* procps-ng and util-linux install their programs in `/usr/bin` and `/usr/sbin`, where the path finds them before BusyBox's applets. util-linux's `libmount`, `libblkid` and `libuuid` replace Debian's copies, and `newgrp` is set-id root.
* tmux uses libevent and takes `tmux-256color` as its terminal. nano reads `/etc/nanorc`, which includes the syntax files. Texinfo installs `makeinfo`, `info` and `install-info`.
* lsof is built from Debian's source of it, since its upstream signs nothing, with its own Configure script. zip and unzip are Info-ZIP's last releases, since it makes no more, with Debian's patches applied from its packaging tarballs (`DEBIAN` in `build_port.sh`), unzip with bzip2.
* cronie's `crond` runs from init, and `crontab` is set-id to the group `crontab`, which may write the crontabs in `/var/spool/cron/crontabs`. There is no anacron and no PAM.
* Python has the modules of its standard library that link the root's libraries, and neither `tkinter`, `dbm.gnu`, pip nor the test suite. Its ssl module is built against OpenSSL 4, which has no SSLv3 and no methods of one protocol version. Its byte code records a hash of each source, since the image gives every file the same time.
* htop has ncurses' wide characters and CPU affinity, and no sensors, capabilities, delay accounting, hwloc or libunwind. dosfstools installs `mkfs.fat` and `fsck.fat` with Debian's other names for them, which the path finds before BusyBox's `mkdosfs`.
* shadow installs the account tools BusyBox has not, `useradd`, `usermod`, `userdel`, the group tools, `gpasswd`, `chsh`, `chfn`, `chage`, `vipw` and the checkers, with `/etc/login.defs`; `passwd`, `su`, `login`, `chpasswd` and `nologin` stay BusyBox's and `newgrp` util-linux's. `chsh`, `chfn` and `gpasswd` are set-id root, as on Debian. There is no PAM, no subordinate ids and no `lastlog`.
* GnuPG has its agent, `dirmngr` with NTBTLS for hkps, `keyboxd` with SQLite and `pinentry`'s curses and tty programs, and no LDAP and no TPM2 daemon.

## The root image

`make os-real-gnu-ext2` builds `build/images/os-real-gnu.ext2`, a 4 GiB ext2 volume, sparse on the host, that the kernel boots with `rach2.root=ext2`: the os-real system, staged from `krffs/manifests/os-real-gnu.manifest`, with this layer installed over it by `install_root.sh`. `tools/image/mkext2.sh --overlay` runs the script over the staged tree before it formats the volume, and the volume passes mkext2.sh's checks: what the overlay added is root's, with the fixed staged instant as its times, `e2fsck -fn` finds the volume clean, and the set-id guard reads it with its own reader and admits no set-id file but BusyBox's `su`, `passwd` and `login`, util-linux's `newgrp` and cronie's `crontab`, which the manifest installs from `build/vendor/gnu/setid`, where their recipes leave them. The system takes 1.2 GiB of the volume and 26,588 of its inodes; the rest is room for what the machine builds and fetches.

The image is built where the layer is, on a Linux host, and is not part of `make`. `sh tools/dev/testing.sh run make os-real-gnu-ext2` builds it in the testing host's copy, first building what of the layer is missing, and `sh tools/dev/testing.sh get build/images/os-real-gnu.ext2` brings it to the same path on the Mac, compressed on the wire, in 40 seconds; there it takes nearly all of its 4 GiB, since APFS keeps few of the volume's holes. With the layer built, the image took 10 to 14 seconds on the testing host; the layer from nothing took 447 seconds for the toolchain and then 639 seconds under `make -j32` for the ports, the native compiler, GDB, the shell and the browser at once.

`install_root.sh` installs the system as a Debian i386 system has it:

* The trees of the ports, the native compiler, GDB and the shell: glibc with its loader as `/lib/ld-linux.so.2` and its libraries in `/lib`, and the rest under `/usr`. The native compiler's `libstdc++.so.6` replaces the runtime's copy of the same library, and any other file the tree already has is refused, so what the manifest staged is what the image has. The two C libraries share `/lib`, where TinyCC finds musl's `libc.a` and start files beside glibc's shared libraries; GCC takes glibc's start files and libraries from `/usr/lib`, which it searches before `/lib`.
* From the packages `chromium/fetch_chromium.sh` unpacked, the browser in `/usr/lib/chromium`, its libraries in `/usr/lib/i386-linux-gnu`, and D-Bus's daemon and tools in `/usr/bin` with the bus's configuration in `/usr/share/dbus-1`: every package of the closure except those whose libraries the root has its own builds of, of the same sonames and newer, `libc6`, `libgcc-s1`, `libstdc++6`, `libatomic1`, `zlib1g`, `liblzma5`, `libpcre2-8-0`, `libmount1`, `libblkid1`, `libuuid1`, `libffi8`, `libsqlite3-0`, `libbz2-1.0` and `libzstd1`, with `gcc-14-base`, which only documents GCC's, and except the programs that `chromium-common`'s dependencies bring and a headless shell does not run, in `debconf`, `lsb-base`, `sysvinit-utils`, `x11-utils` and `xdg-utils`.
* `/etc/ld.so.cache`, which the root's own `ldconfig` writes from the manifest's `/etc/ld.so.conf`, which lists `/usr/lib/i386-linux-gnu`. glibc links `ldconfig` statically, so the build host runs it without the loader, with `-r` for the tree. The cache has 182 entries, and the loader searches `/lib` and `/usr/lib` for a library the cache does not have.
* `/bin/bash`, a link to `/usr/bin/bash`, `/bin/sh`, a second name of bash's file, since the set-id guard requires the shell root runs to be a file, `/usr/bin/cc`, a link to `gcc`, and `/usr/bin/vi`, a link to `vim`.
* A link to `/bin/busybox` for each BusyBox applet that no file in `/usr/bin`, `/bin`, `/usr/sbin` or `/sbin` answers for, 203 of the 318, where BusyBox's own install would put it. The build host runs the BusyBox the manifest stages, which is static, from the build for the list, since the stager writes the tree's files without the modes the volume gets.
* For each program of `/bin` that `/usr/bin` lacks, a link of its name in `/usr/bin`, and for each of `/usr/bin` that `/bin` lacks, one in `/bin`, and the same for `/sbin` and `/usr/sbin`: Debian's `/bin` and `/sbin` are links into `/usr`, so every program has both names, and a script that names `/bin/true` or `/usr/sbin/ldconfig` finds it. A name both have keeps its two programs.
* The ports' manual pages rendered to plain text under `/usr/share/man/gnu/catN`, by `render_man.sh` with the build host's mandoc.
* The terminfo entry of the console, from `rach2.ti`, compiled by the root's `tic`, which the build host runs through the root's loader. The kernel gives a console session `TERM=rach2`, and the entry lists what the console's interpreter in `rach2/sys/kio.c` does and what its keyboard sends.
* The CA bundle as `/etc/ssl/certs/ca-certificates.crt`, Debian's name for it, and `/etc/ssl/cert.pem`, the file OpenSSL reads by default, as a link to it, so that curl and Git verify HTTPS against Mozilla's roots.
* `/etc/services`, `/etc/protocols` and `/etc/networks`, the files Debian's netbase gives, written from IANA's assignments in `etc/`, so that a program names a port as `https` or `ssh`; and `/etc/localtime`, a link to the zone `Etc/UTC`, with `/etc/timezone` naming it, as Debian's tzdata leaves a machine set to UTC.
* `ssh-keysign` without its set-id bit: the system uses no host-based authentication, and the guard admits no set-id file the manifest does not declare.
* `/run`, a link to `/var/run`, where D-Bus's programs look for the system bus's socket, `/run/dbus/system_bus_socket`.
* `/usr/share/config.site`, linked from `/usr/local/share/config.site`, which autoconf's `configure` reads for the prefixes `/usr` and `/usr/local`: it names the build `i686-pc-linux-gnu`, since `config.guess` knows no system by the kernel's name, `Rach2`, and stops every `configure` run without `--build`.

The ports install their upstream manual pages under `/usr/share/man/manN` as troff sources, which the system's `man` does not read: it reads plain text. `render_man.sh` renders the pages of sections 1, 5, 7 and 8 with the build host's mandoc, as `man/render.sh` renders this project's, into `/usr/share/man/gnu/catN`, 1213 pages, and `man` looks a name up there when this project's manual has no page for it, or first when the command the path finds by the name is a port's and not a link to BusyBox, so `man git` shows Git's page and `man ls` GNU's.

### Which program a name finds

The path, as `/etc/profile` sets it and as OpenSSH's server gives a session, is `/usr/local/bin:/usr/bin:/bin:/usr/local/sbin:/usr/sbin:/sbin`, so what `make install` puts under `/usr/local` answers first, and a GNU program in `/usr/bin` before a file of `/bin`. Root and the test account log in to bash; `nobody` and `nogroup` are Debian's account and group with nothing of their own, 65534; `/etc/profile` gives a prompt of the user, the machine and the directory, a session on a terminal other than the console the C.UTF-8 locale that glibc builds in, since the console draws ASCII alone, and a D-Bus session bus, as the section on D-Bus below says. `/etc/hosts` is a machine's: `localhost` and the machine's own name, `rach2`, at 127.0.1.1. Two names are in both: `make`, which is GNU make before pdpmake, and `cc`, which is GCC before TinyCC. A program that asks the path for either on this system is nearly always a GNU build, a configure script or a makefile written for GNU make and GCC, and TinyCC refuses the dynamic link such a build asks for. pdpmake and TinyCC keep `/bin/make`, `/bin/tcc` and `/bin/cc`, which the musl self-build of `/src` names: in a writable copy of a component's tree, `/bin/make -f guest.mk` runs pdpmake on the POSIX makefile, which calls the compiler `/bin/cc`.

`/bin/sh` is bash, so a script, what runs a command through the shell, such as `system(3)` and a recipe with shell syntax in it, and a package's test suite find GNU's `grep` and `sed` by the path, as on a GNU/Linux system. BusyBox's ash, the os-real system's `/bin/sh`, runs its own applet for a name before it searches the path at all, which gave GNU sed's test suite BusyBox's sed through Perl's `system`. The system's scripts, `man`, `dmesg` and udhcpc's, are POSIX shell scripts and run under bash. The applets no GNU program stands for, such as `ps`, `top`, `free`, `mount`, `which`, `hostname` and `ifconfig`, are files as well, links to `/bin/busybox` that `install_root.sh` makes, so bash and a program that searches the path find them; `vi` is a link to Vim, so that Git's default editor is found. GNU tar's recipe names gzip, xz and lzma by their paths, so that `tar -cJ` runs GNU's xz whatever the caller's path holds.

### OpenSSH in Dropbear's place

The image's `/sbin/init` is `init-metal`, which starts OpenSSH's `sshd` at boot where the machine's disk starts Dropbear: after the console login and, on a boot with `rach2.ip=none`, after `ifup -a` (`init(8)`). Before each start it runs `ssh-keygen -A`, which on the first boot makes the host keys in `/etc/ssh`, RSA, ECDSA, Ed25519 and ML-DSA-44 with Ed25519, and needs the kernel's entropy. The server runs in the foreground with its log in `/var/log/services`, and its unprivileged child runs as `sshd`, uid 74, with no password and `/usr/bin/false` for a shell, in `/var/empty`, which is root's and mode 0755. Root logs in by key alone unless `/etc/rach2/sshd.conf` permits its password, and the test account by key or by the password in `/etc/shadow`, which glibc and libxcrypt check. `ssh`, `scp` and `sftp` are the clients, and `sftp-server` serves the last two. Dropbear stays in the other images.

### D-Bus's system bus

Debian's `dbus-daemon`, from the browser's closure, is the system bus that the browser and other programs linked with libdbus connect to at `/run/dbus/system_bus_socket`; without it the browser prints an error for each of its parts that asks the bus for something. `init-metal` starts it at boot as the user `messagebus`, uid 81, with no password and `/usr/bin/false` for a shell, after it has made `/var/run/dbus`, owned by that user, and on the first boot `/etc/machine-id`, the machine's identity, from the kernel's random numbers (`init(8)`). The bus is started as its user rather than as root, which Debian's service does: started as root, it drops to its user through libcap-ng, which keeps part of root's capabilities, and the kernel keeps all of a process's capabilities or none (`capget(2)`). It reads Debian's configuration in `/usr/share/dbus-1/system.conf`, and its messages go to the service log. No service is activated by the bus: Debian's `dbus` package, whose set-id helper activates them, is not installed. A login shell has a session bus of its user's, at `~/.dbus/session_bus_socket`, which `/etc/profile` starts where none answers there and names in `DBUS_SESSION_BUS_ADDRESS`, as a desktop's login gives one; a program started outside a login shell can have one of its own from `dbus-run-session`.

`tests/run gnu` boots the image and has each port do its main job on the machine ([the system tests' Readme](../../tests/system/Readme.md)).
