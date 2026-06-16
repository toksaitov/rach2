# Host seam tests (the dual-target seam)

These tests run pure kernel/util logic natively on the host, under AddressSanitizer + UndefinedBehaviorSanitizer, in milliseconds.

A dual-target seam compiles syscall-free logic (string and memory routines, parsers, the VMA tree, path resolution, allocators, scheduler policy) two ways from the same `.c` file: freestanding into the kernel, where only QEMU can run it, and hosted into a native test binary, where a debugger and the sanitizers can. A bounds bug then shows up here, instantly and with a precise diagnosis, instead of as a triple fault in the guest.

Linux unit-tests self-contained pieces the same way (`tools/testing`), and libraries keep their core logic host-verifiable.

## Running

```
make verify-host          # from the repo root
```

`tools/hosttest.sh` compiles every `tests/host/*_hosttest.c` with `-fsanitize=address,undefined -fno-sanitize-recover=all` and runs it. A test passes when it exits 0 (it returns its failed-check count as the status, capped so it never wraps to a false 0, and prints nothing); the sanitizers abort on any out-of-bounds access, overflow, or other undefined behavior in the linked kernel code.

## Writing a seam test

Name the file `*_hosttest.c` in this directory and declare the kernel sources it links with a `host-deps:` comment:

```c
/*
 * foo_hosttest.c
 * host-deps: rach2/sys/lib/foo.c rach2/sys/lib/bar.c
 */
#include "foo.h"          /* kernel prototypes; use kernel types, not host libc */
static int fails;
#define CHECK(c) do { if (!(c)) { fails++; } } while (0)
int main(void) { /* ... CHECK(...) ... */ return fails; }
```

Keep the test free of host `<stdio.h>`/`<string.h>` so kernel and host type definitions never collide; assert via the return code; let the sanitizers do the rest.

Current tests:

* `kstring_hosttest.c`: the kernel's `sys/lib/kstring.c` string/memory routines (the first seam already in the tree). Later phases add tests for the VMA tree, path resolution, dirent parsing, and the CSPRNG core.
