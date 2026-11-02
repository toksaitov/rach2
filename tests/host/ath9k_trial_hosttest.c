/**
 * Run the whole ath9k driver, every part of sys/network/driver/ath9k/,
 * through the path a trial boot on the Lenovo takes, against the chip
 * model made the Lenovo's AR9565: the probe at boot, `ip link set wlan0
 * up`, interrupts by message and the timer tick while frames arrive, a
 * scan across channels 1 to 13 with a probe request on each, a transmit
 * queue that stops and the reset that recovers it; then the join of task
 * 10's trial, with the station's data path, rate control and key handling
 * linked in: authentication and association to a WPA2 AP on channel 4,
 * the 4-way handshake's EAPOL frames, the pairwise and group keys put in
 * the key cache, encrypted traffic both ways under load with BlockAck
 * aggregation and Minstrel choosing the rate, a stuck queue reset under
 * that load with the keys written back, and the AP's restart, after which
 * the station joins again; and last `ip link set wlan0 down`. It runs
 * twice, in two processes: with the card's messages, and polled from the
 * tick as `rach2.ath9k_nomsi` leaves it.
 *
 * host-deps: tests/host/ath9k_model.c rach2/sys/network/driver/ath9k/hw.c
 * host-deps: rach2/sys/network/driver/ath9k/eeprom.c
 * host-deps: rach2/sys/network/driver/ath9k/chips.c
 * host-deps: rach2/sys/network/driver/ath9k/io.c
 * host-deps: rach2/sys/network/driver/ath9k/phy.c
 * host-deps: rach2/sys/network/driver/ath9k/calib.c
 * host-deps: rach2/sys/network/driver/ath9k/ani.c
 * host-deps: rach2/sys/network/driver/ath9k/mac.c
 * host-deps: rach2/sys/network/driver/ath9k/recv.c
 * host-deps: rach2/sys/network/driver/ath9k/xmit.c
 * host-deps: rach2/sys/network/driver/ath9k/key.c
 * host-deps: rach2/sys/network/driver/ath9k/intr.c
 * host-deps: rach2/sys/network/driver/ath9k/recover.c
 * host-deps: rach2/sys/network/driver/ath9k/antenna.c
 * host-deps: rach2/sys/network/driver/ath9k/power.c
 * host-deps: rach2/sys/network/driver/ath9k/ampdu.c
 * host-deps: rach2/sys/network/wireless/tx.c rach2/sys/network/wireless/rx.c
 * host-deps: rach2/sys/network/wireless/key.c
 * host-deps: rach2/sys/network/wireless/agg_rx.c
 * host-deps: rach2/sys/network/wireless/rate.c
 * host-deps: rach2/sys/network/wireless/elems.c
 * host-deps: rach2/sys/network/wireless/rc_minstrel.c
 * host-deps: rach2/sys/network/wireless/crypto/aes.c
 * host-deps: rach2/sys/network/wireless/crypto/ccmp.c
 * host-deps: rach2/sys/network/wireless/crypto/bip.c
 * host-deps: rach2/sys/network/wireless/crypto/tkip.c
 * host-deps: rach2/sys/lib/kstring.c rach2/sys/lib/kctypes.c
 * host-deps: tests/host/compat/kheap_host.c tests/host/compat/spinlock_host.c
 *
 * The machine is modelled where the driver meets it. A message is sent
 * while the chip raises an enabled cause with its interrupts on, as the
 * AR9003's message engine sends one; the handler runs until the chip
 * stops asking, and a cause that asks again at once more than a thousand
 * times is a storm, which is how a card stops a machine. The worker runs
 * what is due on a millisecond clock, the tick serves the card each
 * millisecond, and every wait the driver makes is counted, so that a call
 * that busy-waits past its bound fails the row. The model counts accesses
 * outside the window, DMA outside the driver's pages and FIFO overruns;
 * each must stay zero.
 *
 * The test plays the MLME and wpa_supplicant where they meet the driver:
 * it moves the station's state as mlme.c and wiphy.c do, and installs the
 * keys as nl80211's NEW_KEY does, but the frames themselves go through the
 * station's data path, tx.c, rx.c, key.c, agg_rx.c and rate.c, as on the
 * machine. It plays the AP from the model's transmit FIFOs: it reads each
 * descriptor the driver pushed and the frame in its buffer, answers it,
 * and finishes the chain with the status a link gives on which MCS 0 to 5
 * get through and MCS 6 and 7 do not, so that rate control has something
 * to find. The chip's encryption is the model's to skip: a frame is
 * judged by what the chip is told to do with it, the key entry, the
 * cipher and the lengths, and the AP's protected frames arrive as the key
 * cache leaves them, decrypted with the CCMP header and MIC in place.
 *
 * The model is ath9k's description of the family, and a chip that behaves
 * otherwise passes here and fails on the machine. What the model cannot
 * show, a register the hardware treats differently from ath9k's
 * description, is the trial's to find.
 *
 * The test exits with the number of failed checks, at most 255.
 */

#include "ath9k_model.h"
#include "irq.h"
#include "klog.h"
#include "timer.h"
#include "udelay.h"
#include "mm/paging.h"
#include "lib/kerrno.h"
#include "lib/krandom.h"
#include "lib/kstring.h"
#include "network/wireless/data.h"
#include "network/wireless/mlme.h"
#include "network/wireless/rc_minstrel.h"
#include "network/wireless/wiphy.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

/* exit alone of <stdlib.h>, whose <sys/types.h> on glibc defines int64_t
 * as long, against kint.h's long long. */
extern void exit(int status);
extern int printf(const char *format, ...);
extern int vsnprintf(char *str, __SIZE_TYPE__ size, const char *format, va_list ap);

static unsigned failures;

#define CHECK(x)                                                                   \
    do {                                                                           \
        if (!(x)) {                                                                \
            printf("ath9k_trial_hosttest: line %u: %s\n", (unsigned)__LINE__, #x); \
            failures++;                                                            \
        }                                                                          \
    } while (0)

/** Whether `s` begins with `prefix`. */
static int starts(const char *s, const char *prefix)
{
    while (*prefix)
        if (*s++ != *prefix++)
            return 0;
    return 1;
}

/* ------------------------------------------------------------------ */
/* The kernel's log and clock.                                         */
/* ------------------------------------------------------------------ */

#define LINES 256
static char lines[LINES][192];
static unsigned n_lines;

/** Keep a kernel log record as the console would print it. */
void klog(int level, const char *subsystem, const char *format, ...)
{
    va_list ap;
    char *p;
    unsigned n = 0;

    (void)level;
    if (n_lines == LINES)
        return;
    p = lines[n_lines++];
    while (*subsystem && n < 100)
        p[n++] = *subsystem++;
    p[n++] = ':';
    p[n++] = ' ';
    va_start(ap, format);
    vsnprintf(p + n, sizeof(lines[0]) - n, format, ap);
    va_end(ap);
}

/** The number of log lines that begin with `prefix`. */
static unsigned logged(const char *prefix)
{
    unsigned i, n = 0;

    for (i = 0; i < n_lines; i++)
        if (starts(lines[i], prefix))
            n++;
    return n;
}

/** Print the log, for a failing row. */
static void print_log(void)
{
    unsigned i;

    for (i = 0; i < n_lines; i++)
        printf("  log: %s\n", lines[i]);
}

static uint64_t now_ms = 10000;

uint64_t tick_count(void)
{
    return now_ms;
}

/** The model counts the driver's waits; the kernel's own is not used. */
void k_udelay(uint32_t usec)
{
    (void)usec;
}

void krandom_bytes(void *buf, uint32_t n)
{
    unsigned char *p = buf;

    while (n--)
        *p++ = 0x5a;
}

/** The boot line has no option: the trial boots the installed line. */
/* The run without messages, as rach2.ath9k_nomsi asks. */
static int polled;

int kcmdline_has(const char *flag)
{
    return polled && strcmp(flag, "rach2.ath9k_nomsi") == 0;
}

/* Interrupts of the CPU: off between kirq_save and kirq_restore. */
static int cpu_irq_off;

uint32_t kirq_save(void)
{
    uint32_t was = (uint32_t)cpu_irq_off;

    cpu_irq_off = 1;
    return was;
}

void kirq_restore(uint32_t flags)
{
    cpu_irq_off = (int)flags;
}

/* ------------------------------------------------------------------ */
/* The bus: the card at 02:00.0 as the Lenovo reports it.              */
/* ------------------------------------------------------------------ */

static uint16_t link_control = 0x0043; /* ASPM L0s and L1 on, as firmware left it */

bool pci_find_device_by_id(uint16_t vendor_id, uint16_t device_id, pci_device_t *out)
{
    unsigned i;

    if (vendor_id != 0x168c || device_id != 0x0036)
        return 0;
    k_memset(out, 0, sizeof(*out));
    out->vendor_id = vendor_id;
    out->device_id = device_id;
    out->class_code = 0x02;
    out->subclass = 0x80;
    out->revision = 1;
    out->bus = 2;
    out->irq = 19;
    for (i = 0; i < 6; i++)
        out->bar[i] = 0;
    out->bar[0] = 0xc0400004u;
    return 1;
}

bool pci_bar_is_memory(uint32_t bar)
{
    return !(bar & 1);
}

uint64_t pci_bar_address64(const pci_device_t *dev, int i)
{
    return (uint64_t)(dev->bar[i] & ~0xfu) | (uint64_t)dev->bar[i + 1] << 32;
}

uint8_t pci_read_config8(uint8_t bus, uint8_t device, uint8_t function, uint8_t offset)
{
    (void)bus;
    (void)device;
    (void)function;
    (void)offset;
    return 0;
}

/** The subsystem IDs and the PCI Express capability's link control. */
uint16_t pci_read_config16(uint8_t bus, uint8_t device, uint8_t function, uint8_t offset)
{
    (void)bus;
    (void)device;
    (void)function;
    if (offset == 0x2c)
        return 0x17aa;
    if (offset == 0x2e)
        return 0x4026;
    if (offset == 0x80)
        return link_control;
    return 0;
}

void pci_write_config8(uint8_t bus, uint8_t device, uint8_t function, uint8_t offset, uint8_t value)
{
    (void)bus;
    (void)device;
    (void)function;
    (void)offset;
    (void)value;
}

void pci_write_config16(uint8_t bus, uint8_t device, uint8_t function, uint8_t offset,
                        uint16_t value)
{
    (void)bus;
    (void)device;
    (void)function;
    if (offset == 0x80)
        link_control = value;
}

/** The PCI Express capability at 0x70 and MSI at 0x50, as the card has. */
uint8_t pci_find_capability(uint8_t bus, uint8_t device, uint8_t function, uint8_t id)
{
    (void)bus;
    (void)device;
    (void)function;
    if (id == 0x10)
        return 0x70;
    if (id == 0x05)
        return 0x50;
    return 0;
}

bool pci_find_bridge_for_bus(uint8_t bus, pci_device_t *out)
{
    (void)bus;
    (void)out;
    return 0;
}

void pci_enable_memory_space(pci_device_t *dev)
{
    (void)dev;
}

void pci_enable_bus_master(pci_device_t *dev)
{
    (void)dev;
}

uintptr_t map_mmio(uintptr_t phys_addr, size_t size)
{
    (void)phys_addr;
    (void)size;
    return 0x10000;
}

/* The message vector. */
static isr_handler_t msi_handler;
static int chip_irq_on_at_msi; /* the chip's interrupts were on when MSI was enabled */
static struct ath9k_model model;

int isr_msi_ready(void)
{
    return 1;
}

int isr_alloc_vector(void)
{
    return 52;
}

void isr_free_vector(int vector)
{
    (void)vector;
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
    if (ath9k_model_peek(&model, AR_IER) & AR_IER_ENABLE)
        chip_irq_on_at_msi = 1;
    return 0;
}

void pci_msi_disable(const pci_device_t *dev)
{
    (void)dev;
}

/* ------------------------------------------------------------------ */
/* DMA memory: pages whose bus addresses the model's dma hook maps.     */
/* ------------------------------------------------------------------ */

#define PAGES    400
#define BUS_BASE 0x10000000u

static unsigned char pages[PAGES][ATH9K_DMA_PAGE] __attribute__((aligned(4096)));
static unsigned pages_used;

void *ath9k_dma_alloc(struct ath9k *ah, uint32_t *bus)
{
    (void)ah;
    if (pages_used == PAGES)
        return 0;
    k_memset(pages[pages_used], 0, ATH9K_DMA_PAGE);
    *bus = BUS_BASE + pages_used * ATH9K_DMA_PAGE;
    return pages[pages_used++];
}

void ath9k_dma_free(struct ath9k *ah, void *page)
{
    (void)ah;
    (void)page;
}

static void *dma_map(struct ath9k_model *m, uint32_t bus, unsigned len)
{
    (void)m;
    if (bus < BUS_BASE || bus + len > BUS_BASE + pages_used * ATH9K_DMA_PAGE)
        return 0;
    return &pages[0][0] + (bus - BUS_BASE);
}

/* ------------------------------------------------------------------ */
/* The station layer.                                                  */
/* ------------------------------------------------------------------ */

static struct wlan_radio *registered;
static struct wlan the_wlan;
static struct wlan_mlme the_mlme; /* what rc_minstrel.c reads of the MLME */
static unsigned rx_frames, tx_statuses, tx_failed, rfkill_reports;
static int rfkill_blocked = -1;

int wlan_register_radio(struct wlan_radio *radio, struct wlan **out)
{
    registered = radio;
    the_wlan.radio = radio;
    the_wlan.mlme_priv = &the_mlme;
    radio->wlan = &the_wlan;
    *out = &the_wlan;
    return 0;
}

void wlan_unregister_radio(struct wlan *wlan)
{
    (void)wlan;
}

/* Suspend to idle, which the trial does not take. */
int wlan_start(struct wlan *wlan)
{
    (void)wlan;
    return 0;
}

void wlan_stop(struct wlan *wlan)
{
    (void)wlan;
}

/* What the radio handed the station, which the worker takes as wiphy.c's
 * queues do: frames, and completions, which never wait for room. */
#define RXQ   256
#define STATQ 1024
static struct {
    uint8_t frame[IEEE80211_MAX_FRAME_LEN];
    unsigned len;
    struct wlan_rx_status status;
} rxq[RXQ];
static unsigned rxq_head, rxq_count, rxq_full;
static struct wlan_tx_status statq[STATQ];
static unsigned statq_head, statq_count, statq_lost;
static int station_on; /* the data path takes what the radio hands up */

int wlan_radio_rx(struct wlan_radio *radio, const uint8_t *frame, unsigned len,
                  const struct wlan_rx_status *status)
{
    unsigned at;

    (void)radio;
    rx_frames++;
    if (!station_on)
        return 0;
    if (rxq_count == RXQ || len > IEEE80211_MAX_FRAME_LEN) {
        rxq_full++;
        return -ENOBUFS;
    }
    at = (rxq_head + rxq_count++) % RXQ;
    k_memcpy(rxq[at].frame, frame, len);
    rxq[at].len = len;
    rxq[at].status = *status;
    return 0;
}

void wlan_radio_tx_status(struct wlan_radio *radio, const struct wlan_tx_status *status)
{
    (void)radio;
    tx_statuses++;
    if (!(status->flags & WLAN_TX_ACKED))
        tx_failed++;
    if (!station_on)
        return;
    if (statq_count == STATQ) {
        statq_lost++;
        return;
    }
    statq[(statq_head + statq_count++) % STATQ] = *status;
}

void wlan_radio_rfkill(struct wlan_radio *radio, int blocked)
{
    (void)radio;
    rfkill_reports++;
    rfkill_blocked = blocked;
}

/* The worker: queued work, run in order of its due time. As work.c does,
 * it refuses a work queued already or whose callback runs. */
#define MAX_WORK 32
static struct wlan_work *queued[MAX_WORK];
static struct wlan_work *work_running;

int wlan_queue_work(struct wlan_work *work, uint64_t due)
{
    unsigned i;

    if (work == work_running)
        return -EBUSY;
    for (i = 0; i < MAX_WORK; i++)
        if (queued[i] == work)
            return -EBUSY;
    for (i = 0; i < MAX_WORK; i++)
        if (!queued[i]) {
            work->due = due;
            work->pending = 1;
            queued[i] = work;
            return 0;
        }
    printf("ath9k_trial_hosttest: the worker's table is full\n");
    failures++;
    return -EBUSY;
}

void wlan_cancel_work(struct wlan_work *work)
{
    unsigned i;

    for (i = 0; i < MAX_WORK; i++)
        if (queued[i] == work) {
            queued[i] = 0;
            work->pending = 0;
        }
}

/* ------------------------------------------------------------------ */
/* The driver, whole.                                                  */
/* ------------------------------------------------------------------ */

#include "network/driver/ath9k/pci.c"

#include "ath9k_lenovo.h"

/* The peak detector's output turns at this DAC value, as the phy test's
 * card does. */
#define PEAK_DAC 40

/* Accesses to the MAC, the baseband or the radio while the chip was not
 * awake, which an AR9003 may answer with a stalled bus; the host interface
 * (0x4000 to 0x4fff) and the RTC (0x7000 to 0x7fff) answer in every state. */
static unsigned asleep_accesses;
static uint32_t asleep_first; /* the first such register */

/* Writes of the PCIe PHY's table, which ath9k makes at the attach, the
 * start and the stop while ASPM is on. */
static unsigned serdes_writes;

/* Accesses at or past the end of the window pci.c maps, which on the
 * machine reach no register of the card. */
static unsigned beyond_window;
static uint32_t beyond_first;

/** Count an access to `reg` past the mapped window or while the RTC is not
 * on. */
static void awake_check(const struct ath9k_model *m, uint32_t reg)
{
    if (reg + 4 > ATH9K_WINDOW && beyond_window++ == 0)
        beyond_first = reg;
    if ((reg >= 0x4000 && reg <= 0x4fff) || (reg >= 0x7000 && reg <= 0x7fff))
        return;
    if ((m->reg[AR_RTC_RESET / 4] & AR_RTC_RESET_EN) &&
        (m->reg[AR_RTC_FORCE_WAKE / 4] & AR_RTC_FORCE_WAKE_EN))
        return;
    if (asleep_accesses++ == 0)
        asleep_first = reg;
}

static int chip_asks(void);

/* AR_INTR_SYNC_CAUSE, the PCIe errors, which clear by writing ones. */
static uint32_t sync_cause;

/* The receiver hears nothing until the MAC's next reset, as a receive path
 * that stalls; the air's frames pass it by. */
static int rx_deaf;

/* The radio switch: the card's EEPROM gives RF silence on GPIO 11, active
 * low (rfSilent 0x2d), and the machine's reads it unblocked. */
static int radio_switch_on = 1;

/** Answer AR_INTR_SYNC_CAUSE and the radio switch's GPIO; grant the radio bus
 * while it is asked for, and turn the peak detector's output at PEAK_DAC, as
 * the hardware does. */
static int chip_read(struct ath9k_model *m, uint32_t reg, uint32_t *value)
{
    awake_check(m, reg);
    if (reg == AR_INTR_SYNC_CAUSE) {
        *value = sync_cause;
        return 1;
    }
    if (reg == AR_INTR_ASYNC_CAUSE) {
        /* The MAC's interrupt reaches the cause only while AR_IER and the
         * async enable let it out, as on the chip. */
        *value = chip_asks() ? AR_INTR_MAC_IRQ : 0;
        return 1;
    }
    if (reg == AR_GPIO_IN) {
        *value = radio_switch_on ? 1u << 11 : 0;
        return 1;
    }
    if (reg == AR_PHY_RFBUS_GRANT) {
        *value = (m->reg[AR_PHY_RFBUS_REQ / 4] & AR_PHY_RFBUS_REQ_EN) ? AR_PHY_RFBUS_GRANT_EN : 0;
        return 1;
    }
    if (reg == AR_PHY_65NM_RXRF_AGC(0)) {
        uint32_t v = m->reg[reg / 4] & ~AR_PHY_65NM_RXRF_AGC_AGC_OUT;

        if (MS(v, AR_PHY_65NM_RXRF_AGC_AGC2G_CALDAC_OVR) > PEAK_DAC)
            v |= AR_PHY_65NM_RXRF_AGC_AGC_OUT;
        *value = v;
        return 1;
    }
    return 0;
}

/** Finish the AGC and noise floor calibrations and the RX IQ sample as
 * soon as they are started, as the hardware clears their bits, and clear
 * the sync causes written. */
static void chip_write(struct ath9k_model *m, uint32_t reg, uint32_t value)
{
    awake_check(m, reg);
    if (reg == 0x18c00) /* the first row of the PCIe PHY's table */
        serdes_writes++;
    if (reg == AR_RTC_RC && value)
        rx_deaf = 0;
    if (reg == AR_INTR_SYNC_CAUSE)
        sync_cause &= ~value;
    if (reg == AR_PHY_AGC_CONTROL)
        m->reg[reg / 4] = value & ~(AR_PHY_AGC_CONTROL_CAL | AR_PHY_AGC_CONTROL_NF);
    else if (reg == AR_PHY_TIMING4)
        m->reg[reg / 4] = value & ~AR_PHY_TIMING4_DO_CAL;
}

/* What one call may wait, at most, in microseconds: the longest bounded
 * wait of the driver is the offset calibration's 100 ms. */
#define WAIT_BOUND 250000u

/* The longest wait of any one call, and the calls that passed the bound. */
static uint64_t longest_wait;
static unsigned over_bound;

/** Note what the last call waited, from the model's clock at its start. */
static void waited_since(uint64_t start, const char *what)
{
    uint64_t w = model.usec - start;

    if (w > longest_wait)
        longest_wait = w;
    if (w > WAIT_BOUND) {
        over_bound++;
        printf("  %s waited %lu us\n", what, (unsigned long)w);
    }
}

/* Messages the card sent, and storms: a cause that asked again at once. */
static unsigned messages, storms;

/** Whether the chip asks for an interrupt: an enabled cause raised with
 * its interrupts on, as AR_INTR_ASYNC_CAUSE's MAC bit and the enables
 * decide it. */
static int chip_asks(void)
{
    uint32_t isr = ath9k_model_peek(&model, AR_ISR);
    uint32_t imr = ath9k_model_peek(&model, AR_IMR);

    if (!(ath9k_model_peek(&model, AR_IER) & AR_IER_ENABLE))
        return 0;
    if (!(ath9k_model_peek(&model, AR_INTR_ASYNC_ENABLE) & AR_INTR_MAC_IRQ))
        return 0;
    return (isr & imr) != 0;
}

/** Send messages while the chip asks, as the machine delivers them. */
static void deliver(void)
{
    unsigned n = 0;
    uint64_t start = model.usec;

    while (msi_handler && chip_asks() && !cpu_irq_off) {
        msi_handler(0);
        messages++;
        if (++n > 1000) {
            storms++;
            printf("  storm: AR_ISR 0x%x AR_IMR 0x%x\n", ath9k_model_peek(&model, AR_ISR),
                   ath9k_model_peek(&model, AR_IMR));
            break;
        }
    }
    waited_since(start, "the interrupt");
}

/** Run the work that is due, earliest first. */
static void run_work(void)
{
    for (;;) {
        struct wlan_work *w = 0;
        unsigned i, at = 0;
        uint64_t start;

        for (i = 0; i < MAX_WORK; i++)
            if (queued[i] && queued[i]->due <= now_ms && (!w || queued[i]->due < w->due)) {
                w = queued[i];
                at = i;
            }
        if (!w)
            return;
        queued[at] = 0;
        w->pending = 0;
        start = model.usec;
        work_running = w;
        w->run(w);
        work_running = 0;
        waited_since(start, "work");
        deliver();
    }
}

/* The beacons and probe responses the air carries on each channel. */
static unsigned air_frames;

/** Put a beacon of the BSS on channel `number` into the low-priority
 * FIFO, when the radio listens there. */
static void air(unsigned number)
{
    static const uint8_t bssid[6] = {0x02, 0xaa, 0xbb, 0xcc, 0xdd, 0x00};
    uint8_t f[64];
    struct ar9003_rxs st;

    if (!model.rxe || rx_deaf || card.phy.channel.number != number)
        return;
    k_memset(f, 0, sizeof(f));
    k_memset(&st, 0, sizeof(st));
    f[0] = 0x80; /* beacon */
    k_memset(f + 4, 0xff, 6);
    k_memcpy(f + 10, bssid, 6);
    k_memcpy(f + 16, bssid, 6);
    f[16 + 5] = (uint8_t)number;
    st.status11 = AR_RxFrameOK;
    if (ath9k_model_rx(&model, 0, f, sizeof(f), &st) == 0)
        air_frames++;
}

/* ------------------------------------------------------------------ */
/* The AP: a WPA2 network on channel 4, as TDA-NET is.                 */
/* ------------------------------------------------------------------ */

#define AP_FREQ 2427
static const uint8_t ap_mac[6] = {0x02, 0xaa, 0xbb, 0xcc, 0xdd, 0x04};
/* The Mac, behind the AP. */
static const uint8_t far_mac[6] = {0x02, 0x10, 0x20, 0x30, 0x40, 0x50};
static const uint8_t bcast[6] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
static const uint8_t snap[6] = {0xaa, 0xaa, 0x03, 0x00, 0x00, 0x00};

/* The station's data path, its counters and its BSS. */
static struct wlan_data data;
static struct wlan_counters counters;
static struct wlan_bss bss;

/* The frames the AP sends, each once the radio listens on its channel
 * and a buffer is free, as the air holds them for the next millisecond. */
#define AIRQ 512
static struct {
    uint8_t frame[IEEE80211_MAX_FRAME_LEN];
    unsigned len;
    struct ar9003_rxs status;
} airq[AIRQ];
static unsigned airq_head, airq_count;

/* What the AP is and has heard. */
static struct {
    int present; /* it plays the frames' part; before it, every frame is acknowledged */
    int on;      /* it beacons and answers; off, nothing it is sent gets through */
    unsigned auths, assocs, m2, m4, addba;
    unsigned data; /* the station's protected data, every check passed */
    unsigned bad;  /* frames that failed a check */
    unsigned aggregates, subframes;
    uint64_t pn;            /* the last PN under the pairwise key */
    unsigned rate_code;     /* the first series of the last data frame */
    unsigned fast;          /* data frames whose first series was MCS 3 or faster */
    uint64_t tx_pn, gtx_pn; /* the AP's own PNs, pairwise and group */
    uint16_t seq;
    unsigned sent_data; /* protected data frames it sent the station */
} ap;

/* The keys the handshake agrees, and the entries the driver gave them. */
static const uint8_t tk[16] = {0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17,
                               0x18, 0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f};
static const uint8_t gtk[16] = {0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27, 0x28,
                                0x29, 0x2a, 0x2b, 0x2c, 0x2d, 0x2e, 0x2f, 0x20};

/* What the station's data path delivered, as the network stack sees it. */
static unsigned delivered_eapol[5], delivered_ip, delivered_bad;

/** The descriptor at bus address `bus`, or NULL. */
static struct ar9003_txc *desc_at(uint32_t bus)
{
    return dma_map(&model, bus, sizeof(struct ar9003_txc));
}

/** The descriptors of the oldest chain in queue `q`'s FIFO, by their
 * links, into `out`; returns how many. */
static unsigned chain_of(unsigned q, struct ar9003_txc **out, unsigned max)
{
    const struct ath9k_model_fifo *f = &model.tx[q];
    uint32_t bus;
    unsigned n = 0;

    if (f->count == 0)
        return 0;
    bus = f->bus[f->head % f->depth];
    while (bus && n < max) {
        out[n] = desc_at(bus);
        if (!out[n])
            break;
        bus = out[n++]->link;
    }
    return n;
}

/** The frame a descriptor sends, the chip's padding in it, or NULL. */
static const uint8_t *frame_of(const struct ar9003_txc *dc)
{
    return dma_map(&model, dc->data0, MS(dc->ctl3, AR_BufLen));
}

/** The 12-bit sequence number of the frame a descriptor sends. */
static unsigned seq_of(const struct ar9003_txc *dc)
{
    const uint8_t *f = frame_of(dc);

    return f ? ((unsigned)f[22] | (unsigned)f[23] << 8) >> 4 : 0xffff;
}

/** Queue a frame of the AP's: `body` of `n` bytes after a header of
 * `fc`, to `da`, with its receive status words. */
static void ap_queue(unsigned fc, const uint8_t *da, const uint8_t *body, unsigned n,
                     uint32_t status11)
{
    uint8_t *f;
    unsigned at;

    if (airq_count == AIRQ || 24 + n + 4 > IEEE80211_MAX_FRAME_LEN) {
        printf("ath9k_trial_hosttest: the air's queue is full\n");
        failures++;
        return;
    }
    at = (airq_head + airq_count++) % AIRQ;
    f = airq[at].frame;
    k_memset(f, 0, 24);
    f[0] = (uint8_t)fc;
    f[1] = (uint8_t)(fc >> 8);
    k_memcpy(f + 4, da, 6);
    k_memcpy(f + 10, ap_mac, 6);
    k_memcpy(f + 16, (fc & 0x0c) == 0x08 ? far_mac : ap_mac, 6);
    f[22] = (uint8_t)(ap.seq << 4);
    f[23] = (uint8_t)(ap.seq >> 4);
    ap.seq = (uint16_t)((ap.seq + 1) & 0xfff);
    k_memcpy(f + 24, body, n);
    k_memset(f + 24 + n, 0, 4); /* the FCS, which recv.c strips */
    airq[at].len = 24 + n + 4;
    k_memset(&airq[at].status, 0, sizeof(airq[at].status));
    airq[at].status.status11 = AR_RxFrameOK | status11;
    airq[at].status.status1 = SM(0x0b, AR_RxRate); /* 6 Mbit/s, OFDM */
}

/** Queue a management frame of subtype `stype` to the station. */
static void ap_mgmt(unsigned stype, const uint8_t *body, unsigned n)
{
    ap_queue(stype, card.radio.mac, body, n, 0);
}

/** Queue a data frame of `ethertype` with `n` bytes of `payload`: in the
 * clear, or as the key cache leaves a frame it decrypted, its CCMP header
 * and MIC in place, under the pairwise key or, to every station, the
 * group key of index 1. `slot` is the pairwise key's entry. */
static void ap_data(unsigned ethertype, const uint8_t *payload, unsigned n, int protect, int group,
                    int slot)
{
    static uint8_t b[IEEE80211_MAX_FRAME_LEN];
    unsigned at = 0, fc = 0x0208; /* data, FromDS */
    uint32_t s11 = 0;

    if (protect) {
        uint64_t pn = group ? ++ap.gtx_pn : ++ap.tx_pn;

        fc |= IEEE80211_FCTL_PROTECTED;
        b[0] = (uint8_t)pn;
        b[1] = (uint8_t)(pn >> 8);
        b[2] = 0;
        b[3] = (uint8_t)(0x20 | (group ? 1 : 0) << 6);
        b[4] = (uint8_t)(pn >> 16);
        b[5] = (uint8_t)(pn >> 24);
        b[6] = (uint8_t)(pn >> 32);
        b[7] = (uint8_t)(pn >> 40);
        at = 8;
        /* A pairwise entry is found by the transmitter's address; a group
         * key by the key index, which recv.c reads from the header. */
        if (!group)
            s11 = AR_RxKeyIdxValid | SM((unsigned)slot, AR_KeyIdx);
    }
    k_memcpy(b + at, snap, 6);
    b[at + 6] = (uint8_t)(ethertype >> 8);
    b[at + 7] = (uint8_t)ethertype;
    k_memcpy(b + at + 8, payload, n);
    at += 8 + n;
    if (protect) {
        k_memset(b + at, 0, 8); /* the MIC, which the chip checked and left */
        at += 8;
        ap.sent_data++;
    }
    ap_queue(fc, group ? bcast : card.radio.mac, b, at, s11);
}

/** Put the AP's queued frames into the receive FIFO while it has room
 * and the radio listens on channel 4; the rest wait. */
static void ap_air(void)
{
    if (rx_deaf)
        airq_count = 0;
    while (airq_count && model.rxe && card.phy.channel.frequency == AP_FREQ) {
        if (ath9k_model_rx(&model, 0, airq[airq_head].frame, airq[airq_head].len,
                           &airq[airq_head].status) != 0)
            return;
        airq_head = (airq_head + 1) % AIRQ;
        airq_count--;
    }
}

/* The RSSI of the AP's beacons over the noise floor, a strong signal. */
#define AP_BEACON_RSSI 45

/** The AP's beacon, every 100 ms while it is on. */
static void ap_beacon(void)
{
    uint8_t b[12 + 11];

    k_memset(b, 0, sizeof(b));
    b[8] = 100; /* the beacon interval */
    b[10] = (uint8_t)(WLAN_CAPABILITY_ESS | WLAN_CAPABILITY_PRIVACY);
    b[12] = 0;
    b[13] = 7;
    k_memcpy(b + 14, "TDA-NET", 7);
    /* A beacon goes to every station; ap_queue's address is the BSSID. */
    ap_queue(0x0080, bcast, b, 12 + 9, 0);
    if (airq_count)
        airq[(airq_head + airq_count - 1) % AIRQ].status.status5 =
            SM(AP_BEACON_RSSI, AR_RxRSSICombined);
}

/** Judge a protected data frame of the station's, which descriptor `dc`
 * sends from `f` with `pad` bytes after its `hdrlen`-byte header: the
 * chip must be told to encrypt it with the pairwise entry under AES, the
 * frame's length must count the MIC and the FCS the chip appends, which
 * the buffer must leave out, and its PN must grow. Returns the ethertype,
 * or 0 for a frame that failed. */
static unsigned ap_check_protected(const struct ar9003_txc *dc, const uint8_t *f, unsigned hdrlen,
                                   unsigned pad, int slot)
{
    unsigned buflen = MS(dc->ctl3, AR_BufLen);
    const uint8_t *iv = f + hdrlen + pad;
    uint64_t pn;

    if (!(dc->ctl11 & AR_DestIdxValid) || (int)MS(dc->ctl12, AR_DestIdx) != slot ||
        MS(dc->ctl17, AR_EncrType) != 2 || (dc->ctl11 & AR_FrameLen) != buflen - pad + 8 + 4 ||
        buflen < hdrlen + pad + 16)
        return 0;
    if (!(iv[3] & 0x20) || (iv[3] >> 6) != 0)
        return 0;
    pn = (uint64_t)iv[0] | (uint64_t)iv[1] << 8 | (uint64_t)iv[4] << 16 | (uint64_t)iv[5] << 24 |
         (uint64_t)iv[6] << 32 | (uint64_t)iv[7] << 40;
    if (pn <= ap.pn)
        return 0;
    ap.pn = pn;
    if (memcmp(iv + 8, snap, 6) != 0)
        return 0;
    return (unsigned)iv[14] << 8 | iv[15];
}

static int pairwise_slot(void);

/** The AP hears the frame descriptor `dc` sends, and answers it. */
static void ap_hear(const struct ar9003_txc *dc)
{
    static const uint8_t auth_ok[6] = {0, 0, 2, 0, 0, 0};
    /* Capability, status and AID 1; the BSS's elements are the test's
     * MLME's, which keeps the AP's from its scan. */
    static const uint8_t assoc_ok[6] = {0x11, 0x04, 0x00, 0x00, 0x01, 0xc0};
    const uint8_t *f = frame_of(dc);
    unsigned fc, hdrlen, pad, buflen = MS(dc->ctl3, AR_BufLen), type;

    if (!f || buflen < 10) {
        ap.bad++;
        return;
    }
    fc = (unsigned)f[0] | (unsigned)f[1] << 8;
    hdrlen = ath9k_header_len(fc);
    pad = (hdrlen & 3) ? 4 - (hdrlen & 3) : 0;
    if ((fc & 0x0c) == 0x00) {
        switch (fc & 0xf0) {
        case 0xb0: /* authentication */
            ap.auths++;
            ap_mgmt(0x00b0, auth_ok, sizeof(auth_ok));
            break;
        case 0x00: /* association request */
            ap.assocs++;
            ap_mgmt(0x0010, assoc_ok, sizeof(assoc_ok));
            break;
        case 0xd0: /* action */
            if (buflen >= 24 + 9 && f[24] == WLAN_CATEGORY_BACK && f[25] == WLAN_ACTION_ADDBA_REQ) {
                uint8_t r[9];
                unsigned params = (unsigned)f[27] | (unsigned)f[28] << 8;

                ap.addba++;
                r[0] = WLAN_CATEGORY_BACK;
                r[1] = WLAN_ACTION_ADDBA_RESP;
                r[2] = f[26];
                r[3] = r[4] = 0;
                params = (params & 0x003c) | 0x0002 | 64u << 6;
                r[5] = (uint8_t)params;
                r[6] = (uint8_t)(params >> 8);
                r[7] = r[8] = 0;
                ap_mgmt(0x00d0, r, sizeof(r));
            }
            break;
        default:
            break;
        }
        return;
    }
    if ((fc & 0x0c) != 0x08)
        return; /* a BlockAckReq the driver sends, which the AP acknowledges */
    if (buflen < hdrlen + pad + 8) {
        ap.bad++;
        return;
    }
    type = 0;
    if (fc & IEEE80211_FCTL_PROTECTED) {
        type = ap_check_protected(dc, f, hdrlen, pad, pairwise_slot());
        if (!type) {
            ap.bad++;
            return;
        }
        if (type == 0x0800) {
            ap.data++;
            ap.rate_code = MS(dc->ctl14, AR_XmitRate0);
            if ((ap.rate_code & 0x80) && (ap.rate_code & 0x7f) >= 3)
                ap.fast++;
        }
        return;
    }
    /* In the clear only EAPOL, which goes out without a key cache entry. */
    if (memcmp(f + hdrlen + pad, snap, 6) != 0 || f[hdrlen + pad + 6] != 0x88 ||
        f[hdrlen + pad + 7] != 0x8e || (dc->ctl11 & AR_DestIdxValid) ||
        (dc->ctl11 & AR_FrameLen) != buflen - pad + 4) {
        ap.bad++;
        return;
    }
    if (f[hdrlen + pad + 8] == 2) {
        static const uint8_t m3[1] = {3};

        ap.m2++;
        ap_data(0x888e, m3, 1, 0, 0, -1);
    } else if (f[hdrlen + pad + 8] == 4) {
        ap.m4++;
    }
}

/** Whether a link that carries MCS 0 to 5, CCK and OFDM up to 24 Mbit/s
 * carries the rate code `code`: ath9k's codes for 36, 48 and 54 Mbit/s
 * are 0x0d, 0x08 and 0x0c. */
static int rate_gets_through(unsigned code)
{
    if (code & 0x80)
        return (code & 0x7f) <= 5;
    return code != 0x0d && code != 0x08 && code != 0x0c;
}

/**
 * Finish queue `q`'s oldest chain as the AP's link does: the AP hears
 * each frame, and the chain goes out at the first series whose rate gets
 * through, an aggregate acknowledged whole by a BlockAck from its lowest
 * number; with the AP off, or no series that gets through, every try of
 * every series fails.
 */
static void ap_finish(unsigned q)
{
    struct ar9003_txc *dc[ATH9K_BA_WINDOW];
    struct ar9003_txs st;
    unsigned n = chain_of(q, dc, ATH9K_BA_WINDOW), i, final = 0, tries = 0, ok = 0;

    if (n == 0) {
        (void)ath9k_model_tx_complete(&model, q, NULL);
        return;
    }
    if (MS(dc[0]->ctl13, AR_XmitDataTries0) == 0 || MS(dc[0]->ctl14, AR_XmitRate0) == 0)
        ap.bad++; /* a descriptor ath9k never writes, which the chip may not send */
    for (i = 0; i < 4; i++) {
        unsigned t = (dc[0]->ctl13 >> (16 + 4 * i)) & 0xf;
        unsigned code = (dc[0]->ctl14 >> (8 * i)) & 0xff;

        if (t == 0)
            break;
        final = i;
        tries = t;
        if (ap.on && rate_gets_through(code)) {
            ok = 1;
            break;
        }
    }
    if (ok)
        for (i = 0; i < n; i++)
            ap_hear(dc[i]);
    k_memset(&st, 0, sizeof(st));
    st.status8 = SM(final, AR_FinalTxIdx);
    if (!ok) {
        st.status3 = AR_ExcessiveRetries | SM(tries ? tries - 1 : 0, AR_DataFailCnt);
    } else {
        st.status3 = AR_FrmXmitOK;
        if (dc[0]->ctl12 & AR_IsAggr) {
            unsigned base = seq_of(dc[0]);
            uint64_t bits = 0;

            for (i = 1; i < n; i++)
                if (((base - seq_of(dc[i])) & 0xfff) < ATH9K_BA_WINDOW)
                    base = seq_of(dc[i]);
            for (i = 0; i < n; i++)
                bits |= (uint64_t)1 << ((seq_of(dc[i]) - base) & 0x3f);
            st.status2 = AR_TxBaStatus;
            st.status5 = (uint32_t)bits;
            st.status6 = (uint32_t)(bits >> 32);
            st.status8 |= SM(base, AR_SeqNum);
            ap.aggregates++;
            ap.subframes += n;
        }
    }
    (void)ath9k_model_tx_complete(&model, q, &st);
}

/* ------------------------------------------------------------------ */
/* The station: the data path, and the MLME's and the supplicant's     */
/* calls into it.                                                      */
/* ------------------------------------------------------------------ */

/** The data path's delivery to the network stack: EAPOL to the
 * supplicant, by the message number the test's frames carry in their
 * first byte, and IPv4 to the stack. */
static int station_deliver(struct wlan_data *d, const uint8_t *frame, unsigned len)
{
    (void)d;
    if (len < 24 + 9 || memcmp(frame + 24, snap, 6) != 0) {
        delivered_bad++;
        return 0;
    }
    if (frame[30] == 0x88 && frame[31] == 0x8e && frame[32] < 5)
        delivered_eapol[frame[32]]++;
    else if (frame[30] == 0x08 && frame[31] == 0x00 && frame[32] == 0x45)
        delivered_ip++;
    else
        delivered_bad++;
    return 0;
}

static void station_event(struct wlan *wlan, const struct wlan_event *event)
{
    (void)wlan;
    (void)event;
    delivered_bad++; /* a MIC failure, which no frame here should raise */
}

/* What the station's MLME got back. */
static unsigned got_auth, got_assoc;

/** Take what the radio queued, as wiphy.c's worker and data.c's
 * wlan_data_dispatch do: completions first, then frames, data to the data
 * path and Block Ack actions to agg_rx.c, authentication and association
 * responses to the MLME; beacons are the BSS table's, not linked here. */
static void station_run(void)
{
    while (statq_count) {
        struct wlan_tx_status st = statq[statq_head];

        statq_head = (statq_head + 1) % STATQ;
        statq_count--;
        wlan_data_tx_status(&data, &st);
    }
    while (rxq_count) {
        uint8_t *f = rxq[rxq_head].frame;
        unsigned len = rxq[rxq_head].len, fc = (unsigned)f[0] | (unsigned)f[1] << 8;

        if ((fc & 0x0c) == 0x08) {
            (void)wlan_data_rx(&data, f, len, &rxq[rxq_head].status);
        } else if ((fc & 0x0c) == 0x00 && memcmp(f + 4, card.radio.mac, 6) == 0) {
            if ((fc & 0xf0) == 0xb0)
                got_auth++;
            else if ((fc & 0xf0) == 0x10)
                got_assoc++;
            else if ((fc & 0xf0) == 0xd0 && len > 24 && f[24] == WLAN_CATEGORY_BACK)
                (void)wlan_data_rx_action(&data, f, len, 0);
        }
        rxq_head = (rxq_head + 1) % RXQ;
        rxq_count--;
    }
}

/** Move the station to `state` as wiphy.c's wlan_set_state does: the
 * radio first, then the data path's connection; a move to NONE always
 * happens. */
static int station_state(enum wlan_sta_state state)
{
    enum wlan_sta_state old = the_wlan.state;
    const struct wlan_bss *b = state == WLAN_STA_NONE ? 0 : &bss;
    int rc = registered->ops->sta_state(registered, b, old, state);

    if (rc && state != WLAN_STA_NONE)
        return rc;
    the_wlan.state = state;
    the_mlme.state = state;
    if (state >= WLAN_STA_ASSOCIATED && old < WLAN_STA_ASSOCIATED)
        rc = wlan_data_connect(&data, &bss, 1, 1, 0);
    else if (state < WLAN_STA_ASSOCIATED && old >= WLAN_STA_ASSOCIATED)
        wlan_data_disconnect(&data);
    return rc;
}

/** Send a management frame of subtype `stype` with `body` to the AP, as
 * sta.c does through wlan_data_tx_mgmt on the AP's channel. */
static int station_mgmt(unsigned stype, const uint8_t *body, unsigned n)
{
    static uint8_t f[IEEE80211_MAX_FRAME_LEN];

    k_memset(f, 0, 24);
    f[0] = (uint8_t)stype;
    k_memcpy(f + 4, ap_mac, 6);
    k_memcpy(f + 10, card.radio.mac, 6);
    k_memcpy(f + 16, ap_mac, 6);
    k_memcpy(f + 24, body, n);
    return wlan_data_tx_mgmt(&data, f, 24 + n, sizeof(f), AP_FREQ, 0, 0);
}

/** Send `n` bytes of `payload` of `ethertype` to the Mac at 802.1D
 * priority `prio`, as data.c's drain does with what wlan_encap made. */
static int station_send(unsigned ethertype, const uint8_t *payload, unsigned n, unsigned prio)
{
    static uint8_t f[IEEE80211_MAX_FRAME_LEN];

    k_memset(f, 0, 24);
    f[0] = 0x08;
    f[1] = 0x01; /* ToDS */
    k_memcpy(f + 4, ap_mac, 6);
    k_memcpy(f + 10, card.radio.mac, 6);
    k_memcpy(f + 16, far_mac, 6);
    k_memcpy(f + 24, snap, 6);
    f[30] = (uint8_t)(ethertype >> 8);
    f[31] = (uint8_t)ethertype;
    k_memcpy(f + 32, payload, n);
    return wlan_data_tx(&data, f, 32 + n, sizeof(f), prio);
}

/** The key cache entry the pairwise key took, or -1. */
static int pairwise_slot(void)
{
    struct wlan_data_key *k = wlan_data_pairwise_key(&data, 0);

    return k ? k->key.hw_slot : -1;
}

/** Install a CCMP key as nl80211's NEW_KEY does: the pairwise key for the
 * AP, or the group key of index 1. */
static int install_key(int pairwise)
{
    struct wlan_key k;

    k_memset(&k, 0, sizeof(k));
    k.cipher = WLAN_CIPHER_SUITE_CCMP;
    k.len = 16;
    k.hw_slot = -1;
    if (pairwise) {
        k.index = 0;
        k.flags = WLAN_KEY_PAIRWISE;
        k_memcpy(k.peer, ap_mac, 6);
        k_memcpy(k.data, tk, 16);
    } else {
        k.index = 1;
        k.flags = WLAN_KEY_DEFAULT;
        k_memset(k.peer, 0xff, 6);
        k_memcpy(k.data, gtk, 16);
    }
    return wlan_data_install_key(&data, &k);
}

/** Whether key cache entry `e` holds the CCMP key `key` for `mac`, or for
 * no address when it is NULL, as ath's key.c writes it; with `key` NULL,
 * whether the entry is clear. */
static int entry_holds(unsigned e, const uint8_t *key, const uint8_t *mac)
{
    uint32_t lo = 0, hi = 0;

    if (!key)
        return ath9k_model_peek(&model, AR_KEYTABLE_TYPE(e)) == AR_KEYTABLE_TYPE_CLR &&
               ath9k_model_peek(&model, AR_KEYTABLE_KEY0(e)) == 0 &&
               ath9k_model_peek(&model, AR_KEYTABLE_MAC1(e)) == 0;
    if (mac) {
        lo = (uint32_t)mac[0] | (uint32_t)mac[1] << 8 | (uint32_t)mac[2] << 16 |
             (uint32_t)mac[3] << 24;
        hi = (uint32_t)mac[4] | (uint32_t)mac[5] << 8;
        lo = lo >> 1 | (hi & 1) << 31;
        hi >>= 1;
    }
    return ath9k_model_peek(&model, AR_KEYTABLE_TYPE(e)) == AR_KEYTABLE_TYPE_CCM &&
           ath9k_model_peek(&model, AR_KEYTABLE_KEY0(e)) ==
               ((uint32_t)key[0] | (uint32_t)key[1] << 8 | (uint32_t)key[2] << 16 |
                (uint32_t)key[3] << 24) &&
           ath9k_model_peek(&model, AR_KEYTABLE_KEY1(e)) ==
               ((uint32_t)key[4] | (uint32_t)key[5] << 8) &&
           ath9k_model_peek(&model, AR_KEYTABLE_KEY4(e)) ==
               ((uint32_t)key[12] | (uint32_t)key[13] << 8 | (uint32_t)key[14] << 16 |
                (uint32_t)key[15] << 24) &&
           ath9k_model_peek(&model, AR_KEYTABLE_MAC0(e)) == lo &&
           ath9k_model_peek(&model, AR_KEYTABLE_MAC1(e)) == (hi | AR_KEYTABLE_VALID);
}

/** Let `ms` milliseconds pass: each one a tick, the work due and the
 * messages the chip sends, with a beacon every 100 ms on channel 1, 6 and
 * 11 and, while it is on, the AP's on channel 4; the AP's frames go into
 * the receive FIFO, the queued frames are sent after a millisecond, and
 * the station takes what the radio handed it. */
static void run_for(unsigned ms)
{
    unsigned i, q;

    for (i = 0; i < ms; i++) {
        uint64_t start = model.usec;

        now_ms++;
        if (now_ms % 100 == 0) {
            air(1);
            air(6);
            air(11);
        }
        if (ap.on && now_ms % 100 == 0)
            ap_beacon();
        ap_air();
        if (!model.stuck_tx_dma)
            for (q = 0; q < AR_NUM_QCU; q++)
                while (model.tx[q].count) {
                    if (ap.present)
                        ap_finish(q);
                    else
                        (void)ath9k_model_tx_complete(&model, q, NULL);
                }
        deliver();
        ath9k_poll();
        waited_since(start, "the tick");
        run_work();
        station_run();
    }
}

/** Send a probe request on the current channel, as the scan does. */
static int probe_request(void)
{
    static uint8_t f[48];
    struct wlan_tx_info info;

    k_memset(f, 0, sizeof(f));
    k_memset(&info, 0, sizeof(info));
    f[0] = 0x40;
    k_memset(f + 4, 0xff, 6);
    k_memcpy(f + 10, card.radio.mac, 6);
    k_memset(f + 16, 0xff, 6);
    info.ac = WLAN_AC_VO;
    info.cookie = now_ms;
    info.rates[0].index = 0;
    info.rates[0].count = 2;
    info.rates[1].index = -1;
    info.rates[2].index = -1;
    info.rates[3].index = -1;
    return registered->ops->tx(registered, f, sizeof(f), &info);
}

/** The model's faults so far must be none. */
static void model_clean(const char *step)
{
    if (model.unmapped || model.dma_faults || model.overruns || storms || over_bound ||
        asleep_accesses || beyond_window) {
        printf("  after %s: unmapped %u, dma faults %u, overruns %u, storms %u, over %u, "
               "asleep %u (first 0x%x), past the window %u (first 0x%x)\n",
               step, model.unmapped, model.dma_faults, model.overruns, storms, over_bound,
               asleep_accesses, asleep_first, beyond_window, beyond_first);
        failures++;
    }
}

/* ------------------------------------------------------------------ */
/* Task 10's trial: the join, traffic, a reset under load and the AP's */
/* restart.                                                            */
/* ------------------------------------------------------------------ */

/** Whether the chip's BSSID is `mac`, with association ID 0. */
static int bssid_is(const uint8_t *mac)
{
    return ath9k_model_peek(&model, AR_BSS_ID0) ==
               ((uint32_t)mac[0] | (uint32_t)mac[1] << 8 | (uint32_t)mac[2] << 16 |
                (uint32_t)mac[3] << 24) &&
           ath9k_model_peek(&model, AR_BSS_ID1) == ((uint32_t)mac[4] | (uint32_t)mac[5] << 8);
}

/**
 * The join, as wpa_supplicant drives it through nl80211 and mlme.c: the
 * AP's channel, Open System authentication, association, the 4-way
 * handshake's four EAPOL frames, the group and pairwise keys installed,
 * and the port authorized.
 */
static void join(void)
{
    static const uint8_t none[6] = {0, 0, 0, 0, 0, 0};
    static const uint8_t auth_req[6] = {0, 0, 1, 0, 0, 0};
    static const uint8_t assoc_req[] = {0x11, 0x04, 0x0a, 0x00, 0x00, 0x07, 'T',
                                        'D',  'A',  '-',  'N',  'E',  'T'};
    static const uint8_t m1[1] = {1}, m2[1] = {2}, m4[1] = {4};
    unsigned auths = ap.auths, assocs = ap.assocs, i;
    struct wlan_data_key *g;
    uint64_t start;
    int slot;

    ap.present = ap.on = 1;
    ap.m2 = ap.m4 = 0;
    ap.pn = ap.tx_pn = ap.gtx_pn = 0;
    got_auth = got_assoc = 0;
    for (i = 0; i < 5; i++)
        delivered_eapol[i] = 0;
    CHECK(bssid_is(none));

    start = model.usec;
    CHECK(registered->ops->config(registered, &card.radio.band.channels[3], 0) == 0);
    waited_since(start, "the channel change");
    CHECK(card.phy.channel.frequency == AP_FREQ);
    CHECK(station_state(WLAN_STA_AUTHENTICATING) == 0);
    CHECK(bssid_is(ap_mac));
    CHECK(station_mgmt(0x00b0, auth_req, sizeof(auth_req)) == 0);
    run_for(20);
    CHECK(ap.auths == auths + 1 && got_auth == 1);
    CHECK(station_state(WLAN_STA_AUTHENTICATED) == 0);
    CHECK(station_state(WLAN_STA_ASSOCIATING) == 0);
    CHECK(station_mgmt(0x0000, assoc_req, sizeof(assoc_req)) == 0);
    run_for(20);
    CHECK(ap.assocs == assocs + 1 && got_assoc == 1);
    the_mlme.bss = &bss;
    the_mlme.ht = 1;
    CHECK(station_state(WLAN_STA_ASSOCIATED) == 0);
    CHECK(data.connected);
    CHECK(card.ampdu.max_len == 65535);

    /* The 4-way handshake: EAPOL in the clear, both ways. */
    ap_data(0x888e, m1, 1, 0, 0, -1);
    run_for(10);
    CHECK(delivered_eapol[1] == 1);
    CHECK(station_send(0x888e, m2, 1, 7) == 0);
    run_for(10);
    CHECK(ap.m2 == 1 && delivered_eapol[3] == 1);
    CHECK(station_send(0x888e, m4, 1, 7) == 0);
    run_for(10);
    CHECK(ap.m4 == 1);

    /* NEW_KEY for the pairwise key and the group key, then the port. */
    CHECK(install_key(1) == 0);
    CHECK(install_key(0) == 0);
    slot = pairwise_slot();
    g = wlan_data_group_key(&data, 1);
    CHECK(slot >= AR_RSVD_KEYTABLE_ENTRIES && slot < AR_KEY_CACHE_SIZE);
    CHECK(g && g->key.hw_slot == 1);
    if (slot >= 0)
        CHECK(entry_holds((unsigned)slot, tk, ap_mac));
    CHECK(entry_holds(1, gtk, 0));
    CHECK(station_state(WLAN_STA_AUTHORIZED) == 0);
    CHECK(ap.bad == 0 && delivered_bad == 0);
    model_clean("the join");
}

/* Frames the station's data path took and the radio refused while a
 * phase of traffic ran, and those refused otherwise. */
static unsigned taken, refused, failed_send;

/**
 * Traffic for `ms` milliseconds: four IPv4 datagrams of 1400 bytes a
 * millisecond to the Mac, as a transfer sends them, while the AP sends two
 * of 1000 bytes under the pairwise key and, every 50 ms, one to every
 * station under the group key; then `tail` ms more for what is in flight.
 * A datagram the radio refuses with -ENOBUFS is counted and not sent again;
 * data.c's drain, which this test does not run, would hold it instead.
 */
static void traffic(unsigned ms, unsigned tail)
{
    static uint8_t ip[1400];
    unsigned i, k;
    int rc, slot = pairwise_slot();

    k_memset(ip, 0, sizeof(ip));
    ip[0] = 0x45;
    taken = refused = failed_send = 0;
    for (i = 0; i < ms; i++) {
        for (k = 0; k < 4; k++) {
            ip[8] = (uint8_t)k;
            rc = station_send(0x0800, ip, sizeof(ip), 0);
            if (rc == 0)
                taken++;
            else if (rc == -ENOBUFS)
                refused++;
            else
                failed_send++;
        }
        if (ap.on) {
            ap_data(0x0800, ip, 1000, 1, 0, slot);
            ap_data(0x0800, ip, 1000, 1, 0, slot);
            if (i % 50 == 0)
                ap_data(0x0800, ip, 200, 1, 1, slot);
        }
        run_for(1);
    }
    run_for(tail);
}

/** The trial's traffic with the AP up: everything taken is heard once,
 * encrypted with the right entry, and everything sent is delivered. */
static void clean_traffic(unsigned ms, const char *step)
{
    unsigned data0 = ap.data, ip0 = delivered_ip, sent0 = ap.sent_data;
    unsigned replays = counters.rx_replays, decrypt = counters.rx_decrypt_errors;
    unsigned dropped = counters.rx_dropped, resets = 0, i;

    for (i = 0; i < ATH9K_RESET_REASONS; i++)
        resets += card.stats.resets[i];
    traffic(ms, 300);
    CHECK(failed_send == 0);
    CHECK(taken >= 3 * ms);
    CHECK(ap.data - data0 == taken);
    CHECK(delivered_ip - ip0 == ap.sent_data - sent0);
    CHECK(ap.bad == 0 && delivered_bad == 0);
    CHECK(counters.rx_replays == replays && counters.rx_decrypt_errors == decrypt &&
          counters.rx_dropped == dropped);
    CHECK(rxq_full == 0 && statq_lost == 0 && airq_count == 0);
    for (i = 0; i < ATH9K_RESET_REASONS; i++)
        resets -= card.stats.resets[i];
    CHECK(resets == 0);
    model_clean(step);
}

/** Fill `bss` with the AP of the trial as its beacons describe it. */
static void ap_bss(void)
{
    /* SSID, rates, HT Capabilities (MCS 0 to 7, A-MPDUs of 64 KiB
     * spaced 4 us), HT Operation on channel 4, and WMM. */
    static const uint8_t ies[] = {
        /* SSID */
        0, 7, 'T', 'D', 'A', '-', 'N', 'E', 'T',
        /* Supported Rates, 1 to 11 basic, and Extended Supported Rates */
        1, 8, 0x82, 0x84, 0x8b, 0x96, 0x0c, 0x12, 0x18, 0x24, 50, 4, 0x30, 0x48, 0x60, 0x6c,
        /* HT Capabilities */
        45, 26, 0x2c, 0x01, 0x17, 0xff, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
        0, 0,
        /* HT Operation */
        61, 22, 4, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
        /* WMM Parameter */
        0xdd, 24, 0x00, 0x50, 0xf2, 0x02, 0x01, 0x01, 0x00, 0x00, 0x03, 0xa4, 0x00, 0x00, 0x27,
        0xa4, 0x00, 0x00, 0x42, 0x43, 0x5e, 0x00, 0x62, 0x32, 0x2f, 0x00};

    k_memset(&bss, 0, sizeof(bss));
    k_memcpy(bss.bssid, ap_mac, 6);
    bss.frequency = AP_FREQ;
    bss.beacon_interval = 100;
    bss.capability = WLAN_CAPABILITY_ESS | WLAN_CAPABILITY_PRIVACY;
    k_memcpy(bss.ies, ies, sizeof(ies));
    bss.ies_len = sizeof(ies);
}

/** Task 10's path on the trial boot, after the scan and its reset. */
static void connect_and_soak(void)
{
    unsigned hangs, stalls, held, i;
    int slot;

    wlan_data_init(&data, &the_wlan, registered, &counters, station_deliver, station_event);
    ap_bss();
    station_on = 1;
    join();

    /* Traffic both ways, during which the station asks for a BlockAck
     * session, aggregates, and Minstrel finds MCS 5. */
    clean_traffic(3000, "the traffic");
    CHECK(ap.addba == 1 && card.ampdu.tid[0].active);
    CHECK(card.ani.beacon_rssi == AP_BEACON_RSSI);
    CHECK(ap.aggregates > 0 && ap.subframes > 2 * ap.aggregates);
    CHECK(ap.fast > ap.data / 2);
    /* What `iw dev wlan0 station dump` reports, from the last frames:
     * MCS 3 to 5 out, which are 26 to 52 Mbit/s, and 6 Mbit/s in. */
    CHECK(data.tx_rate >= 260 && data.tx_rate <= 520);
    CHECK(data.rx_rate == 60);
    printf("ath9k_trial_hosttest: %s: %u frames out, %u in aggregates of %u, %u in, last rate "
           "0x%02x\n",
           polled ? "polled" : "messages", ap.data, ap.subframes, ap.aggregates, delivered_ip,
           ap.rate_code);

    /* A queue that stops under load: the hang check resets the chip, the
     * keys are written back, and the traffic goes on with growing PNs. */
    hangs = card.stats.resets[ATH9K_RESET_TX_HANG];
    model.stuck_tx_dma = 1;
    for (i = 0, held = 0; i < 30 && card.stats.resets[ATH9K_RESET_TX_HANG] == hangs; i++) {
        traffic(100, 0);
        CHECK(failed_send == 0);
        held += refused;
    }
    model.stuck_tx_dma = 0;
    CHECK(held > 0);
    traffic(500, 300);
    CHECK(card.stats.resets[ATH9K_RESET_TX_HANG] == hangs + 1);
    CHECK(failed_send == 0);
    slot = pairwise_slot();
    if (slot >= 0)
        CHECK(entry_holds((unsigned)slot, tk, ap_mac));
    CHECK(entry_holds(1, gtk, 0));
    CHECK(bssid_is(ap_mac));
    CHECK(model.rxe);
    model_clean("the reset under load");
    clean_traffic(1000, "the traffic after the reset");

    /* The receiver stops hearing while the AP beacons: a window of the
     * hang check without a frame a second, judged since the station is
     * associated, resets the chip, which hears again. */
    stalls = card.stats.resets[ATH9K_RESET_RX_STALL];
    rx_deaf = 1;
    for (i = 0; i < 12 && rx_deaf; i++)
        run_for(1000);
    CHECK(!rx_deaf && card.stats.resets[ATH9K_RESET_RX_STALL] == stalls + 1);
    CHECK(bssid_is(ap_mac) && model.rxe);
    model_clean("the receive stall");
    clean_traffic(1000, "the traffic after the receive stall");

    /* The AP restarts: nothing gets through for five seconds and no
     * beacon comes, so the MLME drops the connection, which clears the
     * keys and the BSSID; then the AP is back, and the station joins
     * again and carries traffic. */
    for (i = 0, held = 0; i < ATH9K_RESET_REASONS; i++)
        held += card.stats.resets[i];
    ap.on = 0;
    traffic(5000, 300);
    CHECK(taken > 0 && failed_send == 0);
    for (i = 0; i < ATH9K_RESET_REASONS; i++)
        held -= card.stats.resets[i];
    CHECK(held == 0);
    CHECK(station_state(WLAN_STA_NONE) == 0);
    CHECK(!data.connected && card.ani.beacon_rssi == 0);
    if (slot >= 0)
        CHECK(entry_holds((unsigned)slot, 0, 0));
    CHECK(entry_holds(1, 0, 0));
    for (i = 0; i < AR_KEY_CACHE_SIZE; i++)
        CHECK(card.keys.entry[i].used == ATH9K_KEY_FREE);
    CHECK(!card.ampdu.tid[0].active);
    run_for(1000);
    model_clean("the AP's restart");
    ap.addba = 0;
    join();
    clean_traffic(1000, "the traffic after the rejoin");
    CHECK(ap.addba == 1 && card.ampdu.tid[0].active);

    /* The MLME leaves before the interface goes down. */
    CHECK(station_state(WLAN_STA_NONE) == 0);
    station_on = 0;
    ap.on = 0;
    run_for(100);
    model_clean("the leave");
}

/* ------------------------------------------------------------------ */
/* The trial.                                                          */
/* ------------------------------------------------------------------ */

/** The scan of the trial boot, then a transmit queue that stops and the
 * reset that recovers it, with the radio started on channel 1. */
static void scan_and_hang(void)
{
    const struct wlan_channel *ch;
    unsigned i, before;
    uint64_t start;

    /* A scan: each channel for 50 ms with a probe request, as scan.c
     * does, then back home to channel 1. */
    for (i = 0; i < 13; i++) {
        ch = &card.radio.band.channels[i];
        start = model.usec;
        CHECK(registered->ops->config(registered, ch, WLAN_FILTER_OTHER_BSS) == 0);
        waited_since(start, "a channel change");
        CHECK(card.phy.channel.frequency == ch->frequency);
        CHECK(probe_request() == 0);
        run_for(110);
    }
    CHECK(registered->ops->config(registered, &card.radio.band.channels[0], 0) == 0);
    run_for(1000);
    CHECK(tx_statuses == 13);
    CHECK(tx_failed == 0);
    CHECK(logged("ath9k: reset") == 0);
    model_clean("the scan");

    /* A queue that stops sending: the hang check finds it and resets the
     * chip, and the next frame goes out. */
    model.stuck_tx_dma = 1;
    before = tx_statuses;
    CHECK(probe_request() == 0);
    run_for(3000);
    CHECK(logged("ath9k: reset: ") >= 1);
    CHECK(card.stats.resets[ATH9K_RESET_TX_HANG] == 1);
    CHECK(tx_statuses == before + 1 && tx_failed == 1);
    model.stuck_tx_dma = 0;
    CHECK(probe_request() == 0);
    run_for(1000);
    CHECK(tx_statuses == before + 2 && tx_failed == 1);
    CHECK(model.rxe);
    model_clean("the recovery");
}

/** The trial, once; returns the failures, at most 255. */
static int trial(void)
{
    unsigned before;
    uint64_t start;

    lenovo_model(1);
    model.hooks.dma = dma_map;
    model.hooks.read = chip_read;
    model.hooks.write = chip_write;
    card_io = &ath9k_model_io;
    card_ctx = &model;

    /* The boot: probe, registration, the radio switch's first sample. */
    start = model.usec;
    CHECK(ath9k_probe() == 0);
    waited_since(start, "the probe");
    CHECK(registered != 0);
    CHECK(logged("ath9k: probe: 02:00.0 AR9565 rev 1.0.1 mac b8:ee:65:32:89:15 subsys "
                 "17aa:4026") == 1);
    CHECK(!chip_irq_on_at_msi);
    CHECK(!(ath9k_model_peek(&model, AR_IER) & AR_IER_ENABLE));
    CHECK(serdes_writes == 1);
    CHECK(ath9k_model_peek(&model, AR_PCIE_PM_CTRL) & AR_PCIE_PM_CTRL_ENA);
    run_for(3000);
    CHECK(messages == 0);
    CHECK(rfkill_reports == 1 && rfkill_blocked == 0);
    model_clean("the boot");
    if (!registered) {
        print_log();
        return failures > 255 ? 255 : (int)failures;
    }

    /* ip link set wlan0 up: the radio's start, then three seconds of
     * beacons on channel 1. */
    start = model.usec;
    CHECK(registered->ops->start(registered) == 0);
    waited_since(start, "the start");
    CHECK(card.bus.started);
    CHECK(serdes_writes == 2);
    CHECK(model.rxe);
    CHECK(ath9k_model_peek(&model, AR_IER) & AR_IER_ENABLE);
    deliver();
    run_for(3000);
    CHECK(air_frames >= 25);
    CHECK(rx_frames == air_frames);
    CHECK(logged("ath9k: reset") == 0);
    model_clean("the start");

    scan_and_hang();

    /* Task 10's: the join, traffic, a reset under load, the AP's restart. */
    connect_and_soak();

    /* ip link set wlan0 down. */
    start = model.usec;
    registered->ops->stop(registered);
    waited_since(start, "the stop");
    CHECK(!card.bus.started);
    CHECK(serdes_writes == 3);
    CHECK(!model.rxe);
    CHECK(!(ath9k_model_peek(&model, AR_IER) & AR_IER_ENABLE));
    before = messages;
    run_for(2000);
    CHECK(messages == before);
    model_clean("the stop");

    printf("ath9k_trial_hosttest: %s: %u messages, %u frames, longest wait %lu us\n",
           polled ? "polled" : "messages", messages, rx_frames, (unsigned long)longest_wait);
    if (failures)
        print_log();
    return failures > 255 ? 255 : (int)failures;
}

int main(void)
{
    pid_t pid;
    int status, rc;

    fflush(stdout);
    pid = fork();
    if (pid < 0) {
        printf("ath9k_trial_hosttest: fork failed\n");
        return 1;
    }
    if (pid == 0) {
        polled = 1;
        exit(trial());
    }
    rc = trial();
    if (waitpid(pid, &status, 0) != pid || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        printf("ath9k_trial_hosttest: polled: failed\n");
        return rc ? rc : 1;
    }
    return rc;
}
