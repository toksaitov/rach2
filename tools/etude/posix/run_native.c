/*
 * run_native.c: `etude run native`, which runs one case on a Rach2 machine
 * that is already up, such as the Lenovo, through `etude-guest --stdio`
 * started by a transport command such as ssh(1), and prints the case's output
 * and its verdict.
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "../../../utils/sha256.h"
#include "../json.h"
#include "etude/remote.h"
#include "etude/native.h"

#define FILES_MAX    16
#define ENV_MAX      32
#define FIXTURES_MAX 16

/* The exit statuses: one for each verdict, and one for a wrong command. */
#define EXIT_PASS       0
#define EXIT_FAILURE    1
#define EXIT_USAGE      2
#define EXIT_CANCELLED  3
#define EXIT_INCOMPLETE 4

/* A file of the case, read whole. */
struct file {
    const char *name; /* its last component, its name in the workspace */
    char *data;
    size_t size;
    const char *mode;
    char sha[65];
};

/* What the command line asks for. */
struct options {
    const char *run, *attempt, *case_id, *journal, *transport, *evidence;
    const char *path[FILES_MAX];
    int npaths;
    const char *env[ENV_MAX];
    int nenv;
    const char *unset[ENV_MAX]; /* the variables left out */
    int nunset;
    const char *fixture[FIXTURES_MAX]; /* each `KIND VALUE...` beside the workspace */
    int nfixtures;
    const char *input; /* the name of the file that is the case's input, or NULL */
    uint64_t deadline_ms, grace_ms, output_max, cancel_ms, stage_ms;
    unsigned reconnects;
    int keep;
    char **argv; /* the program and its arguments */
    char *const *transport_argv;
    int staged_only;
    enum etude_end expected_end;
    int expected_value;
};

/* A string that grows, for the invocation. */
struct text {
    char *p;
    size_t len, cap;
    int bad; /* out of memory */
};

/** Print the usage and return EXIT_USAGE. */
static int usage(void)
{
    fputs("usage: etude run native -R RUN -a ATTEMPT -c CASE -j JOURNAL -t TRANSPORT -f FILE...\n"
          "                    [-d MS] [-g MS] [-o BYTES] [-x MS] [-s MS] [-r N]\n"
          "                    [-e EVIDENCE] [-E NAME=VALUE]... [-U NAME]... [-F FIXTURE]...\n"
          "                    [-i INPUT] [-k] -- PROGRAM [ARGUMENT...]\n",
          stderr);
    return EXIT_USAGE;
}

/** Append `n` bytes to `t`. */
static void put(struct text *t, const char *s, size_t n)
{
    char *p;

    if (t->bad)
        return;
    if (t->len + n + 1 > t->cap) {
        size_t cap = t->cap == 0 ? 1024 : t->cap;
        while (cap < t->len + n + 1)
            cap *= 2;
        p = realloc(t->p, cap);
        if (p == NULL) {
            t->bad = 1;
            return;
        }
        t->p = p;
        t->cap = cap;
    }
    memcpy(t->p + t->len, s, n);
    t->len += n;
    t->p[t->len] = '\0';
}

/** Append the string `s` to `t`. */
static void put_text(struct text *t, const char *s)
{
    put(t, s, strlen(s));
}

/** Append formatted text to `t`. */
static void putf(struct text *t, const char *fmt, ...)
{
    char buf[256];
    va_list ap;
    int n;

    va_start(ap, fmt);
    n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= sizeof(buf))
        t->bad = 1;
    else
        put(t, buf, (size_t)n);
}

/** Append `s` as a JSON string, its quotes, backslashes and control
 * characters escaped. */
static void put_string(struct text *t, const char *s)
{
    char esc[8];

    put_text(t, "\"");
    for (; *s; s++) {
        if (*s == '"' || *s == '\\') {
            esc[0] = '\\';
            esc[1] = *s;
            put(t, esc, 2);
        } else if ((unsigned char)*s < 32) {
            snprintf(esc, sizeof(esc), "\\u%04x", (unsigned)(unsigned char)*s);
            put(t, esc, 6);
        } else {
            put(t, s, 1);
        }
    }
    put_text(t, "\"");
}

/** Say whether `s` is ASCII, which an invocation's strings must be. */
static int ascii(const char *s)
{
    for (; *s; s++)
        if ((unsigned char)*s > 127)
            return 0;
    return 1;
}

/** Say whether `s` is a file name that etude-guest stores: a letter or digit
 * and then letters, digits and `._+-`, at most 128 of them. */
static int good_name(const char *s)
{
    size_t n = strlen(s);

    return n > 0 && n <= 128 && strchr("._+-", s[0]) == NULL &&
           strspn(s, "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789._+-") == n;
}

/** Read a number of the command line into `*v`; 0, or -1 when it is not a
 * positive decimal number. */
static int number(const char *s, uint64_t *v)
{
    char *end;
    unsigned long long n;

    if (s == NULL || *s < '0' || *s > '9')
        return -1;
    n = strtoull(s, &end, 10);
    if (*end != '\0' || n == 0)
        return -1;
    *v = (uint64_t)n;
    return 0;
}

/** The member of `o` that the numeric option `c` sets, or NULL when `c` is
 * not one. */
static uint64_t *number_option(struct options *o, int c)
{
    switch (c) {
    case 'd':
        return &o->deadline_ms;
    case 'g':
        return &o->grace_ms;
    case 'o':
        return &o->output_max;
    case 'x':
        return &o->cancel_ms;
    case 's':
        return &o->stage_ms;
    default:
        return NULL;
    }
}

/** Set the option `c` to the value `v`; 0, or -1 when `c` is no option or
 * `v` is not a value it takes. */
static int option(struct options *o, int c, const char *v)
{
    uint64_t n, *num = number_option(o, c);

    if (!v)
        return -1;
    if (num != NULL)
        return number(v, num);
    switch (c) {
    case 'R':
        o->run = v;
        break;
    case 'a':
        o->attempt = v;
        break;
    case 'c':
        o->case_id = v;
        break;
    case 'j':
        o->journal = v;
        break;
    case 't':
        o->transport = v;
        break;
    case 'e':
        o->evidence = v;
        break;
    case 'f':
        if (o->npaths == FILES_MAX)
            return -1;
        o->path[o->npaths++] = v;
        break;
    case 'E':
        if (o->nenv == ENV_MAX || strchr(v, '=') == NULL || v[0] == '=')
            return -1;
        o->env[o->nenv++] = v;
        break;
    case 'U':
        if (o->nunset == ENV_MAX || v[0] == '\0' || strchr(v, '=') != NULL)
            return -1;
        o->unset[o->nunset++] = v;
        break;
    case 'F':
        if (o->nfixtures == FIXTURES_MAX || v[0] == '\0' || v[0] == ' ')
            return -1;
        o->fixture[o->nfixtures++] = v;
        break;
    case 'i':
        o->input = v;
        break;
    case 'r':
        if (number(v, &n) != 0 || n > 100)
            return -1;
        o->reconnects = (unsigned)n;
        break;
    default:
        return -1;
    }
    return 0;
}

/** Read the options; 0, or -1 for a command line that is not valid. */
static int parse(int argc, char **argv, struct options *o)
{
    int i;

    memset(o, 0, sizeof(*o));
    o->deadline_ms = 600000;
    o->grace_ms = 5000;
    o->output_max = 1024 * 1024;
    o->stage_ms = 30000;
    o->reconnects = 3;
    for (i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (strcmp(a, "--") == 0) {
            i++;
            break;
        }
        if (a[0] != '-')
            break;
        if (strcmp(a, "-k") == 0) {
            o->keep = 1;
            continue;
        }
        if (a[1] == '\0' || a[2] != '\0' || i + 1 == argc || option(o, a[1], argv[i + 1]) != 0)
            return -1;
        i++;
    }
    if (i >= argc || o->run == NULL || o->attempt == NULL || o->case_id == NULL ||
        o->journal == NULL || o->transport == NULL || o->npaths == 0)
        return -1;
    o->argv = argv + i;
    return 0;
}

/** Read the file at `path` whole into `f`; 0, or -1 with a diagnostic. */
static int read_case_file(const char *path, struct file *f, int staged_only)
{
    struct stat st;
    const char *slash = strrchr(path, '/');
    FILE *in;

    f->name = slash != NULL ? slash + 1 : path;
    if (!good_name(f->name)) {
        fprintf(stderr, "etude run: %s: not a name a workspace may have\n", path);
        return -1;
    }
    in = fopen(path, "rb");
    if (in == NULL || fstat(fileno(in), &st) != 0 || !S_ISREG(st.st_mode) ||
        (uint64_t)st.st_size > ETUDE_REMOTE_FILE_MAX) {
        fprintf(stderr, "etude run: %s: not a regular file that can be read\n", path);
        if (in != NULL)
            fclose(in);
        return -1;
    }
    f->size = (size_t)st.st_size;
    if (staged_only) {
        struct sha256 hash;
        unsigned char data[65536], digest[32];
        size_t n, total = 0;
        sha256_init(&hash);
        while ((n = fread(data, 1, sizeof(data), in))) {
            sha256_update(&hash, data, n);
            total += n;
        }
        if (ferror(in) || total != f->size) {
            fclose(in);
            return -1;
        }
        sha256_final(&hash, digest);
        sha256_hex(digest, f->sha);
    } else {
        struct sha256 hash;
        unsigned char digest[32];
        f->data = malloc(f->size + 1);
        if (f->data == NULL || fread(f->data, 1, f->size, in) != f->size) {
            fprintf(stderr, "etude run: %s: cannot be read\n", path);
            fclose(in);
            return -1;
        }
        sha256_init(&hash);
        sha256_update(&hash, (const unsigned char *)f->data, f->size);
        sha256_final(&hash, digest);
        sha256_hex(digest, f->sha);
    }
    fclose(in);
    f->mode = (st.st_mode & 0111) != 0 ? "0755" : "0644";
    return 0;
}

/** Give the first ETUDE_FRAME_TOKEN_LEN digits of the SHA-256 of `s`. */
static void token_of(const char *s, char out[ETUDE_FRAME_TOKEN_LEN + 1])
{
    struct sha256 h;
    unsigned char d[32];
    char hex[65];

    sha256_init(&h);
    sha256_update(&h, (const unsigned char *)s, strlen(s));
    sha256_final(&h, d);
    sha256_hex(d, hex);
    memcpy(out, hex, ETUDE_FRAME_TOKEN_LEN);
    out[ETUDE_FRAME_TOKEN_LEN] = '\0';
}

/** Say whether the variable that `env`, NAME=VALUE, sets is one that -U
 * leaves out. */
static int left_out(const struct options *o, const char *env, size_t len)
{
    int i;

    for (i = 0; i < o->nunset; i++)
        if (strlen(o->unset[i]) == len && strncmp(o->unset[i], env, len) == 0)
            return 1;
    return 0;
}

/** Append the fixture `spec`, `KIND VALUE...` separated by spaces, to `t` as
 * an object of the invocation's fixtures. */
static void put_fixture(struct text *t, const char *spec)
{
    char word[256];
    size_t n;
    int i;

    for (i = 0;; i++) {
        spec += strspn(spec, " ");
        n = strcspn(spec, " ");
        if (n == 0)
            break;
        if (n >= sizeof(word)) {
            t->bad = 1;
            return;
        }
        memcpy(word, spec, n);
        word[n] = '\0';
        spec += n;
        put_text(t, i == 0 ? ",{\"fixture\":" : i == 1 ? ",\"given\":[" : ",");
        put_string(t, word);
    }
    put_text(t, i == 1 ? ",\"given\":[]}" : "]}");
}

/**
 * Write the invocation of the case into `t`, as
 * tools/etude/schemas/invocation.schema.json gives it: the program and its
 * arguments, the files, the input -i names, a workspace and the fixtures of
 * -F, and an environment of PATH, HOME as the workspace, ETUDE_CASE and the
 * -E variables, the last of a name winning, less the variables -U names.
 */
static void invocation(struct text *t, const struct options *o, const struct file *files,
                       const char *token)
{
    static const char *const base[] = {"PATH=/bin:/usr/bin:/sbin:/usr/sbin", "HOME=${WORKSPACE}"};
    const char *env[ENV_MAX + 3];
    char name[128];
    int n = 0, i, j;

    for (i = 0; i < 2; i++)
        env[n++] = base[i];
    for (i = 0; i < o->nenv; i++)
        env[n++] = o->env[i];
    putf(t, "{\"schema_version\":1,\"run_id\":");
    put_string(t, o->run);
    put_text(t, ",\"attempt_id\":");
    put_string(t, o->attempt);
    putf(t, ",\"attempt_token\":\"%s\",\"case_id\":", token);
    put_string(t, o->case_id);
    put_text(t, ",\"program\":");
    put_string(t, o->argv[0]);
    put_text(t, ",\"arguments\":[");
    for (i = 1; o->argv[i] != NULL; i++) {
        if (i > 1)
            put_text(t, ",");
        put_string(t, o->argv[i]);
    }
    put_text(t, "],\"environment\":{\"ETUDE_CASE\":");
    put_string(t, o->case_id);
    for (i = 0; i < n; i++) {
        size_t len = (size_t)(strchr(env[i], '=') - env[i]);
        for (j = i + 1; j < n; j++)
            if (strncmp(env[j], env[i], len + 1) == 0)
                break;
        if (j < n || len >= sizeof(name) || (len == 10 && strncmp(env[i], "ETUDE_CASE", 10) == 0) ||
            left_out(o, env[i], len))
            continue;
        memcpy(name, env[i], len);
        name[len] = '\0';
        put_text(t, ",");
        put_string(t, name);
        put_text(t, ":");
        put_string(t, env[i] + len + 1);
    }
    put_text(t, "},\"stdin\":");
    if (o->input != NULL)
        put_string(t, o->input);
    else
        put_text(t, "null");
    put_text(t, ",\"role\":\"ordinary\",\"files\":[");
    for (i = 0; i < o->npaths; i++) {
        put_text(t, i > 0 ? ",{\"name\":" : "{\"name\":");
        put_string(t, files[i].name);
        putf(t, ",\"sha256\":\"%s\",\"size_bytes\":%llu,\"mode\":\"%s\"}", files[i].sha,
             (unsigned long long)files[i].size, files[i].mode);
    }
    put_text(t, "],\"fixtures\":[{\"fixture\":\"workspace\",\"given\":[]}");
    for (i = 0; i < o->nfixtures; i++)
        put_fixture(t, o->fixture[i]);
    putf(t,
         "],\"deadline_us\":%llu,"
         "\"grace_us\":%llu,\"output_max_bytes\":%llu}\n",
         (unsigned long long)o->deadline_ms * 1000, (unsigned long long)o->grace_ms * 1000,
         (unsigned long long)o->output_max);
}

/**
 * Judge an attempt from what arrived of it.
 *
 * A pass is an exit with status 0 whose cleanup is confirmed. A cancelled
 * attempt is one that the controller cancelled at its deadline and that
 * stopped. An attempt whose whole end did not arrive, whose end was lost
 * with its etude-guest, that stopped when its connection was lost, or that
 * exited 0 without its cleanup confirmed is incomplete; any other end is a
 * failure.
 *
 * @return the exit status of the verdict, with its name in `*verdict` and
 *         its reason in `why`.
 */
static int judge(enum etude_remote_status st, const struct etude_remote_result *res,
                 const char **verdict, char *why, size_t cap)
{
    why[0] = '\0';
    if (st != ETUDE_REMOTE_OK || !res->complete) {
        *verdict = "incomplete";
        snprintf(why, cap, "its whole end did not arrive (%s)", etude_remote_status_text(st));
        return EXIT_INCOMPLETE;
    }
    switch (res->end) {
    case ETUDE_END_EXITED:
        snprintf(why, cap, "exited %d", res->value);
        if (res->value != 0)
            break;
        if (res->cleanup != ETUDE_CLEANUP_CONFIRMED) {
            *verdict = "incomplete";
            snprintf(why, cap, "exited 0, but its cleanup is unconfirmed");
            return EXIT_INCOMPLETE;
        }
        *verdict = "pass";
        return EXIT_PASS;
    case ETUDE_END_SIGNALED:
        snprintf(why, cap, "killed by signal %d", res->value);
        break;
    case ETUDE_END_STOPPED:
        if (res->cancelled) {
            *verdict = "cancelled";
            snprintf(why, cap, "stopped at the controller's deadline");
            return EXIT_CANCELLED;
        }
        if (res->reconnects > 0) {
            *verdict = "incomplete";
            snprintf(why, cap, "stopped when its connection was lost");
            return EXIT_INCOMPLETE;
        }
        snprintf(why, cap, "stopped at its deadline");
        break;
    default:
        *verdict = "incomplete";
        snprintf(why, cap, "its end was lost with its etude-guest");
        return EXIT_INCOMPLETE;
    }
    *verdict = "failure";
    return EXIT_FAILURE;
}

/** Write what the case printed: its standard output to ours, its standard
 * error to ours, each ended by a newline when it has none. */
static void print_output(const struct etude_remote_result *res)
{
    const struct etude_output *o[2];
    FILE *to[2];
    int i;

    o[0] = &res->out;
    o[1] = &res->err;
    to[0] = stdout;
    to[1] = stderr;
    for (i = 0; i < 2; i++) {
        if (o[i]->data == NULL || o[i]->kept == 0)
            continue;
        fwrite(o[i]->data, 1, o[i]->kept, to[i]);
        if (o[i]->data[o[i]->kept - 1] != '\n')
            fputc('\n', to[i]);
        if (o[i]->total > o[i]->kept)
            fprintf(stderr, "etude run: the case's %s had %llu bytes, of which %lu were kept\n",
                    i == 0 ? "output" : "error output", (unsigned long long)o[i]->total,
                    (unsigned long)o[i]->kept);
    }
    fflush(stdout);
    fflush(stderr);
}

static int structured_judge(const struct options *o, enum etude_remote_status st,
                            const struct etude_remote_result *res, const char **verdict, int code)
{
    if (st != ETUDE_REMOTE_OK || !res->complete)
        return code;
    if (res->cleanup != ETUDE_CLEANUP_CONFIRMED) {
        *verdict = "incomplete";
        return EXIT_INCOMPLETE;
    }
    if (res->end == o->expected_end && res->value == o->expected_value) {
        *verdict = "pass";
        return EXIT_PASS;
    }
    if (res->end == ETUDE_END_EXITED || res->end == ETUDE_END_SIGNALED) {
        *verdict = "failure";
        return EXIT_FAILURE;
    }
    return code;
}

/**
 * Store the files and the invocation, run the attempt and wait for its end,
 * remove its workspace after a pass unless `-k` keeps it, and end the
 * session.
 *
 * @return the exit status of the verdict.
 */
static int run_case(const struct options *o, const struct file *files,
                    struct etude_native_result *outcome)
{
    static struct etude_remote_conf conf;
    static char *argv[4];
    enum etude_remote_status st;
    struct etude_remote_result res;
    struct etude_remote *r = NULL;
    struct text inv;
    char token[ETUDE_FRAME_TOKEN_LEN + 1], why[256], process[24];
    const char *verdict = "incomplete";
    int i, code;

    token_of(o->attempt, token);
    memset(&inv, 0, sizeof(inv));
    invocation(&inv, o, files, token);
    if (inv.bad) {
        free(inv.p);
        fputs("etude run: out of memory\n", stderr);
        return EXIT_INCOMPLETE;
    }
    argv[0] = "/bin/sh";
    argv[1] = "-c";
    argv[2] = (char *)o->transport;
    argv[3] = NULL;
    memset(&conf, 0, sizeof(conf));
    conf.argv = o->transport_argv ? o->transport_argv : argv;
    conf.run_id = o->run;
    conf.journal = o->journal;
    conf.evidence = o->evidence;
    conf.stage_us = o->stage_ms * 1000;
    conf.reconnects = o->reconnects;
    memset(&res, 0, sizeof(res));
    st = etude_remote_open(&conf, &r, why, sizeof(why));
    if (st != ETUDE_REMOTE_OK) {
        free(inv.p);
        printf("etude run: result: incomplete: case %s: not run: the connection failed (%s: %s)\n",
               o->case_id, etude_remote_status_text(st), why);
        if (outcome)
            outcome->transport = st;
        return EXIT_INCOMPLETE;
    }
    for (i = 0; i < o->npaths && st == ETUDE_REMOTE_OK; i++) {
        fprintf(stderr, "etude run: put: %s: %lu bytes\n", files[i].name,
                (unsigned long)files[i].size);
        st = o->staged_only
                 ? etude_remote_reference(r, files[i].name, files[i].size, files[i].sha,
                                          files[i].mode)
                 : etude_remote_put(r, files[i].name, files[i].data, files[i].size, files[i].mode);
    }
    if (st == ETUDE_REMOTE_OK) {
        fprintf(stderr, "etude run: run: case %s, attempt %s\n", o->case_id, token);
        st = etude_remote_run(r, inv.p, inv.len, o->cancel_ms * 1000, &res);
    } else {
        memcpy(res.attempt, token, sizeof(token));
    }
    free(inv.p);
    print_output(&res);
    code = judge(st, &res, &verdict, why, sizeof(why));
    if (outcome) {
        code = structured_judge(o, st, &res, &verdict, code);
        outcome->transport = st;
    }
    if (res.started)
        snprintf(process, sizeof(process), "%ld", res.process.pid);
    else
        snprintf(process, sizeof(process), "-");
    printf("etude run: result: %s: case %s, attempt %s: %s; process %s, cleanup %s, "
           "reconnections %u\n",
           verdict, o->case_id, token, why, process,
           res.cleanup == ETUDE_CLEANUP_CONFIRMED ? "confirmed" : "unconfirmed", res.reconnects);
    if (st != ETUDE_REMOTE_OK)
        printf("etude run: why: %s\n", etude_remote_why(r));
    fflush(stdout);
    if (code == EXIT_PASS && !o->keep) {
        st = etude_remote_remove(r, token);
        if (outcome)
            outcome->removed = st == ETUDE_REMOTE_OK;
        if (st != ETUDE_REMOTE_OK)
            fprintf(stderr, "etude run: remove: %s: %s\n", etude_remote_status_text(st),
                    etude_remote_why(r));
    }
    if (outcome)
        outcome->attempt = res;
    else
        etude_remote_result_free(&res);
    if (etude_remote_close(r) != ETUDE_REMOTE_OK ||
        (outcome && code == EXIT_PASS && !o->keep && !outcome->removed))
        code = EXIT_INCOMPLETE;
    return code;
}

/** Validate and execute the shared structured invocation. */
static int execute_options(struct options o, struct etude_native_result *outcome)
{
    struct file files[FILES_MAX];
    int i, j, code;
    json_program = "etude run";
    if (!ascii(o.run) || !ascii(o.attempt) || !ascii(o.case_id) || !good_name(o.argv[0])) {
        fputs("etude run: the ids must be ASCII and the program a file's name\n", stderr);
        return EXIT_USAGE;
    }
    for (i = 0; o.argv[i] != NULL; i++)
        if (!ascii(o.argv[i])) {
            fprintf(stderr, "etude run: argument %d is not ASCII\n", i);
            return EXIT_USAGE;
        }
    for (i = 0; i < o.nenv; i++)
        if (!ascii(o.env[i])) {
            fprintf(stderr, "etude run: %s is not ASCII\n", o.env[i]);
            return EXIT_USAGE;
        }
    for (i = 0; i < o.nfixtures; i++)
        if (!ascii(o.fixture[i])) {
            fprintf(stderr, "etude-native: the fixture %s is not ASCII\n", o.fixture[i]);
            return EXIT_USAGE;
        }
    memset(files, 0, sizeof(files));
    code = EXIT_USAGE;
    for (i = 0; i < o.npaths; i++) {
        if (read_case_file(o.path[i], &files[i], o.staged_only) != 0)
            goto out;
        for (j = 0; j < i; j++)
            if (strcmp(files[j].name, files[i].name) == 0) {
                fprintf(stderr, "etude run: two files are named %s\n", files[i].name);
                goto out;
            }
    }
    for (i = 0; i < o.npaths && strcmp(files[i].name, o.argv[0]) != 0; i++) {
    }
    if (i == o.npaths) {
        fprintf(stderr, "etude run: the program %s is not one of the files\n", o.argv[0]);
        goto out;
    }
    for (i = 0; o.input != NULL && i < o.npaths && strcmp(files[i].name, o.input) != 0; i++) {
    }
    if (o.input != NULL && i == o.npaths) {
        fprintf(stderr, "etude-native: the input %s is not one of the files\n", o.input);
        goto out;
    }
    code = run_case(&o, files, outcome);
out:
    for (i = 0; i < o.npaths; i++)
        free(files[i].data);
    return code;
}

int etude_native_main(int argc, char **argv)
{
    struct options o;
    if (parse(argc, argv, &o) != 0)
        return usage();
    return execute_options(o, NULL);
}
static int valid_specification(const struct etude_native_spec *s)
{
    if (!s || !s->run || !s->attempt || !s->case_id || !s->journal || !s->transport_argv ||
        !s->transport_argv[0] || !s->argv || !s->argv[0] || !s->files || !s->nfiles ||
        s->nfiles > FILES_MAX || s->nenvironment > ENV_MAX || s->nunset > ENV_MAX ||
        s->nfixtures > FIXTURES_MAX || s->reconnects > 100 || s->expected_value < 0 ||
        s->expected_value > 255 ||
        (s->expected_end == ETUDE_END_SIGNALED && s->expected_value == 0) ||
        (s->expected_end != ETUDE_END_EXITED && s->expected_end != ETUDE_END_SIGNALED))
        return 0;
    return 1;
}
static int specification(const struct etude_native_spec *s, struct options *o)
{
    size_t i;
    memset(o, 0, sizeof(*o));
    if (!valid_specification(s))
        return 0;
    o->run = s->run;
    o->attempt = s->attempt;
    o->case_id = s->case_id;
    o->journal = s->journal;
    o->evidence = s->evidence;
    o->transport_argv = s->transport_argv;
    o->argv = (char **)s->argv;
    o->input = s->input;
    o->staged_only = s->staged_only;
    o->keep = s->keep;
    o->expected_end = s->expected_end;
    o->expected_value = s->expected_value;
    o->deadline_ms = s->deadline_ms ? s->deadline_ms : 600000;
    o->grace_ms = s->grace_ms ? s->grace_ms : 5000;
    o->output_max = s->output_max ? s->output_max : 1048576;
    o->stage_ms = s->stage_ms ? s->stage_ms : 30000;
    o->reconnects = s->reconnects;
    o->cancel_ms = s->cancel_ms;
    for (i = 0; i < s->nfiles; i++)
        if (option(o, 'f', s->files[i]))
            return 0;
    for (i = 0; i < s->nenvironment; i++)
        if (!s->environment || option(o, 'E', s->environment[i]))
            return 0;
    for (i = 0; i < s->nunset; i++)
        if (!s->unset || option(o, 'U', s->unset[i]))
            return 0;
    for (i = 0; i < s->nfixtures; i++)
        if (!s->fixtures || option(o, 'F', s->fixtures[i]))
            return 0;
    return 1;
}
int etude_native_attempt(const struct etude_native_spec *spec, struct etude_native_result *result)
{
    struct options o;
    if (!result)
        return EXIT_USAGE;
    memset(result, 0, sizeof(*result));
    result->transport = ETUDE_REMOTE_SYSTEM;
    result->attempt.end = ETUDE_END_LOST;
    result->attempt.value = -1;
    result->attempt.cleanup = ETUDE_CLEANUP_UNCONFIRMED;
    result->status = EXIT_USAGE;
    if (specification(spec, &o))
        result->status = execute_options(o, result);
    return result->status;
}
void etude_native_result_free(struct etude_native_result *result)
{
    if (result)
        etude_remote_result_free(&result->attempt);
}
