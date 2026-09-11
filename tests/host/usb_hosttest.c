/*
 * usb_hosttest.c: the standard half of the USB stack, with no controller.
 *
 * What a device says about itself arrives as a string of descriptors, each
 * naming its own length, and the only way through them is to trust those
 * lengths. A device that lies about one is not a device this kernel will ever
 * be given in QEMU: the emulated keyboard and mouse are correct, and so is
 * every real one that works elsewhere. A malformed descriptor is what a broken
 * device, or a hostile one, sends.
 *
 * So the walk is compiled natively and given descriptors no device would send,
 * in buffers allocated to the exact length, under AddressSanitizer. A read
 * past the end is then a failure rather than a byte of whatever follows.
 */

extern int printf(const char *fmt, ...);
extern void *malloc(unsigned long size);
extern void free(void *p);

#include "../../rach2/sys/usb/core.h"
#include "../../rach2/sys/usb/hub.h"

/* ---- what the core reaches for outside itself ---- */

#include "../../rach2/sys/usb/xhci.h"

void klog(int level, const char *subsystem, const char *format, ...)
{
    (void)level;
    (void)subsystem;
    (void)format;
}

#include "../../rach2/sys/lib/kdef.h"

/* The kernel's fill, which the core reaches and this test does not build. */
void *k_memset(void *ptr, char c, size_t n)
{
    unsigned char *p = (unsigned char *)ptr;

    while (n-- > 0) {
        *p++ = (unsigned char)c;
    }
    return ptr;
}

/*
 * The identity-mapped frames a device's record lives in. On the host they are
 * pages of this program's memory; nothing here is given to a controller, so
 * the address it would be reached by is the only one that matters.
 */
/*
 * The driver's table of controllers, as the stack asks about it. These tests
 * drive the layers above the driver through their own stubs, so there is one
 * controller and its record is never dereferenced.
 */
#define FRAME_COUNT 64
static uint8_t frames[FRAME_COUNT][4096] __attribute__((aligned(4096)));
static uint8_t frame_taken[FRAME_COUNT];

void *alloc_kernel_frame(void)
{
    unsigned i;

    for (i = 0; i < FRAME_COUNT; i++) {
        if (!frame_taken[i]) {
            frame_taken[i] = 1;
            return &frames[i][0];
        }
    }
    return 0;
}

void free_kernel_frame(void *frame)
{
    unsigned i;

    for (i = 0; i < FRAME_COUNT; i++) {
        if (frame == &frames[i][0]) {
            frame_taken[i] = 0;
            return;
        }
    }
}

static struct xhci_ctrl *the_ctrl = (struct xhci_ctrl *)1;

int xhci_controllers(void)
{
    return 1;
}

struct xhci_ctrl *xhci_controller(int n)
{
    return n == 0 ? the_ctrl : 0;
}

int xhci_number(const struct xhci_ctrl *hc)
{
    return hc == the_ctrl ? 0 : -1;
}

int xhci_ports(struct xhci_ctrl *hc)
{
    return 0;
}

/* One root port with a device on it, for the rows that step a running
   device: the port scan asks for none, since `xhci_ports` answers zero. */
static int port_present;

int xhci_port_status(struct xhci_ctrl *hc, int port, struct usb_port_status *out)
{
    (void)port;
    if (!port_present) {
        return -1;
    }
    out->connected = 1;
    out->enabled = 1;
    out->speed = USB_SPEED_HIGH;
    return 0;
}

int xhci_port_settled(struct xhci_ctrl *hc, int port)
{
    (void)port;
    return 0;
}

int xhci_port_reset_begin(struct xhci_ctrl *hc, int port)
{
    (void)port;
    return -1;
}

int xhci_port_reset_done(struct xhci_ctrl *hc, int port)
{
    (void)port;
    return -1;
}

int xhci_device_hub(struct xhci_ctrl *hc, int device, int ports, int multi_tt, int think_time)
{
    (void)device;
    (void)ports;
    (void)multi_tt;
    (void)think_time;
    return 0;
}

int xhci_device_open(struct xhci_ctrl *hc, const struct usb_location *where, int speed)
{
    (void)where;
    (void)speed;
    return -1;
}

int xhci_device_address(struct xhci_ctrl *hc, int device)
{
    (void)device;
    return -1;
}

int xhci_device_packet_size(struct xhci_ctrl *hc, int device, int max_packet)
{
    (void)device;
    (void)max_packet;
    return -1;
}

/*
 * The controller's command ring, as the core sees it: one command in flight
 * at a time, issued by the calls below and answered when the row says so.
 * What was issued is recorded in order, for the rows that check which
 * command came first and that the turn was held across them.
 */
#define CMD_STOP    1
#define CMD_RESET   2
#define CMD_DEQUEUE 3
#define CMD_DISABLE 4
static struct {
    int pending;      /* a command issued and not yet answered */
    int answer;       /* 1 to answer it with `status`, 0 to leave it */
    int status;       /* the answer's status */
    int refuse_issue; /* every issue answers -1 */
    int issued[8];
    int endpoint[8]; /* the endpoint each command named */
    int count;
    int ep_state; /* the endpoint's state, as the driver would report it */
    int quiescing_on, quiescing_off;
    int quiescing_endpoint; /* the endpoint the last flag call named */
    int quiescing[32];      /* the flag per endpoint */
    int retired_all;
    int submits; /* transfers the core tried to submit */
} ring;

/*
 * Issue a command on `endpoint`, with the preconditions the driver keeps: a
 * stop is issued only on a running endpoint, a reset only on a halted one,
 * a dequeue only on one that is stopped or in the Error state (xHCI 1.2,
 * 4.6.9, 4.6.8, 4.6.10); the endpoint's state moves on with the command's
 * success.
 */
static int ring_issue(int what, int endpoint)
{
    if (ring.refuse_issue || ring.pending) {
        return -1;
    }
    if ((what == CMD_STOP && ring.ep_state != XHCI_EP_RUNNING) ||
        (what == CMD_RESET && ring.ep_state != XHCI_EP_HALTED) ||
        (what == CMD_DEQUEUE && ring.ep_state != XHCI_EP_STOPPED &&
         ring.ep_state != XHCI_EP_ERROR)) {
        return -1;
    }
    if (ring.count < 8) {
        ring.issued[ring.count] = what;
        ring.endpoint[ring.count] = endpoint;
    }
    ring.count++;
    ring.pending = 1;
    return 0;
}

static void ring_reset(void)
{
    int i;

    for (i = 0; i < 8; i++) {
        ring.issued[i] = 0;
    }
    ring.pending = 0;
    ring.answer = 0;
    ring.status = USB_XFER_OK;
    ring.refuse_issue = 0;
    ring.count = 0;
    ring.ep_state = XHCI_EP_RUNNING;
    ring.quiescing_on = 0;
    ring.quiescing_off = 0;
    ring.quiescing_endpoint = 0;
    for (i = 0; i < 32; i++) {
        ring.quiescing[i] = 0;
    }
    ring.retired_all = 0;
    ring.submits = 0;
}

int xhci_device_close(struct xhci_ctrl *hc, int device)
{
    (void)device;
    return ring_issue(CMD_DISABLE, 0);
}

void xhci_device_closed(struct xhci_ctrl *hc, int device)
{
    (void)device;
}

int xhci_command_busy(struct xhci_ctrl *hc)
{
    return 0;
}

int xhci_endpoint_open(struct xhci_ctrl *hc, int device, const struct usb_endpoint *endpoint)
{
    (void)device;
    (void)endpoint;
    return -1;
}

int xhci_command_done(struct xhci_ctrl *hc, struct usb_command_result *out)
{
    if (!ring.pending || !ring.answer) {
        return 0;
    }
    ring.pending = 0;
    ring.answer = 0;
    out->status = ring.status;
    out->code = 0;
    out->device = 0;
    if (ring.status == USB_XFER_OK && ring.count > 0 && ring.count <= 8) {
        int what = ring.issued[ring.count - 1];

        if (what == CMD_STOP || what == CMD_RESET || what == CMD_DEQUEUE) {
            ring.ep_state = XHCI_EP_STOPPED;
        }
    }
    return 1;
}

int xhci_submit(struct xhci_ctrl *hc, int device, int endpoint, const struct usb_transfer *transfer)
{
    (void)device;
    (void)transfer;
    if (ring.quiescing[endpoint & 31]) {
        return -1; /* what the driver does while the core quiesces the endpoint */
    }
    ring.submits++;
    return 0;
}

/* The class driver the core hands a stick to. This test drives the core's
   own walk of the descriptors and its enumeration, not that driver, so the
   stubs record what they were told and nothing more. */
int msc_attach(struct xhci_ctrl *hc, int device, int interface, int ep_in, int ep_out)
{
    (void)device;
    (void)interface;
    (void)ep_in;
    (void)ep_out;
    return 0;
}

void msc_detach(struct xhci_ctrl *hc, int device)
{
    (void)hc;
    (void)device;
}

int xhci_command_free(struct xhci_ctrl *hc)
{
    return !ring.pending;
}

int xhci_interrupts_on(struct xhci_ctrl *hc)
{
    return 0; /* the model's controller sends no interrupts of its own */
}

void xhci_state_record(struct xhci_ctrl *hc)
{}

int hub_attach(struct xhci_ctrl *hc, int index, int device, int interface, int protocol, int ep_dci,
               int depth, int super)
{
    (void)index;
    (void)device;
    (void)interface;
    (void)protocol;
    (void)depth;
    (void)super;
    (void)ep_dci;
    return -1;
}

void hub_detach(int index)
{
    (void)index;
}

void hub_step(uint32_t elapsed_ms)
{
    (void)elapsed_ms;
}

int hub_completion(const struct usb_completion *completion)
{
    (void)completion;
    return 0;
}

int hub_port_status(struct xhci_ctrl *hc, int owner, int port, struct usb_port_status *out)
{
    (void)owner;
    (void)port;
    out->connected = 0;
    return -1;
}

int hub_port_settled(struct xhci_ctrl *hc, int owner, int port)
{
    (void)owner;
    (void)port;
    return 0;
}

int hub_port_reset_begin(struct xhci_ctrl *hc, int owner, int port)
{
    (void)owner;
    (void)port;
    return -1;
}

int hub_port_reset_done(struct xhci_ctrl *hc, int owner, int port)
{
    (void)owner;
    (void)port;
    return -1;
}

void usbdisk_step(void)
{}

void msc_step(uint32_t elapsed_ms)
{
    (void)elapsed_ms;
}

int msc_completion(const struct usb_completion *completion)
{
    (void)completion;
    return 0;
}

int xhci_poll(struct xhci_ctrl *hc, struct usb_completion *out)
{
    (void)out;
    return 0;
}

int xhci_endpoint_reset(struct xhci_ctrl *hc, int device, int endpoint)
{
    (void)device;
    return ring_issue(CMD_RESET, endpoint);
}

int xhci_endpoint_stop(struct xhci_ctrl *hc, int device, int endpoint)
{
    (void)device;
    return ring_issue(CMD_STOP, endpoint);
}
int xhci_endpoint_dequeue_here(struct xhci_ctrl *hc, int device, int endpoint)
{
    (void)device;
    return ring_issue(CMD_DEQUEUE, endpoint);
}
int xhci_endpoint_state(struct xhci_ctrl *hc, int device, int endpoint)
{
    (void)device;
    (void)endpoint;
    return ring.ep_state;
}
void xhci_endpoint_quiescing(struct xhci_ctrl *hc, int device, int endpoint, int on)
{
    (void)device;
    ring.quiescing_endpoint = endpoint;
    ring.quiescing[endpoint & 31] = on ? 1 : 0;
    if (on) {
        ring.quiescing_on++;
    } else {
        ring.quiescing_off++;
    }
}
void xhci_endpoint_retire_all(struct xhci_ctrl *hc, int device, int endpoint)
{
    (void)device;
    (void)endpoint;
    ring.retired_all++;
}
/* The stick driver's word that a slot was given back, counted. */
static int slots_closed;
void msc_slot_closed(struct xhci_ctrl *hc, int device)
{
    (void)device;
    slots_closed++;
}

/* The class driver, which this test does not exercise: what it does with a
   report is its own file's business, and this one is about the walk. */
void hid_report(int slot, int kind, const uint8_t *report, int length)
{
    (void)slot;
    (void)kind;
    (void)report;
    (void)length;
}

void hid_gone(int slot)
{
    (void)slot;
}

void input_mouse_found(void)
{}

#include "../../rach2/sys/usb/core.c"

/* ---- the rows ---- */

static int fails;

#define CHECK(what, cond)                 \
    do {                                  \
        if (!(cond)) {                    \
            printf("  FAIL: %s\n", what); \
            fails++;                      \
        }                                 \
    } while (0)

/*
 * A configuration exactly as a boot keyboard sends it: the configuration
 * itself, one interface of class 3 subclass 1 protocol 1, the HID descriptor
 * the class adds, and one interrupt IN endpoint.
 */
static const unsigned char keyboard_config[] = {
    9, 2,  34,   0, 1, 1, 0,  0xA0, 50, /* configuration, value 1 */
    9, 4,  0,    0, 1, 3, 1,  1,    0,  /* interface: HID, boot, keyboard */
    9, 33, 0x11, 1, 0, 1, 34, 63,   0,  /* the HID descriptor, which is skipped */
    7, 5,  0x81, 3, 8, 0, 10            /* endpoint 1, in, interrupt, 8 bytes */
};

/** Give the walker its own buffer of exactly the right size. */
static void walk(const unsigned char *bytes, int length, struct usb_interface_info *out)
{
    unsigned char *copy = (unsigned char *)malloc((unsigned long)(length > 0 ? length : 1));
    int i;

    for (i = 0; i < length; i++) {
        copy[i] = bytes[i];
    }
    usb_parse_config(copy, length, out);
    free(copy);
}

/* ---- the quiesce, and the slot given back ---- */

/** 1 when the ring's record is exactly `want`, every endpoint command on 3. */
static int issued_is(const int *want, int n)
{
    int i;

    if (ring.count != n) {
        return 0;
    }
    for (i = 0; i < n; i++) {
        if (ring.issued[i] != want[i]) {
            return 0;
        }
        if (ring.issued[i] != CMD_DISABLE && ring.endpoint[i] != 3) {
            return 0;
        }
    }
    return 1;
}

/** A device the core did not enumerate, in `state` with `slot`, as a stick. */
static struct usb_device_state *stick_at(int i, int state, int slot)
{
    struct usb_device_state *d = dev_take(i);

    k_memset(d, 0, sizeof(*d));
    d->index = i;
    d->state = state;
    d->slot = slot;
    d->hc = the_ctrl;
    d->owner = -1;
    d->port = i;
    d->ops = &root_ops;
    d->interface.kind = USB_KIND_STORAGE;
    d->ep_dci = 3;
    d->ep_out_dci = 4;
    d->deadline = core_ms + STEP_MS;
    return d;
}

static void quiesce_rows(void)
{
    struct usb_device_state *d;

    /* A running endpoint: Stop Endpoint, then Set TR Dequeue, the turn held
       from the first to the answer of the second. */
    usb_core_reset();
    ring_reset();
    d = stick_at(0, ST_RUNNING, 1);
    {
        int want[2] = {CMD_STOP, CMD_DEQUEUE};

        CHECK("a quiesce issues a stop and answers pending",
              usb_core_endpoint_quiesce(the_ctrl, 1, 3) == 0 && issued_is(want, 1));
        CHECK("and holds the turn", enumerating[0] == 0);
        CHECK("and marks the endpoint quiescing, by number",
              ring.quiescing_on == 1 && ring.quiescing_endpoint == 3);
        {
            struct usb_transfer transfer;

            transfer.setup = 0;
            transfer.buffer = d->report;
            transfer.length = 8;
            transfer.direction = USB_DIR_IN;
            CHECK("so a submission on it is refused meanwhile",
                  xhci_submit(the_ctrl, 1, 3, &transfer) != 0 && ring.submits == 0);
            CHECK("and one on another endpoint is not",
                  xhci_submit(the_ctrl, 1, 4, &transfer) == 0 && ring.submits == 1);
        }
        CHECK("asked again, it waits for the stop's answer",
              usb_core_endpoint_quiesce(the_ctrl, 1, 3) == 0 && issued_is(want, 1));
        ring.answer = 1;
        ring.status = USB_XFER_OK;
        CHECK("the stop's success issues the dequeue and answers pending",
              usb_core_endpoint_quiesce(the_ctrl, 1, 3) == 0 && issued_is(want, 2));
        CHECK("with the turn still held", enumerating[0] == 0 && ring.retired_all == 0);
        ring.answer = 1;
        CHECK("the dequeue's success answers one", usb_core_endpoint_quiesce(the_ctrl, 1, 3) == 1);
        CHECK("retires the endpoint's transfers once", ring.retired_all == 1);
        CHECK("and clears the mark", ring.quiescing_off == 1);
        CHECK("and gives the turn back", enumerating[0] == -1 && d->quiesce_stage == 0);
    }

    /* Another device has the turn: nothing is issued until it is given back. */
    usb_core_reset();
    ring_reset();
    d = stick_at(0, ST_RUNNING, 1);
    enumerating[0] = 3;
    CHECK("with another device enumerating, a quiesce issues nothing and answers pending",
          usb_core_endpoint_quiesce(the_ctrl, 1, 3) == 0 && ring.count == 0);
    CHECK("and marks the endpoint quiescing while it waits", ring.quiescing_on == 1);
    enumerating[0] = -1;
    CHECK("and issues the stop once the turn is free",
          usb_core_endpoint_quiesce(the_ctrl, 1, 3) == 0 && ring.count == 1 && enumerating[0] == 0);

    /* A dequeue the driver will not issue, and one never answered. */
    usb_core_reset();
    ring_reset();
    d = stick_at(0, ST_RUNNING, 1);
    CHECK("the stop goes out", usb_core_endpoint_quiesce(the_ctrl, 1, 3) == 0);
    ring.answer = 1;
    ring.refuse_issue = 1;
    CHECK("a dequeue the driver refuses to issue answers -1",
          usb_core_endpoint_quiesce(the_ctrl, 1, 3) == -1);
    CHECK("with nothing retired and the turn given back",
          ring.retired_all == 0 && enumerating[0] == -1 && ring.quiescing_off == 1 &&
              ring.count == 1);
    usb_core_reset();
    ring_reset();
    d = stick_at(0, ST_RUNNING, 1);
    CHECK("the stop goes out", usb_core_endpoint_quiesce(the_ctrl, 1, 3) == 0);
    ring.answer = 1;
    CHECK("and the dequeue", usb_core_endpoint_quiesce(the_ctrl, 1, 3) == 0 && ring.count == 2);
    core_ms += STEP_MS;
    CHECK("a dequeue never answered answers -1 at the deadline",
          usb_core_endpoint_quiesce(the_ctrl, 1, 3) == -1);
    CHECK("with nothing retired and the turn given back",
          ring.retired_all == 0 && enumerating[0] == -1 && ring.quiescing_off == 1);

    /* An endpoint in the Error state needs only the dequeue. */
    usb_core_reset();
    ring_reset();
    d = stick_at(0, ST_RUNNING, 1);
    ring.ep_state = XHCI_EP_ERROR;
    {
        int want[1] = {CMD_DEQUEUE};

        CHECK("an endpoint in the Error state is only moved on",
              usb_core_endpoint_quiesce(the_ctrl, 1, 3) == 0 && issued_is(want, 1));
        ring.answer = 1;
        CHECK("and is the caller's",
              usb_core_endpoint_quiesce(the_ctrl, 1, 3) == 1 && ring.retired_all == 1);
    }

    /* A stop that fails: -1, nothing retired, the turn given back. */
    usb_core_reset();
    ring_reset();
    d = stick_at(0, ST_RUNNING, 1);
    CHECK("the stop goes out", usb_core_endpoint_quiesce(the_ctrl, 1, 3) == 0);
    ring.answer = 1;
    ring.status = USB_XFER_FAILED;
    CHECK("a stop answered with a failure answers -1",
          usb_core_endpoint_quiesce(the_ctrl, 1, 3) == -1);
    CHECK("with nothing retired and the turn given back",
          ring.retired_all == 0 && enumerating[0] == -1 && ring.quiescing_off == 1 &&
              ring.count == 1);

    /* A dequeue that fails, the same. */
    usb_core_reset();
    ring_reset();
    d = stick_at(0, ST_RUNNING, 1);
    CHECK("the stop goes out", usb_core_endpoint_quiesce(the_ctrl, 1, 3) == 0);
    ring.answer = 1;
    CHECK("and the dequeue", usb_core_endpoint_quiesce(the_ctrl, 1, 3) == 0 && ring.count == 2);
    ring.answer = 1;
    ring.status = USB_XFER_FAILED;
    CHECK("a dequeue answered with a failure answers -1",
          usb_core_endpoint_quiesce(the_ctrl, 1, 3) == -1);
    CHECK("with nothing retired and the turn given back",
          ring.retired_all == 0 && enumerating[0] == -1 && ring.quiescing_off == 1);

    /* A stop never answered: -1 at the deadline, nothing retired. */
    usb_core_reset();
    ring_reset();
    d = stick_at(0, ST_RUNNING, 1);
    CHECK("the stop goes out", usb_core_endpoint_quiesce(the_ctrl, 1, 3) == 0);
    core_ms += STEP_MS - 1;
    CHECK("and is waited for", usb_core_endpoint_quiesce(the_ctrl, 1, 3) == 0);
    core_ms += 1;
    CHECK("until the deadline, which answers -1", usb_core_endpoint_quiesce(the_ctrl, 1, 3) == -1);
    CHECK("with nothing retired and the turn given back",
          ring.retired_all == 0 && enumerating[0] == -1 && ring.quiescing_off == 1);

    /* A stop the controller driver refuses to issue: -1 at once. */
    usb_core_reset();
    ring_reset();
    d = stick_at(0, ST_RUNNING, 1);
    ring.refuse_issue = 1;
    CHECK("a stop refused by the driver answers -1",
          usb_core_endpoint_quiesce(the_ctrl, 1, 3) == -1);
    CHECK("with the turn given back", enumerating[0] == -1 && ring.quiescing_off == 1);

    /* A halted endpoint is reset rather than stopped; a stopped one needs
       only the dequeue. */
    usb_core_reset();
    ring_reset();
    d = stick_at(0, ST_RUNNING, 1);
    ring.ep_state = XHCI_EP_HALTED;
    {
        int want[2] = {CMD_RESET, CMD_DEQUEUE};

        CHECK("a halted endpoint is reset first",
              usb_core_endpoint_quiesce(the_ctrl, 1, 3) == 0 && issued_is(want, 1));
        ring.answer = 1;
        CHECK("then moved on",
              usb_core_endpoint_quiesce(the_ctrl, 1, 3) == 0 && issued_is(want, 2));
        ring.answer = 1;
        CHECK("and is the caller's",
              usb_core_endpoint_quiesce(the_ctrl, 1, 3) == 1 && ring.retired_all == 1);
    }
    usb_core_reset();
    ring_reset();
    d = stick_at(0, ST_RUNNING, 1);
    ring.ep_state = XHCI_EP_STOPPED;
    {
        int want[1] = {CMD_DEQUEUE};

        CHECK("a stopped endpoint is only moved on",
              usb_core_endpoint_quiesce(the_ctrl, 1, 3) == 0 && issued_is(want, 1));
        ring.answer = 1;
        CHECK("and is the caller's",
              usb_core_endpoint_quiesce(the_ctrl, 1, 3) == 1 && ring.retired_all == 1);
    }

    /* A report transfer that fails: the endpoint is halted or in the Error
       state, and the device goes through the quiesce before it reports
       again, as it does after a stall. */
    {
        int which;

        for (which = 0; which < 2; which++) {
            int want[1];

            usb_core_reset();
            ring_reset();
            port_present = 1;
            d = stick_at(0, ST_RUNNING, 1);
            d->interface.kind = USB_KIND_HID;
            d->reporting = 1;
            d->have_report = 1;
            d->report_done.status = which == 0 ? USB_XFER_FAILED : USB_XFER_STALLED;
            d->report_done.length = 8;
            d->report_done.residual = 8;
            ring.ep_state = which == 0 ? XHCI_EP_ERROR : XHCI_EP_HALTED;
            want[0] = which == 0 ? CMD_DEQUEUE : CMD_RESET;
            usb_core_step(1);
            CHECK("a report transfer that failed halts the device's reporting",
                  d->state == ST_HALTED);
            usb_core_step(1);
            CHECK("and the quiesce runs with the command the endpoint's state asks for",
                  issued_is(want, 1) && ring.submits == 0);
            ring.answer = 1;
            usb_core_step(1);
            if (which != 0) {
                ring.answer = 1;
                usb_core_step(1);
            }
            usb_core_step(1);
            CHECK("after which the device reports again",
                  d->state == ST_RUNNING && ring.retired_all == 1 && ring.submits == 1);
            port_present = 0;
        }
    }

    /* The slot given back: the stick driver hears of it after Disable Slot
       succeeds, and not before, not on a refused disable, and not on a
       tombstone. */
    usb_core_reset();
    ring_reset();
    slots_closed = 0;
    d = stick_at(0, ST_CLOSING, 1);
    {
        int want[1] = {CMD_DISABLE};

        usb_core_step(1);
        CHECK("a closing stick asks for its slot back", issued_is(want, 1) && d->closing_asked);
        CHECK("and its driver has not heard yet", slots_closed == 0 && d->state == ST_CLOSING);
        usb_core_step(1);
        CHECK("nor while the answer is awaited", slots_closed == 0);
        ring.answer = 1;
        ring.status = USB_XFER_OK;
        usb_core_step(1);
        CHECK("the disable's success tells the driver once", slots_closed == 1);
        CHECK("and frees the entry", usb_core_count() == 0);
    }
    usb_core_reset();
    ring_reset();
    slots_closed = 0;
    d = stick_at(0, ST_CLOSING, 1);
    usb_core_step(1);
    ring.answer = 1;
    ring.status = USB_XFER_FAILED;
    usb_core_step(1);
    CHECK("a disable answered with a failure leaves a tombstone",
          d->state == ST_TOMBSTONE && slots_closed == 0);
    usb_core_reset();
    ring_reset();
    slots_closed = 0;
    d = stick_at(0, ST_CLOSING, 1);
    usb_core_step(1);
    usb_core_step(CLOSE_LIMIT_MS);
    CHECK("a disable never answered leaves a tombstone",
          d->state == ST_TOMBSTONE && slots_closed == 0);
    usb_core_reset();
    ring_reset();
    slots_closed = 0;
    d = stick_at(0, ST_CLOSING, 1);
    ring.refuse_issue = 1;
    usb_core_step(1);
    CHECK("a disable the driver will not issue is asked for again", d->state == ST_CLOSING);
    usb_core_step(STEP_MS);
    CHECK("until the deadline, which leaves a tombstone",
          d->state == ST_TOMBSTONE && slots_closed == 0 && ring.count == 0);
    usb_core_reset();
}

int main(void)
{
    struct usb_interface_info info;
    unsigned char broken[sizeof(keyboard_config)];
    unsigned i;

    quiesce_rows();

    /* A configuration as a device sends it. */
    walk(keyboard_config, (int)sizeof(keyboard_config), &info);
    CHECK("a boot keyboard's interface is found", info.found == 1);
    CHECK("with its configuration's value", info.config_value == 1);
    CHECK("its interface number", info.number == 0);
    CHECK("its class, subclass and protocol",
          info.class_code == 3 && info.subclass == 1 && info.protocol == 1);
    CHECK("its endpoint", info.ep_number == 1);
    CHECK("that endpoint's packet size", info.ep_max_packet == 8);
    CHECK("and the interval it asked for", info.ep_interval == 10);

    /* The descriptors between the interface and its endpoint are skipped by
       their own lengths rather than by knowing what they are. */
    for (i = 0; i < sizeof(broken); i++) {
        broken[i] = keyboard_config[i];
    }
    broken[19] = 0x22; /* the HID descriptor, with a type this kernel has no
                          case for at all */
    walk(broken, (int)sizeof(broken), &info);
    CHECK("a descriptor this kernel does not know is stepped over by its length", info.found == 1);

    for (i = 0; i < sizeof(broken); i++) {
        broken[i] = keyboard_config[i];
    }
    broken[14] = 3;
    broken[15] = 1;
    broken[16] = 5; /* HID, boot, but a protocol with no report layout */
    walk(broken, (int)sizeof(broken), &info);
    CHECK("a boot interface of neither kind is not taken", info.found == 0);

    for (i = 0; i < sizeof(broken); i++) {
        broken[i] = keyboard_config[i];
    }
    broken[12] = 1; /* an alternate setting, which this kernel never selects */
    walk(broken, (int)sizeof(broken), &info);
    CHECK("an interface on an alternate setting is not taken", info.found == 0);

    for (i = 0; i < sizeof(broken); i++) {
        broken[i] = keyboard_config[i];
    }
    broken[5] = 0; /* a configuration numbered zero, which selects none */
    walk(broken, (int)sizeof(broken), &info);
    CHECK("a configuration numbered zero is not taken", info.found == 0);

    for (i = 0; i < sizeof(broken); i++) {
        broken[i] = keyboard_config[i];
    }
    broken[29] = 0x80; /* endpoint zero, which is the control endpoint */
    walk(broken, (int)sizeof(broken), &info);
    CHECK("the control endpoint is not taken as a reporting one", info.found == 0);

    /* A device that is not one this kernel drives. */
    for (i = 0; i < sizeof(broken); i++) {
        broken[i] = keyboard_config[i];
    }
    broken[14] = 8; /* a mass-storage interface */
    walk(broken, (int)sizeof(broken), &info);
    CHECK("an interface of another class is not taken", info.found == 0);

    for (i = 0; i < sizeof(broken); i++) {
        broken[i] = keyboard_config[i];
    }
    broken[15] = 0; /* HID, but with no boot protocol */
    walk(broken, (int)sizeof(broken), &info);
    CHECK("an interface with no boot protocol is not taken", info.found == 0);

    for (i = 0; i < sizeof(broken); i++) {
        broken[i] = keyboard_config[i];
    }
    broken[29] = 0x01; /* the endpoint is out, not in */
    walk(broken, (int)sizeof(broken), &info);
    CHECK("an endpoint that reports nothing is not taken", info.found == 0);

    for (i = 0; i < sizeof(broken); i++) {
        broken[i] = keyboard_config[i];
    }
    broken[30] = 2; /* bulk, not interrupt */
    walk(broken, (int)sizeof(broken), &info);
    CHECK("an endpoint of the wrong kind is not taken", info.found == 0);

    /* Descriptors no device would send. Each of these reads past the buffer,
       or never ends, in a walker that trusts what it is given. */
    for (i = 0; i < sizeof(broken); i++) {
        broken[i] = keyboard_config[i];
    }
    broken[9] = 0; /* an interface descriptor of length zero */
    walk(broken, (int)sizeof(broken), &info);
    CHECK("a descriptor of length zero ends the walk", info.found == 0);

    for (i = 0; i < sizeof(broken); i++) {
        broken[i] = keyboard_config[i];
    }
    broken[9] = 200; /* an interface descriptor longer than what arrived */
    walk(broken, (int)sizeof(broken), &info);
    CHECK("a descriptor longer than the buffer ends the walk", info.found == 0);

    for (i = 0; i < sizeof(broken); i++) {
        broken[i] = keyboard_config[i];
    }
    broken[0] = 1; /* a descriptor shorter than its own header */
    walk(broken, (int)sizeof(broken), &info);
    CHECK("a descriptor shorter than its header ends the walk", info.found == 0);

    walk(keyboard_config, 20, &info);
    CHECK("a configuration cut short is walked only as far as it goes", info.found == 0);

    /* The last descriptor cut in half: its length says seven bytes and three
       arrived. A walk that reads it reads past the end of what the device
       sent, which is what the sanitizer is here to see. */
    walk(keyboard_config, (int)sizeof(keyboard_config) - 4, &info);
    CHECK("a descriptor cut off at the end is not read", info.found == 0);

    walk(keyboard_config, 4, &info);
    CHECK("one shorter than a configuration descriptor is refused", info.found == 0);

    walk(keyboard_config, 0, &info);
    CHECK("and an empty one is refused", info.found == 0);

    usb_parse_config(0, 9, &info);
    CHECK("as is nothing at all", info.found == 0);

    /* The device table. */
    usb_core_reset();
    CHECK("nothing is configured to begin with", usb_core_count() == 0);
    CHECK("and nothing is part way through", usb_core_idle() == 1);

    if (fails == 0) {
        printf("usb_hosttest: OK\n");
        return 0;
    }
    printf("usb_hosttest: %d failures\n", fails);
    return 1;
}
