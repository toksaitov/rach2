#ifndef KRFFS_PLATFORM_H
#define KRFFS_PLATFORM_H

#include <stddef.h>
#include <stdint.h>
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
#define PLATFORM_PREFIX(CALL) _##CALL
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
#define PLATFORM_STAT  stat
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

/* The alignment a KRFFS region's byte offset must satisfy: 1 MiB. It is the
 * coarsest mmap granularity across hosts (Windows MapViewOfFile needs 64 KiB,
 * Unix a page) and keeps a partition both sector- and page-aligned. */
#define KRFFS_REGION_ALIGN (1024 * 1024)

/* The largest KRFFS region: its size is stored in a uint32_t, so it must stay
 * under 4 GiB. Capped at 4095 MiB to leave the value clear of the 32-bit edge.
 */
#define KRFFS_MAX_REGION_SIZE (4095u * 1024u * 1024u)

/**
 * Parse the shared `--offset BYTES` and `--size BYTES` region arguments and
 * validate them against a backing file of `file_size` bytes.
 *
 * A region lets a tool target a KRFFS that lives inside a partition of a larger
 * disk image, not at the start of a whole file. `--offset` defaults to 0
 * and must be a multiple of `KRFFS_REGION_ALIGN`. `--size` defaults to the rest
 * of the file after the offset, but a nonzero `--offset` names a partition that
 * does not extend to the end of the disk, so it requires an explicit `--size`.
 * The region must fit in the file and be at least `min_size` bytes (the caller
 * passes the smallest file system it can hold). A flag with no following value
 * is an error, not a silent default. Other arguments are ignored, so
 * each tool keeps its own option parsing.
 *
 * On success returns 0 and writes `*out_offset` and `*out_size`; on a bad or
 * out of range region returns -1 with a `krffs: detail` message on stderr.
 */
int krffs_parse_region_args(int argc, char **argv, uint64_t file_size, uint64_t min_size,
                            off_t *out_offset, size_t *out_size);

/**
 * Maps a region of a file into virtual memory.
 *
 * Reads and writes through the returned pages read and write the file. Uses
 * `mmap` on Unix and `MapViewOfFile` on Windows.
 */
void *krffs_map_file(int file_descriptor, off_t offset, size_t size);

/**
 * Maps a region of a file into virtual memory for reading only.
 *
 * Returns `(void *) -1` on failure, as `mmap` does.
 */
void *krffs_map_file_readonly(int file_descriptor, off_t offset, size_t size);

/**
 * Flushes a mapped region back to its backing file.
 *
 * Uses `msync` on Unix and `FlushViewOfFile` on Windows.
 */
int krffs_sync_mapping(void *address, size_t size);

/**
 * Unmaps a region previously mapped by `krffs_map_file`.
 *
 * On Windows the size is ignored and `address` must point to the first mapped
 * page. Uses `munmap` on Unix and `UnmapViewOfFile` on Windows.
 */
int krffs_unmap_file(void *address, size_t size);

#endif
