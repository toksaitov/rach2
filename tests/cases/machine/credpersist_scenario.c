/*
 * credpersist_scenario.c: credential publication and lock release across
 * reboots.
 */
#include <stdlib.h>

#include "netcase.h"
#include "console_report.h"
#include "../../creddisk_rows.h"

/** Report the rows after boot (0 to 2) as not run once that boot failed. */
static void later_notrun(struct etude_case *c, unsigned boot)
{
    const char *const *row;
    unsigned i;

    for (i = boot + 1; i < 3; i++) {
        for (row = creddisk_rows[i]; *row != NULL; row++) {
            etude_notrun(c, *row, "an earlier boot failed");
        }
    }
}

/**
 * Run three cold boots of the same private disk, reporting the probe's rows.
 */
int etude_scenario(struct etude_case *c)
{
    static const char *const steps[] = {"write", "read", "final"};
    struct net_case u;
    struct net_boot b;
    struct etude_termination end;
    char *reply, *text;
    uint64_t from;
    int failed = 0;
    unsigned i;

    net_case(&u, c);
    for (i = 0; i < 3 && net_ok(&u) && !failed; i++) {
        net_step(&u, steps[i]);
        net_boot(&b, &u, "fixture", i + 1, NULL);
        net_wait(&b, NET_SHELL);
        net_line(&b, "exec /creddisk");
        from = b.at;
        if (net_ok(&u) && console_report(c, b.m, "fixture", i + 1, b.at, &b.at) != ETUDE_OK)
            net_fail(&u, "creddisk did not report boot %u", i + 1);
        if (net_ok(&u)) {
            text = net_text(&b, from, b.at);
            failed = net_find(text, "@etude 1 assert * fail") != NULL ||
                     net_find(text, "@etude 1 notrun ") != NULL;
            free(text);
            if (failed) {
                later_notrun(c, i);
            }
            reply = NULL;
            if (etude_qmp(b.m, "quit", NULL, &reply) != ETUDE_OK ||
                etude_wait_end(b.m, &end) != ETUDE_OK)
                net_fail(&u, "boot %u did not end", i + 1);
            free(reply);
        }
    }
    return net_end(&u);
}
