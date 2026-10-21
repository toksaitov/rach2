/*
 * lifecycle.c: the code the scenarios of machine.crash, machine.reset and
 * machine.shutdown share, over libetude-case.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "history_json.h"
#include "lifecycle.h"

/** Give the name of status `st`. */
static const char *status_name(enum etude_status st)
{
    switch (st) {
    case ETUDE_OK:
        return "ok";
    case ETUDE_FAILED:
        return "failed";
    case ETUDE_LOST:
        return "lost";
    case ETUDE_BOUND:
        return "past the step's bound";
    case ETUDE_STOPPED:
        return "stopped";
    case ETUDE_REFUSED:
        return "refused";
    }
    return "unknown";
}

/** Give the name of end `e`. */
static const char *end_name(enum etude_end e)
{
    switch (e) {
    case ETUDE_END_EXITED:
        return "exited";
    case ETUDE_END_SIGNALED:
        return "signaled";
    case ETUDE_END_INIT_EXITED:
        return "init_exited";
    case ETUDE_END_INIT_KILLED:
        return "init_killed";
    case ETUDE_END_PANIC:
        return "panic";
    case ETUDE_END_RESET:
        return "reset";
    case ETUDE_END_SHUTDOWN:
        return "shutdown";
    case ETUDE_END_HALTED:
        return "halted";
    case ETUDE_END_QEMU_EXITED:
        return "qemu_exited";
    case ETUDE_END_ACCEL_FAILED:
        return "accel_failed";
    case ETUDE_END_STOPPED:
        return "stopped";
    case ETUDE_END_LOST:
        return "lost";
    }
    return "unknown";
}

void lc_begin(struct lc *l, struct etude_case *c, const char *const ids[])
{
    size_t i;

    memset(l, 0, sizeof(*l));
    l->c = c;
    for (i = 0; i < LC_ASSERTS && ids[i] != NULL; i++)
        l->ids[i] = ids[i];
    if (ids[i] != NULL)
        fprintf(stderr, "lifecycle: %s: more than %d assertions\n", etude_case_id(c), LC_ASSERTS);
}

void lc_assert(struct lc *l, const char *id, int held, const char *detail)
{
    size_t i;

    for (i = 0; l->ids[i] != NULL; i++)
        if (strcmp(l->ids[i], id) == 0)
            break;
    if (l->ids[i] == NULL || l->told[i]) {
        fprintf(stderr, "lifecycle: %s: %s is not declared or was reported\n", etude_case_id(l->c),
                id);
        return;
    }
    l->told[i] = 1;
    etude_assert(l->c, id, held, held ? NULL : detail);
}

int lc_ok(const struct lc *l)
{
    return l->why[0] == '\0';
}

void lc_halt(struct lc *l, enum etude_status st, const char *what)
{
    if (st == ETUDE_STOPPED)
        l->stopped = 1;
    if (l->why[0] == '\0')
        snprintf(l->why, sizeof(l->why), "%s: %s", what, status_name(st));
}

int lc_finish(struct lc *l)
{
    size_t i;

    for (i = 0; l->ids[i] != NULL; i++)
        if (!l->told[i]) {
            l->told[i] = 1;
            etude_notrun(l->c, l->ids[i], l->why[0] != '\0' ? l->why : "the scenario passed it by");
        }
    return l->stopped;
}

int lc_step(struct lc *l, const char *step)
{
    enum etude_status st;
    char what[64];

    if (!lc_ok(l))
        return 0;
    st = etude_step(l->c, step);
    if (st != ETUDE_OK) {
        snprintf(what, sizeof(what), "step %s", step);
        lc_halt(l, st, what);
    }
    return st == ETUDE_OK;
}

struct etude_case_machine *lc_boot(struct lc *l, const char *machine)
{
    struct etude_case_machine *m = NULL;
    struct etude_termination end;
    enum etude_status st;
    char what[96];

    if (!lc_ok(l))
        return NULL;
    end.end = ETUDE_END_LOST;
    st = etude_boot(l->c, machine, NULL, &m, &end);
    if (st == ETUDE_OK)
        return m;
    snprintf(what, sizeof(what), "the boot of %s, which ended %s", machine, end_name(end.end));
    lc_halt(l, st, what);
    return NULL;
}

int lc_wait(struct lc *l, struct etude_case_machine *m, const char *text, uint64_t *at,
            const char *id)
{
    enum etude_status st;
    char what[160];

    if (!lc_ok(l))
        return 0;
    st = etude_console_wait(m, text, *at, at);
    if (st == ETUDE_OK) {
        if (id != NULL)
            lc_assert(l, id, 1, NULL);
        return 1;
    }
    snprintf(what, sizeof(what), "the wait for '%s'", text);
    lc_halt(l, st, what);
    if (id != NULL && st != ETUDE_STOPPED)
        lc_assert(l, id, 0, l->why);
    return 0;
}

int lc_type(struct lc *l, struct etude_case_machine *m, const char *text)
{
    enum etude_status st;

    if (!lc_ok(l))
        return 0;
    st = etude_type(m, text);
    if (st != ETUDE_OK)
        lc_halt(l, st, "typing at the console");
    return st == ETUDE_OK;
}

void lc_find(struct lc *l, struct etude_case_machine *m, const char *id, const char *text,
             uint64_t from, int present)
{
    enum etude_status st;
    uint64_t at = 0;
    char detail[160];

    if (!lc_ok(l))
        return;
    st = etude_console_find(m, text, from, &at);
    if (st == ETUDE_OK)
        snprintf(detail, sizeof(detail), "the console says '%s'", text);
    else if (st == ETUDE_FAILED)
        snprintf(detail, sizeof(detail), "the console never says '%s'", text);
    else
        snprintf(detail, sizeof(detail), "the search for '%s' was %s", text, status_name(st));
    lc_assert(l, id, present ? st == ETUDE_OK : st == ETUDE_FAILED, detail);
}

int lc_end(struct lc *l, struct etude_case_machine *m, struct etude_termination *end)
{
    enum etude_status st;

    if (!lc_ok(l))
        return 0;
    end->end = ETUDE_END_LOST;
    st = etude_wait_end(m, end);
    if (st != ETUDE_OK)
        lc_halt(l, st, "the wait for the machine's end");
    return st == ETUDE_OK;
}

int lc_quit(struct lc *l, struct etude_case_machine *m, const char *id)
{
    struct etude_termination end;
    enum etude_status quit, st;
    char *reply = NULL, detail[160];

    if (!lc_ok(l))
        return 0;
    quit = etude_qmp(m, "quit", NULL, &reply);
    free(reply);
    if (quit == ETUDE_STOPPED) {
        lc_halt(l, quit, "QMP's quit");
        return 0;
    }
    end.end = ETUDE_END_LOST;
    st = etude_wait_end(m, &end);
    if (st != ETUDE_OK) {
        lc_halt(l, st, "the wait for the machine's end after QMP's quit");
        if (st != ETUDE_STOPPED && id != NULL)
            lc_assert(l, id, 0, l->why);
        return 0;
    }
    if (id == NULL)
        return 1;
    snprintf(detail, sizeof(detail), "QMP's quit was %s, and the machine ended %s",
             status_name(quit), end_name(end.end));
    lc_assert(l, id, quit == ETUDE_OK && end.end == ETUDE_END_QEMU_EXITED, detail);
    return 1;
}

/* The room for the reason an assertion of the machine's state fails. */
#define WHY_LEN 160

/**
 * Send QMP command `execute` with `arguments` and give the member `return`
 * of its reply, parsed, with the whole reply in `*doc` for the caller to
 * free; or NULL, with the reason in `why`, when there is none.
 */
static const struct eh_json *ask(struct lc *l, struct etude_case_machine *m, const char *execute,
                                 const char *arguments, struct eh_json **doc, char why[WHY_LEN])
{
    char *reply = NULL;
    const struct eh_json *r = NULL;
    enum etude_status st;

    *doc = NULL;
    st = etude_qmp(m, execute, arguments, &reply);
    if (st == ETUDE_OK && reply != NULL && (*doc = eh_json_parse(reply)) != NULL)
        r = eh_json_get(*doc, "return");
    free(reply);
    if (r == NULL) {
        snprintf(why, WHY_LEN, "QMP's %s was %s", execute, status_name(st));
        if (st != ETUDE_OK && st != ETUDE_FAILED)
            lc_halt(l, st, why);
    }
    return r;
}

/**
 * Say whether every processor of the `info registers -a` text `regs` has
 * halted with its interrupts off, and they are `cpus` when that is not 0;
 * the reason when not is put in `why`. Each processor has one line of
 * flags, `EFL=` or `RFL=` in hexadecimal, that ends with `HLT=0` or `HLT=1`.
 */
static int all_halted(const char *regs, int cpus, char *why, size_t size)
{
    const char *p = regs, *eol, *hlt;
    unsigned long flags;
    char *end;
    int n = 0;

    while ((p = strstr(p, "FL=")) != NULL) {
        n++;
        flags = strtoul(p + 3, &end, 16);
        eol = strchr(p, '\n');
        hlt = strstr(p, "HLT=");
        if (end == p + 3 || hlt == NULL || (eol != NULL && hlt > eol)) {
            snprintf(why, size, "the registers of processor %d are not readable", n);
            return 0;
        }
        if (hlt[4] != '1') {
            snprintf(why, size, "processor %d has not halted", n);
            return 0;
        }
        if (flags & 0x200) {
            snprintf(why, size, "processor %d has halted with its interrupts on", n);
            return 0;
        }
        p = hlt;
    }
    if (n == 0) {
        snprintf(why, size, "the monitor gave the registers of no processor");
        return 0;
    }
    if (cpus != 0 && n != cpus) {
        snprintf(why, size, "the monitor gave the registers of %d processors, not %d", n, cpus);
        return 0;
    }
    return 1;
}

void lc_halted(struct lc *l, struct etude_case_machine *m, int cpus, const char *id)
{
    const struct eh_json *r, *status;
    struct eh_json *doc;
    char why[WHY_LEN];
    int held = 0;

    if (!lc_ok(l))
        return;
    r = ask(l, m, "query-status", NULL, &doc, why);
    status = r != NULL ? eh_json_get(r, "status") : NULL;
    if (r != NULL && (status == NULL || status->type != 's' || strcmp(status->string, "running")))
        snprintf(why, sizeof(why), "QEMU says that the machine is %s; a halt leaves it running",
                 status != NULL && status->type == 's' ? status->string : "in no state");
    else if (r != NULL)
        held = 1;
    eh_json_free(doc);
    if (held) {
        r = ask(l, m, "human-monitor-command", "{\"command-line\": \"info registers -a\"}", &doc,
                why);
        held = r != NULL && r->type == 's' && all_halted(r->string, cpus, why, sizeof(why));
        eh_json_free(doc);
    }
    if (!l->stopped)
        lc_assert(l, id, held, why);
}

void lc_received(struct lc *l, struct etude_case_machine *m, const char *id)
{
    const struct eh_json *r;
    const char *value;
    struct eh_json *doc;
    unsigned long lsr = 0;
    char why[WHY_LEN];
    int held = 0;

    if (!lc_ok(l))
        return;
    r = ask(l, m, "human-monitor-command", "{\"command-line\": \"i /b 0x3fd\"}", &doc, why);
    value = r != NULL && r->type == 's' ? strstr(r->string, "= 0x") : NULL;
    if (value != NULL) {
        lsr = strtoul(value + 2, NULL, 16);
        held = (lsr & 1) != 0;
        snprintf(why, sizeof(why), "the line status register is 0x%02lx: no byte received", lsr);
    } else if (r != NULL) {
        snprintf(why, sizeof(why), "the monitor gave no port value");
    }
    eh_json_free(doc);
    if (!l->stopped)
        lc_assert(l, id, held, why);
}

/** Say whether file `path` has `text`, reading at most its first MiB. */
static int file_has(const char *path, const char *text)
{
    FILE *f = fopen(path, "rb");
    size_t n;
    char *buf;
    int found;

    if (f == NULL)
        return 0;
    buf = malloc((1u << 20) + 1);
    n = buf != NULL ? fread(buf, 1, 1u << 20, f) : 0;
    fclose(f);
    if (buf == NULL)
        return 0;
    buf[n] = '\0';
    found = strstr(buf, text) != NULL;
    free(buf);
    return found;
}

void lc_fsck(struct lc *l, const char *disk, int dirty, const char *id)
{
    static const char unclean[] = "not cleanly unmounted";
    char name[] = "fsck.krffs", offset[] = "--offset", where[] = LC_PART_OFFSET, size[] = "--size",
         bytes[] = LC_PART_SIZE, path[1024], detail[200];
    char *argv[] = {name, path, offset, where, size, bytes, NULL};
    const char *copy = etude_private_copy(l->c, disk);
    struct etude_termination end;
    enum etude_status st;
    int said;

    if (!lc_ok(l))
        return;
    if (copy == NULL || strlen(copy) >= sizeof(path)) {
        lc_halt(l, ETUDE_REFUSED, "the copy of the disk");
        return;
    }
    snprintf(path, sizeof(path), "%s", copy);
    etude_progress(l->c, "fsck", 0);
    st = etude_host(l->c, argv, "fsck", &end);
    if (st != ETUDE_OK) {
        lc_halt(l, st, "fsck.krffs");
        return;
    }
    etude_progress(l->c, "fsck", 1);
    said = file_has(etude_evidence(l->c, "fsck"), unclean) ||
           file_has(etude_evidence(l->c, "fsck.err"), unclean);
    if (end.end != ETUDE_END_EXITED || end.value != 0)
        snprintf(detail, sizeof(detail), "fsck.krffs ended %s %d", end_name(end.end), end.value);
    else
        snprintf(detail, sizeof(detail), "fsck.krffs says the copy %s",
                 said ? "was not cleanly unmounted" : "was cleanly unmounted");
    lc_assert(l, id, end.end == ETUDE_END_EXITED && end.value == 0 && said == dirty, detail);
}
