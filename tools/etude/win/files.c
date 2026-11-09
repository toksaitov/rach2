/* files.c: UTF-8 files, durable publication and file locks on Windows. */
#include <ctype.h>
#include <errno.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include "files.h"
#include "winfmt.h"
#include <bcrypt.h>
#include <winioctl.h>

#undef stat
#undef fstat
#undef fopen
#undef open
#undef chmod
#undef fchmod
#undef access
#undef rmdir
#undef rename
#undef unlink
#undef mkdir
#undef fcntl

struct etude_win_dir {
    HANDLE find;
    WIN32_FIND_DATAW data;
    int first;
    struct etude_win_dirent entry;
};

/** Translate the last Windows error to errno and return -1. */
int etude_win_file_error(void)
{
    switch (GetLastError()) {
    case ERROR_FILE_NOT_FOUND:
    case ERROR_PATH_NOT_FOUND:
        errno = ENOENT;
        break;
    case ERROR_ALREADY_EXISTS:
    case ERROR_FILE_EXISTS:
        errno = EEXIST;
        break;
    case ERROR_SHARING_VIOLATION:
    case ERROR_LOCK_VIOLATION:
        errno = EAGAIN;
        break;
    case ERROR_ACCESS_DENIED:
        errno = EACCES;
        break;
    case ERROR_DIR_NOT_EMPTY:
        errno = ENOTEMPTY;
        break;
    case ERROR_DISK_FULL:
        errno = ENOSPC;
        break;
    case ERROR_NOT_ENOUGH_MEMORY:
        errno = ENOMEM;
        break;
    default:
        errno = EIO;
        break;
    }
    return -1;
}

/** Fill ST from H's identity and timestamps, with reparse points distinct. */
static int handle_stat(HANDLE h, struct etude_win_stat *st, int nofollow)
{
    BY_HANDLE_FILE_INFORMATION f;
    FILE_BASIC_INFO b;
    uint64_t t;
    if (!GetFileInformationByHandle(h, &f) ||
        !GetFileInformationByHandleEx(h, FileBasicInfo, &b, sizeof b))
        return etude_win_file_error();
    memset(st, 0, sizeof *st);
    st->st_dev = f.dwVolumeSerialNumber;
    st->st_nlink = f.nNumberOfLinks;
    st->st_ino = (uint64_t)f.nFileIndexHigh << 32 | f.nFileIndexLow;
    st->st_size = (int64_t)f.nFileSizeHigh << 32 | f.nFileSizeLow;
    st->st_mode = (f.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ? S_IFDIR : S_IFREG;
    if (nofollow && (f.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT))
        st->st_mode = 0120000;
    st->st_mode |= (f.dwFileAttributes & FILE_ATTRIBUTE_READONLY) ? 0444 : 0666;
    t = (uint64_t)b.LastWriteTime.QuadPart;
    st->st_mtim.tv_sec = (time_t)(t / 10000000u - ETUDE_WIN_EPOCH_SECONDS);
    st->st_mtim.tv_nsec = (long)(t % 10000000u * 100);
    t = (uint64_t)b.ChangeTime.QuadPart;
    st->st_ctim.tv_sec = (time_t)(t / 10000000u - ETUDE_WIN_EPOCH_SECONDS);
    st->st_ctim.tv_nsec = (long)(t % 10000000u * 100);
    return 0;
}

/** Read a path, optionally opening its reparse point itself. */
static int path_stat(const char *path, struct etude_win_stat *st, int nofollow)
{
    wchar_t *w = etude_win_long_path(path);
    HANDLE h;
    int rc;
    if (w == NULL)
        return etude_win_file_error();
    h = CreateFileW(w, FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                    NULL, OPEN_EXISTING,
                    FILE_FLAG_BACKUP_SEMANTICS | (nofollow ? FILE_FLAG_OPEN_REPARSE_POINT : 0),
                    NULL);
    free(w);
    if (h == INVALID_HANDLE_VALUE)
        return etude_win_file_error();
    rc = handle_stat(h, st, nofollow);
    CloseHandle(h);
    return rc;
}

/** Read PATH's status, following a reparse point. */
int etude_win_stat(const char *path, struct etude_win_stat *st)
{
    return path_stat(path, st, 0);
}
/** Read PATH's status without traversing a reparse point. */
int etude_win_lstat(const char *path, struct etude_win_stat *st)
{
    return path_stat(path, st, 1);
}
/** Read FD's status from its native handle. */
int etude_win_fstat(int fd, struct etude_win_stat *st)
{
    return handle_stat((HANDLE)_get_osfhandle(fd), st, 0);
}

/** Open a UTF-8 path, always binary and noninheritable. */
int etude_win_open(const char *path, int flags, ...)
{
    wchar_t *w = etude_win_long_path(path);
    DWORD access = (flags & O_RDWR)     ? GENERIC_READ | GENERIC_WRITE
                   : (flags & O_WRONLY) ? GENERIC_WRITE
                                        : GENERIC_READ;
    DWORD create = (flags & O_CREAT) ? ((flags & O_EXCL)    ? CREATE_NEW
                                        : (flags & O_TRUNC) ? CREATE_ALWAYS
                                                            : OPEN_ALWAYS)
                                     : ((flags & O_TRUNC) ? TRUNCATE_EXISTING : OPEN_EXISTING);
    HANDLE h;
    int fd;
    DWORD ignored;
    if (w == NULL)
        return etude_win_file_error();
    h = CreateFileW(
        w, access, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, create,
        FILE_ATTRIBUTE_NORMAL | ((flags & O_NOFOLLOW) ? FILE_FLAG_OPEN_REPARSE_POINT : 0), NULL);
    free(w);
    if (h == INVALID_HANDLE_VALUE)
        return etude_win_file_error();
    if (flags & O_NOFOLLOW) {
        FILE_ATTRIBUTE_TAG_INFO attributes;
        if (!GetFileInformationByHandleEx(h, FileAttributeTagInfo, &attributes,
                                          sizeof attributes) ||
            (attributes.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
            CloseHandle(h);
            errno = ELOOP;
            return -1;
        }
    }
    if ((flags & O_CREAT) && create == CREATE_NEW)
        DeviceIoControl(h, FSCTL_SET_SPARSE, NULL, 0, NULL, 0, &ignored, NULL);
    fd = _open_osfhandle((intptr_t)h, (flags & ~O_NOFOLLOW) | _O_BINARY | _O_NOINHERIT);
    if (fd < 0)
        CloseHandle(h);
    return fd;
}

FILE *etude_win_fopen(const char *path, const char *mode)
{
    wchar_t *w = etude_win_long_path(path), *m = etude_win_wide(mode);
    FILE *f = w != NULL && m != NULL ? _wfopen(w, m) : NULL;
    free(w);
    free(m);
    return f;
}

char *etude_win_realpath(const char *path, char *out)
{
    wchar_t *w = etude_win_long_path(path);
    char *u, *p;
    struct etude_win_stat st;
    if (w == NULL || etude_win_stat(path, &st) != 0) {
        free(w);
        return NULL;
    }
    if (wcsncmp(w, L"\\\\?\\UNC\\", 8) == 0) {
        char *tail = etude_win_utf8(w + 8);
        u = tail != NULL ? malloc(strlen(tail) + 3) : NULL;
        if (u != NULL)
            sprintf(u, "//%s", tail);
        free(tail);
    } else {
        u = etude_win_utf8(w + 4);
    }
    free(w);
    if (u == NULL)
        return NULL;
    for (p = u; *p; p++)
        if (*p == '\\')
            *p = '/';
    if (out == NULL)
        return u;
    if (strlen(u) >= 4096) {
        free(u);
        errno = ENAMETOOLONG;
        return NULL;
    }
    strcpy(out, u);
    free(u);
    return out;
}

/** Open a stream on a UTF-8 path. */
FILE *etude_win_fopen(const char *path, const char *mode);
/** Return an absolute UTF-8 path in OUT, or allocated when OUT is NULL. */
char *etude_win_realpath(const char *path, char *out);
/** Make a directory; MODE is the portable caller's permission mask. */
int etude_win_mkdir(const char *path, int mode)
{
    wchar_t *w = etude_win_long_path(path);
    int rc;
    (void)mode;
    rc = w != NULL && CreateDirectoryW(w, NULL) ? 0 : etude_win_file_error();
    free(w);
    return rc;
}

/** Remove one file, including a read-only artifact. */
int etude_win_unlink(const char *path)
{
    wchar_t *w = etude_win_long_path(path);
    HANDLE h;
    FILE_DISPOSITION_INFO_EX disposition;
    int rc;
    if (w == NULL)
        return etude_win_file_error();
    h = CreateFileW(w, DELETE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                    OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, NULL);
    free(w);
    if (h == INVALID_HANDLE_VALUE)
        return etude_win_file_error();
    disposition.Flags = FILE_DISPOSITION_FLAG_DELETE | FILE_DISPOSITION_FLAG_POSIX_SEMANTICS |
                        FILE_DISPOSITION_FLAG_IGNORE_READONLY_ATTRIBUTE;
    rc = SetFileInformationByHandle(h, FileDispositionInfoEx, &disposition, sizeof disposition)
             ? 0
             : etude_win_file_error();
    CloseHandle(h);
    return rc;
}

/** Remove an empty directory. */
int etude_win_rmdir(const char *path)
{
    wchar_t *w = etude_win_long_path(path);
    int rc = w != NULL && RemoveDirectoryW(w) ? 0 : etude_win_file_error();
    free(w);
    return rc;
}

/** Replace TO atomically with FROM, on the same volume. */
int etude_win_rename(const char *from, const char *to)
{
    wchar_t *a = etude_win_long_path(from), *b = etude_win_long_path(to);
    DWORD attrs = b != NULL ? GetFileAttributesW(b) : INVALID_FILE_ATTRIBUTES;
    int rc;
    if (attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_READONLY))
        SetFileAttributesW(b, attrs & ~FILE_ATTRIBUTE_READONLY);
    rc = a != NULL && b != NULL &&
                 MoveFileExW(a, b, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)
             ? 0
             : etude_win_file_error();
    if (rc != 0 && b != NULL && attrs != INVALID_FILE_ATTRIBUTES)
        SetFileAttributesW(b, attrs);
    free(a);
    free(b);
    return rc;
}

/** Publish a hard link without replacing an existing file. */
int etude_win_link(const char *from, const char *to)
{
    wchar_t *a = etude_win_long_path(from), *b = etude_win_long_path(to);
    int rc = a != NULL && b != NULL && CreateHardLinkW(b, a, NULL) ? 0 : etude_win_file_error();
    free(a);
    free(b);
    return rc;
}

/** Change FD's read-only attribute by MODE's write bits. */
int etude_win_fchmod(int fd, int mode)
{
    FILE_BASIC_INFO b;
    HANDLE h = (HANDLE)_get_osfhandle(fd);
    if (!GetFileInformationByHandleEx(h, FileBasicInfo, &b, sizeof b))
        return etude_win_file_error();
    if (mode & 0222)
        b.FileAttributes &= ~FILE_ATTRIBUTE_READONLY;
    else
        b.FileAttributes |= FILE_ATTRIBUTE_READONLY;
    return SetFileInformationByHandle(h, FileBasicInfo, &b, sizeof b) ? 0 : etude_win_file_error();
}

/** Change the file's read-only attribute by MODE's write bits. */
int etude_win_chmod(const char *path, int mode)
{
    wchar_t *w = etude_win_long_path(path);
    DWORD a = w != NULL ? GetFileAttributesW(w) : INVALID_FILE_ATTRIBUTES;
    int rc;
    if (a == INVALID_FILE_ATTRIBUTES) {
        free(w);
        return etude_win_file_error();
    }
    a = mode & 0222 ? a & ~FILE_ATTRIBUTE_READONLY : a | FILE_ATTRIBUTE_READONLY;
    rc = SetFileAttributesW(w, a) ? 0 : etude_win_file_error();
    free(w);
    return rc;
}

/** Check whether PATH exists and, with write access, is writable. */
int etude_win_access(const char *path, int mode)
{
    struct etude_win_stat st;
    if (etude_win_stat(path, &st) != 0)
        return -1;
    if ((mode & 2) && !(st.st_mode & 0222)) {
        errno = EACCES;
        return -1;
    }
    return 0;
}

/** Reserve a unique path with random bytes from the system. */
static int temporary(char *path, int dir)
{
    size_t n = strlen(path), i;
    unsigned tries;
    unsigned char bytes[6];
    int fd;
    if (n < 6 || strcmp(path + n - 6, "XXXXXX") != 0) {
        errno = EINVAL;
        return -1;
    }
    for (tries = 0; tries < 128; tries++) {
        if (BCryptGenRandom(NULL, bytes, sizeof bytes, BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0) {
            errno = EIO;
            return -1;
        }
        for (i = 0; i < 6; i++)
            path[n - 6 + i] = "abcdefghijklmnopqrstuvwxyz0123456789"[bytes[i] % 36];
        fd = dir ? etude_win_mkdir(path, 0700)
                 : etude_win_open(path, O_RDWR | O_CREAT | O_EXCL, 0600);
        if (fd >= 0 || errno != EEXIST)
            return fd;
    }
    errno = EEXIST;
    return -1;
}
/** Reserve TEMPLATE, whose final six bytes are XXXXXX, as a private file. */
int etude_win_mkstemp(char *path)
{
    return temporary(path, 0);
}
char *etude_win_mkdtemp(char *path)
{
    return temporary(path, 1) == 0 ? path : NULL;
}

/**
 * Lock FD's whole file; TYPE is read, write or unlock, WAIT permits waiting.
 */
int etude_win_file_lock(int fd, int type, int wait)
{
    OVERLAPPED ov;
    DWORD flags =
        (wait ? 0 : LOCKFILE_FAIL_IMMEDIATELY) | (type == F_WRLCK ? LOCKFILE_EXCLUSIVE_LOCK : 0);
    HANDLE h = (HANDLE)_get_osfhandle(fd);
    memset(&ov, 0, sizeof ov);
    return (type == F_UNLCK ? UnlockFileEx(h, 0, MAXDWORD, MAXDWORD, &ov)
                            : LockFileEx(h, flags, 0, MAXDWORD, MAXDWORD, &ov))
               ? 0
               : etude_win_file_error();
}
/** Make FD noninheritable; other commands are rejected. */
int etude_win_fcntl(int fd, int command, ...)
{
    if (command != F_SETFD) {
        errno = EINVAL;
        return -1;
    }
    return SetHandleInformation((HANDLE)_get_osfhandle(fd), HANDLE_FLAG_INHERIT, 0)
               ? 0
               : etude_win_file_error();
}

struct etude_win_dir *etude_win_opendir(const char *path)
{
    struct etude_win_dir *d = calloc(1, sizeof *d);
    char *pattern;
    wchar_t *w;
    if (d == NULL)
        return NULL;
    pattern = malloc(strlen(path) + 3);
    if (pattern == NULL) {
        free(d);
        return NULL;
    }
    sprintf(pattern, "%s/*", path);
    w = etude_win_long_path(pattern);
    free(pattern);
    d->find = w == NULL ? INVALID_HANDLE_VALUE : FindFirstFileW(w, &d->data);
    free(w);
    if (d->find == INVALID_HANDLE_VALUE) {
        etude_win_file_error();
        free(d);
        return NULL;
    }
    d->first = 1;
    return d;
}
struct etude_win_dirent *etude_win_readdir(struct etude_win_dir *d)
{
    char *u;
    if (!d->first && !FindNextFileW(d->find, &d->data)) {
        if (GetLastError() != ERROR_NO_MORE_FILES)
            etude_win_file_error();
        else
            errno = 0;
        return NULL;
    }
    d->first = 0;
    u = etude_win_utf8(d->data.cFileName);
    if (u == NULL)
        return NULL;
    strcpy(d->entry.d_name, u);
    free(u);
    return &d->entry;
}
/** Open a directory without traversing its entries. */
struct etude_win_dir *etude_win_opendir(const char *path);
/** Read the next UTF-8 entry, or NULL at the end or an error. */
struct etude_win_dirent *etude_win_readdir(struct etude_win_dir *dir);
/** Close a directory. */
int etude_win_closedir(struct etude_win_dir *d)
{
    FindClose(d->find);
    free(d);
    return 0;
}

/** Return the native temporary directory as UTF-8. */
const char *etude_win_temp(void);
/** Locate a program on native PATH, including its .exe suffix. */
/**
 * Give in `out` the native path of `name`, an absolute path of MSYS2's, such
 * as `/usr/bin/env` or `/c/Windows`, under the root of the MSYS2 whose sh is
 * on native PATH. A command that MSYS2 starts with MSYS2_ARG_CONV_EXCL set
 * passes such paths on unconverted, as the installed launcher's `command -v
 * env` reaches `etude admit`.
 *
 * @return 0, or -1 for another name, no MSYS2 or a path too long.
 */
static int msys_native(const char *name, char *out, size_t size)
{
    const char *shell, *tail = "/usr/bin/sh.exe";
    size_t root, k = strlen(tail);
    int n;

    if (name[0] != '/' || name[1] == '/')
        return -1;
    if (isalpha((unsigned char)name[1]) && (name[2] == '/' || name[2] == 0)) {
        n = snprintf(out, size, "%c:%s", name[1], name[2] ? name + 2 : "/");
        return n < 0 || (size_t)n >= size ? -1 : 0;
    }
    shell = etude_win_shell();
    if (shell == NULL || (root = strlen(shell)) <= k || _stricmp(shell + root - k, tail) != 0)
        return -1;
    root -= k;
    if (strncmp(name, "/bin/", 5) == 0)
        n = snprintf(out, size, "%.*s/usr%s", (int)root, shell, name);
    else
        n = snprintf(out, size, "%.*s%s", (int)root, shell, name);
    return n < 0 || (size_t)n >= size ? -1 : 0;
}

int etude_win_program(const char *name, char *out, size_t size)
{
    wchar_t *w = etude_win_wide(name), path[32768];
    char *u, native[4096];
    DWORD n = w != NULL ? SearchPathW(NULL, w, L".exe", 32768, path, NULL) : 0;
    free(w);
    if ((n == 0 || n >= 32768) && msys_native(name, native, sizeof native) == 0 &&
        (w = etude_win_wide(native)) != NULL) {
        n = SearchPathW(NULL, w, L".exe", 32768, path, NULL);
        free(w);
    }
    if (n == 0 || n >= 32768) {
        errno = ENOENT;
        return -1;
    }
    u = etude_win_utf8(path);
    if (u == NULL)
        return -1;
    if (strlen(u) >= size) {
        free(u);
        errno = ENAMETOOLONG;
        return -1;
    }
    strcpy(out, u);
    free(u);
    for (char *p = out; *p; p++)
        if (*p == '\\')
            *p = '/';
    return 0;
}
const char *etude_win_shell(void)
{
    static char path[4096];
    const char *given = getenv("ETUDE_SHELL");
    if (given != NULL && *given)
        return given;
    if (etude_win_program("sh", path, sizeof path) != 0)
        return NULL;
    return path;
}

/** Return a POSIX shell supplied by MSYS2, or NULL. */
const char *etude_win_shell(void);

/** Read one line, growing its caller-owned buffer. */
ssize_t etude_win_getline(char **line, size_t *capacity, FILE *file)
{
    size_t n = 0;
    int ch;
    char *next;
    while ((ch = fgetc(file)) != EOF) {
        if (n + 2 > *capacity) {
            size_t want = *capacity < 128 ? 128 : *capacity * 2;
            if (want < *capacity || (next = realloc(*line, want)) == NULL)
                return -1;
            *line = next;
            *capacity = want;
        }
        (*line)[n++] = (char)ch;
        if (ch == '\n')
            break;
    }
    if (!n)
        return -1;
    (*line)[n] = '\0';
    return (ssize_t)n;
}
struct tm *etude_win_gmtime(const time_t *time, struct tm *out)
{
    struct tm *value = gmtime(time);
    if (value == NULL)
        return NULL;
    *out = *value;
    return out;
}

/** Say whether PATH is absolute, including a drive or a UNC server. */
int etude_win_absolute(const char *path)
{
    return path != NULL &&
           ((path[0] == '/' && path[1] == '/') || (path[0] == '\\' && path[1] == '\\') ||
            (path[0] != '\0' && path[1] == ':' && (path[2] == '/' || path[2] == '\\')));
}
const char *etude_win_temp(void)
{
    static char out[4096];
    wchar_t path[4096];
    char *u;
    const char *given = getenv("TMPDIR");
    if (etude_win_absolute(given))
        return given;
    if (!GetTempPathW(4096, path))
        return NULL;
    u = etude_win_utf8(path);
    if (u == NULL)
        return NULL;
    if (strlen(u) >= sizeof out) {
        free(u);
        errno = ENAMETOOLONG;
        return NULL;
    }
    strcpy(out, u);
    free(u);
    return out;
}

/** Reserve TEMPLATE as a private directory. */
char *etude_win_mkdtemp(char *path);
/** Set FD's length without changing its current offset. */
int etude_win_truncate(int fd, int64_t size)
{
    HANDLE h = (HANDLE)_get_osfhandle(fd);
    LARGE_INTEGER zero, old, end;
    int rc;
    if (size < 0) {
        errno = EINVAL;
        return -1;
    }
    zero.QuadPart = 0;
    end.QuadPart = size;
    if (!SetFilePointerEx(h, zero, &old, FILE_CURRENT))
        return etude_win_file_error();
    rc = SetFilePointerEx(h, end, NULL, FILE_BEGIN) && SetEndOfFile(h) ? 0 : etude_win_file_error();
    if (!SetFilePointerEx(h, old, NULL, FILE_BEGIN))
        return etude_win_file_error();
    return rc;
}

/** Record the kernel version without the application manifest's emulation. */
void etude_win_version(char *out, size_t size)
{
    typedef LONG(WINAPI * version_fn)(OSVERSIONINFOW *);
    version_fn fn =
        (version_fn)(void *)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion");
    OSVERSIONINFOW version;
    memset(&version, 0, sizeof version);
    version.dwOSVersionInfoSize = sizeof version;
    if (fn == NULL || fn(&version) < 0)
        snprintf(out, size, "unknown");
    else
        snprintf(out, size, "%lu.%lu.%lu", (unsigned long)version.dwMajorVersion,
                 (unsigned long)version.dwMinorVersion, (unsigned long)version.dwBuildNumber);
}

/** Read the boot environment and the loader's boot sequence. */
int etude_win_boot_id(char *out, size_t size)
{
    typedef LONG(NTAPI * query_fn)(ULONG, void *, ULONG, ULONG *);
    uint64_t boot[4];
    ULONG sequence;
    SIZE_T got;
    GUID id;
    query_fn fn = (query_fn)(void *)GetProcAddress(GetModuleHandleW(L"ntdll.dll"),
                                                   "NtQuerySystemInformation");
    int n;
    /* KUSER_SHARED_DATA.BootId is at 0x2c4 in the shared user page.
     * The loader increments it at boot; clock corrections cannot change it.
     * SystemTimeOfDayInformation.BootTime changes with the wall clock. */
    if (fn == NULL || fn(90, &boot, sizeof boot, NULL) < 0 ||
        !ReadProcessMemory(GetCurrentProcess(), (const void *)(uintptr_t)0x7ffe02c4, &sequence,
                           sizeof sequence, &got) ||
        got != sizeof sequence) {
        errno = EIO;
        return -1;
    }
    memcpy(&id, boot, sizeof id);
    n = snprintf(out, size, "%08lx-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x.boot-%08lx",
                 (unsigned long)id.Data1, id.Data2, id.Data3, id.Data4[0], id.Data4[1], id.Data4[2],
                 id.Data4[3], id.Data4[4], id.Data4[5], id.Data4[6], id.Data4[7],
                 (unsigned long)sequence);
    if (n < 0 || (size_t)n >= size) {
        errno = ENAMETOOLONG;
        return -1;
    }
    return 0;
}

/** Perform positioned I/O on an independent handle. ReOpenFile keeps the
 * caller's file position unchanged, including while another thread uses
 * the original handle. An overlapped request also accepts offsets beyond
 * the CRT's 32-bit off_t. */
static ssize_t positioned_io(int fd, void *buf, size_t size, int64_t offset, int writing)
{
    HANDLE h, source = (HANDLE)_get_osfhandle(fd);
    OVERLAPPED at;
    DWORD n = 0, error = 0;
    BOOL ok;

    if (offset < 0) {
        errno = EINVAL;
        return -1;
    }
    if (source == INVALID_HANDLE_VALUE) {
        errno = EBADF;
        return -1;
    }
    h = ReOpenFile(source, writing ? GENERIC_WRITE : GENERIC_READ,
                   FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, FILE_FLAG_OVERLAPPED);
    if (h == INVALID_HANDLE_VALUE)
        return etude_win_file_error();
    memset(&at, 0, sizeof(at));
    at.Offset = (DWORD)(uint64_t)offset;
    at.OffsetHigh = (DWORD)((uint64_t)offset >> 32);
    at.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (at.hEvent == NULL) {
        error = GetLastError();
        CloseHandle(h);
        SetLastError(error);
        return etude_win_file_error();
    }
    if (size > MAXDWORD)
        size = MAXDWORD;
    ok = writing ? WriteFile(h, buf, (DWORD)size, &n, &at) : ReadFile(h, buf, (DWORD)size, &n, &at);
    if (!ok && GetLastError() == ERROR_IO_PENDING)
        ok = GetOverlappedResult(h, &at, &n, TRUE);
    if (!ok)
        error = GetLastError();
    CloseHandle(at.hEvent);
    CloseHandle(h);
    if (!writing && error == ERROR_HANDLE_EOF)
        return 0;
    if (!ok) {
        SetLastError(error);
        return etude_win_file_error();
    }
    return (ssize_t)n;
}

ssize_t etude_win_pread(int fd, void *buf, size_t size, int64_t offset)
{
    return positioned_io(fd, buf, size, offset, 0);
}

ssize_t etude_win_pwrite(int fd, const void *buf, size_t size, int64_t offset)
{
    return positioned_io(fd, (void *)buf, size, offset, 1);
}

char *etude_win_strndup(const char *text, size_t size)
{
    size_t n = 0;
    char *copy;

    while (n < size && text[n] != '\0')
        n++;
    copy = malloc(n + 1);
    if (copy == NULL) {
        errno = ENOMEM;
        return NULL;
    }
    memcpy(copy, text, n);
    copy[n] = '\0';
    return copy;
}

int etude_win_setenv(const char *name, const char *value, int overwrite)
{
    int error;

    if (name == NULL || name[0] == '\0' || strchr(name, '=') != NULL || value == NULL) {
        errno = EINVAL;
        return -1;
    }
    if (!overwrite && getenv(name) != NULL)
        return 0;
    error = _putenv_s(name, value);
    if (error == 0)
        return 0;
    errno = error;
    return -1;
}

int etude_win_unsetenv(const char *name)
{
    return etude_win_setenv(name, "", 1);
}
