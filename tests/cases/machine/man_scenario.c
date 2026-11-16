/*
 * man_scenario.c: the scenario of the man cases (../machine.man.json). On
 * os-real, logged in as root, `man` must give every staged page and every
 * alias's page byte for byte through a pipe, and rach2(7) on the console
 * under the pager; on the recovery image, every page it stages. What the
 * boots must count, and rach2(7)'s first and last lines, are the summary the
 * build writes from the rendered pages.
 */
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lifecycle.h"

/* Every staged page by its own name, and every alias of the table against its
 * target's page, each loop one line typed at the shell. Only man's copy goes
 * to /tmp, on the RAM root; the staged page reaches cmp through a pipe. A page
 * that differs prints `BAD /...`; the alias line writes its slash in quotes,
 * so that the console's echo of the command does not have the text. */
static const char start[] = "n=0; M=/usr/share/man\r";
static const char pages[] =
    "for f in $M/cat*/*; do b=${f##*/}; man ${b##*.} ${b%.*} > /tmp/p; "
    "gunzip -c $f | cmp -s - /tmp/p && test -s /tmp/p || echo BAD $f; n=$((n+1)); done\r";
static const char aliases[] =
    "while read -r a s t; do case $a in '#'*|'') continue;; esac; man $s $a > /tmp/p; "
    "gunzip -c $M/cat$s/$t.$s | cmp -s - /tmp/p && test -s /tmp/p || "
    "echo BAD \"/\"alias/$a; n=$((n+1)); done < $M/aliases\r";
static const char total[] = "echo MAN\"\"ALL $n\r";

/* What the summary says. */
struct summary {
    long pages, aliases, recovery;
    char first[256], last[256];
};

/** Give the last part of the attempt's case identity. */
static const char *case_name(struct etude_case *c)
{
    const char *id = etude_case_id(c), *dot = strrchr(id, '.');

    return dot != NULL ? dot + 1 : id;
}

/** Read artifact `summary` into `s`. @return 1 when each of its five lines was
 * there. */
static int read_summary(struct lc *l, struct summary *s)
{
    const char *path = etude_artifact(l->c, "summary");
    FILE *f = path != NULL ? fopen(path, "r") : NULL;
    char line[300];
    int seen = 0;

    memset(s, 0, sizeof(*s));
    while (f != NULL && fgets(line, sizeof(line), f) != NULL) {
        line[strcspn(line, "\n")] = '\0';
        if (sscanf(line, "pages %ld", &s->pages) == 1)
            seen |= 1;
        else if (sscanf(line, "aliases %ld", &s->aliases) == 1)
            seen |= 2;
        else if (sscanf(line, "recovery %ld", &s->recovery) == 1)
            seen |= 4;
        else if (strncmp(line, "first ", 6) == 0 &&
                 snprintf(s->first, sizeof(s->first), "%s", line + 6) > 0)
            seen |= 8;
        else if (strncmp(line, "last ", 5) == 0 &&
                 snprintf(s->last, sizeof(s->last), "%s", line + 5) > 0)
            seen |= 16;
    }
    if (f != NULL)
        fclose(f);
    if (seen != 31 || s->pages <= 0 || s->first[0] == '\0' || s->last[0] == '\0') {
        lc_halt(l, ETUDE_FAILED, "the read of the summary of the pages");
        return 0;
    }
    return 1;
}

/** Read the password artifact and type it with a carriage return. */
static int type_password(struct lc *l, struct etude_case_machine *m)
{
    const char *path = etude_artifact(l->c, "password");
    char password[128], line[140];
    FILE *f = path != NULL ? fopen(path, "r") : NULL;
    int read = f != NULL && fgets(password, sizeof(password), f) != NULL;

    if (f != NULL)
        fclose(f);
    if (!read) {
        lc_halt(l, ETUDE_FAILED, "the read of the password");
        return 0;
    }
    password[strcspn(password, "\r\n")] = '\0';
    snprintf(line, sizeof(line), "%s\r", password);
    return lc_type(l, m, line);
}

/**
 * Type the commands that read the pages, and with `with_aliases` the
 * aliases, and the count, on machine `machine`, `m`, whose console has been
 * read to `*at`; report assertion `count`, that the count is `want`, and
 * `pipe`, that no page differed.
 */
static void read_pages(struct lc *l, struct etude_case_machine *m, const char *machine,
                       uint64_t *at, int with_aliases, long want)
{
    uint64_t from = *at;
    char *w, *p, detail[120];
    long n;

    if (!lc_type(l, m, start) || !lc_type(l, m, pages) ||
        (with_aliases && !lc_type(l, m, aliases)) || !lc_type(l, m, total) ||
        !lc_wait(l, m, "MANALL ", at, NULL) || !lc_wait(l, m, "\n", at, NULL) ||
        (w = lc_console(l, machine, from, *at)) == NULL)
        return;
    p = strstr(w, "MANALL ");
    n = p != NULL ? strtol(p + 7, NULL, 10) : -1;
    snprintf(detail, sizeof(detail), "man read %ld pages, not %ld", n, want);
    lc_assert(l, "count", n == want, detail);
    p = strstr(w, "BAD /");
    snprintf(detail, sizeof(detail), "man gave a page that differs: %.80s", p != NULL ? p : "");
    lc_assert(l, "pipe", p == NULL, detail);
    free(w);
}

/**
 * On os-real: log in, read every page and alias through a pipe, then
 * rach2(7) under the pager, its first line and, once the pager is sent to
 * the end, its last; then with `-P`, a pager of the command's own whose
 * options are kept, which shows the last line in capitals before the shell
 * runs the command after it.
 */
static void real(struct lc *l, const struct summary *s)
{
    struct etude_case_machine *m;
    char upper[256], *p, *w;
    uint64_t at = 0, from;

    if (!lc_step(l, "boot") || (m = lc_boot(l, "real")) == NULL ||
        !lc_wait(l, m, "login:", &at, NULL) || !lc_type(l, m, "root\r") ||
        !lc_wait(l, m, "Password:", &at, NULL) || !type_password(l, m) ||
        !lc_wait(l, m, "# ", &at, NULL) || !lc_step(l, "read"))
        return;
    read_pages(l, m, "real", &at, 1, s->pages + s->aliases);
    if (!lc_step(l, "pager") || !lc_type(l, m, "man rach2\r") ||
        !lc_wait(l, m, s->first, &at, "first") || !lc_type(l, m, "G") ||
        !lc_wait(l, m, s->last, &at, "last") || !lc_type(l, m, "q"))
        return;
    snprintf(upper, sizeof(upper), "%s", s->last);
    for (p = upper; *p != '\0'; p++)
        *p = (char)toupper((unsigned char)*p);
    from = at;
    if (!lc_type(l, m, "man -P \"tr '[:lower:]' '[:upper:]'\" rach2; echo PAGER\"\"DONE\r") ||
        !lc_wait(l, m, "PAGERDONE", &at, NULL) || (w = lc_console(l, "real", from, at)) == NULL)
        return;
    lc_assert(l, "named-pager", strstr(w, upper) != NULL,
              "man -P did not run the pager with its options");
    free(w);
    lc_quit(l, m, NULL);
}

/** On the recovery image, whose console is a shell without a login: read
 * every page it stages through a pipe. */
static void recovery(struct lc *l, const struct summary *s)
{
    struct etude_case_machine *m;
    uint64_t at = 0;

    if (!lc_step(l, "boot") || (m = lc_boot(l, "recovery")) == NULL ||
        !lc_wait(l, m, "recovery image", &at, NULL) || !lc_wait(l, m, "# ", &at, NULL) ||
        !lc_step(l, "read"))
        return;
    read_pages(l, m, "recovery", &at, 0, s->recovery);
    lc_quit(l, m, NULL);
}

int etude_scenario(struct etude_case *c)
{
    static const char *const real_ids[] = {"count", "pipe", "first", "last", "named-pager", NULL};
    static const char *const recovery_ids[] = {"count", "pipe", NULL};
    int on_real = strcmp(case_name(c), "real") == 0;
    struct summary s;
    struct lc l;

    lc_begin(&l, c, on_real ? real_ids : recovery_ids);
    if (read_summary(&l, &s)) {
        if (on_real)
            real(&l, &s);
        else
            recovery(&l, &s);
    }
    return lc_finish(&l);
}
