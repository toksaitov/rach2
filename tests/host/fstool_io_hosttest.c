/*
 * Check the independent fstools adapter against a sector-only block model:
 * partial writes preserve neighboring bytes, both offset ABIs agree, regular
 * files pass through, and boundary, short-transfer and error results remain
 * visible to the formatter or checker. No disk or child process is used.
 * host-cflags: -Wall -Wextra -Werror
 */
#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#ifdef __APPLE__
/* Clang's freestanding limits.h omits the system POSIX limits. */
#include <machine/limits.h>
#endif
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

static unsigned char disk[2048], expected[2048], data[1200];
static int reads, writes, read_interrupt, write_interrupt, short_read, short_write;
static int fail_write, regular_calls, failed;

static int model_stat(int fd, struct stat *st)
{
    memset(st, 0, sizeof(*st));
    if (fd != 7 && fd != 8) {
        errno = EBADF;
        return -1;
    }
    st->st_mode = fd == 7 ? S_IFBLK : S_IFREG;
    return 0;
}

ssize_t model_pread64(int fd, void *buffer, size_t size, int64_t offset)
{
    (void)fd;
    reads++;
    if (read_interrupt) {
        read_interrupt--;
        errno = EINTR;
        return -1;
    }
    if (offset < 0 || offset > (int64_t)sizeof(disk) || size > sizeof(disk) - (size_t)offset) {
        errno = EIO;
        return -1;
    }
    if (short_read == reads)
        size /= 2;
    memcpy(buffer, disk + offset, size);
    return (ssize_t)size;
}

ssize_t __real_pwrite64(int fd, const void *buffer, size_t size, int64_t offset)
{
    writes++;
    if (write_interrupt) {
        write_interrupt--;
        errno = EINTR;
        return -1;
    }
    if (fd == 8)
        regular_calls++;
    if (fail_write == writes || offset < 0 || offset > (int64_t)sizeof(disk) ||
        size > sizeof(disk) - (size_t)offset || (fd == 7 && (offset % 512 || size % 512))) {
        errno = EIO;
        return -1;
    }
    if (short_write == writes)
        size /= 2;
    memcpy(disk + offset, buffer, size);
    return (ssize_t)size;
}

ssize_t __real_pwrite(int fd, const void *buffer, size_t size, long offset)
{
    return __real_pwrite64(fd, buffer, size, offset);
}

/* Some desktop CRT headers make fstat an ABI-selecting macro. The model
 * supplies its own descriptor classification and uses no host block device. */
#ifdef fstat
#undef fstat
#endif
#define fstat   model_stat
#define pread64 model_pread64
#include "../../vendor/gnu/fstools/positional_io.c"
#undef pread64
#undef fstat

static void check(int held, const char *name)
{
    if (!held) {
        fprintf(stderr, "fstool-io: failed: %s\n", name);
        failed++;
    }
}

static void reset(void)
{
    for (size_t i = 0; i < sizeof(disk); i++)
        disk[i] = (unsigned char)(i * 19 + 7);
    memset(data, 0x5a, sizeof(data));
    memcpy(expected, disk, sizeof(disk));
    reads = writes = read_interrupt = write_interrupt = short_read = short_write = 0;
    fail_write = regular_calls = 0;
}

static void preservation(void)
{
    reset();
    memcpy(expected + 4, data, 4);
    check(__wrap_pwrite64(7, data, 4, 4) == 4 && !memcmp(disk, expected, sizeof(disk)),
          "four-byte FAT entry preserves its sector");
    check(reads == 1 && writes == 1, "one touched sector only");
    reset();
    memcpy(expected + 510, data, 4);
    check(__wrap_pwrite(7, data, 4, 510) == 4 && !memcmp(disk, expected, sizeof(disk)),
          "ordinary offset ABI crosses a sector boundary");
    check(reads == 2 && writes == 2, "only two boundary sectors touched");
    reset();
    memcpy(expected + 13, data, sizeof(data));
    check(__wrap_pwrite64(7, data, sizeof(data), 13) == sizeof(data) &&
              !memcmp(disk, expected, sizeof(disk)),
          "head middle and tail preserve bytes");
    check(reads == 2 && writes == 3, "aligned middle is one direct write");
    reset();
    check(__wrap_pwrite64(7, data, 1024, 512) == 1024 && !reads && writes == 1,
          "aligned block write passes through");
    reset();
    check(__wrap_pwrite(8, data, 4, 3) == 4 && !reads && regular_calls == 1,
          "regular file accepts its original byte write");
}

static void failures(void)
{
    reset();
    read_interrupt = write_interrupt = 1;
    check(__wrap_pwrite64(7, data, 4, 3) == 4 && reads == 2 && writes == 2,
          "interrupted sector calls retry");
    reset();
    short_read = 1;
    check(__wrap_pwrite64(7, data, 4, 3) == -1 && errno == EIO && !writes &&
              !memcmp(disk, expected, sizeof(disk)),
          "short sector read writes nothing");
    reset();
    short_write = 1;
    check(__wrap_pwrite64(7, data, 4, 3) == -1 && errno == EIO,
          "short sector write cannot claim a complete FAT entry");
    reset();
    short_write = 1;
    check(__wrap_pwrite64(7, data, 1024, 0) == 512 && writes == 1,
          "aligned short write returns actual progress");
    reset();
    short_read = 2;
    expected[511] = data[0];
    check(__wrap_pwrite64(7, data, 2, 511) == 1 && writes == 1 &&
              !memcmp(disk, expected, sizeof(disk)),
          "later read error returns prior progress");
    reset();
    fail_write = 1;
    check(__wrap_pwrite64(7, data, 4, 3) == -1 && errno == EIO, "write error reaches the caller");
    reset();
    check(__wrap_pwrite64(7, data, 4, -1) == -1 && errno == EINVAL && !writes && !reads,
          "negative offset rejected before transfer");
    check(__wrap_pwrite64(7, data, 4, INT64_MAX - 1) == -1 && errno == EINVAL && !writes,
          "overflow rejected before transfer");
    check(__wrap_pwrite64(7, data, 4, 2048) == -1 && errno == EIO && !writes,
          "past-end sector read prevents write");
    check(__wrap_pwrite64(9, data, 4, 0) == -1 && errno == EBADF,
          "bad descriptor remains an error");
    reset();
    check(__wrap_pwrite64(7, data, 0, 0) == 0 && !reads, "zero length passes through");
    reset();
    expected[2047] = data[0];
    check(__wrap_pwrite64(7, data, 4, 2047) == 1 && writes == 1 &&
              !memcmp(disk, expected, sizeof(disk)),
          "end boundary returns only completed bytes");
    reset();
    write_interrupt = 1;
    check(__wrap_pwrite64(7, data, 512, 0) == 512 && !reads && writes == 2,
          "interrupted aligned write retries");
}

int main(void)
{
    preservation();
    failures();
    if (!failed)
        puts("fstool-io: passed");
    return failed != 0;
}
