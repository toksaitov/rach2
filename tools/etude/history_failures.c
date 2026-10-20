/* history_failures.c: list the attempts of a run that did not pass. */
#include "failures.h"

/** Run the command that failures.c implements. */
int main(int argc, char **argv)
{
    return etude_failures_main(argc, argv);
}
