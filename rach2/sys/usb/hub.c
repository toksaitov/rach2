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
#define REQ_SET_INTERFACE   11
#define TYPE_STD_INTERFACE  0x01

/* The hub's own status word and change word (Table 11-19 and 11-20), and
 * the two hub features (Table 11-17). */
#define HUB_STAT_LOCAL_POWER  0x0001
#define HUB_STAT_OVER_CURRENT 0x0002
#define C_HUB_LOCAL_POWER     0
#define C_HUB_OVER_CURRENT    1

/* The port feature selectors (Table 11-17). */
#define PORT_RESET          4
#define PORT_POWER          8
#define C_PORT_CONNECTION   16
#define C_PORT_ENABLE       17
#define C_PORT_RESET        20
#define C_PORT_SUSPEND      18
#define C_PORT_OVER_CURRENT 19

/* The port status word (Table 11-21). */
#define PORT_STAT_CONNECTION   0x0001
#define PORT_STAT_ENABLE       0x0002
#define PORT_STAT_OVER_CURRENT 0x0008
#define PORT_STAT_RESET        0x0010
#define PORT_STAT_POWER        0x0100
#define PORT_STAT_LOW_SPEED    0x0200
#define PORT_STAT_HIGH_SPEED   0x0400

/* The power switching modes of wHubCharacteristics (Table 11-13). */
#define LPSM_GANGED     0
#define LPSM_INDIVIDUAL 1
#define LPSM_NONE       2

/* How long a hub has for any one request, how long a connection must hold
 * before it is settled, how often a port in reset is asked, and how long a
 * reset may take, all in milliseconds. The debounce and the reset floor are
 * the specification's; the reset ceiling is this driver's. */
#define HUB_REQUEST_MS            500u
#define HUB_DEBOUNCE_MS           100u
#define HUB_RESET_POLL_MS         20u
#define HUB_RESET_MS              500u
#define HUB_POWER_MAX_MS          500u
#define HUB_OVER_CURRENT_RETRY_MS 1000u

/* What the hub as a whole is doing. */
#define HUB_DESCRIPTOR 1 /* its descriptor asked for */
#define HUB_TELL       2 /* the controller being told it is a hub */
#define HUB_POWER      3 /* its ports being powered */
#define HUB_POWER_WAIT 4 /* the power-good time */
#define HUB_SCAN       5 /* each port's status read once */
#define HUB_RUNNING    6
#define HUB_SETTING    7 /* a multi-TT hub's setting selected */

/* What a port is doing, for the core's reset. */
#define RESET_NONE   0
#define RESET_ASKED  1 /* the core asked; the request has not gone out */
#define RESET_SENT   2 /* SET_PORT_FEATURE(PORT_RESET) out */
#define RESET_POLL   3 /* the status being read until the reset ends */
#define RESET_CLEAR  4 /* C_PORT_RESET being cleared */
#define RESET_DONE   5
#define RESET_FAILED 6

/* What the request in flight was for. */
#define REQ_NONE         0
#define REQ_DESCRIPTOR   1
#define REQ_POWER        2
#define REQ_PORT_STATUS  3
#define REQ_PORT_RESET   4
#define REQ_CLEAR        5
#define REQ_SETTING      6
#define REQ_HUB_STATUS   7
#define REQ_HUB_CLEAR    8
#define REQ_CHANGE_CLEAR 9 /* one port change bit acknowledged */
#define REQ_POWER_OFF    10
#define REQ_POWER_ON     11

/* The change bits as bits of the change word, and one more of this driver's
 * own for a power-off owed to an over-current. */
#define C_PORT_CONNECTION_BIT   0x0001
#define C_PORT_ENABLE_BIT       0x0002
#define C_PORT_SUSPEND_BIT      0x0004
#define C_PORT_OVER_CURRENT_BIT 0x0008
#define C_PORT_RESET_BIT        0x0010
#define POWER_OFF_BIT           0x8000

struct hub_port {
    uint16_t status;
    uint16_t change;
    int known;              /* the status has been read at least once */
    uint32_t connect_ms;    /* when the connection was first seen */
    int reset;              /* one of the RESET_ values */
    uint32_t reset_ms;      /* how long the reset has run */
    uint32_t poll_ms;       /* until the next status read in a reset */
    uint16_t to_clear;      /* the change bits acknowledged one by one */
    int over_current_tries; /* how many times the port was powered on again */
    uint32_t power_on_ms;   /* until the port is powered on again, or 0 */
    int off;                /* left powered off after over-current recurred */
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
    /* The change endpoint: the transfer outstanding on it, the bitmap it
     * answers with, and the ports whose bits arrived and are not yet dealt
     * with, bit 0 being the hub itself. */
    int change_out;
    uint32_t change_tag;
    uint8_t bitmap[2];
    uint32_t changes;
    int change_reading; /* the port whose status is being read for a change */
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

/** Keep a transfer on the change endpoint; its bitmap is the hub's report. */
static int change_submit(struct hub *h)
{
    struct usb_transfer transfer;
    uint32_t length = (uint32_t)(h->ports + 1 + 7) / 8u;

    transfer.setup = 0;
    transfer.buffer = h->bitmap;
    transfer.length = length > sizeof(h->bitmap) ? (uint32_t)sizeof(h->bitmap) : length;
    transfer.direction = USB_DIR_IN;
    h->change_tag = ++tag_seed;
    transfer.generation = h->change_tag;
    if (xhci_submit(h->device, h->ep_dci, &transfer) != 0) {
        return -1;
    }
    h->change_out = 1;
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

/** Answer an over-current on a port: power it off and close what is
 * below, then power it on again once after a second, and leave it off with
 * a record when it recurs. Called from the first scan when the status
 * already shows the fault, and from a status read the change endpoint
 * prompted. */
static void over_current(struct hub *h, int port)
{
    struct hub_port *p = &h->port[port - 1];

    klog(KLOG_WARN, "usb", "hub: device %d port %d: over-current; powered off", h->device, port);
    usb_core_hub_port(h->index, port - 1, 0);
    if (p->over_current_tries == 0) {
        p->over_current_tries = 1;
        p->power_on_ms = HUB_OVER_CURRENT_RETRY_MS;
    } else {
        p->off = 1;
        klog(KLOG_WARN, "usb", "hub: device %d port %d: over-current again; left off", h->device,
             port);
    }
    p->to_clear |= POWER_OFF_BIT;
}

/**
 * A port's status has just been read: record it, and for a change reported
 * on the change endpoint, act on each change bit and acknowledge it.
 *
 * A connection change is a change of identity whatever the status says
 * now: the core closes what was below the port before a device that is
 * there is looked at. An enable lost is recorded and the device below
 * closed; the core enumerates again if the connection is still there. An
 * over-current, found by the first scan or reported later, is answered by
 * `over_current`.
 */
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
    if (h->state == HUB_SCAN) {
        /* The change bits found by the first scan are acknowledged before
         * the change endpoint is read, or the hub would report the same
         * connections a second time and each device would arrive twice. */
        p->to_clear = change & (C_PORT_CONNECTION_BIT | C_PORT_ENABLE_BIT | C_PORT_SUSPEND_BIT |
                                C_PORT_OVER_CURRENT_BIT | C_PORT_RESET_BIT);
        if (status & PORT_STAT_OVER_CURRENT) {
            over_current(h, port);
        } else if (now) {
            usb_core_hub_port(h->index, port - 1, 1);
        }
        return;
    }
    if (h->change_reading != port) {
        return; /* a read for a reset, which its own arm judges */
    }
    h->change_reading = 0;
    p->to_clear = change & (C_PORT_CONNECTION_BIT | C_PORT_ENABLE_BIT | C_PORT_SUSPEND_BIT |
                            C_PORT_OVER_CURRENT_BIT | C_PORT_RESET_BIT);
    if (change & C_PORT_OVER_CURRENT_BIT) {
        if (status & PORT_STAT_OVER_CURRENT) {
            over_current(h, port);
        }
    } else if (change & C_PORT_CONNECTION_BIT) {
        p->connect_ms = hub_ms;
        usb_core_hub_port(h->index, port - 1, now);
    } else if ((change & C_PORT_ENABLE_BIT) && !(status & PORT_STAT_ENABLE) && now &&
               p->reset == RESET_NONE) {
        klog(KLOG_WARN, "usb", "hub: device %d port %d: enable lost", h->device, port);
        usb_core_hub_port(h->index, port - 1, 0);
        p->connect_ms = hub_ms;
        usb_core_hub_port(h->index, port - 1, 1);
    }
}

/** Acknowledge the next change bit owed on a port; 1 when a request went
 * out. */
static int clear_next(struct hub *h, int port)
{
    struct hub_port *p = &h->port[port - 1];
    int feature;

    if (p->to_clear & POWER_OFF_BIT) {
        p->to_clear &= (uint16_t)~POWER_OFF_BIT;
        return request(h, TYPE_CLASS_PORT_OUT, REQ_CLEAR_FEATURE, PORT_POWER, port, 0,
                       REQ_POWER_OFF, port) == 0;
    }
    if (p->to_clear & C_PORT_CONNECTION_BIT) {
        feature = C_PORT_CONNECTION;
        p->to_clear &= (uint16_t)~C_PORT_CONNECTION_BIT;
    } else if (p->to_clear & C_PORT_ENABLE_BIT) {
        feature = C_PORT_ENABLE;
        p->to_clear &= (uint16_t)~C_PORT_ENABLE_BIT;
    } else if (p->to_clear & C_PORT_SUSPEND_BIT) {
        feature = C_PORT_SUSPEND;
        p->to_clear &= (uint16_t)~C_PORT_SUSPEND_BIT;
    } else if (p->to_clear & C_PORT_OVER_CURRENT_BIT) {
        feature = C_PORT_OVER_CURRENT;
        p->to_clear &= (uint16_t)~C_PORT_OVER_CURRENT_BIT;
    } else if (p->to_clear & C_PORT_RESET_BIT) {
        feature = C_PORT_RESET;
        p->to_clear &= (uint16_t)~C_PORT_RESET_BIT;
    } else {
        return 0;
    }
    return request(h, TYPE_CLASS_PORT_OUT, REQ_CLEAR_FEATURE, feature, port, 0, REQ_CHANGE_CLEAR,
                   port) == 0;
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
        if (h->multi_tt) {
            /* One translator per port is the interface's alternate setting
             * 1; a hub that refuses it is driven with one translator. */
            if (request(h, TYPE_STD_INTERFACE, REQ_SET_INTERFACE, 1, h->interface, 0, REQ_SETTING,
                        0) != 0) {
                h->multi_tt = 0;
                h->state = HUB_TELL;
                return;
            }
            h->state = HUB_SETTING;
            return;
        }
        h->state = HUB_TELL;
        return;
    case REQ_SETTING:
        if (!ok) {
            klog(KLOG_INFO, "usb", "hub: device %d refused a translator per port; one is used",
                 h->device);
            h->multi_tt = 0;
        }
        h->state = HUB_TELL;
        return;
    case REQ_HUB_STATUS:
        if (ok && got >= 4) {
            uint16_t status = (uint16_t)(h->data[0] | (h->data[1] << 8));
            uint16_t change = (uint16_t)(h->data[2] | (h->data[3] << 8));

            if (change & HUB_STAT_LOCAL_POWER) {
                klog(KLOG_WARN, "usb", "hub: device %d: local power %s", h->device,
                     (status & HUB_STAT_LOCAL_POWER) ? "lost" : "back");
                (void)request(h, TYPE_CLASS_HUB_OUT, REQ_CLEAR_FEATURE, C_HUB_LOCAL_POWER, 0, 0,
                              REQ_HUB_CLEAR, 0);
                return;
            }
            if (change & HUB_STAT_OVER_CURRENT) {
                klog(KLOG_WARN, "usb", "hub: device %d: over-current %s", h->device,
                     (status & HUB_STAT_OVER_CURRENT) ? "reported" : "cleared");
                (void)request(h, TYPE_CLASS_HUB_OUT, REQ_CLEAR_FEATURE, C_HUB_OVER_CURRENT, 0, 0,
                              REQ_HUB_CLEAR, 0);
                return;
            }
        }
        return;
    case REQ_POWER_ON:
        if (!ok) {
            /* The one retry after an over-current could not be made; the
             * port stays off, as it would after a second over-current. */
            h->port[port - 1].off = 1;
            klog(KLOG_WARN, "usb", "hub: device %d port %d: could not be powered on again; left off",
                 h->device, port);
        }
        return;
    case REQ_HUB_CLEAR:
    case REQ_POWER_OFF:
    case REQ_CHANGE_CLEAR:
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
                /* The running arm reads the change endpoint once every
                 * change bit the scan found has been acknowledged. */
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
        if (completion->endpoint == h->ep_dci) {
            if (completion->generation != h->change_tag) {
                return 1;
            }
            h->change_out = 0;
            if (completion->status != USB_XFER_OK && completion->status != USB_XFER_SHORT) {
                give_up(h, "its change endpoint stopped answering");
                return 1;
            }
            h->changes |= (uint32_t)h->bitmap[0] | ((uint32_t)h->bitmap[1] << 8);
            return 1;
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
        /* A change bit owed an acknowledgement, and a port powered off for
         * over-current owed its power back. */
        for (i = 0; i < h->ports; i++) {
            struct hub_port *p = &h->port[i];

            if (p->to_clear != 0) {
                if (clear_next(h, i + 1)) {
                    return;
                }
            }
            if (p->power_on_ms != 0) {
                p->power_on_ms = (p->power_on_ms > elapsed_ms) ? p->power_on_ms - elapsed_ms : 0;
                if (p->power_on_ms == 0) {
                    if (request(h, TYPE_CLASS_PORT_OUT, REQ_SET_FEATURE, PORT_POWER, i + 1, 0,
                                REQ_POWER_ON, i + 1) == 0) {
                        return;
                    }
                }
            }
        }
        /* The changes the bitmap reported: the hub's own status for bit 0,
         * a port's status for its bit, one read at a time. */
        if (h->changes & 1u) {
            h->changes &= ~1u;
            if (request(h, TYPE_CLASS_HUB_IN, REQ_GET_STATUS, 0, 0, 4, REQ_HUB_STATUS, 0) == 0) {
                return;
            }
        }
        for (i = 0; i < h->ports; i++) {
            if (h->changes & ((uint32_t)1u << (i + 1))) {
                h->changes &= ~((uint32_t)1u << (i + 1));
                h->change_reading = i + 1;
                if (port_status_read(h, i + 1) == 0) {
                    return;
                }
                h->change_reading = 0;
            }
        }
        /* The report itself, out again once the last answer is acted on. */
        if (!h->change_out && change_submit(h) != 0) {
            give_up(h, "its change endpoint could not be read");
            return;
        }
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
    out->connected = (p->status & PORT_STAT_CONNECTION) != 0 && !p->off && p->power_on_ms == 0;
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
