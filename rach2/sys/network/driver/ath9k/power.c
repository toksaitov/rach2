/*
 * power.c: PCIe ASPM, the EEPROM's RF-silence switch and suspend to idle.
 * GPIO decoding follows Linux v6.16 ath9k hw.c and eeprom.h. The link is
 * left as firmware set it unless rach2.ath9k_noaspm is on the boot line.
 */
#include "ath9k.h"
#include "../../wireless/wiphy.h"
#include "../../../cmdline.h"
#include "../../../lib/kerrno.h"

/* Borrowed for the kernel's lifetime: this driver serves one fixed card. */
static struct ath9k *power_card;

/** Read a device's valid PCIe link control word into *control.
 * Returns its configuration offset, or zero for no readable capability. */
static unsigned power_link(const pci_device_t *dev, uint16_t *control)
{
    unsigned cap = pci_find_capability(dev->bus, dev->device, dev->function, PCI_CAP_ID_PCIE);

    if (!cap || cap > 0xec)
        return 0;
    *control =
        pci_read_config16(dev->bus, dev->device, dev->function, (uint8_t)(cap + PCIE_LINK_CONTROL));
    return *control == 0xffff ? 0 : cap + PCIE_LINK_CONTROL;
}

/** Clear only the L0s and L1 bits at this end of a PCIe link. */
static void power_no_aspm(const pci_device_t *dev)
{
    uint16_t control;
    unsigned offset = power_link(dev, &control);

    if (offset && (control & PCIE_LINK_CTL_ASPM_MASK))
        pci_write_config16(dev->bus, dev->device, dev->function, (uint8_t)offset,
                           control & ~PCIE_LINK_CTL_ASPM_MASK);
}

/** Set ASPM policy and decode the EEPROM's RF-silence GPIO at attach.
 * The card remains allocated for the kernel's lifetime. Call rfkill_poll
 * again after radio registration, and periodically even while stopped. */
void ath9k_power_init(struct ath9k *ah)
{
    struct ath9k_power_state *p = &ah->power;
    pci_device_t bridge;
    uint16_t control;
    unsigned silent = ah->eeprom.image.baseEepHeader.rfSilent;

    power_card = ah;
    p->aspm = power_link(&ah->bus.dev, &control) ? control & PCIE_LINK_CTL_ASPM_MASK : 0;
    if (kcmdline_has("rach2.ath9k_noaspm"))
        ah->options |= ATH9K_OPT_NO_ASPM;
    if (ah->options & ATH9K_OPT_NO_ASPM) {
        if (pci_find_bridge_for_bus(ah->bus.dev.bus, &bridge))
            power_no_aspm(&bridge);
        power_no_aspm(&ah->bus.dev);
    }
    p->rfkill_enabled = silent & 1;
    p->rfkill_polarity = (silent >> 1) & 1;
    p->rfkill_gpio = (silent >> 2) & 0x3f;
    p->rfkill_reported = p->hw_blocked = p->suspended = p->resume = 0;
    ath9k_rfkill_poll(ah);
}

/** Sample the hard switch and queue a change through the station layer.
 * IRQ-safe under the kernel lock; the host GPIO window remains readable
 * while the radio is stopped. Restore the input direction after a reset.
 * An unreadable card is treated as blocked. */
void ath9k_rfkill_poll(struct ath9k *ah)
{
    struct ath9k_power_state *p = &ah->power;
    uint32_t value, drive, mask;
    unsigned blocked = 0;

    if (p->suspended)
        return;
    if (p->rfkill_enabled) {
        if (p->rfkill_gpio >= ah->hw.chip->num_gpio || p->rfkill_gpio >= 16)
            blocked = 1;
        else {
            value = REG_READ(ah, AR_GPIO_IN);
            if (value != 0xffffffffu) {
                mask = 3u << (p->rfkill_gpio * 2);
                drive = REG_READ(ah, AR_GPIO_OE_OUT);
                if (drive & mask) {
                    REG_WRITE(ah, AR_GPIO_OE_OUT, drive & ~mask);
                    value = REG_READ(ah, AR_GPIO_IN);
                }
            }
            blocked = value == 0xffffffffu || ((value >> p->rfkill_gpio) & 1) == p->rfkill_polarity;
        }
    }
    if (ah->radio.wlan && (!p->rfkill_reported || p->hw_blocked != blocked)) {
        wlan_radio_rfkill(&ah->radio, blocked);
        p->rfkill_reported = 1;
    }
    p->hw_blocked = blocked;
}

/** Stop the station before processes freeze, remembering whether it ran
 * or was waiting for rfkill. Process context under the kernel lock. */
void ath9k_suspend(struct ath9k *ah)
{
    struct wlan *wlan = ah->radio.wlan;

    if (ah->power.suspended || !wlan)
        return;
    ah->power.resume = wlan->started || wlan->resume;
    ah->power.suspended = 1;
    wlan_stop(wlan);
}

/** Sample the switch and restart the wanted station after processes thaw.
 * A hard or soft block defers restart to rfkill. Other start errors leave
 * the restart request set, so a later call can retry. */
int ath9k_resume(struct ath9k *ah)
{
    struct wlan *wlan = ah->radio.wlan;
    int rc = 0;

    if ((!ah->power.suspended && !ah->power.resume) || !wlan)
        return 0;
    ah->power.suspended = 0;
    ath9k_rfkill_poll(ah);
    if (ah->power.resume) {
        if (ah->power.hw_blocked || wlan->soft_blocked) {
            wlan->resume = 1;
        } else {
            rc = wlan_start(wlan);
            if (rc == -ERFKILL)
                rc = 0;
        }
    }
    if (!rc)
        ah->power.resume = 0;
    return rc;
}

/** Stop the attached card, if any, before suspend to idle. */
void ath9k_suspend_all(void)
{
    if (power_card)
        ath9k_suspend(power_card);
}

/** Resume the attached card after suspend to idle; return its start error. */
int ath9k_resume_all(void)
{
    return power_card ? ath9k_resume(power_card) : 0;
}
