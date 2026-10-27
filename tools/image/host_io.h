/* Wide byte positions, UTF-8 paths and sparse image writes on each host. */
#ifndef RACH2_HOST_IMAGE_IO_H
#define RACH2_HOST_IMAGE_IO_H
#ifndef _FILE_OFFSET_BITS
#define _FILE_OFFSET_BITS 64
#endif
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#ifdef _WIN32
#include <io.h>
#include <windows.h>
#include <winioctl.h>
#endif

#ifdef _WIN32
/** Give the UTF-16 form of the UTF-8 string `text` from malloc, or NULL. */
static inline wchar_t *host_image_wide(const char *text)
{
    int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text, -1, NULL, 0);
    wchar_t *wide = n > 0 ? malloc((size_t)n * sizeof *wide) : NULL;

    if (wide != NULL &&
        MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text, -1, wide, n) != n) {
        free(wide);
        wide = NULL;
    }
    if (wide == NULL)
        errno = EINVAL;
    return wide;
}
#endif

/**
 * Open a stream on the UTF-8 path `path`, as fopen does; with `stream`, reopen
 * that stream on it, as freopen does. Windows's narrow functions read a path
 * in the legacy code page.
 */
static inline FILE *host_image_open(const char *path, const char *mode, FILE *stream)
{
#ifdef _WIN32
    wchar_t *wpath = host_image_wide(path), *wmode = host_image_wide(mode);
    FILE *file = NULL;

    if (wpath != NULL && wmode != NULL)
        file = stream != NULL ? _wfreopen(wpath, wmode, stream) : _wfopen(wpath, wmode);
    free(wpath);
    free(wmode);
    return file;
#else
    return stream != NULL ? freopen(path, mode, stream) : fopen(path, mode);
#endif
}

/** Seek without narrowing a Windows position through long or off_t. */
static inline int host_image_seek(FILE *file, int64_t offset, int origin)
{
#ifdef _WIN32
    return _fseeki64(file, offset, origin);
#else
    return fseeko(file, (off_t)offset, origin);
#endif
}

/** Return a byte position, or -1 with the stream's error. */
static inline int64_t host_image_tell(FILE *file)
{
#ifdef _WIN32
    return _ftelli64(file);
#else
    return ftello(file);
#endif
}

/** Seek to a 512-byte sector only when its byte position is representable. */
static inline int host_image_sector(FILE *file, uint64_t sector)
{
    if (sector > INT64_MAX / 512) {
        errno = EOVERFLOW;
        return -1;
    }
    return host_image_seek(file, (int64_t)(sector * 512), SEEK_SET);
}

/** Mark a regular image sparse before extending it with a far write. */
static inline int host_image_sparse(FILE *file)
{
#ifdef _WIN32
    DWORD transferred;
    HANDLE handle = (HANDLE)_get_osfhandle(_fileno(file));
    if (!DeviceIoControl(handle, FSCTL_SET_SPARSE, NULL, 0, NULL, 0, &transferred, NULL)) {
        errno = EIO;
        return -1;
    }
#else
    (void)file;
#endif
    return 0;
}

/** Extend a sparse image before writing any other far byte. */
static inline int host_image_sparse_extend(FILE *file, int64_t length)
{
#ifdef _WIN32
    if (host_image_sparse(file) != 0)
        return -1;
#endif
    if (host_image_seek(file, length - 1, SEEK_SET) != 0 || fputc(0, file) == EOF)
        return -1;
    return 0;
}

/** Parse a complete unsigned field without a sign or overflow. */
static inline int host_image_number(const char *text, uint64_t *out)
{
    char *end;
    unsigned long long value;
    if (*text < '0' || *text > '9')
        return -1;
    errno = 0;
    value = strtoull(text, &end, 10);
    if (errno || *end)
        return -1;
    *out = value;
    return 0;
}
#endif
