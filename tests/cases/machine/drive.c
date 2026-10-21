/*
 * drive.c: the console driver of the machine scenarios under tests/cases,
 * over libetude-case, and the host's readings of their disks and evidence.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "drive.h"
#include "sha256.h"

/* The case was stopped: every scenario returns at once. */
static int halted;

/* The boots of each machine in this attempt, which name its consoles. */
static struct {
    char machine[64];
    unsigned boots;
} boots[16];

int drive_halted(void)
{
    return halted;
}

/** Name status `st` for a person. */
static const char *status_name(enum etude_status st)
{
    switch (st) {
    case ETUDE_OK:
        return "ok";
    case ETUDE_FAILED:
        return "failed";
    case ETUDE_LOST:
        return "the machine was lost";
    case ETUDE_BOUND:
        return "the step's bound expired";
    case ETUDE_STOPPED:
        return "the case was stopped";
    case ETUDE_REFUSED:
        return "refused by the manifest";
    }
    return "unknown";
}

/** Name end `e` for a person. */
static const char *end_name(enum etude_end e)
{
    static const char *const names[] = {
        "exited",   "signaled", "init exited", "init killed",        "panic",   "reset",
        "shutdown", "halted",   "QEMU exited", "accelerator failed", "stopped", "lost"};

    return (unsigned)e < sizeof(names) / sizeof(names[0]) ? names[e] : "unknown";
}

/** Record the first call of `d` that did not complete, and its status. */
static int fail(struct drive *d, enum etude_status st, const char *fmt, ...)
{
    va_list ap;
    size_t n;

    if (st == ETUDE_STOPPED)
        halted = 1;
    if (d->failed)
        return -1;
    d->failed = 1;
    va_start(ap, fmt);
    vsnprintf(d->why, sizeof(d->why), fmt, ap);
    va_end(ap);
    n = strlen(d->why);
    snprintf(d->why + n, sizeof(d->why) - n, ": %s", status_name(st));
    return -1;
}

/** Count a boot of `machine` and give its number, from 1. */
static unsigned count_boot(const char *machine)
{
    size_t i;

    for (i = 0; i < sizeof(boots) / sizeof(boots[0]); i++) {
        if (boots[i].machine[0] == '\0')
            snprintf(boots[i].machine, sizeof(boots[i].machine), "%s", machine);
        if (strcmp(boots[i].machine, machine) == 0)
            return ++boots[i].boots;
    }
    return 0;
}

int drive_boot(struct drive *d, struct etude_case *c, const char *machine,
               const char *const disks[])
{
    enum etude_status st;

    memset(d, 0, sizeof(*d));
    d->c = c;
    snprintf(d->machine, sizeof(d->machine), "%s", machine);
    if (halted)
        return fail(d, ETUDE_STOPPED, "the boot of %s", machine);
    st = etude_boot(c, machine, disks, &d->m, &d->end);
    if (st != ETUDE_REFUSED)
        d->boot = count_boot(machine);
    if (st != ETUDE_OK) {
        d->m = NULL;
        return fail(d, st, "the boot of %s, which ended %s %d", machine, end_name(d->end.end),
                    d->end.value);
    }
    return 0;
}

int drive_expect(struct drive *d, const char *text)
{
    enum etude_status st;
    uint64_t at;

    if (d->failed)
        return -1;
    st = etude_console_wait(d->m, text, d->at, &at);
    if (st != ETUDE_OK)
        return fail(d, st, "the wait for \"%s\"", text);
    d->at = at;
    return 0;
}

int drive_send(struct drive *d, const char *fmt, ...)
{
    char line[DRIVE_LINE_MAX + 2];
    enum etude_status st;
    va_list ap;
    int n;

    if (d->failed)
        return -1;
    va_start(ap, fmt);
    n = vsnprintf(line, sizeof(line) - 1, fmt, ap);
    va_end(ap);
    if (n < 0 || n > DRIVE_LINE_MAX)
        return fail(d, ETUDE_REFUSED, "a line longer than %d bytes", DRIVE_LINE_MAX);
    line[n] = '\n';
    line[n + 1] = '\0';
    st = etude_type(d->m, line);
    if (st != ETUDE_OK) {
        line[n] = '\0';
        return fail(d, st, "the line \"%.80s\"", line);
    }
    return 0;
}

int drive_stop(struct drive *d)
{
    char *reply = NULL;
    enum etude_status st;

    if (d->m == NULL)
        return -1;
    etude_qmp(d->m, "quit", NULL, &reply);
    free(reply);
    st = etude_wait_end(d->m, &d->end);
    if (st == ETUDE_STOPPED)
        halted = 1;
    return st == ETUDE_OK ? 0 : -1;
}

int drive_wait_end(struct drive *d)
{
    enum etude_status st;

    if (d->m == NULL)
        return -1;
    st = etude_wait_end(d->m, &d->end);
    if (st != ETUDE_OK)
        return fail(d, st, "the end of %s", d->machine);
    return 0;
}

/** Read file `path` whole, with a NUL after it, leaving out every byte
 * `drop`, or none when `drop` is -1. */
static char *read_text(const char *path, int drop)
{
    char *buf = NULL, *grown;
    size_t len = 0, cap = 0, i, o;
    ssize_t r;
    int fd;

    if (path == NULL || (fd = open(path, O_RDONLY | O_CLOEXEC)) < 0)
        return NULL;
    for (;;) {
        if (cap - len < 65536) {
            cap = cap == 0 ? 1 << 20 : cap * 2;
            grown = realloc(buf, cap + 1);
            if (grown == NULL) {
                free(buf);
                close(fd);
                return NULL;
            }
            buf = grown;
        }
        r = read(fd, buf + len, cap - len);
        if (r < 0 && errno == EINTR)
            continue;
        if (r < 0) {
            free(buf);
            close(fd);
            return NULL;
        }
        if (r == 0)
            break;
        len += (size_t)r;
    }
    close(fd);
    for (i = o = 0; i < len; i++)
        if (drop < 0 || buf[i] != (char)drop)
            buf[o++] = buf[i];
    buf[o] = '\0';
    return buf;
}

char *drive_console(const struct drive *d)
{
    char name[128];

    snprintf(name, sizeof(name), "%s.%u.console", d->machine, d->boot);
    return read_text(etude_evidence(d->c, name), '\r');
}

int text_field(const char *text, const char *prefix, int nth, char *buf, size_t size)
{
    size_t plen = strlen(prefix), n;
    const char *line = text, *eol;

    while (line != NULL && *line != '\0') {
        eol = strchr(line, '\n');
        n = eol != NULL ? (size_t)(eol - line) : strlen(line);
        if (n >= plen && memcmp(line, prefix, plen) == 0 && --nth == 0) {
            n -= plen;
            if (n >= size)
                n = size - 1;
            memcpy(buf, line + plen, n);
            buf[n] = '\0';
            return 0;
        }
        line = eol != NULL ? eol + 1 : NULL;
    }
    return -1;
}

int text_lines(const char *text, const char *needle)
{
    const char *line = text, *eol, *hit;
    int count = 0;

    while (line != NULL && *line != '\0') {
        eol = strchr(line, '\n');
        hit = strstr(line, needle);
        if (hit != NULL && (eol == NULL || hit < eol))
            count++;
        line = eol != NULL ? eol + 1 : NULL;
    }
    return count;
}

int file_digest(const char *path, uint64_t off, uint64_t len, char hex[65])
{
    static unsigned char buf[1 << 20];
    unsigned char sum[32];
    struct sha256 h;
    uint64_t left = len;
    ssize_t r;
    int fd;

    if (path == NULL || (fd = open(path, O_RDONLY | O_CLOEXEC)) < 0)
        return -1;
    if (lseek(fd, (off_t)off, SEEK_SET) == (off_t)-1) {
        close(fd);
        return -1;
    }
    sha256_init(&h);
    while (left > 0) {
        r = read(fd, buf, left < sizeof(buf) ? (size_t)left : sizeof(buf));
        if (r < 0 && errno == EINTR)
            continue;
        if (r < 0 || (r == 0 && len != UINT64_MAX)) {
            close(fd);
            return -1;
        }
        if (r == 0)
            break;
        sha256_update(&h, buf, (size_t)r);
        if (len != UINT64_MAX)
            left -= (uint64_t)r;
    }
    close(fd);
    sha256_final(&h, sum);
    sha256_hex(sum, hex);
    return 0;
}

int fill_pattern(const char *path, uint64_t size)
{
    static char mib[(1 << 20) + 1]; /* one mebibyte and the NUL snprintf ends it with */
    uint64_t done = 0;
    size_t n, w;
    ssize_t r;
    int fd, i;

    for (i = 0; i < 65536; i++)
        snprintf(mib + (size_t)i * 16, 17, "%016d", i);
    if (path == NULL || (fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644)) < 0)
        return -1;
    while (done < size) {
        n = size - done < sizeof(mib) - 1 ? (size_t)(size - done) : sizeof(mib) - 1;
        for (w = 0; w < n; w += (size_t)r) {
            r = write(fd, mib + w, n - w);
            if (r < 0 && errno == EINTR) {
                r = 0;
                continue;
            }
            if (r <= 0) {
                close(fd);
                return -1;
            }
        }
        done += n;
    }
    return close(fd);
}

int drive_host(struct etude_case *c, char *const argv[], const char *output)
{
    struct etude_termination end;
    enum etude_status st;

    st = etude_host(c, argv, output, &end);
    if (st == ETUDE_STOPPED)
        halted = 1;
    if (st != ETUDE_OK || end.end != ETUDE_END_EXITED)
        return -1;
    return end.value;
}

char *evidence_text(struct etude_case *c, const char *name)
{
    return read_text(etude_evidence(c, name), -1);
}
