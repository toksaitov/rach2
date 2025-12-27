/*
 * sys/stat.h - File stat structures for Doom on Rach2
 */
#ifndef _SYS_STAT_H
#define _SYS_STAT_H

#include <stddef.h>
#include <sys/types.h>

typedef long blksize_t;
typedef long blkcnt_t;

struct stat {
    dev_t st_dev;
    ino_t st_ino;
    mode_t st_mode;
    nlink_t st_nlink;
    uid_t st_uid;
    gid_t st_gid;
    dev_t st_rdev;
    off_t st_size;
    blksize_t st_blksize;
    blkcnt_t st_blocks;
    long st_atime;
    long st_mtime;
    long st_ctime;
};

#define S_IFMT   0170000
#define S_IFDIR  0040000
#define S_IFREG  0100000

#define S_ISDIR(m)  (((m) & S_IFMT) == S_IFDIR)
#define S_ISREG(m)  (((m) & S_IFMT) == S_IFREG)

static inline int stat(const char *path, struct stat *buf) {
    (void)path; (void)buf;
    return -1;
}

static inline int fstat(int fd, struct stat *buf) {
    (void)fd; (void)buf;
    return -1;
}

static inline int mkdir(const char *path, mode_t mode) {
    (void)path; (void)mode;
    return -1;
}

#endif /* _SYS_STAT_H */
