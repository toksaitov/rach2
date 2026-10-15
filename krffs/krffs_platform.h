/*
 * krffs_platform.h: what the KRFFS host tools take from Unix or Windows: the
 * POSIX names, the clock, the mapping of an image, and the region arguments.
 */
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
 * The offset and `stat` types, taken on Windows from the FUSE headers when
 * they define `FUSE_OFF_T` and `FUSE_STAT`.
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

/** The `statvfs` count types, which have 64-bit names on Windows. */
#ifdef WINDOWS
#define fsblkcnt_t fsblkcnt64_t
#define fsfilcnt_t fsfilcnt64_t
#endif

/** `S_ISREG`, which Windows lacks. */
#ifdef WINDOWS
#define S_ISREG(MODE) (((MODE) & S_IFMT) == S_IFREG)
#endif

/**
 * Reads the wall clock into `time_of_day`: `gettimeofday` on Unix, and on
 * Windows the system time, whose microseconds are its milliseconds times 1000.
 */
void krffs_get_time_of_day(struct timeval *time_of_day);

/**
 * The alignment of a region's byte offset, 1 MiB: a multiple of the mapping
 * granularity of every host (64 KiB for `MapViewOfFile`, a page for `mmap`)
 * and of the sector size, so a partition there is aligned for both.
 */
#define KRFFS_REGION_ALIGN (1024 * 1024)

/**
 * The largest region, 4095 MiB, the largest whole number of MiB under 4 GiB:
 * the kernel keeps a file system's size in a `uint32_t`
 * (`krffs_file_system_t` in rach2/sys/fs/krffs.h).
 */
#define KRFFS_MAX_REGION_SIZE (4095u * 1024u * 1024u)

/**
 * Reads the `--offset BYTES` and `--size BYTES` arguments, numbers in any C
 * base, that place a KRFFS region in a file of `file_size` bytes, such as a
 * partition of a disk image. Other arguments are skipped, so each tool parses
 * its own, and of a repeated flag the last counts. The offset defaults to 0
 * and must be a multiple of `KRFFS_REGION_ALIGN`. The size defaults to the
 * rest of the file, but a nonzero offset needs one, since a partition does
 * not reach the end of the disk. The region must lie in the file and be from
 * `min_size`, the smallest file system the caller can work on, to
 * `KRFFS_MAX_REGION_SIZE` bytes.
 *
 * Returns 0 and sets `*out_offset` and `*out_size`, or returns -1, with a
 * `krffs: detail` message on stderr, for a flag with no number after it or a
 * region that breaks a rule.
 */
int krffs_parse_region_args(int argc, char **argv, uint64_t file_size, uint64_t min_size,
                            off_t *out_offset, size_t *out_size);

/**
 * Maps `size` bytes of the file open on `file_descriptor`, from `offset`,
 * shared and writable, so a store into the mapping is a store into the file:
 * `mmap` on Unix, `MapViewOfFile` on Windows. `offset` must be a multiple of
 * the mapping granularity. Returns the address, or `(void *)-1` on failure,
 * as `mmap` does. On Windows the mapping object's handle is closed before the
 * return, since the view keeps the object.
 */
void *krffs_map_file(int file_descriptor, off_t offset, size_t size);

/**
 * Maps the region as `krffs_map_file` does, for reading only, so a tool that
 * only reads an image can open it `O_RDONLY`. Returns the address, or
 * `(void *)-1` on failure.
 */
void *krffs_map_file_readonly(int file_descriptor, off_t offset, size_t size);

/**
 * Writes `size` bytes of a mapping at `address` back to the file and waits
 * for the write: `msync` with `MS_SYNC` on Unix, `FlushViewOfFile` on
 * Windows. On Unix `address` must be a multiple of the page size, or `msync`
 * fails with `EINVAL`. Returns 0, or -1 on failure.
 */
int krffs_sync_mapping(void *address, size_t size);

/**
 * Unmaps a mapping from `krffs_map_file` or `krffs_map_file_readonly`:
 * `munmap` on Unix, `UnmapViewOfFile` on Windows, which ignores `size` and
 * needs the address the map returned. Returns 0, or -1 on failure.
 */
int krffs_unmap_file(void *address, size_t size);

#endif
