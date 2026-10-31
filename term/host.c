/*
 * host.c: term -H, the child on a host pty with no screen, whose cells are
 * dumped on request, for tests. Task 16 writes it.
 */

#include <stdio.h>

#include "term.h"

int term_host_run(const struct term_options *o)
{
    (void)o;
    fprintf(stderr, "term: -H: not implemented\n");
    return 1;
}
