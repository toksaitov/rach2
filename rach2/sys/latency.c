/*
 * latency.c: the longest delays, as latency.h says.
 */

#include "latency.h"

#include "pit.h"

static uint32_t worst[LAT_COUNT];
static uint32_t last_tick_us;

void latency_note(int kind, uint32_t us)
{
    if (kind >= 0 && kind < LAT_COUNT && us > worst[kind]) {
        worst[kind] = us;
    }
}

void latency_span(int kind, uint32_t from, uint32_t to)
{
    if (to >= from) {
        latency_note(kind, to - from);
    }
}

uint32_t latency_max(int kind)
{
    return kind >= 0 && kind < LAT_COUNT ? worst[kind] : 0;
}

void latency_tick(void)
{
    uint32_t now = pit_now_us();

    if (last_tick_us) {
        latency_span(LAT_TIMER, last_tick_us, now);
    }
    last_tick_us = now;
}
