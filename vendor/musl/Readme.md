# musl 1.2.5 integration

Rach2 links its hosted user programs against a static build of [musl libc](https://musl.libc.org/) for each architecture it builds: i386 for `ARCH=i686`, the default, and x86-64 for `ARCH=x86_64`. Musl supplies the C library, startup objects, headers, and the Linux system call ABI of the architecture used by the utilities, tests, and larger ports.

This directory contains the project-owned integration files and the pinned upstream archive. The tracked inputs are `.gitignore`, this Readme, `build_libc.sh`, `muslgcc`, `musl-1.2.5.tar.gz`, and `patches/0001-posix-spawn-forks.patch`. The extracted source, installed files, and build stamp are ignored and reproducible.

The i686 build extracts, installs and stamps here, under `musl-1.2.5/`, `install/` and `.stamp`. The x86-64 build does the same under `build/x86_64/vendor-work/musl/`, so neither architecture's build touches the other's files. `mk/arch.sh` names the directories, the cross prefix and the configure target of each architecture.

## Build

From the repository root:

```
make musl
make musl ARCH=x86_64
```

The ordinary root `make` builds musl before everything that links it. `make busybox`, `make dropbear`, `make tcc`, and `make pdpmake` build the other four ports individually.

The root stamp depends on `build_libc.sh`, the exact archive, the patch manifest `patches/series`, and every patch currently under `patches/`. The script applies exactly the patches the series names, in its order, and fails on a named patch that is absent or a patch file the series does not name, so adding, editing, or removing a patch always invalidates the stamp through a tracked input. The stamp also depends on `mk/arch.sh`. The recipe removes the old installed archive, invokes the script with `RACH2_ARCH` set to the build's `ARCH`, requires a nonempty `install/lib/libc.a`, and records success only afterward. `make clean` preserves the extracted and installed trees; `make distclean` removes them and the stamp.

`build_libc.sh` performs these steps:

1. Require `musl-1.2.5.tar.gz` to have SHA-256 `a9a118bbe84d8764da0ea0d28b3ab3fae8477fc7e4085d90102b8596fc7c75e4`.
2. Replace the extracted `musl-1.2.5/` tree and apply `patches/*.patch` in name order with no fuzz.
3. Clear inherited compiler and make overrides. Configure with `CROSS_COMPILE=i686-elf-` and `--target=i686`, or `CROSS_COMPILE=x86_64-elf-` and `--target=x86_64`, with `--disable-shared` and the absolute `install/` prefix.
4. Build in parallel, replace the old install tree, and run musl's install target.

The compiler is deliberately fixed at the architecture's `i686-elf-gcc` or `x86_64-elf-gcc`; a caller's `CC`, `CROSS_CC`, `ARCH` or make flags cannot change this vendor build. The generated configuration selects musl's i386 or x86_64 architecture and static libraries.

The integration consumes:

* `install/include/`: musl headers.
* `install/lib/libc.a`: the static C library.
* `install/lib/crt1.o`, `crti.o`, and `crtn.o`: process startup and init/fini objects.
* `musl-1.2.5/src/crypt/crypt_sha512.c`: the pinned SHA-512 crypt implementation compiled into the KRFFS host credential tools and their host tests.

The root `check-libc` target checks both `libc.a` and the extracted crypt source. If either is missing while `.stamp` remains, remove the stamp and run `make musl`.

## Project patch

The build applies one source patch. `0001-posix-spawn-forks.patch` changes musl's `posix_spawn` child creation from `clone(CLONE_VM|CLONE_VFORK|SIGCHLD)` to the raw `fork` syscall, so the child runs in a copy of the parent's memory rather than in the parent's own.

The kernel implements `clone` with those flags, which GNU libc's `posix_spawn` uses; musl's spawn does not need them. A forked spawn child uses the existing implementation because it either execs or writes its errno through the existing pipe and exits. The raw syscall also preserves musl's negative-errno path and does not run `pthread_atfork` handlers.

The patch serves `posix_spawn`, `system`, and `popen`; pdpmake uses the latter interfaces to run recipes. The default `spawnmusl` probe checks command status, pipe output, spawn errors, child cleanup, parent memory, and repeated calls.

Any additional project patch belongs under `patches/` with a numbered descriptive name. Rebase and explain a patch when the pinned release changes. Do not edit the extracted source as the maintained copy.

## Static link contract

A direct i686 link from the repository root has this order:

```
i686-elf-gcc -m32 -static -nostdlib -nostartfiles \
  -isystem vendor/musl/install/include \
  -o prog \
  vendor/musl/install/lib/crt1.o \
  vendor/musl/install/lib/crti.o \
  prog.c \
  vendor/musl/install/lib/libc.a \
  $(i686-elf-gcc -m32 -print-libgcc-file-name) \
  vendor/musl/install/lib/crtn.o \
  -Wl,-z,noexecstack
```

`crt1.o` and `crti.o` precede program objects. `libc.a` and 32-bit libgcc follow them, and `crtn.o` closes the init/fini sections. The result is a static, non-PIE, little-endian ELF32 `ET_EXEC` for i386 with no `PT_INTERP`. The current linker places its first load at `0x08048000`; the exact entry address depends on the program.

An x86-64 link has the same order with `x86_64-elf-gcc -m64`, the objects and archive of `build/x86_64/vendor-work/musl/install`, the compiler's 64-bit libgcc, and `-Wl,-z,max-page-size=0x1000`, which aligns segments to 4 KiB pages where the x86-64 linker would use 2 MiB. The result is a static, non-PIE ELF64 `ET_EXEC` for x86-64 whose first load is at `0x400000`.

The utilities use this order directly, and so does `tests/musl/build.sh`, for i686 alone. The ports build through `muslgcc`, one wrapper for all five, which reads the architecture from `RACH2_ARCH` through `mk/arch.sh`, calls that architecture's compiler with `-m32` or `-m64`, adds only headers during compilation, omits startup and libraries from partial links, and adds the complete sequence at the final link. `MUSLGCC_DEFINES`, set by a port's build script, is added to its compiles and final links; e2fsprogs sets `-D__rach2__` there.

## Process ABI

This section is the i686 kernel's contract; the x86-64 kernel's follows Linux's x86-64 ABI, with `syscall` in place of `int 0x80`.

The kernel gives a new process one System V i386 entry stack: `argc`, the `argv` pointers and null, the `envp` pointers and null, an auxiliary vector ending with `AT_NULL`, then the referenced strings and 16 random bytes. Entry ESP is aligned to 16 bytes.

Rach2 supplies `AT_PHDR`, `AT_PHENT`, `AT_PHNUM`, `AT_ENTRY`, `AT_PAGESZ`, `AT_CLKTCK`, `AT_SECURE`, `AT_RANDOM`, `AT_BASE`, the four ids, `AT_EXECFN` and `AT_MINSIGSTKSZ`, the least alternate stack a signal frame fits, which `sysconf(_SC_MINSIGSTKSZ)` returns when it is above `MINSIGSTKSZ`. `AT_SECURE` is one when an effective uid or gid differs from its real id. `AT_RANDOM` gives musl the bytes used for its stack protector and allocator state; the [randomness document](../../rach2/sys/lib/krandom.Readme.md) defines their trust policy.

Musl's i386 `_start` preserves the entry stack, aligns its working stack, and calls `__libc_start_main`. Startup finds envp and the following auxiliary vector, initializes the thread pointer and stack protector, runs constructors, calls `main`, and passes its return value to `exit`.

Static TLS scans the program headers for `PT_TLS`, creates the initial thread block, calls `set_thread_area`, loads `%gs`, and registers the clear-tid address. Rach2 keeps a TLS base for each thread and loads it into its one TLS descriptor as it switches to the thread; a thread `pthread_create` makes gets its base from `clone`'s `CLONE_SETTLS`. The [TLS document](../../rach2/sys/arch/i686/tls.Readme.md) defines the supported descriptor and inheritance rules.

Rach2 publishes no `AT_SYSINFO` or `AT_SYSINFO_EHDR`, so musl uses its i386 `int 0x80` syscall path. The kernel returns failures as negative Linux errno values. The kernel and subsystem Readmes own the individual syscall contracts.

## Images and self-build

The os-real, metal-disk, and GRUB-test manifests stage the installed headers as `/include` and the library and startup objects as `/lib/libc.a` and `/lib/crt*.o`. The staged TinyCC and pdpmake use them to build the project utilities. Musl itself is cross-built on the host and is not rebuilt in the guest.

The [test documentation](../../tests/Readme.md) describes the 35 musl-linked programs and which 17 run in the default suite. `tests/run selfbuild` exercises the staged compiler, library, headers, startup objects, and guest makefiles.

## Updating the pin

To update musl:

1. Replace the archive with a release tarball from the [musl releases](https://musl.libc.org/releases/) directory.
2. Change `MUSL_VERSION` and `SHA256` in `build_libc.sh`, and change `LIBC_TARBALL` in the root Makefile.
3. Rebase every patch under `patches/` onto the new release. Keep the spawn patch unless the new source or kernel contract makes it unnecessary. If a patch is removed, remove `.stamp` too.
4. Remove the old ignored extracted directory if its versioned name is no longer used.
5. Run the build and verification commands below. Inspect the generated configuration and one linked ELF again.

## Verification

```
make musl
make musl-programs
tests/run style
tests/run host
make test
tests/run selfbuild
```

`make test` includes `spawnmusl`. The root `make` also builds all four larger ports against the installed musl.
