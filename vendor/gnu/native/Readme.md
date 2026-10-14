# GCC, binutils and GDB for Rach2

This directory builds the tools for compiling and debugging programs on Rach2 itself: binutils and GCC for C and C++, and GDB with gdbserver. The cross toolchain of the [GNU layer](../Readme.md) compiles them on the build host into programs for its target, linked against its glibc. `make gnu-native` runs `build_native.sh`, which installs into `native/` of the layer's build tree, and `make gnu-gdb` runs `build_gdb.sh`, which installs into `gdb/`; both install under the prefix `/usr`, and `install_root.sh` copies both trees into the root image.

## Sources and license

binutils, GCC, GMP, MPFR and MPC are the tarballs the cross toolchain is built from; GDB and expat are pinned with them in [`../sources.txt`](../sources.txt), and ncurses is the port's tarball. binutils, GCC and GDB are under the GNU General Public License, version 3 or later; GCC's runtime libraries add the GCC Runtime Library Exception. GMP, MPFR and MPC are under the GNU Lesser General Public License, and expat and ncurses under MIT-style licenses.

## Local changes

`build_native.sh` points the `m64` line of GCC's `gcc/config/i386/t-linux64` at `lib`, as the cross toolchain does, so the x86-64 compiler finds its libraries in `/lib` and `/usr/lib`. Nothing else in the sources is changed.

## Building

Each script runs its steps from nothing when none is named, and only the named steps otherwise. Each step writes a log, `log/native-STEP.log` or `log/gdb-STEP.log`. Both are crossed native builds: the build machine is the host, and the host and target are the toolchain's target. pkg-config reads only the sysroot.

[`build_native.sh`](build_native.sh) has the steps `binutils`, `gcc` and `check`.

* binutils is configured with `--with-sysroot=/`, so that `ld` takes `--sysroot`, and without NLS, gprofng, zstd, debuginfod and msgpack.
* GCC is configured for the languages, instruction set and libraries of the cross compiler, with `--with-build-sysroot` naming the sysroot. The installed compiler has no sysroot and searches `/usr`. GMP, MPFR and MPC are built in GCC's tree. Unlike the cross compiler, it builds libgomp and the sanitizer runtimes; libitm, libquadmath, libssp and libvtv are left out.
* Both are installed with `install-strip`.
* `check` prints `gcc --version` through the sysroot's loader, then copies the sysroot and the staging root into one tree and, under `unshare --map-root-user --root`, compiles and runs a C program with `gcc` and a C++ program with `g++`. Both must print their greeting.

[`build_gdb.sh`](build_gdb.sh) has the steps `gmp`, `mpfr`, `expat`, `ncurses`, `gdb` and `check`.

* GMP, MPFR, expat and ncurses are built as static libraries in `work/gdb-deps`, so that GDB needs only glibc and the C++ runtime on Rach2. GMP is compiled with `-std=gnu17`, since its configure tests use C that GCC 16's default C23 rejects. ncurses has wide characters and reads `/usr/share/terminfo`, where the ncurses port installs the database.
* GDB is configured with GMP, MPFR, expat (for `catch syscall` by name) and the text user interface (`--enable-tui`), and without Python, Guile, the compressors, xxhash, debuginfod, babeltrace, Intel PT, source highlighting, the simulators and NLS. Only `gdb` and `gdbserver` are installed.
* `check` prints the versions of `gdb` and `gdbserver` through the sysroot's loader and requires `--enable-tui` in GDB's configuration. Then, under `unshare` with new user, mount and PID namespaces and a `/proc` of their own, GDB runs a program the cross compiler built, stops at a breakpoint, prints a backtrace, steps, prints a variable, and lets the program exit normally; each must appear in GDB's output.

The host tools are those the [GNU layer](../Readme.md) lists, with `unshare`.

## Updating

GCC, binutils, GMP, MPFR and MPC follow the toolchain's lines in `sources.txt`, so updating the toolchain updates them here. GDB and expat are updated by their own lines. A change to `sources.txt` rebuilds both trees.

## Tests

The `check` steps run with every build. On Rach2, the `gcc` and `gdb` rows of `tests/run gnu` compile, run and debug programs; [`tests/system/Readme.md`](../../../tests/system/Readme.md) describes them.
