/* portlock.c: take the host lock and a port block that runs on a host share. */
#include <errno.h>
#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#include "etude/portlock.h"

/* The longest path of a lock file. */
#define PATH_MAX_BYTES 4096

#ifdef _WIN32

typedef HANDLE lockfile;
#define NO_FILE INVALID_HANDLE_VALUE

/* The Win32 error of the last take that gave EIO, or 0. */
static DWORD unmapped;

/** Give the errno value nearest the Win32 error `err`, keeping one of EIO. */
static int win_errno(DWORD err)
{
    switch (err) {
    case ERROR_FILE_NOT_FOUND:
    case ERROR_PATH_NOT_FOUND:
        return ENOENT;
    case ERROR_ACCESS_DENIED:
    case ERROR_SHARING_VIOLATION:
        return EACCES;
    case ERROR_LOCK_VIOLATION:
        return EAGAIN;
    case ERROR_NOT_ENOUGH_MEMORY:
    case ERROR_OUTOFMEMORY:
        return ENOMEM;
    default:
        unmapped = err;
        return EIO;
    }
}

unsigned long etude_portlock_win_error(void)
{
    return unmapped;
}

/** Open the lock file `path`, creating it when it is not there. */
static lockfile open_file(const char *path)
{
    wchar_t w[PATH_MAX_BYTES];
    HANDLE h;

    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, w, PATH_MAX_BYTES) == 0) {
        errno = EINVAL;
        return NO_FILE;
    }
    h = CreateFileW(w, GENERIC_READ | GENERIC_WRITE,
                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_ALWAYS,
                    FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE)
        errno = win_errno(GetLastError());
    else if (GetFileType(h) != FILE_TYPE_DISK) {
        CloseHandle(h);
        errno = EINVAL;
        return NO_FILE;
    }
    return h;
}

/** Try a lock of the whole of `f`, giving EAGAIN when another holds it. */
static int try_lock(lockfile f, int exclusive)
{
    OVERLAPPED ov;
    DWORD flags = LOCKFILE_FAIL_IMMEDIATELY | (exclusive ? LOCKFILE_EXCLUSIVE_LOCK : 0);

    memset(&ov, 0, sizeof(ov));
    if (LockFileEx(f, flags, 0, MAXDWORD, MAXDWORD, &ov))
        return 0;
    errno = win_errno(GetLastError());
    return -1;
}

/** Give back the lock of `f`, if it has one, and close it. */
static void close_file(lockfile f)
{
    OVERLAPPED ov;

    memset(&ov, 0, sizeof(ov));
    UnlockFileEx(f, 0, MAXDWORD, MAXDWORD, &ov);
    CloseHandle(f);
}

#else

typedef int lockfile;
#define NO_FILE (-1)

/**
 * Open the lock file `path`, which every account on the host must be able
 * to lock. The first opener creates it mode 0666 with the umask cleared, so
 * that no account finds it narrower, and every opener sets the mode again,
 * which repairs a narrower file of its own account. An existing file is
 * opened without O_CREAT, which Linux refuses on another account's file in a
 * sticky directory such as /tmp. The caller is single-threaded, so nothing
 * else sees the cleared umask.
 */
static lockfile open_file(const char *path)
{
    struct stat st;
    mode_t mask;
    int fd;

    fd = open(path, O_RDWR | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0 && errno == ENOENT) {
        mask = umask(0);
        fd = open(path, O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0666);
        umask(mask);
        if (fd < 0 && errno == EEXIST)
            fd = open(path, O_RDWR | O_CLOEXEC | O_NOFOLLOW);
    }
    if (fd < 0)
        return NO_FILE;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
        close(fd);
        errno = EINVAL;
        return NO_FILE;
    }
    (void)fchmod(fd, 0666);
    return fd;
}

/** Try a lock of the whole of `f`, giving EAGAIN when another holds it. */
static int try_lock(lockfile f, int exclusive)
{
    struct flock fl;
    short type = exclusive ? F_WRLCK : F_RDLCK;

    memset(&fl, 0, sizeof(fl));
    fl.l_type = type;
    fl.l_whence = SEEK_SET;
    if (fcntl(f, F_SETLK, &fl) == 0)
        return 0;
    if (errno == EACCES)
        errno = EAGAIN;
    return -1;
}

/** Close `f`, which gives back its lock. */
static void close_file(lockfile f)
{
    close(f);
}

#endif

/** Open `dir/name`, writing its path to `path`. */
static lockfile open_lock(const char *dir, const char *name, char *path)
{
    int n = snprintf(path, PATH_MAX_BYTES, "%s/%s", dir, name);

    if (n < 0 || n >= PATH_MAX_BYTES) {
        errno = ENAMETOOLONG;
        return NO_FILE;
    }
    return open_file(path);
}

/** Pause as `s` asks, or end the wait with EAGAIN when it gives no pause. */
static int pause_for(const struct etude_portlock_spec *s, const struct etude_portlock_wait *w)
{
    if (s->pause == NULL) {
        errno = EAGAIN;
        return -1;
    }
    return s->pause(s->ctx, w);
}

/** Take a lock of `f`, pausing while another process holds it. */
static int hold(const struct etude_portlock_spec *s, lockfile f, int exclusive, const char *path)
{
    struct etude_portlock_wait w;

    w.what = "host";
    w.path = path;
    w.exclusive = exclusive;
    w.count = 0;
    w.first = 1;
    while (try_lock(f, exclusive) != 0) {
        if (errno != EAGAIN || pause_for(s, &w) != 0)
            return -1;
        w.first = 0;
    }
    return 0;
}

/** Take the gate and then the host lock, both of one kind. */
static int host_take(struct etude_portlock *l, const struct etude_portlock_spec *s)
{
    char gate[PATH_MAX_BYTES], host[PATH_MAX_BYTES];
    int alone = s->host == ETUDE_PORTLOCK_ALONE;

    if ((l->file[0] = open_lock(s->dir, "rach2-host.gate", gate)) == NO_FILE ||
        (l->file[1] = open_lock(s->dir, "rach2-host.lock", host)) == NO_FILE ||
        hold(s, l->file[0], alone, gate) != 0 || hold(s, l->file[1], alone, host) != 0)
        return -1;
    if (!alone) {
        close_file(l->file[0]);
        l->file[0] = NO_FILE;
    }
    return 0;
}

/** Take the lowest free of the blocks `s` asks for, pausing while none is. */
static int block_take(struct etude_portlock *l, const struct etude_portlock_spec *s)
{
    struct etude_portlock_wait w;
    char name[32], path[PATH_MAX_BYTES];
    lockfile f;
    int slot, e;

    w.what = "ports";
    w.path = s->dir;
    w.exclusive = 1;
    w.count = s->blocks;
    w.first = 1;
    for (;;) {
        for (slot = 0; slot < s->blocks; slot++) {
            snprintf(name, sizeof(name), "rach2-ports.%d", slot);
            if ((f = open_lock(s->dir, name, path)) == NO_FILE)
                return -1;
            if (try_lock(f, 1) == 0) {
                l->file[2] = f;
                l->block = slot;
                return 0;
            }
            e = errno;
            close_file(f);
            if (e != EAGAIN) {
                errno = e;
                return -1;
            }
        }
        if (pause_for(s, &w) != 0)
            return -1;
        w.first = 0;
    }
}

int etude_portlock_take(struct etude_portlock *l, const struct etude_portlock_spec *s)
{
    int i, e;

    for (i = 0; i < 3; i++)
        l->file[i] = NO_FILE;
    l->block = -1;
#ifdef _WIN32
    unmapped = 0;
#endif
    if (s->blocks < 0 || s->blocks > ETUDE_PORTLOCK_BLOCKS) {
        errno = EINVAL;
        return -1;
    }
    if ((s->host == ETUDE_PORTLOCK_NONE || host_take(l, s) == 0) &&
        (s->blocks == 0 || block_take(l, s) == 0))
        return 0;
    e = errno;
    etude_portlock_release(l);
    errno = e;
    return -1;
}

void etude_portlock_release(struct etude_portlock *l)
{
    int i;

    for (i = 0; i < 3; i++) {
        if (l->file[i] != NO_FILE)
            close_file(l->file[i]);
        l->file[i] = NO_FILE;
    }
    l->block = -1;
}
