/*
 * ioctlcount.c: an LD_PRELOAD library that counts a process's virtio-gpu
 * ioctls by kind, into the shared file IOCTLCOUNT_FILE, for task 5676's
 * measurement of piglit's streaming-texture-leak.
 *
 *   cc -shared -fPIC -O2 -o ioctlcount.so ioctlcount.c -ldl
 *   IOCTLCOUNT_FILE=/tmp/counts LD_PRELOAD=./ioctlcount.so PROGRAM
 *
 * The file holds IC_N unsigned 64-bit counters in the host's order, which a
 * sampler reads while the program runs (`od -An -t u8`). With IOCTLCOUNT_LOG
 * set, each counted call is also recorded in that file, as four unsigned
 * 64-bit words: CLOCK_MONOTONIC's nanoseconds at its start, its duration in
 * nanoseconds, its number and its result, the calls in the order they
 * ended, up to IC_LOG_MAX of them; ioctllog.py reads it. Mesa's virgl
 * winsys reaches the kernel through libdrm's drmIoctl, which calls the C
 * library's ioctl, so wrapping that symbol sees every call.
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

/* The counters, in the file's order. */
enum {
    IC_EXECBUFFER,
    IC_WAIT,
    IC_WAIT_NOWAIT,
    IC_WAIT_BUSY,
    IC_RESOURCE_CREATE,
    IC_GEM_CLOSE,
    IC_TRANSFER_TO_HOST,
    IC_TRANSFER_FROM_HOST,
    IC_CREATE_BLOB,
    IC_N
};

/* DRM's ioctl type and the virtio-gpu commands' numbers, DRM_COMMAND_BASE
 * (0x40) plus Linux's DRM_VIRTGPU_*; GEM_CLOSE is DRM's own 0x09. */
#define DRM_TYPE            'd'
#define NR_GEM_CLOSE        0x09
#define NR_EXECBUFFER       0x42
#define NR_RESOURCE_CREATE  0x44
#define NR_TRANSFER_FROM    0x46
#define NR_TRANSFER_TO      0x47
#define NR_WAIT             0x48
#define NR_CREATE_BLOB      0x4a
#define VIRTGPU_WAIT_NOWAIT 1u

/* The most calls the log records, 64 MiB of records. */
#define IC_LOG_MAX (2u << 20)

static uint64_t ic_local[IC_N];
static uint64_t (*ic_log)[4];
static uint64_t ic_logged;
static uint64_t *ic = ic_local;
static int (*real_ioctl)(int, unsigned long, ...);

/** Map IOCTLCOUNT_FILE, when it is set, as the counters. */
__attribute__((constructor)) static void ic_init(void)
{
    const char *path = getenv("IOCTLCOUNT_FILE");
    void *p;
    int fd;

    real_ioctl = (int (*)(int, unsigned long, ...))dlsym(RTLD_NEXT, "ioctl");
    if ((path = getenv("IOCTLCOUNT_LOG")) != NULL && (fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0644)) >= 0) {
        if (ftruncate(fd, (off_t)IC_LOG_MAX * sizeof(*ic_log)) == 0) {
            p = mmap(NULL, (size_t)IC_LOG_MAX * sizeof(*ic_log), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
            if (p != MAP_FAILED)
                ic_log = (uint64_t (*)[4])p;
        }
        close(fd);
    }
    path = getenv("IOCTLCOUNT_FILE");
    if (path == NULL || (fd = open(path, O_RDWR | O_CREAT, 0644)) < 0)
        return;
    if (ftruncate(fd, sizeof(ic_local)) == 0) {
        p = mmap(NULL, sizeof(ic_local), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        if (p != MAP_FAILED)
            ic = (uint64_t *)p;
    }
    close(fd);
}

static void ic_add(int which)
{
    __atomic_add_fetch(&ic[which], 1, __ATOMIC_RELAXED);
}

static uint64_t ic_now(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000u + (uint64_t)ts.tv_nsec;
}

/** Record a counted call that began at `t0` in the log, if there is one. */
static void ic_record(uint64_t t0, unsigned nr, int rc, int err)
{
    uint64_t i;

    if (ic_log == NULL)
        return;
    i = __atomic_fetch_add(&ic_logged, 1, __ATOMIC_RELAXED);
    if (i >= IC_LOG_MAX)
        return;
    ic_log[i][1] = ic_now() - t0;
    ic_log[i][2] = nr;
    ic_log[i][3] = (uint64_t)(int64_t)(rc < 0 ? -err : rc);
    __atomic_store_n(&ic_log[i][0], t0, __ATOMIC_RELEASE);
}

/** The C library's ioctl, counting the calls of the kinds above. */
int ioctl(int fd, unsigned long req, ...)
{
    va_list ap;
    void *arg;
    int rc, err;
    unsigned nr = (unsigned)(req & 0xff);
    uint64_t t0;

    va_start(ap, req);
    arg = va_arg(ap, void *);
    va_end(ap);
    if (real_ioctl == NULL)
        real_ioctl = (int (*)(int, unsigned long, ...))dlsym(RTLD_NEXT, "ioctl");
    t0 = ic_log != NULL ? ic_now() : 0;
    rc = real_ioctl(fd, req, arg);
    err = errno;
    if (((req >> 8) & 0xff) == DRM_TYPE) {
        switch (nr) {
        case NR_EXECBUFFER:
            ic_add(IC_EXECBUFFER);
            break;
        case NR_WAIT:
            ic_add(IC_WAIT);
            if (arg != NULL && (((const uint32_t *)arg)[1] & VIRTGPU_WAIT_NOWAIT))
                ic_add(IC_WAIT_NOWAIT);
            if (rc < 0 && err == EBUSY)
                ic_add(IC_WAIT_BUSY);
            break;
        case NR_RESOURCE_CREATE:
            ic_add(IC_RESOURCE_CREATE);
            break;
        case NR_GEM_CLOSE:
            ic_add(IC_GEM_CLOSE);
            break;
        case NR_TRANSFER_TO:
            ic_add(IC_TRANSFER_TO_HOST);
            break;
        case NR_TRANSFER_FROM:
            ic_add(IC_TRANSFER_FROM_HOST);
            break;
        case NR_CREATE_BLOB:
            ic_add(IC_CREATE_BLOB);
            break;
        default:
            nr = 0;
        }
        if (nr != 0)
            ic_record(t0, nr, rc, err);
    }
    errno = err;
    return rc;
}
