/* Fixed routine inputs for the existing native sequence and phase owners. */
#ifndef ETUDE_ROUTINE_ENTRY_H
#define ETUDE_ROUTINE_ENTRY_H
#include <stdint.h>
#include <signal.h>

#define ROUTINE_PATH 4096

struct routine_site {
    char endpoint[16], mode[24];
    char launcher[ROUTINE_PATH], ledger[ROUTINE_PATH], locks[ROUTINE_PATH];
    char program[ROUTINE_PATH], capacity_text[128], resources[128], account[256];
    uint64_t capacity[3];
    unsigned wait, seconds, jobs;
};

struct hp_words;
struct json;
struct routine_input {
    const char *purpose, *arches, *run, *accel;
    char tree[ROUTINE_PATH], out[ROUTINE_PATH], site[ROUTINE_PATH];
    char program[ROUTINE_PATH], commit[41];
    int count;
    char **selection;
};

int routine_input_read(int argc, char **argv, struct routine_input *input);
int routine_path(char out[ROUTINE_PATH], const char *root, const char *name);
int routine_command(struct hp_words *args, volatile sig_atomic_t *stop);
int routine_query(const struct routine_input *input, struct hp_words *args, const char *name,
                  volatile sig_atomic_t *stop);
int routine_selection(const char *expected, const char *actual);
int routine_phase_inputs(const char *purpose, struct json **value);
int etude_prepare_routine_main(int argc, char **argv, volatile sig_atomic_t *stop);
/* The two stages of etude_prepare_routine_main: the build, then the context. */
int etude_prepare_routine_build_main(int argc, char **argv, volatile sig_atomic_t *stop);
int etude_prepare_routine_context_main(int argc, char **argv, volatile sig_atomic_t *stop);
int etude_routine_entry_main(int argc, char **argv);
int etude_routine_sequence_main(int argc, char **argv);
/* A sequence in two runs: its preparation's build, then the collection. */
int etude_routine_prepare_main(int argc, char **argv);
int etude_routine_collect_main(int argc, char **argv);
int etude_routine_owner_main(int argc, char **argv);

/* A route may inspect its disabled endpoint only after fixed-binding checks. */
int routine_site_read(const char *path, const char *route, struct routine_site *site);
int routine_route_mode(const char *path, char mode[24]);

#endif
