/*
 * core.c: from a connected port to a configured device.
 *
 * The standard half of the stack. It implements the requests every device
 * answers, the shape of a descriptor, and the choice of an interface this
 * kernel has a driver for. It reaches the wire through `hcd.h` and touches no
 * register.
 *
 * Everything here is a state machine, and the reason is in `hcd.h`: the
 * completions are drained from the timer handler, where a wait would stop the
 * clock. So no step of enumeration waits for the one before it. Each call to
 * `usb_core_step` moves one device forward by one step, and a step that has
 * not finished is looked at again next time. A step that never finishes fails
 * its device at a deadline and gives the slot back.
 *
 * One device is enumerated at a time, because the controller has one command
 * ring and this driver keeps one command in flight. A second device waits at
 * its port, which costs a hundred milliseconds.
 */

#include "core.h"

#include "../input.h"
#include "../klog.h"
#include "hid.h"
#include "msc.h"
#include "xhci.h"

/* The standard requests this kernel sends, and the descriptor types it asks
 * for. Everything else a device offers is left alone. */
#define REQ_GET_DESCRIPTOR   6
#define REQ_SET_CONFIGURATION 9
/* The class request that puts a human-interface device in boot protocol,
 * whose two report layouts are fixed and need no describing. */
#define REQ_SET_PROTOCOL 11
#define TYPE_TO_INTERFACE 0x21
#define DESC_DEVICE 1
#define DESC_CONFIG 2

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
#define STEP_MS 500u

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
#define ST_FREE      0
#define ST_SETTLING  1
#define ST_RESETTING 2
#define ST_SLOT      3
#define ST_ADDRESS   4
#define ST_DESC8     5
#define ST_PACKET    6
#define ST_DESC18    7
#define ST_CONFIG9   8
#define ST_CONFIGALL 9
#define ST_OPEN_EP   10
/* A stick has two endpoints and the controller takes one command each. */
#define ST_OPEN_EP2  18
#define ST_SET_CONFIG 11
#define ST_PROTOCOL  15
#define ST_RUNNING   12
#define ST_FAILED    13
#define ST_CLOSING   14
#define ST_HALTED    16
/* A device whose slot the controller never gave back. The entry is not free
 * and never will be: the controller may still write into the rings and the
 * context that slot names, so nothing of this device is reused. */
#define ST_TOMBSTONE 17

struct usb_device_state {
    int state;
    int port;
    int speed;
    int slot;
    uint32_t deadline;
    int closing_asked;
    uint32_t short_by;
    int reporting;
    int dequeue_asked;
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
    int ep0_packet;
    int ep_dci;
    int ep_out_dci; /* a stick's outward endpoint; zero on a device with one */
    struct usb_interface_info interface;
};

static struct usb_device_state devices[USB_MAX_DEVICES];

/* Which device is being enumerated, or -1. Only one is, because the controller
 * takes one command at a time. */
static int enumerating = -1;

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
static uint32_t port_refused;

/* This module's own clock, advanced with the driver's. */
static uint32_t core_ms;

void usb_parse_config(const uint8_t *buffer, int length, struct usb_interface_info *out)
{
    int offset = 0;
    int in_boot_interface = 0;
    int in_storage_interface = 0;

    out->found = 0;
    out->kind = USB_KIND_NONE;
    out->number = 0;
    out->class_code = 0;
    out->subclass = 0;
    out->protocol = 0;
    out->ep_number = 0;
    out->ep_max_packet = 0;
    out->ep_interval = 0;
    out->ep_out_number = 0;
    out->ep_out_max_packet = 0;
    out->config_value = 0;

    if (buffer == 0 || length < 9) {
        return;
    }
    /* The configuration descriptor's own value, which SET_CONFIGURATION
     * names. Zero is the value that means no configuration, so a device
     * offering it is offering nothing to select. */
    out->config_value = buffer[5];
    if (out->config_value == 0) {
        return;
    }

    while (offset + 2 <= length) {
        int size = buffer[offset];
        int type = buffer[offset + 1];
        int in_storage_interface_now;

        /* A descriptor shorter than its header, or longer than what arrived,
         * ends the walk. Either would otherwise be read past the buffer, and
         * a length of zero would be walked for ever. */
        if (size < 2 || offset + size > length) {
            break;
        }

        if (type == DESC_INTERFACE && size >= 9) {
            /* The alternate setting has to be zero: this kernel sends no
             * SET_INTERFACE, so a device is left on setting zero whatever
             * else it offers, and taking an endpoint from another setting
             * would be taking one that is not there. The protocol has to be
             * one of the two the boot report layouts are defined for, or the
             * one transport every flash drive carries. */
            in_boot_interface = (buffer[offset + 3] == 0 &&
                                 buffer[offset + 5] == USB_CLASS_HID &&
                                 buffer[offset + 6] == USB_SUBCLASS_BOOT &&
                                 (buffer[offset + 7] == USB_PROTOCOL_KBD ||
                                  buffer[offset + 7] == USB_PROTOCOL_MOUSE));
            in_storage_interface_now = (buffer[offset + 3] == 0 &&
                                        buffer[offset + 5] == USB_CLASS_STORAGE &&
                                        buffer[offset + 6] == USB_SUBCLASS_SCSI &&
                                        buffer[offset + 7] == USB_PROTOCOL_BULK_ONLY);
            in_storage_interface = in_storage_interface_now;
            if ((in_boot_interface || in_storage_interface_now) && !out->found &&
                out->kind == USB_KIND_NONE) {
                out->kind = in_boot_interface ? USB_KIND_HID : USB_KIND_STORAGE;
                out->number = buffer[offset + 2];
                out->class_code = buffer[offset + 5];
                out->subclass = buffer[offset + 6];
                out->protocol = buffer[offset + 7];
            } else if (in_storage_interface_now && out->kind != USB_KIND_STORAGE) {
                /* A storage interface behind an interface of another kind
                 * this kernel already took: it is not this device's driver's
                 * to have, and the endpoints below belong to nobody. */
                in_storage_interface = 0;
            }
        } else if (type == DESC_ENDPOINT && size >= 7 && in_boot_interface && !out->found) {
            int address = buffer[offset + 2];
            int attributes = buffer[offset + 3];

            if ((address & EP_ADDR_IN) && (attributes & EP_ATTR_TYPE_MASK) == EP_ATTR_INTERRUPT &&
                (address & 0xF) != 0) {
                out->ep_number = address & 0xF;
                out->ep_max_packet = buffer[offset + 4] | (buffer[offset + 5] << 8);
                out->ep_interval = buffer[offset + 6];
                out->found = 1;
            }
        } else if (type == DESC_ENDPOINT && size >= 7 && in_storage_interface && !out->found) {
            int address = buffer[offset + 2];
            int attributes = buffer[offset + 3];

            /* A stick answers on one bulk endpoint each way, and this kernel
             * takes the first of each. The numbers are the device's own. */
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
    return xhci_submit(d->slot, 1, &transfer);
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
    return xhci_submit(d->slot, 1, &transfer);
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

/** Give up on a device, with the reason. */
static void fail(struct usb_device_state *d, const char *why)
{
    klog(KLOG_WARN, "usb", "device: port %d: %s", d->port + 1, why);
    if (enumerating == (int)(d - devices)) {
        enumerating = -1;
    }
    if (d->interface.kind == USB_KIND_STORAGE) {
        msc_detach(d->slot);
    }
    hid_gone(d->slot);
    /* The port is remembered, not the entry: a machine with several devices
     * this kernel cannot drive would otherwise spend its whole table on them
     * and have none left for what a person plugs in. */
    if (d->port >= 0 && d->port < 32) {
        port_refused |= (uint32_t)1u << d->port;
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
 */
static void step_closing(struct usb_device_state *d)
{
    struct usb_command_result result;
    int index = (int)(d - devices);

    /*
     * Closing takes the same turn enumeration does. There is one command in
     * flight and one answer to it, and whichever device asks first takes that
     * answer: a device waiting for its slot would otherwise be handed the
     * answer to another device's Disable Slot, and would take the slot being
     * given back as the slot it had just been given.
     */
    if (enumerating >= 0 && enumerating != index) {
        return;
    }
    enumerating = index;

    if (d->slot <= 0) {
        d->state = ST_FREE;
        enumerating = -1;
        return;
    }
    if (d->closing_asked) {
        int done = xhci_command_done(&result);

        if (done == 0 && !expired(d)) {
            return;
        }
        /* Either the controller gave the slot back or it never answered. The
         * second is recorded; the entry goes either way, because a slot this
         * kernel cannot get back is one of four it has lost, and holding the
         * entry as well would lose a device table row too. */
        if (done != 1) {
            /*
             * The controller never answered. Its slot, the rings under it and
             * the context it names stay the controller's, because it may
             * still write into them; the entry becomes a tombstone and this
             * kernel is one device poorer until the machine restarts. Giving
             * the slot back here and reusing the entry is what would hand the
             * next device a ring the controller is still reading.
             */
            klog(KLOG_WARN, "usb", "slot: %d was not given back; the entry is lost", d->slot);
            enumerating = -1;
            d->state = ST_TOMBSTONE;
            return;
        }
        xhci_device_closed(d->slot);
        d->slot = 0;
        enumerating = -1;
        d->state = ST_FREE;
        return;
    }
    if (xhci_device_close(d->slot) == 0) {
        d->closing_asked = 1;
        d->deadline = core_ms + CLOSE_LIMIT_MS;
    }
}

/**
 * The transfer this device is waiting for: 1 when it finished well, 0 when it
 * has not finished, -1 when it failed.
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
            /* The device answered with less than was asked for. That is a
             * legal thing for a device to do and a fatal thing for a reader
             * that parses the buffer as if it were whole, so how much is
             * missing is kept and each step decides. */
            d->short_by = completion.residual;
            return 1;
        }
        return -1;
    }
    return expired(d) ? -1 : 0;
}

/** The command this device is waiting for, with the same three answers. */
static int command_result(struct usb_device_state *d, struct usb_command_result *out)
{
    int done = xhci_command_done(out);

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
 */
static void step_running(struct usb_device_state *d)
{
    int kind;
    int length;

    if (d->interface.kind == USB_KIND_STORAGE) {
        /* A stick answers when it is asked and not before, so there is no
         * transfer to keep outstanding on it. Its own driver steps it. */
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
        if (completion.status == USB_XFER_STALLED) {
            /* A halted endpoint keeps its ring until it is reset, and nothing
             * queued on it moves. Both halves of the recovery are commands,
             * and the second is issued when the first has answered. */
            if (xhci_endpoint_reset(d->slot, d->ep_dci) == 0) {
                d->state = ST_HALTED;
                arm(d);
                return;
            }
        } else if (completion.status == USB_XFER_OK || completion.status == USB_XFER_SHORT) {
            hid_report(d->slot, kind, d->report, (int)(completion.length - completion.residual));
        }
    }

    if (!d->reporting) {
        struct usb_transfer transfer;

        transfer.setup = 0;
        transfer.buffer = d->report;
        transfer.length = (uint32_t)length;
        transfer.direction = USB_DIR_IN;
        if (xhci_submit(d->slot, d->ep_dci, &transfer) == 0) {
            d->reporting = 1;
        }
    }
}

/** One step of one device. */
static void step_device(struct usb_device_state *d)
{
    struct usb_command_result result;
    int index = (int)(d - devices);
    int answer;

    switch (d->state) {
    case ST_CLOSING:
        step_closing(d);
        return;

    case ST_HALTED:
        answer = command_result(d, &result);
        if (answer == 0) {
            return;
        }
        if (answer < 0) {
            fail(d, "its halted endpoint would not reset");
            return;
        }
        if (d->dequeue_asked) {
            d->dequeue_asked = 0;
            d->state = ST_RUNNING;
            d->reporting = 0;
            return;
        }
        if (xhci_endpoint_dequeue(d->slot, d->ep_dci) != 0) {
            fail(d, "its endpoint's ring could not be pointed anywhere");
            return;
        }
        d->dequeue_asked = 1;
        arm(d);
        return;

    case ST_SETTLING:
        if (!xhci_port_settled(d->port)) {
            /* A connect that never settles is a port that keeps bouncing, or
             * a device pulled out again. Either way this entry is not held for
             * ever. */
            if (expired(d)) {
                d->state = ST_FREE;
            }
            return;
        }
        if (enumerating >= 0 && enumerating != index) {
            return; /* another device has the command ring */
        }
        enumerating = index;
        if (xhci_port_reset_begin(d->port) != 0) {
            fail(d, "the port would not begin a reset");
            return;
        }
        d->state = ST_RESETTING;
        arm(d);
        return;

    case ST_RESETTING:
        answer = xhci_port_reset_done(d->port);
        if (answer == 0) {
            return;
        }
        if (answer < 0) {
            fail(d, "the port never came out of reset");
            return;
        }
        {
            struct usb_port_status status;

            if (xhci_port_status(d->port, &status) != 0 || !status.connected) {
                fail(d, "the device left during its reset");
                return;
            }
            d->speed = status.speed;
        }
        if (xhci_device_open(d->port, d->speed) != 0) {
            /* The controller takes one command at a time, and another device
             * may have one in flight; this is a turn to wait, not a refusal.
             * The deadline armed when the reset began is what ends the
             * waiting. */
            if (!expired(d)) {
                return;
            }
            fail(d, "no slot could be asked for");
            return;
        }
        d->state = ST_SLOT;
        arm(d);
        return;

    case ST_SLOT:
        answer = command_result(d, &result);
        if (answer == 0) {
            return;
        }
        if (answer < 0 || result.device <= 0) {
            fail(d, "the controller gave out no slot");
            return;
        }
        d->slot = result.device;
        if (xhci_device_address(d->slot) != 0) {
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
        /* The seventh byte is the size of its control endpoint, which is the
         * one thing that could not be known before asking. */
        d->ep0_packet = d->buffer[7];
        if (d->ep0_packet != 8 && d->speed != USB_SPEED_HIGH && d->speed != USB_SPEED_SUPER) {
            if (xhci_device_packet_size(d->slot, d->ep0_packet) != 0) {
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
        if (d->config_length > USB_DESC_MAX) {
            d->config_length = USB_DESC_MAX;
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
        /* What arrived, not what was asked for: a device that answered short
         * is walked only as far as it answered. */
        usb_parse_config(d->buffer, (int)(d->config_length - d->short_by), &d->interface);
        if (!d->interface.found) {
            fail(d, "it offers no interface this kernel can drive");
            return;
        }
        {
            struct usb_endpoint endpoint;

            endpoint.number = d->interface.ep_number;
            endpoint.direction = USB_DIR_IN;
            endpoint.type = (d->interface.kind == USB_KIND_STORAGE) ? USB_EP_BULK
                                                                    : USB_EP_INTERRUPT;
            endpoint.max_packet = d->interface.ep_max_packet;
            endpoint.interval = d->interface.ep_interval;
            d->ep_dci = d->interface.ep_number * 2 + 1;
            d->ep_out_dci = 0;
            if (xhci_endpoint_open(d->slot, &endpoint) != 0) {
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

            /* The second of the two a stick answers on. It is a command of
             * its own, so it is a state of its own. */
            endpoint.number = d->interface.ep_out_number;
            endpoint.direction = USB_DIR_OUT;
            endpoint.type = USB_EP_BULK;
            endpoint.max_packet = d->interface.ep_out_max_packet;
            endpoint.interval = 0;
            d->ep_out_dci = d->interface.ep_out_number * 2;
            if (xhci_endpoint_open(d->slot, &endpoint) != 0) {
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
        /* The endpoint exists and has bandwidth before the device is told to
         * use the configuration it belongs to, which is the order the
         * controller requires. */
        if (control_out(d, 0x00, REQ_SET_CONFIGURATION, d->interface.config_value, 0) != 0) {
            fail(d, "it could not be told its configuration");
            return;
        }
        d->state = ST_SET_CONFIG;
        arm(d);
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
        arm(d);
        return;

    case ST_SET_CONFIG:
        answer = transfer_result(d);
        if (answer == 0) {
            return;
        }
        if (answer < 0) {
            fail(d, "it refused its configuration");
            return;
        }
        if (d->interface.kind == USB_KIND_STORAGE) {
            /* A stick has no protocol to be put into: its transport is the
             * class requests and the two bulk endpoints, and the class driver
             * takes it from here. */
            if (msc_attach(d->slot, d->interface.number, d->ep_dci, d->ep_out_dci) != 0) {
                fail(d, "this kernel drives no more sticks at once");
                return;
            }
            d->state = ST_RUNNING;
            d->reporting = 0;
            enumerating = -1;
            klog(KLOG_INFO, "usb", "device: port %d: %x:%x, class %d protocol %d, endpoints %d and %d",
                 d->port + 1, (int)d->vendor, (int)d->product, d->interface.class_code,
                 d->interface.protocol, d->interface.ep_number, d->interface.ep_out_number);
            return;
        }
        /* The device is configured; now it is told to report in the protocol
         * this kernel reads. A device that refuses is left configured and
         * unread rather than driven in a language nothing here parses. */
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
        enumerating = -1;
        if (d->interface.protocol == USB_PROTOCOL_MOUSE) {
            /* `/dev/mouse` opens when a mouse exists, of either kind. Without
             * this an open of that node fails on a machine whose only mouse is
             * on USB. */
            input_mouse_found();
        }
        klog(KLOG_INFO, "usb", "device: port %d: %x:%x, class %d protocol %d, endpoint %d",
             d->port + 1, (int)d->vendor, (int)d->product, d->interface.class_code,
             d->interface.protocol, d->interface.ep_number);
        return;

    default:
        return;
    }
}

void usb_core_step(uint32_t elapsed_ms)
{
    int ports = xhci_ports();
    int port;
    int i;

    core_ms += elapsed_ms;

    /*
     * One drain, and every completion goes to the device it names. A device
     * that drained the controller itself would take the completions of the
     * others and throw them away, which is a device enumerated beside a
     * running one never being answered.
     */
    {
        struct usb_completion completion;

        while (xhci_poll(&completion)) {
            if (msc_completion(&completion)) {
                continue; /* a stick's, and its driver has taken it */
            }
            for (i = 0; i < USB_MAX_DEVICES; i++) {
                if (devices[i].state == ST_FREE || devices[i].slot != completion.device) {
                    continue;
                }
                if (completion.endpoint == 1) {
                    devices[i].control = completion;
                    devices[i].have_control = 1;
                } else if (completion.endpoint == devices[i].ep_dci) {
                    devices[i].report_done = completion;
                    devices[i].have_report = 1;
                }
                break;
            }
        }
    }

    /* A device that has left. Its port says so; the controller still holds
     * everything it was given, and the slot is asked back before the entry is
     * reused. */
    for (i = 0; i < USB_MAX_DEVICES; i++) {
        struct usb_port_status status;

        if (devices[i].state == ST_FREE || devices[i].state == ST_CLOSING ||
            devices[i].state == ST_TOMBSTONE) {
            continue;
        }
        if (xhci_port_status(devices[i].port, &status) != 0 || !status.connected) {
            if (devices[i].state == ST_RUNNING) {
                klog(KLOG_INFO, "usb", "device: port %d: gone", devices[i].port + 1);
                if (devices[i].interface.kind == USB_KIND_STORAGE) {
                    msc_detach(devices[i].slot);
                } else {
                    hid_gone(devices[i].slot);
                }
            }
            if (enumerating == i) {
                enumerating = -1;
            }
            devices[i].closing_asked = 0;
            devices[i].state = devices[i].slot > 0 ? ST_CLOSING : ST_FREE;
            devices[i].deadline = core_ms + STEP_MS;
        }
    }

    /* A port that has a device and no state gets one. */
    for (port = 0; port < ports && port < 32; port++) {
        struct usb_port_status status;
        int known = 0;

        if (xhci_port_status(port, &status) != 0 || !status.connected) {
            /* Nothing is there, so whatever was refused there is forgotten:
             * the next thing plugged into that port is tried like any other. */
            port_refused &= ~((uint32_t)1u << port);
            continue;
        }
        if (port_refused & ((uint32_t)1u << port)) {
            continue;
        }
        for (i = 0; i < USB_MAX_DEVICES; i++) {
            if (devices[i].state != ST_FREE && devices[i].port == port) {
                known = 1;
            }
        }
        if (known) {
            continue;
        }
        for (i = 0; i < USB_MAX_DEVICES; i++) {
            if (devices[i].state == ST_FREE) {
                devices[i].state = ST_SETTLING;
                devices[i].port = port;
                devices[i].slot = 0;
                devices[i].ep_dci = 0;
                devices[i].closing_asked = 0;
                devices[i].interface.found = 0;
                devices[i].deadline = core_ms + SETTLE_LIMIT_MS;
                break;
            }
        }
    }

    for (i = 0; i < USB_MAX_DEVICES; i++) {
        if (devices[i].state == ST_RUNNING) {
            step_running(&devices[i]);
        } else if (devices[i].state != ST_FREE && devices[i].state != ST_FAILED &&
                   devices[i].state != ST_TOMBSTONE) {
            step_device(&devices[i]);
        }
    }

    /* And the sticks, after the completions above have been handed out: a
     * command's next stage goes out in the same pass as the answer to the
     * one before it. */
    msc_step(elapsed_ms);
}

void usb_core_reset(void)
{
    int i;

    port_refused = 0;

    for (i = 0; i < USB_MAX_DEVICES; i++) {
        devices[i].state = ST_FREE;
        devices[i].slot = 0;
        devices[i].port = 0;
    }
    enumerating = -1;
    core_ms = 0;
}

int usb_core_idle(void)
{
    int i;

    for (i = 0; i < USB_MAX_DEVICES; i++) {
        if (devices[i].state != ST_FREE && devices[i].state != ST_RUNNING &&
            devices[i].state != ST_FAILED && devices[i].state != ST_TOMBSTONE) {
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
        if (devices[i].state == ST_RUNNING) {
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

int usb_core_describe(int index, char *out, int size)
{
    struct usb_device_state *d;
    int at = 0;
    const char *speed = "unknown";

    if (index < 0 || index >= USB_MAX_DEVICES || out == 0 || size < 8) {
        return 0;
    }
    d = &devices[index];
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
    at = put_num(out, size, at, (uint32_t)(d->port + 1), 10);
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
