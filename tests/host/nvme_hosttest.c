/*
 * nvme_hosttest.c: the NVMe driver against a model of a controller.
 *
 * host-deps:
 *
 * QEMU's NVMe controller resets when told, becomes ready, answers every
 * command and keeps its namespace in 512-byte blocks unless asked otherwise.
 * What no boot produces on demand is a controller that does not: one that
 * never becomes ready, a command that fails, a command that is never
 * answered, a controller that will not stop for the recovery, a namespace
 * of 4096-byte blocks. So the driver is compiled natively and given a model
 * of a controller through the register interface it reaches registers by,
 * `struct nvme_io`, and the model reads the queues and moves the data
 * through the PRP entries the driver wrote, as a controller does.
 *
 * A physical address the driver gives the controller is a host address,
 * except where a row scatters a buffer's pages. The model keeps the memory
 * it may be pointed at in regions and finds a host byte by its address.
 *
 * Every row names what the driver must do, and the check is on what it
 * returned, what the model saw and what it left in memory, not on what it
 * printed, except where the log is the only account a failure gives.
 */

#include <stdarg.h>

extern int printf(const char *fmt, ...);
extern int strcmp(const char *a, const char *b);
extern char *strstr(const char *haystack, const char *needle);
extern int vsnprintf(char *out, unsigned long size, const char *fmt, va_list ap);
extern int fork(void);
extern int waitpid(int pid, int *status, int options);
extern void _exit(int status);

#include "../../rach2/sys/block/nvme.h"
#include "../../rach2/sys/block/block.h"
#include "../../rach2/sys/block/disk.h"
#include "../../rach2/sys/klog.h"
#include "../../rach2/sys/latency.h"
#include "../../rach2/sys/pci.h"
#include "../../rach2/sys/irq.h"
#include "../../rach2/sys/mm/paging.h"

/* ---- memory the controller may be pointed at ---- */

#define ARENA_PAGES 32
#define BIG_PAGES   8
#define MEDIUM      2048 /* sectors of the model's namespace */

static uint8_t arena[ARENA_PAGES][4096] __attribute__((aligned(4096)));
static unsigned arena_used;
static int frames_fail;

/* A buffer as the driver sees it, and the pages behind it when a row
   scatters them: `big` page i is `shadow` page perm[i]. */
static uint8_t big[BIG_PAGES * 4096] __attribute__((aligned(4096)));
static uint8_t shadow[BIG_PAGES * 4096] __attribute__((aligned(4096)));
static const unsigned perm[BIG_PAGES] = {5, 2, 7, 0, 3, 6, 1, 4};
static int permuted;

static uint8_t medium[MEDIUM * 512];

void *alloc_kernel_frame(void)
{
    if (frames_fail || arena_used >= ARENA_PAGES) {
        return 0;
    }
    return arena[arena_used++];
}

uintptr_t paging_virt_to_phys(uintptr_t va)
{
    uintptr_t at = va - (uintptr_t)big;

    if (permuted && at < sizeof(big)) {
        return (uintptr_t)(shadow + perm[at / 4096] * 4096 + (at & 0xFFFu));
    }
    return va;
}

/** The host byte the controller reaches at `phys`, `len` bytes of it in one
 * region, or NULL, which the model counts as a fault. */
static unsigned model_faults;

static uint8_t *host_of(uint64_t phys, uint32_t len)
{
    uint8_t *bases[3];
    uint32_t sizes[3];
    int i;

    bases[0] = &arena[0][0];
    sizes[0] = sizeof(arena);
    bases[1] = big;
    sizes[1] = sizeof(big);
    bases[2] = shadow;
    sizes[2] = sizeof(shadow);
    for (i = 0; i < 3; i++) {
        uint64_t at = phys - (uint64_t)(uintptr_t)bases[i];

        if (at < sizes[i] && len <= sizes[i] - at) {
            return bases[i] + at;
        }
    }
    model_faults++;
    return 0;
}

/** The byte the controller moves for byte `i` of `big`. */
static uint8_t *at_phys(uint32_t i)
{
    return permuted ? shadow + perm[i / 4096] * 4096 + (i & 0xFFFu) : big + i;
}

static uint32_t get32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint64_t get64(const uint8_t *p)
{
    return (uint64_t)get32(p) | ((uint64_t)get32(p + 4) << 32);
}

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

/* ---- the controller ---- */

/* What this run's controller does, set before each row. */
static struct {
    int never_ready;  /* EN set, RDY never follows */
    int fatal;        /* EN set, CFS instead of RDY */
    int stuck;        /* EN cleared, RDY stays */
    int ready_again;  /* a second enable comes up; 0 makes recovery fail */
    int no_active;    /* Identify CNS 2 is refused */
    int drop_io;      /* I/O commands are taken and never answered */
    int fail_io;      /* the next N I/O commands answer LBA out of range */
    int defer;        /* I/O completions wait for model_release */
    uint32_t mdts;    /* Identify Controller byte 77 */
    uint32_t lbads;   /* the namespace's block size, a power of two */
    uint32_t ms;      /* its metadata bytes */
    uint32_t nn;           /* namespaces */
    uint64_t nsze[32];     /* each namespace's size, 0 for an inactive one */
    uint32_t lbads_ns[32]; /* a namespace's block size, 0 for `lbads` */
    uint32_t drop_ident;   /* Identify of this namespace is taken, never answered */
    uint32_t cap_hi;  /* CAP's high half */
    uint32_t mqes;    /* CAP.MQES */
} m;

static uint32_t cc, csts, aqa;
static uint64_t asq, acq;
static unsigned enables, disables, aqa_before_disable, cc_entries_bad;

struct mq {
    int live;
    uint8_t *sq, *cq;
    uint32_t sq_size, cq_size;
    uint32_t sq_head, sq_tail, cq_head, cq_tail;
    uint32_t phase;
};

static struct mq q[2];
static unsigned created_cq, created_sq, cq_flags_bad, set_queues;
static unsigned io_commands, reads, writes, flushes, fua_writes, max_nlb, dropped;
static unsigned prp_faults, list_entries, overflows;
static uint16_t last_cid;

/* Completions the model keeps back until a row releases them. */
static struct {
    uint16_t cid, status;
} held[32];
static unsigned nheld;

static void model_post_now(int qid, uint16_t cid, uint16_t status, uint32_t result)
{
    struct mq *c = &q[qid];
    uint8_t *e;

    if (!c->live || (c->cq_tail + 1u) % c->cq_size == c->cq_head) {
        overflows++;
        return;
    }
    e = c->cq + c->cq_tail * 16u;
    put32(e, result);
    put32(e + 4, 0);
    put32(e + 8, c->sq_head | ((uint32_t)qid << 16));
    put32(e + 12, cid | ((uint32_t)((status << 1) | c->phase) << 16));
    if (++c->cq_tail == c->cq_size) {
        c->cq_tail = 0;
        c->phase ^= 1u;
    }
}

static void model_post(int qid, uint16_t cid, uint16_t status, uint32_t result)
{
    if (qid == 1 && m.defer && nheld < 32) {
        held[nheld].cid = cid;
        held[nheld].status = status;
        nheld++;
        return;
    }
    model_post_now(qid, cid, status, result);
}

/** Post what the model held back, oldest first. */
static void model_release(void)
{
    unsigned i;

    for (i = 0; i < nheld; i++) {
        model_post_now(1, held[i].cid, held[i].status, 0);
    }
    nheld = 0;
}

static void copy_string(uint8_t *page, uint32_t at, const char *s, uint32_t len)
{
    uint32_t i;

    for (i = 0; i < len; i++) {
        page[at + i] = *s ? (uint8_t)*s++ : (uint8_t)' ';
    }
}

/** Fill `page` with the Identify answer for `cns` and `nsid`; the status
 * the model answers. */
static uint16_t model_identify(uint8_t *page, uint32_t cns, uint32_t nsid)
{
    uint32_t i, n = 0;

    if (cns == 1) {
        copy_string(page, 4, "MODEL0042", 20);
        copy_string(page, 24, "Model NVMe Disk", 40);
        copy_string(page, 64, "1.0", 8);
        page[77] = (uint8_t)m.mdts;
        put32(page + 516, m.nn);
        return 0;
    }
    if (cns == 2) {
        if (m.no_active) {
            return 0x02;
        }
        for (i = 1; i <= m.nn; i++) {
            if (i > nsid && m.nsze[i - 1] != 0) {
                put32(page + 4 * n++, i);
            }
        }
        return 0;
    }
    if (cns != 0 || nsid == 0 || nsid > m.nn) {
        return 0x0B;
    }
    put32(page, (uint32_t)m.nsze[nsid - 1]);
    put32(page + 4, (uint32_t)(m.nsze[nsid - 1] >> 32));
    page[25] = 1; /* two formats */
    page[26] = 1; /* the second is in use */
    put32(page + 128, 9u << 16);
    put32(page + 132, m.ms | ((m.lbads_ns[nsid - 1] ? m.lbads_ns[nsid - 1] : m.lbads) << 16));
    return 0;
}

static void model_create(const uint8_t *e, uint8_t opcode)
{
    uint32_t cdw10 = get32(e + 40), cdw11 = get32(e + 44);
    uint32_t qid = cdw10 & 0xFFFFu, size = (cdw10 >> 16) + 1u;
    uint64_t prp1 = get64(e + 24);

    if (qid != 1 || !(cdw11 & 1u) || size > m.mqes + 1u) {
        model_post(0, (uint16_t)(get32(e) >> 16), 0x0101, 0);
        return;
    }
    if (opcode == 0x05) {
        created_cq++;
        if ((cdw11 & 2u) == 0 || (cdw11 >> 16) != 0) {
            cq_flags_bad++;
        }
        q[1].cq = host_of(prp1, size * 16u);
        q[1].cq_size = size;
        q[1].cq_head = q[1].cq_tail = 0;
        q[1].phase = 1;
    } else {
        created_sq++;
        if ((cdw11 >> 16) != 1 || created_cq == 0) {
            cq_flags_bad++;
        }
        q[1].sq = host_of(prp1, size * 64u);
        q[1].sq_size = size;
        q[1].sq_head = q[1].sq_tail = 0;
        q[1].live = q[1].sq && q[1].cq;
    }
    model_post(0, (uint16_t)(get32(e) >> 16), 0, 0);
}

static void model_admin(const uint8_t *e)
{
    uint8_t opcode = (uint8_t)get32(e);
    uint16_t cid = (uint16_t)(get32(e) >> 16);
    uint8_t *page;

    switch (opcode) {
    case 0x06:
        if ((get32(e + 40) & 0xFFu) == 0 && m.drop_ident != 0 && get32(e + 4) == m.drop_ident) {
            break;
        }
        page = host_of(get64(e + 24), 4096);
        model_post(0, cid, page ? model_identify(page, get32(e + 40) & 0xFFu, get32(e + 4)) : 0x06,
                   0);
        break;
    case 0x09:
        set_queues += (get32(e + 40) & 0xFFu) == 7u;
        model_post(0, cid, 0, 0);
        break;
    case 0x01:
    case 0x05:
        model_create(e, opcode);
        break;
    default:
        model_post(0, cid, 0x01, 0);
        break;
    }
}

/** Move `bytes` between the medium and the memory the PRP entries name,
 * checking each entry against the rules a controller checks it by. */
static void model_dma(const uint8_t *e, uint8_t *disk, uint32_t bytes, int to_host)
{
    uint64_t prp1 = get64(e + 24), prp2 = get64(e + 32);
    uint64_t at = prp1;
    uint32_t n = 0;
    const uint8_t *list = 0;

    if (prp1 & 3u) {
        prp_faults++;
        return;
    }
    while (bytes > 0) {
        uint32_t chunk = 4096u - (uint32_t)(at & 0xFFFu);
        uint8_t *p;

        chunk = chunk > bytes ? bytes : chunk;
        p = host_of(at, chunk);
        if (!p) {
            return;
        }
        if (to_host) {
            __builtin_memcpy(p, disk, chunk);
        } else {
            __builtin_memcpy(disk, p, chunk);
        }
        disk += chunk;
        bytes -= chunk;
        if (bytes == 0) {
            break;
        }
        if (at == prp1 && bytes <= 4096u) {
            at = prp2;
        } else {
            if (!list && (list = host_of(prp2, 8)) == 0) {
                return;
            }
            if (((uint32_t)prp2 & 0xFFFu) + 8u * n >= 4096u) {
                prp_faults++;
                return;
            }
            at = get64(list + 8u * n++);
        }
        if (at & 0xFFFu) {
            prp_faults++;
            return;
        }
    }
    list_entries = n > list_entries ? n : list_entries;
}

static void model_io(const uint8_t *e)
{
    uint8_t opcode = (uint8_t)get32(e);
    uint16_t cid = (uint16_t)(get32(e) >> 16);
    uint64_t slba = get64(e + 40);
    uint32_t nlb = (get32(e + 48) & 0xFFFFu) + 1u;

    io_commands++;
    last_cid = cid;
    if (m.drop_io) {
        dropped++;
        return;
    }
    if (m.fail_io > 0) {
        m.fail_io--;
        model_post(1, cid, 0x80, 0);
        return;
    }
    if (opcode == 0x00) {
        flushes++;
        model_post(1, cid, 0, 0);
        return;
    }
    if (get32(e + 4) != 1 || slba + nlb > MEDIUM) {
        model_post(1, cid, 0x80, 0);
        return;
    }
    max_nlb = nlb > max_nlb ? nlb : max_nlb;
    if (opcode == 0x02) {
        reads++;
    } else {
        writes++;
        fua_writes += (get32(e + 48) >> 30) & 1u;
    }
    model_dma(e, medium + slba * 512u, nlb * 512u, opcode == 0x02);
    model_post(1, cid, 0, 0);
}

static void model_ring(uint32_t qid, uint32_t tail)
{
    struct mq *c = &q[qid];

    if (!c->live || tail >= c->sq_size) {
        model_faults++;
        return;
    }
    c->sq_tail = tail;
    while (c->sq_head != c->sq_tail) {
        const uint8_t *e = c->sq + c->sq_head * 64u;

        c->sq_head = (c->sq_head + 1u) % c->sq_size;
        if (qid == 0) {
            model_admin(e);
        } else {
            model_io(e);
        }
    }
}

/** CC: enabling brings the admin queue up from AQA, ASQ and ACQ, and
 * disabling takes every queue down, as a controller reset does. */
static void model_cc(uint32_t v)
{
    uint32_t was = cc & 1u;

    cc = v;
    if (!was && (v & 1u)) {
        enables++;
        if ((v & 0x00FF0000u) != 0x00460000u || (v & 0x3F80u) != 0) {
            cc_entries_bad++;
        }
        if (m.never_ready || (enables > 1 && !m.ready_again)) {
            return;
        }
        if (m.fatal) {
            csts |= 2u;
            return;
        }
        q[0].sq = host_of(asq, 64u * ((aqa & 0xFFFu) + 1u));
        q[0].cq = host_of(acq, 16u * (((aqa >> 16) & 0xFFFu) + 1u));
        q[0].sq_size = (aqa & 0xFFFu) + 1u;
        q[0].cq_size = ((aqa >> 16) & 0xFFFu) + 1u;
        q[0].sq_head = q[0].sq_tail = q[0].cq_head = q[0].cq_tail = 0;
        q[0].phase = 1;
        q[0].live = q[0].sq && q[0].cq;
        csts |= 1u;
    } else if (was && !(v & 1u)) {
        disables++;
        if (m.stuck) {
            return;
        }
        csts = 0;
        q[0].live = q[1].live = 0;
        nheld = 0;
    }
}

static uint32_t model_r32(uintptr_t base, uint32_t off)
{
    (void)base;
    switch (off) {
    case 0x00:
        return m.mqes | (1u << 24); /* TO: 500 ms */
    case 0x04:
        return m.cap_hi;
    case 0x08:
        return 0x00010400u;
    case 0x14:
        return cc;
    case 0x1C:
        return csts;
    default:
        return 0;
    }
}

static void model_w32(uintptr_t base, uint32_t off, uint32_t v)
{
    (void)base;
    if (off >= 0x1000u) {
        uint32_t index = (off - 0x1000u) / 4u;

        if (index > 3u) {
            model_faults++;
        } else if (index % 2u == 0) {
            model_ring(index / 2u, v);
        } else {
            q[index / 2u].cq_head = v;
        }
        return;
    }
    switch (off) {
    case 0x14:
        model_cc(v);
        break;
    case 0x24:
        aqa = v;
        aqa_before_disable += disables == 0 && (cc & 1u);
        break;
    case 0x28:
        asq = (asq & ~0xFFFFFFFFull) | v;
        break;
    case 0x2C:
        asq = (asq & 0xFFFFFFFFull) | ((uint64_t)v << 32);
        break;
    case 0x30:
        acq = (acq & ~0xFFFFFFFFull) | v;
        break;
    case 0x34:
        acq = (acq & 0xFFFFFFFFull) | ((uint64_t)v << 32);
        break;
    default:
        break;
    }
}

static void model_udelay(uint32_t us)
{
    (void)us;
}

static const struct nvme_io model_io_ops = {model_r32, model_w32, model_udelay};

/* ---- what the driver reaches for outside itself ---- */

static int controllers = 1;

bool pci_find_device_skip(uint8_t cls, uint8_t sub, pci_device_t *skip, pci_device_t *out)
{
    int next = skip ? skip->device - 2 : 0;

    if (cls != PCI_CLASS_MASS_STORAGE || sub != 0x08 || next >= controllers) {
        return 0;
    }
    out->vendor_id = 0x1B36;
    out->device_id = 0x0010;
    out->bus = 0;
    out->device = (uint8_t)(next + 3);
    out->function = 0;
    out->prog_if = 0x02;
    out->bar[0] = 0xFEBD0004u; /* 64-bit memory */
    out->bar[1] = 0;
    out->bar[4] = 0xFEBE0004u;
    out->bar[5] = 0;
    return 1;
}

bool pci_bar_is_64bit(uint32_t bar)
{
    return (bar & 0x6u) == 0x4u;
}

bool pci_bar_is_memory(uint32_t bar)
{
    return (bar & 1u) == 0;
}

uint32_t pci_bar_address(uint32_t bar)
{
    return bar & ~0xFu;
}

/** The whole address of memory BAR `i`, as sys/pci.c reads it. */
uint64_t pci_bar_address64(const pci_device_t *dev, int i)
{
    uint64_t phys;

    if (i < 0 || i > 5 || !pci_bar_is_memory(dev->bar[i])) {
        return 0;
    }
    phys = pci_bar_address(dev->bar[i]);
    if (pci_bar_is_64bit(dev->bar[i])) {
        if (i == 5) {
            return 0;
        }
        phys |= (uint64_t)dev->bar[i + 1] << 32;
    }
    return phys;
}

static uint16_t pci_command;

uint16_t pci_read_config16(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off)
{
    (void)bus;
    (void)dev;
    (void)fn;
    return off == PCI_COMMAND ? pci_command : 0;
}

void pci_write_config16(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off, uint16_t v)
{
    (void)bus;
    (void)dev;
    (void)fn;
    if (off == PCI_COMMAND) {
        pci_command = v;
    }
}

void pci_enable_bus_master(pci_device_t *dev)
{
    (void)dev;
    pci_command |= PCI_CMD_BUS_MASTER;
}

void pci_enable_memory_space(pci_device_t *dev)
{
    (void)dev;
    pci_command |= PCI_CMD_MEMORY_SPACE;
}

static int msix_ok, msi_ok, msix_vector, msi_vector;
/* The holds of MSI-X, and how many completion queues each found made. */
static int msix_holds, msix_held_late;

int pci_msix_table(const pci_device_t *dev, uint8_t *bar, uint32_t *offset, uint32_t *entries)
{
    (void)dev;
    *bar = 4;
    *offset = 0x3000;
    *entries = 2;
    return msix_ok ? 0 : -1;
}

static uintptr_t mapped_last;

int pci_msix_hold(const pci_device_t *dev)
{
    (void)dev;
    msix_holds++;
    msix_held_late += created_cq != 0;
    return msix_ok ? 0 : -1;
}

int pci_msix_enable(const pci_device_t *dev, uintptr_t table, uint8_t vector)
{
    (void)dev;
    (void)table;
    msix_vector = vector;
    return 0;
}

int pci_msi_enable(const pci_device_t *dev, uint8_t vector)
{
    (void)dev;
    if (!msi_ok) {
        return -1;
    }
    msi_vector = vector;
    return 0;
}

uintptr_t map_mmio(uintptr_t phys, size_t size)
{
    (void)size;
    mapped_last = phys;
    return 0x10000u;
}

static int lapic_ok, vector_free, nvmepoll;
static isr_handler_t handler;
static const char *handler_name;

int isr_msi_ready(void)
{
    return lapic_ok;
}

int isr_alloc_vector(void)
{
    return vector_free ? 48 : -1;
}

void register_isr_handler(uint8_t vector, isr_handler_t h, const char *name)
{
    (void)vector;
    handler = h;
    handler_name = name;
}

int kcmdline_has(const char *flag)
{
    return nvmepoll && strcmp(flag, "rach2.nvmepoll") == 0;
}

void k_udelay(uint32_t us)
{
    (void)us;
}

uint32_t timer_now_us(void)
{
    return 0;
}

void latency_span(int kind, uint32_t from, uint32_t to)
{
    (void)kind;
    (void)from;
    (void)to;
}

static uint32_t masked;

uint32_t kirq_save(void)
{
    uint32_t old = masked;

    masked = 1;
    return old;
}

void kirq_restore(uint32_t flags)
{
    masked = flags;
}

int kirq_enabled(void)
{
    return !masked;
}

/* The log, kept so a row can ask whether a line was written. */
static char logged[64][200];
static unsigned nlogged;

void klog(int level, const char *subsystem, const char *format, ...)
{
    va_list ap;

    if (level > KLOG_INFO || strcmp(subsystem, "nvme") != 0) {
        return;
    }
    va_start(ap, format);
    vsnprintf(logged[nlogged % 64], sizeof(logged[0]), format, ap);
    va_end(ap);
    nlogged++;
}

static int was_logged(const char *needle)
{
    unsigned i;

    for (i = 0; i < nlogged && i < 64; i++) {
        if (strstr(logged[i], needle)) {
            return 1;
        }
    }
    return 0;
}

block_device_t *block0;
static unsigned polls, commands;

void block_count_command(void)
{
    commands++;
}

void block_count_flush(void)
{}

void block_count_poll(void)
{
    polls++;
}

static struct {
    struct block_request *r;
    uint32_t generation;
    int status;
} done[32];
static unsigned ndone;

void block_request_complete(struct block_request *request, uint32_t generation, int status)
{
    if (ndone < 32) {
        done[ndone].r = request;
        done[ndone].generation = generation;
        done[ndone].status = status;
    }
    ndone++;
}

static unsigned registered;
static const char *reg_transport;
static int reg_location;
static uint64_t reg_sectors;
static char reg_model[41];

struct disk *disk_register(block_device_t *dev, const char *transport, int location,
                           const volatile uint16_t *identify)
{
    int i;

    registered++;
    reg_transport = transport;
    reg_location = location;
    reg_sectors = dev->sector_count;
    for (i = 0; i < 40; i++) {
        uint16_t w = identify[27 + i / 2];

        reg_model[i] = (char)((i & 1) ? (w & 0xFF) : (w >> 8));
    }
    reg_model[40] = '\0';
    return 0;
}

static int policy_allows = 1;

int diskcmd_allowed(uint8_t command)
{
    return policy_allows || command == 0x25 || command == 0xEC;
}

const char diskcmd_policy_banner[] = "the model's policy";

static int expect_panic;

#include "../../rach2/sys/block/nvme.c"

void k_panic(const char *s)
{
    if (expect_panic &&
        strcmp(s, "nvme: recovery: the controller did not stop; DMA buffers still owned") == 0 &&
        masked && ndone == 0) {
        _exit(42);
    }
    printf("  panic: %s\n", s);
    _exit(43);
}

/* ---- the rows ---- */

static int fails;

#define CHECK(what, cond)                 \
    do {                                  \
        if (!(cond)) {                    \
            printf("  FAIL: %s\n", what); \
            fails++;                      \
        }                                 \
    } while (0)

/** A controller that does everything right, before a row changes one thing
 * about it, and a driver that has never seen it. */
static void model_reset(void)
{
    unsigned i;

    __builtin_memset(&m, 0, sizeof(m));
    __builtin_memset(q, 0, sizeof(q));
    __builtin_memset(nvme_ctrls, 0, sizeof(nvme_ctrls));
    nvme_nctrls = 0;
    nvme_messages = 0;
    m.mdts = 5;
    m.lbads = 9;
    m.nn = 1;
    m.nsze[0] = MEDIUM;
    m.cap_hi = 1u << 5; /* the NVM command set, DSTRD 0, MPSMIN 0 */
    m.mqes = 63;
    m.ready_again = 1;
    cc = csts = aqa = 0;
    asq = acq = 0;
    enables = disables = aqa_before_disable = cc_entries_bad = 0;
    created_cq = created_sq = cq_flags_bad = set_queues = 0;
    io_commands = reads = writes = flushes = fua_writes = max_nlb = dropped = 0;
    prp_faults = list_entries = overflows = model_faults = 0;
    nheld = ndone = registered = nlogged = 0;
    polls = commands = 0;
    arena_used = 0;
    frames_fail = permuted = 0;
    masked = 0;
    pci_command = 0;
    controllers = 1;
    msix_ok = msi_ok = lapic_ok = vector_free = 1;
    msix_vector = msi_vector = -1;
    msix_holds = msix_held_late = 0;
    nvmepoll = 0;
    policy_allows = 1;
    block0 = 0;
    for (i = 0; i < sizeof(medium); i++) {
        medium[i] = (uint8_t)(i * 7u + (i >> 9));
    }
    nvme_set_io(&model_io_ops);
}

static struct nvme_ns *ns0(void)
{
    return &nvme_ctrls[0].ns[0];
}

/** Bring-up: from a controller firmware left enabled to a namespace
 * registered as a disk. */
static void row_bring_up(void)
{
    model_reset();
    cc = 1;
    csts = 1;
    CHECK("a controller that answers is brought up", nvme_init() == 0);
    CHECK("the controller is disabled before its admin queue is programmed",
          disables >= 1 && aqa_before_disable == 0);
    CHECK("it is enabled once, with 64- and 16-byte entries and 4 KiB pages",
          enables == 1 && cc_entries_bad == 0 && (cc & 1u));
    CHECK("the admin queue is the driver's pages", asq == nvme_ctrls[0].admin.sq_phys &&
                                                       acq == nvme_ctrls[0].admin.cq_phys &&
                                                       (asq & 0xFFFu) == 0 && (acq & 0xFFFu) == 0);
    CHECK("one I/O queue is asked for", set_queues == 1);
    CHECK("its completion queue has interrupts on vector 0 and comes first",
          created_cq == 1 && created_sq == 1 && cq_flags_bad == 0);
    CHECK("MSI-X is held, every vector masked, before the completion queue is made",
          msix_holds == 1 && msix_held_late == 0);
    CHECK("the controller decodes memory, masters the bus and has INTx off",
          (pci_command & (PCI_CMD_MEMORY_SPACE | PCI_CMD_BUS_MASTER | PCI_CMD_INTERRUPT_DIS)) ==
              (PCI_CMD_MEMORY_SPACE | PCI_CMD_BUS_MASTER | PCI_CMD_INTERRUPT_DIS));
    CHECK("BAR0 is mapped", mapped_last == 0xFEBD0000u);
    CHECK("the namespace is registered as an nvme disk at location 0",
          registered == 1 && strcmp(reg_transport, "nvme") == 0 && reg_location == 0 &&
              reg_sectors == MEDIUM);
    CHECK("with the controller's model in the IDENTIFY words",
          strstr(reg_model, "Model NVMe Disk") == reg_model);
    CHECK("and it is block0", block0 == &ns0()->dev);
    CHECK("polled, fault-safe and write-through",
          ns0()->dev.flags == (BLOCK_FAULT_SAFE | BLOCK_POLLABLE | BLOCK_WRITE_THROUGH) &&
              ns0()->dev.submit == 0 && ns0()->dev.sector_size == 512);
    CHECK("MDTS 5 is 128 KiB a command", ns0()->dev.max_sectors == 256);
    CHECK("and the model saw nothing out of place", model_faults == 0 && overflows == 0);

    nvme_init();
    CHECK("a second probe does not reset a controller that is up", enables == 1 && registered == 2);
}

/** Controllers and namespaces the driver refuses, each by name, none of
 * which stops the probe. */
static void row_refusals(void)
{
    model_reset();
    m.never_ready = 1;
    CHECK("a controller that never becomes ready is refused", nvme_init() == -1);
    CHECK("by name", was_logged("unusable: the controller did not become ready"));
    CHECK("and left disabled", (cc & 1u) == 0 && registered == 0 && block0 == 0);

    model_reset();
    m.fatal = 1;
    CHECK("a controller with a fatal status is refused", nvme_init() == -1);
    CHECK("by name", was_logged("unusable: the controller reported a fatal status"));

    model_reset();
    m.cap_hi = 0;
    CHECK("a controller without the NVM command set is refused",
          nvme_init() == -1 && was_logged("no NVM command set") && enables == 0);

    model_reset();
    m.cap_hi |= 1u << 16;
    CHECK("a controller whose smallest page is 8 KiB is refused",
          nvme_init() == -1 && was_logged("smallest page is larger than 4 KiB"));

    model_reset();
    frames_fail = 1;
    CHECK("no pages for the queues is refused", nvme_init() == -1 && was_logged("no memory"));

    model_reset();
    m.lbads = 12;
    CHECK("a namespace of 4096-byte blocks is refused",
          nvme_init() == -1 && registered == 0 && was_logged("refused: blocks of 4096 bytes"));

    model_reset();
    m.ms = 8;
    CHECK("a namespace with metadata is refused",
          nvme_init() == -1 && was_logged("refused: a format with 8 bytes of metadata"));

    model_reset();
    m.nn = 3;
    m.nsze[1] = 0;
    m.nsze[2] = 512;
    m.no_active = 1;
    CHECK("without an active list every identifier is asked, and the inactive one passed over",
          nvme_init() == 0 && registered == 2 && reg_location == 1 && reg_sectors == 512);
}

/** A read and a write across a page boundary, and through a PRP list, with
 * the buffer's pages scattered in physical memory. */
static void row_transfers(void)
{
    block_device_t *dev;
    uint32_t i, start = 4096 - 1024;
    int ok = 1;

    model_reset();
    CHECK("the transfer row brings a controller up", nvme_init() == 0);
    dev = &ns0()->dev;
    permuted = 1;
    CHECK("a read of four sectors across a page boundary succeeds",
          dev->read(dev, 100, 4, big + start) == 0);
    for (i = 0; i < 4 * 512; i++) {
        ok = ok && *at_phys(start + i) == medium[100 * 512 + i];
    }
    CHECK("and lands in both scattered pages in order", ok);
    CHECK("with the second page in the second entry, not a list",
          list_entries == 0 && prp_faults == 0);

    ok = 1;
    for (i = 0; i < 40 * 512; i++) {
        *at_phys(start + i) = (uint8_t)(i * 13u + 5u);
    }
    CHECK("a write of forty sectors from mid-page succeeds",
          dev->write(dev, 1000, 40, big + start) == 0);
    for (i = 0; i < 40 * 512; i++) {
        ok = ok && medium[1000 * 512 + i] == (uint8_t)(i * 13u + 5u);
    }
    CHECK("and puts every scattered page on the medium in order", ok);
    CHECK("through a list of the pages past the first", list_entries == 5 && prp_faults == 0);
    CHECK("as one write, with FUA", writes == 1 && fua_writes == 1);
    CHECK("a read that runs past the namespace is refused",
          dev->read(dev, MEDIUM - 2, 4, big) == -EIO && reads == 1);
    CHECK("and the model saw nothing out of place", model_faults == 0 && overflows == 0);
}

/** A transfer past MDTS is split at it, and a buffer that is not aligned to
 * four bytes goes a page at a time through the bounce page. */
static void row_split_and_bounce(void)
{
    block_device_t *dev;
    uint32_t i;
    int ok = 1;

    model_reset();
    m.mdts = 1;
    CHECK("the split row brings a controller up", nvme_init() == 0);
    dev = &ns0()->dev;
    CHECK("MDTS 1 is 8 KiB a command", dev->max_sectors == 16);
    CHECK("a read of sixty sectors succeeds", dev->read(dev, 7, 60, big) == 0);
    CHECK("as four commands of at most sixteen sectors", reads == 4 && max_nlb == 16);
    for (i = 0; i < 60 * 512; i++) {
        ok = ok && big[i] == medium[7 * 512 + i];
    }
    CHECK("and the sixty sectors are in order", ok);

    model_reset();
    CHECK("the bounce row brings a controller up", nvme_init() == 0);
    dev = &ns0()->dev;
    ok = 1;
    CHECK("a read into an odd address succeeds", dev->read(dev, 20, 10, big + 1) == 0);
    for (i = 0; i < 10 * 512; i++) {
        ok = ok && big[1 + i] == medium[20 * 512 + i];
    }
    CHECK("through the bounce page, a page at a time", ok && reads == 2 && max_nlb == 8);
    for (i = 0; i < 3 * 512; i++) {
        big[3 + i] = (uint8_t)(i ^ 0x5A);
    }
    ok = 1;
    CHECK("a write from an odd address succeeds", dev->write(dev, 30, 3, big + 3) == 0);
    for (i = 0; i < 3 * 512; i++) {
        ok = ok && medium[30 * 512 + i] == (uint8_t)(i ^ 0x5A);
    }
    CHECK("and is on the medium", ok);
    CHECK("with interrupts as they were", masked == 0 && prp_faults == 0 && model_faults == 0);
}

/** A command the controller answers with an error fails, and the next one
 * works: an error is an answer, and nothing is reset for it. */
static void row_failed_command(void)
{
    block_device_t *dev;

    model_reset();
    CHECK("the failure row brings a controller up", nvme_init() == 0);
    dev = &ns0()->dev;
    m.fail_io = 1;
    CHECK("a read the controller fails returns EIO", dev->read(dev, 0, 1, big) == -EIO);
    CHECK("logged with the command and the status",
          was_logged("namespace 1: command 0x2 failed: status 0x80"));
    CHECK("the next read works", dev->read(dev, 0, 1, big) == 0);
    CHECK("without a reset", enables == 1 && disables == 0);
    policy_allows = 0;
    CHECK("a write the disk policy refuses is not sent", dev->write(dev, 0, 1, big) == -1 &&
                                                             io_commands == 2 &&
                                                             was_logged("refused: command 0x1"));
    CHECK("a read under that policy works", dev->read(dev, 0, 1, big) == 0);
    model_post_now(1, 0x7FF5, 0, 0);
    CHECK("a read after a completion for no command works", dev->read(dev, 0, 1, big) == 0);
    CHECK("and the stray completion is reported", was_logged("a completion for no command"));
}

/** A command never answered: the polled wait gives up, the controller is
 * reset and its queues created again, and the next command works. */
static void row_timeout(void)
{
    block_device_t *dev;

    model_reset();
    CHECK("the timeout row brings a controller up", nvme_init() == 0);
    dev = &ns0()->dev;
    m.drop_io = 1;
    polls = 0;
    CHECK("a read nobody answers returns EIO", dev->read(dev, 0, 1, big) == -EIO);
    CHECK("after thirty seconds of polls", polls >= NVME_CMD_STEPS);
    CHECK("logged as such", was_logged("command 0x2 failed: no answer in thirty seconds"));
    CHECK("the controller is reset and its queues made again",
          disables == 1 && enables == 2 && created_sq == 2 && set_queues == 2);
    CHECK("and logs that it answers again", was_logged("recovery: the controller answers again"));
    m.drop_io = 0;
    CHECK("the next read works", dev->read(dev, 5, 2, big) == 0 && big[0] == medium[5 * 512]);

    m.drop_io = 1;
    masked = 1;
    polls = 0;
    CHECK("with interrupts off the wait is shorter", dev->read(dev, 0, 1, big) == -EIO &&
                                                         polls >= NVME_CMD_STEPS_MASKED &&
                                                         polls < NVME_CMD_STEPS);
    CHECK("and the log says so", was_logged("no answer in five seconds with interrupts off"));
    masked = 0;

    m.ready_again = 0;
    CHECK("a controller that does not come back fails the command",
          dev->read(dev, 0, 1, big) == -EIO);
    CHECK("and is out of use", nvme_ctrls[0].dead && was_logged("recovery: failed"));
    io_commands = 0;
    m.drop_io = 0;
    CHECK("its next command answers EIO at once",
          dev->read(dev, 0, 1, big) == -EIO && io_commands == 0);
}

/** The controller does not stop for the recovery: a panic, with interrupts
 * masked, before any request is given back, since the controller may still
 * write into its buffer. */
static void row_stuck(void)
{
    int pid = fork();
    int status = 0;

    CHECK("the stuck row forks", pid >= 0);
    if (pid == 0) {
        static struct block_request r;
        block_device_t *dev;

        model_reset();
        if (nvme_init() != 0) {
            _exit(44);
        }
        nvme_start();
        dev = &ns0()->dev;
        r.lba = 0;
        r.count = 1;
        r.page = big;
        m.drop_io = 1;
        (void)dev->submit(dev, &r);
        m.stuck = 1;
        expect_panic = 1;
        (void)dev->read(dev, 0, 1, big + 512);
        _exit(45);
    }
    CHECK("the stuck child is reaped", waitpid(pid, &status, 0) == pid);
    CHECK("a controller that will not stop is a panic before its buffers are given back",
          status == (42 << 8));
}

/** `n` requests of one sector from `lba` into `big`, a kilobyte apart. */
static struct block_request reqs[12];

static void make_requests(int n, uint64_t lba, int write, uint32_t flags)
{
    int i;

    for (i = 0; i < n; i++) {
        __builtin_memset(&reqs[i], 0, sizeof(reqs[i]));
        reqs[i].lba = lba + (uint64_t)i;
        reqs[i].count = 1;
        reqs[i].write = write;
        reqs[i].flags = flags;
        reqs[i].page = big + 1024 * i;
        reqs[i].generation = 100u + (uint32_t)i;
    }
}

/** Every request in `reqs[0..n)` completed once, with its own generation and
 * `status`. */
static int all_done(int n, int status)
{
    int i, j, ok = (int)ndone == n;

    for (i = 0; i < n && ok; i++) {
        int seen = 0;

        for (j = 0; j < (int)ndone; j++) {
            seen += done[j].r == &reqs[i] && done[j].generation == reqs[i].generation &&
                    done[j].status == status;
        }
        ok = seen == 1;
    }
    return ok;
}

/** The interrupt-driven path: more requests than slots, completed by the
 * controller's messages, with the rest issued as slots come free. */
static void row_queued(void)
{
    block_device_t *dev;
    int i, ok = 1;

    model_reset();
    CHECK("the queued row brings a controller up", nvme_init() == 0);
    nvme_start();
    dev = &ns0()->dev;
    CHECK("the namespace goes over to its messages",
          was_logged("interrupts: messages on vector 48, msi-x, 1 disks queued") &&
              msix_vector == 48 && strcmp(handler_name, "nvme") == 0);
    CHECK("and is no longer fault-safe, but still pollable",
          (dev->flags & (BLOCK_FAULT_SAFE | BLOCK_POLLABLE)) == BLOCK_POLLABLE &&
              dev->submit != 0 && dev->flush != 0);
    m.defer = 1;
    make_requests(10, 200, 0, 0);
    for (i = 0; i < 10; i++) {
        CHECK("a request is taken", dev->submit(dev, &reqs[i]) == 0);
    }
    CHECK("as many are issued as there are slots", io_commands == 8 && nheld == 8 && ndone == 0);
    model_post_now(1, (uint16_t)(nvme_ctrls[0].slots[0].cid ^ 0x8u), 0, 0);
    handler(0);
    CHECK("a completion naming a busy slot under an old identifier completes nothing",
          ndone == 0 && was_logged("a completion for no command"));
    model_release();
    handler(0);
    CHECK("a message completes them and issues the rest", ndone == 8 && io_commands == 10);
    model_release();
    handler(0);
    CHECK("the next completes those", all_done(10, 0) && nvme_interrupts() == 3);
    for (i = 0; i < 10; i++) {
        ok = ok && reqs[i].page[0] == medium[(200 + i) * 512] &&
             reqs[i].page[511] == medium[(200 + i) * 512 + 511];
    }
    CHECK("each into its own page", ok);
    CHECK("with nothing out of place", model_faults == 0 && overflows == 0 && prp_faults == 0);
}

/**
 * Writes that leave the disk's cache unflushed, and what clears the mark:
 * a write that must be on the medium has a flush after it while the mark
 * is set, since FUA covers its own blocks alone; a flush clears the mark
 * when it finishes, not when it is issued, and not when a write was
 * answered unflushed while it ran; a failed flush, and one a recovery
 * fails, leave it set.
 */
static void row_flushes(void)
{
    block_device_t *dev;

    model_reset();
    CHECK("the flush row brings a controller up", nvme_init() == 0);
    nvme_start();
    dev = &ns0()->dev;
    make_requests(2, 300, 1, BLOCK_REQ_NOFLUSH);
    reqs[1].flags = 0;
    big[0] = 0xA1;
    CHECK("a write that may stay in the cache is taken", dev->submit(dev, &reqs[0]) == 0);
    handler(0);
    CHECK("is sent without FUA, and marks the disk", ndone == 1 && done[0].status == 0 &&
                                                         fua_writes == 0 && dev->unflushed == 1 &&
                                                         medium[300 * 512] == 0xA1);
    CHECK("a write that must be on the medium is sent with FUA",
          dev->submit(dev, &reqs[1]) == 0 && fua_writes == 1);
    m.defer = 1;
    handler(0);
    CHECK("and, with the disk marked, has a flush issued in its slot, not yet answered",
          flushes == 1 && ndone == 1 && dev->unflushed == 1);
    model_release();
    handler(0);
    CHECK("whose end completes the write and clears the mark",
          ndone == 2 && done[1].status == 0 && dev->unflushed == 0);
    m.defer = 0;
    CHECK("a write that must be on the medium on an unmarked disk is FUA alone",
          dev->submit(dev, &reqs[1]) == 0 && fua_writes == 2);
    handler(0);
    CHECK("with no flush", ndone == 3 && flushes == 1);

    make_requests(2, 310, 1, BLOCK_REQ_NOFLUSH);
    CHECK("an unflushed write marks the disk again",
          dev->submit(dev, &reqs[0]) == 0 && (handler(0), dev->unflushed == 1));
    m.defer = 1;
    make_requests(1, 0, 1, 0);
    CHECK("a flush is taken", dev->flush(dev, &reqs[0]) == 0 && flushes == 2);
    CHECK("and the mark stays while it runs, so that a second caller flushes too",
          dev->unflushed == 1);
    model_release();
    handler(0);
    CHECK("and is cleared when it finishes", ndone == 5 && done[4].status == 0 &&
                                                 dev->unflushed == 0);
    m.defer = 0;

    make_requests(2, 320, 1, BLOCK_REQ_NOFLUSH);
    CHECK("another unflushed write marks the disk",
          dev->submit(dev, &reqs[0]) == 0 && (handler(0), dev->unflushed == 1));
    m.defer = 1;
    make_requests(1, 0, 1, 0);
    CHECK("a flush is issued", dev->flush(dev, &reqs[0]) == 0 && flushes == 3);
    m.defer = 0;
    make_requests(1, 330, 1, BLOCK_REQ_NOFLUSH);
    reqs[0].generation = 300;
    CHECK("a write answered unflushed while it runs",
          dev->submit(dev, &reqs[0]) == 0 && (handler(0), ndone == 7));
    model_release();
    handler(0);
    CHECK("keeps the mark when the flush finishes", ndone == 8 && dev->unflushed == 1);

    make_requests(1, 0, 1, 0);
    m.fail_io = 1;
    CHECK("a flush the controller fails is taken", dev->flush(dev, &reqs[0]) == 0);
    handler(0);
    CHECK("completes with EIO and leaves the mark",
          ndone == 9 && done[8].status == -EIO && dev->unflushed == 1);
    CHECK("a flush by polling works and clears the mark",
          dev->flush(dev, 0) == 0 && dev->unflushed == 0 && flushes == 4);

    make_requests(1, 335, 1, BLOCK_REQ_NOFLUSH);
    CHECK("an unflushed write marks the disk once more",
          dev->submit(dev, &reqs[0]) == 0 && (handler(0), dev->unflushed == 1));
    CHECK("and a polled write has a flush after it, which clears the mark",
          dev->write(dev, 336, 1, big) == 0 && flushes == 5 && dev->unflushed == 0);

    make_requests(1, 340, 1, BLOCK_REQ_NOFLUSH);
    CHECK("a last unflushed write marks the disk",
          dev->submit(dev, &reqs[0]) == 0 && (handler(0), dev->unflushed == 1));
    m.drop_io = 1;
    make_requests(1, 0, 1, 0);
    CHECK("a flush nobody answers is issued", dev->flush(dev, &reqs[0]) == 0);
    CHECK("and a polled read nobody answers resets the controller",
          dev->read(dev, 0, 1, big) == -EIO && disables >= 1);
    CHECK("which fails the flush and leaves the mark",
          done[ndone - 1].status == -EIO && dev->unflushed == 1);
    m.drop_io = 0;
}

/** Namespaces past the sixteen one list page gives at a time: sixteen of
 * 4096-byte blocks, refused, and a seventeenth of 512-byte blocks, taken. */
static void row_scan(void)
{
    uint32_t i;

    model_reset();
    m.nn = 17;
    for (i = 0; i < 17; i++) {
        m.nsze[i] = MEDIUM;
        m.lbads_ns[i] = i < 16 ? 12u : 9u;
    }
    CHECK("a controller of seventeen namespaces comes up", nvme_init() == 0);
    CHECK("and publishes the seventeenth, past the first list's sixteen",
          nvme_ctrls[0].nns == 1 && ns0()->nsid == 17 && registered == 1);
    model_reset();
    m.nn = 17;
    m.no_active = 1;
    for (i = 0; i < 17; i++) {
        m.nsze[i] = MEDIUM;
        m.lbads_ns[i] = i < 16 ? 12u : 9u;
    }
    CHECK("so does one without the active list, counting to its seventeen",
          nvme_init() == 0 && nvme_ctrls[0].nns == 1 && ns0()->nsid == 17);
}

/** An Identify of a namespace the controller never answers: its page may
 * still be written, so the controller is stopped, out of use, and the scan
 * ends there. */
static void row_identify_timeout(void)
{
    model_reset();
    m.nn = 2;
    m.nsze[1] = MEDIUM;
    m.drop_ident = 1;
    CHECK("a controller whose Identify of namespace 1 is never answered is not used",
          nvme_init() != 0 || nvme_ctrls[0].nns == 0);
    CHECK("it is stopped and out of use, and namespace 2 is not asked",
          nvme_ctrls[0].dead && disables >= 1 && registered == 0 &&
              was_logged("an admin command was not answered"));
}

/** A polled command beside queued ones: it takes a slot of its own and does
 * not take their completions. With every slot held, it waits, and a
 * controller that answers none of them is reset, failing them first. */
static void row_polled_beside_queued(void)
{
    block_device_t *dev;
    int i;

    model_reset();
    CHECK("the mixed row brings a controller up", nvme_init() == 0);
    nvme_start();
    dev = &ns0()->dev;
    m.defer = 1;
    make_requests(3, 400, 0, 0);
    for (i = 0; i < 3; i++) {
        (void)dev->submit(dev, &reqs[i]);
    }
    m.defer = 0;
    masked = 1;
    CHECK("a polled read beside three queued ones works",
          dev->read(dev, 9, 1, big + 8192) == 0 && big[8192] == medium[9 * 512]);
    masked = 0;
    CHECK("and leaves their completions to them", ndone == 0 && nheld == 3);
    model_release();
    handler(0);
    CHECK("which complete at the message", all_done(3, 0));

    m.defer = 1;
    ndone = 0;
    make_requests(8, 500, 0, 0);
    for (i = 0; i < 8; i++) {
        (void)dev->submit(dev, &reqs[i]);
    }
    m.defer = 0;
    masked = 1;
    CHECK("a polled read with every slot held and never answered works after a reset",
          dev->read(dev, 11, 1, big + 8192) == 0 && big[8192] == medium[11 * 512]);
    masked = 0;
    CHECK("the reset failed the eight it held", all_done(8, -EIO));
    CHECK("and said why", was_logged("recovery: no command slot came free"));
}

/** The reasons a controller keeps polling, each in the log, and MSI when
 * there is no MSI-X. */
static void row_fallbacks(void)
{
    model_reset();
    msix_ok = 0;
    (void)nvme_init();
    nvme_start();
    CHECK("without MSI-X the controller takes MSI",
          msi_vector == 48 && was_logged("messages on vector 48, msi, 1 disks queued"));

    model_reset();
    msix_ok = msi_ok = 0;
    (void)nvme_init();
    nvme_start();
    CHECK("without either it polls, and the log says so",
          was_logged("polled: the controller has no message capability") &&
              (ns0()->dev.flags & BLOCK_FAULT_SAFE) && ns0()->dev.submit == 0 &&
              nvme_ctrls[0].vector == -1);

    model_reset();
    vector_free = 0;
    (void)nvme_init();
    nvme_start();
    CHECK("without a vector it polls",
          was_logged("polled: no vector for the controller's messages") &&
              (ns0()->dev.flags & BLOCK_FAULT_SAFE));

    model_reset();
    lapic_ok = 0;
    (void)nvme_init();
    nvme_start();
    CHECK("without the local APIC it polls", was_logged("polled: no vector"));

    model_reset();
    nvmepoll = 1;
    (void)nvme_init();
    nvme_start();
    CHECK("under rach2.nvmepoll it polls",
          was_logged("polled: rach2.nvmepoll") && (ns0()->dev.flags & BLOCK_FAULT_SAFE));
}

int main(void)
{
    row_bring_up();
    row_refusals();
    row_transfers();
    row_split_and_bounce();
    row_failed_command();
    row_timeout();
    row_stuck();
    row_queued();
    row_flushes();
    row_scan();
    row_identify_timeout();
    row_polled_beside_queued();
    row_fallbacks();
    if (fails == 0) {
        printf("nvme_hosttest: OK\n");
        return 0;
    }
    printf("nvme_hosttest: %d failure(s)\n", fails);
    return 1;
}
