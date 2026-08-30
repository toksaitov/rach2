/*
 * netconsole.c: the kernel log, repeated onto the wire as broadcast frames.
 *
 * The whole of this file runs from inside `klog`'s delivery, which is entered
 * from anywhere in the kernel including interrupt handlers and the panic path,
 * so it does three things and no others: it appends a byte to a fixed ring, it
 * fills a fixed frame buffer, and it calls the Ethernet layer. It allocates
 * nothing and blocks on nothing.
 *
 * The buffer exists because the interesting output comes out before the
 * network device is up: the memory layout, the module ranges, the device
 * probes. Those characters accumulate, and the first frames carry them.
 *
 * Two different things can interleave here and each has its own guard.
 *
 * Interrupts. A syscall prints with interrupts enabled, and the timer and the
 * NIC print from their handlers, so both the ring and the Ethernet layer's
 * shared assembly buffer can be entered from two contexts at once. `net.h`
 * already states the rule for the second (every use of that buffer runs with
 * interrupts disabled), and the ring needs the same: without it, a handler
 * that prints between a producer computing its next index and storing it
 * writes into the slot the producer then overwrites, and a character vanishes
 * from the log with nothing to show for it. So `net_lock`/`net_unlock` bracket
 * the ring update and each frame. Each frame, not the whole drain, because a
 * full ring is sixteen frames and the timer should not be held off across all
 * of them.
 *
 * Recursion. A driver reports a transmit error with a record of its own, which
 * arrives back here in the SAME context, where disabling interrupts protects
 * nothing. A record raised on the delivery path cannot do it: `klog` sends a
 * nested record to the ring alone. One raised inside `netconsole_flush` can,
 * because the flush is entered from `netconsole_enable` and not from a
 * delivery. `nc_sending` is what stops that: while a frame is being sent, a
 * character only joins the ring. That alone is not enough, because the
 * characters it adds would extend the drain that is running; so a drain also
 * fixes its end at the ring's head when it starts, and a chip that fails every
 * transmit costs one bounded pass, not an endless one.
 */

#include "../klog.h"
#include "netconsole.h"
#include "net.h"
#include "core/byteorder.h"
#include "ethernet/ethernet.h"
#include "ip/ip.h"
#include "../lib/krandom.h"

/* What is held before transmitting starts, and while a frame is in flight.
 * Sized to carry a boot log's worth of output ahead of the device probe. */
#define NETCONSOLE_RING 8192

static char nc_ring[NETCONSOLE_RING];
static uint32_t nc_head; /* where the next character goes */
static uint32_t nc_tail; /* the oldest character not yet sent */

static uint8_t nc_payload[NETCONSOLE_HDR_LEN + NETCONSOLE_PAYLOAD_MAX];

static uint32_t nc_boot_id;
static uint32_t nc_seq;
static uint32_t nc_frames;
static uint32_t nc_dropped; /* characters the ring had no room for */
static int nc_enabled;
static int nc_sending;

static const uint8_t nc_broadcast[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

/** How many characters are waiting. */
static uint32_t nc_pending(void)
{
    return (nc_head >= nc_tail) ? (nc_head - nc_tail) : (NETCONSOLE_RING - nc_tail + nc_head);
}

void netconsole_putc(char c)
{
    uint32_t flags = net_lock();
    uint32_t next = nc_head + 1;
    int send;

    if (next == NETCONSOLE_RING) {
        next = 0;
    }
    if (next == nc_tail) {
        /* Full. The oldest characters are the boot log this exists to carry,
         * so the newest is the one dropped. */
        nc_dropped++;
        net_unlock(flags);
        return;
    }
    nc_ring[nc_head] = c;
    nc_head = next;

    send = nc_enabled && !nc_sending && (c == '\n' || nc_pending() >= NETCONSOLE_PAYLOAD_MAX);
    net_unlock(flags);

    if (send) {
        netconsole_flush();
    }
}

void netconsole_flush(void)
{
    uint32_t flags = net_lock();
    uint32_t end;

    if (!nc_enabled || nc_sending) {
        net_unlock(flags);
        return;
    }
    nc_sending = 1;
    /* Drain what is here NOW and stop. A driver that reports an error on every
     * transmit adds to the ring from inside this loop, and draining to a moving
     * end would never finish: the machine would sit inside one record with
     * interrupts disabled, transmitting the report of its own failure to
     * transmit. Whatever arrives during the drain goes out on the next one. */
    end = nc_head;
    net_unlock(flags);

    for (;;) {
        uint16_t n = 0;

        flags = net_lock();
        while (nc_tail != end && n < NETCONSOLE_PAYLOAD_MAX) {
            nc_payload[NETCONSOLE_HDR_LEN + n] = (uint8_t)nc_ring[nc_tail];
            n++;
            nc_tail++;
            if (nc_tail == NETCONSOLE_RING) {
                nc_tail = 0;
            }
        }
        if (n == 0) {
            nc_sending = 0;
            net_unlock(flags);
            return;
        }

        nc_payload[0] = 'R';
        nc_payload[1] = 'C';
        nc_payload[2] = 'H';
        nc_payload[3] = '2';
        write32be(nc_payload + 4, nc_boot_id);
        write32be(nc_payload + 8, nc_seq);
        write16be(nc_payload + 12, n);
        write16be(nc_payload + 14, 0);
        write16be(nc_payload + 14, ip_checksum(nc_payload, (uint16_t)(NETCONSOLE_HDR_LEN + n)));

        ethernet_tx(nc_broadcast, NETCONSOLE_ETHERTYPE, nc_payload,
                    (uint16_t)(NETCONSOLE_HDR_LEN + n));
        nc_seq++;
        nc_frames++;
        net_unlock(flags);
    }
}

void netconsole_enable(void)
{
    if (nc_enabled) {
        return;
    }
    /* Any per-boot value distinguishes one boot's frames from the next in a
     * capture that spans a reboot; nothing here needs it to be unpredictable.
     * Zero is reserved for "not set", so a zero draw becomes one. */
    nc_boot_id = krandom_u32();
    if (nc_boot_id == 0) {
        nc_boot_id = 1;
    }
    nc_enabled = 1;
    netconsole_flush();
}

int netconsole_is_enabled(void)
{
    return nc_enabled;
}

void netconsole_print_info(void)
{
    if (!nc_enabled) {
        return;
    }
    klog(KLOG_INFO, "netconsole",
         "broadcast, ethertype 0x%x, boot id 0x%x, "
         "%d frames, %d characters dropped",
                  (int)NETCONSOLE_ETHERTYPE, (int)nc_boot_id, (int)nc_frames, (int)nc_dropped);
}
