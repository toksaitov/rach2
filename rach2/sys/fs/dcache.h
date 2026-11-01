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
 * Every function runs under the big kernel lock; the cache's lists change
 * only between sleeps, and a `put`, which may sleep, comes after the entry
 * has left them.
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

/** The counters, into `c`. */
void dcache_counts(struct dcache_counts *c);

#endif /* _VFS_DCACHE_H_ */
