/**
 * Run the whole ath9k driver, every part of sys/network/driver/ath9k/,
 * through the path a trial boot on the Lenovo takes, against the chip
 * model made the Lenovo's AR9565: the probe at boot, `ip link set wlan0
 * up`, interrupts by message and the timer tick while frames arrive, a
 * scan across channels 1 to 13 with a probe request on each, a transmit
 * queue that stops and the reset that recovers it, and `ip link set wlan0
 * down`. It runs twice, in two processes: with the card's messages, and
 * polled from the tick as `rach2.ath9k_nomsi` leaves it.
 *
 * host-deps: tests/host/ath9k_model.c rach2/sys/network/driver/ath9k/hw.c
 * host-deps: rach2/sys/network/driver/ath9k/eeprom.c rach2/sys/network/driver/ath9k/chips.c
 * host-deps: rach2/sys/network/driver/ath9k/io.c rach2/sys/network/driver/ath9k/phy.c
 * host-deps: rach2/sys/network/driver/ath9k/calib.c rach2/sys/network/driver/ath9k/ani.c
 * host-deps: rach2/sys/network/driver/ath9k/mac.c rach2/sys/network/driver/ath9k/recv.c
 * host-deps: rach2/sys/network/driver/ath9k/xmit.c rach2/sys/network/driver/ath9k/key.c
 * host-deps: rach2/sys/network/driver/ath9k/intr.c rach2/sys/network/driver/ath9k/recover.c
 * host-deps: rach2/sys/network/driver/ath9k/antenna.c rach2/sys/network/driver/ath9k/power.c
 * host-deps: rach2/sys/network/driver/ath9k/ampdu.c
 * host-deps: rach2/sys/lib/kstring.c rach2/sys/lib/kctypes.c
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
#include "network/wireless/rc_minstrel.h"
#include "network/wireless/wiphy.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

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
static unsigned rx_frames, tx_statuses, tx_failed, rfkill_reports;
static int rfkill_blocked = -1;

int wlan_register_radio(struct wlan_radio *radio, struct wlan **out)
{
    registered = radio;
    the_wlan.radio = radio;
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

void wlan_minstrel_init(struct wlan_radio *radio, struct wlan_minstrel *state)
{
    (void)radio;
    (void)state;
}

int wlan_radio_rx(struct wlan_radio *radio, const uint8_t *frame, unsigned len,
                  const struct wlan_rx_status *status)
{
    (void)radio;
    (void)frame;
    (void)len;
    (void)status;
    rx_frames++;
    return 0;
}

void wlan_radio_tx_status(struct wlan_radio *radio, const struct wlan_tx_status *status)
{
    (void)radio;
    tx_statuses++;
    if (!(status->flags & WLAN_TX_ACKED))
        tx_failed++;
}

void wlan_radio_rfkill(struct wlan_radio *radio, int blocked)
{
    (void)radio;
    rfkill_reports++;
    rfkill_blocked = blocked;
}

/* The worker: queued work, run in order of its due time. */
#define MAX_WORK 16
static struct wlan_work *queued[MAX_WORK];

int wlan_queue_work(struct wlan_work *work, uint64_t due)
{
    unsigned i;

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

/* The radio switch: the card's EEPROM gives RF silence on GPIO 11, active
 * low (rfSilent 0x2d), and the machine's reads it unblocked. */
static int radio_switch_on = 1;

/** Answer AR_INTR_SYNC_CAUSE and the radio switch's GPIO; grant the radio bus while it is asked
 * for, and turn the peak detector's output at PEAK_DAC, as the hardware does. */
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
        w->run(w);
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

    if (!model.rxe || card.phy.channel.number != number)
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

/** Let `ms` milliseconds pass: each one a tick, the work due and the
 * messages the chip sends, with a beacon every 100 ms on channel 1, 6 and
 * 11, and the queued frames sent after a millisecond. */
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
        if (!model.stuck_tx_dma)
            for (q = 0; q < AR_NUM_QCU; q++)
                while (model.tx[q].count)
                    (void)ath9k_model_tx_complete(&model, q, NULL);
        deliver();
        ath9k_poll();
        waited_since(start, "the tick");
        run_work();
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
/* The trial.                                                          */
/* ------------------------------------------------------------------ */

/** The trial, once; returns the failures, at most 255. */
static int trial(void)
{
    const struct wlan_channel *ch;
    unsigned i, before;
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
