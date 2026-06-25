#include "krffs_utilities.h"

#include <assert.h>
#include <inttypes.h>
#include <stdio.h>

/**
 * Computes Bernstein's djb2 hash over the NUL-terminated `string`.
 *
 * The hash starts at 5381 and folds each byte in as
 * `hash = (hash * 33) ^ c`, the xor variant documented at
 * http://www.cse.yorku.ca/~oz/hash.html.
 */
uint64_t krffs_calculate_djb_hash(uint8_t *string)
{
    assert(string);

    uint64_t hash = 5381;

    int character;
    while ((character = *string++)) {
        hash =
            ((hash << 5) + hash) ^ character;
    }

    return hash;
}

/**
 * Formats `size` bytes into `buffer` using the largest binary unit whose
 * value reads as a short number, and returns `buffer`.
 *
 * It tries KB, MB, GB, TB, then PB in turn, keeping the first rendering
 * whose printed length is at most a few digits; if even petabytes overflow
 * that width it falls back to raw bytes. The conversion uses 1024-based
 * (binary) units throughout.
 */
char *krffs_scale_size(uint64_t size, char *buffer, size_t buffer_size)
{
    static const int Size_Scale_Threshold = 7;

    assert(buffer && buffer_size > 0);

    if (snprintf(
            buffer, buffer_size,
            "%" PRIu64 " KB", size / 1024LU
        ) <= Size_Scale_Threshold) {
        return buffer;
    } else if (snprintf(
                   buffer, buffer_size,
                   "%" PRIu64 " MB", size / 1048576LU
               ) <= Size_Scale_Threshold) {
        return buffer;
    } else if (snprintf(
                   buffer, buffer_size,
                   "%" PRIu64 " GB", size / 1073741824LU
               ) <= Size_Scale_Threshold) {
        return buffer;
    } else if (snprintf(
                   buffer, buffer_size,
                   "%" PRIu64 " TB", size / 1099511627776LU
               ) <= Size_Scale_Threshold) {
        return buffer;
    } else if (snprintf(
                   buffer, buffer_size,
                   "%" PRIu64 " PB", size / 1125899906842624LU
               ) <= Size_Scale_Threshold) {
        return buffer;
    } else {
        snprintf(
            buffer, buffer_size,
            "%" PRIu64 " B ", size
        );
    }

    return buffer;
}
