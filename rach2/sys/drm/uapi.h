/*
 * uapi.h: Linux's interface to its display and rendering devices, the
 * numbers and structures of the DRM, KMS, dma-buf, sync file, sw_sync and
 * udmabuf ioctls, as Linux 6.12's uapi headers give them.
 */
#ifndef _DRM_UAPI_H_
#define _DRM_UAPI_H_

#include "../lib/kint.h"
#include "../lib/kdef.h"
#include "../lib/kuser.h"

/*
 * The names and layouts are Linux's, from include/uapi/drm/drm.h,
 * drm_mode.h and drm_fourcc.h and include/uapi/linux/dma-buf.h, sync_file.h
 * and udmabuf.h of Linux 6.12, the kernel Debian 13 ships, against which
 * Mesa 25.0.7, libdrm and IGT 2.0 build. A structure is laid out by the
 * kernel's own ABI, so most have one layout on i686 and x86-64; those with
 * a pointer, a `size_t` or a `long`, and those whose 64-bit member follows
 * an odd number of 32-bit ones (`drm_mode_fb_cmd2`, `drm_update_draw`),
 * differ, and their ioctl numbers with them, as they do on Linux.
 * tests/host/drm_abi_hosttest.c holds every size and offset, and every
 * number, against Linux's on both ABIs.
 *
 * Linux's `long` and `unsigned long` are written `intptr_t` and `uintptr_t`,
 * which have their width on both ABIs. The DRI1 calls that Linux 6.12 no
 * longer serves (maps, buffers, contexts, locks, AGP and scatter-gather)
 * are left out, with their structures; their numbers fall to the unknown
 * ioctl's EINVAL, as on Linux.
 */

/*
 * The ioctl number: direction, size, type and number, Linux's
 * asm-generic encoding, which x86 uses: 2 bits of direction from bit 30, 14
 * of size from bit 16, 8 of type from bit 8 and 8 of number.
 */
#define UAPI_IOC_NONE  0u
#define UAPI_IOC_WRITE 1u
#define UAPI_IOC_READ  2u
#define UAPI_IOC(dir, type, nr, size)                                                          \
    ((uint32_t)(((uint32_t)(dir) << 30) | ((uint32_t)(size) << 16) | ((uint32_t)(type) << 8) | \
                (uint32_t)(nr)))
#define UAPI_IO(type, nr)      UAPI_IOC(UAPI_IOC_NONE, (type), (nr), 0)
#define UAPI_IOR(type, nr, t)  UAPI_IOC(UAPI_IOC_READ, (type), (nr), sizeof(t))
#define UAPI_IOW(type, nr, t)  UAPI_IOC(UAPI_IOC_WRITE, (type), (nr), sizeof(t))
#define UAPI_IOWR(type, nr, t) UAPI_IOC(UAPI_IOC_READ | UAPI_IOC_WRITE, (type), (nr), sizeof(t))
#define UAPI_IOC_DIR(request)  (((uint32_t)(request) >> 30) & 3u)
#define UAPI_IOC_SIZE(request) (((uint32_t)(request) >> 16) & 0x3FFFu)
#define UAPI_IOC_TYPE(request) (((uint32_t)(request) >> 8) & 0xFFu)
#define UAPI_IOC_NR(request)   ((uint32_t)(request) & 0xFFu)

/* ------------------------------------------------------------------------
 * DRM core: drm.h
 */

typedef unsigned int drm_handle_t;
typedef unsigned int drm_context_t;
typedef unsigned int drm_drawable_t;
typedef unsigned int drm_magic_t;

/* A rectangle of a framebuffer, for DIRTYFB: x2 and y2 are exclusive. */
struct drm_clip_rect {
    unsigned short x1;
    unsigned short y1;
    unsigned short x2;
    unsigned short y2;
};

/* DRM_IOCTL_VERSION: the driver's version and the lengths of its three
 * strings; each string is copied up to its `_len`, which the call then sets
 * to the string's whole length. */
struct drm_version {
    int version_major;
    int version_minor;
    int version_patchlevel;
    size_t name_len;
    char __user *name;
    size_t date_len;
    char __user *date;
    size_t desc_len;
    char __user *desc;
};

/* DRM_IOCTL_GET_UNIQUE and SET_UNIQUE: the bus name of the device. */
struct drm_unique {
    size_t unique_len;
    char __user *unique;
};

/* DRM_IOCTL_BLOCK and UNBLOCK, which do nothing. */
struct drm_block {
    int unused;
};

/* DRM_IOCTL_GET_CLIENT: client `idx`'s record, which Linux 6.12 fills only
 * for the caller itself, at index 0. */
struct drm_client {
    int idx;
    int auth;
    uintptr_t pid;
    uintptr_t uid;
    uintptr_t magic;
    uintptr_t iocs;
};

enum drm_stat_type {
    _DRM_STAT_LOCK,
    _DRM_STAT_OPENS,
    _DRM_STAT_CLOSES,
    _DRM_STAT_IOCTLS,
    _DRM_STAT_LOCKS,
    _DRM_STAT_UNLOCKS,
    _DRM_STAT_VALUE,
    _DRM_STAT_BYTE,
    _DRM_STAT_COUNT,
    _DRM_STAT_IRQ,
    _DRM_STAT_PRIMARY,
    _DRM_STAT_SECONDARY,
    _DRM_STAT_DMA,
    _DRM_STAT_SPECIAL,
    _DRM_STAT_MISSED
};

/* DRM_IOCTL_GET_STATS, which Linux 6.12 answers with a count of 0. */
struct drm_stats {
    uintptr_t count;
    struct {
        uintptr_t value;
        enum drm_stat_type type;
    } data[15];
};

enum drm_lock_flags {
    _DRM_LOCK_READY = 0x01,
    _DRM_LOCK_QUIESCENT = 0x02,
    _DRM_LOCK_FLUSH = 0x04,
    _DRM_LOCK_FLUSH_ALL = 0x08,
    _DRM_HALT_ALL_QUEUES = 0x10,
    _DRM_HALT_CUR_QUEUES = 0x20
};

/* DRM_IOCTL_FINISH's argument, which it ignores. */
struct drm_lock {
    int context;
    enum drm_lock_flags flags;
};

/* DRM_IOCTL_ADD_DRAW and RM_DRAW, which do nothing. */
struct drm_draw {
    drm_drawable_t handle;
};

/* DRM_IOCTL_UPDATE_DRAW, which does nothing; `data` is a user pointer. Its
 * 64-bit member after three 32-bit ones makes it 20 bytes on i686 and 24
 * on x86-64. */
struct drm_update_draw {
    drm_drawable_t handle;
    unsigned int type;
    unsigned int num;
    uint64_t data;
};

/* DRM_IOCTL_GET_MAGIC and AUTH_MAGIC: a client's token, which the master
 * authenticates. */
struct drm_auth {
    drm_magic_t magic;
};

/* The request of DRM_IOCTL_WAIT_VBLANK: the low bits of `type` are
 * absolute or relative, the high CRTC index sits at bits 1 to 5, and the
 * flags above ask for an event in place of a wait. */
enum drm_vblank_seq_type {
    _DRM_VBLANK_ABSOLUTE = 0x0,
    _DRM_VBLANK_RELATIVE = 0x1,
    _DRM_VBLANK_HIGH_CRTC_MASK = 0x0000003e,
    _DRM_VBLANK_EVENT = 0x4000000,
    _DRM_VBLANK_FLIP = 0x8000000,
    _DRM_VBLANK_NEXTONMISS = 0x10000000,
    _DRM_VBLANK_SECONDARY = 0x20000000,
    _DRM_VBLANK_SIGNAL = 0x40000000
};
#define _DRM_VBLANK_HIGH_CRTC_SHIFT 1
#define _DRM_VBLANK_TYPES_MASK      (_DRM_VBLANK_ABSOLUTE | _DRM_VBLANK_RELATIVE)
#define _DRM_VBLANK_FLAGS_MASK \
    (_DRM_VBLANK_EVENT | _DRM_VBLANK_SIGNAL | _DRM_VBLANK_SECONDARY | _DRM_VBLANK_NEXTONMISS)

struct drm_wait_vblank_request {
    enum drm_vblank_seq_type type;
    unsigned int sequence;
    uintptr_t signal;
};

/* The reply: the count reached and its time, in seconds and microseconds
 * of CLOCK_MONOTONIC. */
struct drm_wait_vblank_reply {
    enum drm_vblank_seq_type type;
    unsigned int sequence;
    intptr_t tval_sec;
    intptr_t tval_usec;
};

union drm_wait_vblank {
    struct drm_wait_vblank_request request;
    struct drm_wait_vblank_reply reply;
};

/* DRM_IOCTL_SET_VERSION: the interface versions a client asks for; -1 in a
 * field asks nothing, and the call answers the versions in force. */
struct drm_set_version {
    int drm_di_major;
    int drm_di_minor;
    int drm_dd_major;
    int drm_dd_minor;
};

/* DRM_IOCTL_GEM_CLOSE: drop the caller's handle of a buffer object. */
struct drm_gem_close {
    uint32_t handle;
    uint32_t pad;
};

/* DRM_IOCTL_GEM_FLINK: give a buffer a global name. */
struct drm_gem_flink {
    uint32_t handle;
    uint32_t name;
};

/* DRM_IOCTL_GEM_OPEN: a handle and the size of the buffer of a name. */
struct drm_gem_open {
    uint32_t name;
    uint32_t handle;
    uint64_t size;
};

/* DRM_IOCTL_GET_CAP's capabilities. */
#define DRM_CAP_DUMB_BUFFER            0x1
#define DRM_CAP_VBLANK_HIGH_CRTC       0x2
#define DRM_CAP_DUMB_PREFERRED_DEPTH   0x3
#define DRM_CAP_DUMB_PREFER_SHADOW     0x4
#define DRM_CAP_PRIME                  0x5
#define DRM_PRIME_CAP_IMPORT           0x1
#define DRM_PRIME_CAP_EXPORT           0x2
#define DRM_CAP_TIMESTAMP_MONOTONIC    0x6
#define DRM_CAP_ASYNC_PAGE_FLIP        0x7
#define DRM_CAP_CURSOR_WIDTH           0x8
#define DRM_CAP_CURSOR_HEIGHT          0x9
#define DRM_CAP_ADDFB2_MODIFIERS       0x10
#define DRM_CAP_PAGE_FLIP_TARGET       0x11
#define DRM_CAP_CRTC_IN_VBLANK_EVENT   0x12
#define DRM_CAP_SYNCOBJ                0x13
#define DRM_CAP_SYNCOBJ_TIMELINE       0x14
#define DRM_CAP_ATOMIC_ASYNC_PAGE_FLIP 0x15

struct drm_get_cap {
    uint64_t capability;
    uint64_t value;
};

/* DRM_IOCTL_SET_CLIENT_CAP's capabilities. */
#define DRM_CLIENT_CAP_STEREO_3D            1
#define DRM_CLIENT_CAP_UNIVERSAL_PLANES     2
#define DRM_CLIENT_CAP_ATOMIC               3
#define DRM_CLIENT_CAP_ASPECT_RATIO         4
#define DRM_CLIENT_CAP_WRITEBACK_CONNECTORS 5
#define DRM_CLIENT_CAP_CURSOR_PLANE_HOTSPOT 6

struct drm_set_client_cap {
    uint64_t capability;
    uint64_t value;
};

/* PRIME_HANDLE_TO_FD's flags, Linux's O_RDWR and O_CLOEXEC. */
#define DRM_RDWR    02
#define DRM_CLOEXEC 02000000

/* DRM_IOCTL_PRIME_HANDLE_TO_FD and FD_TO_HANDLE: a buffer's handle and a
 * dma-buf descriptor of it. */
struct drm_prime_handle {
    uint32_t handle;
    uint32_t flags;
    int32_t fd;
};

#define DRM_SYNCOBJ_CREATE_SIGNALED (1 << 0)

struct drm_syncobj_create {
    uint32_t handle;
    uint32_t flags;
};

struct drm_syncobj_destroy {
    uint32_t handle;
    uint32_t pad;
};

#define DRM_SYNCOBJ_FD_TO_HANDLE_FLAGS_IMPORT_SYNC_FILE (1 << 0)
#define DRM_SYNCOBJ_HANDLE_TO_FD_FLAGS_EXPORT_SYNC_FILE (1 << 0)

struct drm_syncobj_handle {
    uint32_t handle;
    uint32_t flags;
    int32_t fd;
    uint32_t pad;
};

struct drm_syncobj_transfer {
    uint32_t src_handle;
    uint32_t dst_handle;
    uint64_t src_point;
    uint64_t dst_point;
    uint32_t flags;
    uint32_t pad;
};

#define DRM_SYNCOBJ_WAIT_FLAGS_WAIT_ALL        (1 << 0)
#define DRM_SYNCOBJ_WAIT_FLAGS_WAIT_FOR_SUBMIT (1 << 1)
#define DRM_SYNCOBJ_WAIT_FLAGS_WAIT_AVAILABLE  (1 << 2)
#define DRM_SYNCOBJ_WAIT_FLAGS_WAIT_DEADLINE   (1 << 3)

/* SYNCOBJ_WAIT: `handles` is a user pointer to `count_handles` handles;
 * the timeout is an absolute CLOCK_MONOTONIC time. */
struct drm_syncobj_wait {
    uint64_t handles;
    int64_t timeout_nsec;
    uint32_t count_handles;
    uint32_t flags;
    uint32_t first_signaled;
    uint32_t pad;
    uint64_t deadline_nsec;
};

struct drm_syncobj_timeline_wait {
    uint64_t handles;
    uint64_t points;
    int64_t timeout_nsec;
    uint32_t count_handles;
    uint32_t flags;
    uint32_t first_signaled;
    uint32_t pad;
    uint64_t deadline_nsec;
};

/* SYNCOBJ_EVENTFD: signal the eventfd `fd` when `point` arrives. */
struct drm_syncobj_eventfd {
    uint32_t handle;
    uint32_t flags;
    uint64_t point;
    int32_t fd;
    uint32_t pad;
};

struct drm_syncobj_array {
    uint64_t handles;
    uint32_t count_handles;
    uint32_t pad;
};

#define DRM_SYNCOBJ_QUERY_FLAGS_LAST_SUBMITTED (1 << 0)

struct drm_syncobj_timeline_array {
    uint64_t handles;
    uint64_t points;
    uint32_t count_handles;
    uint32_t flags;
};

/* CRTC_GET_SEQUENCE: the CRTC's vblank count and the time of the last. */
struct drm_crtc_get_sequence {
    uint32_t crtc_id;
    uint32_t active;
    uint64_t sequence;
    int64_t sequence_ns;
};

#define DRM_CRTC_SEQUENCE_RELATIVE     0x00000001
#define DRM_CRTC_SEQUENCE_NEXT_ON_MISS 0x00000002

/* CRTC_QUEUE_SEQUENCE: an event at a vblank count. */
struct drm_crtc_queue_sequence {
    uint32_t crtc_id;
    uint32_t flags;
    uint64_t sequence;
    uint64_t user_data;
};

/* An event read from the card's descriptor: a header, then the body of
 * its type, `length` bytes in all. */
struct drm_event {
    uint32_t type;
    uint32_t length;
};

#define DRM_EVENT_VBLANK        0x01
#define DRM_EVENT_FLIP_COMPLETE 0x02
#define DRM_EVENT_CRTC_SEQUENCE 0x03

/* DRM_EVENT_VBLANK and FLIP_COMPLETE: the time is CLOCK_MONOTONIC's. */
struct drm_event_vblank {
    struct drm_event base;
    uint64_t user_data;
    uint32_t tv_sec;
    uint32_t tv_usec;
    uint32_t sequence;
    uint32_t crtc_id;
};

struct drm_event_crtc_sequence {
    struct drm_event base;
    uint64_t user_data;
    int64_t time_ns;
    uint64_t sequence;
};

/* ------------------------------------------------------------------------
 * Mode setting: drm_mode.h
 */

#define DRM_CONNECTOR_NAME_LEN 32
#define DRM_DISPLAY_MODE_LEN   32
#define DRM_PROP_NAME_LEN      32

#define DRM_MODE_TYPE_BUILTIN   (1 << 0)
#define DRM_MODE_TYPE_CLOCK_C   ((1 << 1) | DRM_MODE_TYPE_BUILTIN)
#define DRM_MODE_TYPE_CRTC_C    ((1 << 2) | DRM_MODE_TYPE_BUILTIN)
#define DRM_MODE_TYPE_PREFERRED (1 << 3)
#define DRM_MODE_TYPE_DEFAULT   (1 << 4)
#define DRM_MODE_TYPE_USERDEF   (1 << 5)
#define DRM_MODE_TYPE_DRIVER    (1 << 6)
#define DRM_MODE_TYPE_ALL       (DRM_MODE_TYPE_PREFERRED | DRM_MODE_TYPE_USERDEF | DRM_MODE_TYPE_DRIVER)

#define DRM_MODE_FLAG_PHSYNC                   (1 << 0)
#define DRM_MODE_FLAG_NHSYNC                   (1 << 1)
#define DRM_MODE_FLAG_PVSYNC                   (1 << 2)
#define DRM_MODE_FLAG_NVSYNC                   (1 << 3)
#define DRM_MODE_FLAG_INTERLACE                (1 << 4)
#define DRM_MODE_FLAG_DBLSCAN                  (1 << 5)
#define DRM_MODE_FLAG_CSYNC                    (1 << 6)
#define DRM_MODE_FLAG_PCSYNC                   (1 << 7)
#define DRM_MODE_FLAG_NCSYNC                   (1 << 8)
#define DRM_MODE_FLAG_HSKEW                    (1 << 9)
#define DRM_MODE_FLAG_BCAST                    (1 << 10)
#define DRM_MODE_FLAG_PIXMUX                   (1 << 11)
#define DRM_MODE_FLAG_DBLCLK                   (1 << 12)
#define DRM_MODE_FLAG_CLKDIV2                  (1 << 13)
#define DRM_MODE_FLAG_3D_MASK                  (0x1f << 14)
#define DRM_MODE_FLAG_3D_NONE                  (0 << 14)
#define DRM_MODE_FLAG_3D_FRAME_PACKING         (1 << 14)
#define DRM_MODE_FLAG_3D_FIELD_ALTERNATIVE     (2 << 14)
#define DRM_MODE_FLAG_3D_LINE_ALTERNATIVE      (3 << 14)
#define DRM_MODE_FLAG_3D_SIDE_BY_SIDE_FULL     (4 << 14)
#define DRM_MODE_FLAG_3D_L_DEPTH               (5 << 14)
#define DRM_MODE_FLAG_3D_L_DEPTH_GFX_GFX_DEPTH (6 << 14)
#define DRM_MODE_FLAG_3D_TOP_AND_BOTTOM        (7 << 14)
#define DRM_MODE_FLAG_3D_SIDE_BY_SIDE_HALF     (8 << 14)

#define DRM_MODE_PICTURE_ASPECT_NONE    0
#define DRM_MODE_PICTURE_ASPECT_4_3     1
#define DRM_MODE_PICTURE_ASPECT_16_9    2
#define DRM_MODE_PICTURE_ASPECT_64_27   3
#define DRM_MODE_PICTURE_ASPECT_256_135 4

#define DRM_MODE_CONTENT_TYPE_NO_DATA  0
#define DRM_MODE_CONTENT_TYPE_GRAPHICS 1
#define DRM_MODE_CONTENT_TYPE_PHOTO    2
#define DRM_MODE_CONTENT_TYPE_CINEMA   3
#define DRM_MODE_CONTENT_TYPE_GAME     4

#define DRM_MODE_FLAG_PIC_AR_MASK    (0x0F << 19)
#define DRM_MODE_FLAG_PIC_AR_NONE    (DRM_MODE_PICTURE_ASPECT_NONE << 19)
#define DRM_MODE_FLAG_PIC_AR_4_3     (DRM_MODE_PICTURE_ASPECT_4_3 << 19)
#define DRM_MODE_FLAG_PIC_AR_16_9    (DRM_MODE_PICTURE_ASPECT_16_9 << 19)
#define DRM_MODE_FLAG_PIC_AR_64_27   (DRM_MODE_PICTURE_ASPECT_64_27 << 19)
#define DRM_MODE_FLAG_PIC_AR_256_135 (DRM_MODE_PICTURE_ASPECT_256_135 << 19)

#define DRM_MODE_FLAG_ALL                                                                        \
    (DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_NHSYNC | DRM_MODE_FLAG_PVSYNC | DRM_MODE_FLAG_NVSYNC | \
     DRM_MODE_FLAG_INTERLACE | DRM_MODE_FLAG_DBLSCAN | DRM_MODE_FLAG_CSYNC |                     \
     DRM_MODE_FLAG_PCSYNC | DRM_MODE_FLAG_NCSYNC | DRM_MODE_FLAG_HSKEW | DRM_MODE_FLAG_DBLCLK |  \
     DRM_MODE_FLAG_CLKDIV2 | DRM_MODE_FLAG_3D_MASK)

#define DRM_MODE_DPMS_ON      0
#define DRM_MODE_DPMS_STANDBY 1
#define DRM_MODE_DPMS_SUSPEND 2
#define DRM_MODE_DPMS_OFF     3

#define DRM_MODE_SCALE_NONE       0
#define DRM_MODE_SCALE_FULLSCREEN 1
#define DRM_MODE_SCALE_CENTER     2
#define DRM_MODE_SCALE_ASPECT     3

#define DRM_MODE_DITHERING_OFF  0
#define DRM_MODE_DITHERING_ON   1
#define DRM_MODE_DITHERING_AUTO 2

#define DRM_MODE_DIRTY_OFF      0
#define DRM_MODE_DIRTY_ON       1
#define DRM_MODE_DIRTY_ANNOTATE 2

#define DRM_MODE_LINK_STATUS_GOOD 0
#define DRM_MODE_LINK_STATUS_BAD  1

#define DRM_MODE_ROTATE_0   (1 << 0)
#define DRM_MODE_ROTATE_90  (1 << 1)
#define DRM_MODE_ROTATE_180 (1 << 2)
#define DRM_MODE_ROTATE_270 (1 << 3)
#define DRM_MODE_ROTATE_MASK \
    (DRM_MODE_ROTATE_0 | DRM_MODE_ROTATE_90 | DRM_MODE_ROTATE_180 | DRM_MODE_ROTATE_270)
#define DRM_MODE_REFLECT_X    (1 << 4)
#define DRM_MODE_REFLECT_Y    (1 << 5)
#define DRM_MODE_REFLECT_MASK (DRM_MODE_REFLECT_X | DRM_MODE_REFLECT_Y)

#define DRM_MODE_CONTENT_PROTECTION_UNDESIRED 0
#define DRM_MODE_CONTENT_PROTECTION_DESIRED   1
#define DRM_MODE_CONTENT_PROTECTION_ENABLED   2

/* A display mode: the pixel clock in kHz and the horizontal and vertical
 * timings in pixels and lines. */
struct drm_mode_modeinfo {
    uint32_t clock;
    uint16_t hdisplay;
    uint16_t hsync_start;
    uint16_t hsync_end;
    uint16_t htotal;
    uint16_t hskew;
    uint16_t vdisplay;
    uint16_t vsync_start;
    uint16_t vsync_end;
    uint16_t vtotal;
    uint16_t vscan;
    uint32_t vrefresh;
    uint32_t flags;
    uint32_t type;
    char name[DRM_DISPLAY_MODE_LEN];
};

/* MODE_GETRESOURCES: each `_ptr` is a user pointer to an array of IDs, of
 * the length its count gives, which the call fills as far as it reaches and
 * then sets to the number there are. */
struct drm_mode_card_res {
    uint64_t fb_id_ptr;
    uint64_t crtc_id_ptr;
    uint64_t connector_id_ptr;
    uint64_t encoder_id_ptr;
    uint32_t count_fbs;
    uint32_t count_crtcs;
    uint32_t count_connectors;
    uint32_t count_encoders;
    uint32_t min_width;
    uint32_t max_width;
    uint32_t min_height;
    uint32_t max_height;
};

/* MODE_GETCRTC and SETCRTC. */
struct drm_mode_crtc {
    uint64_t set_connectors_ptr;
    uint32_t count_connectors;
    uint32_t crtc_id;
    uint32_t fb_id;
    uint32_t x;
    uint32_t y;
    uint32_t gamma_size;
    uint32_t mode_valid;
    struct drm_mode_modeinfo mode;
};

#define DRM_MODE_PRESENT_TOP_FIELD    (1 << 0)
#define DRM_MODE_PRESENT_BOTTOM_FIELD (1 << 1)

/* MODE_SETPLANE: the source rectangle is in 16.16 fixed point. */
struct drm_mode_set_plane {
    uint32_t plane_id;
    uint32_t crtc_id;
    uint32_t fb_id;
    uint32_t flags;
    int32_t crtc_x;
    int32_t crtc_y;
    uint32_t crtc_w;
    uint32_t crtc_h;
    uint32_t src_x;
    uint32_t src_y;
    uint32_t src_h;
    uint32_t src_w;
};

struct drm_mode_get_plane {
    uint32_t plane_id;
    uint32_t crtc_id;
    uint32_t fb_id;
    uint32_t possible_crtcs;
    uint32_t gamma_size;
    uint32_t count_format_types;
    uint64_t format_type_ptr;
};

struct drm_mode_get_plane_res {
    uint64_t plane_id_ptr;
    uint32_t count_planes;
};

#define DRM_MODE_ENCODER_NONE    0
#define DRM_MODE_ENCODER_DAC     1
#define DRM_MODE_ENCODER_TMDS    2
#define DRM_MODE_ENCODER_LVDS    3
#define DRM_MODE_ENCODER_TVDAC   4
#define DRM_MODE_ENCODER_VIRTUAL 5
#define DRM_MODE_ENCODER_DSI     6
#define DRM_MODE_ENCODER_DPMST   7
#define DRM_MODE_ENCODER_DPI     8

struct drm_mode_get_encoder {
    uint32_t encoder_id;
    uint32_t encoder_type;
    uint32_t crtc_id;
    uint32_t possible_crtcs;
    uint32_t possible_clones;
};

enum drm_mode_subconnector {
    DRM_MODE_SUBCONNECTOR_Automatic = 0,
    DRM_MODE_SUBCONNECTOR_Unknown = 0,
    DRM_MODE_SUBCONNECTOR_VGA = 1,
    DRM_MODE_SUBCONNECTOR_DVID = 3,
    DRM_MODE_SUBCONNECTOR_DVIA = 4,
    DRM_MODE_SUBCONNECTOR_Composite = 5,
    DRM_MODE_SUBCONNECTOR_SVIDEO = 6,
    DRM_MODE_SUBCONNECTOR_Component = 8,
    DRM_MODE_SUBCONNECTOR_SCART = 9,
    DRM_MODE_SUBCONNECTOR_DisplayPort = 10,
    DRM_MODE_SUBCONNECTOR_HDMIA = 11,
    DRM_MODE_SUBCONNECTOR_Native = 15,
    DRM_MODE_SUBCONNECTOR_Wireless = 18
};

#define DRM_MODE_CONNECTOR_Unknown     0
#define DRM_MODE_CONNECTOR_VGA         1
#define DRM_MODE_CONNECTOR_DVII        2
#define DRM_MODE_CONNECTOR_DVID        3
#define DRM_MODE_CONNECTOR_DVIA        4
#define DRM_MODE_CONNECTOR_Composite   5
#define DRM_MODE_CONNECTOR_SVIDEO      6
#define DRM_MODE_CONNECTOR_LVDS        7
#define DRM_MODE_CONNECTOR_Component   8
#define DRM_MODE_CONNECTOR_9PinDIN     9
#define DRM_MODE_CONNECTOR_DisplayPort 10
#define DRM_MODE_CONNECTOR_HDMIA       11
#define DRM_MODE_CONNECTOR_HDMIB       12
#define DRM_MODE_CONNECTOR_TV          13
#define DRM_MODE_CONNECTOR_eDP         14
#define DRM_MODE_CONNECTOR_VIRTUAL     15
#define DRM_MODE_CONNECTOR_DSI         16
#define DRM_MODE_CONNECTOR_DPI         17
#define DRM_MODE_CONNECTOR_WRITEBACK   18
#define DRM_MODE_CONNECTOR_SPI         19
#define DRM_MODE_CONNECTOR_USB         20

/* MODE_GETCONNECTOR: `connection` is 1 connected, 2 disconnected, 3
 * unknown. */
struct drm_mode_get_connector {
    uint64_t encoders_ptr;
    uint64_t modes_ptr;
    uint64_t props_ptr;
    uint64_t prop_values_ptr;
    uint32_t count_modes;
    uint32_t count_props;
    uint32_t count_encoders;
    uint32_t encoder_id;
    uint32_t connector_id;
    uint32_t connector_type;
    uint32_t connector_type_id;
    uint32_t connection;
    uint32_t mm_width;
    uint32_t mm_height;
    uint32_t subpixel;
    uint32_t pad;
};

#define DRM_MODE_PROP_PENDING   (1 << 0)
#define DRM_MODE_PROP_RANGE     (1 << 1)
#define DRM_MODE_PROP_IMMUTABLE (1 << 2)
#define DRM_MODE_PROP_ENUM      (1 << 3)
#define DRM_MODE_PROP_BLOB      (1 << 4)
#define DRM_MODE_PROP_BITMASK   (1 << 5)
#define DRM_MODE_PROP_LEGACY_TYPE \
    (DRM_MODE_PROP_RANGE | DRM_MODE_PROP_ENUM | DRM_MODE_PROP_BLOB | DRM_MODE_PROP_BITMASK)
#define DRM_MODE_PROP_EXTENDED_TYPE 0x0000ffc0
#define DRM_MODE_PROP_TYPE(n)       ((n) << 6)
#define DRM_MODE_PROP_OBJECT        DRM_MODE_PROP_TYPE(1)
#define DRM_MODE_PROP_SIGNED_RANGE  DRM_MODE_PROP_TYPE(2)
#define DRM_MODE_PROP_ATOMIC        0x80000000

struct drm_mode_property_enum {
    uint64_t value;
    char name[DRM_PROP_NAME_LEN];
};

struct drm_mode_get_property {
    uint64_t values_ptr;
    uint64_t enum_blob_ptr;
    uint32_t prop_id;
    uint32_t flags;
    char name[DRM_PROP_NAME_LEN];
    uint32_t count_values;
    uint32_t count_enum_blobs;
};

struct drm_mode_connector_set_property {
    uint64_t value;
    uint32_t prop_id;
    uint32_t connector_id;
};

#define DRM_MODE_OBJECT_CRTC      0xcccccccc
#define DRM_MODE_OBJECT_CONNECTOR 0xc0c0c0c0
#define DRM_MODE_OBJECT_ENCODER   0xe0e0e0e0
#define DRM_MODE_OBJECT_MODE      0xdededede
#define DRM_MODE_OBJECT_PROPERTY  0xb0b0b0b0
#define DRM_MODE_OBJECT_FB        0xfbfbfbfb
#define DRM_MODE_OBJECT_BLOB      0xbbbbbbbb
#define DRM_MODE_OBJECT_PLANE     0xeeeeeeee
#define DRM_MODE_OBJECT_ANY       0

struct drm_mode_obj_get_properties {
    uint64_t props_ptr;
    uint64_t prop_values_ptr;
    uint32_t count_props;
    uint32_t obj_id;
    uint32_t obj_type;
};

struct drm_mode_obj_set_property {
    uint64_t value;
    uint32_t prop_id;
    uint32_t obj_id;
    uint32_t obj_type;
};

struct drm_mode_get_blob {
    uint32_t blob_id;
    uint32_t length;
    uint64_t data;
};

/* MODE_ADDFB and GETFB: one plane, by bits per pixel and depth. */
struct drm_mode_fb_cmd {
    uint32_t fb_id;
    uint32_t width;
    uint32_t height;
    uint32_t pitch;
    uint32_t bpp;
    uint32_t depth;
    uint32_t handle;
};

#define DRM_MODE_FB_INTERLACED (1 << 0)
#define DRM_MODE_FB_MODIFIERS  (1 << 1)

/* MODE_ADDFB2 and GETFB2: up to four planes, by fourcc format. The
 * modifiers start at 68 on i686 and at 72 on x86-64. */
struct drm_mode_fb_cmd2 {
    uint32_t fb_id;
    uint32_t width;
    uint32_t height;
    uint32_t pixel_format;
    uint32_t flags;
    uint32_t handles[4];
    uint32_t pitches[4];
    uint32_t offsets[4];
    uint64_t modifier[4];
};

#define DRM_MODE_FB_DIRTY_ANNOTATE_COPY 0x01
#define DRM_MODE_FB_DIRTY_ANNOTATE_FILL 0x02
#define DRM_MODE_FB_DIRTY_FLAGS         0x03
#define DRM_MODE_FB_DIRTY_MAX_CLIPS     256

/* MODE_DIRTYFB: `clips_ptr` points at `num_clips` drm_clip_rect. */
struct drm_mode_fb_dirty_cmd {
    uint32_t fb_id;
    uint32_t flags;
    uint32_t color;
    uint32_t num_clips;
    uint64_t clips_ptr;
};

/* MODE_ATTACHMODE and DETACHMODE, which do nothing. */
struct drm_mode_mode_cmd {
    uint32_t connector_id;
    struct drm_mode_modeinfo mode;
};

#define DRM_MODE_CURSOR_BO    0x01
#define DRM_MODE_CURSOR_MOVE  0x02
#define DRM_MODE_CURSOR_FLAGS 0x03

struct drm_mode_cursor {
    uint32_t flags;
    uint32_t crtc_id;
    int32_t x;
    int32_t y;
    uint32_t width;
    uint32_t height;
    uint32_t handle;
};

struct drm_mode_cursor2 {
    uint32_t flags;
    uint32_t crtc_id;
    int32_t x;
    int32_t y;
    uint32_t width;
    uint32_t height;
    uint32_t handle;
    int32_t hot_x;
    int32_t hot_y;
};

/* MODE_GETGAMMA and SETGAMMA: three user pointers to `gamma_size` 16-bit
 * entries each. */
struct drm_mode_crtc_lut {
    uint32_t crtc_id;
    uint32_t gamma_size;
    uint64_t red;
    uint64_t green;
    uint64_t blue;
};

/* The CTM property's blob: S31.32 sign-magnitude, row-major. */
struct drm_color_ctm {
    uint64_t matrix[9];
};

/* An entry of the GAMMA_LUT and DEGAMMA_LUT properties' blobs. */
struct drm_color_lut {
    uint16_t red;
    uint16_t green;
    uint16_t blue;
    uint16_t reserved;
};

/* An entry of a cursor plane's SIZE_HINTS blob. */
struct drm_plane_size_hint {
    uint16_t width;
    uint16_t height;
};

/* HDR_OUTPUT_METADATA's blob, CTA-861-G's static metadata. */
struct hdr_metadata_infoframe {
    uint8_t eotf;
    uint8_t metadata_type;
    struct {
        uint16_t x, y;
    } display_primaries[3];
    struct {
        uint16_t x, y;
    } white_point;
    uint16_t max_display_mastering_luminance;
    uint16_t min_display_mastering_luminance;
    uint16_t max_cll;
    uint16_t max_fall;
};

/* Linux puts the infoframe in an anonymous union of one member, which C89
 * cannot name without a name; the layout is the same. */
struct hdr_output_metadata {
    uint32_t metadata_type;
    struct hdr_metadata_infoframe hdmi_metadata_type1;
};

#define DRM_MODE_PAGE_FLIP_EVENT           0x01
#define DRM_MODE_PAGE_FLIP_ASYNC           0x02
#define DRM_MODE_PAGE_FLIP_TARGET_ABSOLUTE 0x4
#define DRM_MODE_PAGE_FLIP_TARGET_RELATIVE 0x8
#define DRM_MODE_PAGE_FLIP_TARGET \
    (DRM_MODE_PAGE_FLIP_TARGET_ABSOLUTE | DRM_MODE_PAGE_FLIP_TARGET_RELATIVE)
#define DRM_MODE_PAGE_FLIP_FLAGS \
    (DRM_MODE_PAGE_FLIP_EVENT | DRM_MODE_PAGE_FLIP_ASYNC | DRM_MODE_PAGE_FLIP_TARGET)

struct drm_mode_crtc_page_flip {
    uint32_t crtc_id;
    uint32_t fb_id;
    uint32_t flags;
    uint32_t reserved;
    uint64_t user_data;
};

/* PAGE_FLIP with a target flag reads `reserved` as the target vblank. */
struct drm_mode_crtc_page_flip_target {
    uint32_t crtc_id;
    uint32_t fb_id;
    uint32_t flags;
    uint32_t sequence;
    uint64_t user_data;
};

/* MODE_CREATE_DUMB: the call sets handle, pitch and size. */
struct drm_mode_create_dumb {
    uint32_t height;
    uint32_t width;
    uint32_t bpp;
    uint32_t flags;
    uint32_t handle;
    uint32_t pitch;
    uint64_t size;
};

/* MODE_MAP_DUMB: the offset to give mmap on the card's descriptor. */
struct drm_mode_map_dumb {
    uint32_t handle;
    uint32_t pad;
    uint64_t offset;
};

struct drm_mode_destroy_dumb {
    uint32_t handle;
};

#define DRM_MODE_ATOMIC_TEST_ONLY     0x0100
#define DRM_MODE_ATOMIC_NONBLOCK      0x0200
#define DRM_MODE_ATOMIC_ALLOW_MODESET 0x0400
#define DRM_MODE_ATOMIC_FLAGS                                                          \
    (DRM_MODE_PAGE_FLIP_EVENT | DRM_MODE_PAGE_FLIP_ASYNC | DRM_MODE_ATOMIC_TEST_ONLY | \
     DRM_MODE_ATOMIC_NONBLOCK | DRM_MODE_ATOMIC_ALLOW_MODESET)

/* MODE_ATOMIC: `count_objs` objects, each with its count of properties,
 * and the properties and values of all of them in order. */
struct drm_mode_atomic {
    uint32_t flags;
    uint32_t count_objs;
    uint64_t objs_ptr;
    uint64_t count_props_ptr;
    uint64_t props_ptr;
    uint64_t prop_values_ptr;
    uint64_t reserved;
    uint64_t user_data;
};

#define FORMAT_BLOB_CURRENT 1

/* The IN_FORMATS property's blob: a header, the formats, then the
 * modifiers, each with a mask of 64 formats from its `offset`. */
struct drm_format_modifier_blob {
    uint32_t version;
    uint32_t flags;
    uint32_t count_formats;
    uint32_t formats_offset;
    uint32_t count_modifiers;
    uint32_t modifiers_offset;
};

struct drm_format_modifier {
    uint64_t formats;
    uint32_t offset;
    uint32_t pad;
    uint64_t modifier;
};

struct drm_mode_create_blob {
    uint64_t data;
    uint32_t length;
    uint32_t blob_id;
};

struct drm_mode_destroy_blob {
    uint32_t blob_id;
};

/* MODE_CREATE_LEASE: the call sets lessee_id and fd. */
struct drm_mode_create_lease {
    uint64_t object_ids;
    uint32_t object_count;
    uint32_t flags;
    uint32_t lessee_id;
    uint32_t fd;
};

struct drm_mode_list_lessees {
    uint32_t count_lessees;
    uint32_t pad;
    uint64_t lessees_ptr;
};

struct drm_mode_get_lease {
    uint32_t count_objects;
    uint32_t pad;
    uint64_t objects_ptr;
};

struct drm_mode_revoke_lease {
    uint32_t lessee_id;
};

/* A rectangle of the FB_DAMAGE_CLIPS property's blob. */
struct drm_mode_rect {
    int32_t x1;
    int32_t y1;
    int32_t x2;
    int32_t y2;
};

struct drm_mode_closefb {
    uint32_t fb_id;
    uint32_t pad;
};

/* ------------------------------------------------------------------------
 * Pixel formats and modifiers: the few of drm_fourcc.h the kernel's
 * drivers name.
 */

#define fourcc_code(a, b, c, d) \
    ((uint32_t)(a) | ((uint32_t)(b) << 8) | ((uint32_t)(c) << 16) | ((uint32_t)(d) << 24))

#define DRM_FORMAT_BIG_ENDIAN  (1u << 31)
#define DRM_FORMAT_INVALID     0
#define DRM_FORMAT_C8          fourcc_code('C', '8', ' ', ' ')
#define DRM_FORMAT_RGB565      fourcc_code('R', 'G', '1', '6')
#define DRM_FORMAT_RGB888      fourcc_code('R', 'G', '2', '4')
#define DRM_FORMAT_BGR888      fourcc_code('B', 'G', '2', '4')
#define DRM_FORMAT_XRGB8888    fourcc_code('X', 'R', '2', '4')
#define DRM_FORMAT_XBGR8888    fourcc_code('X', 'B', '2', '4')
#define DRM_FORMAT_ARGB8888    fourcc_code('A', 'R', '2', '4')
#define DRM_FORMAT_ABGR8888    fourcc_code('A', 'B', '2', '4')
#define DRM_FORMAT_XRGB2101010 fourcc_code('X', 'R', '3', '0')
#define DRM_FORMAT_ARGB2101010 fourcc_code('A', 'R', '3', '0')

/* A modifier is a vendor in the top byte and the vendor's layout below. */
#define DRM_FORMAT_MOD_VENDOR_NONE 0
#define DRM_FORMAT_RESERVED        ((1ULL << 56) - 1)
#define fourcc_mod_code(vendor, val) \
    ((((uint64_t)DRM_FORMAT_MOD_VENDOR_##vendor) << 56) | ((uint64_t)(val) & 0x00ffffffffffffffULL))
#define DRM_FORMAT_MOD_INVALID fourcc_mod_code(NONE, DRM_FORMAT_RESERVED)
#define DRM_FORMAT_MOD_LINEAR  fourcc_mod_code(NONE, 0)

/* ------------------------------------------------------------------------
 * The DRM ioctl numbers, type 'd'. Numbers 0x40 to 0x9F are the driver's.
 */

#define DRM_IOCTL_BASE     'd'
#define DRM_IO(nr)         UAPI_IO(DRM_IOCTL_BASE, nr)
#define DRM_IOR(nr, type)  UAPI_IOR(DRM_IOCTL_BASE, nr, type)
#define DRM_IOW(nr, type)  UAPI_IOW(DRM_IOCTL_BASE, nr, type)
#define DRM_IOWR(nr, type) UAPI_IOWR(DRM_IOCTL_BASE, nr, type)
#define DRM_COMMAND_BASE   0x40
#define DRM_COMMAND_END    0xA0

#define DRM_IOCTL_VERSION                 DRM_IOWR(0x00, struct drm_version)
#define DRM_IOCTL_GET_UNIQUE              DRM_IOWR(0x01, struct drm_unique)
#define DRM_IOCTL_GET_MAGIC               DRM_IOR(0x02, struct drm_auth)
#define DRM_IOCTL_GET_CLIENT              DRM_IOWR(0x05, struct drm_client)
#define DRM_IOCTL_GET_STATS               DRM_IOR(0x06, struct drm_stats)
#define DRM_IOCTL_SET_VERSION             DRM_IOWR(0x07, struct drm_set_version)
#define DRM_IOCTL_GEM_CLOSE               DRM_IOW(0x09, struct drm_gem_close)
#define DRM_IOCTL_GEM_FLINK               DRM_IOWR(0x0a, struct drm_gem_flink)
#define DRM_IOCTL_GEM_OPEN                DRM_IOWR(0x0b, struct drm_gem_open)
#define DRM_IOCTL_GET_CAP                 DRM_IOWR(0x0c, struct drm_get_cap)
#define DRM_IOCTL_SET_CLIENT_CAP          DRM_IOW(0x0d, struct drm_set_client_cap)
#define DRM_IOCTL_SET_UNIQUE              DRM_IOW(0x10, struct drm_unique)
#define DRM_IOCTL_AUTH_MAGIC              DRM_IOW(0x11, struct drm_auth)
#define DRM_IOCTL_BLOCK                   DRM_IOWR(0x12, struct drm_block)
#define DRM_IOCTL_UNBLOCK                 DRM_IOWR(0x13, struct drm_block)
#define DRM_IOCTL_SET_MASTER              DRM_IO(0x1e)
#define DRM_IOCTL_DROP_MASTER             DRM_IO(0x1f)
#define DRM_IOCTL_ADD_DRAW                DRM_IOWR(0x27, struct drm_draw)
#define DRM_IOCTL_RM_DRAW                 DRM_IOWR(0x28, struct drm_draw)
#define DRM_IOCTL_FINISH                  DRM_IOW(0x2c, struct drm_lock)
#define DRM_IOCTL_PRIME_HANDLE_TO_FD      DRM_IOWR(0x2d, struct drm_prime_handle)
#define DRM_IOCTL_PRIME_FD_TO_HANDLE      DRM_IOWR(0x2e, struct drm_prime_handle)
#define DRM_IOCTL_WAIT_VBLANK             DRM_IOWR(0x3a, union drm_wait_vblank)
#define DRM_IOCTL_CRTC_GET_SEQUENCE       DRM_IOWR(0x3b, struct drm_crtc_get_sequence)
#define DRM_IOCTL_CRTC_QUEUE_SEQUENCE     DRM_IOWR(0x3c, struct drm_crtc_queue_sequence)
#define DRM_IOCTL_UPDATE_DRAW             DRM_IOW(0x3f, struct drm_update_draw)
#define DRM_IOCTL_MODE_GETRESOURCES       DRM_IOWR(0xA0, struct drm_mode_card_res)
#define DRM_IOCTL_MODE_GETCRTC            DRM_IOWR(0xA1, struct drm_mode_crtc)
#define DRM_IOCTL_MODE_SETCRTC            DRM_IOWR(0xA2, struct drm_mode_crtc)
#define DRM_IOCTL_MODE_CURSOR             DRM_IOWR(0xA3, struct drm_mode_cursor)
#define DRM_IOCTL_MODE_GETGAMMA           DRM_IOWR(0xA4, struct drm_mode_crtc_lut)
#define DRM_IOCTL_MODE_SETGAMMA           DRM_IOWR(0xA5, struct drm_mode_crtc_lut)
#define DRM_IOCTL_MODE_GETENCODER         DRM_IOWR(0xA6, struct drm_mode_get_encoder)
#define DRM_IOCTL_MODE_GETCONNECTOR       DRM_IOWR(0xA7, struct drm_mode_get_connector)
#define DRM_IOCTL_MODE_ATTACHMODE         DRM_IOWR(0xA8, struct drm_mode_mode_cmd)
#define DRM_IOCTL_MODE_DETACHMODE         DRM_IOWR(0xA9, struct drm_mode_mode_cmd)
#define DRM_IOCTL_MODE_GETPROPERTY        DRM_IOWR(0xAA, struct drm_mode_get_property)
#define DRM_IOCTL_MODE_SETPROPERTY        DRM_IOWR(0xAB, struct drm_mode_connector_set_property)
#define DRM_IOCTL_MODE_GETPROPBLOB        DRM_IOWR(0xAC, struct drm_mode_get_blob)
#define DRM_IOCTL_MODE_GETFB              DRM_IOWR(0xAD, struct drm_mode_fb_cmd)
#define DRM_IOCTL_MODE_ADDFB              DRM_IOWR(0xAE, struct drm_mode_fb_cmd)
#define DRM_IOCTL_MODE_RMFB               DRM_IOWR(0xAF, unsigned int)
#define DRM_IOCTL_MODE_PAGE_FLIP          DRM_IOWR(0xB0, struct drm_mode_crtc_page_flip)
#define DRM_IOCTL_MODE_DIRTYFB            DRM_IOWR(0xB1, struct drm_mode_fb_dirty_cmd)
#define DRM_IOCTL_MODE_CREATE_DUMB        DRM_IOWR(0xB2, struct drm_mode_create_dumb)
#define DRM_IOCTL_MODE_MAP_DUMB           DRM_IOWR(0xB3, struct drm_mode_map_dumb)
#define DRM_IOCTL_MODE_DESTROY_DUMB       DRM_IOWR(0xB4, struct drm_mode_destroy_dumb)
#define DRM_IOCTL_MODE_GETPLANERESOURCES  DRM_IOWR(0xB5, struct drm_mode_get_plane_res)
#define DRM_IOCTL_MODE_GETPLANE           DRM_IOWR(0xB6, struct drm_mode_get_plane)
#define DRM_IOCTL_MODE_SETPLANE           DRM_IOWR(0xB7, struct drm_mode_set_plane)
#define DRM_IOCTL_MODE_ADDFB2             DRM_IOWR(0xB8, struct drm_mode_fb_cmd2)
#define DRM_IOCTL_MODE_OBJ_GETPROPERTIES  DRM_IOWR(0xB9, struct drm_mode_obj_get_properties)
#define DRM_IOCTL_MODE_OBJ_SETPROPERTY    DRM_IOWR(0xBA, struct drm_mode_obj_set_property)
#define DRM_IOCTL_MODE_CURSOR2            DRM_IOWR(0xBB, struct drm_mode_cursor2)
#define DRM_IOCTL_MODE_ATOMIC             DRM_IOWR(0xBC, struct drm_mode_atomic)
#define DRM_IOCTL_MODE_CREATEPROPBLOB     DRM_IOWR(0xBD, struct drm_mode_create_blob)
#define DRM_IOCTL_MODE_DESTROYPROPBLOB    DRM_IOWR(0xBE, struct drm_mode_destroy_blob)
#define DRM_IOCTL_SYNCOBJ_CREATE          DRM_IOWR(0xBF, struct drm_syncobj_create)
#define DRM_IOCTL_SYNCOBJ_DESTROY         DRM_IOWR(0xC0, struct drm_syncobj_destroy)
#define DRM_IOCTL_SYNCOBJ_HANDLE_TO_FD    DRM_IOWR(0xC1, struct drm_syncobj_handle)
#define DRM_IOCTL_SYNCOBJ_FD_TO_HANDLE    DRM_IOWR(0xC2, struct drm_syncobj_handle)
#define DRM_IOCTL_SYNCOBJ_WAIT            DRM_IOWR(0xC3, struct drm_syncobj_wait)
#define DRM_IOCTL_SYNCOBJ_RESET           DRM_IOWR(0xC4, struct drm_syncobj_array)
#define DRM_IOCTL_SYNCOBJ_SIGNAL          DRM_IOWR(0xC5, struct drm_syncobj_array)
#define DRM_IOCTL_MODE_CREATE_LEASE       DRM_IOWR(0xC6, struct drm_mode_create_lease)
#define DRM_IOCTL_MODE_LIST_LESSEES       DRM_IOWR(0xC7, struct drm_mode_list_lessees)
#define DRM_IOCTL_MODE_GET_LEASE          DRM_IOWR(0xC8, struct drm_mode_get_lease)
#define DRM_IOCTL_MODE_REVOKE_LEASE       DRM_IOWR(0xC9, struct drm_mode_revoke_lease)
#define DRM_IOCTL_SYNCOBJ_TIMELINE_WAIT   DRM_IOWR(0xCA, struct drm_syncobj_timeline_wait)
#define DRM_IOCTL_SYNCOBJ_QUERY           DRM_IOWR(0xCB, struct drm_syncobj_timeline_array)
#define DRM_IOCTL_SYNCOBJ_TRANSFER        DRM_IOWR(0xCC, struct drm_syncobj_transfer)
#define DRM_IOCTL_SYNCOBJ_TIMELINE_SIGNAL DRM_IOWR(0xCD, struct drm_syncobj_timeline_array)
#define DRM_IOCTL_MODE_GETFB2             DRM_IOWR(0xCE, struct drm_mode_fb_cmd2)
#define DRM_IOCTL_SYNCOBJ_EVENTFD         DRM_IOWR(0xCF, struct drm_syncobj_eventfd)
#define DRM_IOCTL_MODE_CLOSEFB            DRM_IOWR(0xD0, struct drm_mode_closefb)

/* ------------------------------------------------------------------------
 * dma-buf: dma-buf.h
 */

#define DMA_BUF_SYNC_READ             (1 << 0)
#define DMA_BUF_SYNC_WRITE            (2 << 0)
#define DMA_BUF_SYNC_RW               (DMA_BUF_SYNC_READ | DMA_BUF_SYNC_WRITE)
#define DMA_BUF_SYNC_START            (0 << 2)
#define DMA_BUF_SYNC_END              (1 << 2)
#define DMA_BUF_SYNC_VALID_FLAGS_MASK (DMA_BUF_SYNC_RW | DMA_BUF_SYNC_END)
#define DMA_BUF_NAME_LEN              32

/* DMA_BUF_IOCTL_SYNC: the start or end of the CPU's access. */
struct dma_buf_sync {
    uint64_t flags;
};

/* EXPORT_SYNC_FILE: a sync file of the buffer's fences of the kind
 * `flags` names (DMA_BUF_SYNC_READ or WRITE); the call sets `fd`. */
struct dma_buf_export_sync_file {
    uint32_t flags;
    int32_t fd;
};

/* IMPORT_SYNC_FILE: add the sync file `fd`'s fence to the buffer. */
struct dma_buf_import_sync_file {
    uint32_t flags;
    int32_t fd;
};

#define DMA_BUF_BASE                   'b'
#define DMA_BUF_IOCTL_SYNC             UAPI_IOW(DMA_BUF_BASE, 0, struct dma_buf_sync)
/* SET_NAME's size is a pointer's, so its number differs by ABI: A is the
 * 32-bit one and B the 64-bit one, and the kernel takes both. */
#define DMA_BUF_SET_NAME               UAPI_IOW(DMA_BUF_BASE, 1, const char *)
#define DMA_BUF_SET_NAME_A             UAPI_IOW(DMA_BUF_BASE, 1, uint32_t)
#define DMA_BUF_SET_NAME_B             UAPI_IOW(DMA_BUF_BASE, 1, uint64_t)
#define DMA_BUF_IOCTL_EXPORT_SYNC_FILE UAPI_IOWR(DMA_BUF_BASE, 2, struct dma_buf_export_sync_file)
#define DMA_BUF_IOCTL_IMPORT_SYNC_FILE UAPI_IOW(DMA_BUF_BASE, 3, struct dma_buf_import_sync_file)

/* ------------------------------------------------------------------------
 * Sync files: sync_file.h
 */

/* SYNC_IOC_MERGE: a new sync file of the fences of this one and `fd2`;
 * the call sets `fence`. */
struct sync_merge_data {
    char name[32];
    int32_t fd2;
    int32_t fence;
    uint32_t flags;
    uint32_t pad;
};

/* One fence of SYNC_IOC_FILE_INFO: status 1 signalled, 0 active, or a
 * negative error. */
struct sync_fence_info {
    char obj_name[32];
    char driver_name[32];
    int32_t status;
    uint32_t flags;
    uint64_t timestamp_ns;
};

/* SYNC_IOC_FILE_INFO: `sync_fence_info` points at `num_fences` entries;
 * a call with `num_fences` 0 only counts them. */
struct sync_file_info {
    char name[32];
    int32_t status;
    uint32_t flags;
    uint32_t num_fences;
    uint32_t pad;
    uint64_t sync_fence_info;
};

struct sync_set_deadline {
    uint64_t deadline_ns;
    uint64_t pad;
};

#define SYNC_IOC_MAGIC        '>'
#define SYNC_IOC_MERGE        UAPI_IOWR(SYNC_IOC_MAGIC, 3, struct sync_merge_data)
#define SYNC_IOC_FILE_INFO    UAPI_IOWR(SYNC_IOC_MAGIC, 4, struct sync_file_info)
#define SYNC_IOC_SET_DEADLINE UAPI_IOW(SYNC_IOC_MAGIC, 5, struct sync_set_deadline)

/* ------------------------------------------------------------------------
 * sw_sync, the software timeline of debugfs's sync/sw_sync, whose
 * interface Linux keeps in drivers/dma-buf/sw_sync.c, outside uapi.
 */

struct sw_sync_create_fence_data {
    uint32_t value;
    char name[32];
    int32_t fence;
};

struct sw_sync_get_deadline {
    uint64_t deadline_ns;
    uint32_t fence_fd;
    uint32_t pad;
};

#define SW_SYNC_IOC_MAGIC        'W'
#define SW_SYNC_IOC_CREATE_FENCE UAPI_IOWR(SW_SYNC_IOC_MAGIC, 0, struct sw_sync_create_fence_data)
#define SW_SYNC_IOC_INC          UAPI_IOW(SW_SYNC_IOC_MAGIC, 1, uint32_t)
#define SW_SYNC_GET_DEADLINE     UAPI_IOWR(SW_SYNC_IOC_MAGIC, 2, struct sw_sync_get_deadline)

/* ------------------------------------------------------------------------
 * udmabuf: udmabuf.h
 */

#define UDMABUF_FLAGS_CLOEXEC 0x01

/* UDMABUF_CREATE: a dma-buf of `size` bytes of the memfd from `offset`. */
struct udmabuf_create {
    uint32_t memfd;
    uint32_t flags;
    uint64_t offset;
    uint64_t size;
};

struct udmabuf_create_item {
    uint32_t memfd;
    uint32_t __pad;
    uint64_t offset;
    uint64_t size;
};

/* UDMABUF_CREATE_LIST: `count` items follow the header in the caller's
 * memory, where Linux declares them as a flexible array, which C89 lacks;
 * the header's size, which the ioctl number carries, is the same. */
struct udmabuf_create_list {
    uint32_t flags;
    uint32_t count;
};

#define UDMABUF_CREATE      UAPI_IOW('u', 0x42, struct udmabuf_create)
#define UDMABUF_CREATE_LIST UAPI_IOW('u', 0x43, struct udmabuf_create_list)

#endif /* _DRM_UAPI_H_ */
