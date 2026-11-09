/*
 * pciwalk_hosttest.c: the one walk of the PCI buses that pci.c's searches
 * read, against a model of configuration space with bridges.
 *
 * host-deps: rach2/sys/arch/x86/pciconf.c
 * host-deps: tests/host/compat/spinlock_host.c
 *
 * The model stands behind the configuration ports 0xCF8 and 0xCFC, so the
 * accessors of arch/x86/pciconf.c run as they do in the kernel, and counts
 * each read of the data port by bus and by function. pci.c is included
 * whole so that a row can forget the list and make the walk again on
 * another model.
 *
 * The first model has an empty slot, a function 0 of vendor 0x0000, a
 * multi-function device with a gap, a single-function device that answers
 * for all eight function numbers, a bridge that firmware left unnumbered, a
 * chain of two bridges, a bridge whose secondary bus is numbered below its
 * own, a CardBus bridge, a second root bus with a bridge of its own, and a
 * bus no bridge leads to whose device 0 is empty. Most buses behind a
 * bridge have no device 0 either, so that only the bridge leads the walk
 * there. The rows check that the walk lists every function behind a bridge
 * or on a root bus in the order of bus, device and function, reads device 0
 * of each other bus once, and is not made again by a later search, which
 * reads only the function it returns. A second model has one root and a bus
 * numbered below its bridge. The third has more functions than the list's
 * first array holds, so the list moves to the heap, and then the heap is
 * refused.
 */

#include "../../rach2/sys/pci.c"

/* The host's C library, declared here: its headers would define NULL again
 * over the kernel's. */
extern int printf(const char *fmt, ...);
extern void *malloc(size_t size);
extern void free(void *p);
extern void *memset(void *s, int c, size_t n);
extern void *memcpy(void *dest, const void *src, size_t n);
extern char *strstr(const char *haystack, const char *needle);

/* ---- the model ---- */

#define MAX_FUNCS 400

/* One function of the model: its place and its 256 bytes of space. A
 * function with `ghost` set answers for function numbers 1 to 7 of its
 * device as well, as a single-function device may. */
struct func {
    int bus, dev, fn, ghost;
    unsigned char space[256];
    long reads; /* reads of the data port while the function was addressed */
};

static struct func funcs[MAX_FUNCS];
static int nfuncs;
static unsigned int address_port;
static long bus_reads[256];
static long all_reads;

/** Empty the model and its counts. */
static void model_reset(void)
{
    memset(funcs, 0, sizeof(funcs));
    memset(bus_reads, 0, sizeof(bus_reads));
    nfuncs = 0;
    all_reads = 0;
}

/** Put a dword at `off` of `f`'s space. */
static void put32(struct func *f, int off, unsigned int v)
{
    f->space[off] = (unsigned char)v;
    f->space[off + 1] = (unsigned char)(v >> 8);
    f->space[off + 2] = (unsigned char)(v >> 16);
    f->space[off + 3] = (unsigned char)(v >> 24);
}

/**
 * Add a function at bus:dev.fn with the identity `id`, the vendor in its low
 * half and the device in its high half, the class in the high byte of
 * `cls` and the subclass in its low byte, and the header type byte given;
 * the header's bit 7 marks a multi-function device on function 0.
 */
static struct func *model_add(int bus, int dev, int fn, unsigned int id, unsigned int cls,
                              int header)
{
    struct func *f = &funcs[nfuncs++];

    f->bus = bus;
    f->dev = dev;
    f->fn = fn;
    put32(f, 0x00, id);
    put32(f, 0x08, cls << 16 | 0x01);
    f->space[0x0E] = (unsigned char)header;
    return f;
}

/** Add a PCI-to-PCI bridge (header 1) or a CardBus bridge (header 2) with
 * its primary, secondary and subordinate bus numbers. */
static struct func *model_bridge(int bus, int dev, int fn, int header, int secondary,
                                 int subordinate)
{
    struct func *f = model_add(bus, dev, fn, 0x24488086u, header == 2 ? 0x0607u : 0x0604u, header);

    f->space[0x18] = (unsigned char)bus;
    f->space[0x19] = (unsigned char)secondary;
    f->space[0x1A] = (unsigned char)subordinate;
    return f;
}

/** The function the address port selects, or NULL when none answers. */
static struct func *addressed(void)
{
    int bus = (int)(address_port >> 16) & 0xFF;
    int dev = (int)(address_port >> 11) & 0x1F;
    int fn = (int)(address_port >> 8) & 0x7;
    int i;

    if (!(address_port & 0x80000000u)) {
        return NULL;
    }
    for (i = 0; i < nfuncs; i++) {
        if (funcs[i].bus == bus && funcs[i].dev == dev &&
            (funcs[i].fn == fn || (funcs[i].ghost && funcs[i].fn == 0))) {
            return &funcs[i];
        }
    }
    return NULL;
}

/** Take a dword write to the address port or the data port. */
void outd(unsigned short port, unsigned int value)
{
    struct func *f;

    if (port == 0xCF8) {
        address_port = value;
        return;
    }
    f = addressed();
    if (port == 0xCFC && f) {
        put32(f, (int)(address_port & 0xFC), value);
    }
}

/** Read the dword the address port selects, all ones where nothing
 * answers, and count the read. */
unsigned int ind(unsigned short port)
{
    struct func *f;
    unsigned int off = address_port & 0xFC;

    if (port != 0xCFC) {
        return 0xFFFFFFFFu;
    }
    all_reads++;
    bus_reads[(address_port >> 16) & 0xFF]++;
    f = addressed();
    if (!f) {
        return 0xFFFFFFFFu;
    }
    f->reads++;
    return (unsigned int)f->space[off] | (unsigned int)f->space[off + 1] << 8 |
           (unsigned int)f->space[off + 2] << 16 | (unsigned int)f->space[off + 3] << 24;
}

/** Ignore a byte write: no row writes a byte. */
void outb(unsigned short port, unsigned char value)
{
    (void)port;
    (void)value;
}

/** Return 0xFF, as an absent port reads. */
unsigned char inb(unsigned short port)
{
    (void)port;
    return 0xFF;
}

/** Ignore a halfword write: no row writes one. */
void outw(unsigned short port, unsigned short value)
{
    (void)port;
    (void)value;
}

/** Return all ones, as an absent port reads. */
unsigned short inw(unsigned short port)
{
    (void)port;
    return 0xFFFF;
}

/** Interrupts are not modeled. */
unsigned int kirq_save(void)
{
    return 0;
}

/** Interrupts are not modeled. */
void kirq_restore(unsigned int flags)
{
    (void)flags;
}

/* The warnings the code logged, which the heap rows count, and the
 * messages that name a root bus, which the walk's rows count. */
static int warnings;
static int roots;

/** Count a warning or a root bus; the rest of the log is discarded. */
void klog(int level, const char *subsystem, const char *fmt, ...)
{
    (void)subsystem;
    if (level == KLOG_WARN) {
        warnings++;
    }
    if (strstr(fmt, "root of its own")) {
        roots++;
    }
}

/** No row sends a message. */
unsigned int lapic_id(void)
{
    return 0;
}

/* Whether the heap refuses, and what it handed out and took back. */
static int heap_refuses;
static int heap_live;

/** The kernel's heap, as the host's, refusing when a row says so. */
void *kmalloc(size_t size)
{
    void *p;

    if (heap_refuses) {
        return NULL;
    }
    p = malloc(size);
    if (p) {
        heap_live++;
    }
    return p;
}

/** Give back a block of `kmalloc`'s. */
void kfree(void *p)
{
    if (p) {
        heap_live--;
        free(p);
    }
}

/** The kernel's memcpy, as the host's. */
void *k_memcpy(void *dest, const void *src, size_t n)
{
    return memcpy(dest, src, n);
}

/** Forget the list, as a new boot would, giving back its heap array. */
static void list_forget(void)
{
    if (pci_list != pci_first) {
        kfree(pci_list);
    }
    pci_list = pci_first;
    pci_list_cap = PCI_FIRST_MAX;
    pci_list_len = -1;
}

/* ---- the rows ---- */

static int fails;

/** Count and print a failed row when `ok` is 0. */
static void check(int ok, const char *what)
{
    if (!ok) {
        fails++;
        printf("pciwalk_hosttest: FAIL: %s\n", what);
    }
}

/** True when `d` is at bus:dev.fn. */
static int at(const pci_device_t *d, int bus, int dev, int fn)
{
    return d->bus == bus && d->device == dev && d->function == fn;
}

/** The functions the first model's walk must list, in order. */
static const int expected[][3] = {
    {0, 0, 0},  /* the host bridge */
    {0, 1, 0},  /* a multi-function device, its function 1 empty */
    {0, 1, 2},  /* */
    {0, 2, 0},  /* a single-function device answering for all eight */
    {0, 3, 0},  /* a bridge to bus 1 */
    {0, 4, 0},  /* a bridge firmware left unnumbered */
    {0, 6, 0},  /* a bridge to bus 9 */
    {0, 8, 0},  /* a CardBus bridge to bus 10 */
    {0, 31, 0}, /* the last slot */
    {1, 2, 0},  /* a bridge to bus 2 */
    {1, 5, 0},  /* */
    {2, 4, 0},  /* a network card two bridges down */
    {2, 5, 0},  /* the same card's twin */
    {4, 6, 0},  /* a SATA controller, behind bus 9's bridge */
    {9, 2, 0},  /* a bridge to bus 4, numbered below its own */
    {10, 0, 0}, /* a card in the CardBus slot */
    {64, 0, 0}, /* a second root bus's host bridge */
    {64, 1, 0}, /* its bridge to bus 65 */
    {65, 0, 0}, /* a disk controller there */
};

#define NEXPECTED ((int)(sizeof(expected) / sizeof(expected[0])))

/** Build the first model. */
static void model_bridges(void)
{
    struct func *f;

    model_reset();
    model_add(0, 0, 0, 0x12378086u, 0x0600u, 0x00);
    model_add(0, 1, 0, 0x70008086u, 0x0601u, 0x80);
    model_add(0, 1, 2, 0x70208086u, 0x0C03u, 0x00);
    f = model_add(0, 2, 0, 0x11111234u, 0x0300u, 0x00);
    f->ghost = 1;
    model_bridge(0, 3, 0, 0x01, 1, 2);
    model_bridge(0, 4, 0, 0x01, 0, 0);
    /* 00:05 is an empty slot. */
    model_bridge(0, 6, 0, 0x01, 9, 9);
    model_add(0, 7, 0, 0x00000000u, 0x0000u, 0x00);
    model_bridge(0, 8, 0, 0x02, 10, 10);
    model_add(0, 31, 0, 0x29188086u, 0x0601u, 0x00);
    model_bridge(1, 2, 0, 0x01, 2, 2);
    model_add(1, 5, 0, 0x813910ECu, 0x0200u, 0x00);
    f = model_add(2, 4, 0, 0x100E8086u, 0x0200u, 0x00);
    put32(f, 0x10, 0xFEB00000u);
    f->space[0x3C] = 11;
    model_add(2, 5, 0, 0x100E8086u, 0x0200u, 0x00);
    model_bridge(9, 2, 0, 0x01, 4, 4);
    model_add(4, 6, 0, 0x29228086u, 0x0106u, 0x00);
    model_add(10, 0, 0, 0x0013168Cu, 0x0280u, 0x00);
    /* Bus 7 has a device, but no bridge leads to it and its device 0 is
     * empty, so it is not a root. */
    model_add(7, 3, 0, 0xFFF08086u, 0x0200u, 0x00);
    /* Bus 64 is a root of its own, with a bridge to bus 65. */
    model_add(64, 0, 0, 0x14501022u, 0x0600u, 0x00);
    model_bridge(64, 1, 0, 0x01, 65, 65);
    model_add(65, 0, 0, 0xA808144Du, 0x0108u, 0x00);
}

/** The walk lists every function a bridge leads to or a root bus has, in
 * order, and reads device 0 of each other bus once. */
static void walk_rows(void)
{
    pci_device_t d;
    int i, bus, ok;
    long walk_reads;

    model_bridges();
    list_forget();
    roots = 0;

    ok = 1;
    for (i = 0; i < NEXPECTED; i++) {
        if (!pci_nth(i, &d) || !at(&d, expected[i][0], expected[i][1], expected[i][2])) {
            ok = 0;
            printf("pciwalk_hosttest: entry %d is %d:%d.%d, not %d:%d.%d\n", i, d.bus, d.device,
                   d.function, expected[i][0], expected[i][1], expected[i][2]);
        }
    }
    check(ok, "pci_nth lists every function behind a bridge in order");
    check(!pci_nth(NEXPECTED, &d), "pci_nth ends after the last function");
    check(roots == 1, "the walk takes bus 64 alone for a root, the others by their bridges");
    check(!pci_nth(-1, &d), "pci_nth refuses a negative index");

    for (bus = 0, ok = 1; bus < 256; bus++) {
        int led = bus == 0 || bus == 1 || bus == 2 || bus == 4 || bus == 9 || bus == 10 ||
                  bus == 64 || bus == 65;

        if (!led && bus_reads[bus] != 1) {
            ok = 0;
            printf("pciwalk_hosttest: bus %d read %ld times\n", bus, bus_reads[bus]);
        }
        if (led && bus_reads[bus] < 32) {
            ok = 0;
            printf("pciwalk_hosttest: bus %d read only %ld times\n", bus, bus_reads[bus]);
        }
    }
    check(ok, "the walk reads each bus it finds whole and device 0 of each other once");

    /* The walk itself: one read of each of the eight buses' 32 slots, one of
     * device 0 of every other bus, and a few for each function; the old walk
     * read 8,192 slots and more. */
    list_forget();
    model_bridges();
    (void)pci_list_count();
    walk_reads = all_reads;
    check(walk_reads <= 8 * 32 + 255 + NEXPECTED * 4 + 7,
          "the walk reads the slots of eight buses and each function's header once");
    check(pci_list_len == NEXPECTED, "the walk lists each function once");

    /* A search after the walk reads only the function it returns. */
    all_reads = 0;
    for (i = 0; i < nfuncs; i++) {
        funcs[i].reads = 0;
    }
    check(pci_find_device(0x02, 0x00, &d) && at(&d, 1, 5, 0),
          "pci_find_device finds the first network card");
    for (i = 0, ok = 1; i < nfuncs; i++) {
        if (funcs[i].reads != 0 && !(funcs[i].bus == 1 && funcs[i].dev == 5)) {
            ok = 0;
        }
    }
    check(ok && all_reads == 9, "a search reads the nine dwords of the function it returns");
}

/** The finders read the list and the registers of what they return now. */
static void finder_rows(void)
{
    pci_device_t d, prev;
    int n, ok;

    check(pci_find_device(0x01, 0x06, &d) && at(&d, 4, 6, 0),
          "pci_find_device finds the controller behind a bridge numbered low");
    check(!pci_find_device(0x09, 0x00, &d), "pci_find_device finds no absent class");

    /* Every PCI-to-PCI bridge in turn: 00:03, 00:04, 00:06, 01:02, 09:02,
     * 40:01. */
    n = 0;
    ok = 1;
    if (pci_find_device_skip(0x06, 0x04, NULL, &d)) {
        n++;
        ok = ok && at(&d, 0, 3, 0);
        prev = d;
        while (pci_find_device_skip(0x06, 0x04, &prev, &d)) {
            n++;
            prev = d;
        }
        ok = ok && at(&prev, 64, 1, 0);
    }
    check(ok && n == 6, "pci_find_device_skip walks every bridge of one class");
    prev.bus = 7;
    prev.device = 0;
    prev.function = 0;
    check(!pci_find_device_skip(0x02, 0x00, &prev, &d),
          "pci_find_device_skip finds nothing after a function not on the list");

    check(pci_find_nth_by_id(0x8086, 0x100E, 0, &d) && at(&d, 2, 4, 0),
          "pci_find_nth_by_id finds the first twin");
    check(pci_find_nth_by_id(0x8086, 0x100E, 1, &d) && at(&d, 2, 5, 0),
          "pci_find_nth_by_id finds the second twin");
    check(!pci_find_nth_by_id(0x8086, 0x100E, 2, &d), "pci_find_nth_by_id ends after the twins");
    check(pci_find_device_by_id(0x168C, 0x0013, &d) && at(&d, 10, 0, 0) && d.class_code == 0x02 &&
              d.subclass == 0x80,
          "pci_find_device_by_id finds the card behind the CardBus bridge");
    check(!pci_find_device_by_id(0x8086, 0xFFF0, &d),
          "pci_find_device_by_id finds nothing on the bus no bridge leads to");

    check(pci_find_bridge_for_bus(2, &d) && at(&d, 1, 2, 0) && d.vendor_id == 0x8086,
          "pci_find_bridge_for_bus finds the bridge to bus 2");
    check(pci_find_bridge_for_bus(4, &d) && at(&d, 9, 2, 0),
          "pci_find_bridge_for_bus finds the bridge numbered above its bus");
    check(pci_find_device(0x01, 0x08, &d) && at(&d, 65, 0, 0),
          "pci_find_device finds the controller under the second root bus");
    check(!pci_find_bridge_for_bus(7, &d), "pci_find_bridge_for_bus finds no bridge to bus 7");
}

/** A search reads what changes after the walk again: a BAR, the interrupt
 * line, a bridge's secondary bus. */
static void fresh_rows(void)
{
    pci_device_t d;
    struct func *f;

    f = &funcs[0];
    while (!(f->bus == 2 && f->dev == 4)) {
        f++;
    }
    check(pci_find_nth_by_id(0x8086, 0x100E, 0, &d) && d.bar[0] == 0xFEB00000u && d.irq == 11 &&
              d.revision == 0x01,
          "a search returns the BAR, the interrupt line and the revision");
    put32(f, 0x10, 0xFE000000u);
    f->space[0x3C] = 10;
    check(pci_find_nth_by_id(0x8086, 0x100E, 0, &d) && d.bar[0] == 0xFE000000u && d.irq == 10,
          "a search reads the BAR and line as they are now");
    f = &funcs[0];
    while (!(f->bus == 1 && f->dev == 2)) {
        f++;
    }
    f->space[0x19] = 3;
    check(pci_find_bridge_for_bus(3, &d) && at(&d, 1, 2, 0) && !pci_find_bridge_for_bus(2, &d),
          "pci_find_bridge_for_bus reads the secondary bus as it is now");
}

/** A bus numbered below the bridge that leads to it, on a machine with one
 * root: 00:01 leads to bus 5, and 05:02 to bus 3. */
static void low_rows(void)
{
    pci_device_t d;

    model_reset();
    model_add(0, 0, 0, 0x29C08086u, 0x0600u, 0x00);
    model_bridge(0, 1, 0, 0x01, 5, 5);
    model_bridge(5, 2, 0, 0x01, 3, 3);
    model_add(3, 4, 0, 0x10D38086u, 0x0200u, 0x00);
    list_forget();
    roots = 0;
    check(pci_nth(2, &d) && at(&d, 3, 4, 0) && pci_nth(3, &d) && at(&d, 5, 2, 0) &&
              !pci_nth(4, &d) && roots == 0,
          "the walk reads a bus numbered below its bridge by another pass");
}

/** A machine with more functions than the first array holds: 30 devices of
 * eight functions on bus 0, 240 in all. */
static void model_many(void)
{
    int dev, fn;

    model_reset();
    for (dev = 0; dev < 30; dev++) {
        for (fn = 0; fn < 8; fn++) {
            model_add(0, dev, fn, 0x1AF4u | (0x1000u + (unsigned int)(dev * 8 + fn)) << 16, 0x0200u,
                      fn == 0 ? 0x80 : 0x00);
        }
    }
}

/** The list moves to the heap as it grows, and stops with a warning when
 * the heap refuses. */
static void heap_rows(void)
{
    pci_device_t d;
    int i, ok;

    model_many();
    list_forget();
    warnings = 0;
    for (i = 0, ok = 1; i < 240; i++) {
        if (!pci_nth(i, &d) || !at(&d, 0, i / 8, i % 8) || d.device_id != 0x1000 + i) {
            ok = 0;
        }
    }
    check(ok && !pci_nth(240, &d), "the list holds 240 functions");
    check(pci_list_cap == 256 && heap_live == 1 && warnings == 0,
          "the list grew to 256 entries on the heap, giving back the smaller array");

    list_forget();
    check(heap_live == 0, "the heap array is given back");
    heap_refuses = 1;
    check(!pci_nth(PCI_FIRST_MAX, &d) && pci_nth(PCI_FIRST_MAX - 1, &d) && warnings == 1,
          "a refused heap stops the list at the first array with a warning");
    heap_refuses = 0;
    list_forget();
}

/** Run the rows. */
int main(void)
{
    walk_rows();
    finder_rows();
    fresh_rows();
    low_rows();
    heap_rows();
    if (fails) {
        printf("pciwalk_hosttest: %d failed\n", fails);
        return 1;
    }
    printf("pciwalk_hosttest: ok\n");
    return 0;
}
