/* Transport proof bytes while retaining their original native locators. */
#ifndef ETUDE_ROUTINE_EVIDENCE_H
#define ETUDE_ROUTINE_EVIDENCE_H
#include <signal.h>
int etude_prepare_evidence_main(int argc, char **argv, volatile sig_atomic_t *stop);
int etude_prepare_reader_main(int argc, char **argv);
int etude_routine_pack(const char *out, const char *const roots[], volatile sig_atomic_t *stop);
#endif
