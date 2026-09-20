/**
 * Check the host stager against the kernel's common hash vectors.
 *
 * host-deps: krffs/krffs_allocator.c krffs/krffs_node.c
 * host-deps: krffs/krffs_file_system.c krffs/krffs_utilities.c
 * host-deps: krffs/krffs_platform.c
 */
#define main krffs_stage_main
#include "../../krffs/stageimage.krffs.c"
#undef main

/** Check fixed results, including unsigned bytes, wrap and the zero remap. */
int main(void)
{
    int failures = 0;

#define HASH_VECTOR(parent, name, djb, mixed)                                                      \
    if (krffs_calculate_djb_hash((uint8_t *)name) != djb || krffs_mix_id(parent, name) != mixed) { \
        fprintf(stderr, "krffs-hash-test: hash vector failed\n");                                  \
        failures++;                                                                                \
    }
#include "krffs_hash_vectors.h"
#undef HASH_VECTOR
    printf("krffs-hash-test: result: %d failures\n", failures);
    return failures != 0;
}
