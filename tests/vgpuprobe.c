/* vgpuprobe.c: virgl capability sets, transfers, submissions and device loss.
 * Runs as root on the virgl QEMU profile. The reset row unplugs its GPU. */
#include "probe.h"
#include "probe_report.h"
#include "../rach2/sys/drm/virtio_gpu.h"

static int gpu_fd = -1, other_fd = -1, fence_fd = -1, reset_fd = -1;
static void *mapping;
static uint32_t bo_handle;
static int card_index;

/** Clear an ioctl argument. */
static void zero(void *p, size_t n)
{
    unsigned char *s = p;
    while (n--) {
        *s++ = 0;
    }
}

/** The ioctl result, as a negated errno on failure. */
static long call(int fd, unsigned long op, void *arg)
{
    return syscall3(SYS_IOCTL, fd, (long)op, (long)arg);
}

/** Release every descriptor and mapping, then exit. */
static void finish(int rc)
{
    int fds[4], i;
    struct drm_gem_close c;

    if (mapping) {
        syscall2(SYS_MUNMAP, (long)mapping, 4096);
    }
    if (bo_handle && gpu_fd >= 0) {
        zero(&c, sizeof(c));
        c.handle = bo_handle;
        (void)call(gpu_fd, DRM_IOCTL_GEM_CLOSE, &c);
    }
    fds[0] = gpu_fd;
    fds[1] = other_fd;
    fds[2] = fence_fd;
    fds[3] = reset_fd;
    for (i = 0; i < 4; i++) {
        if (fds[i] >= 0) {
            syscall1(SYS_CLOSE, fds[i]);
        }
    }
    syscall1(SYS_EXIT, rc);
    __builtin_unreachable();
}

/** Fail the active report row, clean up and stop. */
static void require(int ok, const char *what)
{
    if (!ok) {
        probe_line("VGPUPROBE FAIL: ", what, 0);
        report_stop(what);
        finish(1);
    }
}

/** Append a decimal number to a path's prefix. */
static char *number(char *p, uint32_t n)
{
    char digits[11];
    uint32_t i = 0;
    do {
        digits[i++] = (char)('0' + n % 10);
        n /= 10;
    } while (n);
    while (i) {
        *p++ = digits[--i];
    }
    *p = 0;
    return p;
}

/** Form a render node's path. */
static void render_path(char *p, uint32_t minor)
{
    const char *s = "/dev/dri/renderD";
    while (*s) {
        *p++ = *s++;
    }
    (void)number(p, minor);
}

/** Find virtio-gpu by its name and 3D parameter, independent of node order. */
static void find_gpu(void)
{
    char path[64], name[32];
    struct drm_version v;
    struct drm_virtgpu_getparam p;
    int32_t value;
    int i;

    for (i = 0; i < DRM_MAX_DEVICES; i++) {
        render_path(path, (uint32_t)(128 + i));
        gpu_fd = (int)syscall3(SYS_OPEN, (long)path, 2, 0);
        if (gpu_fd < 0) {
            continue;
        }
        zero(&v, sizeof(v));
        zero(name, sizeof(name));
        v.name = name;
        v.name_len = sizeof(name) - 1;
        if (call(gpu_fd, DRM_IOCTL_VERSION, &v) == 0 && v.name_len == 10 && name[0] == 'v' &&
            name[1] == 'i' && name[9] == 'u') {
            p.param = VIRTGPU_PARAM_3D_FEATURES;
            p.value = (uint64_t)(uintptr_t)&value;
            if (call(gpu_fd, DRM_IOCTL_VIRTGPU_GETPARAM, &p) == 0 && value == 1) {
                card_index = i;
                other_fd = (int)syscall3(SYS_OPEN, (long)path, 2, 0);
                return;
            }
        }
        syscall1(SYS_CLOSE, gpu_fd);
        gpu_fd = -1;
    }
    require(0, "no virgl render node");
}

/** Check both capability sets with short reads and Linux's refusal rules. */
static void params_row(void)
{
    struct drm_virtgpu_getparam p;
    struct drm_virtgpu_get_caps c;
    int32_t value;
    uint8_t caps[16];
    uint32_t id;

    find_gpu();
    p.param = VIRTGPU_PARAM_CAPSET_QUERY_FIX;
    p.value = (uint64_t)(uintptr_t)&value;
    require(call(gpu_fd, DRM_IOCTL_VIRTGPU_GETPARAM, &p) == 0 && value == 1, "capset query fix");
    p.param = VIRTGPU_PARAM_SUPPORTED_CAPSET_IDs;
    require(call(gpu_fd, DRM_IOCTL_VIRTGPU_GETPARAM, &p) == 0 && (value & 6) == 6,
            "virgl capset ids");
    for (id = 1; id <= 2; id++) {
        zero(&c, sizeof(c));
        c.cap_set_id = id;
        c.addr = (uint64_t)(uintptr_t)caps;
        c.size = sizeof(caps);
        require(call(gpu_fd, DRM_IOCTL_VIRTGPU_GET_CAPS, &c) == 0, "read virgl capability bytes");
        c.size = 0;
        require(call(gpu_fd, DRM_IOCTL_VIRTGPU_GET_CAPS, &c) == -22, "zero-sized capset refused");
    }
    report_held();
}

/** Create a second independent context with an empty virgl submission. */
static void context_row(void)
{
    struct drm_virtgpu_execbuffer a;
    zero(&a, sizeof(a));
    require(other_fd >= 0 && call(other_fd, DRM_IOCTL_VIRTGPU_EXECBUFFER, &a) == 0,
            "second open's context");
    syscall1(SYS_CLOSE, other_fd);
    other_fd = -1;
    report_held();
}

/** Make a 4 by 4 BGRA texture over one page of backing. */
static void resource_row(void)
{
    struct drm_virtgpu_resource_create a;
    struct drm_virtgpu_resource_info info;
    struct drm_virtgpu_map m;
    long address;

    zero(&a, sizeof(a));
    a.target = 2;
    a.format = 1;
    a.bind = 2;
    a.width = a.height = 4;
    a.depth = a.array_size = 1;
    a.size = 4096;
    require(call(gpu_fd, DRM_IOCTL_VIRTGPU_RESOURCE_CREATE, &a) == 0 && a.bo_handle && a.res_handle,
            "3D texture creation");
    bo_handle = a.bo_handle;
    zero(&info, sizeof(info));
    info.bo_handle = bo_handle;
    require(call(gpu_fd, DRM_IOCTL_VIRTGPU_RESOURCE_INFO, &info) == 0 && info.size == 4096 &&
                info.res_handle == a.res_handle,
            "resource info");
    zero(&m, sizeof(m));
    m.handle = bo_handle;
    require(call(gpu_fd, DRM_IOCTL_VIRTGPU_MAP, &m) == 0, "resource mmap offset");
#ifdef __x86_64__
    address = syscall6(SYS_MMAP, 0, 4096, 3, 1, gpu_fd, (long)m.offset);
#else
    address = syscall6(SYS_MMAP2, 0, 4096, 3, 1, gpu_fd, (long)(m.offset >> 12));
#endif
    require((unsigned long)address < (unsigned long)-4095, "resource mapping");
    mapping = (void *)address;
    report_held();
}

/** Wait for the texture's queued work. */
static void wait_texture(void)
{
    struct drm_virtgpu_3d_wait a;
    a.handle = bo_handle;
    a.flags = 0;
    require(call(gpu_fd, DRM_IOCTL_VIRTGPU_WAIT, &a) == 0, "wait for texture");
}

/** Upload and read back a texture, verifying all four channels. */
static void transfer_row(void)
{
    struct drm_virtgpu_3d_transfer a;
    uint8_t *bytes = mapping;
    uint32_t i;

    zero(&a, sizeof(a));
    a.bo_handle = bo_handle;
    a.box.w = a.box.h = 4;
    a.box.d = 1;
    for (i = 0; i < 64; i++) {
        bytes[i] = (uint8_t)(i * 3 + 17);
    }
    require(call(gpu_fd, DRM_IOCTL_VIRTGPU_TRANSFER_TO_HOST, &a) == 0, "upload texture");
    wait_texture();
    for (i = 0; i < 64; i++) {
        bytes[i] = 0;
    }
    require(call(gpu_fd, DRM_IOCTL_VIRTGPU_TRANSFER_FROM_HOST, &a) == 0, "read back texture");
    wait_texture();
    for (i = 0; i < 64; i++) {
        require(bytes[i] == (uint8_t)(i * 3 + 17), "texture bytes equal upload");
    }
    a.stride = 16;
    require(call(gpu_fd, DRM_IOCTL_VIRTGPU_TRANSFER_TO_HOST, &a) == -22,
            "stride without a blob refused");
    report_held();
}

/** Queue a submission with an out-fence, with its texture named as a BO. */
static void submit_row(void)
{
    struct drm_virtgpu_execbuffer a;
    zero(&a, sizeof(a));
    a.bo_handles = (uint64_t)(uintptr_t)&bo_handle;
    a.num_bo_handles = 1;
    a.flags = VIRTGPU_EXECBUF_FENCE_FD_OUT;
    require(call(gpu_fd, DRM_IOCTL_VIRTGPU_EXECBUFFER, &a) == 0 && a.fence_fd >= 0,
            "submit with out-fence");
    fence_fd = a.fence_fd;
    report_held();
}

/** Check the real sync file and use it as the next submission's dependency. */
static void fence_row(void)
{
    struct sync_file_info info;
    struct drm_virtgpu_execbuffer a;
    zero(&info, sizeof(info));
    require(call(fence_fd, SYNC_IOC_FILE_INFO, &info) == 0 && info.num_fences == 1,
            "out-fence is a sync file");
    zero(&a, sizeof(a));
    a.flags = VIRTGPU_EXECBUF_FENCE_FD_IN;
    a.fence_fd = fence_fd;
    a.bo_handles = (uint64_t)(uintptr_t)&bo_handle;
    a.num_bo_handles = 1;
    require(call(gpu_fd, DRM_IOCTL_VIRTGPU_EXECBUFFER, &a) == 0, "same-queue in-fence submission");
    report_held();
}

/** Blocking WAIT completes the work and NOWAIT then reports idle. */
static void wait_row(void)
{
    struct drm_virtgpu_3d_wait a;
    wait_texture();
    a.handle = bo_handle;
    a.flags = VIRTGPU_WAIT_NOWAIT;
    require(call(gpu_fd, DRM_IOCTL_VIRTGPU_WAIT, &a) == 0, "NOWAIT after completion");
    report_held();
}

/** Reset with an exported fence; check pending work and old handles. */
static void reset_row(void)
{
    char path[80], *p = path;
    const char *prefix = "/sys/kernel/debug/dri/", *suffix = "/virtio_gpu_reset";
    struct drm_virtgpu_execbuffer a;
    struct drm_virtgpu_3d_wait w;
    struct sync_file_info before, after;

    syscall1(SYS_CLOSE, fence_fd);
    fence_fd = -1;
    zero(&a, sizeof(a));
    a.bo_handles = (uint64_t)(uintptr_t)&bo_handle;
    a.num_bo_handles = 1;
    a.flags = VIRTGPU_EXECBUF_FENCE_FD_OUT;
    require(call(gpu_fd, DRM_IOCTL_VIRTGPU_EXECBUFFER, &a) == 0, "submission before reset");
    fence_fd = a.fence_fd;
    zero(&before, sizeof(before));
    require(call(fence_fd, SYNC_IOC_FILE_INFO, &before) == 0, "fence before reset");
    while (*prefix) {
        *p++ = *prefix++;
    }
    p = number(p, (uint32_t)card_index);
    while (*suffix) {
        *p++ = *suffix++;
    }
    *p = 0;
    reset_fd = (int)syscall3(SYS_OPEN, (long)path, 1, 0);
    require(reset_fd >= 0, "open reset trigger");
    require(syscall3(SYS_WRITE, reset_fd, (long)"1\n", 2) == 2, "trigger reset");
    zero(&after, sizeof(after));
    require(call(fence_fd, SYNC_IOC_FILE_INFO, &after) == 0, "fence after reset");
    require(before.status != 0 || after.status == -19, "every pending fence fails with ENODEV");
    w.handle = bo_handle;
    w.flags = 0;
    zero(&a, sizeof(a));
    require(call(gpu_fd, DRM_IOCTL_VIRTGPU_WAIT, &w) == -19 &&
                call(gpu_fd, DRM_IOCTL_VIRTGPU_EXECBUFFER, &a) == -19,
            "old handles fail after reset");
    report_held();
}

void probe_main(unsigned long *sp);
PROBE_SP_ENTRY;

/** Run the eight reported rows and close every resource on exit. */
void probe_main(unsigned long *sp)
{
    const char *rows[] = {"params", "context",   "resource", "transfer",
                          "submit", "out-fence", "wait",     "reset"};
    uint32_t i;
    report_begin(report_case((char **)(sp + 1) + sp[0] + 1, "machine.vgpu.virgl"));
    for (i = 0; i < 8; i++) {
        report_plan(rows[i]);
    }
    params_row();
    context_row();
    resource_row();
    transfer_row();
    submit_row();
    fence_row();
    wait_row();
    reset_row();
    report_end();
    probe_line("VGPUPROBE OK", 0, 0);
    finish(0);
}
