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
};

/* Each exported buffer holds its exporter until the last descriptor or
 * mapping releases it. An external reservation must live that long too. */
struct dma_buf_poll_cb {
    struct dma_fence_cb cb;
    struct dma_buf *buf;
};

struct dma_buf {
    struct anon_obj anon;
    uint32_t refs;
    uint64_t size, ino;
    const struct dma_buf_ops *ops;
    void *priv;
    struct dma_resv *resv;
    struct dma_resv own;
    wait_queue_t wq;
    struct dma_fence *poll_fences[2];
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
 * Return the descriptor or -errno; retain the caller's reference. */
int32_t dma_buf_fd(struct dma_buf *buf, uint32_t flags);
/** Get a referenced buffer from the caller's descriptor, or NULL. */
struct dma_buf *dma_buf_get(int32_t fd);
/** Release every reservation slot and its storage; big kernel lock held. */
void dma_resv_fini(struct dma_resv *resv);
/** Add a read fence, or replace all slots for a write. 0 or -ENOMEM.
 * The reservation takes its own reference; does not sleep. */
int32_t dma_resv_add_fence(struct dma_resv *resv, struct dma_fence *fence, int write);
/** Snapshot the write fence and, for write access, the readers too.
 * Return a referenced fence (signalled for no work), or NULL on allocation
 * failure. Does not sleep; the caller holds the big kernel lock. */
struct dma_fence *dma_resv_snapshot(struct dma_resv *resv, int write);
/** Merge fences into one referenced all-of fence, or NULL on allocation
 * failure. Inputs are borrowed; nested arrays are flattened. */
struct dma_fence *dma_fence_merge(struct dma_fence **fences, uint32_t count);
/** Number of leaf fences, and the borrowed leaf at index, or NULL. */
uint32_t dma_fence_count(struct dma_fence *fence);
struct dma_fence *dma_fence_at(struct dma_fence *fence, uint32_t index);
/** Status: 0 active, 1 completed, or the negative producer error. */
int32_t dma_fence_status(struct dma_fence *fence);
/** CLOCK_MONOTONIC in nanoseconds, under the big kernel lock. */
int64_t dma_fence_now(void);
/** Install a close-on-exec sync file with its own fence reference.
 * Return its descriptor or -errno. name may be NULL. */
int32_t sync_file_create(struct dma_fence *fence, const char *name);
/** The earliest deadline requested for a fence, or INT64_MAX. */
int64_t dma_fence_deadline(struct dma_fence *fence);
/** Record a deadline hint on every leaf; no wait or signal is implied. */
void dma_fence_set_deadline(struct dma_fence *fence, int64_t deadline);

#endif
