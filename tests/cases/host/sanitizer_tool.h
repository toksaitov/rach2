/* Resolve a prepared sanitizer snapshot to its verified native OS tool. */
#ifndef HOST_SANITIZER_TOOL_H
#define HOST_SANITIZER_TOOL_H

#include <stddef.h>

int host_sanitizer_source(const char *snapshot, char *source, size_t size);

#endif
