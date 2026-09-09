/*
 * msc.c: a flash drive, driven over Bulk-Only Transport.
 *
 * The transport is three transfers per command: a command block wrapper out,
 * a data stage when the command has one, and a status wrapper in. This file
 * is a state machine over those three and over the recovery the class
 * defines when one of them goes wrong, stepped from the one pump the timer
 * and the controller's interrupt share. One command is in flight per stick.
 *
 * Bringing a stick up is a sequence of such commands, and every step of it is
 * written from what real drives do rather than from the specification alone:
 * a drive answers `TEST UNIT READY` with a failure for a while after it is
 * plugged in, wants `REQUEST SENSE` after any failure before it will take the
 * next command, reports the address of its last block rather than a count,
 * stalls `Get Max LUN` instead of answering it, hides its flash behind a
 * CD-ROM on a promotional drive, and crashes if it is reset at
 * initialisation, which is why reset here is recovery and nothing else.
 *
 * What this driver refuses, by name and in the record: a block size other
 * than 512, because the block layer's sectors are 512 bytes; a capacity that
 * says the drive is larger than `READ(10)` can address; a drive with no LUN
 * that becomes ready inside ten seconds; and the UAS protocol, which the core
 * refuses before this file sees it.
 */

#include "msc.h"

#include "../block/block.h"
#include "../block/usbdisk.h"
#include "../klog.h"
#include "../lib/kerrno.h"
#include "../lib/kirq.h"
#include "../lib/kstring.h"
#include "core.h"
#include "hcd.h"
#include "xhci.h"

/* The two class requests, addressed to the interface. */
#define MSC_REQ_RESET   0xFF
#define MSC_REQ_MAX_LUN 0xFE
#define TYPE_CLASS_OUT  0x21
#define TYPE_CLASS_IN   0xA1

/* The wrappers. Both are little-endian on the wire, which is the byte order
 * this machine already has, so they are built and read field by field. */
#define CBW_SIGNATURE 0x43425355u /* "USBC" */
#define CSW_SIGNATURE 0x53425355u /* "USBS" */
#define CBW_BYTES     31
#define CSW_BYTES     13
#define CBW_IN        0x80

/* The status byte of a status wrapper. */
#define CSW_GOOD        0
#define CSW_FAILED      1
#define CSW_PHASE_ERROR 2

/* The commands this driver sends, and nothing else. */
#define SCSI_TEST_UNIT_READY 0x00
#define SCSI_REQUEST_SENSE   0x03
#define SCSI_INQUIRY         0x12
#define SCSI_MODE_SENSE_6    0x1A
#define SCSI_READ_CAPACITY   0x25
#define SCSI_READ_10         0x28
#define SCSI_WRITE_10        0x2A
#define SCSI_SYNCHRONIZE     0x35
#define SCSI_READ_16         0x88
#define SCSI_WRITE_16        0x8A
#define SCSI_SERVICE_IN      0x9E /* with the service action READ CAPACITY(16) */
#define SCSI_READ_CAPACITY16 0x10

/* The device type INQUIRY reports in the low five bits of its first byte. A
 * direct-access device is a disk; anything else on a LUN is passed over. */
#define SCSI_TYPE_DISK 0x00

/* Sense keys, of which this driver acts on three and records the rest. */
#define SENSE_NOT_READY       0x02
#define SENSE_ILLEGAL_REQUEST 0x05
#define SENSE_UNIT_ATTENTION  0x06
#define SENSE_MEDIUM_ERROR    0x03
#define SENSE_ILLEGAL_REQ     0x05
#define SENSE_UNIT_ATTENTION  0x06

/* The stages of one command. */
#define STAGE_IDLE  0
#define STAGE_CBW   1
#define STAGE_DATA  2
#define STAGE_CSW   3
#define STAGE_DONE  4
/* A class request on the control endpoint, which is one transfer and not a
 * wrapper: `Get Max LUN` and the reset are the two. */
#define STAGE_CLASS 5

/* What the driver is doing with the stick as a whole. */
#define MSC_STATE_FREE     0
#define MSC_STATE_MAX_LUN  1
#define MSC_STATE_INQUIRY  2
#define MSC_STATE_TEST     3
#define MSC_STATE_SENSE    4
#define MSC_STATE_CAPACITY 5
#define MSC_STATE_MODE     6
#define MSC_STATE_READY    7
#define MSC_STATE_REFUSED  8
#define MSC_STATE_GONE     9
#define MSC_STATE_CAPACITY16 10 /* the ten-byte answer was all ones */
#define MSC_STATE_SERIAL 11     /* INQUIRY's unit serial page, asked once */

/*
 * Recovery, which the class defines as three things in this order: the reset
 * request, the halt cleared on the inward endpoint, then on the outward one.
 * A driver that cleared them the other way round leaves a device that
 * answers nothing again.
 *
 * The two clears are commands the controller executes, and the controller
 * takes one command at a time for the whole stack, so they go through the
 * core's turn rather than being issued here.
 */
#define RECOVER_NONE      0
#define RECOVER_RESET     1
#define RECOVER_CLEAR_IN  2
#define RECOVER_CLEAR_OUT 3
/* The lighter recovery a stalled data or status stage takes: one endpoint
 * cleared, and then the status wrapper read. */
#define RECOVER_CLEAR_ONE 4

/* How long any one transfer of a command may take, and how long a stick has
 * to become ready after it is plugged in. */
#define MSC_STEP_MS     10000u
#define MSC_READY_MS    10000u
#define MSC_TEST_GAP_MS 100u

/* The most this driver moves in one command, which is the block layer's
 * request page. */
#define MSC_MAX_SECTORS BLOCK_REQ_SECTORS
#define MSC_SECTOR_SIZE 512u

/* How many requests may wait for one stick. The block layer's pool is
 * sixteen, one per process, so a queue of sixteen cannot overflow. */
#define MSC_QUEUE 16

/* How long a request has before the block layer gives up on it, in ticks of
 * the 100 Hz timer. Ten seconds, because a stick may have a controller of its
 * own to wake up. */
#define MSC_DEADLINE_TICKS 1000u

struct msc_device;

/** One logical unit of a stick: what it is and the device it is served as. */
struct msc_lun {
    int present;
    int index;
    struct msc_device *owner;
    uint64_t blocks;
    uint32_t block_size;
    int write_protect;
    block_device_t block;
};

struct msc_device {
    int used;
    struct xhci_ctrl *hc; /* the controller this stick is on */
    /* The stick has gone and the slot is not free: something still holds the
     * block device, so neither the slot nor anything in it may be reused
     * until the disk layer says the last hold is gone. */
    int closing;
    int device;    /* the controller's handle */
    int interface; /* for the two class requests */
    int ep_in;
    int ep_out;

    int state;
    int stage;
    int lun;
    int max_lun;
    int recover;       /* one of the `RECOVER_` values */
    int after_recover; /* the one endpoint a single clear is for */
    uint32_t tag;
    uint32_t deadline;
    uint32_t ready_left; /* milliseconds left for the stick to become ready */
    uint32_t test_gap;   /* milliseconds until the next TEST UNIT READY */

    /* The command in flight, its wrappers and its data. */
    uint8_t cbw[CBW_BYTES];
    uint8_t csw[CSW_BYTES];
    uint8_t scratch[64]; /* INQUIRY, REQUEST SENSE and MODE SENSE answers */
    uint8_t *data;
    uint32_t data_length;
    int data_in;
    uint32_t residue;
    int status; /* the status byte of the wrapper that ended the command */
    int sense_key;
    int retried; /* the request in flight has been sent a second time */
    int sensing; /* the command in flight is the REQUEST SENSE for the request */

    /* What the stick is, once it is up. INQUIRY's two strings are kept
     * because `scratch` is the next command's buffer as well. */
    char vendor[9];
    char product[17];
    char serial[21]; /* the unit serial page, or empty when it has none */
    uint32_t flushes; /* SYNCHRONIZE CACHE commands sent, for /proc/usb */

    /* What the unit being brought up answered; copied into its record. */
    uint64_t blocks;
    uint32_t block_size;
    int write_protect;

    /* The logical units that answered, each the block device it is
     * published as, whose `drvdata` is its record; a command addresses
     * `lun`. The request in flight on the stick and the ones waiting their
     * turn: two processes may want one stick, since either can sleep inside
     * a request while the other runs, so the requests are admitted in the
     * order they arrived and served one at a time, the units sharing the
     * one pipe; a flush waits its turn like a read, which is what puts it
     * after every write before it. */
    struct msc_lun luns[MSC_MAX_LUNS];
    int ready_luns;
    struct block_request *request;
    int flushing;
    struct block_request *queue[MSC_QUEUE];
    int queue_flush[MSC_QUEUE];
    int queue_lun[MSC_QUEUE]; /* the unit each waiting request is for */
    int queue_head;
    int queue_count;
};

static struct msc_device sticks[MSC_MAX_DEVICES];

/* The tag of the last command sent to any stick. Tags go up across every
 * stick and every attach, so a completion left over from a stick that was
 * pulled cannot match a command of the stick plugged in after it on the
 * same controller handle. */
static uint32_t tag_seed;

/* The one buffer a control transfer's setup packet is built in. One command
 * is in flight per stick and the class requests are part of that command, so
 * one per stick is enough. */
static uint8_t setup_buffer[MSC_MAX_DEVICES][8];

static struct block_request *queue_take(struct msc_device *m, int *flush);
static int queue_put(struct msc_device *m, struct block_request *r, int flush, int lun);
static int status_begin(struct msc_device *m);
static void command_end(struct msc_device *m, int status);
static void queue_drain(struct msc_device *m, int status);
static void publish(struct msc_device *m, int lun);

/*
 * The stick at `device` on `hc`. Slot numbers start at one on every
 * controller, so a machine with two of them has two sticks called slot one
 * and the controller is half of the name.
 */
static struct msc_device *by_device(struct xhci_ctrl *hc, int device)
{
    int i;

    for (i = 0; i < MSC_MAX_DEVICES; i++) {
        if (sticks[i].used && !sticks[i].closing && sticks[i].hc == hc &&
            sticks[i].device == device) {
            return &sticks[i];
        }
    }
    return 0;
}

/**
 * Copy a fixed-width SCSI string out, trimmed of its trailing spaces.
 *
 * INQUIRY pads its two names with spaces to their full width, and a name
 * printed with the padding lines up with nothing. `out` has room for
 * `width` characters and the terminator.
 */
static void take_string(char *out, const uint8_t *in, int width)
{
    int i;
    int end = 0;

    for (i = 0; i < width; i++) {
        char c = (char)in[i];

        /* A stick may put anything in these bytes; what cannot be printed is
         * not passed on to a reader of /proc/usb. */
        if (c < 0x20 || c > 0x7E) {
            c = ' ';
        }
        out[i] = c;
        if (c != ' ') {
            end = i + 1;
        }
    }
    out[end] = '\0';
}

/** Build a setup packet in the stick's own buffer. */
static void setup_fill(struct msc_device *m, int type, int request, int value, int index,
                       int length)
{
    uint8_t *p = setup_buffer[m - sticks];

    p[0] = (uint8_t)type;
    p[1] = (uint8_t)request;
    p[2] = (uint8_t)(value & 0xFF);
    p[3] = (uint8_t)((value >> 8) & 0xFF);
    p[4] = (uint8_t)(index & 0xFF);
    p[5] = (uint8_t)((index >> 8) & 0xFF);
    p[6] = (uint8_t)(length & 0xFF);
    p[7] = (uint8_t)((length >> 8) & 0xFF);
}

static uint32_t read32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void write32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static uint32_t read32_be(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static void write32_be(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

/**
 * Start one SCSI command: build its wrapper and submit it.
 *
 * `cdb` is the command block, `data` the buffer for its data stage and
 * `length` how many bytes that stage moves, zero for a command with none.
 * Returns 0 when the wrapper was submitted.
 */
static int command_begin(struct msc_device *m, const uint8_t *cdb, int cdb_length, uint8_t *data,
                         uint32_t length, int data_in)
{
    struct usb_transfer transfer;
    int i;

    if (cdb_length < 1 || cdb_length > 16) {
        return -1;
    }
    m->tag = ++tag_seed;
    for (i = 0; i < CBW_BYTES; i++) {
        m->cbw[i] = 0;
    }
    write32(&m->cbw[0], CBW_SIGNATURE);
    write32(&m->cbw[4], m->tag);
    write32(&m->cbw[8], length);
    m->cbw[12] = (uint8_t)((length != 0 && data_in) ? CBW_IN : 0);
    m->cbw[13] = (uint8_t)m->lun;
    m->cbw[14] = (uint8_t)cdb_length;
    for (i = 0; i < cdb_length; i++) {
        m->cbw[15 + i] = cdb[i];
    }

    m->data = data;
    m->data_length = length;
    m->data_in = data_in;
    m->residue = 0;
    m->status = -1;

    transfer.setup = 0;
    transfer.buffer = m->cbw;
    transfer.length = CBW_BYTES;
    transfer.direction = USB_DIR_OUT;
    transfer.generation = m->tag;
    if (xhci_submit(m->hc, m->device, m->ep_out, &transfer) != 0) {
        return -1;
    }
    m->stage = STAGE_CBW;
    m->deadline = MSC_STEP_MS;
    return 0;
}

/** Submit the data stage of the command in flight. */
static int data_begin(struct msc_device *m)
{
    struct usb_transfer transfer;

    transfer.setup = 0;
    transfer.buffer = m->data;
    transfer.length = m->data_length;
    transfer.direction = m->data_in ? USB_DIR_IN : USB_DIR_OUT;
    transfer.generation = m->tag;
    if (xhci_submit(m->hc, m->device, m->data_in ? m->ep_in : m->ep_out, &transfer) != 0) {
        return -1;
    }
    m->stage = STAGE_DATA;
    m->deadline = MSC_STEP_MS;
    return 0;
}

/** Submit the status wrapper of the command in flight. */
static int status_begin(struct msc_device *m)
{
    struct usb_transfer transfer;
    int i;

    for (i = 0; i < CSW_BYTES; i++) {
        m->csw[i] = 0;
    }
    transfer.setup = 0;
    transfer.buffer = m->csw;
    transfer.length = CSW_BYTES;
    transfer.direction = USB_DIR_IN;
    transfer.generation = m->tag;
    if (xhci_submit(m->hc, m->device, m->ep_in, &transfer) != 0) {
        return -1;
    }
    m->stage = STAGE_CSW;
    m->deadline = MSC_STEP_MS;
    return 0;
}

/**
 * Begin reset recovery: the class reset, the halt cleared on the inward
 * endpoint, then on the outward one, and then the command is failed.
 *
 * The specification's order is exactly this, and a driver that cleared the
 * outward endpoint first leaves a device that answers nothing again.
 */
/** As `recover_begin`, and says what the controller reported. */
static void recover_with_code(struct msc_device *m, const char *why, int code);

static void recover_begin(struct msc_device *m, const char *why)
{
    klog(KLOG_WARN, "usbdisk", "recovery: device %d: %s", m->device, why);
    setup_fill(m, TYPE_CLASS_OUT, MSC_REQ_RESET, 0, m->interface, 0);
    {
        struct usb_transfer transfer;

        transfer.setup = setup_buffer[m - sticks];
        transfer.buffer = 0;
        transfer.length = 0;
        transfer.direction = USB_DIR_OUT;
        transfer.generation = m->tag;
        if (xhci_submit(m->hc, m->device, 1, &transfer) != 0) {
            m->recover = RECOVER_NONE;
            m->stage = STAGE_DONE;
            m->status = CSW_PHASE_ERROR;
            return;
        }
    }
    m->recover = RECOVER_RESET;
    m->deadline = MSC_STEP_MS;
}

static void recover_with_code(struct msc_device *m, const char *why, int code)
{
    klog(KLOG_WARN, "usbdisk", "device %d: %s: completion code %d", m->device, why, code);
    recover_begin(m, why);
}

/**
 * Turn the recovery that is waiting on the controller's commands.
 *
 * Returns 1 while recovery is running, 0 when there is none.
 */
static int step_recovery(struct msc_device *m)
{
    int answer;

    switch (m->recover) {
    case RECOVER_NONE:
        return 0;

    case RECOVER_RESET:
        return 1; /* the reset request's own completion moves this on */

    case RECOVER_CLEAR_IN:
        answer = usb_core_clear_halt(m->hc, m->device, m->ep_in);
        if (answer == 0) {
            return 1;
        }
        m->recover = RECOVER_CLEAR_OUT;
        m->deadline = MSC_STEP_MS;
        return 1;

    case RECOVER_CLEAR_OUT:
        answer = usb_core_clear_halt(m->hc, m->device, m->ep_out);
        if (answer == 0) {
            return 1;
        }
        m->recover = RECOVER_NONE;
        /* Recovery is done and the command it was for is failed: the device
         * is in a state to take the next one, and this one is lost. */
        command_end(m, CSW_PHASE_ERROR);
        return 1;

    case RECOVER_CLEAR_ONE:
        answer = usb_core_clear_halt(m->hc, m->device, m->after_recover);
        if (answer == 0) {
            return 1;
        }
        m->recover = RECOVER_NONE;
        if (answer < 0 || status_begin(m) != 0) {
            command_end(m, CSW_PHASE_ERROR);
        }
        return 1;

    default:
        m->recover = RECOVER_NONE;
        return 0;
    }
}

/** The command ended, well or badly; whoever asked for it is told by state. */
static void command_end(struct msc_device *m, int status)
{
    m->stage = STAGE_DONE;
    m->status = status;
}

/**
 * Finish the block request in flight, if there is one, with `status`.
 */
static void request_end(struct msc_device *m, int status)
{
    struct block_request *r = m->request;

    if (r == 0) {
        return;
    }
    m->request = 0;
    block_request_complete(r, r->generation, status);
}

int msc_attach(struct xhci_ctrl *hc, int device, int interface, int ep_in, int ep_out)
{
    int i;
    struct msc_device *m = 0;

    for (i = 0; i < MSC_MAX_DEVICES; i++) {
        if (!sticks[i].used) {
            m = &sticks[i];
            break;
        }
    }
    if (m == 0) {
        klog(KLOG_WARN, "usbdisk", "refused: device %d: this kernel drives %d sticks at once",
             device, MSC_MAX_DEVICES);
        return -1;
    }

    k_memset(m, 0, sizeof(*m));
    m->used = 1;
    m->hc = hc;
    m->device = device;
    m->interface = interface;
    m->ep_in = ep_in;
    m->ep_out = ep_out;
    m->lun = 0;
    m->max_lun = 0;
    m->state = MSC_STATE_MAX_LUN;
    m->stage = STAGE_IDLE;
    m->ready_left = MSC_READY_MS;
    m->block_size = MSC_SECTOR_SIZE;
    klog(KLOG_INFO, "usbdisk", "found: device %d on endpoints %d and %d", device, ep_in, ep_out);
    return 0;
}

void msc_detach(struct xhci_ctrl *hc, int device)
{
    struct msc_device *m = by_device(hc, device);

    if (m == 0) {
        return;
    }
    request_end(m, -EIO);
    queue_drain(m, -EIO);
    m->state = MSC_STATE_GONE;
    m->stage = STAGE_IDLE;
    /* The slot stays taken. Its block device is what a mount, a description
     * and a held buffer point at, and a second stick given this slot would
     * be given their pointers with it; the disk layer frees the slot when
     * the last of those has gone. */
    m->closing = 1;
    klog(KLOG_INFO, "usbdisk", "gone: device %d", device);
    usbdisk_left((int)(m - sticks));
}

/**
 * Free a slot whose device nothing holds any more.
 *
 * Called by the disk layer and by nothing else: it is the one place that
 * knows the teardown count reached zero.
 */
void msc_release(int index)
{
    if (index < 0 || index >= MSC_MAX_DEVICES) {
        return;
    }
    if (!sticks[index].closing) {
        return;
    }
    k_memset(&sticks[index], 0, sizeof(sticks[index]));
}

int msc_ready(int index)
{
    if (index < 0 || index >= MSC_MAX_DEVICES) {
        return 0;
    }
    return sticks[index].used && !sticks[index].closing && sticks[index].state == MSC_STATE_READY;
}

int msc_lun_ident(int index, int lun, struct msc_ident *out)
{
    struct msc_device *m;
    struct msc_lun *u;
    int i;

    if (index < 0 || index >= MSC_MAX_DEVICES || lun < 0 || lun >= MSC_MAX_LUNS || out == 0) {
        return -1;
    }
    m = &sticks[index];
    u = &m->luns[lun];
    if (!m->used || !u->present) {
        return -1;
    }
    for (i = 0; i < (int)sizeof(out->vendor); i++) {
        out->vendor[i] = m->vendor[i];
    }
    for (i = 0; i < (int)sizeof(out->product); i++) {
        out->product[i] = m->product[i];
    }
    for (i = 0; i < (int)sizeof(out->serial); i++) {
        out->serial[i] = m->serial[i];
    }
    out->blocks = u->blocks;
    out->flushes = m->flushes;
    out->block_size = u->block_size;
    out->write_protect = u->write_protect;
    out->closing = m->closing;
    out->device = m->device;
    return 0;
}

int msc_ident(int index, struct msc_ident *out)
{
    return msc_lun_ident(index, 0, out);
}

int msc_count(void)
{
    int i;
    int n = 0;

    for (i = 0; i < MSC_MAX_DEVICES; i++) {
        if (sticks[i].used && sticks[i].state == MSC_STATE_READY) {
            n++;
        }
    }
    return n;
}

int msc_completion(const struct usb_completion *completion)
{
    struct msc_device *m = by_device(completion->hc, completion->device);

    if (m == 0 || !m->used) {
        return 0;
    }
    if (completion->endpoint != m->ep_in && completion->endpoint != m->ep_out &&
        completion->endpoint != 1) {
        return 0;
    }
    if (completion->generation != m->tag) {
        /* A completion for a command this driver has given up on. Its buffer
         * may already be another request's, so it is dropped and recorded. */
        klog(KLOG_DEBUG, "usbdisk", "late: device %d: a completion for tag %d", m->device,
             (int)completion->generation);
        return 1;
    }

    if (m->recover == RECOVER_RESET) {
        /* The reset request has answered, well or badly; either way the two
         * clears follow, because an endpoint left halted is an endpoint that
         * carries nothing again. */
        m->recover = RECOVER_CLEAR_IN;
        m->deadline = MSC_STEP_MS;
        return 1;
    }
    if (m->recover != RECOVER_NONE) {
        return 1; /* the clears are commands, and `msc_step` turns them */
    }

    switch (m->stage) {
    case STAGE_CLASS:
        /* A class request is one transfer. A stall is an answer of its own:
         * a drive that will not say how many logical units it has, has one. */
        command_end(m, (completion->status == USB_XFER_OK || completion->status == USB_XFER_SHORT)
                           ? CSW_GOOD
                           : CSW_FAILED);
        m->residue = completion->residual;
        return 1;

    case STAGE_CBW:
        if (completion->status == USB_XFER_STALLED) {
            recover_begin(m, "the command wrapper was refused");
            return 1;
        }
        if (completion->status != USB_XFER_OK) {
            recover_begin(m, "the command wrapper did not go out");
            return 1;
        }
        if (m->data_length != 0) {
            if (data_begin(m) != 0) {
                command_end(m, CSW_PHASE_ERROR);
            }
            return 1;
        }
        if (status_begin(m) != 0) {
            command_end(m, CSW_PHASE_ERROR);
        }
        return 1;

    case STAGE_DATA:
        if (completion->status == USB_XFER_STALLED) {
            /* A halted data endpoint is cleared and the status wrapper read:
             * the stick still has one to give, and it says how much of the
             * transfer it did. */
            m->recover = RECOVER_CLEAR_ONE;
            m->after_recover = m->data_in ? m->ep_in : m->ep_out;
            m->deadline = MSC_STEP_MS;
            return 1;
        }
        if (completion->status != USB_XFER_OK && completion->status != USB_XFER_SHORT) {
            recover_with_code(m, "the data stage failed", completion->code);
            return 1;
        }
        m->residue = completion->residual;
        if (status_begin(m) != 0) {
            command_end(m, CSW_PHASE_ERROR);
        }
        return 1;

    case STAGE_CSW:
        if (completion->status == USB_XFER_STALLED) {
            if (m->retried) {
                recover_begin(m, "the status wrapper stalled twice");
                return 1;
            }
            m->retried = 1;
            m->recover = RECOVER_CLEAR_ONE;
            m->after_recover = m->ep_in;
            m->deadline = MSC_STEP_MS;
            return 1;
        }
        if (completion->status != USB_XFER_OK && completion->status != USB_XFER_SHORT) {
            recover_begin(m, "the status wrapper did not arrive");
            return 1;
        }
        if (completion->length - completion->residual != CSW_BYTES) {
            recover_begin(m, "the status wrapper was short");
            return 1;
        }
        if (read32(&m->csw[0]) != CSW_SIGNATURE) {
            recover_begin(m, "the status wrapper had the wrong signature");
            return 1;
        }
        if (read32(&m->csw[4]) != m->tag) {
            recover_begin(m, "the status wrapper named another command");
            return 1;
        }
        if (m->csw[12] > CSW_PHASE_ERROR) {
            recover_begin(m, "the status wrapper had a status this class does not define");
            return 1;
        }
        if (m->csw[12] == CSW_PHASE_ERROR) {
            recover_begin(m, "the device reported a phase error");
            return 1;
        }
        m->residue = read32(&m->csw[8]);
        if (m->residue > m->data_length) {
            recover_begin(m, "the status wrapper claimed more residue than the transfer had");
            return 1;
        }
        command_end(m, m->csw[12]);
        return 1;

    default:
        return 1;
    }
}

/* ---- bringing a stick up, and serving it ---- */

/** Send one class request with no data: the reset, or nothing else so far. */
static int class_request_in(struct msc_device *m, int request, uint8_t *data, int length)
{
    struct usb_transfer transfer;

    setup_fill(m, TYPE_CLASS_IN, request, 0, m->interface, length);
    transfer.setup = setup_buffer[m - sticks];
    transfer.buffer = data;
    transfer.length = (uint32_t)length;
    transfer.direction = USB_DIR_IN;
    m->tag = ++tag_seed;
    transfer.generation = m->tag;
    if (xhci_submit(m->hc, m->device, 1, &transfer) != 0) {
        return -1;
    }
    m->stage = STAGE_CLASS;
    m->data_length = 0;
    m->deadline = MSC_STEP_MS;
    return 0;
}

/** The six-byte command blocks this driver sends, built where they are used. */
static void cdb_clear(uint8_t *cdb, int length)
{
    int i;

    for (i = 0; i < length; i++) {
        cdb[i] = 0;
    }
}

static int send_test_unit_ready(struct msc_device *m)
{
    uint8_t cdb[6];

    cdb_clear(cdb, 6);
    cdb[0] = SCSI_TEST_UNIT_READY;
    return command_begin(m, cdb, 6, 0, 0, 0);
}

static int send_request_sense(struct msc_device *m)
{
    uint8_t cdb[6];

    cdb_clear(cdb, 6);
    cdb[0] = SCSI_REQUEST_SENSE;
    cdb[4] = 18;
    return command_begin(m, cdb, 6, m->scratch, 18, 1);
}

static int send_inquiry(struct msc_device *m)
{
    uint8_t cdb[6];

    cdb_clear(cdb, 6);
    cdb[0] = SCSI_INQUIRY;
    cdb[4] = 36;
    return command_begin(m, cdb, 6, m->scratch, 36, 1);
}

/** INQUIRY's vital product data page 0x80, the unit serial number, which
 * is what a person names a stick by; a device without the page answers
 * with an error, and the serial is left empty. */
static int send_inquiry_serial(struct msc_device *m)
{
    uint8_t cdb[6];

    cdb_clear(cdb, 6);
    cdb[0] = SCSI_INQUIRY;
    cdb[1] = 1; /* EVPD */
    cdb[2] = 0x80;
    cdb[4] = 64;
    return command_begin(m, cdb, 6, m->scratch, 64, 1);
}

static int send_read_capacity(struct msc_device *m)
{
    uint8_t cdb[10];

    cdb_clear(cdb, 10);
    cdb[0] = SCSI_READ_CAPACITY;
    return command_begin(m, cdb, 10, m->scratch, 8, 1);
}

/** READ CAPACITY(16), for a unit whose ten-byte answer was all ones. */
static int send_read_capacity16(struct msc_device *m)
{
    uint8_t cdb[16];

    cdb_clear(cdb, 16);
    cdb[0] = SCSI_SERVICE_IN;
    cdb[1] = SCSI_READ_CAPACITY16;
    cdb[13] = 32; /* the allocation length, big-endian in bytes 10-13 */
    return command_begin(m, cdb, 16, m->scratch, 32, 1);
}

static int send_mode_sense(struct msc_device *m)
{
    uint8_t cdb[6];

    cdb_clear(cdb, 6);
    cdb[0] = SCSI_MODE_SENSE_6;
    cdb[2] = 0x3F; /* every page, of which only the header is read */
    cdb[4] = 4;
    return command_begin(m, cdb, 6, m->scratch, 4, 1);
}

/**
 * READ(10) or WRITE(10), or their sixteen-byte forms on a unit with more
 * sectors than a ten-byte address names, to the unit `m->lun` addresses.
 */
static int send_read_write(struct msc_device *m, struct block_request *r)
{
    struct msc_lun *u = &m->luns[m->lun];
    uint8_t cdb[16];

    if (u->blocks > 0xFFFFFFFFull) {
        cdb_clear(cdb, 16);
        cdb[0] = r->write ? SCSI_WRITE_16 : SCSI_READ_16;
        write32_be(&cdb[2], (uint32_t)(r->lba >> 32));
        write32_be(&cdb[6], (uint32_t)r->lba);
        write32_be(&cdb[10], r->count);
        return command_begin(m, cdb, 16, r->page, r->count * MSC_SECTOR_SIZE, !r->write);
    }
    cdb_clear(cdb, 10);
    cdb[0] = r->write ? SCSI_WRITE_10 : SCSI_READ_10;
    write32_be(&cdb[2], (uint32_t)r->lba);
    cdb[7] = (uint8_t)((r->count >> 8) & 0xFF);
    cdb[8] = (uint8_t)(r->count & 0xFF);
    return command_begin(m, cdb, 10, r->page, r->count * MSC_SECTOR_SIZE, !r->write);
}

static int send_synchronize(struct msc_device *m)
{
    uint8_t cdb[10];

    cdb_clear(cdb, 10);
    cdb[0] = SCSI_SYNCHRONIZE;
    return command_begin(m, cdb, 10, 0, 0, 0);
}

/** Give up on this stick, with the reason in the record. */
static void refuse(struct msc_device *m, const char *why)
{
    klog(KLOG_WARN, "usbdisk", "refused: device %d: %s", m->device, why);
    m->state = MSC_STATE_REFUSED;
    m->stage = STAGE_IDLE;
    request_end(m, -EIO);
    queue_drain(m, -EIO);
}

static void ready(struct msc_device *m);

/**
 * Go on to the next logical unit, or, when there is none, serve the units
 * that answered; refuse the device when none did. `why` is what the unit
 * just left behind said, or NULL for one that answered.
 */
static void next_lun(struct msc_device *m, const char *why)
{
    if (why) {
        klog(KLOG_INFO, "usbdisk", "device %d: lun %d passed over: %s", m->device, m->lun, why);
    }
    if (m->lun >= m->max_lun || m->lun + 1 >= MSC_MAX_LUNS) {
        if (m->ready_luns == 0) {
            refuse(m, why ? why : "no unit answered");
            return;
        }
        ready(m);
        return;
    }
    m->lun++;
    m->ready_left = MSC_READY_MS;
    m->state = MSC_STATE_INQUIRY;
    m->stage = STAGE_IDLE;
}

/** The unit being brought up answered everything: keep its record. */
static void lun_ready(struct msc_device *m)
{
    struct msc_lun *u = &m->luns[m->lun];

    u->present = 1;
    u->index = m->lun;
    u->owner = m;
    u->blocks = m->blocks;
    u->block_size = m->block_size;
    u->write_protect = m->write_protect;
    m->ready_luns++;
    klog(KLOG_INFO, "usbdisk", "ready: device %d: lun %d, %d sectors of %d bytes%s", m->device,
         m->lun, (int)m->blocks, (int)m->block_size, m->write_protect ? ", write protected" : "");
    next_lun(m, 0);
}

/** Every unit has been asked: serve the ones that answered. */
static void ready(struct msc_device *m)
{
    int lun;

    m->state = MSC_STATE_READY;
    m->stage = STAGE_IDLE;
    for (lun = 0; lun < MSC_MAX_LUNS; lun++) {
        if (m->luns[lun].present) {
            publish(m, lun);
            usbdisk_arrived((int)(m - sticks), lun);
        }
    }
}

/**
 * Finish the request being served with `status`, and count a flush that
 * reached the medium.
 */
static void serve_end(struct msc_device *m, int status)
{
    if (status == 0 && m->flushing) {
        m->flushes++;
        block_count_flush(); /* reported by /proc/disk beside the disk's own */
    }
    m->flushing = 0;
    m->retried = 0;
    m->sensing = 0;
    request_end(m, status);
}

/**
 * What to do with a command that has ended, given what it was for.
 */
static void step_command_done(struct msc_device *m)
{
    int status = m->status;

    m->stage = STAGE_IDLE;

    switch (m->state) {
    case MSC_STATE_MAX_LUN:
        /* A drive that stalls the request has one logical unit, which is
         * what most sticks do; one that answers with more than fifteen is
         * answering with rubbish. */
        if (status == CSW_GOOD && m->residue == 0 && m->scratch[0] <= 15) {
            m->max_lun = m->scratch[0];
        } else {
            m->max_lun = 0;
        }
        m->lun = 0;
        m->state = MSC_STATE_INQUIRY;
        return;

    case MSC_STATE_INQUIRY:
        if (status != CSW_GOOD) {
            next_lun(m, "it would not say what it is");
            return;
        }
        if ((m->scratch[0] & 0x1F) != SCSI_TYPE_DISK) {
            next_lun(m, "it is not a disk");
            return;
        }
        take_string(m->vendor, &m->scratch[8], 8);
        take_string(m->product, &m->scratch[16], 16);
        m->serial[0] = '\0';
        m->state = MSC_STATE_SERIAL;
        return;

    case MSC_STATE_SERIAL:
        if (status == CSW_GOOD && m->scratch[1] == 0x80) {
            int len = m->scratch[3];

            take_string(m->serial, &m->scratch[4], len < 20 ? len : 20);
            m->state = MSC_STATE_TEST;
            return;
        }
        /* No such page: the failure's sense is asked for, as any other, and
         * the tests go on from there. */
        m->state = status == CSW_GOOD ? MSC_STATE_TEST : MSC_STATE_SENSE;
        return;

    case MSC_STATE_TEST:
        if (status == CSW_GOOD) {
            m->state = MSC_STATE_CAPACITY;
            return;
        }
        /* A drive answers a failure with a sense that says why, and will not
         * take the next command until it has been asked for it. */
        m->state = MSC_STATE_SENSE;
        return;

    case MSC_STATE_SENSE:
        m->sense_key = (status == CSW_GOOD) ? (m->scratch[2] & 0x0F) : 0;
        if (m->sense_key == SENSE_NOT_READY && m->scratch[12] == 0x3A) {
            next_lun(m, "there is no medium in it");
            return;
        }
        if (m->ready_left == 0) {
            refuse(m, "it did not become ready");
            return;
        }
        m->test_gap = MSC_TEST_GAP_MS;
        m->state = MSC_STATE_TEST;
        return;

    case MSC_STATE_CAPACITY:
        if (status != CSW_GOOD) {
            refuse(m, "it would not say how large it is");
            return;
        }
        {
            uint32_t last = read32_be(&m->scratch[0]);
            uint32_t size = read32_be(&m->scratch[4]);

            if (last == 0xFFFFFFFFu) {
                /* More sectors than a ten-byte address names: the
                 * sixteen-byte form says how many. */
                m->state = MSC_STATE_CAPACITY16;
                return;
            }
            if (size != MSC_SECTOR_SIZE) {
                next_lun(m, "its blocks are not 512 bytes");
                return;
            }
            /* The field is the address of the last block, not a count. */
            m->blocks = (uint64_t)last + 1u;
            m->block_size = size;
        }
        m->state = MSC_STATE_MODE;
        return;

    case MSC_STATE_CAPACITY16:
        if (status != CSW_GOOD) {
            refuse(m, "it would not say how large it is");
            return;
        }
        {
            uint64_t last = ((uint64_t)read32_be(&m->scratch[0]) << 32) | read32_be(&m->scratch[4]);
            uint32_t size = read32_be(&m->scratch[8]);

            if (last == 0xFFFFFFFFFFFFFFFFull) {
                refuse(m, "it reported an impossible capacity");
                return;
            }
            if (size != MSC_SECTOR_SIZE) {
                next_lun(m, "its blocks are not 512 bytes");
                return;
            }
            m->blocks = last + 1u;
            m->block_size = size;
        }
        m->state = MSC_STATE_MODE;
        return;

    case MSC_STATE_MODE:
        /* A drive that refuses the page has no write protection to report,
         * which is the ordinary answer and not a failure. */
        m->write_protect = (status == CSW_GOOD) && (m->scratch[2] & 0x80) != 0;
        lun_ready(m);
        return;

    case MSC_STATE_READY:
        if (m->request == 0) {
            return;
        }
        if (m->sensing) {
            /* The answer to the REQUEST SENSE sent for the request. A stick
             * with no cache answers a SYNCHRONIZE CACHE with ILLEGAL
             * REQUEST, which is not a failure; a UNIT ATTENTION is the stick
             * saying it was reset or its medium changed, and the command is
             * sent once more; anything else fails the request. */
            int key = (status == CSW_GOOD && m->residue == 0) ? (m->scratch[2] & 0x0F) : -1;

            m->sensing = 0;
            m->sense_key = key < 0 ? 0 : key;
            if (key == SENSE_ILLEGAL_REQUEST && m->flushing) {
                serve_end(m, 0);
                return;
            }
            if (key == SENSE_UNIT_ATTENTION && !m->retried) {
                m->retried = 1;
                if ((m->flushing ? send_synchronize(m) : send_read_write(m, m->request)) == 0) {
                    return;
                }
            }
            serve_end(m, -EIO);
            return;
        }
        if (status == CSW_FAILED) {
            /* A check condition: the sense says what it was, and the stick
             * will not take the next command until it has been asked. */
            m->sensing = 1;
            if (send_request_sense(m) == 0) {
                return;
            }
            serve_end(m, -EIO);
            return;
        }
        serve_end(m, (status == CSW_GOOD && (m->residue == 0 || m->flushing)) ? 0 : -EIO);
        return;

    default:
        return;
    }
}

/** Start whatever this stick should be doing next. */
static void step_idle(struct msc_device *m)
{
    switch (m->state) {
    case MSC_STATE_MAX_LUN:
        (void)class_request_in(m, MSC_REQ_MAX_LUN, m->scratch, 1);
        return;

    case MSC_STATE_INQUIRY:
        (void)send_inquiry(m);
        return;

    case MSC_STATE_SERIAL:
        (void)send_inquiry_serial(m);
        return;

    case MSC_STATE_TEST:
        if (m->test_gap != 0) {
            return;
        }
        (void)send_test_unit_ready(m);
        return;

    case MSC_STATE_SENSE:
        (void)send_request_sense(m);
        return;

    case MSC_STATE_CAPACITY:
        (void)send_read_capacity(m);
        return;

    case MSC_STATE_CAPACITY16:
        (void)send_read_capacity16(m);
        return;

    case MSC_STATE_MODE:
        (void)send_mode_sense(m);
        return;

    case MSC_STATE_READY:
        if (m->request != 0) {
            if (m->flushing) {
                (void)send_synchronize(m);
            } else {
                (void)send_read_write(m, m->request);
            }
        }
        return;

    default:
        return;
    }
}

/**
 * A transfer of the command in flight never answered.
 *
 * The controller still owns the transfer and the page it points at, which is
 * the block layer's and goes to the next request the moment this one is
 * completed. So the request is not completed here. Recovery is begun
 * instead, and its clearing of the two endpoints takes the transfer back
 * from the controller; the request is failed when recovery ends, late,
 * which is what the block layer's quarantine is for. A stick that stops
 * answering while it is being brought up is refused.
 */
static void step_timeout(struct msc_device *m)
{
    klog(KLOG_WARN, "usbdisk", "device %d: a transfer never answered", m->device);
    m->tag = ++tag_seed; /* whatever arrives late is not this command's */
    if (m->state != MSC_STATE_READY) {
        m->stage = STAGE_IDLE;
        refuse(m, "it stopped answering while it was being brought up");
        return;
    }
    recover_begin(m, "a transfer never answered");
}

void msc_step(uint32_t elapsed_ms)
{
    int i;

    for (i = 0; i < MSC_MAX_DEVICES; i++) {
        struct msc_device *m = &sticks[i];

        if (!m->used) {
            continue;
        }
        if (m->test_gap != 0) {
            m->test_gap = (m->test_gap > elapsed_ms) ? m->test_gap - elapsed_ms : 0;
        }
        if (m->state == MSC_STATE_TEST && m->stage == STAGE_IDLE) {
            m->ready_left = (m->ready_left > elapsed_ms) ? m->ready_left - elapsed_ms : 0;
        }
        if (step_recovery(m)) {
            /* The deadline still runs while recovery does: a controller that
             * never answers a clear must not leave the stick here for ever. */
            m->deadline = (m->deadline > elapsed_ms) ? m->deadline - elapsed_ms : 0;
            if (m->deadline == 0) {
                int device = m->device;

                klog(KLOG_WARN, "usbdisk", "device %d: recovery never finished", device);
                m->recover = RECOVER_NONE;
                m->stage = STAGE_IDLE;
                m->tag = ++tag_seed;
                serve_end(m, -EIO);
                /* The transport is dead on a device the hub still shows
                 * connected; the core resets its port and enumerates it
                 * again, as a replug would. This entry is detached by the
                 * core's close. */
                usb_core_device_drop(m->hc, device, "its transport stopped answering");
            }
        } else {
            if (m->stage == STAGE_DONE) {
                step_command_done(m);
            }
            if (m->stage != STAGE_IDLE) {
                m->deadline = (m->deadline > elapsed_ms) ? m->deadline - elapsed_ms : 0;
                if (m->deadline == 0) {
                    step_timeout(m);
                }
            } else {
                step_idle(m);
            }
        }
        /* The next request waiting, once the stick is idle: after a command
         * ended, and after a timeout as well, so that nothing submitted from
         * a process in the meantime goes out ahead of it. */
        if (m->stage == STAGE_IDLE && m->recover == RECOVER_NONE && m->request == 0 &&
            m->state == MSC_STATE_READY && m->queue_count != 0) {
            int flush = 0;
            struct block_request *next = queue_take(m, &flush);

            m->request = next;
            m->flushing = flush;
            if ((flush ? send_synchronize(m) : send_read_write(m, next)) != 0) {
                serve_end(m, -EIO);
            }
        }
    }
}

/* ---- the stick as a block device ---- */

/** Put a request at the end of this stick's queue. */
static int queue_put(struct msc_device *m, struct block_request *r, int flush, int lun)
{
    int at;

    if (m->queue_count >= MSC_QUEUE) {
        return -1;
    }
    at = (m->queue_head + m->queue_count) % MSC_QUEUE;
    m->queue[at] = r;
    m->queue_flush[at] = flush;
    m->queue_lun[at] = lun;
    m->queue_count++;
    return 0;
}

/** Take the next request off the queue, or nothing; `lun` becomes its unit. */
static struct block_request *queue_take(struct msc_device *m, int *flush)
{
    struct block_request *r;

    if (m->queue_count == 0) {
        return 0;
    }
    r = m->queue[m->queue_head];
    *flush = m->queue_flush[m->queue_head];
    m->lun = m->queue_lun[m->queue_head];
    m->queue_head = (m->queue_head + 1) % MSC_QUEUE;
    m->queue_count--;
    return r;
}

/** Fail everything waiting, which is what a stick that has gone leaves. */
static void queue_drain(struct msc_device *m, int status)
{
    int flush;
    struct block_request *r;

    while ((r = queue_take(m, &flush)) != 0) {
        block_request_complete(r, r->generation, status);
    }
}

/**
 * Take one request for a stick.
 *
 * The block layer has already put the write's bytes in the request's own
 * page and will take a read's out of it, so this only has to move them. A
 * request that arrives while the stick is busy waits its turn; one for a
 * stick that has gone is refused at once, because the alternative is a
 * process asleep on a device that will never answer.
 */
static int msc_block_submit(block_device_t *device, struct block_request *request)
{
    struct msc_lun *u = (struct msc_lun *)device->drvdata;
    struct msc_device *m = u ? u->owner : 0;
    uint32_t flags;
    int rc = 0;

    if (m == 0 || !m->used || m->state != MSC_STATE_READY || !u->present) {
        return -EIO;
    }
    if (request->write && u->write_protect) {
        return -EROFS;
    }
    if (request->lba + request->count > u->blocks) {
        return -EIO;
    }
    /* This is the one entry from a process, and everything else that
     * touches the stick runs in the pump with interrupts off. The emulator
     * completes a transfer inside the doorbell write, so the interrupt for
     * a wrapper can arrive before the stage that sent it has been recorded;
     * the pump then reads a stick that is idle with a completion for it,
     * and the next wrapper goes out into the middle of the command. */
    flags = kirq_save();
    if (m->request == 0 && m->stage == STAGE_IDLE) {
        m->request = request;
        m->flushing = 0;
        m->lun = u->index;
        if (send_read_write(m, request) != 0) {
            m->request = 0;
            rc = -EIO;
        }
    } else if (queue_put(m, request, 0, u->index) != 0) {
        rc = -EIO;
    }
    kirq_restore(flags);
    return rc;
}

/**
 * Tell the stick to put what it has in its own cache on the medium.
 *
 * A stick with nothing to flush answers `ILLEGAL REQUEST`, which is a stick
 * with no cache and not a failure; anything else is one, and the caller's
 * sync or unmount fails with it.
 */
static int msc_block_flush(block_device_t *device, struct block_request *request)
{
    struct msc_lun *u = (struct msc_lun *)device->drvdata;
    struct msc_device *m = u ? u->owner : 0;
    uint32_t flags;
    int rc = 0;

    if (m == 0 || !m->used || m->state != MSC_STATE_READY || !u->present) {
        return -EIO;
    }
    flags = kirq_save(); /* as in `msc_block_submit` */
    if (m->request == 0 && m->stage == STAGE_IDLE) {
        m->request = request;
        m->flushing = 1;
        m->lun = u->index;
        if (send_synchronize(m) != 0) {
            m->request = 0;
            m->flushing = 0;
            rc = -EIO;
        }
    } else if (queue_put(m, request, 1, u->index) != 0) {
        rc = -EIO;
    }
    kirq_restore(flags);
    return rc;
}

/** Publish this stick to the block layer. */
static void publish(struct msc_device *m, int lun)
{
    struct msc_lun *u = &m->luns[lun];

    u->block.sector_size = u->block_size;
    u->block.sector_count = u->blocks;
    u->block.flags = u->write_protect ? BLOCK_READ_ONLY : 0u;
    u->block.deadline_ticks = MSC_DEADLINE_TICKS;
    u->block.read = 0;
    u->block.write = 0;
    u->block.submit = msc_block_submit;
    u->block.flush = msc_block_flush;
    u->block.backing = 0;
    u->block.drvdata = u;
}

block_device_t *msc_lun_device(int index, int lun)
{
    if (index < 0 || index >= MSC_MAX_DEVICES || lun < 0 || lun >= MSC_MAX_LUNS) {
        return 0;
    }
    if (!sticks[index].used || !sticks[index].luns[lun].present) {
        return 0;
    }
    return &sticks[index].luns[lun].block;
}

block_device_t *msc_block_device(int index)
{
    return msc_lun_device(index, 0);
}
