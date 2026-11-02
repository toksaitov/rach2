/* Parse and build the information elements of Wi-Fi management frames. */
#include "ieee80211.h"
#include "../../lib/kerrno.h"

/** Clear borrowed element references; out must be non-NULL. */
static void elems_clear(struct ieee80211_elems *out)
{
    unsigned i;

    for (i = 0; i < 256; ++i)
        out->data[i] = 0;
    out->wmm_info = 0;
    out->wmm_param = 0;
    out->wpa = 0;
}

/** Validate the length of a known element, leaving unknown ids to callers. */
static int elem_length_ok(unsigned id, unsigned len)
{
    switch (id) {
    case WLAN_EID_SSID:
        return len <= IEEE80211_MAX_SSID_LEN;
    case WLAN_EID_SUPP_RATES:
        return len >= 1 && len <= 8;
    case WLAN_EID_DS_PARAMS:
    case WLAN_EID_ERP_INFO:
        return len == 1;
    case WLAN_EID_TIM:
        return len >= 4;
    case WLAN_EID_COUNTRY:
        return len >= 3;
    case WLAN_EID_HT_CAPABILITY:
        return len == IEEE80211_HT_CAP_LEN;
    case WLAN_EID_HT_OPERATION:
        return len == IEEE80211_HT_OPERATION_LEN;
    case WLAN_EID_RSN:
        return len >= 2;
    case WLAN_EID_TIMEOUT_INTERVAL:
        return len == 5;
    case WLAN_EID_MMIE:
        return len == IEEE80211_MMIE_LEN - 2;
    case WLAN_EID_EXT_SUPP_RATES:
    case WLAN_EID_EXT_CAPABILITY:
    case WLAN_EID_RSNX:
    case WLAN_EID_EXTENSION:
        return len >= 1;
    default:
        return 1;
    }
}

/** Recognize WPA and the two WMM forms by OUI, type and subtype. */
static int vendor_elem(const uint8_t *p, struct ieee80211_elems *out)
{
    const uint8_t **slot;
    unsigned len;

    len = p[1];
    if (len < 4 || p[2] != 0 || p[3] != 0x50 || p[4] != 0xf2)
        return 0;
    slot = 0;
    if (p[5] == 1) {
        if (len < 6)
            return -EINVAL;
        slot = &out->wpa;
    } else if (p[5] == WLAN_WMM_OUI_TYPE) {
        if (len < 6 || p[7] != 1)
            return -EINVAL;
        if (p[6] == WLAN_WMM_INFO) {
            if (len != 7)
                return -EINVAL;
            slot = &out->wmm_info;
        } else if (p[6] == WLAN_WMM_PARAM) {
            if (len != 24)
                return -EINVAL;
            slot = &out->wmm_param;
        }
    }
    if (slot) {
        if (*slot)
            return -EINVAL;
        *slot = p;
    }
    return 0;
}

/** Parse bounded elements; the pointers set borrow buf until it changes. */
int ieee80211_parse_elems(const uint8_t *buf, unsigned len, struct ieee80211_elems *out)
{
    unsigned id, n, i;
    uint8_t extensions[32];

    if (!out)
        return -EINVAL;
    elems_clear(out);
    if (len && !buf)
        return -EINVAL;
    for (i = 0; i < sizeof(extensions); ++i)
        extensions[i] = 0;
    while (len) {
        if (len < 2)
            goto bad;
        id = buf[0];
        n = buf[1];
        if (n > len - 2 || !elem_length_ok(id, n))
            goto bad;
        if (id == WLAN_EID_VENDOR_SPECIFIC) {
            if (vendor_elem(buf, out))
                goto bad;
        } else if (id == WLAN_EID_EXTENSION) {
            i = buf[2];
            if (extensions[i / 8] & (1u << (i % 8)))
                goto bad;
            extensions[i / 8] |= (uint8_t)(1u << (i % 8));
        } else if (out->data[id]) {
            goto bad;
        }
        if (!out->data[id])
            out->data[id] = buf;
        buf += n + 2;
        len -= n + 2;
    }
    return 0;

bad:
    elems_clear(out);
    return -EINVAL;
}

/** Append one bounded element, preserving the buffer on any error. */
int ieee80211_put_elem(uint8_t *buf, unsigned cap, unsigned *used, unsigned id,
                       const uint8_t *payload, unsigned len)
{
    unsigned i, pos;

    if (!buf || !used || id > 255 || len > 255 || (len && !payload) || !elem_length_ok(id, len))
        return -EINVAL;
    pos = *used;
    if (pos > cap || cap - pos < len + 2)
        return -ENOSPC;
    buf[pos] = (uint8_t)id;
    buf[pos + 1] = (uint8_t)len;
    for (i = 0; i < len; ++i)
        buf[pos + 2 + i] = payload[i];
    *used = pos + len + 2;
    return 0;
}

/** Build station elements after validating every pointer, length and bound. */
int ieee80211_build_sta_elems(uint8_t *buf, unsigned cap, unsigned *used,
                              const struct ieee80211_sta_elems *p)
{
    static const uint8_t wmm[7] = {0, 0x50, 0xf2, 2, 0, 1, 0};
    unsigned need, first, pos;

    if (!buf || !used || !p || p->ssid_len > IEEE80211_MAX_SSID_LEN || (p->ssid_len && !p->ssid) ||
        !p->rates || !p->rates_len || p->rates_len > IEEE80211_MAX_RATES || p->rsn_len > 255 ||
        p->rsnx_len > 255 || p->ext_cap_len > 255 || (p->rsn_len && (!p->rsn || p->rsn_len < 2)) ||
        (p->rsnx_len && !p->rsnx) || (p->ext_cap_len && !p->ext_cap))
        return -EINVAL;
    first = p->rates_len > 8 ? 8 : p->rates_len;
    need = 2 + p->ssid_len + 2 + p->rates_len;
    if (p->rates_len > 8)
        need += 2;
    if (p->rsn_len)
        need += 2 + p->rsn_len;
    if (p->ht_cap)
        need += 2 + IEEE80211_HT_CAP_LEN;
    if (p->ext_cap_len)
        need += 2 + p->ext_cap_len;
    if (p->wmm)
        need += 2 + sizeof(wmm);
    if (p->rsnx_len)
        need += 2 + p->rsnx_len;
    if (need > cap)
        return -ENOSPC;
    pos = 0;
    ieee80211_put_elem(buf, cap, &pos, WLAN_EID_SSID, p->ssid, p->ssid_len);
    ieee80211_put_elem(buf, cap, &pos, WLAN_EID_SUPP_RATES, p->rates, first);
    if (p->rates_len > first)
        ieee80211_put_elem(buf, cap, &pos, WLAN_EID_EXT_SUPP_RATES, p->rates + first,
                           p->rates_len - first);
    if (p->rsn_len)
        ieee80211_put_elem(buf, cap, &pos, WLAN_EID_RSN, p->rsn, p->rsn_len);
    if (p->ht_cap)
        ieee80211_put_elem(buf, cap, &pos, WLAN_EID_HT_CAPABILITY, p->ht_cap, IEEE80211_HT_CAP_LEN);
    if (p->ext_cap_len)
        ieee80211_put_elem(buf, cap, &pos, WLAN_EID_EXT_CAPABILITY, p->ext_cap, p->ext_cap_len);
    if (p->wmm)
        ieee80211_put_elem(buf, cap, &pos, WLAN_EID_VENDOR_SPECIFIC, wmm, sizeof(wmm));
    if (p->rsnx_len)
        ieee80211_put_elem(buf, cap, &pos, WLAN_EID_RSNX, p->rsnx, p->rsnx_len);
    *used = pos;
    return 0;
}
