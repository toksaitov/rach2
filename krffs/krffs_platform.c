#include "krffs_platform.h"

#ifdef WINDOWS
#include <io.h>
#else
#include <sys/mman.h>
#endif

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/**
 * Parse one `unsigned long long` from `s` in any C base; -1 on trailing junk or
 * a value that overflows the type.
 */
static int krffs_parse_u64(const char *s, uint64_t *out)
{
    char *end;
    unsigned long long value;

    errno = 0;
    value = strtoull(s, &end, 0);
    if (end == s || *end != '\0' || errno != 0) {
        return -1;
    }
    *out = (uint64_t) value;

    return 0;
}

int krffs_parse_region_args(
        int argc,
        char **argv,
        uint64_t file_size,
        uint64_t min_size,
        off_t *out_offset,
        size_t *out_size
    )
{
    uint64_t offset = 0;
    uint64_t size = 0;
    int have_size = 0;
    int i;

    for (i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--offset") == 0) {
            if (i + 1 >= argc || krffs_parse_u64(argv[++i], &offset) != 0) {
                fprintf(stderr, "krffs: --offset needs a numeric argument\n");
                return -1;
            }
        } else if (strcmp(argv[i], "--size") == 0) {
            if (i + 1 >= argc || krffs_parse_u64(argv[++i], &size) != 0) {
                fprintf(stderr, "krffs: --size needs a numeric argument\n");
                return -1;
            }
            have_size = 1;
        }
    }

    if (offset % KRFFS_REGION_ALIGN != 0) {
        fprintf(stderr, "krffs: --offset %llu is not %d-aligned\n",
                (unsigned long long) offset, KRFFS_REGION_ALIGN);
        return -1;
    }
    if (offset > file_size) {
        fprintf(stderr, "krffs: --offset %llu is past the file end (%llu)\n",
                (unsigned long long) offset, (unsigned long long) file_size);
        return -1;
    }
    /* A nonzero offset means a partition, which does not extend to the end of the
       disk, so its size must be given rather than defaulted to the rest of the
       file. The size defaults to the rest of the file only for a whole-file FS. */
    if (offset > 0 && !have_size) {
        fprintf(stderr, "krffs: --offset requires an explicit --size\n");
        return -1;
    }
    if (!have_size) {
        size = file_size - offset;
    }
    /* file_size - offset cannot underflow: offset <= file_size above. */
    if (size < min_size || size > file_size - offset || size > (uint64_t) SIZE_MAX) {
        fprintf(stderr,
                "krffs: region (offset %llu, size %llu) does not fit a %llu-byte "
                "file system in a %llu-byte file\n",
                (unsigned long long) offset, (unsigned long long) size,
                (unsigned long long) min_size, (unsigned long long) file_size);
        return -1;
    }
    /* KRFFS stores its size in a uint32_t, so a region must stay under 4 GiB. */
    if (size > KRFFS_MAX_REGION_SIZE) {
        fprintf(stderr, "krffs: region size %llu exceeds the %u-byte KRFFS limit\n",
                (unsigned long long) size, (unsigned) KRFFS_MAX_REGION_SIZE);
        return -1;
    }

    *out_offset = (off_t) offset;
    *out_size = (size_t) size;

    return 0;
}

/**
 * Reads the wall-clock time with microsecond precision.
 *
 * Uses `gettimeofday` on Unix; on Windows the FILETIME 100-nanosecond ticks
 * are rebased from the 1601 epoch to the Unix epoch and split into seconds
 * and microseconds.
 */
void krffs_get_time_of_day(struct timeval *time_of_day)
{
#ifdef WINDOWS
    static const uint64_t epoch =
        116444736000000000ULL;

    SYSTEMTIME system_time;
    GetSystemTime(&system_time);

    FILETIME file_time;
    SystemTimeToFileTime(&system_time, &file_time);

    ULARGE_INTEGER time;
    time.LowPart =
        file_time.dwLowDateTime;
    time.HighPart =
        file_time.dwHighDateTime;

    time_of_day->tv_sec =
        (long) ((time.QuadPart - epoch) / 10000000L);
    time_of_day->tv_usec =
        (long) (system_time.wMilliseconds * 1000);
#else
    gettimeofday(time_of_day, NULL);
#endif
}

/**
 * Maps `size` bytes of the file at `file_descriptor`, starting at `offset`,
 * into shared read/write memory.
 *
 * Reads and writes through the returned pages read and write the file. Uses
 * `mmap` on Unix and `MapViewOfFile` on Windows. Returns `(void *) -1` on
 * failure.
 */
void *krffs_map_file(
          int file_descriptor,
          off_t offset,
          size_t size
      )
{
#ifdef WINDOWS
    void *result =
        (void *) -1;

    DWORD offset_high =
        (DWORD) ((uint64_t) offset >> 32);
    DWORD offset_low =
        (DWORD) ((uint64_t) offset & 0xFFFFFFFF);

    HANDLE file_handle =
        (HANDLE) _get_osfhandle(file_descriptor);

    HANDLE mapping_handle =
        CreateFileMapping(
            file_handle,
            NULL,
            PAGE_READWRITE,
            0,
            0,
            NULL
        );

    if (mapping_handle) {
        result =
            MapViewOfFile(
                mapping_handle,
                FILE_MAP_WRITE,
                offset_high,
                offset_low,
                size
            );

        if (result) {
            CloseHandle(
                mapping_handle
            );
        } else {
            result =
                (void *) -1;
        }
    }

    return result;
#else
    return mmap(
               NULL,
               size,
               PROT_READ | PROT_WRITE,
               MAP_SHARED,
               file_descriptor,
               offset
           );
#endif
}

/**
 * Flushes `size` bytes of a mapping at `address` back to its file.
 *
 * Uses a synchronous `msync` (`MS_SYNC`) on Unix and `FlushViewOfFile` on
 * Windows. Returns 0 on success and -1 on failure.
 */
int krffs_sync_mapping(
        void *address,
        size_t size
    )
{
#ifdef WINDOWS
    return FlushViewOfFile(
               address,
               size
           ) == 0 ? -1 : 0;
#else
    return msync(
               address,
               size,
               MS_SYNC
           );
#endif
}

/**
 * Unmaps a region returned by `krffs_map_file`.
 *
 * Uses `munmap` on Unix and `UnmapViewOfFile` on Windows. On Windows `size`
 * is ignored and `address` must be the first mapped page. Returns 0 on
 * success and -1 on failure.
 */
int krffs_unmap_file(
        void *address,
        size_t size
    )
{
#ifdef WINDOWS
    return UnmapViewOfFile(
               address
           ) == 0 ? -1 : 0;
#else
    return munmap(
               address,
               size
           );
#endif
}
