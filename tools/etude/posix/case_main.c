/*
 * case_main.c: the `main` of a machine scenario, which libetude-case gives,
 * so that a scenario defines etude_scenario alone.
 */
#include "etude/scenario.h"

int main(int argc, char **argv)
{
    return etude_case_main(argc, argv);
}
