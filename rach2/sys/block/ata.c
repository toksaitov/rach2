/*
 * ata.c: the legacy ATA driver: the two channels of an IDE controller, each
 * with a master and a slave, driven through the controller's bus master where
 * there is one and through the data port where there is not.
 *
 * A channel is a pair of port ranges, the command block and the control
 * block, at the legacy addresses or where a controller in native mode says
 * in its PCI base registers. A drive is selected by one bit of the drive
 * register, and every command is issued to the drive selected. Each drive
 * that answers IDENTIFY with the disk signature is published as a block
 * device and registered with the disk registry; the first found becomes
 * `block0`, unless another driver has already published one. Transfers are
 * `READ DMA` and `WRITE DMA` through the controller's bus master, one
 * command and one completion per request, where the controller has a bus
 * master and the drive answers IDENTIFY with DMA; `READ SECTORS` and
 * `WRITE SECTORS` through the data port, a sector a time, where either is
 * missing. Both are in LBA28 when the sectors lie below 2^28 and in their
 * 48-bit forms above, on a drive that has them; a write is followed by a
 * cache flush.
 *
 * The bus master is the PIIX form: a physical region descriptor giving the
 * transfer's memory, its address in one register and its start bit in
 * another, and the drive's interrupt at the end. The memory is the channel's
 * own bounce buffer, one request's worth, aligned so it lies in one 64 KiB
 * region as the bus master requires, and static, in the kernel image, so it
 * is physically contiguous and below 4 GiB. A caller's buffer is copied into
 * it before a write and out of it after a read, in the caller's own context:
 * a fault-safe device's transfer may not let another process run against the
 * caller's memory, since the page cache's window may evict or remap that
 * memory meanwhile, and a sleeping transfer straight into it would move
 * whatever page is there when the engine runs. The processor's cache is
 * coherent with this DMA, so nothing is flushed. The data port costs a
 * completion a sector, each a round trip through the hypervisor when there
 * is one; the bus master costs one completion a request.
 *
 * A channel takes one command at a time. A caller that may sleep owns the
 * channel from its first register write to its last, sleeping on the
 * channel's queue while another owns it; two sleepers on one channel would
 * otherwise describe their buffers to one table and stop each other's
 * engine. A page fault, with interrupts off, cannot wait for an owner that
 * cannot run, so it waits for the drive alone and moves its sectors
 * through the data port, which touches no register of the bus master's:
 * the owner's command had ended before the drive went idle, and the owner
 * finds the drive idle again when it wakes.
 *
 * A wait for the drive sleeps when it can and polls when it must. From a
 * system call, with interrupts on, the caller reads the status register a
 * few times and then sleeps on the channel's queue, woken by the channel's
 * interrupt, which the drive raises when a sector is ready or a command is
 * done, and by the tick, which bounds a lost interrupt and ends a wait
 * that has lasted `ATA_WAIT_TICKS`. From a page fault, with interrupts off
 * and no timer to bound by, the wait polls and counts its reads. Under a
 * hypervisor every port read is an exit, which is why a wait that may sleep
 * reads the status only a few times first; the status reads and the ticks
 * waited are counted for `/proc/disk`.
 */

#include "../arch/x86/portio.h"
#include "../cmdline.h"
#include "../klog.h"
#include "block.h"
#include "ata.h"
#include "diskcmd.h"
#include "disk.h"

#include "../kio.h"
#include "../irq.h"
#include "../lib/kirq.h"
#include "../lib/kstring.h"
#include "../pci.h"
#include "../proc/process.h"
#include "../proc/sched.h"
#include "../proc/wait_queue.h"
#include "../serial.h"

#include "../lib/kerrno.h"
#include "../lib/kint.h"

/* The most one request to the disk may move: 32 KiB, which is also the
 * bounce buffer's size, and which PIO moves a sector at a time through the
 * data port with interrupts off. A request is kept short here, where the AHCI
 * disk takes the block layer's whole limit. */
#define ATA_REQ_SECTORS 64u

/* Command-block registers, as offsets from the channel's command base. */
#define ATA_REG_DATA     0 /* 16-bit data port */
#define ATA_REG_SECCOUNT 2 /* sector count (0 means 256) */
#define ATA_REG_LBA_LOW  3 /* LBA bits 0-7 */
#define ATA_REG_LBA_MID  4 /* LBA bits 8-15 */
#define ATA_REG_LBA_HIGH 5 /* LBA bits 16-23 */
#define ATA_REG_DRIVE    6 /* drive select + LBA bits 24-27 */
#define ATA_REG_STATUS   7 /* status (read) / command (write) */

/* Status register bits. */
#define ATA_SR_BSY 0x80 /* busy */
#define ATA_SR_DF  0x20 /* device fault */
#define ATA_SR_DRQ 0x08 /* data request ready */
#define ATA_SR_ERR 0x01 /* error */

/* The commands this driver issues. */
#define ATA_CMD_READ          0x20
#define ATA_CMD_READ_EXT      0x24
#define ATA_CMD_WRITE         0x30
#define ATA_CMD_WRITE_EXT     0x34
#define ATA_CMD_READ_DMA      0xC8
#define ATA_CMD_READ_DMA_EXT  0x25
#define ATA_CMD_WRITE_DMA     0xCA
#define ATA_CMD_WRITE_DMA_EXT 0x35

/* The bus master's registers, from its base in the controller's fifth
   base address register: the primary channel's at 0, the secondary's at 8. */
#define ATA_BM_CMD            0 /* start, and the direction: 1 reads the disk into memory */
#define ATA_BM_STATUS         2 /* active, error, interrupt; the last two clear when written */
#define ATA_BM_PRD            4 /* the descriptor table's physical address */
#define ATA_BM_START          0x01
#define ATA_BM_READ           0x08
#define ATA_BM_ERROR          0x02
#define ATA_BM_INTR           0x04
#define ATA_BM_SECONDARY      8
#define ATA_PROGIF_BUS_MASTER 0x80

/* IDENTIFY word 49 bit 8: the drive does DMA. */
#define ATA_ID_CAP_DMA 0x0100u

/* A physical region descriptor: the bounce buffer's address, the byte
   count, 0 meaning 64 KiB, and the end flag. One describes a request. */
struct ata_prd {
    uint32_t phys;
    uint16_t bytes;
    uint16_t flags;
};
#define ATA_PRD_EOT       0x8000
#define ATA_BOUNCE_BYTES  (ATA_REQ_SECTORS * BLOCK_SECTOR_SIZE)
#define ATA_CMD_FLUSH     0xE7
#define ATA_CMD_FLUSH_EXT 0xEA
#define ATA_CMD_IDENTIFY  0xEC

/* The drive register: bit 4 selects the slave, bit 6 LBA addressing, and
   in LBA28 the low four bits are LBA bits 24-27. */
#define ATA_DRIVE_SELECT 0xA0
#define ATA_DRIVE_LBA    0x40
#define ATA_DRIVE_SLAVE  0x10

/* The machine's ports, or what a test put in their place (`ata_set_io`). */
static const struct ata_io ata_port_io = {inb, outb, outd, insw, outsw};
static const struct ata_io *ata_io = &ata_port_io;

void ata_set_io(const struct ata_io *ops)
{
    ata_io = ops ? ops : &ata_port_io;
}

/* One command moves at most 256 sectors: the legacy count register is
   8-bit, with 0 meaning 256, and the 48-bit form is kept to the same. */
#define ATA_MAX_SECTORS_PER_CMD 256
#define ATA_WORDS_PER_SECTOR    (BLOCK_SECTOR_SIZE / 2)

/* The largest LBA the 28-bit commands address, plus one. */
#define ATA_LBA28_SECTORS 0x10000000ull

/* A polled wait reads the status this many times plainly, which is what
   the emulator's DMA needs, and then in rounds of a pause off the device
   and a read; the bound is the number of rounds. With interrupts off
   there is no timer to bound by, so the watchdog counts rounds instead of
   time: a round's pause is ten thousand `pause` instructions, a few tens
   of microseconds at the least on the processors this kernel runs on, so
   the bound is at least five seconds. The pause is what lets an emulator
   answer: under a hypervisor every status read is an exit that takes the
   emulator's lock, and a loop of nothing but reads can keep the emulator's
   own completion out for as long as the loop runs. */
#define ATA_POLL_PLAIN  256
#define ATA_PAUSE_LOOPS 10000
#define ATA_POLL_LIMIT  200000

/* How many times a wait that may sleep reads the status first: the
   first sector of a write is ready within these without an interrupt,
   and a DMA command is not, on the emulator or the hardware, so the rest
   of the wait is the sleep. Each read is an exit under a hypervisor. */
#define ATA_SPIN_BEFORE_SLEEP 8

/* A sleeping wait's bound in ticks, thirty seconds at 100 Hz, the AHCI and
   NVMe drivers' bound with interrupts on and Linux's ATA command timeout:
   an emulator completes a write through its host's file, and on a loaded
   host one write can take longer than five seconds. */
#define ATA_WAIT_TICKS 3000

/* The channels' interrupt lines and the vectors the router gives them. */
#define ATA_IRQ_PRIMARY   14
#define ATA_IRQ_SECONDARY 15
#define ATA_VECTOR_BASE   32

/* The IDE controller class, and the prog-if bits that say a channel is in
   native mode, with its ports in the base address registers. */
#define PCI_CLASS_STORAGE           0x01
#define PCI_SUBCLASS_IDE            0x01
#define ATA_PROGIF_PRIMARY_NATIVE   0x01
#define ATA_PROGIF_SECONDARY_NATIVE 0x04

struct ata_channel {
    uint16_t io;   /* the command block */
    uint16_t ctrl; /* the control block: alternate status and device control */
    const char *name;
    uint8_t irq;
    wait_queue_t wq;         /* sleepers waiting for the drive's status */
    volatile uint32_t ticks; /* counted by ata_tick, the sleeping wait's clock */
    uint32_t irqs;           /* interrupts taken on this channel */
    int owner;               /* 1 while a caller owns the channel */
    int sleepers;            /* callers asleep for a command, for the tick's count */
    int error_seen; /* a caller without the channel found ERR or DF, which are the owner's */
    uint16_t bm;    /* the bus master's first port, or 0 for none */
    /* The descriptor, aligned to its size, and the bounce buffer, aligned
       to its own so it lies in one 64 KiB region; both in the image's .bss,
       below 4 GiB, and the controller is given `virt_to_phys` of each. */
    struct ata_prd prd __attribute__((aligned(8)));
    uint8_t bounce[ATA_BOUNCE_BYTES] __attribute__((aligned(ATA_BOUNCE_BYTES)));
};

/* A sleeping wait: the channel, what the status must show (`ATA_WANT_`),
   the channel's `ticks` when the wait began, and its answer, 0 or `-EIO`. */
struct ata_wait {
    struct ata_channel *ch;
    int want;
    uint32_t t0;
    int rc;
};

/* What a wait is for: the drive not busy; ready, not busy with no data
   pending; with a sector to move; or idle, with no command in any phase
   (`ata_status_done`). */
#define ATA_WANT_NOT_BUSY 0
#define ATA_WANT_READY    1
#define ATA_WANT_DATA     2
#define ATA_WANT_IDLE     3

struct ata_drive {
    struct ata_channel *channel;
    int slave;
    int lba48;
    int dma; /* the drive does DMA and its channel has a bus master */
    uint64_t sectors;
    int present;
    block_device_t dev;
};

static struct ata_channel channels[2] = {
    {0x1F0, 0x3F6, "primary", ATA_IRQ_PRIMARY, {0}, 0, 0, 0, 0, 0, 0, {0, 0, 0}, {0}},
    {0x170, 0x376, "secondary", ATA_IRQ_SECONDARY, {0}, 0, 0, 0, 0, 0, 0, {0, 0, 0}, {0}}};
static struct ata_drive drives[4];

static int ata_read(block_device_t *device, uint64_t lba, uint32_t count, void *buffer);
static int ata_write(block_device_t *device, uint64_t lba, uint32_t count, const void *buffer);

/**
 * Issue `command` to the drive selected on `ch` when the disk policy allows
 * it, and count it for `/proc/disk`. Returns 1 when it was issued, 0 when the
 * policy does not allow it.
 */
static int ata_command(const struct ata_channel *ch, uint8_t command)
{
    if (!diskcmd_allowed(command)) {
        klog(KLOG_WARN, "ata", "refused: command 0x%X: %s", (unsigned)command,
             diskcmd_policy_banner);
        return 0;
    }
    block_count_command();
    if (command == ATA_CMD_FLUSH || command == ATA_CMD_FLUSH_EXT) {
        block_count_flush();
    }
    ata_io->out8(ch->io + ATA_REG_STATUS, command);
    return 1;
}

/** The 400 ns a drive needs after a select or a command before its status
 * means anything: four reads of the alternate status register. */
static void ata_settle(const struct ata_channel *ch)
{
    ata_io->in8(ch->ctrl);
    ata_io->in8(ch->ctrl);
    ata_io->in8(ch->ctrl);
    ata_io->in8(ch->ctrl);
}

/**
 * Read the status once and say whether a wait for `want` is over. Returns
 * 1 with `*rc` set, 0 or `-EIO`, when it is, and 0 while the drive is
 * still busy or, for a wait for data, has none yet.
 *
 * A wait for idle ends only with no command in any phase: a drive in a data
 * phase, PIO or DMA, shows DRQ with BSY clear, and a command written to it
 * then is ignored or, worse, answered with the other command's data. The
 * error bits an idle drive shows are the owner's command's, so they do not
 * fail that wait; they are noted in `error_seen` for the owner, since the
 * command written next clears them.
 */
static int ata_status_done(struct ata_channel *ch, int want, int *rc)
{
    uint8_t status = ata_io->in8(ch->io + ATA_REG_STATUS);

    block_count_poll();
    if (status & ATA_SR_BSY) {
        return 0;
    }
    if (want == ATA_WANT_NOT_BUSY) {
        *rc = 0;
        return 1;
    }
    if (want == ATA_WANT_IDLE) {
        if (status & ATA_SR_DRQ) {
            return 0;
        }
        if (status & (ATA_SR_ERR | ATA_SR_DF)) {
            ch->error_seen = 1;
        }
        *rc = 0;
        return 1;
    }
    if (status & (ATA_SR_ERR | ATA_SR_DF)) {
        *rc = -EIO;
        return 1;
    }
    if (want == ATA_WANT_DATA) {
        if (status & ATA_SR_DRQ) {
            *rc = 0;
            return 1;
        }
        return 0;
    }
    if (status & ATA_SR_DRQ) {
        return 0;
    }
    *rc = 0;
    return 1;
}

/* Whether interrupts are on: the processor's answer, or a host test's,
   whose `kirq_save` is a stand-in that reads no register. */
#ifdef ATA_HOSTTEST
int ata_hosttest_interrupts_on(void);
#define ata_interrupts_on ata_hosttest_interrupts_on
#else
#define ata_interrupts_on kirq_enabled
#endif

/**
 * Whether a wait here may sleep: interrupts on, inside a system call, and
 * the scheduler running. A shutdown stops the scheduler before it flushes
 * the root, and a wait then for another process, the channel's owner or
 * the tick, would never end, since nothing runs it.
 */
static int ata_can_sleep(void)
{
    return ata_interrupts_on() && proc_in_syscall() && sched_is_enabled();
}

/** The condition a caller waiting for the channel waits on: no owner. */
static int ata_free(void *arg)
{
    return ((struct ata_channel *)arg)->owner == 0;
}

/**
 * Take the channel for one command sequence. A caller that can sleep waits
 * on the channel's queue until no one owns it. Returns 1 with the channel
 * owned, or 0 when another owns it still, as it may for a caller that
 * cannot sleep, who then waits for the drive alone and uses the data port.
 */
static int ata_acquire(struct ata_channel *ch)
{
    uint32_t flags;

    if (ata_can_sleep()) {
        wq_wait_event_nointr(&ch->wq, ata_free, ch);
    }
    flags = kirq_save();
    if (ch->owner) {
        kirq_restore(flags);
        return 0;
    }
    ch->owner = 1;
    kirq_restore(flags);
    return 1;
}

/** Give up the channel and wake the callers waiting for it. */
static void ata_release(struct ata_channel *ch)
{
    ch->owner = 0;
    wq_wake_all(&ch->wq);
}

/** The sleeping wait's condition: the status says the wait is over, or the
 * tick says it has lasted long enough to be a drive that never answers. */
static int ata_wait_cond(void *arg)
{
    struct ata_wait *w = arg;

    if (ata_status_done(w->ch, w->want, &w->rc)) {
        return 1;
    }
    if (w->ch->ticks - w->t0 > ATA_WAIT_TICKS) {
        klog(KLOG_WARN, "ata", "%s: no answer in %d ticks: status 0x%X, bus master 0x%X, want %d",
             w->ch->name, ATA_WAIT_TICKS, (unsigned)ata_io->in8(w->ch->io + ATA_REG_STATUS),
             w->ch->bm ? (unsigned)ata_io->in8(w->ch->bm + ATA_BM_STATUS) : 0u, w->want);
        w->rc = -EIO;
        return 1;
    }
    return 0;
}

/**
 * Stay off the device for a while, without touching a timer either:
 * `ATA_PAUSE_LOOPS` `pause` instructions. A host test, whose host may not be
 * an x86, gives `ata_hosttest_paused` instead, its model of an emulator whose
 * completion arrives while the driver is off the device.
 */
static void ata_pause(void)
{
#ifdef ATA_HOSTTEST
    void ata_hosttest_paused(void);

    ata_hosttest_paused();
#else
    uint32_t n;

    for (n = 0; n < ATA_PAUSE_LOOPS; n++) {
        __asm__ __volatile__("pause");
    }
#endif
}

/**
 * Wait until the drive's status shows `want`; an error, or a drive that
 * never answers, is `-EIO`. A few polls first, then a sleep on the
 * channel's queue where sleeping is allowed and a polled wait where it is
 * not, whose reads past the first few hundred are each preceded by a
 * pause off the device. A polled wait that gives up says so, since the
 * caller's answer to it may be a panic with no record of its own.
 */
static int ata_wait_for(struct ata_channel *ch, int want)
{
    struct ata_wait w;
    int spin, rc;

    for (spin = 0; spin < ATA_SPIN_BEFORE_SLEEP; spin++) {
        if (ata_status_done(ch, want, &rc)) {
            return rc;
        }
    }
    if (!ata_can_sleep()) {
        for (spin = 0; spin < ATA_POLL_PLAIN; spin++) {
            if (ata_status_done(ch, want, &rc)) {
                return rc;
            }
        }
        for (spin = 0; spin < ATA_POLL_LIMIT; spin++) {
            ata_pause();
            if (ata_status_done(ch, want, &rc)) {
                return rc;
            }
        }
        klog(KLOG_ERR, "ata",
             "%s: no answer to a polled wait after %d reads: status 0x%X, bus master 0x%X, want %d",
             ch->name, ATA_POLL_PLAIN + ATA_POLL_LIMIT,
             (unsigned)ata_io->in8(ch->io + ATA_REG_STATUS),
             ch->bm ? (unsigned)ata_io->in8(ch->bm + ATA_BM_STATUS) : 0u, want);
        return -EIO;
    }
    w.ch = ch;
    w.want = want;
    w.t0 = ch->ticks;
    w.rc = -EIO;
    ch->sleepers++;
    wq_wait_event_nointr(&ch->wq, ata_wait_cond, &w);
    ch->sleepers--;
    return w.rc;
}

/** Wait until the drive is not busy; only a drive that never answers is
 * `-EIO`. */
static int ata_wait_not_busy(struct ata_channel *ch)
{
    return ata_wait_for(ch, ATA_WANT_NOT_BUSY);
}

/**
 * Wait until the drive has no command in any phase. A timeout is `-EIO`.
 *
 * This is the wait before a command from a caller that does not own the
 * channel: the owner's command may be in flight, and its data phase, DRQ
 * with BSY clear, is where a second command is ignored or answered with
 * the first one's data. The owner's DMA is issued and started with
 * interrupts off, so a caller here sees it either not yet issued or
 * running, never issued and waiting for its engine.
 *
 * An idle drive showing ERR or DF ends this wait with success: those bits
 * belong to the owner's command, and they are recorded for it rather than
 * answered here.
 */
static int ata_wait_idle(struct ata_channel *ch)
{
    return ata_wait_for(ch, ATA_WANT_IDLE);
}

/** Wait until the drive is idle with no data pending; an error is `-EIO`. */
static int ata_wait_ready(struct ata_channel *ch)
{
    return ata_wait_for(ch, ATA_WANT_READY);
}

/** Wait until the drive has a sector to move; an error is `-EIO`. */
static int ata_wait_data(struct ata_channel *ch)
{
    return ata_wait_for(ch, ATA_WANT_DATA);
}

/**
 * The channel's interrupt: reading the status register clears the drive's
 * interrupt request, and the sleepers re-read it themselves.
 */
static void ata_irq(isr_params_t *params)
{
    struct ata_channel *ch = &channels[params->vector == ATA_VECTOR_BASE + ATA_IRQ_PRIMARY ? 0 : 1];

    ata_io->in8(ch->io + ATA_REG_STATUS);
    ch->irqs++;
    wq_wake_all(&ch->wq);
}

/**
 * A woken sleeper reads the status again, which ends a wait for an interrupt
 * the drive never raised, the first sector of a write among them. A channel
 * with sleepers counts toward `disk-wait-ticks` of `/proc/disk` here, since
 * this driver's transfers take no request of the block layer, which counts
 * the ticks its requests wait.
 */
void ata_tick(void)
{
    int c;

    for (c = 0; c < 2; c++) {
        if (channels[c].sleepers > 0) {
            block_count_wait_tick();
        }
    }
    for (c = 0; c < 2; c++) {
        struct ata_channel *ch = &channels[c];

        ch->ticks++;
        if (wq_active(&ch->wq)) {
            wq_wake_all(&ch->wq);
        }
    }
}

/**
 * Select `d` and program `count` sectors from `lba`, in the 28-bit form
 * when the range lies below 2^28 and the 48-bit form otherwise. Returns
 * the read or write command to issue for that form, 0 or 1 for `write`,
 * through the data port or, for `dma`, through the bus master. The 48-bit
 * form puts no address bits in the drive register.
 */
static uint8_t ata_program(const struct ata_drive *d, uint64_t lba, uint32_t count, int write,
                           int dma)
{
    struct ata_channel *ch = d->channel;
    uint8_t slave = d->slave ? ATA_DRIVE_SLAVE : 0;

    if (lba + count <= ATA_LBA28_SECTORS) {
        ata_io->out8(ch->io + ATA_REG_DRIVE,
                     (uint8_t)(ATA_DRIVE_SELECT | ATA_DRIVE_LBA | slave | ((lba >> 24) & 0x0F)));
        ata_io->out8(ch->io + ATA_REG_SECCOUNT,
                     count == ATA_MAX_SECTORS_PER_CMD ? 0 : (uint8_t)count);
        ata_io->out8(ch->io + ATA_REG_LBA_LOW, (uint8_t)lba);
        ata_io->out8(ch->io + ATA_REG_LBA_MID, (uint8_t)(lba >> 8));
        ata_io->out8(ch->io + ATA_REG_LBA_HIGH, (uint8_t)(lba >> 16));
        if (dma) {
            return write ? ATA_CMD_WRITE_DMA : ATA_CMD_READ_DMA;
        }
        return write ? ATA_CMD_WRITE : ATA_CMD_READ;
    }
    /* The 48-bit form writes each register twice, the high byte first. */
    ata_io->out8(ch->io + ATA_REG_DRIVE, (uint8_t)(ATA_DRIVE_SELECT | ATA_DRIVE_LBA | slave));
    ata_io->out8(ch->io + ATA_REG_SECCOUNT, (uint8_t)(count >> 8));
    ata_io->out8(ch->io + ATA_REG_LBA_LOW, (uint8_t)(lba >> 24));
    ata_io->out8(ch->io + ATA_REG_LBA_MID, (uint8_t)(lba >> 32));
    ata_io->out8(ch->io + ATA_REG_LBA_HIGH, (uint8_t)(lba >> 40));
    ata_io->out8(ch->io + ATA_REG_SECCOUNT, (uint8_t)count);
    ata_io->out8(ch->io + ATA_REG_LBA_LOW, (uint8_t)lba);
    ata_io->out8(ch->io + ATA_REG_LBA_MID, (uint8_t)(lba >> 8));
    ata_io->out8(ch->io + ATA_REG_LBA_HIGH, (uint8_t)(lba >> 16));
    if (dma) {
        return write ? ATA_CMD_WRITE_DMA_EXT : ATA_CMD_READ_DMA_EXT;
    }
    return write ? ATA_CMD_WRITE_EXT : ATA_CMD_READ_EXT;
}

/**
 * Move `count` sectors at `lba` through the bus master, one command and
 * one completion: a write's bytes from `src` copied into the bounce buffer
 * first, the descriptor written, the direction set, the command issued, the
 * engine started, the drive waited for, and a read's bytes copied out to
 * `dst` after. `count` is at most ATA_REQ_SECTORS, the bounce's size, and the
 * caller owns the channel. Returns 0, `-EIO` for a transfer the drive or the
 * bus master reported failed, or for a read the disk policy does not allow,
 * and `-EPERM` for a write it does not allow.
 *
 * The command is issued and the engine started with interrupts off: a drive
 * that has the command and not the engine shows DRQ and waits, and a caller
 * that cannot sleep, arriving on a tick between the two, would wait for that
 * DRQ to clear until its bound and then fail. A caller without the channel
 * may have run a command of its own while this one slept, and the status it
 * found first was this command's: an error it saw (`error_seen`) is this
 * command's failure.
 */
static int ata_dma_transfer(struct ata_drive *d, uint64_t lba, uint32_t count, const void *src,
                            void *dst)
{
    int write = src != NULL;
    struct ata_channel *ch = d->channel;
    uint32_t bytes = count * BLOCK_SECTOR_SIZE;
    uint32_t flags;
    uint8_t status;
    int rc;

    if (count > ATA_REQ_SECTORS) {
        return -EIO;
    }
    if (write) {
        k_memcpy(ch->bounce, src, bytes);
    }
    ch->prd.phys = (uint32_t)virt_to_phys(ch->bounce);
    ch->prd.bytes = (uint16_t)bytes;
    ch->prd.flags = ATA_PRD_EOT;
    if (ata_wait_not_busy(ch) != 0) {
        return -EIO;
    }
    ata_io->out8(ch->bm + ATA_BM_CMD, 0);
    ata_io->out8(ch->bm + ATA_BM_STATUS, ATA_BM_ERROR | ATA_BM_INTR);
    ata_io->out32(ch->bm + ATA_BM_PRD, (uint32_t)virt_to_phys(&ch->prd));
    ata_io->out8(ch->bm + ATA_BM_CMD, write ? 0 : ATA_BM_READ);
    flags = kirq_save();
    ch->error_seen = 0;
    if (!ata_command(ch, ata_program(d, lba, count, write, 1))) {
        kirq_restore(flags);
        return write ? -EPERM : -EIO;
    }
    ata_settle(ch);
    ata_io->out8(ch->bm + ATA_BM_CMD, (uint8_t)((write ? 0 : ATA_BM_READ) | ATA_BM_START));
    kirq_restore(flags);
    rc = ata_wait_ready(ch);
    status = ata_io->in8(ch->bm + ATA_BM_STATUS);
    ata_io->out8(ch->bm + ATA_BM_CMD, 0);
    ata_io->out8(ch->bm + ATA_BM_STATUS, ATA_BM_ERROR | ATA_BM_INTR);
    if (rc != 0 || (status & ATA_BM_ERROR) || ch->error_seen) {
        return -EIO;
    }
    if (!write) {
        k_memcpy(dst, ch->bounce, bytes);
    }
    return 0;
}

/**
 * Read `count` sectors at `lba` into `buffer`: through the bus master, a
 * bounce buffer's worth a command, when the drive does DMA and the caller
 * owns the channel (`owned`); through the data port, up to 256 sectors a
 * command, otherwise. Returns 0 or `-EIO`.
 *
 * The data port's sequence runs with interrupts off, whoever the caller is:
 * a command left in its data phase while another caller ran would hand that
 * caller its sectors.
 */
static int ata_read_on(struct ata_drive *d, uint64_t lba, uint32_t count, void *buffer, int owned)
{
    struct ata_channel *ch = d->channel;
    uint8_t *out = (uint8_t *)buffer;
    uint32_t flags;

    while (count > 0) {
        uint32_t chunk = (count > ATA_MAX_SECTORS_PER_CMD) ? ATA_MAX_SECTORS_PER_CMD : count;
        uint32_t i;

        if (d->dma && owned) {
            int rc;

            if (chunk > ATA_REQ_SECTORS) {
                chunk = ATA_REQ_SECTORS;
            }
            rc = ata_dma_transfer(d, lba, chunk, NULL, out);

            if (rc != 0) {
                return rc;
            }
            out += chunk * BLOCK_SECTOR_SIZE;
            lba += chunk;
            count -= chunk;
            continue;
        }
        flags = kirq_save();
        if (ata_wait_idle(ch) != 0) {
            kirq_restore(flags);
            return -EIO;
        }
        if (!ata_command(ch, ata_program(d, lba, chunk, 0, 0))) {
            kirq_restore(flags);
            return -EIO;
        }
        ata_settle(ch);
        for (i = 0; i < chunk; ++i) {
            if (ata_wait_data(ch) != 0) {
                kirq_restore(flags);
                return -EIO;
            }
            ata_io->ins16(ch->io + ATA_REG_DATA, out, ATA_WORDS_PER_SECTOR);
            out += BLOCK_SECTOR_SIZE;
        }
        kirq_restore(flags);
        lba += chunk;
        count -= chunk;
    }
    return 0;
}

/**
 * The device's `read`: whole sectors, from any context, owning the channel
 * where the caller can sleep for it. Returns 0, or `-EIO` for a range past
 * the disk or a failed transfer.
 */
static int ata_read(block_device_t *device, uint64_t lba, uint32_t count, void *buffer)
{
    struct ata_drive *d = (struct ata_drive *)device->drvdata;
    int owned, rc;

    if (count == 0 || lba > device->sector_count || count > device->sector_count - lba) {
        return -EIO;
    }
    owned = ata_acquire(d->channel);
    rc = ata_read_on(d, lba, count, buffer, owned);
    if (owned) {
        ata_release(d->channel);
    }
    return rc;
}

/**
 * Write `count` sectors at `lba` from `buffer` as `ata_read_on` reads them,
 * each command followed by a cache flush. Returns 0, `-EIO`, or `-EPERM`
 * when the disk policy does not allow the write or the flush.
 *
 * The flush waits for the drive to be idle first, so a caller without the
 * channel flushes behind the owner's command and not into it; and it selects
 * its drive again, since that caller may have selected the other one.
 */
static int ata_write_on(struct ata_drive *d, uint64_t lba, uint32_t count, const void *buffer,
                        int owned)
{
    struct ata_channel *ch = d->channel;
    const uint8_t *in = (const uint8_t *)buffer;
    uint32_t flags;

    while (count > 0) {
        uint32_t chunk = (count > ATA_MAX_SECTORS_PER_CMD) ? ATA_MAX_SECTORS_PER_CMD : count;
        uint32_t i;

        if (d->dma && owned) {
            int rc;

            if (chunk > ATA_REQ_SECTORS) {
                chunk = ATA_REQ_SECTORS;
            }
            rc = ata_dma_transfer(d, lba, chunk, in, NULL);

            if (rc != 0) {
                return rc;
            }
            in += chunk * BLOCK_SECTOR_SIZE;
        } else {
            flags = kirq_save();
            if (ata_wait_idle(ch) != 0) {
                kirq_restore(flags);
                return -EIO;
            }
            if (!ata_command(ch, ata_program(d, lba, chunk, 1, 0))) {
                kirq_restore(flags);
                return -EPERM;
            }
            ata_settle(ch);
            for (i = 0; i < chunk; ++i) {
                if (ata_wait_data(ch) != 0) {
                    kirq_restore(flags);
                    return -EIO;
                }
                ata_io->outs16(ch->io + ATA_REG_DATA, in, ATA_WORDS_PER_SECTOR);
                in += BLOCK_SECTOR_SIZE;
            }
            if (ata_wait_ready(ch) != 0) {
                kirq_restore(flags);
                return -EIO;
            }
            kirq_restore(flags);
        }
        if (ata_wait_idle(ch) != 0) {
            return -EIO;
        }
        ata_io->out8(ch->io + ATA_REG_DRIVE,
                     (uint8_t)(ATA_DRIVE_SELECT | (d->slave ? ATA_DRIVE_SLAVE : 0)));
        ata_settle(ch);
        if (!ata_command(ch, d->lba48 ? ATA_CMD_FLUSH_EXT : ATA_CMD_FLUSH)) {
            return -EPERM;
        }
        ata_settle(ch);
        if (ata_wait_ready(ch) != 0) {
            return -EIO;
        }
        lba += chunk;
        count -= chunk;
    }
    return 0;
}

/**
 * The device's `write`: whole sectors, then a flush of the drive's cache, so
 * a returned write is on the medium; the flush is the 48-bit form on a drive
 * that has it. Returns as `ata_write_on` does, and `-EIO` for a range past
 * the disk.
 */
static int ata_write(block_device_t *device, uint64_t lba, uint32_t count, const void *buffer)
{
    struct ata_drive *d = (struct ata_drive *)device->drvdata;
    int owned, rc;

    if (count == 0 || lba > device->sector_count || count > device->sector_count - lba) {
        return -EIO;
    }
    owned = ata_acquire(d->channel);
    rc = ata_write_on(d, lba, count, buffer, owned);
    if (owned) {
        ata_release(d->channel);
    }
    return rc;
}

/**
 * IDENTIFY the drive `d`, with its 256 words in `identify`, and fill its
 * record. Returns 0 for a disk, -1 for an absent drive, a packet device, a
 * drive that did not answer, or one the block layer cannot use.
 *
 * A channel with no drive leaves the bus floating, which reads as 0 on
 * emulators and 0xFF on most real hardware; 0xFF has BSY set, so without
 * that check an absent drive would wait out the whole polled bound. A packet
 * device answers IDENTIFY with the ATAPI signature in the LBA registers and
 * is passed over. A drive whose logical sector is not 512 bytes (word 106
 * bit 14 valid and bit 12 set), which the block math needs, is rejected with
 * a record, as the AHCI driver rejects one, and so is a capacity past what
 * LBA48 addresses. The 48-bit sector count is words 100-103 when word 83
 * says the drive has the 48-bit commands, else the 28-bit count in words
 * 60-61; a 28-bit drive is capped at what its commands address.
 */
static int ata_identify(struct ata_drive *d, uint16_t *identify)
{
    struct ata_channel *ch = d->channel;
    uint8_t status;

    ata_io->out8(ch->io + ATA_REG_DRIVE,
                 (uint8_t)(ATA_DRIVE_SELECT | (d->slave ? ATA_DRIVE_SLAVE : 0)));
    ata_settle(ch);
    ata_io->out8(ch->io + ATA_REG_SECCOUNT, 0);
    ata_io->out8(ch->io + ATA_REG_LBA_LOW, 0);
    ata_io->out8(ch->io + ATA_REG_LBA_MID, 0);
    ata_io->out8(ch->io + ATA_REG_LBA_HIGH, 0);
    if (!ata_command(ch, ATA_CMD_IDENTIFY)) {
        return -1;
    }
    ata_settle(ch);
    status = ata_io->in8(ch->io + ATA_REG_STATUS);
    if (status == 0 || status == 0xFF) {
        return -1;
    }
    if (ata_wait_not_busy(ch) != 0) {
        return -1;
    }
    if (ata_io->in8(ch->io + ATA_REG_LBA_MID) != 0 || ata_io->in8(ch->io + ATA_REG_LBA_HIGH) != 0) {
        return -1;
    }
    if (ata_wait_data(ch) != 0) {
        return -1;
    }
    ata_io->ins16(ch->io + ATA_REG_DATA, identify, ATA_WORDS_PER_SECTOR);
    if ((identify[106] & 0x4000u) != 0 && (identify[106] & 0x1000u) != 0) {
        klog(KLOG_ERR, "ata", "%s %s: refused: the logical sector is not 512 bytes", ch->name,
             d->slave ? "slave" : "master");
        return -1;
    }
    d->lba48 = (identify[83] & 0x0400u) != 0;
    if (d->lba48) {
        d->sectors = (uint64_t)identify[100] | ((uint64_t)identify[101] << 16) |
                     ((uint64_t)identify[102] << 32) | ((uint64_t)identify[103] << 48);
        if ((d->sectors >> 48) != 0) {
            klog(KLOG_ERR, "ata", "%s %s: refused: the capacity is past what LBA48 addresses",
                 ch->name, d->slave ? "slave" : "master");
            return -1;
        }
    }
    if (!d->lba48 || d->sectors == 0) {
        d->lba48 = 0;
        d->sectors = (uint64_t)identify[60] | ((uint64_t)identify[61] << 16);
        if (d->sectors > ATA_LBA28_SECTORS) {
            d->sectors = ATA_LBA28_SECTORS;
        }
    }
    return d->sectors == 0 ? -1 : 0;
}

/**
 * Take a channel's ports from the IDE controller's base registers when its
 * prog-if says the channel is in native mode, and leave the legacy ports
 * otherwise; and take the bus master's ports from the fifth base register,
 * and turn on bus mastering, when the prog-if says it has one. With no IDE
 * controller on the bus the legacy ports stay and there is no bus master.
 */
static void ata_native_ports(void)
{
    pci_device_t ide;
    uint8_t progif;

    if (!pci_find_device(PCI_CLASS_STORAGE, PCI_SUBCLASS_IDE, &ide)) {
        return;
    }
    progif = pci_read_config8(ide.bus, ide.device, ide.function, 0x09);
    if (progif & ATA_PROGIF_BUS_MASTER) {
        uint32_t bar4 = pci_read_config32(ide.bus, ide.device, ide.function, 0x20);

        if ((bar4 & 1u) && (bar4 & ~3u) != 0) {
            channels[0].bm = (uint16_t)(bar4 & ~3u);
            channels[1].bm = (uint16_t)((bar4 & ~3u) + ATA_BM_SECONDARY);
            pci_enable_bus_master(&ide);
        }
    }
    if (progif & ATA_PROGIF_PRIMARY_NATIVE) {
        channels[0].io =
            (uint16_t)(pci_read_config32(ide.bus, ide.device, ide.function, 0x10) & ~3u);
        channels[0].ctrl =
            (uint16_t)((pci_read_config32(ide.bus, ide.device, ide.function, 0x14) & ~3u) + 2);
    }
    if (progif & ATA_PROGIF_SECONDARY_NATIVE) {
        channels[1].io =
            (uint16_t)(pci_read_config32(ide.bus, ide.device, ide.function, 0x18) & ~3u);
        channels[1].ctrl =
            (uint16_t)((pci_read_config32(ide.bus, ide.device, ide.function, 0x1C) & ~3u) + 2);
    }
}

/**
 * Whether two IDENTIFY answers name one drive: words 10 through 46 equal,
 * which covers the serial number, the firmware revision and the model.
 */
static int same_drive(const uint16_t *a, const uint16_t *b)
{
    int w;

    for (w = 10; w < 47; w++) {
        if (a[w] != b[w]) {
            return 0;
        }
    }
    return 1;
}

/**
 * `rach2.atapio` keeps a drive on the data port, so its transfers move with
 * interrupts off rather than through the bus master. A caller can still
 * sleep then, for the channel and for the cache flush that follows a write.
 */
int ata_init(void)
{
    static uint16_t identify[4][ATA_WORDS_PER_SECTOR];
    int c, s, found = 0;

    ata_native_ports();
    for (c = 0; c < 2; c++) {
        struct ata_channel *ch = &channels[c];

        wait_queue_init(&ch->wq);
        register_isr_handler((uint8_t)(ATA_VECTOR_BASE + ch->irq), ata_irq, "ata");
        /* Device control with nIEN clear: the channel raises its interrupt. */
        ata_io->out8(ch->ctrl, 0x00);
        for (s = 0; s < 2; s++) {
            struct ata_drive *d = &drives[c * 2 + s];

            d->channel = ch;
            d->slave = s;
            d->present = 0;
            if (ata_identify(d, identify[c * 2 + s]) != 0) {
                continue;
            }
            /* Some controllers answer for an absent slave with the master. */
            if (s == 1 && drives[c * 2].present &&
                same_drive(identify[c * 2], identify[c * 2 + 1])) {
                continue;
            }
            d->present = 1;
            d->dma = ch->bm != 0 && (identify[c * 2 + s][49] & ATA_ID_CAP_DMA) != 0 &&
                     !kcmdline_has("rach2.atapio");
            d->dev.sector_size = BLOCK_SECTOR_SIZE;
            d->dev.sector_count = d->sectors;
            d->dev.flags = BLOCK_FAULT_SAFE;
            d->dev.max_sectors = ATA_REQ_SECTORS;
            d->dev.read = ata_read;
            d->dev.write = ata_write;
            d->dev.drvdata = d;
            if (!block0) {
                block0 = &d->dev;
            }
            disk_register(&d->dev, "ata", c * 2 + s, identify[c * 2 + s]);
            klog(KLOG_INFO, "ata", "%s %s: %d sectors, %d MiB%s, %s", ch->name,
                 s ? "slave" : "master", (uint32_t)d->sectors, (uint32_t)(d->sectors / 2048),
                 d->lba48 ? ", LBA48" : "", d->dma ? "bus master DMA" : "data port PIO");
            found++;
        }
    }
    return found > 0 ? 0 : -1;
}
