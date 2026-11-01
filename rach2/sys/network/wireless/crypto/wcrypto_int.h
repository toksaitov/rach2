/* Calls shared by the Wi-Fi cipher files and their host test, not by
 * the station layer, which uses wcrypto.h alone. */
#ifndef _WLAN_WCRYPTO_INT_H_
#define _WLAN_WCRYPTO_INT_H_

#include "../wcrypto.h"

/* An AES-CMAC computation in progress, for messages given in pieces. */
struct wcrypto_cmac {
    const struct wlan_aes128 *ctx; /* borrowed for the computation */
    uint8_t x[16];                 /* CBC chain over the blocks done */
    uint8_t buf[16];               /* the last block, held until final */
    unsigned n;                    /* bytes in buf, 0 to 16 */
};

/** Encrypt the two blocks a and b in place under ctx, with one pass of the
 * bitsliced cipher, which holds two blocks at no extra cost. a and b must
 * not overlap; either may be the other's copy. */
void wcrypto_aes2(const struct wlan_aes128 *ctx, uint8_t a[16], uint8_t b[16]);

/** Begin a CMAC under ctx, which must stay unchanged until the final. */
void wcrypto_cmac_init(struct wcrypto_cmac *st, const struct wlan_aes128 *ctx);

/** Add len bytes to the message; data may be NULL only at len 0. */
void wcrypto_cmac_update(struct wcrypto_cmac *st, const uint8_t *data, unsigned len);

/** Write the 16-byte tag of the whole message and erase st. */
void wcrypto_cmac_final(struct wcrypto_cmac *st, uint8_t out[16]);

/** Compute Michael over len bytes with its padding, as the TKIP MIC does
 * over its pseudo-header and MSDU; the host test checks it on the plain
 * vectors of IEEE 802.11-2020 Annex J. data may be NULL only at len 0. */
void wcrypto_michael(const uint8_t key[8], const uint8_t *data, unsigned len, uint8_t mic[8]);

/** Return 1 when the n bytes of a and b are equal, in time that depends on
 * n alone, so that a forged tag learns nothing from the comparison. */
int wcrypto_equal(const uint8_t *a, const uint8_t *b, unsigned n);

/** Overwrite n bytes with zeros by stores the compiler keeps, for keys and
 * keystream on the stack. */
void wcrypto_wipe(void *p, unsigned n);

#endif
