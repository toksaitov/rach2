/*
 * virtio_pci_shm_hosttest.c: the shared memory capabilities of
 * rach2/sys/virtio/virtio_pci.c, read by its capability walk from a model
 * of two functions' configuration space.
 *
 * host-deps: rach2/sys/virtio/virtio_pci.c
 * host-deps: rach2/sys/lib/kstring.c rach2/sys/lib/kctypes.c
 * host-deps: tests/host/compat/spinlock_host.c tests/host/compat/kirq_none.c
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
 * Each function taken is shown bound to virtio-pci once, however often it
 * is found, with its virtio device added to the bus once, which names it,
 * and one "bind" uevent.
 *
 * The probe rows add three functions after them: a GPU without its common
 * capability, which is refused, unbound and unmapped and adds nothing to
 * the bus, so that the next function takes the next free name; a
 * virtio-net function, which has its device though no driver asks for it;
 * and a GPU whose bus registration fails, which leaves no window mapped,
 * its command word as it was and no record.
 *
 * The interrupt rows give every MSI-X entry a remapped message, release
 * them before vectors, and force vector and remapper allocation failures.
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
extern int strcmp(const char *a, const char *b);

static int failures;

#define CHECK(c, s)                                         \
    do {                                                    \
        if (!(c)) {                                         \
            printf("virtio-pci-shm-test: fail: %s\n", (s)); \
            failures++;                                     \
        }                                                   \
    } while (0)

/* ---- the model: two functions on bus 0 ---- */

#define FUNCTIONS 5
#define TIB       ((uint64_t)1 << 40)
#define GIB       ((uint64_t)1 << 30)

static uint8_t space[FUNCTIONS][256];
static uint64_t bar_size[FUNCTIONS][6];
static pci_device_t fns[FUNCTIONS];
static uint8_t windows[FUNCTIONS][0x4000] __attribute__((aligned(4096)));
static uint64_t mapped[16];
static int nmapped, live_maps;
static int msix_ready, msix_held, msix_enabled, vector_limit = 3;
static int reject_entry = -1;
static unsigned live_vectors, remapped_entries;
static unsigned starts;
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
    space[fn_of(dev->bus, dev->device)][PCI_COMMAND] |= PCI_CMD_BUS_MASTER;
}

void pci_enable_memory_space(pci_device_t *dev)
{
    space[fn_of(dev->bus, dev->device)][PCI_COMMAND] |= PCI_CMD_MEMORY_SPACE;
}

/** Expose three entries in the notification BAR's unused final page. */
int pci_msix_table(const pci_device_t *dev, uint8_t *bar, uint32_t *offset, uint32_t *entries)
{
    (void)dev;
    if (!msix_ready)
        return -1;
    *bar = 2;
    *offset = 0x3000;
    *entries = 3;
    return 0;
}

/** Hold the function mask while its messages are programmed. */
int pci_msix_hold(const pci_device_t *dev)
{
    (void)dev;
    msix_held = msix_enabled = 1;
    return 0;
}

/** Model the remapper, including an allocation failure at one entry. */
int pci_msix_entry_set(const pci_device_t *dev, uintptr_t table, unsigned int index, uint8_t vector)
{
    uint32_t *entry = (uint32_t *)table + index * 4;

    CHECK(dev->device == 0 && table == (uintptr_t)&windows[0][0x3000],
          "MSI-X belongs to the device's table");
    CHECK(msix_held && msix_enabled && index < 3, "program under the function mask");
    CHECK(live_vectors & (1u << (vector - 64)), "a message has a live vector");
    if ((int)index == reject_entry)
        return -1;
    CHECK(!(remapped_entries & (1u << index)), "each entry registered once");
    remapped_entries |= 1u << index;
    entry[0] = 0xfee00010u | (index << 5);
    entry[1] = entry[2] = entry[3] = 0;
    return 0;
}

/** Release every remapping before the transport frees any vector. */
void pci_msix_disable(const pci_device_t *dev)
{
    (void)dev;
    msix_held = 1;
    msix_enabled = 0;
    remapped_entries = 0;
}

/** Require complete messages before any interrupt can be sent. */
void pci_msix_unmask(const pci_device_t *dev)
{
    (void)dev;
    CHECK(msix_held && msix_enabled && remapped_entries == live_vectors && live_vectors,
          "all vectors remapped before interrupts start");
    msix_held = 0;
    starts++;
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
            live_maps++;
            return (uintptr_t)&windows[f][phys - base];
        }
    }
    return 0;
}

/** Give back a window `map_mmio` mapped. */
void mmio_unmap(uintptr_t va, size_t size)
{
    CHECK(va >= (uintptr_t)windows && va + size <= (uintptr_t)windows + sizeof(windows) &&
              live_maps > 0,
          "a window given back is one the transport mapped");
    live_maps--;
}

void *kzalloc(size_t size)
{
    return __builtin_calloc(1, size);
}

void kfree(void *ptr)
{
    __builtin_free(ptr);
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

/** Allocate from a small vector pool to force shared-vector fallback. */
int isr_alloc_vector(void)
{
    int i;

    for (i = 0; i < vector_limit; i++) {
        if (!(live_vectors & (1u << i))) {
            live_vectors |= 1u << i;
            return 64 + i;
        }
    }
    return -1;
}

void isr_free_vector(int vector)
{
    CHECK(msix_held && !remapped_entries, "remappings released before vectors");
    CHECK(vector >= 64 && vector < 67 && (live_vectors & (1u << (vector - 64))),
          "release a live vector once");
    live_vectors &= ~(1u << (vector - 64));
}

int isr_msi_ready(void)
{
    return msix_ready;
}

void register_isr_handler(uint8_t vector, isr_handler_t handler, const char *name)
{
    (void)vector;
    (void)handler;
    (void)name;
}

void isr_register_flags(uint8_t vector, isr_handler_t handler, unsigned int flags)
{
    (void)vector;
    (void)handler;
    (void)flags;
}

/* The bindings the transport showed in /sys: how many, and the last. */
static int binds, bound_key = -1;
static const char *bound_driver = "";

/** Record a binding, as sysfs.c shows one. */
int sysfs_pci_bind(int key, const char *driver)
{
    binds++;
    bound_key = key;
    bound_driver = driver;
    return 0;
}

/** Take the binding of `key` away. */
void sysfs_pci_unbind(int key)
{
    if (key == bound_key) {
        bound_key = -1;
    }
}

/* The "bind" uevents of functions, and the devices added to the virtio bus:
 * how many, the last one's name, vendor and type, the numbers taken, and
 * what the next addition fails with, or 0. */
static int bind_events, bus_adds, bus_error;
static char bus_name[16];
static unsigned bus_vendor, bus_type;
static int bus_taken[10];

/** Count a function's "bind" uevent. */
int32_t sysfs_pci_uevent(int key, const char *action)
{
    CHECK(key == bound_key && strcmp(action, "bind") == 0,
          "the bind uevent is the bound function's");
    bind_events++;
    return 0;
}

/** The anchor of a fixed directory, here its key plus one. */
int sysfs_anchor(int node, int key)
{
    (void)node;
    return key + 1;
}

/** Record a device added to the virtio bus and name it virtioN for the
 * lowest N free, as virtio_bus.c does, which virtio_bus_hosttest.c tests;
 * or fail with `bus_error`. */
int virtio_bus_add(struct virtio_dev *d, int parent, uint16_t vendor)
{
    int n;

    CHECK(parent == bound_key + 1, "the virtio device goes below its function");
    if (bus_error) {
        n = bus_error;
        bus_error = 0;
        return n;
    }
    for (n = 0; n < 9 && bus_taken[n]; n++) {
    }
    bus_taken[n] = 1;
    k_memcpy(d->bus_name, "virtio", 6);
    d->bus_name[6] = (char)('0' + n);
    d->bus_name[7] = '\0';
    d->name = d->bus_name;
    bus_adds++;
    k_memcpy(bus_name, d->name, k_strlen(d->name) + 1);
    bus_vendor = vendor;
    bus_type = d->id;
    return 0;
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
    space[f][0x2C] = 0xF4; /* the subsystem vendor, QEMU's */
    space[f][0x2D] = 0x1A;
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
    CHECK(binds == 1 && bound_key == 0 && strcmp(bound_driver, "virtio-pci") == 0,
          "the function is shown bound to virtio-pci, as Linux binds it");
    CHECK(bus_adds == 1 && strcmp(bus_name, "virtio0") == 0 && bus_vendor == 0x1AF4u &&
              bind_events == 1,
          "the function's virtio device is virtio0 on the bus with the subsystem vendor, and "
          "its bind uevent is sent");
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
    CHECK(virtio_pci_probe() == 1, "the probe takes the second function");
    CHECK(virtio_pci_find(VIRTIO_ID_GPU, 1, &d) == 0, "the second function is found");
    CHECK(bus_adds == 2 && strcmp(bus_name, "virtio1") == 0,
          "the second virtio function's device is virtio1");
    CHECK(d->ops->shm_region(d, 2, &addr, &len) == -ENOENT, "a region in an I/O BAR is refused");
    CHECK(d->ops->shm_region(d, 3, &addr, &len) == -ENOENT,
          "a region past its BAR's end is refused");
    CHECK(d->ops->shm_region(d, 4, &addr, &len) == -ENOENT, "a region whose end wraps is refused");
    CHECK(d->ops->shm_region(d, 5, &addr, &len) == -ENOENT, "an empty region is refused");
    CHECK(d->ops->shm_region(d, 6, &addr, &len) == 0 && addr == base + GIB && len == 2 * GIB,
          "a region within its BAR is taken at its offset");
    CHECK(strstr(log_text, "refused: 00:01.0: shared memory 3: offset 0x0c0000000") != 0 &&
              strstr(log_text, "refused: 00:01.0: shared memory 4:") != 0 &&
              strstr(log_text, "refused: 00:01.0: shared memory 5:") != 0,
          "each refusal is recorded by the function's address with the region and its BAR");
    CHECK(!was_mapped(base + GIB), "no region is mapped");
}

/** Exercise queue and configuration remapping, reuse and partial rollback. */
static void interrupts(void)
{
    struct virtio_dev *d;
    unsigned i;
    uint32_t *table = (uint32_t *)&windows[0][0x3000];

    CHECK(virtio_pci_find(VIRTIO_ID_GPU, 0, &d) == 0, "reuse the first function");
    CHECK(binds == 2 && bus_adds == 2 && bind_events == 2,
          "a function found again is not bound or added again");
    msix_ready = 1;
    for (i = 0; i < 2; i++) {
        CHECK(d->ops->irq_setup(d, 2) == 0, "set up two queues and configuration");
        CHECK(remapped_entries == 7 && live_vectors == 7,
              "configuration and both queues use remapped messages");
        CHECK((table[0] & 0x10) && (table[4] & 0x10) && (table[8] & 0x10),
              "every message has the remapped format");
        d->ops->irq_start(d);
        CHECK(!msix_held && starts == i + 1, "lift the mask only at start");
        d->ops->irq_release(d);
        CHECK(!remapped_entries && !live_vectors && !msix_enabled,
              "teardown returns messages and vectors");
    }
    vector_limit = 2;
    CHECK(d->ops->irq_setup(d, 2) == 0, "share a vector after a partial allocation");
    CHECK(remapped_entries == 3 && live_vectors == 3,
          "shared queues retain two complete remappings");
    d->ops->irq_start(d);
    d->ops->irq_release(d);
    CHECK(!remapped_entries && !live_vectors, "shared teardown returns resources");
    vector_limit = 3;
    reject_entry = 1;
    CHECK(d->ops->irq_setup(d, 2) == 0, "fall back when the remapper is full");
    CHECK(!remapped_entries && !live_vectors && !msix_enabled,
          "failed dedicated and shared setups leave no resources");
    d->ops->irq_release(d);
    reject_entry = -1;
}

/** A GPU without its common capability, refused, and a virtio-net function
 * after it, which takes the next free name and has its device though no
 * driver asks for one, as Linux's virtio-pci takes every virtio function. */
static void probe_refused(void)
{
    struct virtio_dev *d;
    int adds = bus_adds, maps = live_maps;

    make_function(2, 0xFC000000u);
    space[2][0x40 + 3] = 0; /* the common capability's type, now none */
    make_function(3, 0xFB000000u);
    fns[3].device_id = 0x1041;
    space[3][2] = 0x41;
    log_text[0] = 0;
    CHECK(virtio_pci_probe() == 1, "the probe takes the virtio-net function alone");
    CHECK(strstr(log_text, "refused: 00:02.0: no common capability in a memory BAR") != 0 &&
              bound_key != (2 << 3) && live_maps == maps + 4 && space[2][PCI_COMMAND] == 0,
          "the GPU without its common capability is refused, unbound, unmapped and untouched, "
          "and only virtio-net's four windows are mapped");
    CHECK(bus_adds == adds + 1 && strcmp(bus_name, "virtio2") == 0 && bus_type == VIRTIO_ID_NET,
          "the refused function takes no number, so virtio-net is virtio2");
    CHECK(virtio_pci_find(VIRTIO_ID_NET, 0, &d) == 0 && strcmp(d->name, "virtio2") == 0,
          "virtio-net is found as virtio2");
    CHECK(virtio_pci_find(VIRTIO_ID_GPU, 2, &d) == -ENODEV, "the refused GPU is no device");
}

/** A GPU whose registration on the bus fails: every window it mapped given
 * back, its command word as it was before the probe, and no record. */
static void probe_bus_fails(void)
{
    struct virtio_dev *d;
    int adds = bus_adds, maps = live_maps;

    make_function(4, 0xFA000000u);
    space[4][PCI_COMMAND] = 0x0003;
    bus_error = -ENOMEM;
    log_text[0] = 0;
    CHECK(virtio_pci_probe() == 0, "the probe takes nothing");
    CHECK(strstr(log_text, "refused: 00:04.0: not added to the virtio bus: error -12") != 0 &&
              bus_adds == adds && bound_key == -1,
          "the failed registration is recorded and the function unbound");
    CHECK(live_maps == maps && space[4][PCI_COMMAND] == 0x03 && space[4][PCI_COMMAND + 1] == 0,
          "no window stays mapped and the command word is as it was");
    CHECK(virtio_pci_find(VIRTIO_ID_GPU, 2, &d) == -ENODEV, "the function has no record");
}

/** Run the rows. */
int main(void)
{
    qemu_layout();
    refusals();
    interrupts();
    probe_refused();
    probe_bus_fails();
    printf("virtio-pci-shm-test: %s\n", failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}
