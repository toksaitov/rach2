/* Compile the real preparation graph with the artifact fixture's clock. */
#include "../../../tools/etude/posix/posix.h"

uint64_t etude_artifact_prep_now_us(void);
#define posix_now_us etude_artifact_prep_now_us
#include "../../../tools/etude/posix/prepare.c"
