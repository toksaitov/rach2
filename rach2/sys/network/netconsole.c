/*
 * netconsole.c: the kernel log, repeated onto the wire as broadcast frames,
 * as netconsole.h describes it.
 *
 * The output path, `netconsole_putc` and the flush it starts, runs inside
 * `klog`'s delivery, which is entered from anywhere in the kernel, interrupt
 * handlers and the panic path included, so it does three things and no
 * others: it appends a character to a fixed ring, it fills a fixed frame
 * buffer, and it calls the Ethernet layer. It allocates nothing and waits for
 * nothing.
 *
 * The ring keeps what the log writes before a card is up: the memory layout,
 * the module ranges, the device probes, which the first frames carry. A card
 * whose link is not up yet, as a real card's is not for the seconds its PHY
 * takes to negotiate, could not send them, so they wait in the ring until
 * the link is up and the next line starts a drain, even a line the full ring
 * has no room for. A frame the card refuses, because the link went down in
 * the middle of a drain or its transmit ring is full, stays in the ring too,
 * and the drain stops until the next line: characters leave the ring only
 * once their frame is sent, which also keeps a character that arrives during
 * the send from taking their place. While a drain is held so, only a newline
 * starts another, and not the 512 waiting characters that otherwise do
 * (`nc_held`), so the card is asked once a line.
 *
 * Two things can interleave here, and each has its own guard.
 *
 * Interrupts. A system call logs with interrupts on, and the timer and the
 * cards log from their handlers, so the ring and the Ethernet layer's frame
 * buffer can each be entered from two contexts. net.h gives the rule for the
 * frame buffer, and the ring needs the same: without it, a handler that logs
 * between a writer computing its next index and storing it writes into the
 * slot the writer then overwrites, and a character is lost from the log with
 * nothing to show for it. So `net_lock` brackets the ring's update and each
 * frame; each frame and not the whole drain, since a full ring is sixteen
 * frames and the timer's interrupt should not wait for all of them.
 *
 * Recursion. A driver reports a transmit error with a record of its own,
 * which comes back here in the same context, where disabling interrupts
 * protects nothing. A record raised during `klog`'s delivery waits in `klog`'s
 * queue until that delivery is done, so it cannot. One raised inside the
 * flush `netconsole_enable` starts can, since that flush is no delivery.
 * `nc_sending` stops it: while a frame is being sent, a character only joins
 * the ring. The characters it adds would still extend the drain that is
 * running, so a drain fixes its end at the ring's head when it starts. A card
 * that refuses every frame costs one refused frame a line, and the ring fills
 * and drops the newest characters, as it does with no link.
 */

#include "../klog.h"
#include "netconsole.h"
#include "net.h"
#include "core/byteorder.h"
#include "ethernet/ethernet.h"
#include "driver/netdev.h"
#include "ip/ip.h"
#include "../lib/krandom.h"

/* The characters not yet sent, a boot log's worth ahead of the probe. One
 * place is kept empty to tell a full ring from an empty one. */
#define NETCONSOLE_RING 8192

static char nc_ring[NETCONSOLE_RING];
static uint32_t nc_head; /* where the next character goes */
static uint32_t nc_tail; /* the oldest character not yet sent */

/* The frame payload being built: the header, then the characters. */
static uint8_t nc_payload[NETCONSOLE_HDR_LEN + NETCONSOLE_PAYLOAD_MAX];

static uint32_t nc_boot_id;
static uint32_t nc_seq;     /* the next frame's sequence number */
static uint32_t nc_frames;  /* frames the card took */
static uint32_t nc_dropped; /* characters the ring had no room for */
static int nc_enabled;
static int nc_sending; /* a drain runs: a character only joins the ring */
static int nc_held;    /* the last drain stopped: no link or a refused frame */

static const uint8_t nc_broadcast[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

/** How many characters are waiting. */
static uint32_t nc_pending(void)
{
    return (nc_head >= nc_tail) ? (nc_head - nc_tail) : (NETCONSOLE_RING - nc_tail + nc_head);
}

/** A full ring drops the newest character, since the oldest are the boot
 * log; a newline still starts a drain, which makes room. */
void netconsole_putc(char c)
{
    uint32_t flags = net_lock();
    uint32_t next = nc_head + 1;
    int send;

    if (next == NETCONSOLE_RING) {
        next = 0;
    }
    if (next == nc_tail) {
        nc_dropped++;
        send = nc_enabled && !nc_sending && c == '\n';
        net_unlock(flags);
        if (send) {
            netconsole_flush();
        }
        return;
    }
    nc_ring[nc_head] = c;
    nc_head = next;

    send = nc_enabled && !nc_sending &&
           (c == '\n' || (!nc_held && nc_pending() >= NETCONSOLE_PAYLOAD_MAX));
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
    if (!netdev_running(netdev_first())) {
        nc_held = 1;
        net_unlock(flags);
        return;
    }
    nc_sending = 1;
    end = nc_head;
    net_unlock(flags);

    for (;;) {
        uint16_t n = 0;
        uint32_t at;

        flags = net_lock();
        at = nc_tail;
        while (at != end && n < NETCONSOLE_PAYLOAD_MAX) {
            nc_payload[NETCONSOLE_HDR_LEN + n] = (uint8_t)nc_ring[at];
            n++;
            at++;
            if (at == NETCONSOLE_RING) {
                at = 0;
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

        if (ethernet_tx(netdev_first(), nc_broadcast, NETCONSOLE_ETHERTYPE, nc_payload,
                        (uint16_t)(NETCONSOLE_HDR_LEN + n)) != NETDEV_TX_OK) {
            nc_held = 1;
            nc_sending = 0;
            net_unlock(flags);
            return;
        }
        nc_held = 0;
        nc_tail = at;
        nc_seq++;
        nc_frames++;
        net_unlock(flags);
    }
}

/** The boot id tells boots apart in a capture and need not be unpredictable;
 * it is never 0. */
void netconsole_enable(void)
{
    if (nc_enabled) {
        return;
    }
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
