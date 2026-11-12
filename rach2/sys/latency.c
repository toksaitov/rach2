/*
 * latency.c: the longest delays, as latency.h describes them.
 */

#include "latency.h"

#include "timer.h"

/* The longest delay of each kind, and the clock's reading at the last tick,
 * 0 before the first. */
static uint32_t worst[LAT_COUNT];
static uint32_t last_tick_us;
static volatile int idle_tick;

/** Exclude the first timer gap after the boot processor stops its idle tick. */
void latency_idle_begin(void)
{
    idle_tick = 1;
}

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
    uint32_t now = timer_now_us();

    if (last_tick_us && !idle_tick) {
        latency_span(LAT_TIMER, last_tick_us, now);
    }
    idle_tick = 0;
    last_tick_us = now;
}
