/*
 * core.c: from a connected port to a configured device.
 *
 * The standard half of the stack. It knows the requests every device answers,
 * the shape of a descriptor, and which interface this kernel has a driver for.
 * It reaches the wire through `hcd.h` and names no register.
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
 * its port, which costs a hundred milliseconds and no complexity at all.
 */

#include "core.h"

#include "../klog.h"
#include "xhci.h"

/* The standard requests this kernel sends, and the descriptor types it asks
 * for. Everything else a device offers is left alone. */
#define REQ_GET_DESCRIPTOR   6
#define REQ_SET_CONFIGURATION 9
#define DESC_DEVICE 1
#define DESC_CONFIG 2

/* Descriptor types as they appear inside a configuration. */
#define DESC_INTERFACE 4
#define DESC_ENDPOINT  5

/* The endpoint descriptor's attribute and address fields. */
#define EP_ATTR_TYPE_MASK 0x3
#define EP_ATTR_INTERRUPT 3
#define EP_ADDR_IN        0x80

/* How long any one step of enumeration may take. A device that has not
 * answered by then is not going to. */
#define STEP_MS 500u

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
#define ST_SET_CONFIG 11
#define ST_RUNNING   12
#define ST_FAILED    13

struct usb_device_state {
    int state;
    int port;
    int speed;
    int slot;
    uint32_t deadline;
    uint8_t setup[8];
    uint8_t buffer[USB_DESC_MAX];
    uint32_t config_length;
    uint16_t vendor;
    uint16_t product;
    int ep0_packet;
    int ep_dci;
    struct usb_interface_info interface;
};

static struct usb_device_state devices[USB_MAX_DEVICES];

/* Which device is being enumerated, or -1. Only one is, because the controller
 * takes one command at a time. */
static int enumerating = -1;

/* This module's own clock, advanced with the driver's. */
static uint32_t core_ms;

void usb_parse_config(const uint8_t *buffer, int length, struct usb_interface_info *out)
{
    int offset = 0;
    int in_boot_interface = 0;

    out->found = 0;
    out->number = 0;
    out->class_code = 0;
    out->subclass = 0;
    out->protocol = 0;
    out->ep_number = 0;
    out->ep_max_packet = 0;
    out->ep_interval = 0;
    out->config_value = 0;

    if (buffer == 0 || length < 9) {
        return;
    }
    /* The configuration descriptor's own value, which SET_CONFIGURATION
     * names. */
    out->config_value = buffer[5];

    while (offset + 2 <= length) {
        int size = buffer[offset];
        int type = buffer[offset + 1];

        /* A descriptor shorter than its header, or longer than what arrived,
         * ends the walk. Either would otherwise be read past the buffer, and
         * a length of zero would be walked for ever. */
        if (size < 2 || offset + size > length) {
            break;
        }

        if (type == DESC_INTERFACE && size >= 9) {
            in_boot_interface = (buffer[offset + 5] == USB_CLASS_HID &&
                                 buffer[offset + 6] == USB_SUBCLASS_BOOT);
            if (in_boot_interface && !out->found) {
                out->number = buffer[offset + 2];
                out->class_code = buffer[offset + 5];
                out->subclass = buffer[offset + 6];
                out->protocol = buffer[offset + 7];
            }
        } else if (type == DESC_ENDPOINT && size >= 7 && in_boot_interface && !out->found) {
            int address = buffer[offset + 2];
            int attributes = buffer[offset + 3];

            if ((address & EP_ADDR_IN) && (attributes & EP_ATTR_TYPE_MASK) == EP_ATTR_INTERRUPT) {
                out->ep_number = address & 0xF;
                out->ep_max_packet = buffer[offset + 4] | (buffer[offset + 5] << 8);
                out->ep_interval = buffer[offset + 6];
                out->found = 1;
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
static int control_out(struct usb_device_state *d, int request, int value)
{
    struct usb_transfer transfer;

    setup_fill(d->setup, 0x00, request, value, 0, 0);
    transfer.setup = d->setup;
    transfer.buffer = 0;
    transfer.length = 0;
    transfer.direction = USB_DIR_OUT;
    return xhci_submit(d->slot, 1, &transfer);
}

/** Give up on a device, with the reason. */
static void fail(struct usb_device_state *d, const char *why)
{
    klog(KLOG_WARN, "usb", "device: port %d: %s", d->port + 1, why);
    if (d->slot > 0) {
        xhci_device_close(d->slot);
    }
    d->state = ST_FAILED;
    if (enumerating == (int)(d - devices)) {
        enumerating = -1;
    }
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

/**
 * The transfer this device is waiting for: 1 when it finished well, 0 when it
 * has not finished, -1 when it failed.
 */
static int transfer_result(struct usb_device_state *d)
{
    struct usb_completion completion;

    while (xhci_poll(&completion)) {
        if (completion.device != d->slot) {
            continue;
        }
        if (completion.status == USB_XFER_OK || completion.status == USB_XFER_SHORT) {
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

/** One step of one device. */
static void step_device(struct usb_device_state *d)
{
    struct usb_command_result result;
    int index = (int)(d - devices);
    int answer;

    switch (d->state) {
    case ST_SETTLING:
        if (!xhci_port_settled(d->port)) {
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
        /* The seventh byte is the size of its control endpoint, which is the
         * one thing that could not be known before asking. */
        d->ep0_packet = d->buffer[7];
        if (d->ep0_packet != 8 && d->speed != USB_SPEED_HIGH && d->speed != USB_SPEED_SUPER) {
            if (xhci_device_packet_size(d->slot, d->ep0_packet) != 0) {
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
        usb_parse_config(d->buffer, (int)d->config_length, &d->interface);
        if (!d->interface.found) {
            fail(d, "it offers no interface this kernel can drive");
            return;
        }
        {
            struct usb_endpoint endpoint;

            endpoint.number = d->interface.ep_number;
            endpoint.direction = USB_DIR_IN;
            endpoint.type = USB_EP_INTERRUPT;
            endpoint.max_packet = d->interface.ep_max_packet;
            endpoint.interval = d->interface.ep_interval;
            d->ep_dci = d->interface.ep_number * 2 + 1;
            if (xhci_endpoint_open(d->slot, &endpoint) != 0) {
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
        /* The endpoint exists and has bandwidth before the device is told to
         * use the configuration it belongs to, which is the order the
         * controller requires. */
        if (control_out(d, REQ_SET_CONFIGURATION, d->interface.config_value) != 0) {
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
        d->state = ST_RUNNING;
        enumerating = -1;
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

    /* A port that has a device and no state gets one. */
    for (port = 0; port < ports && port < USB_MAX_DEVICES * 4; port++) {
        struct usb_port_status status;
        int known = 0;

        if (xhci_port_status(port, &status) != 0 || !status.connected) {
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
                devices[i].interface.found = 0;
                break;
            }
        }
    }

    for (i = 0; i < USB_MAX_DEVICES; i++) {
        if (devices[i].state != ST_FREE && devices[i].state != ST_RUNNING &&
            devices[i].state != ST_FAILED) {
            step_device(&devices[i]);
        }
    }
}

void usb_core_reset(void)
{
    int i;

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
            devices[i].state != ST_FAILED) {
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
