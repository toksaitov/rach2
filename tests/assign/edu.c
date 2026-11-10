/* Force an EDU DMA fault or a forged MSI through a real VFIO domain. */
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/io.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#define _KINT_H_
#include "../../rach2/sys/vfio/vfio_abi.h"

struct device {
    int container, group, fd, event;
    unsigned key, cap;
    uint32_t msi[3];
    volatile uint32_t *bar;
    unsigned char *guard;
    int raw_msi;
};

/** Read one counter from a fresh snapshot; exit on a missing counter. */
static unsigned long counter(const char *name)
{
    FILE *f = fopen("/proc/stat", "r");
    char line[256], key[80];
    unsigned long value;

    if (f != NULL) {
        while (fgets(line, sizeof(line), f) != NULL)
            if (sscanf(line, "%79s %lu", key, &value) == 2 && strcmp(key, name) == 0) {
                fclose(f);
                return value;
            }
        fclose(f);
    }
    fprintf(stderr, "assign-edu: missing counter: %s\n", name);
    exit(1);
}

/** Read the real PCI configuration dword, bypassing VFIO's MSI shadow. */
static uint32_t config_read(struct device *d, unsigned at)
{
    outl(0x80000000u | d->key << 8 | (at & ~3u), 0xcf8);
    return inl(0xcfc);
}

/** Write the real PCI configuration dword for a deliberately forged MSI. */
static void config_write(struct device *d, unsigned at, uint32_t value)
{
    outl(0x80000000u | d->key << 8 | (at & ~3u), 0xcf8);
    outl(value, 0xcfc);
}

/** Restore the real MSI before VFIO tears down its interrupt, then close. */
static void close_device(struct device *d)
{
    unsigned i;

    if (d->raw_msi)
        for (i = 0; i < 3; i++)
            config_write(d, d->cap + 4 + i * 4, d->msi[i]);
    if (d->bar != MAP_FAILED)
        munmap((void *)d->bar, 0x100000);
    if (d->fd >= 0)
        close(d->fd);
    if (d->event >= 0)
        close(d->event);
    if (d->group >= 0)
        close(d->group);
    if (d->container >= 0)
        close(d->container);
    if (d->guard != MAP_FAILED)
        munmap(d->guard, 4096);
}

/** Report a failed system call or assertion, retaining errno for diagnosis. */
static int error(const char *what)
{
    fprintf(stderr, "assign-edu: fail: %s: errno %d\n", what, errno);
    return -1;
}

/** Open the function through a Type1v2 container and enable its memory BAR. */
static int open_device(struct device *d, const char *bdf)
{
    char path[160], link[160];
    ssize_t n;
    unsigned bus, slot, fn;
    uint16_t command = 6;

    if (sscanf(bdf, "0000:%x:%x.%x", &bus, &slot, &fn) != 3)
        return error("device name");
    d->key = bus << 8 | slot << 3 | fn;
    snprintf(path, sizeof(path), "/sys/bus/pci/devices/%s/iommu_group", bdf);
    n = readlink(path, link, sizeof(link) - 1);
    if (n < 0)
        return error("group link");
    link[n] = 0;
    if (strrchr(link, '/') == NULL)
        return error("group link target");
    snprintf(path, sizeof(path), "/dev/vfio/%s", strrchr(link, '/') + 1);
    d->container = open("/dev/vfio/vfio", O_RDWR);
    d->group = open(path, O_RDWR);
    if (d->container < 0 || d->group < 0 ||
        ioctl(d->group, VFIO_GROUP_SET_CONTAINER, &d->container) < 0 ||
        ioctl(d->container, VFIO_SET_IOMMU, VFIO_TYPE1v2_IOMMU) < 0)
        return error("container");
    d->fd = ioctl(d->group, VFIO_GROUP_GET_DEVICE_FD, bdf);
    if (d->fd < 0 || pwrite(d->fd, &command, 2, ((off_t)7 << 40) + 4) != 2)
        return error("device configuration");
    d->bar = mmap(NULL, 0x100000, PROT_READ | PROT_WRITE, MAP_SHARED, d->fd, 0);
    if (d->bar == MAP_FAILED || (d->bar[0] & 0xffff) != 0xed)
        return error("EDU BAR");
    return 0;
}

/** Run an EDU transfer and wait on its hardware completion bit. */
static void dma(struct device *d, uint64_t src, uint64_t dst, uint32_t command)
{
    volatile uint64_t *regs = (volatile uint64_t *)d->bar;

    regs[0x80 / 8] = src;
    regs[0x88 / 8] = dst;
    regs[0x90 / 8] = 4096;
    regs[0x98 / 8] = command;
    while (regs[0x98 / 8] & 1)
        usleep(1000);
}

/** Map a sentinel, prove DMA works, unmap it and require the stale IOVA
 * to fault. */
static int dma_fault(struct device *d)
{
    struct vfio_iommu_type1_dma_map map;
    struct vfio_iommu_type1_dma_unmap unmap;
    unsigned long before;
    unsigned i;

    d->guard = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
    if (d->guard == MAP_FAILED)
        return error("sentinel allocation");
    memset(d->guard, 0x5a, 4096);
    memset(&map, 0, sizeof(map));
    map.argsz = sizeof(map);
    map.flags = VFIO_DMA_MAP_FLAG_READ | VFIO_DMA_MAP_FLAG_WRITE;
    map.vaddr = (uintptr_t)d->guard;
    map.iova = 0x1000000;
    map.size = 4096;
    if (ioctl(d->container, VFIO_IOMMU_MAP_DMA, &map) < 0)
        return error("sentinel map");
    dma(d, map.iova, 0x40000, 1);
    memset(d->guard, 0, 4096);
    dma(d, 0x40000, map.iova, 3);
    for (i = 0; i < 4096; i++)
        if (d->guard[i] != 0x5a)
            return error("positive DMA control");
    memset(&unmap, 0, sizeof(unmap));
    unmap.argsz = sizeof(unmap);
    unmap.iova = map.iova;
    unmap.size = map.size;
    if (ioctl(d->container, VFIO_IOMMU_UNMAP_DMA, &unmap) < 0 || unmap.size != map.size)
        return error("sentinel unmap");
    memset(d->guard, 0xa5, 4096);
    printf("assign-edu: control: DMA copied 4096 bytes; IOVA now unmapped\n");
    before = counter("iommu_dma_faults");
    dma(d, 0x40000, map.iova, 3);
    while (counter("iommu_dma_faults") == before)
        usleep(1000);
    for (i = 0; i < 4096; i++)
        if (d->guard[i] != 0xa5)
            return error("sentinel changed after unmap");
    printf("assign-edu: dma faults: %lu -> %lu; sentinel unchanged\n", before,
           counter("iommu_dma_faults"));
    return 0;
}

/** Prove a normal MSI delivers, then require a forged MSI to fault
 * without delivery. */
static int irq_fault(struct device *d, int compatibility)
{
    unsigned char request[sizeof(struct vfio_irq_set) + sizeof(int32_t)];
    struct vfio_irq_set *irq = (struct vfio_irq_set *)request;
    unsigned i, cap;
    uint64_t value;
    unsigned long before;

    d->event = eventfd(0, EFD_NONBLOCK);
    memset(request, 0, sizeof(request));
    irq->argsz = sizeof(request);
    irq->flags = VFIO_IRQ_SET_DATA_EVENTFD | VFIO_IRQ_SET_ACTION_TRIGGER;
    irq->index = VFIO_PCI_MSI_IRQ_INDEX;
    irq->count = 1;
    memcpy(request + sizeof(*irq), &d->event, sizeof(int32_t));
    if (d->event < 0 || ioctl(d->fd, VFIO_DEVICE_SET_IRQS, irq) < 0 || iopl(3) < 0)
        return error("MSI eventfd or raw configuration access");
    cap = config_read(d, 0x34) & 0xff;
    for (i = 0; i < 48 && cap >= 0x40; i++) {
        uint32_t head = config_read(d, cap);
        if ((head & 0xff) == 5)
            break;
        cap = (head >> 8) & 0xff;
    }
    if (cap < 0x40 || i == 48)
        return error("MSI capability absent");
    d->cap = cap;
    for (i = 0; i < 3; i++)
        d->msi[i] = config_read(d, cap + 4 + i * 4);
    if (!(d->msi[0] & 0x10))
        return error("normal MSI is not remapped");
    d->bar[0x60 / 4] = 1;
    while (read(d->event, &value, sizeof(value)) != sizeof(value)) {
        if (errno != EAGAIN)
            return error("positive MSI control");
        usleep(1000);
    }
    d->bar[0x64 / 4] = 1;
    printf("assign-edu: control: remapped MSI delivered; forging %s\n",
           compatibility ? "compatibility format" : "index 32767");
    before = counter("iommu_irq_faults");
    d->raw_msi = 1;
    config_write(d, cap + 4, compatibility ? 0xfee00000 : 0xfeeffff0);
    config_write(d, cap + 8, 0);
    config_write(d, cap + 12, compatibility ? 0x40 : 0);
    d->bar[0x60 / 4] = 1;
    while (counter("iommu_irq_faults") == before)
        usleep(1000);
    if (read(d->event, &value, sizeof(value)) >= 0 || errno != EAGAIN)
        return error("forged MSI reached its eventfd");
    d->bar[0x64 / 4] = 1;
    printf("assign-edu: irq faults: %lu -> %lu; no eventfd delivery\n", before,
           counter("iommu_irq_faults"));
    return 0;
}

/** Run one injection and release all assignment resources on every return. */
int main(int argc, char **argv)
{
    struct device d;
    int r;

    setvbuf(stdout, NULL, _IONBF, 0);
    memset(&d, 0, sizeof(d));
    d.container = d.group = d.fd = d.event = -1;
    d.bar = MAP_FAILED;
    d.guard = MAP_FAILED;
    if (argc != 3)
        return 2;
    r = open_device(&d, argv[1]);
    if (r == 0)
        r = strcmp(argv[2], "dma") == 0 ? dma_fault(&d)
                                        : irq_fault(&d, strcmp(argv[2], "compat") == 0);
    close_device(&d);
    return r != 0;
}
