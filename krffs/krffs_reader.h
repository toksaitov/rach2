/*
 * krffs_reader.h: a lookup of a file by path in an image that may be damaged,
 * for readfile.krffs.
 */
#ifndef KRFFS_READER_H
#define KRFFS_READER_H

#include <stddef.h>
#include <stdint.h>

struct krffs_file_system;

/**
 * Finds the regular file at the absolute `path` in a mapped KRFFS region,
 * points `*data` at its data in the mapping and sets `*size` to its
 * `data_size`. Each component is the reserved node of that name whose
 * `parent_id` is the `id` of the one before, starting from 0 at the root.
 *
 * Every node the lookup reads is checked first: it has the magic number,
 * lies whole inside the region, and has a `size` of at least a header that
 * ends inside the region. The file's `data_size` must fit its node. A
 * truncated or damaged image so fails with a reason and is not read out of
 * bounds.
 *
 * Returns 0, or -1 with the reason in `errbuf`, of `errlen` bytes, when a
 * check fails, `path` is not absolute or names no node, or the node is a
 * directory, a FIFO or a socket.
 */
int krffs_read_regular(struct krffs_file_system *file_system, const char *path, uint8_t **data,
                       uint64_t *size, char *errbuf, size_t errlen);

#endif
