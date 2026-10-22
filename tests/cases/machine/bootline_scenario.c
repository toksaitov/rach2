/*
 * bootline_scenario.c: the scenario of the bootline cases
 * (../machine.bootline.json). Each case boots the kernel once with a boot
 * line, a disk or a processor that it must refuse, or that it must take, and
 * judges the console and, for a refusal, the machine halted with its
 * interrupts off and no shell after the refusal.
 */
#include <stdlib.h>
#include <string.h>

#include "lifecycle.h"

/* What a boot must come to. */
enum outcome {
    REFUSED,   /* a refusal, then a halt with no shell */
    PROCESSOR, /* a refusal before the kernel's own code runs, then a halt */
    SHELL      /* the debug shell */
};

/* The last line of the refusal of a boot line that skips the disk its root is
 * on; the halt follows it. */
#define NODISK_LAST \
    "and no rach2.rescue: nothing would mount and the debug shell authenticates nobody"

/* One case. */
struct row {
    const char *name; /* the last part of the case's identity */
    enum outcome outcome;
    const char *last; /* the text that ends what the boot prints */
    const char *said; /* a refusal: the text that names it; a shell: a text
                         that must not be there */
};

static const struct row rows[] = {
    {"nodisk-fsdisk", REFUSED, NODISK_LAST,
     "boot: refused: rach2.nodisk with rach2.fsdisk or rach2.root=disk"},
    {"nodisk-root", REFUSED, NODISK_LAST,
     "boot: refused: rach2.nodisk with rach2.fsdisk or rach2.root=disk"},
    {"blank", REFUSED,
     "refused: no file system mounted and no rach2.rescue: the debug shell authenticates nobody",
     "boot: refused: no file system mounted"},
    {"init-loop", REFUSED, "is not the answer to a disk that cannot be read",
     "boot: refused: /sbin/init cannot be read (-40)"},
    {"cut", REFUSED, NODISK_LAST, "boot: refused: rach2.nodisk"},
    {"cpu-486", PROCESSOR, "boot: refused: the processor has no rdtsc, and this kernel is i686",
     NULL},
    {"cpu-pentium", PROCESSOR, "boot: refused: the processor has no cmov, and this kernel is i686",
     NULL},
    {"cpu-qemu32", PROCESSOR,
     "boot: refused: the processor has no long mode, and this kernel is x86-64", NULL},
    {"kept", SHELL, "Kernel debug shell active", "boot: command line cut"},
    {"rescue-fsdisk", SHELL, "Kernel debug shell active", "boot: refused: rach2.nodisk"},
    {"rescue-init", SHELL, "Kernel debug shell active", "boot: refused: rach2.nodisk"},
};

/* The texts of which one ends what a boot of these cases prints: the last
 * line of each refusal of the kernel's and the loader's, the shell's banner,
 * and the kernel's report of an exception, after which it stops. */
static const char *const ends[] = {"the debug shell authenticates nobody",
                                   "a disk that cannot be read",
                                   "and this kernel is",
                                   "Kernel debug shell active",
                                   "kernel exception:",
                                   NULL};

/* The most reads of the registers that look for the halt after a refusal. */
#define HALT_TRIES 100

/* What must not follow a refusal: the debug shell, a login prompt, or the
 * kernel's fall to the shell. */
static const char *const shells[] = {"Kernel debug shell active", "rach2 login",
                                     "falling back to the debug shell", NULL};

/** Give the last part of the attempt's case identity. */
static const char *case_name(struct etude_case *c)
{
    const char *id = etude_case_id(c), *dot = strrchr(id, '.');

    return dot != NULL ? dot + 1 : id;
}

/**
 * Report assertion `id` from searches of the console of `m`, which has
 * ended: it holds when none of `texts`, ended by NULL, is there.
 */
static void none_of(struct lc *l, struct etude_case_machine *m, const char *id,
                    const char *const texts[])
{
    enum etude_status st = ETUDE_FAILED;
    uint64_t at;
    size_t i;

    if (!lc_ok(l))
        return;
    for (i = 0; texts[i] != NULL && st == ETUDE_FAILED; i++)
        st = etude_console_find(m, texts[i], 0, &at);
    lc_assert(l, id, st == ETUDE_FAILED,
              st == ETUDE_OK ? "the console has a shell, a login or the fall to the shell"
                             : "the console could not be searched");
}

/**
 * Say whether `w` logs the cut of a long boot line with its lengths:
 * `boot: command line cut: kept K of 511 or more bytes`.
 */
static int cut_logged(const char *w)
{
    static const char head[] = "boot: command line cut: kept ";
    const char *p = strstr(w, head);

    if (p == NULL)
        return 0;
    p += sizeof(head) - 1;
    if (*p < '0' || *p > '9')
        return 0;
    while (*p >= '0' && *p <= '9')
        p++;
    return strncmp(p, " of 511 or more bytes", 21) == 0;
}

/** Say whether the console of `m` has `text` anywhere. */
static int has(struct etude_case_machine *m, const char *text)
{
    uint64_t at;

    return etude_console_find(m, text, 0, &at) == ETUDE_OK;
}

/**
 * Wait, a line at a time, until the console of `m` has one of `ends`, the
 * texts of which one ends what any of these boots prints, whether it halts
 * or comes to the shell; give in `*at` the offset past that line. A boot that
 * takes the other way is judged when it gets there, not at the step's bound.
 * A failed wait reports assertion `id` failed.
 */
static int settled(struct lc *l, struct etude_case_machine *m, uint64_t *at, const char *id)
{
    enum etude_status st;
    size_t i;

    for (;;) {
        st = etude_console_wait(m, "\n", *at, at);
        if (st != ETUDE_OK) {
            lc_halt(l, st, "the wait for the end of the boot's output");
            if (st != ETUDE_STOPPED)
                lc_assert(l, id, 0, l->why);
            return 0;
        }
        for (i = 0; ends[i] != NULL; i++)
            if (has(m, ends[i]))
                return 1;
    }
}

/**
 * Wait for the boot to end what it prints, require the refusal's last line
 * and the text that names it, and record that the boot ended halted, which
 * `etude.fault` judges; then end the machine and search its whole console
 * for what must not follow. The last line comes a few instructions before the
 * halt, so the registers are read again, up to HALT_TRIES times, before the
 * boot is judged not halted.
 */
static void refused(struct lc *l, struct etude_case_machine *m, const struct row *r)
{
    enum etude_status st;
    uint64_t at = 0;
    unsigned tries;
    char *w;

    if (!lc_ok(l) || !settled(l, m, &at, "refused"))
        return;
    lc_assert(l, "refused", has(m, r->last) && (r->said == NULL || has(m, r->said)),
              "the boot did not print the refusal");
    if (strcmp(r->name, "init-loop") == 0)
        lc_find(l, m, "mounted", "ext2: root: /dev/disk0p1", 0, 1);
    if (strcmp(r->name, "cut") == 0 && (w = lc_console(l, "fixture", 0, at)) != NULL) {
        lc_assert(l, "cut-logged", cut_logged(w),
                  "the boot line of 668 bytes was cut with no log line that gives its lengths");
        free(w);
    }
    for (tries = 1; (st = etude_fault(m, ETUDE_END_HALTED)) == ETUDE_FAILED && tries < HALT_TRIES;
         tries++)
        ;
    if (st != ETUDE_OK && st != ETUDE_FAILED) {
        lc_halt(l, st, "the record of the halt");
        return;
    }
    lc_assert(l, "halted", st == ETUDE_OK,
              "the machine was not halted with its interrupts off after the refusal");
    if (!lc_quit(l, m, NULL))
        return;
    if (r->outcome == PROCESSOR)
        lc_find(l, m, "no-kernel", "boot: Rach2 Booting", 0, 0);
    else
        none_of(l, m, "no-shell", shells);
}

/** Wait for the boot to end what it prints, require the shell the boot line
 * asks for, end the machine, and search its console for what must not be
 * there. */
static void shell(struct lc *l, struct etude_case_machine *m, const struct row *r)
{
    uint64_t at = 0;

    if (!lc_ok(l) || !settled(l, m, &at, "shell"))
        return;
    lc_assert(l, "shell", has(m, r->last), "the boot did not come to the debug shell");
    if (lc_quit(l, m, NULL))
        lc_find(l, m, strcmp(r->name, "kept") == 0 ? "not-cut" : "not-refused", r->said, 0, 0);
}

int etude_scenario(struct etude_case *c)
{
    static const char *const refused_ids[] = {"refused", "halted", "no-shell", NULL};
    static const char *const loop_ids[] = {"refused", "mounted", "halted", "no-shell", NULL};
    static const char *const cut_ids[] = {"refused", "cut-logged", "halted", "no-shell", NULL};
    static const char *const processor_ids[] = {"refused", "halted", "no-kernel", NULL};
    static const char *const kept_ids[] = {"shell", "not-cut", NULL};
    static const char *const rescue_ids[] = {"shell", "not-refused", NULL};
    const char *name = case_name(c);
    const struct row *r = NULL;
    struct etude_case_machine *m;
    struct lc l;
    size_t i;

    for (i = 0; i < sizeof(rows) / sizeof(rows[0]); i++)
        if (strcmp(rows[i].name, name) == 0)
            r = &rows[i];
    if (r == NULL)
        lc_begin(&l, c, rescue_ids);
    else if (r->outcome == PROCESSOR)
        lc_begin(&l, c, processor_ids);
    else if (r->outcome == SHELL)
        lc_begin(&l, c, strcmp(name, "kept") == 0 ? kept_ids : rescue_ids);
    else
        lc_begin(&l, c,
                 strcmp(name, "init-loop") == 0 ? loop_ids
                 : strcmp(name, "cut") == 0     ? cut_ids
                                                : refused_ids);
    if (r == NULL) {
        lc_halt(&l, ETUDE_REFUSED, "the case's identity");
    } else if (lc_step(&l, "boot") && (m = lc_boot(&l, "fixture")) != NULL) {
        if (r->outcome == SHELL)
            shell(&l, m, r);
        else
            refused(&l, m, r);
    }
    return lc_finish(&l);
}
