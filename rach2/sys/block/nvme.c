/*
 * The NVMe driver. `nvme.h` says what it is for, and `sys/block/Readme.md`
 * describes the queues, the two paths a transfer takes and the recovery.
 */

#include "nvme.h"
#include "block.h"
#include "disk.h"
#include "diskcmd.h"

#include "../klog.h"
#include "../cmdline.h"
#include "../latency.h"
#include "../panic.h"
#include "../mm/paging.h"
#include "../pci.h"
#include "../irq.h"
#include "../timer.h"
#include "../udelay.h"

#include "../lib/kerrno.h"
#include "../lib/kint.h"
#include "../lib/kirq.h"

/* PCI class 01 (mass storage), subclass 08 (non-volatile memory), interface
   02 (NVM Express). */
#define NVME_PCI_SUBCLASS 0x08
#define NVME_PCI_PROGIF   0x02

/* The controller registers, as offsets into BAR0. CAP, ASQ and ACQ are 64
   bits wide and are reached as two halves, low first. */
#define NVME_CAP       0x00
#define NVME_VS        0x08
#define NVME_CC        0x14
#define NVME_CSTS      0x1C
#define NVME_AQA       0x24
#define NVME_ASQ       0x28
#define NVME_ACQ       0x30
#define NVME_DOORBELLS 0x1000

/* The fields of CAP, from its low half and its high half. */
#define NVME_CAP_MQES(lo)    ((lo) & 0xFFFFu)
#define NVME_CAP_TO(lo)      (((lo) >> 24) & 0xFFu)
#define NVME_CAP_DSTRD(hi)   ((hi) & 0xFu)
#define NVME_CAP_CSS_NVM(hi) (((hi) >> 5) & 1u)
#define NVME_CAP_MPSMIN(hi)  (((hi) >> 16) & 0xFu)

/* CC with 64-byte submission entries and 16-byte completion entries; the
   command set, the page size and the arbitration are left at zero: NVM,
   4 KiB and round robin. */
#define NVME_CC_EN      0x1u
#define NVME_CC_ENTRIES ((6u << 16) | (4u << 20))
#define NVME_CSTS_RDY   0x1u
#define NVME_CSTS_CFS   0x2u

/* Admin commands, the NVM commands, and the arguments this driver gives
   them. */
#define NVME_ADMIN_CREATE_SQ    0x01
#define NVME_ADMIN_CREATE_CQ    0x05
#define NVME_ADMIN_IDENTIFY     0x06
#define NVME_ADMIN_SET_FEATURES 0x09
#define NVME_FEATURE_QUEUES     0x07
#define NVME_CNS_NAMESPACE      0x00
#define NVME_CNS_CONTROLLER     0x01
#define NVME_CNS_ACTIVE         0x02
#define NVME_QUEUE_CONTIGUOUS   0x1u
#define NVME_QUEUE_INTERRUPTS   0x2u
#define NVME_CMD_FLUSH          0x00
#define NVME_CMD_WRITE          0x01
#define NVME_CMD_READ           0x02
#define NVME_RW_FUA             (1u << 30)

/* The sizes this driver chooses. A queue is one page or less, so it is
   physically contiguous whatever frame it is given; each command slot has a
   page of its own for its PRP list, which holds the 256 entries a transfer
   of the block layer's 1 MiB limit needs past its first page. The low three
   bits of a command identifier are its slot. */
#define NVME_ADMIN_DEPTH 8u
#define NVME_IO_DEPTH    16u
#define NVME_SLOTS       8u
#define NVME_SLOT_MASK   (NVME_SLOTS - 1u)
#define NVME_NAMESPACES  4
#define NVME_CONTROLLERS 4
#define NVME_LIST_MAX    16
#define NVME_EXAMINE_MAX 1024u
#define NVME_PRP_ENTRIES (PAGE_SIZE / 8u)
#define NVME_MAP_SIZE    0x2000u

/* The waits. A command is polled every 10 us for thirty seconds with
   interrupts on and five with them off, AHCI's bounds and for its reasons;
   CSTS is polled every millisecond for as long as CAP.TO allows, in its
   units of 500 ms. */
#define NVME_POLL_US          10u
#define NVME_CMD_STEPS        3000000u
#define NVME_CMD_STEPS_MASKED 500000u
#define NVME_READY_POLL_US    1000u
#define NVME_READY_STEPS_PER  500u

/* The status a polled command is given when recovery took its slot: not a
   value `nvme_status` returns, which is eleven bits. */
#define NVME_STATUS_RESET 0xFFFFu

/** A submission queue entry, as the controller reads it. */
struct nvme_sqe {
    uint32_t cdw0; /* opcode in 7:0, command identifier in 31:16 */
    uint32_t nsid;
    uint32_t cdw2, cdw3;
    uint64_t mptr;
    uint64_t prp1, prp2;
    uint32_t cdw10, cdw11, cdw12, cdw13, cdw14, cdw15;
};

/** A completion queue entry, as the controller writes it. */
struct nvme_cqe {
    uint32_t result;
    uint32_t reserved;
    uint16_t sq_head;
    uint16_t sq_id;
    uint16_t cid;
    uint16_t status; /* the phase in bit 0, the status in 15:1 */
};

typedef char nvme_sqe_is_64_bytes[sizeof(struct nvme_sqe) == 64 ? 1 : -1];
typedef char nvme_cqe_is_16_bytes[sizeof(struct nvme_cqe) == 16 ? 1 : -1];

/** One queue pair: the admin pair, or the I/O pair. */
struct nvme_queue {
    volatile struct nvme_sqe *sq;
    volatile struct nvme_cqe *cq;
    uint64_t sq_phys, cq_phys;
    uint16_t qid;
    uint16_t depth;
    uint16_t sq_tail;
    uint16_t cq_head;
    uint16_t phase;
};

struct nvme_ctrl;

/** A namespace, published as a disk. */
struct nvme_ns {
    struct nvme_ctrl *ctrl;
    uint32_t nsid;
    block_device_t dev;
    struct block_request *queue_head, *queue_tail;
    /* The writes answered without a flush, counted, so that a flush clears
     * the disk's `unflushed` mark only when none was answered after it was
     * issued (`nvme_flushed`). */
    uint32_t wseq;
};

/**
 * A command in flight on the controller: a request of the interrupt-driven
 * path, or, with `request` NULL, a polled command whose caller reads `done`
 * and `status`.
 */
struct nvme_slot {
    int busy;
    int flushing;  /* the flush half of a write the request wants durable */
    uint32_t fseq; /* the namespace's `wseq` when this flush was issued */
    uint16_t cid;
    uint8_t opcode;
    struct nvme_ns *ns;
    struct block_request *request;
    uint32_t generation;
    volatile int done;
    volatile uint16_t status;
    volatile uint64_t *prp_list;
    uint64_t prp_phys;
};

/** One controller on the bus. */
struct nvme_ctrl {
    int number;
    pci_device_t pci;
    uint64_t bar_phys;
    uintptr_t base;
    int up;        /* brought up, its namespaces taken */
    int dead;      /* recovery failed; every command answers EIO */
    int resetting; /* recovery is running; the handler leaves it alone */
    int vector;    /* -1 while polled */
    uint32_t stride;
    uint32_t ready_steps;
    uint32_t max_sectors;
    uint32_t nslots;
    uint16_t admin_cid;
    uint16_t seq;
    struct nvme_queue admin, io;
    struct nvme_slot slots[NVME_SLOTS];
    volatile uint8_t *identify;
    uint64_t identify_phys;
    uint8_t *bounce;
    uint64_t bounce_phys;
    uint8_t strings[72]; /* Identify Controller's bytes 0-71: SN, MN, FR */
    uint32_t nn;
    int nns;
    int next_ns;
    struct nvme_ns ns[NVME_NAMESPACES];
};

static struct nvme_ctrl nvme_ctrls[NVME_CONTROLLERS];
static int nvme_nctrls;
static uint32_t nvme_messages;

/* Why the last bring-up step failed, for the line that reports it. */
static const char *nvme_why = "";

/*
 * The registers as the driver reaches them: through the mapped BAR on a
 * machine, through a model in the host test.
 */
static uint32_t nvme_mmio_r32(uintptr_t base, uint32_t offset)
{
    return mmio_read32(base + offset);
}

static void nvme_mmio_w32(uintptr_t base, uint32_t offset, uint32_t value)
{
    mmio_write32(base + offset, value);
}

static const struct nvme_io nvme_mmio_io = {nvme_mmio_r32, nvme_mmio_w32, k_udelay};

static const struct nvme_io *nvme_io = &nvme_mmio_io;

void nvme_set_io(const struct nvme_io *ops)
{
    nvme_io = ops ? ops : &nvme_mmio_io;
}

uint32_t nvme_interrupts(void)
{
    return nvme_messages;
}

static uint32_t nvme_r32(const struct nvme_ctrl *c, uint32_t offset)
{
    return nvme_io->r32(c->base, offset);
}

static void nvme_w32(const struct nvme_ctrl *c, uint32_t offset, uint32_t value)
{
    nvme_io->w32(c->base, offset, value);
}

/** Write a 64-bit register as two halves, low first. */
static void nvme_w64(const struct nvme_ctrl *c, uint32_t offset, uint64_t value)
{
    nvme_w32(c, offset, (uint32_t)value);
    nvme_w32(c, offset + 4u, (uint32_t)(value >> 32));
}

/** The bus address of the byte at `p`, or 0 when nothing is mapped there. */
static uint64_t nvme_phys(const volatile void *p)
{
    return (uint64_t)paging_virt_to_phys((uintptr_t)p);
}

/** A little-endian field of a page the controller wrote. */
static uint32_t nvme_le32(const volatile uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint64_t nvme_le64(const volatile uint8_t *p)
{
    return (uint64_t)nvme_le32(p) | ((uint64_t)nvme_le32(p + 4) << 32);
}

/** The status field of a completion: its type and code, without the
 * phase. Zero is success. */
static uint16_t nvme_status(uint16_t raw)
{
    return (uint16_t)((raw >> 1) & 0x7FFu);
}

/** The polled wait's bound in steps, by whether interrupts are on. */
static uint32_t nvme_cmd_steps(void)
{
    return kirq_enabled() ? NVME_CMD_STEPS : NVME_CMD_STEPS_MASKED;
}

static void nvme_cmd_clear(struct nvme_sqe *cmd)
{
    cmd->cdw0 = cmd->nsid = cmd->cdw2 = cmd->cdw3 = 0;
    cmd->mptr = cmd->prp1 = cmd->prp2 = 0;
    cmd->cdw10 = cmd->cdw11 = cmd->cdw12 = cmd->cdw13 = cmd->cdw14 = cmd->cdw15 = 0;
}

/** Copy `cmd` into `q`'s next submission entry and ring its doorbell. */
static void nvme_push(const struct nvme_ctrl *c, struct nvme_queue *q, const struct nvme_sqe *cmd)
{
    volatile struct nvme_sqe *e = &q->sq[q->sq_tail];

    e->cdw0 = cmd->cdw0;
    e->nsid = cmd->nsid;
    e->cdw2 = cmd->cdw2;
    e->cdw3 = cmd->cdw3;
    e->mptr = cmd->mptr;
    e->prp1 = cmd->prp1;
    e->prp2 = cmd->prp2;
    e->cdw10 = cmd->cdw10;
    e->cdw11 = cmd->cdw11;
    e->cdw12 = cmd->cdw12;
    e->cdw13 = cmd->cdw13;
    e->cdw14 = cmd->cdw14;
    e->cdw15 = cmd->cdw15;
    q->sq_tail = (uint16_t)((q->sq_tail + 1u) % q->depth);
    block_count_command();
    nvme_w32(c, NVME_DOORBELLS + 2u * q->qid * c->stride, q->sq_tail);
}

/**
 * Take `q`'s next completion into `out` and tell the controller its entry
 * is free. Returns 0 when the controller has posted nothing new: the entry
 * at the head still has the phase of the last pass.
 */
static int nvme_pop(const struct nvme_ctrl *c, struct nvme_queue *q, struct nvme_cqe *out)
{
    volatile struct nvme_cqe *e = &q->cq[q->cq_head];
    uint16_t status = e->status;

    if ((status & 1u) != q->phase) {
        return 0;
    }
    out->result = e->result;
    out->reserved = e->reserved;
    out->sq_head = e->sq_head;
    out->sq_id = e->sq_id;
    out->cid = e->cid;
    out->status = status;
    q->cq_head = (uint16_t)(q->cq_head + 1u);
    if (q->cq_head == q->depth) {
        q->cq_head = 0;
        q->phase ^= 1u;
    }
    nvme_w32(c, NVME_DOORBELLS + (2u * q->qid + 1u) * c->stride, q->cq_head);
    return 1;
}

/** Empty `q` for a controller that has just been enabled: both indexes at
 * zero, and every completion entry zero, so the first pass's phase of one
 * tells a posted entry from an old one. */
static void nvme_queue_reset(struct nvme_queue *q)
{
    uint32_t i;

    q->sq_tail = 0;
    q->cq_head = 0;
    q->phase = 1;
    for (i = 0; i < q->depth; i++) {
        q->cq[i].status = 0;
    }
}

static int nvme_disable(struct nvme_ctrl *c);

/**
 * Run one admin command by polling.
 *
 * Returns 0 with the completion's first dword in `*result`, the status the
 * controller answered when it is not success, or -1 when it did not answer
 * within the wait, which leaves the controller stopped and out of use, or
 * when it was out of use already. Admin commands run one at a time, at
 * bring-up and in recovery, so a completion for another identifier is an
 * old one and is passed over.
 */

static int nvme_admin(struct nvme_ctrl *c, struct nvme_sqe *cmd, uint32_t *result)
{
    struct nvme_cqe e;
    uint32_t steps = nvme_cmd_steps();
    uint16_t cid;

    if (c->dead) {
        return -1;
    }
    cid = c->admin_cid++;
    cmd->cdw0 = (cmd->cdw0 & 0xFFFFu) | ((uint32_t)cid << 16);
    nvme_push(c, &c->admin, cmd);
    while (steps-- > 0) {
        block_count_poll();
        while (nvme_pop(c, &c->admin, &e)) {
            if (e.cid != cid) {
                continue;
            }
            if (nvme_status(e.status) != 0) {
                return nvme_status(e.status);
            }
            if (result) {
                *result = e.result;
            }
            return 0;
        }
        nvme_io->udelay(NVME_POLL_US);
    }
    /* The command may still write into the page it was given, the identify
     * page among them, after this: the controller is stopped and out of
     * use, so that no later command reads a page it may still write. */
    klog(KLOG_WARN, "nvme", "%d: an admin command was not answered; the controller is out of use",
         c->number);
    c->dead = 1;
    (void)nvme_disable(c);
    return -1;
}

/** Ask the controller to identify itself (`cns` 1), namespace `nsid` (0),
 * or its active namespaces (2), into the identify page. Returns what
 * `nvme_admin` does. */
static int nvme_identify(struct nvme_ctrl *c, uint32_t cns, uint32_t nsid)
{
    struct nvme_sqe cmd;
    uint32_t i;

    if (!diskcmd_allowed(DISKCMD_IDENTIFY)) {
        return -1;
    }
    for (i = 0; i < PAGE_SIZE; i++) {
        c->identify[i] = 0;
    }
    nvme_cmd_clear(&cmd);
    cmd.cdw0 = NVME_ADMIN_IDENTIFY;
    cmd.nsid = nsid;
    cmd.prp1 = c->identify_phys;
    cmd.cdw10 = cns;
    return nvme_admin(c, &cmd, 0);
}

/**
 * Wait for CSTS.RDY to read `want`, for as long as CAP.TO allows. A
 * register that reads all ones is a controller that has gone from the bus,
 * and a fatal status while it is being enabled is one that will not come
 * up; both end the wait at once.
 */
static int nvme_wait_ready(struct nvme_ctrl *c, uint32_t want)
{
    uint32_t steps;

    for (steps = 0; steps < c->ready_steps; steps++) {
        uint32_t csts = nvme_r32(c, NVME_CSTS);

        if (csts == 0xFFFFFFFFu) {
            nvme_why = "the controller does not answer";
            return -1;
        }
        if ((csts & NVME_CSTS_RDY) == want) {
            return 0;
        }
        if (want && (csts & NVME_CSTS_CFS)) {
            nvme_why = "the controller reported a fatal status";
            return -1;
        }
        nvme_io->udelay(NVME_READY_POLL_US);
    }
    nvme_why = want ? "the controller did not become ready" : "the controller did not stop";
    return -1;
}

/**
 * Disable the controller and wait until CSTS.RDY reads 0. A controller
 * still on its way up is let finish first, since changing EN before RDY
 * has followed it is undefined.
 */
static int nvme_disable(struct nvme_ctrl *c)
{
    uint32_t cc = nvme_r32(c, NVME_CC);

    if ((cc & NVME_CC_EN) && !(nvme_r32(c, NVME_CSTS) & NVME_CSTS_RDY)) {
        (void)nvme_wait_ready(c, 1);
    }
    nvme_w32(c, NVME_CC, cc & ~NVME_CC_EN);
    return nvme_wait_ready(c, 0);
}

/** Program the admin queue and enable the controller. */
static int nvme_enable(struct nvme_ctrl *c)
{
    nvme_queue_reset(&c->admin);
    nvme_w32(c, NVME_AQA, ((NVME_ADMIN_DEPTH - 1u) << 16) | (NVME_ADMIN_DEPTH - 1u));
    nvme_w64(c, NVME_ASQ, c->admin.sq_phys);
    nvme_w64(c, NVME_ACQ, c->admin.cq_phys);
    nvme_w32(c, NVME_CC, NVME_CC_ENTRIES);
    nvme_w32(c, NVME_CC, NVME_CC_ENTRIES | NVME_CC_EN);
    return nvme_wait_ready(c, 1);
}

/**
 * Ask for one I/O queue pair and create it: the completion queue first,
 * with its interrupts on vector 0, then the submission queue on it. The
 * messages reach the processor only once `nvme_start` enables them.
 */
static int nvme_create_io_queues(struct nvme_ctrl *c)
{
    struct nvme_sqe cmd;

    nvme_queue_reset(&c->io);
    nvme_cmd_clear(&cmd);
    cmd.cdw0 = NVME_ADMIN_SET_FEATURES;
    cmd.cdw10 = NVME_FEATURE_QUEUES;
    if (nvme_admin(c, &cmd, 0) != 0) {
        nvme_why = "the controller gave no I/O queue";
        return -1;
    }
    nvme_cmd_clear(&cmd);
    cmd.cdw0 = NVME_ADMIN_CREATE_CQ;
    cmd.prp1 = c->io.cq_phys;
    cmd.cdw10 = ((uint32_t)(c->io.depth - 1u) << 16) | c->io.qid;
    cmd.cdw11 = NVME_QUEUE_INTERRUPTS | NVME_QUEUE_CONTIGUOUS;
    if (nvme_admin(c, &cmd, 0) != 0) {
        nvme_why = "the I/O completion queue was refused";
        return -1;
    }
    nvme_cmd_clear(&cmd);
    cmd.cdw0 = NVME_ADMIN_CREATE_SQ;
    cmd.prp1 = c->io.sq_phys;
    cmd.cdw10 = ((uint32_t)(c->io.depth - 1u) << 16) | c->io.qid;
    cmd.cdw11 = ((uint32_t)c->io.qid << 16) | NVME_QUEUE_CONTIGUOUS;
    if (nvme_admin(c, &cmd, 0) != 0) {
        nvme_why = "the I/O submission queue was refused";
        return -1;
    }
    return 0;
}

/**
 * Map BAR0 and make the controller decode it and master the bus, with its
 * INTx line off and its MSI-X held, enabled with every vector masked: the
 * queues are created with interrupts enabled, and until `nvme_start` enables
 * messages they must reach no line. MSI-X is on before the queues are made
 * because a controller may tie a queue's vector only then; QEMU's does, and
 * one whose queue was made with MSI-X off never sends its messages. A BAR
 * the kernel cannot map, one above its addresses on i686 among them, is
 * refused.
 */
static int nvme_map(struct nvme_ctrl *c)
{
    uint64_t phys = pci_bar_address64(&c->pci, 0);
    uintptr_t mapped;
    uint16_t command;

    if (phys == 0) {
        nvme_why = "BAR0 is not a programmed memory window";
        return -1;
    }
    if ((uint64_t)(uintptr_t)phys != phys) {
        nvme_why = "BAR0 lies above the addresses this kernel maps";
        return -1;
    }
    if (!c->base || c->bar_phys != phys) {
        mapped = map_mmio((uintptr_t)phys, NVME_MAP_SIZE);
        if (!mapped) {
            nvme_why = "BAR0 could not be mapped";
            return -1;
        }
        c->base = mapped;
        c->bar_phys = phys;
    }
    pci_enable_memory_space(&c->pci);
    command = pci_read_config16(c->pci.bus, c->pci.device, c->pci.function, PCI_COMMAND);
    pci_write_config16(c->pci.bus, c->pci.device, c->pci.function, PCI_COMMAND,
                       (uint16_t)(command | PCI_CMD_INTERRUPT_DIS));
    pci_enable_bus_master(&c->pci);
    (void)pci_msix_hold(&c->pci);
    return 0;
}

/** A zeroed page from the kernel's allocator, with its bus address in
 * `*phys`, or NULL. */
static void *nvme_page(uint64_t *phys)
{
    void *frame = alloc_kernel_frame();
    volatile uint8_t *p = (volatile uint8_t *)frame;
    uint32_t i;

    if (!frame) {
        return 0;
    }
    for (i = 0; i < PAGE_SIZE; i++) {
        p[i] = 0;
    }
    *phys = nvme_phys(frame);
    return frame;
}

/**
 * The pages `c` needs, each taken once and kept for the life of the
 * kernel: the four queues, the identify page, the bounce page and a PRP
 * list for each slot. A controller that failed may still write into what
 * it was given, so nothing given to one is freed.
 */
static int nvme_alloc(struct nvme_ctrl *c)
{
    uint32_t i;

    if (!c->admin.sq) {
        c->admin.sq = nvme_page(&c->admin.sq_phys);
    }
    if (!c->admin.cq) {
        c->admin.cq = nvme_page(&c->admin.cq_phys);
    }
    if (!c->io.sq) {
        c->io.sq = nvme_page(&c->io.sq_phys);
    }
    if (!c->io.cq) {
        c->io.cq = nvme_page(&c->io.cq_phys);
    }
    if (!c->identify) {
        c->identify = nvme_page(&c->identify_phys);
    }
    if (!c->bounce) {
        c->bounce = nvme_page(&c->bounce_phys);
    }
    for (i = 0; i < NVME_SLOTS; i++) {
        if (!c->slots[i].prp_list) {
            c->slots[i].prp_list = nvme_page(&c->slots[i].prp_phys);
        }
        if (!c->slots[i].prp_list) {
            break;
        }
    }
    if (!c->admin.sq || !c->admin.cq || !c->io.sq || !c->io.cq || !c->identify || !c->bounce ||
        i < NVME_SLOTS) {
        nvme_why = "no memory for the queues";
        return -1;
    }
    return 0;
}

/** The most sectors one command may move, from MDTS: a power of two of the
 * smallest page, 4 KiB here, with 0 for no limit, held to the block
 * layer's. */
static uint32_t nvme_max_sectors(uint8_t mdts)
{
    if (mdts == 0 || mdts >= 8) {
        return BLOCK_REQ_MAX_SECTORS;
    }
    return 8u << mdts;
}

/**
 * Read CAP and size the driver's use of the controller: the doorbell
 * stride, the ready timeout, the I/O queue's depth and the slots. A
 * controller without the NVM command set, or whose smallest page is larger
 * than the 4 KiB this driver uses, is refused.
 */
static int nvme_read_cap(struct nvme_ctrl *c)
{
    uint32_t lo = nvme_r32(c, NVME_CAP);
    uint32_t hi = nvme_r32(c, NVME_CAP + 4u);
    uint32_t to = NVME_CAP_TO(lo);
    uint32_t entries = NVME_CAP_MQES(lo) + 1u;

    if (lo == 0xFFFFFFFFu && hi == 0xFFFFFFFFu) {
        nvme_why = "the controller does not answer";
        return -1;
    }
    if (!NVME_CAP_CSS_NVM(hi)) {
        nvme_why = "the controller has no NVM command set";
        return -1;
    }
    if (NVME_CAP_MPSMIN(hi) != 0) {
        nvme_why = "the controller's smallest page is larger than 4 KiB";
        return -1;
    }
    c->stride = 4u << NVME_CAP_DSTRD(hi);
    if (NVME_DOORBELLS + 4u * c->stride > NVME_MAP_SIZE) {
        nvme_why = "the doorbells lie past the registers this driver maps";
        return -1;
    }
    if (entries < 2u) {
        nvme_why = "the controller's queues hold a single entry";
        return -1;
    }
    c->ready_steps = (to ? to : 1u) * NVME_READY_STEPS_PER;
    c->admin.qid = 0;
    c->admin.depth = (uint16_t)NVME_ADMIN_DEPTH;
    c->io.qid = 1;
    c->io.depth = (uint16_t)(entries < NVME_IO_DEPTH ? entries : NVME_IO_DEPTH);
    c->nslots = c->io.depth - 1u < NVME_SLOTS ? c->io.depth - 1u : NVME_SLOTS;
    return 0;
}

/** Identify the controller and keep what the driver uses of the answer:
 * the strings, MDTS and the number of namespaces. */
static int nvme_identify_controller(struct nvme_ctrl *c)
{
    uint32_t i;

    if (nvme_identify(c, NVME_CNS_CONTROLLER, 0) != 0) {
        nvme_why = "the controller did not answer Identify";
        return -1;
    }
    for (i = 0; i < sizeof(c->strings); i++) {
        c->strings[i] = c->identify[i];
    }
    c->max_sectors = nvme_max_sectors(c->identify[77]);
    c->nn = nvme_le32(c->identify + 516);
    return 0;
}

/**
 * Bring `c` up from whatever state firmware left it in: disabled, its
 * admin queue programmed, enabled, identified, and its I/O queue pair
 * created. A controller that fails after it was enabled is disabled again,
 * so it does nothing with the pages it was given.
 */
static int nvme_bring_up(struct nvme_ctrl *c)
{
    uint32_t vs;
    uint32_t i;

    if (nvme_read_cap(c) != 0 || nvme_disable(c) != 0) {
        return -1;
    }
    for (i = 0; i < NVME_SLOTS; i++) {
        c->slots[i].busy = 0;
    }
    if (nvme_enable(c) != 0) {
        (void)nvme_disable(c);
        return -1;
    }
    if (nvme_identify_controller(c) != 0 || nvme_create_io_queues(c) != 0) {
        (void)nvme_disable(c);
        return -1;
    }
    vs = nvme_r32(c, NVME_VS);
    klog(KLOG_INFO, "nvme", "%d: ready: version %d.%d, %d slots, %d KiB a command", c->number,
         (int)(vs >> 16), (int)((vs >> 8) & 0xFFu), (int)c->nslots, (int)(c->max_sectors / 2u));
    return 0;
}

/** The block size of the namespace the identify page describes, as a power
 * of two, with its bytes of metadata in `*ms`; 0 when FLBAS selects a
 * format past those the namespace lists. */
static uint32_t nvme_lba_shift(const volatile uint8_t *id, uint32_t *ms)
{
    uint32_t nlbaf = id[25];
    uint32_t flbas = id[26];
    uint32_t format = flbas & 0xFu;
    uint32_t lbaf;

    if (nlbaf >= 16u) {
        format |= ((flbas >> 5) & 0x3u) << 4;
    }
    if (format > nlbaf) {
        return 0;
    }
    lbaf = nvme_le32(id + 128u + 4u * format);
    *ms = lbaf & 0xFFFFu;
    return (lbaf >> 16) & 0xFFu;
}

static int nvme_read(block_device_t *device, uint64_t lba, uint32_t count, void *buffer);
static int nvme_write(block_device_t *device, uint64_t lba, uint32_t count, const void *buffer);

/**
 * Identify namespace `nsid` and take it as a disk when the block layer can
 * drive it: blocks of 512 bytes and no metadata. A namespace of 4096-byte
 * blocks is refused: the partition tables on it and the firmware that reads
 * them count in its own blocks, and the block layer counts in 512-byte
 * sectors, so a table written here would be at the wrong place for anyone
 * else. An inactive namespace, of no size, is passed over in silence.
 */
static void nvme_take_namespace(struct nvme_ctrl *c, uint32_t nsid)
{
    struct nvme_ns *ns;
    uint64_t nsze;
    uint32_t shift, ms = 0;

    if (nvme_identify(c, NVME_CNS_NAMESPACE, nsid) != 0) {
        klog(KLOG_WARN, "nvme", "%d: namespace %d: refused: it did not answer Identify", c->number,
             (int)nsid);
        return;
    }
    nsze = nvme_le64(c->identify);
    if (nsze == 0) {
        return;
    }
    shift = nvme_lba_shift(c->identify, &ms);
    if (shift == 0) {
        klog(KLOG_WARN, "nvme", "%d: namespace %d: refused: its format is not one it lists",
             c->number, (int)nsid);
        return;
    }
    if (ms != 0) {
        klog(KLOG_WARN, "nvme", "%d: namespace %d: refused: a format with %d bytes of metadata",
             c->number, (int)nsid, (int)ms);
        return;
    }
    if (shift != 9u) {
        klog(KLOG_WARN, "nvme",
             "%d: namespace %d: refused: blocks of %d bytes; the block layer counts in "
             "512-byte sectors",
             c->number, (int)nsid, shift < 31u ? (int)(1u << shift) : -1);
        return;
    }
    if (c->nns >= NVME_NAMESPACES) {
        klog(KLOG_WARN, "nvme", "%d: namespace %d: a namespace past the %d this driver publishes",
             c->number, (int)nsid, NVME_NAMESPACES);
        return;
    }
    ns = &c->ns[c->nns++];
    ns->ctrl = c;
    ns->nsid = nsid;
    ns->queue_head = ns->queue_tail = 0;
    ns->dev.sector_size = BLOCK_SECTOR_SIZE;
    ns->dev.sector_count = nsze;
    ns->dev.max_sectors = c->max_sectors;
    ns->dev.flags = BLOCK_FAULT_SAFE | BLOCK_POLLABLE | BLOCK_WRITE_THROUGH;
    ns->dev.unflushed = 0;
    ns->dev.read = nvme_read;
    ns->dev.write = nvme_write;
    ns->dev.submit = 0;
    ns->dev.flush = 0;
    ns->dev.backing = 0;
    ns->dev.drvdata = ns;
    klog(KLOG_INFO, "nvme", "%d: namespace %d: %d MiB", c->number, (int)nsid,
         (int)(uint32_t)(nsze >> 11));
}

/**
 * Take the controller's namespaces: those in its active list, or, from a
 * controller that has no such list, every identifier up to its count, the
 * inactive ones answering a size of zero. They are taken sixteen at a time,
 * the list asked again for the identifiers past the last of them, since
 * each Identify after the list writes the same page, until the list or the
 * count ends, NVME_EXAMINE_MAX have been examined, or the controller is
 * out of use.
 */
static void nvme_scan_namespaces(struct nvme_ctrl *c)
{
    uint32_t ids[NVME_LIST_MAX];
    uint32_t n, i, examined = 0, after = 0;
    int list = 1;

    c->nns = 0;
    while (examined < NVME_EXAMINE_MAX && !c->dead) {
        n = 0;
        if (list && nvme_identify(c, NVME_CNS_ACTIVE, after) == 0) {
            while (n < NVME_LIST_MAX && (ids[n] = nvme_le32(c->identify + 4u * n)) != 0) {
                n++;
            }
        } else if (!c->dead) {
            list = 0;
            while (n < NVME_LIST_MAX && after + n < c->nn) {
                ids[n] = after + n + 1u;
                n++;
            }
        }
        if (n == 0) {
            break;
        }
        for (i = 0; i < n && !c->dead; i++) {
            nvme_take_namespace(c, ids[i]);
        }
        examined += n;
        after = ids[n - 1];
    }
    if (c->nns == 0) {
        klog(KLOG_WARN, "nvme", "%d: no namespace this driver can use", c->number);
    }
}

/* The controller's strings laid out as ATA IDENTIFY words, first character
   in the high byte, which is what the disk registry reads a model, a serial
   and a firmware revision from. */
static uint16_t nvme_ata_id[256];

static void nvme_ata_field(const uint8_t *from, uint32_t chars, uint32_t first_word)
{
    uint32_t i;

    for (i = 0; i < chars; i += 2u) {
        nvme_ata_id[first_word + i / 2u] = (uint16_t)(((uint32_t)from[i] << 8) | from[i + 1u]);
    }
}

/**
 * Register `c`'s namespaces with the disk registry, the first as `block0`
 * when no other driver has published one. The location is the controller's
 * number times four and the namespace's place among those it published.
 */
static int nvme_publish(struct nvme_ctrl *c)
{
    int i;

    for (i = 0; i < 256; i++) {
        nvme_ata_id[i] = 0;
    }
    nvme_ata_field(c->strings + 4, 20, 10);
    nvme_ata_field(c->strings + 64, 8, 23);
    nvme_ata_field(c->strings + 24, 40, 27);
    for (i = 0; i < c->nns; i++) {
        if (!block0) {
            block0 = &c->ns[i].dev;
        }
        disk_register(&c->ns[i].dev, "nvme", c->number * NVME_NAMESPACES + i, nvme_ata_id);
    }
    return c->nns;
}

/**
 * Record the NVMe controllers on the bus, up to `NVME_CONTROLLERS`, in the
 * order the bus lists them. A controller an earlier call recorded keeps its
 * record, and so its state and its pages.
 */
static void nvme_discover(void)
{
    pci_device_t dev, skip;
    pci_device_t *after = 0;
    int i;

    while (pci_find_device_skip(PCI_CLASS_MASS_STORAGE, NVME_PCI_SUBCLASS, after, &dev)) {
        skip = dev;
        after = &skip;
        if (dev.prog_if != NVME_PCI_PROGIF) {
            continue;
        }
        for (i = 0; i < nvme_nctrls; i++) {
            if (nvme_ctrls[i].pci.bus == dev.bus && nvme_ctrls[i].pci.device == dev.device &&
                nvme_ctrls[i].pci.function == dev.function) {
                break;
            }
        }
        if (i < nvme_nctrls) {
            continue;
        }
        if (nvme_nctrls >= NVME_CONTROLLERS) {
            klog(KLOG_WARN, "nvme",
                 "%X:%X at %d:%d.%d: a controller past the %d this driver drives",
                 (unsigned)dev.vendor_id, (unsigned)dev.device_id, (int)dev.bus, (int)dev.device,
                 (int)dev.function, NVME_CONTROLLERS);
            continue;
        }
        nvme_ctrls[nvme_nctrls].number = nvme_nctrls;
        nvme_ctrls[nvme_nctrls].pci = dev;
        nvme_ctrls[nvme_nctrls].vector = -1;
        klog(KLOG_DEBUG, "nvme", "%d: found: %X:%X at %d:%d.%d", nvme_nctrls,
             (unsigned)dev.vendor_id, (unsigned)dev.device_id, (int)dev.bus, (int)dev.device,
             (int)dev.function);
        nvme_nctrls++;
    }
}

int nvme_init(void)
{
    int i, published = 0;

    nvme_discover();
    if (nvme_nctrls == 0) {
        klog(KLOG_INFO, "nvme", "none: no NVMe controller on the bus");
        return -1;
    }
    for (i = 0; i < nvme_nctrls; i++) {
        struct nvme_ctrl *c = &nvme_ctrls[i];

        if (!c->up) {
            if (nvme_map(c) != 0 || nvme_alloc(c) != 0 || nvme_bring_up(c) != 0) {
                klog(KLOG_ERR, "nvme", "%d: unusable: %s", c->number, nvme_why);
                continue;
            }
            nvme_scan_namespaces(c);
            c->up = 1;
        }
        published += nvme_publish(c);
    }
    return published > 0 ? 0 : -1;
}

/*
 * Commands on the I/O queue, polled and interrupt-driven alike.
 */

/**
 * One transfer as the driver issues it: a read, a write or a flush. `buf`
 * is the memory the controller is given; `out` is the same memory for a
 * read, which the bounce path copies into, and NULL for a write.
 */
struct nvme_xfer {
    struct nvme_ns *ns;
    uint8_t opcode;
    uint64_t lba;
    uint32_t count;
    const uint8_t *buf;
    uint8_t *out;
    uint32_t control; /* NVME_RW_FUA or 0 */
};

/** The ATA command the disk policy judges an NVMe command by: a read by a
 * read, a write by a write and a flush by a flush. */
static uint8_t nvme_policy_command(uint8_t opcode)
{
    switch (opcode) {
    case NVME_CMD_READ:
        return DISKCMD_READ_DMA_EXT;
    case NVME_CMD_WRITE:
        return DISKCMD_WRITE_DMA_EXT;
    case NVME_CMD_FLUSH:
        return DISKCMD_FLUSH_CACHE_EXT;
    default:
        return 0;
    }
}

/**
 * Describe `bytes` at `buf` in `cmd`'s two PRP entries: the first with its
 * offset into its page, and the second the next page or, past two pages,
 * `s`'s list of the rest, a page each. Returns 0, or -1 when a page has no
 * physical address or the buffer is not aligned to four bytes, which the
 * first entry requires.
 */
static int nvme_prps(struct nvme_slot *s, struct nvme_sqe *cmd, const uint8_t *buf, uint32_t bytes)
{
    uint32_t first = PAGE_SIZE - (uint32_t)((uintptr_t)buf & (PAGE_SIZE - 1u));
    uint32_t n = 0;

    cmd->prp1 = nvme_phys(buf);
    cmd->prp2 = 0;
    if (((uintptr_t)buf & 3u) != 0 || cmd->prp1 == 0) {
        return -1;
    }
    if (bytes <= first) {
        return 0;
    }
    buf += first;
    bytes -= first;
    if (bytes <= PAGE_SIZE) {
        cmd->prp2 = nvme_phys(buf);
        return cmd->prp2 ? 0 : -1;
    }
    while (bytes > 0) {
        uint64_t phys = nvme_phys(buf);

        if (phys == 0 || n >= NVME_PRP_ENTRIES) {
            return -1;
        }
        s->prp_list[n++] = phys;
        buf += bytes > PAGE_SIZE ? PAGE_SIZE : bytes;
        bytes -= bytes > PAGE_SIZE ? PAGE_SIZE : bytes;
    }
    cmd->prp2 = s->prp_phys;
    return 0;
}

/** Build `xf` as the command in slot `s`. Returns 0, or -1 when the disk
 * policy refuses it or its buffer cannot be described. */
static int nvme_build(const struct nvme_ctrl *c, struct nvme_slot *s, const struct nvme_xfer *xf,
                      struct nvme_sqe *cmd)
{
    if (!diskcmd_allowed(nvme_policy_command(xf->opcode))) {
        klog(KLOG_WARN, "nvme", "%d: refused: command 0x%X: %s", c->number, (unsigned)xf->opcode,
             diskcmd_policy_banner);
        return -1;
    }
    nvme_cmd_clear(cmd);
    cmd->cdw0 = (uint32_t)xf->opcode | ((uint32_t)s->cid << 16);
    cmd->nsid = xf->ns->nsid;
    if (xf->opcode != NVME_CMD_FLUSH) {
        if (nvme_prps(s, cmd, xf->buf, xf->count * BLOCK_SECTOR_SIZE) != 0) {
            klog(KLOG_WARN, "nvme", "%d: refused: a buffer the PRP list cannot describe",
                 c->number);
            return -1;
        }
        cmd->cdw10 = (uint32_t)xf->lba;
        cmd->cdw11 = (uint32_t)(xf->lba >> 32);
        cmd->cdw12 = (xf->count - 1u) | xf->control;
    }
    s->opcode = xf->opcode;
    s->ns = xf->ns;
    return 0;
}

/** A free slot of `c`, marked busy under a fresh command identifier, or
 * NULL. Called with interrupts masked. */
static struct nvme_slot *nvme_slot_take(struct nvme_ctrl *c)
{
    uint32_t i;

    for (i = 0; i < c->nslots; i++) {
        struct nvme_slot *s = &c->slots[i];

        if (s->busy) {
            continue;
        }
        c->seq = (uint16_t)(c->seq + 1u);
        s->cid = (uint16_t)(((uint32_t)c->seq << 3) | i);
        if (s->cid == 0xFFFFu) {
            c->seq = 0;
            s->cid = (uint16_t)i;
        }
        s->busy = 1;
        s->done = 0;
        s->status = 0;
        s->request = 0;
        return s;
    }
    return 0;
}

/** A write of `ns` was answered without a flush: the disk is marked, and
 * the count of such writes moves on. */
static void nvme_unflushed(struct nvme_ns *ns)
{
    ns->wseq++;
    ns->dev.unflushed = 1;
}

/**
 * A flush of `ns` issued when its count of unflushed writes was `fseq` has
 * finished well. It covers every write answered before it was issued
 * (NVMe 1.4, 6.8), and so clears the mark unless a write was answered
 * unflushed while it ran. The mark is cleared here and not at the issue,
 * so that a second caller's flush finds it set until this one has finished
 * and waits on a flush of its own instead of reporting what is not yet on
 * the medium.
 */
static void nvme_flushed(struct nvme_ns *ns, uint32_t fseq)
{
    if (ns->wseq == fseq) {
        ns->dev.unflushed = 0;
    }
}

/**
 * Issue a flush of `ns` as the second half of the write in slot `s`, which
 * the controller has answered: FUA made that write's own blocks durable and
 * no others (NVMe 1.4, Figure 406), and a write the request wants durable
 * must leave every write answered before it on the medium too, as the
 * buffer cache's write after an unflushed one expects. Returns 0, or -1
 * when the flush cannot be built. Called with interrupts masked.
 */
static int nvme_flush_half(struct nvme_ctrl *c, struct nvme_slot *s, struct nvme_ns *ns)
{
    struct nvme_xfer xf;
    struct nvme_sqe cmd;

    xf.ns = ns;
    xf.opcode = NVME_CMD_FLUSH;
    xf.lba = 0;
    xf.count = 0;
    xf.buf = 0;
    xf.out = 0;
    xf.control = 0;
    if (nvme_build(c, s, &xf, &cmd) != 0) {
        return -1;
    }
    s->flushing = 1;
    s->fseq = ns->wseq;
    block_count_flush();
    nvme_push(c, &c->io, &cmd);
    return 0;
}

/**
 * The controller has answered the command `e` identifies. A request is
 * completed and its slot freed; a polled command is marked done for its
 * caller, who frees the slot. A write the request let go unflushed marks
 * the disk; a write it wants durable, answered while the disk is marked,
 * has a flush issued in its slot before it is completed; a flush that
 * finishes well clears the mark (`nvme_flushed`). A completion whose
 * identifier matches no command in flight is dropped with a record.
 */
static void nvme_complete(struct nvme_ctrl *c, const struct nvme_cqe *e)
{
    struct nvme_slot *s = &c->slots[e->cid & NVME_SLOT_MASK];
    struct block_request *r = s->request;
    uint16_t status = nvme_status(e->status);
    int rc = status ? -EIO : 0;

    if (!s->busy || s->cid != e->cid) {
        klog(KLOG_WARN, "nvme", "%d: a completion for no command: cid 0x%X", c->number,
             (unsigned)e->cid);
        return;
    }
    if (status != 0) {
        klog(KLOG_WARN, "nvme", "%d: namespace %d: command 0x%X failed: status 0x%X", c->number,
             (int)s->ns->nsid, (unsigned)s->opcode, (unsigned)status);
    }
    if (!r) {
        s->status = status;
        s->done = 1;
        return;
    }
    if (rc == 0 && s->opcode == NVME_CMD_WRITE && (r->flags & BLOCK_REQ_NOFLUSH)) {
        nvme_unflushed(s->ns);
    } else if (rc == 0 && s->opcode == NVME_CMD_WRITE && s->ns->dev.unflushed) {
        if (nvme_flush_half(c, s, s->ns) == 0) {
            return;
        }
        rc = -EIO;
    } else if (rc == 0 && s->opcode == NVME_CMD_FLUSH) {
        nvme_flushed(s->ns, s->fseq);
    }
    s->flushing = 0;
    s->request = 0;
    s->busy = 0;
    latency_span(LAT_DISK, r->taken_us, timer_now_us());
    block_request_complete(r, s->generation, rc);
}

/** Take every completion the I/O queue has. Called with interrupts
 * masked. */
static void nvme_reap(struct nvme_ctrl *c)
{
    struct nvme_cqe e;

    while (nvme_pop(c, &c->io, &e)) {
        nvme_complete(c, &e);
    }
}

/** The next namespace of `c` with a request waiting, taken in turn, or
 * NULL. */
static struct nvme_ns *nvme_next_waiting(struct nvme_ctrl *c)
{
    int i;

    for (i = 0; i < c->nns; i++) {
        struct nvme_ns *ns = &c->ns[(c->next_ns + i) % c->nns];

        if (ns->queue_head) {
            c->next_ns = (c->next_ns + i + 1) % c->nns;
            return ns;
        }
    }
    return 0;
}

/**
 * Issue request `r` of `ns` in slot `s`: a read, a write that is on the
 * medium when it is answered unless the request lets it stay in the disk's
 * cache, or a flush, which notes the count of unflushed writes it covers
 * (`nvme_flushed`).
 */
static int nvme_issue(struct nvme_ctrl *c, struct nvme_slot *s, struct nvme_ns *ns,
                      struct block_request *r)
{
    struct nvme_xfer xf;
    struct nvme_sqe cmd;

    xf.ns = ns;
    xf.lba = r->lba;
    xf.count = r->count;
    xf.buf = r->page;
    xf.out = r->page;
    xf.control = 0;
    if (r->flags & BLOCK_REQ_FLUSH) {
        xf.opcode = NVME_CMD_FLUSH;
    } else if (r->write) {
        xf.opcode = NVME_CMD_WRITE;
        xf.control = (r->flags & BLOCK_REQ_NOFLUSH) ? 0 : NVME_RW_FUA;
    } else {
        xf.opcode = NVME_CMD_READ;
    }
    if (nvme_build(c, s, &xf, &cmd) != 0) {
        return -1;
    }
    s->flushing = 0;
    if (xf.opcode == NVME_CMD_FLUSH) {
        s->fseq = ns->wseq;
        block_count_flush();
    }
    s->request = r;
    s->generation = r->generation;
    nvme_push(c, &c->io, &cmd);
    return 0;
}

/**
 * Issue waiting requests while slots are free, the namespaces taking turns.
 * A controller being reset is left alone, and a dead one answers each
 * waiting request with EIO.
 */
static void nvme_start_next(struct nvme_ctrl *c)
{
    uint32_t flags = kirq_save();

    while (!c->resetting) {
        struct nvme_ns *ns = nvme_next_waiting(c);
        struct nvme_slot *s = 0;
        struct block_request *r;

        if (!ns || (!c->dead && (s = nvme_slot_take(c)) == 0)) {
            break;
        }
        r = ns->queue_head;
        ns->queue_head = r->queued;
        if (!ns->queue_head) {
            ns->queue_tail = 0;
        }
        r->queued = 0;
        if (c->dead || nvme_issue(c, s, ns, r) != 0) {
            if (s) {
                s->busy = 0;
            }
            block_request_complete(r, r->generation, -EIO);
        }
    }
    kirq_restore(flags);
}

/**
 * Take a slot from its owner when recovery has reset the controller: a
 * request is answered with EIO, a polled command is marked done with
 * `NVME_STATUS_RESET`, and the slot is free.
 */
static void nvme_slot_fail(struct nvme_slot *s)
{
    struct block_request *r = s->request;

    if (!s->busy) {
        return;
    }
    s->busy = 0;
    s->request = 0;
    if (r) {
        block_request_complete(r, s->generation, -EIO);
        return;
    }
    s->status = NVME_STATUS_RESET;
    s->done = 1;
}

/**
 * Reset `c` after a command it did not answer, and create its queues
 * again.
 *
 * Disabling the controller is the one way to be sure it writes into no
 * buffer it was given again: the reset aborts every command in flight, and
 * none of them completes after. A controller that does not stop may still
 * write into those buffers once their owners have them back, so that is a
 * panic, with interrupts masked, as an AHCI engine that will not stop is.
 * One that stops and does not come back is out of use, and every later
 * command answers EIO.
 */
static void nvme_recover(struct nvme_ctrl *c, const char *why)
{
    uint32_t flags = kirq_save();
    uint32_t i;

    klog(KLOG_WARN, "nvme", "%d: recovery: %s; resetting the controller", c->number, why);
    c->resetting = 1;
    if (nvme_disable(c) != 0) {
        k_panic("nvme: recovery: the controller did not stop; DMA buffers still owned");
    }
    for (i = 0; i < NVME_SLOTS; i++) {
        nvme_slot_fail(&c->slots[i]);
    }
    if (nvme_enable(c) != 0 || nvme_create_io_queues(c) != 0) {
        c->dead = 1;
        (void)nvme_disable(c);
        klog(KLOG_ERR, "nvme", "%d: recovery: failed: %s; the controller is out of use", c->number,
             nvme_why);
    } else {
        klog(KLOG_WARN, "nvme", "%d: recovery: the controller answers again", c->number);
    }
    c->resetting = 0;
    kirq_restore(flags);
    nvme_start_next(c);
}

/**
 * A slot for a polled command, reaping completions while every slot is
 * busy. Slots that stay busy through the whole wait are a controller that
 * stopped answering, which is recovered. NULL when the controller is out of
 * use or being reset.
 */
static struct nvme_slot *nvme_slot_wait(struct nvme_ctrl *c)
{
    uint32_t steps = nvme_cmd_steps();
    struct nvme_slot *s = 0;

    while (!c->dead && !c->resetting) {
        uint32_t flags = kirq_save();

        nvme_reap(c);
        s = nvme_slot_take(c);
        kirq_restore(flags);
        if (s) {
            break;
        }
        if (--steps == 0) {
            nvme_recover(c, "no command slot came free");
            steps = nvme_cmd_steps();
            continue;
        }
        nvme_io->udelay(NVME_POLL_US);
    }
    return s;
}

/**
 * Issue `xf` and poll for its answer: 0 when the controller answered
 * success, `-EIO` when it answered an error or did not answer within the
 * wait, after recovery, and -1 when the command could not be built.
 *
 * The completion may be taken by this loop or by the interrupt handler,
 * which marks the slot done either way. The wait neither sleeps nor
 * allocates, so it serves a page fault with interrupts off.
 */
static int nvme_polled(struct nvme_ctrl *c, const struct nvme_xfer *xf)
{
    struct nvme_slot *s = nvme_slot_wait(c);
    struct nvme_sqe cmd;
    int masked = !kirq_enabled();
    uint32_t steps = nvme_cmd_steps();
    uint32_t flags;

    if (!s) {
        return -EIO;
    }
    flags = kirq_save();
    if (nvme_build(c, s, xf, &cmd) != 0) {
        s->busy = 0;
        kirq_restore(flags);
        nvme_start_next(c);
        return -1;
    }
    nvme_push(c, &c->io, &cmd);
    kirq_restore(flags);
    while (steps-- > 0) {
        flags = kirq_save();
        nvme_reap(c);
        block_count_poll();
        if (s->done) {
            uint16_t status = s->status;

            s->busy = 0;
            kirq_restore(flags);
            nvme_start_next(c);
            return status ? -EIO : 0;
        }
        kirq_restore(flags);
        nvme_io->udelay(NVME_POLL_US);
    }
    klog(KLOG_WARN, "nvme", "%d: namespace %d: command 0x%X failed: %s", c->number,
         (int)xf->ns->nsid, (unsigned)xf->opcode,
         masked ? "no answer in five seconds with interrupts off" : "no answer in thirty seconds");
    nvme_recover(c, "a command was not answered");
    return -EIO;
}

/**
 * A polled transfer of at most a page through the controller's bounce
 * page, for a buffer the PRP entries cannot describe because it is not
 * aligned to four bytes. Interrupts stay masked throughout, since the
 * bounce page is one per controller.
 */
static int nvme_bounced(struct nvme_ctrl *c, const struct nvme_xfer *xf)
{
    struct nvme_xfer b = *xf;
    uint32_t bytes = xf->count * BLOCK_SECTOR_SIZE, i;
    uint32_t flags = kirq_save();
    int rc;

    b.buf = c->bounce;
    b.out = c->bounce;
    if (xf->opcode == NVME_CMD_WRITE) {
        for (i = 0; i < bytes; i++) {
            c->bounce[i] = xf->buf[i];
        }
    }
    rc = nvme_polled(c, &b);
    if (rc == 0 && xf->opcode == NVME_CMD_READ) {
        for (i = 0; i < bytes; i++) {
            xf->out[i] = c->bounce[i];
        }
    }
    kirq_restore(flags);
    return rc;
}

/**
 * Carry out `xf` by polling, in commands of at most the controller's limit,
 * or of a page through the bounce page for a buffer that is not aligned to
 * four bytes. `xf` is advanced past what each command moved.
 */
static int nvme_polled_rw(struct nvme_xfer *xf)
{
    struct nvme_ctrl *c = xf->ns->ctrl;
    uint32_t count = xf->count;
    int odd = ((uintptr_t)xf->buf & 3u) != 0;
    uint32_t most = odd ? PAGE_SIZE / BLOCK_SECTOR_SIZE : c->max_sectors;

    while (count > 0) {
        uint32_t bytes;
        int rc;

        xf->count = count > most ? most : count;
        rc = odd ? nvme_bounced(c, xf) : nvme_polled(c, xf);
        if (rc != 0) {
            return rc;
        }
        bytes = xf->count * BLOCK_SECTOR_SIZE;
        xf->lba += xf->count;
        xf->buf += bytes;
        if (xf->out) {
            xf->out += bytes;
        }
        count -= xf->count;
    }
    return 0;
}

static int nvme_flush_polled(struct nvme_ns *ns);

/** A polled transfer of `count` sectors at `lba` of the device's namespace,
 * from `src` for a write or into `dst` for a read. A write is on the medium
 * when it is answered: it is sent with FUA, and followed by a flush while
 * the disk has writes answered unflushed, which FUA does not cover. */
static int nvme_rw(block_device_t *device, uint64_t lba, uint32_t count, const void *src, void *dst)
{
    struct nvme_xfer xf;
    int rc;

    if (count == 0 || lba > device->sector_count || count > device->sector_count - lba) {
        return -EIO;
    }
    xf.ns = (struct nvme_ns *)device->drvdata;
    xf.opcode = dst ? NVME_CMD_READ : NVME_CMD_WRITE;
    xf.lba = lba;
    xf.count = count;
    xf.buf = dst ? (const uint8_t *)dst : (const uint8_t *)src;
    xf.out = (uint8_t *)dst;
    xf.control = dst ? 0 : NVME_RW_FUA;
    rc = nvme_polled_rw(&xf);
    if (rc == 0 && !dst && device->unflushed) {
        rc = nvme_flush_polled(xf.ns);
    }
    return rc;
}

/** The device's read, polled. */
static int nvme_read(block_device_t *device, uint64_t lba, uint32_t count, void *buffer)
{
    return nvme_rw(device, lba, count, 0, buffer);
}

/** The device's write, polled and on the medium when it returns. */
static int nvme_write(block_device_t *device, uint64_t lba, uint32_t count, const void *buffer)
{
    return nvme_rw(device, lba, count, buffer, 0);
}

/** Flush the disk's cache by polling, clearing the unflushed mark as a
 * queued flush clears it (`nvme_flushed`). */
static int nvme_flush_polled(struct nvme_ns *ns)
{
    struct nvme_xfer xf;
    uint32_t fseq = ns->wseq;
    int rc;

    xf.ns = ns;
    xf.opcode = NVME_CMD_FLUSH;
    xf.lba = 0;
    xf.count = 0;
    xf.buf = 0;
    xf.out = 0;
    xf.control = 0;
    block_count_flush();
    rc = nvme_polled(ns->ctrl, &xf);
    if (rc == 0) {
        nvme_flushed(ns, fseq);
    }
    return rc;
}

/** The device's `submit`: queue `r` on its namespace and issue what slots
 * allow. */
static int nvme_submit(block_device_t *device, struct block_request *r)
{
    struct nvme_ns *ns = (struct nvme_ns *)device->drvdata;
    uint32_t flags;

    if (r->count == 0 || r->lba > device->sector_count ||
        r->count > device->sector_count - r->lba) {
        return -EIO;
    }
    flags = kirq_save();
    r->queued = 0;
    if (ns->queue_tail) {
        ns->queue_tail->queued = r;
    } else {
        ns->queue_head = r;
    }
    ns->queue_tail = r;
    kirq_restore(flags);
    nvme_start_next(ns->ctrl);
    return 0;
}

/** The device's `flush`: `r` queued as a flush request, or, with `r` NULL,
 * a flush by polling for a caller that cannot sleep. */
static int nvme_flush_submit(block_device_t *device, struct block_request *r)
{
    if (!r) {
        return nvme_flush_polled((struct nvme_ns *)device->drvdata);
    }
    r->flags |= BLOCK_REQ_FLUSH;
    return nvme_submit(device, r);
}

/**
 * The controllers' message: every controller on its messages has its
 * completions taken and its waiting requests issued. The vectors of all the
 * controllers share this handler, so it looks at each.
 */
static void nvme_handler(isr_params_t *params)
{
    int i;

    (void)params;
    nvme_messages++;
    for (i = 0; i < nvme_nctrls; i++) {
        struct nvme_ctrl *c = &nvme_ctrls[i];

        if (c->vector < 0 || c->resetting) {
            continue;
        }
        nvme_reap(c);
        nvme_start_next(c);
    }
}

/**
 * Point entry 0 of `c`'s MSI-X table at `vector` and enable MSI-X. The
 * table is in one of the controller's BARs, at an offset the capability
 * gives, and is mapped here.
 */
static int nvme_msix(struct nvme_ctrl *c, uint8_t vector)
{
    uint8_t bir;
    uint32_t offset, entries;
    uint64_t phys;
    uintptr_t table;

    if (pci_msix_table(&c->pci, &bir, &offset, &entries) != 0 || entries == 0) {
        return -1;
    }
    phys = pci_bar_address64(&c->pci, (int)bir);
    if (phys == 0 || (uint64_t)(uintptr_t)(phys + offset) != phys + offset) {
        return -1;
    }
    table = map_mmio((uintptr_t)(phys + offset), 16);
    if (!table) {
        return -1;
    }
    return pci_msix_enable(&c->pci, table, vector);
}

/** Move `c`'s namespaces to its messages, or log why it keeps polling. */
static void nvme_start_one(struct nvme_ctrl *c)
{
    const char *kind = "msi-x";
    int vector = -1, i;

    if (kcmdline_has("rach2.nvmepoll")) {
        klog(KLOG_INFO, "nvme", "polled: rach2.nvmepoll");
        return;
    }
    if (!isr_msi_ready() || (vector = isr_alloc_vector()) < 0) {
        klog(KLOG_INFO, "nvme", "polled: no vector for the controller's messages");
        return;
    }
    register_isr_handler((uint8_t)vector, nvme_handler, "nvme");
    c->vector = vector;
    if (nvme_msix(c, (uint8_t)vector) != 0) {
        kind = "msi";
        if (pci_msi_enable(&c->pci, (uint8_t)vector) != 0) {
            c->vector = -1;
            klog(KLOG_INFO, "nvme", "polled: the controller has no message capability");
            return;
        }
    }
    for (i = 0; i < c->nns; i++) {
        c->ns[i].dev.flags &= ~BLOCK_FAULT_SAFE;
        c->ns[i].dev.submit = nvme_submit;
        c->ns[i].dev.flush = nvme_flush_submit;
    }
    klog(KLOG_INFO, "nvme", "interrupts: messages on vector %d, %s, %d disks queued", vector, kind,
         c->nns);
}

void nvme_start(void)
{
    int i;

    for (i = 0; i < nvme_nctrls; i++) {
        struct nvme_ctrl *c = &nvme_ctrls[i];

        if (c->up && !c->dead && c->nns > 0 && c->vector < 0) {
            nvme_start_one(c);
        }
    }
}
