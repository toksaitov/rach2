# musl: the C library for Rach2

Rach2 links its userspace against [musl libc](https://musl.libc.org), a small, clean, MIT-licensed C library. musl is what lets Rach2 run real C programs built against a real libc, rather than a handful of hand-written freestanding ones, and porting it drives the process ABI, TLS, virtual memory, and syscall work.

This is how a static i386 Linux program is built and started: musl is a drop-in libc on Linux, and the startup contract below is the SysV/i386 process ABI that Linux implements.

## Building

```
./build_libc.sh
```

You rarely run this by hand. The top-level `make` (and `make deps`) builds libc on demand behind a stamp file, in dependency order, before anything that links against it.

No musl cross-toolchain is required. The build uses the same `i686-elf-gcc` the kernel uses (`CROSS_COMPILE=i686-elf- ./configure --target=i686 --disable-shared`), so it stays one compiler and one library build. The musl version is pinned to 1.2.5, and the vendored tarball's sha256 is verified before extraction. The extracted source and the `install/` output are rebuildable and git-ignored; the tracked files are the tarball, this Readme, the build script, and the one patch under `patches/` explained below.

Outputs (under `install/`):

* `lib/libc.a`, the static C library
* `lib/crt1.o`, `lib/crti.o`, `lib/crtn.o`, the C runtime startup and teardown objects
* `include/`, the musl headers

## Updating from upstream

musl is vendored as the pinned source tarball beside this Readme, and built from it with the patches under `patches/` applied at extraction; the Rach2-owned files here are this Readme, `build_libc.sh`, and those patches. To move to a new musl release:

1. Download the new `musl-X.Y.Z.tar.gz` from https://musl.libc.org and replace the tarball here.
2. Set `MUSL_VERSION` and `SHA256` in `build_libc.sh` to the new release and its published sha256.
3. REBASE EVERY PATCH under `patches/` onto the new source and check that each still applies; `build_libc.sh` stops the build when one does not, which is the point. A patch that no longer applies is a decision to make again, not a line to delete.
4. Rebuild with `make` (or `./build_libc.sh`), then run `make verify-host`, `make test` (which includes `musl/spawnmusl`, the probe that fails outright if the spawn patch is missing) and a boot.
5. Otherwise musl is used as-is. If a new release expects a syscall the kernel does not implement, add it in `rach2/sys` rather than patching here: the adaptation belongs in the kernel unless, as with the spawn, the kernel cannot honestly provide what is asked for.

There is ONE source patch, and it is explained here because a patch nobody can
find the reason for becomes a fork.

`patches/0001-posix-spawn-forks.patch` makes `posix_spawn` fork rather than
clone. Upstream it issues `__clone(child, stack, CLONE_VM|CLONE_VFORK|SIGCHLD,
&args)`: the child runs in the PARENT's address space, on a stack of its own,
until it execs. Rach2 cannot share an address space between two processes, and a
syscall that accepted those flags and quietly handed back a private
copy-on-write image would be reporting semantics it does not provide, so syscall
120 stays unimplemented and this call site changes instead. The child function
never returns (it execs, or writes its errno to the pipe and exits) and it
reports failure through that pipe rather than through shared memory, so a forked
child serves it exactly as a cloned one did. Without this, `system(3)` and
`popen(3)` fail on this system with a bare -1, which means no program can run a
shell command and no make can run a recipe.

Any further patch goes in `patches/NNNN-reason.patch` (applied right after
extraction by `build_libc.sh`) and is explained here; keep them the rare
exception, not a fork.

## Linking a program against musl

```
i686-elf-gcc -static -nostdlib -nostartfiles -isystem vendor/musl/install/include \
  -o prog \
  vendor/musl/install/lib/crt1.o vendor/musl/install/lib/crti.o \
  prog.c \
  vendor/musl/install/lib/libc.a $(i686-elf-gcc -print-libgcc-file-name) \
  vendor/musl/install/lib/crtn.o \
  -Wl,-z,noexecstack
```

Order matters: `crt1.o crti.o` first (they provide `_start` and the init/fini frames), then the program, then `libc.a`, then libgcc (for the compiler's runtime helpers musl calls, such as 64-bit division), then `crtn.o` last. The result is a static, non-PIE ELF32 EXEC for i386, based at 0x08048000, issuing `int 0x80` syscalls: the same shape as the freestanding utilities, but with a real libc behind it.

```
ELF 32-bit LSB executable, Intel 80386, statically linked
Type: EXEC   LOAD @ 0x08048000   Entry: 0x0804xxxx   (no PT_INTERP)
```

## The startup contract (what the kernel must provide)

When the kernel hands control to a fresh musl process at `_start`, this chain runs before `main`, and each step depends on facts the kernel must supply.

```
_start                 (crt1.o) reads argc/argv/envp off the initial stack,
  |                    16-byte-aligns esp, calls __libc_start_main
  v
__libc_start_main
  |-- __init_libc(envp) scans envp, then the auxv just past envp's NULL
  |       |             terminator, recording page size, AT_SECURE, the
  |       |             AT_RANDOM pointer, and the rest
  |-- __init_tls(auxv)  finds PT_TLS via AT_PHDR/PHENT/PHNUM, allocates the
  |       |             TLS block, installs it with the set_thread_area
  |       |             syscall, then loads %gs  <-- the TLS crux
  |-- __libc_start_init runs the init_array constructors
  v
main(argc, argv, envp)
  v
exit(status)           runs atexit/fini, then exit_group   <-- terminates
```

### The initial stack

The kernel must build a correct SysV/i386 stack, low to high: `argc`, the `argv[]` pointers, `NULL`, the real `envp[]` pointers, `NULL`, the auxv pairs terminated by `AT_NULL`, then the string and aux-data area, with the entry `esp` 16-byte aligned. musl reads argc/argv/envp positionally and walks the auxv immediately after envp.

### The auxiliary vector

musl consults these `AT_` entries (each is a `{type, value}` pair); the kernel populates them from real facts:

| Entry                         | Value           | Why musl needs it                                  |
|-------------------------------|-----------------|----------------------------------------------------|
| `AT_PAGESZ` (6)               | 4096            | page size for `sysconf`, `malloc`, `mmap` rounding |
| `AT_CLKTCK` (17)              | 100             | `sysconf(_SC_CLK_TCK)` (the timer is 100 Hz)       |
| `AT_PHDR/PHENT/PHNUM` (3/4/5) | program headers | `__init_tls` locates `PT_TLS` here                 |
| `AT_ENTRY` (9)                | ELF entry       | informational                                      |
| `AT_RANDOM` (25)              | ptr to 16 bytes | seeds the stack canary and malloc secret           |
| `AT_SECURE` (23)              | 0 or 1          | 1 when the effective id differs from the real one  |
| `AT_NULL` (0)                 | none            | terminates the vector                              |

`AT_PHDR/PHENT/PHNUM/ENTRY` are recorded by the kernel's `elf_load`. `AT_RANDOM` points at 16 bytes from the kernel CSPRNG (`krandom`), seeding the stack canary and malloc secret; it is best-effort and always provided, since a canary only needs to be unpredictable. Crypto-grade randomness goes through `getrandom` and `/dev/urandom`, which draw from the same CSPRNG but fail closed (`-EIO`) until it is seeded from a trusted hardware source (`RDSEED`/`RDRAND`). See the krandom Readme.

Deliberately absent: `AT_SYSINFO` and `AT_SYSINFO_EHDR` (the vDSO). With no vDSO advertised, musl falls back to issuing syscalls via `int 0x80`, exactly the mechanism Rach2 implements. The linked `hello` has six `int 0x80` sites and no vsyscall path.

### TLS and the syscall set

`__init_tls` requires the `set_thread_area` syscall and a per-process GDT TLS descriptor so `%gs`-relative thread-local access works: the riskiest part of the port. Reaching and returning from `main` then needs a core syscall set (`writev`, `exit_group`, `set_tid_address`, `ioctl(TCGETS/TIOCGWINSZ)`, `getpid`, `uname`, plus `brk` and `mmap` for `malloc`) under the `-errno` contract.
