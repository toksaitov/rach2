/* Native phase proofs and the physical-owner preparation barrier for WSL. */
#ifndef ETUDE_ROUTINE_DIRECT_H
#define ETUDE_ROUTINE_DIRECT_H
#include "include/etude/exec.h"
#include "include/etude/history.h"
int routine_direct_phase(const char *phase, const struct etude_pid *owner,
                          const struct etude_exec_result *result,
                          const struct etude_reading *start);
/* 1 acknowledged, 0 pending, -1 invalid or failed. */
int routine_direct_ack(void);
#endif
