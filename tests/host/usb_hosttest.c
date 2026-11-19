/*
 * usb_hosttest.c: the USB core, usb/core.c, with no controller: its walk of a
 * configuration's descriptors, the quiesce of an endpoint, the slot given
 * back, a keyboard's LED report and the control endpoint's recovery after
 * one fails, and a device of several HID interfaces, up to fifteen,
 * enumerated from its configuration to its running or its unplugging.
 *
 * What a device says about itself arrives as a string of descriptors, each
 * giving its own length, and the walk has nothing but those lengths to go
 * by. QEMU's emulated keyboard and mouse send correct ones, as does every
 * working device; a malformed descriptor is what a broken device, or a
 * hostile one, sends.
 *
 * So the walk is compiled natively and given descriptors no device would send,
 * in buffers allocated to the exact length, under AddressSanitizer. A read
 * past the end is then a failure rather than a byte of whatever follows.
 */

extern int printf(const char *fmt, ...);
extern void *malloc(unsigned long size);
extern void free(void *p);

#include "../../rach2/sys/usb/core.h"
#include "../../rach2/sys/usb/hid.h"
#include "../../rach2/sys/usb/hub.h"

/* ---- the functions outside core.c that it calls ---- */

#include "../../rach2/sys/usb/xhci.h"

/** Discard the message: no row checks the log. */
void klog(int level, const char *subsystem, const char *format, ...)
{
    (void)level;
    (void)subsystem;
    (void)format;
}

#include "../../rach2/sys/lib/kdef.h"

/** Fill `n` bytes at `ptr` with `c`, as the kernel's `k_memset`, which this
 * test does not build. */
void *k_memset(void *ptr, int c, size_t n)
{
    unsigned char *p = (unsigned char *)ptr;

    while (n-- > 0) {
        *p++ = (unsigned char)c;
    }
    return ptr;
}

/* The frames the core takes its device records from. On the host they are
 * pages of this program's memory, and none is given to a controller. */
#define FRAME_COUNT 64
static uint8_t frames[FRAME_COUNT][4096] __attribute__((aligned(4096)));
static uint8_t frame_taken[FRAME_COUNT];

/** Take a free frame, or return 0 when all are taken. */
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

/** Give back a frame `alloc_kernel_frame` took. */
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

/* The one controller. The stubs stand for the driver, so its record is a
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

/** Return 0, so the port scan looks at no root port. */
int xhci_ports(struct xhci_ctrl *hc)
{
    return 0;
}

/* Whether a high-speed device is connected and enabled on every root port,
 * for the rows that step a running device. The port scan asks about no port,
 * since `xhci_ports` returns 0. */
static int port_present;

/** Report a connected, enabled high-speed device when `port_present` is
 * set; return -1 otherwise. */
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

/** Return `port_present`, as `xhci_port_status` reports the port. */
int xhci_port_connected(struct xhci_ctrl *hc, int port)
{
    (void)hc;
    (void)port;
    return port_present;
}

/** Return 0: no root port has settled. */
int xhci_port_settled(struct xhci_ctrl *hc, int port)
{
    (void)port;
    return 0;
}

/** Return -1: no root port is reset. */
int xhci_port_reset_begin(struct xhci_ctrl *hc, int port)
{
    (void)port;
    return -1;
}

/** Return -1, as `xhci_port_reset_begin` does. */
int xhci_port_reset_done(struct xhci_ctrl *hc, int port)
{
    (void)port;
    return -1;
}

/** Return 0: the slot is marked a hub. */
int xhci_device_hub(struct xhci_ctrl *hc, int device, int ports, int multi_tt, int think_time)
{
    (void)device;
    (void)ports;
    (void)multi_tt;
    (void)think_time;
    return 0;
}

/** Return -1: the rows open no slot through the driver. */
int xhci_device_open(struct xhci_ctrl *hc, const struct usb_location *where, int speed)
{
    (void)where;
    (void)speed;
    return -1;
}

/** Return -1: no device is addressed. */
int xhci_device_address(struct xhci_ctrl *hc, int device)
{
    (void)device;
    return -1;
}

/** Return -1: no packet size is changed. */
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
#define CMD_ENABLE  5
#define CMD_OPEN    6
static struct {
    int pending; /* a command issued and not yet answered */
    int answer;  /* 1 to answer it with `status`, 0 to leave it */
    int status;  /* the answer's status */

    /* The slot the answer gives back, the slot of the last Disable Slot, and
     * the slot `xhci_device_closed` was told of; 0 for none. */
    int result_device;
    int disabled_device;
    int closed_device;
    int refuse_issue; /* every issue answers -1 */

    /* The first 8 commands issued, in order, and the endpoint each named;
     * `count` is every command issued, which may be more. */
    int issued[8];
    int endpoint[8];
    int count;
    int ep_state; /* the endpoint's state, as the driver would report it */

    /* The calls that set and that cleared a quiescing flag. */
    int quiescing_on, quiescing_off;
    int quiescing_endpoint; /* the endpoint the last flag call named */
    int quiescing[32];      /* the flag per endpoint */

    /* The calls of `xhci_endpoint_retire_all`. */
    int retired_all;
    int submits; /* transfers the core tried to submit */

    /* The transfers submitted on the control endpoint, and the last of
     * them: its setup packet, its data stage's length and direction, and
     * the first byte of its data. */
    int control_submits;
    uint8_t setup[8];
    uint32_t length;
    int direction;
    int data;
    /* The last transfer on another endpoint: its length and buffer; and
     * the endpoints, by DCI, a transfer was submitted on, a bit each. */
    uint32_t in_length;
    const uint8_t *in_buffer;
    uint32_t in_endpoints;

    /* Whether `xhci_endpoint_open` issues its command, and the endpoints
     * it opened, by DCI, a bit each. */
    int open_ok;
    uint32_t opened;

    /* The first 32 control requests, each its bRequest, its descriptor
     * type for GET_DESCRIPTOR, and the low byte of its wIndex, packed as
     * `request(...)` packs them, and the last, packed the same way;
     * `requests` counts them all. */
    int request[32];
    int last_request;
    int requests;
} ring;

/** Pack a control request as `ring.request` keeps it. */
static int request(int brequest, int desc_type, int windex)
{
    return brequest << 16 | desc_type << 8 | windex;
}

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

/** Empty the record, with no command pending and the endpoint running. */
static void ring_reset(void)
{
    int i;

    for (i = 0; i < 8; i++) {
        ring.issued[i] = 0;
    }
    ring.pending = 0;
    ring.answer = 0;
    ring.status = USB_XFER_OK;
    ring.result_device = 0;
    ring.disabled_device = 0;
    ring.closed_device = 0;
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
    ring.control_submits = 0;
    for (i = 0; i < 8; i++) {
        ring.setup[i] = 0;
    }
    ring.length = 0;
    ring.direction = -1;
    ring.data = -1;
    ring.in_endpoints = 0;
    ring.open_ok = 0;
    ring.opened = 0;
    ring.requests = 0;
}

/** Issue Disable Slot for `device`, recorded as `CMD_DISABLE`. */
int xhci_device_close(struct xhci_ctrl *hc, int device)
{
    ring.disabled_device = device;
    return ring_issue(CMD_DISABLE, 0);
}

/** Record that the core gave back the driver's record of `device`. */
void xhci_device_closed(struct xhci_ctrl *hc, int device)
{
    (void)hc;
    ring.closed_device = device;
}

/** Return 0: the ring is never busy with a command of the driver's own. */
int xhci_command_busy(struct xhci_ctrl *hc)
{
    return 0;
}

/** Issue Configure Endpoint for `endpoint`, recorded as `CMD_OPEN` with its
 * DCI, when `ring.open_ok` is set; return -1 otherwise. */
int xhci_endpoint_open(struct xhci_ctrl *hc, int device, const struct usb_endpoint *endpoint)
{
    int dci = endpoint->number * 2 + (endpoint->direction == USB_DIR_IN ? 1 : 0);

    (void)device;
    if (!ring.open_ok || ring_issue(CMD_OPEN, dci) != 0) {
        return -1;
    }
    ring.opened |= 1u << dci;
    return 0;
}

/**
 * Answer the pending command once a row has set `ring.answer`: return 1 for
 * success, which leaves the endpoint stopped after a stop, reset or dequeue,
 * -1 for a failure, and 0 while there is no answer.
 */
int xhci_command_done(struct xhci_ctrl *hc, struct usb_command_result *out)
{
    if (!ring.pending || !ring.answer) {
        return 0;
    }
    ring.pending = 0;
    ring.answer = 0;
    out->status = ring.status;
    out->code = 0;
    out->device = ring.result_device;
    if (ring.status == USB_XFER_OK && ring.count > 0 && ring.count <= 8) {
        int what = ring.issued[ring.count - 1];

        if (what == CMD_STOP || what == CMD_RESET || what == CMD_DEQUEUE) {
            ring.ep_state = XHCI_EP_STOPPED;
        }
    }
    return ring.status == USB_XFER_OK ? 1 : -1;
}

/** Count a transfer, and record one on the control endpoint, or return -1
 * on an endpoint marked quiescing, as the driver does. */
int xhci_submit(struct xhci_ctrl *hc, int device, int endpoint, const struct usb_transfer *transfer)
{
    int i;

    (void)device;
    if (ring.quiescing[endpoint & 31]) {
        return -1; /* what the driver does while the core quiesces the endpoint */
    }
    ring.submits++;
    if (transfer->setup == 0) {
        ring.in_length = transfer->length;
        ring.in_buffer = transfer->buffer;
        ring.in_endpoints |= 1u << (endpoint & 31);
    }
    if (endpoint == 1 && transfer->setup != 0) {
        ring.last_request =
            request(transfer->setup[1], transfer->setup[1] == 6 ? transfer->setup[3] : 0,
                    transfer->setup[4]);
        if (ring.requests < 32) {
            ring.request[ring.requests] = ring.last_request;
        }
        ring.requests++;
        ring.control_submits++;
        for (i = 0; i < 8; i++) {
            ring.setup[i] = transfer->setup[i];
        }
        ring.length = transfer->length;
        ring.direction = transfer->direction;
        ring.data = transfer->length != 0 ? transfer->buffer[0] : -1;
    }
    return 0;
}

/** Accept the stick: the mass-storage driver is not under test here. */
int msc_attach(struct xhci_ctrl *hc, int device, int interface, int ep_in, int ep_out)
{
    (void)device;
    (void)interface;
    (void)ep_in;
    (void)ep_out;
    return 0;
}

/** Do nothing, as `msc_attach` does. */
void msc_detach(struct xhci_ctrl *hc, int device)
{
    (void)hc;
    (void)device;
}

/** Return 1 when no command is pending. */
int xhci_command_free(struct xhci_ctrl *hc)
{
    return !ring.pending;
}

/** Return 0, so a failure records no controller state. */
int xhci_interrupts_on(struct xhci_ctrl *hc)
{
    return 0; /* the model's controller sends no interrupts of its own */
}

/** Do nothing: there are no registers to record. */
void xhci_state_record(struct xhci_ctrl *hc)
{}

/** Return -1: the hub driver is not under test here. */
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

/** Do nothing, as `hub_attach` does. */
void hub_detach(int index)
{
    (void)index;
}

/** Do nothing: there is no hub. */
void hub_step(uint32_t elapsed_ms)
{
    (void)elapsed_ms;
}

/** Return 0: no completion is a hub's. */
int hub_completion(const struct usb_completion *completion)
{
    (void)completion;
    return 0;
}

/** Return 0: there is no hub to be starting. */
int hub_starting(void)
{
    return 0;
}

/** Report nothing connected and return -1: there is no hub port. */
int hub_port_status(struct xhci_ctrl *hc, int owner, int port, struct usb_port_status *out)
{
    (void)owner;
    (void)port;
    out->connected = 0;
    return -1;
}

/** Return 0, as `hub_port_status` finds no port. */
int hub_port_settled(struct xhci_ctrl *hc, int owner, int port)
{
    (void)owner;
    (void)port;
    return 0;
}

/** Return -1: there is no hub port to reset. */
int hub_port_reset_begin(struct xhci_ctrl *hc, int owner, int port)
{
    (void)owner;
    (void)port;
    return -1;
}

/** Return -1, as `hub_port_reset_begin` does. */
int hub_port_reset_done(struct xhci_ctrl *hc, int owner, int port)
{
    (void)owner;
    (void)port;
    return -1;
}

/** Do nothing: there is no disk. */
void usbdisk_step(void)
{}

/** Do nothing: the mass-storage driver is not under test here. */
void msc_step(uint32_t elapsed_ms)
{
    (void)elapsed_ms;
}

/** Return 0: no completion is a stick's. */
int msc_completion(const struct usb_completion *completion)
{
    (void)completion;
    return 0;
}

/** Return 0: there is no stick to be starting. */
int msc_starting(void)
{
    return 0;
}

/** Return 0: no transfer completes. */
int xhci_poll(struct xhci_ctrl *hc, struct usb_completion *out)
{
    (void)out;
    return 0;
}

/** Issue Reset Endpoint, recorded as `CMD_RESET`. */
int xhci_endpoint_reset(struct xhci_ctrl *hc, int device, int endpoint)
{
    (void)device;
    return ring_issue(CMD_RESET, endpoint);
}

/** Issue Stop Endpoint, recorded as `CMD_STOP`. */
int xhci_endpoint_stop(struct xhci_ctrl *hc, int device, int endpoint)
{
    (void)device;
    return ring_issue(CMD_STOP, endpoint);
}
/** Issue Set TR Dequeue Pointer, recorded as `CMD_DEQUEUE`. */
int xhci_endpoint_dequeue_here(struct xhci_ctrl *hc, int device, int endpoint)
{
    (void)device;
    return ring_issue(CMD_DEQUEUE, endpoint);
}
/** Return the endpoint's state as the row set it in `ring.ep_state`. */
int xhci_endpoint_state(struct xhci_ctrl *hc, int device, int endpoint)
{
    (void)device;
    (void)endpoint;
    return ring.ep_state;
}
/** Set or clear the quiescing flag of `endpoint`, and count the calls. */
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
/** Count a retirement of every transfer queued on the endpoint. */
void xhci_endpoint_retire_all(struct xhci_ctrl *hc, int device, int endpoint)
{
    (void)device;
    (void)endpoint;
    ring.retired_all++;
}
/* The calls of `msc_slot_closed`, which tells the stick driver that a slot
 * was given back. */
static int slots_closed;
/** Count the call. */
void msc_slot_closed(struct xhci_ctrl *hc, int device)
{
    (void)device;
    slots_closed++;
}

/* The interfaces the HID driver took, by their place on the device, a bit
 * each, in the report protocol and in the boot protocol, the
 * bInterfaceNumber each was given, and the kind the last boot one was. */
static uint32_t attached, attached_boot;
static int attached_number[USB_HID_INTERFACES];
static int attached_kind;

/** Take any interface: the HID driver has its own test, hid_hosttest.c. */
int hid_attach(int device, int iface, const struct hid_ids *ids, const uint8_t *rdesc, int length)
{
    (void)device;
    (void)rdesc;
    (void)length;
    attached |= 1u << iface;
    attached_number[iface] = ids->interface;
    return 0;
}

/* Optional multitouch setup row: GET followed by SET, and the last reply. */
static int feature_requests = -1;
static int feature_bytes, feature_first, feature_registered;

/** Offer two feature requests, when the multitouch row enables them. */
int hid_setup_request(int device, int iface, uint8_t *data, int size, int *value, int *write)
{
    (void)device;
    (void)iface;
    if (feature_requests < 0 || feature_requests > 1 || size < 4) {
        return 0;
    }
    *value = 0x305;
    *write = feature_requests++ != 0;
    if (*write) {
        k_memset(data, 0, 4);
        data[0] = 5;
    }
    return 4;
}

/** Record the feature reply, as it arrived on the control endpoint. */
void hid_setup_answer(int device, int iface, const uint8_t *data, int length)
{
    (void)device;
    (void)iface;
    feature_bytes = length;
    feature_first = length > 0 ? data[0] : -1;
}

/** Count the publication after feature setup. */
int hid_setup_finish(int device, int iface)
{
    (void)device;
    (void)iface;
    feature_registered++;
    return 0;
}

/** Take any interface in the boot protocol. */
int hid_attach_boot(int device, int iface, int kind, const struct hid_ids *ids)
{
    (void)device;
    attached_kind = kind;
    attached_boot |= 1u << iface;
    attached_number[iface] = ids->interface;
    return 0;
}

/* The longest report the rows have the HID driver give, 0 for the
 * endpoint's packet, and the last report the core handed on, with the
 * interface it was handed on for. */
static int report_size_model;
static const uint8_t *report_seen;
static int report_seen_len = -1;
static int report_seen_iface = -1;

/** Return `report_size_model`. */
int hid_report_size(int device, int iface)
{
    (void)device;
    (void)iface;
    return report_size_model;
}

/** Keep the report handed on. */
void hid_report(int device, int iface, const uint8_t *report, int length)
{
    (void)device;
    report_seen = report;
    report_seen_len = length;
    report_seen_iface = iface;
}

/** Zeroed memory from the host's heap, as the kernel's kzalloc gives. */
void *kzalloc(size_t size)
{
    unsigned char *p = (unsigned char *)malloc((unsigned long)(size ? size : 1));
    size_t i;

    for (i = 0; p && i < size; i++) {
        p[i] = 0;
    }
    return p;
}

/** Give back what `kzalloc` gave. */
void kfree(void *p)
{
    free(p);
}

/* The calls of `hid_gone`, and the device the last one named. */
static int gone_count, gone_device;

/** Count the call. */
void hid_gone(int device)
{
    gone_device = device;
    gone_count++;
}

/* The LED byte the rows have the HID driver offer, -1 for none, and the
 * interface it is offered for, -1 for every one; the times it was asked;
 * the byte it offered last; and the last report the core said it sent: its
 * device, interface and outcome, and how many it said it sent. */
static int led_wanted = -1;
static int led_iface = -1;
static int led_asked;
static int led_offered = -1;
static int led_sent_device, led_sent_iface, led_sent_report, led_sent_ok, led_sent_count;
static int led_length = 1; /* the report's bytes, each `led_wanted` */

/** The output report's bytes, `led_length`. */
int hid_output_size(int device, int iface)
{
    (void)device;
    (void)iface;
    return led_length;
}

/** Offer `led_wanted` as an LED report of `led_length` bytes, wValue
 * 0x0200, to interface `led_iface`, or to any when it is -1. */
int hid_output_report(int device, int iface, uint8_t *out, int size, int *value)
{
    int i;

    (void)device;
    led_asked++;
    if (led_wanted < 0 || size < led_length || (led_iface >= 0 && iface != led_iface)) {
        return -1;
    }
    for (i = 0; i < led_length; i++) {
        out[i] = (uint8_t)led_wanted;
    }
    *value = 0x0200;
    led_offered = led_wanted;
    return led_length;
}

/** Record what the core sent. */
void hid_output_sent(int device, int iface, int ok)
{
    led_sent_device = device;
    led_sent_iface = iface;
    led_sent_report = led_offered;
    led_sent_ok = ok;
    led_sent_count++;
}

/** Do nothing: no row checks the announcement of a mouse. */
void input_mouse_found(void)
{}

/** Return 0: the core masks interrupts, and the host has none. */
uint32_t kirq_save(void)
{
    return 0;
}

/** Do nothing, as `kirq_save` does. */
void kirq_restore(uint32_t flags)
{
    (void)flags;
}

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
    9, 33, 0x11, 1, 0, 1, 34, 63,   0,  /* HID 1.11, a report descriptor of 63 */
    7, 5,  0x81, 3, 8, 0, 10            /* endpoint 1, in, interrupt, 8 bytes */
};

/** Walk a copy of `length` bytes of `bytes` in a buffer of exactly that
 * size, so AddressSanitizer catches a read past the end. */
static void walk(const unsigned char *bytes, int length, struct usb_interface_info *out)
{
    unsigned char *copy = (unsigned char *)malloc((unsigned long)(length > 0 ? length : 1));
    int i;

    for (i = 0; i < length; i++) {
        copy[i] = bytes[i];
    }
    usb_parse_config(copy, length, out, 1);
    free(copy);
}

/* ---- the quiesce, and the slot given back ---- */

/** Return 1 when the ring's record is exactly the `n` commands of `want` and
 * every endpoint command named endpoint 3. */
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

/**
 * Put a stick in entry `i` without enumerating it: in `state`, on `slot`, on
 * root port `i`, with endpoint DCIs 3 (IN) and 4 (OUT) and a deadline one
 * step away. Return the entry.
 */
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

/**
 * Put a running boot keyboard in entry `i`, on `slot`, with its interface
 * number 2 and its reports on DCI 3; with `protocol` USB_PROTOCOL_MOUSE, a
 * mouse. Return the entry.
 */
static struct usb_device_state *hid_at(int i, int slot, int protocol)
{
    struct usb_device_state *d = stick_at(i, ST_RUNNING, slot);

    d->interface.kind = USB_KIND_HID;
    d->interface.protocol = protocol;
    d->interface.number = 2;
    d->interface.ep_max_packet = 8;
    d->ep_out_dci = 0;
    d->hid = (struct usb_hid_ifs *)alloc_kernel_frame();
    k_memset(d->hid, 0, sizeof(*d->hid));
    d->hid->count = 1;
    d->hid->info[0] = d->interface;
    d->hid->at[0].ep_dci = 3;
    d->hid->at[0].live = 1;
    return d;
}

/**
 * Check `usb_core_endpoint_quiesce` on endpoint 3 of a stick in every state
 * of the endpoint and with every failure of a command, then the Disable Slot
 * of a closing stick. The quiesce takes the command turn that enumeration
 * takes, `enumerating[0]`, and holds it from the first command to the answer
 * of the last. A failed report transfer leaves the endpoint halted or in the
 * Error state, and the device is quiesced before it reports again, as after
 * a stall. The stick driver hears that its slot was given back only after a
 * Disable Slot succeeds: not before, not on a refused disable, and not on a
 * tombstone.
 */
static void quiesce_rows(void)
{
    struct usb_device_state *d;

    /* A running endpoint: Stop Endpoint, then Set TR Dequeue Pointer. */
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
            transfer.buffer = d->buffer;
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

    /* A halted endpoint is reset first; a stopped one is only moved on. */
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

    /* A report transfer that fails, with a USB error and with a stall. */
    {
        int which;

        for (which = 0; which < 2; which++) {
            int want[1];

            usb_core_reset();
            ring_reset();
            port_present = 1;
            d = hid_at(0, 1, USB_PROTOCOL_KBD);
            d->hid->at[0].reporting = 1;
            d->hid->at[0].have_report = 1;
            d->hid->at[0].report_done.status = which == 0 ? USB_XFER_FAILED : USB_XFER_STALLED;
            d->hid->at[0].report_done.length = 8;
            d->hid->at[0].report_done.residual = 8;
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

    /* The slot given back, by a Disable Slot that succeeds or fails. */
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

/**
 * Check the cleanup of an Enable Slot that outlives the enumeration deadline,
 * once answered with a failure and once with a slot, which must be closed.
 */
static void slot_timeout_rows(void)
{
    struct usb_device_state *d;
    int refused;

    for (refused = 0; refused < 2; refused++) {
        usb_core_reset();
        ring_reset();
        port_present = 1;
        d = stick_at(0, ST_SLOT, 0);
        d->interface.kind = USB_KIND_NONE;
        enumerating[0] = 0;
        CHECK("enable slot is outstanding", ring_issue(CMD_ENABLE, 0) == 0);
        usb_core_step(STEP_MS - 1u);
        CHECK("enable slot waits before the deadline", d->state == ST_SLOT);
        usb_core_step(1);
        CHECK("slot timeout retains the device and command turn",
              devices[0] == d && d->state == ST_CLOSING && d->abandoned && enumerating[0] == 0 &&
                  ring.count == 1);
        usb_core_step(100);
        CHECK("slot cleanup waits for the outstanding enable",
              devices[0] == d && d->state == ST_CLOSING && ring.pending && ring.count == 1);
        ring.answer = 1;
        ring.result_device = refused ? 0 : 7;
        ring.status = refused ? USB_XFER_FAILED : USB_XFER_OK;
        usb_core_step(100);
        if (refused) {
            CHECK("failed enable releases the entry without disable",
                  d->state == ST_FREE && enumerating[0] == -1 && ring.count == 1 &&
                      ring.disabled_device == 0);
        } else {
            CHECK("late enable closes the returned slot",
                  d->state == ST_CLOSING && d->slot == 7 && d->closing_asked &&
                      enumerating[0] == 0 && ring.count == 2 && ring.issued[1] == CMD_DISABLE &&
                      ring.disabled_device == 7);
            CHECK("slot record remains until disable completes", ring.closed_device == 0);
            ring.answer = 1;
            usb_core_step(1);
            CHECK("disable completion releases the late slot",
                  ring.closed_device == 7 && d->state == ST_FREE && enumerating[0] == -1);
        }
        usb_core_step(1);
        CHECK("resolved slot cleanup frees the device frame", devices[0] == 0);
    }
    port_present = 0;
    usb_core_reset();
}

/**
 * Check that the endpoints the walk selects all belong to one complete
 * interface: a storage interface split across two, on an alternate setting,
 * or cut short is not taken, and a complete interface after an incomplete
 * one of another class is.
 */
static void interface_rows(void)
{
    unsigned char config[] = {9,    2, 48, 0, 2,    1, 0,   0x80, 50, 9, 4, 0,    0, 1, 8, 6,
                              0x50, 0, 7,  5, 0x81, 2, 64,  0,    0,  9, 4, 1,    0, 2, 8, 6,
                              0x50, 0, 7,  5, 0x02, 2, 128, 0,    0,  7, 5, 0x83, 2, 0, 2, 0};
    unsigned char cross_class[] = {9, 2,    41, 0, 2, 1, 0, 0x80, 50,   9, 4, 0,    0, 1,
                                   3, 1,    1,  0, 9, 4, 1, 0,    2,    8, 6, 0x50, 0, 7,
                                   5, 0x82, 2,  0, 2, 0, 7, 5,    0x03, 2, 0, 2,    0};
    struct usb_interface_info info;

    walk(config, 41, &info);
    CHECK("split storage interfaces are incomplete", !info.found);
    walk(config, sizeof(config), &info);
    CHECK("complete storage interface replaces incomplete candidate",
          info.found && info.kind == USB_KIND_STORAGE && info.number == 1 && info.ep_number == 3 &&
              info.ep_max_packet == 512 && info.ep_out_number == 2 &&
              info.ep_out_max_packet == 128);
    config[28] = 1;
    walk(config, sizeof(config), &info);
    CHECK("alternate setting cannot complete a storage candidate", !info.found);
    config[28] = 0;
    config[27] = 0;
    walk(config, sizeof(config), &info);
    CHECK("repeated interface descriptor starts a fresh candidate",
          info.found && info.number == 0 && info.ep_number == 3);
    config[27] = 1;
    config[31] = 0xFF;
    walk(config, sizeof(config), &info);
    CHECK("unsupported interface cannot complete a storage candidate", !info.found);
    config[25] = 2;
    config[27] = 7;
    config[28] = 5;
    config[29] = 0x02;
    config[30] = 2;
    walk(config, 34, &info);
    CHECK("short interface descriptor ends endpoint collection", !info.found);

    walk(cross_class, sizeof(cross_class), &info);
    CHECK("complete storage follows incomplete HID", info.found && info.kind == USB_KIND_STORAGE &&
                                                         info.number == 1 && info.ep_number == 2 &&
                                                         info.ep_out_number == 3);
    cross_class[14] = 8;
    cross_class[15] = 6;
    cross_class[16] = 0x50;
    cross_class[23] = 3;
    cross_class[24] = 1;
    cross_class[25] = 2;
    cross_class[30] = 3;
    cross_class[33] = 10;
    walk(cross_class, sizeof(cross_class), &info);
    CHECK("complete HID follows incomplete storage",
          info.found && info.kind == USB_KIND_HID && info.number == 1 &&
              info.protocol == USB_PROTOCOL_MOUSE && info.ep_number == 2 &&
              info.ep_interval == 10 && info.ep_out_number == 0);
    cross_class[23] = 9;
    cross_class[24] = 0;
    cross_class[25] = 0;
    walk(cross_class, sizeof(cross_class), &info);
    CHECK("complete hub follows incomplete storage",
          info.found && info.kind == USB_KIND_HUB && info.number == 1 && info.ep_number == 2);
    cross_class[14] = 3;
    cross_class[15] = 1;
    cross_class[16] = 1;
    cross_class[18] = 7;
    cross_class[19] = 5;
    cross_class[20] = 0x84;
    cross_class[21] = 3;
    cross_class[22] = 8;
    cross_class[23] = 0;
    cross_class[24] = 4;
    walk(cross_class, sizeof(cross_class), &info);
    CHECK("first complete interface remains selected",
          info.found && info.kind == USB_KIND_HID && info.number == 0 && info.ep_number == 4);
}

/* ---- a keyboard's LEDs ---- */

/** Complete the control transfer outstanding on `d` with `status`. */
static void control_done(struct usb_device_state *d, int status)
{
    d->control.status = status;
    d->control.endpoint = 1;
    d->have_control = 1;
}

/** Fetch and set a HID feature before the interface is made live. */
static void feature_setup_rows(void)
{
    struct usb_device_state *d;

    usb_core_reset();
    ring_reset();
    d = hid_at(0, 1, USB_PROTOCOL_MOUSE);
    d->hid->at[0].features = 1;
    d->hid->at[0].live = 0;
    d->hid_if = 0;
    led_length = sizeof(d->buffer) + 1;
    feature_requests = feature_registered = 0;
    hid_advance(d, HS_REPORT);
    CHECK("a numbered GET_REPORT goes to the interface before registration",
          ring.control_submits == 1 && ring.setup[0] == TYPE_GET_REPORT &&
              ring.setup[1] == REQ_GET_REPORT && ring.setup[2] == 5 && ring.setup[3] == 3 &&
              ring.setup[4] == 2 && ring.length == 4 && ring.direction == USB_DIR_IN &&
              !feature_registered && !d->hid->at[0].live);
    if (d->long_output) {
        d->long_output[0] = 5;
    }
    d->short_by = 1;
    hid_answer(d, 1);
    CHECK("the driver reads the actual bytes in the heap buffer before SET_REPORT",
          feature_bytes == 3 && feature_first == 5 && ring.control_submits == 2 &&
              ring.setup[0] == TYPE_TO_INTERFACE && ring.setup[1] == REQ_SET_REPORT &&
              ring.direction == USB_DIR_OUT && ring.data == 5 && !feature_registered);
    d->short_by = 0;
    hid_answer(d, 1);
    CHECK("the interface is published after its last feature",
          feature_registered == 1 && d->hid->at[0].live && d->state == ST_RUNNING);
    feature_requests = -1;
    led_length = 1;
    usb_core_reset();
}

/** Check the LED report's stall on `d`, whose report 0x05 is out: the
 * halted control endpoint is reset and moved on before the next goes out. */
static void leds_stall_rows(struct usb_device_state *d)
{
    control_done(d, USB_XFER_STALLED);
    ring.ep_state = XHCI_EP_HALTED;
    led_wanted = 0x01;
    step_running(d);
    CHECK("leds: a stall is a failure",
          led_sent_count == 2 && led_sent_report == 0x05 && !led_sent_ok);
    CHECK("leds: and the halted control endpoint is reset before another report goes out",
          ring.count == 1 && ring.issued[0] == CMD_RESET && ring.endpoint[0] == 1 &&
              ring.quiescing[1] && d->output_quiesce && ring.control_submits == 2);
    step_running(d);
    CHECK("leds: none goes out while the reset is awaited", ring.control_submits == 2);
    ring.answer = 1;
    step_running(d);
    CHECK("leds: the reset's success moves the endpoint on",
          ring.count == 2 && ring.issued[1] == CMD_DEQUEUE && ring.control_submits == 2);
    ring.answer = 1;
    step_running(d);
    CHECK("leds: sent again once the endpoint is usable",
          ring.retired_all == 1 && !d->output_quiesce && ring.control_submits == 3 &&
              ring.data == 0x01);
}

/** Check the LED report on `d` that is never answered: its transfer is
 * stopped and moved on before the next goes out, and a completion that
 * comes meanwhile is told to nobody. */
static void leds_timeout_rows(struct usb_device_state *d)
{
    core_ms += STEP_MS - 1;
    step_running(d);
    CHECK("leds: still out within STEP_MS", led_sent_count == 2 && ring.control_submits == 3);
    core_ms += 1;
    ring.ep_state = XHCI_EP_RUNNING;
    led_wanted = 0x04;
    step_running(d);
    CHECK("leds: no completion in STEP_MS is a failure", led_sent_count == 3 && !led_sent_ok);
    CHECK("leds: and the transfer is no longer out", d->output_out == 0 && d->output_quiesce);
    step_running(d);
    CHECK("leds: its transfer is stopped before another report reuses its buffer",
          ring.count == 3 && ring.issued[2] == CMD_STOP && ring.endpoint[2] == 1 &&
              ring.control_submits == 3);
    control_done(d, USB_XFER_OK);
    step_running(d);
    CHECK("leds: a completion that comes meanwhile is told to nobody",
          led_sent_count == 3 && ring.control_submits == 3);
    ring.answer = 1;
    step_running(d);
    ring.answer = 1;
    step_running(d);
    CHECK("leds: the next report goes out once the endpoint is stopped and moved on",
          ring.count == 4 && ring.issued[3] == CMD_DEQUEUE && ring.retired_all == 2 &&
              ring.control_submits == 4 && ring.data == 0x04);
    control_done(d, USB_XFER_OK);
    led_wanted = -1;
    step_running(d);
    CHECK("leds: and its completion is told", led_sent_count == 4 && led_sent_ok);
}

/** Check that `d`, whose LED report 0x06 is out, fails when its control
 * endpoint will not reset after that report stalls. */
static void leds_unrecovered_rows(struct usb_device_state *d)
{
    int submits;

    control_done(d, USB_XFER_STALLED);
    ring.ep_state = XHCI_EP_HALTED;
    led_wanted = -1;
    step_running(d);
    ring.answer = 1;
    ring.status = USB_XFER_FAILED;
    submits = ring.submits;
    step_running(d);
    CHECK("leds: a control endpoint that will not reset fails the device",
          d->state == ST_CLOSING && !d->output_quiesce && ring.submits == submits);
    ring.status = USB_XFER_OK;
}

/**
 * Check a running keyboard's LED report: SET_REPORT of the Output report to
 * its interface, a one-byte OUT data stage with the byte the HID driver
 * offers, one at a time, its outcome told to the HID driver on its
 * completion, as a failure on a stall and after STEP_MS with none, offered
 * again while the control endpoint refuses transfers, and asked of a
 * mouse's driver too. After a stall the halted control endpoint is reset,
 * and after a report never answered its transfer is stopped, each moved on
 * past what was queued, before another report goes out; a completion that
 * comes meanwhile is told to nobody, and an endpoint that will not recover
 * fails the device.
 */
static void leds_rows(void)
{
    static const uint8_t set_report[8] = {0x21, 0x09, 0x00, 0x02, 0x02, 0x00, 0x01, 0x00};
    struct usb_device_state *d;
    int i;
    int same = 1;

    usb_core_reset();
    ring_reset();
    d = hid_at(0, 1, USB_PROTOCOL_KBD);
    led_wanted = 0x02;
    led_sent_count = 0;
    step_running(d);
    for (i = 0; i < 8; i++) {
        same = same && ring.setup[i] == set_report[i];
    }
    CHECK("leds: one control transfer", ring.control_submits == 1);
    CHECK("leds: SET_REPORT Output to the interface", same);
    CHECK("leds: one byte out, the report",
          ring.length == 1 && ring.direction == USB_DIR_OUT && ring.data == 0x02);
    step_running(d);
    CHECK("leds: none while one is out", ring.control_submits == 1 && led_sent_count == 0);
    control_done(d, USB_XFER_OK);
    led_wanted = -1;
    step_running(d);
    CHECK("leds: the completion is told",
          led_sent_count == 1 && led_sent_device == 1 && led_sent_report == 0x02 && led_sent_ok);
    CHECK("leds: none when the driver asks for none", ring.control_submits == 1);

    led_wanted = 0x05;
    step_running(d);
    CHECK("leds: a change is sent", ring.control_submits == 2 && ring.data == 0x05);
    leds_stall_rows(d);
    leds_timeout_rows(d);

    led_wanted = 0x06;
    ring.quiescing[1] = 1;
    step_running(d);
    CHECK("leds: a refused transfer is not out", d->output_out == 0 && led_sent_count == 4);
    ring.quiescing[1] = 0;
    step_running(d);
    CHECK("leds: and is tried at the next step", ring.control_submits == 5 && ring.data == 0x06);

    leds_unrecovered_rows(d);

    usb_core_reset();
    ring_reset();
    d = hid_at(0, 1, USB_PROTOCOL_MOUSE);
    led_wanted = -1;
    led_asked = 0;
    step_running(d);
    CHECK("leds: a mouse's driver is asked too, and offers none",
          led_asked == 1 && ring.control_submits == 0 && ring.submits == 1);
    usb_core_reset();
}

/**
 * Check a device whose reports pass 64 bytes: its input reports are asked
 * for and handed on whole, in a buffer of its longest report, as usbhid
 * sizes its buffer, and an output report of 100 bytes goes out whole. A
 * long output report never answered keeps its buffer until its transfer is
 * stopped, and only then is the buffer given back for a longer one.
 */
static void long_report_rows(void)
{
    struct usb_device_state *d;
    uint8_t *held;

    usb_core_reset();
    ring_reset();
    d = hid_at(0, 1, USB_PROTOCOL_MOUSE);
    report_size_model = 200;
    led_wanted = -1;
    step_running(d);
    CHECK("long: a report of 200 bytes is asked for whole",
          ring.in_length == 200 && d->hid->at[0].long_report &&
              ring.in_buffer == d->hid->at[0].long_report);
    d->hid->at[0].report_done.status = USB_XFER_OK;
    d->hid->at[0].report_done.length = 200;
    d->hid->at[0].report_done.residual = 0;
    d->hid->at[0].have_report = 1;
    step_running(d);
    CHECK("long: and handed on whole",
          report_seen_len == 200 && report_seen == d->hid->at[0].long_report);
    led_wanted = 0x07;
    led_length = 100;
    step_running(d);
    CHECK("long: an output report of 100 bytes goes out whole",
          ring.control_submits == 1 && ring.length == 100 && ring.data == 0x07);
    control_done(d, USB_XFER_OK);
    led_wanted = 0x09;
    led_length = 5000;
    step_running(d);
    CHECK("long: and one of 5000 bytes, from a buffer of its length",
          ring.control_submits == 2 && ring.length == 5000 && ring.data == 0x09 && d->long_output &&
              d->long_output_cap == 5000);
    held = d->long_output;
    core_ms += STEP_MS;
    ring.ep_state = XHCI_EP_RUNNING;
    led_wanted = 0x0A;
    led_length = 6000;
    step_running(d);
    step_running(d);
    CHECK("long: one never answered keeps its buffer while its transfer is stopped",
          d->long_output == held && d->long_output_cap == 5000 && ring.control_submits == 2 &&
              ring.pending && ring.issued[0] == CMD_STOP);
    ring.answer = 1;
    step_running(d);
    ring.answer = 1;
    step_running(d);
    CHECK("long: and a longer one goes out only after",
          ring.control_submits == 3 && ring.length == 6000 && ring.data == 0x0A &&
              d->long_output_cap == 6000);
    led_wanted = -1;
    led_length = 1;
    report_size_model = 0;
    usb_core_reset();
}

/**
 * Check what the walk reads of a HID interface: its HID descriptor's bcdHID
 * and report descriptor length, and an interface of any subclass and
 * protocol taken, since its report descriptor says what it reports.
 */
static void hid_interface_rows(void)
{
    struct usb_interface_info info;
    unsigned char broken[sizeof(keyboard_config)];
    unsigned i;

    walk(keyboard_config, (int)sizeof(keyboard_config), &info);
    CHECK("a HID interface's bcdHID and report descriptor length",
          info.hid_version == 0x0111 && info.report_length == 63);

    for (i = 0; i < sizeof(broken); i++) {
        broken[i] = keyboard_config[i];
    }
    broken[16] = 5; /* HID, boot, but a protocol with no boot layout */
    walk(broken, (int)sizeof(broken), &info);
    CHECK("a HID interface of another protocol is taken, for its report descriptor",
          info.found == 1 && info.kind == USB_KIND_HID && info.protocol == 5);

    broken[15] = 0; /* and with no boot protocol */
    walk(broken, (int)sizeof(broken), &info);
    CHECK("an interface with no boot protocol is taken, for its report descriptor",
          info.found == 1 && info.kind == USB_KIND_HID && info.subclass == 0);
}

/* ---- a device of several HID interfaces ---- */

/*
 * A keyboard whose media keys are a second interface, as many are: a boot
 * keyboard on interface 0 and, on interface 1, a HID interface with no boot
 * protocol whose report descriptor gives its consumer keys.
 */
static const unsigned char two_config[] = {
    9, 2,  59,   0, 2, 1, 0,  0xA0, 50, /* configuration, value 1 */
    9, 4,  0,    0, 1, 3, 1,  1,    0,  /* interface 0: HID, boot, keyboard */
    9, 33, 0x11, 1, 0, 1, 34, 63,   0,  /* HID 1.11, a report descriptor of 63 */
    7, 5,  0x81, 3, 8, 0, 10,           /* endpoint 1, in, interrupt, 8 bytes */
    9, 4,  1,    0, 1, 3, 0,  0,    0,  /* interface 1: HID, no boot protocol */
    9, 33, 0x11, 1, 0, 1, 34, 25,   0,  /* a report descriptor of 25 */
    7, 5,  0x82, 3, 4, 0, 10            /* endpoint 2, in, interrupt, 4 bytes */
};

/*
 * Three HID interfaces and a vendor's interface among them: the keyboard of
 * `two_config`, an interface 1 whose report descriptor the rows refuse, a
 * vendor's interface 2, an alternate setting of interface 1, and a HID
 * interface 3 with its HID descriptor after its endpoint.
 */
static const unsigned char three_config[] = {
    9, 2,  116,  0, 4, 1,    0,  0xA0, 50, /* configuration, value 1 */
    9, 4,  0,    0, 1, 3,    1,  1,    0,  /* interface 0: boot keyboard */
    9, 33, 0x11, 1, 0, 1,    34, 63,   0,  /* its HID descriptor */
    7, 5,  0x81, 3, 8, 0,    10,           /* endpoint 1 */
    9, 4,  1,    0, 1, 3,    0,  0,    0,  /* interface 1: HID */
    9, 33, 0x11, 1, 0, 1,    34, 25,   0,  /* its HID descriptor */
    7, 5,  0x82, 3, 4, 0,    10,           /* endpoint 2 */
    9, 4,  2,    0, 1, 0xFF, 0,  0,    0,  /* interface 2: a vendor's */
    7, 5,  0x84, 3, 8, 0,    10,           /* endpoint 4 */
    9, 4,  1,    1, 1, 3,    0,  0,    0,  /* interface 1, setting 1 */
    7, 5,  0x85, 3, 8, 0,    10,           /* endpoint 5 */
    9, 4,  3,    0, 1, 3,    0,  0,    0,  /* interface 3: HID */
    7, 5,  0x83, 3, 8, 0,    10,           /* endpoint 3 */
    9, 33, 0x11, 1, 0, 1,    34, 50,   0   /* its HID descriptor, after */
};

/** Walk a copy of `length` bytes of `bytes`, as `walk` does, keeping up to
 * `max` interfaces in `out`; return how many were kept. */
static int walk_all(const unsigned char *bytes, int length, struct usb_interface_info *out, int max)
{
    unsigned char *copy = (unsigned char *)malloc((unsigned long)(length > 0 ? length : 1));
    int i, kept;

    for (i = 0; i < length; i++) {
        copy[i] = bytes[i];
    }
    kept = usb_parse_config(copy, length, out, max);
    free(copy);
    return kept;
}

/* The place of the interface whose setup `enumerate` stops at, once that
 * setup has begun; -1 to go on to the end. */
static int enumerate_stop_if = -1;

/**
 * Enumerate the configuration `config` of `length` bytes on entry 0, from
 * its whole configuration read to its running or its failure, in at most
 * 200 steps: each command is answered with success, and each control
 * request too, but a GET_DESCRIPTOR of the report descriptor of interface
 * `refuse`, which stalls. Return the entry.
 */
static struct usb_device_state *enumerate(const unsigned char *config, int length, int refuse)
{
    struct usb_device_state *d;
    int n, i, seen = 0;

    usb_core_reset();
    ring_reset();
    ring.open_ok = 1;
    attached = 0;
    attached_boot = 0;
    d = dev_take(0);
    d->state = ST_CONFIGALL;
    d->slot = 1;
    d->hc = the_ctrl;
    d->owner = -1;
    d->port = 0;
    d->ops = &root_ops;
    d->config_length = (uint32_t)length;
    for (i = 0; i < length; i++) {
        d->buffer[i] = config[i];
    }
    d->deadline = core_ms + STEP_MS;
    enumerating[0] = 0;
    d->control.residual = 0;
    control_done(d, USB_XFER_OK);
    for (n = 0; n < 200 && d->state != ST_RUNNING && d->state != ST_CLOSING &&
                d->state != ST_FREE && !(d->state == ST_HIDSETUP && d->hid_if == enumerate_stop_if);
         n++) {
        if (ring.pending) {
            ring.answer = 1;
        }
        if (ring.control_submits > seen) {
            seen = ring.control_submits;
            control_done(d, ring.last_request == request(6, 0x22, refuse) ? USB_XFER_STALLED
                                                                          : USB_XFER_OK);
        }
        step_device(d);
    }
    return d;
}

/** Whether the first 32 control requests of the ring include `want`. */
static int requested(int want)
{
    int i;

    for (i = 0; i < ring.requests && i < 32; i++) {
        if (ring.request[i] == want) {
            return 1;
        }
    }
    return 0;
}

/** Check the walk of a configuration of several HID interfaces: it keeps
 * each HID interface on alternate setting 0, up to the room it is given, and
 * passes over the rest. */
static void multi_walk_rows(void)
{
    struct usb_interface_info info[USB_HID_INTERFACES];

    CHECK("multi: two HID interfaces are kept",
          walk_all(two_config, (int)sizeof(two_config), info, USB_HID_INTERFACES) == 2 &&
              info[0].number == 0 && info[0].ep_number == 1 && info[0].report_length == 63 &&
              info[1].found && info[1].number == 1 && info[1].ep_number == 2 &&
              info[1].ep_max_packet == 4 && info[1].report_length == 25 &&
              info[1].config_value == 1);
    CHECK("multi: three of them, past a vendor's interface and an alternate setting",
          walk_all(three_config, (int)sizeof(three_config), info, USB_HID_INTERFACES) == 3 &&
              info[1].number == 1 && info[1].ep_number == 2 && info[2].number == 3 &&
              info[2].ep_number == 3 && info[2].report_length == 50);
    CHECK("multi: no more than the room given",
          walk_all(three_config, (int)sizeof(three_config), info, 2) == 2 && info[1].number == 1);
    CHECK("multi: the first alone with room for one",
          walk_all(two_config, (int)sizeof(two_config), info, 1) == 1 && info[0].number == 0);
}

/**
 * Check on `d`, the running keyboard of `two_config` whose second interface
 * was sent a report last, that one interface's stalled output report has
 * the control endpoint every interface shares reset before the other's
 * goes out, and that a report endpoint halting meanwhile waits for that
 * reset; `completion` is a report completion on the second interface's
 * endpoint.
 */
static void multi_shared_rows(struct usb_device_state *d, struct usb_completion *completion)
{
    led_wanted = 0x02;
    step_running(d);
    CHECK("multi: another output report to the second interface",
          d->output_out && d->output_if == 1);
    control_done(d, USB_XFER_STALLED);
    ring.ep_state = XHCI_EP_HALTED;
    led_iface = 0;
    step_running(d);
    CHECK("multi: its stall resets the control endpoint every interface shares",
          d->output_quiesce && !d->output_out && ring.pending);
    ring.answer = 1;
    step_running(d);
    ring.answer = 1;
    step_running(d);
    CHECK("multi: after which the first interface's report goes out",
          !d->output_quiesce && d->output_out && d->output_if == 0 && ring.setup[4] == 0);

    control_done(d, USB_XFER_STALLED);
    ring.ep_state = XHCI_EP_HALTED;
    led_wanted = -1;
    step_running(d);
    completion->status = USB_XFER_FAILED;
    report_route(d->hid, completion);
    step_running(d);
    CHECK("multi: a report endpoint that halts meanwhile waits for the control endpoint",
          d->state == ST_HALTED && d->hid_if == 1 && d->output_quiesce && d->quiesce_dci == 1);
    ring.answer = 1;
    step_device(d);
    ring.answer = 1;
    step_device(d);
    CHECK("multi: whose recovery ends first, and then the report endpoint's begins",
          !d->output_quiesce && d->quiesce_dci == 5 && d->quiesce_stage != 0);
    ring.answer = 1;
    step_device(d);
    CHECK("multi: after which the device runs again", d->state == ST_RUNNING);
}

/**
 * Check a keyboard of two HID interfaces, as Linux's usbhid binds one to
 * each: the enumeration opens each one's endpoint and runs SET_IDLE and the
 * report descriptor on each one's number; each has its own report transfer,
 * its completions are handed on for it, and an output report goes to the
 * interface whose driver offered it.
 */
static void multi_two_rows(void)
{
    struct usb_completion completion;
    struct usb_sys_row row;
    struct usb_device_state *d;

    d = enumerate(two_config, (int)sizeof(two_config), -1);
    CHECK("multi: a keyboard of two HID interfaces runs",
          d->state == ST_RUNNING && d->hid && d->hid->count == 2 && d->hid->at[0].live &&
              d->hid->at[1].live);
    CHECK("multi: each interface's endpoint is opened",
          ring.opened == ((1u << 3) | (1u << 5)) && d->hid->at[1].ep_dci == 5);
    CHECK("multi: SET_IDLE and the report descriptor go to each interface",
          requested(request(10, 0, 0)) && requested(request(6, 0x22, 0)) &&
              requested(request(10, 0, 1)) && requested(request(6, 0x22, 1)));
    CHECK("multi: the driver takes each, by its place and its number",
          attached == 3 && attached_number[0] == 0 && attached_number[1] == 1);
    led_wanted = -1;
    step_running(d);
    CHECK("multi: each interface has its report transfer",
          ring.in_endpoints == ((1u << 3) | (1u << 5)));
    completion.hc = the_ctrl;
    completion.device = 1;
    completion.endpoint = 5;
    completion.status = USB_XFER_OK;
    completion.length = 4;
    completion.residual = 1;
    report_route(d->hid, &completion);
    report_seen_iface = -1;
    step_running(d);
    CHECK("multi: a report on the second endpoint goes to the second interface",
          report_seen_iface == 1 && report_seen_len == 3 && d->hid->at[1].reports == 1 &&
              d->hid->at[0].reports == 0);
    CHECK("multi: and the interface gets its next transfer", d->hid->at[1].reporting == 1);
    CHECK("multi: /sys names each interface",
          usb_core_interface(0, 1, &row) == 0 && row.ifnum == 1 && row.ifsubclass == 0 &&
              usb_core_row(0, &row) == 0 && row.ifnum == 0 && row.ifprotocol == 1 &&
              usb_core_interface(0, 2, &row) == -1);
    led_wanted = 0x02;
    led_iface = 1;
    step_running(d);
    CHECK("multi: an output report goes to the interface that offered it",
          ring.setup[1] == 9 && ring.setup[4] == 1 && d->output_out && d->output_if == 1);
    control_done(d, USB_XFER_OK);
    led_wanted = -1;
    step_running(d);
    CHECK("multi: and its completion is told to that interface",
          led_sent_iface == 1 && led_sent_ok && !d->output_out);

    multi_shared_rows(d, &completion);
    led_iface = -1;
    usb_core_reset();
}

/** Check a device of three HID interfaces whose second refuses its report
 * descriptor, which is given up alone, and a device of one that refuses it,
 * which fails. */
static void multi_three_rows(void)
{
    struct usb_sys_row row;
    struct usb_device_state *d;
    unsigned char lone[sizeof(keyboard_config)];
    unsigned i;

    d = enumerate(three_config, (int)sizeof(three_config), 1);
    CHECK("multi: a device whose second interface refuses its descriptor runs",
          d->state == ST_RUNNING && d->hid && d->hid->count == 3);
    CHECK("multi: with that interface given up alone", d->hid->at[0].live && !d->hid->at[1].live &&
                                                           d->hid->at[1].given_up &&
                                                           d->hid->at[2].live && attached == 5);
    CHECK("multi: the third interface was set up after the refusal",
          requested(request(10, 0, 3)) && requested(request(6, 0x22, 3)) &&
              ring.opened == ((1u << 3) | (1u << 5) | (1u << 7)));
    step_running(d);
    CHECK("multi: no report is asked of the interface given up",
          ring.in_endpoints == ((1u << 3) | (1u << 7)));
    CHECK("multi: and /sys has no directory for it", usb_core_interface(0, 1, &row) == -1 &&
                                                         usb_core_interface(0, 3, &row) == 0 &&
                                                         row.ifclass == 3);

    for (i = 0; i < sizeof(lone); i++) {
        lone[i] = keyboard_config[i];
    }
    lone[15] = 0; /* no boot protocol */
    d = enumerate(lone, (int)sizeof(lone), 0);
    CHECK("multi: a device whose one interface refuses its descriptor fails",
          d->state == ST_CLOSING && attached == 0);
    usb_core_reset();
}

/**
 * Check a device of two HID interfaces unplugged after its first interface
 * was taken and while its second is being set up: the HID driver hears that
 * it went, so the first interface's event devices and state go with it; and
 * a device whose first interface is given up, whose /sys row names the
 * second.
 */
static void multi_gone_rows(void)
{
    struct usb_sys_row row;
    struct usb_device_state *d;
    unsigned char first_off[sizeof(two_config)];
    unsigned i;

    enumerate_stop_if = 1;
    d = enumerate(two_config, (int)sizeof(two_config), -1);
    enumerate_stop_if = -1;
    CHECK("multi: the first interface is taken before the second's setup ends",
          d->state == ST_HIDSETUP && d->hid_if == 1 && attached == 1);
    gone_count = 0;
    gone_device = 0;
    device_gone(0);
    CHECK("multi: a device gone during its second interface's setup is told to the HID driver",
          gone_count == 1 && gone_device == 1 && d->state == ST_CLOSING);
    usb_core_reset();

    for (i = 0; i < sizeof(first_off); i++) {
        first_off[i] = two_config[i];
    }
    first_off[15] = 0; /* interface 0 with no boot protocol */
    first_off[16] = 0;
    d = enumerate(first_off, (int)sizeof(first_off), 0);
    CHECK("multi: a device whose first interface is given up runs on its second",
          d->state == ST_RUNNING && !d->hid->at[0].live && d->hid->at[1].live);
    CHECK("multi: and /sys names the second as its first",
          usb_core_row(0, &row) == 0 && row.ifnum == 1);
    usb_core_reset();
}

/**
 * Check a device whose second interface, a boot mouse, refuses its report
 * descriptor: that interface alone falls back to the boot protocol, with
 * SET_PROTOCOL to its own number, and the first runs in the report protocol.
 */
static void multi_boot_rows(void)
{
    unsigned char mouse_second[sizeof(two_config)];
    struct usb_device_state *d;
    unsigned i;

    for (i = 0; i < sizeof(mouse_second); i++) {
        mouse_second[i] = two_config[i];
    }
    mouse_second[40] = 1; /* interface 1: boot subclass, mouse protocol */
    mouse_second[41] = 2;
    d = enumerate(mouse_second, (int)sizeof(mouse_second), 1);
    CHECK("multi: a second interface that refuses its descriptor runs in the boot protocol",
          d->state == ST_RUNNING && d->hid->at[0].live && d->hid->at[1].live &&
              d->hid->at[1].boot && !d->hid->at[0].boot);
    CHECK("multi: with SET_PROTOCOL to that interface alone",
          requested(request(11, 0, 1)) && !requested(request(11, 0, 0)));
    CHECK("multi: and the driver takes it as a boot mouse, the first in the report protocol",
          attached == 1 && attached_boot == 2 && attached_kind == HID_MOUSE &&
              attached_number[1] == 1);
    usb_core_reset();
}

/**
 * Write in `out` a configuration of sixteen HID interfaces, numbered 0 to
 * 15, each with a report descriptor of 25 bytes and an interrupt IN
 * endpoint: 1 to 15 for the first fifteen, and 1 again for the last, which
 * no configuration may have. Return its length.
 */
static int full_config(unsigned char *out)
{
    static const unsigned char iface[9] = {9, 4, 0, 0, 1, 3, 0, 0, 0};
    static const unsigned char hid[9] = {9, 33, 0x11, 1, 0, 1, 34, 25, 0};
    static const unsigned char ep[7] = {7, 5, 0x81, 3, 8, 0, 10};
    int at = 9, k, i;

    for (k = 0; k < 16; k++) {
        for (i = 0; i < 9; i++) {
            out[at + i] = iface[i];
            out[at + 9 + i] = hid[i];
        }
        out[at + 2] = (unsigned char)k;
        at += 18;
        for (i = 0; i < 7; i++) {
            out[at + i] = ep[i];
        }
        out[at + 2] = (unsigned char)(0x81 + (k < 15 ? k : 0));
        at += 7;
    }
    out[0] = 9;
    out[1] = 2;
    out[2] = (unsigned char)(at & 0xFF);
    out[3] = (unsigned char)(at >> 8);
    out[4] = 16;
    out[5] = 1;
    out[6] = 0;
    out[7] = 0xA0;
    out[8] = 50;
    return at;
}

/**
 * Check a device of every HID interface a configuration can have: fifteen,
 * one on each IN endpoint, each kept, opened, set up and driven, and a
 * sixteenth on an endpoint another has passed over, as Linux drops it.
 */
static void multi_full_rows(void)
{
    unsigned char config[9 + 16 * 25];
    struct usb_interface_info info[USB_HID_INTERFACES + 1];
    struct usb_device_state *d;
    uint32_t want = 0;
    int length = full_config(config);
    int k;

    CHECK("multi: fifteen HID interfaces are kept, and one on a repeated endpoint is not",
          walk_all(config, length, info, USB_HID_INTERFACES + 1) == 15 && info[14].number == 14 &&
              info[14].ep_number == 15);
    d = enumerate(config, length, -1);
    for (k = 1; k <= 15; k++) {
        want |= 1u << (2 * k + 1);
    }
    CHECK("multi: a device of fifteen runs with each one driven",
          d->state == ST_RUNNING && d->hid->count == 15 && attached == 0x7FFFu &&
              ring.opened == want && requested(request(6, 0x22, 14)));
    usb_core_reset();
}

/**
 * Run the rows and print the result; return 1 when a row failed.
 *
 * The walk is given a boot keyboard's configuration, then copies with a
 * few bytes changed: interfaces and endpoints of kinds the core does not drive,
 * and lengths no device would send, which a walk that trusted them would
 * read past or never finish. The last descriptor cut short is read past
 * only by a walk that trusts its length, which the sanitizer reports.
 */
int main(void)
{
    struct usb_interface_info info;
    unsigned char broken[sizeof(keyboard_config)];
    unsigned i;

    quiesce_rows();
    slot_timeout_rows();
    leds_rows();
    long_report_rows();
    interface_rows();
    hid_interface_rows();
    multi_walk_rows();
    multi_two_rows();
    multi_three_rows();
    multi_gone_rows();
    multi_boot_rows();
    multi_full_rows();
    feature_setup_rows();

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

    /* A descriptor after the interface is skipped by its length alone. */
    for (i = 0; i < sizeof(broken); i++) {
        broken[i] = keyboard_config[i];
    }
    broken[19] = 0x22; /* the HID descriptor, given a type with no case */
    walk(broken, (int)sizeof(broken), &info);
    CHECK("a descriptor this kernel does not know is stepped over by its length",
          info.found == 1 && info.report_length == 0);

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
    broken[29] = 0x01; /* the endpoint is out, not in */
    walk(broken, (int)sizeof(broken), &info);
    CHECK("an endpoint that reports nothing is not taken", info.found == 0);

    for (i = 0; i < sizeof(broken); i++) {
        broken[i] = keyboard_config[i];
    }
    broken[30] = 2; /* bulk, not interrupt */
    walk(broken, (int)sizeof(broken), &info);
    CHECK("an endpoint of the wrong kind is not taken", info.found == 0);

    /* Descriptors no device would send. */
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

    /* The last descriptor cut short: it says seven bytes and three arrived. */
    walk(keyboard_config, (int)sizeof(keyboard_config) - 4, &info);
    CHECK("a descriptor cut off at the end is not read", info.found == 0);

    walk(keyboard_config, 4, &info);
    CHECK("one shorter than a configuration descriptor is refused", info.found == 0);

    walk(keyboard_config, 0, &info);
    CHECK("and an empty one is refused", info.found == 0);

    usb_parse_config(0, 9, &info, 1);
    CHECK("as is nothing at all", info.found == 0);

    /* The device table. */
    usb_core_reset();
    CHECK("nothing is configured to begin with", usb_core_count() == 0);
    CHECK("and nothing is part way through", usb_core_busy() == 0);

    if (fails == 0) {
        printf("usb_hosttest: OK\n");
        return 0;
    }
    printf("usb_hosttest: %d failures\n", fails);
    return 1;
}
