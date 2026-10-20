/* The host's pathname limit, also when compiled as freestanding C. */
#ifndef PATHMAX_HOST_H
#define PATHMAX_HOST_H

#include <limits.h>
#if defined(__APPLE__)
#include <sys/syslimits.h>
#endif

#endif /* PATHMAX_HOST_H */
