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
