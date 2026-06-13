/*
 * hello.c - Phase 3 integration milestone: the first real musl program.
 *
 * Unlike the freestanding probes in tests/, this is an ordinary hosted C
 * program: it includes <stdio.h>, calls printf (buffered stdio over writev),
 * and returns from main so musl's __libc_start_main runs its exit path. It is
 * linked statically against the ported musl (crt1 + libc.a + crtn). If it runs
 * in the guest, musl reached main, did TLS setup (set_thread_area), buffered
 * stdio, and exited cleanly through exit_group -- the whole Phase 3 contract.
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
