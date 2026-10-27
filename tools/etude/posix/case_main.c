/*
 * case_main.c: the `main` of a machine scenario, which libetude-case gives,
 * so that a scenario defines etude_scenario alone.
 */
#include "etude/scenario.h"
#include "etude/args.h"

int main(int argc, char **argv)
{
    etude_utf8_args(argc, argv, "etude-case");
    return etude_case_main(argc, argv);
}
