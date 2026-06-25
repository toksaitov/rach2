/**
 * The first ordinary hosted C program run on Rach2 through the real musl.
 *
 * Unlike the freestanding probes in `tests/`, this includes `<stdio.h>`, calls
 * `printf` (buffered stdio over `writev`), and returns from `main` so musl's
 * `__libc_start_main` runs its exit path. It is linked statically against the
 * ported musl (`crt1` + `libc.a` + `crtn`). Running it in the guest exercises
 * the whole startup path: musl reaches `main`, does TLS setup
 * (`set_thread_area`), buffers stdio, and exits cleanly through `exit_group`.
 * It also calls `uname` and prints the result when that succeeds.
 */

#include <stdio.h>
#include <sys/utsname.h>

int main(void)
{
    struct utsname u;

    printf("hello, musl on Rach2\n");

    if (uname(&u) == 0)
    {
        printf("uname: %s %s %s\n", u.sysname, u.release, u.machine);
    }

    return 0;
}
