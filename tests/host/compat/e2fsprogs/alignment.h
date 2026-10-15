/**
 * Exercise the port's raw I/O against a sector device kept in memory.
 *
 * Both host tests compile unix_io.c itself. Every data call and seek is
 * intercepted; no host file descriptor reaches the code under test.
 * A short positional call retries the original range through seek and
 * read or write. A second short call reports EXT2_ET_SHORT_READ or
 * EXT2_ET_SHORT_WRITE; read also zeroes the missing bytes. These are the
 * upstream rules, checked with aligned buffers as well as misaligned ones.
 */
#ifndef RACH2_TEST_E2FSPROGS_ALIGNMENT_H
#define RACH2_TEST_E2FSPROGS_ALIGNMENT_H

#define _XOPEN_SOURCE 600
#define _DARWIN_C_SOURCE
#define _GNU_SOURCE
#define _LARGEFILE64_SOURCE
#define __rach2__ 1

#include <stdint.h>
#include <unistd.h>
#include "config.h"
#include <ext2fs/ext2fs.h>

static ssize_t model_pread(int fd, void *buf, size_t size, off_t offset);
static ssize_t model_pread64(int fd, void *buf, size_t size, ext2_loff_t offset);
static ssize_t model_pwrite(int fd, const void *buf, size_t size, off_t offset);
static ssize_t model_pwrite64(int fd, const void *buf, size_t size, ext2_loff_t offset);
static ssize_t model_read(int fd, void *buf, size_t size);
static ssize_t model_write(int fd, const void *buf, size_t size);

#define pread    model_pread
#define pread64  model_pread64
#define pwrite   model_pwrite
#define pwrite64 model_pwrite64
#define read     model_read
#define write    model_write
#include "../../../../vendor/e2fsprogs/e2fsprogs-1.47.4/lib/ext2fs/unix_io.c"
#undef pread
#undef pread64
#undef pwrite
#undef pwrite64
#undef read
#undef write

#define SECTOR     512
#define LARGE_SIZE (128 * SECTOR)
#define SMALL_SIZE (8 * SECTOR)
#define DISK_SIZE  (LARGE_SIZE + 4 * SECTOR)
#define DEVICE_FD  12345

enum call_kind { CALL_PREAD, CALL_PREAD64, CALL_PWRITE, CALL_PWRITE64, CALL_READ, CALL_WRITE };
enum reply_kind { REPLY_FULL, REPLY_SHORT, REPLY_ERROR };

#ifdef ALIGNMENT_TEST_64
#define POSITIONAL_READ  CALL_PREAD64
#define POSITIONAL_WRITE CALL_PWRITE64
#define TEST_NAME        "e2fsprogs_alignment64_hosttest"
#else
#define POSITIONAL_READ  CALL_PREAD
#define POSITIONAL_WRITE CALL_PWRITE
#define TEST_NAME        "e2fsprogs_alignment_hosttest"
#endif

struct call_record {
    enum call_kind kind;
    size_t size;
    ext2_loff_t offset;
    const void *buf;
};

static unsigned char disk[DISK_SIZE], expected[DISK_SIZE];
static struct call_record calls[512];
static int ncalls, nseeks, invalid_calls, handler_calls;
static size_t handler_size;
static int handler_actual;
static errcode_t handler_error;
static ext2_loff_t position; /* the offset of the next read or write */
/* How the model answers the positional calls and the read and write calls. */
static enum reply_kind positional_reply, stream_reply;
static struct struct_io_channel channel;
static struct unix_private_data private_data;
static int failed;

#define CHECK(condition, row)                               \
    do {                                                    \
        if (condition)                                      \
            printf(TEST_NAME ": pass: %s\n", row);          \
        else {                                              \
            fprintf(stderr, TEST_NAME ": fail: %s\n", row); \
            failed++;                                       \
        }                                                   \
    } while (0)

/**
 * Record one call, then transfer its requested or injected byte count: the
 * count moved, or -1 with errno EINVAL for a call outside the device or not
 * in whole sectors, and EIO when the reply for its kind is an error. A call
 * past the record's room counts as invalid.
 */
static ssize_t transfer(enum call_kind kind, int fd, void *buf, size_t size, ext2_loff_t offset)
{
    enum reply_kind reply = kind < CALL_READ ? positional_reply : stream_reply;
    size_t actual = reply == REPLY_SHORT ? size / 2 : size;
    int writing = kind == CALL_PWRITE || kind == CALL_PWRITE64 || kind == CALL_WRITE;

    if (ncalls < (int)(sizeof(calls) / sizeof(calls[0]))) {
        calls[ncalls].kind = kind;
        calls[ncalls].size = size;
        calls[ncalls].offset = offset;
        calls[ncalls].buf = buf;
    } else
        invalid_calls++;
    ncalls++;
    if (fd != DEVICE_FD || offset < 0 || offset > DISK_SIZE || size > DISK_SIZE - (size_t)offset ||
        offset % SECTOR || size % SECTOR) {
        invalid_calls++;
        errno = EINVAL;
        return -1;
    }
    if (reply == REPLY_ERROR) {
        errno = EIO;
        return -1;
    }
    if (writing)
        memcpy(disk + offset, buf, actual);
    else
        memcpy(buf, disk + offset, actual);
    return (ssize_t)actual;
}

/** pread(2) for unix_io.c: `transfer`. */
static ssize_t model_pread(int fd, void *buf, size_t size, off_t offset)
{
    return transfer(CALL_PREAD, fd, buf, size, offset);
}

/** pread64 for unix_io.c: `transfer`. */
static ssize_t model_pread64(int fd, void *buf, size_t size, ext2_loff_t offset)
{
    return transfer(CALL_PREAD64, fd, buf, size, offset);
}

/** pwrite(2) for unix_io.c: `transfer`. */
static ssize_t model_pwrite(int fd, const void *buf, size_t size, off_t offset)
{
    return transfer(CALL_PWRITE, fd, (void *)buf, size, offset);
}

/** pwrite64 for unix_io.c: `transfer`. */
static ssize_t model_pwrite64(int fd, const void *buf, size_t size, ext2_loff_t offset)
{
    return transfer(CALL_PWRITE64, fd, (void *)buf, size, offset);
}

/** read(2) for unix_io.c: `transfer` at `position`, which advances by the
 * count moved. */
static ssize_t model_read(int fd, void *buf, size_t size)
{
    ssize_t actual = transfer(CALL_READ, fd, buf, size, position);

    if (actual > 0)
        position += actual;
    return actual;
}

/** write(2) for unix_io.c: `transfer` at `position`, which advances by the
 * count moved. */
static ssize_t model_write(int fd, const void *buf, size_t size)
{
    ssize_t actual = transfer(CALL_WRITE, fd, (void *)buf, size, position);

    if (actual > 0)
        position += actual;
    return actual;
}

/** Set `position` to `offset` and return it, counting the seek; -1 with
 * errno EINVAL for another descriptor, a `whence` other than SEEK_SET, or
 * an offset outside the device. */
ext2_loff_t ext2fs_llseek(int fd, ext2_loff_t offset, int whence)
{
    nseeks++;
    if (fd != DEVICE_FD || whence != SEEK_SET || offset < 0 || offset > DISK_SIZE) {
        invalid_calls++;
        errno = EINVAL;
        return -1;
    }
    position = offset;
    return position;
}

/** Abort: cache allocation and channel opening are outside this fixture. */
errcode_t io_channel_alloc_buf(io_channel io, int count, void *ptr)
{
    (void)io;
    (void)count;
    (void)ptr;
    abort();
}

/** Emit the header's C99 inline definition for the unused cache code. */
extern inline errcode_t ext2fs_free_mem(void *ptr);

/** Abort: the raw calls under test read no environment. */
char *ext2fs_safe_getenv(const char *name)
{
    (void)name;
    abort();
}

/** Abort: the fixture sets the channel's alignment itself. */
int ext2fs_get_dio_alignment(int fd)
{
    (void)fd;
    abort();
}

/** Record the error handler's arguments without changing its result. */
static errcode_t read_error(io_channel io, unsigned long block, int count, void *buf, size_t size,
                            int actual, errcode_t error)
{
    (void)io;
    (void)block;
    (void)count;
    (void)buf;
    handler_calls++;
    handler_size = size;
    handler_actual = actual;
    handler_error = error;
    return error;
}

/** The write error handler: `read_error`. */
static errcode_t write_error(io_channel io, unsigned long block, int count, const void *buf,
                             size_t size, int actual, errcode_t error)
{
    return read_error(io, block, count, (void *)buf, size, actual, error);
}

/** Reset the device and channel before each independent request. */
static void reset_device(unsigned char *bounce)
{
    size_t i;

    for (i = 0; i < sizeof(disk); i++)
        disk[i] = (unsigned char)(i * 29 + (i / SECTOR) * 7 + 3);
    memcpy(expected, disk, sizeof(disk));
    memset(calls, 0, sizeof(calls));
    memset(&channel, 0, sizeof(channel));
    memset(&private_data, 0, sizeof(private_data));
    channel.block_size = SECTOR;
    channel.align = SECTOR;
    channel.read_error = read_error;
    channel.write_error = write_error;
    channel.private_data = &private_data;
    private_data.dev = DEVICE_FD;
    private_data.bounce = bounce;
    ncalls = nseeks = invalid_calls = handler_calls = 0;
    handler_size = 0;
    handler_actual = 0;
    handler_error = 0;
    position = 0;
    positional_reply = stream_reply = REPLY_FULL;
}

/** Whether call `index` was recorded with this kind, size, offset and
 * buffer. */
static int call_is(int index, enum call_kind kind, size_t size, ext2_loff_t offset, const void *buf)
{
    return index < ncalls && calls[index].kind == kind && calls[index].size == size &&
           calls[index].offset == offset && calls[index].buf == buf;
}

/** Give adjacent sectors different contents to expose repeated copies. */
static void fill_buffer(unsigned char *buf, size_t size)
{
    size_t i;

    for (i = 0; i < size; i++)
        buf[i] = (unsigned char)(i * 17 + (i / SECTOR) * 31 + 91);
}

/** Check both the transfer size and every byte of the affected range. */
static void whole_requests(unsigned char *buf, unsigned char *bounce, int size, const char *label)
{
    errcode_t result;
    char row[120];

    reset_device(bounce);
    memset(buf, 0xa5, size);
    result = raw_read_blk(&channel, &private_data, 2, size / SECTOR, buf);
    snprintf(row, sizeof(row), "%s read uses one whole call and returns the bytes", label);
    CHECK(result == 0 && ncalls == 1 && nseeks == 0 && invalid_calls == 0 && handler_calls == 0 &&
              call_is(0, POSITIONAL_READ, size, 2 * SECTOR, buf) &&
              memcmp(buf, expected + 2 * SECTOR, size) == 0,
          row);

    reset_device(bounce);
    fill_buffer(buf, size);
    memcpy(expected + 2 * SECTOR, buf, size);
    result = raw_write_blk(&channel, &private_data, 2, size / SECTOR, buf, 0);
    snprintf(row, sizeof(row), "%s write uses one whole call and stores the bytes", label);
    CHECK(result == 0 && ncalls == 1 && nseeks == 0 && invalid_calls == 0 && handler_calls == 0 &&
              call_is(0, POSITIONAL_WRITE, size, 2 * SECTOR, buf) &&
              memcmp(disk, expected, sizeof(disk)) == 0,
          row);
}

/** A short length and an interior offset each require sector preservation. */
static void partial_requests(unsigned char *buf, unsigned char *bounce)
{
    errcode_t result;

    reset_device(bounce);
    fill_buffer(buf, SECTOR);
    memcpy(expected + 2 * SECTOR, buf, 73);
    result = raw_write_blk(&channel, &private_data, 2, -73, buf, 0);
    CHECK(result == 0 && ncalls == 2 && invalid_calls == 0 && handler_calls == 0 &&
              call_is(0, CALL_READ, SECTOR, 2 * SECTOR, bounce) &&
              call_is(1, CALL_WRITE, SECTOR, 2 * SECTOR, bounce) &&
              memcmp(disk, expected, sizeof(disk)) == 0,
          "a short write preserves the rest of its sector");

    reset_device(bounce);
    private_data.offset = 37;
    memcpy(expected + 2 * SECTOR + 37, buf, SECTOR);
    result = raw_write_blk(&channel, &private_data, 2, 1, buf, 0);
    CHECK(result == 0 && ncalls == 4 && invalid_calls == 0 && handler_calls == 0 &&
              call_is(0, CALL_READ, SECTOR, 2 * SECTOR, bounce) &&
              call_is(1, CALL_WRITE, SECTOR, 2 * SECTOR, bounce) &&
              call_is(2, CALL_READ, SECTOR, 3 * SECTOR, bounce) &&
              call_is(3, CALL_WRITE, SECTOR, 3 * SECTOR, bounce) &&
              memcmp(disk, expected, sizeof(disk)) == 0,
          "an interior write preserves both surrounding sector fragments");
}

/** Check upstream retries, final short counts, zero fill, and errno. */
static void failed_transfers(unsigned char *buf, unsigned char *bounce, const char *label)
{
    enum reply_kind first, second;
    int writing;
    char row[160];

    for (writing = 0; writing <= 1; writing++) {
        for (first = REPLY_SHORT; first <= REPLY_ERROR; first++) {
            for (second = REPLY_FULL; second <= REPLY_ERROR; second++) {
                errcode_t result, error = 0;
                int actual = second == REPLY_ERROR ? (writing ? -1 : 0) : SMALL_SIZE / 2;
                size_t changed;
                const char *outcome = second == REPLY_FULL    ? "retries the whole range"
                                      : second == REPLY_SHORT ? "reports the short count"
                                                              : "reports eio";

                reset_device(bounce);
                if (writing)
                    fill_buffer(buf, SMALL_SIZE);
                else
                    memset(buf, 0xa5, SMALL_SIZE);
                positional_reply = first;
                stream_reply = second;
                if (second != REPLY_FULL)
                    error = second == REPLY_ERROR ? EIO
                            : writing             ? EXT2_ET_SHORT_WRITE
                                                  : EXT2_ET_SHORT_READ;
                if (writing) {
                    changed = second == REPLY_FULL                              ? SMALL_SIZE
                              : (first == REPLY_SHORT || second == REPLY_SHORT) ? SMALL_SIZE / 2
                                                                                : 0;
                    memcpy(expected + 2 * SECTOR, buf, changed);
                    result = raw_write_blk(&channel, &private_data, 2, SMALL_SIZE / SECTOR, buf, 0);
                } else {
                    if (second != REPLY_FULL)
                        memset(expected + 2 * SECTOR + actual, 0, SMALL_SIZE - actual);
                    result = raw_read_blk(&channel, &private_data, 2, SMALL_SIZE / SECTOR, buf);
                }
                snprintf(row, sizeof(row), "%s %s after positional %s %s", label,
                         writing ? "write" : "read", first == REPLY_SHORT ? "short" : "error",
                         outcome);
                CHECK(
                    result == error && ncalls == 2 && nseeks == 1 && invalid_calls == 0 &&
                        call_is(0, writing ? POSITIONAL_WRITE : POSITIONAL_READ, SMALL_SIZE,
                                2 * SECTOR, buf) &&
                        call_is(1, writing ? CALL_WRITE : CALL_READ, SMALL_SIZE, 2 * SECTOR, buf) &&
                        handler_calls == (second != REPLY_FULL) &&
                        (second == REPLY_FULL ||
                         (handler_size == SMALL_SIZE && handler_actual == actual &&
                          handler_error == error)) &&
                        (writing ? memcmp(disk, expected, sizeof(disk)) == 0
                                 : memcmp(buf, expected + 2 * SECTOR, SMALL_SIZE) == 0),
                    row);
            }
        }
    }
}

/**
 * Run the rows with a buffer aligned to a sector and one at an even address
 * two bytes past a sector boundary. Returns the count of failed checks.
 */
int main(void)
{
    unsigned char *allocation = malloc(LARGE_SIZE + 2 * SECTOR);
    unsigned char *bounce_allocation = malloc(2 * SECTOR);
    unsigned char *aligned, *misaligned, *bounce;

    CHECK(allocation != NULL && bounce_allocation != NULL, "the fixture allocates its buffers");
    if (!allocation || !bounce_allocation) {
        free(allocation);
        free(bounce_allocation);
        return failed;
    }
    aligned = allocation + (SECTOR - (uintptr_t)allocation % SECTOR) % SECTOR;
    misaligned = aligned + 2;
    bounce = bounce_allocation + (SECTOR - (uintptr_t)bounce_allocation % SECTOR) % SECTOR;
    CHECK((uintptr_t)aligned % SECTOR == 0 && (uintptr_t)bounce % SECTOR == 0 &&
              (uintptr_t)misaligned % SECTOR == 2 && (uintptr_t)misaligned % 2 == 0,
          "the caller buffer is even and deliberately misaligned to 512 bytes");
    whole_requests(misaligned, bounce, SMALL_SIZE, "misaligned");
    whole_requests(aligned, bounce, LARGE_SIZE, "large aligned");
    partial_requests(misaligned, bounce);
    failed_transfers(aligned, bounce, "aligned");
    failed_transfers(misaligned, bounce, "misaligned");
    free(bounce_allocation);
    free(allocation);
    printf(TEST_NAME ": result: %d failed checks\n", failed);
    return failed;
}

#endif /* RACH2_TEST_E2FSPROGS_ALIGNMENT_H */
