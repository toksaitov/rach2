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
 * A SuperSpeed hub is the same machine with the USB 3 differences. It is
 * known by its speed, since the specification puts its protocol 3 in the
 * device descriptor and leaves the interface's at zero: its descriptor is
 * the USB 3 one, it is told its depth in the tree before its ports are
 * used, its port status words have no speed bits because everything below
 * it is SuperSpeed and no translator because nothing below it is slow, its
 * ports report three changes USB 2 has no feature numbers for, and a port
 * whose link has fallen to SS.Inactive is given a warm reset and what is on
 * it enumerated again. The USB 3.0 hub in the notebook's blue socket is two
 * devices, this one and a high-speed hub for the USB 2 devices.
 *
 * Everything the hub is asked is a control transfer on its endpoint zero,
 * one at a time per hub, stepped from the pump. Nothing here waits.
 */

#include "hub.h"

#include "../klog.h"
#include "../lib/kstring.h"
#include "../mm/paging.h"
#include "core.h"
#include "hcd.h"
#include "xhci.h"
#include "../cmdline.h"

/* `rach2.hubslow=MS`: the scan waits this long between two ports, so a
 * device found early settles while the scan goes on, which is what a slow
 * machine does on its own; 0 is the ordinary scan. Read once. */
static uint32_t hub_slow_ms;
static int hub_slow_known;

static uint32_t hub_slow(void)
{
    char value[16];
    int i;

    if (!hub_slow_known) {
        hub_slow_known = 1;
        if (kcmdline_value("rach2.hubslow", value, sizeof(value))) {
            for (i = 0; value[i] >= '0' && value[i] <= '9' && hub_slow_ms < 100000u; i++) {
                hub_slow_ms = hub_slow_ms * 10u + (uint32_t)(value[i] - '0');
            }
        }
    }
    return hub_slow_ms;
}

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
#define DESC_HUB_SS         0x2A /* the USB 3 hub descriptor */
#define REQ_SET_HUB_DEPTH   12   /* USB 3: the hub's tier, before its ports are used */
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
#define BH_PORT_RESET       28 /* USB 3: a warm reset, for a link that fell inactive */
#define PORT_POWER          8
#define C_PORT_CONNECTION   16
#define C_PORT_ENABLE       17
#define C_PORT_RESET        20
#define C_PORT_SUSPEND      18
#define C_PORT_OVER_CURRENT 19
#define C_PORT_LINK_STATE   25 /* USB 3 */
#define C_PORT_CONFIG_ERROR 26 /* USB 3 */
#define C_BH_PORT_RESET     29 /* USB 3: a warm reset completed */

/* The port status word (Table 11-21). */
#define PORT_STAT_CONNECTION    0x0001
#define PORT_STAT_ENABLE        0x0002
#define PORT_STAT_OVER_CURRENT  0x0008
#define PORT_STAT_RESET         0x0010
#define PORT_STAT_POWER         0x0100
#define PORT_STAT_LOW_SPEED     0x0200
#define PORT_STAT_HIGH_SPEED    0x0400
/* USB 3: bits 5 to 8 of the status word are the link state. */
#define PORT_LINK_STATE(status) (((status) >> 5) & 0xFu)
#define LINK_SS_INACTIVE        6

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
#define HUB_POWER_MAX_MS          510u /* the descriptor's byte of 2 ms units at 255 */
/* A hub may say its power is good in 0 ms; a port read that soon after its
 * power request reports nothing connected. 100 ms is the floor Linux uses. */
#define HUB_POWER_MIN_MS          100u
#define HUB_OVER_CURRENT_RETRY_MS 1000u

/* What the hub as a whole is doing. */
#define HUB_DESCRIPTOR 1 /* its descriptor asked for */
#define HUB_TELL       2 /* the controller being told it is a hub */
#define HUB_POWER      3 /* its ports being powered */
#define HUB_POWER_WAIT 4 /* the power-good time */
#define HUB_SCAN       5 /* each port's status read once */
#define HUB_RUNNING    6
#define HUB_SCAN_WAIT  7 /* `rach2.hubslow`: between two ports of the scan */
#define HUB_SETTING    7 /* a multi-TT hub's setting selected */
#define HUB_DEPTH      8 /* a SuperSpeed hub being told its depth */

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
#define REQ_DEPTH        12
#define REQ_WARM_RESET   13

/* The change bits as bits of the change word, and one more of this driver's
 * own for a power-off owed to an over-current. */
#define C_PORT_CONNECTION_BIT   0x0001
#define C_PORT_ENABLE_BIT       0x0002
#define C_PORT_SUSPEND_BIT      0x0004
#define C_PORT_OVER_CURRENT_BIT 0x0008
#define C_PORT_RESET_BIT        0x0010
#define C_BH_PORT_RESET_BIT     0x0020 /* USB 3 */
#define C_PORT_LINK_STATE_BIT   0x0040 /* USB 3 */
#define C_PORT_CONFIG_ERROR_BIT 0x0080 /* USB 3 */
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
    int recheck;            /* the status read again after an acknowledgement */
    int over_current_tries; /* how many times the port was powered on again */
    uint32_t power_on_ms;   /* until the port is powered on again, or 0 */
    int off;                /* left powered off after over-current recurred */
    int warm;               /* a warm reset asked for after a link error */
    int warm_pending;       /* waiting for earlier change bits to be cleared */
    int publish;            /* connected at the scan; told to the core when it ends */
};

struct hub {
    int used;
    struct xhci_ctrl *hc; /* the controller this hub is on */
    int index;            /* the core's device index */
    int device;           /* the controller's handle */
    int interface;
    int protocol;
    int ep_dci; /* the change endpoint, read from phase 4 */
    int state;
    int ports; /* published, at most 15 */
    int lpsm;
    int think_time;
    int multi_tt;
    int super; /* a SuperSpeed hub: the USB 3 descriptor, depth and status words */
    int depth; /* its tier below the root port, told to a SuperSpeed hub */
    uint32_t power_good_ms;
    int wait_fresh; /* the wait was set this tick; the tick is not counted */
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
    /* One bit per port plus the hub's own, so a hub with more ports than
     * a byte holds still reports every one of them. */
    uint8_t bitmap[(USB_HUB_PORT_MAX + 8) / 8];
    uint32_t changes;
    int change_reading; /* the port whose status is being read for a change */
    struct hub_port port[USB_HUB_PORT_MAX];
};

/*
 * One record per hub, each its own frame from the identity-mapped window.
 * The record holds the buffers the controller writes a hub's descriptor and
 * its port-change bitmap into, and a controller is given an address to write
 * to rather than a pointer this kernel reaches it by.
 */
static struct hub *hubs[HUB_MAX];

/* The record is a frame, so it must fit one. */
typedef char hub_record_fits_a_frame[(sizeof(struct hub) <= 4096) ? 1 : -1];

/** The record of hub `i`, or a spare one that is always free. */
static struct hub *hub_at(int i)
{
    static struct hub none;

    return hubs[i] ? hubs[i] : &none;
}

/** Take a frame for hub `i`'s record, or say there is none. */
static struct hub *hub_take(int i)
{
    void *frame;

    if (hubs[i]) {
        return hubs[i];
    }
    frame = alloc_kernel_frame();
    if (!frame) {
        return 0;
    }
    hubs[i] = (struct hub *)frame;
    k_memset(hubs[i], 0, sizeof(*hubs[i]));
    return hubs[i];
}

/** Give back hub `i`'s frame. */
static void hub_release(int i)
{
    if (hubs[i]) {
        free_kernel_frame(hubs[i]);
        hubs[i] = 0;
    }
}
static uint32_t hub_ms;
static uint32_t tag_seed;

static struct hub *by_index(int index)
{
    int i;

    for (i = 0; i < HUB_MAX; i++) {
        if (hub_at(i)->used && hub_at(i)->index == index) {
            return hub_at(i);
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
    if (xhci_submit(h->hc, h->device, 1, &transfer) != 0) {
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
    if (xhci_submit(h->hc, h->device, h->ep_dci, &transfer) != 0) {
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

int hub_attach(struct xhci_ctrl *hc, int index, int device, int interface, int protocol, int ep_dci,
               int depth, int super)
{
    int i;
    struct hub *h = 0;

    for (i = 0; i < HUB_MAX; i++) {
        if (!hubs[i]) {
            h = hub_take(i);
            if (!h) {
                klog(KLOG_WARN, "usb", "hub: device %d refused: no frame for its record", device);
                return -1;
            }
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
    h->hc = hc;
    h->index = index;
    h->device = device;
    h->interface = interface;
    h->protocol = protocol;
    h->ep_dci = ep_dci;
    h->multi_tt = protocol == USB_PROTOCOL_HUB_MULTI;
    h->super = super;
    h->depth = depth;
    h->state = HUB_DESCRIPTOR;
    if (request(h, TYPE_CLASS_HUB_IN, REQ_GET_DESCRIPTOR, (h->super ? DESC_HUB_SS : DESC_HUB) << 8,
                0, (int)sizeof(h->data), REQ_DESCRIPTOR, 0) != 0) {
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

/** What the descriptor said; -1 when it is not the hub descriptor asked
 * for. The USB 3 descriptor keeps the port count, the characteristics and
 * the power-good time at the USB 2 offsets, and has no think time. */
static int take_descriptor(struct hub *h, int length)
{
    int ports;

    if (length < 7 || h->data[1] != (h->super ? DESC_HUB_SS : DESC_HUB)) {
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
    h->think_time = h->super ? 0 : (h->data[3] >> 5) & 3;
    h->power_good_ms = (uint32_t)h->data[5] * 2u;
    if (h->power_good_ms < HUB_POWER_MIN_MS) {
        h->power_good_ms = HUB_POWER_MIN_MS;
    }
    if (h->power_good_ms > HUB_POWER_MAX_MS) {
        h->power_good_ms = HUB_POWER_MAX_MS;
    }
    return 0;
}

/**
 * Start powering the next port, or say there is none left.
 *
 * Every port gets its own request whether the descriptor says the ports
 * switch together or one by one. On a ganged hub one request energises the
 * gang, but the specification (11.11.1) leaves every other port in its
 * Powered-off state until that port gets a request of its own, and the
 * owner's hub reported exactly that: after one request, port 1 powered and
 * ports 2 to 4 not.
 */
static int power_next(struct hub *h)
{
    if (h->lpsm == LPSM_NONE || h->lpsm > LPSM_INDIVIDUAL) {
        return 0; /* a hub that cannot switch has its ports on already */
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

/** Return the port change bits acknowledged for this hub's protocol. */
static uint16_t change_mask(const struct hub *h)
{
    if (h->super) {
        return C_PORT_CONNECTION_BIT | C_PORT_OVER_CURRENT_BIT | C_PORT_RESET_BIT |
               C_BH_PORT_RESET_BIT | C_PORT_LINK_STATE_BIT | C_PORT_CONFIG_ERROR_BIT;
    }
    return C_PORT_CONNECTION_BIT | C_PORT_ENABLE_BIT | C_PORT_SUSPEND_BIT |
           C_PORT_OVER_CURRENT_BIT | C_PORT_RESET_BIT;
}

/**
 * Record port status and process its reported changes.
 *
 * A connection change closes the previous device even when the port
 * remains connected. Process concurrent changes before acknowledging
 * their bits.
 *
 * The first scan publishes connections after all ports have been read.
 * Later reads close an old connection before power or link recovery.
 * Power-off prevents enumeration and warm resets. A completed warm reset
 * is processed before a new link fault. A new warm reset waits until the
 * old change bits have been cleared, so their acknowledgement cannot clear
 * its completion. A connection change cancels the previous reset.
 */
/**
 * Act on a port's link and enable changes, and start the device below.
 *
 * A link that went inactive on a SuperSpeed hub is warm reset; an enable lost
 * on a lower-speed hub closes the device below and starts the connect window
 * again. A connection change, or that lost enable, then starts enumeration
 * unless a warm reset is already in flight.
 */
static void port_link_change(struct hub *h, int port, uint16_t status, uint16_t change, int now)
{
    struct hub_port *p = &h->port[port - 1];
    int enable_lost = !h->super && (change & C_PORT_ENABLE_BIT) && !(status & PORT_STAT_ENABLE) &&
                      now && p->reset == RESET_NONE;

    if (h->super && (change & (C_PORT_LINK_STATE_BIT | C_PORT_CONFIG_ERROR_BIT)) && now &&
        PORT_LINK_STATE(status) == LINK_SS_INACTIVE && !p->warm) {
        klog(KLOG_WARN, "usb", "hub: device %d port %d: link inactive; warm reset", h->device,
             port);
        usb_core_hub_port(h->index, port - 1, 0);
        p->warm = 1;
        p->warm_pending = 1;
    }
    if (enable_lost) {
        klog(KLOG_WARN, "usb", "hub: device %d port %d: enable lost", h->device, port);
        usb_core_hub_port(h->index, port - 1, 0);
        p->connect_ms = hub_ms;
    }
    if (now && !p->warm && ((change & C_PORT_CONNECTION_BIT) || enable_lost)) {
        usb_core_hub_port(h->index, port - 1, 1);
    }
}

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
        p->to_clear = change & change_mask(h);
        if (status & PORT_STAT_OVER_CURRENT) {
            over_current(h, port);
        } else if (now) {
            p->publish = 1;
        }
        return;
    }
    if (h->change_reading != port) {
        return;
    }
    h->change_reading = 0;
    p->recheck = 0;
    p->to_clear = change & change_mask(h);
    if (change & C_PORT_CONNECTION_BIT) {
        p->warm = 0;
        p->warm_pending = 0;
        p->connect_ms = hub_ms;
        usb_core_hub_port(h->index, port - 1, 0);
    }
    if (h->super && p->warm && !p->warm_pending && (change & C_BH_PORT_RESET_BIT)) {
        p->warm = 0;
        p->connect_ms = hub_ms;
    }
    if ((change & C_PORT_OVER_CURRENT_BIT) && (status & PORT_STAT_OVER_CURRENT)) {
        p->warm = 0;
        p->warm_pending = 0;
        over_current(h, port);
    }
    if (p->off || p->power_on_ms != 0) {
        return;
    }
    port_link_change(h, port, status, change, now);
}

/**
 * Acknowledge the next change bit owed on a port; 1 when a request went
 * out.
 *
 * The hub clears the latch this request names at the time the request
 * arrives, and the bit was chosen from a status read that came earlier. A
 * change the port latched between the two is cleared unread, and the hub
 * then leaves the port out of its change bitmap: a device that arrived in
 * that time would stay plugged in and unknown. The first scan is where the
 * two are furthest apart, since the bits it records are acknowledged only
 * once every port has been read. So a port whose change bits are
 * acknowledged is read once more, and the core's pass over the hub's ports
 * acts on the record that read leaves.
 *
 * That read is the only thing left that can find such a change: the latch
 * is clear, and the hub leaves the port out of its bitmap, so nothing asks
 * again. A hub that answers it with an error or with fewer than four bytes
 * is closed with everything below it, since a hub kept running on an answer
 * that says nothing would leave a device plugged in and unknown.
 */
static int clear_next(struct hub *h, int port)
{
    struct hub_port *p = &h->port[port - 1];
    int feature;

    if (p->to_clear & POWER_OFF_BIT) {
        p->to_clear = (uint16_t)(p->to_clear & ~POWER_OFF_BIT);
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
    } else if (p->to_clear & C_BH_PORT_RESET_BIT) {
        feature = C_BH_PORT_RESET;
        p->to_clear &= (uint16_t)~C_BH_PORT_RESET_BIT;
    } else if (p->to_clear & C_PORT_LINK_STATE_BIT) {
        feature = C_PORT_LINK_STATE;
        p->to_clear &= (uint16_t)~C_PORT_LINK_STATE_BIT;
    } else if (p->to_clear & C_PORT_CONFIG_ERROR_BIT) {
        feature = C_PORT_CONFIG_ERROR;
        p->to_clear &= (uint16_t)~C_PORT_CONFIG_ERROR_BIT;
    } else {
        return 0;
    }
    if (request(h, TYPE_CLASS_PORT_OUT, REQ_CLEAR_FEATURE, feature, port, 0, REQ_CHANGE_CLEAR,
                port) != 0) {
        return 0;
    }
    p->recheck = 1;
    return 1;
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
        if (h->super) {
            /* USB 3.0 10.14.2.8: a SuperSpeed hub must know its tier, which
             * is how it routes packets to its ports, before any is used. */
            if (request(h, TYPE_CLASS_HUB_OUT, REQ_SET_HUB_DEPTH, h->depth, 0, 0, REQ_DEPTH, 0) !=
                0) {
                give_up(h, "it could not be told its depth");
                return;
            }
            h->state = HUB_DEPTH;
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
    case REQ_DEPTH:
        if (!ok) {
            give_up(h, "it would not take its depth");
            return;
        }
        h->state = HUB_TELL;
        return;
    case REQ_WARM_RESET:
        if (!ok) {
            h->port[port - 1].warm = 0;
            klog(KLOG_WARN, "usb", "hub: device %d port %d: the warm reset was refused", h->device,
                 port);
        }
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
            klog(KLOG_WARN, "usb",
                 "hub: device %d port %d: could not be powered on again; left off", h->device,
                 port);
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
            h->wait_fresh = 1;
        }
        return;
    case REQ_PORT_STATUS:
        if (!ok || got < 4) {
            if (h->state == HUB_SCAN) {
                give_up(h, "it would not say what is on a port");
                return;
            }
            if (h->port[port - 1].recheck) {
                give_up(h, "it would not say what is on a port after a change was acknowledged");
                return;
            }
            if (h->port[port - 1].reset == RESET_POLL) {
                h->port[port - 1].reset = RESET_FAILED;
            }
            return;
        }
        port_read(h, port);
        if (h->state == HUB_SCAN) {
            int i;

            h->at++;
            if (h->at > h->ports) {
                h->state = HUB_RUNNING;
                klog(KLOG_INFO, "usb", "hub: device %d: %d ports, power mode %d, think time %d",
                     h->device, h->ports, h->lpsm, h->think_time);
                /* The connections the scan found, told now that the hub
                 * can take a reset; the running arm reads the change
                 * endpoint once every change bit the scan found has been
                 * acknowledged. */
                for (i = 0; i < h->ports; i++) {
                    if (h->port[i].publish) {
                        h->port[i].publish = 0;
                        usb_core_hub_port(h->index, i, 1);
                    }
                }
            } else if (hub_slow() != 0) {
                h->state = HUB_SCAN_WAIT;
                h->wait_ms = hub_slow();
                h->wait_fresh = 1;
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
        struct hub *h = hub_at(i);

        if (!h->used || h->hc != completion->hc || h->device != completion->device) {
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
            {
                unsigned b;
                unsigned got = completion->length > completion->residual
                                   ? completion->length - completion->residual
                                   : 0;

                /* Only the bytes the hub sent: a short report leaves the
                 * rest of the buffer as the last report left it. */
                for (b = 0; b < got && b < sizeof(h->bitmap) && b < sizeof(h->changes); b++) {
                    h->changes |= (uint32_t)h->bitmap[b] << (8 * b);
                }
            }
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
            /* No request to wait for: this tick's time has not been
             * counted yet, so the wait starts on the next one as it is. */
            h->state = HUB_POWER_WAIT;
            h->wait_ms = h->power_good_ms;
        }
        return;
    }
    case HUB_POWER_WAIT:
        /* A wait set by a completion inside a tick does not count that
         * tick, so the power-good time is a minimum and not a minimum less
         * one tick. */
        if (h->wait_fresh) {
            h->wait_fresh = 0;
            return;
        }
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
    case HUB_SCAN_WAIT:
        if (h->wait_fresh) {
            h->wait_fresh = 0;
            return;
        }
        h->wait_ms = (h->wait_ms > elapsed_ms) ? h->wait_ms - elapsed_ms : 0;
        if (h->wait_ms != 0) {
            return;
        }
        h->state = HUB_SCAN;
        if (port_status_read(h, h->at) != 0) {
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
            if (p->to_clear == 0 && p->recheck) {
                h->change_reading = i + 1;
                if (port_status_read(h, i + 1) == 0) {
                    return;
                }
                /* The read could not go out: it is owed again. */
                h->change_reading = 0;
            }
            if (p->warm_pending && p->to_clear == 0 && !p->recheck) {
                p->warm_pending = 0;
                if (request(h, TYPE_CLASS_PORT_OUT, REQ_SET_FEATURE, BH_PORT_RESET, i + 1, 0,
                            REQ_WARM_RESET, i + 1) == 0) {
                    return;
                }
                p->warm = 0;
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
                /* The read could not go out: the bit is owed again. */
                h->changes |= (uint32_t)1u << (i + 1);
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

/**
 * Advance active hubs and release detached records after slot closure.
 *
 * A detached record remains unavailable for reuse while the controller
 * can access its control or interrupt-transfer buffers.
 */
void hub_step(uint32_t elapsed_ms)
{
    int i;

    hub_ms += elapsed_ms;
    for (i = 0; i < HUB_MAX; i++) {
        struct usb_location where;

        if (hubs[i] && !hubs[i]->used && usb_core_location(hubs[i]->index, &where) != 0) {
            hub_release(i);
        }
    }
    for (i = 0; i < HUB_MAX; i++) {
        if (hub_at(i)->used) {
            step_hub(hub_at(i), elapsed_ms);
        }
    }
}

int hub_port_status(struct xhci_ctrl *hc, int owner, int port, struct usb_port_status *out)
{
    struct hub *h = by_index(owner);
    const struct hub_port *p;

    (void)hc; /* a hub is reached through its own device */

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
    out->connected =
        (p->status & PORT_STAT_CONNECTION) != 0 && !p->off && p->power_on_ms == 0 && !p->warm;
    out->enabled = (p->status & PORT_STAT_ENABLE) != 0;
    out->changed = p->change != 0;
    /* A SuperSpeed hub's ports carry SuperSpeed devices and nothing else;
     * its status word's speed field says which SuperSpeed, which this
     * kernel does not distinguish. */
    out->speed = h->super                             ? USB_SPEED_SUPER
                 : (p->status & PORT_STAT_LOW_SPEED)  ? USB_SPEED_LOW
                 : (p->status & PORT_STAT_HIGH_SPEED) ? USB_SPEED_HIGH
                                                      : USB_SPEED_FULL;
    return 0;
}

int hub_port_settled(struct xhci_ctrl *hc, int owner, int port)
{
    struct hub *h = by_index(owner);
    const struct hub_port *p;

    (void)hc; /* a hub is reached through its own device */

    if (h == 0 || port < 0 || port >= h->ports) {
        return 0;
    }
    p = &h->port[port];
    /* Not before the hub runs: a device that settles while the scan is
     * still reading later ports would have its reset asked for from a hub
     * that cannot take one yet, and the core would give the port up. */
    return h->state == HUB_RUNNING && p->known && (p->status & PORT_STAT_CONNECTION) != 0 &&
           hub_ms - p->connect_ms >= HUB_DEBOUNCE_MS;
}

int hub_port_reset_begin(struct xhci_ctrl *hc, int owner, int port)
{
    struct hub *h = by_index(owner);

    (void)hc; /* a hub is reached through its own device */

    if (h == 0 || port < 0 || port >= h->ports || h->state != HUB_RUNNING) {
        return -1;
    }
    h->port[port].reset = RESET_ASKED;
    h->port[port].reset_ms = 0;
    return 0;
}

int hub_port_reset_done(struct xhci_ctrl *hc, int owner, int port)
{
    struct hub *h = by_index(owner);

    (void)hc; /* a hub is reached through its own device */

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
