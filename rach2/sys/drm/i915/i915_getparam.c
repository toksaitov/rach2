/*
 * i915_getparam.c: what the Intel graphics driver tells programs of the
 * device: GETPARAM and I915_QUERY, as Linux 6.12's i915_getparam.c and
 * i915_query.c answer them on the Lenovo's HD 4400, and the slices,
 * subslices and EUs they report, after gt/intel_sseu.c.
 */

#include "i915.h"
#include "i915_drm.h"
#include "i915_reg.h"

#include "../../lib/kerrno.h"
#include "../../lib/kstring.h"
#include "../../mm/paging.h"
#include "../../mm/usercopy.h"

/* The user address a 64-bit field of a call names. */
#define I915_USER_PTR(v) ((void __user *)(uintptr_t)(v))

/* The command streamer's timestamp and the OA unit's on graphics version 7,
 * 12.5 MHz (Linux's gen6 read of the clock and i915_perf). */
#define I915_HSW_TIMESTAMP_HZ 12500000

/* MMAP_GTT_VERSION and PERF_REVISION, Linux's i915_gem_mmap_gtt_version
 * and i915_perf_ioctl_version. MMAP_GTT_VERSION is 6.18's 5, which the
 * Lenovo gave under Linux 6.18.58: it announces partial maps from inside an
 * object, which 6.12 already makes and answers 4. */
#define I915_MMAP_GTT_VERSION 5
#define I915_PERF_REVISION    7

/* The values of HSW_PAVP_FUSE1's field of disabled EUs. */
#define HSW_F1_EU_DIS_10EUS 0u
#define HSW_F1_EU_DIS_8EUS  1u
#define HSW_F1_EU_DIS_6EUS  2u

/** The bits set in `v`. */
static uint32_t hweight8(uint8_t v)
{
    uint32_t n = 0;

    for (; v; v &= (uint8_t)(v - 1u)) {
        n++;
    }
    return n;
}

/**
 * Initialize the immutable topology once under the GEM mutex, which may
 * already be held by the caller. May sleep; leaves the caller's hold as is.
 */
void i915_sseu_init(struct drm_i915_private *i915)
{
    struct i915_sseu *sseu = &i915->sseu;
    uint32_t fuse1;
    int taken = i915_gem_lock(i915);

    if (sseu->ready) {
        i915_gem_unlock(i915, taken);
        return;
    }
    /* There is no register that counts slices and subslices: Linux works
     * them out from the part, GT2 here. */
    sseu->slice_mask = 0x1;
    sseu->subslice_mask = 0x3;
    i915_rpm_get(i915);
    fuse1 = i915_uncore_read(i915, HSW_PAVP_FUSE1);
    switch ((fuse1 & HSW_F1_EU_DIS_MASK) >> HSW_F1_EU_DIS_SHIFT) {
    case HSW_F1_EU_DIS_8EUS:
        sseu->eu_per_subslice = 8;
        break;
    case HSW_F1_EU_DIS_6EUS:
        sseu->eu_per_subslice = 6;
        break;
    default:
        sseu->eu_per_subslice = 10;
        break;
    }
    sseu->max_slices = (uint8_t)hweight8(sseu->slice_mask);
    sseu->max_subslices = (uint8_t)hweight8(sseu->subslice_mask);
    sseu->max_eus_per_subslice = sseu->eu_per_subslice;
    sseu->eu_total = (uint16_t)(sseu->max_slices * sseu->max_subslices * sseu->eu_per_subslice);
    sseu->edram = (i915_uncore_read(i915, HSW_EDRAM_CAP) & EDRAM_ENABLED) != 0;
    i915_rpm_put(i915);
    sseu->ready = 1;
    i915_gem_unlock(i915, taken);
}

/* The parameters whose answers are the same on every Haswell GT2 with its
 * four rings, as Linux 6.12 answers them. CMD_PARSER_VERSION is the
 * parser's, since every ring parses (i915_cmd_parser.c).
 * HAS_CONTEXT_FREQ_HINT needs GuC, and Linux answers its error as the
 * value. HAS_CONTEXT_ISOLATION is the classes with a default context
 * image: render. */
static const struct {
    int32_t param;
    int value;
} i915_fixed_params[] = {{I915_PARAM_NUM_FENCES_AVAIL, I915_NUM_FENCES},
                         {I915_PARAM_HAS_ALIASING_PPGTT, 1}, /* INTEL_PPGTT_ALIASING */
                         {I915_PARAM_MMAP_GTT_VERSION, I915_MMAP_GTT_VERSION},
                         {I915_PARAM_HAS_OVERLAY, 0},
                         {I915_PARAM_HAS_SEMAPHORES, 0},
                         {I915_PARAM_HAS_SECURE_BATCHES, 0},
                         {I915_PARAM_CMD_PARSER_VERSION, I915_CMD_PARSER_VERSION},
                         {I915_PARAM_HAS_RESOURCE_STREAMER, 0},
                         {I915_PARAM_HAS_POOLED_EU, 0},
                         {I915_PARAM_MIN_EU_IN_POOL, 0},
                         {I915_PARAM_HAS_SCHEDULER, 0},
                         {I915_PARAM_MMAP_VERSION, 1},
                         {I915_PARAM_HAS_GEM, 1},
                         {I915_PARAM_HAS_PAGEFLIPPING, 1},
                         {I915_PARAM_HAS_EXECBUF2, 1},
                         {I915_PARAM_HAS_RELAXED_FENCING, 1},
                         {I915_PARAM_HAS_COHERENT_RINGS, 1},
                         {I915_PARAM_HAS_RELAXED_DELTA, 1},
                         {I915_PARAM_HAS_GEN7_SOL_RESET, 1},
                         {I915_PARAM_HAS_WAIT_TIMEOUT, 1},
                         {I915_PARAM_HAS_PRIME_VMAP_FLUSH, 1},
                         {I915_PARAM_HAS_PINNED_BATCHES, 1},
                         {I915_PARAM_HAS_EXEC_NO_RELOC, 1},
                         {I915_PARAM_HAS_EXEC_HANDLE_LUT, 1},
                         {I915_PARAM_HAS_COHERENT_PHYS_GTT, 1},
                         {I915_PARAM_HAS_EXEC_SOFTPIN, 1},
                         {I915_PARAM_HAS_EXEC_ASYNC, 1},
                         {I915_PARAM_HAS_EXEC_FENCE, 1},
                         {I915_PARAM_HAS_EXEC_CAPTURE, 1},
                         {I915_PARAM_HAS_EXEC_BATCH_FIRST, 1},
                         {I915_PARAM_HAS_EXEC_FENCE_ARRAY, 1},
                         {I915_PARAM_HAS_EXEC_SUBMIT_FENCE, 1},
                         {I915_PARAM_HAS_EXEC_TIMELINE_FENCES, 1},
                         {I915_PARAM_HAS_USERPTR_PROBE, 1},
                         {I915_PARAM_MMAP_GTT_COHERENT, 1},
                         {I915_PARAM_HAS_CONTEXT_FREQ_HINT, -EINVAL},
                         {I915_PARAM_HAS_CONTEXT_ISOLATION, 1 << I915_ENGINE_CLASS_RENDER},
                         {I915_PARAM_CS_TIMESTAMP_FREQUENCY, I915_HSW_TIMESTAMP_HZ},
                         {I915_PARAM_OA_TIMESTAMP_FREQUENCY, I915_HSW_TIMESTAMP_HZ},
                         {I915_PARAM_PERF_REVISION, I915_PERF_REVISION}};

/** The answer to `param` that depends on the device: its identity, its
 * engines, its slices and EUs, its eDRAM. 0, or -EINVAL for a parameter
 * that is not one of them. */
static int32_t device_param(struct drm_i915_private *i915, int32_t param, int *value)
{
    const struct i915_sseu *sseu = &i915->sseu;

    switch (param) {
    case I915_PARAM_CHIPSET_ID:
        *value = i915->id->device_id;
        return 0;
    case I915_PARAM_REVISION:
        *value = i915->revision;
        return 0;
    case I915_PARAM_HAS_BSD:
        *value = i915_engine_lookup_user(i915, I915_ENGINE_CLASS_VIDEO, 0) != 0;
        return 0;
    case I915_PARAM_HAS_BLT:
        *value = i915_engine_lookup_user(i915, I915_ENGINE_CLASS_COPY, 0) != 0;
        return 0;
    case I915_PARAM_HAS_VEBOX:
        *value = i915_engine_lookup_user(i915, I915_ENGINE_CLASS_VIDEO_ENHANCE, 0) != 0;
        return 0;
    case I915_PARAM_HAS_BSD2:
        *value = i915_engine_lookup_user(i915, I915_ENGINE_CLASS_VIDEO, 1) != 0;
        return 0;
    case I915_PARAM_HAS_LLC:
        *value = i915->id->platform->has_llc;
        return 0;
    case I915_PARAM_HAS_WT:
        *value = sseu->edram;
        return 0;
    case I915_PARAM_SUBSLICE_TOTAL:
        *value = sseu->max_slices * sseu->max_subslices;
        return 0;
    case I915_PARAM_EU_TOTAL:
        *value = sseu->eu_total;
        return 0;
    case I915_PARAM_SLICE_MASK:
        *value = sseu->slice_mask;
        return 0;
    case I915_PARAM_SUBSLICE_MASK:
        *value = sseu->subslice_mask;
        return 0;
    case I915_PARAM_HAS_GPU_RESET:
        /* Hangcheck on and a GPU reset, 2 with an engine's own reset. */
        *value = !i915->params.hangcheck_off && I915_RESET_PARAM(i915) != 0;
        if (*value && i915->id->platform->has_reset_engine && I915_RESET_PARAM(i915) >= 2) {
            *value = 2;
        }
        return 0;
    default:
        return -EINVAL;
    }
}

/**
 * GETPARAM, as Linux's i915_getparam_ioctl on Haswell GT2 with the render,
 * blitter, video and video enhancement rings: the device's identity, what
 * the engines and GEM do, the slices and EUs, and the clocks.
 *
 * @return 0 with the value written to `value`; -ENODEV for the old
 *         parameters of user mode setting and for the HuC and PXP, which
 *         Haswell lacks; -EINVAL for an unknown parameter; or -EFAULT.
 */
int32_t i915_getparam_ioctl(struct drm_device *dev, void *data, struct drm_file *file)
{
    struct drm_i915_private *i915 = to_i915(dev);
    struct drm_i915_getparam *param = (struct drm_i915_getparam *)data;
    int value = 0;
    unsigned i;

    (void)file;
    i915_sseu_init(i915);
    switch (param->param) {
    case I915_PARAM_IRQ_ACTIVE:
    case I915_PARAM_ALLOW_BATCHBUFFER:
    case I915_PARAM_LAST_DISPATCH:
    case I915_PARAM_HAS_EXEC_CONSTANTS:
    case I915_PARAM_HUC_STATUS:
    case I915_PARAM_PXP_STATUS:
        return -ENODEV;
    default:
        break;
    }
    for (i = 0; i < sizeof(i915_fixed_params) / sizeof(i915_fixed_params[0]); i++) {
        if (i915_fixed_params[i].param == param->param) {
            value = i915_fixed_params[i].value;
            break;
        }
    }
    if (i == sizeof(i915_fixed_params) / sizeof(i915_fixed_params[0]) &&
        device_param(i915, param->param, &value) < 0) {
        return -EINVAL;
    }
    if (copy_to_user(param->value, &value, sizeof(value)) != 0) {
        return -EFAULT;
    }
    return 0;
}

/* ------------------------------------------------------------------------
 * I915_QUERY
 */

/**
 * The common start of a query item, as Linux's copy_query_item: with a
 * length of 0 the length the answer needs, `total`; a length short of it
 * -EINVAL; otherwise 0, with the `size` bytes of the header the caller
 * gave read into `hdr`, or -EFAULT.
 */
static int32_t copy_query_item(void *hdr, size_t size, uint32_t total,
                               const struct drm_i915_query_item *item)
{
    if (item->length == 0) {
        return (int32_t)total;
    }
    if ((uint32_t)item->length < total) {
        return -EINVAL;
    }
    return copy_from_user(hdr, I915_USER_PTR(item->data_ptr), size) != 0 ? -EFAULT : 0;
}

/** Bytes for `n` bits, Linux's GEN_SSEU_STRIDE. */
static uint32_t sseu_stride(uint32_t n)
{
    return (n + 7u) / 8u;
}

/**
 * TOPOLOGY_INFO, as Linux's query_topology_info: the header, the slice
 * mask, each slice's subslice mask and each subslice's EU mask, every EU
 * of each subslice enabled, as Haswell has no EU fuses by subslice.
 */
static int32_t query_topology(struct drm_i915_private *i915, struct drm_i915_query_item *item)
{
    const struct i915_sseu *sseu = &i915->sseu;
    struct drm_i915_query_topology_info topo;
    uint32_t ss_stride = sseu_stride(sseu->max_subslices);
    uint32_t eu_stride = sseu_stride(sseu->max_eus_per_subslice);
    uint32_t slice_length = 1, subslice_length = sseu->max_slices * ss_stride;
    uint32_t eu_length = sseu->max_slices * sseu->max_subslices * eu_stride;
    uint32_t total = (uint32_t)sizeof(topo) + slice_length + subslice_length + eu_length;
    uint8_t masks[64];
    uint32_t i, n = 0, eus = (1u << sseu->eu_per_subslice) - 1u;
    int32_t ret;

    if (item->flags != 0) {
        return -EINVAL;
    }
    ret = copy_query_item(&topo, sizeof(topo), total, item);
    if (ret != 0) {
        return ret;
    }
    k_memset(&topo, 0, sizeof(topo));
    topo.max_slices = sseu->max_slices;
    topo.max_subslices = sseu->max_subslices;
    topo.max_eus_per_subslice = sseu->max_eus_per_subslice;
    topo.subslice_offset = (uint16_t)slice_length;
    topo.subslice_stride = (uint16_t)ss_stride;
    topo.eu_offset = (uint16_t)(slice_length + subslice_length);
    topo.eu_stride = (uint16_t)eu_stride;
    masks[n++] = sseu->slice_mask;
    for (i = 0; i < subslice_length; i++) {
        masks[n++] = i == 0 ? sseu->subslice_mask : 0;
    }
    for (i = 0; i < eu_length; i++) {
        masks[n++] = (uint8_t)(eus >> (8u * (i % eu_stride)));
    }
    if (copy_to_user(I915_USER_PTR(item->data_ptr), &topo, sizeof(topo)) != 0 ||
        copy_to_user(I915_USER_PTR(item->data_ptr + sizeof(topo)), masks, n) != 0) {
        return -EFAULT;
    }
    return (int32_t)total;
}

/**
 * ENGINE_INFO, as Linux's query_engine_info: the header, whose count and
 * reserved words must be 0, then rcs0, bcs0, vcs0 and vecs0 in Linux's
 * order, by class, each its class and instance, logical instance 0 and no
 * capabilities, which Haswell's video rings lack (no HEVC, no SFC).
 */
static int32_t query_engine_info(struct drm_i915_private *i915, struct drm_i915_query_item *item)
{
    struct drm_i915_query_engine_info query;
    struct drm_i915_engine_info info;
    uint32_t total = (uint32_t)(sizeof(query) + I915_NUM_ENGINES * sizeof(info)), i;
    int32_t ret;

    if (item->flags != 0) {
        return -EINVAL;
    }
    ret = copy_query_item(&query, sizeof(query), total, item);
    if (ret != 0) {
        return ret;
    }
    if (query.num_engines || query.rsvd[0] || query.rsvd[1] || query.rsvd[2]) {
        return -EINVAL;
    }
    for (i = 0; i < I915_NUM_ENGINES; i++) {
        k_memset(&info, 0, sizeof(info));
        info.engine.engine_class = i915_engine_uabi_class(&i915->gt.engine[i]);
        info.engine.engine_instance = 0;
        info.flags = I915_ENGINE_INFO_HAS_LOGICAL_INSTANCE;
        info.logical_instance = 0;
        if (copy_to_user(I915_USER_PTR(item->data_ptr + sizeof(query) + i * sizeof(info)), &info,
                         sizeof(info)) != 0) {
            return -EFAULT;
        }
        query.num_engines++;
    }
    if (copy_to_user(I915_USER_PTR(item->data_ptr), &query, sizeof(query)) != 0) {
        return -EFAULT;
    }
    return (int32_t)total;
}

/** PERF_CONFIG, as Linux's query_perf_config without i915 perf: -ENODEV
 * for each of its kinds and -EINVAL for any other. */
static int32_t query_perf_config(struct drm_i915_private *i915, struct drm_i915_query_item *item)
{
    (void)i915;
    switch (item->flags) {
    case DRM_I915_QUERY_PERF_CONFIG_LIST:
    case DRM_I915_QUERY_PERF_CONFIG_DATA_FOR_UUID:
    case DRM_I915_QUERY_PERF_CONFIG_DATA_FOR_ID:
        return -ENODEV;
    default:
        return -EINVAL;
    }
}

/**
 * MEMORY_REGIONS, as Linux's query_memregion_info on an integrated part:
 * the header, whose count and reserved words must be 0, and system memory,
 * all of it probed, visible to the processor and unallocated, since the
 * driver keeps no count of what its objects take.
 */
static int32_t query_memregions(struct drm_i915_private *i915, struct drm_i915_query_item *item)
{
    struct drm_i915_query_memory_regions query;
    struct drm_i915_memory_region_info info;
    uint32_t total = (uint32_t)(sizeof(query) + sizeof(info)), i;
    uint64_t bytes = (uint64_t)frame_total_count() * PAGE_SIZE;
    int32_t ret;

    (void)i915;
    if (item->flags != 0) {
        return -EINVAL;
    }
    ret = copy_query_item(&query, sizeof(query), total, item);
    if (ret != 0) {
        return ret;
    }
    if (query.num_regions) {
        return -EINVAL;
    }
    for (i = 0; i < 3; i++) {
        if (query.rsvd[i]) {
            return -EINVAL;
        }
    }
    k_memset(&info, 0, sizeof(info));
    info.memory_class = I915_MEMORY_CLASS_SYSTEM;
    info.memory_instance = 0;
    info.probed_size = bytes;
    info.unallocated_size = bytes;
    info.rsvd1[0] = bytes; /* probed_cpu_visible_size */
    info.rsvd1[1] = bytes; /* unallocated_cpu_visible_size */
    query.num_regions = 1;
    if (copy_to_user(I915_USER_PTR(item->data_ptr + sizeof(query)), &info, sizeof(info)) != 0 ||
        copy_to_user(I915_USER_PTR(item->data_ptr), &query, sizeof(query)) != 0) {
        return -EFAULT;
    }
    return (int32_t)total;
}

/** HWCONFIG_BLOB, GEOMETRY_SUBSLICES and GUC_SUBMISSION_VERSION, which
 * need a GuC or graphics version 12.55: -ENODEV. */
static int32_t query_none(struct drm_i915_private *i915, struct drm_i915_query_item *item)
{
    (void)i915;
    (void)item;
    return -ENODEV;
}

static int32_t (*const query_funcs[])(struct drm_i915_private *, struct drm_i915_query_item *) = {
    query_topology, query_engine_info, query_perf_config, query_memregions,
    query_none,     query_none,        query_none};

/**
 * I915_QUERY, as Linux's i915_query_ioctl: each item answered in its own
 * length, written back only when it differs, with the bytes its answer
 * takes or a negative errno.
 *
 * @return 0; -EINVAL for flags that are not 0 or an item of id 0 or past
 *         what a long holds; or -EFAULT for an item that cannot be read
 *         or written.
 */
int32_t i915_query_ioctl(struct drm_device *dev, void *data, struct drm_file *file)
{
    struct drm_i915_private *i915 = to_i915(dev);
    struct drm_i915_query *args = (struct drm_i915_query *)data;
    struct drm_i915_query_item item;
    uintptr_t at = (uintptr_t)args->items_ptr;
    uint32_t i;
    int32_t ret;

    (void)file;
    if (args->flags != 0) {
        return -EINVAL;
    }
    i915_sseu_init(i915);
    for (i = 0; i < args->num_items; i++, at += sizeof(item)) {
        if (copy_from_user(&item, I915_USER_PTR(at), sizeof(item)) != 0) {
            return -EFAULT;
        }
        if (item.query_id == 0 || item.query_id - 1u > (uint64_t)(unsigned long)-1) {
            return -EINVAL;
        }
        ret = -EINVAL;
        if (item.query_id - 1u < sizeof(query_funcs) / sizeof(query_funcs[0])) {
            ret = query_funcs[item.query_id - 1u](i915, &item);
        }
        if (ret != item.length && copy_to_user(I915_USER_PTR(at + 8u), &ret, sizeof(ret)) != 0) {
            return -EFAULT;
        }
    }
    return 0;
}
