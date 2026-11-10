/* i915resetprobe.c: the Intel driver's recovery from a hang on the fake
 * chip in QEMU: a batch the chip's debugfs file hangs is found by the
 * heartbeat and its engine reset, its context alone charged and the batch
 * of another context behind it run again, and a context that hangs three
 * times banned. Runs under rach2.i915fake as root, which the fake's file,
 * a batch with a length and the reset count need. */
#include "probe.h"
#include "probe_report.h"
#include "../rach2/sys/drm/i915/i915_drm.h"

/* MI_BATCH_BUFFER_END, which the fake does not read but a chip would. */
#define BATCH_END 0x05000000u

#define EIO 5

struct pollfd {
    int fd;
    short events, revents;
};

static int gpu_fd = -1;
static char fake[] = "/sys/kernel/debug/dri/000/i915_fake";
static uint32_t handles[8];
static uint32_t nhandles;
static int fences[8];
static uint32_t nfences;

/* The entries of the last call. */
static struct drm_i915_gem_exec_object2 objs[2];

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

/** Close the objects, the sync files and the files, then exit with `rc`. */
static void finish(int rc)
{
    struct drm_gem_close c;

    while (nhandles > 0) {
        zero(&c, sizeof(c));
        c.handle = handles[--nhandles];
        (void)call(DRM_IOCTL_GEM_CLOSE, &c);
    }
    while (nfences > 0) {
        syscall1(SYS_CLOSE, fences[--nfences]);
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
        probe_line("I915RESETPROBE FAIL: ", what, 0);
        report_stop(what);
        finish(1);
    }
}

/** Open the first render node whose driver is i915, and the fake's file. */
static void find_gpu(void)
{
    char path[] = "/dev/dri/renderD128", name[8];
    struct drm_version v;
    int i, fd = -1;

    for (i = 0; i < 10 && gpu_fd < 0; i++) {
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
        if (call(DRM_IOCTL_VERSION, &v) != 0 || v.name_len != 4 || name[0] != 'i' ||
            name[1] != '9' || name[2] != '1' || name[3] != '5') {
            syscall1(SYS_CLOSE, gpu_fd);
            gpu_fd = -1;
        }
    }
    require(gpu_fd >= 0, "no i915 render node");
    /* The device's directory is named by its minor, one to three digits. */
    for (i = 0; i < 256 && fd < 0; i++) {
        char *p = fake + 22, *s;

        if (i >= 100) {
            *p++ = (char)('0' + i / 100);
        }
        if (i >= 10) {
            *p++ = (char)('0' + i / 10 % 10);
        }
        *p++ = (char)('0' + i % 10);
        for (s = "/i915_fake"; *s; s++) {
            *p++ = *s;
        }
        *p = '\0';
        fd = (int)syscall3(SYS_OPEN, (long)fake, 2, 0);
    }
    require(fd >= 0, "no i915_fake file in debugfs");
    syscall1(SYS_CLOSE, fd);
}

/** Give the fake's file the command `s`. */
static void fake_command(const char *s)
{
    int fd = (int)syscall3(SYS_OPEN, (long)fake, 1, 0);
    long n = 0, r;

    while (s[n]) {
        n++;
    }
    r = fd < 0 ? -1 : syscall3(SYS_WRITE, fd, (long)s, n);
    if (fd >= 0) {
        syscall1(SYS_CLOSE, fd);
    }
    require(r == n, "a command to the fake's file");
}

/** The number after `key` on the fake file's line that begins `line`,
 * from a new open, since the file's text is made at the open. */
static uint32_t fake_count(const char *line, const char *key)
{
    static char text[2048];
    int fd = (int)syscall3(SYS_OPEN, (long)fake, 0, 0);
    long n = 0, r = 1, i, k;
    uint32_t v = 0;

    require(fd >= 0, "the fake's file opened");
    while (r > 0 && n < (long)sizeof(text) - 1) {
        r = syscall3(SYS_READ, fd, (long)(text + n), (long)sizeof(text) - 1 - n);
        n += r > 0 ? r : 0;
    }
    syscall1(SYS_CLOSE, fd);
    require(n > 0, "the fake's file read");
    text[n] = '\0';
    for (i = 0; i < n; i++) {
        if (i > 0 && text[i - 1] != '\n') {
            continue;
        }
        for (k = 0; line[k] && text[i + k] == line[k]; k++) {
        }
        if (line[k]) {
            continue;
        }
        for (i += k; text[i] && text[i] != '\n'; i++) {
            for (k = 0; key[k] && text[i + k] == key[k]; k++) {
            }
            if (!key[k] && text[i + k] == ' ') {
                for (i += k + 1; text[i] >= '0' && text[i] <= '9'; i++) {
                    v = v * 10 + (uint32_t)(text[i] - '0');
                }
                return v;
            }
        }
        break;
    }
    require(0, "a line of the fake's file");
    return 0;
}

/** A new object of `bytes` that begins with a batch's end. */
static uint32_t make(uint64_t bytes)
{
    struct drm_i915_gem_create c;
    struct drm_i915_gem_pwrite w;
    uint32_t end = BATCH_END;

    zero(&c, sizeof(c));
    c.size = bytes;
    require(call(DRM_IOCTL_I915_GEM_CREATE, &c) == 0 && c.handle != 0, "create");
    handles[nhandles++] = c.handle;
    zero(&w, sizeof(w));
    w.handle = c.handle;
    w.size = sizeof(end);
    w.data_ptr = (uintptr_t)&end;
    require(call(DRM_IOCTL_I915_GEM_PWRITE, &w) == 0, "pwrite");
    return c.handle;
}

/** Run `batch` after `target` on the render ring in context `ctx`, with an
 * out-fence kept in `*fence` when it is not null: the ioctl's result. */
static long submit(uint32_t ctx, uint32_t target, uint32_t batch, int *fence)
{
    struct drm_i915_gem_execbuffer2 eb;
    long r;

    zero(objs, sizeof(objs));
    objs[0].handle = target;
    objs[0].flags = EXEC_OBJECT_WRITE;
    objs[1].handle = batch;
    zero(&eb, sizeof(eb));
    eb.buffers_ptr = (uintptr_t)objs;
    eb.buffer_count = 2;
    eb.batch_len = 8;
    eb.flags = I915_EXEC_RENDER | (fence ? I915_EXEC_FENCE_OUT : 0u);
    eb.rsvd1 = ctx;
    eb.rsvd2 = (uint64_t)-1;
    r = call(DRM_IOCTL_I915_GEM_EXECBUFFER2_WR, &eb);
    if (r == 0 && fence) {
        *fence = (int)(eb.rsvd2 >> 32);
        require(*fence >= 0, "an out-fence");
        fences[nfences++] = *fence;
    }
    return r;
}

/** A sync file's status: 1 signalled, 0 active, negative on error. */
static int fence_status(int fd)
{
    struct sync_file_info info;

    zero(&info, sizeof(info));
    if (syscall3(SYS_IOCTL, fd, SYNC_IOC_FILE_INFO, (long)&info) != 0) {
        return -1;
    }
    return info.status;
}

/** Wait with no limit until the sync file `fd` signals: its status then,
 * 1 or the fence's error, or 0 when the poll failed. */
static int fence_end(int fd)
{
    struct pollfd p;

    p.fd = fd;
    p.events = 1;
    p.revents = 0;
    if (syscall3(SYS_POLL, (long)&p, 1, -1) != 1 || !(p.revents & 1)) {
        return 0;
    }
    return fence_status(fd);
}

/** A new context: its id. */
static uint32_t context(void)
{
    struct drm_i915_gem_context_create c;

    zero(&c, sizeof(c));
    require(call(DRM_IOCTL_I915_GEM_CONTEXT_CREATE, &c) == 0 && c.ctx_id != 0, "a context made");
    return c.ctx_id;
}

/** GET_RESET_STATS of the context `ctx`. */
static struct drm_i915_reset_stats stats(uint32_t ctx)
{
    struct drm_i915_reset_stats s;

    zero(&s, sizeof(s));
    s.ctx_id = ctx;
    require(call(DRM_IOCTL_I915_GET_RESET_STATS, &s) == 0, "reset stats");
    return s;
}

/* The contexts, the guilty and the innocent, the out-fences of the hang
 * row's batches, and the fake's counts before the hang: rcs0's batches,
 * the batch starts one batch takes (two when the command parser runs it
 * from its shadow through the trampoline), and the resets of each domain,
 * the fake's own at its start among them. */
static uint32_t ctx_a, ctx_b;
static int fence_a, fence_b;
static uint32_t batches0, per_batch, rcs0, bcs0, full0;

/** A batch of context A on a ring the fake hangs, and one of context B
 * queued behind it, are both accepted and neither has completed. A batch
 * runs first, so that the GT's default state is recorded before the hang,
 * which would otherwise wedge the GPU for good, as in Linux. */
static void hang_row(uint32_t batch, uint32_t data)
{
    int f;

    batches0 = fake_count("ring rcs0", "batches");
    require(submit(0, data, batch, &f) == 0 && fence_end(f) == 1, "hang: a first batch runs");
    per_batch = fake_count("ring rcs0", "batches") - batches0;
    require(per_batch > 0, "hang: the fake counts the first batch");
    ctx_a = context();
    ctx_b = context();
    batches0 = fake_count("ring rcs0", "batches");
    rcs0 = fake_count("resets", "rcs0");
    bcs0 = fake_count("resets", "bcs0");
    full0 = fake_count("resets", "full");
    fake_command("hang");
    require(submit(ctx_a, data, batch, &fence_a) == 0, "hang: A's batch accepted");
    require(submit(ctx_b, data, batch, &fence_b) == 0, "hang: B's batch accepted");
    require(fence_status(fence_a) == 0 && fence_status(fence_b) == 0,
            "hang: both out-fences active");
    report_held();
}

/** The heartbeat resets the render engine alone: B's out-fence signals,
 * A's ends with EIO, and the fake's file counts the two resets of rcs0's
 * domain that Linux's engine reset writes, none of the whole GPU, and the
 * ring no longer hung. */
static void reset_row(void)
{
    require(fence_end(fence_b) == 1, "reset: B's out-fence signals after the reset");
    require(fence_status(fence_a) == -EIO, "reset: A's out-fence ends with EIO");
    require(fake_count("resets", "rcs0") == rcs0 + 2 && fake_count("resets", "bcs0") == bcs0 &&
                fake_count("resets", "full") == full0,
            "reset: the fake reset rcs0's domain twice and nothing else");
    require(fake_count("ring rcs0", "hung") == 0, "reset: rcs0 runs again");
    report_held();
}

/** The batch behind the guilty one runs again and the guilty one does
 * not, and GET_RESET_STATS charges A alone, with no reset of the GPU. */
static void replay_row(void)
{
    struct drm_i915_reset_stats a = stats(ctx_a), b = stats(ctx_b);

    require(fake_count("ring rcs0", "batches") == batches0 + per_batch,
            "replay: B's batch ran and A's did not");
    require(a.batch_active == 1 && a.batch_pending == 0 && a.reset_count == 0,
            "replay: A charged with one hang");
    require(b.batch_active == 0 && b.batch_pending == 0, "replay: B not charged");
    report_held();
}

/** Two more hangs of A within Linux's 120 s ban it: EXECBUFFER2 refuses A
 * with EIO and GET_RESET_STATS counts its three hangs, while B's batches
 * still run. */
static void ban_row(uint32_t batch, uint32_t data)
{
    int f, i;

    for (i = 0; i < 2; i++) {
        fake_command("hang");
        require(submit(ctx_a, data, batch, &f) == 0, "ban: A's batch accepted before its ban");
        require(fence_end(f) == -EIO, "ban: A's batch ends with EIO");
    }
    require(submit(ctx_a, data, batch, 0) == -EIO, "ban: EXECBUFFER2 refuses A with EIO");
    require(stats(ctx_a).batch_active == 3, "ban: A charged with three hangs");
    require(fake_count("resets", "rcs0") == rcs0 + 6,
            "ban: the fake reset rcs0's domain six times");
    require(submit(ctx_b, data, batch, &f) == 0 && fence_end(f) == 1, "ban: B's batch runs");
    report_held();
}

void probe_main(unsigned long *sp);
PROBE_SP_ENTRY;

/** Run the four reported rows and close every resource on exit. */
void probe_main(unsigned long *sp)
{
    const char *rows[] = {"hang", "reset", "replay", "ban"};
    uint32_t i, batch, data;

    report_begin(report_case((char **)(sp + 1) + sp[0] + 1, "machine.i915fake.reset"));
    for (i = 0; i < 4; i++) {
        report_plan(rows[i]);
    }
    find_gpu();
    fake_command("clear");
    batch = make(4096);
    data = make(65536);
    hang_row(batch, data);
    reset_row();
    replay_row();
    ban_row(batch, data);
    report_end();
    probe_line("I915RESETPROBE OK", 0, 0);
    finish(0);
}
