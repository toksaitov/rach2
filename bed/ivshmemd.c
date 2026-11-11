/*
 * ivshmemd.c: QEMU's ivshmem server on Rach2, which makes the region the
 * virtio-gpu transport shares with a VM and hands it, with the doorbells, to
 * the kernel through /dev/vgpu-shm.
 */

#define _GNU_SOURCE /* memfd_create, F_ADD_SEALS */

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "../rach2/sys/virtio/vgpushm.h"

static const char *node = "/dev/vgpu-shm";
static volatile sig_atomic_t stopping;

/** Print `component: detail` with the error and exit 1. */
static void die(const char *what, const char *arg)
{
    fprintf(stderr, "ivshmemd: %s%s%s: %s\n", what, arg ? " " : "", arg ? arg : "",
            strerror(errno));
    exit(1);
}

/** Note a signal to stop; the main loop detaches in order. */
static void on_signal(int sig)
{
    (void)sig;
    stopping = 1;
}

/** Send one message of ivshmem's server protocol, a 64-bit value with the
 * descriptor `fd` unless it is -1. */
static void ivshmem_send(int conn, int64_t v, int fd)
{
    char ctl[CMSG_SPACE(sizeof(int))];
    struct iovec iov = {&v, sizeof(v)};
    struct msghdr m;
    struct cmsghdr *c;

    memset(&m, 0, sizeof(m));
    memset(ctl, 0, sizeof(ctl));
    m.msg_iov = &iov;
    m.msg_iovlen = 1;
    if (fd >= 0) {
        m.msg_control = ctl;
        m.msg_controllen = sizeof(ctl);
        c = CMSG_FIRSTHDR(&m);
        c->cmsg_level = SOL_SOCKET;
        c->cmsg_type = SCM_RIGHTS;
        c->cmsg_len = CMSG_LEN(sizeof(int));
        memcpy(CMSG_DATA(c), &fd, sizeof(int));
    }
    if (sendmsg(conn, &m, MSG_NOSIGNAL) != (ssize_t)sizeof(v)) {
        die("cannot write to QEMU", NULL);
    }
}

/** Print the transport's report of the region, as VGS_IOC_INFO gives it. */
static int report(int ctl, const char *when)
{
    struct vgs_info in;

    if (ioctl(ctl, VGS_IOC_INFO, &in) != 0) {
        die("info", node);
    }
    printf("ivshmemd: %s: attached %u, bound %u, epoch %u, driver %u, device %u, error %u, "
           "doorbells %u, binds %u, rung %llu/%llu, woken %llu/%llu, polls %llu, heap %llu of "
           "%llu pages, refused %llu, maps %llu, map_pages %llu, map_live %llu, revoked %llu\n",
           when, in.attached, in.bound, in.epoch, in.drv_state, in.dev_state, in.dev_error,
           in.doorbells, in.binds, (unsigned long long)in.rung[0], (unsigned long long)in.rung[1],
           (unsigned long long)in.woken[0], (unsigned long long)in.woken[1],
           (unsigned long long)in.polls, (unsigned long long)in.heap_used,
           (unsigned long long)in.heap_pages, (unsigned long long)in.refused,
           (unsigned long long)in.maps, (unsigned long long)in.map_pages,
           (unsigned long long)in.map_live, (unsigned long long)in.revoked);
    fflush(stdout);
    return 0;
}

/**
 * Make the region, a memfd of `size` bytes sealed against shrinking and
 * growing, and the two vectors of each peer, and attach them as peer 0:
 * the kernel waits on `host`, which the VM rings, and rings `vm`, which
 * QEMU delivers to the VM. Returns the control node's descriptor, which
 * holds the attach; the attach is refused when a region is attached.
 */
static int attach(uint64_t size, int *memfd, int host[2], int vm[2])
{
    struct vgs_attach a;
    int ctl, i;

    *memfd = memfd_create("vgpushm", MFD_CLOEXEC | MFD_ALLOW_SEALING);
    if (*memfd < 0 || ftruncate(*memfd, (off_t)size) != 0 ||
        fcntl(*memfd, F_ADD_SEALS, F_SEAL_SHRINK | F_SEAL_GROW) != 0) {
        die("cannot make the region", NULL);
    }
    for (i = 0; i < 2; i++) {
        host[i] = eventfd(0, EFD_CLOEXEC);
        vm[i] = eventfd(0, EFD_CLOEXEC);
        if (host[i] < 0 || vm[i] < 0) {
            die("cannot make an eventfd", NULL);
        }
    }
    if ((ctl = open(node, O_RDWR | O_CLOEXEC)) < 0) {
        die("cannot open", node);
    }
    memset(&a, 0, sizeof(a));
    a.memfd = *memfd;
    a.bell_in[0] = host[0];
    a.bell_in[1] = host[1];
    a.bell_out[0] = vm[0];
    a.bell_out[1] = vm[1];
    a.peer = 0;
    if (ioctl(ctl, VGS_IOC_ATTACH, &a) != 0) {
        die("attach: refused", node);
    }
    printf("ivshmemd: attached: %llu bytes as peer 0\n", (unsigned long long)size);
    fflush(stdout);
    return ctl;
}

/** Listen on the Unix socket at `path`, replacing a stale one. */
static int listen_at(const char *path)
{
    struct sockaddr_un a;
    int ls;

    memset(&a, 0, sizeof(a));
    a.sun_family = AF_UNIX;
    if (strlen(path) >= sizeof(a.sun_path)) {
        errno = ENAMETOOLONG;
        die("socket path", path);
    }
    strcpy(a.sun_path, path);
    unlink(path);
    if ((ls = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0)) < 0 ||
        bind(ls, (struct sockaddr *)&a, sizeof(a)) != 0 || listen(ls, 1) != 0) {
        die("cannot listen on", path);
    }
    printf("ivshmemd: listening on %s\n", path);
    fflush(stdout);
    return ls;
}

/**
 * Serve one QEMU, as QEMU's docs/specs/ivshmem-spec.rst has the protocol:
 * the version 0, its id 1, the region, peer 0's vectors and its own; then
 * wait for its connection to close, which is the VM's death, or for a
 * signal. Returns VGS_DETACH_LOST or VGS_DETACH_ORDERLY.
 */
static uint32_t serve(int ls, int memfd, const int host[2], const int vm[2])
{
    struct pollfd p;
    char buf[64];
    int conn, i;

    p.fd = ls;
    p.events = POLLIN;
    while (!stopping && poll(&p, 1, -1) != 1) {
    }
    if (stopping) {
        return VGS_DETACH_ORDERLY;
    }
    if ((conn = accept4(ls, NULL, NULL, SOCK_CLOEXEC)) < 0) {
        die("accept", NULL);
    }
    ivshmem_send(conn, 0, -1);
    ivshmem_send(conn, 1, -1);
    ivshmem_send(conn, -1, memfd);
    for (i = 0; i < 2; i++) {
        ivshmem_send(conn, 0, host[i]);
    }
    for (i = 0; i < 2; i++) {
        ivshmem_send(conn, 1, vm[i]);
    }
    printf("ivshmemd: QEMU connected as peer 1\n");
    fflush(stdout);
    p.fd = conn;
    p.events = POLLIN;
    while (!stopping) {
        if (poll(&p, 1, -1) != 1) {
            continue;
        }
        if (read(conn, buf, sizeof(buf)) <= 0) {
            close(conn);
            return VGS_DETACH_LOST;
        }
    }
    close(conn);
    return VGS_DETACH_ORDERLY;
}

/** Print the usage and exit 2. */
static void usage(void)
{
    fprintf(stderr, "usage: ivshmemd [-n NODE] [-s SIZE] SOCKET\n"
                    "       ivshmemd [-n NODE] -i\n");
    exit(2);
}

/** Parse the options, attach the region and serve one QEMU; exit 0 once
 * detached, 1 for a failure and 2 for invalid options. */
int main(int argc, char **argv)
{
    uint64_t size = 32u << 20;
    int memfd, host[2], vm[2], ctl, ls, opt, info = 0;
    uint32_t how;
    struct sigaction sa;
    char *end;

    while ((opt = getopt(argc, argv, "in:s:")) != -1) {
        switch (opt) {
        case 'i':
            info = 1;
            break;
        case 'n':
            node = optarg;
            break;
        case 's':
            size = strtoull(optarg, &end, 0);
            if (*end == 'K' || *end == 'k') {
                size <<= 10, end++;
            } else if (*end == 'M' || *end == 'm') {
                size <<= 20, end++;
            }
            if (*end != 0 || size < VGS_REGION_MIN || size > VGS_ATTACH_MAX ||
                (size & (size - 1)) != 0) {
                fprintf(stderr, "ivshmemd: refused: size %s: a power of two from 32K to 64M\n",
                        optarg);
                exit(2);
            }
            break;
        default:
            usage();
        }
    }
    if (info) {
        if (optind != argc || (ctl = open(node, O_RDONLY | O_CLOEXEC)) < 0) {
            optind != argc ? usage() : die("cannot open", node);
        }
        return report(ctl, "info");
    }
    if (optind + 1 != argc) {
        usage();
    }
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_signal;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);
    ctl = attach(size, &memfd, host, vm);
    ls = listen_at(argv[optind]);
    how = serve(ls, memfd, host, vm);
    close(ls);
    unlink(argv[optind]);
    report(ctl, "before the detach");
    if (ioctl(ctl, VGS_IOC_DETACH, &how) != 0) {
        die("detach", node);
    }
    printf("ivshmemd: result: detached: %s\n",
           how == VGS_DETACH_LOST ? "QEMU closed its connection" : "in order");
    return 0;
}
