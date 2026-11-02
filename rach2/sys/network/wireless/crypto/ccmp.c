/*
 * ccmp.c: CCMP-128, the AES-CCM protection of WPA2 data and robust
 * management frames, as IEEE 802.11-2020 12.5.3 defines it.
 *
 * CCM here has an 8-byte MIC and a 2-byte length (M = 8, L = 2), a 13-byte
 * nonce of priority, transmitter address and PN, and as additional
 * authenticated data the MAC header with its changeable bits masked. The
 * masks follow 12.5.3.3.3 and agree with Linux's ccmp_special_blocks
 * (net/mac80211/wpa.c) and hostap's ccmp_aad_nonce (src/common/ccmp.c):
 * the QoS Control field keeps only its TID, since no Rach2 station
 * negotiates SPP A-MSDUs.
 */
#include "wcrypto_int.h"
#include "../../../lib/kerrno.h"

#define CCMP_PN_MAX  0xffffffffffffULL
#define CCMP_EXT_IV  0x20
#define CCMP_AAD_MAX 30
#define CCM_FLAGS_B0 0x59 /* Adata, M = 8, L = 2 */
#define CCM_FLAGS_A  0x01 /* L = 2 */

/* What the MAC header of a frame to be protected says about CCMP. */
struct ccmp_hdr {
    unsigned hdrlen;           /* MAC header bytes, HT Control included */
    unsigned qos;              /* 1 when a QoS Control field is present */
    unsigned a4;               /* 1 when Address 4 is present */
    unsigned mgmt;             /* 1 for a management frame */
    uint8_t aad[CCMP_AAD_MAX]; /* masked header, without its length */
    unsigned aad_len;          /* 22, 24, 28 or 30 */
    uint8_t nonce[13];         /* flags, A2, then the PN once known */
};

/**
 * Parse a frame's MAC header and fill everything of h but the PN.
 * Return 0, or -EINVAL for a frame shorter than its header, a version
 * other than 0, or a control or extension frame, which CCMP never covers.
 */
static int ccmp_parse(const uint8_t *frame, unsigned len, struct ccmp_hdr *h)
{
    unsigned fc, type, i, o;
    uint8_t tid;

    if (len < IEEE80211_HDRLEN)
        return -EINVAL;
    fc = (unsigned)frame[0] | (unsigned)frame[1] << 8;
    type = fc & IEEE80211_FCTL_FTYPE;
    if ((fc & IEEE80211_FCTL_VERS) != 0)
        return -EINVAL;
    if (type != IEEE80211_FTYPE_DATA && type != IEEE80211_FTYPE_MGMT)
        return -EINVAL;
    h->mgmt = type == IEEE80211_FTYPE_MGMT;
    h->a4 = !h->mgmt && (fc & (IEEE80211_FCTL_TODS | IEEE80211_FCTL_FROMDS)) ==
                            (IEEE80211_FCTL_TODS | IEEE80211_FCTL_FROMDS);
    h->qos = !h->mgmt && (fc & IEEE80211_STYPE_QOS_DATA) != 0;
    h->hdrlen = IEEE80211_HDRLEN + (h->a4 ? IEEE80211_ADDR_LEN : 0);
    tid = 0;
    if (h->qos) {
        h->hdrlen += IEEE80211_QOS_CTL_LEN;
        if (fc & IEEE80211_FCTL_ORDER)
            h->hdrlen += IEEE80211_HT_CTL_LEN;
    } else if (h->mgmt && (fc & IEEE80211_FCTL_ORDER)) {
        h->hdrlen += IEEE80211_HT_CTL_LEN;
    }
    if (len < h->hdrlen)
        return -EINVAL;

    fc &= ~(unsigned)(IEEE80211_FCTL_RETRY | IEEE80211_FCTL_PM | IEEE80211_FCTL_MOREDATA);
    if (!h->mgmt)
        fc &= ~0x0070u; /* subtype bits 4 to 6 of a data frame */
    if (h->qos)
        fc &= ~(unsigned)IEEE80211_FCTL_ORDER;
    fc |= IEEE80211_FCTL_PROTECTED;
    h->aad[0] = (uint8_t)fc;
    h->aad[1] = (uint8_t)(fc >> 8);
    for (i = 0; i < 3 * IEEE80211_ADDR_LEN; i++)
        h->aad[2 + i] = frame[4 + i];
    h->aad[20] = (uint8_t)(frame[22] & IEEE80211_SCTL_FRAG);
    h->aad[21] = 0;
    o = 22;
    if (h->a4) {
        for (i = 0; i < IEEE80211_ADDR_LEN; i++)
            h->aad[o + i] = frame[IEEE80211_HDRLEN + i];
        o += IEEE80211_ADDR_LEN;
    }
    if (h->qos) {
        tid = (uint8_t)(frame[IEEE80211_HDRLEN + (h->a4 ? IEEE80211_ADDR_LEN : 0)] &
                        IEEE80211_QOS_CTL_TID_MASK);
        h->aad[o] = tid;
        h->aad[o + 1] = 0;
        o += 2;
    }
    h->aad_len = o;

    h->nonce[0] = (uint8_t)(tid | (h->mgmt ? 0x10 : 0));
    for (i = 0; i < IEEE80211_ADDR_LEN; i++)
        h->nonce[1 + i] = frame[10 + i];
    return 0;
}

/** Put the PN into the nonce, most significant byte first. */
static void ccmp_nonce_pn(struct ccmp_hdr *h, uint64_t pn)
{
    int i;

    for (i = 0; i < 6; i++)
        h->nonce[7 + i] = (uint8_t)(pn >> (8 * (5 - i)));
}

/** Fill a counter block A_i for the nonce. */
static void ccm_ctr(uint8_t a[16], const uint8_t nonce[13], unsigned i)
{
    unsigned j;

    a[0] = CCM_FLAGS_A;
    for (j = 0; j < 13; j++)
        a[1 + j] = nonce[j];
    a[14] = (uint8_t)(i >> 8);
    a[15] = (uint8_t)i;
}

/**
 * Run CCM over n bytes at data and return the 8-byte MIC in tag.
 * Encrypt the bytes in place when write is set and decrypt is clear;
 * with decrypt set, authenticate the plaintext that the ciphertext gives
 * and change nothing. The CBC-MAC block of one step and the counter
 * block of the next share each pass of AES. n is at most 65535.
 */
static void ccm_run(const struct wlan_aes128 *ctx, const struct ccmp_hdr *h, uint8_t *data,
                    unsigned n, int decrypt, int write, uint8_t tag[8])
{
    uint8_t x[16], ks[16], ad[32];
    unsigned i, j, m, off, blk;

    x[0] = CCM_FLAGS_B0;
    for (j = 0; j < 13; j++)
        x[1 + j] = h->nonce[j];
    x[14] = (uint8_t)(n >> 8);
    x[15] = (uint8_t)n;
    wlan_aes128_encrypt(ctx, x, x);

    ad[0] = 0;
    ad[1] = (uint8_t)h->aad_len;
    for (j = 0; j < 30; j++)
        ad[2 + j] = j < h->aad_len ? h->aad[j] : 0;
    for (j = 0; j < 16; j++)
        x[j] ^= ad[j];
    wlan_aes128_encrypt(ctx, x, x);
    for (j = 0; j < 16; j++)
        x[j] ^= ad[16 + j];
    ccm_ctr(ks, h->nonce, n > 0 ? 1 : 0);
    wcrypto_aes2(ctx, x, ks);

    blk = 1;
    for (off = 0; off < n; off += 16, blk++) {
        m = n - off < 16 ? n - off : 16;
        for (j = 0; j < m; j++) {
            uint8_t c, p;

            c = data[off + j];
            p = decrypt ? (uint8_t)(c ^ ks[j]) : c;
            x[j] ^= p;
            if (write)
                data[off + j] = (uint8_t)(c ^ ks[j]);
        }
        ccm_ctr(ks, h->nonce, off + 16 < n ? blk + 1 : 0);
        wcrypto_aes2(ctx, x, ks);
    }
    for (i = 0; i < 8; i++)
        tag[i] = (uint8_t)(x[i] ^ ks[i]);
    wcrypto_wipe(x, sizeof(x));
    wcrypto_wipe(ks, sizeof(ks));
}

/** Decrypt n bytes from src to dst by CTR from counter 1; dst may lie
 * below src in the same buffer, since each byte is read before any write
 * reaches it. */
static void ccm_ctr_copy(const struct wlan_aes128 *ctx, const uint8_t nonce[13], const uint8_t *src,
                         uint8_t *dst, unsigned n)
{
    uint8_t ks[32];
    unsigned off, j, m, blk;

    blk = 1;
    for (off = 0; off < n; off += 32, blk += 2) {
        ccm_ctr(ks, nonce, blk);
        ccm_ctr(ks + 16, nonce, blk + 1);
        wcrypto_aes2(ctx, ks, ks + 16);
        m = n - off < 32 ? n - off : 32;
        for (j = 0; j < m; j++)
            dst[off + j] = (uint8_t)(src[off + j] ^ ks[j]);
    }
    wcrypto_wipe(ks, sizeof(ks));
}

/** Insert the CCMP header, encrypt and append the MIC, all validated
 * before the first byte changes. */
int wlan_ccmp_encrypt(const struct wlan_aes128 *ctx, uint8_t *frame, unsigned *len, unsigned cap,
                      uint64_t pn, unsigned key_id)
{
    struct ccmp_hdr h;
    uint8_t tag[8];
    unsigned n, i;
    int err;

    if (!frame || *len > IEEE80211_MAX_FRAME_LEN || *len > cap || key_id > 3 || pn == 0 ||
        pn > CCMP_PN_MAX)
        return -EINVAL;
    err = ccmp_parse(frame, *len, &h);
    if (err)
        return err;
    if ((frame[1] << 8) & IEEE80211_FCTL_PROTECTED)
        return -EINVAL;
    if (*len + WLAN_CCMP_HDR_LEN + WLAN_CCMP_MIC_LEN > cap ||
        *len + WLAN_CCMP_HDR_LEN + WLAN_CCMP_MIC_LEN > IEEE80211_MAX_FRAME_LEN)
        return -ENOSPC;
    ccmp_nonce_pn(&h, pn);

    n = *len - h.hdrlen;
    for (i = n; i > 0; i--)
        frame[h.hdrlen + WLAN_CCMP_HDR_LEN + i - 1] = frame[h.hdrlen + i - 1];
    frame[h.hdrlen + 0] = (uint8_t)pn;
    frame[h.hdrlen + 1] = (uint8_t)(pn >> 8);
    frame[h.hdrlen + 2] = 0;
    frame[h.hdrlen + 3] = (uint8_t)(CCMP_EXT_IV | key_id << 6);
    frame[h.hdrlen + 4] = (uint8_t)(pn >> 16);
    frame[h.hdrlen + 5] = (uint8_t)(pn >> 24);
    frame[h.hdrlen + 6] = (uint8_t)(pn >> 32);
    frame[h.hdrlen + 7] = (uint8_t)(pn >> 40);
    frame[1] |= IEEE80211_FCTL_PROTECTED >> 8;

    ccm_run(ctx, &h, frame + h.hdrlen + WLAN_CCMP_HDR_LEN, n, 0, 1, tag);
    for (i = 0; i < WLAN_CCMP_MIC_LEN; i++)
        frame[h.hdrlen + WLAN_CCMP_HDR_LEN + n + i] = tag[i];
    *len += WLAN_CCMP_HDR_LEN + WLAN_CCMP_MIC_LEN;
    wcrypto_wipe(tag, sizeof(tag));
    return 0;
}

/** Authenticate the frame without writing, then decrypt it over its CCMP
 * header, so that a forged frame leaves the buffer as it came. */
int wlan_ccmp_decrypt(const struct wlan_aes128 *ctx, uint8_t *frame, unsigned *len, uint64_t *pn,
                      unsigned *key_id)
{
    struct ccmp_hdr h;
    uint8_t tag[8];
    const uint8_t *iv;
    uint64_t rx_pn;
    unsigned n;
    int err, ok;

    if (!frame || *len > IEEE80211_MAX_FRAME_LEN)
        return -EINVAL;
    err = ccmp_parse(frame, *len, &h);
    if (err)
        return err;
    if (!((frame[1] << 8) & IEEE80211_FCTL_PROTECTED))
        return -EINVAL;
    if (*len < h.hdrlen + WLAN_CCMP_HDR_LEN + WLAN_CCMP_MIC_LEN)
        return -EINVAL;
    iv = frame + h.hdrlen;
    if (!(iv[3] & CCMP_EXT_IV))
        return -EINVAL;
    rx_pn = (uint64_t)iv[0] | (uint64_t)iv[1] << 8 | (uint64_t)iv[4] << 16 | (uint64_t)iv[5] << 24 |
            (uint64_t)iv[6] << 32 | (uint64_t)iv[7] << 40;
    ccmp_nonce_pn(&h, rx_pn);

    n = *len - h.hdrlen - WLAN_CCMP_HDR_LEN - WLAN_CCMP_MIC_LEN;
    ccm_run(ctx, &h, frame + h.hdrlen + WLAN_CCMP_HDR_LEN, n, 1, 0, tag);
    ok = wcrypto_equal(tag, frame + h.hdrlen + WLAN_CCMP_HDR_LEN + n, WLAN_CCMP_MIC_LEN);
    wcrypto_wipe(tag, sizeof(tag));
    if (!ok)
        return -EBADMSG;

    *key_id = iv[3] >> 6;
    ccm_ctr_copy(ctx, h.nonce, frame + h.hdrlen + WLAN_CCMP_HDR_LEN, frame + h.hdrlen, n);
    frame[1] &= (uint8_t)~(IEEE80211_FCTL_PROTECTED >> 8);
    *len = h.hdrlen + n;
    *pn = rx_pn;
    return 0;
}
