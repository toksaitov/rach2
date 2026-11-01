/*
 * ath9k_ant_hosttest.c: antenna, bus and suspend on the AR9003 chip model.
 *
 * host-deps: tests/host/ath9k_model.c
 * host-deps: rach2/sys/network/driver/ath9k/io.c
 * host-deps: rach2/sys/network/driver/ath9k/chips.c
 * host-deps: rach2/sys/network/driver/ath9k/antenna.c
 * host-deps: rach2/sys/network/driver/ath9k/power.c
 * host-deps: rach2/sys/cmdline.c rach2/sys/lib/kstring.c
 * host-deps: rach2/sys/lib/kctypes.c rach2/sys/proc/suspend.c
 */
#include <stdio.h>

#include "ath9k_model.h"
#include "network/wireless/wiphy.h"
#include "cmdline.h"
#include "lib/kerrno.h"
#include "lib/kstring.h"
#include "proc/suspend.h"
#include "proc/percpu.h"
#include "proc/wait_queue.h"
#include "klog.h"

static unsigned fails;
#define CHECK(c)                                                                \
    do {                                                                        \
        if (!(c)) {                                                             \
            fprintf(stderr, "ath9k_ant_hosttest: line %d: %s\n", __LINE__, #c); \
            fails++;                                                            \
        }                                                                       \
    } while (0)

static struct ath9k ah;
static struct ath9k_model model;
static struct wlan station;
struct percpu cpu_table[NR_CPUS];
static int task_a, task_b;
static unsigned reports, last_block, stops, starts, resume_errors;
static int start_error, wait_error, wake_in_stop, nested_in_stop;
static int bridge_present;
static unsigned caps[2];
static uint16_t links[2];
static unsigned pci_writes, pci_order[2];

/** Give the two functions a PCIe capability, or model its absence. */
uint8_t pci_find_capability(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t id)
{
    CHECK((bus == 0 || bus == 2) && dev == 0 && fn == 0 && id == PCI_CAP_ID_PCIE);
    return (uint8_t)caps[bus == 2];
}

/** Read a modeled link word; an unexpected access fails the row. */
uint16_t pci_read_config16(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t offset)
{
    unsigned n = bus == 2;
    CHECK((bus == 0 || bus == 2) && dev == 0 && fn == 0);
    CHECK(offset == caps[n] + PCIE_LINK_CONTROL);
    return links[n];
}

/** Record the order and width of link writes, preserving the other end. */
void pci_write_config16(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t offset, uint16_t value)
{
    unsigned n = bus == 2;
    CHECK((bus == 0 || bus == 2) && dev == 0 && fn == 0);
    CHECK(offset == caps[n] + PCIE_LINK_CONTROL);
    CHECK(pci_writes < 2);
    if (pci_writes < 2)
        pci_order[pci_writes] = n;
    pci_writes++;
    links[n] = value;
}

/** Model the upstream port on bus zero of endpoint bus two. */
bool pci_find_bridge_for_bus(uint8_t bus, pci_device_t *out)
{
    CHECK(bus == 2);
    k_memset(out, 0, sizeof(*out));
    return bridge_present != 0;
}

/** Record a hard switch report; the real station applies it on its worker. */
void wlan_radio_rfkill(struct wlan_radio *radio, int blocked)
{
    CHECK(radio == &ah.radio && radio->wlan == &station);
    reports++;
    last_block = blocked != 0;
    station.hw_blocked = last_block;
}

/** Model the station stop, including its clearing of a pending restart. */
void wlan_stop(struct wlan *wlan)
{
    CHECK(wlan == &station);
    if (wlan->started)
        stops++;
    wlan->started = wlan->resume = 0;
    if (nested_in_stop)
        CHECK(suspend_to_idle() == -EBUSY);
    if (wake_in_stop)
        suspend_wake();
}

/** Model the station start and its deferred rfkill restart. */
int wlan_start(struct wlan *wlan)
{
    CHECK(wlan == &station && !ah.power.suspended);
    if (wlan->blocked) {
        wlan->resume = 1;
        return -ERFKILL;
    }
    starts++;
    if (start_error)
        return start_error;
    wlan->started = 1;
    return 0;
}

/** Count a logged resume failure; the suspend's own result stays separate. */
void klog(int level, const char *subsystem, const char *format, ...)
{
    (void)subsystem;
    (void)format;
    if (level == KLOG_ERR)
        resume_errors++;
}

/** The modeled wait queue needs no allocation or scheduler. */
void wait_queue_init(wait_queue_t *wq)
{
    (void)wq;
}
/** A wake is kept by suspend.c's counter; there is no sleeping thread here. */
void wq_wake_all(wait_queue_t *wq)
{
    (void)wq;
}

/** Check the actual freezer with the radio stopped, then wake or signal it. */
int wq_wait_event(wait_queue_t *wq, wq_cond_fn cond, void *arg)
{
    (void)wq;
    CHECK(ah.power.suspended && !station.started);
    CHECK(suspend_lets_run((struct process *)&task_a));
    CHECK(!suspend_lets_run((struct process *)&task_b));
    CHECK(suspend_to_idle() == -EBUSY);
    CHECK(!!cond(arg) == !!wake_in_stop);
    if (wait_error)
        return wait_error;
    suspend_wake();
    CHECK(cond(arg));
    return 0;
}

/** Make one Lenovo card and fresh PCIe and station models for a row. */
static void fresh(void)
{
    k_memset(&ah, 0, sizeof(ah));
    k_memset(&station, 0, sizeof(station));
    ah.hw.chip = ath9k_chip_find(0x0036);
    CHECK(ah.hw.chip != 0);
    ah.bus.flags = ath9k_chip_flags(ah.hw.chip, 0x17aa, 0x4026);
    ah.bus.dev.bus = 2;
    ah.radio.wlan = &station;
    station.radio = &ah.radio;
    ath9k_model_init(&model, ah.hw.chip, 2);
    ath9k_model_attach(&model, &ah);
    caps[0] = caps[1] = 0x40;
    links[0] = 0x1233;
    links[1] = 0x4563;
    bridge_present = 1;
    pci_writes = reports = stops = starts = resume_errors = 0;
    start_error = wait_error = wake_in_stop = nested_in_stop = 0;
    kcmdline_capture(0);
    cpu_table[0].current = (struct process *)&task_a;
}

/** Check the full-width switch fields, preserved bits and WLAN ownership. */
static void switch_tables(void)
{
    uint32_t gain;
    fresh();
    ah.eeprom.image.modalHeader2G.antCtrlCommon = 0x123456;
    ah.eeprom.image.modalHeader2G.antCtrlCommon2 = 0xabcdef;
    ah.eeprom.image.modalHeader2G.antCtrlChain[0] = 0x789;
    ah.eeprom.image.modalHeader2G.switchcomspdt = 0xabc;
    ath9k_model_poke(&model, AR_PHY_SWITCH_COM, 0xa5000000);
    ath9k_model_poke(&model, AR_PHY_SWITCH_COM_2, 0x5a000000);
    ath9k_model_poke(&model, AR_PHY_SWITCH_CHAIN_0, 0xabcd0000);
    ath9k_model_poke(&model, AR_PHY_SWITCH_CHAIN_1, 0x12345678);
    ath9k_model_poke(&model, AR_PHY_GLB_CONTROL, 0xabcd0002);
    ath9k_model_poke(&model, AR_BTCOEX_CTRL, 0xffffffff);
    ath9k_model_poke(&model, AR_MCI_INTERRUPT_EN, 0xffffffff);
    ath9k_model_poke(&model, AR_MCI_INTERRUPT_RX_MSG_EN, 0xffffffff);
    ath9k_model_poke(&model, AR_PHY_MC_GAIN_CTRL, 0x80000155);
    ath9k_ant_init(&ah);
    CHECK(ah.ant.diversity && ah.ant.combining);
    ath9k_ant_set(&ah);
    CHECK(ath9k_model_peek(&model, AR_PHY_SWITCH_COM) == 0xa5123456);
    CHECK(ath9k_model_peek(&model, AR_PHY_SWITCH_COM_2) == 0x5aabcdef);
    CHECK(ath9k_model_peek(&model, AR_PHY_SWITCH_CHAIN_0) == 0xabcd0789);
    CHECK(ath9k_model_peek(&model, AR_PHY_SWITCH_CHAIN_1) == 0x12345678);
    CHECK(ath9k_model_peek(&model, AR_PHY_GLB_CONTROL) == 0xabcdabc1);
    CHECK(ath9k_model_peek(&model, AR_BTCOEX_CTRL) == 0);
    CHECK(ath9k_model_peek(&model, AR_MCI_INTERRUPT_EN) == 0);
    CHECK(ath9k_model_peek(&model, AR_MCI_INTERRUPT_RX_MSG_EN) == 0);
    gain = ath9k_model_peek(&model, AR_PHY_MC_GAIN_CTRL);
    CHECK((gain & 0x800001ff) == 0x80000155);
    CHECK(MS(gain, AR_PHY_ANT_DIV_MAIN_LNACONF) == 2);
    CHECK(MS(gain, AR_PHY_ANT_DIV_ALT_LNACONF) == 1);
    CHECK(MS(gain, AR_PHY_ANT_FAST_DIV_BIAS) == 3);
    CHECK(gain & AR_ANT_DIV_ENABLE);
    CHECK(gain & AR_PHY_ANT_SW_RX_PROT);
    CHECK(ath9k_model_peek(&model, AR_PHY_CCK_DETECT) & AR_FAST_DIV_ENABLE);
    CHECK(ath9k_model_peek(&model, AR_PHY_RESTART) & AR_PHY_RESTART_ENABLE_DIV_M2FLAG);
    CHECK(ath9k_model_peek(&model, AR_BTCOEX_WL_LNADIV) & AR_BTCOEX_WL_LNADIV_FORCE_ON);
    CHECK(!model.unmapped);
    CHECK(ath9k_model_find(&model, 0, ATH9K_MODEL_WRITE, AR_MCI_INTERRUPT_EN) <
          ath9k_model_find(&model, 0, ATH9K_MODEL_WRITE, AR_PHY_GLB_CONTROL));
    ah.bus.flags = ATH9K_PCI_1ANT;
    ath9k_ant_set(&ah);
    CHECK(!ah.ant.diversity && !ah.ant.combining);
    CHECK(!(ath9k_model_peek(&model, AR_PHY_MC_GAIN_CTRL) & AR_ANT_DIV_ENABLE));
    CHECK(!(ath9k_model_peek(&model, AR_PHY_CCK_DETECT) & AR_FAST_DIV_ENABLE));
    CHECK(!(ath9k_model_peek(&model, AR_PHY_RESTART) & AR_PHY_RESTART_ENABLE_DIV_M2FLAG));
    CHECK(!(ath9k_model_peek(&model, AR_BTCOEX_WL_LNADIV) & AR_BTCOEX_WL_LNADIV_FORCE_ON));
    ah.bus.flags |= ATH9K_PCI_BT_ANT_DIV;
    ath9k_ant_set(&ah);
    CHECK(ah.ant.diversity && ah.ant.combining);
}

/** Build receive metadata independently from the chip's documented fields. */
static struct ar9003_rxs sample(unsigned main, unsigned alt, unsigned conf, int more)
{
    struct ar9003_rxs rx;
    k_memset(&rx, 0, sizeof(rx));
    rx.status1 = main | (alt << 8) | (conf << 16);
    rx.status11 = 3 | (more ? 0x10000 : 0);
    return rx;
}

/** Feed one window from a stable signal for each physical or combined LNA. */
static void window(const unsigned signal[4])
{
    unsigned i, main = ah.ant.main_conf, alt = ah.ant.alt_conf;
    struct ar9003_rxs rx = sample(signal[main], signal[alt], main << 4 | main << 2 | alt, 0);
    for (i = 0; i < 512; i++)
        ath9k_ant_rx(&ah, &rx);
}

/** Select LNA2 and A+B, ignore bad metadata and bound a long aggregate. */
static void combining(void)
{
    const unsigned signal[4] = {15, 40, 20, 50};
    const unsigned changed[4] = {60, 20, 45, 25};
    struct ar9003_rxs rx;
    unsigned i;
    fresh();
    ath9k_ant_set(&ah);
    rx = sample(20, 40, 0x29, 0);
    rx.status11 = 1;
    ath9k_ant_rx(&ah, &rx);
    CHECK(!ah.ant.total);
    rx = sample(20, 128, 0x29, 0);
    ath9k_ant_rx(&ah, &rx);
    CHECK(!ah.ant.total);
    rx = sample(20, 40, 0x16, 0);
    ath9k_ant_rx(&ah, &rx);
    CHECK(!ah.ant.total);
    rx = sample(20, 40, 0x29, 1);
    for (i = 0; i < 2048; i++)
        ath9k_ant_rx(&ah, &rx);
    CHECK(ah.ant.total == 512 && ah.ant.alt_conf == 1);
    rx.status11 = 3;
    ath9k_ant_rx(&ah, &rx);
    CHECK(ah.ant.alt_conf == 3 && !ah.ant.total);
    window(signal);
    CHECK(ah.ant.alt_conf == 0);
    window(signal);
    CHECK(ah.ant.main_conf == 1 && ah.ant.alt_conf == 3 && !ah.ant.scanning);
    CHECK(MS(ath9k_model_peek(&model, AR_PHY_MC_GAIN_CTRL), AR_PHY_ANT_FAST_DIV_BIAS) == 2);
    for (i = 0; i < 16; i++)
        window(changed);
    CHECK(ah.ant.scanning && ah.ant.alt_conf == 2);
    window(changed);
    window(changed);
    window(changed);
    CHECK(ah.ant.main_conf == 2 && ah.ant.alt_conf == 0);
    ah.power.suspended = 1;
    window(signal);
    CHECK(!ah.ant.total);
    ah.power.suspended = 0;
    ath9k_ant_set(&ah);
    CHECK(ah.ant.main_conf == 2 && ah.ant.alt_conf == 1 && ah.ant.scanning);
    CHECK(!model.unmapped && !model.log_lost);
}

/** Leave all firmware ASPM states alone, then disable both link ends only. */
static void aspm(void)
{
    unsigned i;
    for (i = 0; i < 4; i++) {
        fresh();
        links[0] = 0x1230 | i;
        links[1] = 0x4560 | i;
        ath9k_power_init(&ah);
        CHECK(!pci_writes && ah.power.aspm == i);
        CHECK(links[0] == (0x1230 | i) && links[1] == (0x4560 | i));
        kcmdline_capture("rach2.ath9k_noaspm=0 norach2.ath9k_noaspm");
        ath9k_power_init(&ah);
        CHECK(!pci_writes);
        kcmdline_capture("rach2.ath9k_noaspm");
        ath9k_power_init(&ah);
        CHECK(links[0] == 0x1230 && links[1] == 0x4560);
        CHECK(pci_writes == (i ? 2u : 0u));
        if (i)
            CHECK(pci_order[0] == 0 && pci_order[1] == 1);
        CHECK(ah.power.aspm == i);
    }
    fresh();
    ah.options = ATH9K_OPT_NO_ASPM;
    caps[0] = caps[1] = 0;
    ath9k_power_init(&ah);
    CHECK(!pci_writes);
    fresh();
    ah.options = ATH9K_OPT_NO_ASPM;
    caps[0] = caps[1] = 0xf0;
    ath9k_power_init(&ah);
    CHECK(!pci_writes);
    fresh();
    ah.options = ATH9K_OPT_NO_ASPM;
    links[0] = links[1] = 0xffff;
    ath9k_power_init(&ah);
    CHECK(!pci_writes);
    fresh();
    ah.options = ATH9K_OPT_NO_ASPM;
    caps[1] = 0xec;
    bridge_present = 0;
    ath9k_power_init(&ah);
    CHECK(pci_writes == 1 && links[1] == 0x4560 && links[0] == 0x1233);
}

/** Decode polarity and the whole GPIO field; report the initial state once. */
static void rf_silence(void)
{
    unsigned gpio, polarity;
    for (gpio = 0; gpio <= 13; gpio += 13) {
        for (polarity = 0; polarity < 2; polarity++) {
            fresh();
            ah.eeprom.image.baseEepHeader.rfSilent = 1 | polarity << 1 | gpio << 2;
            ath9k_model_poke(&model, AR_GPIO_OE_OUT, 0xffffffff);
            ath9k_model_poke(&model, AR_GPIO_IN, polarity << gpio);
            ah.radio.wlan = 0;
            ath9k_power_init(&ah);
            CHECK(!reports && ah.power.hw_blocked);
            CHECK(ath9k_model_peek(&model, AR_GPIO_OE_OUT) == (0xffffffffu & ~(3u << (gpio * 2))));
            ah.radio.wlan = &station;
            ath9k_rfkill_poll(&ah);
            CHECK(reports == 1 && last_block);
            ath9k_rfkill_poll(&ah);
            CHECK(reports == 1);
            ath9k_model_poke(&model, AR_GPIO_IN, (1u - polarity) << gpio);
            ath9k_rfkill_poll(&ah);
            CHECK(reports == 2 && !last_block);
            model.dead = 1;
            ath9k_rfkill_poll(&ah);
            CHECK(reports == 3 && last_block);
        }
    }
    fresh();
    ah.eeprom.image.baseEepHeader.rfSilent = 0xfd;
    ath9k_power_init(&ah);
    CHECK(reports == 1 && last_block && model.log_count == 0);
    fresh();
    ath9k_model_poke(&model, AR_GPIO_OE_OUT, 0xabcdef);
    ath9k_power_init(&ah);
    CHECK(reports == 1 && !last_block && model.log_count == 0);
    CHECK(ath9k_model_peek(&model, AR_GPIO_OE_OUT) == 0xabcdef);
}

/** Exercise the actual freezer, wake during preparation, signal and retry. */
static void suspend_radio(void)
{
    fresh();
    ath9k_power_init(&ah);
    station.started = 1;
    CHECK(suspend_to_idle() == 0);
    CHECK(stops == 1 && starts == 1 && station.started && !ah.power.suspended);
    CHECK(suspend_lets_run((struct process *)&task_b));
    nested_in_stop = wake_in_stop = 1;
    CHECK(suspend_to_idle() == 0);
    CHECK(stops == 2 && starts == 2);
    nested_in_stop = wake_in_stop = 0;
    wait_error = -EINTR;
    CHECK(suspend_to_idle() == -EINTR);
    CHECK(stops == 3 && starts == 3 && station.started);
    wait_error = 0;
    start_error = -EIO;
    CHECK(suspend_to_idle() == 0);
    CHECK(!station.started && !ah.power.suspended && ah.power.resume);
    CHECK(resume_errors == 1);
    start_error = 0;
    CHECK(ath9k_resume_all() == 0 && station.started);
    CHECK(!ah.power.resume);
    station.started = 0;
    starts = 0;
    CHECK(suspend_to_idle() == 0);
    CHECK(!starts && !station.started);
    station.started = 1;
    ath9k_suspend(&ah);
    ath9k_suspend(&ah);
    CHECK(ah.power.suspended && !station.started);
    station.soft_blocked = 1;
    CHECK(ath9k_resume(&ah) == 0);
    CHECK(!starts && !station.started && station.resume);
    station.soft_blocked = 0;
    station.blocked = 1;
    ath9k_suspend(&ah);
    CHECK(ath9k_resume(&ah) == 0 && station.resume && !starts);
    station.blocked = 0;
    ah.eeprom.image.baseEepHeader.rfSilent = 3;
    ath9k_power_init(&ah);
    station.started = 1;
    ath9k_suspend(&ah);
    ath9k_model_poke(&model, AR_GPIO_IN, 1);
    ath9k_rfkill_poll(&ah);
    CHECK(!ah.power.hw_blocked);
    CHECK(ath9k_resume(&ah) == 0 && ah.power.hw_blocked);
    CHECK(!starts && station.resume);
}

/** Run the one part's checks; failures return nonzero to the host runner. */
int main(void)
{
    CHECK(ath9k_resume_all() == 0);
    ath9k_suspend_all();
    switch_tables();
    combining();
    aspm();
    rf_silence();
    suspend_radio();
    if (!fails)
        printf("ath9k_ant_hosttest: ok\n");
    return fails != 0;
}
