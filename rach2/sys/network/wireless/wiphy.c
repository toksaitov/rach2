/* Register radios as Ethernet interfaces and keep station state and carrier. */
#include "wiphy.h"
#include "data.h"
#include "../ethernet/ethernet.h"
#include "../net.h"
#include "../netconf.h"
#include "../arp/arp.h"
#include "../ip6/route6.h"
#include "../ip6/nd6.h"
#include "../socket/netlink.h"
#include "../../lib/kerrno.h"
#include "../../lib/kstring.h"
#include "../../lib/kstd.h"
#include "../../mm/kheap.h"

static struct wlan *radios[WLAN_MAX_RADIOS];
static unsigned next_phy;
static wlan_event_fn event_handler;

/** Install the server's event consumer under the kernel lock. */
void wlan_set_event_handler(wlan_event_fn handler)
{
    event_handler = handler;
}

/** Dispatch a worker event while borrowing the event and its frame. */
void wlan_event(struct wlan *wlan, const struct wlan_event *event)
{
    if (event_handler && wlan && !wlan->removing)
        event_handler(wlan, event);
}

/** Route an IRQ hardware-scan completion to this radio's worker record. */
void wlan_radio_scan_done(struct wlan_radio *radio, int aborted)
{
    if (radio && radio->wlan)
        wlan_scan_hw_done(&radio->wlan->scan, aborted);
}

/** Return the nth registered station under the kernel lock. */
struct wlan *wlan_at(unsigned n)
{
    unsigned i;

    for (i = 0; i < WLAN_MAX_RADIOS; ++i)
        if (radios[i] && !radios[i]->removing && n-- == 0)
            return radios[i];
    return 0;
}

/** Find a radio by its stable phy number under the kernel lock. */
struct wlan *wlan_by_index(unsigned index)
{
    unsigned i;

    for (i = 0; i < WLAN_MAX_RADIOS; ++i)
        if (radios[i] && !radios[i]->removing && radios[i]->radio->index == index)
            return radios[i];
    return 0;
}

/** Find the station whose Ethernet interface is nd. */
struct wlan *wlan_from_netdev(const struct netdev *nd)
{
    unsigned i;

    for (i = 0; i < WLAN_MAX_RADIOS; ++i)
        if (radios[i] && !radios[i]->removing && &radios[i]->netdev == nd)
            return radios[i];
    return 0;
}

/** Enqueue through the data worker without sleeping under net_lock. */
static int wireless_tx(void *dev, const uint8_t *data, uint16_t len)
{
    struct wlan *wlan = dev;
    uint16_t protocol;

    if (!data || len < ETH_HDR_SIZE || len > ETH_HDR_SIZE + ETH_MTU)
        return NETDEV_TX_DROPPED;
    protocol = (uint16_t)((unsigned)data[12] << 8 | data[13]);
    if (!wlan->netdev.up || !wlan_port_allowed(wlan, protocol) || !wlan->tx_ethernet)
        return NETDEV_TX_DROPPED;
    return wlan->tx_ethernet(wlan, data, len) == 0 ? NETDEV_TX_OK : NETDEV_TX_DROPPED;
}

/** Report association as carrier, independently of authorization. */
static int wireless_link(void *dev, struct netdev_link *out)
{
    const struct wlan *wlan = dev;

    out->up =
        wlan->started && !wlan->blocked && !wlan->removing && wlan->state >= WLAN_STA_ASSOCIATED;
    out->mbps = 0;
    out->full_duplex = 0;
    return 1;
}

/** Stop at shutdown, which runs in process context without net_lock. */
static void wireless_stop(void *dev)
{
    wlan_stop(dev);
}

static const struct netdev_ops wireless_ops = {"wireless",    0, wireless_tx, 0, wireless_stop,
                                               wireless_link, 0, 0,           0};

/**
 * Apply on the worker the block the switch and the driver ask for. A block
 * aborts a scan, which then reports its abort, and stops the radio,
 * remembering whether it ran or was asked to start; the release starts it
 * again. Loops until the block applied is the one asked for, since either
 * may change while the driver's stop or start sleeps.
 */
static void wlan_rfkill_apply(struct wlan_work *work)
{
    struct wlan *wlan = work->wlan;
    unsigned want, resume;

    for (;;) {
        want = wlan->soft_blocked || wlan->hw_blocked;
        if (wlan->removing || want == wlan->blocked)
            return;
        if (want) {
            resume = wlan->started || wlan->resume;
            if (wlan_scan_running(&wlan->scan))
                (void)wlan_scan_abort(&wlan->scan);
            wlan_stop(wlan);
            wlan->blocked = 1;
            wlan->resume = resume;
        } else {
            resume = wlan->resume;
            wlan->blocked = 0;
            wlan->resume = 0;
            if (resume)
                (void)wlan_start(wlan);
        }
    }
}

/** Queue the block's application: the first record, or the second when the
 * first is pending or running, so that a change the running one has passed
 * is not lost. IRQ-safe under the kernel lock. */
static void wlan_rfkill_queue(struct wlan *wlan)
{
    if (wlan_queue_work(&wlan->rfkill_work[0], 0) == -EBUSY)
        (void)wlan_queue_work(&wlan->rfkill_work[1], 0);
}

/** The switch's soft block, from /dev/rfkill or sysfs; the worker applies
 * it. Process context under the kernel lock. */
static int wlan_rfkill_set_block(void *data, int blocked)
{
    struct wlan *wlan = data;

    wlan->soft_blocked = blocked ? 1u : 0u;
    wlan_rfkill_queue(wlan);
    return 0;
}

static const struct rfkill_ops wlan_rfkill_ops = {wlan_rfkill_set_block};

/** Record the driver's hard block on the switch and queue its application. */
void wlan_radio_rfkill(struct wlan_radio *radio, int blocked)
{
    struct wlan *wlan = radio ? radio->wlan : 0;

    if (!wlan || wlan->removing)
        return;
    wlan->hw_blocked = blocked ? 1u : 0u;
    (void)rfkill_set_hw_state(wlan->rfkill, blocked);
    wlan_rfkill_queue(wlan);
}

/** Validate and register a borrowed radio, leaving it down on success. */
int wlan_register_radio(struct wlan_radio *radio, struct wlan **out)
{
    struct wlan *wlan;
    unsigned i, j, slot;
    int rc;
    char phy[8];

    if (!out)
        return -EINVAL;
    *out = 0;
    if (!radio || !radio->ops || !radio->ops->start || !radio->ops->stop || !radio->ops->config ||
        !radio->ops->tx || !radio->name || !radio->name[0] || radio->wlan || (radio->mac[0] & 1) ||
        !(radio->mac[0] | radio->mac[1] | radio->mac[2] | radio->mac[3] | radio->mac[4] |
          radio->mac[5]) ||
        !radio->band.n_channels || radio->band.n_channels > WLAN_RADIO_MAX_CHANNELS ||
        !radio->band.n_rates || radio->band.n_rates > WLAN_RADIO_MAX_RATES)
        return -EINVAL;
    for (i = 0; radio->name[i]; ++i)
        if (i == 31 || !((radio->name[i] >= 'a' && radio->name[i] <= 'z') ||
                         (radio->name[i] >= 'A' && radio->name[i] <= 'Z') ||
                         (radio->name[i] >= '0' && radio->name[i] <= '9') ||
                         radio->name[i] == '_' || radio->name[i] == '-'))
            return -EINVAL;
    for (i = 0; i < radio->band.n_channels; ++i)
        for (j = 0; j < i; ++j)
            if (radio->band.channels[i].number == radio->band.channels[j].number ||
                radio->band.channels[i].frequency == radio->band.channels[j].frequency)
                return -EINVAL;
    for (i = 0; i < radio->band.n_rates; ++i)
        if (!radio->band.rates[i])
            return -EINVAL;
    for (slot = 0; slot < WLAN_MAX_RADIOS && radios[slot]; ++slot) {
    }
    if (slot == WLAN_MAX_RADIOS || next_phy > 999)
        return -ENOSPC;
    wlan = kzalloc(sizeof(*wlan));
    if (!wlan)
        return -ENOMEM;
    wlan->radio = radio;
    wlan->band = radio->band;
    wlan_reg_apply(&wlan->band, radio->country);
    wlan->netdev.ops = &wireless_ops;
    wlan->netdev.dev = wlan;
    wlan->netdev.pci_key = -1;
    wlan->netdev.mtu = ETH_MTU;
    k_memcpy(wlan->netdev.mac, radio->mac, 6);
    k_memcpy(wlan->netdev.name, "wlan", 4);
    k_itoa((int)next_phy, wlan->netdev.name + 4, 4, 10, 0);
    rc = wlan_work_start();
    if (rc < 0) {
        kfree(wlan);
        return rc;
    }
    k_memcpy(phy, "phy", 3);
    k_itoa((int)next_phy, phy + 3, 4, 10, 0);
    rc = rfkill_register(phy, RFKILL_TYPE_WLAN, (int)next_phy, &wlan_rfkill_ops, wlan,
                         &wlan->rfkill);
    if (rc < 0) {
        kfree(wlan);
        return rc;
    }
    for (i = 0; i < 2; ++i) {
        wlan->rfkill_work[i].wlan = wlan;
        wlan->rfkill_work[i].run = wlan_rfkill_apply;
    }
    wlan->soft_blocked = (unsigned)rfkill_soft_blocked(wlan->rfkill);
    wlan->blocked = wlan->soft_blocked;
    wlan_bss_init(&wlan->bss_table);
    wlan_scan_attach(&wlan->scan, wlan, wlan_event, radio, &wlan->band, &wlan->bss_table);
    rc = wlan_data_attach(wlan, wlan_event);
    if (rc < 0) {
        rfkill_unregister(wlan->rfkill);
        kfree(wlan);
        return rc;
    }
    rc = netdev_register(&wlan->netdev);
    if (rc < 0) {
        wlan_data_detach(wlan);
        rfkill_unregister(wlan->rfkill);
        kfree(wlan);
        return rc;
    }
    radio->index = next_phy++;
    radio->wlan = wlan;
    radios[slot] = wlan;
    *out = wlan;
    return 0;
}

/** Start an unblocked radio in process context without net_lock. */
int wlan_start(struct wlan *wlan)
{
    int rc;

    if (!wlan || wlan->removing)
        return -ENODEV;
    if (wlan->blocked) {
        wlan->resume = 1;
        return -ERFKILL;
    }
    if (wlan->started)
        return 0;
    rc = wlan->radio->ops->start(wlan->radio);
    if (!rc)
        wlan->started = 1;
    return rc;
}

/** Commit a state and borrowed BSS after the driver accepts the change, and
 * let the data path follow it. */
int wlan_set_state(struct wlan *wlan, enum wlan_sta_state state, struct wlan_bss *bss)
{
    enum wlan_sta_state old;
    int rc;

    if (!wlan || state < WLAN_STA_NONE || state > WLAN_STA_AUTHORIZED ||
        (state >= WLAN_STA_ASSOCIATED && (!bss || !wlan->started || wlan->blocked)))
        return -EINVAL;
    if (wlan->radio->ops->sta_state) {
        rc = wlan->radio->ops->sta_state(wlan->radio, bss, wlan->state, state);
        if (rc && state != WLAN_STA_NONE)
            return rc;
    }
    old = wlan->state;
    wlan->bss = bss;
    wlan->state = state;
    wlan_data_state(wlan, bss, old, state);
    return 0;
}

/** Stop the radio and clear carrier, authorization and the data path's
 * connection and keys; safe twice. */
void wlan_stop(struct wlan *wlan)
{
    if (!wlan)
        return;
    wlan->resume = 0;
    wlan->state = WLAN_STA_NONE;
    wlan->bss = 0;
    wlan_data_disconnect(wlan_data_of(wlan));
    if (wlan->started) {
        wlan->started = 0;
        wlan->radio->ops->stop(wlan->radio);
    }
}

/** Stop and drain a radio, withdraw its interface and free station storage. */
void wlan_unregister_radio(struct wlan *wlan)
{
    unsigned i;
    int n;
    uint32_t flags;
    struct netdev_rt r;
    struct arp_row neighbors[ARP_CACHE_SIZE];
    int count;
    struct nd6_row neighbors6[ND6_CACHE_SIZE];

    if (!wlan || wlan->removing)
        return;
    wlan->removing = 1;
    rfkill_unregister(wlan->rfkill);
    wlan_cancel_work(&wlan->rfkill_work[0]);
    wlan_cancel_work(&wlan->rfkill_work[1]);
    if (wlan->scan.state == WLAN_SCAN_HARDWARE && wlan->radio->ops->scan_cancel)
        wlan->radio->ops->scan_cancel(wlan->radio);
    wlan_stop(wlan);
    wlan_scan_detach(&wlan->scan);
    if (wlan->quiesce)
        wlan->quiesce(wlan);
    wlan_data_detach(wlan);
    flags = net_lock();
    netconf_set_up(&wlan->netdev, 0);
    nd6_if_down(&wlan->netdev);
    route6_forget(&wlan->netdev, -1);
    for (n = 0; netdev_route_at(n, &r);) {
        if (r.ifindex == wlan->netdev.index && r.kind == NETDEV_RT_ADDED)
            netdev_route_del(&wlan->netdev, r.dst, r.mask, r.gw);
        else
            ++n;
    }
    netconf_set_address(&wlan->netdev, 0, 0);
    arp_forget_dev(&wlan->netdev);
    count = arp_snapshot(neighbors, ARP_CACHE_SIZE);
    for (n = 0; n < count; ++n)
        if (neighbors[n].iface == wlan->netdev.name)
            arp_forget(&wlan->netdev, neighbors[n].ip);
    count = nd6_snapshot(neighbors6, ND6_CACHE_SIZE);
    for (n = 0; n < count; ++n)
        if (neighbors6[n].ifindex == wlan->netdev.index)
            nd6_forget(&wlan->netdev, neighbors6[n].addr);
    netdev_unregister(&wlan->netdev);
    net_unlock(flags);
    for (i = 0; i < WLAN_MAX_RADIOS; ++i)
        if (radios[i] == wlan)
            radios[i] = 0;
    wlan->radio->wlan = 0;
    wlan_bss_destroy(&wlan->bss_table);
    kfree(wlan);
}

/** Copy the counters under the kernel lock without retaining pointers. */
void wlan_get_counters(const struct wlan *wlan, struct wlan_counters *out)
{
    if (wlan && out)
        *out = wlan->counters;
}
