/*
 * launch_make.c: `etude make`, the make wrapper that keeps a top-level build
 * inside the job count its claim admitted, with the reading of make's job
 * flags and the start of programs that `etude launch` shares with it.
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#ifdef _WIN32
#include "win/files.h"
#include "win/winfmt.h"
#else
#include <sys/wait.h>
#include <unistd.h>
#endif
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif
#include "authority_platform.h"
#include "launch.h"
#include "verbs.h"

#ifdef _WIN32
#define LAUNCH_PATH_SEPARATOR ";"
#define LAUNCH_MAKE           "make.exe"
#else
#define LAUNCH_PATH_SEPARATOR ":"
#define LAUNCH_MAKE           "make"
#endif

/* A buffer for one path. */
struct launch_path {
    char *text;
    size_t size;
};

int launch_number(const char *s)
{
    return s[0] >= '1' && s[0] <= '9' && strspn(s, "0123456789") == strlen(s);
}

int launch_absolute(const char *path)
{
    return path[0] == '/' ||
           (((path[0] >= 'A' && path[0] <= 'Z') || (path[0] >= 'a' && path[0] <= 'z')) &&
            path[1] == ':' && path[2] == '/');
}

/** Read one word of a make command line into `s`. */
static void take(struct launch_jobs *s, const char *word)
{
    if (s->pending) {
        s->pending = 0;
        if (launch_number(word)) {
            s->count = word;
            return;
        }
    }
    if (!strcmp(word, "-j") || !strcmp(word, "--jobs")) {
        s->count = s->unbounded;
        s->pending = 1;
    } else if (!strncmp(word, "-j", 2) && word[2] &&
               strspn(word + 2, "0123456789") == strlen(word + 2))
        s->count = word + 2;
    else if (!strncmp(word, "--jobs=", 7))
        s->count = word + 7;
    else if (s->jobs && !strncmp(word, "JOBS=", 5))
        s->jobs = word + 5;
}

/** Keep a copy of `text` in `s`, for its words to point into. */
static char *keep(struct launch_jobs *s, const char *text)
{
    char **copies = realloc(s->copies, (s->ncopies + 1) * sizeof *copies);
    if (!copies)
        return NULL;
    s->copies = copies;
    if (!(copies[s->ncopies] = strdup(text)))
        return NULL;
    return copies[s->ncopies++];
}

/** Read each line of `text` as one word. */
static void take_lines(struct launch_jobs *s, char *text)
{
    for (char *line = text, *next; line; line = next) {
        next = strchr(line, '\n');
        if (next)
            *next++ = 0;
        take(s, line);
    }
}

int launch_jobs_read(struct launch_jobs *s, char *const *argv)
{
    const char *given = getenv("MAKEFLAGS");
    char *flags = keep(s, given ? given : ""), *rest, *word;
    if (!flags)
        return -1;
    rest = strchr(flags, '\n');
    if (rest)
        *rest++ = 0;
    for (word = strtok(flags, " \t\v\f\r"); word && strcmp(word, "--");
         word = strtok(NULL, " \t\v\f\r"))
        take(s, word);
    s->pending = 0;
    if (rest)
        take_lines(s, rest);
    for (size_t i = 0; argv[i]; i++) {
        char *copy = keep(s, argv[i]);
        if (!copy)
            return -1;
        take_lines(s, copy);
    }
    return 0;
}

void launch_jobs_free(struct launch_jobs *s)
{
    for (size_t i = 0; i < s->ncopies; i++)
        free(s->copies[i]);
    free(s->copies);
    s->copies = NULL;
    s->ncopies = 0;
}

/** Say whether `path` is an executable regular file. */
static int executable(const char *path)
{
    struct stat st;
    return !stat(path, &st) && S_ISREG(st.st_mode) && !access(path, X_OK);
}

/**
 * Call `found` for each `DIR/name` of PATH, or of `fallback` when PATH is
 * unset or empty, that is an executable regular file, until it returns 0.
 *
 * @return 0 when `found` took one, or -1.
 */
static int each_on_path(const char *name, const char *fallback,
                        int (*found)(const char *dir, const char *path, void *data), void *data)
{
    const char *path = getenv("PATH");
    char dir[4096], candidate[4200];
    if (!path || !*path)
        path = fallback;
    for (const char *p = path;; p++) {
        size_t n = strcspn(p, LAUNCH_PATH_SEPARATOR);
        int d = snprintf(dir, sizeof dir, "%.*s", (int)n, n ? p : ".");
        int m = n ? snprintf(candidate, sizeof candidate, "%s/%s", dir, name)
                  : snprintf(candidate, sizeof candidate, "%s", name);
        if (d > 0 && (size_t)d < sizeof dir && m > 0 && (size_t)m < sizeof candidate &&
            executable(candidate) && !found(dir, candidate, data))
            return 0;
        p += n;
        if (!*p)
            return -1;
    }
}

/** Copy the first program found into the `struct launch_path` of `data`. */
static int first(const char *dir, const char *path, void *data)
{
    struct launch_path *out = data;
    (void)dir;
    if (strlen(path) >= out->size)
        return -1;
    strcpy(out->text, path);
    return 0;
}

int launch_find(const char *name, char *out, size_t size)
{
#ifdef _WIN32
    return etude_win_program(name, out, size);
#else
    struct launch_path found = {out, size};
    return each_on_path(name, "", first, &found);
#endif
}

#ifdef _WIN32
/** Let a child inherit the handle `h`. */
static void inherit(HANDLE h)
{
    if (h != NULL && h != INVALID_HANDLE_VALUE)
        SetHandleInformation(h, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT);
}

int launch_child(const char *who, char **argv)
{
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    DWORD code = 127;
    char program[4096], *line = NULL;
    wchar_t *wide = NULL;
    if (etude_win_program(argv[0], program, sizeof program) ||
        etude_win_cmdline(program, (const char *const *)argv + 1, &line) ||
        !(wide = etude_win_wide(line))) {
        fprintf(stderr, "%s: %s: cannot start the program\n", who, argv[0]);
        free(line);
        return 127;
    }
    free(line);
    memset(&si, 0, sizeof si);
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    si.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
    si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    inherit(si.hStdInput);
    inherit(si.hStdOutput);
    inherit(si.hStdError);
    if (!CreateProcessW(NULL, wide, NULL, NULL, TRUE, 0, NULL, NULL, &si, &pi)) {
        fprintf(stderr, "%s: %s: Windows error %lu\n", who, argv[0], (unsigned long)GetLastError());
        free(wide);
        return 127;
    }
    free(wide);
    CloseHandle(pi.hThread);
    WaitForSingleObject(pi.hProcess, INFINITE);
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    return (int)code;
}

int launch_replace(const char *who, char **argv, int search)
{
    (void)search;
    return launch_child(who, argv);
}
#else
/** Report a program that could not start and give the shell's status for it. */
static int not_started(const char *who, const char *program, int error)
{
    fprintf(stderr, "%s: %s: %s\n", who, program, strerror(error));
    return error == ENOENT ? 127 : 126;
}

int launch_replace(const char *who, char **argv, int search)
{
    if (search)
        execvp(argv[0], argv);
    else
        execv(argv[0], argv);
    return not_started(who, argv[0], errno);
}

int launch_child(const char *who, char **argv)
{
    int status;
    pid_t pid;
    fflush(NULL);
    pid = fork();
    if (pid < 0)
        return not_started(who, argv[0], errno);
    if (pid == 0) {
        execvp(argv[0], argv);
        _exit(not_started(who, argv[0], errno));
    }
    while (waitpid(pid, &status, 0) < 0)
        if (errno != EINTR)
            return not_started(who, argv[0], errno);
    return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
}
#endif

/** Find the identity of etude's own executable. */
static int self(struct authority_file_id *id)
{
#ifdef _WIN32
    wchar_t wide[32768];
    DWORD n = GetModuleFileNameW(NULL, wide, sizeof wide / sizeof *wide);
    char *path = n && n < sizeof wide / sizeof *wide ? etude_win_utf8(wide) : NULL;
    int rc = path ? authority_file_id(path, id) : -1;
    free(path);
    return rc;
#elif defined(__APPLE__)
    char path[4096];
    uint32_t size = sizeof path;
    return _NSGetExecutablePath(path, &size) ? -1 : authority_file_id(path, id);
#else
    return authority_file_id("/proc/self/exe", id);
#endif
}

/** Say whether `path` is etude's own executable, by its file's identity. */
static int is_self(const char *path)
{
    struct authority_file_id mine, other;
    return !self(&mine) && !authority_file_id(path, &other) && mine.device == other.device &&
           mine.inode == other.inode;
}

/** Take a make that is not this wrapper, by its directory's absolute path. */
static int real_make(const char *dir, const char *path, void *data)
{
    struct launch_path *out = data;
    char absolute[4096];
    int n;
    if (is_self(path) || !realpath(dir, absolute))
        return -1;
    n = snprintf(out->text, out->size, "%s/%s", absolute, LAUNCH_MAKE);
    return n > 0 && (size_t)n < out->size ? 0 : -1;
}

/**
 * Read the number at the start of `s` as awk converts a string to one: a
 * decimal number after blanks, and otherwise 0.
 */
static double awk_number(const char *s)
{
    const char *p = s + strspn(s, " \t\n"), *q = p + (*p == '+' || *p == '-');
    if (!((*q >= '0' && *q <= '9') || (*q == '.' && q[1] >= '0' && q[1] <= '9')) ||
        (q[0] == '0' && (q[1] == 'x' || q[1] == 'X')))
        return 0;
    return strtod(p, NULL);
}

/**
 * Give the requested job count of a top-level make whose limit is `limit`:
 * the count of its last job flag, or one, when that is at least one and less
 * than the limit, and the limit otherwise.
 */
static const char *requested(char *const *argv, const char *limit, struct launch_jobs *s)
{
    s->unbounded = limit;
    s->count = "1";
    if (launch_jobs_read(s, argv))
        return NULL;
    double count = awk_number(s->count);
    return count >= 1 && count < strtod(limit, NULL) ? s->count : limit;
}

/**
 * The make wrapper. With ETUDE_BUILD_JOBS unset, as in a child that scrubbed
 * its environment, the limit is one job, and ETUDE_MAKE_REAL, when that is
 * unset too, becomes the first make on PATH that is not this wrapper. A
 * top-level make, MAKELEVEL unset or 0, gets `-jN`, N being its own count
 * when that is below the limit and the limit otherwise; a recursive make
 * keeps its parent's jobserver.
 */
int etude_make_main(int argc, char **argv)
{
    const char *limit = getenv("ETUDE_BUILD_JOBS"), *real, *level = getenv("MAKELEVEL"), *jobs;
    char text[4096], flag[4200];
    struct launch_path found = {text, sizeof text};
    struct launch_jobs s = {0};
    if (!limit) {
        limit = "1";
        real = getenv("ETUDE_MAKE_REAL");
        if ((!real || !*real) && !each_on_path(LAUNCH_MAKE, "/usr/bin:/bin", real_make, &found))
            setenv("ETUDE_MAKE_REAL", text, 1);
    }
    if (!launch_number(limit) || strlen(limit) > 4 || atoi(limit) > 1024) {
        fputs("etude-make: invalid job count\n", stderr);
        return 2;
    }
    real = getenv("ETUDE_MAKE_REAL");
    if (!real || !launch_absolute(real)) {
        fputs("etude-make: make is not an absolute path\n", stderr);
        return 2;
    }
    if (is_self(real)) {
        fputs("etude-make: ETUDE_MAKE_REAL names this wrapper\n", stderr);
        return 2;
    }
    char **args = calloc((size_t)argc + 2, sizeof *args);
    if (!args)
        return 2;
    args[0] = (char *)real;
    memcpy(args + 1, argv + 1, (size_t)(argc - 1) * sizeof *args);
    if (!level || !*level || !strcmp(level, "0")) {
        if (!(jobs = requested(argv + 1, limit, &s))) {
            launch_jobs_free(&s);
            free(args);
            return 2;
        }
        snprintf(flag, sizeof flag, "-j%s", jobs);
        args[argc] = flag;
    }
    int status = launch_replace("etude-make", args, 0);
    launch_jobs_free(&s);
    free(args);
    return status;
}
