/*
 * krffs_hash_hosttest.c: the host stager's hashes, `krffs_calculate_djb_hash`
 * of krffs/krffs_utilities.c and `krffs_mix_id` of krffs/stageimage.krffs.c,
 * against the vectors krffs_hosttest.c checks the kernel's hashes against,
 * so that a node the stager writes and the same node made on Rach2 get the
 * same id.
 *
 * host-deps: krffs/krffs_allocator.c krffs/krffs_node.c
 * host-deps: krffs/krffs_file_system.c krffs/krffs_utilities.c
 * host-deps: krffs/krffs_platform.c
 */
#define main krffs_stage_main
#include "../../krffs/stageimage.krffs.c"
#undef main

#include "krffs_hash_vectors.h"

/**
 * Check every vector of krffs_hash_vectors.h: names with bytes above 127,
 * which must hash as unsigned, sums that wrap modulo 2^64, and a mixed id
 * of zero, which must become 1. Returns 0 when every vector matches.
 */
int main(void)
{
    int failures = 0;

#define HASH_VECTOR(parent, name, djb, mixed)                                                      \
    if (krffs_calculate_djb_hash((uint8_t *)name) != djb || krffs_mix_id(parent, name) != mixed) { \
        fprintf(stderr, "krffs-hash-test: hash vector failed\n");                                  \
        failures++;                                                                                \
    }
    KRFFS_HASH_VECTORS
#undef HASH_VECTOR
    printf("krffs-hash-test: result: %d failures\n", failures);
    return failures != 0;
}
