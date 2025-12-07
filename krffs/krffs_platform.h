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

#ifdef WINDOWS
#define PLATFORM_PREFIX(CALL) _ ## CALL
#else
#define PLATFORM_PREFIX(CALL) CALL
#endif

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

#ifdef WINDOWS
#define fsblkcnt_t fsblkcnt64_t
#define fsfilcnt_t fsfilcnt64_t
#endif

#ifdef WINDOWS
#define S_ISREG(MODE) (((MODE) & S_IFMT) == S_IFREG)
#endif

void krffs_get_time_of_day(struct timeval *time_of_day);
void *krffs_map_file(int file_descriptor, off_t offset, size_t size);
int krffs_sync_mapping(void *address, size_t size);
int krffs_unmap_file(void *address, size_t size);

#endif
