/*
 * bip.c: BIP-CMAC-128, the integrity protection of group-addressed robust
 * management frames, as IEEE 802.11-2020 12.5.4 defines it.
 *
 * The MIC is the first 8 bytes of AES-CMAC over the masked frame control
 * (Retry, Power Management and More Data cleared), the three addresses and
 * the whole frame body after the 24-byte header, with the MMIE's MIC field
 * taken as zero. Like Linux's ieee80211_crypto_aes_cmac_encrypt
 * (net/mac80211/wpa.c) and hostap's bip_protect, the body starts at byte
 * 24 whatever the Order bit says. The body is not encrypted.
 */
#include "wcrypto_int.h"
#include "../../../lib/kerrno.h"

#define BIP_IPN_MAX 0xffffffffffffULL
#define BIP_AAD_LEN 20
#define BIP_MIC_OFF 10 /* the MIC's offset in the MMIE */

/**
 * Check that frame is a group-addressed management frame of len bytes
 * that BIP may cover, and build its AAD. Return 0 or -EINVAL.
 */
static int bip_aad(const uint8_t *frame, unsigned len, uint8_t aad[BIP_AAD_LEN])
{
    unsigned fc, i;

    if (!frame || len < IEEE80211_HDRLEN || len > IEEE80211_MAX_FRAME_LEN)
        return -EINVAL;
    fc = (unsigned)frame[0] | (unsigned)frame[1] << 8;
    if ((fc & IEEE80211_FCTL_VERS) != 0 || (fc & IEEE80211_FCTL_FTYPE) != IEEE80211_FTYPE_MGMT ||
        (fc & IEEE80211_FCTL_PROTECTED))
        return -EINVAL;
    if (!(frame[4] & 0x01)) /* the group bit of Address 1 */
        return -EINVAL;
    fc &= ~(unsigned)(IEEE80211_FCTL_RETRY | IEEE80211_FCTL_PM | IEEE80211_FCTL_MOREDATA);
    aad[0] = (uint8_t)fc;
    aad[1] = (uint8_t)(fc >> 8);
    for (i = 0; i < 3 * IEEE80211_ADDR_LEN; i++)
        aad[2 + i] = frame[4 + i];
    return 0;
}

/** Compute the 8-byte MIC of a frame of len bytes whose last 18 are its
 * MMIE, taking the MMIE's own MIC field as zero. */
static void bip_mic(const struct wlan_aes128 *ctx, const uint8_t aad[BIP_AAD_LEN],
                    const uint8_t *frame, unsigned len, uint8_t mic[8])
{
    static const uint8_t zero[8] = {0};
    struct wcrypto_cmac st;
    uint8_t tag[16];
    unsigned i;

    wcrypto_cmac_init(&st, ctx);
    wcrypto_cmac_update(&st, aad, BIP_AAD_LEN);
    wcrypto_cmac_update(&st, frame + IEEE80211_HDRLEN, len - IEEE80211_HDRLEN - 8);
    wcrypto_cmac_update(&st, zero, 8);
    wcrypto_cmac_final(&st, tag);
    for (i = 0; i < 8; i++)
        mic[i] = tag[i];
    wcrypto_wipe(tag, sizeof(tag));
}

/** Append the MMIE with the IPN and sign it, validated before writing. */
int wlan_bip_sign(const struct wlan_aes128 *ctx, uint8_t *frame, unsigned *len, unsigned cap,
                  uint64_t ipn, unsigned key_id)
{
    uint8_t aad[BIP_AAD_LEN], *mmie;
    unsigned n, i;
    int err;

    if (key_id != 4 && key_id != 5)
        return -EINVAL;
    if (ipn == 0 || ipn > BIP_IPN_MAX || *len > cap)
        return -EINVAL;
    err = bip_aad(frame, *len, aad);
    if (err)
        return err;
    n = *len + IEEE80211_MMIE_LEN;
    if (n > cap || n > IEEE80211_MAX_FRAME_LEN)
        return -ENOSPC;

    mmie = frame + *len;
    mmie[0] = WLAN_EID_MMIE;
    mmie[1] = IEEE80211_MMIE_LEN - 2;
    mmie[2] = (uint8_t)key_id;
    mmie[3] = 0;
    for (i = 0; i < 6; i++)
        mmie[4 + i] = (uint8_t)(ipn >> (8 * i));
    bip_mic(ctx, aad, frame, n, mmie + BIP_MIC_OFF);
    *len = n;
    return 0;
}

/** Verify the trailing MMIE and strip it; a forged frame is unchanged. */
int wlan_bip_check(const struct wlan_aes128 *ctx, uint8_t *frame, unsigned *len, uint64_t *ipn,
                   unsigned *key_id)
{
    uint8_t aad[BIP_AAD_LEN], mic[8];
    const uint8_t *mmie;
    unsigned kid, i;
    uint64_t v;
    int err, ok;

    err = bip_aad(frame, *len, aad);
    if (err)
        return err;
    if (*len < IEEE80211_HDRLEN + IEEE80211_MMIE_LEN)
        return -EINVAL;
    mmie = frame + *len - IEEE80211_MMIE_LEN;
    if (mmie[0] != WLAN_EID_MMIE || mmie[1] != IEEE80211_MMIE_LEN - 2)
        return -EINVAL;
    kid = (unsigned)mmie[2] | (unsigned)mmie[3] << 8;
    if (kid != 4 && kid != 5)
        return -EINVAL;
    v = 0;
    for (i = 0; i < 6; i++)
        v |= (uint64_t)mmie[4 + i] << (8 * i);

    bip_mic(ctx, aad, frame, *len, mic);
    ok = wcrypto_equal(mic, mmie + BIP_MIC_OFF, 8);
    wcrypto_wipe(mic, sizeof(mic));
    if (!ok)
        return -EBADMSG;
    *len -= IEEE80211_MMIE_LEN;
    *ipn = v;
    *key_id = kid;
    return 0;
}
