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
 * The cases run against a pipe, which is where a program's input usually comes
 * from when something is feeding it:
 *
 * `fgets` returns the line that was written, which is the case that was broken.
 * `fread` of a known length returns exactly that many bytes. A `readv` issued
 * directly across three buffers fills them in order and returns the total, and
 * one with a zero-length middle buffer skips it rather than stopping. A short
 * read stops the walk, since a pipe holding four bytes cannot fill eight.
 * `readv` of nothing returns 0, and a negative count is refused with -EINVAL.
 *
 * Prints "READVMUSL OK" on success and "READVMUSL FAIL (detail)" otherwise.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/uio.h>

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
 * that will never write again.
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
 * The call itself, at its edges.
 */
static int check_readv(void)
{
    struct iovec parts[3];
    char one[4];
    char two[4];
    char three[4];
    ssize_t got;
    int fd;

    fd = pipe_holding("AAAABBBBCCCC");
    if (fd < 0) {
        return fail("could not make the pipe readv uses");
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
        return fail("readv across three buffers did not return the total");
    }
    if (memcmp(one, "AAAA", 4) != 0 || memcmp(two, "BBBB", 4) != 0 ||
        memcmp(three, "CCCC", 4) != 0) {
        return fail("readv filled the buffers out of order");
    }

    /* A zero-length buffer in the middle is skipped, not treated as an end. */
    fd = pipe_holding("XXXXYYYY");
    if (fd < 0) {
        return fail("could not make the pipe the empty-buffer case uses");
    }
    parts[0].iov_base = one;
    parts[0].iov_len = sizeof(one);
    parts[1].iov_base = two;
    parts[1].iov_len = 0;
    parts[2].iov_base = three;
    parts[2].iov_len = sizeof(three);
    got = readv(fd, parts, 3);
    close(fd);
    if (got != 8) {
        return fail("a zero-length buffer stopped the walk");
    }
    if (memcmp(one, "XXXX", 4) != 0 || memcmp(three, "YYYY", 4) != 0) {
        return fail("the buffers around the empty one hold the wrong bytes");
    }

    /* Less input than the buffers can hold stops the walk rather than blocking
     * for a writer that has already gone. */
    fd = pipe_holding("ZZZZ");
    if (fd < 0) {
        return fail("could not make the pipe the short case uses");
    }
    parts[0].iov_base = one;
    parts[0].iov_len = sizeof(one);
    parts[1].iov_base = two;
    parts[1].iov_len = sizeof(two);
    got = readv(fd, parts, 2);
    close(fd);
    if (got != 4) {
        return fail("a short read did not stop the walk");
    }

    fd = pipe_holding("Q");
    if (fd < 0) {
        return fail("could not make the pipe the edge cases use");
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
        rc = check_readv();
    }
    if (rc == 0) {
        printf("READVMUSL OK\n");
    }

    return rc;
}
