# libc -- musl for Rach2

Rach2 links its userspace against [musl libc](https://musl.libc.org), a small,
clean, MIT-licensed C library. musl is what turns Rach2 from "a kernel that runs
a few hand-written freestanding programs" into "a kernel that runs real C
programs built against a real libc" -- and porting it is the forcing function
for the process ABI, TLS, virtual memory, and syscall work in `PLAN.md`.

Real-OS analogue: this is exactly how a static i386 Linux program is built and
started -- musl is a drop-in libc on Linux, and the startup contract below is
the SysV/i386 process ABI that Linux implements.

## Building

```
./build_libc.sh
```

You rarely run this by hand: the top-level `make` (and `make deps`) builds libc on
demand behind a stamp file, in dependency order, before anything that links against it.

No musl cross-toolchain is required. The build uses the **same `i686-elf-gcc`**
the kernel uses (`CROSS_COMPILE=i686-elf- ./configure --target=i686
--disable-shared`), so the onramp stays "one compiler, one library build." The
musl version is **pinned to 1.2.5** and the vendored tarball's sha256 is
verified before extraction; the extracted source and the `install/` output are
rebuildable and git-ignored (only the tarball, this Readme, and the build
script are tracked).

Outputs (under `install/`):

- `lib/libc.a` -- the static C library
- `lib/crt1.o`, `lib/crti.o`, `lib/crtn.o` -- the C runtime startup/teardown objects
- `include/` -- the musl headers

## Linking a program against musl

```
i686-elf-gcc -static -nostdlib -nostartfiles -isystem libc/install/include \
  -o prog \
  libc/install/lib/crt1.o libc/install/lib/crti.o \
  prog.c \
  libc/install/lib/libc.a $(i686-elf-gcc -print-libgcc-file-name) \
  libc/install/lib/crtn.o \
  -Wl,-z,noexecstack
```

Order matters: `crt1.o crti.o` first (they provide `_start` and the
init/fini frames), then the program, then `libc.a`, then libgcc (for the
compiler's runtime helpers musl calls, e.g. 64-bit division), then `crtn.o`
last. The result is a **static, non-PIE ELF32 EXEC for i386, based at
0x08048000, with `int 0x80` syscalls** -- the same shape as today's
freestanding utilities, but with a real libc behind it. Verified in Phase 0:

```
ELF 32-bit LSB executable, Intel 80386, statically linked
Type: EXEC   Entry: 0x80480de   LOAD @ 0x08048000   (no PT_INTERP)
```

## The startup contract (what the kernel must provide)

When the kernel hands control to a fresh musl process at `_start`, this is the
chain that runs before `main`, and the kernel facts each step depends on. This
is the contract Phases 1-3 implement.

```
_start                 (crt1.o) reads argc/argv/envp off the initial stack,
  |                    16-byte-aligns esp, calls __libc_start_main
  v
__libc_start_main
  |-- __init_libc(envp) scans envp, then the auxv just past envp's NULL
  |       |             terminator, recording page size, AT_SECURE, the
  |       |             AT_RANDOM pointer, etc.
  |-- __init_tls(auxv)  finds PT_TLS via AT_PHDR/PHENT/PHNUM, allocates the
  |       |             TLS block, and installs it with the set_thread_area
  |       |             syscall, then loads %gs  <-- the Phase 2 crux
  |-- __libc_start_init runs the init_array constructors
  v
main(argc, argv, envp)
  v
exit(status)           runs atexit/fini, then exit_group   <-- terminates
```

### The initial stack (Phase 1)

The kernel must build a correct SysV/i386 stack, low to high: `argc`, the
`argv[]` pointers, `NULL`, the real `envp[]` pointers, `NULL`, the **auxv**
pairs terminated by `AT_NULL`, then the string/aux-data area, with the entry
`esp` 16-byte aligned. musl reads argc/argv/envp positionally and walks the
auxv immediately after envp.

### The auxiliary vector (Phase 1)

musl consults these `AT_` entries (each is a `{type, value}` pair); the kernel
populates them from real facts:

| Entry | Value | Why musl needs it |
|---|---|---|
| `AT_PAGESZ` (6) | 4096 | page size for `sysconf`, `malloc`, `mmap` rounding |
| `AT_CLKTCK` (17) | 100 | `sysconf(_SC_CLK_TCK)` (our timer is 100 Hz) |
| `AT_PHDR/PHENT/PHNUM` (3/4/5) | program headers | `__init_tls` locates `PT_TLS` here |
| `AT_ENTRY` (9) | ELF entry | informational |
| `AT_RANDOM` (25) | ptr to 16 bytes | seeds the stack canary and malloc secret |
| `AT_SECURE` (23) | 0 | not a setuid/secure exec |
| `AT_NULL` (0) | -- | terminates the vector |

`AT_PHDR/PHENT/PHNUM/ENTRY` are recorded by the kernel's `elf_load`. `AT_RANDOM`
in Part I is **best-effort, non-security** randomness (RDTSC/timer/RTC mixing)
-- fine for a canary, *not* for keys; the crypto-grade `getrandom`/`/dev/urandom`
arrives in Part II-B, before dropbear.

Deliberately **absent**: `AT_SYSINFO`/`AT_SYSINFO_EHDR` (the vDSO). With no
vDSO advertised, musl falls back to issuing syscalls via `int 0x80` -- exactly
the mechanism Rach2 implements. (Confirmed: the linked `hello` has six `int
0x80` sites and no vsyscall path.)

### TLS (Phase 2) and the syscall set (Phase 3)

`__init_tls` requires the **`set_thread_area`** syscall and a per-process **GDT
TLS descriptor** so `%gs`-relative thread-local access works -- the riskiest
part of the port (see Phase 2). Reaching and returning from `main` then needs
the MVP syscalls in Phase 3 (`writev`, `exit_group`, `set_tid_address`,
`ioctl(TCGETS/TIOCGWINSZ)`, `getpid`, `uname`, plus `brk`/`mmap` for `malloc`)
under the `-errno` contract.
