/*
 * ioctlcount.c: an LD_PRELOAD library that counts a process's virtio-gpu
 * ioctls by kind, into the shared file IOCTLCOUNT_FILE, for task 5676's
 * measurement of piglit's streaming-texture-leak.
 *
 *   cc -shared -fPIC -O2 -o ioctlcount.so ioctlcount.c -ldl
 *   IOCTLCOUNT_FILE=/tmp/counts LD_PRELOAD=./ioctlcount.so PROGRAM
 *
 * The file holds IC_N unsigned 64-bit counters in the host's order, which a
 * sampler reads while the program runs (`od -An -t u8`). Mesa's virgl
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

static uint64_t ic_local[IC_N];
static uint64_t *ic = ic_local;
static int (*real_ioctl)(int, unsigned long, ...);

/** Map IOCTLCOUNT_FILE, when it is set, as the counters. */
__attribute__((constructor)) static void ic_init(void)
{
    const char *path = getenv("IOCTLCOUNT_FILE");
    void *p;
    int fd;

    real_ioctl = (int (*)(int, unsigned long, ...))dlsym(RTLD_NEXT, "ioctl");
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

/** The C library's ioctl, counting the calls of the kinds above. */
int ioctl(int fd, unsigned long req, ...)
{
    va_list ap;
    void *arg;
    int rc, err;
    unsigned nr = (unsigned)(req & 0xff);

    va_start(ap, req);
    arg = va_arg(ap, void *);
    va_end(ap);
    if (real_ioctl == NULL)
        real_ioctl = (int (*)(int, unsigned long, ...))dlsym(RTLD_NEXT, "ioctl");
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
        }
    }
    errno = err;
    return rc;
}
