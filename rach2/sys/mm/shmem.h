/*
 * shmem: the bytes of a file kept in memory, for a tmpfs file, shared
 * anonymous memory and the pages that shared mappings of a disk file map.
 */
#ifndef _SHMEM_H_
#define _SHMEM_H_

#include "../lib/kdef.h"
#include "../lib/kint.h"
#include "vm.h"

/* The pages one leaf of a table reaches: a page of entries. */
#define SHMEM_LEAF 1024u

/*
 * A count of the pages that files hold against a limit, which a tmpfs mount
 * keeps for its files; a limit of 0 is none. Both are as wide as the frame
 * counts, so a mount on x86-64 may hold more than 16 TiB of pages. A quota
 * with `cap` reads its limit from it at each page taken instead, 0 then
 * meaning no room: memfd_secret's count of a user's pages, against the
 * RLIMIT_MEMLOCK of the process that touches one.
 */
struct shmem_quota {
    size_t used;
    size_t limit;
    size_t (*cap)(void);
};

/*
 * The policy mbind gave the pages [start, end) of a file in memory, Linux's
 * shared policy of a shmem inode: it belongs to the pages and not to a
 * mapping, so that every mapping of them in every process reads it, and it
 * stays when the last mapping goes. A file's records are in page order and
 * apart (mempolicy.c); `pol` is a copy, whose `refs` nothing reads.
 */
struct shmem_policy {
    struct shmem_policy *next;
    uint32_t start, end;
    struct vm_policy pol;
};

/*
 * The bytes of a file kept in memory: a table of frames, one per page, a page
 * allocated when it is first written or mapped, and a size. A tmpfs file and
 * the memory of a shared anonymous mapping are one each. The table is a
 * directory of leaves of SHMEM_LEAF entries, a leaf made when a page in its
 * span is, so a hole costs nothing and a file reaches SHMEM_MAX_SIZE without
 * one array of that length. The table holds one reference to each frame, and
 * a shared mapping holds one more for every page it maps. `maps` counts the
 * mappings (filemap.c) that map the memory, and while it is not 0 a
 * truncation takes the pages it frees out of them first, so that a mapping
 * never keeps a page the file no longer has. `policies` are the policies
 * mbind gave its pages, which a truncation leaves and `shmem_free` frees.
 */
struct shmem {
    uintptr_t **leaves; /* the directory: each leaf's frames, NULL for none */
    uint32_t nleaves;   /* the directory's length */
    uint32_t npages;    /* the pages the directory reaches, nleaves leaves' */
    uint32_t held;      /* the frames the table holds */
    /* The file's size in bytes. */
    uint64_t size;
    uint32_t maps;             /* the mappings of it, which a truncation reaches */
    struct shmem_quota *quota; /* the count its pages are held against, or NULL */
    struct shmem_policy *policies;
};

/** Whether the quota of `sh` has no room for another page, which a mapping
 * that touches a page it lacks answers with SIGBUS, as Linux does. */
int shmem_quota_full(const struct shmem *sh);

/** Read `count` bytes at `off` into `buf`, a hole as zeros: the bytes read,
 * fewer at the end of the data, or -EIO when no temporary mapping was free
 * before a byte was read. */
int32_t shmem_read(struct shmem *sh, void *buf, uint32_t count, uint64_t off);

/** Write `count` bytes of `buf` at `off`, growing the size: the bytes
 * written, fewer when a page could not be had or mapped part way; -ENOSPC
 * when none could be written, and -EFBIG for a range that passes
 * SHMEM_MAX_SIZE. */
int32_t shmem_write(struct shmem *sh, const void *buf, uint32_t count, uint64_t off);

/** Set the size, freeing the pages past it, which it first takes out of
 * every region that maps them (`filemap_shmem_cut`); the part of the last
 * page past the new end is zeroed, so that it reads as zeros if the file
 * grows again, as on Linux. 0, or -EFBIG past SHMEM_MAX_SIZE. */
int shmem_truncate(struct shmem *sh, uint64_t size);

/** The frame of page `pgoff`, allocated zeroed when there is none, with a
 * reference taken for the caller's mapping; 0 when no frame can be had, the
 * quota has no room, or the page lies at or past SHMEM_MAX_SIZE. */
uintptr_t shmem_map_page(struct shmem *sh, uint32_t pgoff);

/** The frame of page `pgoff` if the memory has one, else 0; nothing is
 * allocated and no reference is taken. */
uintptr_t shmem_resident(const struct shmem *sh, uint32_t pgoff);

/** Count resident pages in the inclusive range [first, last], without
 * visiting absent leaves. The caller keeps `sh` alive; the walk offers
 * scheduling between leaves and keeps no pointer across those points. */
uint64_t shmem_count(const struct shmem *sh, uint32_t first, uint32_t last);

/** Make `phys`, a frame the caller filled, page `pgoff` unless the memory
 * has one by now; the table takes the caller's reference, and the caller's
 * frame is given back when another is there. Returns the page's frame with
 * a reference taken for the caller's mapping, or 0, with the caller's frame
 * given back, when the table cannot reach the page. */
uintptr_t shmem_adopt(struct shmem *sh, uint32_t pgoff, uintptr_t phys);

/** Give every page of [off, off + len) a frame, zeroed where there was none,
 * as fallocate(2) does, and leave the size as it is; 0, or -ENOSPC when a
 * frame or the quota's room runs out, or -EFBIG past SHMEM_MAX_SIZE. */
int shmem_allocate(struct shmem *sh, uint64_t off, uint64_t len);

/** Make [off, off + len) read as zeros, as a punched hole does: a page it
 * covers whole is freed while no mapping maps the memory and zeroed in place
 * while one does, and the parts of the pages at its ends are zeroed. The
 * walk passes over a leaf the table does not have whole. */
void shmem_punch(struct shmem *sh, uint64_t off, uint64_t len);

/** Free every page and the policies of the pages; the size is 0 after.
 * Nothing here sleeps. */
void shmem_free(struct shmem *sh);

/**
 * Free as `shmem_free` does, giving way to the big lock's waiters and the
 * tick's switch after each leaf of SHMEM_LEAF pages (`sched_give_way`), for
 * a caller that may sleep and whose `sh` no other thread reaches. There are
 * three: tmpfs, for a file that has lost its last name and its last hold,
 * `filemap_release`, for shared anonymous memory's own pages once the
 * mapping is off its list and its last region is gone, and the i915
 * object's free that took the GEM lock itself.
 */
void shmem_free_yielding(struct shmem *sh);

/* The largest size a file in memory may have, the last page a 32-bit page
 * index reaches, as Linux's tmpfs has it on i386: 16 TiB less a page. */
#define SHMEM_MAX_SIZE 0xFFFFFFFF000ull

#endif /* _SHMEM_H_ */
