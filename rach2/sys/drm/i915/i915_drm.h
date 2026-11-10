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
 * multiple of 8. A user pointer travels in a 64-bit member. GETPARAM's
 * is the one exception, Linux's oldest: its pointer is the caller's
 * own, 8 bytes on i686 and 16 on x86-64, which an i386 caller on x86-64
 * reaches through the compat path. Each task of the driver adds the calls
 * it serves, in the order of their numbers.
 */

#define DRM_I915_GETPARAM             0x06
#define DRM_I915_GEM_BUSY             0x17
#define DRM_I915_GEM_CREATE           0x1b
#define DRM_I915_GEM_PREAD            0x1c
#define DRM_I915_GEM_PWRITE           0x1d
#define DRM_I915_GEM_MMAP             0x1e
#define DRM_I915_GEM_SET_DOMAIN       0x1f
#define DRM_I915_GEM_SW_FINISH        0x20
#define DRM_I915_GEM_SET_TILING       0x21
#define DRM_I915_GEM_GET_TILING       0x22
#define DRM_I915_GEM_GET_APERTURE     0x23
#define DRM_I915_GEM_MMAP_GTT         0x24
#define DRM_I915_GEM_MADVISE          0x26
#define DRM_I915_GEM_EXECBUFFER2      0x29
#define DRM_I915_GEM_WAIT             0x2c
#define DRM_I915_GEM_CONTEXT_CREATE   0x2d
#define DRM_I915_GEM_CONTEXT_DESTROY  0x2e
#define DRM_I915_GEM_SET_CACHING      0x2f
#define DRM_I915_GEM_GET_CACHING      0x30
#define DRM_I915_REG_READ             0x31
#define DRM_I915_GET_RESET_STATS      0x32
#define DRM_I915_GEM_USERPTR          0x33
#define DRM_I915_GEM_CONTEXT_GETPARAM 0x34
#define DRM_I915_GEM_CONTEXT_SETPARAM 0x35
#define DRM_I915_QUERY                0x39

/* GETPARAM: the parameter `param`'s value, written as an int to `value`. */
#define I915_PARAM_IRQ_ACTIVE               1
#define I915_PARAM_ALLOW_BATCHBUFFER        2
#define I915_PARAM_LAST_DISPATCH            3
#define I915_PARAM_CHIPSET_ID               4
#define I915_PARAM_HAS_GEM                  5
#define I915_PARAM_NUM_FENCES_AVAIL         6
#define I915_PARAM_HAS_OVERLAY              7
#define I915_PARAM_HAS_PAGEFLIPPING         8
#define I915_PARAM_HAS_EXECBUF2             9
#define I915_PARAM_HAS_BSD                  10
#define I915_PARAM_HAS_BLT                  11
#define I915_PARAM_HAS_RELAXED_FENCING      12
#define I915_PARAM_HAS_COHERENT_RINGS       13
#define I915_PARAM_HAS_EXEC_CONSTANTS       14
#define I915_PARAM_HAS_RELAXED_DELTA        15
#define I915_PARAM_HAS_GEN7_SOL_RESET       16
#define I915_PARAM_HAS_LLC                  17
#define I915_PARAM_HAS_ALIASING_PPGTT       18
#define I915_PARAM_HAS_WAIT_TIMEOUT         19
#define I915_PARAM_HAS_SEMAPHORES           20
#define I915_PARAM_HAS_PRIME_VMAP_FLUSH     21
#define I915_PARAM_HAS_VEBOX                22
#define I915_PARAM_HAS_SECURE_BATCHES       23
#define I915_PARAM_HAS_PINNED_BATCHES       24
#define I915_PARAM_HAS_EXEC_NO_RELOC        25
#define I915_PARAM_HAS_EXEC_HANDLE_LUT      26
#define I915_PARAM_HAS_WT                   27
#define I915_PARAM_CMD_PARSER_VERSION       28
#define I915_PARAM_HAS_COHERENT_PHYS_GTT    29
#define I915_PARAM_MMAP_VERSION             30
#define I915_PARAM_HAS_BSD2                 31
#define I915_PARAM_REVISION                 32
#define I915_PARAM_SUBSLICE_TOTAL           33
#define I915_PARAM_EU_TOTAL                 34
#define I915_PARAM_HAS_GPU_RESET            35
#define I915_PARAM_HAS_RESOURCE_STREAMER    36
#define I915_PARAM_HAS_EXEC_SOFTPIN         37
#define I915_PARAM_HAS_POOLED_EU            38
#define I915_PARAM_MIN_EU_IN_POOL           39
#define I915_PARAM_MMAP_GTT_VERSION         40
#define I915_PARAM_HAS_SCHEDULER            41
#define I915_PARAM_HUC_STATUS               42
#define I915_PARAM_HAS_EXEC_ASYNC           43
#define I915_PARAM_HAS_EXEC_FENCE           44
#define I915_PARAM_HAS_EXEC_CAPTURE         45
#define I915_PARAM_SLICE_MASK               46
#define I915_PARAM_SUBSLICE_MASK            47
#define I915_PARAM_HAS_EXEC_BATCH_FIRST     48
#define I915_PARAM_HAS_EXEC_FENCE_ARRAY     49
#define I915_PARAM_HAS_CONTEXT_ISOLATION    50
#define I915_PARAM_CS_TIMESTAMP_FREQUENCY   51
#define I915_PARAM_MMAP_GTT_COHERENT        52
#define I915_PARAM_HAS_EXEC_SUBMIT_FENCE    53
#define I915_PARAM_PERF_REVISION            54
#define I915_PARAM_HAS_EXEC_TIMELINE_FENCES 55
#define I915_PARAM_HAS_USERPTR_PROBE        56
#define I915_PARAM_OA_TIMESTAMP_FREQUENCY   57
#define I915_PARAM_PXP_STATUS               58
#define I915_PARAM_HAS_CONTEXT_FREQ_HINT    59

/* HAS_SCHEDULER's bits. */
#define I915_SCHEDULER_CAP_ENABLED    (1u << 0)
#define I915_SCHEDULER_CAP_PRIORITY   (1u << 1)
#define I915_SCHEDULER_CAP_PREEMPTION (1u << 2)
#define I915_SCHEDULER_CAP_SEMAPHORES (1u << 3)

struct drm_i915_getparam {
    int32_t param;
    int __user *value;
};

/* The engines as programs name them: a class and an instance in it. The
 * class INVALID with the instance INVALID_NONE is a hole in an engine
 * map. */
#define I915_ENGINE_CLASS_RENDER        0
#define I915_ENGINE_CLASS_COPY          1
#define I915_ENGINE_CLASS_VIDEO         2
#define I915_ENGINE_CLASS_VIDEO_ENHANCE 3
#define I915_ENGINE_CLASS_INVALID       0xffffu
#define I915_ENGINE_CLASS_INVALID_NONE  0xffffu

struct i915_engine_class_instance {
    uint16_t engine_class;
    uint16_t engine_instance;
};

/* An extension, chained from a call's `extensions` through
 * `next_extension`, 0 ending the chain: `name` says what follows it, and
 * `flags` and `rsvd` must be 0. */
struct i915_user_extension {
    uint64_t next_extension;
    uint32_t name;
    uint32_t flags;
    uint32_t rsvd[4];
};

/* GEM_CONTEXT_CREATE: a context, its id in `ctx_id`. The 8-byte form's
 * `pad` is the 16-byte form's `flags`. */
#define I915_CONTEXT_CREATE_FLAGS_USE_EXTENSIONS  (1u << 0)
#define I915_CONTEXT_CREATE_FLAGS_SINGLE_TIMELINE (1u << 1)
#define I915_CONTEXT_CREATE_FLAGS_UNKNOWN \
    (~(I915_CONTEXT_CREATE_FLAGS_USE_EXTENSIONS | I915_CONTEXT_CREATE_FLAGS_SINGLE_TIMELINE))
#define I915_CONTEXT_CREATE_EXT_SETPARAM 0
#define I915_CONTEXT_CREATE_EXT_CLONE    1

struct drm_i915_gem_context_create {
    uint32_t ctx_id;
    uint32_t pad;
};

struct drm_i915_gem_context_create_ext {
    uint32_t ctx_id;
    uint32_t flags;
    uint64_t extensions;
};

struct drm_i915_gem_context_destroy {
    uint32_t ctx_id;
    uint32_t pad;
};

/* GEM_CONTEXT_GETPARAM and SETPARAM: the parameter `param` of the context
 * `ctx_id`, in `value`, or for a parameter with a structure, the user
 * address of `size` bytes of it. */
#define I915_CONTEXT_PARAM_BAN_PERIOD        0x1
#define I915_CONTEXT_PARAM_NO_ZEROMAP        0x2
#define I915_CONTEXT_PARAM_GTT_SIZE          0x3
#define I915_CONTEXT_PARAM_NO_ERROR_CAPTURE  0x4
#define I915_CONTEXT_PARAM_BANNABLE          0x5
#define I915_CONTEXT_PARAM_PRIORITY          0x6
#define I915_CONTEXT_PARAM_SSEU              0x7
#define I915_CONTEXT_PARAM_RECOVERABLE       0x8
#define I915_CONTEXT_PARAM_VM                0x9
#define I915_CONTEXT_PARAM_ENGINES           0xa
#define I915_CONTEXT_PARAM_PERSISTENCE       0xb
#define I915_CONTEXT_PARAM_RINGSIZE          0xc
#define I915_CONTEXT_PARAM_PROTECTED_CONTENT 0xd
#define I915_CONTEXT_PARAM_LOW_LATENCY       0xe
#define I915_CONTEXT_PARAM_CONTEXT_IMAGE     0xf

#define I915_CONTEXT_MAX_USER_PRIORITY 1023
#define I915_CONTEXT_DEFAULT_PRIORITY  0
#define I915_CONTEXT_MIN_USER_PRIORITY (-1023)

struct drm_i915_gem_context_param {
    uint32_t ctx_id;
    uint32_t size;
    uint64_t param;
    uint64_t value;
};

/* CONTEXT_CREATE's extension SETPARAM: a parameter set on the new context,
 * whose `ctx_id` must be 0. */
struct drm_i915_gem_context_create_ext_setparam {
    struct i915_user_extension base;
    struct drm_i915_gem_context_param param;
};

/* The parameter SSEU: the slices, subslices and EUs an engine of the
 * context may use. */
#define I915_CONTEXT_SSEU_FLAG_ENGINE_INDEX (1u << 0)

struct drm_i915_gem_context_param_sseu {
    struct i915_engine_class_instance engine;
    uint32_t flags;
    uint64_t slice_mask;
    uint64_t subslice_mask;
    uint16_t min_eus_per_subslice;
    uint16_t max_eus_per_subslice;
    uint32_t rsvd;
};

/*
 * The parameter ENGINES, Linux's packed struct i915_context_param_engines:
 * a chain of extensions at 0, then the engines, 4 bytes each, from 8. Its
 * extensions, all packed after their 32-byte header: LOAD_BALANCE, an
 * engine index and a count of siblings (u16 each) at 32, flags (u32) at
 * 36, a u64 at 40 that must be 0, and the siblings from 48; BOND, the
 * master (4 bytes) at 32, the virtual index and the count of bonds (u16
 * each) at 36, flags (u64) at 40, four u64 that must be 0 from 48, and
 * the bonds from 80; PARALLEL_SUBMIT, an engine index, a width, a count of
 * siblings and a u16 that is ignored at 32 to 38, flags (u64) at 40, three
 * u64 that must be 0 from 48, and the width times the count of siblings
 * engines from 72.
 */
#define I915_CONTEXT_ENGINES_EXT_LOAD_BALANCE    0
#define I915_CONTEXT_ENGINES_EXT_BOND            1
#define I915_CONTEXT_ENGINES_EXT_PARALLEL_SUBMIT 2
#define I915_CONTEXT_PARAM_ENGINES_HEADER        8u

/* The most engines an engine map holds, Linux's I915_EXEC_RING_MASK + 1. */
#define I915_CONTEXT_MAX_ENGINES 64u

/* REG_READ: the register at `offset`, of which only the render ring's
 * timestamp may be read, into `val`; I915_REG_READ_8B_WA in the offset
 * reads its halves so that a carry between them is not seen half done. */
struct drm_i915_reg_read {
    uint64_t offset;
    uint64_t val;
};
#define I915_REG_READ_8B_WA 1u

/* GET_RESET_STATS: the resets seen, as root, and the context's batches
 * that were running at a hang and that were lost behind one. */
struct drm_i915_reset_stats {
    uint32_t ctx_id;
    uint32_t flags;
    uint32_t reset_count;
    uint32_t batch_active;
    uint32_t batch_pending;
    uint32_t pad;
};

/* QUERY: `num_items` items at `items_ptr`, each answered in its own
 * `length`: the bytes the answer takes, written at `data_ptr` when the
 * length given was enough, or a negative errno. */
#define DRM_I915_QUERY_TOPOLOGY_INFO          1
#define DRM_I915_QUERY_ENGINE_INFO            2
#define DRM_I915_QUERY_PERF_CONFIG            3
#define DRM_I915_QUERY_MEMORY_REGIONS         4
#define DRM_I915_QUERY_HWCONFIG_BLOB          5
#define DRM_I915_QUERY_GEOMETRY_SUBSLICES     6
#define DRM_I915_QUERY_GUC_SUBMISSION_VERSION 7

#define DRM_I915_QUERY_PERF_CONFIG_LIST          1
#define DRM_I915_QUERY_PERF_CONFIG_DATA_FOR_UUID 2
#define DRM_I915_QUERY_PERF_CONFIG_DATA_FOR_ID   3

struct drm_i915_query_item {
    uint64_t query_id;
    int32_t length;
    uint32_t flags;
    uint64_t data_ptr;
};

struct drm_i915_query {
    uint32_t num_items;
    uint32_t flags;
    uint64_t items_ptr;
};

/* TOPOLOGY_INFO: this header, then the slice mask at `data`, each slice's
 * subslice mask at `subslice_offset`, and each subslice's EU mask at
 * `eu_offset`, every offset from `data`. */
struct drm_i915_query_topology_info {
    uint16_t flags;
    uint16_t max_slices;
    uint16_t max_subslices;
    uint16_t max_eus_per_subslice;
    uint16_t subslice_offset;
    uint16_t subslice_stride;
    uint16_t eu_offset;
    uint16_t eu_stride;
};

/* ENGINE_INFO: this header, then one drm_i915_engine_info an engine. */
#define I915_ENGINE_INFO_HAS_LOGICAL_INSTANCE (1u << 0)

struct drm_i915_query_engine_info {
    uint32_t num_engines;
    uint32_t rsvd[3];
};

struct drm_i915_engine_info {
    struct i915_engine_class_instance engine;
    uint32_t rsvd0;
    uint64_t flags;
    uint64_t capabilities;
    uint16_t logical_instance;
    uint16_t rsvd1[3];
    uint64_t rsvd2[3];
};

/* MEMORY_REGIONS: this header, then one drm_i915_memory_region_info a
 * region; the CPU-visible sizes are the first two of `rsvd1`. */
#define I915_MEMORY_CLASS_SYSTEM 0
#define I915_MEMORY_CLASS_DEVICE 1

struct drm_i915_query_memory_regions {
    uint32_t num_regions;
    uint32_t rsvd[3];
};

struct drm_i915_memory_region_info {
    uint16_t memory_class;
    uint16_t memory_instance;
    uint32_t rsvd0;
    uint64_t probed_size;
    uint64_t unallocated_size;
    uint64_t rsvd1[8];
};

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

/*
 * GEM_EXECBUFFER2: run a batch. `buffers_ptr` holds `buffer_count`
 * objects, the batch the last of them or, with I915_EXEC_BATCH_FIRST, the
 * first; the batch starts `batch_start_offset` bytes into its object and
 * runs `batch_len` bytes, 0 for the rest of it. The low 32 bits of `rsvd1`
 * name the context. With I915_EXEC_FENCE_IN or _SUBMIT the low 32 bits of
 * `rsvd2` are a sync file to wait for, and with I915_EXEC_FENCE_OUT the
 * call returns a sync file of the batch's fence in its high 32 bits. With
 * I915_EXEC_FENCE_ARRAY `cliprects_ptr` holds `num_cliprects` syncobjs to
 * wait on or to signal; with I915_EXEC_USE_EXTENSIONS it holds a chain of
 * extensions instead.
 */
struct drm_i915_gem_relocation_entry {
    uint32_t target_handle;
    uint32_t delta;
    uint64_t offset;
    uint64_t presumed_offset;
    uint32_t read_domains;
    uint32_t write_domain;
};

#define EXEC_OBJECT_NEEDS_FENCE          (1u << 0)
#define EXEC_OBJECT_NEEDS_GTT            (1u << 1)
#define EXEC_OBJECT_WRITE                (1u << 2)
#define EXEC_OBJECT_SUPPORTS_48B_ADDRESS (1u << 3)
#define EXEC_OBJECT_PINNED               (1u << 4)
#define EXEC_OBJECT_PAD_TO_SIZE          (1u << 5)
#define EXEC_OBJECT_ASYNC                (1u << 6)
#define EXEC_OBJECT_CAPTURE              (1u << 7)
#define __EXEC_OBJECT_UNKNOWN_FLAGS      (~(uint64_t)((EXEC_OBJECT_CAPTURE << 1) - 1u))

struct drm_i915_gem_exec_object2 {
    uint32_t handle;
    uint32_t relocation_count;
    uint64_t relocs_ptr;
    uint64_t alignment;
    uint64_t offset;
    uint64_t flags;
    uint64_t pad_to_size; /* Linux's union with rsvd1 */
    uint64_t rsvd2;
};

#define I915_EXEC_FENCE_WAIT            (1u << 0)
#define I915_EXEC_FENCE_SIGNAL          (1u << 1)
#define __I915_EXEC_FENCE_UNKNOWN_FLAGS (~((I915_EXEC_FENCE_SIGNAL << 1) - 1u))

struct drm_i915_gem_exec_fence {
    uint32_t handle;
    uint32_t flags;
};

#define I915_EXEC_RING_MASK         0x3fu
#define I915_EXEC_DEFAULT           0u
#define I915_EXEC_RENDER            1u
#define I915_EXEC_BSD               2u
#define I915_EXEC_BLT               3u
#define I915_EXEC_VEBOX             4u
#define I915_EXEC_CONSTANTS_MASK    (3u << 6)
#define I915_EXEC_GEN7_SOL_RESET    (1u << 8)
#define I915_EXEC_SECURE            (1u << 9)
#define I915_EXEC_IS_PINNED         (1u << 10)
#define I915_EXEC_NO_RELOC          (1u << 11)
#define I915_EXEC_HANDLE_LUT        (1u << 12)
#define I915_EXEC_BSD_SHIFT         13
#define I915_EXEC_BSD_MASK          (3u << I915_EXEC_BSD_SHIFT)
#define I915_EXEC_BSD_DEFAULT       (0u << I915_EXEC_BSD_SHIFT)
#define I915_EXEC_BSD_RING1         (1u << I915_EXEC_BSD_SHIFT)
#define I915_EXEC_BSD_RING2         (2u << I915_EXEC_BSD_SHIFT)
#define I915_EXEC_RESOURCE_STREAMER (1u << 15)
#define I915_EXEC_FENCE_IN          (1u << 16)
#define I915_EXEC_FENCE_OUT         (1u << 17)
#define I915_EXEC_BATCH_FIRST       (1u << 18)
#define I915_EXEC_FENCE_ARRAY       (1u << 19)
#define I915_EXEC_FENCE_SUBMIT      (1u << 20)
#define I915_EXEC_USE_EXTENSIONS    (1u << 21)
#define __I915_EXEC_UNKNOWN_FLAGS   (~(uint64_t)((I915_EXEC_USE_EXTENSIONS << 1) - 1u))
#define I915_EXEC_CONTEXT_ID_MASK   0xffffffffu

struct drm_i915_gem_execbuffer2 {
    uint64_t buffers_ptr;
    uint32_t buffer_count;
    uint32_t batch_start_offset;
    uint32_t batch_len;
    uint32_t DR1;
    uint32_t DR4;
    uint32_t num_cliprects;
    uint64_t cliprects_ptr;
    uint64_t flags;
    uint64_t rsvd1;
    uint64_t rsvd2;
};

/* The extension of EXECBUFFER2 that takes timeline syncobjs: `fence_count`
 * entries at `handles_ptr`, each with its point, 0 for a binary syncobj,
 * at `values_ptr`. */
#define DRM_I915_GEM_EXECBUFFER_EXT_TIMELINE_FENCES 0
struct drm_i915_gem_execbuffer_ext_timeline_fences {
    struct i915_user_extension base;
    uint64_t fence_count;
    uint64_t handles_ptr;
    uint64_t values_ptr;
};

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

/* GEM_GET_APERTURE: the bytes of the global GTT, and those no pinned
 * binding holds. */
struct drm_i915_gem_get_aperture {
    uint64_t aper_size;
    uint64_t aper_available_size;
};

/* GEM_MADVISE: whether the object's pages are needed, or may be discarded
 * under memory pressure; `retained` says whether they still exist.
 * __I915_MADV_PURGED is the state of an object whose pages went, which
 * programs never pass. */
#define I915_MADV_WILLNEED 0
#define I915_MADV_DONTNEED 1
#define __I915_MADV_PURGED 2

struct drm_i915_gem_madvise {
    uint32_t handle;
    uint32_t madv;
    uint32_t retained;
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

/* GEM_USERPTR: an object over `user_size` bytes of the caller's memory at
 * `user_ptr`, both whole pages, and its handle. */
#define I915_USERPTR_READ_ONLY      0x1u
#define I915_USERPTR_PROBE          0x2u
#define I915_USERPTR_UNSYNCHRONIZED 0x80000000u

struct drm_i915_gem_userptr {
    uint64_t user_ptr;
    uint64_t user_size;
    uint32_t flags;
    uint32_t handle;
};

#define DRM_IOCTL_I915_GETPARAM \
    DRM_IOWR(DRM_COMMAND_BASE + DRM_I915_GETPARAM, struct drm_i915_getparam)
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
#define DRM_IOCTL_I915_GEM_GET_APERTURE \
    DRM_IOR(DRM_COMMAND_BASE + DRM_I915_GEM_GET_APERTURE, struct drm_i915_gem_get_aperture)
#define DRM_IOCTL_I915_GEM_MADVISE \
    DRM_IOWR(DRM_COMMAND_BASE + DRM_I915_GEM_MADVISE, struct drm_i915_gem_madvise)
#define DRM_IOCTL_I915_GEM_EXECBUFFER2 \
    DRM_IOW(DRM_COMMAND_BASE + DRM_I915_GEM_EXECBUFFER2, struct drm_i915_gem_execbuffer2)
#define DRM_IOCTL_I915_GEM_EXECBUFFER2_WR \
    DRM_IOWR(DRM_COMMAND_BASE + DRM_I915_GEM_EXECBUFFER2, struct drm_i915_gem_execbuffer2)
#define DRM_IOCTL_I915_GEM_WAIT \
    DRM_IOWR(DRM_COMMAND_BASE + DRM_I915_GEM_WAIT, struct drm_i915_gem_wait)
#define DRM_IOCTL_I915_GEM_CONTEXT_CREATE \
    DRM_IOWR(DRM_COMMAND_BASE + DRM_I915_GEM_CONTEXT_CREATE, struct drm_i915_gem_context_create)
#define DRM_IOCTL_I915_GEM_CONTEXT_CREATE_EXT \
    DRM_IOWR(DRM_COMMAND_BASE + DRM_I915_GEM_CONTEXT_CREATE, struct drm_i915_gem_context_create_ext)
#define DRM_IOCTL_I915_GEM_CONTEXT_DESTROY \
    DRM_IOW(DRM_COMMAND_BASE + DRM_I915_GEM_CONTEXT_DESTROY, struct drm_i915_gem_context_destroy)
#define DRM_IOCTL_I915_GEM_SET_CACHING \
    DRM_IOW(DRM_COMMAND_BASE + DRM_I915_GEM_SET_CACHING, struct drm_i915_gem_caching)
#define DRM_IOCTL_I915_GEM_GET_CACHING \
    DRM_IOWR(DRM_COMMAND_BASE + DRM_I915_GEM_GET_CACHING, struct drm_i915_gem_caching)
#define DRM_IOCTL_I915_REG_READ \
    DRM_IOWR(DRM_COMMAND_BASE + DRM_I915_REG_READ, struct drm_i915_reg_read)
#define DRM_IOCTL_I915_GET_RESET_STATS \
    DRM_IOWR(DRM_COMMAND_BASE + DRM_I915_GET_RESET_STATS, struct drm_i915_reset_stats)
#define DRM_IOCTL_I915_GEM_USERPTR \
    DRM_IOWR(DRM_COMMAND_BASE + DRM_I915_GEM_USERPTR, struct drm_i915_gem_userptr)
#define DRM_IOCTL_I915_GEM_CONTEXT_GETPARAM \
    DRM_IOWR(DRM_COMMAND_BASE + DRM_I915_GEM_CONTEXT_GETPARAM, struct drm_i915_gem_context_param)
#define DRM_IOCTL_I915_GEM_CONTEXT_SETPARAM \
    DRM_IOWR(DRM_COMMAND_BASE + DRM_I915_GEM_CONTEXT_SETPARAM, struct drm_i915_gem_context_param)
#define DRM_IOCTL_I915_QUERY DRM_IOWR(DRM_COMMAND_BASE + DRM_I915_QUERY, struct drm_i915_query)

#endif /* _I915_DRM_H_ */
