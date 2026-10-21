/*
 * bootpart_scenario.c: the scenario of the machine.bootpart cases
 * (tests/cases/machine.bootpart.json): bootprobe run at the kernel's debug
 * shell in one boot for each arrangement of the boot disk, its report read
 * off the console and its last line judged.
 *
 * The kernel's command line selects bootprobe's mode, so the cases differ
 * only in their machines' profiles; the scenario says which line the probe
 * must end with. The wide case's disk is an empty disk of 260 GiB, too large
 * to be an artifact of the store, so the scenario has mkfixture write its
 * two markers and its MBR signature into the attempt's copy before the boot.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "console_report.h"
#include "netcase.h"

/* The one machine of each case. */
#define MACHINE "bootpart"

/* How each line of bootprobe's that ends a run begins. */
#define TOKEN_HEAD "BOOTPROBE "

/* A case of the family, by the last part of its identity. */
struct mode {
    const char *name;  /* the last part of the case's identity */
    const char *token; /* bootprobe's last line when every check held */
    int wide;          /* the disk is the empty wide disk, which mkfixture
                          writes before the boot */
};

static const struct mode modes[] = {
    {"absent", "BOOTPROBE ABSENT OK", 0},
    {"fixture", "BOOTPROBE OK", 0},
    {"ramroot", "BOOTPROBE RAMROOT OK", 0},
    {"wide", "BOOTPROBE WIDE OK", 1},
};

/** Give the mode of case `c`, or NULL for a case of another family. */
static const struct mode *mode_of(struct etude_case *c)
{
    const char *id = etude_case_id(c), *dot = strrchr(id, '.');
    size_t i;

    for (i = 0; dot != NULL && i < sizeof(modes) / sizeof(modes[0]); i++) {
        if (strcmp(dot + 1, modes[i].name) == 0)
            return &modes[i];
    }
    return NULL;
}

/**
 * Step `markers`: have mkfixture write the wide disk's markers at 5 GiB and
 * 258 GiB and its MBR signature into the attempt's copy of the empty disk,
 * which keeps its size.
 */
static void markers(struct net_case *u)
{
    char mkfixture[] = "mkfixture", wide[] = "wide";
    char *argv[4];
    const char *copy;
    struct net_run r;

    if (net_step(u, "markers") != ETUDE_OK)
        return;
    copy = etude_private_copy(u->c, "wide-disk");
    if (copy == NULL) {
        net_fail(u, "the wide disk has no copy");
        return;
    }
    argv[0] = mkfixture;
    argv[1] = wide;
    argv[2] = (char *)copy;
    argv[3] = NULL;
    if (net_host(u, argv, "mkfixture", &r) == ETUDE_OK && !net_exited0(&r))
        net_fail(u, "mkfixture wide did not write the markers: %.120s", r.err);
    net_run_free(&r);
}

/**
 * Wait for bootprobe's last line, the first that begins `TOKEN_HEAD` past
 * the console read so far, and give it without its end in `line` of `size`
 * bytes; an empty text when it did not come.
 */
static const char *token_line(struct net_boot *b, char *line, size_t size)
{
    uint64_t from;
    char *text;

    line[0] = '\0';
    if (net_wait(b, TOKEN_HEAD) != ETUDE_OK)
        return line;
    from = b->at - strlen(TOKEN_HEAD);
    if (net_wait(b, "\n") != ETUDE_OK)
        return line;
    text = net_text(b, from, b->at);
    net_copy_line(text, line, size);
    free(text);
    return line;
}

/** End boot `b` with QMP's `quit` and wait for the machine to end. */
static void quit(struct net_boot *b)
{
    struct etude_termination end;
    char *reply = NULL;

    if (!net_ok(b->u))
        return;
    etude_qmp(b->m, "quit", NULL, &reply);
    free(reply);
    etude_wait_end(b->m, &end);
}

int etude_scenario(struct etude_case *c)
{
    const struct mode *mode = mode_of(c);
    struct net_case u;
    struct net_boot b;
    char line[200];
    uint64_t from;

    if (mode == NULL) {
        fprintf(stderr, "bootpart_scenario: %s is no case of machine.bootpart\n", etude_case_id(c));
        return 1;
    }
    net_case(&u, c);
    line[0] = '\0';
    if (mode->wide)
        markers(&u);
    if (net_step(&u, "boot") == ETUDE_OK && net_boot(&b, &u, MACHINE, 1, NULL) == ETUDE_OK &&
        net_wait(&b, NET_SHELL) == ETUDE_OK && net_step(&u, "probe") == ETUDE_OK) {
        from = b.at;
        if (net_line(&b, "exec /bootprobe") == ETUDE_OK &&
            console_report(c, b.m, MACHINE, 1, from, &b.at) != ETUDE_OK)
            net_fail(&u, "bootprobe did not finish its report");
        token_line(&b, line, sizeof(line));
        quit(&b);
    }
    net_assert(&u, "token", strcmp(line, mode->token) == 0, "bootprobe's last line is [%.150s]",
               line);
    return net_end(&u);
}
