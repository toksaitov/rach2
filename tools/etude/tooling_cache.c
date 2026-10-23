/* tooling_cache.c: remove completed machines with no job or evidence. */
#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#if defined(__APPLE__) && !defined(_DARWIN_C_SOURCE)
#define _DARWIN_C_SOURCE
#endif
#endif
#include "tooling_cache.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#endif

#define CACHE_PATH 4096
#define CACHE_KEYS 128
struct catalog {
    char machines[CACHE_KEYS][65], pinned[CACHE_KEYS][65];
    size_t nmachines, npinned;
    int uncertain;
};

/** Give a cache path, refusing truncation. */
static int path_of(char path[CACHE_PATH], const char *cache, const char *name)
{
    int n = snprintf(path, CACHE_PATH, "%s/%s", cache, name);
    return n >= 0 && n < CACHE_PATH;
}

/** Wait for a short cache operation, unless cancellation was requested. */
static int pause_lock(const struct etude_tooling_site *site)
{
    if (site->stop && *site->stop)
        return 0;
#ifdef _WIN32
    Sleep(50);
#else
    struct timespec pause = {0, 50000000};
    nanosleep(&pause, NULL);
#endif
    return 1;
}

/** The kernel releases this noninherited mutex if its owner dies. */
int tooling_cache_enter(const struct etude_tooling_site *site, intptr_t *guard)
{
    char path[CACHE_PATH];
    if ((site->stop && *site->stop) || !path_of(path, site->cache, ".tooling-cache.lock"))
        return -1;
#ifdef _WIN32
    HANDLE file =
        CreateFileA(path, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                    OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, NULL);
    BY_HANDLE_FILE_INFORMATION info;
    OVERLAPPED overlap = {0};
    if (file == INVALID_HANDLE_VALUE)
        return -1;
    if (!GetFileInformationByHandle(file, &info) || info.nNumberOfLinks != 1 ||
        (info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT))) {
        CloseHandle(file);
        return -1;
    }
    while (
        !LockFileEx(file, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0, 1, 0, &overlap)) {
        if (GetLastError() != ERROR_LOCK_VIOLATION || !pause_lock(site)) {
            CloseHandle(file);
            return -1;
        }
    }
    *guard = (intptr_t)file;
#else
    int fd = open(path, O_CREAT | O_RDWR | O_CLOEXEC | O_NOFOLLOW, 0600);
    struct stat st;
    struct flock lock;
    if (fd < 0)
        return -1;
    memset(&lock, 0, sizeof(lock));
    lock.l_type = F_WRLCK;
    lock.l_whence = SEEK_SET;
    if (fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_nlink != 1) {
        close(fd);
        return -1;
    }
    while (fcntl(fd, F_SETLK, &lock) != 0) {
        if ((errno != EAGAIN && errno != EACCES && errno != EINTR) || !pause_lock(site)) {
            close(fd);
            return -1;
        }
    }
    *guard = fd;
#endif
    return 0;
}

void tooling_cache_leave(intptr_t guard)
{
#ifdef _WIN32
    OVERLAPPED overlap = {0};
    UnlockFileEx((HANDLE)guard, 0, 1, 0, &overlap);
    CloseHandle((HANDLE)guard);
#else
    close((int)guard);
#endif
}

/** Take a digest-shaped key, followed by the exact suffix. */
static int key_of(const char *text, const char *suffix, char key[65])
{
    if (strlen(text) != 64 + strlen(suffix) || strspn(text, "0123456789abcdef") < 64 ||
        strcmp(text + 64, suffix))
        return 0;
    memcpy(key, text, 64);
    key[64] = 0;
    return 1;
}

/** Job directories retain their base even after failure or controller death. */
static void entry(struct catalog *c, const char *name, int directory, int regular)
{
    char key[65];
    if (!strncmp(name, "prepare-", 8)) {
        c->uncertain = 1;
    } else if (!strncmp(name, "job-", 4)) {
        const char *text = name + 4;
        if (!directory || strlen(text) < 66 || text[64] != '-' ||
            strspn(text, "0123456789abcdef") < 64 || c->npinned == CACHE_KEYS) {
            c->uncertain = 1;
            return;
        }
        memcpy(c->pinned[c->npinned], text, 64);
        c->pinned[c->npinned++][64] = 0;
    } else if (regular && !strncmp(name, "tooling-", 8) && key_of(name + 8, ".qcow2", key)) {
        if (c->nmachines == CACHE_KEYS) {
            c->uncertain = 1;
            return;
        }
        memcpy(c->machines[c->nmachines++], key, 65);
    }
}

/** Scan only the cache itself; never traverse a job or evidence directory. */
static int scan(const char *cache, struct catalog *c)
{
    char path[CACHE_PATH];
#ifdef _WIN32
    WIN32_FIND_DATAA data;
    HANDLE find;
    int ok;
    if (!path_of(path, cache, "*"))
        return 0;
    find = FindFirstFileA(path, &data);
    if (find == INVALID_HANDLE_VALUE)
        return 0;
    do {
        DWORD a = data.dwFileAttributes;
        entry(c, data.cFileName,
              (a & FILE_ATTRIBUTE_DIRECTORY) && !(a & FILE_ATTRIBUTE_REPARSE_POINT),
              !(a & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)));
    } while (FindNextFileA(find, &data));
    ok = GetLastError() == ERROR_NO_MORE_FILES;
    FindClose(find);
    return ok;
#else
    DIR *dir = opendir(cache);
    struct dirent *de;
    struct stat st;
    int ok = 1;
    if (!dir)
        return 0;
    errno = 0;
    while ((de = readdir(dir))) {
        if (!path_of(path, cache, de->d_name) || lstat(path, &st)) {
            ok = 0;
            break;
        }
        entry(c, de->d_name, S_ISDIR(st.st_mode), S_ISREG(st.st_mode));
        errno = 0;
    }
    ok &= errno == 0;
    closedir(dir);
    return ok;
#endif
}

/** A reproducible cache file must have no symlink or hard-link alias. */
static int regular(const char *path)
{
#ifdef _WIN32
    HANDLE f = CreateFileA(path, FILE_READ_ATTRIBUTES,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                           OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, NULL);
    BY_HANDLE_FILE_INFORMATION info;
    int ok;
    if (f == INVALID_HANDLE_VALUE)
        return 0;
    ok = GetFileInformationByHandle(f, &info) && info.nNumberOfLinks == 1 &&
         !(info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT));
    CloseHandle(f);
    return ok;
#else
    struct stat st;
    return lstat(path, &st) == 0 && S_ISREG(st.st_mode) && st.st_nlink == 1;
#endif
}

/** A complete pair has a successful provisioning witness. */
static int completed(const char *disk, const char *tools)
{
    char line[1024];
    FILE *f;
    int witnessed = 0;
    if (!regular(disk) || !regular(tools) || !(f = fopen(tools, "rb")))
        return 0;
    while (fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\r\n")] = 0;
        if (!strcmp(line, "etude-tooling: provision: ok"))
            witnessed = 1;
    }
    witnessed &= !ferror(f);
    fclose(f);
    return witnessed;
}

/** Windows requires clearing read-only before deleting an obsolete output. */
static int discard(const char *path)
{
#ifdef _WIN32
    DWORD attrs = GetFileAttributesA(path);
    DWORD writable = attrs & ~FILE_ATTRIBUTE_READONLY;
    if (attrs == INVALID_FILE_ATTRIBUTES ||
        !SetFileAttributesA(path, writable ? writable : FILE_ATTRIBUTE_NORMAL))
        return -1;
    if (DeleteFileA(path))
        return 0;
    SetFileAttributesA(path, attrs);
    return -1;
#else
    return remove(path);
#endif
}

static int pinned(const struct catalog *c, const char *key)
{
    for (size_t n = 0; n < c->npinned; n++)
        if (!strcmp(c->pinned[n], key))
            return 1;
    return 0;
}

/** Keep downloads, incomplete files, active jobs and retained evidence. */
void tooling_cache_prune(const struct etude_tooling_site *site, const char *current)
{
    struct catalog *c = calloc(1, sizeof(*c));
    intptr_t guard;
    char disk[CACHE_PATH], tools[CACHE_PATH], name[96];
    if (!c)
        return;
    if (tooling_cache_enter(site, &guard)) {
        free(c);
        return;
    }
    snprintf(name, sizeof(name), "tooling-%s.qcow2", current);
    int paths = path_of(disk, site->cache, name);
    snprintf(name, sizeof(name), "tooling-%s.tools", current);
    paths &= path_of(tools, site->cache, name);
    if (paths && completed(disk, tools) && scan(site->cache, c) && !c->uncertain)
        for (size_t n = 0; n < c->nmachines; n++) {
            const char *key = c->machines[n];
            if (!strcmp(key, current) || pinned(c, key))
                continue;
            snprintf(name, sizeof(name), "tooling-%s.qcow2", key);
            if (!path_of(disk, site->cache, name))
                break;
            snprintf(name, sizeof(name), "tooling-%s.tools", key);
            if (!path_of(tools, site->cache, name))
                break;
            if (completed(disk, tools)) {
                if (discard(disk) || discard(tools))
                    fprintf(stderr, "etude tooling: cannot remove obsolete machine %s\n", key);
                else
                    fprintf(stderr, "etude tooling: removed obsolete machine %s\n", key);
            }
        }
    tooling_cache_leave(guard);
    free(c);
}
