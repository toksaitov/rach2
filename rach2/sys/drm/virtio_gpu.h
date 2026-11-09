/*
 * virtio_gpu.h: the virtio GPU as the driver sees it: the device's commands
 * and configuration (virtio 1.2, 5.7), the driver's own ioctls as Linux
 * 6.12's virtgpu_drm.h gives them, and the driver's records.
 */
#ifndef _VIRTIO_GPU_H_
#define _VIRTIO_GPU_H_

#include "drm.h"
#include "../virtio/virtio.h"

/* ------------------------------------------------------------------------
 * The device (virtio 1.2, 5.7; Linux's include/uapi/linux/virtio_gpu.h).
 * Every field is little-endian, as the processors Rach2 runs on are.
 */

/* Feature bits. */
#define VIRTIO_GPU_F_VIRGL         0
#define VIRTIO_GPU_F_EDID          1
#define VIRTIO_GPU_F_RESOURCE_UUID 2
#define VIRTIO_GPU_F_RESOURCE_BLOB 3
#define VIRTIO_GPU_F_CONTEXT_INIT  4

/* The two queues. */
#define VIRTIO_GPU_QUEUE_CONTROL 0
#define VIRTIO_GPU_QUEUE_CURSOR  1

/* Commands and responses: the `type` of a header. */
#define VIRTIO_GPU_CMD_GET_DISPLAY_INFO        0x0100u
#define VIRTIO_GPU_CMD_RESOURCE_CREATE_2D      0x0101u
#define VIRTIO_GPU_CMD_RESOURCE_UNREF          0x0102u
#define VIRTIO_GPU_CMD_SET_SCANOUT             0x0103u
#define VIRTIO_GPU_CMD_RESOURCE_FLUSH          0x0104u
#define VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D     0x0105u
#define VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING 0x0106u
#define VIRTIO_GPU_CMD_RESOURCE_DETACH_BACKING 0x0107u
#define VIRTIO_GPU_CMD_GET_CAPSET_INFO         0x0108u
#define VIRTIO_GPU_CMD_GET_CAPSET              0x0109u
#define VIRTIO_GPU_CMD_GET_EDID                0x010Au
#define VIRTIO_GPU_CMD_UPDATE_CURSOR           0x0300u
#define VIRTIO_GPU_CMD_MOVE_CURSOR             0x0301u
#define VIRTIO_GPU_RESP_OK_NODATA              0x1100u
#define VIRTIO_GPU_RESP_OK_DISPLAY_INFO        0x1101u
#define VIRTIO_GPU_RESP_OK_EDID                0x1104u
#define VIRTIO_GPU_RESP_ERR_UNSPEC             0x1200u
#define VIRTIO_GPU_RESP_ERR_OUT_OF_MEMORY      0x1201u

/* A header's `flags`: the device answers once the command's work is done,
 * and copies `fence_id` into its answer. */
#define VIRTIO_GPU_FLAG_FENCE 1u

/* The resource formats of 2D resources. */
#define VIRTIO_GPU_FORMAT_B8G8R8A8_UNORM 1u
#define VIRTIO_GPU_FORMAT_B8G8R8X8_UNORM 2u
#define VIRTIO_GPU_FORMAT_A8R8G8B8_UNORM 3u
#define VIRTIO_GPU_FORMAT_X8R8G8B8_UNORM 4u

/* `events_read` of the configuration: the displays changed. */
#define VIRTIO_GPU_EVENT_DISPLAY 1u

#define VIRTIO_GPU_MAX_SCANOUTS 16

struct virtio_gpu_ctrl_hdr {
    uint32_t type;
    uint32_t flags;
    uint64_t fence_id;
    uint32_t ctx_id;
    uint8_t ring_idx;
    uint8_t padding[3];
};

struct virtio_gpu_rect {
    uint32_t x, y, width, height;
};

struct virtio_gpu_display_one {
    struct virtio_gpu_rect r;
    uint32_t enabled;
    uint32_t flags;
};

struct virtio_gpu_resp_display_info {
    struct virtio_gpu_ctrl_hdr hdr;
    struct virtio_gpu_display_one pmodes[VIRTIO_GPU_MAX_SCANOUTS];
};

struct virtio_gpu_cmd_get_edid {
    struct virtio_gpu_ctrl_hdr hdr;
    uint32_t scanout;
    uint32_t padding;
};

struct virtio_gpu_resp_edid {
    struct virtio_gpu_ctrl_hdr hdr;
    uint32_t size;
    uint32_t padding;
    uint8_t edid[1024];
};

struct virtio_gpu_resource_create_2d {
    struct virtio_gpu_ctrl_hdr hdr;
    uint32_t resource_id;
    uint32_t format;
    uint32_t width, height;
};

struct virtio_gpu_resource_unref {
    struct virtio_gpu_ctrl_hdr hdr;
    uint32_t resource_id;
    uint32_t padding;
};

struct virtio_gpu_set_scanout {
    struct virtio_gpu_ctrl_hdr hdr;
    struct virtio_gpu_rect r;
    uint32_t scanout_id;
    uint32_t resource_id;
};

struct virtio_gpu_resource_flush {
    struct virtio_gpu_ctrl_hdr hdr;
    struct virtio_gpu_rect r;
    uint32_t resource_id;
    uint32_t padding;
};

struct virtio_gpu_transfer_to_host_2d {
    struct virtio_gpu_ctrl_hdr hdr;
    struct virtio_gpu_rect r;
    uint64_t offset; /* bytes into the backing of the rectangle's first pixel */
    uint32_t resource_id;
    uint32_t padding;
};

struct virtio_gpu_mem_entry {
    uint64_t addr;
    uint32_t length;
    uint32_t padding;
};

/* Followed by `nr_entries` struct virtio_gpu_mem_entry. */
struct virtio_gpu_resource_attach_backing {
    struct virtio_gpu_ctrl_hdr hdr;
    uint32_t resource_id;
    uint32_t nr_entries;
};

struct virtio_gpu_cursor_pos {
    uint32_t scanout_id;
    uint32_t x, y;
    uint32_t padding;
};

/* UPDATE_CURSOR and MOVE_CURSOR, on the cursor queue; MOVE reads `pos`
 * alone. */
struct virtio_gpu_update_cursor {
    struct virtio_gpu_ctrl_hdr hdr;
    struct virtio_gpu_cursor_pos pos;
    uint32_t resource_id;
    uint32_t hot_x, hot_y;
    uint32_t padding;
};

/* The device-specific configuration. */
#define VIRTIO_GPU_CONFIG_EVENTS_READ  0
#define VIRTIO_GPU_CONFIG_EVENTS_CLEAR 4
#define VIRTIO_GPU_CONFIG_NUM_SCANOUTS 8
#define VIRTIO_GPU_CONFIG_NUM_CAPSETS  12

/* ------------------------------------------------------------------------
 * The driver's ioctls, Linux 6.12's include/uapi/drm/virtgpu_drm.h. Each
 * structure has one layout on both ABIs.
 */

#define DRM_VIRTGPU_MAP                  0x01
#define DRM_VIRTGPU_EXECBUFFER           0x02
#define DRM_VIRTGPU_GETPARAM             0x03
#define DRM_VIRTGPU_RESOURCE_CREATE      0x04
#define DRM_VIRTGPU_RESOURCE_INFO        0x05
#define DRM_VIRTGPU_TRANSFER_FROM_HOST   0x06
#define DRM_VIRTGPU_TRANSFER_TO_HOST     0x07
#define DRM_VIRTGPU_WAIT                 0x08
#define DRM_VIRTGPU_GET_CAPS             0x09
#define DRM_VIRTGPU_RESOURCE_CREATE_BLOB 0x0a
#define DRM_VIRTGPU_CONTEXT_INIT         0x0b
#define DRM_VIRTIO_NUM_IOCTLS            11

struct drm_virtgpu_map {
    uint64_t offset; /* for mmap */
    uint32_t handle;
    uint32_t pad;
};

struct drm_virtgpu_execbuffer {
    uint32_t flags;
    uint32_t size;
    uint64_t command;
    uint64_t bo_handles;
    uint32_t num_bo_handles;
    int32_t fence_fd;
    uint32_t ring_idx;
    uint32_t syncobj_stride;
    uint32_t num_in_syncobjs;
    uint32_t num_out_syncobjs;
    uint64_t in_syncobjs;
    uint64_t out_syncobjs;
};

#define VIRTGPU_PARAM_3D_FEATURES          1
#define VIRTGPU_PARAM_CAPSET_QUERY_FIX     2
#define VIRTGPU_PARAM_RESOURCE_BLOB        3
#define VIRTGPU_PARAM_HOST_VISIBLE         4
#define VIRTGPU_PARAM_CROSS_DEVICE         5
#define VIRTGPU_PARAM_CONTEXT_INIT         6
#define VIRTGPU_PARAM_SUPPORTED_CAPSET_IDs 7
#define VIRTGPU_PARAM_EXPLICIT_DEBUG_NAME  8

struct drm_virtgpu_getparam {
    uint64_t param;
    uint64_t value; /* a user pointer to an int the answer goes in */
};

struct drm_virtgpu_resource_create {
    uint32_t target;
    uint32_t format;
    uint32_t bind;
    uint32_t width;
    uint32_t height;
    uint32_t depth;
    uint32_t array_size;
    uint32_t last_level;
    uint32_t nr_samples;
    uint32_t flags;
    uint32_t bo_handle;
    uint32_t res_handle;
    uint32_t size;
    uint32_t stride;
};

struct drm_virtgpu_resource_info {
    uint32_t bo_handle;
    uint32_t res_handle;
    uint32_t size;
    uint32_t blob_mem;
};

struct drm_virtgpu_3d_box {
    uint32_t x, y, z;
    uint32_t w, h, d;
};

/* TRANSFER_TO_HOST and TRANSFER_FROM_HOST. */
struct drm_virtgpu_3d_transfer {
    uint32_t bo_handle;
    struct drm_virtgpu_3d_box box;
    uint32_t level;
    uint32_t offset;
    uint32_t stride;
    uint32_t layer_stride;
};

#define VIRTGPU_WAIT_NOWAIT 1

struct drm_virtgpu_3d_wait {
    uint32_t handle;
    uint32_t flags;
};

struct drm_virtgpu_get_caps {
    uint32_t cap_set_id;
    uint32_t cap_set_ver;
    uint64_t addr;
    uint32_t size;
    uint32_t pad;
};

struct drm_virtgpu_resource_create_blob {
    uint32_t blob_mem;
    uint32_t blob_flags;
    uint32_t bo_handle;
    uint32_t res_handle;
    uint64_t size;
    uint32_t pad;
    uint32_t cmd_size;
    uint64_t cmd;
    uint64_t blob_id;
};

struct drm_virtgpu_context_init {
    uint32_t num_params;
    uint32_t pad;
    uint64_t ctx_set_params;
};

#define DRM_IOCTL_VIRTGPU_MAP DRM_IOWR(DRM_COMMAND_BASE + DRM_VIRTGPU_MAP, struct drm_virtgpu_map)
#define DRM_IOCTL_VIRTGPU_EXECBUFFER \
    DRM_IOWR(DRM_COMMAND_BASE + DRM_VIRTGPU_EXECBUFFER, struct drm_virtgpu_execbuffer)
#define DRM_IOCTL_VIRTGPU_GETPARAM \
    DRM_IOWR(DRM_COMMAND_BASE + DRM_VIRTGPU_GETPARAM, struct drm_virtgpu_getparam)
#define DRM_IOCTL_VIRTGPU_RESOURCE_CREATE \
    DRM_IOWR(DRM_COMMAND_BASE + DRM_VIRTGPU_RESOURCE_CREATE, struct drm_virtgpu_resource_create)
#define DRM_IOCTL_VIRTGPU_RESOURCE_INFO \
    DRM_IOWR(DRM_COMMAND_BASE + DRM_VIRTGPU_RESOURCE_INFO, struct drm_virtgpu_resource_info)
#define DRM_IOCTL_VIRTGPU_TRANSFER_FROM_HOST \
    DRM_IOWR(DRM_COMMAND_BASE + DRM_VIRTGPU_TRANSFER_FROM_HOST, struct drm_virtgpu_3d_transfer)
#define DRM_IOCTL_VIRTGPU_TRANSFER_TO_HOST \
    DRM_IOWR(DRM_COMMAND_BASE + DRM_VIRTGPU_TRANSFER_TO_HOST, struct drm_virtgpu_3d_transfer)
#define DRM_IOCTL_VIRTGPU_WAIT \
    DRM_IOWR(DRM_COMMAND_BASE + DRM_VIRTGPU_WAIT, struct drm_virtgpu_3d_wait)
#define DRM_IOCTL_VIRTGPU_GET_CAPS \
    DRM_IOWR(DRM_COMMAND_BASE + DRM_VIRTGPU_GET_CAPS, struct drm_virtgpu_get_caps)
#define DRM_IOCTL_VIRTGPU_RESOURCE_CREATE_BLOB                    \
    DRM_IOWR(DRM_COMMAND_BASE + DRM_VIRTGPU_RESOURCE_CREATE_BLOB, \
             struct drm_virtgpu_resource_create_blob)
#define DRM_IOCTL_VIRTGPU_CONTEXT_INIT \
    DRM_IOWR(DRM_COMMAND_BASE + DRM_VIRTGPU_CONTEXT_INIT, struct drm_virtgpu_context_init)

/* ------------------------------------------------------------------------
 * The driver's records
 */

/*
 * A command and its answer, in one page the device reads and writes: the
 * request from offset 0, with the memory entries of ATTACH_BACKING after it
 * up to VIRTIO_GPU_RESP_OFF and on in `extra` pages, and the answer at
 * VIRTIO_GPU_RESP_OFF. A command on the control queue carries a fence,
 * which the device's answer signals, with -EIO when it answered an error.
 *
 * A command is the waiter's while `waited` is set: the waiter reads the
 * answer and frees it. Otherwise the answer puts it on the device's done
 * list, and the reaper (`virtio_gpu_reap`, process context) runs its
 * `complete` and frees it. `next` links it on the pending list, while the
 * queue has no room for it, and then on the done list; `live_next` on the
 * list of every command not yet freed, which an unplug walks.
 */
#define VIRTIO_GPU_RESP_OFF  2048u
#define VIRTIO_GPU_CMD_EXTRA 64 /* entry pages beyond the command's own */
#define VIRTIO_GPU_CMD_SGS   (VIRTIO_GPU_CMD_EXTRA + 2)
#define VIRTIO_GPU_PAGE_ENTS (PAGE_SIZE / sizeof(struct virtio_gpu_mem_entry))
#define VIRTIO_GPU_INLINE_ENTS                                                   \
    ((VIRTIO_GPU_RESP_OFF - sizeof(struct virtio_gpu_resource_attach_backing)) / \
     sizeof(struct virtio_gpu_mem_entry))

struct virtio_gpu_device;

struct virtio_gpu_cmd {
    struct virtio_gpu_cmd *next;
    struct virtio_gpu_cmd *live_next;
    struct virtio_gpu_device *vg;
    struct virtqueue *vq;
    uint8_t *page; /* the request at 0, the answer at VIRTIO_GPU_RESP_OFF */
    uint64_t addr; /* the page's address as the device reads it */
    void *extra[VIRTIO_GPU_CMD_EXTRA];
    uint64_t extra_addr[VIRTIO_GPU_CMD_EXTRA];
    uint32_t nextra;
    struct virtio_sg sg[VIRTIO_GPU_CMD_SGS];
    uint32_t nout;           /* segments the device reads, then one it writes */
    struct dma_fence *fence; /* held; NULL on the cursor queue */
    uint32_t resp_type;      /* the answer's type, once `done` */
    int queued;              /* started: its fence is live */
    int done;
    int waited;
    void (*complete)(struct virtio_gpu_cmd *cmd);
    void *arg; /* for `complete` */
};

/*
 * One scanout: its CRTC with a primary and a cursor plane, its encoder and
 * its virtual connector, the device's last word on it (`info` from
 * GET_DISPLAY_INFO, `edid` from GET_EDID), and the cursor's last
 * UPDATE_CURSOR. CRTC, connector and output share an index.
 */
struct virtio_gpu_output {
    struct virtio_gpu_device *vg;
    uint32_t index;
    struct drm_plane primary, cursor;
    struct drm_crtc crtc;
    struct drm_encoder encoder;
    struct drm_connector connector;
    struct virtio_gpu_display_one info;
    uint8_t edid[1024];
    uint32_t edid_len; /* bytes of `edid`, 0 for none */
};

/* Resource ids are the lowest free from 1, as Linux's ida gives them, up to
 * this many. */
#define VIRTIO_GPU_MAX_RESOURCES 65536

/*
 * A bound device. `lock` guards the pending and done lists and every
 * command's `done`, `waited` and `next`, and is taken with interrupts off,
 * since a queue's answers come in its interrupt handler. `display_pending`
 * is set by the configuration interrupt and cleared by the worker, which
 * rereads the displays.
 */
struct virtio_gpu_device {
    struct drm_device drm;
    struct virtio_dev *vdev;
    struct virtqueue *ctrlq, *cursorq;
    const pci_device_t *pci;
    int vga; /* a virtio-vga, which has a VGA's BARs */
    int has_edid;
    int has_indirect;
    uint32_t num_scanouts;
    uint32_t num_capsets;
    struct virtio_gpu_output *outputs; /* num_scanouts of them */
    uint64_t fence_context;
    uint64_t fence_seq; /* the last fence id given */
    uint32_t *res_ids;  /* a bit for each resource id in use, id 1 at bit 0 */
    struct spinlock lock;
    struct virtio_gpu_cmd *pending[2], *pending_tail[2]; /* by queue */
    struct virtio_gpu_cmd *done_list;
    struct virtio_gpu_cmd *live;
    volatile int display_pending;
    int dead;          /* reset or unplugged: no more commands */
    uint32_t errors;   /* answers that were errors */
    uint32_t commands; /* commands sent */
};

/*
 * A buffer object of virtio-gpu: the shmem helper's pages and the host's
 * resource made over them. `res_id` is 0 until the host has the resource.
 * `dumb` marks a dumb buffer, whose pixels a scanout copies to the host
 * before each flush.
 */
struct virtio_gpu_object {
    struct drm_gem_shmem_object shmem;
    uint32_t res_id;
    uint32_t format; /* VIRTIO_GPU_FORMAT_* */
    uint32_t width, height;
    int dumb;
};

#endif /* _VIRTIO_GPU_H_ */
