/*
 * usbcase.c: the operations the USB scenarios of tests/cases/machine share,
 * as usbcase.h declares them.
 */
#include "usbcase.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The longest QMP arguments a scenario sends, and an assertion's detail. */
#define USB_ARGS_MAX   4096
#define USB_DETAIL_MAX 160

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
static enum etude_status failed(struct usb_case *u, enum etude_status st, const char *format, ...)
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

void usb_case(struct usb_case *u, struct etude_case *c)
{
    memset(u, 0, sizeof(*u));
    u->c = c;
    u->st = ETUDE_OK;
}

int usb_ok(const struct usb_case *u)
{
    return u->st == ETUDE_OK;
}

void usb_fail(struct usb_case *u, const char *format, ...)
{
    va_list ap;

    if (u->st != ETUDE_OK)
        return;
    u->st = ETUDE_FAILED;
    va_start(ap, format);
    detail(u->why, sizeof(u->why), format, ap);
    va_end(ap);
}

enum etude_status usb_step(struct usb_case *u, const char *step)
{
    enum etude_status st;

    if (u->st != ETUDE_OK)
        return u->st;
    st = etude_step(u->c, step);
    return failed(u, st, "step %s: %s", step, status_text(st));
}

void usb_assert(struct usb_case *u, const char *id, int held, const char *format, ...)
{
    char why[USB_DETAIL_MAX];
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
    etude_assert(u->c, id, held, why);
}

const char *usb_copy_line(const char *line, char *buf, size_t size)
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

void usb_probe(struct usb_case *u, const char *id, const struct usb_run *r, const char *token)
{
    char ok[64], bad[64], line[USB_DETAIL_MAX];
    const char *f;

    snprintf(ok, sizeof(ok), "%s OK", token);
    snprintf(bad, sizeof(bad), "%s FAIL", token);
    if (r->text == NULL) {
        usb_assert(u, id, 0, "the run did not return");
        return;
    }
    if ((f = usb_find(r->text, bad)) != NULL) {
        usb_assert(u, id, 0, "%s", usb_copy_line(f, line, sizeof(line)));
    } else if (r->signal != 0) {
        usb_assert(u, id, 0, "killed by signal %d", r->signal);
    } else if (r->status != 0) {
        usb_assert(u, id, 0, "exited with status %d", r->status);
    } else {
        usb_assert(u, id, usb_find(r->text, ok) != NULL, "no %s", ok);
    }
}

int usb_end(const struct usb_case *u)
{
    return u->st == ETUDE_OK ? 0 : 1;
}

enum etude_status usb_boot(struct usb_boot *b, struct usb_case *u, const char *machine, unsigned n,
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
static enum etude_status wait_from(struct usb_boot *b, const char *text, uint64_t from,
                                   uint64_t *at)
{
    enum etude_status st;

    if (b->u->st != ETUDE_OK)
        return b->u->st;
    st = etude_console_wait(b->m, text, from, at);
    return failed(b->u, st, "waiting for \"%s\": %s", text, status_text(st));
}

enum etude_status usb_wait(struct usb_boot *b, const char *text)
{
    return wait_from(b, text, b->at, &b->at);
}

enum etude_status usb_seen(struct usb_boot *b, const char *text)
{
    uint64_t at;

    return wait_from(b, text, 0, &at);
}

enum etude_status usb_after(struct usb_boot *b, const char *text, uint64_t from, uint64_t *at)
{
    *at = from;
    return wait_from(b, text, from, at);
}

enum etude_status usb_times(struct usb_boot *b, const char *text, unsigned n, uint64_t *at)
{
    unsigned i;

    *at = 0;
    for (i = 0; i < n; i++) {
        if (wait_from(b, text, *at, at) != ETUDE_OK)
            return b->u->st;
    }
    return b->u->st;
}

enum etude_status usb_end_of(struct usb_boot *b, struct etude_termination *end)
{
    enum etude_status st;

    end->end = ETUDE_END_LOST;
    end->value = -1;
    end->console_offset = 0;
    if (b->u->st != ETUDE_OK)
        return b->u->st;
    st = etude_wait_end(b->m, end);
    return failed(b->u, st, "waiting for the end: %s", status_text(st));
}

enum etude_status usb_line(struct usb_boot *b, const char *line)
{
    enum etude_status st;
    char text[512];

    if (b->u->st != ETUDE_OK)
        return b->u->st;
    snprintf(text, sizeof(text), "%s\r", line);
    st = etude_type(b->m, text);
    return failed(b->u, st, "typing \"%s\": %s", line, status_text(st));
}

/** Give the arguments `format` and `ap` make in `buf` of USB_ARGS_MAX
 * bytes; -1 when they do not fit. */
static int arguments(char *buf, const char *format, va_list ap)
{
    int n = vsnprintf(buf, USB_ARGS_MAX, format, ap);

    return n < 0 || n >= USB_ARGS_MAX ? -1 : 0;
}

/** Send QMP `execute` with `args`, requiring `return`; with `empty`, the
 * monitor command's output must be empty too. */
static enum etude_status qmp(struct usb_boot *b, const char *execute, const char *args, int empty)
{
    enum etude_status st;
    char *reply = NULL, line[USB_DETAIL_MAX];

    if (b->u->st != ETUDE_OK)
        return b->u->st;
    st = etude_qmp(b->m, execute, args, &reply);
    if (st == ETUDE_OK && empty && reply != NULL && strstr(reply, "\"return\": \"\"") == NULL &&
        strstr(reply, "\"return\":\"\"") == NULL)
        st = ETUDE_FAILED;
    if (st != ETUDE_OK)
        failed(b->u, st, "%s %s: %s", execute, args,
               reply != NULL ? usb_copy_line(reply, line, sizeof(line)) : status_text(st));
    free(reply);
    return st;
}

enum etude_status usb_qmp(struct usb_boot *b, const char *execute, const char *format, ...)
{
    char args[USB_ARGS_MAX];
    va_list ap;
    int bad;

    va_start(ap, format);
    bad = arguments(args, format, ap);
    va_end(ap);
    if (bad)
        return failed(b->u, ETUDE_FAILED, "%s: the arguments are too long", execute);
    return qmp(b, execute, args, 0);
}

/** Run the monitor command `command` through QMP, requiring no output. */
static enum etude_status hmp(struct usb_boot *b, const char *command)
{
    char args[USB_ARGS_MAX];

    snprintf(args, sizeof(args), "{\"command-line\": \"%s\"}", command);
    return qmp(b, "human-monitor-command", args, 1);
}

enum etude_status usb_keys(struct usb_boot *b, const char *keys)
{
    char command[64];
    const char *p = keys;
    size_t n;

    while (*p != '\0') {
        while (*p == ' ')
            p++;
        n = strcspn(p, " ");
        if (n == 0)
            break;
        if (n >= sizeof(command) - 9)
            return failed(b->u, ETUDE_FAILED, "a key name is too long: %s", p);
        snprintf(command, sizeof(command), "sendkey %.*s", (int)n, p);
        if (hmp(b, command) != ETUDE_OK)
            return b->u->st;
        p += n;
    }
    return b->u->st;
}

enum etude_status usb_mouse(struct usb_boot *b, int dx, int dy, int buttons)
{
    char command[64];

    snprintf(command, sizeof(command), "mouse_move %d %d", dx, dy);
    if (hmp(b, command) != ETUDE_OK || buttons == 0)
        return b->u->st;
    snprintf(command, sizeof(command), "mouse_button %d", buttons);
    return hmp(b, command);
}

/** Copy `s` into `buf` of `size` bytes as the inside of a JSON string; -1
 * when it does not fit or has a byte JSON would need escaped otherwise. */
static int json_quote(char *buf, size_t size, const char *s)
{
    size_t n = 0;

    for (; *s != '\0'; s++) {
        if ((unsigned char)*s < ' ')
            return -1;
        if ((*s == '"' || *s == '\\') && n + 1 < size)
            buf[n++] = '\\';
        if (n + 1 >= size)
            return -1;
        buf[n++] = *s;
    }
    buf[n] = '\0';
    return 0;
}

enum etude_status usb_plug(struct usb_boot *b, const char *model, const char *bus, const char *id,
                           const char *more)
{
    return usb_qmp(b, "device_add", "{\"driver\": \"%s\", \"bus\": \"%s\", \"id\": \"%s\"%s%s}",
                   model, bus, id, more != NULL ? ", " : "", more != NULL ? more : "");
}

enum etude_status usb_pull(struct usb_boot *b, const char *id)
{
    return usb_qmp(b, "device_del", "{\"id\": \"%s\"}", id);
}

enum etude_status usb_medium(struct usb_boot *b, const char *node, const char *path, int readonly)
{
    char quoted[USB_ARGS_MAX / 2];

    if (path == NULL || json_quote(quoted, sizeof(quoted), path) != 0)
        return failed(b->u, ETUDE_FAILED, "medium %s: no path to give QEMU", node);
    return usb_qmp(b, "blockdev-add",
                   "{\"driver\": \"raw\", \"node-name\": \"%s\", \"read-only\": %s, "
                   "\"file\": {\"driver\": \"file\", \"filename\": \"%s\", \"locking\": \"off\"}}",
                   node, readonly ? "true" : "false", quoted);
}

uint64_t usb_start(struct usb_boot *b, const char *command)
{
    char line[512];

    snprintf(line, sizeof(line), "exec %s", command);
    usb_line(b, line);
    return b->at;
}

/** Give the number after the last `prefix` of `text`, or -1. */
static int number_after(const char *text, const char *prefix)
{
    const char *p, *last = NULL;

    for (p = strstr(text, prefix); p != NULL; p = strstr(p + 1, prefix))
        last = p;
    if (last == NULL)
        return -1;
    last += strlen(prefix);
    if (*last < '0' || *last > '9')
        return -1;
    return atoi(last);
}

enum etude_status usb_finish(struct usb_boot *b, uint64_t from, struct usb_run *r)
{
    uint64_t end;

    r->text = NULL;
    r->status = -1;
    r->signal = 0;
    if (usb_wait(b, "Init process exited.") != ETUDE_OK)
        return b->u->st;
    end = b->at;
    r->text = usb_text(b, from, end);
    r->status = number_after(r->text, "exited with status ");
    r->signal = number_after(r->text, "killed by signal ");
    if (r->signal < 0)
        r->signal = 0;
    return usb_wait(b, USB_SHELL);
}

enum etude_status usb_exec(struct usb_boot *b, const char *command, struct usb_run *r)
{
    return usb_finish(b, usb_start(b, command), r);
}

enum etude_status usb_host(struct usb_case *u, char *const argv[], const char *output)
{
    struct etude_termination end;
    enum etude_status st;

    if (u->st != ETUDE_OK)
        return u->st;
    st = etude_host(u->c, argv, output, &end);
    if (st != ETUDE_OK)
        return failed(u, st, "%s: %s", argv[0], status_text(st));
    if (end.end != ETUDE_END_EXITED || end.value != 0)
        return failed(u, ETUDE_FAILED, "%s: ended %s %d", argv[0],
                      end.end == ETUDE_END_EXITED ? "with status" : "by signal", end.value);
    return ETUDE_OK;
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

char *usb_text(const struct usb_boot *b, uint64_t from, uint64_t to)
{
    return read_part(b->console, from, to > from ? to - from : 0);
}

char *usb_file(const char *path)
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

int usb_match(const char *line, size_t len, const char *pattern)
{
    size_t i;

    for (i = 0; i <= len; i++) {
        if (match_here(line + i, len - i, pattern))
            return 1;
    }
    return 0;
}

/** Give the length of the line at `p`, up to its carriage return or
 * newline. */
static size_t line_length(const char *p)
{
    return strcspn(p, "\r\n");
}

unsigned usb_count(const char *text, const char *pattern)
{
    unsigned n = 0;
    const char *p;
    size_t len;

    if (text == NULL)
        return 0;
    for (p = text; *p != '\0'; p += strspn(p, "\r\n")) {
        len = line_length(p);
        if (usb_match(p, len, pattern))
            n++;
        p += len;
    }
    return n;
}

const char *usb_find(const char *text, const char *pattern)
{
    const char *p;
    size_t len;

    if (text == NULL)
        return NULL;
    for (p = text; *p != '\0'; p += strspn(p, "\r\n")) {
        len = line_length(p);
        if (usb_match(p, len, pattern))
            return p;
        p += len;
    }
    return NULL;
}

const char *usb_last(const char *text, const char *pattern)
{
    const char *p, *last = NULL;
    size_t len;

    if (text == NULL)
        return NULL;
    for (p = text; *p != '\0'; p += strspn(p, "\r\n")) {
        len = line_length(p);
        if (usb_match(p, len, pattern))
            last = p;
        p += len;
    }
    return last;
}
