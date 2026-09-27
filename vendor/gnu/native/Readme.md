# GCC, binutils and GDB for Rach2

This directory builds the tools for writing and debugging programs on Rach2 itself: binutils 2.47 and GCC 16.2.0 for C and C++, and GDB 18.1 with gdbserver. Each is compiled on the Linux build host by the cross toolchain of `make gnu-toolchain` (the [GNU layer](../Readme.md)) into programs for i686-linux-gnu, linked dynamically against its glibc 2.44, and installed with its manual pages under the prefix `/usr` into a staging root under `build/vendor/gnu/`, from which the GNU system is assembled: `native/` for binutils and GCC, `gdb/` for GDB. `make gnu-native` runs `build_native.sh` and `make gnu-gdb` runs `build_gdb.sh`, as `mk/gnu-extra.mk` says; both build on a Linux host only and are not part of `make`.

## Sources

binutils, GCC, GMP 6.3.0, MPFR 4.2.2 and MPC 1.3.1 are the tarballs of `../sources.txt` that the cross toolchain is built from. GDB 18.1 is pinned in `../sources-extra.txt`; its SHA-256 was recorded after `gpgv` found a good signature on the tarball by Joel Brobecker's key, `F40ADB902B24264AA42E50BF92EDB04BFF325CF3`, in the GNU keyring. `../fetch.sh` checks a tarball's digest every time a script unpacks it.

## binutils and GCC

`build_native.sh` has three steps, `binutils`, `gcc` and `check`, and runs all three from nothing when none is named. Each step writes its output to a log in `build/vendor/gnu/log`.

1. binutils is configured with the build host as the build machine and `i686-linux-gnu` as both host and target, which GCC's manual calls a crossed native build. `--with-sysroot=/` lets `ld` take `--sysroot`, as a native linker configured with one does, and NLS, gprofng, zstd, debuginfod and msgpack are left out. pkg-config is pointed at the sysroot in both steps, so that no library of the build host is found and linked.
2. GCC is configured the same way. The cross compiler compiles its host programs, and its target libraries too (libgcc, libstdc++ and libatomic), against the sysroot given by `--with-build-sysroot`; the installed compiler has no sysroot and looks for headers and libraries in `/usr`, as a native compiler does. The languages, the i686 default and the libraries left out (libgomp, libitm, libquadmath, the sanitizers, libssp and libvtv) are those of the cross compiler. GMP, MPFR and MPC are built in GCC's tree, and GCC links them and its own C++ runtime statically, so that `cc1`, `cc1plus` and the driver need only glibc.
3. Both are installed stripped. The staging root is 284 MB: the driver and the tools in `usr/bin`, the compilers proper in `usr/libexec/gcc/i686-linux-gnu/16.2.0`, the C++ headers in `usr/include/c++/16.2.0`, and in `usr/lib` the `libstdc++.so.6`, `libgcc_s.so.1` and `libatomic.so.1` that the programs the compiler makes load. binutils installs `libbfd`, `libopcodes`, `libctf` and `libsframe` with their headers, as a native binutils does.

The check prints `gcc --version` with the sysroot's loader started by hand. Then it copies the sysroot and the staging root into one tree, the system a GNU program on Rach2 sees, and runs the new compiler there under `unshare(1)`: a user namespace makes the user root, so the `chroot` needs no privilege, and `/lib/ld-linux.so.2` in the tree is the sysroot's loader. `gcc` compiles and links a C program and `g++` a C++ one, and the check requires both to run and print their greeting. The C++ program's interpreter is `/lib/ld-linux.so.2`, and it needs `libstdc++.so.6`, `libm.so.6`, `libgcc_s.so.1` and `libc.so.6`.

## GDB and gdbserver

`build_gdb.sh` has four steps, `gmp`, `mpfr`, `gdb` and `check`, and runs all four from nothing when none is named.

1. GMP and MPFR, which GDB needs for its arithmetic, are built for i686-linux-gnu as static libraries in `build/vendor/gnu/work/gdb-deps`, so that GDB links them in. GMP's configure tests the compiler with C written before prototypes, which GCC 16 rejects in its default C23, where `f()` declares a function of no arguments, so GMP is configured with `-std=gnu17`.
2. GDB is configured as a crossed native build too, and finds GMP and MPFR through `--with-gmp` and `--with-mpfr`, which the top-level configure reads, and `--with-libgmp-prefix` and `--with-libmpfr-prefix`, which gdb's own reads. Python, Guile, expat, liblzma, zstd, xxhash, debuginfod, babeltrace, Intel PT, source-highlight, the simulators and NLS are left out. With no curses library in the sysroot, readline uses GDB's termcap stub, and the text user interface is left out. Without expat, GDB cannot read the XML target descriptions and library lists that a remote stub sends, and uses its built-in i386 description instead; native debugging does not need them.
3. Only gdb and gdbserver are installed, stripped, since the tarball has its own copies of bfd and opcodes: `gdb`, `gdbserver`, `gcore`, `gstack`, `gdb-add-index`, `libinproctrace.so`, the JIT reader's header, the system call tables GDB reads, and the manual and info pages. The staging root is 15 MB. `gdb` needs `libstdc++.so.6`, `libgcc_s.so.1` and `libatomic.so.1` beside glibc, which the GCC staging root has.

The check prints the versions of `gdb` and `gdbserver` started with the sysroot's loader by hand. Then, in a tree of the sysroot, the staging root and the cross toolchain's C++ runtime, under `unshare(1)` with new user, mount and process namespaces and a `/proc` of their own, `gdb` debugs a program the cross compiler built with debugging information: it stops at a breakpoint, prints a backtrace, steps a line, prints a variable, and lets the program run to its end, and the check requires each of these in GDB's output. On the testing host, whose WSL2 kernel is 64-bit, glibc's `libthread_db` loads but its first request fails, since `PTRACE_GET_THREAD_AREA` returns `EINVAL` to the 32-bit GDB, so GDB debugs there without thread support.

## Times

On the testing host, a Ryzen 9 7950X3D under WSL2, `make -j16 gnu-native` took 389 s: binutils 42 s, GCC 342 s and the check 1 s. `make -j16 gnu-gdb` took 141 s: GMP 19 s, MPFR 9 s, GDB 112 s and the check 1 s.

## Host

The scripts need what `../build_toolchain.sh` needs, and for the checks `unshare(1)` from util-linux and a kernel that lets an ordinary user make user namespaces.
