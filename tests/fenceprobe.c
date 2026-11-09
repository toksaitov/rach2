/*
 * Producers that are slow, concurrent or failing, and consumers that read a
 * fence's state and only then the buffer, over udmabuf, a card's dumb
 * buffers and DRM syncobjs. Each producer is a child that waits for the
 * parent's go on a pipe, writes the shared buffer and advances its sw_sync
 * timeline, or exits without advancing it, which fails its points.
 */
#include "probe.h"
#include "probe_report.h"
#include "../rach2/sys/drm/uapi.h"

#if defined __x86_64__
#define SYS_MEMFD_CREATE 319
#else
#define SYS_MEMFD_CREATE 356
#endif

#define SIZE      8192
#define PRODUCERS 8
#define SLICE     (SIZE / PRODUCERS)
#define ENOENT    2

/* What a producer does beyond writing its bytes: sleep first, or fail. */
#define SLOW 1
#define FAIL 2

static int fds[64], nfds;
static long maps[8];
static unsigned nmaps;
static long children[16];
static unsigned nchildren;

/* A buffer to produce into: its dma-buf descriptor and its mapping. */
struct buffer {
    int fd;
    unsigned char *map;
};

/* A source of buffers: udmabuf's device, or a card's node. */
struct source {
    int dev;
    int card;
};

struct pollfd {
    int fd;
    short events, revents;
};

/** Kill and reap every child, unmap and close everything the probe made. */
static void cleanup(void)
{
    unsigned i;
    int status;
    for (i = 0; i < nchildren; i++) {
        syscall2(SYS_KILL, children[i], 9);
        syscall4(SYS_WAIT4, children[i], (long)&status, 0, 0);
    }
    nchildren = 0;
    for (i = 0; i < nmaps; i++)
        syscall2(SYS_MUNMAP, maps[i], SIZE);
    nmaps = 0;
    while (nfds)
        syscall1(SYS_CLOSE, fds[--nfds]);
}

/** Report a failed check, release everything and exit. */
static void check(int condition, const char *detail)
{
    if (condition)
        return;
    report_stop(detail);
    probe_line("FENCEPROBE FAIL: ", detail, 0);
    cleanup();
    syscall1(SYS_EXIT, 1);
    for (;;) {
    }
}

/** Remember a descriptor for cleanup. */
static int keep(long fd)
{
    check(fd >= 0 && nfds < 64, "descriptor");
    fds[nfds++] = (int)fd;
    return (int)fd;
}

/** Close a descriptor the probe kept. */
static void drop(int fd)
{
    int i;
    for (i = 0; i < nfds; i++)
        if (fds[i] == fd) {
            fds[i] = fds[--nfds];
            break;
        }
    syscall1(SYS_CLOSE, fd);
}

/** End a child with `ok` as its verdict; it reports nothing itself. */
static void child_exit(int ok)
{
    syscall1(SYS_EXIT, ok ? 0 : 1);
    for (;;) {
    }
}

/** Fork a child the parent reaps; 0 in the child. */
static long spawn(void)
{
    long pid = syscall0(SYS_FORK);
    check(pid >= 0 && nchildren < 16, "fork");
    if (pid)
        children[nchildren++] = pid;
    return pid;
}

/** Reap every child and require that each exited with 0. */
static void reap(const char *detail)
{
    int status, ok = 1;
    while (nchildren) {
        long pid = children[--nchildren];
        ok &= syscall4(SYS_WAIT4, pid, (long)&status, 0, 0) == pid && status == 0;
    }
    check(ok, detail);
}

/** A new software timeline. */
static int timeline(void)
{
    return keep(syscall3(SYS_OPEN, (long)"/sys/kernel/debug/sync/sw_sync", 2, 0));
}

/** A sync file of point `value` of `tl`. */
static int point(int tl, uint32_t value)
{
    struct sw_sync_create_fence_data data;
    int i;
    for (i = 0; i < 32; i++)
        data.name[i] = 0;
    data.value = value;
    data.fence = -1;
    check(syscall3(SYS_IOCTL, tl, SW_SYNC_IOC_CREATE_FENCE, (long)&data) == 0, "sw_sync point");
    return keep(data.fence);
}

/** Advance a timeline by one. */
static long advance(int tl)
{
    uint32_t step = 1;
    return syscall3(SYS_IOCTL, tl, SW_SYNC_IOC_INC, (long)&step);
}

/** A sync file's status and count of leaves, with rows asked for when
 * `rows` is set. */
static int status(int fd, int rows, uint32_t *count)
{
    struct sync_file_info info;
    struct sync_fence_info row[PRODUCERS];
    unsigned char *p = (unsigned char *)&info;
    unsigned i;
    for (i = 0; i < sizeof(info); i++)
        p[i] = 0;
    if (rows) {
        info.num_fences = PRODUCERS;
        info.sync_fence_info = (uintptr_t)row;
    }
    if (syscall3(SYS_IOCTL, fd, SYNC_IOC_FILE_INFO, (long)&info))
        return 99;
    if (count)
        *count = info.num_fences;
    return info.status;
}

/** Wait without a time limit until `fd` is readable: 1, or 0 on error. */
static int wait_readable(int fd)
{
    struct pollfd p;
    p.fd = fd;
    p.events = 1;
    p.revents = 0;
    return syscall3(SYS_POLL, (long)&p, 1, -1) == 1 && (p.revents & 1);
}

/** Whether `fd` is readable now. */
static int readable(int fd)
{
    struct pollfd p;
    p.fd = fd;
    p.events = 1;
    p.revents = 0;
    return syscall3(SYS_POLL, (long)&p, 1, 0) == 1 && (p.revents & 1);
}

/** A sync file of the buffer's writers, as a reader waits for them. */
static int readers_fence(int buf)
{
    struct dma_buf_export_sync_file data;
    data.flags = DMA_BUF_SYNC_READ;
    data.fd = -1;
    check(syscall3(SYS_IOCTL, buf, DMA_BUF_IOCTL_EXPORT_SYNC_FILE, (long)&data) == 0,
          "export the writers' fence");
    return keep(data.fd);
}

/** Make the fence of sync file `fd` a writer of the buffer. */
static void add_writer(int buf, int fd)
{
    struct dma_buf_import_sync_file data;
    data.flags = DMA_BUF_SYNC_WRITE;
    data.fd = fd;
    check(syscall3(SYS_IOCTL, buf, DMA_BUF_IOCTL_IMPORT_SYNC_FILE, (long)&data) == 0,
          "import a writer's fence");
}

/** A new buffer of SIZE bytes from `src`, mapped shared. */
static struct buffer new_buffer(struct source *src)
{
    struct buffer b;
    long at;
    if (src->card) {
        struct drm_mode_create_dumb dumb;
        struct drm_prime_handle prime;
        unsigned char *p = (unsigned char *)&dumb;
        unsigned i;
        for (i = 0; i < sizeof(dumb); i++)
            p[i] = 0;
        dumb.width = 64;
        dumb.height = SIZE / 256;
        dumb.bpp = 32;
        check(syscall3(SYS_IOCTL, src->dev, DRM_IOCTL_MODE_CREATE_DUMB, (long)&dumb) == 0 &&
                  dumb.size >= SIZE,
              "dumb buffer");
        prime.handle = dumb.handle;
        prime.flags = DRM_CLOEXEC | DRM_RDWR;
        prime.fd = -1;
        check(syscall3(SYS_IOCTL, src->dev, DRM_IOCTL_PRIME_HANDLE_TO_FD, (long)&prime) == 0,
              "export dumb buffer");
        b.fd = keep(prime.fd);
    } else {
        struct udmabuf_create data;
        int memfd = keep(syscall2(SYS_MEMFD_CREATE, (long)"fenceprobe", 3));
        check(probe_ftruncate64(memfd, SIZE) == 0, "memfd size");
        check(syscall3(SYS_FCNTL, memfd, 1033, 2) == 0, "shrink seal");
        data.memfd = (uint32_t)memfd;
        data.flags = 1;
        data.offset = 0;
        data.size = SIZE;
        b.fd = keep(syscall3(SYS_IOCTL, src->dev, UDMABUF_CREATE, (long)&data));
        drop(memfd);
    }
    at = probe_mmap(0, SIZE, 3, 1, b.fd, 0);
    check((unsigned long)at < (unsigned long)-4095 && nmaps < 8, "map buffer");
    maps[nmaps++] = at;
    b.map = (unsigned char *)at;
    return b;
}

/** Fill `n` bytes at `p` with `v`. */
static void fill(unsigned char *p, unsigned n, unsigned char v)
{
    while (n--)
        *p++ = v;
}

/** Whether `n` bytes at `p` all hold `v`. */
static int holds(const unsigned char *p, unsigned n, unsigned char v)
{
    while (n--)
        if (*p++ != v)
            return 0;
    return 1;
}

/** In a producer: wait for the go, then write `n` bytes of `v` at `p` and
 * advance `tl`, or, for a failing one, write garbage and exit, which fails
 * the point once its timeline's last descriptor closes. `how` holds SLOW,
 * FAIL or both. */
static void produce(int go, unsigned char *p, unsigned n, unsigned char v, int tl, int how)
{
    char byte;
    struct {
        long sec, nsec;
    } delay;
    if (syscall3(SYS_READ, go, (long)&byte, 1) != 1)
        child_exit(0);
    if (how & SLOW) {
        /* The injected delay: the consumer is asleep on the fence by now. */
        delay.sec = 0;
        delay.nsec = 200000000;
        syscall2(SYS_NANOSLEEP, (long)&delay, 0);
    }
    fill(p, n, how & FAIL ? 0xee : v);
    child_exit((how & FAIL) || advance(tl) == 0);
}

/** A pipe whose read end the producers wait on; the write end is `*go`. */
static int go_pipe(int *go)
{
    int ends[2];
    check(syscall1(SYS_PIPE, (long)ends) == 0, "pipe");
    keep(ends[0]);
    *go = keep(ends[1]);
    return ends[0];
}

/** Release `n` producers waiting on the pipe. */
static void release(int go, int n)
{
    char bytes[PRODUCERS] = {1, 1, 1, 1, 1, 1, 1, 1};
    check(syscall3(SYS_WRITE, go, (long)bytes, n) == n, "release producers");
}

/** One slow producer: the consumer sees the fence active, sleeps on it,
 * then reads the fence's success and only then the buffer. */
static void slow_rows(struct source *src)
{
    struct buffer b = new_buffer(src);
    int tl = timeline(), fence = point(tl, 1), rd, go, reader;
    fill(b.map, SIZE, 0);
    add_writer(b.fd, fence);
    rd = go_pipe(&go);
    if (!spawn())
        produce(rd, b.map, SIZE, 0x5a, tl, SLOW);
    drop(tl);
    reader = readers_fence(b.fd);
    check(status(reader, 0, 0) == 0 && !readable(b.fd), "the slow producer's fence is active");
    release(go, 1);
    check(wait_readable(reader), "the consumer wakes on the fence");
    check(status(reader, 0, 0) == 1 && readable(b.fd), "the fence reports success");
    check(holds(b.map, SIZE, 0x5a), "the consumer reads the whole buffer after the fence");
    reap("the slow producer exits");
}

/** Eight producers on one buffer, each its own slice and timeline: the
 * consumer's fence has eight leaves and signals after the last. */
static void concurrent_rows(struct source *src)
{
    struct buffer b = new_buffer(src);
    int tls[PRODUCERS], rd, go, reader, i;
    uint32_t count;
    fill(b.map, SIZE, 0);
    rd = go_pipe(&go);
    for (i = 0; i < PRODUCERS; i++) {
        tls[i] = timeline();
        add_writer(b.fd, point(tls[i], 1));
    }
    for (i = 0; i < PRODUCERS; i++)
        if (!spawn())
            produce(rd, b.map + i * SLICE, SLICE, (unsigned char)(0x10 + i), tls[i],
                    i & 1 ? SLOW : 0);
    for (i = 0; i < PRODUCERS; i++)
        drop(tls[i]);
    reader = readers_fence(b.fd);
    check(status(reader, 1, &count) == 0 && count == PRODUCERS,
          "the consumer's fence has every producer");
    release(go, PRODUCERS);
    check(wait_readable(reader), "the consumer wakes after every producer");
    check(status(reader, 1, &count) == 1 && count == PRODUCERS, "every producer succeeded");
    for (i = 0; i < PRODUCERS; i++)
        check(holds(b.map + i * SLICE, SLICE, (unsigned char)(0x10 + i)),
              "each producer's slice is whole");
    reap("the concurrent producers exit");
}

/** In a consumer: wait on the buffer's writers and require the producer's
 * error, never trusting the buffer. */
static void consume_error(int buf)
{
    struct dma_buf_export_sync_file data;
    data.flags = DMA_BUF_SYNC_READ;
    data.fd = -1;
    if (syscall3(SYS_IOCTL, buf, DMA_BUF_IOCTL_EXPORT_SYNC_FILE, (long)&data))
        child_exit(0);
    child_exit(wait_readable(data.fd) && status(data.fd, 0, 0) == -ENOENT &&
               status(data.fd, 1, 0) == -ENOENT);
}

/** A producer that fails: every consumer, three children and the parent,
 * reads its error through the fence. */
static void failed_rows(struct source *src)
{
    struct buffer b = new_buffer(src);
    int tl = timeline(), fence = point(tl, 1), rd, go, reader, i;
    add_writer(b.fd, fence);
    rd = go_pipe(&go);
    if (!spawn())
        produce(rd, b.map, SIZE, 0, tl, SLOW | FAIL);
    drop(tl);
    for (i = 0; i < 3; i++)
        if (!spawn())
            consume_error(b.fd);
    reader = readers_fence(b.fd);
    release(go, 1);
    check(wait_readable(reader), "the consumer wakes on the failed fence");
    check(status(reader, 0, 0) == -ENOENT && status(fence, 0, 0) == -ENOENT,
          "the producer's error reaches the consumer");
    reap("every consumer reads the producer's error");
}

/** A node of /dev/dri with the capability `cap`, or -1. */
static int find_node(uint64_t cap, uint64_t bits)
{
    static const char *const nodes[] = {"/dev/dri/renderD128", "/dev/dri/renderD129",
                                        "/dev/dri/card0",      "/dev/dri/card1",
                                        "/dev/dri/card2",      "/dev/dri/card3"};
    struct drm_get_cap c;
    unsigned i;
    long fd;
    for (i = 0; i < sizeof(nodes) / sizeof(nodes[0]); i++) {
        if (cap == DRM_CAP_DUMB_BUFFER && nodes[i][9] == 'r')
            continue;
        fd = syscall3(SYS_OPEN, (long)nodes[i], 2 | 02000000, 0);
        if (fd < 0)
            continue;
        c.capability = cap;
        c.value = 0;
        if (syscall3(SYS_IOCTL, fd, DRM_IOCTL_GET_CAP, (long)&c) == 0 && (c.value & bits) == bits) {
            if (cap != DRM_CAP_DUMB_BUFFER)
                return keep(fd);
            c.capability = DRM_CAP_PRIME;
            c.value = 0;
            if (syscall3(SYS_IOCTL, fd, DRM_IOCTL_GET_CAP, (long)&c) == 0 &&
                (c.value & DRM_PRIME_CAP_EXPORT))
                return keep(fd);
        }
        syscall1(SYS_CLOSE, fd);
    }
    return -1;
}

/** A syncobj handle on `dev`. */
static uint32_t syncobj(int dev)
{
    struct drm_syncobj_create c;
    c.handle = 0;
    c.flags = 0;
    check(syscall3(SYS_IOCTL, dev, DRM_IOCTL_SYNCOBJ_CREATE, (long)&c) == 0, "syncobj create");
    return c.handle;
}

/** Give a syncobj the fence of a sync file. */
static void syncobj_import(int dev, uint32_t handle, int fd)
{
    struct drm_syncobj_handle h;
    h.handle = handle;
    h.flags = DRM_SYNCOBJ_FD_TO_HANDLE_FLAGS_IMPORT_SYNC_FILE;
    h.fd = fd;
    h.pad = 0;
    check(syscall3(SYS_IOCTL, dev, DRM_IOCTL_SYNCOBJ_FD_TO_HANDLE, (long)&h) == 0,
          "syncobj import");
}

/** A sync file of a syncobj's fence, or -1. */
static int syncobj_export(int dev, uint32_t handle)
{
    struct drm_syncobj_handle h;
    h.handle = handle;
    h.flags = DRM_SYNCOBJ_HANDLE_TO_FD_FLAGS_EXPORT_SYNC_FILE;
    h.fd = -1;
    h.pad = 0;
    if (syscall3(SYS_IOCTL, dev, DRM_IOCTL_SYNCOBJ_HANDLE_TO_FD, (long)&h))
        return -1;
    return h.fd;
}

/** Wait for a syncobj without a time limit: the ioctl's answer. */
static long syncobj_wait(int dev, uint32_t handle)
{
    struct drm_syncobj_wait w;
    unsigned char *p = (unsigned char *)&w;
    unsigned i;
    for (i = 0; i < sizeof(w); i++)
        p[i] = 0;
    w.handles = (uintptr_t)&handle;
    w.count_handles = 1;
    w.timeout_nsec = 0x7fffffffffffffffll;
    return syscall3(SYS_IOCTL, dev, DRM_IOCTL_SYNCOBJ_WAIT, (long)&w);
}

/** In a consumer: wait on the syncobj, then read the producer's error from
 * a sync file of its fence. */
static void syncobj_consume_error(int dev, uint32_t handle)
{
    int fd;
    if (syncobj_wait(dev, handle))
        child_exit(0);
    fd = syncobj_export(dev, handle);
    child_exit(fd >= 0 && status(fd, 0, 0) == -ENOENT);
}

/** A slow producer and a failing one behind syncobjs, binary and, where
 * the driver offers them, timeline points. */
static void syncobj_rows(int dev, int timelines)
{
    struct source udma;
    struct buffer b;
    uint32_t bin = syncobj(dev), tlh = 0, other;
    int tl, fence, rd, go, fd, i;
    udma.dev = keep(syscall3(SYS_OPEN, (long)"/dev/udmabuf", 2, 0));
    udma.card = 0;
    b = new_buffer(&udma);
    fill(b.map, SIZE, 0);
    tl = timeline();
    fence = point(tl, 1);
    syncobj_import(dev, bin, fence);
    if (timelines) {
        struct drm_syncobj_transfer t;
        unsigned char *p = (unsigned char *)&t;
        unsigned j;
        for (j = 0; j < sizeof(t); j++)
            p[j] = 0;
        tlh = syncobj(dev);
        t.src_handle = bin;
        t.dst_handle = tlh;
        t.dst_point = 1;
        check(syscall3(SYS_IOCTL, dev, DRM_IOCTL_SYNCOBJ_TRANSFER, (long)&t) == 0,
              "transfer to a timeline point");
    }
    rd = go_pipe(&go);
    if (!spawn())
        produce(rd, b.map, SIZE, 0x77, tl, SLOW);
    drop(tl);
    release(go, 1);
    check(syncobj_wait(dev, bin) == 0, "syncobj wait for the slow producer");
    fd = keep(syncobj_export(dev, bin));
    check(status(fd, 0, 0) == 1 && holds(b.map, SIZE, 0x77), "syncobj fence, then buffer");
    if (timelines) {
        struct drm_syncobj_timeline_wait w;
        struct drm_syncobj_timeline_array q;
        uint64_t pt = 1, reached = 0;
        unsigned char *p = (unsigned char *)&w;
        unsigned j;
        for (j = 0; j < sizeof(w); j++)
            p[j] = 0;
        w.handles = (uintptr_t)&tlh;
        w.points = (uintptr_t)&pt;
        w.count_handles = 1;
        w.timeout_nsec = 0x7fffffffffffffffll;
        check(syscall3(SYS_IOCTL, dev, DRM_IOCTL_SYNCOBJ_TIMELINE_WAIT, (long)&w) == 0,
              "timeline point wait");
        q.handles = (uintptr_t)&tlh;
        q.points = (uintptr_t)&reached;
        q.count_handles = 1;
        q.flags = 0;
        check(syscall3(SYS_IOCTL, dev, DRM_IOCTL_SYNCOBJ_QUERY, (long)&q) == 0 && reached == 1,
              "timeline reached point 1");
    }
    reap("the syncobj's producer exits");
    report_held();

    tl = timeline();
    fence = point(tl, 1);
    other = syncobj(dev);
    syncobj_import(dev, other, fence);
    rd = go_pipe(&go);
    if (!spawn())
        produce(rd, b.map, SIZE, 0, tl, SLOW | FAIL);
    drop(tl);
    for (i = 0; i < 3; i++)
        if (!spawn())
            syncobj_consume_error(dev, other);
    release(go, 1);
    check(syncobj_wait(dev, other) == 0, "a failed fence ends the syncobj wait");
    fd = keep(syncobj_export(dev, other));
    check(status(fd, 0, 0) == -ENOENT, "the syncobj's fence carries the error");
    reap("every syncobj consumer reads the producer's error");
    report_held();
}

void probe_main(unsigned long *sp);
PROBE_SP_ENTRY;

/** Run the rows over udmabuf, then over a card where one offers them. */
void probe_main(unsigned long *sp)
{
    struct source src;
    struct drm_get_cap c;
    int dumb, sync;
    report_begin(report_case((char **)(sp + 1) + sp[0] + 1, "guest.dmabuf.fence"));
    dumb = find_node(DRM_CAP_DUMB_BUFFER, 1);
    sync = find_node(DRM_CAP_SYNCOBJ, 1);
    report_branch("card", dumb >= 0 ? "dumb" : "none");
    report_branch("syncobj", sync >= 0 ? "offered" : "none");
    report_plan("slow");
    report_plan("concurrent");
    report_plan("failed");
    if (dumb >= 0)
        report_plan("dumb");
    if (sync >= 0) {
        report_plan("syncobj-wait");
        report_plan("syncobj-error");
    }
    src.dev = keep(syscall3(SYS_OPEN, (long)"/dev/udmabuf", 2, 0));
    src.card = 0;
    slow_rows(&src);
    report_held();
    concurrent_rows(&src);
    report_held();
    failed_rows(&src);
    report_held();
    cleanup();
    if (dumb >= 0) {
        dumb = find_node(DRM_CAP_DUMB_BUFFER, 1);
        src.dev = dumb;
        src.card = 1;
        slow_rows(&src);
        concurrent_rows(&src);
        failed_rows(&src);
        report_held();
        cleanup();
    }
    if (sync >= 0) {
        sync = find_node(DRM_CAP_SYNCOBJ, 1);
        c.capability = DRM_CAP_SYNCOBJ_TIMELINE;
        c.value = 0;
        syscall3(SYS_IOCTL, sync, DRM_IOCTL_GET_CAP, (long)&c);
        syncobj_rows(sync, c.value == 1);
    }
    cleanup();
    report_end();
    probe_line("FENCEPROBE OK", 0, 0);
    syscall1(SYS_EXIT, 0);
    for (;;) {
    }
}
