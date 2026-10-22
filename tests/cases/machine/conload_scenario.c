/*
 * conload_scenario.c: the scenario of the conload case
 * (../machine.conload.json). conloadprobe loads the machine from the debug
 * shell and reads numbered, checksummed lines in four stages, each typed
 * once the probe announces the stage; the scenario judges the probe's lines
 * and the record that hands the shell back.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lifecycle.h"

/* The lines of each stage, numbered from 0 across the stages. */
#define LINES_A 200
#define LINES_B 60
#define LINES_C 60
#define LINES_D 40

/* One line: `L NNNN `, 20 bytes of payload, a space, eight hex digits and a
 * newline. */
#define LINE_BYTES 37

/* The prefix of every line of the probe's that the scenario reads. */
static const char tag[] = "CONLOAD ";

/**
 * Give `count` lines from line `first`, from malloc, as the probe requires
 * them: `L NNNN payload SUM`, the payload 20 letters and digits that follow
 * from N, and SUM the Adler-32 of the text before it in eight hex digits.
 */
static char *lines(unsigned first, unsigned count)
{
    static const char alphabet[] = "abcdefghijklmnopqrstuvwxyz0123456789";
    char *text = malloc((size_t)count * LINE_BYTES + 1), *p = text;
    unsigned n, i, a, b;

    if (text == NULL)
        return NULL;
    for (n = first; n < first + count; n++) {
        sprintf(p, "L %04u ", n);
        for (i = 0; i < 20; i++)
            p[7 + i] = alphabet[(n + i * 7) % 36];
        for (a = 1, b = 0, i = 0; i < 27; i++) {
            a = (a + (unsigned char)p[i]) % 65521;
            b = (b + a) % 65521;
        }
        sprintf(p + 27, " %08x\n", b * 65536 + a);
        p += LINE_BYTES;
    }
    return text;
}

/** Type `count` lines from line `first`. @return 1 when they were typed. */
static int type_lines(struct lc *l, struct etude_case_machine *m, unsigned first, unsigned count)
{
    char *text = lines(first, count);
    int typed;

    if (text == NULL) {
        lc_halt(l, ETUDE_FAILED, "the lines to type");
        return 0;
    }
    typed = lc_type(l, m, text);
    free(text);
    return typed;
}

/**
 * Read the probe's lines after `*at` until one begins with `want`; the rest
 * of a line may be a number, or bytes the console echoed into it, since the
 * probe writes a line in more than one piece. A line that says FAIL comes
 * before it when the probe failed, and then assertion `id`, the stage's,
 * fails with it; when `done`, the line ends the stage, and `id` holds when
 * it came.
 *
 * @return 1 when the line came.
 */
static int await(struct lc *l, struct etude_case_machine *m, uint64_t *at, const char *want,
                 const char *id, int done)
{
    uint64_t start;
    char *line, detail[160];
    int found = 0, failed = 0;

    while (!found && !failed) {
        if (!lc_wait(l, m, tag, at, NULL))
            return 0;
        start = *at;
        if (!lc_wait(l, m, "\n", at, NULL) || (line = lc_console(l, "guest", start, *at)) == NULL)
            return 0;
        line[strcspn(line, "\r\n")] = '\0';
        found = strncmp(line, want, strlen(want)) == 0;
        failed = strncmp(line, "FAIL", 4) == 0;
        if (failed) {
            snprintf(detail, sizeof(detail), "the probe says %s%.120s", tag, line);
            lc_assert(l, id, 0, detail);
        }
        free(line);
    }
    if (found && done)
        lc_assert(l, id, 1, NULL);
    return found;
}

/**
 * Report assertion `writer` from window `w`: the probe printed how many files
 * its writer completed exactly once for each of the stages A, B and C, the
 * stages under load. Each count is printed for the record.
 */
static void writer_counts(struct lc *l, const char *w)
{
    static const char row[] = "CONLOAD writer files in stage ";
    const char *stages = "ABC", *p;
    int seen[3] = {0, 0, 0}, i, once = 1;
    char *end;
    long files;

    for (p = w; (p = strstr(p, row)) != NULL; p += sizeof(row) - 1) {
        for (i = 0; i < 3; i++)
            if (p[sizeof(row) - 1] == stages[i] && p[sizeof(row)] == ':') {
                files = strtol(p + sizeof(row) + 1, &end, 10);
                if (end != p + sizeof(row) + 1) {
                    seen[i]++;
                    printf("conload: the writer completed %ld files in stage %c\n", files,
                           stages[i]);
                }
            }
    }
    for (i = 0; i < 3; i++)
        once = once && seen[i] == 1;
    lc_assert(l, "writer", once, "the probe did not print one writer count for each of A, B and C");
}

int etude_scenario(struct etude_case *c)
{
    static const char *const ids[] = {"stage-a", "stage-b", "stage-c", "stage-d",
                                      "all",     "writer",  "exit",    NULL};
    struct etude_case_machine *m;
    struct lc l;
    uint64_t at = 0, from;
    char *w;
    int going;

    lc_begin(&l, c, ids);
    if (!lc_step(&l, "boot") || (m = lc_boot(&l, "guest")) == NULL ||
        !lc_wait(&l, m, "Type 'help'", &at, NULL) || !lc_wait(&l, m, "\n> ", &at, NULL) ||
        !lc_step(&l, "load"))
        return lc_finish(&l);
    from = at;
    going = lc_type(&l, m, "exec /conloadprobe\n") && await(&l, m, &at, "A", "stage-a", 0) &&
            type_lines(&l, m, 0, LINES_A) && await(&l, m, &at, "A OK ms=", "stage-a", 1) &&
            await(&l, m, &at, "B", "stage-b", 0) && type_lines(&l, m, LINES_A, LINES_B) &&
            await(&l, m, &at, "B OK", "stage-b", 1) && await(&l, m, &at, "C", "stage-c", 0) &&
            type_lines(&l, m, 0, LINES_A) && lc_type(&l, m, "\x03") &&
            type_lines(&l, m, LINES_A + LINES_B, LINES_C) &&
            await(&l, m, &at, "C OK", "stage-c", 1) && await(&l, m, &at, "D", "stage-d", 0) &&
            type_lines(&l, m, LINES_A + LINES_B + LINES_C, LINES_D) &&
            await(&l, m, &at, "D OK", "stage-d", 1);
    if (going)
        await(&l, m, &at, "OK lines=360", "all", 1);
    if ((w = lc_handback(&l, m, "guest", from, &at, "exit")) != NULL) {
        writer_counts(&l, w);
        free(w);
    }
    lc_quit(&l, m, NULL);
    return lc_finish(&l);
}
