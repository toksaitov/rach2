/* gpucompatprobe.c: i386 GPU requests on i915's fault backend and virgl,
 * including the families whose fixed-width arguments need no translation. */
#include "probe.h"
#include "probe_report.h"
#include "../rach2/sys/drm/i915/i915_drm.h"
#include "../rach2/sys/drm/virtio_gpu.h"

#define SYS_MEMFD_CREATE 356
#define PAGE             4096u

static int gpu = -1, mem = -1, udma = -1, buffer = -1, fence = -1;
static uint32_t bo, syncobj;
static void *mapping, *high;
static int compat; /* the i386 probe runs on the x86-64 kernel */

/* Compile against i386's layouts, including GETPARAM's eight bytes. */
#define SIZE(t, n) typedef char size_##t[sizeof(struct t) == (n) ? 1 : -1]
SIZE(drm_i915_getparam, 8);
SIZE(drm_i915_gem_pwrite, 32);
SIZE(drm_virtgpu_getparam, 16);
SIZE(drm_virtgpu_3d_transfer, 44);
SIZE(drm_syncobj_timeline_array, 24);
SIZE(sync_file_info, 56);
SIZE(udmabuf_create, 24);

/** Clear `n` bytes of an argument. */
static void zero(void *p, size_t n)
{
    unsigned char *s = p;
    while (n--) {
        *s++ = 0;
    }
}

/** Make an ioctl and return its result, a negated errno on failure. */
static long call(int fd, unsigned long cmd, void *arg)
{
    return syscall3(SYS_IOCTL, fd, (long)cmd, (long)arg);
}

/** Release every resource made so far, including after a failed row. */
static void finish(int status)
{
    struct drm_gem_close c;
    struct drm_syncobj_destroy d;
    int fds[6], i;

    if (mapping) {
        syscall2(SYS_MUNMAP, (long)mapping, PAGE);
    }
    if (high) {
        syscall2(SYS_MUNMAP, (long)high, 2 * PAGE);
    }
    if (syncobj) {
        zero(&d, sizeof(d));
        d.handle = syncobj;
        call(gpu, DRM_IOCTL_SYNCOBJ_DESTROY, &d);
    }
    if (bo) {
        zero(&c, sizeof(c));
        c.handle = bo;
        call(gpu, DRM_IOCTL_GEM_CLOSE, &c);
    }
    fds[0] = fence;
    fds[1] = buffer;
    fds[2] = udma;
    fds[3] = mem;
    fds[4] = gpu;
    fds[5] = -1;
    for (i = 0; i < 5; i++) {
        if (fds[i] >= 0) {
            syscall1(SYS_CLOSE, fds[i]);
        }
    }
    syscall1(SYS_EXIT, status);
    __builtin_unreachable();
}

/** Fail the current assertion and clean up unless `ok`. */
static void require(int ok, const char *what)
{
    if (!ok) {
        probe_line("GPUCOMPATPROBE FAIL: ", what, 0);
        report_stop(what);
        finish(1);
    }
}

/** Whether the two terminated strings agree. */
static int same(const char *a, const char *b)
{
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

/** Open a render node by driver name, independent of enumeration order. */
static void open_gpu(const char *driver)
{
    char path[] = "/dev/dri/renderD128", name[32];
    struct drm_version v;
    int i;

    for (i = 128; i < 144; i++) {
        path[16] = (char)('0' + i / 100);
        path[17] = (char)('0' + i / 10 % 10);
        path[18] = (char)('0' + i % 10);
        gpu = (int)syscall3(SYS_OPEN, (long)path, 2, 0);
        if (gpu < 0) {
            continue;
        }
        zero(&v, sizeof(v));
        zero(name, sizeof(name));
        v.name = name;
        v.name_len = sizeof(name) - 1;
        if (call(gpu, DRM_IOCTL_VERSION, &v) == 0 && same(name, driver)) {
            report_held();
            return;
        }
        syscall1(SYS_CLOSE, gpu);
        gpu = -1;
    }
    require(0, "no render node for the requested driver");
}

/** Compat GETPARAM reads exactly eight bytes and writes exactly one int
 * through an address with bit 31 set, even with altered size and direction
 * bits. The request ends at an inaccessible page and is then read-only;
 * a native i686 kernel instead follows the encoded copies, as Linux does. */
static void intel_params(void)
{
    struct drm_i915_getparam *p;
    volatile int *value;
    static int answer;
    long at;

    at = probe_mmap(0x90000000UL, 2 * PAGE, 3, 0x22, -1, 0);
    require((unsigned long)at >= 0x80000000UL && (unsigned long)at < 0xffffe000UL,
            "mapping with a high 32-bit address");
    high = (void *)at;
    require(syscall3(SYS_MPROTECT, at + PAGE, PAGE, 0) == 0, "request's guard page");
    p = (struct drm_i915_getparam *)(at + PAGE - sizeof(*p));
    value = (volatile int *)(at + 32);
    value[-1] = value[1] = 0x12345678;
    *value = -1;
    p->param = I915_PARAM_CHIPSET_ID;
    p->value = (int *)value;
    require(call(gpu, DRM_IOCTL_I915_GETPARAM, p) == 0 && *value == 0x0a16 &&
                value[-1] == 0x12345678 && value[1] == 0x12345678,
            "GETPARAM's high pointer and guarded int");
    *value = -1;
    if (compat) {
        require(call(gpu, DRM_IO(DRM_COMMAND_BASE + DRM_I915_GETPARAM), p) == 0 && *value == 0x0a16,
                "compat GETPARAM ignores the encoded direction and size");
    } else {
        require(call(gpu, DRM_IO(DRM_COMMAND_BASE + DRM_I915_GETPARAM), p) == -22,
                "native GETPARAM with no input has an invalid parameter");
    }
    require(call(gpu, DRM_IOCTL_I915_GETPARAM, (char *)p + 4) == -14,
            "GETPARAM's partial request returns EFAULT");
    p->value = (int *)(at + PAGE);
    require(call(gpu, DRM_IOCTL_I915_GETPARAM, p) == -14, "GETPARAM's bad value pointer");
    p->param = -1;
    p->value = (int *)value;
    *value = -1;
    require(call(gpu, DRM_IOCTL_I915_GETPARAM, p) == -22 && *value == -1,
            "GETPARAM's unknown signed parameter");
    p->param = I915_PARAM_HAS_GEM;
    p->value = &answer;
    require(syscall3(SYS_MPROTECT, at, PAGE, 1) == 0, "read-only request page");
    require(call(gpu, DRM_IOCTL_I915_GETPARAM, p) == (compat ? 0 : -14) && answer == 1,
            "only native GETPARAM writes back its request");
    report_held();
}

/** GEM's fixed-width pointers carry an i386 buffer for PWRITE and PREAD;
 * context GETPARAM carries its scalar in the same 64-bit field. */
static void intel_buffer(void)
{
    struct drm_i915_gem_create c;
    struct drm_i915_gem_pwrite w;
    struct drm_i915_gem_pread r;
    struct drm_i915_gem_context_param p;
    unsigned char src[64], dst[64];
    unsigned i;

    zero(&c, sizeof(c));
    c.size = PAGE;
    require(call(gpu, DRM_IOCTL_I915_GEM_CREATE, &c) == 0 && c.handle, "GEM_CREATE");
    bo = c.handle;
    zero(&w, sizeof(w));
    zero(&r, sizeof(r));
    for (i = 0; i < sizeof(src); i++) {
        src[i] = (unsigned char)(i * 3 + 17);
        dst[i] = 0;
    }
    w.handle = r.handle = bo;
    w.offset = r.offset = 17;
    w.size = r.size = sizeof(src);
    w.data_ptr = (uintptr_t)src;
    r.data_ptr = (uintptr_t)dst;
    require(call(gpu, DRM_IOCTL_I915_GEM_PWRITE, &w) == 0, "GEM_PWRITE's 64-bit pointer");
    require(call(gpu, DRM_IOCTL_I915_GEM_PREAD, &r) == 0, "GEM_PREAD's 64-bit pointer");
    for (i = 0; i < sizeof(src); i++) {
        require(dst[i] == src[i], "GEM readback");
    }
    zero(&p, sizeof(p));
    p.param = I915_CONTEXT_PARAM_GTT_SIZE;
    require(call(gpu, DRM_IOCTL_I915_GEM_CONTEXT_GETPARAM, &p) == 0 && p.value != 0,
            "context GETPARAM's 64-bit value");
    report_held();
}

/** Virgl GETPARAM's fixed-width pointer and a texture upload/readback,
 * including TRANSFER_FROM_HOST, whose number is i915 GETPARAM's. */
static void virtio_rows(void)
{
    struct drm_virtgpu_getparam p;
    struct drm_virtgpu_resource_create c;
    struct drm_virtgpu_map m;
    struct drm_virtgpu_3d_transfer t;
    struct drm_virtgpu_3d_wait w;
    int value = -1;
    unsigned char *bytes;
    unsigned i;
    long at;

    zero(&p, sizeof(p));
    p.param = VIRTGPU_PARAM_3D_FEATURES;
    p.value = (uintptr_t)&value;
    require(call(gpu, DRM_IOCTL_VIRTGPU_GETPARAM, &p) == 0 && value == 1,
            "virtio GETPARAM's 64-bit pointer");
    report_held();
    zero(&c, sizeof(c));
    c.target = 2;
    c.format = 1;
    c.bind = 2;
    c.width = c.height = 4;
    c.depth = c.array_size = 1;
    c.size = PAGE;
    require(call(gpu, DRM_IOCTL_VIRTGPU_RESOURCE_CREATE, &c) == 0 && c.bo_handle,
            "virtio RESOURCE_CREATE");
    bo = c.bo_handle;
    zero(&m, sizeof(m));
    m.handle = bo;
    require(call(gpu, DRM_IOCTL_VIRTGPU_MAP, &m) == 0, "virtio MAP");
    /* The byte offset can exceed 4 GiB; mmap2's page offset still fits. */
    at = syscall6(SYS_MMAP2, 0, PAGE, 3, 1, gpu, (long)(m.offset >> 12));
    require((unsigned long)at < (unsigned long)-4095, "virtio mmap");
    mapping = (void *)at;
    bytes = mapping;
    zero(&t, sizeof(t));
    t.bo_handle = bo;
    t.box.w = t.box.h = 4;
    t.box.d = 1;
    w.handle = bo;
    w.flags = 0;
    for (i = 0; i < 64; i++) {
        bytes[i] = (unsigned char)(i * 3 + 17);
    }
    require(call(gpu, DRM_IOCTL_VIRTGPU_TRANSFER_TO_HOST, &t) == 0 &&
                call(gpu, DRM_IOCTL_VIRTGPU_WAIT, &w) == 0,
            "virtio upload");
    zero(bytes, 64);
    require(call(gpu, DRM_IOCTL_VIRTGPU_TRANSFER_FROM_HOST, &t) == 0 &&
                call(gpu, DRM_IOCTL_VIRTGPU_WAIT, &w) == 0,
            "virtio readback");
    for (i = 0; i < 64; i++) {
        require(bytes[i] == (unsigned char)(i * 3 + 17), "virtio texture bytes");
    }
    report_held();
}

/** A timeline signal and query follow 64-bit fields holding i386 pointers. */
static void syncobj_row(void)
{
    struct drm_syncobj_create c;
    struct drm_syncobj_timeline_array a;
    uint64_t point = 7;

    zero(&c, sizeof(c));
    require(call(gpu, DRM_IOCTL_SYNCOBJ_CREATE, &c) == 0 && c.handle, "syncobj create");
    syncobj = c.handle;
    zero(&a, sizeof(a));
    a.handles = (uintptr_t)&syncobj;
    a.points = (uintptr_t)&point;
    a.count_handles = 1;
    require(call(gpu, DRM_IOCTL_SYNCOBJ_TIMELINE_SIGNAL, &a) == 0, "timeline signal");
    point = 0;
    require(call(gpu, DRM_IOCTL_SYNCOBJ_QUERY, &a) == 0 && point == 7, "timeline query");
    report_held();
}

/** The last successful SET_NAME remains visible after rejected names. */
static void name_row(void)
{
    char path[64], number[12], text[256];
    const char *p = "/proc/self/fdinfo/", *digits;
    const char *wanted = "name:\tgpucompat-b\n";
    unsigned n = 0, i, j;
    long fd, bytes;
    int found = 0;

    while (*p) {
        path[n++] = *p++;
    }
    digits = report_decimal(number, buffer);
    while (*digits) {
        path[n++] = *digits++;
    }
    path[n] = 0;
    fd = syscall3(SYS_OPEN, (long)path, 0, 0);
    require(fd >= 0, "open dma-buf fdinfo");
    bytes = syscall3(SYS_READ, fd, (long)text, sizeof(text));
    syscall1(SYS_CLOSE, fd);
    require(bytes > 0, "read dma-buf fdinfo");
    for (i = 0; i < (unsigned)bytes; i++) {
        for (j = 0; wanted[j] && i + j < (unsigned)bytes && text[i + j] == wanted[j]; j++)
            ;
        if (!wanted[j]) {
            found = 1;
        }
    }
    require(found, "dma-buf name survived rejected names");
}

/** udmabuf exports a sealed memfd; dma-buf takes both name encodings and
 * CPU sync, and exports a sync file whose fixed-width info is readable. */
static void sharing_rows(void)
{
    struct udmabuf_create c;
    struct dma_buf_sync s;
    struct dma_buf_export_sync_file e;
    struct sync_file_info info;
    struct sync_fence_info row;
    char long_name[DMA_BUF_NAME_LEN];
    unsigned i;

    mem = (int)syscall2(SYS_MEMFD_CREATE, (long)"gpucompatprobe", 3);
    require(mem >= 0 && probe_ftruncate64(mem, PAGE) == 0 && syscall3(SYS_FCNTL, mem, 1033, 2) == 0,
            "sealed memfd");
    udma = (int)syscall3(SYS_OPEN, (long)"/dev/udmabuf", 2, 0);
    require(udma >= 0, "open udmabuf");
    zero(&c, sizeof(c));
    c.memfd = (uint32_t)mem;
    c.flags = UDMABUF_FLAGS_CLOEXEC;
    c.size = PAGE;
    buffer = (int)call(udma, UDMABUF_CREATE, &c);
    require(buffer >= 0, "UDMABUF_CREATE");
    report_held();
    require(call(buffer, DMA_BUF_SET_NAME_A, "gpucompat-a") == 0 &&
                call(buffer, DMA_BUF_SET_NAME_B, "gpucompat-b") == 0,
            "both dma-buf names");
    for (i = 0; i < sizeof(long_name); i++) {
        long_name[i] = 'x';
    }
    require(call(buffer, DMA_BUF_SET_NAME_A, long_name) == -36 &&
                call(buffer, DMA_BUF_SET_NAME_B, 0) == -14,
            "invalid dma-buf names");
    name_row();
    s.flags = DMA_BUF_SYNC_START | DMA_BUF_SYNC_RW;
    require(call(buffer, DMA_BUF_IOCTL_SYNC, &s) == 0, "dma-buf CPU sync start");
    s.flags = DMA_BUF_SYNC_END | DMA_BUF_SYNC_RW;
    require(call(buffer, DMA_BUF_IOCTL_SYNC, &s) == 0, "dma-buf CPU sync end");
    zero(&e, sizeof(e));
    e.flags = DMA_BUF_SYNC_READ;
    require(call(buffer, DMA_BUF_IOCTL_EXPORT_SYNC_FILE, &e) == 0 && e.fd >= 0,
            "dma-buf export sync file");
    fence = e.fd;
    report_held();
    zero(&info, sizeof(info));
    require(call(fence, SYNC_IOC_FILE_INFO, &info) == 0 && info.status == 1 && info.num_fences == 1,
            "empty sync file has a signalled stub");
    zero(&row, sizeof(row));
    info.sync_fence_info = (uintptr_t)&row;
    require(call(fence, SYNC_IOC_FILE_INFO, &info) == 0 && row.status == 1,
            "sync-file info's 64-bit pointer");
    report_held();
}

void probe_main(unsigned long *sp);
PROBE_SP_ENTRY;

/** Run the requested driver's rows and the shared families, then exit. */
void probe_main(unsigned long *sp)
{
    char **argv = (char **)(sp + 1);
    int intel = sp[0] > 1 && same(argv[1], "i915");
    const char *driver = intel ? "i915" : "virtio_gpu";
    char uts[6][65];

    report_begin(report_case(argv + sp[0] + 1, "guest.gpucompat"));
    report_plan("open");
    report_plan("params");
    report_plan("buffer");
    report_plan("syncobj");
    report_plan("udmabuf");
    report_plan("dmabuf");
    report_plan("sync-file");
    require(syscall1(SYS_UNAME, (long)uts) == 0, "kernel architecture");
    compat = same(uts[4], "x86_64");
    open_gpu(driver);
    if (intel) {
        intel_params();
        intel_buffer();
    } else {
        virtio_rows();
    }
    syncobj_row();
    sharing_rows();
    report_end();
    probe_line("GPUCOMPATPROBE OK ", driver, 0);
    finish(0);
}
