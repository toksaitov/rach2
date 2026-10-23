#ifndef RACH2_STATUS_PROC_HOST_H
#define RACH2_STATUS_PROC_HOST_H
#include <stddef.h>
#include <sys/types.h>

/* Copy observed process identities and the conflicting lock into proc format. */
int status_proc_capture(const char *root, const pid_t *pids, size_t count, const char *lock);
#endif
