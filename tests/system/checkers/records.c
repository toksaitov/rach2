/*
 * records.c: judge the kernel's counters without a host interpreter.
 * Grow measures must agree with the tool wrapper's independent readings.
 * USB records judge dispatch and work counts; their times are diagnostic.
 */
#include "records.h"
#include <errno.h>
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

/* Split a writable line on ASCII white space; -1 if the array is full. */
static int words(char *line, char **v, int cap)
{
    int n = 0;
    char *p = line;
    while (*p) {
        p += strspn(p, " \t\r\n");
        if (!*p)
            break;
        if (n == cap)
            return -1;
        v[n++] = p;
        p += strcspn(p, " \t\r\n");
        if (*p)
            *p++ = '\0';
    }
    return n;
}

/* Read one decimal counter, rejecting signs, overflow and trailing text. */
static int counter(const char *s, uint64_t *v)
{
    char *end;
    unsigned long long n;
    if (*s < '0' || *s > '9')
        return -1;
    errno = 0;
    n = strtoull(s, &end, 10);
    if (errno || *end)
        return -1;
    *v = (uint64_t)n;
    return 0;
}

/* Copy one line, advancing the input; NULL at its end or on allocation failure. */
static char *line_next(const char **text)
{
    size_t n;
    char *line;
    if (!**text)
        return NULL;
    n = strcspn(*text, "\n");
    line = malloc(n + 1);
    if (!line)
        return NULL;
    memcpy(line, *text, n);
    line[n] = '\0';
    *text += n + ((*text)[n] == '\n');
    return line;
}

static const char *const phases[] = {"table", "check-readonly", "check-repair", "resize", "whole"};

struct grow_records {
    uint64_t c[8], f[8], wc[10], wf[10];
    uint64_t time, commands, flushes;
    unsigned seen[10];
    int n, w, bad;
};

static int grow_measure(struct grow_records *s, char **v, int count, FILE *evidence)
{
    uint64_t t, cmd, fl;
    int n = s->n++;
    if (count != 10 || n >= 8 || strcmp(v[2], phases[n / 2]) ||
        strcmp(v[3], n % 2 ? "after" : "before") || strcmp(v[4], "monotonic-us") ||
        strcmp(v[6], "disk-commands") || strcmp(v[8], "disk-flushes") || counter(v[5], &t) ||
        counter(v[7], &cmd) || counter(v[9], &fl) || t < s->time || cmd < s->commands ||
        fl < s->flushes)
        return 0;
    s->c[n] = cmd;
    s->f[n] = fl;
    if (n % 2) {
        if (cmd <= s->commands)
            s->bad = 1;
        if (evidence)
            fprintf(evidence,
                    "grow-test: phase: %s %" PRIu64 " guest-tick-us %" PRIu64 " commands %" PRIu64
                    " flushes\n",
                    phases[n / 2], t - s->time, cmd - s->commands, fl - s->flushes);
    }
    s->time = t;
    s->commands = cmd;
    s->flushes = fl;
    return 1;
}

static int grow_witness(struct grow_records *s, char **v, int count)
{
    uint64_t cmd, fl;
    int i = 0, side, at;
    s->w++;
    if (count != 7)
        return 0;
    while (i < 5 && strcmp(v[1], phases[i]))
        i++;
    side = !strcmp(v[2], "before") ? 0 : !strcmp(v[2], "after") ? 1 : -1;
    at = 2 * i + side;
    if (i == 0 || i == 5 || side < 0 || strcmp(v[3], "disk-commands") ||
        strcmp(v[5], "disk-flushes") || counter(v[4], &cmd) || counter(v[6], &fl) || s->seen[at]++)
        return 0;
    s->wc[at] = cmd;
    s->wf[at] = fl;
    return 1;
}

static int grow_consistent(const struct grow_records *s)
{
    int n;
    if (s->bad || s->n != 8 || s->w != 8 || !s->seen[8] || !s->seen[9] || s->c[0] < s->wc[8] ||
        s->f[0] < s->wf[8] || s->c[7] != s->wc[9] || s->f[7] != s->wf[9])
        return 0;
    for (n = 2; n < 8; n++)
        if (!s->seen[n] || s->c[n] != s->wc[n] || s->f[n] != s->wf[n])
            return 0;
    return 1;
}

int check_grow_records(const char *text, FILE *evidence)
{
    struct grow_records s = {0};
    char *line;
    if (!text)
        return 0;
    while (*text && (line = line_next(&text)) != NULL) {
        char *v[16];
        int count = words(line, v, 16);
        if (count && !strcmp(v[0], "grow-witness:"))
            s.bad |= !grow_witness(&s, v, count);
        else if ((count > 1 || count < 0) && !strcmp(v[0], "growroot:") &&
                 !strcmp(v[1], "measure:"))
            s.bad |= !grow_measure(&s, v, count, evidence);
        free(line);
    }
    return !*text && grow_consistent(&s);
}

static int usb_value(const char *text, long long *value)
{
    char *end;
    const char *number = text;
    if (*number == '-')
        number++;
    if (*number < '0' || *number > '9')
        return 0;
    errno = 0;
    *value = strtoll(text, &end, 10);
    if (*end == ',')
        end++;
    return !errno && !*end;
}

static int usb_fields(char **v, int n, long long value[8])
{
    static const char *const fields[] = {"msi",     "entry",    "wake",  "read",
                                         "tracked", "dispatch", "pumps", "work"};
    unsigned count[8] = {0};
    int i, j, good = n >= 0;
    for (i = 2; i < n; i++) {
        for (j = 0; j < 8; j++) {
            if (strcmp(v[i], fields[j]))
                continue;
            if (++count[j] != 1 || i + 1 == n || !usb_value(v[i + 1], &value[j]))
                good = 0;
        }
    }
    for (j = 0; j < 8; j++)
        if (count[j] != 1)
            good = 0;
    return good;
}

static int usb_counts(const long long value[8], int random)
{
    return (value[0] == 0 || value[0] == 1) && value[4] == 1 && value[5] == 1 && value[6] >= 0 &&
           (random || value[6] == 0) && value[7] >= 0 && value[7] <= (random ? 4096 : 0);
}

int check_usb_records(const char *text, int random, unsigned keys)
{
    unsigned records = 0, interrupts = 0;
    int bad = 0;
    char *line;
    if (!text || !keys)
        return 0;
    while (*text && (line = line_next(&text)) != NULL) {
        char *start = strstr(line, "usb: latency:"), *v[64];
        long long value[8] = {0};
        if (start) {
            bad |= !usb_fields(v, words(start, v, 64), value);
            bad |= !usb_counts(value, random);
            interrupts += value[0] == 1;
            records++;
        }
        free(line);
    }
    return !*text && !bad && records == keys && (random || interrupts);
}
