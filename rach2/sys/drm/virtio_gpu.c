/*
 * virtio_gpu.c: the virtio GPU display and command transport, Linux 6.12's
 * virtio_gpu:
 * fenced commands on its control and cursor queues, 2D resources over
 * shmem buffer objects, a CRTC with a primary and a cursor plane and a
 * virtual connector for each scanout, modes from the device's EDID, display
 * changes as hotplug, and the render node with virgl contexts and buffers.
 */

#include "virtio_gpu.h"
#include "../dmabuf/dmabuf.h"

#include "../arch/arch.h"
#include "../klog.h"
#include "../lib/kdef.h"
#include "../lib/kerrno.h"
#include "../lib/kirq.h"
#include "../lib/kstring.h"
#include "../mm/kheap.h"
#include "../mm/usercopy.h"
#include "../proc/process.h"
#include "../proc/sched.h"
#include "../udelay.h"

/* Linux's bounds of a scanout's size, and the size it gives the first
 * scanout until the device answers (virtgpu_display.c). */
#define XRES_MIN 32
#define YRES_MIN 32
#define XRES_DEF 1024
#define YRES_DEF 768
#define XRES_MAX 8192
#define YRES_MAX 8192

/* How long an answer is waited for, Linux's five seconds for the displays
 * at probe, here for every command a caller waits on; the step between two
 * polls of a queue while a waiter sleeps; and the step of a waiter that
 * cannot sleep, before the scheduler runs. */
#define VGPU_WAIT_NS 5000000000ll
#define VGPU_POLL_NS 10000000ll
#define VGPU_SPIN_US 100u

/* VIRTGPU_WAIT's bound, Linux's 15 seconds. */
#define VGPU_IOCTL_WAIT_NS 15000000000ll

/* The PCI class of a VGA controller, which virtio-vga has. */
#define PCI_CLASS_DISPLAY 0x03
#define PCI_SUBCLASS_VGA  0x00

#define VGPU_DEVICES 4

/* The devices bound, which the worker serves. */
static struct virtio_gpu_device *vgpu_devices[VGPU_DEVICES];

/* The worker, which rereads the displays after a configuration interrupt
 * and sends the hotplug; made at the first open of a node, once init has
 * its pid. */
static process_t *vgpu_thread;
static wait_queue_t vgpu_wake;
static int vgpu_wake_ready;

/** The device of `dev`, in which it is embedded. */
#define to_vg(dev) \
    ((struct virtio_gpu_device *)(void *)((char *)(dev) - offsetof(struct virtio_gpu_device, drm)))

/** The output a CRTC, plane or connector belongs to. */
#define output_of(p, member) \
    ((struct virtio_gpu_output *)(void *)((char *)(p) - offsetof(struct virtio_gpu_output, member)))

static const struct drm_gem_object_funcs vgpu_obj_funcs;

/** The virtio-gpu object `obj` is, of shmem pages or of host memory alone,
 * or NULL for one of another kind, such as an import through the shmem
 * helper. */
struct virtio_gpu_object *vgpu_bo(struct drm_gem_object *obj)
{
    return obj && (obj->funcs == &vgpu_obj_funcs || obj->funcs == &vgpu_host_funcs)
               ? (struct virtio_gpu_object *)obj
               : NULL;
}

/** Return the driver record embedded around dev; dev belongs to this driver. */
struct virtio_gpu_device *vgpu_device(struct drm_device *dev)
{
    return to_vg(dev);
}

/* ------------------------------------------------------------------------
 * Commands
 */

/**
 * A new command of `type` and `req_len` bytes for queue `vq`, zeroed, with
 * a fence, not yet started, on the control queue: NULL with no memory.
 * Process context.
 */
struct virtio_gpu_cmd *vgpu_cmd_new(struct virtio_gpu_device *vg, struct virtqueue *vq,
                                    uint32_t type, uint32_t req_len)
{
    struct virtio_gpu_cmd *cmd = (struct virtio_gpu_cmd *)kzalloc(sizeof(*cmd));
    uint32_t flags;

    if (!cmd) {
        return NULL;
    }
    cmd->page = (uint8_t *)virtio_dma_alloc(vg->vdev, PAGE_SIZE, &cmd->addr);
    if (vq == vg->ctrlq) {
        cmd->fence = (struct dma_fence *)kzalloc(sizeof(*cmd->fence));
    }
    if (!cmd->page || (vq == vg->ctrlq && !cmd->fence)) {
        if (cmd->page) {
            virtio_dma_free(vg->vdev, cmd->page, PAGE_SIZE);
        }
        kfree(cmd->fence);
        kfree(cmd);
        return NULL;
    }
    cmd->segments = cmd->sg;
    cmd->nin = 1;
    cmd->vg = vg;
    cmd->vq = vq;
    ((struct virtio_gpu_ctrl_hdr *)(void *)cmd->page)->type = type;
    cmd->sg[0].addr = cmd->addr;
    cmd->sg[0].len = req_len;
    cmd->nout = 1;
    flags = spin_lock_irqsave(&vg->lock);
    cmd->live_next = vg->live;
    vg->live = cmd;
    spin_unlock_irqrestore(&vg->lock, flags);
    return cmd;
}

/** Free `cmd`, its pages and its hold on its fence. A fence never started,
 * which has no reference yet, is freed. Process context. */
void vgpu_cmd_free(struct virtio_gpu_cmd *cmd)
{
    struct virtio_gpu_device *vg = cmd->vg;
    struct virtio_gpu_cmd **p;
    uint32_t flags, i;

    flags = spin_lock_irqsave(&vg->lock);
    for (p = &vg->live; *p; p = &(*p)->live_next) {
        if (*p == cmd) {
            *p = cmd->live_next;
            break;
        }
    }
    spin_unlock_irqrestore(&vg->lock, flags);
    for (i = 0; i < cmd->nextra; i++) {
        virtio_dma_free(vg->vdev, cmd->extra[i], PAGE_SIZE);
    }
    for (i = 0; i < cmd->npayload; i++) {
        virtio_dma_free(vg->vdev, cmd->payload[i].page, PAGE_SIZE);
    }
    kfree(cmd->payload);
    if (cmd->segments != cmd->sg) {
        kfree(cmd->segments);
    }
    virtio_dma_free(vg->vdev, cmd->page, PAGE_SIZE);
    if (cmd->fence && cmd->prepared) {
        dma_fence_put(cmd->fence);
    } else {
        kfree(cmd->fence);
    }
    kfree(cmd);
}

/** The request of `cmd`, at the start of its page. */
#define cmd_req(cmd, type) ((type *)(void *)(cmd)->page)

/** Put `cmd` on the done list for the reaper. Under the lock. */
static void done_add(struct virtio_gpu_device *vg, struct virtio_gpu_cmd *cmd)
{
    struct virtio_gpu_cmd **p = &vg->done_list;

    while (*p) {
        p = &(*p)->next;
    }
    cmd->next = NULL;
    *p = cmd;
}

/** Publish the commands held for queue `q` while it has room, in order,
 * and tell the device. Under the lock; a handler may call it. */
static void pending_push(struct virtio_gpu_device *vg, int q)
{
    struct virtqueue *vq = q == VIRTIO_GPU_QUEUE_CONTROL ? vg->ctrlq : vg->cursorq;
    struct virtio_gpu_cmd *cmd;
    int added = 0;

    while ((cmd = vg->pending[q]) != NULL) {
        if (virtqueue_add(vq, cmd->segments, cmd->nout, cmd->nin, cmd) != 0) {
            break;
        }
        vg->pending[q] = cmd->next;
        cmd->next = NULL;
        added = 1;
    }
    if (!vg->pending[q]) {
        vg->pending_tail[q] = NULL;
    }
    if (added) {
        virtqueue_kick(vq);
    }
}

/**
 * Start `cmd`: number its fence, the next of the device's timeline, in the
 * order the commands reach the queue, and publish it, or hold it behind the
 * commands already held until answers make room. On a dead device it is
 * answered at once with no answer, its fence failing with -ENODEV.
 */
static void cmd_submit(struct virtio_gpu_cmd *cmd)
{
    struct virtio_gpu_device *vg = cmd->vg;
    int q = cmd->vq == vg->ctrlq ? VIRTIO_GPU_QUEUE_CONTROL : VIRTIO_GPU_QUEUE_CURSOR;
    uint32_t flags;
    int dead;

    if (!cmd->response_size) {
        cmd->segments[cmd->nout].addr = cmd->addr + VIRTIO_GPU_RESP_OFF;
        cmd->segments[cmd->nout].len = PAGE_SIZE - VIRTIO_GPU_RESP_OFF;
    }
    vgpu_cmd_prepare(cmd);
    flags = spin_lock_irqsave(&vg->lock);
    cmd->queued = 1;
    vg->commands++;
    dead = vg->dead;
    if (dead) {
        cmd->done = 1;
        if (!cmd->waited) {
            done_add(vg, cmd);
        }
    } else {
        cmd->next = NULL;
        if (vg->pending_tail[q]) {
            vg->pending_tail[q]->next = cmd;
        } else {
            vg->pending[q] = cmd;
        }
        vg->pending_tail[q] = cmd;
        pending_push(vg, q);
    }
    spin_unlock_irqrestore(&vg->lock, flags);
    if (dead && cmd->fence) {
        dma_fence_set_error(cmd->fence, -ENODEV);
        dma_fence_signal(cmd->fence);
    }
}

/**
 * A queue's `done`: the device answered `cmd` with `len` bytes. The fence
 * signals first, failing with -EIO on an error answer, and with it every
 * earlier fence of its fence context, whose order the device keeps, as
 * Linux's virtio_gpu_fence_event_process does, while the command is still
 * neither its waiter's to free nor on the done list; then it is marked
 * done, and the commands held for the queue go in. Handler context.
 */
static void cmd_done(struct virtqueue *vq, void *token, uint32_t len)
{
    struct virtio_gpu_cmd *cmd = (struct virtio_gpu_cmd *)token;
    struct virtio_gpu_device *vg = cmd->vg;
    const struct virtio_gpu_ctrl_hdr *resp =
        (const struct virtio_gpu_ctrl_hdr *)(void *)(cmd->response_size
                                                         ? cmd->payload[0].page
                                                         : cmd->page + VIRTIO_GPU_RESP_OFF);
    uint32_t type = len >= sizeof(*resp) ? resp->type : 0;
    int ok = type >= VIRTIO_GPU_RESP_OK_NODATA && type < VIRTIO_GPU_RESP_ERR_UNSPEC;
    uint32_t flags;
    struct virtio_gpu_cmd *prior;
    int valid_fence =
        !cmd->fence || (len >= sizeof(*resp) && (resp->flags & VIRTIO_GPU_FLAG_FENCE) &&
                        resp->fence_id == cmd->fence->seqno);

    if (!valid_fence) {
        ok = 0;
        type = VIRTIO_GPU_RESP_ERR_UNSPEC;
    }
    cmd->response_len = len;
    if (cmd->fence) {
        if (!ok) {
            dma_fence_set_error(cmd->fence, -EIO);
        }
        if (!valid_fence) {
            dma_fence_signal(cmd->fence);
        } else {
            for (prior = vg->live; prior; prior = prior->live_next) {
                if (prior->queued && prior->fence && prior->fence->context == cmd->fence->context &&
                    prior->fence->seqno <= cmd->fence->seqno) {
                    dma_fence_signal(prior->fence);
                }
            }
        }
    }
    flags = spin_lock_irqsave(&vg->lock);
    cmd->resp_type = type;
    cmd->done = 1;
    if (!ok) {
        vg->errors++;
    }
    if (!cmd->waited) {
        done_add(vg, cmd);
    }
    pending_push(vg, vq == vg->ctrlq ? VIRTIO_GPU_QUEUE_CONTROL : VIRTIO_GPU_QUEUE_CURSOR);
    spin_unlock_irqrestore(&vg->lock, flags);
    if (vgpu_wake_ready) {
        wq_wake_all(&vgpu_wake);
    }
}

/**
 * Free every command answered that nobody waits on, after its `complete`,
 * logging an error answer. Both queues are reaped first, which answers the
 * commands of a device without interrupts. Process context.
 */
void vgpu_reap(struct virtio_gpu_device *vg)
{
    struct virtio_gpu_cmd *list, *cmd;
    uint32_t flags;

    drm_dev_get(&vg->drm);
    if (!vg->dead) {
        (void)virtqueue_reap(vg->ctrlq);
        (void)virtqueue_reap(vg->cursorq);
    }
    flags = spin_lock_irqsave(&vg->lock);
    list = vg->done_list;
    vg->done_list = NULL;
    spin_unlock_irqrestore(&vg->lock, flags);
    while ((cmd = list) != NULL) {
        list = cmd->next;
        if (cmd->resp_type >= VIRTIO_GPU_RESP_ERR_UNSPEC) {
            klog(KLOG_WARN, "virtio_gpu", "command 0x%x: error 0x%x",
                 cmd_req(cmd, struct virtio_gpu_ctrl_hdr)->type, cmd->resp_type);
        }
        if (cmd->complete) {
            cmd->complete(cmd);
        }
        vgpu_cmd_free(cmd);
    }
    drm_dev_put(&vg->drm);
}

/** Whether the caller may sleep: a process runs it and the scheduler is
 * on. Before then, at boot, a waiter polls. */
static int can_sleep(void)
{
    return sched_is_enabled() && current_proc != NULL;
}

/** Whether `cmd` has been answered, read under the lock. */
static int cmd_is_done(struct virtio_gpu_cmd *cmd)
{
    uint32_t flags = spin_lock_irqsave(&cmd->vg->lock);
    int done = cmd->done;

    spin_unlock_irqrestore(&cmd->vg->lock, flags);
    return done;
}

/** The error an answer of `type` means: 0 for success, -ENOMEM for the
 * host's lack of memory, -ENODEV for none from a dead device, -EIO for any
 * other. */
static int32_t answer_error(uint32_t type)
{
    if (type >= VIRTIO_GPU_RESP_OK_NODATA && type < VIRTIO_GPU_RESP_ERR_UNSPEC) {
        return 0;
    }
    if (type == VIRTIO_GPU_RESP_ERR_OUT_OF_MEMORY) {
        return -ENOMEM;
    }
    return type ? -EIO : -ENODEV;
}

/**
 * Start `cmd`, wait for its answer, copy up to `out_len` bytes of the
 * answer to `out` (NULL for none) and free it. The wait sleeps on the
 * command's fence, polling the queue every 10 ms in case the device has no
 * interrupt, or spins before the scheduler runs. Returns the answer's error
 * (`answer_error`), or -ETIMEDOUT when none came in five seconds; the
 * command is then left to the reaper, for the answer that may yet come.
 * A command with response pages stays with the caller after completion.
 * May sleep.
 */
int32_t vgpu_cmd_send_wait(struct virtio_gpu_cmd *cmd, void *out, uint32_t out_len)
{
    struct virtio_gpu_device *vg = cmd->vg;
    int64_t deadline = dma_fence_now() + VGPU_WAIT_NS, next;
    uint32_t spins = 0, flags, type = cmd_req(cmd, struct virtio_gpu_ctrl_hdr)->type;
    int32_t rc;

    cmd->waited = 1;
    cmd_submit(cmd);
    for (;;) {
        if (!vg->dead) {
            (void)virtqueue_reap(cmd->vq);
        }
        if (cmd_is_done(cmd)) {
            break;
        }
        if (can_sleep() && cmd->fence) {
            if (dma_fence_now() >= deadline) {
                break;
            }
            next = dma_fence_now() + VGPU_POLL_NS;
            (void)dma_fence_wait_until(cmd->fence, 0, next < deadline ? next : deadline);
        } else {
            if (spins++ >= (uint32_t)(VGPU_WAIT_NS / 1000 / VGPU_SPIN_US)) {
                break;
            }
            k_udelay(VGPU_SPIN_US);
        }
    }
    flags = spin_lock_irqsave(&vg->lock);
    if (!cmd->done) {
        cmd->waited = 0;
        spin_unlock_irqrestore(&vg->lock, flags);
        klog(KLOG_ERR, "virtio_gpu", "command 0x%x: no answer in 5 s", type);
        return -ETIMEDOUT;
    }
    spin_unlock_irqrestore(&vg->lock, flags);
    rc = answer_error(cmd->resp_type);
    if (rc != 0 && cmd->resp_type) {
        klog(KLOG_WARN, "virtio_gpu", "command 0x%x: error 0x%x", type, cmd->resp_type);
    }
    if (rc == 0 && out && out_len > cmd->response_len) {
        rc = -EIO;
    }
    if (rc == 0 && out) {
        k_memcpy(out, cmd->page + VIRTIO_GPU_RESP_OFF, out_len);
    }
    if (!cmd->response_size) {
        vgpu_cmd_free(cmd);
    }
    return rc;
}

/** Start `cmd` with nobody waiting; the reaper runs `complete`, if any, with
 * `arg` in the command, and frees it. */
void vgpu_cmd_send(struct virtio_gpu_cmd *cmd, void (*complete)(struct virtio_gpu_cmd *), void *arg)
{
    cmd->complete = complete;
    cmd->arg = arg;
    cmd_submit(cmd);
}

/* ---- The commands ---- */

/** The device's rectangle of `w` by `h` pixels at `x`, `y`. */
static struct virtio_gpu_rect rect_of(uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{
    struct virtio_gpu_rect r;

    r.x = x;
    r.y = y;
    r.width = w;
    r.height = h;
    return r;
}

/** The device's rectangle of the core's `d`, whose corners are ordered. */
static struct virtio_gpu_rect rect_of_drm(const struct drm_rect *d)
{
    return rect_of((uint32_t)d->x1, (uint32_t)d->y1, (uint32_t)(d->x2 - d->x1),
                   (uint32_t)(d->y2 - d->y1));
}

/** GET_DISPLAY_INFO: each scanout's rectangle and whether it is enabled,
 * into the outputs. 0, or the command's error. May sleep. */
static int32_t vgpu_get_display_info(struct virtio_gpu_device *vg)
{
    struct virtio_gpu_resp_display_info *resp;
    struct virtio_gpu_cmd *cmd;
    uint32_t i;
    int32_t rc;

    resp = (struct virtio_gpu_resp_display_info *)kmalloc(sizeof(*resp));
    cmd = resp ? vgpu_cmd_new(vg, vg->ctrlq, VIRTIO_GPU_CMD_GET_DISPLAY_INFO,
                              sizeof(struct virtio_gpu_ctrl_hdr))
               : NULL;
    if (!cmd) {
        kfree(resp);
        return -ENOMEM;
    }
    rc = vgpu_cmd_send_wait(cmd, resp, sizeof(*resp));
    if (rc == 0 && resp->hdr.type == VIRTIO_GPU_RESP_OK_DISPLAY_INFO) {
        for (i = 0; i < vg->num_scanouts; i++) {
            vg->outputs[i].info = resp->pmodes[i];
            if (resp->pmodes[i].enabled) {
                klog(KLOG_INFO, "virtio_gpu", "display: scanout %u: %ux%u+%u+%u", i,
                     resp->pmodes[i].r.width, resp->pmodes[i].r.height, resp->pmodes[i].r.x,
                     resp->pmodes[i].r.y);
            } else {
                klog(KLOG_INFO, "virtio_gpu", "display: scanout %u: disabled", i);
            }
        }
    }
    kfree(resp);
    return rc;
}

/** GET_EDID of scanout `i` into its output, none when the device answers
 * with no EDID. 0, or the command's error. May sleep. */
static int32_t vgpu_get_edid(struct virtio_gpu_device *vg, uint32_t i)
{
    struct virtio_gpu_output *o = &vg->outputs[i];
    struct virtio_gpu_resp_edid *resp;
    struct virtio_gpu_cmd *cmd;
    int32_t rc;

    resp = (struct virtio_gpu_resp_edid *)kmalloc(sizeof(*resp));
    cmd = resp ? vgpu_cmd_new(vg, vg->ctrlq, VIRTIO_GPU_CMD_GET_EDID,
                              sizeof(struct virtio_gpu_cmd_get_edid))
               : NULL;
    if (!cmd) {
        kfree(resp);
        return -ENOMEM;
    }
    cmd_req(cmd, struct virtio_gpu_cmd_get_edid)->scanout = i;
    rc = vgpu_cmd_send_wait(cmd, resp, sizeof(*resp));
    o->edid_len = 0;
    if (rc == 0 && resp->hdr.type == VIRTIO_GPU_RESP_OK_EDID) {
        o->edid_len = resp->size <= sizeof(o->edid) ? resp->size : sizeof(o->edid);
        k_memcpy(o->edid, resp->edid, o->edid_len);
    }
    kfree(resp);
    return rc;
}

/** Read every scanout's EDID, when the device gives them, and then the
 * displays, as Linux asks for them at probe and on a display change. */
static void vgpu_read_displays(struct virtio_gpu_device *vg)
{
    uint32_t i;

    if (!vg->num_scanouts) {
        return;
    }
    for (i = 0; vg->has_edid && i < vg->num_scanouts; i++) {
        (void)vgpu_get_edid(vg, i);
    }
    (void)vgpu_get_display_info(vg);
}

/** SET_SCANOUT: scanout `scanout` shows rectangle `r` of resource `res`, or
 * nothing for 0. Not waited for. */
static void vgpu_set_scanout(struct virtio_gpu_device *vg, uint32_t scanout, uint32_t res,
                             struct virtio_gpu_rect r)
{
    struct virtio_gpu_cmd *cmd = vgpu_cmd_new(vg, vg->ctrlq, VIRTIO_GPU_CMD_SET_SCANOUT,
                                              sizeof(struct virtio_gpu_set_scanout));
    struct virtio_gpu_set_scanout *req;

    if (!cmd) {
        klog(KLOG_WARN, "virtio_gpu", "scanout %u: no memory for SET_SCANOUT", scanout);
        return;
    }
    req = cmd_req(cmd, struct virtio_gpu_set_scanout);
    req->scanout_id = scanout;
    req->resource_id = res;
    req->r = r;
    vgpu_cmd_send(cmd, NULL, NULL);
}

/**
 * SET_SCANOUT_BLOB, Linux's virtio_gpu_cmd_set_scanout_blob: scanout
 * `scanout` shows rectangle `r` of the blob `bo` read as the framebuffer
 * `fb`, XRGB8888 alone, which the device calls B8G8R8X8. Not waited for.
 */
static void vgpu_set_scanout_blob(struct virtio_gpu_device *vg, uint32_t scanout,
                                  struct virtio_gpu_object *bo, const struct drm_framebuffer *fb,
                                  struct virtio_gpu_rect r)
{
    struct virtio_gpu_cmd *cmd = vgpu_cmd_new(vg, vg->ctrlq, VIRTIO_GPU_CMD_SET_SCANOUT_BLOB,
                                              sizeof(struct virtio_gpu_set_scanout_blob));
    struct virtio_gpu_set_scanout_blob *req;
    int i;

    if (!cmd) {
        klog(KLOG_WARN, "virtio_gpu", "scanout %u: no memory for SET_SCANOUT_BLOB", scanout);
        return;
    }
    req = cmd_req(cmd, struct virtio_gpu_set_scanout_blob);
    req->scanout_id = scanout;
    req->resource_id = bo->res_id;
    req->format = VIRTIO_GPU_FORMAT_B8G8R8X8_UNORM;
    req->width = fb->width;
    req->height = fb->height;
    for (i = 0; i < 4; i++) {
        req->strides[i] = fb->pitches[i];
        req->offsets[i] = fb->offsets[i];
    }
    req->r = r;
    vgpu_cmd_send(cmd, NULL, NULL);
}

/** A TRANSFER_TO_HOST_2D of rectangle `r` of resource `res`, whose first
 * pixel lies `offset` bytes into its backing, not yet started; NULL with no
 * memory. */
static struct virtio_gpu_cmd *vgpu_transfer_cmd(struct virtio_gpu_device *vg, uint32_t res,
                                                uint64_t offset, struct virtio_gpu_rect r)
{
    struct virtio_gpu_cmd *cmd = vgpu_cmd_new(vg, vg->ctrlq, VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D,
                                              sizeof(struct virtio_gpu_transfer_to_host_2d));
    struct virtio_gpu_transfer_to_host_2d *req;

    if (cmd) {
        req = cmd_req(cmd, struct virtio_gpu_transfer_to_host_2d);
        req->resource_id = res;
        req->offset = offset;
        req->r = r;
    }
    return cmd;
}

/** RESOURCE_FLUSH of rectangle `r` of resource `res` to the scanouts that
 * show it, waited for, so that a commit's event follows the picture. 0 or
 * the command's error. May sleep. */
static int32_t vgpu_flush(struct virtio_gpu_device *vg, uint32_t res, struct virtio_gpu_rect r)
{
    struct virtio_gpu_cmd *cmd = vgpu_cmd_new(vg, vg->ctrlq, VIRTIO_GPU_CMD_RESOURCE_FLUSH,
                                              sizeof(struct virtio_gpu_resource_flush));
    struct virtio_gpu_resource_flush *req;

    if (!cmd) {
        return -ENOMEM;
    }
    req = cmd_req(cmd, struct virtio_gpu_resource_flush);
    req->resource_id = res;
    req->r = r;
    return vgpu_cmd_send_wait(cmd, NULL, 0);
}

/** The memory entry slot `n` of a request of `req` bytes: after the
 * request in the command's page for the first ones, then in the extra
 * pages, the next of which is taken when `n` first reaches it. NULL with no
 * memory or past VIRTIO_GPU_CMD_EXTRA pages. */
static struct virtio_gpu_mem_entry *entry_slot(struct virtio_gpu_cmd *cmd, uint32_t req, uint32_t n)
{
    struct virtio_gpu_mem_entry *inl = (struct virtio_gpu_mem_entry *)(void *)(cmd->page + req);
    uint32_t k;

    if (n < VIRTIO_GPU_INLINE_ENTS(req)) {
        return &inl[n];
    }
    n -= VIRTIO_GPU_INLINE_ENTS(req);
    k = n / (uint32_t)VIRTIO_GPU_PAGE_ENTS;
    if (k >= VIRTIO_GPU_CMD_EXTRA) {
        return NULL;
    }
    if (k == cmd->nextra) {
        cmd->extra[k] = virtio_dma_alloc(cmd->vg->vdev, PAGE_SIZE, &cmd->extra_addr[k]);
        if (!cmd->extra[k]) {
            return NULL;
        }
        cmd->nextra++;
    }
    return (struct virtio_gpu_mem_entry *)cmd->extra[k] + n % (uint32_t)VIRTIO_GPU_PAGE_ENTS;
}

/**
 * Follow the request of `cmd`, ATTACH_BACKING or CREATE_BLOB, with the
 * memory entries of `bo`'s pages, each page given now, runs of adjacent
 * frames as one entry, and set its segments; their count goes in *n. 0;
 * -ENOMEM, or -E2BIG for a chain of more segments than the queue takes.
 */
int32_t vgpu_backing_entries(struct virtio_gpu_cmd *cmd, struct virtio_gpu_object *bo, uint32_t *n)
{
    struct virtio_gpu_mem_entry *e = NULL;
    uint32_t pages = (uint32_t)(bo->shmem.base.size >> PAGE_SHIFT), p, k, left;
    uint32_t req = cmd->sg[0].len, inl = VIRTIO_GPU_INLINE_ENTS(req);
    uintptr_t phys;

    *n = 0;
    for (p = 0; p < pages; p++) {
        phys = drm_gem_shmem_page(&bo->shmem, p);
        if (!phys) {
            return -ENOMEM;
        }
        if (e && e->addr + e->length == (uint64_t)phys && e->length <= 0xFFFFFFFFu - PAGE_SIZE) {
            e->length += PAGE_SIZE;
            continue;
        }
        e = entry_slot(cmd, req, *n);
        if (!e) {
            return *n >= inl + VIRTIO_GPU_CMD_EXTRA * VIRTIO_GPU_PAGE_ENTS ? -E2BIG : -ENOMEM;
        }
        e->addr = (uint64_t)phys;
        e->length = PAGE_SIZE;
        e->padding = 0;
        (*n)++;
    }
    if (cmd->nextra + 2 > (uint32_t)cmd->vq->size) {
        return -E2BIG;
    }
    k = *n < inl ? *n : inl;
    cmd->sg[0].len += k * (uint32_t)sizeof(struct virtio_gpu_mem_entry);
    left = *n - k;
    for (k = 0; k < cmd->nextra; k++) {
        uint32_t here = left < VIRTIO_GPU_PAGE_ENTS ? left : (uint32_t)VIRTIO_GPU_PAGE_ENTS;

        cmd->sg[1 + k].addr = cmd->extra_addr[k];
        cmd->sg[1 + k].len = here * (uint32_t)sizeof(struct virtio_gpu_mem_entry);
        left -= here;
    }
    cmd->nout = 1 + cmd->nextra;
    return 0;
}

/**
 * An ATTACH_BACKING of `bo`'s pages to its resource `res`, not yet started
 * (`vgpu_backing_entries`). NULL with *err -ENOMEM or -E2BIG.
 */
static struct virtio_gpu_cmd *vgpu_attach_cmd(struct virtio_gpu_device *vg,
                                              struct virtio_gpu_object *bo, uint32_t res,
                                              int32_t *err)
{
    struct virtio_gpu_cmd *cmd = vgpu_cmd_new(vg, vg->ctrlq, VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING,
                                              sizeof(struct virtio_gpu_resource_attach_backing));
    struct virtio_gpu_resource_attach_backing *req;
    uint32_t n;

    *err = -ENOMEM;
    if (!cmd) {
        return NULL;
    }
    *err = vgpu_backing_entries(cmd, bo, &n);
    if (*err != 0) {
        vgpu_cmd_free(cmd);
        return NULL;
    }
    req = cmd_req(cmd, struct virtio_gpu_resource_attach_backing);
    req->resource_id = res;
    req->nr_entries = n;
    return cmd;
}

/* ------------------------------------------------------------------------
 * Resource ids
 */

/** The lowest free resource id from 1, now taken, or 0 when all are. */
uint32_t vgpu_res_id_get(struct virtio_gpu_device *vg)
{
    uint32_t i;

    for (i = 0; i < VIRTIO_GPU_MAX_RESOURCES / 32; i++) {
        if (vg->res_ids[i] != 0xFFFFFFFFu) {
            uint32_t b = 0;

            while (vg->res_ids[i] & (1u << b)) {
                b++;
            }
            vg->res_ids[i] |= 1u << b;
            return i * 32 + b + 1;
        }
    }
    return 0;
}

/** Give back resource id `id`. */
void vgpu_res_id_put(struct virtio_gpu_device *vg, uint32_t id)
{
    if (id >= 1 && id <= VIRTIO_GPU_MAX_RESOURCES) {
        vg->res_ids[(id - 1) / 32] &= ~(1u << ((id - 1) % 32));
    }
}

/* ------------------------------------------------------------------------
 * Buffer objects
 */

/** A mapping's page of an object: its frame, given on the first touch;
 * none past its end or once its device is unplugged. */
static int obj_vm_page(void *obj, uint32_t pgoff, uintptr_t *phys)
{
    struct virtio_gpu_object *bo = (struct virtio_gpu_object *)obj;
    uintptr_t p;

    if (bo->shmem.base.dev->unplugged) {
        return -1;
    }
    p = drm_gem_shmem_page(&bo->shmem, pgoff);
    if (!p) {
        return -1;
    }
    *phys = p;
    return 0;
}

/** A mapping's release of its reference to the object. */
static void obj_vm_put(void *obj)
{
    drm_gem_object_put((struct drm_gem_object *)obj);
}

static const struct vm_device_ops vgpu_vm_ops = {obj_vm_page, obj_vm_put};

/** The `mmap` of an object: a device region over its frames, cached, as
 * Linux maps virtio-gpu's shmem objects. */
static int32_t obj_mmap(struct drm_gem_object *obj, vma_t *vma)
{
    return vm_device_map(vma, &vgpu_vm_ops, obj, 0u);
}

/** An object's frame for a PRIME importer. */
static uintptr_t obj_page(struct drm_gem_object *obj, uint32_t pgoff)
{
    if (obj->dev->unplugged) {
        return 0;
    }
    return drm_gem_shmem_page(&((struct virtio_gpu_object *)obj)->shmem, pgoff);
}

/** Free an object's frames, its piece of host-visible memory, its resource
 * id and its record. */
static void obj_release(struct virtio_gpu_object *bo)
{
    struct virtio_gpu_object **p;
    struct virtio_gpu_device *vg = to_vg(bo->shmem.base.dev);

    vgpu_blob_release(vg, bo);
    for (p = &vg->resources; *p && *p != bo; p = &(*p)->next) {
    }
    if (*p) {
        *p = bo->next;
    }
    if (bo->res_id) {
        vgpu_res_id_put(to_vg(bo->shmem.base.dev), bo->res_id);
    }
    shmem_free(&bo->shmem.pages);
    kfree(bo);
}

/** The host has let go of an object's resource: free the object, and drop
 * the hold its UNREF kept on the device. */
static void unref_done(struct virtio_gpu_cmd *cmd)
{
    struct virtio_gpu_object *bo = (struct virtio_gpu_object *)cmd->arg;
    struct drm_device *dev = bo->shmem.base.dev;

    if (cmd->resp_type != VIRTIO_GPU_RESP_OK_NODATA && !cmd->vg->dead) {
        drm_dev_unplug(dev);
    }
    obj_release(bo);
    drm_dev_put(dev);
}

/**
 * The `free` of an object, after its last reference: UNMAP_BLOB for a blob
 * placed in host-visible memory, then RESOURCE_UNREF, which also detaches
 * the backing, and the frames and the piece freed once the host has
 * answered, since until then it may use them, as Linux's
 * virtio_gpu_free_object and virtio_gpu_vram_free do. Without memory for
 * the commands, they are freed only after an unplug resets the device and
 * revokes mappings.
 */
void vgpu_obj_free(struct drm_gem_object *obj)
{
    struct virtio_gpu_object *bo = (struct virtio_gpu_object *)obj;
    struct virtio_gpu_device *vg = to_vg(obj->dev);
    struct virtio_gpu_cmd *cmd;

    vgpu_resource_detach(vg, bo);
    if (bo->res_id && !vg->dead) {
        cmd = vgpu_cmd_new(vg, vg->ctrlq, VIRTIO_GPU_CMD_RESOURCE_UNREF,
                           sizeof(struct virtio_gpu_resource_unref));
        if (cmd && bo->host_visible && vgpu_blob_unmap(vg, bo) != 0) {
            vgpu_cmd_free(cmd);
            cmd = NULL;
        }
        if (cmd) {
            cmd_req(cmd, struct virtio_gpu_resource_unref)->resource_id = bo->res_id;
            drm_dev_get(obj->dev);
            vgpu_cmd_send(cmd, unref_done, bo);
            return;
        }
        klog(KLOG_WARN, "virtio_gpu", "resource %u: no memory for UNREF; reset", bo->res_id);
        drm_dev_unplug(&vg->drm);
    }
    obj_release(bo);
}

static const struct drm_gem_object_funcs vgpu_obj_funcs = {vgpu_obj_free, obj_mmap, NULL, NULL,
                                                           obj_page,      NULL,     NULL};

/**
 * Make `bo`, zeroed, an object of `vg` of `size` bytes, a whole number of
 * pages, on the device's list: of shmem pages, given as they are first
 * asked for, or with `host` of host memory alone, which has none. One
 * reference, the caller's; no resource id yet.
 */
void vgpu_obj_init(struct virtio_gpu_device *vg, struct virtio_gpu_object *bo, uint64_t size,
                   int host)
{
    drm_gem_object_init(&vg->drm, &bo->shmem.base, size, host ? &vgpu_host_funcs : &vgpu_obj_funcs);
    bo->next = vg->resources;
    vg->resources = bo;
    if (!host) {
        bo->shmem.pages.size = size;
    }
}

/**
 * A new object of `size` bytes with a 2D resource of `format` and
 * `width` by `height` pixels over its pages, which are given now: the
 * RESOURCE_CREATE_2D and the ATTACH_BACKING, whose answer is waited for. One
 * reference, the caller's; NULL with *err -ENODEV for a dead device, -EINVAL
 * for a size of 0 or past the shmem helper's reach, -ENOSPC when every
 * resource id is taken, -ENOMEM, -E2BIG, or the device's answer. May sleep.
 */
struct virtio_gpu_object *vgpu_obj_create(struct virtio_gpu_device *vg, uint64_t size,
                                          const struct drm_virtgpu_resource_create *params,
                                          int virgl, int32_t *err)
{
    struct virtio_gpu_resource_create_2d *req;
    struct virtio_gpu_cmd *create, *attach;
    struct virtio_gpu_object *bo;
    uint32_t id;
    uint32_t format = params->format, width = params->width, height = params->height;
    const struct drm_virtgpu_resource_create *r3d = virgl ? params : NULL;

    if (vg->dead) {
        *err = -ENODEV;
        return NULL;
    }
    size = (size + PAGE_SIZE - 1) & ~(uint64_t)(PAGE_SIZE - 1);
    if (size == 0 || size > SHMEM_MAX_SIZE) {
        *err = -EINVAL;
        return NULL;
    }
    bo = (struct virtio_gpu_object *)kzalloc(sizeof(*bo));
    if (!bo) {
        *err = -ENOMEM;
        return NULL;
    }
    vgpu_obj_init(vg, bo, size, 0);
    bo->format = format;
    bo->width = width;
    bo->height = height;
    id = vgpu_res_id_get(vg);
    if (!id) {
        *err = -ENOSPC;
        drm_gem_object_put(&bo->shmem.base);
        return NULL;
    }
    create = vgpu_cmd_new(
        vg, vg->ctrlq, r3d ? VIRTIO_GPU_CMD_RESOURCE_CREATE_3D : VIRTIO_GPU_CMD_RESOURCE_CREATE_2D,
        r3d ? sizeof(struct virtio_gpu_resource_create_3d) : sizeof(*req));
    attach = create ? vgpu_attach_cmd(vg, bo, id, err) : NULL;
    if (!attach) {
        if (create) {
            vgpu_cmd_free(create);
        } else {
            *err = -ENOMEM;
        }
        vgpu_res_id_put(vg, id);
        drm_gem_object_put(&bo->shmem.base);
        return NULL;
    }
    if (r3d) {
        struct virtio_gpu_resource_create_3d *r =
            cmd_req(create, struct virtio_gpu_resource_create_3d);
        r->resource_id = id;
        r->target = r3d->target;
        r->format = r3d->format;
        r->bind = r3d->bind;
        r->width = r3d->width;
        r->height = r3d->height;
        r->depth = r3d->depth;
        r->array_size = r3d->array_size;
        r->last_level = r3d->last_level;
        r->nr_samples = r3d->nr_samples;
        r->flags = r3d->flags;
    } else {
        req = cmd_req(create, struct virtio_gpu_resource_create_2d);
        req->resource_id = id;
        req->format = format;
        req->width = width;
        req->height = height;
    }
    *err = vgpu_cmd_send_wait(create, NULL, 0);
    if (*err != 0) {
        /* An error answer leaves the host without the id, which an UNREF
         * would name in vain; only one not answered yet may hold it. */
        vgpu_cmd_free(attach);
        if (*err == -ETIMEDOUT) {
            bo->res_id = id;
        } else {
            vgpu_res_id_put(vg, id);
        }
        drm_gem_object_put(&bo->shmem.base);
        return NULL;
    }
    /* create completed before backing becomes visible. */

    /* The host has the id from here on: a failure below is undone by the
     * object's UNREF. */
    bo->res_id = id;
    *err = vgpu_cmd_send_wait(attach, NULL, 0);
    if (*err != 0) {
        drm_gem_object_put(&bo->shmem.base);
        return NULL;
    }
    return bo;
}

/**
 * MODE_CREATE_DUMB, Linux's virtio_gpu_mode_dumb_create: a buffer of 32
 * bits a pixel, its pitch four bytes a pixel, with a host resource of
 * XRGB8888 (B8G8R8X8) of its size. EINVAL for another depth, or the
 * object's error. May sleep.
 */
static int32_t vgpu_dumb_create(struct drm_file *file, struct drm_device *dev,
                                struct drm_mode_create_dumb *args)
{
    struct virtio_gpu_object *bo;
    uint32_t pitch;
    struct drm_virtgpu_resource_create params;
    uint64_t size;
    int32_t rc;

    if (args->bpp != 32) {
        return -EINVAL;
    }
    pitch = args->width * 4;
    size = ((uint64_t)pitch * args->height + PAGE_SIZE - 1) & ~(uint64_t)(PAGE_SIZE - 1);
    k_memset(&params, 0, sizeof(params));
    params.format = VIRTIO_GPU_FORMAT_B8G8R8X8_UNORM;
    params.width = args->width;
    params.height = args->height;
    bo = vgpu_obj_create(to_vg(dev), size, &params, 0, &rc);
    if (!bo) {
        return rc;
    }
    bo->dumb = 1;
    rc = drm_gem_handle_create(file, &bo->shmem.base, &args->handle);
    drm_gem_object_put(&bo->shmem.base);
    if (rc == 0) {
        args->pitch = pitch;
        args->size = size;
    }
    return rc;
}

/* ------------------------------------------------------------------------
 * Display: planes, CRTCs and connectors
 */

static const uint32_t primary_formats[] = {DRM_FORMAT_XRGB8888};
static const uint32_t cursor_formats[] = {DRM_FORMAT_ARGB8888};

/** The output of CRTC `crtc`, or NULL. */
static struct virtio_gpu_output *crtc_output(struct drm_crtc *crtc)
{
    return crtc ? output_of(crtc, crtc) : NULL;
}

/**
 * A plane's check, Linux's virtio_gpu_plane_atomic_check: a plane that
 * shows a framebuffer brings its CRTC into the commit and lies within it
 * unscaled, the primary plane covering it, the cursor anywhere.
 */
static int32_t vgpu_plane_check(struct drm_plane *plane, struct drm_atomic_state *state)
{
    struct drm_plane_state *ps = drm_atomic_get_new_plane_state(state, plane);
    struct drm_crtc_state *cs;

    if (!ps->fb || !ps->crtc) {
        return 0;
    }
    cs = drm_atomic_get_crtc_state(state, ps->crtc);
    if (!cs) {
        return -ENOMEM;
    }
    return drm_atomic_helper_check_plane_state(ps, cs, DRM_PLANE_NO_SCALING, DRM_PLANE_NO_SCALING,
                                               plane->type == DRM_PLANE_TYPE_CURSOR, 1);
}

/** Whether two rectangles are the same. */
static int rect_equal(const struct drm_rect *a, const struct drm_rect *b)
{
    return a->x1 == b->x1 && a->y1 == b->y1 && a->x2 == b->x2 && a->y2 == b->y2;
}

/**
 * The part of a visible plane's framebuffer an update must copy and flush,
 * in whole pixels, Linux's drm_atomic_helper_damage_merged: the union of
 * the damage clips within the plane's source, or the whole source when the
 * framebuffer or the source changed or no clips came. 0, with nothing to
 * do, for a plane not shown or clips all outside the source.
 */
static int damage_merged(const struct drm_plane_state *old, const struct drm_plane_state *ps,
                         struct drm_rect *r)
{
    struct drm_rect src;
    const struct drm_mode_rect *clips;
    uint32_t n;

    if (!ps->fb || !ps->visible) {
        return 0;
    }
    src.x1 = ps->src.x1 >> 16;
    src.y1 = ps->src.y1 >> 16;
    src.x2 = (ps->src.x2 >> 16) + ((ps->src.x2 & 0xFFFF) != 0);
    src.y2 = (ps->src.y2 >> 16) + ((ps->src.y2 & 0xFFFF) != 0);
    if (old->fb != ps->fb || !rect_equal(&old->src, &ps->src) || !ps->fb_damage_clips) {
        *r = src;
        return 1;
    }
    r->x1 = r->y1 = 0x7FFFFFFF;
    r->x2 = r->y2 = 0;
    clips = (const struct drm_mode_rect *)ps->fb_damage_clips->data;
    for (n = ps->fb_damage_clips->length / sizeof(*clips); n > 0; n--, clips++) {
        int32_t x1 = clips->x1 > src.x1 ? clips->x1 : src.x1;
        int32_t y1 = clips->y1 > src.y1 ? clips->y1 : src.y1;
        int32_t x2 = clips->x2 < src.x2 ? clips->x2 : src.x2;
        int32_t y2 = clips->y2 < src.y2 ? clips->y2 : src.y2;

        if (x1 >= x2 || y1 >= y2) {
            continue;
        }
        r->x1 = x1 < r->x1 ? x1 : r->x1;
        r->y1 = y1 < r->y1 ? y1 : r->y1;
        r->x2 = x2 > r->x2 ? x2 : r->x2;
        r->y2 = y2 > r->y2 ? y2 : r->y2;
    }
    return r->x1 < r->x2 && r->y1 < r->y2;
}

/**
 * The primary plane's update, Linux's virtio_gpu_primary_plane_update: with
 * no framebuffer or the CRTC off, the scanout shows nothing; otherwise the
 * damaged rectangle of a dumb buffer goes to the host, the scanout is set
 * again, with SET_SCANOUT_BLOB for a blob, when the framebuffer, the source
 * or the mode changed, and the
 * rectangle is flushed to the screen. May sleep.
 */
static void primary_update(struct drm_plane *plane, struct drm_atomic_state *state)
{
    struct drm_plane_state *ps = plane->state;
    struct drm_plane_state *old = drm_atomic_get_old_plane_state(state, plane);
    struct virtio_gpu_output *o = crtc_output(ps->crtc ? ps->crtc : old->crtc);
    struct drm_crtc_state *cs;
    struct virtio_gpu_object *bo;
    struct virtio_gpu_device *vg;
    struct virtio_gpu_cmd *cmd;
    struct drm_framebuffer *fb = ps->fb;
    struct drm_rect r;
    uint32_t res;

    if (!o) {
        return;
    }
    vg = o->vg;
    if (!fb || !o->crtc.state->active) {
        vgpu_set_scanout(vg, o->index, 0, rect_of(0, 0, ps->src_w >> 16, ps->src_h >> 16));
        return;
    }
    if (!damage_merged(old, ps, &r)) {
        return;
    }
    bo = vgpu_bo(fb->obj[0]);
    res = bo ? bo->res_id : 0;
    if (bo && bo->dumb) {
        cmd = vgpu_transfer_cmd(vg, res,
                                (uint64_t)r.x1 * fb->info->cpp[0] + (uint64_t)r.y1 * fb->pitches[0],
                                rect_of_drm(&r));
        if (cmd) {
            vgpu_cmd_send(cmd, NULL, NULL);
        }
    }
    cs = drm_atomic_get_new_crtc_state(state, &o->crtc);
    if (fb != old->fb || ps->src_w != old->src_w || ps->src_h != old->src_h ||
        ps->src_x != old->src_x || ps->src_y != old->src_y ||
        (cs && drm_atomic_crtc_needs_modeset(cs))) {
        struct virtio_gpu_rect src =
            rect_of(ps->src_x >> 16, ps->src_y >> 16, ps->src_w >> 16, ps->src_h >> 16);

        if (bo && bo->blob_mem) {
            vgpu_set_scanout_blob(vg, o->index, bo, fb, src);
        } else {
            vgpu_set_scanout(vg, o->index, res, src);
        }
    }
    (void)vgpu_flush(vg, res, rect_of_drm(&r));
}

/**
 * The cursor plane's update, Linux's virtio_gpu_cursor_plane_update: a new
 * framebuffer's pixels go to the host, waited for, and UPDATE_CURSOR names
 * its resource and hotspot; an unchanged one only moves, with MOVE_CURSOR.
 * Both go on the cursor queue. May sleep.
 */
static void cursor_update(struct drm_plane *plane, struct drm_atomic_state *state)
{
    struct drm_plane_state *ps = plane->state;
    struct drm_plane_state *old = drm_atomic_get_old_plane_state(state, plane);
    struct virtio_gpu_output *o = crtc_output(ps->crtc ? ps->crtc : old->crtc);
    struct virtio_gpu_object *bo = ps->fb ? vgpu_bo(ps->fb->obj[0]) : NULL;
    struct virtio_gpu_update_cursor *req;
    struct virtio_gpu_device *vg;
    struct virtio_gpu_cmd *cmd;

    if (!o) {
        return;
    }
    vg = o->vg;
    if (bo && bo->dumb && ps->fb != old->fb) {
        cmd = vgpu_transfer_cmd(vg, bo->res_id, 0, rect_of(0, 0, ps->crtc_w, ps->crtc_h));
        if (cmd) {
            (void)vgpu_cmd_send_wait(cmd, NULL, 0);
        }
    }
    cmd =
        vgpu_cmd_new(vg, vg->cursorq,
                     ps->fb != old->fb ? VIRTIO_GPU_CMD_UPDATE_CURSOR : VIRTIO_GPU_CMD_MOVE_CURSOR,
                     sizeof(struct virtio_gpu_update_cursor));
    if (!cmd) {
        klog(KLOG_WARN, "virtio_gpu", "scanout %u: no memory for the cursor", o->index);
        return;
    }
    req = cmd_req(cmd, struct virtio_gpu_update_cursor);
    if (ps->fb != old->fb) {
        req->resource_id = bo ? bo->res_id : 0;
        req->hot_x = ps->fb ? (uint32_t)ps->hotspot_x : 0;
        req->hot_y = ps->fb ? (uint32_t)ps->hotspot_y : 0;
    }
    req->pos.scanout_id = o->index;
    req->pos.x = (uint32_t)ps->crtc_x;
    req->pos.y = (uint32_t)ps->crtc_y;
    vgpu_cmd_send(cmd, NULL, NULL);
}

/** A CRTC turned off: its scanout shows nothing. */
static void crtc_disable(struct drm_crtc *crtc, struct drm_atomic_state *state)
{
    struct virtio_gpu_output *o = crtc_output(crtc);

    (void)state;
    vgpu_set_scanout(o->vg, o->index, 0, rect_of(0, 0, 0, 0));
}

static const struct drm_plane_funcs primary_funcs = {vgpu_plane_check, primary_update, NULL};
static const struct drm_plane_funcs cursor_funcs = {vgpu_plane_check, cursor_update, NULL};
static const struct drm_crtc_funcs crtc_funcs = {NULL, NULL, crtc_disable, NULL, NULL, NULL,
                                                 NULL, NULL, NULL,         NULL, NULL, NULL};

/** A connector is connected while the device says its scanout is
 * enabled. */
static int conn_detect(struct drm_connector *connector, int force)
{
    (void)force;
    return output_of(connector, connector)->info.enabled ? DRM_CONNECTOR_CONNECTED
                                                         : DRM_CONNECTOR_DISCONNECTED;
}

/**
 * A connector's modes, Linux's virtio_gpu_conn_get_modes: the EDID's; or,
 * without one, every DMT mode up to 8192x8192 with the CVT mode of the
 * scanout's size preferred, or 1024x768 preferred while the device gives
 * no size.
 */
static int conn_get_modes(struct drm_connector *connector)
{
    struct virtio_gpu_output *o = output_of(connector, connector);
    struct drm_display_mode *m;
    struct drm_mode_modeinfo cvt;
    int count;

    if (o->vg->has_edid) {
        count = drm_connector_update_edid(connector, o->edid_len ? o->edid : NULL, o->edid_len);
        if (count) {
            return count;
        }
    }
    count = drm_add_modes_noedid(connector, XRES_MAX, YRES_MAX);
    if (!o->info.r.width || !o->info.r.height) {
        for (m = connector->probed_modes; m; m = m->next) {
            if (m->info.hdisplay == XRES_DEF && m->info.vdisplay == YRES_DEF) {
                m->info.type |= DRM_MODE_TYPE_PREFERRED;
            }
        }
    } else if (drm_cvt_mode((int)o->info.r.width, (int)o->info.r.height, 60, 0, 0, 0, &cvt)) {
        cvt.type |= DRM_MODE_TYPE_PREFERRED;
        if (drm_mode_probed_add(connector, &cvt) == 0) {
            count++;
        }
    }
    return count;
}

/**
 * A mode's check, Linux's virtio_gpu_conn_mode_valid: any mode but a
 * preferred one, which must be 1024x768 or within 16 pixels below the
 * scanout's size in each direction.
 */
static int32_t conn_mode_valid(struct drm_connector *connector,
                               const struct drm_mode_modeinfo *mode)
{
    struct virtio_gpu_output *o = output_of(connector, connector);
    int w = (int)o->info.r.width, h = (int)o->info.r.height;
    int hd = mode->hdisplay, vd = mode->vdisplay;

    if (!(mode->type & DRM_MODE_TYPE_PREFERRED)) {
        return 0;
    }
    if (hd == XRES_DEF && vd == YRES_DEF) {
        return 0;
    }
    if (hd <= w && hd >= w - 16 && vd <= h && vd >= h - 16) {
        return 0;
    }
    return -EINVAL;
}

static const struct drm_connector_funcs conn_funcs = {conn_detect, conn_get_modes, conn_mode_valid,
                                                      NULL};

/**
 * Make output `i`: its primary plane with damage clips, its cursor plane,
 * its CRTC, its virtual connector with the EDID property when the device
 * gives EDIDs, and its virtual encoder, in Linux's order, so that the
 * objects' IDs are Linux's. The first output is enabled at 1024x768 until
 * the device answers, as on Linux. 0 or the core's error.
 */
static int32_t output_init(struct virtio_gpu_device *vg, uint32_t i)
{
    struct virtio_gpu_output *o = &vg->outputs[i];
    struct drm_device *dev = &vg->drm;
    int32_t rc;

    o->vg = vg;
    o->index = i;
    if (i == 0) {
        o->info.enabled = 1;
        o->info.r.width = XRES_DEF;
        o->info.r.height = YRES_DEF;
    }
    rc = drm_universal_plane_init(dev, &o->primary, 1u << i, &primary_funcs, primary_formats, 1,
                                  NULL, DRM_PLANE_TYPE_PRIMARY);
    if (rc != 0) {
        return rc;
    }
    drm_plane_enable_fb_damage_clips(&o->primary);
    rc = drm_universal_plane_init(dev, &o->cursor, 1u << i, &cursor_funcs, cursor_formats, 1, NULL,
                                  DRM_PLANE_TYPE_CURSOR);
    if (rc == 0) {
        rc = drm_crtc_init_with_planes(dev, &o->crtc, &o->primary, &o->cursor, &crtc_funcs);
    }
    if (rc == 0) {
        rc = drm_connector_init(dev, &o->connector, &conn_funcs, DRM_MODE_CONNECTOR_VIRTUAL);
    }
    if (rc != 0) {
        return rc;
    }
    if (vg->has_edid) {
        drm_connector_attach_edid_property(&o->connector);
    }
    rc = drm_encoder_init(dev, &o->encoder, DRM_MODE_ENCODER_VIRTUAL);
    if (rc != 0) {
        return rc;
    }
    o->encoder.possible_crtcs = 1u << i;
    return drm_connector_attach_encoder(&o->connector, &o->encoder);
}

/**
 * Give the device its display, Linux's virtio_gpu_modeset_init: sizes from
 * 32x32 to 8192x8192, DIRTYFB as damage, no modifiers, and an output for
 * each scanout. 0 or the core's error.
 */
static int32_t modeset_init(struct virtio_gpu_device *vg)
{
    struct drm_mode_config *mc = &vg->drm.mode_config;
    uint32_t i;
    int32_t rc;

    if (!vg->num_scanouts) {
        return 0;
    }
    mc->min_width = XRES_MIN;
    mc->min_height = YRES_MIN;
    mc->max_width = XRES_MAX;
    mc->max_height = YRES_MAX;
    mc->fb_dirty = 1;
    mc->fb_modifiers_not_supported = 1;
    rc = drm_mode_config_init(&vg->drm);
    for (i = 0; rc == 0 && i < vg->num_scanouts; i++) {
        rc = output_init(vg, i);
    }
    return rc;
}

/* ------------------------------------------------------------------------
 * Display changes and the worker
 */

/**
 * The configuration interrupt: the displays may have changed. The worker,
 * or the next open while there is none, rereads them. Handler context.
 */
static void vgpu_config_changed(struct virtio_dev *d)
{
    struct virtio_gpu_device *vg = (struct virtio_gpu_device *)d->priv;

    vg->display_pending = 1;
    if (vgpu_wake_ready) {
        wq_wake_all(&vgpu_wake);
    }
}

/**
 * Linux's virtio_gpu_config_changed_work_func: on VIRTIO_GPU_EVENT_DISPLAY,
 * reread the EDIDs and the displays and tell user space with a hotplug
 * uevent, as Linux's display-info answer does on a device that is not
 * polled; then clear the events read. May sleep.
 */
static void vgpu_display_changed(struct virtio_gpu_device *vg)
{
    uint32_t events = 0, clear = 0;

    virtio_config_read(vg->vdev, VIRTIO_GPU_CONFIG_EVENTS_READ, &events, sizeof(events));
    if (events & VIRTIO_GPU_EVENT_DISPLAY) {
        if (vg->num_scanouts) {
            vgpu_read_displays(vg);
            drm_kms_hotplug_event(&vg->drm);
        }
        clear |= VIRTIO_GPU_EVENT_DISPLAY;
    }
    virtio_config_write(vg->vdev, VIRTIO_GPU_CONFIG_EVENTS_CLEAR, &clear, sizeof(clear));
}

/** Whether some device has a display change pending. */
static int work_pending(void *arg)
{
    int i;

    (void)arg;
    for (i = 0; i < VGPU_DEVICES; i++) {
        if (vgpu_devices[i] && (vgpu_devices[i]->display_pending || vgpu_devices[i]->done_list)) {
            return 1;
        }
    }
    return 0;
}

#ifndef RACH2_HOSTTEST
/** The worker: serve each device's display change, then free what its
 * queues answered, and sleep in an idle wait until the next change. */
static void vgpu_worker(void)
{
    struct virtio_gpu_device *vg;
    int i;

    arch_irq_enable();
    for (;;) {
        (void)wq_wait_event_idle(&vgpu_wake, work_pending, NULL);
        for (i = 0; i < VGPU_DEVICES; i++) {
            vg = vgpu_devices[i];
            if (vg && !vg->dead) {
                if (vg->display_pending) {
                    vg->display_pending = 0;
                    vgpu_display_changed(vg);
                }
                vgpu_reap(vg);
            }
        }
    }
}

/** Make the worker, `virtio_gpu`, once. A failure is logged, and an open
 * then serves a pending display change itself. */
static void vgpu_worker_start(void)
{
    process_t *p;
    uintptr_t base;
    uint32_t flags;

    if (vgpu_thread) {
        return;
    }
    flags = kirq_save();
    base = kstack_alloc();
    p = base ? alloc_process() : NULL;
    if (!p) {
        if (base) {
            kstack_free(base + KERNEL_STACK_SIZE);
        }
        kirq_restore(flags);
        klog(KLOG_ERR, "virtio_gpu", "worker: refused: no record or stack for it");
        return;
    }
    p->kernel_stack = base + KERNEL_STACK_SIZE;
    p->space_root = 0;
    p->page_dir = NULL;
    p->parent_pid = 0;
    proc_set_comm(p, "virtio_gpu");
    setup_process_stack(p, vgpu_worker);
    vgpu_thread = p;
    sched_make_ready(p);
    kirq_restore(flags);
    klog(KLOG_DEBUG, "virtio_gpu", "worker: pid %d", (int)p->pid);
}
#endif

/* ------------------------------------------------------------------------
 * The driver's ioctls
 */

/** MAP: the mmap offset of a handle's object, made the first time. ENOENT
 * for no such handle. */
static int32_t vgpu_map_ioctl(struct drm_device *dev, void *data, struct drm_file *file)
{
    struct drm_virtgpu_map *args = (struct drm_virtgpu_map *)data;
    struct drm_gem_object *obj = drm_gem_object_lookup(file, args->handle);
    int32_t rc;

    (void)dev;
    if (!obj) {
        return -ENOENT;
    }
    rc = drm_gem_create_mmap_offset(obj);
    if (rc == 0) {
        args->offset = obj->mmap_offset;
    }
    drm_gem_object_put(obj);
    return rc;
}

/** GETPARAM: the int Linux answers for each parameter, written to the
 * user's pointer. EINVAL for an unknown one, EFAULT. */
static int32_t vgpu_getparam_ioctl(struct drm_device *dev, void *data, struct drm_file *file)
{
    struct drm_virtgpu_getparam *args = (struct drm_virtgpu_getparam *)data;
    int32_t value;

    (void)file;
    switch (args->param) {
    case VIRTGPU_PARAM_CAPSET_QUERY_FIX:
        value = 1;
        break;
    case VIRTGPU_PARAM_3D_FEATURES:
        value = to_vg(dev)->has_virgl;
        break;
    case VIRTGPU_PARAM_SUPPORTED_CAPSET_IDs:
        value = (int32_t)to_vg(dev)->capset_mask;
        break;
    case VIRTGPU_PARAM_RESOURCE_BLOB:
        value = to_vg(dev)->has_blob;
        break;
    case VIRTGPU_PARAM_HOST_VISIBLE:
        value = to_vg(dev)->has_host_visible;
        break;
    case VIRTGPU_PARAM_CROSS_DEVICE:
        value = to_vg(dev)->has_uuid;
        break;
    case VIRTGPU_PARAM_CONTEXT_INIT:
    case VIRTGPU_PARAM_EXPLICIT_DEBUG_NAME:
        value = to_vg(dev)->has_context_init;
        break;
    default:
        return -EINVAL;
    }
    if (copy_to_user((void __user *)(uintptr_t)args->value, &value, sizeof(value)) != 0) {
        return -EFAULT;
    }
    return 0;
}

/**
 * RESOURCE_CREATE without 3D, as Linux: a 2D resource of the given format
 * and size over a new object of `size` bytes, a page for 0, with its handle
 * and resource id. EINVAL for a depth, sample count, level count, array or
 * target (2, PIPE_TEXTURE_2D) a 2D resource cannot have, or the object's
 * error. May sleep.
 */
static int32_t vgpu_resource_create_ioctl(struct drm_device *dev, void *data, struct drm_file *file)
{
    struct drm_virtgpu_resource_create *rc = (struct drm_virtgpu_resource_create *)data;
    struct virtio_gpu_object *bo;
    uint32_t handle = 0;
    int32_t err;

    if (!to_vg(dev)->has_virgl && (rc->depth > 1 || rc->nr_samples > 1 || rc->last_level > 1 ||
                                   rc->target != 2 || rc->array_size > 1)) {
        return -EINVAL;
    }
    if (to_vg(dev)->has_virgl) {
        err = vgpu_context_ensure(to_vg(dev), file);
        if (err != 0) {
            return err;
        }
    }
    bo = vgpu_obj_create(to_vg(dev), rc->size ? rc->size : PAGE_SIZE, rc, to_vg(dev)->has_virgl,
                         &err);
    if (!bo) {
        return err;
    }
    err = to_vg(dev)->has_virgl ? vgpu_context_attach(to_vg(dev), file, bo) : 0;
    if (err == 0) {
        err = drm_gem_handle_create(file, &bo->shmem.base, &handle);
    }
    if (err == 0) {
        rc->res_handle = bo->res_id;
        rc->bo_handle = handle;
    }
    drm_gem_object_put(&bo->shmem.base);
    return err;
}

/** RESOURCE_INFO: a handle's object's size and resource id, and where a
 * blob's memory is as Linux says it: HOST3D for any with host memory,
 * GUEST for guest memory alone, 0 for no blob. ENOENT. */
static int32_t vgpu_resource_info_ioctl(struct drm_device *dev, void *data, struct drm_file *file)
{
    struct drm_virtgpu_resource_info *ri = (struct drm_virtgpu_resource_info *)data;
    struct drm_gem_object *obj = drm_gem_object_lookup(file, ri->bo_handle);
    struct virtio_gpu_object *bo;

    (void)dev;
    if (!obj) {
        return -ENOENT;
    }
    bo = vgpu_bo(obj);
    ri->size = (uint32_t)obj->size;
    ri->res_handle = bo ? bo->res_id : 0;
    ri->blob_mem = !bo || !bo->blob_mem                     ? 0
                   : bo->blob_mem == VIRTGPU_BLOB_MEM_GUEST ? VIRTGPU_BLOB_MEM_GUEST
                                                            : VIRTGPU_BLOB_MEM_HOST3D;
    drm_gem_object_put(obj);
    return 0;
}

/** TRANSFER_FROM_HOST: ENOSYS without 3D, as Linux; 77-2 adds it. */
static int32_t vgpu_transfer_from_host_ioctl(struct drm_device *dev, void *data,
                                             struct drm_file *file)
{
    if (!to_vg(dev)->has_virgl) {
        return -ENOSYS;
    }
    return vgpu_transfer_3d_ioctl(dev, data, file, 1);
}

/**
 * TRANSFER_TO_HOST without 3D, as Linux: TRANSFER_TO_HOST_2D of the box's
 * rectangle of a handle's resource from `offset` in its backing, not waited
 * for; the command's fence joins the object's reads, which WAIT waits for.
 * ENOENT for no such handle, EINVAL for an object with no resource, ENOMEM.
 */
static int32_t vgpu_transfer_to_host_ioctl(struct drm_device *dev, void *data,
                                           struct drm_file *file)
{
    struct drm_virtgpu_3d_transfer *args = (struct drm_virtgpu_3d_transfer *)data;
    struct drm_gem_object *obj;
    struct virtio_gpu_object *bo;
    struct virtio_gpu_cmd *cmd;
    int32_t rc = 0;

    if (to_vg(dev)->has_virgl) {
        return vgpu_transfer_3d_ioctl(dev, data, file, 0);
    }
    obj = drm_gem_object_lookup(file, args->bo_handle);
    if (!obj) {
        return -ENOENT;
    }
    bo = vgpu_bo(obj);
    if (!bo || !bo->res_id) {
        drm_gem_object_put(obj);
        return -EINVAL;
    }
    cmd = vgpu_transfer_cmd(to_vg(dev), bo->res_id, args->offset,
                            rect_of(args->box.x, args->box.y, args->box.w, args->box.h));
    if (!cmd) {
        rc = -ENOMEM;
    } else {
        vgpu_cmd_send(cmd, NULL, NULL);
        /* Only the reaper frees the command, in process context, which this
         * ioctl holds, so its fence is still there. */
        rc = dma_resv_add_fence(obj->resv, cmd->fence, 0);
    }
    drm_gem_object_put(obj);
    return rc;
}

static const struct drm_ioctl_desc vgpu_ioctls[DRM_VIRTIO_NUM_IOCTLS] = {
    {DRM_IOCTL_VIRTGPU_MAP, DRM_RENDER_ALLOW, vgpu_map_ioctl, "VIRTGPU_MAP"},
    {DRM_IOCTL_VIRTGPU_EXECBUFFER, DRM_RENDER_ALLOW, vgpu_execbuffer_ioctl, "VIRTGPU_EXECBUFFER"},
    {DRM_IOCTL_VIRTGPU_GETPARAM, DRM_RENDER_ALLOW, vgpu_getparam_ioctl, "VIRTGPU_GETPARAM"},
    {DRM_IOCTL_VIRTGPU_RESOURCE_CREATE, DRM_RENDER_ALLOW, vgpu_resource_create_ioctl,
     "VIRTGPU_RESOURCE_CREATE"},
    {DRM_IOCTL_VIRTGPU_RESOURCE_INFO, DRM_RENDER_ALLOW, vgpu_resource_info_ioctl,
     "VIRTGPU_RESOURCE_INFO"},
    {DRM_IOCTL_VIRTGPU_TRANSFER_FROM_HOST, DRM_RENDER_ALLOW, vgpu_transfer_from_host_ioctl,
     "VIRTGPU_TRANSFER_FROM_HOST"},
    {DRM_IOCTL_VIRTGPU_TRANSFER_TO_HOST, DRM_RENDER_ALLOW, vgpu_transfer_to_host_ioctl,
     "VIRTGPU_TRANSFER_TO_HOST"},
    {DRM_IOCTL_VIRTGPU_WAIT, DRM_RENDER_ALLOW, vgpu_wait_ioctl, "VIRTGPU_WAIT"},
    {DRM_IOCTL_VIRTGPU_GET_CAPS, DRM_RENDER_ALLOW, vgpu_get_caps_ioctl, "VIRTGPU_GET_CAPS"},
    {DRM_IOCTL_VIRTGPU_RESOURCE_CREATE_BLOB, DRM_RENDER_ALLOW, vgpu_blob_ioctl,
     "VIRTGPU_RESOURCE_CREATE_BLOB"},
    {DRM_IOCTL_VIRTGPU_CONTEXT_INIT, DRM_RENDER_ALLOW, vgpu_context_init_ioctl,
     "VIRTGPU_CONTEXT_INIT"}};

/* ------------------------------------------------------------------------
 * The device's life
 */

/**
 * The open of a node: the worker is made at the first, once init has its
 * pid, and a display change that came before it is served now, so that the
 * opener reads the displays as they are. May sleep.
 */
static int32_t vgpu_open(struct drm_device *dev, struct drm_file *file)
{
    struct virtio_gpu_device *vg = to_vg(dev);

    int32_t rc = vgpu_context_open(vg, file);

    if (rc != 0) {
        return rc;
    }
#ifndef RACH2_HOSTTEST
    vgpu_worker_start();
#endif
    if (vg->display_pending && !vg->dead) {
        vg->display_pending = 0;
        vgpu_display_changed(vg);
    }
    vgpu_reap(vg);
    return 0;
}

/** Take `vg` out of the worker's table. */
static void devices_remove(struct virtio_gpu_device *vg)
{
    int i;

    for (i = 0; i < VGPU_DEVICES; i++) {
        if (vgpu_devices[i] == vg) {
            vgpu_devices[i] = NULL;
        }
    }
}

/**
 * Stop the device for good: reset it and free its queues, then answer every
 * command started and not yet answered with none, its fence failing with
 * -ENODEV, as the device-loss rule asks, and free what nobody waits on; a
 * waiter frees its own. Process context.
 */
void vgpu_kill(struct virtio_gpu_device *vg)
{
    struct virtio_gpu_cmd *cmd;
    struct virtio_gpu_object *bo;
    struct vgpu_context *ctx;
    uint32_t flags;

    if (vg->dead) {
        return;
    }
    flags = spin_lock_irqsave(&vg->lock);
    vg->dead = 1;
    spin_unlock_irqrestore(&vg->lock, flags);
    vg->vdev->config_changed = NULL;
    virtio_teardown(vg->vdev);
    flags = spin_lock_irqsave(&vg->lock);
    vg->pending[0] = vg->pending[1] = NULL;
    vg->pending_tail[0] = vg->pending_tail[1] = NULL;
    for (cmd = vg->live; cmd; cmd = cmd->live_next) {
        if (cmd->queued && !cmd->done) {
            cmd->done = 1;
            cmd->resp_type = 0;
            if (!cmd->waited) {
                done_add(vg, cmd);
            }
        }
    }
    spin_unlock_irqrestore(&vg->lock, flags);
    /* The big kernel lock keeps every waiter and the reaper off until the
     * walk ends, so no command leaves the list under it. */
    for (cmd = vg->live; cmd; cmd = cmd->live_next) {
        if (cmd->queued && cmd->fence && !dma_fence_is_signaled(cmd->fence)) {
            dma_fence_set_error(cmd->fence, -ENODEV);
            dma_fence_signal(cmd->fence);
        }
    }
    vgpu_reap(vg);
    for (bo = vg->resources; bo; bo = bo->next) {
        vgpu_resource_detach(vg, bo);
    }
    for (ctx = vg->contexts; ctx; ctx = ctx->next) {
        ctx->created = 0;
        ctx->id = 0;
    }
    for (bo = vg->resources; bo; bo = bo->next) {
        bo->res_id = 0;
        shmem_free(&bo->shmem.pages);
    }
    k_memset(vg->res_ids, 0, VIRTIO_GPU_MAX_RESOURCES / 8);
}

/** The core's unplug, after the nodes have gone, Linux's virtio_gpu_remove:
 * the display is turned off, which lets go of the framebuffers it shows,
 * and the device stopped (`vgpu_kill`). The record stays until the last
 * reference. */
static void vgpu_unplug(struct drm_device *dev)
{
    struct virtio_gpu_device *vg = to_vg(dev);

    vgpu_reset_remove(vg);
    devices_remove(vg);
    (void)drm_atomic_helper_shutdown(dev);
    vgpu_kill(vg);
}

/** The core's release, after the last reference: free the display, the
 * outputs, the ids and the record. */
static void vgpu_release(struct drm_device *dev)
{
    struct virtio_gpu_device *vg = to_vg(dev);

    drm_mode_config_cleanup(dev);
    kfree(vg->outputs);
    kfree(vg->res_ids);
    kfree(vg->capsets);
    kfree(vg);
}

/* What the driver says of itself: Linux's virtio_gpu, with its dumb buffers,
 * its ioctls and the shmem helper's PRIME import. */
static const struct drm_driver vgpu_driver = {
    DRM_IDENT_VIRTIO_GPU, vgpu_open,   vgpu_context_close,    vgpu_dumb_create, NULL,
    vgpu_unplug,          vgpu_ioctls, DRM_VIRTIO_NUM_IOCTLS, vgpu_release};

/** Remove the firmware's framebuffer from every memory BAR of a virtio-vga,
 * as Linux's virtio_gpu_pci_quirk does before the driver touches it. */
static void remove_firmware(const pci_device_t *pci)
{
    uint64_t base, size;
    int i;

    for (i = 0; i < 6; i++) {
        if (!pci_bar_is_memory(pci->bar[i])) {
            continue;
        }
        base = pci_bar_address64(pci, i);
        size = pci_bar_size64(pci, i);
        if (base && size) {
            (void)drm_aperture_remove_conflicting(base, size, "virtiodrmfb");
        }
        if (pci_bar_is_64bit(pci->bar[i])) {
            i++;
        }
    }
}

/** Add `vg` to the worker's table: 0, or -ENOSPC when it is full. */
static int32_t devices_add(struct virtio_gpu_device *vg)
{
    int i;

    for (i = 0; i < VGPU_DEVICES; i++) {
        if (!vgpu_devices[i]) {
            vgpu_devices[i] = vg;
            return 0;
        }
    }
    return -ENOSPC;
}

/**
 * Initialize both queues and publish negotiated feature information; 0 or
 * -errno.
 */
static int32_t vgpu_queues_init(struct virtio_gpu_device *vg)
{
    int32_t rc;

    vg->has_virgl = virtio_has(vg->vdev, VIRTIO_GPU_F_VIRGL);
    vg->has_edid = virtio_has(vg->vdev, VIRTIO_GPU_F_EDID);
    vg->has_indirect = virtio_has(vg->vdev, VIRTIO_F_INDIRECT_DESC);
    vg->has_blob = virtio_has(vg->vdev, VIRTIO_GPU_F_RESOURCE_BLOB);
    vg->has_context_init = virtio_has(vg->vdev, VIRTIO_GPU_F_CONTEXT_INIT);
    vg->has_uuid = virtio_has(vg->vdev, VIRTIO_GPU_F_RESOURCE_UUID);
    vgpu_hostmem_init(vg);
    klog(KLOG_INFO, "virtio_gpu", "features: %cvirgl %cedid %cresource_blob %chost_visible",
         vg->has_virgl ? '+' : '-', vg->has_edid ? '+' : '-', vg->has_blob ? '+' : '-',
         vg->has_host_visible ? '+' : '-');
    klog(KLOG_INFO, "virtio_gpu", "features: %ccontext_init", vg->has_context_init ? '+' : '-');
    vg->vdev->priv = vg;
    vg->vdev->config_changed = vgpu_config_changed;
    rc = virtio_queue_setup(vg->vdev, VIRTIO_GPU_QUEUE_CONTROL, cmd_done, vg, &vg->ctrlq);
    if (rc == 0) {
        rc = virtio_queue_setup(vg->vdev, VIRTIO_GPU_QUEUE_CURSOR, cmd_done, vg, &vg->cursorq);
    }
    if (rc != 0) {
        return rc;
    }
    return 0;
}

/**
 * Bind the virtio device `vdev`, Linux's virtio_gpu_probe and
 * virtio_gpu_init: the firmware's framebuffer removed under virtio-vga, the
 * features negotiated (EDID, virgl, resource UUIDs, blobs and context
 * initialization), the host-visible region found, both queues, the
 * display, DRIVER_OK, the EDIDs and the displays read, and the DRM device
 * registered. 0 or -errno, with the device reset.
 */
static int32_t vgpu_bind(struct virtio_dev *vdev)
{
    struct virtio_gpu_device *vg;
    const pci_device_t *pci = virtio_pci_function(vdev);
    uint32_t n = 0;
    int32_t rc;

    vg = (struct virtio_gpu_device *)kzalloc(sizeof(*vg));
    if (!vg) {
        return -ENOMEM;
    }
    drm_dev_init(&vg->drm, &vgpu_driver);
    vg->vdev = vdev;
    vg->pci = pci;
    vg->vga = pci && pci->class_code == PCI_CLASS_DISPLAY && pci->subclass == PCI_SUBCLASS_VGA;
    if (pci) {
        klog(KLOG_INFO, "virtio_gpu", "pci: %s detected at 0000:%02x:%02x.%d",
             vg->vga ? "virtio-vga" : "virtio-gpu-pci", pci->bus, pci->device, pci->function);
        if (vg->vga) {
            remove_firmware(pci);
        }
    }
    vg->res_ids = (uint32_t *)kzalloc(VIRTIO_GPU_MAX_RESOURCES / 8);
    if (!vg->res_ids) {
        kfree(vg);
        return -ENOMEM;
    }
    rc = virtio_init(vdev,
                     VIRTIO_BIT(VIRTIO_GPU_F_EDID) | VIRTIO_BIT(VIRTIO_GPU_F_VIRGL) |
                         VIRTIO_BIT(VIRTIO_GPU_F_RESOURCE_UUID) |
                         VIRTIO_BIT(VIRTIO_GPU_F_RESOURCE_BLOB) |
                         VIRTIO_BIT(VIRTIO_GPU_F_CONTEXT_INIT),
                     2);
    if (rc != 0) {
        kfree(vg->res_ids);
        kfree(vg);
        return rc;
    }
    rc = vgpu_queues_init(vg);
    if (rc != 0) {
        goto fail;
    }
    vg->fence_context = dma_fence_context_alloc(1);
    virtio_config_read(vdev, VIRTIO_GPU_CONFIG_NUM_SCANOUTS, &n, sizeof(n));
    vg->num_scanouts = n < VIRTIO_GPU_MAX_SCANOUTS ? n : VIRTIO_GPU_MAX_SCANOUTS;
    if (!vg->num_scanouts) {
        klog(KLOG_INFO, "virtio_gpu", "KMS disabled");
        vg->has_edid = 0;
        vg->drm.features &= ~(DRIVER_MODESET | DRIVER_ATOMIC);
    } else {
        klog(KLOG_INFO, "virtio_gpu", "number of scanouts: %u", n);
    }
    virtio_config_read(vdev, VIRTIO_GPU_CONFIG_NUM_CAPSETS, &vg->num_capsets,
                       sizeof(vg->num_capsets));
    klog(KLOG_INFO, "virtio_gpu", "number of cap sets: %u", vg->num_capsets);
    if (vg->num_scanouts) {
        vg->outputs = (struct virtio_gpu_output *)kzalloc(vg->num_scanouts *
                                                          sizeof(struct virtio_gpu_output));
        if (!vg->outputs) {
            rc = -ENOMEM;
            goto fail;
        }
    }
    rc = modeset_init(vg);
    if (rc != 0) {
        goto fail;
    }
    virtio_ready(vdev);
    rc = vgpu_capsets_read(vg);
    if (rc != 0) {
        goto fail;
    }
    vgpu_read_displays(vg);
    vgpu_reap(vg);
    if (!vgpu_wake_ready) {
        wait_queue_init(&vgpu_wake);
        vgpu_wake_ready = 1;
    }
    rc = devices_add(vg);
    if (rc != 0) {
        goto fail;
    }
    vg->drm.pci = pci;
    rc = drm_dev_register(&vg->drm);
    if (rc != 0) {
        devices_remove(vg);
        goto fail;
    }
    rc = vgpu_reset_register(vg);
    if (rc != 0) {
        drm_dev_unplug(&vg->drm);
        return rc;
    }
    return 0;
fail:
    vgpu_kill(vg);
    drm_mode_config_cleanup(&vg->drm);
    kfree(vg->outputs);
    kfree(vg->res_ids);
    kfree(vg->capsets);
    kfree(vg);
    return rc;
}

int32_t virtio_gpu_probe(void)
{
    struct virtio_dev *vdev;
    int nth, bound = 0;
    int32_t rc, last = -ENODEV;

    for (nth = 0; nth < VGPU_DEVICES; nth++) {
        rc = virtio_pci_find(VIRTIO_ID_GPU, nth, &vdev);
        if (rc == -ENODEV) {
            break;
        }
        if (rc == 0) {
            rc = vgpu_bind(vdev);
        }
        if (rc == 0) {
            bound++;
        } else {
            klog(KLOG_WARN, "virtio_gpu", "refused: gpu %d: error %d", nth, (int)rc);
            last = rc;
        }
    }
    return bound ? bound : last;
}
