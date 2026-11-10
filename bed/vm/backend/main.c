/*
 * main.c: the VM's virtio-gpu backend for Rach2's shared-memory transport:
 * the region from the vgpushm module, from vfio-pci or from a file, its
 * doorbells as eventfds, and the loop that serves the device.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <linux/vfio.h>

#include "backend.h"
#include "vgpushm_uapi.h"

#define VECTORS 2

/* Where the region and its doorbells come from. */
struct src {
    volatile uint8_t *base;
    uint64_t size;
    uint32_t peer;    /* the VM's ivshmem peer id, or ~0 */
    int efd[VECTORS]; /* each vector's eventfd, or -1 when polling */
    int dev;          /* the module's node, or -1 */
    int vfio;         /* the vfio device, or -1 */
    uint64_t bar0;    /* the registers' offset in the vfio device */
    uint64_t wakes[VECTORS], polled;
};

static volatile sig_atomic_t stop;

/** Note SIGINT or SIGTERM; the loop ends at its next turn. */
static void on_signal(int sig)
{
    (void)sig;
    stop = 1;
}

/** Print a message and exit with status 1. */
static void die(const char *what, const char *arg)
{
    fprintf(stderr, "backend: %s%s%s\n", what, arg ? ": " : "", arg ? arg : "");
    exit(1);
}

/** Make an eventfd for each vector. */
static void make_eventfds(struct src *s)
{
    int i;

    for (i = 0; i < VECTORS; i++) {
        s->efd[i] = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
        if (s->efd[i] < 0) {
            die("cannot make an eventfd", strerror(errno));
        }
    }
}

/**
 * Take the region and vectors of the vgpushm module's node: the region
 * mapped, and an eventfd bound to each vector when the device is an
 * ivshmem-doorbell; an ivshmem-plain device leaves the backend polling.
 */
static void open_module(struct src *s, const char *path)
{
    struct vgpushm_info info;
    struct vgpushm_eventfd b;
    void *p;
    int i;

    if ((s->dev = open(path, O_RDWR | O_CLOEXEC)) < 0) {
        die("cannot open the module's node", path);
    }
    if (ioctl(s->dev, VGPUSHM_IOC_INFO, &info) != 0) {
        die("VGPUSHM_IOC_INFO failed", strerror(errno));
    }
    p = mmap(0, info.size, PROT_READ | PROT_WRITE, MAP_SHARED, s->dev, 0);
    if (p == MAP_FAILED) {
        die("cannot map the region", strerror(errno));
    }
    s->base = p;
    s->size = info.size;
    s->peer = info.peer;
    if (info.vectors < VECTORS) {
        return;
    }
    make_eventfds(s);
    for (i = 0; i < VECTORS; i++) {
        b.vector = (uint32_t)i;
        b.fd = s->efd[i];
        if (ioctl(s->dev, VGPUSHM_IOC_EVENTFD, &b) != 0) {
            die("VGPUSHM_IOC_EVENTFD failed", strerror(errno));
        }
    }
}

/** Return the vfio region's info for index i of the device. */
static struct vfio_region_info vfio_region(struct src *s, uint32_t i)
{
    struct vfio_region_info r;

    memset(&r, 0, sizeof(r));
    r.argsz = sizeof(r);
    r.index = i;
    if (ioctl(s->vfio, VFIO_DEVICE_GET_REGION_INFO, &r) != 0) {
        die("VFIO_DEVICE_GET_REGION_INFO failed", strerror(errno));
    }
    return r;
}

/** Open the device's IOMMU group in a new type-1 container; return the group. */
static int vfio_group(const char *bdf)
{
    char link[PATH_MAX], target[PATH_MAX], path[PATH_MAX + 16];
    struct vfio_group_status st = {sizeof(st), 0};
    const char *n;
    ssize_t len;
    int container, group;

    snprintf(link, sizeof(link), "/sys/bus/pci/devices/%s/iommu_group", bdf);
    if ((len = readlink(link, target, sizeof(target) - 1)) < 0) {
        die("the device has no IOMMU group", bdf);
    }
    target[len] = 0;
    n = strrchr(target, '/');
    snprintf(path, sizeof(path), "/dev/vfio/%s", n ? n + 1 : target);
    container = open("/dev/vfio/vfio", O_RDWR | O_CLOEXEC);
    group = open(path, O_RDWR | O_CLOEXEC);
    if (container < 0 || group < 0) {
        die("cannot open vfio", path);
    }
    if (ioctl(container, VFIO_GET_API_VERSION) != VFIO_API_VERSION ||
        !ioctl(container, VFIO_CHECK_EXTENSION, VFIO_TYPE1v2_IOMMU) ||
        ioctl(group, VFIO_GROUP_GET_STATUS, &st) != 0 || !(st.flags & VFIO_GROUP_FLAGS_VIABLE) ||
        ioctl(group, VFIO_GROUP_SET_CONTAINER, &container) != 0 ||
        ioctl(container, VFIO_SET_IOMMU, VFIO_TYPE1v2_IOMMU) != 0) {
        die("cannot set up the vfio group", path);
    }
    return group;
}

/** Turn the device's bus mastering on, which its MSI-X writes need. */
static void vfio_master(struct src *s)
{
    struct vfio_region_info cfg = vfio_region(s, VFIO_PCI_CONFIG_REGION_INDEX);
    uint16_t cmd;

    if (pread(s->vfio, &cmd, 2, (off_t)cfg.offset + 4) != 2) {
        die("cannot read the command register", strerror(errno));
    }
    cmd |= 4;
    if (pwrite(s->vfio, &cmd, 2, (off_t)cfg.offset + 4) != 2) {
        die("cannot write the command register", strerror(errno));
    }
}

/**
 * Take the region and vectors of the ivshmem device bdf through vfio-pci,
 * under QEMU's emulated IOMMU: BAR 2 mapped, the registers of BAR 0 read
 * and written through the device, and the two MSI-X vectors as eventfds.
 */
static void open_vfio(struct src *s, const char *bdf)
{
    char buf[sizeof(struct vfio_irq_set) + VECTORS * sizeof(int)];
    struct vfio_irq_set *set = (struct vfio_irq_set *)buf;
    struct vfio_region_info shm;
    uint32_t peer;
    void *p;
    int group = vfio_group(bdf);

    if ((s->vfio = ioctl(group, VFIO_GROUP_GET_DEVICE_FD, bdf)) < 0) {
        die("VFIO_GROUP_GET_DEVICE_FD failed", strerror(errno));
    }
    s->bar0 = vfio_region(s, VFIO_PCI_BAR0_REGION_INDEX).offset;
    shm = vfio_region(s, VFIO_PCI_BAR2_REGION_INDEX);
    if (!(shm.flags & VFIO_REGION_INFO_FLAG_MMAP)) {
        die("vfio cannot map BAR 2 of", bdf);
    }
    p = mmap(0, shm.size, PROT_READ | PROT_WRITE, MAP_SHARED, s->vfio, (off_t)shm.offset);
    if (p == MAP_FAILED) {
        die("cannot map BAR 2", strerror(errno));
    }
    s->base = p;
    s->size = shm.size;
    vfio_master(s);
    if (pread(s->vfio, &peer, 4, (off_t)s->bar0 + 8) != 4) {
        die("cannot read IVPosition", strerror(errno));
    }
    s->peer = peer;
    make_eventfds(s);
    set->argsz = sizeof(buf);
    set->flags = VFIO_IRQ_SET_DATA_EVENTFD | VFIO_IRQ_SET_ACTION_TRIGGER;
    set->index = VFIO_PCI_MSIX_IRQ_INDEX;
    set->start = 0;
    set->count = VECTORS;
    memcpy(set->data, s->efd, VECTORS * sizeof(int));
    if (ioctl(s->vfio, VFIO_DEVICE_SET_IRQS, set) != 0) {
        die("VFIO_DEVICE_SET_IRQS failed", strerror(errno));
    }
}

/** Map a file shared with the driver another way; the backend polls. */
static void open_file(struct src *s, const char *path)
{
    struct stat st;
    void *p;
    int fd = open(path, O_RDWR | O_CLOEXEC);

    if (fd < 0 || fstat(fd, &st) != 0) {
        die("cannot open the region", path);
    }
    p = mmap(0, (size_t)st.st_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (p == MAP_FAILED) {
        die("cannot map the region", strerror(errno));
    }
    close(fd);
    s->base = p;
    s->size = (uint64_t)st.st_size;
}

/** Ring the driver's vector through the module or vfio. */
static void ring(void *cookie, uint32_t peer, uint32_t vector)
{
    struct src *s = cookie;
    uint32_t v = VGS_DOORBELL(peer, vector);

    if (s->dev >= 0) {
        ioctl(s->dev, VGPUSHM_IOC_RING, &v);
    } else if (s->vfio >= 0) {
        if (pwrite(s->vfio, &v, 4, (off_t)s->bar0 + 12) != 4) {
            fprintf(stderr, "backend: doorbell write failed: %s\n", strerror(errno));
        }
    }
}

/* The options. */
struct opts {
    const char *module, *vfio, *file, *card, *gbm;
    int direct, writeback, venus, once, verbose;
    long peer;
};

/** Print the usage and exit with status 2. */
static void usage(void)
{
    fprintf(stderr, "usage: vgpu-backend [-m NODE | -V BDF | -f FILE] [-k CARD] [-g DEV] "
                    "[-d] [-w] [-x] [-p PEER] [-1] [-v]\n");
    exit(2);
}

/** Read the options into o. */
static void options(int argc, char **argv, struct opts *o)
{
    int c;

    memset(o, 0, sizeof(*o));
    o->module = "/dev/vgpushm0";
    o->venus = 1;
    o->peer = -1;
    while ((c = getopt(argc, argv, "m:V:f:k:g:dwxp:1v")) != -1) {
        switch (c) {
        case 'm':
            o->module = optarg;
            break;
        case 'V':
            o->vfio = optarg;
            break;
        case 'f':
            o->file = optarg;
            break;
        case 'k':
            o->card = optarg;
            break;
        case 'g':
            o->gbm = optarg;
            break;
        case 'd':
            o->direct = 1;
            break;
        case 'w':
            o->writeback = 1;
            break;
        case 'x':
            o->venus = 0;
            break;
        case 'p':
            o->peer = strtol(optarg, 0, 0);
            break;
        case '1':
            o->once = 1;
            break;
        case 'v':
            o->verbose = 1;
            break;
        default:
            usage();
        }
    }
    if (optind != argc || o->peer > 0xffff) {
        usage();
    }
}

/** Fix the offer: features, limits, the peer and the capsets. */
static void make_offer(struct vd_offer *f, const struct src *s, const struct opts *o, struct vr *r,
                       int edid)
{
    static const struct vgs_limits lim = {512, 64, 0, 1024, VGS_CHAIN_MAX, 1u << 20, 256, 0};

    memset(f, 0, sizeof(*f));
    f->limits = lim;
    f->limits.max_backing = s->size;
    f->features = (1ULL << VD_F_VIRGL) | (1ULL << VD_F_BLOB) | (1ULL << VD_F_CONTEXT_INIT);
    if (edid) {
        f->features |= 1ULL << VD_F_EDID;
    }
    f->transport_features = s->efd[0] >= 0 ? VGS_TF_DOORBELL : 0;
    f->peer = o->peer >= 0 ? (uint32_t)o->peer : s->peer <= 0xffff ? s->peer : 1;
    f->num_scanouts = 1;
    f->num_capsets = vr_capsets(r);
}

/** Read and clear a vector's eventfd; return whether it had rung. */
static int drain(int fd)
{
    uint64_t v;

    return read(fd, &v, sizeof(v)) == sizeof(v);
}

/** Print what the run did, one `backend: result:` line. */
static void report(struct vd *d, struct src *s, struct kms *k)
{
    struct vgpushm_counts mc;

    fprintf(stderr,
            "backend: result: epochs %llu, commands %llu, errors %llu, fenced %llu, "
            "completed %llu, notified %llu, faults %llu, wakes %llu+%llu, polled %llu\n",
            (unsigned long long)d->n.epochs, (unsigned long long)d->n.commands,
            (unsigned long long)d->n.errors, (unsigned long long)d->n.fenced,
            (unsigned long long)d->n.completed, (unsigned long long)d->n.notified,
            (unsigned long long)d->n.faults, (unsigned long long)s->wakes[0],
            (unsigned long long)s->wakes[1], (unsigned long long)s->polled);
    if (s->dev >= 0 && ioctl(s->dev, VGPUSHM_IOC_COUNTS, &mc) == 0) {
        fprintf(stderr, "backend: result: module irqs %llu+%llu, signals %llu+%llu, rings %llu\n",
                (unsigned long long)mc.irqs[0], (unsigned long long)mc.irqs[1],
                (unsigned long long)mc.signals[0], (unsigned long long)mc.signals[1],
                (unsigned long long)mc.rings);
    }
    if (k) {
        const struct kms_counts *n = kms_counts(k);

        fprintf(stderr,
                "backend: result: kms flushes %llu, copies %llu, direct %llu, modesets %llu, "
                "flips %llu, skipped %llu, writebacks %llu\n",
                (unsigned long long)n->flushes, (unsigned long long)n->copies,
                (unsigned long long)n->direct, (unsigned long long)n->modesets,
                (unsigned long long)n->flips, (unsigned long long)n->skipped,
                (unsigned long long)n->writebacks);
    }
}

/**
 * Serve the device until the driver detaches with -1, or a signal: step
 * until idle, then wait on the doorbells and virglrenderer's fences. A
 * transport with doorbells sleeps until one rings; without, it polls each
 * millisecond. A step that finds work after a wait no doorbell ended
 * counts as polled.
 */
static void serve(struct vd *d, struct src *s, struct vr *r, int once)
{
    struct pollfd p[VECTORS + 1];
    int i, ret = VD_IDLE, rung = 1, timeout;

    for (i = 0; i < VECTORS; i++) {
        p[i].fd = s->efd[i];
        p[i].events = POLLIN;
    }
    p[VECTORS].fd = vr_poll_fd(r);
    p[VECTORS].events = POLLIN;
    while (!stop) {
        int steps = 0;

        while ((ret = vd_step(d)) == VD_WORKED && steps++ < 64) {
            if (!rung && d->me.state == VGS_DEV_RUNNING) {
                s->polled++;
            }
            rung = 1;
        }
        if (ret == VD_GONE && once) {
            break;
        }
        vr_poll(r);
        vd_kick(d);
        timeout = d->npend ? 1 : 1000;
        if (s->efd[0] < 0 || !(d->drv.transport_features & VGS_TF_DOORBELL) ||
            d->me.state != VGS_DEV_RUNNING) {
            timeout = 1;
        }
        rung = 0;
        if (poll(p, VECTORS + 1, timeout) <= 0) {
            continue;
        }
        for (i = 0; i < VECTORS; i++) {
            if ((p[i].revents & POLLIN) && drain(p[i].fd)) {
                s->wakes[i]++;
                rung = 1;
            }
        }
        if (p[VECTORS].revents & POLLIN) {
            rung = 1;
        }
    }
}

/**
 * Serve Rach2's virtio-gpu from the VM: take the region, start the
 * renderer and the display, and run the device until told to stop.
 */
int main(int argc, char **argv)
{
    static struct vd dev;
    struct src s = {0, 0, ~0u, {-1, -1}, -1, -1, 0, {0, 0}, 0};
    struct vd_offer off;
    struct opts o;
    struct kms *k = 0;
    struct vr *r;
    int gbm, card = -1;

    options(argc, argv, &o);
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    if (o.vfio) {
        open_vfio(&s, o.vfio);
    } else if (o.file) {
        open_file(&s, o.file);
    } else {
        open_module(&s, o.module);
    }
    /* One open of the card: its first descriptor is DRM's master, and the
       GBM device virglrenderer gets is a duplicate of it. */
    if (o.card && (card = open(o.card, O_RDWR | O_CLOEXEC)) < 0) {
        die("cannot open the display", o.card);
    }
    gbm = o.gbm ? open(o.gbm, O_RDWR | O_CLOEXEC) : card;
    if (o.gbm && gbm < 0) {
        die("cannot open the GBM device", o.gbm);
    }
    if ((r = vr_init(&dev, gbm, o.venus, o.verbose)) == 0) {
        return 1;
    }
    if (o.card && (k = kms_open(card, o.card, r, o.direct, o.writeback)) == 0) {
        return 1;
    }
    vr_set_kms(r, k);
    make_offer(&off, &s, &o, r, k && kms_edid(k, dev.resp, &(uint32_t){0}) == 0);
    if (vd_init(&dev, s.base, s.size, &off, &vr_ops, r) != 0) {
        die("the offer breaks the transport's rules", 0);
    }
    dev.ring = ring;
    dev.ring_cookie = &s;
    dev.verbose = o.verbose;
    fprintf(stderr, "backend: serving %llu bytes as peer %u, %s, %u capsets, %s\n",
            (unsigned long long)s.size, (unsigned)off.peer,
            off.transport_features ? "doorbells" : "polling", (unsigned)off.num_capsets,
            k ? "on the display" : "no display");
    serve(&dev, &s, r, o.once);
    report(&dev, &s, k);
    kms_close(k);
    vd_fini(&dev);
    return 0;
}

/** Return the CRC-32 of n bytes, as zlib's crc32 computes it. */
uint32_t crc32_bytes(const uint8_t *p, uint64_t n)
{
    uint32_t c = 0xffffffffu;
    uint64_t i;
    int k;

    for (i = 0; i < n; i++) {
        c ^= p[i];
        for (k = 0; k < 8; k++) {
            c = (c >> 1) ^ (0xedb88320u & (0u - (c & 1u)));
        }
    }
    return c ^ 0xffffffffu;
}
