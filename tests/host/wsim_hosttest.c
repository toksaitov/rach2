/**
 * Pass frames between two software radios through the relay's medium, as
 * two wsimrelay programs on loopback join them: wsim.c's radios and their
 * /dev/wsim records, and wsimmedium.c's checks, rewrites, answers and
 * timeouts, against a model of the station layer.
 *
 * host-deps: utils/wsimmedium.c
 * host-deps: rach2/sys/lib/kstring.c rach2/sys/lib/kctypes.c
 * host-deps: tests/host/compat/kheap_host.c
 *
 * The test includes wsim.c whole. The model station records each frame a
 * radio receives and each transmit status, and its registry stops a radio
 * when it unregisters it, as the real one does. A datagram goes from one
 * medium to the other by a copy, where a relay sends it over UDP.
 */
#include "network/wireless/wsim.c"
#include "wsimmedium.h"

extern int printf(const char *format, ...);
extern int memcmp(const void *a, const void *b, unsigned long n);
extern unsigned long kheap_host_largest;

static unsigned failures;
#define CHECK(x)                                                            \
    do {                                                                    \
        if (!(x)) {                                                         \
            printf("wsim_hosttest: line %u: %s\n", (unsigned)__LINE__, #x); \
            ++failures;                                                     \
        }                                                                   \
    } while (0)

/* ---- the machine around the code ---- */

static uint64_t now = 5000;
static const char *cmdline_wsim; /* the value of rach2.wsim, or NULL */
static unsigned logs;

uint64_t tick_count(void)
{
    return now;
}

int kcmdline_value(const char *key, char *out, unsigned int outlen)
{
    unsigned i;

    (void)key;
    if (!cmdline_wsim) {
        return 0;
    }
    for (i = 0; cmdline_wsim[i] && i + 1 < outlen; i++) {
        out[i] = cmdline_wsim[i];
    }
    out[i] = 0;
    return 1;
}

void klog(int level, const char *subsystem, const char *format, ...)
{
    (void)level;
    (void)subsystem;
    (void)format;
    logs++;
}

static int wait_ends_by_signal;

/** A wait the test never satisfies: a blocking read with nothing queued is
 * ended by a signal. */
int wq_wait_event(wait_queue_t *wq, wq_cond_fn cond, void *arg)
{
    (void)wq;
    if (cond(arg)) {
        return 0;
    }
    wait_ends_by_signal++;
    return -EINTR;
}

static unsigned wakes;

void wq_wake(wait_queue_t *wq, uint32_t kinds)
{
    (void)wq;
    (void)kinds;
    wakes++;
}

void wait_queue_init(wait_queue_t *wq)
{
    wq->head = NULL;
}

/** Copy from the caller; a NULL source is a bad pointer. */
int copy_from_user(void *dst, const void __user *src, size_t len)
{
    if (!src) {
        return -EFAULT;
    }
    k_memcpy(dst, src, len);
    return 0;
}

/** Copy to the caller; a NULL destination is a bad pointer. */
int copy_to_user(void __user *dst, const void *src, size_t len)
{
    if (!dst) {
        return -EFAULT;
    }
    k_memcpy(dst, src, len);
    return 0;
}

/* ---- the station layer ---- */

#define MAX_LOG 160

static struct {
    struct wlan_radio *radio;
    uint8_t frame[256];
    unsigned len;
    struct wlan_rx_status status;
} rxs[MAX_LOG];
static unsigned n_rx;
static int rx_result; /* what the station's receive answers */

static struct {
    struct wlan_radio *radio;
    struct wlan_tx_status status;
} txs[MAX_LOG];
static unsigned n_tx;

static unsigned registered, unregistered;
static int register_result;

static int st_register(struct wlan_radio *radio, struct wlan **out)
{
    if (register_result != 0) {
        return register_result;
    }
    registered++;
    *out = (struct wlan *)(void *)radio; /* opaque to wsim */
    return 0;
}

static void st_unregister(struct wlan *wlan)
{
    struct wlan_radio *radio = (struct wlan_radio *)(void *)wlan;

    unregistered++;
    radio->ops->stop(radio);
}

static int st_rx(struct wlan_radio *radio, const uint8_t *frame, unsigned len,
                 const struct wlan_rx_status *status)
{
    if (rx_result != 0) {
        return rx_result;
    }
    if (n_rx < MAX_LOG && len <= sizeof(rxs[0].frame)) {
        rxs[n_rx].radio = radio;
        k_memcpy(rxs[n_rx].frame, frame, len);
        rxs[n_rx].len = len;
        rxs[n_rx].status = *status;
    }
    n_rx++;
    return 0;
}

static void st_tx_status(struct wlan_radio *radio, const struct wlan_tx_status *status)
{
    if (n_tx < MAX_LOG) {
        txs[n_tx].radio = radio;
        txs[n_tx].status = *status;
    }
    n_tx++;
}

static const struct wsim_station station = {st_register, st_unregister, st_rx, st_tx_status};

/* ---- the two radios and their relays ---- */

static fd_entry_t e0, e1;
static struct open_file o0, o1;
static struct wlan_radio *r0, *r1;
static struct medium m0, m1;
static uint8_t mac0[6] = {0x02, 0x77, 0x73, 0, 0, 0};
static uint8_t mac1[6] = {0x02, 0x77, 0x73, 0, 0, 1};
static const uint8_t bcast[6] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
static const uint8_t stranger[6] = {0x02, 0x11, 0x22, 0x33, 0x44, 0x55};

/** The radio a descriptor carries. */
static struct wlan_radio *radio_of(fd_entry_t *e)
{
    return &((struct wsim *)e->priv)->radio;
}

/** Write in `f` a data frame from `from` to `to`, of `len` bytes. */
static unsigned data_frame(uint8_t *f, const uint8_t *to, const uint8_t *from, unsigned len)
{
    unsigned i;

    k_memset(f, 0, len);
    f[0] = 0x08; /* data */
    k_memcpy(f + 4, to, 6);
    k_memcpy(f + 10, from, 6);
    k_memcpy(f + 16, from, 6);
    for (i = 24; i < len; i++) {
        f[i] = (uint8_t)(i * 7);
    }
    return len;
}

/** Fill `info` for a frame of cookie `cookie` at 2437 MHz: two attempts at
 * 11 Mbit/s, then three at 1 Mbit/s. */
static void tx_info(struct wlan_tx_info *info, uint64_t cookie)
{
    unsigned i;

    k_memset(info, 0, sizeof(*info));
    info->cookie = cookie;
    info->frequency = 2437;
    info->tid = WLAN_NON_QOS_TID;
    info->hw_key = -1;
    for (i = 0; i < WLAN_TX_RATE_SERIES; i++) {
        info->rates[i].index = -1;
    }
    info->rates[0].index = 3;
    info->rates[0].count = 2;
    info->rates[1].index = 0;
    info->rates[1].count = 3;
}

/** Read one record from `e` and pass it through medium `m` towards the
 * peer; the datagram in `d`, its length the return, 0 on any failure. */
static unsigned out_of(fd_entry_t *e, struct medium *m, uint8_t *d)
{
    int32_t n = wsim_read(e, (char *)d, MEDIUM_RECORD_MAX);

    if (n <= 0) {
        return 0;
    }
    return medium_from_radio(m, d, (unsigned)n, (unsigned long)now) == MEDIUM_TO_PEER ? (unsigned)n
                                                                                      : 0;
}

/** Deliver datagram `d` of `len` bytes to the radio of `e` through medium
 * `m`, and its status back through `back` to the radio of `src`. Returns
 * what the write to the receiving radio returned. */
static int32_t carry(uint8_t *d, unsigned len, fd_entry_t *e, struct medium *m, fd_entry_t *src,
                     struct medium *back)
{
    struct medium_reply reply;
    uint8_t status[MEDIUM_HEADER];
    int32_t w;

    if (medium_from_peer(m, d, len, &reply) != MEDIUM_TO_RADIO || !reply.valid) {
        return -1000;
    }
    w = wsim_write(e, (const char *)d, len);
    medium_answer(m, &reply, w == (int32_t)len, status);
    if (medium_from_peer(back, status, MEDIUM_HEADER, &reply) != MEDIUM_TO_RADIO || reply.valid) {
        return -2000;
    }
    CHECK(wsim_write(src, (const char *)status, MEDIUM_HEADER) == MEDIUM_HEADER);
    return w;
}

/** Send `frame` on radio `r` and carry it to the other radio and its status
 * back. Returns the receiving write's result, or a negative test code. */
static int32_t send_frame(fd_entry_t *src, struct medium *ms, fd_entry_t *dst, struct medium *md,
                          const uint8_t *frame, unsigned len, const struct wlan_tx_info *info)
{
    static uint8_t d[MEDIUM_RECORD_MAX];
    unsigned n;

    if (radio_of(src)->ops->tx(radio_of(src), frame, len, info) != 0) {
        return -3000;
    }
    n = out_of(src, ms, d);
    if (n == 0) {
        return -4000;
    }
    return carry(d, n, dst, md, src, ms);
}

/* ---- the rows ---- */

/** Opens before and after attach, and the radios' identities. */
static void row_open(void)
{
    struct wsim_info info;

    o0.status_flags = 0;
    e0.ofd = &o0;
    e1.ofd = &o1;
    CHECK(wsim_open(&e0) == -ENODEV);
    CHECK(wsim_attach(&station) == 0);
    CHECK(wsim_open(&e0) == 0 && wsim_open(&e1) == 0);
    CHECK(registered == 2);
    r0 = radio_of(&e0);
    r1 = radio_of(&e1);
    CHECK(memcmp(r0->mac, mac0, 6) == 0 && memcmp(r1->mac, mac1, 6) == 0);
    CHECK(r0->band.n_channels == 14 && r0->band.channels[13].frequency == 2484);
    CHECK(r0->band.n_rates == 12 && r0->band.ht_supported && r0->hw_ciphers == 0);
    CHECK(r0->ops->set_key == NULL && r0->ops->scan_start == NULL);
    CHECK(wsim_ioctl(&e1, WSIM_IOC_INFO, (uintptr_t)&info) == 0);
    CHECK(info.radio == 1 && memcmp(info.mac, mac1, 6) == 0 && !info.started);
    CHECK(wsim_ioctl(&e1, 0x5413u, (uintptr_t)&info) == -ENOTTY);
    CHECK(wsim_ioctl(&e1, WSIM_IOC_INFO, 0) == -EFAULT);
    CHECK(sizeof(struct wsim_info) == 16);
}

/** Start both radios on channel 6; frames before the start are refused. */
static void row_start(void)
{
    struct wlan_tx_info info;
    struct wsim_info wi;
    struct wlan_channel ch6 = {2437, 6, 0, 20}, bad = {2400, 0, 0, 20};
    uint8_t f[64];
    uint64_t tsf;

    tx_info(&info, 1);
    CHECK(r0->ops->tx(r0, f, data_frame(f, mac1, mac0, 40), &info) == -ENETDOWN);
    CHECK(r0->ops->start(r0) == 0 && r1->ops->start(r1) == 0);
    CHECK(r0->ops->config(r0, &ch6, 0) == 0 && r1->ops->config(r1, &ch6, 0) == 0);
    CHECK(r0->ops->config(r0, &bad, 0) == -EINVAL);
    CHECK(wsim_ioctl(&e0, WSIM_IOC_INFO, (uintptr_t)&wi) == 0);
    CHECK(wi.started == 1 && wi.frequency == 2437 && wi.radio == 0);
    now += 7;
    CHECK(r0->ops->get_tsf(r0, &tsf) == 0 && tsf == 7000);
    medium_init(&m0, 0, 0, mac0, -50, 100);
    medium_init(&m1, 1, 0, mac1, -42, 100);
}

/** A unicast frame from radio 0 reaches radio 1, which acknowledges it. */
static void row_unicast(void)
{
    static uint8_t d[MEDIUM_RECORD_MAX];
    struct wlan_tx_info info;
    uint8_t f[100];
    unsigned len = data_frame(f, mac1, mac0, 100), rx_before = n_rx, tx_before = n_tx, n;

    tx_info(&info, 0x1234);
    CHECK(r0->ops->tx(r0, f, len, &info) == 0);
    n = (unsigned)wsim_read(&e0, (char *)d, sizeof(d));
    CHECK(n == MEDIUM_HEADER + len);
    CHECK(d[0] == 'W' && d[1] == 'S' && d[2] == 'I' && d[3] == 'M' && d[4] == 1 && d[5] == 1);
    CHECK(wsim_get16(d + 6) == 40 && wsim_get16(d + 8) == 0);
    CHECK(wsim_get16(d + 10) == WLAN_RELAY_F_TX && wsim_get16(d + 12) == 2437);
    CHECK(wsim_get16(d + 14) == 110 && wsim_get16(d + 18) == len);
    CHECK(wsim_get64(d + 20) != 0 && wsim_get64(d + 20) != 0x1234);
    CHECK(memcmp(d + 40, f, len) == 0);
    CHECK(medium_check(d, n) == MEDIUM_FRAME);
    CHECK(medium_from_radio(&m0, d, n, (unsigned long)now) == MEDIUM_TO_PEER);
    CHECK(carry(d, n, &e1, &m1, &e0, &m0) == (int32_t)n);
    CHECK(n_rx == rx_before + 1 && rxs[rx_before].radio == r1);
    CHECK(rxs[rx_before].len == len && memcmp(rxs[rx_before].frame, f, len) == 0);
    CHECK(rxs[rx_before].status.frequency == 2437 && rxs[rx_before].status.signal == -42);
    CHECK(rxs[rx_before].status.rate == 3 && rxs[rx_before].status.rate_flags == 0);
    CHECK(rxs[rx_before].status.hw_key == -1 && rxs[rx_before].status.tsf == 7000);
    CHECK(n_tx == tx_before + 1 && txs[tx_before].radio == r0);
    CHECK(txs[tx_before].status.cookie == 0x1234);
    CHECK(txs[tx_before].status.flags == WLAN_TX_ACKED && txs[tx_before].status.ack_signal == -42);
    CHECK(txs[tx_before].status.rates[0].index == 3 && txs[tx_before].status.rates[0].count == 1);
    CHECK(txs[tx_before].status.rates[1].index == -1 && txs[tx_before].status.rates[1].count == 0);
    CHECK(m0.sent == 1 && m1.received == 1 && medium_next(&m0, (unsigned long)now) == -1);
}

/** A beacon from radio 1 goes without acknowledgement; radio 0 hears it. */
static void row_broadcast(void)
{
    struct wlan_tx_info info;
    uint8_t f[60];
    unsigned rx_before = n_rx, tx_before = n_tx;

    data_frame(f, bcast, mac1, 60);
    f[0] = 0x80; /* beacon */
    tx_info(&info, 77);
    CHECK(send_frame(&e1, &m1, &e0, &m0, f, 60, &info) == 100);
    CHECK(n_rx == rx_before + 1 && rxs[rx_before].radio == r0 &&
          rxs[rx_before].status.signal == -50);
    CHECK(n_tx == tx_before + 1 && txs[tx_before].radio == r1 &&
          txs[tx_before].status.cookie == 77);
    CHECK(txs[tx_before].status.flags == 0 && txs[tx_before].status.ack_signal == 0);
    CHECK(txs[tx_before].status.rates[0].count == 1 && txs[tx_before].status.rates[1].index == -1);
}

/** Frames radio 1 does not hear fail with the whole series tried. */
static void row_unheard(void)
{
    struct wlan_tx_info info;
    struct wlan_channel ch1 = {2412, 1, 0, 20}, ch6 = {2437, 6, 0, 20};
    uint8_t f[40];
    unsigned tx_before = n_tx, rx_before = n_rx;

    tx_info(&info, 5);
    data_frame(f, stranger, mac0, 40);
    CHECK(send_frame(&e0, &m0, &e1, &m1, f, 40, &info) == 0);
    CHECK(n_tx == tx_before + 1 && txs[tx_before].status.flags == 0);
    CHECK(txs[tx_before].status.rates[0].index == 3 && txs[tx_before].status.rates[0].count == 2);
    CHECK(txs[tx_before].status.rates[1].index == 0 && txs[tx_before].status.rates[1].count == 3);
    CHECK(txs[tx_before].status.rates[2].index == -1);

    /* Tuned to another channel. */
    CHECK(r1->ops->config(r1, &ch1, 0) == 0);
    data_frame(f, mac1, mac0, 40);
    CHECK(send_frame(&e0, &m0, &e1, &m1, f, 40, &info) == 0);
    CHECK(n_tx == tx_before + 2 && txs[tx_before + 1].status.flags == 0);
    CHECK(r1->ops->config(r1, &ch6, 0) == 0);

    /* A receive queue that is full. */
    rx_result = -ENOBUFS;
    CHECK(send_frame(&e0, &m0, &e1, &m1, f, 40, &info) == 0);
    CHECK(n_tx == tx_before + 3 && txs[tx_before + 2].status.flags == 0);
    rx_result = 0;
    CHECK(n_rx == rx_before);
}

/** Rates: MCS with a short guard interval, a short preamble, and refusals. */
static void row_rates(void)
{
    struct wlan_tx_info info;
    uint8_t f[40];
    unsigned rx_before = n_rx;

    data_frame(f, mac1, mac0, 40);
    tx_info(&info, 9);
    info.rates[0].index = 7;
    info.rates[0].flags = WLAN_RATE_MCS | WLAN_RATE_SHORT_GI;
    CHECK(send_frame(&e0, &m0, &e1, &m1, f, 40, &info) == 80);
    CHECK(rxs[rx_before].status.rate == 7);
    CHECK(rxs[rx_before].status.rate_flags == (WLAN_RATE_MCS | WLAN_RATE_SHORT_GI));
    info.rates[0].index = 2;
    info.rates[0].flags = WLAN_RATE_SHORT_PREAMBLE;
    CHECK(send_frame(&e0, &m0, &e1, &m1, f, 40, &info) == 80);
    CHECK(rxs[rx_before + 1].status.rate == 2);
    CHECK(rxs[rx_before + 1].status.rate_flags == WLAN_RATE_SHORT_PREAMBLE);
    info.rates[0].index = 8;
    info.rates[0].flags = WLAN_RATE_MCS;
    CHECK(r0->ops->tx(r0, f, 40, &info) == -EINVAL);
    info.rates[0].index = 12;
    info.rates[0].flags = 0;
    CHECK(r0->ops->tx(r0, f, 40, &info) == -EINVAL);
    info.rates[0].index = -1;
    CHECK(r0->ops->tx(r0, f, 40, &info) == -EINVAL);
    tx_info(&info, 9);
    info.frequency = 2400;
    CHECK(r0->ops->tx(r0, f, 40, &info) == -EINVAL);
    tx_info(&info, 9);
    CHECK(r0->ops->tx(r0, f, 9, &info) == -EINVAL);
    CHECK(r0->ops->tx(r0, f, IEEE80211_MAX_FRAME_LEN + 1, &info) == -EINVAL);
    CHECK(e0.priv && ((struct wsim *)e0.priv)->count == 0);
}

/** Filters: control frames and other stations' frames. */
static void row_filters(void)
{
    struct wlan_tx_info info;
    struct wlan_channel ch6 = {2437, 6, 0, 20};
    uint8_t rts[16], f[40];
    unsigned tx_before = n_tx, rx_before = n_rx;

    k_memset(rts, 0, sizeof(rts));
    rts[0] = 0xb4; /* RTS */
    k_memcpy(rts + 4, mac1, 6);
    k_memcpy(rts + 10, mac0, 6);
    tx_info(&info, 11);
    CHECK(send_frame(&e0, &m0, &e1, &m1, rts, 16, &info) == 0);
    CHECK(r1->ops->config(r1, &ch6, WLAN_FILTER_CONTROL) == 0);
    CHECK(send_frame(&e0, &m0, &e1, &m1, rts, 16, &info) == 56);
    CHECK(txs[tx_before + 1].status.flags == WLAN_TX_ACKED);

    /* Promiscuous: heard, but not acknowledged, since it is not for us. */
    CHECK(r1->ops->config(r1, &ch6, WLAN_FILTER_PROMISC) == 0);
    data_frame(f, stranger, mac0, 40);
    CHECK(send_frame(&e0, &m0, &e1, &m1, f, 40, &info) == 80);
    CHECK(txs[tx_before + 2].status.flags == 0);
    CHECK(n_rx == rx_before + 2);
    CHECK(r1->ops->config(r1, &ch6, 0) == 0);
}

/** A frame with no status from the peer fails after the timeout; the late
 * status is dropped by the medium and refused by the radio. */
static void row_timeout(void)
{
    static uint8_t d[MEDIUM_RECORD_MAX];
    struct medium_reply reply;
    struct wlan_tx_info info;
    uint8_t f[40], status[MEDIUM_HEADER], late[MEDIUM_HEADER];
    unsigned tx_before = n_tx, n, refused;

    data_frame(f, mac1, mac0, 40);
    tx_info(&info, 21);
    CHECK(r0->ops->tx(r0, f, 40, &info) == 0);
    n = out_of(&e0, &m0, d);
    CHECK(n == 80);
    CHECK(medium_next(&m0, (unsigned long)now) == 100);
    CHECK(medium_next(&m0, (unsigned long)now + 60) == 40);
    CHECK(medium_expire(&m0, (unsigned long)now + 99, status) == 0);
    CHECK(medium_expire(&m0, (unsigned long)now + 100, status) == 1);
    CHECK(medium_next(&m0, (unsigned long)now + 100) == -1);
    k_memcpy(late, status, MEDIUM_HEADER);
    CHECK(wsim_write(&e0, (const char *)status, MEDIUM_HEADER) == MEDIUM_HEADER);
    CHECK(n_tx == tx_before + 1 && txs[tx_before].status.cookie == 21);
    CHECK(txs[tx_before].status.flags == 0 && txs[tx_before].status.rates[1].count == 3);
    CHECK(m0.expired == 1);

    /* The peer answers at last. */
    CHECK(medium_from_peer(&m1, d, n, &reply) == MEDIUM_TO_RADIO && reply.valid);
    CHECK(wsim_write(&e1, (const char *)d, n) == (int32_t)n);
    medium_answer(&m1, &reply, 1, status);
    refused = (unsigned)m0.refused;
    CHECK(medium_from_peer(&m0, status, MEDIUM_HEADER, &reply) == MEDIUM_DROP);
    CHECK(m0.refused == refused + 1);
    CHECK(wsim_write(&e0, (const char *)late, MEDIUM_HEADER) == -ENOENT);
    CHECK(n_tx == tx_before + 1);
}

/** Store `v` in two big-endian bytes, for the refusal rows. */
static void set16(uint8_t *p, unsigned v)
{
    wsim_put16(p, v);
}

/** Write in `r` a receive record for radio 1 of a 40-byte frame to it. */
static unsigned rx_record(uint8_t *r)
{
    k_memset(r, 0, 80);
    r[0] = 'W';
    r[1] = 'S';
    r[2] = 'I';
    r[3] = 'M';
    r[4] = 1;
    r[5] = WLAN_RELAY_FRAME;
    set16(r + 6, 40);
    set16(r + 8, 1);
    set16(r + 12, 2437);
    set16(r + 14, 10);
    set16(r + 16, (unsigned)-30 & 0xffff);
    set16(r + 18, 40);
    data_frame(r + 40, mac1, mac0, 40);
    return 80;
}

/** Every rule of the format refuses its record, in the radio and in the
 * medium alike. */
static void row_refusals(void)
{
    static const unsigned offsets[] = {0, 4, 5, 6, 7, 8, 10, 12, 14, 18, 20, 36, 37, 38, 39};
    static const unsigned values[] = {'X', 2, 3, 41, 1, 0, 0x0080, 2400, 15, 41, 0, 1, 1, 1, 1};
    uint8_t r[80];
    unsigned i, rx_before = n_rx;
    unsigned refused = (unsigned)m1.refused;
    struct medium_reply reply;

    CHECK(wsim_write(&e1, (const char *)r, rx_record(r)) == 80);
    CHECK(n_rx == rx_before + 1 && rxs[rx_before].status.signal == -30);
    CHECK(rxs[rx_before].status.rate == 0 && rxs[rx_before].status.tsf != 0);
    rx_before++;
    for (i = 0; i < sizeof(offsets) / sizeof(offsets[0]); i++) {
        rx_record(r);
        if (offsets[i] == 0 || offsets[i] == 4 || offsets[i] == 5 || offsets[i] >= 36) {
            r[offsets[i]] = (uint8_t)values[i];
        } else if (offsets[i] == 7) {
            r[7] = 41; /* the header length's low byte */
        } else if (offsets[i] == 8) {
            set16(r + 8, 0); /* radio 0 through radio 1's descriptor */
        } else if (offsets[i] == 20) {
            r[27] = 1; /* a cookie on a receive record */
        } else {
            set16(r + offsets[i], values[i]);
        }
        CHECK(wsim_write(&e1, (const char *)r, 80) == -EINVAL);
    }
    rx_record(r);
    set16(r + 10, WLAN_RELAY_F_TX);
    CHECK(wsim_write(&e1, (const char *)r, 80) == -EINVAL);
    rx_record(r);
    set16(r + 10, WLAN_RELAY_F_NO_ACK);
    CHECK(wsim_write(&e1, (const char *)r, 80) == -EINVAL);
    CHECK(medium_check(r, 80) == -1);
    rx_record(r);
    set16(r + 10, WLAN_RELAY_F_ACKED);
    CHECK(wsim_write(&e1, (const char *)r, 80) == -EINVAL);
    CHECK(medium_check(r, 80) == -1);
    rx_record(r);
    set16(r + 10, WLAN_RELAY_F_SHORT_GI);
    CHECK(wsim_write(&e1, (const char *)r, 80) == -EINVAL);
    rx_record(r);
    set16(r + 10, WLAN_RELAY_F_MCS);
    set16(r + 14, 8);
    CHECK(wsim_write(&e1, (const char *)r, 80) == -EINVAL);
    rx_record(r);
    set16(r + 18, 9);
    CHECK(wsim_write(&e1, (const char *)r, 49) == -EINVAL);
    CHECK(medium_check(r, 49) == -1);
    rx_record(r);
    CHECK(wsim_write(&e1, (const char *)r, 79) == -EINVAL);
    CHECK(wsim_write(&e1, (const char *)r, 39) == -EINVAL);
    CHECK(wsim_write(&e1, (const char *)r, MEDIUM_RECORD_MAX + 1) == -EINVAL);
    CHECK(wsim_write(&e1, NULL, 80) == -EFAULT);
    CHECK(n_rx == rx_before);

    /* The medium refuses a frame that no radio sent, as a peer's datagram. */
    rx_record(r);
    CHECK(medium_check(r, 80) == MEDIUM_FRAME);
    CHECK(medium_from_peer(&m1, r, 80, &reply) == MEDIUM_DROP);
    r[0] = 'X';
    CHECK(medium_from_peer(&m1, r, 80, &reply) == MEDIUM_DROP);
    CHECK(medium_from_peer(&m1, r, 12, &reply) == MEDIUM_DROP);
    CHECK(m1.refused == refused + 3);
    /* Nor does it send on a record of another radio. */
    rx_record(r);
    set16(r + 10, WLAN_RELAY_F_TX);
    r[27] = 1;
    CHECK(medium_from_radio(&m0, r, 80, (unsigned long)now) == MEDIUM_DROP);
}

/** Status records that the format or the frame refuses. */
static void row_status_refusals(void)
{
    static uint8_t d[MEDIUM_RECORD_MAX];
    struct wlan_tx_info info;
    uint8_t f[40], s[MEDIUM_HEADER];
    uint64_t cookie;
    unsigned tx_before, n;

    data_frame(f, bcast, mac0, 40);
    tx_info(&info, 31);
    CHECK(r0->ops->tx(r0, f, 40, &info) == 0);
    n = (unsigned)wsim_read(&e0, (char *)d, sizeof(d));
    CHECK(n == 80 && (wsim_get16(d + 10) & WLAN_RELAY_F_NO_ACK));
    cookie = wsim_get64(d + 20);
    tx_before = n_tx;
    k_memset(s, 0, sizeof(s));
    k_memcpy(s, d, 10);
    s[5] = WLAN_RELAY_TX_STATUS;
    wsim_put64(s + 20, cookie);
    set16(s + 10, WLAN_RELAY_F_TX | WLAN_RELAY_F_ACKED);
    CHECK(wsim_write(&e0, (const char *)s, 40) == -EINVAL); /* acknowledged broadcast */
    set16(s + 10, WLAN_RELAY_F_TX);
    CHECK(wsim_write(&e0, (const char *)s, 40) == -EINVAL); /* no outcome */
    CHECK(medium_check(s, 40) == -1);
    set16(s + 10, WLAN_RELAY_F_TX | WLAN_RELAY_F_ACKED | WLAN_RELAY_F_FAILED);
    CHECK(wsim_write(&e0, (const char *)s, 40) == -EINVAL); /* two outcomes */
    CHECK(medium_check(s, 40) == -1);
    set16(s + 10, WLAN_RELAY_F_NO_ACK);
    CHECK(wsim_write(&e0, (const char *)s, 40) == -EINVAL); /* no TX */
    CHECK(medium_check(s, 40) == -1);
    set16(s + 10, WLAN_RELAY_F_TX | WLAN_RELAY_F_NO_ACK);
    set16(s + 18, 1);
    CHECK(wsim_write(&e0, (const char *)s, 40) == -EINVAL); /* a payload's length */
    set16(s + 18, 0);
    wsim_put64(s + 20, 0);
    CHECK(medium_check(s, 40) == -1); /* no cookie */
    CHECK(wsim_write(&e0, (const char *)s, 40) == -ENOENT);
    wsim_put64(s + 20, cookie);
    CHECK(medium_check(s, 40) == MEDIUM_STATUS);
    CHECK(n_tx == tx_before);
    CHECK(wsim_write(&e0, (const char *)s, 40) == 40);
    CHECK(n_tx == tx_before + 1 && txs[tx_before].status.cookie == 31);
    CHECK(wsim_write(&e0, (const char *)s, 40) == -ENOENT); /* once only */
}

/** The queue: reads, polls, a short buffer, a bad buffer, a full queue, a
 * full pending table, and a stop that fails what waits. */
static void row_queue(void)
{
    static uint8_t d[MEDIUM_RECORD_MAX];
    struct wait_queue *wq;
    struct wlan_tx_info info;
    uint8_t f[40];
    unsigned i, tx_before;

    data_frame(f, mac1, mac0, 40);
    tx_info(&info, 40);
    CHECK(wsim_poll(&e0, POLLIN | POLLOUT, &wq) == POLLOUT && wq != NULL);
    o0.status_flags = OFD_NONBLOCK;
    CHECK(wsim_read(&e0, (char *)d, sizeof(d)) == -EAGAIN);
    o0.status_flags = 0;
    CHECK(wsim_read(&e0, (char *)d, sizeof(d)) == -EINTR && wait_ends_by_signal == 1);

    CHECK(r0->ops->tx(r0, f, 40, &info) == 0);
    CHECK(wsim_poll(&e0, POLLIN, &wq) == POLLIN);
    CHECK(wsim_read(&e0, (char *)d, 79) == -EINVAL);
    tx_before = n_tx;
    CHECK(wsim_read(&e0, NULL, sizeof(d)) == -EFAULT);
    CHECK(n_tx == tx_before + 1 && txs[tx_before].status.cookie == 40);
    CHECK(txs[tx_before].status.flags == 0);

    for (i = 0; i < WSIM_QUEUE; i++) {
        info.cookie = 100 + i;
        CHECK(r0->ops->tx(r0, f, 40, &info) == 0);
    }
    CHECK(r0->ops->tx(r0, f, 40, &info) == -ENOBUFS);
    for (i = 0; i < WSIM_QUEUE; i++) {
        CHECK(wsim_read(&e0, (char *)d, sizeof(d)) == 80);
    }
    CHECK(r0->ops->tx(r0, f, 40, &info) == -ENOBUFS); /* every frame awaits status */
    kheap_host_largest = 1;
    tx_before = n_tx;
    r0->ops->stop(r0);
    kheap_host_largest = 0;
    CHECK(n_tx == tx_before + WSIM_PENDING);
    CHECK(txs[tx_before].status.flags == 0 && txs[tx_before].status.rates[1].count == 3);
    CHECK(r0->ops->tx(r0, f, 40, &info) == -ENETDOWN);
    CHECK(r0->ops->start(r0) == 0);
    kheap_host_largest = 1;
    CHECK(r0->ops->tx(r0, f, 40, &info) == -ENOMEM);
    kheap_host_largest = 0;
    CHECK(r0->ops->tx(r0, f, 40, &info) == 0);
    tx_before = n_tx;
    r0->ops->stop(r0); /* a queued frame fails too */
    r0->ops->stop(r0); /* and twice is safe */
    CHECK(n_tx == tx_before + 1);
    CHECK(wsim_poll(&e0, POLLIN, &wq) == 0);
    CHECK(r0->ops->start(r0) == 0);
}

/** A medium whose waiting table is full fails the frame back at once. */
static void row_medium_full(void)
{
    static uint8_t d[MEDIUM_RECORD_MAX];
    struct medium m;
    struct wlan_tx_info info;
    uint8_t f[40];
    unsigned i, tx_before = n_tx;

    medium_init(&m, 0, 0, mac0, -50, 100);
    for (i = 0; i < MEDIUM_WAITING; i++) {
        m.waiting[i].used = 1;
    }
    data_frame(f, mac1, mac0, 40);
    tx_info(&info, 50);
    CHECK(r0->ops->tx(r0, f, 40, &info) == 0);
    CHECK(wsim_read(&e0, (char *)d, sizeof(d)) == 80);
    CHECK(medium_from_radio(&m, d, 80, (unsigned long)now) == MEDIUM_TO_RADIO);
    CHECK(medium_check(d, MEDIUM_HEADER) == MEDIUM_STATUS);
    CHECK(wsim_write(&e0, (const char *)d, MEDIUM_HEADER) == MEDIUM_HEADER);
    CHECK(n_tx == tx_before + 1 && txs[tx_before].status.cookie == 50);
}

/** Closing a radio made by an open fails what it sent and removes it. */
static void row_close(void)
{
    struct wlan_tx_info info;
    uint8_t f[40];
    unsigned tx_before = n_tx, unreg = unregistered;

    data_frame(f, mac0, mac1, 40);
    tx_info(&info, 60);
    CHECK(r1->ops->tx(r1, f, 40, &info) == 0);
    wsim_close(&e1);
    CHECK(e1.priv == NULL && unregistered == unreg + 1);
    CHECK(n_tx == tx_before + 1 && txs[tx_before].status.cookie == 60);
    CHECK(wsim_radios[1] == NULL);
    wsim_close(&e1); /* a description with no radio */
    CHECK(wsim_open(&e1) == 0 && radio_of(&e1)->mac[5] == 1);
    wsim_close(&e1);
    wsim_close(&e0);
    CHECK(wsim_radios[0] == NULL && unregistered == unreg + 3);
}

/** Radios made by rach2.wsim stay when their descriptors close. */
static void row_boot(void)
{
    static fd_entry_t e[5];
    static struct open_file o[5];
    struct wlan_tx_info info;
    struct wlan_channel ch6 = {2437, 6, 0, 20};
    uint8_t f[40];
    unsigned i, reg, unreg = unregistered, before_logs = logs;

    for (i = 0; i < 5; i++) {
        e[i].ofd = &o[i];
    }
    cmdline_wsim = "9";
    CHECK(wsim_attach(&station) == 0 && logs == before_logs + 1);
    cmdline_wsim = "2";
    reg = registered;
    CHECK(wsim_attach(&station) == 2 && registered == reg + 2);
    CHECK(wsim_radios[0] && wsim_radios[0]->boot && !wsim_radios[0]->held);
    r0 = &wsim_radios[0]->radio;
    CHECK(r0->ops->start(r0) == 0 && r0->ops->config(r0, &ch6, 0) == 0);
    data_frame(f, mac1, mac0, 40);
    tx_info(&info, 70);
    CHECK(r0->ops->tx(r0, f, 40, &info) == -ENOLINK);
    CHECK(wsim_open(&e[0]) == 0 && radio_of(&e[0]) == r0 && registered == reg + 2);
    CHECK(r0->ops->tx(r0, f, 40, &info) == 0);
    wsim_close(&e[0]);
    CHECK(unregistered == unreg && wsim_radios[0] == (struct wsim *)r0->priv);
    CHECK(r0->ops->tx(r0, f, 40, &info) == -ENOLINK);
    CHECK(wsim_open(&e[0]) == 0 && radio_of(&e[0]) == r0);
    CHECK(wsim_open(&e[1]) == 0 && radio_of(&e[1])->mac[5] == 1);
    register_result = -ENOMEM;
    CHECK(wsim_open(&e[2]) == -ENOMEM && wsim_radios[2] == NULL);
    register_result = 0;
    CHECK(wsim_open(&e[2]) == 0 && radio_of(&e[2])->mac[5] == 2);
    CHECK(wsim_open(&e[3]) == 0 && radio_of(&e[3])->mac[5] == 3);
    CHECK(wsim_open(&e[4]) == -ENOSPC);
    for (i = 0; i < 4; i++) {
        wsim_close(&e[i]);
    }
    CHECK(unregistered == unreg + 2 && wsim_radios[0] && wsim_radios[1]);
    CHECK(!wsim_radios[2] && !wsim_radios[3]);
    for (i = 0; i < 2; i++) {
        kfree(wsim_radios[i]);
        wsim_radios[i] = NULL;
    }
}

int main(void)
{
    row_open();
    row_start();
    row_unicast();
    row_broadcast();
    row_unheard();
    row_rates();
    row_filters();
    row_timeout();
    row_refusals();
    row_status_refusals();
    row_queue();
    row_medium_full();
    row_close();
    row_boot();
    if (failures) {
        printf("wsim_hosttest: %u failures\n", failures);
        return 1;
    }
    printf("wsim_hosttest: ok\n");
    return 0;
}
