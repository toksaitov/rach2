/**
 * Write a stream onto a raw device: `reflash DEVICE OFFSET BYTES`.
 *
 * This is how a machine nobody will open again gets a new disk. The image
 * arrives on standard input, which is what makes `ssh` the whole transport and
 * this program the whole receiver:
 *
 *     gzip -c root.img | ssh rach2 'gunzip | reflash /dev/disk0 68157440 524288000'
 *
 * Both numbers come off the machine rather than out of a memory: `/proc/disk`
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
 * Only a kernel built for recovery may write the whole disk; on any other the
 * device refuses, and this reports that refusal rather than a partial write.
 */

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "sectorcopy.h"

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

int main(int argc, char **argv)
{
    struct sectorcopy_io io;
    unsigned long long offset, count, device, written = 0;
    char err[96];
    off_t end;

    if (argc != 4) {
        fprintf(stderr, "usage: reflash DEVICE OFFSET BYTES\n");
        return 2;
    }
    if (sectorcopy_parse_u64(argv[2], &offset) != 0) {
        fprintf(stderr, "reflash: '%s' is not an offset\n", argv[2]);
        return 2;
    }
    if (sectorcopy_parse_u64(argv[3], &count) != 0) {
        fprintf(stderr, "reflash: '%s' is not a length\n", argv[3]);
        return 2;
    }

    devfd = open(argv[1], O_WRONLY);
    if (devfd < 0) {
        fprintf(stderr, "reflash: cannot open %s: %s\n", argv[1], strerror(errno));
        return 1;
    }
    /* The device's size, from the device: a window that ends where the disk
       ends answers SEEK_END with its length. */
    end = lseek(devfd, 0, SEEK_END);
    if (end < 0) {
        fprintf(stderr, "reflash: cannot measure %s: %s\n", argv[1], strerror(errno));
        close(devfd);
        return 1;
    }
    device = (unsigned long long)end;
    if (sectorcopy_bounds(device, offset, count, err, sizeof(err)) != 0) {
        fprintf(stderr, "reflash: %s (%s holds %llu bytes)\n", err, argv[1], device);
        close(devfd);
        return 1;
    }
    if (lseek(devfd, (off_t)offset, SEEK_SET) < 0) {
        fprintf(stderr, "reflash: cannot seek %s to %llu: %s\n", argv[1], offset, strerror(errno));
        close(devfd);
        return 1;
    }

    io.read = stream_read;
    io.write = device_write;
    io.ctx = NULL;
    if (sectorcopy_run(&io, count, &written, err, sizeof(err)) != 0) {
        fprintf(stderr, "reflash: %s (%llu of %llu bytes written at %llu)\n", err, written, count,
                offset);
        close(devfd);
        return 1;
    }
    if (close(devfd) != 0) {
        fprintf(stderr, "reflash: %s did not close cleanly: %s\n", argv[1], strerror(errno));
        return 1;
    }
    printf("reflash: wrote %llu bytes to %s at %llu\n", written, argv[1], offset);
    return 0;
}
