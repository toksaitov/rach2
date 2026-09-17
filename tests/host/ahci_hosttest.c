/*
 * ahci_hosttest.c: the AHCI driver against a model of a controller.
 *
 * host-deps: rach2/sys/lib/kstring.c rach2/sys/lib/kctypes.c
 * host-deps: tests/host/compat/paging_host.c
 *
 * The disk suites boot this driver against QEMU's ich9-ahci, which works: it
 * resets when told, answers the handoff, brings its link up and completes
 * every command. What none of them can produce is a controller that does not.
 * A reset that never completes, firmware that never answers the handoff, a
 * port whose link never comes up, a command that never completes, a device
 * that reports a fatal task-file status: each is a line of this driver that no
 * boot in this project has ever run.
 *
 * So the driver is compiled natively and given a model of a controller through
 * the interface it now reaches registers by. The model answers reads from a
 * register array and moves the state on when the driver writes, which is what
 * lets a case say "this reset never completes" and have the driver meet it.
 *
 * Every row below names what the driver must do, and the check is on what it
 * returned and what it left behind, not on what it printed.
 */

extern int printf(const char *fmt, ...);
extern int strcmp(const char *a, const char *b);
extern int fork(void);
extern int waitpid(int pid, int *status, int options);
extern void _exit(int status);

#include "../../rach2/sys/block/ahci.h"
#include "../../rach2/sys/block/block.h"
#include "../../rach2/sys/latency.h"

/* ---- the model ---- */

#define REGS 0x400

static uint32_t reg[REGS];
/* What this run's controller does, set before each case. */
static int reset_completes;
static int handoff_answers;
static int link_comes_up;
static int command_completes;
static int fatal_status;
static int engine_stuck;
static int immediate_interrupt;
static unsigned stop_reads;
static unsigned issued;
static unsigned stale_issues;
static unsigned completed;
static uint32_t interrupts_masked;
/* What the model says it moved, which the driver compares with what it
   asked for. */
static uint32_t moved_bytes;
/* Set after `ahci.c` is included, because what it does reaches into the
   driver's own command list, which is defined there. */
static void (*on_command_issued)(void);

#define R_CAP  0x00
#define R_GHC  0x04
#define R_IS   0x08
#define R_PI   0x0C
#define R_VS   0x10
#define R_CAP2 0x24
#define R_BOHC 0x28
#define P0     0x100
#define P_CLB  0x00
#define P_FB   0x08
#define P_IS   0x10
#define P_CMD  0x18
#define P_TFD  0x20
#define P_SIG  0x24
#define P_SSTS 0x28
#define P_SCTL 0x2C
#define P_SERR 0x30
#define P_CI   0x38

#define GHC_AE   0x80000000u
#define GHC_HR   0x00000001u
#define CMD_ST   0x0001u
#define CMD_FRE  0x0010u
#define CMD_FR   0x4000u
#define CMD_CR   0x8000u
#define BOHC_BOS 0x0001u
#define BOHC_OOS 0x0002u

/* How many times the driver read the command-issue register: a wait that
   polls and gives up reads it many times and returns, and a wait that does
   not bound itself never returns at all. */
static unsigned ci_reads;

static uint32_t model_r32(uint32_t off)
{
    if (off >= REGS) {
        return 0;
    }
    if (off == P0 + P_CMD && engine_stuck) {
        stop_reads++;
    }
    if (off == P0 + P_CI) {
        ci_reads++;
    }
    return reg[off];
}

static void model_w32(uint32_t off, uint32_t v)
{
    if (off >= REGS) {
        return;
    }
    if (off == R_GHC) {
        /* The reset bit clears itself when the controller has finished, and a
           controller that never finishes never clears it. A reset also clears
           AHCI enable, which is why the driver sets it again afterwards: a
           model that kept the bit would let a driver that forgot to pass. */
        if (v & GHC_HR) {
            if (reset_completes) {
                v &= ~GHC_HR;
                v &= ~GHC_AE;
            }
        }
        reg[off] = v;
        return;
    }
    if (off == R_BOHC) {
        /* The firmware answers by clearing its own ownership bit. */
        if ((v & BOHC_OOS) && handoff_answers) {
            v &= ~BOHC_BOS;
        }
        reg[off] = v;
        return;
    }
    if (off == P0 + P_SCTL) {
        /* COMRESET: while DET is 1 the link is down, and releasing it brings
           the link up on a port that has a device. A model that answered DET
           3 whatever the driver did would pass a driver that never reset the
           link at all. */
        reg[off] = v;
        if ((v & 0xFu) == 1u) {
            reg[P0 + P_SSTS] = 0x000u;
        } else if ((v & 0xFu) == 0u && link_comes_up) {
            reg[P0 + P_SSTS] = 0x123u;
        }
        return;
    }
    if (off == P0 + P_CMD) {
        /* The engines follow the bits that start and stop them. */
        if (v & CMD_ST) {
            v |= CMD_CR;
        } else if (!engine_stuck) {
            v &= ~CMD_CR;
            reg[P0 + P_CI] = 0;
        }
        if (v & CMD_FRE) {
            v |= CMD_FR;
        } else {
            v &= ~CMD_FR;
        }
        reg[off] = v;
        return;
    }
    if (off == R_IS || off == P0 + P_IS || off == P0 + P_SERR) {
        /* Write one to clear, which is how a controller's status registers
           are cleared and the opposite of storing what was written. */
        reg[off] &= ~v;
        return;
    }
    if (off == P0 + P_IS || off == P0 + P_SERR) {
        /* Write one to clear, which is how a controller's status registers
           work and the opposite of storing what was written. */
        reg[off] &= ~v;
        return;
    }
    if (off == P0 + P_CI) {
        /* A command the controller completes clears its bit and reports the
           bytes it moved; one it does not leaves the bit set until the
           driver's wait gives up. The byte count is the controller's word for
           what it did, and the driver checks it, so the model must give one. */
        if (reg[R_IS] & 1u) {
            stale_issues++;
        }
        issued++;
        reg[off] = command_completes ? 0 : v;
        if (command_completes && on_command_issued) {
            on_command_issued();
            if (immediate_interrupt) {
                reg[P0 + P_IS] |= 1u;
                reg[R_IS] |= 1u;
            }
        }
        if (fatal_status == 2) {
            reg[P0 + P_IS] |= 0x40000000u;
            reg[R_IS] |= 1u;
        }
        if (fatal_status) {
            reg[P0 + P_TFD] = 0x81; /* BSY clear, ERR set */
        }
        return;
    }
    reg[off] = v;
}

static void model_udelay(uint32_t us)
{
    (void)us;
}

static const struct ahci_io model_io = {model_r32, model_w32, model_udelay};

/* ---- what the driver reaches for outside itself ---- */

#include "../../rach2/sys/pci.h"

bool pci_find_device_skip(uint8_t cls, uint8_t sub, pci_device_t *skip, pci_device_t *out)
{
    (void)cls;
    (void)sub;
    if (skip) {
        return 0; /* one controller on this bus */
    }
    out->vendor_id = 0x8086;
    out->device_id = 0x2922;
    out->bus = 0;
    out->device = 31;
    out->function = 2;
    out->prog_if = 0x01;
    out->bar[4] = 0;
    out->bar[5] = 0xFEBF0000u;
    out->irq = 11;
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

uint16_t pci_read_config16(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off)
{
    (void)bus;
    (void)dev;
    (void)fn;
    (void)off;
    return 0;
}

uint8_t pci_read_config8(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off)
{
    (void)bus;
    (void)dev;
    (void)fn;
    (void)off;
    return 0;
}

void pci_write_config16(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off, uint16_t v)
{
    (void)bus;
    (void)dev;
    (void)fn;
    (void)off;
    (void)v;
}

void pci_enable_bus_master(pci_device_t *dev)
{
    (void)dev;
}

void pci_enable_memory_space(pci_device_t *dev)
{
    (void)dev;
}

/* The window the driver would map. Its address is never dereferenced here:
   every register access goes through the model. */
static uint8_t window[REGS];

uintptr_t map_mmio(uintptr_t phys, unsigned int size)
{
    (void)phys;
    (void)size;
    return (uintptr_t)(unsigned long)window;
}

void k_udelay(uint32_t us)
{
    (void)us;
}

static int policy_allows = 1;

int diskcmd_allowed(uint8_t command)
{
    (void)command;
    return policy_allows;
}

void klog(int level, const char *subsystem, const char *format, ...)
{
    (void)level;
    (void)subsystem;
    (void)format;
}

void console_printf(const char *format, ...)
{
    (void)format;
}

block_device_t *block0;

/* The counters /proc/disk reports, which this test does not read. */
void block_count_command(void)
{}

void block_count_flush(void)
{}

void block_count_batch(uint32_t sectors)
{
    (void)sectors;
}

/** Count completions so failed recovery cannot release a DMA buffer. */
void block_request_complete(struct block_request *request, uint32_t generation, int status)
{
    (void)request;
    (void)generation;
    (void)status;
    completed++;
}

int isr_alloc_vector(void)
{
    return -1;
}

int kcmdline_has(const char *token)
{
    (void)token;
    return 0;
}

uint32_t kirq_save(void)
{
    uint32_t old = interrupts_masked;

    interrupts_masked = 1;
    return old;
}

void kirq_restore(uint32_t flags)
{
    interrupts_masked = flags;
}

int lapic_ready(void)
{
    return 0;
}

static int disk_spans;          /* delays noted for the disk */
static uint32_t disk_span_from; /* the last one's start */

void latency_span(int which, uint32_t from, uint32_t to)
{
    if (which == LAT_DISK) {
        disk_spans++;
        disk_span_from = from;
    }
    (void)to;
}

int pci_msi_enable(const pci_device_t *dev, uint8_t vector)
{
    (void)dev;
    (void)vector;
    return -1;
}

uint32_t pit_now_us(void)
{
    return 0;
}

struct disk *disk_register(block_device_t *dev, const char *transport, int location,
                           const volatile uint16_t *identify)
{
    (void)dev;
    (void)transport;
    (void)location;
    (void)identify;
    return 0;
}

/* The identity window is turned off for the host build, as the xHCI test
   does: the kernel's 32-bit addresses are the host's low words, so a buffer
   would otherwise look as if it lay in the window by the accident of its
   address, and the command table's page walk would never run. */
#include "../../rach2/sys/mm/paging.h"
#undef HARD_MEMORY_LIMIT
#define HARD_MEMORY_LIMIT 0u
#include "../../rach2/sys/block/ahci.c"

void register_isr_handler(uint8_t vector, isr_handler_t handler, const char *name)
{
    (void)vector;
    (void)handler;
    (void)name;
}

/* What the controller does when a command is issued and completes: it writes
   back the number of bytes it moved. */
static void complete_command(void)
{
    volatile ahci_cmd_header_t *hdr = (volatile ahci_cmd_header_t *)ahci_disks[0].cmdlist;

    hdr->prdbc = moved_bytes;
}

/* ---- the rows ---- */

static int same_text(const char *a, const char *b)
{
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

static int fails;

#define CHECK(what, cond)                 \
    do {                                  \
        if (!(cond)) {                    \
            printf("  FAIL: %s\n", what); \
            fails++;                      \
        }                                 \
    } while (0)

/* A controller that does everything right, before each row changes one
   thing about it. */
static void model_reset(void)
{
    unsigned i;

    for (i = 0; i < REGS; i++) {
        reg[i] = 0;
    }
    reg[R_CAP] = 0x00000001u | (31u << 8); /* one port, 32 command slots */
    reg[R_PI] = 0x1u;                      /* port 0 implemented */
    reg[R_VS] = 0x10300u;
    reg[P0 + P_SSTS] = 0x123u;     /* DET 3, IPM 1, SPD 2: a live link */
    reg[P0 + P_SIG] = 0x00000101u; /* a disk, not an ATAPI device */
    reg[P0 + P_TFD] = 0;
    ci_reads = 0;
    moved_bytes = 512;
    on_command_issued = complete_command;
    reset_completes = 1;
    handoff_answers = 1;
    link_comes_up = 1;
    command_completes = 1;
    fatal_status = 0;
    engine_stuck = 0;
    immediate_interrupt = 0;
    stop_reads = issued = completed = stale_issues = 0;
    interrupts_masked = 0;
    reg[R_CAP2] = 0u; /* no handoff, unless a row asks for one */
    block0 = 0;
    ahci_set_io(&model_io);
}

static struct block_request active_request;
static struct block_request queued_request;
static int expect_active;
static int expect_panic;

/** A panic must precede every release or reuse of the active DMA state. */
void k_panic(const char *s)
{
    struct ahci_disk *d = &ahci_disks[0];

    if (expect_panic &&
        strcmp(s, "ahci: recovery: command engine did not stop; DMA buffer still owned") == 0 &&
        interrupts_masked && stop_reads > 1 && issued == 0 && completed == 0 &&
        d->active == (expect_active ? &active_request : 0) && d->queue_head == &queued_request &&
        d->queue_tail == &queued_request && d->cmdtable[0] == 0xA5 && (reg[P0 + P_CMD] & CMD_CR) &&
        !(reg[P0 + P_CMD] & CMD_ST)) {
        _exit(42);
    }
    _exit(43);
}

/**
 * Fail engine stop in each completion path. A child lets the test observe a
 * panic without returning into code that could reuse the controller's buffer.
 */
static void stuck_recovery(int path)
{
    int pid = fork();
    int status = 0;

    CHECK("the recovery row forks", pid >= 0);
    if (pid < 0) {
        return;
    }
    if (pid == 0) {
        struct ahci_disk *d = &ahci_disks[0];

        model_reset();
        if (ahci_select_port() != 0 || ahci_port_start(d) != 0) {
            _exit(44);
        }
        expect_panic = 1;
        expect_active = path != 3;
        active_request.count = 1;
        active_request.page = (void *)0x1000;
        d->active = expect_active ? &active_request : 0;
        d->queue_head = d->queue_tail = &queued_request;
        d->cmdtable[0] = 0xA5;
        engine_stuck = 1;
        reg[P0 + P_CI] = 1;
        if (path == 0) {
            reg[P0 + P_IS] = AHCI_PxIS_FATAL;
            ahci_handler(0);
        } else if (path == 1) {
            ahci_drain(d);
        } else if (path == 2) {
            reg[P0 + P_CI] = 0;
            reg[P0 + P_IS] = AHCI_PxIS_FATAL;
            ahci_handler(0);
        } else {
            ahci_polled_failed(d);
        }
        _exit(45);
    }
    CHECK("the recovery child is reaped", waitpid(pid, &status, 0) == pid);
    CHECK("failed engine stop preserves DMA ownership before halting", status == (42 << 8));
}

/** A flush and the next queued read may finish before the handler returns. */
static void quick_completions(void)
{
    static uint8_t buf[512];

    model_reset();
    CHECK("the flush row starts a port",
          ahci_select_port() == 0 && ahci_port_start(&ahci_disks[0]) == 0);
    active_request.count = 1;
    active_request.write = 1;
    active_request.page = buf;
    ahci_disks[0].active = &active_request;
    ahci_disks[0].flushing = 0;
    queued_request.count = 1;
    queued_request.page = buf;
    ahci_disks[0].queue_head = ahci_disks[0].queue_tail = &queued_request;
    ((volatile ahci_cmd_header_t *)ahci_disks[0].cmdlist)->prdbc = 512;
    reg[P0 + P_IS] = 1;
    reg[P0 + P_CI] = 0;
    reg[R_IS] = 3;
    immediate_interrupt = 1;
    ahci_handler(0);
    CHECK("the completed write issued its flush", issued == 1 && ahci_disks[0].flushing);
    CHECK("the quick flush completion is still pending", (reg[P0 + P_IS] & 1) != 0);
    CHECK("the flush keeps its controller interrupt pending", (reg[R_IS] & 1) != 0);
    CHECK("another port keeps its controller interrupt pending", (reg[R_IS] & 2) != 0);
    ahci_handler(0);
    CHECK("the quick flush completes its request", completed == 1);
    CHECK("the next request is active", ahci_disks[0].active == &queued_request);
    CHECK("the next completion keeps both interrupt bits pending",
          (reg[P0 + P_IS] & 1) && (reg[R_IS] & 1));
    queued_request.taken_us = 4200;
    disk_spans = 0;
    ahci_handler(0);
    CHECK("the next request completes", completed == 2 && !ahci_disks[0].active);
    CHECK("and its delay is noted from the time it was taken",
          disk_spans == 1 && disk_span_from == 4200);
    CHECK("the other port remains pending", reg[R_IS] == 2);
}

/** A polled command is acknowledged before its queue resumes, even on error. */
static void polled_queue_restart(int fault)
{
    static uint8_t buf[512];
    int rc;

    model_reset();
    CHECK("the polled queue row starts a port",
          ahci_select_port() == 0 && ahci_port_start(&ahci_disks[0]) == 0);
    queued_request.count = 1;
    queued_request.page = buf;
    ahci_disks[0].queue_head = ahci_disks[0].queue_tail = &queued_request;
    reg[R_IS] = 2;
    immediate_interrupt = 1;
    fatal_status = fault;
    command_completes = fault != 2;
    rc = ahci_run_command(&ahci_disks[0], ATA_IDENTIFY, 0, 0, buf, sizeof(buf), 0);
    CHECK("the polled command reports its result", rc == (fault ? -EIO : 0));
    CHECK("the polled command starts the queued request", ahci_disks[0].active == &queued_request);
    CHECK("the queue resumes after the polled interrupt is acknowledged", stale_issues == 0);
    CHECK("the queued completion and the other port remain pending", reg[R_IS] == 3);
}

int main(void)
{
    /* Each row drives the step it is about, rather than the whole bring-up:
       a row that ran the whole of `ahci_init` would pass whenever anything at
       all refused, which is every row on a model that cannot answer an
       IDENTIFY. What a step returned is what is checked. */

    /* IDENTIFY data under the driver's control: a device whose logical
       sector is not 512 bytes, word 106 with bit 14 valid and bit 12 set and
       words 117 and 118 giving 2048 words a sector, is refused by name, the
       restriction the block Readme states; the same words with bit 12 clear
       are taken, with the sector count read from words 100 to 103. */
    model_reset();
    {
        volatile uint16_t *w = (volatile uint16_t *)ahci_idbuf;
        int i;

        for (i = 0; i < 256; i++) {
            w[i] = 0;
        }
        w[100] = 0x1000u;
        w[106] = 0x5000u;
        w[117] = 0x0800u;
        w[118] = 0;
        CHECK("a 4096-byte logical sector is refused", ahci_identify(&ahci_disks[0]) != 0);
        CHECK("by name", same_text(ahci_err, "the device's logical sector is not 512 bytes"));
        w[106] = 0x4000u;
        CHECK("and the same device with 512-byte sectors is taken",
              ahci_identify(&ahci_disks[0]) == 0 && ahci_disks[0].sectors == 0x1000u);
    }

    /* The controller's own reset. */
    model_reset();
    CHECK("a controller that resets is taken", ahci_hba_own() == 0);
    CHECK("and is left in AHCI mode", (reg[R_GHC] & GHC_AE) != 0);
    CHECK("with interrupts off, because this driver polls", (reg[R_GHC] & 0x2u) == 0);

    model_reset();
    reset_completes = 0;
    CHECK("a reset that never completes is refused", ahci_hba_own() != 0);

    /* The handoff, which a controller may or may not offer. Firmware that
       never answers does not stop the driver: being refused by silence is not
       a reason to stop. */
    model_reset();
    reg[R_CAP2] = 0x1u;
    reg[R_BOHC] = BOHC_BOS;
    handoff_answers = 0;
    CHECK("firmware that never answers the handoff does not stop the driver", ahci_hba_own() == 0);
    CHECK("though the request was made", (reg[R_BOHC] & BOHC_OOS) != 0);

    model_reset();
    reg[R_CAP2] = 0x1u;
    reg[R_BOHC] = BOHC_BOS;
    handoff_answers = 1;
    CHECK("firmware that answers hands the controller over", ahci_hba_own() == 0);
    CHECK("and its ownership bit is clear", (reg[R_BOHC] & BOHC_BOS) == 0);

    /* Choosing a port. */
    model_reset();
    CHECK("a port with a disk on it is chosen", ahci_select_port() == 0);
    CHECK("and it is port 0", ahci_port == 0);

    model_reset();
    reg[P0 + P_SSTS] = 0x000u; /* DET 0: nothing on the link */
    link_comes_up = 0;         /* and a reset does not bring one up */
    CHECK("a port with no device is refused", ahci_select_port() != 0);

    model_reset();
    reg[P0 + P_SSTS] = 0x000u;
    link_comes_up = 1; /* nothing yet, but the link comes up when reset */
    CHECK("a port whose link comes up on reset is taken", ahci_select_port() == 0);
    CHECK("and the reset was released", (reg[P0 + P_SCTL] & 0xFu) != 1u);

    model_reset();
    reg[P0 + P_SSTS] = 0x113u;     /* DET 3 with IPM 1 but no speed: not ready */
    reg[P0 + P_SIG] = 0xEB140101u; /* an ATAPI signature */
    CHECK("a port whose device is not a disk is refused", ahci_select_port() != 0);

    model_reset();
    reg[R_PI] = 0; /* no port implemented at all */
    CHECK("a controller with no implemented port is refused", ahci_select_port() != 0);

    /* Starting the port's engines. */
    model_reset();
    CHECK("a port can be selected", ahci_select_port() == 0);
    CHECK("and started", ahci_port_start(&ahci_disks[0]) == 0);
    CHECK("with the command engine running", (reg[P0 + P_CMD] & CMD_CR) != 0);
    CHECK("and the FIS receiver running", (reg[P0 + P_CMD] & CMD_FR) != 0);

    /* A command, and the two ways it can fail. */
    {
        static uint8_t buf[512];

        model_reset();
        CHECK("the port starts for the command rows",
              ahci_select_port() == 0 && ahci_port_start(&ahci_disks[0]) == 0);
        CHECK("a command that completes is accepted",
              ahci_run_command(&ahci_disks[0], ATA_IDENTIFY, 0, 0, buf, sizeof(buf), 0) == 0);

        /* A request of the block layer's whole limit, from a buffer that
           starts mid-page: the table describes it as 257 entries, a piece
           per page, the first and last half pages, the FIS naming the
           sectors; and a buffer of more pieces than the table has entries
           for is refused. */
        {
            static uint8_t huge[266 * 4096] __attribute__((aligned(4096)));
            volatile ahci_cmd_header_t *hdr = (volatile ahci_cmd_header_t *)ahci_disks[0].cmdlist;
            volatile ahci_prdt_t *prdt =
                (volatile ahci_prdt_t *)(ahci_disks[0].cmdtable + AHCI_PRDT_OFFSET);
            volatile uint8_t *cfis = ahci_disks[0].cmdtable;

            moved_bytes = 2048 * 512;
            CHECK("a 2048-sector read from a mid-page buffer is accepted",
                  ahci_run_command(&ahci_disks[0], ATA_READ_DMA_EXT, 0x12345, 2048, huge + 2048,
                                   2048 * 512, 0) == 0);
            CHECK("described as 257 entries", (hdr->dw0 >> 16) == 257);
            CHECK("the first and the last a half page, the ones between a page",
                  (prdt[0].dbc & AHCI_DBC_MASK) + 1 == 2048 &&
                      (prdt[1].dbc & AHCI_DBC_MASK) + 1 == 4096 &&
                      (prdt[255].dbc & AHCI_DBC_MASK) + 1 == 4096 &&
                      (prdt[256].dbc & AHCI_DBC_MASK) + 1 == 2048);
            CHECK("naming 2048 sectors at the address given",
                  cfis[12] == 0 && cfis[13] == 8 && cfis[4] == 0x45 && cfis[5] == 0x23 &&
                      cfis[6] == 0x01);
            moved_bytes = 512;
            {
                unsigned issued_before = issued;

                /* 265 pages from mid-page: 266 pieces, past the table's 264. */
                CHECK("a buffer of more pieces than the table holds is refused",
                      ahci_run_command(&ahci_disks[0], ATA_READ_DMA_EXT, 0, 2048, huge + 2048,
                                       265 * 4096, 0) != 0);
                CHECK("and no command was issued for it", issued == issued_before);
            }
        }

        model_reset();
        (void)ahci_select_port();
        (void)ahci_port_start(&ahci_disks[0]);
        command_completes = 0;
        ci_reads = 0;
        CHECK("a command that never completes is refused",
              ahci_run_command(&ahci_disks[0], ATA_IDENTIFY, 0, 0, buf, sizeof(buf), 0) != 0);
        CHECK("and the wait polled and then gave up", ci_reads > 1);

        model_reset();
        (void)ahci_select_port();
        (void)ahci_port_start(&ahci_disks[0]);
        fatal_status = 1;
        CHECK("a device that reports a fatal status is refused",
              ahci_run_command(&ahci_disks[0], ATA_IDENTIFY, 0, 0, buf, sizeof(buf), 0) != 0);

        model_reset();
        (void)ahci_select_port();
        (void)ahci_port_start(&ahci_disks[0]);
        policy_allows = 0;
        ci_reads = 0;
        CHECK("a command the disk policy refuses is refused",
              ahci_run_command(&ahci_disks[0], ATA_IDENTIFY, 0, 0, buf, sizeof(buf), 0) != 0);
        CHECK("and never reaches the port", ci_reads == 0);
        policy_allows = 1;
    }

    stuck_recovery(0);
    stuck_recovery(1);
    stuck_recovery(2);
    stuck_recovery(3);

    quick_completions();
    polled_queue_restart(0);
    polled_queue_restart(1);
    polled_queue_restart(2);

    if (fails == 0) {
        printf("ahci_hosttest: PASS\n");
    }
    return fails;
}
