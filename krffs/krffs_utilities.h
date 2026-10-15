/*
 * krffs_utilities.h: the name hash and the directory identifier of KRFFS,
 * and a size printer for the host tools.
 */
#ifndef KRFFS_UTILITIES_H
#define KRFFS_UTILITIES_H

#include <stddef.h>
#include <stdint.h>

/**
 * Returns Bernstein's djb2 hash of the NUL-terminated `string`, in the xor
 * form of http://www.cse.yorku.ca/~oz/hash.html: from 5381, each byte `c`
 * makes the hash `hash * 33 ^ c`, in 64 bits. A node's `id` is this hash of
 * its name, alone or mixed with the parent's `id`; the kernel's
 * `krffs_calculate_djb_hash` is the same function.
 */
uint64_t krffs_calculate_djb_hash(uint8_t *string);

/**
 * Returns the id of a node named `name` in the directory `parent_id`: the
 * djb2 hash of the name mixed with `parent_id` by the 64-bit FNV prime, and 1
 * in place of 0, which is the root directory's. The kernel's `krffs_mix_id`
 * (rach2/sys/fs/krffs.c), which gives a directory its id, is the same
 * function, so a directory the stager or the FUSE driver makes has the id the
 * kernel would give it.
 */
uint64_t krffs_mix_id(uint64_t parent_id, const char *name);

/**
 * Prints the byte count `size` into `buffer` as a whole number of KB, MB,
 * GB, TB or PB, in units of 1024, taking the first unit whose text is at
 * most seven characters, or else as bytes, "N B " with a trailing space. A
 * size under 1024 is "0 KB". Returns `buffer`.
 */
char *krffs_scale_size(uint64_t size, char *buffer, size_t buffer_size);

#endif
