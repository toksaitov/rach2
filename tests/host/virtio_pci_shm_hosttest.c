/*
 * virtio_pci_shm_hosttest.c: the shared memory capabilities of
 * rach2/sys/virtio/virtio_pci.c, read by its capability walk from a model
 * of two functions' configuration space.
 *
 * host-deps: rach2/sys/virtio/virtio_pci.c
 * host-deps: rach2/sys/lib/kstring.c rach2/sys/lib/kctypes.c
 *
 * The first function is QEMU 10.2.1's virtio-vga-gl with hostmem=4G: its
 * common, ISR, device and notification capabilities in BAR 2, and the
 * host-visible region, shared memory 1, the whole of the 64-bit BAR 4 that
 * QEMU places at 14 TiB, followed by a second capability of id 1, which is
 * passed over. The second function has the required capabilities and five
 * shared memory capabilities: one in an I/O BAR, one past its BAR's end, one
 * whose offset and length wrap, one of length 0, all refused with a record,
 * and one valid. The rows read each region through the transport's table
 * and check that no window was mapped at a region's address.
 *
 * Returns 0 when every row passes, 1 otherwise.
 */

#include <stdarg.h>

#include "virtio/virtio.h"
#include "irq.h"
#include "klog.h"
#include "pci.h"
#include "mm/paging.h"
#include "lib/kerrno.h"
#include "lib/kstring.h"

extern int printf(const char *format, ...);
extern int vsnprintf(char *out, unsigned long size, const char *fmt, va_list ap);
extern char *strstr(const char *haystack, const char *needle);

static int failures;

#define CHECK(c, s)                                         \
    do {                                                    \
        if (!(c)) {                                         \
            printf("virtio-pci-shm-test: fail: %s\n", (s)); \
            failures++;                                     \
        }                                                   \
    } while (0)

/* ---- the model: two functions on bus 0 ---- */

#define FUNCTIONS 2
#define TIB       ((uint64_t)1 << 40)
#define GIB       ((uint64_t)1 << 30)

static uint8_t space[FUNCTIONS][256];
static uint64_t bar_size[FUNCTIONS][6];
static pci_device_t fns[FUNCTIONS];
static uint8_t windows[FUNCTIONS][0x4000] __attribute__((aligned(4096)));
static uint64_t mapped[16];
static int nmapped;
static char log_text[2048];

/** The function at device `dev` of bus 0. */
static int fn_of(uint8_t bus, uint8_t dev)
{
    CHECK(bus == 0 && dev < FUNCTIONS, "configuration access to a modeled function");
    return dev < FUNCTIONS ? dev : 0;
}

uint8_t pci_read_config8(uint8_t bus, uint8_t device, uint8_t function, uint8_t offset)
{
    (void)function;
    return space[fn_of(bus, device)][offset];
}

uint16_t pci_read_config16(uint8_t bus, uint8_t device, uint8_t function, uint8_t offset)
{
    return (uint16_t)(pci_read_config8(bus, device, function, offset) |
                      pci_read_config8(bus, device, function, (uint8_t)(offset + 1)) << 8);
}

uint32_t pci_read_config32(uint8_t bus, uint8_t device, uint8_t function, uint8_t offset)
{
    return (uint32_t)pci_read_config16(bus, device, function, offset) |
           (uint32_t)pci_read_config16(bus, device, function, (uint8_t)(offset + 2)) << 16;
}

void pci_write_config16(uint8_t bus, uint8_t device, uint8_t function, uint8_t offset,
                        uint16_t value)
{
    (void)function;
    space[fn_of(bus, device)][offset] = (uint8_t)value;
    space[fn_of(bus, device)][offset + 1] = (uint8_t)(value >> 8);
}

bool pci_nth(int nth, pci_device_t *out)
{
    if (nth < 0 || nth >= FUNCTIONS) {
        return 0;
    }
    *out = fns[nth];
    return 1;
}

uint8_t pci_find_capability(uint8_t bus, uint8_t device, uint8_t function, uint8_t cap_id)
{
    (void)bus;
    (void)device;
    (void)function;
    (void)cap_id;
    return 0;
}

bool pci_bar_is_memory(uint32_t bar)
{
    return !(bar & 1);
}

uint64_t pci_bar_address64(const pci_device_t *d, int i)
{
    if (d->bar[i] & 1) {
        return d->bar[i] & ~3u;
    }
    return (uint64_t)(d->bar[i] & ~0xfu) |
           ((d->bar[i] & 6) == 4 && i < 5 ? (uint64_t)d->bar[i + 1] << 32 : 0);
}

uint64_t pci_bar_size64(const pci_device_t *d, int i)
{
    return d->bar[i] & 1 ? 0 : bar_size[d->device][i];
}

void pci_enable_bus_master(pci_device_t *dev)
{
    (void)dev;
}

void pci_enable_memory_space(pci_device_t *dev)
{
    (void)dev;
}

void pci_msi_message(uint8_t vector, uint32_t *address, uint32_t *data)
{
    (void)vector;
    *address = *data = 0;
}

int pci_msix_table(const pci_device_t *dev, uint8_t *bar, uint32_t *offset, uint32_t *entries)
{
    (void)dev;
    (void)bar;
    (void)offset;
    (void)entries;
    return -1;
}

int pci_msix_hold(const pci_device_t *dev)
{
    (void)dev;
    return -1;
}

int pci_msix_enable(const pci_device_t *dev, uintptr_t table, uint8_t vector)
{
    (void)dev;
    (void)table;
    (void)vector;
    return -1;
}

/** A window of a function's BAR 2 is host memory of the model's; any other
 * address is recorded and refused. */
uintptr_t map_mmio(uintptr_t phys, size_t size)
{
    int f;

    if (nmapped < 16) {
        mapped[nmapped++] = phys;
    }
    for (f = 0; f < FUNCTIONS; f++) {
        uint64_t base = pci_bar_address64(&fns[f], 2);

        if (phys >= base && phys + size <= base + sizeof(windows[f])) {
            return (uintptr_t)&windows[f][phys - base];
        }
    }
    return 0;
}

uintptr_t paging_virt_to_phys(uintptr_t va)
{
    return va;
}

void *alloc_kernel_frame(void)
{
    return 0;
}

void free_kernel_frame(void *frame)
{
    (void)frame;
}

int isr_alloc_vector(void)
{
    return -1;
}

void isr_free_vector(int vector)
{
    (void)vector;
}

int isr_msi_ready(void)
{
    return 0;
}

void register_isr_handler(uint8_t vector, isr_handler_t handler, const char *name)
{
    (void)vector;
    (void)handler;
    (void)name;
}

void virtio_interrupt_config(struct virtio_dev *d)
{
    (void)d;
}

void virtio_interrupt_queue(struct virtqueue *vq)
{
    (void)vq;
}

void virtio_interrupt_queues(struct virtio_dev *d)
{
    (void)d;
}

/** Keep every record, one a line, for the rows to search. */
void klog(int level, const char *subsystem, const char *format, ...)
{
    unsigned long n = 0;
    va_list ap;

    (void)level;
    while (log_text[n]) {
        n++;
    }
    if (n + 256 >= sizeof(log_text)) {
        return;
    }
    while (*subsystem) {
        log_text[n++] = *subsystem++;
    }
    log_text[n++] = ':';
    log_text[n++] = ' ';
    va_start(ap, format);
    n += (unsigned long)vsnprintf(log_text + n, sizeof(log_text) - n, format, ap);
    va_end(ap);
    log_text[n++] = '\n';
    log_text[n] = 0;
}

/* ---- building a function's capability list ---- */

static uint8_t next_cap[FUNCTIONS];

/** Append a vendor capability of `type` to function `f`'s list, with its
 * BAR, id, offset and length: 24 bytes for shared memory, 20 for the
 * notification capability, whose multiplier is 4, and 16 for the rest. */
static void add_cap(int f, uint8_t type, uint8_t bar, uint8_t id, uint64_t off, uint64_t length)
{
    uint8_t len = type == 8 ? 24 : type == 2 ? 20 : 16;
    uint8_t at = next_cap[f], *c = &space[f][at];
    uint8_t prev = space[f][0x34], last = prev;
    uint32_t mult = 4;

    while (prev) {
        last = prev;
        prev = space[f][prev + 1];
    }
    if (last) {
        space[f][last + 1] = at;
    } else {
        space[f][0x34] = at;
    }
    CHECK(at + len <= 256 && at <= 0xE8, "the capability fits the walk's range");
    c[0] = 0x09;
    c[1] = 0;
    c[2] = len;
    c[3] = type;
    c[4] = bar;
    c[5] = id;
    k_memcpy(c + 8, &off, 4);
    k_memcpy(c + 12, &length, 4);
    if (type == 2) {
        k_memcpy(c + 16, &mult, 4);
    } else if (type == 8) {
        k_memcpy(c + 16, (const char *)&off + 4, 4);
        k_memcpy(c + 20, (const char *)&length + 4, 4);
    }
    next_cap[f] = (uint8_t)(at + len);
}

/** Make function `f` a modern virtio GPU, 1af4:1050, with BAR 2 a 64-bit
 * memory BAR at `bar2` and the four capabilities QEMU puts in it. */
static void make_function(int f, uint64_t bar2)
{
    k_memset(space[f], 0, sizeof(space[f]));
    k_memset(&fns[f], 0, sizeof(fns[f]));
    fns[f].vendor_id = 0x1AF4;
    fns[f].device_id = 0x1050;
    fns[f].device = (uint8_t)f;
    fns[f].class_code = 0x03;
    fns[f].bar[2] = (uint32_t)bar2 | 0xC;
    fns[f].bar[3] = (uint32_t)(bar2 >> 32);
    bar_size[f][2] = 0x4000;
    space[f][0] = 0xF4;
    space[f][1] = 0x1A;
    space[f][2] = 0x50;
    space[f][3] = 0x10;
    space[f][PCI_STATUS] = PCI_STATUS_CAP_LIST;
    next_cap[f] = 0x40;
    add_cap(f, 1, 2, 0, 0x0000, 0x1000);
    add_cap(f, 3, 2, 0, 0x1000, 0x1000);
    add_cap(f, 4, 2, 0, 0x2000, 0x1000);
    add_cap(f, 2, 2, 0, 0x3000, 0x1000);
}

/* ---- the rows ---- */

/** Whether a window was mapped at `phys`. */
static int was_mapped(uint64_t phys)
{
    int i;

    for (i = 0; i < nmapped; i++) {
        if (mapped[i] == phys) {
            return 1;
        }
    }
    return 0;
}

/** QEMU's host-visible region at 14 TiB, and a second capability of its
 * id passed over. */
static void qemu_layout(void)
{
    struct virtio_dev *d;
    uint64_t addr = 0, len = 0, hostmem = 14 * TIB;

    make_function(0, 0xFE000000u);
    fns[0].bar[4] = (uint32_t)hostmem | 0xC;
    fns[0].bar[5] = (uint32_t)(hostmem >> 32);
    bar_size[0][4] = 4 * GIB;
    add_cap(0, 8, 4, 1, 0, 4 * GIB);
    add_cap(0, 8, 4, 1, GIB, GIB);
    CHECK(virtio_pci_find(VIRTIO_ID_GPU, 0, &d) == 0, "QEMU's virtio-gpu is taken");
    CHECK(d->ops->shm_region(d, 1, &addr, &len) == 0 && addr == hostmem && len == 4 * GIB,
          "shared memory 1 is the whole 4 GiB BAR at 14 TiB");
    CHECK(d->ops->shm_region(d, 0, &addr, &len) == -ENOENT, "no region 0");
    CHECK(!was_mapped(hostmem), "the host-visible region is not mapped at attach");
    CHECK(strstr(log_text, "found: virtio0: shared memory 1 at e00:0, 4096 MiB") != 0,
          "the region is logged once with its address and size");
}

/** Capabilities a region cannot be taken from, refused with a record. */
static void refusals(void)
{
    struct virtio_dev *d;
    uint64_t addr = 0, len = 0, base = 16 * TIB;

    make_function(1, 0xFD000000u);
    fns[1].bar[0] = 0xC001; /* an I/O BAR */
    fns[1].bar[4] = (uint32_t)base | 0xC;
    fns[1].bar[5] = (uint32_t)(base >> 32);
    bar_size[1][4] = 4 * GIB;
    add_cap(1, 8, 0, 2, 0, 0x1000);
    add_cap(1, 8, 4, 3, 3 * GIB, 2 * GIB);
    add_cap(1, 8, 4, 4, ~(uint64_t)0xFFF, 0x2000);
    add_cap(1, 8, 4, 5, 0, 0);
    add_cap(1, 8, 4, 6, GIB, 2 * GIB);
    log_text[0] = 0;
    CHECK(virtio_pci_find(VIRTIO_ID_GPU, 1, &d) == 0, "the second function is taken");
    CHECK(d->ops->shm_region(d, 2, &addr, &len) == -ENOENT, "a region in an I/O BAR is refused");
    CHECK(d->ops->shm_region(d, 3, &addr, &len) == -ENOENT,
          "a region past its BAR's end is refused");
    CHECK(d->ops->shm_region(d, 4, &addr, &len) == -ENOENT, "a region whose end wraps is refused");
    CHECK(d->ops->shm_region(d, 5, &addr, &len) == -ENOENT, "an empty region is refused");
    CHECK(d->ops->shm_region(d, 6, &addr, &len) == 0 && addr == base + GIB && len == 2 * GIB,
          "a region within its BAR is taken at its offset");
    CHECK(strstr(log_text, "refused: virtio1: shared memory 3: offset 0x0c0000000") != 0 &&
              strstr(log_text, "refused: virtio1: shared memory 4:") != 0 &&
              strstr(log_text, "refused: virtio1: shared memory 5:") != 0,
          "each refusal is recorded with the region and its BAR");
    CHECK(!was_mapped(base + GIB), "no region is mapped");
}

/** Run the rows. */
int main(void)
{
    qemu_layout();
    refusals();
    printf("virtio-pci-shm-test: %s\n", failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}
