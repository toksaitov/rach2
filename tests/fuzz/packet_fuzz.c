/**
 * Fuzz harness for the receive path (ethernet_rx and ip_rx, with ARP, ICMP,
 * UDP, TCP and the raw and packet sockets stubbed to count): the input is a
 * frame.
 *
 * fuzz-deps: rach2/sys/lib/kstring.c rach2/sys/lib/kctypes.c
 *
 * The host test's own interface, stubs and `deliver` are what the harness
 * stands on, so a frame is handed over from the end of an allocation of
 * exactly its length and one byte past it is one byte past the allocation.
 */
#define main packet_hosttest_main
#include "../host/packet_hosttest.c"
#undef main
#include "../fuzz/fuzz.h"

/** Receive the input, cut to 1514 bytes, as one frame. */
int LLVMFuzzerTestOneInput(const unsigned char *data, unsigned long size)
{
    if (size > 1514) {
        size = 1514;
    }
    reset();
    deliver(data, (unsigned)size);
    return 0;
}
