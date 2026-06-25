#ifndef KRFFS_PLATFORM_H
#define KRFFS_PLATFORM_H

#include <stddef.h>
#include <sys/stat.h>
#include <sys/types.h>

#if defined(_WIN32) || defined(_WIN64)
#define WINDOWS
#else
#define UNIX
#endif

#ifdef __APPLE__
#define APPLE
#endif

#ifdef __linux__
#define LINUX
#endif

#ifdef WINDOWS
#include <windows.h>
#else
#include <unistd.h>
#include <sys/time.h>
#endif

/**
 * Prefixes a POSIX call name with the platform's spelling.
 *
 * Windows names the POSIX functions with a leading underscore (`_stat64`);
 * Unix uses the bare name.
 */
#ifdef WINDOWS
#define PLATFORM_PREFIX(CALL) _ ## CALL
#else
#define PLATFORM_PREFIX(CALL) CALL
#endif

/**
 * File-information types that differ by platform.
 */
#ifdef WINDOWS
#ifdef FUSE_OFF_T
#define PLATFORM_OFF_T FUSE_OFF_T
#else
#define PLATFORM_OFF_T long long
#endif
#ifdef FUSE_STAT
#define PLATFORM_STAT FUSE_STAT
#else
#define PLATFORM_STAT _stat64
#endif
#else
#define PLATFORM_OFF_T off_t
#define PLATFORM_STAT stat
#endif

/**
 * File-system-information count types that differ by platform.
 */
#ifdef WINDOWS
#define fsblkcnt_t fsblkcnt64_t
#define fsfilcnt_t fsfilcnt64_t
#endif

/**
 * Defines `S_ISREG` on Windows, which lacks it.
 */
#ifdef WINDOWS
#define S_ISREG(MODE) (((MODE) & S_IFMT) == S_IFREG)
#endif

/**
 * Reads the current wall-clock time with microsecond precision.
 */
void krffs_get_time_of_day(struct timeval *time_of_day);

/**
 * Maps a region of a file into virtual memory.
 *
 * Reads and writes through the returned pages read and write the file. Uses
 * `mmap` on Unix and `MapViewOfFile` on Windows.
 */
void *krffs_map_file(
          int file_descriptor,
          off_t offset,
          size_t size
      );

/**
 * Flushes a mapped region back to its backing file.
 *
 * Uses `msync` on Unix and `FlushViewOfFile` on Windows.
 */
int krffs_sync_mapping(
        void *address,
        size_t size
    );

/**
 * Unmaps a region previously mapped by `krffs_map_file`.
 *
 * On Windows the size is ignored and `address` must point to the first mapped
 * page. Uses `munmap` on Unix and `UnmapViewOfFile` on Windows.
 */
int krffs_unmap_file(
        void *address,
        size_t size
    );

#endif
