# The GNU layer

This directory builds the GNU system of milestone 54 (`dev-log/54-gnu/plan.md`): an i686-linux-gnu cross toolchain, and with it the programs of a root that the GNU system is installed from. The layer builds on a Linux host only and is not part of `make` or `make all`; `make gnu-toolchain` and `make gnu-ports` build it (`mk/gnu.mk`).

## Sources

`sources.txt` lists every tarball of the layer with its version, URL and SHA-256, and its header says how each digest was verified: with gpgv, against an upstream signature that covers the release. For most projects that is the release's own signature; for Perl it is the signed CPAN checksum file of the uploader's directory, and for Vim, whose tag archive is not signed, the signed git tag. The tarballs are not committed: GCC's is larger than GitHub accepts for one file, and together they are about twice the rest of the repository. `fetch.sh` downloads a tarball into `downloads/`, which Git ignores, once, and checks its digest every time it is used; a file with another digest is removed and refused. `fetch.sh` with no arguments fetches everything, and `fetch.sh --path NAME` prints one tarball's path. GDB 18.1 is pinned for the phase that builds it.

## The toolchain

`make gnu-toolchain` runs `build_toolchain.sh`, which builds into `build/vendor/gnu/`:

* `cross/`: binutils 2.47 and GCC 16.2.0 for C and C++ (with GMP 6.3.0, MPFR 4.2.2 and MPC 1.3.1 built in GCC's tree), as `i686-linux-gnu-gcc` and the rest, configured for the i686 instruction set.
* `sysroot/`: the target's system: the Linux 6.18.54 headers, glibc 2.44 with the dynamic loader in `lib/`, and libxcrypt 4.5.2, which provides the `crypt` glibc 2.44 no longer has.

The order is binutils, the headers, a first GCC for C without a C library, glibc, the final GCC, and libxcrypt. glibc keeps its default minimum kernel, 3.2.0, so it falls back from the newer system calls to the older ones Rach2 has. The script takes step names (`binutils`, `headers`, `gcc1`, `glibc`, `gcc2`, `libxcrypt`) to run only those over an earlier build; with none, it starts from nothing. Under make its sub-makes share the caller's jobs, so `make -j32 gnu-toolchain` built it in ten minutes on the testing host.

The host needs a C and C++ compiler, GNU make 4, gawk, bison, flex, m4, Perl, Python 3, texinfo, xz, patch, and curl or wget. A pkg-config, where the host has one, is used by the ports' configure scripts and reads only the root's files; none of the ports needs it.

## The runtime

The first port in the root, `build/vendor/gnu/root`, is the runtime, `ports/runtime.sh`, which has no tarball. It installs the toolchain's sysroot: glibc 2.44 with its dynamic loader as `/lib/ld-linux.so.2` and its shared libraries in `/lib`, as an i386 GNU system without multiarch directories has them; glibc's headers, start files and static libraries and the Linux headers in `/usr`; and libxcrypt's `libcrypt.so.1` in `/usr/lib`. To these it adds GCC's shared libraries, `libgcc_s.so.1` in `/lib` and `libstdc++.so.6` in `/usr/lib`, and a locale archive, `/usr/lib/locale/locale-archive`, with `C.UTF-8` and `en_US.UTF-8`, which the target's own `localedef` writes, run on the build host through the staged loader. libxcrypt's soname is `libcrypt.so.1` because the toolchain builds it with the glibc-compatible interface (`--enable-obsolete-api=glibc`); Debian's i386 packages ask for the same name. With the headers and libraries in it, the root is a sysroot as well as a system.

## The ports

`build_port.sh NAME...` builds ports with the toolchain; `build_port.sh --all`, which `make gnu-ports` runs, builds every recipe into an empty root. Each port has a recipe in `ports/`: a shell fragment that sets its version, the ports it depends on, its configure arguments, the configure answers a cross build cannot find out for itself, and the pinned patches applied after the extract. A port that autoconf does not configure has its own build and install steps, and its recipe's comment says how and why.

* The compilers run through wrappers in `build/vendor/gnu/wrap/` that give them the root as their sysroot, so a port finds the headers and libraries of the ports before it where the target has them.
* A port's dependencies are built first when the root does not have them, and every port depends on the runtime.
* A port installs into `build/vendor/gnu/stage/NAME`. There the target's `strip` removes the debugging information from its ELF files, which keep their symbol tables, and its libtool archives and Info index are removed; the rest is copied into the root and listed in `stage/NAME.files`.
* Programs link with the shared glibc and ask for `/lib/ld-linux.so.2`. `GNU_LINK=static` links the ports that autoconf configures statically instead.

A program in the root runs on the build host through the root's own loader, which loads the root's glibc rather than the host's i386 one:

    R=build/vendor/gnu/root
    $R/lib/ld-linux.so.2 --library-path $R/lib:$R/usr/lib $R/usr/bin/git --version

The table gives each port's version, the time it took under `make -j32 gnu-ports` on the testing host, and the size of what it installs after stripping.

| Port      | Version    | Time  | Size    |
| --------- | ---------- | ----- | ------- |
| runtime   | glibc 2.44 | 1 s   | 65.0 MB |
| zlib      | 1.3.2      | 1 s   | 0.4 MB  |
| OpenSSL   | 4.0.2      | 32 s  | 31.5 MB |
| OpenSSH   | 10.5p1     | 31 s  | 10.2 MB |
| ncurses   | 6.6        | 46 s  | 11.1 MB |
| less      | 710        | 4 s   | 0.5 MB  |
| Vim       | 9.2.1132   | 18 s  | 38.0 MB |
| coreutils | 9.12       | 46 s  | 12.4 MB |
| bash      | 5.3        | 24 s  | 5.8 MB  |
| make      | 4.4.1      | 7 s   | 0.9 MB  |

These notes say what some ports provide and what they leave to the system they are installed on.

* OpenSSL is built for `linux-x86`, shared, with zlib. Its configuration is in `/etc/ssl`, where the programs that use it look for certificates; the root has none.
* OpenSSH installs `ssh`, `scp`, `sftp`, `ssh-keygen` and the other clients in `/usr/bin`, `sshd` in `/usr/sbin`, and `sshd-session`, `sshd-auth`, `sftp-server` and the helpers in `/usr/libexec`, with its configuration in `/etc/ssh` and no PAM. The server's privilege-separated child runs as the user `sshd` in `/var/empty`, so the system needs that user and a `/var/empty` owned by root. The host keys are made on the system.
* ncurses has wide characters and installs the terminfo database, 1861 entries, in `/usr/share/terminfo`.
* Vim has its huge feature set, for the terminal only.
* coreutils and make start their children with `vfork` (gnulib's `posix_spawn`, and make's jobs) until `clone` can share an address space.
