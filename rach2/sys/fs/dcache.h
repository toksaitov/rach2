/* The name cache: one name in one directory, to its node or to no entry. */
#ifndef _VFS_DCACHE_H_
#define _VFS_DCACHE_H_

#include "../lib/kint.h"

struct vfs_mount;

/*
 * The cache keeps the answers of a type's `lookup` for the types that set
 * `name_cache` in their `vfs_type`: a positive entry holds the node a name
 * found, and a negative entry records that the name was not there. Every
 * entry holds its directory too, so that no directory's address is reused
 * while an entry names it. The cache has at most DCACHE_LIMIT entries and
 * gives back the least recently used.
 *
 * The cache answers only for the resolver, after the resolver has checked
 * the caller's search permission on the directory, so a cached entry lets
 * nobody through who could not search there. Each mutator of vfs.c forgets
 * the names it changes, one that removes a name before the type's call as
 * well as after it, and an unmount forgets the mount's entries before the
 * type lets the mount go. A lookup that sleeps while a mutator runs
 * keeps its answer out of the cache (`dcache_lookup`).
 *
 * The cache's lists and counts are under its own spinlock, placed in the
 * order vfs.Readme.md's Locks gives, held only while they change or a
 * found node gains a hold. `dcache_lookup_hit`, `dcache_count_hits` and
 * `dcache_counts` may run without the big lock; every other function runs
 * under it, since it calls the type's `lookup` or `put`, which may sleep
 * and come after the entry has left the lists.
 */

#define DCACHE_LIMIT 8192u

/* The counters of /proc/stat. A miss is a lookup the cache passed to the
 * type, a directory read. */
struct dcache_counts {
    uint32_t hits;
    uint32_t misses;
    uint32_t entries;
    uint32_t negative;
};

/**
 * Look `name` up in `dir` of `mnt`, a mount's base, as the type's `lookup`
 * does and with its results: 0 with the node held in `*node`, -ENOENT, or
 * the type's error. `dir` NULL is the mount's root. A type that does not set
 * `name_cache`, and the names "." and "..", go to the type every time.
 */
int dcache_lookup(struct vfs_mount *mnt, void *dir, const char *name, void **node);

/**
 * The answer of the cache alone for `name` in `dir` of `mnt`, a mount's
 * base, without the big lock: 0 with the node held in `*node`, -ENOENT for
 * a negative entry, or -EAGAIN when the cache has no entry, the type keeps
 * no names there, or the name is "." or "..". The answer is not counted
 * (`dcache_count_hits`). Does not sleep; the caller holds no spinlock.
 */
int dcache_lookup_hit(struct vfs_mount *mnt, void *dir, const char *name, void **node);

/** Count `n` answers of `dcache_lookup_hit` as hits, once the walk that
 * asked has used them, so that a walk begun again under the big lock
 * counts each name once. */
void dcache_count_hits(uint32_t n);

/**
 * `dcache_lookup` for a walk with RESOLVE_CACHED, which may not wait for a
 * device: -EAGAIN when the cache has no entry for `name` on a type that keeps
 * its names there, or when the type does not and stands on a device; the
 * type's lookup otherwise, as for "." and "..".
 */
int dcache_lookup_cached(struct vfs_mount *mnt, void *dir, const char *name, void **node);

/**
 * Forget the entry `name` of `dir` on `mnt`, a mount's base, around a
 * mutator that adds, removes or replaces the name, whether or not it
 * succeeded. It also turns away every lookup under way, whose answer may
 * predate the change. May sleep in the type's `put`.
 */
void dcache_forget(struct vfs_mount *mnt, void *dir, const char *name);

/**
 * Before a mutator that removes or replaces `name` of `dir` on `mnt`, a
 * mount's base: forget the name's entry and every entry in the directory it
 * names, so that the type finds none of the cache's holds on what it
 * removes and frees it at once. Returns the node the name named, as a key
 * for `dcache_forget_dir` and never held, or NULL. May sleep.
 */
void *dcache_forget_victim(struct vfs_mount *mnt, void *dir, const char *name);

/**
 * After that mutator removed the name: forget again the entries in the
 * directory `key` names, which a lookup may have made while the mutator
 * slept. A key whose node was freed meanwhile matches nothing that is not
 * safe to forget. May sleep.
 */
void dcache_forget_dir(struct vfs_mount *mnt, void *key);

/** Forget every entry of `mnt`, a mount's base, for its unmount. May
 * sleep. */
void dcache_forget_mount(struct vfs_mount *mnt);

/** Put the entries given back while their caller held a lock that kept
 * their puts for later, as far as the caller's own locks let it; the VFS
 * calls it when a caller gives back its last file lock. May sleep. */
void dcache_drain(void);

/** The counters, into `c`. */
void dcache_counts(struct dcache_counts *c);

#endif /* _VFS_DCACHE_H_ */
