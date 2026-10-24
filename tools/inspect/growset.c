/*
 * growset: the host build of utils/growset.c, the grow set calculator the
 * recovery system stages, for the suites to run on the host.
 */
#include "../image/host_io.h"
#ifdef _WIN32
#define fseeko host_image_seek
#define ftello host_image_tell
#define off_t  int64_t
#endif
#include "../../utils/growset.c"
