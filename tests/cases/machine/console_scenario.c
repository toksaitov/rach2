/*
 * console_scenario.c: the scenario of the console cases
 * (../machine.console.json), stdintest and ttyprobe run at the debug shell
 * and typed at once each says that it reads.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lifecycle.h"

/* The machine a case types at, and the offset its console has been read to. */
struct guest {
    struct lc *l;
    struct etude_case_machine *m;
    uint64_t at;
};

/** Give the last part of the attempt's case identity. */
static const char *case_name(struct etude_case *c)
{
    const char *id = etude_case_id(c), *dot = strrchr(id, '.');

    return dot != NULL ? dot + 1 : id;
}

/**
 * Wait for `tag` and then for `end` after it, and report assertion `id`: it
 * holds when the bytes between them are `want`.
 */
static void framed(struct guest *g, const char *tag, const char *end, const char *want,
                   const char *id)
{
    uint64_t start;
    char *text, detail[160];

    if (!lc_wait(g->l, g->m, tag, &g->at, NULL))
        return;
    start = g->at;
    if (!lc_wait(g->l, g->m, end, &g->at, NULL) ||
        (text = lc_console(g->l, "guest", start, g->at - strlen(end))) == NULL)
        return;
    snprintf(detail, sizeof(detail), "the probe printed %s%.80s, not %s%s", tag, text, tag, want);
    lc_assert(g->l, id, strcmp(text, want) == 0, detail);
    free(text);
}

/**
 * Wait for the probe's request `ask`, type `line` and report assertion `id`
 * from the line the probe prints of what its read gave, which must be
 * `Read ` and `answer`.
 */
static void exchange(struct guest *g, const char *ask, const char *line, const char *answer,
                     const char *id)
{
    if (lc_wait(g->l, g->m, ask, &g->at, NULL) && lc_type(g->l, g->m, line))
        framed(g, "Read ", "\r\n", answer, id);
}

/**
 * Run stdintest: three lines typed, each once the probe asks for it, the
 * third longer than the five bytes its read takes.
 */
static void stdin_case(struct guest *g)
{
    uint64_t from = g->at;
    char *w;

    if (!lc_type(g->l, g->m, "exec /stdintest\n"))
        return;
    exchange(g, "Test 1: read from stdin", "alpha\n", "5 bytes: \"alpha\"", "first");
    exchange(g, "Test 2: second read", "bravo line\n", "10 bytes: \"bravo line\"", "second");
    exchange(g, "Test 3: small buffer read", "charlie-delta\n", "5 bytes: \"charl\"", "small");
    if ((w = lc_handback(g->l, g->m, "guest", from, &g->at, "exit")) != NULL) {
        lc_assert(g->l, "passed", strstr(w, "All stdin syscalls passed!") != NULL,
                  "stdintest does not say that all its reads passed");
        free(w);
    }
}

/**
 * Run ttyprobe: a cooked line with two erases, a raw byte once the probe has
 * made the console raw, and Alt with x pressed through QMP once it reads the
 * keyboard. Its checks of the window and the screen's answers need nothing
 * typed; any check that fails prints a line that begins `  FAIL: `.
 */
static void tty_case(struct guest *g)
{
    static const char alt[] = "{\"keys\": [{\"type\": \"qcode\", \"data\": \"alt\"}, {\"type\": "
                              "\"qcode\", \"data\": \"x\"}]}";
    uint64_t from = g->at;
    enum etude_status st;
    char *reply = NULL, *w;

    if (!lc_type(g->l, g->m, "exec /ttyprobe\n") ||
        !lc_wait(g->l, g->m, "TTYPROBE START", &g->at, NULL) ||
        !lc_type(g->l, g->m,
                 "ab\x7f\x7f"
                 "cd\n"))
        return;
    framed(g, "COOKED:[", "]\r\n", "cd\r\n", "cooked");
    if (!lc_wait(g->l, g->m, "TTYPROBE RAW", &g->at, NULL) || !lc_type(g->l, g->m, "X"))
        return;
    framed(g, "RAW:[", "]\r\n", "X", "raw");
    if (!lc_wait(g->l, g->m, "TTYPROBE ALT", &g->at, NULL))
        return;
    st = etude_qmp(g->m, "send-key", alt, &reply);
    free(reply);
    if (st != ETUDE_OK) {
        lc_halt(g->l, st, "QMP's send-key");
        return;
    }
    if ((w = lc_handback(g->l, g->m, "guest", from, &g->at, "exit")) != NULL) {
        lc_assert(g->l, "alt",
                  strstr(w, "FAIL: Alt with x") == NULL && strstr(w, "TTYPROBE OK") != NULL,
                  "ttyprobe did not read Alt with x as ESC x, or did not pass");
        lc_assert(g->l, "no-fail",
                  strstr(w, "  FAIL: ") == NULL && strstr(w, "TTYPROBE FAIL") == NULL,
                  "ttyprobe names a failed check");
        free(w);
    }
}

int etude_scenario(struct etude_case *c)
{
    static const char *const stdin_ids[] = {"first", "second", "small", "passed", "exit", NULL};
    static const char *const tty_ids[] = {"cooked", "raw", "alt", "no-fail", "exit", NULL};
    const char *name = case_name(c);
    int tty = strcmp(name, "tty") == 0;
    struct guest g;
    struct lc l;

    lc_begin(&l, c, tty ? tty_ids : stdin_ids);
    g.l = &l;
    g.at = 0;
    if ((tty || strcmp(name, "stdin") == 0) && lc_step(&l, "boot") &&
        (g.m = lc_boot(&l, "guest")) != NULL && lc_wait(&l, g.m, "Type 'help'", &g.at, NULL) &&
        lc_wait(&l, g.m, "\n> ", &g.at, NULL) && lc_step(&l, "read")) {
        if (tty)
            tty_case(&g);
        else
            stdin_case(&g);
        lc_quit(&l, g.m, NULL);
    }
    return lc_finish(&l);
}
