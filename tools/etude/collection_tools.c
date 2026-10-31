/* Native collection preparation and compatibility image commands. */
#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#endif
#define _FILE_OFFSET_BITS 64
#if defined(__APPLE__) && !defined(_DARWIN_C_SOURCE)
#define _DARWIN_C_SOURCE
#endif
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#ifdef _WIN32
#include "win/files.h"
#endif
#include "etude/exec.h"
#include "json.h"
#include "collection_tools.h"
#include "routine_proof.h"
#include "../../utils/sha256.h"

#include "etude/tooling.h"
#include "tooling_cache.h"
#ifdef _WIN32
#define environ _environ
#else
extern char **environ;
#endif
#define COL_PATH    4096
#define COL_MAX     64
#define COL_CAPTURE (16u << 20)
static volatile sig_atomic_t col_stop;
static int col_reported; /* a message has given the reason of a refusal */
static const char *const col_tools[] = {
    "mattrib", "mcopy",  "mdel",     "mdeltree", "mdir",       "mformat",    "minfo",
    "mlabel",  "mmd",    "mmove",    "mrd",      "mren",       "mtype",      "mke2fs",
    "debugfs", "e2fsck", "mkfs.fat", "fsck.fat", "mkfs.exfat", "fsck.exfat", NULL};

struct col_list {
    char **v;
    size_t n;
};
struct col_job {
    char root[COL_PATH], work[COL_PATH];
    struct col_list inputs, disks, outputs, back, dest;
    int uncertain;
    unsigned commands;
    unsigned cpus; /* the processors of the job's machine, and make's jobs in it */
};

static int col_error(const char *format, ...)
{
    va_list ap;
    fputs("etude prepare tools: ", stderr);
    va_start(ap, format);
    vfprintf(stderr, format, ap);
    va_end(ap);
    fputc('\n', stderr);
    col_reported = 1;
    return -1;
}

static char *col_format(const char *format, ...)
{
    va_list ap;
    va_start(ap, format);
    int n = vsnprintf(NULL, 0, format, ap);
    va_end(ap);
    char *text = n < 0 ? NULL : malloc((size_t)n + 1);
    if (text) {
        va_start(ap, format);
        vsnprintf(text, (size_t)n + 1, format, ap);
        va_end(ap);
    }
    return text;
}

static int col_add(struct col_list *list, const char *text)
{
    char **next = realloc(list->v, (list->n + 2) * sizeof(*next));
    if (!next)
        return -1;
    list->v = next;
    next[list->n] = strdup(text);
    if (!next[list->n])
        return -1;
    next[++list->n] = NULL;
    return 0;
}

static void col_free(struct col_list *list)
{
    for (size_t i = 0; i < list->n; i++)
        free(list->v[i]);
    free(list->v);
    memset(list, 0, sizeof(*list));
}

static int col_dirs(const char *path, int parent)
{
    char text[COL_PATH];
    if (snprintf(text, sizeof text, "%s", path) >= (int)sizeof text)
        return errno = ENAMETOOLONG, -1;
    for (char *p = text + (text[0] && text[1] == ':' ? 3 : 1); *p; p++) {
        if (*p != '/' && *p != '\\')
            continue;
        char saved = *p;
        *p = 0;
        struct stat st;
        int rc = mkdir(text, 0700);
        *p = saved;
        if (rc && errno != EEXIST)
            return -1;
        *p = 0;
        rc = stat(text, &st);
        *p = saved;
        if (rc)
            return -1;
        if (!S_ISDIR(st.st_mode))
            return errno = ENOTDIR, -1;
    }
    return parent || !mkdir(text, 0700) || errno == EEXIST ? 0 : -1;
}

static int col_exec(struct col_job *, char *const[], char **, size_t *);

#ifdef _WIN32
/* Supporting MSYS callers may still supply POSIX image operands. */
static char *col_msys_path(const char *given)
{
    struct col_job job = {0};
    if (!getcwd(job.root, sizeof job.root))
        return NULL;
    char *argv[] = {"cygpath", "-m", "--", (char *)given, NULL}, *out = NULL;
    size_t size = 0;
    if (col_exec(&job, argv, &out, &size))
        return NULL;
    while (size && (out[size - 1] == '\n' || out[size - 1] == '\r'))
        out[--size] = 0;
    if (!size || strchr(out, '\n')) {
        free(out);
        return NULL;
    }
    return out;
}
#endif

/* A missing output is resolved through its existing parent, without a shell. */
static char *col_path(const char *given)
{
#ifdef _WIN32
    if (given[0] == '/' && given[1] != '/') {
        char *native = col_msys_path(given);
        char *result = native ? col_path(native) : NULL;
        free(native);
        return result;
    }
#endif
    char *absolute = realpath(given, NULL);
    if (absolute)
        return absolute;
    if (errno != ENOENT)
        return NULL;
    if (!strcmp(given, ".") || !strcmp(given, "/"))
        return NULL;
    char *copy = strdup(given), *slash;
    if (!copy)
        return NULL;
    slash = strrchr(copy, '/');
#ifdef _WIN32
    char *back = strrchr(copy, '\\');
    if (back && (!slash || back > slash))
        slash = back;
#endif
    const char *name = slash ? slash + 1 : copy;
    if (!*name || !strcmp(name, ".") || !strcmp(name, "..")) {
        free(copy);
        return NULL;
    }
    if (slash)
        *slash = 0;
    char *parent = col_path(slash ? (*copy ? copy : "/") : ".");
    char *out = parent ? col_format("%s/%s", parent, name) : NULL;
    free(parent);
    free(copy);
    return out;
}

static int col_remove(const char *path)
{
    struct stat st;
    if (lstat(path, &st))
        return errno == ENOENT ? 0 : -1;
    if (!S_ISDIR(st.st_mode))
        return unlink(path);
    DIR *dir = opendir(path);
    if (!dir)
        return -1;
    int rc = 0;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, ".."))
            continue;
        char *child = col_format("%s/%s", path, entry->d_name);
        if (!child || col_remove(child))
            rc = -1;
        free(child);
    }
    closedir(dir);
    return rc ? rc : rmdir(path);
}

static int col_copy_times(FILE *input, FILE *output, const struct stat *st)
{
#ifdef _WIN32
    FILETIME created, accessed, written;
    (void)st;
    HANDLE from = (HANDLE)_get_osfhandle(fileno(input));
    HANDLE to = (HANDLE)_get_osfhandle(fileno(output));
    return GetFileTime(from, &created, &accessed, &written) &&
                   SetFileTime(to, &created, &accessed, &written)
               ? 0
               : -1;
#else
    (void)input;
#ifdef __APPLE__
    struct timespec times[] = {st->st_atimespec, st->st_mtimespec};
#else
    struct timespec times[] = {st->st_atim, st->st_mtim};
#endif
    return futimens(fileno(output), times);
#endif
}

static int col_copy_file(const char *from, const char *to, const struct stat *st)
{
    struct stat target;
    if (col_dirs(to, 1))
        return -1;
    if (!lstat(to, &target) && S_ISREG(target.st_mode) && chmod(to, 0600))
        return -1;
    FILE *input = fopen(from, "rb"), *output = input ? fopen(to, "wb") : NULL;
    int rc = !input || !output;
    unsigned char bytes[65536];
    size_t n;
    while (!rc && !col_stop && (n = fread(bytes, 1, sizeof bytes, input)) != 0)
        if (fwrite(bytes, 1, n, output) != n)
            rc = -1;
    if (col_stop || (input && ferror(input)))
        rc = -1;
    if (!rc && (fflush(output) || col_copy_times(input, output, st)))
        rc = -1;
    if (input && fclose(input))
        rc = -1;
    if (output && fclose(output))
        rc = -1;
    if (!rc && chmod(to, st->st_mode & 0777))
        rc = -1;
    return rc;
}

/* Copy a staged tree, rejecting links and special files on both sides. */
static int col_copy(const char *from, const char *to, unsigned depth)
{
    struct stat st, target;
    if (col_stop || depth > 64 || lstat(from, &st))
        return -1;
    if (!lstat(to, &target) && !S_ISREG(target.st_mode) && !S_ISDIR(target.st_mode))
        return -1;
    if (S_ISREG(st.st_mode))
        return col_copy_file(from, to, &st);
    if (!S_ISDIR(st.st_mode) || col_dirs(to, 0))
        return -1;
    DIR *dir = opendir(from);
    struct dirent *entry;
    int rc = dir ? 0 : -1;
    while (dir && (entry = readdir(dir)) != NULL) {
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, ".."))
            continue;
        char *a = col_format("%s/%s", from, entry->d_name);
        char *b = col_format("%s/%s", to, entry->d_name);
        if (!a || !b || col_copy(a, b, depth + 1))
            rc = -1;
        free(a);
        free(b);
        if (rc)
            break;
    }
    if (dir)
        closedir(dir);
    return rc;
}

/* Quote one operand for the existing Linux tooling job's shell. */
static void col_quote(struct jbuf *text, const char *word)
{
    jbuf_puts(text, "'");
    for (; *word; word++)
        if (*word == '\'')
            jbuf_puts(text, "'\\''");
        else
            jbuf_put(text, word, 1);
    jbuf_puts(text, "'");
}

static char *col_quoted(const char *word)
{
    struct jbuf text = {0};
    col_quote(&text, word);
    return text.data;
}

static void col_signal(int number)
{
    col_stop = number;
}
static void col_signals(void)
{
    signal(SIGINT, col_signal);
    signal(SIGTERM, col_signal);
#ifndef _WIN32
    signal(SIGHUP, col_signal);
#endif
}

static int col_program(const char *name, char out[COL_PATH])
{
#ifdef _WIN32
    if (!strcmp(name, "tar")) {
        const char *shell = etude_win_shell();
        if (!shell || snprintf(out, COL_PATH, "%s", shell) >= COL_PATH)
            return -1;
        char *slash = strrchr(out, '/'), *back = strrchr(out, '\\');
        if (back && (!slash || back > slash))
            slash = back;
        if (!slash || (size_t)(slash - out) + strlen("/tar.exe") >= COL_PATH)
            return -1;
        strcpy(slash + 1, "tar.exe");
        return access(out, X_OK);
    }
    return etude_win_program(name, out, COL_PATH);
#else
    if (strchr(name, '/'))
        return realpath(name, out) ? 0 : -1;
    const char *path = getenv("PATH");
    for (const char *p = path ? path : "/usr/bin:/bin";;) {
        size_t n = strcspn(p, ":");
        int size = snprintf(out, COL_PATH, "%.*s/%s", (int)n, p, name);
        if (size >= 0 && size < COL_PATH && !access(out, X_OK))
            return 0;
        if (!p[n])
            return -1;
        p += n + 1;
    }
#endif
}

static int col_follow(FILE **stream, const char *path, FILE *dest)
{
    if (!*stream)
        *stream = fopen(path, "rb");
    if (!*stream)
        return errno == ENOENT ? 0 : -1;
    clearerr(*stream);
    char bytes[16384];
    size_t size;
    while ((size = fread(bytes, 1, sizeof bytes, *stream)))
        if (fwrite(bytes, 1, size, dest) != size)
            return -1;
    fflush(dest);
    return ferror(*stream) ? -1 : 0;
}

static int col_arguments(struct col_job *job, char *const argv[], struct col_list *args)
{
#ifndef _WIN32
    (void)job;
#endif
    for (size_t i = 1; argv[i]; i++) {
#ifdef _WIN32
        /* MSYS tar needs its archive, index and -C paths in MSYS form. */
        if (!strcmp(argv[0], "tar") && etude_win_absolute(argv[i])) {
            char *convert[] = {"cygpath", "-u", "--", argv[i], NULL}, *path = NULL;
            size_t size = 0;
            if (col_exec(job, convert, &path, &size))
                return -1;
            while (size && (path[size - 1] == '\n' || path[size - 1] == '\r'))
                path[--size] = 0;
            int bad = !size || strchr(path, '\n') || col_add(args, path);
            free(path);
            if (bad)
                return -1;
            continue;
        }
#endif
        if (col_add(args, argv[i]))
            return -1;
    }
    return 0;
}

static int col_wait(struct etude_exec *executor, struct etude_proc *process, const char *out_path,
                    const char *err_path)
{
    FILE *out_file = NULL, *err_file = NULL;
    int output_error = 0;
    for (;;) {
        struct etude_proc *ended = etude_exec_wait(executor, 100);
        if ((out_path && col_follow(&out_file, out_path, stdout)) ||
            (err_path && col_follow(&err_file, err_path, stderr)))
            output_error = 1;
        if (ended == process)
            break;
        if (col_stop || output_error)
            etude_exec_cancel(process);
    }
    if (out_file)
        fclose(out_file);
    if (err_file)
        fclose(err_file);
    return output_error;
}

/* A child must end with confirmed cleanup before its transfers are removed. */
static int col_exec(struct col_job *job, char *const argv[], char **capture, size_t *length)
{
    struct etude_exec *executor = etude_exec_new();
    struct etude_proc *process = NULL;
    char program[COL_PATH];
    struct col_list args = {0};
    int rc = 2, output_error = 0;
    char *out_path = col_format("%s/child-%u.stdout", job->work, ++job->commands);
    char *err_path = col_format("%s/child-%u.stderr", job->work, job->commands);
    if (col_stop || !out_path || !err_path || !executor || col_program(argv[0], program) ||
        col_add(&args, program))
        goto done;
    if (col_arguments(job, argv, &args))
        goto done;
    struct etude_exec_spec spec = {.attempt_id = "collection-tools",
                                   .argv = args.v,
                                   .envp = environ,
                                   .cwd = job->root,
                                   .output_max = COL_CAPTURE,
                                   .grace_us = 1000000,
                                   .stdout_path = capture ? NULL : out_path,
                                   .stderr_path = *job->work ? err_path : NULL};
    if (etude_exec_start(executor, &spec, &process))
        goto done;
    output_error =
        col_wait(executor, process, capture ? NULL : out_path, *job->work ? err_path : NULL);
    const struct etude_exec_result *result = etude_proc_result(process);
    job->uncertain |= result->cleanup != ETUDE_CLEANUP_CONFIRMED;
    if (job->uncertain || output_error || result->end != ETUDE_END_EXITED || result->output_error ||
        (capture && result->out.total != result->out.kept))
        goto done;
    rc = result->value;
    if (!*job->work && result->err.kept)
        fwrite(result->err.data, 1, result->err.kept, stderr);
    if (!rc && capture) {
        *capture = malloc(result->out.kept + 1);
        if (!*capture)
            rc = 2;
        else {
            memcpy(*capture, result->out.data, result->out.kept);
            (*capture)[result->out.kept] = 0;
            *length = result->out.kept;
        }
    }
done:
    if (executor)
        etude_exec_free(executor);
    free(out_path);
    free(err_path);
    col_free(&args);
    return rc;
}

static int col_write(const char *path, const void *bytes, size_t size)
{
    FILE *file = fopen(path, "wb");
    if (!file)
        return -1;
    int rc = fwrite(bytes, 1, size, file) != size;
    return fclose(file) || rc ? -1 : 0;
}

static int col_copy_back(struct col_job *job, int status)
{
    int rc = status;
    if (!job->uncertain)
        for (size_t i = 0; i < job->back.n; i++) {
            char *source = col_format("%s/out/%s", job->work, job->back.v[i]);
            struct stat st;
            if (!source)
                rc = 2;
            else if (lstat(source, &st)) {
                if (errno != ENOENT)
                    rc = 2;
            } else if (col_copy(source, job->dest.v[i], 0))
                rc = 2;
            free(source);
        }
    return rc;
}

static int col_tooling_status(struct col_job *job, int prepare, int status,
                              const struct etude_tooling_result *result)
{
    if (result->out_len)
        fwrite(result->out, 1, result->out_len, stdout);
    if (result->err_len)
        fwrite(result->err, 1, result->err_len, stderr);
    if (result->cost.nsteps && result->cost.cleanup != ETUDE_CLEANUP_CONFIRMED)
        job->uncertain = 1;
    int rc = !status && !job->uncertain              ? (prepare ? 0 : result->status)
             : result->outcome == ETUDE_TOOLING_LATE ? 124
             : strstr(result->why, "is not made")    ? 126
                                                     : 125;
    if (status || job->uncertain)
        fprintf(stderr, "etude provision: %s; evidence: %s\n", result->why, result->dir);
    else if (prepare)
        fprintf(stderr, "etude provision: result: %s\n", result->reused ? "reused" : "made");
    return rc;
}

/* Preserve public defaults and read the owned boot's actual cleanup result. */
static int col_tooling(struct col_job *job, int prepare, unsigned volume, unsigned memory,
                       int network)
{
    char *recipe = col_format("%s/tools/etude/tooling", job->root);
    char *volume_tool = col_format("%s/build/tools/etude/etude-volume", job->root);
    char *out = col_format("%s/out", job->work);
    char *script = col_format("%s/job.sh", job->work);
    const char *given_cache = getenv("ETUDE_TOOLING_CACHE");
    char *cache = given_cache && *given_cache ? col_path(given_cache) : NULL;
    int cache_error = errno, rc = 2;
    struct etude_tooling_result result;
    struct etude_tooling_site site = {.recipe = recipe,
                                      .volume = volume_tool,
                                      .cache = cache ? cache : tooling_cache_default(),
                                      .arch = getenv("ETUDE_TOOLING_ARCH"),
                                      .accel = getenv("RACH2_QEMU_ACCEL"),
                                      .envp = environ,
                                      .stop = &col_stop};
    struct etude_tooling_job request = {.script = script,
                                        .outdir = out,
                                        .inputs = (const char *const *)job->inputs.v,
                                        .disks = (const char *const *)job->disks.v,
                                        .outputs = (const char *const *)job->outputs.v,
                                        .cpus = job->cpus,
                                        .memory_mib = memory,
                                        .volume_mib = volume,
                                        .network = network};
    if (!recipe || !volume_tool || !out || !script) {
        col_error("out of memory");
        goto done;
    }
    if (!site.accel || !*site.accel) {
        col_error("name QEMU's accelerator with RACH2_QEMU_ACCEL");
        goto done;
    }
    if (given_cache && *given_cache && !cache) {
        col_error("cannot resolve ETUDE_TOOLING_CACHE %s: %s", given_cache, strerror(cache_error));
        goto done;
    }
    if (!prepare && col_dirs(out, 0)) {
        col_error("cannot make %s: %s", out, strerror(errno));
        goto done;
    }
#ifdef _WIN32
    if (access(volume_tool, F_OK)) {
        char *native = col_format("%s.exe", volume_tool);
        if (!native) {
            col_error("out of memory");
            goto done;
        }
        free(volume_tool);
        site.volume = volume_tool = native;
    }
#endif
    int status = prepare ? etude_tooling_prepare(&site, job->cpus, 0, &result)
                         : etude_tooling_run(&site, &request, &result);
    rc = col_tooling_status(job, prepare, status, &result);
    etude_tooling_result_free(&result);
    if (!prepare)
        rc = col_copy_back(job, rc);
done:
    free(cache);
    free(recipe);
    free(volume_tool);
    free(out);
    free(script);
    return rc;
}

static int col_run(struct col_job *job, struct jbuf *script, unsigned volume, unsigned memory,
                   int network)
{
    char *path = col_format("%s/job.sh", job->work);
    int rc = !path || col_write(path, script->data, script->len);
    free(path);
    return rc ? 2 : col_tooling(job, 0, volume, memory, network);
}

static int col_decimal(const char *text)
{
    return *text && strspn(text, "0123456789") == strlen(text);
}

static char *col_disk(struct col_job *job, const char *text)
{
    const char *suffix = strstr(text, "@@");
    char *given = suffix ? strndup(text, (size_t)(suffix - text)) : strdup(text);
    char *path = given ? col_path(given) : NULL;
    struct stat st;
    if (!path || stat(path, &st) || !S_ISREG(st.st_mode) || (suffix && !col_decimal(suffix + 2))) {
        free(given);
        free(path);
        return NULL;
    }
    size_t i;
    for (i = 0; i < job->disks.n; i++)
        if (!strcmp(job->disks.v[i], path))
            break;
    int rc = i == job->disks.n && (i >= COL_MAX || col_add(&job->disks, path));
    char *out = rc ? NULL : col_format("\"$ETUDE_DISK%zu\"%s", i + 1, suffix ? suffix : "");
    free(given);
    free(path);
    return out;
}

static char *col_file(struct col_job *job, const char *text, int output)
{
    char *path = col_path(text), *directory = NULL, *staged = NULL, *name = NULL, *result = NULL;
    struct stat st;
    if (!path || job->inputs.n >= COL_MAX)
        goto done;
    const char *base = strrchr(path, '/');
    base = base ? base + 1 : path;
    name = col_format("file%zu", job->inputs.n);
    directory = name ? col_format("%s/%s", job->work, name) : NULL;
    staged = directory ? col_format("%s/%s", directory, base) : NULL;
    if (!staged || mkdir(directory, 0700))
        goto done;
    if (lstat(path, &st) ? (!output || errno != ENOENT) : col_copy(path, staged, 0))
        goto done;
    if (col_add(&job->inputs, directory))
        goto done;
    char *relative = col_format("%s/%s", name, base);
    if (relative && (!output || (!col_add(&job->outputs, name) && !col_add(&job->back, relative) &&
                                 !col_add(&job->dest, path))))
        result = col_quoted(relative);
    free(relative);
done:
    free(path);
    free(directory);
    free(staged);
    free(name);
    return result;
}

/* Read the shell-like request syntax accepted by debugfs -R. */
static int col_words(const char *text, struct col_list *words)
{
    while (*text) {
        while (isspace((unsigned char)*text))
            text++;
        if (!*text)
            break;
        struct jbuf word = {0};
        int quote = 0, any = 0, rc = 0;
        for (; *text; text++) {
            char ch = *text;
            if (!quote && isspace((unsigned char)ch))
                break;
            any = 1;
            if (ch == quote)
                quote = 0;
            else if (!quote && (ch == '\'' || ch == '"'))
                quote = ch;
            else if (ch == '\\' && quote != '\'') {
                if (!text[1]) {
                    rc = -1;
                    break;
                }
                if (quote == '"' && text[1] != '"' && text[1] != '\\')
                    jbuf_put(&word, text, 1);
                else
                    jbuf_put(&word, ++text, 1);
            } else
                jbuf_put(&word, text, 1);
        }
        if (quote || rc || (any && col_add(words, word.data ? word.data : "")))
            rc = -1;
        jbuf_free(&word);
        if (rc)
            return -1;
    }
    return 0;
}

static int col_debugfs(struct col_job *job, struct col_list *words, char *const args[], int count)
{
    for (int i = 0; i + 1 < count; i++) {
        if (strcmp(args[i], "-R"))
            continue;
        struct col_list request = {0};
        int rc = col_words(args[i + 1], &request);
        if (!rc && request.n &&
            (!strcmp(request.v[0], "dump") || !strcmp(request.v[0], "rdump") ||
             !strcmp(request.v[0], "write"))) {
            int output = strcmp(request.v[0], "write") != 0;
            size_t at = output ? request.n - 1 : 1;
            char *file = request.n >= 3 ? col_file(job, request.v[at], output) : NULL;
            struct col_list decoded = {0};
            if (!file || col_words(file, &decoded) || decoded.n != 1)
                rc = -1;
            else {
                free(request.v[at]);
                request.v[at] = json_alloc(strlen(decoded.v[0]) + 1);
                strcpy(request.v[at], decoded.v[0]);
                struct jbuf command = {0};
                for (size_t k = 0; k < request.n; k++) {
                    if (k)
                        jbuf_puts(&command, " ");
                    col_quote(&command, request.v[k]);
                }
                free(words->v[i + 2]);
                words->v[i + 2] = col_quoted(command.data);
                jbuf_free(&command);
                rc = words->v[i + 2] ? 0 : -1;
            }
            col_free(&decoded);
            free(file);
        }
        col_free(&request);
        return rc;
    }
    return 0;
}

static int col_mtools(struct col_job *job, const char *tool, int count, char *const args[],
                      struct col_list *words)
{
    int image = -1, rc = 0;
    for (int i = 0; i + 1 < count; i++)
        if (!strcmp(args[i], "-i")) {
            image = i + 1;
            break;
        }
    if (image < 0)
        rc = -1;
    else {
        free(words->v[image + 1]);
        words->v[image + 1] = col_disk(job, args[image]);
        rc = !words->v[image + 1];
    }
    if (!rc && !strcmp(tool, "mcopy")) {
        rc = count < image + 3;
        for (int i = image + 1; !rc && i < count; i++)
            if (args[i][0] != '-' && strncmp(args[i], "::", 2)) {
                free(words->v[i + 1]);
                words->v[i + 1] = col_file(job, args[i], i == count - 1);
                rc = !words->v[i + 1];
            }
    }
    return rc;
}

static int col_fstools(struct col_job *job, const char *tool, int count, char *const args[],
                       struct col_list *words)
{
    int image, rc = 0;
    image = count - (col_decimal(args[count - 1]) ? 2 : 1);
    if (image < 0)
        rc = -1;
    else {
        const char *suffix = strstr(args[image], "?offset=");
        char *path =
            suffix ? strndup(args[image], (size_t)(suffix - args[image])) : strdup(args[image]);
        char *disk = path ? col_disk(job, path) : NULL;
        free(words->v[image + 1]);
        char *quoted = col_quoted(suffix ? suffix : "");
        words->v[image + 1] = disk && quoted ? col_format("%s%s", disk, quoted) : NULL;
        free(quoted);
        free(path);
        free(disk);
        rc = !words->v[image + 1] || (suffix && !col_decimal(suffix + 8));
    }
    for (int i = 0; !rc && !strcmp(tool, "mke2fs") && i + 1 < count; i++)
        if (!strcmp(args[i], "-d")) {
            free(words->v[i + 2]);
            words->v[i + 2] = col_file(job, args[i + 1], 0);
            rc = !words->v[i + 2];
            break;
        }
    if (!rc && !strcmp(tool, "debugfs"))
        rc = col_debugfs(job, words, args, count);
    return rc;
}

static int col_translate(struct col_job *job, const char *tool, int count, char *const args[],
                         struct jbuf *script)
{
    size_t which;
    for (which = 0; col_tools[which] && strcmp(tool, col_tools[which]); which++) {
    }
    if (!col_tools[which] || !count)
        return col_error("unknown tool or missing image: %s", tool);
    struct col_list words = {0};
    int rc = col_add(&words, tool);
    for (int i = 0; !rc && i < count; i++) {
        char *quoted = col_quoted(args[i]);
        rc = !quoted || col_add(&words, quoted);
        free(quoted);
    }
    if (!rc)
        rc = which < 13 ? col_mtools(job, tool, count, args, &words)
                        : col_fstools(job, tool, count, args, &words);
    if (!rc) {
        jbuf_puts(script, "set +e\n");
        for (size_t i = 0; i < words.n; i++) {
            if (i)
                jbuf_puts(script, " ");
            jbuf_puts(script, words.v[i]);
        }
        jbuf_puts(script, "\nstatus=$?\nexit \"$status\"\n");
    }
    col_free(&words);
    return rc;
}

static int col_native_case(const char *const parts[], size_t count)
{
    if (!strcmp(parts[3], "gnu"))
        return count == 5 && !strcmp(parts[4], "sntpserver");
    if (strcmp(parts[3], "host") && strcmp(parts[3], "machine"))
        return 0;
    return count == 5 && !strchr(parts[4], '.') && strcmp(parts[4], "fstool") &&
           strcmp(parts[4], "root-password") && strcmp(parts[4], "test-password");
}

static int col_native_suite(const char *const parts[], size_t count)
{
    const char *tail = count > 3 ? parts[count - 1] : "";
    size_t n = strlen(tail), suffix = strlen(".inventory.json");
    return (count == 4 && (!strcmp(tail, "vfs_unmount") ||
                           (n >= suffix && !strcmp(tail + n - suffix, ".inventory.json")))) ||
           (count == 5 && (!strcmp(parts[3], "scripts") ||
                           (!strcmp(parts[3], "sanitizer") && !strcmp(tail, "tools.json")) ||
                           (!strcmp(parts[3], "tests") && !strcmp(tail, "usbstamp_hosttest"))));
}

/* Keep native programs apart from prepared guest programs and data. */
int etude_collection_native_goal(const char *goal)
{
    char text[COL_PATH], *save = NULL;
    const char *parts[16];
    if (snprintf(text, sizeof text, "%s", goal) >= (int)sizeof text)
        return 0;
    size_t count = 0;
    for (char *p = strtok_r(text, "/", &save); p; p = strtok_r(NULL, "/", &save)) {
        if (count == 16)
            return 0;
        parts[count++] = p;
    }
    if (count > 1 && !strcmp(parts[0], "build") && !strcmp(parts[1], "x86_64")) {
        memmove(parts + 1, parts + 2, (count - 2) * sizeof(*parts));
        count--;
    }
    if (count < 3 || strcmp(parts[0], "build"))
        return 0;
    if (!strcmp(parts[1], "tools"))
        return !(count >= 4 && !strcmp(parts[2], "etude") && !strcmp(parts[3], "guest"));
    if (!strcmp(parts[1], "krffs"))
        return 1;
    if (strcmp(parts[1], "tests"))
        return 0;
    if (!strcmp(parts[2], "style"))
        return count == 4 && (!strcmp(parts[3], "sparse.json") || !strcmp(parts[3], "sizes.json") ||
                              !strcmp(parts[3], "manual.json"));
    if (!strcmp(parts[2], "cases") && count >= 5)
        return col_native_case(parts, count);
    if (!strcmp(parts[2], "host-suite"))
        return col_native_suite(parts, count);
    if (!strcmp(parts[2], "host"))
        return count == 4 && strcmp(parts[3], "exfat_writer") &&
               strcmp(parts[3], "tooling_machine");
    return count == 4 && !strcmp(parts[2], "system");
}

static int col_relative(const char *path)
{
    if (!*path || *path == '/' || strpbrk(path, "\\:\r\n"))
        return 0;
    for (const char *p = path; *p;) {
        size_t n = strcspn(p, "/");
        if (!n || (n == 1 && *p == '.') || (n == 2 && !strncmp(p, "..", 2)))
            return 0;
        p += n;
        if (*p && !*++p)
            return 0;
    }
    return 1;
}

static char *col_workspace(struct col_job *job, const char *arch)
{
    const char *cache = getenv("ETUDE_TOOLING_CACHE");
    char *base = cache && *cache ? col_path(cache) : col_format("%s/build/tooling", job->root);
    struct sha256 hash;
    unsigned char raw[32];
    char hex[65];
    sha256_init(&hash);
    sha256_update(&hash, (const unsigned char *)job->root, strlen(job->root));
    sha256_final(&hash, raw);
    sha256_hex(raw, hex);
    char *path = base ? col_format("%s/collection-%.16s-%s.img", base, hex, arch) : NULL;
    if (base && col_dirs(base, 0)) {
        free(path);
        path = NULL;
    }
    free(base);
    return path;
}

static int col_disk_create(const char *path, uint64_t size)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (fd < 0)
        return -1;
    int rc = ftruncate(fd, (off_t)size);
    if (close(fd))
        rc = -1;
    if (rc)
        unlink(path);
    return rc;
}

/* Publish a complete new workspace without replacing a concurrent owner. */
static int col_workspace_ready(struct col_job *job, const char *disk)
{
    struct stat st;
    if (!lstat(disk, &st))
        return S_ISREG(st.st_mode) && st.st_size == (96ll << 30) ? 0 : -1;
    if (errno != ENOENT)
        return -1;
    char *part = col_format("%s/workspace.img", job->work);
    int rc = !part || col_disk_create(part, 96ull << 30);
    if (!rc && link(part, disk)) {
        rc = errno != EEXIST || lstat(disk, &st) || !S_ISREG(st.st_mode) ||
             st.st_size != (96ll << 30);
    }
    if (part && unlink(part) && errno != ENOENT)
        rc = -1;
    free(part);
    return rc;
}

static int col_make_line(struct col_job *job, struct jbuf *script, const char *arch,
                         const struct col_list *goals)
{
    char jobs[32];
    snprintf(jobs, sizeof jobs, "make -j%u ARCH=", job->cpus);
    jbuf_puts(script, jobs);
    col_quote(script, arch);
    for (size_t i = 0; i < goals->n; i++) {
        jbuf_puts(script, " ");
        col_quote(script, goals->v[i]);
    }
    jbuf_puts(script, "\n");
    return 0;
}

static const char col_mount[] = "mkdir /mnt/collection\n"
                                "mount \"$ETUDE_DISK1\" /mnt/collection\n"
                                "trap 'cd /; umount /mnt/collection' EXIT\n";
static const char col_compilers[] = "sh tools/setup/elfwrap.sh i686 /root/bin\n"
                                    "sh tools/setup/elfwrap.sh x86_64 /root/bin\n"
                                    "export PATH=/root/bin:$PATH\n";

static int col_style_goals(struct col_job *job, const char *arch, const char *id,
                           struct col_list *goals)
{
    char *manifest = col_format("%s/tests/cases/host.style.json", job->root);
    char *case_id = col_format("host.style.%s", id);
    struct json *document = NULL;
    int rc = 2;
    if (!manifest || !case_id || routine_json(manifest, &document))
        goto done;
    const struct json *cases = json_member(document, "cases"), *selected = NULL;
    size_t matches = 0;
    if (cases && cases->type == JSON_ARRAY)
        for (size_t i = 0; i < cases->count; i++) {
            const char *name = json_text(cases->item[i], "case_id");
            if (name && !strcmp(name, case_id)) {
                selected = cases->item[i];
                matches++;
            }
        }
    const struct json *prepare = json_member(selected, "prepare");
    if (matches != 1 || !prepare || prepare->type != JSON_ARRAY)
        goto done;
    for (size_t i = 0; i < prepare->count; i++) {
        const char *node = json_text(prepare->item[i], "node");
        const struct json *per = json_member(prepare->item[i], "per_architecture");
        const char *base =
            per && per->type == JSON_BOOLEAN && per->integer && !strcmp(arch, "x86_64")
                ? "build/x86_64"
                : "build";
        char *goal = node && col_relative(node) ? col_format("%s/%s", base, node) : NULL;
        int bad = !goal || col_add(goals, goal);
        free(goal);
        if (bad)
            goto done;
    }
    rc = 0;
done:
    free(manifest);
    free(case_id);
    json_free(document);
    return rc;
}

static int col_style(struct col_job *job, const char *arch, const char *id)
{
    char *disk = col_workspace(job, arch), *copy = col_format("%s/workspace.img", job->work);
    struct col_list goals = {0};
    struct jbuf script = {0};
    int rc = 2;
    if (!disk || !copy || col_style_goals(job, arch, id, &goals))
        goto done;
    char *args[] = {"qemu-img", "convert", "-f", "raw", "-O", "raw", "-S", "4k", disk, copy, NULL};
    rc = col_exec(job, args, NULL, NULL);
    if (rc)
        goto done;
    if (col_add(&job->disks, copy) || col_add(&job->outputs, "style-evidence")) {
        rc = 2;
        goto done;
    }
    jbuf_puts(&script, "set -eu\nmkdir \"$ETUDE_WORK/style-evidence\"\n"
                       "export TMPDIR=\"$ETUDE_WORK/style-evidence\"\n");
    jbuf_puts(&script, col_mount);
    jbuf_puts(&script, "cd /mnt/collection/tree\n");
    jbuf_puts(&script, col_compilers);
    col_make_line(job, &script, arch, &goals);
    jbuf_puts(&script, "sh tests/cases/host/style.sh ");
    col_quote(&script, id);
    jbuf_puts(&script, "\n");
    rc = col_run(job, &script, 256, 2048, 0);
    if (rc) {
        const char *tmp = getenv("TMPDIR");
        char *source = col_format("%s/out/style-evidence", job->work);
        char *dest = tmp && *tmp ? col_format("%s/tooling-style-evidence", tmp)
                                 : col_format("%s/build/tests/tooling-style-evidence", job->root);
        struct stat st;
        if (source && dest && !lstat(source, &st)) {
            if (col_copy(source, dest, 0))
                job->uncertain = 1;
            fprintf(stderr, "etude prepare tools: evidence: %s\n", dest);
        }
        free(source);
        free(dest);
    }
done:
    free(disk);
    free(copy);
    col_free(&goals);
    jbuf_free(&script);
    return rc;
}

static int col_source_index(struct col_job *job, struct jbuf *tracked)
{
    char *names = NULL;
    size_t size = 0;
    char *query[] = {"git", "ls-files", "-z", NULL};
    if (col_exec(job, query, &names, &size))
        return -1;
    int rc = 0;
    for (size_t at = 0; at < size;) {
        size_t n = strnlen(names + at, size - at);
        if (!n || n == size - at || !col_relative(names + at)) {
            rc = -1;
            break;
        }
        char *path = col_format("%s/%s", job->root, names + at);
        struct stat st;
        if (!path)
            rc = -1;
        else if (!lstat(path, &st)) {
            if (!S_ISREG(st.st_mode))
                rc = -1;
            else
                jbuf_put(tracked, names + at, n + 1);
        } else if (errno != ENOENT)
            rc = -1;
        free(path);
        if (rc)
            break;
        at += n + 1;
    }
    free(names);
    return rc;
}

static int col_source_extra(struct col_job *job, const struct jbuf *tracked, struct jbuf *all)
{
    int rc = 0;
    if (tracked->len)
        jbuf_put(all, tracked->data, tracked->len);
    const char *extra[] = {"ports/doom/doom1.wad", "krffs/recovery_authorized_keys"};
    for (size_t i = 0; !rc && i < 2; i++) {
        char *path = col_format("%s/%s", job->root, extra[i]);
        struct stat st;
        int present = 0;
        for (size_t at = 0; at < tracked->len; at += strlen(tracked->data + at) + 1)
            present |= !strcmp(tracked->data + at, extra[i]);
        if (!path)
            rc = -1;
        else if (!lstat(path, &st)) {
            if (!S_ISREG(st.st_mode))
                rc = -1;
            else if (!present)
                jbuf_put(all, extra[i], strlen(extra[i]) + 1);
        } else if (errno != ENOENT)
            rc = -1;
        free(path);
    }
    return rc;
}

/* Git supplies the source list; ignored WAD and recovery keys remain inputs. */
static int col_source(struct col_job *job)
{
    struct jbuf tracked = {0}, all = {0};
    int rc = col_source_index(job, &tracked) || col_source_extra(job, &tracked, &all);
    char *index = col_format("%s/source-index", job->work);
    char *archive_index = col_format("%s/archive-index", job->work);
    char *archive = col_format("%s/source.tar", job->work);
    char *bundle = col_format("%s/source.bundle", job->work);
    if (!rc && (!index || !archive_index || !archive || !bundle ||
                col_write(index, tracked.data, tracked.len) ||
                col_write(archive_index, all.data, all.len) || col_add(&job->inputs, index)))
        rc = -1;
    char *head[] = {"git", "rev-parse", "--verify", "HEAD", NULL};
    if (!rc && !col_exec(job, head, NULL, NULL)) {
        char *args[] = {"git", "bundle", "create", bundle, "HEAD", NULL};
        rc = col_exec(job, args, NULL, NULL) || col_add(&job->inputs, bundle);
    }
    if (!rc) {
        char *args[] = {"tar",    "-cf", archive,       "--no-recursion",
                        "--null", "-T",  archive_index, NULL};
        rc = col_exec(job, args, NULL, NULL) || col_add(&job->inputs, archive);
    }
    free(index);
    free(archive_index);
    free(archive);
    free(bundle);
    jbuf_free(&tracked);
    jbuf_free(&all);
    return rc;
}

static int col_prepare_goals(struct col_list *native, struct col_list *guest, const char *arch,
                             const char *kernel, int count, char *const args[])
{
    int manual = 0;
    for (int i = 0; i < count; i++) {
        if (!col_relative(args[i]) || strncmp(args[i], "build/", 6))
            return -1;
        int own = etude_collection_native_goal(args[i]);
        manual |= own && strstr(args[i], "/style/manual.json") != NULL;
        if (col_add(own ? native : guest, args[i]))
            return -1;
    }
    const char *base = !strcmp(arch, "x86_64") ? "build/x86_64" : "build";
    char *goal = col_format("%s/rach2%s%s/kernel", base, *kernel ? "-" : "", kernel);
    int rc = !goal || col_add(guest, goal);
    free(goal);
    if (!rc && manual) {
        goal = col_format("%s/vendor/busybox/applet_tables-main.h", base);
        rc = !goal || col_add(guest, goal);
        free(goal);
    }
    return rc;
}

static const char col_sync[] =
    "mkdir -p /mnt/collection/tree\n"
    "mkdir \"$ETUDE_WORK/source\"\n"
    "tar -xf \"$ETUDE_WORK/source.tar\" -C \"$ETUDE_WORK/source\"\n"
    "tar -tf \"$ETUDE_WORK/source.tar\" | sort > \"$ETUDE_WORK/source.list\"\n"
    "cd /mnt/collection/tree\n"
    "if [ -f .etude-sources ]; then\n"
    "    comm -23 .etude-sources \"$ETUDE_WORK/source.list\" | while IFS= read -r name; do\n"
    "        case $name in */) ;; *) rm -f \"./$name\" ;; esac\n"
    "    done\n"
    "fi\n"
    "rsync -rc \"$ETUDE_WORK/source/\" ./\n"
    "cp \"$ETUDE_WORK/source.list\" .etude-sources\n"
    "git init -q\n"
    "git config core.filemode false\n"
    "if [ -f \"$ETUDE_WORK/source.bundle\" ]; then\n"
    "    git fetch -q \"$ETUDE_WORK/source.bundle\" HEAD\n"
    "    git update-ref refs/heads/master FETCH_HEAD\n"
    "    git symbolic-ref HEAD refs/heads/master\n"
    "    git read-tree HEAD\n"
    "fi\n"
    "git add -u\n"
    "git --literal-pathspecs add -f --pathspec-from-file=\"$ETUDE_WORK/source-index\" "
    "--pathspec-file-nul\n";

/* The archive of tracked sources that the GNU test image stages. */
static const char col_assets[] = "build/tests/host-suite/scripts-assets.tar";

/* Take the host's archive when it holds the same files, bytes and modes. */
static const char col_assets_adopt[] =
    "given=\"$ETUDE_WORK/scripts-assets.tar\" built=build/tests/host-suite/scripts-assets.tar\n"
    "if cmp -s \"$given\" \"$built\"; then\n"
    "    echo 'etude prepare tools: source archive: the host copy'\n"
    "elif tar -tzf \"$given\" > \"$ETUDE_WORK/assets.given\" 2> /dev/null &&\n"
    "    tar -tzf \"$built\" > \"$ETUDE_WORK/assets.built\" &&\n"
    "    cmp -s \"$ETUDE_WORK/assets.given\" \"$ETUDE_WORK/assets.built\" &&\n"
    "    ! LC_ALL=C tar -dzf \"$given\" 2>&1 | grep -v ': Mod time differs$' | grep -q .; then\n"
    "    cp \"$given\" \"$built\"\n"
    "    echo 'etude prepare tools: source archive: the host copy'\n"
    "else\n"
    "    echo 'etude prepare tools: source archive: a new build'\n"
    "fi\n";

static int col_stages_assets(const char *goal)
{
    const char *base = strrchr(goal, '/');
    base = base ? base + 1 : goal;
    return !strcmp(goal, col_assets) || !strcmp(base, "gnu-test.disk") ||
           !strcmp(base, "gnu-test.ext2");
}

/**
 * Make the GNU test images of every workspace stage one source archive.
 *
 * Each architecture's workspace builds `col_assets` itself, with its own file
 * times, so the images of two workspaces would stage different bytes, and
 * neither need equal the archive the host records. When a guest goal stages
 * the archive, the job is given the host's copy, if there is one, builds its
 * own before the images, and takes the host's in its place when the two
 * differ only in their times. The archive is then returned with the images.
 * `etude prepare` runs the x86-64 job after the i686 one, so the x86-64
 * images stage the archive that the i686 job returned.
 *
 * @return 0, or -1 when memory runs out or the host's copy is not a file.
 */
static int col_assets_job(struct col_job *job, struct jbuf *script, const char *arch,
                          struct col_list *guest)
{
    int staged = 0, listed = 0;
    for (size_t i = 0; i < guest->n; i++) {
        staged |= col_stages_assets(guest->v[i]);
        listed |= !strcmp(guest->v[i], col_assets);
    }
    if (!staged)
        return 0;
    if (!listed && col_add(guest, col_assets))
        return -1;
    char *given = col_format("%s/%s", job->root, col_assets);
    struct col_list goal = {0};
    struct stat st;
    int present = 0, rc = !given;
    if (!rc && !lstat(given, &st)) {
        present = 1;
        rc = !S_ISREG(st.st_mode);
    } else if (!rc)
        rc = errno != ENOENT;
    if (!rc && present)
        rc = col_add(&job->inputs, given) || col_add(&goal, col_assets);
    if (!rc && present) {
        col_make_line(job, script, arch, &goal);
        jbuf_puts(script, col_assets_adopt);
    }
    col_free(&goal);
    free(given);
    return rc ? -1 : 0;
}

/** Declare the musl products returned for native builds. */
static int col_native_inputs(struct col_job *job, const char *arch, const struct col_list *native,
                             struct col_list *guest)
{
    if (!native->n)
        return 0;
    char *architecture = col_format("ARCH=%s", arch), *text = NULL;
    size_t length = 0, count = 0;
    char *args[] = {"make",    "-s",         "--no-print-directory",     "-C",
                    job->root, architecture, "print-native-libc-inputs", NULL};
    int rc = !architecture || col_exec(job, args, &text, &length);
    if (!rc) {
        rc = !length || text[length - 1] != '\n';
        char *save = NULL;
        for (char *path = strtok_r(text, "\r\n", &save); !rc && path;
             path = strtok_r(NULL, "\r\n", &save)) {
            rc = !col_relative(path) || col_add(guest, path);
            count++;
        }
        rc |= !count;
    }
    free(architecture);
    free(text);
    return rc ? col_error("the native musl inputs could not be declared") : 0;
}

/** Build native goals from the returned guest products. */
static int col_native_make(struct col_job *job, const char *arch, const struct col_list *goals,
                           const struct col_list *guest)
{
    if (!goals->n)
        return 0;
    struct col_list args = {0};
    char *architecture = col_format("ARCH=%s", arch);
    const char *prefix[] = {"make",
                            "-C",
                            job->root,
                            "--no-print-directory",
                            "-j1",
                            architecture,
                            "HOSTCC=/ucrt64/bin/gcc",
                            "CC=/ucrt64/bin/gcc",
                            "HOST_TEST_PLATFORM=windows",
                            NULL};
    int rc = !architecture;
    for (size_t i = 0; !rc && prefix[i]; i++)
        rc = col_add(&args, prefix[i]);
    for (size_t i = 0; !rc && i < guest->n; i++) {
        char *path = col_format("%s/%s", job->root, guest->v[i]);
        char *old = col_format("-o%s", guest->v[i]);
        struct stat st;
        rc = !path || !old || stat(path, &st) || (!S_ISREG(st.st_mode) && !S_ISDIR(st.st_mode)) ||
             col_add(&args, old);
        free(path);
        free(old);
    }
    for (size_t i = 0; !rc && i < goals->n; i++)
        rc = col_add(&args, goals->v[i]);
    if (!rc)
        rc = col_exec(job, args.v, NULL, NULL);
    free(architecture);
    col_free(&args);
    return rc;
}

/**
 * Write the script of a preparation's job: make its workspace's file system
 * and update the source there, build the components, the kernel that `kernel`
 * names when it is not empty and the guest goals, and archive the goals on the
 * job's second disk. `col_assets_job` may add the source archive to `guest`.
 *
 * @return 0, or -1 when memory runs out.
 */
static int col_prepare_script(struct col_job *job, struct jbuf *script, const char *arch,
                              const char *kernel, struct col_list *guest)
{
    struct col_list components = {0};
    jbuf_puts(script, "set -eu\nblkid -s TYPE -o value \"$ETUDE_DISK1\" | "
                      "grep -qx ext4 || mkfs.ext4 -q -F \"$ETUDE_DISK1\"\n");
    jbuf_puts(script, col_mount);
    jbuf_puts(script, "mkdir -p /mnt/collection/store /var/cache/rach2-store\n"
                      "mount --bind /mnt/collection/store /var/cache/rach2-store\n"
                      "trap 'cd /; umount /var/cache/rach2-store; "
                      "fstrim /mnt/collection; umount /mnt/collection' EXIT\n"
                      "export RACH2_STORE=/var/cache/rach2-store\n");
    jbuf_puts(script, col_sync);
    jbuf_puts(script, col_compilers);
    int rc = col_add(&components, "components");
    if (!rc)
        col_make_line(job, script, arch, &components);
    col_free(&components);
    if (!rc && *kernel) {
        char *goal = col_format("%s-kernel", kernel);
        rc = !goal || col_add(&components, goal);
        free(goal);
        if (!rc)
            col_make_line(job, script, arch, &components);
        col_free(&components);
    }
    int gnu = 0;
    for (size_t i = 0; i < guest->n; i++) {
        const char *base = strrchr(guest->v[i], '/');
        base = base ? base + 1 : guest->v[i];
        gnu |= !strncmp(base, "gnu-test.", 9) || !strncmp(base, "os-real-gnu", 11);
    }
    if (!rc && gnu) {
        char line[128];
        unsigned js_jobs = (job->cpus - 1) * 2 / 3;
        snprintf(line, sizeof line, "export GNU_JOBS=%u\n", js_jobs ? js_jobs : 1);
        jbuf_puts(script, line);
        rc = col_add(&components, "gnu-js");
        if (!rc)
            col_make_line(job, script, arch, &components);
        col_free(&components);
    }
    if (!rc)
        rc = col_assets_job(job, script, arch, guest);
    if (rc)
        return -1;
    col_make_line(job, script, arch, guest);
    jbuf_puts(script, "tar --sparse -cf \"$ETUDE_DISK2\"");
    for (size_t i = 0; i < guest->n; i++) {
        jbuf_puts(script, " ");
        col_quote(script, guest->v[i]);
    }
    jbuf_puts(script, "\n");
    return 0;
}

/** Validate the complete declared archive before native extraction. */
static int col_products(struct col_job *job, char *products, const struct col_list *goals)
{
    char *args[] = {"tar", "-tf", products, NULL}, *names = NULL;
    size_t size = 0;
    if (col_exec(job, args, &names, &size))
        return -1;
    unsigned char *seen = calloc(goals->n ? goals->n : 1, 1);
    if (!seen) {
        free(names);
        return col_error("out of memory");
    }
    int rc = 0;
    for (char *name = names; *name && !rc;) {
        char *end = strchr(name, '\n');
        if (!end) {
            rc = -1;
            break;
        }
        *end = 0;
        size_t n = strlen(name);
        while (n && name[n - 1] == '/')
            name[--n] = 0;
        int found = 0;
        for (size_t i = 0; i < goals->n; i++) {
            size_t prefix = strlen(goals->v[i]);
            found |= n >= prefix && !strncmp(name, goals->v[i], prefix) &&
                     (!name[prefix] || name[prefix] == '/');
            seen[i] |= n == prefix && !strcmp(name, goals->v[i]);
        }
        if (!col_relative(name) || !found)
            rc = -1;
        name = end + 1;
    }
    free(names);
    const char *missing = NULL;
    for (size_t i = 0; i < goals->n && !missing; i++)
        if (!seen[i])
            missing = goals->v[i];
    free(seen);
    if (rc)
        return col_error("the products archive has an undeclared path");
    if (missing)
        return col_error("the products archive omits %s", missing);
    char *extract[] = {"tar", "-xf", products, "-C", job->root, NULL};
    return col_exec(job, extract, NULL, NULL);
}

/**
 * Size a preparation's machine, in processors and MiB of memory.
 *
 * A preparation that a claim admitted has the build jobs that the claim
 * covers, `ETUDE_BUILD_JOBS`, and the claim has a GiB of memory for each job
 * and one more, which QEMU and the controller take. The machine has one
 * processor and a GiB for each job. SpiderMonkey is built before the other
 * GNU layers, with at most two jobs per 3 GiB, since mach has its own pool.
 * A claim of fewer than three jobs is refused. A preparation that no claim
 * admitted has the processors the collection gives one machine, `RACH2_CPUS`,
 * 2 by default, and at least 3 GiB. A machine has at most 64 processors.
 *
 * @return 0, or -1 after a refusal.
 */
static int col_preparation_machine(unsigned *cpus, unsigned *memory)
{
    static const char *const names[] = {"ETUDE_BUILD_JOBS", "RACH2_CPUS"};
    unsigned jobs = 3, processors = 2;
    for (size_t i = 0; i < sizeof names / sizeof *names; i++) {
        const char *text = getenv(names[i]);
        if (text && col_decimal(text) && *text != '0' && strlen(text) <= 4) {
            jobs = (unsigned)atoi(text);
            processors = jobs;
            if (i && jobs < 3)
                jobs = 3;
            break;
        }
    }
    if (jobs < 3)
        return col_error("a claim of fewer than three build jobs leaves the preparation's "
                         "machine less than 3 GiB; claim three jobs at least");
    if (jobs > 64)
        jobs = 64;
    *cpus = processors > 64 ? 64 : processors;
    *memory = jobs * 1024;
    return 0;
}

/** Build guest products in the tooling machine and native goals on the host. */
static int col_prepare(struct col_job *job, const char *arch, const char *kernel, int count,
                       char *const args[])
{
    struct col_list native = {0}, guest = {0};
    struct jbuf script = {0};
    char *disk = NULL, *products = NULL;
    unsigned memory = 0;
    int rc = 2;
    if (strspn(kernel, "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-") !=
            strlen(kernel) ||
        col_prepare_goals(&native, &guest, arch, kernel, count, args) ||
        col_preparation_machine(&job->cpus, &memory) ||
        col_native_inputs(job, arch, &native, &guest))
        goto done;
    rc = col_tooling(job, 1, 0, 0, 0);
    if (rc)
        goto done;
    if (col_source(job))
        goto error;
    disk = col_workspace(job, arch);
    products = col_format("%s/products.tar", job->work);
    struct stat st;
    if (!disk || !products)
        goto error;
    if (col_workspace_ready(job, disk))
        goto error;
    if (col_disk_create(products, 64ull << 30) || col_add(&job->disks, disk) ||
        col_add(&job->disks, products))
        goto error;
    if (col_prepare_script(job, &script, arch, kernel, &guest))
        goto error;
    uint64_t bytes = 0;
    for (size_t i = 0; i < job->inputs.n; i++) {
        if (stat(job->inputs.v[i], &st) || st.st_size < 0)
            goto error;
        bytes += (uint64_t)st.st_size;
    }
    uint64_t volume = (bytes + 1048575) / 1048576 + 64;
    if (volume > 65536)
        goto error;
    rc = col_run(job, &script, volume < 512 ? 512 : (unsigned)volume, memory, 1);
    if (!rc)
        rc = col_products(job, products, &guest);
    if (!rc)
        rc = col_native_make(job, arch, &native, &guest);
    goto done;
error:
    rc = 2;
done:
    free(disk);
    free(products);
    col_free(&native);
    col_free(&guest);
    jbuf_free(&script);
    return rc;
}

static int col_dispatch(struct col_job *job, int argc, char **argv)
{
    int rc = 2;
    if (!strcmp(argv[0], "tool") && argc >= 3) {
        struct jbuf script = {0};
        if (!col_translate(job, argv[2], argc - 3, argv + 3, &script))
            rc = col_run(job, &script, 256, 512, 0);
        jbuf_free(&script);
    } else if (argc >= 4 && (!strcmp(argv[2], "i686") || !strcmp(argv[2], "x86_64"))) {
        if (!strcmp(argv[0], "prepare"))
            rc = col_prepare(job, argv[2], argv[3], argc - 4, argv + 4);
        else if (!strcmp(argv[0], "style") && argc == 4)
            rc = col_style(job, argv[2], argv[3]);
    }
    return rc;
}

int etude_collection_tools_main(int argc, char **argv)
{
    if (argc == 1 && !strcmp(argv[0], "list")) {
        for (size_t i = 0; col_tools[i]; i++)
            printf("%s%s", i ? " " : "", col_tools[i]);
        putchar('\n');
        return 0;
    }
    if (argc < 3)
        return col_error("usage: etude prepare tools list|tool|prepare|style [TREE ...]"), 2;
    struct col_job job = {.cpus = 1};
    col_reported = 0;
    errno = 0;
    char *root = realpath(argv[1], NULL);
    int rc = 2;
    if (!root || strlen(root) >= sizeof job.root)
        goto done;
    strcpy(job.root, root);
    int n = snprintf(job.work, sizeof job.work, "%s/build/tests/rach2-collection.XXXXXX", root);
    if (n < 0 || n >= (int)sizeof job.work || col_dirs(job.work, 1) || !mkdtemp(job.work))
        goto done;
    col_stop = 0;
    col_signals();
#ifdef _WIN32
    setenv("MSYS2_ARG_CONV_EXCL", "*", 1);
    setenv("MSYS_NO_PATHCONV", "1", 1);
#endif
    rc = col_dispatch(&job, argc, argv);
    if (job.uncertain)
        fprintf(stderr, "etude prepare tools: cleanup unconfirmed; evidence: %s\n", job.work);
    else if (col_remove(job.work))
        rc = 2;
done:
    if (rc == 2 && (col_reported || !errno))
        col_error("the collection request failed");
    else if (rc == 2)
        col_error("the collection request failed: %s", strerror(errno));
    free(root);
    col_free(&job.inputs);
    col_free(&job.disks);
    col_free(&job.outputs);
    col_free(&job.back);
    col_free(&job.dest);
    return col_stop ? 128 + col_stop : rc < 0 ? 2 : rc;
}
