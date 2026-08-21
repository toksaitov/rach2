#ifndef KRFFS_READER_H
#define KRFFS_READER_H

#include <stddef.h>
#include <stdint.h>

struct krffs_file_system;

/**
 * Locate the regular file at absolute `path` in a mapped KRFFS region and
 * return a pointer to its data.
 *
 * The walk trusts nothing in the region: every node it visits must carry the
 * magic, lie whole inside the region, and have a size that at least covers
 * its own header, so a truncated or corrupted image is refused with the
 * reason written to `errbuf`, not read out of bounds. A path that
 * resolves to a directory is refused too; this reads files.
 *
 * On success `*data` points into the mapping and `*size` is the file's data
 * size. Returns 0 on success and -1 on refusal.
 */
int krffs_read_regular(struct krffs_file_system *file_system, const char *path, uint8_t **data,
                       uint64_t *size, char *errbuf, size_t errlen);

#endif
