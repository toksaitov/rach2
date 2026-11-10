/* i915gemprobe.c: the Intel driver's buffers on the fake chip in QEMU: an X
 * tiled object written through a GTT map reads back through a CPU map where
 * the tiling and the swizzle put each byte; with the argument `memory`, a
 * working set past the aperture, an aperture full of the bindings of batches
 * the GPU has not run, a userptr object and memory pressure that purges a
 * DONTNEED object and ends in ENOMEM. Runs under rach2.i915fake, as root
 * for the memory rows, whose sw_sync timeline is in debugfs. */
#include "probe.h"
#include "probe_report.h"
#include "../rach2/sys/drm/i915/i915_drm.h"

/* The object: four tile rows of X tiles, 2048 bytes a row, 64 KiB. */
#define OBJ_BYTES  65536u
#define OBJ_STRIDE 2048u

/* The memory rows: WS_OBJECTS objects of WS_BYTES, 320 MiB, past the
 * fake's aperture of 256 MiB; objects of PR_BYTES made until memory gives
 * out, at most PR_MAX; a userptr object over UP_BYTES of the probe's. */
#define WS_OBJECTS 20u
#define WS_BYTES   0x1000000u
#define WS_PAGES   (WS_BYTES / 4096u)
#define PR_BYTES   0x1000000u
#define PR_MAX     256u
#define UP_BYTES   0x10000u

/* The in-flight row: IF_OBJECTS objects of WS_BYTES, 272 MiB, each written
 * by a batch held behind a sw_sync point, which fill the aperture; and
 * MI_BATCH_BUFFER_END, which the fake does not read but a chip would. */
#define IF_OBJECTS 17u
#define BATCH_END  0x05000000u

static int gpu_fd = -1;
static uint32_t bo_handle;
static void *gtt_map, *cpu_map;
static uint32_t handles[WS_OBJECTS + PR_MAX + 2];
static uint32_t nhandles;
static void *ws_maps[WS_OBJECTS];
static void *user_map;

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

/** Close the memory rows' objects from the `from`th made on. */
static void close_from(uint32_t from)
{
    struct drm_gem_close c;

    while (nhandles > from) {
        zero(&c, sizeof(c));
        c.handle = handles[--nhandles];
        (void)syscall3(SYS_IOCTL, gpu_fd, (long)DRM_IOCTL_GEM_CLOSE, (long)&c);
    }
}

/** Unmap, close the objects and the node, then exit with `rc`. */
static void finish(int rc)
{
    struct drm_gem_close c;
    uint32_t i;

    for (i = 0; i < WS_OBJECTS; i++) {
        if (ws_maps[i]) {
            syscall2(SYS_MUNMAP, (long)ws_maps[i], WS_BYTES);
        }
    }
    if (user_map) {
        syscall2(SYS_MUNMAP, (long)user_map, UP_BYTES);
    }
    close_from(0);
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

/* ---- the memory rows ---- */

/** A new object of `bytes`, kept for `finish`: its handle. */
static uint32_t make(uint64_t bytes)
{
    struct drm_i915_gem_create c;

    zero(&c, sizeof(c));
    c.size = bytes;
    require(call(DRM_IOCTL_I915_GEM_CREATE, &c) == 0 && c.handle, "create");
    handles[nhandles++] = c.handle;
    return c.handle;
}

/** Map `bytes` of `handle` of the kind `flags`, shared: the address. */
static void *map_object(uint32_t handle, uint64_t bytes, uint32_t flags)
{
    struct drm_i915_gem_mmap_offset m;
    long address;

    zero(&m, sizeof(m));
    m.handle = handle;
    m.flags = flags;
    require(call(DRM_IOCTL_I915_GEM_MMAP_OFFSET, &m) == 0 && m.offset != 0, "mmap offset");
    address = probe_mmap(0, (long)bytes, 3, 1, gpu_fd, (long)m.offset);
    require((unsigned long)address < (unsigned long)-4095, "mmap");
    return (void *)address;
}

/** SET_DOMAIN of `handle` to `read` and `write`: the ioctl's result. */
static long set_domain(uint32_t handle, uint32_t read, uint32_t write)
{
    struct drm_i915_gem_set_domain d;

    zero(&d, sizeof(d));
    d.handle = handle;
    d.read_domains = read;
    d.write_domain = write;
    return call(DRM_IOCTL_I915_GEM_SET_DOMAIN, &d);
}

/** MADVISE of `handle` to `madv`: `retained`, or the negated errno. */
static long madvise(uint32_t handle, uint32_t madv)
{
    struct drm_i915_gem_madvise m;
    long r;

    zero(&m, sizeof(m));
    m.handle = handle;
    m.madv = madv;
    r = call(DRM_IOCTL_I915_GEM_MADVISE, &m);
    return r < 0 ? r : (long)m.retained;
}

/** The dword written at the start of page `page` of object `n`. */
static uint32_t stamp(uint32_t n, uint32_t page)
{
    return 0x5a000000u | (n << 16) | page;
}

/** GET_APERTURE: the fake's GTT of 2 GiB, part of it free. */
static void aperture_row(void)
{
    struct drm_i915_gem_get_aperture a;

    find_gpu();
    zero(&a, sizeof(a));
    require(call(DRM_IOCTL_I915_GEM_GET_APERTURE, &a) == 0 && a.aper_size == 0x80000000ull &&
                a.aper_available_size > 0 && a.aper_available_size < a.aper_size,
            "get aperture: 2 GiB, part of it available");
    report_held();
}

/**
 * 320 MiB of objects each written through its GTT map, a dword a page, past
 * the 256 MiB aperture, then read back through the maps, whose faults
 * rebind what eviction undid, and through PREAD from memory.
 */
static void working_set_row(void)
{
    uint32_t n, p, h[WS_OBJECTS], word;
    volatile uint32_t *m;

    for (n = 0; n < WS_OBJECTS; n++) {
        h[n] = make(WS_BYTES);
        ws_maps[n] = map_object(h[n], WS_BYTES, I915_MMAP_OFFSET_GTT);
        require(set_domain(h[n], I915_GEM_DOMAIN_GTT, I915_GEM_DOMAIN_GTT) == 0, "set domain");
        m = (volatile uint32_t *)ws_maps[n];
        for (p = 0; p < WS_PAGES; p++) {
            m[p * 1024u] = stamp(n, p);
        }
    }
    for (n = 0; n < WS_OBJECTS; n++) {
        m = (volatile uint32_t *)ws_maps[n];
        for (p = 0; p < WS_PAGES; p++) {
            require(m[p * 1024u] == stamp(n, p), "each page read back through its GTT map");
        }
    }
    for (n = 0; n < WS_OBJECTS; n++) {
        for (p = 0; p < WS_PAGES; p += 7) {
            struct drm_i915_gem_pread r;

            zero(&r, sizeof(r));
            word = 0;
            r.handle = h[n];
            r.offset = (uint64_t)p * 4096u;
            r.size = 4;
            r.data_ptr = (uintptr_t)&word;
            require(call(DRM_IOCTL_I915_GEM_PREAD, &r) == 0 && word == stamp(n, p),
                    "each page in memory, read by PREAD");
        }
        syscall2(SYS_MUNMAP, (long)ws_maps[n], WS_BYTES);
        ws_maps[n] = 0;
    }
    close_from(0);
    report_held();
}

/** The state letter of /proc/PID/stat of `pid`, or 0 when unread. */
static char proc_state(long pid)
{
    char path[32] = "/proc/", text[256];
    char digits[24];
    long n;
    int fd, i = 6, k = 0;

    do {
        digits[k++] = (char)('0' + pid % 10);
        pid /= 10;
    } while (pid > 0);
    while (k > 0) {
        path[i++] = digits[--k];
    }
    for (n = 0; "/stat"[n]; n++) {
        path[i++] = "/stat"[n];
    }
    path[i] = '\0';
    fd = (int)syscall3(SYS_OPEN, (long)path, 0, 0);
    if (fd < 0) {
        return 0;
    }
    n = syscall3(SYS_READ, fd, (long)text, (long)sizeof(text) - 1);
    syscall1(SYS_CLOSE, fd);
    /* The state follows the command's closing parenthesis and a space. */
    for (i = (int)n - 1; i > 0 && text[i] != ')'; i--) {
    }
    return i > 0 && i + 2 < (int)n ? text[i + 2] : 0;
}

/** A sync file's status from SYNC_IOC_FILE_INFO: 1 signalled, 0 active. */
static int fence_status(int fd)
{
    struct sync_file_info info;

    zero(&info, sizeof(info));
    if (syscall3(SYS_IOCTL, fd, SYNC_IOC_FILE_INFO, (long)&info) != 0) {
        return -1;
    }
    return info.status;
}

/** BUSY of `handle`: its answer, or ~0 on failure. */
static uint32_t busy(uint32_t handle)
{
    struct drm_i915_gem_busy b;

    zero(&b, sizeof(b));
    b.handle = handle;
    return call(DRM_IOCTL_I915_GEM_BUSY, &b) == 0 ? b.busy : ~0u;
}

/**
 * 272 MiB of objects, each written by a batch on the render ring that
 * waits for one point of a sw_sync timeline, fill the 256 MiB aperture with
 * bindings the GPU may still use. A GTT map's fault then finds no idle
 * binding to evict, sleeps uninterruptibly until the GPU is idle, as
 * Linux's eviction does, and binds once a child, seeing the probe asleep,
 * signals the point and the batches retire.
 */
static void in_flight_row(void)
{
    struct sw_sync_create_fence_data pt;
    struct drm_i915_gem_exec_object2 objs[2];
    struct drm_i915_gem_execbuffer2 eb;
    struct drm_i915_gem_pwrite w;
    uint32_t n, data = 0, batch, end = BATCH_END, step = 1;
    volatile uint32_t *m;
    long parent = syscall0(SYS_GETPID), child;
    int tl, status = -1;
    char state;

    tl = (int)syscall3(SYS_OPEN, (long)"/sys/kernel/debug/sync/sw_sync", 2, 0);
    require(tl >= 0, "in-flight: a sw_sync timeline");
    zero(&pt, sizeof(pt));
    pt.value = 1;
    pt.fence = -1;
    require(syscall3(SYS_IOCTL, tl, SW_SYNC_IOC_CREATE_FENCE, (long)&pt) == 0 && pt.fence >= 0,
            "in-flight: a sw_sync point");
    for (n = 0; n < IF_OBJECTS; n++) {
        data = make(WS_BYTES);
        batch = make(4096);
        zero(&w, sizeof(w));
        w.handle = batch;
        w.size = sizeof(end);
        w.data_ptr = (uintptr_t)&end;
        require(call(DRM_IOCTL_I915_GEM_PWRITE, &w) == 0, "in-flight: the batch written");
        zero(objs, sizeof(objs));
        objs[0].handle = data;
        objs[0].flags = EXEC_OBJECT_WRITE;
        objs[1].handle = batch;
        zero(&eb, sizeof(eb));
        eb.buffers_ptr = (uintptr_t)objs;
        eb.buffer_count = 2;
        eb.flags = I915_EXEC_RENDER | I915_EXEC_FENCE_IN;
        eb.rsvd2 = (uint64_t)pt.fence;
        require(call(DRM_IOCTL_I915_GEM_EXECBUFFER2_WR, &eb) == 0,
                "in-flight: a batch held behind the point");
    }
    require(busy(data) != 0 && busy(handles[0]) != 0, "in-flight: the objects busy");
    ws_maps[0] = map_object(make(WS_BYTES), WS_BYTES, I915_MMAP_OFFSET_GTT);
    child = syscall0(SYS_FORK);
    require(child >= 0, "in-flight: fork");
    if (child == 0) {
        while ((state = proc_state(parent)) == 'R') {
            syscall0(SYS_SCHED_YIELD);
        }
        syscall3(SYS_IOCTL, tl, SW_SYNC_IOC_INC, (long)&step);
        syscall1(SYS_EXIT, state == 'D' ? 0 : 1);
    }
    m = (volatile uint32_t *)ws_maps[0];
    m[0] = stamp(IF_OBJECTS, 0);
    require(m[0] == stamp(IF_OBJECTS, 0), "in-flight: the fault bound the object");
    require(syscall4(SYS_WAIT4, child, (long)&status, 0, 0) == child && status == 0,
            "in-flight: the fault slept, uninterruptibly, until the point signalled");
    require(fence_status((int)pt.fence) == 1 && busy(data) == 0 && busy(handles[0]) == 0,
            "in-flight: the batches retired");
    syscall2(SYS_MUNMAP, (long)ws_maps[0], WS_BYTES);
    ws_maps[0] = 0;
    syscall1(SYS_CLOSE, (long)pt.fence);
    syscall1(SYS_CLOSE, tl);
    close_from(0);
    report_held();
}

/** A userptr object over 64 KiB of the probe's memory: UNSYNCHRONIZED
 * refused, its pages pinned by SET_DOMAIN, PREAD refused, and EFAULT once
 * the memory is unmapped. */
static void userptr_row(void)
{
    struct drm_i915_gem_userptr u;
    struct drm_i915_gem_pread r;
    long a = probe_mmap(0, UP_BYTES, 3, 0x22, -1, 0);
    uint32_t i, buf[4];

    require((unsigned long)a < (unsigned long)-4095, "mmap anonymous");
    user_map = (void *)a;
    for (i = 0; i < UP_BYTES / 4; i++) {
        ((uint32_t *)user_map)[i] = i;
    }
    zero(&u, sizeof(u));
    u.user_ptr = (uintptr_t)a;
    u.user_size = UP_BYTES;
    u.flags = I915_USERPTR_UNSYNCHRONIZED;
    require(call(DRM_IOCTL_I915_GEM_USERPTR, &u) == -19, "userptr: unsynchronized, ENODEV");
    u.flags = I915_USERPTR_PROBE;
    require(call(DRM_IOCTL_I915_GEM_USERPTR, &u) == 0 && u.handle, "userptr over the probe's");
    handles[nhandles++] = u.handle;
    require(set_domain(u.handle, I915_GEM_DOMAIN_CPU, I915_GEM_DOMAIN_CPU) == 0,
            "userptr: its pages pinned by set domain");
    zero(&r, sizeof(r));
    r.handle = u.handle;
    r.size = sizeof(buf);
    r.data_ptr = (uintptr_t)buf;
    require(call(DRM_IOCTL_I915_GEM_PREAD, &r) == -22, "userptr: pread, EINVAL");
    syscall2(SYS_MUNMAP, (long)user_map, UP_BYTES);
    user_map = 0;
    require(set_domain(u.handle, I915_GEM_DOMAIN_CPU, 0) == -14, "userptr: unmapped, EFAULT");
    close_from(0);
    report_held();
}

/** Objects of 16 MiB given pages until memory gives out with ENOMEM, a
 * DONTNEED object of 16 MiB purged on the way. */
static void pressure_row(void)
{
    uint32_t purge = make(PR_BYTES), made = 0;
    long r;

    require(set_domain(purge, I915_GEM_DOMAIN_CPU, I915_GEM_DOMAIN_CPU) == 0,
            "pressure: an object with pages");
    require(madvise(purge, I915_MADV_DONTNEED) == 1, "pressure: DONTNEED, retained");
    for (;;) {
        struct drm_i915_gem_create c;

        require(made < PR_MAX, "pressure: memory gives out within 4 GiB of objects");
        zero(&c, sizeof(c));
        c.size = PR_BYTES;
        r = call(DRM_IOCTL_I915_GEM_CREATE, &c);
        if (r == 0) {
            handles[nhandles++] = c.handle;
            r = set_domain(c.handle, I915_GEM_DOMAIN_CPU, I915_GEM_DOMAIN_CPU);
        }
        if (r != 0) {
            break;
        }
        made++;
    }
    require(r == -12 && made > 0, "pressure: past memory, ENOMEM");
    require(madvise(purge, I915_MADV_WILLNEED) == 0, "pressure: the DONTNEED object purged");
    close_from(0);
    report_held();
}

/** The rows of `memory`, under the case machine.i915fake.memory. */
static void memory_rows(char **envp)
{
    const char *rows[] = {"aperture", "working-set", "in-flight", "userptr", "pressure"};
    uint32_t i;

    report_begin(report_case(envp, "machine.i915fake.memory"));
    for (i = 0; i < 5; i++) {
        report_plan(rows[i]);
    }
    aperture_row();
    working_set_row();
    in_flight_row();
    userptr_row();
    pressure_row();
    report_end();
    probe_line("I915GEMPROBE OK", 0, 0);
    finish(0);
}

void probe_main(unsigned long *sp);
PROBE_SP_ENTRY;

/** Run the four reported rows, or with `memory` the memory rows, and close
 * every resource on exit. */
void probe_main(unsigned long *sp)
{
    const char *rows[] = {"object", "gtt", "detile", "idle"};
    char **argv = (char **)(sp + 1);
    uint32_t i, swizzle;

    if (sp[0] > 1 && argv[1][0] == 'm' && argv[1][1] == 'e' && argv[1][2] == 'm') {
        memory_rows(argv + sp[0] + 1);
    }
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
