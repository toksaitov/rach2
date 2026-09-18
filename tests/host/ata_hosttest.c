/*
 * ata_hosttest.c: the ATA driver against a model of a drive.
 *
 * host-deps: rach2/sys/lib/kstring.c rach2/sys/lib/kctypes.c
 *
 * The disk suites boot this driver against QEMU's IDE, where a command
 * completes when the emulator's main loop runs, and the machine's disk is
 * behind the AHCI driver. What a boot cannot show is the driver's two
 * paths meeting on one channel: a caller that owns the channel and sleeps
 * for its DMA command, and a caller that cannot sleep, from a page fault,
 * arriving on the same channel while that command is in flight. The
 * emulator shows a drive in a DMA transfer as ready with DRQ set and BSY
 * clear, and a second command written to it then is refused or answered
 * with the first command's data; the self-build suite under KVM in a full
 * pool found both (entries 102 and 103).
 *
 * So the driver is compiled natively and given a model of a drive through
 * the port interface it now reaches registers by. The model answers its
 * registers from a phase, moves the phase on as the emulator would, after a
 * number of status reads, when the driver pauses off the device, or when the
 * owner sleeps, and counts what the driver did: the status reads, the
 * commands it refused, and the data port reads that landed in another
 * command's transfer. Every row names what the driver must do, and the
 * check is on what it returned, what the model counted, and the sectors.
 */

extern int printf(const char *fmt, ...);

#define ATA_HOSTTEST

#include "../../rach2/sys/block/ata.h"
#include "../../rach2/sys/block/block.h"
#include "../../rach2/sys/lib/kerrno.h"

static int fails;

#define CHECK(condition, detail)                    \
    do {                                            \
        if (!(condition)) {                         \
            printf("ata-test: fail: %s\n", detail); \
            fails++;                                \
        }                                           \
    } while (0)

/* ---- what the driver reaches for outside itself ---- */

/* Interrupts as the test's flag says, not the host's register. */
static int masked;
static int in_syscall;

int ata_hosttest_interrupts_on(void)
{
    return !masked;
}

int proc_in_syscall(void)
{
    return in_syscall;
}

uint32_t kirq_save(void)
{
    uint32_t old = (uint32_t)masked;

    masked = 1;
    return old;
}

/* The model notes an unmasking between a DMA command and its engine's
   start, which is where a caller that cannot sleep would find a drive
   waiting for its engine. */
static int dma_written_not_started;
static int unmasked_between;

void kirq_restore(uint32_t flags)
{
    masked = (int)flags;
    if (!masked && dma_written_not_started) {
        unmasked_between = 1;
    }
}

static char last_record[256];
static int records;

void klog(int level, const char *subsystem, const char *format, ...)
{
    unsigned int i;

    (void)level;
    (void)subsystem;
    for (i = 0; format[i] != '\0' && i < sizeof(last_record) - 1; i++) {
        last_record[i] = format[i];
    }
    last_record[i] = '\0';
    records++;
}

const char diskcmd_policy_banner[] = "host test";

/* The boot line, which names no option here. */
int kcmdline_has(const char *flag)
{
    (void)flag;
    return 0;
}

int diskcmd_allowed(uint8_t command)
{
    (void)command;
    return 1;
}

block_device_t *block0;

/* The scheduler runs unless a row stops it, as a shutdown does. */
static int scheduler_on = 1;

int sched_is_enabled(void)
{
    return scheduler_on;
}

static uint32_t polls;
static uint32_t wait_ticks;

void block_count_wait_tick(void)
{
    wait_ticks++;
}

void block_count_command(void)
{}

void block_count_flush(void)
{}

void block_count_poll(void)
{
    polls++;
}

void block_count_batch(uint32_t sectors)
{
    (void)sectors;
}

#include "../../rach2/sys/pci.h"

bool pci_find_device(uint8_t class_code, uint8_t subclass, pci_device_t *out)
{
    (void)class_code;
    (void)subclass;
    (void)out;
    return false;
}

uint8_t pci_read_config8(uint8_t bus, uint8_t device, uint8_t function, uint8_t offset)
{
    (void)bus;
    (void)device;
    (void)function;
    (void)offset;
    return 0;
}

uint32_t pci_read_config32(uint8_t bus, uint8_t device, uint8_t function, uint8_t offset)
{
    (void)bus;
    (void)device;
    (void)function;
    (void)offset;
    return 0;
}

void pci_enable_bus_master(pci_device_t *dev)
{
    (void)dev;
}

#include "../../rach2/sys/isr.h"

void register_isr_handler(uint8_t vector, isr_handler_t handler, const char *name)
{
    (void)vector;
    (void)handler;
    (void)name;
}

#include "../../rach2/sys/block/disk.h"

struct disk *disk_register(block_device_t *dev, const char *transport, int location,
                           const volatile uint16_t *identify)
{
    (void)dev;
    (void)transport;
    (void)location;
    (void)identify;
    return 0;
}

#include "../../rach2/sys/proc/wait_queue.h"

/* ---- the model ---- */

#define IO   0x1F0
#define CTRL 0x3F6
#define BM   0xC000

#define SECTORS 256u
#define SECTOR  512u

enum phase {
    IDLE,
    PIO_READ_BUSY,
    PIO_READ_DRQ,
    PIO_WRITE_DRQ,
    PIO_WRITE_BUSY,
    DMA_WAIT,
    DMA_RUN,
    FLUSH_BUSY
};

static int phase;
static int answer_after; /* status reads a busy phase lasts; -1 is never */
static int reads_in_phase;
static int complete_on_pause; /* the pause at which a running DMA completes */
static int dma_on_reads;      /* a running DMA also completes after the status reads */
static int pauses;
static uint8_t cur[8], prev[8];
static uint64_t cmd_lba;
static uint32_t sectors_left;
static int cmd_write;
static int bm_active, bm_intr;
static uint8_t disk[SECTORS][SECTOR];
static uint32_t status_reads, refused, garbage_reads, garbage_writes, commands;
static int masked_at_dma_cmd, masked_at_start, masked_at_pio_cmd, masked_at_last_data;
static int dma_fails;      /* the running DMA ends in ERR */
static int error_pending;  /* ERR shows in the status until the next command */
static int drive_at_flush; /* the drive register's value at the last flush */
static void (*on_sleep)(void);
static int sleeps;
static uint32_t reads_at_sleep; /* the status reads when the owner came to sleep */
static uint32_t reads_at_start; /* the status reads when the engine was started */

/* The channel's bounce buffer, which the driver's DMA descriptor names by
   a physical address the host cannot follow; defined below the driver. */
static uint8_t *model_bounce(void);

static uint8_t model_status(void)
{
    switch (phase) {
    case IDLE:
        return (uint8_t)(0x50 | (error_pending ? 0x01 : 0));
    case PIO_READ_BUSY:
    case PIO_WRITE_BUSY:
    case FLUSH_BUSY:
        return 0xD0;
    default:
        return 0x58; /* a data phase: ready with DRQ, BSY clear */
    }
}

/** The emulator's completion of whatever phase is pending. */
static void model_advance(void)
{
    uint32_t i;

    switch (phase) {
    case PIO_READ_BUSY:
        phase = PIO_READ_DRQ;
        break;
    case PIO_WRITE_BUSY:
    case FLUSH_BUSY:
        phase = IDLE;
        break;
    case DMA_RUN:
        if (dma_fails) {
            error_pending = 1;
        } else {
            for (i = 0; i < sectors_left; i++) {
                uint8_t *sector = disk[(cmd_lba + i) % SECTORS];

                if (cmd_write) {
                    k_memcpy(sector, model_bounce() + i * SECTOR, SECTOR);
                } else {
                    k_memcpy(model_bounce() + i * SECTOR, sector, SECTOR);
                }
            }
        }
        phase = IDLE;
        bm_active = 0;
        bm_intr = 1;
        break;
    default:
        break;
    }
    reads_in_phase = 0;
}

static uint8_t model_in8(uint16_t port)
{
    if (port == IO + 7) {
        uint8_t status = model_status();

        status_reads++;
        if (answer_after >= 0 && ((status & 0x80) != 0 || (phase == DMA_RUN && dma_on_reads)) &&
            ++reads_in_phase >= answer_after) {
            model_advance();
        }
        return status;
    }
    if (port == CTRL) {
        return model_status();
    }
    if (port == BM + 2) {
        return (uint8_t)((bm_active ? 1 : 0) | (bm_intr ? 4 : 0));
    }
    if (port >= IO && port < IO + 8) {
        return cur[port - IO];
    }
    return 0;
}

static void model_command(uint8_t command)
{
    uint8_t status = model_status();

    if (status & 0x88) {
        refused++;
        return;
    }
    commands++;
    error_pending = 0;
    if (command == 0xE7 || command == 0xEA) {
        drive_at_flush = cur[6];
    }
    switch (command) {
    case 0x20: /* READ SECTORS */
    case 0x30: /* WRITE SECTORS */
    case 0xC8: /* READ DMA */
    case 0xCA: /* WRITE DMA */
        sectors_left = cur[2] == 0 ? 256u : cur[2];
        cmd_lba = (uint64_t)cur[3] | ((uint64_t)cur[4] << 8) | ((uint64_t)cur[5] << 16) |
                  ((uint64_t)(cur[6] & 0x0F) << 24);
        break;
    case 0x24: /* READ SECTORS EXT */
    case 0x34: /* WRITE SECTORS EXT */
    case 0x25: /* READ DMA EXT */
    case 0x35: /* WRITE DMA EXT */
        sectors_left = ((uint32_t)prev[2] << 8) | cur[2];
        cmd_lba = (uint64_t)cur[3] | ((uint64_t)cur[4] << 8) | ((uint64_t)cur[5] << 16) |
                  ((uint64_t)prev[3] << 24) | ((uint64_t)prev[4] << 32) | ((uint64_t)prev[5] << 40);
        break;
    case 0xE7:
    case 0xEA:
        break;
    default:
        printf("ata-test: the driver wrote command 0x%02X\n", command);
        CHECK(0, "the driver wrote a command the model does not know");
        return;
    }
    reads_in_phase = 0;
    switch (command) {
    case 0x20:
    case 0x24:
        cmd_write = 0;
        phase = PIO_READ_BUSY;
        masked_at_pio_cmd = masked;
        break;
    case 0x30:
    case 0x34:
        cmd_write = 1;
        phase = PIO_WRITE_DRQ;
        masked_at_pio_cmd = masked;
        break;
    case 0xC8:
    case 0xCA:
    case 0x25:
    case 0x35:
        cmd_write = command == 0x35 || command == 0xCA;
        phase = DMA_WAIT;
        masked_at_dma_cmd = masked;
        dma_written_not_started = 1;
        break;
    default:
        phase = FLUSH_BUSY;
        break;
    }
}

static void model_out8(uint16_t port, uint8_t value)
{
    if (port == IO + 7) {
        model_command(value);
        return;
    }
    if (port >= IO && port < IO + 7) {
        prev[port - IO] = cur[port - IO];
        cur[port - IO] = value;
        return;
    }
    if (port == BM) {
        if (value & 1) {
            if (phase == DMA_WAIT) {
                phase = DMA_RUN;
                bm_active = 1;
                masked_at_start = masked;
                dma_written_not_started = 0;
                reads_at_start = status_reads;
            }
        } else {
            bm_active = 0;
        }
        return;
    }
    if (port == BM + 2) {
        if (value & 4) {
            bm_intr = 0;
        }
    }
}

static void model_ins16(uint16_t port, void *buffer, uint32_t words)
{
    uint32_t i;
    uint8_t *out = buffer;

    if (port != IO || phase != PIO_READ_DRQ) {
        garbage_reads++;
        for (i = 0; i < words * 2; i++) {
            out[i] = 0xEE;
        }
        return;
    }
    k_memcpy(out, disk[cmd_lba % SECTORS], words * 2);
    cmd_lba++;
    sectors_left--;
    phase = sectors_left ? PIO_READ_BUSY : IDLE;
    reads_in_phase = 0;
}

static void model_outs16(uint16_t port, const void *buffer, uint32_t words)
{
    if (port != IO || phase != PIO_WRITE_DRQ) {
        garbage_writes++;
        return;
    }
    k_memcpy(disk[cmd_lba % SECTORS], buffer, words * 2);
    cmd_lba++;
    sectors_left--;
    if (sectors_left == 0) {
        masked_at_last_data = masked;
        phase = PIO_WRITE_BUSY;
    }
    reads_in_phase = 0;
}

static void model_out32(uint16_t port, uint32_t value)
{
    (void)port;
    (void)value;
}

static const struct ata_io model_io = {model_in8, model_out8, model_out32, model_ins16,
                                       model_outs16};

/* The machine's ports, which the driver names as its default and this
   test never reaches. */
uint8_t inb(uint16_t port)
{
    (void)port;
    return 0;
}

void outb(uint16_t port, uint8_t value)
{
    (void)port;
    (void)value;
}

void outd(uint16_t port, uint32_t value)
{
    (void)port;
    (void)value;
}

void insw(uint16_t port, void *buffer, uint32_t words)
{
    (void)port;
    (void)buffer;
    (void)words;
}

void outsw(uint16_t port, const void *buffer, uint32_t words)
{
    (void)port;
    (void)buffer;
    (void)words;
}

/* The driver's pause: the emulator's main loop gets in here. */
void ata_hosttest_paused(void)
{
    pauses++;
    if (complete_on_pause > 0 && pauses == complete_on_pause && phase == DMA_RUN) {
        model_advance();
    }
}

/* The owner's sleep: the other process runs, and the emulator completes
   what is pending until the sleeper's condition holds. */
int wq_wait_event_nointr(wait_queue_t *wq, wq_cond_fn cond, void *arg)
{
    int spins = 0;

    (void)wq;
    reads_at_sleep = status_reads;
    if (cond(arg)) {
        return 0;
    }
    sleeps++;
    if (on_sleep) {
        void (*run)(void) = on_sleep;

        on_sleep = 0;
        run();
    }
    while (!cond(arg)) {
        model_advance();
        if (++spins > 1000) {
            CHECK(0, "a sleeper's condition never held");
            return 0;
        }
    }
    return 0;
}

void wq_wake_all(wait_queue_t *wq)
{
    (void)wq;
}

void wait_queue_init(wait_queue_t *wq)
{
    (void)wq;
}

int wq_active(const wait_queue_t *wq)
{
    (void)wq;
    return 0;
}

#include "../../rach2/sys/block/ata.c"

static uint8_t *model_bounce(void)
{
    return channels[0].bounce;
}

/* ---- the rows ---- */

static void model_reset(int answer)
{
    uint32_t s, i;

    phase = IDLE;
    answer_after = answer;
    reads_in_phase = 0;
    complete_on_pause = 0;
    dma_on_reads = 1;
    pauses = 0;
    bm_active = 0;
    bm_intr = 0;
    status_reads = refused = garbage_reads = garbage_writes = commands = 0;
    masked_at_dma_cmd = masked_at_start = masked_at_pio_cmd = masked_at_last_data = -1;
    dma_fails = 0;
    error_pending = 0;
    drive_at_flush = -1;
    scheduler_on = 1;
    wait_ticks = 0;
    dma_written_not_started = 0;
    unmasked_between = 0;
    on_sleep = 0;
    sleeps = 0;
    records = 0;
    last_record[0] = '\0';
    masked = 0;
    in_syscall = 1;
    for (s = 0; s < SECTORS; s++) {
        for (i = 0; i < SECTOR; i++) {
            disk[s][i] = (uint8_t)(s * 7 + i);
        }
    }
}

static int sector_is(const uint8_t *got, uint32_t lba)
{
    uint32_t i;

    for (i = 0; i < SECTOR; i++) {
        if (got[i] != (uint8_t)(lba * 7 + i)) {
            return 0;
        }
    }
    return 1;
}

static int contains(const char *text, const char *needle)
{
    uint32_t i, j;

    for (i = 0; text[i] != '\0'; i++) {
        for (j = 0; needle[j] != '\0' && text[i + j] == needle[j]; j++) {
        }
        if (needle[j] == '\0') {
            return 1;
        }
    }
    return 0;
}

static uint8_t owner_buf[8 * SECTOR];
static uint8_t polled_buf[SECTOR];
static int polled_rc;

/* The other process: a page fault's read on the channel, with interrupts
   off and no way to sleep, while the owner's DMA is in flight. */
static void polled_read_while_owner_sleeps(void)
{
    int was_masked = masked;
    int was_in_syscall = in_syscall;

    masked = 1;
    in_syscall = 0;
    dma_on_reads = 0;
    complete_on_pause = pauses + 3;
    polled_rc = ata_read(&drives[0].dev, 200, 1, polled_buf);
    masked = was_masked;
    in_syscall = was_in_syscall;
}

/* The tick, twice, while the owner sleeps. */
static void two_ticks_while_asleep(void)
{
    ata_tick();
    ata_tick();
}

int main(void)
{
    struct ata_drive *d = &drives[0];
    uint32_t before;
    int rc;

    ata_set_io(&model_io);
    channels[0].io = IO;
    channels[0].ctrl = CTRL;
    channels[0].bm = BM;
    channels[0].name = "primary";
    d->channel = &channels[0];
    d->slave = 0;
    d->lba48 = 1;
    d->dma = 1;
    d->sectors = SECTORS;
    d->present = 1;
    d->dev.sector_size = SECTOR;
    d->dev.sector_count = SECTORS;
    d->dev.flags = BLOCK_FAULT_SAFE;
    d->dev.max_sectors = ATA_REQ_SECTORS;
    d->dev.read = ata_read;
    d->dev.write = ata_write;
    d->dev.drvdata = d;

    /* 1. The owner's DMA read: eight status reads, then the sleep, in
          which the emulator completes it. */
    model_reset(2);
    dma_on_reads = 0;
    rc = ata_read(&d->dev, 100, 8, owner_buf);
    CHECK(rc == 0, "the owner's DMA read returns 0");
    CHECK(sleeps == 1, "the owner slept once for its DMA");
    CHECK(sector_is(owner_buf, 100) && sector_is(owner_buf + 7 * SECTOR, 107),
          "the owner's DMA read brought its sectors");
    CHECK(
        reads_at_sleep - reads_at_start == 8,
        "the sleeping wait reads the status eight times after the engine's start before it sleeps");
    CHECK(masked_at_dma_cmd == 1 && masked_at_start == 1 && !unmasked_between,
          "the DMA command and its engine's start are written with interrupts off");
    CHECK(refused == 0 && garbage_reads == 0, "the owner's read met no other command");

    /* 2. A caller that cannot sleep, on an idle channel: the data port,
          with interrupts off throughout. */
    model_reset(2);
    masked = 1;
    in_syscall = 0;
    rc = ata_read(&d->dev, 5, 1, polled_buf);
    CHECK(rc == 0 && sector_is(polled_buf, 5), "a polled read of an idle drive returns its sector");
    CHECK(sleeps == 0, "a polled read does not sleep");
    CHECK(refused == 0 && garbage_reads == 0, "a polled read of an idle drive is answered");

    /* 3. The two paths on one channel: the owner's DMA in flight, the
          drive showing DRQ, and a polled read arriving. The emulator's
          completion needs the driver off the device, as on a loaded host,
          and comes at the third pause; the polled read waits for the drive
          idle and then reads its own sector, and the owner's DMA completes
          with its own. */
    model_reset(2);
    dma_on_reads = 0;
    on_sleep = polled_read_while_owner_sleeps;
    polled_rc = -1;
    rc = ata_read(&d->dev, 100, 8, owner_buf);
    CHECK(rc == 0, "the owner's DMA read returns 0 with a polled read inside its sleep");
    CHECK(polled_rc == 0, "the polled read inside the owner's sleep returns 0");
    CHECK(sector_is(polled_buf, 200), "the polled read brought its own sector, not the DMA's data");
    CHECK(sector_is(owner_buf, 100) && sector_is(owner_buf + 7 * SECTOR, 107),
          "the owner's DMA read brought its sectors around the polled read");
    CHECK(refused == 0, "no command was written into the DMA's data phase");
    CHECK(garbage_reads == 0, "no data port read landed in the DMA's transfer");
    CHECK(pauses >= 3, "the polled read paused off the device while the DMA ran");

    /* 4. A drive that never answers: the polled wait's bound, and the
          record it leaves. */
    model_reset(-1);
    masked = 1;
    in_syscall = 0;
    before = status_reads;
    rc = ata_read(&d->dev, 5, 1, polled_buf);
    CHECK(rc == -EIO, "a polled read of a drive that never answers is -EIO");
    CHECK(status_reads - before == ATA_SPIN_BEFORE_SLEEP + ATA_POLL_PLAIN + ATA_POLL_LIMIT + 2,
          "the polled wait reads the status its plain reads, its rounds, and the record's");
    CHECK(records == 1 && contains(last_record, "no answer to a polled wait"),
          "a polled wait that gives up records it");

    /* 5. A data-port write from the owner of a drive without DMA runs
          with interrupts off from its command to its last sector. */
    model_reset(2);
    d->dma = 0;
    k_memset(polled_buf, 0x5A, SECTOR);
    rc = ata_write(&d->dev, 9, 1, polled_buf);
    CHECK(rc == 0, "a data-port write returns 0");
    CHECK(disk[9][0] == 0x5A && disk[9][SECTOR - 1] == 0x5A,
          "the data-port write reached the sector");
    CHECK(masked_at_pio_cmd == 1 && masked_at_last_data == 1,
          "a data-port transfer runs with interrupts off from its command to its last sector");
    CHECK(refused == 0 && garbage_writes == 0, "the data-port write was taken as written");
    d->dma = 1;

    /* 6. The owner's DMA fails, and a polled read runs inside its sleep,
          whose own command clears the drive's error bits: the owner's
          answer is still the failure. */
    model_reset(2);
    dma_on_reads = 0;
    dma_fails = 1;
    on_sleep = polled_read_while_owner_sleeps;
    rc = ata_read(&d->dev, 100, 8, owner_buf);
    CHECK(rc == -EIO, "a failed DMA is reported failed although a polled command followed it");
    CHECK(polled_rc == 0 && sector_is(polled_buf, 200),
          "and the polled read inside it still succeeds");

    /* 7. A write's flush selects its own drive again: the writer is the
          slave, and the polled read inside its sleep selected the
          master. */
    model_reset(2);
    dma_on_reads = 0;
    drives[1] = drives[0];
    drives[1].slave = 1;
    drives[1].dev.drvdata = &drives[1];
    on_sleep = polled_read_while_owner_sleeps;
    k_memset(owner_buf, 0x3C, SECTOR);
    rc = ata_write(&drives[1].dev, 11, 1, owner_buf);
    CHECK(rc == 0, "the slave's write returns 0 around a polled read of the master");
    CHECK(drive_at_flush >= 0 && (drive_at_flush & 0x10) != 0,
          "the flush after the write went to the slave, not the drive the polled read selected");

    /* 8. With the scheduler stopped, as at a shutdown, a caller in a
          system call polls instead of sleeping for a channel it might
          wait on forever. */
    model_reset(2);
    scheduler_on = 0;
    rc = ata_read(&d->dev, 5, 1, polled_buf);
    CHECK(rc == 0 && sector_is(polled_buf, 5),
          "a read with the scheduler stopped returns its sector");
    CHECK(sleeps == 0, "and did not sleep");

    /* 9. The tick counts a sleeper's wait: two ticks while the owner
          sleeps are two ticks in `disk-wait-ticks`. */
    model_reset(2);
    dma_on_reads = 0;
    on_sleep = two_ticks_while_asleep;
    rc = ata_read(&d->dev, 100, 8, owner_buf);
    CHECK(rc == 0, "the owner's DMA read returns 0 with two ticks inside its sleep");
    CHECK(wait_ticks == 2, "the two ticks the owner slept through are counted");

    if (fails) {
        printf("ata-test: %d failure(s)\n", fails);
        return 1;
    }
    printf("ata-test: OK\n");
    return 0;
}
