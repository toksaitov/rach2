# The GNU layer

The GNU layer builds a GNU system for Rach2: a cross toolchain with glibc, 72 port recipes built with it, a native GCC and GDB, the SpiderMonkey shell, and Debian's chromium-headless-shell, all installed over the os-real system in an ext2 root that the kernel boots. On x86-64 the root can also have the [agents layer](agents/Readme.md), vendors' published builds of coding agents and their tools, such as Anthropic's Claude Code and GitHub's `gh`. It gives Rach2 the dynamically linked programs a person expects of a Linux machine (bash, coreutils, OpenSSH, Python, Git, GCC), which the static musl system does not have. It builds on an x86-64 Linux host; on any other host, such as a Mac, its root images are built in etude's tooling machine, a Linux in QEMU. It is not part of `make`. The targets are `i686-linux-gnu` and `x86_64-rach2-linux-gnu`, as `ARCH` selects.

## Use

The Make targets build for the architecture `ARCH` names, so `make ARCH=x86_64 gnu-ports` builds the x86-64 ports:

* `make gnu-toolchain`: the cross toolchain, by `build_toolchain.sh`.
* `make gnu-ports`: every recipe of `ports/`, by `build_port.sh --all`.
* `make gnu-fstools`: independent exFAT helper tools, from their separate pin, without rebuilding the GNU ports or native compiler.
* `make gnu-native` and `make gnu-gdb`: binutils, GCC and GDB that run on Rach2 ([`native/`](native/Readme.md)).
* `make gnu-js`: the SpiderMonkey shell ([`spidermonkey/`](spidermonkey/Readme.md)).
* `make gnu-chromium`: Debian's chromium-headless-shell with its libraries ([`chromium/`](chromium/Readme.md)).
* `make ARCH=x86_64 GNU_AGENTS=NAME... gnu-agents`: the packages of the agents layer that `GNU_AGENTS` names, or every one for `all` ([`agents/`](agents/Readme.md)).
* `make os-real-gnu-ext2`: the root image, `build/images/os-real-gnu.ext2` or `build/x86_64/images/os-real-gnu.ext2`, 4096 MiB and sparse, which needs all of the above; of the agents layer it has the packages `GNU_AGENTS` names. `make os-real-gnu-agents-ext2` builds `os-real-gnu-agents.ext2`, which has every package. `make machine` builds the first with the kernels and the recovery image an installed machine takes. On a host other than x86-64 Linux these two targets build the layer and the image in the tooling machine, as [Building](#building) says, and the targets above them are not used.

Each target is behind a stamp; [`mk/Readme.md`](../../mk/Readme.md) says what rebuilds it. The scripts also run alone, with `RACH2_ARCH` for the architecture:

* `fetch.sh [NAME...]` downloads tarballs into `downloads/` and checks them; `fetch.sh --path NAME` prints one tarball's path.
* `build_toolchain.sh [STEP...]` runs the steps `binutils`, `headers`, `gcc1`, `glibc`, `gcc2` and `libxcrypt`; with none it builds the toolchain again from nothing.
* `build_port.sh NAME...` builds those ports, and the ports they depend on that are not built yet, into the existing root.

A program of the root runs on the build host through the root's own loader, which loads the root's glibc:

    R=build/vendor/gnu/root
    $R/lib/ld-linux.so.2 --library-path $R/lib:$R/usr/lib $R/usr/bin/git --version

On x86-64 the root is `build/x86_64/vendor/gnu/root` and the loader `lib64/ld-linux-x86-64.so.2`.

### On the machine

The image boots with `rach2.root=ext2`. Its `/sbin/init` is `init-metal`, which starts OpenSSH's `sshd`, D-Bus's system bus, cronie's `crond` and BusyBox's `ntpd` ([`init(8)`](../../man/man8/init.8)); Dropbear is not installed. Root and `test` log in to bash. `/etc/profile` sets the path to `/usr/local/bin:/usr/bin:/bin:/usr/local/sbin:/usr/sbin:/sbin`, gives every terminal but a dumb one `LANG=C.UTF-8`, and starts a D-Bus session bus at `~/.dbus/session_bus_socket` where none answers.

BusyBox's `ntpd` keeps the clock. init starts it once the network is up, while `/etc/ntp.conf` has a line `server NAME`, where NAME is a host name or an address, with `:PORT` after it for a port other than 123; the image's file names the NTP pool's servers, `0.pool.ntp.org` to `3.pool.ntp.org`. ntpd reads the file when it starts, so after an edit run `killall ntpd`, and init starts it again within a second. It writes each step of the clock and each error to `/var/log/services`. The kernel has no slew ([`adjtimex(2)`](../../man/man2/adjtimex.2)): each correction steps the time, and the rate of the clock stays as it was.

The path finds a GNU program in `/usr/bin` before a file of `/bin`. `make` is GNU make and `cc` is GCC; pdpmake and TinyCC keep `/bin/make`, `/bin/cc` and `/bin/tcc`, which the self-build of `/src` names. `/bin/sh` is bash, so a script and `system(3)` find GNU's `sed` and `grep` by the path, where BusyBox's ash would run its own applet first. Each BusyBox applet that no file of the four program directories provides is a link to `/bin/busybox`. `man` also finds the ports' pages ([`man(1)`](../../man/man1/man.1)).

## Sources and license

[`sources.txt`](sources.txt) pins each tarball of the toolchain and the ports, and [`sources-extra.txt`](sources-extra.txt) the sources of the three subdirectories, the Debian archive keys and the CA bundle. A line gives a name, version, URL and SHA-256, and each file's header says which upstream key verified each digest. `fetch.sh` downloads a file once, into `downloads/`, which Git ignores, trying up to three times, and checks its digest every time a script uses it; a file with another digest is removed and refused. Nothing fetched is committed. The separate `gnu-fstools` step builds the exfatprogs pin in [`fstools/sources.txt`](fstools/sources.txt) against the existing cross compiler and sysroot. It publishes only the independent helper tools under `fstools/`, without changing the GNU port list, shared source pins or root. The package links a private positional-write adapter for recovery block nodes: partial sectors use read/modify/write, aligned writes and regular files retain their normal behavior. The single-threaded tools open writable devices exclusively; no libc or kernel write policy changes. GNU test image assembly copies its formatter and checker beside dosfstools' existing FAT checker. The GNU test image requires these programs and the compiled `/host-products/fstool` dispatcher before publication; machine cases execute them over the framed guest channel. The test image also stages the prepared compressed source archive at `/host-products/scripts-assets.tar`. Its Make dependency and tooling input list include that archive; the native scenarios check its digest against their declared artifact before extracting private source trees.

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
| `agents/` | the stage of each package of the agents layer, on x86-64 |
| `rust/` | the Rust toolchain of the SpiderMonkey build |
| `src/`, `work/`, `log/` | sources, build directories and step logs |

With the host's store of step outputs ([etude's Readme](../../tools/etude/Readme.md#the-step-store)), each step of the layer is built in the store, in the entry of its key, and the tree has links to the step's directories there in their place: `cross/` and `sysroot/` for the toolchain; `root/`, `stage/`, `setid/`, `host/` and `wrap/` for the ports; `native/`, `gdb/` and `js/`; `chromium/`, `work/chromium/files` and `work/chromium/closure.txt` for Chromium; and `agents/NAME` for each package of the agents layer. An entry keeps those alone, not the step's `src/`, `work/` and `log/`, and is read-only. A script that copies from an entry and writes to the copy, as the runtime port copies the sysroot, makes the copy writable first, and `install_root.sh` gives every node of the image its owner's write permission and sets the narrower modes the ports install, which an entry, readable by every account, does not keep, so that the image is the same whether the layer came from the store or was built in the copy. A copy whose make builds a step another copy has built from the same inputs takes its links and keeps no build of its own; one whose inputs differ builds the step again in a new entry, through the host's ccache, which has the compiles that did not change. A layer that a copy built in its own tree, without the store or before the host had one, is replaced when a make with the store next uses it: the toolchain's rule removes the tree's directories and stamps, and each step then takes its links. A copy that uses no GNU target keeps such a layer, as a ci runner keeps the one a release's full run builds without the store; a worker loop removes the one in its clone between issues ([tools/Readme.md](../../tools/Readme.md)). A script run alone writes the tree, and fails on a link to the store; `RACH2_STORE= make gnu-ports`, and the other targets, build the layer in the tree first. A wrapper of `wrap/` runs its compiler through the ccache that `ETUDE_STEP_CCACHE` names, when that is set.

### The toolchain

`build_toolchain.sh` builds binutils, installs the Linux headers, builds a C compiler without a C library, glibc, the final GCC for C and C++, and libxcrypt, which provides the `crypt` that glibc no longer has. GMP, MPFR and MPC are built in GCC's tree. glibc keeps its default minimum kernel, 3.2.0, so it falls back from newer system calls to older ones. The loader is `/lib/ld-linux.so.2` on i686 and `/lib64/ld-linux-x86-64.so.2` on x86-64, and the libraries are in `/lib` and `/usr/lib` on both. The x86-64 target has the vendor `rach2` because the build host's own triplet is `x86_64-linux-gnu`, and GCC and autoconf take a target of that name for a native build.

### The ports

A recipe in `ports/` is a shell fragment that `build_port.sh` reads. It sets `VERSION`, `DEPENDS`, `CONFIGURE_ARGS`, `CACHE` and `PATCHES`, and may define `port_build` and `port_install` for a port that autoconf does not configure; [`build_port.sh`](build_port.sh) documents every variable. By default a port is configured with the prefix `/usr`, `/etc` and `/var`, as Debian configures it. `configure_cmake` configures a CMake port with the host CMake.

Every other port depends on `runtime` and `pkgconf`. [`ports/runtime.sh`](ports/runtime.sh) has no tarball: it copies the sysroot, adds GCC's `libgcc_s.so.1` and `libstdc++.so.6`, and writes a locale archive with `C.UTF-8` and `en_US.UTF-8` using the target's `localedef`. [`ports/pkgconf.sh`](ports/pkgconf.sh) also builds a `pkg-config` for the build host that reads only the root's files, so the host needs none.

`--all` builds every recipe into an empty root, which takes minutes on the build host and many hours in the tooling machine under TCG. So that a stopped build keeps the ports it finished, `--all` run by make writes the key that `etude build` gives the step, as `ETUDE_STEP_KEY`, to `work/ports.done`, and adds each port's name there once the port is in the root. The next `--all` with that key, which make runs when the stamp is gone and nothing it depends on has changed, makes the root again from the stages of those ports, in the order they were built, and goes on from the next port, so that it ends with the root a build that never stopped makes. Any other run of the script, and an `--all` that finishes, removes the list. With the store, a stopped step's entry is removed, as for every step, and the next build begins again from an empty root.

A port installs into `stage/NAME`. There the target's `strip --strip-debug` runs on its ELF files, and its libtool archives and Info index are removed. Files the port's previous list had and the new one lacks are removed from the root, and the stage is copied in. Programs link with the shared glibc; `GNU_LINK=static` gives an autoconf port `LDFLAGS=-static`, and `-all-static` where it uses libtool.

### The root image

[`mk/gnu-image.mk`](../../mk/gnu-image.mk) runs `tools/image/mkext2.sh --overlay vendor/gnu/install_root.sh` on [`os-real-gnu.manifest`](../../krffs/manifests/os-real-gnu.manifest). The manifest stages the musl system with OpenSSH's accounts and directories, bash as the login shell, glibc's `/etc` files and the set-id programs from `setid/`. [`tools/Readme.md`](../../tools/Readme.md) describes how `mkext2.sh` stages, formats and checks the volume.

[`install_root.sh`](install_root.sh) refuses to replace a file the staged tree has, except the runtime's `libstdc++.so.6`, which the native compiler's copy replaces. It installs:

* `root/`, `native/`, `gdb/` and `js/`.
* The files of the Chromium packages, with paths under `/bin`, `/lib` and `/sbin` moved under `/usr`. Packages whose libraries the root already builds (`libc6`, GCC's, zlib, liblzma, PCRE2, util-linux's, libffi, SQLite, bzip2 and zstd) and the programs `chromium-common` pulls in that a headless shell does not run are left out; the list is `SKIP` in the script.
* The packages of the agents layer that `GNU_AGENTS` names, each as `fetch_agents.sh` staged it.
* Links: `/bin/bash`, `/bin/apropos` and `/bin/whatis` to `man`, `/usr/bin/cc` to `gcc`, `/usr/bin/vi` to `vim`, `/bin/mkfs.ext2`, `/sbin/fsck.ext2`, `/sbin/e2label`, `/etc/mtab` to `/proc/self/mounts`, and `/run` to `/var/run`. `/bin/sh` is a hard link to bash, since the set-id guard requires root's shell to be a file.
* A link to `/bin/busybox` for each unprovided applet, from the static BusyBox's `--list-full`, and a link in `/bin` for each program only `/usr/bin` has, and the reverse, and the same for `/sbin` and `/usr/sbin`.
* The CA bundle as `/etc/ssl/certs/ca-certificates.crt`, with `/etc/ssl/cert.pem` linked to it.
* `/etc/services`, `/etc/protocols` and `/etc/networks` from [`etc/`](etc), and `/etc/localtime` and `/etc/timezone` for `Etc/UTC`.
* The console's terminfo entry, compiled from [`rach2.ti`](rach2.ti) by the root's `tic`.
* The ports' manual pages of sections 1, 5, 7 and 8, rendered by [`render_man.sh`](render_man.sh) with the host's mandoc to `/usr/share/man/gnu/catN`. A page dated `$Mdocdate$` alone is refused, so that two builds render alike.
* A locked `/etc/shadow` line for each account of `/etc/passwd` that has none, and `/etc/ld.so.cache`, written by the root's static `ldconfig` from the staged `/etc/ld.so.conf`.

It also removes the set-id bit of `ssh-keysign`, since the system uses no host-based authentication.

## Building

The build host is an x86-64 Linux, or an aarch64 Linux that runs i386 and x86-64 programs through QEMU's user-mode emulators, as the tooling machine on a Mac does, with a C and C++ compiler, GNU make, gawk, bison, flex, m4, Perl, Python 3, texinfo, xz, patch, gzip, and curl or wget. The subdirectories add `unshare` from util-linux, `chroot`, and a kernel that lets an ordinary user make user namespaces, for their checks; GnuPG, `ar` and tar for Chromium's packages; and access to crates.io for cbindgen's dependencies. The root image needs mandoc and e2fsprogs, which [`tools/Readme.md`](../../tools/Readme.md) lists for every build.

Under make the scripts share the caller's jobs; alone they use every processor, or `GNU_JOBS` processors in the subdirectories' scripts.

### Sanitizer runtimes

`make gnu-sanitizers` prepares AddressSanitizer and UndefinedBehaviorSanitizer from the pinned GCC source without rebuilding the compiler. The x86-64 output also has LeakSanitizer. The cached step consumes the existing cross compiler and sysroot and publishes `sanitizers/`: `compiler/` supplies the compiler's sanitizer link inputs and interface headers, and `root/usr/lib/` supplies the shared runtimes and their C++ and unwinder dependencies. `mk/gnu-sanitizers.mk` exports the compiler flags and both paths for the GNU host-test preparation rules.

The step runs only during artifact preparation. It does not run probes or claim that Rach2 supports a sanitizer's runtime operations. The host suite runs the prepared fault probes and requires their diagnostics before accepting the runtime. A failed preparation keeps its work directory and prints its path.

### In the tooling machine

On any other host, or with `GNU_TOOLING=1` on Linux, `make os-real-gnu-ext2` and `make os-real-gnu-agents-ext2` run [`tooling_image.sh`](tooling_image.sh), which builds the layer and the image in etude's tooling machine ([`tools/etude/Readme.md`](../../tools/etude/Readme.md#the-tooling-machine)), an Ubuntu in QEMU. On a Mac with Apple silicon the machine is the arm64 one, under Apple's hypervisor, so the compilers run at about the host's speed; the programs of the target that the build runs, such as the loader that `runtime.sh` runs `localedef` with, Perl's Configure tests, bzip2's test of itself and `install_root.sh`'s `ldconfig`, run through QEMU's user-mode emulators, which the job gives the toolchain's sysroot as `QEMU_LD_PREFIX`, so that a program the build has just linked finds its loader and C library there, as on an x86-64 host it finds the host's. QEMU's emulator cannot trace a program, so the check of `native/build_gdb.sh` ends before its debugging session there. SpiderMonkey's build installs Rust for an aarch64 host. Elsewhere it is the amd64 one, under TCG where the host cannot accelerate it. The machine has the tools above, and its job runs the layer's own scripts and rules. The host builds what the manifest stages from os-real, which the machine cannot build without the cross compilers, and stages the tree as one tar archive: the files git tracks or would track, those products, and the file of the extracted musl that the KRFFS tools compile. The job, [`tooling_job.sh`](tooling_job.sh), runs with a network on two raw disks:

* The build disk, `tooling.disk` in the layer's build tree on the host (`build/vendor/gnu/` or `build/x86_64/vendor/gnu/`): a sparse file of 80 GiB with an ext4 volume, mounted at `/gnu`. It has the tree in `/gnu/rach2`, whose layer's build tree and `vendor/gnu/downloads` stay from job to job, the job's logs in `/gnu/log`, of which it keeps the last 20, and the job's scratch. The machine trims the volume at the end of each job, so the file holds about the blocks the volume uses: some 35 GiB once the layer is built. The volume is mounted without delayed allocation, so that a stamp on it follows the files its step wrote. Its file `machine` has the architecture of the machine that built it, and a job in a machine of another architecture refuses the disk, since the build tree's host programs, such as the cross compilers, run on that architecture alone.
* The image's disk: a new sparse file of the image's size, which the job writes the image to.

The job replaces the rest of the tree on the build disk with the host's, each file with the time the host gave it, runs make for the layer's targets as the account `rach2`, uid 1000, and then `tools/image/mkext2.sh` with `install_root.sh` as the rule for a Linux host does. It copies the image to its disk, and the host renames the file into place once its SHA-256 is the one the job computed in the machine. Each line the job's make writes is printed on the host as it comes, and at the end of the job etude's account of the boot, its `etude provision: cost:` line.

The host's make cannot read the stamps, which are on the build disk, so the image depends on what the stamps depend on: their scripts, recipes and lists of sources, `ports.link`, and the recipe of each package of the agents layer the image has. A change to any of them runs a job, whose make rebuilds what the change touched. A job that stops, by an interrupt, its deadline or a failure, loses the step it was in, as a stopped make on Linux does, and the next make goes on from the stamps on the disk; of the ports' step it loses only the port it was building ([The ports](#the-ports)). A signal that stops make, or the script alone, stops the job, and the script removes the job's files on the host and its lock once the machine has stopped; a job killed outright leaves them, and the next job removes them. One job at a time uses a build disk; a second waits for it.

The environment sets the machine: `ETUDE_TOOLING_ARCH` its architecture, `arm64` on a Mac with Apple silicon and `amd64` elsewhere; `RACH2_QEMU_ACCEL` QEMU's accelerator, `hvf` on a Mac that can accelerate the machine and `tcg` elsewhere; `GNU_TOOLING_CPUS` its processors and make's jobs, 8; `GNU_TOOLING_MIB` its memory, 12288 MiB; `GNU_TOOLING_DISK_GIB` the build disk's size, 80 GiB, which a larger value grows; and `GNU_TOOLING_SECONDS` the job's deadline, four days. `make ARCH=x86_64 os-real-gnu-ext2` builds the x86-64 root. `make GNU_TOOLING_DRY_RUN=1 os-real-gnu-ext2` runs the machine's make with `-n`, which shows what a job would rebuild, and makes no image.

A build disk grows to some 35 GiB as its layer is built, and an image's sparse file takes about what the system in it uses, so the two architectures' disks may not fit beside each other on a host that keeps 40 GiB free, as `RACH2_MIN_FREE_GIB` asks of the loops. Once an architecture's images are made, its build tree on the host, `build/vendor/gnu` or `build/x86_64/vendor/gnu` with the disk in it, may be removed as other idle build output may; the next build of that architecture then starts from nothing.

## Updating

To update a port, verify the new tarball's signature with `gpgv` and the key the header of `sources.txt` names, replace the port's line with the new version, URL and digest, set `VERSION` in its recipe, and run `build_port.sh NAME`. A change to `sources.txt` rebuilds every port under `make gnu-ports`, and a change to a toolchain line (binutils, GCC, GMP, MPFR, MPC, glibc, Linux, libxcrypt) rebuilds the toolchain and everything built with it. A new port is a recipe and a line; `--all` builds every recipe in `ports/`.

## Limits

* The layer's own targets, from `make gnu-toolchain` to `make gnu-agents`, build on an x86-64 Linux host only; elsewhere only the two root images are built, in the tooling machine.
* On a Mac with Apple silicon the layer builds in the arm64 tooling machine at about the speed of the testing host. On a Mac mini with an M4, in the machine's default 8 processors and 12 GiB, `make os-real-gnu-ext2` from an empty build disk took 3140 seconds for i686 and 3184 for x86-64, where the testing host, a Ryzen 7950X3D with 32 threads, takes about 51 minutes; the x86-64 root's agents image took 259 seconds more. The amd64 machine under TCG would take some 7 hours for one architecture: its binutils step took 605 seconds there in a machine of 4 processors, and 35 in the arm64 machine of 8.
* An image the tooling machine makes is the image a Linux host makes from the same tree only as far as the build is reproducible: the machine's Ubuntu, compilers, mke2fs and mandoc are those of the day it was made, and on a Mac with Apple silicon they are aarch64 programs; a program that records the account or the host it was built on records `rach2` and `etude-tooling`.
* There is no PAM: OpenSSH, sudo, shadow and cronie are built without it.
* D-Bus activates no service, since Debian's `dbus` package, which has the activation helper, is not installed.
* chromium-headless-shell and D-Bus are Debian's binaries, not built here.

## Tests

`tests/run gnu` (`ARCH=x86_64 tests/run gnu` for x86-64) builds the root image and runs the cases of [`tests/cases/machine.gnu.json`](../../tests/cases/machine.gnu.json), each in a boot of its own: one logs in over OpenSSH by key and by password, one logs in at the console and runs tmux, and each of the others has a port or program do its main job on the machine, the row of [`tests/system/gnu_rows.sh`](../../tests/system/gnu_rows.sh) that the case names. The ntpd row points `/etc/ntp.conf` at an SNTP server on the host, [`tests/system/sntpserver.py`](../../tests/system/sntpserver.py), whose time begins in 2030, and requires the clock to be set to it. The suite is in the full tier. The `check` steps of the subdirectories' scripts test their programs on the build host. `tests/run host` runs [`tests/system/gnu_cmake_jobs_test.sh`](../../tests/system/gnu_cmake_jobs_test.sh), which requires the cmake port's bootstrap to keep to the jobs that Building gives, on a host that reports more processors, and [`tests/system/gnu_port_resume_test.sh`](../../tests/system/gnu_port_resume_test.sh), which requires a stopped `--all` to go on from the port it stopped in, over stand-in recipes. `ARCH=x86_64 tests/run agents` tests the agents layer ([its Readme](agents/Readme.md#tests)).

The prepared root also contains `/etude-guest`, `/ptydrive` and `/etude-wsclient`, the test runner's framed controller, terminal driver and TLS WebSocket client. The manifest stages the first two; `install_root.sh` compiles the client with the GNU cross compiler against the root's OpenSSL. The source is an image prerequisite, so changing it produces a new image before execution.
