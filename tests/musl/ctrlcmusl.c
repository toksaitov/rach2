/**
 * ctrlcmusl: check by hand that Ctrl-C at the console kills a program blocked
 * in `read`.
 *
 * It prints "CTRLC READY" and then blocks in `read()` on stdin. A Ctrl-C on the
 * console (the byte 0x03 over the serial line, or Ctrl+C on the keyboard) must
 * deliver SIGINT, whose default action terminates the process before `read()`
 * returns. The kernel then reports the init process "killed by signal 2" and
 * returns to its shell.
 *
 * No suite runs it; a person runs `exec musl/ctrlcmusl` at the kernel shell
 * and types Ctrl-C. Success is the kill, so the program has no success token.
 * If Ctrl-C did not interrupt the read, `read()` would return and the program
 * would print "CTRLC NOT INTERRUPTED", the failure. No handler is installed,
 * since the default disposition is what the test checks: interrupting a
 * program terminates it.
 *
 * Built by tests/musl/build.sh.
 */

#include <stdio.h>
#include <unistd.h>

int main(void)
{
    char buf[16];
    ssize_t n;

    printf("CTRLC READY\n");
    fflush(stdout);

    n = read(STDIN_FILENO, buf, sizeof buf);

    printf("CTRLC NOT INTERRUPTED (read returned %d)\n", (int)n);
    return 0;
}
