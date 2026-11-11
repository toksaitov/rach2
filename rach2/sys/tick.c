/*
 * tick.c: the arithmetic of the kernel's tick count, a 64-bit count divided
 * by a small divisor or any, and a span of time rounded up to whole ticks,
 * with no help from libgcc.
 */

#include "timer.h"

uint64_t tick_div(uint64_t n, uint32_t d, uint32_t *rem)
{
    uint32_t hi = (uint32_t)(n >> 32);
    uint32_t lo = (uint32_t)n;
    uint32_t r, mid, low;

    r = hi % d;
    hi /= d;
    mid = (r << 16) | (lo >> 16);
    r = mid % d;
    mid /= d;
    low = (r << 16) | (lo & 0xFFFFu);
    r = low % d;
    low /= d;
    if (rem) {
        *rem = r;
    }
    return ((uint64_t)hi << 32) | ((uint64_t)mid << 16) | low;
}

/**
 * Long division, one bit at a time. The partial remainder is below `d`
 * before each shift, so after it it is below 2^65 when `d` is 2^63 or more:
 * the bit shifted out of the top is kept as `carry`, and the remainder is
 * then at least `d`, whose subtraction modulo 2^64 leaves the true
 * remainder.
 */
uint64_t tick_div64(uint64_t n, uint64_t d, uint64_t *rem)
{
    uint64_t q = 0, r = 0, carry;
    int i;

    if ((n >> 32) == 0 && (d >> 32) == 0) {
        q = (uint32_t)n / (uint32_t)d;
        r = (uint32_t)n % (uint32_t)d;
    } else {
        for (i = 63; i >= 0; i--) {
            carry = r >> 63;
            r = (r << 1) | ((n >> i) & 1u);
            if (carry != 0 || r >= d) {
                r -= d;
                q |= (uint64_t)1 << i;
            }
        }
    }
    if (rem) {
        *rem = r;
    }
    return q;
}

uint64_t tick_span(int64_t sec, uint32_t nsec)
{
    if (sec > ((int64_t)1 << 40)) {
        sec = (int64_t)1 << 40;
    }
    return (uint64_t)sec * TICK_HZ + (nsec + (TICK_NS - 1u)) / TICK_NS;
}
