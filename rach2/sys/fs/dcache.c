/* The name cache of dcache.h: a hash of (mount, directory, name) to a held
 * node or to no entry, with a list from the newest entry to the oldest. */

#include "dcache.h"
#include "vfs.h"
#include "../lib/kerrno.h"
#include "../lib/spinlock.h"
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

/* Guards every static below: the buckets, the list of ages, `pending`, the
 * counts and the generation. It is taken with interrupts off and held only
 * while the lists change or an entry's node gains a hold; a type's `put`,
 * which may sleep, runs after it is let go of, on an entry already out. */
static struct spinlock dcache_lock = SPINLOCK_INIT;

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
    uint32_t flags;

    if (vfs_ns_held_other(e->mnt)) {
        flags = spin_lock_irqsave(&dcache_lock);
        e->hnext = pending;
        pending = e;
        spin_unlock_irqrestore(&dcache_lock, flags);
        return;
    }
    put_entry(e);
}

/** `release` each entry of the list `out`, linked by `hnext`, out of the
 * cache. May sleep. */
static void release_list(struct dentry *out)
{
    struct dentry *e, *next;

    for (e = out; e; e = next) {
        next = e->hnext;
        e->hnext = 0;
        release(e);
    }
}

/** Put what `release` left for later, as far as the caller's locks let it:
 * the entries the caller may put leave the list under the lock, and are put
 * after it, since a put may sleep. When `mnt` is not NULL, every entry of
 * that mount goes, for its unmount. */
static void drain(const struct vfs_mount *mnt)
{
    struct dentry **pp;
    struct dentry *e;
    struct dentry *out = 0;
    uint32_t flags = spin_lock_irqsave(&dcache_lock);

    pp = &pending;
    while ((e = *pp) != 0) {
        if (mnt ? e->mnt != mnt : vfs_ns_held_other(e->mnt)) {
            pp = &e->hnext;
            continue;
        }
        *pp = e->hnext;
        e->hnext = out;
        out = e;
    }
    spin_unlock_irqrestore(&dcache_lock, flags);
    while ((e = out) != 0) {
        out = e->hnext;
        put_entry(e);
    }
}

/** Take the oldest entries out while there are more than DCACHE_LIMIT, onto
 * the list `*out` for `release_list`. The caller holds `dcache_lock`. */
static void trim(struct dentry **out)
{
    struct dentry *e;

    while (entries > DCACHE_LIMIT && oldest) {
        e = oldest;
        take_out(e);
        e->hnext = *out;
        *out = e;
    }
}

/**
 * Keep the answer of a lookup of `name` in `dir` of `mnt`, `node` or NULL
 * for none, when no forget ran since the lookup began at `gen` and no other
 * walk kept one meanwhile. kmalloc may sleep, so both are judged after it,
 * under the lock, which the type's `hold` runs under (`name_cache`).
 */
static void keep(struct vfs_mount *mnt, void *dir, const char *name, uint32_t len, uint32_t h,
                 void *node, uint32_t gen)
{
    const struct vfs_ops *ops = mnt->type->ops;
    struct dentry *e = kmalloc(sizeof(*e) + len);
    struct dentry *out = 0;
    uint32_t i, flags;

    if (!e) {
        return;
    }
    flags = spin_lock_irqsave(&dcache_lock);
    if (gen != generation || find(mnt, dir, name, len, h)) {
        spin_unlock_irqrestore(&dcache_lock, flags);
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
    trim(&out);
    spin_unlock_irqrestore(&dcache_lock, flags);
    release_list(out);
}

/**
 * The entry of `name` in `dir` of `mnt` as `dcache_lookup` answers from the
 * cache: 0 with the node held in `*node`, -ENOENT for a negative entry, or
 * -EAGAIN when there is none. The entry becomes the newest; the caller
 * counts the hit (`dcache_count_hits`). Takes and gives back the lock; the
 * hold is taken under it, while the entry holds the node, so a node that
 * gains a hold here already has one.
 */
static int hit(struct vfs_mount *mnt, void *dir, const char *name, uint32_t len, uint32_t h,
               void **node)
{
    struct dentry *e;
    uint32_t flags = spin_lock_irqsave(&dcache_lock);
    int rc = -EAGAIN;

    e = find(mnt, dir, name, len, h);
    if (e) {
        age_unlink(e);
        age_push(e);
        rc = -ENOENT;
        if (e->node) {
            mnt->type->ops->hold(mnt, e->node);
            *node = e->node;
            rc = 0;
        }
    }
    spin_unlock_irqrestore(&dcache_lock, flags);
    return rc;
}

int dcache_lookup(struct vfs_mount *mnt, void *dir, const char *name, void **node)
{
    const struct vfs_ops *ops = mnt->type->ops;
    uint32_t len, h, gen, flags;
    int rc;

    if (!cacheable(mnt) || dot_name(name)) {
        return ops->lookup(mnt, dir, name, node);
    }
    if (pending) {
        drain(0);
    }
    if (!dir) {
        dir = mnt->root;
    }
    h = hash_of(dir, name, &len);
    rc = hit(mnt, dir, name, len, h, node);
    if (rc != -EAGAIN) {
        dcache_count_hits(1);
        return rc;
    }
    flags = spin_lock_irqsave(&dcache_lock);
    misses++;
    gen = generation;
    spin_unlock_irqrestore(&dcache_lock, flags);
    rc = ops->lookup(mnt, dir, name, node);
    if (rc == 0 || rc == -ENOENT) {
        keep(mnt, dir, name, len, h, rc == 0 ? *node : 0, gen);
    }
    return rc;
}

int dcache_lookup_cached(struct vfs_mount *mnt, void *dir, const char *name, void **node)
{
    int rc;

    if (dot_name(name)) {
        return mnt->type->ops->lookup(mnt, dir, name, node);
    }
    if (!cacheable(mnt)) {
        return mnt->dev ? -EAGAIN : mnt->type->ops->lookup(mnt, dir, name, node);
    }
    rc = dcache_lookup_hit(mnt, dir, name, node);
    if (rc != -EAGAIN) {
        dcache_count_hits(1);
    }
    return rc;
}

int dcache_lookup_hit(struct vfs_mount *mnt, void *dir, const char *name, void **node)
{
    uint32_t len, h;

    if (!cacheable(mnt) || dot_name(name)) {
        return -EAGAIN;
    }
    if (!dir) {
        dir = mnt->root;
    }
    h = hash_of(dir, name, &len);
    return hit(mnt, dir, name, len, h, node);
}

void dcache_count_hits(uint32_t n)
{
    uint32_t flags;

    if (n == 0) {
        return;
    }
    flags = spin_lock_irqsave(&dcache_lock);
    hits += n;
    spin_unlock_irqrestore(&dcache_lock, flags);
}

/** Take the entry of `name` in `dir`, a directory, of `mnt` out of the
 * cache and move the generation, under the lock; the entry, for `release`,
 * or NULL. */
static struct dentry *take_name(struct vfs_mount *mnt, void *dir, const char *name)
{
    struct dentry *e = 0;
    uint32_t len, h;
    uint32_t flags = spin_lock_irqsave(&dcache_lock);

    generation++;
    if (entries > 0) {
        h = hash_of(dir, name, &len);
        e = find(mnt, dir, name, len, h);
        if (e) {
            take_out(e);
        }
    }
    spin_unlock_irqrestore(&dcache_lock, flags);
    return e;
}

void dcache_forget(struct vfs_mount *mnt, void *dir, const char *name)
{
    struct dentry *e;
    uint32_t flags;

    if (!cacheable(mnt)) {
        flags = spin_lock_irqsave(&dcache_lock);
        generation++;
        spin_unlock_irqrestore(&dcache_lock, flags);
        return;
    }
    e = take_name(mnt, dir ? dir : mnt->root, name);
    if (e) {
        release(e);
    }
}

/** Take every entry of `mnt` for which `dir` is NULL or the directory out
 * of the cache, and put them. May sleep. */
static void forget_where(struct vfs_mount *mnt, const void *dir)
{
    struct dentry *out = 0;
    struct dentry *e, *next;
    uint32_t flags = spin_lock_irqsave(&dcache_lock);

    generation++;
    for (e = oldest; e; e = next) {
        next = e->newer;
        if (e->mnt == mnt && (!dir || e->dir == dir)) {
            take_out(e);
            e->hnext = out;
            out = e;
        }
    }
    spin_unlock_irqrestore(&dcache_lock, flags);
    release_list(out);
}

void *dcache_forget_victim(struct vfs_mount *mnt, void *dir, const char *name)
{
    const struct vfs_ops *ops = mnt->type->ops;
    struct dentry *e;
    void *node = 0;
    uint32_t flags;

    if (!cacheable(mnt) || dot_name(name)) {
        flags = spin_lock_irqsave(&dcache_lock);
        generation++;
        spin_unlock_irqrestore(&dcache_lock, flags);
        return 0;
    }
    if (!dir) {
        dir = mnt->root;
    }
    e = take_name(mnt, dir, name);
    if (e) {
        node = e->node;
    } else if (entries == 0) {
        return 0;
    } else if (ops->lookup(mnt, dir, name, &node) == 0) {
        ops->put(mnt, node);
    } else {
        node = 0;
    }
    if (node && entries > 0) {
        forget_where(mnt, node);
    }
    if (e) {
        release(e);
    }
    return node;
}

void dcache_forget_dir(struct vfs_mount *mnt, void *key)
{
    if (key && entries > 0 && cacheable(mnt)) {
        forget_where(mnt, key);
    }
}

void dcache_forget_mount(struct vfs_mount *mnt)
{
    if (!cacheable(mnt)) {
        return;
    }
    forget_where(mnt, 0);
    drain(mnt);
}

void dcache_drain(void)
{
    if (pending) {
        drain(0);
    }
}

void dcache_counts(struct dcache_counts *c)
{
    uint32_t flags = spin_lock_irqsave(&dcache_lock);

    c->hits = hits;
    c->misses = misses;
    c->entries = entries;
    c->negative = negatives;
    spin_unlock_irqrestore(&dcache_lock, flags);
}
