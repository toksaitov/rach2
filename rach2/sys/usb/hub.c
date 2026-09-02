/*
 * hub.c: the hub class driver.
 *
 * A hub is a device with ports of its own, and the core enumerates what is
 * on them the way it enumerates a root port: it asks the port's status,
 * waits for the connection to settle, resets the port, reads the speed and
 * opens a device at the location below the hub. This driver answers those
 * questions for a hub's ports, from what the hub says in its port status
 * words, and it keeps the hub itself going: its descriptor read, the
 * controller told that the slot is a hub, every port powered, the power-good
 * time waited, and each port's status read once so that a device already
 * plugged in when the hub arrived is found.
 *
 * Everything the hub is asked is a control transfer on its endpoint zero,
 * one at a time per hub, stepped from the pump. Nothing here waits.
 */

#include "hub.h"

#include "../klog.h"
#include "../lib/kstring.h"
#include "core.h"
#include "hcd.h"
#include "xhci.h"

/* The hub class requests, from the USB 2.0 specification's chapter 11. The
 * request types name the class recipient: the hub for its descriptor and
 * its own status, a port for a port's. */
#define TYPE_CLASS_HUB_IN   0xA0
#define TYPE_CLASS_HUB_OUT  0x20
#define TYPE_CLASS_PORT_IN  0xA3
#define TYPE_CLASS_PORT_OUT 0x23
#define REQ_GET_STATUS      0
#define REQ_CLEAR_FEATURE   1
#define REQ_SET_FEATURE     3
#define REQ_GET_DESCRIPTOR  6
#define DESC_HUB            0x29

/* The port feature selectors (Table 11-17). */
#define PORT_RESET        4
#define PORT_POWER        8
#define C_PORT_CONNECTION 16
#define C_PORT_ENABLE     17
#define C_PORT_RESET      20

/* The port status word (Table 11-21). */
#define PORT_STAT_CONNECTION 0x0001
#define PORT_STAT_ENABLE     0x0002
#define PORT_STAT_RESET      0x0010
#define PORT_STAT_POWER      0x0100
#define PORT_STAT_LOW_SPEED  0x0200
#define PORT_STAT_HIGH_SPEED 0x0400

/* The power switching modes of wHubCharacteristics (Table 11-13). */
#define LPSM_GANGED     0
#define LPSM_INDIVIDUAL 1
#define LPSM_NONE       2

/* How long a hub has for any one request, how long a connection must hold
 * before it is settled, how often a port in reset is asked, and how long a
 * reset may take, all in milliseconds. The debounce and the reset floor are
 * the specification's; the reset ceiling is this driver's. */
#define HUB_REQUEST_MS    500u
#define HUB_DEBOUNCE_MS   100u
#define HUB_RESET_POLL_MS 20u
#define HUB_RESET_MS      500u
#define HUB_POWER_MAX_MS  500u

/* What the hub as a whole is doing. */
#define HUB_DESCRIPTOR 1 /* its descriptor asked for */
#define HUB_TELL       2 /* the controller being told it is a hub */
#define HUB_POWER      3 /* its ports being powered */
#define HUB_POWER_WAIT 4 /* the power-good time */
#define HUB_SCAN       5 /* each port's status read once */
#define HUB_RUNNING    6

/* What a port is doing, for the core's reset. */
#define RESET_NONE   0
#define RESET_ASKED  1 /* the core asked; the request has not gone out */
#define RESET_SENT   2 /* SET_PORT_FEATURE(PORT_RESET) out */
#define RESET_POLL   3 /* the status being read until the reset ends */
#define RESET_CLEAR  4 /* C_PORT_RESET being cleared */
#define RESET_DONE   5
#define RESET_FAILED 6

/* What the request in flight was for. */
#define REQ_NONE        0
#define REQ_DESCRIPTOR  1
#define REQ_POWER       2
#define REQ_PORT_STATUS 3
#define REQ_PORT_RESET  4
#define REQ_CLEAR       5

struct hub_port {
    uint16_t status;
    uint16_t change;
    int known;           /* the status has been read at least once */
    uint32_t connect_ms; /* when the connection was first seen */
    int reset;           /* one of the RESET_ values */
    uint32_t reset_ms;   /* how long the reset has run */
    uint32_t poll_ms;    /* until the next status read in a reset */
};

struct hub {
    int used;
    int index;  /* the core's device index */
    int device; /* the controller's handle */
    int interface;
    int protocol;
    int ep_dci; /* the change endpoint, read from phase 4 */
    int state;
    int ports; /* published, at most 15 */
    int lpsm;
    int think_time;
    int multi_tt;
    uint32_t power_good_ms;
    uint32_t wait_ms;
    int at;      /* the port being powered or scanned, numbered from one */
    int request; /* what the transfer in flight is for, or REQ_NONE */
    int request_port;
    uint32_t tag;
    uint32_t request_ms;
    uint8_t setup[8];
    uint8_t data[16];
    struct hub_port port[USB_HUB_PORT_MAX];
};

static struct hub hubs[HUB_MAX];
static uint32_t hub_ms;
static uint32_t tag_seed;

static struct hub *by_index(int index)
{
    int i;

    for (i = 0; i < HUB_MAX; i++) {
        if (hubs[i].used && hubs[i].index == index) {
            return &hubs[i];
        }
    }
    return 0;
}

/** Send one control request on the hub's endpoint zero. */
static int request(struct hub *h, int type, int req, int value, int index, int length, int what,
                   int port)
{
    struct usb_transfer transfer;

    h->setup[0] = (uint8_t)type;
    h->setup[1] = (uint8_t)req;
    h->setup[2] = (uint8_t)(value & 0xFF);
    h->setup[3] = (uint8_t)((value >> 8) & 0xFF);
    h->setup[4] = (uint8_t)(index & 0xFF);
    h->setup[5] = (uint8_t)((index >> 8) & 0xFF);
    h->setup[6] = (uint8_t)(length & 0xFF);
    h->setup[7] = (uint8_t)((length >> 8) & 0xFF);
    transfer.setup = h->setup;
    transfer.buffer = length ? h->data : 0;
    transfer.length = (uint32_t)length;
    transfer.direction = (type & 0x80) ? USB_DIR_IN : USB_DIR_OUT;
    h->tag = ++tag_seed;
    transfer.generation = h->tag;
    if (xhci_submit(h->device, 1, &transfer) != 0) {
        return -1;
    }
    h->request = what;
    h->request_port = port;
    h->request_ms = 0;
    return 0;
}

static int port_status_read(struct hub *h, int port)
{
    return request(h, TYPE_CLASS_PORT_IN, REQ_GET_STATUS, 0, port, 4, REQ_PORT_STATUS, port);
}

static void give_up(struct hub *h, const char *why)
{
    int index = h->index;

    h->used = 0;
    usb_core_hub_failed(index, why);
}

int hub_attach(int index, int device, int interface, int protocol, int ep_dci)
{
    int i;
    struct hub *h = 0;

    for (i = 0; i < HUB_MAX; i++) {
        if (!hubs[i].used) {
            h = &hubs[i];
            break;
        }
    }
    if (h == 0) {
        klog(KLOG_WARN, "usb", "hub: device %d refused: this kernel drives %d hubs at once", device,
             HUB_MAX);
        return -1;
    }
    k_memset(h, 0, sizeof(*h));
    h->used = 1;
    h->index = index;
    h->device = device;
    h->interface = interface;
    h->protocol = protocol;
    h->ep_dci = ep_dci;
    h->multi_tt = protocol == USB_PROTOCOL_HUB_MULTI;
    h->state = HUB_DESCRIPTOR;
    if (request(h, TYPE_CLASS_HUB_IN, REQ_GET_DESCRIPTOR, DESC_HUB << 8, 0, (int)sizeof(h->data),
                REQ_DESCRIPTOR, 0) != 0) {
        h->used = 0;
        klog(KLOG_WARN, "usb", "hub: device %d refused: its descriptor could not be asked for",
             device);
        return -1;
    }
    return 0;
}

void hub_detach(int index)
{
    struct hub *h = by_index(index);

    if (h != 0) {
        h->used = 0;
    }
}

/** What the descriptor said; -1 when it is not a hub descriptor. */
static int take_descriptor(struct hub *h, int length)
{
    int ports;

    if (length < 7 || h->data[1] != DESC_HUB) {
        return -1;
    }
    ports = h->data[2];
    if (ports == 0) {
        return -1;
    }
    if (ports > USB_HUB_PORT_MAX) {
        klog(KLOG_INFO, "usb", "hub: device %d has %d ports; the first %d are used", h->device,
             ports, USB_HUB_PORT_MAX);
        ports = USB_HUB_PORT_MAX;
    }
    h->ports = ports;
    h->lpsm = h->data[3] & 3;
    h->think_time = (h->data[3] >> 5) & 3;
    h->power_good_ms = (uint32_t)h->data[5] * 2u;
    if (h->power_good_ms > HUB_POWER_MAX_MS) {
        h->power_good_ms = HUB_POWER_MAX_MS;
    }
    return 0;
}

/** Start powering the next port, or say there is none left. */
static int power_next(struct hub *h)
{
    if (h->lpsm == LPSM_NONE || h->lpsm > LPSM_INDIVIDUAL) {
        return 0; /* a hub that cannot switch has its ports on already */
    }
    if (h->lpsm == LPSM_GANGED && h->at > 1) {
        return 0; /* one request powered them all */
    }
    if (h->at > h->ports) {
        return 0;
    }
    if (request(h, TYPE_CLASS_PORT_OUT, REQ_SET_FEATURE, PORT_POWER, h->at, 0, REQ_POWER, h->at) !=
        0) {
        give_up(h, "a port could not be powered");
        return 0;
    }
    return 1;
}

/** A port's status has just been read: record it and tell the core. */
static void port_read(struct hub *h, int port)
{
    struct hub_port *p = &h->port[port - 1];
    uint16_t status = (uint16_t)(h->data[0] | (h->data[1] << 8));
    uint16_t change = (uint16_t)(h->data[2] | (h->data[3] << 8));
    int was = p->known && (p->status & PORT_STAT_CONNECTION);
    int now = (status & PORT_STAT_CONNECTION) != 0;

    p->status = status;
    p->change = change;
    if (!p->known || was != now) {
        p->connect_ms = hub_ms;
    }
    p->known = 1;
    if (h->state == HUB_SCAN && now) {
        usb_core_hub_port(h->index, port - 1, 1);
    }
}

/** The answer to the request in flight. */
static void answered(struct hub *h, const struct usb_completion *c)
{
    int ok = c->status == USB_XFER_OK || c->status == USB_XFER_SHORT;
    int got = (int)(c->length - c->residual);
    int what = h->request;
    int port = h->request_port;

    h->request = REQ_NONE;
    switch (what) {
    case REQ_DESCRIPTOR:
        if (!ok || take_descriptor(h, got) != 0) {
            give_up(h, "it did not give a hub descriptor");
            return;
        }
        h->state = HUB_TELL;
        return;
    case REQ_POWER:
        if (!ok) {
            give_up(h, "it would not power a port");
            return;
        }
        h->at++;
        if (!power_next(h)) {
            h->state = HUB_POWER_WAIT;
            h->wait_ms = h->power_good_ms;
        }
        return;
    case REQ_PORT_STATUS:
        if (!ok || got < 4) {
            if (h->state == HUB_SCAN) {
                give_up(h, "it would not say what is on a port");
                return;
            }
            if (h->port[port - 1].reset == RESET_POLL) {
                h->port[port - 1].reset = RESET_FAILED;
            }
            return;
        }
        port_read(h, port);
        if (h->state == HUB_SCAN) {
            h->at++;
            if (h->at > h->ports) {
                h->state = HUB_RUNNING;
                klog(KLOG_INFO, "usb", "hub: device %d: %d ports, power mode %d, think time %d",
                     h->device, h->ports, h->lpsm, h->think_time);
            } else if (port_status_read(h, h->at) != 0) {
                give_up(h, "a port's status could not be asked for");
            }
            return;
        }
        if (h->port[port - 1].reset == RESET_POLL) {
            struct hub_port *p = &h->port[port - 1];

            if ((p->status & PORT_STAT_RESET) == 0) {
                if ((p->status & PORT_STAT_ENABLE) == 0 ||
                    (p->status & PORT_STAT_CONNECTION) == 0) {
                    p->reset = RESET_FAILED;
                    return;
                }
                if (request(h, TYPE_CLASS_PORT_OUT, REQ_CLEAR_FEATURE, C_PORT_RESET, port, 0,
                            REQ_CLEAR, port) != 0) {
                    p->reset = RESET_FAILED;
                    return;
                }
                p->reset = RESET_CLEAR;
            } else {
                p->poll_ms = HUB_RESET_POLL_MS;
            }
        }
        return;
    case REQ_PORT_RESET:
        if (!ok) {
            h->port[port - 1].reset = RESET_FAILED;
            return;
        }
        h->port[port - 1].reset = RESET_POLL;
        h->port[port - 1].poll_ms = HUB_RESET_POLL_MS;
        return;
    case REQ_CLEAR:
        if (h->port[port - 1].reset == RESET_CLEAR) {
            h->port[port - 1].reset = ok ? RESET_DONE : RESET_FAILED;
        }
        return;
    default:
        return;
    }
}

int hub_completion(const struct usb_completion *completion)
{
    int i;

    for (i = 0; i < HUB_MAX; i++) {
        struct hub *h = &hubs[i];

        if (!h->used || h->device != completion->device) {
            continue;
        }
        if (completion->endpoint != 1 || h->request == REQ_NONE) {
            return 0;
        }
        if (completion->generation != h->tag) {
            return 1; /* an answer to a request given up on */
        }
        answered(h, completion);
        return 1;
    }
    return 0;
}

/** One pass for one hub. */
static void step_hub(struct hub *h, uint32_t elapsed_ms)
{
    int i;

    if (h->request != REQ_NONE) {
        h->request_ms += elapsed_ms;
        if (h->request_ms >= HUB_REQUEST_MS) {
            give_up(h, "it stopped answering");
        }
        return;
    }
    switch (h->state) {
    case HUB_TELL: {
        int answer = usb_core_hub_tell(h->index, h->ports, h->multi_tt, h->think_time);

        if (answer < 0) {
            give_up(h, "the controller would not take it as a hub");
            return;
        }
        if (answer == 0) {
            return;
        }
        h->state = HUB_POWER;
        h->at = 1;
        if (!power_next(h)) {
            h->state = HUB_POWER_WAIT;
            h->wait_ms = h->power_good_ms;
        }
        return;
    }
    case HUB_POWER_WAIT:
        h->wait_ms = (h->wait_ms > elapsed_ms) ? h->wait_ms - elapsed_ms : 0;
        if (h->wait_ms != 0) {
            return;
        }
        h->state = HUB_SCAN;
        h->at = 1;
        if (port_status_read(h, 1) != 0) {
            give_up(h, "a port's status could not be asked for");
        }
        return;
    case HUB_RUNNING:
        /* The resets the core asked for, one request at a time. */
        for (i = 0; i < h->ports; i++) {
            struct hub_port *p = &h->port[i];

            if (p->reset == RESET_NONE || p->reset == RESET_DONE || p->reset == RESET_FAILED) {
                continue;
            }
            p->reset_ms += elapsed_ms;
            if (p->reset_ms >= HUB_RESET_MS) {
                p->reset = RESET_FAILED;
                continue;
            }
            if (p->reset == RESET_ASKED) {
                if (request(h, TYPE_CLASS_PORT_OUT, REQ_SET_FEATURE, PORT_RESET, i + 1, 0,
                            REQ_PORT_RESET, i + 1) != 0) {
                    p->reset = RESET_FAILED;
                    continue;
                }
                p->reset = RESET_SENT;
                return;
            }
            if (p->reset == RESET_POLL) {
                p->poll_ms = (p->poll_ms > elapsed_ms) ? p->poll_ms - elapsed_ms : 0;
                if (p->poll_ms == 0) {
                    if (port_status_read(h, i + 1) != 0) {
                        p->reset = RESET_FAILED;
                        continue;
                    }
                    return;
                }
            }
        }
        return;
    default:
        return;
    }
}

void hub_step(uint32_t elapsed_ms)
{
    int i;

    hub_ms += elapsed_ms;
    for (i = 0; i < HUB_MAX; i++) {
        if (hubs[i].used) {
            step_hub(&hubs[i], elapsed_ms);
        }
    }
}

int hub_port_status(int owner, int port, struct usb_port_status *out)
{
    struct hub *h = by_index(owner);
    const struct hub_port *p;

    out->connected = 0;
    out->enabled = 0;
    out->changed = 0;
    out->speed = USB_SPEED_FULL;
    if (h == 0 || port < 0 || port >= h->ports) {
        return -1;
    }
    p = &h->port[port];
    if (!p->known) {
        return -1;
    }
    out->connected = (p->status & PORT_STAT_CONNECTION) != 0;
    out->enabled = (p->status & PORT_STAT_ENABLE) != 0;
    out->changed = p->change != 0;
    out->speed = (p->status & PORT_STAT_LOW_SPEED)    ? USB_SPEED_LOW
                 : (p->status & PORT_STAT_HIGH_SPEED) ? USB_SPEED_HIGH
                                                      : USB_SPEED_FULL;
    return 0;
}

int hub_port_settled(int owner, int port)
{
    struct hub *h = by_index(owner);
    const struct hub_port *p;

    if (h == 0 || port < 0 || port >= h->ports) {
        return 0;
    }
    p = &h->port[port];
    return p->known && (p->status & PORT_STAT_CONNECTION) != 0 &&
           hub_ms - p->connect_ms >= HUB_DEBOUNCE_MS;
}

int hub_port_reset_begin(int owner, int port)
{
    struct hub *h = by_index(owner);

    if (h == 0 || port < 0 || port >= h->ports || h->state != HUB_RUNNING) {
        return -1;
    }
    h->port[port].reset = RESET_ASKED;
    h->port[port].reset_ms = 0;
    return 0;
}

int hub_port_reset_done(int owner, int port)
{
    struct hub *h = by_index(owner);

    if (h == 0 || port < 0 || port >= h->ports) {
        return -1;
    }
    if (h->port[port].reset == RESET_DONE) {
        h->port[port].reset = RESET_NONE;
        return 1;
    }
    if (h->port[port].reset == RESET_FAILED) {
        h->port[port].reset = RESET_NONE;
        return -1;
    }
    return 0;
}
