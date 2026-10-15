/**
 * sectorcopy_hosttest.c: utils/sectorcopy.c, the copy library of reflash,
 * against a model stream and device.
 *
 * host-deps: utils/sectorcopy.c
 *
 * The library decides where a write lands on a disk and whether what arrived
 * is the whole of what was sent. Neither can be arranged on request at the
 * other end of an `ssh` pipe, so a stream that stops half way, one that
 * hands out a byte at a time, one that has more than it declared, and a
 * device that takes short writes are models here, and the numbers are
 * driven to the ends of what 64 bits hold.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sectorcopy.h"

static int fails;

#define CHECK(cond, what)                          \
    do {                                           \
        if (!(cond)) {                             \
            fails++;                               \
            fprintf(stderr, "  FAIL: %s\n", what); \
        }                                          \
    } while (0)

/* Large enough for a copy of two progress steps and a little, which is what
   the reporting rows below drive; the rest use a few kilobytes. */
#define DISK_BYTES (34 * 1024 * 1024)

/* The stream and the device, with the controls each case needs. */
static struct {
    unsigned char source[DISK_BYTES]; /* what the stream hands out */
    unsigned long source_len;         /* how much of it there is */
    unsigned long source_pos;
    unsigned long per_read; /* the most one read returns, 0 for everything */
    int read_fails_after;   /* -1 never; otherwise reads before one fails */
    int reads_done;
    unsigned char device[DISK_BYTES];
    unsigned long device_pos;
    unsigned long per_write; /* the most one write takes, 0 for everything */
    int write_fails_after;   /* -1 never; otherwise writes before one fails */
    int writes_done;
    unsigned long largest_write;
    unsigned long write_sizes[8]; /* the first writes, in order */
} m;

/**
 * The stream's read: copy up to `n` bytes, at most `per_read`, from the
 * source into `buf`. Returns the count, 0 at the end of the source as a pipe
 * does, or -1 once `read_fails_after` reads have been made. A read past
 * 10000 also fails, so a library that ignored the end of a stream fails
 * with a message instead of reading until the suite's time limit.
 */
static long m_read(void *ctx, unsigned char *buf, unsigned long n)
{
    unsigned long left = m.source_len > m.source_pos ? m.source_len - m.source_pos : 0;

    (void)ctx;
    if (m.read_fails_after >= 0 && m.reads_done >= m.read_fails_after) {
        return -1;
    }
    if (m.reads_done > 10000) {
        return -1;
    }
    m.reads_done++;
    if (m.per_read != 0 && n > m.per_read) {
        n = m.per_read;
    }
    if (n > left) {
        n = left;
    }
    memcpy(buf, m.source + m.source_pos, n);
    m.source_pos += n;
    return (long)n;
}

/** The device's write: take up to `n` bytes, at most `per_write`, from
 *  `buf` at the device's position, recording the sizes. Returns the count
 *  taken, or -1 once `write_fails_after` writes have been made and for a
 *  write past the device. */
static long m_write(void *ctx, const unsigned char *buf, unsigned long n)
{
    (void)ctx;
    if (m.write_fails_after >= 0 && m.writes_done >= m.write_fails_after) {
        return -1;
    }
    m.writes_done++;
    if (m.per_write != 0 && n > m.per_write) {
        n = m.per_write;
    }
    if (m.device_pos + n > DISK_BYTES) {
        return -1;
    }
    if (n > m.largest_write) {
        m.largest_write = n;
    }
    if (m.writes_done <= 8) {
        m.write_sizes[m.writes_done - 1] = n;
    }
    memcpy(m.device + m.device_pos, buf, n);
    m.device_pos += n;
    return (long)n;
}

/* What the copy reported: the counts it passed to `note`, in order, so a
   caller that reported twice at a step or not at all is seen. */
static unsigned long long noted[64];
static int notes;
static unsigned last_percent;

/** The progress callback: record `done` in `noted`, count the call, and keep
 *  `percent`. */
static void m_note(void *ctx, unsigned long long done, unsigned long long total, unsigned percent)
{
    (void)ctx;
    (void)total;
    if (notes < (int)(sizeof(noted) / sizeof(noted[0]))) {
        noted[notes] = done;
    }
    notes++;
    last_percent = percent;
}

static const struct sectorcopy_io IO = {m_read, m_write, NULL, NULL};
static const struct sectorcopy_io IO_NOTED = {m_read, m_write, m_note, NULL};

/** Load the stream with `len` bytes of a pattern, fill the device with
 *  0xEE, and clear the controls. */
static void load(unsigned long len)
{
    unsigned long i;

    memset(&m, 0, sizeof(m));
    m.read_fails_after = -1;
    m.write_fails_after = -1;
    for (i = 0; i < len; i++) {
        m.source[i] = (unsigned char)(i * 7 + (i >> 8));
    }
    m.source_len = len;
    memset(m.device, 0xEE, sizeof(m.device));
}

/**
 * Check `sectorcopy_parse_u64` on decimal numbers up to the largest 64-bit
 * value, refusing one past it, a sign, leading space, trailing text,
 * hexadecimal and the empty string; and `sectorcopy_bounds` on a 250 GB
 * disk at its first and last sectors and past its end, with an offset near
 * the top of 64 bits and a count whose sum with the offset would wrap, which
 * the check must not compute, and with a length of zero, an offset or
 * length inside a sector, and a device of no size.
 */
static void check_numbers(void)
{
    unsigned long long v = 0;
    char err[96];

    CHECK(sectorcopy_parse_u64("0", &v) == 0 && v == 0, "zero parses");
    CHECK(sectorcopy_parse_u64("524288000", &v) == 0 && v == 524288000ull,
          "a root partition's length parses");
    CHECK(sectorcopy_parse_u64("18446744073709551615", &v) == 0 && v == 18446744073709551615ull,
          "the largest 64-bit number parses");
    CHECK(sectorcopy_parse_u64("18446744073709551616", &v) != 0,
          "one past it is refused, not wrapped");
    CHECK(sectorcopy_parse_u64("99999999999999999999999", &v) != 0, "and so is far past it");
    CHECK(sectorcopy_parse_u64("-512", &v) != 0, "a negative number is not an offset");
    CHECK(sectorcopy_parse_u64("+512", &v) != 0, "and neither is a signed one");
    CHECK(sectorcopy_parse_u64("512x", &v) != 0, "trailing text is refused");
    CHECK(sectorcopy_parse_u64(" 512", &v) != 0, "and so is leading space");
    CHECK(sectorcopy_parse_u64("0x200", &v) != 0, "hexadecimal is not decimal");
    CHECK(sectorcopy_parse_u64("", &v) != 0, "nothing is not a number");
    CHECK(sectorcopy_parse_u64("00512", &v) == 0 && v == 512, "leading zeros are digits");

    {
        const unsigned long long disk = 250000000000ull;

        CHECK(sectorcopy_bounds(disk, 0, 512, err, sizeof(err)) == 0,
              "the first sector of a 250 GB disk is writable");
        CHECK(sectorcopy_bounds(disk, disk - 512, 512, err, sizeof(err)) == 0,
              "and so is its last");
        CHECK(sectorcopy_bounds(disk, disk - 512, 1024, err, sizeof(err)) != 0,
              "a write that runs one sector past the end is refused");
        CHECK(sectorcopy_bounds(disk, disk, 512, err, sizeof(err)) != 0,
              "an offset at the end has nothing after it");
        CHECK(sectorcopy_bounds(disk, 0, disk, err, sizeof(err)) == 0,
              "the whole disk fits itself");
        CHECK(sectorcopy_bounds(disk, 512, disk, err, sizeof(err)) != 0,
              "the whole disk one sector in does not");
    }

    CHECK(sectorcopy_bounds(1024, 0xFFFFFFFFFFFFFE00ull, 1024, err, sizeof(err)) != 0,
          "an offset near the top of 64 bits cannot reach a small disk");
    CHECK(sectorcopy_bounds(0xFFFFFFFFFFFFFE00ull, 0xFFFFFFFFFFFFFC00ull, 1024, err, sizeof(err)) !=
              0,
          "and a count that would wrap the sum is refused");

    CHECK(sectorcopy_bounds(1024, 0, 0, err, sizeof(err)) != 0, "a length of zero");
    CHECK(sectorcopy_bounds(1024, 1, 512, err, sizeof(err)) != 0, "an offset inside a sector");
    CHECK(sectorcopy_bounds(1024, 0, 513, err, sizeof(err)) != 0, "a length inside a sector");
    CHECK(sectorcopy_bounds(0, 0, 512, err, sizeof(err)) != 0, "a device of no size");
}

/**
 * Check `sectorcopy_run`.
 *
 * - A stream that hands out everything at once copies byte for byte, and
 *   nothing past the image is written.
 * - A pipe hands out what has arrived, which may be one byte; the device
 *   must still be written in whole sectors.
 * - A stream that ends early, at a sector boundary or inside a sector, fails
 *   and reports how much reached the disk, so a disk with half an image is
 *   not reported done; the part sector is not written.
 * - A stream with more than it declared fails with exactly the declared
 *   length written.
 * - A device that takes one sector a call still gets the whole image, and
 *   one that refuses a write, or a stream that fails, fails the copy.
 * - The block layer moves 2048 sectors, a mebibyte, in one request, and a
 *   library that wrote a sector a call, or each piece the stream handed it,
 *   would turn one command into many: an image of 2 MiB and 32 KiB read in
 *   pieces of 64 KiB goes to the device in exactly three writes.
 * - A failure of the last read, the one that shows the stream ended where it
 *   said, fails the copy, since a success would report a whole image on a
 *   check that was not made.
 * - A length that is not a whole number of sectors, which the bounds refuse
 *   before a copy starts, fails the copy on its own, with only the whole
 *   sector written; one sector copies.
 */
static void check_copy(void)
{
    unsigned long long written = 0;
    char err[96];

    load(64 * 1024);
    CHECK(sectorcopy_run(&IO, 64 * 1024, &written, err, sizeof(err)) == 0, "a whole image copies");
    CHECK(written == 64 * 1024, "and reports its length");
    CHECK(memcmp(m.device, m.source, 64 * 1024) == 0, "byte for byte");
    CHECK(m.device[64 * 1024] == 0xEE, "and nothing past it");

    /* A pipe hands out what has arrived, which may be one byte. The device
       must still see whole sectors and nothing smaller. */
    load(4096);
    m.per_read = 1;
    CHECK(sectorcopy_run(&IO, 4096, &written, err, sizeof(err)) == 0,
          "a stream that hands out a byte at a time still copies");
    CHECK(memcmp(m.device, m.source, 4096) == 0, "byte for byte");
    {
        int all_whole = 1;
        unsigned long i;

        for (i = 0; i < 1; i++) {
            if ((m.device_pos % SECTORCOPY_SECTOR) != 0) {
                all_whole = 0;
            }
        }
        CHECK(all_whole, "the device saw a whole number of sectors");
        CHECK(m.writes_done <= 4096 / SECTORCOPY_SECTOR,
              "and no more writes than there were sectors");
    }

    load(2048);
    m.source_len = 1536;
    CHECK(sectorcopy_run(&IO, 2048, &written, err, sizeof(err)) != 0,
          "a stream that ends early fails");
    CHECK(strstr(err, "ended before") != NULL, "and says the stream ended early");
    CHECK(written == 1536, "and how much had gone to the disk");

    load(2048);
    m.source_len = 1600;
    CHECK(sectorcopy_run(&IO, 2048, &written, err, sizeof(err)) != 0,
          "a stream that ends inside a sector fails");
    CHECK(written == 1536, "and the part sector never reached the device");

    load(4096);
    CHECK(sectorcopy_run(&IO, 2048, &written, err, sizeof(err)) != 0,
          "a stream longer than its declared length fails");
    CHECK(written == 2048, "with exactly the declared length written");
    CHECK(memcmp(m.device, m.source, 2048) == 0, "and those bytes are right");
    CHECK(m.device[2048] == 0xEE, "and nothing past them");

    load(4096);
    m.per_write = SECTORCOPY_SECTOR;
    CHECK(sectorcopy_run(&IO, 4096, &written, err, sizeof(err)) == 0,
          "a device that takes one sector per call still gets the whole image");
    CHECK(memcmp(m.device, m.source, 4096) == 0, "byte for byte");

    load(4096);
    m.per_write = SECTORCOPY_SECTOR;
    m.write_fails_after = 2;
    CHECK(sectorcopy_run(&IO, 4096, &written, err, sizeof(err)) != 0,
          "a device that refuses a write fails the copy");
    CHECK(written == 2 * SECTORCOPY_SECTOR, "and reports how far it got");
    load(4096);
    m.per_read = SECTORCOPY_SECTOR;
    m.read_fails_after = 2;
    CHECK(sectorcopy_run(&IO, 4096, &written, err, sizeof(err)) != 0,
          "a stream that fails mid-way fails the copy");
    CHECK(strstr(err, "stream failed") != NULL, "and says the stream is what failed");

    load(2 * 1024 * 1024 + 32 * 1024);
    m.per_read = 64 * 1024;
    CHECK(sectorcopy_run(&IO, 2 * 1024 * 1024 + 32 * 1024, &written, err, sizeof(err)) == 0,
          "a large image copies");
    CHECK(m.largest_write == 2048 * SECTORCOPY_SECTOR,
          "and goes to the device a mebibyte at a time, gathered from smaller reads");
    CHECK(m.writes_done == 3 && m.write_sizes[0] == 2048 * SECTORCOPY_SECTOR &&
              m.write_sizes[1] == 2048 * SECTORCOPY_SECTOR && m.write_sizes[2] == 32 * 1024,
          "in exactly three writes: a mebibyte, a mebibyte and the 32 KiB left");
    CHECK(memcmp(m.device, m.source, 2 * 1024 * 1024 + 32 * 1024) == 0, "byte for byte");

    load(2048);
    m.per_read = 1024;
    m.read_fails_after = 2;
    CHECK(sectorcopy_run(&IO, 2048, &written, err, sizeof(err)) != 0,
          "a stream that fails on the read that proves it ended fails the copy");
    CHECK(strstr(err, "before its end") != NULL, "and says what could not be established");

    load(1000);
    CHECK(sectorcopy_run(&IO, 1000, &written, err, sizeof(err)) != 0,
          "a length that is not a whole number of sectors fails the copy");
    CHECK(strstr(err, "inside a sector") != NULL, "and says the tail is a part sector");
    CHECK(written == 512, "with only the whole sector written");

    load(512);
    CHECK(sectorcopy_run(&IO, 512, &written, err, sizeof(err)) == 0, "one sector copies");
    CHECK(memcmp(m.device, m.source, 512) == 0, "byte for byte");
}

/**
 * Check the progress rule of `sectorcopy_step`: a line every step, one at
 * the end, and none in between.
 *
 * The rule is arithmetic and not a device, so it is tested here rather than
 * watched on a machine writing a gibibyte and a half. A copy of any length
 * says that it ended, a copy shorter than a step says nothing before that,
 * and the percentage is of the whole. A write that passes several steps at
 * once is one line, since the count is the bytes on the device and not the
 * writes it took. A line at exactly the end is not followed by a second at
 * the same count. A copy of nothing ends at a hundred percent. A quarter of
 * two tebibytes is 25 percent, which needs 64-bit arithmetic.
 */
static void check_progress(void)
{
    unsigned long long last = 0;
    unsigned percent = 0;
    unsigned long long total = 100ULL * SECTORCOPY_STEP;
    unsigned long long step = SECTORCOPY_STEP;

    CHECK(!sectorcopy_step(512, total, &last, &percent),
          "a copy that has not passed a step says nothing");
    CHECK(last == 0, "and the last line reported stays where it was");

    CHECK(sectorcopy_step(step, total, &last, &percent), "the first step is a line");
    CHECK(percent == 1, "at one percent of a hundred steps");
    CHECK(last == step, "and the line reported is that count");

    CHECK(!sectorcopy_step(step + 512, total, &last, &percent),
          "the bytes after it are not another line");
    CHECK(sectorcopy_step(2 * step, total, &last, &percent), "the next step is");
    CHECK(percent == 2, "at two percent");

    last = 0;
    CHECK(sectorcopy_step(8 * step, total, &last, &percent),
          "a write past several steps is a line");
    CHECK(percent == 8, "at the percentage it reached");

    last = 0;
    CHECK(sectorcopy_step(1024, 1024, &last, &percent), "a copy shorter than a step says it ended");
    CHECK(percent == 100, "at a hundred percent");
    last = 0;
    CHECK(!sectorcopy_step(512, 1024, &last, &percent), "and says nothing before that");

    last = 0;
    CHECK(sectorcopy_step(step, step, &last, &percent), "a copy of exactly one step is a line");
    CHECK(!sectorcopy_step(step, step, &last, &percent), "and not two");

    last = 0;
    CHECK(sectorcopy_step(0, 0, &last, &percent), "a copy of nothing says it ended");
    CHECK(percent == 100, "at a hundred percent, since there is nothing left");

    last = 0;
    CHECK(
        sectorcopy_step(512ULL * 1024 * 1024 * 1024, 2048ULL * 1024 * 1024 * 1024, &last, &percent),
        "a step of a two-tebibyte copy is a line");
    CHECK(percent == 25, "at a quarter");
}

/**
 * Check the copy's own reporting: the callback is called once at each step
 * and once at the end, so a copy of two steps and a little reports three
 * times, and a copy given no callback reports nothing. `check_progress`
 * checks the rule; this is the loop calling it.
 */
static void check_notes(void)
{
    unsigned long long written;
    char err[96];

    load(4096);
    notes = 0;
    CHECK(sectorcopy_run(&IO_NOTED, 4096, &written, err, sizeof(err)) == 0, "a short copy runs");
    CHECK(notes == 1, "and reports once, at its end");
    CHECK(noted[0] == 4096, "with the count it wrote");
    CHECK(last_percent == 100, "at a hundred percent");

    load(4096);
    notes = 0;
    CHECK(sectorcopy_run(&IO, 4096, &written, err, sizeof(err)) == 0, "the same copy without one");
    CHECK(notes == 0, "reports nothing");

    load(2 * SECTORCOPY_STEP + 4096);
    notes = 0;
    m.per_read = 64 * 1024;
    CHECK(sectorcopy_run(&IO_NOTED, 2 * SECTORCOPY_STEP + 4096, &written, err, sizeof(err)) == 0,
          "a copy of two steps and a little runs");
    CHECK(notes == 3, "and reports three times");
    CHECK(noted[0] == SECTORCOPY_STEP, "at the first step");
    CHECK(noted[1] == 2 * SECTORCOPY_STEP, "at the second");
    CHECK(noted[2] == 2 * SECTORCOPY_STEP + 4096, "and at the end");
    m.per_read = 0;
}

/** Run the four groups of rows. Returns the count of failed checks, at most
 *  120. */
int main(void)
{
    check_numbers();
    check_copy();
    check_progress();
    check_notes();
    return fails > 120 ? 120 : fails;
}
