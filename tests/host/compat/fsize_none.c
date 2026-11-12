/* RLIMIT_FSIZE for a host test whose rows set no limit (fs/vfs.h). */
#include "../../../rach2/sys/fs/vfs.h"

/** No limit is set, so every `end` fits: 0. */
int32_t vfs_fsize_refused(uint64_t end)
{
    (void)end;
    return 0;
}

/** No limit is set, so every growth fits: 0. */
int32_t vfs_newsize_ok(uint64_t size, uint64_t end)
{
    (void)size;
    (void)end;
    return 0;
}
