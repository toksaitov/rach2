/* Bounded file copies for scenario fixtures; offsets and lengths are bytes. */
#ifndef CASE_FILEIO_H
#define CASE_FILEIO_H
#include <stdint.h>
int case_copy_range(const char *source, uint64_t offset, uint64_t length,
                    const char *destination, uint64_t at, int truncate);
#endif
