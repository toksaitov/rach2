/*
 * recover.c: the hang checks of the AR9003 and the reset that brings the
 * chip and the station's state on it back.
 *
 * It follows ath9k's link.c (ath_hw_check_work, ath_tx_complete_check and
 * the receive inactivity check of b5f871ab4913), ar9003_hw.c's MAC hang
 * signature (ar9003_hw_detect_mac_hang), ar9003_phy.c's baseband watchdog
 * and main.c's ath_reset_internal (Linux v6.16). A check runs on the
 * wireless worker every ATH9K_HANG_MS; the interrupt handler asks for a
 * reset through ath9k_reset_schedule, which the worker carries out.
 */

#include "ath9k.h"
#include "../../../klog.h"
#include "../../../lib/kerrno.h"
#include "../../../timer.h"

/* A receive window of ath9k's RX_INACTIVE_CHECK_INTERVAL; one that ran
 * past twice that, as after a suspend, is not judged. */
#define RX_WINDOW_MS 4000

/* Resets in a row that may fail before the checks stop asking for more. */
#define MAX_FAILURES 3

/* ar9003_hw_init_hang_checks: the baseband watchdog fires after 25 ms in
 * one state outside idle. */
#define BB_WATCHDOG_MS 25

/* ar9003_hw_detect_mac_hang: a DCU chain waiting for its frame to finish,
 * with the DCU's completion state at 1, read 50 times in a row. */
#define DCU_WAIT_FRDONE    0x6
#define DCU_COMPLETE_STATE 0x1
#define HANG_READS         50

/* The names of enum ath9k_reset_reason, for the log. */
static const char *const reason_name[ATH9K_RESET_REASONS] = {
    "fatal", "bb-watchdog", "bb-hang", "mac-hang", "tx-hang", "rx-stall", "calibration"};

/** The card whose recover.`member` is `w`. */
#define CARD_OF(w, member) ((struct ath9k *)(void *)((char *)(w) - offsetof(struct ath9k, member)))

/**
 * Arm the baseband watchdog, as ar9003_hw_bb_watchdog_config does: an
 * interrupt, not a reset of its own, after BB_WATCHDOG_MS in one state
 * while the baseband is busy, in units of 2^15 cycles of 44 MHz, 0.74 ms.
 */
void ath9k_recover_watchdog(struct ath9k *ah)
{
    uint32_t val = REG_READ(ah, AR_PHY_WATCHDOG_CTL_2) & AR_PHY_WATCHDOG_CNTL2_MASK;
    uint32_t idle_count = 100 * BB_WATCHDOG_MS / 74;

    REG_WRITE(ah, AR_PHY_WATCHDOG_CTL_2,
              (val | AR_PHY_WATCHDOG_IRQ_ENABLE) & ~AR_PHY_WATCHDOG_RST_ENABLE);
    REG_WRITE(ah, AR_PHY_WATCHDOG_CTL_1,
              AR_PHY_WATCHDOG_NON_IDLE_ENABLE | AR_PHY_WATCHDOG_IDLE_MASK |
                  (AR_PHY_WATCHDOG_NON_IDLE_MASK & (idle_count << 2)));
}

/** The state of DCU `q`'s chain, five bits of AR_DMADBG_4 for queues 0
 * to 5 and of AR_DMADBG_5 for 6 to 9, given the two registers. */
static unsigned chain_state(uint32_t dbg4, uint32_t dbg5, unsigned q)
{
    return q < 6 ? (dbg4 >> (q * 5)) & 0x1f : (dbg5 >> ((q - 6) * 5)) & 0x1f;
}

/**
 * Whether the MAC is hung, as ar9003_hw_detect_mac_hang and
 * ath9k_hw_verify_hang find it: a DCU waiting for a frame to finish while
 * the DCUs' completion state is 1, the same in HANG_READS reads in a row.
 * The registers are the AR9003's (3e56c80931c7).
 */
static int mac_hung(struct ath9k *ah)
{
    uint32_t dbg4 = REG_READ(ah, AR_DMADBG_4), dbg5 = REG_READ(ah, AR_DMADBG_5);
    uint32_t dbg6 = REG_READ(ah, AR_DMADBG_6);
    unsigned q, i;

    if ((dbg6 & 0x3) != DCU_COMPLETE_STATE)
        return 0;
    for (q = 0; q < AR_NUM_QCU; q++) {
        if (chain_state(dbg4, dbg5, q) != DCU_WAIT_FRDONE)
            continue;
        for (i = 0; i < HANG_READS; i++) {
            uint32_t chain = REG_READ(ah, q < 6 ? AR_DMADBG_4 : AR_DMADBG_5);

            dbg6 = REG_READ(ah, AR_DMADBG_6);
            if (chain_state(chain, chain, q) != DCU_WAIT_FRDONE ||
                (dbg6 & 0x3) != DCU_COMPLETE_STATE)
                break;
        }
        if (i == HANG_READS)
            return 1;
    }
    return 0;
}

/**
 * Reset the chip on its channel and restore the station's state on it, as
 * ath_reset_internal does with ath_prepare_reset and ath_complete_reset:
 * interrupts off, receive stopped, transmit aborted and every frame
 * reported failed; for a hang or a fatal error a cold reset of the MAC and
 * baseband first, then ath9k_hw_reset; then the address, BSSID, keys,
 * filter and receive, the beacon timers, the watchdog and the interrupts.
 * A reset that fails leaves the chip off and is tried again by the next
 * check, MAX_FAILURES times in all.
 */
int ath9k_reset(struct ath9k *ah, enum ath9k_reset_reason why)
{
    int rc;

    if (ah->phy.channel.frequency == 0)
        return -ENETDOWN;
    ah->recover.resetting = 1;
    ath9k_mac_intr_disable(ah);
    (void)ath9k_rx_stop(ah);
    (void)ath9k_mac_abort_tx(ah);
    ath9k_tx_drain(ah);
    if (why != ATH9K_RESET_RX_STALL && why != ATH9K_RESET_CAL)
        (void)ath9k_hw_set_reset(ah, ATH9K_RESET_COLD);
    rc = ath9k_hw_reset(ah, &ah->phy.channel);
    if (rc == 0) {
        ath9k_mac_set_address(ah);
        ath9k_mac_set_bssid(ah, ah->mac.bssid, ah->mac.aid);
        ath9k_key_restore(ah);
        rc = ath9k_rx_start(ah);
    }
    if (why < ATH9K_RESET_REASONS)
        ah->stats.resets[why]++;
    ah->recover.reason = (uint8_t)why;
    if (rc) {
        ah->recover.failures++;
        ah->recover.resetting = 0;
        klog(KLOG_WARN, "ath9k", "reset: %s: failed: error %d", reason_name[why], rc);
        return rc;
    }
    if (ah->mac.beacon_interval)
        ath9k_mac_set_beacon_timers(ah, ah->mac.beacon_interval, ah->mac.dtim_period);
    ath9k_recover_watchdog(ah);
    ah->recover.failures = 0;
    ah->recover.window = tick_count();
    ah->recover.rx_active = 0;
    ah->recover.resetting = 0;
    ath9k_mac_intr_enable(ah);
    klog(KLOG_INFO, "ath9k", "reset: %s: recovered", reason_name[why]);
    return 0;
}

/** The worker's side of ath9k_reset_schedule. */
static void reset_run(struct wlan_work *w)
{
    struct ath9k *ah = CARD_OF(w, recover.reset_work);

    ah->recover.pending = 0;
    (void)ath9k_reset(ah, (enum ath9k_reset_reason)ah->recover.reason);
}

/**
 * Turn the chip's interrupts off and queue a reset for `why` on the
 * worker, as ath9k_queue_reset does; a reset already queued keeps its
 * reason. The tick serves nothing until it has run.
 */
void ath9k_reset_schedule(struct ath9k *ah, enum ath9k_reset_reason why)
{
    ath9k_mac_intr_disable(ah);
    if (ah->recover.pending)
        return;
    ah->recover.pending = 1;
    ah->recover.reason = (uint8_t)why;
    ah->recover.reset_work.wlan = ah->radio.wlan;
    ah->recover.reset_work.run = reset_run;
    if (wlan_queue_work(&ah->recover.reset_work, 0) != 0)
        ah->recover.pending = 0;
}

/**
 * Run the checks of ath_hw_check_work: the chip alive and its MAC not
 * hung; each queue that held pushed frames at the last check has finished
 * one since (ath_tx_complete_check); and, while associated, the receive
 * path ran at least once a second over a window (b5f871ab4913). ath9k
 * judges the receive path unassociated too, which resets a card on a
 * quiet channel every few seconds; a station that hears its AP's beacons
 * ten times a second needs no such guess.
 */
int ath9k_hang_check(struct ath9k *ah)
{
    enum ath9k_reset_reason why = ATH9K_RESET_REASONS;
    uint32_t cfg;
    uint64_t now, interval;
    unsigned ac;

    if (ah->recover.resetting || ah->recover.pending)
        return -1;
    if (ah->recover.failures) {
        if (ah->recover.failures >= MAX_FAILURES)
            return -1;
        why = (enum ath9k_reset_reason)ah->recover.reason;
        (void)ath9k_reset(ah, why);
        return (int)why;
    }
    cfg = REG_READ(ah, AR_CFG);
    if (cfg == 0xdeadbeefu || cfg == 0xffffffffu || mac_hung(ah))
        why = ATH9K_RESET_MAC_HANG;
    for (ac = 0; ac < ATH9K_TX_QUEUES && why == ATH9K_RESET_REASONS; ac++) {
        struct ath9k_txq *q = &ah->tx.q[ac];

        if (q->queued == 0)
            continue;
        if (q->inflight)
            why = ATH9K_RESET_TX_HANG;
        q->inflight = 1;
    }
    now = tick_count();
    interval = now - ah->recover.window;
    if (why == ATH9K_RESET_REASONS && interval >= RX_WINDOW_MS) {
        uint32_t count = ah->recover.rx_active;

        ah->recover.window = now;
        ah->recover.rx_active = 0;
        if (ah->mac.aid && interval <= 2 * RX_WINDOW_MS && count < (uint32_t)interval / 1000)
            why = ATH9K_RESET_RX_STALL;
    }
    if (why == ATH9K_RESET_REASONS)
        return -1;
    (void)ath9k_reset(ah, why);
    return (int)why;
}

/** The periodic check, queued again while the check runs. */
static void check_run(struct wlan_work *w)
{
    struct ath9k *ah = CARD_OF(w, recover.work);

    (void)ath9k_hang_check(ah);
    if (ah->recover.running)
        (void)wlan_queue_work(w, tick_count() + ATH9K_HANG_MS);
}

/** Arm the watchdog and queue the first check a period from now. */
void ath9k_recover_start(struct ath9k *ah)
{
    ah->recover.window = tick_count();
    ah->recover.rx_active = 0;
    ah->recover.failures = 0;
    ah->recover.running = 1;
    ah->recover.work.wlan = ah->radio.wlan;
    ah->recover.work.run = check_run;
    ath9k_recover_watchdog(ah);
    (void)wlan_queue_work(&ah->recover.work, tick_count() + ATH9K_HANG_MS);
}

/** Stop the checks and drop a reset not yet run. */
void ath9k_recover_stop(struct ath9k *ah)
{
    ah->recover.running = 0;
    wlan_cancel_work(&ah->recover.work);
    wlan_cancel_work(&ah->recover.reset_work);
    ah->recover.pending = 0;
}
