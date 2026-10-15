#ifndef _BYTEORDER_H_
#define _BYTEORDER_H_

#include "../../lib/kint.h"

/*
 * byteorder: the reads and writes of a packet header's multi-byte fields,
 * which are big-endian, most significant byte first, where x86 is
 * little-endian. Each moves one byte at a time, since a field need not be
 * aligned in the buffer that holds its packet. The definitions are in
 * net_core.c.
 */

/** The 16-bit and the 32-bit big-endian value at `p`. */
uint16_t read16be(const uint8_t *p);
uint32_t read32be(const uint8_t *p);

/** Store `value` big-endian at `p`, in two bytes and in four. */
void write16be(uint8_t *p, uint16_t value);
void write32be(uint8_t *p, uint32_t value);

#endif /* _BYTEORDER_H_ */
