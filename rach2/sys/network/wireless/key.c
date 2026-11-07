/* The key table: pairwise, group and integrity keys with their counters,
 * kept in software and, when the radio takes them, in its key slots. */
#include "data.h"
#include "../../lib/kerrno.h"
#include "../../lib/kstring.h"
#include "../../timer.h"

static const uint8_t broadcast[6] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};

/** Overwrite n bytes with zeros by volatile stores, which the compiler
 * keeps although nothing reads them again. */
static void wipe(void *p, unsigned n)
{
    volatile uint8_t *q = (volatile uint8_t *)p;

    while (n--)
        *q++ = 0;
}

/** Whether two n-byte strings are equal. */
static int same(const uint8_t *a, const uint8_t *b, unsigned n)
{
    unsigned i;

    for (i = 0; i < n; ++i)
        if (a[i] != b[i])
            return 0;
    return 1;
}

/** The radio's hardware-cipher bit for `cipher`, or 0. */
static unsigned hw_cipher(uint32_t cipher)
{
    switch (cipher) {
    case WLAN_CIPHER_SUITE_CCMP:
        return WLAN_CIPHER_HW_CCMP;
    case WLAN_CIPHER_SUITE_TKIP:
        return WLAN_CIPHER_HW_TKIP;
    case WLAN_CIPHER_SUITE_AES_CMAC:
        return WLAN_CIPHER_HW_BIP;
    default:
        return 0;
    }
}

/** Whether a key's cipher, length, index and peer go together. */
static int key_valid(const struct wlan_key *key)
{
    int pairwise = (key->flags & WLAN_KEY_PAIRWISE) != 0;

    if (key->cipher == WLAN_CIPHER_SUITE_CCMP) {
        if (key->len != 16)
            return 0;
    } else if (key->cipher == WLAN_CIPHER_SUITE_TKIP) {
        if (key->len != WLAN_TKIP_KEY_LEN)
            return 0;
    } else if (key->cipher == WLAN_CIPHER_SUITE_AES_CMAC) {
        if (key->len != 16 || pairwise || key->index < WLAN_DATA_IGTK_FIRST ||
            key->index >= WLAN_KEY_SLOTS)
            return 0;
        return 1;
    } else {
        return 0;
    }
    if (pairwise)
        return key->index < WLAN_DATA_PAIRWISE_KEYS && !(key->peer[0] & 1);
    return key->index < WLAN_DATA_IGTK_FIRST;
}

/** The entry for a key of these flags and index, whether filled or not. */
static struct wlan_data_key *key_entry(struct wlan_data *d, int pairwise, unsigned index)
{
    if (pairwise)
        return index < WLAN_DATA_PAIRWISE_KEYS ? &d->pairwise[index] : 0;
    return index < WLAN_KEY_SLOTS ? &d->group[index] : 0;
}

/** Offer `k` to the radio's key slots; -EOPNOTSUPP leaves it in software. */
static int key_to_radio(struct wlan_data *d, struct wlan_data_key *k)
{
    struct wlan_radio *radio = d->radio;
    int slot = -1, rc;

    k->key.hw_slot = -1;
    if (!radio->ops->set_key || !(radio->hw_ciphers & hw_cipher(k->key.cipher)))
        return 0;
    rc = radio->ops->set_key(radio, 1, k->key.peer, &k->key, &slot);
    if (rc == -EOPNOTSUPP)
        return 0;
    if (rc)
        return rc;
    k->key.hw_slot = slot;
    return 0;
}

/** Take `k` out of the radio's slot, if it is in one. */
static int key_from_radio(struct wlan_data *d, struct wlan_data_key *k)
{
    struct wlan_radio *radio = d->radio;
    int slot = k->key.hw_slot, rc;

    if (slot < 0 || !radio->ops->set_key)
        return 0;
    rc = radio->ops->set_key(radio, 0, k->key.peer, &k->key, &slot);
    if (rc)
        return rc;
    k->key.hw_slot = -1;
    return 0;
}

/** Put `fresh` in the radio in place of `old`, once the BlockAck sessions
 * have ended when it replaces a pairwise key in the radio. Returns as
 * wlan_ba_rekey does when the sessions must wait, else as key_to_radio
 * does. */
static int replace_in_radio(struct wlan_data *d, struct wlan_data_key *fresh,
                            const struct wlan_data_key *old, int pairwise)
{
    int rc = 0;

    if (pairwise && old->gen && old->key.hw_slot >= 0)
        rc = wlan_ba_rekey(d);
    return rc ? rc : key_to_radio(d, fresh);
}

/** Drop every MSDU being reassembled, since a new key must not complete a
 * frame begun under the old one (the fragment cache attack of 2021). */
static void flush_defrag(struct wlan_data *d)
{
    unsigned i;

    for (i = 0; i < WLAN_DATA_DEFRAG; ++i)
        d->defrag[i].len = 0;
}

int wlan_data_install_key(struct wlan_data *d, const struct wlan_key *key)
{
    struct wlan_data_key *old, fresh;
    unsigned tid;
    int pairwise, rc;

    if (!d || !key || !key_valid(key))
        return -EINVAL;
    pairwise = (key->flags & WLAN_KEY_PAIRWISE) != 0;
    if (pairwise)
        d->renewing = 0;
    old = key_entry(d, pairwise, key->index);
    if (old->gen && old->key.cipher == key->cipher && old->key.len == key->len &&
        same(old->key.data, key->data, key->len) &&
        (!pairwise || same(old->key.peer, key->peer, 6))) {
        /* The same key again keeps its counters, so that a replayed
         * handshake message cannot reset them (KRACK, 2017). */
        if (pairwise)
            d->tx_pairwise = key->index;
        return 0;
    }
    k_memset(&fresh, 0, sizeof(fresh));
    fresh.key = *key;
    fresh.key.flags &= WLAN_KEY_PAIRWISE;
    if (!pairwise)
        k_memcpy(fresh.key.peer, broadcast, 6);
    if (fresh.key.tx_pn > WLAN_PN_MAX)
        fresh.key.tx_pn = WLAN_PN_MAX;
    /* The receive counters a caller gives (the RSC of a group key) stand;
     * robust management frames start from the lowest of them. */
    fresh.rx_mgmt_pn = fresh.key.rx_pn[WLAN_NON_QOS_TID];
    if (key->cipher != WLAN_CIPHER_SUITE_TKIP)
        wlan_aes128_setkey(&fresh.aes, key->data);
    rc = replace_in_radio(d, &fresh, old, pairwise);
    if (rc) {
        wipe(&fresh, sizeof(fresh));
        return rc;
    }
    if (old->gen) {
        rc = key_from_radio(d, old);
        if (rc) {
            (void)key_from_radio(d, &fresh);
            wipe(&fresh, sizeof(fresh));
            return rc;
        }
        if (pairwise) {
            for (tid = 0; tid < WLAN_NUM_TIDS; tid++)
                if (old->key.rx_pn[tid] > fresh.old_pn)
                    fresh.old_pn = old->key.rx_pn[tid];
            fresh.old_until = tick_count() + WLAN_DATA_REKEY_SETTLE;
        }
    }
    fresh.gen = ++d->key_gen;
    if (!fresh.gen)
        fresh.gen = ++d->key_gen;
    wipe(old, sizeof(*old));
    *old = fresh;
    wipe(&fresh, sizeof(fresh));
    if (pairwise)
        d->tx_pairwise = key->index;
    flush_defrag(d);
    return 0;
}

int wlan_data_remove_key(struct wlan_data *d, const uint8_t peer[6], unsigned index)
{
    struct wlan_data_key *k;
    int rc;

    if (!d || !peer)
        return -EINVAL;
    k = key_entry(d, !(peer[0] & 1), index);
    if (!k)
        return -EINVAL;
    if (!k->gen || (!(peer[0] & 1) && !same(k->key.peer, peer, 6)))
        return -ENOENT;
    rc = key_from_radio(d, k);
    if (rc)
        return rc;
    wipe(k, sizeof(*k));
    flush_defrag(d);
    return 0;
}

int wlan_data_default_key(struct wlan_data *d, unsigned index, int management)
{
    if (!d)
        return -EINVAL;
    if (management) {
        if (index < WLAN_DATA_IGTK_FIRST || index >= WLAN_KEY_SLOTS)
            return -EINVAL;
        if (!d->group[index].gen)
            return -ENOENT;
        d->default_igtk = index;
        return 0;
    }
    if (index >= WLAN_DATA_IGTK_FIRST)
        return -EINVAL;
    if (!d->group[index].gen)
        return -ENOENT;
    d->default_group = index;
    return 0;
}

void wlan_data_clear_keys(struct wlan_data *d)
{
    unsigned i;

    for (i = 0; i < WLAN_DATA_PAIRWISE_KEYS; ++i) {
        if (d->pairwise[i].gen)
            (void)key_from_radio(d, &d->pairwise[i]);
        wipe(&d->pairwise[i], sizeof(d->pairwise[i]));
    }
    for (i = 0; i < WLAN_KEY_SLOTS; ++i) {
        if (d->group[i].gen)
            (void)key_from_radio(d, &d->group[i]);
        wipe(&d->group[i], sizeof(d->group[i]));
    }
    d->tx_pairwise = 0;
    d->default_group = 0;
    d->default_igtk = 0;
    flush_defrag(d);
}

void wlan_data_restore_keys(struct wlan_data *d)
{
    unsigned i;

    /* A restarted radio has forgotten its slots, so none is removed. The
     * counters are the data path's, so no PN is used twice. */
    for (i = 0; i < WLAN_DATA_PAIRWISE_KEYS; ++i)
        if (d->pairwise[i].gen && key_to_radio(d, &d->pairwise[i]) != 0)
            d->pairwise[i].key.hw_slot = -1;
    for (i = 0; i < WLAN_KEY_SLOTS; ++i)
        if (d->group[i].gen && key_to_radio(d, &d->group[i]) != 0)
            d->group[i].key.hw_slot = -1;
}

struct wlan_data_key *wlan_data_pairwise_key(struct wlan_data *d, unsigned index)
{
    if (index >= WLAN_DATA_PAIRWISE_KEYS || !d->pairwise[index].gen)
        return 0;
    return &d->pairwise[index];
}

struct wlan_data_key *wlan_data_group_key(struct wlan_data *d, unsigned index)
{
    if (index >= WLAN_KEY_SLOTS || !d->group[index].gen)
        return 0;
    return &d->group[index];
}
