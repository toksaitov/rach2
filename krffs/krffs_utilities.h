#ifndef KRFFS_UTILITIES_H
#define KRFFS_UTILITIES_H

#include <stddef.h>
#include <stdint.h>

/** Computes Bernstein's djb2 hash over the NUL-terminated `string`. */
uint64_t krffs_calculate_djb_hash(uint8_t *string);

/** Formats `size` bytes into `buffer` in the largest fitting unit. */
char *krffs_scale_size(uint64_t size, char *buffer, size_t buffer_size);

#endif
