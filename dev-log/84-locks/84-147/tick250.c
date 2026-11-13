/* Check the 250 Hz conversions at fractional and overflow boundaries. */
#include "../../../rach2/sys/timer.h"
#include <stdio.h>

/** Exercise the production conversions, including the full 32-bit input. */
int main(void)
{
    uint32_t n;

    if (TICK_HZ != 250 || TICK_NS != 4000000u) {
        return 1;
    }
    for (n = 0; n < 100000u; n++) {
        if (MS_TO_TICKS(n) != (n + 3u) / 4u ||
            tick_centisecs(n) != ((uint64_t)n * 5u + 1u) / 2u ||
            TICKS_TO_CLOCK(n) != (uint64_t)n * 2u / 5u) {
            return 2;
        }
    }
    if (MS_TO_TICKS(0xFFFFFFFFu) != 1073741824u ||
        tick_centisecs(0xFFFFFFFFu) != 10737418238ull ||
        TICKS_TO_CLOCK(~(uint64_t)0) != 7378697629483820646ull ||
        tick_span(1, 1) != 251 || tick_span(0, 3999999) != 1 ||
        tick_span(0, 4000001) != 2) {
        return 3;
    }
    puts("tick250: ok: fractional rates, upward timeouts and full-width inputs");
    return 0;
}
