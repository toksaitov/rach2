/* i915execprobe.c: the Intel driver's EXECBUFFER2 on the fake chip in QEMU:
 * batches on both rings that complete, a relocation written and given back,
 * and the faults the chip's debugfs file injects, which reach the caller as
 * Linux's errors. Runs under rach2.i915fake as root, which the fake's file
 * and a batch with a length need. */
#include "probe.h"
#include "probe_report.h"
#include "../rach2/sys/drm/i915/i915_drm.h"

#define ETIME 62

/* MI_BATCH_BUFFER_END, which the fake does not read but a chip would. */
#define BATCH_END 0x05000000u

/* The reloc row's delta and the presumed offset it gives, which is wrong. */
#define RELOC_DELTA     0x40u
#define RELOC_AT        8u
#define RELOC_PRESUMED  0x7ff00000u
#define HANG_TIMEOUT_NS 50000000

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

/* The entries of the last call, whose offsets the call gives back. */
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
        probe_line("I915EXECPROBE FAIL: ", what, 0);
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

/** Run `batch` alone, or after `target` with `reloc` into it, on `ring`,
 * with an out-fence kept in `*fence` when it is not null: the ioctl's
 * result. The entries' offsets come back in `objs`. */
static long submit(uint32_t ring, uint32_t len, uint32_t target, uint32_t batch,
                   struct drm_i915_gem_relocation_entry *reloc, int *fence)
{
    struct drm_i915_gem_execbuffer2 eb;
    uint32_t n = 0;
    long r;

    zero(objs, sizeof(objs));
    if (target) {
        objs[n].handle = target;
        objs[n].flags = EXEC_OBJECT_WRITE;
        n++;
    }
    objs[n].handle = batch;
    if (reloc) {
        objs[n].relocation_count = 1;
        objs[n].relocs_ptr = (uintptr_t)reloc;
    }
    n++;
    zero(&eb, sizeof(eb));
    eb.buffers_ptr = (uintptr_t)objs;
    eb.buffer_count = n;
    eb.batch_len = len;
    eb.flags = ring | (fence ? I915_EXEC_FENCE_OUT : 0u);
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

/** Wait with no limit until the sync file `fd` signals: 1, or 0. */
static int fence_wait(int fd)
{
    struct pollfd p;

    p.fd = fd;
    p.events = 1;
    p.revents = 0;
    return syscall3(SYS_POLL, (long)&p, 1, -1) == 1 && (p.revents & 1) && fence_status(fd) == 1;
}

/** BUSY's answer for `handle`. */
static uint32_t busy(uint32_t handle)
{
    struct drm_i915_gem_busy b;

    zero(&b, sizeof(b));
    b.handle = handle;
    require(call(DRM_IOCTL_I915_GEM_BUSY, &b) == 0, "busy");
    return b.busy;
}

/** WAIT on `handle` for at most `ns`: the ioctl's result. */
static long wait_for(uint32_t handle, int64_t ns)
{
    struct drm_i915_gem_wait w;

    zero(&w, sizeof(w));
    w.bo_handle = handle;
    w.timeout_ns = ns;
    return call(DRM_IOCTL_I915_GEM_WAIT, &w);
}

/** A batch with a length on the render ring and one without on the
 * blitter complete: each out-fence signals, each ring counts the batch,
 * and the objects are idle. */
static void submit_row(uint32_t batch, uint32_t data)
{
    uint32_t rcs = fake_count("ring rcs0", "batches");
    uint32_t bcs = fake_count("ring bcs0", "batches");
    int f1, f2;

    require(submit(I915_EXEC_RENDER, 8, data, batch, 0, &f1) == 0, "submit: render");
    require(submit(I915_EXEC_BLT, 0, data, batch, 0, &f2) == 0, "submit: blitter");
    require(fence_wait(f1) && fence_wait(f2), "submit: the out-fences signal");
    require(fake_count("ring rcs0", "batches") == rcs + 1 &&
                fake_count("ring bcs0", "batches") == bcs + 1,
            "submit: each ring ran one batch");
    require(busy(data) == 0 && busy(batch) == 0 && wait_for(data, -1) == 0,
            "submit: the objects idle");
    report_held();
}

/** A relocation with a wrong presumed offset is written with the target's
 * offset and the delta, and the offsets come back to the caller. */
static void reloc_row(uint32_t batch, uint32_t data)
{
    struct drm_i915_gem_relocation_entry r;
    struct drm_i915_gem_pread p;
    uint32_t word = 0;
    int f;

    zero(&r, sizeof(r));
    r.target_handle = data;
    r.delta = RELOC_DELTA;
    r.offset = RELOC_AT;
    r.presumed_offset = RELOC_PRESUMED;
    r.read_domains = I915_GEM_DOMAIN_RENDER;
    r.write_domain = I915_GEM_DOMAIN_RENDER;
    require(submit(I915_EXEC_RENDER, 16, data, batch, &r, &f) == 0 && fence_wait(f),
            "reloc: submit");
    require(r.presumed_offset == (objs[0].offset & ~(uint64_t)0xfff) &&
                r.presumed_offset != RELOC_PRESUMED,
            "reloc: the presumed offset given back");
    zero(&p, sizeof(p));
    p.handle = batch;
    p.offset = RELOC_AT;
    p.size = sizeof(word);
    p.data_ptr = (uintptr_t)&word;
    require(call(DRM_IOCTL_I915_GEM_PREAD, &p) == 0 &&
                word == (uint32_t)r.presumed_offset + RELOC_DELTA,
            "reloc: the batch holds the target's offset and the delta");
    report_held();
}

/** The submit fault fails the call with EIO and the bind fault, armed for
 * good, with ENOSPC once eviction has nothing left; disarmed, the next
 * calls complete. */
static void fault_row(uint32_t batch, uint32_t data)
{
    uint32_t fresh;
    int f;

    fake_command("submit");
    require(submit(I915_EXEC_RENDER, 8, data, batch, 0, 0) == -5, "faults: submit, EIO");
    require(submit(I915_EXEC_RENDER, 8, data, batch, 0, &f) == 0 && fence_wait(f),
            "faults: the next submission completes");
    fresh = make(4096);
    fake_command("bind always");
    require(submit(I915_EXEC_RENDER, 8, 0, fresh, 0, 0) == -28, "faults: bind, ENOSPC");
    fake_command("bind 0");
    require(fake_count("fault bind", "injected") > 0, "faults: the bind fault injected");
    require(submit(I915_EXEC_RENDER, 8, 0, fresh, 0, &f) == 0 && fence_wait(f),
            "faults: disarmed, the batch completes");
    report_held();
}

/** A batch on a hung ring never completes: a bounded WAIT gives ETIME, as
 * Linux's does before a reset, BUSY reports the object busy, and the
 * out-fence stays active. Nothing takes the ring back, so this row is the
 * last. */
static void hang_row(uint32_t batch, uint32_t data)
{
    int f;

    fake_command("hang");
    require(submit(I915_EXEC_RENDER, 8, data, batch, 0, &f) == 0, "hang: submit");
    require(wait_for(data, HANG_TIMEOUT_NS) == -ETIME, "hang: a bounded wait, ETIME");
    require(busy(data) != 0 && fence_status(f) == 0, "hang: busy, the out-fence active");
    require(fake_count("ring rcs0", "hung") == 1, "hang: the fake's ring hung");
    report_held();
}

void probe_main(unsigned long *sp);
PROBE_SP_ENTRY;

/** Run the four reported rows and close every resource on exit. */
void probe_main(unsigned long *sp)
{
    const char *rows[] = {"submit", "reloc", "faults", "hang"};
    uint32_t i, batch, data;

    report_begin(report_case((char **)(sp + 1) + sp[0] + 1, "machine.i915fake.exec"));
    for (i = 0; i < 4; i++) {
        report_plan(rows[i]);
    }
    find_gpu();
    fake_command("clear");
    batch = make(4096);
    data = make(65536);
    submit_row(batch, data);
    reloc_row(batch, data);
    fault_row(batch, data);
    hang_row(batch, data);
    report_end();
    probe_line("I915EXECPROBE OK", 0, 0);
    finish(0);
}
