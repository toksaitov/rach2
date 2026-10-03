# TinyCC 0.9.27 integration

Rach2 stages a static TinyCC that runs on the guest and produces objects and executables for it: on i686 Rach2 an i386 compiler, on x86-64 Rach2 an x86-64 one, each the machine's native compiler. TinyCC includes its own assembler and linker, so a guest build needs no GNU binutils.

The tracked integration has six files: `.gitignore`, this Readme, `build_tcc.sh`, `rach2-output-policy.patch`, `rach2-x86_64-static-call.patch`, and `tcc-0.9.27.tar.bz2`. The extracted source, generated cross-tool shims, compiler, runtime archive, headers, and stamp are ignored and reproducible.

## Build

From the repository root:

```
make tcc
make tcc ARCH=x86_64
```

The build is for the architecture the root `make` names in `ARCH`: i686 by default, x86-64 with `make ARCH=x86_64`. The script reads it from `RACH2_ARCH` through `mk/arch.sh`, which gives the cross prefix, `i686-elf-` or `x86_64-elf-`, and the directories. An i686 build extracts here and stages under `build/vendor/tcc/`; an x86-64 build extracts under `build/x86_64/vendor-work/tcc/` and stages under `build/x86_64/vendor/tcc/`. Each has its own stamp, so building one architecture leaves the other's files alone.

The root `make` also builds TinyCC through the same stamp. The stamp depends on the script, both patches, the musl wrapper, `mk/arch.sh`, exact archive, and musl stamp.

The build script:

1. Requires `tcc-0.9.27.tar.bz2` to have SHA-256 `de23af78fca90ce32dff2dd45b3432b2334740bb9bb7b05bf60fdbfc396ceb9c`.
2. Replaces the extracted tree and applies `rach2-output-policy.patch` and `rach2-x86_64-static-call.patch` with `patch --batch --forward`.
3. Creates a `rach2-` cross prefix: `rach2-gcc` invokes the project musl wrapper, while `rach2-ar` and `rach2-strip` invoke the architecture's ELF tools.
4. Configures an i386 or x86_64 musl target with the guest search paths, then rewrites the generated target OS from the macOS build host to Linux and removes `CONFIG_OSX`.
5. Builds the compiler without executing it on the host, and builds `libtcc1.a` with the cross GCC through TinyCC's `i386-libtcc1-usegcc=yes` or `x86_64-libtcc1-usegcc=yes` rule.
6. Adds the required libgcc helper objects to `libtcc1.a`, stages the compiler, runtime, and five compiler headers, and checks their combined link closure.

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

The compiler is a stripped static `ET_EXEC`, ELF32 i386 or ELF64 x86-64. `make check-tcc` requires every staged output, runs the ELF checker selftest and compiler check, and confirms that the four output-policy diagnostics are present in the shipped binary. `make clean` preserves the vendor build; `make distclean` removes it and the extracted tree.

## Cross-build choices

TinyCC's configure script otherwise infers important target properties from the build host. The integration fixes each one explicitly:

* `--cross-prefix` prevents configure from treating the build as native and trying to execute a Rach2 test binary on the host.
* `--cpu=i386` or `--cpu=x86_64` prevents an Apple Silicon host from selecting arm64 output.
* `TARGETOS=Linux` with no `CONFIG_OSX` prevents Mach-O output on macOS.
* `--config-musl` omits the bounds-checking runtime, whose glibc allocation hooks musl does not provide. TinyCC's `-b` mode is therefore unavailable.

The generated `config.mak` and `config.h` are checked after configure. They must contain `ARCH=i386` or `ARCH=x86_64`, as the architecture asks, no `CONFIG_OSX`, and the guest paths described below.

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

[`tools/inspect/elfcheck.c`](../../tools/inspect/elfcheck.c) separately rejects an artifact outside the architecture's loader contract, given by `--arch`, or one containing `PT_INTERP` or `PT_DYNAMIC`. The musl roots have no loader for a dynamic program, and exec refuses a program whose loader it cannot find, so such an artifact could not run where it is staged.

## The x86-64 static call

GNU as since binutils 2.31 writes `R_X86_64_PLT32` for every `call` to a global function, where it wrote `R_X86_64_PC32` before, and musl's `crt1.o` calls `__libc_start_main` that way. TinyCC 0.9.27 gives each such relocation a PLT slot and fills the slots only in a dynamic link, so in a static executable the slot jumped to a displacement never relocated, and every x86-64 program faulted before `main`. `rach2-x86_64-static-call.patch` makes the call to a function the executable defines a plain PC-relative one, as later TinyCC releases do; an undefined symbol keeps its slot, and the static link refuses it as before. The i386 compiler is built from the same source and is unchanged: the code is under `TCC_TARGET_X86_64`.

## Static link closure

Musl was built with GCC and refers to compiler-runtime symbols that the unmodified TinyCC runtime does not export. On i386 they are three: `__udivmoddi4`, `__divmoddi4`, and `__mulxc3`. On x86-64, where a 64-bit division needs no helper, they are the complex multiplications `__mulsc3`, `__muldc3` and `__mulxc3`. The build extracts their object files from the libgcc of the compiler that built musl and adds them to `libtcc1.a`.

`tools/inspect/check_link.sh` reads the staged musl archive, TinyCC runtime, and startup objects, with the architecture's `nm`. It rejects an ordinary guest link with any unresolved strong symbol after accounting for symbols supplied by the program, the linker, and unreachable bounds-checking members.

## Updating the pin

To update TinyCC:

1. Replace the archive with a release tarball from the [TinyCC release directory](https://download.savannah.gnu.org/releases/tinycc/).
2. Change `VERSION` and `TARBALL_SHA256` in `build_tcc.sh`, and change `TCC_TARBALL` in the root Makefile.
3. Rebase both patches, or drop `rach2-x86_64-static-call.patch` where the release already calls a function an executable defines without a PLT slot, and review configure, `config.mak`, the runtime build rule, guest paths, and compiler-runtime helper references.
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
