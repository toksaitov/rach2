/* native.h: a structured attempt on an already running Rach2 system. */
#ifndef ETUDE_NATIVE_H
#define ETUDE_NATIVE_H
#include "etude/remote.h"
struct etude_native_spec {
    const char *run, *attempt, *case_id, *journal, *evidence;
    char *const *transport_argv;
    const char *const *files;
    size_t nfiles;
    const char *const *environment, *const *unset, *const *fixtures;
    size_t nenvironment, nunset, nfixtures;
    char *const *argv;
    const char *input;
    uint64_t deadline_ms, grace_ms, output_max, stage_ms, cancel_ms;
    unsigned reconnects;
    const volatile sig_atomic_t *cancel_requested;
    int staged_only, keep;
    enum etude_end expected_end;
    int expected_value;
};
struct etude_native_result {
    enum etude_remote_status transport;
    struct etude_remote_result attempt;
    int status; /* 0 pass, 1 failure, 2 refused, 3 cancelled, 4 incomplete */
    char detail[256];
    int removed;
};
/** Run one attempt through direct process arguments. Staged-only files are
 * hashed locally and resolved by size and digest from etude-guest's stage;
 * a missing staged file fails. The caller frees the result after every call. */
int etude_native_attempt(const struct etude_native_spec *spec, struct etude_native_result *result);
void etude_native_result_free(struct etude_native_result *result);
#endif
