#ifndef _KSHA256_H_
#define _KSHA256_H_

#include "kint.h"

/*
 * SHA-256 (FIPS 180-4), for the kernel's pseudo-random functions, such as
 * the stable IPv6 interface identifiers of RFC 7217. It touches no hardware
 * and keeps no state of its own, so it may run in an interrupt handler and
 * compiles hosted.
 */

#define KSHA256_LEN 32

/* A hash in progress: the eight state words, the bytes not yet compressed,
 * and the count of every byte given so far. */
struct ksha256 {
    uint32_t h[8];
    uint8_t block[64];
    uint32_t fill;   /* bytes in `block`, 0 to 63 */
    uint32_t len_lo; /* bytes given, low word */
    uint32_t len_hi; /* bytes given, high word */
};

/** Begin a hash in `c`. */
void ksha256_init(struct ksha256 *c);

/** Add the `n` bytes at `data` to the hash in `c`. */
void ksha256_update(struct ksha256 *c, const void *data, uint32_t n);

/** End the hash in `c` and write its 32 bytes to `out`. */
void ksha256_final(struct ksha256 *c, uint8_t *out);

#endif /* _KSHA256_H_ */
