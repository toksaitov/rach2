/**
 * clockloadmusl: compare guest and host clocks across emulator stops.
 *
 * The argument is the host UDP reference port. A bracketed reference sample
 * precedes CLOCKLOAD READY. Pipe round trips and arithmetic keep the guest
 * busy until the host writes a newline after its last stop, then a second
 * sample ends the measurement. Each clock's whole error interval must be
 * inside 250 ms. Serial transport affects only when the workload ends.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include "clockref.h"

#define PAGE 4096

static int stdin_flags = -1;

/** Restore the shared console flags on every exit after changing them. */
static void restore_stdin(void)
{
    if (stdin_flags >= 0)
        fcntl(STDIN_FILENO, F_SETFL, stdin_flags);
}

/** Report a failed call and return 1, the program's failure status. */
static int fail(const char *what)
{
    printf("CLOCKLOAD FAIL (%s)\n", what);
    return 1;
}

/** One page through the pipe and back, with arithmetic in user mode. */
static int one_pass(const int fd[2], unsigned char *page)
{
    unsigned long sum = 0;
    int i;

    if (write(fd[1], page, PAGE) != PAGE || read(fd[0], page, PAGE) != PAGE)
        return 0;
    for (i = 0; i < PAGE; i++) {
        sum += page[i] * (i + 1);
        page[i] = (unsigned char)(sum >> 3);
    }
    return 1;
}

/** Report and judge a clock's accumulated error, including uncertainty. */
static int elapsed(const char *name, long long before, long long after, long long host)
{
    int verdict = clockref_judge(before - host, after - host, 250000000LL);

    printf("CLOCKLOAD %s error [%lld, %lld] ns\n", name, before - host, after - host);
    if (verdict != 1)
        return fail(verdict == 0 ? "clock drift" : "reference uncertainty");
    return 0;
}

/** Run the load until the host completes its stops, then judge both clocks. */
int main(int argc, char **argv)
{
    static unsigned char page[PAGE];
    struct clockref_sample first, last;
    int fd[2], flags;
    long port = argc == 2 ? strtol(argv[1], NULL, 10) : 0;
    long long host;
    char finish;
    ssize_t length;

    if (port < 1 || port > 65535)
        return fail("usage: clockloadmusl PORT");
    if (pipe(fd) != 0)
        return fail("pipe");
    memset(page, 1, sizeof(page));
    setvbuf(stdout, NULL, _IOLBF, 0);
    flags = fcntl(STDIN_FILENO, F_GETFL);
    if (flags < 0 || fcntl(STDIN_FILENO, F_SETFL, flags | O_NONBLOCK) < 0)
        return fail("stdin flags");
    stdin_flags = flags;
    atexit(restore_stdin);
    if (!clockref_read((int)port, &first))
        return fail("reference unavailable");
    printf("CLOCKLOAD READY\n");
    for (;;) {
        length = read(STDIN_FILENO, &finish, 1);
        if (length == 1 && finish == '\n')
            break;
        if (length == 0 || (length < 0 && errno != EAGAIN && errno != EINTR))
            return fail("finish command unavailable");
        if (!one_pass(fd, page))
            return fail("pipe round trip");
    }
    if (!clockref_read((int)port, &last))
        return fail("reference unavailable");
    host = last.host_mono - first.host_mono;
    if (host <= 0)
        return fail("host reference did not advance");
    if (elapsed("monotonic", last.mono_before - first.mono_after,
                last.mono_after - first.mono_before, host) ||
        elapsed("realtime", last.real_before - first.real_after,
                last.real_after - first.real_before, host))
        return 1;
    close(fd[0]);
    close(fd[1]);
    if (fcntl(STDIN_FILENO, F_SETFL, flags) < 0)
        return fail("restore stdin flags");
    printf("CLOCKLOAD OK\n");
    return 0;
}
