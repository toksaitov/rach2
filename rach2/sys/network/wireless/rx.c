/* Receive: duplicates, decryption and the replay check per TID,
 * reassembly, TKIP's Michael check and A-MSDU, then delivery; and the
 * protection of management frames, CCMP and BIP. After Linux's
 * net/mac80211/rx.c and wpa.c, and 802.11-2020 10.3.2.14 and 12.5. */
#include "data.h"
#include "../../lib/kerrno.h"
#include "../../lib/kstring.h"
#include "../../timer.h"

static const uint8_t rfc1042[6] = {0xaa, 0xaa, 0x03, 0x00, 0x00, 0x00};
static const uint8_t arp_snap[8] = {0xaa, 0xaa, 0x03, 0x00, 0x00, 0x00, 0x08, 0x06};
static const uint8_t eapol_snap[8] = {0xaa, 0xaa, 0x03, 0x00, 0x00, 0x00, 0x88, 0x8e};

/* Key Information bits of an EAPOL-Key frame. */
#define EAPOL_KEY_PAIRWISE 0x0008
#define EAPOL_KEY_ACK      0x0080
#define EAPOL_KEY_MIC      0x0100

/* What a decryption found, for the checks after reassembly. */
struct rx_crypt {
    struct wlan_data_key *key; /* NULL for a frame that came in clear */
    uint64_t pn;               /* its PN or TSC */
    int mic_stripped;          /* the radio checked TKIP's Michael MIC */
    int mic_error;             /* and found it wrong */
};

/** Whether two n-byte strings are equal. */
static int same(const uint8_t *a, const uint8_t *b, unsigned n)
{
    unsigned i;

    for (i = 0; i < n; ++i)
        if (a[i] != b[i])
            return 0;
    return 1;
}

/** Read the little-endian frame control. */
static unsigned frame_control(const uint8_t *frame)
{
    return (unsigned)frame[0] | (unsigned)frame[1] << 8;
}

/** Read a little-endian 16-bit field. */
static unsigned le16(const uint8_t *p)
{
    return (unsigned)p[0] | (unsigned)p[1] << 8;
}

/** Read the 48-bit PN of a CCMP header, or the TSC of a TKIP IV. */
static uint64_t iv_pn(const uint8_t *iv, int tkip)
{
    uint64_t pn = (uint64_t)iv[4] << 16 | (uint64_t)iv[5] << 24 | (uint64_t)iv[6] << 32 |
                  (uint64_t)iv[7] << 40;

    if (tkip)
        return pn | (uint64_t)iv[0] << 8 | iv[2];
    return pn | (uint64_t)iv[1] << 8 | iv[0];
}

/** Remove n bytes at offset `at` of a frame of *len bytes. */
static void cut(uint8_t *frame, unsigned *len, unsigned at, unsigned n)
{
    k_memmove(frame + at, frame + at + n, *len - at - n);
    *len -= n;
}

/** Note the signal and rate of a frame from the BSS, which /proc/net/wireless
 * and nl80211's station report. Quality is the signal above -110 dBm, at most
 * 70, as iw and Linux's wireless extensions scale it. */
static void note_signal(struct wlan_data *d, const struct wlan_rx_status *status)
{
    int q = status->signal + 110;
    unsigned rate = wlan_rate_value(&d->radio->band, status->rate, status->rate_flags);

    if (rate)
        d->rx_rate = (uint16_t)rate;
    if (!status->signal)
        return;
    d->counters->signal = (int32_t)status->signal * 100;
    d->counters->quality = q < 0 ? 0 : q > 70 ? 70 : (unsigned)q;
}

/**
 * Remove a frame's CCMP protection under `k`: check and strip what the
 * radio decrypted, or decrypt it here. Sets *pn. Returns 0, -EINVAL or
 * -EBADMSG; the caller checks replay.
 */
static int ccmp_open(struct wlan_data_key *k, uint8_t *frame, unsigned *len, unsigned hdrlen,
                     const struct wlan_rx_status *status, uint64_t *pn)
{
    unsigned id, fc;
    int rc;

    if (status->flags & WLAN_RX_DECRYPTED) {
        if (status->flags & WLAN_RX_IV_STRIPPED) {
            *pn = status->pn;
        } else {
            if (*len < hdrlen + WLAN_CCMP_HDR_LEN)
                return -EINVAL;
            *pn = iv_pn(frame + hdrlen, 0);
            cut(frame, len, hdrlen, WLAN_CCMP_HDR_LEN);
        }
        if (!(status->flags & WLAN_RX_MIC_STRIPPED)) {
            if (*len < hdrlen + WLAN_CCMP_MIC_LEN)
                return -EINVAL;
            *len -= WLAN_CCMP_MIC_LEN;
        }
        fc = frame_control(frame) & ~(unsigned)IEEE80211_FCTL_PROTECTED;
        frame[0] = (uint8_t)fc;
        frame[1] = (uint8_t)(fc >> 8);
        return 0;
    }
    rc = wlan_ccmp_decrypt(&k->aes, frame, len, pn, &id);
    return rc;
}

/**
 * Remove a frame's TKIP protection under `k`: the IV, and the ICV after
 * RC4, here or by the radio, which removes the ICV of what it decrypts.
 * The Michael MIC stays on the MSDU for the check after reassembly. Sets
 * *tsc. Returns 0, -EINVAL or -EBADMSG for a wrong ICV.
 */
static int tkip_open(struct wlan_data_key *k, uint8_t *frame, unsigned *len, unsigned hdrlen,
                     const struct wlan_rx_status *status, uint64_t *tsc)
{
    uint8_t rc4key[16];
    uint16_t ttak[5];
    unsigned n, fc;
    uint32_t icv;
    int ok;

    if ((status->flags & WLAN_RX_DECRYPTED) && (status->flags & WLAN_RX_IV_STRIPPED)) {
        *tsc = status->pn;
    } else {
        if (*len < hdrlen + 8 + 4)
            return -EINVAL;
        *tsc = iv_pn(frame + hdrlen, 1);
        if (!(status->flags & WLAN_RX_DECRYPTED)) {
            n = *len - hdrlen - 8;
            wlan_tkip_mix1(k->key.data, frame + 10, (uint32_t)(*tsc >> 16), ttak);
            wlan_tkip_mix2(k->key.data, ttak, (uint16_t)*tsc, rc4key);
            wlan_tkip_rc4(rc4key, frame + hdrlen + 8, n);
            icv = wlan_crc32(frame + hdrlen + 8, n - 4);
            ok = frame[*len - 4] == (uint8_t)icv && frame[*len - 3] == (uint8_t)(icv >> 8) &&
                 frame[*len - 2] == (uint8_t)(icv >> 16) && frame[*len - 1] == (uint8_t)(icv >> 24);
            k_memset(ttak, 0, sizeof(ttak));
            k_memset(rc4key, 0, sizeof(rc4key));
            if (!ok)
                return -EBADMSG;
            *len -= 4;
        }
        cut(frame, len, hdrlen, 8);
    }
    fc = frame_control(frame) & ~(unsigned)IEEE80211_FCTL_PROTECTED;
    frame[0] = (uint8_t)fc;
    frame[1] = (uint8_t)(fc >> 8);
    return 0;
}

/**
 * Remove a data frame's protection and check its counter against TID
 * index `idx`. CCMP commits the counter now; TKIP's waits for Michael.
 * Returns 0 or the reason to drop it, which the counters record.
 */
/**
 * Whether pn, on a frame opened under the pairwise key k, continues the
 * count of the key k replaced. An AP that encrypts what it had queued with
 * the new key sends such a number just before it starts again from 1: taken,
 * it would set k's counter far ahead of the AP's, and every frame would be a
 * replay until the AP caught up. The sender keeps one count across TIDs (tx.c
 * as well), so the distance is from the highest number seen on any: a true
 * number of k cannot lie this far past it unless a window of frames was lost,
 * and then only for the seconds of WLAN_DATA_REKEY_SETTLE. The AP's queue
 * runs on past old_pn by however many frames it held, hundreds under a
 * download, so nothing above old_pn bounds the test.
 */
static int pn_of_old_key(const struct wlan_data_key *k, uint64_t pn)
{
    uint64_t high = 0;
    unsigned tid;

    if (tick_count() >= k->old_until)
        return 0;
    for (tid = 0; tid < WLAN_NUM_TIDS; tid++)
        if (k->key.rx_pn[tid] > high)
            high = k->key.rx_pn[tid];
    return pn > high + WLAN_DATA_REKEY_JUMP && pn + WLAN_DATA_REKEY_NEAR > k->old_pn;
}

static int rx_decrypt(struct wlan_data *d, uint8_t *frame, unsigned *len, unsigned hdrlen,
                      const struct wlan_rx_status *status, int group, unsigned idx,
                      struct rx_crypt *c)
{
    struct wlan_data_key *k;
    unsigned keyid;
    int rc;

    if ((status->flags & WLAN_RX_DECRYPTED) && (status->flags & WLAN_RX_IV_STRIPPED)) {
        keyid = status->key_index;
    } else {
        if (*len < hdrlen + 8 || !(frame[hdrlen + 3] & 0x20)) {
            d->counters->rx_decrypt_errors++;
            return -EINVAL;
        }
        keyid = frame[hdrlen + 3] >> 6;
    }
    k = group ? wlan_data_group_key(d, keyid) : wlan_data_pairwise_key(d, keyid);
    if (!k || k->key.cipher == WLAN_CIPHER_SUITE_AES_CMAC) {
        d->counters->rx_decrypt_errors++;
        return -ENOKEY;
    }
    /* The frame was opened under a key since replaced, and carries that
     * key's number: taken here it would set this key's counter ahead of the
     * AP's, and every frame would be a replay until the AP caught up. */
    if (!group && (status->flags & WLAN_RX_DECRYPTED) && status->hw_key >= 0 &&
        status->hw_key != k->key.hw_slot) {
        d->counters->rx_dropped++;
        return -ESTALE;
    }
    if (k->key.cipher == WLAN_CIPHER_SUITE_TKIP)
        rc = tkip_open(k, frame, len, hdrlen, status, &c->pn);
    else
        rc = ccmp_open(k, frame, len, hdrlen, status, &c->pn);
    if (rc) {
        d->counters->rx_decrypt_errors++;
        return rc;
    }
    if (c->pn <= k->key.rx_pn[idx]) {
        d->counters->rx_replays++;
        return -ESTALE;
    }
    if (!group && pn_of_old_key(k, c->pn)) {
        d->counters->rx_dropped++;
        if (c->pn > k->old_pn + WLAN_DATA_REKEY_NEAR)
            d->counters->rx_old_far++;
        return -ESTALE;
    }
    if (k->key.cipher == WLAN_CIPHER_SUITE_CCMP)
        k->key.rx_pn[idx] = c->pn;
    c->key = k;
    c->mic_stripped = (status->flags & WLAN_RX_DECRYPTED) && (status->flags & WLAN_RX_MIC_STRIPPED);
    c->mic_error = (status->flags & WLAN_RX_MIC_ERROR) != 0;
    return 0;
}

/** Report a Michael MIC failure under `k` to user space, which counts them
 * for TKIP's countermeasures (802.11-2020 12.5.2.4); drop the frame. */
static int mic_failure(struct wlan_data *d, const uint8_t *frame, const struct wlan_data_key *k)
{
    struct wlan_event ev;

    d->counters->rx_mic_errors++;
    if (d->event) {
        k_memset(&ev, 0, sizeof(ev));
        ev.type = WLAN_EVENT_MIC_FAILURE;
        k_memcpy(ev.peer, frame + 10, 6);
        ev.key_index = k->key.index;
        ev.pairwise = !(frame[4] & 1);
        ev.frequency = d->frequency;
        d->event(d->wlan, &ev);
    }
    return -EBADMSG;
}

/** Hand on one MSDU, counting it. */
static int deliver(struct wlan_data *d, const uint8_t *frame, unsigned len)
{
    d->counters->rx_packets++;
    d->counters->rx_bytes += len;
    return d->deliver ? d->deliver(d, frame, len) : 0;
}

/**
 * Split an A-MSDU into its subframes (802.11-2020 9.3.2.2.2) and hand on
 * each as a three-address frame of its own. A first subframe whose DA is
 * an LLC/SNAP header drops the whole frame: it is a plain MSDU whose
 * A-MSDU bit an attacker set, since that bit is outside CCMP's protection
 * (Vanhoef's aggregation attack, 2021).
 */
static int rx_amsdu(struct wlan_data *d, const uint8_t *frame, unsigned len, unsigned hdrlen)
{
    unsigned off = hdrlen, sub, n;
    const uint8_t *s;

    if (len < off + 14 || same(frame + off, rfc1042, 6)) {
        d->counters->rx_dropped++;
        return -EINVAL;
    }
    while (off + 14 <= len) {
        s = frame + off;
        sub = (unsigned)s[12] << 8 | s[13];
        if (off + 14 + sub > len) {
            d->counters->rx_dropped++;
            return -EINVAL;
        }
        n = IEEE80211_HDRLEN + sub;
        if (((s[0] & 1) || same(s, d->radio->mac, 6)) && !same(s + 6, d->radio->mac, 6) &&
            n <= sizeof(d->sub)) {
            k_memcpy(d->sub, frame, IEEE80211_HDRLEN);
            d->sub[0] = (uint8_t)(IEEE80211_FTYPE_DATA | IEEE80211_STYPE_DATA);
            d->sub[1] = (uint8_t)(IEEE80211_FCTL_FROMDS >> 8);
            k_memcpy(d->sub + 4, s, 6);
            k_memcpy(d->sub + 16, s + 6, 6);
            k_memcpy(d->sub + IEEE80211_HDRLEN, s + 14, sub);
            (void)deliver(d, d->sub, n);
        } else {
            d->counters->rx_dropped++;
        }
        off += (14 + sub + 3) & ~3u;
    }
    return 0;
}

/** A free reassembly entry, else one that timed out, else the oldest. */
static struct wlan_defrag *defrag_slot(struct wlan_data *d, uint64_t now)
{
    struct wlan_defrag *e, *best = &d->defrag[0];
    unsigned i;

    for (i = 0; i < WLAN_DATA_DEFRAG; ++i) {
        e = &d->defrag[i];
        if (!e->len || now - e->started >= WLAN_DATA_FRAG_TIMEOUT)
            return e;
        if (e->started < best->started)
            best = e;
    }
    return best;
}

/**
 * Take a fragment, whose header is *hdrlen bytes, into reassembly
 * (802.11-2020 10.6). Returns 1 and points *frame, *len and *hdrlen at the
 * whole MSDU and its first fragment's header once its last fragment came, 0
 * while more are due, or a negative errno for a fragment that cannot
 * belong. Every fragment must come under the same key with consecutive PNs,
 * so that fragments of different frames or keys cannot be mixed (the mixed
 * key and fragment cache attacks, 2021), and with a header of the same
 * length, so that the MSDU's body is where the first header ends.
 */
static int defrag(struct wlan_data *d, uint8_t **frame, unsigned *len, unsigned *hdrlen,
                  unsigned idx, const struct rx_crypt *c)
{
    uint8_t *f = *frame;
    unsigned h = *hdrlen;
    unsigned seqctl = le16(f + 22), seq = seqctl >> 4, frag = seqctl & 0xf;
    unsigned more = frame_control(f) & IEEE80211_FCTL_MOREFRAGS, gen = c->key ? c->key->gen : 0;
    uint64_t now = tick_count();
    struct wlan_defrag *e = 0;
    unsigned i, fc;

    d->counters->rx_fragments++;
    if (frag == 0) {
        e = defrag_slot(d, now);
        if (*len > sizeof(e->frame))
            return -EINVAL;
        k_memcpy(e->frame, f, *len);
        e->len = *len;
        e->hdrlen = h;
        e->seq = (uint16_t)seq;
        e->tid = (uint8_t)idx;
        e->frag = 0;
        e->pn = c->pn;
        e->key_gen = gen;
        e->started = now;
        return 0;
    }
    for (i = 0; i < WLAN_DATA_DEFRAG; ++i)
        if (d->defrag[i].len && d->defrag[i].seq == seq && d->defrag[i].tid == idx)
            e = &d->defrag[i];
    if (!e)
        return -EINVAL;
    if (now - e->started >= WLAN_DATA_FRAG_TIMEOUT || frag != e->frag + 1u || gen != e->key_gen ||
        h != e->hdrlen ||
        (c->key && c->key->key.cipher == WLAN_CIPHER_SUITE_CCMP && c->pn != e->pn + 1) ||
        (c->key && c->pn <= e->pn) || e->len + *len - h > sizeof(e->frame)) {
        e->len = 0;
        return -EINVAL;
    }
    k_memcpy(e->frame + e->len, f + h, *len - h);
    e->len += *len - h;
    e->frag = (uint8_t)frag;
    e->pn = c->pn;
    if (more)
        return 0;
    fc = frame_control(e->frame) & ~(unsigned)IEEE80211_FCTL_MOREFRAGS;
    e->frame[0] = (uint8_t)fc;
    e->frame[1] = (uint8_t)(fc >> 8);
    e->frame[22] &= 0xf0;
    *frame = e->frame;
    *len = e->len;
    *hdrlen = e->hdrlen;
    /* The entry is free again; its bytes stay until the next fragment. */
    e->len = 0;
    return 1;
}

/**
 * Check the Michael MIC of the whole TKIP MSDU of *len bytes at frame, whose
 * header is hdrlen bytes long, under the key c names, unless the radio
 * checked it, and cut the MIC from *len. Returns 0, -EINVAL for an MSDU too
 * short to hold the MIC, or mic_failure's result for a wrong MIC.
 */
static int rx_michael(struct wlan_data *d, const uint8_t *frame, unsigned *len, unsigned hdrlen,
                      unsigned priority, const struct rx_crypt *c)
{
    uint8_t mic[WLAN_TKIP_MIC_LEN];

    if (c->mic_error)
        return mic_failure(d, frame, c->key);
    if (c->mic_stripped)
        return 0;
    if (*len < hdrlen + WLAN_TKIP_MIC_LEN) {
        d->counters->rx_dropped++;
        return -EINVAL;
    }
    /* From the AP, DA is Address 1 and SA Address 3. */
    wlan_tkip_michael(c->key->key.data + 24, frame + 4, frame + 16, priority, frame + hdrlen,
                      *len - hdrlen - WLAN_TKIP_MIC_LEN, mic);
    if (!same(mic, frame + *len - WLAN_TKIP_MIC_LEN, WLAN_TKIP_MIC_LEN))
        return mic_failure(d, frame, c->key);
    *len -= WLAN_TKIP_MIC_LEN;
    return 0;
}

/** Take a frame from the BSS opened under a key, which came in at tick
 * `arrived`, as proof that the AP is on the connection's channel: only a
 * frame received on that channel counts, and from its reception, not its
 * delivery, so that one held across a channel move proves nothing there. */
static void seal(struct wlan_data *d, const struct wlan_rx_status *status, uint64_t arrived)
{
    if (status->frequency == d->frequency && arrived > d->sealed)
        d->sealed = arrived;
}

/** Whether the `len` bytes at `p`, LLC/SNAP first, are an ARP reply. */
static int is_arp_reply(const uint8_t *p, unsigned len)
{
    return len >= 8 + 8 && same(p, arp_snap, 8) && p[14] == 0 && p[15] == 2;
}

/**
 * Whether the `frame` of `len` bytes, opened under a key, carries an ARP
 * reply, operation 2 of RFC 826, alone or as any subframe of an A-MSDU. An
 * AP that sends one has passed on our request, which is more than its own
 * frames or its radio's ACKs can show.
 */
static int arp_reply(const uint8_t *frame, unsigned len, unsigned hdrlen, int qos)
{
    unsigned off = hdrlen, sub;

    if (!qos || !(frame[IEEE80211_HDRLEN] & IEEE80211_QOS_CTL_A_MSDU_PRESENT))
        return is_arp_reply(frame + hdrlen, len - hdrlen);
    while (off + 14 <= len) {
        sub = (unsigned)frame[off + 12] << 8 | frame[off + 13];
        if (off + 14 + sub > len)
            return 0;
        if (is_arp_reply(frame + off + 14, sub))
            return 1;
        off += (14 + sub + 3) & ~3u;
    }
    return 0;
}

/**
 * Note the start of a pairwise key's renewal in the `frame` of `len` bytes,
 * opened under a key, so that nobody but the AP can start the wait: message 1
 * of a 4-way handshake, an EAPOL-Key frame, 802.11-2020 Figure 12-35, with
 * the Pairwise and Ack bits and no MIC, on a connection that has a pairwise
 * key. A group key's messages have no Pairwise bit, and a first join has no
 * key. A repeat keeps the first message's tick, and a renewal whose key went
 * in and that still stands, as on an idle link, gives way to the new one.
 */
static void note_renewal(struct wlan_data *d, const uint8_t *frame, unsigned len, unsigned hdrlen,
                         uint64_t arrived)
{
    const uint8_t *p = frame + hdrlen;
    unsigned info;

    if ((frame[4] & 1) || len < hdrlen + 8 + 7 || !same(p, eapol_snap, 8) || p[9] != 3 ||
        !wlan_data_pairwise_key(d, d->tx_pairwise))
        return;
    info = (unsigned)p[13] << 8 | p[14];
    if ((info & (EAPOL_KEY_PAIRWISE | EAPOL_KEY_ACK | EAPOL_KEY_MIC)) !=
        (EAPOL_KEY_PAIRWISE | EAPOL_KEY_ACK))
        return;
    if (d->renewed)
        wlan_data_renewal_end(d);
    if (!d->renewing)
        d->renewing = arrived;
}

int wlan_data_rx_mpdu(struct wlan_data *d, uint8_t *frame, unsigned len,
                      const struct wlan_rx_status *status, uint64_t arrived)
{
    unsigned fc = frame_control(frame), hdrlen = wlan_data_hdrlen(frame, len);
    unsigned seqctl, idx = WLAN_NON_QOS_TID, priority = 0;
    int group = frame[4] & 1, qos = (fc & IEEE80211_STYPE_QOS_DATA) != 0, rc;
    struct rx_crypt c;

    if (!hdrlen)
        return -EINVAL;
    if (qos) {
        priority = frame[IEEE80211_HDRLEN] & IEEE80211_QOS_CTL_TID_MASK;
        idx = priority;
    }
    seqctl = le16(frame + 22);
    if (!group) {
        if ((fc & IEEE80211_FCTL_RETRY) && d->rx_seq[idx] == seqctl) {
            d->counters->rx_duplicates++;
            return -EALREADY;
        }
        d->rx_seq[idx] = (uint16_t)seqctl;
    }

    k_memset(&c, 0, sizeof(c));
    if ((fc & IEEE80211_FCTL_PROTECTED) || (status->flags & WLAN_RX_DECRYPTED)) {
        rc = rx_decrypt(d, frame, &len, hdrlen, status, group, idx, &c);
        if (rc)
            return rc;
    } else if (d->privacy && ((seqctl & 0xf) || (fc & IEEE80211_FCTL_MOREFRAGS) ||
                              len < hdrlen + 8 || !same(frame + hdrlen, eapol_snap, 8))) {
        /* A protected network takes nothing in clear but EAPOL, and EAPOL
         * only whole. */
        d->counters->rx_dropped++;
        return -EACCES;
    }

    if ((seqctl & 0xf) || (fc & IEEE80211_FCTL_MOREFRAGS)) {
        if (group) {
            d->counters->rx_dropped++;
            return -EINVAL;
        }
        rc = defrag(d, &frame, &len, &hdrlen, idx, &c);
        if (rc <= 0) {
            if (rc < 0)
                d->counters->rx_dropped++;
            return rc;
        }
    }

    if (c.key && c.key->key.cipher == WLAN_CIPHER_SUITE_TKIP) {
        rc = rx_michael(d, frame, &len, hdrlen, priority, &c);
        if (rc)
            return rc;
        c.key->key.rx_pn[idx] = c.pn;
    }
    if (c.key) {
        seal(d, status, arrived);
        wlan_data_renewal_proved(d, c.key, arp_reply(frame, len, hdrlen, qos));
        note_renewal(d, frame, len, hdrlen, arrived);
    }

    if (qos && (frame[IEEE80211_HDRLEN] & IEEE80211_QOS_CTL_A_MSDU_PRESENT))
        return rx_amsdu(d, frame, len, hdrlen);
    return deliver(d, frame, len);
}

/** Take a BlockAckReq (802.11-2020 9.3.1.7) from the AP: its starting
 * sequence number moves the TID's reordering window. */
static int rx_bar(struct wlan_data *d, const uint8_t *frame, unsigned len)
{
    unsigned control;

    if (len < 20 || !same(frame + 4, d->radio->mac, 6) || !same(frame + 10, d->bssid, 6))
        return -EINVAL;
    control = le16(frame + 16);
    if (control & 0x0002)
        return -EINVAL; /* Multi-TID */
    wlan_ba_rx_bar(d, control >> 12, le16(frame + 18) >> 4);
    return 0;
}

int wlan_data_rx(struct wlan_data *d, uint8_t *frame, unsigned len,
                 const struct wlan_rx_status *status)
{
    unsigned fc, hdrlen, tid;
    int rc;

    if (!d || !frame || !status || len > IEEE80211_MAX_FRAME_LEN)
        return -EINVAL;
    if (!d->connected)
        return -ENOTCONN;
    if (len < 10)
        return -EINVAL;
    fc = frame_control(frame);
    if ((fc & (IEEE80211_FCTL_VERS | IEEE80211_FCTL_FTYPE | IEEE80211_FCTL_STYPE)) ==
        (IEEE80211_FTYPE_CTL | IEEE80211_STYPE_BACK_REQ))
        return rx_bar(d, frame, len);
    hdrlen = wlan_data_hdrlen(frame, len);
    if ((fc & (IEEE80211_FCTL_VERS | IEEE80211_FCTL_FTYPE)) != IEEE80211_FTYPE_DATA || !hdrlen ||
        (fc & (IEEE80211_FCTL_TODS | IEEE80211_FCTL_FROMDS)) != IEEE80211_FCTL_FROMDS ||
        !same(frame + 10, d->bssid, 6))
        return -EINVAL;
    if (!(frame[4] & 1) ? !same(frame + 4, d->radio->mac, 6) : same(frame + 16, d->radio->mac, 6))
        return -EINVAL; /* not ours, or our own group frame come back */
    note_signal(d, status);
    if (!(frame[4] & 1))
        d->heard = tick_count();
    if (fc & IEEE80211_STYPE_NULLFUNC)
        return 0;
    if ((fc & IEEE80211_STYPE_QOS_DATA) && !(frame[4] & 1)) {
        tid = frame[IEEE80211_HDRLEN] & IEEE80211_QOS_CTL_TID_MASK;
        if (tid < 8 && d->ba[tid].active) {
            rc = wlan_ba_rx_reorder(d, frame, len, status, tid);
            if (rc != 1)
                return rc;
        }
    }
    return wlan_data_rx_mpdu(d, frame, len, status, tick_count());
}

int wlan_data_robust_mgmt(const uint8_t *frame, unsigned len)
{
    unsigned fc, hdrlen;

    if (len < IEEE80211_HDRLEN)
        return 0;
    fc = frame_control(frame);
    if ((fc & IEEE80211_FCTL_FTYPE) != IEEE80211_FTYPE_MGMT)
        return 0;
    switch (fc & IEEE80211_FCTL_STYPE) {
    case IEEE80211_STYPE_DEAUTH:
    case IEEE80211_STYPE_DISASSOC:
        return 1;
    case IEEE80211_STYPE_ACTION:
    case IEEE80211_STYPE_ACTION + 0x10: /* Action No Ack */
        if (fc & IEEE80211_FCTL_PROTECTED)
            return 1;
        hdrlen = wlan_data_hdrlen(frame, len);
        if (!hdrlen || len <= hdrlen)
            return 0;
        switch (frame[hdrlen]) {
        case 4:   /* Public */
        case 7:   /* HT */
        case 11:  /* Unprotected WNM */
        case 15:  /* Self-protected */
        case 20:  /* Unprotected DMG */
        case 21:  /* VHT */
        case 22:  /* Unprotected S1G */
        case 127: /* Vendor-specific */
            return 0;
        default:
            return frame[hdrlen] < 128;
        }
    default:
        return 0;
    }
}

/** Whether any integrity key is installed. */
static int have_igtk(struct wlan_data *d)
{
    unsigned i;

    for (i = WLAN_DATA_IGTK_FIRST; i < WLAN_KEY_SLOTS; ++i)
        if (wlan_data_group_key(d, i))
            return 1;
    return 0;
}

/**
 * Open the unicast robust management frame of *len bytes at frame, whose
 * header is hdrlen bytes long, under the pairwise CCMP key its key ID names,
 * and check its counter against the last management frame's. Returns
 * WLAN_DATA_PROTECTED with *len cut to the plaintext, -EINVAL for a frame
 * that may not be protected so or whose CCMP header is cut short, -ENOKEY,
 * -ESTALE for a replay, or ccmp_open's error; the counters record a failure.
 */
static int rx_mgmt_ccmp(struct wlan_data *d, uint8_t *frame, unsigned *len, unsigned hdrlen,
                        const struct wlan_rx_status *status)
{
    unsigned n = *len, keyid;
    struct wlan_data_key *k;
    uint64_t pn;
    int rc;

    if ((frame[4] & 1) || !wlan_data_robust_mgmt(frame, n))
        return -EINVAL;
    if ((status->flags & WLAN_RX_DECRYPTED) && (status->flags & WLAN_RX_IV_STRIPPED)) {
        keyid = status->key_index;
    } else {
        if (n < hdrlen + WLAN_CCMP_HDR_LEN || !(frame[hdrlen + 3] & 0x20))
            return -EINVAL;
        keyid = frame[hdrlen + 3] >> 6;
    }
    k = wlan_data_pairwise_key(d, keyid);
    if (!k || k->key.cipher != WLAN_CIPHER_SUITE_CCMP) {
        d->counters->rx_decrypt_errors++;
        return -ENOKEY;
    }
    /* As rx_decrypt: opened under a key since replaced, its number is that
     * key's, and it shows nothing of this one. */
    if ((status->flags & WLAN_RX_DECRYPTED) && status->hw_key >= 0 &&
        status->hw_key != k->key.hw_slot) {
        d->counters->rx_dropped++;
        return -ESTALE;
    }
    rc = ccmp_open(k, frame, &n, hdrlen, status, &pn);
    if (rc) {
        d->counters->rx_decrypt_errors++;
        return rc;
    }
    if (pn <= k->rx_mgmt_pn) {
        d->counters->rx_replays++;
        return -ESTALE;
    }
    k->rx_mgmt_pn = pn;
    wlan_data_renewal_proved(d, k, 0);
    *len = n;
    return WLAN_DATA_PROTECTED;
}

/**
 * Check the BIP MMIE of the group-addressed robust management frame of *len
 * bytes at frame, whose header is hdrlen bytes long, under the IGTK its key
 * ID names, and its counter against that key's. Returns
 * WLAN_DATA_UNPROTECTED for a frame BIP does not cover or, without an IGTK,
 * cannot; WLAN_DATA_PROTECTED with *len cut before the MMIE; -ENOKEY,
 * -ESTALE for a replay, or wlan_bip_check's error; the counters record a
 * failure.
 */
static int rx_mgmt_bip(struct wlan_data *d, uint8_t *frame, unsigned *len, unsigned hdrlen)
{
    unsigned n = *len, keyid;
    struct wlan_data_key *k;
    uint64_t pn;
    int rc;

    if (!(frame[4] & 1) || !wlan_data_robust_mgmt(frame, n) || !have_igtk(d))
        return WLAN_DATA_UNPROTECTED;
    if (n < hdrlen + IEEE80211_MMIE_LEN || frame[n - IEEE80211_MMIE_LEN] != WLAN_EID_MMIE ||
        frame[n - IEEE80211_MMIE_LEN + 1] != IEEE80211_MMIE_LEN - 2)
        return WLAN_DATA_UNPROTECTED;
    keyid = le16(frame + n - IEEE80211_MMIE_LEN + 2);
    k = keyid < WLAN_DATA_IGTK_FIRST ? 0 : wlan_data_group_key(d, keyid);
    if (!k || k->key.cipher != WLAN_CIPHER_SUITE_AES_CMAC) {
        d->counters->rx_decrypt_errors++;
        return -ENOKEY;
    }
    rc = wlan_bip_check(&k->aes, frame, &n, &pn, &keyid);
    if (rc) {
        d->counters->rx_decrypt_errors++;
        return rc;
    }
    if (pn <= k->key.rx_pn[WLAN_NON_QOS_TID]) {
        d->counters->rx_replays++;
        return -ESTALE;
    }
    k->key.rx_pn[WLAN_NON_QOS_TID] = pn;
    *len = n;
    return WLAN_DATA_PROTECTED;
}

int wlan_data_rx_mgmt(struct wlan_data *d, uint8_t *frame, unsigned *len,
                      const struct wlan_rx_status *status)
{
    unsigned fc, hdrlen, n;
    int rc;

    if (!d || !frame || !len || !status || *len > IEEE80211_MAX_FRAME_LEN)
        return -EINVAL;
    n = *len;
    hdrlen = wlan_data_hdrlen(frame, n);
    fc = hdrlen ? frame_control(frame) : 0;
    if (!hdrlen || (fc & (IEEE80211_FCTL_VERS | IEEE80211_FCTL_FTYPE)) != IEEE80211_FTYPE_MGMT)
        return -EINVAL;
    if (!d->connected || !same(frame + 10, d->bssid, 6) ||
        (fc & IEEE80211_FCTL_STYPE) == IEEE80211_STYPE_BEACON) {
        if (fc & IEEE80211_FCTL_PROTECTED)
            return -ENOKEY;
        return WLAN_DATA_UNPROTECTED;
    }
    note_signal(d, status);
    if ((fc & IEEE80211_FCTL_PROTECTED) || (status->flags & WLAN_RX_DECRYPTED))
        rc = rx_mgmt_ccmp(d, frame, len, hdrlen, status);
    else
        rc = rx_mgmt_bip(d, frame, len, hdrlen);
    if (rc == WLAN_DATA_PROTECTED)
        seal(d, status, tick_count());
    return rc;
}
