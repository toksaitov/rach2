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
 * plugged in, takes no next command after a failure until `REQUEST SENSE`
 * has been sent, reports the address of its last block rather than a count,
 * stalls `Get Max LUN` instead of answering it, hides its flash behind a
 * CD-ROM on a promotional drive, and crashes if it is reset at
 * initialization, which is why reset here is recovery and nothing else.
 *
 * What this driver passes over or refuses, by name and in the record: a unit
 * whose blocks are not 512 bytes is passed over, because the block layer's
 * sectors are 512 bytes; a capacity of all ones from `READ CAPACITY(16)`,
 * which is not a capacity, refuses the device, and so does a unit that does
 * not finish initialization within ten seconds. A unit too large for
 * `READ(10)` is driven with the sixteen-byte commands. A stick that offers
 * only UAS never reaches this file: the core finds no interface it has a
 * driver for.
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
#define MSC_REQ_RESET         0xFF
#define MSC_REQ_MAX_LUN       0xFE
#define TYPE_CLASS_OUT        0x21
#define TYPE_CLASS_IN         0xA1
/* A standard request to an endpoint (USB 2.0, 9.3.1): CLEAR_FEATURE of
 * ENDPOINT_HALT clears the device's halt and resets its data toggle. */
#define TYPE_ENDPOINT_OUT     0x02
#define REQ_CLEAR_FEATURE     1
#define FEATURE_ENDPOINT_HALT 0

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

/* Sense keys, of which this driver acts on three. */
#define SENSE_NOT_READY       0x02
#define SENSE_ILLEGAL_REQUEST 0x05
#define SENSE_UNIT_ATTENTION  0x06

/* The stages of one command. */
#define STAGE_IDLE    0
#define STAGE_CBW     1
#define STAGE_DATA    2
#define STAGE_CSW     3
#define STAGE_DONE    4
/* A class request on the control endpoint, which is one transfer and not a
 * wrapper: `Get Max LUN` and the reset are the two. */
#define STAGE_CLASS   5
#define STAGE_FINISH  6 /* the request's transfer retired before it is completed */
#define STAGE_QUIESCE 7 /* a transfer never answered: the endpoints stopped first */

/* What the driver is doing with the stick as a whole. */
#define MSC_STATE_FREE       0
#define MSC_STATE_MAX_LUN    1
#define MSC_STATE_INQUIRY    2
#define MSC_STATE_TEST       3
#define MSC_STATE_SENSE      4
#define MSC_STATE_CAPACITY   5
#define MSC_STATE_MODE       6
#define MSC_STATE_READY      7
#define MSC_STATE_REFUSED    8
#define MSC_STATE_GONE       9
#define MSC_STATE_CAPACITY16 10 /* the ten-byte answer was all ones */
#define MSC_STATE_SERIAL     11 /* INQUIRY's unit serial page, asked once */

/*
 * Recovery, which the class defines as three things in this order: the reset
 * request, the halt cleared on the inward endpoint, then on the outward one
 * (Bulk-Only Transport 1.0, 5.3.4).
 *
 * The controller's half of each clear is a command, and the controller takes
 * one command at a time for the whole stack, so it goes through the core's
 * turn (`usb_core_clear_halt`) rather than being issued here.
 */
#define RECOVER_NONE          0
#define RECOVER_RESET         1
#define RECOVER_CLEAR_IN      2
#define RECOVER_CLEAR_OUT     3
/* The lighter recovery a stalled data or status stage takes: one endpoint
 * cleared, and then the status wrapper read. */
#define RECOVER_CLEAR_ONE     4
/* The control endpoint halted by a class request the stick stalled: it is
 * moved on, and the request's answer stands. */
#define RECOVER_CLEAR_CONTROL 5
/* Each clear is two halves: the controller's endpoint stopped and moved on
 * through the core, then the device's halt cleared with a standard
 * `CLEAR_FEATURE(ENDPOINT_HALT)` request, whose completion moves on. */
#define RECOVER_CLEAR_IN_DEV  6
#define RECOVER_CLEAR_OUT_DEV 7
#define RECOVER_CLEAR_ONE_DEV 8

/* How long one transfer, one recovery step or one stop of the endpoints may
 * take, in ms. */
#define MSC_STEP_MS     10000u
/* How long each logical unit may take to initialize, in ms. */
#define MSC_READY_MS    10000u
/* The pause before TEST UNIT READY is sent again, in ms. */
#define MSC_TEST_GAP_MS 100u

/* The most this driver moves in one command: 512 KiB, whose 129 pieces at
 * most fit the endpoint ring beside the command's wrapper, where the block
 * layer's whole limit would not. */
#define MSC_REQ_SECTORS 1024u
#define MSC_SECTOR_SIZE 512u

/* How many requests may wait for one stick. The block layer has at most
 * sixteen requests live (`REQ_LIMIT`), so a queue of sixteen cannot
 * overflow. */
#define MSC_QUEUE 16

/* How long a request has before the block layer gives up on it, in ticks of
 * the 100 Hz timer. Ten seconds, because a stick may have a controller of its
 * own to wake up. */
#define MSC_DEADLINE_TICKS 1000u

struct msc_device;

/** One logical unit of a stick: what it is and the device it is served as. */
struct msc_lun {
    int present;
    int index; /* its logical unit number */
    struct msc_device *owner;
    uint64_t blocks; /* 512-byte blocks */
    uint32_t block_size;
    int write_protect;
    char serial[64]; /* the unit's serial page, or empty when it has none */
    block_device_t block;
};

/* The longest serial taken from the page: what the record has room for less
 * the terminator, and what the 64-byte answer has after its header. */
#define MSC_SERIAL_MAX 60

struct msc_device {
    int used;
    struct xhci_ctrl *hc; /* the controller this stick is on */
    /* The stick has gone and its record is not free: something still holds
     * the block device, so neither the record nor anything in it may be
     * reused until the disk layer says the last hold is gone. */
    int closing;
    int device;    /* the controller's handle */
    int interface; /* for the two class requests */
    int ep_in;
    int ep_out;

    /* What the driver is doing with the stick, one of the `MSC_STATE_`
     * values; the stage of the command in flight, one of the `STAGE_` values;
     * the unit the next command addresses; and the highest unit number, from
     * Get Max LUN. */
    int state;
    int stage;
    int lun;
    int max_lun;
    int recover;       /* one of the `RECOVER_` values */
    int after_recover; /* the one endpoint a single clear is for */
    /* The command wrapper's tag, which its status wrapper repeats, and the
     * milliseconds left for the transfer or step in flight. */
    uint32_t tag;
    uint32_t deadline;
    uint32_t ready_left; /* milliseconds left to initialize the current unit */
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
    int retried;     /* the request in flight has been sent a second time */
    int csw_retried; /* the command in flight had its status wrapper cleared once */
    int sensing;     /* the command in flight is the REQUEST SENSE for the request */

    /* What the stick is, once it is up. INQUIRY's two strings are kept
     * because `scratch` is the next command's buffer as well. */
    char vendor[9];
    char product[17];
    uint32_t flushes; /* SYNCHRONIZE CACHE commands sent, for /proc/usb */

    /* What the unit being brought up answered; copied into its record. */
    uint64_t blocks;
    uint32_t block_size;
    int write_protect;

    /* The logical units that answered, each with the block device it is
     * published as, whose `drvdata` is the unit's record. */
    struct msc_lun luns[MSC_MAX_LUNS];
    int ready_luns;
    /* The request in flight, and whether it is a flush. Two processes may
     * want one stick, since either can sleep inside a request while the
     * other runs, so the requests are admitted in the order they arrived,
     * wait in `queue`, and are served one at a time, the units sharing the
     * one pipe; a flush waits its turn like a read, which is what puts it
     * after every write before it. */
    struct block_request *request;
    int flushing;
    /* The controller's hold on the data stage's buffer, a request's pages or
     * `scratch`: set when the data transfer naming it is given to the
     * controller, cleared only when the controller driver says the transfer
     * is retired, which a terminal event, a stop or a reset with its
     * dequeue, or Disable Slot answered establishes. A request is completed
     * only after; a record is reclaimed only when the disk layer has let go
     * and the slot has been closed. */
    int owned;
    int slot_closed;
    int disk_released;
    uint32_t data_gen;   /* the data transfer's generation, held in the controller driver */
    uint32_t expect_gen; /* the generation a completion must have */
    int expect_ep;       /* and the endpoint */
    uint32_t data_moved; /* bytes the data stage moved, kept apart from the wrapper's residue */
    int quiescing;       /* a timeout's stop: 1 bulk IN in progress, 2 bulk OUT */
    int finish_status;   /* the status the request is completed with once its transfer is retired */
    int finish_then;     /* 1: REQUEST SENSE follows the retirement, before the completion */
    int finish_command;  /* 1: the command's end is acted on after the retirement */
    struct block_request *queue[MSC_QUEUE];
    int queue_flush[MSC_QUEUE];
    int queue_lun[MSC_QUEUE]; /* the unit each waiting request is for */
    int queue_head;
    int queue_count;
};

/* The driver's table: an entry is a stick from `msc_attach` until the disk
 * layer has let go of it and its slot has been given back. */
static struct msc_device sticks[MSC_MAX_DEVICES];

/* The tag of the last command sent to any stick. Tags go up across every
 * stick and every attach, so a completion left over from a stick that was
 * pulled cannot match a command of the stick plugged in after it on the
 * same controller handle. */
static uint32_t tag_seed;

/* The buffer each stick's control transfers have their setup packet built
 * in. One command is in flight per stick and the class requests are part of
 * that command, so one per stick is enough. */
static uint8_t setup_buffer[MSC_MAX_DEVICES][8];

static struct block_request *queue_take(struct msc_device *m, int *flush);
static int queue_put(struct msc_device *m, struct block_request *r, int flush, int lun);
static int status_begin(struct msc_device *m);
static void command_end(struct msc_device *m, int status);
static void queue_drain(struct msc_device *m, int status);
static void publish(struct msc_device *m, int lun);
static void refuse(struct msc_device *m, const char *why);
static void refuse_to_core(struct msc_device *m, const char *why);

/**
 * The stick in use and not closing at slot `device` of `hc`, or NULL. Slot
 * numbers start at one on every controller, so a machine with two of them
 * has two sticks called slot one and the controller is half of the name.
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
 * As `by_device`, a closing record included until its slot has been given
 * back. The controller may then give the slot to another stick, while a mount
 * still holds the closed record, and the slot's completions are the new
 * stick's.
 */
static struct msc_device *by_device_any(struct xhci_ctrl *hc, int device)
{
    int i;

    for (i = 0; i < MSC_MAX_DEVICES; i++) {
        if (sticks[i].used && !sticks[i].slot_closed && sticks[i].hc == hc &&
            sticks[i].device == device) {
            return &sticks[i];
        }
    }
    return 0;
}

/**
 * Give the controller a transfer under a generation of its own, and keep
 * the generation and the endpoint a completion must have to be this
 * transfer's: a completion for an earlier stage of the same command, on the
 * same endpoint, is told apart by it.
 */
static int submit_expected(struct msc_device *m, int endpoint, struct usb_transfer *transfer)
{
    transfer->generation = ++tag_seed;
    if (xhci_submit(m->hc, m->device, endpoint, transfer) != 0) {
        return -1;
    }
    m->expect_gen = transfer->generation;
    m->expect_ep = endpoint;
    return 0;
}

/**
 * Copy a fixed-width SCSI string out, trimmed of its trailing spaces.
 *
 * INQUIRY pads its two names with spaces to their full width, and a name
 * printed with the padding lines up with nothing. `out` has room for
 * `width` characters and the terminator. A byte that cannot be printed
 * becomes a space, since a stick may put anything in these bytes and they
 * reach a reader of /proc/usb.
 */
static void take_string(char *out, const uint8_t *in, int width)
{
    int i;
    int end = 0;

    for (i = 0; i < width; i++) {
        char c = (char)in[i];

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

/** The 32-bit value at `p`, little-endian as the wrappers have it. */
static uint32_t read32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/** Store `v` at `p`, little-endian. */
static void write32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

/** The 32-bit value at `p`, big-endian as SCSI has it. */
static uint32_t read32_be(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

/** Store `v` at `p`, big-endian. */
static void write32_be(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

/**
 * Start one SCSI command to unit `m->lun`: build its wrapper under a new tag
 * and submit it.
 *
 * `cdb` is the command block, 1 to 16 bytes, `data` the buffer for its data
 * stage, `length` how many bytes that stage moves, zero for a command with
 * none, and `data_in` whether they come in. Returns 0 when the wrapper was
 * submitted, -1 otherwise.
 */
static int command_begin(struct msc_device *m, const uint8_t *cdb, int cdb_length, uint8_t *data,
                         uint32_t length, int data_in)
{
    struct usb_transfer transfer;
    int i;

    m->csw_retried = 0;
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
    m->data_moved = 0;
    m->data_length = length;
    m->data_in = data_in;
    m->residue = 0;
    m->status = -1;

    transfer.setup = 0;
    transfer.buffer = m->cbw;
    transfer.length = CBW_BYTES;
    transfer.direction = USB_DIR_OUT;
    if (submit_expected(m, m->ep_out, &transfer) != 0) {
        return -1;
    }
    m->stage = STAGE_CBW;
    m->deadline = MSC_STEP_MS;
    return 0;
}

/**
 * Submit the data stage of the command in flight; 0 when it was submitted.
 *
 * The buffer is the controller's from here, a request's pages and the
 * record's own scratch alike: the controller driver's record of the transfer
 * is held (`owned`) so the driver can ask when the transfer is retired, before
 * the buffer is used again.
 */
static int data_begin(struct msc_device *m)
{
    struct usb_transfer transfer;

    transfer.setup = 0;
    transfer.buffer = m->data;
    transfer.length = m->data_length;
    transfer.direction = m->data_in ? USB_DIR_IN : USB_DIR_OUT;
    if (submit_expected(m, m->data_in ? m->ep_in : m->ep_out, &transfer) != 0) {
        return -1;
    }
    m->data_gen = transfer.generation;
    m->owned = xhci_td_hold(m->hc, m->device, m->data_gen) == 0;
    m->stage = STAGE_DATA;
    m->deadline = MSC_STEP_MS;
    return 0;
}

/** Submit the status wrapper of the command in flight; 0 when it was
 * submitted. */
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
    if (submit_expected(m, m->ep_in, &transfer) != 0) {
        return -1;
    }
    m->stage = STAGE_CSW;
    m->deadline = MSC_STEP_MS;
    return 0;
}

/**
 * Ask the device to clear the halt on the endpoint with device context index
 * `dci`: the standard request, on the control endpoint, expected like any
 * other transfer. The endpoint address is the number with the direction bit.
 * Returns 0 when it was submitted.
 */
static int send_clear_halt(struct msc_device *m, int dci)
{
    struct usb_transfer transfer;
    int address = (dci / 2) | ((dci & 1) ? 0x80 : 0);

    setup_fill(m, TYPE_ENDPOINT_OUT, REQ_CLEAR_FEATURE, FEATURE_ENDPOINT_HALT, address, 0);
    transfer.setup = setup_buffer[m - sticks];
    transfer.buffer = 0;
    transfer.length = 0;
    transfer.direction = USB_DIR_OUT;
    if (submit_expected(m, 1, &transfer) != 0) {
        return -1;
    }
    m->deadline = MSC_STEP_MS;
    return 0;
}

static void recover_with_code(struct msc_device *m, const char *why, int code);

/**
 * Begin reset recovery, with `why` in the record: the class reset, the halt
 * cleared on the inward endpoint, then on the outward one, in the order the
 * specification gives, and then the command is failed with a phase error.
 *
 * Only the reset is sent here; its completion and `step_recovery` take the
 * rest. A reset that cannot be sent fails the command with a phase error at
 * once.
 */
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
        if (submit_expected(m, 1, &transfer) != 0) {
            m->recover = RECOVER_NONE;
            m->stage = STAGE_DONE;
            m->status = CSW_PHASE_ERROR;
            return;
        }
    }
    m->recover = RECOVER_RESET;
    m->deadline = MSC_STEP_MS;
}

/** As `recover_begin`, with the controller's completion `code` recorded
 * first. */
static void recover_with_code(struct msc_device *m, const char *why, int code)
{
    klog(KLOG_WARN, "usbdisk", "device %d: %s: completion code %d", m->device, why, code);
    recover_begin(m, why);
}

/**
 * Turn the recovery that is waiting on the controller's commands.
 *
 * Returns 1 while recovery is running, 0 when there is none.
 *
 * Each clear has the controller's half first, the endpoint stopped and moved
 * on through the core, then the device's half, a CLEAR_FEATURE(ENDPOINT_HALT)
 * request. A controller half that fails leaves an endpoint that moves nothing
 * again, so the stick is refused and handed to the core, whose port reset
 * brings it back as a new device. The class reset and the device's half of
 * each clear are requests, whose completions `msc_completion` acts on; here
 * they are only waited for.
 */
static int step_recovery(struct msc_device *m)
{
    int answer;

    switch (m->recover) {
    case RECOVER_NONE:
        return 0;

    case RECOVER_RESET:
        return 1;

    case RECOVER_CLEAR_IN:
    case RECOVER_CLEAR_OUT:
    case RECOVER_CLEAR_ONE: {
        int dci = m->recover == RECOVER_CLEAR_IN    ? m->ep_in
                  : m->recover == RECOVER_CLEAR_OUT ? m->ep_out
                                                    : m->after_recover;

        answer = usb_core_clear_halt(m->hc, m->device, dci);
        if (answer == 0) {
            return 1;
        }
        if (answer < 0) {
            refuse_to_core(m, "an endpoint would not clear");
            return 1;
        }
        if (send_clear_halt(m, dci) != 0) {
            refuse_to_core(m, "the clear of a halt could not be sent");
            return 1;
        }
        m->recover = m->recover == RECOVER_CLEAR_IN    ? RECOVER_CLEAR_IN_DEV
                     : m->recover == RECOVER_CLEAR_OUT ? RECOVER_CLEAR_OUT_DEV
                                                       : RECOVER_CLEAR_ONE_DEV;
        return 1;
    }

    case RECOVER_CLEAR_IN_DEV:
    case RECOVER_CLEAR_OUT_DEV:
    case RECOVER_CLEAR_ONE_DEV:
        return 1;

    case RECOVER_CLEAR_CONTROL:
        answer = usb_core_clear_halt(m->hc, m->device, 1);
        if (answer == 0) {
            return 1;
        }
        m->recover = RECOVER_NONE;
        if (answer < 0) {
            refuse_to_core(m, "the control endpoint would not clear");
            return 1;
        }
        command_end(m, CSW_FAILED);
        return 1;

    default:
        m->recover = RECOVER_NONE;
        return 0;
    }
}

/** End the command in flight with `status`, a status wrapper's status byte;
 * `msc_step` acts on it by the stick's state. */
static void command_end(struct msc_device *m, int status)
{
    m->stage = STAGE_DONE;
    m->status = status;
}

static void serve_end(struct msc_device *m, int status);
static int send_request_sense(struct msc_device *m);
static void step_command_done(struct msc_device *m);

/**
 * Complete the request in flight with `status` once its data transfer is
 * retired, and first send REQUEST SENSE when `then_sense`.
 *
 * A transfer is retired when the controller driver says so; a short one
 * on a controller that posts no event for its last block, QEMU's, is
 * retired by a stop and a dequeue of its endpoint, which `step_finish`
 * runs. Until then nothing is completed and no command starts, so the
 * request's pages and the stick's record are never reused while the
 * controller can reach them. The stick is refused when the controller
 * driver has no record of the transfer.
 */
static void serve_finish(struct msc_device *m, int status, int then_sense)
{
    m->finish_status = status;
    m->finish_then = then_sense;
    m->stage = STAGE_FINISH;
    m->deadline = MSC_STEP_MS;
}

/**
 * Turn `serve_finish`'s wait: 1 while it goes on.
 *
 * A command of the bring-up, or one whose end is read by state, has its end
 * acted on once its transfer is retired and its buffer is the driver's
 * again. A served request's end asks for this finish once more, with nothing
 * left to retire, and goes on to the completion in the same pass.
 */
static int step_finish(struct msc_device *m)
{
    if (m->stage != STAGE_FINISH) {
        return 0;
    }
    if (m->owned) {
        int retired = xhci_td_retired(m->hc, m->device, m->data_gen);

        if (retired < 0) {
            refuse_to_core(m, "the controller driver holds no record of its transfer");
            return 1;
        }
        if (retired == 0) {
            int q = usb_core_endpoint_quiesce(m->hc, m->device, m->data_in ? m->ep_in : m->ep_out);

            if (q == 0) {
                return 1;
            }
            if (q < 0) {
                refuse_to_core(m, "its endpoint would not stop");
                return 1;
            }
        }
        xhci_td_release(m->hc, m->device, m->data_gen);
        m->owned = 0;
    }
    if (m->finish_command) {
        m->finish_command = 0;
        m->stage = STAGE_DONE;
        step_command_done(m);
        if (m->stage != STAGE_FINISH) {
            return 0;
        }
    }
    m->stage = STAGE_IDLE;
    if (m->finish_then) {
        m->finish_then = 0;
        m->sensing = 1;
        if (send_request_sense(m) == 0) {
            return 0;
        }
        serve_end(m, -EIO);
        return 0;
    }
    serve_end(m, m->finish_status);
    return 0;
}

/**
 * Turn a timeout's stop of the two bulk endpoints: 1 while it goes on.
 *
 * Bulk IN first, then bulk OUT, each a stop and a dequeue answered with
 * success; only then is the request's transfer retired and the class reset
 * sent. A stop refused refuses the stick with the request kept. A stop
 * answered at once goes on to the next endpoint in the same pass, so a pass
 * never charges its whole elapsed time to a deadline it has just set.
 */
static int step_quiesce(struct msc_device *m)
{
    int q;

    if (m->quiescing == 0) {
        return 0;
    }
    for (;;) {
        q = usb_core_endpoint_quiesce(m->hc, m->device, m->quiescing == 1 ? m->ep_in : m->ep_out);
        if (q == 0) {
            return 1;
        }
        if (q < 0) {
            refuse_to_core(m, "its endpoint would not stop after a transfer never answered");
            return 1;
        }
        if (m->quiescing != 1) {
            break;
        }
        m->quiescing = 2;
        m->deadline = MSC_STEP_MS;
    }
    m->quiescing = 0;
    if (m->owned) {
        xhci_td_release(m->hc, m->device, m->data_gen);
        m->owned = 0;
    }
    m->stage = STAGE_IDLE;
    recover_begin(m, "a transfer never answered");
    return 0;
}

/** Complete the block request in flight, if there is one, with `status`. */
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

/**
 * A request whose pages the controller may still reach is kept until Disable
 * Slot is answered, which `msc_slot_closed` reports; the rest fail now. The
 * record line names the stage the kept request is in, which `tests/run usb`
 * reads: the pull it arranges lands in the data stage, or in the status
 * wrapper when the emulator's drain of the medium ends the data stage before
 * the device goes. The record stays taken: its block device is what a mount,
 * a description and a held buffer point at, and a second stick given this
 * record would be given their pointers with it. The disk layer frees it when
 * the last of those has gone (`msc_release`), and its own buffers are the
 * controller's until the slot has been given back.
 */
void msc_detach(struct xhci_ctrl *hc, int device)
{
    struct msc_device *m = by_device(hc, device);

    if (m == 0) {
        return;
    }
    m->recover = RECOVER_NONE;
    m->quiescing = 0;
    if (!m->owned) {
        request_end(m, -EIO);
    } else if (m->request != 0) {
        klog(KLOG_INFO, "usbdisk", "device %d: its request is kept, its %s in flight", device,
             m->stage == STAGE_DATA ? "data stage" : "status wrapper");
    }
    queue_drain(m, -EIO);
    m->state = MSC_STATE_GONE;
    m->stage = STAGE_IDLE;
    m->closing = 1;
    klog(KLOG_INFO, "usbdisk", "gone: device %d", device);
    usbdisk_left((int)(m - sticks));
}

/**
 * The record line for the failed request is what `tests/run usb` reads: a
 * request kept through the pull is failed here and not at the pull. Found
 * by `by_device_any`, since the record is closing.
 */
void msc_slot_closed(struct xhci_ctrl *hc, int device)
{
    struct msc_device *m = by_device_any(hc, device);

    if (m == 0) {
        return;
    }
    m->slot_closed = 1;
    if (m->request != 0) {
        klog(KLOG_INFO, "usbdisk", "device %d: its request is failed with the slot given back",
             device);
        m->owned = 0;
        request_end(m, -EIO);
    }
    if (m->closing && m->disk_released) {
        k_memset(m, 0, sizeof(*m));
    }
}

/**
 * The disk layer is the one place that knows the teardown count reached
 * zero. The record goes once the controller has let go as well, which
 * Disable Slot's answer says; a slot never given back keeps the record, its
 * request and its pages, and the log says so.
 */
void msc_release(int index)
{
    if (index < 0 || index >= MSC_MAX_DEVICES) {
        return;
    }
    if (!sticks[index].closing) {
        return;
    }
    sticks[index].disk_released = 1;
    if (sticks[index].slot_closed) {
        k_memset(&sticks[index], 0, sizeof(sticks[index]));
    } else {
        klog(KLOG_INFO, "usbdisk", "device %d: the record waits for its slot to be given back",
             sticks[index].device);
    }
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
        out->serial[i] = u->serial[i];
    }
    out->blocks = u->blocks;
    out->flushes = m->flushes;
    out->block_size = u->block_size;
    out->write_protect = u->write_protect;
    out->closing = m->closing;
    out->device = m->device;
    return 0;
}

/**
 * A gone stick's completions are taken and dropped until its slot is given
 * back; after that the controller can post nothing of its own, and a
 * completion naming the slot is a new device's. A completion for a submission
 * this driver is not waiting for, an earlier stage's or one of a command
 * given up on, is dropped and recorded: each submission's generation and
 * endpoint tell it apart.
 *
 * A class reset that fails leaves the control endpoint halted and the stick in
 * a state this driver cannot read, so the stick is handed to the core; one
 * that succeeds is followed by the two clears, since an endpoint left halted
 * moves nothing again. The controller's half of each clear is a command,
 * which `msc_step` turns, so a completion that arrives during it is taken and
 * ignored. When the clears are done the command recovery was for is failed
 * and the device can take the next one.
 *
 * A class request is one transfer, and a stall is an answer of its own: a
 * drive that will not say how many logical units it has, has one. The stall
 * halts the control endpoint, which is moved on before the next transfer on
 * it (xHCI 1.2, 4.8.3). A halted data endpoint is cleared and the status
 * wrapper read, since the stick still has one to give and it says how much of
 * the transfer it did.
 */
int msc_completion(const struct usb_completion *completion)
{
    struct msc_device *m = by_device_any(completion->hc, completion->device);

    if (m == 0 || !m->used) {
        return 0;
    }
    if (completion->endpoint != m->ep_in && completion->endpoint != m->ep_out &&
        completion->endpoint != 1) {
        return 0;
    }
    if (m->closing) {
        return m->slot_closed ? 0 : 1;
    }
    if (completion->generation != m->expect_gen || completion->endpoint != m->expect_ep) {
        klog(KLOG_DEBUG, "usbdisk", "late: device %d: a completion for generation %d", m->device,
             (int)completion->generation);
        return 1;
    }

    if (m->recover == RECOVER_RESET) {
        if (completion->status != USB_XFER_OK) {
            refuse_to_core(m, "the class reset was refused");
            return 1;
        }
        m->recover = RECOVER_CLEAR_IN;
        m->deadline = MSC_STEP_MS;
        return 1;
    }
    if (m->recover == RECOVER_CLEAR_IN_DEV || m->recover == RECOVER_CLEAR_OUT_DEV ||
        m->recover == RECOVER_CLEAR_ONE_DEV) {
        int which = m->recover;

        if (completion->status != USB_XFER_OK) {
            refuse_to_core(m, "the device would not clear its halt");
            return 1;
        }
        if (which == RECOVER_CLEAR_IN_DEV) {
            m->recover = RECOVER_CLEAR_OUT;
            m->deadline = MSC_STEP_MS;
            return 1;
        }
        m->recover = RECOVER_NONE;
        if (which == RECOVER_CLEAR_ONE_DEV) {
            if (status_begin(m) != 0) {
                command_end(m, CSW_PHASE_ERROR);
            }
            return 1;
        }
        command_end(m, CSW_PHASE_ERROR);
        return 1;
    }
    if (m->recover != RECOVER_NONE) {
        return 1;
    }

    switch (m->stage) {
    case STAGE_CLASS:
        m->residue = completion->residual;
        if (completion->status == USB_XFER_STALLED) {
            m->recover = RECOVER_CLEAR_CONTROL;
            m->deadline = MSC_STEP_MS;
            return 1;
        }
        command_end(m, (completion->status == USB_XFER_OK || completion->status == USB_XFER_SHORT)
                           ? CSW_GOOD
                           : CSW_FAILED);
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
        m->data_moved = completion->length > completion->residual
                            ? completion->length - completion->residual
                            : 0;
        if (status_begin(m) != 0) {
            command_end(m, CSW_PHASE_ERROR);
        }
        return 1;

    case STAGE_CSW:
        if (completion->status == USB_XFER_STALLED) {
            if (m->csw_retried) {
                recover_begin(m, "the status wrapper stalled twice");
                return 1;
            }
            m->csw_retried = 1;
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

/* bringing a stick up, and serving it */

/** Send one class request that reads `length` bytes into `data`: the one
 * sent is `Get Max LUN`. Returns 0 when it was submitted. */
static int class_request_in(struct msc_device *m, int request, uint8_t *data, int length)
{
    struct usb_transfer transfer;

    setup_fill(m, TYPE_CLASS_IN, request, 0, m->interface, length);
    transfer.setup = setup_buffer[m - sticks];
    transfer.buffer = data;
    transfer.length = (uint32_t)length;
    transfer.direction = USB_DIR_IN;
    m->tag = ++tag_seed;
    if (submit_expected(m, 1, &transfer) != 0) {
        return -1;
    }
    m->stage = STAGE_CLASS;
    m->data_length = 0;
    m->deadline = MSC_STEP_MS;
    return 0;
}

/** Zero a command block of `length` bytes; each is built by the function
 * that sends it. */
static void cdb_clear(uint8_t *cdb, int length)
{
    int i;

    for (i = 0; i < length; i++) {
        cdb[i] = 0;
    }
}

/** Send TEST UNIT READY, which has no data stage. The `send_` functions
 * return what `command_begin` returns. */
static int send_test_unit_ready(struct msc_device *m)
{
    uint8_t cdb[6];

    cdb_clear(cdb, 6);
    cdb[0] = SCSI_TEST_UNIT_READY;
    return command_begin(m, cdb, 6, 0, 0, 0);
}

/** Send REQUEST SENSE for 18 bytes of fixed-format sense into `scratch`. */
static int send_request_sense(struct msc_device *m)
{
    uint8_t cdb[6];

    cdb_clear(cdb, 6);
    cdb[0] = SCSI_REQUEST_SENSE;
    cdb[4] = 18;
    return command_begin(m, cdb, 6, m->scratch, 18, 1);
}

/** Send INQUIRY for its 36-byte standard answer into `scratch`. */
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
 * with an error, and the serial is left empty. `scratch` is cleared first,
 * so a short answer leaves zeros and not the last command's bytes. */
static int send_inquiry_serial(struct msc_device *m)
{
    uint8_t cdb[6];

    int i;

    for (i = 0; i < (int)sizeof(m->scratch); i++) {
        m->scratch[i] = 0;
    }
    cdb_clear(cdb, 6);
    cdb[0] = SCSI_INQUIRY;
    cdb[1] = 1; /* EVPD */
    cdb[2] = 0x80;
    cdb[4] = 64;
    return command_begin(m, cdb, 6, m->scratch, 64, 1);
}

/** Send READ CAPACITY(10), whose 8-byte answer is the last block's address
 * and the block size. */
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

/** Send MODE SENSE(6) for the 4-byte header, whose byte 2 has the write
 * protect bit. */
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

    block_count_command(); /* reported by /proc/disk: one per READ or WRITE */
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

/** Send SYNCHRONIZE CACHE(10) for the whole unit. */
static int send_synchronize(struct msc_device *m)
{
    uint8_t cdb[10];

    cdb_clear(cdb, 10);
    cdb[0] = SCSI_SYNCHRONIZE;
    return command_begin(m, cdb, 10, 0, 0, 0);
}

/**
 * Give up on this stick, with the reason in the record, and fail its waiting
 * requests. A request whose pages the controller may still reach is kept
 * until the slot is given back, which the core's close of the device, asked
 * for here, brings about; one it cannot reach fails now.
 */
static void refuse(struct msc_device *m, const char *why)
{
    klog(KLOG_WARN, "usbdisk", "refused: device %d: %s", m->device, why);
    m->state = MSC_STATE_REFUSED;
    m->stage = STAGE_IDLE;
    m->recover = RECOVER_NONE;
    m->quiescing = 0;
    if (m->owned && m->request != 0) {
        klog(KLOG_WARN, "usbdisk", "device %d: its request waits for the slot to be given back",
             m->device);
        usb_core_device_drop(m->hc, m->device, why);
    } else {
        request_end(m, -EIO);
    }
    queue_drain(m, -EIO);
}

/**
 * Refuse a stick whose transport is dead and hand it to the core, whose
 * port reset enumerates it again as a replug would. Its waiting requests
 * fail now. The request in flight is left to the core's close of the device:
 * `msc_detach`, reached through `usb_core_device_drop`, fails it at once when
 * the controller cannot reach its pages and otherwise keeps it until the slot
 * is given back.
 */
static void refuse_to_core(struct msc_device *m, const char *why)
{
    int device = m->device;
    struct xhci_ctrl *hc = m->hc;

    klog(KLOG_WARN, "usbdisk", "refused: device %d: %s", device, why);
    m->state = MSC_STATE_REFUSED;
    m->stage = STAGE_IDLE;
    m->recover = RECOVER_NONE;
    m->quiescing = 0;
    if (m->request != 0) {
        klog(KLOG_WARN, "usbdisk", "device %d: its request waits for the slot to be given back",
             device);
    }
    queue_drain(m, -EIO);
    usb_core_device_drop(hc, device, why);
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
 * Act on a command that has ended, by what it was for: one arm per state of
 * the stick, each setting the state whose command `step_idle` sends next.
 *
 * A drive that stalls `Get Max LUN` has one logical unit, which is what most
 * sticks do, and an answer above fifteen is taken as one unit too. A unit
 * that fails INQUIRY, is not a disk, or has blocks other than 512 bytes is
 * passed over; one that will not report its capacity refuses the stick. A
 * drive answers a failed TEST UNIT READY with a sense that says why, and
 * takes no next command until it has been asked for it; a unit with no
 * medium in it (NOT READY, additional sense code 0x3A) is passed over, and
 * any other sense has TEST UNIT READY sent again after MSC_TEST_GAP_MS, until
 * the unit's time runs out. A unit without the serial page answers with a
 * failure, whose sense is read in the same way. A drive that refuses the MODE
 * SENSE page has no write protection to report, which is the ordinary answer
 * and not a failure.
 *
 * For a served request, a check condition is followed, once the request's
 * transfer is retired, by REQUEST SENSE, since the stick takes no next command
 * until it has been asked, and the outcome depends on the sense key: ILLEGAL
 * REQUEST for a SYNCHRONIZE CACHE is a stick with no cache and not a failure,
 * UNIT ATTENTION says the stick was reset or its medium changed and the
 * command is sent once more, and anything else fails the request. A request
 * is good only when the wrapper says so and the data stage moved every byte,
 * since a wrapper claiming no residue over a short data stage is not a whole
 * read.
 */
static void step_command_done(struct msc_device *m)
{
    int status = m->status;

    m->stage = STAGE_IDLE;

    switch (m->state) {
    case MSC_STATE_MAX_LUN:
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
        m->luns[m->lun].serial[0] = '\0';
        m->state = MSC_STATE_SERIAL;
        return;

    case MSC_STATE_SERIAL:
        if (status == CSW_GOOD && m->scratch[1] == 0x80) {
            int len = m->scratch[3];

            if (len > MSC_SERIAL_MAX) {
                klog(KLOG_INFO, "usbdisk",
                     "device %d: lun %d: a serial of %d characters is not taken", m->device, m->lun,
                     len);
            } else {
                take_string(m->luns[m->lun].serial, &m->scratch[4], len);
            }
            m->state = MSC_STATE_TEST;
            return;
        }
        m->state =
            status == CSW_GOOD ? MSC_STATE_TEST : MSC_STATE_SENSE; /* no page: its sense next */
        return;

    case MSC_STATE_TEST:
        if (status == CSW_GOOD) {
            m->state = MSC_STATE_CAPACITY;
            return;
        }
        m->state = MSC_STATE_SENSE;
        return;

    case MSC_STATE_SENSE:
        m->sense_key = (status == CSW_GOOD) ? (m->scratch[2] & 0x0F) : 0;
        if (m->sense_key == SENSE_NOT_READY && m->scratch[12] == 0x3A) {
            next_lun(m, "there is no medium in it");
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
                /* All ones: READ CAPACITY(16) has the last block's address. */
                m->state = MSC_STATE_CAPACITY16;
                return;
            }
            if (size != MSC_SECTOR_SIZE) {
                next_lun(m, "its blocks are not 512 bytes");
                return;
            }
            /* The field is the last block's address; the count is one more. */
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
        m->write_protect = (status == CSW_GOOD) && (m->scratch[2] & 0x80) != 0;
        lun_ready(m);
        return;

    case MSC_STATE_READY:
        if (m->request == 0) {
            return;
        }
        if (m->sensing) {
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
            serve_finish(m, -EIO, 1);
            return;
        }
        serve_finish(m,
                     (status == CSW_GOOD &&
                      (m->flushing || (m->residue == 0 && m->data_moved == m->data_length)))
                         ? 0
                         : -EIO,
                     0);
        return;

    default:
        return;
    }
}

/** Send the command this stick's state calls for; one that cannot be sent is
 * tried again on the next pass. */
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
 * completed. So the request is not completed here. The two bulk endpoints are
 * stopped and their rings moved on first (`step_quiesce`), which takes the
 * transfer back from the controller, and then reset recovery runs; the
 * request is failed when recovery ends, late, which is what the block
 * layer's quarantine is for. A stick that stops answering while it is being
 * brought up is refused.
 */
static void step_timeout(struct msc_device *m)
{
    klog(KLOG_WARN, "usbdisk", "device %d: a transfer never answered", m->device);
    if (m->state != MSC_STATE_READY) {
        m->stage = STAGE_IDLE;
        refuse_to_core(m, "it stopped answering while it was being brought up");
        return;
    }
    m->quiescing = 1;
    m->stage = STAGE_QUIESCE;
    m->deadline = MSC_STEP_MS;
}

/**
 * Charge `elapsed_ms` to the unit being brought up, and refuse the stick
 * through the core when its MSC_READY_MS have run out: 1 when it was refused.
 *
 * Every stage of bring-up counts. Unit zero's time includes Get Max LUN;
 * each later unit's starts at its INQUIRY. The core's close of the device
 * keeps any buffer the controller can reach until the slot is given back.
 */
static int initialization_expired(struct msc_device *m, uint32_t elapsed_ms)
{
    if (m->state == MSC_STATE_FREE || m->state == MSC_STATE_READY ||
        m->state == MSC_STATE_REFUSED || m->state == MSC_STATE_GONE) {
        return 0;
    }
    m->ready_left = (m->ready_left > elapsed_ms) ? m->ready_left - elapsed_ms : 0;
    if (m->ready_left != 0) {
        return 0;
    }
    refuse_to_core(m, "its logical unit did not initialize within ten seconds");
    return 1;
}

/**
 * A command that ended with its data transfer still held has that transfer
 * retired and released before its end is acted on, so no buffer is reused
 * while the controller can reach it. The deadline runs through a stop, a
 * retirement and a recovery as well: past it the stick is refused and handed
 * to the core rather than left here for ever, and the request in flight is
 * kept while the controller may still reach its pages.
 * The next waiting request goes out once the stick is idle, after a command
 * ended and after a timeout alike, so nothing a process submitted meanwhile
 * goes out ahead of it.
 */
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
        if (initialization_expired(m, elapsed_ms)) {
            continue;
        }
        if (m->stage == STAGE_DONE && m->owned) {
            m->finish_command = 1;
            m->stage = STAGE_FINISH;
            m->deadline = MSC_STEP_MS;
        } else if (m->stage == STAGE_DONE) {
            step_command_done(m);
        }
        if (step_quiesce(m) || step_finish(m)) {
            m->deadline = (m->deadline > elapsed_ms) ? m->deadline - elapsed_ms : 0;
            if (m->deadline == 0 && (m->stage == STAGE_QUIESCE || m->stage == STAGE_FINISH)) {
                klog(KLOG_WARN, "usbdisk", "device %d: its endpoint never stopped", m->device);
                m->quiescing = 0;
                refuse_to_core(m, "its endpoint never stopped");
            }
        } else if (step_recovery(m)) {
            m->deadline = (m->deadline > elapsed_ms) ? m->deadline - elapsed_ms : 0;
            if (m->deadline == 0 && m->recover != RECOVER_NONE) {
                klog(KLOG_WARN, "usbdisk", "device %d: recovery never finished", m->device);
                m->recover = RECOVER_NONE;
                m->stage = STAGE_IDLE;
                refuse_to_core(m, "its recovery never finished");
            }
        } else {
            if (m->stage != STAGE_IDLE) {
                m->deadline = (m->deadline > elapsed_ms) ? m->deadline - elapsed_ms : 0;
                if (m->deadline == 0) {
                    step_timeout(m);
                }
            } else {
                step_idle(m);
            }
        }
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

/* the stick as a block device */

/** Put request `r`, a flush or not, for unit `lun` at the end of this
 * stick's queue; -1 when the queue is full. */
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

/** Take the next request off the queue, or nothing; `m->lun` becomes its
 * unit. */
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

/** Complete every waiting request with `status`, as a stick that has gone or
 * been refused leaves them. */
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
 * process asleep on a device that will never answer. A write to a
 * write-protected unit fails with -EROFS, and a request past the unit's end,
 * or one the full queue has no room for, with -EIO.
 *
 * This and `msc_block_flush` are the entries from a process, and everything
 * else that touches the stick runs in the pump with interrupts off, so these
 * run with them off too. The emulator completes a transfer inside the
 * doorbell write, so without that the interrupt for a wrapper could arrive
 * before the stage that sent it was recorded, the pump would find an idle
 * stick with a completion for it, and the next wrapper would go out into the
 * middle of the command.
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
 * sync or unmount fails with it. Interrupts are off for the reason
 * `msc_block_submit` gives.
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
    flags = kirq_save();
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

/** Fill in the block device that unit `lun` is published to the block layer
 * as. */
static void publish(struct msc_device *m, int lun)
{
    struct msc_lun *u = &m->luns[lun];

    u->block.sector_size = u->block_size;
    u->block.sector_count = u->blocks;
    u->block.flags = u->write_protect ? BLOCK_READ_ONLY : 0u;
    u->block.deadline_ticks = MSC_DEADLINE_TICKS;
    u->block.max_sectors = MSC_REQ_SECTORS; /* a 512 KiB request as one command */
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
