/* Check radio registration, station Ethernet conversion and the EAPOL gate.
 * host-deps: rach2/sys/network/wireless/wiphy.c
 * host-deps: rach2/sys/network/wireless/wlan.c
 * host-deps: rach2/sys/network/wireless/reg.c
 * host-deps: rach2/sys/network/driver/netdev.c
 * host-deps: rach2/sys/network/ethernet/ethernet.c
 * host-deps: rach2/sys/network/socket/packet.c
 * host-deps: rach2/sys/network/socket/dgq.c
 * host-deps: rach2/sys/network/socket/address.c
 * host-deps: rach2/sys/network/route.c
 * host-deps: rach2/sys/network/ip6/addr6.c
 * host-deps: rach2/sys/lib/kstd.c
 * host-deps: rach2/sys/lib/kstring.c
 * host-deps: rach2/sys/lib/kctypes.c
 * host-deps: tests/host/compat/kheap_host.c
 */
#include "network/wireless/wiphy.h"
#include "network/net.h"
#include "network/netconf.h"
#include "network/ethernet/ethernet.h"
#include "network/arp/arp.h"
#include "network/ip/ip.h"
#include "network/ip6/ip6.h"
#include "network/ip6/route6.h"
#include "network/ip6/nd6.h"
#include "network/socket/socket.h"
#include "network/socket/packet.h"
#include "cmdline.h"
#include "klog.h"
#include "lib/kerrno.h"
#include "lib/kstring.h"

extern int printf(const char *format, ...);
extern unsigned long kheap_host_largest;
static unsigned failures, ip_received, wakes, lock_depth;
static int start_error, state_error;
static unsigned starts, stops, transmissions;
static uint8_t sent[IEEE80211_MAX_FRAME_LEN], queued[ETH_HDR_SIZE + ETH_MTU];
static unsigned sent_len, queued_len;
uint8_t net_mac[6];

#define CHECK(x)                                                                \
    do {                                                                        \
        if (!(x)) {                                                             \
            ++failures;                                                         \
            printf("wlan_dev_hosttest: line %u: %s\n", (unsigned)__LINE__, #x); \
        }                                                                       \
    } while (0)

/** All Ethernet probes find no card; radios are attached after this probe. */
static int absent(struct netdev *nd, int nth)
{
    (void)nd;
    (void)nth;
    return NETDEV_ABSENT;
}

const struct netdev_ops rtl8139_netdev = {"rtl8139", absent, 0, 0, 0, 0, 0, 0, 0};
const struct netdev_ops e1000_netdev = {"e1000", absent, 0, 0, 0, 0, 0, 0, 0};
const struct netdev_ops rtl8168_netdev = {"rtl8168", absent, 0, 0, 0, 0, 0, 0, 0};

/** The model has no boot options. */
int kcmdline_value(const char *key, char *out, unsigned int cap)
{
    (void)key;
    (void)out;
    (void)cap;
    return 0;
}

/** Logs are not part of these assertions. */
void klog(int level, const char *subsystem, const char *format, ...)
{
    (void)level;
    (void)subsystem;
    (void)format;
}

/** Record the lock context presented to the worker boundary. */
uint32_t net_lock(void)
{
    return lock_depth++;
}

/** Restore the model's previous lock depth. */
void net_unlock(uint32_t flags)
{
    lock_depth = flags;
}

/** The model remains in the first network namespace. */
int net_ns_seen(void)
{
    return 1;
}

/** Every registered interface is visible in the first namespace. */
int netdev_seen_count(void)
{
    return netdev_listed();
}

/** The IP recorder counts delivery after the control-port gate. */
void ip_rx(const uint8_t *data, uint16_t len, int loopback)
{
    (void)data;
    (void)len;
    (void)loopback;
    ++ip_received;
}

/** ARP is represented by the packet socket assertions. */
void arp_rx(struct netdev *nd, const uint8_t *data, uint16_t len)
{
    (void)nd;
    (void)data;
    (void)len;
}

/** No neighbour mappings are installed by this fixture. */
int arp_snapshot(struct arp_row *out, int max)
{
    (void)out;
    (void)max;
    return 0;
}

/** Flush no pending ARP datagram in the model. */
void arp_forget_dev(struct netdev *nd)
{
    (void)nd;
}

/** Removing a nonexistent mapping reports the normal miss. */
int arp_forget(struct netdev *nd, uint32_t ip)
{
    (void)nd;
    (void)ip;
    return -ENOENT;
}

/** IPv6 is represented by the control-port assertions. */
void ip6_rx(struct netdev *nd, const uint8_t *data, uint16_t len)
{
    (void)nd;
    (void)data;
    (void)len;
}

/** The queue itself is real; waking a waiter needs no scheduler here. */
void wq_wake(wait_queue_t *wq, uint32_t kinds)
{
    (void)wq;
    (void)kinds;
    ++wakes;
}

/** Model administrative state without the separate IPv6 state machine. */
void netconf_set_up(struct netdev *nd, int up)
{
    nd->up = up;
}

/** Clear the model address at unregister. */
int netconf_set_address(struct netdev *nd, uint32_t ip, uint32_t mask)
{
    nd->ip = ip;
    nd->mask = mask;
    return 0;
}

/** This model installs IPv4 routes only. */
void route6_forget(struct netdev *nd, int proto)
{
    (void)nd;
    (void)proto;
}

/** The fixture has no IPv6 discovery work to stop. */
void nd6_if_down(struct netdev *nd)
{
    (void)nd;
}

/** No IPv6 neighbours are installed here. */
int nd6_snapshot(struct nd6_row *out, int max)
{
    (void)out;
    (void)max;
    return 0;
}

/** Removing a nonexistent IPv6 neighbour reports a miss. */
int nd6_forget(struct netdev *nd, const uint8_t *addr)
{
    (void)nd;
    (void)addr;
    return -ENOENT;
}

/** The model drains data explicitly and needs no worker thread. */
int wlan_work_start(void)
{
    return 0;
}

/** Keep the same per-radio scan attachments as the real scan layer. */
void wlan_scan_attach(struct wlan_scan *scan, struct wlan *wlan, wlan_event_fn event,
                      struct wlan_radio *radio, const struct wlan_band *band,
                      struct wlan_bss_table *table)
{
    scan->wlan = wlan;
    scan->event = event;
    scan->radio = radio;
    scan->band = band;
    scan->table = table;
}

/** No scan work is queued by the model data worker. */
void wlan_scan_detach(struct wlan_scan *scan)
{
    (void)scan;
}

/** Record a hardware scan completion in the model scan. */
void wlan_scan_hw_done(struct wlan_scan *scan, int aborted)
{
    scan->hw_done = 1;
    scan->hw_aborted = aborted;
}

/** Initialize the model's empty BSS table. */
void wlan_bss_init(struct wlan_bss_table *table)
{
    k_memset(table, 0, sizeof(*table));
}

/** The model table owns no allocated BSS entries. */
void wlan_bss_destroy(struct wlan_bss_table *table)
{
    CHECK(table->count == 0);
}

/** Start only outside net_lock, optionally failing before any state changes. */
static int radio_start(struct wlan_radio *radio)
{
    (void)radio;
    CHECK(lock_depth == 0);
    ++starts;
    return start_error;
}

/** Stop only outside net_lock. */
static void radio_stop(struct wlan_radio *radio)
{
    (void)radio;
    CHECK(lock_depth == 0);
    ++stops;
}

/** Accept a channel configuration in the stub radio. */
static int radio_config(struct wlan_radio *radio, const struct wlan_channel *channel,
                        unsigned filters)
{
    (void)radio;
    (void)channel;
    (void)filters;
    CHECK(lock_depth == 0);
    return 0;
}

/** Copy the MPDU exactly as a lower radio must before returning. */
static int radio_tx(struct wlan_radio *radio, const uint8_t *frame, unsigned len,
                    const struct wlan_tx_info *info)
{
    (void)radio;
    (void)info;
    CHECK(lock_depth == 0);
    CHECK(len <= sizeof(sent));
    if (len > sizeof(sent))
        return -EMSGSIZE;
    k_memcpy(sent, frame, len);
    sent_len = len;
    ++transmissions;
    return 0;
}

/** Reject an optional state change to prove that registry state is atomic. */
static int radio_state(struct wlan_radio *radio, const struct wlan_bss *bss,
                       enum wlan_sta_state old, enum wlan_sta_state state)
{
    (void)radio;
    (void)bss;
    (void)old;
    (void)state;
    return state_error;
}

static const struct wlan_radio_ops radio_ops = {
    radio_start, radio_stop, radio_config, radio_tx, 0, radio_state, 0, 0, 0, 0};

/** Copy at the data worker boundary without calling a sleeping radio op. */
static int enqueue(struct wlan *wlan, const uint8_t *frame, unsigned len)
{
    (void)wlan;
    if (queued_len || len > sizeof(queued))
        return -ENOBUFS;
    k_memcpy(queued, frame, len);
    queued_len = len;
    return 0;
}

/** Run one model worker turn, using the real Ethernet conversion. */
static void drain(struct wlan *wlan)
{
    struct wlan_tx_info info;
    static uint8_t mpdu[IEEE80211_MAX_FRAME_LEN];
    unsigned len;

    CHECK(queued_len != 0);
    k_memset(&info, 0, sizeof(info));
    if (queued_len) {
        CHECK(wlan_encap(queued, queued_len, wlan->bss->bssid, 7, mpdu, sizeof(mpdu), &len) == 0);
        CHECK(wlan->radio->ops->tx(wlan->radio, mpdu, len, &info) == 0);
    }
    queued_len = 0;
}

/** Construct a valid, world-domain radio with a unique unicast address. */
static void setup(struct wlan_radio *radio, unsigned number)
{
    unsigned i;

    k_memset(radio, 0, sizeof(*radio));
    radio->ops = &radio_ops;
    radio->name = "stub";
    radio->mac[0] = 2;
    radio->mac[5] = (uint8_t)number;
    radio->country[0] = radio->country[1] = '0';
    radio->band.n_channels = 14;
    for (i = 0; i < 14; ++i) {
        radio->band.channels[i].number = (uint8_t)(i + 1);
        radio->band.channels[i].frequency = (uint16_t)(i == 13 ? 2484 : 2412 + 5 * i);
        radio->band.channels[i].max_power = 30;
    }
    radio->band.rates[0] = 10;
    radio->band.n_rates = 1;
}

/** Exercise identity, carrier, EAPOL and authorization through real sockets. */
static void station(void)
{
    struct wlan_radio radio, other;
    struct wlan *wlan = 0, *second = 0;
    static struct wlan_bss bss;
    static struct socket pae, ip;
    static const uint8_t payload[] = {0x45, 0, 0, 20};
    static uint8_t ethernet[ETH_HDR_SIZE + 4], mpdu[128], decoded[128];
    unsigned len, got, i;
    int index, lo_index;
    struct netdev_rt route;

    CHECK(netdev_probe() == 0);
    lo_index = netdev_loopback()->index;
    setup(&radio, 1);
    radio.ops = 0;
    CHECK(wlan_register_radio(&radio, &wlan) == -EINVAL && !wlan);
    radio.ops = &radio_ops;
    kheap_host_largest = 1;
    CHECK(wlan_register_radio(&radio, &wlan) == -ENOMEM && !wlan && !radio.wlan);
    kheap_host_largest = 0;
    CHECK(wlan_register_radio(&radio, &wlan) == 0);
    CHECK(wlan && !wlan->netdev.up && !wlan->started);
    if (!wlan)
        return;
    CHECK(wlan_by_index(radio.index) == wlan && wlan_at(0) == wlan);
    CHECK(wlan->scan.radio == &radio && wlan->scan.band == &wlan->band &&
          wlan->scan.table == &wlan->bss_table);
    wlan_radio_scan_done(&radio, 1);
    CHECK(wlan->scan.hw_done && wlan->scan.hw_aborted);
    CHECK(netdev_by_name("wlan0") == &wlan->netdev);
    CHECK(netdev_loopback()->index == lo_index);
    index = wlan->netdev.index;
    CHECK(netdev_by_index(index) == &wlan->netdev);
    CHECK(netdev_by_index(lo_index) == netdev_loopback());
    CHECK(!netdev_running(&wlan->netdev));
    wlan->blocked = 1;
    CHECK(wlan_start(wlan) == -ERFKILL);
    wlan->blocked = 0;
    start_error = -EIO;
    CHECK(wlan_start(wlan) == -EIO && !wlan->started);
    start_error = 0;
    CHECK(wlan_start(wlan) == 0 && wlan_start(wlan) == 0 && starts == 2);
    wlan->netdev.up = 1;
    wlan->tx_ethernet = enqueue;
    bss.bssid[0] = 2;
    bss.bssid[5] = 99;
    CHECK(wlan_set_state(wlan, WLAN_STA_ASSOCIATED, &bss) == 0);
    CHECK(netdev_running(&wlan->netdev));
    pae.protocol = ETH_TYPE_PAE;
    pae.rcvbuf = 8192;
    ip.protocol = ETH_TYPE_IP;
    ip.rcvbuf = 8192;
    CHECK(packet_bind(&pae, index) == 0 && packet_bind(&ip, index) == 0);
    CHECK(packet_send(&ip, index, bss.bssid, ETH_TYPE_IP, payload, sizeof(payload)) == -ENETDOWN);
    CHECK(transmissions == 0 && queued_len == 0);
    CHECK(packet_send(&pae, index, bss.bssid, ETH_TYPE_PAE, payload, sizeof(payload)) == 4);
    CHECK(transmissions == 0 && queued_len == ETH_HDR_SIZE + sizeof(payload));
    drain(wlan);
    CHECK(transmissions == 1 && sent_len == 32 + sizeof(payload));
    CHECK(sent[0] == 8 && sent[1] == 1 && sent[30] == 0x88 && sent[31] == 0x8e);
    CHECK(sent[24] == 0xaa && sent[25] == 0xaa && sent[26] == 3 && sent[29] == 0);
    for (i = 0; i < 6; ++i) {
        CHECK(sent[4 + i] == bss.bssid[i]);
        CHECK(sent[10 + i] == radio.mac[i]);
    }
    k_memcpy(ethernet, radio.mac, 6);
    k_memcpy(ethernet + 6, bss.bssid, 6);
    k_memcpy(ethernet + 14, payload, sizeof(payload));
    ethernet[12] = 8;
    ethernet[13] = 0;
    CHECK(wlan_rx_ethernet(wlan, ethernet, sizeof(ethernet)) == -EACCES);
    CHECK(ip_received == 0 && ip.rq.count == 0);
    ethernet[12] = 0x88;
    ethernet[13] = 0x8e;
    CHECK(wlan_rx_ethernet(wlan, ethernet, sizeof(ethernet)) == 0);
    CHECK(pae.rq.count == 1 && ip.rq.count == 0 && wakes == 1);
    CHECK(SOCK_DGRAM_DATA(pae.rq.head)[0] == payload[0]);
    state_error = -EIO;
    CHECK(wlan_set_state(wlan, WLAN_STA_AUTHORIZED, &bss) == -EIO);
    CHECK(wlan->state == WLAN_STA_ASSOCIATED);
    state_error = 0;
    CHECK(wlan_set_state(wlan, WLAN_STA_AUTHORIZED, &bss) == 0);
    CHECK(packet_send(&ip, index, bss.bssid, ETH_TYPE_IP, payload, sizeof(payload)) == 4);
    drain(wlan);
    CHECK(sent[30] == 8 && sent[31] == 0);
    ethernet[12] = 8;
    ethernet[13] = 0;
    CHECK(wlan_rx_ethernet(wlan, ethernet, sizeof(ethernet)) == 0);
    CHECK(ip_received == 1 && ip.rq.count == 1);
    CHECK(wlan_encap(ethernet, sizeof(ethernet), bss.bssid, 12, mpdu, sizeof(mpdu), &len) == 0);
    mpdu[1] = 2;
    k_memcpy(mpdu + 4, radio.mac, 6);
    k_memcpy(mpdu + 10, bss.bssid, 6);
    k_memcpy(mpdu + 16, bss.bssid, 6);
    CHECK(wlan_decap(mpdu, len, bss.bssid, decoded, sizeof(decoded), &got) == 0);
    CHECK(got == sizeof(ethernet));
    for (i = 0; i < got; ++i)
        CHECK(decoded[i] == ethernet[i]);
    got = 123;
    CHECK(wlan_decap(mpdu, 25, bss.bssid, decoded, sizeof(decoded), &got) == -EINVAL && got == 123);
    mpdu[1] |= 0x40;
    CHECK(wlan_decap(mpdu, len, bss.bssid, decoded, sizeof(decoded), &got) == -EINVAL);
    mpdu[1] = 3;
    CHECK(wlan_decap(mpdu, len, bss.bssid, decoded, sizeof(decoded), &got) == -EINVAL);
    CHECK(netdev_route_add(&wlan->netdev, 0xc0000200u, 0xffffff00u, 0, 17, 0) == 0);
    setup(&other, 2);
    CHECK(wlan_register_radio(&other, &second) == 0);
    CHECK(netdev_by_index(index) == &wlan->netdev && netdev_by_index(lo_index)->loopback);
    wlan_unregister_radio(wlan);
    CHECK(netdev_route_at(0, &route) == 0);
    CHECK(packet_send(&pae, index, bss.bssid, ETH_TYPE_PAE, payload, sizeof(payload)) == -ENODEV);
    packet_unbind(&pae);
    packet_unbind(&ip);
    CHECK(radio.wlan == 0 && !wlan_by_index(radio.index) && !netdev_by_index(index));
    CHECK(stops == 1 && netdev_count() == 1 && second && netdev_first() == &second->netdev);
    if (second)
        wlan_unregister_radio(second);
    CHECK(netdev_count() == 0 && netdev_listed() == 1);
}

/** Check bridge-tunnel, RFC 1042 exceptions and QoS header bounds. */
static void conversion(void)
{
    static const uint8_t ap[6] = {2, 3, 4, 5, 6, 7};
    uint8_t eth[18], mpdu[128], result[128], qos[128];
    unsigned n, got, i;

    k_memset(eth, 0, sizeof(eth));
    eth[0] = eth[6] = 2;
    eth[12] = 0x81;
    eth[13] = 0x37;
    eth[14] = 0x12;
    eth[15] = 0x34;
    CHECK(wlan_encap(eth, sizeof(eth), ap, 4095, mpdu, sizeof(mpdu), &n) == 0);
    CHECK(mpdu[29] == 0xf8 && mpdu[22] == 0xf0 && mpdu[23] == 0xff);
    mpdu[1] = 2;
    k_memcpy(mpdu + 4, eth, 6);
    k_memcpy(mpdu + 10, ap, 6);
    k_memcpy(mpdu + 16, eth + 6, 6);
    CHECK(wlan_decap(mpdu, n, ap, result, sizeof(result), &got) == 0);
    CHECK(got == sizeof(eth) && result[12] == 0x81 && result[13] == 0x37);
    mpdu[29] = 0;
    CHECK(wlan_decap(mpdu, n, ap, result, sizeof(result), &got) == 0);
    CHECK(got == sizeof(eth) + 8 && result[12] == 0 && result[13] == 12);
    CHECK(result[14] == 0xaa && result[20] == 0x81 && result[21] == 0x37);
    CHECK(wlan_encap(result, got, ap, 1, qos, sizeof(qos), &got) == 0);
    CHECK(got == n && qos[24] == 0xaa && qos[30] == 0x81 && qos[31] == 0x37);
    mpdu[30] = 0x88;
    mpdu[31] = 0x8e;
    k_memcpy(qos, mpdu, 24);
    qos[0] |= 0x80;
    qos[1] |= 0x80;
    k_memset(qos + 24, 0, 6);
    k_memcpy(qos + 30, mpdu + 24, n - 24);
    CHECK(wlan_decap(qos, n + 6, ap, result, sizeof(result), &got) == 0);
    CHECK(got == sizeof(eth) && result[12] == 0x88 && result[13] == 0x8e);
    for (i = 0; i < n + 6; ++i) {
        if (i >= 38)
            break;
        got = 555;
        CHECK(wlan_decap(qos, i, ap, result, sizeof(result), &got) == -EINVAL);
        CHECK(got == 555);
    }
    qos[24] = IEEE80211_QOS_CTL_A_MSDU_PRESENT;
    CHECK(wlan_decap(qos, n + 6, ap, result, sizeof(result), &got) == -EINVAL);
    qos[24] = 0;
    got = 555;
    result[0] = 99;
    CHECK(wlan_decap(qos, n + 6, ap, result, 4, &got) == -EMSGSIZE);
    CHECK(got == 555 && result[0] == 99);
    mpdu[0] |= IEEE80211_STYPE_NULLFUNC;
    CHECK(wlan_decap(mpdu, n, ap, result, sizeof(result), &got) == -EINVAL);
    got = 555;
    result[0] = 99;
    CHECK(wlan_encap(eth, sizeof(eth), ap, 0, result, 4, &got) == -EMSGSIZE);
    CHECK(got == 555 && result[0] == 99);
    CHECK(wlan_encap(eth, sizeof(eth), ap, 4096, result, sizeof(result), &got) == -EINVAL);
    eth[12] = 0;
    eth[13] = 3;
    eth[14] = 0x42;
    eth[15] = 0x42;
    eth[16] = 3;
    CHECK(wlan_encap(eth, sizeof(eth), ap, 0, mpdu, sizeof(mpdu), &n) == 0);
    CHECK(n == 27 && mpdu[24] == 0x42 && mpdu[26] == 3);
    mpdu[1] = 2;
    k_memcpy(mpdu + 10, ap, 6);
    CHECK(wlan_decap(mpdu, n, ap, result, sizeof(result), &got) == 0);
    CHECK(got == 17 && result[12] == 0 && result[13] == 3 && result[14] == 0x42);
    eth[13] = 5;
    CHECK(wlan_encap(eth, sizeof(eth), ap, 0, result, sizeof(result), &got) == -EINVAL);
    eth[12] = eth[13] = 0;
    CHECK(wlan_encap(eth, sizeof(eth), ap, 0, result, sizeof(result), &got) == -EINVAL);
}

/** Check world restrictions and ensure EEPROM flags and power only narrow. */
static void regulatory(void)
{
    struct wlan_radio radio;
    struct wlan wlan;

    setup(&radio, 1);
    k_memset(&wlan, 0, sizeof(wlan));
    wlan.band = radio.band;
    wlan.band.channels[0].flags = WLAN_CHANNEL_DISABLED;
    wlan.band.channels[1].max_power = 10;
    wlan_reg_apply(&wlan.band, radio.country);
    CHECK(!wlan_channel(&wlan, 2412, 0));
    CHECK(wlan_channel(&wlan, 2417, 1)->max_power == 10);
    CHECK(wlan_channel(&wlan, 2462, 1)->max_power == 20);
    CHECK(wlan_channel(&wlan, 2467, 0) && !wlan_channel(&wlan, 2467, 1));
    CHECK(wlan_channel(&wlan, 2472, 0) && !wlan_channel(&wlan, 2484, 0));
    wlan_reg_apply(&wlan.band, "US");
    CHECK(!wlan_channel(&wlan, 2467, 0) && !wlan_channel(&wlan, 2472, 0));
}

/** Run allocation-free conversions and the model station; create no files. */
int main(void)
{
    station();
    conversion();
    regulatory();
    return failures ? 1 : 0;
}
