/* Fixed producer workspace operations and accepted-job evidence. */
#ifndef ETUDE_ROUTINE_PRODUCER_H
#define ETUDE_ROUTINE_PRODUCER_H
#include "host_prepare.h"
int etude_routine_producer_main(int argc, char **argv, volatile sig_atomic_t *stop);
struct json *etude_routine_jobs_snapshot(struct hp_context *host, const struct json *config,
                                         const char *run, const char *runner, const char *tag);
/* Pure listener reconciliation, also used by the existing focused fixture. */
struct json *etude_routine_listener_state(const char *text, long long boot, long long now);
#endif
