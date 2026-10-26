/* A single authority-owned invocation; no case selection or scheduling. */
#ifndef ETUDE_AUTHORITY_RUN_H
#define ETUDE_AUTHORITY_RUN_H
#include "include/etude/exec.h"
#include <signal.h>
struct authority_run {
    const char *receipt, *phase;
    char **argv;
    int watch_input, forward_input, dedicated_input, clear_parent;
    const volatile sig_atomic_t *stop;
};
int authority_run_command(const struct authority_run *run, struct etude_exec_result *result);
#endif
