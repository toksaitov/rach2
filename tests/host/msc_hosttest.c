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
 * it, and the pump below hands the completion back the way the USB core's
 * pump does, so every row is one command's worth of passes and the order the
 * driver puts its stages in is what the model sees.
 */

extern int printf(const char *fmt, ...);

#include "../../rach2/sys/usb/msc.h"

#include "../../rach2/sys/block/block.h"
#include "../../rach2/sys/usb/hcd.h"

/* ---- what the driver reaches for outside itself ---- */

void klog(int level, const char *subsystem, const char *format, ...)
{
    (void)level;
    (void)subsystem;
    (void)format;
}

#include "../../rach2/sys/lib/kstring.h"

void *k_memset(void *dst, char value, size_t length)
{
    unsigned char *p = (unsigned char *)dst;
    size_t i;

    for (i = 0; i < length; i++) {
        p[i] = (unsigned char)value;
    }
    return dst;
}

/* ---- the model ---- */

#define MEDIUM_SECTORS 64u
#define SECTOR         512u

/* What the model answers, one knob per case a row needs. */
static struct {
    int attached; /* the device is on the bus */
    int max_lun;  /* what Get Max LUN answers */
    int stall_max_lun;
    int lun_type[4];        /* the SCSI device type of each logical unit */
    int lun_ready_after[4]; /* how many TEST UNIT READY it fails first */
    int lun_no_medium[4];   /* its sense says the medium is absent */
    int tests_seen[4];
    uint32_t last_block; /* what READ CAPACITY reports */
    uint32_t block_size;
    int mode_sense_stalls;
    int write_protect;
    int sync_illegal;   /* SYNCHRONIZE CACHE answers ILLEGAL REQUEST */
    int sync_fails;     /* and this one fails outright */
    int fail_next_data; /* the next data stage fails with a check condition */
    int stall_stage;    /* which stage stalls: 1 wrapper, 2 data, 3 status */
    int stall_times;    /* how many times it does */
    int bad_signature;  /* the next status wrapper has the wrong signature */
    int bad_tag;
    int bad_status; /* a status byte outside 0 to 2 */
    int phase_error;
    int short_status;
    int residue_over; /* a residue larger than the transfer */
    int residue;      /* a residue this many bytes on the next command */
    int vanish_at;    /* the stage at which the device stops answering */
    int resets;       /* class resets seen */
    int cleared_in, cleared_out;
    int clear_order[8];
    int clears;
    int capacity_lun; /* the unit whose capacity was asked for */
    unsigned char medium[MEDIUM_SECTORS * SECTOR];
} dev;

/* The transfer the driver has submitted and the completion it is owed. */
static struct {
    int pending;
    struct usb_completion completion;
} wire;

/* What the model is part way through: a command block wrapper it has read
   and the data stage it expects next. */
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

static uint32_t rd32(const unsigned char *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void wr32(unsigned char *p, uint32_t v)
{
    p[0] = (unsigned char)v;
    p[1] = (unsigned char)(v >> 8);
    p[2] = (unsigned char)(v >> 16);
    p[3] = (unsigned char)(v >> 24);
}

static uint32_t rd32_be(const unsigned char *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static void wr32_be(unsigned char *p, uint32_t v)
{
    p[0] = (unsigned char)(v >> 24);
    p[1] = (unsigned char)(v >> 16);
    p[2] = (unsigned char)(v >> 8);
    p[3] = (unsigned char)v;
}

/** Hand the driver the answer to the transfer it submitted. */
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
}

/* The endpoint numbers this model answers on, as the core would have given
   them: the control endpoint is 1, and the bulk pair is 3 and 4. */
#define EP_CONTROL 1
#define EP_IN      3
#define EP_OUT     4

/** Which stage a transfer on `endpoint` is, for the vanishing knob. */
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

/** The SCSI target: answer one command's data stage into `buffer`. */
static void scsi_data_in(unsigned char *buffer, uint32_t length)
{
    uint32_t i;

    for (i = 0; i < length; i++) {
        buffer[i] = 0;
    }
    switch (cmd.cdb[0]) {
    case 0x12: /* INQUIRY */
        buffer[0] = (unsigned char)dev.lun_type[cmd.lun];
        buffer[4] = 31;
        return;
    case 0x03: /* REQUEST SENSE */
        buffer[0] = 0x70;
        if (dev.lun_no_medium[cmd.lun]) {
            buffer[2] = 0x02;  /* not ready */
            buffer[12] = 0x3A; /* medium not present */
        } else if (dev.fail_next_data) {
            buffer[2] = 0x03; /* medium error */
            buffer[12] = 0x11;
        } else {
            buffer[2] = 0x02;
            buffer[12] = 0x04; /* not ready, becoming ready */
        }
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

/** And take one command's data stage out of `buffer`. */
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

/** What the status wrapper of the command in flight says. */
static int scsi_status(void)
{
    switch (cmd.cdb[0]) {
    case 0x00: /* TEST UNIT READY */
        dev.tests_seen[cmd.lun]++;
        if (dev.lun_no_medium[cmd.lun]) {
            return 1;
        }
        return dev.tests_seen[cmd.lun] > dev.lun_ready_after[cmd.lun] ? 0 : 1;
    case 0x35: /* SYNCHRONIZE CACHE */
        return (dev.sync_illegal || dev.sync_fails) ? 1 : 0;
    case 0x28:
    case 0x2A:
        return dev.fail_next_data ? 1 : 0;
    default:
        return 0;
    }
}

int xhci_submit(int device, int endpoint, const struct usb_transfer *transfer)
{
    int stage = stage_of(endpoint, transfer);

    (void)device;
    if (!dev.attached) {
        return -1;
    }
    if (dev.vanish_at != 0 && dev.vanish_at == stage) {
        /* The device is pulled at this stage: the transfer is submitted and
           nothing ever answers it, which is what the driver's deadline is
           for. */
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
            answer(endpoint, USB_XFER_OK, 0, 0, transfer->generation);
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
        wr32(&w[8], dev.residue_over ? cmd.length + 512u : cmd.residue);
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

/*
 * The core clears a halt on the class driver's behalf, because the
 * controller takes one command at a time for the whole stack. Here it
 * answers at once and records which endpoint, in order: the specification
 * says the inward one first, and a driver that cleared them the other way
 * round leaves a device that answers nothing again.
 */
int usb_core_clear_halt(int device, int endpoint)
{
    (void)device;
    if (dev.clears < 8) {
        dev.clear_order[dev.clears] = endpoint;
    }
    dev.clears++;
    if (endpoint == EP_IN) {
        dev.cleared_in++;
    } else {
        dev.cleared_out++;
    }
    return 1;
}

/*
 * The disk layer's side. The class driver tells it that a stick is up and
 * that one has gone; here the two are counted, because the rows check that
 * a stick that never became ready is never announced and that one that goes
 * is announced exactly once.
 */
static int arrivals;
static int departures;

void usbdisk_arrived(int slot)
{
    (void)slot;
    arrivals++;
}

void usbdisk_left(int slot)
{
    (void)slot;
    departures++;
}

/* The block layer's side, recorded rather than driven. */
static struct {
    int completions;
    int last_status;
    struct block_request *last;
} block;

void block_request_complete(struct block_request *request, uint32_t generation, int status)
{
    (void)generation;
    block.completions++;
    block.last_status = status;
    block.last = request;
    if (request != 0) {
        request->status = status;
        request->state = BLOCK_REQ_DONE;
    }
}

#include "../../rach2/sys/usb/msc.c"

/* ---- the pump, and the rows ---- */

/**
 * One turn of the pump: the completion the model owes, then a step.
 *
 * The USB core's pump does exactly this, in this order, which is what makes
 * a command's next stage go out in the same pass as the answer to the one
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

/** Turn the pump until the stick is ready, refused, or the turns run out. */
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
    wire.pending = 0;
    block.completions = 0;
    block.last_status = 0;
    block.last = 0;

    /* The driver's own state, which is static and outlives a row. A stick
     * that goes leaves its slot taken until the disk layer says nothing
     * holds it, so the reset does what that layer would. */
    for (i = 0; i < MSC_MAX_DEVICES; i++) {
        msc_detach(1 + i);
    }
    for (i = 0; i < MSC_MAX_DEVICES; i++) {
        msc_release(i);
    }
    arrivals = 0;
    departures = 0;
}

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

int main(void)
{
    static unsigned char page[8 * SECTOR];

    /* A stick that comes up at once. */
    model_reset();
    CHECK("a stick is taken", msc_attach(1, 0, EP_IN, EP_OUT) == 0);
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

    /* A stick that fails TEST UNIT READY a dozen times, which is what a real
       one does for a moment after it is plugged in. */
    model_reset();
    dev.lun_ready_after[0] = 12;
    CHECK("a stick that is not ready yet is taken", msc_attach(1, 0, EP_IN, EP_OUT) == 0);
    CHECK("and comes up when it becomes ready", run_until_settled(4000));
    CHECK("after the tests it needed", dev.tests_seen[0] == 13);

    /* One that never becomes ready is refused at the deadline. */
    model_reset();
    dev.lun_ready_after[0] = 100000;
    CHECK("a stick that never becomes ready is taken", msc_attach(1, 0, EP_IN, EP_OUT) == 0);
    CHECK("and is not brought up", run_until_settled(30000) == 0);
    CHECK("and serves nothing", !msc_ready(0));
    CHECK("and the disk layer is never told of it", arrivals == 0);

    /* Get Max LUN stalled, which most sticks do, is one logical unit. */
    model_reset();
    dev.stall_max_lun = 1;
    CHECK("a stick that will not say how many units it has is taken",
          msc_attach(1, 0, EP_IN, EP_OUT) == 0);
    CHECK("and comes up on the one it has", run_until_settled(100));

    /* A promotional drive: a CD-ROM on the first unit and the flash on the
       second. */
    model_reset();
    dev.max_lun = 1;
    dev.lun_type[0] = 0x05; /* a CD-ROM */
    dev.lun_type[1] = 0x00;
    dev.capacity_lun = -1;
    CHECK("a drive with a CD-ROM in front of its flash is taken",
          msc_attach(1, 0, EP_IN, EP_OUT) == 0);
    CHECK("and comes up", run_until_settled(200));
    CHECK("on the unit that is a disk and not on the one that is not", dev.capacity_lun == 1);

    /* A card reader whose first slot is empty. */
    model_reset();
    dev.max_lun = 1;
    dev.lun_no_medium[0] = 1;
    dev.capacity_lun = -1;
    CHECK("a reader with an empty slot is taken", msc_attach(1, 0, EP_IN, EP_OUT) == 0);
    CHECK("and comes up", run_until_settled(200));
    CHECK("on the slot that has a card", dev.capacity_lun == 1);

    /* The capacity rules. */
    model_reset();
    dev.last_block = 0xFFFFFFFFu;
    CHECK("a drive larger than a ten-byte read can address is taken",
          msc_attach(1, 0, EP_IN, EP_OUT) == 0);
    CHECK("and refused", run_until_settled(200) == 0);

    model_reset();
    dev.block_size = 4096;
    CHECK("a drive with 4 KiB blocks is taken", msc_attach(1, 0, EP_IN, EP_OUT) == 0);
    CHECK("and refused", run_until_settled(200) == 0);

    /* Write protection, and the drive that will not answer the question. */
    model_reset();
    dev.write_protect = 1;
    CHECK("a protected stick is taken", msc_attach(1, 0, EP_IN, EP_OUT) == 0);
    CHECK("and comes up", run_until_settled(200));
    CHECK("read-only", (msc_block_device(0)->flags & BLOCK_READ_ONLY) != 0);
    {
        struct block_request *r = make_request(1, 0, 1, page);

        CHECK("and refuses a write without sending one",
              msc_block_device(0)->submit(msc_block_device(0), r) == -EROFS);
    }

    /* The status wrapper's rules, each of which is reset recovery. */
    model_reset();
    CHECK("a stick is taken", msc_attach(1, 0, EP_IN, EP_OUT) == 0);
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

    /* A device pulled between the command and its answer: every request it
       had is failed, and nothing of it is used again. */
    model_reset();
    CHECK("a stick is taken", msc_attach(1, 0, EP_IN, EP_OUT) == 0);
    CHECK("and comes up", run_until_settled(200));
    {
        struct block_request *r = make_request(0, 0, 1, page);

        block.completions = 0;
        dev.vanish_at = 2; /* it goes during the data stage */
        CHECK("a read is taken", msc_block_device(0)->submit(msc_block_device(0), r) == 0);
        pump(1);
        pump(1);
        msc_detach(1);
        CHECK("the read is failed when the stick goes",
              block.completions == 1 && block.last_status == -EIO);
        CHECK("and the stick stops serving", msc_count() == 0 && !msc_ready(0));
        CHECK("the disk layer is told once", departures == 1);
        /* The slot is not free yet. A mount, a description and a held buffer
         * all point at this block device, and a second stick given this slot
         * would be given their pointers with it. */
        CHECK("its device is kept until the disk layer releases it", msc_block_device(0) != 0);
        CHECK("and refuses what it is asked for",
              msc_block_device(0)->submit(msc_block_device(0), r) == -EIO);
        CHECK("a second stick is given a different slot",
              msc_attach(2, 0, EP_IN, EP_OUT) == 0 && msc_block_device(1) != 0 &&
                  msc_block_device(1) != msc_block_device(0));
        msc_release(0);
        CHECK("the slot goes back when the last hold does", msc_block_device(0) == 0);
    }

    /* Two requests, admitted in the order they arrived. */
    model_reset();
    CHECK("a stick is taken", msc_attach(1, 0, EP_IN, EP_OUT) == 0);
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

    /* A read whose data stage came up short is a failure: a block is whole
       or it is not. */
    model_reset();
    CHECK("a stick is taken", msc_attach(1, 0, EP_IN, EP_OUT) == 0);
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

    /* A status wrapper that stalls once is read again; one that stalls twice
       is reset recovery, because a device that will not give its status is a
       device in a state this driver cannot reason about. */
    model_reset();
    CHECK("a stick is taken", msc_attach(1, 0, EP_IN, EP_OUT) == 0);
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

    /* Every stage of one command goes out in the pass that answers the one
       before it. A driver that advanced a stage a tick would take three
       ticks for a read, and a request of eight sectors would cost thirty
       milliseconds of nothing. */
    model_reset();
    CHECK("a stick is taken", msc_attach(1, 0, EP_IN, EP_OUT) == 0);
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

    /* A completion for a command this driver gave up on: the block layer has
       the slot back and its page may be another request's, so the answer is
       dropped by the number the transfer carried. */
    model_reset();
    CHECK("a stick is taken", msc_attach(1, 0, EP_IN, EP_OUT) == 0);
    CHECK("and comes up", run_until_settled(200));
    {
        struct block_request *r = make_request(0, 0, 1, page);
        struct usb_completion late;
        int i;

        dev.vanish_at = 2;
        block.completions = 0;
        CHECK("a read is taken", msc_block_device(0)->submit(msc_block_device(0), r) == 0);
        for (i = 0; i < 40 && block.completions == 0; i++) {
            pump(20000); /* past the driver's own deadline */
        }
        CHECK("the read fails at the deadline",
              block.completions == 1 && block.last_status == -EIO);
        /* A fresh request, and the abandoned transfer's answer arriving
           while its first stage is in flight. Taken as this command's, it
           would advance a stage that has not happened and the read would
           come back with whatever the page held. */
        dev.vanish_at = 0;
        r = make_request(0, 3, 1, page);
        page[0] = 0xEE;
        block.completions = 0;
        CHECK("a fresh read is taken", msc_block_device(0)->submit(msc_block_device(0), r) == 0);
        late.device = 1;
        late.endpoint = EP_IN;
        /* The abandoned transfer fails, as one on a device that went away
           does. Taken as this command's, it would send the device through
           reset recovery and fail a read that has nothing wrong with it. */
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

    /* Nothing resets a drive while it is being brought up: there are drives
       that stop answering for good when that is done to them. */
    model_reset();
    dev.lun_ready_after[0] = 4;
    CHECK("a stick that takes a moment is taken", msc_attach(1, 0, EP_IN, EP_OUT) == 0);
    CHECK("and comes up", run_until_settled(2000));
    CHECK("with no reset sent while it did", dev.resets == 0);

    /* The flush, and the two answers that are not failures. */
    model_reset();
    CHECK("a stick is taken", msc_attach(1, 0, EP_IN, EP_OUT) == 0);
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
    }

    if (fails == 0) {
        printf("msc_hosttest: OK\n");
        return 0;
    }
    printf("msc_hosttest: %d failure(s)\n", fails);
    return 1;
}
