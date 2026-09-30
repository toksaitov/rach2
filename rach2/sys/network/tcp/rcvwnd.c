/*
 * rcvwnd.c: the receiver's window-update decision, as a pure function.
 *
 * It is a file apart from tcp.c so that a host test can drive the rule at
 * ring and segment sizes the running kernel never uses. It keeps no state,
 * sends nothing and reads no connection's block.
 */

#include "rcvwnd.h"

uint32_t rcvwnd_update(uint32_t advertised, uint32_t buffered, uint32_t ring, uint32_t most,
                       uint32_t mss)
{
    uint32_t window;
    uint32_t threshold;

    if (ring == 0u || buffered > ring) {
        return 0u;
    }

    window = ring - buffered;
    if (window > most) {
        window = most;
    }
    if (window <= advertised) {
        return 0u;
    }

    threshold = mss;
    if (threshold > ring / 2u) {
        threshold = ring / 2u;
    }
    if (threshold == 0u) {
        threshold = 1u;
    }

    if (window - advertised < threshold) {
        return 0u;
    }
    return window;
}
