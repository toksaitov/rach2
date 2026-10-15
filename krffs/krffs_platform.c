/*
 * krffs_platform.c: the Unix and Windows code behind krffs_platform.h.
 */

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
 * Reads `s` as one number in any C base, as `strtoull` does, into `*out`.
 * Returns 0, or -1 for an empty string, characters after the number or a
 * value past `unsigned long long`.
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
    *out = (uint64_t)value;

    return 0;
}

int krffs_parse_region_args(int argc, char **argv, uint64_t file_size, uint64_t min_size,
                            off_t *out_offset, size_t *out_size)
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
        fprintf(stderr, "krffs: --offset %llu is not %d-aligned\n", (unsigned long long)offset,
                KRFFS_REGION_ALIGN);
        return -1;
    }
    if (offset > file_size) {
        fprintf(stderr, "krffs: --offset %llu is past the file end (%llu)\n",
                (unsigned long long)offset, (unsigned long long)file_size);
        return -1;
    }
    if (offset > 0 && !have_size) {
        fprintf(stderr, "krffs: --offset requires an explicit --size\n");
        return -1;
    }
    if (!have_size) {
        size = file_size - offset;
    }
    if (size < min_size || size > file_size - offset || size > (uint64_t)SIZE_MAX) {
        fprintf(stderr,
                "krffs: region (offset %llu, size %llu) does not fit a %llu-byte "
                "file system in a %llu-byte file\n",
                (unsigned long long)offset, (unsigned long long)size, (unsigned long long)min_size,
                (unsigned long long)file_size);
        return -1;
    }
    if (size > KRFFS_MAX_REGION_SIZE) {
        fprintf(stderr, "krffs: region size %llu exceeds the %u-byte KRFFS limit\n",
                (unsigned long long)size, (unsigned)KRFFS_MAX_REGION_SIZE);
        return -1;
    }

    *out_offset = (off_t)offset;
    *out_size = (size_t)size;

    return 0;
}

/**
 * On Windows the seconds come from the FILETIME count of 100-nanosecond
 * ticks, less `epoch`, the count from 1601 to 1970.
 */
void krffs_get_time_of_day(struct timeval *time_of_day)
{
#ifdef WINDOWS
    static const uint64_t epoch = 116444736000000000ULL;

    SYSTEMTIME system_time;
    GetSystemTime(&system_time);

    FILETIME file_time;
    SystemTimeToFileTime(&system_time, &file_time);

    ULARGE_INTEGER time;
    time.QuadPart = ((uint64_t)file_time.dwHighDateTime << 32) | file_time.dwLowDateTime;

    time_of_day->tv_sec = (long)((time.QuadPart - epoch) / 10000000L);
    time_of_day->tv_usec = (long)(system_time.wMilliseconds * 1000);
#else
    gettimeofday(time_of_day, NULL);
#endif
}

void *krffs_map_file(int file_descriptor, off_t offset, size_t size)
{
#ifdef WINDOWS
    void *result = (void *)-1;

    DWORD offset_high = (DWORD)((uint64_t)offset >> 32);
    DWORD offset_low = (DWORD)((uint64_t)offset & 0xFFFFFFFF);

    HANDLE file_handle = (HANDLE)_get_osfhandle(file_descriptor);

    HANDLE mapping_handle = CreateFileMapping(file_handle, NULL, PAGE_READWRITE, 0, 0, NULL);

    if (mapping_handle) {
        result = MapViewOfFile(mapping_handle, FILE_MAP_WRITE, offset_high, offset_low, size);

        if (result) {
            CloseHandle(mapping_handle);
        } else {
            result = (void *)-1;
        }
    }

    return result;
#else
    return mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, file_descriptor, offset);
#endif
}

void *krffs_map_file_readonly(int file_descriptor, off_t offset, size_t size)
{
#ifdef WINDOWS
    void *result = (void *)-1;

    DWORD offset_high = (DWORD)((uint64_t)offset >> 32);
    DWORD offset_low = (DWORD)((uint64_t)offset & 0xFFFFFFFF);

    HANDLE file_handle = (HANDLE)_get_osfhandle(file_descriptor);

    HANDLE mapping_handle = CreateFileMapping(file_handle, NULL, PAGE_READONLY, 0, 0, NULL);

    if (mapping_handle) {
        result = MapViewOfFile(mapping_handle, FILE_MAP_READ, offset_high, offset_low, size);

        if (result) {
            CloseHandle(mapping_handle);
        } else {
            result = (void *)-1;
        }
    }

    return result;
#else
    return mmap(NULL, size, PROT_READ, MAP_SHARED, file_descriptor, offset);
#endif
}

int krffs_sync_mapping(void *address, size_t size)
{
#ifdef WINDOWS
    return FlushViewOfFile(address, size) == 0 ? -1 : 0;
#else
    return msync(address, size, MS_SYNC);
#endif
}

int krffs_unmap_file(void *address, size_t size)
{
#ifdef WINDOWS
    return UnmapViewOfFile(address) == 0 ? -1 : 0;
#else
    return munmap(address, size);
#endif
}
