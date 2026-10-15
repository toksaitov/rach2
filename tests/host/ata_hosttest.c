/*
 * ata_hosttest.c: the ATA driver of rach2/sys/block/ata.c against a model of
 * a drive, for the driver's two paths meeting on one channel.
 *
 * host-deps: rach2/sys/lib/kstring.c rach2/sys/lib/kctypes.c
 *
 * The disk suites boot the driver on QEMU's IDE, where a command completes
 * when the emulator's main loop runs. A boot does not show the driver's two
 * paths meeting on one channel: a caller that owns the channel and sleeps
 * for its DMA command, and a caller that cannot sleep, from a page fault,
 * arriving on the same channel while that command is in flight. The
 * emulator shows a drive in a DMA transfer as ready with DRQ set and BSY
 * clear, and a second command written to it then is refused or answered
 * with the first command's data.
 *
 * The test compiles ata.c natively with ATA_HOSTTEST and gives it a model of
 * a drive through its port interface (`ata_set_io`). The model answers its
 * registers from a phase, and moves the phase on as the emulator would:
 * after a number of status reads, when the driver pauses off the device, or
 * when the owner sleeps. It counts what the driver did: the status reads,
 * the commands written while the drive was busy, and the data port reads
 * that landed in another command's transfer. A row checks what the driver
 * returned, what the model counted, and the sectors. The model also answers
 * IDENTIFY with words a row sets, for the refusal of a drive whose logical
 * sector is not 512 bytes, which QEMU cannot present.
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

/* Interrupts as the test's flag says, not the host's register. */
static int masked;
static int in_syscall;

/** Whether interrupts are on: not `masked`. */
int ata_hosttest_interrupts_on(void)
{
    return !masked;
}

/** Whether the caller is in a system call: `in_syscall`. */
int proc_in_syscall(void)
{
    return in_syscall;
}

/** Mask interrupts, and return whether they were masked. */
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

/** Restore the mask `kirq_save` returned, noting an unmasking while a DMA
 *  command waits for its engine's start. */
void kirq_restore(uint32_t flags)
{
    masked = (int)flags;
    if (!masked && dma_written_not_started) {
        unmasked_between = 1;
    }
}

static char last_record[256];
static int records;

/** The kernel log: keep the last record's format in `last_record`, and count
 *  the records. */
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

/** No option is on the boot line. */
int kcmdline_has(const char *flag)
{
    (void)flag;
    return 0;
}

/** The disk command policy allows every command. */
int diskcmd_allowed(uint8_t command)
{
    (void)command;
    return 1;
}

block_device_t *block0;

/* The scheduler runs unless a row stops it, as a shutdown does. */
static int scheduler_on = 1;

/** Whether the scheduler runs: `scheduler_on`. */
int sched_is_enabled(void)
{
    return scheduler_on;
}

/* The polls and the ticks of a sleeper's wait the driver counted. */
static uint32_t polls;
static uint32_t wait_ticks;

/** Count a tick of a sleeper's wait in `wait_ticks`. */
void block_count_wait_tick(void)
{
    wait_ticks++;
}

/** Count a command; no row reads the count. */
void block_count_command(void)
{}

/** Count a flush; no row reads the count. */
void block_count_flush(void)
{}

/** Count a poll in `polls`. */
void block_count_poll(void)
{
    polls++;
}

/** Count a batch of `sectors`; no row reads the count. */
void block_count_batch(uint32_t sectors)
{
    (void)sectors;
}

#include "../../rach2/sys/pci.h"

/** No PCI IDE controller is found. */
bool pci_find_device(uint8_t class_code, uint8_t subclass, pci_device_t *out)
{
    (void)class_code;
    (void)subclass;
    (void)out;
    return false;
}

/** Configuration space reads 0. */
uint8_t pci_read_config8(uint8_t bus, uint8_t device, uint8_t function, uint8_t offset)
{
    (void)bus;
    (void)device;
    (void)function;
    (void)offset;
    return 0;
}

/** Configuration space reads 0. */
uint32_t pci_read_config32(uint8_t bus, uint8_t device, uint8_t function, uint8_t offset)
{
    (void)bus;
    (void)device;
    (void)function;
    (void)offset;
    return 0;
}

/** Nothing to enable. */
void pci_enable_bus_master(pci_device_t *dev)
{
    (void)dev;
}

#include "../../rach2/sys/arch/x86/isr.h"

/** Register no handler. */
void register_isr_handler(uint8_t vector, isr_handler_t handler, const char *name)
{
    (void)vector;
    (void)handler;
    (void)name;
}

#include "../../rach2/sys/block/disk.h"

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

#include "../../rach2/sys/proc/wait_queue.h"

#define IO   0x1F0
#define CTRL 0x3F6
#define BM   0xC000

#define SECTORS 256u
#define SECTOR  512u

/* The drive's phase: idle, busy before or after a PIO sector, in a PIO data
   phase (DRQ), a DMA command waiting for the bus master's start or running,
   or busy with a flush. */
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
/* The task-file registers 0 to 7, and each one's value before its last write,
   which a 48-bit command reads as the high bytes. */
static uint8_t cur[8], prev[8];
static uint64_t cmd_lba;
static uint32_t sectors_left;
static int cmd_write;
static int bm_active, bm_intr;
/* The drive's volatile image, and the media, which a flush makes equal. */
static uint8_t disk[SECTORS][SECTOR];
static uint8_t durable[SECTORS][SECTOR];
static int flush_fails;
static uint32_t status_reads, refused, garbage_reads, garbage_writes, commands;
static uint32_t dma_writes, flushes, pio_write_sectors;
/* Whether interrupts were masked at the DMA command, at the engine's start,
   at the PIO command and at the last PIO sector written; -1 for not yet. */
static int masked_at_dma_cmd, masked_at_start, masked_at_pio_cmd, masked_at_last_data;
static int dma_fails;      /* the running DMA ends in ERR */
static int error_pending;  /* ERR shows in the status until the next command */
static int drive_at_flush; /* the drive register's value at the last flush */
/* What runs once while the owner sleeps, as the other process, and the
   sleeps. */
static void (*on_sleep)(void);
static int sleeps;
static uint32_t reads_at_sleep;      /* the status reads when the owner came to sleep */
static uint32_t reads_at_start;      /* the status reads when the engine was started */
static uint16_t identify_words[256]; /* what the drive answers IDENTIFY with */
static int identifying;              /* the data phase is IDENTIFY's, not a sector's */

/* The channel's bounce buffer, which the driver's DMA descriptor names by
   a physical address the host cannot follow; defined below the driver. */
static uint8_t *model_bounce(void);

/** The status register for the phase: DRDY and DSC when idle, with ERR
 *  while an error is pending; BSY in a busy phase; DRQ in a data phase. */
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

/**
 * Move a busy phase on, as the drive completes its work.
 *
 * A PIO read goes to its data phase, a PIO write to its next sector or to
 * idle, and a flush or a DMA to idle. A flush copies the volatile image to
 * the durable media unless `flush_fails`; only a flush does. A DMA moves its
 * sectors between the image and the channel's bounce buffer unless
 * `dma_fails`, and ends the bus master's transfer with its interrupt. A
 * failed flush or DMA leaves ERR pending.
 */
static void model_advance(void)
{
    uint32_t i;

    switch (phase) {
    case PIO_READ_BUSY:
        phase = PIO_READ_DRQ;
        break;
    case PIO_WRITE_BUSY:
        phase = sectors_left ? PIO_WRITE_DRQ : IDLE;
        break;
    case FLUSH_BUSY:
        if (flush_fails) {
            error_pending = 1;
        } else {
            k_memcpy(durable, disk, sizeof(durable));
        }
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

/**
 * Read port `port` of the model.
 *
 * A status read is counted, and after `answer_after` reads in a busy phase,
 * or in a running DMA when `dma_on_reads`, it moves the phase on; with -1 it
 * never does. The alternate status reads the status uncounted, the bus
 * master's status gives its active and interrupt bits, and a task-file
 * register reads back what was written.
 */
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

/**
 * Take `command`, written to the command register.
 *
 * A command written while BSY or DRQ is set is refused and counted in
 * `refused`. Any other clears a pending error, takes its count and LBA from
 * the task-file registers, and enters its phase; a command the model does
 * not know fails the test.
 */
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
        flushes++;
    }
    if (command == 0xCA || command == 0x35) {
        dma_writes++;
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
    case 0xEC: /* IDENTIFY DEVICE: one block of words, read as a sector is */
        sectors_left = 1;
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
    case 0xEC:
        cmd_write = 0;
        phase = PIO_READ_BUSY;
        identifying = 1;
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

/**
 * Write `value` to port `port` of the model.
 *
 * The command register takes a command, and a task-file register keeps its
 * last two values. Bit 0 of the bus master's command starts a waiting DMA,
 * and clearing it stops the engine. Bit 2 written to the bus master's
 * status clears its interrupt.
 */
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

/** Read `words` from the data port: in a PIO read's data phase the next
 *  sector or the IDENTIFY words, and otherwise 0xEE bytes, counted in
 *  `garbage_reads`. */
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
    if (identifying) {
        k_memcpy(out, identify_words, words * 2);
        identifying = 0;
        sectors_left = 0;
        phase = IDLE;
        reads_in_phase = 0;
        return;
    }
    k_memcpy(out, disk[cmd_lba % SECTORS], words * 2);
    cmd_lba++;
    sectors_left--;
    phase = sectors_left ? PIO_READ_BUSY : IDLE;
    reads_in_phase = 0;
}

/** Write `words` to the data port: in a PIO write's data phase the next
 *  sector, after which the drive is busy, and otherwise nothing, counted in
 *  `garbage_writes`. */
static void model_outs16(uint16_t port, const void *buffer, uint32_t words)
{
    if (port != IO || phase != PIO_WRITE_DRQ) {
        garbage_writes++;
        return;
    }
    k_memcpy(disk[cmd_lba % SECTORS], buffer, words * 2);
    pio_write_sectors++;
    cmd_lba++;
    sectors_left--;
    if (sectors_left == 0) {
        masked_at_last_data = masked;
    }
    phase = PIO_WRITE_BUSY;
    reads_in_phase = 0;
}

/** A 32-bit port write, which the model drops. */
static void model_out32(uint16_t port, uint32_t value)
{
    (void)port;
    (void)value;
}

/* The port interface the driver is given by `ata_set_io`. */
static const struct ata_io model_io = {model_in8, model_out8, model_out32, model_ins16,
                                       model_outs16};

/** The machine's byte read, the driver's default, which no row reaches. */
uint8_t inb(uint16_t port)
{
    (void)port;
    return 0;
}

/** The machine's byte write, which no row reaches. */
void outb(uint16_t port, uint8_t value)
{
    (void)port;
    (void)value;
}

/** The machine's 32-bit write, which no row reaches. */
void outd(uint16_t port, uint32_t value)
{
    (void)port;
    (void)value;
}

/** The machine's string read, which no row reaches. */
void insw(uint16_t port, void *buffer, uint32_t words)
{
    (void)port;
    (void)buffer;
    (void)words;
}

/** The machine's string write, which no row reaches. */
void outsw(uint16_t port, const void *buffer, uint32_t words)
{
    (void)port;
    (void)buffer;
    (void)words;
}

/** The driver's pause off the device, where the emulator's main loop runs:
 *  the pause numbered `complete_on_pause` completes a running DMA. */
void ata_hosttest_paused(void)
{
    pauses++;
    if (complete_on_pause > 0 && pauses == complete_on_pause && phase == DMA_RUN) {
        model_advance();
    }
}

/**
 * The owner's sleep: `on_sleep` runs once, as the other process, and the
 * emulator then completes what is pending until `cond` holds.
 *
 * Records the status reads at the sleep in `reads_at_sleep`, and fails the
 * test when `cond` does not hold within 1000 steps.
 */
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

/** Wake nobody: the sleep above ends by itself. */
void wq_wake_all(wait_queue_t *wq)
{
    (void)wq;
}

/** Nothing to set up. */
void wait_queue_init(wait_queue_t *wq)
{
    (void)wq;
}

/** No one waits. */
int wq_active(const wait_queue_t *wq)
{
    (void)wq;
    return 0;
}

#include "../../rach2/sys/block/ata.c"

/** The channel's bounce buffer. */
static uint8_t *model_bounce(void)
{
    return channels[0].bounce;
}

/** Reset the model to an idle drive whose sectors hold their pattern on the
 *  media too, a busy phase lasting `answer` status reads (-1 for ever), and
 *  the caller in a system call with interrupts on. */
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
    dma_writes = flushes = pio_write_sectors = 0;
    masked_at_dma_cmd = masked_at_start = masked_at_pio_cmd = masked_at_last_data = -1;
    dma_fails = 0;
    flush_fails = 0;
    error_pending = 0;
    drive_at_flush = -1;
    scheduler_on = 1;
    wait_ticks = 0;
    dma_written_not_started = 0;
    unmasked_between = 0;
    on_sleep = 0;
    sleeps = 0;
    identifying = 0;
    records = 0;
    last_record[0] = '\0';
    masked = 0;
    in_syscall = 1;
    for (s = 0; s < SECTORS; s++) {
        for (i = 0; i < SECTOR; i++) {
            disk[s][i] = (uint8_t)(s * 7 + i);
        }
    }
    k_memcpy(durable, disk, sizeof(durable));
}

/** Return 1 when `got` holds sector `lba`'s pattern, byte i being
 *  lba * 7 + i, and 0 otherwise. */
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

/** Return 1 when `needle` occurs in `text`, and 0 otherwise. */
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

/** The other process: a page fault's read of sector 200 on the channel,
 *  with interrupts off and no way to sleep, while the owner's DMA is in
 *  flight; the DMA completes at the third pause from here. */
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

/** The tick, twice, while the owner sleeps. */
static void two_ticks_while_asleep(void)
{
    ata_tick();
    ata_tick();
}

/** Ten seconds of ticks while the owner sleeps, which a loaded host can
 *  take for one write. */
static void ten_seconds_while_asleep(void)
{
    int i;

    for (i = 0; i < 1000; i++) {
        ata_tick();
    }
}

/** One tick past thirty seconds while the owner sleeps. */
static void thirty_seconds_while_asleep(void)
{
    int i;

    for (i = 0; i < 3001; i++) {
        ata_tick();
    }
}

/**
 * Check the sleeping wait's bound of thirty seconds of ticks: a DMA read
 * the drive ends after ten seconds is its answer, and one still running a
 * tick past thirty seconds is given up with `-EIO` and a record.
 */
static void wait_bound_rows(struct ata_drive *d)
{
    int rc;

    model_reset(2);
    dma_on_reads = 0;
    on_sleep = ten_seconds_while_asleep;
    rc = ata_read(&d->dev, 100, 8, owner_buf);
    CHECK(rc == 0 && sector_is(owner_buf, 100) && records == 0,
          "a DMA read the drive ends after ten seconds returns its sectors");
    model_reset(-1);
    dma_on_reads = 0;
    on_sleep = thirty_seconds_while_asleep;
    rc = ata_read(&d->dev, 100, 8, owner_buf);
    CHECK(rc == -EIO && records == 1 && contains(last_record, "no answer in"),
          "a DMA read still running after thirty seconds fails with a record");
}

/**
 * Check DMA writes of 512 bytes, 4 KiB, 32 KiB and across the bounce
 * buffer's end, in both address forms, LBA28 and LBA48.
 *
 * A DMA completes only while the caller sleeps, and a busy phase, a PIO
 * sector or a flush, after 512 status reads: a delayed completion modelled
 * without judging host time. The drive is set up as a DMA drive here;
 * finding the controller and taking real interrupts need a guest boot.
 */
static void write_rate_rows(struct ata_drive *d)
{
    static const uint32_t sizes[] = {1, 8, 64, 72};
    static uint8_t data[72 * SECTOR];
    uint32_t row, high, i, j, count, chunks;
    uint64_t lba;
    int rc;

    for (i = 0; i < sizeof(data); i++) {
        data[i] = (uint8_t)(i * 13 + i / SECTOR + 3);
    }
    for (high = 0; high < 2; high++) {
        lba = high ? ATA_LBA28_SECTORS + 32 : 32;
        d->dev.sector_count = lba + SECTORS;
        for (row = 0; row < sizeof(sizes) / sizeof(sizes[0]); row++) {
            model_reset(512);
            dma_on_reads = 0;
            count = sizes[row];
            chunks = (count + 63) / 64;
            rc = ata_write(&d->dev, lba, count, data);
            CHECK(rc == 0, "a delayed write completes");
            for (i = 0; i < count; i++) {
                for (j = 0; j < SECTOR && disk[32 + i][j] == data[i * SECTOR + j]; j++) {
                }
                CHECK(j == SECTOR, "every written byte reaches its sector");
            }
            CHECK(sector_is(disk[31], 31) && sector_is(disk[32 + count], 32 + count),
                  "a write leaves its neighbouring sectors unchanged");
            CHECK(dma_writes == chunks && pio_write_sectors == 0,
                  "a DMA write uses one data command per 32 KiB and no data-port sectors");
            CHECK(commands == 2 * chunks && flushes == chunks,
                  "each DMA write command has one cache flush");
            CHECK(sleeps == (int)(2 * chunks), "a delayed write and its flush each sleep");
            CHECK(status_reads <= 32 * chunks,
                  "a delayed write and flush use at most 32 status reads per 32 KiB");
            CHECK(refused == 0 && garbage_writes == 0 && channels[0].owner == 0,
                  "the write completes without a conflicting command and releases its channel");
            printf("ata-test: write: LBA%u, %u sectors, %u DMA, %u flushes, "
                   "%u PIO sectors, %u status reads, %d sleeps\n",
                   high ? 48u : 28u, count, dma_writes, flushes, pio_write_sectors, status_reads,
                   sleeps);
        }
    }
    d->dev.sector_count = SECTORS;
}

/** Return 1 when the `count` bytes at `a` and at `b` are equal. */
static int bytes_equal(const uint8_t *a, const uint8_t *b, uint32_t count)
{
    uint32_t i;

    for (i = 0; i < count; i++) {
        if (a[i] != b[i]) {
            return 0;
        }
    }
    return 1;
}

/**
 * Check that a write that returns success is on the media, so that it
 * survives the loss of the drive's volatile cache.
 *
 * Path 0 is a PIO write by a caller that takes the channel, 1 a sleeping
 * DMA write, and 2 a polled write with interrupts off while the channel is
 * owned; each runs in both address forms. A failed flush fails the write,
 * after the whole data transfer, and leaves the media as they were.
 */
static void durability_rows(struct ata_drive *d)
{
    static uint8_t data[8 * SECTOR], readback[8 * SECTOR];
    uint64_t lba;
    uint32_t high, path, i;
    int rc;

    for (i = 0; i < sizeof(data); i++) {
        data[i] = (uint8_t)(i * 11 + i / SECTOR + 17);
    }
    for (high = 0; high < 2; high++) {
        lba = high ? ATA_LBA28_SECTORS + 32 : 32;
        d->lba48 = (int)high;
        d->dev.sector_count = lba + SECTORS;
        for (path = 0; path < 3; path++) {
            model_reset(32);
            d->dma = path != 0;
            channels[0].owner = path == 2;
            masked = path == 2;
            in_syscall = path != 2;
            dma_on_reads = 0;
            rc = ata_write(&d->dev, lba, 8, data);
            CHECK(rc == 0, "a write with a completed flush succeeds");
            CHECK(bytes_equal(durable[32], data, sizeof(data)),
                  "a returned write is durable before power loss");
            CHECK(phase == IDLE, "a returned write has no pending flush");
            CHECK(sector_is(durable[31], 31) && sector_is(durable[40], 40),
                  "a durable write preserves neighbouring sectors");
            k_memcpy(disk, durable, sizeof(disk));
            rc = ata_read(&d->dev, lba, 8, readback);
            CHECK(rc == 0 && bytes_equal(readback, data, sizeof(data)),
                  "a returned write survives volatile cache loss");

            model_reset(32);
            channels[0].owner = path == 2;
            masked = path == 2;
            in_syscall = path != 2;
            dma_on_reads = 0;
            flush_fails = 1;
            rc = ata_write(&d->dev, lba, 8, data);
            CHECK(rc == -EIO, "a failed flush fails the write");
            CHECK(bytes_equal(disk[32], data, sizeof(data)),
                  "a failed flush follows the complete data transfer");
            CHECK(sector_is(durable[32], 32) && sector_is(durable[39], 39),
                  "a failed flush does not update durable media");
            printf("ata-test: durability: LBA%u path %u, success and flush failure checked\n",
                   high ? 48u : 28u, path);
        }
    }
    channels[0].owner = 0;
    d->dma = 1;
    d->lba48 = 1;
    d->dev.sector_count = SECTORS;
}

/**
 * Check IDENTIFY and the logical sector size.
 *
 * Word 106 with its valid bit, bit 14, and bit 12 set, and words 117 and 118
 * giving 2048 words a sector, is a drive whose logical sector is 4096 bytes,
 * which the block layer, counting in 512-byte sectors, cannot serve: it is
 * refused by name. The same words with bit 12 clear are a 512-byte drive,
 * taken with the 48-bit count of words 100 to 103, and bit 12 without the
 * valid bit says nothing of the sector. The AHCI driver's refusal of the
 * same word is ahci_hosttest's; tests/host/ata_identify_mutate.sh drops this
 * driver's.
 */
static void identify_rows(void)
{
    static uint16_t words[256];
    struct ata_drive probe;
    int rc;

    k_memset(&probe, 0, sizeof(probe));
    probe.channel = &channels[0];
    k_memset(identify_words, 0, sizeof(identify_words));
    identify_words[83] = 0x0400u;  /* the 48-bit commands */
    identify_words[100] = 0x1000u; /* 4096 sectors */
    identify_words[117] = 0x0800u; /* 2048 words a logical sector */
    identify_words[118] = 0;

    model_reset(2);
    in_syscall = 0;
    identify_words[106] = 0x5000u;
    rc = ata_identify(&probe, words);
    CHECK(rc == -1, "IDENTIFY with a 4096-byte logical sector is refused");
    CHECK(records == 1 && contains(last_record, "the logical sector is not 512 bytes"),
          "the refusal of a 4096-byte logical sector is recorded with its reason");
    CHECK(refused == 0 && garbage_reads == 0 && phase == IDLE,
          "the IDENTIFY data was read whole, as the drive gave it");

    model_reset(2);
    in_syscall = 0;
    identify_words[106] = 0x4000u;
    rc = ata_identify(&probe, words);
    CHECK(rc == 0 && probe.lba48 && probe.sectors == 0x1000u,
          "the same drive with 512-byte sectors is taken, with the count of words 100 to 103");

    model_reset(2);
    in_syscall = 0;
    identify_words[106] = 0x1000u;
    rc = ata_identify(&probe, words);
    CHECK(rc == 0 && probe.sectors == 0x1000u,
          "bit 12 of word 106 without its valid bit says nothing of the sector");
}

/**
 * Set the primary channel's master up on the model as a DMA drive of 256
 * sectors, run the rows, and return 1 when any check failed.
 *
 * The rows, in order:
 *
 * 1. The owner's DMA read reads the status eight times after the engine's
 *    start and then sleeps, and the emulator completes it in the sleep; the
 *    command and the engine's start are written with interrupts off.
 * 2. A caller that cannot sleep reads an idle channel through the data port,
 *    with interrupts off throughout.
 * 3. The two paths on one channel: with the owner's DMA in flight and the
 *    drive showing DRQ, a polled read arrives. The emulator completes the
 *    DMA only at the third pause off the device, as on a loaded host; the
 *    polled read waits for the drive to go idle and reads its own sector,
 *    and the DMA completes with its own.
 * 4. A drive that never answers: the polled wait's bound in status reads,
 *    and the record it leaves.
 * 5. A data-port write by the owner of a drive without DMA runs with
 *    interrupts off from its command to its last sector.
 * 6. The owner's DMA fails, and a polled read runs inside its sleep, whose
 *    own command clears the drive's error bits; the owner's answer is still
 *    the failure.
 * 7. A write's flush selects its own drive again: the writer is the slave,
 *    and the polled read inside its sleep selected the master.
 * 8. With the scheduler stopped, as at a shutdown, a caller in a system call
 *    polls rather than sleep on a channel it might wait on for ever.
 * 9. The tick counts a sleeper's wait: two ticks while the owner sleeps are
 *    two in `disk-wait-ticks`.
 *
 * Then the rows of IDENTIFY, of the sleeping wait's bound, of the write
 * rate and of durability.
 */
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

    model_reset(2);
    masked = 1;
    in_syscall = 0;
    rc = ata_read(&d->dev, 5, 1, polled_buf);
    CHECK(rc == 0 && sector_is(polled_buf, 5), "a polled read of an idle drive returns its sector");
    CHECK(sleeps == 0, "a polled read does not sleep");
    CHECK(refused == 0 && garbage_reads == 0, "a polled read of an idle drive is answered");

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

    model_reset(2);
    dma_on_reads = 0;
    dma_fails = 1;
    on_sleep = polled_read_while_owner_sleeps;
    rc = ata_read(&d->dev, 100, 8, owner_buf);
    CHECK(rc == -EIO, "a failed DMA is reported failed although a polled command followed it");
    CHECK(polled_rc == 0 && sector_is(polled_buf, 200),
          "and the polled read inside it still succeeds");

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

    model_reset(2);
    scheduler_on = 0;
    rc = ata_read(&d->dev, 5, 1, polled_buf);
    CHECK(rc == 0 && sector_is(polled_buf, 5),
          "a read with the scheduler stopped returns its sector");
    CHECK(sleeps == 0, "and did not sleep");

    model_reset(2);
    dma_on_reads = 0;
    on_sleep = two_ticks_while_asleep;
    rc = ata_read(&d->dev, 100, 8, owner_buf);
    CHECK(rc == 0, "the owner's DMA read returns 0 with two ticks inside its sleep");
    CHECK(wait_ticks == 2, "the two ticks the owner slept through are counted");

    identify_rows();

    wait_bound_rows(d);
    write_rate_rows(d);
    durability_rows(d);

    if (fails) {
        printf("ata-test: %d failure(s)\n", fails);
        return 1;
    }
    printf("ata-test: OK\n");
    return 0;
}
