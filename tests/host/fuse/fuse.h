/*
 * The part of libfuse 2's <fuse.h> that krffs's FUSE operations use, so that
 * krffs_fuse_hosttest.c builds them on a host without libfuse.
 */
#ifndef TEST_FUSE_H
#define TEST_FUSE_H

#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/types.h>
#include <time.h>

struct fuse_file_info {
    int flags;
};
struct fuse_context {
    uid_t uid;
    gid_t gid;
};
typedef int (*fuse_fill_dir_t)(void *, const char *, const struct stat *, off_t);
struct fuse_operations {
    int (*getattr)(const char *, struct stat *);
    int (*readdir)(const char *, void *, fuse_fill_dir_t, off_t, struct fuse_file_info *);
    int (*open)(const char *, struct fuse_file_info *);
    int (*read)(const char *, char *, size_t, off_t, struct fuse_file_info *);
    int (*mknod)(const char *, mode_t, dev_t);
    int (*create)(const char *, mode_t, struct fuse_file_info *);
    int (*mkdir)(const char *, mode_t);
    int (*write)(const char *, const char *, size_t, off_t, struct fuse_file_info *);
    int (*fsync)(const char *, int, struct fuse_file_info *);
    int (*truncate)(const char *, off_t);
    int (*unlink)(const char *);
    int (*rmdir)(const char *);
    int (*rename)(const char *, const char *);
    int (*utimens)(const char *, const struct timespec[2]);
    int (*chmod)(const char *, mode_t);
    int (*chown)(const char *, uid_t, gid_t);
    int (*statfs)(const char *, struct statvfs *);
};
/** Mount and serve `ops`; the host test defines it to count the call. */
int fuse_main(int argc, char **argv, const struct fuse_operations *ops, void *data);

/** The identity of the caller of the request under way; fixed in the test. */
struct fuse_context *fuse_get_context(void);

#endif
