/*
 * i915_evict_hosttest.c: the Intel driver's eviction, aperture accounting,
 * purgeable buffers and userptr objects, on the chip of i915_gem_model.c,
 * whose 16 MiB aperture holds what a program wrote through it until the
 * driver syncs it, and whose memory has a limit a row may set.
 * host-deps: rach2/sys/lib/kstring.c rach2/sys/lib/kctypes.c
 * host-deps: tests/host/compat/spinlock_host.c tests/host/compat/atomic_host.c
 * host-deps: tests/host/compat/mmap_lock_host.c tests/host/compat/fd_flat.c
 * host-deps: tests/host/compat/mutex_none.c
 */

#define MODEL_TEST "i915-evict-test"
#include "i915_gem_model.c"

/** No interrupts on the host: the fences' masking is a no-op. */
uint32_t kirq_save(void)
{
    return 0;
}

/** The pair of kirq_save. */
void kirq_restore(uint32_t flags)
{
    (void)flags;
}

/* The working set: 48 objects of 1 MiB, three times the aperture. */
#define SET_OBJECTS 48u
#define SET_BYTES   0x100000u
#define SET_PAGES   (SET_BYTES / 4096u)

/** The dword a row writes at the start of page `page` of object `n`. */
static uint32_t stamp(uint32_t n, uint32_t page)
{
    return 0x5a000000u | (n << 12) | page;
}

/** The binding of the whole object `obj`, or NULL. */
static struct i915_vma *whole(struct drm_i915_gem_object *obj)
{
    struct i915_vma *v;

    for (v = obj->vmas; v; v = v->obj_next) {
        if (v->view_pages == 0) {
            return v;
        }
    }
    return 0;
}

/** The bytes of the aperture the bindings of objects take. */
static uint64_t aperture_used(struct drm_i915_private *i915)
{
    struct i915_vma *v;
    uint64_t used = 0;

    for (v = i915->gem.bound; v; v = v->ggtt_next) {
        if (v->start < APERTURE_BYTES) {
            used += v->node_size;
        }
    }
    return used;
}

/** A fence that has not signalled, put on `obj`'s reservation as a write:
 * the GPU's work on it. The caller signals and puts it. */
static struct dma_fence *busy_fence(struct drm_i915_gem_object *obj)
{
    struct dma_fence *f = (struct dma_fence *)kzalloc(sizeof(*f));

    dma_fence_init(f, 0, dma_fence_context_alloc(1), 1);
    CHECK(dma_resv_add_fence(obj->base.resv, f, 1) == 0, "fence: on the object");
    return f;
}

/** Drop the pin of `*v`, when a pin gave one, and forget it. */
static void drop_pin(struct i915_vma **v)
{
    if (*v) {
        i915_vma_unpin(*v);
        *v = 0;
    }
}

/** `i915_vma_pin` of the whole object: 0 with the binding in `*v`, or the
 * error with `*v` NULL. */
static int32_t pin_whole(struct drm_i915_gem_object *obj, uint32_t flags, struct i915_vma **v)
{
    int32_t err = i915_vma_pin(obj, 0, 0, flags, 0, v);

    if (err != 0) {
        *v = 0;
    }
    return err;
}

/* ---- 78-3b.1: eviction ---- */

/**
 * A working set three times the aperture written through GTT maps, each
 * page's first dword, then read back through GTT maps and PREAD: every
 * fault past the first sixteen evicts the least recently used binding,
 * whose writes reach memory first, and every page holds what was written.
 */
static void working_set(void)
{
    uint32_t h[SET_OBJECTS], n, p;
    vma_t gtt[SET_OBJECTS];
    struct drm_i915_gem_object *obj;
    int32_t err = 0;
    uint32_t bad = 0, over = 0;

    dev_a->gem.evictions = 0;
    for (n = 0; n < SET_OBJECTS; n++) {
        h[n] = create(&file_a, SET_BYTES);
        CHECK(map(&file_a, h[n], I915_MMAP_OFFSET_GTT, SET_PAGES, &gtt[n]) == 0,
              "working set: a GTT map of each object");
    }
    for (n = 0; n < SET_OBJECTS; n++) {
        for (p = 0; p < SET_PAGES; p++) {
            uint8_t *page = fault(&gtt[n], p);

            if (!page) {
                err = -1;
                break;
            }
            *(uint32_t *)page = stamp(n, p);
        }
        over += aperture_used(dev_a) > APERTURE_BYTES;
        if (n == 16) {
            CHECK(!object(&file_a, h[0])->vmas && whole(object(&file_a, h[1])) &&
                      whole(object(&file_a, h[16])),
                  "working set: the 17th object takes the first's place, the least recently used");
        }
    }
    CHECK(err == 0, "working set: every page of 48 MiB faults through a 16 MiB aperture");
    CHECK(!over, "working set: the bindings never pass the aperture");
    CHECK(dev_a->gem.evictions >= SET_OBJECTS - 16,
          "working set: an eviction for each object past the aperture's sixteen");
    for (n = 0; n < SET_OBJECTS; n++) {
        for (p = 0; p < SET_PAGES; p += 37) {
            uint8_t *page = fault(&gtt[n], p);

            bad += !page || *(uint32_t *)page != stamp(n, p);
        }
    }
    CHECK(bad == 0, "working set: read back through rebound GTT maps, each page as written");
    for (n = 0; n < SET_OBJECTS; n++) {
        uint32_t word;

        obj = object(&file_a, h[n]);
        for (p = 0; p < SET_PAGES; p += 51) {
            word = 0;
            if (prw(&file_a, h[n], (uint64_t)p * 4096u, &word, 4, 0) != 0 || word != stamp(n, p)) {
                bad++;
            }
        }
        (void)obj;
    }
    CHECK(bad == 0, "working set: PREAD finds in memory what was written through the aperture");
    for (n = 0; n < SET_OBJECTS; n++) {
        unmap(&gtt[n]);
        close_handle(&file_a, h[n]);
    }
    CHECK(!dev_a->gem.bound && !dev_a->gem.lru_head && !dev_a->gem.lru_tail,
          "working set: closed, nothing bound and the list empty");
}

/**
 * A pinned binding and one whose object the GPU still uses are never
 * evicted: a bind that needs their room and may not wait fails with
 * ENOSPC; one that may waits for the GPU, as Linux's ggtt_flush, and once
 * the busy object's request retires its binding goes, the pinned one
 * staying.
 */
static void pinned_and_active(void)
{
    uint32_t big = create(&file_a, 8u * SET_BYTES), busy = create(&file_a, 8u * SET_BYTES);
    uint32_t h = create(&file_a, SET_BYTES);
    struct drm_i915_gem_object *a = object(&file_a, big), *b = object(&file_a, busy);
    struct i915_vma *va = 0, *vb = 0, *v = 0;
    struct dma_fence *f;
    vma_t gtt;

    CHECK(pin_whole(a, I915_PIN_MAPPABLE, &va) == 0 && pin_whole(b, I915_PIN_MAPPABLE, &vb) == 0,
          "pinned: two objects of 8 MiB fill the aperture");
    if (vb) {
        i915_vma_unpin(vb);
    }
    f = busy_fence(b);
    idle_waits = 0;
    CHECK(pin_whole(object(&file_a, h), I915_PIN_MAPPABLE | I915_PIN_NONBLOCK, &v) == -ENOSPC &&
              whole(a) == va && whole(b) == vb && idle_waits == 0,
          "pinned: without a wait, a pinned binding and an active one stay, the bind ENOSPC");
    CHECK(map(&file_a, h, I915_MMAP_OFFSET_GTT, SET_PAGES, &gtt) == 0, "pinned: a GTT map");
    idle_intr = 1;
    idle_waits = 0;
    CHECK(fault_code(&gtt, 0) == 1 && idle_waits == 1 && !object(&file_a, h)->vmas &&
              !dma_fence_is_signaled(f) && whole(b) == vb && whole(a) == va,
          "pinned: a GTT fault's wait for the GPU ends at a signal, the access to run again as "
          "Linux's, nothing moved");
    idle_intr = 0;
    idle_waits = 0;
    unmap(&gtt);
    idle_signals = f;
    CHECK(pin_whole(object(&file_a, h), I915_PIN_MAPPABLE, &v) == 0 && idle_waits == 1 &&
              dma_fence_is_signaled(f) && !b->vmas && whole(a) == va,
          "pinned: the bind waits for the GPU, then the busy one goes; the pinned one stays");
    dma_fence_put(f);
    drop_pin(&v);
    drop_pin(&va);
    close_handle(&file_a, h);
    close_handle(&file_a, busy);
    close_handle(&file_a, big);
}

/**
 * The GTT beyond the aperture: twenty objects of 4 MiB bound and unbound
 * in turn outside the aperture fill its 64 MiB and evict the least
 * recently used; a kernel's reserved range is never evicted.
 */
static void whole_gtt(void)
{
    uint32_t h[20], n;
    struct i915_vma *v = 0;
    uint64_t kept;
    int32_t err = 0;

    CHECK(i915_ggtt_reserve(dev_a, 4096u, 0, &kept) == 0, "gtt: a page reserved for the kernel");
    dev_a->gem.evictions = 0;
    clears_yielding = 0;
    for (n = 0; n < 20; n++) {
        h[n] = create(&file_a, 4u * SET_BYTES);
        err |= pin_whole(object(&file_a, h[n]), 0, &v);
        if (err == 0) {
            drop_pin(&v);
        }
    }
    CHECK(err == 0 && dev_a->gem.evictions >= 4,
          "gtt: 80 MiB bound in turn in 64 MiB, the oldest evicted");
    CHECK(clears_yielding >= 4, "gtt: each eviction's clear may give way, as its caller may sleep");
    for (v = dev_a->gem.bound; v && v->start != kept; v = v->ggtt_next) {
    }
    CHECK(v && !v->obj, "gtt: the reserved page is still there");
    CHECK(!object(&file_a, h[0])->vmas && object(&file_a, h[19])->vmas,
          "gtt: the first bound went, the last stays");
    for (n = 0; n < 20; n++) {
        close_handle(&file_a, h[n]);
    }
    i915_ggtt_release(dev_a, kept);
}

/* ---- 78-3b.2: GET_APERTURE ---- */

/** GET_APERTURE's sizes as pins, reservations, kept surfaces and the GT's
 * own objects take the GTT. */
static void get_aperture(void)
{
    struct drm_i915_gem_get_aperture ap;
    uint32_t h = create(&file_a, SET_BYTES);
    struct i915_vma *v = 0;
    uint64_t res;

    k_memset(&ap, 0xff, sizeof(ap));
    CHECK(i915_gem_get_aperture_ioctl(&dev_a->drm, &ap, &file_a) == 0 &&
              ap.aper_size == GTT_BYTES && ap.aper_available_size == GTT_BYTES,
          "aperture: the GTT's 64 MiB, all of it available");
    CHECK(pin_whole(object(&file_a, h), I915_PIN_MAPPABLE, &v) == 0, "aperture: an object pinned");
    (void)i915_gem_get_aperture_ioctl(&dev_a->drm, &ap, &file_a);
    CHECK(ap.aper_available_size == GTT_BYTES - SET_BYTES, "aperture: its 1 MiB taken");
    drop_pin(&v);
    (void)i915_gem_get_aperture_ioctl(&dev_a->drm, &ap, &file_a);
    CHECK(ap.aper_available_size == GTT_BYTES && object(&file_a, h)->vmas,
          "aperture: unpinned, still bound, available again as on Linux");
    CHECK(i915_ggtt_reserve(dev_a, 8192u, 0, &res) == 0, "aperture: two pages reserved");
    dev_a->ggtt.nkept = 1;
    dev_a->ggtt.kept[0].start = GTT_BYTES - 0x200000u;
    dev_a->ggtt.kept[0].end = GTT_BYTES - 0x100000u;
    dev_a->gt.ggtt_bottom = GTT_BYTES - 0x100000u;
    (void)i915_gem_get_aperture_ioctl(&dev_a->drm, &ap, &file_a);
    CHECK(ap.aper_available_size == GTT_BYTES - 8192u - 0x200000u,
          "aperture: a reserved range, a kept surface and the GT's objects taken");
    dev_a->ggtt.nkept = 0;
    dev_a->gt.ggtt_bottom = GTT_BYTES;
    i915_ggtt_release(dev_a, res);
    close_handle(&file_a, h);
}

/* ---- 78-3b.3: MADVISE and purging ---- */

/** MADVISE's answer for `handle` and `madv`, `retained` in `*kept`. */
static int32_t madvise(uint32_t handle, uint32_t madv, uint32_t *kept)
{
    struct drm_i915_gem_madvise m = {0};
    int32_t err;

    m.handle = handle;
    m.madv = madv;
    m.retained = 7;
    err = i915_gem_madvise_ioctl(&dev_a->drm, &m, &file_a);
    *kept = m.retained;
    return err;
}

/** MADVISE's refusals and states, and an object without pages purged at
 * once, as Linux discards its storage. */
static void madvise_states(void)
{
    uint32_t h = create(&file_a, 8192), e = create(&file_a, 8192), kept;
    uint8_t buf[64];

    CHECK(madvise(h, __I915_MADV_PURGED, &kept) == -EINVAL, "madvise: the purged state, EINVAL");
    CHECK(madvise(999, I915_MADV_DONTNEED, &kept) == -ENOENT, "madvise: no such object, ENOENT");
    CHECK(prw(&file_a, h, 0, buf, sizeof(buf), 1) == 0, "madvise: an object with pages");
    CHECK(madvise(h, I915_MADV_DONTNEED, &kept) == 0 && kept == 1 &&
              object(&file_a, h)->madv == I915_MADV_DONTNEED,
          "madvise: DONTNEED with pages, retained");
    CHECK(madvise(h, I915_MADV_WILLNEED, &kept) == 0 && kept == 1,
          "madvise: WILLNEED again, retained");
    CHECK(madvise(e, I915_MADV_DONTNEED, &kept) == 0 && kept == 0 &&
              object(&file_a, e)->madv == __I915_MADV_PURGED,
          "madvise: DONTNEED without pages, purged at once");
    CHECK(madvise(e, I915_MADV_WILLNEED, &kept) == 0 && kept == 0, "madvise: purged stays purged");
    CHECK(prw(&file_a, e, 0, buf, sizeof(buf), 0) == -EFAULT &&
              domain(&file_a, e, I915_GEM_DOMAIN_CPU, 0) == -EFAULT,
          "madvise: a purged object's PREAD and SET_DOMAIN, EFAULT");
    close_handle(&file_a, e);
    close_handle(&file_a, h);
}

/* The objects of the pressure rows: `p` purgeable, bound and mapped
 * write-back in `cpu`; `q` needed; `b` purgeable but busy with `f`; `k`
 * purgeable but bound and pinned in `kv`. `fill` is what `p` and `q` hold. */
struct pressure_set {
    uint32_t p, q, b, k;
    struct drm_i915_gem_object *po, *bo, *ko;
    struct i915_vma *kv;
    struct dma_fence *f;
    vma_t cpu;
};

static uint8_t fill[SET_BYTES];

/** Make the pressure rows' objects, each with pages, and register the
 * shrinker. */
static void pressure_setup(struct pressure_set *ps)
{
    struct i915_vma *v = 0;
    uint32_t kept, i;
    uint8_t *page;

    ps->p = create(&file_a, 2u * SET_BYTES);
    ps->q = create(&file_a, SET_BYTES);
    ps->b = create(&file_a, 256u * 1024u);
    ps->k = create(&file_a, 256u * 1024u);
    ps->po = object(&file_a, ps->p);
    ps->bo = object(&file_a, ps->b);
    ps->ko = object(&file_a, ps->k);
    i915_gem_shrinker_register(dev_a);
    for (i = 0; i < SET_BYTES; i++) {
        fill[i] = (uint8_t)(i * 3u + 1u);
    }
    CHECK(prw(&file_a, ps->q, 0, fill, SET_BYTES, 1) == 0 &&
              prw(&file_a, ps->p, 0, fill, SET_BYTES, 1) == 0 &&
              domain(&file_a, ps->p, I915_GEM_DOMAIN_CPU, 0) == 0 &&
              domain(&file_a, ps->b, I915_GEM_DOMAIN_CPU, 0) == 0 &&
              domain(&file_a, ps->k, I915_GEM_DOMAIN_CPU, 0) == 0,
          "pressure: four objects with pages");
    CHECK(map(&file_a, ps->p, I915_MMAP_OFFSET_WB, 512, &ps->cpu) == 0 &&
              (page = fault(&ps->cpu, 3)) != 0 && page[0] == fill[0],
          "pressure: the purgeable one mapped write-back");
    CHECK(pin_whole(ps->po, I915_PIN_MAPPABLE, &v) == 0, "pressure: and bound");
    drop_pin(&v);
    ps->f = busy_fence(ps->bo);
    CHECK(pin_whole(ps->ko, 0, &ps->kv) == 0, "pressure: one bound and pinned");
    CHECK(madvise(ps->p, I915_MADV_DONTNEED, &kept) == 0 &&
              madvise(ps->b, I915_MADV_DONTNEED, &kept) == 0 &&
              madvise(ps->k, I915_MADV_DONTNEED, &kept) == 0 && kept == 1,
          "pressure: three DONTNEED, one busy and one pinned");
}

/**
 * Under a limit with room for 1 MiB more, an object of 2 MiB is given once
 * reclaim purges the DONTNEED object, its maps revoked and its binding
 * undone, while the needed, the busy and the pinned objects stay whole.
 * Returns the new object's handle.
 */
static uint32_t pressure_purge(struct pressure_set *ps)
{
    struct drm_i915_gem_object *po = ps->po;
    uint32_t r, kept, i, bad = 0;

    frame_limit = frames_live + 256;
    reclaims = 0;
    zaps = 0;
    r = create(&file_a, 2u * SET_BYTES);
    CHECK(domain(&file_a, r, I915_GEM_DOMAIN_CPU, 0) == 0, "pressure: 2 MiB more, given");
    CHECK(reclaims > 0 && dev_a->gem.purged == 1 && po->madv == __I915_MADV_PURGED && !po->frames &&
              !po->vmas && po->pages.held == 0,
          "pressure: reclaim purged the DONTNEED object, binding and pages gone");
    CHECK(zaps >= I915_MMAP_TYPES && fault(&ps->cpu, 3) == 0,
          "pressure: its maps revoked, a touch after it SIGBUS");
    CHECK(madvise(ps->p, I915_MADV_WILLNEED, &kept) == 0 && kept == 0, "pressure: not retained");
    CHECK(ps->bo->frames && ps->bo->madv == I915_MADV_DONTNEED && ps->ko->frames &&
              whole(ps->ko) == ps->kv,
          "pressure: the busy and the pinned DONTNEED objects kept");
    for (i = 0; i < SET_BYTES; i += 4096) {
        uint8_t word[4];

        bad += prw(&file_a, ps->q, i, word, 4, 0) != 0 || memcmp(word, fill + i, 4) != 0;
    }
    CHECK(bad == 0, "pressure: the WILLNEED object whole");
    return r;
}

/**
 * Reclaim under a limit of memory: a DONTNEED object bound and mapped is
 * purged before an allocation that needs its room fails; past what reclaim
 * frees, the allocation fails with ENOMEM and leaves no frame of its own
 * pinned; once idle and unpinned the other two are purged too.
 */
static void pressure(void)
{
    struct pressure_set ps;
    uint32_t r, s;

    pressure_setup(&ps);
    r = pressure_purge(&ps);
    s = create(&file_a, 4u * SET_BYTES);
    CHECK(domain(&file_a, s, I915_GEM_DOMAIN_CPU, 0) == -ENOMEM && !object(&file_a, s)->frames &&
              frames_live <= frame_limit,
          "pressure: beyond memory, ENOMEM, nothing pinned");
    close_handle(&file_a, s);
    frame_limit = 0;
    dma_fence_signal(ps.f);
    dma_fence_put(ps.f);
    drop_pin(&ps.kv);
    CHECK(i915_gem_shrink(dev_a, 1u << 20) == 128u && ps.bo->madv == __I915_MADV_PURGED &&
              ps.ko->madv == __I915_MADV_PURGED,
          "pressure: once idle and unpinned, the other two purged, 128 frames");
    i915_gem_shrinker_unregister(dev_a);
    CHECK(!shrinker_list, "pressure: the shrinker taken off");
    unmap(&ps.cpu);
    close_handle(&file_a, r);
    close_handle(&file_a, ps.k);
    close_handle(&file_a, ps.b);
    close_handle(&file_a, ps.q);
    close_handle(&file_a, ps.p);
}

/* ---- 78-3b.4: USERPTR ---- */

static uint8_t *user_mem;

/** GEM_USERPTR's answer for `ptr`, `size` and `flags`, the handle in
 * `*handle`. */
static int32_t userptr(uintptr_t ptr, uint64_t size, uint32_t flags, uint32_t *handle)
{
    struct drm_i915_gem_userptr u = {0};
    int32_t err;

    u.user_ptr = ptr;
    u.user_size = size;
    u.flags = flags;
    err = i915_gem_userptr_ioctl(&dev_a->drm, &u, &file_a);
    *handle = u.handle;
    return err;
}

/** The program's memory: 64 KiB of the test's at `user_mem`, writable, then
 * a device's mapping of 16 KiB. */
static void user_space(void)
{
    void *p;

    CHECK(posix_memalign(&p, 4096, 0x14000) == 0, "userptr: the program's memory");
    user_mem = (uint8_t *)p;
    k_memset(&user_regions, 0, sizeof(user_regions));
    user_regions[0].start = (uintptr_t)user_mem;
    user_regions[0].end = (uintptr_t)user_mem + 0x10000;
    user_regions[0].prot = VM_PROT_READ | VM_PROT_WRITE;
    user_regions[1].start = user_regions[0].end;
    user_regions[1].end = user_regions[1].start + 0x4000;
    user_regions[1].prot = VM_PROT_READ | VM_PROT_WRITE;
    user_regions[1].file = &device_file;
    user_nregions = 2;
}

/** GEM_USERPTR's refusals, as Linux 6.12's. */
static void userptr_refusals(void)
{
    uintptr_t m = (uintptr_t)user_mem;
    uint32_t h;

    CHECK(userptr(m, 4096, 0x4, &h) == -EINVAL, "userptr: an unknown flag, EINVAL");
    CHECK(userptr(m, 0, 0, &h) == -EINVAL, "userptr: no bytes, EINVAL");
    CHECK(userptr(m + 16, 4096, 0, &h) == -EINVAL && userptr(m, 100, 0, &h) == -EINVAL,
          "userptr: not whole pages, EINVAL");
    CHECK(userptr((uintptr_t)-4096, 8192, 0, &h) == -EFAULT,
          "userptr: a range that wraps past user space, EFAULT");
    CHECK(userptr(m, SHMEM_MAX_SIZE + 4096u, 0, &h) == -E2BIG,
          "userptr: a size past what an object holds, E2BIG");
    CHECK(userptr(0, 4096, I915_USERPTR_PROBE, &h) == -EFAULT,
          "userptr: PROBE of a pointer of 0, EFAULT");
    CHECK(userptr(0, 4096, 0, &h) == 0 && h, "userptr: a pointer of 0 without PROBE, as Linux's");
    CHECK(domain(&file_a, h, I915_GEM_DOMAIN_CPU, 0) == -EFAULT,
          "userptr: and its first use, EFAULT, as IGT's invalid-null-pointer expects");
    close_handle(&file_a, h);
    CHECK(userptr(m, 4096, I915_USERPTR_UNSYNCHRONIZED, &h) == -ENODEV,
          "userptr: UNSYNCHRONIZED, ENODEV as Linux 6.12 for everyone");
    CHECK(userptr(m, 4096, I915_USERPTR_READ_ONLY, &h) == -ENODEV,
          "userptr: READ_ONLY, ENODEV without read-only GTT entries");
    CHECK(userptr(m + 0x10000, 0x4000, I915_USERPTR_PROBE, &h) == -EFAULT,
          "userptr: PROBE over a device's mapping, EFAULT");
    CHECK(userptr(m + 0x14000, 4096, I915_USERPTR_PROBE, &h) == -EFAULT,
          "userptr: PROBE over a hole, EFAULT");
    CHECK(userptr(m, 0x10000, I915_USERPTR_PROBE, &h) == 0 && h, "userptr: PROBE over memory");
    close_handle(&file_a, h);
}

/** A userptr object's calls that Linux refuses: PREAD, PWRITE, every map,
 * SET_TILING, SET_CACHING but CACHED, and PRIME's export. */
static void userptr_calls(void)
{
    struct drm_i915_gem_set_tiling t;
    struct drm_i915_gem_caching cc = {0};
    struct drm_prime_handle ph = {0, DRM_RDWR, -1};
    uint8_t buf[16];
    uint32_t h;
    int32_t err;
    uint64_t kind;
    int refused = 0;

    CHECK(userptr((uintptr_t)user_mem, 0x10000, 0, &h) == 0, "userptr: an object of 64 KiB");
    CHECK(prw(&file_a, h, 0, buf, 16, 0) == -EINVAL && prw(&file_a, h, 0, buf, 16, 1) == -EINVAL,
          "userptr: PREAD and PWRITE, EINVAL");
    for (kind = I915_MMAP_OFFSET_GTT; kind <= I915_MMAP_OFFSET_UC; kind++) {
        (void)mmap_offset(&file_a, h, kind, &err);
        refused += err == -ENODEV;
    }
    CHECK(refused == 4, "userptr: every kind of map, ENODEV");
    CHECK(tile(&file_a, h, I915_TILING_X, 512, &t) == -ENXIO, "userptr: SET_TILING, ENXIO");
    cc.handle = h;
    cc.caching = I915_CACHING_NONE;
    CHECK(i915_gem_set_caching_ioctl(&dev_a->drm, &cc, &file_a) == -ENXIO,
          "userptr: SET_CACHING to none, ENXIO");
    cc.caching = I915_CACHING_CACHED;
    CHECK(i915_gem_set_caching_ioctl(&dev_a->drm, &cc, &file_a) == 0,
          "userptr: SET_CACHING to cached, which it is");
    ph.handle = h;
    CHECK(drm_prime_handle_to_fd_ioctl(&dev_a->drm, &ph, &file_a) == -EINVAL,
          "userptr: PRIME's export, EINVAL");
    close_handle(&file_a, h);
}

/** Whether the GTT maps `obj`'s binding `v` on the program's pages. */
static int maps_user(struct drm_i915_gem_object *obj, struct i915_vma *v)
{
    uint32_t i, n = (uint32_t)(obj->base.size >> 12);

    for (i = 0; i < n; i++) {
        if (chip_a.gtt[(v->start >> 12) + i] != (uintptr_t)user_mem + (uintptr_t)i * 4096u) {
            return 0;
        }
    }
    return 1;
}

/** Whether the GTT's 16 entries from `start` all map nothing. */
static int gtt_clear(uint64_t start)
{
    uint32_t i;

    for (i = 0; i < 16; i++) {
        if (chip_a.gtt[(start >> 12) + i]) {
            return 0;
        }
    }
    return 1;
}

/* The object another thread pins while this one's pin sleeps. */
static struct drm_i915_gem_object *raced_obj;

/** Another thread's pin of `raced_obj`'s pages. */
static void pin_raced(void)
{
    CHECK(i915_gem_object_pin_pages(raced_obj) == 0, "race: the other thread's pin");
}

/** Another thread's DONTNEED on `raced_obj` and the reclaim that purges
 * it. */
static void purge_raced(void)
{
    raced_obj->madv = I915_MADV_DONTNEED;
    (void)i915_gem_object_truncate(raced_obj);
}

/**
 * Two pins of one object's pages while reclaim sleeps: the second counts
 * one more pin on the first's pages, and nothing leaks; an object purged
 * meanwhile gets no pages.
 */
static void pin_raced_reclaim(void)
{
    uint32_t h = create(&file_a, 0x4000);
    int pins = pins_live, frames = frames_live;

    raced_obj = object(&file_a, h);
    balance_sleep = pin_raced;
    CHECK(i915_gem_object_pin_pages(raced_obj) == 0 && raced_obj->pages_pin_count == 2 &&
              pins_live == pins + 1 && frames_live == frames + 4,
          "race: a pin while reclaim sleeps counts one more on the first pages, nothing leaked");
    i915_gem_object_unpin_pages(raced_obj);
    i915_gem_object_unpin_pages(raced_obj);
    close_handle(&file_a, h);
    CHECK(pins_live == pins && frames_live == frames, "race: closed, every page and pin gone");

    h = create(&file_a, 0x4000);
    raced_obj = object(&file_a, h);
    balance_sleep = purge_raced;
    CHECK(i915_gem_object_pin_pages(raced_obj) == -EFAULT && !raced_obj->frames &&
              raced_obj->madv == __I915_MADV_PURGED && pins_live == pins && frames_live == frames,
          "race: purged while reclaim sleeps, EFAULT and no pages");
    close_handle(&file_a, h);
}

/** Another thread's pin of the userptr object `raced_obj` while this
 * one's pin of the range sleeps. */
static void pin_userptr_raced(void)
{
    CHECK(i915_gem_object_pin_pages(raced_obj) == 0, "userptr: the other thread's pin");
}

/** Another thread's mprotect while the pin sleeps. */
static void protect_during_pin(void)
{
    mmn_notify(&user_mm, MMN_PROTECT, (uintptr_t)user_mem, (uintptr_t)user_mem + 4096u);
}

/**
 * A userptr object's pages, pinned: a second pin while the first sleeps
 * counts one more on its frames, and a change while the GPU uses the
 * object keeps its binding and frames until the next use waits for the GPU.
 */
static void userptr_raced(struct drm_i915_gem_object *obj, uint32_t h)
{
    uintptr_t m = (uintptr_t)user_mem;
    struct i915_vma *v = 0;
    struct dma_fence *f;
    uint64_t start;

    /* Two pins of the range, the second while the first sleeps. */
    mmn_notify(&user_mm, MMN_PROTECT, m, m + 0x10000);
    raced_obj = obj;
    pin_sleep = pin_userptr_raced;
    CHECK(i915_gem_object_pin_pages(obj) == 0 && obj->pages_pin_count == 2 && user_pins == 16,
          "userptr: a pin while another sleeps counts one more on its frames, none leaked");
    i915_gem_object_unpin_pages(obj);
    i915_gem_object_unpin_pages(obj);

    /* An unmap while the GPU uses the object: its binding and frames stay
     * until the work is done, and go at the next use. */
    CHECK(pin_whole(obj, 0, &v) == 0, "userptr: bound for a batch");
    start = v->start;
    drop_pin(&v);
    f = busy_fence(obj);
    mmn_notify(&user_mm, MMN_PROTECT, m, m + 0x10000);
    CHECK(obj->vmas && !gtt_clear(start) && obj->userptr.stale && user_pins == 16,
          "userptr: a change while the GPU uses it keeps the binding and the frames");
    signal_on_wait = f;
    CHECK(domain(&file_a, h, I915_GEM_DOMAIN_CPU, 0) == 0 && dma_fence_is_signaled(f) &&
              !obj->vmas && gtt_clear(start) && !obj->userptr.stale && user_pins == 16,
          "userptr: the next use waits for the GPU, undoes the binding and pins anew");
    dma_fence_put(f);
}

/**
 * A userptr object's pages pinned when it is used and bound, and let go,
 * its bindings revoked first, when the program unmaps, changes protection,
 * forks or exits; a pinned binding maps the scratch page until its last
 * pin, which frees the frames; another address space and a pin raced by a
 * change are refused or tried again.
 */
static void userptr_pages(void)
{
    uintptr_t m = (uintptr_t)user_mem;
    struct drm_i915_gem_object *obj;
    struct i915_vma *v = 0;
    uint64_t start;
    struct mm other;
    uint32_t h;

    CHECK(userptr(m, 0x10000, 0, &h) == 0, "userptr: an object over the program's 64 KiB");
    obj = object(&file_a, h);
    CHECK(user_pins == 0 && !obj->frames, "userptr: nothing pinned at its making");
    CHECK(domain(&file_a, h, I915_GEM_DOMAIN_CPU, I915_GEM_DOMAIN_CPU) == 0 && user_pins == 16 &&
              obj->read_domains == I915_GEM_DOMAIN_CPU,
          "userptr: SET_DOMAIN pins its 16 pages, as Mesa's check of it");
    CHECK(pin_whole(obj, I915_PIN_MAPPABLE, &v) == 0 && maps_user(obj, v),
          "userptr: bound, the GTT maps the program's pages");
    user_mem[4096 + 8] = 0x77;
    chip_copy(&chip_a, v->start, 0x10000, 0);
    CHECK(chip_a.ap[v->start + 4096 + 8] == 0x77, "userptr: the GPU reads what the program wrote");
    start = v->start;
    drop_pin(&v);

    /* munmap of one page: the binding undone, its entries cleared, the
     * frames let go. */
    clears = 0;
    clears_yielding = 0;
    mmn_notify(&user_mm, MMN_UNMAP, m + 0x3000, m + 0x4000);
    CHECK(!obj->vmas && gtt_clear(start) && user_pins == 0 && !obj->frames,
          "userptr: an unmap undoes the binding and lets the 16 frames go");
    CHECK(clears > 0 && clears_yielding == 0,
          "userptr: the notifier's clear gives no way, since its caller holds the big lock");
    CHECK(pin_whole(obj, 0, &v) == 0 && user_pins == 16 && maps_user(obj, v),
          "userptr: used again, pinned and bound again");

    /* mprotect while the binding is pinned: it maps the scratch page, the
     * frames held until its last pin. */
    start = v->start;
    mmn_notify(&user_mm, MMN_PROTECT, m, m + 0x10000);
    CHECK(obj->vmas == v && gtt_clear(start) && obj->userptr.stale && user_pins == 16,
          "userptr: mprotect under a pin, the binding on the scratch page, the frames held");
    CHECK(domain(&file_a, h, I915_GEM_DOMAIN_CPU, 0) == -EAGAIN,
          "userptr: while a pin holds the old frames, EAGAIN");
    drop_pin(&v);
    CHECK(!obj->vmas && user_pins == 0 && !obj->userptr.stale,
          "userptr: the last pin undoes it and the frames go");

    /* A fork makes the pages copy-on-write, which tells MMN_PROTECT. */
    CHECK(pin_whole(obj, 0, &v) == 0, "userptr: bound for the fork");
    drop_pin(&v);
    mmn_notify(&user_mm, MMN_PROTECT, m, m + 0x10000);
    CHECK(!obj->vmas && user_pins == 0, "userptr: a fork undoes it and lets the frames go");

    /* A change while the pin sleeps: tried again. */
    pin_sleep = protect_during_pin;
    CHECK(domain(&file_a, h, I915_GEM_DOMAIN_CPU, 0) == 0 && user_pins == 16 && obj->frames,
          "userptr: an mprotect during the pin, pinned again");

    userptr_raced(obj, h);

    /* Another address space may not pin it. */
    mmn_notify(&user_mm, MMN_COW, m, m + 4096u);
    CHECK(user_pins == 0, "userptr: a copy-on-write break lets the frames go");
    k_memset(&other, 0, sizeof(other));
    self.mm = &other;
    CHECK(domain(&file_a, h, I915_GEM_DOMAIN_CPU, 0) == -EFAULT,
          "userptr: another address space, EFAULT");
    self.mm = &user_mm;

    /* A page gone from the program's memory. */
    user_regions[0].end = m + 0x8000;
    CHECK(domain(&file_a, h, I915_GEM_DOMAIN_CPU, 0) == -EFAULT && user_pins == 0,
          "userptr: half the range unmapped, EFAULT, nothing pinned");
    user_regions[0].end = m + 0x10000;

    /* The end of the address space. */
    CHECK(pin_whole(obj, 0, &v) == 0, "userptr: bound before the exit");
    drop_pin(&v);
    mmn_release(&user_mm);
    CHECK(!obj->vmas && user_pins == 0 && !obj->userptr.range.mm,
          "userptr: the exit undoes the binding, lets the frames go and ends the range");
    CHECK(domain(&file_a, h, I915_GEM_DOMAIN_CPU, 0) == -EFAULT, "userptr: after the exit, EFAULT");
    close_handle(&file_a, h);

    /* An object closed while pinned and bound lets everything go. */
    CHECK(userptr(m, 0x10000, 0, &h) == 0, "userptr: another object");
    CHECK(pin_whole(object(&file_a, h), 0, &v) == 0 && user_pins == 16, "userptr: bound");
    drop_pin(&v);
    close_handle(&file_a, h);
    CHECK(user_pins == 0 && !user_mm.notify, "userptr: closed, frames and range gone");
}

/* ---- 84-17b.2: the free gives way wherever its holder may ---- */

/** Make a bound, idle object of 1 MiB with its pages and close its handle,
 * keeping one reference: the object, whose next put is its last. */
static struct drm_i915_gem_object *held_bound(void)
{
    uint32_t h = create(&file_a, SET_BYTES);
    struct drm_i915_gem_object *obj = object(&file_a, h);
    struct i915_vma *v = 0;

    CHECK(pin_whole(obj, 0, &v) == 0 && obj->pages.held == SET_PAGES,
          "free: an object bound, with its pages");
    drop_pin(&v);
    drm_gem_object_get(&obj->base);
    close_handle(&file_a, h);
    return obj;
}

/** Zero the counts of clears, page frees and give-ways. */
static void free_counts_zero(void)
{
    clears = 0;
    clears_yielding = 0;
    frees_yielding = 0;
    give_ways = 0;
}

/** A GEM_CLOSE of a bound idle object with the GEM lock free undoes it
 * at once, its clear and its pages giving way, as Linux's free worker
 * does for every free (gem/i915_gem_object.c:423). */
static void free_on_close(void)
{
    struct i915_vma *v = 0;
    uint32_t h = create(&file_a, SET_BYTES);
    int frames;

    CHECK(pin_whole(object(&file_a, h), 0, &v) == 0 && v->obj->pages.held == SET_PAGES,
          "free: an object bound, with its pages, behind its handle alone");
    drop_pin(&v);
    frames = frames_live;
    free_counts_zero();
    close_handle(&file_a, h);
    CHECK(clears == 1 && clears_yielding == 1 && frees_yielding == 1 &&
              frames_live == frames - (int)SET_PAGES && !dev_a->gem.freed,
          "free: a GEM_CLOSE with the lock free clears and frees the pages giving way");
}

/** A last put under a held GEM lock, as retirement's, leaves the object on
 * `gem.freed` until the holder that took the lock lets it go, which undoes
 * it giving way. */
static void free_under_lock(void)
{
    struct drm_i915_gem_object *obj = held_bound();
    int frames = frames_live, taken;

    free_counts_zero();
    taken = i915_gem_lock(dev_a);
    i915_gem_object_put(obj);
    CHECK(taken && dev_a->gem.freed == obj && clears == 0 && frames_live == frames,
          "free: a last put under a held lock waits on the list, undone at no point inside it");
    i915_gem_unlock(dev_a, taken);
    CHECK(!dev_a->gem.freed && clears == 1 && clears_yielding == 1 && frees_yielding == 1 &&
              give_ways == 1 && frames_live == frames - (int)SET_PAGES &&
              !mutex_held(&dev_a->gem.lock),
          "free: the holder that took the lock frees the list as it lets go, giving way");
}

/** A last put under the shrinker, the pool's reference the last, undoes
 * the object at once with no give-way and counts its frames; the
 * shrinker, taking the lock in a wait's drop of it, undoes `gem.freed` the
 * same way and counts its frames too. */
static void free_under_shrinker(void)
{
    struct mem_shrinker *sk = &dev_a->gem.shrinker.base;
    struct drm_i915_gem_object *queued;
    int frames, taken;

    pool_parked = held_bound();
    frames = frames_live;
    free_counts_zero();
    i915_gem_shrinker_register(dev_a);
    CHECK(sk->shrink(sk, 1, 1) == SET_PAGES && !pool_parked && !dev_a->gem.freed && clears == 1 &&
              clears_yielding == 0 && frees_yielding == 0 && give_ways == 0 &&
              frames_live == frames - (int)SET_PAGES,
          "free: under the shrinker, the object goes at once with no give-way, its frames counted");

    queued = held_bound();
    pool_parked = held_bound();
    frames = frames_live;
    taken = i915_gem_lock(dev_a);
    i915_gem_object_put(queued);
    CHECK(i915_gem_lock_drop(dev_a) == 1, "free: a wait lets the lock go, the list kept");
    free_counts_zero();
    CHECK(sk->shrink(sk, 1, 1) == 2 * SET_PAGES && !dev_a->gem.freed && clears == 2 &&
              clears_yielding == 0 && give_ways == 0 &&
              frames_live == frames - 2 * (int)SET_PAGES && !mutex_held(&dev_a->gem.lock),
          "free: the shrinker, in a wait's drop of the lock, frees the list and its own, counted");
    i915_gem_lock_retake(dev_a, 1);
    i915_gem_unlock(dev_a, taken);
    CHECK(clears == 2 && clears_yielding == 0 && frees_yielding == 0 && give_ways == 0,
          "free: the holder finds the list empty once it lets the lock go");
    i915_gem_shrinker_unregister(dev_a);
}

/** The shrinker under its caller's own GEM lock, as an allocation of
 * EXECBUFFER2's that reclaims, undoes `gem.freed` with no give-way and
 * counts its frames, as Linux's shrinker reaches an object its free worker
 * has not yet freed (gem/i915_gem_shrinker.c:186-192). */
static void free_shrinker_holder(void)
{
    struct mem_shrinker *sk = &dev_a->gem.shrinker.base;
    struct drm_i915_gem_object *queued;
    int frames, taken;

    i915_gem_shrinker_register(dev_a);
    queued = held_bound();
    frames = frames_live;
    taken = i915_gem_lock(dev_a);
    i915_gem_object_put(queued);
    free_counts_zero();
    CHECK(taken && sk->shrink(sk, 1, 1) == SET_PAGES && !dev_a->gem.freed && clears == 1 &&
              clears_yielding == 0 && frees_yielding == 0 && give_ways == 0 &&
              frames_live == frames - (int)SET_PAGES && mutex_held(&dev_a->gem.lock),
          "free: the shrinker under its caller's lock frees the list with no give-way, counted");
    i915_gem_unlock(dev_a, taken);
    CHECK(clears == 1 && give_ways == 0, "free: the holder finds the list empty as it lets go");
    i915_gem_shrinker_unregister(dev_a);
}

/** A free undoes every binding of its object whatever fence another device
 * left on the reservation, as Linux's force_unbind
 * (i915_vma.c:1760-1768): no binding stays in the GTT to a freed object. */
static void free_busy_shared(void)
{
    struct drm_i915_gem_object *obj = held_bound();
    struct dma_fence *f = busy_fence(obj);
    struct i915_vma *v;
    int frames = frames_live, bound = 0;

    for (v = dev_a->gem.bound; v; v = v->ggtt_next) {
        bound++;
    }
    free_counts_zero();
    i915_gem_object_put(obj);
    for (v = dev_a->gem.bound; v; v = v->ggtt_next) {
        bound--;
    }
    CHECK(bound == 1 && clears == 1 && frames_live == frames - (int)SET_PAGES && !dev_a->gem.freed,
          "free: the binding goes with its object though another device's fence is unsignalled");
    dma_fence_signal(f);
    dma_fence_put(f);
}

/** Run every row: 0 when each passed. */
/* ---- RPS's waitboost and interactive mode, which the driver calls ---- */

/** Whether the chip has begun `rq`; no request runs here, so only once
 * it has signalled. */
int i915_request_started(const struct i915_request *rq)
{
    return rq->fence.base.signaled;
}

/** The waitboost, RPS's, which no row here measures. */
void i915_rps_boost(struct i915_request *rq)
{
    (void)rq;
}

/** A wait for a request, which here is its fence alone: the fence's default
 * wait until the timeout, for ever when it is negative. */
int32_t i915_request_wait(struct i915_request *rq, uint32_t flags, int64_t timeout_ns)
{
    return dma_fence_default_wait(&rq->fence.base, (flags & I915_WAIT_INTERRUPTIBLE) != 0,
                                  timeout_ns < 0 ? -1 : dma_fence_now() + timeout_ns);
}

/* The driver's fences, i915_request.c's; no fence here is a request whose
 * breadcrumb a chip writes, so each signals only when a row signals it. */
const struct dma_fence_ops i915_fence_ops = {0, 0, 0, 0, 0, 0};

int main(void)
{
    int before;

    model_start();
    dev_a = bind(&chip_a, 1);
    file_a.dev = &dev_a->drm;
    before = allocations;
    working_set();
    pinned_and_active();
    whole_gtt();
    get_aperture();
    madvise_states();
    pressure();
    pin_raced_reclaim();
    user_space();
    userptr_refusals();
    userptr_calls();
    userptr_pages();
    free_on_close();
    free_under_lock();
    free_under_shrinker();
    free_shrinker_holder();
    free_busy_shared();
    CHECK(!dev_a->drm.objects && !dev_a->gem.bound && !frames_live && !pins_live && !user_pins,
          "the end: no object, binding, page or pin left");
    CHECK(allocations == before, "the end: every record freed");
    free(user_mem);
    free(chip_a.regs);
    free(chip_a.ap);
    free(dev_a);
    printf("%s: %s\n", MODEL_TEST, failures ? "failed" : "ok");
    return failures != 0;
}
