/* family_check.h: C controllers of host-family product checks. */
#ifndef HOST_FAMILY_CHECK_H
#define HOST_FAMILY_CHECK_H
#include "fixture.h"
#include <stdint.h>
const char *family_path(struct fixture *f, const char *name);
const char *family_source(struct fixture *f, const char *name);
int family_row(struct fixture *f, const char *id, int held);
int family_sparse(const char *path, uint64_t bytes);
int family_contains(const char *path, const char *text);
int family_digest(const char *path, char hex[65]);
int family_extract(struct fixture *f, const char *names);
int family_run(struct fixture *f, const char *const argv[], const char *cwd,
               const char *const env[], const char *input);
int family_eval(struct fixture *f, const char *command);
int family_seed(struct fixture *f, const char *name, unsigned mib, const char *group);
int family_grow(struct fixture *f, const char *id);
int family_grow_boot(struct fixture *f);
int family_grow_space(struct fixture *f);
int family_grow_replay(struct fixture *f);
int family_grow_mock(int argc, char **argv);
#endif
