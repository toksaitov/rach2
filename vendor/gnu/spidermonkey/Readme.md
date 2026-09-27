# The SpiderMonkey shell

This directory builds `js`, the shell of SpiderMonkey, Firefox's JavaScript engine, from the source of Firefox 140.16.0esr, the release of Firefox ESR 140 current on 2026-09-27, as a program for i686-linux-gnu with its JIT. `make gnu-js` runs `build_js.sh`, as `mk/gnu-extra.mk` says, and the script installs the shell under the prefix `/usr` into `build/vendor/gnu/js`, a staging root. It builds on a Linux host only and is not part of `make`.

## Sources

`../sources-extra.txt` lists three kinds of source with their digests, and `../fetch.sh` checks each one's digest every time the script unpacks it.

* Firefox's source, `firefox-140.16.0esr.source.tar.xz`. Its digest was recorded after its signature, and the signed `SHA256SUMS` beside it, verified with Mozilla's release key, `14F26682D0916CDD81E37B6D61B7B526D98F0353`.
* Rust 1.86.0, the release Mozilla's automation builds Firefox 140 with (`taskcluster/kinds/toolchain/rust.yml` in the tree): `rustc`, `cargo` and the standard library for the build host, and the standard library for `i686-unknown-linux-gnu`. Each was verified with the Rust release key, `108F66205EAEB0AAA8DD5E1C85AB96E6FA1BE5FE`.
* The crate of cbindgen 0.28.0. It has no signature; its digest is the one the crates.io index lists, and its `.cargo_vcs_info.json` has the git revision `bd78bbe59b10eda6ef1255e4acda95c56c6d0279`, the one Mozilla's automation builds cbindgen from, as `taskcluster/kinds/fetch/toolchains.yml` in Firefox's signed source gives it. The crate's `Cargo.lock` lists its dependencies with their digests, and cargo fetches them from crates.io with `--locked` and checks each one.

Firefox's own Rust crates are in its source, under `third_party/rust`.

## The build

`build_js.sh` has five steps, `rust`, `cbindgen`, `source`, `build` and `check`, and runs all five from nothing when none is named. Each step writes its output to a log in `build/vendor/gnu/log`.

1. `rust` installs the four Rust components with their own `install.sh` into `build/vendor/gnu/rust`, a toolchain of the build's own that no `rustup` manages.
2. `cbindgen` builds cbindgen from its crate into the same directory. Firefox's configure requires cbindgen for every project, though the js project makes no header with it.
3. `source` unpacks Firefox's source and applies `python-3.14.patch`. Firefox 140's mach and moz.build reader use `ast.Str` and the `s` attribute of `ast.Constant`, which Python 3.14, the Python of the testing host, removed; the patch uses `ast.Constant` and its `value`. mach warns that Python 3.14 is newer than any it is known to work with, and the build succeeds.
4. `build` writes a mozconfig and runs `./mach build`, as Mozilla's instructions for building SpiderMonkey say: `--enable-project=js`, `--enable-optimize`, `--disable-debug`, `--disable-tests`, `--disable-jemalloc`, `--without-system-zlib`, `--disable-bootstrap` and `--target=i686-linux-gnu`, with the cross toolchain's `gcc`, `g++`, `ar`, `nm`, `ranlib` and `strip` for the target and the host's own compilers for the build's tools. `--disable-bootstrap` keeps mach from downloading toolchains of its own. `--without-system-zlib` builds the zlib of Firefox's tree into the shell, since SpiderMonkey alone takes zlib from the system by default and the sysroot has none. The code is compiled with `-msse -msse2 -mfpmath=sse`, as Mozilla's automation compiles 32-bit x86, so that the interpreter and the JIT round floating point alike; the shell needs a processor with SSE2. configure requires `llvm-objdump`, which only the browser's packaging runs and which the build host lacks, so `LLVM_OBJDUMP` is set to the cross toolchain's `objdump`. mach keeps its state and its virtual environments in `build/vendor/gnu/work/js-state` and installs no Python packages from outside the tree.
5. The shell is copied from the object directory's `dist/bin/js` into the staging root and stripped: a position-independent executable of 43.7 MB, whose interpreter is `/lib/ld-linux.so.2` and which needs `libstdc++.so.6`, `libgcc_s.so.1`, `libm.so.6` and `libc.so.6`.

The check runs a script with the shell, started with the sysroot's loader by hand and the cross toolchain's C++ runtime on the library path. The script calls a function in a loop until SpiderMonkey compiles it, then asks the shell's testing functions whether the function runs in JIT code and in Ion, SpiderMonkey's optimizing compiler, and the check requires both. On the testing host the script prints:

```
sum(1000) = 499500
baseline.enable = 1, ion.enable = 1
inJit() = true, inIon() = true
```

## Times

On the testing host, a Ryzen 9 7950X3D under WSL2, `make gnu-js` with `GNU_JOBS=16` took 201 s: the Rust install 10 s, cbindgen 18 s, unpacking and patching the source 24 s, the build 143 s, and the check under a second. The Rust toolchain in `build/vendor/gnu/rust` takes 705 MB, the unpacked source 4.1 GB and the object directory, `build/vendor/gnu/work/js-obj`, 2.6 GB; the source and the object directory stay after the build.

## Host

Python 3, GNU make, m4, patch, xz and a C and C++ compiler for the build host, and access to crates.io for cbindgen's dependencies. mach and cargo run `GNU_JOBS` jobs, or one for every processor when it is not set, under make as well, since mach starts its own make.
