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

int xhci_ports(void)
{
    return 0;
}

int xhci_port_status(int port, struct usb_port_status *out)
{
    (void)port;
    (void)out;
    return -1;
}

int xhci_port_settled(int port)
{
    (void)port;
    return 0;
}

int xhci_port_reset_begin(int port)
{
    (void)port;
    return -1;
}

int xhci_port_reset_done(int port)
{
    (void)port;
    return -1;
}

int xhci_device_hub(int device, int ports, int multi_tt, int think_time)
{
    (void)device;
    (void)ports;
    (void)multi_tt;
    (void)think_time;
    return 0;
}

int xhci_device_open(const struct usb_location *where, int speed)
{
    (void)where;
    (void)speed;
    return -1;
}

int xhci_device_address(int device)
{
    (void)device;
    return -1;
}

int xhci_device_packet_size(int device, int max_packet)
{
    (void)device;
    (void)max_packet;
    return -1;
}

int xhci_device_close(int device)
{
    (void)device;
    return -1;
}

void xhci_device_closed(int device)
{
    (void)device;
}

int xhci_command_busy(void)
{
    return 0;
}

int xhci_endpoint_open(int device, const struct usb_endpoint *endpoint)
{
    (void)device;
    (void)endpoint;
    return -1;
}

int xhci_command_done(struct usb_command_result *out)
{
    (void)out;
    return -1;
}

int xhci_submit(int device, int endpoint, const struct usb_transfer *transfer)
{
    (void)device;
    (void)endpoint;
    (void)transfer;
    return -1;
}

/* The class driver the core hands a stick to. This test drives the core's
   own walk of the descriptors and its enumeration, not that driver, so the
   stubs record what they were told and nothing more. */
int msc_attach(int device, int interface, int ep_in, int ep_out)
{
    (void)device;
    (void)interface;
    (void)ep_in;
    (void)ep_out;
    return 0;
}

void msc_detach(int device)
{
    (void)device;
}

int hub_attach(int index, int device, int interface, int protocol, int ep_dci)
{
    (void)index;
    (void)device;
    (void)interface;
    (void)protocol;
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

int hub_port_status(int owner, int port, struct usb_port_status *out)
{
    (void)owner;
    (void)port;
    out->connected = 0;
    return -1;
}

int hub_port_settled(int owner, int port)
{
    (void)owner;
    (void)port;
    return 0;
}

int hub_port_reset_begin(int owner, int port)
{
    (void)owner;
    (void)port;
    return -1;
}

int hub_port_reset_done(int owner, int port)
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

int xhci_poll(struct usb_completion *out)
{
    (void)out;
    return 0;
}

int xhci_endpoint_reset(int device, int endpoint)
{
    (void)device;
    (void)endpoint;
    return -1;
}

int xhci_endpoint_dequeue(int device, int endpoint)
{
    (void)device;
    (void)endpoint;
    return -1;
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

int main(void)
{
    struct usb_interface_info info;
    unsigned char broken[sizeof(keyboard_config)];
    unsigned i;

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
