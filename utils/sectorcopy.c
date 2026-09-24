/**
 * Copying a declared number of bytes onto a raw device. See sectorcopy.h.
 */

#include <string.h>

#include "sectorcopy.h"

/* Two thousand and forty-eight sectors, a mebibyte, the most the block layer
 * moves in one request: a write is gathered to that before it goes, so a
 * stream that arrives in small pieces still reaches the disk in large
 * requests. */
#define CHUNK (2048 * SECTORCOPY_SECTOR)

/** Put a refusal in the caller's buffer, if it asked for one. */
static void say(char *err, size_t errlen, const char *what)
{
    if (err && errlen > 0) {
        strncpy(err, what, errlen - 1);
        err[errlen - 1] = '\0';
    }
}

/**
 * Read a decimal number that has to fit 64 bits.
 *
 * Nothing here is signed: an offset into a disk and a count of bytes are both
 * quantities, and a minus sign in either is a mistake to report, not a large
 * number to wrap into. Overflow is caught before it happens, so a number too
 * large for 64 bits is refused, not silently reduced. An empty text is
 * refused as well.
 *
 * @return 0 when `out` has the number, -1 when the text is not one.
 */
int sectorcopy_parse_u64(const char *text, unsigned long long *out)
{
    unsigned long long v = 0;
    int digits = 0;

    if (!text) {
        return -1;
    }
    for (; *text != '\0'; text++) {
        unsigned digit;

        if (*text < '0' || *text > '9') {
            return -1;
        }
        digit = (unsigned)(*text - '0');
        if (v > (0xFFFFFFFFFFFFFFFFull - digit) / 10ull) {
            return -1;
        }
        v = v * 10ull + digit;
        digits++;
    }
    if (digits == 0) {
        return -1;
    }
    *out = v;
    return 0;
}

/**
 * Judge where the write would land, before a byte of it is read.
 *
 * The length is taken as an argument so that this can be answered first: a
 * stream's length is not known until it ends, so a tool that read first and
 * checked afterwards would already have written part of a disk it was never
 * allowed to touch. The offset and the length must be whole numbers of
 * sectors, the length nonzero, and the range inside a device that reports a
 * size. Every comparison is a subtraction rather than an addition, since
 * `offset + count` can wrap for numbers the parser accepts.
 *
 * @return 0 when the write fits, -1 with the reason in `err`.
 */
int sectorcopy_bounds(unsigned long long device, unsigned long long offset,
                      unsigned long long count, char *err, size_t errlen)
{
    if (count == 0) {
        say(err, errlen, "a length of zero writes nothing");
        return -1;
    }
    if ((offset % SECTORCOPY_SECTOR) != 0) {
        say(err, errlen, "the offset is not a whole number of sectors");
        return -1;
    }
    if ((count % SECTORCOPY_SECTOR) != 0) {
        say(err, errlen, "the length is not a whole number of sectors");
        return -1;
    }
    if (device == 0) {
        say(err, errlen, "the device reports no size");
        return -1;
    }
    if (offset >= device) {
        say(err, errlen, "the offset is past the end of the device");
        return -1;
    }
    if (count > device - offset) {
        say(err, errlen, "the write would run past the end of the device");
        return -1;
    }
    return 0;
}

int sectorcopy_step(unsigned long long done, unsigned long long total, unsigned long long *last,
                    unsigned *percent)
{
    if (done != total && done / SECTORCOPY_STEP == *last / SECTORCOPY_STEP) {
        return 0;
    }
    if (done == *last && done != 0) {
        return 0;
    }
    *last = done;
    *percent = total ? (unsigned)(done * 100 / total) : 100;
    return 1;
}

/**
 * Copy exactly `count` bytes from the stream to the device.
 *
 * A read from a pipe returns whatever has arrived, and a raw device takes whole
 * sectors, so the two are joined here, not passed through to each other:
 * bytes accumulate until there is a full `CHUNK` or the count is reached, and
 * only whole sectors are written. The stream must end exactly where it said it
 * would. A short one leaves the disk holding part of an image and the rest of
 * what was there before, which is worse than either, so it is a failure and
 * says how far it got; the whole sectors gathered before it ended are written
 * first, so the count reported is what the disk holds. A longer one means the
 * length this was told is not the length of the thing being written, and the
 * bytes past the count are not written at all: after the count one more read
 * must find the end of the stream, and one that fails is a failure too, since
 * it proves nothing either way.
 *
 * @param written set to the number of bytes written, on success and failure
 *                alike, so a caller can say how far a broken transfer got.
 * @return 0 when the whole count reached the device.
 */
int sectorcopy_run(const struct sectorcopy_io *io, unsigned long long count,
                   unsigned long long *written, char *err, size_t errlen)
{
    static unsigned char buf[CHUNK];
    unsigned long long done = 0;
    unsigned long long last = 0;
    unsigned long held = 0;
    unsigned percent;

    *written = 0;
    while (done + held < count) {
        unsigned long long left = count - done - held;
        unsigned long want = CHUNK - held;
        long n;

        if ((unsigned long long)want > left) {
            want = (unsigned long)left;
        }
        n = io->read(io->ctx, buf + held, want);
        if (n <= 0) {
            unsigned long chunk = held - (held % SECTORCOPY_SECTOR);
            unsigned long off = 0;

            while (off < chunk) {
                long w = io->write(io->ctx, buf + off, chunk - off);

                if (w <= 0) {
                    break;
                }
                off += (unsigned long)w;
                done += (unsigned long long)w;
                *written = done;
            }
            say(err, errlen,
                n < 0 ? "the stream failed" : "the stream ended before the length it declared");
            return -1;
        }
        held += (unsigned long)n;

        while (held >= SECTORCOPY_SECTOR && (held == CHUNK || done + held == count)) {
            unsigned long chunk = held - (held % SECTORCOPY_SECTOR);
            unsigned long off = 0;

            while (off < chunk) {
                long w = io->write(io->ctx, buf + off, chunk - off);

                if (w <= 0) {
                    say(err, errlen, "the device did not take the write");
                    return -1;
                }
                off += (unsigned long)w;
                done += (unsigned long long)w;
                *written = done;
            }
            if (io->note && sectorcopy_step(done, count, &last, &percent)) {
                io->note(io->ctx, done, count, percent);
            }
            memmove(buf, buf + chunk, held - chunk);
            held -= chunk;
        }
    }
    if (held != 0) {
        /* Not whole sectors, which sectorcopy_bounds already refuses. */
        say(err, errlen, "the stream ended inside a sector");
        return -1;
    }

    {
        unsigned char extra[1];
        long n = io->read(io->ctx, extra, sizeof(extra));

        if (n > 0) {
            say(err, errlen, "the stream carried more than the length it declared");
            return -1;
        }
        if (n < 0) {
            say(err, errlen, "the stream failed before its end could be established");
            return -1;
        }
    }
    return 0;
}
