/*
 * ath9k_mac_hosttest.c: the ath9k driver's DMA, interrupts, key cache and
 * recovery, mac.c, recv.c, xmit.c, key.c, intr.c and recover.c, against
 * the chip model of ath9k_model.h.
 *
 * host-deps: rach2/sys/network/driver/ath9k/io.c
 * host-deps: rach2/sys/network/driver/ath9k/chips.c
 * host-deps: rach2/sys/network/driver/ath9k/mac.c
 * host-deps: rach2/sys/network/driver/ath9k/recv.c
 * host-deps: rach2/sys/network/driver/ath9k/xmit.c
 * host-deps: rach2/sys/network/driver/ath9k/key.c
 * host-deps: rach2/sys/network/driver/ath9k/intr.c
 * host-deps: rach2/sys/network/driver/ath9k/recover.c
 * host-deps: rach2/sys/network/driver/ath9k/ampdu.c
 * host-deps: tests/host/ath9k_model.c rach2/sys/lib/kstring.c
 * host-deps: rach2/sys/lib/kctypes.c
 *
 * The test stands in for the parts it does not link: hw.c's reset gives
 * a channel with no noise floor the quiet noise level, resets the model
 * warm and sets up mac.c's queues, as ath9k_hw_reset ends; the station's
 * receive and transmit status are recorded; the worker's queue is a list
 * the test runs; the clock is a counter. The rows push frames through both
 * receive FIFOs and the transmit queues and their status ring, check each
 * descriptor's words against ath9k's, check a frame's and an ACK's signal
 * against the noise level, install keys and read the cache's registers,
 * raise each interrupt cause, and make the chip hang each way recover.c
 * looks for, then check that a reset brought back the queues, the filter,
 * the receive buffers, the interrupts and the keys. It exits with the
 * count of failed checks, at most 255.
 * host-cflags: -DTICK_HZ=1000
 */
#include "ath9k_model.h"
#include "irq.h"
#include "lib/kerrno.h"
#include "lib/kstring.h"

extern int printf(const char *format, ...);

/** memcmp's answer, zero when the `n` bytes match. */
static int k_memcmp(const void *a, const void *b, unsigned n)
{
    const unsigned char *x = a, *y = b;
    unsigned i;

    for (i = 0; i < n; i++)
        if (x[i] != y[i])
            return x[i] - y[i];
    return 0;
}

static unsigned fails;

#define CHECK(c)                                                            \
    do {                                                                    \
        if (!(c)) {                                                         \
            printf("ath9k_mac_hosttest: line %d: %s\n", (int)__LINE__, #c); \
            fails++;                                                        \
        }                                                                   \
    } while (0)

/* ------------------------------------------------------------------ */
/* DMA memory: pages whose bus addresses the model's dma hook maps.     */
/* ------------------------------------------------------------------ */

#define PAGES    300
#define BUS_BASE 0x10000000u

static unsigned char pages[PAGES][ATH9K_DMA_PAGE] __attribute__((aligned(4096)));
static unsigned pages_used;
static int fail_alloc_at = -1; /* the allocation that fails, or -1 */
static unsigned pages_freed;

void *ath9k_dma_alloc(struct ath9k *ah, uint32_t *bus)
{
    (void)ah;
    if ((int)pages_used == fail_alloc_at || pages_used == PAGES)
        return 0;
    k_memset(pages[pages_used], 0, ATH9K_DMA_PAGE);
    *bus = BUS_BASE + pages_used * ATH9K_DMA_PAGE;
    return pages[pages_used++];
}

void ath9k_dma_free(struct ath9k *ah, void *page)
{
    (void)ah;
    if (page)
        pages_freed++;
}

static void *dma_map(struct ath9k_model *m, uint32_t bus, unsigned len)
{
    (void)m;
    if (bus < BUS_BASE || bus + len > BUS_BASE + pages_used * ATH9K_DMA_PAGE)
        return 0;
    return &pages[0][0] + (bus - BUS_BASE);
}

/* ------------------------------------------------------------------ */
/* The kernel and the other parts, as the driver sees them.             */
/* ------------------------------------------------------------------ */

static uint64_t now_ms = 1000;
static unsigned logs;

uint64_t tick_count(void)
{
    return now_ms;
}

void klog(int level, const char *subsystem, const char *format, ...)
{
    (void)level;
    (void)subsystem;
    (void)format;
    logs++;
}

uint32_t kirq_save(void)
{
    return 0;
}

void kirq_restore(uint32_t flags)
{
    (void)flags;
}

static int msi_ready, msi_refused;
static isr_handler_t msi_handler;
static int vector_freed;

int isr_msi_ready(void)
{
    return msi_ready;
}

int isr_alloc_vector_aligned(int align)
{
    (void)align;
    return 0x60;
}

uint8_t pci_find_capability(uint8_t bus, uint8_t device, uint8_t function, uint8_t id)
{
    (void)bus;
    (void)device;
    (void)function;
    (void)id;
    return 0;
}

void isr_free_vector(int vector)
{
    (void)vector;
    vector_freed++;
    msi_handler = 0;
}

void register_isr_handler(uint8_t vector, isr_handler_t handler, const char *name)
{
    (void)vector;
    (void)name;
    msi_handler = handler;
}

int pci_msi_enable(const pci_device_t *dev, uint8_t vector)
{
    (void)dev;
    (void)vector;
    return msi_refused ? -1 : 0;
}

static unsigned msi_disables;

/* The card's command word: memory space and bus master on, as the probe
 * leaves them, and Interrupt Disable as the driver sets it. */
static uint16_t pci_command;
#define INTX_OFF() ((pci_command & PCI_CMD_INTERRUPT_DIS) != 0)

uint16_t pci_read_config16(uint8_t bus, uint8_t device, uint8_t function, uint8_t offset)
{
    (void)bus;
    (void)device;
    (void)function;
    return offset == PCI_COMMAND ? pci_command : 0;
}

void pci_write_config16(uint8_t bus, uint8_t device, uint8_t function, uint8_t offset,
                        uint16_t value)
{
    (void)bus;
    (void)device;
    (void)function;
    if (offset == PCI_COMMAND)
        pci_command = value;
}
static int nomsi; /* rach2.ath9k_nomsi on the boot line */

/** The boot line, which holds rach2.ath9k_nomsi alone, when nomsi says. */
int kcmdline_has(const char *flag)
{
    static const char want[] = "rach2.ath9k_nomsi";
    unsigned i;

    if (!nomsi)
        return 0;
    for (i = 0; want[i] && flag[i] == want[i]; i++) {
    }
    return want[i] == 0 && flag[i] == 0;
}

void pci_msi_disable(const pci_device_t *dev)
{
    (void)dev;
    msi_disables++;
}

/* The worker: the queued work, which the test runs. As work.c does, it
 * refuses a work queued already or whose callback runs. */
#define MAX_WORK 4
static struct wlan_work *queued[MAX_WORK];
static struct wlan_work *work_running;

int wlan_queue_work(struct wlan_work *work, uint64_t due)
{
    unsigned i;

    if (work == work_running)
        return -EBUSY;
    work->due = due;
    for (i = 0; i < MAX_WORK; i++)
        if (queued[i] == work)
            return -EBUSY;
    for (i = 0; i < MAX_WORK; i++)
        if (!queued[i]) {
            queued[i] = work;
            return 0;
        }
    return -EBUSY;
}

void wlan_cancel_work(struct wlan_work *work)
{
    unsigned i;

    for (i = 0; i < MAX_WORK; i++)
        if (queued[i] == work)
            queued[i] = 0;
}

/** Run the queued work that is due by now_ms; returns how many ran. */
static unsigned run_work(void)
{
    unsigned i, n = 0;

    for (i = 0; i < MAX_WORK; i++) {
        struct wlan_work *w = queued[i];

        if (w && w->due <= now_ms) {
            queued[i] = 0;
            work_running = w;
            w->run(w);
            work_running = 0;
            n++;
        }
    }
    return n;
}

/* The station's side: frames and statuses handed up. */
#define MAX_RX 16
static uint8_t rx_frame[MAX_RX][2048];
static unsigned rx_len[MAX_RX];
static struct wlan_rx_status rx_st[MAX_RX];
static struct wlan_rx_status rx_last; /* the last frame's, past MAX_RX too */
static unsigned rx_count;
static int rx_refuse, rx_full;

int wlan_radio_rx_full(struct wlan_radio *radio)
{
    (void)radio;
    return rx_full;
}

int wlan_radio_rx(struct wlan_radio *radio, const uint8_t *frame, unsigned len,
                  const struct wlan_rx_status *status)
{
    (void)radio;
    if (rx_refuse)
        return -ENOBUFS;
    if (rx_count < MAX_RX) {
        k_memcpy(rx_frame[rx_count], frame, len);
        rx_len[rx_count] = len;
        rx_st[rx_count] = *status;
    }
    rx_last = *status;
    rx_count++;
    return 0;
}

#define MAX_TXS 160
static struct wlan_tx_status txs[MAX_TXS];
static unsigned txs_count;

void wlan_radio_tx_status(struct wlan_radio *radio, const struct wlan_tx_status *status)
{
    (void)radio;
    if (txs_count < MAX_TXS)
        txs[txs_count] = *status;
    txs_count++;
}

void wlan_radio_ba_stop(struct wlan_radio *radio, unsigned tid)
{
    (void)radio;
    (void)tid;
}

static unsigned ant_calls;

void ath9k_ant_rx(struct ath9k *ah, const struct ar9003_rxs *rxs)
{
    (void)ah;
    (void)rxs;
    ant_calls++;
}

static unsigned cal_starts;

/** calib.c's start, which a reset that recovers calls on a started radio. */
void ath9k_cal_start(struct ath9k *ah)
{
    (void)ah;
    cal_starts++;
}

static int hw_reset_rc;
static unsigned hw_resets, cold_resets;

/** hw.c's reset as far as this test needs it: the noise level of a channel
 * with no noise floor yet, as ath9k_cal_set_channel gives it, a warm reset
 * of the MAC and baseband, which loses the PCU's registers and the FIFOs,
 * and the queues set up again, as ath9k_hw_reset ends. */
int ath9k_hw_reset(struct ath9k *ah, const struct wlan_channel *chan)
{
    (void)chan;
    hw_resets++;
    if (!ah->cal.noise_floor)
        ah->cal.noise = -95;
    if (hw_reset_rc)
        return hw_reset_rc;
    REG_WRITE(ah, AR_RTC_RC, AR_RTC_RC_MAC_WARM);
    REG_WRITE(ah, AR_RTC_RC, 0);
    ath9k_mac_init_queues(ah);
    return 0;
}

int ath9k_hw_set_reset(struct ath9k *ah, enum ath9k_reset_type type)
{
    if (type == ATH9K_RESET_COLD) {
        cold_resets++;
        REG_WRITE(ah, AR_RTC_RC, AR_RTC_RC_MAC_COLD);
        REG_WRITE(ah, AR_RTC_RC, 0);
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* The card.                                                            */
/* ------------------------------------------------------------------ */

static struct ath9k_model model;
static struct ath9k ah;
static uint32_t sync_cause; /* AR_INTR_SYNC_CAUSE, which clears by writing ones */

static int model_read(struct ath9k_model *m, uint32_t reg, uint32_t *value)
{
    (void)m;
    if (reg == AR_INTR_SYNC_CAUSE) {
        *value = sync_cause;
        return 1;
    }
    return 0;
}

static void model_write(struct ath9k_model *m, uint32_t reg, uint32_t value)
{
    (void)m;
    if (reg == AR_INTR_SYNC_CAUSE)
        sync_cause &= ~value;
}

static const uint8_t station[6] = {0x02, 0x11, 0x22, 0x33, 0x44, 0x55};
static const uint8_t ap[6] = {0x02, 0xaa, 0xbb, 0xcc, 0xdd, 0xee};

/** A started card, as pci.c starts one: reset on channel 6, address,
 * filter, receive, interrupts and the hang check. */
static void setup(int msi)
{
    static const uint16_t rates[12] = {10, 20, 55, 110, 60, 90, 120, 180, 240, 360, 480, 540};
    const struct ath9k_chip *chip = ath9k_chip_find(0x0036);
    unsigned i;

    k_memset(&ah, 0, sizeof(ah));
    k_memset(queued, 0, sizeof(queued));
    pages_used = 0;
    pages_freed = 0;
    fail_alloc_at = -1;
    rx_count = txs_count = 0;
    rx_refuse = rx_full = 0;
    hw_reset_rc = 0;
    hw_resets = cold_resets = 0;
    sync_cause = 0;
    ant_calls = 0;
    msi_ready = msi;
    msi_refused = 0;
    msi_handler = 0;
    msi_disables = 0;
    pci_command = PCI_CMD_MEMORY_SPACE | PCI_CMD_BUS_MASTER;

    ath9k_model_init(&model, chip, chip->revs[0].mac_rev);
    model.hooks.dma = dma_map;
    model.hooks.read = model_read;
    model.hooks.write = model_write;
    ath9k_model_attach(&model, &ah);
    ath9k_model_poke(&model, AR_RTC_RESET, AR_RTC_RESET_EN);
    ath9k_model_poke(&model, AR_RTC_FORCE_WAKE, AR_RTC_FORCE_WAKE_EN);

    ah.hw.chip = chip;
    for (i = 0; i < 12; i++)
        ah.radio.band.rates[i] = rates[i];
    ah.radio.band.n_rates = 12;
    k_memcpy(ah.radio.mac, station, 6);
    ah.phy.channel.frequency = 2437;
    ah.phy.channel.number = 6;

    CHECK(ath9k_rx_init(&ah) == 0);
    CHECK(ath9k_tx_init(&ah) == 0);
    CHECK(ath9k_intr_attach(&ah) == 0);
    CHECK(ath9k_hw_reset(&ah, &ah.phy.channel) == 0);
    ath9k_mac_set_address(&ah);
    ath9k_mac_set_filter(&ah, 0);
    CHECK(ath9k_rx_start(&ah) == 0);
    ath9k_mac_intr_enable(&ah);
    ah.bus.started = 1;
    ath9k_recover_start(&ah);
}

/** The interrupt as the machine delivers it: the message's handler, or
 * the tick on a card without messages. */
static void interrupt(void)
{
    if (msi_handler)
        msi_handler(0);
    else
        ath9k_tick(&ah);
}

/** Receive `len` bytes of `frame` and its FCS into FIFO `hp`, with a good
 * status at rate code `rate` and RSSI `rssi`, plus status11 bits `s11`. A
 * QoS data frame gets the two bytes the chip puts after its header. */
static int receive(int hp, const uint8_t *frame, unsigned len, unsigned rate, int rssi,
                   uint32_t s11)
{
    static uint8_t buf[4096];
    struct ar9003_rxs st;

    k_memset(&st, 0, sizeof(st));
    if ((frame[0] & 0xfc) == 0x88 && (frame[1] & 0x03) != 0x03) {
        k_memcpy(buf, frame, 26);
        buf[26] = buf[27] = 0xee;
        k_memcpy(buf + 28, frame + 26, len - 26);
        len += 2;
    } else {
        k_memcpy(buf, frame, len);
    }
    k_memset(buf + len, 0x5a, 4);
    st.status1 = SM(rate, AR_RxRate);
    st.status5 = SM((uint8_t)rssi, AR_RxRSSICombined);
    st.status11 = AR_RxFrameOK | s11;
    return ath9k_model_rx(&model, hp, buf, len + 4, &st);
}

/** A management frame of `len` bytes from the AP: a beacon's header and a
 * counting body. */
static unsigned beacon(uint8_t *f, unsigned len)
{
    unsigned i;

    k_memset(f, 0, len);
    f[0] = 0x80;
    k_memset(f + 4, 0xff, 6);
    k_memcpy(f + 10, ap, 6);
    k_memcpy(f + 16, ap, 6);
    for (i = 24; i < len; i++)
        f[i] = (uint8_t)i;
    return len;
}

/** A QoS data frame of `body` bytes from the AP to the station, with the
 * protected bit when `prot`. */
static unsigned qos_data(uint8_t *f, unsigned body, int prot)
{
    unsigned i;

    k_memset(f, 0, 26 + body);
    f[0] = 0x88;
    f[1] = (uint8_t)(0x02 | (prot ? 0x40 : 0));
    k_memcpy(f + 4, station, 6);
    k_memcpy(f + 10, ap, 6);
    k_memcpy(f + 16, ap, 6);
    for (i = 0; i < body; i++)
        f[26 + i] = (uint8_t)(i * 7);
    return 26 + body;
}

/* ------------------------------------------------------------------ */
/* Receive.                                                             */
/* ------------------------------------------------------------------ */

/** Associated, the AP's beacons set noise immunity's average, each
 * weighing a tenth after the first; another BSS's beacon, a frame that is
 * no beacon, an RSSI the chip could not read and a CRC error leave it. The
 * beacon is the `len` bytes of `f`; the counts are left as the first beacon
 * left them, for the rows after. */
static void rx_beacon_rssi(uint8_t *f, unsigned len)
{
    ah.mac.associated = 1;
    k_memcpy(ah.mac.bssid, ap, 6);
    CHECK(receive(0, f, len, 0x1b, 50, 0) == 0);
    interrupt();
    CHECK(ah.ani.beacon_rssi == 50 && ah.ani.beacon_lpf == 50 * 128);
    CHECK(receive(0, f, len, 0x1b, 30, 0) == 0);
    interrupt();
    CHECK(ah.ani.beacon_rssi == 48); /* (50 * 9 + 30) / 10 */
    f[16 + 5] ^= 1;
    CHECK(receive(0, f, len, 0x1b, 5, 0) == 0);
    f[16 + 5] ^= 1;
    f[0] = 0x50; /* a probe response */
    CHECK(receive(0, f, len, 0x1b, 5, 0) == 0);
    f[0] = 0x80;
    CHECK(receive(0, f, len, 0x1b, -128, 0) == 0);
    {
        static uint8_t buf[200];
        struct ar9003_rxs st;

        k_memset(&st, 0, sizeof(st));
        k_memcpy(buf, f, len);
        st.status5 = SM(5, AR_RxRSSICombined);
        st.status11 = AR_CRCErr;
        CHECK(ath9k_model_rx(&model, 0, buf, len + 4, &st) == 0);
    }
    interrupt();
    CHECK(ah.ani.beacon_rssi == 48 && ah.stats.rx_frames == 6);
    CHECK(receive(0, f, len, 0x1b, -3, 0) == 0); /* below the floor: 0 */
    interrupt();
    CHECK(ah.ani.beacon_rssi == 43); /* 48 * 9 / 10 */
    ah.mac.associated = 0;
    ah.ani.beacon_rssi = ah.ani.beacon_lpf = ah.ani.beacon_heard = 0;
    rx_count = 1;
    ah.stats.rx_frames = 1;
    ah.stats.rx_crc = 0;
}

/** A QoS data frame, which the chip pads after its 26-byte header, on the
 * high-priority FIFO at MCS 5 with a short guard interval. */
static void rx_qos(void)
{
    uint8_t f[600], chip[700];
    unsigned len;

    len = qos_data(f, 300, 0);
    k_memcpy(chip, f, 26);
    chip[26] = chip[27] = 0xee;
    k_memcpy(chip + 28, f + 26, 300);
    {
        static uint8_t buf[700];
        struct ar9003_rxs st;

        k_memset(&st, 0, sizeof(st));
        k_memcpy(buf, chip, 328);
        k_memset(buf + 328, 0, 4);
        st.status1 = SM(0x85, AR_RxRate);
        st.status4 = AR_GI;
        st.status11 = AR_RxFrameOK;
        CHECK(ath9k_model_rx(&model, 1, buf, 332, &st) == 0);
    }
    interrupt();
    CHECK(rx_count == 2 && rx_len[1] == len && k_memcmp(rx_frame[1], f, len) == 0);
    CHECK(rx_st[1].rate == 5 && rx_st[1].rate_flags == (WLAN_RATE_MCS | WLAN_RATE_SHORT_GI));
    CHECK(model.rx[0].count == ATH9K_RX_HP_DEPTH);
}

/** Retune to `frequency`, whose noise level is `noise`, as pci.c's config
 * does: interrupts and DMA stopped, the received frames not yet read kept,
 * a reset onto the channel, receive started again and interrupts on. */
static void retune(unsigned frequency, int noise)
{
    struct wlan_channel ch = ah.phy.channel;

    ch.frequency = (uint16_t)frequency;
    ath9k_mac_intr_disable(&ah);
    (void)ath9k_mac_abort_tx(&ah);
    CHECK(ath9k_rx_stop(&ah, 1) == 0);
    ath9k_tx_drain(&ah);
    CHECK(ath9k_hw_reset(&ah, &ch) == 0);
    ah.phy.channel = ch;
    ah.cal.noise = (int16_t)noise;
    ath9k_mac_set_filter(&ah, ah.rx.filters);
    CHECK(ath9k_rx_start(&ah) == 0);
    ath9k_mac_intr_enable(&ah);
}

/**
 * Two frames held in the FIFO for want of room, then `reset`, which is a
 * retune or a recovery, before the next read. Unless the reset drops them,
 * the tick delivers each once, with the channel it came on, after which the
 * chip holds every buffer again; a dropped one is counted in rx_dropped.
 */
static void rx_held_across(const uint8_t *f, unsigned len, int retuned, enum ath9k_reset_reason why)
{
    unsigned n = rx_count, dropped = ah.stats.rx_dropped, freq = ah.phy.channel.frequency;
    int noise = ah.cal.noise, keep = retuned || why != ATH9K_RESET_FATAL;

    rx_full = 1;
    CHECK(receive(0, f, len, 0x1b, 10, 0) == 0);
    CHECK(receive(0, f, len, 0x1b, 12, 0) == 0);
    ath9k_tick(&ah);
    CHECK(rx_count == n && ah.rx.held[1]);
    if (retuned)
        retune(freq == 2437 ? 2412 : 2437, -90);
    else
        CHECK(ath9k_reset(&ah, why) == 0);
    CHECK(model.rx[1].count == ATH9K_RX_LP_DEPTH - (keep ? 2 : 0));
    CHECK(ah.rx.fifo[1].kept == (keep ? 2u : 0u) && ah.rx.held[1] == keep);
    rx_full = 0;
    ath9k_tick(&ah);
    ath9k_tick(&ah);
    CHECK(rx_count == n + (keep ? 2 : 0) && !ah.rx.held[1] && ah.rx.fifo[1].kept == 0);
    CHECK(ah.stats.rx_dropped == dropped + (keep ? 0 : 2));
    CHECK(model.rx[1].count == ATH9K_RX_LP_DEPTH);
    if (keep)
        CHECK(rx_last.frequency == freq && rx_last.signal == noise + 12);

    /* The next frame came on the channel the radio is on now. */
    CHECK(receive(0, f, len, 0x1b, 10, 0) == 0);
    interrupt();
    CHECK(rx_count == n + (keep ? 3 : 1) && rx_last.frequency == ah.phy.channel.frequency);
    CHECK(rx_last.signal == ah.cal.noise + 10);
}

/** The frames receive drops or counts: errors, a frame over two buffers,
 * one the station refuses, and the ring's wrap. */
static void rx_drops(void)
{
    uint8_t f[100];
    unsigned len, n, msi;

    /* A CRC error and a phy error are dropped and counted; their buffers
     * go back. A phy restart after a good delimiter is not an error. */
    len = beacon(f, 60);
    CHECK(receive(0, f, len, 0x0b, 10, 0) == 0);
    model.rx[1].count = model.rx[1].count; /* the FIFO the next writes fill */
    {
        static uint8_t buf[100];
        struct ar9003_rxs st;

        k_memset(&st, 0, sizeof(st));
        k_memcpy(buf, f, len);
        st.status11 = AR_CRCErr;
        CHECK(ath9k_model_rx(&model, 0, buf, len + 4, &st) == 0);
        st.status11 = AR_PHYErr | SM(5, AR_PHYErrCode);
        CHECK(ath9k_model_rx(&model, 0, buf, len + 4, &st) == 0);
        st.status11 = AR_PHYErr | SM(39, AR_PHYErrCode) | AR_PostDelimCRCErr;
        CHECK(ath9k_model_rx(&model, 0, buf, len + 4, &st) == 0);
    }
    interrupt();
    CHECK(rx_count == 4);
    CHECK(rx_st[2].rate == 4); /* 6 Mbit/s */
    CHECK(ah.stats.rx_crc == 1 && ah.stats.rx_phy == 1);
    CHECK(model.rx[1].count == ATH9K_RX_LP_DEPTH);

    /* A frame spanning two buffers is dropped whole; the next is not. */
    {
        static uint8_t buf[100];
        struct ar9003_rxs st;

        k_memset(&st, 0, sizeof(st));
        k_memcpy(buf, f, len);
        st.status2 = AR_RxMore;
        st.status11 = AR_RxFrameOK;
        CHECK(ath9k_model_rx(&model, 0, buf, len + 4, &st) == 0);
        st.status2 = 0;
        CHECK(ath9k_model_rx(&model, 0, buf, len + 4, &st) == 0);
        CHECK(ath9k_model_rx(&model, 0, buf, len + 4, &st) == 0);
    }
    n = rx_count;
    interrupt();
    CHECK(rx_count == n + 1);

    /* A frame the station cannot take is counted, and its buffer returns. */
    rx_refuse = 1;
    CHECK(receive(0, f, len, 0x1b, 10, 0) == 0);
    interrupt();
    CHECK(ah.stats.rx_dropped >= 1 && model.rx[1].count == ATH9K_RX_LP_DEPTH);
    rx_refuse = 0;

    /* A full queue leaves the frame in the FIFO, for the tick to read, with
     * messages or without, when no cause is raised for it. */
    for (msi = 0; msi < 2; msi++) {
        unsigned was = ah.mac.msi, dropped = ah.stats.rx_dropped;

        n = rx_count;
        ah.mac.msi = (uint8_t)msi;
        rx_full = 1;
        CHECK(receive(0, f, len, 0x1b, 10, 0) == 0);
        ath9k_tick(&ah);
        CHECK(rx_count == n && ah.rx.held[1]);
        CHECK(model.rx[1].count == ATH9K_RX_LP_DEPTH - 1);
        rx_full = 0;
        ath9k_tick(&ah);
        CHECK(rx_count == n + 1 && !ah.rx.held[1]);
        CHECK(model.rx[1].count == ATH9K_RX_LP_DEPTH);
        CHECK(ah.stats.rx_dropped == dropped);
        ah.mac.msi = (uint8_t)was;
    }

    /* No room is kept for a key message: a message 3 behind a data frame
     * waits with it in the FIFO and comes whole, in its turn. */
    n = rx_count;
    CHECK(n + 2 <= MAX_RX);
    len = beacon(f, 60);
    rx_full = 1;
    CHECK(receive(0, f, len, 0x1b, 10, 0) == 0);
    k_memset(f, 0, sizeof(f));
    f[0] = 0x08;
    f[1] = 0x02;
    k_memcpy(f + 24, "\xaa\xaa\x03\x00\x00\x00\x88\x8e\x02\x03", 10);
    CHECK(receive(0, f, 60, 0x1b, 10, 0) == 0);
    ath9k_tick(&ah);
    CHECK(rx_count == n && ah.rx.held[1]);
    rx_full = 0;
    ath9k_tick(&ah);
    CHECK(rx_count == n + 2 && !ah.rx.held[1]);
    CHECK(rx_len[n + 1] == 60 && k_memcmp(rx_frame[n + 1], f, 60) == 0);

    /* The ring wraps: three FIFOs' worth through the high-priority one. */
    n = rx_count;
    for (len = 0; len < 3 * ATH9K_RX_HP_DEPTH; len++) {
        beacon(f, 40);
        CHECK(receive(1, f, 40, 0x1b, 10, 0) == 0);
        interrupt();
    }
    CHECK(rx_count == n + 3 * ATH9K_RX_HP_DEPTH);
    CHECK(model.overruns == 0 && model.dma_faults == 0);
}

/** Receive on a card with messages: a beacon on the low-priority FIFO at
 * 1 Mbit/s, then the rows of rx_beacon_rssi, rx_qos and rx_drops, and
 * frames held across resets. */
static void test_rx(void)
{
    uint8_t f[100];
    unsigned len;

    setup(1);
    CHECK(msi_handler != 0 && ah.mac.msi && ah.bus.vector == 0x60 && !INTX_OFF());
    CHECK(model.rx[0].count == ATH9K_RX_HP_DEPTH && model.rx[1].count == ATH9K_RX_LP_DEPTH);
    CHECK(ath9k_model_peek(&model, AR_DATABUF_SIZE) == ATH9K_RX_BUF_LEN - 48);
    CHECK(!(ath9k_model_peek(&model, AR_DIAG_SW) & (AR_DIAG_RX_DIS | AR_DIAG_RX_ABORT)));
    CHECK(model.rxe);

    len = beacon(f, 100);
    CHECK(receive(0, f, len, 0x1b, 40, 0) == 0);
    CHECK(model.rx[1].count == ATH9K_RX_LP_DEPTH - 1);
    interrupt();
    CHECK(rx_count == 1 && rx_len[0] == len && k_memcmp(rx_frame[0], f, len) == 0);
    CHECK(rx_st[0].rate == 0 && rx_st[0].rate_flags == 0);
    CHECK(rx_st[0].signal == -95 + 40 && rx_st[0].frequency == 2437 && rx_st[0].hw_key == -1);
    CHECK(rx_st[0].flags == 0);
    CHECK(model.rx[1].count == ATH9K_RX_LP_DEPTH);
    CHECK(ant_calls == 1 && ah.stats.rx_frames == 1);
    CHECK(ath9k_model_peek(&model, AR_IER) == AR_IER_ENABLE);
    CHECK(ah.ani.beacon_rssi == 0); /* not associated: not the AP's */

    rx_beacon_rssi(f, len);
    rx_qos();
    rx_drops();

    /* Held frames outlast a retune, there and back, and a recovery; the
     * reset of a fatal error drops them, counted. */
    rx_held_across(f, len, 1, ATH9K_RESET_REASONS);
    rx_held_across(f, len, 1, ATH9K_RESET_REASONS);
    rx_held_across(f, len, 0, ATH9K_RESET_BB_HANG);
    rx_held_across(f, len, 0, ATH9K_RESET_FATAL);
}

/* ------------------------------------------------------------------ */
/* Transmit.                                                            */
/* ------------------------------------------------------------------ */

/** The descriptor the model holds at bus address `bus`. */
static const struct ar9003_txc *desc_at(uint32_t bus)
{
    return (const struct ar9003_txc *)dma_map(&model, bus, sizeof(struct ar9003_txc));
}

static struct wlan_tx_info tx_info(unsigned ac, uint64_t cookie)
{
    struct wlan_tx_info info;
    unsigned i;

    k_memset(&info, 0, sizeof(info));
    info.cookie = cookie;
    info.ac = (uint8_t)ac;
    info.tid = 0;
    info.hw_key = -1;
    info.frequency = 2437;
    for (i = 0; i < WLAN_TX_RATE_SERIES; i++)
        info.rates[i].index = -1;
    info.rates[0].index = 11; /* 54 Mbit/s */
    info.rates[0].count = 2;
    info.rates[1].index = 4; /* 6 Mbit/s */
    info.rates[1].count = 3;
    info.rates[2].index = 0; /* 1 Mbit/s */
    info.rates[2].count = 4;
    return info;
}

/** Check that `d`, the descriptor of the `len`-byte QoS data frame `f` on
 * BE, holds what ath9k writes: its queue, length, checksum, tries, rates,
 * durations and chains, and the frame padded after its header. */
static void check_be_desc(const struct ar9003_txc *d, const uint8_t *f, unsigned len)
{
    const uint8_t *buf;
    uint32_t sum;

    CHECK(MS(d->info, AR_DescId) == ATH9K_VENDOR_ID && (d->info & AR_TxRxDesc) &&
          (d->info & AR_CtrlStat));
    CHECK(MS(d->info, AR_TxQcuNum) == 1 && (d->info & 0xff) == 0x18);
    CHECK(MS(d->ctl3, AR_BufLen) == len + 2);
    sum = d->info + d->link + d->data0 + d->ctl3 + d->data1 + d->ctl5 + d->data2 + d->ctl7 +
          d->data3 + d->ctl9;
    CHECK((d->ctl10 & AR_TxPtrChkSum) == (((sum & 0xffff) + (sum >> 16)) & 0xffff));
    CHECK((d->ctl11 & AR_FrameLen) == len + 4 && !(d->ctl11 & AR_DestIdxValid));
    CHECK(MS(d->ctl13, AR_XmitDataTries0) == 2 && MS(d->ctl13, AR_XmitDataTries1) == 3);
    CHECK(MS(d->ctl13, AR_XmitDataTries2) == 4 && MS(d->ctl13, AR_XmitDataTries3) == 0);
    CHECK(MS(d->ctl14, AR_XmitRate0) == 0x0c && MS(d->ctl14, AR_XmitRate1) == 0x0b);
    CHECK(MS(d->ctl14, AR_XmitRate2) == 0x1b);
    CHECK(MS(d->ctl15, AR_PacketDur0) > 16 &&
          MS(d->ctl15, AR_PacketDur1) > MS(d->ctl15, AR_PacketDur0));
    CHECK(MS(d->ctl18, AR_ChainSel0) == ah.hw.chip->chainmask);
    CHECK(d->ctl19 == AR_Not_Sounding && MS(d->ctl17, AR_EncrType) == 0);
    buf = dma_map(&model, d->data0, len + 2);
    CHECK(buf && k_memcmp(buf, f, 26) == 0 && k_memcmp(buf + 28, f + 26, 200) == 0);
}

/** A QoS data frame on BE, which goes out padded after its header, and its
 * status; then a frame that ran out of tries. */
static void tx_be(void)
{
    uint8_t f[400];
    struct wlan_tx_info info;
    struct ar9003_txs st;
    const struct ar9003_txc *d;
    uint32_t bus;
    unsigned len, q;

    len = qos_data(f, 200, 0);
    info = tx_info(WLAN_AC_BE, 77);
    CHECK(ath9k_tx(&ah, f, len, &info) == 0);
    q = ah.tx.q[WLAN_AC_BE].qnum;
    CHECK(q == 1 && model.tx[1].count == 1);
    bus = model.tx[1].bus[model.tx[1].head];
    d = desc_at(bus);
    CHECK(d != 0);
    if (d)
        check_be_desc(d, f, len);

    /* Its status: acknowledged at the second series after one failure
     * there, so the first used both its tries. */
    k_memset(&st, 0, sizeof(st));
    st.status3 = AR_FrmXmitOK | SM(1, AR_DataFailCnt);
    st.status8 = SM(1, AR_FinalTxIdx);
    CHECK(ath9k_model_tx_complete(&model, 1, &st) == bus);
    interrupt();
    CHECK(txs_count == 1 && txs[0].cookie == 77 && txs[0].flags == WLAN_TX_ACKED);
    CHECK(txs[0].rates[0].count == 2 && txs[0].rates[1].count == 2 && txs[0].rates[2].index == -1);
    CHECK(ah.tx.q[WLAN_AC_BE].count == 0 && ah.stats.tx_frames == 1 && ah.stats.tx_retries == 3);

    /* A frame that ran out of tries is reported failed. */
    info = tx_info(WLAN_AC_BE, 78);
    CHECK(ath9k_tx(&ah, f, len, &info) == 0);
    st.status3 = AR_ExcessiveRetries | SM(3, AR_DataFailCnt);
    st.status8 = SM(2, AR_FinalTxIdx);
    CHECK(ath9k_model_tx_complete(&model, 1, &st) != 0);
    interrupt();
    CHECK(txs_count == 2 && txs[1].cookie == 78 && txs[1].flags == 0 && ah.stats.tx_failed == 1);
    CHECK(txs[1].rates[2].count == 4 && txs[1].rates[3].index == -1);
}

/** The FIFO and the ring: ten frames on VO, a queue's ring and one more,
 * and the frames transmit refuses. */
static void tx_fifo(void)
{
    uint8_t f[100];
    struct wlan_tx_info info;
    struct ar9003_txs st;
    unsigned len, i;

    /* Ten frames on VO: eight go to the FIFO, the rest as statuses free
     * room, in order. */
    len = beacon(f, 64);
    for (i = 0; i < 10; i++) {
        info = tx_info(WLAN_AC_VO, 100 + i);
        CHECK(ath9k_tx(&ah, f, len, &info) == 0);
    }
    CHECK(model.tx[3].count == ATH9K_TX_FIFO_DEPTH && ah.tx.q[WLAN_AC_VO].count == 10);
    k_memset(&st, 0, sizeof(st));
    st.status3 = AR_FrmXmitOK;
    CHECK(ath9k_model_tx_complete(&model, 3, &st) != 0);
    CHECK(ath9k_model_tx_complete(&model, 3, &st) != 0);
    interrupt();
    CHECK(model.tx[3].count == ATH9K_TX_FIFO_DEPTH && ah.tx.q[WLAN_AC_VO].count == 8);
    while (model.tx[3].count)
        CHECK(ath9k_model_tx_complete(&model, 3, &st) != 0);
    interrupt();
    CHECK(ah.tx.q[WLAN_AC_VO].count == 0);
    for (i = 0; i < 10; i++)
        CHECK(txs[2 + i].cookie == 100 + i);
    CHECK(model.overruns == 0);

    /* The ring around: a queue's 64 frames and one more, refused. */
    for (i = 0; i < ATH9K_TX_RING; i++) {
        info = tx_info(WLAN_AC_BK, 1000 + i);
        CHECK(ath9k_tx(&ah, f, len, &info) == 0);
    }
    info = tx_info(WLAN_AC_BK, 2000);
    CHECK(ath9k_tx(&ah, f, len, &info) == -ENOBUFS);
    for (i = 0; i < ATH9K_TX_RING; i++) {
        CHECK(ath9k_model_tx_complete(&model, 0, &st) != 0);
        interrupt();
    }
    CHECK(ah.tx.q[WLAN_AC_BK].count == 0 && txs_count == 12 + ATH9K_TX_RING);
    CHECK(txs[11 + ATH9K_TX_RING].cookie == 1000 + ATH9K_TX_RING - 1);

    /* Bad frames accept nothing. */
    info = tx_info(7, 1);
    CHECK(ath9k_tx(&ah, f, len, &info) == -EINVAL);
    info = tx_info(WLAN_AC_BE, 1);
    CHECK(ath9k_tx(&ah, f, 4, &info) == -EINVAL);
    info.flags = WLAN_TX_HW_KEY;
    info.hw_key = 9;
    CHECK(ath9k_tx(&ah, f, len, &info) == -EINVAL);
    CHECK(model.dma_faults == 0);
}

/** An underrun or a descriptor the chip refused is no ACK, even beside
 * AR_FrmXmitOK, and a status without AR_FrmXmitOK is none either, unless
 * the frame asked for no ACK or went to a group address. */
static void tx_no_ack(void)
{
    static const uint32_t s3[5] = {AR_FrmXmitOK | AR_TxDataUnderrun,
                                   AR_FrmXmitOK | AR_TxDelimUnderrun, AR_FrmXmitOK | AR_DescCfgErr,
                                   0, 0};
    unsigned base = txs_count, failed = ah.stats.tx_failed;
    uint8_t f[300];
    struct wlan_tx_info info;
    struct ar9003_txs st;
    unsigned len, i;

    for (i = 0; i < 6; i++) {
        len = i < 5 ? qos_data(f, 200, 0) : beacon(f, 64);
        info = tx_info(WLAN_AC_BE, 300 + i);
        if (i == 4)
            info.flags = WLAN_TX_NO_ACK;
        CHECK(ath9k_tx(&ah, f, len, &info) == 0);
        k_memset(&st, 0, sizeof(st));
        st.status3 = i < 5 ? s3[i] : 0;
        CHECK(ath9k_model_tx_complete(&model, 1, &st) != 0);
        interrupt();
    }
    CHECK(txs_count == base + 6 && ah.stats.tx_failed == failed + 4);
    for (i = 0; i < 6 && base + i < MAX_TXS; i++)
        CHECK(txs[base + i].cookie == 300 + i &&
              txs[base + i].flags == (i < 4 ? 0 : WLAN_TX_ACKED));
}

/** Transmit on a card with messages: the queues' set-up, then the rows of
 * tx_be, tx_fifo and tx_no_ack. */
static void test_tx(void)
{
    setup(1);
    /* Queues: VO is QCU 3 and BK QCU 0, each a DCU of its own number. */
    CHECK(ah.tx.q[WLAN_AC_VO].qnum == 3 && ah.tx.q[WLAN_AC_BK].qnum == 0);
    CHECK(ath9k_model_peek(&model, AR_DQCUMASK(2)) == 4);
    CHECK(MS(ath9k_model_peek(&model, AR_DLCL_IFS(1)), AR_D_LCL_IFS_AIFS) == 3);
    CHECK(MS(ath9k_model_peek(&model, AR_DLCL_IFS(3)), AR_D_LCL_IFS_CWMIN) == 3);
    CHECK(ath9k_model_peek(&model, AR_Q_STATUS_RING_START) == ah.tx.status_bus);
    CHECK(model.txs_end == ah.tx.status_bus + ATH9K_TXS_RING * sizeof(struct ar9003_txs));

    tx_be();
    tx_fifo();
    tx_no_ack();
}

/* ------------------------------------------------------------------ */
/* The signal.                                                          */
/* ------------------------------------------------------------------ */

/** Receive a beacon at RSSI 40 and send a QoS data frame on BE whose ACK
 * comes back at RSSI 30, and check that the frame's signal is `noise` + 40
 * and the ACK's `noise` + 30, in dBm. */
static void check_signal(int noise)
{
    uint8_t f[400];
    struct wlan_tx_info info;
    struct ar9003_txs st;
    unsigned len;

    rx_count = txs_count = 0;
    len = beacon(f, 100);
    CHECK(receive(0, f, len, 0x1b, 40, 0) == 0);
    interrupt();
    CHECK(rx_count == 1 && rx_st[0].signal == noise + 40);

    len = qos_data(f, 200, 0);
    info = tx_info(WLAN_AC_BE, 90);
    CHECK(ath9k_tx(&ah, f, len, &info) == 0);
    k_memset(&st, 0, sizeof(st));
    st.status3 = AR_FrmXmitOK;
    st.status7 = SM(30, AR_TxRSSICombined);
    CHECK(ath9k_model_tx_complete(&model, ah.tx.q[WLAN_AC_BE].qnum, &st) != 0);
    interrupt();
    CHECK(txs_count == 1 && txs[0].flags == WLAN_TX_ACKED && txs[0].ack_signal == noise + 30);
}

/** The signal of a frame and of an ACK is the RSSI over the noise level, as
 * ath9k_process_rssi gives it, not over the raw noise floor: the first
 * noise floor result of a quiet channel, -117 dBm, leaves both readings as
 * they were, and a floor of -95 dBm, 9 dB beyond the threshold of 6 dB
 * over the nominal -110 dBm, raises them by 9 dB. The calibration's fields
 * are set as ath9k_nf_read leaves them, which ath9k_phy_hosttest checks. */
static void test_signal(void)
{
    setup(1);
    CHECK(ah.cal.noise_floor == 0 && ah.cal.noise == -95);
    check_signal(-95);
    ah.cal.noise_floor = -117;
    ah.cal.noise = -95;
    check_signal(-95);
    ah.cal.noise_floor = -95;
    ah.cal.noise = -95 + 9;
    check_signal(-95 + 9);
}

/* ------------------------------------------------------------------ */
/* Keys.                                                                */
/* ------------------------------------------------------------------ */

static struct wlan_key make_key(uint32_t cipher, unsigned index, unsigned flags,
                                const uint8_t *peer)
{
    struct wlan_key k;
    unsigned i;

    k_memset(&k, 0, sizeof(k));
    k.cipher = cipher;
    k.index = index;
    k.flags = flags;
    k.len = cipher == WLAN_CIPHER_SUITE_TKIP ? 32 : 16;
    for (i = 0; i < k.len; i++)
        k.data[i] = (uint8_t)(0x10 + i + index);
    k_memcpy(k.peer, peer, 6);
    k.hw_slot = -1;
    return k;
}

static uint32_t le32_at(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

/** Whether entry `e` holds `k`'s first 128 bits, type and address bits. */
static int entry_is(unsigned e, const struct wlan_key *k, uint32_t type, uint32_t mac1)
{
    return ath9k_model_peek(&model, AR_KEYTABLE_KEY0(e)) == le32_at(k->data) &&
           ath9k_model_peek(&model, AR_KEYTABLE_KEY2(e)) == le32_at(k->data + 6) &&
           ath9k_model_peek(&model, AR_KEYTABLE_KEY4(e)) == le32_at(k->data + 12) &&
           ath9k_model_peek(&model, AR_KEYTABLE_TYPE(e)) == type &&
           ath9k_model_peek(&model, AR_KEYTABLE_MAC1(e)) == mac1;
}

static const uint8_t bcast[6] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};

/** A CCMP frame the cache decrypted keeps its IV and MIC; a TKIP one loses
 * its ICV. `slot` holds the CCMP key and `s2` the TKIP one. */
static void keys_rx(int slot, int s2)
{
    uint8_t f[200];
    unsigned len;

    len = qos_data(f, 120, 1);
    CHECK(receive(0, f, len, 0x0c, 30, AR_RxKeyIdxValid | SM(slot, AR_KeyIdx)) == 0);
    CHECK(receive(0, f, len, 0x0c, 30, AR_RxKeyIdxValid | SM(s2, AR_KeyIdx)) == 0);
    CHECK(receive(0, f, len, 0x0c, 30, AR_DecryptCRCErr & 0) == 0);
    ath9k_tick(&ah);
    CHECK(rx_count == 3);
    CHECK((rx_st[0].flags & WLAN_RX_DECRYPTED) && rx_st[0].hw_key == slot && rx_len[0] == len);
    CHECK((rx_st[1].flags & WLAN_RX_DECRYPTED) && rx_st[1].hw_key == s2 && rx_len[1] == len - 4);
    CHECK(!(rx_st[2].flags & WLAN_RX_DECRYPTED)); /* keyid 0's group entry is empty */
    {
        static uint8_t buf[300];
        struct ar9003_rxs st;

        k_memset(&st, 0, sizeof(st));
        k_memcpy(buf, f, 26);
        buf[26] = buf[27] = 0;
        k_memcpy(buf + 28, f + 26, 120);
        st.status11 = AR_DecryptCRCErr;
        CHECK(ath9k_model_rx(&model, 0, buf, len + 2 + 4, &st) == 0);
    }
    ath9k_tick(&ah);
    CHECK(rx_count == 4 && !(rx_st[3].flags & WLAN_RX_DECRYPTED) && ah.stats.rx_decrypt == 1);
}

/** A frame sent with the CCMP key at `slot` names its entry and leaves the
 * MIC to the chip. */
static void keys_tx(int slot)
{
    uint8_t f[200];
    unsigned len;
    struct wlan_tx_info info;

    info = tx_info(WLAN_AC_BE, 5);
    info.flags = WLAN_TX_HW_KEY;
    info.hw_key = slot;
    len = qos_data(f, 100, 1) + 8 + 8;
    CHECK(ath9k_tx(&ah, f, len, &info) == 0);
    {
        const struct ar9003_txc *d = desc_at(model.tx[1].bus[model.tx[1].head]);

        CHECK(d && (d->ctl11 & AR_DestIdxValid) && MS(d->ctl12, AR_DestIdx) == (unsigned)slot);
        CHECK(d && MS(d->ctl17, AR_EncrType) == 2 && (d->ctl11 & AR_FrameLen) == len + 4);
        CHECK(d && MS(d->ctl3, AR_BufLen) == len - 8 + 2);
    }
}

/** Clearing the TKIP key `tkip` at `s2` clears its registers and its MIC
 * entry, twice harmlessly, and the key can be installed again. */
static void keys_clear(const struct wlan_key *tkip, int s2)
{
    ath9k_key_clear(&ah, s2, ap, 0);
    CHECK(ath9k_model_peek(&model, AR_KEYTABLE_TYPE(s2)) == AR_KEYTABLE_TYPE_CLR);
    CHECK(ath9k_model_peek(&model, AR_KEYTABLE_KEY0(s2 + 64)) == 0);
    CHECK(ah.keys.entry[s2].used == ATH9K_KEY_FREE &&
          ah.keys.entry[s2 + 64].used == ATH9K_KEY_FREE);
    ath9k_key_clear(&ah, s2, ap, 0); /* twice is harmless */
    CHECK(ath9k_key_set(&ah, ap, tkip, &s2) == 0);
}

/** A group key installed at the index of the one it replaces takes its
 * entry, and the station's removal of the old key after the install
 * leaves the new one, in the cache and in the copy a reset writes
 * back; removing the new one clears the entry. */
static void keys_group_replace(void)
{
    struct wlan_key g1 = make_key(WLAN_CIPHER_SUITE_CCMP, 2, 0, bcast);
    struct wlan_key g2 = g1;
    int a = -1, b = -1;

    g2.data[0] ^= 0xff;
    CHECK(ath9k_key_set(&ah, bcast, &g1, &a) == 0 && a == 2);
    CHECK(ath9k_key_set(&ah, bcast, &g2, &b) == 0 && b == 2);
    ath9k_key_clear(&ah, a, bcast, &g1);
    CHECK(ah.keys.entry[2].used == ATH9K_KEY_USED);
    CHECK(entry_is(2, &g2, AR_KEYTABLE_TYPE_CCM, AR_KEYTABLE_VALID));
    ath9k_key_restore(&ah);
    CHECK(entry_is(2, &g2, AR_KEYTABLE_TYPE_CCM, AR_KEYTABLE_VALID));
    ath9k_key_clear(&ah, b, bcast, &g2);
    CHECK(ah.keys.entry[2].used == ATH9K_KEY_FREE);
    CHECK(ath9k_model_peek(&model, AR_KEYTABLE_TYPE(2)) == AR_KEYTABLE_TYPE_CLR);
}

/** A pairwise key cleared and then replaced, in the station's order, takes
 * another entry, and the next takes the first again: a frame that names the
 * cleared entry is known by it (radio.h). */
static void keys_slot_moves(const struct wlan_key *ccmp, const uint8_t *ap)
{
    int a, b, c;

    CHECK(ath9k_key_set(&ah, ap, ccmp, &a) == 0);
    ath9k_key_clear(&ah, a, ap, ccmp);
    CHECK(ath9k_key_set(&ah, ap, ccmp, &b) == 0 && b != a);
    ath9k_key_clear(&ah, b, ap, ccmp);
    CHECK(ath9k_key_set(&ah, ap, ccmp, &c) == 0 && c != b);
    ath9k_key_clear(&ah, c, ap, ccmp);
}

/** Send `n` frames on `ac` from number `first` with the key at `slot`. */
static void keys_send_on(unsigned ac, int slot, unsigned first, unsigned n)
{
    uint8_t f[200];
    struct wlan_tx_info info;
    unsigned len = qos_data(f, 100, 1) + 8 + 8, i;

    for (i = 0; i < n; i++) {
        info = tx_info(ac, first + i);
        info.flags = WLAN_TX_HW_KEY;
        info.hw_key = slot;
        CHECK(ath9k_tx(&ah, f, len, &info) == 0);
    }
}

/** Send `n` frames on BE from number `first` with the key at `slot`. */
static void keys_send(int slot, unsigned first, unsigned n)
{
    keys_send_on(WLAN_AC_BE, slot, first, n);
}

/** Whether entry `slot` is dying: the address gone from the chip's search,
 * its key and type in place. */
static int key_dying(int slot, const struct wlan_key *key)
{
    return ah.keys.entry[slot].used == ATH9K_KEY_DYING &&
           ath9k_model_peek(&model, AR_KEYTABLE_MAC0(slot)) == 0 &&
           ath9k_model_peek(&model, AR_KEYTABLE_MAC1(slot)) == AR_KEYTABLE_VALID &&
           ath9k_model_peek(&model, AR_KEYTABLE_TYPE(slot)) == AR_KEYTABLE_TYPE_CCM &&
           ath9k_model_peek(&model, AR_KEYTABLE_KEY0(slot)) ==
               (uint32_t)(key->data[0] | key->data[1] << 8 | key->data[2] << 16 |
                          (uint32_t)key->data[3] << 24);
}

/** Whether entry `slot` is free and clear in the chip. */
static int key_gone(int slot)
{
    return ah.keys.entry[slot].used == ATH9K_KEY_FREE &&
           ath9k_model_peek(&model, AR_KEYTABLE_TYPE(slot)) == AR_KEYTABLE_TYPE_CLR &&
           ath9k_model_peek(&model, AR_KEYTABLE_KEY0(slot)) == 0;
}

/** A pairwise key replaced behind an upload: the frame sent by keys_tx and
 * eight more on BE, eight in the FIFO and one behind. The removal spends no
 * time and resets nothing; the entry leaves the address search with its key
 * in place, is not given to another key, and a second removal changes
 * nothing. The ninth frame, not pushed, is dropped as its key's last, which
 * clears the entry: a frame the old key sends now could cross the AP's change
 * of key. */
static void keys_pairwise_replace(const struct wlan_key *ccmp, int slot, uint32_t mac1)
{
    struct wlan_key next = *ccmp, third = *ccmp;
    struct ar9003_txs st;
    unsigned i, base = txs_count;
    uint64_t usec;
    uint32_t stale, failed;
    int s4 = -1, s5 = -1;

    keys_send(slot, 10, 8);
    CHECK(ah.tx.q[WLAN_AC_BE].count == 9 && model.tx[1].count == ATH9K_TX_FIFO_DEPTH);
    k_memset(&st, 0, sizeof(st));
    st.status3 = AR_FrmXmitOK;
    for (i = 0; i < 8; i++)
        CHECK(ath9k_model_tx_complete(&model, 1, &st) != 0);
    next.data[0] ^= 0xff;
    CHECK(ath9k_key_set(&ah, ap, &next, &s4) == 0 && s4 != slot);
    usec = model.usec;
    ath9k_key_clear(&ah, slot, ap, ccmp);
    CHECK(model.usec == usec);
    CHECK(ah.stats.resets[ATH9K_RESET_TX_HANG] == 0 && txs_count == base);
    CHECK(key_dying(slot, ccmp) && ah.keys.dying == 1);
    ath9k_key_clear(&ah, slot, ap, ccmp);
    ath9k_key_clear(&ah, slot, ap, 0);
    CHECK(key_dying(slot, ccmp) && ah.keys.dying == 1);
    third.data[0] ^= 0x0f;
    CHECK(ath9k_key_set(&ah, ap, &third, &s5) == 0 && s5 != slot && s5 != s4);
    ath9k_key_clear(&ah, s5, ap, &third);
    CHECK(key_gone(s5));

    /* The eight in the FIFO finish; the ninth, not pushed, is not sent. */
    stale = ah.stats.tx_stale;
    failed = ah.stats.tx_failed;
    CHECK(ath9k_tx_service(&ah) == 8);
    CHECK(txs_count == base + 9 && ah.tx.q[WLAN_AC_BE].count == 0);
    for (i = 0; i < 8 && base + i < MAX_TXS; i++)
        CHECK(txs[base + i].flags == WLAN_TX_ACKED);
    CHECK(base + 8 < MAX_TXS && txs[base + 8].flags == WLAN_TX_FILTERED);
    CHECK(ah.stats.tx_stale == stale + 1 && ah.stats.tx_failed == failed);
    CHECK(model.tx[1].count == 0);
    CHECK(key_gone(slot) && ah.keys.dying == 0);
    CHECK(ah.stats.resets[ATH9K_RESET_TX_HANG] == 0);
    CHECK(entry_is((unsigned)s4, &next, AR_KEYTABLE_TYPE_CCM, mac1));
    ath9k_key_clear(&ah, s4, ap, &next);
    CHECK(key_gone(s4));
}

/** A voice frame behind the FIFO stays through its key's removal and goes out
 * under it: message 4 of the renewal may be one, which the AP can read under
 * no other key. */
static void keys_voice_stays(const struct wlan_key *ccmp)
{
    struct wlan_key k = *ccmp;
    const struct ath9k_txq *q = &ah.tx.q[WLAN_AC_VO];
    struct ar9003_txs st;
    unsigned i, base = txs_count;
    int slot = -1;

    k.data[0] ^= 0x55;
    CHECK(ath9k_key_set(&ah, ap, &k, &slot) == 0);
    keys_send_on(WLAN_AC_VO, slot, 30, 9);
    CHECK(q->count == 9 && model.tx[q->qnum].count == ATH9K_TX_FIFO_DEPTH);
    ath9k_key_clear(&ah, slot, ap, &k);
    CHECK(key_dying(slot, &k));
    k_memset(&st, 0, sizeof(st));
    st.status3 = AR_FrmXmitOK;
    for (i = 0; i < 8; i++)
        CHECK(ath9k_model_tx_complete(&model, q->qnum, &st) != 0);
    CHECK(ath9k_tx_service(&ah) == 8 && q->count == 1 && model.tx[q->qnum].count == 1);
    CHECK(ath9k_model_tx_complete(&model, q->qnum, &st) != 0);
    CHECK(ath9k_tx_service(&ah) == 1 && q->count == 0);
    CHECK(txs_count == base + 9 && key_gone(slot));
    for (i = 0; i < 9 && base + i < MAX_TXS; i++)
        CHECK(txs[base + i].flags == WLAN_TX_ACKED);
}

/** A reap does not move what the next pairwise key skips: entry `b`, which
 * the current key left a moment ago, stays closed to the key after it when
 * the older entry `a`, dying until now, is freed in between by an interrupt,
 * so that a frame in the receive ring that names `b` cannot pass for one the
 * new key opened. */
static void keys_reap_keeps_freed(const struct wlan_key *ccmp)
{
    struct wlan_key k = *ccmp;
    struct ar9003_txs st;
    int a = -1, b = -1, c = -1;

    CHECK(ath9k_key_set(&ah, ap, &k, &a) == 0);
    keys_send(a, 50, 1);
    k.data[0] ^= 0xff;
    CHECK(ath9k_key_set(&ah, ap, &k, &b) == 0 && b != a);
    k.data[0] ^= 0xff;
    ath9k_key_clear(&ah, a, ap, &k);
    CHECK(key_dying(a, ccmp));
    k.data[0] ^= 0xff;
    ath9k_key_clear(&ah, b, ap, &k);
    CHECK(key_gone(b));
    k_memset(&st, 0, sizeof(st));
    st.status3 = AR_FrmXmitOK;
    CHECK(ath9k_model_tx_complete(&model, 1, &st) != 0);
    CHECK(ath9k_tx_service(&ah) == 1);
    CHECK(key_gone(a));
    k.data[0] ^= 0x0f;
    CHECK(ath9k_key_set(&ah, ap, &k, &c) == 0 && c != b);
    ath9k_key_clear(&ah, c, ap, &k);
    CHECK(key_gone(c));
}

/** A chip that stops completing keeps no entry: the hang check finds the
 * frame that names a dying entry, and its reset fails the frame and frees
 * the entry. So does a restart after a stop, whose drain left no frame. */
static void keys_dying_hang(const struct wlan_key *ccmp)
{
    unsigned base = txs_count;
    int slot = -1;

    CHECK(ath9k_key_set(&ah, ap, ccmp, &slot) == 0);
    keys_send(slot, 30, 1);
    ath9k_key_clear(&ah, slot, ap, ccmp);
    CHECK(key_dying(slot, ccmp));
    (void)ath9k_hang_check(&ah);
    CHECK(ath9k_hang_check(&ah) == ATH9K_RESET_TX_HANG);
    CHECK(txs_count == base + 1 && base < MAX_TXS && txs[base].flags == 0);
    CHECK(key_gone(slot) && ah.keys.dying == 0);

    CHECK(ath9k_key_set(&ah, ap, ccmp, &slot) == 0);
    keys_send(slot, 31, 1);
    ath9k_key_clear(&ah, slot, ap, ccmp);
    CHECK(key_dying(slot, ccmp));
    ath9k_tx_drain(&ah);
    ah.bus.started = 0;
    ath9k_key_clear(&ah, slot, ap, ccmp);
    CHECK(ah.keys.entry[slot].used == ATH9K_KEY_DYING);
    ah.bus.started = 1;
    ath9k_key_restore(&ah);
    CHECK(key_gone(slot) && ah.keys.dying == 0);
    CHECK(ath9k_reset(&ah, ATH9K_RESET_CAL) == 0);
}

/** A group key at the index of a dying entry cannot wait: the frame the
 * chip has finished is served, and the new key has the entry, which leaves
 * dying. */
static void keys_group_dying(const struct wlan_key *gtk)
{
    struct wlan_key next = *gtk;
    struct ar9003_txs st;
    int slot = -1, again = -1;
    unsigned base = txs_count;

    next.data[0] ^= 0xff;
    CHECK(ath9k_key_set(&ah, bcast, gtk, &slot) == 0);
    keys_send(slot, 40, 1);
    ath9k_key_clear(&ah, slot, bcast, gtk);
    CHECK(ah.keys.entry[slot].used == ATH9K_KEY_DYING && ah.keys.dying == 1);
    k_memset(&st, 0, sizeof(st));
    st.status3 = AR_FrmXmitOK;
    CHECK(ath9k_model_tx_complete(&model, 1, &st) != 0);
    CHECK(ath9k_key_set(&ah, bcast, &next, &again) == 0 && again == slot);
    CHECK(txs_count == base + 1 && ah.keys.dying == 0);
    CHECK(ah.keys.entry[slot].used == ATH9K_KEY_USED);
    CHECK(ah.stats.resets[ATH9K_RESET_TX_HANG] == 1);
    ath9k_key_clear(&ah, slot, bcast, &next);
}

/** The key cache on a card without messages: CCMP, TKIP and group keys
 * installed, then the rows of keys_rx, keys_tx, keys_clear,
 * keys_group_replace, keys_pairwise_replace, keys_dying_hang and
 * keys_group_dying. */
static void test_keys(void)
{
    struct wlan_key ccmp = make_key(WLAN_CIPHER_SUITE_CCMP, 0, WLAN_KEY_PAIRWISE, ap);
    struct wlan_key tkip = make_key(WLAN_CIPHER_SUITE_TKIP, 0, WLAN_KEY_PAIRWISE, ap);
    struct wlan_key gtk = make_key(WLAN_CIPHER_SUITE_TKIP, 1, WLAN_KEY_DEFAULT, bcast);
    struct wlan_key igtk = make_key(WLAN_CIPHER_SUITE_AES_CMAC, 4, 0, bcast);
    uint32_t mac1 = (((uint32_t)ap[4] | (uint32_t)ap[5] << 8) >> 1) | AR_KEYTABLE_VALID;
    int slot = -1, s2 = -1, s3 = -1, first, last;

    setup(0);
    CHECK(msi_handler == 0 && !ah.mac.msi && ah.bus.vector == 0);
    CHECK(pci_command == (PCI_CMD_MEMORY_SPACE | PCI_CMD_BUS_MASTER | PCI_CMD_INTERRUPT_DIS));
    CHECK(ath9k_model_peek(&model, AR_PCU_MISC) & AR_PCU_MIC_NEW_LOC_ENA);
    CHECK(ath9k_model_peek(&model, AR_STA_ID1) & AR_STA_ID1_CRPT_MIC_ENABLE);
    CHECK(MS(ath9k_model_peek(&model, AR_AES_MUTE_MASK1), AR_AES_MUTE_MASK1_FC_MGMT) == 0xc7ff);

    CHECK(ath9k_key_set(&ah, ap, &ccmp, &slot) == 0 && slot == AR_RSVD_KEYTABLE_ENTRIES);
    CHECK(entry_is((unsigned)slot, &ccmp, AR_KEYTABLE_TYPE_CCM, mac1));
    CHECK(ath9k_model_peek(&model, AR_KEYTABLE_MAC0(slot)) ==
          ((le32_at(ap) >> 1) | ((uint32_t)(ap[4] & 1) << 31)));

    /* TKIP: its first 48 bits go in inverted, then the MIC entry 64 up,
     * then the right bits. */
    ath9k_model_log_clear(&model);
    CHECK(ath9k_key_set(&ah, ap, &tkip, &s2) == 0 && s2 == AR_RSVD_KEYTABLE_ENTRIES + 1);
    first = ath9k_model_find(&model, 0, ATH9K_MODEL_WRITE, AR_KEYTABLE_KEY0(s2));
    last = ath9k_model_find(&model, (unsigned)first + 1, ATH9K_MODEL_WRITE, AR_KEYTABLE_KEY0(s2));
    CHECK(first >= 0 && last > first);
    if (first >= 0 && last > first) {
        CHECK(model.log[first].value == ~le32_at(tkip.data));
        CHECK(ath9k_model_find(&model, (unsigned)first, ATH9K_MODEL_WRITE,
                               AR_KEYTABLE_KEY0(s2 + 64)) < last);
    }
    CHECK(entry_is((unsigned)s2, &tkip, AR_KEYTABLE_TYPE_TKIP, mac1));
    CHECK(ath9k_model_peek(&model, AR_KEYTABLE_KEY0(s2 + 64)) == le32_at(tkip.data + 24));
    CHECK(ath9k_model_peek(&model, AR_KEYTABLE_KEY4(s2 + 64)) == le32_at(tkip.data + 20));
    CHECK(ath9k_model_peek(&model, AR_KEYTABLE_TYPE(s2 + 64)) == AR_KEYTABLE_TYPE_CLR);

    /* A group key at its index, with no address; an integrity key and a
     * cipher the cache lacks stay in software. */
    CHECK(ath9k_key_set(&ah, bcast, &gtk, &s3) == 0 && s3 == 1);
    CHECK(entry_is(1, &gtk, AR_KEYTABLE_TYPE_TKIP, AR_KEYTABLE_VALID));
    CHECK(ath9k_model_peek(&model, AR_KEYTABLE_KEY0(65)) == le32_at(gtk.data + 24));
    CHECK(ath9k_key_set(&ah, bcast, &igtk, &slot) == -EOPNOTSUPP);
    slot = AR_RSVD_KEYTABLE_ENTRIES;

    keys_rx(slot, s2);
    keys_tx(slot);
    keys_clear(&tkip, s2);
    keys_group_replace();
    keys_pairwise_replace(&ccmp, slot, mac1);
    keys_voice_stays(&ccmp);
    keys_reap_keeps_freed(&ccmp);
    keys_dying_hang(&ccmp);
    keys_group_dying(&gtk);
    keys_slot_moves(&ccmp, ap);
}

/* ------------------------------------------------------------------ */
/* Hangs and resets.                                                    */
/* ------------------------------------------------------------------ */

/** Whether the card is back after a reset: queues, status ring, filter,
 * receive buffers, interrupts, and the keys `ccmp` at `slot`. */
static void check_restored(const struct wlan_key *ccmp, int slot, uint32_t mac1)
{
    CHECK(model.rx[0].count == ATH9K_RX_HP_DEPTH && model.rx[1].count == ATH9K_RX_LP_DEPTH);
    CHECK(model.rxe && ah.rx.running && ah.tx.running && !ah.recover.resetting);
    CHECK(ath9k_model_peek(&model, AR_Q_STATUS_RING_START) == ah.tx.status_bus);
    CHECK(ath9k_model_peek(&model, AR_IER) == AR_IER_ENABLE);
    CHECK(ath9k_model_peek(&model, AR_IMR) == ah.mac.imask);
    CHECK(ath9k_model_peek(&model, AR_RX_FILTER) == ah.rx.hw_filter);
    CHECK(ath9k_model_peek(&model, AR_STA_ID0) == le32_at(station));
    CHECK(ath9k_model_peek(&model, AR_BSS_ID0) == le32_at(ah.mac.bssid));
    CHECK(entry_is((unsigned)slot, ccmp, AR_KEYTABLE_TYPE_CCM, mac1));
}

/** A queue whose frame gets no status across two checks is hung: the
 * frame is reported failed, and the reset brings everything back,
 * the key the warm reset cleared among it. Then the MAC's hang signature
 * on queue 1, the AR9003's DMA debug words. */
static void recover_tx_hang(const struct wlan_key *ccmp, int slot, uint32_t mac1)
{
    struct wlan_tx_info info;
    uint8_t f[100];
    unsigned len, i;

    len = beacon(f, 64);
    info = tx_info(WLAN_AC_BE, 9);
    CHECK(ath9k_tx(&ah, f, len, &info) == 0);
    for (i = 0; i < 4; i++) {
        CHECK(receive(0, f, len, 0x1b, 20, 0) == 0);
        interrupt();
    }
    CHECK(ath9k_hang_check(&ah) == -1);
    CHECK(ath9k_hang_check(&ah) == ATH9K_RESET_TX_HANG);
    CHECK(txs_count == 1 && txs[0].cookie == 9 && txs[0].flags == 0);
    /* No reset of its own before ath9k_hw_reset, which resets cold while
     * DMA runs, as ath_reset_internal leaves it. */
    CHECK(hw_resets == 2 && cold_resets == 0 && ah.stats.resets[ATH9K_RESET_TX_HANG] == 1);
    CHECK(ah.tx.q[WLAN_AC_BE].count == 0 && model.tx[1].count == 0);
    check_restored(ccmp, slot, mac1);
    CHECK(ath9k_model_peek(&model, AR_BEACON_PERIOD) == 102400);
    CHECK(ath9k_model_peek(&model, AR_PHY_WATCHDOG_CTL_2) & AR_PHY_WATCHDOG_IRQ_ENABLE);

    ath9k_model_poke(&model, AR_DMADBG_4, 0x6u << 5);
    ath9k_model_poke(&model, AR_DMADBG_6, 0x1);
    CHECK(ath9k_hang_check(&ah) == ATH9K_RESET_MAC_HANG);
    CHECK(ah.stats.resets[ATH9K_RESET_MAC_HANG] == 1 && hw_resets == 3);
    check_restored(ccmp, slot, mac1);
    ath9k_model_poke(&model, AR_DMADBG_4, 0x6u << 5);
    ath9k_model_poke(&model, AR_DMADBG_6, 0x2); /* not complete: no hang */
    CHECK(ath9k_hang_check(&ah) == -1);
    ath9k_model_poke(&model, AR_DMADBG_4, 0);
}

/** Receive: a window with a frame a second passes; one without, while
 * associated, is a stall; one while not associated is not judged. */
static void recover_rx_stall(const struct wlan_key *ccmp, int slot, uint32_t mac1)
{
    uint8_t f[100];
    unsigned len, i;

    len = beacon(f, 64);
    now_ms += 4000;
    for (i = 0; i < 4; i++) {
        CHECK(receive(0, f, len, 0x1b, 20, 0) == 0);
        interrupt();
    }
    CHECK(ath9k_hang_check(&ah) == -1);
    now_ms += 4000;
    CHECK(receive(0, f, len, 0x1b, 20, 0) == 0);
    interrupt();
    CHECK(ath9k_hang_check(&ah) == ATH9K_RESET_RX_STALL);
    CHECK(cold_resets == 0);
    check_restored(ccmp, slot, mac1);
    ah.mac.associated = 0;
    ath9k_mac_set_bssid(&ah, (const uint8_t *)"\0\0\0\0\0\0", 0);
    now_ms += 4000;
    CHECK(ath9k_hang_check(&ah) == -1); /* not associated: not judged */
    CHECK(ah.rx.hw_filter & ATH9K_RX_FILTER_BEACON);
}

/** A fatal PCIe error asks the worker for a reset and leaves the chip's
 * interrupts off until it runs; the baseband watchdog's hang is reset; beacon
 * misses and receive overruns are counted. */
static void recover_fatal(const struct wlan_key *ccmp, int slot, uint32_t mac1)
{
    unsigned before;

    before = hw_resets;
    sync_cause = AR_INTR_SYNC_HOST1_FATAL;
    interrupt();
    CHECK(ath9k_model_peek(&model, AR_IER) == AR_IER_DISABLE && ah.recover.pending);
    CHECK(sync_cause == 0);
    ath9k_tick(&ah);
    CHECK(run_work() >= 1 && hw_resets == before + 1 && ah.stats.resets[ATH9K_RESET_FATAL] == 1);
    check_restored(ccmp, slot, 0x0 | mac1);

    /* The baseband watchdog: a known harmless state is cleared, a hang is
     * reset. */
    before = hw_resets;
    ath9k_model_poke(&model, AR_ISR_S2, AR_ISR_S2_BB_WATCHDOG);
    ath9k_model_poke(&model, AR_PHY_WATCHDOG_STATUS, 0x1300000a);
    ath9k_model_raise(&model, AR_ISR_BCNMISC);
    interrupt();
    CHECK(!ah.recover.pending && ath9k_model_peek(&model, AR_IER) == AR_IER_ENABLE);
    ath9k_model_poke(&model, AR_PHY_WATCHDOG_STATUS, 0x0400000a);
    ath9k_model_raise(&model, AR_ISR_BCNMISC);
    interrupt();
    CHECK(ah.recover.pending && ah.recover.bb_status == 0x0400000a);
    run_work();
    CHECK(hw_resets == before + 1 && ah.stats.resets[ATH9K_RESET_BB_WATCHDOG] == 1);
    check_restored(ccmp, slot, mac1);

    /* Beacon misses and receive overruns are counted. */
    ath9k_model_raise(&model, AR_ISR_BMISS | AR_ISR_RXORN);
    interrupt();
    CHECK(ah.stats.beacon_miss == 1 && ah.stats.rx_overrun == 1);
}

/** A message sent before a disable that arrives after it, in the reset
 * or retune that followed, touches nothing; the cause waits for the
 * enable, and a served message enables the interrupts again. */
static void recover_late_message(void)
{
    unsigned before;

    ath9k_mac_intr_disable(&ah);
    ath9k_model_raise(&model, AR_ISR_BMISS);
    ath9k_model_log_clear(&model);
    model.log_reads = 1;
    interrupt();
    CHECK(model.log_count == 0 && ah.stats.beacon_miss == 1);
    model.log_reads = 0;
    ath9k_mac_intr_enable(&ah);
    before = ath9k_model_count(&model, ATH9K_MODEL_WRITE, AR_PCIE_MSI);
    interrupt();
    CHECK(ah.stats.beacon_miss == 2);
    /* A served message enables the interrupts again, the message engine's
     * enable with them, as ath9k's tasklet does. */
    CHECK(!msi_handler || ath9k_model_count(&model, ATH9K_MODEL_WRITE, AR_PCIE_MSI) > before);
    CHECK(ath9k_model_peek(&model, AR_IER) == AR_IER_ENABLE);
}

/** Messages that find no cause are counted, and EMPTY_MAX of them in a
 * row end the messages: the card is then served from the tick. */
static void recover_msi_storms(void)
{
    unsigned i, spurious = ah.stats.spurious;

    ath9k_tick(&ah);
    for (i = 0; i < 999; i++)
        interrupt();
    CHECK(ah.stats.spurious == spurious + 999 && ah.mac.msi && !INTX_OFF());
    ath9k_tick(&ah);
    ath9k_model_raise(&model, AR_ISR_BMISS);
    interrupt();
    CHECK(ah.stats.beacon_miss == 3 && ah.mac.empty == 0);
    for (i = 0; i < 1000; i++)
        interrupt();
    CHECK(!ah.mac.msi && msi_disables == 1 && INTX_OFF());
    ah.mac.msi = 1;
    pci_command &= (uint16_t)~PCI_CMD_INTERRUPT_DIS;
    ath9k_mac_intr_enable(&ah);

    /* BURST_MAX messages between two ticks, each with a cause, as a
     * cause that came back at once would send, end them too. */
    ath9k_tick(&ah);
    for (i = 0; i < 999; i++) {
        ath9k_model_raise(&model, AR_ISR_BMISS);
        interrupt();
    }
    CHECK(ah.mac.msi);
    interrupt();
    CHECK(!ah.mac.msi && msi_disables == 2 && INTX_OFF());
    CHECK(ath9k_model_peek(&model, AR_IER) == AR_IER_ENABLE);
    ah.mac.msi = 1;
    pci_command &= (uint16_t)~PCI_CMD_INTERRUPT_DIS;
    ath9k_mac_intr_enable(&ah);
    ath9k_tick(&ah);
}

/** A reset that fails leaves the chip off; the next check tries again;
 * receive DMA that does not stop is waited for; the filter is kept while
 * receive is stopped; and the checks stop with the radio. */
static void recover_failed(const struct wlan_key *ccmp, int slot, uint32_t mac1)
{
    unsigned before;

    hw_reset_rc = -EIO;
    CHECK(ath9k_reset(&ah, ATH9K_RESET_CAL) == -EIO && ah.recover.failures == 1);
    CHECK(ath9k_model_peek(&model, AR_IER) == AR_IER_DISABLE);
    hw_reset_rc = 0;
    before = cal_starts;
    CHECK(ath9k_hang_check(&ah) == ATH9K_RESET_CAL && ah.recover.failures == 0);
    CHECK(cal_starts == before + 1);
    check_restored(ccmp, slot, mac1);

    /* Receive DMA that does not stop is waited for, and the reset goes on. */
    model.stuck_rx_dma = 1;
    CHECK(ath9k_reset(&ah, ATH9K_RESET_BB_HANG) == 0);
    model.stuck_rx_dma = 0;
    check_restored(ccmp, slot, mac1);

    /* The filter is kept while receive is stopped (f0b2c30a2029): the chip
     * holds none, and the next start writes the one asked for meanwhile. */
    CHECK(ath9k_rx_stop(&ah, 1) == 0);
    CHECK(ath9k_model_peek(&model, AR_RX_FILTER) == 0);
    ath9k_model_log_clear(&model);
    ath9k_mac_set_filter(&ah, WLAN_FILTER_PROMISC);
    CHECK(ath9k_model_count(&model, ATH9K_MODEL_WRITE, AR_RX_FILTER) == 0);
    CHECK(ath9k_hw_reset(&ah, &ah.phy.channel) == 0 && ath9k_rx_start(&ah) == 0);
    CHECK(ath9k_model_peek(&model, AR_RX_FILTER) & ATH9K_RX_FILTER_PROM);

    /* The checks stop with the radio. */
    ath9k_recover_stop(&ah);
    CHECK(queued[0] == 0 && queued[1] == 0 && !ah.recover.running);
    CHECK(!INTX_OFF());
    ath9k_intr_detach(&ah);
    CHECK(vector_freed == 1 && !ah.mac.msi && INTX_OFF());
    CHECK(model.overruns == 0 && model.dma_faults == 0 && model.unmapped == 0);
}

/** Hangs and resets on a card with messages, associated and with a CCMP
 * key: the beacon timers and the queued hang check, then the rows of the
 * recover_ functions. */
static void test_recover(void)
{
    struct wlan_key ccmp = make_key(WLAN_CIPHER_SUITE_CCMP, 0, WLAN_KEY_PAIRWISE, ap);
    uint32_t mac1 = (((uint32_t)ap[4] | (uint32_t)ap[5] << 8) >> 1) | AR_KEYTABLE_VALID;
    int slot = -1;

    setup(1);
    CHECK(ath9k_key_set(&ah, ap, &ccmp, &slot) == 0);
    /* Associated, as pci.c's sta_state leaves the card, which this test
     * does not link. */
    ah.mac.associated = 1;
    ath9k_mac_set_bssid(&ah, ap, 1);
    model.usec = 0x123456789ull;
    ath9k_mac_set_beacon_timers(&ah, 100, 3);
    CHECK(ath9k_model_peek(&model, AR_NEXT_TBTT_TIMER) ==
          (uint32_t)((0x123456789ull + 2048) / 102400 * 102400 + 102400));
    CHECK(ath9k_model_peek(&model, AR_DTIM_PERIOD) == 3 * 102400);
    CHECK(ath9k_model_peek(&model, AR_BEACON_PERIOD) == 102400);
    CHECK(ath9k_model_peek(&model, AR_TIMER_MODE) & AR_TBTT_TIMER_EN);
    CHECK(MS(ath9k_model_peek(&model, AR_RSSI_THR), AR_RSSI_THR_BM_THR) == 10);
    CHECK(ah.rx.hw_filter & ATH9K_RX_FILTER_MYBEACON);
    CHECK(ath9k_model_peek(&model, AR_BSS_ID1) ==
          ((uint32_t)ap[4] | (uint32_t)ap[5] << 8) + (1u << AR_BSS_ID1_AID_S));

    /* The hang check is queued on the worker and finds nothing. */
    CHECK(queued[0] == &ah.recover.work[0].work);
    now_ms += ATH9K_HANG_MS;
    CHECK(run_work() == 1 && queued[0] == &ah.recover.work[1].work && hw_resets == 1);
    now_ms += ATH9K_HANG_MS;
    CHECK(run_work() == 1 && queued[0] == &ah.recover.work[0].work && hw_resets == 1);

    recover_tx_hang(&ccmp, slot, mac1);
    recover_rx_stall(&ccmp, slot, mac1);
    recover_fatal(&ccmp, slot, mac1);
    recover_late_message();
    if (msi_handler)
        recover_msi_storms();
    recover_failed(&ccmp, slot, mac1);
}

/* ------------------------------------------------------------------ */
/* Allocation, messages refused, and the tick.                          */
/* ------------------------------------------------------------------ */

static void test_attach(void)
{
    uint8_t f[100];
    unsigned len;

    setup(1);
    pages_used = 0;
    pages_freed = 0;
    fail_alloc_at = 20;
    CHECK(ath9k_rx_init(&ah) == -ENOMEM && pages_freed == 20);
    pages_used = 0;
    pages_freed = 0;
    fail_alloc_at = 2; /* the first buffer page after a descriptor page */
    CHECK(ath9k_tx_init(&ah) == -ENOMEM && pages_freed == 2 && ah.tx.status == 0);
    fail_alloc_at = -1;

    /* A probe that fails later gives back every page both took, once. */
    k_memset(&ah.rx, 0, sizeof(ah.rx));
    k_memset(&ah.tx, 0, sizeof(ah.tx));
    pages_used = 0;
    pages_freed = 0;
    CHECK(ath9k_rx_init(&ah) == 0 && ath9k_tx_init(&ah) == 0);
    len = pages_used;
    CHECK(len ==
          ATH9K_RX_HP_DEPTH + ATH9K_RX_LP_DEPTH + 1 +
              ATH9K_TX_QUEUES * ATH9K_TX_RING / (ATH9K_DMA_PAGE / sizeof(struct ar9003_txc)) +
              ATH9K_TX_QUEUES * ATH9K_TX_RING / (ATH9K_DMA_PAGE / ATH9K_TX_BUF_LEN));
    ath9k_tx_free(&ah);
    ath9k_rx_free(&ah);
    CHECK(pages_freed == len && ah.tx.status == 0 && ah.rx.fifo[1].buf[0] == 0);
    ath9k_tx_free(&ah);
    ath9k_rx_free(&ah);
    CHECK(pages_freed == len);

    /* A card whose messages cannot be enabled is served by the tick. */
    setup(1);
    msi_refused = 1;
    ath9k_intr_detach(&ah);
    pci_command &= (uint16_t)~PCI_CMD_INTERRUPT_DIS;
    CHECK(ath9k_intr_attach(&ah) == 0 && !ah.mac.msi && ah.bus.vector == 0 && msi_handler == 0);
    CHECK(INTX_OFF());
    len = beacon(f, 50);
    CHECK(receive(1, f, len, 0x1b, 5, 0) == 0);
    ath9k_tick(&ah);
    CHECK(rx_count == 1 && ath9k_model_peek(&model, AR_ISR) == 0);

    /* With messages on, the tick finds what a lost message left. */
    setup(1);
    CHECK(receive(0, f, len, 0x1b, 5, 0) == 0);
    ath9k_tick(&ah);
    CHECK(rx_count == 1 && ah.stats.tick_work == 1);
    ath9k_tick(&ah);
    CHECK(ah.stats.tick_work == 1);

    /* rach2.ath9k_nomsi leaves the card polled. */
    ath9k_intr_detach(&ah);
    pci_command &= (uint16_t)~PCI_CMD_INTERRUPT_DIS;
    nomsi = 1;
    CHECK(ath9k_intr_attach(&ah) == 0 && !ah.mac.msi && ah.bus.vector == 0 && msi_handler == 0);
    CHECK(INTX_OFF());
    nomsi = 0;
}

int main(void)
{
    test_rx();
    test_tx();
    test_signal();
    test_keys();
    test_recover();
    test_attach();
    if (fails)
        printf("ath9k_mac_hosttest: %u failed\n", fails);
    return fails > 255 ? 255 : (int)fails;
}
