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

/* The unit every raw device here takes and gives: a partial one fails. */
#define SECTORCOPY_SECTOR 512

/* How far apart the progress lines are. Sixteen mebibytes is a line every few
 * seconds on the transports this runs over, which is often enough to show that
 * a write is moving and rare enough that the lines are a small part of what
 * the transport carries. */
#define SECTORCOPY_STEP (16ULL * 1024 * 1024)

/** Where the bytes come from and where they go. */
struct sectorcopy_io {
    /* Read up to `n` bytes: the count read, 0 at the end of the stream, or a
     * negative number for a failure, like read(2). */
    long (*read)(void *ctx, unsigned char *buf, unsigned long n);
    /* Write `n` bytes at the device's current position, which this advances:
     * the count written, or a negative number, like write(2). A short count is
     * not an error here; the loop calls again. */
    long (*write)(void *ctx, const unsigned char *buf, unsigned long n);
    /* Called as the copy passes each step and once at its end; NULL prints no
     * progress at all. `done` is the bytes that have reached the device,
     * `total` the whole count. */
    void (*note)(void *ctx, unsigned long long done, unsigned long long total, unsigned percent);
    void *ctx;
};

/**
 * Whether a progress line is due, and the percentage it would carry.
 *
 * A line is due when the copy has passed a step boundary since the last one,
 * and again at the end, so a copy shorter than one step still prints the line
 * that reports its end. A count that has not moved since the last line is not
 * a second line. `*last` is the count the last line reported, zero at the
 * start.
 *
 * The percentage is `done * 100 / total`, so `total` is bounded by a hundredth
 * of what 64 bits hold, which is 184 petabytes: past every medium this writes.
 *
 * @return 1 when a line is due, with `*last` moved to `done` and `*percent`
 *         set; 0 when it is not, and neither is touched.
 */
int sectorcopy_step(unsigned long long done, unsigned long long total, unsigned long long *last,
                    unsigned *percent);

/**
 * Read a decimal number that has to fit 64 bits.
 *
 * Nothing here is signed: an offset into a disk and a count of bytes are both
 * quantities, and a minus sign in either is a mistake to report, not a large
 * number to wrap into. A number too large for 64 bits fails rather than being
 * reduced, and so does an empty text.
 *
 * @return 0 when `out` has the number, -1 when the text is not one.
 */
int sectorcopy_parse_u64(const char *text, unsigned long long *out);

/**
 * Judge where a write of `count` bytes at `offset` of a device of `device`
 * bytes would land, before a byte of it is read.
 *
 * The length is taken as an argument so that this can be answered first: a
 * stream's length is not known until it ends, so a tool that read first and
 * checked afterwards would already have written part of a disk it was never
 * allowed to touch. The offset and the length must be whole numbers of
 * sectors, the length nonzero, and the range inside a device that reports a
 * size.
 *
 * @return 0 when the write fits, -1 with the reason in `err`.
 */
int sectorcopy_bounds(unsigned long long device, unsigned long long offset,
                      unsigned long long count, char *err, size_t errlen);

/**
 * Copy exactly `count` bytes from the stream to the device through `io`.
 *
 * Only whole sectors are written. The stream must end exactly where it said
 * it would. A short one leaves the disk holding part of an image and the rest
 * of what was there before, which is worse than either, so it is a failure
 * and says how far it got; the whole sectors gathered before it ended are
 * written first, so the count reported is what the disk holds. A longer one
 * means the length this was told is not the length of the thing being
 * written, and the bytes past the count are not written at all: after the
 * count one more read must find the end of the stream, and one that fails is
 * a failure too, since it proves nothing either way.
 *
 * @param written set to the number of bytes written, on success and failure
 *                alike, so a caller can say how far a broken transfer got.
 * @return 0 when the whole count reached the device, -1 with the reason in
 *         `err`.
 */
int sectorcopy_run(const struct sectorcopy_io *io, unsigned long long count,
                   unsigned long long *written, char *err, size_t errlen);

#endif /* SECTORCOPY_H */
