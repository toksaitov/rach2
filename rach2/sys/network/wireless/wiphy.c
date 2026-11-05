/* Register radios as Ethernet interfaces and keep station state and carrier. */
#include "wiphy.h"
#include "data.h"
#include "sta.h"
#include "wsim.h"
#include "nl80211.h"
#include "../../klog.h"
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
#include "../../proc/wait_queue.h"

static struct wlan *radios[WLAN_MAX_RADIOS];
static unsigned next_phy;
static wlan_event_fn event_handler;
static wait_queue_t request_wq; /* a request ended, or removal may go on */

/* A request to bring the interface up or take it down, linked on its radio
 * from the call until its caller resumes. The record is on the caller's
 * stack, and the way down carries it to the worker. */
struct wlan_request {
    struct wlan_work work; /* first: wlan_down_run finds the record from it */
    struct wlan_request *next;
    int done; /* the worker has run the way down */
    int gone; /* removal ran meanwhile: the radio is freed or going */
};

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

/** On the worker, end a scan, deauthenticate, sending the frame, and stop the
 * radio; then let go of the radio and wake the caller and removal. */
static void wlan_down_run(struct wlan_work *work)
{
    struct wlan_request *req = (struct wlan_request *)work;
    struct wlan *wlan = work->wlan;

    if (wlan_scan_running(&wlan->scan))
        (void)wlan_scan_abort(&wlan->scan);
    wlan_sta_leave(wlan, 1);
    wlan_stop(wlan);
    --wlan->busy;
    req->done = 1;
    wq_wake_all(&request_wq);
}

/** Whether the way down in request arg has run. */
static int wlan_down_done(void *arg)
{
    return ((const struct wlan_request *)arg)->done;
}

/** Whether no request holds the radio arg. */
static int wlan_idle(void *arg)
{
    return ((const struct wlan *)arg)->busy == 0;
}

/** Take request req off its radio's list; kernel lock held. */
static void wlan_request_unlink(struct wlan *wlan, struct wlan_request *req)
{
    struct wlan_request **link;

    for (link = &wlan->requests; *link; link = &(*link)->next)
        if (*link == req) {
            *link = req->next;
            return;
        }
}

/**
 * Start the radio as its interface comes up, and leave the network and stop
 * it as the interface goes down, as cfg80211 does on dev_open and dev_close.
 * The switch's blocks refuse the way up with -ERFKILL, as Linux's do, also
 * when one is set while the driver's start sleeps. A release whose block the
 * worker has not yet cleared lets the interface up, and the worker starts
 * the radio as it clears the block, so that an up which follows the
 * release's CHANGE event at once is not lost. A worker that failed to start
 * is made again first: without one the interface stays down with -ENOMEM.
 * The way down stops the radio itself, instead of waiting, when there is no
 * worker or the block is applied: the block has left the network already,
 * and its worker closes the interface through here. Process context without
 * net_lock; the driver's start may sleep, and the way down may wait for the
 * worker. Returns 0, -ENOMEM, -ERFKILL, the driver's error from its start,
 * or -ENODEV when the radio was removed meanwhile, in which case nothing of
 * it may be touched: removal waits for the worker to run the way down and
 * for the driver's start or stop to return, but not for the caller.
 */
static int wireless_set_up(void *dev, int up)
{
    struct wlan *wlan = dev;
    struct wlan_request req;
    int rc = 0, worker;

    if (wlan->removing)
        return 0;
    if (up && (wlan->soft_blocked || wlan->hw_blocked))
        return -ERFKILL;
    worker = wlan_work_start() == 0;
    if (up && !worker)
        return -ENOMEM;
    k_memset(&req, 0, sizeof(req));
    req.work.wlan = wlan;
    req.work.run = wlan_down_run;
    req.next = wlan->requests;
    wlan->requests = &req;
    ++wlan->busy;
    if (up) {
        rc = wlan_start(wlan);
        if (rc == -ERFKILL)
            rc = 0;
        if (rc == 0 && !req.gone && (wlan->soft_blocked || wlan->hw_blocked)) {
            wlan_stop(wlan);
            rc = -ERFKILL;
        }
    } else if (worker && !wlan->blocked && wlan_queue_work(&req.work, 0) == 0) {
        (void)wq_wait_event_nointr(&request_wq, wlan_down_done, &req);
        if (req.gone)
            return -ENODEV;
        wlan_request_unlink(wlan, &req);
        return 0;
    } else {
        wlan_stop(wlan);
    }
    /* Held until here, since the driver's stop may sleep too. */
    --wlan->busy;
    if (req.gone) {
        wq_wake_all(&request_wq);
        return -ENODEV;
    }
    wlan_request_unlink(wlan, &req);
    return rc;
}

static const struct netdev_ops wireless_ops = {"wireless",    0, wireless_tx, 0, wireless_stop,
                                               wireless_link, 0, 0,           0, wireless_set_up};

/**
 * Apply on the worker the block the switch and the driver ask for, as
 * Linux's cfg80211_rfkill_set_block does. A block aborts a scan, which then
 * reports its abort, has the station leave without a frame, stops the radio
 * and closes the interface, so that wpa_supplicant sees IFF_UP fall over
 * rtnetlink and stops scanning; the interface's way down finds the block
 * applied and stops nothing more, so the worker does not wait for itself.
 * The release starts the radio only when an up came between the release
 * and this call, or the release came while the stop slept and the
 * interface is still up; otherwise the interface stays down until wpa_supplicant,
 * told by the switch's CHANGE event, brings it up. Loops until the block
 * applied is the one asked for, with the interface down while blocked,
 * since either may change while the driver's stop or start sleeps.
 */
static void wlan_rfkill_apply(struct wlan_work *work)
{
    struct wlan *wlan = work->wlan;
    unsigned want, resume;

    for (;;) {
        want = wlan->soft_blocked || wlan->hw_blocked;
        if (wlan->removing)
            return;
        if (want && (!wlan->blocked || wlan->netdev.up)) {
            if (wlan_scan_running(&wlan->scan))
                (void)wlan_scan_abort(&wlan->scan);
            wlan_sta_leave(wlan, 0);
            wlan_stop(wlan);
            wlan->blocked = 1;
            if (!wlan->soft_blocked && !wlan->hw_blocked) {
                /* Released while the stop slept: the release's up found the
                 * interface up and did nothing, so start the radio. */
                if (wlan->netdev.up)
                    wlan->resume = 1;
                continue;
            }
            if (netconf_set_up(&wlan->netdev, 0) != 0)
                return;
        } else if (!want && wlan->blocked) {
            resume = wlan->resume;
            wlan->blocked = 0;
            wlan->resume = 0;
            if (resume)
                (void)wlan_start(wlan);
        } else {
            return;
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

/** Return whether name is a radio's name: 1 to 31 letters, digits, '_'
 * and '-'. */
static int radio_name_valid(const char *name)
{
    unsigned i;
    char c;

    if (!name || !name[0])
        return 0;
    for (i = 0; name[i]; ++i) {
        c = name[i];
        if (i == 31 || !((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                         (c >= '0' && c <= '9') || c == '_' || c == '-'))
            return 0;
    }
    return 1;
}

/** Return whether band has channels and rates within their bounds, no two
 * channels sharing a number or a frequency and no rate zero. */
static int band_valid(const struct wlan_band *band)
{
    unsigned i, j;

    if (!band->n_channels || band->n_channels > WLAN_RADIO_MAX_CHANNELS || !band->n_rates ||
        band->n_rates > WLAN_RADIO_MAX_RATES)
        return 0;
    for (i = 0; i < band->n_channels; ++i)
        for (j = 0; j < i; ++j)
            if (band->channels[i].number == band->channels[j].number ||
                band->channels[i].frequency == band->channels[j].frequency)
                return 0;
    for (i = 0; i < band->n_rates; ++i)
        if (!band->rates[i])
            return 0;
    return 1;
}

/** Return whether radio may be registered: every operation set, a valid
 * name and band, no wlan yet, and a unicast MAC address that is not zero. */
static int radio_valid(const struct wlan_radio *radio)
{
    const uint8_t *mac;

    if (!radio || !radio->ops || !radio->ops->start || !radio->ops->stop || !radio->ops->config ||
        !radio->ops->tx || radio->wlan)
        return 0;
    mac = radio->mac;
    if ((mac[0] & 1) || !(mac[0] | mac[1] | mac[2] | mac[3] | mac[4] | mac[5]))
        return 0;
    return radio_name_valid(radio->name) && band_valid(&radio->band);
}

/** Validate and register a borrowed radio, leaving it down on success. */
int wlan_register_radio(struct wlan_radio *radio, struct wlan **out)
{
    struct wlan *wlan;
    unsigned i, slot;
    int rc;
    char phy[8];

    if (!out)
        return -EINVAL;
    *out = 0;
    if (!radio_valid(radio))
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
    rc = wlan_sta_attach(wlan);
    if (rc < 0) {
        wlan_data_detach(wlan);
        rfkill_unregister(wlan->rfkill);
        kfree(wlan);
        return rc;
    }
    rc = netdev_register(&wlan->netdev);
    if (rc < 0) {
        wlan_sta_detach(wlan);
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
    wlan->frequency = bss ? bss->frequency : 0;
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
    wlan->frequency = 0;
    wlan_data_disconnect(wlan_data_of(wlan));
    if (wlan->started) {
        wlan->started = 0;
        wlan->radio->ops->stop(wlan->radio);
    }
}

/** Stop and drain a radio, withdraw its interface and free station storage.
 * The interface's requests in flight are marked gone first, and removal
 * waits until the worker has run each way down and each driver start or
 * stop has returned, so that nothing still running holds the radio it frees. */
void wlan_unregister_radio(struct wlan *wlan)
{
    unsigned i;
    int n;
    uint32_t flags;
    struct netdev_rt r;
    struct arp_row neighbors[ARP_CACHE_SIZE];
    int count;
    struct nd6_row neighbors6[ND6_CACHE_SIZE];
    struct wlan_request *req;

    if (!wlan || wlan->removing)
        return;
    wlan->removing = 1;
    for (req = wlan->requests; req; req = req->next)
        req->gone = 1;
    wlan->requests = 0;
    (void)wq_wait_event_nointr(&request_wq, wlan_idle, wlan);
    rfkill_unregister(wlan->rfkill);
    wlan_cancel_work(&wlan->rfkill_work[0]);
    wlan_cancel_work(&wlan->rfkill_work[1]);
    wlan_sta_detach(wlan);
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

/* The registry's calls the software radio makes. */
static const struct wsim_station wsim_calls = {wlan_register_radio, wlan_unregister_radio,
                                               wlan_radio_rx, wlan_radio_tx_status};

int wlan_init(void)
{
    int rc = nl80211_init();

    if (rc) {
        klog(KLOG_ERR, "wlan", "nl80211: not registered: error %d", rc);
    }
    return wsim_attach(&wsim_calls);
}
