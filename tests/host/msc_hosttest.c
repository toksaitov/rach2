/*
 * msc_hosttest.c: the mass-storage class driver against a model of a stick.
 *
 * `msc.c` is included rather than linked, so that the model can reach the
 * driver's own static state between rows and put it back.
 *
 * A flash drive is the one device in this kernel that can be taken out of the
 * machine in the middle of a transfer, and the states that follow from that
 * cannot be produced on demand anywhere else: a drive that fails `TEST UNIT
 * READY` a dozen times, one that stalls `Get Max LUN`, one that hides its
 * flash behind a CD-ROM, one that reports the wrong tag in its status
 * wrapper, and one that is pulled between the command and its answer. So the
 * driver is compiled natively and given a model of a Bulk-Only device
 * through the two calls it reaches the controller by.
 *
 * The model is a SCSI target with a medium and a set of knobs, one per case
 * the driver must answer. It completes each transfer as the driver submits
 * it, and `pump` below hands the completion back the way the USB core's pump
 * does, so the model sees the stages in the order the driver puts them.
 */

extern int printf(const char *fmt, ...);

#include "../../rach2/sys/usb/msc.h"

#include "../../rach2/sys/block/block.h"
#include "../../rach2/sys/usb/hcd.h"

/* ---- the functions outside msc.c that it calls ---- */

/* Messages that a record waits for its slot to be given back. */
static int waits_logged;

/** Return 1 when `text` contains `words`. */
static int has_words(const char *text, const char *words)
{
    int i, j;

    for (i = 0; text[i] != 0; i++) {
        for (j = 0; words[j] != 0 && text[i + j] == words[j]; j++) {
        }
        if (words[j] == 0) {
            return 1;
        }
    }
    return 0;
}

/** Count the messages that a record waits for its slot. */
void klog(int level, const char *subsystem, const char *format, ...)
{
    (void)level;
    (void)subsystem;
    if (has_words(format, "waits for its slot")) {
        waits_logged++;
    }
}

#include "../../rach2/sys/lib/kstring.h"

/** Fill `length` bytes at `dst` with `value`, as the kernel's `k_memset`. */
void *k_memset(void *dst, int value, size_t length)
{
    unsigned char *p = (unsigned char *)dst;
    size_t i;

    for (i = 0; i < length; i++) {
        p[i] = (unsigned char)value;
    }
    return dst;
}

/* ---- the model ---- */

#define MEDIUM_SECTORS 2048u
#define SECTOR         512u

/* What the model answers, one knob per case a row needs. */
static struct {
    int attached; /* the device is on the bus */
    int max_lun;  /* what Get Max LUN answers */
    /* Get Max LUN is stalled. */
    int stall_max_lun;
    int lun_type[4];           /* the SCSI device type of each logical unit */
    const char *lun_serial[4]; /* each unit's serial page, NULL for the default */
    int no_serial_page;        /* INQUIRY of page 0x80 fails */
    int lun_ready_after[4];    /* how many TEST UNIT READY it fails first */
    int lun_no_medium[4];      /* its sense says the medium is absent */
    /* TEST UNIT READY commands seen, per unit. */
    int tests_seen[4];
    uint32_t last_block; /* what READ CAPACITY reports */
    /* The block size READ CAPACITY reports; a knob that no answer reads, the
     * model never stalls MODE SENSE; and whether MODE SENSE reports write
     * protection. */
    uint32_t block_size;
    int mode_sense_stalls;
    int write_protect;
    int sync_illegal;   /* SYNCHRONIZE CACHE answers ILLEGAL REQUEST */
    int sync_fails;     /* and this one fails outright */
    int fail_next_data; /* the next data stage fails with a check condition */
    int stall_stage;    /* which stage stalls: 1 wrapper, 2 data, 3 status */
    int stall_times;    /* how many times it does */
    int bad_signature;  /* the next status wrapper has the wrong signature */
    /* The next status wrapper has the wrong tag. */
    int bad_tag;
    int bad_status; /* a status byte outside 0 to 2 */
    /* The next status is 2, a phase error; the next status wrapper is 9 bytes
     * where 13 were asked for. */
    int phase_error;
    int short_status;
    int residue_over;    /* a residue larger than the transfer */
    int residue;         /* a residue this many bytes on the next command */
    int vanish_at;       /* the stage at which the device stops answering */
    int sense_key;       /* what the next REQUEST SENSE answers with, or 0 */
    int failed;          /* a command failed and its sense is owed */
    int senses;          /* REQUEST SENSE commands seen */
    uint32_t last_tag;   /* the tag of the last wrapper taken */
    int resets;          /* class resets seen */
    int reset_never;     /* the class reset is never answered */
    int reset_stalls;    /* the class reset is stalled */
    int dev_clears;      /* CLEAR_FEATURE(ENDPOINT_HALT) requests seen */
    int devclear_stalls; /* and they are stalled */
    /* The halts the core cleared on each endpoint, and the endpoints cleared,
     * in order, the first 8 of `clears`. */
    int cleared_in, cleared_out;
    int clear_order[8];
    int clears;
    int capacity_lun; /* the unit whose capacity was asked for */
    unsigned char medium[MEDIUM_SECTORS * SECTOR];
} dev;

/* Whether a completion is owed to the driver, and the completion. */
static struct {
    int pending;
    struct usb_completion completion;
} wire;

/* The command the model is part way through: the fields of the command
 * block wrapper it read, the bytes its data stage left unmoved, and whether
 * the data stage is done. */
static struct {
    int have_cbw;
    uint32_t tag;
    uint32_t length;
    int data_in;
    int lun;
    unsigned char cdb[16];
    int status; /* what its status wrapper will say */
    uint32_t residue;
    int data_done;
} cmd;

static int fails;

#define CHECK(what, cond)                 \
    do {                                  \
        if (!(cond)) {                    \
            printf("  FAIL: %s\n", what); \
            fails++;                      \
        }                                 \
    } while (0)

/** Read a little-endian 32-bit value, as the Bulk-Only wrappers have. */
static uint32_t rd32(const unsigned char *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/** Write a little-endian 32-bit value. */
static void wr32(unsigned char *p, uint32_t v)
{
    p[0] = (unsigned char)v;
    p[1] = (unsigned char)(v >> 8);
    p[2] = (unsigned char)(v >> 16);
    p[3] = (unsigned char)(v >> 24);
}

/** Read a big-endian 32-bit value, as SCSI command blocks have. */
static uint32_t rd32_be(const unsigned char *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

/** Write a big-endian 32-bit value. */
static void wr32_be(unsigned char *p, uint32_t v)
{
    p[0] = (unsigned char)(v >> 24);
    p[1] = (unsigned char)(v >> 16);
    p[2] = (unsigned char)(v >> 8);
    p[3] = (unsigned char)v;
}

/* The endpoint DCIs this model answers on, as the core gives them: the
 * control endpoint is 1, and the bulk pair is 3 and 4. */
#define EP_CONTROL 1
#define EP_IN      3
#define EP_OUT     4

/* The controller driver's transfer records, as the stick driver asks about
 * them: a transfer is in flight from its submission until the wire answers it
 * whole, a stop retires every transfer of the endpoint, and a Disable Slot
 * retires them all. A record retired and not held by the driver is free. A
 * transfer or a completed request whose buffer an unretired record still
 * names counts in `pages_reused`. */
static struct {
    uint32_t gen;
    int endpoint;
    int used;
    int retired;
    int held;
    const unsigned char *buffer;
} tds[8];
/* The quiesces asked for, and their endpoints in order, the first 8. */
static int quiesces;
static int quiesce_order[8];
static int quiesce_answer = 1; /* what usb_core_endpoint_quiesce answers once it is asked */
static int quiesce_answer_for[8] = {1, 1, 1, 1, 1, 1, 1, 1}; /* per endpoint, 2 = the general one */
static int clear_answer = 1;                                 /* what usb_core_clear_halt answers */
static int keep_data_td;         /* the data stage is answered but its record stays in flight */
static int csw_residue_zero;     /* the next status wrapper claims no residue whatever moved */
static int pages_reused;         /* a buffer completed or submitted again with a record in flight */
static int completions_at_reset; /* how many requests had completed when the class reset went out */

/* The order of what the driver asked of the core and the wire, as codes:
 * ORDER_QUIESCE plus the endpoint, ORDER_CLEAR plus the endpoint, the class
 * reset, and a command wrapper; `orders` counts past the 32 recorded. */
#define ORDER_QUIESCE  10
#define ORDER_RESET    20
#define ORDER_CLEAR    30
#define ORDER_WRAPPER  40
#define ORDER_DEVCLEAR 50 /* the device's CLEAR_FEATURE(ENDPOINT_HALT), plus the endpoint */
static int order[32];
static int orders;

/** Record `code` as the next thing the driver asked for. */
static void order_note(int code)
{
    if (orders < 32) {
        order[orders] = code;
    }
    orders++;
}

/** Return 1 when the order recorded is exactly the `n` codes of `want`. */
static int order_is(const int *want, int n)
{
    int i;

    if (orders != n) {
        return 0;
    }
    for (i = 0; i < n; i++) {
        if (order[i] != want[i]) {
            return 0;
        }
    }
    return 1;
}

/** Return 1 when a record not yet retired names `buffer`. */
static int td_names(const unsigned char *buffer)
{
    int i;

    for (i = 0; i < 8; i++) {
        if (tds[i].used && !tds[i].retired && tds[i].buffer == buffer) {
            return 1;
        }
    }
    return 0;
}

/** Take a free record for a transfer on `endpoint`; a retired record the
 * driver still holds stays the driver's until it releases it. */
static void td_track(int endpoint, uint32_t gen, const unsigned char *buffer)
{
    int i;

    for (i = 0; i < 8; i++) {
        if (!tds[i].used || (tds[i].retired && !tds[i].held)) {
            tds[i].used = 1;
            tds[i].gen = gen;
            tds[i].endpoint = endpoint;
            tds[i].retired = 0;
            tds[i].held = 0;
            tds[i].buffer = buffer;
            return;
        }
    }
}
/** Retire every record, as a Disable Slot does. */
static void td_retire_all(void)
{
    int i;

    for (i = 0; i < 8; i++) {
        if (tds[i].used) {
            tds[i].retired = 1;
        }
    }
}
/** Retire the record of the transfer numbered `gen`. */
static void td_retire_gen(uint32_t gen)
{
    int i;

    for (i = 0; i < 8; i++) {
        if (tds[i].used && tds[i].gen == gen) {
            tds[i].retired = 1;
        }
    }
}
/** Retire every record on `endpoint`, as a stop and a dequeue do. */
static void td_retire_endpoint(int endpoint)
{
    int i;

    for (i = 0; i < 8; i++) {
        if (tds[i].used && tds[i].endpoint == endpoint) {
            tds[i].retired = 1;
        }
    }
}
/** Return 1 when a record the driver holds is not yet retired. */
static int td_in_flight(void)
{
    int i;

    for (i = 0; i < 8; i++) {
        if (tds[i].used && !tds[i].retired && tds[i].held) {
            return 1;
        }
    }
    return 0;
}
/** Mark the record of transfer `generation` held, or return -1 for none. */
int xhci_td_hold(struct xhci_ctrl *hc, int device, uint32_t generation)
{
    int i;

    (void)hc;
    (void)device;
    for (i = 0; i < 8; i++) {
        if (tds[i].used && tds[i].gen == generation) {
            tds[i].held = 1;
            return 0;
        }
    }
    return -1;
}
/** Return whether the record of transfer `generation` is retired, or -1. */
int xhci_td_retired(struct xhci_ctrl *hc, int device, uint32_t generation)
{
    int i;

    (void)hc;
    (void)device;
    for (i = 0; i < 8; i++) {
        if (tds[i].used && tds[i].gen == generation) {
            return tds[i].retired;
        }
    }
    return -1;
}
/** Release the driver's hold on a record, which frees it once retired. */
void xhci_td_release(struct xhci_ctrl *hc, int device, uint32_t generation)
{
    int i;

    (void)hc;
    (void)device;
    for (i = 0; i < 8; i++) {
        if (tds[i].used && tds[i].gen == generation) {
            tds[i].held = 0;
            if (tds[i].retired) {
                tds[i].used = 0;
            }
        }
    }
}
/**
 * Record a quiesce of `endpoint` and answer it from `quiesce_answer_for`, or
 * from `quiesce_answer` where that holds 2. An answer of 1, done, retires
 * the endpoint's records.
 */
int usb_core_endpoint_quiesce(struct xhci_ctrl *hc, int device, int endpoint)
{
    int answer =
        quiesce_answer_for[endpoint & 7] == 2 ? quiesce_answer : quiesce_answer_for[endpoint & 7];

    (void)hc;
    (void)device;
    if (quiesces < 8) {
        quiesce_order[quiesces] = endpoint;
    }
    quiesces++;
    order_note(ORDER_QUIESCE + endpoint);
    if (answer == 1) {
        td_retire_endpoint(endpoint);
    }
    return answer;
}

static int stage_of_last; /* the stage of the transfer being answered */

/* The block layer's side, recorded rather than driven: the completions, and
 * the status and request of the last. */
static struct {
    int completions;
    int last_status;
    struct block_request *last;
} block;

/**
 * Owe the driver the answer to the transfer it submitted. A transfer that
 * answered whole or short is retired by its event, unless `keep_data_td`
 * keeps a data stage's record; a stalled or failed one is the controller's
 * until its endpoint is reset and moved on, which `usb_core_clear_halt` does.
 */
static void answer(int endpoint, int status, uint32_t length, uint32_t residual,
                   uint32_t generation)
{
    wire.pending = 1;
    wire.completion.device = 1;
    wire.completion.endpoint = endpoint;
    wire.completion.status = status;
    wire.completion.length = length;
    wire.completion.residual = residual;
    wire.completion.generation = generation;
    if ((status == USB_XFER_OK || status == USB_XFER_SHORT) &&
        !(keep_data_td && endpoint != EP_CONTROL && stage_of_last == 2)) {
        td_retire_gen(generation);
    }
}

/** Return the stage a transfer on `endpoint` is: 0 control, 1 the command
 * wrapper, 2 data, 3 the status wrapper. */
static int stage_of(int endpoint, const struct usb_transfer *t)
{
    if (endpoint == EP_CONTROL) {
        return 0;
    }
    if (endpoint == EP_OUT && t->length == 31u) {
        return 1;
    }
    if (endpoint == EP_IN && t->length == 13u) {
        return 3;
    }
    return 2;
}

/* READ(10) counts that disagree with the transfer's length, which on a stick
 * would end with a residue. */
static int command_mismatches;

/** Answer the data stage of the command in flight with `length` bytes into
 * `buffer`, as a SCSI target would. */
static void scsi_data_in(unsigned char *buffer, uint32_t length)
{
    uint32_t i;

    for (i = 0; i < length; i++) {
        buffer[i] = 0;
    }
    switch (cmd.cdb[0]) {
    case 0x12:                /* INQUIRY */
        if (cmd.cdb[1] & 1) { /* the unit serial page, 0x80 */
            const char *serial = dev.lun_serial[cmd.lun] ? dev.lun_serial[cmd.lun] : "MODEL01";
            uint32_t n = 0;

            while (serial[n]) {
                n++;
            }
            buffer[1] = 0x80;
            buffer[3] = (unsigned char)n;
            for (i = 0; i < n && 4 + i < length; i++) {
                buffer[4 + i] = (unsigned char)serial[i];
            }
            return;
        }
        buffer[0] = (unsigned char)dev.lun_type[cmd.lun];
        buffer[4] = 31;
        return;
    case 0x03: /* REQUEST SENSE */
        dev.senses++;
        buffer[0] = 0x70;
        if (dev.lun_no_medium[cmd.lun]) {
            buffer[2] = 0x02;  /* not ready */
            buffer[12] = 0x3A; /* medium not present */
        } else if (dev.sense_key != 0) {
            buffer[2] = (unsigned char)dev.sense_key;
        } else if (dev.sync_illegal && dev.failed) {
            buffer[2] = 0x05; /* illegal request: no cache to synchronize */
            buffer[12] = 0x20;
        } else if (dev.failed) {
            buffer[2] = 0x03; /* medium error */
            buffer[12] = 0x11;
        } else {
            buffer[2] = 0x02;
            buffer[12] = 0x04; /* not ready, becoming ready */
        }
        dev.failed = 0;
        return;
    case 0x25:                      /* READ CAPACITY */
        dev.capacity_lun = cmd.lun; /* which unit the driver settled on */
        wr32_be(&buffer[0], dev.last_block);
        wr32_be(&buffer[4], dev.block_size);
        return;
    case 0x1A: /* MODE SENSE(6) */
        buffer[0] = 3;
        buffer[2] = (unsigned char)(dev.write_protect ? 0x80 : 0x00);
        return;
    case 0x28: { /* READ(10) */
        uint32_t lba = rd32_be(&cmd.cdb[2]);
        uint32_t n = length / SECTOR;

        if ((((uint32_t)cmd.cdb[7] << 8) | cmd.cdb[8]) != n) {
            command_mismatches++;
        }

        for (i = 0; i < n && lba + i < MEDIUM_SECTORS; i++) {
            unsigned int b;

            for (b = 0; b < SECTOR; b++) {
                buffer[i * SECTOR + b] = dev.medium[(lba + i) * SECTOR + b];
            }
        }
        return;
    }
    default:
        return;
    }
}

/** Take the data stage of the command in flight, `length` bytes, from
 * `buffer`: a WRITE(10) is written to the medium. */
static void scsi_data_out(const unsigned char *buffer, uint32_t length)
{
    if (cmd.cdb[0] == 0x2A) { /* WRITE(10) */
        uint32_t lba = rd32_be(&cmd.cdb[2]);
        uint32_t n = length / SECTOR;
        uint32_t i;

        for (i = 0; i < n && lba + i < MEDIUM_SECTORS; i++) {
            unsigned int b;

            for (b = 0; b < SECTOR; b++) {
                dev.medium[(lba + i) * SECTOR + b] = buffer[i * SECTOR + b];
            }
        }
    }
}

/** Return the status the wrapper of the command in flight will give: 0 for
 * success, 1 for a failed command, whose sense is then owed. */
static int scsi_status(void)
{
    switch (cmd.cdb[0]) {
    case 0x12: /* INQUIRY, whose serial page a stick may not have */
        if ((cmd.cdb[1] & 1) && dev.no_serial_page) {
            dev.sense_key = 0x05; /* illegal request */
            return 1;
        }
        return 0;
    case 0x00: /* TEST UNIT READY */
        dev.tests_seen[cmd.lun]++;
        if (dev.lun_no_medium[cmd.lun]) {
            return 1;
        }
        return dev.tests_seen[cmd.lun] > dev.lun_ready_after[cmd.lun] ? 0 : 1;
    case 0x35: /* SYNCHRONIZE CACHE */
        if (dev.sync_illegal || dev.sync_fails) {
            dev.failed = 1;
            return 1;
        }
        return 0;
    case 0x28:
    case 0x2A:
        if (dev.fail_next_data > 0) {
            dev.fail_next_data--;
            dev.failed = 1;
            return 1;
        }
        return 0;
    default:
        return 0;
    }
}

/* The one controller. The model stands for the driver, so its record is a
 * token that is never dereferenced. */
static struct xhci_ctrl *the_ctrl = (struct xhci_ctrl *)1;

/** Return 1: there is one controller. */
int xhci_controllers(void)
{
    return 1;
}

/** Return the one controller for `n` 0, else 0. */
struct xhci_ctrl *xhci_controller(int n)
{
    return n == 0 ? the_ctrl : 0;
}

/** Return 0 for the one controller, else -1. */
int xhci_number(const struct xhci_ctrl *hc)
{
    return hc == the_ctrl ? 0 : -1;
}

static uint32_t largest_length; /* the longest transfer submitted */

/**
 * Take a transfer and answer it as the stick would, through the knobs of
 * `dev`; return -1 when the stick is not attached. A stick pulled at the
 * transfer's stage leaves it unanswered, which the driver's deadline is
 * for. The control endpoint answers Get Max LUN, the class reset and
 * CLEAR_FEATURE(ENDPOINT_HALT); the bulk pair carries the wrappers and the
 * data stage.
 */
int xhci_submit(struct xhci_ctrl *hc, int device, int endpoint, const struct usb_transfer *transfer)
{
    int stage = stage_of(endpoint, transfer);

    (void)device;
    if (!dev.attached) {
        return -1;
    }
    if (td_names(transfer->buffer)) {
        pages_reused++;
    }
    if (transfer->length > largest_length) {
        largest_length = transfer->length;
    }
    td_track(endpoint, transfer->generation, transfer->buffer);
    stage_of_last = stage;
    if (stage == 1) {
        order_note(ORDER_WRAPPER);
    }
    if (dev.vanish_at != 0 && dev.vanish_at == stage) {
        return 0;
    }
    if (dev.stall_times > 0 && dev.stall_stage == stage) {
        dev.stall_times--;
        answer(endpoint, USB_XFER_STALLED, transfer->length, transfer->length,
               transfer->generation);
        return 0;
    }

    if (endpoint == EP_CONTROL) {
        const unsigned char *setup = transfer->setup;

        if (setup != 0 && setup[1] == 0xFE) { /* Get Max LUN */
            if (dev.stall_max_lun) {
                answer(endpoint, USB_XFER_STALLED, transfer->length, transfer->length,
                       transfer->generation);
                return 0;
            }
            transfer->buffer[0] = (unsigned char)dev.max_lun;
            answer(endpoint, USB_XFER_OK, transfer->length, 0, transfer->generation);
            return 0;
        }
        if (setup != 0 && setup[1] == 0xFF) { /* the class reset */
            dev.resets++;
            order_note(ORDER_RESET);
            completions_at_reset = block.completions;
            if (dev.reset_never) {
                return 0; /* submitted, and nothing ever answers it */
            }
            if (dev.reset_stalls) {
                answer(endpoint, USB_XFER_STALLED, 0, 0, transfer->generation);
                return 0;
            }
            answer(endpoint, USB_XFER_OK, 0, 0, transfer->generation);
            return 0;
        }
        if (setup != 0 && setup[0] == 0x02 && setup[1] == 1) { /* CLEAR_FEATURE on an endpoint */
            int dci = (setup[4] & 0x0F) * 2 + ((setup[4] & 0x80) ? 1 : 0);

            dev.dev_clears++;
            order_note(ORDER_DEVCLEAR + dci);
            answer(endpoint, dev.devclear_stalls ? USB_XFER_STALLED : USB_XFER_OK, 0, 0,
                   transfer->generation);
            return 0;
        }
        answer(endpoint, USB_XFER_OK, transfer->length, 0, transfer->generation);
        return 0;
    }

    if (stage == 1) { /* the command block wrapper */
        const unsigned char *w = transfer->buffer;
        int i;

        if (rd32(&w[0]) != 0x43425355u) {
            answer(endpoint, USB_XFER_FAILED, transfer->length, 0, transfer->generation);
            return 0;
        }
        cmd.have_cbw = 1;
        cmd.tag = rd32(&w[4]);
        dev.last_tag = cmd.tag;
        cmd.length = rd32(&w[8]);
        cmd.data_in = (w[12] & 0x80) != 0;
        cmd.lun = w[13];
        for (i = 0; i < 16; i++) {
            cmd.cdb[i] = w[15 + i];
        }
        cmd.residue = 0;
        cmd.data_done = 0;
        answer(endpoint, USB_XFER_OK, transfer->length, 0, transfer->generation);
        return 0;
    }

    if (stage == 2) { /* the data stage */
        uint32_t moved = transfer->length;

        if (dev.residue != 0) {
            moved = (transfer->length > dev.residue) ? transfer->length - dev.residue : 0;
            cmd.residue = dev.residue;
            dev.residue = 0;
        }
        if (cmd.data_in) {
            scsi_data_in(transfer->buffer, moved);
        } else {
            scsi_data_out(transfer->buffer, moved);
        }
        cmd.data_done = 1;
        answer(endpoint, moved == transfer->length ? USB_XFER_OK : USB_XFER_SHORT, transfer->length,
               transfer->length - moved, transfer->generation);
        return 0;
    }

    /* the status wrapper */
    {
        unsigned char *w = transfer->buffer;
        int status = scsi_status();

        wr32(&w[0], dev.bad_signature ? 0x55534253u : 0x53425355u);
        wr32(&w[4], dev.bad_tag ? cmd.tag + 1u : cmd.tag);
        wr32(&w[8], dev.residue_over ? cmd.length + 512u : (csw_residue_zero ? 0u : cmd.residue));
        csw_residue_zero = 0;
        w[12] = (unsigned char)(dev.bad_status ? 9 : (dev.phase_error ? 2 : status));
        dev.bad_signature = 0;
        dev.bad_tag = 0;
        dev.bad_status = 0;
        dev.phase_error = 0;
        dev.residue_over = 0;
        answer(endpoint, USB_XFER_OK, 13u, dev.short_status ? 4u : 0u, transfer->generation);
        dev.short_status = 0;
        return 0;
    }
}

static int drops; /* devices handed back to the core */

/** Count a stick the driver handed back to the core. */
void usb_core_device_drop(struct xhci_ctrl *hc, int device, const char *why)
{
    (void)hc;
    (void)device;
    (void)why;
    drops++;
}

/**
 * Clear a halt on `endpoint` and answer `clear_answer`, recording the
 * endpoint in order. The core clears a halt for the class driver, because
 * the controller takes one command at a time for the whole stack. The
 * Bulk-Only specification clears the inward endpoint first, and a driver that
 * cleared them the other way round leaves a device that answers nothing.
 */
int usb_core_clear_halt(struct xhci_ctrl *hc, int device, int endpoint)
{
    (void)hc;
    (void)device;
    if (dev.clears < 8) {
        dev.clear_order[dev.clears] = endpoint;
    }
    dev.clears++;
    order_note(ORDER_CLEAR + endpoint);
    if (endpoint == EP_IN) {
        dev.cleared_in++;
    } else {
        dev.cleared_out++;
    }
    if (clear_answer == 1) {
        td_retire_endpoint(endpoint); /* the reset and the dequeue retire them */
    }
    return clear_answer;
}

/*
 * The disk layer's side. The class driver tells it that a stick is up and
 * that one has gone; here the two are counted, because the rows check that
 * a stick that never became ready is never announced and that one that goes
 * is announced exactly once.
 */
static int arrivals;
static int departures;

/** Count a unit announced to the disk layer. */
void usbdisk_arrived(int slot, int lun)
{
    (void)slot;
    (void)lun;
    arrivals++;
}

/** Count a stick the disk layer is told has gone. */
void usbdisk_left(int slot)
{
    (void)slot;
    departures++;
}

/** Return 0: no interrupt comes in the middle of a row. */
uint32_t kirq_save(void)
{
    return 0;
}

/** Do nothing, as `kirq_save` does. */
void kirq_restore(uint32_t flags)
{
    (void)flags;
}

/* The flushes the drivers count for /proc/disk; a stick's SYNCHRONIZE CACHE
 * is one of them. */
static int flushes_counted;

/** Count a flush. */
void block_count_flush(void)
{
    flushes_counted++;
}

/* The data commands the driver counts for /proc/disk, one per READ or
 * WRITE sent. */
static int commands_counted;

/** Count a data command. */
void block_count_command(void)
{
    commands_counted++;
}

/** Record a completed request, and mark it done with `status`. */
void block_request_complete(struct block_request *request, uint32_t generation, int status)
{
    (void)generation;
    block.completions++;
    block.last_status = status;
    block.last = request;
    if (request != 0 && td_names(request->page)) {
        pages_reused++;
    }
    if (request != 0) {
        request->status = status;
        request->state = BLOCK_REQ_DONE;
    }
}

#include "../../rach2/sys/usb/msc.c"

/* ---- the rows' view of the driver's table ---- */

/** How many sticks are ready to serve requests. */
static int msc_count(void)
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

/** Whether entry `index` holds a stick that is serving requests. */
static int msc_ready(int index)
{
    return sticks[index].used && !sticks[index].closing && sticks[index].state == MSC_STATE_READY;
}

/** As `msc_lun_ident`, for logical unit 0. */
static int msc_ident(int index, struct msc_ident *out)
{
    return msc_lun_ident(index, 0, out);
}

/** As `msc_lun_device`, for logical unit 0. */
static block_device_t *msc_block_device(int index)
{
    return msc_lun_device(index, 0);
}

/* ---- the pump, and the rows ---- */

/**
 * Turn the pump once: hand the driver the completion the model owes, then
 * step it by `elapsed_ms`.
 *
 * The USB core's pump does the same, in this order, which is what makes a
 * command's next stage go out in the same pass as the answer to the one
 * before it.
 */
static void pump(uint32_t elapsed_ms)
{
    if (wire.pending) {
        struct usb_completion c = wire.completion;

        wire.pending = 0;
        msc_completion(&c);
    }
    msc_step(elapsed_ms);
}

/** Turn the pump, a millisecond a turn, until the stick is ready or the
 * turns run out; return 1 when it is ready. */
static int run_until_settled(int turns)
{
    int i;

    for (i = 0; i < turns; i++) {
        pump(1);
        if (msc_count() == 1) {
            return 1;
        }
    }
    return 0;
}

/** Answer the core's Disable Slot of `device`: retire every record and tell
 * the driver. */
static void close_slot(int device)
{
    td_retire_all();
    msc_slot_closed(0, device);
}

/**
 * Reset the model to a ready stick of one unit with 2048 sectors of 512 bytes
 * and no knob set, and reset the driver's own static state, which outlives a
 * row. A stick that goes keeps its slot until the disk layer says nothing
 * holds it, so the reset releases each slot as that layer would.
 */
static void model_reset(void)
{
    int i;

    for (i = 0; i < (int)sizeof(dev); i++) {
        ((unsigned char *)&dev)[i] = 0;
    }
    for (i = 0; i < (int)sizeof(cmd); i++) {
        ((unsigned char *)&cmd)[i] = 0;
    }
    dev.attached = 1;
    dev.max_lun = 0;
    dev.block_size = SECTOR;
    dev.last_block = MEDIUM_SECTORS - 1;
    for (i = 0; i < 4; i++) {
        dev.lun_type[i] = 0x00;
    }
    for (i = 0; i < (int)sizeof(dev.medium); i++) {
        dev.medium[i] = (unsigned char)(i & 0xFF);
    }
    dev.sense_key = 0;
    dev.failed = 0;
    dev.senses = 0;
    dev.last_tag = 0;
    wire.pending = 0;
    block.completions = 0;
    block.last_status = 0;
    block.last = 0;

    for (i = 0; i < MSC_MAX_DEVICES; i++) {
        msc_detach(0, 1 + i);
        close_slot(1 + i);
    }
    for (i = 0; i < MSC_MAX_DEVICES; i++) {
        msc_release(i);
    }
    for (i = 0; i < 8; i++) {
        tds[i].used = 0;
        quiesce_answer_for[i] = 1;
    }
    quiesces = 0;
    quiesce_answer = 1;
    clear_answer = 1;
    keep_data_td = 0;
    csw_residue_zero = 0;
    pages_reused = 0;
    completions_at_reset = 0;
    orders = 0;
    drops = 0;
    waits_logged = 0;
    arrivals = 0;
    departures = 0;
    flushes_counted = 0;
    commands_counted = 0;
}

/** Fill in and return the one static request: `count` sectors at `lba` into
 * or from `page`, generation 7, in flight. */
static struct block_request *make_request(int write, uint64_t lba, uint32_t count,
                                          unsigned char *page)
{
    static struct block_request r;

    r.device = 0;
    r.write = write;
    r.lba = lba;
    r.count = count;
    r.page = page;
    r.status = 0;
    r.state = BLOCK_REQ_IN_FLIGHT;
    r.generation = 7;
    r.deadline = 0;
    return &r;
}

/**
 * Check that the time of transfers, retries and recovery counts against each
 * unit's ten-second initialization deadline, in every state and stage, and
 * that a pump with no elapsed time counts nothing.
 */
static void initialization_deadline(void)
{
    int i;
    int states[] = {MSC_STATE_MAX_LUN, MSC_STATE_INQUIRY,  MSC_STATE_SERIAL,     MSC_STATE_TEST,
                    MSC_STATE_SENSE,   MSC_STATE_CAPACITY, MSC_STATE_CAPACITY16, MSC_STATE_MODE};
    int stages[] = {STAGE_IDLE, STAGE_CBW,   STAGE_DATA,  STAGE_CSW,
                    STAGE_DONE, STAGE_CLASS, STAGE_FINISH};
    unsigned int state;
    unsigned int stage;

    model_reset();
    dev.lun_ready_after[0] = 100000;
    CHECK("a slow unit attaches", msc_attach(0, 1, 0, EP_IN, EP_OUT) == 0);
    for (i = 0; i < 99; i++) {
        pump(100);
    }
    CHECK("a slow unit retains the last 100 ms", drops == 0);
    pump(100);
    CHECK("initialization expires after ten elapsed seconds", drops == 1 && arrivals == 0);
    pump(0);
    CHECK("an expired unit is not dropped twice", drops == 1);

    for (state = 0; state < sizeof(states) / sizeof(states[0]); state++) {
        for (stage = 0; stage < sizeof(stages) / sizeof(stages[0]); stage++) {
            model_reset();
            CHECK("a deadline case attaches", msc_attach(0, 1, 0, EP_IN, EP_OUT) == 0);
            sticks[0].state = states[state];
            sticks[0].stage = stages[stage];
            sticks[0].owned = 1;
            sticks[0].ready_left = 10;
            sticks[0].deadline = MSC_STEP_MS;
            msc_step(10);
            CHECK("every initialization state and stage expires before it advances",
                  drops == 1 && sticks[0].state == MSC_STATE_REFUSED && sticks[0].owned &&
                      sticks[0].used && arrivals == 0 && pages_reused == 0);
        }
    }
    model_reset();
    CHECK("a recovering unit attaches", msc_attach(0, 1, 0, EP_IN, EP_OUT) == 0);
    sticks[0].recover = RECOVER_CLEAR_IN;
    sticks[0].ready_left = 10;
    sticks[0].deadline = MSC_STEP_MS;
    msc_step(10);
    CHECK("recovery does not extend initialization", drops == 1);

    model_reset();
    dev.max_lun = 1;
    CHECK("two slow units attach", msc_attach(0, 1, 0, EP_IN, EP_OUT) == 0);
    for (i = 0; i < 40 && !msc_ready(0); i++) {
        pump(500);
    }
    CHECK("each unit gets a separate elapsed deadline",
          msc_ready(0) && arrivals == 2 && drops == 0 && i * 500 > MSC_READY_MS);
    msc_step(MSC_READY_MS);
    CHECK("a ready device has no initialization deadline", msc_ready(0) && drops == 0);

    model_reset();
    CHECK("zero elapsed time attaches", msc_attach(0, 1, 0, EP_IN, EP_OUT) == 0);
    for (i = 0; i < 50; i++) {
        pump(0);
    }
    CHECK("interrupt pumps do not consume elapsed time", msc_ready(0) && drops == 0);
}

/**
 * Run the rows and print the result; return 1 when a row failed.
 *
 * A stick fails TEST UNIT READY for a moment after it is plugged in, and
 * most sticks stall Get Max LUN, which means one logical unit. A unit's
 * serial longer than the record holds is left empty rather than cut. Nothing
 * resets a drive while it is being brought up, since some drives stop
 * answering for good when that is done to them.
 *
 * Every rule of the status wrapper is reset recovery. A status wrapper that
 * stalls once is read again, and one that stalls twice is reset recovery,
 * since a device that does not give its status is in a state the driver
 * cannot reason about. A read whose data stage came up short fails: a block
 * is whole or it is not. Every stage of a command goes out in the pass that
 * answers the stage before it; a driver that advanced a stage a tick would
 * take three ticks for a read. A check condition is followed by REQUEST
 * SENSE, and the sense decides the request: a medium error fails it, and a
 * unit attention has the command sent once more.
 *
 * A device pulled between the command and its answer has every request
 * failed, and nothing of it is used again. Its slot is not free at once: a
 * mount, a description and a held buffer all point at its block device, and
 * a second stick given the slot would get their pointers with it. A new
 * stick's units are published only once it is up, so what shows that it took
 * another slot is that the first slot's device is still the closing one.
 *
 * A completion for a command the driver gave up on is dropped by the
 * generation its transfer carried, since the block layer has the request
 * back and its page may be another request's. Taken as the fresh command's,
 * such an answer would advance a stage that has not happened, so the read
 * would return whatever the page held; as a failure, it would send the
 * device through reset recovery and fail a read with nothing wrong. Tags go
 * up across sticks and attaches, so a completion left from a pulled stick
 * cannot match the next stick's command. A record kept after its slot was
 * given back claims no completion, since one naming the slot is the next
 * device's.
 *
 * A transfer that never answers is not completed at its deadline, since the
 * controller still owns its page; recovery comes first, and the request
 * fails when it ends. When the class reset never answers either, the
 * request fails at the recovery's deadline and the stick is handed back to
 * the core, which enumerates its port again as a replug would. A class
 * request the stick stalls halts the control endpoint, which is moved on
 * before the first command's wrapper. A stick that stalls the class reset or
 * the clear of a halt is handed to the core rather than served with an
 * endpoint halted.
 *
 * The request's pages are the controller's while a transfer that names them
 * is in flight. A stick pulled with a read's data stage in flight has the
 * request failed when the slot is given back, not at the pull, and the
 * record goes when the disk layer and the controller have both let go, in
 * either order. Pulled during the command wrapper, the pages are not yet the
 * controller's and the request fails at once; pulled during the status
 * wrapper, the data record is retired but still held, and the request waits
 * for the slot. A Disable Slot never answered leaves the request
 * uncompleted, the record kept, and a message in the log. A data stage that
 * never answers has bulk IN stopped and moved on, then bulk OUT, then the
 * class reset and the two clears, and the request failed once after all of
 * it. A stop refused or never answered on that path leaves the request
 * uncompleted and the record kept, and hands the stick to the core. A clear
 * refused after the reset fails the request, since the quiesces have
 * retired the pages, and hands the stick to the core.
 *
 * The bytes a data stage moved are the command's, whatever the wrapper
 * claims: a short or stalled data stage under a wrapper claiming no residue
 * is not a whole read. A wrapper over a data record the controller has not
 * retired, as QEMU leaves a short transfer, has the endpoint stopped before
 * the next command's wrapper, and so has a failed one before its REQUEST
 * SENSE. A completion delivered twice, one carrying an earlier stage's
 * generation, one after the stick has gone, and one with an old stick's
 * generation after its slot, record and request were reused all move
 * nothing.
 */
int main(void)
{
    static unsigned char page[8 * SECTOR];

    /* A stick that comes up at once. */
    model_reset();
    CHECK("a stick is taken", msc_attach(0, 1, 0, EP_IN, EP_OUT) == 0);
    CHECK("and comes up", run_until_settled(100));
    CHECK("with the capacity the drive reported, which is its last block plus one",
          msc_block_device(0) != 0 && msc_block_device(0)->sector_count == MEDIUM_SECTORS);
    CHECK("and 512-byte sectors", msc_block_device(0)->sector_size == SECTOR);
    CHECK("and no write protection", (msc_block_device(0)->flags & BLOCK_READ_ONLY) == 0);
    CHECK("and it is not fault-safe, since it sleeps its caller",
          (msc_block_device(0)->flags & BLOCK_FAULT_SAFE) == 0);

    /* A read of one page, and a write of one sector read back. */
    {
        struct block_request *r = make_request(0, 2, 8, page);
        int i;

        CHECK("a read is taken", msc_block_device(0)->submit(msc_block_device(0), r) == 0);
        for (i = 0; i < 20 && block.completions == 0; i++) {
            pump(1);
        }
        CHECK("and completes", block.completions == 1 && block.last_status == 0);
        CHECK("with the medium's own bytes",
              page[0] == dev.medium[2 * SECTOR] && page[SECTOR] == dev.medium[3 * SECTOR]);
        CHECK("as one command, counted", commands_counted == 1);
    }

    /* A read of the driver's largest request, 1024 sectors. */
    {
        static unsigned char bigpage[1024 * SECTOR];
        struct block_request *r = make_request(0, 16, 1024, bigpage);
        int i;

        block.completions = 0;
        commands_counted = 0;
        largest_length = 0;
        dev.medium[16 * SECTOR] = 0x5A;
        dev.medium[(16 + 1023) * SECTOR] = 0xA5;
        CHECK("a 1024-sector read is taken",
              msc_block_device(0)->submit(msc_block_device(0), r) == 0);
        for (i = 0; i < 40 && block.completions == 0; i++) {
            pump(1);
        }
        CHECK("and completes", block.completions == 1 && block.last_status == 0);
        CHECK("as one command with one data transfer of 512 KiB",
              commands_counted == 1 && largest_length == 1024 * SECTOR);
        CHECK("whose count names the 1024 sectors the transfer carries", command_mismatches == 0);
        CHECK("the 1024 sectors the stick's device publishes as its most",
              msc_block_device(0)->max_sectors == 1024);
        CHECK("with the medium's own bytes, first and last sector",
              bigpage[0] == dev.medium[16 * SECTOR] &&
                  bigpage[1023 * SECTOR] == dev.medium[(16 + 1023) * SECTOR]);
    }
    {
        struct block_request *r = make_request(1, 5, 1, page);
        int i;

        for (i = 0; i < (int)SECTOR; i++) {
            page[i] = (unsigned char)(0xA0 + (i & 0x0F));
        }
        block.completions = 0;
        CHECK("a write is taken", msc_block_device(0)->submit(msc_block_device(0), r) == 0);
        for (i = 0; i < 20 && block.completions == 0; i++) {
            pump(1);
        }
        CHECK("and completes", block.completions == 1 && block.last_status == 0);
        CHECK("with the bytes on the medium", dev.medium[5 * SECTOR] == 0xA0);
    }

    /* A stick that fails TEST UNIT READY a dozen times before it is ready. */
    model_reset();
    dev.lun_ready_after[0] = 12;
    CHECK("a stick that is not ready yet is taken", msc_attach(0, 1, 0, EP_IN, EP_OUT) == 0);
    CHECK("and comes up when it becomes ready", run_until_settled(4000));
    CHECK("after the tests it needed", dev.tests_seen[0] == 13);

    /* One that never becomes ready is refused at the deadline. */
    model_reset();
    dev.lun_ready_after[0] = 100000;
    CHECK("a stick that never becomes ready is taken", msc_attach(0, 1, 0, EP_IN, EP_OUT) == 0);
    CHECK("and is not brought up", run_until_settled(30000) == 0);
    CHECK("and serves nothing", !msc_ready(0));
    CHECK("and the disk layer is never told of it", arrivals == 0);

    /* Get Max LUN stalled, which most sticks do, is one logical unit. */
    model_reset();
    dev.stall_max_lun = 1;
    CHECK("a stick that will not say how many units it has is taken",
          msc_attach(0, 1, 0, EP_IN, EP_OUT) == 0);
    CHECK("and comes up on the one it has", run_until_settled(100));

    /* A drive with a CD-ROM on its first unit and the flash on its second. */
    model_reset();
    dev.max_lun = 1;
    dev.lun_type[0] = 0x05; /* a CD-ROM */
    dev.lun_type[1] = 0x00;
    dev.capacity_lun = -1;
    CHECK("a drive with a CD-ROM in front of its flash is taken",
          msc_attach(0, 1, 0, EP_IN, EP_OUT) == 0);
    CHECK("and comes up", run_until_settled(200));
    CHECK("on the unit that is a disk and not on the one that is not", dev.capacity_lun == 1);

    /* Each unit's own serial, and one longer than the record holds. */
    model_reset();
    dev.max_lun = 1;
    dev.lun_serial[0] = "FIRST-UNIT";
    dev.lun_serial[1] = "0123456789012345678901234567890123456789012345678901234567890123456789";
    CHECK("a drive with two units and two serials is taken",
          msc_attach(0, 1, 0, EP_IN, EP_OUT) == 0);
    CHECK("and comes up", run_until_settled(200));
    {
        struct msc_ident ident;

        CHECK("the first unit reports its own serial",
              msc_lun_ident(0, 0, &ident) == 0 && ident.serial[0] == 'F' &&
                  ident.serial[9] == 'T' && ident.serial[10] == '\0');
        CHECK("and the second, whose serial is seventy characters, reports none",
              msc_lun_ident(0, 1, &ident) == 0 && ident.serial[0] == '\0');
    }

    /* A stick without the serial page comes up with an empty serial. */
    model_reset();
    dev.no_serial_page = 1;
    CHECK("a stick without the serial page is taken", msc_attach(0, 1, 0, EP_IN, EP_OUT) == 0);
    CHECK("and comes up", run_until_settled(200));
    {
        struct msc_ident ident;

        CHECK("with an empty serial", msc_ident(0, &ident) == 0 && ident.serial[0] == '\0');
    }

    /* A card reader whose first slot is empty. */
    model_reset();
    dev.max_lun = 1;
    dev.lun_no_medium[0] = 1;
    dev.capacity_lun = -1;
    CHECK("a reader with an empty slot is taken", msc_attach(0, 1, 0, EP_IN, EP_OUT) == 0);
    CHECK("and comes up", run_until_settled(200));
    CHECK("on the slot that has a card", dev.capacity_lun == 1);

    /* The capacity rules. */
    model_reset();
    dev.last_block = 0xFFFFFFFFu;
    CHECK("a drive larger than a ten-byte read can address is taken",
          msc_attach(0, 1, 0, EP_IN, EP_OUT) == 0);
    CHECK("and refused", run_until_settled(200) == 0);

    model_reset();
    dev.block_size = 4096;
    CHECK("a drive with 4 KiB blocks is taken", msc_attach(0, 1, 0, EP_IN, EP_OUT) == 0);
    CHECK("and refused", run_until_settled(200) == 0);

    /* Write protection, and the drive that will not answer the question. */
    model_reset();
    dev.write_protect = 1;
    CHECK("a protected stick is taken", msc_attach(0, 1, 0, EP_IN, EP_OUT) == 0);
    CHECK("and comes up", run_until_settled(200));
    CHECK("read-only", (msc_block_device(0)->flags & BLOCK_READ_ONLY) != 0);
    {
        struct block_request *r = make_request(1, 0, 1, page);

        CHECK("and refuses a write without sending one",
              msc_block_device(0)->submit(msc_block_device(0), r) == -EROFS);
    }

    /* The status wrapper's rules, each of which is reset recovery. */
    model_reset();
    CHECK("a stick is taken", msc_attach(0, 1, 0, EP_IN, EP_OUT) == 0);
    CHECK("and comes up", run_until_settled(200));
    {
        struct block_request *r = make_request(0, 0, 1, page);
        int i;

        dev.bad_tag = 1;
        block.completions = 0;
        CHECK("a read is taken", msc_block_device(0)->submit(msc_block_device(0), r) == 0);
        for (i = 0; i < 40 && block.completions == 0; i++) {
            pump(1);
        }
        CHECK("a status wrapper naming another command fails the read",
              block.completions == 1 && block.last_status == -EIO);
        CHECK("and the device is reset", dev.resets == 1);
        CHECK("with the inward endpoint cleared first, then the outward one",
              dev.clears >= 2 && dev.clear_order[0] == EP_IN && dev.clear_order[1] == EP_OUT);
    }

    /* A device pulled between the command and its answer. */
    model_reset();
    CHECK("a stick is taken", msc_attach(0, 1, 0, EP_IN, EP_OUT) == 0);
    CHECK("and comes up", run_until_settled(200));
    {
        struct block_request *r = make_request(0, 0, 1, page);

        block.completions = 0;
        dev.vanish_at = 2; /* it goes during the data stage */
        CHECK("a read is taken", msc_block_device(0)->submit(msc_block_device(0), r) == 0);
        pump(1);
        pump(1);
        msc_detach(0, 1);
        CHECK("the read is kept when the stick goes, its pages the controller's",
              block.completions == 0);
        CHECK("and the stick stops serving", msc_count() == 0 && !msc_ready(0));
        CHECK("the disk layer is told once", departures == 1);
        /* The slot stays taken while the disk layer holds the device. */
        CHECK("its device is kept until the disk layer releases it", msc_block_device(0) != 0);
        CHECK("and refuses what it is asked for",
              msc_block_device(0)->submit(msc_block_device(0), r) == -EIO);
        /* The second stick's units appear only once it is up. */
        CHECK("a second stick is given a different slot",
              msc_attach(0, 2, 0, EP_IN, EP_OUT) == 0 && msc_block_device(0) != 0 &&
                  msc_block_device(0)->submit(msc_block_device(0), r) == -EIO);
        msc_release(0);
        CHECK("and is kept while the controller has not given the slot back",
              msc_block_device(0) != 0);
        close_slot(1);
        CHECK("the slot goes back when the last hold does", msc_block_device(0) == 0);
        CHECK("and the read is failed with it",
              block.completions == 1 && block.last_status == -EIO);
    }

    /* Two requests, admitted in the order they arrived. */
    model_reset();
    CHECK("a stick is taken", msc_attach(0, 1, 0, EP_IN, EP_OUT) == 0);
    CHECK("and comes up", run_until_settled(200));
    {
        static struct block_request first, second;
        static unsigned char page_a[SECTOR], page_b[SECTOR];
        int i;

        first.write = 0;
        first.lba = 1;
        first.count = 1;
        first.page = page_a;
        first.state = BLOCK_REQ_IN_FLIGHT;
        first.generation = 1;
        second = first;
        second.lba = 2;
        second.page = page_b;
        second.generation = 2;

        block.completions = 0;
        CHECK("the first read is taken",
              msc_block_device(0)->submit(msc_block_device(0), &first) == 0);
        CHECK("and the second waits its turn",
              msc_block_device(0)->submit(msc_block_device(0), &second) == 0);
        for (i = 0; i < 60 && block.completions < 2; i++) {
            pump(1);
        }
        CHECK("both complete", block.completions == 2);
        CHECK("each with its own sector",
              page_a[0] == dev.medium[SECTOR] && page_b[0] == dev.medium[2 * SECTOR]);
    }

    /* A read whose data stage came up short. */
    model_reset();
    CHECK("a stick is taken", msc_attach(0, 1, 0, EP_IN, EP_OUT) == 0);
    CHECK("and comes up", run_until_settled(200));
    {
        struct block_request *r = make_request(0, 0, 1, page);
        int i;

        dev.residue = 64; /* the drive moved all but the last 64 bytes */
        block.completions = 0;
        CHECK("a read is taken", msc_block_device(0)->submit(msc_block_device(0), r) == 0);
        for (i = 0; i < 40 && block.completions == 0; i++) {
            pump(1);
        }
        CHECK("a read that came up short fails",
              block.completions == 1 && block.last_status == -EIO);
    }

    /* A status wrapper that stalls once, then one that stalls twice. */
    model_reset();
    CHECK("a stick is taken", msc_attach(0, 1, 0, EP_IN, EP_OUT) == 0);
    CHECK("and comes up", run_until_settled(200));
    {
        struct block_request *r = make_request(0, 0, 1, page);
        int i;

        dev.stall_stage = 3;
        dev.stall_times = 1;
        block.completions = 0;
        dev.resets = 0;
        CHECK("a read is taken", msc_block_device(0)->submit(msc_block_device(0), r) == 0);
        for (i = 0; i < 40 && block.completions == 0; i++) {
            pump(1);
        }
        CHECK("a status wrapper that stalled once is read again and the read succeeds",
              block.completions == 1 && block.last_status == 0);
        CHECK("with no reset", dev.resets == 0);
        CHECK("and the inward endpoint cleared once", dev.cleared_in == 1 && dev.cleared_out == 0);
    }
    {
        struct block_request *r = make_request(0, 0, 1, page);
        int i;

        dev.stall_stage = 3;
        dev.stall_times = 2;
        block.completions = 0;
        dev.resets = 0;
        dev.clears = 0;
        dev.cleared_in = 0;
        dev.cleared_out = 0;
        CHECK("another read is taken", msc_block_device(0)->submit(msc_block_device(0), r) == 0);
        for (i = 0; i < 60 && block.completions == 0; i++) {
            pump(1);
        }
        CHECK("a status wrapper that stalled twice fails the read",
              block.completions == 1 && block.last_status == -EIO);
        CHECK("and the device is reset", dev.resets == 1);
    }

    /* Each stage goes out in the pass that answers the one before it. */
    model_reset();
    CHECK("a stick is taken", msc_attach(0, 1, 0, EP_IN, EP_OUT) == 0);
    CHECK("and comes up", run_until_settled(200));
    {
        struct block_request *r = make_request(0, 0, 1, page);
        int turns = 0;

        block.completions = 0;
        CHECK("a read is taken", msc_block_device(0)->submit(msc_block_device(0), r) == 0);
        while (turns < 20 && block.completions == 0) {
            pump(1);
            turns++;
        }
        CHECK("and its three stages take three passes of the pump", turns <= 3);
    }

    /* A completion for a command the driver gave up on. */
    model_reset();
    CHECK("a stick is taken", msc_attach(0, 1, 0, EP_IN, EP_OUT) == 0);
    CHECK("and comes up", run_until_settled(200));
    {
        struct block_request *r = make_request(0, 0, 1, page);
        struct usb_completion late;
        int i;

        dev.vanish_at = 2;
        block.completions = 0;
        CHECK("a read is taken", msc_block_device(0)->submit(msc_block_device(0), r) == 0);
        for (i = 0; i < 40 && drops == 0; i++) {
            pump(20000); /* past the driver's own deadline */
        }
        /* The stick is handed to the core with the request kept. */
        CHECK("the deadline refuses the stick and hands it to the core",
              drops == 1 && !msc_ready(0) && block.completions == 0);
        msc_detach(0, 1);
        close_slot(1);
        msc_release(0);
        CHECK("and the read fails when the core has closed the device",
              block.completions == 1 && block.last_status == -EIO);
        dev.vanish_at = 0;
        dev.attached = 1;
        CHECK("the stick comes back on the same handle",
              msc_attach(0, 1, 0, EP_IN, EP_OUT) == 0 && run_until_settled(200));
        /* A fresh request, and the abandoned transfer's answer during it. */
        r = make_request(0, 3, 1, page);
        page[0] = 0xEE;
        block.completions = 0;
        CHECK("a fresh read is taken", msc_block_device(0)->submit(msc_block_device(0), r) == 0);
        late.hc = 0; /* the controller this test's sticks are on */
        late.device = 1;
        late.endpoint = EP_IN;
        /* It fails, as a transfer on a device that went away does. */
        late.status = USB_XFER_FAILED;
        late.length = SECTOR;
        late.residual = SECTOR;
        late.generation = 1; /* whatever the abandoned transfer carried */
        dev.resets = 0;
        CHECK("the abandoned transfer's answer is taken by this driver",
              msc_completion(&late) == 1);
        CHECK("and moves nothing on", block.completions == 0);
        CHECK("and sends the device through no recovery", dev.resets == 0);
        for (i = 0; i < 40 && block.completions == 0; i++) {
            pump(1);
        }
        CHECK("so the fresh read completes on its own answers",
              block.completions == 1 && block.last_status == 0);
        CHECK("with the sector it asked for", page[0] == dev.medium[3 * SECTOR]);
    }

    /* No reset while a drive is being brought up. */
    model_reset();
    dev.lun_ready_after[0] = 4;
    CHECK("a stick that takes a moment is taken", msc_attach(0, 1, 0, EP_IN, EP_OUT) == 0);
    CHECK("and comes up", run_until_settled(2000));
    CHECK("with no reset sent while it did", dev.resets == 0);

    /* The flush, and the two answers that are not failures. */
    model_reset();
    CHECK("a stick is taken", msc_attach(0, 1, 0, EP_IN, EP_OUT) == 0);
    CHECK("and comes up", run_until_settled(200));
    {
        struct block_request *r = make_request(0, 0, 0, page);
        int i;

        block.completions = 0;
        CHECK("a flush is taken", msc_block_device(0)->flush(msc_block_device(0), r) == 0);
        for (i = 0; i < 40 && block.completions == 0; i++) {
            pump(1);
        }
        CHECK("and completes", block.completions == 1 && block.last_status == 0);
        CHECK("and is counted with the disk's flushes", flushes_counted == 1);
        {
            struct msc_ident ident;

            CHECK("and on the stick's own line", msc_ident(0, &ident) == 0 && ident.flushes == 1);
            CHECK("and the stick's serial is the unit serial page's",
                  msc_ident(0, &ident) == 0 && ident.serial[0] == 'M' && ident.serial[6] == '1' &&
                      ident.serial[7] == '\0');
        }
    }

    /* A check condition: a medium error, then a unit attention. */
    model_reset();
    CHECK("a stick is taken", msc_attach(0, 1, 0, EP_IN, EP_OUT) == 0);
    CHECK("and comes up", run_until_settled(200));
    {
        struct block_request *r = make_request(0, 0, 1, page);
        int i;

        block.completions = 0;
        dev.fail_next_data = 1;
        CHECK("a read is taken", msc_block_device(0)->submit(msc_block_device(0), r) == 0);
        for (i = 0; i < 40 && block.completions == 0; i++) {
            pump(1);
        }
        CHECK("a read the stick fails with a medium error is failed",
              block.completions == 1 && block.last_status == -EIO);
        CHECK("after one REQUEST SENSE", dev.senses == 1);
    }
    model_reset();
    CHECK("a stick is taken", msc_attach(0, 1, 0, EP_IN, EP_OUT) == 0);
    CHECK("and comes up", run_until_settled(200));
    {
        struct block_request *r = make_request(0, 3, 1, page);
        int i;

        for (i = 0; i < SECTOR; i++) {
            page[i] = 0;
        }
        block.completions = 0;
        dev.fail_next_data = 1;
        dev.sense_key = 0x06; /* unit attention */
        CHECK("a read is taken", msc_block_device(0)->submit(msc_block_device(0), r) == 0);
        for (i = 0; i < 40 && block.completions == 0; i++) {
            pump(1);
        }
        CHECK("a read answered with a unit attention is sent again and succeeds",
              block.completions == 1 && block.last_status == 0);
        CHECK("with the sector from the second attempt",
              page[5] == dev.medium[3 * SECTOR + 5] && dev.senses == 1);
        block.completions = 0;
        dev.fail_next_data = 2;
        r = make_request(0, 0, 1, page);
        CHECK("a read is taken", msc_block_device(0)->submit(msc_block_device(0), r) == 0);
        for (i = 0; i < 40 && block.completions == 0; i++) {
            pump(1);
        }
        CHECK("and a unit attention on the second attempt fails it",
              block.completions == 1 && block.last_status == -EIO && dev.senses == 3);
    }

    /* The flush's three answers. */
    model_reset();
    CHECK("a stick is taken", msc_attach(0, 1, 0, EP_IN, EP_OUT) == 0);
    CHECK("and comes up", run_until_settled(200));
    {
        struct block_request *r = make_request(0, 0, 0, page);
        struct msc_ident ident;
        int i;

        block.completions = 0;
        dev.sync_illegal = 1;
        CHECK("a flush is taken", msc_block_device(0)->flush(msc_block_device(0), r) == 0);
        for (i = 0; i < 40 && block.completions == 0; i++) {
            pump(1);
        }
        CHECK("a flush the stick calls an illegal request is not a failure",
              block.completions == 1 && block.last_status == 0 && dev.senses == 1);
        CHECK("and is counted", msc_ident(0, &ident) == 0 && ident.flushes == 1);
        dev.sync_illegal = 0;
        dev.sync_fails = 1;
        block.completions = 0;
        CHECK("a flush is taken", msc_block_device(0)->flush(msc_block_device(0), r) == 0);
        for (i = 0; i < 40 && block.completions == 0; i++) {
            pump(1);
        }
        CHECK("a flush the stick fails is failed",
              block.completions == 1 && block.last_status == -EIO);
        CHECK("and is not counted", msc_ident(0, &ident) == 0 && ident.flushes == 1);
    }

    /* A transfer that never answers. */
    model_reset();
    CHECK("a stick is taken", msc_attach(0, 1, 0, EP_IN, EP_OUT) == 0);
    CHECK("and comes up", run_until_settled(200));
    {
        struct block_request *r = make_request(0, 0, 1, page);
        int i;

        block.completions = 0;
        dev.resets = 0;
        dev.vanish_at = 2; /* the data stage is never answered */
        CHECK("a read is taken", msc_block_device(0)->submit(msc_block_device(0), r) == 0);
        for (i = 0; i < 900; i++) {
            pump(10);
        }
        CHECK("a transfer that never answers is not completed before its deadline",
              block.completions == 0);
        dev.vanish_at = 0;
        for (i = 0; i < 300 && block.completions == 0; i++) {
            pump(10);
        }
        CHECK("and not at its deadline either, before recovery is attempted", dev.resets == 1);
        CHECK("and the request is failed when recovery ends",
              block.completions == 1 && block.last_status == -EIO);
    }

    /* A transfer that never answers, then a class reset that never does. */
    model_reset();
    drops = 0;
    CHECK("a stick is taken", msc_attach(0, 1, 0, EP_IN, EP_OUT) == 0);
    CHECK("and comes up", run_until_settled(200));
    {
        struct block_request *r = make_request(0, 0, 1, page);
        int i;

        block.completions = 0;
        dev.resets = 0;
        dev.vanish_at = 2;   /* the data stage is never answered */
        dev.reset_never = 1; /* nor the class reset that follows */
        CHECK("a read is taken", msc_block_device(0)->submit(msc_block_device(0), r) == 0);
        for (i = 0; i < 3000 && drops == 0; i++) {
            pump(10);
        }
        CHECK("a recovery that never answers refuses the stick with the request kept",
              dev.resets == 1 && block.completions == 0 && drops == 1);
        msc_detach(0, 1);
        close_slot(1);
        msc_release(0);
        CHECK("and the request fails when the core has closed the device",
              block.completions == 1 && block.last_status == -EIO);
        dev.vanish_at = 0;
        dev.reset_never = 0;
    }

    /* Tags go up across sticks and attaches. */
    model_reset();
    CHECK("a stick is taken", msc_attach(0, 1, 0, EP_IN, EP_OUT) == 0);
    CHECK("and comes up", run_until_settled(200));
    {
        uint32_t first_tag;
        struct block_request *r = make_request(0, 0, 1, page);
        int i;

        block.completions = 0;
        CHECK("a read is taken", msc_block_device(0)->submit(msc_block_device(0), r) == 0);
        for (i = 0; i < 40 && block.completions == 0; i++) {
            pump(1);
        }
        first_tag = dev.last_tag;
        msc_detach(0, 1);
        close_slot(1);
        msc_release(0);
        dev.attached = 1;
        CHECK("a second stick is taken on the same handle",
              msc_attach(0, 1, 0, EP_IN, EP_OUT) == 0);
        CHECK("and comes up", run_until_settled(200));
        CHECK("and its first command's tag is past every tag the first stick used",
              dev.last_tag > first_tag);
    }

    /* A class request the stick stalls halts the control endpoint. */
    model_reset();
    dev.stall_max_lun = 1;
    orders = 0;
    CHECK("a stick that stalls Get Max LUN is taken", msc_attach(0, 1, 0, EP_IN, EP_OUT) == 0);
    CHECK("and comes up", run_until_settled(200));
    CHECK("with its control endpoint moved on before its first command",
          orders >= 2 && order[0] == ORDER_CLEAR + EP_CONTROL && order[1] == ORDER_WRAPPER);

    /* A stalled data stage under a wrapper claiming no residue. */
    model_reset();
    CHECK("a stick is taken", msc_attach(0, 1, 0, EP_IN, EP_OUT) == 0);
    CHECK("and comes up", run_until_settled(200));
    {
        struct block_request *r = make_request(0, 0, 2, page);
        int i;

        block.completions = 0;
        CHECK("a read is taken", msc_block_device(0)->submit(msc_block_device(0), r) == 0);
        for (i = 0; i < 40 && block.completions == 0; i++) {
            pump(1);
        }
        CHECK("and completes", block.completions == 1 && block.last_status == 0);
        r = make_request(0, 4, 2, page);
        block.completions = 0;
        dev.stall_stage = 2;
        dev.stall_times = 1;
        CHECK("a second read is taken", msc_block_device(0)->submit(msc_block_device(0), r) == 0);
        for (i = 0; i < 60 && block.completions == 0; i++) {
            pump(1);
        }
        CHECK("whose stalled data stage under a wrapper claiming no residue fails it",
              block.completions == 1 && block.last_status == -EIO);
    }

    /* A bring-up command whose data transfer the controller keeps. */
    model_reset();
    keep_data_td = 1;
    orders = 0;
    CHECK("a stick is taken", msc_attach(0, 1, 0, EP_IN, EP_OUT) == 0);
    CHECK("and comes up", run_until_settled(400));
    CHECK("with each unretired transfer's endpoint stopped before the next wrapper",
          orders >= 3 && order[0] == ORDER_WRAPPER && order[1] == ORDER_QUIESCE + EP_IN &&
              order[2] == ORDER_WRAPPER && pages_reused == 0);
    keep_data_td = 0;

    /* A record kept after its slot was given back. */
    model_reset();
    CHECK("a stick is taken", msc_attach(0, 1, 0, EP_IN, EP_OUT) == 0);
    CHECK("and comes up", run_until_settled(200));
    {
        struct block_request *r = make_request(0, 0, 1, page);
        struct usb_completion next;

        block.completions = 0;
        dev.vanish_at = 2;
        CHECK("a read is taken", msc_block_device(0)->submit(msc_block_device(0), r) == 0);
        pump(1);
        pump(1);
        msc_detach(0, 1);
        close_slot(1);
        CHECK("the record is kept for the disk layer", msc_block_device(0) != 0);
        next.hc = 0;
        next.device = 1;
        next.endpoint = EP_CONTROL;
        next.status = USB_XFER_OK;
        next.length = 8;
        next.residual = 0;
        next.generation = 1;
        CHECK("and a completion naming its slot is not taken", msc_completion(&next) == 0);
        msc_release(0);
        dev.vanish_at = 0;
    }

    /* A wrapper that never answers, and the stop of bulk IN refused. */
    model_reset();
    CHECK("a stick is taken", msc_attach(0, 1, 0, EP_IN, EP_OUT) == 0);
    CHECK("and comes up", run_until_settled(200));
    {
        struct block_request *r = make_request(0, 0, 1, page);
        int i;

        block.completions = 0;
        dev.vanish_at = 1;
        quiesce_answer_for[EP_IN] = -1;
        CHECK("a read is taken", msc_block_device(0)->submit(msc_block_device(0), r) == 0);
        for (i = 0; i < 40 && drops == 0; i++) {
            pump(MSC_STEP_MS / 2);
        }
        CHECK("a stop refused after a wrapper never answered hands the stick to the core",
              drops == 1 && !msc_ready(0) && block.completions == 0);
        msc_detach(0, 1);
        close_slot(1);
        msc_release(0);
        CHECK("and the request fails at the close", block.completions == 1);
        dev.vanish_at = 0;
    }

    /* A class reset the stick stalls. */
    model_reset();
    CHECK("a stick is taken", msc_attach(0, 1, 0, EP_IN, EP_OUT) == 0);
    CHECK("and comes up", run_until_settled(200));
    {
        struct block_request *r = make_request(0, 0, 1, page);
        int i;

        block.completions = 0;
        dev.phase_error = 1;
        dev.reset_stalls = 1;
        CHECK("a read is taken", msc_block_device(0)->submit(msc_block_device(0), r) == 0);
        for (i = 0; i < 60 && drops == 0; i++) {
            pump(1);
        }
        CHECK("a class reset the stick stalls refuses the stick to the core",
              dev.resets == 1 && drops == 1 && !msc_ready(0) && dev.dev_clears == 0);
        dev.reset_stalls = 0;
    }

    /* A device that will not clear its halt: the same. */
    model_reset();
    CHECK("a stick is taken", msc_attach(0, 1, 0, EP_IN, EP_OUT) == 0);
    CHECK("and comes up", run_until_settled(200));
    {
        struct block_request *r = make_request(0, 0, 1, page);
        int i;

        block.completions = 0;
        dev.phase_error = 1;
        dev.devclear_stalls = 1;
        CHECK("a read is taken", msc_block_device(0)->submit(msc_block_device(0), r) == 0);
        for (i = 0; i < 60 && drops == 0; i++) {
            pump(1);
        }
        CHECK("a device that stalls the clear of its halt is refused to the core",
              dev.resets == 1 && dev.dev_clears == 1 && drops == 1 && !msc_ready(0));
        dev.devclear_stalls = 0;
    }

    /* ---- the request's pages while the controller can reach them ---- */

    /* A stick pulled with a read's data stage in flight. */
    model_reset();
    CHECK("a stick is taken", msc_attach(0, 1, 0, EP_IN, EP_OUT) == 0);
    CHECK("and comes up", run_until_settled(200));
    {
        struct block_request *r = make_request(0, 0, 1, page);

        block.completions = 0;
        dev.vanish_at = 2;
        CHECK("a read is taken", msc_block_device(0)->submit(msc_block_device(0), r) == 0);
        pump(1);
        pump(1);
        CHECK("and its data stage is in flight", sticks[0].stage == STAGE_DATA && sticks[0].owned);
        msc_detach(0, 1);
        CHECK("pulled during the data stage, the request is not completed",
              block.completions == 0 && departures == 1);
        msc_release(0);
        CHECK("the disk layer's release keeps the record",
              msc_block_device(0) != 0 && block.completions == 0);
        close_slot(1);
        CHECK("the slot given back fails the request",
              block.completions == 1 && block.last_status == -EIO);
        CHECK("and reclaims the record once", msc_block_device(0) == 0 && msc_count() == 0);
        CHECK("with no page touched while a transfer named it", pages_reused == 0);
    }
    /* The other order: the slot given back first, the disk layer after. */
    model_reset();
    CHECK("a stick is taken", msc_attach(0, 1, 0, EP_IN, EP_OUT) == 0);
    CHECK("and comes up", run_until_settled(200));
    {
        struct block_request *r = make_request(0, 0, 1, page);

        block.completions = 0;
        dev.vanish_at = 2;
        CHECK("a read is taken", msc_block_device(0)->submit(msc_block_device(0), r) == 0);
        pump(1);
        pump(1);
        msc_detach(0, 1);
        close_slot(1);
        CHECK("the slot given back fails the request",
              block.completions == 1 && block.last_status == -EIO);
        CHECK("and keeps the record for the disk layer", msc_block_device(0) != 0);
        msc_release(0);
        CHECK("which reclaims it", msc_block_device(0) == 0);
        CHECK("with no page touched while a transfer named it", pages_reused == 0);
    }
    /* Pulled during the command wrapper, then during the status wrapper. */
    model_reset();
    CHECK("a stick is taken", msc_attach(0, 1, 0, EP_IN, EP_OUT) == 0);
    CHECK("and comes up", run_until_settled(200));
    {
        struct block_request *r = make_request(0, 0, 1, page);

        block.completions = 0;
        dev.vanish_at = 1;
        CHECK("a read is taken", msc_block_device(0)->submit(msc_block_device(0), r) == 0);
        pump(1);
        CHECK("and its wrapper is in flight", sticks[0].stage == STAGE_CBW && !sticks[0].owned);
        msc_detach(0, 1);
        CHECK("pulled during the command wrapper, the request fails at the pull",
              block.completions == 1 && block.last_status == -EIO && pages_reused == 0);
        close_slot(1);
        msc_release(0);
    }
    model_reset();
    CHECK("a stick is taken", msc_attach(0, 1, 0, EP_IN, EP_OUT) == 0);
    CHECK("and comes up", run_until_settled(200));
    {
        struct block_request *r = make_request(0, 0, 1, page);

        block.completions = 0;
        dev.vanish_at = 3;
        CHECK("a read is taken", msc_block_device(0)->submit(msc_block_device(0), r) == 0);
        pump(1);
        pump(1);
        pump(1);
        CHECK("and its status wrapper is in flight",
              sticks[0].stage == STAGE_CSW && sticks[0].owned);
        msc_detach(0, 1);
        CHECK("pulled during the status wrapper, the request is not completed",
              block.completions == 0);
        close_slot(1);
        CHECK("until the slot is given back", block.completions == 1 && block.last_status == -EIO);
        msc_release(0);
        CHECK("and the record goes with the disk layer's release", msc_block_device(0) == 0);
    }

    /* A Disable Slot never answered. */
    model_reset();
    CHECK("a stick is taken", msc_attach(0, 1, 0, EP_IN, EP_OUT) == 0);
    CHECK("and comes up", run_until_settled(200));
    {
        struct block_request *r = make_request(0, 0, 1, page);
        int i;

        block.completions = 0;
        dev.vanish_at = 2;
        CHECK("a read is taken", msc_block_device(0)->submit(msc_block_device(0), r) == 0);
        pump(1);
        pump(1);
        msc_detach(0, 1);
        msc_release(0);
        for (i = 0; i < 50; i++) {
            pump(MSC_STEP_MS);
        }
        CHECK("with the slot never given back, the request is never completed",
              block.completions == 0);
        CHECK("and the record is kept", msc_block_device(0) != 0 && waits_logged == 1);
        CHECK("and a second stick takes another slot",
              msc_attach(0, 2, 0, EP_IN, EP_OUT) == 0 && msc_block_device(0) != 0);
        close_slot(1);
        CHECK("the slot given back at last fails the request and frees the record",
              block.completions == 1 && msc_block_device(0) == 0);
    }

    /* A data stage that never answers: the order of the recovery. */
    model_reset();
    CHECK("a stick is taken", msc_attach(0, 1, 0, EP_IN, EP_OUT) == 0);
    CHECK("and comes up", run_until_settled(200));
    {
        struct block_request *r = make_request(0, 0, 1, page);
        int want[8] = {ORDER_WRAPPER,        ORDER_QUIESCE + EP_IN,  ORDER_QUIESCE + EP_OUT,
                       ORDER_RESET,          ORDER_CLEAR + EP_IN,    ORDER_DEVCLEAR + EP_IN,
                       ORDER_CLEAR + EP_OUT, ORDER_DEVCLEAR + EP_OUT};
        int i;

        block.completions = 0;
        dev.vanish_at = 2;
        orders = 0;
        CHECK("a read is taken", msc_block_device(0)->submit(msc_block_device(0), r) == 0);
        for (i = 0; i < 40 && block.completions == 0; i++) {
            pump(MSC_STEP_MS / 2);
        }
        CHECK("a data stage that never answers fails the read",
              block.completions == 1 && block.last_status == -EIO);
        CHECK("after both endpoints were stopped, then the reset, then each clear on the "
              "controller and on the device",
              order_is(want, 8));
        CHECK("and the request was completed after all of them", completions_at_reset == 0);
        CHECK("and the stick still serves", msc_ready(0));
        CHECK("with no page touched while a transfer named it", pages_reused == 0);
        CHECK("and nothing handed to the core", drops == 0);
    }
    /* Each refusal on that path, and a stop never answered. */
    {
        int which;

        for (which = 0; which < 3; which++) {
            struct block_request *r;
            int i;

            model_reset();
            CHECK("a stick is taken", msc_attach(0, 1, 0, EP_IN, EP_OUT) == 0);
            CHECK("and comes up", run_until_settled(200));
            r = make_request(0, 0, 1, page);
            block.completions = 0;
            dev.vanish_at = 2;
            if (which == 0) {
                quiesce_answer_for[EP_IN] = -1;
            } else if (which == 1) {
                quiesce_answer_for[EP_OUT] = -1;
            } else {
                quiesce_answer_for[EP_OUT] = 0;
            }
            CHECK("a read is taken", msc_block_device(0)->submit(msc_block_device(0), r) == 0);
            for (i = 0; i < 40 && drops == 0; i++) {
                pump(MSC_STEP_MS / 2);
            }
            CHECK("a stop refused or never answered refuses the stick",
                  drops == 1 && !msc_ready(0) && dev.resets == 0);
            CHECK("with the request uncompleted", block.completions == 0);
            CHECK("and the record kept", msc_block_device(0) != 0 && sticks[0].owned);
            msc_detach(0, 1);
            close_slot(1);
            msc_release(0);
            CHECK("until the core has closed the device",
                  block.completions == 1 && block.last_status == -EIO && msc_block_device(0) == 0 &&
                      pages_reused == 0);
        }
    }
    /* A clear refused after the reset, and the recovery's deadline. */
    {
        int which;

        for (which = 0; which < 2; which++) {
            struct block_request *r;
            int i;

            model_reset();
            CHECK("a stick is taken", msc_attach(0, 1, 0, EP_IN, EP_OUT) == 0);
            CHECK("and comes up", run_until_settled(200));
            r = make_request(0, 0, 1, page);
            block.completions = 0;
            dev.vanish_at = 2;
            if (which == 0) {
                clear_answer = -1;
            } else {
                dev.reset_never = 1;
            }
            CHECK("a read is taken", msc_block_device(0)->submit(msc_block_device(0), r) == 0);
            for (i = 0; i < 40 && drops == 0; i++) {
                pump(MSC_STEP_MS / 2);
            }
            CHECK("a clear refused, or a recovery that never finishes, refuses the stick",
                  drops == 1 && !msc_ready(0) && dev.resets == 1);
            CHECK("with the request uncompleted and the record kept",
                  block.completions == 0 && msc_block_device(0) != 0 && quiesces == 2);
            msc_detach(0, 1);
            close_slot(1);
            msc_release(0);
            CHECK("until the core has closed the device",
                  block.completions == 1 && block.last_status == -EIO && msc_block_device(0) == 0 &&
                      pages_reused == 0);
            dev.reset_never = 0;
        }
    }

    /* The status wrapper against the data stage. */
    /* A short data stage under a wrapper claiming no residue. */
    model_reset();
    CHECK("a stick is taken", msc_attach(0, 1, 0, EP_IN, EP_OUT) == 0);
    CHECK("and comes up", run_until_settled(200));
    {
        struct block_request *r = make_request(0, 0, 2, page);
        int i;

        block.completions = 0;
        dev.residue = 100;
        csw_residue_zero = 1;
        CHECK("a read is taken", msc_block_device(0)->submit(msc_block_device(0), r) == 0);
        for (i = 0; i < 40 && block.completions == 0; i++) {
            pump(1);
        }
        CHECK("a short data stage under a wrapper claiming no residue fails the read",
              block.completions == 1 && block.last_status == -EIO && dev.resets == 0);
    }
    /* A wrapper over a data record the controller has not retired. */
    model_reset();
    CHECK("a stick is taken", msc_attach(0, 1, 0, EP_IN, EP_OUT) == 0);
    CHECK("and comes up", run_until_settled(200));
    {
        struct block_request *r = make_request(0, 0, 2, page);
        int want[3] = {ORDER_WRAPPER, ORDER_QUIESCE + EP_IN, ORDER_WRAPPER};
        int i;

        block.completions = 0;
        dev.residue = 100;
        keep_data_td = 1;
        orders = 0;
        CHECK("a read is taken", msc_block_device(0)->submit(msc_block_device(0), r) == 0);
        for (i = 0; i < 40 && block.completions == 0; i++) {
            pump(1);
        }
        CHECK("a short read over an unretired record fails",
              block.completions == 1 && block.last_status == -EIO);
        CHECK("with its endpoint stopped first",
              quiesces == 1 && quiesce_order[0] == EP_IN && pages_reused == 0);
        keep_data_td = 0;
        r = make_request(0, 4, 1, page);
        block.completions = 0;
        CHECK("the next read is taken", msc_block_device(0)->submit(msc_block_device(0), r) == 0);
        for (i = 0; i < 40 && block.completions == 0; i++) {
            pump(1);
        }
        CHECK("and completes", block.completions == 1 && block.last_status == 0);
        CHECK("with its wrapper sent after the stop", order_is(want, 3));
    }
    /* A failed wrapper over such a record, then REQUEST SENSE. */
    model_reset();
    CHECK("a stick is taken", msc_attach(0, 1, 0, EP_IN, EP_OUT) == 0);
    CHECK("and comes up", run_until_settled(200));
    {
        struct block_request *r = make_request(0, 0, 1, page);
        /* The sense command's data record is kept unretired too. */
        int want[4] = {ORDER_WRAPPER, ORDER_QUIESCE + EP_IN, ORDER_WRAPPER, ORDER_QUIESCE + EP_IN};
        int i;

        block.completions = 0;
        dev.fail_next_data = 1;
        dev.sense_key = 0x03; /* a medium error: the request fails */
        keep_data_td = 1;
        orders = 0;
        CHECK("a read is taken", msc_block_device(0)->submit(msc_block_device(0), r) == 0);
        for (i = 0; i < 40 && block.completions == 0; i++) {
            pump(1);
        }
        CHECK("a read the stick failed over an unretired record fails",
              block.completions == 1 && block.last_status == -EIO);
        CHECK("after the stop, then the sense command",
              order_is(want, 4) && dev.senses == 1 && pages_reused == 0);
    }
    /* The status wrapper's own transfer outstanding at a timeout. */
    model_reset();
    CHECK("a stick is taken", msc_attach(0, 1, 0, EP_IN, EP_OUT) == 0);
    CHECK("and comes up", run_until_settled(200));
    {
        struct block_request *r = make_request(0, 0, 1, page);
        int want[8] = {ORDER_WRAPPER,        ORDER_QUIESCE + EP_IN,  ORDER_QUIESCE + EP_OUT,
                       ORDER_RESET,          ORDER_CLEAR + EP_IN,    ORDER_DEVCLEAR + EP_IN,
                       ORDER_CLEAR + EP_OUT, ORDER_DEVCLEAR + EP_OUT};
        int i;

        block.completions = 0;
        dev.vanish_at = 3;
        orders = 0;
        CHECK("a read is taken", msc_block_device(0)->submit(msc_block_device(0), r) == 0);
        for (i = 0; i < 40 && block.completions == 0; i++) {
            pump(MSC_STEP_MS / 2);
        }
        CHECK("a status wrapper that never answers fails the read once, after the recovery",
              block.completions == 1 && block.last_status == -EIO && order_is(want, 8) &&
                  msc_ready(0) && pages_reused == 0);
    }
    /* A completion delivered twice, or with an earlier stage's generation. */
    model_reset();
    CHECK("a stick is taken", msc_attach(0, 1, 0, EP_IN, EP_OUT) == 0);
    CHECK("and comes up", run_until_settled(200));
    {
        struct block_request *r = make_request(0, 0, 1, page);
        struct usb_completion again;
        int i;

        block.completions = 0;
        dev.vanish_at = 3;
        CHECK("a read is taken", msc_block_device(0)->submit(msc_block_device(0), r) == 0);
        for (i = 0; i < 10 && sticks[0].stage != STAGE_CSW; i++) {
            pump(1);
        }
        CHECK("and waits for its status wrapper", sticks[0].stage == STAGE_CSW);
        again.hc = 0;
        again.device = 1;
        again.endpoint = EP_IN;
        again.status = USB_XFER_OK;
        again.length = SECTOR;
        again.residual = 0;
        again.generation = sticks[0].data_gen;
        CHECK("the data stage's completion delivered again is taken", msc_completion(&again) == 1);
        CHECK("and moves nothing, though it is on the wrapper's endpoint",
              sticks[0].stage == STAGE_CSW && sticks[0].recover == RECOVER_NONE &&
                  block.completions == 0);
    }

    /* A completion after the stick has gone, and one after its reuse. */
    model_reset();
    CHECK("a stick is taken", msc_attach(0, 1, 0, EP_IN, EP_OUT) == 0);
    CHECK("and comes up", run_until_settled(200));
    {
        struct block_request *r = make_request(0, 0, 1, page);
        struct usb_completion old;
        int i;

        block.completions = 0;
        dev.vanish_at = 2;
        CHECK("a read is taken", msc_block_device(0)->submit(msc_block_device(0), r) == 0);
        pump(1);
        pump(1);
        old.hc = 0;
        old.device = 1;
        old.endpoint = EP_IN;
        old.status = USB_XFER_OK;
        old.length = SECTOR;
        old.residual = 0;
        old.generation = sticks[0].data_gen;
        msc_detach(0, 1);
        CHECK("a completion for a stick that has gone is taken", msc_completion(&old) == 1);
        CHECK("and completes nothing", block.completions == 0 && pages_reused == 0);
        close_slot(1);
        msc_release(0);
        CHECK("the slot goes back", msc_block_device(0) == 0 && block.completions == 1);
        /* The same handle, the same bring-up, the same request. */
        dev.attached = 1;
        dev.vanish_at = 0;
        CHECK("a new stick is taken on the same handle", msc_attach(0, 1, 0, EP_IN, EP_OUT) == 0);
        CHECK("and comes up", run_until_settled(200));
        r = make_request(0, 0, 1, page);
        block.completions = 0;
        dev.vanish_at = 2;
        CHECK("its read is taken", msc_block_device(0)->submit(msc_block_device(0), r) == 0);
        pump(1);
        pump(1);
        CHECK("and its data stage is in flight", sticks[0].stage == STAGE_DATA);
        CHECK("the old stick's completion is taken", msc_completion(&old) == 1);
        CHECK("and moves the new stick's command nowhere",
              sticks[0].stage == STAGE_DATA && block.completions == 0);
        dev.vanish_at = 0;
    }

    initialization_deadline();

    if (fails == 0) {
        printf("msc_hosttest: OK\n");
        return 0;
    }
    printf("msc_hosttest: %d failure(s)\n", fails);
    return 1;
}
