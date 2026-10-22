/* files.h: native Windows files used by the shared collection driver. */
#ifndef ETUDE_WIN_FILES_H
#define ETUDE_WIN_FILES_H
#include <stdio.h>
#include <time.h>
#include <stdint.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <io.h>
#include <direct.h>
#include <unistd.h>
#include <dirent.h>
#include "winexec.h"

/* File identity and times from the handle, including files beyond 4 GiB. */
struct etude_win_stat {
    uint64_t st_dev, st_ino;
    int64_t st_size;
    unsigned st_mode;
    struct timespec st_mtim, st_ctim;
};
typedef struct etude_win_dir etude_win_dir;
struct etude_win_dirent {
    char d_name[32768];
};

/** Read PATH's status, following a reparse point. */
int etude_win_stat(const char *path, struct etude_win_stat *st);
/** Read PATH's status without traversing a reparse point. */
int etude_win_lstat(const char *path, struct etude_win_stat *st);
/** Read FD's status from its native handle. */
int etude_win_fstat(int fd, struct etude_win_stat *st);
/** Open a UTF-8 path, always binary and noninheritable. */
int etude_win_open(const char *path, int flags, ...);
/** Open a stream on a UTF-8 path. */
FILE *etude_win_fopen(const char *path, const char *mode);
/** Return an absolute UTF-8 path in OUT, or allocated when OUT is NULL. */
char *etude_win_realpath(const char *path, char *out);
/** Make a directory; MODE is the portable caller's permission mask. */
int etude_win_mkdir(const char *path, int mode);
/** Remove one file, including a read-only artifact. */
int etude_win_unlink(const char *path);
/** Remove an empty directory. */
int etude_win_rmdir(const char *path);
/** Replace TO atomically with FROM, on the same volume. */
int etude_win_rename(const char *from, const char *to);
/** Publish a hard link without replacing an existing file. */
int etude_win_link(const char *from, const char *to);
/** Change the file's read-only attribute by MODE's write bits. */
int etude_win_chmod(const char *path, int mode);
/** Change FD's read-only attribute by MODE's write bits. */
int etude_win_fchmod(int fd, int mode);
/** Check whether PATH exists and, with write access, is writable. */
int etude_win_access(const char *path, int mode);
/** Reserve TEMPLATE, whose final six bytes are XXXXXX, as a private file. */
int etude_win_mkstemp(char *path);
/** Reserve TEMPLATE as a private directory. */
char *etude_win_mkdtemp(char *path);
/** Set FD's length without changing its current offset. */
int etude_win_truncate(int fd, int64_t size);
/**
 * Lock FD's whole file; TYPE is read, write or unlock, WAIT permits waiting.
 */
int etude_win_file_lock(int fd, int type, int wait);
/** Make FD noninheritable; other commands are rejected. */
int etude_win_fcntl(int fd, int command, ...);
/** Open a directory without traversing its entries. */
struct etude_win_dir *etude_win_opendir(const char *path);
/** Read the next UTF-8 entry, or NULL at the end or an error. */
struct etude_win_dirent *etude_win_readdir(struct etude_win_dir *dir);
/** Close a directory. */
int etude_win_closedir(struct etude_win_dir *dir);
/** Translate the last Windows error to errno and return -1. */
int etude_win_file_error(void);
/** Say whether PATH is absolute, including a drive or a UNC server. */
int etude_win_absolute(const char *path);
/** Return the native temporary directory as UTF-8. */
const char *etude_win_temp(void);
/** Read the boot environment and kernel start time as one identifier. */
int etude_win_boot_id(char *out, size_t size);
/** Record the Windows kernel version and build, or an unavailable value. */
void etude_win_version(char *out, size_t size);
/** Locate a program on native PATH, including its .exe suffix. */
int etude_win_program(const char *name, char *out, size_t size);
/** Return a POSIX shell supplied by MSYS2, or NULL. */
const char *etude_win_shell(void);

/** Read one line, growing its caller-owned buffer. */
ssize_t etude_win_getline(char **line, size_t *capacity, FILE *file);
/** Convert a Unix time to UTC in caller-owned storage. */
struct tm *etude_win_gmtime(const time_t *time, struct tm *out);
#define getline      etude_win_getline
#define gmtime_r     etude_win_gmtime
#define stat         etude_win_stat
#define fstat        etude_win_fstat
#define lstat        etude_win_lstat
#define open         etude_win_open
#define fopen        etude_win_fopen
#define realpath     etude_win_realpath
#define mkdir        etude_win_mkdir
#define unlink       etude_win_unlink
#define rmdir        etude_win_rmdir
#define rename       etude_win_rename
#define link         etude_win_link
#define chmod        etude_win_chmod
#define fchmod       etude_win_fchmod
#define access       etude_win_access
#define mkstemp      etude_win_mkstemp
#define mkdtemp      etude_win_mkdtemp
#define fsync        _commit
#define ftruncate    etude_win_truncate
#define lseek        _lseeki64
#define off_t        int64_t
#define fcntl        etude_win_fcntl
#define DIR          etude_win_dir
#define dirent       etude_win_dirent
#define opendir      etude_win_opendir
#define readdir      etude_win_readdir
#define closedir     etude_win_closedir
#define posix_now_us etude_win_mono_us

#undef O_CLOEXEC
#define O_CLOEXEC  _O_NOINHERIT
#define O_NOFOLLOW 0x20000000
#define F_SETFD    1
#define FD_CLOEXEC 1
#define F_RDLCK    1
#define F_WRLCK    2
#define F_UNLCK    3
#ifndef S_IRWXU
#define S_IRWXU 0700
#endif
#define ETUDE_SHELL etude_win_shell()
#endif
