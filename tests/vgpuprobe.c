/* vgpuprobe.c: virgl capability sets, transfers, submissions, the closes
 * and frees of shared and busy buffers, and device loss on the virgl QEMU
 * profile, as root, where the reset row unplugs its GPU;
 * a program's exit that frees thousands of textures at once, on the same
 * profile; and blobs, host-visible memory and CONTEXT_INIT on the Venus
 * profile. */
#include "probe.h"
#include "probe_report.h"
#include "../rach2/sys/drm/virtio_gpu.h"

/* virgl's command header and the commands, format, bind and flags the blob
 * row uses, from virglrenderer's src/virgl_protocol.h and src/virgl_hw.h. */
#define VIRGL_CMD0(cmd, obj, len) \
    ((uint32_t)(cmd) | ((uint32_t)(obj) << 8) | ((uint32_t)(len) << 16))
#define VIRGL_CCMD_RESOURCE_COPY_REGION     17u
#define VIRGL_CCMD_PIPE_RESOURCE_CREATE     48u
#define VIRGL_CMD_RESOURCE_COPY_REGION_SIZE 13u
#define VIRGL_PIPE_RES_CREATE_SIZE          11u
#define VIRGL_FORMAT_R8_UNORM               64u
#define VIRGL_BIND_VERTEX_BUFFER            (1u << 4)
#define VIRGL_RESOURCE_FLAG_MAP_PERSISTENT  (1u << 1)
#define VIRGL_RESOURCE_FLAG_MAP_COHERENT    (1u << 2)

#define BLOB_BYTES 64 /* the bytes the blob row writes and reads back */

/* The textures the exit case's child makes and leaves to its exit: each is
 * freed there with a CTX_DETACH and a RESOURCE_UNREF of a DMA page each,
 * 64 MiB in all, the whole of the window below HARD_MEMORY_LIMIT. */
#define EXIT_TEXTURES 8192
#define EXIT_BATCH    128 /* the textures made between two waits */

static int gpu_fd = -1, other_fd = -1, fence_fd = -1, reset_fd = -1, ctx_fd = -1, hold_fd = -1;
static void *mapping, *blob_map, *buf_map;
static uint32_t bo_handle, blob_handle, buf_handle;
static uint32_t capsets; /* SUPPORTED_CAPSET_IDs */
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

/** Release every descriptor, buffer and mapping, then exit. */
static void finish(int rc)
{
    int fds[6], i;
    void *maps[3];
    uint32_t handles[3];
    struct drm_gem_close c;

    maps[0] = mapping;
    maps[1] = blob_map;
    maps[2] = buf_map;
    handles[0] = bo_handle;
    handles[1] = blob_handle;
    handles[2] = buf_handle;
    for (i = 0; i < 3; i++) {
        if (maps[i]) {
            syscall2(SYS_MUNMAP, (long)maps[i], 4096);
        }
        if (handles[i] && gpu_fd >= 0) {
            zero(&c, sizeof(c));
            c.handle = handles[i];
            (void)call(gpu_fd, DRM_IOCTL_GEM_CLOSE, &c);
        }
    }
    fds[0] = gpu_fd;
    fds[1] = other_fd;
    fds[2] = fence_fd;
    fds[3] = reset_fd;
    fds[4] = ctx_fd;
    fds[5] = hold_fd;
    for (i = 0; i < 6; i++) {
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

/** Whether strings `a` and `b` are equal. */
static int same(const char *a, const char *b)
{
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
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

/** Map the page of buffer `handle` of the GPU's file for reading and
 * writing: its address, or a failed row named `what`. */
static void *map_bo(uint32_t handle, const char *what)
{
    struct drm_virtgpu_map m;
    long address;

    zero(&m, sizeof(m));
    m.handle = handle;
    require(call(gpu_fd, DRM_IOCTL_VIRTGPU_MAP, &m) == 0, what);
#ifdef __x86_64__
    address = syscall6(SYS_MMAP, 0, 4096, 3, 1, gpu_fd, (long)m.offset);
#else
    address = syscall6(SYS_MMAP2, 0, 4096, 3, 1, gpu_fd, (long)(m.offset >> 12));
#endif
    require((unsigned long)address < (unsigned long)-4095, what);
    return (void *)address;
}

/** Make a 4 by 4 BGRA texture over one page of backing. */
static void resource_row(void)
{
    struct drm_virtgpu_resource_create a;
    struct drm_virtgpu_resource_info info;

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
    mapping = map_bo(bo_handle, "resource mapping");
    report_held();
}

/** Wait for the queued work of buffer `handle`. */
static void wait_bo(uint32_t handle)
{
    struct drm_virtgpu_3d_wait a;
    a.handle = handle;
    a.flags = 0;
    require(call(gpu_fd, DRM_IOCTL_VIRTGPU_WAIT, &a) == 0, "wait for a buffer");
}

/** Wait for the texture's queued work. */
static void wait_texture(void)
{
    wait_bo(bo_handle);
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

/** Make a 4 by 4 BGRA texture over one page of backing in file `fd`: its
 * handle, or 0. */
static uint32_t small_texture(int fd)
{
    struct drm_virtgpu_resource_create a;

    zero(&a, sizeof(a));
    a.target = 2;
    a.format = 1;
    a.bind = 2;
    a.width = a.height = 4;
    a.depth = a.array_size = 1;
    a.size = 4096;
    return call(fd, DRM_IOCTL_VIRTGPU_RESOURCE_CREATE, &a) == 0 ? a.bo_handle : 0;
}

/* A timespec, two of the machine's longs on both architectures. */
struct probe_timespec {
    long tv_sec, tv_nsec;
};

/** CLOCK_MONOTONIC in microseconds. */
static long long now_us(void)
{
    struct probe_timespec t;

    syscall2(SYS_CLOCK_GETTIME, 1, (long)&t);
    return (long long)t.tv_sec * 1000000 + t.tv_nsec / 1000;
}

/** Sleep a millisecond, with no GPU call. */
static void nap(void)
{
    struct probe_timespec t;

    t.tv_sec = 0;
    t.tv_nsec = 1000000;
    /* interval: a polled count or fence is read again every millisecond */
    syscall2(SYS_NANOSLEEP, (long)&t, 0);
}

static char proc_text[32768]; /* a /proc or debugfs file read whole */

/** Read the /proc or debugfs file `path` whole into proc_text: 0, or -1. */
static int proc_read(const char *path)
{
    long fd = syscall3(SYS_OPEN, (long)path, 0, 0), n, got = 0;

    if (fd < 0) {
        return -1;
    }
    while ((n = syscall3(SYS_READ, fd, (long)(proc_text + got),
                         (long)(sizeof(proc_text) - 1 - got))) > 0) {
        got += n;
    }
    syscall1(SYS_CLOSE, fd);
    proc_text[got] = 0;
    return 0;
}

/** The kernel's count of virtio-gpu objects, /proc/stat's
 * virtio_gpu_objects, or a failed row named `what`. */
static uint32_t objects_now(const char *what)
{
    const char *key = "\nvirtio_gpu_objects ", *k;
    char *p;
    uint32_t n = 0;

    require(proc_read("/proc/stat") == 0, what);
    for (p = proc_text; *p; p++) {
        for (k = key; *k && *k == p[k - key]; k++) {
        }
        if (!*k) {
            for (p += k - key; *p >= '0' && *p <= '9'; p++) {
                n = n * 10 + (uint32_t)(*p - '0');
            }
            return n;
        }
    }
    require(0, what);
    return 0;
}

/** Whether sync file `fd` has signalled, or a failed row named `what`. */
static int fence_done(int fd, const char *what)
{
    struct sync_file_info info;

    zero(&info, sizeof(info));
    require(call(fd, SYNC_IOC_FILE_INFO, &info) == 0 && info.status >= 0, what);
    return info.status == 1;
}

/** Write the texture's 64 bytes as `i * mul + add`, upload them, clear the
 * mapping and read them back through the GPU's file, or a failed row named
 * `what`. */
static void roundtrip(uint8_t mul, uint8_t add, const char *what)
{
    struct drm_virtgpu_3d_transfer a;
    uint8_t *bytes = mapping;
    uint32_t i;

    zero(&a, sizeof(a));
    a.bo_handle = bo_handle;
    a.box.w = a.box.h = 4;
    a.box.d = 1;
    for (i = 0; i < 64; i++) {
        bytes[i] = (uint8_t)(i * mul + add);
    }
    require(call(gpu_fd, DRM_IOCTL_VIRTGPU_TRANSFER_TO_HOST, &a) == 0, what);
    wait_texture();
    for (i = 0; i < 64; i++) {
        bytes[i] = 0;
    }
    require(call(gpu_fd, DRM_IOCTL_VIRTGPU_TRANSFER_FROM_HOST, &a) == 0, what);
    wait_texture();
    for (i = 0; i < 64; i++) {
        require(bytes[i] == (uint8_t)(i * mul + add), what);
    }
}

/** Submit an empty batch on file `fd` naming its buffer `handle`, with an
 * out-fence when `fence` is set: the fence's descriptor, or 0. */
static int submit_naming(int fd, uint32_t *handle, int fence, const char *what)
{
    struct drm_virtgpu_execbuffer a;

    zero(&a, sizeof(a));
    a.bo_handles = (uint64_t)(uintptr_t)handle;
    a.num_bo_handles = 1;
    a.flags = fence ? VIRTGPU_EXECBUF_FENCE_FD_OUT : 0;
    require(call(fd, DRM_IOCTL_VIRTGPU_EXECBUFFER, &a) == 0 && (!fence || a.fence_fd >= 0), what);
    return fence ? a.fence_fd : 0;
}

/** Close buffer `handle` of file `fd`, or a failed row named `what`. */
static void close_handle(int fd, uint32_t handle, const char *what)
{
    struct drm_gem_close c;

    zero(&c, sizeof(c));
    c.handle = handle;
    require(call(fd, DRM_IOCTL_GEM_CLOSE, &c) == 0, what);
}

/** Put "/sys/kernel/debug/dri/N/" and file `name` of the GPU's debugfs
 * directory at `path`, of 80 bytes. */
static void debug_path(char *path, const char *name)
{
    const char *prefix = "/sys/kernel/debug/dri/";
    char *p = path;

    while (*prefix) {
        *p++ = *prefix++;
    }
    p = number(p, (uint32_t)card_index);
    *p++ = '/';
    while (*name) {
        *p++ = *name++;
    }
    *p = 0;
}

/** Hold the driver's answers until hold_off: a descriptor of its debugfs
 * file virtio_gpu_hold, while which the references of a submission and a
 * DETACH on their buffers stay, whatever the host's speed. */
static void hold_on(const char *what)
{
    char path[80];

    debug_path(path, "virtio_gpu_hold");
    hold_fd = (int)syscall3(SYS_OPEN, (long)path, 1, 0);
    require(hold_fd >= 0, what);
}

/** Let the held answers go: the close runs their completions at once. */
static void hold_off(void)
{
    syscall1(SYS_CLOSE, hold_fd);
    hold_fd = -1;
}

/** The resource id of buffer `handle` of file `fd`, or a failed row named
 * `what`. */
static uint32_t res_of(int fd, uint32_t handle, const char *what)
{
    struct drm_virtgpu_resource_info info;

    zero(&info, sizeof(info));
    info.bo_handle = handle;
    require(call(fd, DRM_IOCTL_VIRTGPU_RESOURCE_INFO, &info) == 0 && info.res_handle, what);
    return info.res_handle;
}

/** Read decimal at `*p`, moving past it. */
static uint32_t decimal(char **p)
{
    uint32_t n = 0;

    for (; **p >= '0' && **p <= '9'; (*p)++) {
        n = n * 10 + (uint32_t)(**p - '0');
    }
    return n;
}

/** Put in `ctx` the ids of the contexts resource `res` is attached to, by
 * the driver's debugfs file virtio_gpu_attachments, at most 4: their count,
 * or a failed row named `what`. */
static uint32_t attached_to(uint32_t res, uint32_t *ctx, const char *what)
{
    char path[80], *p;
    uint32_t c, n = 0;

    debug_path(path, "virtio_gpu_attachments");
    require(proc_read(path) == 0, what);
    for (p = proc_text; *p;) {
        c = decimal(&p);
        require(*p++ == ' ', what);
        if (decimal(&p) == res) {
            require(n < 4, what);
            ctx[n++] = c;
        }
        require(*p++ == '\n', what);
    }
    return n;
}

/** Give buffer `handle` of the GPU's file to the second file by PRIME: its
 * handle there, or a failed row named `what`. */
static uint32_t share(uint32_t handle, const char *what)
{
    struct drm_prime_handle ph;
    long rc;
    int fd;

    zero(&ph, sizeof(ph));
    ph.handle = handle;
    ph.flags = DRM_CLOEXEC | DRM_RDWR;
    require(call(gpu_fd, DRM_IOCTL_PRIME_HANDLE_TO_FD, &ph) == 0 && ph.fd >= 0, what);
    fd = ph.fd;
    zero(&ph, sizeof(ph));
    ph.fd = fd;
    rc = call(other_fd, DRM_IOCTL_PRIME_FD_TO_HANDLE, &ph);
    syscall1(SYS_CLOSE, fd);
    require(rc == 0 && ph.handle != 0, what);
    return ph.handle;
}

/** A buffer that both files hold and have named in a submission is attached
 * to both their contexts, read from virtio_gpu_attachments with no draw
 * between, which would attach it again; the close of one file's handle
 * detaches that file's context alone, the second file's first and then,
 * with another texture, the first file's. The first file then still
 * uploads, reads back and submits its texture, and the second file still
 * submits the other. */
static void shared_row(void)
{
    char path[64];
    uint32_t oh, h, res, ctx[4], first, second;

    render_path(path, (uint32_t)(128 + card_index));
    other_fd = (int)syscall3(SYS_OPEN, (long)path, 2, 0);
    require(other_fd >= 0, "a second open of the render node");
    res = res_of(gpu_fd, bo_handle, "the texture's resource");
    require(attached_to(res, ctx, "read the attachments") == 1,
            "the texture attached to the first file's context alone");
    first = ctx[0];
    oh = share(bo_handle, "import the texture in the second file");
    (void)submit_naming(other_fd, &oh, 0, "the second file's submission naming it");
    require(attached_to(res, ctx, "read the attachments") == 2 &&
                (ctx[0] == first) != (ctx[1] == first),
            "the texture attached to both files' contexts");
    second = ctx[0] == first ? ctx[1] : ctx[0];
    close_handle(other_fd, oh, "the second file's close of the texture");
    require(attached_to(res, ctx, "read the attachments") == 1 && ctx[0] == first,
            "the second file's close detaches its context alone");
    roundtrip(7, 41, "the first file's texture after the second's close");
    (void)submit_naming(gpu_fd, &bo_handle, 0, "the first file's submission after it");
    wait_texture();
    h = small_texture(gpu_fd);
    require(h != 0, "a texture to share");
    res = res_of(gpu_fd, h, "the shared texture's resource");
    oh = share(h, "import the second texture in the second file");
    (void)submit_naming(gpu_fd, &h, 0, "the first file's submission naming the second texture");
    (void)submit_naming(other_fd, &oh, 0, "the second file's submission naming it");
    require(attached_to(res, ctx, "read the attachments") == 2 &&
                ((ctx[0] == first && ctx[1] == second) || (ctx[0] == second && ctx[1] == first)),
            "the second texture attached to both files' contexts");
    close_handle(gpu_fd, h, "the first file's close of the second texture");
    require(attached_to(res, ctx, "read the attachments") == 1 && ctx[0] == second,
            "the first file's close detaches its context alone");
    (void)submit_naming(other_fd, &oh, 0, "the second file's submission after the first's close");
    close_handle(other_fd, oh, "the second file's close of the second texture");
    require(attached_to(res, ctx, "read the attachments") == 0,
            "the last close detaches the last context");
    syscall1(SYS_CLOSE, other_fd);
    other_fd = -1;
    report_held();
}

/** Close a texture while a mapping and a submission hold it, with the
 * driver's answers held (`hold_on`), so that the submission is still
 * pending, whatever the host's speed, until the row lets it end: the
 * texture stays, counted in virtio_gpu_objects, until both have ended,
 * whichever ends first, and then goes with no further call. */
static void busy_close_row(void)
{
    uint32_t h, n, i;
    uint8_t *m;
    int fence;

    /* The submission ends first, then the mapping. */
    h = small_texture(gpu_fd);
    require(h != 0, "a texture to close while busy");
    m = map_bo(h, "map the texture to close");
    for (i = 0; i < 64; i++) {
        m[i] = (uint8_t)(i + 99);
    }
    n = objects_now("read virtio_gpu_objects");
    hold_on("hold the driver's answers");
    fence = submit_naming(gpu_fd, &h, 1, "a submission naming the texture");
    close_handle(gpu_fd, h, "close the busy texture");
    while (!fence_done(fence, "the submission's fence")) {
        nap();
    }
    syscall1(SYS_CLOSE, fence);
    require(objects_now("read virtio_gpu_objects") == n,
            "the held submission keeps the closed texture");
    hold_off();
    require(objects_now("read virtio_gpu_objects") == n,
            "the mapping keeps the texture after its submission ends");
    for (i = 0; i < 64; i++) {
        require(m[i] == (uint8_t)(i + 99), "the mapping's bytes after the close");
    }
    syscall2(SYS_MUNMAP, (long)m, 4096);
    while (objects_now("read virtio_gpu_objects") != n - 1) {
        nap();
    }
    /* The mapping ends first, then the submission. */
    h = small_texture(gpu_fd);
    require(h != 0, "a second texture to close while busy");
    m = map_bo(h, "map the second texture");
    n = objects_now("read virtio_gpu_objects");
    hold_on("hold the driver's answers again");
    fence = submit_naming(gpu_fd, &h, 1, "a submission naming the second texture");
    close_handle(gpu_fd, h, "close the second busy texture");
    syscall2(SYS_MUNMAP, (long)m, 4096);
    require(objects_now("read virtio_gpu_objects") == n,
            "the held submission keeps the texture after its mapping ends");
    hold_off();
    while (objects_now("read virtio_gpu_objects") != n - 1) {
        nap();
    }
    syscall1(SYS_CLOSE, fence);
    report_held();
}

/** A texture named in a submission and closed while the driver holds its
 * answers is held by the submission and the DETACH alone, so that their
 * answers drop its last reference, which goes to the kernel's worker: once
 * the answers go, it is freed with no further call of the program, and the
 * UNREF's answer takes it out of virtio_gpu_objects. The time from the
 * answers' release to that is printed, not judged. */
static void unref_row(void)
{
    uint32_t h, n;
    long long began;
    char ms[12];

    h = small_texture(gpu_fd);
    require(h != 0, "a texture to free");
    n = objects_now("read virtio_gpu_objects");
    hold_on("hold the driver's answers");
    (void)submit_naming(gpu_fd, &h, 0, "a submission naming the texture to free");
    close_handle(gpu_fd, h, "close the texture to free");
    require(objects_now("read virtio_gpu_objects") == n,
            "the held answers keep the closed texture");
    began = now_us();
    hold_off();
    while (objects_now("read virtio_gpu_objects") != n - 1) {
        nap();
    }
    probe_line("VGPUPROBE UNREF ms ", report_decimal(ms, (int)(now_us() - began) / 1000), 0);
    report_held();
}

/** Reset with an exported fence; check pending work and old handles. */
static void reset_row(void)
{
    char path[80];
    struct drm_virtgpu_execbuffer a;
    struct drm_virtgpu_3d_wait w;
    struct sync_file_info before, after;
    char status[16];

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
    debug_path(path, "virtio_gpu_reset");
    reset_fd = (int)syscall3(SYS_OPEN, (long)path, 1, 0);
    require(reset_fd >= 0, "open reset trigger");
    require(syscall3(SYS_WRITE, reset_fd, (long)"1\n", 2) == 2, "trigger reset");
    zero(&after, sizeof(after));
    require(call(fence_fd, SYNC_IOC_FILE_INFO, &after) == 0, "fence after reset");
    probe_line("VGPUPROBE RESET before ", report_decimal(status, before.status), 0);
    probe_line(after.status < 0 ? "VGPUPROBE RESET after -" : "VGPUPROBE RESET after ",
               report_decimal(status, after.status < 0 ? -after.status : after.status), 0);
    require((before.status == 1 && after.status == 1) ||
                (before.status == 0 && (after.status == 1 || after.status == -19)),
            "fence completes or fails with ENODEV at reset");
    w.handle = bo_handle;
    w.flags = 0;
    zero(&a, sizeof(a));
    require(call(gpu_fd, DRM_IOCTL_VIRTGPU_WAIT, &w) == -19 &&
                call(gpu_fd, DRM_IOCTL_VIRTGPU_EXECBUFFER, &a) == -19,
            "old handles fail after reset");
    report_held();
}

/** GETPARAM `which` of the GPU's file, or -1. */
static int32_t param(uint64_t which)
{
    struct drm_virtgpu_getparam p;
    int32_t value = -1;

    p.param = which;
    p.value = (uint64_t)(uintptr_t)&value;
    return call(gpu_fd, DRM_IOCTL_VIRTGPU_GETPARAM, &p) == 0 ? value : -1;
}

/** Blobs, host-visible memory and context init are offered, with the
 * capability sets of virgl, virgl2 and Venus. */
static void blob_params_row(void)
{
    char text[12];

    find_gpu();
    require(param(VIRTGPU_PARAM_RESOURCE_BLOB) == 1 && param(VIRTGPU_PARAM_HOST_VISIBLE) == 1,
            "blobs and host-visible memory");
    require(param(VIRTGPU_PARAM_CONTEXT_INIT) == 1 && param(VIRTGPU_PARAM_EXPLICIT_DEBUG_NAME) == 1,
            "context init and its names");
    capsets = (uint32_t)param(VIRTGPU_PARAM_SUPPORTED_CAPSET_IDs);
    probe_line("VGPUPROBE CAPSETS ", report_decimal(text, (int)capsets), 0);
    require((capsets & 0x16) == 0x16, "capsets of virgl, virgl2 and Venus");
    report_held();
}

/** CONTEXT_INIT of a new file with capability set `id`, one ring that is
 * polled and a name; an empty submission on the ring comes back as the
 * ring's event, and a second CONTEXT_INIT is refused. */
static void ring_context(uint32_t id)
{
    struct drm_virtgpu_context_set_param p[4];
    struct drm_virtgpu_context_init a;
    struct drm_virtgpu_execbuffer e;
    uint32_t event[2];
    char path[64], text[12];

    render_path(path, (uint32_t)(128 + card_index));
    ctx_fd = (int)syscall3(SYS_OPEN, (long)path, 2, 0);
    require(ctx_fd >= 0, "open a file for a context");
    p[0].param = VIRTGPU_CONTEXT_PARAM_CAPSET_ID;
    p[0].value = id;
    p[1].param = VIRTGPU_CONTEXT_PARAM_NUM_RINGS;
    p[1].value = 1;
    p[2].param = VIRTGPU_CONTEXT_PARAM_POLL_RINGS_MASK;
    p[2].value = 1;
    p[3].param = VIRTGPU_CONTEXT_PARAM_DEBUG_NAME;
    p[3].value = (uint64_t)(uintptr_t)"vgpuprobe";
    a.num_params = 4;
    a.pad = 0;
    a.ctx_set_params = (uint64_t)(uintptr_t)p;
    require(call(ctx_fd, DRM_IOCTL_VIRTGPU_CONTEXT_INIT, &a) == 0,
            "CONTEXT_INIT with an offered capset");
    zero(&e, sizeof(e));
    e.flags = VIRTGPU_EXECBUF_RING_IDX;
    require(call(ctx_fd, DRM_IOCTL_VIRTGPU_EXECBUFFER, &e) == 0, "a submission on the ring");
    require(syscall3(SYS_READ, ctx_fd, (long)event, sizeof(event)) == 8 &&
                event[0] == VIRTGPU_EVENT_FENCE_SIGNALED && event[1] == 8,
            "the polled ring's fence event");
    require(call(ctx_fd, DRM_IOCTL_VIRTGPU_CONTEXT_INIT, &a) == -17,
            "a second CONTEXT_INIT refused");
    probe_line("VGPUPROBE CONTEXT capset ", report_decimal(text, (int)id), 0);
    syscall1(SYS_CLOSE, ctx_fd);
    ctx_fd = -1;
}

/** A context of each capability set the device offers. */
static void context_init_row(void)
{
    uint32_t id;

    for (id = 1; id < 32; id++) {
        if (capsets & (1u << id)) {
            ring_context(id);
        }
    }
    report_held();
}

/** A mappable blob of host memory, a buffer that virgl's
 * PIPE_RESOURCE_CREATE makes, as Mesa's virgl makes one: its handle, and
 * its resource id in *res. */
static uint32_t host_blob(uint32_t *res)
{
    uint32_t cmd[VIRGL_PIPE_RES_CREATE_SIZE + 1];
    struct drm_virtgpu_resource_create_blob a;

    zero(cmd, sizeof(cmd));
    cmd[0] = VIRGL_CMD0(VIRGL_CCMD_PIPE_RESOURCE_CREATE, 0, VIRGL_PIPE_RES_CREATE_SIZE);
    cmd[2] = VIRGL_FORMAT_R8_UNORM;
    cmd[3] = VIRGL_BIND_VERTEX_BUFFER;
    cmd[4] = 4096;
    cmd[5] = cmd[6] = cmd[7] = 1;
    cmd[10] = VIRGL_RESOURCE_FLAG_MAP_PERSISTENT | VIRGL_RESOURCE_FLAG_MAP_COHERENT;
    cmd[11] = 1;
    zero(&a, sizeof(a));
    a.blob_mem = VIRTGPU_BLOB_MEM_HOST3D;
    a.blob_flags = VIRTGPU_BLOB_FLAG_USE_MAPPABLE;
    a.size = 4096;
    a.cmd = (uint64_t)(uintptr_t)cmd;
    a.cmd_size = sizeof(cmd);
    a.blob_id = 1;
    require(call(gpu_fd, DRM_IOCTL_VIRTGPU_RESOURCE_CREATE_BLOB, &a) == 0 && a.bo_handle,
            "a mappable blob of host memory");
    *res = a.res_handle;
    return a.bo_handle;
}

/** A buffer of a page with guest backing: its handle, and its resource id
 * in *res. */
static uint32_t staging(uint32_t *res)
{
    struct drm_virtgpu_resource_create a;

    zero(&a, sizeof(a));
    a.format = VIRGL_FORMAT_R8_UNORM;
    a.bind = VIRGL_BIND_VERTEX_BUFFER;
    a.width = 4096;
    a.height = a.depth = a.array_size = 1;
    a.size = 4096;
    require(call(gpu_fd, DRM_IOCTL_VIRTGPU_RESOURCE_CREATE, &a) == 0 && a.bo_handle,
            "a buffer to read the blob back into");
    *res = a.res_handle;
    return a.bo_handle;
}

/** Copy the first BLOB_BYTES of resource `src` to resource `dst` on the
 * host with virgl's RESOURCE_COPY_REGION, naming the two buffers. */
static void copy_region(uint32_t dst, uint32_t src, uint32_t *handles)
{
    uint32_t cmd[VIRGL_CMD_RESOURCE_COPY_REGION_SIZE + 1];
    struct drm_virtgpu_execbuffer e;

    zero(cmd, sizeof(cmd));
    cmd[0] = VIRGL_CMD0(VIRGL_CCMD_RESOURCE_COPY_REGION, 0, VIRGL_CMD_RESOURCE_COPY_REGION_SIZE);
    cmd[1] = dst;
    cmd[6] = src;
    cmd[11] = BLOB_BYTES;
    cmd[12] = cmd[13] = 1;
    zero(&e, sizeof(e));
    e.command = (uint64_t)(uintptr_t)cmd;
    e.size = sizeof(cmd);
    e.bo_handles = (uint64_t)(uintptr_t)handles;
    e.num_bo_handles = 2;
    require(call(gpu_fd, DRM_IOCTL_VIRTGPU_EXECBUFFER, &e) == 0, "copy the blob on the host");
}

/** Write a blob of host memory through its mapping, copy it on the host to
 * a buffer with guest backing, and read the copy back with a transfer. */
static void blob_row(void)
{
    struct drm_virtgpu_resource_info info;
    struct drm_virtgpu_3d_transfer t;
    uint32_t handles[2], blob_res, buf_res, i;
    uint8_t *bytes;

    blob_handle = handles[0] = host_blob(&blob_res);
    zero(&info, sizeof(info));
    info.bo_handle = blob_handle;
    require(call(gpu_fd, DRM_IOCTL_VIRTGPU_RESOURCE_INFO, &info) == 0 &&
                info.blob_mem == VIRTGPU_BLOB_MEM_HOST3D && info.size == 4096,
            "the blob's info");
    blob_map = map_bo(blob_handle, "map the blob");
    bytes = blob_map;
    for (i = 0; i < BLOB_BYTES; i++) {
        bytes[i] = (uint8_t)(i * 5 + 3);
    }
    buf_handle = handles[1] = staging(&buf_res);
    buf_map = map_bo(buf_handle, "map the buffer");
    copy_region(buf_res, blob_res, handles);
    zero(&t, sizeof(t));
    t.bo_handle = buf_handle;
    t.box.w = BLOB_BYTES;
    t.box.h = t.box.d = 1;
    require(call(gpu_fd, DRM_IOCTL_VIRTGPU_TRANSFER_FROM_HOST, &t) == 0, "read the copy back");
    wait_bo(buf_handle);
    bytes = buf_map;
    for (i = 0; i < BLOB_BYTES; i++) {
        require(bytes[i] == (uint8_t)(i * 5 + 3), "the blob's bytes read back");
    }
    report_held();
}

/** Print the line of /proc file `path` that begins with `key`, as
 * "VGPUPROBE EXIT `when` line", or nothing when either is missing. */
static void proc_line(const char *path, const char *key, const char *when)
{
    char *line, *end;
    const char *k;

    if (proc_read(path) != 0) {
        return;
    }
    for (line = proc_text; *line; line = *end ? end + 1 : end) {
        for (end = line; *end && *end != '\n'; end++) {
        }
        for (k = key; *k && *k == line[k - key]; k++) {
        }
        if (!*k) {
            *end = 0;
            probe_line("VGPUPROBE EXIT ", when, line);
            return;
        }
    }
}

/** Print the window's free frames and their fewest since boot, the trace of
 * the pool whose end unplugged the GPU. */
static void window_trace(const char *when)
{
    proc_line("/proc/meminfo", "LowFree:", when);
    proc_line("/proc/stat", "window_free_min ", when);
}

/** The exit case's child: open the render node, make a context and
 * EXIT_TEXTURES textures, waiting on every EXIT_BATCHth so that the commands
 * of their making are answered, and exit with them all still open. Exits
 * 0, or 1 with no context, or 2 when a texture could not be made. */
static void flood_child(const char *path)
{
    struct drm_virtgpu_execbuffer e;
    struct drm_virtgpu_3d_wait w;
    uint32_t i, handle;
    int fd = (int)syscall3(SYS_OPEN, (long)path, 2, 0);

    zero(&e, sizeof(e));
    if (fd < 0 || call(fd, DRM_IOCTL_VIRTGPU_EXECBUFFER, &e) != 0) {
        syscall1(SYS_EXIT, 1);
    }
    for (i = 1; i <= EXIT_TEXTURES; i++) {
        handle = small_texture(fd);
        if (!handle) {
            syscall1(SYS_EXIT, 2);
        }
        if (i % EXIT_BATCH == 0) {
            w.handle = handle;
            w.flags = 0;
            (void)call(fd, DRM_IOCTL_VIRTGPU_WAIT, &w);
        }
    }
    syscall1(SYS_EXIT, 0);
}

/** A child makes EXIT_TEXTURES textures and exits without freeing them, so
 * that its exit frees them all at once under the big lock, as a piglit
 * test of every internal format did when virgl's guest lost its GPU. */
static void flood_row(void)
{
    char path[64];
    long pid;
    int status = -1;

    find_gpu();
    render_path(path, (uint32_t)(128 + card_index));
    syscall1(SYS_CLOSE, other_fd);
    other_fd = -1;
    window_trace("before ");
    pid = syscall0(SYS_FORK);
    require(pid >= 0, "fork the child that floods");
    if (pid == 0) {
        flood_child(path);
    }
    require(syscall4(SYS_WAIT4, pid, (long)&status, 0, 0) == pid, "wait for the child");
    window_trace("after ");
    require((status & 0x7f) == 0 && ((status >> 8) & 0xff) != 1, "the child's context");
    require((status & 0x7f) == 0 && ((status >> 8) & 0xff) == 0, "the child's 8192 textures");
    report_held();
}

/** After the child's exit the GPU is still there: a texture of the
 * parent's file is made and waited on, and the render node opens again
 * as virgl's. */
static void survives_row(void)
{
    struct drm_virtgpu_3d_wait w;

    w.handle = small_texture(gpu_fd);
    w.flags = 0;
    require(w.handle != 0, "a texture after the exit");
    require(call(gpu_fd, DRM_IOCTL_VIRTGPU_WAIT, &w) == 0, "wait after the exit");
    syscall1(SYS_CLOSE, gpu_fd);
    gpu_fd = -1;
    find_gpu();
    report_held();
}

/** The exit case's two rows, on the virgl profile. */
static void exit_case(void)
{
    report_plan("flood");
    report_plan("survives");
    flood_row();
    survives_row();
    window_trace("end ");
    report_end();
    probe_line("VGPUPROBE OK", 0, 0);
    finish(0);
}

void probe_main(unsigned long *sp);
PROBE_SP_ENTRY;

/** The blob case's three rows, on the Venus profile. */
static void blob_case(void)
{
    report_plan("blob-params");
    report_plan("context-init");
    report_plan("blob");
    blob_params_row();
    context_init_row();
    blob_row();
    report_end();
    probe_line("VGPUPROBE OK", 0, 0);
    finish(0);
}

/** Run the case's rows, the virgl case's eleven, the exit case's two or the
 * blob case's three, and close every resource on exit. */
void probe_main(unsigned long *sp)
{
    const char *rows[] = {"params", "context", "resource",   "transfer", "submit", "out-fence",
                          "wait",   "shared",  "busy-close", "unref",    "reset"};
    const char *name = report_case((char **)(sp + 1) + sp[0] + 1, "machine.vgpu.virgl");
    uint32_t i;

    report_begin(name);
    if (same(name, "machine.vgpu.blob")) {
        blob_case();
    }
    if (same(name, "machine.vgpu.exit")) {
        exit_case();
    }
    for (i = 0; i < 11; i++) {
        report_plan(rows[i]);
    }
    params_row();
    context_row();
    resource_row();
    transfer_row();
    submit_row();
    fence_row();
    wait_row();
    shared_row();
    busy_close_row();
    unref_row();
    reset_row();
    report_end();
    probe_line("VGPUPROBE OK", 0, 0);
    finish(0);
}
