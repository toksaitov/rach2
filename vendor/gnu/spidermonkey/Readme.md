# The SpiderMonkey shell

This directory builds `js`, the shell of SpiderMonkey, Firefox's JavaScript engine, with its JIT, for the [GNU layer](../Readme.md)'s target. It gives Rach2 a JavaScript engine that compiles to machine code. `make gnu-js` runs `build_js.sh`, which installs the shell as `usr/bin/js` in `js/` of the layer's build tree; `install_root.sh` copies it into the root image.

## Sources and license

[`../sources-extra.txt`](../sources-extra.txt) gives the digests of Firefox's ESR source tarball, the Rust components (`rustc`, `cargo`, the standard library for the x86-64 build host, and the i686 standard library), and the cbindgen crate, and its header says how each digest was verified. cargo fetches cbindgen's dependencies from crates.io and checks each against the crate's `Cargo.lock`; Firefox's own crates are in its source. SpiderMonkey is under the Mozilla Public License 2.0. Rust and cbindgen run only on the build host.

## Local changes

[`python-3.14.patch`](python-3.14.patch) replaces `ast.Str` and the `s` attribute of `ast.Constant`, which Python 3.14 removed, in mach and the moz.build reader, so that mach runs with Python 3.14. The SpiderMonkey sources are not changed.

## Building

[`build_js.sh`](build_js.sh) has five steps, and runs all of them from nothing when none is named. Each writes a log, `log/js-STEP.log`.

1. `rust` installs the Rust components into `rust/` of the build tree, adding the i686 standard library for an i686 target.
2. `cbindgen` builds cbindgen from its crate with `cargo build --locked`. Firefox's configure requires it, though the js project does not use it.
3. `source` unpacks Firefox's source and applies the patch.
4. `build` writes a mozconfig and runs `./mach build`: `--enable-project=js`, `--enable-optimize`, `--disable-debug`, `--disable-tests`, `--disable-jemalloc`, `--without-system-zlib`, `--disable-bootstrap` and `--target=` the toolchain's target, with the cross toolchain for the target and the host's `cc` and `c++` for build tools. `--disable-bootstrap` keeps mach from downloading toolchains, and `--without-system-zlib` builds Firefox's own zlib in, since the sysroot has none. `LLVM_OBJDUMP` is the cross `objdump`, since configure requires one. On i686 the code is compiled with `-msse -msse2 -mfpmath=sse`, as Mozilla builds 32-bit x86. The shell is copied from `dist/bin/js` and stripped.
5. `check` runs a script with the shell through the sysroot's loader and `--ion-offthread-compile=off`. The script calls a function until SpiderMonkey compiles it, then asks the shell's testing functions; the step requires the output line `inJit() = true, inIon() = true`.

mach and cargo run `GNU_JOBS` jobs, or one for each processor, even under make, since mach starts its own make. The object directory is `work/js-obj` and mach's state is in `work/js-state`. The host tools are those the [GNU layer](../Readme.md) lists, with access to crates.io.

## Updating

Replace the `firefox`, `rustc`, `cargo`, `rust-std`, `rust-std-i686` and `cbindgen` lines of `sources-extra.txt` together, with the Rust and cbindgen releases that Mozilla's automation builds that Firefox with (`taskcluster/kinds/toolchain/rust.yml` and `taskcluster/kinds/fetch/toolchains.yml` in its source). Drop or remake the patch if the new source no longer needs it or no longer takes it.

## Limits

The i686 shell needs a processor with SSE2.

## Tests

The `check` step runs with every build. On Rach2, the `js` row of `tests/run gnu` runs a script of the same kind and requires the same line; [`tests/system/gnu_rows.sh`](../../../tests/system/gnu_rows.sh) has it.
