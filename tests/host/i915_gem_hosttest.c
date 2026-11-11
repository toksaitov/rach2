/*
 * i915_gem_hosttest.c: the Intel driver's buffers over the real GEM, PRIME,
 * dma-buf and fence code, on the chip of i915_gem_model.c, whose aperture
 * holds what a program or the kernel last wrote through it until the driver
 * syncs it.
 * host-deps: rach2/sys/lib/kstring.c rach2/sys/lib/kctypes.c
 */

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

/* ---- 78-3a.1: objects, handles, PREAD and PWRITE ---- */

/** GEM_CREATE, handles, PREAD and PWRITE, cached and uncached. */
static void objects(void)
{
    struct drm_i915_gem_create c = {0};
    uint8_t out[8192], in[8192];
    uint32_t h, i;
    struct drm_i915_gem_object *obj;
    int before = allocations;

    CHECK(i915_gem_create_ioctl(&dev_a->drm, &c, &file_a) == -EINVAL, "create: 0 bytes, EINVAL");
    c.size = SHMEM_MAX_SIZE + 1u;
    CHECK(i915_gem_create_ioctl(&dev_a->drm, &c, &file_a) == -E2BIG, "create: too large, E2BIG");
    c.size = 5000;
    CHECK(i915_gem_create_ioctl(&dev_a->drm, &c, &file_a) == 0 && c.size == 8192 && c.handle,
          "create: 5000 bytes give two pages, the size written back");
    h = c.handle;
    obj = object(&file_a, h);
    CHECK(obj && obj->read_domains == I915_GEM_DOMAIN_CPU &&
              obj->write_domain == I915_GEM_DOMAIN_CPU && obj->cache_level == I915_CACHE_LLC,
          "create: in the CPU domain, cached in the LLC");
    for (i = 0; i < sizeof(out); i++) {
        out[i] = (uint8_t)(i * 7u + 3u);
    }
    flushes = 0;
    CHECK(prw(&file_a, h, 0, out, 8192, 1) == 0, "pwrite: two pages");
    CHECK(flushes == 0, "pwrite: an LLC object needs no flush");
    CHECK(memcmp((void *)obj->frames[1], out + 4096, 4096) == 0,
          "pwrite: the bytes are in the object's second page");
    k_memset(in, 0, sizeof(in));
    CHECK(prw(&file_a, h, 100, in, 5000, 0) == 0 && memcmp(in, out + 100, 5000) == 0,
          "pread: 5000 bytes from 100 across the pages");
    CHECK(prw(&file_a, h, 8000, in, 200, 0) == -EINVAL, "pread: past the end, EINVAL");
    CHECK(prw(&file_a, h, 0, in, 0, 0) == 0, "pread: 0 bytes, nothing");
    CHECK(prw(&file_a, h, 0, (void *)16, 64, 0) == -EFAULT, "pread: a bad buffer, EFAULT");
    CHECK(prw(&file_a, 999, 0, in, 64, 0) == -ENOENT, "pread: no such object, ENOENT");

    /* Uncached on an LLC part: the processor's cache is dirty from the
     * start, so PWRITE still writes through it, as on Linux, and the lines
     * are flushed when the GPU takes the object. */
    {
        struct drm_i915_gem_caching cc = {0};

        cc.handle = h;
        cc.caching = I915_CACHING_NONE;
        CHECK(i915_gem_set_caching_ioctl(&dev_a->drm, &cc, &file_a) == 0 && obj->cache_dirty &&
                  !i915_gem_cpu_write_needs_clflush(obj),
              "caching: none, the cache dirty, a write needs no flush of its own");
    }
    for (i = 0; i < sizeof(out); i++) {
        out[i] = (uint8_t)(i * 13u + 1u);
    }
    CHECK(prw(&file_a, h, 0, out, 8192, 1) == 0 && memcmp((void *)obj->frames[0], out, 4096) == 0 &&
              memcmp((void *)obj->frames[1], out + 4096, 4096) == 0,
          "pwrite: uncached, the bytes in memory");
    k_memset(in, 0, sizeof(in));
    CHECK(prw(&file_a, h, 0, in, 8192, 0) == 0 && memcmp(in, out, 8192) == 0,
          "pread: uncached, the same bytes back");
    close_handle(&file_a, h);
    CHECK(allocations == before && pins_live == 0, "close: the object's records and pins go");
}

/** A GEM_CREATE_EXT of `size` bytes with `flags` and the chain at `ext`:
 * its return, the handle closed on success. */
static int32_t create_ext(uint64_t size, uint32_t flags, const void *ext)
{
    struct drm_i915_gem_create_ext c = {0};
    int32_t err;

    c.size = size;
    c.flags = flags;
    c.extensions = (uint64_t)(uintptr_t)ext;
    err = i915_gem_create_ext_ioctl(&dev_a->drm, &c, &file_a);
    if (err == 0) {
        CHECK(c.handle && c.size == ((size + 4095u) & ~(uint64_t)4095u),
              "create_ext: a handle, the size rounded to pages and written back");
        close_handle(&file_a, c.handle);
    }
    return err;
}

/** 78-5c: GEM_CREATE_EXT and its extensions, as IGT's gem_create
 * create-ext-placement-sanity-check and create-clear call them. */
static void create_ext_rows(void)
{
    struct drm_i915_gem_memory_class_instance smem[2] = {{I915_MEMORY_CLASS_SYSTEM, 0},
                                                         {I915_MEMORY_CLASS_SYSTEM, 0}};
    struct drm_i915_gem_memory_class_instance bad = {0xffff, 0xffff};
    struct drm_i915_gem_create_ext_memory_regions r = {{0}}, r2;
    struct {
        struct i915_user_extension base;
        uint32_t flags;
    } pxp = {{0}};
    struct i915_user_extension pat = {0};
    struct drm_i915_gem_create c = {0};
    int before = allocations;

    CHECK(UAPI_IOC_NR(DRM_IOCTL_I915_GEM_CREATE_EXT) == DRM_COMMAND_BASE + 0x3cu &&
              UAPI_IOC_SIZE(DRM_IOCTL_I915_GEM_CREATE_EXT) == 24,
          "create_ext: Linux's number and size");
    c.size = (uint64_t)-1;
    CHECK(i915_gem_create_ioctl(&dev_a->drm, &c, &file_a) == -EINVAL && !c.handle,
          "create: a size that rounds up to 0, EINVAL as Linux's");
    CHECK(create_ext(4096, 0, 0) == 0, "create_ext: no extensions is GEM_CREATE");
    CHECK(create_ext(0, 0, 0) == -EINVAL && create_ext((uint64_t)-1, 0, 0) == -EINVAL,
          "create_ext: 0 bytes, or a size that rounds up to 0, EINVAL");
    CHECK(create_ext(4096, ~I915_GEM_CREATE_EXT_FLAG_NEEDS_CPU_ACCESS, 0) == -EINVAL,
          "create_ext: an unknown flag, EINVAL");
    CHECK(create_ext(4096, I915_GEM_CREATE_EXT_FLAG_NEEDS_CPU_ACCESS, 0) == -EINVAL,
          "create_ext: NEEDS_CPU_ACCESS with one region to place in, EINVAL");
    r.base.name = I915_GEM_CREATE_EXT_MEMORY_REGIONS;
    r.regions = (uint64_t)(uintptr_t)smem;
    r.num_regions = 1;
    CHECK(create_ext(5000, 0, &r) == 0, "create_ext: MEMORY_REGIONS of system memory");
    r.num_regions = 0;
    CHECK(create_ext(4096, 0, &r) == -EINVAL, "create_ext: no regions, EINVAL");
    r.num_regions = 8;
    CHECK(create_ext(4096, 0, &r) == -EINVAL, "create_ext: more than seven regions, EINVAL");
    r.num_regions = 2;
    CHECK(create_ext(4096, 0, &r) == -EINVAL, "create_ext: a region named twice, EINVAL");
    r.num_regions = 1;
    r.regions = (uint64_t)(uintptr_t)&bad;
    CHECK(create_ext(4096, 0, &r) == -EINVAL, "create_ext: a region the device lacks, EINVAL");
    smem[1].memory_class = I915_MEMORY_CLASS_DEVICE;
    r.regions = (uint64_t)(uintptr_t)smem;
    r.num_regions = 2;
    CHECK(create_ext(4096, 0, &r) == -EINVAL, "create_ext: device memory on Haswell, EINVAL");
    r.num_regions = 1;
    r.pad = 1;
    CHECK(create_ext(4096, 0, &r) == -EINVAL, "create_ext: a pad not 0, EINVAL");
    r.pad = 0;
    r.regions = 16;
    CHECK(create_ext(4096, 0, &r) == -EFAULT, "create_ext: regions at a bad address, EFAULT");
    r.regions = (uint64_t)(uintptr_t)smem;
    r.base.flags = 1;
    CHECK(create_ext(4096, 0, &r) == -EINVAL, "create_ext: an extension's flags not 0, EINVAL");
    r.base.flags = 0;
    r2 = r;
    r.base.next_extension = (uint64_t)(uintptr_t)&r2;
    CHECK(create_ext(4096, 0, &r) == -EINVAL, "create_ext: MEMORY_REGIONS twice, EINVAL");
    r.base.next_extension = (uint64_t)(uintptr_t)&r;
    CHECK(create_ext(4096, 0, &r) == -EINVAL, "create_ext: a chain that loops, ended");
    r.base.next_extension = 0;
    pxp.base.name = I915_GEM_CREATE_EXT_PROTECTED_CONTENT;
    CHECK(create_ext(4096, 0, &pxp) == -ENODEV,
          "create_ext: PROTECTED_CONTENT, ENODEV without PXP");
    pxp.flags = 1;
    CHECK(create_ext(4096, 0, &pxp) == -EINVAL, "create_ext: PROTECTED_CONTENT's flags, EINVAL");
    pat.name = I915_GEM_CREATE_EXT_SET_PAT;
    CHECK(create_ext(4096, 0, &pat) == -ENODEV, "create_ext: SET_PAT, ENODEV before 12.70");
    pat.name = 3;
    CHECK(create_ext(4096, 0, &pat) == -EINVAL, "create_ext: an unknown extension, EINVAL");
    CHECK(create_ext(4096, 0, (const void *)(uintptr_t)16) == -EFAULT,
          "create_ext: a chain at a bad address, EFAULT");
    CHECK(allocations == before, "create_ext: every record freed");
}

/* ---- 78-3a.2: domains and caching ---- */

/** SET_DOMAIN, SW_FINISH, SET_CACHING and GET_CACHING. */
static void domains(void)
{
    uint32_t h = create(&file_a, 4096);
    struct drm_i915_gem_object *obj = object(&file_a, h);
    struct drm_i915_gem_caching cc = {0};
    struct drm_i915_gem_sw_finish sw = {0};

    CHECK(domain(&file_a, h, I915_GEM_DOMAIN_RENDER, 0) == -EINVAL, "set_domain: a GPU domain");
    CHECK(domain(&file_a, h, I915_GEM_DOMAIN_CPU, I915_GEM_DOMAIN_GTT) == -EINVAL,
          "set_domain: a write domain not read, EINVAL");
    CHECK(domain(&file_a, h, 0, 0) == 0, "set_domain: no domain, nothing");
    CHECK(domain(&file_a, 999, I915_GEM_DOMAIN_CPU, 0) == -ENOENT, "set_domain: ENOENT");
    CHECK(domain(&file_a, h, I915_GEM_DOMAIN_GTT, I915_GEM_DOMAIN_GTT) == 0 &&
              obj->read_domains == I915_GEM_DOMAIN_GTT && obj->write_domain == I915_GEM_DOMAIN_GTT,
          "set_domain: the GTT's for writing");
    CHECK(domain(&file_a, h, I915_GEM_DOMAIN_CPU, 0) == 0 &&
              obj->read_domains == (I915_GEM_DOMAIN_GTT | I915_GEM_DOMAIN_CPU) &&
              obj->write_domain == 0,
          "set_domain: the CPU's for reading, the GTT's writes flushed");
    CHECK(domain(&file_a, h, I915_GEM_DOMAIN_WC, I915_GEM_DOMAIN_WC) == 0 &&
              obj->read_domains == I915_GEM_DOMAIN_WC && obj->write_domain == I915_GEM_DOMAIN_WC,
          "set_domain: write-combining for writing");
    CHECK(domain(&file_a, h, I915_GEM_DOMAIN_CPU, I915_GEM_DOMAIN_CPU) == 0 &&
              obj->read_domains == I915_GEM_DOMAIN_CPU && obj->write_domain == I915_GEM_DOMAIN_CPU,
          "set_domain: the CPU's for writing");
    sw.handle = h;
    CHECK(i915_gem_sw_finish_ioctl(&dev_a->drm, &sw, &file_a) == 0, "sw_finish: done");
    sw.handle = 999;
    CHECK(i915_gem_sw_finish_ioctl(&dev_a->drm, &sw, &file_a) == -ENOENT, "sw_finish: ENOENT");

    cc.handle = h;
    CHECK(i915_gem_get_caching_ioctl(&dev_a->drm, &cc, &file_a) == 0 &&
              cc.caching == I915_CACHING_CACHED,
          "get_caching: an object starts cached");
    cc.caching = I915_CACHING_DISPLAY;
    CHECK(i915_gem_set_caching_ioctl(&dev_a->drm, &cc, &file_a) == 0 &&
              obj->cache_level == I915_CACHE_NONE,
          "set_caching: display is uncached on Haswell GT2");
    CHECK(i915_gem_get_caching_ioctl(&dev_a->drm, &cc, &file_a) == 0 &&
              cc.caching == I915_CACHING_NONE,
          "get_caching: none");
    cc.caching = 7;
    CHECK(i915_gem_set_caching_ioctl(&dev_a->drm, &cc, &file_a) == -EINVAL,
          "set_caching: another value, EINVAL");
    cc.caching = I915_CACHING_CACHED;
    cc.handle = 999;
    CHECK(i915_gem_set_caching_ioctl(&dev_a->drm, &cc, &file_a) == -ENOENT, "set_caching: ENOENT");
    cc.handle = h;
    {
        struct i915_vma *vma;

        CHECK(i915_vma_pin(obj, 0, 0, I915_PIN_MAPPABLE, 0, &vma) == 0, "bind: the object");
        i915_vma_unpin(vma);
        CHECK(i915_gem_set_caching_ioctl(&dev_a->drm, &cc, &file_a) == 0 && !obj->vmas &&
                  obj->cache_level == I915_CACHE_LLC,
              "set_caching: cached again, the binding undone for the new level");
    }
    close_handle(&file_a, h);
}

/* ---- 78-3a.3: mappings ---- */

/** MMAP_OFFSET's refusals, a place for each kind, and GEM_MMAP_GTT on its
 * number. */
static void offsets(void)
{
    uint32_t h = create(&file_a, 4096);
    struct drm_i915_gem_mmap_offset mo = {0}, legacy = {0};
    uint64_t gtt_off, wb_off, ext[4] = {0, 0, 0, 0};
    int32_t err;

    CHECK(mmap_offset(&file_a, h, 9, &err) == 0 && err == -EINVAL, "mmap_offset: a bad kind");
    CHECK(mmap_offset(&file_a, h, I915_MMAP_OFFSET_FIXED, &err) == 0 && err == -ENODEV,
          "mmap_offset: FIXED is for discrete parts, ENODEV");
    CHECK(mmap_offset(&file_a, 999, I915_MMAP_OFFSET_WB, &err) == 0 && err == -ENOENT,
          "mmap_offset: ENOENT");
    mo.handle = h;
    mo.extensions = (uintptr_t)ext;
    CHECK(i915_gem_mmap_offset_ioctl(&dev_a->drm, &mo, &file_a) == -EINVAL,
          "mmap_offset: an extension, of which none is defined, EINVAL");
    mo.extensions = 8;
    CHECK(i915_gem_mmap_offset_ioctl(&dev_a->drm, &mo, &file_a) == -EFAULT,
          "mmap_offset: an extension that cannot be read, EFAULT");
    gtt_off = mmap_offset(&file_a, h, I915_MMAP_OFFSET_GTT, &err);
    wb_off = mmap_offset(&file_a, h, I915_MMAP_OFFSET_WB, &err);
    CHECK(gtt_off && wb_off && gtt_off != wb_off, "mmap_offset: a place for each kind");
    CHECK(mmap_offset(&file_a, h, I915_MMAP_OFFSET_GTT, &err) == gtt_off,
          "mmap_offset: the same place again");
    /* GEM_MMAP_GTT shares the number; the dispatcher zeroes the rest. */
    legacy.handle = h;
    CHECK(i915_gem_mmap_offset_ioctl(&dev_a->drm, &legacy, &file_a) == 0 &&
              legacy.offset == gtt_off,
          "mmap_gtt: the GTT's place");
    close_handle(&file_a, h);
}

/** A write-back map and a GTT map of one object, each written and read
 * through the other, and the GTT map revoked when its binding goes. */
static void maps(void)
{
    uint32_t h = create(&file_a, 3 * 4096), i;
    struct drm_i915_gem_object *obj = object(&file_a, h);
    struct drm_i915_gem_caching cc = {0};
    vma_t wb, gtt;
    uint8_t *p, *q;

    CHECK(map(&file_b, h, I915_MMAP_OFFSET_WB, 1, &wb) != 0,
          "mmap: another file without a handle is refused");
    CHECK(map(&file_a, h, I915_MMAP_OFFSET_WB, 3, &wb) == 0 &&
              ((struct region *)wb.file)->cache == 0,
          "mmap: write-back, cached");
    CHECK(map(&file_a, h, I915_MMAP_OFFSET_GTT, 3, &gtt) == 0 &&
              ((struct region *)gtt.file)->cache == VM_PROT_WC,
          "mmap: through the aperture, write-combining");
    p = fault(&wb, 1);
    CHECK(p && (uintptr_t)p == shmem_resident(&obj->pages, 1),
          "fault: write-back maps the object's page, given on the first touch");
    CHECK(fault(&wb, 3) == 0, "fault: past the end, SIGBUS");
    CHECK(domain(&file_a, h, I915_GEM_DOMAIN_GTT, I915_GEM_DOMAIN_GTT) == 0, "set_domain: GTT");
    q = fault(&gtt, 1);
    CHECK(q && obj->vmas && obj->vmas->userfault && obj->userfault_count == 1 &&
              q == chip_a.ap + obj->vmas->start + 4096u,
          "fault: through the aperture, the bound page, marked as mapped");
    for (i = 0; q && i < 4096; i++) {
        q[i] = (uint8_t)(i ^ 0x5a);
    }
    CHECK(domain(&file_a, h, I915_GEM_DOMAIN_CPU, 0) == 0 && p[0] == 0x5a &&
              p[4095] == (uint8_t)(0xfff ^ 0x5a),
          "gtt map: written through the aperture, read through the CPU's map");
    CHECK(domain(&file_a, h, I915_GEM_DOMAIN_CPU, I915_GEM_DOMAIN_CPU) == 0, "set_domain: CPU");
    p[0] = 0x11;
    CHECK(domain(&file_a, h, I915_GEM_DOMAIN_GTT, 0) == 0 && q && q[0] == 0x11,
          "cpu map: written, read back through the aperture");
    zaps = 0;
    cc.handle = h;
    cc.caching = I915_CACHING_NONE;
    CHECK(i915_gem_set_caching_ioctl(&dev_a->drm, &cc, &file_a) == 0, "caching: none");
    CHECK(zaps == 1 && zapped == &obj->mmo[I915_MMAP_OFFSET_GTT] && zap_pgoff == 0 &&
              zap_count == 3 && obj->userfault_count == 0,
          "unbind: the aperture's mapping is revoked, the CPU's is not");
    unmap(&wb);
    unmap(&gtt);
    close_handle(&file_a, h);
}

/** GEM_MMAP: `size` bytes of `handle` from `offset` with `flags`; the
 * ioctl's answer, the address in `*addr`. */
static int32_t gem_mmap(uint32_t handle, uint64_t offset, uint64_t size, uint64_t flags,
                        uint64_t *addr)
{
    struct drm_i915_gem_mmap m = {0};
    int32_t err;

    m.handle = handle;
    m.offset = offset;
    m.size = size;
    m.flags = flags;
    err = i915_gem_mmap_ioctl(&dev_a->drm, &m, &file_a);
    *addr = m.addr_ptr;
    return err;
}

/** 78-14a.5: GEM_MMAP, which libdrm_intel's drm_intel_bo_map uses: the
 * object's pages from the offset, write-back or write-combining, the
 * mapping holding the object, and Linux's refusals. */
static void legacy_maps(void)
{
    uint32_t h = create(&file_a, 3 * 4096);
    struct drm_i915_gem_object *obj = object(&file_a, h);
    uint64_t addr = 0;
    uint8_t *p;

    CHECK(gem_mmap(h, 4096, 8192, 0, &addr) == 0 && addr == (uint64_t)MMAPPED_AT &&
              mmapped.pgoff == 1 && mmapped.end == 8192 &&
              mmapped.prot == (VM_PROT_READ | VM_PROT_WRITE) &&
              ((struct region *)mmapped.file)->cache == 0 &&
              ((struct region *)mmapped.file)->obj == &obj->mmo[I915_MMAP_OFFSET_WB],
          "gem_mmap: two pages from the second, write-back, at the address mmap gave");
    p = fault(&mmapped, mmapped.pgoff);
    CHECK(p && (uintptr_t)p == shmem_resident(&obj->pages, 1),
          "gem_mmap: its first page is the object's second");
    CHECK(obj->base.refs == 2, "gem_mmap: the mapping holds the object");
    unmap(&mmapped);
    CHECK(obj->base.refs == 1, "gem_mmap: and lets it go with the mapping");
    CHECK(gem_mmap(h, 0, 4096, I915_MMAP_WC, &addr) == 0 &&
              ((struct region *)mmapped.file)->cache == VM_PROT_WC &&
              ((struct region *)mmapped.file)->obj == &obj->mmo[I915_MMAP_OFFSET_WC],
          "gem_mmap: write-combining under I915_MMAP_WC");
    unmap(&mmapped);
    CHECK(gem_mmap(h, 0, 4096, 2, &addr) == -EINVAL, "gem_mmap: another flag, EINVAL");
    CHECK(gem_mmap(h, 3 * 4096, 0, 0, &addr) == -EINVAL &&
              gem_mmap(h, 4096, 3 * 4096, 0, &addr) == -EINVAL &&
              gem_mmap(h, 100, 4096, 0, &addr) == -EINVAL && gem_mmap(h, 0, 0, 0, &addr) == -EINVAL,
          "gem_mmap: an offset or size outside the object, an offset within a page and a size "
          "of 0, EINVAL");
    CHECK(gem_mmap(999, 0, 4096, 0, &addr) == -ENOENT, "gem_mmap: no such object, ENOENT");
    CHECK(obj->base.refs == 1, "gem_mmap: a refusal holds no reference");
    close_handle(&file_a, h);
}

/** An object larger than the aperture: a GTT fault binds a view of 1 MiB
 * around the page, or the object's tail. */
static void views(void)
{
    uint32_t big = create(&file_a, APERTURE_BYTES + 4 * 4096);
    struct drm_i915_gem_object *bobj = object(&file_a, big);
    vma_t gtt;
    uint8_t *q;

    CHECK(map(&file_a, big, I915_MMAP_OFFSET_GTT, (APERTURE_BYTES >> 12) + 4, &gtt) == 0,
          "mmap: an object past the aperture");
    q = fault(&gtt, (APERTURE_BYTES >> 12) + 3);
    CHECK(q && bobj->vmas && bobj->vmas->view_pages == 4 &&
              bobj->vmas->view_offset == APERTURE_BYTES >> 12,
          "fault: the last page through a partial view, the object's tail");
    q = fault(&gtt, 300);
    CHECK(q && bobj->vmas->view_offset == 256 && bobj->vmas->view_pages == 256,
          "fault: page 300 through the second 1 MiB view");
    unmap(&gtt);
    close_handle(&file_a, big);
}

/**
 * A remapped view of an object, as a plane scans out the source of a
 * framebuffer wider than its stride (78-16): page k of the binding maps
 * row k / 3, column k % 3 of the view, its rows 8 pages apart in the
 * object, as Linux's intel_remap_pages lists them; a pin of the same view
 * reuses its binding, the whole object's is another, a GTT map's fault
 * never takes the view, a view past the object is refused, and the
 * entries go with the object.
 */
static void remapped(void)
{
    uint32_t h = create(&file_a, 64 * 4096);
    struct drm_i915_gem_object *obj = object(&file_a, h);
    struct i915_remap_info r, past;
    struct i915_vma *v = 0, *again = 0, *whole = 0;
    uint32_t row, col, page;
    uint64_t start;
    int ok = 1;

    r.offset = 5;
    r.width = 3;
    r.height = 4;
    r.src_stride = 8;
    past = r;
    past.offset = 38; /* its last row ends at page 38 + 3 * 8 + 3 = 65 */
    CHECK(i915_gem_object_pin_to_display(obj, &r, 256u * 1024u, &v) == 0 && v &&
              v->view_pages == 12 && v->remap.width == 3 && (v->start & (256u * 1024u - 1u)) == 0,
          "remap: a view of 4 rows of 3 pages bound for scanout");
    for (row = 0; v && row < 4; row++) {
        for (col = 0; col < 3; col++) {
            page = 5u + row * 8u + col;
            ok &= chip_a.gtt[(v->start >> 12) + row * 3u + col] == obj->frames[page];
        }
    }
    CHECK(v && ok, "remap: each row of the view the object's pages 8 apart");
    CHECK(i915_vma_for_page(obj, 5) == 0, "remap: a GTT map's fault never takes the view");
    CHECK(i915_gem_object_pin_to_display(obj, &r, 256u * 1024u, &again) == 0 && again == v &&
              v->pin_count == 2,
          "remap: a pin of the same view reuses its binding");
    CHECK(i915_gem_object_pin_to_display(obj, 0, 256u * 1024u, &whole) == 0 && whole != v &&
              whole->view_pages == 0 && !whole->remap.width,
          "remap: the whole object's binding is another");
    CHECK(i915_vma_pin_remapped(obj, &past, 0, 0, &again) == -EINVAL,
          "remap: a view past the object refused");
    start = v ? v->start : 0;
    if (v) {
        i915_vma_unpin(v);
        i915_vma_unpin(v);
    }
    if (whole) {
        i915_vma_unpin(whole);
    }
    close_handle(&file_a, h);
    for (page = 0; page < 12; page++) {
        ok &= chip_a.gtt[(start >> 12) + page] == 0;
    }
    CHECK(ok, "remap: the view's entries cleared with the object");
}

/**
 * The scanout's binding of a whole object, as Linux's on a chip without a
 * GMCH (78-18): inside the aperture when it fits there, and an object
 * larger than the aperture bound past it rather than refused.
 */
static void scanout(void)
{
    uint32_t small = create(&file_a, 16 * 4096);
    uint32_t big = create(&file_a, APERTURE_BYTES + 4 * 4096);
    struct i915_vma *s = 0, *b = 0;

    CHECK(i915_gem_object_pin_to_display(object(&file_a, small), 0, 256u * 1024u, &s) == 0 && s &&
              s->start + s->size <= APERTURE_BYTES && (s->start & (256u * 1024u - 1u)) == 0,
          "scanout: a small object bound inside the aperture");
    CHECK(i915_gem_object_pin_to_display(object(&file_a, big), 0, 256u * 1024u, &b) == 0 && b &&
              b->view_pages == 0 && b->start + b->size > APERTURE_BYTES &&
              (b->start & (256u * 1024u - 1u)) == 0,
          "scanout: an object larger than the aperture bound past it");
    if (s) {
        i915_vma_unpin(s);
    }
    if (b) {
        i915_vma_unpin(b);
    }
    close_handle(&file_a, small);
    close_handle(&file_a, big);
}

/** More objects through the aperture than the windows hold at once: the
 * other device's imports, which have no pages of the processor's, so that
 * PWRITE reaches them through the aperture alone. */
static void windows(void)
{
    enum { N = 6 };
    uint32_t h[N], imp[N], i, j;
    int fd[N], ok = 1;
    static uint8_t buf[0x200000];

    i915_aperture_fini(&dev_b->gem.aperture);
    for (i = 0; i < N; i++) {
        h[i] = create(&file_a, 0x200000);
        fd[i] = export_fd(&file_a, h[i]);
        imp[i] = import_fd(&file_b, fd[i]);
    }
    chip_b.maps = chip_b.unmaps = chip_b.syncs_to = 0;
    for (i = 0; i < N; i++) {
        for (j = 0; j < sizeof(buf); j += 4096) {
            buf[j] = (uint8_t)(i + 1);
            buf[j + 4095] = (uint8_t)(j >> 12);
        }
        CHECK(prw(&file_b, imp[i], 0, buf, sizeof(buf), 1) == 0,
              "pwrite: 2 MiB of an import through the windows");
    }
    for (i = 0; i < N; i++) {
        struct drm_i915_gem_object *obj = object(&file_a, h[i]);

        for (j = 0; j < 512; j++) {
            const uint8_t *page = (const uint8_t *)shmem_resident(&obj->pages, j);

            ok &= page && page[0] == i + 1 && page[4095] == (uint8_t)j;
        }
    }
    CHECK(ok && chip_b.syncs_to >= N * 512, "pwrite: six imports of 2 MiB, each page synced");
    CHECK(chip_b.maps == N && chip_b.unmaps == N - I915_APERTURE_WINDOWS,
          "windows: six mapped in turn over the aperture, four at most at once");
    for (i = 0; i < I915_APERTURE_WINDOWS; i++) {
        ok &= dev_b->gem.aperture.win[i].users == 0;
    }
    CHECK(ok, "windows: none held after the copies");
    for (i = 0; i < N; i++) {
        close_handle(&file_b, imp[i]);
        fd_free(&table, fd[i]);
        close_handle(&file_a, h[i]);
    }
}

/* ---- 78-3a.4: tiling, swizzle and fences ---- */

/** The bytes of the first 512 KiB where the driver's tiled_offset and
 * Linux's selftest's differ, for one tiling, stride and swizzle. */
static uint32_t arithmetic_misses(uint32_t tiling, uint32_t stride, uint32_t swizzle)
{
    uint32_t v, bad = 0;

    for (v = 0; v < 0x80000; v += 4) {
        bad += i915_tiled_offset(tiling, stride, swizzle, v) !=
               ref_tiled_offset(tiling, stride, swizzle, v);
    }
    return bad;
}

/** The driver's detiling arithmetic against Linux's selftest's, and at
 * points worked by hand. */
static void arithmetic(void)
{
    static const uint32_t swizzles[] = {I915_BIT_6_SWIZZLE_NONE, I915_BIT_6_SWIZZLE_9,
                                        I915_BIT_6_SWIZZLE_9_10, I915_BIT_6_SWIZZLE_9_11,
                                        I915_BIT_6_SWIZZLE_9_10_11};
    static const uint32_t strides[] = {512, 1024, 2048, 4096, 7680, 8192, 0x40000};
    uint32_t t, s, w, bad = 0;

    for (t = I915_TILING_X; t <= I915_TILING_Y; t++) {
        for (s = 0; s < 5; s++) {
            for (w = 0; w < sizeof(strides) / sizeof(strides[0]); w++) {
                bad += arithmetic_misses(t, strides[w], swizzles[s]);
            }
        }
    }
    CHECK(bad == 0, "tiled_offset: as Linux's selftest, X and Y, every swizzle, 7 strides");
    /* Points worked by hand: the X tile holds 8 rows of 512 bytes, the Y
     * tile 8 columns of 16 bytes by 32 rows. */
    CHECK(i915_tiled_offset(I915_TILING_X, 4096, I915_BIT_6_SWIZZLE_NONE, 512) == 4096,
          "tiled_offset: X, byte 512 of row 0 begins the second tile");
    CHECK(i915_tiled_offset(I915_TILING_X, 4096, I915_BIT_6_SWIZZLE_NONE, 4096) == 512,
          "tiled_offset: X, row 1 is the tile's second 512 bytes");
    CHECK(i915_tiled_offset(I915_TILING_Y, 512, I915_BIT_6_SWIZZLE_NONE, 16) == 512,
          "tiled_offset: Y, byte 16 of row 0 begins the second column");
    CHECK(i915_tiled_offset(I915_TILING_Y, 512, I915_BIT_6_SWIZZLE_NONE, 512) == 16,
          "tiled_offset: Y, row 1 is the column's second 16 bytes");
    CHECK(i915_tiled_offset(I915_TILING_X, 4096, I915_BIT_6_SWIZZLE_9, 512 * 1) == 4096,
          "tiled_offset: X, bit 9 clear leaves bit 6");
    CHECK(i915_tiled_offset(I915_TILING_X, 4096, I915_BIT_6_SWIZZLE_9, 4096) == 512 + 64,
          "tiled_offset: X, bit 9 set flips bit 6");
    CHECK(i915_tiled_offset(I915_TILING_NONE, 4096, I915_BIT_6_SWIZZLE_9, 4096) == 4096,
          "tiled_offset: linear is the identity");
}

/** The swizzle read from the DIMMs of the channels, and the GPU's and the
 * display's swizzling set to match. */
static void swizzle(void)
{
    CHECK(dev_a->gem.swizzle_x == I915_BIT_6_SWIZZLE_9_10 &&
              dev_a->gem.swizzle_y == I915_BIT_6_SWIZZLE_9,
          "swizzle: DIMMs of one size in both channels, 9/10 for X and 9 for Y");
    CHECK((chip_a.regs[TILECTL / 4] & TILECTL_SWZCTL) &&
              (chip_a.regs[DISP_ARB_CTL / 4] & DISP_TILE_SURFACE_SWIZZLING) &&
              chip_a.regs[ARB_MODE / 4] == I915_MASKED_ENABLE(ARB_MODE_SWIZZLE_IVB),
          "swizzle: the GPU and the display swizzle as the controller");
    CHECK(dev_b->gem.swizzle_x == I915_BIT_6_SWIZZLE_NONE &&
              dev_b->gem.swizzle_y == I915_BIT_6_SWIZZLE_NONE &&
              !(chip_b.regs[TILECTL / 4] & TILECTL_SWZCTL),
          "swizzle: DIMMs of two sizes, none, and the GPU left alone");
}

/** SET_TILING's refusals and answers and GET_TILING's, leaving `h` X
 * tiled with a stride of 2048 bytes; a framebuffer of the object fixes its
 * layout, as Linux's frontbuffer does (kms_addfb_basic's
 * framebuffer-vs-set-tiling). */
static void tiling_ioctls(uint32_t h)
{
    struct drm_i915_gem_set_tiling st;
    struct drm_i915_gem_get_tiling gt = {0};
    struct drm_i915_gem_object *obj;

    CHECK(tile(&file_a, h, 3, 512, &st) == -EINVAL, "set_tiling: no such tiling, EINVAL");
    CHECK(tile(&file_a, h, I915_TILING_X, 640, &st) == -EINVAL,
          "set_tiling: X, a stride not of 512 bytes, EINVAL");
    CHECK(tile(&file_a, h, I915_TILING_Y, 0x800 * 128 + 128, &st) == -EINVAL,
          "set_tiling: past the fence's pitch, EINVAL");
    CHECK(tile(&file_a, 999, I915_TILING_X, 512, &st) == -ENOENT, "set_tiling: ENOENT");
    CHECK(tile(&file_a, h, I915_TILING_Y, 384, &st) == 0 && st.tiling_mode == I915_TILING_Y &&
              st.stride == 384 && st.swizzle_mode == I915_BIT_6_SWIZZLE_9,
          "set_tiling: Y of 384 bytes, swizzle 9");
    CHECK(tile(&file_a, h, I915_TILING_NONE, 512, &st) == 0 && st.stride == 0 &&
              st.swizzle_mode == I915_BIT_6_SWIZZLE_NONE,
          "set_tiling: linear takes stride 0");
    CHECK(tile(&file_a, h, I915_TILING_X, 2048, &st) == 0 &&
              st.swizzle_mode == I915_BIT_6_SWIZZLE_9_10,
          "set_tiling: X of 2048 bytes, swizzle 9/10");
    gt.handle = h;
    CHECK(i915_gem_get_tiling_ioctl(&dev_a->drm, &gt, &file_a) == 0 &&
              gt.tiling_mode == I915_TILING_X && gt.swizzle_mode == I915_BIT_6_SWIZZLE_9_10 &&
              gt.phys_swizzle_mode == I915_BIT_6_SWIZZLE_9_10,
          "get_tiling: X, 9/10, physically too");
    obj = i915_gem_object_lookup(&file_a, h);
    obj->fb_refs = 1;
    CHECK(tile(&file_a, h, I915_TILING_X, 2048, &st) == 0 &&
              tile(&file_a, h, I915_TILING_X, 1024, &st) == -EBUSY &&
              tile(&file_a, h, I915_TILING_Y, 2048, &st) == -EBUSY &&
              st.tiling_mode == I915_TILING_X && st.stride == 2048,
          "set_tiling: of a framebuffer's object, its own layout taken, another EBUSY");
    obj->fb_refs = 0;
    i915_gem_object_put(obj);
}

/** Fault each of the `pages` pages of the GTT map `gtt` and write each
 * dword its own offset: the pages that would not fault. */
static uint32_t write_through(vma_t *gtt, uint32_t pages)
{
    uint32_t i, x, bad = 0;

    for (i = 0; i < pages; i++) {
        uint8_t *q = fault(gtt, i);

        if (!q) {
            bad++;
            continue;
        }
        for (x = 0; x < 4096; x += 4) {
            *(uint32_t *)(q + x) = i * 4096u + x;
        }
    }
    return bad;
}

/** The dwords of `obj`'s first `bytes` not where X tiling of `stride` and
 * swizzle 9/10 put each in memory. */
static uint32_t detile_misses(const struct drm_i915_gem_object *obj, uint32_t stride,
                              uint32_t bytes)
{
    uint32_t y, bad = 0;

    for (y = 0; y < bytes; y += 4) {
        uint64_t at = ref_tiled_offset(I915_TILING_X, stride, I915_BIT_6_SWIZZLE_9_10, y);

        bad += *(const uint32_t *)(obj->frames[at >> 12] + (at & 4095)) != y;
    }
    return bad;
}

/** The done test in small: `h`, X tiled, written through a GTT map, reads
 * back through the CPU where the tiling puts each byte; then retiled while
 * mapped. */
static void tiled_map(uint32_t h)
{
    struct drm_i915_gem_object *obj = object(&file_a, h);
    struct drm_i915_gem_set_tiling st;
    struct i915_vma *vma;
    uint64_t want;
    vma_t gtt;

    CHECK(map(&file_a, h, I915_MMAP_OFFSET_GTT, 64, &gtt) == 0, "mmap: the tiled object");
    CHECK(domain(&file_a, h, I915_GEM_DOMAIN_GTT, I915_GEM_DOMAIN_GTT) == 0, "set_domain: GTT");
    CHECK(write_through(&gtt, 64) == 0 && obj->vmas && obj->vmas->fence,
          "fault: every page, with a fence");
    vma = obj->vmas;
    if (!vma || !vma->fence) {
        unmap(&gtt);
        return;
    }
    want = ((uint64_t)(vma->start + vma->fence_size - 4096u) << 32) | vma->start |
           ((uint64_t)(2048 / 128 - 1) << 32) | I965_FENCE_REG_VALID;
    CHECK(chip_a.regs[FENCE_REG_GEN6_LO(vma->fence->id) / 4] == (uint32_t)want &&
              chip_a.regs[FENCE_REG_GEN6_HI(vma->fence->id) / 4] == (uint32_t)(want >> 32),
          "fence: the object's range, pitch and X, valid");
    CHECK(vma->fence_size == 64 * 4096 && (vma->start & 4095) == 0,
          "fence: whole tile rows, page-aligned");
    CHECK(domain(&file_a, h, I915_GEM_DOMAIN_CPU, 0) == 0, "set_domain: CPU, the writes flushed");
    CHECK(detile_misses(obj, 2048, 64 * 4096) == 0,
          "detile: each dword where X tiling and swizzle 9/10 put it in memory");
    zaps = 0;
    CHECK(tile(&file_a, h, I915_TILING_Y, 512, &st) == 0 && zaps >= 1 &&
              zapped == &obj->mmo[I915_MMAP_OFFSET_GTT] && vma->fence->dirty,
          "set_tiling: while mapped, the map revoked and the fence marked");
    CHECK(domain(&file_a, h, I915_GEM_DOMAIN_GTT, 0) == 0 && fault(&gtt, 0) &&
              (chip_a.regs[FENCE_REG_GEN6_LO(vma->fence->id) / 4] & 2u),
          "fault: the fence rewritten for Y");
    unmap(&gtt);
}

/** One more tiled map than fences, after `h`'s: the least recently used
 * fence is taken, `h`'s, and its map revoked, then the next. */
static void fences(uint32_t h)
{
    struct drm_i915_gem_object *obj = object(&file_a, h);
    uint32_t tiles[I915_NUM_FENCES + 1], i;
    struct drm_i915_gem_set_tiling st;
    vma_t gtt;

    for (i = 0; i <= I915_NUM_FENCES; i++) {
        tiles[i] = create(&file_a, 4 * 4096);
        CHECK(tile(&file_a, tiles[i], I915_TILING_X, 512, &st) == 0, "set_tiling: X");
        CHECK(map(&file_a, tiles[i], I915_MMAP_OFFSET_GTT, 4, &gtt) == 0, "mmap: a tile");
        zaps = 0;
        CHECK(fault(&gtt, 0) != 0, "fault: a tiled object");
        unmap(&gtt);
        if (i == I915_NUM_FENCES - 2) {
            CHECK(zaps == 0, "fences: a free one while any is");
        } else if (i == I915_NUM_FENCES - 1) {
            CHECK(zaps == 1 && zapped == &obj->mmo[I915_MMAP_OFFSET_GTT] && !obj->vmas->fence,
                  "fences: none free, the least recently used taken, its map revoked");
        }
    }
    CHECK(zaps == 1 && !object(&file_a, tiles[0])->vmas->fence &&
              zapped == &object(&file_a, tiles[0])->mmo[I915_MMAP_OFFSET_GTT],
          "fences: then the next least recently used");
    for (i = 0; i <= I915_NUM_FENCES; i++) {
        close_handle(&file_a, tiles[i]);
    }
}

/** Tiling, its swizzle and the fences that serve GTT maps. */
static void tiling(void)
{
    uint32_t h = create(&file_a, 64 * 4096);

    swizzle();
    tiling_ioctls(h);
    tiled_map(h);
    fences(h);
    close_handle(&file_a, h);
}

/* ---- 78-3a.5: BUSY, WAIT and PRIME ---- */

/** A fence of the driver's on the engine of class `cls`. */
static struct i915_fence *engine_fence(uint16_t cls)
{
    struct i915_fence *f = (struct i915_fence *)kzalloc(sizeof(*f));

    dma_fence_init(&f->base, &i915_fence_ops, dma_fence_context_alloc(1), 1);
    f->uabi_class = cls;
    return f;
}

/** BUSY's bits for the driver's fences and WAIT's timeouts. */
static void busy_and_wait(void)
{
    uint32_t h = create(&file_a, 4096);
    struct drm_i915_gem_object *obj = object(&file_a, h);
    struct drm_i915_gem_busy busy = {0};
    struct drm_i915_gem_wait wait = {0};
    struct i915_fence *rcs = engine_fence(0), *bcs = engine_fence(1);
    struct dma_fence *other = (struct dma_fence *)kzalloc(sizeof(*other));

    busy.handle = h;
    CHECK(i915_gem_busy_ioctl(&dev_a->drm, &busy, &file_a) == 0 && busy.busy == 0, "busy: idle, 0");
    dma_fence_init(other, 0, dma_fence_context_alloc(1), 1);
    CHECK(dma_resv_add_fence(obj->base.resv, &rcs->base, 1) == 0 &&
              dma_resv_add_fence(obj->base.resv, &bcs->base, 0) == 0 &&
              dma_resv_add_fence(obj->base.resv, other, 0) == 0,
          "fences: a render write, a blitter read, another driver's read");
    CHECK(i915_gem_busy_ioctl(&dev_a->drm, &busy, &file_a) == 0 &&
              busy.busy == (0x00010001u | 0x00020000u),
          "busy: render writes and reads, the blitter reads, the other driver's unseen");
    busy.handle = 999;
    CHECK(i915_gem_busy_ioctl(&dev_a->drm, &busy, &file_a) == -ENOENT, "busy: ENOENT");
    busy.handle = h;

    wait.bo_handle = h;
    wait.flags = 1;
    CHECK(i915_gem_wait_ioctl(&dev_a->drm, &wait, &file_a) == -EINVAL, "wait: flags, EINVAL");
    wait.flags = 0;
    wait.timeout_ns = 0;
    CHECK(i915_gem_wait_ioctl(&dev_a->drm, &wait, &file_a) == -ETIME, "wait: 0 asks, ETIME");
    wait.bo_handle = 999;
    CHECK(i915_gem_wait_ioctl(&dev_a->drm, &wait, &file_a) == -ENOENT, "wait: ENOENT");
    wait.bo_handle = h;
    wait.timeout_ns = 5000000;
    CHECK(i915_gem_wait_ioctl(&dev_a->drm, &wait, &file_a) == -ETIME && wait.timeout_ns == 0,
          "wait: 5 ms pass, ETIME, no time left");
    dma_fence_signal(&rcs->base);
    dma_fence_signal(other);
    signal_on_wait = &bcs->base;
    wait.timeout_ns = -1;
    CHECK(i915_gem_wait_ioctl(&dev_a->drm, &wait, &file_a) == 0 && wait.timeout_ns == -1,
          "wait: for ever, until the blitter's read signals");
    CHECK(i915_gem_busy_ioctl(&dev_a->drm, &busy, &file_a) == 0 && busy.busy == 0,
          "busy: idle again");
    dma_fence_put(&rcs->base);
    dma_fence_put(&bcs->base);
    dma_fence_put(other);
    close_handle(&file_a, h);
}

/** PRIME: an import of the device's own export and another device's,
 * a dma-buf's mapping and its CPU access sync. */
static void prime(void)
{
    uint32_t h = create(&file_a, 2 * 4096), imp, i;
    struct drm_i915_gem_object *obj = object(&file_a, h), *iobj;
    uint8_t out[8192], in[8192];
    struct dma_buf *buf;
    int fd;
    struct drm_i915_gem_mmap legacy;
    int32_t err;
    vma_t vma;

    for (i = 0; i < sizeof(out); i++) {
        out[i] = (uint8_t)(i * 31u + 7u);
    }
    CHECK(prw(&file_a, h, 0, out, sizeof(out), 1) == 0, "pwrite: the exporter's pages");
    fd = export_fd(&file_a, h);
    buf = (struct dma_buf *)slots[fd].priv;
    CHECK(import_fd(&file_a, fd) == h, "prime: an import of its own export is the object");
    imp = import_fd(&file_b, fd);
    iobj = object(&file_b, imp);
    CHECK(iobj && iobj != obj && iobj->base.import == buf && iobj->base.resv == obj->base.resv &&
              iobj->read_domains == I915_GEM_DOMAIN_GTT && iobj->write_domain == 0 &&
              iobj->base.funcs == &i915_gem_object_funcs,
          "prime: another device's import shares the pages and fences, in the GTT's domain");
    k_memset(in, 0, sizeof(in));
    CHECK(prw(&file_b, imp, 0, in, sizeof(in), 0) == 0 && memcmp(in, out, sizeof(in)) == 0,
          "pread: an import reads the exporter's bytes through its own aperture");
    CHECK(mmap_offset(&file_b, imp, I915_MMAP_OFFSET_WB, &err) == 0 && err == -ENODEV,
          "mmap_offset: an import's pages are another device's, ENODEV");
    CHECK(mmap_offset(&file_b, imp, I915_MMAP_OFFSET_GTT, &err) != 0,
          "mmap_offset: an import through the aperture");
    k_memset(&legacy, 0, sizeof(legacy));
    legacy.handle = imp;
    legacy.size = 4096;
    CHECK(i915_gem_mmap_ioctl(&dev_b->drm, &legacy, &file_b) == -ENXIO,
          "gem_mmap: an import has no pages of its own, ENXIO");

    /* The dma-buf's own mapping and its CPU access sync. */
    k_memset(&vma, 0, sizeof(vma));
    vma.type = VM_SHARED;
    vma.start = 0x20000000u;
    vma.end = vma.start + 8192;
    CHECK(slots[fd].ops->mmap(&slots[fd], &vma) == 0 && ((struct region *)vma.file)->cache == 0,
          "dma-buf: mapped write-back");
    CHECK(fault(&vma, 1) == (uint8_t *)obj->frames[1], "dma-buf: the exporter's page");
    unmap(&vma);
    CHECK(buf->ops->cpu_access(buf, DMA_BUF_SYNC_START | DMA_BUF_SYNC_WRITE) == 0 &&
              obj->read_domains == I915_GEM_DOMAIN_CPU && obj->write_domain == I915_GEM_DOMAIN_CPU,
          "sync: a write's start moves the object to the CPU");
    CHECK(buf->ops->cpu_access(buf, DMA_BUF_SYNC_END | DMA_BUF_SYNC_WRITE) == 0 &&
              (obj->read_domains & I915_GEM_DOMAIN_GTT) && obj->write_domain == 0,
          "sync: the end moves it back to the GTT for reading");
    fd_free(&table, fd);
    close_handle(&file_b, imp);
    close_handle(&file_a, h);
    CHECK(!dev_b->drm.objects, "prime: the import goes with its handle");
}

/* ---- the ioctl table and the unplug ---- */

/** The ioctl table, a fault whose binds fail, and the unplug. */
static void table_and_unplug(void)
{
    uint32_t i, j, h, dup = 0, bad = 0;
    vma_t gtt;

    for (i = 0; i < I915_NUM_IOCTLS; i++) {
        /* GET_PIPE_FROM_CRTC_ID is the display's, flags 0 as on Linux. */
        bad += !i915_ioctls[i].func || (i915_ioctls[i].cmd == DRM_IOCTL_I915_GET_PIPE_FROM_CRTC_ID
                                            ? i915_ioctls[i].flags != 0
                                            : !(i915_ioctls[i].flags & DRM_RENDER_ALLOW));
        for (j = 0; j < i; j++) {
            dup += UAPI_IOC_NR(i915_ioctls[i].cmd) == UAPI_IOC_NR(i915_ioctls[j].cmd);
        }
    }
    CHECK(
        !bad && !dup,
        "ioctls: each has a handler, a render node may call each but the display's, numbers apart");
    CHECK(UAPI_IOC_NR(DRM_IOCTL_I915_GEM_MMAP_GTT) == UAPI_IOC_NR(DRM_IOCTL_I915_GEM_MMAP_OFFSET) &&
              UAPI_IOC_SIZE(DRM_IOCTL_I915_GEM_MMAP_OFFSET) == 32,
          "ioctls: MMAP_OFFSET extends MMAP_GTT's number");
    h = create(&file_a, 4096);
    CHECK(map(&file_a, h, I915_MMAP_OFFSET_GTT, 1, &gtt) == 0, "mmap: before the unplug");
    bind_fail = 3;
    CHECK(fault_code(&gtt, 0) == 1 && !object(&file_a, h)->vmas &&
              object(&file_a, h)->pages_pin_count == 0,
          "fault: the three binds fail with ENOSPC, the access to run again as Linux's, nothing "
          "left bound and the pages let go");
    CHECK(fault(&gtt, 0) != 0, "fault: then it binds");
    zaps = 0;
    dev_a->drm.unplugged = 1;
    i915_gem_unplug(dev_a);
    CHECK(zaps == I915_MMAP_TYPES, "unplug: every kind of map revoked");
    CHECK(fault(&gtt, 0) == 0, "unplug: a fault after it, SIGBUS");
    dev_a->drm.unplugged = 0;
    unmap(&gtt);
    close_handle(&file_a, h);
}

/** Run every row on two devices: 0 when each passed. */
/** 78-5c: a GTT fault on an object whose binding the GPU still uses
 * outside the aperture binds a partial view of the whole object and does
 * not wait, as Linux's vm_fault_gtt, which IGT's pf-nonblock times. */
static void fault_busy(void)
{
    uint32_t h = create(&file_a, 2 * 4096);
    struct drm_i915_gem_object *obj = object(&file_a, h);
    struct i915_fence *rcs = engine_fence(0);
    struct i915_pin_args a = {I915_PIN_HIGH, 0, 0, 0};
    struct i915_vma *high = 0, *v;
    vma_t gtt;
    uint8_t *q;

    CHECK(i915_vma_pin_args(obj, &a, &high) == 0 && high && !high->mappable,
          "fault busy: the object bound above the aperture");
    i915_vma_unpin(high);
    CHECK(dma_resv_add_fence(obj->base.resv, &rcs->base, 1) == 0 && i915_gem_object_is_active(obj),
          "fault busy: and the GPU writes it");
    CHECK(map(&file_a, h, I915_MMAP_OFFSET_GTT, 2, &gtt) == 0, "fault busy: a GTT map");
    signal_on_wait = &rcs->base;
    q = fault(&gtt, 1);
    v = i915_vma_for_page(obj, 1);
    CHECK(q && v && v != high && v->view_offset == 0 && v->view_pages == 2 && v->mappable,
          "fault busy: a partial view of the whole object, inside the aperture");
    CHECK(!dma_fence_is_signaled(&rcs->base) && obj->vmas &&
              (obj->vmas == high || obj->vmas->obj_next == high),
          "fault busy: no wait, the busy binding kept");
    signal_on_wait = 0;
    dma_fence_signal(&rcs->base);
    unmap(&gtt);
    close_handle(&file_a, h);
    dma_fence_put(&rcs->base);
}

/** 78-5c: ptrace's reach into a GTT map and a write-back map, Linux's
 * vm_access, through which IGT's gem_mmap_gtt@ptrace peeks and pokes. */
static void map_access(void)
{
    uint32_t h = create(&file_a, 2 * 4096), i;
    struct drm_i915_gem_object *obj = object(&file_a, h);
    struct region *rg, *rw;
    vma_t gtt, wb;
    uint8_t *q, buf[16], want[16];
    int ok = 1;

    CHECK(map(&file_a, h, I915_MMAP_OFFSET_GTT, 2, &gtt) == 0 &&
              map(&file_a, h, I915_MMAP_OFFSET_WB, 2, &wb) == 0,
          "access: a GTT map and a write-back map");
    rg = (struct region *)gtt.file;
    rw = (struct region *)wb.file;
    q = fault(&gtt, 1);
    CHECK(q != 0, "access: the GTT map's second page faults");
    for (i = 0; i < 16; i++) {
        q[100 + i] = 0x55;
    }
    memset(buf, 0, sizeof(buf));
    memset(want, 0x55, sizeof(want));
    CHECK(rg->ops->access && rg->ops->access(rg->obj, 4096 + 100, buf, 16, 0) == 0 &&
              memcmp(buf, want, 16) == 0,
          "access: a peek through the GTT map reads what the aperture took");
    memset(buf, 0xaa, sizeof(buf));
    CHECK(rw->ops->access && rw->ops->access(rw->obj, 4096 + 100, buf, 16, 1) == 0 &&
              memcmp((uint8_t *)i915_gem_object_frame(obj, 1) + 100, buf, 16) == 0,
          "access: a poke through the write-back map lands in the page");
    for (i = 0; i < 16; i++) {
        buf[i] = (uint8_t)i;
    }
    CHECK(rg->ops->access(rg->obj, 4096 - 8, buf, 16, 1) == 0, "access: a poke across pages");
    for (i = 0; i < 8; i++) {
        ok &= ((uint8_t *)i915_gem_object_frame(obj, 0))[4088 + i] == i &&
              ((uint8_t *)i915_gem_object_frame(obj, 1))[i] == i + 8;
    }
    CHECK(ok, "access: its bytes in both pages");
    CHECK(rg->ops->access(rg->obj, 2 * 4096 - 8, buf, 16, 0) == -EINVAL,
          "access: past the end, EINVAL");
    unmap(&gtt);
    unmap(&wb);
    close_handle(&file_a, h);
}

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

int main(void)
{
    model_start();
    dev_a = bind(&chip_a, 1);
    dev_b = bind(&chip_b, 0);
    file_a.dev = &dev_a->drm;
    file_b.dev = &dev_b->drm;
    objects();
    create_ext_rows();
    domains();
    offsets();
    maps();
    legacy_maps();
    views();
    remapped();
    scanout();
    windows();
    arithmetic();
    tiling();
    busy_and_wait();
    fault_busy();
    map_access();
    prime();
    table_and_unplug();
    CHECK(!dev_a->drm.objects && !dev_b->drm.objects && !dev_a->gem.bound && !frames_live &&
              !pins_live,
          "the end: no object, binding, page or pin left");
    printf("i915-gem-test: %s\n", failures ? "failed" : "ok");
    return failures != 0;
}
