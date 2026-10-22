/**
 * readvmusl: check readv, which musl's stdio fills its buffer with.
 *
 * musl's stdio reads with `readv`, not `read`, whenever it wants more than a
 * single byte, so every program that reads through `fgets`, `fread` or
 * `scanf` depends on the call; BusyBox's `chpasswd`, for one, reads its input
 * with `xmalloc_fgetline`.
 *
 * The buffers are filled in order, and the probe checks how far each kind of
 * descriptor goes. A regular file is read again for each buffer, since its
 * read returns at once and its bytes have no boundary. A pipe is read again
 * only while it has more, so a read never waits for input nobody has written.
 * A datagram socket is not read again at all: one message is copied out
 * across the buffers it reaches, and the next read starts the next message.
 * A terminal is not read again either, since the line is its unit. A reader
 * that asked for more than it got must go round again, which is what musl's
 * stdio does and what the buffered cases here show.
 *
 * The rest is the edges: the whole vector list is checked before a byte is
 * taken, so a bad address in a later buffer fails the call with the input
 * still queued; a list that asks for nothing still checks the descriptor; and
 * a negative count fails with EINVAL.
 *
 * Prints `READVMUSL OK` and exits 0 on success, and prints
 * `READVMUSL FAIL (detail)` and exits 1 otherwise. Writes etude's report
 * records (../probe_report.h) for the case ETUDE_CASE names, an assertion for
 * each check `checks` lists; a failure leaves the checks after it not run.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <signal.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "../probe_report.h"

#define SCRATCH "/readvmusl.tmp"

/* The checks, one assertion each, in the order the program makes them, and
 * the text it prints for each that fails. */
enum {
    LINE_PIPE,
    FDOPEN,
    FGETS,
    FGETS_LINE,
    SECOND_LINE,
    BLOCK_PIPE,
    FDOPEN_BLOCK,
    FREAD,
    FREAD_BYTES,
    WALK_FILE,
    FILE_TOTAL,
    FILE_ORDER,
    EMPTY_FILE,
    EMPTY_BUFFER,
    EMPTY_AROUND,
    SHORT_FILE,
    SHORT_EOF,
    WALK_PIPE,
    PIPE_HELD,
    PIPE_ORDER,
    LIVE_PIPE,
    LIVE_WRITE,
    LIVE_STOP,
    REFUSAL_PIPE,
    OUTSIDE_WINDOW,
    UNMAPPED,
    INPUT_KEPT,
    EDGE_PIPE,
    EMPTY_BUFFERS,
    CLOSED_FD,
    COUNT_PIPE,
    NO_BUFFERS,
    NEGATIVE_COUNT,
    DGRAM_SOCKET,
    DGRAM_BIND,
    DGRAM_SEND,
    DGRAM_FILL,
    DGRAM_ORDER,
    DGRAM_SEND_2,
    DGRAM_TRUNCATE,
    DGRAM_TRUNCATED_BYTES,
    DGRAM_SEND_ALIAS,
    DGRAM_ALIAS,
    DGRAM_SEND_3,
    DGRAM_NEXT,
    PTY_MASTER,
    PTY_UNLOCK,
    PTY_SLAVE,
    PTY_WRITE,
    TTY_ONE_LINE,
    TTY_LINE,
    SHARED_FORK,
    SHARED_WAIT,
    SHARED_PAGE,
    CHECKS
};

static const struct {
    const char *id;
    const char *text;
} checks[CHECKS] = {
    {"line-pipe", "could not make the pipe the buffered read uses"},
    {"fdopen", "fdopen"},
    {"fgets", "fgets read nothing, which is the defect this probe exists for"},
    {"fgets-line", "fgets returned something other than the line written"},
    {"second-line", "the second buffered line did not arrive"},
    {"block-pipe", "could not make the pipe the block read uses"},
    {"fdopen-block", "fdopen for the block read"},
    {"fread", "fread did not return the bytes that were written"},
    {"fread-bytes", "fread returned the wrong bytes"},
    {"walk-file", "could not make the file the walk uses"},
    {"file-total", "readv across three buffers of a file did not return the total"},
    {"file-order", "readv filled the buffers out of order"},
    {"empty-file", "could not make the file the empty-buffer case uses"},
    {"empty-buffer", "a zero-length buffer stopped the walk"},
    {"empty-around", "the buffers around the empty one hold the wrong bytes"},
    {"short-file", "could not make the file the short case uses"},
    {"short-eof", "end of file did not stop the walk"},
    {"walk-pipe", "could not make the pipe the walk uses"},
    {"pipe-held", "readv of a pipe stopped before the bytes it held ran out"},
    {"pipe-order", "readv of a pipe filled the wrong buffer"},
    {"live-pipe", "could not make the pipe the live-writer case uses"},
    {"live-write", "could not write the live-writer pipe"},
    {"live-stop", "readv did not stop at the bytes a live writer had written"},
    {"refusal-pipe", "could not make the pipe the refusal case uses"},
    {"outside-window", "an address outside the user window was not refused"},
    {"unmapped", "an unmapped address inside the user window was not refused"},
    {"input-kept", "the refused readv had already taken the input"},
    {"edge-pipe", "could not make the pipe the edge cases use"},
    {"empty-buffers", "readv of empty buffers did not return 0"},
    {"closed-fd", "readv of empty buffers on a closed fd was not refused"},
    {"count-pipe", "could not make the pipe the count case uses"},
    {"no-buffers", "readv of no buffers did not return 0"},
    {"negative-count", "a negative count was not refused with EINVAL"},
    {"dgram-socket", "could not make the datagram socket"},
    {"dgram-bind", "could not bind the datagram socket to a port"},
    {"dgram-send", "could not send the datagram"},
    {"dgram-fill", "one datagram did not fill the buffers it reached"},
    {"dgram-order", "the datagram arrived out of order across the buffers"},
    {"dgram-send-2", "could not send the second datagram"},
    {"dgram-truncate", "a datagram past the end of the buffers did not stop there"},
    {"dgram-truncated-bytes", "the truncated datagram has the wrong bytes"},
    {"dgram-send-alias", "could not send the aliasing datagram"},
    {"dgram-alias", "the aliasing readv did not even fill the first buffer"},
    {"dgram-send-3", "could not send the third datagram"},
    {"dgram-next", "a read after a truncated datagram saw the wrong message"},
    {"pty-master", "could not open a pty master"},
    {"pty-unlock", "could not unlock the pty"},
    {"pty-slave", "could not open the pty slave"},
    {"pty-write", "could not write the two lines"},
    {"tty-one-line", "readv of a terminal took more than the line it asked for"},
    {"tty-line", "readv of a terminal returned something other than the line"},
    {"shared-fork", "fork for the shared-page case"},
    {"shared-wait", "waiting for the shared-page child"},
    {"shared-page", "a readv into a page inherited through fork did not deliver"},
};

/** In the process that began the report, report every check before `check`
 * as held, `check` as failed with its text, those after it as not run, and
 * the report's end; with `check` CHECKS, every check held. */
static void report_at(int check)
{
    int i;

    if (report_ended || report_getpid() != report_owner) {
        return;
    }
    for (i = 0; i < CHECKS; i++) {
        if (i < check) {
            report_pass(checks[i].id);
        } else if (i == check) {
            report_fail(checks[i].id, checks[i].text);
        } else {
            report_notrun(checks[i].id, "an earlier check failed");
        }
    }
    report_end();
}

/** Report the failed check `check` and return 1, the probe's failure
 * status. */
static int fail(int check)
{
    printf("READVMUSL FAIL (%s)\n", checks[check].text);
    report_at(check);

    return 1;
}

/**
 * Make a pipe with `text` in it, and return its read end, or -1.
 *
 * The writing end is closed before the caller reads, so a reader that asks for
 * more than was written sees end of input instead of blocking for a writer
 * that will never write again. The cases that need a live writer make their own
 * pipe and keep its other end.
 */
static int pipe_holding(const char *text)
{
    int ends[2];

    if (pipe(ends) != 0) {
        return -1;
    }
    if (write(ends[1], text, strlen(text)) != (ssize_t)strlen(text)) {
        close(ends[0]);
        close(ends[1]);

        return -1;
    }
    close(ends[1]);

    return ends[0];
}

/**
 * Make the file `SCRATCH` with `text` in it, and return a descriptor open on
 * it for reading, or -1.
 */
static int file_holding(const char *text)
{
    int fd = open(SCRATCH, O_WRONLY | O_CREAT | O_TRUNC, 0644);

    if (fd < 0) {
        return -1;
    }
    if (write(fd, text, strlen(text)) != (ssize_t)strlen(text)) {
        close(fd);

        return -1;
    }
    close(fd);

    return open(SCRATCH, O_RDONLY);
}

/**
 * Check `fgets` and `fread` through stdio on a pipe, the reads the call is
 * for. Returns 0, or 1 once the failure is reported.
 */
static int check_buffered(void)
{
    char line[64];
    char block[16];
    FILE *stream;
    int fd;

    fd = pipe_holding("a line through stdio\nsecond\n");
    if (fd < 0) {
        return fail(LINE_PIPE);
    }
    stream = fdopen(fd, "r");
    if (!stream) {
        close(fd);

        return fail(FDOPEN);
    }
    if (!fgets(line, sizeof(line), stream)) {
        fclose(stream);

        return fail(FGETS);
    }
    if (strcmp(line, "a line through stdio\n") != 0) {
        fclose(stream);

        return fail(FGETS_LINE);
    }
    if (!fgets(line, sizeof(line), stream) || strcmp(line, "second\n") != 0) {
        fclose(stream);

        return fail(SECOND_LINE);
    }
    fclose(stream);

    fd = pipe_holding("0123456789abcdef");
    if (fd < 0) {
        return fail(BLOCK_PIPE);
    }
    stream = fdopen(fd, "r");
    if (!stream) {
        close(fd);

        return fail(FDOPEN_BLOCK);
    }
    if (fread(block, 1, sizeof(block), stream) != sizeof(block)) {
        fclose(stream);

        return fail(FREAD);
    }
    fclose(stream);
    if (memcmp(block, "0123456789abcdef", sizeof(block)) != 0) {
        return fail(FREAD_BYTES);
    }

    return 0;
}

/**
 * Check a regular file, the one kind of descriptor read again for each
 * buffer: the order of the buffers, a zero-length buffer, and the end of the
 * file. Returns 0, or 1 once the failure is reported.
 */
static int check_file(void)
{
    struct iovec parts[3];
    char one[4];
    char two[4];
    char three[4];
    ssize_t got;
    int fd;

    fd = file_holding("AAAABBBBCCCC");
    if (fd < 0) {
        return fail(WALK_FILE);
    }
    parts[0].iov_base = one;
    parts[0].iov_len = sizeof(one);
    parts[1].iov_base = two;
    parts[1].iov_len = sizeof(two);
    parts[2].iov_base = three;
    parts[2].iov_len = sizeof(three);
    got = readv(fd, parts, 3);
    close(fd);
    if (got != 12) {
        return fail(FILE_TOTAL);
    }
    if (memcmp(one, "AAAA", 4) != 0 || memcmp(two, "BBBB", 4) != 0 ||
        memcmp(three, "CCCC", 4) != 0) {
        return fail(FILE_ORDER);
    }

    /* A zero-length buffer in the middle is skipped, not treated as an end. */
    fd = file_holding("XXXXYYYY");
    if (fd < 0) {
        return fail(EMPTY_FILE);
    }
    parts[1].iov_len = 0;
    got = readv(fd, parts, 3);
    close(fd);
    if (got != 8) {
        return fail(EMPTY_BUFFER);
    }
    if (memcmp(one, "XXXX", 4) != 0 || memcmp(three, "YYYY", 4) != 0) {
        return fail(EMPTY_AROUND);
    }

    /* End of file stops the walk instead of reporting an error. */
    fd = file_holding("ZZZZ");
    if (fd < 0) {
        return fail(SHORT_FILE);
    }
    parts[1].iov_len = sizeof(two);
    got = readv(fd, parts, 3);
    close(fd);
    if (got != 4) {
        return fail(SHORT_EOF);
    }

    unlink(SCRATCH);

    return 0;
}

/**
 * Check a pipe, which fills every buffer its bytes reach and stops there.
 * Returns 0, or 1 once the failure is reported.
 *
 * The second case is the one that matters: the pipe has exactly what the
 * first buffer takes and its writer is still open, so a further read would
 * have nothing to return and would wait until something wrote again. The
 * kernel checks whether there is more before it reads on, so it stops. A
 * fault there makes the probe hang, not fail, and the case's watchdog
 * reports it.
 */
static int check_stream(void)
{
    struct iovec parts[2];
    char one[4];
    char two[4];
    ssize_t got;
    int ends[2];
    int fd;

    fd = pipe_holding("AAAABBBB");
    if (fd < 0) {
        return fail(WALK_PIPE);
    }
    parts[0].iov_base = one;
    parts[0].iov_len = sizeof(one);
    parts[1].iov_base = two;
    parts[1].iov_len = sizeof(two);
    got = readv(fd, parts, 2);
    close(fd);
    if (got != 8) {
        return fail(PIPE_HELD);
    }
    if (memcmp(one, "AAAA", 4) != 0 || memcmp(two, "BBBB", 4) != 0) {
        return fail(PIPE_ORDER);
    }

    if (pipe(ends) != 0) {
        return fail(LIVE_PIPE);
    }
    if (write(ends[1], "LIVE", 4) != 4) {
        close(ends[0]);
        close(ends[1]);

        return fail(LIVE_WRITE);
    }
    got = readv(ends[0], parts, 2);
    close(ends[0]);
    close(ends[1]);
    if (got != 4) {
        return fail(LIVE_STOP);
    }

    return 0;
}

/**
 * Check a datagram socket, where the message and not the buffer is the unit.
 * Returns 0, or 1 once the failure is reported.
 *
 * One datagram fills as many buffers as it reaches, because a second read
 * would take the next datagram, and a copy into the first buffer alone would
 * drop whatever did not fit. A datagram longer than the buffers loses its
 * tail, as it does for a `read` too small for it, and the next read starts a
 * new message: a read after it must return the next datagram whole, not the
 * tail of the last.
 *
 * A caller may point a buffer at its own vector list, so that the message
 * rewrites the descriptors after it, with addresses nothing checked. The
 * aliasing payload does that: four bytes for the first buffer, then a
 * descriptor of an address in the user window that no mapping backs (8 bytes
 * on i386, 16 on x86-64), then bytes for the kernel to put there. The call
 * must return, not fault in kernel mode, with what it copied before it
 * stopped.
 *
 * The socket is on loopback, so the case needs no network device.
 */
static int check_datagram(void)
{
    struct sockaddr_in me;
    struct iovec parts[3];
    socklen_t melen = sizeof(me);
    char one[4];
    char two[4];
    char three[8];
    ssize_t got;
    int fd;

    fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        return fail(DGRAM_SOCKET);
    }
    memset(&me, 0, sizeof(me));
    me.sin_family = AF_INET;
    me.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(fd, (struct sockaddr *)&me, sizeof(me)) != 0 ||
        getsockname(fd, (struct sockaddr *)&me, &melen) != 0) {
        close(fd);

        return fail(DGRAM_BIND);
    }
    if (sendto(fd, "AAAABBBBCC", 10, 0, (struct sockaddr *)&me, sizeof(me)) != 10) {
        close(fd);

        return fail(DGRAM_SEND);
    }

    parts[0].iov_base = one;
    parts[0].iov_len = sizeof(one);
    parts[1].iov_base = two;
    parts[1].iov_len = sizeof(two);
    parts[2].iov_base = three;
    parts[2].iov_len = sizeof(three);
    got = readv(fd, parts, 3);
    if (got != 10) {
        close(fd);

        return fail(DGRAM_FILL);
    }
    if (memcmp(one, "AAAA", 4) != 0 || memcmp(two, "BBBB", 4) != 0 || memcmp(three, "CC", 2) != 0) {
        close(fd);

        return fail(DGRAM_ORDER);
    }

    /* A datagram longer than the buffers. */
    if (sendto(fd, "0123456789", 10, 0, (struct sockaddr *)&me, sizeof(me)) != 10) {
        close(fd);

        return fail(DGRAM_SEND_2);
    }
    parts[1].iov_len = 2;
    got = readv(fd, parts, 2);
    if (got != 6) {
        close(fd);

        return fail(DGRAM_TRUNCATE);
    }
    if (memcmp(one, "0123", 4) != 0 || memcmp(two, "45", 2) != 0) {
        close(fd);

        return fail(DGRAM_TRUNCATED_BYTES);
    }

    /* A buffer that aliases the vector list. */
    {
        struct iovec alias[3];
        struct iovec forged = {(void *)0x30000000, 4};
        unsigned char payload[4 + sizeof(struct iovec) + 8];
        char sink[4];

        memcpy(payload, "AAAA", 4);
        memcpy(payload + 4, &forged, sizeof(forged));
        memcpy(payload + 4 + sizeof(forged), "BBBBCCCC", 8);

        if (sendto(fd, payload, sizeof(payload), 0, (struct sockaddr *)&me, sizeof(me)) !=
            (ssize_t)sizeof(payload)) {
            close(fd);

            return fail(DGRAM_SEND_ALIAS);
        }
        alias[0].iov_base = sink;
        alias[0].iov_len = sizeof(sink);
        alias[1].iov_base = &alias[2]; /* the next descriptor itself */
        alias[1].iov_len = sizeof(alias[2]);
        alias[2].iov_base = sink;
        alias[2].iov_len = sizeof(sink);
        got = readv(fd, alias, 3);
        if (got < 4) {
            close(fd);

            return fail(DGRAM_ALIAS);
        }
    }

    /* The tail went with its message, so the next read is the next message. */
    if (sendto(fd, "TAIL", 4, 0, (struct sockaddr *)&me, sizeof(me)) != 4) {
        close(fd);

        return fail(DGRAM_SEND_3);
    }
    parts[0].iov_len = sizeof(one);
    parts[1].iov_len = sizeof(two);
    got = readv(fd, parts, 2);
    close(fd);
    if (got != 4 || memcmp(one, "TAIL", 4) != 0) {
        return fail(DGRAM_NEXT);
    }

    return 0;
}

/**
 * Check a terminal, which returns one line however many buffers are given.
 * Returns 0, or 1 once the failure is reported.
 *
 * The line is the unit a terminal reads in, so a program that asked for one
 * line and got two would have consumed input it will not look at. Two lines
 * are written into the master with two writes before either is read, so both
 * are complete and waiting, and the first buffer is exactly the size of the
 * first line: only the rule of one line to a read stops the read from going
 * on to the second.
 */
static int check_terminal(void)
{
    struct iovec parts[2];
    char one[4];
    char two[8];
    ssize_t got;
    int master;
    int slave;

    master = posix_openpt(O_RDWR | O_NOCTTY);
    if (master < 0) {
        return fail(PTY_MASTER);
    }
    if (grantpt(master) != 0 || unlockpt(master) != 0) {
        close(master);

        return fail(PTY_UNLOCK);
    }
    slave = open(ptsname(master), O_RDWR | O_NOCTTY);
    if (slave < 0) {
        close(master);

        return fail(PTY_SLAVE);
    }
    if (write(master, "one\n", 4) != 4 || write(master, "two\n", 4) != 4) {
        close(slave);
        close(master);

        return fail(PTY_WRITE);
    }

    parts[0].iov_base = one;
    parts[0].iov_len = sizeof(one);
    parts[1].iov_base = two;
    parts[1].iov_len = sizeof(two);
    got = readv(slave, parts, 2);
    /* Closing the master hangs up the slave and sends SIGHUP to this group. */
    signal(SIGHUP, SIG_IGN);
    close(slave);
    close(master);
    signal(SIGHUP, SIG_DFL);
    if (got != 4) {
        return fail(TTY_ONE_LINE);
    }
    if (memcmp(one, "one\n", 4) != 0) {
        return fail(TTY_LINE);
    }

    return 0;
}

/**
 * Check the edges, where the call fails without taking input. Returns 0, or 1
 * once the failure is reported.
 *
 * A bad address in the second buffer fails the call, and the bytes the first
 * buffer would have taken are still there afterwards. There are two kinds of
 * bad address: one below the user window, and one inside it that no mapping
 * backs, which a range check alone would let through and only a write to it
 * would find, by which time the input would be gone.
 */
static int check_edges(void)
{
    struct iovec parts[2];
    char one[4];
    char rest[8];
    ssize_t got;
    int fd;

    parts[0].iov_base = one;
    parts[0].iov_len = sizeof(one);

    /* A bad address in the second buffer, below the window and inside it. */
    fd = pipe_holding("KEEPTHIS");
    if (fd < 0) {
        return fail(REFUSAL_PIPE);
    }
    parts[1].iov_base = (void *)0x10;
    parts[1].iov_len = sizeof(rest);
    if (readv(fd, parts, 2) != -1 || errno != EFAULT) {
        close(fd);

        return fail(OUTSIDE_WINDOW);
    }
    parts[1].iov_base = (void *)0x30000000;
    if (readv(fd, parts, 2) != -1 || errno != EFAULT) {
        close(fd);

        return fail(UNMAPPED);
    }
    if (read(fd, rest, sizeof(rest)) != 8 || memcmp(rest, "KEEPTHIS", 8) != 0) {
        close(fd);

        return fail(INPUT_KEPT);
    }
    close(fd);

    /* Asking for nothing still says whether the descriptor was any good. */
    parts[0].iov_len = 0;
    parts[1].iov_base = rest;
    parts[1].iov_len = 0;
    fd = pipe_holding("Q");
    if (fd < 0) {
        return fail(EDGE_PIPE);
    }
    got = readv(fd, parts, 2);
    if (got != 0) {
        close(fd);

        return fail(EMPTY_BUFFERS);
    }
    close(fd);
    if (readv(fd, parts, 2) != -1 || errno != EBADF) {
        return fail(CLOSED_FD);
    }

    parts[0].iov_len = sizeof(one);
    fd = pipe_holding("Q");
    if (fd < 0) {
        return fail(COUNT_PIPE);
    }
    if (readv(fd, parts, 0) != 0) {
        close(fd);

        return fail(NO_BUFFERS);
    }
    if (readv(fd, parts, -1) != -1 || errno != EINVAL) {
        close(fd);

        return fail(NEGATIVE_COUNT);
    }
    close(fd);

    return 0;
}

/**
 * Check a destination shared by a fork, where the check before the read has
 * something to do. Returns 0, or 1 once the failure is reported.
 *
 * Every page a child inherits is present, read-only and marked copy-on-write
 * until it is written. `readv` checks that each destination is writable
 * before it takes a byte, and a check that only asked whether the page was
 * present would pass a page nothing may write to yet. The child reads into a
 * buffer it has not touched since the fork, and the bytes must arrive. The
 * buffer is the middle page of three, since a buffer sharing a page with
 * anything else the child writes would be made private by that write before
 * the read.
 */
static int check_shared(void)
{
    struct iovec parts[2];
    /* Three pages, so the middle one can be used alone. */
    static char arena[3 * 4096];
    char *inherited = arena + (4096 - ((unsigned long)arena % 4096));
    int status = 0;
    int child;
    int fd;

    memset(inherited, 'x', 4096);
    child = fork();
    if (child < 0) {
        return fail(SHARED_FORK);
    }
    if (child == 0) {
        fd = file_holding("COPYONWRITE");
        if (fd < 0) {
            _exit(2);
        }
        parts[0].iov_base = inherited;
        parts[0].iov_len = 11;
        parts[1].iov_base = inherited + 2048;
        parts[1].iov_len = 16;
        if (readv(fd, parts, 2) != 11) {
            _exit(3);
        }
        close(fd);
        _exit(memcmp(inherited, "COPYONWRITE", 11) == 0 ? 0 : 4);
    }
    if (waitpid(child, &status, 0) != child) {
        return fail(SHARED_WAIT);
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        return fail(SHARED_PAGE);
    }
    unlink(SCRATCH);

    return 0;
}

/**
 * Run the cases in order, stopping at the first failure.
 */
int main(void)
{
    int rc;

    report_begin(report_case(environ, "guest.default.readvmusl"));
    rc = check_buffered();

    if (rc == 0) {
        rc = check_file();
    }
    if (rc == 0) {
        rc = check_stream();
    }
    if (rc == 0) {
        rc = check_edges();
    }
    if (rc == 0) {
        rc = check_datagram();
    }
    if (rc == 0) {
        rc = check_terminal();
    }
    if (rc == 0) {
        rc = check_shared();
    }
    if (rc == 0) {
        report_at(CHECKS);
        printf("READVMUSL OK\n");
    }

    return rc;
}
