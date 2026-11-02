/* Ethernet and three-address station data conversion, with a control port. */
#include "wiphy.h"
#include "../ethernet/ethernet.h"
#include "../net.h"
#include "../../lib/kerrno.h"
#include "../../lib/kstring.h"

/** Compare two six-byte addresses without aligned loads. */
static int same_mac(const uint8_t *a, const uint8_t *b)
{
    unsigned i;
    for (i = 0; i < 6; ++i)
        if (a[i] != b[i])
            return 0;
    return 1;
}

/** Read an unaligned big-endian EtherType. */
static uint16_t type_at(const uint8_t *p)
{
    return (uint16_t)((unsigned)p[0] << 8 | p[1]);
}

/** Permit EAPOL on an associated station and other data once authorized. */
int wlan_port_allowed(const struct wlan *wlan, uint16_t protocol)
{
    return wlan && wlan->started && !wlan->blocked && !wlan->removing &&
           wlan->state >= WLAN_STA_ASSOCIATED &&
           (protocol == WLAN_ETH_P_PAE || wlan->state == WLAN_STA_AUTHORIZED);
}

/** Build a ToDS station MPDU, preserving outputs on invalid input. */
int wlan_encap(const uint8_t *ethernet, unsigned len, const uint8_t bssid[6], unsigned seq,
               uint8_t *out, unsigned cap, unsigned *outlen)
{
    unsigned total, payload;
    uint16_t type;
    int snap;

    if (!ethernet || !bssid || !out || !outlen || len < ETH_HDR_SIZE || seq > 4095)
        return -EINVAL;
    if (len > ETH_HDR_SIZE + ETH_MTU)
        return -EMSGSIZE;
    type = type_at(ethernet + 12);
    snap = type >= 0x0600;
    payload = len - ETH_HDR_SIZE;
    if (!snap) {
        if (type < 3 || type > ETH_MTU || type > payload)
            return -EINVAL;
        payload = type;
    }
    total = payload + IEEE80211_HDRLEN + (snap ? 8 : 0);
    if (cap < total)
        return -EMSGSIZE;
    k_memset(out, 0, IEEE80211_HDRLEN);
    out[0] = IEEE80211_FTYPE_DATA;
    out[1] = IEEE80211_FCTL_TODS >> 8;
    k_memcpy(out + 4, bssid, 6);
    k_memcpy(out + 10, ethernet + 6, 6);
    k_memcpy(out + 16, ethernet, 6);
    out[22] = (uint8_t)(seq << 4);
    out[23] = (uint8_t)(seq >> 4);
    if (snap) {
        out[24] = out[25] = 0xaa;
        out[26] = 3;
        out[27] = out[28] = 0;
        /* IEEE 802.1H carries IPX and AARP through the bridge-tunnel OUI. */
        out[29] = type == 0x8137 || type == 0x80f3 ? 0xf8 : 0;
        k_memcpy(out + 30, ethernet + 12, payload + 2);
    } else {
        k_memcpy(out + 24, ethernet + 14, payload);
    }
    *outlen = total;
    return 0;
}

/** Return 1 when the payload of n bytes at llc, at least 3, begins with an
 * RFC 1042 or bridge-tunnel SNAP header that the Ethernet frame drops for
 * its type, 0 when the frame keeps the payload whole behind a length, or -1
 * for an LLC/SNAP header cut short. */
static int snap_strip(const uint8_t *llc, unsigned n)
{
    uint16_t type;

    if (llc[0] != 0xaa || llc[1] != 0xaa || llc[2] != 3)
        return 0;
    if (n < 8)
        return -1;
    type = type_at(llc + 6);
    /* RFC 1042's IPX/AARP exception retains the complete LLC header. */
    return !llc[3] && !llc[4] && type >= 0x0600 &&
           (llc[5] == 0xf8 || (!llc[5] && type != 0x8137 && type != 0x80f3));
}

/** Decode a decrypted FromDS station MPDU, preserving outputs on error. */
int wlan_decap(const uint8_t *frame, unsigned len, const uint8_t bssid[6], uint8_t *out,
               unsigned cap, unsigned *outlen)
{
    unsigned hdr = IEEE80211_HDRLEN, payload, total;
    uint16_t fc;
    int strip;
    const uint8_t *llc;

    if (!frame || !bssid || !out || !outlen || len < IEEE80211_HDRLEN)
        return -EINVAL;
    fc = (uint16_t)((unsigned)frame[1] << 8 | frame[0]);
    if ((fc & (IEEE80211_FCTL_VERS | IEEE80211_FCTL_FTYPE | IEEE80211_FCTL_TODS |
               IEEE80211_FCTL_FROMDS)) != (IEEE80211_FTYPE_DATA | IEEE80211_FCTL_FROMDS) ||
        (fc & (IEEE80211_FCTL_PROTECTED | IEEE80211_FCTL_MOREFRAGS)) || (frame[22] & 15) ||
        (fc & IEEE80211_STYPE_NULLFUNC) || !same_mac(frame + 10, bssid))
        return -EINVAL;
    if (fc & IEEE80211_STYPE_QOS_DATA) {
        if (len < hdr + 2 || (frame[hdr] & IEEE80211_QOS_CTL_A_MSDU_PRESENT))
            return -EINVAL;
        hdr += 2;
        if (fc & IEEE80211_FCTL_ORDER)
            hdr += 4;
    }
    if (len < hdr + 3)
        return -EINVAL;
    llc = frame + hdr;
    payload = len - hdr;
    strip = snap_strip(llc, payload);
    if (strip < 0)
        return -EINVAL;
    if (strip)
        payload -= 8;
    total = ETH_HDR_SIZE + payload;
    if (payload > ETH_MTU || cap < total)
        return -EMSGSIZE;
    k_memcpy(out, frame + 4, 6);
    k_memcpy(out + 6, frame + 16, 6);
    if (!strip) {
        out[12] = (uint8_t)(payload >> 8);
        out[13] = (uint8_t)payload;
        k_memcpy(out + 14, llc, payload);
    } else {
        k_memcpy(out + 12, llc + 6, payload + 2);
    }
    *outlen = total;
    return 0;
}

/** Gate worker-delivered data before it reaches any Ethernet consumer. */
int wlan_rx_ethernet(struct wlan *wlan, const uint8_t *frame, unsigned len)
{
    uint32_t flags;

    if (!wlan || !frame || len < ETH_HDR_SIZE || len > ETH_HDR_SIZE + ETH_MTU)
        return -EINVAL;
    if (!wlan->netdev.up || !wlan_port_allowed(wlan, type_at(frame + 12))) {
        wlan->counters.rx_dropped++;
        wlan->netdev.rx_dropped++;
        return -EACCES;
    }
    flags = net_lock();
    ethernet_rx(&wlan->netdev, frame, (uint16_t)len);
    net_unlock(flags);
    return 0;
}
