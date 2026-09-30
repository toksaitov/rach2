#ifndef _SHMEM_H_
#define _SHMEM_H_

#include "../lib/kint.h"

/*
 * The bytes of a file kept in memory: a table of frames, one per page, a page
 * allocated when it is first written or mapped, and a size. A tmpfs file and
 * the memory of a shared anonymous mapping are one each. The table holds one
 * reference to each frame, and a shared mapping holds one more for every
 * page it maps. `maps` counts the mappings (filemap.c) that map the memory,
 * and while it is not 0 a truncation takes the pages it frees out of them
 * first, so that a mapping never keeps a page the file no longer has.
 */
struct shmem {
    uintptr_t *pages; /* physical frames, 0 for a page never written */
    uint32_t npages;  /* the table's length */
    uint64_t size;
    uint32_t maps; /* the mappings of it, which a truncation reaches */
};

/** Read `count` bytes at `off` into `buf`, a hole as zeros: the bytes read,
 * fewer at the end of the data. */
int32_t shmem_read(struct shmem *sh, void *buf, uint32_t count, uint64_t off);

/** Write `count` bytes of `buf` at `off`, growing the size: the bytes
 * written, or -ENOSPC or -EFBIG. */
int32_t shmem_write(struct shmem *sh, const void *buf, uint32_t count, uint64_t off);

/** Set the size, freeing the pages past it, which it first takes out of
 * every region that maps them (`filemap_shmem_cut`); 0, or -EFBIG. */
int shmem_truncate(struct shmem *sh, uint64_t size);

/** The frame of page `pgoff`, allocated zeroed when there is none, with a
 * reference taken for the caller's mapping; 0 when no frame can be had, or
 * when the page lies at or past SHMEM_MAX_SIZE. */
uintptr_t shmem_map_page(struct shmem *sh, uint32_t pgoff);

/** The frame of page `pgoff` if the memory has one, else 0; nothing is
 * allocated and no reference is taken. */
uintptr_t shmem_resident(const struct shmem *sh, uint32_t pgoff);

/** Make `phys`, a frame the caller filled, page `pgoff` unless the memory
 * has one by now; the table takes the caller's reference, and the caller's
 * frame is given back when another is there. Returns the page's frame with
 * a reference taken for the caller's mapping, or 0 when the table cannot
 * reach the page. */
uintptr_t shmem_adopt(struct shmem *sh, uint32_t pgoff, uintptr_t phys);

/** Free every page; the size is 0 after. */
void shmem_free(struct shmem *sh);

/* Policy: the largest size a file in memory may have, 1 GiB; a machine of
 * this kind has less memory than that. */
#define SHMEM_MAX_SIZE 0x40000000ull

#endif /* _SHMEM_H_ */
