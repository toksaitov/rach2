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

int check_grow_records(const char *text, FILE *evidence)
{
    static const char *const phases[] = {"table", "check-readonly", "check-repair", "resize",
                                         "whole"};
    uint64_t c[8] = {0}, f[8] = {0}, wc[10] = {0}, wf[10] = {0};
    uint64_t time = 0, commands = 0, flushes = 0;
    unsigned seen[10] = {0};
    int n = 0, w = 0, bad = text == NULL;
    char *line;
    if (!text)
        return 0;
    while (*text && (line = line_next(&text)) != NULL) {
        char *v[16];
        int record = strncmp(line, "growroot: measure:", 18) == 0 ||
                     strncmp(line, "grow-witness:", 13) == 0;
        int count = words(line, v, 16), i, side, at;
        uint64_t t = 0, cmd = 0, fl = 0;
        if (record && count < 0)
            bad = 1;
        if (count > 0 && strcmp(v[0], "growroot:") == 0 && count > 1 &&
            strcmp(v[1], "measure:") == 0) {
            if (count != 10 || n >= 8 || strcmp(v[2], phases[n / 2]) ||
                strcmp(v[3], n % 2 ? "after" : "before") ||
                strcmp(v[4], "monotonic-us") || strcmp(v[6], "disk-commands") ||
                strcmp(v[8], "disk-flushes") || counter(v[5], &t) ||
                counter(v[7], &cmd) || counter(v[9], &fl) || t < time ||
                cmd < commands || fl < flushes) {
                bad = 1;
            } else {
                c[n] = cmd;
                f[n] = fl;
                if (n % 2) {
                    if (cmd <= commands)
                        bad = 1;
                    if (evidence)
                        fprintf(evidence, "grow-test: phase: %s %" PRIu64
                                " guest-tick-us %" PRIu64 " commands %" PRIu64 " flushes\n",
                                phases[n / 2], t - time, cmd - commands, fl - flushes);
                }
                time = t;
                commands = cmd;
                flushes = fl;
            }
            n++;
        } else if (count > 0 && strcmp(v[0], "grow-witness:") == 0) {
            i = 0;
            if (count != 7) {
                bad = 1;
            } else {
                while (i < 5 && strcmp(v[1], phases[i]))
                    i++;
                side = strcmp(v[2], "before") == 0 ? 0 : strcmp(v[2], "after") == 0 ? 1 : -1;
                at = 2 * i + side;
                if (i == 0 || i == 5 || side < 0 || strcmp(v[3], "disk-commands") ||
                    strcmp(v[5], "disk-flushes") || counter(v[4], &cmd) ||
                    counter(v[6], &fl) || seen[at]++) {
                    bad = 1;
                } else {
                    wc[at] = cmd;
                    wf[at] = fl;
                }
            }
            w++;
        }
        free(line);
    }
    if (*text || n != 8 || w != 8 || !seen[8] || !seen[9] ||
        c[0] < wc[8] || f[0] < wf[8] || c[7] != wc[9] || f[7] != wf[9])
        bad = 1;
    for (n = 2; n < 8; n++)
        if (!seen[n] || c[n] != wc[n] || f[n] != wf[n])
            bad = 1;
    return !bad;
}

int check_usb_records(const char *text, int random, unsigned keys)
{
    static const char *const fields[] = {"msi", "entry", "wake", "read", "tracked",
                                         "dispatch", "pumps", "work"};
    unsigned records = 0, interrupts = 0;
    int bad = 0;
    char *line;
    if (!text || !keys)
        return 0;
    while (*text && (line = line_next(&text)) != NULL) {
        char *start = strstr(line, "usb: latency:"), *v[64];
        long long value[8] = {0};
        unsigned count[8] = {0};
        int n, i, j;
        if (!start) {
            free(line);
            continue;
        }
        n = words(start, v, 64);
        if (n < 0)
            bad = 1;
        for (i = 2; i < n; i++) {
            for (j = 0; j < 8; j++) {
                char *end, *number;
                if (strcmp(v[i], fields[j]))
                    continue;
                if (++count[j] != 1 || i + 1 == n) {
                    bad = 1;
                    continue;
                }
                number = v[i + 1];
                if (*number == '-')
                    number++;
                if (*number < '0' || *number > '9') {
                    bad = 1;
                    continue;
                }
                errno = 0;
                value[j] = strtoll(v[i + 1], &end, 10);
                if (*end == ',')
                    end++;
                if (errno || *end)
                    bad = 1;
            }
        }
        for (j = 0; j < 8; j++)
            if (count[j] != 1)
                bad = 1;
        if ((value[0] != 0 && value[0] != 1) || value[4] != 1 || value[5] != 1 ||
            value[6] < 0 || (!random && value[6] != 0) || value[7] < 0 ||
            value[7] > (random ? 4096 : 0))
            bad = 1;
        interrupts += value[0] == 1;
        records++;
        free(line);
    }
    return !*text && !bad && records == keys && (random || interrupts);
}
