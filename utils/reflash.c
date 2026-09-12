/**
 * Write a stream onto a raw device: `reflash DEVICE OFFSET BYTES`.
 *
 * This is how a machine nobody will open again gets a new disk. The image
 * arrives on standard input, which is what makes `ssh` the whole transport and
 * this program the whole receiver:
 *
 * gzip -c root.img | ssh rach2 'gunzip | reflash /dev/disk0 68157440 524288000'
 *
 * Both numbers come off the machine, not out of a memory: `/proc/disk`
 * reports each partition as `partN TYPE START_LBA SECTORS`, and the two above
 * are the sealed disk's root, 133120 and 1024000, multiplied by 512. Writing a
 * correct image at a wrong offset is a repair that reports success and leaves a
 * machine that still will not boot.
 *
 * The length is an argument and not something to be discovered, because a
 * stream's length is not known until it ends: a tool that read first and
 * checked afterwards would already have written part of a disk it was never
 * allowed to touch. With the count declared, where the write would land is
 * settled before a byte is read.
 *
 * `-p` prints how far the write has got, on standard error, every sixteen
 * mebibytes and once at the end: a person watching an install of a gibibyte
 * and a half over a serial console has nothing else to look at, and a log of
 * one line an hour says nothing about whether a stalled write is stalled.
 *
 * `--verify` writes nothing. It reads the same range back from the device and
 * prints its SHA-256, which is how an installer says what reached the disk
 * rather than what left the host. The digest is taken on the machine that did
 * the writing, through the same window the write went through, so a wrong
 * offset shows up as a wrong digest.
 *
 * Only a kernel built for recovery may write the whole disk; on any other
 * the device refuses, and this reports that refusal, not a partial
 * write.
 */

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "sectorcopy.h"
#include "sha256.h"

static int devfd = -1;

/** Read from standard input, retrying an interrupted call. */
static long stream_read(void *ctx, unsigned char *buf, unsigned long n)
{
    (void)ctx;
    for (;;) {
        ssize_t r = read(STDIN_FILENO, buf, (size_t)n);

        if (r < 0 && errno == EINTR) {
            continue;
        }
        return (long)r;
    }
}

/** Write to the device, retrying an interrupted call. */
static long device_write(void *ctx, const unsigned char *buf, unsigned long n)
{
    (void)ctx;
    for (;;) {
        ssize_t w = write(devfd, buf, (size_t)n);

        if (w < 0 && errno == EINTR) {
            continue;
        }
        return (long)w;
    }
}

/** The progress line, on standard error so a piped digest stays a digest. */
static void note(void *ctx, unsigned long long done, unsigned long long total, unsigned percent)
{
    (void)ctx;
    fprintf(stderr, "reflash: %llu of %llu bytes, %u%%\n", done, total, percent);
}

/**
 * Read `count` bytes at `offset` back from the open device and print their
 * digest.
 *
 * The read is in the same units the write used, and a short read is a failure:
 * a device that stops answering part way through must not yield the digest of
 * what it did give.
 */
static int verify(const char *path, unsigned long long offset, unsigned long long count, int show)
{
    static unsigned char buf[1024 * 1024];
    struct sha256 sha;
    unsigned long long done = 0, last = 0;
    unsigned char digest[32];
    char hex[65];
    unsigned percent;

    if (lseek(devfd, (off_t)offset, SEEK_SET) < 0) {
        fprintf(stderr, "reflash: cannot seek %s to %llu: %s\n", path, offset, strerror(errno));
        return 1;
    }
    sha256_init(&sha);
    while (done < count) {
        unsigned long long left = count - done;
        size_t want = sizeof(buf);
        ssize_t n;

        if ((unsigned long long)want > left) {
            want = (size_t)left;
        }
        n = read(devfd, buf, want);
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n <= 0) {
            fprintf(stderr, "reflash: %s stopped answering after %llu of %llu bytes\n", path, done,
                    count);
            return 1;
        }
        sha256_update(&sha, buf, (size_t)n);
        done += (unsigned long long)n;
        if (show && sectorcopy_step(done, count, &last, &percent)) {
            fprintf(stderr, "reflash: read %llu of %llu bytes, %u%%\n", done, count, percent);
        }
    }
    sha256_final(&sha, digest);
    sha256_hex(digest, hex);
    printf("reflash: sha256 %s of %llu bytes at %llu\n", hex, count, offset);
    return 0;
}

int main(int argc, char **argv)
{
    struct sectorcopy_io io;
    unsigned long long offset, count, device, written = 0;
    char err[96];
    off_t end;
    const char *args[3];
    const char *path;
    int nargs = 0, show = 0, reading = 0, i;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-p") == 0) {
            show = 1;
        } else if (strcmp(argv[i], "--verify") == 0) {
            reading = 1;
        } else if (nargs < 3) {
            args[nargs++] = argv[i];
        } else {
            nargs++;
        }
    }
    if (nargs != 3) {
        fprintf(stderr, "usage: reflash [-p] [--verify] DEVICE OFFSET BYTES\n");
        return 2;
    }
    if (sectorcopy_parse_u64(args[1], &offset) != 0) {
        fprintf(stderr, "reflash: '%s' is not an offset\n", args[1]);
        return 2;
    }
    if (sectorcopy_parse_u64(args[2], &count) != 0) {
        fprintf(stderr, "reflash: '%s' is not a length\n", args[2]);
        return 2;
    }
    path = args[0];

    devfd = open(path, reading ? O_RDONLY : O_WRONLY);
    if (devfd < 0) {
        fprintf(stderr, "reflash: cannot open %s: %s\n", path, strerror(errno));
        return 1;
    }
    /* The device's size, from the device: a window that ends where the disk
       ends answers SEEK_END with its length. */
    end = lseek(devfd, 0, SEEK_END);
    if (end < 0) {
        fprintf(stderr, "reflash: cannot measure %s: %s\n", path, strerror(errno));
        close(devfd);
        return 1;
    }
    device = (unsigned long long)end;
    if (sectorcopy_bounds(device, offset, count, err, sizeof(err)) != 0) {
        fprintf(stderr, "reflash: %s (%s has %llu bytes)\n", err, path, device);
        close(devfd);
        return 1;
    }
    if (reading) {
        int rc = verify(path, offset, count, show);

        close(devfd);
        return rc;
    }
    if (lseek(devfd, (off_t)offset, SEEK_SET) < 0) {
        fprintf(stderr, "reflash: cannot seek %s to %llu: %s\n", path, offset, strerror(errno));
        close(devfd);
        return 1;
    }

    io.read = stream_read;
    io.write = device_write;
    io.note = show ? note : 0;
    io.ctx = NULL;
    if (sectorcopy_run(&io, count, &written, err, sizeof(err)) != 0) {
        fprintf(stderr, "reflash: %s (%llu of %llu bytes written at %llu)\n", err, written, count,
                offset);
        close(devfd);
        return 1;
    }
    if (close(devfd) != 0) {
        fprintf(stderr, "reflash: %s did not close cleanly: %s\n", path, strerror(errno));
        return 1;
    }
    printf("reflash: wrote %llu bytes to %s at %llu\n", written, path, offset);
    return 0;
}
