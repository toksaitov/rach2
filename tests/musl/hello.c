/**
 * hello: check that a hosted C program starts and exits through the ported
 * musl.
 *
 * Unlike the freestanding probes in `tests/`, this includes `<stdio.h>`, calls
 * `printf` (buffered stdio over `writev`), and returns from `main` so musl's
 * `__libc_start_main` runs its exit path. It is linked statically against the
 * ported musl (`crt1.o`, `crti.o`, `libc.a`, libgcc and `crtn.o`, as
 * `build.sh` links every program here). Running it in the guest exercises
 * the whole startup path: musl reaches `main`, sets up thread-local storage
 * (`set_thread_area` on i686, `arch_prctl` on x86-64), buffers stdio, and
 * exits through `exit_group`. It also calls `uname` and prints the result when
 * that succeeds. No suite runs it; it is run by hand, as `exec musl/hello` at
 * the kernel shell.
 */

#include <stdio.h>
#include <sys/utsname.h>

int main(void)
{
    struct utsname u;

    printf("hello, musl on Rach2\n");

    if (uname(&u) == 0) {
        printf("uname: %s %s %s\n", u.sysname, u.release, u.machine);
    }

    return 0;
}
