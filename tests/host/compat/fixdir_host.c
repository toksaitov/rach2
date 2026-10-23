/*
 * The fixture directory of a host test, as fixdir_host.h describes it. This
 * file includes the C library's headers, and the test's own file does not
 * need to.
 */

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <dirent.h>
#include <errno.h>
#include <sys/stat.h>
#endif

#include "fixdir_host.h"

#define FIX_PATH_MAX 1024
#define FIX_NAMES    64 /* the names fix_path keeps a path for */

/* The characters a shell passes through unchanged, which a directory the
 * tests put in their commands unquoted may have. */
#define FIX_PLAIN "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789/._+-"

/* The longest $TMPDIR taken, so that every command the tests format has
 * room for the paths in it. */
#define FIX_TMP_MAX 100

static char own[FIX_PATH_MAX]; /* the test's own directory; empty until made */
static int passed;             /* set by fix_passed */

/** Whether $TMPDIR, `tmp`, can hold the fixture directory: an absolute path
 * of at most FIX_TMP_MAX characters, each one a shell passes unchanged. */
static int fix_plain(const char *tmp)
{
#ifdef _WIN32
    return tmp != NULL && strlen(tmp) <= FIX_TMP_MAX &&
           ((tmp[0] && tmp[1] == ':' && (tmp[2] == '/' || tmp[2] == '\\')) || tmp[0] == '/') &&
           tmp[strspn(tmp, FIX_PLAIN ":\\")] == '\0';
#else
    return tmp != NULL && tmp[0] == '/' && strlen(tmp) <= FIX_TMP_MAX &&
           tmp[strspn(tmp, FIX_PLAIN)] == '\0';
#endif
}

/* LeakSanitizer's check, when the test is built with it; the reference is
 * weak, so a test built without it links. */
extern int __lsan_do_recoverable_leak_check(void) __attribute__((weak));

/** Whether LeakSanitizer finds a leak now, which its own check at the end of
 * the process, after this one, reports as the test's failure. */
static int fix_leaks(void)
{
    return __lsan_do_recoverable_leak_check != NULL && __lsan_do_recoverable_leak_check() != 0;
}

#include "tree_host.h"
#define fix_remove_tree host_remove_tree

/** Keep failed fixtures as named evidence; remove successful fixtures. */
static void fix_remove(void)
{
    if (!passed || fix_leaks()) {
        printf("fixtures: kept in %s\n", own);
        return;
    }
    if (fix_remove_tree(own) != 0)
        printf("fixtures: %s could not be removed\n", own);
}

#ifdef _WIN32
/* Create the directory atomically; a colliding name is tried again. */
static char *fix_mkdtemp(char *path)
{
    size_t n = strlen(path);
    unsigned long seed = GetCurrentProcessId() ^ GetTickCount();
    if (n < 6)
        return NULL;
    for (unsigned long i = 0; i < 4096; i++) {
        snprintf(path + n - 6, 7, "%06lx", (seed + i) & 0xfffffful);
        if (CreateDirectoryA(path, NULL))
            return path;
        if (GetLastError() != ERROR_ALREADY_EXISTS)
            return NULL;
    }
    return NULL;
}
#endif

const char *fix_dir(void)
{
    const char *tmp;
    size_t len;

    if (own[0] != '\0') {
        return own;
    }
    tmp = getenv("TMPDIR");
    if (!fix_plain(tmp)) {
#ifdef _WIN32
        static char native_tmp[FIX_TMP_MAX + 1];
        DWORD got = GetTempPathA(sizeof(native_tmp), native_tmp);
        if (!got || got >= sizeof(native_tmp) || !fix_plain(native_tmp)) {
            printf("fixtures: no usable native temporary directory\n");
            exit(1);
        }
        tmp = native_tmp;
#else
        tmp = "/tmp";
#endif
    }
    len = strlen(tmp);
    while (len > 0 && tmp[len - 1] == '/') {
        len--;
    }
    snprintf(own, sizeof(own), "%.*s/rach2_fixtures.XXXXXX", (int)len, tmp);
#ifdef _WIN32
    if (fix_mkdtemp(own) == NULL) {
#else
    if (mkdtemp(own) == NULL) {
#endif
        printf("fixtures: no directory could be made under %s\n", tmp);
        exit(1);
    }
    atexit(fix_remove);
    return own;
}

const char *fix_path(const char *name)
{
    static char names[FIX_NAMES][64];
    static char paths[FIX_NAMES][FIX_PATH_MAX + 64];
    int i;

    for (i = 0; i < FIX_NAMES && names[i][0] != '\0'; i++) {
        if (strcmp(names[i], name) == 0) {
            return paths[i];
        }
    }
    if (i == FIX_NAMES || strlen(name) >= sizeof(names[0])) {
        printf("fixtures: no room for the name %s\n", name);
        exit(1);
    }
    snprintf(names[i], sizeof(names[i]), "%s", name);
    fix_format(paths[i], sizeof(paths[i]), "%s/%s", fix_dir(), name);
    return paths[i];
}

void fix_passed(void)
{
    passed = 1;
}

void fix_format(char *buf, unsigned long size, const char *format, ...)
{
    va_list ap;
    int n;

    va_start(ap, format);
    n = vsnprintf(buf, size, format, ap);
    va_end(ap);
    if (n < 0 || (unsigned long)n >= size) {
        printf("fixtures: %d bytes do not fit in %lu: %.60s\n", n, size, buf);
        exit(1);
    }
}
