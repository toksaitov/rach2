/* The name cache of dcache.h: a hash of (mount, directory, name) to a held
 * node or to no entry, with a list from the newest entry to the oldest. */

#include "dcache.h"
#include "vfs.h"
#include "../lib/kerrno.h"
#include "../mm/kheap.h"

#define DCACHE_BUCKETS 4096u /* a power of two, half the entries */

struct dentry {
    struct dentry *hnext; /* the bucket's chain; the list of `pending` once out */
    struct dentry *newer;
    struct dentry *older;
    struct vfs_mount *mnt; /* a mount's base */
    void *dir;             /* held */
    void *node;            /* held; NULL for a negative entry */
    uint32_t hash;
    uint32_t len;
    char name[1]; /* `len` bytes and a NUL */
};

static struct dentry *buckets[DCACHE_BUCKETS];
static struct dentry *newest;
static struct dentry *oldest;

/* Entries out of the cache whose holds wait for a caller that holds no
 * other mount's namespace lock (`release`). */
static struct dentry *pending;

static uint32_t entries;
static uint32_t negatives;
static uint32_t hits;
static uint32_t misses;

/* Moves at every forget; a lookup that saw it move while it slept in the
 * type keeps its answer out of the cache. */
static uint32_t generation;

/** Whether `mnt` keeps its names in the cache: a type that sets
 * `name_cache` and whose `hold` keeps a node. */
static int cacheable(const struct vfs_mount *mnt)
{
    const struct vfs_ops *ops = mnt->type->ops;

    return mnt->type->name_cache && ops->lookup && ops->hold && ops->put;
}

/** Whether `name` is "." or "..", which the cache never keeps: the first
 * costs nothing and the second changes when its directory moves. */
static int dot_name(const char *name)
{
    return name[0] == '.' && (name[1] == '\0' || (name[1] == '.' && name[2] == '\0'));
}

/** The FNV-1a hash of `dir` and `name`, with the name's length into
 * `*len`. */
static uint32_t hash_of(const void *dir, const char *name, uint32_t *len)
{
    uintptr_t d = (uintptr_t)dir;
    uint32_t h = 2166136261u;
    uint32_t i;

    for (i = 0; i < sizeof(d); i++) {
        h = (h ^ (uint32_t)(d & 0xffu)) * 16777619u;
        d >>= 8;
    }
    for (i = 0; name[i] != '\0'; i++) {
        h = (h ^ (uint8_t)name[i]) * 16777619u;
    }
    *len = i;
    return h;
}

/** The entry of `name` in `dir` of `mnt`, or NULL. */
static struct dentry *find(const struct vfs_mount *mnt, const void *dir, const char *name,
                           uint32_t len, uint32_t h)
{
    struct dentry *e;
    uint32_t i;

    for (e = buckets[h & (DCACHE_BUCKETS - 1)]; e; e = e->hnext) {
        if (e->hash != h || e->mnt != mnt || e->dir != dir || e->len != len) {
            continue;
        }
        for (i = 0; i < len && e->name[i] == name[i]; i++) {
        }
        if (i == len) {
            return e;
        }
    }
    return 0;
}

/** Take `e` off the list of ages. */
static void age_unlink(struct dentry *e)
{
    if (e->newer) {
        e->newer->older = e->older;
    } else {
        newest = e->older;
    }
    if (e->older) {
        e->older->newer = e->newer;
    } else {
        oldest = e->newer;
    }
    e->newer = e->older = 0;
}

/** Put `e` at the newest end of the list of ages. */
static void age_push(struct dentry *e)
{
    e->newer = 0;
    e->older = newest;
    if (newest) {
        newest->newer = e;
    } else {
        oldest = e;
    }
    newest = e;
}

/** Take `e` out of the cache: its bucket, the list of ages and the counts.
 * Its holds stay until `release`. */
static void take_out(struct dentry *e)
{
    struct dentry **pp = &buckets[e->hash & (DCACHE_BUCKETS - 1)];

    while (*pp != e) {
        pp = &(*pp)->hnext;
    }
    *pp = e->hnext;
    e->hnext = 0;
    age_unlink(e);
    entries--;
    if (!e->node) {
        negatives--;
    }
}

/** Put the holds of `e`, out of the cache, and free it. May sleep. */
static void put_entry(struct dentry *e)
{
    const struct vfs_ops *ops = e->mnt->type->ops;

    if (e->node) {
        ops->put(e->mnt, e->node);
    }
    ops->put(e->mnt, e->dir);
    kfree(e);
}

/**
 * Put the holds of `e`, out of the cache, now, or later when the caller
 * holds the namespace lock of another mount: a type's `put` takes its own
 * mount's lock, and two callers that each held one mount's lock and waited
 * for the other's would wait for good.
 */
static void release(struct dentry *e)
{
    if (vfs_ns_held_other(e->mnt)) {
        e->hnext = pending;
        pending = e;
        return;
    }
    put_entry(e);
}

/** Put what `release` left for later, as far as the caller's locks let it.
 * A put may sleep, and the list is read again from its head after each. */
static void drain(void)
{
    struct dentry **pp = &pending;
    struct dentry *e;

    while ((e = *pp) != 0) {
        if (vfs_ns_held_other(e->mnt)) {
            pp = &e->hnext;
            continue;
        }
        *pp = e->hnext;
        put_entry(e);
        pp = &pending;
    }
}

/** Give back the oldest entries while there are more than DCACHE_LIMIT. */
static void trim(void)
{
    struct dentry *e;

    while (entries > DCACHE_LIMIT && oldest) {
        e = oldest;
        take_out(e);
        release(e);
    }
}

/**
 * Keep the answer of a lookup of `name` in `dir` of `mnt`, `node` or NULL
 * for none, when no forget ran since the lookup began at `gen` and no other
 * walk kept one meanwhile. kmalloc may sleep, so both are judged after it.
 */
static void keep(struct vfs_mount *mnt, void *dir, const char *name, uint32_t len, uint32_t h,
                 void *node, uint32_t gen)
{
    const struct vfs_ops *ops = mnt->type->ops;
    struct dentry *e = kmalloc(sizeof(*e) + len);
    uint32_t i;

    if (!e) {
        return;
    }
    if (gen != generation || find(mnt, dir, name, len, h)) {
        kfree(e);
        return;
    }
    e->mnt = mnt;
    e->dir = dir;
    e->node = node;
    e->hash = h;
    e->len = len;
    for (i = 0; i < len; i++) {
        e->name[i] = name[i];
    }
    e->name[len] = '\0';
    ops->hold(mnt, dir);
    if (node) {
        ops->hold(mnt, node);
    } else {
        negatives++;
    }
    e->hnext = buckets[h & (DCACHE_BUCKETS - 1)];
    buckets[h & (DCACHE_BUCKETS - 1)] = e;
    age_push(e);
    entries++;
    trim();
}

int dcache_lookup(struct vfs_mount *mnt, void *dir, const char *name, void **node)
{
    const struct vfs_ops *ops = mnt->type->ops;
    struct dentry *e;
    uint32_t len, h, gen;
    int rc;

    if (!cacheable(mnt) || dot_name(name)) {
        return ops->lookup(mnt, dir, name, node);
    }
    if (pending) {
        drain();
    }
    if (!dir) {
        dir = mnt->root;
    }
    h = hash_of(dir, name, &len);
    e = find(mnt, dir, name, len, h);
    if (e) {
        hits++;
        age_unlink(e);
        age_push(e);
        if (!e->node) {
            return -ENOENT;
        }
        ops->hold(mnt, e->node);
        *node = e->node;
        return 0;
    }
    misses++;
    gen = generation;
    rc = ops->lookup(mnt, dir, name, node);
    if (rc == 0 || rc == -ENOENT) {
        keep(mnt, dir, name, len, h, rc == 0 ? *node : 0, gen);
    }
    return rc;
}

void dcache_forget(struct vfs_mount *mnt, void *dir, const char *name)
{
    struct dentry *e;
    uint32_t len, h;

    generation++;
    if (!cacheable(mnt) || entries == 0) {
        return;
    }
    if (!dir) {
        dir = mnt->root;
    }
    h = hash_of(dir, name, &len);
    e = find(mnt, dir, name, len, h);
    if (e) {
        take_out(e);
        release(e);
    }
}

void *dcache_victim(struct vfs_mount *mnt, void *dir, const char *name)
{
    struct dentry *e;
    uint32_t len, h;
    void *node;

    if (!cacheable(mnt) || dot_name(name)) {
        return 0;
    }
    if (!dir) {
        dir = mnt->root;
    }
    h = hash_of(dir, name, &len);
    e = find(mnt, dir, name, len, h);
    if (e) {
        if (e->node) {
            mnt->type->ops->hold(mnt, e->node);
        }
        return e->node;
    }
    return mnt->type->ops->lookup(mnt, dir, name, &node) == 0 ? node : 0;
}

/** Take every entry of `mnt` for which `dir` is NULL or the directory out
 * of the cache, and put them. May sleep. */
static void forget_where(struct vfs_mount *mnt, const void *dir)
{
    struct dentry *out = 0;
    struct dentry *e, *next;

    generation++;
    for (e = oldest; e; e = next) {
        next = e->newer;
        if (e->mnt == mnt && (!dir || e->dir == dir)) {
            take_out(e);
            e->hnext = out;
            out = e;
        }
    }
    for (e = out; e; e = next) {
        next = e->hnext;
        release(e);
    }
}

void dcache_forget_dir(struct vfs_mount *mnt, void *victim, int removed)
{
    if (!victim) {
        return;
    }
    if (removed && entries > 0) {
        forget_where(mnt, victim);
    }
    mnt->type->ops->put(mnt, victim);
}

void dcache_forget_mount(struct vfs_mount *mnt)
{
    struct dentry **pp = &pending;
    struct dentry *e;

    if (!cacheable(mnt)) {
        return;
    }
    forget_where(mnt, 0);
    while ((e = *pp) != 0) {
        if (e->mnt != mnt) {
            pp = &e->hnext;
            continue;
        }
        *pp = e->hnext;
        put_entry(e);
        pp = &pending;
    }
}

void dcache_counts(struct dcache_counts *c)
{
    c->hits = hits;
    c->misses = misses;
    c->entries = entries;
    c->negative = negatives;
}
