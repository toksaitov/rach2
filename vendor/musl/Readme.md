# musl 1.2.5 integration

Rach2 links its hosted user programs against a static i386 build of [musl libc](https://musl.libc.org/). Musl supplies the C library, startup objects, headers, and Linux i386 syscall ABI used by the utilities, tests, and larger ports.

This directory contains the project-owned integration files and the pinned upstream archive. The tracked inputs are `.gitignore`, this Readme, `build_libc.sh`, `musl-1.2.5.tar.gz`, and `patches/0001-posix-spawn-forks.patch`. The extracted source, installed files, and build stamp are ignored and reproducible.

## Build

From the repository root:

```
make deps
```

The ordinary root build also builds musl before the utilities that require it. `make deps DEPS=full` adds the other four ports.

The root stamp depends on `build_libc.sh`, the exact archive, and every file currently under `patches/`. An added or edited patch invalidates it; deleting a patch requires removing `.stamp` because the vanished prerequisite is no longer in Make's dependency list. The recipe removes the old installed archive, invokes the script, requires a nonempty `install/lib/libc.a`, and records success only afterward. `make clean` preserves the extracted and installed trees; `make distclean` removes them and the stamp.

`build_libc.sh` performs these steps:

1. Require `musl-1.2.5.tar.gz` to have SHA-256 `a9a118bbe84d8764da0ea0d28b3ab3fae8477fc7e4085d90102b8596fc7c75e4`.
2. Replace the extracted `musl-1.2.5/` tree and apply `patches/*.patch` in name order with no fuzz.
3. Clear inherited compiler and make overrides. Configure with `CROSS_COMPILE=i686-elf-`, `--target=i686`, `--disable-shared`, and the absolute `install/` prefix.
4. Build in parallel, replace the old install tree, and run musl's install target.

The compiler is deliberately fixed at `i686-elf-gcc`; a caller's `CC`, `CROSS_CC`, or make flags cannot change this vendor build. The generated configuration selects the i386 architecture and static libraries.

The integration consumes:

* `install/include/`: musl headers.
* `install/lib/libc.a`: the static C library.
* `install/lib/crt1.o`, `crti.o`, and `crtn.o`: process startup and init/fini objects.
* `musl-1.2.5/src/crypt/crypt_sha512.c`: the pinned SHA-512 crypt implementation compiled into the KRFFS host credential tools and their host tests.

The root `check-libc` target checks both `libc.a` and the extracted crypt source. If either is missing while `.stamp` remains, remove the stamp and run `make deps`.

## Project patch

The build applies one source patch. `0001-posix-spawn-forks.patch` changes musl's `posix_spawn` child creation from `clone(CLONE_VM|CLONE_VFORK|SIGCHLD)` to the raw `fork` syscall.

Rach2 has no shared address spaces and leaves syscall 120, `clone`, unimplemented. Pretending that a private copy satisfied `CLONE_VM` would give the caller the wrong contract. A forked spawn child can use the existing implementation because it either execs or writes its errno through the existing pipe and exits. The raw syscall also preserves musl's negative-errno path and does not run `pthread_atfork` handlers.

The patch is required by `posix_spawn`, `system`, and `popen`; pdpmake uses the latter interfaces to run recipes. The default `spawnmusl` probe checks command status, pipe output, spawn errors, child cleanup, parent memory, repeated calls, and continued `clone` refusal with `ENOSYS`.

Any additional project patch belongs under `patches/` with a numbered descriptive name. Rebase and explain a patch when the pinned release changes. Do not edit the extracted source as the maintained copy.

## Static link contract

A direct link from the repository root has this order:

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

The utilities and `tests/musl/build.sh` use this order directly. BusyBox and Dropbear have project-owned `muslgcc` wrappers that add only headers during compilation, omit startup and libraries from partial links, and add the complete sequence at the final link. TinyCC and pdpmake use the BusyBox wrapper for their host builds.

## Process ABI

The kernel gives a new process one System V i386 entry stack: `argc`, the `argv` pointers and null, the `envp` pointers and null, an auxiliary vector ending with `AT_NULL`, then the referenced strings and 16 random bytes. Entry ESP is aligned to 16 bytes.

Rach2 supplies `AT_PHDR`, `AT_PHENT`, `AT_PHNUM`, `AT_ENTRY`, `AT_PAGESZ`, `AT_CLKTCK`, `AT_SECURE`, and `AT_RANDOM`. `AT_SECURE` is one when an effective uid or gid differs from its real id. `AT_RANDOM` gives musl the bytes used for its stack protector and allocator state; the [randomness document](../../rach2/sys/lib/krandom.Readme.md) defines their trust policy.

Musl's i386 `_start` preserves the entry stack, aligns its working stack, and calls `__libc_start_main`. Startup finds envp and the following auxiliary vector, initializes the thread pointer and stack protector, runs constructors, calls `main`, and passes its return value to `exit`.

Static TLS scans the program headers for `PT_TLS`, creates the initial thread block, calls `set_thread_area`, loads `%gs`, and registers the clear-tid address. Rach2 provides one per-process TLS descriptor and restores its base on a context switch. It does not provide `clone` or threads. The [TLS document](../../rach2/sys/proc/tls.Readme.md) defines the supported descriptor and inheritance rules.

Rach2 publishes no `AT_SYSINFO` or `AT_SYSINFO_EHDR`, so musl uses its i386 `int 0x80` syscall path. The kernel returns failures as negative Linux errno values. The kernel and subsystem Readmes own the individual syscall contracts.

## Images and self-build

The os-real, metal-disk, and GRUB-test manifests stage the installed headers as `/include` and the library and startup objects as `/lib/libc.a` and `/lib/crt*.o`. The staged TinyCC and pdpmake use them to build the project utilities. Musl itself is cross-built on the host and is not rebuilt in the guest.

The [test documentation](../../tests/Readme.md) describes the 35 musl-linked programs and which 16 run in the default suite. `make selfbuild-test` exercises the staged compiler, library, headers, startup objects, and guest makefiles.

## Updating the pin

To update musl:

1. Replace the archive with a release tarball from the [musl releases](https://musl.libc.org/releases/) directory.
2. Change `MUSL_VERSION` and `SHA256` in `build_libc.sh`, and change `LIBC_TARBALL` in the root Makefile.
3. Rebase every patch under `patches/` onto the new release. Keep the spawn patch unless the new source or kernel contract makes it unnecessary. If a patch is removed, remove `.stamp` too.
4. Remove the old ignored extracted directory if its versioned name is no longer used.
5. Run the build and verification commands below. Inspect the generated configuration and one linked ELF again.

## Verification

```
make deps
make musl-programs
make verify-style
make verify-host
make test
make selfbuild-test
```

`make test` includes `spawnmusl`. `make full` also builds all four larger ports against the installed musl.
