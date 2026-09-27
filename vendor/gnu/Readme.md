# The GNU layer

This directory builds the GNU system of milestone 54 (`dev-log/54-gnu/plan.md`): an i686-linux-gnu cross toolchain first, and the programs it builds after. The layer builds on a Linux host only and is not part of `make` or `make all`.

## Sources

`sources.txt` lists every tarball of the layer with its version, URL and SHA-256. Each digest was recorded after the upstream signature verified. The tarballs are not committed: GCC's is larger than GitHub accepts for one file, and together they are about twice the rest of the repository. `fetch.sh` downloads a tarball into `downloads/`, which Git ignores, once, and checks its digest every time it is used; a file with another digest is removed and refused. `fetch.sh` with no arguments fetches everything, and `fetch.sh --path NAME` prints one tarball's path.

## The toolchain

`make gnu-toolchain` runs `build_toolchain.sh`, which builds into `build/vendor/gnu/`:

* `cross/`: binutils 2.47 and GCC 16.2.0 for C and C++ (with GMP 6.3.0, MPFR 4.2.2 and MPC 1.3.1 built in GCC's tree), as `i686-linux-gnu-gcc` and the rest, configured for the i686 instruction set.
* `sysroot/`: the target's system: the Linux 6.18.54 headers, glibc 2.44 with the dynamic loader in `lib/`, and libxcrypt 4.5.2, which provides the `crypt` glibc 2.44 no longer has.

The order is binutils, the headers, a first GCC for C without a C library, glibc, the final GCC, and libxcrypt. glibc keeps its default minimum kernel, 3.2.0, so it falls back from the newer system calls to the older ones Rach2 has. The script takes step names (`binutils`, `headers`, `gcc1`, `glibc`, `gcc2`, `libxcrypt`) to run only those over an earlier build; with none, it starts from nothing. Under make its sub-makes share the caller's jobs, so `make -j32 gnu-toolchain` built it in ten minutes on the testing host.

The host needs a C and C++ compiler, GNU make 4, gawk, bison, flex, m4, Python 3, texinfo, xz, patch, and curl or wget.

## The ports

`build_port.sh NAME...` builds programs with that toolchain. Each program has a recipe in `ports/`, a shell fragment that sets its version, its configure arguments, the configure answers a cross build cannot find out for itself, and the pinned patches applied after the extract. The program is configured for i686-linux-gnu with `--prefix=/usr`, compiled with the toolchain's GCC named by its full path, and installed with its manual pages into `build/vendor/gnu/root`, the tree the GNU system is staged from. It is linked statically unless `GNU_LINK=shared`, since the kernel loads no shared libraries yet. The recipes so far are coreutils 9.12, GNU make 4.4.1 and bash 5.3 with its twenty official patches.
