/* Fixed endpoint binding and durable state shared by authority operations. */
#ifndef ETUDE_AUTHORITY_INTERNAL_H
#define ETUDE_AUTHORITY_INTERNAL_H
#include "authority.h"
#include "authority_platform.h"
#define AUTHORITY_PATH 4096
struct authority_binding {
    char ledger[AUTHORITY_PATH], site[AUTHORITY_PATH], gate[AUTHORITY_PATH];
    char state[AUTHORITY_PATH], archive[AUTHORITY_PATH], operator_id[256];
    char program[AUTHORITY_PATH], launcher_body[AUTHORITY_PATH];
    char physical_block[AUTHORITY_PATH], route[AUTHORITY_PATH];
};
struct authority_state {
    char run[256], commit[41], phase[16], nonce[65], snapshot[AUTHORITY_PATH];
    struct etude_pid owner;
};
struct etude_authority {
    struct authority_binding binding;
    struct authority_gate gate;
    struct authority_state state;
    int exclusive;
};
int authority_binding_read(struct authority_binding *binding);
int authority_state_read(const struct authority_binding *binding, struct authority_state *state);
int authority_state_write(const struct authority_binding *binding,
                          const struct authority_state *state);
int authority_read(const char *path, char **bytes, size_t *size);
int authority_token(const char *text);
int authority_absolute(const char *text);
int authority_owner_live(const struct etude_pid *identity);
int authority_policy(const struct authority_binding *binding, const char *path,
                      char **bytes, size_t *size, uint64_t capacity[3], int *enabled);
#endif
