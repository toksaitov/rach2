/*
 * mac.c: the AR9003's queues, DMA engines, PCU addresses and filter,
 * interrupt masks and station beacon timers, at the level of registers.
 *
 * The sequences follow ath9k's mac.c, ar9003_mac.c and the parts of hw.c
 * that set up DMA and the interrupt masks (Linux v6.16); each function
 * names the one it follows. The calls that hold the frames, recv.c and
 * xmit.c, sit above these.
 */

#include "ath9k.h"
#include "../../../lib/kerrno.h"
#include "../../../lib/kstring.h"
#ifndef RACH2_HOSTTEST
#include "../../../mm/paging.h"
#endif

/* ath9k's mac.h defaults for a queue: the short and long retry limits of
 * the station, and of a frame, which the descriptor's series replace. */
#define INIT_SH_RETRY  10
#define INIT_SSH_RETRY 32
#define INIT_SLG_RETRY 32

/* ath9k's hw.h: the RSSI threshold after a reset, the beacon timers' slop
 * and time-outs, and the TSF out-of-range threshold, 16 ms. */
#define INIT_RSSI_THR          0x00000700
#define SLEEP_SLOP_US          (3 * 1024)
#define CAB_TIMEOUT_VAL        10
#define MIN_BEACON_TIMEOUT_VAL 1
#define TSFOOR_THRESHOLD       0x00004240

/* ath9k_cmn_beacon_config_sta: a beacon miss after ten of them, and the
 * two TU ath9k_get_next_tbtt adds before rounding up to the next one. */
#define BMISS_BEACONS  10
#define NEXT_TBTT_SLOP 2

/* How long ath9k waits for a queue and for receive DMA to stop, in
 * microseconds, and its poll interval for the queue. */
#define TX_STOP_TIMEOUT 1000
#define TX_STOP_QUANTUM 100
#define RX_STOP_TIMEOUT 10000

/* A queue's contention parameters: mac80211's defaults for each access
 * category, as ath9k gets them before the station's first beacon, by
 * enum wlan_ac. The burst time is in microseconds. */
static const struct {
    uint8_t aifs;
    uint16_t cwmin, cwmax;
    uint16_t burst;
} ac_params[ATH9K_TX_QUEUES] = {{2, 3, 7, 1504},   /* WLAN_AC_VO */
                                {2, 7, 15, 3008},  /* WLAN_AC_VI */
                                {3, 15, 1023, 0},  /* WLAN_AC_BE */
                                {7, 15, 1023, 0}}; /* WLAN_AC_BK */

#ifndef RACH2_HOSTTEST
/**
 * One zeroed kernel frame, which the frame allocator takes from below the
 * window's top so that its address fits the card's 32-bit pointers.
 */
void *ath9k_dma_alloc(struct ath9k *ah, uint32_t *bus)
{
    void *page = alloc_kernel_frame();

    (void)ah;
    if (!page)
        return 0;
    k_memset(page, 0, ATH9K_DMA_PAGE);
    *bus = (uint32_t)virt_to_phys(page);
    return page;
}

/** Give back a frame of ath9k_dma_alloc. */
void ath9k_dma_free(struct ath9k *ah, void *page)
{
    (void)ah;
    if (page)
        free_kernel_frame(page);
}
#endif

/** The hardware queue of access category `ac`: ath9k's ATH_TXQ_AC_BK to
 * _VO are queues 0 to 3, and the DCU of a higher queue wins arbitration. */
static unsigned ac_qnum(unsigned ac)
{
    return ATH9K_TX_QUEUES - 1 - ac;
}

/**
 * Set up queue `q` for access category `ac`, as ath9k_hw_resettxqueue
 * does for a data queue that interrupts on every frame's status.
 */
static void reset_tx_queue(struct ath9k *ah, unsigned q, unsigned ac)
{
    unsigned burst = ac_params[ac].burst;

    REG_WRITE(ah, AR_DLCL_IFS(q),
              SM(ac_params[ac].cwmin, AR_D_LCL_IFS_CWMIN) |
                  SM(ac_params[ac].cwmax, AR_D_LCL_IFS_CWMAX) |
                  SM(ac_params[ac].aifs, AR_D_LCL_IFS_AIFS));
    REG_WRITE(ah, AR_DRETRY_LIMIT(q),
              SM(INIT_SSH_RETRY, AR_D_RETRY_LIMIT_STA_SH) |
                  SM(INIT_SLG_RETRY, AR_D_RETRY_LIMIT_STA_LG) |
                  SM(INIT_SH_RETRY, AR_D_RETRY_LIMIT_FR_SH));
    REG_WRITE(ah, AR_QMISC(q), AR_Q_MISC_DCU_EARLY_TERM_REQ);
    REG_WRITE(ah, AR_DMISC(q), AR_D_MISC_CW_BKOFF_EN | AR_D_MISC_FRAG_WAIT_EN | 0x2);
    REG_WRITE(ah, AR_DCHNTIME(q), SM(burst, AR_D_CHNTIME_DUR) | (burst ? AR_D_CHNTIME_EN : 0));
    REG_WRITE(ah, AR_Q_DESC_CRCCHK, AR_Q_DESC_CRCCHK_EN);
}

/**
 * Set up DMA, the data queues, the status ring and the interrupt masks,
 * and let xmit.c push frames again. DMA is ath9k_hw_set_dma's: 128-byte
 * bursts both ways, one free buffer before a receive FIFO's threshold, the
 * receive buffer's length after its status, and the PCU's usable buffer.
 * Every queue reports its frames done and failed, and none a descriptor,
 * its end or an underrun. The masks are ath9k_hw_init_interrupt_masks'
 * without receive mitigation, so that each FIFO's frame raises its own
 * cause, and with the baseband watchdog in the second mask; recover.c's
 * check of the queues stands in for the global transmit time-out.
 */
void ath9k_mac_init_queues(struct ath9k *ah)
{
    uint32_t qmask = 0;
    unsigned ac, i;

    /* ath9k_hw_set_dma. */
    REG_RMW(ah, AR_TXCFG, AR_TXCFG_DMASZ_128B, AR_TXCFG_DMASZ_MASK);
    REG_RMW(ah, AR_RXCFG, AR_RXCFG_DMASZ_128B, AR_RXCFG_DMASZ_MASK);
    REG_WRITE(ah, AR_RXFIFO_CFG, 0x200);
    REG_RMW_FIELD(ah, AR_RXBP_THRESH, AR_RXBP_THRESH_HP, 0x1);
    REG_RMW_FIELD(ah, AR_RXBP_THRESH, AR_RXBP_THRESH_LP, 0x1);
    REG_WRITE(ah, AR_DATABUF_SIZE,
              (ATH9K_RX_BUF_LEN - sizeof(struct ar9003_rxs)) & AR_DATABUF_SIZE_MASK);
    REG_WRITE(ah, AR_PCU_TXBUF_CTRL, AR_PCU_TXBUF_CTRL_USABLE_SIZE);

    /* ath9k_hw_reset_txstatus_ring: the ring starts empty at its start. */
    if (ah->tx.status) {
        k_memset(ah->tx.status, 0, ATH9K_TXS_RING * sizeof(struct ar9003_txs));
        REG_WRITE(ah, AR_Q_STATUS_RING_START, ah->tx.status_bus);
        REG_WRITE(ah, AR_Q_STATUS_RING_END,
                  ah->tx.status_bus + ATH9K_TXS_RING * sizeof(struct ar9003_txs));
    }
    ah->tx.status_index = 0;
    ah->tx.running = 1;

    /* ath9k_hw_reset: each DCU serves the QCU of its number. */
    for (i = 0; i < AR_NUM_DCU; i++)
        REG_WRITE(ah, AR_DQCUMASK(i), 1u << i);
    for (ac = 0; ac < ATH9K_TX_QUEUES; ac++) {
        unsigned q = ac_qnum(ac);

        ah->tx.q[ac].qnum = q;
        reset_tx_queue(ah, q, ac);
        qmask |= 1u << q;
    }

    /* ath9k_hw_set_txq_interrupts. */
    REG_WRITE(ah, AR_IMR_S0, SM(qmask, AR_IMR_S0_QCU_TXOK));
    REG_WRITE(ah, AR_IMR_S1, SM(qmask, AR_IMR_S1_QCU_TXERR));

    /* ath9k_hw_init_interrupt_masks. */
    ah->mac.imask = AR_IMR_TXOK | AR_IMR_TXERR | AR_IMR_TXURN | AR_IMR_RXERR | AR_IMR_RXORN |
                    AR_IMR_RXOK_HP | AR_IMR_RXOK_LP | AR_IMR_BCNMISC | AR_IMR_BMISS;
    ah->mac.imask2 = AR_IMR_S2_BB_WATCHDOG;
    REG_WRITE(ah, AR_IMR, ah->mac.imask);
    REG_WRITE(ah, AR_IMR_S2, ah->mac.imask2);
    if (ah->mac.msi) {
        ah->mac.msi_reg = (REG_READ(ah, AR_PCIE_MSI) | AR_PCIE_MSI_HW_DBI_WR_EN) &
                          AR_PCIE_MSI_HW_INT_PENDING_ADDR_MSI_64;
        REG_WRITE(ah, AR_INTCFG, AR_INTCFG_MSI_RXOK | AR_INTCFG_MSI_TXOK);
    }
    REG_WRITE(ah, AR_INTR_SYNC_CAUSE, 0xffffffffu);
    REG_WRITE(ah, AR_INTR_SYNC_ENABLE, AR_INTR_SYNC_DEFAULT);
    REG_WRITE(ah, AR_INTR_SYNC_MASK, 0);
    REG_WRITE(ah, AR_INTR_PRIO_ASYNC_ENABLE, 0);
    REG_WRITE(ah, AR_INTR_PRIO_ASYNC_MASK, 0);
    REG_WRITE(ah, AR_INTR_PRIO_SYNC_ENABLE, 0);
    REG_WRITE(ah, AR_INTR_PRIO_SYNC_MASK, 0);
}

/** Push a descriptor chain to queue `qnum`'s FIFO, which starts it: on
 * the AR9003 a write of AR_QTXDP is the whole of ath9k_hw_puttxbuf. */
void ath9k_mac_tx_push(struct ath9k *ah, unsigned qnum, uint32_t bus)
{
    REG_WRITE(ah, AR_QTXDP(qnum), bus);
}

/** The frames queue `q` holds, as ath9k_hw_numtxpending counts them. */
static uint32_t tx_pending(struct ath9k *ah, unsigned q)
{
    uint32_t n = REG_READ(ah, AR_QSTS(q)) & AR_Q_STS_PEND_FR_CNT;

    if (n == 0 && (REG_READ(ah, AR_Q_TXE) & (1u << q)))
        n = 1;
    return n;
}

/**
 * Stop queue `qnum`, as ath9k_hw_stop_dma_queue does: disable it, wait up
 * to a millisecond for its frames to leave, and enable it again.
 */
int ath9k_mac_stop_queue(struct ath9k *ah, unsigned qnum)
{
    unsigned wait;
    int rc = -ETIMEDOUT;

    REG_WRITE(ah, AR_Q_TXD, 1u << qnum);
    for (wait = 0; wait < TX_STOP_TIMEOUT / TX_STOP_QUANTUM; wait++) {
        if (wait)
            ATH9K_UDELAY(ah, TX_STOP_QUANTUM);
        if (tx_pending(ah, qnum) == 0) {
            rc = 0;
            break;
        }
    }
    REG_WRITE(ah, AR_Q_TXD, 0);
    return rc;
}

/**
 * Abort every queue, as ath9k_hw_abort_tx_dma does: disable them all,
 * force the channel idle and the backoff off so that nothing waits for
 * the air, and wait up to 5 ms for each queue to empty.
 */
int ath9k_mac_abort_tx(struct ath9k *ah)
{
    unsigned q, i;
    int rc = 0;

    REG_WRITE(ah, AR_Q_TXD, AR_Q_TXD_M);
    REG_SET_BIT(ah, AR_PCU_MISC, AR_PCU_FORCE_QUIET_COLL | AR_PCU_CLEAR_VMF);
    REG_SET_BIT(ah, AR_DIAG_SW, AR_DIAG_FORCE_CH_IDLE_HIGH);
    REG_SET_BIT(ah, AR_D_GBL_IFS_MISC, AR_D_GBL_IFS_MISC_IGNORE_BACKOFF);
    for (q = 0; q < AR_NUM_QCU; q++) {
        for (i = 0; i < 1000; i++) {
            if (i)
                ATH9K_UDELAY(ah, 5);
            if (!tx_pending(ah, q))
                break;
        }
        if (i == 1000)
            rc = -ETIMEDOUT;
    }
    REG_CLR_BIT(ah, AR_PCU_MISC, AR_PCU_FORCE_QUIET_COLL | AR_PCU_CLEAR_VMF);
    REG_CLR_BIT(ah, AR_DIAG_SW, AR_DIAG_FORCE_CH_IDLE_HIGH);
    REG_CLR_BIT(ah, AR_D_GBL_IFS_MISC, AR_D_GBL_IFS_MISC_IGNORE_BACKOFF);
    REG_WRITE(ah, AR_Q_TXD, 0);
    return rc;
}

/** Push a receive buffer, as ath9k_hw_addrxbuf_edma does. */
void ath9k_mac_rx_push(struct ath9k *ah, int hp, uint32_t bus)
{
    REG_WRITE(ah, hp ? AR_HP_RXDP : AR_LP_RXDP, bus);
}

/** Write the filter bits `bits` and the phy errors they ask for, as
 * ath9k_hw_setrxfilter does. */
static void write_filter(struct ath9k *ah, uint32_t bits)
{
    uint32_t phybits =
        (bits & ATH9K_RX_FILTER_PHYERR) ? AR_PHY_ERR_OFDM_TIMING | AR_PHY_ERR_CCK_TIMING : 0;

    REG_WRITE(ah, AR_RX_FILTER, bits);
    REG_WRITE(ah, AR_PHY_ERR, phybits);
    if (phybits)
        REG_SET_BIT(ah, AR_RXCFG, AR_RXCFG_ZLFDMA);
    else
        REG_CLR_BIT(ah, AR_RXCFG, AR_RXCFG_ZLFDMA);
}

/**
 * Start receive, as ath9k's ath_edma_start_recv does once the buffers are
 * pushed: the receive DMA, the filter, the BSSID mask and every multicast
 * group, the station's mode, and the PCU's receive last.
 */
void ath9k_mac_rx_start(struct ath9k *ah)
{
    REG_WRITE(ah, AR_CR, AR_CR_RXE);
    write_filter(ah, ah->rx.hw_filter);
    REG_WRITE(ah, AR_BSSMSKL, 0xffffffffu);
    REG_WRITE(ah, AR_BSSMSKU, 0xffff);
    REG_RMW(ah, AR_STA_ID1, AR_STA_ID1_KSRCH_MODE, AR_STA_ID1_STA_AP | AR_STA_ID1_ADHOC);
    REG_WRITE(ah, AR_MCAST_FIL0, 0xffffffffu);
    REG_WRITE(ah, AR_MCAST_FIL1, 0xffffffffu);
    REG_CLR_BIT(ah, AR_DIAG_SW, AR_DIAG_RX_DIS | AR_DIAG_RX_ABORT);
}

/**
 * Stop receive, as ath_stoprecv does with ath9k_hw_abortpcurecv and
 * ath9k_hw_stopdmarecv: abort the PCU, clear the filter on the chip while
 * keeping rx.hw_filter for the next start (f0b2c30a2029), and wait up to
 * 10 ms for the receive DMA to stop.
 */
int ath9k_mac_rx_stop(struct ath9k *ah)
{
    unsigned i;

    REG_SET_BIT(ah, AR_DIAG_SW, AR_DIAG_RX_DIS | AR_DIAG_RX_ABORT);
    write_filter(ah, 0);
    REG_WRITE(ah, AR_MACMISC,
              (AR_MACMISC_DMA_OBS_LINE_8 << AR_MACMISC_DMA_OBS_S) |
                  (AR_MACMISC_MISC_OBS_BUS_1 << AR_MACMISC_MISC_OBS_BUS_MSB_S));
    REG_WRITE(ah, AR_CR, AR_CR_RXD);
    for (i = 0; i < RX_STOP_TIMEOUT / ATH9K_WAIT_QUANTUM; i++) {
        if ((REG_READ(ah, AR_CR) & AR_CR_RXE) == 0)
            return 0;
        ATH9K_UDELAY(ah, ATH9K_WAIT_QUANTUM);
    }
    return -ETIMEDOUT;
}

/**
 * Turn WLAN_FILTER_* into ath9k's filter bits, as ath_calcrxfilter does
 * for one station: frames to the station, broadcast and multicast; the
 * BSS's beacons alone once associated, and every beacon otherwise or
 * when another BSS's frames are asked for; the AR9462 design's control
 * wrapper. While receive is stopped the bits are kept, not written; the
 * next start writes them.
 */
void ath9k_mac_set_filter(struct ath9k *ah, unsigned filters)
{
    uint32_t bits = ATH9K_RX_FILTER_UCAST | ATH9K_RX_FILTER_BCAST | ATH9K_RX_FILTER_MCAST;

    if (filters & WLAN_FILTER_PROMISC)
        bits |= ATH9K_RX_FILTER_PROM;
    if (filters & WLAN_FILTER_CONTROL)
        bits |= ATH9K_RX_FILTER_CONTROL;
    if (ah->mac.aid && !(filters & WLAN_FILTER_OTHER_BSS))
        bits |= ATH9K_RX_FILTER_MYBEACON;
    else
        bits |= ATH9K_RX_FILTER_BEACON;
    if (filters & WLAN_FILTER_OTHER_BSS)
        bits |= ATH9K_RX_FILTER_MCAST_BCAST_ALL;
    if (ah->hw.chip && (ah->hw.chip->caps & ATH9K_CHIP_JUPITER))
        bits |= ATH9K_RX_FILTER_CONTROL_WRAPPER;
    ah->rx.filters = filters;
    ah->rx.hw_filter = bits;
    if (ah->rx.running)
        write_filter(ah, bits);
}

/** A little-endian 32-bit and 16-bit word of an address. */
static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static uint32_t le16(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8;
}

/**
 * Set the station's address from radio.mac, as ath9k_hw_reset_opmode
 * does: AR_STA_ID1 keeps its other bits and takes RTS's default rate,
 * the MIC's new location and the key search of multicast frames.
 */
void ath9k_mac_set_address(struct ath9k *ah)
{
    REG_WRITE(ah, AR_STA_ID0, le32(ah->radio.mac));
    REG_RMW(ah, AR_STA_ID1,
            le16(ah->radio.mac + 4) | AR_STA_ID1_RTS_USE_DEF | AR_STA_ID1_CRPT_MIC_ENABLE |
                AR_STA_ID1_MCAST_KSRCH | AR_STA_ID1_PRESERVE_SEQNUM,
            AR_STA_ID1_SADH_MASK);
    REG_SET_BIT(ah, AR_PCU_MISC, AR_PCU_MIC_NEW_LOC_ENA);
    REG_WRITE(ah, AR_ISR, 0xffffffffu);
    REG_WRITE(ah, AR_RSSI_THR, INIT_RSSI_THR);
}

/**
 * Set the BSSID and association ID, as ath9k_hw_write_associd does, and
 * keep them for the next reset. A station that leaves gives zeros, which
 * also widens the filter to every beacon again.
 */
void ath9k_mac_set_bssid(struct ath9k *ah, const uint8_t bssid[6], unsigned aid)
{
    k_memcpy(ah->mac.bssid, bssid, 6);
    ah->mac.aid = (uint16_t)(aid & 0x3fff);
    REG_WRITE(ah, AR_BSS_ID0, le32(bssid));
    REG_WRITE(ah, AR_BSS_ID1, le16(bssid + 4) | (uint32_t)ah->mac.aid << AR_BSS_ID1_AID_S);
    ath9k_mac_set_filter(ah, ah->rx.filters);
}

/**
 * Enable the chip's interrupts, as ath9k_hw_set_interrupts and
 * ath9k_hw_enable_interrupts do: the masks, the global enable, the
 * asynchronous cause of the MAC and the synchronous PCIe errors, and on
 * a card with messages the message engine.
 */
void ath9k_mac_intr_enable(struct ath9k *ah)
{
    unsigned i;

    REG_WRITE(ah, AR_IMR, ah->mac.imask);
    REG_WRITE(ah, AR_IMR_S2, ah->mac.imask2);
    REG_WRITE(ah, AR_IER, AR_IER_ENABLE);
    REG_WRITE(ah, AR_INTR_ASYNC_ENABLE, AR_INTR_MAC_IRQ);
    REG_WRITE(ah, AR_INTR_ASYNC_MASK, AR_INTR_MAC_IRQ);
    REG_WRITE(ah, AR_INTR_SYNC_ENABLE, AR_INTR_SYNC_DEFAULT);
    REG_WRITE(ah, AR_INTR_SYNC_MASK, AR_INTR_SYNC_DEFAULT);
    if (!ah->mac.msi)
        return;
    REG_WRITE(ah, AR_INTR_PRIO_ASYNC_ENABLE,
              AR_INTR_PRIO_TX | AR_INTR_PRIO_RXLP | AR_INTR_PRIO_RXHP);
    REG_WRITE(ah, AR_INTR_PRIO_ASYNC_MASK, AR_INTR_PRIO_TX | AR_INTR_PRIO_RXLP | AR_INTR_PRIO_RXHP);
    if (ah->mac.msi_reg == 0)
        ah->mac.msi_reg = REG_READ(ah, AR_PCIE_MSI);
    /* ath9k writes the enable until it reads back, up to 200 times. */
    for (i = 0; i < 200; i++) {
        REG_WRITE(ah, AR_PCIE_MSI,
                  (ah->mac.msi_reg | AR_PCIE_MSI_ENABLE) & AR_PCIE_MSI_HW_INT_PENDING_ADDR_MSI_64);
        if (REG_READ(ah, AR_PCIE_MSI) & AR_PCIE_MSI_ENABLE)
            break;
    }
}

/**
 * Disable every interrupt, as ath9k_hw_kill_interrupts does; each write is
 * read back so that it has reached the chip before the caller goes on.
 */
void ath9k_mac_intr_disable(struct ath9k *ah)
{
    REG_WRITE(ah, AR_IER, AR_IER_DISABLE);
    (void)REG_READ(ah, AR_IER);
    REG_WRITE(ah, AR_INTR_ASYNC_ENABLE, 0);
    (void)REG_READ(ah, AR_INTR_ASYNC_ENABLE);
    REG_WRITE(ah, AR_INTR_SYNC_ENABLE, 0);
    (void)REG_READ(ah, AR_INTR_SYNC_ENABLE);
}

/** The TSF, as ath9k_hw_gettsf64 reads it, in at most ten tries. */
uint64_t ath9k_mac_get_tsf(struct ath9k *ah)
{
    uint32_t upper = REG_READ(ah, AR_TSF_U32), lower = 0, again;
    unsigned i;

    for (i = 0; i < 10; i++) {
        lower = REG_READ(ah, AR_TSF_L32);
        again = REG_READ(ah, AR_TSF_U32);
        if (again == upper)
            break;
        upper = again;
    }
    return (uint64_t)upper << 32 | lower;
}

/**
 * `n` modulo `d`, which is not 0, a bit at a time, since the freestanding
 * kernel has no `__umoddi3` and tick_div takes no divisor past 65535.
 */
static uint32_t mod64(uint64_t n, uint32_t d)
{
    uint64_t r = 0;
    int i;

    for (i = 63; i >= 0; i--) {
        r = r << 1 | ((n >> i) & 1);
        if (r >= d)
            r -= d;
    }
    return (uint32_t)r;
}

/** The TSF's next multiple of `period` microseconds after `tsf` and a slop
 * of two TU, as ath9k_get_next_tbtt computes it. */
static uint32_t next_tbtt(uint64_t tsf, uint32_t period)
{
    tsf += NEXT_TBTT_SLOP * 1024;
    return (uint32_t)tsf + period - mod64(tsf, period);
}

/**
 * Start or stop the station's beacon timers. Intervals are in TU; the
 * registers take microseconds. The TIM timer's period is ath9k's sleep
 * duration, 100 ms rounded up to whole beacons and at most a DTIM period,
 * when that is longer than a beacon's.
 */
void ath9k_mac_set_beacon_timers(struct ath9k *ah, unsigned interval, unsigned dtim_period)
{
    uint32_t intval, dtim, nexttbtt, nextdtim, sleep, timperiod;
    uint64_t tsf;

    if (interval == 0) {
        ah->mac.beacon_interval = 0;
        REG_CLR_BIT(ah, AR_TIMER_MODE, AR_TBTT_TIMER_EN | AR_TIM_TIMER_EN | AR_DTIM_TIMER_EN);
        return;
    }
    if (dtim_period == 0)
        dtim_period = 1;
    ah->mac.beacon_interval = (uint16_t)interval;
    ah->mac.dtim_period = (uint8_t)dtim_period;

    intval = interval * 1024;
    dtim = dtim_period * intval;
    tsf = ath9k_mac_get_tsf(ah);
    nexttbtt = next_tbtt(tsf, intval);
    nextdtim = dtim_period > 1 ? next_tbtt(tsf, dtim) : nexttbtt;
    sleep = ((100 * 1000 / 1024 + interval - 1) / interval) * intval;
    if (sleep > dtim)
        sleep = dtim;
    timperiod = sleep > intval ? sleep : intval;

    REG_WRITE(ah, AR_NEXT_TBTT_TIMER, nexttbtt);
    REG_WRITE(ah, AR_BEACON_PERIOD, intval);
    REG_WRITE(ah, AR_DMA_BEACON_PERIOD, intval);
    REG_RMW_FIELD(ah, AR_RSSI_THR, AR_RSSI_THR_BM_THR, BMISS_BEACONS);
    REG_WRITE(ah, AR_NEXT_DTIM, nextdtim - SLEEP_SLOP_US);
    REG_WRITE(ah, AR_NEXT_TIM, (timperiod == dtim ? nextdtim : nexttbtt) - SLEEP_SLOP_US);
    REG_WRITE(ah, AR_SLEEP1,
              SM(CAB_TIMEOUT_VAL << 3, AR_SLEEP1_CAB_TIMEOUT) | AR_SLEEP1_ASSUME_DTIM);
    REG_WRITE(ah, AR_SLEEP2, SM(MIN_BEACON_TIMEOUT_VAL, AR_SLEEP2_BEACON_TIMEOUT));
    REG_WRITE(ah, AR_TIM_PERIOD, timperiod);
    REG_WRITE(ah, AR_DTIM_PERIOD, dtim);
    REG_SET_BIT(ah, AR_TIMER_MODE, AR_TBTT_TIMER_EN | AR_TIM_TIMER_EN | AR_DTIM_TIMER_EN);
    REG_WRITE(ah, AR_TSFOOR_THRESHOLD, TSFOOR_THRESHOLD);
}
