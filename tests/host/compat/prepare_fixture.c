/*
 * The fixtures of etude_legacy_hosttest.c's preparation and import rows, as
 * prepare_fixture.h describes them: their files, their commands, the JSON
 * they write and read, and this program's stand-ins for the commands etude
 * runs.
 */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "fixdir_host.h"
#include "prepare_fixture.h"
#include "sha256.h"
#include "tree_host.h"

extern char **environ;

int pf_fails;
char pf_repo[PF_PATH], pf_self[PF_PATH], pf_etude[PF_PATH], pf_cc[PF_PATH];

/* ---- Files. ---- */

/** Give `dir/name` in one of 32 buffers that are reused in turn. */
const char *pf_at(const char *dir, const char *name)
{
    static char ring[32][PF_PATH];
    static unsigned next;
    char *out = ring[next++ % 32];

    fix_format(out, PF_PATH, "%s/%s", dir, name);
    return out;
}

/** Make the directory `path` and its parents; 0 or -1. */
int pf_mkdirs(const char *path)
{
    char copy[PF_PATH];
    char *p;

    fix_format(copy, sizeof(copy), "%s", path);
    for (p = copy + 1; *p; p++) {
        if (*p != '/')
            continue;
        *p = 0;
        if (mkdir(copy, 0755) && errno != EEXIST)
            return -1;
        *p = '/';
    }
    return mkdir(copy, 0755) && errno != EEXIST ? -1 : 0;
}

/** Make the parent directories of `path`; 0 or -1. */
static int parents(const char *path)
{
    char copy[PF_PATH];
    char *slash;

    fix_format(copy, sizeof(copy), "%s", path);
    slash = strrchr(copy, '/');
    if (slash == NULL || slash == copy)
        return 0;
    *slash = 0;
    return pf_mkdirs(copy);
}

/* Write `size` bytes to `path` with the fopen mode `mode`, making its
 * directories; 0 or -1. */
static int put(const char *path, const void *bytes, size_t size, const char *mode)
{
    FILE *f;
    int bad;

    if (parents(path) || (f = fopen(path, mode)) == NULL)
        return -1;
    bad = size && fwrite(bytes, 1, size, f) != size;
    return fclose(f) || bad ? -1 : 0;
}

/** Make `path`, and its directories, hold `size` bytes; 0 or -1. */
int pf_write(const char *path, const void *bytes, size_t size)
{
    return put(path, bytes, size, "wb");
}

/** Make `path` hold `text`; 0 or -1. */
int pf_write_text(const char *path, const char *text)
{
    return put(path, text, strlen(text), "wb");
}

/** Add `text` to the end of `path`; 0 or -1. */
int pf_append_text(const char *path, const char *text)
{
    return put(path, text, strlen(text), "ab");
}

/** Read `path` whole into a new buffer ended by NUL; NULL when it cannot. */
char *pf_read(const char *path, size_t *size)
{
    struct jbuf b = {0};
    char chunk[65536];
    size_t n;
    FILE *f = fopen(path, "rb");

    if (f == NULL)
        return NULL;
    jbuf_puts(&b, "");
    while ((n = fread(chunk, 1, sizeof(chunk), f)) > 0)
        jbuf_put(&b, chunk, n);
    fclose(f);
    if (size)
        *size = b.len;
    return b.data;
}

/** Say whether `path` exists, a dangling link among them. */
int pf_exists(const char *path)
{
    struct stat st;

    return lstat(path, &st) == 0;
}

/** Copy the file `from` to `to` with its mode, as cp -p does; 0 or -1. */
int pf_copy(const char *from, const char *to)
{
    struct stat st;
    size_t size;
    char *bytes = pf_read(from, &size);
    int bad = bytes == NULL || stat(from, &st) || pf_write(to, bytes, size) ||
              chmod(to, st.st_mode & 07777);

    free(bytes);
    return bad ? -1 : 0;
}

/** Say whether `a` and `b` have the same bytes. */
int pf_same_file(const char *a, const char *b)
{
    size_t na, nb;
    char *x = pf_read(a, &na), *y = pf_read(b, &nb);
    int same = x && y && na == nb && !memcmp(x, y, na);

    free(x);
    free(y);
    return same;
}

/** Give the SHA-256 of `size` bytes in hex. */
void pf_sha_bytes(const void *bytes, size_t size, char hex[65])
{
    unsigned char sum[32];
    struct sha256 h;

    sha256_init(&h);
    sha256_update(&h, bytes, size);
    sha256_final(&h, sum);
    sha256_hex(sum, hex);
}

/** Give the SHA-256 of the file `path` in hex; 0 or -1. */
int pf_sha_file(const char *path, char hex[65])
{
    size_t size;
    char *bytes = pf_read(path, &size);

    hex[0] = 0;
    if (bytes == NULL)
        return -1;
    pf_sha_bytes(bytes, size, hex);
    free(bytes);
    return 0;
}

/** Find the program `name` on PATH and give its resolved path; 0 or -1. */
int pf_which(const char *name, char out[PF_PATH])
{
    const char *path = getenv("PATH");
    char dir[PF_PATH], candidate[PF_PATH];

    while (path && *path) {
        size_t n = strcspn(path, ":");
        fix_format(dir, sizeof(dir), "%.*s", (int)n, path);
        fix_format(candidate, sizeof(candidate), "%s/%s", n ? dir : ".", name);
        if (access(candidate, X_OK) == 0 && realpath(candidate, out) != NULL)
            return 0;
        path += n + (path[n] == ':');
    }
    return -1;
}

/** Make a fresh directory `name` in the fixture directory and give its path
 * in `path`. */
void pf_scratch(const char *name, char path[PF_PATH])
{
    fix_format(path, PF_PATH, "%s/%s", fix_dir(), name);
    host_remove_tree(path);
    if (pf_mkdirs(path)) {
        printf("etude-prepare-test: fail: no directory %s\n", path);
        exit(1);
    }
}

/** Remove the row's directory `path`; a failed row keeps it as evidence. */
void pf_cleanup(const char *path)
{
    if (pf_fails == 0)
        host_remove_tree(path);
}

/** Count the places `needle` starts in `text`. */
size_t pf_count(const char *text, const char *needle)
{
    size_t n = 0;

    for (; text && (text = strstr(text, needle)) != NULL; text++)
        n++;
    return n;
}

/* ---- Commands. ---- */

/**
 * Run `argv` in `cwd` with the environment changed by `env`, whose entries
 * either set a variable, `NAME=value`, or remove one, `NAME`. With `out`,
 * take its standard output and error together; without, discard them.
 * Return the exit status, 128 plus a signal, or -1.
 */
int pf_run(const char *cwd, const char *const env[], const char *const argv[], struct jbuf *out)
{
    int pipes[2], status;
    pid_t pid;
    char chunk[65536];
    ssize_t n;

    if (out)
        jbuf_puts(out, "");
    fflush(stdout);
    if (pipe(pipes))
        return -1;
    pid = fork();
    if (pid == 0) {
        int null = open("/dev/null", O_RDONLY);
        size_t i;
        dup2(null, 0);
        dup2(pipes[1], 1);
        dup2(pipes[1], 2);
        close(pipes[0]);
        close(pipes[1]);
        for (i = 0; env && env[i]; i++) {
            const char *equals = strchr(env[i], '=');
            if (equals == NULL) {
                unsetenv(env[i]);
            } else {
                char name[256];
                fix_format(name, sizeof(name), "%.*s", (int)(equals - env[i]), env[i]);
                setenv(name, equals + 1, 1);
            }
        }
        if (cwd && chdir(cwd))
            _exit(126);
        execvp(argv[0], (char *const *)argv);
        _exit(127);
    }
    close(pipes[1]);
    while ((n = read(pipes[0], chunk, sizeof(chunk))) > 0 || (n < 0 && errno == EINTR))
        if (n > 0 && out)
            jbuf_put(out, chunk, (size_t)n);
    close(pipes[0]);
    if (pid < 0 || waitpid(pid, &status, 0) != pid)
        return -1;
    return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
}

/** Run `program` alone and give its status. */
int pf_status(const char *program)
{
    const char *argv[] = {program, NULL};

    return pf_run(NULL, NULL, argv, NULL);
}

/* ---- JSON. ---- */

/** Parse `text`, as etude reads external data; NULL when it is not JSON. */
struct json *pf_jparse(const char *text)
{
    struct json *v = NULL;
    char error[256];

    if (text == NULL || json_parse_data(text, strlen(text), 1, &v, error, sizeof(error)))
        return NULL;
    return v;
}

/** Parse the file `path`; NULL when it cannot be read or is not JSON. */
struct json *pf_jload(const char *path)
{
    char *text = pf_read(path, NULL);
    struct json *v = pf_jparse(text);

    free(text);
    return v;
}

/* A new value of `type`, empty. */
static struct json *value(enum json_type type)
{
    struct json *v = json_alloc(sizeof(*v));

    v->type = type;
    return v;
}

/* A copy of `text` from the JSON reader's allocator. */
static char *copy_text(const char *text)
{
    size_t n = strlen(text) + 1;
    char *out = json_alloc(n);

    memcpy(out, text, n);
    return out;
}

/** A new string of `text`. */
struct json *pf_jstr(const char *text)
{
    struct json *v = value(JSON_STRING);

    v->string = copy_text(text);
    return v;
}

/** A new integer. */
struct json *pf_jint(long long integer)
{
    struct json *v = value(JSON_INTEGER);

    v->integer = integer;
    return v;
}

/** A new boolean, true when `truth` is not 0. */
struct json *pf_jbool(int truth)
{
    struct json *v = value(JSON_BOOLEAN);

    v->integer = truth != 0;
    return v;
}

/** A new null. */
struct json *pf_jnull(void)
{
    return value(JSON_NULL);
}

/** A new object without members. */
struct json *pf_jobj(void)
{
    return value(JSON_OBJECT);
}

/** A new array without elements. */
struct json *pf_jarr(void)
{
    return value(JSON_ARRAY);
}

/** Give a deep copy of `v`, without its member name. */
struct json *pf_jclone(const struct json *v)
{
    struct json *c = value(v->type);
    size_t i;

    c->integer = v->integer;
    c->string = v->string ? copy_text(v->string) : NULL;
    for (i = 0; i < v->count; i++) {
        struct json *item = pf_jclone(v->item[i]);
        if (v->item[i]->name)
            item->name = copy_text(v->item[i]->name);
        pf_jadd(c, item);
    }
    return c;
}

/** Append `item` to the array or object `v`, which takes it. */
void pf_jadd(struct json *v, struct json *item)
{
    struct json **grown = json_alloc((v->count + 1) * sizeof(*grown));

    if (v->count)
        memcpy(grown, v->item, v->count * sizeof(*grown));
    free(v->item);
    v->item = grown;
    v->item[v->count++] = item;
}

/** Give the member `name` of `object` the value `v`, which it takes. */
void pf_jset(struct json *object, const char *name, struct json *v)
{
    size_t i;

    free(v->name);
    v->name = copy_text(name);
    for (i = 0; i < object->count; i++) {
        if (!strcmp(object->item[i]->name, name)) {
            json_free(object->item[i]);
            object->item[i] = v;
            return;
        }
    }
    pf_jadd(object, v);
}

/** Remove the member `name` of `object`, when it has one. */
void pf_jdel(struct json *object, const char *name)
{
    size_t i;

    for (i = 0; i < object->count; i++) {
        if (!strcmp(object->item[i]->name, name)) {
            json_free(object->item[i]);
            memmove(object->item + i, object->item + i + 1,
                    (object->count - i - 1) * sizeof(*object->item));
            object->count--;
            return;
        }
    }
}

/** The member `name` of `object`, which the caller may change; or NULL. */
struct json *pf_jget(struct json *object, const char *name)
{
    return (struct json *)json_member(object, name);
}

/** Give the canonical text of `v` in a new buffer. */
char *pf_jtext(const struct json *v)
{
    struct jbuf b = {0};

    json_canonical(v, &b);
    return b.data;
}

/** Write the canonical text of `v` to `path`; 0 or -1. */
int pf_jsave(const char *path, const struct json *v)
{
    char *text = pf_jtext(v);
    int r = pf_write_text(path, text);

    free(text);
    return r;
}

/** Describe the file `path` as a record does: its sha256 and size_bytes. */
struct json *pf_describe(const char *path)
{
    struct json *v = pf_jobj();
    struct stat st;
    char hex[65];

    pf_sha_file(path, hex);
    pf_jset(v, "sha256", pf_jstr(hex));
    pf_jset(v, "size_bytes", pf_jint(stat(path, &st) ? -1 : (long long)st.st_size));
    return v;
}

/** Say whether `v` describes the file `path` as it is now. */
int pf_jsame(const struct json *v, const char *path)
{
    struct json *now = pf_describe(path);
    int same = v && json_equal(v, now);

    json_free(now);
    return same;
}

/* ---- Words. ---- */

/**
 * Split `text` into words as a POSIX shell does for quoting alone: blanks
 * separate, single quotes keep their text, double quotes keep theirs with
 * backslash before one of $`"\, and a backslash elsewhere keeps the next
 * character. Give the words in `*words`; NULL on an unclosed quote.
 */
const char *pf_split(const char *text, size_t *count, char ***words)
{
    struct jbuf word = {0};
    int in = 0;

    *words = NULL;
    *count = 0;
    for (;; text++) {
        if (*text == 0 || *text == ' ' || *text == '\t' || *text == '\n') {
            if (in) {
                char **grown = json_alloc((*count + 1) * sizeof(*grown));
                if (*count)
                    memcpy(grown, *words, *count * sizeof(*grown));
                free(*words);
                *words = grown;
                (*words)[(*count)++] = word.data ? word.data : copy_text("");
                word.data = NULL;
                word.len = word.cap = 0;
                in = 0;
            }
            if (*text == 0)
                return "";
            continue;
        }
        in = 1;
        if (*text == '\'') {
            const char *end = strchr(text + 1, '\'');
            if (end == NULL)
                break;
            jbuf_put(&word, text + 1, (size_t)(end - text - 1));
            jbuf_puts(&word, "");
            text = end;
        } else if (*text == '"') {
            for (text++; *text && *text != '"'; text++) {
                if (*text == '\\' && text[1] && strchr("$`\"\\", text[1]))
                    text++;
                jbuf_put(&word, text, 1);
            }
            if (*text == 0)
                break;
            jbuf_puts(&word, "");
        } else if (*text == '\\' && text[1]) {
            jbuf_put(&word, ++text, 1);
        } else {
            jbuf_put(&word, text, 1);
        }
    }
    jbuf_free(&word);
    pf_words_free(*words, *count);
    *words = NULL;
    *count = 0;
    return NULL;
}

/** Free the `count` words that pf_split gave. */
void pf_words_free(char **words, size_t count)
{
    size_t i;

    for (i = 0; i < count; i++)
        free(words[i]);
    free(words);
}

/** Say whether the `n` words at `words` are `expected`, a NULL-ended list. */
static int words_are(char **words, size_t n, const char *const expected[])
{
    size_t i;

    for (i = 0; i < n && expected[i]; i++)
        if (strcmp(words[i], expected[i]))
            return 0;
    return i == n && expected[i] == NULL;
}

/* ---- Stand-ins. ---- */

/*
 * This program also plays the commands that etude and the products run in
 * the rows: copied as `ssh` and `rsync` into a directory on PATH, with a file
 * `standin` beside it that gives the call log, the producer's metadata, the
 * real etude and the producer's tree, one a line; copied as `uname`, which
 * build_openssl.sh asks; and, with `--windows-cc SDK COMPILER`, as the
 * compiler of a Windows preparation.
 */

/* Append the call `argv` to the log as a JSON array of its words, the
 * program's file name first. */
static void standin_log(const char *log, int argc, char **argv)
{
    struct json *call = pf_jarr();
    const char *base = strrchr(argv[0], '/');
    char *text;
    int i;

    pf_jadd(call, pf_jstr(base ? base + 1 : argv[0]));
    for (i = 1; i < argc; i++)
        pf_jadd(call, pf_jstr(argv[i]));
    text = pf_jtext(call);
    pf_append_text(log, text);
    pf_append_text(log, "\n");
    free(text);
    json_free(call);
}

/* The value of the first argument that begins with `prefix`, or NULL. */
static const char *option(int argc, char **argv, const char *prefix)
{
    int i;

    for (i = 1; i < argc; i++)
        if (!strncmp(argv[i], prefix, strlen(prefix)))
            return argv[i] + strlen(prefix);
    return NULL;
}

/* How many arguments are `word`. */
static int occurrences(int argc, char **argv, const char *word)
{
    int i, n = 0;

    for (i = 1; i < argc; i++)
        n += !strcmp(argv[i], word);
    return n;
}

/* The producer's SSH: it reads the metadata and the digest of etude. */
static int standin_ssh(char *conf[4], char *command)
{
    static const char *const wsl[] = {"env",
                                      "MSYS_NO_PATHCONV=1",
                                      "wsl.exe",
                                      "--distribution",
                                      "Ubuntu-26.04",
                                      "--user",
                                      "testing",
                                      "--exec",
                                      NULL};
    static const char *const cat[] = {"/immutable/etude", "lock",     "tree",      "/producer",
                                      "prepare-import",   "/bin/cat", "/metadata", NULL};
    static const char *const sum[] = {"/usr/bin/sha256sum", "/immutable/etude", NULL};
    char **words, **rest, hex[65];
    size_t n, size;
    int status = 91;

    if (pf_split(command, &n, &words) == NULL)
        return 91;
    rest = words;
    if (n && !strcmp(words[0], "env")) {
        if (n < 8 || !words_are(words, 8, wsl)) {
            pf_words_free(words, n);
            return 1;
        }
        rest += 8;
    }
    if (words_are(rest, n - (size_t)(rest - words), cat)) {
        char *bytes = pf_read(conf[1], &size);
        status = bytes && fwrite(bytes, 1, size, stdout) == size ? 0 : 1;
        free(bytes);
    } else if (words_are(rest, n - (size_t)(rest - words), sum)) {
        status = pf_sha_file(conf[2], hex) ? 1 : 0;
        printf("%s  /immutable/etude\n", hex);
    }
    pf_words_free(words, n);
    return status;
}

/* The producer's rsync: it copies the listed files of the producer's tree. */
static int standin_rsync(char *conf[4], int argc, char **argv)
{
    static const char *const server[] = {"/immutable/etude", "lock",           "tree",
                                         "/producer",        "prepare-import", "/usr/bin/rsync"};
    const char *remote = option(argc, argv, "--rsync-path="),
               *listing = option(argc, argv, "--files-from=");
    char **words, *names, *line, *next;
    size_t n, i;
    int ok;

    if (occurrences(argc, argv, "--compress") != 1 || !occurrences(argc, argv, "--sparse") ||
        !occurrences(argc, argv, "--protect-args") || !remote || !listing || argc < 3 ||
        strcmp(argv[argc - 2], "fixture:/producer/") || pf_split(remote, &n, &words) == NULL)
        return 1;
    ok = n >= 6;
    for (i = 0; ok && i < 6; i++)
        ok = !strcmp(words[n - 6 + i], server[i]);
    pf_words_free(words, n);
    names = ok ? pf_read(listing, NULL) : NULL;
    for (line = names; ok && line && *line; line = next) {
        next = line + strcspn(line, "\n");
        if (*next)
            *next++ = 0;
        ok = !pf_copy(pf_at(conf[3], line), pf_at(argv[argc - 1], line));
    }
    free(names);
    return ok && names ? 0 : 1;
}

/* A Windows compiler: it drops the Win32 options, adds the tree's include
 * directory and the stand-in SDK, and gives each output the suffix .exe. */
static int standin_windows_cc(int argc, char **argv)
{
    const char **args = json_alloc((size_t)(argc + 8) * sizeof(*args));
    const char *output = NULL;
    size_t n = 0;
    int i, status;

    args[n++] = argv[3];
    for (i = 4; i < argc; i++) {
        if (!strcmp(argv[i], "-static") || !strcmp(argv[i], "-municode") ||
            !strcmp(argv[i], "-lws2_32") || !strcmp(argv[i], "-lbcrypt"))
            continue;
        if (!strcmp(argv[i], "-o") && i + 1 < argc && output == NULL)
            output = argv[i + 1];
        args[n++] = argv[i];
    }
    args[n++] = "-Iinclude";
    args[n++] = "-isystem";
    args[n++] = argv[2];
    args[n] = NULL;
    fflush(stdout);
    pid_t pid = fork();
    if (pid == 0) {
        execv(args[0], (char *const *)args);
        _exit(127);
    }
    free(args);
    if (pid < 0 || waitpid(pid, &status, 0) != pid || !WIFEXITED(status))
        return 1;
    status = WEXITSTATUS(status);
    if (status == 0 && output) {
        char renamed[PF_PATH];
        fix_format(renamed, sizeof(renamed), "%s.exe", output);
        if (rename(output, renamed))
            return 1;
    }
    return status;
}

/**
 * A compiler that watches a slot file, started as `--slot-cc PID SLOTS LOG
 * CC ARGS`: a call for its version sends SIGUSR1 to PID, and a call that
 * writes a file appends to LOG the process ID that holds the first slot of
 * SLOTS, or "free". Either then runs CC with ARGS.
 */
static int standin_slot_cc(int argc, char **argv)
{
    struct flock lock;
    char line[64];
    int fd;

    if (argc < 6)
        return 91;
    if (occurrences(argc, argv, "--version")) {
        if (kill((pid_t)atol(argv[2]), SIGUSR1))
            return 91;
    } else if (occurrences(argc, argv, "-o")) {
        memset(&lock, 0, sizeof(lock));
        lock.l_type = F_WRLCK;
        lock.l_whence = SEEK_SET;
        lock.l_len = 1;
        fd = open(argv[3], O_RDWR);
        if (fd < 0 || fcntl(fd, F_GETLK, &lock)) {
            if (fd >= 0)
                close(fd);
            return 91;
        }
        close(fd);
        if (lock.l_type == F_UNLCK)
            fix_format(line, sizeof(line), "free\n");
        else
            fix_format(line, sizeof(line), "%ld\n", (long)lock.l_pid);
        if (pf_append_text(argv[4], line))
            return 91;
    }
    execv(argv[5], argv + 5);
    return 127;
}

/**
 * Play the command this program was started as, when it was started as one
 * of the stand-ins, and give its exit status; -1 when it was not.
 */
int pf_standin(int argc, char **argv)
{
    const char *base = strrchr(argv[0], '/');
    char *conf[4] = {0}, *text, *line;
    char dir[PF_PATH];
    int i, status;

    base = base ? base + 1 : argv[0];
    if (argc >= 4 && !strcmp(argv[1], "--windows-cc"))
        return standin_windows_cc(argc, argv);
    if (argc >= 2 && !strcmp(argv[1], "--slot-cc"))
        return standin_slot_cc(argc, argv);
    if (!strcmp(base, "uname")) {
        if (argc == 2 && !strcmp(argv[1], "-s"))
            return puts("Linux") < 0;
        if (argc == 2 && !strcmp(argv[1], "-m"))
            return puts("x86_64") < 0;
        return 2;
    }
    if (strcmp(base, "ssh") && strcmp(base, "rsync"))
        return -1;
    if (base == argv[0])
        return 91;
    fix_format(dir, sizeof(dir), "%.*s/standin", (int)(base - 1 - argv[0]), argv[0]);
    text = pf_read(dir, NULL);
    for (line = text, i = 0; line && i < 4; i++) {
        conf[i] = line;
        line += strcspn(line, "\n");
        if (*line)
            *line++ = 0;
    }
    if (text == NULL || conf[3] == NULL) {
        free(text);
        return 91;
    }
    standin_log(conf[0], argc, argv);
    status =
        !strcmp(base, "ssh") ? standin_ssh(conf, argv[argc - 1]) : standin_rsync(conf, argc, argv);
    free(text);
    return status;
}

/**
 * Find the source tree, from ETUDE_TREE or the working directory, this
 * program, etude, from ETUDE_HOST_PREPARE or the tree's build, and the C
 * compiler; 0, or -1 with a line that says which is missing.
 */
int pf_start(const char *argv0)
{
    const char *tree = getenv("ETUDE_TREE"), *etude = getenv("ETUDE_HOST_PREPARE");
    char path[PF_PATH];

    if (realpath(tree && *tree ? tree : ".", pf_repo) == NULL || realpath(argv0, pf_self) == NULL) {
        printf("etude-prepare-test: fail: no source tree or no path of this program\n");
        return -1;
    }
    fix_format(path, sizeof(path), "%s/build/tools/etude/etude", pf_repo);
    if (realpath(etude && *etude ? etude : path, pf_etude) == NULL || pf_which("cc", pf_cc)) {
        printf("etude-prepare-test: fail: no etude at %s, or no cc on PATH\n", path);
        return -1;
    }
    return 0;
}

/* ---- Host preparation. ---- */

/** Make the smallest tree that host preparation reads, at `tree`. */
void pf_native_tree(const char *tree)
{
    static const char *const dirs[] = {"mk", "tools/etude", "tests/host", "tests/cases/host"};
    size_t i;
    int bad = 0;

    for (i = 0; i < sizeof(dirs) / sizeof(*dirs); i++)
        bad |= pf_mkdirs(pf_at(tree, dirs[i]));
    bad |= pf_copy(pf_at(pf_repo, "mk/host-check.mk"), pf_at(tree, "mk/host-check.mk"));
    bad |= pf_write_text(pf_at(tree, "tools/etude/sources.mk"), "");
    bad |= pf_write_text(pf_at(tree, "tests/cases/host.host.json"), "{}\n");
    unlink(pf_at(tree, "cc"));
    bad |= symlink(pf_cc, pf_at(tree, "cc"));
    PF_CHECK(!bad, "native tree: made");
}

/** Write the host test `name` of `tree`, with the metadata lines `metadata`
 * in its first comment and the program `body`, which returns 0 when NULL. */
void pf_native_source(const char *tree, const char *name, const char *metadata, const char *body)
{
    struct jbuf text = {0};
    const char *line;

    jbuf_puts(&text, "/* ");
    for (line = metadata ? metadata : ""; *line; line++)
        jbuf_puts(&text, *line == '\n' ? "\n * " : (char[]){*line, 0});
    jbuf_puts(&text, "*/\n");
    jbuf_puts(&text, body ? body : "int main(void) { return 0; }\n");
    PF_CHECK(!pf_write_text(pf_at(pf_at(tree, "tests/host"), name), text.data),
             "native tree: source written");
    jbuf_free(&text);
}

/**
 * Give `tree` the Windows companion programs of etude's platform check, whose
 * target includes a header of the tree, one of the include directory the
 * stand-in compiler adds and one of a stand-in SDK beside the tree.
 */
void pf_windows_tree(const char *tree)
{
    static const char *const programs[] = {"winstand.c", "fixture.c", "main.c", "image_command.c"};
    static const char *const declarations[] = {"hold.c", "winfmt.c", "files.c"};
    char sdk[PF_PATH], win[PF_PATH], *slash;
    size_t i;
    int bad = 0;

    fix_format(win, sizeof(win), "%s/tools/etude/win", tree);
    fix_format(sdk, sizeof(sdk), "%s", tree);
    slash = strrchr(sdk, '/');
    fix_format(slash, sizeof(sdk) - (size_t)(slash - sdk), "/sdk");
    bad |= pf_write_text(pf_at(sdk, "external.h"), "#define EXTERNAL 0\n");
    bad |= pf_write_text(pf_at(tree, "include/shadow.h"), "#define SHADOW 0\n");
    bad |= pf_mkdirs(pf_at(tree, "tools/etude/volume"));
    bad |= pf_write_text(pf_at(tree, "tools/etude/schemas/history.schema.json"), "{}\n");
    bad |= pf_write_text(pf_at(win, "value.h"), "#define VALUE 0\n");
    bad |= pf_write_text(pf_at(win, "wintarget.c"),
                         "#include \"value.h\"\n#include <shadow.h>\n#include <external.h>\n"
                         "int main(void) { return VALUE + SHADOW + EXTERNAL; }\n");
    for (i = 0; i < sizeof(programs) / sizeof(*programs); i++)
        bad |= pf_write_text(pf_at(win, programs[i]), "int main(void) { return 0; }\n");
    for (i = 0; i < sizeof(declarations) / sizeof(*declarations); i++)
        bad |= pf_write_text(pf_at(win, declarations[i]), "typedef int fixture_declaration;\n");
    bad |= pf_write_text(pf_at(tree, "tools/etude/volume/volume.c"),
                         "typedef int fixture_declaration;\n");
    bad |= pf_write_text(pf_at(tree, "tools/etude/volume/etude_volume.c"),
                         "int main(void) { return 0; }\n");
    bad |= pf_write_text(pf_at(tree, "tools/etude/sources.mk"),
                         "ETUDE_WIN_FIXTURE_SRC = tools/etude/win/fixture.c\n"
                         "ETUDE_WIN_EXEC_SRC = tools/etude/win/files.c\n"
                         "ETUDE_WIN_FILE_SRC = tools/etude/win/files.c\n"
                         "ETUDE_WIN_SRC = tools/etude/win/main.c\n"
                         "ETUDE_VOLUME_SRC = tools/etude/volume/etude_volume.c "
                         "tools/etude/volume/volume.c\n");
    PF_CHECK(!bad, "windows tree: made");
}

/* The sorted names in `dir` that begin with `prefix`, one a line. */
static void names_with(const char *dir, const char *prefix, struct jbuf *out)
{
    struct dirent **list = NULL;
    int n = scandir(dir, &list, NULL, alphasort), i;

    jbuf_puts(out, "");
    for (i = 0; i < n; i++) {
        if (!strncmp(list[i]->d_name, prefix, strlen(prefix))) {
            jbuf_puts(out, list[i]->d_name);
            jbuf_puts(out, "\n");
        }
        free(list[i]);
    }
    free(list);
}

/**
 * Run `etude prepare -H ARGS GROUP` in `tree`, with the C compiler at
 * `tree/cc`, no inherited make settings and no platform but what `env` sets;
 * a Windows platform has the stand-in compiler. Check its status against
 * `expected`, and that a preparation that passed left no work directory.
 * Give its output in `out`, and return its inventory, which the caller
 * frees; NULL when it failed or only printed commands.
 */
struct json *pf_native_prepare(const char *tree, const char *group, const char *const args[],
                               const char *const env[], int expected, struct jbuf *out)
{
    const char *argv[32], *environment[32];
    char output[PF_PATH], scratch[PF_PATH], prefix[64], cc[PF_PATH], windows_cc[3 * PF_PATH];
    char sdk[PF_PATH], *slash;
    struct jbuf before = {0}, after = {0}, text = {0};
    struct json *record;
    size_t n = 0, e = 0, i;
    int status, printing = 0, windows = 0;

    fix_format(output, sizeof(output), "build/tests/host-suite/%s.inventory.json", group);
    fix_format(scratch, sizeof(scratch), "%s/build/tests/host-suite", tree);
    fix_format(prefix, sizeof(prefix), ".%s-prepare.", group);
    fix_format(cc, sizeof(cc), "CC=%s/cc", tree);
    argv[n++] = pf_etude;
    argv[n++] = "prepare";
    argv[n++] = "-H";
    for (i = 0; args && args[i]; i++) {
        printing |= !strcmp(args[i], "--print-commands");
        argv[n++] = args[i];
    }
    argv[n++] = group;
    argv[n++] = output;
    argv[n] = NULL;
    environment[e++] = cc;
    environment[e++] = "MAKEFLAGS=";
    environment[e++] = "MFLAGS=";
    environment[e++] = "MAKELEVEL=";
    environment[e++] = "HOST_TEST_PLATFORM";
    for (i = 0; env && env[i]; i++) {
        if (!strncmp(env[i], "HOST_TEST_PLATFORM", 18))
            windows = !strcmp(env[i], "HOST_TEST_PLATFORM=windows");
        environment[e++] = env[i];
    }
    if (windows) {
        fix_format(sdk, sizeof(sdk), "%s", tree);
        slash = strrchr(sdk, '/');
        *slash = 0;
        fix_format(windows_cc, sizeof(windows_cc), "CC=%s --windows-cc %s/sdk %s", pf_self, sdk,
                   pf_cc);
        environment[e++] = windows_cc;
        environment[e++] = "OS=Windows_NT";
    }
    environment[e] = NULL;
    names_with(scratch, prefix, &before);
    status = pf_run(tree, environment, argv, out ? out : &text);
    PF_CHECK(status == expected, "host preparation: its status");
    if (status != expected)
        printf("etude-prepare-test: %s in %s gave %d:\n%s", group, tree, status,
               out ? out->data : text.data);
    if (status == 0 && expected == 0) {
        names_with(scratch, prefix, &after);
        PF_CHECK(!strcmp(before.data, after.data), "host preparation: no work left");
    }
    record = status == 0 && expected == 0 && !printing ? pf_jload(pf_at(tree, output)) : NULL;
    jbuf_free(&before);
    jbuf_free(&after);
    jbuf_free(&text);
    return record;
}

/* ---- Import. ---- */

/* Run git in `tree` with `words`, a NULL-ended list, taking its output into
 * `out` when given; its status. */
static int git(const char *tree, const char *const words[], struct jbuf *out)
{
    const char *argv[16] = {"git", "-C", tree};
    size_t n = 3;

    while (*words && n < 15)
        argv[n++] = *words++;
    argv[n] = NULL;
    return pf_run(NULL, NULL, argv, out);
}

/** Commit the native tree as it is and give the producer's metadata the
 * commit and the tree's source snapshot. */
void pf_import_source(struct pf_import *s)
{
    static const char *const add[] = {"add", ".", NULL};
    static const char *const commit[] = {"commit", "-qm", "Fixture source", "--allow-empty", NULL};
    static const char *const head[] = {"rev-parse", "HEAD", NULL};
    struct jbuf out = {0};
    struct json *snapshot, *source;
    const struct json *inputs;
    char path[PF_PATH], *slash;
    size_t i;
    int bad = git(s->tree, add, NULL) || git(s->tree, commit, NULL) || git(s->tree, head, &out) ||
              out.len < 40;

    PF_CHECK(!bad, "import fixture: the source committed");
    fix_format(s->commit, sizeof(s->commit), "%.40s", bad ? "" : out.data);
    jbuf_free(&out);
    fix_format(path, sizeof(path), "%s", s->tree);
    slash = strrchr(path, '/');
    fix_format(slash, sizeof(path) - (size_t)(slash - path), "/source-%s.json", s->commit);
    const char *argv[] = {pf_etude,   "prepare", "source",   "--tree", s->tree,
                          "--commit", s->commit, "--output", path,     NULL};
    PF_CHECK(pf_run(NULL, NULL, argv, NULL) == 0, "import fixture: the source snapshot");
    snapshot = pf_jload(path);
    inputs = json_member(snapshot, "inputs");
    source = pf_jobj();
    for (i = 0; inputs && i < inputs->count; i++) {
        struct json *row = pf_jclone(inputs->item[i]);
        const char *name = json_text(row, "path");
        if (name) {
            char *key = copy_text(name);
            pf_jdel(row, "path");
            pf_jset(source, key, row);
            free(key);
        } else {
            json_free(row);
        }
    }
    json_free(snapshot);
    pf_jset(s->metadata, "commit", pf_jstr(s->commit));
    pf_jset(s->metadata, "source", source);
    pf_jset(pf_jget(s->metadata, "receipt"), "commit", pf_jstr(s->commit));
}

/* The nested inputs that `checks` names inside the producer's tree, each
 * written and described in `nested`. */
static void nested_inputs(struct pf_import *s, const struct json *checks, struct json *nested)
{
    size_t i, j;

    for (i = 0; checks && i < checks->count; i++) {
        const struct json *inputs = json_member(checks->item[i], "inputs");
        for (j = 0; inputs && j < inputs->count; j++) {
            const char *hex = json_text(inputs->item[j], "path_hex");
            char relative[PF_PATH] = "";
            const char *path;
            size_t k;
            if (hex) {
                for (k = 0; hex[k] && hex[k + 1] && k / 2 < sizeof(relative) - 1; k += 2) {
                    unsigned byte;
                    sscanf(hex + k, "%2x", &byte);
                    relative[k / 2] = (char)byte;
                    relative[k / 2 + 1] = 0;
                }
            } else {
                fix_format(relative, sizeof(relative), "%s", json_text(inputs->item[j], "path"));
            }
            if (relative[0] == '/' || !strncmp(relative, "../", 3) || strstr(relative, "/.."))
                continue;
            path = pf_at(s->producer, relative);
            PF_CHECK(!pf_write_text(path, "nested fixture bytes\n"), "import fixture: nested");
            pf_jset(nested, relative, pf_describe(path));
        }
    }
}

/* Write the native tree's source, its manifest, which prepares each goal of
 * `s` for both architectures, and its Makefile. */
static void import_tree(struct pf_import *s)
{
    struct json *manifest = pf_jobj(), *cases = pf_jarr(), *row = pf_jobj(), *prepare = pf_jarr();
    struct json *arches = pf_jarr();
    size_t i;

    PF_CHECK(!pf_write_text(pf_at(s->tree, ".gitignore"), "build/\nvendor/\n") &&
                 !pf_write_text(pf_at(s->tree, "source.c"),
                                "#include \"value.h\"\nint main(void) { return VALUE; }\n") &&
                 !pf_write_text(pf_at(s->tree, "value.h"), "#define VALUE 0\n"),
             "import fixture: source");
    for (i = 0; i < s->ngoals; i++) {
        struct json *artifact = pf_jobj();
        const char *node = s->goals[i];
        int per = !strncmp(node, "build/x86_64/", 13);
        node += per ? 13 : !strncmp(node, "build/", 6) ? 6 : 0;
        pf_jset(artifact, "node", pf_jstr(node));
        pf_jset(artifact, "per_architecture", pf_jbool(per));
        pf_jadd(prepare, artifact);
    }
    pf_jadd(arches, pf_jstr("i686"));
    pf_jadd(arches, pf_jstr("x86_64"));
    pf_jset(row, "architectures", arches);
    pf_jset(row, "prepare", prepare);
    pf_jadd(cases, row);
    pf_jset(manifest, "cases", cases);
    PF_CHECK(!pf_jsave(pf_at(s->tree, "tests/cases/host.host.json"), manifest),
             "import fixture: manifest");
    json_free(manifest);
    PF_CHECK(!pf_write_text(pf_at(s->tree, "Makefile"),
                            "build/tests/cases/machine/a: source.c value.h build/images/root.disk\n"
                            "\t@mkdir -p $(@D)\n\t@printf \"compile\\n\" >> build/compile.calls\n"
                            "\t$(CC) -fsanitize=address,undefined -fno-sanitize-recover=all -g -O1 "
                            "-o $@ $<\n"
                            ".PHONY: build/images/root.disk\nbuild/images/root.disk:\n"
                            "\t@mkdir -p $(@D)\n\t@printf changed > $@\n"),
             "import fixture: Makefile");
}

/*
 * Make the producer's inventory `goal`, which lists `checks` under the native
 * tree's manifest and compiler; give its text to `texts`, and the native tree
 * a copy and a rule that makes the goal from it.
 */
static void import_inventory(struct pf_import *s, const char *goal, const struct json *checks,
                             struct json *texts)
{
    struct json *inventory = pf_jobj(), *cc = pf_jobj();
    char hex[65], fixture[64], rule[2 * PF_PATH], *text;

    pf_sha_file(pf_at(s->tree, "tests/cases/host.host.json"), hex);
    pf_jset(inventory, "schema_version", pf_jint(1));
    pf_jset(inventory, "manifest_sha256", pf_jstr(hex));
    pf_sha_file(pf_cc, hex);
    pf_jset(cc, "executable", pf_jstr(pf_cc));
    pf_jset(cc, "sha256", pf_jstr(hex));
    pf_jset(inventory, "compiler", cc);
    pf_jset(inventory, "checks", checks ? pf_jclone(checks) : pf_jarr());
    text = pf_jtext(inventory);
    json_free(inventory);
    PF_CHECK(!pf_write_text(pf_at(s->producer, goal), text), "import fixture: inventory");
    pf_jset(texts, goal, pf_jstr(text));
    fix_format(fixture, sizeof(fixture), "inventory-%zu.json", texts->count);
    PF_CHECK(!pf_write_text(pf_at(s->tree, fixture), text), "import fixture: its copy");
    fix_format(rule, sizeof(rule), "%s: %s\n\t@mkdir -p $(@D)\n\tcp $< $@\n", goal, fixture);
    PF_CHECK(!pf_append_text(pf_at(s->tree, "Makefile"), rule), "import fixture: its rule");
    free(text);
}

/* A clock reading of a receipt: `utc`, at `monotonic_us`, to 1 us. */
static struct json *reading(const char *utc, long long monotonic_us)
{
    struct json *clock = pf_jobj();

    pf_jset(clock, "utc", pf_jstr(utc));
    pf_jset(clock, "monotonic_us", pf_jint(monotonic_us));
    pf_jset(clock, "resolution_us", pf_jint(1));
    return clock;
}

/* Give the producer the record of its files and a receipt of a passing,
 * cleaned, ordinary production of that record. */
static void import_receipt(struct pf_import *s)
{
    struct json *receipt = pf_jget(s->metadata, "receipt"), *conditioning = pf_jobj();
    struct jbuf record = {0};
    char hex[65];
    size_t i;

    jbuf_puts(&record, "etude-prepare 1\ntree /fixture-producer\n");
    for (i = 0; i < s->ngoals; i++) {
        const struct json *file = json_member(s->files, s->goals[i]);
        char line[2 * PF_PATH];
        fix_format(line, sizeof(line), "file %s %lld 1 0 %s\n", json_text(file, "sha256"),
                   json_member(file, "size_bytes")->integer, s->goals[i]);
        jbuf_puts(&record, line);
    }
    s->record = record.data;
    pf_jset(s->metadata, "record", pf_jstr(s->record));
    pf_jset(receipt, "format", pf_jstr("etude-preparation-receipt"));
    pf_jset(receipt, "schema_version", pf_jint(1));
    pf_jset(receipt, "outcome", pf_jstr("pass"));
    pf_jset(receipt, "cleanup", pf_jstr("confirmed"));
    pf_jset(receipt, "run_id", pf_jstr("fixture-ordinary-production"));
    pf_jset(receipt, "machine_id", pf_jstr("fixture-machine"));
    pf_jset(receipt, "boot_id", pf_jstr("fixture-boot"));
    pf_sha_bytes(s->record, strlen(s->record), hex);
    pf_jset(receipt, "record_sha256", pf_jstr(hex));
    pf_jset(receipt, "start", reading("2026-10-24T00:00:00Z", 1));
    pf_jset(receipt, "end", reading("2026-10-24T00:00:01Z", 5));
    pf_jset(conditioning, "inhouse", pf_jstr("clean"));
    pf_jset(conditioning, "backend", pf_jstr("ordinary"));
    pf_jset(conditioning, "provenance", pf_jnull());
    pf_jset(receipt, "conditioning", conditioning);
}

/**
 * Make, under `work`, a producer whose files are `goals` and a native tree
 * that builds `build/tests/cases/machine/a` from its own source and imports
 * the rest. A goal that is an inventory lists `checks`, and the producer has
 * the nested inputs they name. The tree is committed, and the producer's
 * metadata gives its record, a passing ordinary receipt and its validator.
 */
void pf_import_fixture(struct pf_import *s, const char *work, const char *const goals[],
                       size_t ngoals, const struct json *checks)
{
    static const char *const init[] = {"init", "-q", NULL};
    static const char *const name[] = {"config", "user.name", "Preparation fixture", NULL};
    static const char *const email[] = {"config", "user.email", "fixture@example.invalid", NULL};
    struct json *nested = pf_jobj(), *texts = pf_jobj(), *validator = pf_describe(pf_etude);
    size_t i;

    memset(s, 0, sizeof(*s));
    fix_format(s->tree, sizeof(s->tree), "%s/native", work);
    fix_format(s->producer, sizeof(s->producer), "%s/producer", work);
    PF_CHECK(!pf_mkdirs(s->producer) && !pf_mkdirs(pf_at(s->tree, "tests/cases")),
             "import fixture: directories");
    s->goals = json_alloc((ngoals ? ngoals : 1) * sizeof(*s->goals));
    for (i = 0; i < ngoals; i++)
        s->goals[i] = copy_text(goals[i]);
    s->ngoals = ngoals;
    s->files = pf_jobj();
    import_tree(s);
    for (i = 0; i < ngoals; i++) {
        size_t n = strlen(goals[i]);
        if (n >= 15 && !strcmp(goals[i] + n - 15, ".inventory.json")) {
            import_inventory(s, goals[i], checks, texts);
            nested_inputs(s, checks, nested);
        } else {
            PF_CHECK(!pf_write_text(pf_at(s->producer, goals[i]), "producer bytes\n"),
                     "import fixture: producer file");
        }
        pf_jset(s->files, goals[i], pf_describe(pf_at(s->producer, goals[i])));
    }
    PF_CHECK(!git(s->tree, init, NULL) && !git(s->tree, name, NULL) && !git(s->tree, email, NULL),
             "import fixture: git");
    s->metadata = pf_jobj();
    pf_jset(s->metadata, "receipt", pf_jobj());
    pf_import_source(s);
    import_receipt(s);
    pf_jset(validator, "path", pf_jstr(pf_etude));
    pf_jset(s->metadata, "format", pf_jstr("etude-preparation-producer"));
    pf_jset(s->metadata, "schema_version", pf_jint(1));
    pf_jset(s->metadata, "validator", validator);
    pf_jset(s->metadata, "auxiliary", pf_jobj());
    pf_jset(s->metadata, "nested", nested);
    pf_jset(s->metadata, "inventories", texts);
}

/**
 * Give the inventory `goal` of the producer the text `text`, and its record
 * and receipt the inventory's new digest and size.
 */
void pf_rerecord(struct pf_import *s, const char *goal, const char *text)
{
    const struct json *file = json_member(s->files, goal);
    char old[128], new[128], hex[65];
    struct jbuf record = {0};
    const char *at = s->record, *found;

    pf_jset(pf_jget(s->metadata, "inventories"), goal, pf_jstr(text));
    fix_format(old, sizeof(old), "%s %lld", json_text(file, "sha256"),
               json_member(file, "size_bytes")->integer);
    pf_sha_bytes(text, strlen(text), hex);
    fix_format(new, sizeof(new), "%s %zu", hex, strlen(text));
    while ((found = strstr(at, old)) != NULL) {
        jbuf_put(&record, at, (size_t)(found - at));
        jbuf_puts(&record, new);
        at = found + strlen(old);
    }
    jbuf_puts(&record, at);
    pf_jset(s->metadata, "record", pf_jstr(record.data));
    pf_sha_bytes(record.data, record.len, hex);
    pf_jset(pf_jget(s->metadata, "receipt"), "record_sha256", pf_jstr(hex));
    jbuf_free(&record);
}

/**
 * Import the request of `s` into its native tree with `etude prepare
 * import`, from the local producer, or the producer that `producer`'s members
 * change, under the conditioning, for the architectures and with the request
 * line end that `s` gives, and with the environment changed by `env`.
 * Check its status against `expected`, and that it wrote its provenance only
 * when it passed; give that provenance, or NULL.
 */
struct json *pf_import_run(struct pf_import *s, int expected, const struct json *producer,
                           const char *const env[])
{
    char output[PF_PATH], number[32], hex[65], cc[PF_PATH + 8];
    const char *metadata = pf_at(s->producer, "producer.json"), *work, *config, *request,
               *provenance;
    struct json *document = pf_jobj(), *from = pf_jobj(), *proof = NULL;
    struct jbuf text = {0}, out = {0};
    const char *environment[16];
    size_t i, e = 0;
    int status;

    fix_format(number, sizeof(number), "build/import-proof/%d", ++s->count);
    fix_format(output, sizeof(output), "%s", pf_at(s->tree, number));
    work = pf_at(output, "work");
    config = pf_at(output, "config.json");
    request = pf_at(output, "request");
    provenance = pf_at(output, "provenance.json");
    PF_CHECK(!pf_mkdirs(work) && !pf_jsave(metadata, s->metadata), "import: its files");
    pf_sha_file(metadata, hex);
    pf_jset(from, "transport", pf_jstr("local"));
    pf_jset(from, "tree", pf_jstr(s->producer));
    pf_jset(from, "metadata", pf_jstr(metadata));
    pf_jset(from, "sha256", pf_jstr(hex));
    for (i = 0; producer && i < producer->count; i++)
        pf_jset(from, producer->item[i]->name, pf_jclone(producer->item[i]));
    pf_jset(document, "format", pf_jstr("etude-preparation-import"));
    pf_jset(document, "schema_version", pf_jint(1));
    pf_jset(document, "commit", pf_jstr(s->commit));
    pf_jset(document, "producer", from);
    PF_CHECK(!pf_jsave(config, document), "import: its configuration");
    json_free(document);
    for (i = 0; i < s->ngoals; i++) {
        jbuf_puts(&text, s->goals[i]);
        jbuf_puts(&text, s->newline ? s->newline : "\n");
    }
    PF_CHECK(!pf_write_text(request, text.data ? text.data : ""), "import: its request");
    jbuf_free(&text);
    fix_format(cc, sizeof(cc), "CC=%s", pf_cc);
    environment[e++] = cc;
    environment[e++] = "MAKEFLAGS=";
    environment[e++] = "MFLAGS=";
    environment[e++] = "MAKELEVEL=";
    for (i = 0; env && env[i] && e < 15; i++)
        environment[e++] = env[i];
    environment[e] = NULL;
    const char *argv[] = {pf_etude,
                          "prepare",
                          "import",
                          "--tree",
                          s->tree,
                          "--config",
                          config,
                          "--request",
                          request,
                          "--work",
                          work,
                          "--provenance",
                          provenance,
                          "--commit",
                          s->commit,
                          "--arches",
                          s->arches ? s->arches : "i686",
                          "--conditioning",
                          s->conditioning ? s->conditioning : "retained",
                          NULL};
    status = pf_run(NULL, environment, argv, &out);
    PF_CHECK(status == expected, "import: its status");
    PF_CHECK(pf_exists(provenance) == (expected == 0), "import: provenance only on a pass");
    if (status != expected)
        printf("etude-prepare-test: import %d gave %d, not %d:\n%s", s->count, status, expected,
               out.data);
    jbuf_free(&out);
    if (expected == 0 && status == 0)
        proof = pf_jload(provenance);
    return proof;
}

/** Free what `s` holds. */
void pf_import_free(struct pf_import *s)
{
    size_t i;

    for (i = 0; i < s->ngoals; i++)
        free(s->goals[i]);
    free(s->goals);
    json_free(s->files);
    json_free(s->metadata);
    free(s->record);
    memset(s, 0, sizeof(*s));
}
