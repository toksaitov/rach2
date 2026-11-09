/* i915gemprobe.c: the Intel driver's buffers on the fake chip in QEMU: an X
 * tiled object written through a GTT map reads back through a CPU map where
 * the tiling and the swizzle put each byte. Runs under rach2.i915fake. */
#include "probe.h"
#include "probe_report.h"
#include "../rach2/sys/drm/i915/i915_drm.h"

/* The object: four tile rows of X tiles, 2048 bytes a row, 64 KiB. */
#define OBJ_BYTES  65536u
#define OBJ_STRIDE 2048u

static int gpu_fd = -1;
static uint32_t bo_handle;
static void *gtt_map, *cpu_map;

/** Clear an ioctl argument. */
static void zero(void *p, size_t n)
{
    unsigned char *s = p;

    while (n--) {
        *s++ = 0;
    }
}

/** The ioctl's result, a negated errno on failure. */
static long call(unsigned long op, void *arg)
{
    return syscall3(SYS_IOCTL, gpu_fd, (long)op, (long)arg);
}

/** Unmap, close the object and the node, then exit with `rc`. */
static void finish(int rc)
{
    struct drm_gem_close c;

    if (gtt_map) {
        syscall2(SYS_MUNMAP, (long)gtt_map, OBJ_BYTES);
    }
    if (cpu_map) {
        syscall2(SYS_MUNMAP, (long)cpu_map, OBJ_BYTES);
    }
    if (bo_handle) {
        zero(&c, sizeof(c));
        c.handle = bo_handle;
        (void)call(DRM_IOCTL_GEM_CLOSE, &c);
    }
    if (gpu_fd >= 0) {
        syscall1(SYS_CLOSE, gpu_fd);
    }
    syscall1(SYS_EXIT, rc);
    __builtin_unreachable();
}

/** Fail the open row, clean up and stop, unless `ok`. */
static void require(int ok, const char *what)
{
    if (!ok) {
        probe_line("I915GEMPROBE FAIL: ", what, 0);
        report_stop(what);
        finish(1);
    }
}

/** Open the first render node whose driver is i915. */
static void find_gpu(void)
{
    char path[] = "/dev/dri/renderD128", name[8];
    struct drm_version v;
    int i;

    for (i = 0; i < 10; i++) {
        path[18] = (char)('0' + (8 + i) % 10);
        path[17] = (char)('2' + (8 + i) / 10);
        gpu_fd = (int)syscall3(SYS_OPEN, (long)path, 2, 0);
        if (gpu_fd < 0) {
            continue;
        }
        zero(&v, sizeof(v));
        zero(name, sizeof(name));
        v.name = name;
        v.name_len = sizeof(name) - 1;
        if (call(DRM_IOCTL_VERSION, &v) == 0 && v.name_len == 4 && name[0] == 'i' &&
            name[1] == '9' && name[2] == '1' && name[3] == '5') {
            return;
        }
        syscall1(SYS_CLOSE, gpu_fd);
        gpu_fd = -1;
    }
    require(0, "no i915 render node");
}

/** Map `handle` of the kind `flags` shared, read and write: the address. */
static void *map(uint32_t flags)
{
    struct drm_i915_gem_mmap_offset m;
    long address;

    zero(&m, sizeof(m));
    m.handle = bo_handle;
    m.flags = flags;
    require(call(DRM_IOCTL_I915_GEM_MMAP_OFFSET, &m) == 0 && m.offset != 0, "mmap offset");
#ifdef __x86_64__
    address = syscall6(SYS_MMAP, 0, OBJ_BYTES, 3, 1, gpu_fd, (long)m.offset);
#else
    address = syscall6(SYS_MMAP2, 0, OBJ_BYTES, 3, 1, gpu_fd, (long)(m.offset >> 12));
#endif
    require((unsigned long)address < (unsigned long)-4095, "mmap");
    return (void *)address;
}

/** Move the object to the domains `read` and `write`. */
static void domain(uint32_t read, uint32_t write)
{
    struct drm_i915_gem_set_domain d;

    zero(&d, sizeof(d));
    d.handle = bo_handle;
    d.read_domains = read;
    d.write_domain = write;
    require(call(DRM_IOCTL_I915_GEM_SET_DOMAIN, &d) == 0, "set domain");
}

/** Make the object and tile it X with OBJ_STRIDE: the swizzle the driver
 * says programs must apply. */
static uint32_t object_row(void)
{
    struct drm_i915_gem_create c;
    struct drm_i915_gem_set_tiling t;

    find_gpu();
    zero(&c, sizeof(c));
    c.size = OBJ_BYTES;
    require(call(DRM_IOCTL_I915_GEM_CREATE, &c) == 0 && c.handle && c.size == OBJ_BYTES, "create");
    bo_handle = c.handle;
    zero(&t, sizeof(t));
    t.handle = bo_handle;
    t.tiling_mode = I915_TILING_X;
    t.stride = OBJ_STRIDE;
    require(call(DRM_IOCTL_I915_GEM_SET_TILING, &t) == 0 && t.tiling_mode == I915_TILING_X &&
                t.stride == OBJ_STRIDE,
            "set tiling X");
    require(t.swizzle_mode == I915_BIT_6_SWIZZLE_NONE || t.swizzle_mode == I915_BIT_6_SWIZZLE_9_10,
            "swizzle none or 9/10, as Haswell's controller gives X tiles");
    report_held();
    return t.swizzle_mode;
}

/** Write each dword of the object its own offset through a GTT map. */
static void gtt_row(void)
{
    uint32_t *p, i;

    gtt_map = map(I915_MMAP_OFFSET_GTT);
    domain(I915_GEM_DOMAIN_GTT, I915_GEM_DOMAIN_GTT);
    p = (uint32_t *)gtt_map;
    for (i = 0; i < OBJ_BYTES / 4; i++) {
        p[i] = i * 4;
    }
    for (i = 0; i < OBJ_BYTES / 4; i++) {
        require(p[i] == i * 4, "read back through the GTT map");
    }
    report_held();
}

/** Where byte `v` of the linear surface lies in an X tiled object, as
 * Linux's selftest tiled_offset gives it. */
static uint32_t tiled(uint32_t v, uint32_t swizzle)
{
    uint32_t y = v / OBJ_STRIDE, x = v % OBJ_STRIDE;

    v = (y / 8) * OBJ_STRIDE * 8 + (y % 8) * 512 + (x / 512) * 4096 + x % 512;
    if (swizzle == I915_BIT_6_SWIZZLE_9_10) {
        v ^= ((v >> 3) ^ (v >> 4)) & 64;
    }
    return v;
}

/** Read every dword through a CPU map, after the GTT's writes are flushed,
 * where the tiling puts it. */
static void detile_row(uint32_t swizzle)
{
    const uint8_t *m;
    uint32_t v;

    cpu_map = map(I915_MMAP_OFFSET_WB);
    domain(I915_GEM_DOMAIN_CPU, 0);
    m = (const uint8_t *)cpu_map;
    for (v = 0; v < OBJ_BYTES; v += 4) {
        require(*(const uint32_t *)(m + tiled(v, swizzle)) == v,
                "each dword where X tiling puts it in memory");
    }
    report_held();
}

/** The object is idle: BUSY says so and WAIT returns at once. */
static void idle_row(void)
{
    struct drm_i915_gem_busy b;
    struct drm_i915_gem_wait w;

    zero(&b, sizeof(b));
    b.handle = bo_handle;
    require(call(DRM_IOCTL_I915_GEM_BUSY, &b) == 0 && b.busy == 0, "busy");
    zero(&w, sizeof(w));
    w.bo_handle = bo_handle;
    require(call(DRM_IOCTL_I915_GEM_WAIT, &w) == 0, "wait");
    report_held();
}

void probe_main(unsigned long *sp);
PROBE_SP_ENTRY;

/** Run the four reported rows and close every resource on exit. */
void probe_main(unsigned long *sp)
{
    const char *rows[] = {"object", "gtt", "detile", "idle"};
    uint32_t i, swizzle;

    report_begin(report_case((char **)(sp + 1) + sp[0] + 1, "machine.i915fake.gem"));
    for (i = 0; i < 4; i++) {
        report_plan(rows[i]);
    }
    swizzle = object_row();
    gtt_row();
    detile_row(swizzle);
    idle_row();
    report_end();
    probe_line("I915GEMPROBE OK", 0, 0);
    finish(0);
}
