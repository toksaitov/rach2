#ifndef KRFFS_UTILITIES_H
#define KRFFS_UTILITIES_H

#include <stddef.h>
#include <stdint.h>

uint64_t krffs_calculate_djb_hash(uint8_t *string);
char *krffs_scale_size(uint64_t size, char *buffer, size_t buffer_size);

#endif
