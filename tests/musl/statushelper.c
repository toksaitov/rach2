/**
 * statushelper: end with the exit status or the signal it is told to, for
 * probes of what a parent reads back.
 *
 * `statushelper N` exits with N; `statushelper signal` kills itself with
 * SIGTERM, whose default action terminates; any other argument count exits
 * with 2. It is a fixture and not a probe: the probe runner never runs it,
 * since a program whose purpose is to fail on request would fail the suite it
 * was listed in.
 *
 * `ishmusl` runs it through `ish -c` to read back what the shell reports.
 */

#include <stdlib.h>
#include <string.h>
#include <signal.h>

int main(int argc, char **argv)
{
    if (argc != 2) {
        return 2;
    }
    if (strcmp(argv[1], "signal") == 0) {
        raise(SIGTERM);

        return 3; /* not reached: SIGTERM's default action terminates */
    }

    return atoi(argv[1]);
}
