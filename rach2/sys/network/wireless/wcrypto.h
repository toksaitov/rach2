/* Software Wi-Fi cryptography: caller-owned keys and bounded MPDUs. */
#ifndef _WLAN_WCRYPTO_H_
#define _WLAN_WCRYPTO_H_

#include "ieee80211.h"

#define WLAN_AES_BLOCK_LEN 16
#define WLAN_CCMP_HDR_LEN  8
#define WLAN_CCMP_MIC_LEN  8
#define WLAN_TKIP_KEY_LEN  32
#define WLAN_TKIP_MIC_LEN  8
#define WLAN_PN_LEN        6

struct wlan_aes128 {
    uint32_t round_key[44]; /* private expanded AES-128 encryption schedule */
};

/* All calls below are reentrant, allocate nothing, take no locks and do not
 * sleep. Callers serialize changes to a key. Buffers are borrowed for the
 * call; input and output overlap only where explicitly allowed. Errors are
 * negative errno values. PN and IPN are host-order 48-bit unsigned counters;
 * callers allocate them monotonically, never wrap them, and check replay
 * before committing an authenticated receive. */

/** Expand exactly 16 key bytes into ctx, with no secret-indexed lookups.
 * ctx and key must be non-NULL; ctx is caller-owned and erased at key removal. */
void wlan_aes128_setkey(struct wlan_aes128 *ctx, const uint8_t key[16]);

/** Encrypt one 16-byte block; in == out is allowed, partial overlap is not.
 * ctx is read-only and may be shared by simultaneous callers. */
void wlan_aes128_encrypt(const struct wlan_aes128 *ctx, const uint8_t in[16], uint8_t out[16]);

/** Compute NIST SP 800-38B CMAC of len bytes, permitting a NULL data at len 0.
 * Write the full 16-byte tag to out; all other arguments must be non-NULL. */
void wlan_aes128_cmac(const struct wlan_aes128 *ctx, const uint8_t *data, unsigned len,
                      uint8_t out[16]);

/**
 * Protect a complete MPDU in place, without FCS, using CCMP-128.
 * Parse the MAC header (including address 4 and QoS when present), insert
 * the 8-byte CCMP header, encrypt the body and append its 8-byte tag; set
 * Protected. key_id is 0 to 3; pn is 1 to 2^48-1. Returns 0 and updates
 * *len, -EINVAL for an invalid frame/counter, or -ENOSPC when cap is short.
 * On error frame and *len are unchanged. No buffer may overlap ctx or len.
 */
int wlan_ccmp_encrypt(const struct wlan_aes128 *ctx, uint8_t *frame, unsigned *len, unsigned cap,
                      uint64_t pn, unsigned key_id);

/**
 * Authenticate and decrypt a CCMP MPDU in place, stripping CCMP header/tag
 * and clearing Protected. On success return 0 and set *len, *pn and *key_id.
 * Return -EINVAL for malformed input or -EBADMSG for a bad tag, preserving
 * frame and every output on error. Caller, not this primitive, checks replay.
 */
int wlan_ccmp_decrypt(const struct wlan_aes128 *ctx, uint8_t *frame, unsigned *len, uint64_t *pn,
                      unsigned *key_id);

/**
 * Append and sign a BIP-CMAC-128 MMIE on a group-addressed management MPDU.
 * key_id is 4 or 5; ipn is 1 to 2^48-1. Keep Protected clear and update *len only
 * on success. Return 0, -EINVAL, or -ENOSPC; preserve all outputs on error.
 */
int wlan_bip_sign(const struct wlan_aes128 *ctx, uint8_t *frame, unsigned *len, unsigned cap,
                  uint64_t ipn, unsigned key_id);

/**
 * Check the last MMIE and remove it on successful BIP
 * authentication, setting *len, *ipn and *key_id. Return 0, -EINVAL or
 * -EBADMSG with every output unchanged on error. Replay is the caller's.
 */
int wlan_bip_check(const struct wlan_aes128 *ctx, uint8_t *frame, unsigned *len, uint64_t *ipn,
                   unsigned *key_id);

/** Compute TKIP Michael over DA, SA, priority (0 to 15), three zero bytes
 * and the MSDU, applying Michael's padding. key is 8 bytes, mic is 8 bytes;
 * data may be NULL only at len 0. All other pointers must be non-NULL. */
void wlan_tkip_michael(const uint8_t key[8], const uint8_t da[6], const uint8_t sa[6],
                       unsigned priority, const uint8_t *data, unsigned len, uint8_t mic[8]);

/** Mix the 16-byte temporal key, transmitter address and upper 32 TSC bits
 * into five phase-1 words. ttak is caller-owned; input must not overlap it. */
void wlan_tkip_mix1(const uint8_t tk[16], const uint8_t ta[6], uint32_t iv32, uint16_t ttak[5]);

/** Mix phase-1 words, temporal key and low 16 TSC bits into a 16-byte RC4
 * key. rc4key is caller-owned and does not overlap the inputs. */
void wlan_tkip_mix2(const uint8_t tk[16], const uint16_t ttak[5], uint16_t iv16,
                    uint8_t rc4key[16]);

/** Encrypt or decrypt len bytes in place with RC4, starting at byte zero
 * of the keystream, for TKIP only. key is exactly 16 bytes. data may be NULL
 * at len 0. Caller appends or checks the TKIP ICV and erases temporary keys. */
void wlan_tkip_rc4(const uint8_t key[16], uint8_t *data, unsigned len);

#endif
