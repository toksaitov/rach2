/* Compiled workloads and native child control for the set-driver fixture. */
#ifndef RACH2_LEGACYSET_FIXTURE_H
#define RACH2_LEGACYSET_FIXTURE_H

#include <stddef.h>
#ifdef _WIN32
#define LEGACY_PATH_SEPARATOR ";"
#define LEGACY_EXE            ".exe"
#else
#define LEGACY_PATH_SEPARATOR ":"
#define LEGACY_EXE            ""
#endif

/* Return -1 for the controller, or a fixture role's exit status. */
int legacy_fixture_role(int argc, char **argv);
int legacy_fixture_stage(const char *self, const char *tree, const char *guest_build,
                         const char *arch);
int legacy_fixture_copy(const char *from, const char *to);

struct legacy_child;
struct legacy_child *legacy_child_start(const char *self, const char *description);
int legacy_child_running(struct legacy_child *child);
int legacy_child_wait(struct legacy_child *child);
int legacy_child_stop(struct legacy_child *child);
void legacy_child_free(struct legacy_child *child);
/* Catch a real console break on Windows, and SIGTERM on POSIX. */
void legacy_child_catch(void (*stop)(int));
#endif
