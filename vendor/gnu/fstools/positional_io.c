/*
 * exfatprogs writes individual FAT entries. Rach2 recovery block nodes
 * require whole-sector writes. Adapt only these tools, which open writable
 * devices O_RDWR|O_EXCL and run one thread. The raw-window writer claim
 * excludes other raw writers while a partial sector is read and changed.
 * Regular-file writes and descriptor offsets retain their original behavior.
 */
#ifdef _FILE_OFFSET_BITS
#undef _FILE_OFFSET_BITS
#endif
#define _FILE_OFFSET_BITS 64
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

/* GNU pwrite uses a signed long offset, pwrite64 a signed 64-bit offset.
 * fstat still needs its large-file ABI for block nodes larger than 2 GiB. */
extern ssize_t __real_pwrite(int, const void *, size_t, long);
extern ssize_t __real_pwrite64(int, const void *, size_t, int64_t);
extern ssize_t pread64(int, void *, size_t, int64_t);

static int block_descriptor(int fd)
{
    struct stat st;
    if (fstat(fd, &st))
        return -1;
    return S_ISBLK(st.st_mode) != 0;
}

static ssize_t sector_write(int fd, const unsigned char *data, size_t size, int64_t offset)
{
    unsigned char sector[512];
    size_t skip = (size_t)(offset % 512);
    int64_t start = offset - (int64_t)skip;
    ssize_t n;
    do {
        n = pread64(fd, sector, sizeof(sector), start);
    } while (n < 0 && errno == EINTR);
    if (n != (ssize_t)sizeof(sector)) {
        if (n >= 0)
            errno = EIO;
        return -1;
    }
    memcpy(sector + skip, data, size);
    do {
        n = __real_pwrite64(fd, sector, sizeof(sector), start);
    } while (n < 0 && errno == EINTR);
    if (n != (ssize_t)sizeof(sector)) {
        if (n >= 0)
            errno = EIO;
        return -1;
    }
    return (ssize_t)size;
}

static ssize_t block_write(int fd, const void *buffer, size_t count, int64_t offset)
{
    const unsigned char *data = buffer;
    size_t done = 0;
    if (offset < 0 || count > SSIZE_MAX || (uint64_t)count > (uint64_t)(INT64_MAX - offset)) {
        errno = EINVAL;
        return -1;
    }
    while (done < count) {
        int64_t at = offset + (int64_t)done;
        size_t left = count - done, size;
        ssize_t n;
        if (at % 512 || left < 512) {
            size = 512 - (size_t)(at % 512);
            if (size > left)
                size = left;
            n = sector_write(fd, data + done, size, at);
        } else {
            size = left - left % 512;
            do {
                n = __real_pwrite64(fd, data + done, size, at);
            } while (n < 0 && errno == EINTR);
        }
        if (n < 0)
            return done ? (ssize_t)done : -1;
        done += (size_t)n;
        if ((size_t)n != size)
            break;
    }
    return (ssize_t)done;
}

ssize_t __wrap_pwrite(int fd, const void *buffer, size_t count, long offset)
{
    int block = block_descriptor(fd);
    if (block < 0)
        return -1;
    if (!block || !count)
        return __real_pwrite(fd, buffer, count, offset);
    return block_write(fd, buffer, count, offset);
}

ssize_t __wrap_pwrite64(int fd, const void *buffer, size_t count, int64_t offset)
{
    int block = block_descriptor(fd);
    if (block < 0)
        return -1;
    if (!block || !count)
        return __real_pwrite64(fd, buffer, count, offset);
    return block_write(fd, buffer, count, offset);
}
