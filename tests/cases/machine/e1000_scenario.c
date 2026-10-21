/*
 * e1000_scenario.c: the scenario of the e1000 cases,
 * tests/cases/machine.e1000.json: which card the kernel binds, booted with
 * an RTL8139 and an e1000 together, with an e1000 alone, and with an e1000e,
 * which no driver takes.
 *
 * Each case boots the test image to the debug shell and waits for the bound
 * driver's own detail line, or for the probe's line that no card was found.
 * Only a driver that took its card prints a detail line, so the line is the
 * binding; the e1000e's 82574L has another register interface, which the
 * e1000 driver must refuse by its device identity, and the machine must
 * still reach its shell.
 */
#include <string.h>

#include "netcase.h"

/* A case of the family: the end of its identity and the line it waits
 * for. */
struct card_case {
    const char *name;
    const char *line;
};

static const struct card_case cases[] = {
    {"machine.e1000.both", "rtl8139: IRQ "},
    {"machine.e1000.alone", "e1000: 82540EM"},
    {"machine.e1000.unsupported", "no supported network card"},
};

int etude_scenario(struct etude_case *c)
{
    const struct card_case *k = NULL;
    struct net_case u;
    struct net_boot b;
    size_t i;

    net_case(&u, c);
    for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        if (strcmp(etude_case_id(c), cases[i].name) == 0)
            k = &cases[i];
    }
    if (k == NULL)
        net_fail(&u, "no case %s", etude_case_id(c));
    if (net_step(&u, "boot") == ETUDE_OK && net_boot(&b, &u, "card", 1, NULL) == ETUDE_OK)
        net_wait(&b, k->line);
    net_assert(&u, "card", 1, "");
    if (net_step(&u, "shell") == ETUDE_OK)
        net_wait(&b, NET_SHELL);
    net_assert(&u, "shell", 1, "");
    return net_end(&u);
}
