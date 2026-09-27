# GCC and binutils for Rach2

This directory builds the tools for writing programs on Rach2 itself: binutils 2.47 and GCC 16.2.0 for C and C++. They are compiled on the Linux build host by the cross toolchain of `make gnu-toolchain` (the [GNU layer](../Readme.md)) into programs for i686-linux-gnu, linked dynamically against its glibc 2.44, and installed with their manual pages under the prefix `/usr` into `build/vendor/gnu/native`, a staging root from which the GNU system is assembled. `make gnu-native` runs `build_native.sh`, as `mk/gnu-extra.mk` says; it builds on a Linux host only and is not part of `make`.

## Sources

binutils, GCC, GMP 6.3.0, MPFR 4.2.2 and MPC 1.3.1 are the tarballs of `../sources.txt` that the cross toolchain is built from. `../fetch.sh` checks a tarball's digest every time the script unpacks it.

## binutils and GCC

`build_native.sh` has three steps, `binutils`, `gcc` and `check`, and runs all three from nothing when none is named. Each step writes its output to a log in `build/vendor/gnu/log`.

1. binutils is configured with the build host as the build machine and `i686-linux-gnu` as both host and target, which GCC's manual calls a crossed native build. `--with-sysroot=/` lets `ld` take `--sysroot`, as a native linker configured with one does, and NLS, gprofng, zstd, debuginfod and msgpack are left out. pkg-config is pointed at the sysroot in both steps, so that no library of the build host is found and linked.
2. GCC is configured the same way. The cross compiler compiles its host programs, and its target libraries too (libgcc, libstdc++ and libatomic), against the sysroot given by `--with-build-sysroot`; the installed compiler has no sysroot and looks for headers and libraries in `/usr`, as a native compiler does. The languages, the i686 default and the libraries left out (libgomp, libitm, libquadmath, the sanitizers, libssp and libvtv) are those of the cross compiler. GMP, MPFR and MPC are built in GCC's tree, and GCC links them and its own C++ runtime statically, so that `cc1`, `cc1plus` and the driver need only glibc.
3. Both are installed stripped. The staging root is 284 MB: the driver and the tools in `usr/bin`, the compilers proper in `usr/libexec/gcc/i686-linux-gnu/16.2.0`, the C++ headers in `usr/include/c++/16.2.0`, and in `usr/lib` the `libstdc++.so.6`, `libgcc_s.so.1` and `libatomic.so.1` that the programs the compiler makes load. binutils installs `libbfd`, `libopcodes`, `libctf` and `libsframe` with their headers, as a native binutils does.

The check prints `gcc --version` with the sysroot's loader started by hand. Then it copies the sysroot and the staging root into one tree, the system a GNU program on Rach2 sees, and runs the new compiler there under `unshare(1)`: a user namespace makes the user root, so the `chroot` needs no privilege, and `/lib/ld-linux.so.2` in the tree is the sysroot's loader. `gcc` compiles and links a C program and `g++` a C++ one, and the check requires both to run and print their greeting. The C++ program's interpreter is `/lib/ld-linux.so.2`, and it needs `libstdc++.so.6`, `libm.so.6`, `libgcc_s.so.1` and `libc.so.6`.

## Times

On the testing host, a Ryzen 9 7950X3D under WSL2, `make -j16 gnu-native` took 389 s: binutils 42 s, GCC 342 s and the check 1 s.

## Host

The script needs what `../build_toolchain.sh` needs, and for the check `unshare(1)` from util-linux and a kernel that lets an ordinary user make user namespaces.
