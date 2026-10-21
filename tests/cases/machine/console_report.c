/*
 * console_report.c: a probe's report records, read from the console of the
 * machine that ran it at the debug shell, made a machine scenario's own.
 */
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "console_report.h"
#include "etude/report.h"

/** Read the whole file `path` into a buffer the caller frees, its length in
 * `*len`; NULL when it cannot be read. */
static char *read_file(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    char *buf = NULL;
    size_t cap = 0, n = 0, got;

    if (f == NULL)
        return NULL;
    do {
        if (n == cap) {
            char *more = realloc(buf, cap = cap * 2 + 65536);

            if (more == NULL) {
                free(buf);
                fclose(f);
                return NULL;
            }
            buf = more;
        }
        got = fread(buf + n, 1, cap - n, f);
        n += got;
    } while (got > 0);
    if (ferror(f)) {
        free(buf);
        buf = NULL;
    }
    fclose(f);
    *len = n;
    return buf;
}

/**
 * Make one record `rest`, the text after the prefix, the case's own, and
 * count it in `*records` when `end` counts it. Return 0, or -1 when it does
 * not parse.
 */
static int adopt(struct etude_case *c, const char *rest, unsigned *records)
{
    char verb[16], id[ETUDE_REPORT_LINE_MAX], word[ETUDE_REPORT_LINE_MAX];
    int n = 0, k = 0;
    unsigned long long units;

    if (sscanf(rest, "%15s %254s%n", verb, id, &n) < 2)
        return -1;
    rest += n;
    if (strcmp(verb, ETUDE_REPORT_ASSERT) == 0) {
        if (sscanf(rest, " %254s%n", word, &k) < 1)
            return -1;
        rest += k;
        if (strcmp(word, ETUDE_REPORT_PASS) == 0 && *rest == '\0') {
            etude_assert(c, id, 1, NULL);
        } else if (strcmp(word, ETUDE_REPORT_FAIL) == 0) {
            etude_assert(c, id, 0, *rest == ' ' ? rest + 1 : "no detail");
        } else {
            return -1;
        }
        (*records)++;
    } else if (strcmp(verb, ETUDE_REPORT_NOTRUN) == 0) {
        etude_notrun(c, id, *rest == ' ' ? rest + 1 : "no reason");
        (*records)++;
    } else if (strcmp(verb, ETUDE_REPORT_BRANCH) == 0) {
        if (sscanf(rest, " %254s%n", word, &k) < 1 || rest[k] != '\0')
            return -1;
        etude_branch(c, id, word);
        (*records)++;
    } else if (strcmp(verb, ETUDE_REPORT_PROGRESS) == 0) {
        if (sscanf(rest, " %llu%n", &units, &k) < 1 || rest[k] != '\0')
            return -1;
        etude_progress(c, id, (uint64_t)units);
    } else if (strcmp(verb, ETUDE_REPORT_CHILD) != 0) {
        return -1;
    }
    return 0;
}

enum etude_status console_report(struct etude_case *c, struct etude_case_machine *m,
                                 const char *machine, unsigned boot, uint64_t from, uint64_t *at)
{
    static const char prefix[] = ETUDE_REPORT_PREFIX;
    const size_t plen = sizeof(prefix) - 1;
    char name[160];
    const char *path;
    char *text, *line, *stop;
    uint64_t end_at, eol;
    unsigned records = 0, counted;
    int begun = 0, ended = 0, bad = 0;
    enum etude_status st;
    size_t len;

    if ((st = etude_console_wait(m, ETUDE_REPORT_PREFIX ETUDE_REPORT_END " ", from, &end_at)) !=
            ETUDE_OK ||
        (st = etude_console_wait(m, "\n", end_at, &eol)) != ETUDE_OK)
        return st;
    snprintf(name, sizeof(name), "%s.%u.console", machine, boot);
    path = etude_evidence(c, name);
    if (path == NULL || (text = read_file(path, &len)) == NULL || len < eol) {
        fprintf(stderr, "console_report: %s: the console's file cannot be read\n", name);
        return ETUDE_FAILED;
    }
    text[eol - 1] = '\0';
    for (line = text + from; !ended && line < text + eol; line = stop + 1) {
        stop = strchr(line, '\n');
        if (stop == NULL)
            stop = text + eol - 1;
        *stop = '\0';
        if (stop > line && stop[-1] == '\r')
            stop[-1] = '\0';
        if (strncmp(line, prefix, plen) != 0)
            continue;
        line += plen;
        if (strncmp(line, ETUDE_REPORT_BEGIN " ", 6) == 0) {
            bad |= begun;
            begun = 1;
            records = 0;
        } else if (strncmp(line, ETUDE_REPORT_END " ", 4) == 0) {
            ended = 1;
            bad |= !begun || sscanf(line + 4, "%u", &counted) != 1 || counted != records;
        } else if (!begun || adopt(c, line, &records) != 0) {
            fprintf(stderr, "console_report: %s: a record out of place: %s\n", name, line);
            bad = 1;
        }
    }
    free(text);
    if (bad || !ended) {
        fprintf(stderr, "console_report: %s: the report is not whole\n", name);
        return ETUDE_FAILED;
    }
    if (at != NULL)
        *at = eol;
    return ETUDE_OK;
}
