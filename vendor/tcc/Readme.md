# TinyCC 0.9.27 integration

Rach2 stages a static i386 TinyCC that runs on the guest and produces Rach2 i386 objects and executables. TinyCC includes its own assembler and linker, so a guest build needs no GNU binutils.

The tracked integration has five files: `.gitignore`, this Readme, `build_tcc.sh`, `rach2-output-policy.patch`, and `tcc-0.9.27.tar.bz2`. The extracted source, generated cross-tool shims, compiler, runtime archive, headers, and stamp are ignored and reproducible.

## Build

From the repository root:

```
make tcc
```

The root `make` also builds TinyCC through the same stamp. The stamp depends on the script, policy patch, BusyBox musl wrapper, exact archive, and musl stamp.

The build script:

1. Requires `tcc-0.9.27.tar.bz2` to have SHA-256 `de23af78fca90ce32dff2dd45b3432b2334740bb9bb7b05bf60fdbfc396ceb9c`.
2. Replaces the extracted tree and applies `rach2-output-policy.patch` with `patch --batch --forward`.
3. Creates a `rach2-` cross prefix: `rach2-gcc` invokes the project musl wrapper, while `rach2-ar` and `rach2-strip` invoke the i686 ELF tools.
4. Configures an i386 musl target with the guest search paths, then rewrites the generated target OS from the macOS build host to Linux and removes `CONFIG_OSX`.
5. Builds the compiler without executing it on the host, and builds `libtcc1.a` with the cross GCC through TinyCC's `i386-libtcc1-usegcc=yes` rule.
6. Adds three required 32-bit libgcc helper objects to `libtcc1.a`, stages the compiler, runtime, and five compiler headers, and checks their combined link closure.

The staged outputs are:

```
build/vendor/tcc/tcc
build/vendor/tcc/libtcc1.a
build/vendor/tcc/include/float.h
build/vendor/tcc/include/stdarg.h
build/vendor/tcc/include/stdbool.h
build/vendor/tcc/include/stddef.h
build/vendor/tcc/include/varargs.h
```

The current compiler is a stripped static ELF32 i386 `ET_EXEC`. `make check-tcc` requires every staged output, runs the ELF checker selftest and compiler check, and confirms that the four output-policy diagnostics are present in the shipped binary. `make clean` preserves the vendor build; `make distclean` removes it and the extracted tree.

## Cross-build choices

TinyCC's configure script otherwise infers important target properties from the build host. The integration fixes each one explicitly:

* `--cross-prefix` prevents configure from treating the build as native and trying to execute a Rach2 i386 test binary on the host.
* `--cpu=i386` prevents an Apple Silicon host from selecting arm64 output.
* `TARGETOS=Linux` with no `CONFIG_OSX` prevents Mach-O output on macOS.
* `--config-musl` omits the bounds-checking runtime, whose glibc allocation hooks musl does not provide. TinyCC's `-b` mode is therefore unavailable.

The generated `config.mak` and `config.h` are checked after configure. They must contain `ARCH=i386`, no `CONFIG_OSX`, and the guest paths described below.

## Guest toolchain

The os-real, GRUB-test, and metal-disk profiles stage the same compiler bytes as `/bin/tcc` and `/bin/cc`. KRFFS has no symbolic links, so both paths are regular files. The compiler searches:

* `/lib/tcc/include` before `/include` for TinyCC's compiler-provided headers and then musl's headers;
* `/lib/tcc` for `libtcc1.a`;
* `/lib` for `libc.a`, `crt1.o`, `crti.o`, and `crtn.o`.

The profiles also stage pdpmake as `/bin/make` and the project sources under `/src`. A direct guest link must request a static executable:

```
tcc -static -o hello hello.c
```

Preprocessing with `-E` and object compilation with `-c` remain available for multi-step builds.

## Output policy

`rach2-output-policy.patch` rejects four output kinds that the system cannot execute:

* `-run`, because code run from memory finds the C library's functions through `dlsym`, which in this static TinyCC finds none;
* `-shared`, because musl is here only as the static `libc.a`, and nothing loads a shared object built against it;
* an executable link without `-static`, because a dynamic program asks for a loader, and there is no musl loader;
* `-Wl,-oformat` for a non-ELF image, because `exec` requires an ELF program-header table.

The checks run in TinyCC's `main` after command-line processing. They do not change `tcc_set_output_type`, because `-print-search-dirs` calls that function with an in-memory output type while initializing search paths.

[`tools/inspect/elfcheck.c`](../../tools/inspect/elfcheck.c) separately rejects an artifact outside the kernel's ELF32 i386 loader contract or one containing `PT_INTERP` or `PT_DYNAMIC`. The musl roots have no loader for a dynamic program, and exec refuses a program whose loader it cannot find, so such an artifact could not run where it is staged.

## Static link closure

Musl was built with GCC and refers to three compiler-runtime symbols that the unmodified TinyCC runtime does not export: `__udivmoddi4`, `__divmoddi4`, and `__mulxc3`. The build extracts their object files from the same 32-bit libgcc used by the musl toolchain and adds them to `libtcc1.a`.

`tools/inspect/check_link.sh` reads the staged musl archive, TinyCC runtime, and startup objects. It rejects an ordinary guest link with any unresolved strong symbol after accounting for symbols supplied by the program, the linker, and unreachable bounds-checking members.

## Updating the pin

To update TinyCC:

1. Replace the archive with a release tarball from the [TinyCC release directory](https://download.savannah.gnu.org/releases/tinycc/).
2. Change `VERSION` and `TARBALL_SHA256` in `build_tcc.sh`, and change `TCC_TARBALL` in the root Makefile.
3. Rebase `rach2-output-policy.patch` and review configure, `config.mak`, the runtime build rule, guest paths, and compiler-runtime helper references.
4. Rebuild and inspect every staged artifact before running the verification below.

## Verification

```
make tcc
make check-tcc
make
tests/run style
tests/run host
tests/run tcc
tests/run make
tests/run selfbuild
```

`tests/run tcc` compiles and runs a static program, proves all four policy refusals and their diagnostics, checks object compilation, builds and compares the project `cat`, links the two-source `ping`, and fails if the compiler requests an unimplemented syscall.

`tests/run make` exercises the staged compiler through pdpmake across an incremental build and reboot. `tests/run selfbuild` builds the kernel twice, utilities, and ISH in the guest; checks the userland results; extracts and checks the produced kernel; runs the full default probe suite with it; and rebuilds the staged ISH tree in place. Its GRUB boot is skipped with an explicit reason when Docker is unavailable.
