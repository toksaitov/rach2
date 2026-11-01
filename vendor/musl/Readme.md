# musl

musl is the C library of every hosted Rach2 program: the in-house utilities, the musl test programs, and the ports BusyBox, Dropbear, TinyCC, pdpmake and e2fsprogs. Rach2 builds it as a static library for the architecture of the build, i386 or x86-64, with the kernel's own cross compiler, and stages its headers, `libc.a` and startup objects on the systems that compile programs on themselves. musl makes Linux system calls, and the kernel implements the Linux system call interface of each architecture, so musl uses that interface with the local changes below.

## Interfaces

The build installs, for i686 under `vendor/musl/install/` and for x86-64 under `build/x86_64/vendor-work/musl/install/`:

* `include/`: the headers.
* `lib/libc.a`: the static library. There is no shared library.
* `lib/crt1.o`, `lib/crti.o` and `lib/crtn.o`: the startup objects.

The extracted source is used as well: `krffs/mkhash.krffs` and the host tests `mkhash`, `pwauth` and `pwauthatexit` compile its `src/crypt/crypt_sha512.c` for the host.

A program links against musl in this order, which [`utils/Makefile`](../../utils/Makefile) and [`tests/musl/build.sh`](../../tests/musl/build.sh) use:

```
i686-elf-gcc -m32 -static -nostdlib -nostartfiles \
    -isystem vendor/musl/install/include -o prog \
    vendor/musl/install/lib/crt1.o vendor/musl/install/lib/crti.o \
    prog.c \
    vendor/musl/install/lib/libc.a \
    $(i686-elf-gcc -m32 -print-libgcc-file-name) \
    vendor/musl/install/lib/crtn.o -Wl,-z,noexecstack
```

On x86-64 the compiler is `x86_64-elf-gcc -m64`, the files are those of the x86-64 install, and the link adds `-Wl,-z,max-page-size=0x1000`, so that segments are aligned to 4 KiB pages rather than 2 MiB. The result is a static `ET_EXEC` with no `PT_INTERP`, its first segment at `0x08048000` on i686 and at `0x400000` on x86-64.

The ports compile and link through [`muslgcc`](muslgcc), which reads the architecture from `RACH2_ARCH` through [`mk/arch.sh`](../../mk/arch.sh) and calls that architecture's compiler. With `-c`, `-S`, `-E`, `-M` or `-MM` it adds the musl headers; with `-r`, a partial link, it adds the headers and `-nostdlib` but no startup objects or libraries; otherwise it adds the whole sequence above. In a compile and a final link it also turns off the unwind tables, the stack protector, control-flow protection and PIE, which a distribution's GCC turns on by default, and adds the words of `MUSLGCC_DEFINES`, which a port's build script may set; e2fsprogs sets `-D__rach2__` there.

musl enters the kernel with `int $0x80` on i386, since the kernel gives no `AT_SYSINFO`, and with `syscall` on x86-64. The initial stack and auxiliary vector a program starts with are described in [execve(2)](../../man/man2/execve.2), the thread pointer musl sets up in the [thread-local storage document](../../rach2/sys/arch/tls.Readme.md), and the system calls in [syscalls(2)](../../man/man2/syscalls.2).

The manifests `os-real`, `os-real-gnu`, `grub-test` and `rach2-metal-disk` in [`krffs/manifests/`](../../krffs/manifests) stage the headers as `/include` and the library and startup objects in `/lib`, where the staged [TinyCC](../tcc/Readme.md) finds them. musl itself is not rebuilt on Rach2.

## Sources and license

The source is the release tarball from <https://musl.libc.org/releases/>, kept beside [`build_libc.sh`](build_libc.sh), which names its version in `MUSL_VERSION` and refuses a tarball whose SHA-256 is not `SHA256`. musl is under the MIT license, in the tarball's `COPYRIGHT` file.

## Local changes

[`patches/series`](patches/series) names the patches `build_libc.sh` applies, in order. There are five.

### posix_spawn forks

[`0001-posix-spawn-forks.patch`](patches/0001-posix-spawn-forks.patch) makes `posix_spawn` start its child with the raw `fork` system call in place of `__clone` with `CLONE_VM|CLONE_VFORK`, so the child runs in a copy of the parent's memory rather than in the parent's own. The child function execs, or writes its `errno` to the pipe and exits, so it works the same either way. The raw call keeps the negative error return that the code after it expects, and runs no `pthread_atfork` handlers. `system(3)` and `popen(3)` spawn through `posix_spawn`, and pdpmake runs its recipes through `system`. See [fork(2)](../../man/man2/fork.2).

### Linux's input headers

[`0002-linux-input-headers.patch`](patches/0002-linux-input-headers.patch) adds `<linux/input.h>` and `<linux/input-event-codes.h>`, the user headers of the kernel's input event interface, `/dev/input/eventN`, and the headers they include under `linux/`, `asm/` and `asm-generic/`. They are the files `make headers_install ARCH=x86` installs from Linux 6.18.54, the release the GNU root pins, unchanged; musl's `make install` installs them with its own. BusyBox's `acpid` includes `<linux/input.h>`. Under musl's 64-bit `time_t` the header gives `struct input_event` 16 bytes on i386 and 24 on x86-64, the layouts the kernel writes for each. To update them, run `headers_install` on the new release and make the patch again from the same files.

### Linux's keyboard headers

`0003-linux-keyboard-headers.patch` adds `<linux/kd.h>`, `<linux/keyboard.h>` and `<linux/wait.h>` from Linux 6.18.54's user headers. As `headers_install` does, it removes the kernel-only compiler include and `__user` annotations. kbd uses these numbers and structures to configure the console's keyboard. The headers retain Linux's syscall-note license exception.

### Condition variable wake fallback

`0004-cond-requeue-fallback.patch` wakes the next private condition waiter when both forms of `FUTEX_REQUEUE` return `ENOSYS`. The kernel does not implement requeue; musl's ordinary broadcast otherwise releases its first waiter and leaves the rest blocked on their barriers. The fallback preserves the barrier chain and lets each waiter take the mutex in turn. `threadscaling` exercises it with 16, 64 and 256 waiters.

### Linux's sound headers

[`0005-linux-sound-headers.patch`](patches/0005-linux-sound-headers.patch) adds `<linux/soundcard.h>` and `<linux/patchkey.h>`, the user headers of the OSS sound interface, `/dev/dsp` and `/dev/mixer`, and the ioctl headers they include, `<linux/ioctl.h>`, `<asm/ioctl.h>` and `<asm-generic/ioctl.h>`, as `make headers_install ARCH=x86` installs them from Linux 6.18.54. musl's own `<sys/soundcard.h>` includes `<linux/soundcard.h>`, so an OSS program includes `<sys/soundcard.h>` as it would on Linux. `<sys/ioctl.h>` and `<asm-generic/ioctl.h>` both define `_IOC`, `_IO`, `_IOR`, `_IOW` and `_IOWR`, with the same numbers; the patch makes each header define them only when the other has not, so that a program including both, which TinyCC compiles without treating either as a system header, gets no redefinition warnings. `soundcard.h` is under a BSD license, in its own comment.

## Building

```
make musl
make musl ARCH=x86_64
```

The root `make` builds musl before everything that links it. The stamp, `vendor/musl/.stamp` or `build/x86_64/vendor-work/musl/.stamp`, depends on `build_libc.sh`, `mk/arch.sh`, the tarball, `patches/series` and every `patches/*.patch`. Its recipe runs `build_libc.sh` with `RACH2_ARCH` set from `ARCH`, requires a nonempty `libc.a`, and writes `.outputs`, the list of installed files and the crypt source. When the Makefile is read, a stamp whose listed files are not all present, or whose `libc.a` is empty, is removed, so the next `make` builds musl again. `make check-libc` requires `libc.a` and the crypt source.

[`build_libc.sh`](build_libc.sh):

1. Checks the tarball's SHA-256 with `tools/verify/sha256.c`.
2. Extracts it, into `vendor/musl/` for i686 or `build/x86_64/vendor-work/musl/` for x86-64, removing an earlier tree.
3. Applies the patches `patches/series` names, with `patch -p1 -F 0 --forward`. A named patch that is missing or does not apply, or a `patches/*.patch` the series does not name, stops the build.
4. Unsets `CC` and runs musl's `configure` with `CROSS_COMPILE` set to `i686-elf-` or `x86_64-elf-`, `--target=i686` or `--target=x86_64`, `--disable-shared` and the install directory as the prefix. `mk/arch.sh` has removed `ARCH` and a caller's `make` variable overrides from the environment and `MAKEFLAGS`, so neither reaches musl's own Makefile.
5. Runs `make`, removes the old install directory, and runs `make install`. It then removes musl's own wrapper, `bin/musl-gcc`, and its `lib/musl-gcc.specs`, which name the install by its absolute path and which nothing here runs, so that the install is the same in every copy of the tree.

`make clean` leaves the extracted and installed trees; `make distclean` removes them and the stamps.

## Updating

1. Put the new release tarball in this directory and remove the old one.
2. Change `MUSL_VERSION` and `SHA256` in `build_libc.sh`, `LIBC_TARBALL` in the root `Makefile`, `MUSL_CRYPT` in `krffs/Makefile`, the `host-deps:` lines of `tests/host/mkhash_hosttest.c`, `pwauth_hosttest.c` and `pwauthatexit_hosttest.c`, and the extracted tree's name in `.gitignore`.
3. Rebase each patch onto the new source, or remove it from the directory and from `patches/series` when the release has made it unnecessary.
4. Build both architectures and run the tests below.

## Tests

* `tests/run style` links a program against the installed musl and checks with `readelf` that it is a static `ET_EXEC` of the architecture, with no `PT_INTERP` and its first segment at the address above.
* `tests/run host` builds the host tests that compile musl's crypt source.
* `tests/run default` runs the musl programs of [`tests/musl/`](../../tests/Readme.md), `spawnmusl` among them, which checks `posix_spawn`, `system` and `popen`: exit status, pipe output, spawn errors, the child's cleanup and the parent's memory.
* `tests/run selfbuild` builds the kernel, the utilities and the shell on Rach2 with the staged headers, library and startup objects.
* The root `make` builds the five ports against musl.
