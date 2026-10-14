# The GNU layer

The GNU layer builds a GNU system for Rach2: a cross toolchain with glibc, 72 port recipes built with it, a native GCC and GDB, the SpiderMonkey shell, and Debian's chromium-headless-shell, all installed over the os-real system in an ext2 root that the kernel boots. It gives Rach2 the dynamically linked programs a person expects of a Linux machine (bash, coreutils, OpenSSH, Python, Git, GCC), which the static musl system does not have. It builds on an x86-64 Linux host only and is not part of `make`. The targets are `i686-linux-gnu` and `x86_64-rach2-linux-gnu`, as `ARCH` selects.

## Use

The Make targets build for the architecture `ARCH` names, so `make ARCH=x86_64 gnu-ports` builds the x86-64 ports:

* `make gnu-toolchain`: the cross toolchain, by `build_toolchain.sh`.
* `make gnu-ports`: every recipe of `ports/`, by `build_port.sh --all`.
* `make gnu-native` and `make gnu-gdb`: binutils, GCC and GDB that run on Rach2 ([`native/`](native/Readme.md)).
* `make gnu-js`: the SpiderMonkey shell ([`spidermonkey/`](spidermonkey/Readme.md)).
* `make gnu-chromium`: Debian's chromium-headless-shell with its libraries ([`chromium/`](chromium/Readme.md)).
* `make os-real-gnu-ext2`: the root image, `build/images/os-real-gnu.ext2` or `build/x86_64/images/os-real-gnu.ext2`, 4096 MiB and sparse, which needs all of the above. `make machine` builds it with the kernels and the recovery image an installed machine takes.

Each target is behind a stamp; [`mk/Readme.md`](../../mk/Readme.md) says what rebuilds it. The scripts also run alone, with `RACH2_ARCH` for the architecture:

* `fetch.sh [NAME...]` downloads tarballs into `downloads/` and checks them; `fetch.sh --path NAME` prints one tarball's path.
* `build_toolchain.sh [STEP...]` runs the steps `binutils`, `headers`, `gcc1`, `glibc`, `gcc2` and `libxcrypt`; with none it builds the toolchain again from nothing.
* `build_port.sh NAME...` builds those ports, and the ports they depend on that are not built yet, into the existing root.

A program of the root runs on the build host through the root's own loader, which loads the root's glibc:

    R=build/vendor/gnu/root
    $R/lib/ld-linux.so.2 --library-path $R/lib:$R/usr/lib $R/usr/bin/git --version

On x86-64 the root is `build/x86_64/vendor/gnu/root` and the loader `lib64/ld-linux-x86-64.so.2`.

### On the machine

The image boots with `rach2.root=ext2`. Its `/sbin/init` is `init-metal`, which starts OpenSSH's `sshd`, D-Bus's system bus and cronie's `crond` ([`init(8)`](../../man/man8/init.8)); Dropbear is not installed. Root and `test` log in to bash. `/etc/profile` sets the path to `/usr/local/bin:/usr/bin:/bin:/usr/local/sbin:/usr/sbin:/sbin`, gives every terminal but a dumb one `LANG=C.UTF-8`, and starts a D-Bus session bus at `~/.dbus/session_bus_socket` where none answers.

The path finds a GNU program in `/usr/bin` before a file of `/bin`. `make` is GNU make and `cc` is GCC; pdpmake and TinyCC keep `/bin/make`, `/bin/cc` and `/bin/tcc`, which the self-build of `/src` names. `/bin/sh` is bash, so a script and `system(3)` find GNU's `sed` and `grep` by the path, where BusyBox's ash would run its own applet first. Each BusyBox applet that no file of the four program directories provides is a link to `/bin/busybox`. `man` also finds the ports' pages ([`man(1)`](../../man/man1/man.1)).

## Sources and license

[`sources.txt`](sources.txt) pins each tarball of the toolchain and the ports, and [`sources-extra.txt`](sources-extra.txt) the sources of the three subdirectories, the Debian archive keys and the CA bundle. A line gives a name, version, URL and SHA-256, and each file's header says which upstream key verified each digest. `fetch.sh` downloads a file once, into `downloads/`, which Git ignores, and checks its digest every time a script uses it; a file with another digest is removed and refused. Nothing fetched is committed.

Each port keeps its upstream license, which its tarball has. The scripts, the recipes, `rach2.ti` and the files in `etc/` are in-house.

## Local changes

A recipe changes its upstream only where Rach2 or a cross build needs it, and its comment gives the reason:

* bash and Readline take their upstream patch series, pinned in `sources.txt`. zip and unzip take Debian's patch series from Debian's packaging tarballs (`DEBIAN` in [`build_port.sh`](build_port.sh)).
* The toolchain and the native GCC point the `m64` line of GCC's `gcc/config/i386/t-linux64` at `lib`, so that x86-64 libraries are in `/lib` and `/usr/lib`, as on i686.
* [`ports/lsof.sh`](ports/lsof.sh) treats an unknown count of an epoll descriptor's watched descriptors as 0; [`ports/bind9.sh`](ports/bind9.sh) keeps OpenSSL's own allocator; [`ports/chawan.sh`](ports/chawan.sh) adds the Rach2 console, `TERM=rach2`, to Chawan's table of terminals; [`ports/tmux.sh`](ports/tmux.sh) dates tmux's manual page.
* A recipe's `CACHE` gives configure the answers a cross build cannot find out. [`ports/openssh.sh`](ports/openssh.sh) edits configure's own cross-build answer instead, so that it accepts the rlimit sandbox.

## Design

### The build tree

Everything is built under `build/vendor/gnu/` for i686 and `build/x86_64/vendor/gnu/` for x86-64. [`arch.sh`](arch.sh) gives each script that tree, the target, GCC's `--with-arch`, the loader, and Debian's names for the architecture.

| Directory | Contents |
| --- | --- |
| `cross/` | binutils and GCC for C and C++, as `TARGET-gcc` and the rest |
| `sysroot/` | the Linux headers, glibc and libxcrypt |
| `wrap/` | compiler wrappers that pass `--sysroot=root/`, and the build host's `TARGET-pkg-config` |
| `root/` | the runtime and every port |
| `stage/` | each port's install, and `NAME.files`, the list of what it put in `root/` |
| `setid/` | programs a recipe moved out of its stage to be installed set-id |
| `host/` | CMake for the build host |
| `native/`, `gdb/`, `js/`, `chromium/` | the staging roots of the subdirectories |
| `rust/` | the Rust toolchain of the SpiderMonkey build |
| `src/`, `work/`, `log/` | sources, build directories and step logs |

### The toolchain

`build_toolchain.sh` builds binutils, installs the Linux headers, builds a C compiler without a C library, glibc, the final GCC for C and C++, and libxcrypt, which provides the `crypt` that glibc no longer has. GMP, MPFR and MPC are built in GCC's tree. glibc keeps its default minimum kernel, 3.2.0, so it falls back from newer system calls to older ones. The loader is `/lib/ld-linux.so.2` on i686 and `/lib64/ld-linux-x86-64.so.2` on x86-64, and the libraries are in `/lib` and `/usr/lib` on both. The x86-64 target has the vendor `rach2` because the build host's own triplet is `x86_64-linux-gnu`, and GCC and autoconf take a target of that name for a native build.

### The ports

A recipe in `ports/` is a shell fragment that `build_port.sh` reads. It sets `VERSION`, `DEPENDS`, `CONFIGURE_ARGS`, `CACHE` and `PATCHES`, and may define `port_build` and `port_install` for a port that autoconf does not configure; [`build_port.sh`](build_port.sh) documents every variable. By default a port is configured with the prefix `/usr`, `/etc` and `/var`, as Debian configures it. `configure_cmake` configures a CMake port with the host CMake.

Every other port depends on `runtime` and `pkgconf`. [`ports/runtime.sh`](ports/runtime.sh) has no tarball: it copies the sysroot, adds GCC's `libgcc_s.so.1` and `libstdc++.so.6`, and writes a locale archive with `C.UTF-8` and `en_US.UTF-8` using the target's `localedef`. [`ports/pkgconf.sh`](ports/pkgconf.sh) also builds a `pkg-config` for the build host that reads only the root's files, so the host needs none.

A port installs into `stage/NAME`. There the target's `strip --strip-debug` runs on its ELF files, and its libtool archives and Info index are removed. Files the port's previous list had and the new one lacks are removed from the root, and the stage is copied in. Programs link with the shared glibc; `GNU_LINK=static` gives an autoconf port `LDFLAGS=-static`, and `-all-static` where it uses libtool.

### The root image

[`mk/gnu-image.mk`](../../mk/gnu-image.mk) runs `tools/image/mkext2.sh --overlay vendor/gnu/install_root.sh` on [`os-real-gnu.manifest`](../../krffs/manifests/os-real-gnu.manifest). The manifest stages the musl system with OpenSSH's accounts and directories, bash as the login shell, glibc's `/etc` files and the set-id programs from `setid/`. [`tools/Readme.md`](../../tools/Readme.md) describes how `mkext2.sh` stages, formats and checks the volume.

[`install_root.sh`](install_root.sh) refuses to replace a file the staged tree has, except the runtime's `libstdc++.so.6`, which the native compiler's copy replaces. It installs:

* `root/`, `native/`, `gdb/` and `js/`.
* The files of the Chromium packages, with paths under `/bin`, `/lib` and `/sbin` moved under `/usr`. Packages whose libraries the root already builds (`libc6`, GCC's, zlib, liblzma, PCRE2, util-linux's, libffi, SQLite, bzip2 and zstd) and the programs `chromium-common` pulls in that a headless shell does not run are left out; the list is `SKIP` in the script.
* Links: `/bin/bash`, `/bin/apropos` and `/bin/whatis` to `man`, `/usr/bin/cc` to `gcc`, `/usr/bin/vi` to `vim`, `/bin/mkfs.ext2`, `/sbin/fsck.ext2`, `/sbin/e2label`, `/etc/mtab` to `/proc/self/mounts`, and `/run` to `/var/run`. `/bin/sh` is a hard link to bash, since the set-id guard requires root's shell to be a file.
* A link to `/bin/busybox` for each unprovided applet, from the static BusyBox's `--list-full`, and a link in `/bin` for each program only `/usr/bin` has, and the reverse, and the same for `/sbin` and `/usr/sbin`.
* The CA bundle as `/etc/ssl/certs/ca-certificates.crt`, with `/etc/ssl/cert.pem` linked to it.
* `/etc/services`, `/etc/protocols` and `/etc/networks` from [`etc/`](etc), and `/etc/localtime` and `/etc/timezone` for `Etc/UTC`.
* The console's terminfo entry, compiled from [`rach2.ti`](rach2.ti) by the root's `tic`.
* The ports' manual pages of sections 1, 5, 7 and 8, rendered by [`render_man.sh`](render_man.sh) with the host's mandoc to `/usr/share/man/gnu/catN`. A page dated `$Mdocdate$` alone is refused, so that two builds render alike.
* A locked `/etc/shadow` line for each account of `/etc/passwd` that has none, and `/etc/ld.so.cache`, written by the root's static `ldconfig` from the staged `/etc/ld.so.conf`.

It also removes the set-id bit of `ssh-keysign`, since the system uses no host-based authentication.

## Building

The build host is an x86-64 Linux with a C and C++ compiler, GNU make, gawk, bison, flex, m4, Perl, Python 3, texinfo, xz, patch, gzip, and curl or wget. The subdirectories add `unshare` from util-linux, `chroot`, and a kernel that lets an ordinary user make user namespaces, for their checks; GnuPG, `ar` and tar for Chromium's packages; and access to crates.io for cbindgen's dependencies. The root image needs mandoc and e2fsprogs, which [`tools/Readme.md`](../../tools/Readme.md) lists for every build.

Under make the scripts share the caller's jobs; alone they use every processor, or `GNU_JOBS` processors in the subdirectories' scripts.

## Updating

To update a port, verify the new tarball's signature with `gpgv` and the key the header of `sources.txt` names, replace the port's line with the new version, URL and digest, set `VERSION` in its recipe, and run `build_port.sh NAME`. A change to `sources.txt` rebuilds every port under `make gnu-ports`, and a change to a toolchain line (binutils, GCC, GMP, MPFR, MPC, glibc, Linux, libxcrypt) rebuilds the toolchain and everything built with it. A new port is a recipe and a line; `--all` builds every recipe in `ports/`.

## Limits

* The layer builds on an x86-64 Linux host only.
* There is no PAM: OpenSSH, sudo, shadow and cronie are built without it.
* D-Bus activates no service, since Debian's `dbus` package, which has the activation helper, is not installed.
* chromium-headless-shell and D-Bus are Debian's binaries, not built here.

## Tests

`tests/run gnu` (`ARCH=x86_64 tests/run gnu` for x86-64) builds the root image, boots it, logs in over OpenSSH, and has each port and program do its main job on the machine, one row each; [`tests/system/Readme.md`](../../tests/system/Readme.md) lists the rows. It is in the full tier. The `check` steps of the subdirectories' scripts test their programs on the build host.
