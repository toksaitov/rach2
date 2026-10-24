/* A routine route closes its physical charge after the collector exits. */
#ifndef ETUDE_ROUTINE_CHARGE_H
#define ETUDE_ROUTINE_CHARGE_H
#include "json.h"
#include <stdint.h>

struct etude_routine_input {
    const struct json *nodes;
    const char *run_id, *commit, *completion_path, *history_path;
    const unsigned char *plan_digest, *run_digest;
};
struct etude_routine_cost {
    uint64_t charged_us, preparation_us, error_us;
    int load_available, preparation_load_available;
};
/* A reader may use transported proof bytes, by exact hash and size only. */
int etude_routine_proof_directory(const char *path);
int etude_routine_location(const struct json *ref, char *path, size_t size);
char *etude_routine_blob(const char *sha256, uint64_t size, const char *location, size_t *length);
struct json *etude_routine_document(const char *sha256, uint64_t size, const char *location);
/* Extract the existing purpose deadline; callers still validate its policy. */
int etude_routine_work_us(const char *policy_path, const char *purpose, uint64_t *work_us);
/* Read exact hash-bound bytes, requiring one absolute location. */
char *etude_routine_bytes(const struct json *ref, size_t *size);
/* Validate the immutable preflight chain. Completion does not exist yet. */
int etude_routine_context(const struct json *grant, const struct json *upstream, const char *run_id,
                          const char *commit);
/* 0: ordinary route; 1: complete physical charge; -1: invalid route proof. */
int etude_routine_charge(const struct etude_routine_input *input, struct etude_routine_cost *cost);
int etude_routine_plan(const struct json *plan, const struct etude_routine_input *input,
                       struct etude_routine_cost *cost);
#endif
