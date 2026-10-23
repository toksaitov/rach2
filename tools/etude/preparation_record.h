/* preparation_record.h: shared preparation record fields and paths. */
#ifndef ETUDE_PREPARATION_RECORD_H
#define ETUDE_PREPARATION_RECORD_H
#include <sys/stat.h>
#ifdef _WIN32
#include "win/files.h"
#endif

#define ETUDE_PREPARATION_HEAD "etude-prepare 1"

long long etude_record_mtime_ns(const struct stat *st);
char *etude_record_digests_path(const char *tree);
char *etude_record_artifact_path(const char *tree, const char *name);
#endif
