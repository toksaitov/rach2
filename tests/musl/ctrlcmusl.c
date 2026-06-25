/**
 * Exercise Ctrl-C signal delivery through the ported musl.
 *
 * An ordinary hosted C program linked statically against the ported musl. It
 * prints "CTRLC READY" and then blocks in `read()` on stdin. A Ctrl-C on the
 * console (the harness sends the ETX byte 0x03 over the serial line; the PS/2
 * keyboard path does the same on Ctrl+C) must deliver SIGINT, whose default
 * action terminates the process before `read()` ever returns. The kernel then
 * reports "killed by signal 2" and returns to its shell.
 *
 * Success is the harness seeing the kill, not this program printing. If Ctrl-C
 * were ignored, `read()` would return and the program would print "CTRLC NOT
 * INTERRUPTED", which the harness treats as failure. No handler is installed on
 * purpose: the default disposition is the point, since interrupting a program
 * means terminating it.
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
