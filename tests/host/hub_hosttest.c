/*
 * hub_hosttest.c: the hub driver and the core's tree against a model of a
 * hub with devices behind it.
 *
 * host-deps:
 *
 * The core and the hub driver are included whole, and the controller under
 * them is a model: a slot is given out for a location, and what answers on
 * that slot is decided by the location, so a hub on root port 0 answers as
 * a hub and the keyboard on its port 2 answers as a keyboard. The hub's
 * class requests are answered from the model's port table, which a row sets
 * up: which ports have a device and at what speed, how the hub switches
 * power, its think time, how many ports it claims, and whether a reset on a
 * port ever ends.
 *
 * The rows are the contract: the descriptor read and the port count passed
 * to the controller, the think time given to the hub's slot and a child
 * carrying the hub as its translator, a full-speed hub giving none; ganged,
 * per-port and unswitched power; the power-good wait before the first port
 * is looked at; the scan finding a device already present and the core
 * enumerating it at its location; a device that lands on a port the scan
 * has just read, whose connection change the scan's acknowledgement would
 * clear unread, with the hub closed when the read that follows the
 * acknowledgement comes back short; a reset that completes and one that
 * never does, with the port refused and forgotten when its device leaves;
 * the speed read from the status word; a disconnection closing the device
 * below, and its class driver told when its report endpoint had halted; a hub
 * pulled with two devices below closing both children before itself; a
 * power-on after an over-current asked for again until it is queued;
 * `/proc/usb` reading the table with interrupts off; a hub with twenty ports
 * keeping all twenty, a device on port twenty named through its translator;
 * and a hub that stalls its descriptor request closed.
 *
 * No hosted headers: the kernel headers define the fixed-width types, and
 * the libc calls are declared by hand as the other host tests declare them.
 */

#include "../../rach2/sys/lib/kstring.h"
#include "../../rach2/sys/usb/core.h"
#include "../../rach2/sys/usb/hcd.h"
#include "../../rach2/sys/usb/hid.h"
#include "../../rach2/sys/usb/hub.h"
#include "../../rach2/sys/usb/msc.h"
#include "../../rach2/sys/usb/xhci.h"

extern int printf(const char *fmt, ...);

/* ---- the kernel functions the two files call ---- */

static int full_records; /* records of a device refused for want of an entry */

/** Return 0: the boot line has no value for any key. */
int kcmdline_value(const char *key, char *out, unsigned int outlen)
{
    (void)key;
    (void)out;
    (void)outlen;
    return 0;
}

/** Count the messages that refuse a device because the table is full. */
void klog(int level, const char *subsystem, const char *format, ...)
{
    const char *full = "device: port %s.%d: refused: the device table is full";
    int i;

    (void)level;
    (void)subsystem;
    for (i = 0; format[i] != 0 && format[i] == full[i]; i++) {
    }
    if (format[i] == 0 && full[i] == 0) {
        full_records++;
    }
}

/** Fill `n` bytes at `ptr` with `c`, as the kernel's `k_memset`. */
void *k_memset(void *ptr, int c, size_t n)
{
    unsigned char *p = (unsigned char *)ptr;
    size_t i;

    for (i = 0; i < n; i++) {
        p[i] = (unsigned char)c;
    }
    return ptr;
}

/* The slots `hid_gone` was called with, the first 16, and the calls. */
static int hid_gone_slot[16];
static int hid_gone_count;

/** Do nothing: no row checks a report. */
void hid_report(int slot, int kind, const uint8_t *report, int length)
{
    (void)slot;
    (void)kind;
    (void)report;
    (void)length;
}

/** Record the slot of a keyboard that went. */
void hid_gone(int slot)
{
    if (hid_gone_count < 16) {
        hid_gone_slot[hid_gone_count] = slot;
    }
    hid_gone_count++;
}

/** Return -1: there is no stick in this model. */
int msc_attach(struct xhci_ctrl *hc, int device, int interface, int ep_in, int ep_out)
{
    (void)device;
    (void)interface;
    (void)ep_in;
    (void)ep_out;
    return -1;
}

/** Do nothing, as `msc_attach` takes no stick. */
void msc_detach(struct xhci_ctrl *hc, int device)
{
    (void)hc;
    (void)device;
}

/** Do nothing, as `msc_attach` takes no stick. */
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

/** Do nothing: there is no disk. */
void usbdisk_step(void)
{}

/** Do nothing: there is no mouse. */
void input_mouse_found(void)
{}

/* ---- the model ---- */

/* What answers at a location: nothing, the hub, or a boot keyboard. */
#define KIND_NONE 0
#define KIND_HUB  1
#define KIND_KBD  2

/* The ports the model's hub can have; port n is entry n - 1 of each array. */
#define MODEL_PORTS 31

static struct {
    int root_connected; /* the hub on root port 0 */
    int root_speed;
    int hub_protocol; /* the hub's interface protocol byte */
    int hub_ports;    /* what its descriptor claims */

    /* The descriptor's power switching mode, 0 ganged, 1 per port and 2 none,
     * and its TT think time field. */
    int hub_lpsm;
    int hub_think;
    int hub_pwrgood;    /* in 2 ms units */
    int hub_desc_stall; /* the hub descriptor is refused */

    /* For each port: what is on it and at what speed, whether its reset never
     * ends, whether it is in reset and the status reads since the reset began,
     * whether it is enabled, its change word, whether it has an over-current,
     * and whether its power is off. */
    int port_kind[MODEL_PORTS];
    int port_speed[MODEL_PORTS];
    int port_reset_never[MODEL_PORTS];
    int port_in_reset[MODEL_PORTS];
    int port_reset_reads[MODEL_PORTS];
    int port_enabled[MODEL_PORTS];
    unsigned int port_change[MODEL_PORTS]; /* the change word */
    int port_over_current[MODEL_PORTS];
    int port_powered_off[MODEL_PORTS];
    /* The hub's own status and change words. */
    unsigned int hub_status;
    unsigned int hub_change;
    int alt_refuse;        /* SET_INTERFACE is stalled */
    int power_on_refuse;   /* SET_FEATURE(PORT_POWER) on a port that is off is stalled */
    int power_on_unqueued; /* that many of those requests are not queued */
    int pull_on_open;      /* the next device opened is pulled while its Enable Slot is out */
    int connect_on_read;   /* a device lands on this port just after its status is read */
    int short_after_clear; /* the status read after a clear on this port is short */
    int short_clear_seen;  /* that clear has arrived; the next read is the short one */

    /* Disable Slot fails. */
    int close_refuse;
    int hold_commands; /* no command answers while set */
    int slot_cap;      /* the slots the controller has, 0 for fifteen */
    int long_config;   /* a device whose configuration is 115 bytes */
    int config_claims; /* what its header claims, 0 for the truth */
    int change_stall;  /* the change endpoint answers with a stall */
    /* The transfer outstanding on the change endpoint. */
    int change_device;
    unsigned char *change_buffer;
    uint32_t change_length;
    uint32_t change_generation;
    unsigned int change_bits; /* what the next bitmap says */
    /* What was seen. */
    int change_submits;
    int cleared_feature[32];
    int cleared_port[32];
    int clears;
    int hub_status_reads;
    int hub_clears;
    int power_offs;
    int power_ons;
    int alt_set;
    int power_requests;
    int status_reads;
    uint32_t first_status_ms;
    uint32_t last_power_ms;
    int reset_requests;
    int reset_clears;
    int hub_told_ports;
    int hub_told_multi;
    int hub_told_think;
    int hub_told_slot;
    int hub_told;
    int config_delay_ms; /* SET_CONFIGURATION is answered this late */
    int config_due_ms;   /* when the delayed answer is owed, or 0 */
    int config_device;
    uint32_t config_generation;
    int hub_depth_refuse;       /* SET_HUB_DEPTH is stalled */
    int port_link[MODEL_PORTS]; /* USB 3: the link state, 0 for U0, 6 for SS.Inactive */
    int warm_resets;            /* SET_PORT_FEATURE(BH_PORT_RESET) requests */
    int double_issues;          /* commands issued while one was pending */
    int hub_depth_sets;         /* SET_HUB_DEPTH requests, a SuperSpeed hub's */
    int hub_depth;              /* the depth the last one carried */
    int hub_depth_before_tell;  /* the depth arrived before the controller was told */

    /* The model's clock, in milliseconds, advanced by `pump`. */
    uint32_t now_ms;
} model;

/* The slots, from 1, the highest given being `slots_given`: what is on each,
 * where it was opened, and whether it is open. */
static struct {
    int kind;
    int speed;        /* what the model has there */
    int opened_speed; /* what the core opened it with, from the status word */
    struct usb_location where;
    int open;
} slot_of[USB_MAX_DEVICES + 2];
static int slots_given;

/* The slots closed by Disable Slot, in order, the first 16, and the count. */
static int close_order[USB_MAX_DEVICES + 2];
static int closes;
static int closed_done; /* slots the core took back through `xhci_device_closed` */

/* The command in flight and the completions waiting, a ring of 32 that
 * `done_head` and `done_tail` count into. The one controller is a token that
 * is never dereferenced. */
static int command_pending;
static struct usb_command_result command_result_pending;
static struct usb_completion done[32];
static struct xhci_ctrl *the_ctrl = (struct xhci_ctrl *)1;
static int done_head, done_tail;

static void model_change(int port);

/** Queue a completion of a transfer on `device` and `endpoint` for
 * `xhci_poll`. */
static void answer(int device, int endpoint, int status, uint32_t length, uint32_t residual,
                   uint32_t generation)
{
    struct usb_completion *c = &done[done_tail % 32];

    c->hc = the_ctrl;
    c->device = device;
    c->endpoint = endpoint;
    c->status = status;
    c->length = length;
    c->residual = residual;
    c->generation = generation;
    done_tail++;
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

/** Take the oldest waiting completion into `out` and return 1, or return 0. */
int xhci_poll(struct xhci_ctrl *hc, struct usb_completion *out)
{
    if (done_head == done_tail) {
        return 0;
    }
    *out = done[done_head % 32];
    done_head++;
    return 1;
}

/** Return 4 root ports. */
int xhci_ports(struct xhci_ctrl *hc)
{
    return 4;
}

/** Report the hub on root port 0, when connected, at the root's speed. */
int xhci_port_status(struct xhci_ctrl *hc, int port, struct usb_port_status *out)
{
    out->connected = port == 0 && model.root_connected;
    out->enabled = out->connected;
    out->speed = model.root_speed;
    return 0;
}

/** Return 1: a root port is settled at once. */
int xhci_port_settled(struct xhci_ctrl *hc, int port)
{
    (void)port;
    return 1;
}

/** Return 0: a root port's reset starts. */
int xhci_port_reset_begin(struct xhci_ctrl *hc, int port)
{
    (void)port;
    return 0;
}

/** Return 1: a root port's reset ends at once. */
int xhci_port_reset_done(struct xhci_ctrl *hc, int port)
{
    (void)port;
    return 1;
}

/** Return 0: the driver issues no command of its own. */
int xhci_command_busy(struct xhci_ctrl *hc)
{
    return 0;
}

/** Issue a command that the controller will answer with `USB_NO_SLOTS`, and
 * count it in `double_issues` when one was already pending. */
static void command_no_slots(void)
{
    if (command_pending) {
        model.double_issues++;
    }
    command_pending = 1;
    command_result_pending.device = 0;
    command_result_pending.status = USB_XFER_FAILED;
    command_result_pending.code = USB_NO_SLOTS;
}

/** Issue a command that will succeed for `device`, counted as
 * `command_no_slots` counts. */
static void command_ok(int device)
{
    if (command_pending) {
        model.double_issues++; /* a command issued on a ring holding one */
    }
    command_pending = 1;
    command_result_pending.device = device;
    command_result_pending.status = USB_XFER_OK;
    command_result_pending.code = 0;
}

/**
 * Give the pending command's answer in `out` and return 1 for success or -1
 * for a failure, as the driver does; return 0 while no command is pending or
 * the row holds the answers.
 */
int xhci_command_done(struct xhci_ctrl *hc, struct usb_command_result *out)
{
    if (!command_pending || model.hold_commands) {
        return 0;
    }
    command_pending = 0;
    *out = command_result_pending;
    return command_result_pending.status == USB_XFER_OK ? 1 : -1;
}

/**
 * Return what is at a location, the hub on the root port or a device on one
 * of its ports, and its speed in `speed`. A port above fifteen is written as
 * fifteen in the route string, and the translator's port number gives the
 * real one, as it does for a controller.
 */
static int kind_at(const struct usb_location *where, int *speed)
{
    if (where->depth == 0) {
        *speed = model.root_speed;
        return model.root_connected ? KIND_HUB : KIND_NONE;
    }
    if (where->depth == 1) {
        int port = (int)(where->route & 0xFu);

        if (port == 15 && where->tt_port != 0) {
            port = where->tt_port;
        }
        if (port >= 1 && port <= MODEL_PORTS) {
            *speed = model.port_speed[port - 1];
            return model.port_kind[port - 1];
        }
    }
    return KIND_NONE;
}

/**
 * Issue Enable Slot for the device at `where` and give it the next slot, or
 * answer `USB_NO_SLOTS` once `slot_cap` slots are given. With `pull_on_open`
 * the device is pulled while the command is held unanswered.
 */
int xhci_device_open(struct xhci_ctrl *hc, const struct usb_location *where, int speed)
{
    int n;

    if (slots_given >= (int)(sizeof(slot_of) / sizeof(slot_of[0])) - 1) {
        return -1;
    }
    if (model.slot_cap != 0 && slots_given >= model.slot_cap) {
        command_no_slots();
        return 0; /* the command went out; its answer is the refusal */
    }
    n = ++slots_given;
    slot_of[n].where = *where;
    slot_of[n].opened_speed = speed;
    slot_of[n].kind = kind_at(where, &slot_of[n].speed);
    slot_of[n].open = 1;
    command_ok(n);
    if (model.pull_on_open && where->depth == 1) {
        int port = (int)(where->route & 0xFu);

        model.pull_on_open = 0;
        model.hold_commands = 1;
        model.port_kind[port - 1] = KIND_NONE;
        model.port_change[port - 1] |= 1u;
        model_change(port);
    }
    return 0;
}

/** Issue Address Device, which succeeds. */
int xhci_device_address(struct xhci_ctrl *hc, int device)
{
    command_ok(device);
    return 0;
}

/** Issue Evaluate Context for the packet size, which succeeds. */
int xhci_device_packet_size(struct xhci_ctrl *hc, int device, int max_packet)
{
    (void)max_packet;
    command_ok(device);
    return 0;
}

/** Return 1 when no command is pending. */
int xhci_command_free(struct xhci_ctrl *hc)
{
    return !command_pending;
}

/** Return 0, so a failure records no controller state. */
int xhci_interrupts_on(struct xhci_ctrl *hc)
{
    return 0; /* the model's controller sends no interrupts of its own */
}

/** Do nothing: there are no registers to record. */
void xhci_state_record(struct xhci_ctrl *hc)
{}

/** Record what the controller was told of a hub, and issue the command. */
int xhci_device_hub(struct xhci_ctrl *hc, int device, int ports, int multi_tt, int think_time)
{
    model.hub_told = 1;
    model.hub_told_slot = device;
    model.hub_told_ports = ports;
    model.hub_told_multi = multi_tt;
    model.hub_told_think = think_time;
    command_ok(device);
    return 0;
}

/** Issue Configure Endpoint, which succeeds. */
int xhci_endpoint_open(struct xhci_ctrl *hc, int device, const struct usb_endpoint *endpoint)
{
    (void)endpoint;
    command_ok(device);
    return 0;
}

/** Record the slot closed, in order, and issue Disable Slot, which fails
 * when `close_refuse` is set. */
int xhci_device_close(struct xhci_ctrl *hc, int device)
{
    if (closes < 16) {
        close_order[closes] = device;
    }
    closes++;
    slot_of[device].open = 0;
    command_ok(device);
    if (model.close_refuse) {
        command_result_pending.status = USB_XFER_FAILED;
    }
    return 0;
}

/** Count a slot the core gave back. */
void xhci_device_closed(struct xhci_ctrl *hc, int device)
{
    (void)device;
    closed_done++;
}

/** Issue Reset Endpoint, which succeeds. */
int xhci_endpoint_reset(struct xhci_ctrl *hc, int device, int endpoint)
{
    (void)endpoint;
    command_ok(device);
    return 0;
}

/** Issue Stop Endpoint, which succeeds. */
int xhci_endpoint_stop(struct xhci_ctrl *hc, int device, int endpoint)
{
    (void)endpoint;
    command_ok(device);
    return 0;
}

/** Issue Set TR Dequeue Pointer, which succeeds. */
int xhci_endpoint_dequeue_here(struct xhci_ctrl *hc, int device, int endpoint)
{
    (void)endpoint;
    command_ok(device);
    return 0;
}

/** Return `XHCI_EP_RUNNING`: no endpoint halts here. */
int xhci_endpoint_state(struct xhci_ctrl *hc, int device, int endpoint)
{
    (void)device;
    (void)endpoint;
    return XHCI_EP_RUNNING;
}

/** Do nothing: no row checks the quiescing flag. */
void xhci_endpoint_quiescing(struct xhci_ctrl *hc, int device, int endpoint, int on)
{
    (void)device;
    (void)endpoint;
    (void)on;
}

/** Do nothing: no transfer is queued to retire. */
void xhci_endpoint_retire_all(struct xhci_ctrl *hc, int device, int endpoint)
{
    (void)device;
    (void)endpoint;
}

/** Do nothing: there is no stick. */
void msc_slot_closed(struct xhci_ctrl *hc, int device)
{
    (void)device;
}

/* The descriptors the two kinds answer with. */
static const unsigned char keyboard_config[] = {
    9, 2,  34,   0, 1, 1, 0,  0xA0, 50, /* configuration, value 1 */
    9, 4,  0,    0, 1, 3, 1,  1,    0,  /* interface: HID, boot, keyboard */
    9, 33, 0x11, 1, 0, 1, 34, 63,   0,  /* the HID descriptor */
    7, 5,  0x81, 3, 8, 0, 10            /* endpoint 1, in, interrupt */
};

static unsigned char hub_config[31];

/*
 * A configuration of 115 bytes: a vendor block of ninety, then the interface
 * and its endpoint. A driver that read only the first sixty-four bytes would
 * find no interface here at all.
 */
#define LONG_CONFIG_LEN 115
static unsigned char long_config[LONG_CONFIG_LEN];

/** Build `long_config`: a boot keyboard's interface after a vendor block. */
static void long_config_fill(void)
{
    int i;

    for (i = 0; i < LONG_CONFIG_LEN; i++) {
        long_config[i] = 0;
    }
    long_config[0] = 9;
    long_config[1] = 2;
    long_config[2] = LONG_CONFIG_LEN;
    long_config[3] = 0;
    long_config[4] = 1;
    long_config[5] = 1;
    long_config[6] = 0;
    long_config[7] = 0xA0;
    long_config[8] = 50;
    /* A vendor descriptor of ninety bytes, which a walker steps over. */
    long_config[9] = 90;
    long_config[10] = 0x21;
    /* The interface and its endpoint, past byte ninety-nine. */
    long_config[99] = 9;
    long_config[100] = 4;
    long_config[103] = 1;
    long_config[104] = 3;
    long_config[105] = 1;
    long_config[106] = 1;
    long_config[108] = 7;
    long_config[109] = 5;
    long_config[110] = 0x81;
    long_config[111] = 3;
    long_config[112] = 8;
    long_config[114] = 10;
}

/* A USB 2 hub's configuration, or a SuperSpeed hub's: the interface's
 * protocol is the translator kind for USB 2 and zero for USB 3, whose 3 is
 * in the device descriptor; a SuperSpeed hub's status endpoint takes two
 * bytes at interval 8 and is followed by its companion descriptor. */
static void hub_config_fill(void)
{
    unsigned char c[31] = {
        9, 2,    25,   0, 1, 1, 0,  0xE0, 0, /* configuration, value 1 */
        9, 4,    0,    0, 1, 9, 0,  0,    0, /* interface: hub, protocol below */
        7, 5,    0x81, 3, 1, 0, 12,          /* endpoint 1, in, interrupt */
        6, 0x30, 0,    0, 0, 0               /* the SuperSpeed companion */
    };
    int i;

    if (model.hub_protocol == 3) {
        c[2] = 31;
        c[22] = 2;
        c[24] = 8;
    }

    c[16] = (unsigned char)(model.hub_protocol == 3 ? 0 : model.hub_protocol);
    for (i = 0; i < 31; i++) {
        hub_config[i] = c[i];
    }
}

/**
 * Return the status word of hub port `port`, from 1. USB 3 puts the power bit
 * at 9 and has no low- and high-speed bits; its speed field above them says
 * which SuperSpeed, 0 for 5 Gbps, and bits 5 to 8 are the link state.
 */
static unsigned int port_status_word(int port)
{
    unsigned int status =
        model.port_powered_off[port - 1] ? 0 : (model.hub_protocol == 3 ? 0x0200 : 0x0100);

    if (model.port_over_current[port - 1]) {
        status |= 0x0008;
    }
    if (model.port_kind[port - 1] != KIND_NONE && !model.port_powered_off[port - 1]) {
        status |= 0x0001;
        if (model.port_enabled[port - 1]) {
            status |= 0x0002;
        }
        if (model.hub_protocol != 3 && model.port_speed[port - 1] == USB_SPEED_LOW) {
            status |= 0x0200;
        } else if (model.hub_protocol != 3 && model.port_speed[port - 1] == USB_SPEED_HIGH) {
            status |= 0x0400;
        }
    }
    if (model.port_in_reset[port - 1]) {
        status |= 0x0010;
    }
    if (model.hub_protocol == 3) {
        status |= (unsigned int)(model.port_link[port - 1] & 0xF) << 5;
    }
    return status;
}

/** Answer the outstanding change transfer with the bits that changed. */
static void model_report_changes(void)
{
    if (model.change_device == 0 || model.change_buffer == 0) {
        return;
    }
    {
        uint32_t b;

        for (b = 0; b < model.change_length && b < 4; b++) {
            model.change_buffer[b] = (unsigned char)((model.change_bits >> (8 * b)) & 0xFF);
        }
    }
    answer(model.change_device, 3, USB_XFER_OK, model.change_length, 0, model.change_generation);
    model.change_device = 0;
    model.change_bits = 0;
}

/** A change on port `port` (0 for the hub itself): set the bit and report it
 * if the hub driver's transfer is out. */
static void model_change(int port)
{
    model.change_bits |= 1u << port;
    model_report_changes();
}

/**
 * Answer SET_PORT_FEATURE on `port`: power, a warm reset or a reset. Return
 * -1 for a power-on the row says is not queued, and 0 otherwise.
 */
static int set_port_feature(int device, int feature, int port, uint32_t generation)
{
    if (feature == 8) {
        if (port >= 1 && port <= MODEL_PORTS && model.port_powered_off[port - 1] &&
            model.power_on_unqueued > 0) {
            model.power_on_unqueued--;
            return -1;
        }
        model.power_requests++;
        model.last_power_ms = model.now_ms;
        if (port >= 1 && port <= MODEL_PORTS && model.port_powered_off[port - 1]) {
            if (model.power_on_refuse) {
                answer(device, 1, USB_XFER_STALLED, 0, 0, generation);
                return 0;
            }
            model.port_powered_off[port - 1] = 0;
            model.power_ons++;
        }
    } else if (feature == 28 && port >= 1 && port <= MODEL_PORTS) {
        /* BH_PORT_RESET: the link returns to U0, both reset changes set. */
        model.warm_resets++;
        if (model.port_kind[port - 1] == KIND_NONE) {
            answer(device, 1, USB_XFER_OK, 0, 0, generation);
            return 0;
        }
        model.port_link[port - 1] = 0;
        model.port_enabled[port - 1] = 1;
        model.port_change[port - 1] |= 0x30u;
        answer(device, 1, USB_XFER_OK, 0, 0, generation);
        model_change(port);
        return 0;
    } else if (feature == 4 && port >= 1 && port <= MODEL_PORTS) {
        /* A hub stalls a reset while C_PORT_RESET is unacknowledged. */
        if (model.port_change[port - 1] & 16u) {
            answer(device, 1, USB_XFER_STALLED, 0, 0, generation);
            return 0;
        }
        model.reset_requests++;
        model.port_in_reset[port - 1] = 1;
        model.port_reset_reads[port - 1] = 0;
    }
    answer(device, 1, USB_XFER_OK, 0, 0, generation);
    return 0;
}

/**
 * Take a transfer on an open slot and answer it as what is at the slot's
 * location would; return -1 for a slot not open, or for a power-on the row
 * says is not queued. A hub's change endpoint is answered when a change is
 * reported, and any other endpoint but the control endpoint, 1, stays out.
 * On endpoint 1 the standard requests are answered for both kinds; a
 * keyboard stalls everything else. A SuperSpeed
 * hub has the USB 3 hub descriptor and stalls a request for the USB 2 one,
 * as the specification's hubs do, and a USB 2 hub the other way round. A
 * warm reset on an empty port reports nothing.
 */
int xhci_submit(struct xhci_ctrl *hc, int device, int endpoint, const struct usb_transfer *transfer)
{
    const unsigned char *setup = transfer->setup;
    unsigned char *buf = transfer->buffer;
    int kind;
    uint32_t i;

    if (device <= 0 || device >= (int)(sizeof(slot_of) / sizeof(slot_of[0])) ||
        !slot_of[device].open) {
        return -1;
    }
    kind = slot_of[device].kind;
    if (endpoint != 1) {
        if (kind == KIND_HUB) {
            /* The change endpoint: answered when something changes. */
            model.change_submits++;
            model.change_device = device;
            model.change_buffer = buf;
            model.change_length = transfer->length;
            model.change_generation = transfer->generation;
            if (model.change_stall) {
                answer(device, endpoint, USB_XFER_STALLED, transfer->length, transfer->length,
                       transfer->generation);
                model.change_device = 0;
            } else if (model.change_bits != 0) {
                model_report_changes();
            }
        }
        return 0; /* a report transfer stays out; nothing here types */
    }
    for (i = 0; i < transfer->length && buf != 0; i++) {
        buf[i] = 0;
    }
    if (setup[0] == 0x80 && setup[1] == 6) { /* GET_DESCRIPTOR */
        int type = setup[3];
        uint32_t want = transfer->length;
        uint32_t have = 0;

        if (type == 1) {
            buf[0] = 18;
            buf[1] = 1;
            if (kind == KIND_HUB) {
                buf[4] = 9; /* a hub's class, and a SuperSpeed hub's protocol 3 */
                buf[6] = (unsigned char)(model.hub_protocol == 3 ? 3 : 0);
            }
            buf[7] = (unsigned char)(kind == KIND_HUB && model.hub_protocol == 3 ? 9 : 64);
            buf[8] = 0x09;
            buf[9] = 0x04; /* vendor 0x0409 */
            buf[10] = kind == KIND_HUB ? 0xAA : 0x01;
            buf[11] = kind == KIND_HUB ? 0x55 : 0x00;
            buf[17] = 1;
            have = 18;
        } else if (type == 2) {
            const unsigned char *cfg = kind == KIND_HUB ? hub_config : keyboard_config;
            uint32_t len = (uint32_t)sizeof(keyboard_config);

            if (kind == KIND_HUB) {
                hub_config_fill();
                cfg = hub_config;
                len = hub_config[2];
            } else if (model.long_config) {
                long_config_fill();
                if (model.config_claims != 0) {
                    /* A header claiming a length the device will not give. */
                    long_config[2] = (unsigned char)(model.config_claims & 0xFF);
                    long_config[3] = (unsigned char)((model.config_claims >> 8) & 0xFF);
                }
                cfg = long_config;
                len = LONG_CONFIG_LEN;
            }
            for (i = 0; i < len && i < want; i++) {
                buf[i] = cfg[i];
            }
            have = len;
        }
        if (have > want) {
            have = want;
        }
        answer(device, 1, have < want ? USB_XFER_SHORT : USB_XFER_OK, want, want - have,
               transfer->generation);
        return 0;
    }
    if (setup[0] == 0x00 && (setup[1] == 9)) { /* SET_CONFIGURATION */
        if (model.config_delay_ms > 0 && kind != KIND_HUB) {
            /* A device starting its flash: `pump` answers when it is due. */
            model.config_due_ms = (int)model.now_ms + model.config_delay_ms;
            model.config_device = device;
            model.config_generation = transfer->generation;
            return 0;
        }
        answer(device, 1, USB_XFER_OK, 0, 0, transfer->generation);
        return 0;
    }
    if (setup[0] == 0x21 && setup[1] == 0x0B) { /* SET_PROTOCOL */
        answer(device, 1, USB_XFER_OK, 0, 0, transfer->generation);
        return 0;
    }
    if (setup[0] == 0x01 && setup[1] == 11) { /* SET_INTERFACE */
        if (model.alt_refuse) {
            answer(device, 1, USB_XFER_STALLED, 0, 0, transfer->generation);
            return 0;
        }
        model.alt_set = setup[2];
        answer(device, 1, USB_XFER_OK, 0, 0, transfer->generation);
        return 0;
    }
    if (kind != KIND_HUB) {
        answer(device, 1, USB_XFER_STALLED, transfer->length, transfer->length,
               transfer->generation);
        return 0;
    }
    /* The hub's class requests. */
    if (setup[0] == 0xA0 && setup[1] == 6 && setup[3] == 0x2A) {
        if (model.hub_protocol != 3 || model.hub_desc_stall) {
            answer(device, 1, USB_XFER_STALLED, transfer->length, transfer->length,
                   transfer->generation);
            return 0;
        }
        buf[0] = 12;
        buf[1] = 0x2A;
        buf[2] = (unsigned char)model.hub_ports;
        buf[3] = (unsigned char)model.hub_lpsm;
        buf[4] = 0;
        buf[5] = (unsigned char)model.hub_pwrgood;
        buf[6] = 50;
        buf[7] = 0; /* header decode latency */
        buf[8] = 0; /* hub delay */
        buf[9] = 0;
        buf[10] = 0; /* removable */
        buf[11] = 0;
        answer(device, 1, transfer->length > 12 ? USB_XFER_SHORT : USB_XFER_OK, transfer->length,
               transfer->length > 12 ? transfer->length - 12 : 0, transfer->generation);
        return 0;
    }
    if (setup[0] == 0x20 && setup[1] == 12) { /* SET_HUB_DEPTH */
        model.hub_depth_sets++;
        model.hub_depth = setup[2] | (setup[3] << 8);
        if (!model.hub_told) {
            model.hub_depth_before_tell = 1;
        }
        answer(device, 1,
               model.hub_protocol == 3 && !model.hub_depth_refuse ? USB_XFER_OK : USB_XFER_STALLED,
               0, 0, transfer->generation);
        return 0;
    }
    if (setup[0] == 0xA0 && setup[1] == 6 && setup[3] == 0x29) {
        if (model.hub_protocol == 3 || model.hub_desc_stall) {
            answer(device, 1, USB_XFER_STALLED, transfer->length, transfer->length,
                   transfer->generation);
            return 0;
        }
        buf[0] = 9;
        buf[1] = 0x29;
        buf[2] = (unsigned char)model.hub_ports;
        buf[3] = (unsigned char)(model.hub_lpsm | (model.hub_think << 5));
        buf[4] = 0;
        buf[5] = (unsigned char)model.hub_pwrgood;
        buf[6] = 50;
        buf[7] = 0;
        buf[8] = 0xFF;
        answer(device, 1, transfer->length > 9 ? USB_XFER_SHORT : USB_XFER_OK, transfer->length,
               transfer->length > 9 ? transfer->length - 9 : 0, transfer->generation);
        return 0;
    }
    if (setup[0] == 0xA0 && setup[1] == 0) { /* GET_STATUS of the hub */
        model.hub_status_reads++;
        buf[0] = (unsigned char)(model.hub_status & 0xFF);
        buf[1] = (unsigned char)(model.hub_status >> 8);
        buf[2] = (unsigned char)(model.hub_change & 0xFF);
        buf[3] = (unsigned char)(model.hub_change >> 8);
        answer(device, 1, USB_XFER_OK, 4, 0, transfer->generation);
        return 0;
    }
    if (setup[0] == 0x20 && setup[1] == 1) { /* CLEAR_FEATURE of the hub */
        model.hub_clears++;
        model.hub_change &= ~(1u << setup[2]);
        answer(device, 1, USB_XFER_OK, 0, 0, transfer->generation);
        return 0;
    }
    if (setup[0] == 0xA3 && setup[1] == 0) { /* GET_PORT_STATUS */
        int port = setup[4];
        unsigned int status;

        if (port < 1 || port > MODEL_PORTS) {
            answer(device, 1, USB_XFER_STALLED, 4, 4, transfer->generation);
            return 0;
        }
        model.status_reads++;
        if (model.status_reads == 1) {
            model.first_status_ms = model.now_ms;
        }
        if (model.short_clear_seen && model.short_after_clear == port) {
            /* One byte where four were asked for. */
            model.short_clear_seen = 0;
            model.short_after_clear = 0;
            buf[0] = (unsigned char)(port_status_word(port) & 0xFF);
            answer(device, 1, USB_XFER_SHORT, 4, 3, transfer->generation);
            return 0;
        }
        if (model.port_in_reset[port - 1] && !model.port_reset_never[port - 1]) {
            model.port_reset_reads[port - 1]++;
            if (model.port_reset_reads[port - 1] >= 2) {
                model.port_in_reset[port - 1] = 0;
                model.port_enabled[port - 1] = 1;
                model.port_change[port - 1] |= 16u; /* C_PORT_RESET, owed a clear */
            }
        }
        status = port_status_word(port);
        buf[0] = (unsigned char)(status & 0xFF);
        buf[1] = (unsigned char)(status >> 8);
        buf[2] = (unsigned char)(model.port_change[port - 1] & 0xFF);
        buf[3] = (unsigned char)(model.port_change[port - 1] >> 8);
        answer(device, 1, USB_XFER_OK, 4, 0, transfer->generation);
        if (model.connect_on_read == port) {
            /* The device lands after the read; the latched change is its. */
            model.connect_on_read = 0;
            model.port_kind[port - 1] = KIND_KBD;
        }
        return 0;
    }
    if (setup[0] == 0x23 && setup[1] == 3) { /* SET_PORT_FEATURE */
        return set_port_feature(device, setup[2], setup[4], transfer->generation);
    }
    if (setup[0] == 0x23 && setup[1] == 1) { /* CLEAR_PORT_FEATURE */
        int feature = setup[2];
        int port = setup[4];

        if (feature == 20) {
            model.reset_clears++;
        }
        if (model.clears < 32) {
            model.cleared_feature[model.clears] = feature;
            model.cleared_port[model.clears] = port;
        }
        model.clears++;
        if (feature >= 16 && feature <= 20 && port >= 1 && port <= MODEL_PORTS) {
            model.port_change[port - 1] &= ~(1u << (feature - 16));
        }
        /* USB 3's warm reset, link state and config error changes: bits 5-7. */
        if (port >= 1 && port <= MODEL_PORTS) {
            if (feature == 29) {
                model.port_change[port - 1] &= ~(1u << 5);
            } else if (feature == 25) {
                model.port_change[port - 1] &= ~(1u << 6);
            } else if (feature == 26) {
                model.port_change[port - 1] &= ~(1u << 7);
            }
        }
        if (feature == 8 && port >= 1 && port <= MODEL_PORTS) {
            model.power_offs++;
            model.port_powered_off[port - 1] = 1;
        }
        if (model.short_after_clear == port) {
            model.short_clear_seen = 1;
        }
        answer(device, 1, USB_XFER_OK, 0, 0, transfer->generation);
        return 0;
    }
    answer(device, 1, USB_XFER_STALLED, transfer->length, transfer->length, transfer->generation);
    return 0;
}

/* The times the core masked interrupts, and how many masks are in force. */
static int irq_saves;
static int irq_depth;

/** Count a mask and return 0: the host has no interrupts to mask. */
uint32_t kirq_save(void)
{
    irq_saves++;
    irq_depth++;
    return 0;
}

/** Count the mask's end. */
void kirq_restore(uint32_t flags)
{
    (void)flags;
    irq_depth--;
}

#include "../../rach2/sys/usb/core.c"
#include "../../rach2/sys/usb/hub.c"

/* ---- the rows ---- */

static int fails;

#define CHECK(what, cond)                 \
    do {                                  \
        if (!(cond)) {                    \
            printf("  FAIL: %s\n", what); \
            fails++;                      \
        }                                 \
    } while (0)

/**
 * Reset the model, the core and the hub driver to a full-speed hub on root
 * port 0 with eight empty ports, no power switching and a power-good time of
 * 20 ms.
 */
static void model_reset(void)
{
    int i;
    unsigned long k;

    for (k = 0; k < sizeof(model); k++) {
        ((unsigned char *)&model)[k] = 0;
    }
    for (k = 0; k < sizeof(slot_of); k++) {
        ((unsigned char *)slot_of)[k] = 0;
    }
    slots_given = 0;
    closes = 0;
    closed_done = 0;
    full_records = 0;
    hid_gone_count = 0;
    command_pending = 0;
    done_head = 0;
    done_tail = 0;
    model.root_connected = 1;
    model.root_speed = USB_SPEED_FULL;
    model.hub_protocol = 0;
    model.hub_ports = 8;
    model.hub_lpsm = 2;
    model.hub_think = 0;
    model.hub_pwrgood = 10; /* 20 ms */
    for (i = 0; i < MODEL_PORTS; i++) {
        model.port_kind[i] = KIND_NONE;
        model.port_speed[i] = USB_SPEED_FULL;
    }
    usb_core_reset();
    for (i = 0; i < HUB_MAX; i++) {
        hub_release(i);
    }
}

/** Step the core for `ms` milliseconds, ten at a time, answering a delayed
 * SET_CONFIGURATION when it is due. */
static void pump(uint32_t ms)
{
    uint32_t t;

    for (t = 0; t < ms; t += 10) {
        model.now_ms += 10;
        if (model.config_due_ms > 0 && (int)model.now_ms >= model.config_due_ms) {
            model.config_due_ms = 0;
            answer(model.config_device, 1, USB_XFER_OK, 0, 0, model.config_generation);
        }
        usb_core_step(10);
    }
}

/** Return the slot of the first open device of `kind`, or 0. */
static int slot_of_kind(int kind)
{
    int n;

    for (n = 1; n <= slots_given; n++) {
        if (slot_of[n].open && slot_of[n].kind == kind) {
            return n;
        }
    }
    return 0;
}

/** Return the slot of the open device on hub port `port`, or 0: a port above
 * fifteen is written as fifteen in the route string and named by the
 * translator's port. */
static int slot_at_port(int port)
{
    int n;

    for (n = 1; n < (int)(sizeof(slot_of) / sizeof(slot_of[0])); n++) {
        if (!slot_of[n].open || slot_of[n].where.depth != 1) {
            continue;
        }
        if (slot_of[n].where.route == 0xFu ? slot_of[n].where.tt_port == port
                                           : slot_of[n].where.route == (uint32_t)port) {
            return n;
        }
    }
    return 0;
}

/** Return the slot of the open device whose route is `route`, or 0. */
static int slot_at_route(uint32_t route)
{
    int n;

    for (n = 1; n <= slots_given; n++) {
        if (slot_of[n].open && slot_of[n].where.depth == 1 && slot_of[n].where.route == route) {
            return n;
        }
    }
    return 0;
}

/** Return the number of core entries in the running state. */
static int running_count(void)
{
    int i;
    int n = 0;

    for (i = 0; i < USB_MAX_DEVICES; i++) {
        if (dev_at(i)->state == ST_RUNNING) {
            n++;
        }
    }
    return n;
}

/** Return the number of free core entries. */
static int free_count(void)
{
    int i;
    int n = 0;

    for (i = 0; i < USB_MAX_DEVICES; i++) {
        if (dev_at(i)->state == ST_FREE) {
            n++;
        }
    }
    return n;
}

/** Return 1 when the hub was asked to clear `feature` on `port` since the
 * reset. */
static int cleared(int feature, int port)
{
    int i;

    for (i = 0; i < model.clears && i < 32; i++) {
        if (model.cleared_feature[i] == feature && model.cleared_port[i] == port) {
            return 1;
        }
    }
    return 0;
}

/** Return 1 when `needle` occurs in `hay`. */
static int contains(const char *hay, const char *needle)
{
    const char *h;

    for (h = hay; *h; h++) {
        const char *a = h;
        const char *b = needle;

        while (*a && *b && *a == *b) {
            a++;
            b++;
        }
        if (!*b) {
            return 1;
        }
    }
    return 0;
}

/**
 * Check that a detached hub's record and buffers are kept, and not given to
 * another hub, until its Disable Slot succeeds; `refused` makes the Disable
 * Slot fail, which keeps them for good.
 */
static void detached_record(int refused)
{
    struct hub *old;
    int index;

    model_reset();
    pump(2000);
    old = hubs[0];
    CHECK("the hub has an interrupt buffer in flight", old && model.change_buffer == old->bitmap);
    if (!old) {
        return;
    }
    index = old->index;
    model.root_connected = 0;
    model.hold_commands = !refused;
    model.close_refuse = refused;
    pump(30);
    CHECK("the detached core entry retains its slot",
          dev_at(index)->state == (refused ? ST_TOMBSTONE : ST_CLOSING));
    old->data[0] = 0xA5;
    old->bitmap[0] = 0x5A;
    slot_of[2].open = 1;
    slot_of[2].kind = KIND_HUB;
    CHECK("another hub attaches while the old slot is retained",
          hub_attach(the_ctrl, USB_MAX_DEVICES - 1, 2, 0, 0, 3, 0, 0) == 0);
    CHECK("a detached hub record is not reused",
          hubs[0] == old && !old->used && old->index == index && old->device == 1 &&
              old->data[0] == 0xA5 && old->bitmap[0] == 0x5A);
    hub_detach(USB_MAX_DEVICES - 1);
    model.hold_commands = 0;
    pump(30);
    if (refused) {
        CHECK("failed Disable Slot keeps the detached hub buffers", hubs[0] == old);
    } else {
        CHECK("successful Disable Slot releases the detached hub buffers", hubs[0] == 0);
    }
}

/**
 * Check closure, recovery and acknowledgements for concurrent port changes.
 */
static void concurrent_changes(void)
{
    int kbd;
    int bits;

    for (bits = 1; bits <= 2; bits++) {
        model_reset();
        model.port_kind[1] = KIND_KBD;
        pump(2000);
        kbd = slot_of_kind(KIND_KBD);
        if (bits == 2) {
            model.port_enabled[1] = 0;
        }
        model.port_change[1] = (unsigned int)bits | 8u;
        model_change(2);
        pump(2000);
        CHECK("cleared over-current does not suppress another change",
              closes == 1 && close_order[0] == kbd && slots_given == 3 && running_count() == 2);
        CHECK("both USB 2 change bits are acknowledged", cleared(19, 2) && cleared(15 + bits, 2));
    }
    model_reset();
    model.root_speed = USB_SPEED_SUPER;
    model.hub_protocol = 3;
    model.port_kind[1] = KIND_KBD;
    model.port_speed[1] = USB_SPEED_SUPER;
    pump(2000);
    kbd = slot_of_kind(KIND_KBD);
    model.port_link[1] = 6;
    model.port_change[1] = 0xE9u;
    model_change(2);
    pump(2000);
    CHECK("a simultaneous connection and link fault starts a warm reset",
          closes == 1 && close_order[0] == kbd && model.warm_resets == 1 && running_count() == 2);
    CHECK("all concurrent USB 3 changes are acknowledged",
          cleared(16, 2) && cleared(19, 2) && cleared(29, 2) && cleared(25, 2) && cleared(26, 2));

    model_reset();
    model.root_speed = USB_SPEED_SUPER;
    model.hub_protocol = 3;
    model.port_kind[1] = KIND_KBD;
    model.port_speed[1] = USB_SPEED_SUPER;
    pump(2000);
    hubs[0]->port[1].warm = 1;
    model.port_change[1] = 0x28u;
    model_change(2);
    pump(500);
    CHECK("cleared over-current does not suppress warm reset completion", !hubs[0]->port[1].warm);

    model_reset();
    model.root_speed = USB_SPEED_SUPER;
    model.hub_protocol = 3;
    model.port_kind[1] = KIND_KBD;
    model.port_speed[1] = USB_SPEED_SUPER;
    pump(2000);
    model.port_link[1] = 6;
    model.port_over_current[1] = 1;
    model.port_change[1] = 0xE9u;
    model_change(2);
    pump(500);
    CHECK("active over-current prevents concurrent enumeration and warm reset",
          closes == 1 && model.power_offs == 1 && model.warm_resets == 0 && running_count() == 1);
}

/** Read `/proc/usb`'s lines for a keyboard on port 2 of the hub. */
static void proc_usb_lines(void)
{
    char line[96];
    int i;
    int found = 0;
    int saves = irq_saves;

    line[0] = '\0';
    for (i = 0; i < USB_MAX_DEVICES; i++) {
        if (usb_core_describe(i, line, (int)sizeof(line)) > 0 && contains(line, "class 3")) {
            found = contains(line, "port 1.2 ");
        }
    }
    CHECK("/proc/usb gives the keyboard's location as 1.2", found);
    CHECK("and reads the device table with interrupts off",
          irq_saves == saves + USB_MAX_DEVICES && irq_depth == 0);
}

/** Pull a keyboard whose report endpoint halted before it is quiesced. */
static void halted_pulled(void)
{
    int i;
    int kbd;
    int hub_index = -1;
    int kbd_index = -1;

    model_reset();
    model.port_kind[1] = KIND_KBD;
    pump(2000);
    kbd = slot_of_kind(KIND_KBD);
    for (i = 0; i < USB_MAX_DEVICES; i++) {
        if (dev_at(i)->state == ST_RUNNING && dev_at(i)->interface.kind == USB_KIND_HUB) {
            hub_index = i;
        }
        if (dev_at(i)->state == ST_RUNNING && kbd != 0 && dev_at(i)->slot == kbd) {
            kbd_index = i;
        }
    }
    CHECK("a keyboard is running behind the hub", hub_index >= 0 && kbd_index >= 0);
    if (hub_index < 0 || kbd_index < 0) {
        return;
    }
    model.hold_commands = 1;
    answer(kbd, dev_at(kbd_index)->ep_dci, USB_XFER_STALLED, 8, 8, 0);
    pump(100);
    CHECK("a keyboard whose report transfer stalls waits halted",
          dev_at(kbd_index)->state == ST_HALTED);
    model.port_kind[1] = KIND_NONE;
    usb_core_hub_port(hub_index, 1, 0);
    CHECK("pulled while halted, its class driver is told",
          hid_gone_count == 1 && hid_gone_slot[0] == kbd_index + 1);
    model.hold_commands = 0;
    pump(1000);
    CHECK("and its slot is closed", closes == 1 && close_order[0] == kbd);
}

/** Power a port on after an over-current, the request not queued at the
 * first two tries. */
static void power_on_unqueued(void)
{
    model_reset();
    model.port_kind[1] = KIND_KBD;
    pump(2000);
    model.power_on_unqueued = 2;
    model.port_over_current[1] = 1;
    model.port_change[1] |= 8u;
    model_change(2);
    pump(500);
    model.port_over_current[1] = 0;
    pump(1500);
    CHECK("a power-on that cannot be queued is asked for again until it is",
          model.power_on_unqueued == 0 && model.power_ons == 1);
}

/**
 * Run the rows and print the result; return 1 when a row failed.
 *
 * A hub may say it is ganged with power good in 0 ms and yet power only the
 * port a request names, as the hub 35d6:2510 does, so every port is asked
 * for and no port is read before 100 ms. A SuperSpeed hub is told its depth
 * before the controller is told it is a hub. When a ring holds a command
 * nobody answers, the device that issued it gives up at its deadline, and
 * the next device must not take the turn and issue on the held ring. A
 * device behind a hub, a USB stick starting its flash at boot, may answer
 * SET_CONFIGURATION late, and is configured when the answer comes within five
 * seconds. A device pulled during a reset the hub never finishes gives the
 * enumeration turn back.
 *
 * Some SuperSpeed hubs report a pull as the link falling inactive with the
 * device still shown connected, then the disconnection while the warm reset
 * that answered the first report is out; the connection change ends the warm
 * reset. A port the scan reads as disconnected, with a connection change
 * already latched, may get its device the moment after the read; the change
 * the scan acknowledges is then the new device's, so the port is read again
 * after the acknowledgement. When that read comes back short, the latch is
 * cleared and the hub leaves the port out of its bitmap, so nothing would
 * ask again; the hub is closed instead.
 */
int main(void)
{
    int hub;
    int kbd;
    int port;

    printf("hub_hosttest: the hub driver and the tree\n");

    /* A full-speed hub with a keyboard already on its port 2. */
    model_reset();
    model.port_kind[1] = KIND_KBD;
    pump(2000);
    hub = slot_of_kind(KIND_HUB);
    kbd = slot_of_kind(KIND_KBD);
    CHECK("the hub is enumerated on the root port", hub != 0);
    CHECK("its descriptor is read and its eight ports told to the controller",
          model.hub_told && model.hub_told_ports == 8 && model.hub_told_slot == hub);
    CHECK("a full-speed hub is told nothing of translators",
          model.hub_told_multi == 0 && model.hub_told_think == 0);
    CHECK("a hub that cannot switch power is asked to power nothing", model.power_requests == 0);
    CHECK("every port's status is read once after power", model.status_reads >= 8);
    CHECK("the keyboard on port 2 is found and enumerated", kbd != 0);
    CHECK("at its location: root port 0, one tier down, route 2",
          kbd != 0 && slot_of[kbd].where.root_port == 0 && slot_of[kbd].where.depth == 1 &&
              slot_of[kbd].where.route == 0x2u);
    CHECK("with no translator below a full-speed hub",
          kbd != 0 && slot_of[kbd].where.tt_device == 0);
    CHECK("its port was reset through the hub and the reset change cleared",
          model.reset_requests == 1 && model.reset_clears == 1);
    CHECK("and both are running", running_count() == 2);
    proc_usb_lines();

    /* Power: one request per port, ganged or not, then the power-good wait. */
    model_reset();
    model.hub_lpsm = 0;
    model.hub_pwrgood = 100; /* 200 ms */
    pump(2000);
    CHECK("a ganged hub is powered port by port too", model.power_requests == 8);
    CHECK("and no port is read before the power-good time",
          model.first_status_ms >= model.last_power_ms + 200u);
    model_reset();
    model.hub_lpsm = 1;
    pump(2000);
    CHECK("a per-port hub is powered port by port", model.power_requests == 8);

    /* A ganged hub that powers only the port named, power good in 0 ms. */
    model_reset();
    model.hub_lpsm = 0;
    model.hub_pwrgood = 0;
    model.hub_ports = 4;
    model.port_powered_off[0] = model.port_powered_off[1] = 1;
    model.port_powered_off[2] = model.port_powered_off[3] = 1;
    model.port_kind[2] = KIND_KBD;
    model.port_speed[2] = USB_SPEED_LOW;
    pump(2000);
    CHECK("a hub that powers only the port named has every port asked for",
          model.power_requests == 4 && model.power_ons == 4);
    CHECK("its keyboard on port 3 is enumerated", slot_of_kind(KIND_KBD) != 0);
    CHECK("and with power good in 0 ms no port is read before 100 ms",
          model.first_status_ms >= model.last_power_ms + 100u);

    /* A high-speed hub, a translator per port, think time 2, keyboard below. */
    model_reset();
    model.root_speed = USB_SPEED_HIGH;
    model.hub_protocol = 2;
    model.hub_think = 2;
    model.port_kind[2] = KIND_KBD;
    model.port_speed[2] = USB_SPEED_LOW;
    pump(2000);
    hub = slot_of_kind(KIND_HUB);
    kbd = slot_of_kind(KIND_KBD);
    CHECK("a multi-TT hub's slot is told its translators and think time",
          model.hub_told_multi == 1 && model.hub_told_think == 2);
    CHECK("a low-speed keyboard below it is enumerated", kbd != 0);
    CHECK("at low speed, read from the status word",
          kbd != 0 && slot_of[kbd].opened_speed == USB_SPEED_LOW);
    CHECK("carrying the hub as its translator at port 3, per port",
          kbd != 0 && slot_of[kbd].where.tt_device == hub && slot_of[kbd].where.tt_port == 3 &&
              slot_of[kbd].where.tt_multi == 1);
    CHECK("and a route of 3", kbd != 0 && slot_of[kbd].where.route == 0x3u);

    /* A SuperSpeed hub, protocol 3, on a SuperSpeed root port. */
    model_reset();
    model.root_speed = USB_SPEED_SUPER;
    model.hub_protocol = 3;
    model.hub_lpsm = 1;
    model.port_kind[1] = KIND_KBD;
    model.port_speed[1] = USB_SPEED_SUPER;
    pump(2000);
    hub = slot_of_kind(KIND_HUB);
    kbd = slot_of_kind(KIND_KBD);
    CHECK("a SuperSpeed hub is enumerated on the root port", hub != 0);
    CHECK("through the USB 3 hub descriptor, and told its eight ports", model.hub_told_ports == 8);
    CHECK("it is told its depth, zero on a root port, once",
          model.hub_depth_sets == 1 && model.hub_depth == 0);
    CHECK("before the controller is told it is a hub", model.hub_depth_before_tell == 1);
    CHECK("and told no translators and no think time",
          model.hub_told_multi == 0 && model.hub_told_think == 0);
    CHECK("its ports are powered", model.power_requests == 8);
    CHECK("the device on port 2 is found and enumerated", kbd != 0);
    CHECK("at SuperSpeed, from the hub's protocol and not a speed bit",
          kbd != 0 && slot_of[kbd].opened_speed == USB_SPEED_SUPER);
    CHECK("with no translator below a SuperSpeed hub",
          kbd != 0 && slot_of[kbd].where.tt_device == 0 && slot_of[kbd].where.tt_port == 0);
    CHECK("and a route of 2", kbd != 0 && slot_of[kbd].where.route == 0x2u);
    CHECK("its port was reset through the hub and the reset change cleared",
          model.reset_requests >= 1 && model.reset_clears >= 1);
    /* A configuration error with the link still up is only acknowledged. */
    model.clears = 0;
    model.port_change[1] |= 0x80u;
    model_change(2);
    pump(200);
    CHECK("a configuration error change is acknowledged with USB 3's feature 26", cleared(26, 2));
    CHECK("with no warm reset while the link is up", model.warm_resets == 0);
    CHECK("and the device below still running", running_count() == 2);
    /* The link falls to SS.Inactive, and comes back after a warm reset. */
    model.clears = 0;
    {
        int given = slots_given;

        model.port_link[1] = 6;
        model.port_change[1] |= 0x40u;
        model_change(2);
        pump(3000);
        CHECK("a link state change is acknowledged with USB 3's feature 25", cleared(25, 2));
        CHECK("a link in SS.Inactive gets a warm reset", model.warm_resets == 1);
        CHECK("whose end is acknowledged with USB 3's feature 29", cleared(29, 2));
        CHECK("and the device below is enumerated again on the link that came back",
              slots_given == given + 1 && slot_of_kind(KIND_KBD) != 0 && running_count() == 2);
    }

    /* A ring holding a command nobody answers. */
    model_reset();
    pump(2000);
    CHECK("the hub is up with nothing below it", running_count() == 1);
    model.hold_commands = 1;
    model.port_kind[1] = KIND_KBD;
    model.port_change[1] |= 1u;
    model_change(2);
    pump(300);
    model.port_kind[3] = KIND_KBD;
    model.port_change[3] |= 1u;
    model_change(4);
    pump(3000);
    CHECK("no command is issued on a ring that holds one", model.double_issues == 0);
    model.hold_commands = 0;

    /* A device that answers SET_CONFIGURATION after 1.5 s. */
    model_reset();
    model.port_kind[1] = KIND_KBD;
    model.config_delay_ms = 1500;
    pump(4000);
    CHECK("a device that answers its configuration after 1.5 s is enumerated",
          slot_of_kind(KIND_KBD) != 0 && running_count() == 2);

    /* A device pulled during a reset through the hub that never ends. */
    model_reset();
    model.port_kind[1] = KIND_KBD;
    model.port_reset_never[1] = 1;
    pump(600);
    CHECK("a device whose reset never ends is not running", running_count() == 1);
    model.port_kind[1] = KIND_NONE;
    model.port_change[1] |= 1u; /* a connection change: it left */
    model_change(2);
    pump(600);
    model.port_kind[3] = KIND_KBD;
    model.port_change[3] |= 1u;
    model_change(4);
    pump(2000);
    CHECK("a device pulled during its reset through the hub gives the turn back",
          slot_of_kind(KIND_KBD) != 0 && running_count() == 2);

    /* A pull reported as the link falling inactive, then a disconnection. */
    model_reset();
    model.root_speed = USB_SPEED_SUPER;
    model.hub_protocol = 3;
    model.hub_lpsm = 1;
    model.port_kind[1] = KIND_KBD;
    model.port_speed[1] = USB_SPEED_SUPER;
    pump(2000);
    CHECK("the device behind the SuperSpeed hub is running", running_count() == 2);
    model.port_link[1] = 6;
    model.port_change[1] |= 0x40u;
    model_change(2);
    for (port = 0; port < 20 && model.warm_resets == 0; port++) {
        pump(10);
    }
    CHECK("the link falling inactive asks for a warm reset", model.warm_resets == 1);
    model.port_kind[1] = KIND_NONE;
    model.port_change[1] |= 1u;
    model_change(2);
    pump(500);
    CHECK("and the pull closes the device", running_count() == 1);
    model.port_kind[1] = KIND_KBD;
    model.port_link[1] = 0;
    model.port_change[1] |= 1u;
    model_change(2);
    pump(2000);
    CHECK("the device plugged back in after a pull under a warm reset is enumerated",
          slot_of_kind(KIND_KBD) != 0 && running_count() == 2);

    /* SuperSpeed hubs that stall the descriptor request and SET_HUB_DEPTH. */
    model_reset();
    model.root_speed = USB_SPEED_SUPER;
    model.hub_protocol = 3;
    model.hub_desc_stall = 1;
    pump(2000);
    CHECK("a SuperSpeed hub that will not give its descriptor is closed",
          running_count() == 0 && model.hub_depth_sets == 0);
    model_reset();
    model.root_speed = USB_SPEED_SUPER;
    model.hub_protocol = 3;
    model.hub_depth_refuse = 1;
    pump(2000);
    CHECK("a SuperSpeed hub that refuses its depth is closed",
          running_count() == 0 && model.hub_depth_sets == 1 && model.hub_told == 0);

    /* A hub with twenty ports keeps all twenty. */
    model_reset();
    model.hub_ports = 20;
    pump(2000);
    CHECK("a hub with twenty ports is told as twenty", model.hub_told_ports == 20);

    /* A reset that never ends: the port is refused until its device leaves. */
    model_reset();
    model.port_kind[1] = KIND_KBD;
    model.port_reset_never[1] = 1;
    pump(3000);
    CHECK("a device whose port never leaves reset is not enumerated", slot_of_kind(KIND_KBD) == 0);
    {
        int i;
        int hub_index = -1;

        for (i = 0; i < USB_MAX_DEVICES; i++) {
            if (dev_at(i)->state == ST_RUNNING && dev_at(i)->interface.kind == USB_KIND_HUB) {
                hub_index = i;
            }
        }
        CHECK("and its port is remembered as refused on the hub",
              hub_index >= 0 && (dev_at(hub_index)->refused & 0x2u) != 0);
        pump(2000);
        CHECK("so it is not tried again", model.reset_requests == 1);
        usb_core_hub_port(hub_index, 1, 0);
        CHECK("until its device leaves",
              hub_index >= 0 && (dev_at(hub_index)->refused & 0x2u) == 0);
    }

    /* A disconnection closes the device below. */
    model_reset();
    model.port_kind[1] = KIND_KBD;
    pump(2000);
    kbd = slot_of_kind(KIND_KBD);
    CHECK("the keyboard is running", kbd != 0 && running_count() == 2);
    {
        int i;
        int hub_index = -1;

        for (i = 0; i < USB_MAX_DEVICES; i++) {
            if (dev_at(i)->state == ST_RUNNING && dev_at(i)->interface.kind == USB_KIND_HUB) {
                hub_index = i;
            }
        }
        model.port_kind[1] = KIND_NONE;
        usb_core_hub_port(hub_index, 1, 0);
        pump(1000);
        CHECK("a disconnection reported by the hub closes the device below",
              hid_gone_count == 1 && hid_gone_slot[0] == kbd && closes == 1 &&
                  close_order[0] == kbd);
        CHECK("and the hub stays", running_count() == 1);
    }

    halted_pulled();

    /* A hub pulled with two devices below: both children close before it. */
    model_reset();
    model.port_kind[1] = KIND_KBD;
    model.port_kind[2] = KIND_KBD;
    pump(3000);
    CHECK("two keyboards behind the hub are running", running_count() == 3);
    hub = slot_of_kind(KIND_HUB);
    model.root_connected = 0;
    pump(2000);
    CHECK("pulling the hub closes all three", closes == 3);
    CHECK("the two children before the hub",
          closes == 3 && close_order[0] != hub && close_order[1] != hub && close_order[2] == hub);
    CHECK("and nothing is left running", running_count() == 0);
    CHECK("every slot was given back and every entry freed",
          closed_done == 3 && free_count() == USB_MAX_DEVICES);

    /* The change endpoint, its transfer put out again after each answer. */
    model_reset();
    pump(2000);
    hub = slot_of_kind(KIND_HUB);
    CHECK("the change endpoint is read once the hub is up", model.change_submits == 1);
    model.port_kind[3] = KIND_KBD;
    model.port_change[3] |= 1u; /* C_PORT_CONNECTION */
    model_change(4);
    pump(2000);
    kbd = slot_of_kind(KIND_KBD);
    CHECK("a keyboard plugged into port 4 after the hub is up is enumerated",
          kbd != 0 && slot_of[kbd].where.route == 0x4u);
    CHECK("its connection change acknowledged with C_PORT_CONNECTION on port 4", cleared(16, 4));
    CHECK("and the change transfer put out again", model.change_submits == 2);
    model.port_kind[3] = KIND_NONE;
    model.port_change[3] |= 1u;
    model_change(4);
    pump(1000);
    CHECK("pulling it is reported and it goes", hid_gone_count == 1 && hid_gone_slot[0] == kbd);
    CHECK("with the transfer out a third time", model.change_submits == 3);

    /* The hub's own status on bit 0: read, and its change cleared. */
    model.hub_status = 0x0001; /* local power lost */
    model.hub_change = 0x0001;
    model_change(0);
    pump(500);
    CHECK("a hub-status change is read and its local-power change cleared",
          model.hub_status_reads == 1 && model.hub_clears == 1 && model.hub_change == 0);

    /* A device that lands on a port just after the scan read it. */
    model_reset();
    model.port_change[1] = 1u; /* C_PORT_CONNECTION, from a contact that bounced */
    model.connect_on_read = 2;
    pump(2000);
    kbd = slot_of_kind(KIND_KBD);
    CHECK("a device that connects during the scan, after its port was read, is enumerated",
          kbd != 0 && slot_of[kbd].where.route == 0x2u && running_count() == 2);

    /* The same, with the read after the acknowledgement answered short. */
    model_reset();
    model.port_change[1] = 1u;
    model.connect_on_read = 2;
    model.short_after_clear = 2;
    pump(2000);
    CHECK("a hub whose read after an acknowledgement comes back short is closed",
          cleared(16, 2) && closes == 1 && running_count() == 0 && slot_of_kind(KIND_KBD) == 0);

    /* A swap: a connection change with the port still connected. */
    model_reset();
    model.port_kind[1] = KIND_KBD;
    pump(2000);
    kbd = slot_of_kind(KIND_KBD);
    CHECK("a keyboard is running on port 2", kbd != 0);
    model.port_change[1] |= 1u;
    model_change(2);
    pump(2000);
    CHECK("a connection change with the device still there closes the old device",
          closes == 1 && close_order[0] == kbd);
    CHECK("before a new one is enumerated in its place",
          slots_given == 3 && slot_of[3].kind == KIND_KBD && slot_of[3].open);

    /* A port that loses its enable. */
    model_reset();
    model.port_kind[1] = KIND_KBD;
    pump(2000);
    kbd = slot_of_kind(KIND_KBD);
    model.port_enabled[1] = 0;
    model.port_change[1] |= 2u; /* C_PORT_ENABLE */
    model_change(2);
    pump(2000);
    CHECK("an enable lost closes the device below", closes >= 1 && close_order[0] == kbd);
    CHECK("and clears C_PORT_ENABLE", cleared(17, 2));

    /* Over-current: off, on again once after a second, off for good after. */
    model_reset();
    model.port_kind[1] = KIND_KBD;
    pump(2000);
    kbd = slot_of_kind(KIND_KBD);
    model.port_over_current[1] = 1;
    model.port_change[1] |= 8u; /* C_PORT_OVER_CURRENT */
    model_change(2);
    pump(500);
    CHECK("an over-current powers the port off", model.power_offs == 1);
    CHECK("and closes the device below", closes == 1 && close_order[0] == kbd);
    CHECK("and clears the over-current change", cleared(19, 2));
    CHECK("and does not power it on at once", model.power_ons == 0);
    model.port_over_current[1] = 0;
    pump(1500);
    CHECK("the port is powered on again after a second", model.power_ons == 1);
    model.port_over_current[1] = 1;
    model.port_change[1] |= 8u;
    model_change(2);
    pump(3000);
    CHECK("an over-current again leaves the port off",
          model.power_offs == 2 && model.power_ons == 1);

    /* A device pulled while its Enable Slot is in flight. */
    model_reset();
    pump(2000);
    model.pull_on_open = 1;
    model.port_kind[1] = KIND_KBD;
    model.port_change[1] |= 1u;
    model_change(2);
    pump(300);
    CHECK("the keyboard's Enable Slot is out and the keyboard is gone",
          model.hold_commands == 1 && model.port_kind[1] == KIND_NONE);
    model.hold_commands = 0;
    pump(1000);
    CHECK("the slot it answered with is given back", closed_done == 1 && closes == 1);
    CHECK("and its entry is free", free_count() == USB_MAX_DEVICES - 1);
    model.port_kind[2] = KIND_KBD;
    model.port_change[2] |= 1u;
    model_change(3);
    pump(2000);
    CHECK("and the next keyboard enumerates", running_count() == 2);

    /* A full table: the last keyboard, on a port above fifteen, waits. */
    model_reset();
    model.root_speed = USB_SPEED_HIGH;
    model.hub_protocol = 1;
    model.hub_ports = USB_MAX_DEVICES;
    for (port = 0; port < USB_MAX_DEVICES; port++) {
        model.port_kind[port] = KIND_KBD;
    }
    pump(8000);
    CHECK("a hub and the keyboards below it fill the table",
          running_count() == USB_MAX_DEVICES && free_count() == 0);
    CHECK("the one that found no entry is recorded once", full_records == 1);
    model.port_kind[0] = KIND_NONE;
    model.port_change[0] |= 1u;
    model_change(1);
    pump(4000);
    CHECK("when one is pulled, the one that waited takes its entry",
          running_count() == USB_MAX_DEVICES && slot_at_port(USB_MAX_DEVICES) != 0);

    /* The power-on after an over-current, stalled by the hub. */
    model_reset();
    model.port_kind[1] = KIND_KBD;
    pump(2000);
    model.power_on_refuse = 1;
    model.port_over_current[1] = 1;
    model.port_change[1] |= 8u;
    model_change(2);
    pump(500);
    CHECK("an over-current powers the port off", model.power_offs == 1);
    model.port_over_current[1] = 0;
    port = model.power_requests;
    kbd = model.reset_requests;
    pump(1500);
    CHECK("the power-on after a second is asked once and refused",
          model.power_requests == port + 1 && model.power_ons == 0);
    pump(3000);
    CHECK("and the port is left off", model.power_requests == port + 1 && model.power_ons == 0);
    CHECK("with nothing enumerated on it", running_count() == 1 && model.reset_requests == kbd);

    power_on_unqueued();

    /* A multi-TT hub's alternate setting 1, selected or stalled. */
    model_reset();
    model.root_speed = USB_SPEED_HIGH;
    model.hub_protocol = 2;
    pump(2000);
    CHECK("a multi-TT hub has its setting 1 selected", model.alt_set == 1);
    CHECK("and is told as multi-TT", model.hub_told_multi == 1);
    model_reset();
    model.root_speed = USB_SPEED_HIGH;
    model.hub_protocol = 2;
    model.alt_refuse = 1;
    pump(2000);
    CHECK("a hub that refuses the setting is told with one translator",
          model.hub_told && model.hub_told_multi == 0);
    model_reset();
    model.root_speed = USB_SPEED_HIGH;
    model.hub_protocol = 2;
    model.alt_refuse = 1;
    model.port_kind[2] = KIND_KBD;
    model.port_speed[2] = USB_SPEED_LOW;
    pump(2000);
    kbd = slot_of_kind(KIND_KBD);
    CHECK("and a low-speed device below it is opened with one translator too",
          kbd != 0 && slot_of[kbd].where.tt_multi == 0);

    /* An over-current already present at the first scan. */
    model_reset();
    model.port_kind[1] = KIND_KBD;
    model.port_over_current[1] = 1;
    pump(500);
    CHECK("an over-current at the first scan powers the port off",
          model.power_offs == 1 && slot_of_kind(KIND_KBD) == 0);
    model.port_over_current[1] = 0;
    pump(1500);
    CHECK("and the port is powered on again after a second", model.power_ons == 1);

    /* A stalled change endpoint closes the hub with its children. */
    model_reset();
    model.port_kind[1] = KIND_KBD;
    pump(2000);
    CHECK("the keyboard behind the hub is running", running_count() == 2);
    model.change_stall = 1;
    model.port_change[1] |= 1u;
    model_change(2);
    pump(2000);
    CHECK("a change endpoint that stalls closes the hub and what is below it",
          closes == 2 && running_count() == 0);

    /* A hub that stalls its descriptor request. */
    model_reset();
    model.hub_desc_stall = 1;
    pump(2000);
    CHECK("a hub that will not give its descriptor is closed", closes == 1 && running_count() == 0);

    /* Port twenty: the route nibble says fifteen, the translator says 20. */
    model_reset();
    model.root_speed = USB_SPEED_HIGH;
    model.hub_ports = 20;
    model.hub_protocol = 1; /* a single-translator high-speed hub */
    pump(2000);
    CHECK("a hub with twenty ports keeps them all", hub_at(0)->ports == 20);
    model.port_kind[19] = KIND_KBD;
    model.port_speed[19] = USB_SPEED_FULL;
    model.port_change[19] |= 1u;
    model_change(20);
    pump(3000);
    {
        int kbd = slot_of_kind(KIND_KBD);

        CHECK("a device on port twenty is enumerated", kbd != 0 && running_count() == 2);
        CHECK("with fifteen in the route string's nibble",
              kbd != 0 && slot_of[kbd].where.route == 0xFu);
        CHECK("and the translator carrying the port it is really on",
              kbd != 0 && slot_of[kbd].where.tt_port == 20);
    }

    /* A configuration of 115 bytes, then one that claims 5000. */
    model_reset();
    model.long_config = 1;
    model.port_kind[0] = KIND_KBD;
    pump(4000);
    CHECK("a device whose configuration is 115 bytes is enumerated", running_count() == 2);
    CHECK("with the interface found past the first sixty-four bytes", slot_of_kind(KIND_KBD) != 0);

    model_reset();
    model.long_config = 1;
    model.config_claims = 5000; /* longer than the record's buffer */
    model.port_kind[0] = KIND_KBD;
    pump(4000);
    CHECK("a configuration longer than this kernel reads is refused",
          running_count() == 1 && slot_of_kind(KIND_KBD) == 0);

    /* Every slot taken: the device waits for one and is not refused. */
    model_reset();
    pump(2000);
    CHECK("the hub is up with nothing below it", running_count() == 1);
    model.port_kind[0] = KIND_KBD;
    model.port_change[0] |= 1u;
    model_change(1);
    pump(3000);
    CHECK("a keyboard below it is enumerated", running_count() == 2);
    {
        int held = slots_given;

        /* The controller has no slot left from here. */
        model.slot_cap = held;
        model.port_kind[1] = KIND_KBD;
        model.port_change[1] |= 1u;
        model_change(2);
        pump(3000);
        CHECK("a device that finds no slot is not refused", running_count() == 2);
        CHECK("and it keeps its entry, waiting", free_count() == USB_MAX_DEVICES - 3);
        CHECK("with no slot taken for it", slots_given == held);

        /* One comes free. */
        model.slot_cap = held + 1;
        pump(3000);
        CHECK("and it is enumerated once a slot comes free",
              slots_given == held + 1 && running_count() == 3);
    }

    detached_record(0);
    detached_record(1);
    concurrent_changes();

    if (fails == 0) {
        printf("hub_hosttest: OK\n");
        return 0;
    }
    printf("hub_hosttest: %d failure(s)\n", fails);
    return 1;
}
