/*
 * netcase.c: the operations the network scenarios of tests/cases/machine
 * share, as netcase.h declares them.
 */
#include "netcase.h"

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>


/* The longest assertion's detail. */
#define NET_DETAIL_MAX 160

/** Give the detail of status `st`. */
static const char *status_text(enum etude_status st)
{
    switch (st) {
    case ETUDE_OK:
        return "done";
    case ETUDE_FAILED:
        return "failed";
    case ETUDE_LOST:
        return "the machine or its transport was lost";
    case ETUDE_BOUND:
        return "the step's bound expired";
    case ETUDE_STOPPED:
        return "the case was stopped";
    case ETUDE_REFUSED:
        return "refused: the manifest does not declare it";
    }
    return "unknown";
}

/** Make `format` and `ap` the detail `buf` of `size` bytes, in printable
 * ASCII on one line. */
static void detail(char *buf, size_t size, const char *format, va_list ap)
{
    size_t i;

    vsnprintf(buf, size, format, ap);
    for (i = 0; buf[i] != '\0'; i++) {
        if (buf[i] < ' ' || buf[i] > '~')
            buf[i] = ' ';
    }
}

/** Record that an operation of `u` ended with `st` when it is the first to
 * fail, for the reason `format` makes; give `st`. */
static enum etude_status failed(struct net_case *u, enum etude_status st, const char *format, ...)
{
    va_list ap;

    if (st == ETUDE_OK || u->st != ETUDE_OK)
        return st;
    u->st = st;
    va_start(ap, format);
    detail(u->why, sizeof(u->why), format, ap);
    va_end(ap);
    return st;
}

void net_case(struct net_case *u, struct etude_case *c)
{
    memset(u, 0, sizeof(*u));
    u->c = c;
    u->st = ETUDE_OK;
    if (etude_has_machine(c, "channel-client")) {
        struct etude_termination end;
        u->st = etude_step(c, "channel-client");
        if (u->st == ETUDE_OK)
            u->st = etude_boot(c, "channel-client", NULL, &u->client, &end);
        if (u->st != ETUDE_OK)
            snprintf(u->why, sizeof(u->why), "the declared Rach2 SSH client did not start");
    }
}

int net_ok(const struct net_case *u)
{
    return u->st == ETUDE_OK;
}

void net_fail(struct net_case *u, const char *format, ...)
{
    va_list ap;

    if (u->st != ETUDE_OK)
        return;
    u->st = ETUDE_FAILED;
    va_start(ap, format);
    detail(u->why, sizeof(u->why), format, ap);
    va_end(ap);
}

enum etude_status net_step(struct net_case *u, const char *step)
{
    enum etude_status st;

    if (u->st != ETUDE_OK)
        return u->st;
    st = etude_step(u->c, step);
    return failed(u, st, "step %s: %s", step, status_text(st));
}

void net_assert(struct net_case *u, const char *id, int held, const char *format, ...)
{
    char why[NET_DETAIL_MAX];
    va_list ap;

    if (u->st != ETUDE_OK) {
        if (u->told) {
            snprintf(why, sizeof(why), "not reached: %.140s", u->why);
            etude_notrun(u->c, id, why);
        } else {
            u->told = 1;
            etude_assert(u->c, id, 0, u->why);
        }
        return;
    }
    va_start(ap, format);
    detail(why, sizeof(why), format, ap);
    va_end(ap);
    if (!held)
        u->failures++;
    etude_assert(u->c, id, held, why);
}

int net_end(struct net_case *u)
{
    struct net_made *m;
    int passed = u->st == ETUDE_OK && u->failures == 0;

    while ((m = u->made) != NULL) {
        u->made = m->next;
        if (passed && unlink(m->path) != 0 && errno != ENOENT)
            fprintf(stderr, "netcase: %s: not removed: %s\n", m->path, strerror(errno));
        free(m);
    }
    if (u->st != ETUDE_OK && !u->told)
        fprintf(stderr, "netcase: failed: %s\n", u->why);
    return u->st == ETUDE_OK ? 0 : 1;
}

const char *net_path(struct net_case *u, const char *name, char *buf, size_t size)
{
    const char *path;
    struct net_made *m;

    if (u->st != ETUDE_OK)
        return NULL;
    path = etude_evidence(u->c, name);
    if (path == NULL || strlen(path) >= size || strlen(path) >= sizeof(m->path)) {
        failed(u, ETUDE_FAILED, "evidence %s: no path", name);
        return NULL;
    }
    m = malloc(sizeof(*m));
    if (m == NULL) {
        failed(u, ETUDE_FAILED, "evidence %s: out of memory", name);
        return NULL;
    }
    strcpy(m->path, path);
    m->next = u->made;
    u->made = m;
    strcpy(buf, path);
    return buf;
}

enum etude_status net_boot(struct net_boot *b, struct net_case *u, const char *machine, unsigned n,
                           const char *const disks[])
{
    struct etude_termination end;
    enum etude_status st;
    char name[200];
    const char *path;

    memset(b, 0, sizeof(*b));
    b->u = u;
    if (u->st != ETUDE_OK)
        return u->st;
    snprintf(name, sizeof(name), "%s.%u.console", machine, n);
    path = etude_evidence(u->c, name);
    if (path == NULL || strlen(path) >= sizeof(b->console))
        return failed(u, ETUDE_FAILED, "boot %s: no evidence path %s", machine, name);
    strcpy(b->console, path);
    st = etude_boot(u->c, machine, disks, &b->m, &end);
    return failed(u, st, "boot %s: %s", machine, status_text(st));
}

/** Wait for `text` on `b`'s console after `from`, giving the offset past it
 * in `at`. */
static enum etude_status wait_from(struct net_boot *b, const char *text, uint64_t from,
                                   uint64_t *at)
{
    enum etude_status st;

    if (b->u->st != ETUDE_OK)
        return b->u->st;
    st = etude_console_wait(b->m, text, from, at);
    return failed(b->u, st, "waiting for \"%s\": %s", text, status_text(st));
}

enum etude_status net_wait(struct net_boot *b, const char *text)
{
    return wait_from(b, text, b->at, &b->at);
}

enum etude_status net_seen(struct net_boot *b, const char *text)
{
    uint64_t at;

    return wait_from(b, text, 0, &at);
}

enum etude_status net_times(struct net_boot *b, const char *text, unsigned n, uint64_t *at)
{
    unsigned i;

    *at = 0;
    for (i = 0; i < n; i++) {
        if (wait_from(b, text, *at, at) != ETUDE_OK)
            return b->u->st;
    }
    return b->u->st;
}

/** Type `line` and a carriage return, naming it in a failure as `shown`. */
static enum etude_status type_line(struct net_boot *b, const char *line, const char *shown)
{
    enum etude_status st;
    char text[512];

    if (b->u->st != ETUDE_OK)
        return b->u->st;
    if (strlen(line) + 2 > sizeof(text))
        return failed(b->u, ETUDE_FAILED, "typing \"%s\": the line is too long", shown);
    snprintf(text, sizeof(text), "%s\r", line);
    st = etude_type(b->m, text);
    return failed(b->u, st, "typing \"%s\": %s", shown, status_text(st));
}

enum etude_status net_line(struct net_boot *b, const char *line)
{
    return type_line(b, line, line);
}

enum etude_status net_login(struct net_boot *b, const char *user, const char *password,
                            const char *prompt)
{
    if (net_wait(b, "login:") != ETUDE_OK || net_line(b, user) != ETUDE_OK ||
        net_wait(b, "Password:") != ETUDE_OK || type_line(b, password, "the password") != ETUDE_OK)
        return b->u->st;
    return net_wait(b, prompt);
}

enum etude_status net_command(struct net_boot *b, const char *command, const char *marker,
                              char **text)
{
    uint64_t from;

    *text = NULL;
    if (net_line(b, command) != ETUDE_OK)
        return b->u->st;
    from = b->at;
    if (net_wait(b, marker) != ETUDE_OK)
        return b->u->st;
    *text = net_text(b, from, b->at);
    return ETUDE_OK;
}

enum etude_status net_host(struct net_case *u, char *const argv[], const char *output,
                           struct net_run *r)
{
    enum etude_status st;
    char name[200];
    const char *path;

    memset(r, 0, sizeof(*r));
    r->end.end = ETUDE_END_LOST;
    r->end.value = -1;
    if (u->st != ETUDE_OK)
        return u->st;
    if (strcmp(argv[0], "ssh") == 0 || strcmp(argv[0], "scp") == 0 ||
        strcmp(argv[0], "sftp") == 0 || strcmp(argv[0], "ptydrive") == 0)
        return net_ssh_input(u, argv, NULL, output, r);
    st = etude_host(u->c, argv, output, &r->end);
    if (st != ETUDE_OK)
        return failed(u, st, "%s: %s", argv[0], status_text(st));
    path = etude_evidence(u->c, output);
    r->out = net_file(path != NULL ? path : "");
    snprintf(name, sizeof(name), "%s.err", output);
    path = etude_evidence(u->c, name);
    r->err = net_file(path != NULL ? path : "");
    return ETUDE_OK;
}

/* The most argument words accepted by the guest's invocation. */
#define CLIENT_WORDS 128

/* A translated OpenSSH invocation and an optional sftp return file. */
struct client_command {
    const char *argv[CLIENT_WORDS];
    char *owned[CLIENT_WORDS];
    size_t count;
    char returned[160];
    char evidence[256];
};

/** Free the translated words after the channel has copied its invocation. */
static void client_free(struct client_command *a)
{
    size_t i;

    for (i = 0; i < a->count; i++)
        free(a->owned[i]);
}

/** Copy one translated word, keeping it until the invocation is stored. */
static const char *client_word(struct client_command *a, const char *word)
{
    size_t n = strlen(word) + 1;
    char *p = malloc(n);

    if (p == NULL || a->count + 1 >= CLIENT_WORDS) {
        free(p);
        return NULL;
    }
    memcpy(p, word, n);
    a->owned[a->count] = p;
    a->argv[a->count++] = p;
    a->argv[a->count] = NULL;
    return p;
}

/** Put a local input on the Rach2 client and return its temporary path. */
static int client_file(struct net_case *u, const char *from, char path[160])
{
    snprintf(path, 160, "/tmp/etude-client-%u", ++u->client_files);
    if (etude_guest_put(u->client, from, path, "0600") == ETUDE_OK)
        return 1;
    net_fail(u, "the SSH client's input could not be transferred");
    return 0;
}

/** Replace the one get of a scenario's sftp batch with a client-local
 * destination. The finished file is fetched through the framed channel. */
static int client_batch(struct net_case *u, struct client_command *a, const char *from,
                         char path[160])
{
    char *text = net_file(from), command[16], remote[4096], local[4096];
    char evidence[80], batch[4096];
    const char *base;
    FILE *f;
    int ok;

    ok = sscanf(text, "%15s %4095s %4095[^\n]", command, remote, local) == 3 &&
         strcmp(command, "get") == 0 && strchr(remote, '"') == NULL;
    free(text);
    if (!ok) {
        net_fail(u, "the SSH client's sftp batch is not one file retrieval");
        return 0;
    }
    base = strrchr(local, '/');
    base = base != NULL ? base + 1 : local;
    if (strlen(base) >= sizeof(a->evidence)) {
        net_fail(u, "the sftp return evidence name is too long");
        return 0;
    }
    strcpy(a->evidence, base);
    snprintf(a->returned, sizeof(a->returned), "/tmp/etude-return-%u", ++u->client_files);
    snprintf(evidence, sizeof(evidence), "client-batch-%u", u->client_files);
    if (net_path(u, evidence, batch, sizeof(batch)) == NULL)
        return 0;
    f = fopen(batch, "wb");
    if (f == NULL)
        return 0;
    ok = fprintf(f, "get \"%s\" \"%s\"\n", remote, a->returned) > 0;
    if (fclose(f) != 0)
        ok = 0;
    return ok && client_file(u, batch, path);
}

/** Translate only the client invocation: local file arguments are copied,
 * and its destination reaches the target's host forward through QEMU's
 * gateway. The remote command string is left byte for byte. */
static int client_arguments(struct net_case *u, char *const argv[], struct client_command *a)
{
    struct stat st;
    char path[160], address[512];
    const char *word, *at;
    size_t i;

    memset(a, 0, sizeof(*a));
    if (u->client == NULL) {
        net_fail(u, "the manifest declares no Rach2 SSH client");
        return 0;
    }
    for (i = 0; argv[i] != NULL; i++) {
        word = argv[i];
        if (i == 0)
            word = strcmp(word, "ptydrive") == 0 ? "/ptydrive" :
                   strcmp(word, "scp") == 0 ? "/usr/bin/scp" :
                   strcmp(word, "sftp") == 0 ? "/usr/bin/sftp" : "/usr/bin/ssh";
        else if (i > 0 && strcmp(argv[i - 1], "-b") == 0) {
            if (!client_batch(u, a, word, path))
                return 0;
            word = path;
        } else if (word[0] != '-' && stat(word, &st) == 0 && S_ISREG(st.st_mode)) {
            if (!client_file(u, word, path))
                return 0;
            word = path;
        } else if ((at = strstr(word, "@127.0.0.1")) != NULL &&
                   (at[10] == '\0' || at[10] == ':')) {
            snprintf(address, sizeof(address), "%.*s@10.0.2.2%s", (int)(at - word), word, at + 10);
            word = address;
        }
        if (client_word(a, word) == NULL) {
            net_fail(u, "the SSH client's invocation is too long");
            return 0;
        }
    }
    return 1;
}

/** Give the default environment and the password only to the Rach2
 * terminal driver, which redacts it before writing either transcript. */
static void client_environment(const char *env[5], char password[512])
{
    const char *value = getenv("RACH2_PASSWORD");

    env[0] = "PATH=/bin:/sbin:/usr/bin:/usr/sbin:/";
    env[1] = "HOME=/root";
    env[2] = "TERM=vt100";
    env[3] = NULL;
    env[4] = NULL;
    if (value != NULL) {
        snprintf(password, 512, "RACH2_PASSWORD=%s", value);
        env[3] = password;
    }
}

/** Read a client's result evidence into the ordinary network result. */
static enum etude_status client_result(struct net_case *u, const char *output,
                                        enum etude_status st,
                                        const struct etude_guest_result *result, struct net_run *r)
{
    char name[256];
    const char *path;

    r->end = result->termination;
    if (st == ETUDE_OK && (!result->complete || result->cleanup != ETUDE_CLEANUP_CONFIRMED))
        st = ETUDE_LOST;
    if (st != ETUDE_OK)
        return failed(u, st, "Rach2 SSH client: %s", status_text(st));
    path = etude_evidence(u->c, output);
    r->out = net_file(path != NULL ? path : "");
    snprintf(name, sizeof(name), "%s.err", output);
    path = etude_evidence(u->c, name);
    r->err = net_file(path != NULL ? path : "");
    return ETUDE_OK;
}

enum etude_status net_guest(struct net_case *u, struct etude_case_machine *m,
                            const char *const argv[], const char *const env[],
                            const char *input, const char *output, struct net_run *r)
{
    struct etude_guest_result result;
    enum etude_status st;

    memset(r, 0, sizeof(*r));
    r->end.end = ETUDE_END_LOST;
    r->end.value = -1;
    if (!net_ok(u))
        return u->st;
    st = etude_guest_command(m, argv, env, input, output, &result);
    return client_result(u, output, st, &result, r);
}

enum etude_status net_ssh_input(struct net_case *u, char *const argv[], const char *input,
                                const char *output, struct net_run *r)
{
    struct client_command a;
    struct etude_guest_result result;
    const char *env[5];
    char password[512];
    enum etude_status st;

    memset(r, 0, sizeof(*r));
    r->end.end = ETUDE_END_LOST;
    r->end.value = -1;
    if (!net_ok(u))
        return u->st;
    if (!client_arguments(u, argv, &a)) {
        client_free(&a);
        return u->st;
    }
    client_environment(env, password);
    st = etude_guest_command(u->client, a.argv, env, input, output, &result);
    if (st == ETUDE_OK && a.returned[0] != '\0' &&
        result.termination.end == ETUDE_END_EXITED && result.termination.value == 0)
        st = etude_guest_get(u->client, a.returned, a.evidence);
    client_free(&a);
    return client_result(u, output, st, &result, r);
}

enum etude_status net_ssh_start(struct net_case *u, char *const argv[], const char *ready)
{
    struct client_command a;
    const char *env[5];
    char password[512];
    enum etude_status st;

    if (!net_ok(u))
        return u->st;
    if (!client_arguments(u, argv, &a)) {
        client_free(&a);
        return u->st;
    }
    client_environment(env, password);
    st = etude_guest_start(u->client, a.argv, env, NULL);
    client_free(&a);
    if (st == ETUDE_OK)
        st = etude_guest_output_wait(u->client, ready);
    return failed(u, st, "the Rach2 SSH workload did not start: %s", status_text(st));
}

enum etude_status net_ssh_finish(struct net_case *u, const char *output, struct net_run *r)
{
    struct etude_guest_result result;
    enum etude_status st;

    memset(r, 0, sizeof(*r));
    st = etude_guest_finish(u->client, 1, output, &result);
    return client_result(u, output, st, &result, r);
}

void net_run_free(struct net_run *r)
{
    free(r->out);
    free(r->err);
    r->out = NULL;
    r->err = NULL;
}

int net_exited0(const struct net_run *r)
{
    return r->end.end == ETUDE_END_EXITED && r->end.value == 0;
}

const char *net_secret(struct net_case *u, const char *name, char *buf, size_t size)
{
    const char *path;
    char *text;
    size_t n;

    buf[0] = '\0';
    if (u->st != ETUDE_OK)
        return buf;
    path = etude_artifact(u->c, name);
    if (path == NULL) {
        failed(u, ETUDE_REFUSED, "artifact %s: %s", name, status_text(ETUDE_REFUSED));
        return buf;
    }
    text = net_file(path);
    n = strcspn(text, "\r\n");
    if (n == 0 || n >= size) {
        failed(u, ETUDE_FAILED, "artifact %s: no password on its first line", name);
    } else {
        memcpy(buf, text, n);
        buf[n] = '\0';
    }
    free(text);
    return buf;
}

const char *net_key(struct net_case *u, const char *name, char *buf, size_t size)
{
    const char *from;
    char *text;
    size_t n;
    int fd;

    if (u->st != ETUDE_OK)
        return NULL;
    from = etude_artifact(u->c, name);
    if (from == NULL) {
        failed(u, ETUDE_REFUSED, "artifact %s: %s", name, status_text(ETUDE_REFUSED));
        return NULL;
    }
    if (net_path(u, name, buf, size) == NULL)
        return NULL;
    text = net_file(from);
    fd = open(buf, O_WRONLY | O_CREAT | O_EXCL, 0600);
    n = strlen(text);
    if (fd < 0 || n == 0 || write(fd, text, n) != (ssize_t)n) {
        failed(u, ETUDE_FAILED, "artifact %s: no private copy: %s", name,
               fd < 0 ? strerror(errno) : "a short write");
        buf = NULL;
    }
    if (fd >= 0)
        close(fd);
    free(text);
    return buf;
}

/** Read at most `max` bytes of the file at `path` from `from`, into a text
 * from malloc. */
static char *read_part(const char *path, uint64_t from, uint64_t max)
{
    FILE *f = fopen(path, "rb");
    char *text;
    size_t n = 0, got;

    text = malloc((size_t)max + 1);
    if (text == NULL)
        abort();
    if (f != NULL && fseek(f, (long)from, SEEK_SET) == 0) {
        while (n < max && (got = fread(text + n, 1, (size_t)max - n, f)) > 0)
            n += got;
    }
    if (f != NULL)
        fclose(f);
    text[n] = '\0';
    return text;
}

char *net_text(const struct net_boot *b, uint64_t from, uint64_t to)
{
    return read_part(b->console, from, to > from ? to - from : 0);
}

char *net_file(const char *path)
{
    FILE *f = fopen(path, "rb");
    long size = -1;

    if (f != NULL && fseek(f, 0, SEEK_END) == 0)
        size = ftell(f);
    if (f != NULL)
        fclose(f);
    return read_part(path, 0, size > 0 ? (uint64_t)size : 0);
}

/** Say whether `pattern` matches the whole of `s` of `n` bytes, from its
 * start. */
static int match_here(const char *s, size_t n, const char *pattern)
{
    size_t i;

    if (*pattern == '\0')
        return 1;
    if (*pattern == '*') {
        for (i = 0; i <= n; i++) {
            if (match_here(s + i, n - i, pattern + 1))
                return 1;
        }
        return 0;
    }
    if (*pattern == '#') {
        for (i = 0; i < n && s[i] >= '0' && s[i] <= '9'; i++) {
            if (match_here(s + i + 1, n - i - 1, pattern + 1))
                return 1;
        }
        return 0;
    }
    return n > 0 && *s == *pattern && match_here(s + 1, n - 1, pattern + 1);
}

/** Say whether line `line` of `len` bytes has `pattern` in it. */
static int match(const char *line, size_t len, const char *pattern)
{
    size_t i;

    for (i = 0; i <= len; i++) {
        if (match_here(line + i, len - i, pattern))
            return 1;
    }
    return 0;
}

const char *net_find(const char *text, const char *pattern)
{
    const char *p;
    size_t len;

    if (text == NULL)
        return NULL;
    for (p = text; *p != '\0'; p += strspn(p, "\r\n")) {
        len = strcspn(p, "\r\n");
        if (match(p, len, pattern))
            return p;
        p += len;
    }
    return NULL;
}

unsigned net_count(const char *text, const char *pattern)
{
    unsigned n = 0;
    const char *p;
    size_t len;

    if (text == NULL)
        return 0;
    for (p = text; *p != '\0'; p += strspn(p, "\r\n")) {
        len = strcspn(p, "\r\n");
        if (match(p, len, pattern))
            n++;
        p += len;
    }
    return n;
}

const char *net_copy_line(const char *line, char *buf, size_t size)
{
    size_t n = 0;

    if (size == 0)
        return buf;
    while (line != NULL && line[n] != '\0' && line[n] != '\r' && line[n] != '\n' && n + 1 < size) {
        buf[n] = line[n];
        n++;
    }
    buf[n] = '\0';
    return buf;
}

long net_number_after(const char *text, const char *prefix)
{
    const char *p, *last = NULL;

    if (text == NULL)
        return -1;
    for (p = strstr(text, prefix); p != NULL; p = strstr(p + 1, prefix))
        last = p;
    if (last == NULL)
        return -1;
    last += strlen(prefix);
    if (*last < '0' || *last > '9')
        return -1;
    return strtol(last, NULL, 10);
}
