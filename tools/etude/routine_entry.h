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

/* A route may inspect its disabled endpoint only after fixed-binding checks. */
int routine_site_read(const char *path, const char *route, struct routine_site *site);
int routine_route_mode(const char *path, char mode[24]);

#endif
