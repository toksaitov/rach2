/*
 * recv.c: the AR9003's EDMA receive, two FIFOs of buffers whose status
 * the chip writes at each buffer's head, and the frames handed to the
 * station.
 *
 * It follows ath9k's recv.c and ar9003_mac.c's ath9k_hw_process_rxdesc_edma
 * (Linux v6.16). Every buffer belongs to the chip from the start of receive
 * to its stop: a buffer the service reads is pushed back at once, so each
 * FIFO is a ring the chip fills in the order the driver pushed it.
 */

#include "ath9k.h"
#include "../../../lib/kerrno.h"
#include "../../../lib/kstring.h"

/* ath9k's mac.h: an RSSI the chip could not read, and the phy error that a
 * later subframe's good delimiter shows to be harmless. */
#define RSSI_BAD            -128
#define PHYERR_OFDM_RESTART 39

/* ath9k's ATH_RSSI_EP_MULTIPLIER and ATH_RSSI_LPF_LEN: the beacons'
 * average is kept in 128ths, each beacon weighing a tenth. */
#define RSSI_EP      128
#define RSSI_LPF_LEN 10

/* The FCS the chip leaves on every frame, and TKIP's ICV, which it leaves
 * on a frame it decrypts and which the station expects removed. */
#define FCS_LEN      4
#define TKIP_ICV_LEN 4

/** The FIFO of `hp`: [0] high priority, [1] low. */
static struct ath9k_rx_fifo *fifo_of(struct ath9k *ah, int hp)
{
    return &ah->rx.fifo[hp ? 0 : 1];
}

/**
 * Allocate a page for each buffer of both FIFOs. Returns 0 or -ENOMEM,
 * having given back the pages this call took.
 */
int ath9k_rx_init(struct ath9k *ah)
{
    static const unsigned depth[2] = {ATH9K_RX_HP_DEPTH, ATH9K_RX_LP_DEPTH};
    unsigned f, i;

    for (f = 0; f < 2; f++) {
        struct ath9k_rx_fifo *fifo = &ah->rx.fifo[f];

        fifo->depth = depth[f];
        fifo->head = 0;
        fifo->count = 0;
        for (i = 0; i < fifo->depth; i++) {
            fifo->buf[i] = ath9k_dma_alloc(ah, &fifo->bus[i]);
            if (!fifo->buf[i])
                goto fail;
        }
    }
    return 0;
fail:
    for (;;) {
        while (i > 0) {
            i--;
            ath9k_dma_free(ah, ah->rx.fifo[f].buf[i]);
            ah->rx.fifo[f].buf[i] = 0;
        }
        if (f == 0)
            break;
        f--;
        i = depth[f];
    }
    return -ENOMEM;
}

/** Give back every buffer of both FIFOs, for a probe that fails after
 * ath9k_rx_init; safe twice. */
void ath9k_rx_free(struct ath9k *ah)
{
    unsigned f, i;

    for (f = 0; f < 2; f++)
        for (i = 0; i < ATH9K_RX_LP_DEPTH; i++) {
            ath9k_dma_free(ah, ah->rx.fifo[f].buf[i]);
            ah->rx.fifo[f].buf[i] = 0;
        }
}

/** Clear buffer `i`'s status, so that AR_RxDone reads clear until the
 * chip fills it, and push it to the chip. */
static void give(struct ath9k *ah, int hp, unsigned i)
{
    struct ath9k_rx_fifo *fifo = fifo_of(ah, hp);

    k_memset(fifo->buf[i], 0, sizeof(struct ar9003_rxs));
    k_barrier();
    ath9k_mac_rx_push(ah, hp, fifo->bus[i]);
}

/**
 * Push every buffer, high-priority FIFO first, and start receiving, as
 * ath_edma_start_recv does. The chip must be fresh from a reset, which
 * forgets what was pushed before. Returns 0, or -ENOMEM before
 * ath9k_rx_init.
 */
int ath9k_rx_start(struct ath9k *ah)
{
    int hp;
    unsigned i;

    for (hp = 1; hp >= 0; hp--) {
        struct ath9k_rx_fifo *fifo = fifo_of(ah, hp);

        if (fifo->depth == 0 || !fifo->buf[0])
            return -ENOMEM;
        fifo->head = 0;
        fifo->count = fifo->depth;
        for (i = 0; i < fifo->depth; i++)
            give(ah, hp, i);
        ah->rx.discard[hp ? 0 : 1] = 0;
    }
    ah->rx.running = 1;
    ath9k_mac_rx_start(ah);
    return 0;
}

/**
 * Stop receiving and take the buffers back, as ath_stoprecv does. The
 * buffers stay allocated for the next start. Returns 0, or -ETIMEDOUT
 * when the receive DMA did not stop, which asks for a reset.
 */
int ath9k_rx_stop(struct ath9k *ah)
{
    int rc;

    ah->rx.running = 0;
    rc = ath9k_mac_rx_stop(ah);
    ah->rx.fifo[0].count = 0;
    ah->rx.fifo[1].count = 0;
    return rc;
}

/** The length of the 802.11 header of a frame whose frame control is
 * `fc`, as ieee80211_hdrlen gives it. */
unsigned ath9k_header_len(unsigned fc)
{
    unsigned len;

    switch (fc & IEEE80211_FCTL_FTYPE) {
    case IEEE80211_FTYPE_DATA:
        len = 24;
        if ((fc & (IEEE80211_FCTL_TODS | IEEE80211_FCTL_FROMDS)) ==
            (IEEE80211_FCTL_TODS | IEEE80211_FCTL_FROMDS))
            len += 6;
        if (fc & IEEE80211_STYPE_QOS_DATA) {
            len += 2;
            if (fc & IEEE80211_FCTL_ORDER)
                len += 4;
        }
        return len;
    case IEEE80211_FTYPE_MGMT:
        return (fc & IEEE80211_FCTL_ORDER) ? 28 : 24;
    case IEEE80211_FTYPE_CTL:
        switch (fc & IEEE80211_FCTL_STYPE) {
        case IEEE80211_STYPE_ACK:
        case IEEE80211_STYPE_CTS:
            return 10;
        default:
            return 16;
        }
    default:
        return 24;
    }
}

/** The 64-bit TSF of a frame whose status stamped the TSF's lower half
 * `stamp`, from the TSF read at this service, as ath9k's ath_rx_tasklet
 * extends it across a wrap of the lower half either way. */
static uint64_t extend_tsf(const struct ath9k *ah, uint32_t stamp)
{
    uint64_t tsf = (uint64_t)ah->rx.tsf_high << 32 | stamp;

    if (stamp > ah->rx.tsf_low && stamp - ah->rx.tsf_low > 0x10000000u)
        tsf -= (uint64_t)1 << 32;
    if (stamp < ah->rx.tsf_low && ah->rx.tsf_low - stamp > 0x10000000u)
        tsf += (uint64_t)1 << 32;
    return tsf;
}

/**
 * Take the RSSI `rssi` of a frame into the average of the associated AP's
 * beacons that noise immunity reads, as ath9k_process_rssi does: a frame
 * received whole, a beacon whose BSSID is the station's while it is
 * associated, and an RSSI the chip could read. A reading below the noise
 * floor counts as 0, so that the average, as ath9k's ends, stays at 0 or
 * above.
 */
static void beacon_signal(struct ath9k *ah, const uint8_t *frame, unsigned len, int rssi)
{
    struct ath9k_ani *a = &ah->ani;
    unsigned i;

    if (!ah->mac.associated || rssi == RSSI_BAD || len < 24 || (frame[0] & 0xfc) != 0x80)
        return;
    for (i = 0; i < 6; i++)
        if (frame[16 + i] != ah->mac.bssid[i])
            return;
    if (rssi < 0)
        rssi = 0;
    if (a->beacon_heard)
        a->beacon_lpf = (a->beacon_lpf * (RSSI_LPF_LEN - 1) + rssi * RSSI_EP) / RSSI_LPF_LEN;
    else
        a->beacon_lpf = rssi * RSSI_EP;
    a->beacon_heard = 1;
    a->beacon_rssi = (a->beacon_lpf + RSSI_EP / 2) / RSSI_EP;
}

/**
 * Judge a frame by its status word `s11`, as ath9k_cmn_rx_accept does: a
 * bad CRC, or a phy error other than an OFDM restart with a bad CRC after
 * the delimiter, drops it; a failed decryption, which sets `*decrypt_error`,
 * or a failed Michael MIC keeps it. Each is counted. Returns 0 to keep the
 * frame, -1 to drop it.
 */
static int rx_accept(struct ath9k *ah, uint32_t s11, int *decrypt_error)
{
    if (s11 & AR_RxFrameOK)
        return 0;
    if (s11 & AR_CRCErr) {
        ah->stats.rx_crc++;
        return -1;
    }
    if ((s11 & AR_PHYErr) &&
        !(MS(s11, AR_PHYErrCode) == PHYERR_OFDM_RESTART && (s11 & AR_PostDelimCRCErr))) {
        ah->stats.rx_phy++;
        return -1;
    }
    if (s11 & AR_DecryptCRCErr) {
        ah->stats.rx_decrypt++;
        *decrypt_error = 1;
    } else if (s11 & AR_MichaelErr) {
        ah->stats.rx_mic++;
    }
    return 0;
}

/**
 * Hand one completed buffer's frame to the station, as ath9k's
 * ath9k_cmn_rx_accept and ath9k_cmn_rx_skb_postprocess judge it: a frame
 * with a bad CRC or a phy error is dropped and counted; one the chip
 * failed to decrypt goes up still protected, for the station to try in
 * software. The FCS and the padding after the header are removed, and a
 * frame the key cache decrypted is marked so, its IV and MIC left for the
 * station and TKIP's ICV removed. The Michael MIC is always checked by the
 * station, since the chip's verdict on it has false positives. A frame
 * longer than a buffer continues in the next; ath9k chains them, but
 * nothing the station asks for is that long, so the whole chain is
 * dropped. Returns 1 when a frame went up, else 0.
 */
static int deliver(struct ath9k *ah, uint8_t *buf, int hp)
{
    const struct ar9003_rxs *rxs = (const struct ar9003_rxs *)(const void *)buf;
    struct wlan_rx_status st;
    uint8_t *frame = buf + sizeof(*rxs);
    uint32_t s11 = rxs->status11;
    unsigned len = rxs->status2 & AR_DataLen, fc, hdrlen, pad;
    int decrypt_error = 0, rssi, slot = -1;
    uint8_t *discard = &ah->rx.discard[hp ? 0 : 1];

    if (*discard || (rxs->status2 & AR_RxMore)) {
        *discard = (rxs->status2 & AR_RxMore) != 0;
        ah->stats.rx_dropped++;
        return 0;
    }
    if (rx_accept(ah, s11, &decrypt_error) != 0)
        return 0;
    if (len < FCS_LEN + 10 || len > ATH9K_RX_BUF_LEN - sizeof(*rxs)) {
        ah->stats.rx_dropped++;
        return 0;
    }
    len -= FCS_LEN;
    fc = (unsigned)frame[0] | (unsigned)frame[1] << 8;
    if ((s11 & AR_KeyMiss) && !(frame[4] & 0x01))
        decrypt_error = 1;

    hdrlen = ath9k_header_len(fc);
    pad = hdrlen & 3;
    if (pad && len >= hdrlen + pad) {
        k_memmove(frame + pad, frame, hdrlen);
        frame += pad;
        len -= pad;
    }

    k_memset(&st, 0, sizeof(st));
    st.hw_key = -1;
    if ((fc & IEEE80211_FCTL_PROTECTED) && !decrypt_error) {
        if (s11 & AR_RxKeyIdxValid)
            slot = (int)MS(s11, AR_KeyIdx);
        else if (len >= hdrlen + 4)
            slot = frame[hdrlen + 3] >> 6; /* a group key, kept by its index */
        if (slot >= 0 && ah->keys.entry[slot].used == ATH9K_KEY_USED) {
            st.flags |= WLAN_RX_DECRYPTED;
            st.hw_key = slot;
            if (ah->keys.entry[slot].type == AR_KEYTABLE_TYPE_TKIP &&
                len >= hdrlen + 8 + TKIP_ICV_LEN)
                len -= TKIP_ICV_LEN;
        }
    }

    st.tsf = extend_tsf(ah, rxs->status3);
    st.frequency = ah->phy.channel.frequency;
    rssi = (int8_t)MS(rxs->status5, AR_RxRSSICombined);
    if (s11 & AR_RxFrameOK)
        beacon_signal(ah, frame, len, rssi);
    if (rssi == RSSI_BAD)
        rssi = 0;
    st.signal = (int16_t)(ah->cal.noise + rssi); /* ath9k_process_rssi */
    st.rate = (int16_t)ath9k_rate_index(ah, MS(rxs->status1, AR_RxRate), &st.rate_flags);
    if (rxs->status4 & AR_GI)
        st.rate_flags |= WLAN_RATE_SHORT_GI;

    ah->stats.rx_frames++;
    ath9k_ant_rx(ah, rxs);
    if (wlan_radio_rx(&ah->radio, frame, len, &st) != 0) {
        ah->stats.rx_dropped++;
        return 0;
    }
    return 1;
}

/**
 * Read the completed buffers of FIFO `hp` in order, hand each frame up and
 * give each buffer back, as ath_rx_tasklet with ath_edma_get_buffers does.
 * A buffer whose status does not carry the chip's vendor ID is given back
 * unread. At most a FIFO's depth of buffers are read, so that a chip
 * filling them as fast as they return cannot hold the handler. A frame the
 * station has no room for stays in the FIFO, which the chip has acknowledged
 * it from, and the tick reads on.
 */
unsigned ath9k_rx_service(struct ath9k *ah, int hp)
{
    struct ath9k_rx_fifo *fifo = fifo_of(ah, hp);
    uint8_t *held = &ah->rx.held[hp ? 0 : 1];
    unsigned n = 0, i;
    int tsf_read = 0;

    *held = 0;
    if (!ah->rx.running)
        return 0;
    for (i = 0; i < fifo->depth; i++) {
        unsigned slot = fifo->head;
        struct ar9003_rxs *rxs = (struct ar9003_rxs *)(void *)fifo->buf[slot];

        k_barrier();
        if (!(rxs->status11 & AR_RxDone))
            break;
        if (wlan_radio_rx_full(&ah->radio)) {
            *held = 1;
            break;
        }
        if (MS(rxs->ds_info, AR_DescId) == ATH9K_VENDOR_ID) {
            if (rxs->ds_info & (AR_TxRxDesc | AR_CtrlStat))
                break;
            if (!tsf_read) {
                uint64_t tsf = ath9k_mac_get_tsf(ah);

                ah->rx.tsf_high = (uint32_t)(tsf >> 32);
                ah->rx.tsf_low = (uint32_t)tsf;
                tsf_read = 1;
            }
            n += (unsigned)deliver(ah, fifo->buf[slot], hp);
        } else {
            ah->stats.rx_dropped++;
        }
        fifo->head = (slot + 1) % fifo->depth;
        give(ah, hp, slot);
    }
    return n;
}
