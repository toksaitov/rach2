/* The CRC-32 of the reflected polynomial 0xedb88320, as crc32.h describes
 * it. */
#include "crc32.h"

/* crc32_table[k][b]: the CRC of the byte b followed by k zero bytes, so that
 * eight bytes fold into the CRC with eight lookups, as Linux's slice-by-8
 * tables do. The tables and the flag are volatile so that the compiler keeps
 * the order of their stores and loads, which x86 keeps between processors:
 * a reader that sees `crc32_ready` set sees every entry. */
static volatile uint32_t crc32_table[8][256];
static volatile int crc32_ready;

/** Fill the tables and then set `crc32_ready`. Two processors that fill them
 * at once store the same values. */
static void crc32_fill(void)
{
    uint32_t i, c;
    int k;

    for (i = 0; i < 256; i++) {
        c = i;
        for (k = 0; k < 8; k++) {
            c = (c & 1u) ? (c >> 1) ^ 0xedb88320u : c >> 1;
        }
        crc32_table[0][i] = c;
    }
    for (i = 0; i < 256; i++) {
        for (k = 1; k < 8; k++) {
            c = crc32_table[k - 1][i];
            crc32_table[k][i] = (c >> 8) ^ crc32_table[0][c & 0xffu];
        }
    }
    crc32_ready = 1;
}

/** Carry `crc` over the `len` bytes at `p`, as crc32.h says. */
uint32_t crc32_le(uint32_t crc, const uint8_t *p, uint32_t len)
{
    uint32_t a, b;

    if (!crc32_ready) {
        crc32_fill();
    }
    while (len >= 8u) {
        a = crc ^
            ((uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24);
        b = (uint32_t)p[4] | (uint32_t)p[5] << 8 | (uint32_t)p[6] << 16 | (uint32_t)p[7] << 24;
        crc = crc32_table[7][a & 0xffu] ^ crc32_table[6][(a >> 8) & 0xffu] ^
              crc32_table[5][(a >> 16) & 0xffu] ^ crc32_table[4][a >> 24] ^
              crc32_table[3][b & 0xffu] ^ crc32_table[2][(b >> 8) & 0xffu] ^
              crc32_table[1][(b >> 16) & 0xffu] ^ crc32_table[0][b >> 24];
        p += 8;
        len -= 8u;
    }
    while (len-- > 0u) {
        crc = (crc >> 8) ^ crc32_table[0][(crc ^ *p++) & 0xffu];
    }
    return crc;
}
