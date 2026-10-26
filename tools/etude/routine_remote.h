/* Existing serve protocol used by ordinary route preparation. */
#ifndef ETUDE_ROUTINE_REMOTE_H
#define ETUDE_ROUTINE_REMOTE_H
#include <stddef.h>
#include <signal.h>
#include "json.h"

struct etude_routine_remote;
struct etude_routine_remote *etude_routine_remote_open(const struct json *producer, const char *run,
                                                       const char *evidence, const char *mode,
                                                       const char *wsl,
                                                       volatile sig_atomic_t *stop);
int etude_routine_remote_put(struct etude_routine_remote *remote, const char *name,
                             const void *bytes, size_t size, const char *mode);
/* The returned token belongs to REMOTE and lasts until its next execute. */
const char *etude_routine_remote_execute(struct etude_routine_remote *remote, const char *name,
                                         const char *program, const struct json *arguments,
                                         const struct json *environment, const char *output);
int etude_routine_remote_get(struct etude_routine_remote *remote, const char *token,
                             const char *name, size_t maximum, char **bytes, size_t *size);
/* Both calls consume REMOTE and clear the caller's pointer. */
int etude_routine_remote_close(struct etude_routine_remote **remote);
int etude_routine_remote_abandon(struct etude_routine_remote **remote);
#endif
