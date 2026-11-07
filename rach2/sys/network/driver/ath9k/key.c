/*
 * key.c: the AR9003's key cache of 128 entries, which encrypts and
 * decrypts TKIP and CCMP in hardware, and the copy of it that a reset
 * writes back.
 *
 * It follows ath's key.c (Linux v6.16), which ath9k shares with the other
 * Atheros drivers: ath_hw_set_keycache_entry, ath_hw_keysetmac,
 * ath_hw_keyreset, ath_key_config and ath_reserve_key_cache_slot, for a
 * station on a chip that keeps TKIP's two MIC keys in one entry
 * (AR_PCU_MIC_NEW_LOC_ENA, which mac.c sets). Group keys take the entry of
 * their key index; a pairwise key takes a free entry from
 * AR_RSVD_KEYTABLE_ENTRIES up, and TKIP the entry 64 above it as well.
 * Integrity keys and every other cipher stay in software. The chip does
 * unicast management frames too, so a protected action frame to the
 * station is decrypted here like a data frame.
 */

#include "ath9k.h"
#include "../../../lib/kerrno.h"
#include "../../../lib/kirq.h"
#include "../../../lib/kstring.h"

/* An entry's MIC half, 64 above it (ath's key.c). */
#define MIC_OFFSET 64

/* A TKIP key's bytes as the station holds them: the temporal key, then
 * the transmit and receive MIC keys (nl80211's TKIP data offsets). */
#define TKIP_TX_MIC 16
#define TKIP_RX_MIC 24

/** A little-endian 32-bit and 16-bit word at `p`. */
static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static uint32_t le16(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8;
}

/**
 * Write entry `entry`'s address, as ath_hw_keysetmac does: shifted right
 * by one bit across the two registers, with AR_KEYTABLE_VALID for a
 * unicast address or for none, which a group key has in a station.
 */
static void write_mac(struct ath9k *ah, unsigned entry, const uint8_t *mac)
{
    uint32_t lo = 0, hi = 0, valid = AR_KEYTABLE_VALID;

    if (mac) {
        if (mac[0] & 0x01)
            valid = 0;
        lo = le32(mac);
        hi = le16(mac + 4);
        lo >>= 1;
        lo |= (hi & 1) << 31;
        hi >>= 1;
    }
    REG_WRITE(ah, AR_KEYTABLE_MAC0(entry), lo);
    REG_WRITE(ah, AR_KEYTABLE_MAC1(entry), hi | valid);
}

/** Whether `mac` is all zeros, the address a group key's copy keeps. */
static int no_mac(const uint8_t *mac)
{
    unsigned i;

    for (i = 0; i < 6; i++)
        if (mac[i])
            return 0;
    return 1;
}

/**
 * Write entry `entry` from its copy `k`, as ath_hw_set_keycache_entry
 * does. The key cache's registers update their memory in pairs of writes,
 * so the order and the pairs are ath's. A TKIP key is first written with
 * its first 48 bits inverted, so that no frame meets a half-written key,
 * then its MIC entry, then the right bits. The MIC entry, 64 above, has
 * the receive MIC key's two words in KEY0 and KEY2, the transmit key's
 * halves in KEY1, KEY3 and KEY4, and no address.
 */
static void write_entry(struct ath9k *ah, unsigned entry, const struct ath9k_key *k)
{
    uint32_t key0 = le32(k->key), key1 = le16(k->key + 4), key2 = le32(k->key + 6);
    uint32_t key3 = le16(k->key + 10), key4 = le32(k->key + 12);
    const uint8_t *mac = no_mac(k->peer) ? 0 : k->peer;

    if (k->type != AR_KEYTABLE_TYPE_TKIP) {
        REG_WRITE(ah, AR_KEYTABLE_KEY0(entry), key0);
        REG_WRITE(ah, AR_KEYTABLE_KEY1(entry), key1);
        REG_WRITE(ah, AR_KEYTABLE_KEY2(entry), key2);
        REG_WRITE(ah, AR_KEYTABLE_KEY3(entry), key3);
        REG_WRITE(ah, AR_KEYTABLE_KEY4(entry), key4);
        REG_WRITE(ah, AR_KEYTABLE_TYPE(entry), k->type);
        write_mac(ah, entry, mac);
        return;
    }
    REG_WRITE(ah, AR_KEYTABLE_KEY0(entry), ~key0);
    REG_WRITE(ah, AR_KEYTABLE_KEY1(entry), ~key1);
    REG_WRITE(ah, AR_KEYTABLE_KEY2(entry), key2);
    REG_WRITE(ah, AR_KEYTABLE_KEY3(entry), key3);
    REG_WRITE(ah, AR_KEYTABLE_KEY4(entry), key4);
    REG_WRITE(ah, AR_KEYTABLE_TYPE(entry), k->type);
    write_mac(ah, entry, mac);

    entry += MIC_OFFSET;
    REG_WRITE(ah, AR_KEYTABLE_KEY0(entry), le32(k->mic));
    REG_WRITE(ah, AR_KEYTABLE_KEY1(entry), le16(k->mic + 8 + 2));
    REG_WRITE(ah, AR_KEYTABLE_KEY2(entry), le32(k->mic + 4));
    REG_WRITE(ah, AR_KEYTABLE_KEY3(entry), le16(k->mic + 8));
    REG_WRITE(ah, AR_KEYTABLE_KEY4(entry), le32(k->mic + 8 + 4));
    REG_WRITE(ah, AR_KEYTABLE_TYPE(entry), AR_KEYTABLE_TYPE_CLR);
    REG_WRITE(ah, AR_KEYTABLE_MAC0(entry), 0);
    REG_WRITE(ah, AR_KEYTABLE_MAC1(entry), 0);
    entry -= MIC_OFFSET;
    REG_WRITE(ah, AR_KEYTABLE_KEY0(entry), key0);
    REG_WRITE(ah, AR_KEYTABLE_KEY1(entry), key1);
}

/** Clear entry `entry`, and its MIC entry when `tkip`, as ath_hw_keyreset
 * does. */
static void reset_entry(struct ath9k *ah, unsigned entry, int tkip)
{
    REG_WRITE(ah, AR_KEYTABLE_KEY0(entry), 0);
    REG_WRITE(ah, AR_KEYTABLE_KEY1(entry), 0);
    REG_WRITE(ah, AR_KEYTABLE_KEY2(entry), 0);
    REG_WRITE(ah, AR_KEYTABLE_KEY3(entry), 0);
    REG_WRITE(ah, AR_KEYTABLE_KEY4(entry), 0);
    REG_WRITE(ah, AR_KEYTABLE_TYPE(entry), AR_KEYTABLE_TYPE_CLR);
    REG_WRITE(ah, AR_KEYTABLE_MAC0(entry), 0);
    REG_WRITE(ah, AR_KEYTABLE_MAC1(entry), 0);
    if (tkip) {
        entry += MIC_OFFSET;
        REG_WRITE(ah, AR_KEYTABLE_KEY0(entry), 0);
        REG_WRITE(ah, AR_KEYTABLE_KEY1(entry), 0);
        REG_WRITE(ah, AR_KEYTABLE_KEY2(entry), 0);
        REG_WRITE(ah, AR_KEYTABLE_KEY3(entry), 0);
        REG_WRITE(ah, AR_KEYTABLE_KEY4(entry), 0);
        REG_WRITE(ah, AR_KEYTABLE_TYPE(entry), AR_KEYTABLE_TYPE_CLR);
    }
}

/** Whether entry `i` holds a key or a TKIP key's MIC half. */
static int taken(const struct ath9k *ah, unsigned i)
{
    return ah->keys.entry[i].used != ATH9K_KEY_FREE;
}

/** Whether a pairwise key may take entry `i`: free, and not the entry its
 * predecessor just left, so that a frame in the receive ring that names it
 * cannot pass for one the new key opened (radio.h). */
static int open_entry(const struct ath9k *ah, unsigned i)
{
    return !taken(ah, i) && i != ah->keys.freed;
}

/**
 * A free entry for a pairwise key of `type`, as ath_reserve_key_cache_slot
 * chooses with the MIC keys combined: TKIP needs an entry below 64 whose
 * MIC entry is free too; another cipher first takes an entry whose partner
 * 64 away is used, which TKIP could not have, then any free one. Returns
 * -1 when the cache is full.
 */
static int reserve(const struct ath9k *ah, unsigned type)
{
    unsigned i;

    for (i = AR_RSVD_KEYTABLE_ENTRIES; i < MIC_OFFSET; i++) {
        if (type == AR_KEYTABLE_TYPE_TKIP) {
            if (open_entry(ah, i) && !taken(ah, i + MIC_OFFSET))
                return (int)i;
            continue;
        }
        if (open_entry(ah, i) && taken(ah, i + MIC_OFFSET))
            return (int)i;
        if (taken(ah, i) && open_entry(ah, i + MIC_OFFSET))
            return (int)(i + MIC_OFFSET);
    }
    if (type == AR_KEYTABLE_TYPE_TKIP)
        return -1;
    for (i = AR_RSVD_KEYTABLE_ENTRIES; i < AR_KEY_CACHE_SIZE; i++)
        if (open_entry(ah, i) && !(i >= MIC_OFFSET && i < MIC_OFFSET + AR_RSVD_KEYTABLE_ENTRIES))
            return (int)i;
    return -1;
}

/** Whether `key` for `peer` is a group key: a multicast peer, or no
 * WLAN_KEY_PAIRWISE. */
static int is_group(const uint8_t peer[6], const struct wlan_key *key)
{
    return (peer[0] & 0x01) || !(key->flags & WLAN_KEY_PAIRWISE);
}

/**
 * Fill `k` with the copy of `key` for `peer` that the cache holds: its
 * type, the peer of a pairwise key, and the temporal key; for TKIP the
 * receive MIC key and, after it, the transmit MIC key, or the receive one
 * again for a group key, which a station only receives with, as
 * ath_setkey_tkip writes them. Returns 0, or -EOPNOTSUPP for a cipher the
 * cache does not do.
 */
static int make_copy(struct ath9k_key *k, const uint8_t peer[6], const struct wlan_key *key)
{
    int group = is_group(peer, key);

    k_memset(k, 0, sizeof(*k));
    if (key->cipher == WLAN_CIPHER_SUITE_CCMP && key->len == 16)
        k->type = AR_KEYTABLE_TYPE_CCM;
    else if (key->cipher == WLAN_CIPHER_SUITE_TKIP && key->len == 32)
        k->type = AR_KEYTABLE_TYPE_TKIP;
    else
        return -EOPNOTSUPP;
    k->used = ATH9K_KEY_USED;
    if (!group)
        k_memcpy(k->peer, peer, 6);
    k_memcpy(k->key, key->data, 16);
    if (k->type == AR_KEYTABLE_TYPE_TKIP) {
        k_memcpy(k->mic, key->data + TKIP_RX_MIC, 8);
        k_memcpy(k->mic + 8, key->data + (group ? TKIP_RX_MIC : TKIP_TX_MIC), 8);
    }
    return 0;
}

/** Clear entry `slot`, which no frame names, and forget its copy. */
static void clear_entry(struct ath9k *ah, int slot)
{
    struct ath9k_key *k = &ah->keys.entry[slot];
    int tkip = k->type == AR_KEYTABLE_TYPE_TKIP;

    /* A stopped chip sleeps, and the next start's restore clears the whole
     * cache. */
    if (ah->bus.started)
        reset_entry(ah, (unsigned)slot, tkip);
    if (tkip)
        ah->keys.entry[slot + MIC_OFFSET].used = ATH9K_KEY_FREE;
    if (k->used == ATH9K_KEY_DYING)
        ah->keys.dying--;
    k_memset(k, 0, sizeof(*k));
    ah->keys.freed = (unsigned)slot;
}

/**
 * Clear entry `slot` for a key that takes it in place, so at once: the
 * frames that name it get 20 ms to go out with the key they were accepted
 * with, and then the chip is reset, whose drain reports them failed. A
 * stopped chip has none.
 */
static void forget(struct ath9k *ah, int slot)
{
    if (ah->bus.started && ath9k_tx_flush_key(ah, slot) != 0)
        (void)ath9k_reset(ah, ATH9K_RESET_TX_HANG);
    clear_entry(ah, slot);
}

/**
 * Install `key` for `peer`, as ath_key_config does in a station: a group
 * key at the entry of its index, replacing the key there; a pairwise key
 * of index 0 at a free entry. Returns 0 and *slot; -EOPNOTSUPP for another
 * cipher, an integrity key or a pairwise key of another index, which stay
 * in software; -ENOSPC when the cache is full.
 */
int ath9k_key_set(struct ath9k *ah, const uint8_t peer[6], const struct wlan_key *key, int *slot)
{
    struct ath9k_key copy;
    int idx;

    if (make_copy(&copy, peer, key) != 0)
        return -EOPNOTSUPP;
    if (is_group(peer, key)) {
        if (key->index >= AR_RSVD_KEYTABLE_ENTRIES)
            return -EOPNOTSUPP;
        idx = (int)key->index;
        if (ah->keys.entry[idx].used != ATH9K_KEY_FREE)
            forget(ah, idx);
    } else {
        if (key->index != 0)
            return -EOPNOTSUPP;
        idx = reserve(ah, copy.type);
        if (idx < 0)
            return -ENOSPC;
    }
    ah->keys.entry[idx] = copy;
    if (copy.type == AR_KEYTABLE_TYPE_TKIP)
        ah->keys.entry[idx + MIC_OFFSET].used = ATH9K_KEY_MIC;
    write_entry(ah, (unsigned)idx, &copy);
    *slot = idx;
    return 0;
}

/**
 * Clear entry `slot` and forget its copy, as ath9k_set_key does at
 * DISABLE_KEY. Frames that name it keep it: it leaves the address search
 * now, so that it opens nothing more, and its key stays for them until
 * ath9k_key_reap, since a frame sent with a cleared entry would leave
 * without the encryption it was accepted for. An entry not in use is left.
 * With `key`, the entry is cleared only while it holds `key` for `peer`: a
 * group key installed at the same index replaces the old one in place, so
 * that the removal of the old key, which the station makes after the
 * install, leaves the new.
 */
void ath9k_key_clear(struct ath9k *ah, int slot, const uint8_t peer[6], const struct wlan_key *key)
{
    struct ath9k_key copy;
    const uint8_t *a, *b;
    uint32_t flags;
    unsigned i;
    int named;

    if (slot < 0 || slot >= AR_KEY_CACHE_SIZE || ah->keys.entry[slot].used != ATH9K_KEY_USED)
        return;
    if (key) {
        if (make_copy(&copy, peer, key) != 0)
            return;
        a = (const uint8_t *)&copy;
        b = (const uint8_t *)&ah->keys.entry[slot];
        for (i = 0; i < sizeof(copy); i++)
            if (a[i] != b[i])
                return;
    }
    flags = kirq_save();
    named = ah->bus.started && ath9k_tx_names_key(ah, slot);
    if (named) {
        ah->keys.entry[slot].used = ATH9K_KEY_DYING;
        ah->keys.dying++;
        write_mac(ah, (unsigned)slot, 0);
    } else {
        clear_entry(ah, slot);
    }
    kirq_restore(flags);
}

void ath9k_key_reap(struct ath9k *ah)
{
    unsigned i;

    for (i = 0; ah->keys.dying && i < AR_KEY_CACHE_SIZE; i++)
        if (ah->keys.entry[i].used == ATH9K_KEY_DYING && !ath9k_tx_names_key(ah, (int)i))
            clear_entry(ah, (int)i);
}

/**
 * Clear all 128 entries, as ath9k's ath9k_cmn_init_crypto does, then
 * write each key the copy holds, so that a reset that lost the cache
 * leaves the station's keys where its frames name them. The drain before
 * a reset or a start left no frame, so no dying entry stays.
 */
void ath9k_key_restore(struct ath9k *ah)
{
    unsigned i;

    ath9k_key_reap(ah);
    for (i = 0; i < AR_KEY_CACHE_SIZE; i++)
        reset_entry(ah, i, 0);
    for (i = 0; i < AR_KEY_CACHE_SIZE; i++)
        if (ah->keys.entry[i].used == ATH9K_KEY_USED)
            write_entry(ah, i, &ah->keys.entry[i]);
}
