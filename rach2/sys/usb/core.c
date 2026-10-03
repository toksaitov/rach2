/*
 * core.c: from a connected port to a configured device.
 *
 * The standard half of the stack. It implements the requests every device
 * answers, the shape of a descriptor, and the choice of an interface this
 * kernel has a driver for. It reaches the wire through `hcd.h` and touches no
 * register.
 *
 * Everything here is a state machine, and the reason is in `hcd.h`: the
 * completions are drained from the timer's handler and the controller's
 * interrupt, where a wait would stop the clock. So no step of enumeration
 * waits for the one before it. Each call to `usb_core_step` moves every
 * device forward by one step, and a step that has not finished is looked at
 * again next time. An enumeration timeout starts cleanup. The device record
 * remains allocated until any outstanding slot-allocation command is resolved
 * and any returned slot is closed.
 *
 * One device is enumerated at a time on each controller, because a
 * controller has one command ring and this driver keeps one command in
 * flight on it. A second device on the same controller waits at its port.
 */

#include "core.h"

#include "../input.h"
#include "../klog.h"
#include "../lib/kstring.h"
#include "../lib/kirq.h"
#include "../mm/paging.h"
#include "hid.h"
#include "hub.h"
#include "msc.h"
#include "../block/usbdisk.h"
#include "xhci.h"

/* The standard requests this kernel sends, and the descriptor types it asks
 * for. Everything else a device offers is left alone. */
#define REQ_GET_DESCRIPTOR    6
#define REQ_SET_CONFIGURATION 9
/* The class request that puts a human-interface device in boot protocol,
 * whose two report layouts are fixed and need no describing. */
#define REQ_SET_PROTOCOL      11
#define TYPE_TO_INTERFACE     0x21
#define DESC_DEVICE           1
#define DESC_CONFIG           2

/* Descriptor types as they appear inside a configuration. */
#define DESC_INTERFACE 4
#define DESC_ENDPOINT  5

/* The endpoint descriptor's attribute and address fields. */
#define EP_ATTR_TYPE_MASK 0x3
#define EP_ATTR_BULK      2
#define EP_ATTR_INTERRUPT 3
#define EP_ADDR_IN        0x80

/* How long any one step of enumeration may take. A device that has not
 * answered by then is not going to. */
#define STEP_MS   500u
/* The configuration request alone gets five seconds: a stick's controller is
 * still starting its flash when the request arrives at boot, and the
 * notebook's stick behind the hub answered it after the half second every
 * other step gets. Five seconds is what the Linux driver allows it. */
#define CONFIG_MS 5000u

/* How long a connect is given to settle before the entry holding it is let go.
 * A contact that is still bouncing after this is a device being pushed in and
 * out, and the next scan will pick it up again. */
#define SETTLE_LIMIT_MS 5000u

/* How long the controller has to give a slot back. It is five seconds and
 * not one step, because the answer is a command like any other and a
 * controller that is busy is not a controller that has failed; past it the
 * entry is a tombstone. */
#define CLOSE_LIMIT_MS 5000u

/* Where a device is in its enumeration. */
#define ST_FREE       0
#define ST_SETTLING   1
#define ST_RESETTING  2
#define ST_SLOT       3
#define ST_ADDRESS    4
#define ST_DESC8      5
#define ST_PACKET     6
#define ST_DESC18     7
#define ST_CONFIG9    8
#define ST_CONFIGALL  9
#define ST_OPEN_EP    10
/* A stick has two endpoints and the controller takes one command each. */
#define ST_OPEN_EP2   18
#define ST_SET_CONFIG 11
#define ST_PROTOCOL   15
#define ST_RUNNING    12
#define ST_FAILED     13
#define ST_CLOSING    14
#define ST_HALTED     16
/* A device whose slot the controller never gave back. The entry is not free
 * and never will be: the controller may still write into the rings and the
 * context that slot names, so nothing of this device is reused. */
#define ST_TOMBSTONE  17
/* Every slot the controller has is taken. The device keeps its entry and its
 * reset, and asks again when one comes free. */
#define ST_SLOTWAIT   19

struct usb_device_state {
    int index; /* its entry in the table, which a frame's address does not give */
    int state;
    int port; /* on the root controller or on the hub, numbered from zero */
    int speed;
    int slot;
    /* Where the device is: its port's operations, the hub it is under or -1
     * on a root port, and the location the controller was given. */
    const struct usb_port_ops *ops;
    /* The controller this device is on. Every call about it goes to that
     * controller and to no other. */
    struct xhci_ctrl *hc;
    int owner;
    struct usb_location where;
    /* For a hub: the ports whose device this kernel could not drive, one bit
     * each, kept as the root's are, and the ports whose device is waiting
     * for a table entry, recorded once and tried on every pass. */
    uint32_t refused;
    uint32_t unentered;
    /* For a hub: whether it was told to translate per port, which its
     * descendants' translator words repeat; the interface's protocol says
     * what the hub offers, and this says what was accepted. */
    int hub_multi;
    uint32_t deadline;
    int closing_asked;
    /* Said once, so a device waiting for a slot does not say so every pass. */
    int waiting_said;
    /* A command whose result must be consumed before the device closes. */
    int abandoned;
    uint32_t short_by;
    int reporting;
    /* A quiesce in progress on this device's endpoint `quiesce_dci`: 1 with
     * the stop or the reset out, 2 with the dequeue out. */
    int quiesce_stage;
    int quiesce_dci;
    /* The completions waiting for this device, one for its control endpoint
     * and one for the endpoint it reports on. They are put here by the one
     * drain in `usb_core_step`: a device that polled the controller itself
     * would take the completions of every other device and drop them. */
    int have_control;
    int have_report;
    struct usb_completion control;
    struct usb_completion report_done;
    uint8_t report[8];
    uint8_t setup[8];
    uint8_t buffer[USB_DESC_MAX];
    uint32_t config_length;
    uint16_t vendor;
    uint16_t product;
    uint16_t release; /* bcdDevice */
    int ep0_packet;
    int ep_dci;
    int ep_out_dci; /* a stick's outward endpoint; zero on a device with one */
    struct usb_interface_info interface;
};

/*
 * One record per device, each its own frame of the DMA pool
 * (`alloc_kernel_frame`). The record has the buffers the controller writes
 * into, and a controller is given the physical address to write to, so the
 * record is one frame, its bytes contiguous and below 4 GiB. A record is
 * taken when a device appears and given back when its entry closes, so only
 * the devices present take memory; USB_MAX_DEVICES bounds how many there
 * are.
 */
static struct usb_device_state *devices[USB_MAX_DEVICES];

/* The record is a frame, so it must fit one. */
typedef char usb_record_fits_a_frame[(sizeof(struct usb_device_state) <= 4096) ? 1 : -1];

/** The record of entry `i`, or a spare one that is always free. */
static struct usb_device_state *dev_at(int i)
{
    static struct usb_device_state none;

    return devices[i] ? devices[i] : &none;
}

/**
 * Take a frame for entry `i`'s record, or say there is none.
 *
 * The frame is of the DMA pool, since the controller writes a device's
 * descriptors and its reports straight into this record and is given the
 * physical address to write to.
 */
static struct usb_device_state *dev_take(int i)
{
    void *frame;

    if (devices[i]) {
        return devices[i];
    }
    frame = alloc_kernel_frame();
    if (!frame) {
        return 0;
    }
    devices[i] = (struct usb_device_state *)frame;
    k_memset(devices[i], 0, sizeof(*devices[i]));
    devices[i]->index = i;
    return devices[i];
}

/** Give back entry `i`'s frame. */
static void dev_release(int i)
{
    if (devices[i]) {
        free_kernel_frame(devices[i]);
        devices[i] = 0;
    }
}

/* A root port's operations are its controller's; `owner` is not used. */
static int root_status(struct xhci_ctrl *hc, int owner, int port, struct usb_port_status *out)
{
    (void)owner;
    return xhci_port_status(hc, port, out);
}

static int root_settled(struct xhci_ctrl *hc, int owner, int port)
{
    (void)owner;
    return xhci_port_settled(hc, port);
}

static int root_reset_begin(struct xhci_ctrl *hc, int owner, int port)
{
    (void)owner;
    return xhci_port_reset_begin(hc, port);
}

static int root_reset_done(struct xhci_ctrl *hc, int owner, int port)
{
    (void)owner;
    return xhci_port_reset_done(hc, port);
}

static const struct usb_port_ops root_ops = {root_status, root_settled, root_reset_begin,
                                             root_reset_done};

static const struct usb_port_ops hub_ops = {hub_port_status, hub_port_settled, hub_port_reset_begin,
                                            hub_port_reset_done};

/* Which device is being enumerated on each controller, or -1. Only one is per
 * controller, because a controller takes one command at a time; two
 * controllers enumerate beside each other. */
static int enumerating[XHCI_MAX_CONTROLLERS];

/** Which controller's turn a device's enumeration takes. */
static int bus_of(const struct usb_device_state *d)
{
    int n = xhci_number(d->hc);

    return n >= 0 ? n : 0;
}

/*
 * The ports whose device this kernel could not drive, one bit each.
 *
 * A refusal has to be remembered or the device is enumerated again the moment
 * the port is looked at, which on a machine with a device of that kind is
 * every tick until the slots run out. It must not be remembered in the device
 * table: a notebook has several such devices soldered to it, and entries spent
 * on them are entries a person's keyboard cannot have. So the port is
 * remembered here, in a word, and the table stays for devices this kernel
 * drives.
 *
 * A bit is cleared when its port says the device has gone, which is where
 * unplugging one arrives, so a device that was refused once is tried again
 * when it is plugged in again.
 */
static uint32_t port_refused[XHCI_MAX_CONTROLLERS];
static uint32_t port_unentered[XHCI_MAX_CONTROLLERS]; /* ports waiting for a table entry */

/* This module's own clock, advanced with the driver's. */
static uint32_t core_ms;

/**
 * Select the first supported interface on alternate setting zero with all
 * required endpoints. Collect every selected endpoint from that interface;
 * discard incomplete candidates before examining the next interface.
 */
void usb_parse_config(const uint8_t *buffer, int length, struct usb_interface_info *out)
{
    int offset = 0;
    int in_boot_interface = 0;
    int in_storage_interface = 0;
    int in_hub_interface = 0;

    k_memset(out, 0, sizeof(*out));
    if (buffer == 0 || length < 9) {
        return;
    }
    out->config_value = buffer[5];
    if (out->config_value == 0) {
        return;
    }

    while (!out->found && offset + 2 <= length) {
        int size = buffer[offset];
        int type = buffer[offset + 1];

        if (size < 2 || offset + size > length) {
            break;
        }

        if (type == DESC_INTERFACE) {
            k_memset(out, 0, sizeof(*out));
            out->config_value = buffer[5];
            in_boot_interface = 0;
            in_storage_interface = 0;
            in_hub_interface = 0;
            if (size >= 9 && buffer[offset + 3] == 0) {
                in_boot_interface = (buffer[offset + 5] == USB_CLASS_HID &&
                                     buffer[offset + 6] == USB_SUBCLASS_BOOT &&
                                     (buffer[offset + 7] == USB_PROTOCOL_KBD ||
                                      buffer[offset + 7] == USB_PROTOCOL_MOUSE));
                in_storage_interface = (buffer[offset + 5] == USB_CLASS_STORAGE &&
                                        buffer[offset + 6] == USB_SUBCLASS_SCSI &&
                                        buffer[offset + 7] == USB_PROTOCOL_BULK_ONLY);
                in_hub_interface = (buffer[offset + 5] == USB_CLASS_HUB &&
                                    buffer[offset + 7] <= USB_PROTOCOL_HUB_SUPER);
                if (in_boot_interface || in_storage_interface || in_hub_interface) {
                    out->kind = in_boot_interface  ? USB_KIND_HID
                                : in_hub_interface ? USB_KIND_HUB
                                                   : USB_KIND_STORAGE;
                    out->number = buffer[offset + 2];
                    out->class_code = buffer[offset + 5];
                    out->subclass = buffer[offset + 6];
                    out->protocol = buffer[offset + 7];
                }
            }
        } else if (type == DESC_ENDPOINT && size >= 7 && (in_boot_interface || in_hub_interface)) {
            int address = buffer[offset + 2];
            int attributes = buffer[offset + 3];

            if ((address & EP_ADDR_IN) && (attributes & EP_ATTR_TYPE_MASK) == EP_ATTR_INTERRUPT &&
                (address & 0xF) != 0) {
                out->ep_number = address & 0xF;
                out->ep_max_packet = buffer[offset + 4] | (buffer[offset + 5] << 8);
                out->ep_interval = buffer[offset + 6];
                out->found = 1;
            }
        } else if (type == DESC_ENDPOINT && size >= 7 && in_storage_interface) {
            int address = buffer[offset + 2];
            int attributes = buffer[offset + 3];

            if ((attributes & EP_ATTR_TYPE_MASK) == EP_ATTR_BULK && (address & 0xF) != 0) {
                if ((address & EP_ADDR_IN) && out->ep_number == 0) {
                    out->ep_number = address & 0xF;
                    out->ep_max_packet = buffer[offset + 4] | (buffer[offset + 5] << 8);
                } else if (!(address & EP_ADDR_IN) && out->ep_out_number == 0) {
                    out->ep_out_number = address & 0xF;
                    out->ep_out_max_packet = buffer[offset + 4] | (buffer[offset + 5] << 8);
                }
                if (out->ep_number != 0 && out->ep_out_number != 0) {
                    out->found = 1;
                }
            }
        }

        offset += size;
    }
}

/** Fill a setup packet. */
static void setup_fill(uint8_t *setup, int type, int request, int value, int index, int length)
{
    setup[0] = (uint8_t)type;
    setup[1] = (uint8_t)request;
    setup[2] = (uint8_t)(value & 0xFF);
    setup[3] = (uint8_t)((value >> 8) & 0xFF);
    setup[4] = (uint8_t)(index & 0xFF);
    setup[5] = (uint8_t)((index >> 8) & 0xFF);
    setup[6] = (uint8_t)(length & 0xFF);
    setup[7] = (uint8_t)((length >> 8) & 0xFF);
}

/** Ask a device for something on its control endpoint. */
static int control_in(struct usb_device_state *d, int request, int value, int index, int length)
{
    struct usb_transfer transfer;

    setup_fill(d->setup, 0x80, request, value, index, length);
    transfer.setup = d->setup;
    transfer.buffer = d->buffer;
    transfer.length = (uint32_t)length;
    transfer.direction = USB_DIR_IN;
    return xhci_submit(d->hc, d->slot, 1, &transfer);
}

/** Tell a device something on its control endpoint. */
static int control_out(struct usb_device_state *d, int type, int request, int value, int index)
{
    struct usb_transfer transfer;

    setup_fill(d->setup, type, request, value, index, 0);
    transfer.setup = d->setup;
    transfer.buffer = 0;
    transfer.length = 0;
    transfer.direction = USB_DIR_OUT;
    return xhci_submit(d->hc, d->slot, 1, &transfer);
}

/** Where the next step's deadline falls. */
static void arm(struct usb_device_state *d)
{
    d->deadline = core_ms + STEP_MS;
}

static int expired(const struct usb_device_state *d)
{
    return (int32_t)(core_ms - d->deadline) >= 0;
}

static void subtree_gone(int hub);

/**
 * A device's place as a path, `5` for root port 5 and `5.2` for port 2 of a
 * hub on it, in a buffer that the next call overwrites: it is for one record.
 */
static const char *path_of(const struct usb_device_state *d)
{
    static char text[USB_TREE_TIERS * 3 + 4];
    int chain[USB_TREE_TIERS + 1];
    int depth = 0;
    int at = 0;
    int k;

    while (d != 0 && depth <= USB_TREE_TIERS) {
        chain[depth++] = d->port + 1;
        d = d->owner >= 0 ? dev_at(d->owner) : 0;
    }
    for (k = depth - 1; k >= 0; k--) {
        int n = chain[k];

        if (n >= 10) {
            text[at++] = (char)('0' + n / 10);
        }
        text[at++] = (char)('0' + n % 10);
        if (k > 0) {
            text[at++] = '.';
        }
    }
    text[at] = '\0';
    return text;
}

/**
 * Take one device down: its class driver told, and its entry closing if the
 * controller gave it a slot, free otherwise.
 *
 * A device taken down during its port reset gives up its turn at once: the
 * reset is the hub's or the controller's port and not a command, so no
 * answer is owed, and a wait for one would hold every later device's
 * enumeration. One taken down with the command turn keeps it until the
 * command that may be in flight has answered or been given up on;
 * `step_closing` takes it.
 */
static void device_gone(int i)
{
    struct usb_device_state *d = dev_at(i);

    if (d->state == ST_FREE || d->state == ST_CLOSING || d->state == ST_TOMBSTONE) {
        return;
    }
    /* Whatever is below a hub goes before the hub does. */
    subtree_gone(i);
    if (d->state == ST_RUNNING) {
        klog(KLOG_INFO, "usb", "device: port %s: gone", path_of(d));
        if (d->interface.kind == USB_KIND_STORAGE) {
            msc_detach(d->hc, d->slot);
        } else if (d->interface.kind == USB_KIND_HUB) {
            hub_detach(i);
        } else {
            hid_gone(d->index + 1);
        }
    }
    d->have_control = 0;
    d->have_report = 0;
    d->closing_asked = 0;
    if (enumerating[bus_of(d)] == i && d->state == ST_RESETTING) {
        enumerating[bus_of(d)] = -1;
    } else if (enumerating[bus_of(d)] == i) {
        d->abandoned = 1;
        d->state = ST_CLOSING;
        d->deadline = core_ms + CLOSE_LIMIT_MS;
        return;
    }
    d->state = d->slot > 0 ? ST_CLOSING : ST_FREE;
    d->deadline = core_ms + STEP_MS;
}

/** Every device under hub `hub`, depth-first: children before their hub. */
static void subtree_gone(int hub)
{
    int i;

    for (i = 0; i < USB_MAX_DEVICES; i++) {
        if (dev_at(i)->owner == hub && dev_at(i)->state != ST_FREE) {
            device_gone(i);
        }
    }
}

/**
 * Where a device is, from where its hub is.
 *
 * A root port's device has depth zero and an empty route. A device on port
 * P of a hub has the hub's route with P, numbered from one, in the nibble of
 * its tier, and a depth one more; a sixth tier is refused. A low or
 * full-speed device below a high-speed hub has the nearest high-speed hub
 * above it as its translator, with the port on that hub that the path to it
 * goes through, which is the specification's rule for a full-speed hub
 * between them as well.
 *
 * A port above 15 does not fit its nibble and is written as 15. The
 * specification allows that only where the translator's port number says
 * which port it is, which is a full- or low-speed device directly below its
 * high-speed translator; anywhere else the route string is all the
 * controller has, and such a port is refused.
 */
static int location_of(const struct usb_device_state *d, struct usb_location *out)
{
    out->tt_device = 0;
    out->tt_port = 0;
    out->tt_multi = 0;
    if (d->owner < 0) {
        out->root_port = d->port;
        out->depth = 0;
        out->route = 0;
        return 0;
    }
    {
        const struct usb_device_state *hub = dev_at(d->owner);
        int tier = hub->where.depth;
        uint32_t nibble = (uint32_t)(d->port + 1);

        if (d->port + 1 > USB_HUB_PORT_MAX || tier >= USB_TREE_TIERS) {
            return -1;
        }
        out->root_port = hub->where.root_port;
        out->depth = tier + 1;
        if (d->speed == USB_SPEED_LOW || d->speed == USB_SPEED_FULL) {
            const struct usb_device_state *at = d;

            while (at->owner >= 0) {
                const struct usb_device_state *above = dev_at(at->owner);

                if (above->speed == USB_SPEED_HIGH) {
                    out->tt_device = above->slot;
                    out->tt_port = at->port + 1;
                    out->tt_multi = above->hub_multi;
                    break;
                }
                at = above;
            }
        }
        if (nibble > 15u) {
            if (out->tt_device == 0 || out->tt_device != hub->slot ||
                (uint32_t)out->tt_port != nibble) {
                return -1;
            }
            nibble = 15u;
        }
        out->route = hub->where.route | (nibble << (4 * tier));
    }
    return 0;
}

/** Whether anything below hub `hub` is still on its way out. */
static int subtree_closing(int hub)
{
    int i;

    for (i = 0; i < USB_MAX_DEVICES; i++) {
        if (dev_at(i)->owner == hub && dev_at(i)->state != ST_FREE &&
            dev_at(i)->state != ST_TOMBSTONE) {
            return 1;
        }
    }
    return 0;
}

/**
 * Fail a device and close its slot, retaining any outstanding command.
 *
 * Cleanup consumes the command's result before releasing its turn or the
 * device record, so a late Enable Slot result can be closed. The port, not
 * the entry, is remembered as refused, a hub's port on the hub's own record,
 * so devices this kernel cannot drive do not spend the table that a device a
 * person plugs in needs.
 */
static void fail(struct usb_device_state *d, const char *why)
{
    klog(KLOG_WARN, "usb", "device: port %s: %s", path_of(d), why);
    if (xhci_interrupts_on(d->hc)) {
        xhci_state_record(d->hc); /* once, at the first failure after bring-up */
    }
    if (enumerating[bus_of(d)] == d->index) {
        d->abandoned = !xhci_command_free(d->hc);
        if (!d->abandoned) {
            enumerating[bus_of(d)] = -1;
        }
    }
    if (d->interface.kind == USB_KIND_STORAGE) {
        msc_detach(d->hc, d->slot);
    }
    if (d->interface.kind == USB_KIND_HUB) {
        hub_detach(d->index);
        subtree_gone(d->index);
    }
    hid_gone(d->index + 1);
    if (d->port >= 0 && d->port < 32) {
        if (d->owner >= 0) {
            dev_at(d->owner)->refused |= (uint32_t)1u << d->port;
        } else {
            port_refused[bus_of(d)] |= (uint32_t)1u << d->port;
        }
    }
    if (d->slot > 0 || d->abandoned) {
        d->state = ST_CLOSING;
        d->deadline = core_ms + STEP_MS;
        return;
    }
    d->state = ST_FREE;
}

/**
 * A device this kernel has no driver for: an ordinary note, and the same
 * teardown a failure gets.
 *
 * A laptop has a camera, a card reader and a fingerprint sensor on its
 * internal hub, and none of them is a fault; reporting each as one, at
 * warning level with a dump of the controller's registers, would make a
 * normal boot look broken. The port is still remembered so the table is not
 * spent on devices that will never be driven.
 */
static void unsupported(struct usb_device_state *d)
{
    klog(KLOG_INFO, "usb", "device: port %s: %x:%x, class %d: no driver for it here", path_of(d),
         (int)d->vendor, (int)d->product, (int)d->interface.class_code);
    if (enumerating[bus_of(d)] == d->index) {
        enumerating[bus_of(d)] = -1;
    }
    hid_gone(d->index + 1);
    if (d->port >= 0 && d->port < 32) {
        if (d->owner >= 0) {
            dev_at(d->owner)->refused |= (uint32_t)1u << d->port;
        } else {
            port_refused[bus_of(d)] |= (uint32_t)1u << d->port;
        }
    }
    if (d->slot > 0) {
        d->state = ST_CLOSING;
        d->deadline = core_ms + STEP_MS;
        return;
    }
    d->state = ST_FREE;
}

/**
 * Take a device down: ask for its slot back, and let the entry go when the
 * controller has given it.
 *
 * A device that has been unplugged is gone from the bus and not from the
 * controller: its slot, its context and its rings are still the controller's
 * until it is told. Freeing the entry first is how a slot ends up doing work
 * for two devices at once.
 *
 * Closing takes the same turn enumeration does. There is one command in
 * flight and one answer to it, and whichever device asks first takes that
 * answer: a device waiting for its slot would otherwise be handed the answer
 * to another device's Disable Slot, and would take the slot being given back
 * as the slot it had just been given. A hub waits for everything below it.
 *
 * A Disable Slot that fails or is never answered, or one that cannot be asked
 * because the command ring never came back from an abort, leaves the entry a
 * tombstone: the slot, its rings and the pages its transfers name may still
 * be written by the controller, so nothing of it is reused and this kernel is
 * one device poorer until the machine restarts. Once a stick's slot is given
 * back its driver is told, since the controller reaches nothing of it any
 * more and its request in flight can be completed.
 */
static void step_closing(struct usb_device_state *d)
{
    struct usb_command_result result;
    int index = d->index;

    if (d->interface.kind == USB_KIND_HUB && subtree_closing(index)) {
        return; /* children before their hub */
    }
    if (enumerating[bus_of(d)] >= 0 && enumerating[bus_of(d)] != index) {
        return;
    }
    if (enumerating[bus_of(d)] != index && !d->abandoned && !xhci_command_free(d->hc)) {
        return; /* the Disable Slot waits for the ring, not for a deadline */
    }
    enumerating[bus_of(d)] = index;

    if (d->abandoned) {
        int done = xhci_command_done(d->hc, &result);

        if (done == 0) {
            return; /* the controller gives up on it at its own deadline */
        }
        if (done == 1 && d->slot == 0 && result.device > 0) {
            d->slot = result.device;
        }
        d->abandoned = 0;
        d->deadline = core_ms + CLOSE_LIMIT_MS;
    }
    if (d->slot <= 0) {
        d->state = ST_FREE;
        enumerating[bus_of(d)] = -1;
        return;
    }
    if (d->closing_asked) {
        int done = xhci_command_done(d->hc, &result);

        if (done == 0 && !expired(d)) {
            return;
        }
        if (done != 1 || result.status != USB_XFER_OK) {
            klog(KLOG_WARN, "usb", "slot: %d was not given back; the entry is lost", d->slot);
            enumerating[bus_of(d)] = -1;
            d->state = ST_TOMBSTONE;
            return;
        }
        klog(KLOG_DEBUG, "usb", "slot: %d: given back", d->slot);
        if (d->interface.kind == USB_KIND_STORAGE) {
            msc_slot_closed(d->hc, d->slot);
        }
        xhci_device_closed(d->hc, d->slot);
        d->slot = 0;
        enumerating[bus_of(d)] = -1;
        d->state = ST_FREE;
        return;
    }
    if (xhci_device_close(d->hc, d->slot) == 0) {
        d->closing_asked = 1;
        d->deadline = core_ms + CLOSE_LIMIT_MS;
    } else if (expired(d)) {
        klog(KLOG_WARN, "usb", "slot: %d could not be asked back; the entry is lost", d->slot);
        enumerating[bus_of(d)] = -1;
        d->state = ST_TOMBSTONE;
    }
}

/**
 * The transfer this device is waiting for: 1 when it finished well, 0 when it
 * has not finished, -1 when it failed.
 *
 * A short answer is legal for a device and counts as finished, but a reader
 * that parsed the buffer as if it were whole would misread it, so how much is
 * missing is kept in `short_by` and each step decides.
 */
static int transfer_result(struct usb_device_state *d)
{
    struct usb_completion completion;

    if (d->have_control) {
        completion = d->control;
        d->have_control = 0;
        if (completion.status == USB_XFER_OK) {
            d->short_by = 0;
            return 1;
        }
        if (completion.status == USB_XFER_SHORT) {
            d->short_by = completion.residual;
            return 1;
        }
        return -1;
    }
    return expired(d) ? -1 : 0;
}

/**
 * Why the control transfer `transfer_result` just failed, for a record.
 *
 * The completion is still in `d->control` after the result was taken; a
 * device whose deadline passed has none.
 */
static const char *transfer_why(const struct usb_device_state *d)
{
    if (expired(d)) {
        return "no answer within the step's time";
    }
    if (d->control.status == USB_XFER_STALLED) {
        return "it stalled the request";
    }
    return "the transfer failed";
}

/** The command this device is waiting for, with the same three answers. */
static int command_result(struct usb_device_state *d, struct usb_command_result *out)
{
    int done = xhci_command_done(d->hc, out);

    if (done == 1) {
        return 1;
    }
    if (done < 0) {
        return -1;
    }
    return expired(d) ? -1 : 0;
}

/**
 * A device that is configured and reporting.
 *
 * One transfer is outstanding at a time: it is queued, and when the controller
 * says it finished the report is handed to the class driver and another is
 * queued. A device that says nothing leaves its transfer outstanding for as
 * long as nobody touches it, which is what an interrupt endpoint is for.
 *
 * A stick answers only when asked, so it has no transfer kept outstanding
 * here; its own driver steps it. A stall halts a device's endpoint and any
 * other failure leaves it halted or in the Error state; nothing queued on it
 * moves until it has been quiesced on the core's turn, which ST_HALTED waits
 * for.
 */
static void step_running(struct usb_device_state *d)
{
    int kind;
    int length;

    if (d->interface.kind == USB_KIND_HUB) {
        return; /* the hub driver reads its own change endpoint */
    }

    if (d->interface.kind == USB_KIND_STORAGE) {
        return;
    }
    kind = d->interface.protocol == USB_PROTOCOL_KBD ? HID_KEYBOARD : HID_MOUSE;
    length = d->interface.ep_max_packet;

    if (length > (int)sizeof(d->report)) {
        length = (int)sizeof(d->report);
    }

    if (d->have_report) {
        struct usb_completion completion = d->report_done;

        d->have_report = 0;
        d->reporting = 0;
        if (completion.status != USB_XFER_OK && completion.status != USB_XFER_SHORT) {
            d->state = ST_HALTED;
            arm(d);
            return;
        } else {
            hid_report(d->index + 1, kind, d->report,
                       (int)(completion.length - completion.residual));
        }
    }

    if (!d->reporting) {
        struct usb_transfer transfer;

        transfer.setup = 0;
        transfer.buffer = d->report;
        transfer.length = (uint32_t)length;
        transfer.direction = USB_DIR_IN;
        if (xhci_submit(d->hc, d->slot, d->ep_dci, &transfer) == 0) {
            d->reporting = 1;
        }
    }
}

/**
 * One step of one device.
 *
 * A connect that does not settle within SETTLE_LIMIT_MS, a port that keeps
 * bouncing or a device pulled out again, frees the entry. A slot, an address,
 * a packet size or an endpoint that cannot be asked for because another
 * device's command is in flight is a turn to wait and not a refusal, until
 * the deadline armed at the step before. When every slot is taken, which
 * `xhci_command_done` reports as a failure with the controller's own code,
 * the device keeps its entry and its reset and asks again every STEP_MS: a
 * slot comes free when another device closes.
 *
 * The configuration is read whole or not at all, since one walked only as
 * far as a buffer reached would drive a device by half its description; an
 * answer that came back short is parsed only as far as it goes. Each
 * endpoint is opened, and so has bandwidth, before the device is told to use
 * the configuration it belongs to, the order the controller requires; a
 * stick's second endpoint is a command, and so a state, of its own. Then a
 * stick goes to its class driver and a hub to the hub driver, a SuperSpeed
 * hub known by its speed since the specification puts its protocol 3 in the
 * device descriptor and leaves the interface's at zero; a keyboard or mouse
 * is put in boot protocol, and fails when that request does.
 */
static void step_device(struct usb_device_state *d)
{
    struct usb_command_result result;
    int index = d->index;
    int answer;

    switch (d->state) {
    case ST_CLOSING:
        step_closing(d);
        return;

    case ST_HALTED:
        answer = usb_core_endpoint_quiesce(d->hc, d->slot, d->ep_dci);
        if (answer == 0) {
            return;
        }
        if (answer < 0) {
            fail(d, "its halted endpoint would not reset");
            return;
        }
        d->state = ST_RUNNING;
        d->reporting = 0;
        return;

    case ST_SETTLING:
        if (!d->ops->settled(d->hc, d->owner, d->port)) {
            if (expired(d)) {
                d->state = ST_FREE;
            }
            return;
        }
        if (enumerating[bus_of(d)] >= 0 && enumerating[bus_of(d)] != index) {
            return; /* another device has the command ring */
        }
        if (!xhci_command_free(d->hc)) {
            return; /* a command another device left is still on it */
        }
        enumerating[bus_of(d)] = index;
        if (d->ops->reset_begin(d->hc, d->owner, d->port) != 0) {
            fail(d, "the port would not begin a reset");
            return;
        }
        d->state = ST_RESETTING;
        arm(d);
        return;

    case ST_RESETTING:
        answer = d->ops->reset_done(d->hc, d->owner, d->port);
        if (answer == 0) {
            return;
        }
        if (answer < 0) {
            fail(d, "the port never came out of reset");
            return;
        }
        {
            struct usb_port_status status;

            if (d->ops->status(d->hc, d->owner, d->port, &status) != 0 || !status.connected) {
                fail(d, "the device left during its reset");
                return;
            }
            d->speed = status.speed;
        }
        {
            if (location_of(d, &d->where) != 0) {
                fail(d, "it is deeper in the tree than a route string can say");
                return;
            }
            if (xhci_device_open(d->hc, &d->where, d->speed) != 0) {
                if (!expired(d)) {
                    return;
                }
                fail(d, "no slot could be asked for");
                return;
            }
        }
        d->state = ST_SLOT;
        arm(d);
        return;

    case ST_SLOTWAIT:
        if (!expired(d)) {
            return;
        }
        if (enumerating[bus_of(d)] >= 0 && enumerating[bus_of(d)] != index) {
            d->deadline = core_ms + STEP_MS;
            return;
        }
        enumerating[bus_of(d)] = index;
        if (xhci_device_open(d->hc, &d->where, d->speed) != 0) {
            d->deadline = core_ms + STEP_MS;
            return;
        }
        d->state = ST_SLOT;
        arm(d);
        return;

    case ST_SLOT:
        result.code = 0;
        answer = command_result(d, &result);
        if (answer == 0) {
            return;
        }
        if (answer < 0 && result.code == USB_NO_SLOTS) {
            if (!d->waiting_said) {
                klog(KLOG_INFO, "usb", "device: port %s: waiting for a slot", path_of(d));
                d->waiting_said = 1;
            }
            enumerating[bus_of(d)] = -1;
            d->state = ST_SLOTWAIT;
            d->deadline = core_ms + STEP_MS;
            return;
        }
        if (answer < 0 || result.device <= 0) {
            fail(d, "the controller gave out no slot");
            return;
        }
        d->slot = result.device;
        if (xhci_device_address(d->hc, d->slot) != 0) {
            if (!expired(d)) {
                return;
            }
            fail(d, "the device could not be addressed");
            return;
        }
        d->state = ST_ADDRESS;
        arm(d);
        return;

    case ST_ADDRESS:
        answer = command_result(d, &result);
        if (answer == 0) {
            return;
        }
        if (answer < 0) {
            fail(d, "the controller would not address the device");
            return;
        }
        if (control_in(d, REQ_GET_DESCRIPTOR, DESC_DEVICE << 8, 0, 8) != 0) {
            fail(d, "its first descriptor could not be asked for");
            return;
        }
        d->state = ST_DESC8;
        arm(d);
        return;

    case ST_DESC8:
        answer = transfer_result(d);
        if (answer == 0) {
            return;
        }
        if (answer < 0) {
            fail(d, "it did not answer for its descriptor");
            return;
        }
        if (d->short_by != 0) {
            fail(d, "its first descriptor came back short");
            return;
        }
        /* Byte 7 of the device descriptor is bMaxPacketSize0. */
        d->ep0_packet = d->buffer[7];
        if (d->ep0_packet != 8 && d->speed != USB_SPEED_HIGH && d->speed != USB_SPEED_SUPER) {
            if (xhci_device_packet_size(d->hc, d->slot, d->ep0_packet) != 0) {
                if (!expired(d)) {
                    return;
                }
                fail(d, "its control endpoint could not be corrected");
                return;
            }
            d->state = ST_PACKET;
            arm(d);
            return;
        }
        if (control_in(d, REQ_GET_DESCRIPTOR, DESC_DEVICE << 8, 0, 18) != 0) {
            fail(d, "its descriptor could not be asked for");
            return;
        }
        d->state = ST_DESC18;
        arm(d);
        return;

    case ST_PACKET:
        answer = command_result(d, &result);
        if (answer == 0) {
            return;
        }
        if (answer < 0) {
            fail(d, "the controller would not take the corrected packet size");
            return;
        }
        if (control_in(d, REQ_GET_DESCRIPTOR, DESC_DEVICE << 8, 0, 18) != 0) {
            fail(d, "its descriptor could not be asked for");
            return;
        }
        d->state = ST_DESC18;
        arm(d);
        return;

    case ST_DESC18:
        answer = transfer_result(d);
        if (answer == 0) {
            return;
        }
        if (answer < 0) {
            fail(d, "it did not answer for its descriptor");
            return;
        }
        if (d->short_by != 0) {
            fail(d, "its descriptor came back short");
            return;
        }
        d->vendor = (uint16_t)(d->buffer[8] | (d->buffer[9] << 8));
        d->product = (uint16_t)(d->buffer[10] | (d->buffer[11] << 8));
        d->release = (uint16_t)(d->buffer[12] | (d->buffer[13] << 8));
        if (control_in(d, REQ_GET_DESCRIPTOR, DESC_CONFIG << 8, 0, 9) != 0) {
            fail(d, "its configuration could not be asked for");
            return;
        }
        d->state = ST_CONFIG9;
        arm(d);
        return;

    case ST_CONFIG9:
        answer = transfer_result(d);
        if (answer == 0) {
            return;
        }
        if (answer < 0) {
            fail(d, "it did not answer for its configuration");
            return;
        }
        if (d->short_by != 0) {
            fail(d, "its configuration header came back short");
            return;
        }
        d->config_length = (uint32_t)(d->buffer[2] | (d->buffer[3] << 8));
        if (d->config_length < 9 || d->config_length > USB_DESC_MAX) {
            fail(d, "its configuration is longer than this kernel reads, or too short to be one");
            return;
        }
        if (control_in(d, REQ_GET_DESCRIPTOR, DESC_CONFIG << 8, 0, (int)d->config_length) != 0) {
            fail(d, "its configuration could not be read");
            return;
        }
        d->state = ST_CONFIGALL;
        arm(d);
        return;

    case ST_CONFIGALL:
        answer = transfer_result(d);
        if (answer == 0) {
            return;
        }
        if (answer < 0) {
            fail(d, "it did not answer for its configuration");
            return;
        }
        usb_parse_config(d->buffer, (int)(d->config_length - d->short_by), &d->interface);
        if (!d->interface.found) {
            unsupported(d);
            return;
        }
        {
            struct usb_endpoint endpoint;

            endpoint.number = d->interface.ep_number;
            endpoint.direction = USB_DIR_IN;
            endpoint.type =
                (d->interface.kind == USB_KIND_STORAGE) ? USB_EP_BULK : USB_EP_INTERRUPT;
            endpoint.max_packet = d->interface.ep_max_packet;
            endpoint.interval = d->interface.ep_interval;
            d->ep_dci = d->interface.ep_number * 2 + 1;
            d->ep_out_dci = 0;
            if (xhci_endpoint_open(d->hc, d->slot, &endpoint) != 0) {
                if (!expired(d)) {
                    return;
                }
                fail(d, "its endpoint could not be opened");
                return;
            }
        }
        d->state = ST_OPEN_EP;
        arm(d);
        return;

    case ST_OPEN_EP:
        answer = command_result(d, &result);
        if (answer == 0) {
            return;
        }
        if (answer < 0) {
            fail(d, "the controller would not configure its endpoint");
            return;
        }
        if (d->interface.kind == USB_KIND_STORAGE) {
            struct usb_endpoint endpoint;

            endpoint.number = d->interface.ep_out_number;
            endpoint.direction = USB_DIR_OUT;
            endpoint.type = USB_EP_BULK;
            endpoint.max_packet = d->interface.ep_out_max_packet;
            endpoint.interval = 0;
            d->ep_out_dci = d->interface.ep_out_number * 2;
            if (xhci_endpoint_open(d->hc, d->slot, &endpoint) != 0) {
                if (!expired(d)) {
                    return;
                }
                fail(d, "its second endpoint could not be opened");
                return;
            }
            d->state = ST_OPEN_EP2;
            arm(d);
            return;
        }
        if (control_out(d, 0x00, REQ_SET_CONFIGURATION, d->interface.config_value, 0) != 0) {
            fail(d, "it could not be told its configuration");
            return;
        }
        d->state = ST_SET_CONFIG;
        d->deadline = core_ms + CONFIG_MS;
        return;

    case ST_OPEN_EP2:
        answer = command_result(d, &result);
        if (answer == 0) {
            return;
        }
        if (answer < 0) {
            fail(d, "the controller would not configure its second endpoint");
            return;
        }
        if (control_out(d, 0x00, REQ_SET_CONFIGURATION, d->interface.config_value, 0) != 0) {
            fail(d, "it could not be told its configuration");
            return;
        }
        d->state = ST_SET_CONFIG;
        d->deadline = core_ms + CONFIG_MS;
        return;

    case ST_SET_CONFIG:
        answer = transfer_result(d);
        if (answer == 0) {
            return;
        }
        if (answer < 0) {
            klog(KLOG_WARN, "usb", "device: port %s: configuration: %s", path_of(d),
                 transfer_why(d));
            fail(d, "it refused its configuration");
            return;
        }
        if (d->interface.kind == USB_KIND_STORAGE) {
            if (msc_attach(d->hc, d->slot, d->interface.number, d->ep_dci, d->ep_out_dci) != 0) {
                fail(d, "this kernel drives no more sticks at once");
                return;
            }
            d->state = ST_RUNNING;
            d->reporting = 0;
            enumerating[bus_of(d)] = -1;
            klog(KLOG_INFO, "usb",
                 "device: port %s: %x:%x, class %d protocol %d, endpoints %d and %d", path_of(d),
                 (int)d->vendor, (int)d->product, d->interface.class_code, d->interface.protocol,
                 d->interface.ep_number, d->interface.ep_out_number);
            return;
        }
        if (d->interface.kind == USB_KIND_HUB) {
            if (hub_attach(d->hc, d->index, d->slot, d->interface.number, d->interface.protocol,
                           d->ep_dci, d->where.depth, d->speed == USB_SPEED_SUPER) != 0) {
                fail(d, "this kernel drives no more hubs at once");
                return;
            }
            d->state = ST_RUNNING;
            d->reporting = 0;
            d->refused = 0;
            d->unentered = 0;
            d->hub_multi = 0;
            enumerating[bus_of(d)] = -1;
            klog(KLOG_INFO, "usb",
                 "device: port %s: %x:%x, class %d protocol %d, endpoint %d, a hub", path_of(d),
                 (int)d->vendor, (int)d->product, d->interface.class_code, d->interface.protocol,
                 d->interface.ep_number);
            return;
        }
        if (control_out(d, TYPE_TO_INTERFACE, REQ_SET_PROTOCOL, 0, d->interface.number) != 0) {
            fail(d, "it could not be put in boot protocol");
            return;
        }
        d->state = ST_PROTOCOL;
        arm(d);
        return;

    case ST_PROTOCOL:
        answer = transfer_result(d);
        if (answer == 0) {
            return;
        }
        if (answer < 0) {
            fail(d, "it refused the boot protocol");
            return;
        }
        d->state = ST_RUNNING;
        d->reporting = 0;
        enumerating[bus_of(d)] = -1;
        if (d->interface.protocol == USB_PROTOCOL_MOUSE) {
            /* `/dev/mouse` opens once a mouse of either kind is found. */
            input_mouse_found();
        }
        klog(KLOG_INFO, "usb", "device: port %s: %x:%x, class %d protocol %d, endpoint %d",
             path_of(d), (int)d->vendor, (int)d->product, d->interface.class_code,
             d->interface.protocol, d->interface.ep_number);
        return;

    default:
        return;
    }
}

/**
 * Give a device that has appeared on a port its entry, if one is free.
 *
 * `hc` is the controller the port is on: a root port's own, or the one the
 * hub that owns the port is on, so a device is always recorded against the
 * controller that will drive it.
 */
static void device_arrive(struct xhci_ctrl *hc, int owner, int port)
{
    int i;

    for (i = 0; i < USB_MAX_DEVICES; i++) {
        if (dev_at(i)->state == ST_FREE) {
            if (!dev_take(i)) {
                klog(KLOG_WARN, "usb", "device: port %d: refused: no frame for its record",
                     port + 1);
                return;
            }
            dev_at(i)->state = ST_SETTLING;
            dev_at(i)->hc = hc;
            dev_at(i)->owner = owner;
            dev_at(i)->ops = owner < 0 ? &root_ops : &hub_ops;
            dev_at(i)->port = port;
            dev_at(i)->slot = 0;
            dev_at(i)->ep_dci = 0;
            dev_at(i)->closing_asked = 0;
            dev_at(i)->abandoned = 0;
            dev_at(i)->have_control = 0;
            dev_at(i)->have_report = 0;
            dev_at(i)->refused = 0;
            dev_at(i)->unentered = 0;
            dev_at(i)->interface.found = 0;
            dev_at(i)->interface.kind = USB_KIND_NONE;
            dev_at(i)->deadline = core_ms + SETTLE_LIMIT_MS;
            if (owner >= 0) {
                dev_at(owner)->unentered &= ~((uint32_t)1u << port);
            } else {
                port_unentered[xhci_number(hc)] &= ~((uint32_t)1u << port);
            }
            return;
        }
    }
    /* The port is tried again on every pass; the record is made once. */
    if (owner >= 0) {
        if (!(dev_at(owner)->unentered & ((uint32_t)1u << port))) {
            dev_at(owner)->unentered |= (uint32_t)1u << port;
            klog(KLOG_WARN, "usb", "device: port %s.%d: refused: the device table is full",
                 path_of(dev_at(owner)), port + 1);
        }
    } else if (!(port_unentered[xhci_number(hc)] & ((uint32_t)1u << port))) {
        port_unentered[xhci_number(hc)] |= (uint32_t)1u << port;
        klog(KLOG_WARN, "usb", "device: port %d: refused: the device table is full", port + 1);
    }
}

void usb_core_device_drop(struct xhci_ctrl *hc, int device, const char *why)
{
    int i;

    for (i = 0; i < USB_MAX_DEVICES; i++) {
        if (dev_at(i)->state == ST_RUNNING && dev_at(i)->hc == hc && dev_at(i)->slot == device) {
            klog(KLOG_WARN, "usb", "device: port %s: %s; its port is enumerated again",
                 path_of(dev_at(i)), why);
            device_gone(i);
            return;
        }
    }
}

void usb_core_hub_port(int hub, int port, int connected)
{
    int i;

    if (hub < 0 || hub >= USB_MAX_DEVICES || dev_at(hub)->state != ST_RUNNING || port < 0 ||
        port >= 32) {
        return;
    }
    /* What was on the port goes first: a swapped device is two devices. */
    for (i = 0; i < USB_MAX_DEVICES; i++) {
        if (dev_at(i)->owner == hub && dev_at(i)->port == port && dev_at(i)->state != ST_FREE) {
            device_gone(i);
        }
    }
    dev_at(hub)->refused &= ~((uint32_t)1u << port);
    if (connected) {
        device_arrive(dev_at(hub)->hc, hub, port);
    }
}

int usb_core_hub_tell(int index, int ports, int multi_tt, int think_time)
{
    struct usb_device_state *d;
    struct usb_command_result result;
    int answer;

    if (index < 0 || index >= USB_MAX_DEVICES || dev_at(index)->state != ST_RUNNING) {
        return -1;
    }
    d = dev_at(index);
    if (enumerating[bus_of(d)] >= 0 && enumerating[bus_of(d)] != index) {
        return 0; /* another device has the command ring */
    }
    if (enumerating[bus_of(d)] != index && !xhci_command_free(d->hc)) {
        return 0; /* a command another device left is still on it */
    }
    enumerating[bus_of(d)] = index;
    if (!d->closing_asked) {
        d->hub_multi = multi_tt;
        if (xhci_device_hub(d->hc, d->slot, ports, multi_tt, think_time) != 0) {
            enumerating[bus_of(d)] = -1;
            return -1;
        }
        d->closing_asked = 1; /* the one command flag a running device has */
        d->deadline = core_ms + STEP_MS;
        return 0;
    }
    answer = command_result(d, &result);
    if (answer == 0) {
        return 0;
    }
    d->closing_asked = 0;
    enumerating[bus_of(d)] = -1;
    return answer < 0 ? -1 : 1;
}

void usb_core_hub_failed(int index, const char *why)
{
    if (index < 0 || index >= USB_MAX_DEVICES || dev_at(index)->state != ST_RUNNING) {
        return;
    }
    fail(dev_at(index), why);
}

int usb_core_location(int index, struct usb_location *out)
{
    if (index < 0 || index >= USB_MAX_DEVICES || dev_at(index)->state == ST_FREE) {
        return -1;
    }
    *out = dev_at(index)->where;
    return 0;
}

/**
 * One pass of the core, from the pump.
 *
 * An entry that has come free gives its frame back here and not where it was
 * freed: the record is the frame, and a caller that released it itself would
 * go on reading what it had just given away.
 *
 * Each controller is drained once, and every completion goes to the device
 * on that controller whose slot it names, since slot numbers repeat across
 * controllers. A device that drained the controller itself would take the
 * others' completions and throw them away, and a device enumerated beside a
 * running one would never be answered.
 *
 * A device whose port says it has gone is taken down, its slot asked back
 * before the entry is reused. A connected port with no device, on a root
 * port or on a running hub, gets an entry unless it was refused, and an empty
 * port forgets its refusal, so the next thing plugged in there is tried like
 * any other. A hub's ports are looked at on every pass because the hub driver
 * reports a connection only once, and a device that found the table full
 * then would otherwise stay unknown after an entry came free. The class
 * drivers step last, so a command's next stage goes out in the same pass as
 * the answer to the one before it.
 */
void usb_core_step(uint32_t elapsed_ms)
{
    int port;
    int i;
    int n;

    core_ms += elapsed_ms;

    for (i = 0; i < USB_MAX_DEVICES; i++) {
        if (devices[i] && devices[i]->state == ST_FREE) {
            dev_release(i);
        }
    }

    for (n = 0; n < xhci_controllers(); n++) {
        struct xhci_ctrl *hc = xhci_controller(n);
        struct usb_completion completion;

        while (xhci_poll(hc, &completion)) {
            if (msc_completion(&completion)) {
                continue; /* a stick's, and its driver has taken it */
            }
            if (hub_completion(&completion)) {
                continue;
            }
            for (i = 0; i < USB_MAX_DEVICES; i++) {
                if (dev_at(i)->state == ST_FREE || dev_at(i)->hc != completion.hc ||
                    dev_at(i)->slot != completion.device) {
                    continue;
                }
                if (completion.endpoint == 1) {
                    dev_at(i)->control = completion;
                    dev_at(i)->have_control = 1;
                } else if (completion.endpoint == dev_at(i)->ep_dci) {
                    dev_at(i)->report_done = completion;
                    dev_at(i)->have_report = 1;
                }
                break;
            }
        }
    }

    for (i = 0; i < USB_MAX_DEVICES; i++) {
        struct usb_port_status status;

        if (dev_at(i)->state == ST_FREE || dev_at(i)->state == ST_CLOSING ||
            dev_at(i)->state == ST_TOMBSTONE) {
            continue;
        }
        if (dev_at(i)->ops->status(dev_at(i)->hc, dev_at(i)->owner, dev_at(i)->port, &status) !=
                0 ||
            !status.connected) {
            device_gone(i);
        }
    }

    /* A port that has a device and no state gets one, on every controller. */
    for (n = 0; n < xhci_controllers(); n++) {
        struct xhci_ctrl *hc = xhci_controller(n);
        int ports = xhci_ports(hc);

        for (port = 0; port < ports && port < 32; port++) {
            struct usb_port_status status;
            int known = 0;

            if (xhci_port_status(hc, port, &status) != 0 || !status.connected) {
                port_refused[n] &= ~((uint32_t)1u << port);
                port_unentered[n] &= ~((uint32_t)1u << port);
                continue;
            }
            if (port_refused[n] & ((uint32_t)1u << port)) {
                continue;
            }
            for (i = 0; i < USB_MAX_DEVICES; i++) {
                if (dev_at(i)->state != ST_FREE && dev_at(i)->hc == hc && dev_at(i)->owner < 0 &&
                    dev_at(i)->port == port) {
                    known = 1;
                }
            }
            if (known) {
                continue;
            }
            device_arrive(hc, -1, port);
        }
    }

    for (i = 0; i < USB_MAX_DEVICES; i++) {
        struct usb_device_state *hub = dev_at(i);

        if (hub->state != ST_RUNNING || hub->interface.kind != USB_KIND_HUB) {
            continue;
        }
        for (port = 0; port < USB_HUB_PORT_MAX; port++) {
            struct usb_port_status status;
            int j;
            int known = 0;

            if (hub_port_status(hub->hc, i, port, &status) != 0 || !status.connected) {
                hub->refused &= ~((uint32_t)1u << port);
                hub->unentered &= ~((uint32_t)1u << port);
                continue;
            }
            if (hub->refused & ((uint32_t)1u << port)) {
                continue;
            }
            for (j = 0; j < USB_MAX_DEVICES; j++) {
                if (dev_at(j)->state != ST_FREE && dev_at(j)->owner == i &&
                    dev_at(j)->port == port) {
                    known = 1;
                }
            }
            if (!known) {
                device_arrive(hub->hc, i, port);
            }
        }
    }

    for (i = 0; i < USB_MAX_DEVICES; i++) {
        if (dev_at(i)->state == ST_RUNNING) {
            step_running(dev_at(i));
        } else if (dev_at(i)->state != ST_FREE && dev_at(i)->state != ST_FAILED &&
                   dev_at(i)->state != ST_TOMBSTONE) {
            step_device(dev_at(i));
        }
    }

    msc_step(elapsed_ms);
    hub_step(elapsed_ms);
    usbdisk_step();
}

void usb_core_reset(void)
{
    int k;
    int i;

    for (k = 0; k < XHCI_MAX_CONTROLLERS; k++) {
        port_refused[k] = 0;
        port_unentered[k] = 0;
        enumerating[k] = -1;
    }

    for (i = 0; i < USB_MAX_DEVICES; i++) {
        dev_release(i);
    }
    core_ms = 0;
}

/**
 * Clear a halted endpoint on a device the core enumerated, for its class
 * driver.
 *
 * The controller takes one command at a time and the core is what shares it
 * out, so a class driver that issued its own would take the answer to
 * somebody else's. This takes the same turn enumeration takes and reports
 * the command the way the core reads it: 1 when the endpoint is usable
 * again, 0 while the controller has not answered, and -1 when it refused or
 * never did.
 *
 * Nothing is submitted on the endpoint from the moment a quiesce is asked
 * for, the wait for the turn included. The first command depends on the
 * endpoint's state: a running one is stopped, a halted one reset, and a
 * stopped one or one in the Error state needs neither (xHCI 1.2, 4.6.9,
 * 4.6.8, 4.6.10); a Set TR Dequeue follows. When the commands have answered
 * with success no block of the endpoint's transfers can be fetched again,
 * the transfers are retired, and the endpoint is the caller's.
 */
int usb_core_endpoint_quiesce(struct xhci_ctrl *hc, int device, int endpoint)
{
    int i;

    for (i = 0; i < USB_MAX_DEVICES; i++) {
        struct usb_device_state *d = dev_at(i);
        struct usb_command_result result;
        int answer;
        int rc;

        if (d->state == ST_FREE || d->hc != hc || d->slot != device) {
            continue;
        }
        if (d->quiesce_stage == 0) {
            int state;

            xhci_endpoint_quiescing(d->hc, device, endpoint, 1);
            if (enumerating[bus_of(d)] >= 0 && enumerating[bus_of(d)] != i) {
                return 0; /* another device has this controller's command ring */
            }
            if (!xhci_command_free(d->hc)) {
                return 0;
            }
            enumerating[bus_of(d)] = i;
            d->quiesce_dci = endpoint;
            state = xhci_endpoint_state(d->hc, device, endpoint);
            if (state == XHCI_EP_HALTED) {
                rc = xhci_endpoint_reset(d->hc, device, endpoint);
                d->quiesce_stage = 1;
            } else if (state == XHCI_EP_STOPPED || state == XHCI_EP_ERROR) {
                rc = xhci_endpoint_dequeue_here(d->hc, device, endpoint);
                d->quiesce_stage = 2;
            } else {
                rc = xhci_endpoint_stop(d->hc, device, endpoint);
                d->quiesce_stage = 1;
            }
            if (rc != 0) {
                d->quiesce_stage = 0;
                xhci_endpoint_quiescing(d->hc, device, endpoint, 0);
                enumerating[bus_of(d)] = -1;
                return -1;
            }
            d->deadline = core_ms + STEP_MS;
            return 0;
        }
        if (d->quiesce_dci != endpoint) {
            return 0; /* a quiesce of another endpoint of the device is in progress */
        }
        answer = command_result(d, &result);
        if (answer == 0) {
            return 0;
        }
        if (answer < 0 || result.status != USB_XFER_OK) {
            d->quiesce_stage = 0;
            xhci_endpoint_quiescing(d->hc, device, endpoint, 0);
            enumerating[bus_of(d)] = -1;
            return -1;
        }
        if (d->quiesce_stage == 1) {
            if (xhci_endpoint_dequeue_here(d->hc, device, endpoint) != 0) {
                d->quiesce_stage = 0;
                xhci_endpoint_quiescing(d->hc, device, endpoint, 0);
                enumerating[bus_of(d)] = -1;
                return -1;
            }
            d->quiesce_stage = 2;
            d->deadline = core_ms + STEP_MS;
            return 0;
        }
        xhci_endpoint_retire_all(d->hc, device, endpoint);
        xhci_endpoint_quiescing(d->hc, device, endpoint, 0);
        d->quiesce_stage = 0;
        enumerating[bus_of(d)] = -1;
        return 1;
    }
    return -1;
}

int usb_core_clear_halt(struct xhci_ctrl *hc, int device, int endpoint)
{
    return usb_core_endpoint_quiesce(hc, device, endpoint);
}

int usb_core_idle(void)
{
    int i;

    for (i = 0; i < USB_MAX_DEVICES; i++) {
        /* A device waiting for a slot counts as idle: it may wait for ever. */
        if (dev_at(i)->state != ST_FREE && dev_at(i)->state != ST_RUNNING &&
            dev_at(i)->state != ST_FAILED && dev_at(i)->state != ST_TOMBSTONE &&
            dev_at(i)->state != ST_SLOTWAIT) {
            return 0;
        }
    }
    return 1;
}

int usb_core_count(void)
{
    int i;
    int count = 0;

    for (i = 0; i < USB_MAX_DEVICES; i++) {
        if (dev_at(i)->state == ST_RUNNING) {
            count++;
        }
    }
    return count;
}

/** Append a string, bounded. */
static int put_str(char *out, int size, int at, const char *s)
{
    while (*s != '\0' && at < size - 1) {
        out[at] = *s++;
        at++;
    }
    return at;
}

/** Append an unsigned number in the given base, bounded. */
static int put_num(char *out, int size, int at, uint32_t value, int base)
{
    char tmp[12];
    int n = 0;

    if (value == 0) {
        tmp[n] = '0';
        n++;
    }
    while (value != 0 && n < (int)sizeof(tmp)) {
        uint32_t digit = value % (uint32_t)base;

        tmp[n] = (char)(digit < 10 ? '0' + digit : 'a' + (digit - 10));
        n++;
        value /= (uint32_t)base;
    }
    while (n > 0 && at < size - 1) {
        n--;
        out[at] = tmp[n];
        at++;
    }
    return at;
}

int usb_core_row(int index, struct usb_sys_row *row)
{
    const struct usb_device_state *d;
    int chain[USB_TREE_TIERS + 1];
    int depth = 0;
    uint32_t flags;

    if (index < 0 || index >= USB_MAX_DEVICES) {
        return -1;
    }
    flags = kirq_save();
    d = dev_at(index);
    if (d->state != ST_RUNNING) {
        kirq_restore(flags);
        return -1;
    }
    row->bus = bus_of(d) + 1;
    row->devnum = d->slot;
    row->vendor = d->vendor;
    row->product = d->product;
    row->release = d->release;
    while (d != 0 && depth <= USB_TREE_TIERS) {
        chain[depth++] = d->port + 1;
        d = d->owner >= 0 ? dev_at(d->owner) : 0;
    }
    row->depth = depth;
    while (depth > 0) {
        row->ports[row->depth - depth] = chain[depth - 1];
        depth--;
    }
    kirq_restore(flags);
    return 0;
}

int usb_core_describe(int index, char *out, int size)
{
    struct usb_device_state *d;
    int at = 0;
    const char *speed = "unknown";

    if (index < 0 || index >= USB_MAX_DEVICES || out == 0 || size < 8) {
        return 0;
    }
    d = dev_at(index);
    if (d->state != ST_RUNNING) {
        return 0;
    }
    switch (d->speed) {
    case USB_SPEED_LOW:
        speed = "low";
        break;
    case USB_SPEED_FULL:
        speed = "full";
        break;
    case USB_SPEED_HIGH:
        speed = "high";
        break;
    case USB_SPEED_SUPER:
        speed = "super";
        break;
    default:
        break;
    }

    at = put_str(out, size, at, "port ");
    at = put_str(out, size, at, path_of(d)); /* the root port, then each hub's port */
    at = put_str(out, size, at, " ");
    at = put_str(out, size, at, speed);
    at = put_str(out, size, at, " ");
    at = put_num(out, size, at, d->vendor, 16);
    at = put_str(out, size, at, ":");
    at = put_num(out, size, at, d->product, 16);
    at = put_str(out, size, at, " class ");
    at = put_num(out, size, at, (uint32_t)d->interface.class_code, 10);
    at = put_str(out, size, at, " protocol ");
    at = put_num(out, size, at, (uint32_t)d->interface.protocol, 10);
    at = put_str(out, size, at, "\n");
    out[at] = '\0';
    return at;
}
