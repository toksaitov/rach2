/*
 * i915_gem_hosttest.c: the Intel driver's buffers over the real GEM, PRIME,
 * dma-buf and fence code, on a chip of the test's whose aperture holds what
 * a program or the kernel last wrote through it until the driver syncs it.
 * host-deps: rach2/sys/lib/kstring.c rach2/sys/lib/kctypes.c
 */

#include "drm/drm_gem.c"
#include "drm/drm_prime.c"
#include "dmabuf/fence.c"
#include "dmabuf/dmabuf.c"
#include "drm/i915/i915_gem.c"
#include "drm/i915/i915_vma.c"
#include "drm/i915/i915_gem_tiling.c"
#include "drm/i915/i915_gem_mman.c"
#include "drm/i915/i915_gem_prime.c"
#include "drm/i915/i915_gem_evict.c"
#include "drm/i915/i915_fence.c"
#include "drm/i915/i915_aperture.c"
#include "drm/i915/i915_reg.h"

extern int printf(const char *format, ...);
extern void *calloc(unsigned long n, unsigned long size);
extern void free(void *p);
extern int memcmp(const void *a, const void *b, unsigned long n);
extern int posix_memalign(void **p, unsigned long align, unsigned long size);

static int failures;

#define CHECK(c, s)                                 \
    do {                                            \
        if (!(c)) {                                 \
            printf("i915-gem-test: fail: %s\n", s); \
            failures++;                             \
        }                                           \
    } while (0)

/* ---- the chip ---- */

/* The test's chip: a GTT of 64 MiB whose first 16 MiB are the aperture,
 * eight of the driver's 2 MiB windows, twice what it maps at once. */
#define GTT_BYTES      0x4000000u
#define GTT_ENTRIES    (GTT_BYTES / 4096u)
#define APERTURE_BYTES 0x1000000u
#define REG_BYTES      0x200000u

/*
 * The chip of one device. `regs` holds what was last written to each
 * register. `gtt` holds each entry's page, 0 for none. `ap` is the
 * aperture as the processor reads it: a program's GTT map and the
 * driver's windows reach its bytes, and they reach memory only when the
 * driver syncs them (`aperture_sync`), as the chip's write-combining
 * buffers and the fake's shadows need. An entry or a fence that changes
 * fills its pages again from memory, losing what was not synced. The
 * controller swizzles X tiles by bits 9 and 10 and Y tiles by bit 9.
 * `fence_lo` and `fence_hi` are each fence's registers as last in force.
 */
struct chip {
    uint32_t *regs;
    uint64_t gtt[GTT_ENTRIES];
    uint8_t *ap;
    uint32_t fence_lo[I915_NUM_FENCES];
    uint32_t fence_hi[I915_NUM_FENCES];
    int maps, unmaps, syncs_to, syncs_from, fence_writes;
};

static struct chip chip_a, chip_b;

/** Bit `bit` of `offset` moved to bit 6, as Linux's selftest swizzle_bit. */
static uint64_t ref_swizzle_bit(unsigned bit, uint64_t offset)
{
    return (offset & ((uint64_t)1 << bit)) >> (bit - 6);
}

/**
 * Linux 6.12's selftest tiled_offset, from gem/selftests/i915_gem_mman.c
 * (MIT), with the selftest's tiles for graphics 7 with 128-byte Y tiling:
 * X 512 bytes by 8 rows in 4 KiB, Y 128 bytes by 32 rows. The reference
 * the driver's arithmetic and the chip's detiling are held to.
 */
static uint64_t ref_tiled_offset(uint32_t tiling, uint64_t stride, uint32_t swizzle, uint64_t v)
{
    uint64_t x, y, width = tiling == I915_TILING_X ? 512u : 128u;
    uint64_t height = tiling == I915_TILING_X ? 8u : 32u;

    if (tiling == I915_TILING_NONE) {
        return v;
    }
    y = v / stride;
    x = v % stride;
    v = (y / height) * stride * height;
    y %= height;
    if (tiling == I915_TILING_X) {
        v += y * width;
        v += (x / width) << 12;
        v += x % width;
    } else {
        v += y * 16u;
        v += (x / 16u) * 512u;
        v += x % 16u;
    }
    switch (swizzle) {
    case I915_BIT_6_SWIZZLE_9:
        v ^= ref_swizzle_bit(9, v);
        break;
    case I915_BIT_6_SWIZZLE_9_10:
        v ^= ref_swizzle_bit(9, v) ^ ref_swizzle_bit(10, v);
        break;
    case I915_BIT_6_SWIZZLE_9_11:
        v ^= ref_swizzle_bit(9, v) ^ ref_swizzle_bit(11, v);
        break;
    case I915_BIT_6_SWIZZLE_9_10_11:
        v ^= ref_swizzle_bit(9, v) ^ ref_swizzle_bit(10, v) ^ ref_swizzle_bit(11, v);
        break;
    default:
        break;
    }
    return v;
}

/** The fence over the aperture's offset `addr` as the chip holds it, into
 * `*lo` and `*hi`, or 0 and 0. */
static void chip_fence(const struct chip *c, uint32_t addr, uint32_t *lo, uint32_t *hi)
{
    int i;

    *lo = *hi = 0;
    for (i = 0; i < I915_NUM_FENCES; i++) {
        if ((c->fence_lo[i] & I965_FENCE_REG_VALID) && addr >= (c->fence_lo[i] & 0xfffff000u) &&
            addr < (c->fence_hi[i] & 0xfffff000u) + 4096u) {
            *lo = c->fence_lo[i];
            *hi = c->fence_hi[i];
            return;
        }
    }
}

/** Where the chip's aperture byte `addr` lies in memory, through its fence
 * and the GTT; NULL where the GTT maps nothing. */
static uint8_t *chip_memory(const struct chip *c, uint32_t addr)
{
    uint32_t lo, hi;
    uint64_t at = addr;

    chip_fence(c, addr, &lo, &hi);
    if (lo) {
        uint32_t start = lo & 0xfffff000u, stride = ((hi & 0xfffu) + 1u) * 128u;
        uint32_t tiling = (lo & 2u) ? I915_TILING_Y : I915_TILING_X;

        at = start + ref_tiled_offset(tiling, stride,
                                      tiling == I915_TILING_X ? I915_BIT_6_SWIZZLE_9_10
                                                              : I915_BIT_6_SWIZZLE_9,
                                      addr - start);
    }
    if (!c->gtt[at >> 12]) {
        return 0;
    }
    return (uint8_t *)(uintptr_t)c->gtt[at >> 12] + (at & 0xfffu);
}

/** Copy [offset, offset + size) of the aperture to memory, or from it, 16
 * bytes at a time, a run the detiler never splits. */
static void chip_copy(struct chip *c, uint64_t offset, uint64_t size, int to_memory)
{
    uint64_t at, end = offset + size;

    if (end > APERTURE_BYTES) {
        end = APERTURE_BYTES;
    }
    for (at = offset & ~(uint64_t)15u; at < end; at += 16u) {
        uint8_t *m = chip_memory(c, (uint32_t)at);

        if (to_memory) {
            if (m) {
                k_memcpy(m, c->ap + at, 16);
            }
        } else if (m) {
            k_memcpy(c->ap + at, m, 16);
        } else {
            k_memset(c->ap + at, 0, 16);
        }
    }
}

/** The chip of the device `hw` is. */
static struct chip *chip_of(struct i915_hw *hw)
{
    return (struct chip *)hw->priv;
}

/** i915_hw.h's `aperture_sync`, which the driver calls where the chip's
 * aperture would need nothing. */
static void chip_sync(struct i915_hw *hw, uint64_t offset, uint64_t size, int to_memory)
{
    struct chip *c = chip_of(hw);

    if (to_memory) {
        c->syncs_to++;
    } else {
        c->syncs_from++;
    }
    chip_copy(c, offset, size, to_memory);
}

static const struct i915_hw_ops chip_ops = {.name = "test", .aperture_sync = chip_sync};

/* ---- the driver's boundary, over the chip ---- */

void i915_write(struct i915_hw *hw, uint32_t reg, uint32_t value)
{
    struct chip *c = chip_of(hw);

    c->regs[reg / 4] = value;
    if (reg >= FENCE_REG_GEN6_LO(0) && reg < FENCE_REG_GEN6_LO(I915_NUM_FENCES) && !(reg & 4u)) {
        int i = (int)((reg - FENCE_REG_GEN6_LO(0)) / 8u);
        uint32_t old_lo = c->fence_lo[i], old_hi = c->fence_hi[i];

        c->fence_writes++;
        c->fence_lo[i] = value;
        c->fence_hi[i] = c->regs[FENCE_REG_GEN6_HI(i) / 4];
        /* What the aperture shows under the old fence and the new. */
        if (old_lo & I965_FENCE_REG_VALID) {
            chip_copy(c, old_lo & 0xfffff000u,
                      (old_hi & 0xfffff000u) + 4096u - (old_lo & 0xfffff000u), 0);
        }
        if (value & I965_FENCE_REG_VALID) {
            chip_copy(c, value & 0xfffff000u,
                      (c->fence_hi[i] & 0xfffff000u) + 4096u - (value & 0xfffff000u), 0);
        }
    }
}

uint32_t i915_read(struct i915_hw *hw, uint32_t reg)
{
    return chip_of(hw)->regs[reg / 4];
}

void i915_posting_read(struct i915_hw *hw, uint32_t reg)
{
    (void)i915_read(hw, reg);
}

uint32_t i915_rmw(struct i915_hw *hw, uint32_t reg, uint32_t clear, uint32_t set)
{
    uint32_t old = i915_read(hw, reg);

    i915_write(hw, reg, (old & ~clear) | set);
    return old;
}

uint32_t i915_uncore_read(struct drm_i915_private *i915, uint32_t reg)
{
    return i915_read(&i915->hw, reg);
}

void i915_uncore_write(struct drm_i915_private *i915, uint32_t reg, uint32_t value)
{
    i915_write(&i915->hw, reg, value);
}

static int bind_fail;

int32_t i915_ggtt_insert(struct drm_i915_private *i915, uint64_t offset, const uint64_t *pages,
                         uint32_t n, int cache)
{
    struct chip *c = chip_of(&i915->hw);
    uint32_t i;

    if (bind_fail) {
        bind_fail--;
        return -ENOSPC;
    }
    if (offset + (uint64_t)n * 4096u > GTT_BYTES || (offset & 0xfffu)) {
        return -EINVAL;
    }
    for (i = 0; i < n; i++) {
        c->gtt[(offset >> 12) + i] = pages[i];
    }
    (void)cache;
    chip_copy(c, offset, (uint64_t)n * 4096u, 0);
    return 0;
}

int32_t i915_ggtt_clear_range(struct drm_i915_private *i915, uint64_t offset, uint64_t size)
{
    struct chip *c = chip_of(&i915->hw);
    uint64_t i;

    if (offset + size > GTT_BYTES) {
        return -EINVAL;
    }
    for (i = 0; i < size >> 12; i++) {
        c->gtt[(offset >> 12) + i] = 0;
    }
    chip_copy(c, offset, size, 0);
    return 0;
}

/** The driver's windows: the chip's aperture, by its address. */
uintptr_t mmio_map(uint64_t phys, size_t size, pte_t cache)
{
    (void)size;
    (void)cache;
    if (phys >= (uintptr_t)chip_a.ap && phys < (uintptr_t)chip_a.ap + APERTURE_BYTES) {
        chip_a.maps++;
    } else {
        chip_b.maps++;
    }
    return (uintptr_t)phys;
}

void mmio_unmap(uintptr_t va, size_t size)
{
    (void)size;
    if (va >= (uintptr_t)chip_a.ap && va < (uintptr_t)chip_a.ap + APERTURE_BYTES) {
        chip_a.unmaps++;
    } else {
        chip_b.unmaps++;
    }
}

int pat_write_combining(void)
{
    return 1;
}

/* ---- the processor and memory ---- */

static int flushes, fences_dma;

void arch_dma_flush(const volatile void *p, size_t len)
{
    (void)p;
    (void)len;
    flushes++;
}

void arch_dma_fence(void)
{
    fences_dma++;
}

/** A frame is its host page: the direct map. */
void *kmap_temp(uintptr_t phys)
{
    return (void *)phys;
}

void kunmap_temp(void *va)
{
    (void)va;
}

static int frames_live, pins_live;

/** The page `page` of `sh`, given or 0. */
uintptr_t shmem_resident(const struct shmem *sh, uint32_t page)
{
    return sh->leaves && page < sh->npages ? sh->leaves[0][page] : 0;
}

/** Give [off, off + len) of `sh` zeroed pages, in one leaf as long as the
 * file. */
int shmem_allocate(struct shmem *sh, uint64_t off, uint64_t len)
{
    uint32_t page;

    if (!sh->leaves) {
        sh->npages = (uint32_t)((sh->size + 4095u) >> 12);
        sh->leaves = (uintptr_t **)calloc(1, sizeof(*sh->leaves));
        sh->leaves[0] = (uintptr_t *)calloc(sh->npages, sizeof(uintptr_t));
        sh->nleaves = 1;
    }
    for (page = (uint32_t)(off >> 12); page < (off + len + 4095u) >> 12; page++) {
        void *p;

        if (page >= sh->npages) {
            return -EFBIG;
        }
        if (sh->leaves[0][page]) {
            continue;
        }
        if (posix_memalign(&p, 4096, 4096) != 0) {
            return -ENOMEM;
        }
        k_memset(p, 0, 4096);
        sh->leaves[0][page] = (uintptr_t)p;
        frames_live++;
    }
    return 0;
}

void shmem_free(struct shmem *sh)
{
    uint32_t i;

    if (!sh->leaves) {
        return;
    }
    for (i = 0; i < sh->npages; i++) {
        if (sh->leaves[0][i]) {
            free((void *)sh->leaves[0][i]);
            frames_live--;
        }
    }
    free(sh->leaves[0]);
    free(sh->leaves);
    sh->leaves = 0;
}

int pin_frames(struct mm *mm, const uintptr_t *frames, uint32_t count,
               const struct pin_holder *holder, struct pin **out)
{
    (void)mm;
    (void)frames;
    (void)count;
    (void)holder;
    pins_live++;
    *out = (struct pin *)calloc(1, 16);
    return 0;
}

void unpin(struct pin *pin)
{
    if (pin) {
        pins_live--;
        free(pin);
    }
}

static int allocations;

void *kmalloc(size_t size)
{
    void *p = calloc(1, size);

    if (p) {
        allocations++;
    }
    return p;
}

void *kzalloc(size_t size)
{
    return kmalloc(size);
}

void kfree(void *p)
{
    if (p) {
        allocations--;
    }
    free(p);
}

int user_range_ok(uintptr_t addr, size_t len)
{
    return addr >= 4096 && addr + len >= addr;
}

int copy_from_user(void *dst, const void __user *src, size_t n)
{
    if ((uintptr_t)src < 4096) {
        return -EFAULT;
    }
    k_memcpy(dst, src, n);
    return 0;
}

int copy_to_user(void __user *dst, const void *src, size_t n)
{
    if ((uintptr_t)dst < 4096) {
        return -EFAULT;
    }
    k_memcpy(dst, src, n);
    return 0;
}

void klog(int level, const char *subsystem, const char *fmt, ...)
{
    (void)level;
    (void)subsystem;
    (void)fmt;
}

/* ---- processes, descriptors, clocks and waits ---- */

struct percpu cpu_table[NR_CPUS];
static process_t self;
static fd_table_t table;
static fd_entry_t slots[64];
static struct open_file descriptions[64];
static int64_t now_ns;
static struct dma_fence *signal_on_wait;

void rtc_monotonic(int64_t *sec, int64_t *ns)
{
    *sec = now_ns / 1000000000ll;
    *ns = now_ns % 1000000000ll;
}

uint64_t rtc_ticks_now(void)
{
    return (uint64_t)(now_ns / 1000000);
}

uint64_t tick_div(uint64_t n, uint32_t d, uint32_t *rem)
{
    if (rem) {
        *rem = (uint32_t)(n % d);
    }
    return n / d;
}

void wait_queue_init(wait_queue_t *wq)
{
    k_memset(wq, 0, sizeof(*wq));
}

void wq_wake_all(wait_queue_t *wq)
{
    (void)wq;
}

/** A sleep: the fence the test named signals, or the clock reaches the
 * sleeper's timeout. */
int wq_wait_event(wait_queue_t *wq, wq_cond_fn cond, void *arg)
{
    (void)wq;
    if (signal_on_wait) {
        dma_fence_signal(signal_on_wait);
        signal_on_wait = 0;
    } else {
        now_ns = (int64_t)current_proc->wakeup_tick * 1000000;
    }
    (void)cond;
    (void)arg;
    return 0;
}

int wq_wait_event_nointr(wait_queue_t *wq, wq_cond_fn cond, void *arg)
{
    return wq_wait_event(wq, cond, arg);
}

int fd_alloc(fd_table_t *t)
{
    int i;

    for (i = 0; i < 64; i++) {
        if (t->entries[i].type == FD_TYPE_NONE) {
            k_memset(&descriptions[i], 0, sizeof(descriptions[i]));
            t->entries[i].ofd = &descriptions[i];
            return i;
        }
    }
    return -EMFILE;
}

fd_entry_t *fd_get(fd_table_t *t, int fd)
{
    return fd >= 0 && fd < 64 && t->entries[fd].type != FD_TYPE_NONE ? &t->entries[fd] : NULL;
}

int fd_free(fd_table_t *t, int fd)
{
    fd_entry_t *e = fd_get(t, fd);

    if (!e) {
        return -EBADF;
    }
    if (e->ops && e->ops->close) {
        e->ops->close(e);
    }
    k_memset(e, 0, sizeof(*e));
    return 0;
}

int32_t sync_file_create(struct dma_fence *fence, const char *name)
{
    (void)fence;
    (void)name;
    return -ENOMEM;
}

struct dma_fence *sync_file_get_fence(int32_t fd)
{
    (void)fd;
    return 0;
}

void drm_dev_get(struct drm_device *dev)
{
    dev->refs++;
}

void drm_dev_put(struct drm_device *dev)
{
    dev->refs--;
}

/* ---- programs' mappings ---- */

/* A mapping as vm_device_map makes it. */
struct region {
    struct vm_file vf;
    const struct vm_device_ops *ops;
    void *obj;
    uint32_t cache;
};

static const void *zapped;
static uint32_t zap_pgoff, zap_count;
static int zaps;

int vm_device_map(vma_t *vma, const struct vm_device_ops *ops, void *obj, uint32_t cache)
{
    struct region *r;

    if (vma->type != VM_SHARED) {
        return -EINVAL;
    }
    r = (struct region *)kzalloc(sizeof(*r));
    if (!r) {
        return -ENOMEM;
    }
    r->ops = ops;
    r->obj = obj;
    r->cache = cache;
    vma->file = &r->vf;
    return 0;
}

void paging_zap_device(const void *obj, uint32_t pgoff, uint32_t count)
{
    zapped = obj;
    zap_pgoff = pgoff;
    zap_count = count;
    zaps++;
}

/** Fault page `page` of the mapping `vma`: its address, or NULL for
 * SIGBUS. */
static uint8_t *fault(vma_t *vma, uint32_t page)
{
    struct region *r = (struct region *)vma->file;
    uintptr_t phys = 0;

    if (r->ops->page(r->obj, page, &phys) != 0) {
        return 0;
    }
    return (uint8_t *)phys;
}

/** The mapping's last region goes. */
static void unmap(vma_t *vma)
{
    struct region *r = (struct region *)vma->file;

    r->ops->put(r->obj);
    kfree(r);
    vma->file = 0;
}

/* ---- the devices ---- */

static const struct i915_platform hsw = {.name = "HASWELL", .graphics_ver = 7, .has_llc = 1};
static const struct i915_id hsw_id = {0x0a16, 1, 0, &hsw};
static const struct drm_driver driver = {.ident = DRM_IDENT_I915,
                                         .gem_prime_import = i915_gem_prime_import,
                                         .ioctls = i915_ioctls,
                                         .num_ioctls = I915_NUM_IOCTLS};
static struct drm_i915_private *dev_a, *dev_b;
static struct drm_file file_a, file_b;

/** Bind a device of the test's over the chip `c`, its channels' DIMMs of
 * one size or not. */
static struct drm_i915_private *bind(struct chip *c, int interleaved)
{
    struct drm_i915_private *i915 = (struct drm_i915_private *)calloc(1, sizeof(*i915));

    if (!c->regs) {
        c->regs = (uint32_t *)calloc(REG_BYTES / 4, 4);
        c->ap = (uint8_t *)calloc(1, APERTURE_BYTES);
    }
    c->regs[MAD_DIMM_C0 / 4] = 0x00000808u;
    c->regs[MAD_DIMM_C1 / 4] = interleaved ? 0x00000808u : 0x00000004u;
    i915->drm.driver = &driver;
    i915->drm.features = DRIVER_GEM;
    i915->drm.refs = 1;
    i915->drm.driver_priv = i915;
    i915->id = &hsw_id;
    i915->hw.ops = &chip_ops;
    i915->hw.priv = c;
    i915->gmadr_phys = (uintptr_t)c->ap;
    i915->ggtt.total = GTT_BYTES;
    i915->ggtt.mappable_end = APERTURE_BYTES;
    i915->gt.ggtt_bottom = GTT_BYTES;
    i915_aperture_init(&i915->gem.aperture, i915->gmadr_phys, i915->ggtt.mappable_end);
    i915_gem_init_hw(i915);
    return i915;
}

/** Make an object of `size` bytes through GEM_CREATE: its handle. */
static uint32_t create(struct drm_file *f, uint64_t size)
{
    struct drm_i915_gem_create args = {0};

    args.size = size;
    CHECK(i915_gem_create_ioctl(f->dev, &args, f) == 0, "create: an object is made");
    return args.handle;
}

/** The object of `handle`, borrowed. */
static struct drm_i915_gem_object *object(struct drm_file *f, uint32_t handle)
{
    struct drm_i915_gem_object *obj = i915_gem_object_lookup(f, handle);

    i915_gem_object_put(obj);
    return obj;
}

/** PWRITE or PREAD `size` bytes at `offset` of `handle`: the ioctl's
 * answer. */
static int32_t prw(struct drm_file *f, uint32_t handle, uint64_t offset, void *buf, uint64_t size,
                   int write)
{
    if (write) {
        struct drm_i915_gem_pwrite w = {0};

        w.handle = handle;
        w.offset = offset;
        w.size = size;
        w.data_ptr = (uintptr_t)buf;
        return i915_gem_pwrite_ioctl(f->dev, &w, f);
    } else {
        struct drm_i915_gem_pread r = {0};

        r.handle = handle;
        r.offset = offset;
        r.size = size;
        r.data_ptr = (uintptr_t)buf;
        return i915_gem_pread_ioctl(f->dev, &r, f);
    }
}

/** SET_DOMAIN's answer for `read` and `write`. */
static int32_t domain(struct drm_file *f, uint32_t handle, uint32_t read, uint32_t write)
{
    struct drm_i915_gem_set_domain args = {0};

    args.handle = handle;
    args.read_domains = read;
    args.write_domain = write;
    return i915_gem_set_domain_ioctl(f->dev, &args, f);
}

/** The offset MMAP_OFFSET gives `handle` for `flags`, or 0 with `*err`. */
static uint64_t mmap_offset(struct drm_file *f, uint32_t handle, uint64_t flags, int32_t *err)
{
    struct drm_i915_gem_mmap_offset args = {0};

    args.handle = handle;
    args.flags = flags;
    *err = i915_gem_mmap_offset_ioctl(f->dev, &args, f);
    return *err ? 0 : args.offset;
}

/** Map `pages` pages of `handle` of the kind `flags` into `vma`: 0 or the
 * error of MMAP_OFFSET or the mapping. */
static int32_t map(struct drm_file *f, uint32_t handle, uint64_t flags, uint32_t pages, vma_t *vma)
{
    int32_t err;
    uint64_t off = mmap_offset(f, handle, flags, &err);

    if (err) {
        return err;
    }
    k_memset(vma, 0, sizeof(*vma));
    vma->type = VM_SHARED;
    vma->start = 0x10000000u;
    vma->end = vma->start + (uintptr_t)pages * 4096u;
    vma->pgoff = (uint32_t)(off >> 12);
    return drm_gem_mmap(f, vma);
}

/** SET_TILING's answer, with the arguments written back in `*args`. */
static int32_t tile(struct drm_file *f, uint32_t handle, uint32_t tiling, uint32_t stride,
                    struct drm_i915_gem_set_tiling *args)
{
    k_memset(args, 0, sizeof(*args));
    args->handle = handle;
    args->tiling_mode = tiling;
    args->stride = stride;
    return i915_gem_set_tiling_ioctl(f->dev, args, f);
}

/** Close `handle` of `f`. */
static void close_handle(struct drm_file *f, uint32_t handle)
{
    CHECK(drm_gem_handle_delete(f, handle) == 0, "close: the handle goes");
}

/** Export `handle` of `f` as a descriptor. */
static int export_fd(struct drm_file *f, uint32_t handle)
{
    struct drm_prime_handle args = {0, DRM_RDWR, -1};

    args.handle = handle;
    CHECK(drm_prime_handle_to_fd_ioctl(f->dev, &args, f) == 0, "prime: export");
    return args.fd;
}

/** Import the descriptor `fd` into `f`: its handle. */
static uint32_t import_fd(struct drm_file *f, int fd)
{
    struct drm_prime_handle args = {0, 0, -1};

    args.fd = fd;
    CHECK(drm_prime_fd_to_handle_ioctl(f->dev, &args, f) == 0, "prime: import");
    return args.handle;
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
 * tiled with a stride of 2048 bytes. */
static void tiling_ioctls(uint32_t h)
{
    struct drm_i915_gem_set_tiling st;
    struct drm_i915_gem_get_tiling gt = {0};

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
        bad += !i915_ioctls[i].func || !(i915_ioctls[i].flags & DRM_RENDER_ALLOW);
        for (j = 0; j < i; j++) {
            dup += UAPI_IOC_NR(i915_ioctls[i].cmd) == UAPI_IOC_NR(i915_ioctls[j].cmd);
        }
    }
    CHECK(!bad && !dup, "ioctls: each has a handler, a render node may call it, numbers apart");
    CHECK(UAPI_IOC_NR(DRM_IOCTL_I915_GEM_MMAP_GTT) == UAPI_IOC_NR(DRM_IOCTL_I915_GEM_MMAP_OFFSET) &&
              UAPI_IOC_SIZE(DRM_IOCTL_I915_GEM_MMAP_OFFSET) == 32,
          "ioctls: MMAP_OFFSET extends MMAP_GTT's number");
    h = create(&file_a, 4096);
    CHECK(map(&file_a, h, I915_MMAP_OFFSET_GTT, 1, &gtt) == 0, "mmap: before the unplug");
    bind_fail = 2;
    CHECK(fault(&gtt, 0) == 0 && !object(&file_a, h)->vmas &&
              object(&file_a, h)->pages_pin_count == 0,
          "fault: the binds fail, SIGBUS, nothing left bound and the pages let go");
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
int main(void)
{
    table.entries = slots;
    table.cap = 64;
    self.fds = &table;
    cpu_table[0].current = &self;
    dev_a = bind(&chip_a, 1);
    dev_b = bind(&chip_b, 0);
    file_a.dev = &dev_a->drm;
    file_b.dev = &dev_b->drm;
    objects();
    domains();
    offsets();
    maps();
    views();
    windows();
    arithmetic();
    tiling();
    busy_and_wait();
    prime();
    table_and_unplug();
    CHECK(!dev_a->drm.objects && !dev_b->drm.objects && !dev_a->gem.bound && !frames_live &&
              !pins_live,
          "the end: no object, binding, page or pin left");
    printf("i915-gem-test: %s\n", failures ? "failed" : "ok");
    return failures != 0;
}
