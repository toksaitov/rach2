/*
 * ext2.c: the ext2 file system: the mount, the superblock, the inode table and
 * the nodes in memory, and the operations table; the contract is in ext2.h
 * and ext2.Readme.md.
 */

#include "ext2_internal.h"
#include "../lib/kirq.h"

/* The kinds of ext2's locks, as `sleep_lock_records` lists them. */
static struct sleep_lock_record node_record = SLEEP_LOCK_RECORD("ext2 node");
static struct sleep_lock_record batch_record = SLEEP_LOCK_RECORD("ext2 batch");
static struct sleep_lock_record group_record = SLEEP_LOCK_RECORD("ext2 group");
static struct sleep_lock_record super_record = SLEEP_LOCK_RECORD("ext2 super");

/* The nodes every mount's hash has read in its searches, for /proc/stat. */
static uint32_t node_visits;

uint32_t ext2_node_visits(void)
{
    return node_visits;
}

/** The chain of the mount's hash where inode `ino` is, the hash set up
 * with its first buckets when the mount has none yet. */
static struct ext2_node **node_chain(struct ext2_mount *em, uint32_t ino)
{
    if (!em->node_hash) {
        em->node_hash = em->node_first;
        em->node_buckets = EXT2_NODE_FIRST;
    }
    return &em->node_hash[ino & (em->node_buckets - 1u)];
}

/** Double the buckets of the mount's hash once its nodes outnumber them,
 * every node moved to the new table under `em->share_lock`, which a lookup
 * without the big lock holds (`table_names`); a heap with no room for the
 * table leaves the hash as it is, its chains longer. */
static void node_hash_grow(struct ext2_mount *em)
{
    uint32_t size = em->node_buckets * 2u;
    struct ext2_node **table, **old;
    struct ext2_node *n;
    uint32_t flags;

    if (em->node_count <= em->node_buckets || size == 0) {
        return;
    }
    table = kzalloc(size * sizeof(*table));
    if (!table) {
        return;
    }
    if (em->node_buckets >= size) {
        kfree(table); /* the heap slept, and another caller grew the hash */
        return;
    }
    flags = spin_lock_irqsave(&em->share_lock);
    old = em->node_hash;
    em->node_hash = table;
    em->node_buckets = size;
    for (n = em->nodes; n; n = n->next) {
        struct ext2_node **chain = node_chain(em, n->ino);

        n->hnext = *chain;
        *chain = n;
    }
    spin_unlock_irqrestore(&em->share_lock, flags);
    if (old != em->node_first) {
        kfree(old);
    }
}

/** The mount's node in memory for inode `ino`, or NULL. The caller holds
 * the big lock, under which the hash changes, or `em->share_lock`, under
 * which it changes too. */
struct ext2_node *ext2_node_find(struct ext2_mount *em, uint32_t ino)
{
    struct ext2_node *n;

    if (!em->node_hash) {
        return 0;
    }
    for (n = *node_chain(em, ino); n; n = n->hnext) {
        node_visits++;
        if (n->ino == ino) {
            return n;
        }
    }
    return 0;
}

/**
 * Add `delta` to the holds of `n` in one atomic step and return the new
 * count. A walk that runs without the big lock takes a hold on a node a
 * name cache entry holds and gives back one that is not the last
 * (`ext2_put_unless_last`), beside callers that hold the lock, so every
 * change of the count is atomic; the count reaches 0, and leaves it, only
 * under the big lock.
 */
uint32_t ext2_node_ref(struct ext2_node *n, int delta)
{
    volatile unsigned int *p = (volatile unsigned int *)&n->refs;
    unsigned int old;

    do {
        old = *p;
    } while (arch_atomic_cmpxchg(p, old, old + (unsigned int)delta) != old);
    return old + (unsigned int)delta;
}

/**
 * Set the size of `n`, whose lock the caller holds, so that `node_size`
 * reads it whole without the lock: on i686 the 64-bit store is two, and a
 * reader between them would see half of each. `size_seq` is odd while the
 * size changes, as Linux's `i_size_write` keeps a sequence around the size
 * of a 32-bit kernel (include/linux/fs.h:976-981). Interrupts stay off
 * while the count is odd: an interrupt waiting for the big lock must not
 * stop a writer whose reader holds that lock.
 */
void ext2_node_size_set(struct ext2_node *n, uint64_t size)
{
    volatile unsigned int *seq = (volatile unsigned int *)&n->size_seq;
    uint32_t flags = kirq_save();

    (void)arch_atomic_xchg(seq, *seq + 1u);
    n->size = size;
    (void)arch_atomic_xchg(seq, *seq + 1u);
    kirq_restore(flags);
}

/** The size of `n`, read whole without its lock, as Linux's `i_size_read`
 * reads it under the sequence (include/linux/fs.h:947-956). */
static uint64_t node_size(const struct ext2_node *n)
{
    const volatile unsigned int *seq = (const volatile unsigned int *)&n->size_seq;
    unsigned int at;
    uint64_t size;

    do {
        while ((at = *seq) & 1u) {
        }
        k_barrier();
        size = *(const volatile uint64_t *)&n->size;
        k_barrier();
    } while (*seq != at);
    return size;
}

/** A new node from the heap, zeroed, N_LOADING and held once by the caller,
 * on no list and with no number, or NULL when the heap has none. The heap
 * may sleep, so a caller that looked a number up looks again afterwards. */
struct ext2_node *ext2_node_make(void)
{
    struct ext2_node *n = kzalloc(sizeof(*n));

    if (n) {
        n->refs = 1;
        n->state = N_LOADING;
        n->dirty_meta = 1;
        sleep_lock_init(&n->lock, &node_record, SLEEP_LOCK_FILE);
    }
    return n;
}

/** Give node `n` of `ext2_node_make` the number `ino`, and put it on the
 * mount's list and in its hash in one step with the caller's look at the hash.
 * Every node a mount has in memory is on the list, so an unmount frees what
 * is left. */
void ext2_node_publish(struct ext2_mount *em, struct ext2_node *n, uint32_t ino)
{
    struct ext2_node **chain;
    uint32_t flags;

    n->next = em->nodes;
    if (em->nodes) {
        em->nodes->prev = n;
    }
    em->nodes = n;
    flags = spin_lock_irqsave(&em->share_lock);
    n->ino = ino;
    n->lost_list = em->shared_lost || em->device_lost;
    chain = node_chain(em, ino);
    n->hnext = *chain;
    *chain = n;
    spin_unlock_irqrestore(&em->share_lock, flags);
    em->node_count++;
    node_hash_grow(em);
}

/** Take node `n` off the mount's list and hash and free it, waking a caller
 * that waits for a dying node to go. A live node's list of shared blocks
 * is lost with it, which `em->shared_lost` records. */
static void node_drop(struct ext2_mount *em, struct ext2_node *n)
{
    struct ext2_node **link;
    uint32_t flags;

    flags = spin_lock_irqsave(&em->share_lock);
    for (link = node_chain(em, n->ino); *link; link = &(*link)->hnext) {
        if (*link == n) {
            *link = n->hnext;
            break;
        }
    }
    if (n->state == N_LIVE && !n->unlinked && (n->nshared || n->shared_over || n->unlisted)) {
        em->shared_lost = 1;
    }
    spin_unlock_irqrestore(&em->share_lock, flags);
    if (n->prev) {
        n->prev->next = n->next;
    } else {
        em->nodes = n->next;
    }
    if (n->next) {
        n->next->prev = n->prev;
    }
    em->node_count--;
    kfree(n);
    em->nodes_gone++;
    wq_wake_all(&em->node_wq);
}

static void node_final(struct ext2_mount *em, struct ext2_node *n);

/**
 * The end of node `n`, whose last hold has gone, under the big lock: an
 * unlinked inode, and a live one with a change no store has put in the
 * cache (`n->unstored`) on a mount that writes, go through `node_final`,
 * whose free frees or stores the inode before the node goes, as Linux's
 * `iput_final` writes a dirty inode before it evicts it (fs/inode.c:1861);
 * any other node is dropped at once. A node whose load failed, or a new
 * inode's that was never made, is not live, and its mark names nothing.
 */
static void node_last(struct ext2_mount *em, struct ext2_node *n)
{
    if (n->unlinked || (n->unstored && n->state == N_LIVE && ext2_writes(em))) {
        node_final(em, n);
    } else {
        node_drop(em, n);
    }
}

/** Drop a hold on `n` that no other holder's unlinked inode depends on, and
 * end the node with the last (`node_last`): a load that failed, a new
 * inode's, or a hold an operation on names took. */
void ext2_node_unhold(struct ext2_mount *em, struct ext2_node *n)
{
    int big = ext2_big_enter();

    if (ext2_node_ref(n, -1) == 0) {
        node_last(em, n);
    }
    ext2_big_exit(big);
}

/** End the N_LOADING of node `n`, held by the caller: N_LIVE for an `rc` of
 * 0, and otherwise N_FAILED with the caller's hold dropped. Wakes every
 * caller waiting for it. */
void ext2_node_settle(struct ext2_mount *em, struct ext2_node *n, int rc)
{
    int big = ext2_big_enter();

    n->state = rc == 0 ? N_LIVE : N_FAILED;
    n->load_error = rc;
    wq_wake_all(&em->node_wq);
    if (rc != 0) {
        ext2_node_unhold(em, n);
    }
    ext2_big_exit(big);
}

/** Free every node of the mount and its hash's table, at its unmount or a
 * mount that failed. */
static void nodes_free(struct ext2_mount *em)
{
    while (em->nodes) {
        node_drop(em, em->nodes);
    }
    if (em->node_hash != em->node_first) {
        kfree(em->node_hash);
    }
    em->node_hash = 0;
    em->node_buckets = 0;
}

/* ---- little-endian fields ---- */

/** The little-endian 16-bit field at `p`. */
uint32_t ext2_le16(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8);
}

/** The little-endian 32-bit field at `p`. */
uint32_t ext2_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/** The driver's record of mount `mnt`. */
struct ext2_mount *ext2_em_of(struct vfs_mount *mnt)
{
    return mnt->priv;
}

/* ---- the locks ---- */

/** Make the mount's locks free, its queue empty and admission open, before
 * anything else uses the record. */
static void em_locks_init(struct ext2_mount *em)
{
    int i;

    sleep_lock_init(&em->batch, &batch_record, SLEEP_LOCK_FILE);
    for (i = 0; i < EXT2_GROUP_LOCKS; i++) {
        sleep_lock_init(&em->group_locks[i], &group_record, SLEEP_LOCK_FILE);
    }
    sleep_lock_init(&em->super, &super_record, SLEEP_LOCK_FILE);
    spin_lock_init(&em->count_lock, "ext2 counts");
    spin_lock_init(&em->admit_lock, "ext2 admission");
    spin_lock_init(&em->share_lock, "ext2 shared lists");
    wait_queue_init(&em->node_wq);
    em->reap = 0;
    em->writers = 0;
    em->closing = 0;
}

/** Whether the caller holds a node's lock or an allocator's, of any mount,
 * under which it may not free an inode or wait for one. */
static int file_locks_held(void)
{
    return current_proc && current_proc->file_locks > 0;
}

/**
 * Take the lock of node `n`, which covers its attributes, its block map and
 * a directory's entries, sleeping while another process holds it; the
 * holder takes it again by depth. The caller holds `n`, and takes no node
 * lock after `em->batch` and none against the order in ext2.Readme.md.
 */
void ext2_node_lock(struct ext2_node *n)
{
    sleep_lock_take(&n->lock);
}

/** Give back one take of `n`'s lock, as `ext2_node_lock` took it. */
void ext2_node_unlock(struct ext2_node *n)
{
    sleep_lock_give(&n->lock);
}

/** Panic unless the caller may change node `n`: it holds `n`'s lock. */
void ext2_assert_node_locked(const struct ext2_node *n)
{
    sleep_lock_assert(&n->lock, "ext2: a change of an inode outside its lock");
}

/**
 * Take `em->batch`, which covers the file-write batch: its images' bytes,
 * its stage, a piece's plan and commit and a drain. Sleeps while another
 * process holds it; the holder takes it again by depth. Taken after every
 * node lock the caller needs and before any group's lock or `em->super`,
 * and held across a disk wait only for one block evicted after its fetch,
 * or held by another's write PLAN_TRIES times over, or by a strict mount
 * for its ordered writes.
 */
void ext2_batch_lock(struct ext2_mount *em)
{
    sleep_lock_take(&em->batch);
}

/** Give back one take of `em->batch`, as `ext2_batch_lock` took it. */
void ext2_batch_unlock(struct ext2_mount *em)
{
    sleep_lock_give(&em->batch);
}

/** Panic unless the caller holds `em->batch`. */
void ext2_assert_batch_locked(struct ext2_mount *em)
{
    sleep_lock_assert(&em->batch, "ext2: a change of the batch outside em->batch");
}

/** The lock of group `g`, which it shares with the groups whose numbers have
 * the same low bits. */
static struct sleep_lock *group_lock_of(struct ext2_mount *em, uint32_t g)
{
    return &em->group_locks[g & (EXT2_GROUP_LOCKS - 1)];
}

/**
 * Take the lock of group `g`, which covers its block and inode bitmaps and
 * its descriptor in `em->gd`, sleeping while another process holds it; the
 * holder takes it again by depth, as a group that hashes to a lock it holds
 * does. A caller holds one group's lock at a time, but for a batch's plan,
 * which holds `em->batch` too and tells the checker the second and later
 * at subclasses (`writeback_reserve`). Taken after `em->batch` and before
 * `em->super`.
 */
void ext2_group_lock(struct ext2_mount *em, uint32_t g)
{
    sleep_lock_take(group_lock_of(em, g));
}

/** Give back one take of group `g`'s lock, as `ext2_group_lock` took it. */
void ext2_group_unlock(struct ext2_mount *em, uint32_t g)
{
    sleep_lock_give(group_lock_of(em, g));
}

/** Whether the caller holds group `g`'s lock. */
int ext2_group_held(struct ext2_mount *em, uint32_t g)
{
    return sleep_lock_held(group_lock_of(em, g));
}

/** Panic unless the caller holds group `g`'s lock. */
void ext2_assert_group_locked(struct ext2_mount *em, uint32_t g)
{
    sleep_lock_assert(group_lock_of(em, g), "ext2: a change of a group outside its lock");
}

/** Take `em->super`, which orders the writes of the superblock's counts,
 * state and features, as Linux's `s_lock` does; the last of the locks that
 * sleep. */
void ext2_super_lock(struct ext2_mount *em)
{
    sleep_lock_take(&em->super);
}

/** Give back one take of `em->super`. */
void ext2_super_unlock(struct ext2_mount *em)
{
    sleep_lock_give(&em->super);
}

/** Change the free block count by `delta` and, when `hint` is not 0, make it
 * the allocation hint, under `em->count_lock`. The caller holds the lock of
 * the group whose descriptor changed with the count. */
void ext2_count_blocks(struct ext2_mount *em, int delta, uint32_t hint)
{
    uint32_t flags = spin_lock_irqsave(&em->count_lock);

    em->free_blocks += (uint64_t)(int64_t)delta;
    if (hint != 0) {
        em->alloc_hint = hint;
    }
    spin_unlock_irqrestore(&em->count_lock, flags);
}

/** Change the free inode count by `delta`, under `em->count_lock`. */
void ext2_count_inodes(struct ext2_mount *em, int delta)
{
    uint32_t flags = spin_lock_irqsave(&em->count_lock);

    em->free_inodes += (uint64_t)(int64_t)delta;
    spin_unlock_irqrestore(&em->count_lock, flags);
}

/** The free block count, read whole under `em->count_lock`: a hint for a
 * caller without the lock of every group, as Linux's free counters are. */
uint64_t ext2_free_blocks(struct ext2_mount *em)
{
    uint32_t flags = spin_lock_irqsave(&em->count_lock);
    uint64_t v = em->free_blocks;

    spin_unlock_irqrestore(&em->count_lock, flags);
    return v;
}

/** The free inode count, read as `ext2_free_blocks` reads its own. */
uint64_t ext2_free_inodes(struct ext2_mount *em)
{
    uint32_t flags = spin_lock_irqsave(&em->count_lock);
    uint64_t v = em->free_inodes;

    spin_unlock_irqrestore(&em->count_lock, flags);
    return v;
}

/** The block after the last one given out, where a search starts. */
uint32_t ext2_alloc_hint(struct ext2_mount *em)
{
    uint32_t flags = spin_lock_irqsave(&em->count_lock);
    uint32_t v = em->alloc_hint;

    spin_unlock_irqrestore(&em->count_lock, flags);
    return v;
}

/** The generation of a new inode, each one once, as Linux takes
 * `s_next_generation` under `s_next_gen_lock` (fs/ext2/ialloc.c). */
uint32_t ext2_generation_next(struct ext2_mount *em)
{
    uint32_t flags = spin_lock_irqsave(&em->count_lock);
    uint32_t v = em->next_generation++;

    spin_unlock_irqrestore(&em->count_lock, flags);
    return v;
}

/** Set the bits `set` and clear the bits `clear` of `s_state` in memory, in
 * one step; returns the new state. The caller that writes it holds
 * `em->super` (`sb_write`). */
uint32_t ext2_state_change(struct ext2_mount *em, uint32_t set, uint32_t clear)
{
    uint32_t flags = spin_lock_irqsave(&em->count_lock);
    uint32_t v;

    em->state = (em->state | set) & ~clear;
    v = em->state;
    spin_unlock_irqrestore(&em->count_lock, flags);
    return v;
}

/** Add the bits `bits` to the feature word `word`, `em->compat` or
 * `em->ro_compat`, in one step; returns the new word. Bits are only ever
 * added while mounted, so a write of an older value is overtaken by the
 * write of the newer one that follows it. */
uint32_t ext2_feature_add(struct ext2_mount *em, uint32_t *word, uint32_t bits)
{
    uint32_t flags = spin_lock_irqsave(&em->count_lock);
    uint32_t v;

    *word |= bits;
    v = *word;
    spin_unlock_irqrestore(&em->count_lock, flags);
    return v;
}

/** The wait condition of `em_enter`: admission is open on the mount `arg`,
 * read as one word. */
static int em_admitting(void *arg)
{
    return !((const volatile struct ext2_mount *)arg)->closing;
}

/** Count a writer in `em->writers` unless a remount keeps writers out,
 * under `em->admit_lock`: 1 when counted, 0 when `closing` is set. */
static int em_admit(struct ext2_mount *em)
{
    uint32_t flags = spin_lock_irqsave(&em->admit_lock);
    int in = !em->closing;

    if (in) {
        em->writers++;
    }
    spin_unlock_irqrestore(&em->admit_lock, flags);
    return in;
}

/**
 * Admit a writer to the mount, sleeping while a remount keeps writers out
 * (`em->closing`), and count it in `em->writers` until its `em_leave`. The
 * caller checks once admitted whether the mount is still writable, since
 * the remount it waited for may have made it read-only.
 *
 * It may hold node locks while it waits, so an admitted writer takes no
 * lock beyond those it held when it was admitted, but a new or dying
 * inode's: the remount waits for every admitted writer, and one that slept
 * on a lock held by a writer that waits here would never end. A change of
 * one file, which not every caller locks, is admitted by `em_enter_node`.
 * The count and the gate change under `em->admit_lock`, since a writer of a
 * file's data comes without the big lock.
 */
static void em_enter(struct ext2_mount *em)
{
    while (!em_admit(em)) {
        wq_wait_event_nointr(&em->node_wq, em_admitting, em);
    }
}

/** Take node `n`'s lock, which the caller may hold already, and admit a
 * writer of it, in that order (`em_enter`). */
static void em_enter_node(struct ext2_mount *em, struct ext2_node *n)
{
    ext2_node_lock(n);
    em_enter(em);
}

/** End a writer `em_enter` admitted, waking a remount that waits for the
 * last. */
static void em_leave(struct ext2_mount *em)
{
    uint32_t flags = spin_lock_irqsave(&em->admit_lock);
    int last = --em->writers == 0 && em->closing;

    spin_unlock_irqrestore(&em->admit_lock, flags);
    if (last) {
        wq_wake_all(&em->node_wq);
    }
}

/** Close the gate of the mount `em` (`em->closing`) when it is open: 1 when
 * this call closed it, 0 when a remount had it closed already. */
static int em_gate_close(struct ext2_mount *em)
{
    uint32_t flags = spin_lock_irqsave(&em->admit_lock);
    int closed = !em->closing;

    em->closing = 1;
    spin_unlock_irqrestore(&em->admit_lock, flags);
    return closed;
}

/** The writers admitted to the mount `em`, read under `em->admit_lock`. */
static uint32_t em_writers(struct ext2_mount *em)
{
    uint32_t flags = spin_lock_irqsave(&em->admit_lock);
    uint32_t writers = em->writers;

    spin_unlock_irqrestore(&em->admit_lock, flags);
    return writers;
}

/** The wait condition of `em_close`: no writer is left on the mount `arg`,
 * or a node waits on its list for a caller free to reap it, each read as
 * one word. */
static int em_settled(void *arg)
{
    const volatile struct ext2_mount *em = arg;

    return em->writers == 0 || em->reap != 0;
}

static void em_reap(struct ext2_mount *em);

/**
 * Close the mount to new writers for a remount and wait for those inside
 * and for every final put, reaping what is left on the mount's list, since
 * the thread that left a node there may do nothing more on this mount. The
 * caller holds no file lock, and a later remount waits for this one's
 * `em_open`.
 */
static void em_close(struct ext2_mount *em)
{
    while (!em_gate_close(em)) {
        wq_wait_event_nointr(&em->node_wq, em_admitting, em);
    }
    em_reap(em);
    while (em_writers(em)) {
        wq_wait_event_nointr(&em->node_wq, em_settled, em);
        em_reap(em);
    }
}

/* A shutdown's wait on the mount `em`, which ends at the tick `deadline`
   whatever else holds. */
struct em_deadline {
    struct ext2_mount *em;
    uint64_t deadline;
};

/** The wait condition of a shutdown's first wait: no remount keeps the
 * mount `arg` closed, or its deadline has come. */
static int em_open_by(void *arg)
{
    const struct em_deadline *d = arg;

    return !d->em->closing || tick_count() >= d->deadline;
}

/** The wait condition of a shutdown's second wait: `em_settled`, or the
 * deadline has come. */
static int em_settled_by(void *arg)
{
    const struct em_deadline *d = arg;

    return em_settled(d->em) || tick_count() >= d->deadline;
}

/** Sleep on the mount's queue until `cond(d)`, with the timer armed for
 * `d->deadline` before each wait, since a signal's wake disarms it. */
static void em_wait_until(struct em_deadline *d, wq_cond_fn cond)
{
    if (current_proc) {
        current_proc->wakeup_tick = d->deadline != 0 ? d->deadline : 1u;
    }
    (void)wq_wait_event_nointr(&d->em->node_wq, cond, d);
    if (current_proc) {
        current_proc->wakeup_tick = 0;
    }
}

/**
 * Close the mount to new writers for good, for a power off, and wait until
 * the tick `deadline` for a remount under way and then for the writers
 * inside and every final put, as `em_close` does. Admission is never opened
 * again: a writer that comes later sleeps at the gate until the machine
 * stops. 0 when no writer is left, or -EBUSY at the deadline, with the
 * mount closed only when no remount was still under way. The caller holds
 * no file lock.
 */
static int em_close_until(struct ext2_mount *em, uint64_t deadline)
{
    struct em_deadline d;

    d.em = em;
    d.deadline = deadline;
    while (!em_gate_close(em)) {
        if (tick_count() >= deadline) {
            return -EBUSY;
        }
        em_wait_until(&d, em_open_by);
    }
    em_reap(em);
    while (em_writers(em) && tick_count() < deadline) {
        em_wait_until(&d, em_settled_by);
        em_reap(em);
    }
    return em_writers(em) ? -EBUSY : 0;
}

/** Let writers in again after `em_close`. */
static void em_open(struct ext2_mount *em)
{
    uint32_t flags = spin_lock_irqsave(&em->admit_lock);
    int opened = em->closing;

    em->closing = 0;
    spin_unlock_irqrestore(&em->admit_lock, flags);
    if (opened) {
        wq_wake_all(&em->node_wq);
    }
}

/**
 * Begin a plan on mount `em`, try `tries` of PLAN_TRIES: until
 * `ext2_plan_end`, the caller's reads of a block the cache lacks or another
 * holds answer -EAGAIN (`volume_get`), for it to fetch with its locks
 * let go. The last try, and a caller with no process, reads as any caller
 * does. The mark is the thread's (`fs_plan` in process.h), so that plans in
 * two groups of one mount run at once.
 */
void ext2_plan_begin(struct ext2_mount *em, int tries)
{
    if (current_proc) {
        current_proc->fs_plan = tries < PLAN_TRIES ? em : 0;
        current_proc->fs_missed = 0;
    }
}

/** End the caller's plan; returns the block its last -EAGAIN asked for. */
uint32_t ext2_plan_end(struct ext2_mount *em)
{
    (void)em;
    if (!current_proc) {
        return 0;
    }
    current_proc->fs_plan = 0;
    return current_proc->fs_missed;
}

/** Whether the caller plans on mount `em`. */
static int planning(const struct ext2_mount *em)
{
    return current_proc && current_proc->fs_plan == em;
}

/** Whether the planner must give block `blk` back to fetch it with
 * its locks let go: the cache lacks it, or another process holds it, a
 * sync or an fsync writing it among them, whose request the get would wait
 * out under the lock. */
static int plan_lacks(struct ext2_mount *em, uint64_t blk)
{
    return !bcache_cached(em->dev, blk) || bcache_busy(em->dev, blk);
}

/**
 * Cache block `blk` of the volume, held, as `bcache_get` gives it. While the
 * caller plans (`ext2_plan_begin`), a block the cache lacks or another holds
 * is not waited for: NULL with `*rc` -EAGAIN and the block in the thread's
 * `fs_missed`, for it to fetch with its locks let go. NULL with -EIO for a
 * failed read. Only the read primitive calls it (`ext2_block_get`,
 * `ext2_read_bytes`).
 */
static struct bbuf *volume_get(struct ext2_mount *em, uint64_t blk, int *rc)
{
    struct bbuf *b;

    if (planning(em) && plan_lacks(em, blk)) {
        current_proc->fs_missed = (uint32_t)blk;
        *rc = -EAGAIN;
        return 0;
    }
    b = bcache_get(em->dev, blk);
    *rc = b ? 0 : -EIO;
    return b;
}

/** Whether a plan may go on to write a block it read none of: -EAGAIN with
 * cache block `blk` in the thread's `fs_missed`, under a plan, when neither the
 * cache nor the batch has it or another process holds it in the cache, and 0
 * otherwise. */
int ext2_plan_has(struct ext2_mount *em, uint32_t blk)
{
    if (planning(em) && ((!ext2_writeback_has(em, blk) && !bcache_cached(em->dev, blk)) ||
                         bcache_busy(em->dev, blk))) {
        current_proc->fs_missed = blk;
        return -EAGAIN;
    }
    return 0;
}

/** Bring cache block `blk` into the cache when neither it nor the batch has
 * it, and wait until no other process holds it, sleeping on the disk; the
 * caller holds no buffer, no group's lock and not `em->batch`. A failed read is
 * left for the caller's own read to meet. */
void ext2_volume_fetch(struct ext2_mount *em, uint64_t blk)
{
    struct bbuf *b;

    bcache_wait_unheld(em->dev, blk);
    if (ext2_writeback_has(em, (uint32_t)blk) || bcache_cached(em->dev, blk)) {
        return;
    }
    b = bcache_get(em->dev, blk);
    if (b) {
        bcache_release(b);
    }
}

/*
 * What the read primitive does for each kind of block it reads (READ_ in
 * ext2_internal.h): the name of the block in a record, whether the block
 * must lie in the data zone before it is read, which is refused, logged and
 * reported otherwise (`ext2_zone_check`), and whether a failed read is
 * logged and reported to the mount's policy (`ext2_mark_error`). The Linux
 * line each row follows is in ext2.Readme.md's kind table. -EAGAIN under a
 * plan is no failure and passes unreported for every kind.
 */
static const struct read_rule {
    const char *what;
    int zone;
    int report;
} read_rules[READ_KINDS] = {
    {"block", 0, 0},            /* READ_QUIET */
    {"block", 0, 0},            /* READ_DATA */
    {"table block", 0, 1},      /* READ_SLOT */
    {"bitmap block", 0, 1},     /* READ_BITMAP */
    {"descriptor block", 0, 1}, /* READ_DESC */
    {"superblock", 0, 1},       /* READ_SUPER */
    {"pointer block", 1, 0},    /* READ_MAP */
    {"pointer block", 1, 1},    /* READ_RECLAIM */
    {"attribute block", 1, 0},  /* READ_ATTR */
    {"attribute block", 1, 1},  /* READ_ATTR_FREE */
};

/**
 * Refuse block `b`, which inode `ino` names as `what`, when it may not hold
 * data (`ext2_data_block_ok`): logged as `ext2: inode N: WHAT B is out of
 * range`, reported to the mount's policy (`ext2_mark_error`) and -EIO, as
 * Linux's `ext2_error` reports a block outside the data zone. 0 for a block
 * that may hold data. The caller holds no buffer.
 */
int ext2_zone_check(struct ext2_mount *em, uint32_t ino, const char *what, uint32_t b)
{
    if (ext2_data_block_ok(em, b)) {
        return 0;
    }
    klog(KLOG_WARN, "ext2", "inode %d: %s %d is out of range", (int)ino, what, (int)b);
    ext2_mark_error(em);
    return -EIO;
}

/** The rule of `kind` before a read of block `b` of inode `ino`: 0, or
 * -EIO reported for a block outside the data zone. */
static int read_before(struct ext2_mount *em, int kind, uint32_t ino, uint32_t b)
{
    return read_rules[kind].zone ? ext2_zone_check(em, ino, read_rules[kind].what, b) : 0;
}

/** The rule of `kind` after a read of block `b` of inode `ino`, or of no
 * inode at 0, that failed with `rc`: an -EIO logged and reported when the
 * kind reports one; -EAGAIN never. Returns `rc`. The caller holds no
 * buffer. */
static int read_failed(struct ext2_mount *em, int kind, uint32_t ino, uint32_t b, int rc)
{
    if (rc != -EIO || !read_rules[kind].report) {
        return rc;
    }
    if (ino != 0) {
        klog(KLOG_WARN, "ext2", "inode %d: %s %d cannot be read", (int)ino, read_rules[kind].what,
             (int)b);
    } else {
        klog(KLOG_WARN, "ext2", "%s %d cannot be read", read_rules[kind].what, (int)b);
    }
    ext2_mark_error(em);
    return rc;
}

/**
 * Cache block `blk` of the volume, held, read as a block of kind `kind` of
 * inode `ino`, or of no inode at 0: the kind's rules before and after the
 * read (`read_rules`), then `volume_get`. NULL with `*rc` set: -EIO for a
 * block refused or a failed read, or -EAGAIN under a plan.
 */
struct bbuf *ext2_block_get(struct ext2_mount *em, int kind, uint32_t ino, uint64_t blk, int *rc)
{
    uint32_t b = (uint32_t)((blk << BCACHE_BLOCK_SHIFT) >> em->block_shift);
    struct bbuf *buf;

    *rc = read_before(em, kind, ino, b);
    if (*rc != 0) {
        return 0;
    }
    buf = volume_get(em, blk, rc);
    if (!buf) {
        (void)read_failed(em, kind, ino, b, *rc);
    }
    return buf;
}

/**
 * The read primitive: copy `len` bytes at byte offset `off` of the volume
 * into `dst`, read as a block of kind `kind` of inode `ino`, or of no inode
 * at 0, under the kind's rules (`read_rules`), one cache block at a time,
 * from the batch's image of a block when it has one, since the batch's
 * writes are not in the cache yet. The offset is 64 bits, so the volume's
 * size is the block layer's business and not this reader's. 0, -EIO, or
 * -EAGAIN for a block the cache lacks under a plan (`volume_get`).
 */
int ext2_read_bytes(struct ext2_mount *em, int kind, uint32_t ino, uint64_t off, void *dst,
                    uint32_t len)
{
    uint8_t *out = dst;
    uint32_t block = (uint32_t)(off >> em->block_shift);
    int rc = read_before(em, kind, ino, block);

    if (rc != 0) {
        return rc;
    }
    while (len > 0) {
        uint64_t blk = off >> BCACHE_BLOCK_SHIFT;
        uint32_t in = (uint32_t)off & (BCACHE_BLOCK_BYTES - 1);
        uint32_t take = BCACHE_BLOCK_BYTES - in;
        struct bbuf *b;

        if (take > len) {
            take = len;
        }
        if (ext2_writeback_copy(em, (uint32_t)blk, in, out, take, 0)) {
            out += take;
            off += take;
            len -= take;
            continue;
        }
        b = volume_get(em, blk, &rc);
        if (!b) {
            return read_failed(em, kind, ino, block, rc);
        }
        if (in + take > b->bytes) {
            bcache_release(b);
            return read_failed(em, kind, ino, block, -EIO);
        }
        k_memcpy(out, b->data + in, take);
        bcache_release(b);
        out += take;
        off += take;
        len -= take;
    }
    return 0;
}

/**
 * The reclaim reader: `ext2_read_bytes` of a block a free walks, of kind
 * READ_RECLAIM, READ_ATTR_FREE or READ_BITMAP, each of which reports a block
 * it refuses and a failed read. 0; 1 for a block refused or unread, already
 * reported, which the caller abandons, and goes on past, as Linux's
 * `ext2_free_branches` and `ext2_xattr_delete_inode` go on past one
 * (fs/ext2/inode.c:1146-1156, xattr.c:832-853), so that the rest of the
 * file and its inode are still freed; or -EAGAIN under a plan. A reclaimer
 * never sees a read error.
 */
int ext2_read_reclaim(struct ext2_mount *em, int kind, uint32_t ino, uint64_t off, void *dst,
                      uint32_t len)
{
    int rc = ext2_read_bytes(em, kind, ino, off, dst, len);

    return rc == -EIO ? 1 : rc;
}

/* ---- the structural blocks ---- */

/** Whether `g`, which is not 0, is a power of `base`, 1 included. */
static int is_power(uint32_t g, uint32_t base)
{
    while (g % base == 0) {
        g /= base;
    }
    return g == 1;
}

/** Whether group `g` keeps a copy of the superblock and the descriptors: with
 * `sparse_super`, groups 0 and 1 and the powers of 3, 5 and 7, each power
 * of one base alone, so 15 has none; without it, every group. */
int ext2_has_super(const struct ext2_mount *em, uint32_t g)
{
    if (!(em->ro_compat & EXT2_RO_SPARSE_SUPER) || g <= 1) {
        return 1;
    }
    return is_power(g, 3) || is_power(g, 5) || is_power(g, 7);
}

/** The blocks of the descriptor table, which follows each superblock copy. */
uint32_t ext2_gd_blocks(const struct ext2_mount *em)
{
    return (em->groups * 32 + em->block_size - 1) / em->block_size;
}

/** The blocks of a group's inode table. */
uint32_t ext2_inode_table_blocks(const struct ext2_mount *em)
{
    return (em->ipg * em->inode_size + em->block_size - 1) / em->block_size;
}

/**
 * Whether block `b` may hold data: inside the volume, past the boot block,
 * and not a superblock, a descriptor table, a bitmap or an inode table.
 * The constructor requires each group's tables to lie in that group, so
 * the group of `b` is the one whose descriptor is consulted.
 */
int ext2_data_block_ok(const struct ext2_mount *em, uint32_t b)
{
    uint32_t g, in;
    const uint8_t *gd;

    if (b < em->first_data || b >= em->blocks) {
        return 0;
    }
    if (b == 0) {
        return 0; /* the boot block, or the superblock at 4 KiB blocks */
    }
    g = (b - em->first_data) / em->bpg;
    in = (b - em->first_data) % em->bpg;
    if (ext2_has_super(em, g) && in <= ext2_gd_blocks(em)) {
        return 0;
    }
    gd = em->gd + g * 32;
    if (b == ext2_le32(gd + 0) || b == ext2_le32(gd + 4)) {
        return 0;
    }
    if (b >= ext2_le32(gd + 8) && b < ext2_le32(gd + 8) + ext2_inode_table_blocks(em)) {
        return 0;
    }
    return 1;
}

/* ---- inodes ---- */

/** Whether inode `n`'s extra bytes reach `end`, an offset in its slot: the
 * extra field that ends there is kept. */
int ext2_extra_has(const struct ext2_node *n, uint32_t end)
{
    return EXT2_GOOD_OLD + n->extra_isize >= end;
}

/** The time whose low word is at `raw + lo` and, where the inode's extra
 * bytes reach past `extra`, whose extra word is there. */
static struct ext2_time time_load(const struct ext2_node *n, const uint8_t *raw, uint32_t lo,
                                  uint32_t extra)
{
    struct ext2_time t;
    uint32_t word;

    t.sec = (int32_t)ext2_le32(raw + lo);
    t.nsec = 0;
    if (ext2_extra_has(n, extra + 4)) {
        word = ext2_le32(raw + extra);
        t.sec += (int64_t)(word & 3u) << 32;
        t.nsec = word >> 2;
    }
    return t;
}

/** Whether `n` is a fast symbolic link, its target in the pointer words:
 * a link whose `i_blocks` counts no block but its extended-attribute
 * block, as Linux's `ext2_inode_is_fast_symlink` decides. */
int ext2_fast_link(const struct ext2_mount *em, const struct ext2_node *n)
{
    uint32_t attr = n->file_acl ? em->block_size / 512 : 0;

    return (n->mode & S_IFMT_) == S_IFLNK_ && n->blocks == attr;
}

/**
 * The inode slot getter: the byte offset of inode `ino`'s slot into `*at`,
 * once its number passes Linux's check for a slot of kind `kind`
 * (fs/ext2/inode.c:1322). A READ_SLOT, the slot of a file met at run time,
 * must be the root's or at least `s_first_ino`, and within the count; a
 * number below that names a reserved inode no entry may name, and is
 * logged as `ext2: inode N: bad inode number`, reported to the mount's
 * policy (`ext2_mark_error`) and -EIO, as Linux's `ext2_get_inode` reports
 * it. READ_QUIET, the check's, which reads the reserved inodes too, asks
 * only that the number be within the count, and reports nothing. Every
 * read of a slot, a load's, a store's and the batch's image of its table
 * block, comes through here. 0, or -EIO.
 */
int ext2_slot_at(struct ext2_mount *em, int kind, uint32_t ino, uint64_t *at)
{
    if (ino == 0 || ino > em->inodes) {
        klog(KLOG_WARN, "ext2", "inode %d is past the count %d", (int)ino, (int)em->inodes);
    } else if (kind != READ_QUIET && ino != EXT2_ROOT_INO && ino < em->first_ino) {
        klog(KLOG_WARN, "ext2", "inode %d: bad inode number", (int)ino);
    } else {
        *at = ext2_inode_offset(em, ino);
        return 0;
    }
    if (kind != READ_QUIET) {
        ext2_mark_error(em);
    }
    return -EIO;
}

/** Copy the first `len` bytes of inode `ino`'s slot into `raw`, read as a
 * slot of kind `kind`, READ_SLOT or READ_QUIET (`ext2_slot_at`). 0, or the
 * errors of the getter and the read primitive. */
static int slot_read(struct ext2_mount *em, int kind, uint32_t ino, uint8_t *raw, uint32_t len)
{
    uint64_t at;
    int rc = ext2_slot_at(em, kind, ino, &at);

    return rc != 0 ? rc : ext2_read_bytes(em, kind, ino, at, raw, len);
}

/**
 * Load inode `ino` into `n`, its slot read through the getter
 * (`ext2_slot_at`), as a READ_SLOT with `report` set and a READ_QUIET
 * otherwise. The first 128 bytes of the
 * slot are read, and of a larger slot the extra fields up to EXT2_LARGE;
 * the owner and group have their high halves at bytes 120 and 122, and a
 * regular file's size has its high word at byte 108. An `i_extra_isize`
 * that is not a multiple of 4 or passes the slot is logged and -EIO, as
 * Linux's ext4 `__ext4_iget` refuses it, and with `report` set is reported
 * to the mount's policy (`ext2_mark_error`). With `report` set, the loader
 * of a file met at run time, three more are refused as Linux's
 * `ext2_get_inode` and `ext2_iget` refuse them (fs/ext2/inode.c:1348, 1436,
 * 1455): a table block that cannot be read, a live inode with no links, a
 * mode and no deletion time, and an attribute block outside the data zone,
 * each logged, reported and -EIO;
 * and a freed inode, one with no links and no mode or a deletion time, is
 * -ESTALE with no report, since a walk without its directory's lock may
 * meet one that was freed after it read the number, and the caller that
 * holds the lock reports it (`ext2_stale`), as Linux's `ext2_lookup` does.
 * 0, the read's -errno, -EIO, or -ESTALE.
 */
static int inode_load(struct ext2_mount *em, uint32_t ino, struct ext2_node *n, int report)
{
    uint8_t raw[EXT2_LARGE];
    int i, rc;

    rc = slot_read(em, report ? READ_SLOT : READ_QUIET, ino, raw,
                   em->inode_size > EXT2_GOOD_OLD ? EXT2_LARGE : EXT2_GOOD_OLD);
    if (rc != 0) {
        return rc;
    }
    n->ino = ino;
    n->extra_isize = 0;
    if (em->inode_size > EXT2_GOOD_OLD) {
        n->extra_isize = ext2_le16(raw + EXT2_EXTRA_ISIZE);
        if ((n->extra_isize & 3u) || EXT2_GOOD_OLD + n->extra_isize > em->inode_size) {
            klog(KLOG_WARN, "ext2", "inode %d: extra size %d does not fit its %d bytes", (int)ino,
                 (int)n->extra_isize, (int)em->inode_size);
            goto damaged;
        }
    }
    n->mode = ext2_le16(raw + 0);
    n->uid = ext2_le16(raw + 2) | (ext2_le16(raw + 120) << 16);
    n->gid = ext2_le16(raw + 24) | (ext2_le16(raw + 122) << 16);
    n->size = ext2_le32(raw + 4);
    n->atime = time_load(n, raw, 8, EXT2_ATIME_EXTRA);
    n->ctime = time_load(n, raw, 12, EXT2_CTIME_EXTRA);
    n->mtime = time_load(n, raw, 16, EXT2_MTIME_EXTRA);
    n->crtime.sec = 0;
    n->crtime.nsec = 0;
    if (ext2_extra_has(n, EXT2_CRTIME + 4)) {
        n->crtime = time_load(n, raw, EXT2_CRTIME, EXT2_CRTIME_EXTRA);
    }
    n->nlink = ext2_le16(raw + 26);
    if (report && n->nlink == 0) {
        if (n->mode == 0 || ext2_le32(raw + 20) != 0) {
            return -ESTALE;
        }
        klog(KLOG_WARN, "ext2", "inode %d has no links, mode 0%o and no deletion time", (int)ino,
             (unsigned)n->mode);
        goto damaged;
    }
    n->blocks = ext2_le32(raw + 28);
    n->flags = ext2_le32(raw + 32);
    n->generation = ext2_le32(raw + 100);
    n->file_acl = ext2_le32(raw + 104);
    if (report && n->file_acl != 0 && ext2_zone_check(em, ino, "attribute block", n->file_acl)) {
        return -EIO;
    }
    for (i = 0; i < (int)EXT2_NBLOCKS; i++) {
        n->block[i] = ext2_le32(raw + 40 + i * 4);
    }
    n->fast_target = ext2_fast_link(em, n);
    if ((n->mode & S_IFMT_) == S_IFREG_) {
        n->size |= (uint64_t)ext2_le32(raw + 108) << 32;
    }
    return 0;
damaged:
    if (report) {
        ext2_mark_error(em);
    }
    return -EIO;
}

/** `inode_load` of a file the mount meets at run time, its damage reported
 * to the mount's policy. The caller holds no buffer. 0, -ESTALE for a freed
 * inode, or -errno. */
int ext2_inode_load(struct ext2_mount *em, uint32_t ino, struct ext2_node *n)
{
    return inode_load(em, ino, n, 1);
}

/**
 * Report that a name read under its directory's lock names inode `ino`,
 * which is freed, as Linux's `ext2_lookup` reports `deleted inode
 * referenced` (fs/ext2/namei.c:73): logged, reported to the mount's policy
 * (`ext2_mark_error`) and -EIO. The caller holds no buffer.
 */
int ext2_stale(struct ext2_mount *em, uint32_t ino)
{
    klog(KLOG_WARN, "ext2", "inode %d: a name references it, freed", (int)ino);
    ext2_mark_error(em);
    return -EIO;
}

/** `inode_load` for the check of a writable mount or remount
 * (`ext2_check_volume`), which judges the volume itself, its damage not
 * reported. 0, or -errno. */
int ext2_inode_check_load(struct ext2_mount *em, uint32_t ino, struct ext2_node *n)
{
    return inode_load(em, ino, n, 0);
}

/* ---- the nodes in memory ---- */

/* What a caller that found a dying node waits for: a node of the mount has
 * gone since `gone` was read from `em->nodes_gone`. */
struct node_wait {
    const struct ext2_mount *em;
    uint32_t gone;
};

/** The wait condition of `node_outlive`, on a `struct node_wait`. */
static int node_went(void *arg)
{
    const struct node_wait *w = arg;

    return w->em->nodes_gone != w->gone;
}

/**
 * Wait for the dying node `n` to go, after which the caller looks in the
 * hash again; `n` is not to be read after. A node on the list that no thread
 * is freeing is reaped here, since the thread that left it may be the
 * caller. One that a caller under a file lock cannot reap is -ESTALE, as a
 * freed inode's load is (`inode_load`): a name read under its directory's
 * lock that names an inode with no link count is damage, which that caller
 * reports (`ext2_stale`), and one read without the lock may have gone
 * since. 0, or -ESTALE.
 */
static int node_outlive(struct ext2_mount *em, struct ext2_node *n)
{
    struct node_wait w;

    if (n->queued) {
        if (file_locks_held()) {
            return -ESTALE;
        }
        em_reap(em);
        return 0;
    }
    w.em = em;
    w.gone = em->nodes_gone;
    wq_wait_event_nointr(&em->node_wq, node_went, &w);
    return 0;
}

/**
 * Take the dying node `n`, which its last put left on `em->reap` to be
 * stored (`node_last`) and which no thread is ending yet, back for a caller
 * that asks for its inode again: off the list, N_LIVE with its mark, which
 * the next last put settles, and no longer counted as a writer. The inode is
 * linked, so it is the same file still, and a caller under a file lock, which
 * could not reap it, has it at once. Under the big lock, which keeps the
 * list.
 */
static void node_revive(struct ext2_mount *em, struct ext2_node *n)
{
    struct ext2_node **link;

    for (link = &em->reap; *link; link = &(*link)->reap_next) {
        if (*link == n) {
            *link = n->reap_next;
            break;
        }
    }
    n->queued = 0;
    n->state = N_LIVE;
    em_leave(em);
}

/** The wait condition of `node_join`: the node `arg` is no longer loading. */
static int node_settled(void *arg)
{
    return ((const struct ext2_node *)arg)->state != N_LOADING;
}

/** Take a hold on `n`, which another caller may be loading, and wait for the
 * load. `n`, or NULL with `*rc` the failed load's -errno and no hold. */
static struct ext2_node *node_join(struct ext2_mount *em, struct ext2_node *n, int *rc)
{
    if (n->state == N_FAILED) {
        *rc = n->load_error;
        return 0;
    }
    ext2_node_ref(n, 1);
    while (n->state == N_LOADING) {
        wq_wait_event_nointr(&em->node_wq, node_settled, n);
    }
    if (n->state == N_FAILED) {
        *rc = n->load_error;
        ext2_node_unhold(em, n);
        return 0;
    }
    return n;
}

/** `ext2_node_for` under the big lock, which keeps the node table. */
static struct ext2_node *node_for(struct ext2_mount *em, uint32_t ino, int *rc)
{
    struct ext2_node *n, *fresh = 0;

    *rc = 0;
    for (;;) {
        n = ext2_node_find(em, ino);
        if (n && n->state == N_DYING && n->queued && !n->unlinked) {
            node_revive(em, n);
        }
        if (n && n->state == N_DYING) {
            *rc = node_outlive(em, n);
            if (*rc != 0) {
                n = 0;
                break;
            }
            continue;
        }
        if (n || fresh) {
            break;
        }
        fresh = ext2_node_make();
        if (!fresh) {
            klog(KLOG_WARN, "ext2", "node: no memory");
            *rc = -ENOMEM;
            break;
        }
    }
    if (n || *rc != 0) {
        kfree(fresh);
        return n ? node_join(em, n, rc) : 0;
    }
    ext2_node_publish(em, fresh, ino);
    *rc = ext2_inode_load(em, ino, fresh);
    ext2_node_settle(em, fresh, *rc);
    return *rc == 0 ? fresh : 0;
}

/**
 * The node for inode `ino`, held once more if the mount has it in memory
 * and loaded into a new record otherwise; NULL with `*rc` set, -ENOMEM when
 * the heap has no room, -ESTALE for a freed inode (`inode_load`,
 * `node_outlive`), which the caller reports when it read the number under
 * its directory's lock (`ext2_stale`), or the load's -errno. The record is
 * published N_LOADING before the load, which sleeps in the cache, so that a
 * second lookup meanwhile waits for it and does not make another; a dying
 * node is waited for, and the inode then looked up anew, but for a linked
 * one left on `em->reap` to be stored, which is taken back (`node_revive`).
 * The caller may hold node locks: the load holds none, and a node that dies
 * holds none it needs.
 * A caller without the big lock, an operation on names, has it taken for the
 * search and the hold, since the node table is the big lock's.
 */
struct ext2_node *ext2_node_for(struct ext2_mount *em, uint32_t ino, int *rc)
{
    int big = ext2_big_enter();
    struct ext2_node *n = node_for(em, ino, rc);

    ext2_big_exit(big);
    return n;
}

/** The `hold` of `vfs_ops`: one more hold on `node`, which takes no lock
 * and may run without the big lock on a node the caller or the name cache
 * holds. */
static void ext2_hold(struct vfs_mount *mnt, void *node)
{
    (void)mnt;
    ext2_node_ref((struct ext2_node *)node, 1);
}
static int sb_note(struct ext2_mount *em);

/**
 * End the dying node `n` on a mount that writes (`ext2_writes`), a stopped
 * one among them: an unlinked inode is freed, as Linux's `ext2_evict_inode`
 * frees under SB_RDONLY, and a linked one with its mark (`n->unstored`)
 * stored, as Linux's `iput_final` writes a dirty inode (fs/inode.c:1861);
 * then the node is dropped. A read-only or failed mount writes nothing, so
 * an inode left there is an orphan for e2fsck, and so is one whose free
 * fails, logged as `ext2: inode N: not freed at its last put`; a store that
 * fails is logged as `ext2: inode N: not stored at its last put`. Neither is
 * reported again, as Linux's `ext2_evict_inode` ignores a failure there
 * (fs/ext2/inode.c:86-110): the reader reported the damage it met, as the
 * store does a table block it cannot write (`ext2_inode_write`), and a
 * failed write is recorded as one, so the mount's error bit or its failure
 * keeps the valid bit off the volume. The caller holds no file lock. Ends
 * the count `node_final` took.
 */
static void node_free(struct ext2_mount *em, struct ext2_node *n)
{
    if (ext2_writes(em)) {
        int rc = 0, freed = n->unlinked;

        ext2_node_lock(n);
        if (freed) {
            rc = ext2_release_inode(em, n);
        } else if (n->unstored) {
            rc = ext2_inode_store(em, n);
        }
        ext2_node_unlock(n);
        if (rc != 0) {
            klog(KLOG_WARN, "ext2", "inode %d: not %s at its last put", (int)n->ino,
                 freed ? "freed" : "stored");
        }
    }
    node_drop(em, n);
    em_leave(em);
}

/**
 * The end of an unlinked inode, or of a marked one (`node_last`), whose last
 * hold has gone: N_DYING, counted as a writer without waiting, since a
 * remount that is closing the mount must see it and the caller may be a
 * writer it waits for. A caller with no file lock frees or stores it now;
 * any other leaves it on `em->reap`, since the free takes node locks and a
 * group's lock out of order and both wait on the disk.
 */
static void node_final(struct ext2_mount *em, struct ext2_node *n)
{
    uint32_t flags = spin_lock_irqsave(&em->admit_lock);

    n->state = N_DYING;
    em->writers++;
    spin_unlock_irqrestore(&em->admit_lock, flags);
    if (file_locks_held()) {
        n->queued = 1;
        n->reap_next = em->reap;
        em->reap = n;
        if (em->closing) {
            wq_wake_all(&em->node_wq);
        }
        return;
    }
    node_free(em, n);
}

/**
 * Free the nodes `em->reap` holds, each after a drain of the batch, so that
 * no block is freed while an older write is pending, unless the caller
 * holds a file lock, which leaves them for the next caller. Called at the
 * end of an operation, at the last unlock of a node, at a put and in
 * `ext2_sync`, so an unmount leaves none. A caller without the big lock
 * takes it when the list has a node.
 */
static void em_reap(struct ext2_mount *em)
{
    struct ext2_node *volatile *reap = &em->reap;
    int big = 0;

    if (*reap && !file_locks_held() && !bkl_held()) {
        bkl_enter();
        big = 1;
    }
    while (em->reap && !file_locks_held()) {
        struct ext2_node *n = em->reap;

        em->reap = n->reap_next;
        n->queued = 0;
        if (em->wb && em->wb->count) {
            (void)ext2_writeback_drain(em);
        }
        node_free(em, n);
    }
    if (big) {
        bkl_exit();
    }
}

/**
 * The `put` of `vfs_ops`: drop a hold on `node`, and free the node with the
 * last; it takes no lock but those of an unlinked inode's free. A caller
 * without the big lock drops a hold that is not the last without it
 * (`ext2_put_unless_last`) and takes it for any other.
 *
 * An unlinked inode keeps its blocks until its last holder goes, which is
 * what makes an open unlinked file work, and a put of one by a caller with
 * no file lock drains the batch first, so no block is freed while an older
 * write is pending. Its last put marks it N_DYING and frees it on a writable
 * mount (`node_final`); a crash before this leaves an orphan for e2fsck. The
 * last put of a linked inode with a change not yet stored stores it the
 * same way before the node goes (`node_last`).
 */
static int ext2_put_unless_last(struct vfs_mount *mnt, void *node);

void ext2_put(struct vfs_mount *mnt, void *node)
{
    struct ext2_node *n = node;
    struct ext2_mount *em = ext2_em_of(mnt);

    int big = 0;

    if (!bkl_held()) {
        if (ext2_put_unless_last(mnt, node)) {
            return;
        }
        bkl_enter();
        big = 1;
    }
    if (n->unlinked && !file_locks_held() && em->wb && em->wb->count) {
        (void)ext2_writeback_drain(em);
    }
    if (n->refs == 0 || ext2_node_ref(n, -1) == 0) {
        node_last(em, n);
    }
    em_reap(em);
    if (big) {
        bkl_exit();
    }
}

/**
 * The `put_unless_last` of `vfs_ops`: drop a hold on `node` without the big
 * lock when another hold stays and the inode is linked, as Linux's dput
 * drops a count above one without the dentry's lock (`fast_dput`,
 * fs/dcache.c:758-784). 1 when it did; 0 with nothing changed when the
 * caller must `put` under the big lock, for the last hold or an unlinked
 * inode's, whose put drains the batch and may free blocks.
 */
static int ext2_put_unless_last(struct vfs_mount *mnt, void *node)
{
    struct ext2_node *n = node;
    volatile unsigned int *p = (volatile unsigned int *)&n->refs;
    unsigned int old;

    (void)mnt;
    do {
        old = *p;
        if (old < 2 || n->unlinked) {
            return 0;
        }
    } while (arch_atomic_cmpxchg(p, old, old - 1u) != old);
    return 1;
}

/* ---- block mapping ---- */

/**
 * Block `index` of `n`'s data, into `*out`: 0 for a hole, or the pointer
 * through the direct, indirect, double and triple levels, each pointer block
 * checked before it is read. 0, -EFBIG past the triple indirect's reach, or
 * -EIO, with the error bit set and a record for a block out of range.
 */
int ext2_block_at(struct ext2_mount *em, const struct ext2_node *n, uint32_t index, uint32_t *out)
{
    uint32_t per = em->block_size / 4;
    uint32_t b;
    int level;

    if (index < EXT2_NDIR) {
        b = n->block[index];
    } else {
        uint32_t span = per;
        uint32_t rest;

        index -= EXT2_NDIR;
        for (level = 1; level <= 3; level++) {
            if (index < span) {
                break;
            }
            index -= span;
            span *= per;
        }
        if (level > 3) {
            return -EFBIG;
        }
        b = n->block[EXT2_NDIR + level - 1];
        rest = index;
        while (level > 0 && b != 0) {
            uint32_t div = 1;
            uint8_t ptr[4];
            int i, rc;

            for (i = 1; i < level; i++) {
                div *= per;
            }
            rc = ext2_read_bytes(em, READ_MAP, n->ino,
                                 (uint64_t)b * em->block_size + (uint64_t)(rest / div) * 4, ptr, 4);
            if (rc != 0) {
                return rc;
            }
            b = ext2_le32(ptr);
            rest %= div;
            level--;
        }
    }
    if (b != 0 && ext2_zone_check(em, n->ino, "block", b) != 0) {
        return -EIO;
    }
    *out = b;
    return 0;
}

/** Copy `len` bytes of `n`'s data at `off` into `dst`, a hole read as zeros;
 * 0, the errors of `ext2_block_at`, or a failed read's -errno, which sets
 * `*unread` when that is not NULL: the caller that reports a failed read of
 * a directory's block tells it from damage `ext2_block_at` has reported. */
int ext2_read_data(struct ext2_mount *em, const struct ext2_node *n, uint64_t off, void *dst,
                   uint32_t len, int *unread)
{
    uint8_t *out = dst;

    while (len > 0) {
        uint32_t index = (uint32_t)(off >> em->block_shift);
        uint32_t in = (uint32_t)off & (em->block_size - 1);
        uint32_t take = em->block_size - in;
        uint32_t b;
        int rc;

        if (take > len) {
            take = len;
        }
        rc = ext2_block_at(em, n, index, &b);
        if (rc != 0) {
            return rc;
        }
        if (b == 0) {
            k_memset(out, 0, take);
        } else {
            rc = ext2_read_bytes(em, READ_DATA, n->ino, (uint64_t)b * em->block_size + in, out,
                                 take);
            if (rc != 0) {
                if (unread) {
                    *unread = 1;
                }
                return rc;
            }
        }
        out += take;
        off += take;
        len -= take;
    }
    return 0;
}

/* ---- attributes ---- */

/**
 * The `getattr` of `vfs_ops`: the inode's fields as the node has them, with
 * `i_blocks`, the generation and the flags lsattr sees, copied with no lock:
 * under the big lock in one step, since a change of a node is made between
 * sleeps, and without it a word at a time, the size whole (`node_size`), as
 * Linux's `generic_fillattr` reads the inode without its lock. A device's
 * numbers are in the old encoding in the first
 * pointer word, major and minor a byte each, or in the new one in the second
 * when the first is zero, and `rdev` packs them as the kernel does. Always
 * 0.
 */
static int ext2_getattr(struct vfs_mount *mnt, void *node, struct vfs_stat *st)
{
    struct ext2_node *n = node;

    (void)mnt;
    k_memset(st, 0, sizeof(*st));
    st->mode = n->mode;
    st->uid = n->uid;
    st->gid = n->gid;
    st->nlink = n->nlink;
    st->size = node_size(n);
    st->ino = n->ino;
    if ((n->mode & S_IFMT_) == S_IFCHR_ || (n->mode & S_IFMT_) == S_IFBLK_) {
        uint32_t major, minor;

        if (n->block[0] != 0) {
            major = (n->block[0] >> 8) & 0xFFu;
            minor = n->block[0] & 0xFFu;
        } else {
            uint32_t v = n->block[1];

            major = (v >> 8) & 0xFFFu;
            minor = (v & 0xFFu) | ((v >> 12) & 0xFFF00u);
        }
        st->rdev = (major << 8) | (minor & 0xFFu) | ((minor & 0xFFF00u) << 12);
    }
    st->atime_sec = n->atime.sec;
    st->atime_nsec = n->atime.nsec;
    st->mtime_sec = n->mtime.sec;
    st->mtime_nsec = n->mtime.nsec;
    st->ctime_sec = n->ctime.sec;
    st->ctime_nsec = n->ctime.nsec;
    st->blocks = n->blocks;
    st->blocks_known = 1;
    st->flags = n->flags & EXT2_FL_USER_VISIBLE;
    st->generation = n->generation;
    st->flags_known = 1;
    return 0;
}

/** The blocks the volume's own structures take: those before the first
 * group, each superblock copy and descriptor table, and each group's two
 * bitmaps and inode table, as Linux's ext2 counts them. */
static uint32_t overhead_blocks(const struct ext2_mount *em)
{
    uint32_t n = em->first_data + em->groups * (2 + ext2_inode_table_blocks(em));
    uint32_t g;

    for (g = 0; g < em->groups; g++) {
        if (ext2_has_super(em, g)) {
            n += 1 + ext2_gd_blocks(em);
        }
    }
    return n;
}

/**
 * The `statfs` of `vfs_ops`: the volume's size, free and reserved blocks and
 * inodes. The size leaves out the blocks of the volume's own structures
 * (`overhead_blocks`), as Linux's ext2 does under its default `bsddf`, so
 * `df` counts as used only what files and directories take, as `du` does.
 * Always 0.
 */
static int ext2_statfs(struct vfs_mount *mnt, struct vfs_statfs *st)
{
    struct ext2_mount *em = ext2_em_of(mnt);
    uint32_t overhead = overhead_blocks(em);

    st->magic = EXT2_SUPER_MAGIC;
    st->bsize = em->block_size;
    st->blocks = em->blocks > overhead ? em->blocks - overhead : 0;
    st->bfree = ext2_free_blocks(em);
    st->reserved = em->r_blocks;
    st->files = em->inodes;
    st->ffree = ext2_free_inodes(em);
    st->namelen = EXT2_NAME_MAX;
    return 0;
}

/**
 * Empty every node's list of shared blocks for a write of the whole device
 * that starts now, which puts all of them on the device: a list left behind
 * would make the next `fsync` beside its node write that file for nothing
 * (`table_unsynced`). The node keeps `unlisted` and a list lost with its
 * node `device_lost` until `ext2_device_write_end`, so that an `fsync`
 * meanwhile does not take their blocks for written. A pending batch keeps the
 * lists, since what they name may be in its images and not yet in the cache.
 * Takes the big lock, which keeps the list of nodes, and `em->share_lock`.
 */
void ext2_device_write_begin(struct ext2_mount *em)
{
    struct ext2_node *n;
    int big = ext2_big_enter();
    uint32_t flags = spin_lock_irqsave(&em->share_lock);

    em->device_writes++;
    if (!ext2_batch_pending(em)) {
        for (n = em->nodes; n; n = n->next) {
            n->unlisted |= n->nshared || n->shared_over;
            n->nshared = n->shared_over = 0;
        }
        em->device_lost |= em->shared_lost;
        em->shared_lost = 0;
    }
    spin_unlock_irqrestore(&em->share_lock, flags);
    ext2_big_exit(big);
}

/**
 * End a write of the whole device. The last one ending has put what the
 * lists named on the device, or failed, which leaves the mount read-only,
 * so that every later `fsync` writes the device and reports the failure.
 * Takes the locks `ext2_device_write_begin` does.
 */
void ext2_device_write_end(struct ext2_mount *em)
{
    struct ext2_node *n;
    int big = ext2_big_enter();
    uint32_t flags = spin_lock_irqsave(&em->share_lock);

    if (--em->device_writes == 0) {
        for (n = em->nodes; n; n = n->next) {
            n->unlisted = 0;
            n->lost_list &= em->shared_lost;
        }
        em->device_lost = 0;
    }
    spin_unlock_irqrestore(&em->share_lock, flags);
    ext2_big_exit(big);
}

/**
 * Write the device's dirty buffers and flush the disk's cache, with no lock
 * of the mount held, as `vfs_sync_all` does, so that a disk that takes
 * seconds holds up no other call on the mount; a change made meanwhile is
 * the next sync's. A failure is recorded as the batch's error. 0, or
 * -errno.
 */
int ext2_em_write_out(struct vfs_mount *mnt)
{
    struct ext2_mount *em = ext2_em_of(mnt);
    int rc;

    ext2_device_write_begin(em);
    rc = bcache_sync(em->dev);
    if (rc == 0) {
        rc = block_flush(em->dev);
    }
    ext2_device_write_end(em);
    if (rc != 0) {
        rc = ext2_writeback_error(em, rc);
    }
    return rc;
}

/**
 * The `sync` of `vfs_ops`: the batch drained, the final puts left on
 * `em->reap` done, and, on a writable mount, the superblock's free counts
 * and state left dirty under `em->super`, then the device's dirty buffers
 * written and the disk's cache flushed with no lock held (`ext2_em_write_out`).
 * The caller holds no file lock. Returns this sync's result, with a failure
 * recorded as the batch's error and for the descriptions' syncs to report
 * (`ext2_writeback_error`).
 *
 * The superblock is noted only on a mount that keeps its state
 * (`ext2_keeps_state`). A sync is outside admission, so a remount read-only
 * can finish while it sleeps on block 0; the mode is asked again under
 * `em->super`, under which that remount ends it, as Linux orders
 * `sync_filesystem` and a remount by `s_umount`. A mount made read-only or
 * stopped meanwhile keeps its superblock as it was written, and no write is
 * refused.
 */
static int ext2_sync(struct vfs_mount *mnt)
{
    struct ext2_mount *em = ext2_em_of(mnt);
    int rc;

    rc = ext2_writeback_drain(em);
    em_reap(em);
    if (rc == 0 && ext2_keeps_state(em)) {
        ext2_volume_fetch(em, 0);
        ext2_super_lock(em);
        if (ext2_keeps_state(em)) {
            rc = sb_note(em);
        }
        ext2_super_unlock(em);
    }
    return rc != 0 ? ext2_writeback_error(em, rc) : ext2_em_write_out(mnt);
}

/**
 * The `writeback` of `vfs_ops`: the file-write batch drained into the cache
 * once its first byte is as old as `before`, as
 * Linux's flusher writes an inode's pages back once the inode has been
 * dirty for `dirty_expire_centisecs` (`wb_check_old_data_flush`,
 * fs/fs-writeback.c). The drain writes only what the cache must free for
 * its copies (`bcache_reserve`), and the cache's write-back of the rest is
 * the flusher's. A mount that a fault made read-only (`ext2_mark_error`)
 * drains the writes it accepted before, as Linux's flusher writes the dirty
 * pages of a file system made read-only; one that does not write
 * (`ext2_writes`), a failed one among them, waits for a sync, which tries
 * the device again. The batch's age and the mount's mode are read without
 * their locks, a look that the next turn corrects. 1 after a drain, 0 when
 * there was nothing old enough, or the drain's -errno, recorded as the
 * batch's error.
 */
static int ext2_writeback_old(struct vfs_mount *mnt, uint64_t before)
{
    struct ext2_mount *em = ext2_em_of(mnt);
    int rc;

    if (!ext2_writes(em) || !ext2_batch_pending(em) || em->wb->born > before) {
        return 0;
    }
    rc = ext2_writeback_drain(em);
    return rc == 0 ? 1 : rc;
}

/**
 * The end of a writable mount's writes, for an unmount and, with `readonly`
 * set, a remount read-only: `ext2_sync`, which leaves no final put undone,
 * and then, on a mount that keeps its superblock's state
 * (`ext2_keeps_state`), the valid bit set again when this mount cleared it,
 * the error bit is clear and no check was left owed (`em->check_owed`), and
 * the state written to the medium under `em->super`. With `readonly`, the
 * mode goes from EXT2_MODE_RW to EXT2_MODE_RO under the same hold of
 * `em->super`, so that a sync sees the mount writable before the write of
 * the state or read-only after it (`ext2_sync`); a mount a fault stopped
 * meanwhile stays stopped, so that only a remount writable ends the stop.
 * 0, or -errno; a failed write of the state clears the valid bit again and
 * is recorded as the batch's error.
 *
 * A fault a reader meets while the state is on its way to the disk sets
 * the error bit under `em->count_lock` before it waits for `em->super`
 * (`ext2_mark_error`), and must reach the disk, as Linux's `ext2_error`
 * writes the bit while a remount syncs, before SB_RDONLY is set. So the
 * state is written again, without the valid bit, while it changed during
 * the write before, and the mode becomes EXT2_MODE_RO in the same step
 * under `em->count_lock` as the look that found it unchanged: a bit set
 * before that step is on the disk, and one set after it was met on a mount
 * that no longer writes.
 */
static int finish_writes(struct vfs_mount *mnt, int readonly)
{
    struct ext2_mount *em = ext2_em_of(mnt);
    int rc = ext2_sync(mnt);
    uint32_t state, flags;
    int again;

    if (rc != 0 || em->mode != EXT2_MODE_RW) {
        return rc;
    }
    ext2_super_lock(em);
    if (ext2_keeps_state(em)) {
        if (em->valid_cleared && !(em->state & EXT2_ERROR_FS) && !em->check_owed) {
            (void)ext2_state_change(em, EXT2_VALID_FS, 0);
        }
        do {
            flags = spin_lock_irqsave(&em->count_lock);
            state = em->state;
            spin_unlock_irqrestore(&em->count_lock, flags);
            rc = ext2_sb_store(em);
            if (rc != 0) {
                break;
            }
            flags = spin_lock_irqsave(&em->count_lock);
            again = em->state != state;
            if (again) {
                em->state &= ~EXT2_VALID_FS;
            } else if (readonly) {
                em->mode = EXT2_MODE_RO;
            }
            spin_unlock_irqrestore(&em->count_lock, flags);
        } while (again);
        if (rc != 0) {
            (void)ext2_state_change(em, 0, EXT2_VALID_FS);
            rc = ext2_writeback_error(em, rc);
        } else {
            em->valid_cleared = 0;
        }
    }
    if (rc == 0 && readonly && em->mode == EXT2_MODE_RW) {
        flags = spin_lock_irqsave(&em->count_lock);
        em->mode = EXT2_MODE_RO;
        spin_unlock_irqrestore(&em->count_lock, flags);
    }
    ext2_super_unlock(em);
    return rc;
}

/** The `prepare_unmount` of `vfs_ops`: `finish_writes`, the mount left
 * writable for `ext2_abort_unmount`. 0, or -errno. */
static int ext2_prepare_unmount(struct vfs_mount *mnt)
{
    return finish_writes(mnt, 0);
}

/**
 * The `abort_unmount` of `vfs_ops`: on a mount that keeps its superblock's
 * state (`ext2_keeps_state`), the only one whose valid bit
 * `ext2_prepare_unmount` set, that bit is cleared again in the superblock,
 * through the write funnel (`ext2_write_bytes`), and the device synced and
 * flushed, since the unmount is not happening. A failure is recorded as the
 * batch's error. The state is written under `em->super`, and the sync and
 * the flush run with no lock held.
 */
static void ext2_abort_unmount(struct vfs_mount *mnt)
{
    struct ext2_mount *em = ext2_em_of(mnt);
    uint8_t raw[2];
    int rc;

    if (!ext2_keeps_state(em)) {
        return;
    }
    ext2_volume_fetch(em, 0);
    ext2_super_lock(em);
    LE16_PUT(raw, ext2_state_change(em, 0, EXT2_VALID_FS));
    em->valid_cleared = 1;
    rc = ext2_write_bytes(em, 1024 + 58, raw, 2, 0);
    ext2_super_unlock(em);
    if (rc == 0) {
        rc = bcache_sync(em->dev);
    }
    if (rc == 0) {
        rc = block_flush(em->dev);
    }
    if (rc != 0) {
        (void)ext2_writeback_error(em, rc);
    }
}

/** Whether the text from `key` to `end` is exactly `word`. */
static int option_is(const char *key, const char *end, const char *word)
{
    while (key < end && *word && *key == *word) {
        key++;
        word++;
    }
    return key == end && *word == '\0';
}

/**
 * Read `errors=` of `options`, mount(2)'s data or NULL, into `*errors`,
 * left as it is when the option is absent, as Linux's `parse_options`
 * overrides the disk's s_errors (fs/ext2/super.c:523-532); the last one
 * given counts, and other options are ignored. 0, or -EINVAL with the
 * record `ext2: refused: errors=` for a value other than `continue`,
 * `remount-ro` and `panic`.
 */
static int errors_option(const char *options, int *errors)
{
    const char *p = options ? options : "";

    while (*p) {
        const char *key = p, *value, *end;

        while (*p && *p != ',') {
            p++;
        }
        end = p;
        if (*p == ',') {
            p++;
        }
        if (end - key < 7 || !option_is(key, key + 7, "errors=")) {
            continue;
        }
        value = key + 7;
        if (option_is(value, end, "continue")) {
            *errors = EXT2_ERRORS_CONTINUE;
        } else if (option_is(value, end, "remount-ro")) {
            *errors = EXT2_ERRORS_RO;
        } else if (option_is(value, end, "panic")) {
            *errors = EXT2_ERRORS_PANIC;
        } else {
            klog(KLOG_WARN, "ext2", "refused: errors=");
            return -EINVAL;
        }
    }
    return 0;
}

/**
 * Make a writable mount read-only, its admission closed and no writer left:
 * the volume synced and its valid bit set, as an unmount leaves it, and the
 * mount read-only from the write of its state on (`finish_writes`). A node
 * removed while still open prevents it, since its inode is freed at its
 * last put and a read-only mount frees nothing: -EBUSY, as on Linux for such
 * a file. The scan comes after the writers went, so that it sees what they
 * unlinked. 0, or -errno.
 */
static int remount_readonly(struct vfs_mount *mnt, struct ext2_mount *em)
{
    struct ext2_node *n;

    for (n = em->nodes; n; n = n->next) {
        if (n->unlinked) {
            return -EBUSY;
        }
    }
    return finish_writes(mnt, 1);
}

/**
 * Make a read-only mount writable, or one a fault stopped
 * (EXT2_MODE_STOPPED), whose operations under way `em_close` waited for:
 * the only way a stop ends. The writes the batch accepted before the fault
 * (`ext2_mark_error`) are drained,
 * so that the check reads the volume they leave, a volume whose valid bit
 * is clear or whose error bit is set checked as a writable mount checks it,
 * the batch set up when the mount has none, the valid bit cleared on the
 * medium, and the mode set EXT2_MODE_RW under `em->super`.
 * 0, -EROFS for a volume whose features keep it read-only
 * (`em->readonly_why`) or a failed mount (`ext2_failed`), -ENOMEM for a
 * batch the heap cannot give, the drain's -errno, or -errno.
 *
 * The check holds no lock: admission is closed (`em_close`) and the mount
 * admits no change and allocates nothing, so no writer, allocation or free
 * of an inode changes what it reads. The batch is set up under
 * `em->batch`, and the state written under `em->super`.
 */
static int remount_writable(struct ext2_mount *em)
{
    int rc = 0;

    if (em->readonly_why || ext2_failed(em)) {
        return -EROFS;
    }
    rc = ext2_writeback_drain(em);
    if (rc != 0) {
        return rc;
    }
    em->opening = 1;
    if (!(em->state & EXT2_VALID_FS) || (em->state & EXT2_ERROR_FS)) {
        rc = ext2_check_volume(em);
    }
    ext2_batch_lock(em);
    if (rc == 0 && !em->wb) {
        rc = ext2_writeback_init(em);
    } else if (rc == 0) {
        (void)ext2_state_change(em, 0, EXT2_VALID_FS);
        rc = ext2_sb_store(em);
    }
    if (rc == 0) {
        em->valid_cleared = 1;
        ext2_super_lock(em);
        em->mode = EXT2_MODE_RW;
        ext2_super_unlock(em);
    }
    em->opening = 0;
    ext2_batch_unlock(em);
    return rc;
}

/**
 * The `remount` of `vfs_ops`: make the mount read-only or writable, by
 * VFS_MS_RDONLY in `flags`, and take `errors=` of `options`
 * (`errors_option`) once that succeeds, as Linux's `ext2_remount` sets the
 * options it parsed at its end; a remount that keeps the mount as it is
 * changes only the policy, and other options are ignored. -EINVAL for a
 * bad `errors=`, before anything changes. Admission is closed first, so
 * that no writer is inside while the mount changes and new ones wait
 * (`em_close`), and so is a second remount, which then reads the policy
 * this one left: a remount without `errors=` keeps the policy in force
 * when it ends, as Linux's `ext2_remount` starts from the options under
 * `s_umount` (fs/ext2/super.c:1336). A failed write, or a fault under
 * EXT2_ERRORS_RO, that makes the mount read-only is no remount: it sets the
 * flags at once and waits for nobody (`ext2_writeback_error`,
 * `ext2_mark_error`).
 *
 * The change goes by the mode. Toward read-only, only EXT2_MODE_RW does
 * work (`remount_readonly`); a mount read-only, stopped or failed takes the
 * options and nothing else, as Linux's `ext2_remount` returns early for a
 * file system already SB_RDONLY (fs/ext2/super.c:1352), so a stopped mount
 * stays stopped. Toward writable, EXT2_MODE_RO and EXT2_MODE_STOPPED both
 * go through `remount_writable` and its check, the one exit from a stop.
 */
static int ext2_remount(struct vfs_mount *mnt, uint32_t flags, const char *options)
{
    struct ext2_mount *em = ext2_em_of(mnt);
    int errors = 0;
    int rc = errors_option(options, &errors);

    if (rc != 0) {
        return rc;
    }
    em_close(em);
    if (flags & VFS_MS_RDONLY) {
        if (em->mode == EXT2_MODE_RW && !ext2_failed(em)) {
            rc = remount_readonly(mnt, em);
        }
    } else if (em->mode != EXT2_MODE_RW) {
        rc = remount_writable(em);
    } else if (ext2_failed(em)) {
        rc = -EROFS;
    }
    if (rc == 0 && errors != 0) {
        em->errors = errors;
    }
    em_open(em);
    return rc;
}

/**
 * The `shutdown` of `vfs_ops`, for each writable mount at a power off, the root
 * last (`kernel_shutdown`): admission closed for good and the writers inside
 * waited for until the tick `deadline` (`em_close_until`), then
 * `ext2_prepare_unmount`, in the remount's order, so that no writer changes the
 * volume once its valid bit is written.
 *
 * At the deadline nothing more is done and -EBUSY returned: a writer still
 * inside may hold a node lock, `em->batch`, a group's lock or a buffer, and
 * waits on a disk that may never answer, so a sync could wait for ever. The
 * valid bit stays clear on the medium, where a writable mount put it, and the
 * next mount checks the volume. A removed file still open, whose inode only its
 * last put frees, is -EBUSY too, after a sync: marked valid, the volume would
 * keep that inode for good.
 */
static int ext2_shutdown(struct vfs_mount *mnt, uint64_t deadline)
{
    struct ext2_mount *em = ext2_em_of(mnt);
    struct ext2_node *n;
    int rc;

    if (em_close_until(em, deadline) != 0) {
        klog(KLOG_WARN, "ext2", "shutdown: %d writers still inside; the volume stays unclean",
             (int)em->writers);
        return -EBUSY;
    }
    for (n = em->nodes; n; n = n->next) {
        if (n->unlinked && ext2_writes(em)) {
            klog(KLOG_WARN, "ext2", "shutdown: inode %d removed but open; the volume stays unclean",
                 (int)n->ino);
            rc = ext2_sync(mnt);
            return rc != 0 ? rc : -EBUSY;
        }
    }
    return ext2_prepare_unmount(mnt);
}

/**
 * Whether `blk`, a block of the cache, is in the inode table of its group:
 * the cache writes one only after the blocks it names (`bcache_sync_last`,
 * `table_names`), and a sync writes them last, since an inode names blocks
 * and a failed write of one must leave no pointer to it on the device. A
 * table outside its group, which mke2fs does not make without `flex_bg`,
 * keeps the device's order.
 */
static int block_in_table(void *arg, uint64_t blk)
{
    const struct ext2_mount *em = arg;
    uint64_t table;
    uint32_t g;

    if (blk < em->first_data || blk >= em->blocks) {
        return 0;
    }
    g = ((uint32_t)blk - em->first_data) / em->bpg;
    if (g >= em->groups) {
        return 0;
    }
    table = ext2_le32(em->gd + g * 32 + 8);
    return blk >= table &&
           blk < table + ((uint64_t)em->ipg * em->inode_size + BCACHE_BLOCK_BYTES - 1) /
                             BCACHE_BLOCK_BYTES;
}

/** Whether node `m` has blocks listed or under write that the device may
 * lack, so that its table block waits for them (`table_names`). The caller
 * holds `em->share_lock`. */
static int table_listed(const struct ext2_node *m)
{
    return m->nshared || m->shared_over || m->writing || m->unlisted;
}

/**
 * Whether the table block `blk` may name a block tagged `owner`, a data or
 * pointer block of inode `owner` (`ext2_write_owned`), that the device lacks:
 * that inode is in it and its node has allocated or freed since its blocks
 * were last written, by the test `table_unsynced` makes, or, while a list
 * was lost, is not in memory or was loaded again since (`lost_list`). A
 * block a file overwrote in place is named on the device already. The
 * cache writes such a block before the table block, as it does every
 * shared block dirtied before it. Runs under the cache's walks, under its
 * write lock or its spinlock and with or without the big lock, so it looks
 * the node up and reads its lists under `em->share_lock`, under which the
 * hash changes and a node leaves it before it is freed (`node_drop`), and
 * sleeps nowhere.
 */
static int table_names(void *arg, uint64_t blk, uint64_t owner)
{
    struct ext2_mount *em = arg;
    struct ext2_node *m;
    uint32_t flags;
    int names;

    if (owner == 0 || owner > em->inodes) {
        return 1;
    }
    if (ext2_inode_offset(em, (uint32_t)owner) >> BCACHE_BLOCK_SHIFT != blk) {
        return 0;
    }
    flags = spin_lock_irqsave(&em->share_lock);
    m = ext2_node_find(em, (uint32_t)owner);
    names =
        (m && table_listed(m)) || ((!m || m->lost_list) && (em->shared_lost || em->device_lost));
    spin_unlock_irqrestore(&em->share_lock, flags);
    return names;
}

/** The `unmount` of `vfs_ops`: the mount's nodes, batch, descriptors and
 * record freed; a batch still pending on a removed device is recorded as a
 * failure first. */
static void ext2_unmount(struct vfs_mount *mnt)
{
    struct ext2_mount *em = ext2_em_of(mnt);

    bcache_sync_last(em->dev, 0, 0, 0);
    if ((em->dev->flags & BLOCK_GONE) && em->wb && em->wb->count) {
        (void)ext2_writeback_error(em, -EIO);
    }
    nodes_free(em);
    mnt->priv = 0;
    ext2_writeback_free(em->wb);
    kfree(em->gd);
    kfree(em);
}

/** Clamp seconds to ext2's signed 32-bit range and return the disk bits. */
static uint32_t time_seconds(int64_t sec)
{
    if (sec < -2147483647 - 1) {
        return 0x80000000u;
    }
    if (sec > 2147483647) {
        return 0x7FFFFFFFu;
    }
    return (uint32_t)sec;
}

/** The real time in seconds, clamped to ext2's signed 32-bit range, for
 * the deletion time and the generation. */
static uint32_t now_seconds(void)
{
    int64_t sec = 0, nsec = 0;

    rtc_realtime(&sec, &nsec);
    return time_seconds(sec);
}

/**
 * The time `sec` and `nsec` as inode `n` can hold it in the field whose
 * extra word ends at `extra_end`: with the extra word, seconds from 1901
 * to 2446 and the nanoseconds; without, ext2's signed 32-bit seconds and
 * no nanoseconds. Seconds past either end are clamped to it.
 */
struct ext2_time ext2_time_fit(const struct ext2_node *n, uint32_t extra_end, int64_t sec,
                               int64_t nsec)
{
    struct ext2_time t;
    int64_t max = ext2_extra_has(n, extra_end) ? EXT2_TIME_MAX_EXTRA : EXT2_TIME_MAX32;

    t.sec = sec < EXT2_TIME_MIN ? EXT2_TIME_MIN : sec > max ? max : sec;
    t.nsec = 0;
    if (ext2_extra_has(n, extra_end) && nsec > 0 && nsec < 1000000000) {
        t.nsec = (uint32_t)nsec;
    }
    return t;
}

/**
 * Mark node `n` as changed in memory past its last store (`n->unstored`),
 * as Linux's `mark_inode_dirty` marks an inode at every change
 * (fs/ext2/dir.c:92-95, fs/ext2/inode.c:603 and 1303). Called before or
 * with every change of a field `inode_encode` writes, but the batch's,
 * under `n`'s lock or on a node the caller has alone. The mark keeps the
 * node from being dropped, synced past or left behind the valid bit until
 * a store of it succeeds.
 */
void ext2_node_dirty(struct ext2_node *n)
{
    n->unstored = 1;
}

/** Set the times of `n` that `which` selects (T_ATIME, T_MTIME, T_CTIME)
 * to the real time, as finely as the inode keeps each, and mark the node
 * (`ext2_node_dirty`). */
void ext2_stamp(struct ext2_node *n, int which)
{
    ext2_node_dirty(n);
    ext2_stamp_batch(n, which);
}

/** `ext2_stamp` without the mark, for a piece of the file-write batch,
 * which holds the node until its drain stores it. */
void ext2_stamp_batch(struct ext2_node *n, int which)
{
    int64_t sec = 0, nsec = 0;

    rtc_realtime(&sec, &nsec);
    if (which & T_ATIME) {
        n->atime = ext2_time_fit(n, EXT2_ATIME_EXTRA + 4, sec, nsec);
    }
    if (which & T_MTIME) {
        n->mtime = ext2_time_fit(n, EXT2_MTIME_EXTRA + 4, sec, nsec);
    }
    if (which & T_CTIME) {
        n->ctime = ext2_time_fit(n, EXT2_CTIME_EXTRA + 4, sec, nsec);
    }
}

/** Put time `t` into the low word at `raw + lo` and, where the extra bytes
 * of `n` reach past `extra`, the extra word there. `t` is in the range
 * `ext2_time_fit` gives. */
static void time_put(const struct ext2_node *n, uint8_t *raw, uint32_t lo, uint32_t extra,
                     struct ext2_time t)
{
    uint32_t low = (uint32_t)(uint64_t)t.sec;
    uint32_t epoch = (uint32_t)((uint64_t)(t.sec - (int32_t)low) >> 32) & 3u;

    LE32_PUT(raw + lo, low);
    if (ext2_extra_has(n, extra + 4)) {
        LE32_PUT(raw + extra, epoch | (t.nsec << 2));
    }
}

/**
 * Whether the mount puts each metadata write, and each stage of a file's
 * write, on the medium before the next, in the order this driver keeps, as
 * a mount with `sync` or `dirsync` requires. Any other mount writes back as
 * Linux's ext2 does: what an operation changes stays dirty in the buffer
 * cache, and reaches the disk, in no promised order, at `sync`, `fsync`,
 * `syncfs`, unmount, the periodic sync of `update` and the cache's eviction;
 * a stop of the kernel or of the power before then can leave the volume in
 * a state the order does not promise, which `e2fsck` repairs.
 */
int ext2_em_strict(const struct ext2_mount *em)
{
    return em->mnt && (em->mnt->flags & (VFS_MS_SYNCHRONOUS | VFS_MS_DIRSYNC)) != 0;
}

/**
 * Take the big lock for what it still keeps when the caller runs without
 * it, as a writer of a file's data and an operation on names do: the node
 * table's walks and changes, the user's pages under a copy from them
 * (`vm_resident`) and the VFS's flags of the mount. 1 when taken, for
 * `ext2_big_exit`, and 0 when the caller held it. The caller holds no
 * spinlock. A sleep lets the lock go and takes it back, as every holder's
 * does.
 */
int ext2_big_enter(void)
{
    if (bkl_held()) {
        return 0;
    }
    bkl_enter();
    return 1;
}

/** Give back the big lock `ext2_big_enter` took when `taken` says it did. */
void ext2_big_exit(int taken)
{
    if (taken) {
        bkl_exit();
    }
}

/** A barrier of the order: `bcache_sync` of the mount's device on a strict
 * mount, and nothing on one that writes back, whose order is `sync`'s. 0,
 * or -errno. */
int ext2_em_sync(struct ext2_mount *em)
{
    return ext2_em_strict(em) ? bcache_sync(em->dev) : 0;
}

/** On a mount that writes back, write the device's dirty buffers in runs,
 * without the disk's flush, once they pass the dirty limit
 * (`bcache_dirty_level`), as Linux's `balance_dirty_pages` holds a writer
 * back there while its dirty pages are written; below it the flusher does
 * the writing (writeback.h). Only a process that has itself written
 * EXT2_BALANCE_SHARE bytes since it last did so pays, as Linux throttles
 * each task by its own rate: a drain runs on whoever comes next, and a
 * program that exits or logs a line would otherwise write what others
 * dirtied, for seconds. The operation that called it is done in the cache,
 * so a failed write is not its failure; the cache records it against the
 * device, as it records an eviction's. */
void ext2_em_balance(struct ext2_mount *em)
{
    if (ext2_em_strict(em) || !current_proc) {
        return;
    }
    if (bcache_dirty_level() < 2) {
        current_proc->fs_written = 0;
    } else if (current_proc->fs_written >= EXT2_BALANCE_SHARE) {
        current_proc->fs_written = 0;
        (void)bcache_writeback(em->dev, bcache_marks());
    }
}

/** List shared block `block` among those `n`'s next `fsync` writes, or
 * mark the list overflowed when it is full, under `em->share_lock`, since
 * an `fsync` takes the list without `n`'s lock. The caller lists a block
 * before the store that names it, so that an `fsync` that finds no list
 * finds no such store either. */
void ext2_node_share(struct ext2_mount *em, struct ext2_node *n, uint32_t block)
{
    uint32_t flags = spin_lock_irqsave(&em->share_lock);
    int i;

    for (i = 0; i < n->nshared && n->shared[i] != block; i++) {
    }
    if (i < n->nshared) {
    } else if (n->nshared == EXT2_NODE_SHARED) {
        n->shared_over = 1;
    } else {
        n->shared[n->nshared++] = block;
    }
    spin_unlock_irqrestore(&em->share_lock, flags);
}

/** List for `n` the bitmap at byte `bitmap` of group `g`'s descriptor, 0
 * for blocks and 4 for inodes, and the block of that descriptor. */
void ext2_node_share_group(struct ext2_mount *em, struct ext2_node *n, uint32_t g, uint32_t bitmap)
{
    ext2_node_share(em, n, ext2_le32(em->gd + g * 32 + bitmap));
    ext2_node_share(em, n, em->first_data + 1 + (g * 32) / BCACHE_BLOCK_BYTES);
}

/**
 * Copy `len` bytes from `src`, or zeros for NULL, to byte offset `off` of
 * the volume, written the way `sync` says (WRITE_THROUGH and the rest), or
 * left dirty with 0, tagged as a block of inode `owner` alone when that is
 * nonzero (`bcache_dirty_owned`), for a data or pointer block of a file,
 * and as shared otherwise, which clears a tag a freed block kept. A pointer
 * block that gains a pointer has its level added to `sync`
 * (`WRITE_LEVEL`), so that it is not written before what it names. A
 * whole block is not read first, since nothing of it survives. A block the
 * file-write batch has an image of gets the bytes in its image too, in the
 * same step as the buffer, so that the drain, which copies the image over
 * the cache's block, keeps them: an inode stored into a table block the
 * batch holds, a bit beside the batch's reservations, a descriptor or the
 * superblock's counts. The buffer is held across both copies, as the
 * batch holds it to list an image and the drain to copy one over the
 * block, so a reader that holds the buffer or copies under the batch's
 * spinlock sees each write whole; the cache's write lock keeps the marks
 * and writes (bcache.h), so the caller needs no big lock. This is the
 * write funnel: a mount that does not write (`ext2_writes`), read-only
 * outside the check and the opening of a writable mount or remount, or
 * failed, writes nothing: -EROFS, logged as `ext2: write refused: the mount
 * is read-only: block B`. 0, or -errno.
 */
int ext2_write_owned(struct ext2_mount *em, uint32_t owner, uint64_t off, const void *src,
                     uint32_t len, int sync)
{
    const uint8_t *in = src;
    int level = sync / WRITE_LEVEL(1);
    int rc = 0;

    if (!ext2_writes(em)) {
        klog(KLOG_WARN, "ext2", "write refused: the mount is read-only: block %d",
             (int)(off >> em->block_shift));
        return -EROFS;
    }
    sync &= WRITE_WAY;
    while (rc == 0 && len > 0) {
        uint64_t blk = off >> BCACHE_BLOCK_SHIFT;
        uint32_t at = (uint32_t)off & (BCACHE_BLOCK_BYTES - 1);
        uint32_t take = BCACHE_BLOCK_BYTES - at;
        struct bbuf *b;

        if (take > len) {
            take = len;
        }
        b = take == BCACHE_BLOCK_BYTES ? bcache_get_blank(em->dev, blk) : bcache_get(em->dev, blk);
        if (!b) {
            rc = -EIO;
            break;
        }
        if (at + take > b->bytes) {
            bcache_release(b);
            rc = -EIO;
            break;
        }
        if (in) {
            k_memcpy(b->data + at, in, take);
            in += take;
        } else {
            k_memset(b->data + at, 0, take);
        }
        (void)ext2_writeback_copy(em, (uint32_t)blk, at, b->data + at, take, 1);
        if (sync == WRITE_DURABLE || (sync == WRITE_THROUGH && ext2_em_strict(em))) {
            rc = bcache_write(b);
        } else if (sync == WRITE_UNFLUSHED && ext2_em_strict(em)) {
            rc = bcache_write_unflushed(b);
        } else if (sync == WRITE_FIRST || sync == WRITE_AFTER) {
            bcache_dirty_ordered(b, sync == WRITE_FIRST ? BCACHE_ORDER_FIRST : BCACHE_ORDER_AFTER);
            if (b->owner != owner) {
                bcache_dirty_owned(b, owner);
            }
        } else if (level) {
            bcache_dirty_pointer(b, owner, level);
        } else if (owner || b->owner) {
            bcache_dirty_owned(b, owner);
        } else {
            bcache_dirty(b);
        }
        bcache_release(b);
        off += take;
        len -= take;
    }
    return rc;
}

/** `ext2_write_owned` of a block no one file owns: an inode table, a bitmap, a
 * descriptor, the superblock, a directory's entries. */
int ext2_write_bytes(struct ext2_mount *em, uint64_t off, const void *src, uint32_t len, int sync)
{
    return ext2_write_owned(em, 0, off, src, len, sync);
}

/**
 * Whether the mount's write of a device failed: its `write_error`, which
 * `vfs_device_failed` and `ext2_writeback_error` set without calling the
 * mode's transitions, or its device's mark of a failed or removed write.
 * Such a mount is FAILED whatever its mode: it admits nothing, allocates
 * nothing and writes nothing more until it is unmounted.
 */
int ext2_failed(const struct ext2_mount *em)
{
    return em->mnt->write_error || (em->dev->flags & (BLOCK_GONE | BLOCK_WRITE_FAILED));
}

/** Whether the mount admits a new change of the volume: EXT2_MODE_RW and
 * not failed, as Linux's VFS admits one only without SB_RDONLY. */
int ext2_admits(const struct ext2_mount *em)
{
    return em->mode == EXT2_MODE_RW && !ext2_failed(em);
}

/** Whether the mount gives out blocks and inodes: as `ext2_admits`, so that
 * an operation under way on a stopped mount that needs one gives up and
 * rolls back as at ENOSPC. */
int ext2_allocates(const struct ext2_mount *em)
{
    return em->mode == EXT2_MODE_RW && !ext2_failed(em);
}

/** Whether the mount writes, to finish, roll back, reclaim or drain what is
 * under way: EXT2_MODE_RW and EXT2_MODE_STOPPED, as Linux's ext2 writes
 * under SB_RDONLY (fs/ext2/inode.c:71-111), and EXT2_MODE_RO only while it
 * opens (`em->opening`); never once failed. */
int ext2_writes(const struct ext2_mount *em)
{
    if (ext2_failed(em)) {
        return 0;
    }
    return em->mode != EXT2_MODE_RO || em->opening;
}

/** Whether the mount writes its superblock's state, the valid bit and the
 * counts: EXT2_MODE_RW and not failed, as Linux's `ext2_sync_super` and
 * `ext2_error` write it only without SB_RDONLY (fs/ext2/super.c:57, 155). */
int ext2_keeps_state(const struct ext2_mount *em)
{
    return em->mode == EXT2_MODE_RW && !ext2_failed(em);
}

/**
 * Whether a change of attributes or of the namespace may be made: 0, -EROFS
 * on a mount that admits none (`ext2_admits`), or the drain's -errno. A
 * strict mount drains the batch first, so that the change's writes through
 * reach the medium after the file data and pointers an inode it stores
 * names. A mount that writes
 * back does not: `ext2_write_bytes` keeps the batch's images current, and
 * `ext2_punch` drains a batch that holds the file it frees. The caller holds
 * the node lock the change needs.
 */
int ext2_assert_writable(struct vfs_mount *mnt, struct ext2_mount *em)
{
    int rc = 0;

    if (ext2_em_strict(em)) {
        rc = ext2_writeback_drain(em);
    }
    (void)mnt;
    if (rc == 0 && !ext2_admits(em)) {
        rc = -EROFS;
    }
    return rc;
}

/** The byte offset on the volume of inode `ino`'s slot, found through its
 * group's descriptor. */
uint64_t ext2_inode_offset(const struct ext2_mount *em, uint32_t ino)
{
    uint32_t g = (ino - 1) / em->ipg;
    uint32_t index = (ino - 1) % em->ipg;
    uint32_t table = ext2_le32(em->gd + g * 32 + 8);

    return (uint64_t)table * em->block_size + (uint64_t)index * em->inode_size;
}

/**
 * Set the superblock's `large_file` read-only-compatible flag, written
 * through. e2fsck expects it once a regular file reaches 2 GiB, and the
 * flag is set before the inode that needs it is stored, so a stop between
 * the two leaves a volume e2fsck finds no fault in. 0, or -errno.
 */
static int large_file_flag(struct ext2_mount *em)
{
    uint8_t raw[4];
    int rc;

    ext2_super_lock(em);
    LE32_PUT(raw, ext2_feature_add(em, &em->ro_compat, EXT2_RO_LARGE_FILE));
    rc = ext2_write_bytes(em, 1024 + 100, raw, 4, 1);
    ext2_super_unlock(em);
    return rc;
}

/**
 * Encode the fields of `n` this driver keeps into the inode `raw`, leaving
 * its other bytes as they are; the deletion time is cleared. On a volume of
 * large inodes `raw` has EXT2_LARGE bytes or more, and the extra size and
 * the extra fields it covers are written too. The size is read whole
 * (`node_size`), since a drain encodes a node another process may be
 * resizing under its own lock.
 */
static void inode_encode(const struct ext2_mount *em, const struct ext2_node *n, uint8_t *raw)
{
    uint64_t size = node_size(n);
    int i;

    LE16_PUT(raw + 0, n->mode);
    LE16_PUT(raw + 2, n->uid & 0xFFFFu);
    LE32_PUT(raw + 4, (uint32_t)size);
    time_put(n, raw, 8, EXT2_ATIME_EXTRA, n->atime);
    time_put(n, raw, 12, EXT2_CTIME_EXTRA, n->ctime);
    time_put(n, raw, 16, EXT2_MTIME_EXTRA, n->mtime);
    LE32_PUT(raw + 20, 0); /* i_dtime: a live inode has none, whatever it had */
    LE16_PUT(raw + 24, n->gid & 0xFFFFu);
    LE16_PUT(raw + 26, n->nlink);
    LE32_PUT(raw + 28, n->blocks);
    LE32_PUT(raw + 32, n->flags);
    for (i = 0; i < (int)EXT2_NBLOCKS; i++) {
        LE32_PUT(raw + 40 + i * 4, n->block[i]);
    }
    LE32_PUT(raw + 100, n->generation);
    LE32_PUT(raw + 104, n->file_acl);
    LE16_PUT(raw + 120, (n->uid >> 16) & 0xFFFFu);
    LE16_PUT(raw + 122, (n->gid >> 16) & 0xFFFFu);
    if ((n->mode & S_IFMT_) == S_IFREG_) {
        LE32_PUT(raw + 108, (uint32_t)(size >> 32));
    }
    if (em->inode_size > EXT2_GOOD_OLD) {
        LE16_PUT(raw + EXT2_EXTRA_ISIZE, n->extra_isize);
        if (ext2_extra_has(n, EXT2_CRTIME + 4)) {
            time_put(n, raw, EXT2_CRTIME, EXT2_CRTIME_EXTRA, n->crtime);
        }
    }
}

/** Whether byte `at` of an inode slot holds one of its times: the access,
 * change and modification times, their extra words and the creation time. */
static int inode_time_byte(uint32_t at)
{
    return (at >= 8 && at < 20) || (at >= EXT2_CTIME_EXTRA && at < EXT2_CRTIME_EXTRA + 4);
}

/** Whether the bytes `len` of an inode slot changed from `was` to `now` in
 * a field other than the times and the deletion time, which the encoder
 * clears on a live inode whatever the slot had: a change that only
 * `ext2_node_dirty` makes legal. */
static int inode_field_changed(const uint8_t *was, const uint8_t *now, uint32_t len)
{
    uint32_t i;

    for (i = 0; i < len; i++) {
        if (now[i] != was[i] && !inode_time_byte(i) && (i < 20 || i >= 24)) {
            return 1;
        }
    }
    return 0;
}

/** Set `n->dirty_meta` under `em->share_lock`, which an `fsync` clears
 * without `n`'s lock. Call after the table buffer's store attempt, so a
 * sync cannot take a completed store's mark before that store. */
void ext2_node_meta_dirty(struct ext2_mount *em, struct ext2_node *n)
{
    uint32_t flags = spin_lock_irqsave(&em->share_lock);

    n->dirty_meta = 1;
    spin_unlock_irqrestore(&em->share_lock, flags);
}

/** `inode_encode` of `n` over the `len` bytes of its slot at `raw`; return
 * 1 when a byte other than a time's changes, else 0. The caller publishes
 * that change with `ext2_node_meta_dirty` after storing the table buffer. */
int ext2_inode_encode(struct ext2_mount *em, struct ext2_node *n, uint8_t *raw, uint32_t len)
{
    uint8_t was[EXT2_LARGE];
    uint32_t i;

    k_memcpy(was, raw, len);
    inode_encode(em, n, raw);
    for (i = 0; i < len; i++) {
        if (raw[i] != was[i] && !inode_time_byte(i)) {
            return 1;
        }
    }
    return 0;
}

/**
 * Write inode `n` in the way `how` says, over the bytes its slot has, or over
 * zeros when `fresh`, for an inode just allocated: a freed inode's slot keeps
 * what it had, and the encoder writes only the fields this driver keeps. A
 * fresh inode is written over its whole slot, so that a large slot loses the
 * attributes a freed inode kept in it; any other store writes the part of the
 * slot the encoder covers. A regular file of 2 GiB or more sets `large_file`
 * first.
 *
 * An inode the file-write batch holds stays in the node on a mount that
 * writes back (`ext2_writeback_keeps`): its pointers may name blocks the batch
 * has not put in the cache yet, and the table block in the cache goes to the
 * device with any neighbour's sync. The drain encodes the node. A strict
 * mount drains the batch before any store but the batch's own
 * (`ext2_assert_writable`). A table block that cannot be read is reported
 * to the mount's policy, as for a load (`table_unread`). A change of a
 * field but the times sets `n->dirty_meta` after the store, even on error
 * so a retry covers any dirty buffer left by the failed write. The
 * superblock is listed among `n`'s shared blocks when `large_file` is set
 * for it. The caller holds `n`'s lock, or has `n` alone, a new inode or one
 * freed at its last put; that keeps the slot's bytes and the batch's hold
 * on `n` from changing under the read, the batch test and the write, so it
 * needs no big lock. 0, or -errno.
 *
 * Only a store that succeeds clears the node's mark (`n->unstored`), and
 * so does one left to a drain still to come, which owes it. A failed one
 * leaves it, so that the next settle stores the node again. A slot whose
 * fields but the times and the deletion time would change on a node that
 * has no mark, and that the batch does not hold, is a change made without
 * `ext2_node_dirty`, and
 * panics, as `ext2_assert_node_locked` does for a change outside the lock,
 * so that any test that runs the operation finds the missed site.
 */
int ext2_inode_write(struct ext2_mount *em, struct ext2_node *n, int fresh, int how)
{
    uint8_t raw[256], was[EXT2_LARGE];
    uint64_t at;
    uint32_t len = em->inode_size > EXT2_GOOD_OLD ? EXT2_LARGE : EXT2_GOOD_OLD;
    int rc = ext2_slot_at(em, READ_SLOT, n->ino, &at), changed = fresh;

    if (rc != 0) {
        return rc;
    }
    if (fresh) {
        len = em->inode_size;
        k_memset(raw, 0, len);
        inode_encode(em, n, raw);
    } else {
        rc = ext2_read_bytes(em, READ_SLOT, n->ino, at, raw, len);
        if (rc != 0) {
            return rc;
        }
        k_memcpy(was, raw, len);
        changed = ext2_inode_encode(em, n, raw, len);
        if (changed && !n->unstored && !ext2_writeback_holds(em, n) &&
            inode_field_changed(was, raw, len)) {
            klog(KLOG_CRIT, "ext2", "inode %d: changed but not marked", (int)n->ino);
            k_panic("ext2: a change of an inode without its mark");
        }
    }
    if ((n->mode & S_IFMT_) == S_IFREG_ && n->size >= 0x80000000u &&
        !(em->ro_compat & EXT2_RO_LARGE_FILE)) {
        ext2_node_share(em, n, 0);
        rc = large_file_flag(em);
        if (rc != 0) {
            return rc;
        }
    }
    if (!fresh && ext2_writeback_keeps(em, n)) {
        n->unstored = 0;
        return 0;
    }
    rc = ext2_write_bytes(em, at, raw, len, how);
    if (changed) {
        ext2_node_meta_dirty(em, n);
    }
    if (rc == 0) {
        n->unstored = 0;
    }
    return rc;
}

/** Write inode `n` as `ext2_inode_write` does with WRITE_THROUGH: the fields
 * this driver keeps, over the bytes the slot has, so that what it does not keep
 * stays as it was. */
int ext2_inode_store(struct ext2_mount *em, struct ext2_node *n)
{
    return ext2_inode_write(em, n, 0, WRITE_THROUGH);
}

/**
 * Write a directory's times into the cache, for the next sync to put on
 * the disk: after an entry is added or removed only the times are left,
 * and a crash that loses them loses nothing else, since every inode of
 * the block goes out whole with each write through of it. 0, or -errno.
 */
int ext2_inode_times_later(struct ext2_mount *em, struct ext2_node *d)
{
    return ext2_inode_write(em, d, 0, 0);
}

/**
 * Report a fault of the volume met at run time, once the caller has logged
 * it, as Linux's `ext2_error` does (fs/ext2/super.c:49-80), each time it is
 * met, whatever the state was before. The error bit is set in memory under
 * `em->count_lock`, then, on a writable mount, the superblock written to
 * the medium under `em->super`; a remount read-only that is writing the
 * state meanwhile writes it again with the bit before it lets the mount
 * stop writing (`finish_writes`), as Linux's bit, set under `s_lock`,
 * reaches the disk while a remount syncs; a mount that does not keep its
 * state (`ext2_keeps_state`) keeps the bit in memory alone, which a remount
 * writable writes. Then the mount's policy (`em->errors`):
 * EXT2_ERRORS_CONTINUE goes on; EXT2_ERRORS_RO moves EXT2_MODE_RW to
 * EXT2_MODE_STOPPED and sets the VFS's read-only flag in the same hold of
 * `em->super`, waiting for nobody, as Linux sets SB_RDONLY: no new operation
 * is admitted and no block or inode is allocated, while the operations under
 * way, the caller's among them, write what finishes or rolls back their
 * changes (`ext2_writes`), and only a remount writable, which waits for them
 * and checks the volume, ends the stop (`remount_writable`); the batch's
 * writes accepted before are drained by the flusher, a sync or that
 * remount; and EXT2_ERRORS_PANIC stops the kernel, on any mount. A reader
 * without the big lock takes it, for the mount's flags. The caller holds no
 * buffer.
 */
void ext2_mark_error(struct ext2_mount *em)
{
    int big = !bkl_held();
    int stop;

    if (big) {
        bkl_enter();
    }
    (void)ext2_state_change(em, EXT2_ERROR_FS, 0);
    ext2_super_lock(em);
    if (ext2_keeps_state(em)) {
        (void)ext2_sb_store(em);
    }
    stop = em->mode == EXT2_MODE_RW && em->errors == EXT2_ERRORS_RO;
    if (stop) {
        em->mode = EXT2_MODE_STOPPED;
        em->mnt->flags |= VFS_MS_RDONLY;
    }
    ext2_super_unlock(em);
    if (em->errors == EXT2_ERRORS_PANIC) {
        k_panic("ext2: panic from previous error");
    }
    if (stop) {
        klog(KLOG_CRIT, "ext2", "error: remounting read-only");
    }
    if (big) {
        bkl_exit();
    }
}

/** Write the superblock's free counts and its state as `ext2_write_bytes` does
 * with `sync`, under `em->super`, read in one step under `em->count_lock`
 * after it is taken, so that the last write carries the latest values; 0, or
 * -errno. */
static int sb_write(struct ext2_mount *em, int sync)
{
    uint8_t raw[8], state[2];
    uint32_t flags;
    int rc;

    ext2_super_lock(em);
    flags = spin_lock_irqsave(&em->count_lock);
    LE32_PUT(raw, (uint32_t)em->free_blocks);
    LE32_PUT(raw + 4, (uint32_t)em->free_inodes);
    LE16_PUT(state, em->state);
    spin_unlock_irqrestore(&em->count_lock, flags);
    rc = ext2_write_bytes(em, 1024 + 12, raw, 8, sync);
    if (rc == 0) {
        rc = ext2_write_bytes(em, 1024 + 58, state, 2, sync);
    }
    ext2_super_unlock(em);
    return rc;
}

/** Write the superblock's free counts and its state to the medium, on any
 * mount; 0, or -errno. */
int ext2_sb_store(struct ext2_mount *em)
{
    return sb_write(em, WRITE_DURABLE);
}

/** Leave the superblock's free counts and its state dirty in the cache, for
 * the sync that follows; 0, or -errno. */
static int sb_note(struct ext2_mount *em)
{
    return sb_write(em, 0);
}

/** The pointer blocks data index `index` can need at most: none for a
 * direct block, one per level of indirection above them. */
uint32_t ext2_pointer_levels(const struct ext2_mount *em, uint32_t index)
{
    uint32_t per = em->block_size / 4;

    if (index < EXT2_NDIR) {
        return 0;
    }
    index -= EXT2_NDIR;
    if (index < per) {
        return 1;
    }
    if (index < per + per * per) {
        return 2;
    }
    return 3;
}

/** Bring byte offset `at` of the volume's block into the cache with no lock
 * held, for a caller that reads it under a group's lock; a failure is left to
 * that read. */
void ext2_block_fetch(struct ext2_mount *em, uint64_t at)
{
    ext2_volume_fetch(em, at >> BCACHE_BLOCK_SHIFT);
}

/** Fetch what freeing the inode of `n` reads under its groups' locks: its inode
 * bitmap and its group's descriptor, and its attribute block with that
 * block's bitmap. */
static void inode_blocks_fetch(struct ext2_mount *em, const struct ext2_node *n)
{
    uint32_t g = (n->ino - 1) / em->ipg;

    ext2_block_fetch(em, (uint64_t)ext2_le32(em->gd + g * 32 + 4) * em->block_size);
    ext2_gd_fetch(em, g);
    if (n->file_acl != 0 && ext2_data_block_ok(em, n->file_acl)) {
        g = (n->file_acl - em->first_data) / em->bpg;
        ext2_block_fetch(em, (uint64_t)n->file_acl * em->block_size);
        ext2_block_fetch(em, (uint64_t)ext2_le32(em->gd + g * 32) * em->block_size);
    }
}

/**
 * Free inode `n`, which has no name and no holder: the blocks of a regular
 * file, a directory or a long symbolic link from the end (`ext2_punch`), then
 * the inode written zeroed with its deletion time, then its reference to
 * its extended-attribute block (`ext2_attr_release`), then its bit and counts,
 * these two under their groups' locks with the blocks they read fetched
 * first, so that a lock waits on the disk only for a block evicted again.
 * The node's `unlinked` mark is cleared, and once the bit is free its
 * `unstored` mark, since nothing of it is left to store. Each step reads
 * through the reclaim reader (`ext2_read_reclaim`), which reports a damaged
 * or unread block and leaves what it holds taken, so a step returns only a
 * failed write, after which nothing further would land. 0, or -errno.
 */
int ext2_release_inode(struct ext2_mount *em, struct ext2_node *n)
{
    int was_dir = (n->mode & S_IFMT_) == S_IFDIR_;
    int rc;

    if ((n->mode & S_IFMT_) == S_IFREG_ || was_dir ||
        ((n->mode & S_IFMT_) == S_IFLNK_ && !ext2_fast_link(em, n))) {
        rc = ext2_punch(em, n, 0);
        if (rc != 0) {
            return rc;
        }
    }
    {
        uint8_t raw[128];

        k_memset(raw, 0, sizeof(raw));
        LE32_PUT(raw + 20, now_seconds()); /* i_dtime */
        rc = ext2_write_bytes(em, ext2_inode_offset(em, n->ino), raw, sizeof(raw), 1);
    }
    if (rc != 0) {
        return rc;
    }
    inode_blocks_fetch(em, n);
    if (n->file_acl != 0) {
        rc = ext2_attr_release(em, n->ino, n->file_acl);
    }
    if (rc == 0) {
        n->unlinked = 0;
        rc = ext2_inode_bit_free(em, n->ino, was_dir);
    }
    if (rc == 0) {
        n->unstored = 0;
    }
    return rc;
}

/* ---- the mount ---- */

/**
 * The largest regular file a volume with `1 << block_shift`-byte blocks
 * can hold, Linux's `s_maxbytes` for ext2 (`ext2_max_size`,
 * fs/ext2/super.c:730): the direct, single, double and triple pointers'
 * reach, unless a dense file that large, its pointer blocks counted, would
 * pass the 32-bit `i_blocks` count of 512-byte units; then the most data
 * blocks that, with the pointer blocks they need, fit that count. Linux's
 * cap at MAX_LFS_FILESIZE never binds for blocks of 4 KiB or less, whose
 * limit is under 2 TiB. The divisions are of 32-bit values, which the kernel
 * makes without libgcc.
 */
static uint64_t file_limit(uint32_t block_shift)
{
    uint32_t ppb = (uint32_t)1 << (block_shift - 2);
    uint32_t upper = 0xFFFFFFFFu >> (block_shift - 9);
    uint32_t meta = 1 + (1 + ppb) + (1 + ppb + ppb * ppb);
    uint64_t res = EXT2_NDIR + (uint64_t)ppb + (uint64_t)ppb * ppb + (uint64_t)ppb * ppb * ppb;

    if (res + meta > upper) {
        res = upper;
        upper -= EXT2_NDIR + ppb;
        meta = 1;
        if (upper < ppb * ppb) {
            meta += 1 + (upper + ppb - 1) / ppb;
        } else {
            meta += 1 + ppb;
            upper -= ppb * ppb;
            meta += 1 + (upper + ppb - 1) / ppb + (upper + ppb * ppb - 1) / (ppb * ppb);
        }
        res -= meta;
    }
    return res << block_shift;
}

/** Take the superblock `sb`'s state into `em`, and its s_errors as the
 * error policy unless `errors=` set one: an unknown value is remount-ro, as
 * Linux's `ext2_fill_super` takes it (fs/ext2/super.c:898-903). */
static void state_take(struct ext2_mount *em, const uint8_t *sb)
{
    uint32_t v = ext2_le16(sb + 60);

    em->state = ext2_le16(sb + 58);
    if (em->errors == 0) {
        em->errors = v == EXT2_ERRORS_PANIC || v == EXT2_ERRORS_CONTINUE ? (int)v : EXT2_ERRORS_RO;
    }
}

/** Log `ext2: refused: FIELD` and return -EINVAL, which fails the mount on
 * `field`. */
static int refuse(const char *field)
{
    klog(KLOG_WARN, "ext2", "refused: %s", field);
    return -EINVAL;
}

/**
 * The constructor's work on the record `em`, which `ext2_mount` frees, with
 * its nodes, batch and descriptors, when this fails: the volume checked
 * field by field before anything is published, the superblock's magic,
 * revision, sizes and counts, the features against the three sets, the
 * descriptors against the volume, and the root inode; then, for a writable
 * mount, the check of a volume not cleanly unmounted and the batch. Returns
 * 0 with `mnt->priv` and `mnt->root` set and the record `ext2: mounted:
 * ...`, or the errors `vfs_ext2_type` lists in ext2.h.
 *
 * A group count whose descriptor table, 32 bytes a group, would overflow
 * the size asked of the heap or not fit the volume fails the mount, since the
 * product would wrap and the checks after it read past what was allocated.
 * Each group's two bitmaps and inode table must lie in that group, past its
 * superblock copy and descriptors when it has them, and apart from each
 * other; that is where `ext2_data_block_ok` looks for them.
 */
static int ext2_mount_on(struct ext2_mount *em, struct vfs_mount *mnt, block_device_t *dev,
                         uint32_t flags)
{
    uint8_t sb[1024];
    uint32_t log_size, rev, unknown;
    const char *readonly_why = 0;
    struct ext2_node *root;
    uint32_t g;
    int rc;

    em->dev = dev;
    em->mnt = mnt;
    em->next_generation = now_seconds() << 8;
    rc = ext2_read_bytes(em, READ_QUIET, 0, 1024, sb, sizeof(sb));
    if (rc != 0) {
        return rc;
    }
    if (ext2_le16(sb + 56) != EXT2_SUPER_MAGIC) {
        return refuse("magic");
    }
    rev = ext2_le32(sb + 76);
    if (rev > 1) {
        return refuse("revision");
    }
    log_size = ext2_le32(sb + 24);
    if (log_size > 2) {
        return refuse("block size");
    }
    em->block_size = 1024u << log_size;
    em->block_shift = 10 + log_size;
    em->file_limit = file_limit(em->block_shift);
    mnt->max_bytes = em->file_limit;
    em->inodes = ext2_le32(sb + 0);
    em->blocks = ext2_le32(sb + 4);
    em->r_blocks = ext2_le32(sb + 8);
    em->first_data = ext2_le32(sb + 20);
    em->bpg = ext2_le32(sb + 32);
    em->ipg = ext2_le32(sb + 40);
    state_take(em, sb);
    if (rev == 0) {
        em->inode_size = 128;
        em->first_ino = 11;
        em->compat = em->incompat = em->ro_compat = 0;
    } else {
        em->first_ino = ext2_le32(sb + 84);
        em->inode_size = ext2_le16(sb + 88);
        em->compat = ext2_le32(sb + 92);
        em->incompat = ext2_le32(sb + 96);
        em->ro_compat = ext2_le32(sb + 100);
        if (em->compat & EXT2_COMPAT_RESIZE_INODE) {
            em->reserved_gdt = ext2_le16(sb + 206);
        }
    }
    if (em->inode_size != 128 && em->inode_size != 256) {
        return refuse("inode size");
    }
    if (em->first_data != (log_size == 0 ? 1u : 0u)) {
        return refuse("first data block");
    }
    if (em->blocks == 0 || em->bpg == 0 || em->ipg == 0 || em->inodes == 0 ||
        em->bpg > em->block_size * 8 || em->ipg > em->block_size * 8 || em->first_ino < 2 ||
        em->first_ino > em->inodes) {
        return refuse("counts");
    }
    if ((uint64_t)em->blocks * em->block_size > dev->sector_count * (uint64_t)dev->sector_size) {
        return refuse("block count past the device");
    }
    em->groups = (em->blocks - em->first_data + em->bpg - 1) / em->bpg;
    if ((uint64_t)em->groups * em->ipg < em->inodes) {
        return refuse("inodes past the groups");
    }
    if ((uint64_t)em->first_data + 1 + ext2_gd_blocks(em) + em->reserved_gdt > em->blocks) {
        return refuse("descriptor table past the volume");
    }
    unknown = em->incompat & ~EXT2_INCOMPAT_FILETYPE;
    if (unknown != 0) {
        klog(KLOG_WARN, "ext2", "refused: incompatible feature 0x%x", (int)unknown);
        return -EINVAL;
    }
    /* A new large inode covers every extra field, or more where the
     * superblock wants more; with `extra_isize` every inode has at least
     * the superblock's minimum. */
    if (em->inode_size > EXT2_GOOD_OLD) {
        uint32_t room = em->inode_size - EXT2_GOOD_OLD;
        uint32_t min = (em->ro_compat & EXT2_RO_EXTRA_ISIZE) ? ext2_le16(sb + 348) : 0;
        uint32_t want = (em->ro_compat & EXT2_RO_EXTRA_ISIZE) ? ext2_le16(sb + 350) : 0;

        em->new_extra = EXT2_EXTRA_NEW;
        if (want > em->new_extra && !(want & 3u) && want <= room) {
            em->new_extra = want;
        }
        if ((min & 3u) || min > room) {
            readonly_why = "the minimum extra inode size does not fit the inode";
        } else if (min > em->new_extra) {
            em->new_extra = min;
        }
    }
    unknown = em->ro_compat & ~(EXT2_RO_SPARSE_SUPER | EXT2_RO_LARGE_FILE | EXT2_RO_EXTRA_ISIZE);
    if (unknown != 0) {
        readonly_why = "unknown read-only-compatible feature";
    } else if (!readonly_why && em->block_size != 4096) {
        readonly_why = "a block size below 4096 cannot be written in order";
    }
    if (em->groups == 0 || em->groups > 0xFFFFFFFFu / 33 || em->blocks <= em->first_data ||
        (em->groups * 32 + em->block_size - 1) / em->block_size > em->blocks - em->first_data - 1) {
        klog(KLOG_WARN, "ext2", "refused: %d groups do not fit a table on %d blocks",
             (int)em->groups, (int)em->blocks);
        return -EINVAL;
    }
    em->gd = kzalloc(em->groups * 33); /* and a byte per group (`bitmap_check`) */
    if (!em->gd) {
        return -ENOMEM;
    }
    rc = ext2_read_bytes(em, READ_QUIET, 0, (uint64_t)(em->first_data + 1) * em->block_size, em->gd,
                         em->groups * 32);
    if (rc != 0) {
        return rc;
    }
    em->free_blocks = 0;
    em->free_inodes = 0;
    for (g = 0; g < em->groups; g++) {
        const uint8_t *gd = em->gd + g * 32;
        uint32_t bb = ext2_le32(gd + 0), ib = ext2_le32(gd + 4), it = ext2_le32(gd + 8);
        uint32_t lo = em->first_data + g * em->bpg;
        uint32_t hi = lo + em->bpg;
        uint32_t tables = ext2_inode_table_blocks(em);

        if (hi > em->blocks) {
            hi = em->blocks;
        }
        if (ext2_has_super(em, g)) {
            lo += 1 + ext2_gd_blocks(em);
        }
        if (bb < lo || bb >= hi || ib < lo || ib >= hi || it < lo || tables == 0 ||
            (uint64_t)it + tables > hi || bb == ib || (bb >= it && bb < it + tables) ||
            (ib >= it && ib < it + tables)) {
            klog(KLOG_WARN, "ext2",
                 "refused: group %d: bitmaps at %d and %d, table at %d, "
                 "in blocks %d to %d",
                 (int)g, (int)bb, (int)ib, (int)it, (int)lo, (int)hi);
            return -EINVAL;
        }
        /* Against the group's own size: the last group is often short. */
        if (ext2_le16(gd + 12) > hi - (em->first_data + g * em->bpg) ||
            ext2_le16(gd + 14) > (g == em->groups - 1 ? em->inodes - g * em->ipg : em->ipg)) {
            return refuse("group free count");
        }
        em->free_blocks += ext2_le16(gd + 12);
        em->free_inodes += ext2_le16(gd + 14);
    }
    root = ext2_node_for(em, EXT2_ROOT_INO, &rc);
    if (!root) {
        return rc == -ESTALE ? refuse("the root inode is freed") : rc;
    }
    if ((root->mode & S_IFMT_) != S_IFDIR_) {
        node_drop(em, root); /* not ext2_put: the mount is not on `mnt` yet */
        return refuse("root inode");
    }
    if (!readonly_why && !(flags & VFS_MS_RDONLY) &&
        (!(em->state & EXT2_VALID_FS) || (em->state & EXT2_ERROR_FS))) {
        rc = ext2_check_volume(em);
        if (rc != 0) {
            node_drop(em, root);
            return rc;
        }
    }
    em->mode = readonly_why == 0 && !(flags & VFS_MS_RDONLY) ? EXT2_MODE_RW : EXT2_MODE_RO;
    em->readonly_why = readonly_why;
    if (readonly_why) {
        klog(KLOG_WARN, "ext2", "mounted read-only: %s", readonly_why);
        mnt->flags |= VFS_MS_RDONLY;
    }
    em->alloc_hint = em->first_data;
    if (em->mode == EXT2_MODE_RW) {
        rc = ext2_writeback_init(em);
        if (rc != 0) {
            node_drop(em, root);
            return rc;
        }
        em->valid_cleared = 1;
    }
    mnt->priv = em;
    mnt->root = root;
    klog(KLOG_INFO, "ext2", "mounted: %d blocks of %d, %d inodes, %d groups%s", (int)em->blocks,
         (int)em->block_size, (int)em->inodes, (int)em->groups,
         em->mode == EXT2_MODE_RW ? "" : ", read-only");
    return 0;
}

/** The constructor of `vfs_ext2_type`, whose contract is in ext2.h: the
 * record from the heap, its policy set by `errors=` of `options` when they
 * have it (`errors_option`), given to `ext2_mount_on` and freed, with its
 * nodes, batch and descriptors, when that fails. */
int ext2_mount(struct vfs_mount *mnt, block_device_t *dev, uint32_t flags, const char *options)
{
    struct ext2_mount *em = kzalloc(sizeof(*em));
    int rc;

    if (!em) {
        return -ENOMEM;
    }
    em_locks_init(em);
    rc = errors_option(options, &em->errors);
    if (rc == 0) {
        rc = ext2_mount_on(em, mnt, dev, flags);
    }
    if (rc != 0) {
        nodes_free(em);
        ext2_writeback_free(em->wb);
        kfree(em->gd);
        kfree(em);
    } else if (em->block_size == BCACHE_BLOCK_BYTES) {
        bcache_sync_last(dev, block_in_table, table_names, em);
    }
    return rc;
}

/** The `lock_node` of `vfs_ops`: take the lock of the held node `node`
 * (`ext2_node_lock`). */
static void ext2_lock_node(struct vfs_mount *mnt, void *node)
{
    (void)mnt;
    ext2_node_lock(node);
}

/** The `unlock_node` of `vfs_ops`: give back a take of `ext2_node_lock`. The
 * last file lock a system call held is where the final puts it made under them
 * are done (`em_reap`). */
static void ext2_unlock_node(struct vfs_mount *mnt, void *node)
{
    ext2_node_unlock(node);
    em_reap(ext2_em_of(mnt));
}

/* ---- admission ---- */

/** End an operation `em_enter` admitted, with the final puts it left. */
static void em_finish(struct ext2_mount *em)
{
    em_leave(em);
    em_reap(em);
}

/** End an operation `em_enter_node` admitted, with the lock it took. */
static void em_finish_node(struct ext2_mount *em, struct ext2_node *n)
{
    em_leave(em);
    ext2_node_unlock(n);
    em_reap(em);
}

/**
 * Admit an operation on names to mount `em` and let the big lock go for it:
 * the operation runs under the directories' locks the VFS took, as Linux's
 * ext2 runs under the directories' `i_rwsem` (fs/namei.c:4121, 4482 and
 * 4615; `lock_rename`, 3250-3258), and takes the big lock for the steps
 * that still need it (`ext2_big_enter`). A caller that holds the big lock,
 * as a system call's router took it, has one hold let go and taken back by
 * `names_end`, which is what a sleep in the operation does anyway; a
 * caller that holds it deeper keeps the rest, and the operation runs under
 * it. 1 when a hold was let go.
 */
static int names_begin(struct ext2_mount *em)
{
    em_enter(em);
    if (bkl_held()) {
        bkl_exit();
        return 1;
    }
    return 0;
}

/** End an operation `names_begin` admitted, with the final puts it left,
 * and give the caller back the big lock that `names_begin` let go, as
 * `gave` says. */
static void names_end(struct ext2_mount *em, int gave)
{
    if (gave) {
        bkl_enter();
    }
    em_finish(em);
}

/** The `setattr` of `vfs_ops`: `ext2_setattr`, admitted under the node's
 * lock. */
static int admit_setattr(struct vfs_mount *mnt, void *node, int what, const struct vfs_stat *st)
{
    int rc;

    em_enter_node(ext2_em_of(mnt), node);
    rc = ext2_setattr(mnt, node, what, st);
    em_finish_node(ext2_em_of(mnt), node);
    return rc;
}

/** The `write` of `vfs_ops`: `ext2_file_write`, admitted under the node's lock.
 * */
static int32_t admit_write(struct vfs_mount *mnt, void *handle, const void *buf, uint32_t count,
                           uint64_t offset)
{
    struct ext2_handle *h = handle;
    int32_t rc;

    em_enter_node(ext2_em_of(mnt), h->node);
    rc = ext2_file_write(mnt, h->node, buf, count, offset);
    em_finish_node(ext2_em_of(mnt), h->node);
    return rc;
}

/** The `truncate` of `vfs_ops`: `ext2_truncate`, admitted under the node's
 * lock. */
static int admit_truncate(struct vfs_mount *mnt, void *handle, uint64_t size)
{
    struct ext2_handle *h = handle;
    int rc;

    em_enter_node(ext2_em_of(mnt), h->node);
    rc = ext2_truncate(mnt, h->node, size);
    em_finish_node(ext2_em_of(mnt), h->node);
    return rc;
}

/** The `create` of `vfs_ops`: `ext2_create`, admitted, without the big lock
 * (`names_begin`). */
static int admit_create(struct vfs_mount *mnt, void *dir, const char *name, uint32_t mode,
                        void **node)
{
    int gave, rc;

    gave = names_begin(ext2_em_of(mnt));
    rc = ext2_create(mnt, dir, name, mode, node);
    names_end(ext2_em_of(mnt), gave);
    return rc;
}

/** The `unlink` of `vfs_ops`: `ext2_unlink`, admitted, without the big lock
 * (`names_begin`). */
static int admit_unlink(struct vfs_mount *mnt, void *dir, const char *name)
{
    int gave, rc;

    gave = names_begin(ext2_em_of(mnt));
    rc = ext2_unlink(mnt, dir, name);
    names_end(ext2_em_of(mnt), gave);
    return rc;
}

/** The `mkdir` of `vfs_ops`: `ext2_mkdir`, admitted, without the big lock
 * (`names_begin`). */
static int admit_mkdir(struct vfs_mount *mnt, void *dir, const char *name, uint32_t mode)
{
    int gave, rc;

    gave = names_begin(ext2_em_of(mnt));
    rc = ext2_mkdir(mnt, dir, name, mode);
    names_end(ext2_em_of(mnt), gave);
    return rc;
}

/** The `rmdir` of `vfs_ops`: `ext2_rmdir`, admitted, without the big lock
 * (`names_begin`). */
static int admit_rmdir(struct vfs_mount *mnt, void *dir, const char *name)
{
    int gave, rc;

    gave = names_begin(ext2_em_of(mnt));
    rc = ext2_rmdir(mnt, dir, name);
    names_end(ext2_em_of(mnt), gave);
    return rc;
}

/** The `rename` of `vfs_ops`: `ext2_rename`, admitted, without the big lock
 * (`names_begin`). */
static int admit_rename(struct vfs_mount *mnt, void *olddir, const char *oldname, void *newdir,
                        const char *newname)
{
    int gave, rc;

    gave = names_begin(ext2_em_of(mnt));
    rc = ext2_rename(mnt, olddir, oldname, newdir, newname);
    names_end(ext2_em_of(mnt), gave);
    return rc;
}

/** The `link` of `vfs_ops`: `ext2_link`, admitted, without the big lock
 * (`names_begin`). */
static int admit_link(struct vfs_mount *mnt, void *dir, const char *name, void *node)
{
    int gave, rc;

    gave = names_begin(ext2_em_of(mnt));
    rc = ext2_link(mnt, dir, name, node);
    names_end(ext2_em_of(mnt), gave);
    return rc;
}

/** The `symlink` of `vfs_ops`: `ext2_symlink`, admitted, without the big lock
 * (`names_begin`). */
static int admit_symlink(struct vfs_mount *mnt, void *dir, const char *name, const char *target)
{
    int gave, rc;

    gave = names_begin(ext2_em_of(mnt));
    rc = ext2_symlink(mnt, dir, name, target);
    names_end(ext2_em_of(mnt), gave);
    return rc;
}

/** The `mknod` of `vfs_ops`: `ext2_mknod`, admitted, without the big lock
 * (`names_begin`). */
static int admit_mknod(struct vfs_mount *mnt, void *dir, const char *name, uint32_t mode,
                       uint32_t rdev, void **node)
{
    int gave, rc;

    gave = names_begin(ext2_em_of(mnt));
    rc = ext2_mknod(mnt, dir, name, mode, rdev, node);
    names_end(ext2_em_of(mnt), gave);
    return rc;
}

/** The `fallocate` of `vfs_ops`: `ext2_fallocate`, admitted under the node's
 * lock. */
static int admit_fallocate(struct vfs_mount *mnt, void *handle, uint32_t mode, uint64_t offset,
                           uint64_t length)
{
    struct ext2_handle *h = handle;
    int rc;

    em_enter_node(ext2_em_of(mnt), h->node);
    rc = ext2_fallocate(mnt, h->node, mode, offset, length);
    em_finish_node(ext2_em_of(mnt), h->node);
    return rc;
}

/** Read a node's external attribute block into a private checked copy, as
 * a block of kind READ_ATTR: one out of range is refused and reported, and a
 * failed read returned unreported, as Linux's `ext2_xattr_get` returns it.
 * A malformed block is logged, reported to the mount's policy
 * (`ext2_mark_error`) and -EIO, as Linux's `ext2_xattr_get` reports a bad
 * block (fs/ext2/xattr.c:229). The node is locked; NULL in `*out` means no
 * block. Returns 0 or -errno. */
static int attr_read(struct ext2_mount *em, struct ext2_node *n, uint8_t **out)
{
    int rc;

    *out = NULL;
    if (!n->file_acl)
        return 0;
    *out = kmalloc(em->block_size);
    if (!*out)
        return -ENOMEM;
    rc = ext2_read_bytes(em, READ_ATTR, n->ino, (uint64_t)n->file_acl * em->block_size, *out,
                         em->block_size);
    if (!rc) {
        rc = ext2_xattr_check(*out, em->block_size);
        if (rc != 0) {
            klog(KLOG_WARN, "ext2", "inode %d: attribute block %d is malformed", (int)n->ino,
                 (int)n->file_acl);
            ext2_mark_error(em);
        }
    }
    return rc;
}

/** Enable external attributes, promoting a revision-zero superblock first.
 * The revision is read before `em->super` is taken, since a failed read is
 * reported and the report takes that lock: two calls that both find
 * revision zero write the same bytes. `em->super` serialises the feature
 * write; a stop after promotion still has a valid dynamic revision with no
 * attribute inode yet. */
static int attr_feature(struct ext2_mount *em)
{
    uint8_t raw[16], feature[4];
    int rc;

    rc = ext2_read_bytes(em, READ_SUPER, 0, 1024 + 76, raw, sizeof(raw));
    ext2_super_lock(em);
    if (!rc && ext2_le32(raw) == 0) {
        LE32_PUT(raw, 1);
        LE32_PUT(raw + 8, em->first_ino);
        LE16_PUT(raw + 12, em->inode_size);
        rc = ext2_write_bytes(em, 1024 + 76, raw, sizeof(raw), WRITE_DURABLE);
    }
    if (!rc && !(em->compat & 8u)) {
        /* EXT2_FEATURE_COMPAT_EXT_ATTR precedes i_file_acl. */
        LE32_PUT(feature, ext2_feature_add(em, &em->compat, 8u));
        rc = ext2_write_bytes(em, 1024 + 92, feature, 4, WRITE_DURABLE);
    }
    ext2_super_unlock(em);
    return rc;
}

/** Store a private block in place, which needs no free block even on a
 * full disk. The node lock excludes its only inode's other callers. */
static int attr_inplace(struct vfs_mount *mnt, struct ext2_node *n, const uint8_t *raw)
{
    struct ext2_mount *em = ext2_em_of(mnt);
    int rc = ext2_em_write_out(mnt);

    if (!rc)
        rc = attr_feature(em);
    if (!rc)
        rc = ext2_write_owned(em, n->ino, (uint64_t)n->file_acl * em->block_size, raw,
                              em->block_size, WRITE_DURABLE);
    if (!rc) {
        ext2_stamp(n, T_CTIME);
        rc = ext2_inode_write(em, n, 0, WRITE_DURABLE);
    }
    return rc;
}

/** Replace an inode's attribute block without changing a shared block.
 * The node is locked. Allocation and contents reach disk before the inode;
 * the old block's reference goes only after the inode is durable, on every
 * mount. A failed write leaves allocated blocks for e2fsck, never reuses a
 * block an uncertain inode write may name. Returns 0 or -errno. */
static int attr_replace(struct vfs_mount *mnt, struct ext2_node *n, const uint8_t *raw,
                        int nonempty, int exclusive)
{
    struct ext2_mount *em = ext2_em_of(mnt);
    uint32_t old = n->file_acl, fresh = 0;
    int rc;

    if (!old && nonempty && n->blocks > 0xFFFFFFFFu - em->block_size / 512)
        return -EFBIG;
    rc = ext2_assert_writable(mnt, em);
    if (!rc)
        rc = ext2_writeback_drain(em);
    if (!rc && nonempty && exclusive)
        return attr_inplace(mnt, n, raw);
    if (!rc && nonempty)
        rc = ext2_alloc_block(em, old, &fresh);
    if (!rc)
        rc = ext2_em_write_out(mnt);
    if (!rc && nonempty)
        rc = ext2_write_owned(em, n->ino, (uint64_t)fresh * em->block_size, raw, em->block_size,
                              WRITE_DURABLE);
    if (!rc && nonempty)
        rc = attr_feature(em);
    if (rc)
        return rc;
    ext2_node_dirty(n);
    n->file_acl = fresh;
    n->blocks += (fresh != 0 ? em->block_size / 512 : 0);
    n->blocks -= (old != 0 ? em->block_size / 512 : 0);
    ext2_stamp(n, T_CTIME);
    rc = ext2_inode_write(em, n, 0, WRITE_DURABLE);
    if (rc)
        return rc;
    if (old) {
        uint32_t g = (old - em->first_data) / em->bpg;
        ext2_group_fetch(em, old);
        ext2_block_fetch(em, (uint64_t)old * em->block_size);
        rc = ext2_attr_release(em, n->ino, old);
        ext2_node_share(em, n, ext2_le32(em->gd + g * 32));
        ext2_node_share(em, n, em->first_data + 1 + (g * 32) / em->block_size);
    }
    if (!rc)
        rc = ext2_em_write_out(mnt);
    return rc;
}

/** The extended attribute operation, admitted and locked for shutdown.
 * Kernel buffers only; user. attributes of regular files and directories
 * are supported. Other namespaces return -EOPNOTSUPP. */
static int ext2_xattr(struct vfs_mount *mnt, void *node, struct vfs_xattr *arg)
{
    int op = arg->op;
    const char *name = arg->name;
    struct ext2_mount *em = ext2_em_of(mnt);
    struct ext2_node *n = node;
    uint8_t *raw = NULL, *out = NULL;
    uint32_t type = n->mode & S_IFMT_;
    int rc;

    if (op != VFS_XATTR_LIST && (!name || k_strlen(name) < 5 || name[0] != 'u' || name[1] != 's' ||
                                 name[2] != 'e' || name[3] != 'r' || name[4] != '.'))
        return -EOPNOTSUPP;
    if (op != VFS_XATTR_LIST && !name[5])
        return -EINVAL;
    if (op != VFS_XATTR_LIST && type != S_IFREG_ && type != S_IFDIR_)
        return op >= VFS_XATTR_SET ? -EPERM : -ENODATA;
    em_enter_node(em, n);
    rc = attr_read(em, n, &raw);
    if (!rc && op <= VFS_XATTR_LIST)
        rc = ext2_xattr_read(raw, em->block_size, name ? name + 5 : NULL, arg->value, arg->size);
    else if (!rc) {
        out = kmalloc(em->block_size);
        if (out) {
            struct vfs_xattr edit = *arg;
            edit.name += 5;
            rc = ext2_xattr_edit(raw, out, em->block_size, &edit);
        } else
            rc = -ENOMEM;
        if (rc >= 0)
            rc = attr_replace(mnt, n, out, rc, raw && ext2_le32(raw + 4) == 1);
    }
    kfree(raw);
    kfree(out);
    em_finish_node(em, n);
    return rc;
}

/** The `read` of `vfs_ops`, on the node held by the open handle. */
static int32_t open_read(struct vfs_mount *mnt, void *handle, void *buf, uint32_t count,
                         uint64_t offset)
{
    struct ext2_handle *h = handle;

    return ext2_read(mnt, h->node, buf, count, offset);
}

/** The `fsync` of `vfs_ops`, on the node held by the open handle. */
static int open_fsync(struct vfs_mount *mnt, void *handle)
{
    struct ext2_handle *h = handle;

    return ext2_fsync(mnt, h ? h->node : 0);
}

/** The `fdatasync` of `vfs_ops`, on the node held by the open handle. */
static int open_fdatasync(struct vfs_mount *mnt, void *handle)
{
    struct ext2_handle *h = handle;

    return ext2_fdatasync(mnt, h ? h->node : 0);
}

/** The `read_frames` of `vfs_ops`, on the node held by the open handle. */
static int32_t open_read_frames(struct vfs_mount *mnt, void *handle, uint32_t first, uint32_t count,
                                const uintptr_t *frames, const struct block_frames_done *done)
{
    struct ext2_handle *h = handle;

    return ext2_read_frames(mnt, h->node, first, count, frames, done);
}

/** The `handle_node` of `vfs_ops`: the node the open handle holds. */
static void *open_node(void *handle)
{
    return ((struct ext2_handle *)handle)->node;
}

static const struct vfs_ops ext2_ops = {
    ext2_lookup,
    0, /* lookup_rest */
    ext2_hold,
    ext2_put,
    ext2_getattr,
    admit_setattr,
    ext2_open,
    ext2_release,
    open_read,
    admit_write,
    admit_truncate,
    open_fsync,
    ext2_readdir,
    admit_create,
    admit_unlink,
    admit_mkdir,
    admit_rmdir,
    admit_rename,
    admit_link,
    ext2_readlink,
    ext2_statfs,
    ext2_sync,
    ext2_prepare_unmount,
    ext2_abort_unmount, /* abort_unmount */
    ext2_unmount,
    ext2_path,
    admit_symlink,
    admit_mknod,
    0, /* pin: a hold keeps a node */
    0, /* unpin */
    ext2_remount,
    admit_fallocate,
    open_fdatasync,
    ext2_lock_node,
    ext2_unlock_node,
    ext2_shutdown,
    ext2_xattr,
    ext2_cachestat,
    ext2_put_unless_last,
    ext2_readlink_cached,
    open_read_frames,
    open_node,
    ext2_writeback_old,
};

const struct vfs_type vfs_ext2_type = {"ext2", ext2_mount, &ext2_ops, 0, 0, 1, 1, 1, 1, 1};
