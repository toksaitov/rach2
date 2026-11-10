/* vgshmprobe.c: virtio-gpu over the region Rach2 shares with a VM's device:
 * a frame drawn by virgl in the VM and read back, and the fences that the
 * VM's death must fail. */
#include "probe.h"
#include "../rach2/sys/drm/uapi.h"
#include "../rach2/sys/drm/virtio_gpu.h"

/* virgl's commands and objects, from virglrenderer's src/virgl_protocol.h,
 * and gallium's targets, binds and clear bits, as bed/shm/peer.c writes its
 * display frame. */
#define VIRGL_CMD0(cmd, obj, len) \
    ((uint32_t)(cmd) | ((uint32_t)(obj) << 8) | ((uint32_t)(len) << 16))
#define VIRGL_CCMD_CREATE_OBJECT         1u
#define VIRGL_CCMD_SET_FRAMEBUFFER_STATE 5u
#define VIRGL_CCMD_CLEAR                 7u
#define VIRGL_CCMD_SET_SUB_CTX           28u
#define VIRGL_CCMD_CREATE_SUB_CTX        29u
#define VIRGL_CCMD_CLEAR_TEXTURE         47u
#define VIRGL_OBJECT_SURFACE             8u
#define VIRGL_FORMAT_B8G8R8X8_UNORM      2u
#define PIPE_TEXTURE_2D                  2u
#define PIPE_BIND_RENDER_TARGET          2u
#define PIPE_BIND_SCANOUT                (1u << 18)
#define PIPE_BIND_SHARED                 (1u << 20)
#define PIPE_CLEAR_COLOR0                4u

/* The frame: 1024 by 768, cleared to 0.25, 0.5, 0.75 with an orange box of
 * 256 by 128 at 64, 32. */
#define FRAME_W 1024u
#define FRAME_H 768u

/* Fenced submissions the death row leaves pending, at most. */
#define PENDING_MAX 16

static int gpu_fd = -1;

/** The ioctl result, as a negated errno on failure. */
static long call(int fd, unsigned long op, void *arg)
{
    return syscall3(SYS_IOCTL, fd, (long)op, (long)arg);
}

/** Clear an ioctl argument. */
static void zero(void *p, unsigned long n)
{
    unsigned char *s = p;

    while (n--) {
        *s++ = 0;
    }
}

/** `n` as eight hex digits in `out`, which holds nine bytes. */
static char *hex(char *out, uint32_t n)
{
    int i;

    for (i = 7; i >= 0; i--, n >>= 4) {
        out[i] = "0123456789abcdef"[n & 15u];
    }
    out[8] = 0;
    return out;
}

/** `n` in decimal in `out`, which holds twelve bytes. */
static char *dec(char *out, long n)
{
    char d[12];
    int i = 0, j = 0;
    unsigned long v = n < 0 ? (unsigned long)-n : (unsigned long)n;

    do {
        d[i++] = (char)('0' + v % 10u);
        v /= 10u;
    } while (v);
    if (n < 0) {
        out[j++] = '-';
    }
    while (i) {
        out[j++] = d[--i];
    }
    out[j] = 0;
    return out;
}

/** Fail the probe with `what`. */
static void fail(const char *what)
{
    probe_line("VGSHMPROBE FAIL: ", what, 0);
    syscall1(SYS_EXIT_GROUP, 1);
}

/** Open the first render node whose driver is virtio_gpu with virgl. */
static void find_gpu(void)
{
    char path[32] = "/dev/dri/renderD12x", name[32];
    struct drm_version v;
    struct drm_virtgpu_getparam p;
    int32_t value;
    int i;

    for (i = 0; i < 10; i++) {
        path[18] = (char)('8' + i);
        gpu_fd = (int)syscall3(SYS_OPEN, (long)path, 2, 0);
        if (gpu_fd < 0) {
            continue;
        }
        zero(&v, sizeof(v));
        zero(name, sizeof(name));
        v.name = name;
        v.name_len = sizeof(name) - 1;
        p.param = VIRTGPU_PARAM_3D_FEATURES;
        p.value = (uint64_t)(uintptr_t)&value;
        value = 0;
        if (call(gpu_fd, DRM_IOCTL_VERSION, &v) == 0 && v.name_len == 10 && name[0] == 'v' &&
            call(gpu_fd, DRM_IOCTL_VIRTGPU_GETPARAM, &p) == 0 && value == 1) {
            return;
        }
        syscall1(SYS_CLOSE, gpu_fd);
        gpu_fd = -1;
    }
    fail("no virgl render node");
}

/** Make a B8G8R8X8 render target of `w` by `h` that a display may share;
 * its buffer handle, and its resource handle in `*res`. */
static uint32_t target(uint32_t w, uint32_t h, uint32_t *res)
{
    struct drm_virtgpu_resource_create a;

    zero(&a, sizeof(a));
    a.target = PIPE_TEXTURE_2D;
    a.format = VIRGL_FORMAT_B8G8R8X8_UNORM;
    a.bind = PIPE_BIND_RENDER_TARGET | PIPE_BIND_SCANOUT | PIPE_BIND_SHARED;
    a.width = w;
    a.height = h;
    a.depth = a.array_size = 1;
    a.size = w * h * 4u;
    if (call(gpu_fd, DRM_IOCTL_VIRTGPU_RESOURCE_CREATE, &a) != 0 || !a.bo_handle) {
        fail("render target creation");
    }
    *res = a.res_handle;
    return a.bo_handle;
}

/** Submit `n` dwords of virgl at `s` naming buffer `bo`, with an out-fence
 * when `fence` is set: the fence's descriptor, or 0. */
static int submit(const uint32_t *s, uint32_t n, uint32_t bo, int fence)
{
    struct drm_virtgpu_execbuffer a;

    zero(&a, sizeof(a));
    a.command = (uint64_t)(uintptr_t)s;
    a.size = n * 4u;
    a.bo_handles = (uint64_t)(uintptr_t)&bo;
    a.num_bo_handles = 1;
    a.fence_fd = -1;
    a.flags = fence ? VIRTGPU_EXECBUF_FENCE_FD_OUT : 0;
    if (call(gpu_fd, DRM_IOCTL_VIRTGPU_EXECBUFFER, &a) != 0) {
        fail("submission");
    }
    return fence ? a.fence_fd : 0;
}

/** The display frame's stream on resource `res`, as bed/shm/peer.c writes
 * it, into `s`: its length in dwords. */
static uint32_t frame_stream(uint32_t *s, uint32_t res)
{
    static const uint32_t rgba[4] = {0x3e800000u, 0x3f000000u, 0x3f400000u, 0x3f800000u};
    uint32_t n = 0, i;

    s[n++] = VIRGL_CMD0(VIRGL_CCMD_CREATE_SUB_CTX, 0, 1);
    s[n++] = 1;
    s[n++] = VIRGL_CMD0(VIRGL_CCMD_SET_SUB_CTX, 0, 1);
    s[n++] = 1;
    s[n++] = VIRGL_CMD0(VIRGL_CCMD_CREATE_OBJECT, VIRGL_OBJECT_SURFACE, 5);
    s[n++] = 1;
    s[n++] = res;
    s[n++] = VIRGL_FORMAT_B8G8R8X8_UNORM;
    s[n++] = 0;
    s[n++] = 0;
    s[n++] = VIRGL_CMD0(VIRGL_CCMD_SET_FRAMEBUFFER_STATE, 0, 3);
    s[n++] = 1;
    s[n++] = 0;
    s[n++] = 1;
    s[n++] = VIRGL_CMD0(VIRGL_CCMD_CLEAR, 0, 8);
    s[n++] = PIPE_CLEAR_COLOR0;
    for (i = 0; i < 4; i++) {
        s[n++] = rgba[i];
    }
    s[n++] = 0;
    s[n++] = 0;
    s[n++] = 0;
    s[n++] = VIRGL_CMD0(VIRGL_CCMD_CLEAR_TEXTURE, 0, 12);
    s[n++] = res;
    s[n++] = 0;
    s[n++] = 64;
    s[n++] = 32;
    s[n++] = 0;
    s[n++] = 256;
    s[n++] = 128;
    s[n++] = 1;
    s[n++] = 0xff0080ffu;
    s[n++] = 0;
    s[n++] = 0;
    s[n++] = 0;
    return n;
}

/** zlib's CRC-32 of `n` bytes at `p`. */
static uint32_t crc32(const uint8_t *p, unsigned long n)
{
    uint32_t c = 0xffffffffu;
    int k;

    while (n--) {
        c ^= *p++;
        for (k = 0; k < 8; k++) {
            c = c & 1u ? (c >> 1) ^ 0xedb88320u : c >> 1;
        }
    }
    return ~c;
}

/**
 * Draw the display frame with virgl in the VM, read it back into its buffer
 * and print the CRC-32 of its rows as stored, each pixel's X byte 0xff, as
 * bed/shm/peer.c prints a readback; pass when it is `want`, the CRC
 * llvmpipe gives.
 */
static void draw(const char *want)
{
    struct drm_virtgpu_3d_transfer t;
    struct drm_virtgpu_3d_wait w;
    struct drm_virtgpu_map m;
    uint32_t s[64], res, bo, n, got, i;
    unsigned long bytes = FRAME_W * FRAME_H * 4u;
    uint8_t *px;
    long at;
    char text[12];

    find_gpu();
    probe_line("VGSHMPROBE STEP: render node", 0, 0);
    bo = target(FRAME_W, FRAME_H, &res);
    probe_line("VGSHMPROBE STEP: render target", 0, 0);
    n = frame_stream(s, res);
    (void)submit(s, n, bo, 0);
    probe_line("VGSHMPROBE STEP: submitted", 0, 0);
    zero(&t, sizeof(t));
    t.bo_handle = bo;
    t.box.w = FRAME_W;
    t.box.h = FRAME_H;
    t.box.d = 1;
    if (call(gpu_fd, DRM_IOCTL_VIRTGPU_TRANSFER_FROM_HOST, &t) != 0) {
        fail("read back the frame");
    }
    probe_line("VGSHMPROBE STEP: read back", 0, 0);
    w.handle = bo;
    w.flags = 0;
    if (call(gpu_fd, DRM_IOCTL_VIRTGPU_WAIT, &w) != 0) {
        fail("wait for the readback");
    }
    probe_line("VGSHMPROBE STEP: waited", 0, 0);
    zero(&m, sizeof(m));
    m.handle = bo;
    if (call(gpu_fd, DRM_IOCTL_VIRTGPU_MAP, &m) != 0) {
        fail("map the frame");
    }
    at = probe_mmap(0, (long)bytes, 3, 1, gpu_fd, (long)m.offset);
    if (sys_failed(at)) {
        fail("map the frame");
    }
    px = (uint8_t *)at;
    for (i = 3; i < bytes; i += 4) {
        px[i] = 0xff;
    }
    got = crc32(px, bytes);
    probe_line("VGSHMPROBE DRAW: crc ", hex(text, got), 0);
    if (want) {
        for (i = 0; i < 8 && want[i] == text[i]; i++) {
        }
        if (i != 8 || want[8] != 0) {
            fail("the frame is not llvmpipe's");
        }
    }
    probe_line("VGSHMPROBE OK", 0, 0);
}

/**
 * Leave `count` fenced submissions pending, say so, and wait for the
 * buffer and for each fence: each fence must end with an error, which the
 * VM's death gives it, and the buffer's wait must return, with 0 once its
 * fences have ended, as Linux's VIRTGPU_WAIT does; a wait after the death
 * must fail.
 */
static void pending(long count)
{
    struct sync_fence_info fi;
    struct sync_file_info info;
    struct drm_virtgpu_3d_wait w;
    struct {
        int fd;
        short events, revents;
    } p[PENDING_MAX];
    uint32_t res, bo;
    long i, failed = 0, r, waited, after;
    char text[12];

    if (count < 1 || count > PENDING_MAX) {
        fail("a count from 1 to 16");
    }
    find_gpu();
    bo = target(64, 64, &res);
    for (i = 0; i < count; i++) {
        p[i].fd = submit(&res, 0, bo, 1);
        p[i].events = 1; /* POLLIN */
        p[i].revents = 0;
    }
    probe_line("VGSHMPROBE PENDING ", dec(text, count), 0);
    w.handle = bo;
    w.flags = 0;
    waited = call(gpu_fd, DRM_IOCTL_VIRTGPU_WAIT, &w);
    for (i = 0; i < count; i++) {
        r = syscall3(SYS_POLL, (long)&p[i], 1, -1);
        zero(&info, sizeof(info));
        zero(&fi, sizeof(fi));
        info.num_fences = 1;
        info.sync_fence_info = (uint64_t)(uintptr_t)&fi;
        if (r != 1 || call(p[i].fd, SYNC_IOC_FILE_INFO, &info) != 0) {
            fail("a fence's state");
        }
        failed += fi.status < 0;
        if (fi.status >= 0) {
            probe_line("VGSHMPROBE FENCE: signalled without an error, ", dec(text, i), 0);
        } else if (i == 0) {
            probe_line("VGSHMPROBE FENCE: error ", dec(text, fi.status), 0);
        }
    }
    probe_line("VGSHMPROBE WAIT: ", dec(text, waited), 0);
    after = call(gpu_fd, DRM_IOCTL_VIRTGPU_WAIT, &w);
    probe_line("VGSHMPROBE WAIT AFTER: ", dec(text, after), 0);
    if (failed != count || after == 0) {
        fail("a waiter saw no error");
    }
    probe_line("VGSHMPROBE OK", 0, 0);
}

void probe_main(unsigned long *sp);
PROBE_SP_ENTRY;

/** `vgshmprobe draw [CRC]` or `vgshmprobe pending N`, entered with `sp` at
 * argc. */
void probe_main(unsigned long *sp)
{
    int argc = (int)sp[0];
    char **argv = (char **)(sp + 1);
    long n = 0;
    const char *c;

    if (argc >= 2 && argv[1][0] == 'd') {
        draw(argc >= 3 ? argv[2] : 0);
        syscall1(SYS_EXIT_GROUP, 0);
    }
    if (argc == 3 && argv[1][0] == 'p') {
        for (c = argv[2]; *c >= '0' && *c <= '9'; c++) {
            n = n * 10 + (*c - '0');
        }
        pending(n);
        syscall1(SYS_EXIT_GROUP, 0);
    }
    probe_line("usage: vgshmprobe draw [CRC] | pending N", 0, 0);
    syscall1(SYS_EXIT_GROUP, 2);
}
