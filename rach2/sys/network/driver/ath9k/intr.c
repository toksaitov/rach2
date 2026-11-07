/*
 * intr.c: the AR9003's interrupts, by message where the machine can route
 * one and from the timer tick otherwise, and the service of their causes.
 *
 * The causes are read as ath9k's ar9003_hw_get_isr reads them and served
 * as its ath_isr and ath9k_tasklet serve them (Linux v6.16), but in the
 * handler itself: receive and transmit are bounded by their rings, and the
 * causes that need a reset hand it to the worker through recover.c.
 * Rach2 routes no INTx line of a PCI card, since it evaluates no _PRT, so
 * a card whose messages cannot be enabled is served by the tick alone, as
 * the RTL8168's is; with messages on, the tick still serves the rings, as
 * the fallback for a message that did not come.
 */

#include "ath9k.h"
#include "../../../irq.h"
#include "../../../klog.h"
#include "../../../cmdline.h"

/* The baseband watchdog's states that ath9k's ar9003_hw_bb_watchdog_check
 * knows: one it clears by a touch of the radar detector's power, one it
 * ignores, and the rest, which need a reset. */
#define BB_WD_RADAR_FIX 0x04000539u
#define BB_WD_HARMLESS  0x1300000au

/* Messages in a row that found no cause, or messages between two ticks,
 * before the card is served by the tick alone, as Linux's
 * spurious-interrupt check gives up on a line; a tick is a millisecond,
 * and a card hearing beacons sends a few messages in it. */
#define EMPTY_MAX 1000
#define BURST_MAX 1000

/* What serve found. */
#define SERVE_NONE  0 /* no cause */
#define SERVE_DONE  1 /* causes served */
#define SERVE_RESET 2 /* a reset was asked for, which left interrupts off */

/* The card the message vector serves; the driver has one. */
static struct ath9k *msi_card;

/**
 * Mask the card's INTx line with the command word's Interrupt Disable bit,
 * for a card served by the tick: no handler takes the line, so a level
 * interrupt nobody clears would hold its pin, unmasked or shared, for
 * ever. A card with messages needs no mask, since MSI suppresses INTx.
 */
static void intx_off(struct ath9k *ah)
{
    const pci_device_t *d = &ah->bus.dev;
    uint16_t command = pci_read_config16(d->bus, d->device, d->function, PCI_COMMAND);

    pci_write_config16(d->bus, d->device, d->function, PCI_COMMAND,
                       (uint16_t)(command | PCI_CMD_INTERRUPT_DIS));
}

/** The vector's handler. */
static void ath9k_msi(isr_params_t *params)
{
    (void)params;
    if (msi_card)
        ath9k_intr(msi_card);
}

/**
 * Take a vector for the card's messages and enable them, as the RTL8168's
 * driver does: the handler first, so that no message arrives on a vector
 * nothing serves, then the capability. The chip's own message engine is
 * enabled with its interrupts, from mac.msi. A card left polled has its
 * INTx line masked.
 */
int ath9k_intr_attach(struct ath9k *ah)
{
    int vector;

    ah->bus.vector = 0;
    ah->mac.msi = 0;
    if (kcmdline_has("rach2.ath9k_nomsi")) {
        intx_off(ah);
        klog(KLOG_INFO, "ath9k", "interrupts: polled, rach2.ath9k_nomsi");
        return 0;
    }
    if (!isr_msi_ready()) {
        intx_off(ah);
        klog(KLOG_INFO, "ath9k", "interrupts: polled, no usable APIC");
        return 0;
    }
    vector = isr_alloc_vector();
    if (vector < 0) {
        intx_off(ah);
        klog(KLOG_INFO, "ath9k", "interrupts: polled, no vector free");
        return 0;
    }
    msi_card = ah;
    register_isr_handler((uint8_t)vector, ath9k_msi, "ath9k");
    if (pci_msi_enable(&ah->bus.dev, (uint8_t)vector) != 0) {
        isr_free_vector(vector);
        msi_card = 0;
        intx_off(ah);
        klog(KLOG_INFO, "ath9k", "interrupts: polled, the card sends no messages");
        return 0;
    }
    ah->bus.vector = (uint8_t)vector;
    ah->mac.msi = 1;
    klog(KLOG_INFO, "ath9k", "interrupts: messages on vector %d", vector);
    return 0;
}

/** Disable the card's messages, mask its INTx line and give the vector
 * back. */
void ath9k_intr_detach(struct ath9k *ah)
{
    if (!ah->bus.vector) {
        intx_off(ah);
        return;
    }
    pci_msi_disable(&ah->bus.dev);
    intx_off(ah);
    isr_free_vector(ah->bus.vector);
    msi_card = 0;
    ah->bus.vector = 0;
    ah->mac.msi = 0;
}

/**
 * Judge a baseband watchdog's status, as ar9003_hw_bb_watchdog_check
 * does: nonzero when it needs a reset.
 */
static int bb_watchdog_check(struct ath9k *ah, uint32_t status)
{
    switch (status) {
    case BB_WD_RADAR_FIX:
        REG_RMW_FIELD(ah, AR_PHY_RADAR_0, AR_PHY_RADAR_0_FIRPWR, 0x7f);
        ATH9K_UDELAY(ah, 1);
        REG_RMW_FIELD(ah, AR_PHY_RADAR_0, AR_PHY_RADAR_0_FIRPWR, AR9300_DFS_FIRPWR);
        return 0;
    case BB_WD_HARMLESS:
        return 0;
    default:
        return 1;
    }
}

/* The causes that mean the receive path ran. */
#define RX_CAUSES (AR_ISR_HP_RXOK | AR_ISR_LP_RXOK | AR_ISR_RXERR | AR_ISR_RXEOL | AR_ISR_RXORN)

/**
 * Serve the rings `isr` names, high-priority receive first, and return
 * the frames. The receive path counts as active, for recover.c, when a
 * receive cause was raised or, on a look without causes, when a frame
 * was found, as ath9k counts its receive interrupts.
 */
static unsigned serve_rings(struct ath9k *ah, uint32_t isr, int raised)
{
    unsigned work = 0, rx = 0;

    if (isr & AR_ISR_HP_RXOK)
        rx += ath9k_rx_service(ah, 1);
    if (isr & (AR_ISR_LP_RXOK | AR_ISR_RXERR | AR_ISR_RXEOL | AR_ISR_RXORN))
        rx += ath9k_rx_service(ah, 0);
    if ((raised && (isr & RX_CAUSES)) || rx)
        ah->recover.rx_active++;
    if (isr & (AR_ISR_TXOK | AR_ISR_TXERR | AR_ISR_TXEOL))
        work += ath9k_tx_service(ah);
    return work + rx;
}

/**
 * Read and serve the causes, as ath9k_hw_intrpend and ar9003_hw_get_isr
 * read them on a chip whose ISR clears on read, with the chip's
 * interrupts still enabled, since the asynchronous cause shows the MAC's
 * only while they are: the MAC's when the asynchronous cause says so and
 * the chip is awake, its second word when the first asks, and the
 * synchronous PCIe errors, which are fatal. Returns SERVE_NONE, SERVE_DONE
 * or SERVE_RESET.
 */
static int serve(struct ath9k *ah)
{
    uint32_t async, sync, isr = 0, isr2 = 0;

    async = REG_READ(ah, AR_INTR_ASYNC_CAUSE);
    if (async == 0xffffffffu) {
        ath9k_reset_schedule(ah, ATH9K_RESET_FATAL);
        return SERVE_RESET;
    }
    if ((async & AR_INTR_MAC_IRQ) &&
        (REG_READ(ah, AR_RTC_STATUS) & AR_RTC_STATUS_M) == AR_RTC_STATUS_ON) {
        isr = REG_READ(ah, AR_ISR);
        if (isr & AR_ISR_BCNMISC)
            isr2 = REG_READ(ah, AR_ISR_S2);
        isr = REG_READ(ah, AR_ISR_RAC);
    }
    sync = REG_READ(ah, AR_INTR_SYNC_CAUSE) & AR_INTR_SYNC_DEFAULT;
    if (sync) {
        int fatal = (sync & (AR_INTR_SYNC_HOST1_FATAL | AR_INTR_SYNC_HOST1_PERR)) != 0;

        if (sync & AR_INTR_SYNC_RADM_CPL_TIMEOUT) {
            REG_WRITE(ah, AR_RC, AR_RC_HOSTIF);
            REG_WRITE(ah, AR_RC, 0);
            fatal = 1;
        }
        REG_WRITE(ah, AR_INTR_SYNC_CAUSE_CLR, sync);
        (void)REG_READ(ah, AR_INTR_SYNC_CAUSE_CLR);
        if (fatal) {
            ath9k_reset_schedule(ah, ATH9K_RESET_FATAL);
            return SERVE_RESET;
        }
    }
    if (isr == 0xffffffffu)
        return SERVE_DONE;
    if (isr == 0 && sync == 0)
        return SERVE_NONE;
    if ((isr2 & AR_ISR_S2_BB_WATCHDOG) && (ah->mac.imask2 & AR_IMR_S2_BB_WATCHDOG)) {
        uint32_t status = REG_READ(ah, AR_PHY_WATCHDOG_STATUS);

        REG_WRITE(ah, AR_PHY_WATCHDOG_STATUS, status & ~AR_PHY_WATCHDOG_STATUS_CLR);
        ah->recover.bb_status = status;
        if (bb_watchdog_check(ah, status)) {
            ath9k_reset_schedule(ah, ATH9K_RESET_BB_WATCHDOG);
            return SERVE_RESET;
        }
    }
    if (isr & AR_ISR_BMISS)
        ah->stats.beacon_miss++;
    if (isr & AR_ISR_RXORN)
        ah->stats.rx_overrun++;
    (void)serve_rings(ah, isr, 1);
    return SERVE_DONE;
}

/**
 * Give up on the card's messages after EMPTY_MAX in a row found no cause
 * or BURST_MAX came between two ticks: disable them at the bus, so that
 * the card can send no more, mask its INTx line, which the card raises
 * once messages are off, and serve the card from the tick, whose service
 * reads the same causes, with the chip's message engine off.
 */
static void messages_off(struct ath9k *ah, const char *why)
{
    pci_msi_disable(&ah->bus.dev);
    intx_off(ah);
    ah->mac.msi = 0;
    ath9k_mac_intr_disable(ah);
    ath9k_mac_intr_enable(ah);
    klog(KLOG_WARN, "ath9k", "interrupts: polled, %s", why);
}

/**
 * The card's interrupt, as ath_isr and ath9k_tasklet take it on a card
 * with messages: nothing while interrupts are off, since a message sent
 * before a disable may arrive in the middle of the reset or retune that
 * followed it; the chip's interrupts killed and nothing more while a reset
 * is asked for; otherwise the causes read and cleared before anything else
 * is written, the chip's interrupts killed, the causes served at once,
 * since the handler runs with the processor's interrupts off, and the
 * interrupts enabled again, which writes the message engine's enable as
 * ath9k's does after every interrupt. A message that finds no cause is
 * counted. EMPTY_MAX of them in a row, or BURST_MAX messages between two
 * ticks, end the messages.
 */
void ath9k_intr(struct ath9k *ah)
{
    int served;

    ah->stats.interrupts++;
    if (!ah->mac.irq_on)
        return;
    if (ah->recover.resetting || ah->recover.pending) {
        ath9k_mac_intr_disable(ah);
        return;
    }
    if (ah->mac.msi && ++ah->mac.burst >= BURST_MAX) {
        messages_off(ah, "too many messages between two ticks");
        return;
    }
    served = serve(ah);
    if (served == SERVE_RESET)
        return;
    if (served == SERVE_DONE) {
        ah->mac.empty = 0;
        ath9k_mac_intr_disable(ah);
        ath9k_mac_intr_enable(ah);
        return;
    }
    ah->stats.spurious++;
    if (++ah->mac.empty >= EMPTY_MAX && ah->mac.msi)
        messages_off(ah, "messages found no cause");
}

/**
 * The tick: the causes, served as the handler serves them, for a card
 * without messages, whose interrupts stay on since no line carries them;
 * on one with messages, a look at the rings for what a missed message
 * left there, as on any card for frames the station had no room for, which
 * raise no cause of their own. Nothing while interrupts are off, which
 * covers every start, reset and retune.
 */
void ath9k_tick(struct ath9k *ah)
{
    ah->mac.burst = 0;
    if (!ah->mac.irq_on || ah->recover.resetting || ah->recover.pending ||
        (!ah->rx.running && !ah->tx.running))
        return;
    if (!ah->mac.msi) {
        (void)serve(ah);
        if (!ah->rx.held[0] && !ah->rx.held[1])
            return;
    }
    if (serve_rings(ah, AR_ISR_HP_RXOK | AR_ISR_LP_RXOK | AR_ISR_TXOK, 0))
        ah->stats.tick_work++;
}
