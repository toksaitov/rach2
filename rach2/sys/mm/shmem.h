#ifndef _SHMEM_H_
#define _SHMEM_H_

#include "../lib/kint.h"

/*
 * The bytes of a file kept in memory: a table of frames, one per page, a page
 * allocated when it is first written or mapped, and a size. A tmpfs file and
 * the memory of a shared anonymous mapping are one each. The table holds one
 * reference to each frame, and a shared mapping holds one more for every
 * page it maps, so a frame a process still maps outlives a truncation.
 */
struct shmem {
    uintptr_t *pages; /* physical frames, 0 for a page never written */
    uint32_t npages;  /* the table's length */
    uint64_t size;
};

/** Read `count` bytes at `off` into `buf`, a hole as zeros: the bytes read,
 * fewer at the end of the data. */
int32_t shmem_read(struct shmem *sh, void *buf, uint32_t count, uint64_t off);

/** Write `count` bytes of `buf` at `off`, growing the size: the bytes
 * written, or -ENOSPC or -EFBIG. */
int32_t shmem_write(struct shmem *sh, const void *buf, uint32_t count, uint64_t off);

/** Set the size, freeing the pages past it; 0, or -EFBIG. */
int shmem_truncate(struct shmem *sh, uint64_t size);

/** The frame of page `pgoff`, allocated zeroed when there is none, with a
 * reference taken for the caller's mapping; 0 when no frame can be had. */
uintptr_t shmem_map_page(struct shmem *sh, uint32_t pgoff);

/** Free every page; the size is 0 after. */
void shmem_free(struct shmem *sh);

/* Policy: the largest size a file in memory may have, 1 GiB; a machine of
 * this kind has less memory than that. */
#define SHMEM_MAX_SIZE 0x40000000ull

#endif /* _SHMEM_H_ */
