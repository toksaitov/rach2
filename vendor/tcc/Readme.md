# Tiny C Compiler for Rach2

The compiler the machine runs on itself. `tcc` here is a static i386 executable that RUNS on Rach2 and COMPILES for Rach2, built from the pinned tarball by `build_tcc.sh` against the ported musl, and staged out of tree to `build/vendor/tcc/`.

Version 0.9.27, the last release, pinned by sha256.

## Why tinycc and not gcc

Turning C into a running program takes three tools: a compiler, an assembler and a linker. gcc and clang are only the first and hand their output to GNU `as` and `ld`, so porting either means porting binutils too, and then a `make`, and this image has none. The small teaching compilers lose the same way: each emits assembly text for somebody else to assemble.

tcc is all three in one binary. That is the argument, and it is architectural rather than a matter of taste. Memory is a second point: gcc wants hundreds of megabytes to compile one large file, and this machine has 32 MB for everything, kernel included.

## What the build has to arrange

tcc's configure decides what it is building for from the machine it is building on, and this machine is neither the host nor the target. Three things follow, each read out of the tarball rather than guessed.

A cross prefix, always. With an empty one configure calls itself native, and native means it compiles `conftest.c` and RUNS it (`configure:309-316`). Our compiler emits i386 Rach2 binaries, which the build host cannot run. So `build_tcc.sh` writes a small toolchain directory holding `rach2-gcc` (the musl wrapper `vendor/busybox/muslgcc`), `rach2-ar` and `rach2-strip`, and points `--cross-prefix` at it. That also routes every tool through the wrapper, which is why no `--ar` is needed.

A CPU, always. The cross prefix stops the conftest from running; it does not say what to build for. Without `--cpu`, configure takes the target from `uname -m` (`configure:157`), which on an arm64 Mac is arm64.

No Mach-O. On a Mac, configure adds `OSX` to its configuration (`configure:52-56`), which makes the Makefile define `TCC_TARGET_MACHO` (`Makefile:71`) and rename the target to `$(ARCH)-osx` (`Makefile:48`). The build rewrites `TARGETOS` and drops `CONFIG_OSX` in `config.mak`, so the file says what the build does, and then asserts `ARCH=i386` and the absence of the OSX flag rather than hoping.

`--config-musl` is passed, and not for the reason its name suggests. It does select a dynamic loader path, which is nothing to us since we never emit a dynamic executable. What it also does is drop `bcheck.o` from the runtime (`lib/Makefile:31-33`), and `bcheck.c` is written against glibc's malloc hooks, which musl does not have: without the flag the runtime does not build at all. The cost is that `tcc -b`, bounds checking, is not available on this machine.

`libtcc1.a`, tcc's runtime and its answer to libgcc, is normally built by running the tcc that was just built, which here would mean running an i386 Rach2 binary on the build host. `lib/Makefile:23-27` offers `<target>-libtcc1-usegcc=yes`, which builds it with `CC` and `AR` instead. That is what is used, and nothing in this build ever executes the produced binary.

## Where the compiler looks when it runs

The guest's own paths are baked in at configure time, because they are where the compiler searches when it runs on the machine, not where anything lives on the host:

* `--tccdir=/lib/tcc`, which is what sets `CONFIG_TCCDIR`. `--libpaths` does not.
* `--sysincludepaths=/lib/tcc/include:/include`. tcc's own headers come FIRST: musl expects the compiler to provide `stdarg.h` and friends, and tcc with musl's copies ahead of its own is a known way to get a crash instead of an error.
* `--libpaths=/lib` and `--crtprefix=/lib`, where `libc.a` and the startup objects are staged.

## What `make check-tcc` proves

That the artifacts exist and are not empty, the way `check-busybox` does, because a stamp file outlives an artifact somebody deleted.

Then the part `file(1)` cannot do: `tools/check_elf.py` reads the ELF header against what this kernel will actually load. It is two halves on purpose. The LOADER half is `elf_load_image`'s contract copied rule for rule (`rach2/sys/proc/elf.c`): `ELFCLASS32`, `ELFDATA2LSB`, `EV_CURRENT`, `ET_EXEC`, `EM_386`, the exact program-header entry size, at most 32 headers and 8 `PT_LOAD`s, the header table inside a `PT_LOAD` so `AT_PHDR` names something real, `p_filesz <= p_memsz`, ascending page-disjoint non-wrapping segments inside the user window, and an entry point inside it. The POLICY half is this project's and not the kernel's: no `PT_INTERP`, no `PT_DYNAMIC`. The loader does not refuse those, it ignores them, so a dynamic image would load, jump, and run with nothing relocated. That is why the check lives here.

## The output policy

Three of tcc's output modes make something this system cannot run, and `rach2-output-policy.patch` refuses each with a diagnostic that says why:

* `-run` compiles into memory and jumps, which needs `mprotect(2)`, a syscall this kernel does not have.
* `-shared` makes a shared object nothing can load.
* A link without `-static` emits a dynamic executable. That one is the worst, because the kernel does NOT refuse it: `elf_load_image` maps the `PT_LOAD` segments and never looks at `PT_INTERP`, so the program loads, jumps, and runs with nothing relocated.

Compiling to an object (`-c`) and preprocessing (`-E`) are untouched. Neither produces a program to run, and both are how a multi-step build works.

The refusal is in the compiler and not in a wrapper script, deliberately: a wrapper is something a student steps around by typing the real name, and then meets one of the three failures above with nothing to explain it. The patch is applied at build time rather than committed as an edited tree, so the vendored source stays as upstream shipped it and the diff a reader wants is one file here.

It sits at the one place in `main()` where the chosen output type is acted on, and not inside `tcc_set_output_type`, which would look like the tidier single funnel: `-print-search-dirs` calls that function with `TCC_OUTPUT_MEMORY` as a scratch value purely to initialise the search paths (`tcc.c:280`), and refusing there would break a diagnostic that runs no code at all.

A patch that does not apply stops the build, and `make check-tcc` reads the three diagnostics back out of the shipped binary, because a patch that quietly stopped applying and a compiler that accepts `-run` look identical from outside.

## What is deliberately not built

The kernel, musl and dropbear are not compiled in the guest. Each has its own reason and they are recorded in `dev-log/20-compiler/`.
