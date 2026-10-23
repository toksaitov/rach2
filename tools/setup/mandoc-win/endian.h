#ifndef RACH2_MANDOC_ENDIAN_H
#define RACH2_MANDOC_ENDIAN_H
#include <stdint.h>

static inline uint32_t be32toh(uint32_t value)
{
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    return __builtin_bswap32(value);
#else
    return value;
#endif
}

static inline uint32_t htobe32(uint32_t value)
{
    return be32toh(value);
}
#endif
