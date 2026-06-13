#ifndef _BYTEORDER_H_
#define _BYTEORDER_H_

#include "../../lib/kint.h"

/*
 * Network byte order helpers.
 *
 * Protocol headers store multi-byte fields big-endian (most significant byte
 * first); x86 is little-endian, so every field read from or written to a
 * packet buffer must be byte-swapped. These also read and write byte by byte
 * to avoid unaligned accesses on fields that do not sit on a natural boundary
 * inside a DMA buffer.
 */

uint16_t read16be(const uint8_t *p);
uint32_t read32be(const uint8_t *p);
void write16be(uint8_t *p, uint16_t value);
void write32be(uint8_t *p, uint32_t value);

#endif /* _BYTEORDER_H_ */
