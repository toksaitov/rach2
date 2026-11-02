/*
 * pci.c: the bus side of the ath9k driver: finding the card, mapping its
 * window, the register access the kernel gives the other parts, and the
 * radio operations through which the station layer runs the card.
 *
 * The probe follows ath9k's pci.c (ath_pci_probe and ath_pci_id_table,
 * Linux v6.16) and init.c's ath9k_init_device; the operations follow
 * main.c's ath9k_start, ath9k_stop and ath9k_config in what they ask of
 * the chip. The card is one, kept here; a host test includes this file and
 * points `card_io` at its chip model.
 */

#include "ath9k.h"
#include "../../../klog.h"
#include "../../../timer.h"
#include "../../../udelay.h"
#include "../../../mm/paging.h"
#include "../../../lib/kdef.h"
#include "../../../lib/kerrno.h"
#include "../../../lib/kstring.h"
#include "../../wireless/rc_minstrel.h"
#include "../../wireless/wiphy.h"

/* The bytes of the card's window the driver maps, the whole 512 KiB BAR as
 * ath9k maps it: the AR9565's global registers, AR_GLB_SWREG_DISCONT_MODE
 * and AR_PHY_GLB_CONTROL, lie past 128 KiB. */
#define ATH9K_WINDOW 0x80000

/* How often the radio switch is read, in milliseconds. */
#define RFKILL_POLL_MS 1000

/* The subsystem IDs' offsets in PCI configuration space. */
#define PCI_SUBSYS_VENDOR 0x2c
#define PCI_SUBSYS_ID     0x2e

static struct ath9k card;
static struct wlan_minstrel card_minstrel;
static int card_registered; /* the probe registered the card's radio */

/** io.h's read for the kernel: a load from the window. */
static uint32_t mmio_read(void *ctx, uint32_t reg)
{
    const struct ath9k *ah = ctx;

    return mmio_read32(ah->bus.mmio + reg);
}

/** io.h's write for the kernel: a store to the window. */
static void mmio_write(void *ctx, uint32_t reg, uint32_t value)
{
    const struct ath9k *ah = ctx;

    mmio_write32(ah->bus.mmio + reg, value);
}

/** io.h's wait for the kernel. */
static void mmio_udelay(void *ctx, uint32_t usec)
{
    (void)ctx;
    k_udelay(usec);
}

const struct ath9k_io ath9k_mmio_io = {mmio_read, mmio_write, mmio_udelay};

/* The access the probe gives the card, and its context, NULL meaning the
 * card itself; a host test replaces both with its model. */
static const struct ath9k_io *card_io = &ath9k_mmio_io;
static void *card_ctx;

/* ------------------------------------------------------------------ */
/* The radio's operations.                                             */
/* ------------------------------------------------------------------ */

/** The channel to tune at start: the last one, or channel 1. */
static const struct wlan_channel *start_channel(const struct ath9k *ah)
{
    return ah->phy.channel.frequency ? &ah->phy.channel : &ah->radio.band.channels[0];
}

/**
 * Put the chip on `chan` with receive running, as ath9k_start and the
 * reset of a channel change do: reset onto the channel, write back the
 * keys, set the filter, start receive, arm the baseband watchdog, which
 * the tables turn off and ath9k_hw_reset arms again, and enable
 * interrupts.
 */
static int bring_up(struct ath9k *ah, const struct wlan_channel *chan)
{
    int rc = ath9k_hw_reset(ah, chan);

    if (rc != 0)
        return rc;
    ath9k_key_restore(ah);
    ath9k_mac_set_filter(ah, ah->rx.filters);
    rc = ath9k_rx_start(ah);
    if (rc != 0)
        return rc;
    ath9k_recover_watchdog(ah);
    ath9k_mac_intr_enable(ah);
    return 0;
}

/** Stop interrupts and DMA, failing every frame accepted for transmit, as
 * ath9k_stop and ath_reset_internal do before a reset. */
static void quiesce(struct ath9k *ah)
{
    ath9k_mac_intr_disable(ah);
    ath9k_mac_abort_tx(ah);
    ath9k_rx_stop(ah);
    ath9k_tx_drain(ah);
}

/** radio.h's stop: the periodic work off, DMA stopped, accepted frames
 * failed, and the chip in full sleep. Safe twice and after a failed start. */
static void radio_stop(struct wlan_radio *radio)
{
    struct ath9k *ah = radio->priv;

    if (!ah->bus.started)
        return;
    /* Interrupts off first, so that no message or tick queues a reset
     * after the checks are cancelled. */
    ath9k_mac_intr_disable(ah);
    ah->bus.started = 0;
    /* wiphy.c's stop drops the association without a call to sta_state. */
    ah->mac.associated = 0;
    ath9k_recover_stop(ah);
    ath9k_cal_stop(ah);
    quiesce(ah);
    ath9k_hw_disable(ah);
}

/** radio.h's start: bring the chip up on its last channel and start the
 * calibration and the hang checks; on failure leave it stopped. A start
 * while power.c holds the card suspended is refused with -EBUSY; one of a
 * started radio whose last reset failed tries the reset again. */
static int radio_start(struct wlan_radio *radio)
{
    struct ath9k *ah = radio->priv;
    int rc;

    if (ah->bus.started)
        return ah->recover.failures ? ath9k_reset(ah, ATH9K_RESET_CHANNEL) : 0;
    if (ah->power.suspended)
        return -EBUSY;
    /* ath9k_start wakes the chip and sets the link's power save before
     * its reset. */
    if (ath9k_hw_set_power(ah, ATH9K_PM_AWAKE) != 0) {
        klog(KLOG_ERR, "ath9k", "start: failed: the chip does not wake");
        return -EIO;
    }
    ath9k_hw_pcie_powersave(ah, 0);
    rc = bring_up(ah, start_channel(ah));
    if (rc != 0) {
        klog(KLOG_ERR, "ath9k", "start: failed: %d", rc);
        quiesce(ah);
        ath9k_hw_disable(ah);
        return rc;
    }
    ah->bus.started = 1;
    ath9k_cal_start(ah);
    ath9k_recover_start(ah);
    return 0;
}

/**
 * radio.h's config: a new channel is a full reset onto it with the
 * receive path stopped and started around it, which fails the frames
 * queued for transmit; a new filter alone is written as it is. A stopped
 * radio keeps both for its start. A reset that fails leaves the chip off
 * and counts in recover.failures, as one of recover.c's does, so that the
 * hang check tries it again and so does the next config, whatever its
 * channel; one that succeeds clears the count.
 */
static int radio_config(struct wlan_radio *radio, const struct wlan_channel *channel,
                        unsigned filters)
{
    struct ath9k *ah = radio->priv;
    int rc;

    ah->rx.filters = filters;
    if (!ah->bus.started) {
        ah->phy.channel = *channel;
        return 0;
    }
    if (channel->frequency == ah->phy.channel.frequency && !ah->recover.failures) {
        ath9k_mac_set_filter(ah, filters);
        return 0;
    }
    quiesce(ah);
    rc = bring_up(ah, channel);
    if (rc == 0) {
        ah->recover.failures = 0;
        return 0;
    }
    klog(KLOG_ERR, "ath9k", "config: failed: %u MHz: %d", (unsigned)channel->frequency, rc);
    if (ah->recover.failures < 0xff)
        ah->recover.failures++;
    ah->recover.reason = ATH9K_RESET_CHANNEL;
    return rc;
}

/** radio.h's tx. */
static int radio_tx(struct wlan_radio *radio, const uint8_t *frame, unsigned len,
                    const struct wlan_tx_info *info)
{
    struct ath9k *ah = radio->priv;

    if (!ah->bus.started)
        return -ENETDOWN;
    return ath9k_tx(ah, frame, len, info);
}

/** radio.h's set_key: install into a free entry of the cache, or clear
 * the entry an install gave. */
static int radio_set_key(struct wlan_radio *radio, int enable, const uint8_t peer[6],
                         const struct wlan_key *key, int *slot)
{
    struct ath9k *ah = radio->priv;

    if (enable)
        return ah->bus.started ? ath9k_key_set(ah, peer, key, slot) : -ENETDOWN;
    ath9k_key_clear(ah, *slot);
    return 0;
}

/**
 * radio.h's sta_state: the BSSID from the first authentication to the
 * return to NONE, as mac80211's BSS change gives it to ath9k; whether the
 * station is associated, which narrows the beacon filter to the BSS's and
 * lets recover.c judge the receive path, from a window that starts at the
 * association; and the AP's A-MPDU limits from association, as ath9k's
 * sta_add takes them. The station layer gives no association ID here, so
 * the chip's stays 0, which only a station in power save would need.
 */
static int radio_sta_state(struct wlan_radio *radio, const struct wlan_bss *bss,
                           enum wlan_sta_state old_state, enum wlan_sta_state new_state)
{
    static const uint8_t none[6] = {0, 0, 0, 0, 0, 0};
    struct ath9k *ah = radio->priv;
    uint8_t associated = new_state >= WLAN_STA_ASSOCIATED;
    int changed = associated != ah->mac.associated;

    if (associated && changed) {
        ah->recover.window = tick_count();
        ah->recover.rx_active = 0;
    }
    ah->mac.associated = associated;
    if (!ah->bus.started) {
        /* The chip sleeps; the next start's reset writes what is kept. */
        if (new_state == WLAN_STA_NONE)
            k_memcpy(ah->mac.bssid, none, 6);
        else if (old_state == WLAN_STA_NONE && bss)
            k_memcpy(ah->mac.bssid, bss->bssid, 6);
    } else if (new_state == WLAN_STA_NONE) {
        ath9k_mac_set_bssid(ah, none, 0);
    } else if (old_state == WLAN_STA_NONE && bss) {
        ath9k_mac_set_bssid(ah, bss->bssid, 0);
    } else if (changed) {
        ath9k_mac_set_filter(ah, ah->rx.filters);
    }
    if (new_state == WLAN_STA_NONE)
        ath9k_ampdu_sta(ah, NULL);
    if (new_state == WLAN_STA_ASSOCIATED && bss)
        ath9k_ampdu_sta(ah, bss);
    return 0;
}

/** radio.h's ampdu_action. */
static int radio_ampdu_action(struct wlan_radio *radio, enum wlan_ampdu_action action,
                              const uint8_t peer[6], unsigned tid, unsigned ssn, unsigned window)
{
    return ath9k_ampdu_action(radio->priv, action, peer, tid, ssn, window);
}

/** radio.h's get_tsf. */
static int radio_get_tsf(struct wlan_radio *radio, uint64_t *tsf)
{
    struct ath9k *ah = radio->priv;

    if (!ah->bus.started)
        return -ENETDOWN;
    *tsf = ath9k_mac_get_tsf(ah);
    return 0;
}

/* Scanning is the station layer's, in software. */
const struct wlan_radio_ops ath9k_radio_ops = {
    radio_start,     radio_stop,         radio_config, radio_tx, radio_set_key,
    radio_sta_state, radio_ampdu_action, NULL,         NULL,     radio_get_tsf};

/* ------------------------------------------------------------------ */
/* The probe.                                                          */
/* ------------------------------------------------------------------ */

/** Queue the switch's next poll on the record not queued last. */
static void rfkill_queue(struct ath9k *ah, uint64_t due)
{
    struct wlan_work *w = &ah->bus.rfkill_work[ah->bus.rfkill_next];

    ah->bus.rfkill_next ^= 1;
    (void)wlan_queue_work(w, due);
}

/** The switch's poll, on the worker: read it, as power.c asks of the bus
 * side whether the radio runs or not, and queue the next. */
static void rfkill_run(struct wlan_work *work)
{
    struct ath9k *ah = work->wlan->radio->priv;

    ath9k_rfkill_poll(ah);
    rfkill_queue(ah, tick_count() + RFKILL_POLL_MS);
}

/** Find the first card of a chip of the table, and its row. */
static const struct ath9k_chip *find_card(pci_device_t *dev)
{
    unsigned i;

    for (i = 0; ath9k_chips[i]; i++)
        if (pci_find_device_by_id(ATH9K_VENDOR_ID, ath9k_chips[i]->device, dev))
            return ath9k_chips[i];
    return NULL;
}

/**
 * Map the card's first memory BAR and set up its configuration as
 * ath_pci_probe does: memory decoding and bus mastering on, the cache line
 * size if the firmware left none and the latency timer at 0xa8. ath9k also
 * clears a PCI card's retry timeout at 0x41; on the family's PCIe cards that
 * byte is the read-only header of the power management capability, so it
 * is left alone. DMA is 32-bit: the parts give the card addresses below
 * 4 GiB. Returns 0, or -ENODEV for a window that is missing or will not
 * map, which it logs.
 */
static int map_card(struct ath9k *ah)
{
    pci_device_t *dev = &ah->bus.dev;
    uint64_t addr;

    if (!pci_bar_is_memory(dev->bar[0]) || (addr = pci_bar_address64(dev, 0)) == 0) {
        klog(KLOG_ERR, "ath9k", "probe: failed: %02x:%02x.%x: BAR 0 is no memory window", dev->bus,
             dev->device, dev->function);
        return -ENODEV;
    }
    if ((uint64_t)(uintptr_t)addr != addr ||
        (ah->bus.mmio = map_mmio((uintptr_t)addr, ATH9K_WINDOW)) == 0) {
        klog(KLOG_ERR, "ath9k", "probe: failed: %02x:%02x.%x: the window at 0x%llx will not map",
             dev->bus, dev->device, dev->function, (unsigned long long)addr);
        return -ENODEV;
    }
    pci_enable_memory_space(dev);
    if (pci_read_config8(dev->bus, dev->device, dev->function, PCI_CACHE_LINE_SIZE) == 0)
        pci_write_config8(dev->bus, dev->device, dev->function, PCI_CACHE_LINE_SIZE, 64 / 4);
    pci_write_config8(dev->bus, dev->device, dev->function, PCI_LATENCY_TIMER, 0xa8);
    pci_enable_bus_master(dev);
    return 0;
}

/**
 * Undo a probe that failed after the chip attached: stop the chip, then
 * give back the transmit and receive pages that were taken, so that a
 * failed probe keeps no DMA memory. The interrupt is the caller's.
 */
static void probe_unwind(struct ath9k *ah)
{
    ath9k_hw_disable(ah);
    ath9k_tx_free(ah);
    ath9k_rx_free(ah);
}

/**
 * Bring up the card of `chip` at `dev` and register its radio, as
 * ath9k_init_device does: attach the chip, then the parts, then the rate
 * control, log the boot line and register. The chip sleeps until the
 * radio starts. Returns 0 or the failing step's errno, logged.
 */
static int probe_card(struct ath9k *ah, const struct ath9k_chip *chip, const pci_device_t *dev)
{
    const pci_device_t *d = &ah->bus.dev;
    const uint8_t *m;
    struct wlan *wlan;
    int rc;

    k_memset(ah, 0, sizeof(*ah));
    ah->io = card_io;
    ah->io_ctx = card_ctx ? card_ctx : ah;
    ah->bus.dev = *dev;
    ah->bus.subsys_vendor = pci_read_config16(d->bus, d->device, d->function, PCI_SUBSYS_VENDOR);
    ah->bus.subsys_device = pci_read_config16(d->bus, d->device, d->function, PCI_SUBSYS_ID);
    ah->bus.flags = ath9k_chip_flags(chip, ah->bus.subsys_vendor, ah->bus.subsys_device);
    ah->hw.chip = chip;
    rc = map_card(ah);
    if (rc != 0)
        return rc;
    rc = ath9k_hw_attach(ah);
    if (rc != 0) {
        klog(KLOG_ERR, "ath9k", "probe: failed: %02x:%02x.%x: attach: %d", d->bus, d->device,
             d->function, rc);
        return rc;
    }
    ath9k_power_init(ah);
    ath9k_ant_init(ah);
    rc = ath9k_rx_init(ah);
    if (rc == 0)
        rc = ath9k_tx_init(ah);
    if (rc == 0)
        rc = ath9k_intr_attach(ah);
    if (rc != 0) {
        klog(KLOG_ERR, "ath9k", "probe: failed: %02x:%02x.%x: %d", d->bus, d->device, d->function,
             rc);
        probe_unwind(ah);
        return rc;
    }
    ah->radio.ops = &ath9k_radio_ops;
    ah->radio.priv = ah;
    wlan_minstrel_init(&ah->radio, &card_minstrel);
    m = ah->radio.mac;
    klog(KLOG_INFO, "ath9k",
         "probe: %02x:%02x.%x %s rev %s mac %02x:%02x:%02x:%02x:%02x:%02x subsys %04x:%04x", d->bus,
         d->device, d->function, chip->name, ah->hw.rev->name, m[0], m[1], m[2], m[3], m[4], m[5],
         ah->bus.subsys_vendor, ah->bus.subsys_device);
    ath9k_hw_set_power(ah, ATH9K_PM_FULL_SLEEP);
    rc = wlan_register_radio(&ah->radio, &wlan);
    if (rc != 0) {
        klog(KLOG_ERR, "ath9k", "probe: failed: register: %d", rc);
        ath9k_intr_detach(ah);
        probe_unwind(ah);
        return rc;
    }
    ah->bus.rfkill_work[0].wlan = ah->bus.rfkill_work[1].wlan = wlan;
    ah->bus.rfkill_work[0].run = ah->bus.rfkill_work[1].run = rfkill_run;
    ath9k_rfkill_poll(ah);
    rfkill_queue(ah, tick_count() + RFKILL_POLL_MS);
    return 0;
}

/**
 * Find a card of a chip of the table and bring it up as a radio. Boot,
 * process context. Returns 0, -ENODEV with nothing logged when no card is
 * present, or the errno of the step that failed, which it logs.
 */
int ath9k_probe(void)
{
    pci_device_t dev;
    const struct ath9k_chip *chip = find_card(&dev);
    int rc;

    if (!chip)
        return -ENODEV;
    rc = probe_card(&card, chip, &dev);
    card_registered = rc == 0;
    return rc;
}

/**
 * The timer tick's service of the card, as intr.c's ath9k_tick gives it:
 * the whole handler on a card without messages, the rings alone on one
 * with them. Nothing before the probe has registered the card. Interrupt,
 * from the tick.
 */
void ath9k_poll(void)
{
    if (card_registered)
        ath9k_tick(&card);
}
