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
#include <winsock2.h>
#include <windows.h>

#include "win/files.h"
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

/** Prefer only an administrator-provisioned shared directory. */
const char *tooling_cache_default(void)
{
#ifndef _WIN32
    const char *path = "/var/cache/rach2-tooling";
    struct stat st;
    if (!lstat(path, &st) && S_ISDIR(st.st_mode) && st.st_uid == 0 &&
        (st.st_mode & 07777) == 02770 && !access(path, R_OK | W_OK | X_OK))
        return path;
#endif
    return "build/tooling";
}

#ifndef _WIN32
/** Publish a locked, group-accessible inode after its mode is complete. */
static int new_mutex(const char *path)
{
    char temporary[CACHE_PATH];
    struct flock lock;
    int fd, saved;
    if (snprintf(temporary, sizeof(temporary), "%s.XXXXXX", path) >= (int)sizeof(temporary))
        return -1;
    fd = mkstemp(temporary);
    if (fd < 0)
        return -1;
    memset(&lock, 0, sizeof(lock));
    lock.l_type = F_WRLCK;
    lock.l_whence = SEEK_SET;
    if (fcntl(fd, F_SETFD, FD_CLOEXEC) || fchmod(fd, 0660) || fcntl(fd, F_SETLK, &lock) ||
        link(temporary, path)) {
        saved = errno;
        unlink(temporary);
        close(fd);
        errno = saved;
        return -1;
    }
    if (unlink(temporary)) {
        close(fd);
        return -1;
    }
    return fd;
}
#endif

/** The kernel releases this noninherited mutex if its owner dies. */
static int cache_enter(const struct etude_tooling_site *site, const char *name, int wait,
                       intptr_t *guard)
{
    char path[CACHE_PATH];
    if ((site->stop && *site->stop) || !path_of(path, site->cache, name))
        return -1;
#ifdef _WIN32
    wchar_t *w = etude_win_long_path(path);
    if (w == NULL)
        return -1;
    HANDLE file =
        CreateFileW(w, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                    OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, NULL);
    BY_HANDLE_FILE_INFORMATION info;
    OVERLAPPED overlap = {0};
    free(w);
    if (file == INVALID_HANDLE_VALUE)
        return -1;
    if (!GetFileInformationByHandle(file, &info) || info.nNumberOfLinks != 1 ||
        (info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT))) {
        CloseHandle(file);
        return -1;
    }
    while (
        !LockFileEx(file, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0, 1, 0, &overlap)) {
        DWORD error = GetLastError();
        if (!wait || error != ERROR_LOCK_VIOLATION || !pause_lock(site)) {
            CloseHandle(file);
            return !wait && error == ERROR_LOCK_VIOLATION ? 1 : -1;
        }
    }
    *guard = (intptr_t)file;
#else
    int fd;
    struct stat st;
    struct flock lock;
    for (;;) {
        fd = open(path, O_RDWR | O_CLOEXEC | O_NOFOLLOW);
        if (fd >= 0 || errno != ENOENT)
            break;
        fd = new_mutex(path);
        if (fd >= 0 || errno != EEXIST)
            break;
    }
    if (fd < 0)
        return -1;
    memset(&lock, 0, sizeof(lock));
    lock.l_type = F_WRLCK;
    lock.l_whence = SEEK_SET;
    while (fcntl(fd, F_SETLK, &lock) != 0) {
        int busy = errno == EAGAIN || errno == EACCES;
        if (!wait || (!busy && errno != EINTR) || !pause_lock(site)) {
            close(fd);
            return !wait && busy ? 1 : -1;
        }
    }
    if (fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_nlink != 1 ||
        (st.st_uid == geteuid() && fchmod(fd, 0660))) {
        close(fd);
        return -1;
    }
    *guard = fd;
#endif
    return 0;
}

int tooling_cache_enter(const struct etude_tooling_site *site, intptr_t *guard)
{
    return cache_enter(site, ".tooling-cache.lock", 1, guard);
}

static int prepare_guard(const struct etude_tooling_site *site, const char *key, int wait,
                         intptr_t *guard)
{
    char name[96];
    if (strlen(key) != 64 || strspn(key, "0123456789abcdef") != 64)
        return -1;
    snprintf(name, sizeof(name), ".tooling-%s.lock", key);
    return cache_enter(site, name, wait, guard);
}

int tooling_cache_prepare_enter(const struct etude_tooling_site *site, const char *key,
                                intptr_t *guard)
{
    return prepare_guard(site, key, 1, guard);
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
    WIN32_FIND_DATAW data;
    HANDLE find;
    wchar_t *w;
    char *name;
    int ok = 1;
    if (!path_of(path, cache, "*") || (w = etude_win_long_path(path)) == NULL)
        return 0;
    find = FindFirstFileW(w, &data);
    free(w);
    if (find == INVALID_HANDLE_VALUE)
        return 0;
    do {
        DWORD a = data.dwFileAttributes;
        if ((name = etude_win_utf8(data.cFileName)) == NULL) {
            ok = 0;
            break;
        }
        entry(c, name, (a & FILE_ATTRIBUTE_DIRECTORY) && !(a & FILE_ATTRIBUTE_REPARSE_POINT),
              !(a & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)));
        free(name);
    } while (FindNextFileW(find, &data));
    ok = ok && GetLastError() == ERROR_NO_MORE_FILES;
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
    wchar_t *w = etude_win_long_path(path);
    if (w == NULL)
        return 0;
    HANDLE f =
        CreateFileW(w, FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                    NULL, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, NULL);
    BY_HANDLE_FILE_INFORMATION info;
    int ok;
    free(w);
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

/** Remove an obsolete output, which is read-only. */
static int discard(const char *path)
{
#ifdef _WIN32
    return unlink(path);
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

static int kept(const char *const *keep, const char *key)
{
    for (; *keep != NULL; keep++)
        if (!strcmp(*keep, key))
            return 1;
    return 0;
}

/** Keep downloads, incomplete files, active jobs and retained evidence. */
void tooling_cache_prune(const struct etude_tooling_site *site, const char *const *keep)
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
    snprintf(name, sizeof(name), "tooling-%s.qcow2", keep[0]);
    int paths = path_of(disk, site->cache, name);
    snprintf(name, sizeof(name), "tooling-%s.tools", keep[0]);
    paths &= path_of(tools, site->cache, name);
    if (paths && completed(disk, tools) && scan(site->cache, c) && !c->uncertain)
        for (size_t n = 0; n < c->nmachines; n++) {
            const char *key = c->machines[n];
            if (kept(keep, key) || pinned(c, key))
                continue;
            snprintf(name, sizeof(name), "tooling-%s.qcow2", key);
            if (!path_of(disk, site->cache, name))
                break;
            snprintf(name, sizeof(name), "tooling-%s.tools", key);
            if (!path_of(tools, site->cache, name))
                break;
            intptr_t preparation;
            if (prepare_guard(site, key, 0, &preparation))
                continue;
            if (completed(disk, tools)) {
                if (discard(disk) || discard(tools))
                    fprintf(stderr, "etude tooling: cannot remove obsolete machine %s\n", key);
                else
                    fprintf(stderr, "etude tooling: removed obsolete machine %s\n", key);
            }
            tooling_cache_leave(preparation);
        }
    tooling_cache_leave(guard);
    free(c);
}

/**
 * Append "VERB PATH" to the scratch record that ETUDE_SCRATCH_RECORD names.
 *
 * A release run on Windows names the record, and tools/flow/winclean.sh
 * removes each path whose last line says "made" once the run's processes are
 * stopped: what a process stopped by force leaves, never what it kept as
 * evidence or what another run made. PATH is made absolute, with forward
 * slashes on Windows. Each line is one append on a handle opened for it, so
 * the run's processes may note at once. Does nothing when the variable is
 * unset; a line that cannot be written is reported on stderr.
 */
void tooling_scratch(const char *verb, const char *path)
{
    const char *record = getenv("ETUDE_SCRATCH_RECORD");
    char line[CACHE_PATH + 16], cwd[CACHE_PATH];
    int n, ok;

    if (!record || !*record || !path || !*path || strchr(path, '\n'))
        return;
#ifdef _WIN32
    if (etude_win_absolute(path))
#else
    if (path[0] == '/')
#endif
        n = snprintf(line, sizeof(line), "%s %s\n", verb, path);
    else if (getcwd(cwd, sizeof(cwd)) != NULL)
        n = snprintf(line, sizeof(line), "%s %s/%s\n", verb, cwd, path);
    else
        n = -1;
    ok = n > 0 && n < (int)sizeof(line);
#ifdef _WIN32
    if (ok) {
        HANDLE h;
        DWORD wrote = 0;
        int i;

        for (i = 0; i < n; i++)
            if (line[i] == '\\')
                line[i] = '/';
        /* FILE_APPEND_DATA alone makes each write an atomic append. */
        h = CreateFileA(record, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                        OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        ok = h != INVALID_HANDLE_VALUE && WriteFile(h, line, (DWORD)n, &wrote, NULL) &&
             wrote == (DWORD)n;
        if (h != INVALID_HANDLE_VALUE)
            CloseHandle(h);
    }
#else
    if (ok) {
        int fd = open(record, O_WRONLY | O_APPEND | O_CREAT | O_CLOEXEC, 0600);

        ok = fd >= 0 && write(fd, line, (size_t)n) == (ssize_t)n;
        if (fd >= 0)
            close(fd);
    }
#endif
    if (!ok)
        fprintf(stderr, "etude: scratch record: cannot note %s %s in %s\n", verb, path, record);
}
