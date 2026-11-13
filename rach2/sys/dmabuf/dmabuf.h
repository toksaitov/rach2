/* Buffers shared by descriptors, their reservations and software fences. */
#ifndef _DMABUF_H_
#define _DMABUF_H_

#include "../drm/drm.h"
#include "../io/anon.h"
#include "../fs/file.h"

struct dma_buf_ops {
    int32_t (*mmap)(struct dma_buf *buf, vma_t *vma);
    void (*release)(struct dma_buf *buf);
    int32_t (*cpu_access)(struct dma_buf *buf, uint64_t flags);
    /* Borrow a physical page for a device import, or 0 on failure. */
    uintptr_t (*page)(struct dma_buf *buf, uint32_t pgoff);
};

/* Each exported buffer holds its exporter until the last descriptor or
 * mapping releases it. An external reservation must live that long too. */
struct dma_buf_poll_cb {
    struct dma_fence_cb cb;
    struct dma_buf *buf;
};

struct dma_buf {
    struct anon_obj anon;
    volatile uint32_t refs; /* changed atomically */
    uint64_t size, ino;
    const struct dma_buf_ops *ops;
    void *priv;
    int read_only;               /* export descriptor mode; 0 for read/write */
    uint32_t map_flags;          /* VM_PROT_WC or VM_PROT_UC, 0 for cached RAM */
    char name[DMA_BUF_NAME_LEN]; /* SET_NAME's terminated name, at most 31 bytes */
    uint8_t named;               /* SET_NAME has supplied a name, possibly empty */
    struct dma_resv *resv;
    struct dma_resv own;
    wait_queue_t wq;
    struct dma_fence *poll_fences[2]; /* leaf each poll listens on, or NULL */
    struct dma_buf_poll_cb poll_cb[2];
};

/** Export size bytes with one reference, or NULL; take priv on success.
 * resv is borrowed, or NULL for a reservation owned by the new buffer. */
struct dma_buf *dma_buf_export(uint64_t size, const struct dma_buf_ops *ops, void *priv,
                               struct dma_resv *resv);
/** Take a reference, accepting NULL. */
struct dma_buf *dma_buf_ref(struct dma_buf *buf);
/** Release a reference, accepting NULL. */
void dma_buf_put(struct dma_buf *buf);
/** Install a descriptor holding its own reference; flags is O_CLOEXEC or 0.
 * Access is read-only when buf->read_only is set, otherwise read/write.
 * Return the descriptor or -errno; retain the caller's reference. */
int32_t dma_buf_fd(struct dma_buf *buf, uint32_t flags);
/** Get a referenced buffer from the caller's descriptor, or NULL. */
struct dma_buf *dma_buf_get(int32_t fd);
/** Make an empty reservation with its lock; for a record made zeroed too. */
void dma_resv_init(struct dma_resv *resv);
/** Take and give back a reservation's lock (`struct dma_resv`); the take
 * may sleep. */
void dma_resv_lock(struct dma_resv *resv);
void dma_resv_unlock(struct dma_resv *resv);
/** Take a reservation's lock only if it is free, for a caller that may not
 * sleep, as Linux's dma_resv_trylock: 1 when taken. */
int dma_resv_trylock(struct dma_resv *resv);
/** Release every reservation slot and its storage, for its last user. */
void dma_resv_fini(struct dma_resv *resv);
/** Add a read fence, or join outstanding writers. 0 or -ENOMEM. The
 * reservation takes its own reference. The `_locked` form is called with
 * the lock held, the other takes it. */
int32_t dma_resv_add_fence(struct dma_resv *resv, struct dma_fence *fence, int write);
int32_t dma_resv_add_fence_locked(struct dma_resv *resv, struct dma_fence *fence, int write);
/** Snapshot the write fence and, for write access, the readers too.
 * Return a referenced fence (signalled for no work), or NULL on allocation
 * failure. The `_locked` form is called with the lock held, the other
 * takes it. */
struct dma_fence *dma_resv_snapshot(struct dma_resv *resv, int write);
struct dma_fence *dma_resv_snapshot_locked(struct dma_resv *resv, int write);
/** The fences of the reservation, each with a reference, in an array the
 * caller gives to `dma_resv_put_fences`: the write fence or NULL first,
 * then with `all` every read fence; 0 or -ENOMEM. Takes the lock. */
int32_t dma_resv_get_fences(struct dma_resv *resv, int all, struct dma_fence ***fences,
                            uint32_t *count);
void dma_resv_put_fences(struct dma_fence **fences, uint32_t count);
/** The write fence with a reference, or NULL. Takes the lock. */
struct dma_fence *dma_resv_get_write(struct dma_resv *resv);
/** Merge fences into one referenced all-of fence, or NULL on allocation
 * failure. Inputs are borrowed; nested arrays are flattened. */
struct dma_fence *dma_fence_merge(struct dma_fence **fences, uint32_t count);
/** Merge the leaves of `count` fences that have not signalled, as Linux's
 * dma_fence_unwrap_merge: a referenced all-of fence, the one leaf, or a new
 * signalled fence with the newest leaf's completion time when none is
 * left; NULL on allocation failure. Inputs are borrowed. */
struct dma_fence *dma_fence_unwrap_merge(struct dma_fence **fences, uint32_t count);
/** Number of leaf fences in Linux's dma_fence_unwrap order, and the
 * borrowed leaf at index, or NULL: an array's fences, a chain's own fence
 * and those of its earlier points not yet signalled, or the fence itself.
 * Each call walks the leaves afresh, and an earlier point that signals
 * between two calls, which an interrupt may make it do, leaves the list
 * and moves the later leaves down: a caller that visits every leaf walks
 * them with a `struct dma_fence_unwrap` instead. */
uint32_t dma_fence_count(struct dma_fence *fence);
struct dma_fence *dma_fence_at(struct dma_fence *fence, uint32_t index);
/** Whether a fence holds others, an array or a chain's point, as Linux's
 * dma_fence_is_container. Any context. */
int dma_fence_is_container(const struct dma_fence *fence);

/*
 * A walk over the leaves of a fence in the order of dma_fence_count, as
 * Linux's struct dma_fence_unwrap and dma_fence_unwrap_for_each: each
 * leaf is visited once, whatever signals meanwhile, and an earlier point
 * whose fence has signalled when the walk reaches it is passed over.
 * `chain` is the point the walk is at, with a reference, so that the
 * leaf it returned stays while the caller sleeps; `array` is the point's
 * fence when that is an array, borrowed from the point, and `index` the
 * array's fence returned last.
 */
struct dma_fence_unwrap {
    struct dma_fence *chain;
    struct dma_fence *array;
    uint32_t index;
};
/** Begin the walk over the leaves of `head`, a fence or NULL: the first
 * leaf, borrowed from the walk, or NULL when there is none. */
struct dma_fence *dma_fence_unwrap_first(struct dma_fence *head, struct dma_fence_unwrap *cursor);
/** The next leaf, borrowed from the walk, or NULL at the end, where the
 * walk has let go of what it held. Does not sleep. */
struct dma_fence *dma_fence_unwrap_next(struct dma_fence_unwrap *cursor);
/** Let go of what a walk left before its end holds; nothing after the
 * end. */
void dma_fence_unwrap_end(struct dma_fence_unwrap *cursor);
/** Status: 0 active, 1 completed, or the negative producer error. */
int32_t dma_fence_status(struct dma_fence *fence);
/** CLOCK_MONOTONIC in nanoseconds, under the big kernel lock. */
int64_t dma_fence_now(void);
/** Install a close-on-exec sync file with its own fence reference.
 * Return its descriptor or -errno. name may be NULL. */
int32_t sync_file_create(struct dma_fence *fence, const char *name);
struct sync_file;
/** A sync file of `fence` with a reference of its own, that no descriptor
 * holds yet; NULL for no memory. `name` is the user's name for it, which
 * only MERGE gives; NULL or empty names it from its fence, as Linux's
 * sync_file_get_name. */
struct sync_file *sync_file_new(struct dma_fence *fence, const char *name);
/** Free `s`, made by sync_file_new and never attached, and its fence
 * reference; nothing for NULL. */
void sync_file_drop(struct sync_file *s);
/** Make `fd`, a descriptor of the caller's that `fd_alloc` reserved and
 * gave no type, the close-on-exec descriptor of `s`, which it then owns.
 * Does not fail, so that a call can attach after work it cannot undo. */
void sync_file_attach(int32_t fd, struct sync_file *s);
/** Give the caller a sync file of `fence`, as Linux's ioctls do: reserve
 * a descriptor, put it in `*fd_at`, which lies in the `len` bytes at `src`,
 * make the file, copy those bytes to `dst`, and only then attach it, so
 * that a failure leaves no descriptor and no file. 0, -EINVAL, -EMFILE,
 * -ENOMEM or -EFAULT. */
int32_t sync_file_copy_out(struct dma_fence *fence, const char *name, void __user *dst, void *src,
                           uint32_t len, int32_t *fd_at);
/** Make `fd`, a descriptor of the caller's that `fd_alloc` reserved and
 * gave no type, a close-on-exec sync file of `fence` with a reference of
 * its own, so that a call reserves the descriptor before the work whose
 * fence it gives: 0, or -EINVAL or -ENOMEM with the slot left reserved. */
int32_t sync_file_install(int32_t fd, struct dma_fence *fence, const char *name);
/** The earliest deadline requested for a fence, or INT64_MAX. */
int64_t dma_fence_deadline(struct dma_fence *fence);
/** Record a deadline hint on every leaf; no wait or signal is implied. */
void dma_fence_set_deadline(struct dma_fence *fence, int64_t deadline);

/*
 * A point of a timeline, Linux's dma_fence_chain: it signals once its own
 * fence and the fences of every earlier point have signalled, and carries
 * no error of theirs. `prev` is the earlier point, a plain fence or NULL;
 * `prev_seqno` is the earlier point's seqno when this point continues its
 * context, else 0, and a point that does not come after its predecessor
 * gets a context of its own and the predecessor's seqno. The chain holds
 * `prev` and `fence`; a walk drops the earlier points whose fences have
 * signalled.
 */
struct dma_fence_chain;
struct dma_fence_chain_cb {
    struct dma_fence_cb cb;
    struct dma_fence_chain *chain;
};
struct dma_fence_chain {
    struct dma_fence base;
    /* The earlier point, with a reference; read and swapped only under
     * fence.c's chain lock, since a walk unlinks signalled points. */
    struct dma_fence *prev;
    uint64_t prev_seqno;
    struct dma_fence *fence;
    /* The fence the callback waits on, with a reference, or NULL. */
    struct dma_fence *armed;
    struct dma_fence_chain_cb cb;
};

/** A zeroed chain record, or NULL; free an unused one with kfree. */
struct dma_fence_chain *dma_fence_chain_alloc(void);
/** Make `chain` point `seqno` of the fence `fence` after `prev`, taking
 * over the caller's references to both; `prev` may be NULL. The chain has
 * one reference, the caller's. */
void dma_fence_chain_init(struct dma_fence_chain *chain, struct dma_fence *prev,
                          struct dma_fence *fence, uint64_t seqno);
/** The chain record of `fence`, or NULL for any other fence. */
struct dma_fence_chain *to_dma_fence_chain(struct dma_fence *fence);
/** The earliest point before `fence` whose fence has not signalled, or a
 * plain fence before it that has not, with a reference, or NULL; dropping
 * the points between, and the caller's reference to `fence`. */
struct dma_fence *dma_fence_chain_walk(struct dma_fence *fence);
/** Replace `*fence`, a chain's newest point, with the point that covers
 * `seqno`, the newest whose predecessor in its context is below it, or
 * with NULL when every point up to it has signalled; 0 leaves `*fence` as
 * it is. Returns 0, or -EINVAL when `*fence` is no chain or has not reached
 * `seqno`, leaving `*fence` to the caller. */
int32_t dma_fence_chain_find_seqno(struct dma_fence **fence, uint64_t seqno);
/** A reference to the shared fence that signalled at its first use. */
struct dma_fence *dma_fence_get_stub(void);

#endif
