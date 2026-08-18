/*
 * rcvwnd.c: the receiver's window-update decision, as a pure function.
 *
 * Split out of tcp.c so the rule can be driven by a host test at ring and
 * segment sizes the running kernel never uses. No state, no transmit, no
 * knowledge of a connection.
 */

#include "rcvwnd.h"

#define RCVWND_MAX 65535u /* an unscaled TCP window is a 16-bit field */

uint16_t rcvwnd_update(uint16_t advertised, uint32_t buffered, uint32_t ring, uint16_t mss)
{
    uint32_t window;
    uint32_t threshold;

    if (ring == 0u || buffered > ring) {
        return 0u;
    }

    window = ring - buffered;
    if (window > RCVWND_MAX) {
        window = RCVWND_MAX;
    }
    if (window <= (uint32_t)advertised) {
        return 0u;
    }

    threshold = (uint32_t)mss;
    if (threshold > ring / 2u) {
        threshold = ring / 2u;
    }
    if (threshold == 0u) {
        threshold = 1u;
    }

    if (window - (uint32_t)advertised < threshold) {
        return 0u;
    }
    return (uint16_t)window;
}
