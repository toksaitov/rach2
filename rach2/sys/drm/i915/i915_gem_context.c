/*
 * i915_gem_context.c: the Intel graphics driver's contexts: a file's
 * contexts and their parameters, the engine maps programs give them, their
 * states on each engine with the render ring's image, and the reset
 * statistics, after Linux 6.12's gem/i915_gem_context.c and
 * gt/intel_context.c on a legacy ring.
 */

#include "i915.h"
#include "i915_drm.h"
#include "i915_reg.h"

#include "../../dmabuf/dmabuf.h"
#include "../../klog.h"
#include "../../lib/kerrno.h"
#include "../../lib/kstring.h"
#include "../../mm/kheap.h"
#include "../../mm/paging.h"
#include "../../mm/usercopy.h"
#include "../../proc/percpu.h"

/* The user address a 64-bit field of a call names. */
#define I915_USER_PTR(v) ((void __user *)(uintptr_t)(v))

int i915_capable(void)
{
    return current_proc && proc_privileged(current_proc);
}

/* ------------------------------------------------------------------------
 * The states of a context on an engine
 */

struct i915_engine *i915_engine_lookup_user(struct drm_i915_private *i915, uint16_t cls,
                                            uint16_t instance)
{
    int i;

    if (instance != 0) {
        return 0;
    }
    for (i = 0; i < I915_NUM_ENGINES; i++) {
        if (i915->gt.engine[i].uabi_class == cls) {
            return &i915->gt.engine[i];
        }
    }
    return 0;
}

uint16_t i915_engine_uabi_class(const struct i915_engine *engine)
{
    return engine->uabi_class;
}

/** Add `delta` to `*refs` atomically and return the old count. */
static uint32_t ctx_refs_add(volatile uint32_t *refs, uint32_t delta)
{
    uint32_t old;

    do {
        old = *refs;
    } while (arch_atomic_cmpxchg((volatile unsigned int *)refs, old, old + delta) != old);
    return old;
}

/** Add 1 to `*refs` unless it is 0, as Linux's kref_get_unless_zero: 1
 * when it did. */
static int ctx_refs_get_unless_zero(volatile uint32_t *refs)
{
    uint32_t old;

    do {
        old = *refs;
        if (old == 0) {
            return 0;
        }
    } while (arch_atomic_cmpxchg((volatile unsigned int *)refs, old, old + 1u) != old);
    return 1;
}

/* Over every context's `ce` array, so that EXECBUFFER2, which holds the
 * context but no list lock, takes a state's reference while the close on
 * another processor cannot put the array's. Taken with nothing inside. */
static struct spinlock ce_lock = SPINLOCK_INIT_NAMED("i915_ctx_ce");

struct intel_context *intel_context_create(struct drm_i915_private *i915,
                                           struct i915_engine *engine, struct i915_gem_context *ctx)
{
    struct intel_context *ce = (struct intel_context *)kzalloc(sizeof(*ce));

    if (!ce) {
        return 0;
    }
    ce->i915 = i915;
    ce->engine = engine;
    ce->refs = 1;
    ce->ctx = ctx;
    if (ctx) {
        ctx_refs_add(&ctx->refs, 1u);
    }
    if (engine == &i915->gt.engine[I915_RCS0]) {
        /* The render ring's slices and EUs are the GT's, as Linux's
         * intel_sseu_from_device_info; the other engines have none. */
        i915_sseu_init(i915);
        ce->slice_mask = i915->sseu.slice_mask;
        ce->subslice_mask = i915->sseu.subslice_mask;
        ce->min_eus = ce->max_eus = i915->sseu.eu_per_subslice;
    }
    return ce;
}

struct intel_context *intel_context_get(struct intel_context *ce)
{
    if (ce) {
        ctx_refs_add(&ce->refs, 1u);
    }
    return ce;
}

/** Unmap and free a state's image; for the last reference, under the GEM
 * lock, which keeps the GTT. */
static void intel_context_image_free(struct intel_context *ce)
{
    struct drm_i915_private *i915 = ce->i915;
    uint64_t size = (uint64_t)I915_HSW_CXT_PAGES * I915_GTT_PAGE_SIZE;
    int taken;

    if (!ce->image) {
        return;
    }
    taken = i915_gem_lock(i915);
    (void)i915_ggtt_clear_range(i915, ce->image_ggtt, size, 0);
    i915_ggtt_release(i915, ce->image_ggtt);
    i915_gem_unlock(i915, taken);
    kfree_pages(ce->image, I915_HSW_CXT_PAGES);
    ce->image = 0;
}

void intel_context_put(struct intel_context *ce)
{
    if (!ce || ctx_refs_add(&ce->refs, ~0u) != 1u) {
        return;
    }
    intel_context_image_free(ce);
    if (ce->ctx) {
        i915_gem_context_put(ce->ctx);
    }
    kfree(ce);
}

int32_t intel_context_pin_image(struct intel_context *ce)
{
    struct drm_i915_private *i915 = ce->i915;
    uint64_t pages[I915_HSW_CXT_PAGES], offset;
    uint64_t size = (uint64_t)I915_HSW_CXT_PAGES * I915_GTT_PAGE_SIZE;
    uint8_t *v;
    int32_t err;
    uint32_t i;

    if (ce->image || ce->engine != &i915->gt.engine[I915_RCS0]) {
        return 0;
    }
    v = (uint8_t *)kalloc_pages(I915_HSW_CXT_PAGES, 0);
    if (!v) {
        return -ENOMEM;
    }
    for (i = 0; i < I915_HSW_CXT_PAGES; i++) {
        pages[i] = paging_virt_to_phys((uintptr_t)(v + (size_t)i * I915_GTT_PAGE_SIZE));
        if (pages[i] == 0) {
            kfree_pages(v, I915_HSW_CXT_PAGES);
            return -ENOMEM;
        }
    }
    /* High in the GTT, as Linux pins a context's state (PIN_HIGH). */
    err = i915_ggtt_reserve(i915, size, I915_PIN_HIGH, &offset);
    if (err < 0) {
        kfree_pages(v, I915_HSW_CXT_PAGES);
        return err;
    }
    if (i915->gt.default_state) {
        k_memcpy(v, i915->gt.default_state, (size_t)size);
    } else {
        k_memset(v, 0, (size_t)size);
    }
    err = i915_ggtt_insert(i915, offset, pages, I915_HSW_CXT_PAGES, I915_CACHE_LLC);
    if (err < 0) {
        i915_ggtt_release(i915, offset);
        kfree_pages(v, I915_HSW_CXT_PAGES);
        return err;
    }
    ce->image = (uint32_t *)(void *)v;
    ce->image_ggtt = (uint32_t)offset;
    ce->image_valid = i915->gt.default_state != 0;
    return 0;
}

/* ------------------------------------------------------------------------
 * Contexts
 */

/** A new proto context of `i915` with Linux's defaults, one reference, the
 * file's, or NULL; the calling process names it, as Linux's context's pid
 * and name do. */
static struct i915_gem_context *context_alloc(struct drm_i915_private *i915)
{
    struct i915_gem_context *ctx = (struct i915_gem_context *)kzalloc(sizeof(*ctx));
    uint32_t i;

    if (!ctx) {
        return 0;
    }
    ctx->i915 = i915;
    ctx->refs = 1;
    ctx->proto = 1;
    ctx->nmap = -1;
    if (current_proc) {
        ctx->pid = current_proc->pid;
        for (i = 0; i + 1u < sizeof(ctx->comm) && current_proc->comm[i]; i++) {
            ctx->comm[i] = current_proc->comm[i];
        }
    }
    /* Bannable and recoverable, and persistent while hangcheck is on, as
     * Linux's proto_context_create. */
    ctx->flags = I915_CTX_BANNABLE | I915_CTX_RECOVERABLE;
    if (!i915->params.hangcheck_off) {
        ctx->flags |= I915_CTX_PERSISTENCE;
    }
    return ctx;
}

void i915_gem_context_put(struct i915_gem_context *ctx)
{
    if (!ctx || ctx_refs_add(&ctx->refs, ~0u) != 1u) {
        return;
    }
    kfree(ctx->ce);
    kfree(ctx->map);
    kfree(ctx);
}

/**
 * Give up the file's reference to `ctx`, which is off the file's list, as
 * Linux's context_close: its states let go of, each staying while the
 * requests made on it hold it. A persistent context's requests run on.
 * Under the GEM lock, which the hang paths that read `file` hold.
 */
static void context_close(struct i915_gem_context *ctx)
{
    struct drm_i915_private *i915 = ctx->i915;
    struct intel_context *ce;
    uint32_t i, flags;
    int taken = i915_gem_lock(i915);

    ctx->closed = 1;
    ctx->file = 0;
    for (i = 0; i < ctx->nce; i++) {
        flags = spin_lock_irqsave(&ce_lock);
        ce = ctx->ce[i];
        ctx->ce[i] = 0;
        spin_unlock_irqrestore(&ce_lock, flags);
        intel_context_put(ce);
    }
    i915_gem_unlock(i915, taken);
    i915_gem_context_put(ctx);
}

/**
 * Make the states of a proto context, as Linux's i915_gem_create_context:
 * one for each engine by index, or one for each slot of its map.
 *
 * @return 0, or -ENOMEM with the context left a proto context.
 */
static int32_t context_finalize(struct i915_gem_context *ctx)
{
    struct drm_i915_private *i915 = ctx->i915;
    uint32_t i, n = ctx->nmap >= 0 ? (uint32_t)ctx->nmap : I915_NUM_ENGINES;
    struct intel_context **ce;

    ce = (struct intel_context **)kzalloc((n ? n : 1u) * sizeof(*ce));
    if (!ce) {
        return -ENOMEM;
    }
    for (i = 0; i < n; i++) {
        int idx = ctx->nmap >= 0 ? ctx->map[i] : (int)i;

        if (idx < 0) {
            continue;
        }
        ce[i] = intel_context_create(i915, &i915->gt.engine[idx], ctx);
        if (!ce[i]) {
            while (i-- > 0) {
                intel_context_put(ce[i]);
            }
            kfree(ce);
            return -ENOMEM;
        }
    }
    ctx->ce = ce;
    ctx->nce = n;
    ctx->proto = 0;
    return 0;
}

/** The file's state of `file`. */
static struct i915_file *file_priv(struct drm_file *file)
{
    return (struct i915_file *)file->driver_priv;
}

/** The context `id` of `fp`, proto or not, without a reference, or NULL;
 * with the list's lock held. */
static struct i915_gem_context *context_find(struct i915_file *fp, uint32_t id)
{
    struct i915_gem_context *ctx;

    lockdep_assert_held(&fp->lock);
    for (ctx = fp->contexts; ctx && ctx->id <= id; ctx = ctx->next) {
        if (ctx->id == id) {
            return ctx;
        }
    }
    return 0;
}

/** Give `ctx` the lowest id from 1 that `fp` has free and add it to the
 * file's list, as Linux's xa_alloc on context_xa, with the list's lock
 * held. */
static void context_register(struct i915_file *fp, struct i915_gem_context *ctx)
{
    struct i915_gem_context **pp = &fp->contexts;
    uint32_t id = 1;

    lockdep_assert_held(&fp->lock);
    /* The list is sorted, 0 first: the first gap is the lowest id free. */
    while (*pp && (*pp)->id < id) {
        pp = &(*pp)->next;
    }
    while (*pp && (*pp)->id == id) {
        id++;
        pp = &(*pp)->next;
    }
    ctx->id = id;
    ctx->file = fp;
    ctx->next = *pp;
    *pp = ctx;
}

/* Under the list's lock, as Linux's i915_gem_context_lookup finalizes a
 * proto context under proto_context_lock and takes its reference only
 * while the count is not 0 (gem/i915_gem_context.c:2310-2375), so that
 * one caller alone finalizes it. */
int32_t i915_gem_context_lookup(struct drm_file *file, uint32_t id, struct i915_gem_context **out)
{
    struct i915_file *fp = file_priv(file);
    struct i915_gem_context *ctx;
    int32_t err = 0;

    mutex_lock(&fp->lock);
    ctx = context_find(fp, id);
    if (!ctx) {
        err = -ENOENT;
    } else if (ctx->proto) {
        err = context_finalize(ctx);
    }
    if (err == 0 && !ctx_refs_get_unless_zero(&ctx->refs)) {
        err = -ENOENT;
    }
    mutex_unlock(&fp->lock);
    if (err == 0) {
        *out = ctx;
    }
    return err;
}

struct intel_context *i915_gem_context_engine(struct i915_gem_context *ctx, uint32_t idx)
{
    struct intel_context *ce = 0;
    uint32_t flags;

    if (ctx->proto || idx >= ctx->nce) {
        return 0;
    }
    flags = spin_lock_irqsave(&ce_lock);
    ce = intel_context_get(ctx->ce[idx]);
    spin_unlock_irqrestore(&ce_lock, flags);
    return ce;
}

/** The state an SSEU parameter names, with a reference, as Linux's
 * lookup_user_engine: by class and instance without an engine map, by the
 * slot in `instance` with one and the flag ENGINE_INDEX; -EINVAL. */
static int32_t lookup_user_engine(struct i915_gem_context *ctx, uint32_t flags,
                                  const struct i915_engine_class_instance *ci,
                                  struct intel_context **out)
{
    struct i915_engine *engine;
    uint32_t idx;

    if (!!(flags & I915_CONTEXT_SSEU_FLAG_ENGINE_INDEX) != (ctx->nmap >= 0)) {
        return -EINVAL;
    }
    if (ctx->nmap < 0) {
        engine = i915_engine_lookup_user(ctx->i915, ci->engine_class, ci->engine_instance);
        if (!engine) {
            return -EINVAL;
        }
        idx = (uint32_t)(engine - ctx->i915->gt.engine);
    } else {
        idx = ci->engine_instance;
    }
    *out = i915_gem_context_engine(ctx, idx);
    return *out ? 0 : -EINVAL;
}

/* ------------------------------------------------------------------------
 * Extensions
 */

/** Read `size` bytes at the user address `at`: 0 or -EFAULT. */
static int32_t get_user_bytes(void *dst, uintptr_t at, size_t size)
{
    return copy_from_user(dst, I915_USER_PTR(at), size) != 0 ? -EFAULT : 0;
}

/* ------------------------------------------------------------------------
 * Engine maps
 */

/* An engine map being read: its slots, each an engine index or -1. */
struct engines_set {
    struct drm_i915_private *i915;
    uint32_t n;
    int8_t *map;
};

/** LOAD_BALANCE: a virtual engine over siblings, which only execlists
 * give: -ENODEV, as Linux on Haswell. */
static int32_t engines_balance(uintptr_t ext, void *data)
{
    (void)ext;
    (void)data;
    return -ENODEV;
}

/** BOND, as Linux's set_proto_ctx_engines_bond: checked and then ignored,
 * since nothing on a legacy ring acts on it. */
static int32_t engines_bond(uintptr_t ext, void *data)
{
    struct engines_set *set = (struct engines_set *)data;
    struct i915_engine_class_instance ci;
    uint16_t idx, nbonds;
    int32_t err;
    uint32_t n;

    if (get_user_bytes(&idx, ext + 36u, 2) < 0) {
        return -EFAULT;
    }
    if (idx >= set->n || set->map[idx] < 0) {
        return -EINVAL;
    }
    err = i915_check_user_mbz(ext + 40u, 8);
    for (n = 0; n < 4 && err == 0; n++) {
        err = i915_check_user_mbz(ext + 48u + 8u * n, 8);
    }
    if (err < 0) {
        return err;
    }
    if (get_user_bytes(&ci, ext + 32u, sizeof(ci)) < 0) {
        return -EFAULT;
    }
    if (!i915_engine_lookup_user(set->i915, ci.engine_class, ci.engine_instance)) {
        return -EINVAL;
    }
    if (get_user_bytes(&nbonds, ext + 38u, 2) < 0) {
        return -EFAULT;
    }
    for (n = 0; n < nbonds; n++) {
        if (get_user_bytes(&ci, ext + 80u + 4u * n, sizeof(ci)) < 0) {
            return -EFAULT;
        }
        if (!i915_engine_lookup_user(set->i915, ci.engine_class, ci.engine_instance)) {
            return -EINVAL;
        }
    }
    return 0;
}

/**
 * PARALLEL_SUBMIT, as Linux's set_proto_ctx_engines_parallel_submit
 * without GuC: one sibling for each of `width` contexts, of one class
 * other than render, each the next logical instance of the last. No class
 * here has two engines, so every map fails that last check with -EINVAL,
 * as on Linux on Haswell.
 */
static int32_t engines_parallel(uintptr_t ext, void *data)
{
    struct engines_set *set = (struct engines_set *)data;
    struct i915_engine_class_instance ci, prev = {0, 0};
    uint16_t slot, width, nsiblings;
    uint64_t flags;
    uint32_t prev_mask = 0, i;
    struct i915_engine *engine;
    int32_t err;

    if (get_user_bytes(&slot, ext + 32u, 2) < 0 || get_user_bytes(&width, ext + 34u, 2) < 0 ||
        get_user_bytes(&nsiblings, ext + 36u, 2) < 0) {
        return -EFAULT;
    }
    if (nsiblings != 1 || slot >= set->n || set->map[slot] >= 0) {
        return -EINVAL;
    }
    if (get_user_bytes(&flags, ext + 40u, 8) < 0) {
        return -EFAULT;
    }
    if (flags) {
        return -EINVAL;
    }
    for (i = 0; i < 3; i++) {
        err = i915_check_user_mbz(ext + 48u + 8u * i, 8);
        if (err < 0) {
            return err;
        }
    }
    if (width < 2) {
        return -EINVAL;
    }
    for (i = 0; i < width; i++) {
        if (get_user_bytes(&ci, ext + 72u + 4u * i, sizeof(ci)) < 0) {
            return -EFAULT;
        }
        engine = i915_engine_lookup_user(set->i915, ci.engine_class, ci.engine_instance);
        if (!engine || ci.engine_class == I915_ENGINE_CLASS_RENDER ||
            (i > 0 && prev.engine_class != ci.engine_class)) {
            return -EINVAL;
        }
        prev = ci;
        /* Each engine is logical instance 0 of its class. */
        if (i > 0 && 1u != prev_mask << 1) {
            return -EINVAL;
        }
        prev_mask = 1u;
    }
    /* Not reached: the second context always fails the mask's check. */
    return -EINVAL;
}

static const i915_ext_fn engines_extensions[] = {engines_balance, engines_bond, engines_parallel};

/**
 * ENGINES on a proto context, as Linux's set_proto_ctx_engines: the slots,
 * each an engine or a hole, then their extensions. The caller holds a
 * reference on `ctx`. The copies from the program may sleep, so the
 * context is looked at again before the map is stored: -ENOENT once it
 * is destroyed, -EINVAL once it is finalized or has a map, where Linux's
 * proto_context_lock orders the calls instead.
 */
static int32_t set_engines(struct i915_gem_context *ctx,
                           const struct drm_i915_gem_context_param *args)
{
    struct engines_set set;
    struct i915_engine_class_instance ci;
    struct i915_engine *engine;
    uint64_t extensions;
    uintptr_t user = (uintptr_t)args->value;
    int32_t err;
    uint32_t n;

    if (ctx->nmap >= 0) {
        return -EINVAL;
    }
    if (args->size < I915_CONTEXT_PARAM_ENGINES_HEADER ||
        (args->size - I915_CONTEXT_PARAM_ENGINES_HEADER) % sizeof(ci) != 0) {
        return -EINVAL;
    }
    set.i915 = ctx->i915;
    set.n = (args->size - I915_CONTEXT_PARAM_ENGINES_HEADER) / sizeof(ci);
    if (set.n > I915_CONTEXT_MAX_ENGINES) {
        return -EINVAL;
    }
    set.map = (int8_t *)kmalloc(set.n ? set.n : 1u);
    if (!set.map) {
        return -ENOMEM;
    }
    for (n = 0; n < set.n; n++) {
        if (get_user_bytes(&ci, user + I915_CONTEXT_PARAM_ENGINES_HEADER + 4u * n, sizeof(ci)) <
            0) {
            kfree(set.map);
            return -EFAULT;
        }
        set.map[n] = -1;
        if (ci.engine_class == I915_ENGINE_CLASS_INVALID &&
            ci.engine_instance == I915_ENGINE_CLASS_INVALID_NONE) {
            continue;
        }
        engine = i915_engine_lookup_user(ctx->i915, ci.engine_class, ci.engine_instance);
        if (!engine) {
            kfree(set.map);
            return -ENOENT;
        }
        set.map[n] = (int8_t)(engine - ctx->i915->gt.engine);
    }
    err = get_user_bytes(&extensions, user, 8);
    if (err == 0) {
        err =
            i915_user_extensions(extensions, engines_extensions,
                                 sizeof(engines_extensions) / sizeof(engines_extensions[0]), &set);
    }
    if (err == 0 && (ctx->closed || !ctx->proto || ctx->nmap >= 0)) {
        /* The copies slept: the context was destroyed, finalized or given
         * a map meanwhile. */
        err = ctx->closed ? -ENOENT : -EINVAL;
    }
    if (err < 0) {
        kfree(set.map);
        return err;
    }
    ctx->nmap = (int)set.n;
    ctx->map = set.map;
    return 0;
}

/* ------------------------------------------------------------------------
 * Parameters
 */

/** PRIORITY: -EINVAL with a size, then -ENODEV, since a legacy ring has no
 * scheduler to order by priority (Linux's validate_priority). */
static int32_t set_priority(const struct drm_i915_gem_context_param *args)
{
    return args->size ? -EINVAL : -ENODEV;
}

/** SSEU: -EINVAL for a size short of the structure, then -ENODEV, since
 * only graphics version 11 takes a context's own (Linux's set_sseu). */
static int32_t set_sseu(const struct drm_i915_gem_context_param *args)
{
    return args->size < sizeof(struct drm_i915_gem_context_param_sseu) ? -EINVAL : -ENODEV;
}

/**
 * PERSISTENCE: on, which needs hangcheck on to end a persistent request
 * that hangs and is -EINVAL without it, or off, which needs preemption to
 * cancel the context's work and so is -ENODEV, as Linux's
 * __context_set_persistence. A finalized context that is already as asked
 * is left alone.
 */
static int32_t set_persistence(struct i915_gem_context *ctx,
                               const struct drm_i915_gem_context_param *args)
{
    if (args->size) {
        return -EINVAL;
    }
    if (!ctx->proto && !!(ctx->flags & I915_CTX_PERSISTENCE) == !!args->value) {
        return 0;
    }
    if (!args->value) {
        return -ENODEV;
    }
    if (ctx->i915->params.hangcheck_off) {
        return -EINVAL;
    }
    ctx->flags |= I915_CTX_PERSISTENCE;
    return 0;
}

/**
 * Set a parameter of `ctx`, as Linux's set_proto_ctx_param on a proto
 * context and ctx_setparam on a finalized one: the virtual memory, the
 * engine map and protected content only before the first use.
 *
 * @return 0; -EINVAL for a size where none is taken, a parameter Linux
 *         refuses or one only a proto context takes; -EPERM to make a
 *         context unbannable without privilege; -ENODEV for what Haswell
 *         lacks; or the engine map's errors.
 */
static int32_t context_setparam(struct i915_gem_context *ctx,
                                const struct drm_i915_gem_context_param *args)
{
    switch (args->param) {
    case I915_CONTEXT_PARAM_NO_ERROR_CAPTURE:
        if (args->size) {
            return -EINVAL;
        }
        ctx->flags = args->value ? ctx->flags | I915_CTX_NO_ERROR_CAPTURE
                                 : ctx->flags & ~I915_CTX_NO_ERROR_CAPTURE;
        return 0;
    case I915_CONTEXT_PARAM_BANNABLE:
        if (args->size) {
            return -EINVAL;
        }
        if (!args->value && !i915_capable()) {
            return -EPERM;
        }
        ctx->flags = args->value ? ctx->flags | I915_CTX_BANNABLE : ctx->flags & ~I915_CTX_BANNABLE;
        return 0;
    case I915_CONTEXT_PARAM_RECOVERABLE:
        if (args->size) {
            return -EINVAL;
        }
        ctx->flags =
            args->value ? ctx->flags | I915_CTX_RECOVERABLE : ctx->flags & ~I915_CTX_RECOVERABLE;
        return 0;
    case I915_CONTEXT_PARAM_PRIORITY:
        return set_priority(args);
    case I915_CONTEXT_PARAM_SSEU:
        return set_sseu(args);
    case I915_CONTEXT_PARAM_PERSISTENCE:
        return set_persistence(ctx, args);
    case I915_CONTEXT_PARAM_VM:
        /* A context's own address space needs a full PPGTT. */
        if (!ctx->proto) {
            return -EINVAL;
        }
        return args->size ? -EINVAL : -ENODEV;
    case I915_CONTEXT_PARAM_ENGINES:
        return ctx->proto ? set_engines(ctx, args) : -EINVAL;
    case I915_CONTEXT_PARAM_PROTECTED_CONTENT:
        /* Protected content needs PXP, which Haswell has not. */
        if (!ctx->proto) {
            return -EINVAL;
        }
        return args->value ? -ENODEV : 0;
    default:
        /* LOW_LATENCY needs GuC, CONTEXT_IMAGE a debug build, and
         * NO_ZEROMAP, BAN_PERIOD and RINGSIZE are gone, as on Linux. */
        return -EINVAL;
    }
}

/** SSEU's value: the size of the structure for a size of 0, otherwise the
 * slices and EUs of the state it names, as Linux's get_sseu. */
static int32_t get_sseu(struct i915_gem_context *ctx, struct drm_i915_gem_context_param *args)
{
    struct drm_i915_gem_context_param_sseu u;
    struct intel_context *ce;
    int32_t err;

    if (args->size != 0) {
        if (args->size < sizeof(u)) {
            return -EINVAL;
        }
        if (get_user_bytes(&u, (uintptr_t)args->value, sizeof(u)) < 0) {
            return -EFAULT;
        }
        if (u.rsvd || (u.flags & ~I915_CONTEXT_SSEU_FLAG_ENGINE_INDEX)) {
            return -EINVAL;
        }
        err = lookup_user_engine(ctx, u.flags, &u.engine, &ce);
        if (err < 0) {
            return err;
        }
        u.slice_mask = ce->slice_mask;
        u.subslice_mask = ce->subslice_mask;
        u.min_eus_per_subslice = ce->min_eus;
        u.max_eus_per_subslice = ce->max_eus;
        intel_context_put(ce);
        if (copy_to_user(I915_USER_PTR(args->value), &u, sizeof(u)) != 0) {
            return -EFAULT;
        }
    }
    args->size = sizeof(u);
    return 0;
}

/* ------------------------------------------------------------------------
 * The ioctls
 */

/* What CONTEXT_CREATE's extensions work on. */
struct create_ext {
    struct i915_gem_context *ctx;
};

/** CONTEXT_CREATE's SETPARAM: a parameter set before the context exists,
 * whose context id must be 0. */
static int32_t create_setparam(uintptr_t ext, void *data)
{
    struct drm_i915_gem_context_create_ext_setparam local;

    if (get_user_bytes(&local, ext, sizeof(local)) < 0) {
        return -EFAULT;
    }
    if (local.param.ctx_id) {
        return -EINVAL;
    }
    return context_setparam(((struct create_ext *)data)->ctx, &local.param);
}

/** CONTEXT_CREATE's CLONE, which Linux has dropped: -EINVAL. */
static int32_t create_clone(uintptr_t ext, void *data)
{
    (void)ext;
    (void)data;
    return -EINVAL;
}

static const i915_ext_fn create_extensions[] = {create_setparam, create_clone};

/**
 * GEM_CONTEXT_CREATE, as Linux's i915_gem_context_create_ioctl before
 * graphics version 13: a proto context with the lowest id free, its
 * parameters from the extensions, finalized at its first use.
 *
 * @return 0; -EINVAL for an unknown flag or SINGLE_TIMELINE, which needs
 *         execlists; -EIO when the GPU is wedged or the file is banned;
 *         -ENOMEM; or an extension's error.
 */
int32_t i915_gem_context_create_ioctl(struct drm_device *dev, void *data, struct drm_file *file)
{
    struct drm_i915_private *i915 = to_i915(dev);
    struct drm_i915_gem_context_create_ext *args = (struct drm_i915_gem_context_create_ext *)data;
    struct i915_file *fp = file_priv(file);
    struct create_ext ext;
    int32_t err;

    if (args->flags & I915_CONTEXT_CREATE_FLAGS_UNKNOWN) {
        return -EINVAL;
    }
    if (i915->gt.wedged || fp->ban_score >= I915_CLIENT_SCORE_BANNED) {
        return -EIO;
    }
    if (args->flags & I915_CONTEXT_CREATE_FLAGS_SINGLE_TIMELINE) {
        return -EINVAL;
    }
    ext.ctx = context_alloc(i915);
    if (!ext.ctx) {
        return -ENOMEM;
    }
    if (args->flags & I915_CONTEXT_CREATE_FLAGS_USE_EXTENSIONS) {
        err = i915_user_extensions(args->extensions, create_extensions,
                                   sizeof(create_extensions) / sizeof(create_extensions[0]), &ext);
        if (err < 0) {
            i915_gem_context_put(ext.ctx);
            return err;
        }
    }
    /* Made whole before the list shows it, as Linux's. */
    mutex_lock(&fp->lock);
    context_register(fp, ext.ctx);
    args->ctx_id = ext.ctx->id;
    mutex_unlock(&fp->lock);
    return 0;
}

/**
 * GEM_CONTEXT_DESTROY: the context taken from the file; its requests run
 * on. 0; -EINVAL for a pad that is not 0; -ENOENT for the default
 * context or no such context.
 */
int32_t i915_gem_context_destroy_ioctl(struct drm_device *dev, void *data, struct drm_file *file)
{
    struct drm_i915_gem_context_destroy *args = (struct drm_i915_gem_context_destroy *)data;
    struct i915_file *fp = file_priv(file);
    struct i915_gem_context **pp, *ctx;

    (void)dev;
    if (args->pad != 0) {
        return -EINVAL;
    }
    if (args->ctx_id == 0) {
        return -ENOENT;
    }
    mutex_lock(&fp->lock);
    for (pp = &fp->contexts; *pp && (*pp)->id != args->ctx_id; pp = &(*pp)->next) {
    }
    ctx = *pp;
    if (ctx) {
        *pp = ctx->next;
        ctx->next = 0;
    }
    mutex_unlock(&fp->lock);
    if (!ctx) {
        return -ENOENT;
    }
    context_close(ctx);
    return 0;
}

/**
 * GEM_CONTEXT_GETPARAM, as Linux's i915_gem_context_getparam_ioctl, the
 * context finalized: the GTT's size, the flags, the priority, the slices
 * and EUs, and protected content, each with a size of 0 but SSEU's.
 *
 * @return 0; -ENOENT for no such context; -ENODEV for VM without a full
 *         PPGTT; -EINVAL for a parameter Linux does not read back; or
 *         SSEU's errors.
 */
int32_t i915_gem_context_getparam_ioctl(struct drm_device *dev, void *data, struct drm_file *file)
{
    struct drm_i915_gem_context_param *args = (struct drm_i915_gem_context_param *)data;
    struct i915_gem_context *ctx;
    int32_t err;

    (void)dev;
    err = i915_gem_context_lookup(file, args->ctx_id, &ctx);
    if (err < 0) {
        return err;
    }
    switch (args->param) {
    case I915_CONTEXT_PARAM_GTT_SIZE:
        /* Without a full PPGTT the context works in the global GTT. */
        args->size = 0;
        args->value = ctx->i915->ggtt.total;
        break;
    case I915_CONTEXT_PARAM_NO_ERROR_CAPTURE:
        args->size = 0;
        args->value = !!(ctx->flags & I915_CTX_NO_ERROR_CAPTURE);
        break;
    case I915_CONTEXT_PARAM_BANNABLE:
        args->size = 0;
        args->value = !!(ctx->flags & I915_CTX_BANNABLE);
        break;
    case I915_CONTEXT_PARAM_RECOVERABLE:
        args->size = 0;
        args->value = !!(ctx->flags & I915_CTX_RECOVERABLE);
        break;
    case I915_CONTEXT_PARAM_PRIORITY:
        args->size = 0;
        args->value = I915_CONTEXT_DEFAULT_PRIORITY;
        break;
    case I915_CONTEXT_PARAM_SSEU:
        err = get_sseu(ctx, args);
        break;
    case I915_CONTEXT_PARAM_VM:
        err = -ENODEV;
        break;
    case I915_CONTEXT_PARAM_PERSISTENCE:
        args->size = 0;
        args->value = !!(ctx->flags & I915_CTX_PERSISTENCE);
        break;
    case I915_CONTEXT_PARAM_PROTECTED_CONTENT:
        args->size = 0;
        args->value = 0;
        break;
    default:
        err = -EINVAL;
        break;
    }
    i915_gem_context_put(ctx);
    return err;
}

/**
 * GEM_CONTEXT_SETPARAM, as Linux's i915_gem_context_setparam_ioctl: on a
 * proto context before its first use, on the context after. 0, -ENOENT,
 * or the parameter's errors (context_setparam).
 */
int32_t i915_gem_context_setparam_ioctl(struct drm_device *dev, void *data, struct drm_file *file)
{
    struct drm_i915_gem_context_param *args = (struct drm_i915_gem_context_param *)data;
    struct i915_file *fp = file_priv(file);
    struct i915_gem_context *ctx;
    int32_t err = -ENOENT;

    (void)dev;
    /* Under the list's lock, which keeps a proto context's parameters and
     * its finalizing apart, as Linux's set_proto_ctx_param runs under
     * proto_context_lock (gem/i915_gem_context.c:2619-2640). */
    mutex_lock(&fp->lock);
    ctx = context_find(fp, args->ctx_id);
    if (ctx) {
        err = context_setparam(ctx, args);
    }
    mutex_unlock(&fp->lock);
    return err;
}

/* How old a request THROTTLE waits for is, Linux's
 * DRM_I915_THROTTLE_JIFFIES: 20 ms. */
#define I915_THROTTLE_NS 20000000ll

/**
 * THROTTLE, as Linux's i915_gem_throttle_ioctl: wait, interruptibly, for
 * the newest request of each of the file's contexts on each engine that
 * was added more than 20 ms ago, so that a program runs no further ahead
 * of the GPU. A wait sleeps, and contexts may come and go meanwhile, so
 * the walk starts again after each; a request waited for has completed,
 * so each walk finds fewer. 0; -EIO when the GPU is wedged, which is the
 * ABI's report of it; -EINTR.
 */
int32_t i915_gem_throttle_ioctl(struct drm_device *dev, void *data, struct drm_file *file)
{
    struct drm_i915_private *i915 = to_i915(dev);
    struct i915_file *fp = file_priv(file);
    int64_t before = dma_fence_now() - I915_THROTTLE_NS;
    struct i915_gem_context *ctx;
    struct i915_request *rq;
    int32_t err;
    uint32_t i;

    (void)data;
    if (i915->gt.wedged) {
        return -EIO;
    }
    do {
        rq = 0;
        mutex_lock(&fp->lock);
        for (ctx = fp->contexts; ctx && !rq; ctx = ctx->next) {
            for (i = 0; ctx->ce && i < ctx->nce && !rq; i++) {
                if (ctx->ce[i]) {
                    rq = i915_request_throttle_target(ctx->ce[i], before);
                }
            }
        }
        mutex_unlock(&fp->lock);
        err = rq ? i915_request_wait(rq, I915_WAIT_INTERRUPTIBLE, -1) : 0;
        i915_request_put(rq);
    } while (rq && err == 0);
    return err;
}

/**
 * GET_RESET_STATS, as Linux's i915_gem_context_reset_stats_ioctl, for
 * every context: the GPU's resets, to a privileged caller only, and the
 * context's batches active at a hang and pending behind one. 0, -EINVAL
 * for flags or a pad that are not 0, -ENOENT, or -ENOMEM.
 */
int32_t i915_gem_context_reset_stats_ioctl(struct drm_device *dev, void *data,
                                           struct drm_file *file)
{
    struct drm_i915_reset_stats *args = (struct drm_i915_reset_stats *)data;
    struct i915_gem_context *ctx;
    int32_t err;

    if (args->flags || args->pad) {
        return -EINVAL;
    }
    err = i915_gem_context_lookup(file, args->ctx_id, &ctx);
    if (err < 0) {
        return err;
    }
    args->reset_count = i915_capable() ? to_i915(dev)->gt.reset_count : 0;
    args->batch_active = ctx->guilty_count;
    args->batch_pending = ctx->active_count;
    i915_gem_context_put(ctx);
    return 0;
}

/* ------------------------------------------------------------------------
 * Hangs
 */

/**
 * Raise the ban score of the file of `ctx`, a context that hung the GPU at
 * the tick `now`, as Linux's client_mark_guilty: 3 when the context was
 * banned and 1 more when the file's last hang was within a minute.
 */
static void client_mark_guilty(struct i915_gem_context *ctx, int banned, uint64_t now)
{
    struct i915_file *fp = ctx->file;
    uint32_t score = banned ? I915_CLIENT_SCORE_CONTEXT_BAN : 0u;
    uint64_t prev;

    if (!fp) {
        return;
    }
    prev = fp->hang_ticks;
    fp->hang_ticks = now;
    if (prev != 0 && now - prev < I915_CLIENT_FAST_HANG_TICKS) {
        score += I915_CLIENT_SCORE_HANG_FAST;
    }
    if (score) {
        fp->ban_score += score;
        klog(KLOG_DEBUG, "i915", "reset: client %s[%u]: gained %u ban score, now %u", ctx->comm,
             (unsigned)ctx->pid, (unsigned)score, (unsigned)fp->ban_score);
    }
}

int i915_gem_context_mark_guilty(struct intel_context *ce, uint64_t now)
{
    struct i915_gem_context *ctx = ce->ctx;
    uint64_t prev;
    int banned;

    if (!ctx) {
        return ce->banned;
    }
    if (ctx->closed) {
        return 1;
    }
    ctx->guilty_count++;
    /* An unbannable context, which only a privileged program makes, is
     * never banned, as Linux's reset tests need. */
    if (!(ctx->flags & I915_CTX_BANNABLE)) {
        return 0;
    }
    klog(KLOG_INFO, "i915", "reset: %s[%u] context reset due to GPU hang", ctx->comm,
         (unsigned)ctx->pid);
    prev = ctx->hang_ticks[0];
    ctx->hang_ticks[0] = ctx->hang_ticks[1];
    ctx->hang_ticks[1] = now;
    banned = !(ctx->flags & I915_CTX_RECOVERABLE);
    if (prev != 0 && now - prev < I915_CONTEXT_FAST_HANG_TICKS) {
        banned = 1;
    }
    if (banned) {
        klog(KLOG_DEBUG, "i915", "reset: context %s[%u]: guilty %u, banned", ctx->comm,
             (unsigned)ctx->pid, (unsigned)ctx->guilty_count);
    }
    client_mark_guilty(ctx, banned, now);
    return banned;
}

void i915_gem_context_mark_innocent(struct intel_context *ce)
{
    if (ce->ctx) {
        ce->ctx->active_count++;
    }
}

/* ------------------------------------------------------------------------
 * Files
 */

int32_t i915_driver_open(struct drm_device *dev, struct drm_file *file)
{
    struct drm_i915_private *i915 = to_i915(dev);
    struct i915_file *fp = (struct i915_file *)kzalloc(sizeof(*fp));
    struct i915_gem_context *ctx;

    if (!fp) {
        return -ENOMEM;
    }
    fp->i915 = i915;
    mutex_init(&fp->lock, "i915_ctx_list");
    /* The default context, 0, made whole at once, as Linux's
     * i915_gem_context_open. */
    ctx = context_alloc(i915);
    if (!ctx || context_finalize(ctx) < 0) {
        i915_gem_context_put(ctx);
        kfree(fp);
        return -ENOMEM;
    }
    ctx->id = 0;
    ctx->file = fp;
    fp->contexts = ctx;
    file->driver_priv = fp;
    return 0;
}

void i915_driver_postclose(struct drm_device *dev, struct drm_file *file)
{
    struct i915_file *fp = file_priv(file);
    struct i915_gem_context *ctx, *list;

    if (!fp) {
        return;
    }
    /* The list is taken whole under its lock and closed after it, since
     * the close takes the GEM lock, which comes after the list's. */
    mutex_lock(&fp->lock);
    list = fp->contexts;
    fp->contexts = 0;
    mutex_unlock(&fp->lock);
    while ((ctx = list) != 0) {
        list = ctx->next;
        ctx->next = 0;
        context_close(ctx);
    }
    i915_retire_requests(to_i915(dev));
    kfree(fp);
    file->driver_priv = 0;
}
