/*
 * krffs_utilities.c: the djb2 hash, the directory identifier and the size
 * printer of krffs_utilities.h.
 */

#include "krffs_utilities.h"

#include <assert.h>
#include <inttypes.h>
#include <stdio.h>

uint64_t krffs_calculate_djb_hash(uint8_t *string)
{
    assert(string);

    uint64_t hash = 5381;

    int character;
    while ((character = *string++)) {
        hash = ((hash << 5) + hash) ^ character;
    }

    return hash;
}

uint64_t krffs_mix_id(uint64_t parent_id, const char *name)
{
    assert(name);

    uint64_t h = (parent_id + 1) * 1099511628211ULL;

    h ^= krffs_calculate_djb_hash((uint8_t *)name);
    h *= 1099511628211ULL;

    return h ? h : 1;
}

char *krffs_scale_size(uint64_t size, char *buffer, size_t buffer_size)
{
    static const int Size_Scale_Threshold = 7;

    assert(buffer && buffer_size > 0);

    if (snprintf(buffer, buffer_size, "%" PRIu64 " KB", size / 1024LU) <= Size_Scale_Threshold) {
        return buffer;
    } else if (snprintf(buffer, buffer_size, "%" PRIu64 " MB", size / 1048576LU) <=
               Size_Scale_Threshold) {
        return buffer;
    } else if (snprintf(buffer, buffer_size, "%" PRIu64 " GB", size / 1073741824LU) <=
               Size_Scale_Threshold) {
        return buffer;
    } else if (snprintf(buffer, buffer_size, "%" PRIu64 " TB", size / 1099511627776LU) <=
               Size_Scale_Threshold) {
        return buffer;
    } else if (snprintf(buffer, buffer_size, "%" PRIu64 " PB", size / 1125899906842624LU) <=
               Size_Scale_Threshold) {
        return buffer;
    } else {
        snprintf(buffer, buffer_size, "%" PRIu64 " B ", size);
    }

    return buffer;
}
