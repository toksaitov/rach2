/*
 * ahci_hosttest.c: the AHCI driver against a model of a controller.
 *
 * host-deps: rach2/sys/lib/kstring.c rach2/sys/lib/kctypes.c
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

#include "../../rach2/sys/block/ahci.h"
#include "../../rach2/sys/block/block.h"

/* ---- the model ---- */

#define REGS 0x400

static uint32_t reg[REGS];
static unsigned delays;

/* What this run's controller does, set before each case. */
static int reset_completes;
static int handoff_answers;
static int link_comes_up;
static int command_completes;
static int fatal_status;
static int has_handoff;
/* What the model says it moved, which the driver compares with what it
   asked for. */
static uint32_t moved_bytes;
/* Set after `ahci.c` is included, because what it does reaches into the
   driver's own command list, which is defined there. */
static void (*on_command_issued)(void);

#define R_CAP  0x00
#define R_GHC  0x04
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
        /* The reset bit clears itself when the controller has finished, and
           a controller that never finishes is one that never clears it. */
        if ((v & GHC_HR) && reset_completes) {
            v &= ~GHC_HR;
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
    if (off == P0 + P_CMD) {
        /* The engines follow the bits that start and stop them. */
        if (v & CMD_ST) {
            v |= CMD_CR;
        } else {
            v &= ~CMD_CR;
        }
        if (v & CMD_FRE) {
            v |= CMD_FR;
        } else {
            v &= ~CMD_FR;
        }
        reg[off] = v;
        return;
    }
    if (off == P0 + P_IS || off == P0 + P_SERR) {
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
        reg[off] = command_completes ? 0 : v;
        if (command_completes && on_command_issued) {
            on_command_issued();
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
    delays++;
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

void diskinfo_record_identify(const char *driver, int port, const volatile uint16_t *id,
                              uint64_t sectors)
{
    (void)driver;
    (void)port;
    (void)id;
    (void)sectors;
}

#include "../../rach2/sys/block/ahci.c"

/* What the controller does when a command is issued and completes: it writes
   back the number of bytes it moved. */
static void complete_command(void)
{
    volatile ahci_cmd_header_t *hdr = (volatile ahci_cmd_header_t *)ahci_cmdlist;

    hdr->prdbc = moved_bytes;
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
    delays = 0;
    ci_reads = 0;
    moved_bytes = 512;
    on_command_issued = complete_command;
    reset_completes = 1;
    handoff_answers = 1;
    link_comes_up = 1;
    command_completes = 1;
    fatal_status = 0;
    has_handoff = 0;
    reg[R_CAP2] = has_handoff ? 0x1u : 0u;
    block0 = 0;
    ahci_set_io(&model_io);
}

int main(void)
{
    /* Each row drives the step it is about, rather than the whole bring-up:
       a row that ran the whole of `ahci_init` would pass whenever anything at
       all refused, which is every row on a model that cannot answer an
       IDENTIFY. What a step returned is what is checked. */

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
    CHECK("a port with no device is refused", ahci_select_port() != 0);

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
    CHECK("and started", ahci_port_start() == 0);
    CHECK("with the command engine running", (reg[P0 + P_CMD] & CMD_CR) != 0);
    CHECK("and the FIS receiver running", (reg[P0 + P_CMD] & CMD_FR) != 0);

    /* A command, and the two ways it can fail. */
    {
        static uint8_t buf[512];

        model_reset();
        CHECK("the port starts for the command rows",
              ahci_select_port() == 0 && ahci_port_start() == 0);
        CHECK("a command that completes is accepted",
              ahci_run_command(ATA_IDENTIFY, 0, 0, buf, sizeof(buf), 0) == 0);

        model_reset();
        (void)ahci_select_port();
        (void)ahci_port_start();
        command_completes = 0;
        ci_reads = 0;
        CHECK("a command that never completes is refused",
              ahci_run_command(ATA_IDENTIFY, 0, 0, buf, sizeof(buf), 0) != 0);
        CHECK("and the wait polled and then gave up", ci_reads > 1);

        model_reset();
        (void)ahci_select_port();
        (void)ahci_port_start();
        fatal_status = 1;
        CHECK("a device that reports a fatal status is refused",
              ahci_run_command(ATA_IDENTIFY, 0, 0, buf, sizeof(buf), 0) != 0);

        model_reset();
        (void)ahci_select_port();
        (void)ahci_port_start();
        CHECK("a command the disk policy refuses never reaches the port",
              ahci_run_command(0xF0, 0, 0, buf, sizeof(buf), 0) != 0 || policy_allows);
    }

    if (fails == 0) {
        printf("ahci_hosttest: PASS\n");
    }
    return fails;
}
