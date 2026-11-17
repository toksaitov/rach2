/*
 * crc32.h: the CRC-32 of the reflected polynomial 0xedb88320, Linux's
 * crc32_le, computed eight bytes at a time as Linux's lib/crc32.c does by
 * default (CRC32_SLICEBY8). It touches no hardware and keeps no state but
 * its tables, so it may run in any context and compiles hosted.
 */
#ifndef _CRC32_H_
#define _CRC32_H_

#include "kint.h"

/**
 * Carry the CRC `crc` over the `len` bytes at `p`, with no inversion at
 * either end: the CRC-32 of zip files and GUID tables is
 * `~crc32_le(~0u, p, len)`, and vkms's frame CRC starts at 0. The first
 * call fills 8 KiB of tables.
 */
uint32_t crc32_le(uint32_t crc, const uint8_t *p, uint32_t len);

#endif
