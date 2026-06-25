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
/**
 * One line on what this seam exercises.
 *
 * host-deps: rach2/sys/lib/foo.c rach2/sys/lib/bar.c
 */
#include "foo.h"          /* kernel prototypes; use kernel types, not host libc */
static int fails;
#define CHECK(c) do { if (!(c)) { fails++; } } while (0)
int main(void) { /* ... CHECK(...) ... */ return fails; }
```

The `host-deps:` line is a build contract: `tools/hosttest.sh` reads the paths after it and hands them to the compiler, so the list names every kernel source the seam links separately. A test that pulls its source in with a direct `#include` (to reach the file's `static` routines) leaves the line empty.

Keep the test free of host `<stdio.h>`/`<string.h>` so kernel and host type definitions never collide; assert via the return code; let the sanitizers do the rest.

Current tests:

* `kstring_hosttest.c`: the string and memory routines (`sys/lib/kstring.c`, with `sys/lib/kctypes.c`).
* `krandom_hosttest.c`: the ChaCha20 keystream CSPRNG (`sys/lib/krandom.c`).
* `vm_hosttest.c`: the VMA interval tree, its overlap search and split logic (`sys/mm/vm.c`).
* `frame_hosttest.c`: the physical-frame pool sizing from the multiboot memory map (`sys/mm/frame.c`).
* `synthfs_hosttest.c`: the synthetic `/dev` and `/proc` path classifier (`sys/fs/synthfs.c`).
* `poll_select_hosttest.c`: the `fd_set` and `pollfd` translation and the readable/writable/except projection (`sys/io/poll_select.c`).
* `pipe_ring_hosttest.c`: the pipe ring-buffer arithmetic (`sys/ipc/pipe_ring.c`).
* `n_tty_hosttest.c`: the N_TTY line discipline's input editing and echo (`sys/tty/n_tty.c`).
* `tls_hosttest.c`: the `set_thread_area` user_desc validation (`sys/proc/tls.c`).
* `wait_queue_hosttest.c`: the wait-queue enqueue and wake logic (`sys/proc/wait_queue.c`, included directly under a build flag so its static routines are reachable, hence the empty `host-deps:`).
* `uimage_hosttest.c`: the initial-process stack and auxv builder (`sys/proc/uimage.c`, with `sys/lib/kstring.c` and `sys/lib/kctypes.c`).
