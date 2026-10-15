/*
 * ahci_hosttest.c: the AHCI driver of rach2/sys/block/ahci.c against a model
 * of a controller, for the failures QEMU's ich9-ahci does not produce.
 *
 * host-deps: rach2/sys/lib/kstring.c rach2/sys/lib/kctypes.c
 * host-deps: tests/host/compat/paging_host.c
 *
 * The disk suites boot the driver on QEMU's ich9-ahci, which resets when
 * told, answers the handoff, brings its link up and completes every command.
 * This test compiles ahci.c natively and gives it a model through its
 * register interface (`ahci_set_io`). The model answers reads from a register
 * array and moves its state on when the driver writes, so a row can make a
 * reset that never completes, firmware that never answers the handoff, a
 * port whose link never comes up, a command that never completes, or a
 * device that reports a fatal task-file status.
 *
 * A row checks what the driver returned and the registers and records it
 * left; for a failed polled command it also checks the warning logged.
 */

#include <stdarg.h>

extern int printf(const char *fmt, ...);
extern int strcmp(const char *a, const char *b);
extern int fork(void);
extern int waitpid(int pid, int *status, int options);
extern void _exit(int status);

#include "../../rach2/sys/block/ahci.h"
#include "../../rach2/sys/block/block.h"
#include "../../rach2/sys/klog.h"
#include "../../rach2/sys/latency.h"

#define REGS 0x400

static uint32_t reg[REGS];
/* What this run's controller does, set before each case. `fatal_status` 1
   gives a task-file error, and 2 also raises the port's task-file error
   interrupt. `engine_stuck` keeps PxCMD.CR set when ST is cleared. */
static int reset_completes;
static int handoff_answers;
static int link_comes_up;
static int command_completes;
static int fatal_status;
static int engine_stuck;
static int immediate_interrupt;
/* Reads of PxCMD while the engine is stuck. */
static unsigned stop_reads;
/* Reads of the command-issue register a command takes to complete, 0 for at
   once: a slow device, such as a flush a busy emulator's host takes seconds
   over, which the driver's polled wait must outlast. */
static unsigned complete_after;
/* The reads of PxCI left before a slow command completes. */
static unsigned pending_reads;
/* Warnings the driver logged, since a polled command that fails is reported
   only as EIO to its caller and the log is what says why, with the command
   and the reason the last one gave, when it gave them. */
static unsigned ahci_warnings;
static unsigned logged_command;
static const char *logged_why;
/* Commands issued, those issued while port 0's bit of IS was still set, and
   the block layer's completions. */
static unsigned issued;
static unsigned stale_issues;
static unsigned completed;
static uint32_t interrupts_masked;
/* The byte count the model writes back for a command, which the driver
   compares with what it asked for. */
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

/**
 * Read register `off` of the model; an offset past the array reads 0.
 *
 * A read of PxCMD while the engine is stuck is counted in `stop_reads`. A
 * read of PxCI is counted in `ci_reads`, and the read that ends a slow
 * command's `complete_after` clears PxCI and runs `on_command_issued`.
 */
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
        if (pending_reads && --pending_reads == 0) {
            reg[off] = 0;
            if (on_command_issued) {
                on_command_issued();
            }
        }
    }
    return reg[off];
}

/**
 * Issue the commands of PxCI bits `v`, as a write of PxCI does.
 *
 * A command the controller completes clears its bit and reports the bytes it
 * moved, through `on_command_issued`; one it does not complete leaves the bit
 * set until the driver's wait gives up, and a slow one clears it after
 * `complete_after` reads of the register. The driver checks the byte count,
 * so the model must give one. With `immediate_interrupt` a completion also
 * sets port 0's bits of PxIS and IS. The issue is counted in `issued`, and in
 * `stale_issues` when port 0's bit of IS is still set.
 */
static void model_issue(uint32_t v)
{
    if (reg[R_IS] & 1u) {
        stale_issues++;
    }
    issued++;
    if (complete_after && command_completes) {
        reg[P0 + P_CI] = v;
        pending_reads = complete_after;
        return;
    }
    reg[P0 + P_CI] = command_completes ? 0 : v;
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
        reg[P0 + P_TFD] = 0x41; /* DRDY and ERR set, BSY clear */
    }
}

/**
 * Write `v` to register `off` of the model, which moves its state as a
 * controller's would; an offset past the array is dropped.
 *
 * GHC.HR clears itself when the reset completes, and a controller that never
 * finishes keeps it set. A reset also clears GHC.AE, so a driver that does
 * not set AE again afterwards fails. The firmware answers BOHC.OOS by
 * clearing its BOS. A PxSCTL write of DET 1 takes the link down, and DET 0
 * brings it up when `link_comes_up`, so a port that starts with no link gets
 * one only through a reset. PxCMD.CR and FR follow ST and FRE, except that a
 * stuck engine keeps CR; an engine that stops clears PxCI. IS, PxIS and
 * PxSERR are write-one-to-clear. A write of PxCI issues commands.
 */
static void model_w32(uint32_t off, uint32_t v)
{
    if (off >= REGS) {
        return;
    }
    if (off == R_GHC) {
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
        if ((v & BOHC_OOS) && handoff_answers) {
            v &= ~BOHC_BOS;
        }
        reg[off] = v;
        return;
    }
    if (off == P0 + P_SCTL) {
        reg[off] = v;
        if ((v & 0xFu) == 1u) {
            reg[P0 + P_SSTS] = 0x000u;
        } else if ((v & 0xFu) == 0u && link_comes_up) {
            reg[P0 + P_SSTS] = 0x123u;
        }
        return;
    }
    if (off == P0 + P_CMD) {
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
        reg[off] &= ~v;
        return;
    }
    if (off == P0 + P_CI) {
        model_issue(v);
        return;
    }
    reg[off] = v;
}

/** The model's delay, which passes no time. */
static void model_udelay(uint32_t us)
{
    (void)us;
}

/* The register interface `model_reset` gives the driver. */
static const struct ahci_io model_io = {model_r32, model_w32, model_udelay};

#include "../../rach2/sys/pci.h"

/** Find one controller, an Intel ICH9 AHCI function (8086:2922) at 0:31.2
 *  with its ABAR at 0xFEBF0000, and none after it. */
bool pci_find_device_skip(uint8_t cls, uint8_t sub, pci_device_t *skip, pci_device_t *out)
{
    (void)cls;
    (void)sub;
    if (skip) {
        return 0;
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

/** Whether a BAR's type field says 64-bit. */
bool pci_bar_is_64bit(uint32_t bar)
{
    return (bar & 0x6u) == 0x4u;
}

/** Whether a BAR is a memory BAR: bit 0 clear. */
bool pci_bar_is_memory(uint32_t bar)
{
    return (bar & 1u) == 0;
}

/** A memory BAR's address, without its flag bits. */
uint32_t pci_bar_address(uint32_t bar)
{
    return bar & ~0xFu;
}

/** Configuration space reads 0. */
uint16_t pci_read_config16(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off)
{
    (void)bus;
    (void)dev;
    (void)fn;
    (void)off;
    return 0;
}

/** Configuration space reads 0. */
uint8_t pci_read_config8(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off)
{
    (void)bus;
    (void)dev;
    (void)fn;
    (void)off;
    return 0;
}

/** A configuration space write is dropped. */
void pci_write_config16(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off, uint16_t v)
{
    (void)bus;
    (void)dev;
    (void)fn;
    (void)off;
    (void)v;
}

/** Nothing to enable. */
void pci_enable_bus_master(pci_device_t *dev)
{
    (void)dev;
}

/** Nothing to enable. */
void pci_enable_memory_space(pci_device_t *dev)
{
    (void)dev;
}

/* The window the driver would map. Its address is never dereferenced here:
   every register access goes through the model. */
static uint8_t window[REGS];

/** Map the controller's registers: the address of `window`. */
uintptr_t map_mmio(uintptr_t phys, size_t size)
{
    (void)phys;
    (void)size;
    return (uintptr_t)(unsigned long)window;
}

/** The kernel's delay, which passes no time. */
void k_udelay(uint32_t us)
{
    (void)us;
}

/** The model's tables are in host memory, with no cache to write back. */
void arch_dma_flush(const volatile void *p, size_t len)
{
    (void)p;
    (void)len;
}

/* What `diskcmd_allowed` answers for every command; a row clears it. */
static int policy_allows = 1;

/** The disk command policy: `policy_allows`. */
int diskcmd_allowed(uint8_t command)
{
    (void)command;
    return policy_allows;
}

/* The policy's name, which the driver's refusal logs. */
const char diskcmd_policy_banner[] = "the model's policy";

/** The kernel log: count the driver's warnings in `ahci_warnings`, and from
 *  a failed command's warning record the command and the reason in
 *  `logged_command` and `logged_why`. */
void klog(int level, const char *subsystem, const char *format, ...)
{
    va_list ap;

    if (level != KLOG_WARN || strcmp(subsystem, "ahci") != 0) {
        return;
    }
    ahci_warnings++;
    logged_command = 0;
    logged_why = NULL;
    if (strcmp(format, "port %d: command 0x%X failed: %s") == 0) {
        va_start(ap, format);
        (void)va_arg(ap, int);
        logged_command = va_arg(ap, unsigned);
        logged_why = va_arg(ap, const char *);
        va_end(ap);
    }
}

/** Interrupts are on unless `interrupts_masked`. */
int kirq_enabled(void)
{
    return !interrupts_masked;
}

/** The console; output is dropped. */
void console_printf(const char *format, ...)
{
    (void)format;
}

block_device_t *block0;

/** Count a command for /proc/disk; this test reads no counter. */
void block_count_command(void)
{}

/** Count a flush for /proc/disk. */
void block_count_flush(void)
{}

/** Count a poll for /proc/disk. */
void block_count_poll(void)
{}

/** Count a batch of `sectors` for /proc/disk. */
void block_count_batch(uint32_t sectors)
{
    (void)sectors;
}

/** The block layer's completion: count it in `completed`. A completion
 *  releases the request's DMA buffer, so the recovery rows require none. */
void block_request_complete(struct block_request *request, uint32_t generation, int status)
{
    (void)request;
    (void)generation;
    (void)status;
    completed++;
}

/** No interrupt vector is free. */
int isr_alloc_vector(void)
{
    return -1;
}

/** No vector is given out, so none comes back. */
void isr_free_vector(int vector)
{
    (void)vector;
}

/** No token is on the command line. */
int kcmdline_has(const char *token)
{
    (void)token;
    return 0;
}

/** Mask interrupts, and return whether they were masked. */
uint32_t kirq_save(void)
{
    uint32_t old = interrupts_masked;

    interrupts_masked = 1;
    return old;
}

/** Restore the mask `kirq_save` returned. */
void kirq_restore(uint32_t flags)
{
    interrupts_masked = flags;
}

/** MSI is not ready. */
int isr_msi_ready(void)
{
    return 0;
}

static int disk_spans;          /* delays noted for the disk */
static uint32_t disk_span_from; /* the last one's start */

/** Note a delay: for the disk, count it and keep its start. */
void latency_span(int which, uint32_t from, uint32_t to)
{
    if (which == LAT_DISK) {
        disk_spans++;
        disk_span_from = from;
    }
    (void)to;
}

/** MSI cannot be enabled. */
int pci_msi_enable(const pci_device_t *dev, uint8_t vector)
{
    (void)dev;
    (void)vector;
    return -1;
}

/** The clock reads 0. */
uint32_t timer_now_us(void)
{
    return 0;
}

/** The disk registry makes no record. */
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

/* The heap and the DMA pool, for a controller without 64-bit addressing:
   the record of the pages a buffer above 4 GiB moves through, and those
   pages, from an arena whose pages are counted out and back. A host buffer
   lies above 4 GiB, as the identity of `paging_virt_to_phys` gives it, so
   every piece of one moves through a page. */
extern void *calloc(unsigned long count, unsigned long size);
extern void free(void *p);

#define POOL_PAGES 8
static uint8_t pool[POOL_PAGES][4096] __attribute__((aligned(4096)));
static int pool_taken[POOL_PAGES];
static unsigned pool_out;

/** The kernel heap: zeroed host memory. */
void *kzalloc(size_t size)
{
    return calloc(1, size);
}

/** Free host memory from `kzalloc`. */
void kfree(void *p)
{
    free(p);
}

/** Take a free page of `pool`, counting it in `pool_out`; NULL when every
 *  page is taken. */
void *alloc_kernel_frame(void)
{
    int i;

    for (i = 0; i < POOL_PAGES; i++) {
        if (!pool_taken[i]) {
            pool_taken[i] = 1;
            pool_out++;
            return pool[i];
        }
    }
    return 0;
}

/** Return 1 when `p` is the start of a page of `pool`, and 0 otherwise. */
static int in_pool(const volatile void *p)
{
    int i;

    for (i = 0; i < POOL_PAGES; i++) {
        if ((const volatile uint8_t *)p == pool[i]) {
            return 1;
        }
    }
    return 0;
}

/** Register no handler. */
void register_isr_handler(uint8_t vector, isr_handler_t handler, const char *name)
{
    (void)vector;
    (void)handler;
    (void)name;
}

/** What the controller does when a command completes: write `moved_bytes`
 *  back into the command header's byte count. */
static void complete_command(void)
{
    volatile ahci_cmd_header_t *hdr = (volatile ahci_cmd_header_t *)ahci_disks[0].cmdlist;

    hdr->prdbc = moved_bytes;
}

/** Return 1 when the strings are equal, and 0 otherwise. */
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

/** Reset the model to a controller that does everything right, with no BIOS
 *  handoff and 64-bit addressing, and give it to the driver; a row then
 *  changes one thing about it. */
static void model_reset(void)
{
    unsigned i;

    for (i = 0; i < REGS; i++) {
        reg[i] = 0;
    }
    reg[R_CAP] = 0x80000001u | (31u << 8); /* S64A, NCS 31 (32 slots), NP 1 */
    reg[R_PI] = 0x1u;                      /* port 0 implemented */
    reg[R_VS] = 0x10300u;
    reg[P0 + P_SSTS] = 0x123u;     /* DET 3, IPM 1, SPD 2: a live link */
    reg[P0 + P_SIG] = 0x00000101u; /* a disk, not an ATAPI device */
    reg[P0 + P_TFD] = 0;
    ci_reads = 0;
    complete_after = 0;
    pending_reads = 0;
    ahci_warnings = 0;
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
    reg[R_CAP2] = 0u;
    block0 = 0;
    ahci_dma64 = 1;
    ahci_set_io(&model_io);
}

/* The requests the recovery and completion rows hand the driver, and what a
   recovery row's child expects: a panic, and whether a request was active. */
static struct block_request active_request;
static struct block_request queued_request;
static int expect_active;
static int expect_panic;

/**
 * The kernel's panic, in a recovery row's child: exit 42 for the driver's
 * refusal to go on while the command engine runs, and 43 for any other.
 *
 * The refusal counts only with interrupts masked, the engine's stop polled
 * more than once and left with ST clear and CR set, no command issued or
 * completed, and the active request, the queue and the command table as the
 * row left them: the panic must come before any release or reuse of the
 * DMA state.
 */
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
 * Make the command engine fail to stop in recovery path `path`, and require
 * the panic `k_panic` accepts.
 *
 * Path 0 is a fatal interrupt with the command outstanding, 1 a drain of the
 * queue, 2 a fatal interrupt after PxCI cleared, and 3 a failed polled
 * command with no request active. The row runs in a forked child, so that
 * after the panic nothing returns into code that could reuse the
 * controller's buffer.
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
            ahci_polled_failed(d, ATA_IDENTIFY, "the row's recovery");
        }
        _exit(45);
    }
    CHECK("the recovery child is reaped", waitpid(pid, &status, 0) == pid);
    CHECK("failed engine stop preserves DMA ownership before halting", status == (42 << 8));
}

/**
 * Check the interrupt handler when completions arrive at once.
 *
 * A completed write issues its flush, and the flush completes before the
 * handler returns. The handler leaves that completion, and another port's
 * bit of IS, pending; its next call completes the request and starts the
 * queued read, and the call after that completes the read and notes its
 * delay from the time it was taken.
 */
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

/**
 * Check that a polled command's interrupt is acknowledged before the queue
 * it held back resumes, whatever its result.
 *
 * `fault` 0 completes the command, 1 completes it with a fatal task-file
 * status, and 2 never completes it and raises the task-file error interrupt.
 * The queued request starts after it, and the queued completion and another
 * port's bit of IS stay pending.
 */
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

/** Issue IDENTIFY on the model's port, with interrupts on or off as
 * `masked` says, for a command that completes after `after` polls of the
 * command-issue register, or never for `after` of 0 with completion off. */
static int polled_identify(unsigned after, int masked)
{
    static uint8_t buf[512];
    int rc;

    model_reset();
    (void)ahci_select_port();
    (void)ahci_port_start(&ahci_disks[0]);
    command_completes = after != 0;
    complete_after = after;
    ci_reads = 0;
    ahci_warnings = 0;
    interrupts_masked = (uint32_t)masked;
    rc = ahci_run_command(&ahci_disks[0], ATA_IDENTIFY, 0, 0, buf, sizeof(buf), 0);
    interrupts_masked = 0;
    return rc;
}

/**
 * Check a buffer above 4 GiB on controllers with and without 64-bit
 * addressing.
 *
 * With CAP.S64A set the table names the buffer's own pages, both halves.
 * With it clear a write's pieces are copied into pages of the DMA pool and
 * the table names those pages, each address's high half 0; a read's are
 * copied back from them into the buffer after the command. The pages are
 * taken once and kept for the read after the write. A host buffer lies above
 * 4 GiB, so every piece of one goes through a page.
 */
static void dma32_rows(void)
{
    static uint8_t out[3 * 4096] __attribute__((aligned(4096)));
    static uint8_t back[3 * 4096] __attribute__((aligned(4096)));
    struct ahci_disk *d = &ahci_disks[0];
    volatile ahci_prdt_t *prdt = (volatile ahci_prdt_t *)(d->cmdtable + AHCI_PRDT_OFFSET);
    uint64_t at;
    unsigned i, n, same;

    model_reset();
    CHECK("the port starts for the 32-bit rows",
          ahci_select_port() == 0 && ahci_port_start(d) == 0);
    moved_bytes = 2 * 4096;
    CHECK("a 64-bit controller is given a read of a buffer above 4 GiB",
          ahci_run_command(d, ATA_READ_DMA_EXT, 0, 16, out + 2048, 2 * 4096, 0) == 0);
    at = ((uint64_t)prdt[1].dbau << 32) | prdt[1].dba;
    CHECK("named by its own pages, both halves",
          (uintptr_t)(out + 4096) > 0xFFFFFFFFu && at == (uintptr_t)(out + 4096) && pool_out == 0);

    ahci_dma64 = 0;
    d->bounce = kzalloc(sizeof(*d->bounce));
    for (i = 0; i < sizeof(out); i++) {
        out[i] = (uint8_t)(i * 13u + 5u);
    }
    CHECK("a 32-bit controller is given a write of a buffer above 4 GiB",
          ahci_run_command(d, ATA_WRITE_DMA_EXT, 0, 16, out + 2048, 2 * 4096, 1) == 0);
    CHECK("in three pieces, each through a page of the DMA pool",
          (((ahci_cmd_header_t *)d->cmdlist)->dw0 >> 16) == 3 && pool_out == 3 &&
              in_pool(d->bounce->page[0]) && in_pool(d->bounce->page[2]));
    for (n = 0, same = 1; n < 3; n++) {
        same &= prdt[n].dbau == 0 && prdt[n].dba == (uint32_t)(uintptr_t)d->bounce->page[n];
    }
    CHECK("the table names the pages, the high halves 0", same);
    if (!d->bounce->page[0] || !d->bounce->page[1] || !d->bounce->page[2]) {
        CHECK("the pages exist for the rows after", 0);
        ahci_dma64 = 1;
        return;
    }
    for (i = 0, same = 1; i < 2048; i++) {
        same &= d->bounce->page[0][i] == out[2048 + i] && d->bounce->page[2][i] == out[8192 + i];
    }
    for (i = 0; i < 4096; i++) {
        same &= d->bounce->page[1][i] == out[4096 + i];
    }
    CHECK("holding the bytes written, the whole page of the middle piece", same);

    for (n = 0; n < 3; n++) {
        for (i = 0; i < 4096; i++) {
            d->bounce->page[n][i] = (uint8_t)(n * 31u + i * 3u);
        }
    }
    for (i = 0; i < sizeof(back); i++) {
        back[i] = 0;
    }
    CHECK("a read into a buffer above 4 GiB is given",
          ahci_run_command(d, ATA_READ_DMA_EXT, 0, 16, back + 2048, 2 * 4096, 0) == 0);
    for (i = 0, same = 1; i < 2048; i++) {
        same &= back[2048 + i] == (uint8_t)(i * 3u) && back[4096 + i] == (uint8_t)(31u + i * 3u) &&
                back[6144 + i] == (uint8_t)(31u + (i + 2048) * 3u) &&
                back[8192 + i] == (uint8_t)(62u + i * 3u);
    }
    CHECK("and the bytes come back from the pages into it", same && back[2047] == 0);
    CHECK("the pages taken once and kept", pool_out == 3);
    ahci_dma64 = 1;
}

/**
 * Check a polled command's wait.
 *
 * One that never completes is refused after the wait polled and gave up, and
 * the driver logs the command and why, since its caller sees only EIO. With
 * interrupts on, one that answers after ten seconds of polls, as a flush on
 * a busy emulator's host can, completes; with them off, where every tick of
 * the wait is lost to the clock, it is refused after five, and one that
 * answers after a second and a half completes. A command that moves fewer
 * bytes than asked is refused and logged.
 */
static void command_waits(void)
{
    static uint8_t buf[512];

    CHECK("a command that never completes is refused", polled_identify(0, 0) != 0);
    CHECK("and the wait polled and then gave up", ci_reads > 1);
    CHECK("and the driver logged the command and why",
          ahci_warnings == 1 && logged_command == ATA_IDENTIFY && logged_why &&
              same_text(logged_why, "no answer in thirty seconds"));
    CHECK("with interrupts on, a command that answers after ten seconds of polls completes",
          polled_identify(1000000, 0) == 0 && ahci_warnings == 0);
    CHECK("with interrupts off, it is refused after five seconds",
          polled_identify(1000000, 1) != 0 && ahci_warnings == 1 && logged_why &&
              same_text(logged_why, "no answer in five seconds with interrupts off"));
    CHECK("with interrupts off, one that answers after a second and a half completes",
          polled_identify(150000, 1) == 0 && ahci_warnings == 0);

    model_reset();
    (void)ahci_select_port();
    (void)ahci_port_start(&ahci_disks[0]);
    moved_bytes = 256;
    ahci_warnings = 0;
    CHECK("a command that moves fewer bytes than asked is refused and logged",
          ahci_run_command(&ahci_disks[0], ATA_IDENTIFY, 0, 0, buf, sizeof(buf), 0) != 0 &&
              ahci_warnings == 1);
}

/**
 * Run the rows, each on a fresh model, and return the count of failed
 * checks, at most 255.
 *
 * Each row drives the step it is about rather than the whole of
 * `ahci_init`, which would fail on a model that cannot answer IDENTIFY and
 * so pass any row that expects a refusal.
 *
 * IDENTIFY data of a 4096-byte logical sector (word 106 with bit 14 valid
 * and bit 12 set, words 117 and 118 giving 2048 words) is refused by name,
 * as the block Readme states, and the same words with bit 12 clear are
 * taken, with the sector count from words 100 to 103. The controller's reset
 * leaves it in AHCI mode with its interrupts off, and a reset that never
 * completes is refused. Firmware that never answers the handoff does not
 * stop the driver, and firmware that answers clears its ownership bit. A
 * port with a disk is chosen; a port with no device, one with an ATAPI
 * signature and a controller with no implemented port are refused; a port
 * whose link comes up only on a reset is taken, with the reset released. A
 * started port has the command engine and the FIS receiver running.
 *
 * A 2048-sector read, the block layer's limit, into a buffer that starts in
 * the middle of a page is described as 257 entries, a half page at each end
 * and a page between, with the FIS naming the sectors. A buffer of 265 pages
 * from the middle of a page is 266 pieces, more than the table's 264
 * entries, and is refused with no command issued. A device that reports a
 * fatal status is refused, and a command the disk policy refuses never
 * reaches the port.
 */
int main(void)
{
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

    model_reset();
    CHECK("a controller that resets is taken", ahci_hba_own() == 0);
    CHECK("and is left in AHCI mode", (reg[R_GHC] & GHC_AE) != 0);
    CHECK("with interrupts off, because this driver polls", (reg[R_GHC] & 0x2u) == 0);

    model_reset();
    reset_completes = 0;
    CHECK("a reset that never completes is refused", ahci_hba_own() != 0);

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

    model_reset();
    CHECK("a port with a disk on it is chosen", ahci_select_port() == 0);
    CHECK("and it is port 0", ahci_port == 0);

    model_reset();
    reg[P0 + P_SSTS] = 0x000u; /* DET 0: nothing on the link */
    link_comes_up = 0;
    CHECK("a port with no device is refused", ahci_select_port() != 0);

    model_reset();
    reg[P0 + P_SSTS] = 0x000u;
    link_comes_up = 1;
    CHECK("a port whose link comes up on reset is taken", ahci_select_port() == 0);
    CHECK("and the reset was released", (reg[P0 + P_SCTL] & 0xFu) != 1u);

    model_reset();
    reg[P0 + P_SSTS] = 0x113u;
    reg[P0 + P_SIG] = 0xEB140101u; /* an ATAPI signature */
    CHECK("a port whose device is not a disk is refused", ahci_select_port() != 0);

    model_reset();
    reg[R_PI] = 0; /* no port implemented at all */
    CHECK("a controller with no implemented port is refused", ahci_select_port() != 0);

    model_reset();
    CHECK("a port can be selected", ahci_select_port() == 0);
    CHECK("and started", ahci_port_start(&ahci_disks[0]) == 0);
    CHECK("with the command engine running", (reg[P0 + P_CMD] & CMD_CR) != 0);
    CHECK("and the FIS receiver running", (reg[P0 + P_CMD] & CMD_FR) != 0);

    {
        static uint8_t buf[512];

        model_reset();
        CHECK("the port starts for the command rows",
              ahci_select_port() == 0 && ahci_port_start(&ahci_disks[0]) == 0);
        CHECK("a command that completes is accepted",
              ahci_run_command(&ahci_disks[0], ATA_IDENTIFY, 0, 0, buf, sizeof(buf), 0) == 0);

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

                CHECK("a buffer of more pieces than the table holds is refused",
                      ahci_run_command(&ahci_disks[0], ATA_READ_DMA_EXT, 0, 2048, huge + 2048,
                                       265 * 4096, 0) != 0);
                CHECK("and no command was issued for it", issued == issued_before);
            }
        }

        command_waits();
        dma32_rows();

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
    return fails > 255 ? 255 : fails;
}
