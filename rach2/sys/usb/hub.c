/*
 * hub.c: the hub class driver.
 *
 * Nothing here yet drives a hub: the core matches the class and hands the
 * device over, and this refuses it by name, so a hub plugged in is recorded
 * and costs the devices beside it nothing.
 */

#include "hub.h"

#include "../klog.h"
#include "core.h"
#include "hcd.h"

int hub_attach(int index, int device, int interface, int protocol, int ep_dci)
{
    (void)index;
    (void)interface;
    (void)protocol;
    (void)ep_dci;
    klog(KLOG_WARN, "usb", "hub: device %d: this kernel does not drive hubs yet", device);
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
    out->enabled = 0;
    out->changed = 0;
    out->speed = 0;
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
