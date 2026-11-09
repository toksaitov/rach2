/*
 * i915_drm.h: the Intel graphics driver's own ioctls, their numbers and
 * structures as Linux 6.12's include/uapi/drm/i915_drm.h gives them.
 */
#ifndef _I915_DRM_H_
#define _I915_DRM_H_

#include "../uapi.h"

/*
 * Every structure here has one layout on i686 and x86-64: Linux's carry
 * only fixed-width members, each 64-bit one at an offset that is a
 * multiple of 8. A user pointer travels in a 64-bit member. Each task of
 * the driver adds the calls it serves, in the order of their numbers.
 */

#define DRM_I915_GEM_BUSY        0x17
#define DRM_I915_GEM_CREATE      0x1b
#define DRM_I915_GEM_PREAD       0x1c
#define DRM_I915_GEM_PWRITE      0x1d
#define DRM_I915_GEM_MMAP        0x1e
#define DRM_I915_GEM_SET_DOMAIN  0x1f
#define DRM_I915_GEM_SW_FINISH   0x20
#define DRM_I915_GEM_SET_TILING  0x21
#define DRM_I915_GEM_GET_TILING  0x22
#define DRM_I915_GEM_MMAP_GTT    0x24
#define DRM_I915_GEM_WAIT        0x2c
#define DRM_I915_GEM_SET_CACHING 0x2f
#define DRM_I915_GEM_GET_CACHING 0x30

/* GEM_CREATE: an object of `size` bytes, rounded up to pages, which the
 * call writes back, and its handle. */
struct drm_i915_gem_create {
    uint64_t size;
    uint32_t handle;
    uint32_t pad;
};

/* GEM_PREAD and GEM_PWRITE: `size` bytes at `offset` in the object, to or
 * from the user buffer at `data_ptr`. */
struct drm_i915_gem_pread {
    uint32_t handle;
    uint32_t pad;
    uint64_t offset;
    uint64_t size;
    uint64_t data_ptr;
};

struct drm_i915_gem_pwrite {
    uint32_t handle;
    uint32_t pad;
    uint64_t offset;
    uint64_t size;
    uint64_t data_ptr;
};

/* GEM_MMAP: the object's pages mapped into the caller, its address in
 * `addr_ptr`; `flags` I915_MMAP_WC for write-combining. */
#define I915_MMAP_WC 0x1

struct drm_i915_gem_mmap {
    uint32_t handle;
    uint32_t pad;
    uint64_t offset;
    uint64_t size;
    uint64_t addr_ptr;
    uint64_t flags;
};

/* GEM_MMAP_GTT: the offset an mmap of the card or render node gives to map
 * the object through the aperture. */
struct drm_i915_gem_mmap_gtt {
    uint32_t handle;
    uint32_t pad;
    uint64_t offset;
};

/* GEM_MMAP_OFFSET, GEM_MMAP_GTT's number with a larger structure: the
 * offset for a mapping of the kind `flags` names. */
#define I915_MMAP_OFFSET_GTT   0
#define I915_MMAP_OFFSET_WC    1
#define I915_MMAP_OFFSET_WB    2
#define I915_MMAP_OFFSET_UC    3
#define I915_MMAP_OFFSET_FIXED 4

struct drm_i915_gem_mmap_offset {
    uint32_t handle;
    uint32_t pad;
    uint64_t offset;
    uint64_t flags;
    uint64_t extensions;
};

/* The domains an object's pages are read and written in: the CPU's cache,
 * the GPU's caches, the aperture and write-combining. */
#define I915_GEM_DOMAIN_CPU         0x00000001u
#define I915_GEM_DOMAIN_RENDER      0x00000002u
#define I915_GEM_DOMAIN_SAMPLER     0x00000004u
#define I915_GEM_DOMAIN_COMMAND     0x00000008u
#define I915_GEM_DOMAIN_INSTRUCTION 0x00000010u
#define I915_GEM_DOMAIN_VERTEX      0x00000020u
#define I915_GEM_DOMAIN_GTT         0x00000040u
#define I915_GEM_DOMAIN_WC          0x00000080u
#define I915_GEM_GPU_DOMAINS                                                      \
    (I915_GEM_DOMAIN_RENDER | I915_GEM_DOMAIN_SAMPLER | I915_GEM_DOMAIN_COMMAND | \
     I915_GEM_DOMAIN_INSTRUCTION | I915_GEM_DOMAIN_VERTEX)

struct drm_i915_gem_set_domain {
    uint32_t handle;
    uint32_t read_domains;
    uint32_t write_domain;
};

struct drm_i915_gem_sw_finish {
    uint32_t handle;
};

/* GEM_BUSY: in `busy`, the engine classes that read the object in bits
 * 31:16 and one more than the class that writes it in bits 15:0. */
struct drm_i915_gem_busy {
    uint32_t handle;
    uint32_t busy;
};

/* The tiling layouts and the swizzles of address bit 6 that the memory
 * controller gives them. */
#define I915_TILING_NONE 0
#define I915_TILING_X    1
#define I915_TILING_Y    2
#define I915_TILING_LAST I915_TILING_Y

#define I915_BIT_6_SWIZZLE_NONE    0
#define I915_BIT_6_SWIZZLE_9       1
#define I915_BIT_6_SWIZZLE_9_10    2
#define I915_BIT_6_SWIZZLE_9_11    3
#define I915_BIT_6_SWIZZLE_9_10_11 4
#define I915_BIT_6_SWIZZLE_UNKNOWN 5 /* never seen by programs */
#define I915_BIT_6_SWIZZLE_9_17    6
#define I915_BIT_6_SWIZZLE_9_10_17 7

struct drm_i915_gem_set_tiling {
    uint32_t handle;
    uint32_t tiling_mode;
    uint32_t stride;
    uint32_t swizzle_mode;
};

struct drm_i915_gem_get_tiling {
    uint32_t handle;
    uint32_t tiling_mode;
    uint32_t swizzle_mode;
    uint32_t phys_swizzle_mode;
};

/* GEM_WAIT: wait for the object's work for `timeout_ns`, negative for
 * ever, 0 to ask; the time left is written back. */
struct drm_i915_gem_wait {
    uint32_t bo_handle;
    uint32_t flags;
    int64_t timeout_ns;
};

/* GEM_SET_CACHING and GEM_GET_CACHING. */
#define I915_CACHING_NONE    0
#define I915_CACHING_CACHED  1
#define I915_CACHING_DISPLAY 2

struct drm_i915_gem_caching {
    uint32_t handle;
    uint32_t caching;
};

#define DRM_IOCTL_I915_GEM_BUSY \
    DRM_IOWR(DRM_COMMAND_BASE + DRM_I915_GEM_BUSY, struct drm_i915_gem_busy)
#define DRM_IOCTL_I915_GEM_CREATE \
    DRM_IOWR(DRM_COMMAND_BASE + DRM_I915_GEM_CREATE, struct drm_i915_gem_create)
#define DRM_IOCTL_I915_GEM_PREAD \
    DRM_IOW(DRM_COMMAND_BASE + DRM_I915_GEM_PREAD, struct drm_i915_gem_pread)
#define DRM_IOCTL_I915_GEM_PWRITE \
    DRM_IOW(DRM_COMMAND_BASE + DRM_I915_GEM_PWRITE, struct drm_i915_gem_pwrite)
#define DRM_IOCTL_I915_GEM_MMAP \
    DRM_IOWR(DRM_COMMAND_BASE + DRM_I915_GEM_MMAP, struct drm_i915_gem_mmap)
#define DRM_IOCTL_I915_GEM_MMAP_GTT \
    DRM_IOWR(DRM_COMMAND_BASE + DRM_I915_GEM_MMAP_GTT, struct drm_i915_gem_mmap_gtt)
#define DRM_IOCTL_I915_GEM_MMAP_OFFSET \
    DRM_IOWR(DRM_COMMAND_BASE + DRM_I915_GEM_MMAP_GTT, struct drm_i915_gem_mmap_offset)
#define DRM_IOCTL_I915_GEM_SET_DOMAIN \
    DRM_IOW(DRM_COMMAND_BASE + DRM_I915_GEM_SET_DOMAIN, struct drm_i915_gem_set_domain)
#define DRM_IOCTL_I915_GEM_SW_FINISH \
    DRM_IOW(DRM_COMMAND_BASE + DRM_I915_GEM_SW_FINISH, struct drm_i915_gem_sw_finish)
#define DRM_IOCTL_I915_GEM_SET_TILING \
    DRM_IOWR(DRM_COMMAND_BASE + DRM_I915_GEM_SET_TILING, struct drm_i915_gem_set_tiling)
#define DRM_IOCTL_I915_GEM_GET_TILING \
    DRM_IOWR(DRM_COMMAND_BASE + DRM_I915_GEM_GET_TILING, struct drm_i915_gem_get_tiling)
#define DRM_IOCTL_I915_GEM_WAIT \
    DRM_IOWR(DRM_COMMAND_BASE + DRM_I915_GEM_WAIT, struct drm_i915_gem_wait)
#define DRM_IOCTL_I915_GEM_SET_CACHING \
    DRM_IOW(DRM_COMMAND_BASE + DRM_I915_GEM_SET_CACHING, struct drm_i915_gem_caching)
#define DRM_IOCTL_I915_GEM_GET_CACHING \
    DRM_IOWR(DRM_COMMAND_BASE + DRM_I915_GEM_GET_CACHING, struct drm_i915_gem_caching)

#endif /* _I915_DRM_H_ */
