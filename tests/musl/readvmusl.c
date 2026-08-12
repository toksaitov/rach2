/**
 * Buffered reading, which is `readv` seen from where a program stands.
 *
 * musl's stdio fills its buffer with `readv` rather than `read` whenever it
 * wants more than a single byte, so before this kernel had that call every
 * program reading through `fgets`, `fread` or `scanf` failed, and several
 * programs in this tree read a byte at a time to avoid it. busybox's `chpasswd`
 * is the one that forced the issue: it reads its input with `xmalloc_fgetline`
 * and exited without a message.
 *
 * The call has two shapes here, and the probe pins both. A regular file is
 * filled buffer after buffer, since its read returns at once and its bytes
 * carry no message boundary. Anything else is read exactly once, because a
 * second read of a pipe whose first buffer happened to fill exactly would block
 * for input nobody has written, and a second read of a datagram socket would
 * take the next datagram after the first one's tail was dropped. A reader that
 * asked for more than it got must go round again, which is what musl's stdio
 * does and what the buffered cases here demonstrate.
 *
 * The rest is the edges: the whole vector list is checked before a byte is
 * taken, so a bad address in a later buffer refuses the call with the input
 * still queued; a list that asks for nothing still checks the descriptor; and a
 * negative count is refused.
 *
 * Prints "READVMUSL OK" on success and "READVMUSL FAIL (detail)" otherwise.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/uio.h>

#define SCRATCH "/readvmusl.tmp"

/** Report one failed check and return 1, the probe's failure status. */
static int fail(const char *what)
{
    printf("READVMUSL FAIL (%s)\n", what);

    return 1;
}

/**
 * Make a pipe holding `text`, returning its read end, or -1.
 *
 * The writing end is closed before the caller reads, so a reader that asks for
 * more than was written sees end of input rather than blocking for a writer
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
 * Make a file holding `text`, returning a descriptor open for reading, or -1.
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
 * The buffered reads, which is what the kernel call is for.
 */
static int check_buffered(void)
{
    char line[64];
    char block[16];
    FILE *stream;
    int fd;

    fd = pipe_holding("a line through stdio\nsecond\n");
    if (fd < 0) {
        return fail("could not make the pipe the buffered read uses");
    }
    stream = fdopen(fd, "r");
    if (!stream) {
        close(fd);

        return fail("fdopen");
    }
    if (!fgets(line, sizeof(line), stream)) {
        fclose(stream);

        return fail("fgets read nothing, which is the defect this probe exists for");
    }
    if (strcmp(line, "a line through stdio\n") != 0) {
        fclose(stream);

        return fail("fgets returned something other than the line written");
    }
    if (!fgets(line, sizeof(line), stream) || strcmp(line, "second\n") != 0) {
        fclose(stream);

        return fail("the second buffered line did not arrive");
    }
    fclose(stream);

    fd = pipe_holding("0123456789abcdef");
    if (fd < 0) {
        return fail("could not make the pipe the block read uses");
    }
    stream = fdopen(fd, "r");
    if (!stream) {
        close(fd);

        return fail("fdopen for the block read");
    }
    if (fread(block, 1, sizeof(block), stream) != sizeof(block)) {
        fclose(stream);

        return fail("fread did not return the bytes that were written");
    }
    fclose(stream);
    if (memcmp(block, "0123456789abcdef", sizeof(block)) != 0) {
        return fail("fread returned the wrong bytes");
    }

    return 0;
}

/**
 * A regular file, which is the one thing filled buffer after buffer.
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
        return fail("could not make the file the walk uses");
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
        return fail("readv across three buffers of a file did not return the total");
    }
    if (memcmp(one, "AAAA", 4) != 0 || memcmp(two, "BBBB", 4) != 0 ||
        memcmp(three, "CCCC", 4) != 0) {
        return fail("readv filled the buffers out of order");
    }

    /* A zero-length buffer in the middle is skipped, not treated as an end. */
    fd = file_holding("XXXXYYYY");
    if (fd < 0) {
        return fail("could not make the file the empty-buffer case uses");
    }
    parts[1].iov_len = 0;
    got = readv(fd, parts, 3);
    close(fd);
    if (got != 8) {
        return fail("a zero-length buffer stopped the walk");
    }
    if (memcmp(one, "XXXX", 4) != 0 || memcmp(three, "YYYY", 4) != 0) {
        return fail("the buffers around the empty one hold the wrong bytes");
    }

    /* End of file stops the walk rather than reporting an error. */
    fd = file_holding("ZZZZ");
    if (fd < 0) {
        return fail("could not make the file the short case uses");
    }
    parts[1].iov_len = sizeof(two);
    got = readv(fd, parts, 3);
    close(fd);
    if (got != 4) {
        return fail("end of file did not stop the walk");
    }

    unlink(SCRATCH);

    return 0;
}

/**
 * A pipe, which is read exactly once however many buffers were offered.
 *
 * The case that matters is the second one: the pipe holds exactly what the
 * first buffer takes and its writer is still open, so a second read would have
 * nothing to return and would park until something wrote again. If that
 * regressed this probe would not fail, it would hang, and the suite's timeout
 * is the failure.
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
        return fail("could not make the pipe the single read uses");
    }
    parts[0].iov_base = one;
    parts[0].iov_len = sizeof(one);
    parts[1].iov_base = two;
    parts[1].iov_len = sizeof(two);
    got = readv(fd, parts, 2);
    close(fd);
    if (got != 4) {
        return fail("readv of a pipe read past its first buffer");
    }
    if (memcmp(one, "AAAA", 4) != 0) {
        return fail("readv of a pipe filled the wrong buffer");
    }

    if (pipe(ends) != 0) {
        return fail("could not make the pipe the live-writer case uses");
    }
    if (write(ends[1], "LIVE", 4) != 4) {
        close(ends[0]);
        close(ends[1]);

        return fail("could not write the live-writer pipe");
    }
    got = readv(ends[0], parts, 2);
    close(ends[0]);
    close(ends[1]);
    if (got != 4) {
        return fail("readv did not stop at the bytes a live writer had written");
    }

    return 0;
}

/**
 * The edges, which are about refusing without taking anything.
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

    /* A bad address in the second buffer refuses the call, and the bytes the
     * first buffer would have taken are still there afterwards. */
    fd = pipe_holding("KEEPTHIS");
    if (fd < 0) {
        return fail("could not make the pipe the refusal case uses");
    }
    parts[1].iov_base = (void *)0x10;
    parts[1].iov_len = sizeof(rest);
    if (readv(fd, parts, 2) != -1 || errno != EFAULT) {
        close(fd);

        return fail("a bad address in a later buffer was not refused");
    }
    if (read(fd, rest, sizeof(rest)) != 8 || memcmp(rest, "KEEPTHIS", 8) != 0) {
        close(fd);

        return fail("the refused readv had already taken the input");
    }
    close(fd);

    /* Asking for nothing still says whether the descriptor was any good. */
    parts[0].iov_len = 0;
    parts[1].iov_base = rest;
    parts[1].iov_len = 0;
    fd = pipe_holding("Q");
    if (fd < 0) {
        return fail("could not make the pipe the edge cases use");
    }
    got = readv(fd, parts, 2);
    if (got != 0) {
        close(fd);

        return fail("readv of empty buffers did not return 0");
    }
    close(fd);
    if (readv(fd, parts, 2) != -1 || errno != EBADF) {
        return fail("readv of empty buffers on a closed fd was not refused");
    }

    parts[0].iov_len = sizeof(one);
    fd = pipe_holding("Q");
    if (fd < 0) {
        return fail("could not make the pipe the count case uses");
    }
    if (readv(fd, parts, 0) != 0) {
        close(fd);

        return fail("readv of no buffers did not return 0");
    }
    if (readv(fd, parts, -1) != -1 || errno != EINVAL) {
        close(fd);

        return fail("a negative count was not refused with EINVAL");
    }
    close(fd);

    return 0;
}

/**
 * Run the cases and report.
 */
int main(void)
{
    int rc = check_buffered();

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
        printf("READVMUSL OK\n");
    }

    return rc;
}
