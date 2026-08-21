/**
 * Copying a declared number of bytes from a stream onto a raw device.
 *
 * This is the half of `reflash` that needs no device and no descriptor: the
 * numbers, and the loop that turns a stream which hands out whatever it has
 * into writes of whole sectors. It is separate because the numbers decide
 * whether a write lands inside the disk or past its end, and because the loop
 * has to be driven with a stream that stops early, one that hands out a byte at
 * a time, and one that has more than it said, none of which an `ssh` pipe
 * can be asked to do on request. `tests/host/sectorcopy_hosttest.c` drives it
 * with all of them.
 */
#ifndef SECTORCOPY_H
#define SECTORCOPY_H

#include <stddef.h>

/* The unit every raw device here takes and gives: a partial one is refused. */
#define SECTORCOPY_SECTOR 512

/** Where the bytes come from and where they go. */
struct sectorcopy_io {
    /* Read up to `n` bytes: the count read, 0 at the end of the stream, or a
     * negative number for a failure, like read(2). */
    long (*read)(void *ctx, unsigned char *buf, unsigned long n);
    /* Write `n` bytes at the device's current position, which this advances:
     * the count written, or a negative number, like write(2). A short count is
     * not an error here; the loop calls again. */
    long (*write)(void *ctx, const unsigned char *buf, unsigned long n);
    void *ctx;
};

int sectorcopy_parse_u64(const char *text, unsigned long long *out);
int sectorcopy_bounds(unsigned long long device, unsigned long long offset,
                      unsigned long long count, char *err, size_t errlen);
int sectorcopy_run(const struct sectorcopy_io *io, unsigned long long count,
                   unsigned long long *written, char *err, size_t errlen);

#endif /* SECTORCOPY_H */
