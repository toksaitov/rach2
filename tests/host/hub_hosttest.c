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
 * enumerating it at its location; a reset that completes and one that never
 * does, with the port refused and forgotten when its device leaves; the
 * speed read from the status word; a disconnection closing the device
 * below; a hub pulled with two devices below closing both children before
 * itself; a hub with twenty ports publishing fifteen; and a hub that will
 * not give its descriptor refused by name.
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

/* ---- the kernel calls the two files reach ---- */

void klog(int level, const char *subsystem, const char *format, ...)
{
    (void)level;
    (void)subsystem;
    (void)format;
}

void *k_memset(void *ptr, char c, size_t n)
{
    unsigned char *p = (unsigned char *)ptr;
    size_t i;

    for (i = 0; i < n; i++) {
        p[i] = (unsigned char)c;
    }
    return ptr;
}

static int hid_gone_slot[16];
static int hid_gone_count;

void hid_report(int slot, int kind, const uint8_t *report, int length)
{
    (void)slot;
    (void)kind;
    (void)report;
    (void)length;
}

void hid_gone(int slot)
{
    if (hid_gone_count < 16) {
        hid_gone_slot[hid_gone_count] = slot;
    }
    hid_gone_count++;
}

int msc_attach(int device, int interface, int ep_in, int ep_out)
{
    (void)device;
    (void)interface;
    (void)ep_in;
    (void)ep_out;
    return -1;
}

void msc_detach(int device)
{
    (void)device;
}

void msc_step(uint32_t elapsed_ms)
{
    (void)elapsed_ms;
}

int msc_completion(const struct usb_completion *completion)
{
    (void)completion;
    return 0;
}

void usbdisk_step(void)
{}

void input_mouse_found(void)
{}

/* ---- the model ---- */

#define KIND_NONE 0
#define KIND_HUB  1
#define KIND_KBD  2

#define MODEL_PORTS 20

static struct {
    int root_connected; /* the hub on root port 0 */
    int root_speed;
    int hub_protocol; /* the hub's interface protocol byte */
    int hub_ports;    /* what its descriptor claims */
    int hub_lpsm;
    int hub_think;
    int hub_pwrgood;    /* in 2 ms units */
    int hub_desc_stall; /* the hub descriptor is refused */
    int port_kind[MODEL_PORTS];
    int port_speed[MODEL_PORTS];
    int port_reset_never[MODEL_PORTS];
    int port_in_reset[MODEL_PORTS];
    int port_reset_reads[MODEL_PORTS];
    int port_enabled[MODEL_PORTS];
    /* What was seen. */
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
    uint32_t now_ms;
} model;

/* The slots: what is on each, and where it was opened. */
static struct {
    int kind;
    int speed;
    struct usb_location where;
    int open;
} slot_of[16];
static int slots_given;

static int close_order[16];
static int closes;

/* The command in flight and the completions waiting. */
static int command_pending;
static struct usb_command_result command_result_pending;
static struct usb_completion done[32];
static int done_head, done_tail;

static void answer(int device, int endpoint, int status, uint32_t length, uint32_t residual,
                   uint32_t generation)
{
    struct usb_completion *c = &done[done_tail % 32];

    c->device = device;
    c->endpoint = endpoint;
    c->status = status;
    c->length = length;
    c->residual = residual;
    c->generation = generation;
    done_tail++;
}

int xhci_poll(struct usb_completion *out)
{
    if (done_head == done_tail) {
        return 0;
    }
    *out = done[done_head % 32];
    done_head++;
    return 1;
}

int xhci_ports(void)
{
    return 4;
}

int xhci_port_status(int port, struct usb_port_status *out)
{
    out->connected = port == 0 && model.root_connected;
    out->enabled = out->connected;
    out->changed = 0;
    out->speed = model.root_speed;
    return 0;
}

int xhci_port_settled(int port)
{
    (void)port;
    return 1;
}

int xhci_port_reset_begin(int port)
{
    (void)port;
    return 0;
}

int xhci_port_reset_done(int port)
{
    (void)port;
    return 1;
}

int xhci_command_busy(void)
{
    return 0;
}

static void command_ok(int device)
{
    command_pending = 1;
    command_result_pending.device = device;
    command_result_pending.status = USB_XFER_OK;
}

int xhci_command_done(struct usb_command_result *out)
{
    if (!command_pending) {
        return 0;
    }
    command_pending = 0;
    *out = command_result_pending;
    return 1;
}

/** What is at a location: the hub on the root port, or a device on one of
 * its ports. */
static int kind_at(const struct usb_location *where, int *speed)
{
    if (where->depth == 0) {
        *speed = model.root_speed;
        return model.root_connected ? KIND_HUB : KIND_NONE;
    }
    if (where->depth == 1) {
        int port = (int)(where->route & 0xFu);

        if (port >= 1 && port <= MODEL_PORTS) {
            *speed = model.port_speed[port - 1];
            return model.port_kind[port - 1];
        }
    }
    return KIND_NONE;
}

int xhci_device_open(const struct usb_location *where, int speed)
{
    int n;

    if (slots_given >= 15) {
        return -1;
    }
    n = ++slots_given;
    slot_of[n].where = *where;
    slot_of[n].speed = speed;
    slot_of[n].kind = kind_at(where, &slot_of[n].speed);
    slot_of[n].open = 1;
    command_ok(n);
    return 0;
}

int xhci_device_address(int device)
{
    command_ok(device);
    return 0;
}

int xhci_device_packet_size(int device, int max_packet)
{
    (void)max_packet;
    command_ok(device);
    return 0;
}

int xhci_device_hub(int device, int ports, int multi_tt, int think_time)
{
    model.hub_told = 1;
    model.hub_told_slot = device;
    model.hub_told_ports = ports;
    model.hub_told_multi = multi_tt;
    model.hub_told_think = think_time;
    command_ok(device);
    return 0;
}

int xhci_endpoint_open(int device, const struct usb_endpoint *endpoint)
{
    (void)endpoint;
    command_ok(device);
    return 0;
}

int xhci_device_close(int device)
{
    if (closes < 16) {
        close_order[closes] = device;
    }
    closes++;
    slot_of[device].open = 0;
    command_ok(device);
    return 0;
}

void xhci_device_closed(int device)
{
    (void)device;
}

int xhci_endpoint_reset(int device, int endpoint)
{
    (void)endpoint;
    command_ok(device);
    return 0;
}

int xhci_endpoint_dequeue(int device, int endpoint)
{
    (void)endpoint;
    command_ok(device);
    return 0;
}

void xhci_endpoint_close(int device, int endpoint)
{
    (void)device;
    (void)endpoint;
}

/* The descriptors the two kinds answer with. */
static const unsigned char keyboard_config[] = {
    9, 2,  34,   0, 1, 1, 0,  0xA0, 50, /* configuration, value 1 */
    9, 4,  0,    0, 1, 3, 1,  1,    0,  /* interface: HID, boot, keyboard */
    9, 33, 0x11, 1, 0, 1, 34, 63,   0,  /* the HID descriptor */
    7, 5,  0x81, 3, 8, 0, 10            /* endpoint 1, in, interrupt */
};

static unsigned char hub_config[25];

static void hub_config_fill(void)
{
    unsigned char c[25] = {
        9, 2, 25,   0, 1, 1, 0, 0xE0, 0, /* configuration, value 1 */
        9, 4, 0,    0, 1, 9, 0, 0,    0, /* interface: hub, protocol below */
        7, 5, 0x81, 3, 1, 0, 12          /* endpoint 1, in, interrupt */
    };
    int i;

    c[16] = (unsigned char)model.hub_protocol;
    for (i = 0; i < 25; i++) {
        hub_config[i] = c[i];
    }
}

static unsigned int port_status_word(int port)
{
    unsigned int status = 0x0100; /* powered */

    if (model.port_kind[port - 1] != KIND_NONE) {
        status |= 0x0001;
        if (model.port_enabled[port - 1]) {
            status |= 0x0002;
        }
        if (model.port_speed[port - 1] == USB_SPEED_LOW) {
            status |= 0x0200;
        } else if (model.port_speed[port - 1] == USB_SPEED_HIGH) {
            status |= 0x0400;
        }
    }
    if (model.port_in_reset[port - 1]) {
        status |= 0x0010;
    }
    return status;
}

int xhci_submit(int device, int endpoint, const struct usb_transfer *transfer)
{
    const unsigned char *setup = transfer->setup;
    unsigned char *buf = transfer->buffer;
    int kind;
    uint32_t i;

    if (device <= 0 || device > 15 || !slot_of[device].open) {
        return -1;
    }
    kind = slot_of[device].kind;
    if (endpoint != 1) {
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
            buf[7] = 64;
            buf[8] = 0x09;
            buf[9] = 0x04; /* vendor 0x0409 */
            buf[10] = kind == KIND_HUB ? 0xAA : 0x01;
            buf[11] = kind == KIND_HUB ? 0x55 : 0x00;
            buf[17] = 1;
            have = 18;
        } else if (type == 2) {
            const unsigned char *cfg = kind == KIND_HUB ? hub_config : keyboard_config;
            uint32_t len = kind == KIND_HUB ? 25u : (uint32_t)sizeof(keyboard_config);

            if (kind == KIND_HUB) {
                hub_config_fill();
                cfg = hub_config;
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
        answer(device, 1, USB_XFER_OK, 0, 0, transfer->generation);
        return 0;
    }
    if (setup[0] == 0x21 && setup[1] == 0x0B) { /* SET_PROTOCOL */
        answer(device, 1, USB_XFER_OK, 0, 0, transfer->generation);
        return 0;
    }
    if (kind != KIND_HUB) {
        answer(device, 1, USB_XFER_STALLED, transfer->length, transfer->length,
               transfer->generation);
        return 0;
    }
    /* The hub's class requests. */
    if (setup[0] == 0xA0 && setup[1] == 6 && setup[3] == 0x29) {
        if (model.hub_desc_stall) {
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
        if (model.port_in_reset[port - 1] && !model.port_reset_never[port - 1]) {
            model.port_reset_reads[port - 1]++;
            if (model.port_reset_reads[port - 1] >= 2) {
                model.port_in_reset[port - 1] = 0;
                model.port_enabled[port - 1] = 1;
            }
        }
        status = port_status_word(port);
        buf[0] = (unsigned char)(status & 0xFF);
        buf[1] = (unsigned char)(status >> 8);
        buf[2] = 0;
        buf[3] = 0;
        answer(device, 1, USB_XFER_OK, 4, 0, transfer->generation);
        return 0;
    }
    if (setup[0] == 0x23 && setup[1] == 3) { /* SET_PORT_FEATURE */
        int feature = setup[2];
        int port = setup[4];

        if (feature == 8) {
            model.power_requests++;
            model.last_power_ms = model.now_ms;
        } else if (feature == 4 && port >= 1 && port <= MODEL_PORTS) {
            model.reset_requests++;
            model.port_in_reset[port - 1] = 1;
            model.port_reset_reads[port - 1] = 0;
        }
        answer(device, 1, USB_XFER_OK, 0, 0, transfer->generation);
        return 0;
    }
    if (setup[0] == 0x23 && setup[1] == 1) { /* CLEAR_PORT_FEATURE */
        if (setup[2] == 20) {
            model.reset_clears++;
        }
        answer(device, 1, USB_XFER_OK, 0, 0, transfer->generation);
        return 0;
    }
    answer(device, 1, USB_XFER_STALLED, transfer->length, transfer->length, transfer->generation);
    return 0;
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
        hubs[i].used = 0;
    }
}

/** Run the pump for `ms` milliseconds, ten at a time. */
static void pump(uint32_t ms)
{
    uint32_t t;

    for (t = 0; t < ms; t += 10) {
        model.now_ms += 10;
        usb_core_step(10);
    }
}

/** The slot of the first open device of `kind`, or 0. */
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

static int running_count(void)
{
    int i;
    int n = 0;

    for (i = 0; i < USB_MAX_DEVICES; i++) {
        if (devices[i].state == ST_RUNNING) {
            n++;
        }
    }
    return n;
}

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

int main(void)
{
    char line[96];
    int hub;
    int kbd;

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
    line[0] = '\0';
    {
        int i;
        int found = 0;

        for (i = 0; i < USB_MAX_DEVICES; i++) {
            if (usb_core_describe(i, line, (int)sizeof(line)) > 0 && contains(line, "class 3")) {
                found = contains(line, "port 1.2 ");
            }
        }
        CHECK("/proc/usb gives the keyboard's location as 1.2", found);
    }

    /* Power: ganged is one request, per port is one each, and the first
       port is not looked at before the power-good time has passed. */
    model_reset();
    model.hub_lpsm = 0;
    model.hub_pwrgood = 50; /* 100 ms */
    pump(2000);
    CHECK("a ganged hub is powered with one request", model.power_requests == 1);
    CHECK("and no port is read before the power-good time",
          model.first_status_ms >= model.last_power_ms + 100u);
    model_reset();
    model.hub_lpsm = 1;
    pump(2000);
    CHECK("a per-port hub is powered port by port", model.power_requests == 8);

    /* A high-speed hub with one translator per port and a think time of two:
       the hub's slot gets both, and a low-speed keyboard below it carries
       the hub and its port as its translator. */
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
          kbd != 0 && slot_of[kbd].speed == USB_SPEED_LOW);
    CHECK("carrying the hub as its translator at port 3, per port",
          kbd != 0 && slot_of[kbd].where.tt_device == hub && slot_of[kbd].where.tt_port == 3 &&
              slot_of[kbd].where.tt_multi == 1);
    CHECK("and a route of 3", kbd != 0 && slot_of[kbd].where.route == 0x3u);

    /* A hub with twenty ports publishes fifteen. */
    model_reset();
    model.hub_ports = 20;
    pump(2000);
    CHECK("a hub with twenty ports is told as fifteen", model.hub_told_ports == 15);

    /* A reset that never ends: the device below is refused, its port is
       remembered as refused on the hub, and the refusal goes when the device
       does. */
    model_reset();
    model.port_kind[1] = KIND_KBD;
    model.port_reset_never[1] = 1;
    pump(3000);
    CHECK("a device whose port never leaves reset is not enumerated", slot_of_kind(KIND_KBD) == 0);
    {
        int i;
        int hub_index = -1;

        for (i = 0; i < USB_MAX_DEVICES; i++) {
            if (devices[i].state == ST_RUNNING && devices[i].interface.kind == USB_KIND_HUB) {
                hub_index = i;
            }
        }
        CHECK("and its port is remembered as refused on the hub",
              hub_index >= 0 && (devices[hub_index].refused & 0x2u) != 0);
        pump(2000);
        CHECK("so it is not tried again", model.reset_requests == 1);
        usb_core_hub_port(hub_index, 1, 0);
        CHECK("until its device leaves",
              hub_index >= 0 && (devices[hub_index].refused & 0x2u) == 0);
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
            if (devices[i].state == ST_RUNNING && devices[i].interface.kind == USB_KIND_HUB) {
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

    /* A hub that will not give its descriptor is refused by name. */
    model_reset();
    model.hub_desc_stall = 1;
    pump(2000);
    CHECK("a hub that will not give its descriptor is closed", closes == 1 && running_count() == 0);

    if (fails == 0) {
        printf("hub_hosttest: OK\n");
        return 0;
    }
    printf("hub_hosttest: %d failure(s)\n", fails);
    return 1;
}
