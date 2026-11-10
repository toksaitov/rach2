/*
 * i915_gem_model.c: the Intel driver's buffer code whole, over the real GEM,
 * PRIME, dma-buf, fence and address-space notifier code, on a chip of the
 * test's whose aperture holds what a program or the kernel last wrote
 * through it until the driver syncs it, with the kernel around it: memory
 * with a limit and reclaim, a program's memory for userptr, descriptors,
 * clocks, waits and programs' mappings. i915_gem_hosttest.c,
 * i915_evict_hosttest.c and i915_exec_hosttest.c include it, and list
 * kstring.c and kctypes.c in their host-deps.
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
#include "drm/i915/i915_gem_shrinker.c"
#include "drm/i915/i915_gem_userptr.c"
#include "mm/notify.c"
#include "drm/i915/i915_fence.c"
#include "drm/i915/i915_aperture.c"
#include "drm/i915/i915_reg.h"

extern int printf(const char *format, ...);
extern void *calloc(unsigned long n, unsigned long size);
extern void free(void *p);
extern int memcmp(const void *a, const void *b, unsigned long n);
extern int posix_memalign(void **p, unsigned long align, unsigned long size);

static int failures;

/* The name a failure is printed under, which the including test may set. */
#ifndef MODEL_TEST
#define MODEL_TEST "i915-gem-test"
#endif

#define CHECK(c, s)                                  \
    do {                                             \
        if (!(c)) {                                  \
            printf("%s: fail: %s\n", MODEL_TEST, s); \
            failures++;                              \
        }                                            \
    } while (0)

/* The calls of contexts, requests, parameters and submission, which the
 * table names and no row here makes, unless the including test defines
 * MODEL_HAS_REQUESTS and includes their files. */
#ifndef MODEL_HAS_REQUESTS
#define NOT_HERE(name)                                                      \
    int32_t name(struct drm_device *dev, void *data, struct drm_file *file) \
    {                                                                       \
        (void)dev;                                                          \
        (void)data;                                                         \
        (void)file;                                                         \
        return -ENODEV;                                                     \
    }
NOT_HERE(i915_getparam_ioctl)
NOT_HERE(i915_query_ioctl)
NOT_HERE(i915_gem_context_create_ioctl)
NOT_HERE(i915_gem_context_destroy_ioctl)
NOT_HERE(i915_gem_context_getparam_ioctl)
NOT_HERE(i915_gem_context_setparam_ioctl)
NOT_HERE(i915_gem_context_reset_stats_ioctl)
NOT_HERE(i915_gem_execbuffer2_ioctl)
NOT_HERE(i915_gem_throttle_ioctl)

/* The GPU's idling, which eviction waits for when only busy bindings are
 * left: `idle_signals`, a fence a row names, signals, as its request would
 * retire. `idle_waits` counts the waits. */
static struct dma_fence *idle_signals;
static int idle_waits;

int32_t i915_gt_wait_for_idle(struct drm_i915_private *i915, int intr)
{
    (void)i915;
    (void)intr;
    idle_waits++;
    if (idle_signals) {
        dma_fence_signal(idle_signals);
        idle_signals = 0;
    }
    return 0;
}

/* The aliasing PPGTT, which comes with the rings: none here. */
int32_t i915_ppgtt_insert(struct drm_i915_private *i915, uint64_t offset, const uint64_t *pages,
                          uint32_t n, int cache)
{
    (void)i915;
    (void)offset;
    (void)pages;
    (void)n;
    (void)cache;
    return 0;
}

void i915_ppgtt_clear_range(struct drm_i915_private *i915, uint64_t offset, uint64_t size)
{
    (void)i915;
    (void)offset;
    (void)size;
}
#endif

/** GET_PIPE_FROM_CRTC_ID, which the table names; the display is not here. */
int32_t intel_get_pipe_from_crtc_id_ioctl(struct drm_device *dev, void *data, struct drm_file *file)
{
    (void)dev;
    (void)data;
    (void)file;
    return -ENOENT;
}

/** REG_READ, which the table names and i915_ctx_hosttest.c runs on its
 * register model. */
int32_t i915_reg_read_ioctl(struct drm_device *dev, void *data, struct drm_file *file)
{
    (void)dev;
    (void)data;
    (void)file;
    return -ENODEV;
}

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

/* What a test's chip does besides keeping a register's value, such as
 * running a ring whose tail moved; NULL for nothing. */
static void (*chip_write_hook)(struct chip *c, uint32_t reg, uint32_t value);

void i915_write(struct i915_hw *hw, uint32_t reg, uint32_t value)
{
    struct chip *c = chip_of(hw);

    c->regs[reg / 4] = value;
    if (chip_write_hook) {
        chip_write_hook(c, reg, value);
    }
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

/*
 * Memory with a limit. With `frame_limit` above 0, a frame is refused once
 * `frames_live` reaches it. Reclaim, as pressure.h tells it, asks each
 * registered shrinker for frames until the free count is an eighth of the
 * limit or none gives any; `mem_balance` reclaims once fewer than a
 * sixteenth are free. `reclaims` counts the reclaims run.
 */
static int frame_limit, reclaims;
static struct mem_shrinker *shrinker_list;

void mem_shrinker_register(struct mem_shrinker *sk)
{
    sk->next = shrinker_list;
    shrinker_list = sk;
}

void mem_shrinker_unregister(struct mem_shrinker *sk)
{
    struct mem_shrinker **link;

    for (link = &shrinker_list; *link; link = &(*link)->next) {
        if (*link == sk) {
            *link = sk->next;
            return;
        }
    }
}

uint32_t mem_reclaim(void)
{
    struct mem_shrinker *sk;
    uint32_t got = 0;
    int high = frame_limit / 8;

    reclaims++;
    for (sk = shrinker_list; sk && frame_limit - frames_live < high; sk = sk->next) {
        got += sk->shrink(sk, (uint32_t)(high - (frame_limit - frames_live)), 1);
    }
    return got;
}

void mem_balance(void)
{
    if (frame_limit && frame_limit - frames_live < frame_limit / 16) {
        (void)mem_reclaim();
    }
}

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
        if ((frame_limit && frames_live >= frame_limit) || posix_memalign(&p, 4096, 4096) != 0) {
            return -ENOSPC;
        }
        k_memset(p, 0, 4096);
        sh->leaves[0][page] = (uintptr_t)p;
        sh->held++;
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
    sh->held = 0;
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

int user_span_ok(uintptr_t addr, size_t len)
{
    return addr + len >= addr;
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

/*
 * A program's memory, for userptr: the regions of `user_mm` in address
 * order, `user_nregions` of `user_regions`, over host memory the test
 * gives, so that a page's frame is its address. A region whose file is
 * `device_file` stands for a device's mapping. `user_pins` counts the
 * frames pinned; `pin_sleep`, when set, runs once inside the next pin, as
 * another thread would while the pin sleeps.
 */
#define USER_REGIONS 4
static vma_t user_regions[USER_REGIONS];
static int user_nregions;
static struct mm user_mm;
static struct vm_file device_file;
static int user_pins;
static void (*pin_sleep)(void);

vma_t *vm_find(vm_space_t *vm, uintptr_t addr)
{
    int i;

    (void)vm;
    for (i = 0; i < user_nregions; i++) {
        if (user_regions[i].start <= addr && addr < user_regions[i].end) {
            return &user_regions[i];
        }
    }
    return 0;
}

vma_t *vm_next(const vma_t *r)
{
    int i = (int)(r - user_regions) + 1;

    return i < user_nregions ? &user_regions[i] : 0;
}

void *vm_device_object(const struct vm_file *f)
{
    return f == &device_file ? (void *)&device_file : 0;
}

int paging_pin_range(struct mm *mm, uintptr_t start, uintptr_t end, int write, uintptr_t *frames)
{
    uintptr_t page;
    uint32_t i = 0;
    void (*hook)(void) = pin_sleep;

    for (page = start; page < end; page += 4096u) {
        const vma_t *r = vm_find(&mm->vm, page);

        if (!r || vm_device_object(r->file) || (write && !(r->prot & VM_PROT_WRITE))) {
            return -EFAULT;
        }
    }
    for (page = start; page < end; page += 4096u) {
        frames[i++] = page;
        user_pins++;
    }
    pin_sleep = 0;
    if (hook) {
        hook();
    }
    return 0;
}

void frame_unpin(uintptr_t phys)
{
    (void)phys;
    user_pins--;
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

#ifndef MODEL_HAS_SYNC_FILE
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
#endif

void drm_dev_get(struct drm_device *dev)
{
    dev->refs++;
}

void drm_dev_put(struct drm_device *dev)
{
    dev->refs--;
}

/** Give the test's process its descriptors and its address space. */
static void model_start(void)
{
    table.entries = slots;
    table.cap = 64;
    self.fds = &table;
    self.mm = &user_mm;
    cpu_table[0].current = &self;
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

/** Fault page `page` of the mapping `vma`, the object's page vma->pgoff +
 * `page` as the kernel's fault asks: its address, or NULL for SIGBUS. */
static uint8_t *fault(vma_t *vma, uint32_t page)
{
    struct region *r = (struct region *)vma->file;
    uintptr_t phys = 0;

    if (r->ops->page(r->obj, vma->pgoff + page, &phys) != 0) {
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

/* The region the last GEM_MMAP made, as mmap_device gives it to the
 * driver, and the address it answers. */
static vma_t mmapped;
#define MMAPPED_AT 0x40000000L

long mmap_device(uintptr_t len, uint32_t prot, int32_t (*map)(vma_t *vma, void *arg), void *arg)
{
    int32_t rc;

    if (len == 0) {
        return -EINVAL;
    }
    k_memset(&mmapped, 0, sizeof(mmapped));
    mmapped.end = (len + 4095u) & ~(uintptr_t)4095u;
    mmapped.prot = prot;
    mmapped.type = VM_SHARED;
    rc = map(&mmapped, arg);
    return rc != 0 ? rc : MMAPPED_AT;
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

static vma_t legacy_vma;

/** The kernel's drm_vm_mmap: map `size` bytes of the device's mmap space
 * from `offset` into legacy_vma through drm_gem_mmap, as the process's
 * descriptor of the device would; a fake address or the error. */
long drm_vm_mmap(struct drm_file *file, uint64_t offset, uint64_t size)
{
    int32_t err;

    if ((offset & 4095u) != 0 || size == 0) {
        return -EINVAL;
    }
    k_memset(&legacy_vma, 0, sizeof(legacy_vma));
    legacy_vma.type = VM_SHARED;
    legacy_vma.start = 0x20000000u;
    legacy_vma.end = legacy_vma.start + (uintptr_t)((size + 4095u) & ~(uint64_t)4095u);
    legacy_vma.pgoff = (uint32_t)(offset >> 12);
    err = drm_gem_mmap(file, &legacy_vma);
    return err ? err : (long)legacy_vma.start;
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
